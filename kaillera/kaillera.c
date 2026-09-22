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



void CloseKaillera() {
   if (kailleraInitialised) {
      kailleraShutdownF(); //in n02 this callback do nothing
      kailleraInitialised = false;
      kailleraPlaybackMode = false;
      CloseHandle(KailleraHandle);
   }
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

static int InitialiseKaillera() {
   kailleraInfos kInfos;

   kInfos.appName = "RetroArch " PACKAGE_VERSION;
   kInfos.gameList = kailleraRomNames;
   kInfos.gameCallback = kailleraGameCallback;
   kInfos.chatReceivedCallback = kailleraChatReceivedCallback;
   kInfos.clientDroppedCallback = kailleraClientDroppedCallback;
   kInfos.moreInfosCallback = NULL; //kailleraMoreInfosCallback; //not used (support only supraclient)
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

   /* Enter (go live) - host only, only while actually paused (this function
      only runs from RUNLOOP_STATE_PAUSE, so we know we are). No native
      RetroArch hotkey exists for this, so it's a direct key check here
      rather than going through the input-remapping/hotkey system. */
   if (kailleraRetryConnectCanControl()) {
      enter_pressed = (GetAsyncKeyState(VK_RETURN) & 0x8000) ? true : false;
      if (enter_pressed && !old_enter_pressed) {
         command_event(CMD_EVENT_UNPAUSE, NULL);
         kailleraRetryConnectNotify(RC_ACTION_GO_LIVE);
         kailleraRetryConnectRefreshPlaybackMode();
      }
      old_enter_pressed = enter_pressed;
   }

   /* Remote RESUME/GO_LIVE arriving while we're already paused - the normal
      per-frame drain (kaillera_retryconnect_pump(), called from
      kailleraModifyPlayValuesF()) does not run at all while paused, so this
      is the only place a paused client ever sees these. */
   action = 0;
   frame_index = 0;
   while (kailleraRetryConnectPollF(&action, &frame_index)) {
      switch (action) {
      case RC_ACTION_RESUME:
         command_event(CMD_EVENT_UNPAUSE, NULL);
         break;
      case RC_ACTION_GO_LIVE:
         command_event(CMD_EVENT_UNPAUSE, NULL);
         kailleraRetryConnectRefreshPlaybackMode();
         break;
      default:
         break;
      }
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
