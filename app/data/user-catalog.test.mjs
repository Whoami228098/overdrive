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
