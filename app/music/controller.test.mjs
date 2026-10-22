import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
const code = readFileSync(new URL('./controller.js', import.meta.url), 'utf8');
const {createMusicController, normalizePlaylist, loadPlaylist} = await import('data:text/javascript;base64,' + Buffer.from(code).toString('base64'));

const playlist = {version: 1, tracks: [
  {id:'a', title:'DIRTY', artist:'OBLADAET', file:'music/a.mp3', cover:'covers/a.jpg', duration:127},
  {id:'b', title:'600hp', artist:'BATO', file:'music/b.mp3', duration:110},
  {id:'c', title:'OVERDOZE', artist:'BATO, OBLADAET', file:'music/c.mp3', duration:130},
]};
function fixture(input = playlist, random = () => 0) {
  let callback, id = 0, status = {}, cleared = 0;
  const calls = [];
  const music = {
    play(path) { calls.push(['play',path]); status={id:++id, state:'loading', position:0, duration:127}; return id; },
    getStatus: () => status,
    setPaused: value => calls.push(['pause',value]),
    setVolume: value => calls.push(['volume',value]),
    stop: () => calls.push(['stop']),
  };
  const fs = {stat: () => ({isFile:true,size:2000}),readFile: () => JSON.stringify(input)};
  const player = createMusicController({music,fs,roots:['/app0/media'],random,
    interval(fn) { callback=fn; return 1; },clear() { cleared++; }});
  return {player, calls, tick: () => callback?.(), update: patch => Object.assign(status,patch),
    get id() {return id;},get cleared() {return cleared;},music,fs};
}

test('music packs reject escaping paths, URIs, duplicate files and malformed records', () => {
  const tracks = normalizePlaylist({version:1,tracks:[...playlist.tracks, playlist.tracks[0],
    {file:'../escape.mp3'},{file:'/outside.mp3'},{file:'http://remote/track.mp3'},
    {file:'music/%2e%2e/escape.mp3'},{file:'music\\escape.mp3'},{file:'music/track.wav'},null]}, '/pack');
  assert.deepEqual(tracks.map(t=>t.id), ['a','b','c']);
  assert.equal(tracks[0].source,'/pack/music/a.mp3');
  assert.equal(tracks[0].cover,'file:///pack/covers/a.jpg');
  assert.deepEqual(normalizePlaylist({tracks:playlist.tracks},'/pack'),[]);
});

test('an absent or oversized external pack falls back to a valid bundled pack', () => {
  const seen=[];
  const fs={stat(path) { return {isFile:true,size:path.startsWith('/external') ? 1024**2 : 200}; },
    readFile(path) { seen.push(path);return JSON.stringify(playlist); }};
  assert.equal(loadPlaylist(fs,['/external','/app0/media']).length,3);
  assert.deepEqual(seen,['/app0/media/playlist.json']);
  assert.deepEqual(loadPlaylist({stat:()=>{throw Error('absent');}},['/missing']),[]);
});

test('autoplay chooses a random first track and enables audio without a user press', () => {
  const f=fixture(playlist,()=>0.8);
  f.player.start();
  assert.equal(f.player.getSnapshot().track.id,'c');
  assert.deepEqual(f.calls,[['volume',0.35],['play','/app0/media/music/c.mp3'],['pause',false]]);
  f.player.start();
  assert.equal(f.id,1);
});

test('pause keeps the same song and resume does not expose a skip action', () => {
  const f=fixture();f.player.start();
  f.update({state:'playing',position:10});f.tick();
  assert.equal(f.player.togglePaused(),true);
  assert.equal(f.player.getSnapshot().paused,true);
  f.update({state:'ended',position:127});f.tick();
  assert.equal(f.id,1);
  f.player.togglePaused();f.tick();
  assert.equal(f.id,2);
  assert.equal(f.player.getSnapshot().track.id,'b');
  assert.equal('next' in f.player,false);
  assert.equal('previous' in f.player,false);
});

test('completion stays random while avoiding an immediate repeat', () => {
  const f=fixture();f.player.start();
  const songs=[];
  for(let i=0;i<12;i++) {
    songs.push(f.player.getSnapshot().track.id);
    f.update({state:'ended'});f.tick();
  }
  assert.ok(songs.every((id,i)=>i===0 || id!==songs[i-1]));
});

test('a stale native task cannot end or fail the current song', () => {
  const f=fixture();f.player.start();
  f.update({id:999,state:'failed',error:'old request'});f.tick();
  assert.equal(f.id,1);
  assert.equal(f.player.getSnapshot().state,'loading');
});

test('bad files are skipped once and an entirely broken pack stops retrying', () => {
  const f=fixture();f.player.start();
  for(let i=0;i<3;i++) {f.update({state:'failed',error:'bad file'});f.tick();}
  assert.equal(f.id,3);
  assert.equal(f.player.getSnapshot().state,'failed');
  for(let i=0;i<20;i++) f.tick();
  assert.equal(f.id,3);
});

test('empty packs and missing audio do not stop the store startup', () => {
  const f=fixture({version:1,tracks:[]});f.player.start();
  assert.equal(f.player.getSnapshot().state,'empty');
  assert.equal(f.player.togglePaused(),false);
  assert.deepEqual(f.calls,[]);
  const missing=fixture();missing.music.play=()=>{throw Error('audio unavailable');};
  assert.doesNotThrow(()=>missing.player.start());
  assert.equal(missing.player.getSnapshot().state,'failed');
});

test('time polling only publishes changes once per displayed second', () => {
  const f=fixture();let notifications=0;
  f.player.subscribe(()=>notifications++);f.player.start();
  f.update({state:'playing',position:1.1});f.tick();
  const before=f.player.getSnapshot(),count=notifications;
  f.update({position:1.9});f.tick();
  assert.equal(f.player.getSnapshot(),before);
  assert.equal(notifications,count);
  f.update({position:2.0});f.tick();assert.equal(notifications,count+1);
});

test('stopping releases the timer and the next application session autoplays again', () => {
  const f=fixture();f.player.start();f.player.togglePaused();f.player.stop();
  assert.equal(f.cleared,1);assert.equal(f.calls.at(-1)[0],'stop');
  f.player.start();assert.equal(f.player.getSnapshot().paused,false);assert.equal(f.id,2);
  assert.deepEqual(f.calls.at(-1),['pause',false]);
});

test('a one-track music pack loops continuously', () => {
  const f=fixture({version:1,tracks:[playlist.tracks[0]]});f.player.start();
  f.update({state:'ended'});f.tick();assert.equal(f.id,2);assert.equal(f.player.getSnapshot().track.id,'a');
});
