# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate a non-calling runtime check of the final native import addresses."""
from pathlib import Path
import json
import re
import struct


def symbols(path):
    data = Path(path).read_bytes()
    if data[:6] != b'\x7fELF\x02\x01' or len(data) < 64:
        raise ValueError('Expected a little-endian ELF64: ' + str(path))
    section_at = struct.unpack_from('<Q', data, 40)[0]
    width, count = struct.unpack_from('<HH', data, 58)
    if width != 64 or not count or section_at + count * width > len(data):
        raise ValueError('Missing or truncated ELF section table: ' + str(path))
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, section_at + i * width) for i in range(count)]
    result = []
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM
            continue
        if section[9] != 24 or section[5] % 24 or section[6] >= count:
            raise ValueError('Invalid dynamic symbol table')
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        entries = data[section[4]:section[4] + section[5]]
        if len(entries) != section[5] or len(names) != strings[5]:
            raise ValueError('Truncated dynamic symbols or strings')
        for offset in range(24, len(entries), 24):
            name, info, _, index, value, size = struct.unpack_from('<IBBHQQ', entries, offset)
            if name >= len(names) or b'\0' not in names[name:]:
                raise ValueError('Invalid dynamic symbol name')
            result.append({'name': names[name:names.index(0, name)].decode('utf-8'),
                           'binding': info >> 4, 'type': info & 15,
                           'defined': index != 0, 'value': value, 'size': size})
    return result


def imports(path):
    return sorted((s for s in symbols(path) if not s['defined']), key=lambda s: s['name'])


def audit_source(entries):
    """No imported function is called. Volatile reads keep null checks observable."""
    lines = ['#include <cstdio>', 'using Address = void (*)();',
             'struct Import { const char* name; Address volatile address; };']
    for i, symbol in enumerate(entries):
        name = symbol['name']
        if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.$@]*', name):
            raise ValueError('Unsupported native import name: ' + name)
        declaration = f'void import_{i}()' if symbol['type'] != 1 else f'unsigned char import_{i}'
        lines.append(f'extern "C" {declaration} asm({json.dumps(name)});')
    lines.append('static const Import imports[] = {')
    for i, symbol in enumerate(entries):
        lines.append('  {' + json.dumps(symbol['name']) + f', reinterpret_cast<Address>(&import_{i})' + '},')
    # The initial link uses a sentinel; it is replaced by the complete table before conversion.
    lines += ['  {nullptr, nullptr}', '};',
              'extern "C" bool ps5_react_audit_imports(void (*log)(const char*)) {',
              '  unsigned checked = 0, missing = 0; char line[256];',
              '  for (const auto& entry : imports) {',
              '    if (!entry.name) break;',
              '    ++checked;',
              '    if (entry.address != nullptr) continue;',
              '    ++missing;',
              '    std::snprintf(line, sizeof line, "native import NULL symbol=%s", entry.name);',
              '    log(line);', '  }',
              '  std::snprintf(line, sizeof line, "native imports checked=%u unresolved=%u", checked, missing);',
              '  log(line);', '  return missing == 0;', '}', '']
    return '\n'.join(lines)


def verify_providers(entries, stubs):
    exports = set()
    for stub in stubs:
        exports.update(s['name'] for s in symbols(stub) if s['defined'])
    missing = [s['name'] for s in entries if s['name'] not in exports]
    if missing:
        raise ValueError('Native imports without SDK providers: ' + ', '.join(missing))
    banned = {'arc4random', 'arc4random_buf'} & {s['name'] for s in entries}
    if banned:
        raise ValueError('Private WebKit random import is forbidden: ' + ', '.join(sorted(banned)))
