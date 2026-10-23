# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Materialize this PRX's local dynamic TLS indices before OpenOrbis conversion.

Only a derived ELF is changed. Original objects, archives and SDK are read-only.
No native process TCB, FS register, DTV or native TLS module IDs are involved.
"""
import ast
from pathlib import Path
import struct
import subprocess
import re
import hashlib
import base64

MODULE_TAG = 0x4f445256
TLS_RELOCATIONS = set(range(14, 24)) | {34, 35, 36}


class Elf:
    def __init__(self, path):
        self.data = bytearray(Path(path).read_bytes())
        if self.data[:6] != b'\x7fELF\x02\x01': raise ValueError('Expected little-endian ELF64')
        self.header = struct.unpack_from('<HHIQQQIHHHHHH', self.data, 16)
        if self.header[1] != 62: raise ValueError('Expected x86-64 ELF')
        self.phoff, self.shoff = self.header[4:6]
        self.phsize, self.phnum, self.shsize, self.shnum, self.shstr = self.header[8:13]
        if self.phsize != 56 or self.shsize != 64: raise ValueError('Unsupported ELF table layout')
        self.programs = [list(struct.unpack_from('<IIQQQQQQ', self.data, self.phoff + i * self.phsize)) for i in range(self.phnum)]
        self.sections = [list(struct.unpack_from('<IIQQQQIIQQ', self.data, self.shoff + i * self.shsize)) for i in range(self.shnum)]
        s = self.sections[self.shstr]
        strings = bytes(self.data[s[4]:s[4] + s[5]])
        self.names = [strings[s[0]:strings.find(b'\0', s[0])].decode('ascii') for s in self.sections]
        self.by_name = dict(zip(self.names, self.sections))
        self.symbols = {}
        self.named = {}
        for i, s in enumerate(self.sections):
            if s[1] not in (2, 11): continue
            if s[9] != 24: raise ValueError('Unsupported symbol entry size')
            strings_section = self.sections[s[6]]
            strings = bytes(self.data[strings_section[4]:strings_section[4] + strings_section[5]])
            entries = []
            for at in range(s[4], s[4] + s[5], s[9]):
                values = struct.unpack_from('<IBBHQQ', self.data, at)
                name = strings[values[0]:strings.find(b'\0', values[0])].decode('ascii')
                item = {'name': name, 'info': values[1], 'other': values[2], 'section': values[3],
                        'value': values[4], 'size': values[5], 'position': at}
                entries.append(item)
                if s[1] == 2: self.named[name] = item
            self.symbols[i] = entries

    def file_offset(self, address, size=8, allowed=None):
        for name, s in zip(self.names, self.sections):
            if s[1] == 1 and s[3] <= address and address + size <= s[3] + s[5]:
                if allowed is not None and name not in allowed: raise ValueError(f'TLS relocation targets unexpected section {name}')
                return s[4] + address - s[3]
        raise ValueError(f'Address is not file-backed: 0x{address:x}')

    def write_programs(self, programs):
        if len(programs) > self.phnum: raise ValueError('Cannot grow program table in place')
        self.data[self.phoff:self.phoff + self.phnum * self.phsize] = b'\0' * (self.phnum * self.phsize)
        for i, p in enumerate(programs): struct.pack_into('<IIQQQQQQ', self.data, self.phoff + i * self.phsize, *p)
        struct.pack_into('<H', self.data, 56, len(programs))

    def save_sections(self):
        for i, s in enumerate(self.sections): struct.pack_into('<IIQQQQIIQQ', self.data, self.shoff + i * self.shsize, *s)


def materialize(source, destination):
    elf = Elf(source)
    if elf.header[0] != 3: raise ValueError('Private TLS requires an ET_DYN shared link')
    tls = [p for p in elf.programs if p[0] == 7]
    if len(tls) != 1: raise ValueError('Expected one input PT_TLS template')
    tls = tls[0]
    if not tls[7] or tls[7] & (tls[7] - 1): raise ValueError('Invalid PT_TLS alignment')
    start = elf.named['__resident_tls_image_start']['value']
    initialized = elf.named['__resident_tls_initialized_end']['value'] - start
    total = elf.named['__resident_tls_image_end']['value'] - start
    config = elf.named['__resident_tls_config']['value']
    values = struct.unpack_from('<QQQ', elf.data, elf.file_offset(config, 24))
    if start != tls[3] or initialized != tls[5] or total != tls[6] or values != (initialized, total, tls[7]):
        raise ValueError(f'Linker TLS template/config disagree with PT_TLS: {values}, {tls}')
    accessor = elf.named.get('__tls_get_addr')
    if not accessor or not accessor['section'] or not (accessor['info'] >> 4 == 0 or accessor['other'] & 3 in (1, 2)):
        raise ValueError('Private TLS accessor must be defined and local/hidden')
    tls_symbols = []
    for table in elf.symbols.values():
        for symbol in table:
            if symbol['info'] & 15 != 6: continue
            if not symbol['section'] or not (symbol['info'] >> 4 == 0 or symbol['other'] & 3 in (1, 2)):
                raise ValueError(f'External/default-visible TLS is forbidden: {symbol["name"]}')
            if symbol['value'] < 0 or symbol['value'] + symbol['size'] > total:
                raise ValueError(f'TLS symbol outside template: {symbol["name"]}')
            tls_symbols.append(symbol)
    materialized = []
    for name, section in zip(elf.names, elf.sections):
        if section[1] != 4: continue
        if section[9] != 24: raise ValueError('Unsupported RELA entry size')
        kept = bytearray()
        table = elf.symbols[section[6]]
        old_size = section[5]
        for at in range(section[4], section[4] + old_size, 24):
            target, info, addend = struct.unpack_from('<QQq', elf.data, at)
            kind, index = info & 0xffffffff, info >> 32
            if index >= len(table): raise ValueError('Invalid relocation symbol')
            symbol = table[index]
            if kind not in TLS_RELOCATIONS:
                if symbol['info'] & 15 == 6: raise ValueError('Unexpected relocation against TLS symbol')
                if kind in (1, 6, 7) and index and symbol['section']:
                    raise ValueError('Preemptable own-symbol relocation is forbidden in the resident PRX: '+symbol['name']+' type='+str(kind))
                kept.extend(elf.data[at:at + 24]); continue
            if kind not in (16, 17) or name != '.rela.dyn':
                raise ValueError(f'Unsupported TLS relocation/model: {kind} in {name}')
            if index and (symbol['info'] & 15 != 6 or symbol not in tls_symbols):
                raise ValueError('Dynamic TLS references an external/non-TLS symbol')
            if kind == 16:
                if addend: raise ValueError('DTPMOD64 must not have an addend')
                value = MODULE_TAG
            else:
                value = (symbol['value'] if index else 0) + addend
                if not 0 <= value < total: raise ValueError('DTPOFF64 outside private image')
            offset = elf.file_offset(target, allowed={'.got', '.got.plt'})
            struct.pack_into('<Q', elf.data, offset, value)
            materialized.append({'target': target, 'type': kind, 'symbol': symbol['name'], 'value': value})
        elf.data[section[4]:section[4] + old_size] = kept + b'\0' * (old_size - len(kept))
        section[5] = len(kept)
    dynamic = elf.by_name['.dynamic']
    dynrela = elf.by_name.get('.rela.dyn')
    prefix = 0
    if dynrela:
        for at in range(dynrela[4], dynrela[4] + dynrela[5], 24):
            if struct.unpack_from('<Q', elf.data, at + 8)[0] & 0xffffffff != 8: break
            prefix += 1
    for at in range(dynamic[4], dynamic[4] + dynamic[5], 16):
        tag, value = struct.unpack_from('<QQ', elf.data, at)
        if tag == 8 and dynrela: struct.pack_into('<Q', elf.data, at + 8, dynrela[5])
        if tag == 0x6ffffff9: struct.pack_into('<Q', elf.data, at + 8, prefix)
    # The native loader sees ordinary private data, never a TLS segment/export.
    for s in elf.sections: s[2] &= ~0x400
    for symbol in tls_symbols:
        elf.data[symbol['position'] + 4] = 0 # local NOTYPE; value is only retained for diagnostics
    elf.write_programs([p for p in elf.programs if p[0] != 7])
    elf.save_sections()
    Path(destination).write_bytes(elf.data)
    result = {'mode': 'module-private pthread-key TLS', 'template': {'initialized': initialized, 'total': total, 'alignment': tls[7]},
              'tls_symbols': len(tls_symbols), 'materialized_relocations': materialized,
              'native_tls_segment': False, 'native_tls_relocations': False}
    verify_private(destination)
    return result


def verify_private(path):
    elf = Elf(path)
    if any(p[0] == 7 for p in elf.programs): raise ValueError('Native PT_TLS remains')
    for table in elf.symbols.values():
        if any(s['info'] & 15 == 6 for s in table): raise ValueError('Native TLS symbol remains')
    for s in elf.sections:
        if s[2] & 0x400: raise ValueError('SHF_TLS section remains')
        if s[1] != 4: continue
        for at in range(s[4], s[4] + s[5], 24):
            if struct.unpack_from('<Q', elf.data, at + 8)[0] & 0xffffffff in TLS_RELOCATIONS:
                raise ValueError('Native TLS relocation remains')


def normalize_oelf(source, destination):
    elf = Elf(source)
    if elf.header[0] != 0xfe18: raise ValueError('Expected SCE dynamic library')
    programs, seen, dropped = [], set(), 0
    for p in elf.programs:
        if p[0] == 7: raise ValueError('Converter introduced native TLS')
        if p[0] == 1 and tuple(p) in seen: dropped += 1; continue
        if p[0] == 1: seen.add(tuple(p))
        programs.append(p)
    elf.write_programs(programs)
    Path(destination).write_bytes(elf.data)
    return {'duplicate_load_segments_removed': dropped, 'program_headers': len(programs)}


def verify_conversion(source, converted):
    original, result = Elf(source), Elf(converted)
    expected = {}
    for s in original.sections:
        if s[1] != 4: continue
        for at in range(s[4], s[4] + s[5], 24):
            target, info, addend = struct.unpack_from('<QQq', original.data, at)
            symbol = original.symbols[s[6]][info >> 32]
            expected[target] = (info & 0xffffffff, addend, symbol['name'])
    dynamic = next(p for p in result.programs if p[0] == 2)
    tags = dict(struct.unpack_from('<QQ', result.data, at) for at in range(dynamic[2], dynamic[2] + dynamic[5], 16))
    base = next(p[2] for p in result.programs if p[0] == 0x61000000)
    strings = base + tags[0x61000035]
    symbols = base + tags[0x61000039]
    actual, imported = set(), 0
    positions = set()
    for start_tag, size_tag in ((0x6100002f, 0x61000031), (0x61000029, 0x6100002d)):
        positions.update(range(base + tags[start_tag], base + tags[start_tag] + tags[size_tag], 24))
    for at in sorted(positions):
        target, info, addend = struct.unpack_from('<QQq', result.data, at)
        kind, index = info & 0xffffffff, info >> 32
        if target not in expected: raise ValueError('Converter added an unexpected resident relocation')
        old_kind, old_addend, name = expected[target]
        if kind != old_kind or addend != old_addend: raise ValueError('Converter altered relocation type/addend')
        if kind in TLS_RELOCATIONS: raise ValueError('Converted OELF contains native TLS relocation')
        if kind in (1, 6, 7):
            name_at = strings + struct.unpack_from('<I', result.data, symbols + index * 24)[0]
            end = result.data.find(0, name_at)
            nid = bytes(result.data[name_at:end]).decode('ascii')
            digest = hashlib.sha1(name.encode() + bytes.fromhex('518D64A635DED8C1E6B039B1C3E55230')).digest()
            expected_nid = base64.b64encode(digest[:8][::-1]).decode().rstrip('=').replace('/', '-')
            if not nid.startswith(expected_nid + '#'): raise ValueError(f'Converter changed native import binding: {name} -> {nid}')
            imported += 1
        actual.add(target)
    if actual != set(expected): raise ValueError('Converter lost resident relocations: ' + repr([(hex(t), expected[t]) for t in sorted(set(expected) - actual)[:8]]))
    return {'converted_relocations_verified': len(actual), 'native_import_bindings_verified': imported,
            'native_tls_relocations': 0}


def make_self(sdk_script, source, destination):
    # Execute the supplied SDK writer with a Python 3 bytes/range adapter. SDK
    # source is read unchanged; this is the same writer, not a new SELF format.
    text = Path(sdk_script).read_text(encoding='utf-8').split('parser = MyParser', 1)[0]
    tree = ast.parse(text, filename=str(sdk_script))
    class BinaryLiterals(ast.NodeTransformer):
        def visit_Constant(self, node):
            if isinstance(node.value, str) and any(ord(c) < 32 or ord(c) == 127 for c in node.value):
                return ast.copy_location(ast.Constant(node.value.encode('latin1')), node)
            return node
    tree = ast.fix_missing_locations(BinaryLiterals().visit(tree))
    namespace = {'xrange': range, '__name__': 'overdrive_sdk_fself_writer'}
    exec(compile(tree, str(sdk_script), 'exec'), namespace)
    with Path(source).open('rb') as stream:
        elf = namespace['ElfFile'](ignore_shdrs=True)
        elf.load(stream)
    with Path(destination).open('wb') as stream:
        signed = namespace['SignedElfFile'](elf, paid=0x3800000000000011,
                                           ptype=namespace['SignedElfExInfo'].PTYPE_FAKE)
        signed.save(stream)
    return verify_self(destination, source)


def verify_self(path, source):
    data, original = Path(path).read_bytes(), Path(source).read_bytes()
    if data[:4] != b'\x4f\x15\x3d\x1d': raise ValueError('Invalid SELF magic')
    declared = struct.unpack_from('<Q', data, 16)[0]
    count = struct.unpack_from('<H', data, 24)[0]
    if declared != len(data): raise ValueError('SELF length mismatch')
    entries = [struct.unpack_from('<QQQQ', data, 32 + i * 32) for i in range(count)]
    embedded = 32 + count * 32
    ehdr = struct.unpack_from('<HHIQQQIHHHHHH', data, embedded + 16)
    original_header = list(struct.unpack_from('<HHIQQQIHHHHHH', original, 16))
    original_header[11] = 0 # Supplied SDK writer deliberately omits section headers.
    if ehdr != tuple(original_header): raise ValueError('SELF embedded ELF header mismatch')
    verified = 0
    for props, offset, size, memory in entries:
        if not props & (1 << 11): continue
        if props & ((1 << 1) | (1 << 3)): raise ValueError('Unexpected encrypted/compressed SELF segment')
        index = props >> 20 & 0xffff
        p = struct.unpack_from('<IIQQQQQQ', original, ehdr[4] + index * ehdr[8])
        if size != p[5] or data[offset:offset + size] != original[p[2]:p[2] + p[5]]:
            raise ValueError('SELF segment differs from normalized OELF')
        verified += 1
    if not verified: raise ValueError('No verified SELF load segments')
    return {'self_segments_verified': verified, 'self_declared_bytes': declared}


def audit_runtime(path, llvm, destination):
    """Check actual linked isolation, unwind references and absence of TCB use."""
    elf = Elf(path)
    allowed = {'_init', '_fini', 'plugin_load', 'plugin_unload',
               'g_pluginName', 'g_pluginDesc', 'g_pluginAuth', 'g_pluginVersion'}
    exported, imported = set(), set()
    for index, table in elf.symbols.items():
        if elf.sections[index][1] != 11: continue
        for symbol in table:
            if not symbol['section'] and symbol['name']: imported.add(symbol['name'])
            if symbol['section'] and symbol['info'] >> 4 in (1, 2) and symbol['other'] & 3 == 0:
                exported.add(symbol['name'])
    if exported != allowed: raise ValueError(f'Unexpected resident exports: {exported ^ allowed}')
    if '__tls_get_addr' in imported: raise ValueError('Private TLS accessor escaped to a native import')
    for name in ('pthread_key_create', 'pthread_key_delete', 'pthread_getspecific', 'pthread_setspecific', 'pthread_once', '__error'):
        if name not in imported: raise ValueError(f'Required native runtime import missing: {name}')
    for name in ('__libc', 'environ', '__environ', 'stdin', 'stdout', 'stderr', 'getenv', 'setlocale'):
        symbol = elf.named.get(name)
        if symbol and symbol['section'] and symbol['info'] >> 4 != 0 and symbol['other'] & 3 not in (1, 2):
            raise ValueError(f'C runtime state is not module-local: {name}')
    libc = elf.named.get('__libc')
    if not libc or struct.unpack_from('<I', elf.data, elf.file_offset(libc['value'] + 12, 4))[0] != 1:
        raise ValueError('Supplied OpenOrbis libc allocator locking contract changed')
    boundaries = {name: elf.named[name]['value'] for name in
                  ('__eh_frame_start', '__eh_frame_end', '__eh_frame_hdr_start', '__eh_frame_hdr_end')}
    if boundaries['__eh_frame_end'] <= boundaries['__eh_frame_start'] or boundaries['__eh_frame_hdr_end'] <= boundaries['__eh_frame_hdr_start']:
        raise ValueError('Resident exception frame tables are empty')
    name = '_ZN9libunwind17LocalAddressSpace18findUnwindSectionsEmRNS_18UnwindInfoSectionsE'
    function = elf.named.get(name)
    if not function or not function['size']: raise ValueError('Supplied module-local libunwind discovery implementation changed')
    section = elf.sections[function['section']]
    offset = section[4] + function['value'] - section[3]
    body = elf.data[offset:offset + function['size']]
    relative = {}
    for s in elf.sections:
        if s[1] != 4: continue
        for at in range(s[4], s[4] + s[5], 24):
            target, info, addend = struct.unpack_from('<QQq', elf.data, at)
            if info & 0xffffffff == 8: relative[target] = addend
    referenced = set()
    for at in range(len(body) - 6):
        if body[at] not in (0x48, 0x4c) or body[at + 1] not in (0x8b, 0x8d) or body[at + 2] & 0xc7 != 5: continue
        target = function['value'] + at + 7 + struct.unpack_from('<i', body, at + 3)[0]
        referenced.add(target if body[at + 1] == 0x8d else relative.get(target))
    if not set(boundaries.values()).issubset(referenced):
        raise ValueError('libunwind is not bound to all four module-local frame boundary symbols')
    if 'dl_iterate_phdr' in imported: raise ValueError('Unwinder unexpectedly depends on host module iteration')
    result = subprocess.run([str(Path(llvm) / 'llvm-objdump.exe'), '-d', str(path)], text=True,
                            encoding='utf-8', errors='replace', stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    foreign_tls = [line.strip() for line in result.stdout.splitlines() if re.search(r'%[fg]s:', line)]
    if foreign_tls: raise ValueError('Resident code still accesses native FS/GS TLS: ' + repr(foreign_tls[:8]))
    audit = {'native_tls_register_accesses': [], 'private_exports': sorted(exported),
             'native_errno': '__error', 'native_pthread_keys': True, 'libc_need_locks': 1,
             'libc_state': 'module-local allocator, environment, C locale and stdio; native pthread-owned serialized stdio; no host libc initialization',
             'key_lifecycle': 'only module-owned libc++/libc++abi keys tracked, foreign-entry values drained, remaining owned keys deleted at shutdown',
             'unwinder': 'supplied libunwind directly references all four private eh_frame boundaries; no registration/host iteration needed',
             'unwind_boundaries': boundaries, 'hardware_tested': False}
    import json
    Path(destination).write_text(json.dumps(audit, indent=2) + '\n', encoding='utf-8')
    return audit
