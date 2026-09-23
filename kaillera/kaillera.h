 //******************************************************//
 //****************** KAILLERA NETPLAY ******************//
 //******************************************************//
#ifndef KAILLERA_H
#define KAILLERA_H


#define MAX_GAMES 100
#define MAX_INPUTS 8

//#define KAILLERA_DEFAULT_MINIMAL 2
//#define KAILLERA_DEFAULT 4
#define KAILLERA_NEED_ANALOG 8

#define INIT_FRAMES 120

#define COMMAND_SAVE_STATE 1
#define COMMAND_LOAD_STATE 2
#define COMMAND_RESET 3
#define COMMAND_SWAP 4
#define COMMAND_TAKE 5
#define COMMAND_SWAP_RESET 6

/* retry-connect - must match kaillera-client's kcore/k_instruction.h
   RC_ACTION_* exactly (two separate repos, no shared header). */
#define RC_ACTION_PAUSE     1
#define RC_ACTION_RESUME    2
#define RC_ACTION_GO_LIVE   3
#define RC_ACTION_REWIND_TO 4 /* reserved - not implemented yet */
#define RC_ACTION_STATE_READY 5


typedef struct {
   int playerNumber;
} player;


extern player players[MAX_INPUTS];
extern int kaillera_swap_cntr;

void input_state_kaillera(unsigned port, unsigned device, unsigned idx, unsigned id);
void LoadKaillera();
int kailleraSyncData(void* value, const int len);
void EndKailleraGame();
void GetClientVersion(char* version);
void AddGamesToList();
void kMessage_core_info();
void kailleraChatSendExternal(const char* messge);
void cp1251_to_utf8(char* out, const char* in);
extern bool kailleraNetplay;
extern bool kailleraInitialised;
// True whenever the active Kaillera session is n02's Playback mode (static
// .krec file, "Replays Online", or Watch Live) rather than P2P/Server. Unlike
// those two, Playback has no live peer to stay in lockstep with, so hotkeys
// normally disabled while any Kaillera session is active (see kailleraInitialised
// checks) can safely be allowed. Only meaningful once a game has actually
// started (set from kailleraGameCallback() via the optional
// kailleraIsPlaybackMode() DLL export - absent on older/other Kaillera DLLs,
// which always leave this false).
extern bool kailleraPlaybackMode;

/* retry-connect - resume a dropped match from a recorded .krec as a
   synchronized group replay, navigated with RetroArch's own native Pause key
   (host only, repeatable) and committed to live play with Enter (host only,
   only valid while paused). See kaillera-client's kcore/kaillera_retryconnect.h
   for the DLL-side half of this - these are thin wrappers around 3 optional
   exports (kailleraRetryConnectCanControl/NotifyLocalControl/Poll), resolved
   via GetProcAddress like kailleraIsPlaybackModeF above and safely no-op on
   any DLL that doesn't have them.

   True only for the room's host while a retry-connect session is active -
   must be checked before letting local Pause/Enter do anything during a
   Kaillera session (a peer's local presses must not affect anyone). */
bool kailleraRetryConnectCanControl();

/* True for every client (host or peer) while a retry-connect session is
   active - unlike kailleraRetryConnectCanControl() above, does not imply
   "is host". Needed to tell a retry-connect peer apart from someone using
   n02's ordinary standalone Playback/Watch mode (kailleraPlaybackMode is
   true in both cases, but only the former has a host to defer to): peers
   must not be able to fast-forward on their own during a group replay (see
   the fast-forward hotkey block, runloop.c), while solo playback keeps
   fast-forwarding freely for whoever's watching. */
bool kailleraRetryConnectActive();

/* Call when the local user (already confirmed to be the host via
   kailleraRetryConnectCanControl() above) presses native Pause/Resume, or
   Enter to go live - relays it to every other player via the DLL, tagged
   with the current frame (current_core_frame below). action is one of
   RC_ACTION_PAUSE/RESUME/GO_LIVE. */
void kailleraRetryConnectNotify(int action);

/* Re-evaluates kailleraPlaybackMode from kailleraIsPlaybackModeF() (the DLL
   export) right now, instead of waiting for the next kailleraGameCallback()
   (the only other place it's set - see kailleraPlaybackMode's own doc
   comment above). Needed because a retry-connect group replay can end (go
   live) mid-match, unlike every other case that variable covers, which only
   change at a match boundary; without this, fast-forward could stay allowed
   into live play. Call this anywhere kailleraPlaybackMode might have just
   gone stale - currently: every kailleraNetplay frame (runloop.c) and
   whenever kailleraRetryConnectPauseTick() below processes a GO_LIVE. */
void kailleraRetryConnectRefreshPlaybackMode();

/* Call once per frame while the frontend is natively paused (runloop.c's
   RUNLOOP_STATE_PAUSE branch) - this is the only channel retry-connect has
   to reach a paused client, since the normal per-frame sync
   (kailleraSyncData(), called from core_run()) does not run at all while
   paused. Detects the local Enter key (only acted on for the host, and only
   while genuinely paused - Enter has no effect otherwise) and drains/applies
   any PAUSE/RESUME/GO_LIVE a peer sent while we were paused. */
void kailleraRetryConnectPauseTick();

/* Call once per core_run() frame (kailleraNetplay only) - the complement to
   kailleraRetryConnectPauseTick() above for the common case where we're NOT
   currently paused. Drains and applies any PAUSE/RESUME/GO_LIVE/STATE_READY
   a peer sent, exactly as kailleraRetryConnectPauseTick() does while paused -
   needed because fast-forward during a group replay is host-only and purely
   local (see kailleraRetryConnectUploadState() below), so everyone else just
   keeps running core_run() normally at their own pace until the host's next
   signal arrives, and that signal has to be caught from here, not from the
   paused-only channel. */
void kailleraRetryConnectFrameTick();

/* True for a short window (both host and peer) right after the go-live
   countdown ends and control is actually handed back - see
   RETRYCONNECT_NEUTRAL_INPUT_MS in kaillera.c. While true, the local
   player's real controller state should still be sampled/polled normally
   (so hotkeys etc. keep working) but must NOT be sent as this frame's
   Kaillera input - runloop.c zeroes its own slot of netjoy/netjoy_ex right
   before kailleraSyncData() ships it, so both sides exchange one guaranteed
   second of clean neutral frames before real input starts flowing, instead
   of whatever each player happened to be physically holding at the
   arbitrary, unsynchronized instant control came back. Always false outside
   retry-connect (starts false, only ever armed by the go-live countdown
   above). */
bool kailleraRetryConnectSuppressLocalInput();

/* Host-only: call right after locally pausing (the pause hotkey block,
   runloop.c) - takes a core_serialize() snapshot of exactly this frame and
   hands it to kailleraRetryConnectUploadState() below. Every host Pause
   sends a fresh one (host may pause/resume/re-pause any number of times
   while searching for the right moment; the peer just loads whichever one
   arrived most recently, staying paused - see kaillera.c's
   ApplyRetryConnectStateReady()). No-op if not host (defense in depth -
   the caller is expected to have already gated this itself). */
void kailleraRetryConnectCaptureAndSendState();

/* Host-only: uploads `data`/`size` (a core_serialize() blob, taken the
   instant fast-forward stops - see the pause hotkey block, runloop.c) to the
   community server and, on success, notifies every other player
   (RC_ACTION_STATE_READY) so they load that exact state instead of trying to
   reach the same frame by replaying frame-by-frame themselves. Fast-forward
   during a group replay is host-only and purely local for exactly this
   reason: different machines/cores aren't guaranteed to reach the same frame
   at the same real-world time, so nobody but the host ever fast-forwards -
   everyone else jumps via this state hand-off instead. No-op (and no error)
   if the DLL predates this export or the caller isn't host. */
void kailleraRetryConnectUploadState(const void* data, int size);

/* Downloads the state kailleraRetryConnectUploadState() above just uploaded,
   into the caller-owned outBuffer (capacity bufferCap - size it from
   core_serialize_size(), which must match across every client since they all
   run the same core/content). Returns the number of bytes written (pass
   straight to core_unserialize()), or -1 on any failure (network, no state
   uploaded yet, DLL too old, buffer too small). *outFrameIndex receives the
   .krec frame index the state was taken at, so the caller can also fast
   -forward (skip, not simulate) its own local replay position to match. */
int kailleraRetryConnectDownloadState(void* outBuffer, int bufferCap, int* outFrameIndex);

/* Checkpoint-based rewind for solo "Reproducao de Replay" (static local-file
   Playback - kailleraPlaybackMode true, but NOT a retry-connect group replay
   and NOT "Watch Live" streaming, neither of which have a fixed underlying
   file to rewind within). Call once per frontend tick (both while paused and
   while running - the Left-arrow rewind key needs to work either way,
   mirroring retry-connect's own Pause/FrameTick split). No-op (cheap) outside
   that specific mode. See kaillera.c for the actual checkpoint ring buffer. */
void kailleraPlaybackRewindTick();

extern volatile int kailleraInitialisedInternal;
extern int kNumPlayers;
extern int kPlayerNumber;
extern int kReceivedCommand;
extern int stop_execute_shit;
extern char* fname;
extern char* cpath;
extern unsigned int current_core_frame;

extern int kailleraPacketSize;
extern int kailleraCommands;
extern int16_t joy[MAX_INPUTS][6];
//extern int16_t netjoy_min[MAX_INPUTS][1];
#ifdef KAILLERA_DEFAULT
extern int16_t netjoy[MAX_INPUTS][2];
#endif
extern int16_t netjoy_ex[MAX_INPUTS][6];
//extern int kaillera_buttons_write;
extern int kailleraSwitch;

extern unsigned track_0_port_cntr;
extern unsigned _trackal_device[100];
extern unsigned _trackal_idx[100];
extern unsigned _trackal_id[100];

#endif
