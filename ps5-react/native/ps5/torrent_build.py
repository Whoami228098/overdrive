"""Pinned, reproducible libtorrent build for the existing PS5 app pipeline.

No console access and no new package-manager dependencies. The caller supplies
the same SDK, OpenSSL ports, LLVM environment and build directory as the app.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
MANIFEST = json.loads((HERE / 'torrent/dependencies.lock.json').read_text())

def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def fetch(name, cache):
    spec = MANIFEST[name]
    archive = cache / (name + ('.tar.xz' if name == 'boost' else '.tar.gz'))
    if archive.exists() and sha256(archive) == spec['sha256']:
        return archive
    request = urllib.request.Request(spec['url'], headers={'User-Agent': 'Overdrive-pinned-build/1'})
    temporary = archive.with_suffix(archive.suffix + '.tmp')
    with urllib.request.urlopen(request, timeout=120) as response, temporary.open('wb') as out:
        shutil.copyfileobj(response, out)
    if sha256(temporary) != spec['sha256']:
        raise RuntimeError(f'{name}: archive SHA-256 differs from the pinned input')
    temporary.replace(archive)
    return archive

def inputs(cache):
    cache.mkdir(parents=True, exist_ok=True)
    source = cache / 'libtorrent-rasterbar-2.0.10'
    boost = cache / 'boost-1.84.0'
    receipt = cache / 'inputs.json'
    if receipt.exists() and json.loads(receipt.read_text()) == MANIFEST and (source / 'CMakeLists.txt').is_file() and (boost / 'boost/asio.hpp').is_file():
        return source, boost
    with tarfile.open(fetch('libtorrent', cache)) as archive:
        archive.extractall(cache, filter='data')
    with tarfile.open(fetch('boost', cache), 'r|xz') as archive:
        for member in archive:
            if not member.isfile():
                continue
            if '/include/boost/' in member.name:
                relative = 'boost/' + member.name.split('/include/boost/', 1)[1]
            elif member.name.startswith('boost-1.84.0/boost/'):
                relative = member.name.split('/', 1)[1]
            elif member.name == 'boost-1.84.0/LICENSE_1_0.txt':
                relative = 'LICENSE_1_0.txt'
            else:
                continue
            if '..' in Path(relative).parts:
                raise RuntimeError('Unexpected Boost archive path')
            path = boost / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(archive.extractfile(member).read())
    receipt.write_text(json.dumps(MANIFEST, indent=2) + '\n')
    return source, boost

def prepare(sdk, ports, env, build):
    minimal = env.get('TORRENT_MINIMAL', '0')
    if minimal not in ('0', '1'):
        raise ValueError('TORRENT_MINIMAL must be 0 or 1')
    sdk, ports, build = map(Path, (sdk, ports, build))
    cache = ROOT / '.deps/torrent-ps5'
    source, boost = inputs(cache)
    destination = build / 'torrent'
    destination.mkdir(parents=True, exist_ok=True)
    cmake = ['cmake', '-S', str(HERE / 'torrent'), '-B', str(destination), '-G', 'Ninja',
        f'-DCMAKE_TOOLCHAIN_FILE={sdk}/toolchain/prospero.cmake', '-DCMAKE_BUILD_TYPE=Release',
        f'-DTORRENT_SOURCE={source}', f'-DBOOST_INCLUDEDIR={boost}', f'-DBoost_INCLUDE_DIR={boost}',
        f'-DOPENSSL_ROOT_DIR={ports}', f'-DOPENSSL_INCLUDE_DIR={ports}/include',
        f'-DOPENSSL_SSL_LIBRARY={ports}/lib/libssl.a', f'-DOPENSSL_CRYPTO_LIBRARY={ports}/lib/libcrypto.a',
        '-DOPENSSL_USE_STATIC_LIBS=TRUE']
    # Keep line information for console crash symbolization without changing optimization.
    cmake += ['-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -gline-tables-only']
    cmake += ['-DCMAKE_CXX_FLAGS=-DTORRENT_BROKEN_RANDOM_DEVICE=1']
    for name, command in [('configure', cmake), ('build', ['cmake', '--build', str(destination), '--target', 'torrent-rasterbar', '--parallel', '6'])]:
        log_path = build / f'torrent-{name}.log'
        with log_path.open('w') as log:
            result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            text = log_path.read_text(errors='replace')
            print('\n'.join(line for line in text.splitlines() if 'error:' in line or 'undefined symbol' in line), flush=True)
            print(text[-6000:], flush=True)
            raise RuntimeError(f'libtorrent {name} failed. Full log: {log_path}')
    library = destination / 'libtorrent.a'
    if not library.is_file():
        raise RuntimeError('libtorrent build did not produce libtorrent.a')
    return {
        'sources': [ROOT / 'native/shared/torrent_api.cpp', ROOT / 'native/shared/torrent_js.cpp'],
        'includes': [source / 'include', boost],
        'definitions': ['-DPS5_REACT_TORRENT=1', '-DTORRENT_MINIMAL=' + minimal,
            '-DTORRENT_BROKEN_RANDOM_DEVICE=1',
            '-DTORRENT_NO_DEPRECATE', '-DTORRENT_DISABLE_LOGGING',
            '-DTORRENT_USE_OPENSSL', '-DTORRENT_USE_LIBCRYPTO', '-DTORRENT_SSL_PEERS', '-DOPENSSL_NO_SSL2',
            '-DBOOST_ASIO_ENABLE_CANCELIO', '-DBOOST_ASIO_NO_DEPRECATED'],
        'libraries': [library],
        'receipt': {'dependencies': MANIFEST, 'library_sha256': sha256(library),
            'minimal_mode': minimal == '1', 'random_seed_backend': 'openssl-RAND_bytes',
            'hardware_tested': False},
        'licenses': [source / 'COPYING', boost / 'LICENSE_1_0.txt']
    }
