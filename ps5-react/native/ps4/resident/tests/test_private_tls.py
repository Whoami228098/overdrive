"""Offline ELF transformation tests using actual linked compiler TLS fixtures."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--input', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--dev-root', type=Path, default=Path('F:/Dev'))
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
here = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('private_tls', here / 'private_tls.py')
tls = importlib.util.module_from_spec(spec); spec.loader.exec_module(tls)
checks = []
original = args.input.read_bytes()
output = args.output / 'valid.private.elf'
receipt = tls.materialize(args.input, output)
assert args.input.read_bytes() == original
assert not any(p[0] == 7 for p in tls.Elf(output).programs)
assert receipt['materialized_relocations']
checks.append('actual libtorrent ELF transformed; original unchanged; native TLS eliminated')

def first_tls_relocation(elf):
    for section in elf.sections:
        if section[1] != 4: continue
        for at in range(section[4], section[4] + section[5], 24):
            if struct.unpack_from('<Q', elf.data, at + 8)[0] & 0xffffffff == 16: return at
    raise AssertionError('No DTPMOD64 relocation in input')

def rejects(name, mutate, expected):
    elf = tls.Elf(args.input)
    mutate(elf)
    path = args.output / (name + '.elf'); path.write_bytes(elf.data)
    try: tls.materialize(path, args.output / (name + '.private.elf'))
    except ValueError as error: assert expected in str(error), str(error)
    else: raise AssertionError('Accepted forbidden fixture: ' + name)
    checks.append('reject ' + name)

def type_change(kind):
    def mutate(elf):
        at = first_tls_relocation(elf)
        info = struct.unpack_from('<Q', elf.data, at + 8)[0]
        struct.pack_into('<Q', elf.data, at + 8, info & 0xffffffff00000000 | kind)
    return mutate
for name, kind in [('static_tpoff', 18), ('tlsdesc', 36), ('unknown_tls_model', 19)]:
    rejects(name, type_change(kind), 'Unsupported TLS relocation')
def bad_target(elf): struct.pack_into('<Q', elf.data, first_tls_relocation(elf), elf.by_name['.text'][3] + 32)
rejects('text_target', bad_target, 'unexpected section')
def bad_addend(elf): struct.pack_into('<q', elf.data, first_tls_relocation(elf) + 16, 1)
rejects('module_addend', bad_addend, 'must not have an addend')
def visible(elf):
    symbol = next(s for table in elf.symbols.values() for s in table if s['info'] & 15 == 6)
    elf.data[symbol['position'] + 4] = 0x16
    elf.data[symbol['position'] + 5] = 0
rejects('default_visible_tls', visible, 'External/default-visible')

# Compile a genuine general-dynamic fixture whose default-visible TLS forces
# both DTPMOD64 and DTPOFF64. Make the symbols hidden after linking, preserving
# those relocations, to test the supported private binding/materialization path.
source = args.output / 'dtpoff_fixture.cpp'
source.write_text('extern "C" { __thread unsigned long sample[2] = {123,456}; unsigned long* fixture() { return sample; } }\n')
llvm, sdk = args.dev_root / 'LLVM18/bin', args.dev_root / 'OpenOrbis/OpenOrbis/PS4Toolchain'
obj = args.output / 'dtpoff_fixture.o'
subprocess.run([llvm / 'clang++.exe', '--target=x86_64-pc-freebsd12-elf', '-fPIC', '-O2', '-ftls-model=global-dynamic',
                '-c', source, '-o', obj], check=True)
fixture = args.output / 'dtpoff_fixture.elf'
module_objects = list(args.input.parent.glob('*-module_tls.cpp.o'))
assert len(module_objects) == 1, 'Expected one actual compiled module_tls object'
module_object = module_objects[0]
subprocess.run([llvm / 'ld.lld.exe', '-shared', '--script', here / 'module_link.x', '--eh-frame-hdr',
                obj, module_object, '-o', fixture], check=True)
elf = tls.Elf(fixture)
for table in elf.symbols.values():
    for symbol in table:
        if symbol['info'] & 15 == 6: elf.data[symbol['position'] + 5] = 2
found = 0
for section in elf.sections:
    if section[1] != 4: continue
    for at in range(section[4], section[4] + section[5], 24):
        info = struct.unpack_from('<Q', elf.data, at + 8)[0]
        if info & 0xffffffff == 17:
            struct.pack_into('<q', elf.data, at + 16, 8)
            found += 1
assert found, 'Compiler did not generate DTPOFF64 fixture'
fixture.write_bytes(elf.data)
converted = args.output / 'dtpoff_fixture.private.elf'
result = tls.materialize(fixture, converted)
assert any(r['type'] == 17 and r['symbol'] == 'sample' and r['value'] == elf.named['sample']['value'] + 8
           for r in result['materialized_relocations'])
assert result['template']['initialized'] >= 16
checks.append('actual compiler DTPOFF64 with local STT_TLS symbol + addend materialized correctly')
summary = {'passed': len(checks), 'checks': checks, 'hardware_tested': False}
(args.output / 'test-results.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary, indent=2))
