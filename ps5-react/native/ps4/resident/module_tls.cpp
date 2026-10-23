// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Private TLS for this PRX only. Never read/write the host process FS/TCB/DTV.
#include "module_tls.hpp"
#ifdef RESIDENT_TLS_HOST_TEST
// The host test supplies native-thread-key and allocator adapters. Production
// always uses the OpenOrbis headers/native pthread imports below.
using pthread_key_t = unsigned;
using pthread_once_t = int;
extern "C" {
int pthread_key_create(pthread_key_t*, void (*)(void*));
int pthread_key_delete(pthread_key_t);
void* pthread_getspecific(pthread_key_t);
int pthread_setspecific(pthread_key_t, const void*);
int pthread_once(pthread_once_t*, void (*)());
uintptr_t pthread_self();
int sched_yield();
void* malloc(size_t);
void free(void*);
void* memcpy(void*, const void*, size_t);
void* memset(void*, int, size_t);
[[noreturn]] void abort();
const unsigned char* resident_test_template();
const uintptr_t* resident_test_config();
}
#define PTHREAD_ONCE_INIT 0
#else
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
extern const unsigned char __resident_tls_image_start[];
extern const uintptr_t __resident_tls_config[3]; // initialized bytes, total bytes, alignment
}
#endif
extern "C" {
int __real_pthread_key_create(pthread_key_t*, void (*)(void*));
int __real_pthread_key_delete(pthread_key_t);
}

namespace {
struct Destructor { void (*call)(void*); void* value; Destructor* next; };
struct Context { void* allocation; unsigned char* image; Destructor* destructors; bool destroying; };
pthread_key_t key;
pthread_once_t once = PTHREAD_ONCE_INIT;
int key_error = 0;
int fault = 0;
int closed = 0;
size_t live = 0;
uintptr_t stdio_owner = 0;
struct OwnedKey { pthread_key_t key; void (*destroy)(void*); OwnedKey* next; };
OwnedKey* owned_keys = nullptr;
int key_lock = 0;
void lock_keys() {
  while (__atomic_exchange_n(&key_lock, 1, __ATOMIC_ACQUIRE)) sched_yield();
}
void unlock_keys() { __atomic_store_n(&key_lock, 0, __ATOMIC_RELEASE); }
bool destroy_owned_current() {
  lock_keys();
  void* value = nullptr;
  void (*callback)(void*) = nullptr;
  for (auto* item = owned_keys; item; item = item->next) {
    value = pthread_getspecific(item->key);
    if (!value) continue;
    if (pthread_setspecific(item->key, nullptr) != 0) {
      __atomic_store_n(&fault, 1, __ATOMIC_RELEASE);
      unlock_keys(); return false;
    }
    callback = item->destroy;
    break;
  }
  unlock_keys();
  if (value && callback) callback(value);
  return value != nullptr;
}
const uintptr_t* config() {
#ifdef RESIDENT_TLS_HOST_TEST
  return resident_test_config();
#else
  return __resident_tls_config;
#endif
}
const unsigned char* image_template() {
#ifdef RESIDENT_TLS_HOST_TEST
  return resident_test_template();
#else
  return __resident_tls_image_start;
#endif
}
void destroy(void* value) {
  auto* context = static_cast<Context*>(value);
  if (!context || context->destroying) return;
  context->destroying = true;
  // Native pthread clears a key before its callback. Keep this same image
  // reachable while destructors access TLS or register further destructors.
  if (pthread_setspecific(key, context) != 0) { __atomic_store_n(&fault, 1, __ATOMIC_RELEASE); return; }
  while (context->destructors) {
    auto* item = context->destructors;
    context->destructors = item->next;
    auto call = item->call;
    auto argument = item->value;
    free(item);
    call(argument);
  }
  if (pthread_setspecific(key, nullptr) != 0) { __atomic_store_n(&fault, 1, __ATOMIC_RELEASE); return; }
  free(context->allocation);
  free(context);
  __atomic_sub_fetch(&live, size_t(1), __ATOMIC_ACQ_REL);
}
void create_key() {
  const auto* values = config();
  if (!values || values[0] > values[1] || !values[2] || (values[2] & (values[2] - 1)) ||
      values[1] > size_t(-1) - values[2]) {
    key_error = -1;
    return;
  }
  key_error = __real_pthread_key_create(&key, destroy);
}
Context* current(bool create) {
  if (!resident_tls::initialize()) return nullptr;
  auto* context = static_cast<Context*>(pthread_getspecific(key));
  if (context || !create) return context;
  const auto* values = config();
  const size_t size = values[1], alignment = values[2];
  context = static_cast<Context*>(malloc(sizeof(Context)));
  if (!context) return nullptr;
  void* allocation = malloc((size ? size : 1) + alignment - 1);
  if (!allocation) { free(context); return nullptr; }
  auto address = (reinterpret_cast<uintptr_t>(allocation) + alignment - 1) & ~(uintptr_t(alignment) - 1);
  auto* image = reinterpret_cast<unsigned char*>(address);
  memset(image, 0, size);
  memcpy(image, image_template(), values[0]);
  *context = {allocation, image, nullptr, false};
  __atomic_add_fetch(&live, size_t(1), __ATOMIC_ACQ_REL);
  if (pthread_setspecific(key, context) != 0) {
    __atomic_sub_fetch(&live, size_t(1), __ATOMIC_ACQ_REL);
    free(allocation); free(context); return nullptr;
  }
  return context;
}
}

namespace resident_tls {
bool initialize() {
  return !__atomic_load_n(&closed, __ATOMIC_ACQUIRE) &&
         pthread_once(&once, create_key) == 0 && key_error == 0;
}
bool detach_current() {
  if (__atomic_load_n(&closed, __ATOMIC_ACQUIRE)) return true;
  if (!initialize()) return false;
  do {
    destroy(pthread_getspecific(key));
    if (!healthy()) return false;
    // libc++/libc++abi also create native keys (thread structs and exception
    // globals). Drain only keys created by this PRX on a foreign entry thread.
    if (destroy_owned_current()) continue;
    if (!pthread_getspecific(key)) break;
  } while (healthy());
  return healthy();
}
size_t contexts() { return __atomic_load_n(&live, __ATOMIC_ACQUIRE); }
bool healthy() { return !key_error && !__atomic_load_n(&fault, __ATOMIC_ACQUIRE); }
bool shutdown() {
  if (__atomic_load_n(&closed, __ATOMIC_ACQUIRE)) return true;
  if (!detach_current() || contexts() != 0) return false;
  // The service has joined workers, and entry state excludes concurrent calls.
  lock_keys();
  while (owned_keys) {
    auto* item = owned_keys;
    if (__real_pthread_key_delete(item->key) != 0) { unlock_keys(); return false; }
    owned_keys = item->next;
    free(item);
  }
  unlock_keys();
  if (__real_pthread_key_delete(key) != 0) return false;
  __atomic_store_n(&closed, 1, __ATOMIC_RELEASE);
  return true;
}
}

extern "C" __attribute__((visibility("hidden")))
void* __tls_get_addr(const uintptr_t* index) {
  if (!index || index[0] != resident_tls::module_tag || index[1] >= config()[1]) abort();
  auto* context = current(true);
  if (!context) abort(); // Matches the non-returning allocation-failure TLS ABI.
  return context->image + index[1];
}
extern "C" __attribute__((visibility("hidden")))
int __cxa_thread_atexit_impl(void (*call)(void*), void* value, void*) {
  if (!call) return -1;
  auto* context = current(true);
  if (!context) return -1;
  auto* item = static_cast<Destructor*>(malloc(sizeof(Destructor)));
  if (!item) return -1;
  *item = {call, value, context->destructors};
  context->destructors = item;
  return 0;
}

extern "C" __attribute__((visibility("hidden")))
int __wrap_pthread_key_create(pthread_key_t* output, void (*callback)(void*)) {
  auto* item = static_cast<OwnedKey*>(malloc(sizeof(OwnedKey)));
  if (!item) return -1;
  const int result = __real_pthread_key_create(output, callback);
  if (result != 0) { free(item); return result; }
  lock_keys();
  *item = {*output, callback, owned_keys};
  owned_keys = item;
  unlock_keys();
  return 0;
}
extern "C" __attribute__((visibility("hidden")))
int __wrap_pthread_key_delete(pthread_key_t value) {
  lock_keys();
  const int result = __real_pthread_key_delete(value);
  if (result != 0) { unlock_keys(); return result; }
  auto** current = &owned_keys;
  while (*current && (*current)->key != value) current = &(*current)->next;
  auto* item = *current;
  if (item) *current = item->next;
  unlock_keys();
  free(item);
  return 0;
}

// Supplied musl __lockfile reads a musl thread ID through FS. Serialize this
// module's stdio streams with a native pthread owner instead. Nested stdio on
// the same thread returns 0, matching musl's "already owned" helper contract.
// No private/foreign FILE layout or native host stdio objects are accessed.
extern "C" __attribute__((visibility("hidden")))
int __wrap___lockfile(void*) {
  const auto owner = reinterpret_cast<uintptr_t>(pthread_self());
  if (!owner) abort();
  if (__atomic_load_n(&stdio_owner, __ATOMIC_ACQUIRE) == owner) return 0;
  for (;;) {
    uintptr_t expected = 0;
    if (__atomic_compare_exchange_n(&stdio_owner, &expected, owner, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 1;
    sched_yield();
  }
}
extern "C" __attribute__((visibility("hidden")))
void __wrap___unlockfile(void*) {
  const auto owner = reinterpret_cast<uintptr_t>(pthread_self());
  if (__atomic_load_n(&stdio_owner, __ATOMIC_ACQUIRE) != owner) {
    __atomic_store_n(&fault, 1, __ATOMIC_RELEASE);
    return;
  }
  __atomic_store_n(&stdio_owner, uintptr_t(0), __ATOMIC_RELEASE);
}
