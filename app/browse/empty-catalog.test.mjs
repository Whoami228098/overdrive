import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import test from 'node:test';
import vm from 'node:vm';

const root = process.env.EMPTY_CATALOG_TEST_ROOT || fileURLToPath(new URL('../../', import.meta.url));
const noop = () => {};
const jsx = (type, props) => ({type, props});

function evaluate(path, names, injected = {}) {
  let code = readFileSync(root + path, 'utf8');
  const imports = {};
  for (const match of code.matchAll(/^import \{([^}]+)\} from .*;\r?$/gm))
    for (const item of match[1].split(',')) {
      const name = item.trim().split(/\s+as\s+/).at(-1);
      imports[name] = name;
    }
  code = code.replace(/^import .*;\r?$/gm, '').replace(/^export \{[^}]+\};?\r?$/gm, '');
  const context = vm.createContext({console, ...imports, ...injected});
  vm.runInContext(code + ';globalThis.result = {' + names.join(',') + '};', context, {filename:path});
  return context.result;
}

function hooks() {
  const effects = [], timers = [], shown = [], opened = [];
  return {
    jsx, jsxs:jsx, memo:fn=>fn, effects, timers, shown, opened,
    useState:init=>[typeof init === 'function' ? init() : init, noop],
    useRef:current=>({current}), useEffect:fn=>effects.push(fn),
    useMemo:fn=>fn(), setTimeout:fn=>(timers.push(fn),timers.length), clearTimeout:noop,
    useT:()=>key=>key, useLocale:()=>({rtl:false}),
    useNavState:()=>({top:{name:'browse'}}),
    useNavActions:()=>({openGame:(...args)=>opened.push(args),focusLater:noop}),
    useStoreActions:()=>({setFilters:noop,setQuery:noop,setSort:noop,toggleFilter:noop}),
    useStoreState:()=>({filters:{genre:[]}}),
    useRowWindow:()=>({far:()=>false,reached:noop}),
    useSectionShown:value=>shown.push(value), useShowLibrary:()=>noop,
    mount() { for (const effect of effects.splice(0)) effect(); }
  };
}

const picks = evaluate('app/browse/picks.js', ['FAMOUS','INDIE','picked','shuffle']);
const game = overrides => ({id:'CUSA99999', title:'Synthetic user game', genres:[],
  art:{cover:null,hero:null,logo:null}, recommendations:0, releaseDate:'', ...overrides});

function popular(games, h) {
  return evaluate('app/browse/PopularRail.jsx', ['PopularRail','FIRST_POPULAR','POPULAR'], {
    ...h, ...picks, games, FIRST_ART:10, ART_AHEAD:8, NO_FILTERS:{}
  });
}

function spotlight(games, h, active = 0) {
  return evaluate('app/browse/Spotlight.jsx', ['Spotlight','Carousel','Art','featured'], {
    ...h, ...picks, games, FIRST_POPULAR:undefined,
    useSlides:()=>({active}), Image:{prefetch:async()=>{}}, logoStyle:()=>({})
  });
}

test('empty or artless catalog imports and hides the popular rail without a focus target', () => {
  for (const games of [[], [game({})]]) {
    const h=hooks(), value=popular(games,h);
    assert.equal(value.FIRST_POPULAR,undefined);
    assert.equal(value.POPULAR.length,0);
    assert.equal(value.PopularRail(),null);
    assert.equal(h.shown.at(-1),false);
    assert.equal(h.opened.length,0);
  }
});

test('populated popular rail keeps its first focus target and opens the selected user game', () => {
  const entry=game({genres:['Action'],art:{cover:'cover',hero:'hero'}});
  const h=hooks(), value=popular([entry],h);
  assert.equal(value.FIRST_POPULAR,'popular:CUSA99999');
  const rail=value.PopularRail();
  const card=rail.props.children[0].props.children;
  assert.equal(card.props.game,entry);
  card.props.onPress();
  assert.deepEqual(h.opened,[['CUSA99999','popular:CUSA99999']]);
});

test('empty or artless catalog omits spotlight and does not schedule prefetch', () => {
  for (const games of [[],[game({})]]) {
    const h=hooks(), value=spotlight(games,h);
    assert.equal(value.Spotlight(),null);
    assert.equal(value.Carousel({shown:true}),null);
    assert.equal(value.Art({game:undefined}),null);
    h.mount();
    assert.equal(h.timers.length,0);
    assert.equal(h.opened.length,0);
  }
});

test('spotlight preserves a usable slide and safely handles a stale slide index', () => {
  const entry=game({art:{hero:'hero',cover:null,logo:null}});
  const h=hooks(), value=spotlight([entry],h);
  assert.ok(value.Spotlight());
  assert.ok(value.Carousel({shown:true}));
  assert.equal(value.featured[0],entry);
  const stale=spotlight([entry],hooks(),9);
  assert.equal(stale.Carousel({shown:true}),null);
});

test('zero slide count never schedules a modulo-zero timer', () => {
  const h=hooks();
  const {useSlides}=evaluate('app/browse/useSlides.js',['useSlides'],h);
  const slides=useSlides(0,true);
  h.mount();
  assert.equal(slides.active,0);
  assert.equal(Number.isFinite(slides.active),true);
  assert.equal(h.timers.length,0);
});

test('empty genre and chart sections are hidden; partial catalogs keep only nonempty charts', () => {
  const h=hooks();
  const {GenreRail}=evaluate('app/browse/GenreRail.jsx',['GenreRail'],{...h,games:[],genres:[]});
  assert.equal(GenreRail(),null);
  const empty=evaluate('app/browse/TopCharts.jsx',['TopCharts','CHARTS'],{
    ...h, ...picks,games:[],sortGames:list=>[...list],NO_FILTERS:{}
  });
  assert.equal(empty.TopCharts(),null);
  assert.equal(empty.CHARTS.length,0);
  const partial=evaluate('app/browse/TopCharts.jsx',['TopCharts','CHARTS'],{
    ...hooks(),...picks,games:[game({})],sortGames:list=>[...list],NO_FILTERS:{},sortLabel:id=>id
  });
  assert.equal(partial.CHARTS.length,1);
  assert.equal(partial.CHARTS[0].id,'newest');
  assert.ok(partial.TopCharts());
});

test('empty search suggestions preserve history and omit the empty popular heading', () => {
  const h=hooks();
  const {Suggestions}=evaluate('app/search/Suggestions.jsx',['Suggestions'],{
    ...h,games:[],HINT:{clear:{},back:{}},useSearchHistory:()=>['Existing search']
  });
  const tree=Suggestions({onSearch:noop,onOpenGame:noop});
  assert.ok(tree.props.children[0]);
  assert.equal(tree.props.children[1],null);
  assert.equal(tree.props.children[2].props.children.length,0);
});

test('empty catalog search and grid comparison never dereference a missing game', () => {
  const {searchGames}=evaluate('app/search/match.js',['searchGames'],{games:[]});
  assert.equal(searchGames('anything').length,0);
  assert.equal(searchGames('').length,0);
  const {sameGrid}=evaluate('app/browse/Catalog.jsx',['sameGrid'],{...hooks(),px:x=>x,DETAILED_CAPTION:0});
  const a={signature:'same',enter:false,games:[]};
  assert.equal(sameGrid(a,{...a,games:[]}),true);
  assert.equal(sameGrid(a,{...a,games:[game({})]}),false);
});
