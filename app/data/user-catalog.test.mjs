import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const source = readFileSync(new URL('./user-catalog.js', import.meta.url), 'utf8')
  .replace(/^import .*;\r?$/gm, '').replace(/^export .*;\r?$/gm, '');

function load(data) {
  const path = '/data/homebrew/catalog-user.json';
  const context = vm.createContext({URL, Torrent:{available:true},
    torrentKey:()=>'0123456789abcdef0123456789abcdef01234567',
    FileSystem:{readFile:requested=>{
      if (requested !== path) throw new Error('ENOENT');
      return JSON.stringify(data);
    }}});
  vm.runInContext(source + '\n;globalThis.loaded = userCatalog;', context);
  return context.loaded;
}

const game = {title:'Synthetic user game',platform:'ps4',source:'https://example.invalid/test.pkg'};
const mod = {title:'Synthetic mod',game_title:game.title,platform:'ps4',source:'https://example.invalid/mod.pkg'};

test('malformed mod records cannot prevent valid games and mods from loading', () => {
  const catalog = load({games:[null,game],mods:[null,[],0,'bad',{},mod]});
  assert.equal(catalog.games.length,1);
  assert.equal(catalog.games[0].title,game.title);
  assert.equal(catalog.mods.length,1);
  assert.equal(catalog.mods[0].title,mod.title);
});

test('mod text fields reject objects before they reach a rendered catalog', () => {
  const catalog = load({games:[game],mods:[{...mod,title:{}},{...mod,source:{}},mod]});
  assert.equal(catalog.mods.length,1);
  assert.equal(catalog.mods[0].game,game.title);
});

test('malformed optional mod values cannot throw during numeric coercion', () => {
  const catalog = load({mods:[{...mod,size_mb:{valueOf:null,toString:null},version:{},cover_url:[]}]});
  assert.equal(catalog.mods.length,1);
  assert.equal(catalog.mods[0].sizeMb,null);
  assert.equal(catalog.mods[0].version,null);
  assert.equal(catalog.mods[0].coverUrl,null);
});

test('malformed optional game text cannot crash the real title-page text formatter', () => {
  const catalog = load({games:[{...game,developer:{},publisher:[],region:{},dub:[],version:{}}]});
  const fitSource = readFileSync(new URL('../i18n/fit.js',import.meta.url),'utf8').replace(/^export .*;\r?$/gm,'');
  const context = vm.createContext({});
  vm.runInContext(fitSource + '\n;globalThis.fit = fitBytes;',context);
  assert.doesNotThrow(()=>context.fit(catalog.games[0].developer));
  assert.equal(typeof catalog.games[0].publisher,'string');
  assert.equal(catalog.games[0].region,null);
  assert.equal(catalog.games[0].dub,null);
  assert.equal(catalog.games[0].userReleases[0].version,null);
});

test('published passwords and exact archive sizes survive each user release route', () => {
  for (const source of ['magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567',
                        'https://example.invalid/game.7z','https://example.invalid/game.pkg']) {
    const metadata={unpackedSizeBytes:286351425536,encryption:'none',kind:'archive'};
    const catalog=load({games:[{...game,source,archivePassword:'[Case Sensitive] ü',archiveMetadata:metadata,
      topic_url:'https://rutracker.org/forum/viewtopic.php?t=123'}]});
    const release=catalog.games[0].userReleases[0];
    assert.equal(release.archivePassword,'[Case Sensitive] ü');
    assert.equal(release.archiveMetadata.unpackedSizeBytes,286351425536);
    assert.equal(release.topicUrl,'https://rutracker.org/forum/viewtopic.php?t=123');
  }
});

test('source-specific passwords stay attached to their own mirror', () => {
  const catalog=load({games:[{...game,source:{url:game.source,archivePassword:'First'},
    mirrors:[{url:'https://example.invalid/other.pkg',archivePassword:'Second'}]}]});
  assert.deepEqual(Array.from(catalog.games[0].userReleases,release=>release.archivePassword),['First','Second']);
});

test('invalid and oversized UTF-8 passwords block that source without crashing the catalog', () => {
  for(const archivePassword of [{},'x\0y','ü'.repeat(513)]) {
    const catalog=load({games:[{...game,archivePassword}]});
    assert.equal(catalog.games.length,1);
    const release=catalog.games[0].userReleases[0];
    assert.match(release.unavailableReason,/invalid archive password/);
    assert.equal(release.archivePassword,undefined);
  }
});
