// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// PRX ABI metadata follows OpenOrbis crtlib and GoldHEN crtprx conventions.
// This module owns its C++ initialization; it does not hook any host functions.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include "module_tls.hpp"

namespace resident_service { bool start(); void stop(); }

extern "C" {
using Initializer = void (*)();
extern Initializer __resident_init_start[], __resident_init_end[];
extern Initializer __resident_fini_start[], __resident_fini_end[];
__attribute__((visibility("hidden"))) void* __dso_handle = &__dso_handle;
__attribute__((visibility("hidden"))) void* _sceLibc = nullptr;
struct ModuleParam { std::uint64_t size, magic, sdk; };
__attribute__((section(".data.sce_module_param"), used, visibility("hidden")))
const ModuleParam _sceModuleParam = {0x18, 0x13C13F4BFULL, 0x04508101};
}

namespace {
// Zero initialization is available before libc++ and service constructors.
int state = 0; // 0=cold, 1=starting, 2=running, 3=stopping, 4=stopped, 5=pinned failure, -1=clean failure.
bool constructed = false;
bool entered_service = false;
__attribute__((noinline)) void throw_runtime_probe(std::uintptr_t value) { throw value; }
bool probe_runtime() {
  constexpr std::uintptr_t sentinel = 0x4f445256435858ULL;
  try { throw_runtime_probe(sentinel); }
  catch (std::uintptr_t value) { return value == sentinel; }
  catch (...) { return false; }
  return false;
}
int destructor_lock = 0;
struct Destructor { void (*call)(void*); void* value; void* dso; Destructor* next; };
Destructor* destructors = nullptr;
void lock_destructors() {
  while (__atomic_exchange_n(&destructor_lock, 1, __ATOMIC_ACQUIRE))
    __asm__ volatile("pause");
}
void unlock_destructors() { __atomic_store_n(&destructor_lock, 0, __ATOMIC_RELEASE); }
}

// The supplied static musl __cxa_finalize is intentionally empty. Wrap the ABI
// entries so PRX unloading really destroys this module's C++ objects and cannot
// run the host application's destructors. Worker threads must be joined first.
extern "C" __attribute__((visibility("hidden")))
int __wrap___cxa_atexit(void (*call)(void*), void* value, void* dso) {
  auto* entry = static_cast<Destructor*>(std::malloc(sizeof(Destructor)));
  if (!entry) return -1;
  lock_destructors();
  *entry = {call, value, dso, destructors};
  destructors = entry;
  unlock_destructors();
  return 0;
}
extern "C" __attribute__((visibility("hidden")))
void __wrap___cxa_finalize(void* dso) {
  for (;;) {
    lock_destructors();
    auto** current = &destructors;
    while (*current && dso && (*current)->dso != dso) current = &(*current)->next;
    auto* entry = *current;
    if (entry) *current = entry->next;
    unlock_destructors();
    if (!entry) break;
    auto call = entry->call;
    auto value = entry->value;
    std::free(entry);
    call(value);
  }
}

namespace {
void finalize() {
  if (!constructed) return;
  __wrap___cxa_finalize(&__dso_handle);
  for (auto* fn = __resident_fini_end; fn != __resident_fini_start;) {
    --fn;
    if (*fn) (*fn)();
  }
  constructed = false;
}
}

extern "C" __attribute__((visibility("hidden")))
int module_start(std::size_t, const void*) {
  int expected = 0;
  if (!__atomic_compare_exchange_n(&state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return expected == 2 ? 0 : -1;
  if (!resident_tls::initialize()) {
    __atomic_store_n(&state, -1, __ATOMIC_RELEASE);
    return -1;
  }
  try {
    constructed = true;
    for (auto* fn = __resident_init_start; fn != __resident_init_end; ++fn)
      if (*fn) (*fn)();
    if (probe_runtime()) entered_service = true;
    if (entered_service && resident_service::start() && resident_tls::detach_current()) {
      __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
      return 0;
    }
  } catch (...) {
    // No exception may cross the PS4 module/GoldHEN C ABI.
  }
  try {
    if (entered_service) resident_service::stop();
    if (resident_tls::detach_current() && resident_tls::contexts() == 0) {
      finalize();
      if (resident_tls::shutdown()) {
        __atomic_store_n(&state, -1, __ATOMIC_RELEASE);
        return -1;
      }
    }
  } catch (...) {}
  // A loader may unload immediately after a failed start. Keep a failed module
  // mapped if cleanup could leave a thread-key callback into its code. Readiness
  // comes from the service handshake, never from module-load success alone.
  __atomic_store_n(&state, 5, __ATOMIC_RELEASE);
  return 0;
}

extern "C" __attribute__((visibility("hidden")))
int module_stop(std::size_t, const void*) {
  int expected = 2;
  if (!__atomic_compare_exchange_n(&state, &expected, 3, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    if (expected != 5 || !__atomic_compare_exchange_n(&state, &expected, 3, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return expected == 0 || expected == 4 || expected == -1 ? 0 : -1;
  }
  try {
    if (entered_service) resident_service::stop(); // Synchronously join all workers.
    if (!resident_tls::detach_current() || resident_tls::contexts() != 0) {
      __atomic_store_n(&state, 5, __ATOMIC_RELEASE);
      return -1;
    }
    finalize();
    if (!resident_tls::shutdown()) {
      __atomic_store_n(&state, 5, __ATOMIC_RELEASE);
      return -1;
    }
  } catch (...) {
    // Refuse a successful unload if a live worker or destructor could remain.
    __atomic_store_n(&state, 5, __ATOMIC_RELEASE);
    return -1;
  }
  __atomic_store_n(&state, 4, __ATOMIC_RELEASE);
  return 0;
}

extern "C" {
__attribute__((visibility("default"))) int _init(std::size_t count, const void* args) {
  return module_start(count, args);
}
__attribute__((visibility("default"))) int _fini(std::size_t count, const void* args) {
  return module_stop(count, args);
}
// GoldHEN may invoke plugin_load after _init; both entry routes are idempotent.
__attribute__((visibility("default"))) int plugin_load(std::int32_t, const char*[]) {
  return module_start(0, nullptr);
}
__attribute__((visibility("default"))) int plugin_unload(std::int32_t, const char*[]) {
  return module_stop(0, nullptr);
}
__attribute__((visibility("default"))) const char* g_pluginName = "Overdrive Resident";
__attribute__((visibility("default"))) const char* g_pluginDesc = "Torrent HTTP bridge and system package queue";
__attribute__((visibility("default"))) const char* g_pluginAuth = "Overdrive contributors";
__attribute__((visibility("default"))) std::uint32_t g_pluginVersion = 0x00000100;
}
