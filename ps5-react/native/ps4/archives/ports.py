"""Build pinned archive codecs and crypto for OpenOrbis in private build paths."""
from pathlib import Path
import hashlib,json,re,shutil,subprocess,tarfile,urllib.request

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]

def digest(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def prepare(args,env,run,build):
    lock=json.loads((HERE/'dependencies.lock.json').read_text())
    cache=ROOT/'.deps/archive-ports';cache.mkdir(parents=True,exist_ok=True)
    sources={}
    for name,item in lock.items():
        archive=cache/f"{name}-{item['version']}.tar.gz"
        if not archive.exists():
            with urllib.request.urlopen(item['url'],timeout=60) as response:raw=response.read(64*1024*1024)
            if len(raw)>=64*1024*1024:raise RuntimeError('Archive source exceeds download budget')
            archive.write_bytes(raw)
        if digest(archive)!=item['sha256']:raise RuntimeError('Archive dependency checksum mismatch: '+name)
        source=cache/f"{name}-{item['version']}-src"
        marker=source/'.verified-source'
        if not marker.exists() or marker.read_text()!=item['sha256']:
            source.mkdir(exist_ok=True)
            with tarfile.open(archive) as package:
                members=package.getmembers();prefix=members[0].name.split('/')[0]
                for member in members:
                    if member.name!=prefix and not member.name.startswith(prefix+'/'):raise RuntimeError('Unexpected source root')
                    member.name=member.name[len(prefix):].lstrip('/')
                    if member.name:package.extract(member,source,filter='data')
            marker.write_text(item['sha256'])
        for patch in item.get('patches',[]):
            target=source/patch['file']
            raw=target.read_text()
            if patch['after'] in raw:continue
            if raw.count(patch['before'])!=patch['count']:raise RuntimeError('Archive source patch context changed')
            target.write_text(raw.replace(patch['before'],patch['after']),newline='\n')
        sources[name]=source
    destination=Path(build)/'archive-ports';destination.mkdir(parents=True,exist_ok=True)
    prefix=destination/'prefix';(prefix/'lib').mkdir(parents=True,exist_ok=True);(prefix/'include').mkdir(exist_ok=True)
    toolchain=destination/'toolchain.cmake'
    toolchain.write_text(f'''set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "{args.llvm.as_posix()}/clang.exe")
set(CMAKE_CXX_COMPILER "{args.llvm.as_posix()}/clang++.exe")
set(CMAKE_AR "{args.llvm.as_posix()}/llvm-ar.exe")
set(CMAKE_RANLIB "{args.llvm.as_posix()}/llvm-ranlib.exe")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-freebsd12-elf)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-freebsd12-elf)
set(CMAKE_SYSROOT "{args.toolchain.as_posix()}")
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES "{args.toolchain.as_posix()}/include")
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES "{args.toolchain.as_posix()}/include;{args.toolchain.as_posix()}/include/c++/v1")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS_INIT "-fPIC -fshort-wchar -ffunction-sections -fdata-sections -ftls-model=global-dynamic -D__Orbis__=1 -D_BSD_SOURCE=1 -D_POSIX_C_SOURCE=200809L")
set(CMAKE_FIND_ROOT_PATH "{prefix.as_posix()};{args.toolchain.as_posix()}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
''')
    cmake=args.dev_root/'cmake-3.28.6-windows-x86_64/bin/cmake.exe'
    common=['-G','Ninja',f'-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}',
            f'-DCMAKE_MAKE_PROGRAM={(args.dev_root / "ninja/ninja.exe").as_posix()}',
            '-DCMAKE_BUILD_TYPE=Release','-DBUILD_SHARED_LIBS=OFF','-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
            f'-DCMAKE_INSTALL_PREFIX={prefix.as_posix()}',
            '-DCMAKE_C_FLAGS=-fPIC -fshort-wchar -ffunction-sections -fdata-sections -ftls-model=global-dynamic -D__Orbis__=1 -D_BSD_SOURCE=1 -D_POSIX_C_SOURCE=200809L']
    def configure(name,source,options):
        output=destination/name
        run([cmake,'-S',source,'-B',output,*common,*options],destination/(name+'-configure.log'),env)
        return output
    def compile(name,output,target):
        run([cmake,'--build',output,'--target',target,'--parallel',str(args.jobs)],destination/(name+'-build.log'),env)
    def library(directory,names,target):
        found=[file for file in directory.rglob('*.a') if file.name in names]
        if len(found)!=1:raise RuntimeError('Expected one static library: '+str(found))
        shutil.copy2(found[0],prefix/'lib'/target)
    support=configure('support',HERE,[f'-DMBEDTLS_SOURCE={sources["mbedtls"].as_posix()}',f'-DBZIP2_SOURCE={sources["bzip2"].as_posix()}'])
    compile('support',support,'all')
    library(support,{'libmbedcrypto.a'},'libmbedcrypto.a');library(support,{'libbz2.a'},'libbz2.a')
    shutil.copytree(sources['mbedtls']/'include/mbedtls',prefix/'include/mbedtls',dirs_exist_ok=True)
    shutil.copy2(HERE/'ps4_archive_mbedtls_config.h',prefix/'include/mbedtls/config.h')
    shutil.copy2(HERE/'ps4_archive_mbedtls_config.h',prefix/'include/ps4_archive_mbedtls_config.h')
    shutil.copy2(sources['bzip2']/'bzlib.h',prefix/'include/bzlib.h')
    zlib=configure('zlib',sources['zlib'],['-DZLIB_BUILD_EXAMPLES=OFF'])
    compile('zlib',zlib,'zlibstatic')
    library(zlib,{'libz.a','libzlibstatic.a','zlibstatic.a'},'libz.a')
    shutil.copy2(sources['zlib']/'zlib.h',prefix/'include/zlib.h');shutil.copy2(zlib/'zconf.h',prefix/'include/zconf.h')
    xz=configure('xz',sources['xz'],['-DBUILD_TESTING=OFF','-DENABLE_NLS=OFF','-DENABLE_THREADS=OFF'])
    compile('xz',xz,'liblzma')
    library(xz,{'liblzma.a'},'liblzma.a')
    shutil.copy2(sources['xz']/'src/liblzma/api/lzma.h',prefix/'include/lzma.h')
    shutil.copytree(sources['xz']/'src/liblzma/api/lzma',prefix/'include/lzma',dirs_exist_ok=True)
    options=[f'-DCMAKE_PREFIX_PATH={prefix.as_posix()}',
             f'-DZLIB_INCLUDE_DIR={(prefix/"include").as_posix()}',f'-DZLIB_LIBRARY={(prefix/"lib/libz.a").as_posix()}',
             f'-DBZIP2_INCLUDE_DIR={(prefix/"include").as_posix()}',f'-DBZIP2_LIBRARIES={(prefix/"lib/libbz2.a").as_posix()}',
             f'-DLIBLZMA_INCLUDE_DIR={(prefix/"include").as_posix()}',f'-DLIBLZMA_LIBRARY={(prefix/"lib/liblzma.a").as_posix()}',
             f'-DMBEDTLS_INCLUDE_DIRS={(prefix/"include").as_posix()}',
             *[f'-D{name}={(prefix/"lib/libmbedcrypto.a").as_posix()}' for name in ['MBEDTLS_LIBRARY','MBEDX509_LIBRARY','MBEDCRYPTO_LIBRARY']],
             '-DENABLE_MBEDTLS=ON','-DENABLE_OPENSSL=OFF','-DENABLE_LZMA=ON','-DENABLE_ZLIB=ON','-DENABLE_BZip2=ON',
             '-DLIBMD_FOUND=0','-DHAVE__FSEEKI64=0','-DHAVE__GET_TIMEZONE=0']
    options += ['-D'+name+'=OFF' for name in ['ENABLE_NETTLE','ENABLE_LIBB2','ENABLE_LZ4','ENABLE_LZO','ENABLE_ZSTD',
      'ENABLE_LIBXML2','ENABLE_EXPAT','ENABLE_PCREPOSIX','ENABLE_PCRE2POSIX','ENABLE_LIBGCC','ENABLE_CNG',
      'ENABLE_TAR','ENABLE_CPIO','ENABLE_CAT','ENABLE_UNZIP','ENABLE_XATTR','ENABLE_ACL','ENABLE_ICONV','ENABLE_TEST']]
    options += ['-D'+name+'=0' for name in ['HAVE_ARC4RANDOM_BUF','HAVE_ARC4RANDOM','HAVE_GETENTROPY','HAVE_GETRANDOM',
      'HAVE_FORK','HAVE_VFORK','HAVE_POSIX_SPAWN','HAVE_OPENAT','HAVE_FSTATAT','HAVE_READLINKAT','HAVE_UTIMENSAT','HAVE_FUTIMENS']]
    # Static cross checks cannot prove that an external function links. Seed
    # these probes from the actual SDK providers instead of accepting every name.
    providers=[args.toolchain/'lib'/name for name in ['libc.a','libkernel.so','libSceLibcInternal.so']]
    symbols=subprocess.check_output([str(args.llvm/'llvm-nm.exe'),'-g','--defined-only',*[str(p) for p in providers]],text=True)
    exports={line.split()[-1] for line in symbols.splitlines() if len(line.split())>=2 and not line.endswith(':')}
    cmake_source=(sources['libarchive']/'CMakeLists.txt').read_text()
    for function,variable in re.findall(r'CHECK_FUNCTION_EXISTS(?:_GLIBC)?\(\s*(\w+)\s+(\w+)\s*\)',cmake_source):
        if variable in {'HAVE_FORK','HAVE_VFORK','HAVE_POSIX_SPAWNP'}:continue
        options += [f'-D{variable}={int(function in exports)}']
    options += ['-D'+name+'=0' for name in ['HAVE_FCHFLAGS','HAVE_CHFLAGS','HAVE_LCHFLAGS',
      'HAVE_ARC4RANDOM_BUF','HAVE_FORK','HAVE_VFORK','HAVE_POSIX_SPAWN','HAVE_POSIX_SPAWNP',
      'HAVE_OPENAT','HAVE_FSTATAT','HAVE_READLINKAT','HAVE_UNLINKAT','HAVE_FUTIMENS','HAVE_UTIMENSAT']]
    options += ['-DARCHIVE_CRYPTO_RMD160_MBEDTLS=0']
    for kind in ['MD5','SHA1','SHA256','SHA384','SHA512']:
        options += [f'-DARCHIVE_CRYPTO_{kind}_LIBC=0',f'-DARCHIVE_CRYPTO_{kind}_LIBMD=0']
    archive=configure('libarchive',sources['libarchive'],options)
    compile('libarchive',archive,'archive_static')
    library(archive,{'libarchive.a','libarchive_static.a'},'libarchive.a')
    for header in ['archive.h','archive_entry.h']:shutil.copy2(sources['libarchive']/'libarchive'/header,prefix/'include'/header)
    libraries=[prefix/'lib'/name for name in ['libarchive.a','liblzma.a','libbz2.a','libz.a','libmbedcrypto.a']]
    receipt={'dependencies':lock,'libraries':{file.name:digest(file) for file in libraries},
             'crypto_config_sha256':digest(HERE/'ps4_archive_mbedtls_config.h'),'hardware_tested':False,
             'target':'x86_64-pc-freebsd12-elf/OpenOrbis','crypto_purpose':'archive decryption and integrity only; no TLS'}
    (destination/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    return {'libraries':libraries,'includes':[HERE/'crypto',prefix/'include'],'sources':sources,'receipt':receipt,
            'licenses':{name+'-LICENSE':source/lock[name]['license_file'] for name,source in sources.items()}}
