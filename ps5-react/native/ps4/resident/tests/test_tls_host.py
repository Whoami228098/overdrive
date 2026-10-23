"""Run the actual private-TLS C++ implementation as a freestanding Windows DLL.

Only allocator and native pthread-key primitives are adapted. The C++ private
image/destructor/lifecycle implementation is unchanged from the PS4 build.
"""
import argparse
import ctypes as C
from pathlib import Path
import subprocess
import threading
import json
import time

parser = argparse.ArgumentParser()
parser.add_argument('--llvm', type=Path, default=Path('F:/Dev/LLVM18/bin'))
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
here = Path(__file__).resolve().parent
objects = []
for source in (here.parent / 'module_tls.cpp', here / 'tls_host_adapter.cpp'):
    obj = args.output / (source.stem + '.obj')
    subprocess.run([args.llvm / 'clang++.exe', '--target=x86_64-pc-windows-msvc', '-ffreestanding',
                    '-fno-builtin', '-fno-stack-protector', '-fno-exceptions', '-fno-rtti', '-O2',
                    '-DRESIDENT_TLS_HOST_TEST=1', '-std=c++17', '-I', here.parent,
                    '-c', source, '-o', obj], check=True)
    objects.append(obj)
dll = args.output / 'module-tls-host.dll'
exports = ['test_configure', 'test_access', 'test_register', 'test_initialize', 'test_detach', 'test_contexts', 'test_shutdown',
           'test_stdio_lock', 'test_stdio_unlock']
exports += ['test_owned_key_create', 'test_key_set', 'test_owned_key_delete']
subprocess.run([args.llvm / 'lld-link.exe', '/dll', '/noentry', '/nodefaultlib', '/out:' + str(dll),
                *['/export:' + name for name in exports], *objects], check=True)
api = C.CDLL(str(dll))
Allocate = C.CFUNCTYPE(C.c_void_p, C.c_size_t)
Release = C.CFUNCTYPE(None, C.c_void_p)
KeyCreate = C.CFUNCTYPE(C.c_int, C.POINTER(C.c_uint), C.c_void_p)
KeyDelete = C.CFUNCTYPE(C.c_int, C.c_uint)
KeyGet = C.CFUNCTYPE(C.c_void_p, C.c_uint)
KeySet = C.CFUNCTYPE(C.c_int, C.c_uint, C.c_void_p)
Once = C.CFUNCTYPE(C.c_int, C.POINTER(C.c_int), C.c_void_p)
Fatal = C.CFUNCTYPE(None)
Self = C.CFUNCTYPE(C.c_size_t)
Yield = C.CFUNCTYPE(C.c_int)
Destructor = C.CFUNCTYPE(None, C.c_void_p)
class Callbacks(C.Structure):
    _fields_ = [('allocate', Allocate), ('release', Release), ('create', KeyCreate), ('delete', KeyDelete),
                ('get', KeyGet), ('set', KeySet), ('once', Once), ('self', Self), ('yield', Yield), ('fatal', Fatal)]
crt = C.CDLL('msvcrt.dll')
crt.malloc.argtypes, crt.malloc.restype = [C.c_size_t], C.c_void_p
crt.free.argtypes, crt.free.restype = [C.c_void_p], None
values, allocations = {}, set()
key_destructors = {}
next_key = 0
once_lock = threading.RLock()
fail_allocation = False
def allocate(size):
    global fail_allocation
    if fail_allocation:
        fail_allocation = False
        return None
    pointer = crt.malloc(size)
    if pointer: allocations.add(pointer)
    return pointer
def release(pointer):
    if pointer: allocations.remove(pointer)
    crt.free(pointer)
def create(key, callback):
    global next_key
    next_key += 1
    key[0] = next_key
    key_destructors[next_key] = Destructor(callback) if callback else None
    return 0
def delete(key):
    assert not any(k[1] == key for k in values)
    del key_destructors[key]
    return 0
def get(key): return values.get((threading.get_ident(), key))
def put(key, value):
    if value: values[threading.get_ident(), key] = value
    else: values.pop((threading.get_ident(), key), None)
    return 0
def once(state, callback):
    with once_lock:
        if state[0] == 0:
            C.CFUNCTYPE(None)(callback)()
            state[0] = 1
    return 0
def fatal(): raise AssertionError('TLS ABI unexpectedly aborted')
def yield_thread():
    time.sleep(0)
    return 0
callbacks = Callbacks(Allocate(allocate), Release(release), KeyCreate(create), KeyDelete(delete), KeyGet(get), KeySet(put), Once(once),
                      Self(threading.get_ident), Yield(yield_thread), Fatal(fatal))
api.test_configure.argtypes = [C.POINTER(Callbacks)]
api.test_configure(C.byref(callbacks))
api.test_access.argtypes, api.test_access.restype = [C.c_size_t], C.c_void_p
api.test_register.argtypes, api.test_register.restype = [Destructor, C.c_void_p], C.c_int
api.test_contexts.restype = C.c_size_t
api.test_owned_key_create.argtypes, api.test_owned_key_create.restype = [Destructor], C.c_uint
api.test_key_set.argtypes = [C.c_uint, C.c_void_p]
api.test_owned_key_delete.argtypes = [C.c_uint]
checks = []
assert api.test_initialize() == 1
base = api.test_access(0)
assert base % 64 == 0 and C.string_at(base, 96) == bytes(range(1, 9)) + bytes(88)
assert api.test_access(8) == base + 8 and api.test_contexts() == 1
checks.append('initialized template, aligned allocation, zero tbss, offset ABI')
events = []
@Destructor
def callback(value):
    events.append(value)
    assert api.test_access(0) == base
    if value == 2: assert api.test_register(callback, 3) == 0
assert api.test_register(callback, 1) == 0 and api.test_register(callback, 2) == 0
assert api.test_detach() == 1 and events == [2, 3, 1] and api.test_contexts() == 0
assert not allocations and not values
checks.append('reverse destructors, registration during destruction, image retained through callbacks')
fail_allocation = True
assert api.test_register(callback, 10) == -1 and api.test_contexts() == 0 and not allocations
checks.append('allocation failure returned without retaining a context')
barrier = threading.Barrier(8)
thread_errors, addresses = [], []
def native_exit():
    for _ in range(32):
        current = [(key, value) for (thread, key), value in values.items() if thread == threading.get_ident()]
        if not current: return
        for key, value in current:
            values.pop((threading.get_ident(), key), None) # POSIX clears first.
            if key_destructors[key]: key_destructors[key](value)
    raise AssertionError('Native destructor callbacks did not settle')
def worker(number):
    try:
        pointer = api.test_access(0)
        addresses.append(pointer)
        assert C.string_at(pointer, 96) == bytes(range(1, 9)) + bytes(88)
        C.c_uint64.from_address(pointer + 8).value = number
        @Destructor
        def exit_destructor(value):
            try:
                assert api.test_access(0) == pointer
                assert C.c_uint64.from_address(api.test_access(8)).value == number
            except BaseException as error: thread_errors.append(repr(error))
        assert api.test_register(exit_destructor, number) == 0
        barrier.wait()
        assert C.c_uint64.from_address(api.test_access(8)).value == number
        native_exit()
    except BaseException as error: thread_errors.append(repr(error))
threads = [threading.Thread(target=worker, args=(i + 1,)) for i in range(8)]
for thread in threads: thread.start()
for thread in threads: thread.join()
assert not thread_errors, thread_errors
assert len(set(addresses)) == 8 and api.test_contexts() == 0 and not allocations and not values
checks.append('eight concurrent threads, isolated TLS writes, native-exit cleanup')
counter = [0]
def stdio_worker():
    for _ in range(100):
        assert api.test_stdio_lock() == 1
        assert api.test_stdio_lock() == 0
        value = counter[0]
        time.sleep(0)
        counter[0] = value + 1
        api.test_stdio_unlock()
threads = [threading.Thread(target=stdio_worker) for _ in range(4)]
for thread in threads: thread.start()
for thread in threads: thread.join()
assert counter[0] == 400
checks.append('module-local recursive stdio lock serializes native threads without FILE/TCB layout assumptions')
owned_events = []
@Destructor
def owned_callback(value):
    owned_events.append(value)
    api.test_access(0) # Native-key destructor can create a new private image.
owned = api.test_owned_key_create(owned_callback)
assert owned != 0xffffffff and api.test_key_set(owned, 99) == 0
host_key = C.c_uint()
host_callback_events = []
@Destructor
def host_callback(value): host_callback_events.append(value)
assert create(C.pointer(host_key), C.cast(host_callback, C.c_void_p).value) == 0
assert put(host_key.value, 1234) == 0
assert api.test_detach() == 1 and owned_events == [99]
assert get(host_key.value) == 1234 and host_callback_events == []
assert api.test_contexts() == 0
put(host_key.value, None); delete(host_key.value)
assert api.test_owned_key_delete(owned) == 0
checks.append('foreign-entry detach drains module libc++ native keys, preserves host keys and settles recreated private TLS')
remaining_key = api.test_owned_key_create(owned_callback)
assert remaining_key != 0xffffffff # Simulate libc++abi's process-lifetime key.
ready, release_worker = threading.Event(), threading.Event()
def remaining_worker():
    api.test_access(0)
    ready.set()
    release_worker.wait()
    native_exit()
thread = threading.Thread(target=remaining_worker)
thread.start(); ready.wait()
assert api.test_contexts() == 1 and api.test_shutdown() == 0
release_worker.set(); thread.join()
assert api.test_contexts() == 0 and api.test_shutdown() == 1
assert api.test_initialize() == 0 and not allocations and not values
assert not key_destructors
checks.append('unload rejected with live contexts; key deleted only after workers exited')
result = {'passed': len(checks), 'checks': checks, 'hardware_tested': False, 'host': 'Windows x86-64 actual C++ TLS implementation'}
(args.output / 'host-test-results.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
