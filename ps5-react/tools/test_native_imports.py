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


if __name__ == '__main__':
    unittest.main()
