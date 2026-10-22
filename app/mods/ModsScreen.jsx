import '../assets/Inter-Regular.ttf';
import {useEffect, useMemo, useState} from 'react';
import {FileSystem, FocusScope, Platform, ScrollView, Text, View} from '@ps5-react/core';
import {useT} from '../i18n/index.js';
import {fitBytes} from '../i18n/fit.js';
import {useNavActions} from '../state/nav.jsx';
import {Keyboard} from '../search/Keyboard.jsx';
import {Button2} from '../ui/Button.jsx';
import {createModInstaller, installedIdentity, loadModIndex, loadTitleMods, loaderTarget, sameVersion, validId} from './core.js';

const PAGE_SIZE=7;
const INK={fontFamily:'Inter-Regular',fontSize:23,lineHeight:32,color:'#f0f4f8'};
const MUTED={...INK,fontSize:20,lineHeight:27,color:'#a3b0bd'};

function Row({title,detail,onPress,autoFocus}) {
  return <View autoFocus={autoFocus} onPress={onPress} style={({focused})=>({height:88,
    borderWidth:2,borderColor:focused?'#b8e5ff':'#ffffff18',backgroundColor:focused?'#23313e':'#0b1019e6',
    paddingHorizontal:22,paddingVertical:10,borderRadius:10,justifyContent:'center',gap:3})}>
    <Text style={{...INK,numberOfLines:1,ellipsizeMode:'tail'}}>{fitBytes(title)}</Text>
    <Text style={{...MUTED,numberOfLines:1,ellipsizeMode:'tail'}}>{fitBytes(detail)}</Text>
  </View>;
}

export function ModsScreen({titleId:initialId,onBack}) {
  const t=useT(), host=Platform.OS==='ps4'?'ps4':'ps5';
  const {focusLater}=useNavActions();
  const [catalog]=useState(()=>loadModIndex(FileSystem,host==='ps5'?['/data/homebrew/trapstar-mods','/app0/mods']:['/app0/mods'],host));
  const [query,setQuery]=useState(''), [page,setPage]=useState(0);
  const [titleId,setTitleId]=useState(validId(initialId)?initialId:null);
  const [record,setRecord]=useState(null), [game,setGame]=useState(null), [probing,setProbing]=useState(false);
  const [busy,setBusy]=useState(false), [notice,setNotice]=useState('');
  const titles=useMemo(()=>catalog.titles.filter(x=>`${x.title} ${x.titleId}`.toLowerCase().includes(query.trim().toLowerCase()))
    .sort((a,b)=>a.title.toLowerCase().localeCompare(b.title.toLowerCase())),[catalog,query]);
  const packs=useMemo(()=>{
    try {return titleId?loadTitleMods(FileSystem,catalog,host,titleId):[];} catch {return [];}
  },[catalog,host,titleId]);
  const installer=useMemo(()=>createModInstaller({fs:FileSystem,host,root:catalog.root,
    stateDir:`${FileSystem.dataDir}/overdrive/mods`,identity:id=>installedIdentity(FileSystem,host,id),
    log:message=>console.log(`[overdrive] ${message}`)}),[catalog,host]);
  useEffect(()=>{
    let live=true;
    setGame(null);
    if (!titleId) return;
    setProbing(true);
    installedIdentity(FileSystem,host,titleId).then(value=>{if(live){setGame(value);setProbing(false);}},()=>{if(live)setProbing(false);});
    return ()=>{live=false;};
  },[titleId,host]);
  const select=id=>{setTitleId(id);setRecord(null);setPage(0);setNotice('');};
  const back=()=>{
    if(busy)return true;
    if(record){setRecord(null);setNotice('');}
    else if(titleId){setTitleId(null);setPage(0);}
    else onBack();
    return true;
  };
  const action=async kind=>{
    if(busy || !record)return;
    setBusy(true);setNotice(t('mods.working'));
    try {
      const result=kind==='restore'?await installer.restore(record):await installer.install(record,{exportOnly:kind==='export'});
      setNotice(result.state==='restored'?t('mods.restored'):kind==='export'?t('mods.exported'):
        t(`mods.activation.${result.activation}`));
    } catch(error){setNotice(`${t('mods.failed')} ${error.message}`);}
    finally{setBusy(false);}
  };
  const list=titleId?packs:titles, pages=Math.max(1,Math.ceil(list.length/PAGE_SIZE));
  const shown=list.slice(page*PAGE_SIZE,(page+1)*PAGE_SIZE);
  const target=record?loaderTarget(FileSystem,host,record):null;
  const compatible=record && game?.titleId===record.titleId && sameVersion(game.version,record.version);
  return <FocusScope autoFocus onBack={back} onAction={action=>{
    if(!titleId && action==='l2'){setQuery('');setPage(0);return true;}
    if(!titleId && action==='r2'){focusLater('mods-results');return true;}
    return false;
  }}>
    <View style={{flex:1,paddingHorizontal:48,paddingTop:28,paddingBottom:24,gap:20}}>
      <View style={{flexDirection:'row',alignItems:'center',justifyContent:'space-between'}}>
        <Text style={{...INK,fontSize:36,lineHeight:46,fontWeight:600}}>{t('mods.title')}{titleId?` · ${titleId}`:''}</Text>
        <Button2 label={t('hints.back')} onPress={back}/>
      </View>
      <View style={{flex:1,flexDirection:'row',gap:40}}>
        <View style={{width:560,gap:20}}>
          {!titleId?<>
            <View style={{height:62,padding:14,borderRadius:10,backgroundColor:'#15202ce6'}}>
              <Text style={{...INK,numberOfLines:1}}>{query||t('mods.search')}</Text>
            </View>
            <Keyboard onChange={value=>{setQuery(value);setPage(0);}} onSubmit={()=>focusLater('mods-results')}
              focusKey="mods-keyboard" resultsFocus="mods-results"/>
          </>:<>
            <Text style={{...INK,fontSize:28,lineHeight:38}}>{fitBytes(catalog.titles.find(x=>x.titleId===titleId)?.title??titleId)}</Text>
            <Text style={MUTED}>{probing?t('mods.checking'):game?t('mods.installedVersion',{version:game.version}):t('mods.notInstalled')}</Text>
            <Text style={MUTED}>{t('mods.versionHelp')}</Text>
            <Button2 label={t('mods.allTitles')} onPress={()=>{setRecord(null);select(null);}}/>
            {record?<>
              <Text style={INK}>{record.version} · {record.format.toUpperCase()}</Text>
              <Text style={MUTED}>{t('mods.authors',{authors:record.authors.join(', ')||'Community'})}</Text>
              <Text style={MUTED}>{target?t('mods.loader',{loader:target.loader}):t('mods.loaderMissing')}</Text>
              {target&&compatible&&!busy?<Button2 variant="primary" label={t('mods.install')} onPress={()=>action('install')}/>:null}
              {!busy?<Button2 label={t('mods.export')} onPress={()=>action('export')}/>:null}
              {!busy?<Button2 label={t('mods.restore')} onPress={()=>action('restore')}/>:null}
              <Text style={MUTED}>{t('mods.activationHelp')}</Text>
            </>:null}
          </>}
          {!titleId?<Text style={MUTED}>{t('mods.catalogHelp')}</Text>:null}
        </View>
        <FocusScope focusKey="mods-results"><View style={{flex:1,gap:10}}>
          {record?<>
            <Text style={{...INK,fontSize:28,lineHeight:38}}>{t('mods.features')}</Text>
            <ScrollView style={{flex:1}}>
              <View style={{gap:12,paddingBottom:24}}>{record.features.map((name,index)=><Text key={index} style={INK}>{fitBytes(`• ${name}`)}</Text>)}</View>
            </ScrollView>
            <Text style={MUTED}>{record.source?.repository??''}</Text>
          </>:<>
            <View key={`${titleId??'index'}:${page}`} style={{flex:1,gap:8}}>
              {shown.map((item,index)=>titleId?<Row key={item.id} autoFocus={index===0}
                title={`${item.version} · ${item.format.toUpperCase()}`}
                detail={`${item.features.length} ${t('mods.features')} · ${item.authors.join(', ')||'Community'}`}
                onPress={()=>{setRecord(item);setNotice('');}}/>:
                <Row key={item.titleId} autoFocus={index===0} title={item.title}
                  detail={`${item.titleId} · ${item.versions?.join(' / ')??''}`} onPress={()=>select(item.titleId)}/>)}
              {!shown.length?<Text style={INK}>{t('mods.empty')}</Text>:null}
            </View>
            <View style={{flexDirection:'row',gap:24,alignItems:'center',justifyContent:'space-between'}}>
              <Button2 icon="chevronLeft" onPress={()=>setPage(value=>Math.max(0,value-1))}/>
              <Text style={MUTED}>{`${page+1} / ${pages} · ${list.length}`}</Text>
              <Button2 icon="chevronRight" onPress={()=>setPage(value=>Math.min(pages-1,value+1))}/>
            </View>
          </>}
          {notice?<Text style={{...MUTED,color:'#d5eaff',numberOfLines:4,ellipsizeMode:'tail'}}>{fitBytes(notice)}</Text>:null}
        </View></FocusScope>
      </View>
    </View>
  </FocusScope>;
}
