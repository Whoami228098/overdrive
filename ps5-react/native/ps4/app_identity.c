// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app_identity.h"
#include <stdint.h>

static uint16_t little16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t little32(const unsigned char* p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int equal(const unsigned char* a, size_t maximum, const char* b) {
  size_t i=0;
  while (i<maximum && b[i] && a[i]==(unsigned char)b[i]) ++i;
  return i<maximum && !b[i] && !a[i];
}
static int string_value(char* target, size_t capacity, const unsigned char* source, size_t maximum) {
  size_t i=0;
  for (; i<maximum && source[i]; ++i) {
    if (i+1>=capacity || source[i]>127) return 0;
    target[i]=(char)source[i];
  }
  if (i==maximum) return 0;
  target[i]=0;
  return 1;
}
int overdrive_ps4_title_id(const char* value) {
  if (!value) return 0;
  for (size_t i=0;i<4;++i) if (value[i]<'A' || value[i]>'Z') return 0;
  // PS2/PS1/PSP conversions may retain IDs such as SLUS21590. Their verified
  // PS4 PKG container remains mandatory; native PS5 IDs remain excluded.
  if (value[0]=='P' && value[1]=='P' && value[2]=='S' && value[3]=='A') return 0;
  for (size_t i=4;i<9;++i) if (value[i]<'0' || value[i]>'9') return 0;
  return !value[9];
}
int overdrive_ps4_content_title(const char* value, char title_id[10]) {
  if (!value) return 0;
  size_t size=0;
  while (size<37 && value[size]) ++size;
  if (size!=36 || value[6]!='-' || value[16]!='_' || value[19]!='-') return 0;
  for (size_t i=0;i<size;++i)
    if (!((value[i]>='A' && value[i]<='Z') || (value[i]>='0' && value[i]<='9') || value[i]=='-' || value[i]=='_')) return 0;
  char parsed[10];
  for (size_t i=0;i<9;++i) parsed[i]=value[7+i];
  parsed[9]=0;
  if (!overdrive_ps4_title_id(parsed)) return 0;
  for (size_t i=0;i<10;++i) title_id[i]=parsed[i];
  return 1;
}
static int version(const char* input, unsigned values[4]) {
  unsigned count=0;
  if (!input) return 0;
  for (;;) {
    unsigned value=0, digits=0;
    while (*input>='0' && *input<='9') {
      if (++digits>3) return 0;
      value=value*10+(unsigned)(*input++-'0');
    }
    if (!digits || count==4) return 0;
    values[count++]=value;
    if (!*input) return count>=2 ? (int)count : 0;
    if (*input++!='.') return 0;
  }
}
int overdrive_ps4_same_version(const char* left, const char* right) {
  unsigned a[4],b[4];
  const int count=version(left,a);
  if (!count || version(right,b)!=count) return 0;
  for (int i=0;i<count;++i) if (a[i]!=b[i]) return 0;
  return 1;
}
int overdrive_ps4_parse_sfo(const unsigned char* bytes, size_t size, OverdrivePs4Identity* out) {
  if (!bytes || !out || size<20 || size>1024*1024) return 0;
  for (size_t i=0;i<sizeof *out;++i) ((unsigned char*)out)[i]=0;
  if (little32(bytes)!=0x46535000 || little32(bytes+4)!=0x101) return 0;
  const size_t keys=little32(bytes+8), values=little32(bytes+12), count=little32(bytes+16);
  if (count>256 || keys<20+count*16 || values<keys || values>size) return 0;
  for (size_t i=0;i<count;++i) {
    const unsigned char* entry=bytes+20+i*16;
    const size_t key_offset=little16(entry), length=little32(entry+4), maximum=little32(entry+8), offset=little32(entry+12);
    if (key_offset>=values-keys || offset>size-values || length>maximum || maximum>size-values-offset) return 0;
    if (little16(entry+2)!=0x204) continue;
    const unsigned char* key=bytes+keys+key_offset;
    const unsigned char* data=bytes+values+offset;
    const size_t key_maximum=values-keys-key_offset;
    if (equal(key,key_maximum,"TITLE_ID")) {
      if (!string_value(out->title_id,sizeof out->title_id,data,length)) return 0;
    } else if (equal(key,key_maximum,"APP_VER")) {
      if (!string_value(out->version,sizeof out->version,data,length)) return 0;
    } else if (equal(key,key_maximum,"CONTENT_ID")) {
      if (!string_value(out->content_id,sizeof out->content_id,data,length)) return 0;
    }
  }
  unsigned parts[4];
  if (!overdrive_ps4_title_id(out->title_id) || !version(out->version,parts)) return 0;
  if (out->content_id[0]) {
    char title[10];
    if (!overdrive_ps4_content_title(out->content_id,title)) return 0;
    for (size_t i=0;i<10;++i) if (title[i]!=out->title_id[i]) return 0;
  }
  return 1;
}
