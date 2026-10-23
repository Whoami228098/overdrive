// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "async_log.hpp"
#include <cstdarg>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>
namespace async_log {
namespace { std::mutex lock; }
bool start() { return true; }
void stop() {}
void write(const char* format, ...) {
  char line[2048]; va_list args; va_start(args, format);
  const int size = std::vsnprintf(line, sizeof line - 2, format, args); va_end(args);
  if (size < 0) return;
  const auto count = static_cast<std::size_t>(size < static_cast<int>(sizeof line - 2) ? size : sizeof line - 2);
  line[count] = '\n';
  std::lock_guard<std::mutex> guard(lock);
  const int fd = ::open("/data/overdrive/resident/service.log", O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
  if (fd >= 0) { (void)::write(fd, line, count + 1); ::close(fd); }
}
}
