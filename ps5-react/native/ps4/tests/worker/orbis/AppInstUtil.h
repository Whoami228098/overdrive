#pragma once
#include <cstdint>
extern "C" {
int sceAppInstUtilInitialize();
int sceAppInstUtilTerminate();
int sceAppInstUtilAppExists(const char*,int32_t*);
int sceAppInstUtilAppIsInUpdating(const char*,int32_t*);
bool sceAppInstUtilAppIsInInstalling(const char*);
int sceAppInstUtilGetTitleIdFromPkg(const char*,char*,int32_t*);
}
