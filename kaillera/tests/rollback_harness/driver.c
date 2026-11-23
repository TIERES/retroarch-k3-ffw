/* Two "PCs" running the real kaillera_sync.c in lockstep, the way runloop.c's core_run() drives it
   in a Kaillera game: each frame FrameTick (may queue a command), the server hands back everyone's
   input+command from DELAY frames earlier, FrameMode decides run/frozen, AfterFrame digests the
   RAM, and RESTORE/RESUME go to OnCommand after the frame. Chat lines ([SYNC] fingerprints, digests,
   restore point hashes) reach both PCs CHAT_LATENCY frames later (the server echoes to the sender).
   A desync = one RAM byte flipped on PC B (like the old F10 test key).

   Usage: driver <frames> <desync frame,...> [B-misses-RESTORE-frame]                        */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <windows.h>

#define RAM_SIZE (2 * 1024 * 1024)
#define DELAY 3
#define CHAT_LATENCY 8
#define MAXF 40000

typedef struct
{
   void (*chat)(int machine, const char *text);
   void (*osd)(int machine, const char *msg);
} h_callbacks_t;

typedef struct
{
   HMODULE h;
   void (*init)(int, int, const char *, const char *, h_callbacks_t *);
   void (*set_time)(long long);
   unsigned char *(*ram)(void);
   int *(*cmds)(void);
   void (*run)(const unsigned char *, int);
   void (*begin)(int, bool, bool, bool, int);
   void (*loaded)(void);
   void (*tick)(void);
   int (*mode)(void);
   void (*after)(unsigned);
   void (*oncmd)(int);
   bool (*chat)(const char *, const char *);
   void (*end)(void);
   unsigned core_frame;
   int frozen, frozen_start;
   int resumes_seen;
} pc_t;

static pc_t pc[2];
static const char *nick[2] = { "HOST", "PEER" };
static int cur_frame;

typedef struct { int due, sender; char text[256]; } chat_t;
static chat_t chatq[20000];
static int chat_n;

static short out_rec[2][MAXF][6]; /* what each PC sent: [0] = command, [1..5] = input */

static void on_chat(int m, const char *text)
{
   if (chat_n < 20000)
   {
      chatq[chat_n].due    = cur_frame + CHAT_LATENCY;
      chatq[chat_n].sender = m;
      strncpy(chatq[chat_n].text, text, 255);
      chat_n++;
   }
}

static char last_osd[2][256];
static void on_osd(int m, const char *msg)
{
   /* the countdown repeats - print each different line once */
   if (strcmp(last_osd[m], msg) == 0)
      return;
   strncpy(last_osd[m], msg, 255);
   printf("  f=%5d %s OSD: %s\n", cur_frame, nick[m], msg);
}

#define SYM(p, name, field) do { *(FARPROC*)&p->field = GetProcAddress(p->h, name); \
   if (!p->field) { printf("missing %s\n", name); exit(3); } } while (0)

static void load_pc(int m, const char *dir)
{
   char path[MAX_PATH], content[MAX_PATH];
   pc_t *p = &pc[m];
   h_callbacks_t cb = { on_chat, on_osd };
   snprintf(path, sizeof(path), "%s\\ksync_h.dll", dir);
   p->h = LoadLibraryA(path);
   if (!p->h) { printf("LoadLibrary %s failed\n", path); exit(3); }
   SYM(p, "h_init", init); SYM(p, "h_set_time", set_time); SYM(p, "h_get_ram", ram);
   SYM(p, "h_commands", cmds); SYM(p, "h_run", run);
   SYM(p, "kailleraSyncGameBegin", begin); SYM(p, "kailleraSyncContentLoaded", loaded);
   SYM(p, "kailleraSyncFrameTick", tick); SYM(p, "kailleraSyncFrameMode", mode);
   SYM(p, "kailleraSyncAfterFrame", after); SYM(p, "kailleraSyncOnCommand", oncmd);
   SYM(p, "kailleraSyncHandleChat", chat); SYM(p, "kailleraSyncGameEnd", end);
   snprintf(content, sizeof(content), "%s\\game.bin", dir);
   p->init(m, m + 1, dir, content, &cb);
   p->set_time(123456789LL + m * 777777LL); /* different nonce per PC */
   p->begin(2, false, true, false, 0);
   p->loaded();
}

static void input_for(int player, int f, short *w)
{
   unsigned x = (unsigned)(f * 2654435761u) ^ (unsigned)(player * 40503u);
   int k;
   for (k = 1; k < 6; k++)
   {
      x = x * 1103515245u + 12345u;
      w[k] = (short)((x >> 16) & 0x0FFF);
   }
}

int main(int argc, char **argv)
{
   int frames = argc > 1 ? atoi(argv[1]) : 7200;
   int desync[32], nd = 0, i, f, m;
   int miss_restore_b = argc > 3 ? atoi(argv[3]) : 0;
   int diff_start = -1, diff_ranges = 0, rollback_cmds = 0;
   char *p;

   for (p = argc > 2 ? argv[2] : ""; *p && nd < 32;)
   {
      desync[nd++] = atoi(p);
      p = strchr(p, ',');
      if (!p) break;
      p++;
   }
   load_pc(0, argc > 4 ? argv[4] : "A");
   load_pc(1, argc > 5 ? argv[5] : "B");

   for (f = 1; f <= frames && f < MAXF; f++)
   {
      cur_frame = f;
      for (m = 0; m < 2; m++)
      {
         pc_t *q = &pc[m];
         q->core_frame++;
         q->set_time((long long)f * 16667);
         q->tick();
         out_rec[m][f][0] = (short)*q->cmds();
         *q->cmds() = 0;
         /* EXTRA_RESTORE=frame:point - the host's BACKSPACE during the countdown */
         if (m == 0 && getenv("EXTRA_RESTORE") && atoi(getenv("EXTRA_RESTORE")) == f)
            out_rec[m][f][0] = (short)(7 | (atoi(strchr(getenv("EXTRA_RESTORE"), ':') + 1) << 4));
         input_for(m, f, out_rec[m][f]);
         if (out_rec[m][f][0])
            printf("  f=%5d %s sends command 0x%04X\n", f, nick[m], (unsigned short)out_rec[m][f][0]);
      }
      for (m = 0; m < 2; m++)
      {
         pc_t *q = &pc[m];
         int cmd = 0, mode, pl;
         unsigned char in[2 * 10];
         int len = 0;
         if (f > DELAY)
         {
            for (pl = 0; pl < 2; pl++)
            {
               if (out_rec[pl][f - DELAY][0])
                  cmd = out_rec[pl][f - DELAY][0];
               memcpy(in + pl * 10, &out_rec[pl][f - DELAY][1], 10);
            }
            len = sizeof(in);
         }
         /* Another command in the same frame wins (kReceivedCommand = last player's) - the
            RESTORE is lost for EVERY PC, as it would be in a real game. */
         if (miss_restore_b && (cmd & 0xF) == 7 && f >= miss_restore_b)
         {
            printf("  f=%5d %s: RESTORE lost on purpose (test)\n", f, nick[m]);
            cmd = 0;
            if (m == 1)
               miss_restore_b = 0;
         }
         mode = q->mode();
         if (mode == 0)
         {
            q->run(in, len);
            q->after(q->core_frame);
            if (q->frozen)
            {
               printf("  f=%5d %s runs again (was frozen %d frames, from f=%d)\n", f, nick[m],
                     f - q->frozen_start, q->frozen_start);
               q->frozen = 0;
            }
         }
         else if (mode == 2)
         {
            unsigned char z[20] = { 0 };
            q->run(z, sizeof(z));
         }
         else if (!q->frozen)
         {
            q->frozen       = 1;
            q->frozen_start = f;
         }
         if (cmd && ((cmd & 0xF) == 7 || (cmd & 0xF) == 8))
         {
            if (m == 0)
               rollback_cmds++;
            printf("  f=%5d %s gets %s (point %d)\n", f, nick[m], (cmd & 0xF) == 7 ? "RESTORE" : "RESUME",
                  (cmd >> 4) & 0xFFF);
            q->oncmd(cmd);
         }
      }
      for (i = 0; i < nd; i++)
         if (desync[i] == f)
         {
            pc[1].ram()[RAM_SIZE / 2] ^= 0xFF;
            printf("  f=%5d *** desync provocado no PEER (byte da RAM invertido)\n", f);
         }
      for (i = 0; i < chat_n; i++)
         if (chatq[i].due == f)
            for (m = 0; m < 2; m++)
               pc[m].chat(nick[chatq[i].sender], chatq[i].text);

      if (memcmp(pc[0].ram(), pc[1].ram(), RAM_SIZE) != 0)
      {
         if (diff_start < 0)
         {
            diff_start = f;
            diff_ranges++;
         }
      }
      else if (diff_start >= 0)
      {
         printf("  RAM HOST x PEER diferente nos frames %d-%d\n", diff_start, f - 1);
         diff_start = -1;
      }
   }
   if (diff_start >= 0)
      printf("  RAM HOST x PEER diferente do frame %d ATE O FIM\n", diff_start);
   printf("RESULT frames=%d desyncs=%d diff_ranges=%d end_equal=%d rollback_cmds=%d\n", frames, nd,
         diff_ranges, diff_start < 0, rollback_cmds);
   pc[0].end();
   pc[1].end();
   return 0;
}
