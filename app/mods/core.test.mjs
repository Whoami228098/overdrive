import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import test from 'node:test';
import {createModInstaller,digestBytes,installedIdentity,loadModIndex,loadTitleMods,loaderTarget,
  MAX_FILE,normalizeRecord,parseSfo,sameVersion} from './core.js';

const bytes=Uint8Array.from([0,0xff,0x80,10,13,42]);
const digest=createHash('sha256').update(bytes).digest('hex');
const record={id:'a'.repeat(24),host:'ps5',platform:'ps5',titleId:'PPSA01234',version:'01.002.000',
  format:'json',loader:'cheats',file:`payload/${digest}.json`,sha256:digest,bytes:bytes.length,
  features:['Infinite Money'],authors:['Test author'],categories:['money']};
const encode=value=>new TextEncoder().encode(value);
function fixture({loader=true}={}) {
  const files=new Map([['/pack/'+record.file,bytes.slice()]]),dirs=new Set(loader?['/data/cheatrunner']:[]),calls=[];
  const fs={
    stat(path){return files.has(path)?{isFile:true,size:files.get(path).length}:dirs.has(path)?{isDirectory:true,size:0}:null;},
    readBytes(path){if(!files.has(path))throw Error('missing');return files.get(path).slice();},
    readFile(path){return new TextDecoder().decode(fs.readBytes(path));},
    writeFile(path,data){files.set(path,encode(data));calls.push(['write',path]);},
    rename(from,to){files.set(to,files.get(from));files.delete(from);},
    copyFile(from,to){if(!files.has(from))throw Error('missing');files.set(to,files.get(from).slice());calls.push(['copy',to]);},
    mkdir(path){dirs.add(path);},remove(path){files.delete(path);},readDir(){return [];},
  };
  let installed={titleId:record.titleId,version:'1.2.0'};
  const installer=createModInstaller({fs,host:'ps5',root:'/pack',stateDir:'/state',identity:async()=>installed,yieldFrame:async()=>{}});
  return {fs,files,dirs,calls,installer,setInstalled(value){installed=value;}};
}
function sfo(titleId='CUSA12345',version='01.07') {
  const keys=encode('TITLE_ID\0APP_VER\0'),values=encode(`${titleId}\0${version}\0`),result=new Uint8Array(52+keys.length+values.length);
  const v=new DataView(result.buffer),base=52+keys.length;
  v.setUint32(0,0x46535000,true);v.setUint32(4,0x101,true);v.setUint32(8,52,true);v.setUint32(12,base,true);v.setUint32(16,2,true);
  for(const [index,key,offset,length] of [[0,0,0,titleId.length+1],[1,9,titleId.length+1,version.length+1]]) {
    const p=20+index*16;v.setUint16(p,key,true);v.setUint16(p+2,0x204,true);v.setUint32(p+4,length,true);v.setUint32(p+8,length,true);v.setUint32(p+12,offset,true);
  }
  result.set(keys,52);result.set(values,base);return result;
}

test('SHA-256 uses exact binary bytes, all lengths and cooperative chunks',async()=>{
  let yields=0;
  for(const size of [0,1,55,56,63,64,65,16385,MAX_FILE]) {
    const input=Uint8Array.from({length:size},(_,i)=>(i*73)%256);
    assert.equal(await digestBytes(input,async()=>{yields++;}),createHash('sha256').update(input).digest('hex'));
  }
  assert.ok(yields>1);
  await assert.rejects(()=>digestBytes(new Uint8Array(MAX_FILE+1)),/size/);
});
test('host, exact region ID, versions and bounded data paths are validated',()=>{
  assert.ok(normalizeRecord(record,'ps5',record.titleId));
  for(const patch of [{host:'ps4'},{titleId:'PPSA99999'},{file:'../x'},{file:'https://x'},{bytes:MAX_FILE+1},{format:'prx'},
    {platform:'ps4'},{id:'../x'},{loader:'patches'},{version:'*'}]) assert.equal(normalizeRecord({...record,...patch},'ps5',record.titleId),null);
  assert.equal(normalizeRecord({...record,host:'ps4'},'ps4',record.titleId),null);
  assert.equal(sameVersion('01.002.000','1.2.0'),true);
  assert.equal(sameVersion('01.002.000','1.3.0'),false);
  assert.equal(sameVersion('01.00','1.0.0'),false);
});
test('lazy catalog rejects escaping per-title paths and mixed host records',()=>{
  const f=fixture();
  f.files.set('/pack/hosts/ps5/index.json',encode(JSON.stringify({version:1,host:'ps5',titles:[
    {titleId:record.titleId,title:'Game',file:`hosts/ps5/titles/${record.titleId}.json`},
    {titleId:'PPSA99999',file:'../escape'}]})));
  f.files.set(`/pack/hosts/ps5/titles/${record.titleId}.json`,encode(JSON.stringify({version:1,records:[record,{...record,host:'ps4'}]})));
  const c=loadModIndex(f.fs,['/missing','/pack'],'ps5');
  assert.equal(c.titles.length,1);assert.equal(loadTitleMods(f.fs,c,'ps5',record.titleId).length,1);
  assert.deepEqual(loadTitleMods(f.fs,c,'ps5','../bad'),[]);
  assert.deepEqual(loadModIndex(f.fs,['/missing'],'ps4').titles,[]);
});
test('SFO binary parser returns real title/version and rejects corrupt ranges',()=>{
  assert.deepEqual(parseSfo(sfo()),{titleId:'CUSA12345',version:'01.07'});
  const corrupt=sfo();new DataView(corrupt.buffer).setUint32(12,0xffffffff,true);
  assert.throws(()=>parseSfo(corrupt),/tables/);
  assert.throws(()=>parseSfo(sfo().subarray(0,31)),/tables/);
  assert.equal(parseSfo(sfo('WRONGID','01.07')),null);
});
test('installed identity checks metadata, never trusts another region/catalog version',async()=>{
  const f=fixture();f.files.set('/user/appmeta/CUSA12345/param.sfo',sfo());
  assert.equal((await installedIdentity(f.fs,'ps4','CUSA12345',async()=>{})).version,'01.07');
  f.files.set('/user/appmeta/PPSA01234/param.json',encode(JSON.stringify({titleId:'PPSA99999',contentVersion:'01.002.000'})));
  assert.equal(await installedIdentity(f.fs,'ps5','PPSA01234',async()=>{}),null);
  assert.equal(await installedIdentity(f.fs,'ps4','PPSA01234',async()=>{}),null);
});
test('compatible data install keeps a verified backup and restores exact old bytes',async()=>{
  const f=fixture(),target='/data/cheatrunner/cheats/json/PPSA01234_01.002.000.json',old=Uint8Array.from([4,5,6]);
  f.files.set(target,old);
  const saved=await f.installer.install(record);assert.equal(saved.loader,'CheatRunner');assert.deepEqual(f.files.get(target),bytes);
  await f.installer.install(record); // Idempotent, without replacing the original backup.
  await f.installer.restore(record);assert.deepEqual(f.files.get(target),old);
});
test('restore removes an app-added file, retains external edits, and validates receipts',async()=>{
  const f=fixture(),{destination}=await f.installer.install(record);
  f.files.set(destination,encode('user edit'));
  await assert.rejects(()=>f.installer.restore(record),/outside/);assert.equal(f.fs.readFile(destination),'user edit');
  f.files.set(destination,bytes.slice());
  const receipt=JSON.parse(f.fs.readFile(`/state/${record.id}.json`));
  f.files.set(`/state/${record.id}.json`,encode(JSON.stringify({...receipt,destination:'/outside'})));
  await assert.rejects(()=>f.installer.restore(record),/receipt/);
  f.files.set(`/state/${record.id}.json`,encode(JSON.stringify(receipt)));
  await f.installer.restore(record);assert.equal(f.fs.stat(destination),null);
});
test('wrong game version, absent loader, checksum failure never modify loader files',async()=>{
  const f=fixture();f.setInstalled({titleId:record.titleId,version:'01.003.000'});
  await assert.rejects(()=>f.installer.install(record),/version/);
  f.setInstalled(null);await assert.rejects(()=>f.installer.install(record),/version/);
  const missing=fixture({loader:false});await assert.rejects(()=>missing.installer.install(record),/loader/);
  f.files.set('/pack/'+record.file,new Uint8Array(bytes.length));
  await assert.rejects(()=>f.installer.install(record),/checksum/);
  assert.equal(f.calls.length,0);assert.equal(missing.calls.length,0);
});
test('export works without a loader or installed game, stays app-owned and is reversible',async()=>{
  const f=fixture({loader:false});f.setInstalled(null);
  const result=await f.installer.install(record,{exportOnly:true});
  assert.ok(result.destination.startsWith('/state/exports/'));assert.deepEqual(f.files.get(result.destination),bytes);
  await f.installer.restore(record);assert.equal(f.fs.stat(result.destination),null);
});
test('saving a pack first does not block later installation into a configured loader',async()=>{
  const f=fixture();
  const exported=await f.installer.install(record,{exportOnly:true});
  const installed=await f.installer.install(record);
  assert.notEqual(installed.destination,exported.destination);
  await f.installer.restore(record);
  assert.equal(f.fs.stat(installed.destination),null);
  assert.deepEqual(f.files.get(exported.destination),bytes);
});
test('interrupted prepared intent can be cleared without changing the previous file',async()=>{
  const f=fixture();
  f.setInstalled({titleId:record.titleId,version:record.version});
  const old=Uint8Array.from([1,2,3]),target='/data/cheatrunner/cheats/json/PPSA01234_01.002.000.json';
  f.files.set(target,old);
  const copy=f.fs.copyFile;
  f.fs.copyFile=(from,to)=>{if(to===target)throw Error('simulated interruption');copy(from,to);};
  await assert.rejects(()=>f.installer.install(record),/interruption/);
  await f.installer.restore(record);assert.deepEqual(f.files.get(target),old);
  f.fs.copyFile=copy;
  await f.installer.install(record);assert.deepEqual(f.files.get(target),bytes);
});
test('loader routes keep PS5-native patches separate and never use PS4-host cheats on PS5',()=>{
  const f=fixture();
  assert.equal(loaderTarget(f.fs,'ps5',{...record,format:'xml'}).directory,'/data/cheatrunner/patches/xml_prospero');
  f.dirs.clear();f.dirs.add('/data/GoldHEN');
  assert.equal(loaderTarget(f.fs,'ps5',record),null);
  const ps4={...record,titleId:'CUSA12345',host:'ps4',platform:'ps4',format:'xml'};
  assert.equal(loaderTarget(f.fs,'ps4',ps4).filename,'CUSA12345.xml');
  assert.equal(loaderTarget({stat(){throw Error('permission denied');}},'ps5',record),null);
});
test('malformed optional pack labels and version lists cannot become React objects',()=>{
  assert.equal(normalizeRecord({...record,features:[{},null]},'ps5',record.titleId),null);
  assert.equal(normalizeRecord({...record,source:{repository:{bad:true}}},'ps5',record.titleId).source.repository,'');
  const f=fixture();
  f.files.set('/pack/hosts/ps5/index.json',encode(JSON.stringify({version:1,host:'ps5',titles:[
    {titleId:record.titleId,title:{bad:true},versions:{join:'broken'},file:`hosts/ps5/titles/${record.titleId}.json`}]})));
  const c=loadModIndex(f.fs,['/pack'],'ps5');
  assert.equal(c.titles[0].title,record.titleId);assert.deepEqual(c.titles[0].versions,[]);
});
