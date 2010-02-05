// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <windows.h>
#include <mmsystem.h>

#include "base/basictypes.h"
#include "base/base_paths.h"
#include "base/file_util.h"
#include "base/path_service.h"
#include "base/sync_socket.h"
#include "media/audio/audio_output.h"
#include "media/audio/simple_sources.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::DoAll;
using ::testing::InSequence;
using ::testing::NiceMock;
using ::testing::NotNull;
using ::testing::Return;

namespace {

const wchar_t kAudioFile1_16b_m_16K[]
    = L"media\\test\\data\\sweep02_16b_mono_16KHz.raw";

// This class allows to find out if the callbacks are occurring as
// expected and if any error has been reported.
class TestSourceBasic : public AudioOutputStream::AudioSourceCallback {
 public:
  explicit TestSourceBasic()
      : callback_count_(0),
        had_error_(0),
        was_closed_(0) {
  }
  // AudioSourceCallback::OnMoreData implementation:
  virtual uint32 OnMoreData(AudioOutputStream* stream,
                            void* dest, uint32 max_size, uint32 pending_bytes) {
    ++callback_count_;
    // Touch the first byte to make sure memory is good.
    if (max_size)
      reinterpret_cast<char*>(dest)[0] = 1;
    return max_size;
  }
  // AudioSourceCallback::OnClose implementation:
  virtual void OnClose(AudioOutputStream* stream) {
    ++was_closed_;
  }
  // AudioSourceCallback::OnError implementation:
  virtual void OnError(AudioOutputStream* stream, int code) {
    ++had_error_;
  }
  // Returns how many times OnMoreData() has been called.
  int callback_count() const {
    return callback_count_;
  }
  // Returns how many times the OnError callback was called.
  int had_error() const {
    return had_error_;
  }

  void set_error(bool error) {
    had_error_ += error ? 1 : 0;
  }
  // Returns how many times the OnClose callback was called.
  int was_closed() const {
    return was_closed_;
  }

 private:
  int callback_count_;
  int had_error_;
  int was_closed_;
};

bool IsRunningHeadless() {
  return (0 != ::GetEnvironmentVariableW(L"CHROME_HEADLESS", NULL, 0));
}

}  // namespace.

const int kNumBuffers = 3;
// Specializes TestSourceBasic to detect that the AudioStream is using
// triple buffering correctly.
class TestSourceTripleBuffer : public TestSourceBasic {
 public:
  TestSourceTripleBuffer() {
    buffer_address_[0] = NULL;
    buffer_address_[1] = NULL;
    buffer_address_[2] = NULL;
  }
  // Override of TestSourceBasic::OnMoreData.
  virtual uint32 OnMoreData(AudioOutputStream* stream,
                            void* dest, uint32 max_size, uint32 pending_bytes) {
    // Call the base, which increments the callback_count_.
    TestSourceBasic::OnMoreData(stream, dest, max_size, 0);
    if (callback_count() % kNumBuffers == 2) {
      set_error(!CompareExistingIfNotNULL(2, dest));
    } else if (callback_count() % kNumBuffers == 1) {
      set_error(!CompareExistingIfNotNULL(1, dest));
    } else {
      set_error(!CompareExistingIfNotNULL(0, dest));
    }
    if (callback_count() > kNumBuffers) {
      set_error(buffer_address_[0] == buffer_address_[1]);
      set_error(buffer_address_[1] == buffer_address_[2]);
    }
    return max_size;
  }

 private:
  bool CompareExistingIfNotNULL(uint32 index, void* address) {
    void*& entry = buffer_address_[index];
    if (!entry)
      entry = address;
    return (entry == address);
  }

  void* buffer_address_[kNumBuffers];
};

// Specializes TestSourceBasic to simulate a source that blocks for some time
// in the OnMoreData callback.
class TestSourceLaggy : public TestSourceBasic {
 public:
  TestSourceLaggy(int laggy_after_buffer, int lag_in_ms)
      : laggy_after_buffer_(laggy_after_buffer), lag_in_ms_(lag_in_ms) {
  }
  virtual uint32 OnMoreData(AudioOutputStream* stream,
                            void* dest, uint32 max_size, uint32 pending_bytes) {
    // Call the base, which increments the callback_count_.
    TestSourceBasic::OnMoreData(stream, dest, max_size, 0);
    if (callback_count() > kNumBuffers) {
      ::Sleep(lag_in_ms_);
    }
    return max_size;
  }
 private:
  int laggy_after_buffer_;
  int lag_in_ms_;
};

class MockAudioSource : public AudioOutputStream::AudioSourceCallback {
 public:
  MOCK_METHOD4(OnMoreData, uint32(AudioOutputStream* stream, void* dest,
                                  uint32 max_size, uint32 pending_bytes));
  MOCK_METHOD1(OnClose, void(AudioOutputStream* stream));
  MOCK_METHOD2(OnError, void(AudioOutputStream* stream, int code));
};

// Helper class to memory map an entire file. The mapping is read-only. Don't
// use for gigabyte-sized files. Attempts to write to this memory generate
// memory access violations.
class ReadOnlyMappedFile {
 public:
  explicit ReadOnlyMappedFile(const wchar_t* file_name)
      : fmap_(NULL), start_(NULL), size_(0) {
    HANDLE file = ::CreateFileW(file_name, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (INVALID_HANDLE_VALUE == file)
      return;
    fmap_ = ::CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    ::CloseHandle(file);
    if (!fmap_)
      return;
    start_ = reinterpret_cast<char*>(::MapViewOfFile(fmap_, FILE_MAP_READ,
                                                     0, 0, 0));
    if (!start_)
      return;
    MEMORY_BASIC_INFORMATION mbi = {0};
    ::VirtualQuery(start_, &mbi, sizeof(mbi));
    size_ = mbi.RegionSize;
  }
  ~ReadOnlyMappedFile() {
    if (start_) {
      ::UnmapViewOfFile(start_);
      ::CloseHandle(fmap_);
    }
  }
  // Returns true if the file was successfully mapped.
  bool is_valid() const {
    return ((start_ > 0) && (size_ > 0));
  }
  // Returns the size in bytes of the mapped memory.
  uint32 size() const {
    return size_;
  }
  // Returns the memory backing the file.
  const void* GetChunkAt(uint32 offset) {
    return &start_[offset];
  }

 private:
  HANDLE fmap_;
  char* start_;
  uint32 size_;
};

// ============================================================================
// Validate that the AudioManager::AUDIO_MOCK callbacks work.
TEST(WinAudioTest, MockStreamBasicCallbacks) {
  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  AudioOutputStream* oas =
      audio_man->MakeAudioStream(AudioManager::AUDIO_MOCK, 2, 8000, 8);
  ASSERT_TRUE(NULL != oas);
  EXPECT_TRUE(oas->Open(256));
  TestSourceBasic source;
  oas->Start(&source);
  EXPECT_GT(source.callback_count(), 0);
  oas->Stop();
  oas->Close();
  EXPECT_EQ(0, source.had_error());
  EXPECT_EQ(1, source.was_closed());
}

// ===========================================================================
// Validation of AudioManager::AUDIO_PCM_LINEAR
//
// The tests tend to fail in the build bots when somebody connects to them via
// via remote-desktop because it installs an audio device that fails to open
// at some point, possibly when the connection goes idle. So that is why we
// skipped them in headless mode.

// Test that can it be created and closed.
TEST(WinAudioTest, PCMWaveStreamGetAndClose) {
  if (IsRunningHeadless())
    return;
  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  if (!audio_man->HasAudioDevices())
    return;
  AudioOutputStream* oas =
      audio_man->MakeAudioStream(AudioManager::AUDIO_PCM_LINEAR, 2, 8000, 16);
  ASSERT_TRUE(NULL != oas);
  oas->Close();
}

// Test that can it be cannot be created with crazy parameters
TEST(WinAudioTest, SanityOnMakeParams) {
  if (IsRunningHeadless())
    return;
  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  if (!audio_man->HasAudioDevices())
    return;
  AudioManager::Format fmt = AudioManager::AUDIO_PCM_LINEAR;
  EXPECT_TRUE(NULL == audio_man->MakeAudioStream(fmt, 8, 8000, 16));
  EXPECT_TRUE(NULL == audio_man->MakeAudioStream(fmt, 1, 1024 * 1024, 16));
  EXPECT_TRUE(NULL == audio_man->MakeAudioStream(fmt, 2, 8000, 80));
  EXPECT_TRUE(NULL == audio_man->MakeAudioStream(fmt, -2, 8000, 16));
  EXPECT_TRUE(NULL == audio_man->MakeAudioStream(fmt, 2, -8000, 16));
  EXPECT_TRUE(NULL == audio_man->MakeAudioStream(fmt, 2, -8000, -16));
}

// Test that it can be opened and closed.
TEST(WinAudioTest, PCMWaveStreamOpenAndClose) {
  if (IsRunningHeadless())
    return;
  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  if (!audio_man->HasAudioDevices())
    return;
  AudioOutputStream* oas =
      audio_man->MakeAudioStream(AudioManager::AUDIO_PCM_LINEAR, 2, 8000, 16);
  ASSERT_TRUE(NULL != oas);
  EXPECT_TRUE(oas->Open(1024));
  oas->Close();
}

// Test that it has a maximum packet size.
TEST(WinAudioTest, PCMWaveStreamOpenLimit) {
  if (IsRunningHeadless())
    return;
  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  if (!audio_man->HasAudioDevices())
    return;
  AudioOutputStream* oas =
      audio_man->MakeAudioStream(AudioManager::AUDIO_PCM_LINEAR, 2, 8000, 16);
  ASSERT_TRUE(NULL != oas);
  EXPECT_FALSE(oas->Open(1024 * 1024 * 1024));
  oas->Close();
}

// Test that it uses the triple buffers correctly. Because it uses the actual
// audio device, you might hear a short pop noise for a short time.
TEST(WinAudioTest, PCMWaveStreamTripleBuffer) {
  if (IsRunningHeadless())
    return;
  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  if (!audio_man->HasAudioDevices())
    return;
  AudioOutputStream* oas =
      audio_man->MakeAudioStream(AudioManager::AUDIO_PCM_LINEAR, 1, 16000, 16);
  ASSERT_TRUE(NULL != oas);
  TestSourceTripleBuffer test_triple_buffer;
  EXPECT_TRUE(oas->Open(512));
  oas->Start(&test_triple_buffer);
  ::Sleep(300);
  EXPECT_GT(test_triple_buffer.callback_count(), kNumBuffers);
  EXPECT_FALSE(test_triple_buffer.had_error());
  oas->Stop();
  ::Sleep(500);
  oas->Close();
}

// Test potential deadlock situation if the source is slow or blocks for some
// time. The actual EXPECT_GT are mostly meaningless and the real test is that
// the test completes in reasonable time.
TEST(WinAudioTest, PCMWaveSlowSource) {
  if (IsRunningHeadless())
    return;
  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  if (!audio_man->HasAudioDevices())
    return;
  AudioOutputStream* oas =
      audio_man->MakeAudioStream(AudioManager::AUDIO_PCM_LINEAR, 1, 16000, 16);
  ASSERT_TRUE(NULL != oas);
  TestSourceLaggy test_laggy(2, 90);
  EXPECT_TRUE(oas->Open(512));
  // The test parameters cause a callback every 32 ms and the source is
  // sleeping for 90 ms, so it is guaranteed that we run out of ready buffers.
  oas->Start(&test_laggy);
  ::Sleep(500);
  EXPECT_GT(test_laggy.callback_count(), 2);
  EXPECT_FALSE(test_laggy.had_error());
  oas->Stop();
  ::Sleep(500);
  oas->Close();
}

// Test another potential deadlock situation if the thread that calls Start()
// gets paused. This test is best when run over RDP with audio enabled. See
// bug 19276 for more details.
TEST(WinAudioTest, PCMWaveStreamPlaySlowLoop) {
  if (IsRunningHeadless())
    return;
  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  if (!audio_man->HasAudioDevices())
    return;
  AudioOutputStream* oas =
      audio_man->MakeAudioStream(AudioManager::AUDIO_PCM_LINEAR, 1,
                                 AudioManager::kAudioCDSampleRate, 16);
  ASSERT_TRUE(NULL != oas);

  SineWaveAudioSource source(SineWaveAudioSource::FORMAT_16BIT_LINEAR_PCM, 1,
                             200.0, AudioManager::kAudioCDSampleRate);
  uint32 bytes_100_ms = (AudioManager::kAudioCDSampleRate / 10) * 2;

  EXPECT_TRUE(oas->Open(bytes_100_ms));
  oas->SetVolume(1.0);

  for (int ix = 0; ix != 5; ++ix) {
    oas->Start(&source);
    ::Sleep(10);
    oas->Stop();
  }
  oas->Close();
}


// This test produces actual audio for .5 seconds on the default wave
// device at 44.1K s/sec. Parameters have been chosen carefully so you should
// not hear pops or noises while the sound is playing.
TEST(WinAudioTest, PCMWaveStreamPlay200HzTone44Kss) {
  if (IsRunningHeadless())
    return;
  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  if (!audio_man->HasAudioDevices())
    return;
  AudioOutputStream* oas =
      audio_man->MakeAudioStream(AudioManager::AUDIO_PCM_LINEAR, 1,
                                 AudioManager::kAudioCDSampleRate, 16);
  ASSERT_TRUE(NULL != oas);

  SineWaveAudioSource source(SineWaveAudioSource::FORMAT_16BIT_LINEAR_PCM, 1,
                             200.0, AudioManager::kAudioCDSampleRate);
  uint32 bytes_100_ms = (AudioManager::kAudioCDSampleRate / 10) * 2;

  EXPECT_TRUE(oas->Open(bytes_100_ms));
  oas->SetVolume(1.0);
  oas->Start(&source);
  ::Sleep(500);
  oas->Stop();
  oas->Close();
}

// This test produces actual audio for for .5 seconds on the default wave
// device at 22K s/sec. Parameters have been chosen carefully so you should
// not hear pops or noises while the sound is playing. The audio also should
// sound with a lower volume than PCMWaveStreamPlay200HzTone44Kss.
TEST(WinAudioTest, PCMWaveStreamPlay200HzTone22Kss) {
  if (IsRunningHeadless())
    return;
  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  if (!audio_man->HasAudioDevices())
    return;
  AudioOutputStream* oas =
      audio_man->MakeAudioStream(AudioManager::AUDIO_PCM_LINEAR, 1,
                                 AudioManager::kAudioCDSampleRate/2, 16);
  ASSERT_TRUE(NULL != oas);

  SineWaveAudioSource source(SineWaveAudioSource::FORMAT_16BIT_LINEAR_PCM, 1,
                             200.0, AudioManager::kAudioCDSampleRate/2);
  uint32 bytes_100_ms = (AudioManager::kAudioCDSampleRate / 20) * 2;

  EXPECT_TRUE(oas->Open(bytes_100_ms));

  oas->SetVolume(0.5);
  oas->Start(&source);
  ::Sleep(500);

  // Test that the volume is within the set limits.
  double volume = 0.0;
  oas->GetVolume(&volume);
  EXPECT_LT(volume, 0.51);
  EXPECT_GT(volume, 0.49);
  oas->Stop();
  oas->Close();
}

// Uses the PushSource to play a 2 seconds file clip for about 5 seconds. We
// try hard to generate situation where the two threads are accessing the
// object roughly at the same time. What you hear is a sweeping tone from 1KHz
// to 2KHz with a bit of fade out at the end for one second. The file is two
// of these sweeping tones back to back.
TEST(WinAudioTest, PushSourceFile16KHz)  {
  if (IsRunningHeadless())
    return;
  // Open sweep02_16b_mono_16KHz.raw which has no format. It contains the
  // raw 16 bit samples for a single channel in little-endian format. The
  // creation sample rate is 16KHz.
  FilePath audio_file;
  ASSERT_TRUE(PathService::Get(base::DIR_SOURCE_ROOT, &audio_file));
  audio_file = audio_file.Append(kAudioFile1_16b_m_16K);
  // Map the entire file in memory.
  ReadOnlyMappedFile file_reader(audio_file.value().c_str());
  ASSERT_TRUE(file_reader.is_valid());

  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  if (!audio_man->HasAudioDevices())
    return;
  AudioOutputStream* oas =
      audio_man->MakeAudioStream(AudioManager::AUDIO_PCM_LINEAR, 1, 16000, 16);
  ASSERT_TRUE(NULL != oas);

  // compute buffer size for 100ms of audio. Which is 3200 bytes.
  const uint32 kSize50ms = 2 * (16000 / 1000) * 100;
  EXPECT_TRUE(oas->Open(kSize50ms));

  uint32 offset = 0;
  const uint32 kMaxStartOffset = file_reader.size() - kSize50ms;

  // We buffer and play at the same time, buffering happens every ~10ms and the
  // consuming of the buffer happens every ~50ms. We do 100 buffers which
  // effectively wrap around the file more than once.
  PushSource push_source(kSize50ms);
  for (uint32 ix = 0; ix != 100; ++ix) {
    push_source.Write(file_reader.GetChunkAt(offset), kSize50ms);
    if (ix == 2) {
      // For glitch free, start playing after some buffers are in.
      oas->Start(&push_source);
    }
    ::Sleep(10);
    offset += kSize50ms;
    if (offset > kMaxStartOffset)
      offset = 0;
  }

  // Play a little bit more of the file.
  ::Sleep(500);

  oas->Stop();
  oas->Close();
}

// This test is to make sure an AudioOutputStream can be started after it was
// stopped. You will here two .5 seconds wave signal separated by 0.5 seconds
// of silence.
TEST(WinAudioTest, PCMWaveStreamPlayTwice200HzTone44Kss) {
  if (IsRunningHeadless())
    return;
  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  if (!audio_man->HasAudioDevices())
    return;
  AudioOutputStream* oas =
      audio_man->MakeAudioStream(AudioManager::AUDIO_PCM_LINEAR, 1,
                                 AudioManager::kAudioCDSampleRate, 16);
  ASSERT_TRUE(NULL != oas);

  SineWaveAudioSource source(SineWaveAudioSource::FORMAT_16BIT_LINEAR_PCM, 1,
                             200.0, AudioManager::kAudioCDSampleRate);
  uint32 bytes_100_ms = (AudioManager::kAudioCDSampleRate / 10) * 2;

  EXPECT_TRUE(oas->Open(bytes_100_ms));
  oas->SetVolume(1.0);

  // Play the wave for .5 seconds.
  oas->Start(&source);
  ::Sleep(500);
  oas->Stop();

  // Sleep to give silence after stopping the AudioOutputStream.
  ::Sleep(250);

  // Start again and play for .5 seconds.
  oas->Start(&source);
  ::Sleep(500);
  oas->Stop();

  oas->Close();
}

// With the low latency mode, we have two buffers instead of 3 and we
// should be able to handle 20ms buffers at 44KHz. See also the SyncSocketBasic
// test below.
// TODO(cpu): right now the best we can do is 50ms before it sounds choppy.
TEST(WinAudioTest, PCMWaveStreamPlay200HzTone44KssLowLatency) {
  if (IsRunningHeadless())
    return;
  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  if (!audio_man->HasAudioDevices())
    return;
  AudioOutputStream* oas =
      audio_man->MakeAudioStream(AudioManager::AUDIO_PCM_LOW_LATENCY, 1,
                                 AudioManager::kAudioCDSampleRate, 16);
  ASSERT_TRUE(NULL != oas);

  SineWaveAudioSource source(SineWaveAudioSource::FORMAT_16BIT_LINEAR_PCM, 1,
                             200.0, AudioManager::kAudioCDSampleRate);
  uint32 bytes_50_ms = (AudioManager::kAudioCDSampleRate / 20) * sizeof(uint16);

  EXPECT_TRUE(oas->Open(bytes_50_ms));
  oas->SetVolume(1.0);

  // Play the wave for .8 seconds.
  oas->Start(&source);
  ::Sleep(800);
  oas->Stop();
  oas->Close();
}

// Check that the pending bytes value is correct what the stream starts.
TEST(WinAudioTest, PCMWaveStreamPendingBytes) {
  if (IsRunningHeadless())
    return;
  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  if (!audio_man->HasAudioDevices())
    return;
  AudioOutputStream* oas =
      audio_man->MakeAudioStream(AudioManager::AUDIO_PCM_LINEAR, 1,
                                 AudioManager::kAudioCDSampleRate, 16);
  ASSERT_TRUE(NULL != oas);

  NiceMock<MockAudioSource> source;
  uint32 bytes_100_ms = (AudioManager::kAudioCDSampleRate / 10) * 2;
  EXPECT_TRUE(oas->Open(bytes_100_ms));

  // We expect the amount of pending bytes will reaching 2 times of
  // |bytes_100_ms| because the audio output stream has a triple buffer scheme.
  // And then we will try to provide zero data so the amount of pending bytes
  // will go down and eventually read zero.
  InSequence s;
  EXPECT_CALL(source, OnMoreData(oas, NotNull(), bytes_100_ms, 0))
      .WillOnce(Return(bytes_100_ms));
  EXPECT_CALL(source, OnMoreData(oas, NotNull(), bytes_100_ms, bytes_100_ms))
      .WillOnce(Return(bytes_100_ms));
  EXPECT_CALL(source, OnMoreData(oas, NotNull(),
                                 bytes_100_ms, 2 * bytes_100_ms))
      .WillOnce(Return(bytes_100_ms));
  EXPECT_CALL(source, OnMoreData(oas, NotNull(),
                                 bytes_100_ms, 2 * bytes_100_ms))
      .WillOnce(Return(0));
  EXPECT_CALL(source, OnMoreData(oas, NotNull(), bytes_100_ms, bytes_100_ms))
      .WillOnce(Return(0));
  EXPECT_CALL(source, OnMoreData(oas, NotNull(), bytes_100_ms, 0))
      .Times(AnyNumber())
      .WillRepeatedly(Return(0));

  oas->Start(&source);
  ::Sleep(500);
  oas->Stop();
  oas->Close();
}

namespace {
// Simple source that uses a SyncSocket to retrieve the audio data
// from a potentially remote thread.
class SyncSocketSource : public AudioOutputStream::AudioSourceCallback {
 public:
  explicit SyncSocketSource(base::SyncSocket* socket)
      : socket_(socket) {}

  ~SyncSocketSource() {
    delete socket_;
  }

  // AudioSourceCallback::OnMoreData implementation:
  virtual uint32 OnMoreData(AudioOutputStream* stream,
                            void* dest, uint32 max_size, uint32 pending_bytes) {
    socket_->Send(&pending_bytes, sizeof(pending_bytes));
    uint32 got = socket_->Receive(dest, max_size);
    return got;
  }
  // AudioSourceCallback::OnClose implementation:
  virtual void OnClose(AudioOutputStream* stream) {
  }
  // AudioSourceCallback::OnError implementation:
  virtual void OnError(AudioOutputStream* stream, int code) {
  }

 private:
  base::SyncSocket* socket_;
};

struct SyncThreadContext {
  base::SyncSocket* socket;
  int sample_rate;
  double sine_freq;
  uint32 packet_size;
};

// This thread provides the data that the SyncSocketSource above needs
// using the other end of a SyncSocket. The protocol is as follows:
//
// SyncSocketSource ---send 4 bytes ------------> SyncSocketThread
//                  <--- audio packet ----------
//
DWORD __stdcall SyncSocketThread(void* context) {
  SyncThreadContext& ctx = *(reinterpret_cast<SyncThreadContext*>(context));

  const int kTwoSecBytes =
      AudioManager::kAudioCDSampleRate * 2 * sizeof(uint16);
  char* buffer = new char[kTwoSecBytes];
  SineWaveAudioSource sine(SineWaveAudioSource::FORMAT_16BIT_LINEAR_PCM,
                           1, ctx.sine_freq, ctx.sample_rate);
  sine.OnMoreData(NULL, buffer, kTwoSecBytes, 0);

  int pending_bytes = -1;
  int times = 0;
  for (int ix = 0; ix < kTwoSecBytes; ix += ctx.packet_size) {
    if (ctx.socket->Receive(&pending_bytes, sizeof(pending_bytes)) == 0)
      break;
    if ((times > 0) && (pending_bytes < 1000)) __debugbreak();
    ctx.socket->Send(&buffer[ix], ctx.packet_size);
    ++times;
  }

  delete buffer;
  return 0;
}

}  // namespace

// Test the basic operation of AudioOutputStream used with a SyncSocket.
// The emphasis is to test low-latency with buffers less than 100ms. With
// the waveout api it seems not possible to go below 50ms. In this test
// you should hear a continous 200Hz tone.
//
// TODO(cpu): This actually sounds choppy most of the time. Fix it.
TEST(WinAudioTest, SyncSocketBasic) {
  if (IsRunningHeadless())
    return;

  AudioManager* audio_man = AudioManager::GetAudioManager();
  ASSERT_TRUE(NULL != audio_man);
  if (!audio_man->HasAudioDevices())
    return;

  int sample_rate = AudioManager::kAudioCDSampleRate;
  AudioOutputStream* oas =
      audio_man->MakeAudioStream(AudioManager::AUDIO_PCM_LOW_LATENCY, 1,
                                 sample_rate, 16);
  ASSERT_TRUE(NULL != oas);

  // compute buffer size for 20ms of audio, 882 samples (mono).
  const uint32 kSamples20ms = sample_rate / 50 * sizeof(uint16);
  ASSERT_TRUE(oas->Open(kSamples20ms));

  base::SyncSocket* sockets[2];
  ASSERT_TRUE(base::SyncSocket::CreatePair(sockets));

  SyncSocketSource source(sockets[0]);

  SyncThreadContext thread_context;
  thread_context.sample_rate = sample_rate;
  thread_context.sine_freq = 200.0;
  thread_context.packet_size = kSamples20ms;
  thread_context.socket = sockets[1];

  HANDLE thread = ::CreateThread(NULL, 0, SyncSocketThread,
                                 &thread_context, 0, NULL);

  oas->Start(&source);

  ::WaitForSingleObject(thread, INFINITE);
  ::CloseHandle(thread);
  delete sockets[1];

  oas->Stop();
  oas->Close();
}
