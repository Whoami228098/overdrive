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

## Startup diagnosis

Set `TORRENT_MINIMAL=1` when building, or enable the workflow's `torrent_minimal`
input. This embeds `-DTORRENT_MINIMAL=1` in the host, disables DHT and LSD, clears
DHT bootstrap nodes and disables port retries. UPnP and NAT-PMP remain disabled.
The listener is `0.0.0.0:6881`; the startup log prints the effective mode before
constructing the session. Ordinary builds can also opt in with the runtime
environment variable `TORRENT_MINIMAL=1`. A forced minimal build cannot be turned
back to full mode by its runtime environment.
Outgoing interfaces stay empty so the OS selects its route. Unlike the listener,
the outgoing setting is a strict allowlist of actual local addresses/interfaces;
`0.0.0.0` there rejects traffic from a real interface. Logs show `outgoing=system`.

Minimal mode is for console startup diagnosis: magnets without working trackers
may have no peer discovery. It does not prove DHT, hostname resolution or peer
transfers work. A `session ready worker` message only means the constructor
returned; libtorrent's network thread may still fail asynchronously.

CI retains `llvm-pie.elf`, the converted `eboot.elf`, symbol/import/relocation
lists, linker map and test logs as `Native-diagnostics`. New builds retain line
tables for host and libtorrent code. Set `diagnostic_only` to skip draft releases
and the unrelated PS4 build. To reproduce a previous artifact, set `source_ref`
to its exact commit, then compare executable bytes before using its addresses.

The PS5 build sets upstream `TORRENT_BROKEN_RANDOM_DEVICE=1`: libtorrent seeds
its PRNG using the existing cryptographic OpenSSL `RAND_bytes` backend. It does
not use a fixed seed or a dummy entropy function. The linker excludes the private
`libScePosixForWebKit` stub and refuses `arc4random`/`arc4random_buf` imports.
`native-imports.json` lists the actual imports and records SDK-provider checks.
Before session startup the generated runtime audit reads every import address,
logs null symbols and `native imports checked=N unresolved=M`, and refuses the
torrent startup if M is nonzero. Build-time provider checks alone cannot prove
the firmware loader supplied an address; this runtime result requires a console
test. For linker-wrapped imports the audit uses the linker's `__real_` reference,
so a wrapper's address cannot hide a missing system import. A host regression
test covers a non-null wrapper around a deliberately null real symbol. No
imported function is called by the audit.

The sole optional import is `sceVideoOutGetHdmiRawEdid_`, used for HDMI sink
capability discovery. Its caller reads a volatile address and returns false
when missing. The audit logs `optional native import NULL symbol=...` and a
separate `optional_unresolved` count; this does not increase required
`unresolved`. Other imports, including the torrent and entropy backends, remain
required. Tests link the actual EDID caller with both a null and a real provider.

The separate manual `torrent host soak` workflow runs the shared source from an
explicit commit with the pinned libtorrent and Boost sources on Linux. Use zero
minutes for a smoke run, then 90 for an uninterrupted timed test. It builds the
engine and harness with AddressSanitizer/UndefinedBehaviorSanitizer and enables
leak detection. Synthetic files transfer only through a loopback peer; DHT and
LAN discovery are off. It checks metadata, pause/checkpoint/restore, payload
checksums, duplicate/root handling, invalid resumes, filesystem failures, queue
capacity, concurrent calls and shutdown/restart. The artifact records source
hashes, elapsed monotonic time, memory/descriptors and API latency. System OpenSSL
is a host dependency; this test does not execute the PS5 ELF or validate PS5
imports, firmware or DHT.
Short smoke runs repeat the loopback transfer 12 times and verify that late
pause/resume commands cannot resurrect a removed task. Removed handles ignore
older metadata/checkpoint alerts; only removal completion or deletion failure
may update them. Metadata acceptance and the first four peer errors per task
are recorded in the native log.
Before a new workflow is present on the default branch, the existing `build`
workflow can call it with `host_soak_minutes` set to `0` or `90`; console build
and release jobs are skipped in that mode.

The worker owns the session/alerts and exposes bounded submission and snapshots.
The host schedules it at startup before loading JS, without waiting for session
initialization. JS add/control calls never start or join a worker. Each queued
add carries its own save root; directory and resume work run on the worker.
Session lifecycle and add ids are written through the PS5 async logger. A blocked
session constructor or metadata request cannot hold the submission/snapshot lock.
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
node --test ps5-react/native/shared/tests/torrent-integration.test.mjs app/game/game-open.test.mjs app/state/game-detection.test.mjs
```

The PS5 builder runs these checks before compiling libtorrent. They cover real
queue/wrapper/source/catalog code with controlled native/FS fixtures. Separate
local SDK builds linked a complete archive smoke ELF and a native API smoke ELF
with strict symbol checks. No source patch was needed for the initial payload
build. **No console execution or actual peer transfer is confirmed.**

This is a PS5 host engine that can download PS4 and PS5 payloads. It does not
produce a PS4-native executable. Full app packaging follows the existing Linux
workflow; raw payload ELF linking does not by itself verify that app runtime.
