// Distinct archives must never be concatenated as if they were volumes.
// Optional backports, saves and soundtracks stay downloaded; installing them
// needs their own compatibility checks instead of silently replacing game files.
const optional = /(?:^|[/\s_.()[\]-])(?:backports?|backpork|original(?:[_\s-]*files)?|fix(?:es)?|patch(?:es)?|dlc|unlocker|soundtrack|ost|save(?:file|data)?s?|squads|mods?|cheats?|bonus|russound)(?=$|[/\s_.()[\]-])/i;
const firmwarePatch = /(?:^|[/_\s[(])(?:\d{1,2}\.?xx|\d{1,2}\.x)(?=$|[\]_[.\s-])/i;

export function chooseTorrentArchive(files, titleId, requested) {
  const groups=new Map();
  for(const file of files) {
    const path=file.path;
    let match,key,order,first=0;
    if((match=/^(.*)\.part(\d+)\.rar$/i.exec(path))) {key=match[1]+'.rar';order=Number(match[2]);first=1;}
    else if((match=/^(.*\.(?:7z|zip))\.(\d{3})$/i.exec(path))) {key=match[1];order=Number(match[2]);first=1;}
    else if((match=/^(.*)\.r(\d{2,3})$/i.exec(path))) {key=match[1]+'.rar';order=Number(match[2])+1;}
    else if(/\.(?:rar|7z|zip|tar(?:\.(?:gz|bz2|xz|zst))?|tgz)$/i.test(path)) {key=path;order=0;}
    else continue;
    const index=key.toLowerCase();
    let group=groups.get(index);
    if(!group)groups.set(index,group={key,first,parts:[],bytes:0});
    group.parts.push({path,order});group.bytes+=file.size;
  }
  const all=[...groups.values()];
  let candidates=requested?all.filter(group=>group.key===requested||group.parts.some(part=>part.path===requested)):
    all.filter(group=>!optional.test(group.key));
  if(!requested&&candidates.length>1)candidates=candidates.filter(group=>!firmwarePatch.test(group.key));
  if(!requested&&candidates.length>1&&/^(?:PPSA|CUSA)\d{5}$/i.test(titleId||'')) {
    const exact=candidates.filter(group=>group.key.toUpperCase().includes(titleId.toUpperCase()));
    if(exact.length===1)candidates=exact;
  }
  if(!requested&&candidates.length>1) {
    // Source posts sometimes list another region's ID before the base game's
    // own ID. A single title-bearing archive still distinguishes that game
    // from translation/upgrade packs with no console title ID.
    const titled=candidates.filter(group=>/(?:PPSA|CUSA)\d{5}(?!\d)/i.test(group.key));
    if(titled.length===1)candidates=titled;
  }
  if(!candidates.length)return {sources:[],extras:all.map(group=>group.key),
    error:requested?'The selected archive is missing from this torrent. Correct its archiveFile catalog field.':null};
  if(candidates.length!==1)return {sources:[],extras:all.map(group=>group.key),
    error:'This torrent contains several independent game archives. Select one with its exact archiveFile catalog field; downloaded files were preserved.'};
  const selected=candidates[0];
  selected.parts.sort((a,b)=>a.order-b.order);
  const valid=selected.parts.length<=1024&&selected.parts.every((part,index)=>part.order===index+selected.first);
  if(!valid)return {sources:[],extras:all.map(group=>group.key),error:'Missing or duplicate archive volumes. Keep the downloaded files and inspect this source.'};
  return {sources:selected.parts.map(part=>part.path),key:selected.key,bytes:selected.bytes,
    extras:all.filter(group=>group!==selected).map(group=>group.key),error:null};
}
