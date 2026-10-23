#pragma once
#include <cstdarg>
#include <cstdio>
namespace async_log {
inline void write(const char* format,...) {
  va_list args;va_start(args,format);std::vfprintf(stderr,format,args);va_end(args);std::fputc('\n',stderr);
}
}
