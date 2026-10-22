# Hardware evidence

## Overdrive 1.0.16 covers, torrent I/O and PS5 installation — 2026-10-22

User-authorized testing of `PPSA99058`, Normal mode, source
`cc31c847380680f4df8836759d587a8cb8851b0e`, console build `39822913344`.
The image was uploaded and read back with matching SHA-256
`f032418ca02494cf060ad4edf641796ec92b108d113afd268ef68dd26538a2d5`.
The console reports firmware `13.590.001`; ShadowMount+ identifies itself as
`1.7-beta5fix1`, build `2026-10-10T18:39:01Z`. Results apply to this combination.

- Required imports: 453 checked, zero unresolved; one optional EDID address
  remains guarded. Normal mode initialized the threaded `pread`/`pwrite` backend
  with payload mappings disabled, then loaded the bundle and presented a frame.
- The complete user catalog contains 4,118 normalized cards, including 3,602 PS4
  and 516 PS5 cards, and 4,184 cover files. Console framebuffer captures show
  actual PS4/PS5 art and the Hades user-game page (`PPSA03355`). Opening that page
  produced its matching ready event without any `add_magnet` call. The fix gives
  fixed-size cover images initial dimensions and accepts absolute local image
  URIs. Eleven WebP covers were converted to PNG and one oversized JPEG resized;
  records without source art still use a placeholder. Native image loading is
  asynchronous; local files are not unnecessarily prewarmed across the library.
- A controlled single LAN peer transferred an owned 512 MiB torrent. The title
  exited after 69,222,400 persisted bytes, saved its checkpoint, relaunched and
  completed. Independent full-file readback matched SHA-256
  `a528fedfe4d83efde2836edfed2e4ebc9c8045c3e68a19c9cd94d72be5bdc32c`.
  Maximum reported payload speed was 11.616556 MB/s; median positive sampled
  speed was 10.643265 MB/s. The earlier POSIX profile peaked at 10.945157 MB/s
  with the same fixture. Separate FTP probes reached 25.23 MB/s toward the
  console. These measurements do not establish a public-swarm speed guarantee
  or identify every remaining throughput constraint.
- A sparse owned torrent wrote its 1 MiB tail at offset 5,368,709,120. The
  independently read tail matched SHA-256
  `b3dcbbbf60d44776da1cd235ddf135e6d81c52bee673ef0cdf2a14232bb1dbab`.
  Logical size was 5 GiB + 1 MiB; the preceding 5 GiB were not downloaded.
- Record of Lodoss War (`PPSA05347`) downloaded 176,081,223 bytes from its user
  release, extracted its RAR, applied executable permissions and published its
  folder. ShadowMount registered it after the store exited. Its launch returned
  `0xc018`, the loader reported PID 333, and all 90 one-second process samples
  found it running. The loader's 60-second error monitor expired without an
  error. The test then explicitly closed that title. The source RAR had already
  been removed by successful extraction, so it was not independently rehashed.
- The same folder-permission/publishing code first passed two actual PS5 cases
  on 1.0.14: Dreaming Sarah (`PPSA02929`, RAR to folder, 89 positive process
  samples) and Animal Well (`PPSA22520`, compressed `ffpfsc` image, 101 samples).
  Animal Well's complete published image matched all 838 source torrent piece
  hashes and SHA-256
  `17d74b91f326a9e84ceb98f1d71020940da658ae3e545663508fb701498cfce3`.
  No converter was required for these supported image/folder cases. Process
  survival and loader results do not establish visual gameplay validation or
  compatibility with every game, archive or firmware.
- Only the two owned synthetic payloads/checkpoints were removed. Existing user
  jobs and the three completed game installations were retained. No automatic
  input script remains; the live command file is cleared after the check.

Exact-source Linux run `39822918192` passed 613.728 timed seconds with the pinned
libtorrent/Boost inputs, ASan/UBSan and leak detection: 35 checksum-verified
transfers, 33 repeated cycles, 37,859 API calls and five complete session
lifecycles. Maximum measured API call time was 435 microseconds on that runner.
The instrumented harness counted zero file-backed mapping attempts and zero
`madvise` calls; the configuration header appears in both upstream and adapter
compile commands. All three native source hashes match the final source.
These host results are separate from console execution.

The final build also passed 44 application/queue tests, 98 runtime JS tests and
seven import/provider tests. Its 4,010 packaged files, 54 changed source hashes
and executable text against the unstripped ELF were verified. The full Linux
framework suite passed at `82ffc86` in run `39816687933`; its native image-loader
code is unchanged in this artifact, while the later cover component has its own
application integration regression. The corresponding source archive matches
all 4,617 Git blobs of `cc31c84`. The immutable build receipt keeps its earlier
`hardware_tested: false`; this section records the later scoped hardware work.

## Overdrive 1.0.13 startup and torrent data path — 2026-10-22

User-authorized testing of title `PPSA99058`, Normal mode, source
`49bef57c67d159728a368dc0506e4f47829a45c1`, build run `39731084095`.
The uploaded image was read back and its SHA-256 matched
`81046b39bebc1e0af3460c1559b066669ecc44a299464d37d9fb9de814d49187`.
The app reports firmware `13.590.001` (`0x13590001`); the ShadowMount log
labels its environment `13.60`. The loader identifies itself as ShadowMount+
`1.7-beta5fix1`, build `2026-10-10T18:39:01Z`. These are the reported values,
not evidence for other firmware or loader combinations.

- Startup checked 453 native imports: zero missing required addresses and one
  guarded optional EDID address. The torrent session selected the POSIX disk
  backend, reached ready, initialized the JS bundle and processed controller input.
  A final clean launch with no user catalog also opened the zero-game catalog,
  filters and search without an exception or a torrent request.
- PS4 and PS5 user entries each downloaded an owned 1 MiB loopback fixture.
  Both complete files matched SHA-256
  `b3dcbbbf60d44776da1cd235ddf135e6d81c52bee673ef0cdf2a14232bb1dbab`.
  PS4 was paused at 294,912 persisted bytes, exited cleanly, relaunched showing
  the same progress, and completed with the same checksum after resuming.
- A 5,002-entry synthetic user catalog booted with the built-in catalog empty.
  Scripted native controller actions opened and left 360 game pages; all 360
  produced their ready event, with no torrent submission on page opening.
  Periodic heap samples peaked at 84/256 MiB with zero allocation failures.
  The test reused two local covers. Frame windows during page churn and saved
  development screenshots had a 55.5 fps median, 51.6 fps minimum and 185.9 ms
  maximum individual frame; this is not a constant-60-fps claim.
- An intentionally corrupt owned resume file failed its queue job without
  closing the UI. Removing that fixture checkpoint allowed a successful retry.
  Three files spanning unaligned piece boundaries, nested Cyrillic paths and
  an empty placeholder all matched their independent SHA-256 references.
- A fresh public Debian magnet received data from external peers. It was
  deliberately paused after 28,540,928 persisted bytes; three completed ranges
  read independently from the console matched the official torrent's SHA-1
  piece hashes, including a range at offset 780,140,544. This was a partial
  transfer, not validation of the complete ISO or isolated DHT discovery.
- A second fresh Debian test omitted all tracker, explicit-peer and web-seed
  parameters from the magnet and removed the earlier payload/checkpoint first.
  Normal mode found eight external peers, transferred 3,047,424 persisted bytes
  and paused; three complete pieces read independently matched their official
  hashes. DHT and LSD were enabled. The console also answered a bounded DHT
  ping and returned eight routing nodes. An earlier Ubuntu hash produced no
  peers within four minutes and was stopped through the responsive UI; this
  is retained as a no-peer outcome, not treated as a successful transfer.
- A sparse owned fixture wrote and verified its 1 MiB tail at offset
  5,368,709,120. An independent read of that range matched the reference SHA-256;
  its resume bitmap contained only piece 320. The file's logical length was
  5 GiB + 1 MiB; the preceding 5 GiB were not downloaded.
- Each completed test session exited through the host's normal cleanup and
  logged the torrent worker stopping before title closure.

The final clean console session kept the same process for 2,700 seconds.
All nine periodic controller probes changed the rendered navigation state;
89 samples showed advancing logs, zero allocation failures and a maximum
reported heap of 55/256 MiB. Test catalog records, payloads, resume files and
history were removed, the original user queue was preserved, and no automatic
input script remains in the image or title data.

Separately, Linux sanitizer run `39731086999` used the exact shared native source
above with the pinned libtorrent/Boost inputs. It completed 5,450.72 timed
seconds, 716 checksum-verified transfers, 714 cycles and 327,551 measured API
calls. All 54 sessions became ready and stopped; maximum API call time was
0.651 ms on that Linux runner. ASan/UBSan/leak detection were clean, with four
deliberate negative-case task errors. These source hashes match the PS5 image
receipt. The complete Linux framework suite also passed in run `39731794554`
(test source `4a76dca`; only Linux image-allocation fixtures differ from the
application build source). These are host tests, distinct from console execution.

The previous 1.0.12 image
`ecbfd79d7ad048cb142772192e14f4cb8aa6a5e7e7a708c634fe9ad2c2f70978`
reproducibly obtained metadata, then the loader reported
`0xa002030a (SYSTEM_ILLEGAL_FUNCTION_CALL)` in `libtorrent-disk-thread`
at the first payload writes. The same fixture passed with 1.0.13's POSIX
backend. A standalone simple file mapping also worked: the exact prohibited
operation in the full memory-mapped backend remains unidentified.

The immutable build receipt predates these tests and retains its build-time
`hardware_tested: false`; the separate hardware report qualifies the exact
artifact and cases above. This is a PS5 host downloading PS4/PS5 payloads, not
a PS4-native application or confirmation of every install format.

## User-reported Overdrive Minimal startup — 2026-10-22

Firmware 13.590.001, ShadowMount+ active, user-visible build identity
`TRAPSTARMINIMAL`, session 09:55:56 in the user's local time. The user reported
`native imports checked=453 unresolved=1`, identifying
`sceVideoOutGetHdmiRawEdid_`; the audit refused torrent startup rather than
calling a missing import. The application still closed after the independent
JavaScript exception near bundle line 17582 while reading a missing game's id.
The supplied trace and the app.log retrieved through the user-authorized FTP
connection agree on both failures. The exact console image hash and loader
version were not supplied, so this records the reported identity and partial
behavior, not a complete hardware pass or verification of later builds.

Overdrive 1.0.10 adds a guarded optional EDID import and empty-catalog startup
checks. Those new artifacts require their own console evidence.

## User-confirmed baseline — 2026-10-05

PS5 Slim Digital, firmware 13.60, kstuff and ShadowMount active, without etaHEN.
Title `PPSA99052`, uploaded as an extracted folder under `/data/homebrew/`.

The user confirmed responsive rendering, D-pad focus, Cross confirmation,
counter state updates, and Options returning to the system home screen.
The proof of concept intentionally exits after 30 seconds. FPS was not measured.

Confirmed executable SHA-256:
`b4d154838f1df886288ffd938e57336caf07158bdcd82d0f1e2622d098db3339`.

The debug PKG `PPSA99051` failed to launch with CE-100022-5. This alone does
not identify the cause; the same executable worked through the folder workflow.

## Framework starter

`PPSA99053` is a new identity and build with extracted configuration and APIs.
It does not automatically inherit the proof of concept's hardware status.
The build receipt sets `hardware_tested: false` pending a test of that artifact.

Validation procedure: launch PS5 React Starter, move focus, update the counter,
return, exit with Options, and verify the configured timeout. Changes to the
host or SDK need new validation. Desktop tests and ELF checks do not establish
PS5 execution or support for other firmware/loader combinations.

## System Explorer storage crash — 2026-10-05

The user reported a crash after the storage-query change. PS5Upload's read-only
syslog and kernel-log endpoints captured two `PPSA99056` executions (PIDs 194
and 197). Both initialized graphics, QuickJS, the bundle, and presented a frame.
Both then logged `native fs.readDir`, `native fs.diskUsage`, followed by:

```text
PPRBUG-22859: the process pid=197 directly issued a syscall 396 at 0x400b37
exception: 0xa002030a (SYSTEM_ILLEGAL_FUNCTION_CALL)
```

This establishes that the direct `statfs` syscall caused this app termination.
The raw `getfsstat` call was not reached; it has also been removed rather than
repeating the same unsupported route. No system-UI modification was involved.

The replacement calls the native-title `_fstatfs` export through an opened
descriptor, following ProsperoStore's storage probe. Mount discovery uses bounded
queries of accessible paths. Directory parsing now rejects malformed records.
These changes compile and pass local tests but have not been hardware-validated.
Existing rendering evidence does not establish storage support.

The user subsequently reported `Operation not permitted` from the native-wrapper
build (`df14dd51f9fcc03c3da2e97c9839d93d8448978bd0e302c5fae1a1118ea366a5`).
The new System Explorer packages an opt-in upstream Lapy exact-title helper and
requests filesystem access once at startup. Localhost ELF loader port 9021 was
reachable during investigation. This is a transport check, not an elevation or
storage-success test. The new integration remains hardware-unvalidated.

The user tested the helper build and still reported `EPERM`. Captured syslog
shows `filesystem access status=9 path=helper`: a transport failure, not a
successful elevation followed by a storage error. The bootstrap was missing
libSceNet initialization and a network pool. The replacement initializes them
before the client request and logs socket creation, socket options, connection,
and send/receive failures with libSceNet's own error value. It still needs a
console test; network initialization is a source-confirmed missing step, not a
hardware-confirmed explanation of every transport failure.

The network-initialized artifact (`eboot.bin` SHA-256
`21581994b1143175288abe71838554480c8a7f8d0535ca185a0291500a85d9b8`)
connected to the ELF loader successfully. Its helper (SHA-256
`0117526a829bc55ead25a5c6633775e7aee4798aca738370d07dd2abbf08e313`)
crashed with SIGILL at SDK v0.40 startup/termination address `0x200006a38`.
The client received EOF and access status 9; the user still reported EPERM.
This confirms transport worked but filesystem elevation did not complete.
The SDK v0.42 helper replacement is pending hardware validation.

The replacement helper SHA-256 is
`2df1a7bcee26b183aa082081f47fd39a91e7039b9f420bd0eedc1ffaeda0d56b`.
The application executable is unchanged; identify this test by the helper hash.
Local compilation, exact-title protocol tests and ELF/SELF/package checks pass.
A native mock test using the actual patched credential functions confirms full
32-byte save/restore, retention of bytes 1–31, detection of high-byte mismatch
and rejection of failed attribute reads. These do not establish console access.

## System Explorer filesystem success — 2026-10-05

The user confirmed the replacement worked on the same PS5 Slim Digital,
firmware 13.60, with kstuff and ShadowMount, without etaHEN. This confirmation
applies to the native filesystem integration being tested: directory listing,
mount discovery and disk usage after the exact-title helper startup.

Tested title: `PPSA99056`, uploaded as a complete extracted application folder.
Application SHA-256:
`21581994b1143175288abe71838554480c8a7f8d0535ca185a0291500a85d9b8`.
Helper SHA-256:
`2df1a7bcee26b183aa082081f47fd39a91e7039b9f420bd0eedc1ffaeda0d56b`.
Both application and helper use payload SDK v0.42. The helper includes the
pinned 32-byte credential-attributes compatibility patch.

Evidence is the user's direct confirmation, not a captured successful syscall
trace or a measurement of every mount. It does not qualify unrelated native
modules, every filesystem operation, other firmware, or later rebuilt hashes.
Generated build receipts continue to default to `hardware_tested: false`; use
this record to identify the qualified artifacts.

## PKG installation — 2026-10-08

CFI-series console on firmware 13.60 (kernel `r229358/releases/13.60`), kstuff-lite,
John Törnblom's ELF loader on port 9021. `pkg-installer.elf` sent from a Mac to the
loader installed `CUSA04286` (BATTLESHIP, PS4 fake PKG, 1,246,167,040 bytes) from
`/data/downloads/packages/`: `transferring` for 15 s, `promoting`, then `playable`
at 16 s. A first build that imported `libSceAppInstUtil` never reached `main`; the
library is loaded by path. The in-app route (engine to loader on 127.0.0.1) is
recorded separately once tested.

## Render workers — 2026-10-10

Firmware 13.60, kstuff, etaHEN and ShadowMountPlus 1.7beta4. A development
build of uncommitted render-worker changes ran a private 3840×2160 app (not in
this repository) through the same scripted tour of browsing, scrolling and page
changes, with `dev/render-workers.txt` picking the count. The title saw 16 CPUs.
"Heavy" windows are one-second windows repainting more than 500 kilopixels per
frame; frames right after each tour screenshot are excluded.

The builds measured in this section were not recorded by hash or commit, and
some held uncommitted changes, so no row identifies a reproducible artifact.
They are performance observations of the approach on this console, not
qualification of any build, and make no compatibility claim.

| Workers | Heavy-window raster | Slow frames | Time over 16.7 ms | Full-screen raster (median) |
| --- | --- | --- | --- | --- |
| 1 | 23.1 ms | 160 | 9.3 s | 53.2 ms |
| 2 | 13.2 ms | 114 | 4.8 s | 34.2 ms |
| 4 | 10.1 ms | 71 | 3.3 s | 29.7 ms |
| 8 | 10.3 ms | 44 | 2.7 s | 15.7 ms |

With 768-row opacity strips the heap dropped to 192 MiB (328 MiB of flexible
memory free); 128-row strips rendered the tour at the same speed with the heap
at 256 MiB (368 MiB free) and identical screenshots apart from the clock. A
1920×1080 build of the same app at eight workers against the build before the
change: heavy-window raster 7.1 → 3.3 ms, slow frames 80 → 22, full-screen
present 0.9 ms in both. The starter has not been run with this change.

A later revision of the same app and tour (more pages, so not comparable with
the table above) then measured the two Embedded React patches below, each run
once, at eight workers. Screenshot frames are excluded.

| Build | Slow frames | Time over 16.7 ms | Row-scroll frames over | Full-screen fade frames over |
| --- | --- | --- | --- | --- |
| Before both | 36 | 1406 ms | 15 (574 ms) | 10 (272 ms) |
| Kept transform sources | 29 | 1024 ms | 10 (226 ms) | 8 (235 ms) |
| Plus deferred raster | 16 | 664 ms | 1 (28 ms) | 4 (82 ms) |

A focused card's scale animation took 30–43 ms per frame before its transform
source was kept, and about 3 ms after. The deferred-raster screenshots differ
from the previous build in under 0.07% of pixels, at the clock and a focus ring
caught mid-animation.

All three builds drew full-width scaled images only 2560 px wide, so the app's
hero backdrop lost its right third and its crossfades cost a third less than
they should. With that fixed (the wide-rows patch), the same tour showed the
whole backdrop and spent 1328 ms over budget in 50 slow frames: 39 full-screen
crossfade frames (765 ms, about 30 ms of raster each), 9 React frames (511 ms)
and 1 row-scroll frame (20 ms).

With `ERUI_MAX_NODES` and the bridge handles at 4096 and the app's home screen
kept mounted while hidden, the heap still took 256 MiB (104 MiB of flexible
memory free after it, against 107 MiB). Idle commits spent 0.1 ms in the damage
pre-pass and flag sweep, and no slow frame more than 0.3 ms. Deepest render
recursion over the tour, measured on the desktop preview: about 115 KiB of
stack at 8.8 KiB per level, against the workers' 1 MiB. Keeping the screen's
images held pinned about 60 MiB of decoded art while the next page loaded (heap
peak 238 MiB, one failed allocation, retried); under `ReleaseImages` the peak
was 176 MiB. Returning to it took a 79–81 ms frame (45–47 ms of React) and an
81 ms frame of focus handling, instead of rebuilding it. Idle collections held
7.4–9.2 MiB live against 3.5–7.2 MiB, and paused 14–34 ms against 10–32 ms.

The app's backdrops then became `<Image layer>`, drawn by the presenter beneath
the framebuffer, with finished image loads delivered under a per-frame budget.
One build, the same tour, once with layers and once with `dev/layers.txt` at 0:

| Build | Slow frames | Time over 16.7 ms | Full-screen crossfade frames over |
| --- | --- | --- | --- |
| Layers off | 45 | 1243 ms | 21 (446 ms) |
| Layers on | 26 | 832 ms | 0 |

What remains is React and layout work when a page mounts, mostly in the frame
an exit animation completes: 25–68 ms of React per tab switch, 83 ms for a
search keystroke, 200 ms for three quick L1 presses back to the home screen.
With layers on, full-screen slow frames spent a median 4.9 ms in present, against
3.7 ms; the PS5 present also includes the wait for the display.

Later builds of the same day, the same tour, one run each:

| Build | Time over 16.7 ms |
| --- | --- |
| Layers on (above) | 832 ms |
| Pages kept by `Screens`, preloaded | 842 ms |
| Plus idle collections waiting for a still screen | 694 ms |
| Plus label changes re-rendering only label users | about 495 ms |
| Plus the fix for nodes translated off screen | about 403 ms |
| Plus the 8 MiB collection room | about 419 ms |

Before the label change, a tab switch re-rendered about 157 motion elements and
three quick L1 presses took a 203 ms frame; after it, 69 ms (45 ms of React).
The last three rows differ by less than run-to-run noise in React time; what is
left is React work when a page mounts or a search key is pressed (40–63 ms).

A details sheet sliding 360 px over the screen, opened and closed twice with
every frame over 20 ms logged: before the translate copy every frame of both
slides took 20–24 ms (10–16 ms of raster); with it, only the frame that mounts
the page (39–55 ms, 16–30 ms of React) and the first frame of each exit (21 ms)
went over.

Garbage collection, a scripted burst of detail pages and grid scrolling, the
same build with three scheduling policies:

| Policy | Runs | Automatic collections | Idle pauses | Time over 16.7 ms |
| --- | --- | --- | --- | --- |
| Room 1.5× live, no still wait | 2 | 1 each, in a full-screen frame (51–58 ms) | 12–32 ms | 232–253 ms |
| Room at least 8 MiB, still wait with valve | 1 | 0 | 16, 26, 33 ms | 181 ms |
| Same, idle trigger at a sixth of live | 3 | 0 | five per run, 12–33 ms | 225–239 ms |

Every idle pause landed in a frame that repainted under 2% of the screen. A
longer session of fast browsing still reached the 8 MiB room once (a 79 ms
detail-page frame against 39 ms) and then paused 48 ms at 17.5 MiB live.

With `"refreshRate": 120` the console accepted 3840×2160 at 120 Hz on this
display. Page slides and row scrolls ran at 117–120 fps; windows with images
streaming in, search typing or a page's first mount fell to 70–100 fps. The
controller was in use during that run, so its tour path differed from the
60 Hz runs and the numbers are not a comparison.

Before and after, the same private app and 2.5-minute tour at 3840×2160 and
60 Hz, one run each from a clean build directory. "Before" is upstream
`6c168a6` with only the offscreen buffers sized for a 3840-wide render
(`ERUI_SCRATCH_*` and `ERUI_XFORM_*` 768, `ERUI_MAX_IMG_ROW_PIXELS` 3840), so
that full-width images and focus scales render at all, and the app's
`Screens`/`Screen` replaced by one keyed page under `AnimatePresence
mode="wait"`. "After" is this branch at a commit that was not recorded. Frames
over 33 ms are logged; frames right after each tour screenshot are excluded. The screenshots show the same
screens, except that on return to Home the branch keeps the scrolled row.

| | Before | After |
| --- | --- | --- |
| Frames over 33 ms | 217 | 4 |
| Time over 16.7 ms | 27.4 s | 0.09 s |
| Median of those frames | 138 ms | 41 ms |
| Worst frame | 855 ms | 42 ms |
| Home idle, hero animating | about 4 fps (each frame a full repaint, 133 ms raster + 113 ms upload) | 60 fps |

## Adaptive trigger resistance — 2026-10-10

Firmware 13.60, kstuff, etaHEN and ShadowMountPlus 1.7beta4, a DualSense, a development
build of a private app (not in this repository) from uncommitted changes.
`Controller.setTriggerResistance('l2', 0.75, 0.4)` while a search keyboard had focus, released
when focus left it: the user felt L2 resist from about 40% of its travel and go slack again
outside the keyboard. `scePadSetTriggerEffect` logged no error (the host logs only a nonzero
result). R2, other strengths and positions, and the release when the app quits were not checked.

## LAN broadcast (LocalNetwork) — 2026-10-10

Firmware 13.60, kstuff, etaHEN and ShadowMountPlus 1.7beta4, on the same LAN as a
Jellyfin-compatible server, a development build of a private app (not in this repository) from
uncommitted changes, driven by `dev/input-script.txt`. `LocalNetwork.broadcast({port: 7359,
message: 'who is JellyfinServer?'})` got the server's reply on the PS5: the app's sign-in screen
listed it, from the reply's sender address, and a confirm on that entry signed in. One server,
one reply; cancellation, `maxReplies`, several concurrent queries and a network without a
responder were not checked on the console.

## Video playback — 2026-10-11

Firmware 13.60, kstuff, etaHEN and ShadowMountPlus. `apps/video-lab` built as a test
identity (`PPSA99063`, a registered test title; executable SHA-256 `c1d0e563ba6deb0d1d99e0f2b9d41ed343ade598a4d978518ef37c877a702fdd`) from the
`video-playback` branch, with `dev/playlist.txt` and the files of `media/` copied to the title
folder. The TV was off, so nothing was seen or heard: the evidence is the host's log.

- **Decoders.** At launch, before filesystem elevation, `libSceVideodec2` loaded and the H.264,
  HEVC (Main 10) and VP9 decoders came up sized for 3840×2176, each with its own compute queue;
  flexible memory went from 531 to 369 MiB of 1024.
- **4K.** Big Buck Bunny 3840×2160 60 fps H.264 High (Blender's release), from the title folder
  and over http from a Mac on the LAN: 60 pictures shown per second with the audio (MP3) as the
  clock, 0 dropped in 20 s including a seek to 5:00 (over http, 0 in 25 s with a seek to 2:00).
  Earlier builds that took every picture already due dropped about 4 a second; picking within half
  a display frame fixed it. A 3840×2160 HEVC Main 10 clip tagged PQ: 24 fps, 0 dropped.
- **Six minutes.** The same 4K 60 fps file for 360 s with four pictures queued ahead: 29 dropped,
  24 of the player's display frames with no picture ready. Timing each `sceVideodec2Decode` call
  showed 0.2 ms for most and 50–57 ms for the 1 MB keyframes, every one of which matched a
  starvation. With eight queued and sixteen frame buffers (executable SHA-256
  `988c8cb242485ea3eaa8b489ea7802cd29b172c0741806bd6dc611837933ce88`): no starvation in 360 s, 21
  dropped, which is what a 60.00 fps file loses on a 59.94 Hz output (one every 16.7 s); heap
  71–88 of 384 MiB throughout, 0.8–1.2 cores; 9301 MiB of direct memory free after the frame
  buffers.
- **VP9.** A 3840×2160 60 fps clip of 600 pictures first showed 607 with 7 dropped: the decoder
  hands back a hidden (alt-ref) frame from the call that decodes it. Leaving those out: 600 shown,
  0 dropped. The same build: the HDR10 clip with a seek, 0 dropped; a 3840×2160 60 fps HEVC
  Main 10 open-GOP encode (x265, a CRA every 2 s with RASL pictures) with seeks to 5.5, 9.1 and
  13.3 s, 0 dropped and no decode errors.
- **Other files.** 1920×1080 H.264 (hardware), a 1280×720 H.264 4:4:4 MKV with AC-3 5.1 and AAC
  tracks and a 320×240 MPEG-4 Part 2 AVI with MP3 (FFmpeg): 0 dropped.
- **Upload.** About 1.5 ms per 4K frame through a storage buffer.
- **CPU.** The process's `getrusage` time over each two seconds: 1.0–1.2 cores busy for the 4K 60 fps
  H.264 file, 0.8 for the 4K HEVC one, 0.18 for the 320×240 MPEG-4 one (FFmpeg).
  `sceKernelGetProcessTime` (`DeviceInfo.processTime`) and `CLOCK_PROCESS_CPUTIME_ID` advance with
  the wall clock on the console, so they cannot measure this.
- **Stability.** 21 opens and closes in a row across the five files: every hardware decode reused
  its decoder, the heap stayed at 50–55 of 384 MiB, nothing failed.
- **Elevation.** With the decoders created at launch, filesystem elevation failed every time
  (`status=7`): the helper's log, sent over UDP with a temporary `/data/homebrew/dev.conf`, showed
  `target_preflight error=16` with 58 threads, and it takes only a one-thread process. Created after
  elevation, decoders failed at `QueryDecoderMemoryInfo` (0x811D0200) and compute queues at
  0x811D0111. Preparing everything but the decoders before elevation and creating them on first
  use: elevation `status=0`, and H.264, HEVC and VP9 decoders created afterwards played as above.
- **A Jellyfin client.** A development build of a private app (not in this repository) with
  `"video": true`, signed in to a Jellyfin-compatible server on the LAN: Play resumed a 2002×1080
  H.264 film at the server's saved position and played it through the hardware decoder at 24 fps,
  0 dropped in 20 s, pictures within about 1 ms of the audio clock, with native networking up.
- **Network loss.** The 4K 60 fps file over http, with the Mac's server stopped 15 s into
  playback. For 40 s: the 30 s read ahead played, then 16 s of buffering, and FFmpeg's own
  reconnect carried on. For 150 s, earlier: FFmpeg stopped reconnecting, the end of the read-ahead
  counted as the end of the file, and the audio clock stopped with seven pictures queued: the
  player stood still, reporting `ready`, and did not recover. With the stream opened again where
  it broke off and the clock handed over (executable SHA-256
  `58428b9dbf3acb3082067b1b54831fcc58699f1766ae5364f97063a47dc1d549`): `buffering` at 62.9 s, playing again at 63.9 s once the
  server was back, nothing skipped or repeated.
- **Not checked.** Picture and sound on a TV, A/V sync by eye or ear, playback longer than six
  minutes, a real Jellyfin server.

## Video upscalers and 10-bit pictures — 2026-10-11

Firmware 13.60, kstuff, etaHEN and ShadowMountPlus. `apps/video-lab` built as the test identity
`PPSA99063` from the `artcnn-spike` branch (executable SHA-256 `bafa433c64f3a108d135633932a8374528b1c921b811e30755f50a78e028e1f8`), playing 1920×1080
and 1280×720 24 fps encodes of Big Buck Bunny from the title folder with `dev/playlist.txt`, the TV
on.

- **10-bit.** A 1920×1080 HEVC Main 10 clip showed green from start to end. The decoder's luma,
  copied out on the GPU, peaked at 4 of 255 where an 8-bit encode of the same frames reaches 242:
  `sceVideodec2` writes 10-bit samples low in their 16-bit words, and the presenter read them as
  P010 (high). Read as low-aligned (`nv12_10`), the luma peaks at 242 (the picture on the TV not yet confirmed).
- **Correctness.** The ArtCNN C4F16 output of a 1920×1080 frame (H.264 and HEVC Main 10), read
  back from the GPU, against the ArtCNN ONNX model run on the same input with ONNX Runtime on a
  Mac: at most 1/255 apart (H.264; 3/255 for the 10-bit frame, whose reference input was rounded
  to 8 bits), 62.6 dB. Anime4K has no reference model; its outputs stay within 45–47 dB of a
  bilinear doubling with the same mean, as a residual on it should.
- **GPU time** per picture, from `GL_TIME_ELAPSED` queries around the passes, 1920×1080 to
  3840×2160: ArtCNN 12.5 ms (6.1 ms from 1280×720), Anime4K S about 3, M 4.1, L 6.2, VL 12.0,
  UL 20.0 (10.0 from 1280×720).
- **Display rate.** With every pass in the display frame its picture is due, ArtCNN took the
  presenter from 60 to 36 frames a second (each new picture missed a vsync). Split over two display
  frames: 60 for ArtCNN and Anime4K S to VL with 1080p sources, 52–58 for Anime4K UL (its longest
  half 13 ms), 60 for UL with a 720p source. 0 pictures dropped once a model was compiled.
- **Compiling** a model the first time it is used holds the render thread 0.5–0.8 s; the player
  dropped 18–37 pictures then.

## HDMI audio passthrough — 2026-10-11

Firmware 13.60, kstuff, etaHEN and ShadowMountPlus, the console's HDMI output on a Dell AW3225QF
monitor. Read-only probes sent as payloads, not the app:

- `sceVideoOutGetHdmiRawEdid_(buffer, 513)` returned 0 with a block count (3) followed by the
  monitor's EDID: base block, CTA-861 and DisplayID extensions, each checksum valid. The first
  header byte read 0x09 where the standard has 0x00; the parser does not check the header. The CTA
  block has no audio data block and its basic-audio flag is clear, so `AudioOutput.capabilities()`
  would list no formats and no passthrough codecs for it (`tools/tests/media/edid-monitor-no-audio.bin`
  is that EDID with its serial numbers blanked).
- `sceAudioOutExGetMonitorInfo(0, 0, buffer, 1024)` also answers from a payload, with 889 bytes of
  a layout of its own that contains the EDID's vendor, model and HDR metadata; the EDID is read
  instead because its audio descriptors are standard.

Not checked: the app itself (`apps/video-lab` as `PPSA99064` was built and uploaded, but
ShadowMountPlus did not register the new title within 25 minutes, and another app was running on
the console), so `sceAudioOutExConfigureOutput` and `sceAudioOutExOpen` have not been called from
PS5 React, nor has a bitstream reached a sink. SlopFin reports them working from its app on
firmware 8.20, with AC-3, E-AC-3 and DTS heard on a TV. A/V sync with a real receiver is unmeasured.

## Not yet validated

- `Power.keepAwake` (ABI v4, `sceSystemServicePowerTick` every 30 s): builds,
  but no console has been observed staying out of rest mode with it on.
- HDMI audio passthrough (`<Video audioOutput>`, ABI v13): the bitstream output has not been opened
  from a PS5 React app; see above.

## Remote images — 2026-10-06

Overdrive (`PPSA99058`, game-store `65a3e58`) on the same PS5 Slim Digital,
firmware 13.60, kstuff, console filesystem elevation through the bundled
helper. A development build of ps5-react `b6047eb` with an uncommitted
per-frame motion log, screenshot command and heap log loaded every cover,
hero, logo and screenshot from the Steam and Spectrum CDNs; console
screenshots show them on browse and game pages.

- Held-key scroll on browse (14 down, 14 up) while covers streamed in: 225
  motion frames, none motionless, longest frame 17.7 ms, none over 20 ms.
- One-second windows: 59.9–60.0 fps, p95 17.3 ms. Game page mount 25 ms
  (cached art); user-driven mounts of uncached pages 43–88 ms, all JavaScript.
- 128 MiB heap: live 64–71 MiB on browse, about 80 MiB with a game page, peak
  98 MiB, no allocation failures.

The release artifact without diagnostics (eboot SHA-256
`4592332f53eaa597116faee4ecf98d1edb3d512a896b9cd426c4c35a7d0c7606`) was
installed and launched: first frame and 60 fps windows, not re-measured in
detail. Downloads were not exercised.

### Image loading latency — 2026-10-06

Same console, Overdrive game-store `ee3d2a4`, ps5-react `6bfdbec` (official
eboot SHA-256 `9f6486d9ddf6037762dfd3a579038b40622efb95afa795634d2fa57e0f943569`).
Per-image timings came from a temporary log in a development build; the 76
images a cold launch mounts before its first screenshot:

| Launch | Median to visible | Slowest |
| --- | --- | --- |
| Network, 4 connections | 4.2 s | 5.7 s (queued up to 5.2 s) |
| Network, 12 connections | 1.8 s | 2.5 s |
| Disk cache (relaunch) | 0.15 s | network only for art not cached |

A Steam CDN request takes about 250 ms (750 ms for the first, with DNS and
TLS); decode and resample take 1–30 ms on the decode worker. Opening a game
page whose card had focus for 300 ms showed hero, cover, logo and first shot
in the first screenshot 300 ms later; without the settle the hero arrived
after 0.7 s and the first shot after 1.1 s. Scroll windows stayed at
59.9–60.2 fps with frames under 20 ms while images arrived.
