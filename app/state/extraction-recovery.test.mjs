import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
const source=name=>readFileSync(new URL(name,import.meta.url),'utf8').replace(/^import .*;\r?$/gm,'').replace(/^export .*;\r?$/gm,'');
const key='v1-0123456789abcdef0123456789abcdef01234567';
const torrent=`/data/downloads/torrents/${key}`;
const archive=`${torrent}/game.7z`;
const output='/data/downloads/extracted/synthetic-release';
const uri='magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567';

async function recover({missing=false}={}) {
  const release={id:'SYNTHETIC',source:'user',type:'torrent',url:uri,filename:'synthetic-release',sizeBytes:128e9};
  const job={id:'PPSA99994/SYNTHETIC',gameId:'PPSA99994',title:'Owned archive fixture',root:'/data',rootIdentity:'1:2',
    release,artifact:{destination:torrent,nextAction:'extraction-required',directory:'/data/downloads/torrents'},
    state:'extracting',written:100e9,total:286e9,output,torrentFiles:[{path:archive,size:128e9}]};
  const files=new Map([['/data/app/overdrive/downloads.json',JSON.stringify({version:1,jobs:[job]})]]);
  const directories=new Set(['/data','/data/app','/data/app/overdrive',output+'.extracting']);
  let transfers=0,resolves=0;
  const requests=[];
  const fs={
    exists:path=>files.has(path)||directories.has(path)||!missing&&path===archive,
    readFile:path=>{if(!files.has(path))throw Error('ENOENT');return files.get(path);},
    writeFile:(path,value)=>files.set(path,value),remove:path=>files.delete(path),
    rename:(from,to)=>{if(!files.has(from))throw Error('ENOENT');files.set(to,files.get(from));files.delete(from);},
    mkdir:path=>directories.add(path),readDir:()=>[],
    stat:path=>path==='/data'?{isDirectory:true,device:'1',inode:'2'}:!missing&&path===archive?{isFile:true,size:128e9}:files.has(path)?{isFile:true,size:files.get(path).length,modified:1}:null,
    diskUsage:()=>({free:1024**3})
  };
  const context=vm.createContext({console:{log(){},error(){}},URL,AbortController,setTimeout,clearTimeout,
    Torrent:{available:false},INCOMPLETE_PART:/no-match/,STALE_PARTIAL:/no-match/,cause:value=>value,t:key=>key,
    sourceKind:()=> 'torrent',torrentKey:()=>key,formatBytes:value=>String(value),MARGIN2:16*1024**2,
    archiveRefusal:()=>null,BrowserRequired:class BrowserRequired extends Error{},
    cancellation:()=>new AbortController()});
  vm.runInContext(source('./archive-password.js')+'\n'+source('./torrent-archives.js').replace('export function','function')+'\n'+source('./read-json.js')+'\n'+source('./download-queue.js')+'\nglobalThis.create=createDownloadQueue;',context);
  const queue=context.create({fs,dataDir:'/data/app',platform:'ps5',power:{keepAwake(){}},notifications:{show(){}},
    sources:{async resolve(){resolves++;throw Error('Network must not be used');}},
    torrents:{available:false,enqueue(){transfers++;throw Error('Torrent must not be re-added');}},
    releasesOf:()=>[{...release,archivePassword:'[Published case]',archiveMetadata:{unpackedSizeBytes:286351425536}}],
    archives:{extract(options){requests.push(options);return{subscribe:()=>()=>{},done:Promise.resolve({artifacts:[]})};}}
  });
  for(let n=0;n<30&&['queued','resolving','extracting'].includes(queue.snapshot()[0].state);n++)await new Promise(resolve=>setTimeout(resolve,1));
  return {job:queue.snapshot()[0],requests,transfers,resolves,fs};
}

test('reopening extraction reuses exact completed files without a network engine or free full-copy space',async()=>{
  const result=await recover();
  assert.equal(result.job.state,'extracted',result.job.error);
  assert.equal(result.transfers,0);assert.equal(result.resolves,0);
  assert.equal(result.requests.length,1);
  assert.equal(result.requests[0].password,'[Published case]');
  assert.equal(result.requests[0].destination,output);
  assert.equal(result.requests[0].maxBytes,1024**4);
  assert.equal(result.fs.stat(archive).size,128e9);
  assert.equal(result.job.total,128e9);
});

test('a missing completed archive fails visibly while preserving its tracking; no silent redownload',async()=>{
  const result=await recover({missing:true});
  assert.equal(result.job.state,'failed');
  assert.match(result.job.error,/missing or changed/);
  assert.equal(result.requests.length,0);
  assert.equal(result.transfers,0);assert.equal(result.resolves,0);
});
