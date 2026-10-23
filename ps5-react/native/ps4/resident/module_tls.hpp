// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace resident_tls {
// The linker/ELF materializer and accessor must agree on this private tag.
constexpr uintptr_t module_tag = 0x4f445256;
bool initialize();
bool detach_current();
size_t contexts();
bool shutdown();
bool healthy();
}

extern "C" {
__attribute__((visibility("hidden"))) void* __tls_get_addr(const uintptr_t* index);
__attribute__((visibility("hidden"))) int __cxa_thread_atexit_impl(void (*call)(void*), void* value, void* dso);
}
