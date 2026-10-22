// Local music is independent of the catalog and download store. Only its small
// widget subscribes; elapsed-time updates never rebuild the game library.
const MAX_MANIFEST_BYTES = 512 * 1024;
const MAX_TRACKS = 1024;

export function safeMediaPath(value) {
  return typeof value === 'string' && value.length > 0 && value.length < 1024 &&
    !/[\\\0\r\n:%?#]/.test(value) &&
    value.split('/').every(part => part && part !== '.' && part !== '..');
}

export function normalizePlaylist(input, root) {
  if (input?.version !== 1 || !Array.isArray(input.tracks)) return [];
  const seen = new Set();
  return input.tracks.slice(0, MAX_TRACKS).flatMap((item, index) => {
    if (!item || !safeMediaPath(item.file) || !/\.mp3$/i.test(item.file) || seen.has(item.file)) return [];
    seen.add(item.file);
    const text = (value, fallback) => typeof value === 'string' && value.trim() ? value.trim().slice(0, 256) : fallback;
    return [{id: text(item.id, String(index)), title: text(item.title, item.file.split('/').at(-1)),
      artist: text(item.artist, 'Unknown artist'), source: `${root}/${item.file}`,
      cover: safeMediaPath(item.cover) && /\.(png|jpe?g|webp)$/i.test(item.cover) ? `file://${root}/${item.cover}` : null,
      duration: Number.isFinite(item.duration) && item.duration > 0 ? item.duration : 0}];
  });
}

export function loadPlaylist(fs, roots) {
  for (const root of roots) {
    try {
      const path = `${root}/playlist.json`;
      const stat = fs.stat(path);
      if (!stat?.isFile || stat.size > MAX_MANIFEST_BYTES) continue;
      const tracks = normalizePlaylist(JSON.parse(fs.readFile(path)), root);
      if (tracks.length) return tracks;
    } catch { /* An absent/old music pack must not prevent opening the store. */ }
  }
  return [];
}

export function createMusicController({music, fs, roots, random = Math.random,
  interval = setInterval, clear = clearInterval, log = () => {}}) {
  let tracks = [], current = null, activeId = 0, timer = null, active = false;
  let paused = false, previous = -1;
  const failed = new Set(), listeners = new Set();
  let view = {track: null, state: 'idle', paused: false, position: 0, duration: 0, error: '', available: 0};
  function publish(change) {
    const next = {...view, ...change};
    if (Object.keys(next).every(key => next[key] === view[key])) return;
    view = next;
    listeners.forEach(listener => listener(view));
  }
  function choose() {
    if (!active) return;
    const candidates = tracks.map((_, index) => index).filter(index => !failed.has(index));
    const pool = candidates.length > 1 ? candidates.filter(index => index !== previous) : candidates;
    if (!pool.length) {
      current = null;
      publish({state: 'failed', error: 'No playable tracks in this music pack'});
      return;
    }
    const pick = Math.min(pool.length - 1, Math.max(0, Math.floor(random() * pool.length)));
    previous = pool[pick];
    current = tracks[previous];
    try {
      activeId = music.play(current.source);
      music.setPaused(paused);
      publish({track: current, state: 'loading', paused, position: 0, duration: current.duration, error: ''});
      log(`music queued track=${current.id}`);
    } catch (error) {
      current = null;
      publish({state: 'failed', error: String(error.message ?? error)});
    }
  }
  function tick() {
    if (!active || !current) return;
    try {
      const status = music.getStatus();
      if (status.id !== activeId) return;
      if (status.state === 'failed') {
        log(`music failed track=${current.id}: ${status.error}`);
        failed.add(previous);
        choose();
      } else if (status.state === 'ended' && !paused) {
        choose();
      } else {
        publish({state: status.state, paused,
          position: Math.max(0, Math.floor(status.position || 0)),
          duration: status.duration > 0 ? status.duration : current.duration});
      }
    } catch (error) {
      current = null;
      publish({state: 'failed', error: String(error.message ?? error)});
    }
  }
  return {
    getSnapshot: () => view,
    subscribe(listener) { listeners.add(listener); listener(view); return () => listeners.delete(listener); },
    start() {
      if (active) return;
      active = true;
      paused = false; // Every application launch starts with music enabled.
      previous = -1;
      failed.clear();
      tracks = loadPlaylist(fs, roots);
      publish({track: null, position: 0, duration: 0, error: '', available: tracks.length,
        paused: false, state: tracks.length ? 'loading' : 'empty'});
      if (!tracks.length) return;
      try { music.setVolume(0.35); } catch (error) {
        publish({state: 'failed', error: String(error.message ?? error)}); return;
      }
      choose();
      timer = interval(tick, 250);
    },
    togglePaused() {
      if (!active || !current) return false;
      paused = !paused;
      try { music.setPaused(paused); publish({paused}); }
      catch (error) { publish({state: 'failed', error: String(error.message ?? error)}); }
      return true;
    },
    stop() {
      if (!active) return;
      active = false;
      if (timer !== null) clear(timer);
      timer = null;
      current = null;
      try { music.stop(); } catch { /* Host shutdown also releases the worker. */ }
    },
  };
}
