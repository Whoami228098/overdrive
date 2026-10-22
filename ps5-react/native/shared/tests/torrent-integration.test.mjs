import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';
import {fileURLToPath} from 'node:url';

const repo = fileURLToPath(new URL('../../../../', import.meta.url)).replaceAll('\\', '/');
const planned = process.env.TORRENT_TEST_ROOT || repo;
const magnet = 'magnet:?xt=urn:btih:' + '0123456789abcdef'.repeat(2) + '01234567';
function evaluate(path, names, injected = {}) {
  let code = readFileSync(path, 'utf8').replace(/^import .*?;\s*$/gm, '').replace(/^export \{[^}]+\};?\s*$/gm, '')
    .replace(/\bexport (const|let|var|function|class) /g, '$1 ');
  const context = vm.createContext({console: {log() {}, error() {}}, setTimeout, clearTimeout,
    setInterval, clearInterval, ...injected});
  vm.runInContext(code + '\n;globalThis.result = {' + names.join(',') + '};', context, {filename: path});
  return {...context.result, context};
}
const source = () => evaluate(planned + 'app/state/download-sources.js', ['sourceKind', 'torrentKey', 'createSourceResolver', 'cancellation', 'BrowserRequired', 'webURL', 'archiveRefusal']);

test('magnet v1, base32 and v2 directory keys match native hex keys', () => {
  const {torrentKey} = source();
  assert.equal(torrentKey(magnet), 'v1-0123456789abcdef0123456789abcdef01234567');
  assert.equal(torrentKey('magnet:?xt=urn:btih:' + 'A'.repeat(32)), 'v1-' + '0'.repeat(40));
  assert.equal(torrentKey('magnet:?xt=urn%3Abtmh%3A1220' + 'a'.repeat(64)), 'v2-' + 'a'.repeat(64));
  assert.throws(() => torrentKey(magnet + '&xt=urn:btih:' + 'f'.repeat(40)), /Conflicting/);
  assert.throws(() => torrentKey('magnet:?dn=no-hash'), /valid/);
  assert.throws(() => torrentKey(magnet + '\n'), /Invalid/);
});

test('torrent resolution performs no HTTP request and preserves HTTP source kinds', async () => {
  const {sourceKind, createSourceResolver} = source();
  const resolver = createSourceResolver({http: {request() { throw new Error('HTTP must not run'); }}});
  assert.equal(sourceKind({url: magnet}), 'torrent');
  assert.equal((await resolver.resolve({url: magnet})).uri, magnet);
  assert.equal(sourceKind({url: 'https://example.org/game.pkg'}), 'direct');
  assert.equal(sourceKind({manifestUrl: 'https://example.org/manifest.json'}), 'manifest');
  assert.equal(sourceKind({url: 'https://mediafire.com/file/id'}), 'mediafire');
});

function task() {
  let resolve, reject;
  const listeners = new Set();
  const done = new Promise((ok, fail) => { resolve = ok; reject = fail; });
  done.catch(() => {});
  return {done, resolve, reject, cancelled: 0,
    subscribe(fn) { listeners.add(fn); fn({state:'queued'}); return () => listeners.delete(fn); },
    progress(value) { for (const fn of listeners) fn(value); },
    cancel() { this.cancelled++; const error = new Error('Cancelled.'); error.name = 'AbortError'; reject(error); }};
}
function fixture() {
  const nodes = new Map([['/data', {isDirectory:true,device:'1',inode:'1'}], ['/state', {isDirectory:true,device:'1',inode:'2'}]]);
  const fs = {
    stat: path => nodes.get(path) || null, exists: path => nodes.has(path),
    mkdir(path) { nodes.set(path, {isDirectory:true,device:'1',inode:'3'}); },
    writeFile(path, data) { nodes.set(path, {isFile:true,size:data.length,data}); },
    readFile(path) { if (!nodes.has(path)) throw new Error('missing'); return nodes.get(path).data; },
    rename(from,to) {
      if (!nodes.has(from)) throw new Error('missing ' + from);
      for (const [path,value] of [...nodes]) if (path===from || path.startsWith(from+'/')) {
        nodes.set(to+path.slice(from.length),value); nodes.delete(path);
      }
    },
    remove(path) { nodes.delete(path); }, diskUsage: () => ({free:10**12,total:10**12}),
    chmod(path, mode) { if (!nodes.has(path)) throw new Error('missing ' + path); nodes.get(path).mode = mode; },
    readDir: path => [...nodes].filter(([child]) => child.startsWith(path+'/') && !child.slice(path.length+1).includes('/'))
      .map(([child,value])=>({name:child.slice(path.length+1),...value}))
  };
  const tasks = [], httpTasks = [], options = [];
  const torrents = {available:true, enqueue(value) { options.push(value); const t=task(); tasks.push(t); return t; }};
  const downloads = {enqueue(value) { options.push(value); const t=task(); httpTasks.push(t); return t; }};
  const src = source();
  const formats = evaluate(repo + 'ps5-react/runtime/js/download-formats.js', ['DownloadFormats']).DownloadFormats;
  const context = {
    ...src, Torrent:torrents, t: key => key, readJSON: () => null,
    INCOMPLETE_PART:/part (\d+)/, STALE_PARTIAL:/stale/, cause:value=>value,
    archivePassword:()=>undefined, mountedImages:()=>[], within2:()=>false,
    MARGIN2:16*1024**2, stillNeeded: job=>Math.max(0,(job.total ?? job.release?.sizeBytes ?? 0)-(job.written||0)),
    spaceShortfall:()=>'', deletesPackages:()=>false, formatBytes:String,
    LINK_PREFIX:'',VIKING_PAGE_PREFIX:'',VIKING_PREFIX:''
  };
  const create = evaluate(planned + 'app/state/download-queue.js', ['createDownloadQueue'], context).createDownloadQueue;
  const queue = create({fs,downloads,http:{},formats,dataDir:'/state',stateDir:'/state',platform:'desktop',
    notifications:{show(){}},power:{keepAwake(){}},torrents,sources:src.createSourceResolver({http:{}})});
  const release = {id:'release',type:'torrent',format:'torrent',filename:'game',url:magnet,sizeBytes:100};
  return {queue,release,tasks,httpTasks,options,fs,nodes,key:src.torrentKey(magnet)};
}
const until = async predicate => { for (let n=0;n<100;n++) { if (predicate()) return; await new Promise(ok=>setTimeout(ok,0)); } throw new Error('fixture did not settle'); };

test('torrent queue routes all PKGs without invoking HTTP downloads', async () => {
  const f=fixture();
  f.queue.enqueue({id:'game',title:'Game'},f.release,'/data');
  await until(()=>f.tasks.length===1);
  const path=`/data/downloads/torrents/${f.key}/Game/base.pkg`;
  f.nodes.set(path,{isFile:true,size:100});
  f.tasks[0].progress({state:'downloading',written:50,total:100,seeds:3,peers:4});
  assert.equal(f.queue.snapshot()[0].written,50);
  f.tasks[0].resolve({files:[{path,size:100}]});
  await until(()=>f.queue.snapshot()[0].state==='installer-required');
  assert.equal(f.httpTasks.length,0);
  assert.equal(f.queue.snapshot()[0].packages[0],path);
});

test('pause retains byte progress, resume reuses the same torrent directory', async () => {
  const f=fixture();
  const id=f.queue.enqueue({id:'game',title:'Game'},f.release,'/data');
  await until(()=>f.tasks.length===1);
  f.tasks[0].progress({state:'downloading',written:40,total:100});
  f.queue.pause(id);
  await until(()=>f.queue.snapshot()[0].state==='paused');
  assert.equal(f.queue.snapshot()[0].written,40);
  assert.ok(f.tasks[0].cancelled);
  f.queue.resume(id);
  await until(()=>f.tasks.length===2);
  assert.equal(f.options[0].uri,f.options[1].uri);
  assert.equal(f.options[0].saveRoot,f.options[1].saveRoot);
  f.queue.pause(id);
  await until(()=>f.queue.snapshot()[0].state==='paused');
});

test('completed images publish once and another job cannot claim the same hash directory', async () => {
  const f=fixture();
  f.queue.enqueue({id:'game',title:'Game'},f.release,'/data');
  await until(()=>f.tasks.length===1);
  const directory=`/data/downloads/torrents/${f.key}`;
  const path=directory+'/Game.exfat';
  f.nodes.set(directory,{isDirectory:true});
  f.nodes.set(path,{isFile:true,size:100});
  f.tasks[0].resolve({files:[{path,size:100}]});
  await until(()=>f.queue.snapshot()[0].state==='downloaded');
  assert.equal(f.queue.snapshot()[0].published[0],'/data/homebrew/game.exfat');
  assert.equal(f.fs.exists('/data/homebrew/game.exfat'),true);
  f.nodes.set(`/data/downloads/torrents/.resume/${f.key}.resume`,{isFile:true,size:1});
  assert.throws(()=>f.queue.enqueue({id:'other',title:'Other'},{...f.release,id:'other'},'/data'),/owns this filename/);
});

test('an unpacked PS5 game folder is published through the existing game selector', async () => {
  const f=fixture();
  f.queue.enqueue({id:'game',title:'Game'},f.release,'/data');
  await until(()=>f.tasks.length===1);
  const directory=`/data/downloads/torrents/${f.key}`;
  for (const path of [directory,directory+'/Game',directory+'/Game/sce_sys']) f.nodes.set(path,{isDirectory:true});
  const files=[{path:directory+'/Game/eboot.bin',size:50},{path:directory+'/Game/sce_sys/param.json',size:50}];
  for (const {path,size} of files) f.nodes.set(path,{isFile:true,size,data:'{}'});
  f.tasks[0].resolve({files});
  await until(()=>f.queue.snapshot()[0].state==='downloaded');
  assert.equal(f.queue.snapshot()[0].artifact.destination,'/data/homebrew/game-app');
  assert.equal(f.fs.exists('/data/homebrew/game-app/eboot.bin'),true);
  assert.equal(f.fs.stat('/data/homebrew/game-app').mode,0o777);
  assert.equal(f.fs.stat('/data/homebrew/game-app/eboot.bin').mode,0o777);
  assert.equal(f.fs.stat('/data/homebrew/game-app/sce_sys/param.json').mode,0o777);
});

test('empty or escaped torrent payload fails without publishing anything', async () => {
  for (const files of [[],[{path:'/data/elsewhere/game.pkg',size:100}]]) {
    const f=fixture();
    f.queue.enqueue({id:'game',title:'Game'},f.release,'/data');
    await until(()=>f.tasks.length===1);
    f.tasks[0].resolve({files});
    await until(()=>f.queue.snapshot()[0].state==='failed');
    assert.equal(f.httpTasks.length,0);
  }
});

test('ordinary HTTP PKG still uses the original HTTP backend', async () => {
  const f=fixture();
  const release={id:'http',format:'pkg',filename:'game.pkg',url:'https://example.org/game.pkg',sizeBytes:100};
  f.queue.enqueue({id:'game',title:'Game'},release,'/data');
  await until(()=>f.httpTasks.length===1);
  assert.equal(f.tasks.length,0);
  assert.equal(f.options[0].url,release.url);
  assert.equal(f.options[0].connections,64);
  f.httpTasks[0].resolve({});
  await until(()=>f.queue.snapshot()[0].state==='installer-required');
});

test('public Torrent wrapper uses existing task bridge and releases a completed handle', async () => {
  let tick, snapshots=[], released=[];
  const api={add:()=>7,poll:()=>snapshots,cancel(){},remove:(...args)=>released.push(args)};
  const {Torrent}=evaluate(planned+'ps5-react/runtime/js/native.js',['Torrent'],{
    __ps5ReactNative:{torrents:api},setInterval:fn=>(tick=fn,1),clearInterval(){},
    gamepadSource:()=>({subscribe(){}}),onFrameUnbatched(){},useEffect(){},useLayoutEffect(){},useRef(){},useState(){}
  });
  assert.equal(Torrent.available,true);
  const t=Torrent.enqueue({uri:magnet,saveRoot:'/data'});
  snapshots=[{id:7,state:'completed',files:[{path:'/data/file.pkg',size:5}],written:5}];
  tick();
  assert.equal((await t.done).files[0].size,5);
  await Promise.resolve();
  assert.equal(released[0][0],7);
  assert.equal(released[0][1],false);
});

test('user catalog enables valid magnets only when the host supports torrents', () => {
  const data={games:[{title:'Game',platform:'ps4',source:magnet,region:'EUR',size_gb:1},{title:'Bad',platform:'ps4',source:'magnet:?dn=bad'}]};
  for (const available of [true,false]) {
    const {userCatalog}=evaluate(planned+'app/data/user-catalog.js',['userCatalog'],{
      FileSystem:{readFile:()=>JSON.stringify(data)},Torrent:{available},torrentKey:source().torrentKey
    });
    assert.equal(userCatalog.games.length,available?1:0);
    if (available) {
      assert.equal(userCatalog.games[0].releases[0].type,'torrent');
      assert.equal(userCatalog.games[0].releases[0].sizeBytes,10**9);
    }
  }
});

test('failed torrent tasks release their native slot and retain the original failure', async () => {
  let tick, snapshots=[], released=[];
  const api={add:()=>8,poll:()=>snapshots,cancel(){},remove:(...args)=>released.push(args)};
  const {Torrent}=evaluate(planned+'ps5-react/runtime/js/native.js',['Torrent'],{
    __ps5ReactNative:{torrents:api},setInterval:fn=>(tick=fn,1),clearInterval(){},
    gamepadSource:()=>({subscribe(){}}),onFrameUnbatched(){},useEffect(){},useLayoutEffect(){},useRef(){},useState(){}
  });
  const task=Torrent.enqueue({uri:magnet,saveRoot:'/data'});
  snapshots=[{id:8,state:'failed',error:'Resume save failed'}];
  tick();
  await assert.rejects(task.done,/Resume save failed/);
  await Promise.resolve();
  assert.equal(released.length,1);
  assert.equal(released[0][0],8);
  assert.equal(released[0][1],false);
});
