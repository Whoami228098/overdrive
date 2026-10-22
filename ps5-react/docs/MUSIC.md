# Local music

Set `"music": true` in the application manifest and import `Music` from
`@ps5-react/core`. This opens the normal stereo mixer at host startup. It does
not enable video decoding or reserve the hardware video workspace.

```js
import {Music} from '@ps5-react/core';
const id = Music.play('/app0/media/music/track.mp3');
Music.setVolume(0.35);
Music.setPaused(true);
const status = Music.getStatus();
Music.stop();
```

`play` returns an ID immediately. It replaces an older request; there is one
pending path, one decoder, an explicit 512 KiB worker stack and one 32,768-frame stereo ring (256 KiB). Only local
paths accepted by the host's normal filesystem policy are supported. Opening,
reading, MP3 decoding, mono duplication and conversion to the mixer's 48 kHz
format run on one worker. A bounded linear interpolator retains its phase
across source blocks. The pinned, unmodified `dr_mp3.h` uses the MIT-0 option
included at the bottom of that header; its revision/hash are in the dependency
lock. There is no whole-song or playlist PCM allocation.

`getStatus()` returns `{id, state, paused, position, duration, volume, error}`.
States are `idle`, `loading`, `playing`, `ended` and `failed`. Polling also routes
the latest ring to the mixer's second stream slot; poll about four times per
second while playing. Position follows consumed PCM. `ended` waits for the
audio ring to drain. A pause detaches the stream rather than silently consuming
its queued music. Video keeps its original first stream slot and UI effects
remain separate voices. Master `Sound.setVolume` affects the whole mixer;
`Music.setVolume` affects music only.

Replaced rings remain owned for several audio grains, following the existing
video lifetime contract. A full mixer command queue retains the previous owner
and retries the change. Host cleanup cancels and joins the decoder, stops the
audio callback, then releases ring owners. No JS control opens a file, decodes
audio or joins a worker.

`tools/test_music.py` compiles the actual worker with ASan/UBSan/leak checking.
It needs a host C++20 compiler and the FFmpeg command-line tool (also used by the
normal build's music checks); `npm run setup` installs these prerequisites.
Its owned FFmpeg sine fixtures exercise resampling, mono duplication, pause and
audible completion, missing/corrupt-file recovery, superseded requests and
repeated shutdown/restart. A complete decoded stereo output is compared with
FFmpeg. These are host checks, not evidence of console audio output.
