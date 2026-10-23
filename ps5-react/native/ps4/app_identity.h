// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  char title_id[10];
  char version[16];
  char content_id[37];
} OverdrivePs4Identity;
int overdrive_ps4_title_id(const char* value);
int overdrive_ps4_content_title(const char* value, char title_id[10]);
int overdrive_ps4_same_version(const char* left, const char* right);
int overdrive_ps4_parse_sfo(const unsigned char* bytes, size_t size, OverdrivePs4Identity* out);
#ifdef __cplusplus
}
#endif
