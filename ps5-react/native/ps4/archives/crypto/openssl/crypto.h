// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
static inline void OPENSSL_cleanse(void* data,std::size_t size) {
  auto* bytes=static_cast<volatile unsigned char*>(data);
  while(size--)*bytes++=0;
}
static inline int CRYPTO_memcmp(const void* first,const void* second,std::size_t size) {
  const auto* a=static_cast<const unsigned char*>(first);const auto* b=static_cast<const unsigned char*>(second);
  unsigned difference=0;for(std::size_t i=0;i<size;++i)difference|=a[i]^b[i];return static_cast<int>(difference);
}
