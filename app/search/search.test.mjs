import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

function evaluate(path, names, injected) {
  const code = readFileSync(new URL(path, import.meta.url), 'utf8')
    .replace(/^import .*;\r?$/gm, '').replace(/^export \{[^}]+\};?\r?$/gm, '');
  const context = vm.createContext(injected);
  vm.runInContext(code + ';globalThis.result = {' + names.join(',') + '};', context);
  return context.result;
}
const game = (id, title, extra = {}) => ({id, title, developer:'', publisher:'', genres:[], ...extra});

test('search preserves title-start, word-start, substring and metadata ranking', () => {
  const games = [game('metadata', 'Other', {developer:'War Studio'}),
    game('substring', 'D-war-f'), game('word', 'God of War'), game('start', 'Warframe'),
    game('accent', 'Wárrior')];
  const {searchGames} = evaluate('./match.js', ['searchGames'], {games});
  assert.deepEqual(Array.from(searchGames(' WAR '), g => g.id), ['start','accent','word','substring','metadata']);
  assert.equal(searchGames('   '), games);
  assert.equal(searchGames('missing').length, 0);
});

test('query metacharacters are literal and a broad result set compiles one expression', () => {
  const games = Array.from({length:4200}, (_, i) => game(String(i), 'Game ' + i));
  games.push(game('literal', 'Game [a+b].*'));
  let compiled = 0;
  function CountedRegExp(...args) { compiled++; return new RegExp(...args); }
  const {searchGames} = evaluate('./match.js', ['searchGames'], {games, RegExp:CountedRegExp});
  assert.equal(searchGames('game').length, 4201);
  assert.equal(compiled, 1);
  assert.deepEqual(Array.from(searchGames('[a+b].*'), g => g.id), ['literal']);
  assert.equal(compiled, 2);
});

test('search rows retain their renderer and data across unrelated screen renders', () => {
  const slots = [];
  let cursor = 0, searches = 0;
  const memoHook = (fn, deps) => {
    const index = cursor++, old = slots[index];
    if (!old || old.deps.length !== deps.length || deps.some((value, i) => value !== old.deps[i]))
      slots[index] = {deps, value:fn()};
    return slots[index].value;
  };
  const jsx = (type, props) => ({type, props});
  const first = [game('one', 'One'), game('two', 'Two')];
  const second = [game('three', 'Three')];
  const {Results} = evaluate('./index.jsx', ['Results'], {
    jsx, jsxs:jsx, Fragment:'Fragment', memo:fn=>fn,
    useMemo:memoHook, useCallback:(fn, deps)=>memoHook(()=>fn, deps),
    useT:()=>key=>key, searchGames:query=>{searches++; return query==='one'?first:second;},
    CARD_WIDTH3:182, COLUMNS3:4, fitBytes:x=>x, SPAN_MAX:256,
    GameCard:'GameCard', Text:'Text', ScrollView:'ScrollView', VirtualList:'VirtualList',
    NoResults:'NoResults'
  });
  const open = () => {};
  function render(query) {
    cursor = 0;
    const tree = Results({query, onOpenGame:open});
    return tree.props.children[1].props.children;
  }
  const a = render('one'), b = render('one');
  assert.equal(searches, 1);
  assert.equal(a.props.data, b.props.data);
  assert.equal(a.props.renderItem, b.props.renderItem);
  const c = render('two');
  assert.equal(searches, 2);
  assert.equal(c.props.data, second);
  assert.notEqual(c.props.renderItem, b.props.renderItem);
  assert.equal(c.props.renderItem({item:second[0], index:0}).props.rowEnd, true);
});
