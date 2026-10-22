// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app_control.hpp"
#include "app_identity.h"
#include "async_log.hpp"
#include "worker_thread.hpp"
#include <orbis/AppInstUtil.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" int sceSysmoduleLoadModuleInternal(std::uint32_t);

namespace ps4_apps {
namespace {
using Clock=std::chrono::steady_clock;
std::mutex guard, service_guard;
std::condition_variable wake;
WorkerThread worker;
bool running=false, service_ready=false;
std::uint32_t next_launch=1;
struct Cached { TitleState value; Clock::time_point checked{}; bool pending=false; };
struct Command { std::string title; std::uint32_t launch_id=0; };
std::map<std::string,Cached> titles;
std::map<std::uint32_t,LaunchState> launches;
std::deque<Command> commands;

// Direct import addresses are checked before every service is first used.
auto (*volatile app_init)() = &sceAppInstUtilInitialize;
auto (*volatile app_term)() = &sceAppInstUtilTerminate;
auto (*volatile app_exists)(const char*,int32_t*) = &sceAppInstUtilAppExists;
auto (*volatile app_updating)(const char*,int32_t*) = &sceAppInstUtilAppIsInUpdating;
bool (*volatile app_installing)(const char*) = &sceAppInstUtilAppIsInInstalling;
auto (*volatile app_launch)(const char*,const char**,LncAppParam*) = &sceLncUtilLaunchApp;
auto (*volatile foreground_user)(int32_t*) = &sceUserServiceGetForegroundUser;

std::string failure(const char* call,int code) {
  char text[160];
  std::snprintf(text,sizeof text,"%s failed (0x%08x)",call,static_cast<unsigned>(code));
  return text;
}

bool metadata(const std::string& title,OverdrivePs4Identity& out) {
  for (const char* root:{"/user/appmeta/","/system_data/priv/appmeta/"}) {
    const std::string path=std::string(root)+title+"/param.sfo";
    const int fd=::open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if (fd<0) continue;
    struct stat info{};
    const bool valid=::fstat(fd,&info)==0 && S_ISREG(info.st_mode) && info.st_size>=20 && info.st_size<=1024*1024;
    std::vector<unsigned char> bytes(valid?static_cast<std::size_t>(info.st_size):0);
    std::size_t offset=0;
    while (offset<bytes.size()) {
      const auto count=::read(fd,bytes.data()+offset,bytes.size()-offset);
      if (count<0 && errno==EINTR) continue;
      if (count<=0) break;
      offset+=static_cast<std::size_t>(count);
    }
    ::close(fd);
    if (valid && offset==bytes.size() && overdrive_ps4_parse_sfo(bytes.data(),bytes.size(),&out) && title==out.title_id) return true;
  }
  return false;
}

TitleState query(const std::string& title) {
  TitleState result;result.title_id=title;result.ready=true;
  if (!initialize(result.error)) return result;
  int32_t exists=0, updating=0;
  int rc=app_exists(title.c_str(),&exists);
  if (rc) { result.error=failure("sceAppInstUtilAppExists",rc);return result; }
  result.exists=exists!=0;
  if (!result.exists) return result;
  rc=app_updating(title.c_str(),&updating);
  if (rc) { result.error=failure("sceAppInstUtilAppIsInUpdating",rc);return result; }
  result.updating=updating!=0;
  OverdrivePs4Identity identity{};
  if (!metadata(title,identity)) {
    result.error="Installed title metadata is unavailable; its version was not guessed";
    return result;
  }
  result.version=identity.version;result.content_id=identity.content_id;
  result.installing=!result.content_id.empty() && app_installing(result.content_id.c_str());
  result.installed=!result.updating && !result.installing;
  return result;
}

void run() {
  async_log::write("[PS4 Apps] worker ready");
  for (;;) {
    Command command;
    {
      std::unique_lock lock(guard);
      wake.wait(lock,[]{return !running || !commands.empty();});
      if (!running) return;
      command=std::move(commands.front());commands.pop_front();
      if (command.launch_id) launches.at(command.launch_id).state="checking";
    }
    auto state=query(command.title);
    {
      std::lock_guard lock(guard);
      auto& cached=titles[command.title];
      if (!state.error.empty() && state.error!=cached.value.error)
        async_log::write("[PS4 Apps] title %s: %s",command.title.c_str(),state.error.c_str());
      cached.value=state;cached.checked=Clock::now();cached.pending=false;
    }
    if (!command.launch_id) continue;
    std::string error=state.error;
    if (error.empty() && !state.installed) error="The selected PS4 title is not fully installed; inspect Notifications > Downloads";
    if (error.empty() && (!app_launch || !foreground_user)) error="PS4 launch service imports are unavailable";
    int32_t user=-1;
    if (error.empty()) {
      const int rc=foreground_user(&user);
      if (rc || user<0) error=failure("sceUserServiceGetForegroundUser",rc);
    }
    if (error.empty()) {
      static_assert(sizeof(LncAppParam)==0x20 && offsetof(LncAppParam,user_id)==4 && offsetof(LncAppParam,crash_report)==0x10);
      LncAppParam parameters{};parameters.size=sizeof parameters;parameters.user_id=static_cast<std::uint32_t>(user);
      async_log::write("[PS4 Apps] launching id=%u title=%s",command.launch_id,command.title.c_str());
      const int rc=app_launch(command.title.c_str(),nullptr,&parameters);
      if (rc<0) error=failure("sceLncUtilLaunchApp",rc);
      async_log::write("[PS4 Apps] launch id=%u result=0x%08x",command.launch_id,static_cast<unsigned>(rc));
    }
    {
      std::lock_guard lock(guard);
      auto& item=launches.at(command.launch_id);item.state=error.empty()?"completed":"failed";item.error=std::move(error);
    }
  }
}

bool start_locked(std::string& error) {
  if (running) return true;
  running=true;
  if (!worker.start(run,512*1024)) { running=false;error="Cannot start the PS4 app-control worker";return false; }
  return true;
}
bool valid(const std::string& title,std::string& error) {
  if (title.size()==9 && overdrive_ps4_title_id(title.c_str())) return true;
  error="Expected a PS4 CUSA title ID";return false;
}
}

bool initialize(std::string& error) {
  std::lock_guard lock(service_guard);
  if (service_ready) return true;
  if (!app_init || !app_term || !app_exists || !app_updating || !app_installing) {
    error="PS4 AppInstUtil service imports are unavailable";return false;
  }
  (void)sceSysmoduleLoadModuleInternal(0x80000014u);
  const int rc=app_init();
  if (rc) { error=failure("sceAppInstUtilInitialize",rc);return false; }
  service_ready=true;
  return true;
}

bool status(const std::string& title,TitleState& out,std::string& error) {
  if (!valid(title,error)) return false;
  std::lock_guard lock(guard);
  if (!start_locked(error)) return false;
  if (!titles.contains(title) && titles.size()>=256) {
    auto oldest=titles.end();
    for (auto it=titles.begin();it!=titles.end();++it)
      if (!it->second.pending && (oldest==titles.end() || it->second.checked<oldest->second.checked)) oldest=it;
    if (oldest==titles.end()) { error="PS4 title status queue is busy";return false; }
    titles.erase(oldest);
  }
  auto& cached=titles[title];
  if (!cached.pending && (!cached.value.ready || Clock::now()-cached.checked>std::chrono::seconds(1))) {
    cached.pending=true;commands.push_back({title,0});wake.notify_one();
  }
  out=cached.value;out.title_id=title;
  return true;
}

std::uint32_t launch(const std::string& title,std::string& error) {
  if (!valid(title,error)) return 0;
  std::lock_guard lock(guard);
  if (!start_locked(error)) return 0;
  for (const auto& [id,item]:launches)
    if (item.title_id==title && item.state!="completed" && item.state!="failed") return id;
  if (launches.size()>=16) {
    const auto previous=std::find_if(launches.begin(),launches.end(),[](const auto& item){return item.second.state=="completed" || item.second.state=="failed";});
    if (previous!=launches.end()) launches.erase(previous);
  }
  if (launches.size()>=16 || (titles.size()>=256 && !titles.contains(title)) || !next_launch) {
    error="PS4 launch queue is busy";return 0;
  }
  const auto id=next_launch++;
  LaunchState item;item.id=id;item.title_id=title;launches.emplace(id,std::move(item));
  commands.push_back({title,id});wake.notify_one();
  return id;
}

bool launch_status(std::uint32_t id,LaunchState& out,std::string& error) {
  std::lock_guard lock(guard);
  const auto item=launches.find(id);
  if (item==launches.end()) { error="Unknown PS4 launch request";return false; }
  out=item->second;return true;
}

void stop() {
  { std::lock_guard lock(guard);running=false; }
  wake.notify_all();worker.join();
  { std::lock_guard lock(guard);titles.clear();launches.clear();commands.clear(); }
  std::lock_guard lock(service_guard);
  if (service_ready) { (void)app_term();service_ready=false; }
}
}
