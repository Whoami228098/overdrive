// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#include <string>
#include <vector>

namespace torrent {
enum State { queued, metadata, checking, downloading, paused, completed, failed, removed };
struct Status {
  int id = 0, state = queued;
  float progress = 0;
  long long total = 0, done = 0;
  unsigned rate_down = 0, rate_up = 0;
  int seeds = 0, peers = 0;
  char name[128] = {};
};
struct File { std::string path; long long size = 0; };
// Calls submit bounded commands/copy snapshots; session and disk I/O live on the worker.
// A later start changes the default root for new jobs; existing jobs keep their own root.
bool start(const char* save_root);
void stop();
int add_magnet(const char* uri, const char* name, long long size_hint);
int poll(Status* out, int max);
bool finished(int id);
void pause(int id);
void resume(int id);
void remove(int id, bool delete_files);
const char* last_error();
std::string error(int id);
std::vector<File> files(int id);
std::string destination(int id);
} // namespace torrent
