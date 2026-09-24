//******************************************************//
//****************** KAILLERA NETPLAY ******************//
//******************************************************//

//#include "n02.h" // uncomment to use n02 callbacks instead of original kaillera

#ifdef N02_LINUX
#include "runloop.h" 
#include <pthread.h>
static pthread_t threadk;
#else
#include "gfx/common/win32_common.h"
#endif
#include "playlist.h"
#include "../defaults.h"
#include "verbosity.h"
#include "paths.h"
#include "string/stdstring.h"
#include "kaillera.h"
#include "../tasks/tasks_internal.h"
#include "../libretro-common/include/queues/message_queue.h"
#include "command.h"
#include "version.h"
#include "core.h"
#include "runloop.h"
#include "input/input_driver.h"
#include "gfx/video_driver.h"
#include <commctrl.h> /* TOOLTIPS_CLASSA - playback toolbar's button tooltips */
#include <commdlg.h> /* GetOpenFileNameA - kailleraFindOrBrowseGame()'s manual-pick fallback */
#include "file/file_path.h" /* path_is_valid/fill_pathname_basedir - same fallback's folder search */

 
bool kailleraInitialised;
bool kailleraNetplay;
bool kailleraPlaybackMode;
int kNumPlayers;
int kPlayerNumber;
#ifdef KAILLERA_DEFAULT
int kailleraPacketSize = 0;
#endif
int kailleraSwitch = 0;
//int kaillera_buttons_write = 0;
int kReceivedCommand;
char* fname;
char* cpath;
player players[MAX_INPUTS];


int totalGames;
char kailleraRomNames[MAX_GAMES * 128];
char filePathK[MAX_GAMES][512];
char corePaths[MAX_GAMES][512];
char* kailleraGames;

#if !defined(N02_WIN32) && !defined(N02_LINUX)
extern int (WINAPI* kailleraIsPlaybackModeF)();
#endif


#if defined(N02_WIN32) || defined(N02_LINUX)
static int N02CCNV nGameLoad(char* game, int player, int numPlayers, int gameplayType)
#else
static int WINAPI kailleraGameCallback(char* game, int player, int numPlayers)
#endif
{
   settings_t* settings = config_get_ptr();

   kPlayerNumber = player;
   kNumPlayers = numPlayers;

   if (kNumPlayers > MAX_INPUTS)
      kNumPlayers = MAX_INPUTS;

#if !defined(N02_WIN32) && !defined(N02_LINUX)
   /* kailleraIsPlaybackModeF is only ever non-NULL here (see LoadKaillera()
      below) - the Open Kaillera n02 path above has no such export. */
   kailleraPlaybackMode = (kailleraIsPlaybackModeF != NULL) && (kailleraIsPlaybackModeF() != 0);
#endif

   settings->bools.preemptive_frames_enable = false;
   settings->bools.menu_pause_libretro = false;
   settings->bools.run_ahead_enabled = false;

   for (int n = 0; n < MAX_INPUTS; n++)
      players[n].playerNumber = n;

   fname = kailleraRomNames;

   for (int i = 0; i < totalGames; i++) {
      if (strncmp(fname, game, strlen(fname)) == 0) {
         fname = filePathK[i];
         cpath = corePaths[i];
         break;
      }
      fname += strlen(fname) + 1;
   }

   kailleraInitialisedInternal = 1;
   kailleraSwitch = 0;
   kailleraWaitSaveLoad = 0;
   stop_execute_shit = 0;
#ifdef KAILLERA_DEFAULT
   kailleraPacketSize = 0;
#endif
   //kaillera_buttons_write = 0;
   current_core_frame = 0;
   track_0_port_cntr = 0;

#ifdef _WIN32
   SetForegroundWindow(main_window.hwnd);
#endif
   return 0;
}

#if defined(N02_WIN32) || defined(N02_LINUX)
static void N02CCNV nChatReceived(const char* nick, const char* text)
#else
static void WINAPI kailleraChatReceivedCallback(char* nick, char* text)
#endif
{
   char new_msg[320];
   new_msg[0] = '\0';

   if (*text == '!') {
      text++;
      if (strncmp(text, "swap", 4) == 0) {
         while (*++text != ' '); 	text++;
         int n1 = atoi(text);
         while (*++text != ' '); 	text++;
         int n2 = atoi(text);
         if ((n1 > 0 && n1 <= kNumPlayers) && (n2 > 0 && n2 <= kNumPlayers)) {
            kailleraCommands = COMMAND_SWAP;
            kailleraCommands |= ((n1 & 0xF) << 12) | ((n2 & 0xF) << 8);
            int p1 = players[n1 - 1].playerNumber;
            int p2 = players[n2 - 1].playerNumber;

            snprintf(new_msg, sizeof(new_msg), "Controllers %i and %i have been swapped", p1 + 1, p2 + 1);
         }
      }
      else if (strncmp(text, "take", 4) == 0) {
         while (*++text != ' ');	text++;
         int n1 = atoi(text);
         while (*++text != ' ');	text++;
         int n2 = atoi(text);
         if ((n1 > 0 && n1 <= kNumPlayers) && (n2 > 0 && n2 <= kNumPlayers)) {
            kailleraCommands = COMMAND_TAKE;
            kailleraCommands |= ((n1 & 0xF) << 12) | ((n2 & 0xF) << 8);

            snprintf(new_msg, sizeof(new_msg), "Player %i took controller %i", n1, n2);
         }
      }
      else if (strcmp(text, "reset") == 0) {
         kailleraCommands = COMMAND_SWAP_RESET;

         snprintf(new_msg, sizeof(new_msg), "Swap has been reset!");
      }
      else if (strcmp(text, "version") == 0) {
         char ver[48];
         GetClientVersion(ver);
         kailleraChatSendExternal(ver);
      }
      else if (strcmp(text, "list") == 0) {
         bool player = false;
         char txt[32];
         for (int i = 0; i < kNumPlayers; i++) {
            if ((players[i].playerNumber + 1) == kPlayerNumber) {
               char n[16];
               if (!player) {
                  sprintf(txt, "is player %i", (i + 1));
                  player = true;
               }
               else {
                  sprintf(n, " %i", (i + 1));
                  strcat(txt, n);
               }
            }
         }
         if (!player) sprintf(txt, "is a lurker");
         kailleraChatSendExternal(txt);
      }
   } else
      snprintf(new_msg, sizeof(new_msg), "%s: %s", nick, text);

   if (strlen(new_msg) > 0) {
      runloop_msg_queue_push(new_msg, 0, 5 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
   }
}

#if defined(N02_WIN32) || defined(N02_LINUX)
static void N02CCNV nPlayerDropped(const char* nick, int playernb)
#else
static void WINAPI kailleraClientDroppedCallback(char* nick, int playernb)
#endif
{
   char new_msg[128];
   new_msg[0] = '\0';
   snprintf(new_msg, sizeof(new_msg), "Dropped: %s (Player %i)", nick, playernb);
   runloop_msg_queue_push(new_msg, 0, 5 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
}



#if defined(N02_WIN32) || defined(N02_LINUX)

/* Open Kaillera n02 Implementation */
#ifdef N02_WIN32
static HMODULE n02dll;
static HANDLE nHandle;

typedef int (N02CCNV* n02ResetInterfaceT)(void*, int);
n02ResetInterfaceT n02ResetInterfaceF = 0;
#endif

n02ClientInterface client;
n02ClientInfoInterface clientInfo;



static void N02CCNV nGameEnd() //called from client
{
   if (kailleraInitialisedInternal) {
      kailleraWaitSaveLoad = 0;
      kailleraNetplay = false;
      kailleraInitialisedInternal = 0;
      stop_execute_shit = 0;
      command_event(CMD_EVENT_CLOSE_CONTENT, NULL);
   }
}

static void CloseKaillera() {
   if (kailleraInitialised) {
      kailleraInitialised = false;
#ifndef N02_LINUX
      n02ResetInterfaceF = 0;
      CloseHandle(nHandle);
   }
   if (n02dll)
      FreeLibrary(n02dll);
   n02dll = NULL;
#else
   }
#endif
}

#ifndef N02_LINUX
static DWORD WINAPI nThread(LPVOID pParam) {
   client.activate(&client);
   CloseKaillera();
   ExitThread(0);
   return 0;
}
#else
static void* nThread(void*) {
   client.activate(&client);
   CloseKaillera();
   pthread_exit(0);
}
#endif

static void InitialiseN02() {
#ifndef N02_LINUX
   n02ResetInterfaceF(&client, INTERFACE_CLIENT);
   n02ResetInterfaceF(&clientInfo, INTERFACE_CLIENTINFO);
#else
   n02ResetInterface(&client, INTERFACE_CLIENT);
   n02ResetInterface(&clientInfo, INTERFACE_CLIENTINFO);
#endif
   strcpy(client.app.name, "RetroArch " PACKAGE_VERSION);
   client.games.clear();
   client.gameplay.callbackGameLoad = &nGameLoad;
   client.gameplay.callbackChatReceived = &nChatReceived;
   client.gameplay.callbackPlayerDropped = &nPlayerDropped;
   client.gameplay.callbackGameEnd = nGameEnd;

   AddGamesToList();
#ifndef N02_LINUX
   nHandle = CreateThread(NULL, 0, nThread, NULL, 0, 0);
#else
   pthread_create(&threadk, NULL, nThread, NULL);
#endif
   kailleraInitialised = true;
}

void LoadKaillera() {
   if (kailleraInitialised)
      return;
#ifdef N02_LINUX
   InitialiseN02();
#else
   n02dll = LoadLibrary("n02.dll");
   if (n02dll != NULL) {
#ifdef _WIN64
      n02ResetInterfaceF = (n02ResetInterfaceT)GetProcAddress(n02dll, "n02ResetInterface");
#else
      n02ResetInterfaceF = (n02ResetInterfaceT)GetProcAddress(n02dll, "_n02ResetInterface@8");
#endif
      if (n02ResetInterfaceF != 0) {
         InitialiseN02();
         return;
      }

      CloseKaillera();
   }

   runloop_msg_queue_push("Failed to load library n02.dll", 0, 5 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);
#endif
}

#else

/* Original Kaillera Implementation */
#include "kailleraclient.h"

static HINSTANCE kailleraDLL;
static HANDLE KailleraHandle;


int (WINAPI* kailleraGetVersionF)(char* version); 
int (WINAPI* kailleraSetInfosF)(kailleraInfos* infos);
int (WINAPI* kailleraSelectServerDialogF)(HWND parent);
int (WINAPI* kailleraModifyPlayValuesF)(void* values, int size);
int (WINAPI* kailleraChatSendF)(char* text);
int (WINAPI* kailleraInitF)();
int (WINAPI* kailleraShutdownF)();
int (WINAPI* kailleraEndGameF)();
/* Optional - not part of the standard Kaillera client API, so only real/newer
   n02 kailleraclient.dll builds export it. Left NULL (and kailleraPlaybackMode
   left false) for any DLL that doesn't. */
int (WINAPI* kailleraIsPlaybackModeF)();

/* retry-connect - optional, same GetProcAddress-if-present convention as
   kailleraIsPlaybackModeF above. See kaillera.h. */
int (WINAPI* kailleraRetryConnectCanControlF)();
void (WINAPI* kailleraRetryConnectNotifyLocalControlF)(int action, int frame_index);
int (WINAPI* kailleraRetryConnectPollF)(int* outAction, int* outFrameIndex);
int (WINAPI* kailleraRetryConnectActiveF)();
void (WINAPI* kailleraRetryConnectUploadStateF)(const void* data, int size);
int (WINAPI* kailleraRetryConnectDownloadStateF)(void* outBuffer, int bufferCap, int* outFrameIndex);

/* Host-only local rewind support (toolbar "Rebobinar" button) - optional,
   same convention. See kaillera.h. */
int (WINAPI* kailleraRetryConnectGetFrameIndexF)();
int (WINAPI* kailleraRetryConnectGetTotalFramesF)();
void (WINAPI* kailleraRetryConnectSeekLocalF)(int frame_index);

/* Playback checkpoint rewind - optional, same convention. See kaillera.h's
   kailleraPlaybackRewindTick(). */
int (WINAPI* kailleraPlaybackGetFrameIndexF)();
void (WINAPI* kailleraPlaybackSeekToFrameF)(int frame);
int (WINAPI* kailleraPlaybackGetTotalFramesF)();
void (WINAPI* kailleraPlaybackStopF)();

/* "Ir direto para o Ao Vivo!" (Watch Live toolbar) - optional, same
   convention. Spectator-side (kailleraWatch*) and host-side
   (kailleraStream*) - see kaillera.h. */
int (WINAPI* kailleraWatchRequestStateF)();
int (WINAPI* kailleraWatchStateReadyF)();
int (WINAPI* kailleraWatchDownloadStateF)(void* outBuffer, int bufferCap, int* outFrameIndex, int* outByteOffset);
void (WINAPI* kailleraWatchJumpToLiveF)(int frameIndex, int byteOffset);
int (WINAPI* kailleraStreamCheckStateRequestedF)();
void (WINAPI* kailleraStreamUploadStateF)(int frameIndex, const void* data, int size);



void CloseKaillera() {
   if (kailleraInitialised) {
      kailleraShutdownF(); //in n02 this callback do nothing
      kailleraInitialised = false;
      kailleraPlaybackMode = false;
      CloseHandle(KailleraHandle);
   }

   /* Per-frame ticks in core_run() call through these pointers whenever
      kailleraNetplay is set - clear the session state and every optional
      export before the DLL is unloaded, or the next plain content load
      calls into freed memory. */
   kailleraNetplay             = false;
   kailleraPlaybackMode        = false;
   kailleraInitialisedInternal = 0;
   kailleraWaitSaveLoad        = 0;
   stop_execute_shit           = 0;

   kailleraIsPlaybackModeF                 = NULL;
   kailleraRetryConnectCanControlF         = NULL;
   kailleraRetryConnectNotifyLocalControlF = NULL;
   kailleraRetryConnectPollF               = NULL;
   kailleraRetryConnectActiveF             = NULL;
   kailleraRetryConnectUploadStateF        = NULL;
   kailleraRetryConnectDownloadStateF      = NULL;
   kailleraRetryConnectGetFrameIndexF      = NULL;
   kailleraRetryConnectGetTotalFramesF     = NULL;
   kailleraRetryConnectSeekLocalF          = NULL;
   kailleraPlaybackGetFrameIndexF          = NULL;
   kailleraPlaybackSeekToFrameF            = NULL;
   kailleraPlaybackGetTotalFramesF         = NULL;
   kailleraPlaybackStopF                   = NULL;
   kailleraWatchRequestStateF              = NULL;
   kailleraWatchStateReadyF                = NULL;
   kailleraWatchDownloadStateF             = NULL;
   kailleraWatchJumpToLiveF                = NULL;
   kailleraStreamCheckStateRequestedF      = NULL;
   kailleraStreamUploadStateF              = NULL;

   if (kailleraDLL)
      FreeLibrary(kailleraDLL);
   kailleraDLL = NULL;
}

static DWORD WINAPI kailleraThread(LPVOID pParam) {
   kailleraSelectServerDialogF(NULL);
   CloseKaillera();
   ExitThread(0);
   return 0;
}

//static void WINAPI kailleraMoreInfosCallback(char* gamename) {}

// Appends a new entry to kailleraRomNames/filePathK/corePaths at runtime
// (AddGamesToList() itself only ever runs once, from the user's content
// history) - used by kailleraFindOrBrowseGame() below so nGameLoad() can
// resolve a manually-located ROM completely normally, the same as any
// pre-existing history entry, once the room's match actually starts.
// Recomputes the current end-of-blob write position by walking the
// existing double-null-terminated entries from the start rather than
// trusting kailleraGames' exact resting position (AddGamesToList() leaves
// it one byte past the natural next-entry slot - see its own trailing
// "*++kailleraGames = '\0';") - this runs at most once per failed room
// join, so the O(totalGames) walk costs nothing.
static bool AppendGameEntry(const char* displayName, const char* filePath, const char* corePath) {
   char* p;
   size_t remaining, needed;
   int i;

   if (totalGames >= MAX_GAMES)
      return false;

   p = kailleraRomNames;
   for (i = 0; i < totalGames; i++)
      p += strlen(p) + 1;

   remaining = sizeof(kailleraRomNames) - (size_t)(p - kailleraRomNames);
   needed = strlen(displayName) + 2; /* the entry's own NUL + the final double-NUL byte */
   if (needed > remaining)
      return false; /* blob genuinely full - not expected in practice */

   strlcpy(p, displayName, remaining);
   p += strlen(displayName) + 1;
   *p = '\0'; /* new end-of-list marker */
   kailleraGames = p;

   strlcpy(filePathK[totalGames], filePath, sizeof(filePathK[totalGames]));
   strlcpy(corePaths[totalGames], corePath, sizeof(corePaths[totalGames]));
   totalGames++;
   return true;
}

// Native "pick a file" dialog - the same one Ctrl+O itself normally opens
// (see gfx/common/win32_common.c's ID_M_LOAD_CONTENT handler), which is
// otherwise unconditionally disabled for the whole duration of a Kaillera
// session ("if (kailleraInitialised) break;", same file). Pre-filters by
// the wanted file's own extension so the picker starts useful. Returns
// false if the user cancelled.
static bool BrowseForGameFile(const char* wantedFilename, char* outPath, size_t outPathCap) {
   OPENFILENAMEA ofn;
   char fileBuf[1024];
   char filter[300];
   const char* ext = strrchr(wantedFilename, '.');
   int pos = 0;

   ZeroMemory(fileBuf, sizeof(fileBuf));
   ZeroMemory(&ofn, sizeof(ofn));

   if (ext != NULL) {
      pos += _snprintf(filter + pos, sizeof(filter) - pos, "Arquivo esperado (*%s)", ext); filter[pos++] = 0;
      pos += _snprintf(filter + pos, sizeof(filter) - pos, "*%s", ext); filter[pos++] = 0;
   }
   pos += _snprintf(filter + pos, sizeof(filter) - pos, "Todos os arquivos (*.*)"); filter[pos++] = 0;
   pos += _snprintf(filter + pos, sizeof(filter) - pos, "*.*"); filter[pos++] = 0;
   filter[pos++] = 0; /* final double-NUL terminator OPENFILENAME's lpstrFilter requires */

   ofn.lStructSize = sizeof(ofn);
   ofn.hwndOwner = win32_get_window();
   ofn.lpstrFilter = filter;
   ofn.lpstrFile = fileBuf;
   ofn.nMaxFile = sizeof(fileBuf);
   ofn.lpstrTitle = "Selecione o arquivo do jogo";
   ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

   if (!GetOpenFileNameA(&ofn))
      return false;

   strlcpy(outPath, fileBuf, outPathCap);
   return true;
}

// Called (via the new kInfos.findOrBrowseGameCallback) when the DLL's own
// plain-string match against kailleraRomNames failed for a room the user
// is trying to join - kaillera_ui.cpp's kailelra_sdlg_join_selected_game()
// would otherwise immediately show "The rom '...' is not in your list."
// Tries, in order:
//   1. Search every folder already referenced by a content-history entry
//      for a file with the exact wanted filename (same folder as some ROM
//      the user has played before - the common case for anyone with more
//      than one ROM per folder). Naturally a no-op when the history is
//      empty (the loop just doesn't run), falling straight through to (2).
//   2. Fall back to the native file-pick dialog above, paired with
//      whichever core is currently loaded (this only ever runs from
//      inside an already-active Kaillera session, which requires a core
//      to already be running - the overwhelmingly common case is picking
//      a different ROM for that SAME system/core).
// Either way, on success the match is registered into kailleraRomNames/
// filePathK/corePaths under the EXACT wanted display name (AppendGameEntry()
// above), so nGameLoad() resolves it completely normally - exactly like
// any pre-existing history entry - once the match actually starts; nothing
// about that existing load pipeline needs to change.
// wantedGame: the exact "<core>: <filename>" string the room expects (same
// format AddGamesToList() produces, since that's what's being compared
// against). Returns 1 if a file was found/picked (join may proceed), 0 if
// the user cancelled the picker (caller should show its own "not in your
// list" error).
static int WINAPI kailleraFindOrBrowseGame(char* wantedGame) {
   const char* sep = strstr(wantedGame, ": ");
   const char* wantedFilename = sep ? sep + 2 : wantedGame;
   char candidate[1024];
   char corePath[512];
   bool found = false;
   int i;

   for (i = 0; i < totalGames && !found; i++) {
      if (!path_is_valid(filePathK[i]))
         continue;
      fill_pathname_basedir(candidate, filePathK[i], sizeof(candidate));
      strlcat(candidate, wantedFilename, sizeof(candidate));
      if (path_is_valid(candidate)) {
         strlcpy(corePath, corePaths[i], sizeof(corePath));
         found = true;
      }
   }

   if (!found) {
      const char* currentCore = path_get(RARCH_PATH_CORE);
      if (!BrowseForGameFile(wantedFilename, candidate, sizeof(candidate)))
         return 0; /* user cancelled - caller shows its own error */
      strlcpy(corePath, currentCore ? currentCore : "", sizeof(corePath));
   }

   AppendGameEntry(wantedGame, candidate, corePath);
   return 1;
}

static int InitialiseKaillera() {
   kailleraInfos kInfos;

   kInfos.appName = "RetroArch " PACKAGE_VERSION;
   kInfos.gameList = kailleraRomNames;
   kInfos.gameCallback = kailleraGameCallback;
   kInfos.chatReceivedCallback = kailleraChatReceivedCallback;
   kInfos.clientDroppedCallback = kailleraClientDroppedCallback;
   kInfos.moreInfosCallback = NULL; //kailleraMoreInfosCallback; //not used (support only supraclient)
   kInfos.findOrBrowseGameCallback = kailleraFindOrBrowseGame;
   kailleraInitF();
   kailleraSetInfosF(&kInfos);
   KailleraHandle = CreateThread(NULL, 0, kailleraThread, NULL, 0, 0);

   kailleraInitialised = true;
   return 0;
}

void LoadKaillera() {
   if (kailleraInitialised) return;

   kailleraDLL = LoadLibrary("kailleraclient.dll");

   if (kailleraDLL != NULL)
   {

#ifdef _WIN64
      kailleraGetVersionF = (int (WINAPI*)(char* version)) GetProcAddress(kailleraDLL, "kailleraGetVersion");
      kailleraInitF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "kailleraInit");
      kailleraShutdownF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "kailleraShutdown");
      kailleraSetInfosF = (int (WINAPI*)(kailleraInfos * infos)) GetProcAddress(kailleraDLL, "kailleraSetInfos");
      kailleraSelectServerDialogF = (int (WINAPI*)(HWND parent)) GetProcAddress(kailleraDLL, "kailleraSelectServerDialog");
      kailleraModifyPlayValuesF = (int (WINAPI*)(void* values, int size)) GetProcAddress(kailleraDLL, "kailleraModifyPlayValues");
      kailleraChatSendF = (int (WINAPI*)(char* text)) GetProcAddress(kailleraDLL, "kailleraChatSend");
      kailleraEndGameF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "kailleraEndGame");
      kailleraIsPlaybackModeF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "kailleraIsPlaybackMode");
      kailleraRetryConnectCanControlF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "kailleraRetryConnectCanControl");
      kailleraRetryConnectNotifyLocalControlF = (void (WINAPI*)(int, int)) GetProcAddress(kailleraDLL, "kailleraRetryConnectNotifyLocalControl");
      kailleraRetryConnectPollF = (int (WINAPI*)(int*, int*)) GetProcAddress(kailleraDLL, "kailleraRetryConnectPoll");
      kailleraRetryConnectActiveF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "kailleraRetryConnectActive");
      kailleraRetryConnectUploadStateF = (void (WINAPI*)(const void*, int)) GetProcAddress(kailleraDLL, "kailleraRetryConnectUploadState");
      kailleraRetryConnectDownloadStateF = (int (WINAPI*)(void*, int, int*)) GetProcAddress(kailleraDLL, "kailleraRetryConnectDownloadState");
      kailleraRetryConnectGetFrameIndexF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "kailleraRetryConnectGetFrameIndex");
      kailleraRetryConnectGetTotalFramesF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "kailleraRetryConnectGetTotalFrames");
      kailleraRetryConnectSeekLocalF = (void (WINAPI*)(int)) GetProcAddress(kailleraDLL, "kailleraRetryConnectSeekLocal");
      kailleraPlaybackGetFrameIndexF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "kailleraPlaybackGetFrameIndex");
      kailleraPlaybackSeekToFrameF = (void (WINAPI*)(int)) GetProcAddress(kailleraDLL, "kailleraPlaybackSeekToFrame");
      kailleraPlaybackGetTotalFramesF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "kailleraPlaybackGetTotalFrames");
      kailleraPlaybackStopF = (void (WINAPI*)()) GetProcAddress(kailleraDLL, "kailleraPlaybackStop");
      kailleraWatchRequestStateF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "kailleraWatchRequestState");
      kailleraWatchStateReadyF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "kailleraWatchStateReady");
      kailleraWatchDownloadStateF = (int (WINAPI*)(void*, int, int*, int*)) GetProcAddress(kailleraDLL, "kailleraWatchDownloadState");
      kailleraWatchJumpToLiveF = (void (WINAPI*)(int, int)) GetProcAddress(kailleraDLL, "kailleraWatchJumpToLive");
      kailleraStreamCheckStateRequestedF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "kailleraStreamCheckStateRequested");
      kailleraStreamUploadStateF = (void (WINAPI*)(int, const void*, int)) GetProcAddress(kailleraDLL, "kailleraStreamUploadState");
#else
      kailleraGetVersionF = (int (WINAPI*)(char* version)) GetProcAddress(kailleraDLL, "_kailleraGetVersion@4");
      kailleraInitF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "_kailleraInit@0");
      kailleraShutdownF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "_kailleraShutdown@0");
      kailleraSetInfosF = (int (WINAPI*)(kailleraInfos * infos)) GetProcAddress(kailleraDLL, "_kailleraSetInfos@4");
      kailleraSelectServerDialogF = (int (WINAPI*)(HWND parent)) GetProcAddress(kailleraDLL, "_kailleraSelectServerDialog@4");
      kailleraModifyPlayValuesF = (int (WINAPI*)(void* values, int size)) GetProcAddress(kailleraDLL, "_kailleraModifyPlayValues@8");
      kailleraChatSendF = (int (WINAPI*)(char* text)) GetProcAddress(kailleraDLL, "_kailleraChatSend@4");
      kailleraEndGameF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "_kailleraEndGame@0");
      kailleraIsPlaybackModeF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "_kailleraIsPlaybackMode@0");
      kailleraRetryConnectCanControlF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "_kailleraRetryConnectCanControl@0");
      kailleraRetryConnectNotifyLocalControlF = (void (WINAPI*)(int, int)) GetProcAddress(kailleraDLL, "_kailleraRetryConnectNotifyLocalControl@8");
      kailleraRetryConnectPollF = (int (WINAPI*)(int*, int*)) GetProcAddress(kailleraDLL, "_kailleraRetryConnectPoll@8");
      kailleraRetryConnectActiveF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "_kailleraRetryConnectActive@0");
      kailleraRetryConnectUploadStateF = (void (WINAPI*)(const void*, int)) GetProcAddress(kailleraDLL, "_kailleraRetryConnectUploadState@8");
      kailleraRetryConnectDownloadStateF = (int (WINAPI*)(void*, int, int*)) GetProcAddress(kailleraDLL, "_kailleraRetryConnectDownloadState@12");
      kailleraRetryConnectGetFrameIndexF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "_kailleraRetryConnectGetFrameIndex@0");
      kailleraRetryConnectGetTotalFramesF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "_kailleraRetryConnectGetTotalFrames@0");
      kailleraRetryConnectSeekLocalF = (void (WINAPI*)(int)) GetProcAddress(kailleraDLL, "_kailleraRetryConnectSeekLocal@4");
      kailleraPlaybackGetFrameIndexF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "_kailleraPlaybackGetFrameIndex@0");
      kailleraPlaybackSeekToFrameF = (void (WINAPI*)(int)) GetProcAddress(kailleraDLL, "_kailleraPlaybackSeekToFrame@4");
      kailleraPlaybackGetTotalFramesF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "_kailleraPlaybackGetTotalFrames@0");
      kailleraPlaybackStopF = (void (WINAPI*)()) GetProcAddress(kailleraDLL, "_kailleraPlaybackStop@0");
      kailleraWatchRequestStateF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "_kailleraWatchRequestState@0");
      kailleraWatchStateReadyF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "_kailleraWatchStateReady@0");
      kailleraWatchDownloadStateF = (int (WINAPI*)(void*, int, int*, int*)) GetProcAddress(kailleraDLL, "_kailleraWatchDownloadState@16");
      kailleraWatchJumpToLiveF = (void (WINAPI*)(int, int)) GetProcAddress(kailleraDLL, "_kailleraWatchJumpToLive@8");
      kailleraStreamCheckStateRequestedF = (int (WINAPI*)()) GetProcAddress(kailleraDLL, "_kailleraStreamCheckStateRequested@0");
      kailleraStreamUploadStateF = (void (WINAPI*)(int, const void*, int)) GetProcAddress(kailleraDLL, "_kailleraStreamUploadState@12");
#endif

      if (kailleraGetVersionF != NULL &&
         kailleraInitF != NULL &&
         kailleraShutdownF != NULL &&
         kailleraSetInfosF != NULL &&
         kailleraSelectServerDialogF != NULL &&
         kailleraModifyPlayValuesF != NULL &&
         kailleraChatSendF != NULL &&
         kailleraEndGameF != NULL) {

         AddGamesToList();

         InitialiseKaillera();

         return;
      }

      CloseKaillera();
   }

   runloop_msg_queue_push("Failed to load library kailleraclient.dll", 0, 5 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);
}

#endif

int kailleraSyncData(void* value, const int len) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   return client.gameplay.syncData(value, len);
#else
   return kailleraModifyPlayValuesF(value, len);
#endif
}

/* retry-connect targets the classic kailleraclient.dll API only for now (see
   kaillera-client's kcore/kaillera_retryconnect.h) - no-ops on the n02/Open
   Kaillera path, which has its own (unused here) extension points. */
bool kailleraRetryConnectCanControl() {
#if defined(N02_WIN32) || defined(N02_LINUX)
   return false;
#else
   return (kailleraRetryConnectCanControlF != NULL) && (kailleraRetryConnectCanControlF() != 0);
#endif
}

void kailleraRetryConnectNotify(int action) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   (void)action;
#else
   if (kailleraRetryConnectNotifyLocalControlF != NULL)
      kailleraRetryConnectNotifyLocalControlF(action, (int)current_core_frame);
#endif
}

void kailleraRetryConnectRefreshPlaybackMode() {
#if !defined(N02_WIN32) && !defined(N02_LINUX)
   kailleraPlaybackMode = (kailleraIsPlaybackModeF != NULL) && (kailleraIsPlaybackModeF() != 0);
#endif
}

bool kailleraRetryConnectActive() {
#if defined(N02_WIN32) || defined(N02_LINUX)
   return false;
#else
   return (kailleraRetryConnectActiveF != NULL) && (kailleraRetryConnectActiveF() != 0);
#endif
}

void kailleraRetryConnectUploadState(const void* data, int size) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   (void)data; (void)size;
#else
   if (kailleraRetryConnectUploadStateF != NULL)
      kailleraRetryConnectUploadStateF(data, size);
#endif
}

int kailleraRetryConnectDownloadState(void* outBuffer, int bufferCap, int* outFrameIndex) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   (void)outBuffer; (void)bufferCap; (void)outFrameIndex;
   return -1;
#else
   if (kailleraRetryConnectDownloadStateF == NULL)
      return -1;
   return kailleraRetryConnectDownloadStateF(outBuffer, bufferCap, outFrameIndex);
#endif
}

/* Host-only local rewind support (toolbar "Rebobinar" button) - see
   kaillera.h and the retry-connect checkpoint ring further below. */
static int kailleraRetryConnectGetFrameIndex(void) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   return -1;
#else
   if (kailleraRetryConnectGetFrameIndexF == NULL)
      return -1;
   return kailleraRetryConnectGetFrameIndexF();
#endif
}

static int kailleraRetryConnectGetTotalFrames(void) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   return -1;
#else
   if (kailleraRetryConnectGetTotalFramesF == NULL)
      return -1;
   return kailleraRetryConnectGetTotalFramesF();
#endif
}

static void kailleraRetryConnectSeekLocal(int frame_index) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   (void)frame_index;
#else
   if (kailleraRetryConnectSeekLocalF != NULL)
      kailleraRetryConnectSeekLocalF(frame_index);
#endif
}

static int kailleraPlaybackGetFrameIndex(void) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   return -1;
#else
   if (kailleraPlaybackGetFrameIndexF == NULL)
      return -1;
   return kailleraPlaybackGetFrameIndexF();
#endif
}

static void kailleraPlaybackSeekToFrame(int frame) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   (void)frame;
#else
   if (kailleraPlaybackSeekToFrameF != NULL)
      kailleraPlaybackSeekToFrameF(frame);
#endif
}

static int kailleraPlaybackGetTotalFrames(void) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   return -1;
#else
   if (kailleraPlaybackGetTotalFramesF == NULL)
      return -1;
   return kailleraPlaybackGetTotalFramesF();
#endif
}

// Same action as the Player dialog's own "Stop" button.
static void kailleraPlaybackStop(void) {
#if defined(N02_WIN32) || defined(N02_LINUX)
#else
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   if (runloop_st->flags & RUNLOOP_FLAG_PAUSED)
      command_event(CMD_EVENT_UNPAUSE, NULL); /* unpause first - stopping while paused left things in a bad state */
   if (kailleraPlaybackStopF != NULL)
      kailleraPlaybackStopF();
#endif
}

/* "Ir direto para o Ao Vivo!" (Watch Live toolbar) - spectator-side wrappers.
   See kaillera.h's own doc comments and PB_BTN_GOLIVE below for how these
   three get used together. */
static int kailleraWatchRequestState(void) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   return 0;
#else
   return (kailleraWatchRequestStateF != NULL) && (kailleraWatchRequestStateF() != 0);
#endif
}
static int kailleraWatchStateReady(void) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   return 0;
#else
   return (kailleraWatchStateReadyF != NULL) && (kailleraWatchStateReadyF() != 0);
#endif
}
static int kailleraWatchDownloadState(void* outBuffer, int bufferCap, int* outFrameIndex, int* outByteOffset) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   (void)outBuffer; (void)bufferCap; (void)outFrameIndex; (void)outByteOffset;
   return -1;
#else
   if (kailleraWatchDownloadStateF == NULL)
      return -1;
   return kailleraWatchDownloadStateF(outBuffer, bufferCap, outFrameIndex, outByteOffset);
#endif
}
static void kailleraWatchJumpToLive(int frameIndex, int byteOffset) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   (void)frameIndex; (void)byteOffset;
#else
   if (kailleraWatchJumpToLiveF != NULL)
      kailleraWatchJumpToLiveF(frameIndex, byteOffset);
#endif
}

/* "Ir direto para o Ao Vivo!" - host-side half, called from
   kailleraRetryConnectFrameTick()'s call site in runloop.c every
   kailleraNetplay frame (cheap/no-op unless actually streaming - see
   n02_stream_check_state_requested()'s own self-rate-limiting on the
   kaillera-client side). Captures a state and uploads it the moment a
   spectator's request is seen - no pause needed, this host is playing live,
   not idle (unlike retry-connect's kailleraRetryConnectCaptureAndSendState(),
   which this otherwise mirrors closely). */
void kailleraWatchServiceStateRequest(void) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   /* no-op - see kailleraRetryConnectCanControl() above */
#else
   size_t state_size;
   void  *state_buf;

   if (kailleraStreamCheckStateRequestedF == NULL || kailleraStreamCheckStateRequestedF() == 0)
      return;

   state_size = core_serialize_size();
   state_buf  = state_size ? malloc(state_size) : NULL;
   if (state_buf == NULL)
      return;

   {
      retro_ctx_serialize_info_t info;
      info.data       = state_buf;
      info.data_const = NULL;
      info.size       = state_size;
      if (core_serialize(&info) && kailleraStreamUploadStateF != NULL) {
         /* current_core_frame (kaillera.h) is already ticking for any
            kailleraNetplay session (runloop.c's core_run()), not just
            retry-connect - and the value only needs to be locally
            meaningful on the receiving end anyway (see
            n02_stream_upload_state()'s own doc comment). */
         kailleraStreamUploadStateF((int)current_core_frame, state_buf, (int)state_size);
      }
   }
   free(state_buf);
#endif
}

/* Checkpoint-based rewind for solo "Reproducao de Replay" (static local-file
   Playback only - see kaillera.h's kailleraPlaybackRewindTick()). Deliberately
   NOT RetroArch's own built-in rewind (state_manager_check_rewind(), the
   RARCH_REWIND hotkey) - that needs an unbroken per-frame delta history kept
   continuously from the moment rewind is armed, which doesn't fit well with
   content fed by a kaillera-style pseudo core_run() and would cost memory
   the whole time playback runs, for a feature only used occasionally. This
   is coarser but far cheaper: a full core_serialize() snapshot every
   PLAYBACK_CHECKPOINT_INTERVAL_FRAMES of actual replay content (using the
   .krec reader's own frame count via kailleraPlaybackGetFrameIndex() above,
   not wall-clock time, so spacing stays consistent whatever speed the user
   is fast-forwarding at), keeping the last PLAYBACK_CHECKPOINT_COUNT of them
   in a ring buffer. Left steps back through them one at a time - each press
   loads the next-older checkpoint and re-anchors the .krec reader to its
   exact frame (kailleraPlaybackSeekToFrame()), without touching pause state
   either way - playback just keeps going (or stays paused, if that's how
   the user is scrubbing through checkpoints one at a time). */
#define PLAYBACK_CHECKPOINT_INTERVAL_FRAMES 600 /* ~10s at 60fps */
#define PLAYBACK_CHECKPOINT_COUNT 10
/* How long a rewind "gesture" stays open after the last Left press - see
   kailleraPlaybackRewindTick(). Repeated presses inside this window keep
   stacking (go back further each time); once it elapses with no further
   press, playback is treated as having moved on for good, and the next
   Left press starts counting from zero again instead of stacking onto
   however many steps were taken before. */
#define PLAYBACK_REWIND_GESTURE_MS 1000

typedef struct {
   void  *data;
   size_t size;
   int    frame_index;
} PlaybackCheckpoint;

static PlaybackCheckpoint s_pb_checkpoints[PLAYBACK_CHECKPOINT_COUNT];
static int s_pb_checkpoint_count = 0;      /* how many ring slots are actually filled (grows to PLAYBACK_CHECKPOINT_COUNT, then stays there) */
static int s_pb_checkpoint_next = 0;       /* ring write cursor - where the NEXT checkpoint will be written */
static int s_pb_rewind_steps = 0;          /* 0 = at the live playback edge; N = loaded the checkpoint N steps before it */
static int s_pb_last_checkpoint_frame = -1; /* frame_index() as of the last checkpoint (or session start) - -1 = no static playback session tracked yet */
static DWORD s_pb_rewind_gesture_until = 0; /* 0 = no rewind gesture in progress; else the GetTickCount() deadline it expires at */
static bool s_pb_virtual_rewind_request = false; /* one-shot flag set by the on-screen toolbar's Rewind button - see kailleraPlaybackRewindTick() */

// Called by the on-screen playback toolbar's Rewind button (mouse click) -
// consumed exactly like a real Left keypress by kailleraPlaybackRewindTick()
// below, just via a flag instead of GetAsyncKeyState().
static void kailleraPlaybackRequestRewind(void) {
   s_pb_virtual_rewind_request = true;
}

static void PlaybackRewindReset(void) {
   int i;
   for (i = 0; i < PLAYBACK_CHECKPOINT_COUNT; i++) {
      free(s_pb_checkpoints[i].data);
      s_pb_checkpoints[i].data = NULL;
      s_pb_checkpoints[i].size = 0;
   }
   s_pb_checkpoint_count = 0;
   s_pb_checkpoint_next = 0;
   s_pb_rewind_steps = 0;
   s_pb_last_checkpoint_frame = -1;
   s_pb_rewind_gesture_until = 0;
}

// `force` bypasses the normal PLAYBACK_CHECKPOINT_INTERVAL_FRAMES spacing -
// used once a rewind gesture (see above) has settled, to capture a fresh
// checkpoint right where playback ended up instead of waiting out however
// much of the normal ~10s interval happens to be left.
static void PlaybackRewindMaybeCapture(int frame_index, bool force) {
   size_t state_size;
   void *state_buf;
   retro_ctx_serialize_info_t info;
   PlaybackCheckpoint *slot;

   if (s_pb_last_checkpoint_frame < 0) {
      s_pb_last_checkpoint_frame = frame_index; /* first frame seen this session - anchor from here, nothing to capture yet */
      return;
   }
   if (!force && frame_index - s_pb_last_checkpoint_frame < PLAYBACK_CHECKPOINT_INTERVAL_FRAMES)
      return;

   if (force && s_pb_rewind_steps > 0) {
      // We're branching forward from a rewound point (s_pb_rewind_steps
      // checkpoints back from the write cursor) - anything strictly newer
      // than that point (the s_pb_rewind_steps-1 entries between it and the
      // write cursor) describes a future that no longer happened once
      // playback continued differently from here. Drop them and rewind the
      // write cursor to right after our current spot, so what gets written
      // below becomes the new "most recent" entry instead of landing
      // wherever forward-only progress had last reached - otherwise a step
      // further back can land on something NEWER than a step closer in,
      // since the ring would still hold that stale, now-invalid future.
      int discard = s_pb_rewind_steps - 1;
      s_pb_checkpoint_next = (s_pb_checkpoint_next - discard + PLAYBACK_CHECKPOINT_COUNT * 2) % PLAYBACK_CHECKPOINT_COUNT;
      s_pb_checkpoint_count -= discard;
   }

   state_size = core_serialize_size();
   if (state_size == 0)
      return;
   state_buf = malloc(state_size);
   if (state_buf == NULL)
      return;
   info.data       = state_buf;
   info.data_const = NULL;
   info.size       = state_size;
   if (!core_serialize(&info)) {
      free(state_buf);
      return;
   }

   slot = &s_pb_checkpoints[s_pb_checkpoint_next];
   free(slot->data);
   slot->data        = state_buf;
   slot->size        = state_size;
   slot->frame_index = frame_index;

   s_pb_checkpoint_next = (s_pb_checkpoint_next + 1) % PLAYBACK_CHECKPOINT_COUNT;
   if (s_pb_checkpoint_count < PLAYBACK_CHECKPOINT_COUNT)
      s_pb_checkpoint_count++;
   s_pb_rewind_steps = 0; /* real forward progress happened - any earlier rewind is now old news */
   s_pb_last_checkpoint_frame = frame_index;
}

/* Checkpoint-based rewind for retry-connect's group replay - host-only
   (only the host navigates; a peer's own view is always overwritten by the
   host's next broadcast anyway, same reasoning as the FF handoff). Mirrors
   the solo-Playback ring above (same interval/count, same core_serialize()
   capture), but kept as its own separate ring rather than sharing
   s_pb_checkpoints - the two modes are mutually exclusive but have
   independent lifetimes (kailleraRetryConnectActive() vs "is static
   Playback"), and the rewind here has to broadcast to every peer afterward
   (kailleraRetryConnectSeekLocal() + kailleraRetryConnectUploadState()) while
   solo Playback's stays purely local - simplest to keep them from ever being
   able to interfere with each other. */
#define RC_CHECKPOINT_INTERVAL_FRAMES 600 /* ~10s at 60fps, same spacing as solo Playback */
#define RC_CHECKPOINT_COUNT 10
static PlaybackCheckpoint s_rc_checkpoints[RC_CHECKPOINT_COUNT];
static int s_rc_checkpoint_count = 0;
static int s_rc_checkpoint_next = 0;
static int s_rc_last_checkpoint_frame = -1;

static void RetryConnectCheckpointReset(void) {
   int i;
   for (i = 0; i < RC_CHECKPOINT_COUNT; i++) {
      free(s_rc_checkpoints[i].data);
      s_rc_checkpoints[i].data = NULL;
      s_rc_checkpoints[i].size = 0;
   }
   s_rc_checkpoint_count = 0;
   s_rc_checkpoint_next = 0;
   s_rc_last_checkpoint_frame = -1;
}

static void RetryConnectCheckpointMaybeCapture(int frame_index) {
   size_t state_size;
   void *state_buf;
   retro_ctx_serialize_info_t info;
   PlaybackCheckpoint *slot;

   if (s_rc_last_checkpoint_frame < 0) {
      s_rc_last_checkpoint_frame = frame_index;
      return;
   }
   if (frame_index - s_rc_last_checkpoint_frame < RC_CHECKPOINT_INTERVAL_FRAMES)
      return;

   state_size = core_serialize_size();
   if (state_size == 0)
      return;
   state_buf = malloc(state_size);
   if (state_buf == NULL)
      return;
   info.data       = state_buf;
   info.data_const = NULL;
   info.size       = state_size;
   if (!core_serialize(&info)) {
      free(state_buf);
      return;
   }

   slot = &s_rc_checkpoints[s_rc_checkpoint_next];
   free(slot->data);
   slot->data        = state_buf;
   slot->size        = state_size;
   slot->frame_index = frame_index;

   s_rc_checkpoint_next = (s_rc_checkpoint_next + 1) % RC_CHECKPOINT_COUNT;
   if (s_rc_checkpoint_count < RC_CHECKPOINT_COUNT)
      s_rc_checkpoint_count++;
   s_rc_last_checkpoint_frame = frame_index;
}

/* Host-only: restores the newest checkpoint strictly before the current
   position, locally (core_unserialize() + kailleraRetryConnectSeekLocal()),
   then re-uses the existing kailleraRetryConnectUploadState() hand-off to
   broadcast it - every peer converges via the SAME already-working
   RC_ACTION_STATE_READY path a normal Pause already uses, no peer-side
   changes needed. No-op if there's nothing earlier to rewind to. */
static void RetryConnectRequestRewind(void) {
   int current_frame, i, best_frame = -1, target = -1;

   if (!kailleraRetryConnectCanControl())
      return;
   current_frame = kailleraRetryConnectGetFrameIndex();
   if (current_frame < 0)
      return;

   for (i = 0; i < s_rc_checkpoint_count; i++) {
      PlaybackCheckpoint *cp = &s_rc_checkpoints[i];
      if (cp->data != NULL && cp->frame_index < current_frame && cp->frame_index > best_frame) {
         best_frame = cp->frame_index;
         target = i;
      }
   }
   if (target < 0) {
      runloop_msg_queue_push("Nao posso voltar mais do que isso ;(", 0, 90, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
      return;
   }

   {
      PlaybackCheckpoint *cp = &s_rc_checkpoints[target];
      retro_ctx_serialize_info_t info;
      info.data_const = cp->data;
      info.data       = NULL;
      info.size       = cp->size;
      if (!core_unserialize(&info))
         return;
      kailleraRetryConnectSeekLocal(cp->frame_index);
      kailleraRetryConnectUploadState(cp->data, (int)cp->size);
      s_rc_last_checkpoint_frame = cp->frame_index; /* periodic capture spacing resumes from here */
   }
}

/* On-screen mouse-clickable control toolbar for solo "Reproducao de Replay" -
   a small always-on-top window (owned by the main RetroArch window, not a
   true child, so it survives the game window resizing/moving and follows
   its z-order) drawn entirely with GDI rather than built from standard
   BUTTON controls, so the "Avancar (segure)" button can implement genuine
   press-and-hold semantics (WM_LBUTTONDOWN starts fast-forward, WM_LBUTTONUP
   or losing mouse capture stops it) with the same code path as the other
   three simple-click buttons, instead of needing to subclass a stock button
   just to see raw mouse events.
   NOTE: sits behind the game view if RetroArch is running in exclusive
   (not windowed/borderless) fullscreen - D3D/GL exclusive fullscreen owns
   the whole display output, and no other top-level window (topmost or not)
   can paint over it. Works normally in windowed or borderless fullscreen. */
#if !defined(N02_WIN32) && !defined(N02_LINUX)

#define PB_TOOLBAR_CLASS "N02PlaybackToolbar"
#define PB_TOOLBAR_BTN_COUNT 6
#define PB_TOOLBAR_BTN_W 52
#define PB_TOOLBAR_BTN_H 42
#define PB_TOOLBAR_PAD 4
#define PB_TOOLBAR_PROGRESS_H 10
#define PB_TOOLBAR_W (PB_TOOLBAR_BTN_W * PB_TOOLBAR_BTN_COUNT + PB_TOOLBAR_PAD * (PB_TOOLBAR_BTN_COUNT + 1))
#define PB_TOOLBAR_H (PB_TOOLBAR_PROGRESS_H + PB_TOOLBAR_PAD + PB_TOOLBAR_BTN_H + PB_TOOLBAR_PAD * 2)
#define PB_TOOLBAR_ALPHA 210 /* out of 255 - "certa transparencia" over the game view */

enum { PB_BTN_REWIND = 0, PB_BTN_PAUSE, PB_BTN_HOLDFF, PB_BTN_TOGGLEFF, PB_BTN_STOP, PB_BTN_GOLIVE };

static HWND s_pb_toolbar = NULL;
static HWND s_pb_tooltip = NULL;
static int  s_pb_toolbar_hover = -1;
static bool s_pb_toolbar_holdff_down = false;

/* "Ir direto para o Ao Vivo!" (PB_BTN_GOLIVE) - Watch Live only (see
   kailleraPlaybackRewindTick()'s is_static_playback check, which now also
   covers Watch Live - true whenever kailleraPlaybackGetTotalFrames() <= 0,
   i.e. no fixed total, unlike static local-file Playback). Starts
   "at the live edge" (disabled); Rewind or pausing the recorded stream
   means we've fallen behind, so re-enable it - see
   PlaybackTogglePause()/kailleraPlaybackRequestRewind()'s call sites below. */
static bool s_watch_behind_live = false;
static bool s_watch_golive_pending = false; /* request sent, waiting on the host */
static DWORD s_watch_golive_last_poll = 0;
static DWORD s_watch_golive_deadline = 0;
#define WATCH_GOLIVE_POLL_INTERVAL_MS 1000
#define WATCH_GOLIVE_TIMEOUT_MS 15000 /* host polls every ~3s (n02_stream.cpp) - well clear of that */

/* Mirrors the FASTMOTION hotkey block's own on/off transitions exactly
   (runloop.c, "Check fastmotion hotkeys") - shared by both FF toolbar
   buttons below instead of duplicating that logic per-button. */
static void PlaybackSetFastForward(bool on) {
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   input_driver_state_t *input_st = input_state_get_ptr();
   video_driver_state_t *video_st = video_state_get_ptr();
   settings_t *settings = config_get_ptr();
   bool currently_on = (runloop_st->flags & RUNLOOP_FLAG_FASTMOTION) ? true : false;

   if (on == currently_on)
      return;

   if (on) {
      input_st->flags |= INP_FLAG_NONBLOCKING;
      runloop_st->flags |= RUNLOOP_FLAG_FASTMOTION;
      command_event(CMD_EVENT_SET_FRAME_LIMIT, NULL);
   } else {
      input_st->flags &= ~INP_FLAG_NONBLOCKING;
      runloop_st->flags &= ~RUNLOOP_FLAG_FASTMOTION;
      runloop_st->fastforward_after_frames = 1;
   }
   driver_set_nonblock_state();
   if (!on && settings->bools.frame_time_counter_reset_after_fastforwarding)
      video_st->frame_time_count = 0;
}

static void PlaybackTogglePause(void) {
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   if (runloop_st->flags & RUNLOOP_FLAG_PAUSED)
      command_event(CMD_EVENT_UNPAUSE, NULL);
   else {
      PlaybackSetFastForward(false); /* wouldn't make sense to still be fast-forwarding once paused */
      command_event(CMD_EVENT_PAUSE, NULL);
   }
}

/* Host-only: same effect as the native Pause hotkey's retry-connect branch
   (runloop.c) - factored out here so the toolbar's Pause button can share it
   without duplicating the stop-FF + pause + broadcast sequence. */
static void RetryConnectTogglePause(void) {
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   if (!kailleraRetryConnectCanControl())
      return;
   if (runloop_st->flags & RUNLOOP_FLAG_PAUSED)
      command_event(CMD_EVENT_UNPAUSE, NULL);
   else {
      PlaybackSetFastForward(false); /* wouldn't make sense to still be fast-forwarding once paused */
      command_event(CMD_EVENT_PAUSE, NULL);
      kailleraRetryConnectCaptureAndSendState();
   }
}

/* Forward declarations - both defined further below, once s_rc_seq exists.
   Let the toolbar (defined here, well before that point in the file) start/
   check the go-live hand-off without moving that whole state machine up
   here. */
static void RetryConnectRequestGoLive(void);
static bool RetryConnectSelectReady(void);

static void PlaybackToolbarButtonRect(int index, RECT *out) {
   out->left = PB_TOOLBAR_PAD + index * (PB_TOOLBAR_BTN_W + PB_TOOLBAR_PAD);
   out->top = PB_TOOLBAR_PROGRESS_H + PB_TOOLBAR_PAD * 2;
   out->right = out->left + PB_TOOLBAR_BTN_W;
   out->bottom = out->top + PB_TOOLBAR_BTN_H;
}

static void PlaybackToolbarProgressRect(RECT *out) {
   out->left = PB_TOOLBAR_PAD;
   out->top = PB_TOOLBAR_PAD;
   out->right = PB_TOOLBAR_W - PB_TOOLBAR_PAD;
   out->bottom = out->top + PB_TOOLBAR_PROGRESS_H;
}

/* Tooltip text for each button - describes the action and matches the
   actual configured keys for this deployment's retroarch.cfg
   (input_pause_toggle=p, input_hold_fast_forward=l, input_toggle_fast_forward=space).
   Static/state-independent text (covers both Pause and Continuar in one
   line) rather than trying to keep a live tooltip in sync with the icon. */
static const char *PlaybackToolbarButtonTooltip(int index) {
   switch (index) {
   case PB_BTN_REWIND:   return "Rebobinar (Seta esquerda)";
   case PB_BTN_PAUSE:    return "Pausar / Continuar (P)";
   case PB_BTN_HOLDFF:   return "Avancar rapido - segure (L)";
   case PB_BTN_TOGGLEFF: return "Alternar velocidade do avanco rapido (Espaco)";
   case PB_BTN_STOP:     return "Parar a reproducao (Esc)";
   /* Shared with retry-connect's "Selecionar" (Enter, host-only, so re-uses
      the same physical toolbar window/slot) - text baked in once at toolbar
      creation (PlaybackToolbarCreateTooltip()), so it has to read sensibly
      in both contexts rather than being refreshed per-mode. */
   case PB_BTN_GOLIVE:   return "Ir ao vivo! / Selecionar (Enter)";
   default: return "";
   }
}

static void PlaybackDrawTriangleRight(HDC dc, int cx, int cy, int size, HBRUSH brush) {
   POINT pts[3];
   HGDIOBJ old_brush, old_pen;
   pts[0].x = cx - size / 2; pts[0].y = cy - size / 2;
   pts[1].x = cx - size / 2; pts[1].y = cy + size / 2;
   pts[2].x = cx + size / 2; pts[2].y = cy;
   old_brush = SelectObject(dc, brush);
   old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
   Polygon(dc, pts, 3);
   SelectObject(dc, old_pen);
   SelectObject(dc, old_brush);
}

static void PlaybackDrawTriangleLeft(HDC dc, int cx, int cy, int size, HBRUSH brush) {
   POINT pts[3];
   HGDIOBJ old_brush, old_pen;
   pts[0].x = cx + size / 2; pts[0].y = cy - size / 2;
   pts[1].x = cx + size / 2; pts[1].y = cy + size / 2;
   pts[2].x = cx - size / 2; pts[2].y = cy;
   old_brush = SelectObject(dc, brush);
   old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
   Polygon(dc, pts, 3);
   SelectObject(dc, old_pen);
   SelectObject(dc, old_brush);
}

static void PlaybackDrawBar(HDC dc, int cx, int cy, int w, int h, HBRUSH brush) {
   RECT r;
   r.left = cx - w / 2; r.right = cx + w / 2;
   r.top = cy - h / 2; r.bottom = cy + h / 2;
   FillRect(dc, &r, brush);
}

/* Watch Live only (see PB_BTN_GOLIVE's own comment above) - solo Playback
   has no live edge to jump to, so the button stays permanently disabled
   there regardless of s_watch_behind_live. */
static bool IsWatchLive(void) {
   return kailleraPlaybackGetFrameIndex() >= 0 && kailleraPlaybackGetTotalFrames() <= 0;
}

static bool GoLiveEnabled(void) {
   return IsWatchLive() && s_watch_behind_live && !s_watch_golive_pending;
}

static void PlaybackToolbarPaint(HWND hwnd) {
   PAINTSTRUCT ps;
   HDC dc;
   HBRUSH bg;
   RECT client;
   int i;
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   bool paused = (runloop_st->flags & RUNLOOP_FLAG_PAUSED) ? true : false;
   bool ff_on  = (runloop_st->flags & RUNLOOP_FLAG_FASTMOTION) ? true : false;
   bool golive_enabled = GoLiveEnabled();
   /* retry-connect re-uses this same toolbar window - see PB_BTN_GOLIVE's
      case below and RetryConnectRequestRewind()/TogglePause()/RequestGoLive()
      above. Only the host navigates (everyone else's view is authoritatively
      overwritten by the host's next broadcast anyway), so a peer sees every
      button greyed out here. */
   bool rc_mode = kailleraRetryConnectActive();
   bool rc_can_control = rc_mode && kailleraRetryConnectCanControl();
   bool rc_select_enabled = rc_can_control && paused && RetryConnectSelectReady();

   dc = BeginPaint(hwnd, &ps);
   bg = CreateSolidBrush(RGB(24, 24, 24));
   GetClientRect(hwnd, &client);
   FillRect(dc, &client, bg);
   DeleteObject(bg);

   for (i = 0; i < PB_TOOLBAR_BTN_COUNT; i++) {
      RECT r;
      HBRUSH btn_bg, icon_brush;
      bool is_down = (i == PB_BTN_HOLDFF && s_pb_toolbar_holdff_down);
      bool is_disabled;
      int cx, cy;

      if (rc_mode) {
         is_disabled = (i == PB_BTN_STOP) ? true /* no wired action during retry-connect */
                     : (i == PB_BTN_GOLIVE) ? !rc_select_enabled
                     : !rc_can_control;
      } else {
         is_disabled = (i == PB_BTN_GOLIVE && !golive_enabled);
      }

      PlaybackToolbarButtonRect(i, &r);
      btn_bg = CreateSolidBrush(is_down ? RGB(90, 90, 20) : (i == s_pb_toolbar_hover && !is_disabled ? RGB(70, 70, 70) : RGB(50, 50, 50)));
      FillRect(dc, &r, btn_bg);
      DeleteObject(btn_bg);
      FrameRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));

      cx = (r.left + r.right) / 2;
      cy = (r.top + r.bottom) / 2;
      icon_brush = CreateSolidBrush(is_disabled ? RGB(110, 110, 110) : RGB(240, 240, 240));

      switch (i) {
      case PB_BTN_REWIND:
         PlaybackDrawTriangleLeft(dc, cx - 7, cy, 14, icon_brush);
         PlaybackDrawTriangleLeft(dc, cx + 7, cy, 14, icon_brush);
         break;
      case PB_BTN_PAUSE:
         if (paused)
            PlaybackDrawTriangleRight(dc, cx, cy, 16, icon_brush);
         else {
            PlaybackDrawBar(dc, cx - 5, cy, 6, 18, icon_brush);
            PlaybackDrawBar(dc, cx + 5, cy, 6, 18, icon_brush);
         }
         break;
      case PB_BTN_HOLDFF:
         PlaybackDrawTriangleRight(dc, cx - 7, cy, 14, icon_brush);
         PlaybackDrawTriangleRight(dc, cx + 7, cy, 14, icon_brush);
         break;
      case PB_BTN_TOGGLEFF:
         {
            RECT textr = r;
            PlaybackDrawTriangleRight(dc, cx - 7, cy - 6, 12, icon_brush);
            PlaybackDrawTriangleRight(dc, cx + 7, cy - 6, 12, icon_brush);
            textr.top = cy + 3;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(240, 240, 240));
            DrawTextA(dc, ff_on ? "1x" : "10x", -1, &textr, DT_CENTER | DT_TOP | DT_SINGLELINE);
         }
         break;
      case PB_BTN_STOP:
         PlaybackDrawBar(dc, cx, cy, 16, 16, icon_brush);
         break;
      case PB_BTN_GOLIVE:
         /* skip-to-end (bar + arrow) - "waiting on the host" state is
            already conveyed by the disabled greying above, kept simple
            rather than animating anything here. */
         PlaybackDrawTriangleRight(dc, cx - 6, cy, 14, icon_brush);
         PlaybackDrawBar(dc, cx + 7, cy, 4, 18, icon_brush);
         break;
      }
      DeleteObject(icon_brush);
   }

   /* Progress bar - current position within the recording, not wall-clock
      time (fast-forwarding covers ground faster without the bar looking
      "wrong"). Hidden (just the empty track) if the total is unknown. */
   {
      RECT pr;
      HBRUSH track, fill;
      int frame_index = rc_mode ? kailleraRetryConnectGetFrameIndex() : kailleraPlaybackGetFrameIndex();
      int total = rc_mode ? kailleraRetryConnectGetTotalFrames() : kailleraPlaybackGetTotalFrames();

      PlaybackToolbarProgressRect(&pr);
      track = CreateSolidBrush(RGB(60, 60, 60));
      FillRect(dc, &pr, track);
      DeleteObject(track);
      FrameRect(dc, &pr, (HBRUSH)GetStockObject(BLACK_BRUSH));

      if (frame_index >= 0 && total > 0) {
         RECT fr = pr;
         double frac = (double)frame_index / (double)total;
         if (frac < 0.0) frac = 0.0;
         if (frac > 1.0) frac = 1.0;
         fr.right = fr.left + (LONG)((fr.right - fr.left) * frac);
         if (fr.right > fr.left) {
            fill = CreateSolidBrush(RGB(90, 170, 90));
            FillRect(dc, &fr, fill);
            DeleteObject(fill);
         }
      }
   }
   EndPaint(hwnd, &ps);
}

static int PlaybackToolbarHitTest(int x, int y) {
   int i;
   for (i = 0; i < PB_TOOLBAR_BTN_COUNT; i++) {
      RECT r;
      PlaybackToolbarButtonRect(i, &r);
      if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
         return i;
   }
   return -1;
}

static LRESULT CALLBACK PlaybackToolbarWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
   switch (msg) {
   case WM_ERASEBKGND:
      return 1;
   case WM_PAINT:
      PlaybackToolbarPaint(hwnd);
      return 0;
   case WM_LBUTTONDOWN:
      {
         int idx = PlaybackToolbarHitTest((short)LOWORD(lp), (short)HIWORD(lp));
         bool rc_mode = kailleraRetryConnectActive();

         if (rc_mode) {
            /* Only the host navigates - a peer's click on any of these
               (already shown greyed out, PlaybackToolbarPaint()) does
               nothing, same as its keyboard equivalents being ignored. */
            bool rc_can_control = kailleraRetryConnectCanControl();
            if (idx == PB_BTN_REWIND) {
               if (rc_can_control) RetryConnectRequestRewind();
            } else if (idx == PB_BTN_PAUSE) {
               if (rc_can_control) RetryConnectTogglePause();
            } else if (idx == PB_BTN_HOLDFF) {
               if (rc_can_control) {
                  s_pb_toolbar_holdff_down = true;
                  SetCapture(hwnd);
                  PlaybackSetFastForward(true);
               }
            } else if (idx == PB_BTN_TOGGLEFF) {
               if (rc_can_control) {
                  runloop_state_t *runloop_st = runloop_state_get_ptr();
                  PlaybackSetFastForward(!(runloop_st->flags & RUNLOOP_FLAG_FASTMOTION));
               }
            } else if (idx == PB_BTN_GOLIVE) {
               if (rc_can_control) RetryConnectRequestGoLive();
            }
         } else if (idx == PB_BTN_REWIND)
            kailleraPlaybackRequestRewind();
         else if (idx == PB_BTN_PAUSE)
            PlaybackTogglePause();
         else if (idx == PB_BTN_HOLDFF) {
            s_pb_toolbar_holdff_down = true;
            SetCapture(hwnd);
            PlaybackSetFastForward(true);
         } else if (idx == PB_BTN_TOGGLEFF) {
            runloop_state_t *runloop_st = runloop_state_get_ptr();
            PlaybackSetFastForward(!(runloop_st->flags & RUNLOOP_FLAG_FASTMOTION));
         } else if (idx == PB_BTN_STOP) {
            kailleraPlaybackStop();
         } else if (idx == PB_BTN_GOLIVE && GoLiveEnabled()) {
            if (kailleraWatchRequestState()) {
               s_watch_golive_pending = true;
               s_watch_golive_last_poll = GetTickCount();
               s_watch_golive_deadline = GetTickCount() + WATCH_GOLIVE_TIMEOUT_MS;
               if (s_watch_golive_deadline == 0) s_watch_golive_deadline = 1;
            } else {
               runloop_msg_queue_push("Nao foi possivel pedir o sync ao vivo agora - tente de novo em instantes.", 0, 90, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);
            }
         }
         InvalidateRect(hwnd, NULL, FALSE);
      }
      return 0;
   case WM_LBUTTONUP:
      if (s_pb_toolbar_holdff_down) {
         s_pb_toolbar_holdff_down = false;
         ReleaseCapture();
         PlaybackSetFastForward(false);
         InvalidateRect(hwnd, NULL, FALSE);
      }
      return 0;
   case WM_MOUSEMOVE:
      {
         int idx = PlaybackToolbarHitTest((short)LOWORD(lp), (short)HIWORD(lp));
         if (idx != s_pb_toolbar_hover) {
            s_pb_toolbar_hover = idx;
            InvalidateRect(hwnd, NULL, FALSE);
         }
      }
      return 0;
   case WM_CAPTURECHANGED:
      /* Mouse released outside the button (drag-off) - stock button
         controls handle this the same way; DefWindowProc doesn't apply
         here since we own WM_LBUTTONUP ourselves, so mirror it explicitly. */
      if (s_pb_toolbar_holdff_down) {
         s_pb_toolbar_holdff_down = false;
         PlaybackSetFastForward(false);
         InvalidateRect(hwnd, NULL, FALSE);
      }
      return 0;
   case WM_MOUSEACTIVATE:
      /* Never let clicking the toolbar activate it - that would steal
         keyboard focus away from the game window, breaking every hotkey
         (P, L, Space, Left...) until the user clicks back on the game to
         refocus it. WS_EX_NOACTIVATE alone doesn't cover every activation
         path reliably, so this is the belt-and-suspenders half of the
         fix. */
      return MA_NOACTIVATE;
   case WM_DESTROY:
      s_pb_toolbar = NULL;
      return 0;
   }
   return DefWindowProc(hwnd, msg, wp, lp);
}

static void PlaybackToolbarPosition(HWND toolbar) {
   HWND game_hwnd;
   RECT game_rect;
   if (toolbar == NULL)
      return;
   game_hwnd = win32_get_window();
   if (game_hwnd == NULL || !GetWindowRect(game_hwnd, &game_rect))
      return;
   {
      int x = game_rect.left + (game_rect.right - game_rect.left - PB_TOOLBAR_W) / 2;
      int y = game_rect.top + 20;
      SetWindowPos(toolbar, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
   }
}

static void PlaybackToolbarCreateTooltip(HWND toolbar) {
   INITCOMMONCONTROLSEX icx;
   int i;

   ZeroMemory(&icx, sizeof(icx));
   icx.dwSize = sizeof(icx);
   icx.dwICC = ICC_WIN95_CLASSES; /* needed for TOOLTIPS_CLASSA */
   InitCommonControlsEx(&icx);

   s_pb_tooltip = CreateWindowExA(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSA, NULL,
      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
      CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
      toolbar, NULL, GetModuleHandle(NULL), NULL);
   if (s_pb_tooltip == NULL)
      return;

   for (i = 0; i < PB_TOOLBAR_BTN_COUNT; i++) {
      TOOLINFOA ti;
      ZeroMemory(&ti, sizeof(ti));
      ti.cbSize   = sizeof(ti);
      ti.uFlags   = TTF_SUBCLASS; /* rect-based tool - several tools sharing one HWND (the toolbar), not TTF_IDISHWND */
      ti.hwnd     = toolbar;
      ti.hinst    = GetModuleHandle(NULL);
      ti.uId      = (UINT_PTR)i;
      ti.lpszText = (LPSTR)PlaybackToolbarButtonTooltip(i);
      PlaybackToolbarButtonRect(i, &ti.rect);
      SendMessageA(s_pb_tooltip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
   }
}

static void PlaybackToolbarEnsureCreated(void) {
   WNDCLASSA wc;
   HWND game_hwnd;

   if (s_pb_toolbar != NULL)
      return;

   ZeroMemory(&wc, sizeof(wc));
   wc.lpfnWndProc = PlaybackToolbarWndProc;
   wc.hInstance = GetModuleHandle(NULL);
   wc.hCursor = LoadCursor(NULL, IDC_ARROW);
   wc.lpszClassName = PB_TOOLBAR_CLASS;
   RegisterClassA(&wc); /* fine to call again if a prior session already registered it */

   game_hwnd = win32_get_window();
   s_pb_toolbar = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, PB_TOOLBAR_CLASS, "Reproducao de Replay",
      WS_POPUP, 20, 20, PB_TOOLBAR_W, PB_TOOLBAR_H, game_hwnd, NULL, wc.hInstance, NULL);
   if (s_pb_toolbar) {
      SetLayeredWindowAttributes(s_pb_toolbar, 0, PB_TOOLBAR_ALPHA, LWA_ALPHA); /* "com certa transparencia" over the game view */
      PlaybackToolbarPosition(s_pb_toolbar);
      PlaybackToolbarCreateTooltip(s_pb_toolbar);
   }
}

static void PlaybackToolbarSetVisible(bool visible) {
   if (visible) {
      PlaybackToolbarEnsureCreated();
      if (s_pb_toolbar) {
         PlaybackToolbarPosition(s_pb_toolbar);
         ShowWindow(s_pb_toolbar, SW_SHOWNOACTIVATE);
      }
   } else if (s_pb_toolbar) {
      ShowWindow(s_pb_toolbar, SW_HIDE);
   }
}

#endif /* !N02_WIN32 && !N02_LINUX */

void kailleraPlaybackRewindTick(void) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   /* no-op - see kailleraRetryConnectCanControl() above */
#else
   static bool was_static_playback = false;
   static bool old_left_pressed = false;
   static bool old_esc_pressed = false;
   bool esc_pressed;
   int frame_index = kailleraPlaybackGetFrameIndex();
   bool is_static_playback = (frame_index >= 0) && !kailleraRetryConnectActive();
   bool left_pressed;
   DWORD now;

   /* Reset the ring the moment a static-Playback session starts OR ends -
      cheapest way to guarantee a previous file's checkpoints never leak
      into the next one. */
   if (is_static_playback != was_static_playback)
      PlaybackRewindReset();
   was_static_playback = is_static_playback;

   /* "Ir direto para o Ao Vivo!" - a fresh pause (any trigger: the toolbar's
      own Pause button, or the "P" hotkey, which doesn't funnel through
      PlaybackTogglePause() at all) means we've fallen behind the live edge -
      catches both input paths uniformly instead of hooking each one. No-op
      outside Watch Live in practice (see PB_BTN_GOLIVE's enabled-state check
      below, which also gates on kailleraPlaybackGetTotalFrames() <= 0 - solo
      Playback pausing sets this flag too, harmlessly, since the button stays
      disabled there regardless). */
   {
      static bool old_paused_for_golive = false;
      bool now_paused = (runloop_state_get_ptr()->flags & RUNLOOP_FLAG_PAUSED) ? true : false;
      if (is_static_playback && now_paused && !old_paused_for_golive)
         s_watch_behind_live = true;
      old_paused_for_golive = now_paused;
   }
   /* WS_EX_TOPMOST floats the toolbar above EVERY window, not just
      RetroArch's - only actually show it while the game window is the
      foreground one, so alt-tabbing away doesn't leave it plastered over
      whatever else the user switches to. */
   PlaybackToolbarSetVisible(is_static_playback && GetForegroundWindow() == win32_get_window());

   if (!is_static_playback) {
      old_left_pressed = false;
      old_esc_pressed = false;
      s_pb_virtual_rewind_request = false;
      return;
   }

   PlaybackToolbarPosition(s_pb_toolbar); /* cheap - tracks the game window moving/resizing */
   if (s_pb_toolbar)
      InvalidateRect(s_pb_toolbar, NULL, FALSE); /* Pause/FF button labels reflect live state, which can change from the keyboard too */

   /* Esc = Stop, mirroring the toolbar's Stop button - safe to bind
      directly (bypassing the input-remapping system, same as Left/Enter
      elsewhere in this file) because RetroArch's own native quit hotkey is
      already unconditionally disabled for the whole duration of any
      Kaillera session, Playback included (runloop.c's quit-hotkey check:
      "if (kailleraInitialised) trig_quit_key = false;"), so this can't
      race with or accidentally trigger "exit emulator". */
   esc_pressed = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) ? true : false;
   if (esc_pressed && !old_esc_pressed)
      kailleraPlaybackStop();
   old_esc_pressed = esc_pressed;

   now = GetTickCount();

   /* A rewind gesture that's gone quiet (no Left press for
      PLAYBACK_REWIND_GESTURE_MS) means the user has moved on from wherever
      they landed - capture a checkpoint right there (bypassing the normal
      ~10s spacing) and close the gesture, so the NEXT Left press counts
      from zero relative to HERE instead of stacking onto however many steps
      were taken before. Without this, briefly resuming play for only a
      couple of seconds and then rewinding again would jump back much
      further than expected, relative to the ORIGINAL live edge rather than
      to this recent point. */
   if (s_pb_rewind_gesture_until != 0 && (LONG)(now - s_pb_rewind_gesture_until) >= 0) {
      PlaybackRewindMaybeCapture(frame_index, true);
      s_pb_rewind_steps = 0;
      s_pb_rewind_gesture_until = 0;
   } else {
      PlaybackRewindMaybeCapture(frame_index, false);
   }

   left_pressed = (GetAsyncKeyState(VK_LEFT) & 0x8000) ? true : false;
   {
      bool left_triggered = (left_pressed && !old_left_pressed) || s_pb_virtual_rewind_request;
      s_pb_virtual_rewind_request = false;
      old_left_pressed = left_pressed;
      left_pressed = left_triggered; /* repurpose for the trigger check below - real edge OR the toolbar's one-shot request */
   }
   if (left_pressed) {
      if (s_pb_rewind_gesture_until == 0)
         s_pb_rewind_steps = 0; /* fresh gesture, not a rapid repeat of a previous one - don't stack on old steps */

      if (s_pb_rewind_steps < s_pb_checkpoint_count) {
         int idx;
         PlaybackCheckpoint *cp;
         retro_ctx_serialize_info_t info;

         s_pb_rewind_steps++;
         idx = (s_pb_checkpoint_next - s_pb_rewind_steps + PLAYBACK_CHECKPOINT_COUNT * 2) % PLAYBACK_CHECKPOINT_COUNT;
         cp = &s_pb_checkpoints[idx];

         info.data_const = cp->data;
         info.data       = NULL;
         info.size       = cp->size;
         core_unserialize(&info);
         kailleraPlaybackSeekToFrame(cp->frame_index);
         s_watch_behind_live = true; /* no-op outside Watch Live - see PB_BTN_GOLIVE's own comment above */
         /* Deliberately doesn't touch pause state either way - keeps
            playing right on if it was already running (no need to hit
            Resume after every rewind tap), and stays put if the user had
            paused first to scrub through checkpoints one at a time. */

         s_pb_rewind_gesture_until = now + PLAYBACK_REWIND_GESTURE_MS;
         if (s_pb_rewind_gesture_until == 0)
            s_pb_rewind_gesture_until = 1;
      } else {
         runloop_msg_queue_push("Nao posso voltar mais do que isso ;(", 0, 90, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
      }
   }

   /* "Ir direto para o Ao Vivo!" - poll for the host's response after a
      PB_BTN_GOLIVE click (see the toolbar's WM_LBUTTONDOWN handler above).
      Bounded by WATCH_GOLIVE_TIMEOUT_MS in case the host is running an
      older DLL/build that never services the request at all - the button
      just re-arms afterward so the user can try again. */
   if (s_watch_golive_pending) {
      if ((LONG)(now - s_watch_golive_deadline) >= 0) {
         s_watch_golive_pending = false;
         runloop_msg_queue_push("O host nao respondeu ao pedido de sync ao vivo.", 0, 90, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);
      } else if (now - s_watch_golive_last_poll >= WATCH_GOLIVE_POLL_INTERVAL_MS) {
         s_watch_golive_last_poll = now;
         if (kailleraWatchStateReady()) {
            size_t state_size = core_serialize_size();
            void  *state_buf  = state_size ? malloc(state_size) : NULL;

            s_watch_golive_pending = false;
            if (state_buf != NULL) {
               int frame_index_dl = 0, byte_offset = 0;
               int n = kailleraWatchDownloadState(state_buf, (int)state_size, &frame_index_dl, &byte_offset);
               if (n > 0 && (size_t)n == state_size) {
                  retro_ctx_serialize_info_t info;
                  info.data_const = state_buf;
                  info.data       = NULL;
                  info.size       = (size_t)n;
                  core_unserialize(&info);
                  kailleraWatchJumpToLive(frame_index_dl, byte_offset);
                  s_watch_behind_live = false;
                  PlaybackRewindReset(); /* old checkpoints point into the buffer we just discarded (player_watch_jump_to_live()) - stale */
               } else {
                  runloop_msg_queue_push("Falha ao baixar o sync ao vivo do host.", 0, 90, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);
               }
               free(state_buf);
            }
         }
      }
   }
#endif
}

/* retry-connect: while the host searches for the right moment, every single
   Pause it presses immediately hands off a fresh savestate (see
   kailleraRetryConnectCaptureAndSendState(), called from the pause hotkey
   block in runloop.c) - the peer just applies whichever one arrived most
   recently (raw core_unserialize(), staying paused - see
   ApplyRetryConnectStateReady()), as many times as the host
   pauses/resumes/re-pauses. Only once the host commits with Enter does
   anything about *releasing* control happen, via this small hand-off
   sequence:
     1. RC_SEQ_HOST_PRE_GO_LIVE - host waits a small timeout (still paused)
        before actually sending RC_ACTION_GO_LIVE, so the pause has fully
        settled first;
     2. host sends RC_ACTION_GO_LIVE and immediately starts
        RC_SEQ_COUNTDOWN itself - no state to capture here, the last Pause
        already sent the one that matters;
     3. the peer, on receiving RC_ACTION_GO_LIVE, starts RC_SEQ_COUNTDOWN
        too - it's already sitting correctly loaded from the last
        RC_ACTION_STATE_READY.
   An earlier version had the peer briefly UNPAUSE for ~1s after each
   RC_ACTION_STATE_READY (then reload the same state and re-pause) to work
   around a cosmetic pause-menu-cursor glitch after a raw unserialize - that
   was removed after it turned out to be the actual cause of full game
   desyncs later in the resumed match: it made the peer run real emulation
   through a savestate round-trip the host's own core never went through,
   and this core's serialize/unserialize isn't provably complete (RetroArch's
   own netplay refuses to trust a core reporting
   RETRO_SERIALIZATION_QUIRK_INCOMPLETE for exactly this reason - see
   network/netplay/netplay_frontend.c). A cosmetic glitch beats a desynced
   match; if the cursor issue needs solving again, it'll need a fix that
   doesn't run the core forward at all (e.g. an explicit menu-state refresh
   call instead of "play a second of real frames").
   GetTickCount()-based throughout (this file is Windows-only - see the
   N02_WIN32/N02_LINUX guards elsewhere); the subtraction-and-compare form is
   the standard idiom for comparing tick counts safely across a wraparound. */
enum {
   RC_SEQ_NONE = 0,
   RC_SEQ_HOST_PRE_GO_LIVE,
   RC_SEQ_COUNTDOWN,
};

#define RETRYCONNECT_PRE_GO_LIVE_MS 500
#define RETRYCONNECT_COUNTDOWN_MS   5000

/* How long after the countdown ends (both host and peer unpause and the
   real Kaillera netcode resumes normally) local controller input keeps
   getting sent as neutral (nothing pressed) instead of whatever the player
   is actually holding - see kailleraRetryConnectSuppressLocalInput() and its
   call site in runloop.c. The idea: whatever a player happened to be
   physically holding at the exact moment control comes back is arbitrary
   and unsynchronized between machines (each side samples it at a slightly
   different wall-clock instant); giving both sides one guaranteed second of
   clean, deterministic neutral frames before real input starts flowing
   means the first REAL input either side contributes lines up on both ends
   the same way any other frame of live play would, instead of racing
   against the transition itself. */
#define RETRYCONNECT_NEUTRAL_INPUT_MS 1000

static int   s_rc_seq = RC_SEQ_NONE;
static DWORD s_rc_seq_deadline = 0;
static DWORD s_rc_neutral_input_until = 0;
static int   s_rc_last_announced_second = -1;

bool kailleraRetryConnectSuppressLocalInput() {
#if defined(N02_WIN32) || defined(N02_LINUX)
   return false;
#else
   return s_rc_neutral_input_until != 0 && (LONG)(GetTickCount() - s_rc_neutral_input_until) < 0;
#endif
}

static void RetryConnectStartCountdown(void) {
   s_rc_seq = RC_SEQ_COUNTDOWN;
   s_rc_seq_deadline = GetTickCount() + RETRYCONNECT_COUNTDOWN_MS;
   if (s_rc_seq_deadline == 0)
      s_rc_seq_deadline = 1;
   s_rc_last_announced_second = -1;
}

void kailleraRetryConnectCaptureAndSendState() {
#if defined(N02_WIN32) || defined(N02_LINUX)
   /* no-op - see kailleraRetryConnectCanControl() above */
#else
   size_t state_size = core_serialize_size();
   void  *state_buf  = state_size ? malloc(state_size) : NULL;

   if (state_buf != NULL) {
      retro_ctx_serialize_info_t info;
      info.data       = state_buf;
      info.data_const = NULL;
      info.size       = state_size;
      if (core_serialize(&info)) {
         kailleraRetryConnectUploadState(state_buf, (int)state_size);
      } else
         kailleraRetryConnectNotify(RC_ACTION_PAUSE);
      free(state_buf);
   } else {
      kailleraRetryConnectNotify(RC_ACTION_PAUSE);
   }
#endif
}

/* Downloads the state the host just uploaded and loads it, staying paused -
   called from DispatchRetryConnectAction() below on RC_ACTION_STATE_READY,
   every single time the host pauses (any number of times per session). Does
   NOT run the core forward at all (see the big comment above for why an
   earlier version's brief-unpause "settle" window was removed - it was
   causing real match desyncs). */
static void ApplyRetryConnectStateReady(void) {
   size_t state_size = core_serialize_size();
   void* state_buf;
   int n;

   if (state_size == 0) {
      command_event(CMD_EVENT_PAUSE, NULL);
      return;
   }

   state_buf = malloc(state_size);
   if (state_buf == NULL) {
      command_event(CMD_EVENT_PAUSE, NULL);
      return;
   }

   n = kailleraRetryConnectDownloadState(state_buf, (int)state_size, NULL);
   if (n > 0 && (size_t)n == state_size) {
      retro_ctx_serialize_info_t info;
      info.data_const = state_buf;
      info.data       = NULL;
      info.size       = (size_t)n;
      core_unserialize(&info);
      free(state_buf);
      command_event(CMD_EVENT_PAUSE, NULL);
      return;
   }

   runloop_msg_queue_push("retry-connect: falha ao sincronizar o state save do host.", 0, 5 * 60, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);
   free(state_buf);
   command_event(CMD_EVENT_PAUSE, NULL);
}

/* Ticks whichever stage is currently pending - call unconditionally from
   both kailleraRetryConnectFrameTick() (a peer could still be in its
   initial few seconds of unpaused free-play - see
   RETRYCONNECT_PEER_INITIAL_PLAY_MS below - when RC_ACTION_GO_LIVE arrives
   and starts RC_SEQ_COUNTDOWN, so this needs covering from the unpaused
   path too) and kailleraRetryConnectPauseTick() (the overwhelmingly common
   case, since every stage normally runs paused). A no-op the overwhelmingly
   common time nothing is pending. */
static void TickRetryConnectSequence(void) {
   switch (s_rc_seq) {
   case RC_SEQ_HOST_PRE_GO_LIVE:
      if ((LONG)(GetTickCount() - s_rc_seq_deadline) >= 0) {
         /* One more, authoritative capture right before declaring go-live -
            confirmed via the sync-checksum diagnostic that the host's own
            "paused" state can still drift by a frame or so between the last
            user-initiated Pause and this exact instant (a one-frame race in
            the pause hotkey handling, runloop.c - the frame Pause is
            pressed on can still run core_run() once more, with the OLD
            input, before actually freezing), so the snapshot taken back
            then is not reliably identical to what the host is actually
            sitting on right now. Re-sending here guarantees the peer loads
            EXACTLY the state the host is about to go live from, instead of
            whatever it happened to be a Pause or two ago. */
         kailleraRetryConnectCaptureAndSendState();
         kailleraRetryConnectNotify(RC_ACTION_GO_LIVE);
         RetryConnectStartCountdown();
      }
      break;
   case RC_SEQ_COUNTDOWN:
      {
         LONG remaining_ms = (LONG)(s_rc_seq_deadline - GetTickCount());
         if (remaining_ms <= 0) {
            s_rc_seq = RC_SEQ_NONE;
            s_rc_neutral_input_until = GetTickCount() + RETRYCONNECT_NEUTRAL_INPUT_MS;
            if (s_rc_neutral_input_until == 0)
               s_rc_neutral_input_until = 1;
            command_event(CMD_EVENT_UNPAUSE, NULL);
            kailleraRetryConnectRefreshPlaybackMode();
         } else {
            /* retry-connect: on-screen countdown, once per second - purely
               local (each side counts down its own instance of this same
               stage independently, both started within a network
               round-trip of each other), no need to broadcast it over game
               chat and risk two independent countdowns interleaving there. */
            int remaining_seconds = (remaining_ms + 999) / 1000;
            if (remaining_seconds != s_rc_last_announced_second) {
               char msg[64];
               s_rc_last_announced_second = remaining_seconds;
               snprintf(msg, sizeof(msg), "A partida iniciara em %d...", remaining_seconds);
               runloop_msg_queue_push(msg, 0, 60, true, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
            }
         }
      }
      break;
   default:
      break;
   }
}

/* Shared by kailleraRetryConnectFrameTick() (still running) and
   kailleraRetryConnectPauseTick() (already paused) below - a remote action
   can arrive while we're in either state, and both need to react the same
   way once it does. */
static void DispatchRetryConnectAction(int action) {
   switch (action) {
   case RC_ACTION_PAUSE:
      command_event(CMD_EVENT_PAUSE, NULL);
      break;
   case RC_ACTION_RESUME:
      command_event(CMD_EVENT_UNPAUSE, NULL);
      break;
   case RC_ACTION_GO_LIVE:
      /* The host's final commit (Enter) - peer is already sitting correctly
         loaded from the last RC_ACTION_STATE_READY, nothing left to settle;
         go straight to the shared release countdown. */
      RetryConnectStartCountdown();
      break;
   case RC_ACTION_STATE_READY:
      ApplyRetryConnectStateReady();
      break;
   default:
      break;
   }
}

/* Call once per core_run() frame (unlike kailleraRetryConnectPauseTick()
   below, this runs while NOT paused - the common case, since fast-forward is
   host-only and purely local: everyone else just keeps playing the replay
   at their own pace until the host's next signal arrives). Drains the same
   mailbox kailleraRetryConnectPauseTick() drains while paused, so a peer
   reacts to the host regardless of whether it happens to be paused already
   when the message shows up. */
#define RETRYCONNECT_PEER_INITIAL_PLAY_MS 5000
static DWORD s_peer_initial_pause_deadline = 0;

void kailleraRetryConnectFrameTick() {
#if defined(N02_WIN32) || defined(N02_LINUX)
   /* no-op - see kailleraRetryConnectCanControl() above */
#else
   int action, frame_index;

   /* retry-connect: let the peer play the first few seconds of the group
      replay for real (visual continuity with the match, rather than an
      instant freeze), then auto-pause and just wait - the host is
      deliberately excluded here, it keeps running normally right away,
      free to fast-forward/scrub on its own from the very start. The
      "Aguardando ..." announce (kaillera-client's
      kaillera_retryconnect_host_select()) is timed to land around the same
      moment this fires. */
   {
      static bool was_active = false;
      bool now_active = kailleraRetryConnectActive();
      if (now_active && !was_active && !kailleraRetryConnectCanControl()) {
         s_peer_initial_pause_deadline = GetTickCount() + RETRYCONNECT_PEER_INITIAL_PLAY_MS;
         if (s_peer_initial_pause_deadline == 0)
            s_peer_initial_pause_deadline = 1;
      }
      was_active = now_active;
   }
   if (s_peer_initial_pause_deadline != 0 &&
         (LONG)(GetTickCount() - s_peer_initial_pause_deadline) >= 0) {
      s_peer_initial_pause_deadline = 0;
      command_event(CMD_EVENT_PAUSE, NULL);
   }

   /* Covers the rare case where RC_ACTION_GO_LIVE arrives while still in the
      initial unpaused free-play window above - see TickRetryConnectSequence()'s
      own doc comment. */
   TickRetryConnectSequence();

   if (kailleraRetryConnectPollF == NULL)
      return;

   action = 0;
   frame_index = 0;
   while (kailleraRetryConnectPollF(&action, &frame_index))
      DispatchRetryConnectAction(action);
#endif
}

/* Host-only, only while genuinely paused and no hand-off already in
   progress - starts the multi-stage go-live sequence (see the big comment
   above ApplyRetryConnectStateReady()). Shared by the raw Enter-key poll
   below and the toolbar's "Selecionar" button, so both trigger the exact
   same sequence instead of two independent copies of this 3-line dance. */
static void RetryConnectRequestGoLive(void) {
   if (!kailleraRetryConnectCanControl() || s_rc_seq != RC_SEQ_NONE)
      return;
   s_rc_seq = RC_SEQ_HOST_PRE_GO_LIVE;
   s_rc_seq_deadline = GetTickCount() + RETRYCONNECT_PRE_GO_LIVE_MS;
   if (s_rc_seq_deadline == 0)
      s_rc_seq_deadline = 1;
}

/* See the forward declaration next to the toolbar code above. */
static bool RetryConnectSelectReady(void) {
   return s_rc_seq == RC_SEQ_NONE;
}

void kailleraRetryConnectPauseTick() {
#if defined(N02_WIN32) || defined(N02_LINUX)
   /* no-op - see kailleraRetryConnectCanControl() above */
#else
   static bool old_enter_pressed = false;
   bool enter_pressed;
   int action;
   int frame_index;

   if (kailleraRetryConnectPollF == NULL)
      return;

   /* Enter (commit to this point) - host only, only while actually paused
      (this function only runs from RUNLOOP_STATE_PAUSE, so we know we are).
      No native RetroArch hotkey exists for this, so it's a direct key check
      here rather than going through the input-remapping/hotkey system. */
   enter_pressed = (GetAsyncKeyState(VK_RETURN) & 0x8000) ? true : false;
   if (enter_pressed && !old_enter_pressed)
      RetryConnectRequestGoLive();
   old_enter_pressed = enter_pressed;

   TickRetryConnectSequence();

   /* Remote RESUME/GO_LIVE/STATE_READY arriving while we're already paused -
      the normal per-frame drain (kailleraRetryConnectFrameTick(), called from
      core_run()) does not run at all while paused, so this is the only place
      a paused client ever sees these. */
   action = 0;
   frame_index = 0;
   while (kailleraRetryConnectPollF(&action, &frame_index))
      DispatchRetryConnectAction(action);
#endif
}

/* Shows/positions/repaints the shared toolbar (see PlaybackToolbarPaint()'s
   rc_mode branch) for retry-connect and captures the host's periodic
   rewind checkpoints - the retry-connect counterpart to
   kailleraPlaybackRewindTick(), called from the exact same two call sites
   (runloop.c, both while paused and during normal core_run() frames) since
   the toolbar needs to stay usable in either state, same as solo Playback's.
   Only ever calls PlaybackToolbarSetVisible() for its OWN transitions
   (entering or leaving retry-connect) - NOT unconditionally on every "not
   active" tick, since that would stomp the SAME call
   kailleraPlaybackRewindTick() just made a moment earlier for solo Playback/
   Watch Live (both "not retry-connect" too, but very much still meant to
   show the toolbar) - this actually happened once already (Watch Live's
   toolbar going invisible the moment this function shipped), hence the
   explicit transition-only guard rather than the more obvious-looking
   unconditional call. No-op outside retry-connect. */
void kailleraRetryConnectToolbarTick(void) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   /* no-op - see kailleraRetryConnectCanControl() above */
#else
   static bool was_active = false;
   bool now_active = kailleraRetryConnectActive();
   bool can_control = now_active && kailleraRetryConnectCanControl();

   if (now_active != was_active) {
      /* New session (or the previous one just ended) - old checkpoints
         point at a different replay file entirely, same reasoning as
         PlaybackRewindReset()'s own call site. */
      RetryConnectCheckpointReset();
      if (!now_active)
         PlaybackToolbarSetVisible(false); /* we were the one showing it - hide it on our way out */
   }
   was_active = now_active;

   if (!now_active)
      return;

   PlaybackToolbarSetVisible(GetForegroundWindow() == win32_get_window());

   PlaybackToolbarPosition(s_pb_toolbar);
   if (s_pb_toolbar)
      InvalidateRect(s_pb_toolbar, NULL, FALSE); /* Pause/FF/progress reflect live state, which can change from the keyboard too */

   if (can_control) {
      runloop_state_t *runloop_st = runloop_state_get_ptr();
      bool paused = (runloop_st->flags & RUNLOOP_FLAG_PAUSED) ? true : false;
      if (!paused)
         RetryConnectCheckpointMaybeCapture(kailleraRetryConnectGetFrameIndex());
   }
#endif
}

void kailleraChatSendExternal(const char* messge) {
   if (strlen(messge) > 0)
   {
#if defined(N02_WIN32) || defined(N02_LINUX)
      client.gameplay.sendChat(messge);
#else
      kailleraChatSendF(messge);
#endif
   }
}

void EndKailleraGame() {
   if (kailleraInitialisedInternal) {
      kailleraWaitSaveLoad = 0;
      kailleraNetplay = false;
      kailleraPlaybackMode = false;
      kailleraInitialisedInternal = 0;
      stop_execute_shit = 0;
#if defined(N02_WIN32) || defined(N02_LINUX)
      client.gameplay.endGame();
#else
      kailleraEndGameF();
#endif
      command_event(CMD_EVENT_CLOSE_CONTENT, NULL);
   }
}

void GetClientVersion(char* version) {
#if defined(N02_WIN32) || defined(N02_LINUX)
   strcpy(version, clientInfo.name);
   strcat(version, " v");
   strcat(version, clientInfo.version);
   strcat(version, " ");
   strcat(version, clientInfo.buildDate);
#else
   kailleraGetVersionF(version);
#endif
}

#ifdef N02_LINUX
#define PATHSEPARATOR "/"
#else
#define PATHSEPARATOR "\\"
#endif

void AddGamesToList() {
   char path[1024];
   char temp[256];
   char gname[128];
   char cn[64];
   char* ch;
   const struct playlist_entry* entry = NULL;

   kailleraGames = kailleraRomNames;
   memset(kailleraRomNames, 0, sizeof(kailleraRomNames));
   memset(filePathK, 0, sizeof(filePathK));
   memset(corePaths, 0, sizeof(corePaths));
   totalGames = 0;

   for (int i = 0; i < playlist_size(g_defaults.content_history) && i < MAX_GAMES; i++) {
      playlist_get_index(g_defaults.content_history, i, &entry);
      if (entry->path != NULL && entry->core_name != NULL && entry->core_path != NULL) {
         strlcpy(filePathK[totalGames], entry->path, sizeof(filePathK[totalGames]));
         strlcpy(corePaths[totalGames], entry->core_path, sizeof(corePaths[totalGames]));
         strcpy(path, entry->path);
         strcpy(cn, entry->core_name);
         ch = strtok(path, PATHSEPARATOR);
         while (ch != NULL) {
            strcpy(temp, ch);
            ch = strtok(NULL, PATHSEPARATOR);
         }
         *gname = 0;
         if (strstr(cn, "(") != NULL) {
            ch = strtok(cn, "(");
            ch = strtok(NULL, ")");
            strcat(gname, ch);
         } else strcat(gname, cn);    
         strcat(gname, ": ");
         strcat(gname, temp);
         gname[127] = 0;
#if defined(N02_WIN32) || defined(N02_LINUX)
         client.games.add(gname, MAX_INPUTS);
#endif
         strncpy(kailleraGames, gname, strlen(gname) + 1);
         kailleraGames += strlen(gname) + 1;
         totalGames++;
      }
   }
   *++kailleraGames = '\0';
}

void kMessage_core_info()
{
#if defined(N02_WIN32) || defined(N02_LINUX)
   client.gameplay.synchronizeGame(0, 0);
#endif
   struct retro_system_info* system = &runloop_state_get_ptr()->system.info;
   if (system->library_name != NULL)
   {
      char str[80] = "[CORE] ";
      strcat(str, system->library_name);
      strcat(str, " ");
      strcat(str, system->library_version);
      kailleraChatSendExternal(str);
#ifdef KAILLERA_DEFAULT
      kailleraPacketSize = KAILLERA_NEED_ANALOG; //8 bytes should be always until we find stable way detect core input size before loading
#endif
   }
}

void cp1251_to_utf8(char* out, const char* in) {
   static const char table[128][5] = {
       "\2\xD0\x82","\2\xD0\x83","\3\xE2\x80\x9A","\2\xD1\x93",
       "\3\xE2\x80\x9E","\3\xE2\x80\xA6","\3\xE2\x80\xA0","\3\xE2\x80\xA1",
       "\3\xE2\x82\xAC","\3\xE2\x80\xB0","\2\xD0\x89","\3\xE2\x80\xB9",
       "\2\xD0\x8A","\2\xD0\x8C","\2\xD0\x8B","\2\xD0\x8F",
       "\2\xD1\x92","\3\xE2\x80\x98","\3\xE2\x80\x99","\3\xE2\x80\x9C",
       "\3\xE2\x80\x9D","\3\xE2\x80\xA2","\3\xE2\x80\x93","\3\xE2\x80\x94",
       "\0","\3\xE2\x84\xA2","\2\xD1\x99","\3\xE2\x80\xBA",
       "\2\xD1\x9A","\2\xD1\x9C","\2\xD1\x9B","\2\xD1\x9F",
       "\2\xC2\xA0","\2\xD0\x8E","\2\xD1\x9E","\2\xD0\x88",
       "\2\xC2\xA4","\2\xD2\x90","\2\xC2\xA6","\2\xC2\xA7",
       "\2\xD0\x81","\2\xC2\xA9","\2\xD0\x84","\2\xC2\xAB",
       "\2\xC2\xAC","\2\xC2\xAD","\2\xC2\xAE","\2\xD0\x87",
       "\2\xC2\xB0","\2\xC2\xB1","\2\xD0\x86","\2\xD1\x96",
       "\2\xD2\x91","\2\xC2\xB5","\2\xC2\xB6","\2\xC2\xB7",
       "\2\xD1\x91","\3\xE2\x84\x96","\2\xD1\x94","\2\xC2\xBB",
       "\2\xD1\x98","\2\xD0\x85","\2\xD1\x95","\2\xD1\x97",
       "\2\xD0\x90","\2\xD0\x91","\2\xD0\x92","\2\xD0\x93",
       "\2\xD0\x94","\2\xD0\x95","\2\xD0\x96","\2\xD0\x97",
       "\2\xD0\x98","\2\xD0\x99","\2\xD0\x9A","\2\xD0\x9B",
       "\2\xD0\x9C","\2\xD0\x9D","\2\xD0\x9E","\2\xD0\x9F",
       "\2\xD0\xA0","\2\xD0\xA1","\2\xD0\xA2","\2\xD0\xA3",
       "\2\xD0\xA4","\2\xD0\xA5","\2\xD0\xA6","\2\xD0\xA7",
       "\2\xD0\xA8","\2\xD0\xA9","\2\xD0\xAA","\2\xD0\xAB",
       "\2\xD0\xAC","\2\xD0\xAD","\2\xD0\xAE","\2\xD0\xAF",
       "\2\xD0\xB0","\2\xD0\xB1","\2\xD0\xB2","\2\xD0\xB3",
       "\2\xD0\xB4","\2\xD0\xB5","\2\xD0\xB6","\2\xD0\xB7",
       "\2\xD0\xB8","\2\xD0\xB9","\2\xD0\xBA","\2\xD0\xBB",
       "\2\xD0\xBC","\2\xD0\xBD","\2\xD0\xBE","\2\xD0\xBF",
       "\2\xD1\x80","\2\xD1\x81","\2\xD1\x82","\2\xD1\x83",
       "\2\xD1\x84","\2\xD1\x85","\2\xD1\x86","\2\xD1\x87",
       "\2\xD1\x88","\2\xD1\x89","\2\xD1\x8A","\2\xD1\x8B",
       "\2\xD1\x8C","\2\xD1\x8D","\2\xD1\x8E","\2\xD1\x8F"
   };
   while (*in)
   {
      if (*in & 0xC0) {
         const char* p = table[(int)(0x7f & *in++)];
         if (!*p)
            continue;
         *out++ = p[1];
         if (*p == 1)
            continue;
         *out++ = p[2];
         if (*p == 2)
            continue;
         *out++ = p[3];
      }
      else
         *out++ = *in++;
   }
   *out = 0;
}
