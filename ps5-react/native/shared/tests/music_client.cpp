// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "music_player.hpp"
#include "audio/stream_ring.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
music::Snapshot ready(std::uint64_t id) {
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < deadline) {
    auto status = music::snapshot();
    if (status.id == id && status.state != music::State::loading) return status;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("Music open timed out");
}
std::vector<float> complete(std::uint64_t id) {
  std::vector<float> samples;
  std::array<float, 1024 * 2> block{};
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  while (Clock::now() < deadline) {
    const auto status = music::snapshot();
    require(status.id == id, "Request identity changed while completing");
    require(status.state != music::State::failed, status.error.c_str());
    if (status.state == music::State::ended) return samples;
    if (status.ring && !status.paused) {
      require(status.ring->capacity() == 32768, "Unexpected buffer budget");
      const auto count = status.ring->read(block.data(), 1024);
      samples.insert(samples.end(), block.begin(), block.begin() + count * 2);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("Music completion timed out");
}
int main(int argc, char** argv) {
  require(argc == 5, "Expected mono, stereo, truncated MP3 and output paths");
  try {
    music::start();
    // A real 22.05 kHz mono MP3 is streamed/resampled to duplicated 48 kHz stereo.
    auto id = music::play(argv[1]);
    require(ready(id).state == music::State::playing, "Mono MP3 did not start");
    auto mono = complete(id);
    require(mono.size() > 20000 && mono.size() < 50000, "Wrong resampled mono duration");
    for (std::size_t i = 0; i < mono.size(); i += 2) {
      require(std::isfinite(mono[i]) && std::abs(mono[i]) <= 1, "Invalid decoded sample");
      require(mono[i] == mono[i + 1], "Mono channel duplication failed");
    }
    std::puts("PASS mono resampling and bounded stream");

    id = music::play(argv[2]);
    auto playing = ready(id);
    require(playing.state == music::State::playing, "Stereo MP3 did not start");
    std::array<float, 256 * 2> prefix{};
    playing.ring->read(prefix.data(), 256);
    music::set_paused(true);
    const double position = music::snapshot().position;
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    const auto held = music::snapshot();
    require(held.paused && held.position == position, "Paused playback position advanced");
    require(held.state != music::State::ended, "Read-ahead incorrectly emitted end");
    music::set_paused(false);
    const auto stereo = complete(id);
    require(stereo.size() > 30000, "Resume lost buffered PCM");
    std::FILE* out = std::fopen(argv[4], "wb");
    require(out != nullptr, "Cannot write decoded reference");
    std::fwrite(prefix.data(), sizeof(float), prefix.size(), out);
    std::fwrite(stereo.data(), sizeof(float), stereo.size(), out);
    std::fclose(out);
    std::puts("PASS pause/resume and audible end");

    id = music::play("/a-missing-owned-music-fixture.mp3");
    require(ready(id).state == music::State::failed, "Missing file was accepted");
    id = music::play(argv[3]);
    require(ready(id).state == music::State::failed, "Invalid MP3 was accepted");
    id = music::play(argv[1]);
    require(ready(id).state == music::State::playing, "A failed file broke the worker");
    complete(id);
    std::puts("PASS missing/corrupt input and recovery");

    std::int64_t max_api_us = 0;
    for (int i = 0; i < 250; ++i) {
      const auto began = Clock::now();
      id = music::play(i % 2 ? argv[1] : argv[2]);
      music::snapshot();
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - began).count();
      max_api_us = std::max<std::int64_t>(max_api_us, us);
    }
    require(ready(id).state == music::State::playing, "Latest request did not win");
    complete(id);
    require(max_api_us < 100000, "Command API blocked for over 100 ms");
    std::printf("PASS superseded requests; max_api_us=%lld\n", static_cast<long long>(max_api_us));

    music::set_volume(NAN);
    require(music::snapshot().volume == 0, "Non-finite volume was retained");
    music::set_volume(4);
    require(music::snapshot().volume == 1, "Volume was not bounded");
    for (int i = 0; i < 5; ++i) {
      music::play(argv[2]);
      music::stop();
      require(music::snapshot().state == music::State::idle, "Stop did not clear status");
      music::shutdown();
      music::start();
    }
    id = music::play(argv[2]);
    complete(id);
    music::shutdown();
    std::puts("PASS stop and repeated worker lifecycle");
  } catch (const std::exception& error) {
    music::shutdown();
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
  }
}
