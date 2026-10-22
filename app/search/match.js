// ../pub-game-store/native/search/match.js
// reconstructed from the esbuild bundle in eboot.bin

// The state module first: it takes the games of paused hosts out of this list before it is read.
import '../state/catalog.js';
import { games } from '../data/catalog.js';

  var fold = (text) => text.toLowerCase().normalize("NFD").replace(/[\u0300-\u036f]/g, "");
  var haystacks = games.map((game) => fold(`${game.title} ${game.developer} ${game.publisher} ${game.id} ${(game.genres || []).join(" ")} ${game.region || ""} ${game.year || ""}`));
  var lowerTitles = games.map((game) => fold(game.title));
  function searchGames(query) {
    const needle = fold(query.trim());
    if (!needle) return games;
    // Word-start and title-start matches rank above substring hits.
    const wordStart = new RegExp(`(^| )[a-z0-9]*${needle.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}`);
    const scored = [];
    games.forEach((game, i) => {
      if (!haystacks[i].includes(needle)) return;
      let score = 0;
      if (lowerTitles[i].startsWith(needle)) score = 3;
      else if (wordStart.test(lowerTitles[i])) score = 2;
      else if (lowerTitles[i].includes(needle)) score = 1;
      else score = 0;
      scored.push([score, i]);
    });
    scored.sort((a, b) => b[0] - a[0] || a[1] - b[1]);
    return scored.map(([, i]) => games[i]);
  }

export { searchGames };
