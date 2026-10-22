# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the real GC scheduler against the pinned allocator's byte contract."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

QUICKJS = r'''
#pragma once
#include <stddef.h>
typedef struct JSRuntime { size_t threshold; unsigned collections; } JSRuntime;
typedef struct JSMallocFunctions {
 void* (*js_calloc)(void*, size_t, size_t);
 void* (*js_malloc)(void*, size_t);
 void (*js_free)(void*, void*);
 void* (*js_realloc)(void*, void*, size_t);
 size_t (*js_malloc_usable_size)(const void*);
} JSMallocFunctions;
size_t JS_GetGCThreshold(JSRuntime*);
void JS_SetGCThreshold(JSRuntime*, size_t);
void JS_RunGC(JSRuntime*);
'''

DRIVER = r'''
#include "js_heap.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>

constexpr size_t MiB = 1024 * 1024;
#ifdef __APPLE__
constexpr size_t overhead = 0;
#else
constexpr size_t overhead = 8;
#endif
bool fail_realloc = false;
size_t usable(const void* p) { return p ? static_cast<const size_t*>(p)[-1] : 0; }
void* alloc(void*, size_t n) {
 auto p = static_cast<size_t*>(std::malloc(n + sizeof(size_t)));
 if (!p) return nullptr;
 *p = n; return p + 1;
}
void* zero(void* o, size_t n, size_t size) { return alloc(o, n * size); }
void free_block(void*, void* p) { if (p) std::free(static_cast<size_t*>(p) - 1); }
void* resize(void* o, void* p, size_t n) {
 if (fail_realloc) return nullptr;
 if (!n) { free_block(o, p); return nullptr; }
 void* next = alloc(o, n);
 if (next) free_block(o, p);
 return next;
}
JSMallocFunctions inner = {zero, alloc, free_block, resize, usable};
extern "C" const JSMallocFunctions* er_js_default_malloc_functions() { return &inner; }
extern "C" size_t JS_GetGCThreshold(JSRuntime* r) { return r->threshold; }
extern "C" void JS_SetGCThreshold(JSRuntime* r, size_t n) { r->threshold = n; }
extern "C" void JS_RunGC(JSRuntime* r) { ++r->collections; }
int64_t clock_us() { return 1000000; }
void expect_accounting(size_t runtime_bytes) {
 JSRuntime rt = {256 * 1024, 0};
 GcScheduler gc(clock_us);
 assert(gc.frame(&rt, 0, 0, 1000) == nullptr);
 assert(rt.threshold == runtime_bytes + 16 * MiB);
}
int main(int argc, char** argv) {
 assert(argc == 2);
 const auto* f = js_heap_functions();
 if (!std::strcmp(argv[1], "accounting")) {
   void* a = f->js_malloc(nullptr, 100), *b = f->js_calloc(nullptr, 2, 80);
   expect_accounting(260 + 2 * overhead);
   a = f->js_realloc(nullptr, a, 300);
   expect_accounting(460 + 2 * overhead);
   f->js_free(nullptr, a); f->js_free(nullptr, b);
   expect_accounting(0);
 } else if (!std::strcmp(argv[1], "realloc")) {
   void* p = f->js_realloc(nullptr, nullptr, 100);
   fail_realloc = true;
   assert(!f->js_realloc(nullptr, p, 200));
   expect_accounting(100 + overhead);
   fail_realloc = false;
   assert(!f->js_realloc(nullptr, p, 0));
   expect_accounting(0);
 } else if (!std::strcmp(argv[1], "catalog")) {
   void* p = f->js_malloc(nullptr, 58 * MiB);
   JSRuntime rt = {256 * 1024, 0};
   GcScheduler gc(clock_us);
   gc.frame(&rt, 0, 0, 1000);
   assert(rt.threshold >= 58 * MiB + overhead + 16 * MiB);
   assert(rt.threshold < kJsMemoryLimit);
   gc.input(100);
   for (int i = 0; i < 100; i++) gc.frame(&rt, 100 + i, 1000, 1000);
   assert(rt.collections == 0);
   f->js_free(nullptr, p);
 } else if (!std::strcmp(argv[1], "idle")) {
   void* a = f->js_malloc(nullptr, MiB);
   JSRuntime rt = {256 * 1024, 0};
   GcScheduler gc(clock_us);
   gc.frame(&rt, 0, 0, 1000);
   void* b = f->js_malloc(nullptr, 6 * MiB);
   gc.input(100);
   assert(!gc.frame(&rt, 300000, 0, 1000));
   assert(rt.collections == 0);
   assert(gc.frame(&rt, 600000, 0, 1000));
   assert(rt.collections == 1);
   assert(rt.threshold > 7 * MiB + 2 * overhead);
   f->js_free(nullptr, a); f->js_free(nullptr, b);
 } else if (!std::strcmp(argv[1], "ceiling")) {
   void* p = f->js_malloc(nullptr, 58 * MiB);
   JSRuntime rt = {200 * MiB, 0};
   GcScheduler gc(clock_us);
   gc.frame(&rt, 0, 0, 1000);
   assert(rt.threshold == kJsMemoryLimit - kJsMemoryLimit / 8);
   rt.threshold = 200 * MiB;
   const char* log = gc.frame(&rt, 1, 0, 1000);
   assert(log && std::strstr(log, "automatic"));
   assert(rt.threshold > 58 * MiB + overhead);
   f->js_free(nullptr, p);
 } else return 2;
}
'''


@unittest.skipIf(os.name == 'nt', 'Executable fixtures run on the Linux CI host')
class JsHeapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        (root / 'quickjs.h').write_text(QUICKJS)
        (root / 'er_js_alloc.h').write_text('#include "quickjs.h"\nconst JSMallocFunctions* er_js_default_malloc_functions();\n')
        (root / 'driver.cpp').write_text(DRIVER)
        cls.exe = root / 'js-heap-test'
        subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
            '-I', str(root), '-I', str(ROOT / 'native/shared'),
            str(root / 'driver.cpp'), str(ROOT / 'native/shared/js_heap.cpp'), '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_accounting_includes_each_block_overhead(self):
        subprocess.run([self.exe, 'accounting'], check=True, timeout=10)

    def test_failed_resize_and_zero_size_preserve_accounting(self):
        subprocess.run([self.exe, 'realloc'], check=True, timeout=10)

    def test_large_catalog_has_interactive_headroom(self):
        subprocess.run([self.exe, 'catalog'], check=True, timeout=10)

    def test_input_defers_collection_until_idle(self):
        subprocess.run([self.exe, 'idle'], check=True, timeout=10)

    def test_automatic_collection_threshold_is_bounded(self):
        subprocess.run([self.exe, 'ceiling'], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
