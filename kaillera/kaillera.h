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
