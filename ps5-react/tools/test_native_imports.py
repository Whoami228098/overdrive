# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from native_imports import audit_source, imports, symbols, verify_providers


@unittest.skipIf(os.name == 'nt', 'The executable fixtures use the Linux CI host compiler')
class NativeImportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        fixture = cls.root / 'fixture.cpp'
        fixture.write_text('''extern "C" int fixture_missing();
extern "C" unsigned char fixture_data;
extern "C" int fixture_good() { return fixture_missing() + fixture_data; }
extern "C" unsigned arc4random() { return 4; }
''')
        cls.library = cls.root / 'fixture.so'
        subprocess.run(['c++', '-shared', '-fPIC', str(fixture), '-o', str(cls.library)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_reads_real_elf_imports_and_data_symbols(self):
        unresolved = {s['name'] for s in imports(self.library)}
        self.assertIn('fixture_missing', unresolved)
        self.assertIn('fixture_data', unresolved)
        self.assertIn('fixture_good', {s['name'] for s in symbols(self.library) if s['defined']})

    def test_refuses_missing_providers_and_private_random_import(self):
        verify_providers([{'name': 'fixture_good'}], [self.library])
        with self.assertRaisesRegex(ValueError, 'without SDK providers'):
            verify_providers([{'name': 'fixture_missing'}], [self.library])
        with self.assertRaisesRegex(ValueError, 'WebKit random'):
            verify_providers([{'name': 'arc4random'}], [self.library])

    def test_runtime_audit_detects_null_and_calls_no_import(self):
        audit = self.root / 'audit.cpp'
        audit.write_text(audit_source([{'name': 'fixture_good', 'type': 2},
            {'name': 'fixture_missing', 'type': 2}, {'name': 'fixture_data', 'type': 1}]))
        driver = self.root / 'driver.cpp'
        driver.write_text('''#include <cstring>
unsigned called = 0; bool found = false, counted = false;
extern "C" { unsigned char fixture_data = 7; }
extern "C" void fixture_good() { ++called; }
extern "C" bool ps5_react_audit_imports(void (*)(const char*));
void log(const char* text) {
  if (std::strstr(text, "NULL symbol=fixture_missing")) found = true;
  if (std::strstr(text, "checked=3 unresolved=1")) counted = true;
}
int main() { return !ps5_react_audit_imports(log) && found && counted && called == 0 ? 0 : 1; }
''')
        executable = self.root / 'audit-test'
        subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-no-pie', '-fno-builtin',
            str(audit), str(driver), '-Wl,--defsym=fixture_missing=0', '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=10)

    def test_runtime_audit_checks_real_symbol_behind_linker_wrapper(self):
        audit = self.root / 'wrapped-audit.cpp'
        audit.write_text(audit_source([{'name': 'fixture_missing', 'type': 2}], ['fixture_missing']))
        driver = self.root / 'wrapped-driver.cpp'
        driver.write_text('''#include <cstring>
unsigned called = 0; bool found = false, counted = false;
extern "C" void __wrap_fixture_missing() { ++called; }
extern "C" bool ps5_react_audit_imports(void (*)(const char*));
void log(const char* text) {
  if (std::strstr(text, "NULL symbol=fixture_missing")) found = true;
  if (std::strstr(text, "checked=1 unresolved=1")) counted = true;
}
int main() { return !ps5_react_audit_imports(log) && found && counted && called == 0 ? 0 : 1; }
''')
        executable = self.root / 'wrapped-audit-test'
        subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-no-pie', '-fno-builtin',
            str(audit), str(driver), '-Wl,--wrap=fixture_missing', '-Wl,--defsym=fixture_missing=0',
            '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=10)

    def test_only_guarded_edid_import_is_optional(self):
        for required_missing in (False, True):
            audit = self.root / ('optional-' + str(required_missing) + '.cpp')
            audit.write_text(audit_source([{'name': 'sceVideoOutGetHdmiRawEdid_', 'type': 2},
                                           {'name': 'fixture_missing', 'type': 2}]))
            driver = self.root / 'optional-driver.cpp'
            driver.write_text('''#include <cstring>
bool optional_found = false, required_found = false;
extern "C" bool ps5_react_audit_imports(void (*)(const char*));
void log(const char* text) {
  if (std::strstr(text, "optional native import NULL symbol=sceVideoOutGetHdmiRawEdid_")) optional_found = true;
  if (std::strstr(text, "native import NULL symbol=fixture_missing")) required_found = true;
}
''' + ('' if required_missing else 'extern "C" void fixture_missing() {}\n') + '''
int main() { const bool passed = ps5_react_audit_imports(log);
''' + '  constexpr bool required_missing = ' + ('true' if required_missing else 'false') + ''';
  return optional_found && required_found == required_missing && passed == !required_missing ? 0 : 1; }
''')
            executable = self.root / ('optional-test-' + str(required_missing))
            subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-no-pie', '-fno-builtin',
                str(audit), str(driver), '-Wl,--defsym=sceVideoOutGetHdmiRawEdid_=0',
                *(['-Wl,--defsym=fixture_missing=0'] if required_missing else []), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=10)

    def test_actual_edid_call_handles_missing_and_present_import(self):
        from common import dependency
        root = Path(__file__).resolve().parents[1]
        quickjs = dependency('quickjsSource')
        for missing in (True, False):
            driver = self.root / 'edid-driver.cpp'
            driver.write_text('''#include "audio_sink.hpp"
''' + ('' if missing else '''
extern "C" int sceVideoOutGetHdmiRawEdid_(void* out, unsigned long size) {
  if (size != 513) return -1;
  auto bytes = static_cast<unsigned char*>(out);
  bytes[0] = 1; bytes[1] = 77;
  return 0;
}
''') + '''
int main() {
  std::vector<std::uint8_t> bytes;
  const bool found = host::sink_edid(bytes);
''' + ('  return !found && bytes.empty() ? 0 : 1;\n' if missing else
       '  return found && bytes.size() == 128 && bytes[0] == 77 ? 0 : 1;\n') + '}\n')
            executable = self.root / ('edid-test-' + str(missing))
            subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-no-pie',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-pthread',
                '-I' + str(root / 'native/shared'), '-I' + str(root / 'native/ps5'), '-I' + str(quickjs),
                str(root / 'native/ps5/bitstream_output.cpp'), str(driver),
                *(['-Wl,--defsym=sceVideoOutGetHdmiRawEdid_=0'] if missing else []),
                '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=10)

    def test_actual_dev_path_requires_explicit_directory_and_keeps_fallback(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / 'native/ps5/native_host.cpp').read_text()
        start = source.index('std::string dev_path(const char* name) {')
        function = source[start:source.index('\n}\n', start) + 3]
        driver = self.root / 'dev-path-driver.cpp'
        driver.write_text('''#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <climits>
#include <sys/stat.h>
#define PS5_REACT_TITLE "PPSATEST"
int mode = 0, resolved = 0;
namespace async_log { void write(const char*, ...) {} }
namespace host {
bool resolve_path(const char* logical, char* out, size_t size) {
  ++resolved;
  if (mode == 2 || std::strcmp(logical, "/app0/dev/")) return false;
  std::snprintf(out, size, "/fixture/app/dev/"); return true;
}
}
int test_stat(const char* path, struct stat* out) {
  if (std::strcmp(path, "/data/ps5-react/PPSATEST/dev/")) return -1;
  if (mode != 1 && mode != 3) return -1;
  out->st_mode = mode == 1 ? S_IFDIR : S_IFREG;
  return 0;
}
#define stat(...) test_stat(__VA_ARGS__)
''' + function + '''
int main(int argc, char** argv) {
  if (argc != 2) return 1;
  mode = std::atoi(argv[1]);
  const std::string expected = mode == 1 ? "/data/ps5-react/PPSATEST/dev/" : mode == 2 ? "" : "/fixture/app/dev/";
  const std::string first = dev_path("commands.txt");
  if (first != (expected.empty() ? "" : expected + "commands.txt")) return 2;
  const int calls = resolved;
  if (calls != (mode == 1 ? 0 : 1)) return 3;
  mode = 1;
  if (dev_path("profile.folded") != (expected.empty() ? "" : expected + "profile.folded")) return 4;
  return resolved == calls ? 0 : 5;
}
''')
        executable = self.root / 'dev-path-test'
        subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(driver), '-o', str(executable)], check=True)
        for mode in range(4):
            subprocess.run([str(executable), str(mode)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
