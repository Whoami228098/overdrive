# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Linux sanitizer/loopback test; does not run the PS5 executable or access a console."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import os
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(command, log, **kwargs):
    print('Running ' + log.name, flush=True)
    with log.open('w') as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=2400, **kwargs)
    if result.returncode:
        print(log.read_text(errors='replace')[-12000:], flush=True)
        raise RuntimeError(log.name + ' failed')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-ref', required=True)
    parser.add_argument('--minutes', type=int, default=90, choices=range(121))
    parser.add_argument('--output', type=Path, default=ROOT / '.build/torrent-soak')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    commit = subprocess.check_output(['git', 'rev-parse', args.source_ref + '^{commit}'], cwd=ROOT, text=True).strip()
    source = output / 'exact-native-source'
    source.mkdir(exist_ok=True)
    inputs = {}
    for name in ('torrent_api.cpp', 'torrent_api.hpp'):
        path = 'ps5-react/native/shared/' + name
        data = subprocess.check_output(['git', 'show', commit + ':' + path], cwd=ROOT)
        (source / name).write_bytes(data)
        inputs[path] = hashlib.sha256(data).hexdigest()
    lock = ROOT / 'native/ps5/torrent/dependencies.lock.json'
    pinned = subprocess.check_output(['git', 'show', commit + ':ps5-react/native/ps5/torrent/dependencies.lock.json'], cwd=ROOT)
    if lock.read_bytes() != pinned:
        raise RuntimeError('Test dependency pins differ from the requested native source')
    spec = importlib.util.spec_from_file_location('torrent_builder', ROOT / 'native/ps5/torrent_build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    upstream, boost = module.inputs(ROOT / '.deps/torrent-ps5')
    project = output / 'project'
    project.mkdir(exist_ok=True)
    harness = ROOT / 'tools/tests/torrent_soak.cpp'
    (project / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(overdrive_native_soak LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(flag build_tests build_examples build_tools build_benchmark deprecated-functions logging)
  set(${flag} OFF CACHE BOOL "" FORCE)
endforeach()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer -DTORRENT_BROKEN_RANDOM_DEVICE=1")
add_link_options(-fsanitize=address,undefined)
add_subdirectory("''' + str(upstream) + '''" upstream)
add_executable(native-soak "''' + str(source / 'torrent_api.cpp') + '''" "''' + str(harness) + '''")
target_include_directories(native-soak PRIVATE "''' + str(source) + '''")
target_compile_definitions(native-soak PRIVATE TORRENT_MINIMAL=1)
target_link_libraries(native-soak PRIVATE torrent-rasterbar)
''')
    build = output / 'build'
    run(['cmake', '-S', str(project), '-B', str(build), '-G', 'Ninja',
         '-DCMAKE_C_COMPILER=clang-18', '-DCMAKE_CXX_COMPILER=clang++-18',
         '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -gline-tables-only -DNDEBUG',
         '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DBoost_INCLUDE_DIR=' + str(boost),
         '-DBOOST_INCLUDEDIR=' + str(boost)], output / 'configure.log')
    run(['cmake', '--build', str(build), '--parallel', '2'], output / 'compile.log')
    manifest = {'native_source_commit': commit, 'native_sources': inputs,
        'test_code_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'test_harness_sha256': sha(harness), 'dependency_lock_sha256': sha(lock),
        'pinned_libtorrent': json.loads(pinned)['libtorrent'], 'minutes_requested': args.minutes,
        'platform': 'Linux host', 'sanitizers': ['address', 'undefined'], 'hardware_tested': False,
        'openssl_host_version': subprocess.check_output(['openssl', 'version'], text=True).strip()}
    (output / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    environment = dict(os.environ,
        ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:quarantine_size_mb=64:thread_local_quarantine_size_kb=1024',
        UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    started = time.monotonic()
    records = []
    command = [str(build / 'native-soak'), str(args.minutes), str(output / 'sandbox')]
    print('Starting native host cases and timed soak', flush=True)
    with (output / 'soak.log').open('w') as log, subprocess.Popen(command, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, env=environment, text=True, bufsize=1) as process:
        for line in process.stdout:
            log.write(line)
            log.flush()
            print(line, end='', flush=True)
            if line.startswith(('METRICS ', 'RESULT ')):
                records.append(json.loads(line.split(' ', 1)[1]))
            if time.monotonic() - started > args.minutes * 60 + 600:
                process.kill()
                raise RuntimeError('Native soak exceeded its time allowance')
        code = process.wait(timeout=60)
    summary = {**manifest, 'process_seconds': time.monotonic() - started,
        'exit_code': code, 'progress': records, 'passed': code == 0 and bool(records)}
    if records:
        summary['timed_seconds'] = records[-1]['elapsed_seconds']
        summary['passed'] &= summary['timed_seconds'] >= args.minutes * 60
    (output / 'soak-result.json').write_text(json.dumps(summary, indent=2) + '\n')
    if not summary['passed']:
        raise RuntimeError('Native host regression/soak failed; inspect soak.log')
    print(json.dumps({k: v for k, v in summary.items() if k != 'progress'}), flush=True)


if __name__ == '__main__':
    main()
