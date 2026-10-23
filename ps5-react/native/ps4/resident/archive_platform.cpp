// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "archives.hpp"
#include "async_log.hpp"
#include <dirent.h>
#include <string>
namespace archives {
bool list_directory(const char* path,void(*visit)(const char*,void*),void* user) {
  auto* directory=::opendir(path);if(!directory)return false;
  while(auto* entry=::readdir(directory))visit(entry->d_name,user);
  return ::closedir(directory)==0;
}
void trace(const char* step,const char* detail) {async_log::write("[resident archive] %s %s",step,detail);}
std::size_t pipeline_bytes(){return 8*1024*1024;}
bool hold_memory(bool){return true;}
bool downloading(){return false;}
int start_rar_worker(const std::string&,std::string& error){error="No separate UnRAR process on PS4; using libarchive";return -1;}
long read_rar_worker(int,char*,std::size_t){return -1;}
void cancel_rar_worker(int){}
void close_rar_worker(int){}
}
