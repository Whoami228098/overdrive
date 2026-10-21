# Optional libtorrent backend

`torrent_build.py` builds libtorrent 2.0.10 from the SHA-256-pinned inputs in
`dependencies.lock.json`, using the app's existing payload SDK and PacBrew
OpenSSL. Boost 1.84 supplies headers only. No additional package-manager library
is required. Its cache is `.deps/torrent-ps5`; build logs are
`.build/torrent-configure.log` and `.build/torrent-build.log`.

The existing PS5 host builder adds `shared/torrent_api.cpp` and `torrent_js.cpp`,
the matching public ABI definitions and static archive. Only the engine C++ file
needs exceptions/RTTI; the QuickJS adapter keeps the host's existing flags. The
final package includes both dependency notices and `torrent-build.json`.

JavaScript uses the public `Torrent` module through the existing native task
bridge. `Torrent.available` is false on older or desktop hosts without this
optional backend. Existing HTTP transfers keep their original path.

The worker owns the session/alerts and exposes bounded submission and snapshots.
Listeners use IPv4 ports 6881–6889. Connections are capped at 64, the send watermark
at 256 KiB, disk workers at 2, and hashing workers at 1. The optional PS5 network
pool is 4 MiB instead of the HTTP-only 1 MiB. These are resource limits, not a
measured throughput claim. The app still runs one queue transfer at a time.

Each torrent's payload is isolated under `saveRoot/downloads/torrents/<info-hash>`.
Fast-resume data is separate in `.resume`. Metadata paths and existing symlinks
are checked before payload downloading is enabled. Completion/pause waits for
closed disk handles and a flushed resume save. Periodic checkpoints run every
30 seconds; shutdown drains for up to five seconds. A forced exit may cause a
piece-hash recheck. Empty torrents and unsafe metadata fail explicitly.

The queue routes completed PKGs, mountable images, game folders and archives
through its existing install/mount/extract handling. Other files remain available
in their torrent folder. Cancelling releases the native handle before queue
cleanup deletes partial files. Only the requested job's hash directory and
resume file belong to that job.

Run the dependency-free JavaScript behavior checks from the repository root:

```sh
node --test ps5-react/native/shared/tests/torrent-integration.test.mjs app/state/game-detection.test.mjs
```

The PS5 builder runs these checks before compiling libtorrent. They cover real
queue/wrapper/source/catalog code with controlled native/FS fixtures. Separate
local SDK builds linked a complete archive smoke ELF and a native API smoke ELF
with strict symbol checks. No source patch was needed for the initial payload
build. **No console execution or actual peer transfer is confirmed.**

This is a PS5 host engine that can download PS4 and PS5 payloads. It does not
produce a PS4-native executable. Full app packaging follows the existing Linux
workflow; raw payload ELF linking does not by itself verify that app runtime.
