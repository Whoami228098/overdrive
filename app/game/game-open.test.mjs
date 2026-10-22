import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import test from 'node:test';
import vm from 'node:vm';

const repo = fileURLToPath(new URL('../../', import.meta.url));
const magnet = 'magnet:?xt=urn:btih:' + '0123456789abcdef'.repeat(2) + '01234567';
const noop = () => {};
const jsx = (type, props) => ({type, props});
function evaluate(file, names, injected = {}) {
  let code = readFileSync(repo + file, 'utf8');
  const imports = {};
  for (const match of code.matchAll(/^import \{([^}]+)\} from .*;\r?$/gm))
    for (const item of match[1].split(',')) {
      const name = item.trim().split(/\s+as\s+/).at(-1);
      imports[name] = name;
    }
  code = code.replace(/^import .*;\r?$/gm, '').replace(/^export \{[^}]+\};?\r?$/gm, '')
    .replace(/\bexport (const|let|var|function|class) /g, '$1 ');
  const context = vm.createContext({console: {log: noop}, setTimeout, clearTimeout,
    setInterval, clearInterval, ...imports, ...injected});
  vm.runInContext(code + ';globalThis.result = {' + names.join(',') + '};', context, {filename:file});
  return context.result;
}
function catalog() {
  let calls = 0;
  const forbidden = () => { calls++; throw new Error('Unexpected torrent API call while opening game'); };
  const Torrent = {available:true, enqueue:forbidden, start:forbidden, poll:forbidden};
  const data = {games:[
    {title:'User PS5', platform:'ps5', title_id:'PPSA10662', source:magnet, region:'EUR', size_gb:1},
    {title:'User PS4', platform:'ps4', title_id:'CUSA00001', source:magnet, size_gb:2},
    {title:'HTTP game', platform:'ps4', source:'https://example.org/game.pkg'}
  ]};
  const {torrentKey} = evaluate('app/state/download-sources.js', ['torrentKey']);
  const {userCatalog} = evaluate('app/data/user-catalog.js', ['userCatalog'], {
    FileSystem:{readFile:() => JSON.stringify(data)}, Torrent, torrentKey
  });
  const lists = evaluate('app/data/catalog.js', ['games','twins','features'], {userCatalog});
  const noRead = () => { throw new Error('User game accessed a built-in data file'); };
  const state = evaluate('app/state/catalog.js', ['getGame','relatedGames','editionsOf'], {
    ...lists, createReleaseCache:()=>noRead, createPageCache:()=>noRead,
    getHomebrew:noop, homebrewApps:()=>[], pausedFilter:()=>false,
    t:key=>key, FileSystem:{readFile:noRead}, formatDate2:noop, formatNumber2:String
  });
  return {...state, lists, calls:()=>calls};
}
function hooks() {
  const effects = [], cleanups = [];
  return {effects,
    useState:init=>[typeof init==='function'?init():init,noop], useRef:init=>({current:init}),
    useEffect:fn=>effects.push(fn),
    mount() { for (const fn of effects.splice(0)) { const off=fn(); if (typeof off==='function') cleanups.push(off); } },
    unmount() { for (const off of cleanups.splice(0)) off(); }
  };
}
function walk(node, visit) {
  if (!node || typeof node !== 'object') return;
  if (Array.isArray(node)) { node.forEach(child=>walk(child,visit)); return; }
  visit(node);
  walk(node.props?.children,visit);
}

test('a user cover starts its native request before an empty image can report layout', () => {
  const h = hooks(), acquired = [];
  let released = 0;
  const {Image} = evaluate('ps5-react/runtime/js/focus/image.js', ['Image'], {
    ...h, useLayoutEffect:h.useEffect, useReducer:()=>[0,noop], useContext:()=>null,
    createContext:()=>({}), forwardRef:fn=>fn, createElement:jsx,
    createFocusable:type=>type, isFocusable:()=>false, resolveStyle:style=>style,
    acquireImage:(...args)=>{acquired.push(args);return ()=>released++;}
  });
  const {Cover} = evaluate('app/ui/Cover.jsx', ['Cover'], {...h, jsx,jsxs:jsx,Image,Text:'Text',View:'View'});
  const uri='file:///data/homebrew/catalog-user/covers/1.jpg';
  const tree=Cover({game:{title:'User game',art:{cover:uri,tint:'#e0242c'}},style:{width:152,height:203,borderRadius:8}});
  walk(tree,node=>{if(node.type===Image)Image(node.props);});
  h.mount();
  assert.equal(acquired.length,1, 'The cover must not wait for intrinsic image dimensions');
  assert.deepEqual(acquired[0].slice(0,5),[uri,152,203,0,false]);
  h.unmount();
  assert.equal(released,1);
});

test('empty built-in catalog preserves PS4/PS5 userReleases and never reads built-in pages', () => {
  const c=catalog();
  assert.equal(c.lists.games.length,3);
  assert.equal(c.lists.twins.length,0);
  for (const game of c.lists.games) {
    assert.equal(c.getGame(game.id),game);
    assert.equal(game.releases,game.userReleases);
    assert.ok(game.releases.length);
    assert.equal(game.about.length,0);
    assert.equal(game.shots.length,0);
    assert.equal(game.shortDescription,'');
  }
  assert.equal(c.getGame('PPSA10662').releases[0].url,magnet);
  assert.equal(c.calls(),0);
});

test('real game screen, hero and download button render and mount without torrent work', () => {
  const c=catalog();
  for (const game of c.lists.games) {
    const h=hooks();
    const queue={snapshot:()=>[],subscribe:fn=>{fn([]);return noop;}};
    const common={...h, jsx,jsxs:jsx,motion:{View:'motion.View'},useT:()=>key=>key,
      useNavActions:()=>({focusLater:noop}),downloadQueue:()=>queue,console:{log:noop}};
    const useDownload=evaluate('app/game/useDownload.js',['useDownload'],{
      ...common, fractionOf:()=>0,sameRelease:()=>false
    }).useDownload;
    const {DownloadButton}=evaluate('app/game/DownloadButton.jsx',['DownloadButton'],{
      ...common,useDownload,storageRoots:()=>['/data'],freeBytes:()=>10**12,
      useFreeSpace:()=>[10**12],spaceNeeded:release=>release?.sizeBytes||0,MARGIN2:0,
      STALE_PARTIAL:/stale/,cause:x=>x,shownState:()=>'',formatPercent2:String,formatBytes:String,
      statusLine:()=>['idle',''],useShadowMount:()=>true,DownloadFormats:{images:[]},
      COMPLETE:[],BUSY:[],WAITING:[],DISABLED:[],preferredRelease:()=>0,
      canDownload:g=>g.releases.length>0,editionsOf:c.editionsOf,isOverdriveCdn:()=>false,driveName:String
    });
    const {GameHero}=evaluate('app/game/GameHero.jsx',['GameHero'],{
      ...common,DownloadButton,useLocale:()=>({rtl:false}),useStoreActions:()=>({toggleWish:noop}),
      useWishlist:()=>[],listedId:x=>x,canDownload:g=>g.releases.length>0,
      latin:(_,text)=>text,fitBytes:x=>x,SPAN_MAX:256,genreLabel:x=>x
    });
    const {GameScreen}=evaluate('app/game/index.jsx',['GameScreen'],{
      ...common,GameHero,relatedGames:c.relatedGames,useFavoriteAction:()=>noop,lightGame:noop,unlight:noop
    });
    let buttons=0;
    const screen=GameScreen({game,onBack:noop,onOpenGame:noop});
    walk(screen,node=>{
      if (node.type!==GameHero) return;
      walk(GameHero(node.props),child=>{
        if (child.type!==DownloadButton) return;
        const button=DownloadButton(child.props);
        walk(button,element=>{if(element.props?.focusKey===`download:${game.id}`) buttons++;});
      });
    });
    h.mount(); h.unmount();
    assert.equal(buttons,1);
    assert.equal(c.calls(),0);
  }
});

test('opening the download hook only observes; explicit advance queues the user release', () => {
  const c=catalog(), h=hooks(), queued=[];
  const queue={snapshot:()=>[],subscribe:fn=>{fn([]);return noop;},enqueue:(...args)=>queued.push(args)};
  const {useDownload}=evaluate('app/game/useDownload.js',['useDownload'],{
    ...h,downloadQueue:()=>queue,fractionOf:()=>0,sameRelease:()=>false
  });
  const game=c.getGame('PPSA10662'), release=game.userReleases[0];
  const hook=useDownload(game,release,'/data');
  h.mount();
  assert.equal(queued.length,0);
  hook.advance();
  assert.equal(queued.length,1);
  assert.equal(queued[0][0],game);
  assert.equal(queued[0][1],release);
  assert.equal(queued[0][1].url,magnet);
  h.unmount();
});
