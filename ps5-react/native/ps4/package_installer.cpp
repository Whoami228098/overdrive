// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "package_installer.hpp"
#include "app_control.hpp"
#include "app_identity.h"
#include "bgft_abi.hpp"
#include "host_api.hpp"
#include "async_log.hpp"
#include "worker_thread.hpp"
#include <orbis/AppInstUtil.h>
#include <orbis/UserService.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int sceSysmoduleLoadModuleInternal(std::uint32_t);

namespace packages {
namespace {
using Clock=std::chrono::steady_clock;
std::mutex guard;
std::condition_variable wake;
WorkerThread worker;
bool running=false, initialized=false;
ps4_bgft::Init service{};
std::uint32_t next_id=1;
struct Job {
  std::string path,name,content_id;
  Snapshot value;
  ps4_bgft::DownloadEx parameters{};
  bool local=false,submitted=false,reporting=true,dirty=true;
  std::uint64_t package_bytes=0;
};
struct Query { Snapshot value;Clock::time_point checked{};bool requested=false; };
// URLs, local paths and ABI parameter storage live until service termination.
std::map<std::uint32_t,std::unique_ptr<Job>> jobs;
std::map<std::uint32_t,Query> queries;
std::vector<Snapshot> cancellations;
auto (*volatile bgft_init)(ps4_bgft::Init*)=&sceBgftServiceInit;
auto (*volatile bgft_term)()=&sceBgftServiceTerm;
auto (*volatile bgft_register)(ps4_bgft::DownloadEx*,int*)=&sceBgftServiceIntDebugDownloadRegisterPkg;
auto (*volatile bgft_storage)(ps4_bgft::DownloadEx*,int*)=&sceBgftServiceIntDownloadRegisterTaskByStorageEx;
auto (*volatile bgft_start)(int)=&sceBgftServiceDownloadStartTask;
auto (*volatile bgft_progress)(int,ps4_bgft::Progress*)=&sceBgftServiceDownloadGetProgress;
auto (*volatile pkg_title)(const char*,char*,int32_t*)=&sceAppInstUtilGetTitleIdFromPkg;

bool terminal(const std::string& state) { return state=="completed" || state=="failed"; }
std::string failure(const char* call,int code) {
  char text[256];std::snprintf(text,sizeof text,"%s failed (0x%08x)",call,static_cast<unsigned>(code));
  std::string error(text);
  if (static_cast<unsigned>(code)==0x80990015 || static_cast<unsigned>(code)==0x80990086)
    error+="; an existing download conflicts: inspect Notifications > Downloads";
  if (static_cast<unsigned>(code)==0x80990088) error+="; the title already exists and was preserved";
  async_log::write("[PS4 BGFT] %s",error.c_str());return error;
}
bool initialize(std::string& error) {
  if (initialized) return true;
  if (!bgft_init || !bgft_term || !bgft_register || !bgft_start || !bgft_progress) {
    error="Required PS4 BGFT imports are unavailable";return false;
  }
  if (!ps4_apps::initialize(error)) return false;
  (void)sceSysmoduleLoadModuleInternal(0x8000002au);
  service.heap_size=1024*1024;service.heap=std::calloc(1,service.heap_size);
  if (!service.heap) { error="Cannot allocate the 1 MiB BGFT service heap";service={};return false; }
  const int rc=bgft_init(&service);
  if (rc) { error=failure("sceBgftServiceInit",rc);std::free(service.heap);service={};return false; }
  initialized=true;return true;
}
std::uint64_t big64(const unsigned char* data) {
  std::uint64_t value=0;for (int i=0;i<8;++i) value=(value<<8)|data[i];return value;
}
bool inspect_local(Job& job,std::string& error) {
  if (!bgft_storage || !pkg_title) { error="Local PS4 package service imports are unavailable";return false; }
  char resolved[4096];
  if (!host::resolve_path(job.path.c_str(),resolved,sizeof resolved)) { error="Local package path is outside the host filesystem policy";return false; }
  const int fd=::open(resolved,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
  if (fd<0) { error="Cannot open the local PS4 package";return false; }
  struct stat info{};std::array<unsigned char,0x440> bytes{};
  bool valid=::fstat(fd,&info)==0 && S_ISREG(info.st_mode) && info.st_size>=static_cast<off_t>(bytes.size());
  std::size_t offset=0;
  while (valid && offset<bytes.size()) {
    const auto count=::read(fd,bytes.data()+offset,bytes.size()-offset);
    if (count<0 && errno==EINTR) continue;
    if (count<=0) { valid=false;break; }
    offset+=static_cast<std::size_t>(count);
  }
  ::close(fd);
  if (!valid || std::memcmp(bytes.data(),"\x7f" "CNT",4) || big64(bytes.data()+0x430)!=static_cast<std::uint64_t>(info.st_size)) {
    error="The local file is not a complete PS4 PKG";return false;
  }
  const std::string content(reinterpret_cast<const char*>(bytes.data()+0x40),36);
  char verified_title[10];
  if (!overdrive_ps4_content_title(content.c_str(),verified_title)) { error="The package has no valid PS4 CUSA content ID";return false; }
  char native_title[16]{};int32_t is_app=0;
  const int rc=pkg_title(resolved,native_title,&is_app);
  if (rc || !overdrive_ps4_title_id(native_title) || std::memcmp(native_title,verified_title,10)) {
    error=rc ? failure("sceAppInstUtilGetTitleIdFromPkg",rc) : "The system package title differs from its verified header";return false;
  }
  job.path=resolved;job.package_bytes=static_cast<std::uint64_t>(info.st_size);
  job.content_id=content;return true;
}
void publish(Job& job,Snapshot value) {
  std::lock_guard lock(guard);job.value=std::move(value);job.dirty=true;
}
void submit(Job& job) {
  Snapshot value=job.value;
  value.state="installing";value.status="Registering a visible PS4 system download";publish(job,value);
  std::string error;
  if (!initialize(error) || (job.local && !inspect_local(job,error))) {
    value.state="failed";value.error=std::move(error);publish(job,value);return;
  }
  int32_t user=-1;
  int rc=sceUserServiceGetForegroundUser(&user);
  if (rc || user<0) { value.state="failed";value.error=failure("sceUserServiceGetForegroundUser",rc);publish(job,value);return; }
  auto& parameters=job.parameters;
  parameters.param.user_id=user;parameters.param.entitlement_type=5;parameters.param.id="";
  parameters.param.url=job.path.c_str();parameters.param.name=job.name.c_str();parameters.param.icon="";
  parameters.param.scenario="0";parameters.param.options=ps4_bgft::disable_cdn_query;
  int task=-1;
  rc=job.local ? bgft_storage(&parameters,&task) : bgft_register(&parameters,&task);
  if (rc || task<0) {
    value.state="failed";value.error=failure(job.local?"sceBgftServiceIntDownloadRegisterTaskByStorageEx":"sceBgftServiceIntDebugDownloadRegisterPkg",rc);
    publish(job,value);return;
  }
  // Emit the real accepted ID before Start. Failure never unregisters an accepted task.
  value.bgft_id=task;value.content_id=job.content_id;value.total=job.package_bytes;
  value.status="Accepted in Notifications > Downloads";publish(job,value);
  async_log::write("[PS4 BGFT] request %u accepted as system job %d",value.id,task);
  rc=bgft_start(task);
  if (rc) {
    value.state="failed";value.error=failure("sceBgftServiceDownloadStartTask",rc)+"; accepted system job "+std::to_string(task)+" was preserved";
  } else value.status="Downloading and installing in Notifications > Downloads";
  publish(job,value);
}
Snapshot query(std::uint32_t system_id,Snapshot value,std::uint64_t minimum=0) {
  value.ready=true;
  std::string error;
  if (!initialize(error)) { value.state="failed";value.error=std::move(error);return value; }
  ps4_bgft::Progress progress{};
  const int rc=bgft_progress(static_cast<int>(system_id),&progress);
  value.bgft_id=system_id;
  if (rc) {
    value.state="failed";value.error=failure("sceBgftServiceDownloadGetProgress",rc)+"; the system job was not cancelled";return value;
  }
  value.written=progress.length_total ? progress.transferred_total : progress.transferred;
  value.total=progress.length_total ? progress.length_total : progress.length;
  value.error.clear();value.state="installing";value.status="Transferring in Notifications > Downloads";
  if (progress.error) { value.state="failed";value.error=failure("BGFT download",progress.error); }
  else if (value.total && value.total>=minimum && value.written>=value.total) {
    value.state="completed";value.status="Transfer complete; final system installation is checked separately";
  }
  return value;
}
void run() {
  async_log::write("[PS4 BGFT] worker ready");
  for (;;) {
    std::vector<Job*> pending,active;
    std::vector<std::uint32_t> checks;
    {
      std::unique_lock lock(guard);
      wake.wait_for(lock,std::chrono::milliseconds(500),[]{
        return !running || std::any_of(jobs.begin(),jobs.end(),[](const auto& item){return !item.second->submitted;}) ||
          std::any_of(queries.begin(),queries.end(),[](const auto& item){return item.second.requested;});
      });
      if (!running) return;
      for (auto& [_,job]:jobs) {
        if (!job->submitted) { job->submitted=true;pending.push_back(job.get()); }
        else if (job->reporting && job->value.bgft_id>=0 && !terminal(job->value.state)) active.push_back(job.get());
      }
      for (auto& [id,item]:queries) if (item.requested) { item.requested=false;checks.push_back(id); }
    }
    for (auto* job:pending) submit(*job);
    for (auto* job:active) {
      Snapshot value;
      { std::lock_guard lock(guard);value=job->value; }
      publish(*job,query(static_cast<std::uint32_t>(value.bgft_id),value,job->package_bytes));
    }
    for (const auto id:checks) {
      Snapshot previous;
      { std::lock_guard lock(guard);const auto it=queries.find(id);if (it==queries.end()) continue;previous=it->second.value; }
      auto result=query(id,previous);
      std::lock_guard lock(guard);const auto it=queries.find(id);
      if (it!=queries.end()) { it->second.value=std::move(result);it->second.checked=Clock::now(); }
    }
  }
}
bool start_locked(std::string& error) {
  if (running) return true;
  running=true;
  if (!worker.start(run,512*1024)) { running=false;error="Cannot start the PS4 BGFT worker";return false; }
  return true;
}
}

std::uint32_t install(std::string path,std::string name,std::string& error) {
  error.clear();
  const auto end=path.find_first_of("?#");const auto base=path.substr(0,end);
  const bool local=path.starts_with("/");
  std::string extension=base.size()>=4 ? base.substr(base.size()-4) : "";
  std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(c>='A' && c<='Z'?c+32:c);});
  if ((!local && !path.starts_with("http://")) || path.size()>=2048 || path.find('\0')!=path.npos ||
      path.find_first_of("\r\n\t")!=path.npos || (!local && path.find(' ')!=path.npos) || extension!=".pkg" ||
      (local && end!=path.npos)) {
    error="PS4 packages.install expects a complete local .pkg or direct http://.../file.pkg URL";return 0;
  }
  if (name.size()>200 || name.find_first_of("\r\n")!=name.npos || name.find('\0')!=name.npos) {
    error="PS4 package name must be one line of at most 200 bytes";return 0;
  }
  std::lock_guard lock(guard);
  if (jobs.size()>=32 || next_id>INT_MAX || !next_id) { error="PS4 package queue is full; reopen the store to release old foreground tracking";return 0; }
  if (!start_locked(error)) return 0;
  const auto id=next_id++;auto job=std::make_unique<Job>();
  job->path=std::move(path);job->name=name.empty()?"TRAPSTARSHOP PS4 package":std::move(name);job->local=local;
  job->value.id=id;job->value.status="Waiting for the PS4 package worker";
  jobs.emplace(id,std::move(job));wake.notify_one();return id;
}
bool status(std::uint32_t id,Snapshot& out,std::string& error) {
  if (id>INT_MAX) { error="Invalid BGFT system job ID";return false; }
  std::lock_guard lock(guard);
  if (!start_locked(error)) return false;
  if (!queries.contains(id) && queries.size()>=64) { error="PS4 BGFT status cache is full; reopen the store";return false; }
  auto& item=queries[id];
  if (item.checked==Clock::time_point{}) {
    item.value.id=id;item.value.bgft_id=id;item.value.state="installing";item.value.status="Checking the existing system download";
    item.value.ready=false;
  }
  if (!item.requested && Clock::now()-item.checked>std::chrono::seconds(1)) { item.requested=true;wake.notify_one(); }
  out=item.value;return true;
}
void cancel(std::uint32_t id) {
  std::lock_guard lock(guard);const auto it=jobs.find(id);
  if (it==jobs.end() || !it->second->reporting) return;
  auto value=it->second->value;value.state="cancelled";value.error.clear();cancellations.push_back(std::move(value));
  it->second->reporting=false;
}
void detach(std::uint32_t bgft_id) {
  std::lock_guard lock(guard);queries.erase(bgft_id);
  // This releases reporting only; never control or delete an accepted system task.
}
std::vector<Snapshot> poll() {
  std::lock_guard lock(guard);std::vector<Snapshot> out;out.swap(cancellations);
  for (auto& [_,job]:jobs) {
    if (!job->reporting || !job->dirty) continue;
    out.push_back(job->value);job->dirty=false;
    if (terminal(job->value.state)) job->reporting=false;
  }
  return out;
}
void stop() {
  { std::lock_guard lock(guard);running=false; }
  wake.notify_all();worker.join();
  // Accepted jobs belong to BGFT. Closing the store never stops/unregisters them.
  if (initialized) {
    const int rc=bgft_term();
    if (rc) { failure("sceBgftServiceTerm",rc);return; }
  }
  initialized=false;std::free(service.heap);service={};
  std::lock_guard lock(guard);jobs.clear();queries.clear();cancellations.clear();
}
}
