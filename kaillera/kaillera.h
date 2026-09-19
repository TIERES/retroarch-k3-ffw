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
