 //******************************************************//
 //************ KAILLERA NETPLAY - ANTI-DESYNC **********//
 //******************************************************//
#ifndef KAILLERA_SYNC_H
#define KAILLERA_SYNC_H

#include <stdarg.h>
#include <boolean.h>

/* Kaillera only keeps the players' *inputs* in lockstep - it assumes every
   machine starts the game from a bit-identical state and emulates it with
   bit-identical settings. Nothing used to enforce that, so each player's own
   memory card, core options, BIOS and controller type silently leaked into
   the match and desynced it. This module closes those gaps:

   1. Neutral start: no SRAM load/save, no auto-loaded savestate, no restored
      disk index while a Kaillera game is loading/running.
   2. Canonical settings: determinism-relevant core options (PCSX ReARMed) and
      controller types are forced to the same values on every machine - the
      player's own .opt / remaps are only overridden in memory, never saved.
   3. Fingerprint handshake: once the content is loaded, every player posts a
      "[SYNC] ..." game-chat line (core version, content CRC32, BIOS actually
      loaded, forced-settings hash) and compares it with everyone else's,
      warning on screen when something differs.
   4. Desync detector: every ~5s each player posts a digest of the core's
      system RAM for the same frame window; the first mismatch is shown on
      screen and, like everything above, written to logs\kaillera_sync.log.

   All of it is gated on kailleraInitialisedInternal, i.e. only while a
   Kaillera game (live, retry-connect, playback or Watch Live) is loading or
   running - offline play keeps each player's settings untouched. */

/* Main thread, once, from LoadKaillera() - before any game can start. */
void kailleraSyncInit(void);

/* From the Kaillera game callback (DLL thread), before content loads. */
void kailleraSyncGameBegin(int num_players, bool playback);

/* Main thread, right after the Kaillera game's content finished loading. */
void kailleraSyncContentLoaded(void);

/* Main thread, once per Kaillera frame (core_run()). */
void kailleraSyncFrameTick(void);

/* Main thread, right after each Kaillera frame's retro_run(), with that
   frame's number (current_core_frame) - feeds the periodic RAM digest that
   detects an actual mid-match desync (4. below). */
void kailleraSyncAfterFrame(unsigned frame);

/* Retry-connect go-live (end of the countdown, host and peers alike): every
   side resumes from the same loaded state, so the RAM desync detector -
   off during the replay phase, whose frame counters differ per machine -
   starts here, numbering frames from this point. */
void kailleraSyncRetryConnectGoLive(void);

/* Whenever a Kaillera game ends. */
void kailleraSyncGameEnd(void);

/* From the chat callback (DLL thread). Returns true when the line was a
   "[SYNC]" handshake line and must not be shown as normal chat. */
bool kailleraSyncHandleChat(const char *nick, const char *text);

/* True while a Kaillera game is loading or running. */
bool kailleraSyncActive(void);

/* RETRO_ENVIRONMENT_GET_VARIABLE hook: forced value for `key`, or NULL. */
const char *kailleraSyncForcedCoreOption(const char *key);

/* command_event_init_controllers() hook: device to plug into `port`. */
unsigned kailleraSyncForcedDevice(unsigned port, unsigned device);

/* libretro log callback hook - picks up which BIOS the core loaded. */
void kailleraSyncCoreLog(const char *fmt, va_list vp);

#endif
