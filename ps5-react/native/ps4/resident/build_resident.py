# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Build a separate OpenOrbis PRX; never load or upload anything to a console.

prepare(args, env, run, build, torrent_config) uses service.cpp, log.cpp and a
generated torrent engine passed as torrent_config['resident_engine_source'].
The returned dictionary contains prx, sources, libraries, licenses and receipt.
The standalone --probe mode is only a link/SELF diagnostic, not a working service.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import hashlib
import json
import os
import struct
import subprocess
import importlib.util


def digest(file):
    with Path(file).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def textpath(value):
    return value.as_posix() if isinstance(value, Path) else str(value)


def inspect_elf(file):
    data = Path(file).read_bytes()
    if data[:5] != b'\x7fELF\x02' or data[5] != 1:
        raise RuntimeError(f'Expected a little-endian ELF64 file: {file}')
    header = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    kind, machine = header[:2]
    phoff, shoff = header[4:6]
    phsize, phnum, shsize, shnum, shstr = header[8:13]
    programs = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phsize) for i in range(phnum)]
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shsize) for i in range(shnum)]
    names = sections[shstr]
    strings = data[names[4]:names[4] + names[5]]
    def name(at): return strings[at:strings.find(b'\0', at)].decode('ascii')
    by_name = {name(s[0]): s for s in sections}
    symbols = {}
    for s in sections:
        if s[1] != 2: continue
        strings_section = sections[s[6]]
        symbol_strings = data[strings_section[4]:strings_section[4] + strings_section[5]]
        for at in range(s[4], s[4] + s[5], s[9]):
            noff, info, other, section, value, size = struct.unpack_from('<IBBHQQ', data, at)
            symbols[symbol_strings[noff:symbol_strings.find(b'\0', noff)].decode('ascii')] = value
    lock_word = None
    if '__libc' in symbols:
        address = symbols['__libc'] + 12
        for s in sections:
            if s[1] == 1 and s[3] <= address and address + 4 <= s[3] + s[5]:
                lock_word = struct.unpack_from('<I', data, s[4] + address - s[3])[0]
                break
    relocations = []
    for s in sections:
        if s[1] != 4: continue
        for at in range(s[4], s[4] + s[5], s[9]):
            offset, info, addend = struct.unpack_from('<QQq', data, at)
            relocations.append({'offset': offset, 'type': info & 0xffffffff,
                                'symbol': info >> 32, 'addend': addend})
    dynamic = []
    if '.dynamic' in by_name:
        s = by_name['.dynamic']
        dynamic = [struct.unpack_from('<QQ', data, at) for at in range(s[4], s[4] + s[5], 16)]
    return {'type': kind, 'machine': machine, 'entry': header[3],
            'tls': [{'file_size': p[5], 'memory_size': p[6], 'alignment': p[7]} for p in programs if p[0] == 7],
            'module_param': by_name.get('.data.sce_module_param', [0] * 6)[5],
            'module_libc_lock_word_at_plus_12': lock_word,
            'static_tls_accessor_linked': bool(symbols.get('__tls_get_addr')),
            'init_bytes': symbols.get('__resident_init_end', 0) - symbols.get('__resident_init_start', 0),
            'fini_bytes': symbols.get('__resident_fini_end', 0) - symbols.get('__resident_fini_start', 0),
            'dynamic_tags': dict(dynamic), 'relocation_types': sorted({r['type'] for r in relocations}),
            'tls_relocations': [r for r in relocations if r['type'] in (16, 17, 18, 36)]}


def prepare(args, env, run, build, torrent_config):
    here = Path(__file__).resolve().parent
    platform = here.parent
    root = platform.parents[1]
    destination = Path(build) / 'resident'
    destination.mkdir(parents=True, exist_ok=True)
    probe_source = torrent_config.get('resident_probe_source')
    if probe_source:
        sources = [here / 'module_entry.cpp', Path(probe_source), here / 'module_tls.cpp']
    else:
        engine = torrent_config.get('resident_engine_source')
        if not engine:
            raise RuntimeError('Resident build needs the generated streaming engine source')
        sources = [here / 'module_entry.cpp', here / 'service.cpp', here / 'log.cpp', Path(engine),
                   platform / 'network_platform.cpp',
                   here / 'module_tls.cpp', platform / 'time_compat.c',platform/'app_identity.c',here/'archive_platform.cpp',here/'archive_no_program.c',here/'archive_stdio.c',
                   *[root/'native/shared'/name for name in ['archives.cpp','archive_writer.cpp','archive_preflight.cpp','rar5_password.cpp']]]
    archive_config={}
    if not probe_source:
        spec=importlib.util.spec_from_file_location('ps4_archive_ports',platform/'archives/ports.py')
        builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
        archive_config=builder.prepare(args,env,run,build)
    for source in sources:
        if not source.is_file(): raise RuntimeError(f'Resident source is missing: {source}')
    sdk, llvm, dev = args.toolchain, args.llvm, args.dev_root
    includes = [*archive_config.get('includes',[]),here, platform, root / 'native/shared', *torrent_config['includes']]
    flags = ['--target=x86_64-pc-freebsd12-elf', '-fPIC', '-funwind-tables', '-ffunction-sections',
             '-fdata-sections', '-ftls-model=global-dynamic', '-fvisibility=hidden', '-O2', '-DPS4=1', '-D__Orbis__=1', '-DOO=1', '-D_BSD_SOURCE=1',
             '-D_POSIX_C_SOURCE=200809L', '-DPS5_REACT_TORRENT=1', '-DOVERDRIVE_RESIDENT=1',
             *torrent_config['definitions'], '-isysroot', sdk, '-isystem', sdk / 'include',
             '-isystem', sdk / 'include/c++/v1', '-isystem', dev / 'SDL-PS4/ps4compat',
             *[piece for directory in includes for piece in ('-I', directory)]]
    inputs = sorted({p for directory in (platform, root / 'native/shared')
                     for pattern in ('*.h', '*.hpp', '*.inc') for p in directory.rglob(pattern)})
    header_hash = hashlib.sha256(b''.join(p.read_bytes() for p in inputs)).hexdigest()
    def compile_one(item):
        index, source = item
        obj = destination / f'{index:02}-{source.name}.o'
        cpp = source.suffix == '.cpp'
        command = [llvm / ('clang++.exe' if cpp else 'clang.exe'), *flags,
                   *(['-std=c++20', '-fexceptions', '-frtti'] if cpp else ['-std=c11']),
                   '-Wall', '-Wextra', '-Werror=return-type', '-c', source, '-o', obj]
        key = hashlib.sha256((json.dumps(list(map(textpath, command))) + header_hash + digest(source)).encode()).hexdigest()
        stamp = obj.with_suffix('.sha256')
        if getattr(args, 'rebuild', False) or not obj.exists() or not stamp.exists() or stamp.read_text() != key:
            run(command, destination / f'compile-{index:02}.log', env)
            stamp.write_text(key)
        return obj
    with ThreadPoolExecutor(max_workers=getattr(args, 'jobs', 4)) as workers:
        objects = list(workers.map(compile_one, enumerate(sources)))
    libraries = [Path(torrent_config['library']),*archive_config.get('libraries',[]), sdk / 'lib/libc++.a', sdk / 'lib/libc++abi.a',
                 sdk / 'lib/libpthread.a', sdk / 'lib/libclang_rt.builtins-x86_64.a', sdk / 'lib/libc.a']
    elf = destination / 'overdrive-resident.elf'
    exports = destination / 'module-exports.map'
    exports.write_text('{ global: _init; _fini; plugin_load; plugin_unload; '
                       'g_pluginName; g_pluginDesc; g_pluginAuth; g_pluginVersion; local: *; };\n', encoding='utf-8')
    # A PRX is a shared object. -pie permits TLS relaxation to fixed executable
    # offsets, which is invalid when this module is loaded into an existing process.
    options = ['-m', 'elf_x86_64', '-shared', '--script', here / 'module_link.x', '-e', '_init',
               '--init=_init', '--fini=_fini', '--eh-frame-hdr', '--gc-sections', '--no-undefined',
               '--exclude-libs=ALL','-Bsymbolic-functions', '--version-script', exports, '--wrap=__cxa_atexit', '--wrap=__cxa_finalize',
               '--wrap=__lockfile', '--wrap=__unlockfile','--wrap=fputc',
               '--wrap=pthread_key_create', '--wrap=pthread_key_delete',
               '-L', sdk / 'lib', '-o', elf, *objects, '--start-group', *libraries,
               '--end-group', '-lSceSysUtil', '-lSceBgft', '-lSceSysmodule', '-lSceSystemService',
               '-lSceAppInstUtil', '-lSceNet', '-lSceNetCtl', '-lSceUserService', '-lkernel']
    response = destination / 'link.rsp'
    response.write_text('\n'.join('"' + textpath(p).replace('"', '\\"') + '"' for p in options), encoding='utf-8')
    run([llvm / 'ld.lld.exe', '@' + textpath(response)], destination / 'link.log', env)
    run([llvm / 'llvm-readobj.exe', '--file-headers', '--program-headers', '--sections', '--dynamic-table',
         '--needed-libs', '--dyn-symbols', '--relocations', elf], destination / 'elf-inspection.txt', env)
    diagnostics = inspect_elf(elf)
    if diagnostics['type'] != 3 or diagnostics['machine'] != 62 or diagnostics['module_param'] != 24:
        raise RuntimeError('Resident ELF is not an x86-64 shared PRX with module metadata')
    if 25 in diagnostics['dynamic_tags'] or 26 in diagnostics['dynamic_tags']:
        raise RuntimeError('PRX must run C++ init/fini arrays only through module_entry.cpp')
    if any(r['type'] in (18, 36) for r in diagnostics['tls_relocations']):
        raise RuntimeError('Resident ELF contains unsupported executable/static TLS or TLSDESC relocations')
    spec = importlib.util.spec_from_file_location('resident_private_tls', here / 'private_tls.py')
    tls_builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tls_builder)
    runtime_audit = tls_builder.audit_runtime(elf, llvm, destination / 'runtime-audit.json')
    private_elf = destination / 'overdrive-resident.private.elf'
    private_receipt = tls_builder.materialize(elf, private_elf)
    private_diagnostics = inspect_elf(private_elf)
    if private_diagnostics['tls'] or private_diagnostics['tls_relocations']:
        raise RuntimeError('Private TLS conversion retained native TLS state')
    oelf, prx = destination / 'overdrive-resident.oelf', destination / 'overdrive-resident.prx'
    converted = destination / 'overdrive-resident.converter.oelf'
    run([sdk / 'bin/windows/create-fself.exe', f'-in={textpath(private_elf)}', f'-out={textpath(converted)}',
         f'--lib={textpath(destination / "overdrive-resident.converter.prx")}', '--libname=overdrive-resident', '--paid', '0x3800000000000011'],
        destination / 'fself.log', env)
    normalization = tls_builder.normalize_oelf(converted, oelf)
    conversion = tls_builder.verify_conversion(private_elf, oelf)
    signed = tls_builder.make_self(sdk / 'scripts/make_fself.py', oelf, prx)
    if prx.read_bytes()[:4] != b'\x4f\x15\x3d\x1d':
        raise RuntimeError('create-fself did not produce a valid SELF header')
    run([llvm / 'llvm-readobj.exe', '--file-headers', '--program-headers', oelf],
        destination / 'oelf-inspection.txt', env)
    receipt = {'hardware_tested': False, 'probe_only': bool(probe_source), 'production_build_enabled': True,
               'runtime_blockers': [], 'strict_undefined_check': True,
               'module_lifecycle': 'explicit C++ constructors, joined service stop, scoped finalizers',
               'cxx_startup_probe': 'within-module throw/catch sentinel before service readiness',
               'link_type': 'shared', 'diagnostics': diagnostics, 'private_elf_diagnostics': private_diagnostics,
               'private_tls': private_receipt, 'oelf_normalization': normalization, 'self_verification': signed,
               'conversion_verification': conversion,
               'runtime_audit': runtime_audit,
               'prx': {'bytes': prx.stat().st_size, 'sha256': digest(prx)},
               'sources': {textpath(p): digest(p) for p in [*sources, *inputs, here / 'module_link.x', here / 'private_tls.py', Path(__file__)]},
               'libraries': {textpath(p): digest(p) for p in libraries},
               'sdk_fself_writer_sha256': digest(sdk / 'scripts/make_fself.py'),
               'runtime_validation': 'private TLS structurally verified; exact native loader/service execution still needs console evidence'}
    (destination / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    return {'prx': prx, 'sources': sources, 'libraries': libraries, 'receipt': receipt,
            'licenses': {**torrent_config.get('licenses', {}),**archive_config.get('licenses',{}), 'OpenOrbis-LICENSE': sdk / 'LICENSE'},
            'archive_config':archive_config.get('receipt')}


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True, help='Explicit temporary service source; no production service')
    parser.add_argument('--dev-root', type=Path, default=Path(__file__).resolve().parents[5])
    args = parser.parse_args()
    args.toolchain = args.dev_root / 'OpenOrbis/OpenOrbis/PS4Toolchain'
    args.llvm = args.dev_root / 'LLVM18/bin'
    args.jobs = 4
    args.rebuild = False
    repository = Path(__file__).resolve().parents[4]
    work = repository / 'work/resident-private-tls-probe'
    work.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, 'OO_PS4_TOOLCHAIN': textpath(args.toolchain), 'PYTHONUTF8': '1'}
    def run(command, log, environment):
        with Path(log).open('w', encoding='utf-8') as output:
            result = subprocess.run(list(map(textpath, command)), env=environment, stdout=output, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f'Command failed: {log}\n' + Path(log).read_text(encoding='utf-8', errors='replace')[-8000:])
    config = json.loads((repository / 'work/torrent-port/build-receipt.json').read_text())
    config.update(library=repository / 'work/torrent-port/build/libtorrent.a',
                  includes=[repository / 'work/torrent-port/libtorrent-rasterbar-2.0.10/include',
                            args.dev_root / 'torrent-ps5/src/boost-1.84.0'], resident_probe_source=args.probe)
    result = prepare(args, env, run, work, config)
    print(json.dumps({'prx': textpath(result['prx']), 'receipt': result['receipt']}, indent=2))
