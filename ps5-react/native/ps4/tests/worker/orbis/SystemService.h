#pragma once
#include <cstdint>
struct LncAppParam {
  uint32_t size,user_id,app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
};
extern "C" int sceLncUtilLaunchApp(const char*,const char**,LncAppParam*);
