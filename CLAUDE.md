# CLAUDE.md

This repository is the RetroArch fork used by WE Camp and the Kaillera client. Preserve the existing fork-specific integrations when changing upstream RetroArch code.

## WE Camp Broadcaster Branch

Branch: `feature/wecamp-broadcaster`, paired with `feature/wecamp-broadcaster` in `kaillera-client`.

Purpose: run a headless Watch Live spectator for a WE Camp match. RetroArch must be launched with the PlayStation core and matching game content already loaded. In `core_run()` (`runloop.c`), `kailleraBroadcasterStartIfRequested()` checks for the environment variables below and loads the Kaillera DLL once.

- `WECAMP_BROADCASTER=1` enables the headless worker behavior and hides the replay toolbar.
- `WECAMP_WATCH_SESSION_ID=<live session id>` selects the WE Camp live input session.
- `WECAMP_WATCH_ROOM=<display name>` is optional.
- `WECAMP_STREAM_URL=<rtmp://... or file>` turns on the stream output. Without it, the worker only watches.
- `WECAMP_BROADCASTER_START_TIMEOUT=<s>` (default 120): quit if the Watch Live game has not started by then.
- Stream options (all optional): `WECAMP_FFMPEG`, `WECAMP_STREAM_SIZE` (640x480), `WECAMP_STREAM_FPS` (30), `WECAMP_STREAM_VBITRATE` (1500k), `WECAMP_STREAM_ABITRATE` (128k), `WECAMP_STREAM_PRESET` (veryfast), `WECAMP_FFMPEG_LOG`.

`kaillera/kaillera.c` resolves the optional `kailleraWatchSession` DLL export and loads `kailleraclient.dll`; the broadcaster build of that DLL sees the same environment and starts Watch Live without opening the Kaillera lobby window. Keep the state pump active: it advances the existing `KSSDFA` transitions and invokes RetroArch's game callback.

One session per process:
1. When `kailleraNetplay` turns on, `kailleraBroadcasterStartIfRequested()` starts the stream with `recording_init()`. It calls it directly because `CMD_EVENT_RECORD_INIT` is refused during Kaillera games.
2. `EndKailleraGame()` stops the stream before the content closes.
3. Then it calls `retroarch_main_quit()`. `CMD_EVENT_QUIT` is refused while the DLL is loaded, and the DLL's headless loop never returns.

### Stream output (`record/drivers/record_wecamp.c`)

This is a record driver named "wecamp", registered before "null" in `record_drivers[]`. Its init returns NULL unless `WECAMP_BROADCASTER` is set.

**Data path**
- It sends core frames and raw s16 audio to an external `ffmpeg.exe`, using two overlapped named pipes: rgb565le video at a fixed size, and s16le audio.
- ffmpeg encodes x264 + AAC, then writes FLV to RTMP, or a file.
- The fork itself is built with `HAVE_FFMPEG = 0`.

**Pacing**
- During Watch Live the main thread blocks in the DLL whenever live input is late.
- So two threads paced by the wall clock feed the pipes.
- Video: the newest frame at least 80 ms old, repeated during stalls.
- Audio: a ring buffer with ±0.5% resampling around an 80 ms target. Silence on underrun, a drop on overflow.
- Fast-forward frames are not sent.

**Recovery and priority**
- If ffmpeg dies, or does not read for 30 s, the driver restarts it with backoff.
- ffmpeg runs at below-normal priority.

**Deployment**
- The worker config is in `kaillera-client` `tools/broadcaster/`: video/audio/input drivers null, `vrr_runloop_enable` for timer pacing.
- With that config it runs without a window, even from an SSH session.

### Changed files in this branch

- `kaillera/kaillera.c`: environment-triggered startup, stream start/stop, quit at the session end, start timeout, replay toolbar suppression.
- `kaillera/kaillera.h`: broadcaster startup declaration.
- `runloop.c`: per-frame startup hook.
- `record/drivers/record_wecamp.c` (new), `record/record_driver.c/.h`, `Makefile.common`: the "wecamp" record driver.

### Handoff/build status

Builds with MSYS2 MINGW64: `make OUTDIR=out -j8`. The only non-system DLLs needed are libwinpthread-1, libstdc++-6 and libgcc_s_seh-1.

Test 2026-11-18 on the broadcaster box (`C:\broadcaster`): Watch Live of a finished session went through `out\*.flv` with correct image and game audio, at about 15% CPU.

YouTube RTMP broadcast creation and match-start automation are still integration work. Do not remove the existing Kaillera, live-watch, replay, or sync behavior while simplifying this broadcaster build.
