// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: MIT
// Restart-safe worker ownership and reports. No passwords are written here.
#pragma once
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace durable_archive {
inline constexpr const char* journal_name = ".ps5-react-worker-journal";
inline constexpr const char* cancel_name = ".ps5-react-worker-cancel";
inline constexpr const char* worker_lock_name = ".ps5-react-worker-lock";
inline constexpr std::uint64_t max_journal_bytes = 4ULL << 30;

inline bool write_all(int fd, const char* data, std::size_t size) {
  while (size) {
    const auto count = ::write(fd, data, size);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    data += count;
    size -= static_cast<std::size_t>(count);
  }
  return true;
}

// fcntl is already used by console titles. Closing the descriptor releases this
// process's lock, including after a crash. The file itself is never unlinked.
class Lease {
public:
  ~Lease() { if (fd_ >= 0) ::close(fd_); }
  Lease() = default;
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;
  bool acquire(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0666);
    if (fd_ < 0) return false;
    struct stat st{};
    struct flock lock{};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    const bool ok = !::fstat(fd_, &st) && S_ISREG(st.st_mode) && !::fcntl(fd_, F_SETLK, &lock);
    if (!ok) { const int reason = errno; ::close(fd_); fd_ = -1; errno = reason; }
    return ok;
  }
private:
  int fd_ = -1;
};

inline bool token(const std::string& value) {
  return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
inline std::string header(const std::string& identity, std::uint64_t limit, const std::string& generation) {
  return "P5WK002:" + identity + ":" + std::to_string(limit) + ":" + generation + "\n";
}

class Journal {
public:
  ~Journal() { if (fd_ >= 0) ::close(fd_); }
  bool open(const std::string& directory, const std::string& identity, std::uint64_t limit, const std::string& generation) {
    if (!token(identity) || !token(generation) ||
        !lease_.acquire(directory + "/" + worker_lock_name)) return false;
    const auto expected = header(identity, limit, generation);
    fd_ = ::open((directory + "/" + journal_name).c_str(), O_WRONLY | O_APPEND | O_NOFOLLOW | O_NONBLOCK);
    if (fd_ < 0) return false;
    struct stat st{};
    // A worker claims a new journal only. Reopening belongs to the controller,
    // which follows the old worker instead of spawning a second decoder.
    if (::fstat(fd_, &st) || !S_ISREG(st.st_mode) || st.st_size != static_cast<off_t>(expected.size())) return false;
    const int input = ::open((directory + "/" + journal_name).c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (input < 0) return false;
    char saved[256]{};
    const auto count = ::read(input, saved, sizeof saved);
    ::close(input);
    if (count != static_cast<ssize_t>(expected.size()) || std::memcmp(saved, expected.data(), expected.size())) return false;
    directory_ = directory;
    bytes_ = expected.size();
    ready_ = true;
    return true;
  }
  bool append(const std::string& line) {
    const auto text = line + "\n";
    if (!ready_ || text.size() > max_journal_bytes - bytes_ || !write_all(fd_, text.data(), text.size())) return false;
    bytes_ += text.size();
    if ((line == "ok" || line == "cancelled" || line.starts_with("fail ")) && ::fsync(fd_)) return false;
    return true;
  }
  bool cancelled() const {
    struct stat st{};
    return !::lstat((directory_ + "/" + cancel_name).c_str(), &st) && S_ISREG(st.st_mode);
  }
private:
  Lease lease_;
  std::string directory_;
  std::uint64_t bytes_ = 0;
  int fd_ = -1;
  bool ready_ = false;
};
}
