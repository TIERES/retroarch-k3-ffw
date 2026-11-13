# CLAUDE.md

This repository is the RetroArch fork used by WE Camp and the Kaillera client. Preserve the existing fork-specific integrations when changing upstream RetroArch code.

## WE Camp Broadcaster Branch

Branch: `feature/wecamp-broadcaster`, paired with `feature/wecamp-broadcaster` in `kaillera-client`.

Purpose: run a headless Watch Live spectator for a WE Camp match. RetroArch must be launched with the PlayStation core and matching game content already loaded. In `core_run()` (`runloop.c`), `kailleraBroadcasterStartIfRequested()` checks for the environment variables below and loads the Kaillera DLL once.

- `WECAMP_BROADCASTER=1` enables the headless worker behavior and hides the replay toolbar.
- `WECAMP_WATCH_SESSION_ID=<live session id>` selects the WE Camp live input session.
- `WECAMP_WATCH_ROOM=<display name>` is optional.

`kaillera/kaillera.c` resolves the optional `kailleraWatchSession` DLL export and loads `kailleraclient.dll`; the broadcaster build of that DLL sees the same environment and starts Watch Live without opening the Kaillera lobby window. Keep the state pump active: it advances the existing `KSSDFA` transitions and invokes RetroArch's game callback.

### Changed files in this branch

- `kaillera/kaillera.c`: environment-triggered startup and replay toolbar suppression.
- `kaillera/kaillera.h`: broadcaster startup declaration.
- `runloop.c`: per-frame startup hook.

### Handoff/build status

The paired Kaillera DLL compiled successfully with VS2026/MSVC v145 and exports `kailleraWatchSession`. RetroArch has not yet compiled successfully on the original development machine: the installed MinGW GCC exited with code 1 without diagnostics, including while compiling unchanged `version_git.c`. Compile this branch on the other machine and fix/report compiler diagnostics before deploying it.

External Xvfb capture, FFmpeg encoding, YouTube broadcast creation, and complete match-start automation are not implemented by these source changes; they remain integration work. Do not remove the existing Kaillera, live-watch, replay, or sync behavior while simplifying this broadcaster build.
