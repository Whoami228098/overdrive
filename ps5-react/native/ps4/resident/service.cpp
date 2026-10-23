// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "stream.hpp"
#include "torrent_api.hpp"
#include "network.hpp"
#include "async_log.hpp"
#include "bgft_abi.hpp"
#include "protocol.hpp"
#include "archives.hpp"
#include "digest.hpp"
#include "app_identity.h"
#include <orbis/UserService.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/file.h>
#include <thread>
#include <unistd.h>

extern "C" int sceSysmoduleLoadModuleInternal(std::uint32_t);
extern "C" int sceBgftServiceDownloadPauseTask(int);
extern "C" int sceBgftServiceDownloadResumeTask(int);
extern "C" int sceBgftServiceDownloadFindTaskByContentId(const char*, int, int*);

namespace resident_service {
namespace {
using namespace resident_protocol;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
constexpr char base[] = "/data/overdrive/resident";
constexpr char job_dir[] = "/data/overdrive/resident/jobs";
constexpr char archive_dir[] = "/data/overdrive/resident/history";
#ifdef __Orbis__
constexpr int no_signal = 0x20000; // FreeBSD/Orbis MSG_NOSIGNAL.
#else
constexpr int no_signal = MSG_NOSIGNAL; // Owned Linux service fixtures.
#endif
std::atomic<bool> alive{false}, ready{false};
int listener = -1, lease = -1;
std::thread listener_thread, controller_thread, registration_thread, archive_thread;
std::array<std::thread, 2> controls, feeders;
std::mutex jobs_mutex, sockets_mutex, queues_mutex;
std::condition_variable queue_wake, controller_wake;
std::set<int> sockets;
std::deque<int> control_queue;
std::deque<int> registration_queue;
std::deque<int> archive_queue;
std::string token;
int next_id = 1;

struct Job {
  int id = 0, engine = 0, bgft = -1;
  std::string key, uri, name, path, content, state = "queued", error, status;
  std::string password, requested_archive, archive_source;
  std::uint64_t archive_bytes = 0;
  bool archive_pending = false;
  std::uint64_t hint = 0, size = 0, written = 0;
  unsigned rate = 0;
  int seeds = 0, peers = 0;
  bool want_paused = false, pause_sent = false, resume_pending = false, registered = false, header_checked = false;
  bool retiring = false, need_feeder = false, registration_uncertain = false, retry_engine = false, ownership_verified = false;
  bool start_pending = false, pause_retiring = false;
  bool registration_pending = false;
  std::vector<char> header_bytes;
  unsigned readers = 0;
  Clock::time_point last_use = Clock::now();
};
std::map<int, Job> jobs;
std::map<std::string, int> history_keys;
std::map<int, std::string> history_paths;
struct Request { std::string method, path, body, range, auth; };
struct Feed { int socket; Request request; };
std::deque<Feed> feed_queue;

struct Fd {
  int value;
  explicit Fd(int fd = -1) : value(fd) {}
  ~Fd() { if (value >= 0) ::close(value); }
  Fd(const Fd&) = delete;
};
void directory(const std::string& path) {
  struct stat info{};
  if (::lstat(path.c_str(), &info) == 0) {
    if (!S_ISDIR(info.st_mode)) throw std::runtime_error("Resident path is not a directory: " + path);
  } else if (errno != ENOENT || ::mkdir(path.c_str(), 0700)) throw std::runtime_error("Cannot create resident directory: " + path);
}
bool write_file(int fd, const std::string& bytes) {
  for (std::size_t offset = 0; offset < bytes.size();) {
    const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return ::fsync(fd) == 0;
}
std::string read_file(const std::string& path, std::size_t max) {
  Fd fd(::open(path.c_str(), O_RDONLY | O_NOFOLLOW)); struct stat info{};
  if (fd.value < 0 || ::fstat(fd.value, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      static_cast<std::uint64_t>(info.st_size) > max) throw std::runtime_error("Cannot read bounded resident file: " + path);
  std::string out(static_cast<std::size_t>(info.st_size), '\0');
  for (std::size_t offset = 0; offset < out.size();) {
    const auto count = ::read(fd.value, out.data() + offset, out.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("Short resident file: " + path);
    offset += static_cast<std::size_t>(count);
  }
  return out;
}
void atomic_file(const std::string& path, const std::string& bytes) {
  static std::atomic<unsigned> serial{0};
  const std::string temporary = path + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(serial++);
  Fd out(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
  if (out.value < 0) throw std::runtime_error("Cannot create resident checkpoint");
  if (!write_file(out.value, bytes) || ::rename(temporary.c_str(), path.c_str())) {
    ::unlink(temporary.c_str()); throw std::runtime_error("Cannot commit resident checkpoint");
  }
  Fd parent(::open(job_dir, O_RDONLY));
  if (parent.value >= 0) (void)::fsync(parent.value);
}
void save(const Job& job) {
  const std::string record = "1\n" + std::to_string(job.id) + "\n" + job.key + "\n" + hex(job.uri) + "\n" +
    hex(job.name) + "\n" + std::to_string(job.hint) + "\n" + hex(job.path) + "\n" + std::to_string(job.size) + "\n" +
    std::to_string(job.bgft + 1LL) + "\n" + job.state + "\n" + hex(job.error) + "\n" +
    (job.want_paused ? "1\n" : "0\n") + hex(job.content) + "\n" + (job.registration_uncertain ? "1\n" : "0\n") + (job.start_pending ? "1\n" : "0\n") +
    hex(job.password)+"\n"+hex(job.requested_archive)+"\n"+hex(job.archive_source)+"\n"+std::to_string(job.archive_bytes)+"\n";
  atomic_file(std::string(job_dir) + "/" + std::to_string(job.id) + ".job", record);
}
Job decode(const std::string& path) {
    auto fields = lines(read_file(path, 65536));
    if ((fields.size() < 13 || (fields.size() > 15 && fields.size()!=19)) || fields[0] != "1") throw std::runtime_error("Invalid resident job record; preserve it for diagnosis");
    Job job; job.id = static_cast<int>(number(fields[1], INT_MAX - 1));
    if (!job.id) throw std::runtime_error("Invalid resident job identity");
    job.key = fields[2]; job.uri = unhex(fields[3], 16384); job.name = unhex(fields[4], 512);
    job.hint = number(fields[5], 9007199254740991ULL); job.path = unhex(fields[6], 4096);
    job.size = number(fields[7], 1ULL << 50); job.bgft = static_cast<int>(static_cast<std::int64_t>(number(fields[8], static_cast<std::uint64_t>(INT_MAX) + 1)) - 1);
    job.state = fields[9]; job.error = unhex(fields[10], 4096); job.want_paused = fields[11] == "1";
    job.content = unhex(fields[12], 64);
    job.registration_uncertain = fields.size() >= 14 && fields[13] == "1";
    if (fields.size() >= 14 && fields[13] != "0" && fields[13] != "1") throw std::runtime_error("Invalid registration checkpoint flag");
    job.start_pending = job.state == "registered" || (fields.size() >= 15 && fields[14] == "1");
    if (fields.size() >= 15 && fields[14] != "0" && fields[14] != "1") throw std::runtime_error("Invalid start checkpoint flag");
    if(fields.size()==19){job.password=unhex(fields[15],1024);job.requested_archive=unhex(fields[16],4096);
      job.archive_source=unhex(fields[17],4096);job.archive_bytes=number(fields[18],1ULL<<40);}
    const std::set<std::string> states{"queued", "connecting", "verifying", "downloading", "installing", "pausing", "paused", "registering", "registered", "completed", "failed"};
    if (!states.count(job.state)) throw std::runtime_error("Invalid resident checkpoint state");
    std::string error;
    if (torrent::stream::key(job.uri, error) != job.key ||
        (fields[11] != "0" && fields[11] != "1")) throw std::runtime_error("Resident magnet record does not match its identity");
    if (!job.path.empty()) {
      const fs::path expected = !job.archive_source.empty()&&job.path!=job.archive_source?
        fs::path(base)/"archives"/job.key/"unpacked":fs::path("/data/downloads/torrents") / job.key;
      const auto relative = fs::path(job.path).lexically_relative(expected);
      if (relative.empty() || relative.is_absolute()) throw std::runtime_error("Invalid saved package path");
      for (const auto& part : relative) if (part == "..") throw std::runtime_error("Saved package escapes its torrent directory");
    }
    if(!job.archive_source.empty()) {
      const auto relative=fs::path(job.archive_source).lexically_relative(fs::path("/data/downloads/torrents")/job.key);
      if(relative.empty()||relative.is_absolute()||!job.archive_bytes)throw std::runtime_error("Invalid saved archive identity");
      for(const auto& part:relative)if(part=="..")throw std::runtime_error("Saved archive escapes its torrent root");
    }
    if (job.state == "registering" && job.bgft < 0) {
      job.registration_uncertain = true;
      job.state = "failed"; job.error = "Registration was interrupted. Inspect Notifications > Downloads before retrying; no duplicate was submitted.";
    } else if (job.want_paused && job.state != "failed" && job.state != "completed") job.state = job.bgft >= 0 ? "pausing" : "paused";
    else if (job.state != "completed" && job.state != "failed") job.state = "queued";
    job.resume_pending = job.bgft >= 0 && !job.start_pending && !job.want_paused && job.state != "completed" && job.state != "failed";
    if (job.state == "completed") job.written = job.size;
    return job;
}
void load() {
  std::set<std::string> keys;
  for (const auto& item : fs::directory_iterator(job_dir)) {
    if (item.path().extension() != ".job") continue;
    if (jobs.size() >= 64) throw std::runtime_error("Resident queue has too many active records");
    Job job = decode(item.path().string());
    if (item.path().filename() != std::to_string(job.id) + ".job" || jobs.count(job.id) || !keys.insert(job.key).second) throw std::runtime_error("Duplicate active resident record");
    next_id = std::max(next_id, job.id + 1); jobs.emplace(job.id, std::move(job));
  }
  for (const auto& item : fs::directory_iterator(archive_dir)) {
    if (item.path().extension() != ".job") continue;
    if (history_paths.size() >= 65536) throw std::runtime_error("Resident history index exceeds its bounded limit");
    const auto name = item.path().filename().string(); const auto dash = name.find('-');
    if (dash == name.npos) throw std::runtime_error("Invalid history filename");
    const int id = static_cast<int>(number(name.substr(0, dash), INT_MAX - 1));
    const auto key = name.substr(dash + 1, name.size() - dash - 5);
    if (!id || !((key.starts_with("v1-") && key.size() == 43) || (key.starts_with("v2-") && key.size() == 67)) ||
        key.substr(3).find_first_not_of("0123456789abcdef") != key.npos || jobs.count(id) || keys.count(key) || history_paths.count(id) || history_keys.count(key)) throw std::runtime_error("Invalid or conflicting resident history identity");
    history_keys.emplace(key, id); history_paths.emplace(id, item.path().string()); next_id = std::max(next_id, id + 1);
  }
}
bool restore_locked(int id) {
  if (jobs.count(id)) return true;
  const auto found = history_paths.find(id); if (found == history_paths.end()) return false;
  Job job = decode(found->second);
  if (job.id != id || history_keys.find(job.key) == history_keys.end() || history_keys.at(job.key) != id ||
      (job.state != "completed" && job.state != "failed")) throw std::runtime_error("History record does not match its index");
  const std::string active = std::string(job_dir) + "/" + std::to_string(id) + ".job";
  struct stat existing{};
  if (::lstat(active.c_str(), &existing) == 0 || errno != ENOENT || ::rename(found->second.c_str(), active.c_str())) throw std::runtime_error("Cannot reactivate resident history without replacing a record");
  history_keys.erase(job.key); history_paths.erase(found); jobs.emplace(id, std::move(job)); return true;
}
void housekeeping() {
  torrent::Status states[32]; const int count = torrent::poll(states, 32);
  std::lock_guard<std::mutex> lock(jobs_mutex);
  for (auto it = jobs.begin(); it != jobs.end();) {
    auto& job = it->second;
    const bool terminal = job.state == "completed" || job.state == "failed";
    if (job.retiring) {
      bool present = false, removed = false;
      for (int i = 0; i < count; ++i) if (states[i].id == job.engine) { present = true; removed = states[i].state == torrent::removed; }
      // add_magnet prunes removed snapshots. A missing monotonic engine ID is
      // also an acknowledgement after this service already requested remove.
      if (removed || !present) { job.engine = 0; job.retiring = false; job.need_feeder = job.readers > 0; }
      ++it; continue;
    }
    if (!terminal) { ++it; continue; }
    if (job.readers) { ++it; continue; }
    const bool idle = Clock::now() - job.last_use > std::chrono::seconds(60);
    if (!idle && jobs.size() < 24) { ++it; continue; }
    if (job.engine) { torrent::stream::suspend(job.engine); torrent::remove(job.engine, false); job.retiring = true; ++it; continue; }
    if (history_paths.size() >= 65536) { ++it; continue; }
    const auto destination = std::string(archive_dir) + "/" + std::to_string(job.id) + "-" + job.key + ".job";
    struct stat existing{};
    if (::lstat(destination.c_str(), &existing) == 0 || errno != ENOENT) { ++it; continue; }
    if (::rename((std::string(job_dir) + "/" + std::to_string(job.id) + ".job").c_str(), destination.c_str())) { ++it; continue; }
    history_paths.emplace(job.id, destination); history_keys.emplace(job.key, job.id); it = jobs.erase(it);
  }
}
void auth() {
  const std::string path = std::string(base) + "/auth";
  struct stat info{};
  if (::lstat(path.c_str(), &info) == 0) token = read_file(path, 65);
  else {
    if (errno != ENOENT) throw std::runtime_error("Cannot inspect resident authentication file");
    std::string bytes(32, '\0'); Fd random(::open("/dev/urandom", O_RDONLY));
    if (random.value < 0) random.value = ::open("/dev/random", O_RDONLY);
    if (random.value < 0) throw std::runtime_error("Cannot open console random source");
    for (std::size_t offset = 0; offset < bytes.size();) {
      const auto count = ::read(random.value, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) throw std::runtime_error("Cannot read console random source");
      offset += static_cast<std::size_t>(count);
    }
    token = hex(bytes); Fd out(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
    if (out.value < 0 || !write_file(out.value, token + "\n")) throw std::runtime_error("Cannot save resident authentication token");
  }
  if (!token.empty() && token.back() == '\n') token.pop_back();
  if (token.size() != 64 || token.find_first_not_of("0123456789abcdef") != token.npos) throw std::runtime_error("Invalid resident authentication file; it was preserved");
}
int code(const std::string& state) {
  if (state == "paused") return 4;
  if (state == "completed") return 5;
  if (state == "failed") return 6;
  if (state == "connecting") return 1;
  if (state == "verifying") return 2;
  if (state == "queued") return 0;
  return 3;
}
std::string snapshot(const Job& job) {
  std::string out = "{\"id\":" + std::to_string(job.id) + ",\"state\":" + json(job.state) + ",\"stateCode\":" + std::to_string(code(job.state)) +
    ",\"name\":" + json(job.name) + ",\"error\":" + json(job.error) + ",\"status\":" + json(job.status) +
    ",\"destination\":" + json(job.path) + ",\"total\":" + std::to_string(job.size ? job.size : job.hint) +
    ",\"written\":" + std::to_string(job.written) + ",\"completedBytes\":" + std::to_string(job.written) +
    ",\"bytesPerSecond\":" + std::to_string(job.rate) + ",\"seeds\":" + std::to_string(job.seeds) + ",\"peers\":" + std::to_string(job.peers) +
    ",\"systemInstall\":true,\"installed\":false,\"contentId\":" + json(job.content);
  if (job.bgft >= 0) out += ",\"bgftId\":" + std::to_string(job.bgft);
  return out + '}';
}
void fail(int id, const std::string& error) {
  std::lock_guard<std::mutex> lock(jobs_mutex); auto& job = jobs.at(id);
  if (job.want_paused && error.find("Package stream paused") != error.npos) return;
  if (job.state == "registering" && job.bgft < 0) job.registration_uncertain = true;
  job.state = "failed"; job.error = error; job.rate = 0;
  if (job.engine) { try { torrent::stream::suspend(job.engine); } catch (...) {} }
  try { save(job); } catch (const std::exception& e) { job.error += "; checkpoint: " + std::string(e.what()); }
  // No URI, authentication token or private catalog URL is logged.
  async_log::write("[resident] job %d failed: %s", id, job.error.c_str());
}

bool socket_wait(int fd, bool writing, Clock::time_point deadline) {
  if (fd < 0 || fd >= FD_SETSIZE) return false;
  while (alive && Clock::now() < deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
    if (left <= 0) break;
    timeval wait{static_cast<long>(left / 1000000), static_cast<long>(left % 1000000)};
    fd_set set; FD_ZERO(&set); FD_SET(fd, &set);
    const int rc = ::select(fd + 1, writing ? nullptr : &set, writing ? &set : nullptr, nullptr, &wait);
    if (rc > 0) return true;
    if (!rc || errno != EINTR) break;
  }
  return false;
}
bool send_bytes(int fd, const char* bytes, std::size_t size) {
  const auto deadline = Clock::now() + std::chrono::seconds(15);
  for (std::size_t offset = 0; offset < size;) {
    const auto count = ::send(fd, bytes + offset, size - offset, no_signal);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && socket_wait(fd, true, deadline)) continue;
    if (count <= 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}
bool send_text(int fd, const std::string& text) { return send_bytes(fd, text.data(), text.size()); }
void respond(int fd, int status, const std::string& body, const std::string& extra = "") {
  const char* reason = status == 200 ? "OK" : status == 201 ? "Created" : status == 206 ? "Partial Content" :
    status == 400 ? "Bad Request" : status == 403 ? "Forbidden" : status == 404 ? "Not Found" : status == 416 ? "Range Not Satisfiable" : "Service Unavailable";
  const std::string header = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n";
  if (send_text(fd, header)) (void)send_text(fd, body);
}
void close_socket(int fd) {
  { std::lock_guard<std::mutex> lock(sockets_mutex); sockets.erase(fd); }
  ::close(fd);
}
Request receive(int fd) {
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  std::string wire; std::size_t split = wire.npos;
  std::array<char, 4096> buffer{};
  auto more = [&] {
    if (!socket_wait(fd, false, deadline)) throw std::runtime_error("HTTP receive deadline");
    const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (count <= 0) throw std::runtime_error("HTTP peer closed request");
    wire.append(buffer.data(), static_cast<std::size_t>(count));
    if (wire.size() > 49152) throw std::runtime_error("HTTP request exceeds budget");
  };
  while ((split = wire.find("\r\n\r\n")) == wire.npos) { if (wire.size() >= 16384) throw std::runtime_error("HTTP header exceeds budget"); more(); }
  if (split > 16384 || wire.find('\0') != wire.npos) throw std::runtime_error("Invalid HTTP header");
  const auto first_end = wire.find("\r\n");
  const std::string first = wire.substr(0, first_end);
  const auto space = first.find(' '), last_space = first.rfind(' ');
  if (space == first.npos || last_space == space || (first.substr(last_space + 1) != "HTTP/1.1" && first.substr(last_space + 1) != "HTTP/1.0")) throw std::runtime_error("Invalid HTTP request line");
  Request request; request.method = first.substr(0, space); request.path = first.substr(space + 1, last_space - space - 1);
  if (request.path.empty() || request.path.size() > 512 || request.path[0] != '/') throw std::runtime_error("Invalid HTTP path");
  std::set<std::string> seen; std::uint64_t content_length = 0;
  for (std::size_t at = first_end + 2; at < split;) {
    const auto end = wire.find("\r\n", at), colon = wire.find(':', at);
    if (end == wire.npos || colon == wire.npos || colon >= end) throw std::runtime_error("Invalid HTTP field");
    const auto name = lower(wire.substr(at, colon - at));
    std::string value = wire.substr(colon + 1, end - colon - 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    if (name == "transfer-encoding") throw std::runtime_error("Chunked requests are unsupported");
    if ((name == "content-length" || name == "range" || name == "x-overdrive-token") && !seen.insert(name).second) throw std::runtime_error("Duplicate HTTP field");
    if (name == "content-length") content_length = number(value, 32768);
    if (name == "range") request.range = value;
    if (name == "x-overdrive-token") request.auth = value;
    at = end + 2;
  }
  if (request.method == "POST" && !seen.count("content-length")) throw std::runtime_error("POST requires Content-Length");
  while (wire.size() - split - 4 < content_length) more();
  if (wire.size() - split - 4 != content_length) throw std::runtime_error("Pipelined requests are unsupported");
  request.body = wire.substr(split + 4); return request;
}

namespace bgft {
ps4_bgft::Init heap{}; bool initialized = false, own_init = false;
struct Parameters { std::string url, name; ps4_bgft::DownloadEx value{}; };
std::deque<std::unique_ptr<Parameters>> retained;
std::string error(const char* name, int result) {
  char value[160]; std::snprintf(value, sizeof value, "%s failed (0x%08x); check Notifications > Downloads", name, static_cast<unsigned>(result)); return value;
}
void initialize() {
  if (initialized) return;
  (void)sceSysmoduleLoadModuleInternal(0x8000002a);
  heap.heap_size = 1024 * 1024; heap.heap = std::calloc(1, heap.heap_size);
  if (!heap.heap) throw std::runtime_error("Cannot allocate resident BGFT heap");
  const int rc = sceBgftServiceInit(&heap);
  if (rc) {
    std::free(heap.heap); heap = {};
    // Public resident reference SSPI handles this as an already initialized
    // process client. Never terminate or free another client's service heap.
    if (static_cast<unsigned>(rc) == 0x80990001u) { initialized = true; own_init = false; return; }
    throw std::runtime_error(error("sceBgftServiceInit", rc));
  }
  initialized = own_init = true;
}
int registration(const std::string& url, const std::string& name) {
  initialize(); int user = -1;
  const int user_rc = sceUserServiceGetForegroundUser(&user);
  if (user_rc || user < 0) throw std::runtime_error(error("sceUserServiceGetForegroundUser", user_rc));
  auto parameters = std::make_unique<Parameters>(); parameters->url = url; parameters->name = name;
  auto& params = parameters->value;
  params.param.user_id = user; params.param.entitlement_type = 5; params.param.id = "";
  params.param.url = parameters->url.c_str(); params.param.name = parameters->name.c_str(); params.param.icon = ""; params.param.scenario = "0";
  params.param.options = ps4_bgft::disable_cdn_query; // Visible system Downloads.
  retained.push_back(std::move(parameters));
  int task = -1; const int rc = sceBgftServiceIntDebugDownloadRegisterPkg(&params, &task);
  if (rc || task < 0) throw std::runtime_error(error("sceBgftServiceIntDebugDownloadRegisterPkg", rc));
  return task;
}
ps4_bgft::Progress progress(int id) {
  initialize(); ps4_bgft::Progress result{}; const int rc = sceBgftServiceDownloadGetProgress(id, &result);
  if (rc) throw std::runtime_error(error("sceBgftServiceDownloadGetProgress", rc));
  return result;
}
void verify_owner(const Job& job) {
  if (job.bgft < 0 || job.ownership_verified) return;
  initialize();
  if (job.content.size() != 36) throw std::runtime_error("Saved BGFT task has no verified package identity");
  // Read-only lookup may confirm this exact recorded ID, never adopt another
  // download merely because its content/title matches.
  for (int subtype : {6, 7, 8, 12}) {
    int found = -1;
    if (sceBgftServiceDownloadFindTaskByContentId(job.content.c_str(), subtype, &found) == 0 && found == job.bgft) return;
  }
  throw std::runtime_error("Saved BGFT ID no longer matches this verified PKG; inspect system Downloads. No other task was controlled.");
}
void stop() {
  if (!initialized) return;
  // Never stop or unregister accepted system tasks during component shutdown.
  const int rc = own_init ? sceBgftServiceTerm() : 0;
  if (rc) throw std::runtime_error(error("sceBgftServiceTerm", rc));
  std::free(heap.heap); heap = {}; initialized = own_init = false; retained.clear();
}
}

bool job_copy(int id, Job& out) { std::lock_guard<std::mutex> lock(jobs_mutex); const auto it = jobs.find(id); if (it == jobs.end()) return false; out = it->second; return true; }
std::string choose(const std::vector<torrent::File>& files) {
  std::vector<std::string> packages, primary;
  for (const auto& file : files) {
    const auto name = lower(fs::path(file.path).filename().string());
    if (name.size() < 5 || name.substr(name.size() - 4) != ".pkg" || file.size < 0x440) continue;
    packages.push_back(file.path);
    const auto full = lower(file.path);
    if (full.find("backport") == full.npos && name.find("patch") == name.npos && name.find("update") == name.npos &&
        name.find("dlc") == name.npos && name.find("fix") == name.npos) primary.push_back(file.path);
  }
  if (packages.size() == 1) return packages.front();
  if (primary.size() == 1) return primary.front();
  throw std::runtime_error(packages.empty() ? "Torrent contains no directly installable PS4 PKG" :
    "Torrent contains multiple ambiguous PKGs; use a magnet containing one base package. No package was guessed or installed.");
}
std::uint64_t big64(const char* data) { std::uint64_t value = 0; for (int i = 0; i < 8; ++i) value = (value << 8) | static_cast<unsigned char>(data[i]); return value; }
#include "archive_service.inc"
bool header(Job& job) {
  std::vector<char> chunk;
  auto& bytes = job.header_bytes;
  std::string error;
  if(archived_package(job)) {
    Fd file(::open(job.path.c_str(),O_RDONLY|O_NOFOLLOW));struct stat status{};bytes.resize(0x440);
    if(file.value<0||::fstat(file.value,&status)||!S_ISREG(status.st_mode)||status.st_size<0||static_cast<std::uint64_t>(status.st_size)!=job.size||
       ::pread(file.value,bytes.data(),bytes.size(),0)!=static_cast<ssize_t>(bytes.size()))throw std::runtime_error("Extracted PS4 package changed or cannot be read");
  }
  while (bytes.size() < 0x440) {
    if (!torrent::stream::peek(job.engine, bytes.size(), 0x440 - bytes.size(), chunk, error)) {
      if (!error.empty()) throw std::runtime_error("PKG header: " + error);
      return false;
    }
    bytes.insert(bytes.end(), chunk.begin(), chunk.end());
  }
  if (std::memcmp(bytes.data(), "\x7f" "CNT", 4) || big64(bytes.data() + 0x430) != job.size) throw std::runtime_error("Verified torrent file has an invalid PS4 PKG header or size");
  job.content.assign(bytes.data() + 0x40, 36);
  char title_id[10];if(!overdrive_ps4_content_title(job.content.c_str(),title_id))throw std::runtime_error("Extracted content is not a PS4 package identity");
  if (job.content.size() != 36 || job.content[6] != '-' || job.content[16] != '_' || job.content[19] != '-' ||
      job.content.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != job.content.npos) throw std::runtime_error("PKG has an invalid PS4 content ID");
  job.header_checked = true; job.header_bytes.clear(); return true;
}
void install_package(Job job,torrent::Status state={}) {
  const int id=job.id;
  if (job.state == "completed") return; // A final range read must not register/install again.
  if (job.bgft < 0) {
    bgft::initialize();
    { std::lock_guard<std::mutex> lock(jobs_mutex); auto& current = jobs.at(id); if (current.want_paused) return; current.registration_pending = true; current.status = "Waiting for system package registration"; }
    { std::lock_guard<std::mutex> lock(queues_mutex); registration_queue.push_back(id); }
    queue_wake.notify_all(); return;
  } else if (job.start_pending || (!job.registered && !job.resume_pending && job.state != "completed")) {
    bgft::initialize(); const int rc = sceBgftServiceDownloadStartTask(job.bgft);
    if (rc) { const int resume = sceBgftServiceDownloadResumeTask(job.bgft); if (resume) throw std::runtime_error(bgft::error("StartTask/ResumeTask", resume)); }
    std::lock_guard<std::mutex> lock(jobs_mutex); auto& current = jobs.at(id);
    current.registered = true; current.start_pending = false; current.resume_pending = false; current.state = "installing"; save(current); job.resume_pending = false;
  }
  if (job.bgft >= 0 && job.resume_pending) {
    bgft::initialize(); const int rc = sceBgftServiceDownloadResumeTask(job.bgft);
    if (rc) throw std::runtime_error(bgft::error("sceBgftServiceDownloadResumeTask", rc));
    std::lock_guard<std::mutex> lock(jobs_mutex); auto& current = jobs.at(id); current.resume_pending = false; current.registered = true;
  }
  const auto progress = bgft::progress(job.bgft);
  if (progress.error) throw std::runtime_error(bgft::error("BGFT transfer", progress.error));
  const auto total = progress.length_total ? progress.length_total : progress.length;
  const auto written = progress.length_total ? progress.transferred_total : progress.transferred;
  // Preparation may report a small metadata/header transfer. It cannot finish
  // the package row before at least the entire verified PKG size is transferred.
  const bool complete = total >= job.size && job.size > 0 && written >= total;
  std::lock_guard<std::mutex> lock(jobs_mutex); auto& current = jobs.at(id);
  current.written = written; current.rate = state.rate_down; current.seeds = state.seeds; current.peers = state.peers;
  if (!current.want_paused) {
    if (complete) {
      if (current.state != "completed") { current.state = "completed"; current.rate = 0; current.status = "Transfer complete; system Downloads owns final installation checks"; save(current); torrent::pause(job.engine); }
    } else { current.state = "installing"; current.status = "Downloading and installing in PS4 Notifications > Downloads"; }
  }
}

void tick(int id) {
  Job job; if (!job_copy(id, job)) return;
  if (job.state == "failed" || job.retiring || job.registration_pending || job.archive_pending || (job.state == "completed" && !job.need_feeder)) return;
  if (job.bgft >= 0 && job.state != "completed" && !job.ownership_verified) {
    bgft::verify_owner(job);
    std::lock_guard<std::mutex> lock(jobs_mutex); jobs.at(id).ownership_verified = true;
    job.ownership_verified = true;
  }
  if(archived_package(job)) {
    if(job.want_paused) {
      if(job.bgft>=0&&!job.pause_sent) {bgft::initialize();const int rc=sceBgftServiceDownloadPauseTask(job.bgft);
        if(rc)throw std::runtime_error(bgft::error("Archive package pause",rc));}
      std::lock_guard<std::mutex> lock(jobs_mutex);auto& current=jobs.at(id);current.pause_sent=true;current.state="paused";save(current);return;
    }
    if(!job.header_checked) {header(job);std::lock_guard<std::mutex> lock(jobs_mutex);auto& current=jobs.at(id);
      current.header_checked=true;current.content=job.content;save(current);}
    install_package(job);return;
  }
  if(!job.want_paused&&saved_archive(job)) {queue_archive(id);return;}
  if (!job.engine) {
    if (job.want_paused) {
      // Reconcile a crash between durable pause intent and the native pause RPC.
      if (job.bgft >= 0 && !job.pause_sent) {
        bgft::initialize(); const int rc = sceBgftServiceDownloadPauseTask(job.bgft);
        if (rc) throw std::runtime_error(bgft::error("sceBgftServiceDownloadPauseTask", rc));
        std::lock_guard<std::mutex> lock(jobs_mutex); auto& current = jobs.at(id); current.pause_sent = true; current.state = "paused"; save(current);
      }
      return;
    }
    torrent::Status active[32]; const int active_count = torrent::poll(active, 32);
    const int occupied = static_cast<int>(std::count_if(active, active + active_count, [](const auto& state) { return state.state != torrent::removed; }));
    if (occupied >= (job.need_feeder ? 32 : 30)) return; // Keep two slots for final system range reads.
    const int engine = torrent::add_magnet(job.uri.c_str(), job.name.c_str(), static_cast<long long>(job.hint));
    if (!engine) throw std::runtime_error(torrent::last_error());
    { std::lock_guard<std::mutex> lock(jobs_mutex); jobs.at(id).engine = engine; }
    job.engine = engine;
  }
  torrent::Status states[32]; const int count = torrent::poll(states, 32); torrent::Status state{};
  for (int i = 0; i < count; ++i) if (states[i].id == job.engine) state = states[i];
  if (job.want_paused) {
    if (!job.pause_sent) {
      if (job.bgft >= 0) { bgft::initialize(); const int rc = sceBgftServiceDownloadPauseTask(job.bgft); if (rc) throw std::runtime_error(bgft::error("sceBgftServiceDownloadPauseTask", rc)); }
      torrent::stream::suspend(job.engine);
      if (state.state == torrent::failed) torrent::remove(job.engine, false);
      else torrent::pause(job.engine);
      std::lock_guard<std::mutex> lock(jobs_mutex); auto& current = jobs.at(id); current.pause_sent = true; current.pause_retiring = state.state == torrent::failed;
    }
    if (state.state == torrent::paused || (job.pause_retiring && (state.state == torrent::removed || !state.id))) {
      std::lock_guard<std::mutex> lock(jobs_mutex); auto& current = jobs.at(id);
      if (current.want_paused && current.state != "paused") {
        if (current.pause_retiring) { current.engine = 0; current.pause_retiring = false; }
        current.state = "paused"; current.rate = 0; current.error.clear(); save(current);
      }
    }
    return;
  }
  if (state.state == torrent::failed) {
    if (job.retry_engine) {
      { std::lock_guard<std::mutex> lock(jobs_mutex); jobs.at(id).retry_engine = false; }
      torrent::resume(job.engine); return;
    }
    throw std::runtime_error(torrent::error(job.engine));
  }
  if (job.pause_sent || (state.state == torrent::paused && job.state != "completed")) {
    torrent::resume(job.engine);
    std::lock_guard<std::mutex> lock(jobs_mutex); auto& current = jobs.at(id);
    current.pause_sent = false; current.resume_pending = current.bgft >= 0;
    job.resume_pending = current.resume_pending;
  }
  const auto files = torrent::files(job.engine);
  if (files.empty()) {
    std::lock_guard<std::mutex> lock(jobs_mutex); auto& current = jobs.at(id);
    if (!current.want_paused && current.state != "completed") { current.state = "connecting"; current.status = "Waiting for torrent metadata and peers"; current.seeds = state.seeds; current.peers = state.peers; }
    return;
  }
  if (job.path.empty()) {
    const bool has_pkg=std::any_of(files.begin(),files.end(),[](const auto& file){return lower(fs::path(file.path).extension().string())==".pkg";});
    job.path=has_pkg?choose(files):choose_archive(job,files);
    if(!has_pkg)job.archive_source=job.path;
    for (const auto& file : files) if (file.path == job.path) job.size = static_cast<std::uint64_t>(file.size);
    if (!job.size || job.size > (1ULL << 50)) throw std::runtime_error("Invalid package size in metadata");
    if (job.bgft < 0) {
      struct statvfs disk{};
      if (::statvfs("/data", &disk) == 0) {
        const std::uint64_t block = disk.f_frsize ? disk.f_frsize : disk.f_bsize;
        const std::uint64_t free = block && disk.f_bavail <= UINT64_MAX / block ? disk.f_bavail * block : 0;
        if (free && free < 2 * job.size + 256 * 1024 * 1024) throw std::runtime_error("Internal storage needs room for the torrent cache and system installation (about twice the PKG size)");
      }
    }
    std::lock_guard<std::mutex> lock(jobs_mutex); auto& current = jobs.at(id); current.path = job.path; current.size = job.size; current.archive_source=job.archive_source;current.archive_bytes=job.archive_bytes=job.size;save(current);
  } else {
    const auto original_size=job.archive_source.empty()?job.size:job.archive_bytes;
    if (std::none_of(files.begin(), files.end(), [&](const torrent::File& file) { return file.path == job.path && static_cast<std::uint64_t>(file.size) == original_size; })) throw std::runtime_error("Saved payload no longer matches the verified torrent metadata");
  }
  if(!job.archive_source.empty()) {
    queue_archive(id);return;
  }
  std::string selection_error;
  if (!torrent::stream::select(job.engine, job.path, selection_error)) { if (!selection_error.empty()) throw std::runtime_error(selection_error); return; }
  if (!job.header_checked) {
    const bool checked = header(job);
    { std::lock_guard<std::mutex> lock(jobs_mutex); auto& current = jobs.at(id);
      current.header_bytes = job.header_bytes; current.rate = state.rate_down; current.seeds = state.seeds; current.peers = state.peers;
      if (checked) { current.header_checked = true; current.content = job.content; save(current); }
      else if (!current.want_paused) { current.state = "connecting"; current.status = "Fetching a hash-verified PS4 package header"; }
    }
    if (!checked) return;
  }
  install_package(job,state);
}

void register_loop() noexcept {
  for (;;) {
    int id;
    { std::unique_lock<std::mutex> lock(queues_mutex); queue_wake.wait(lock, [] { return !alive || !registration_queue.empty(); });
      if (!alive) return; id = registration_queue.front(); registration_queue.pop_front(); }
    try {
      Job job;
      { std::lock_guard<std::mutex> lock(jobs_mutex); auto& current = jobs.at(id);
        if (current.want_paused) { current.registration_pending = false; continue; }
        current.state = "registering"; current.registration_uncertain = true; current.status = "Registering a visible system download"; save(current); job = current;
      }
      const auto url = "http://127.0.0.1:28931/pkg/" + token + "/" + std::to_string(id) + ".pkg";
      const int task = bgft::registration(url, job.name);
      bool pause;
      { std::lock_guard<std::mutex> lock(jobs_mutex); auto& current = jobs.at(id);
        current.bgft = task; current.start_pending = true; current.ownership_verified = true; current.registration_uncertain = false;
        current.state = current.want_paused ? "pausing" : "registered"; save(current); pause = current.want_paused;
      }
      if (!pause) {
        const int rc = sceBgftServiceDownloadStartTask(task);
        if (rc) throw std::runtime_error(bgft::error("sceBgftServiceDownloadStartTask", rc));
      }
      { std::lock_guard<std::mutex> lock(jobs_mutex); auto& current = jobs.at(id);
        current.registration_pending = false;
        if (!pause) { current.start_pending = false; current.registered = true; }
        current.state = current.want_paused ? "pausing" : "installing"; save(current);
      }
      async_log::write("[resident] job %d accepted as visible BGFT task %d", id, task);
    } catch (const std::exception& error) {
      { std::lock_guard<std::mutex> lock(jobs_mutex); const auto found = jobs.find(id); if (found != jobs.end()) found->second.registration_pending = false; }
      fail(id, error.what());
    } catch (...) {
      { std::lock_guard<std::mutex> lock(jobs_mutex); const auto found = jobs.find(id); if (found != jobs.end()) found->second.registration_pending = false; }
      fail(id, "System package registration failed");
    }
    controller_wake.notify_all();
  }
}

void feed(int fd, const Request& request) {
  const std::string prefix = "/pkg/" + token + "/";
  if ((request.method != "GET" && request.method != "HEAD") || !request.path.starts_with(prefix) || !request.path.ends_with(".pkg")) { respond(fd, 404, "{}"); return; }
  const auto id_text = request.path.substr(prefix.size(), request.path.size() - prefix.size() - 4);
  const int id = static_cast<int>(number(id_text, INT_MAX)); Job job;
  {
    std::lock_guard<std::mutex> lock(jobs_mutex);
    if (!restore_locked(id)) { respond(fd, 404, "{}"); return; }
    auto& current = jobs.at(id); ++current.readers; current.last_use = Clock::now(); current.need_feeder = true;
  }
  struct Reading {
    int id;
    ~Reading() { std::lock_guard<std::mutex> lock(jobs_mutex); const auto found = jobs.find(id); if (found != jobs.end()) { --found->second.readers; found->second.last_use = Clock::now(); } }
  } reading{id};
  controller_wake.notify_all();
  const auto deadline = Clock::now() + std::chrono::seconds(60);
  for (;;) {
    if (!job_copy(id, job) || job.want_paused || job.state == "failed") { respond(fd, 503, "{}"); return; }
    std::string error;
    if (archived_package(job)||(job.engine && !job.retiring && torrent::stream::select(job.engine, job.path, error))) break;
    if (!error.empty() || Clock::now() >= deadline || !alive) { respond(fd, 503, "{}", "Retry-After: 2\r\n"); return; }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!job_copy(id, job) || !job.size || job.path.empty() || job.want_paused || job.state == "failed") { respond(fd, 503, "{}", "Retry-After: 2\r\n"); return; }
  OverdriveRange range{}; const int status = overdrive_range(request.range.data(), request.range.size(), job.size, &range);
  if (status == 416) { respond(fd, 416, "{}", "Content-Range: bytes */" + std::to_string(job.size) + "\r\n"); return; }
  std::string response = "HTTP/1.1 " + std::to_string(status) + (status == 206 ? " Partial Content\r\n" : " OK\r\n") +
    "Connection: close\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\nContent-Length: " + std::to_string(range.last - range.first + 1) + "\r\n";
  if (range.partial) response += "Content-Range: bytes " + std::to_string(range.first) + "-" + std::to_string(range.last) + "/" + std::to_string(job.size) + "\r\n";
  response += "\r\n";
  if (!send_text(fd, response) || request.method == "HEAD") return;
  std::vector<char> bytes; std::string error;
  Fd local(archived_package(job)?::open(job.path.c_str(),O_RDONLY|O_NOFOLLOW):-1);
  if(archived_package(job)&&local.value<0)return;
  for (std::uint64_t offset = range.first; alive && offset <= range.last;) {
    Job current; if (!job_copy(id, current) || current.want_paused || current.state == "failed") return;
    const auto wanted=static_cast<std::size_t>(std::min<std::uint64_t>(128*1024,range.last-offset+1));
    bool read_ok;
    if(local.value>=0){bytes.resize(wanted);const auto count=::pread(local.value,bytes.data(),wanted,static_cast<off_t>(offset));
      read_ok=count>0;if(read_ok)bytes.resize(static_cast<std::size_t>(count));}
    else read_ok=torrent::stream::read(current.engine,offset,wanted,bytes,error);
    if (!read_ok) {
      async_log::write("[resident] stream job %d interrupted: %s", id, error.c_str()); return;
    }
    if (bytes.empty() || !send_bytes(fd, bytes.data(), bytes.size())) return;
    offset += bytes.size();
  }
}
void control(int fd, const Request& request) {
  if (request.path == "/health" && request.method == "GET") {
    respond(fd, ready ? 200 : 503, ready ? "overdrive-resident/1\n" : "not ready\n"); return;
  }
  if (request.auth != token) { respond(fd, 403, "{\"error\":\"Resident authentication failed\"}"); return; }
  if (request.method == "POST" && request.path == "/jobs") {
    const auto fields = lines(request.body);
    if (fields.size() != 3 && fields.size()!=5) throw std::runtime_error("Expected magnet, name, size and optional archive metadata fields");
    const std::string uri = unhex(fields[0], 16384), name = unhex(fields[1], 512);
    const auto hint = number(fields[2], 9007199254740991ULL); std::string error;
    const auto password=fields.size()==5?unhex(fields[3],1024):std::string{};
    const auto archive_file=fields.size()==5?unhex(fields[4],4096):std::string{};
    if(!archive_file.empty()&&(!within_path(fs::path("/archive")/archive_file,"/archive")||fs::path(archive_file).is_absolute()))throw std::runtime_error("Invalid relative archiveFile metadata");
    const auto key = torrent::stream::key(uri, error); if (key.empty()) throw std::runtime_error(error);
    std::unique_lock<std::mutex> lock(jobs_mutex);
    const auto archived = history_keys.find(key);
    if (archived != history_keys.end()) (void)restore_locked(archived->second);
    for (auto& [id, job] : jobs) if (job.key == key) {
      if((job.password!=password||job.requested_archive!=archive_file)&&job.archive_pending)throw std::runtime_error("Archive metadata cannot change while a decoder owns this job");
      job.password=password;job.requested_archive=archive_file;
      if ((job.want_paused || job.state == "failed") && (job.bgft >= 0 || !job.registration_uncertain)) {
        job.retry_engine = job.state == "failed"; job.want_paused = false; job.resume_pending = job.bgft >= 0; job.pause_sent = false; job.error.clear(); job.state = "queued"; job.last_use = Clock::now(); save(job);
      }
      const auto body = snapshot(job); lock.unlock(); respond(fd, 200, body); controller_wake.notify_all(); return;
    }
    const auto pending = std::count_if(jobs.begin(), jobs.end(), [](const auto& entry) { return entry.second.state != "completed" && entry.second.state != "failed"; });
    if (pending >= 32 || jobs.size() >= 64 || next_id == INT_MAX) throw std::runtime_error("Resident queue is full (32 active jobs); completed records are archived automatically");
    Job job; job.id = next_id++; job.key = key; job.uri = uri; job.name = name.empty() ? "Overdrive PS4 package" : name; job.hint = hint;job.password=password;job.requested_archive=archive_file;
    save(job); auto& stored = jobs.emplace(job.id, std::move(job)).first->second;
    const auto body = snapshot(stored); lock.unlock(); respond(fd, 201, body); controller_wake.notify_all(); return;
  }
  if (!request.path.starts_with("/jobs/")) { respond(fd, 404, "{}"); return; }
  const auto part = request.path.substr(6); const auto slash = part.find('/');
  const int id = static_cast<int>(number(part.substr(0, slash), INT_MAX));
  std::unique_lock<std::mutex> lock(jobs_mutex); const auto found = jobs.find(id);
  if (found == jobs.end() && !restore_locked(id)) { lock.unlock(); respond(fd, 404, "{}"); return; }
  auto& job = jobs.at(id);
  if (request.method == "GET" && slash == part.npos) { const auto body = snapshot(job); lock.unlock(); respond(fd, 200, body); return; }
  if (request.method != "POST" || !request.body.empty() || slash == part.npos) throw std::runtime_error("Invalid resident control request");
  const auto action = part.substr(slash + 1);
  if (action == "pause") {
    if (job.registration_uncertain && job.bgft < 0 && !job.registration_pending) throw std::runtime_error("The interrupted registration has no confirmed BGFT ID; inspect system Downloads to pause it");
    job.want_paused = true; job.error.clear();
    if (!job.engine && job.bgft < 0) job.state = "paused";
    else job.state = "pausing";
    save(job); const int engine = job.engine; const auto body = snapshot(job); lock.unlock();
    if (engine) torrent::stream::suspend(engine); // Interrupt a waiting header read immediately.
    controller_wake.notify_all(); respond(fd, 200, body); return;
  }
  if (action == "resume") {
    if (job.registration_uncertain && job.bgft < 0) throw std::runtime_error("Interrupted registration requires inspection in system Downloads before retry");
    job.want_paused = false; job.resume_pending = job.bgft >= 0; job.error.clear(); if (job.state != "completed") job.state = "queued";
    save(job); const auto body = snapshot(job); lock.unlock(); controller_wake.notify_all(); respond(fd, 200, body); return;
  }
  respond(fd, 404, "{}");
}
void control_worker() noexcept {
  for (;;) {
    int fd;
    { std::unique_lock<std::mutex> lock(queues_mutex); queue_wake.wait(lock, [] { return !alive || !control_queue.empty(); });
      if (!alive && control_queue.empty()) return; fd = control_queue.front(); control_queue.pop_front(); }
    bool handed = false;
    try {
      auto request = receive(fd);
      if (request.path.starts_with("/pkg/")) {
        std::lock_guard<std::mutex> lock(queues_mutex);
        if (feed_queue.size() < 8 && alive) { feed_queue.push_back({fd, std::move(request)}); handed = true; queue_wake.notify_all(); }
        else respond(fd, 503, "{}", "Retry-After: 2\r\n");
      } else control(fd, request);
    } catch (const std::exception& error) { respond(fd, 400, "{\"error\":" + json(error.what()) + '}'); }
    catch (...) { respond(fd, 503, "{}"); }
    if (!handed) close_socket(fd);
  }
}
void feed_worker() noexcept {
  for (;;) {
    Feed item;
    { std::unique_lock<std::mutex> lock(queues_mutex); queue_wake.wait(lock, [] { return !alive || !feed_queue.empty(); });
      if (!alive && feed_queue.empty()) return; item = std::move(feed_queue.front()); feed_queue.pop_front(); }
    try { feed(item.socket, item.request); }
    catch (...) { /* A partial response is ended by closing, never replaced by fake bytes. */ }
    close_socket(item.socket);
  }
}
void listen_loop() noexcept {
  while (alive) {
    if (!socket_wait(listener, false, Clock::now() + std::chrono::milliseconds(250))) continue;
    sockaddr_in peer{}; socklen_t length = sizeof peer;
    const int fd = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &length);
    if (fd < 0) continue;
    if (fd >= FD_SETSIZE || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || ::fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { ::close(fd); continue; }
    { std::lock_guard<std::mutex> lock(sockets_mutex); sockets.insert(fd); }
    { std::lock_guard<std::mutex> lock(queues_mutex); if (control_queue.size() < 16 && alive) control_queue.push_back(fd); else { close_socket(fd); continue; } }
    queue_wake.notify_all();
  }
}
void controller() noexcept {
  while (alive) {
    std::vector<int> ids;
    { std::lock_guard<std::mutex> lock(jobs_mutex); for (const auto& [id, job] : jobs) ids.push_back(id); }
    for (int id : ids) {
      if (!alive) break;
      try { tick(id); }
      catch (const std::exception& error) { fail(id, error.what()); }
      catch (...) { fail(id, "Resident worker failed"); }
    }
    try { housekeeping(); } catch (const std::exception& error) { async_log::write("[resident] checkpoint housekeeping: %s", error.what()); }
    std::unique_lock<std::mutex> lock(queues_mutex); controller_wake.wait_for(lock, std::chrono::milliseconds(500), [] { return !alive; });
  }
}
} // namespace

bool start() {
  if (alive.exchange(true)) return ready;
  try {
    directory("/data/overdrive"); directory(base); directory(job_dir); directory(archive_dir);directory(std::string(base)+"/archives");
    lease = ::open((std::string(base) + "/service.lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (lease < 0 || ::flock(lease, LOCK_EX | LOCK_NB)) throw std::runtime_error("Cannot acquire resident service lock");
    // Listener bind is the process-wide ownership proof; never attach to another service's port.
    auth(); load();
    if (!network::start() || !torrent::start("/data")) throw std::runtime_error("Cannot initialize resident torrent networking");
    listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0 || listener >= FD_SETSIZE) throw std::runtime_error("Cannot create resident loopback listener");
    int reuse = 1; (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(28931); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef __Orbis__
    address.sin_len = sizeof address;
#endif
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) || ::listen(listener, 16) || ::fcntl(listener, F_SETFL, O_NONBLOCK) < 0) throw std::runtime_error("Resident loopback port 28931 is unavailable");
    for (auto& thread : controls) thread = std::thread(control_worker);
    for (auto& thread : feeders) thread = std::thread(feed_worker);
    archive_thread=std::thread(archive_loop);listener_thread = std::thread(listen_loop); registration_thread = std::thread(register_loop); controller_thread = std::thread(controller);
    ready = true; async_log::write("[resident] loopback service ready; hardware validation pending"); return true;
  } catch (const std::exception& error) { async_log::write("[resident] startup failed: %s", error.what()); return false; }
}
void stop() {
  ready = false; alive = false;
  std::vector<int> engines;
  { std::lock_guard<std::mutex> lock(jobs_mutex); for (const auto& [id, job] : jobs) if (job.engine) engines.push_back(job.engine); }
  for (int engine : engines) torrent::stream::suspend(engine);
  if (listener >= 0) ::shutdown(listener, SHUT_RDWR);
  { std::lock_guard<std::mutex> lock(sockets_mutex); for (int fd : sockets) ::shutdown(fd, SHUT_RDWR); }
  queue_wake.notify_all(); controller_wake.notify_all();
  if (listener_thread.joinable()) listener_thread.join();
  for (auto& thread : controls) if (thread.joinable()) thread.join();
  for (auto& thread : feeders) if (thread.joinable()) thread.join();
  if (controller_thread.joinable()) controller_thread.join();
  if (registration_thread.joinable()) registration_thread.join();
  if(archive_thread.joinable())archive_thread.join();
  archives::stop();torrent::stop(); bgft::stop(); network::stop();
  if (listener >= 0) ::close(listener); listener = -1;
  if (lease >= 0) ::close(lease); lease = -1;
  std::lock_guard<std::mutex> lock(jobs_mutex); jobs.clear(); history_keys.clear(); history_paths.clear();
}
} // namespace resident_service
