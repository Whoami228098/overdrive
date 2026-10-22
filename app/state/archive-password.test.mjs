import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';
const code=readFileSync(new URL('./archive-password.js',import.meta.url),'utf8').replace(/^export .*$/gm,'');
const context=vm.createContext({URL});
vm.runInContext(code+'\nglobalThis.api={archivePassword,refreshArchiveMetadata};',context);
const {archivePassword,refreshArchiveMetadata}=context.api;
test('password refresh follows an exact magnet even when trackers changed',()=>{
  const stored={url:'magnet:?xt=urn:btih:aaa&tr=old'};
  const current=[{url:'magnet:?xt=urn:btih:bbb',archivePassword:'Other edition'},
    {url:'magnet:?xt=urn:btih:aaa&tr=new',archivePassword:'[Exact]',archiveMetadata:{unpackedSizeBytes:42}}];
  assert.equal(refreshArchiveMetadata(stored,current,url=>new URL(url).searchParams.get('xt')),'[Exact]');
  assert.equal(stored.archiveMetadata.unpackedSizeBytes,42);
});
test('a catalog password cannot leak between different release sources',()=>{
  const stored={url:'https://example.invalid/one.zip'};
  assert.equal(refreshArchiveMetadata(stored,[{url:'https://example.invalid/two.zip',archivePassword:'Wrong'}]),undefined);
});
test('volume-specific passwords still refuse conflicting source metadata',()=>{
  const stored={parts:[{sources:[{url:'https://example.invalid/one.rar'}]},{sources:[{url:'https://example.invalid/two.rar'}]}]};
  const current=[{parts:[{sources:[{url:'https://example.invalid/one.rar',archivePassword:'A'}]},
    {sources:[{url:'https://example.invalid/two.rar',archivePassword:'B'}]}]}];
  assert.throws(()=>refreshArchiveMetadata(stored,current),/conflicting/);
  assert.throws(()=>archivePassword({archivePassword:'bad\0password'}),/Invalid/);
});
