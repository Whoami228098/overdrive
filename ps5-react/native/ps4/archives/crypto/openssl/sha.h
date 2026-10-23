// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <mbedtls/sha256.h>
#include <cstddef>
#define SHA256_DIGEST_LENGTH 32
static inline unsigned char* SHA256(const unsigned char* bytes,std::size_t size,unsigned char* out) {
  return out&&mbedtls_sha256_ret(bytes,size,out,0)==0?out:nullptr;
}
