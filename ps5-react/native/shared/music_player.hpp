// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace hui::audio { class StreamRing; }

// One bounded local MP3 stream. File I/O, decoding and resampling belong to the
// worker; the render thread only posts intent and reads a short status snapshot.
namespace music {
enum class State { idle, loading, playing, ended, failed };
struct Snapshot {
  std::uint64_t id = 0;
  State state = State::idle;
  bool paused = false;
  float volume = 0.35f;
  double position = 0, duration = 0;
  std::uint64_t produced_frames = 0;
  std::string error;
  std::shared_ptr<hui::audio::StreamRing> ring;
};

void start();                       // host startup, before JS
void shutdown();                    // host cleanup, before stopping audio
std::uint64_t play(std::string path); // latest request wins; no I/O
void stop();
void set_paused(bool paused);
void set_volume(float volume);
Snapshot snapshot();
const char* state_name(State state);
} // namespace music
