// TRAPSTARSHOP: user catalog loader.
// Reads the user's own catalog files (catalog-user.json / catalog-user/*.json)
// from /data/homebrew and /mnt/usb0, normalizes them into the store's game
// shape, and hands them to data/catalog.js before the platform gate runs.
// The catalog content itself belongs to the user; this module only reads,
// validates and merges. Entries with no usable source are skipped.

import { FileSystem, Torrent } from '@ps5-react/core';
import { torrentKey } from '../state/download-sources.js';

// An older/desktop host without libtorrent keeps these entries hidden.
const ALLOW_MAGNETS = true;

const PLATFORMS = new Set(['ps4', 'ps5', 'ps1', 'ps2', 'psp', 'psx', 'retro']);

const SEARCH_PATHS = [
  '/data/homebrew/catalog-user/catalog-user.json',
  '/data/homebrew/catalog-user.json',
  '/mnt/usb0/homebrew/catalog-user/catalog-user.json',
  '/mnt/usb0/catalog-user.json'
];

function readJson(path) {
  try {
    const text = FileSystem.readFile(path);
    return JSON.parse(text);
  } catch {
    return null;
  }
}

// FNV-1a over a string, base36: stable ids without a crypto dependency.
function hashId(text) {
  let hash = 0x811c9dc5;
  for (let i = 0; i < text.length; i++) {
    hash ^= text.charCodeAt(i);
    hash = (hash * 0x01000193) >>> 0;
  }
  return 'USR' + hash.toString(36).toUpperCase().padStart(7, '0');
}

function hostName(url) {
  try {
    return new URL(url).hostname.replace(/^www\./, '');
  } catch {
    return 'TrapStar mirror';
  }
}

// A source field can be: one url/magnet string, an array of them (mirrors,
// best first), or an object { source, mirrors: [...] }.
function collectSources(entry) {
  const raw = [];
  if (typeof entry.source === 'string') raw.push(entry.source);
  else if (entry.source && typeof entry.source === 'object' && entry.source.url) raw.push(entry.source.url);
  else if (entry.url) raw.push(entry.url);
  if (Array.isArray(entry.mirrors)) {
    for (const mirror of entry.mirrors) {
      const url = typeof mirror === 'string' ? mirror : mirror?.url || mirror?.source;
      if (url) raw.push(url);
    }
  }
  const seen = new Set();
  const out = [];
  for (const value of raw) {
    if (typeof value !== 'string' || !value.trim() || seen.has(value)) continue;
    seen.add(value);
    out.push(value.trim());
  }
  return out;
}

function guessFormat(url) {
  const match = /\.(ffpfsc|ffpfs|ffpkg|exfat|pkg|zip|rar|7z)(?:[?#]|$)/i.exec(url);
  if (!match) return null;
  const ext = match[1].toLowerCase();
  if (ext === 'zip' || ext === 'rar' || ext === '7z') return 'archive-set';
  return ext;
}

// A release the store's SourcePicker and download queue already understand:
// a direct HTTP file (pkg / ffpkg / exfat / ffpfsc) or an archive set (parts).
function buildRelease(entry, source, index) {
  const isMagnet = /^magnet:\?/i.test(source);
  if (isMagnet) {
    if (!ALLOW_MAGNETS || !Torrent.available) return null;
    let key;
    try { key = torrentKey(source); } catch { return null; }
    const sizeBytes = entry.size_gb ? Math.round(Number(entry.size_gb) * 1e9) : 0;
    return {
      id: hashId(key), type: 'torrent', format: 'torrent', source: 'user',
      label: [entry.region, entry.version].filter(Boolean).join(' · ') || 'Torrent',
      provider: 'BitTorrent', health: 'ok', version: entry.version || null,
      filename: `${entry.title_id || entry.id || 'user'}-${key}`,
      sizeBytes: Number.isSafeInteger(sizeBytes) && sizeBytes > 0 ? sizeBytes : 0,
      url: source.replace(/^magnet:/i, 'magnet:')
    };
  }
  const format = (entry.format || guessFormat(source) || 'pkg').toLowerCase();
  const sizeBytes = entry.size_gb ? Math.round(Number(entry.size_gb) * 1e9) : 0;
  const label = [entry.region, entry.dub === 'RUS' ? 'RUS' : null].filter(Boolean).join(' · ') || `Mirror ${index + 1}`;
  const provider = hostName(source);
  // A multi-part archive (rar/7z/zip) becomes an archive-set whose parts the
  // queue downloads in order and then extracts; anything else is one file.
  if (format === 'archive-set') {
    return {
      label, provider, format, version: entry.version || null,
      sizeBytes, health: 'ok', source: 'user',
      filename: String(entry.id || 'user-set'),
      parts: [{ sources: [{ url: source }] }]
    };
  }
  return {
    label, provider, format, version: entry.version || null,
    sizeBytes, health: 'ok', source: 'user',
    url: source
  };
}

// The store's own entry shape: everything state/catalog.js touches must be
// present and sane, because it normalizes without guards.
function toGame(entry, catalogDir) {
  const title = String(entry.title || '').trim();
  const platform = String(entry.platform || '').trim().toLowerCase();
  if (!title || !PLATFORMS.has(platform)) return null;
  const sources = collectSources(entry);
  if (!sources.length) return null;
  const releases = [];
  sources.forEach((source, index) => {
    const release = buildRelease({ ...entry, id: hashId(title + platform) }, source, index);
    if (release) releases.push(release);
  });
  const magnetOnly = releases.length === 0;
  if (magnetOnly) return null;
  const id = entry.title_id ? String(entry.title_id).toUpperCase() : hashId(title + platform);
  // Covers ship beside the catalog on USB/data (file:///covers/<id>.jpg) or on a
  // CDN (https). Relative paths resolve against the catalog file's directory.
  let cover = null;
  if (typeof entry.cover_url === 'string') {
    if (entry.cover_url.startsWith('file:///covers/')) {
      cover = 'file://' + catalogDir + '/' + entry.cover_url.slice(8);
    } else if (entry.cover_url.startsWith('http')) {
      cover = entry.cover_url;
    }
  }
  return {
    id,
    title,
    platform,
    user: true,
    providers: releases.map((release) => release.provider),
    direct: true,
    developer: entry.developer || '',
    publisher: entry.publisher || '',
    releaseDate: String(entry.releaseDate || entry.release_date || ''),
    year: Number(entry.year) || null,
    genres: Array.isArray(entry.genres) ? entry.genres.filter((genre) => typeof genre === 'string') : [],
    features: [],
    metacritic: null,
    recommendations: 0,
    sizeGb: Number(entry.size_gb) || null,
    region: entry.region || null,
    dub: entry.dub || null,
    format: releases[0]?.format || null,
    magnetOnly,
    art: { cover, tint: '#e0242c' },
    userReleases: releases
  };
}

// User entries ride the store's data arrays through the same normalization
// the built-in ones go through; releases ride Entry.get releases.
function withUserReleases(entry) {
  const base = Object.create(null);
  entry.userReleases = entry.userReleases || [];
  base.releases = entry.userReleases;
  return Object.assign(base, entry, {
    get releases() { return entry.userReleases; }
  });
}

function loadUserCatalog() {
  const games = [];
  const mods = [];
  const seen = new Set();
  let catalogDir = '';
  for (const path of SEARCH_PATHS) {
    const data = readJson(path);
    if (!data) continue;
    catalogDir = path.slice(0, path.lastIndexOf('/'));
    const gameList = Array.isArray(data) ? data : Array.isArray(data.games) ? data.games : [];
    const modList = Array.isArray(data.mods) ? data.mods : [];
    for (const entry of gameList) {
      try {
        const game = toGame(entry, catalogDir);
        if (!game) continue;
        const key = `${game.title.toLowerCase()}|${game.platform}`;
        if (seen.has(key)) continue;
        seen.add(key);
        games.push(withUserReleases(game));
      } catch { /* a bad entry never blocks the rest of the catalog */ }
    }
    for (const entry of modList) {
      if (!entry.title || !entry.game_title || !entry.source) continue;
      mods.push({
        id: hashId(`${entry.title}|${entry.game_title}`),
        title: String(entry.title),
        game: String(entry.game_title),
        platform: String(entry.platform || 'ps4').toLowerCase(),
        source: String(entry.source),
        isMagnet: String(entry.source).startsWith('magnet:'),
        version: entry.version || null,
        sizeMb: Number(entry.size_mb) || null,
        coverUrl: entry.cover_url || null
      });
    }
  }
  return { games, mods };
}

const userCatalog = loadUserCatalog();

export { userCatalog };
