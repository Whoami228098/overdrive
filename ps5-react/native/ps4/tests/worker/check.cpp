// Owned SDK doubles exercise the actual workers; they do not emulate PS4 hardware.
#include "package_installer.hpp"
#include "app_control.hpp"
#include "bgft_abi.hpp"
#include <orbis/SystemService.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace std::chrono_literals;
namespace async_log {
void write(const char* format,...) {
  va_list args;va_start(args,format);std::vfprintf(stderr,format,args);va_end(args);std::fputc('\n',stderr);
}
}
const auto main_thread=std::this_thread::get_id();
std::atomic<int> init_count{0},term_count{0},registered{0},storage_registered{0},launched{0},foreground{7};
std::atomic<bool> registration_entered{false},allow_registration{false},finished{false},updating{false},installing{false},exists{true};
std::atomic<int> launch_result{0};
const char* title="CUSA12345";
const char* content="UP0000-CUSA12345_00-AAAAAAAAAAAAAAAA";
std::string fixture_sfo,fixture_pkg;
std::mutex accepted_guard;
std::map<int,ps4_bgft::DownloadEx*> accepted;
void off_main(){assert(std::this_thread::get_id()!=main_thread);}
void wait_for(const std::function<bool()>& predicate){
  const auto end=std::chrono::steady_clock::now()+8s;
  while(!predicate()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(5ms);}
}
template<class F> auto cheap(F function){
  const auto begin=std::chrono::steady_clock::now();auto value=function();
  assert(std::chrono::steady_clock::now()-begin<40ms);return value;
}
extern "C" int sceSysmoduleLoadModuleInternal(std::uint32_t){off_main();return 0;}
extern "C" int sceAppInstUtilInitialize(){off_main();init_count++;return 0;}
extern "C" int sceAppInstUtilTerminate(){assert(std::this_thread::get_id()==main_thread);term_count++;return 0;}
extern "C" int sceAppInstUtilAppExists(const char* value,int32_t* out){off_main();assert(!std::strcmp(value,title));std::this_thread::sleep_for(80ms);*out=exists;return 0;}
extern "C" int sceAppInstUtilAppIsInUpdating(const char*,int32_t* out){off_main();*out=updating;return 0;}
extern "C" bool sceAppInstUtilAppIsInInstalling(const char* value){off_main();assert(!std::strcmp(value,content));return installing;}
extern "C" int sceAppInstUtilGetTitleIdFromPkg(const char* path,char* out,int32_t* app){off_main();assert(path==fixture_pkg);std::strcpy(out,title);*app=1;return 0;}
extern "C" int sceUserServiceGetForegroundUser(int32_t* out){off_main();*out=foreground;return 0;}
extern "C" int sceLncUtilLaunchApp(const char* value,const char** args,LncAppParam* param){
  off_main();assert(value==std::string(title) && args==nullptr);
  assert(param->size==0x20 && param->user_id==static_cast<unsigned>(foreground.load()));
  assert(!param->app_opt && !param->check_flag && !param->crash_report);launched++;return launch_result;
}
extern "C" int sceBgftServiceInit(ps4_bgft::Init* value){off_main();assert(value->heap && value->heap_size==1024*1024);return 0;}
extern "C" int sceBgftServiceTerm(){assert(std::this_thread::get_id()==main_thread);return 0;}
int accept(ps4_bgft::DownloadEx* value,int* out,bool local){
  off_main();assert(value->param.user_id==foreground && value->param.options==ps4_bgft::disable_cdn_query);
  if(!local){registration_entered=true;wait_for([]{return allow_registration.load();});}
  const auto index=registered++;*out=index?40+index:0;
  if(local)storage_registered++;
  std::lock_guard lock(accepted_guard);accepted[*out]=value;return 0;
}
extern "C" int sceBgftServiceIntDebugDownloadRegisterPkg(ps4_bgft::DownloadEx* p,int* out){return accept(p,out,false);}
extern "C" int sceBgftServiceIntDownloadRegisterTaskByStorageEx(ps4_bgft::DownloadEx* p,int* out){return accept(p,out,true);}
extern "C" int sceBgftServiceDownloadStartTask(int id){off_main();return id==41?-55:0;}
extern "C" int sceBgftServiceDownloadGetProgress(int id,ps4_bgft::Progress* value){
  off_main();{std::lock_guard lock(accepted_guard);assert(accepted.contains(id));
    // Retained parameter pointers remain valid after registration returns.
    assert(accepted[id]->param.url && std::strlen(accepted[id]->param.url)>4);}
  value->length_total=8ull*1024*1024*1024;value->transferred_total=(finished?8ull:5ull)*1024*1024*1024;return 0;
}
extern "C" int sceBgftServiceIntDownloadUnregisterTask(int){assert(false && "accepted system downloads must never be unregistered");return -1;}
namespace host {bool resolve_path(const char* path,char* out,std::size_t size){
  off_main();if(path!=fixture_pkg || std::strlen(path)>=size)return false;std::strcpy(out,path);return true;
}}
extern "C" int __real_open(const char*,int,...);
extern "C" int __wrap_open(const char* path,int flags,...){
  std::string real=path;
  if(real.starts_with("/user/appmeta/") || real.starts_with("/system_data/priv/appmeta/")){off_main();real=fixture_sfo;}
  if(flags&O_CREAT){va_list args;va_start(args,flags);const int mode=va_arg(args,int);va_end(args);return __real_open(real.c_str(),flags,mode);}
  return __real_open(real.c_str(),flags);
}
void le32(std::vector<unsigned char>& bytes,std::size_t pos,unsigned value){for(int i=0;i<4;i++)bytes[pos+i]=value>>(8*i);}
void fixtures(){
  char pattern[]="/tmp/trapstar-workers-XXXXXX";const char* root=mkdtemp(pattern);assert(root);
  fixture_sfo=std::string(root)+"/param.sfo";fixture_pkg=std::string(root)+"/example.pkg";
  const std::vector<std::pair<std::string,std::string>> fields={{"TITLE_ID",title},{"APP_VER","01.03"},{"CONTENT_ID",content}};
  std::string keys,values;for(const auto& [key,value]:fields){keys+=key;keys+='\0';values+=value;values+='\0';}
  const auto key_start=20+fields.size()*16,value_start=key_start+keys.size();std::vector<unsigned char> bytes(value_start+values.size());
  le32(bytes,0,0x46535000);le32(bytes,4,0x101);le32(bytes,8,key_start);le32(bytes,12,value_start);le32(bytes,16,fields.size());
  std::size_t ko=0,vo=0;for(std::size_t i=0;i<fields.size();i++){
    const auto p=20+i*16;bytes[p]=ko;bytes[p+2]=4;bytes[p+3]=2;
    le32(bytes,p+4,fields[i].second.size()+1);le32(bytes,p+8,fields[i].second.size()+1);le32(bytes,p+12,vo);
    ko+=fields[i].first.size()+1;vo+=fields[i].second.size()+1;
  }
  std::memcpy(bytes.data()+key_start,keys.data(),keys.size());std::memcpy(bytes.data()+value_start,values.data(),values.size());
  std::ofstream sfo(fixture_sfo,std::ios::binary);sfo.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());sfo.close();
  bytes.assign(0x440,0);std::memcpy(bytes.data(),"\x7f" "CNT",4);std::memcpy(bytes.data()+0x40,content,36);
  for(int i=0;i<8;i++)bytes[0x430+i]=static_cast<std::uint64_t>(bytes.size())>>(8*(7-i));
  std::ofstream pkg(fixture_pkg,std::ios::binary);pkg.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
}
int main(){
  fixtures();std::string error;ps4_apps::TitleState state;
  assert(!ps4_apps::status("PPSA12345",state,error));assert(init_count==0);
  const auto request=cheap([&]{return packages::install("http://owned.test/fixture.pkg","Owned test",error);});assert(request);
  wait_for([]{return registration_entered.load();});
  // A deliberately stalled system call cannot stall any render-thread endpoint.
  cheap([&]{return packages::poll();});packages::Snapshot query;
  assert(cheap([&]{return packages::status(0,query,error);}) && !query.ready);
  assert(cheap([&]{return ps4_apps::status(title,state,error);}) && !state.ready);
  allow_registration=true;bool real_zero=false,large=false;
  wait_for([&]{for(const auto& value:packages::poll())if(value.id==request){real_zero|=value.bgft_id==0;large|=value.written>4ull*1024*1024*1024;}return real_zero&&large;});
  assert(init_count==1);wait_for([&]{ps4_apps::status(title,state,error);return state.ready;});assert(state.installed && state.version=="01.03");
  const auto launch=cheap([&]{return ps4_apps::launch(title,error);});assert(launch);
  assert(ps4_apps::launch(title,error)==launch);ps4_apps::LaunchState launch_state;
  wait_for([&]{ps4_apps::launch_status(launch,launch_state,error);return launch_state.state=="completed";});assert(launched==1);
  updating=true;const auto refused=ps4_apps::launch(title,error);
  wait_for([&]{ps4_apps::launch_status(refused,launch_state,error);return launch_state.state=="failed";});assert(launched==1);updating=false;
  foreground=-1;const auto no_user=ps4_apps::launch(title,error);
  wait_for([&]{ps4_apps::launch_status(no_user,launch_state,error);return launch_state.state=="failed";});assert(launched==1);foreground=7;
  const auto failed=packages::install("http://owned.test/second.pkg","Start failure",error);bool preserved=false;
  wait_for([&]{for(const auto& value:packages::poll())if(value.id==failed && value.state=="failed")preserved=value.bgft_id==41 && value.error.find("preserved")!=value.error.npos;return preserved;});
  packages::cancel(request);bool cancelled=false;for(const auto& value:packages::poll())cancelled|=value.id==request&&value.state=="cancelled";assert(cancelled);
  packages::detach(0);assert(registered==2);
  const auto local=packages::install(fixture_pkg,"Owned local fixture",error);bool local_ok=false;
  wait_for([&]{for(const auto& value:packages::poll())if(value.id==local)local_ok=value.bgft_id==42 && value.content_id==content;return local_ok;});assert(storage_registered==1);
  for(const auto& path:{"https://owned.test/no.pkg","magnet:?xt=bad","/tmp/no.zip"})assert(!packages::install(path,"Rejected",error));
  finished=true;bool complete=false;
  wait_for([&]{for(const auto& value:packages::poll())if(value.id==local)complete=value.state=="completed";return complete;});
  packages::stop();ps4_apps::stop();assert(registered==3 && term_count==1);
  std::remove(fixture_sfo.c_str());std::remove(fixture_pkg.c_str());
  std::puts("PASS actual PS4 workers: nonblocking commands, shared init, BGFT zero/64-bit, retained task on failure/cancel, local PKG, checked launch, clean joins");
}
