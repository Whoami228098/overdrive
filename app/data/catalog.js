// ../pub-game-store/native/data/catalog.js
// reconstructed from the esbuild bundle in eboot.bin

// Build-time platform gate: which catalog platforms this build shows.
// PS5 build: keep "ps5" (plus everything else). PS4 build: REMOVE "ps5" —
// a PS4 console cannot run PS5 titles, so the PS4 client must not list them.
import { userCatalog } from './user-catalog.js';

var SUPPORTED_PLATFORMS = ["ps5", "ps4", "ps1", "ps2", "psp", "psx", "retro"];



  var games = [];
  var twins = [];
  var features = ["Single-player", "Online multiplayer", "Co-op", "Cross-play", "DualSense features", "Cloud saves"];

// The user catalog is the only source of games and releases.
// No built-in alternate edition may shadow a user entry with the same title id.
games = games.concat(userCatalog.games);

games = games.filter((g) => SUPPORTED_PLATFORMS.includes(g.platform));
twins = twins.filter((g) => SUPPORTED_PLATFORMS.includes(g.platform));

export { features, games, twins };
