// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

namespace ps4_apps {
struct TitleState {
  std::string title_id, version, content_id, error;
  bool ready=false, exists=false, installed=false, installing=false, updating=false;
};
struct LaunchState { std::uint32_t id=0; std::string state="queued", title_id, error; };
// Render-thread entry points only copy bounded records and queue work.
bool status(const std::string& title, TitleState& out, std::string& error);
std::uint32_t launch(const std::string& title, std::string& error);
bool launch_status(std::uint32_t id, LaunchState& out, std::string& error);
// Worker-only initialization is shared with the foreground BGFT client.
bool initialize(std::string& error);
// Call after packages::stop(); all app/service workers are joined before termination.
void stop();
}
