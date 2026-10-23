// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Real resident/extractor code with owned torrent and PS4 system SDK doubles.
#include "../../resident/service.cpp"
#include <cassert>
#include <fstream>
#include <iostream>
namespace fixture {
std::string source;std::uint64_t bytes=0;std::atomic<int> reads{0},adds{0},registrations{0};
std::atomic<bool> fail_read{false};std::string registered_url;
}
namespace network {bool start(){return true;}void stop(){} }
namespace torrent {
bool start(const char*){return true;}void stop(){}
int add_magnet(const char*,const char*,long long){++fixture::adds;return 1;}
int poll(Status* out,int){out[0]={};out[0].id=1;out[0].state=downloading;return 1;}
void pause(int){}void resume(int){}void remove(int,bool){}
const char* last_error(){return "owned fixture";}
std::string error(int){return "owned fixture failure";}
std::vector<File> files(int){return{{fixture::source,static_cast<long long>(fixture::bytes)}};}
namespace stream {
std::string key(const std::string& uri,std::string&){return "v1-"+uri.substr(uri.find_last_of(':')+1,40);}
bool select(int,const std::string& path,std::string& error){if(path!=fixture::source){error="wrong fixture source";return false;}return true;}
bool read(int,std::uint64_t offset,std::size_t count,std::vector<char>& bytes,std::string& error){
  ++fixture::reads;std::this_thread::sleep_for(std::chrono::milliseconds(3));
  if(fixture::fail_read){error="owned injected read failure";return false;}
  std::ifstream file(fixture::source,std::ios::binary);file.seekg(offset);bytes.resize(count);
  file.read(bytes.data(),static_cast<std::streamsize>(count));bytes.resize(static_cast<std::size_t>(file.gcount()));return !bytes.empty();
}
bool peek(int id,std::uint64_t offset,std::size_t count,std::vector<char>& bytes,std::string& error){return read(id,offset,count,bytes,error);}
void suspend(int){}
}
}
extern "C" {
int sceSysmoduleLoadModuleInternal(std::uint32_t){return 0;}
int sceUserServiceGetForegroundUser(int* value){*value=1;return 0;}
int sceBgftServiceInit(ps4_bgft::Init*){return 0;}int sceBgftServiceTerm(){return 0;}
int sceBgftServiceIntDebugDownloadRegisterPkg(ps4_bgft::DownloadEx* request,int* id){
  ++fixture::registrations;fixture::registered_url=request->param.url;*id=0;return 0;
}
int sceBgftServiceDownloadStartTask(int){return 0;}int sceBgftServiceDownloadPauseTask(int){return 0;}
int sceBgftServiceDownloadResumeTask(int){return 0;}
int sceBgftServiceDownloadFindTaskByContentId(const char*,int,int* id){*id=0;return 0;}
int sceBgftServiceDownloadGetProgress(int,ps4_bgft::Progress* out){*out={};return 0;}
}
using namespace resident_service;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void until(F done){for(int n=0;n<600;n++){if(done())return;std::this_thread::sleep_for(std::chrono::milliseconds(10));}throw std::runtime_error("Resident fixture timed out");}
std::string submit(const std::string& uri,const std::string& password,const std::string& file) {
  int pair[2];require(::socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0,"socketpair");Fd client(pair[0]),server(pair[1]);
  Request request;request.path="/jobs";request.method="POST";request.auth=token;
  request.body=hex(uri)+"\n"+hex("Owned PS4 archive")+"\n"+std::to_string(fixture::bytes)+"\n"+hex(password)+"\n"+hex(file)+"\n";
  control(server.value,request);::shutdown(server.value,SHUT_WR);std::string out;char chunk[4096];
  for(ssize_t count;(count=::read(client.value,chunk,sizeof chunk))>0;)out.append(chunk,count);return out;
}
int main(int argc,char** argv) {
  require(argc==5,"archive, expected extracted package, published password, mode required");
  // The runner prepares a fresh disposable /data tree. Never erase existing data.
  require(!fs::exists(base),"Refusing an existing resident test root");
  fs::create_directories(job_dir);fs::create_directories(archive_dir);fs::create_directories(std::string(base)+"/archives");
  const auto key="v1-"+std::string(40,'a');const auto uri="magnet:?xt=urn:btih:"+std::string(40,'a');
  fs::path source=fs::path("/data/downloads/torrents")/key/fs::path(argv[1]).filename();fs::create_directories(source.parent_path());fs::copy_file(argv[1],source);
  fixture::source=source.string();fixture::bytes=fs::file_size(source);token=std::string(64,'a');alive=true;
  const auto first=submit(uri,argv[3],source.filename().string());require(first.find("201 Created")!=first.npos,"Initial durable submission");
  Job job;require(job_copy(1,job)&&job.password==argv[3],"Exact password persisted");
  const auto restored=decode(std::string(job_dir)+"/1.job");require(restored.password==argv[3]&&restored.requested_archive==source.filename().string(),"Metadata roundtrip");
  archive_thread=std::thread(archive_loop);registration_thread=std::thread(register_loop);
  tick(1);until([]{return fixture::reads>0;});
  if(std::string(argv[4])=="read-failure")fixture::fail_read=true;
  // There is no foreground app or client during this work. Reattaching the
  // same magnet must retain its worker and never enqueue another transfer.
  const auto again=submit(uri,argv[3],source.filename().string());require(again.find("200 OK")!=again.npos,"Reattach existing source");
  until([&]{job_copy(1,job);return job.state=="failed"||archived_package(job);});
  if(std::string(argv[4])=="failure"||std::string(argv[4])=="read-failure") {
    require(job.state=="failed","Refused malformed/password/read fixture");require(fixture::registrations==0,"No failed archive registered with BGFT");
    require(fs::file_size(source)==fixture::bytes,"Failed original archive retained");
  }else{
    require(job.state!="failed",job.error.c_str());require(archived_package(job),"Archive was actually decoded");
    require(file_hash(job.path,fs::file_size(job.path),1)==file_hash(argv[2],fs::file_size(argv[2]),1),"Extracted PKG SHA-256 equals fixture");
    until([&]{job_copy(1,job);return !job.archive_pending;});
    const auto read_count=fixture::reads.load();
    // Simulate a saved extraction intent restored after service/process loss.
    // Its complete archive and published receipt must recover without peers.
    {std::lock_guard<std::mutex> lock(jobs_mutex);auto& current=jobs.at(1);current.path=current.archive_source;
     current.size=0;current.engine=0;current.state="queued";save(current);}
    tick(1);until([&]{job_copy(1,job);return !job.archive_pending&&(job.state=="failed"||archived_package(job));});
    require(job.state!="failed",job.error.c_str());require(fixture::reads==read_count,"Completed cache recovery performs no torrent reads");
    header(job);require(job.content=="UP9000-SLUS21590_00-SLUS215900000001","Actual PS4 content header");
    tick(1);until([&]{job_copy(1,job);return job.bgft==0;});
    require(fixture::registrations==1,"One and only one BGFT registration, including ID zero");
    const auto restored_again=decode(std::string(job_dir)+"/1.job");require(restored_again.path==job.path,"Extracted PKG survives service checkpoint restore");
    // Serve the actual validated extracted PKG through the real ranged feeder.
    int pair[2];require(::socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0,"feed pair");Fd client(pair[0]),server(pair[1]);
    Request request;request.method="GET";request.path="/pkg/"+token+"/1.pkg";request.range="bytes=0-1087";
    feed(server.value,request);::shutdown(server.value,SHUT_WR);std::string response;char buffer[4096];
    for(ssize_t n;(n=::read(client.value,buffer,sizeof buffer))>0;)response.append(buffer,n);
    require(response.find("206 Partial Content")!=response.npos&&response.find(std::string("\x7f" "CNT",4))!=response.npos,"Actual local PKG HTTP range");
    require(submit(uri,argv[3],source.filename().string()).find("200 OK")!=std::string::npos,"Completed archive source reattaches");
  }
  require(fixture::adds==1,"Foreground reattachment cannot create another engine");
  stop();std::cout<<"PASS: owned resident archive flow "<<argv[4]<<"; real decoder, hashes, checkpoint, BGFT ownership and range feed\n";
}
