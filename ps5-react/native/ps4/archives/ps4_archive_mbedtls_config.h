// Archive crypto only; no TLS, networking, entropy service or PSA global state.
// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CIPHER_MODE_CTR
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_PKCS5_C
#include <mbedtls/check_config.h>
