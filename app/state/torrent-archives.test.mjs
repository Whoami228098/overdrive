import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
const context=vm.createContext({});
vm.runInContext(readFileSync(new URL('./torrent-archives.js',import.meta.url),'utf8').replace('export function','function')+'\nglobalThis.choose=chooseTorrentArchive;',context);
const choose=(names,...args)=>context.choose(names.map(path=>({path,size:1})),...args);
test('classic RAR volumes begin with .rar even when metadata lists .r00 first',()=>{
  assert.deepEqual(Array.from(choose(['game.r01','game.rar','game.r00']).sources),['game.rar','game.r00','game.r01']);
});
test('numbered RAR and split 7z use numeric order and detect missing volumes',()=>{
  assert.deepEqual(Array.from(choose(['game.part03.rar','game.part01.rar','game.part02.rar']).sources),['game.part01.rar','game.part02.rar','game.part03.rar']);
  assert.deepEqual(Array.from(choose(['game.7z.002','game.7z.001']).sources),['game.7z.001','game.7z.002']);
  assert.match(choose(['game.7z.001','game.7z.003']).error,/Missing/);
});
test('a game archive stays separate from backports and original-file restoration copies',()=>{
  const plan=choose(['[4.xx] Backport for v1.008.7z','PPSA99994.7z','RusSound.7z','Other/Original_files for v1.008.7z'],'PPSA99994');
  assert.deepEqual(Array.from(plan.sources),['PPSA99994.7z']);
  assert.equal(plan.extras.length,3);
});
test('different complete games are refused until an exact archive is selected',()=>{
  assert.match(choose(['Game A.7z','Game B.7z']).error,/independent/);
  assert.deepEqual(Array.from(choose(['Game A.7z','Game B.7z'],null,'Game B.7z').sources),['Game B.7z']);
});
test('firmware overlays and language packs cannot displace the title-bearing base game',()=>{
  assert.deepEqual(Array.from(choose(['REANIMAL [PPSA99994].zip','Reanimal_4xx[PPSA99994][01.000.006].zip'],'PPSA99994').sources),['REANIMAL [PPSA99994].zip']);
  assert.deepEqual(Array.from(choose(['PPSA99994.7z','FullRus/FullRus.7z','Anniversary Upgrade/Upgrade.7z'],'PPSA99993').sources),['PPSA99994.7z']);
});
