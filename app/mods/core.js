// Mod data is selected by the console host as well as the game's title/version.
// Applying memory changes remains the compatible loader's job.
import {sha256} from './vendor/noble-hashes/sha2.js';
import {bytesToHex} from './vendor/noble-hashes/utils.js';

export const MAX_FILE = 1024 * 1024;
export const validId = value => /^(CUSA|PPSA)\d{5}$/.test(value ?? '');
export const validVersion = value => typeof value === 'string' && /^\d{1,3}(?:\.\d{1,3}){1,3}$/.test(value);
export const sameVersion = (a, b) => validVersion(a) && validVersion(b) &&
  a.split('.').map(Number).join('.') === b.split('.').map(Number).join('.');
const hashPattern = /^[a-f0-9]{64}$/;
const tokenPattern = /^[a-f0-9]{24}$/;
const formats = new Set(['json','shn','xml']);
const cleanText = (value, fallback = '') => typeof value === 'string' ? value.replace(/\0/g,'').trim().slice(0,256) : fallback;

export function readJson(fs, path) {
  const stat = fs.stat(path);
  if (!stat?.isFile || stat.size < 1 || stat.size > MAX_FILE) throw new Error('Invalid mod metadata file');
  return JSON.parse(fs.readFile(path));
}

export function normalizeRecord(item, host, titleId) {
  if (!item || !tokenPattern.test(item.id) || !validId(titleId) || item.titleId !== titleId ||
      item.host !== host || !['ps4','ps5'].includes(host) || host === 'ps4' && !titleId.startsWith('CUSA') ||
      !validVersion(item.version) || !formats.has(item.format) || !hashPattern.test(item.sha256) ||
      item.file !== `payload/${item.sha256}.${item.format}` ||
      !Number.isSafeInteger(item.bytes) || item.bytes < 1 || item.bytes > MAX_FILE ||
      !Array.isArray(item.features) || !item.features.length) return null;
  const platform = titleId.startsWith('PPSA') ? 'ps5' : 'ps4';
  if (item.platform !== platform || item.loader !== (item.format === 'xml' ? 'patches' : 'cheats')) return null;
  return {...item, title:cleanText(item.title,titleId),
    features:item.features.slice(0,256).map(x=>cleanText(x)).filter(Boolean),
    authors:Array.isArray(item.authors) ? item.authors.slice(0,32).map(x=>cleanText(x)).filter(Boolean) : [],
    categories:Array.isArray(item.categories) ? item.categories.filter(x=>['graphics','appearance','money','menu','gameplay'].includes(x)) : []};
}

export function loadModIndex(fs, roots, host) {
  if (!['ps4','ps5'].includes(host)) return {root:null,titles:[]};
  for (const root of roots) {
    try {
      const data = readJson(fs,`${root}/hosts/${host}/index.json`);
      if (data.version !== 1 || data.host !== host || !Array.isArray(data.titles)) continue;
      const seen = new Set();
      const titles = data.titles.slice(0,5000).filter(item=>{
        if (!item || !validId(item.titleId) || seen.has(item.titleId) || host==='ps4' && !item.titleId.startsWith('CUSA') ||
            item.file !== `hosts/${host}/titles/${item.titleId}.json`) return false;
        seen.add(item.titleId); return true;
      }).map(item=>({...item,title:cleanText(item.title,item.titleId)}));
      if (titles.length) return {root,titles};
    } catch { /* An absent optional pack cannot stop the store. */ }
  }
  return {root:null,titles:[]};
}

export function loadTitleMods(fs, catalog, host, titleId) {
  if (!catalog.root || !validId(titleId)) return [];
  const item = catalog.titles.find(x=>x.titleId===titleId);
  if (!item) return [];
  const data=readJson(fs,`${catalog.root}/${item.file}`);
  if (data.version !== 1 || !Array.isArray(data.records)) return [];
  return data.records.slice(0,256).map(x=>normalizeRecord(x,host,titleId)).filter(Boolean);
}

function ascii(bytes, start, length) {
  if (start < 0 || length < 1 || start + length > bytes.length) throw new Error('Invalid SFO range');
  let result='';
  for (let i=start; i<start+length && bytes[i]; i++) {
    if (bytes[i] > 127) throw new Error('Non-ASCII SFO identity');
    result += String.fromCharCode(bytes[i]);
  }
  return result;
}

export function parseSfo(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.length < 20 || bytes.length > MAX_FILE) throw new Error('Invalid SFO size');
  const view = new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
  if (view.getUint32(0,true) !== 0x46535000 || view.getUint32(4,true) !== 0x101) throw new Error('Invalid SFO header');
  const keys=view.getUint32(8,true), values=view.getUint32(12,true), count=view.getUint32(16,true);
  if (count > 256 || keys < 20+count*16 || values < keys || values > bytes.length) throw new Error('Invalid SFO tables');
  const result={};
  for (let index=0;index<count;index++) {
    const base=20+index*16;
    const key=keys+view.getUint16(base,true), format=view.getUint16(base+2,true);
    const length=view.getUint32(base+4,true), maximum=view.getUint32(base+8,true), offset=values+view.getUint32(base+12,true);
    if (key >= values || length > maximum || offset+maximum > bytes.length) throw new Error('Invalid SFO entry');
    const name=ascii(bytes,key,values-key);
    if (['TITLE_ID','APP_VER'].includes(name) && format===0x204) result[name]=ascii(bytes,offset,length);
  }
  return validId(result.TITLE_ID) && validVersion(result.APP_VER) ? {titleId:result.TITLE_ID,version:result.APP_VER} : null;
}

export async function installedIdentity(fs, host, titleId, yieldFrame = () => new Promise(resolve=>setTimeout(resolve,0))) {
  if (!validId(titleId) || host==='ps4' && titleId.startsWith('PPSA')) return null;
  const roots=[`/user/appmeta/${titleId}`,`/system_data/priv/appmeta/${titleId}`];
  for (const root of roots) {
    for (const format of ['json','sfo']) {
      try {
        let identity;
        if (format==='json') {
          const data=readJson(fs,`${root}/param.json`);
          identity={titleId:data.titleId,version:data.contentVersion};
        } else identity=parseSfo(fs.readBytes(`${root}/param.sfo`));
        if (identity?.titleId===titleId && validVersion(identity.version)) return {...identity,path:root};
      } catch { /* Try the next canonical source; never infer a version from catalog text. */ }
      await yieldFrame();
    }
  }
  if (host==='ps5') {
    let mounts=[];
    try { mounts=fs.readDir('/mnt/shadowmnt').filter(x=>x.isDirectory && /^[A-Za-z0-9_.-]+$/.test(x.name) && !['.','..'].includes(x.name)).slice(0,256); } catch {}
    for (const mount of mounts) {
      const path=`/mnt/shadowmnt/${mount.name}/sce_sys`;
      try {
        const data=readJson(fs,`${path}/param.json`);
        if (data.titleId===titleId && validVersion(data.contentVersion)) return {titleId,version:data.contentVersion,path};
      } catch {}
      await yieldFrame();
    }
  }
  return null;
}

export function loaderTarget(fs, host, record) {
  const filename=`${record.titleId}_${record.version}.${record.format}`;
  if (host==='ps5' && fs.stat('/data/cheatrunner')?.isDirectory) {
    const dir=record.format==='xml' ? `patches/${record.platform==='ps5' ? 'xml_prospero' : 'xml'}` : `cheats/${record.format}`;
    return {loader:'CheatRunner',directory:`/data/cheatrunner/${dir}`,filename,activation:'cheatrunner'};
  }
  if (host==='ps5' && record.format!=='xml' && fs.stat('/data/etaHEN')?.isDirectory) {
    return {loader:'etaHEN',directory:`/data/etaHEN/cheats/${record.format}`,filename,activation:'etahen'};
  }
  if (host==='ps4') {
    const root=['/data/GoldHEN','/user/data/GoldHEN'].find(path=>fs.stat(path)?.isDirectory);
    if (root) return {loader:'GoldHEN',directory:`${root}/${record.format==='xml' ? 'patches/xml' : `cheats/${record.format}`}`,
      filename:record.format==='xml' ? `${record.titleId}.xml` : filename,activation:record.format==='xml' ? 'goldhenPatch' : 'goldhen'};
  }
  return null;
}

export async function digestBytes(bytes, yieldFrame=()=>new Promise(resolve=>setTimeout(resolve,0))) {
  if (!(bytes instanceof Uint8Array) || bytes.length>MAX_FILE) throw new Error('Invalid mod payload size');
  const hash=sha256.create();
  for (let offset=0;offset<bytes.length;offset+=16384) {
    hash.update(bytes.subarray(offset,offset+16384));
    if (offset+16384<bytes.length) await yieldFrame();
  }
  return bytesToHex(hash.digest());
}

// Atomic file copies plus an app-owned receipt keep an existing user's mod recoverable.
// Restore refuses files changed by another program after our installation.
export function createModInstaller({fs, host, root, stateDir, identity, yieldFrame, log=()=>{}}) {
  let busy=false;
  const hashFile=async path=>{
    const stat=fs.stat(path);
    if (!stat?.isFile || stat.size>MAX_FILE) throw new Error('Mod file is unavailable or too large');
    return digestBytes(fs.readBytes(path),yieldFrame);
  };
  const receiptPath=(record,exportOnly=false)=>`${stateDir}/${record.id}${exportOnly?'.export':''}.json`;
  const save=(record,receipt)=>{
    fs.mkdir(stateDir,{recursive:true});
    const path=receiptPath(record,receipt.exportOnly);
    fs.writeFile(`${path}.tmp`,JSON.stringify(receipt),{sync:true}); fs.rename(`${path}.tmp`,path);
  };
  return {
    async install(raw, {exportOnly=false}={}) {
      if (busy) throw new Error('Another mod operation is in progress');
      const record=normalizeRecord(raw,host,raw?.titleId);
      if (!record) throw new Error('Invalid or incompatible mod record');
      busy=true;
      try {
        const payload=`${root}/${record.file}`;
        if (fs.stat(payload)?.size!==record.bytes || await hashFile(payload)!==record.sha256) throw new Error('Mod checksum mismatch');
        const target=exportOnly ? null : loaderTarget(fs,host,record);
        if (!exportOnly && !target) throw new Error('No compatible mod loader is configured');
        if (!exportOnly) {
          const game=await identity(record.titleId);
          if (game?.titleId!==record.titleId || !sameVersion(game.version,record.version)) throw new Error('The installed game version does not match this mod');
        }
        const directory=target?.directory ?? `${stateDir}/exports/${record.titleId}`;
        const filename=target?.filename ?? `${record.titleId}_${record.version}.${record.format}`;
        const destination=`${directory}/${filename}`;
        const backup=`${stateDir}/${record.id}.backup`;
        let receipt;
        try { receipt=readJson(fs,receiptPath(record,exportOnly)); } catch {}
        const current=fs.stat(destination);
        if (current && !current.isFile) throw new Error('Mod destination is not a regular file');
        if (current && await hashFile(destination)===record.sha256) return {state:'saved',loader:target?.loader,activation:target?.activation,destination};
        // A second mod may target the same loader file; never overwrite our earlier undo receipt.
        if (receipt && receipt.state!=='restored') throw new Error('Restore the previous installation before replacing this mod');
        let previous=null;
        fs.mkdir(stateDir,{recursive:true});
        if (current) {
          previous=await hashFile(destination);
          fs.copyFile(destination,backup);
          if (await hashFile(backup)!==previous) throw new Error('Could not verify the mod backup');
        }
        const prepared={version:1,state:'prepared',id:record.id,host,titleId:record.titleId,
          gameVersion:record.version,sha256:record.sha256,previous,exportOnly,destination,loader:target?.loader??null};
        save(record,prepared);
        fs.mkdir(directory,{recursive:true});
        // Recheck after the asynchronous hashing/probe, before the atomic replacement.
        const before=fs.stat(destination);
        if (previous ? !before?.isFile || await hashFile(destination)!==previous : before) throw new Error('Mod destination changed during installation');
        if (!exportOnly) {
          const game=await identity(record.titleId);
          if (game?.titleId!==record.titleId || !sameVersion(game.version,record.version)) throw new Error('The installed game version changed during installation');
        }
        fs.copyFile(payload,destination);
        if (await hashFile(destination)!==record.sha256) throw new Error('Written mod checksum mismatch; backup retained');
        save(record,{...prepared,state:'saved'});
        log(`mod saved id=${record.id} title=${record.titleId} loader=${target?.loader??'export'}`);
        return {state:'saved',loader:target?.loader,activation:target?.activation,destination};
      } finally { busy=false; }
    },
    async restore(raw) {
      if (busy) throw new Error('Another mod operation is in progress');
      const record=normalizeRecord(raw,host,raw?.titleId);
      if (!record) throw new Error('Invalid mod record');
      busy=true;
      try {
        let receipt;
        try { receipt=readJson(fs,receiptPath(record)); }
        catch { receipt=readJson(fs,receiptPath(record,true)); }
        const target=receipt.exportOnly ? {directory:`${stateDir}/exports/${record.titleId}`,filename:`${record.titleId}_${record.version}.${record.format}`} : loaderTarget(fs,host,record);
        if (!target || receipt.version!==1 || receipt.id!==record.id || receipt.host!==host ||
            receipt.sha256!==record.sha256 || receipt.destination!==`${target.directory}/${target.filename}` ||
            !['prepared','saved'].includes(receipt.state)) throw new Error('No matching restore receipt');
        const present=fs.stat(receipt.destination);
        const current=present ? await hashFile(receipt.destination) : null;
        // A power loss or failed probe can leave prepared intent before the atomic copy.
        if (receipt.state==='prepared' && current===receipt.previous) {
          save(record,{...receipt,state:'restored'});
          return {state:'restored'};
        }
        if (current!==record.sha256) throw new Error('This mod was changed outside the store; its file was kept');
        if (receipt.previous) {
          if (!hashPattern.test(receipt.previous) || await hashFile(`${stateDir}/${record.id}.backup`)!==receipt.previous) throw new Error('Mod backup checksum mismatch');
          fs.copyFile(`${stateDir}/${record.id}.backup`,receipt.destination);
          if (await hashFile(receipt.destination)!==receipt.previous) throw new Error('Restored mod checksum mismatch');
        } else fs.remove(receipt.destination);
        save(record,{...receipt,state:'restored'});
        return {state:'restored'};
      } finally { busy=false; }
    },
  };
}
