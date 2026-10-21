// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "torrent_api.hpp"
#include <libtorrent/session.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

namespace torrent {
namespace {
namespace lt = libtorrent;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
struct Entry { Status status; std::string error; std::vector<File> files; std::string uri, root, destination; };
enum class Op { add, pause, resume, remove };
struct Command { Op op; int id; bool delete_files = false; };
struct Job {
  int id;
  lt::torrent_handle handle;
  std::string resume_path, destination;
  bool finishing = false, pausing = false, saving = false, retire = false, delete_files = false;
  bool paused_ack = false, flush_save = false, saved_after_pause = false, removing = false;
  bool metadata_checked = false;
  Clock::time_point saved = Clock::now();
};
std::mutex mutex;
std::condition_variable wake;
std::map<int, Entry> entries;
std::deque<Command> commands;
std::thread worker;
std::string root, latest_error;
bool running = false, stopping = false;
int next_id = 1;

void fail(int id, const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex);
  latest_error = message;
  const auto found = entries.find(id);
  if (found != entries.end()) {
    found->second.status.state = failed;
    found->second.status.rate_down = found->second.status.rate_up = 0;
    found->second.error = message;
  }
}
std::string hash_key(const lt::info_hash_t& hashes) {
  const std::string bytes = hashes.has_v1() ? hashes.v1.to_string() : hashes.v2.to_string();
  std::string out = hashes.has_v1() ? "v1-" : "v2-";
  for (unsigned char byte : bytes) { out += "0123456789abcdef"[byte >> 4]; out += "0123456789abcdef"[byte & 15]; }
  return out;
}
void store_resume(const std::string& path, const std::vector<char>& bytes) {
  const std::string temp = path + ".tmp";
  int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) throw std::runtime_error("Cannot create torrent resume file: " + temp);
  bool ok = true;
  for (std::size_t n = 0; n < bytes.size();) {
    const auto written = ::write(fd, bytes.data() + n, bytes.size() - n);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) { ok = false; break; }
    n += static_cast<std::size_t>(written);
  }
  if (ok) ok = ::fsync(fd) == 0;
  if (::close(fd) != 0) ok = false;
  if (!ok || ::rename(temp.c_str(), path.c_str()) != 0)
    throw std::runtime_error("Cannot commit torrent resume file: " + path);
}
void settled(Job& job);
void save(Job& job, bool flush) {
  if (job.saving || (flush && !job.paused_ack)) return;
  if (!job.metadata_checked) { if (job.paused_ack) { job.saved_after_pause = true; settled(job); } return; }
  auto flags = lt::torrent_handle::save_info_dict;
  if (flush) flags |= lt::torrent_handle::flush_disk_cache;
  else flags |= lt::torrent_handle::only_if_modified;
  job.handle.save_resume_data(flags);
  job.saving = true;
  job.flush_save = flush;
  job.saved = Clock::now();
}
void settled(Job& job) {
  const auto status = job.handle.status();
  std::lock_guard<std::mutex> lock(mutex);
  auto& entry = entries.at(job.id);
  if (entry.status.state == failed) return;
  entry.status.state = job.finishing ? completed : paused;
  if (status.has_metadata) { entry.status.total = status.total_wanted; entry.status.done = status.total_wanted_done; }
  entry.status.rate_down = entry.status.rate_up = 0;
  if (job.finishing) { entry.status.progress = 1; entry.status.done = entry.status.total; }
}
void inspect(Job& job) {
  const auto info = job.handle.torrent_file();
  if (!info) return;
  const auto& storage = info->files();
  if (storage.num_files() > 8192) throw std::runtime_error("Torrent has more than 8192 files.");
  std::vector<File> files;
  const fs::path base = fs::path(job.destination).lexically_normal();
  for (auto index : storage.file_range()) {
    const auto flags = storage.file_flags(index);
    if (flags & lt::file_storage::flag_pad_file) continue;
    const fs::path relative(storage.file_path(index));
    if (relative.empty() || relative.is_absolute() || (flags & lt::file_storage::flag_symlink))
      throw std::runtime_error("Torrent has an unsafe file or symlink.");
    for (const auto& part : relative)
      if (part == "..") throw std::runtime_error("Torrent file escapes its download directory.");
    // Existing symlinks on a reused save path may not redirect writes outside this job.
    fs::path current = base;
    for (const auto& part : relative) {
      current /= part;
      std::error_code ec;
      if (fs::is_symlink(fs::symlink_status(current, ec)))
        throw std::runtime_error("Torrent destination contains a symlink.");
    }
    files.push_back({(base / relative).string(), storage.file_size(index)});
  }
  if (files.empty() || storage.total_size() <= 0) throw std::runtime_error("Torrent contains no payload files.");
  {
    std::lock_guard<std::mutex> lock(mutex);
    entries.at(job.id).files = std::move(files);
    job.metadata_checked = true;
  }
  job.handle.unset_flags(lt::torrent_flags::upload_mode);
}
void run() noexcept {
  std::map<int, Job> jobs;
  try {
    lt::settings_pack settings;
    settings.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:6881");
    settings.set_str(lt::settings_pack::outgoing_interfaces, "0.0.0.0");
    settings.set_int(lt::settings_pack::max_retry_port_bind, 8);
    settings.set_bool(lt::settings_pack::listen_system_port_fallback, false);
    settings.set_int(lt::settings_pack::max_metadata_size, 10 * 1024 * 1024);
    settings.set_int(lt::settings_pack::connections_limit, 64);
    settings.set_int(lt::settings_pack::send_buffer_watermark, 256 * 1024);
    settings.set_int(lt::settings_pack::aio_threads, 2);
    settings.set_int(lt::settings_pack::hashing_threads, 1);
    settings.set_int(lt::settings_pack::max_queued_disk_bytes, 4 * 1024 * 1024);
    settings.set_int(lt::settings_pack::alert_queue_size, 512);
    settings.set_bool(lt::settings_pack::enable_upnp, false);
    settings.set_bool(lt::settings_pack::enable_natpmp, false);
    settings.set_int(lt::settings_pack::alert_mask,
      lt::alert_category::error | lt::alert_category::status | lt::alert_category::storage);
    lt::session session(settings);
    bool exiting = false;
    Clock::time_point deadline;
    for (;;) {
      std::deque<Command> pending;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return (!exiting && stopping) || !commands.empty(); });
        pending.swap(commands);
        if (stopping && !exiting) { exiting = true; deadline = Clock::now() + std::chrono::seconds(5); }
      }
      for (const auto& command : pending) {
        try {
          if (command.op == Op::add || (command.op == Op::resume && !jobs.count(command.id))) {
            Entry entry;
            { std::lock_guard<std::mutex> lock(mutex); entry = entries.at(command.id); }
            lt::error_code ec;
            auto params = lt::parse_magnet_uri(entry.uri, ec);
            if (ec) throw std::runtime_error(ec.message());
            const auto hashes = params.info_hashes;
            const std::string key = hash_key(hashes);
            const fs::path parent = fs::path(entry.root) / "downloads/torrents";
            const fs::path destination = parent / key;
            const fs::path resume_dir = parent / ".resume";
            fs::path ancestor;
            for (const auto& part : destination) {
              ancestor /= part;
              std::error_code link_error;
              if (fs::is_symlink(fs::symlink_status(ancestor, link_error)))
                throw std::runtime_error("Torrent save directory contains a symlink.");
            }
            fs::create_directories(destination);
            fs::create_directories(resume_dir);
            if (fs::is_symlink(fs::symlink_status(parent)) || fs::is_symlink(fs::symlink_status(destination)) ||
                fs::is_symlink(fs::symlink_status(resume_dir)))
              throw std::runtime_error("Torrent save directory must not be a symlink.");
            const std::string resume_path = (resume_dir / (key + ".resume")).string();
            if (fs::exists(resume_path)) {
              const auto size = fs::file_size(resume_path);
              if (size > 16 * 1024 * 1024) throw std::runtime_error("Torrent resume file is too large.");
              std::ifstream input(resume_path, std::ios::binary);
              std::vector<char> data(static_cast<std::size_t>(size));
              if (!input.read(data.data(), static_cast<std::streamsize>(data.size())))
                throw std::runtime_error("Cannot read torrent resume file.");
              auto restored = lt::read_resume_data(data, ec);
              if (ec || (hashes.has_v1() && (!restored.info_hashes.has_v1() || restored.info_hashes.v1 != hashes.v1)) ||
                  (hashes.has_v2() && (!restored.info_hashes.has_v2() || restored.info_hashes.v2 != hashes.v2)))
                throw std::runtime_error("Invalid torrent resume file; remove it to recheck existing data.");
              // The current magnet may contain a refreshed tracker list.
              restored.trackers = params.trackers;
              restored.tracker_tiers = params.tracker_tiers;
              params = std::move(restored);
            }
            params.save_path = destination.string();
            params.flags &= ~lt::torrent_flags::auto_managed;
            params.flags &= ~lt::torrent_flags::paused;
            // Metadata is allowed, payload writes wait for our path/symlink inspection.
            params.flags |= lt::torrent_flags::upload_mode;
            params.flags |= lt::torrent_flags::duplicate_is_error;
            params.renamed_files.clear();
            params.file_priorities.clear();
            params.piece_priorities.clear();
            auto handle = session.add_torrent(params, ec);
            if (ec) throw std::runtime_error(ec.message());
            jobs.emplace(command.id, Job{command.id, handle, resume_path, destination.string()});
            { std::lock_guard<std::mutex> lock(mutex); entries.at(command.id).status.state = queued;
              entries.at(command.id).error.clear(); entries.at(command.id).destination = destination.string(); }
          } else {
            const auto found = jobs.find(command.id);
            if (found == jobs.end()) continue;
            Job& job = found->second;
            if (command.op == Op::resume) {
              job.pausing = job.finishing = false;
              job.paused_ack = job.saved_after_pause = false;
              { std::lock_guard<std::mutex> lock(mutex); entries.at(job.id).status.state = queued; entries.at(job.id).error.clear(); }
              job.handle.resume();
            } else {
              job.pausing = true;
              if (command.op == Op::remove) { job.retire = true; job.delete_files = command.delete_files; }
              job.handle.pause();
              // Only the paused alert acknowledges closed disk handles; the paused flag alone does not.
              if (job.paused_ack) save(job, true);
            }
          }
        } catch (const std::exception& error) { fail(command.id, error.what()); }
      }
      std::vector<lt::alert*> alerts;
      std::vector<int> erase;
      session.pop_alerts(&alerts);
      for (const auto* alert : alerts) {
        const auto* torrent_alert = dynamic_cast<const lt::torrent_alert*>(alert);
        if (!torrent_alert) {
          if (const auto* listen = lt::alert_cast<lt::listen_failed_alert>(alert)) {
            std::lock_guard<std::mutex> lock(mutex); latest_error = listen->message();
          }
          continue;
        }
        const auto found = std::find_if(jobs.begin(), jobs.end(), [&](const auto& item) { return item.second.handle == torrent_alert->handle; });
        if (found == jobs.end()) continue;
        Job& job = found->second;
        try {
          if (const auto* saved = lt::alert_cast<lt::save_resume_data_alert>(alert)) {
            store_resume(job.resume_path, lt::write_resume_data_buf(saved->params));
            job.saving = false;
            if (job.flush_save && job.paused_ack && (job.pausing || job.finishing || exiting)) {
              job.saved_after_pause = true;
              settled(job);
            }
          } else if (const auto* failure = lt::alert_cast<lt::save_resume_data_failed_alert>(alert)) {
            job.saving = false;
            if (failure->error != lt::errors::resume_data_not_modified) fail(job.id, "Resume save: " + failure->error.message());
          } else if (lt::alert_cast<lt::torrent_paused_alert>(alert)) {
            if (job.handle.status().flags & lt::torrent_flags::paused) { job.paused_ack = true; save(job, true); }
          } else if (lt::alert_cast<lt::torrent_deleted_alert>(alert) ||
                     (lt::alert_cast<lt::torrent_removed_alert>(alert) && !job.delete_files)) {
            if (job.delete_files) fs::remove(job.resume_path);
            { std::lock_guard<std::mutex> lock(mutex); entries.at(job.id).status.state = removed; }
            erase.push_back(job.id);
          } else if (const auto* failure = lt::alert_cast<lt::torrent_delete_failed_alert>(alert)) {
            fail(job.id, "Torrent file removal: " + failure->error.message());
            erase.push_back(job.id);
          } else if (const auto* failure = lt::alert_cast<lt::torrent_error_alert>(alert)) {
            fail(job.id, failure->error.message());
            job.handle.pause();
          } else if (lt::alert_cast<lt::metadata_received_alert>(alert)) {
            inspect(job);
          }
        } catch (const std::exception& error) { fail(job.id, error.what()); if (job.handle.is_valid()) job.handle.pause(); }
      }
      for (int id : erase) jobs.erase(id);
      for (auto& item : jobs) {
        Job& job = item.second;
        if (job.removing) continue;
        try {
          const auto status = job.handle.status();
          if (status.has_metadata && !job.metadata_checked) inspect(job);
          if (exiting && !job.pausing) { job.pausing = true; job.handle.pause(); save(job, true); }
          if (status.is_seeding && !job.finishing) { job.finishing = job.pausing = true; job.handle.pause(); }
          if (job.paused_ack && (job.pausing || job.finishing) && !job.saved_after_pause && !job.saving) save(job, true);
          if (job.retire && job.paused_ack && job.saved_after_pause && !job.saving) {
            session.remove_torrent(job.handle, job.delete_files ? lt::session::delete_files : lt::remove_flags_t{});
            job.removing = true;
          }
          if (!job.pausing && !job.saving && Clock::now() - job.saved > std::chrono::seconds(30)) save(job, false);
          std::lock_guard<std::mutex> lock(mutex);
          auto& snapshot = entries.at(job.id).status;
          if (snapshot.state == completed || snapshot.state == failed || snapshot.state == paused) continue;
          snapshot.state = !status.has_metadata ? metadata :
            status.state == lt::torrent_status::checking_files || status.state == lt::torrent_status::checking_resume_data || job.finishing
              ? checking : downloading;
          snapshot.progress = status.progress;
          snapshot.total = status.has_metadata ? status.total_wanted : snapshot.total;
          snapshot.done = status.total_wanted_done;
          snapshot.rate_down = static_cast<unsigned>(std::max(0, status.download_payload_rate));
          snapshot.rate_up = static_cast<unsigned>(std::max(0, status.upload_payload_rate));
          snapshot.seeds = status.num_seeds; snapshot.peers = status.num_peers;
        } catch (const std::exception& error) { fail(job.id, error.what()); }
      }
      if (exiting && (Clock::now() >= deadline || std::all_of(jobs.begin(), jobs.end(), [](const auto& item) {
        return item.second.paused_ack && item.second.saved_after_pause && !item.second.saving;
      }))) break;
    }
  } catch (const std::exception& error) {
    std::lock_guard<std::mutex> lock(mutex);
    latest_error = error.what();
    for (auto& item : entries) if (item.second.status.state != completed && item.second.status.state != removed) {
      item.second.status.state = failed; item.second.error = latest_error;
    }
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex); latest_error = "Torrent worker failed with an unknown exception.";
    for (auto& item : entries) { item.second.status.state = failed; item.second.error = latest_error; }
  }
  std::lock_guard<std::mutex> lock(mutex); running = false;
}
void command(Op op, int id, bool delete_files = false) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!running || !entries.count(id)) { latest_error = "Unknown torrent task or stopped session."; return; }
  if (commands.size() >= 128) { latest_error = "Torrent command queue is full."; return; }
  commands.push_back({op, id, delete_files}); wake.notify_one();
}
} // namespace

bool start(const char* save_root) {
  if (!save_root || save_root[0] != '/' || std::strlen(save_root) > 2048 || std::strchr(save_root, '\\')) {
    std::lock_guard<std::mutex> lock(mutex); latest_error = "Expected an absolute torrent save root."; return false;
  }
  const fs::path path(save_root);
  for (const auto& part : path) if (part == "..") { std::lock_guard<std::mutex> lock(mutex); latest_error = "Torrent save root contains '..'."; return false; }
  std::unique_lock<std::mutex> lock(mutex);
  if (stopping) { latest_error = "Torrent session is stopping."; return false; }
  if (worker.joinable() && !running) {
    // A failed startup has already destroyed its session; joining this finished worker does no I/O.
    std::thread previous = std::move(worker);
    lock.unlock(); previous.join(); lock.lock();
  }
  root = path.lexically_normal().string();
  if (running) return true;
  try { running = true; worker = std::thread(run); return true; }
  catch (const std::exception& error) { running = false; latest_error = error.what(); return false; }
}
void stop() {
  { std::lock_guard<std::mutex> lock(mutex); stopping = true; wake.notify_one(); }
  if (worker.joinable()) worker.join();
  std::lock_guard<std::mutex> lock(mutex);
  entries.clear(); commands.clear(); stopping = running = false; root.clear();
}
int add_magnet(const char* uri, const char* name, long long size_hint) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!running || stopping) { latest_error = "Torrent session is not running."; return 0; }
  if (!uri || std::strncmp(uri, "magnet:?", 8) || std::strlen(uri) > 16384 || size_hint < 0 || next_id == INT_MAX) {
    latest_error = "Invalid magnet URI or size hint."; return 0;
  }
  for (auto& item : entries) if (item.second.uri == uri && item.second.root == root && item.second.status.state != removed) {
    if (item.second.status.state == paused || item.second.status.state == failed) {
      if (commands.size() >= 128) { latest_error = "Torrent command queue is full."; return 0; }
      commands.push_back({Op::resume, item.first}); wake.notify_one();
    }
    return item.first;
  }
  for (auto it = entries.begin(); it != entries.end();) {
    if (it->second.status.state == removed) it = entries.erase(it); else ++it;
  }
  if (entries.size() >= 32 || commands.size() >= 128) { latest_error = "Torrent queue is full (32 jobs)."; return 0; }
  const int id = next_id++;
  Entry entry; entry.uri = uri; entry.root = root; entry.status.id = id; entry.status.total = size_hint;
  std::snprintf(entry.status.name, sizeof entry.status.name, "%s", name ? name : "Torrent");
  entries.emplace(id, std::move(entry)); commands.push_back({Op::add, id}); wake.notify_one(); return id;
}
int poll(Status* out, int max) {
  if (!out || max <= 0) return 0;
  std::lock_guard<std::mutex> lock(mutex); int count = 0;
  for (const auto& item : entries) { if (count == max) break; out[count++] = item.second.status; }
  return count;
}
bool finished(int id) { std::lock_guard<std::mutex> lock(mutex); const auto it = entries.find(id); return it != entries.end() && it->second.status.state == completed; }
void pause(int id) { command(Op::pause, id); }
void resume(int id) { command(Op::resume, id); }
void remove(int id, bool delete_files) { command(Op::remove, id, delete_files); }
const char* last_error() { thread_local std::string text; std::lock_guard<std::mutex> lock(mutex); text = latest_error; return text.c_str(); }
std::string error(int id) { std::lock_guard<std::mutex> lock(mutex); const auto it = entries.find(id); return it == entries.end() ? "Unknown torrent task." : it->second.error; }
std::vector<File> files(int id) { std::lock_guard<std::mutex> lock(mutex); const auto it = entries.find(id); return it == entries.end() ? std::vector<File>{} : it->second.files; }
std::string destination(int id) { std::lock_guard<std::mutex> lock(mutex); const auto it = entries.find(id); return it == entries.end() ? std::string{} : it->second.destination; }
} // namespace torrent
