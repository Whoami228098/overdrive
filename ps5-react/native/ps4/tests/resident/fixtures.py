from pathlib import Path
import random,struct,sys,zipfile
import importlib.util
out=Path(sys.argv[1]);data=bytearray(random.Random(4242).randbytes(32*1024*1024))
data[:4]=b'\x7fCNT';data[0x40:0x64]=b'UP9000-SLUS21590_00-SLUS215900000001'
struct.pack_into('>Q',data,0x430,len(data));(out/'owned.pkg').write_bytes(data)
with zipfile.ZipFile(out/'game.zip','w') as archive:archive.writestr('SLUS21590/game.pkg',data)
with zipfile.ZipFile(out/'zip-slip.zip','w') as archive:archive.writestr('../escaped.pkg',data)
with zipfile.ZipFile(out/'zip-symlink.zip','w') as archive:
 entry=zipfile.ZipInfo('link');entry.create_system=3;entry.external_attr=0o120777<<16;archive.writestr(entry,'/tmp/outside')
 archive.writestr('link/game.pkg',data)
tools=Path(__file__).resolve().parents[4]/'tools';sys.path.insert(0,str(tools))
spec=importlib.util.spec_from_file_location('archive_fixtures',tools/'test_archives.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
(out/'game.rar').write_bytes(module.plain_rar5('SLUS21590/game.pkg',data))
(out/'protected.rar').write_bytes(module.encrypted_rar5(data,password='Published fixture',name='SLUS21590/game.pkg')[0])
