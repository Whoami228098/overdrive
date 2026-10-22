// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "music_player.hpp"
#include "audio/stream_ring.hpp"
#define DR_MP3_NO_STDIO
#define DR_MP3_IMPLEMENTATION
#include "third_party/dr_mp3.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace music {
namespace {
constexpr unsigned kRate = 48000;
constexpr std::size_t kRingFrames = 32768; // 256 KiB, independent of song length
constexpr std::size_t kBlock = 1024;
std::mutex mutex;
std::condition_variable changed;
std::thread worker;
std::atomic<bool> quitting{false};
std::atomic<bool> paused{false};
std::atomic<std::uint64_t> generation{0};
std::uint64_t next_id = 1;
std::string pending;
bool requested = false;
Snapshot current;

struct Decoder {
  int fd = -1;
  bool initialized = false;
  bool read_error = false;
  drmp3 mp3{};
  ~Decoder() {
    if (initialized) drmp3_uninit(&mp3);
    if (fd >= 0) close(fd);
  }
  static std::size_t read(void* context, void* out, std::size_t bytes) {
    auto& self = *static_cast<Decoder*>(context);
    ssize_t result;
    do { result = ::read(self.fd, out, bytes); } while (result < 0 && errno == EINTR);
    if (result < 0) self.read_error = true;
    return result > 0 ? static_cast<std::size_t>(result) : 0;
  }
  static drmp3_bool32 seek(void* context, int offset, drmp3_seek_origin origin) {
    const int whence = origin == DRMP3_SEEK_SET ? SEEK_SET : origin == DRMP3_SEEK_END ? SEEK_END : SEEK_CUR;
    return lseek(static_cast<Decoder*>(context)->fd, offset, whence) >= 0;
  }
  static drmp3_bool32 tell(void* context, drmp3_int64* cursor) {
    const off_t position = lseek(static_cast<Decoder*>(context)->fd, 0, SEEK_CUR);
    if (position < 0) return DRMP3_FALSE;
    *cursor = position;
    return DRMP3_TRUE;
  }
  bool open(const std::string& path) {
    fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    initialized = drmp3_init(&mp3, read, seek, tell, nullptr, this, nullptr);
    return initialized && (mp3.channels == 1 || mp3.channels == 2) &&
           mp3.sampleRate >= 8000 && mp3.sampleRate <= 48000;
  }
};

bool cancelled(std::uint64_t id) { return quitting || generation.load() != id; }

void fail(std::uint64_t id, const char* message) {
  std::lock_guard lock(mutex);
  if (current.id != id) return;
  current.state = State::failed;
  current.error = message;
  std::printf("[PS5-REACT] music failed id=%llu: %s\n", static_cast<unsigned long long>(id), message);
}

void decode(std::uint64_t id, const std::string& path) {
  // drmp3 includes its decode scratch: keep it off a console thread's stack.
  auto decoder = std::make_unique<Decoder>();
  if (!decoder->open(path)) { fail(id, "Cannot open or decode this local MP3"); return; }
  auto ring = std::make_shared<hui::audio::StreamRing>(kRingFrames);
  {
    std::lock_guard lock(mutex);
    if (current.id != id) return;
    current.ring = ring;
    if (decoder->mp3.totalPCMFrameCount != DRMP3_UINT64_MAX)
      current.duration = static_cast<double>(decoder->mp3.totalPCMFrameCount) / decoder->mp3.sampleRate;
  }
  std::printf("[PS5-REACT] music decoded id=%llu rate=%u channels=%u\n",
              static_cast<unsigned long long>(id), decoder->mp3.sampleRate, decoder->mp3.channels);
  std::array<float, 1024 * 2> source{};
  std::array<float, kBlock * 2> output{};
  std::size_t source_at = 0, source_frames = 0;
  auto frame = [&](std::array<float, 2>& value) {
    if (source_at == source_frames) {
      if (cancelled(id)) return false;
      source_frames = static_cast<std::size_t>(drmp3_read_pcm_frames_f32(&decoder->mp3, 1024, source.data()));
      source_at = 0;
      if (!source_frames) return false;
    }
    value[0] = source[source_at * decoder->mp3.channels];
    value[1] = decoder->mp3.channels == 2 ? source[source_at * 2 + 1] : value[0];
    ++source_at;
    return true;
  };
  std::array<float, 2> left{}, right{};
  if (!frame(left)) { fail(id, "The MP3 contains no decoded audio"); return; }
  bool has_right = frame(right);
  if (!has_right) right = left;
  double phase = 0;
  const double step = static_cast<double>(decoder->mp3.sampleRate) / kRate;
  std::uint64_t written = 0;
  bool eof = false;
  while (!cancelled(id) && !eof) {
    if (paused || ring->space() < kBlock) {
      std::unique_lock lock(mutex);
      changed.wait_for(lock, std::chrono::milliseconds(5));
      continue;
    }
    std::size_t count = 0;
    while (count < kBlock && !eof) {
      for (unsigned channel = 0; channel < 2; ++channel)
        output[count * 2 + channel] = left[channel] + static_cast<float>(phase) * (right[channel] - left[channel]);
      ++count;
      phase += step;
      while (phase >= 1.0 && !eof) {
        phase -= 1.0;
        if (!has_right) { eof = true; break; }
        left = right;
        has_right = frame(right);
        if (!has_right) right = left;
      }
    }
    if (cancelled(id)) return;
    {
      std::lock_guard lock(mutex);
      if (current.id == id) {
        const std::size_t accepted = ring->write(output.data(), count);
        // Keep the head/count publication consistent for a status reader. This
        // bounded 8 KiB copy never includes file I/O or decoder work.
        if (accepted != count) {
          current.state = State::failed;
          current.error = "Music buffer accounting failed";
          return;
        }
        written += accepted;
        current.produced_frames = written;
        current.state = State::playing;
      }
    }
  }
  if (cancelled(id)) return;
  if (decoder->read_error) { fail(id, "Reading the MP3 failed"); return; }
  // The end event belongs to audible completion, not decoder read-ahead.
  while (!cancelled(id) && ring->available()) {
    std::unique_lock lock(mutex);
    changed.wait_for(lock, std::chrono::milliseconds(5));
  }
  if (cancelled(id)) return;
  std::lock_guard lock(mutex);
  if (current.id != id) return;
  current.state = State::ended;
  current.duration = current.position = static_cast<double>(written) / kRate;
  std::printf("[PS5-REACT] music ended id=%llu\n", static_cast<unsigned long long>(id));
}

void run() {
  std::printf("[PS5-REACT] music worker ready\n");
  while (!quitting) {
    std::string path;
    std::uint64_t id;
    {
      std::unique_lock lock(mutex);
      changed.wait(lock, [] { return quitting || requested; });
      if (quitting) break;
      path = std::move(pending);
      requested = false;
      id = current.id;
    }
    decode(id, path);
  }
}
} // namespace

void start() {
  if (worker.joinable()) return;
  quitting = false;
  paused = false;
  worker = std::thread(run);
}
void shutdown() {
  quitting = true;
  generation.fetch_add(1);
  changed.notify_all();
  if (worker.joinable()) worker.join();
  std::lock_guard lock(mutex);
  current = {};
  pending.clear();
  requested = false;
}
std::uint64_t play(std::string path) {
  std::lock_guard lock(mutex);
  if (!worker.joinable() || quitting) return 0;
  const float volume = current.volume;
  current = {};
  current.id = next_id++;
  current.volume = volume;
  current.state = State::loading;
  generation.store(current.id);
  pending = std::move(path);
  requested = true;
  changed.notify_all();
  return current.id;
}
void stop() {
  std::lock_guard lock(mutex);
  generation.fetch_add(1);
  requested = false;
  pending.clear();
  current = {};
  changed.notify_all();
}
void set_paused(bool value) { paused = value; changed.notify_all(); }
void set_volume(float value) {
  std::lock_guard lock(mutex);
  current.volume = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0;
}
Snapshot snapshot() {
  std::lock_guard lock(mutex);
  Snapshot result = current;
  result.paused = paused;
  if (result.ring && result.state != State::ended) {
    const auto made = result.produced_frames;
    const auto buffered = result.ring->available();
    result.position = static_cast<double>(made > buffered ? made - buffered : 0) / kRate;
  }
  return result;
}
const char* state_name(State state) {
  switch (state) {
    case State::loading: return "loading";
    case State::playing: return "playing";
    case State::ended: return "ended";
    case State::failed: return "failed";
    default: return "idle";
  }
}
} // namespace music
