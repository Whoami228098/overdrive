// Native host harness only: adapters are supplied by the Python test process.
#include "module_tls.hpp"
extern "C" {
struct Callbacks {
  void* (*allocate)(size_t);
  void (*release)(void*);
  int (*key_create)(unsigned*, void (*)(void*));
  int (*key_delete)(unsigned);
  void* (*key_get)(unsigned);
  int (*key_set)(unsigned, const void*);
  int (*once)(int*, void (*)());
  uintptr_t (*self)();
  int (*yield)();
  void (*fatal)();
};
static Callbacks callbacks;
static unsigned char image[8] = {1,2,3,4,5,6,7,8};
static uintptr_t config[3] = {8, 96, 64};
void test_configure(const Callbacks* value) { callbacks = *value; }
void* malloc(size_t size) { return callbacks.allocate(size); }
void free(void* value) { callbacks.release(value); }
void* memcpy(void* destination, const void* source, size_t count) {
  auto* out = static_cast<unsigned char*>(destination);
  const auto* in = static_cast<const unsigned char*>(source);
  for (size_t i = 0; i < count; ++i) out[i] = in[i];
  return destination;
}
void* memset(void* destination, int value, size_t count) {
  auto* out = static_cast<unsigned char*>(destination);
  for (size_t i = 0; i < count; ++i) out[i] = static_cast<unsigned char>(value);
  return destination;
}
[[noreturn]] void abort() { callbacks.fatal(); for (;;) {} }
int pthread_key_create(unsigned* key, void (*destroy)(void*)) { return callbacks.key_create(key, destroy); }
int pthread_key_delete(unsigned key) { return callbacks.key_delete(key); }
int __real_pthread_key_create(unsigned* key, void (*destroy)(void*)) { return callbacks.key_create(key, destroy); }
int __real_pthread_key_delete(unsigned key) { return callbacks.key_delete(key); }
void* pthread_getspecific(unsigned key) { return callbacks.key_get(key); }
int pthread_setspecific(unsigned key, const void* value) { return callbacks.key_set(key, value); }
int pthread_once(int* state, void (*call)()) { return callbacks.once(state, call); }
uintptr_t pthread_self() { return callbacks.self(); }
int sched_yield() { return callbacks.yield(); }
const unsigned char* resident_test_template() { return image; }
const uintptr_t* resident_test_config() { return config; }
void* test_access(uintptr_t offset) { const uintptr_t index[2] = {resident_tls::module_tag, offset}; return __tls_get_addr(index); }
int test_register(void (*call)(void*), void* value) { return __cxa_thread_atexit_impl(call, value, nullptr); }
int test_initialize() { return resident_tls::initialize(); }
int test_detach() { return resident_tls::detach_current(); }
size_t test_contexts() { return resident_tls::contexts(); }
int test_shutdown() { return resident_tls::shutdown(); }
int __wrap___lockfile(void*);
void __wrap___unlockfile(void*);
int test_stdio_lock() { return __wrap___lockfile(nullptr); }
void test_stdio_unlock() { __wrap___unlockfile(nullptr); }
int __wrap_pthread_key_create(unsigned*, void (*)(void*));
int __wrap_pthread_key_delete(unsigned);
unsigned test_owned_key_create(void (*call)(void*)) {
  unsigned key;
  return __wrap_pthread_key_create(&key, call) == 0 ? key : unsigned(-1);
}
int test_key_set(unsigned key, const void* value) { return pthread_setspecific(key, value); }
int test_owned_key_delete(unsigned key) { return __wrap_pthread_key_delete(key); }
}
