// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Linux host regression/soak harness for the exact shared native source.
#include "torrent_api.hpp"
#include <libtorrent/session.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/peer_class.hpp>
#include <openssl/sha.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
#include <sys/stat.h>

namespace lt = libtorrent;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
std::atomic<long long> heartbeat{0}, calls{0}, longest_call_us{0};
std::atomic<unsigned> starts{0}, ready{0}, stops{0}, accepted{0}, expected_failures{0};
std::atomic<bool> done{false}, wrong_thread{false};
std::thread::id main_thread;
std::mutex log_mutex;
std::deque<std::string> recent_logs;
unsigned checks = 0, transfers = 0, cycles = 0;

long long now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
void beat() { heartbeat = now_ms(); }
void check(bool value, const std::string& message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
void log_native(const char* line) {
  if (std::strstr(line, "session starting worker")) { ++starts; if (std::this_thread::get_id() == main_thread) wrong_thread = true; }
  if (std::strstr(line, "session ready worker")) ++ready;
  if (std::strstr(line, "session stopped worker")) ++stops;
  if (std::strstr(line, "add_magnet accepted worker")) ++accepted;
  if (std::strstr(line, "task failed id=")) ++expected_failures;
  std::lock_guard<std::mutex> lock(log_mutex);
  recent_logs.emplace_back(line);
  if (recent_logs.size() > 128) recent_logs.pop_front();
}
void elapsed_call(Clock::time_point from) {
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - from).count();
  auto old = longest_call_us.load();
  while (us > old && !longest_call_us.compare_exchange_weak(old, us)) {}
  ++calls;
}
template<class F> auto api(F fn) {
  const auto from = Clock::now();
  if constexpr (std::is_void_v<decltype(fn())>) { fn(); elapsed_call(from); }
  else { auto value = fn(); elapsed_call(from); return value; }
}
void wait_for(const std::function<bool()>& condition, const char* label, std::chrono::seconds timeout = 30s) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    beat();
    if (condition()) return;
    std::this_thread::sleep_for(20ms);
  }
  throw std::runtime_error(std::string("Timeout: ") + label + "; last native error: " + torrent::last_error());
}
torrent::Status status(int id) {
  torrent::Status out[32];
  const int count = api([&] { return torrent::poll(out, 32); });
  for (int i = 0; i < count; ++i) if (out[i].id == id) return out[i];
  throw std::runtime_error("Task is absent from poll: " + std::to_string(id));
}
void state(int id, int value, const char* label, std::chrono::seconds timeout = 30s) {
  wait_for([&] {
    const auto current = status(id).state;
    if (current == torrent::failed && value != torrent::failed && value != torrent::removed)
      throw std::runtime_error(torrent::error(id));
    return current == value;
  }, label, timeout);
}
void start(const fs::path& root) {
  const auto previous = ready.load();
  check(api([&] { return torrent::start(root.c_str()); }), "start refused");
  wait_for([&] { return ready.load() > previous; }, "worker startup");
  check(!wrong_thread, "session construction ran on the submitter thread");
}
void stop() {
  beat();
  const auto from = Clock::now();
  torrent::stop();
  check(Clock::now() - from < 12s, "worker shutdown exceeded 12 seconds");
  torrent::Status out[32];
  check(torrent::poll(out, 32) == 0, "stop retained tasks");
}
std::string hash(const std::string& bytes) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
  std::string result;
  for (unsigned char byte : digest) { result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15]; }
  return result;
}
std::string read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  check(bool(file), "missing payload: " + path.string());
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::string unknown(unsigned number) {
  char uri[96]; std::snprintf(uri, sizeof uri, "magnet:?xt=urn:btih:%040x", number);
  return uri;
}
int add(const std::string& uri, const fs::path& root) {
  const int id = api([&] { return torrent::add_magnet(uri.c_str(), "Synthetic loopback fixture", 262144, root.c_str()); });
  check(id > 0, std::string("submission refused: ") + torrent::last_error());
  return id;
}
void remove(int id, bool files) { api([&] { torrent::remove(id, files); }); state(id, torrent::removed, "task removal"); }
long long rss_kb() {
  std::ifstream file("/proc/self/status"); std::string line;
  while (std::getline(file, line)) if (line.rfind("VmRSS:", 0) == 0) return std::stoll(line.substr(6));
  return -1;
}
unsigned descriptors() {
  unsigned result = 0;
  for (const auto& item : fs::directory_iterator("/proc/self/fd")) { (void)item; ++result; }
  return result;
}
void metrics(double seconds, bool final = false) {
  std::cout << (final ? "RESULT " : "METRICS ") << "{\"elapsed_seconds\":" << seconds
    << ",\"checks\":" << checks << ",\"transfers\":" << transfers << ",\"cycles\":" << cycles
    << ",\"api_calls\":" << calls << ",\"max_api_call_us\":" << longest_call_us
    << ",\"session_starts\":" << starts << ",\"session_ready\":" << ready << ",\"session_stops\":" << stops
    << ",\"accepted\":" << accepted << ",\"expected_task_errors\":" << expected_failures
    << ",\"rss_kb\":" << rss_kb() << ",\"open_descriptors\":" << descriptors() << "}" << std::endl;
}
void concurrency(const fs::path& root) {
  std::mutex ids_mutex; std::set<int> ids;
  std::vector<std::thread> producers;
  for (int thread = 0; thread < 4; ++thread) producers.emplace_back([&, thread] {
    for (unsigned i = 0; i < 200; ++i) {
      const auto uri = unknown(10000 + unsigned(thread));
      const int id = api([&] { return torrent::add_magnet(uri.c_str(), "Concurrent fixture", 1, root.c_str()); });
      if (id > 0) {
        { std::lock_guard<std::mutex> lock(ids_mutex); ids.insert(id); }
        if (i % 20 == 0) api([&] { torrent::pause(id); });
        else if (i % 20 == 1) api([&] { torrent::resume(id); });
      }
      torrent::Status out[4]; api([&] { return torrent::poll(out, 4); });
    }
  });
  for (auto& producer : producers) producer.join();
  check(ids.size() == 4, "concurrent identical submissions produced duplicate tasks");
  // Allow bounded queue pressure to drain before submitting removals.
  wait_for([&] { beat(); std::this_thread::sleep_for(100ms); return true; }, "queue drain");
  for (int id : ids) remove(id, false);
}
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const int minutes = std::stoi(argv[1]);
  if (minutes < 0 || minutes > 120) return 2;
  const fs::path sandbox = fs::absolute(argv[2]);
  fs::create_directories(sandbox);
  main_thread = std::this_thread::get_id(); beat(); torrent::set_logger(log_native);
  std::thread watchdog([] {
    while (!done) {
      std::this_thread::sleep_for(1s);
      if (now_ms() - heartbeat > 45'000) { std::fprintf(stderr, "FAIL native API/worker stalled for 45 seconds\n"); std::_Exit(3); }
    }
  });
  int result = 0;
  try {
    check(!torrent::start("relative"), "relative root accepted");
    check(!torrent::start("/tmp/../escape"), "parent traversal accepted");
    check(!torrent::add_magnet("magnet:?", "before start", 0, sandbox.c_str()), "submit before startup accepted");
    const auto seed_path = sandbox / "seed";
    fs::create_directories(seed_path);
    std::string payload(262144, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = char((i * 37 + i / 251) % 256);
    { std::ofstream file(seed_path / "fixture.bin", std::ios::binary); file.write(payload.data(), payload.size()); }
    lt::file_storage storage; storage.add_file("fixture.bin", payload.size());
    lt::create_torrent creator(storage, 16384, lt::create_torrent::v1_only);
    lt::set_piece_hashes(creator, seed_path.string());
    std::vector<char> metadata; lt::bencode(std::back_inserter(metadata), creator.generate());
    auto info = std::make_shared<lt::torrent_info>(lt::span<char const>(metadata.data(), metadata.size()), lt::from_span);
    std::string key = "v1-";
    for (unsigned char byte : info->info_hashes().v1.to_string()) { key += "0123456789abcdef"[byte >> 4]; key += "0123456789abcdef"[byte & 15]; }
    lt::settings_pack settings;
    settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
    settings.set_str(lt::settings_pack::outgoing_interfaces, "127.0.0.1");
    settings.set_str(lt::settings_pack::dht_bootstrap_nodes, "");
    settings.set_bool(lt::settings_pack::enable_dht, false); settings.set_bool(lt::settings_pack::enable_lsd, false);
    settings.set_bool(lt::settings_pack::enable_upnp, false); settings.set_bool(lt::settings_pack::enable_natpmp, false);
    settings.set_int(lt::settings_pack::upload_rate_limit, 65536);
    lt::session seeder(settings);
    // Local peers bypass the global rate class by default. Limit their own
    // class so the pause case cannot race a completed loopback transfer.
    auto local = seeder.get_peer_class(lt::session::local_peer_class_id);
    local.upload_limit = 65536;
    seeder.set_peer_class(lt::session::local_peer_class_id, local);
    wait_for([&] { return seeder.listen_port() != 0; }, "loopback seed listener");
    lt::add_torrent_params params; params.ti = info; params.save_path = seed_path.string();
    params.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed); params.flags |= lt::torrent_flags::seed_mode;
    seeder.add_torrent(params);
    const auto magnet = lt::make_magnet_uri(*info) + "&x.pe=127.0.0.1:" + std::to_string(seeder.listen_port());
    const auto root = sandbox / "app";
    start(root); check(api([&] { return torrent::start(); }), "idempotent start failed");
    check(!api([&] { return torrent::add_magnet("https://invalid", "bad", 0); }), "non-magnet accepted");
    check(!api([&] { return torrent::add_magnet(magnet.c_str(), "bad", -1); }), "negative size accepted");
    const int bad = add("magnet:?xt=urn:btih:invalid", root); state(bad, torrent::failed, "invalid magnet failure"); remove(bad, false);
    const int id = add(magnet, root);
    check(add(magnet, root) == id, "duplicate magnet not reused");
    wait_for([&] { return !torrent::files(id).empty(); }, "magnet metadata and path inspection");
    api([&] { torrent::pause(id); }); state(id, torrent::paused, "pause with flushed checkpoint");
    check(fs::exists(root / "downloads/torrents/.resume" / (key + ".resume")), "pause checkpoint absent");
    api([&] { torrent::resume(id); }); state(id, torrent::completed, "download completion", 60s);
    check(torrent::finished(id), "finished flag absent");
    check(hash(read(fs::path(torrent::destination(id)) / "fixture.bin")) == hash(payload), "downloaded payload differs"); ++transfers;
    const fs::path destination = torrent::destination(id);
    remove(id, false); check(fs::exists(destination / "fixture.bin"), "keep-files removal deleted payload");
    const int restored = add(magnet, root); state(restored, torrent::completed, "checkpoint restore", 60s);
    check(restored != id && hash(read(destination / "fixture.bin")) == hash(payload), "restore lost task identity or data");
    remove(restored, true); check(!fs::exists(destination / "fixture.bin"), "delete-files removal left payload");
    check(!fs::exists(root / "downloads/torrents/.resume" / (key + ".resume")), "delete left checkpoint");
    const auto second_root = sandbox / "other-app";
    const int other = add(magnet, second_root); state(other, torrent::completed, "second save root", 60s);
    check(fs::path(torrent::destination(other)).string().rfind(second_root.string(), 0) == 0, "save roots mixed");
    check(hash(read(fs::path(torrent::destination(other)) / "fixture.bin")) == hash(payload), "second payload differs"); ++transfers;
    remove(other, true);
    const auto corrupt = sandbox / "corrupt";
    fs::create_directories(corrupt / "downloads/torrents/.resume");
    { std::ofstream file(corrupt / "downloads/torrents/.resume" / (key + ".resume")); file << "invalid checkpoint"; }
    const int corrupt_id = add(magnet, corrupt); state(corrupt_id, torrent::failed, "corrupt resume rejected"); remove(corrupt_id, false);
    const auto protected_path = sandbox / "protected"; fs::create_directories(protected_path); ::chmod(protected_path.c_str(), 0500);
    const int denied = add(magnet, protected_path); state(denied, torrent::failed, "write error reported"); ::chmod(protected_path.c_str(), 0700); remove(denied, false);
    const auto target = sandbox / "symlink-target"; fs::create_directories(target); fs::create_directory_symlink(target, sandbox / "link");
    const int linked = add(magnet, sandbox / "link"); state(linked, torrent::failed, "symlink root rejected"); remove(linked, false);
    check(fs::is_empty(target), "symlink target was written");
    stop(); start(root);
    std::vector<int> capacity;
    for (unsigned n = 1; n <= 32; ++n) capacity.push_back(add(unknown(n), root));
    check(!api([&] { return torrent::add_magnet(unknown(33).c_str(), "over capacity", 1); }), "queue exceeded 32 jobs");
    for (int item : capacity) remove(item, false);
    concurrency(root);
    std::cout << "SMOKE cases passed; timed soak begins" << std::endl;
    const auto from = Clock::now(); auto report = from;
    const auto duration = std::chrono::minutes(minutes);
    do {
      beat();
      const int item = add(magnet, root);
      if (cycles % 2) {
        wait_for([&] { return !torrent::files(item).empty(); }, "soak metadata");
        api([&] { torrent::pause(item); }); state(item, torrent::paused, "soak pause");
        api([&] { torrent::resume(item); });
      }
      state(item, torrent::completed, "soak transfer", 60s);
      check(hash(read(fs::path(torrent::destination(item)) / "fixture.bin")) == hash(payload), "soak payload checksum differs");
      ++transfers; remove(item, true); ++cycles;
      const auto elapsed = Clock::now() - from;
      if (minutes && elapsed > duration * 2 / 3 && cycles % 5 == 0) { stop(); start(root); concurrency(root); }
      if (Clock::now() - report >= 60s) { metrics(std::chrono::duration<double>(Clock::now() - from).count()); report = Clock::now(); }
      if (minutes && elapsed < duration) for (int n = 0; n < 10; ++n) { beat(); std::this_thread::sleep_for(100ms); }
    } while (Clock::now() - from < duration);
    stop();
    check(starts == ready && starts == stops, "worker lifecycle counts differ");
    check(longest_call_us < 2'000'000, "native submit/poll call blocked for over 2 seconds");
    check(!wrong_thread, "worker ran on caller");
    metrics(std::chrono::duration<double>(Clock::now() - from).count(), true);
    std::cout << "PASS native host soak; PS5 execution is not established by this test" << std::endl;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << std::endl;
    { std::lock_guard<std::mutex> lock(log_mutex); for (const auto& line : recent_logs) std::cerr << line << '\n'; }
    torrent::stop(); result = 1;
  }
  torrent::set_logger(nullptr); done = true; watchdog.join();
  return result;
}
