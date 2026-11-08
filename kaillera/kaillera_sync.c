 //******************************************************//
 //************ KAILLERA NETPLAY - ANTI-DESYNC **********//
 //******************************************************//
 /* See kaillera_sync.h for what this module does and why. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <boolean.h>
#include <libretro.h>
#include <compat/strl.h>
#include <encodings/crc32.h>
#include <features/features_cpu.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#ifdef HAVE_THREADS
#include <rthreads/rthreads.h>
#endif

#include "kaillera.h"
#include "kaillera_sync.h"
#include "core.h"
#include "runloop.h"
#include "paths.h"
#include "verbosity.h"

#ifdef _WIN32
/* gfx/common/win32_common.h - declared here to keep that header's weight
   out of this file; only needed to tell RetroArch's window apart from the
   Kaillera dialogs, which live in this same process (typing in the chat box
   must not trigger a rollback). */
HWND win32_get_window(void);
#endif

/* Bump when the "[SYNC]" line format or the meaning of a field changes -
   fingerprints from a different protocol version are never compared. */
#define KSYNC_PROTOCOL        "1"
#define KSYNC_PREFIX          "[SYNC] "
#define KSYNC_FIELD_LEN       40
#define KSYNC_MAX_REMOTES     (MAX_INPUTS * 2)
#define KSYNC_MAX_BIOS        4
#define KSYNC_CHUNK           (1024 * 1024)
/* ~10s: re-send once if somebody's fingerprint is still missing (they may
   have missed ours too). ~20s: give up and warn about who never answered. */
#define KSYNC_RESEND_FRAMES   (10 * 60)
#define KSYNC_MISSING_FRAMES  (20 * 60)
/* 4. Desync detector: one digest of the core's system RAM per window of
   KSYNC_DIGEST_FRAMES frames (~5s), built incrementally - 1/300 of the RAM
   (~7KB of the PS1's 2MB) per frame, so there is never a spike. Machines run
   in lockstep, so each hashes the same slice on the same frame and their
   digests only differ once the emulation itself has diverged. */
#define KSYNC_DIGEST_FRAMES   300
#define KSYNC_DIGEST_HISTORY  8     /* windows kept for a lagging peer (~40s) */
#define KSYNC_LOG_NAME        "kaillera_sync.log"
/* 5./6. Restore points: every KSYNC_SNAPSHOT_FRAMES frames (~10s) every
   machine serializes the core at the same frame (~4MB for PCSX, a few ms).
   The ring keeps one more than it offers, so a peer that noticed the desync
   a little later (and took one extra point) still has every point offered. */
#define KSYNC_SNAPSHOT_FRAMES 600
#define KSYNC_SNAPSHOTS       6
#define KSYNC_SNAPSHOT_OFFER  5
#define KSYNC_COUNTDOWN_USEC  (5 * 1000000)
#define KSYNC_PROMPT_EVERY    (20 * 60)  /* frames between prompt repeats */
#define KSYNC_PROMPT_REPEATS  3
/* After this long the rollback offer lapses - a key hit by accident minutes
   later must not jump minutes back. */
#define KSYNC_PROMPT_EXPIRE   (60 * 60)
/* A RESTORE/RESUME we sent that never came back through the stream (lost,
   or replaced by another command in the same frame) - try again after this. */
#define KSYNC_COMMAND_RETRY   (3 * 60)
/* Rollback key - rarely a controller binding (ENTER is Start on keyboards). */
#define KSYNC_ROLLBACK_KEY    "BACKSPACE"
#define KSYNC_ROLLBACK_VK     VK_BACK
#define KSYNC_STATE_MISMATCH_LOGS 3      /* detailed log lines per game */

/* 2. Canonical settings. Only emulation-relevant options are listed - purely
   visual/audio ones (resolution, dithering, interpolation, ...) may differ
   freely between players. Values are the package defaults, except for the
   memory cards: "none" in both slots, so nobody's local .srm /
   pcsx-card2.mcd (option files, formations, Master League...) can make one
   machine boot differently from the other. Keys belong to PCSX ReARMed only,
   so other cores are unaffected. */
static const struct
{
   const char *key;
   const char *value;
} ksync_forced_options[] = {
   { "pcsx_rearmed_memcard1",             "none"     },
   { "pcsx_rearmed_memcard2",             "none"     },
   { "pcsx_rearmed_bios",                 "auto"     },
   { "pcsx_rearmed_show_bios_bootlogo",   "disabled" },
   { "pcsx_rearmed_region",               "auto"     },
   { "pcsx_rearmed_drc",                  "enabled"  },
   { "pcsx_rearmed_psxclock",             "57"       },
   { "pcsx_rearmed_nostalls",             "disabled" },
   { "pcsx_rearmed_icache_emulation",     "enabled"  },
   { "pcsx_rearmed_exception_emulation",  "disabled" },
   { "pcsx_rearmed_nosmccheck",           "disabled" },
   { "pcsx_rearmed_gteregsunneeded",      "disabled" },
   { "pcsx_rearmed_nogteflags",           "disabled" },
   { "pcsx_rearmed_gpu_slow_llists",      "auto"     },
   /* Not just a speed setting: threaded and non-threaded GPU emulate
      different timings, so the game runs differently from boot (and a
      spectator's state diverges ~5s after "Ir ao vivo!"). "auto" = the
      package default = threaded on any 2+ core CPU. */
   { "pcsx_rearmed_gpu_thread_rendering", "auto"     },
   { "pcsx_rearmed_cd_turbo",             "disabled" },
   { "pcsx_rearmed_noxadecoding",         "enabled"  },
   { "pcsx_rearmed_nocdaudio",            "enabled"  },
   { "pcsx_rearmed_spu_reverb",           "enabled"  },
   { "pcsx_rearmed_frameskip_type",       "disabled" },
   /* Placeholders - the real value depends on the game's player count and
      the room's "MultiTap" checkbox, see ksync_forced_value(). */
   { "pcsx_rearmed_multitap",             "disabled" },
   { "pcsx_rearmed_multitap1",            "disabled" },
   { "pcsx_rearmed_multitap2",            "disabled" },
   { "pcsx_rearmed_analog_axis_modifier", "square"   },
   { "pcsx_rearmed_input_sensitivity",    "1.00"     },
};

/* Everything else kailleraSyncForcedDevice()/input_driver.c/runloop.c force
   during a Kaillera game - part of the settings hash so a fork build that
   forces something different is reported as a mismatch. */
static const char ksync_forced_extra[] =
      "device=joypad;portmap=identity;sram=off;autostate=off;diskindex=off;";

typedef struct
{
   char nick[32];
   char proto[8];
   char core[KSYNC_FIELD_LEN];
   char content[KSYNC_FIELD_LEN];
   char bios[KSYNC_FIELD_LEN];
   char opts[KSYNC_FIELD_LEN];
   char memcard[KSYNC_FIELD_LEN]; /* "off", or "on:<crc32 of card 1>" */
} ksync_fingerprint_t;

/* A RAM digest (window = digest window) or a restore point's state hash
   (window = restore point id), tagged with the rollback epoch it belongs
   to - both numberings restart after every rollback. */
typedef struct
{
   unsigned window;
   unsigned epoch;
   unsigned peers_matched; /* ours only: peers whose digest matched it */
   char     hash[17];
   bool     valid;
} ksync_digest_t;

/* One restore point - main thread only. */
typedef struct
{
   void    *data;
   size_t   size;
   unsigned id;             /* detector frame / KSYNC_SNAPSHOT_FRAMES */
   unsigned epoch;
   unsigned peers_same;     /* peers whose state hash equals ours */
   unsigned peers_diff;
   char     hash[17];
   bool     valid;
   bool     verified_blob;  /* every peer's copy is byte-identical */
   bool     ram_before;     /* RAM window just before it matched everywhere */
   bool     ram_after;      /* ...and the one starting at it */
} ksync_snapshot_t;

/* Another player in the game, keyed by nick. */
typedef struct
{
   ksync_fingerprint_t fp;
   ksync_digest_t      digests[KSYNC_DIGEST_HISTORY]; /* by window % HISTORY */
   ksync_digest_t      states[KSYNC_SNAPSHOTS];       /* by id % SNAPSHOTS */
   unsigned            digests_matched;
   unsigned            digests_mismatched;
   bool                used;            /* slot taken by this nick */
   bool                has_fp;          /* its fingerprint line arrived */
   bool                compared;        /* ...and was already reported */
   bool                desync_reported;
} ksync_peer_t;

typedef struct
{
   unsigned gen;
   bool pcsx;     /* the core's sidecar-.cue rule applies, see ksync_pcsx_sidecar_cue() */
   char path[PATH_MAX_LENGTH];
} ksync_crc_job_t;

/* Shared with the DLL's chat thread and the CRC thread - KSYNC_LOCK(). */
static ksync_fingerprint_t ksync_local;
static ksync_peer_t        ksync_peers[KSYNC_MAX_REMOTES];
static char                ksync_own_nick[32];
static char                ksync_nonce[8];
static char                ksync_local_msg[192];
static volatile unsigned   ksync_gen;
static int                 ksync_num_players;
static bool                ksync_playback;
static bool                ksync_no_memcard = true; /* room's "Sem M. Card" */
static void ksync_live_reset(void); /* placar ao vivo - section 4 */
/* PSX Multitap ports for this game: 0 = none, 1 = port 1 (3-4 players),
   2 = ports 1 and 2 (5-8 players) - see kailleraSyncGameBegin(). */
static int                 ksync_multitap_ports;
/* Memory Card online (wg-camp, via the DLL): ksync_memcard_mode is the room's
   choice (KSYNC_MEMCARD_ONLINE = online); ksync_mc_online is set once the
   DLL actually put both cards in ksync_mc_dir - slot 1 = 1P's card, slot 2 =
   2P's, the same files on every machine. The core then runs with both slots
   on "shared" and that folder as its save directory, so the player's own
   cards/saves are never read or written. See kailleraSyncPrepareMemcards().
   ksync_mc_empty_slots: the slots the DLL left without a card (bit 0 = slot
   1, bit 1 = slot 2 - the player of that slot picked "Sem M. Card"), which
   then run on "none": no card inserted, not a blank one. */
static int                 ksync_memcard_mode;
static bool                ksync_mc_online;
static unsigned            ksync_mc_empty_slots;
static char                ksync_mc_dir[PATH_MAX_LENGTH];
static bool                ksync_send_enabled;
static bool                ksync_content_ready;
static bool                ksync_sent;
static bool                ksync_resent;
static bool                ksync_missing_warned;
static unsigned            ksync_frames_since_sent;
static bool                ksync_digest_enabled;

/* Main thread only - the running RAM digest and our finished ones. */
static uint64_t            ksync_ram_acc;
static unsigned            ksync_ram_window;
static unsigned            ksync_frame_base; /* frame 0 of the detector's numbering */
static bool                ksync_renumber_pending; /* restart numbering next frame */
static unsigned            ksync_last_frame;       /* last frame run, detector numbering */
static ksync_digest_t      ksync_local_digests[KSYNC_DIGEST_HISTORY];

/* Rollback epoch - bumped (identically everywhere) by every resume. Guarded
   by KSYNC_LOCK() like the peers' digests it's compared against. */
static unsigned            ksync_epoch;

/* 5./6. Restore points and rollback - main thread only. */
enum { KSYNC_R_NONE = 0, KSYNC_R_PROMPT, KSYNC_R_FROZEN };
static ksync_snapshot_t    ksync_snapshots[KSYNC_SNAPSHOTS]; /* by id % SNAPSHOTS */
static unsigned            ksync_snapshot_last_id;   /* newest taken this epoch */
static bool                ksync_capture_stopped;    /* desync seen: keep the ring */
static int                 ksync_r_state;
static unsigned            ksync_r_id;               /* point loaded while frozen */
static bool                ksync_r_render;           /* show it: run one neutral frame */
static retro_time_t        ksync_r_deadline;         /* countdown end */
static int                 ksync_r_last_second;
static bool                ksync_r_resume_sent;
static bool                ksync_r_request_pending;  /* our RESTORE is in flight */
static unsigned            ksync_r_command_frames;   /* ...or our RESUME, for this long */
static unsigned            ksync_prompt_frames;
static unsigned            ksync_prompt_total;       /* frames since the prompt started */
static int                 ksync_prompt_shown;
/* Per-game summary (logs\kaillera_sync.log). */
static unsigned            ksync_snaps_taken, ksync_snaps_blob_ok,
                           ksync_snaps_ram_ok, ksync_snaps_diff,
                           ksync_state_mismatch_logs, ksync_rollbacks;
static retro_time_t        ksync_snaps_usec;

/* Main thread only - the core logs these from retro_init()/retro_load_game(). */
static char                ksync_bios_found_name[KSYNC_MAX_BIOS][64];
static uint32_t            ksync_bios_found_crc[KSYNC_MAX_BIOS];
static int                 ksync_bios_found_count;
static char                ksync_bios_loaded_name[64];

#ifdef HAVE_THREADS
static slock_t *ksync_lock;
#define KSYNC_LOCK()   do { if (ksync_lock) slock_lock(ksync_lock);   } while (0)
#define KSYNC_UNLOCK() do { if (ksync_lock) slock_unlock(ksync_lock); } while (0)
#else
#define KSYNC_LOCK()
#define KSYNC_UNLOCK()
#endif

static bool ksync_core_is_pcsx(void)
{
   struct retro_system_info *info = &runloop_state_get_ptr()->system.info;
   return info->library_name && strstr(info->library_name, "PCSX");
}

bool kailleraSyncActive(void)
{
   return kailleraInitialisedInternal != 0;
}

bool kailleraSyncBlockSram(void)
{
   return kailleraSyncActive() && (ksync_no_memcard || ksync_mc_online);
}

bool kailleraSyncBlockSramSave(void)
{
   /* A replay, Watch Live or retry-connect replaying a match with memory
      cards must not write that match's saves over the viewer's own card. */
   return kailleraSyncActive() && (ksync_no_memcard || ksync_mc_online || ksync_playback);
}

const char *kailleraSyncSaveDirectory(void)
{
   return kailleraSyncActive() && ksync_mc_online ? ksync_mc_dir : NULL;
}

/* Appends one timestamped line to logs\kaillera_sync.log next to
   retroarch.exe (handshake results, desyncs, per-game summary) - so a
   player's report can be checked afterwards even with RetroArch's own
   logging off, as it is in the package. Main thread only. */
static void ksync_log(const char *fmt, ...)
{
   static char path[PATH_MAX_LENGTH];
   char line[512];
   char stamp[32];
   time_t now        = time(NULL);
   struct tm *tm_now = localtime(&now);
   va_list vp;
   RFILE *f;
   int n;

   if (!path[0])
   {
      char dir[PATH_MAX_LENGTH];
      char logs[PATH_MAX_LENGTH];
      fill_pathname_application_dir(dir, sizeof(dir));
      fill_pathname_join(logs, dir, "logs", sizeof(logs));
      fill_pathname_join(path, path_is_directory(logs) ? logs : dir,
            KSYNC_LOG_NAME, sizeof(path));
   }

   if (!tm_now || !strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm_now))
      strlcpy(stamp, "?", sizeof(stamp));

   n = snprintf(line, sizeof(line), "%s  ", stamp);
   va_start(vp, fmt);
   if (n > 0 && (size_t)n < sizeof(line))
      vsnprintf(line + n, sizeof(line) - n, fmt, vp);
   va_end(vp);
   strlcat(line, "\r\n", sizeof(line));

   /* Append: open the existing file, or create it the first time. */
   if (!(f = filestream_open(path,
               RETRO_VFS_FILE_ACCESS_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
               RETRO_VFS_FILE_ACCESS_HINT_NONE)))
      f = filestream_open(path, RETRO_VFS_FILE_ACCESS_WRITE,
            RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!f)
      return;
   filestream_seek(f, 0, RETRO_VFS_SEEK_POSITION_END);
   filestream_write(f, line, strlen(line));
   filestream_close(f);
}

/* ------------------------------------------------------------------------ */
/* 2. Canonical settings                                                    */
/* ------------------------------------------------------------------------ */

/* ksync_forced_options[i]'s value for this game. The multitap keys follow
   ksync_multitap_ports: a multitap in port 1 for 3-4 players, one in each
   port for 5-8 (which player gets which pad: kailleraSyncSlotForPort()). The legacy
   multitap1/multitap2 keys ("auto"/"disabled"/"enabled", older cores) are
   kept consistent with the current single key. */
static const char *ksync_forced_value(size_t i)
{
   const char *key = ksync_forced_options[i].key;

   if (string_is_equal(key, "pcsx_rearmed_multitap"))
      return ksync_multitap_ports == 2 ? "ports 1 and 2"
           : ksync_multitap_ports == 1 ? "port 1" : "disabled";
   if (string_is_equal(key, "pcsx_rearmed_multitap1"))
      return ksync_multitap_ports >= 1 ? "enabled" : "disabled";
   if (string_is_equal(key, "pcsx_rearmed_multitap2"))
      return ksync_multitap_ports >= 2 ? "enabled" : "disabled";
   /* Online cards: both slots read/write pcsx-card1.mcd / pcsx-card2.mcd in
      the save directory, which points at the match's own folder - but a slot
      the DLL left empty has no card at all. */
   if (ksync_mc_online && string_is_equal(key, "pcsx_rearmed_memcard1"))
      return (ksync_mc_empty_slots & 1) ? "none" : "shared";
   if (ksync_mc_online && string_is_equal(key, "pcsx_rearmed_memcard2"))
      return (ksync_mc_empty_slots & 2) ? "none" : "shared";

   return ksync_forced_options[i].value;
}

const char *kailleraSyncForcedCoreOption(const char *key)
{
   size_t i;

   if (!key || !kailleraSyncActive())
      return NULL;

   /* The room turned "Sem M. Card" off: everyone keeps their own cards (and
      the fingerprint's m= field checks card 1 matches). */
   if (!ksync_no_memcard && !ksync_mc_online && strncmp(key, "pcsx_rearmed_memcard", 20) == 0)
      return NULL;

   for (i = 0; i < sizeof(ksync_forced_options) / sizeof(ksync_forced_options[0]); i++)
      if (string_is_equal(key, ksync_forced_options[i].key))
         return ksync_forced_value(i);

   return NULL;
}

unsigned kailleraSyncForcedDevice(unsigned port, unsigned device)
{
   (void)port;

   /* Standard pad on every port for everyone: a DualShock (analog) on one
      machine and a digital pad on the other make the game see different
      controllers. The package's analog-to-dpad setting keeps the left stick
      usable on the standard pad. */
   if (kailleraSyncActive() && ksync_core_is_pcsx())
      return RETRO_DEVICE_JOYPAD;

   return device;
}

static uint32_t ksync_options_hash(void)
{
   size_t i;
   uint32_t crc = 0;
   char line[128];

   for (i = 0; i < sizeof(ksync_forced_options) / sizeof(ksync_forced_options[0]); i++)
   {
      int n = snprintf(line, sizeof(line), "%s=%s;",
            ksync_forced_options[i].key, ksync_forced_value(i));
      if (n > 0)
         crc = encoding_crc32(crc, (const uint8_t*)line, (size_t)n);
   }

   crc = encoding_crc32(crc, (const uint8_t*)ksync_forced_extra,
         sizeof(ksync_forced_extra) - 1);
   /* A build that feeds the multitap's pads in another order (before
      kailleraSyncSlotForPort()) gets a mismatch instead of a silent desync. */
   if (ksync_multitap_ports)
   {
      static const char order[] = "multitap_order=1A,2,1B,1C,1D;";
      crc = encoding_crc32(crc, (const uint8_t*)order, sizeof(order) - 1);
   }
   return crc;
}

/* Kaillera slot (player - 1) whose input core port `port` reads. Without a
   multitap it's the same number. With one in port 1, PCSX ReARMed's ports
   0-3 are the pads 1A-1D and port 4 is the pad in port 2 - and Winning Eleven
   numbers its controllers 1A, 2, 1B, 1C, 1D (1P to 5P). So player 2 goes to
   the port-2 pad and players 3-5 to 1B-1D: every Kaillera player is the
   game's player of the same number. Before this, player 4 landed on 1D, the
   game's 5P, while its 2P (the port-2 pad) had nobody (Walter's 4-player
   test, 2026-10-09). With multitaps in both ports, players 6-8 stay on
   2B-2D. */
unsigned kailleraSyncSlotForPort(unsigned port)
{
   static const unsigned slot_of_port[8] = { 0, 2, 3, 4, 1, 5, 6, 7 };

   if (!ksync_multitap_ports || port >= 8)
      return port;
   return slot_of_port[port];
}

/* ------------------------------------------------------------------------ */
/* BIOS actually loaded by the core (PCSX ReARMed log lines)                */
/* ------------------------------------------------------------------------ */

static void ksync_copy_basename(const char *path, size_t len, char *out, size_t out_len)
{
   size_t i, start = 0, n;

   for (i = 0; i < len && path[i]; i++)
      if (path[i] == '/' || path[i] == '\\')
         start = i + 1;

   n = i - start;
   if (n >= out_len)
      n = out_len - 1;
   memcpy(out, path + start, n);
   out[n] = '\0';

   /* Drop the trailing newline/whitespace the core's messages end with. */
   while (n > 0 && isspace((unsigned char)out[n - 1]))
      out[--n] = '\0';
}

void kailleraSyncCoreLog(const char *fmt, va_list vp)
{
   static const char found_tag[]  = "BIOS file, crc32 ";
   static const char loaded_tag[] = "Loaded BIOS \"";
   char msg[512];
   const char *p;

   if (!fmt || !kailleraSyncActive())
      return;

   vsnprintf(msg, sizeof(msg), fmt, vp);

   /* "found <region> BIOS file, crc32 <crc>: <path>" - one per region found. */
   if ((p = strstr(msg, found_tag)))
   {
      unsigned crc   = 0;
      const char *at = strstr(p, ": ");

      if (     at
            && ksync_bios_found_count < KSYNC_MAX_BIOS
            && sscanf(p + sizeof(found_tag) - 1, "%x", &crc) == 1)
      {
         ksync_copy_basename(at + 2, strlen(at + 2),
               ksync_bios_found_name[ksync_bios_found_count],
               sizeof(ksync_bios_found_name[0]));
         ksync_bios_found_crc[ksync_bios_found_count] = crc;
         ksync_bios_found_count++;
      }
   }
   /* 'Loaded BIOS "<dir>/<file>".' - the one the game really boots with. */
   else if ((p = strstr(msg, loaded_tag)))
   {
      const char *start = p + sizeof(loaded_tag) - 1;
      const char *end   = strchr(start, '"');

      if (end)
         ksync_copy_basename(start, (size_t)(end - start),
               ksync_bios_loaded_name, sizeof(ksync_bios_loaded_name));
   }
}

static void ksync_describe_bios(char *out, size_t len)
{
   int i;

   if (!ksync_core_is_pcsx())
   {
      strlcpy(out, "-", len); /* not applicable - never compared */
      return;
   }

   /* No 'Loaded BIOS' line: the core fell back to its HLE BIOS. */
   if (!ksync_bios_loaded_name[0])
   {
      strlcpy(out, "HLE", len);
      return;
   }

   for (i = 0; i < ksync_bios_found_count; i++)
   {
      if (string_is_equal_noncase(ksync_bios_found_name[i], ksync_bios_loaded_name))
      {
         snprintf(out, len, "%08X", (unsigned)ksync_bios_found_crc[i]);
         return;
      }
   }

   strlcpy(out, "?", len);
}

/* ------------------------------------------------------------------------ */
/* Content CRC32 (background thread - a 700MB image takes a few seconds)    */
/* ------------------------------------------------------------------------ */

static bool ksync_hash_file(const char *path, unsigned gen,
      uint32_t *crc, uint64_t *size)
{
   int64_t n;
   uint8_t *buf;
   RFILE *f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (!f)
      return false;

   if (!(buf = (uint8_t*)malloc(KSYNC_CHUNK)))
   {
      filestream_close(f);
      return false;
   }

   while ((n = filestream_read(f, buf, KSYNC_CHUNK)) > 0)
   {
      *crc   = encoding_crc32(*crc, buf, (size_t)n);
      *size += (uint64_t)n;
      if (gen != ksync_gen) /* a newer game started - this result is moot */
         break;
   }

   free(buf);
   filestream_close(f);
   return gen == ksync_gen;
}

static bool ksync_line_starts_with_file(const char *s)
{
   return toupper((unsigned char)s[0]) == 'F'
       && toupper((unsigned char)s[1]) == 'I'
       && toupper((unsigned char)s[2]) == 'L'
       && toupper((unsigned char)s[3]) == 'E'
       && (s[4] == ' ' || s[4] == '\t');
}

/* PCSX ReARMed opens a disc image through cdriso.c's ISOopen(), which first
   tries the image's name with its last 4 characters swapped for .toc, .ccd,
   .mds and .cue. A .cue found that way takes over: parsecue() swaps the open
   file for the .bin the .cue names, and the image itself is never read - a
   .cue/.bin pair left next to the .chd of the same game, say. Two players
   "with the same .chd" then emulate different discs (the core reads a .chd's
   audio tracks a few sectors off), and the game drifts apart: the WE PW26
   desync of 2026-10-14. True, with that .cue in `cue`, when it takes over;
   a .toc/.ccd/.mds comes first and keeps the image. */
static bool ksync_pcsx_sidecar_cue(const char *image, char *cue, size_t len)
{
   static const char *const before[] = { ".toc", ".ccd", ".mds" };
   size_t n = strlen(image);
   const char *tail;
   size_t i;

   if (n < 4 || n >= len)
      return false;
   tail = image + n - 4;
   /* A .cue is read as itself, and so is a ".cd<X>" file. */
   if (     string_is_equal_noncase(tail, ".cue")
         || string_is_equal_noncase(tail, ".m3u")
         || (tail[0] == '.' && tolower((unsigned char)tail[1]) == 'c'
            && tolower((unsigned char)tail[2]) == 'd'))
      return false;

   strlcpy(cue, image, len);
   for (i = 0; i < sizeof(before) / sizeof(before[0]); i++)
   {
      strlcpy(cue + n - 4, before[i], len - (n - 4));
      if (path_is_valid(cue))
         return false;
   }
   strlcpy(cue + n - 4, ".cue", len - (n - 4));
   return path_is_valid(cue);
}

/* .cue/.m3u only describe the real data - hash the files they point at (in
   order), so two different .bin behind identically named .cue still differ.
   pcsx: `path` is an image PCSX ReARMed opens itself (the content, or an
   .m3u entry), so its sidecar .cue counts instead - see above. The first
   such .cue goes to `sidecar` (PATH_MAX_LENGTH chars, may be NULL). */
static bool ksync_hash_content(const char *path, unsigned gen,
      uint32_t *crc, uint64_t *size, int depth, bool pcsx, char *sidecar)
{
   const char *ext = path_get_extension(path);
   bool is_cue     = string_is_equal_noncase(ext, "cue");
   bool is_m3u     = string_is_equal_noncase(ext, "m3u");
   void *raw       = NULL;
   int64_t raw_len = 0;
   char *line, *next;
   bool any        = false;
   bool ok         = true;

   if (pcsx && !is_cue && !is_m3u)
   {
      char cue[PATH_MAX_LENGTH];
      uint32_t cue_crc  = *crc;
      uint64_t cue_size = *size;

      if (     ksync_pcsx_sidecar_cue(path, cue, sizeof(cue))
            && ksync_hash_content(cue, gen, &cue_crc, &cue_size, depth, false, NULL))
      {
         *crc  = cue_crc;
         *size = cue_size;
         if (sidecar && !sidecar[0])
            strlcpy(sidecar, cue, PATH_MAX_LENGTH);
         return true;
      }
      /* A .cue naming files that aren't there doesn't take over. */
   }

   if ((!is_cue && !is_m3u) || depth >= 2)
      return ksync_hash_file(path, gen, crc, size);

   if (!filestream_read_file(path, &raw, &raw_len) || !raw)
      return false;

   for (line = (char*)raw; line && *line && ok; line = next)
   {
      char name[PATH_MAX_LENGTH];
      char full[PATH_MAX_LENGTH];
      char *end;

      if ((next = strchr(line, '\n')))
         *next++ = '\0';
      if ((end = strchr(line, '\r')))
         *end = '\0';
      while (*line == ' ' || *line == '\t')
         line++;

      name[0] = '\0';

      if (is_cue)
      {
         if (!ksync_line_starts_with_file(line))
            continue;
         line += 4;
         while (*line == ' ' || *line == '\t')
            line++;
         if (*line == '"')
         {
            char *q = strchr(++line, '"');
            if (!q)
               continue;
            *q = '\0';
         }
         else if ((end = strpbrk(line, " \t")))
            *end = '\0';
         strlcpy(name, line, sizeof(name));
      }
      else if (*line && *line != '#')
         strlcpy(name, line, sizeof(name));

      if (!name[0])
         continue;

      fill_pathname_resolve_relative(full, path, name, sizeof(full));
      /* .m3u entries are images the core opens; a .cue's FILEs aren't. */
      ok  = ksync_hash_content(full, gen, crc, size, depth + 1,
            pcsx && is_m3u, sidecar);
      any = true;
   }

   free(raw);

   if (!any) /* malformed playlist - fall back to the file itself */
      return ksync_hash_file(path, gen, crc, size);

   return ok;
}

/* The player is about to play something other than the file they picked - a
   different "arquivo do jogo (ISO)" in the fingerprint is the likely result. */
static void ksync_report_sidecar(const char *content, const char *cue)
{
   char msg[256];

   ksync_log("Atenção: %s está na pasta do jogo - o PCSX ReARMed joga o que esse "
         ".cue indica, e não o %s. A verificação de sincronia usa o que ele joga.",
         path_basename(cue), path_basename(content));
   snprintf(msg, sizeof(msg), "Atenção: o RetroArch vai jogar o %s (está na "
         "mesma pasta), e não o %s.", path_basename(cue), path_basename(content));
   runloop_msg_queue_push(msg, 1, 600, false, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_WARNING);
}

static void ksync_crc_thread(void *userdata)
{
   ksync_crc_job_t *job = (ksync_crc_job_t*)userdata;
   uint32_t crc         = 0;
   uint64_t size        = 0;
   char sidecar[PATH_MAX_LENGTH];
   bool ok;

#ifdef _WIN32
   /* Don't compete with the emulator for CPU during the first seconds. */
   SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

   sidecar[0] = '\0';
   ok = ksync_hash_content(job->path, job->gen, &crc, &size, 0, job->pcsx, sidecar);
   if (ok && sidecar[0] && job->gen == ksync_gen)
      ksync_report_sidecar(job->path, sidecar);

   KSYNC_LOCK();
   if (job->gen == ksync_gen)
   {
      if (ok)
         snprintf(ksync_local.content, sizeof(ksync_local.content),
               "%08X:%X", (unsigned)crc, (unsigned)size);
      else
         strlcpy(ksync_local.content, "?", sizeof(ksync_local.content));
      ksync_content_ready = true;
   }
   KSYNC_UNLOCK();

   free(job);
}

/* "CRC32:SIZE" of this game's content, when kailleraSyncPrepareMemcards()
   already hashed it - kailleraSyncContentLoaded() reuses it instead of
   hashing the whole ISO a second time. Empty otherwise. */
static char ksync_content_precomputed[KSYNC_FIELD_LEN];

static bool ksync_core_path_is_pcsx(const char *core_path)
{
   char name[PATH_MAX_LENGTH];
   char *c;
   if (string_is_empty(core_path))
      return false;
   strlcpy(name, path_basename(core_path), sizeof(name));
   for (c = name; *c; c++)
      *c = (char)tolower((unsigned char)*c);
   return strstr(name, "pcsx") != NULL;
}

void kailleraSyncPrepareMemcards(const char *content_path, const char *core_path)
{
   char content_id[KSYNC_FIELD_LEN];
   char path[PATH_MAX_LENGTH];
   char dir[PATH_MAX_LENGTH];
   char sidecar[PATH_MAX_LENGTH];
   uint32_t crc  = 0;
   uint64_t size = 0;
   bool ok;
   int result;

   ksync_mc_online              = false;
   ksync_mc_empty_slots         = 0;
   ksync_mc_dir[0]              = '\0';
   ksync_content_precomputed[0] = '\0';
   sidecar[0]                   = '\0';

   /* PS1 cards only - another core in an "online" room just plays without. */
   if (ksync_memcard_mode != KSYNC_MEMCARD_ONLINE || !ksync_core_path_is_pcsx(core_path))
      return;

   /* What the core will play - the fingerprint's content field. */
   ok = ksync_hash_content(content_path, ksync_gen, &crc, &size, 0, true, sidecar);
   if (ok)
      snprintf(ksync_content_precomputed, sizeof(ksync_content_precomputed),
            "%08X:%X", (unsigned)crc, (unsigned)size);
   /* The cards stay those of the file the player picked, sidecar .cue or not. */
   if (ok && sidecar[0])
   {
      ksync_report_sidecar(content_path, sidecar);
      crc  = 0;
      size = 0;
      ok   = ksync_hash_content(content_path, ksync_gen, &crc, &size, 0, false, NULL);
   }

   if (!ok)
   {
      ksync_log("Memory Card online: não consegui ler o jogo (%s).", content_path);
      result = -1;
   }
   else
   {
      snprintf(content_id, sizeof(content_id), "%08X:%X", (unsigned)crc, (unsigned)size);

      /* One folder, emptied first - a stale card from an earlier match must
         never be what the core opens if the DLL fails to write a new one. */
      fill_pathname_application_dir(dir, sizeof(dir));
      fill_pathname_join(ksync_mc_dir, dir, "kaillera-memcards", sizeof(ksync_mc_dir));
      path_mkdir(ksync_mc_dir);
      fill_pathname_join(path, ksync_mc_dir, "pcsx-card1.mcd", sizeof(path));
      filestream_delete(path);
      fill_pathname_join(path, ksync_mc_dir, "pcsx-card2.mcd", sizeof(path));
      filestream_delete(path);

      result = kailleraMemcardPrepareExternal(content_id,
            path_basename(content_path), ksync_mc_dir);
   }

   if (result == 1)
   {
      ksync_mc_online      = true;
      ksync_mc_empty_slots = (unsigned)kailleraMemcardEmptySlotsExternal() & 3;
      ksync_log("Memory Card online: cartões do jogo %s prontos em %s (slot 1: %s, slot 2: %s).",
            content_id, ksync_mc_dir,
            (ksync_mc_empty_slots & 1) ? "vazio" : "cartão",
            (ksync_mc_empty_slots & 2) ? "vazio" : "cartão");
      return;
   }

   /* Couldn't get the cards: play this match with no memory card at all (the
      safe setup). Should the others have gotten theirs, the fingerprint
      check reports the memory card difference to everyone. */
   ksync_mc_dir[0]  = '\0';
   ksync_no_memcard = true;
   ksync_log("Memory Card online indisponível (código %d) - partida sem memory card.", result);
   runloop_msg_queue_push("Memory Card online indisponível - esta partida vai sem memory card.",
         2, 600, true, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_WARNING);
}

/* ------------------------------------------------------------------------ */
/* 3. Fingerprint handshake                                                 */
/* ------------------------------------------------------------------------ */

void kailleraSyncInit(void)
{
#ifdef HAVE_THREADS
   if (!ksync_lock)
      ksync_lock = slock_new();
#endif
}

/* Drops every restore point and any rollback in progress. Main thread. */
static void ksync_rollback_reset(void)
{
   int i;

   for (i = 0; i < KSYNC_SNAPSHOTS; i++)
      free(ksync_snapshots[i].data);
   memset(ksync_snapshots, 0, sizeof(ksync_snapshots));
   ksync_snapshot_last_id    = 0;
   ksync_capture_stopped     = false;
   ksync_r_state             = KSYNC_R_NONE;
   ksync_r_render            = false;
   ksync_r_request_pending   = false;
   ksync_r_resume_sent       = false;
   ksync_prompt_shown        = 0;
   ksync_snaps_taken         = 0;
   ksync_snaps_blob_ok       = 0;
   ksync_snaps_ram_ok        = 0;
   ksync_snaps_diff          = 0;
   ksync_state_mismatch_logs = 0;
   ksync_rollbacks           = 0;
   ksync_snaps_usec          = 0;
}

void kailleraSyncGameBegin(int num_players, bool playback, bool no_memcard,
      bool multitap, int memcard_mode)
{
   unsigned seed;

   KSYNC_LOCK();
   ksync_gen++;
   memset(&ksync_local, 0, sizeof(ksync_local));
   memset(ksync_peers,  0, sizeof(ksync_peers));
   memset(ksync_local_digests, 0, sizeof(ksync_local_digests));
   ksync_own_nick[0]       = '\0';
   ksync_local_msg[0]      = '\0';
   ksync_num_players       = num_players;
   ksync_playback          = playback;
   ksync_no_memcard        = no_memcard;
   ksync_memcard_mode      = memcard_mode;
   ksync_mc_online         = false;
   ksync_mc_empty_slots    = 0;
   ksync_mc_dir[0]         = '\0';
   ksync_content_precomputed[0] = '\0';
   /* Every player derives this from the same inputs (the server's player
      count, the host's announced checkbox), and it is part of the settings
      hash - a player whose build decides differently gets a mismatch. */
   ksync_multitap_ports    = !multitap || num_players <= 2 ? 0
                           : num_players <= 4             ? 1 : 2;
   ksync_send_enabled      = false;
   ksync_digest_enabled    = false;
   ksync_content_ready     = false;
   ksync_sent              = false;
   ksync_resent            = false;
   ksync_missing_warned    = false;
   ksync_frames_since_sent = 0;
   ksync_live_reset();
   ksync_ram_window        = (unsigned)-1;
   ksync_frame_base        = 0;
   ksync_renumber_pending  = false;
   ksync_last_frame        = 0;
   ksync_epoch             = 0;

   /* Tells our own "[SYNC]" line apart when the server echoes it back.
      Per-machine entropy only - an unseeded rand() would give every player
      the same nonce and each would discard the others' lines as "ours". */
   seed = (unsigned)cpu_features_get_time_usec() ^ (ksync_gen << 20);
#ifdef _WIN32
   seed ^= (unsigned)GetCurrentProcessId() << 8;
#endif
   snprintf(ksync_nonce, sizeof(ksync_nonce), "%06x", seed & 0xFFFFFF);

   ksync_bios_found_count    = 0;
   ksync_bios_loaded_name[0] = '\0';
   KSYNC_UNLOCK();
   /* Restore points are main-thread only (this runs on the DLL's thread) -
      kailleraSyncContentLoaded() resets them. */
}

void kailleraSyncGameEnd(void)
{
   char summary[KSYNC_MAX_REMOTES + 1][200];
   int  summary_count = 0;
   int  i;

   KSYNC_LOCK();
   if (ksync_digest_enabled && ksync_snaps_taken)
      snprintf(summary[summary_count++], sizeof(summary[0]),
            "Pontos de volta: %u tirados (média %.1f ms), %u idênticos em todos "
            "os PCs, %u só confirmados pela RAM, %u diferentes; %u volta(s) "
            "feita(s).", ksync_snaps_taken,
            (double)ksync_snaps_usec / 1000.0 / ksync_snaps_taken,
            ksync_snaps_blob_ok, ksync_snaps_ram_ok, ksync_snaps_diff,
            ksync_rollbacks);
   if (ksync_digest_enabled)
   {
      for (i = 0; i < KSYNC_MAX_REMOTES; i++)
      {
         const ksync_peer_t *peer = &ksync_peers[i];
         if (!peer->used)
            continue;
         snprintf(summary[summary_count++], sizeof(summary[0]),
               "Fim da partida - %s: %u verificações de RAM iguais, "
               "%u diferentes%s.", peer->fp.nick,
               peer->digests_matched, peer->digests_mismatched,
               peer->desync_reported ? " (DESYNC)" : "");
      }
   }
   ksync_gen++; /* orphans a CRC job that may still be running */
   ksync_send_enabled   = false;
   ksync_digest_enabled = false;
   ksync_content_ready  = false;
   memset(ksync_peers, 0, sizeof(ksync_peers));
   KSYNC_UNLOCK();

   ksync_rollback_reset();

   for (i = 0; i < summary_count; i++)
      ksync_log("%s", summary[i]);
}

void kailleraSyncContentLoaded(void)
{
   struct retro_system_info *info = &runloop_state_get_ptr()->system.info;
   const char *content            = path_get(RARCH_PATH_CONTENT);
   ksync_crc_job_t *job           = NULL;
   char core[KSYNC_FIELD_LEN];
   char memcard[KSYNC_FIELD_LEN];
   char *c;

   ksync_rollback_reset();

   /* Memory card setup for the fingerprint - taken now, right after the
      player's .srm was loaded and before a single frame could write to it. */
   if (ksync_mc_online)
   {
      /* Both online cards, as the core is about to use them. */
      char path[PATH_MAX_LENGTH];
      uint32_t crc[2] = { 0, 0 };
      int slot;
      for (slot = 0; slot < 2; slot++)
      {
         void *data  = NULL;
         int64_t len = 0;
         fill_pathname_join(path, ksync_mc_dir,
               slot ? "pcsx-card2.mcd" : "pcsx-card1.mcd", sizeof(path));
         if (filestream_read_file(path, &data, &len) && data)
            crc[slot] = encoding_crc32(0, (const uint8_t*)data, (size_t)len);
         free(data);
      }
      snprintf(memcard, sizeof(memcard), "mc:%08X:%08X",
            (unsigned)crc[0], (unsigned)crc[1]);
   }
   else if (ksync_no_memcard)
      strlcpy(memcard, "off", sizeof(memcard));
   else
   {
      struct retro_core_t *rc = &runloop_state_get_ptr()->current_core;
      const void *sram = rc->retro_get_memory_data
         ? rc->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM) : NULL;
      size_t sram_len  = rc->retro_get_memory_size
         ? rc->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) : 0;
      if (sram && sram_len)
         snprintf(memcard, sizeof(memcard), "on:%08X",
               (unsigned)encoding_crc32(0, (const uint8_t*)sram, sram_len));
      else
         strlcpy(memcard, "on:?", sizeof(memcard));
   }

   snprintf(core, sizeof(core), "%s %s",
         info->library_name    ? info->library_name    : "?",
         info->library_version ? info->library_version : "?");
   for (c = core; *c; c++)
      if (*c == ' ' || *c == '=')
         *c = '_';

   KSYNC_LOCK();
   strlcpy(ksync_local.proto, KSYNC_PROTOCOL, sizeof(ksync_local.proto));
   strlcpy(ksync_local.core,  core,           sizeof(ksync_local.core));
   snprintf(ksync_local.opts, sizeof(ksync_local.opts), "%08X",
         (unsigned)ksync_options_hash());
   strlcpy(ksync_local.memcard, memcard, sizeof(ksync_local.memcard));

   /* Nothing to compare in a solo game or in plain playback/Watch Live
      (recorded input, no live peer) - retry-connect is a live room though. */
   ksync_send_enabled = ksync_num_players > 1
      && (!ksync_playback || kailleraRetryConnectActive());

   /* RAM digests need every machine to number frames identically from the
      game's start. Retry-connect (and playback) jump in mid-match with a
      loaded state and per-machine frame counters, so they sit this out -
      retry-connect joins in at go-live (kailleraSyncRetryConnectGoLive()). */
   ksync_digest_enabled = ksync_send_enabled && !ksync_playback;

   if (ksync_send_enabled)
   {
      if (string_is_empty(content))
      {
         strlcpy(ksync_local.content, "?", sizeof(ksync_local.content));
         ksync_content_ready = true;
      }
      else if (ksync_content_precomputed[0])
      {
         /* Same "CRC32:SIZE" ksync_crc_thread() would compute. */
         strlcpy(ksync_local.content, ksync_content_precomputed, sizeof(ksync_local.content));
         ksync_content_ready = true;
      }
      else if ((job = (ksync_crc_job_t*)malloc(sizeof(*job))))
      {
         job->gen = ksync_gen;
         job->pcsx = ksync_core_is_pcsx();
         strlcpy(job->path, content, sizeof(job->path));
      }
   }
   KSYNC_UNLOCK();

   if (ksync_send_enabled)
      ksync_log("=== Partida: %s | %s | %d jogadores | %s | multitap: %s%s",
            string_is_empty(content) ? "?" : path_basename(content), core,
            ksync_num_players,
            ksync_mc_online ? "memory card online"
            : ksync_no_memcard ? "sem memory card" : "com memory card",
            ksync_multitap_ports == 2 ? "portas 1 e 2"
            : ksync_multitap_ports == 1 ? "porta 1" : "nao",
            ksync_digest_enabled ? ""
            : " (retry-connect: detector de RAM liga no go-live)");

   if (ksync_multitap_ports && ksync_core_is_pcsx())
      runloop_msg_queue_push(ksync_multitap_ports == 2
            ? "MultiTap ativado: jogador 1 = controle 1-A, 2 = controle 2-A, 3 a 5 = 1-B a 1-D, 6 a 8 = 2-B a 2-D."
            : "MultiTap ativado: jogador 1 = controle 1-A, 2 = controle 2, 3 a 5 = 1-B a 1-D.",
            1, 300, false, NULL, MESSAGE_QUEUE_ICON_DEFAULT,
            MESSAGE_QUEUE_CATEGORY_INFO);

   if (!job)
      return;

#ifdef HAVE_THREADS
   {
      sthread_t *thread = sthread_create(ksync_crc_thread, job);
      if (thread)
         sthread_detach(thread);
      else
         ksync_crc_thread(job);
   }
#else
   ksync_crc_thread(job);
#endif
}

static bool ksync_same_fingerprint(const ksync_fingerprint_t *a,
      const ksync_fingerprint_t *b)
{
   return string_is_equal(a->proto,   b->proto)
       && string_is_equal(a->core,    b->core)
       && string_is_equal(a->content, b->content)
       && string_is_equal(a->bios,    b->bios)
       && string_is_equal(a->opts,    b->opts)
       && string_is_equal(a->memcard, b->memcard);
}

/* Slot for `nick`, allocating one on first sight. Caller holds the lock. */
static ksync_peer_t *ksync_peer_for(const char *nick)
{
   int i, free_slot = -1;

   for (i = 0; i < KSYNC_MAX_REMOTES; i++)
   {
      if (ksync_peers[i].used && string_is_equal(ksync_peers[i].fp.nick, nick))
         return &ksync_peers[i];
      if (!ksync_peers[i].used && free_slot < 0)
         free_slot = i;
   }

   if (free_slot < 0)
      return NULL;

   memset(&ksync_peers[free_slot], 0, sizeof(ksync_peers[free_slot]));
   ksync_peers[free_slot].used = true;
   strlcpy(ksync_peers[free_slot].fp.nick, nick, sizeof(ksync_peers[free_slot].fp.nick));
   return &ksync_peers[free_slot];
}

/* "<window or restore point id>:<16 hex>" - a peer's RAM digest ("d=") or
   restore point state hash ("s="), into its slot of `ring`. Caller holds
   the lock. */
static void ksync_store_hash(ksync_digest_t *ring, unsigned ring_len,
      const char *value, unsigned epoch)
{
   char *end;
   unsigned long window = strtoul(value, &end, 10);
   ksync_digest_t *d;

   if (end == value || *end != ':' || strlen(end + 1) != 16)
      return;

   d         = &ring[window % ring_len];
   d->window = (unsigned)window;
   d->epoch  = epoch;
   d->valid  = true;
   strlcpy(d->hash, end + 1, sizeof(d->hash));
}

bool kailleraSyncHandleChat(const char *nick, const char *text)
{
   ksync_fingerprint_t fp;
   ksync_peer_t *peer;
   char buf[256];
   char nonce[8]   = "";
   char digest[40] = "";
   char state[40]  = "";
   unsigned epoch  = 0; /* lines from before rollbacks existed carry no e= */
   char *tok, *next;

   if (!text || strncmp(text, KSYNC_PREFIX, sizeof(KSYNC_PREFIX) - 1) != 0)
      return false;

   memset(&fp, 0, sizeof(fp));
   strlcpy(fp.nick, nick ? nick : "?", sizeof(fp.nick));
   strlcpy(buf, text + sizeof(KSYNC_PREFIX) - 1, sizeof(buf));

   for (tok = buf; tok && *tok; tok = next)
   {
      if ((next = strchr(tok, ' ')))
         *next++ = '\0';
      if (tok[0] == '\0' || tok[1] != '=')
         continue;

      switch (tok[0])
      {
         case 'v': strlcpy(fp.proto,   tok + 2, sizeof(fp.proto));   break;
         case 'n': strlcpy(nonce,      tok + 2, sizeof(nonce));      break;
         case 'c': strlcpy(fp.core,    tok + 2, sizeof(fp.core));    break;
         case 'i': strlcpy(fp.content, tok + 2, sizeof(fp.content)); break;
         case 'b': strlcpy(fp.bios,    tok + 2, sizeof(fp.bios));    break;
         case 'o': strlcpy(fp.opts,    tok + 2, sizeof(fp.opts));    break;
         case 'm': strlcpy(fp.memcard, tok + 2, sizeof(fp.memcard)); break;
         case 'd': strlcpy(digest,     tok + 2, sizeof(digest));     break;
         case 's': strlcpy(state,      tok + 2, sizeof(state));      break;
         case 'e': epoch = (unsigned)strtoul(tok + 2, NULL, 10);     break;
         default:  break;
      }
   }

   KSYNC_LOCK();
   if (string_is_equal(nonce, ksync_nonce))
      /* Our own line echoed back by the server - just learn our nick. */
      strlcpy(ksync_own_nick, fp.nick, sizeof(ksync_own_nick));
   else if ((peer = ksync_peer_for(fp.nick)))
   {
      if (digest[0])
         ksync_store_hash(peer->digests, KSYNC_DIGEST_HISTORY, digest, epoch);
      else if (state[0])
         ksync_store_hash(peer->states, KSYNC_SNAPSHOTS, state, epoch);
      else
      {
         /* A re-send of the same fingerprint was already reported once. */
         peer->compared = peer->has_fp && peer->compared
            && ksync_same_fingerprint(&peer->fp, &fp);
         peer->fp       = fp;
         peer->has_fp   = true;
      }
   }
   KSYNC_UNLOCK();

   return true; /* never shown as normal chat */
}

/* "?" (couldn't tell), "-" (not applicable) or empty on either side means
   there is nothing to compare for that field. */
static bool ksync_field_differs(const char *a, const char *b)
{
   if (     string_is_empty(a) || string_is_empty(b)
         || string_is_equal(a, "?") || string_is_equal(b, "?")
         || string_is_equal(a, "-") || string_is_equal(b, "-"))
      return false;
   return !string_is_equal(a, b);
}

/* "off" vs "on:..." always differs; two "on:" only when both cards could
   be hashed ("on:?" = the core keeps card 1 in a file of its own). */
static bool ksync_memcard_differs(const char *a, const char *b)
{
   if (string_is_empty(a) || string_is_empty(b)) /* peer predates m= */
      return false;
   if (     !strncmp(a, "on:", 3) && !strncmp(b, "on:", 3)
         && (string_is_equal(a + 3, "?") || string_is_equal(b + 3, "?")))
      return false;
   return !string_is_equal(a, b);
}

static void ksync_append(char *out, size_t len, const char *what)
{
   if (out[0])
      strlcat(out, ", ", len);
   strlcat(out, what, len);
}

/* Fills `msg` with the on-screen text for one remote fingerprint. */
static bool ksync_compare(const ksync_fingerprint_t *r, char *msg, size_t len,
      bool *is_warning)
{
   char diff[160] = "";

   if (!string_is_equal(r->proto, ksync_local.proto))
   {
      snprintf(msg, len, "Sincronia: %s usa outra versão da verificação "
            "(RetroArch diferente) - confiram as versões.", r->nick);
      *is_warning = true;
      return true;
   }

   if (ksync_field_differs(ksync_local.content, r->content))
      ksync_append(diff, sizeof(diff), "arquivo do jogo (ISO)");
   if (ksync_field_differs(ksync_local.core, r->core))
      ksync_append(diff, sizeof(diff), "versão do core");
   if (ksync_field_differs(ksync_local.bios, r->bios))
      ksync_append(diff, sizeof(diff), "BIOS");
   if (ksync_field_differs(ksync_local.opts, r->opts))
      ksync_append(diff, sizeof(diff), "configurações do RetroArch");
   if (ksync_memcard_differs(ksync_local.memcard, r->memcard))
      ksync_append(diff, sizeof(diff), "memory card");

   if (diff[0])
   {
      snprintf(msg, len, "ATENÇÃO: risco de DESYNC com %s - diferente: %s.",
            r->nick, diff);
      *is_warning = true;
   }
   else
   {
      snprintf(msg, len, "Sincronia verificada com %s: mesmo jogo, core, "
            "BIOS e configurações.", r->nick);
      *is_warning = false;
   }
   return true;
}

/* A restore point only counts as a safe place to go back to when every peer
   verifiably had the same state there: its serialized bytes hashed equal
   everywhere, or - if this core's savestates turn out not to be byte-stable
   across machines - the RAM windows right before and right after it
   matched everywhere. */
static bool ksync_snapshot_usable(const ksync_snapshot_t *s)
{
   return s->valid && s->epoch == ksync_epoch
      && (s->verified_blob || (s->ram_before && s->ram_after));
}

/* RAM window `window` (current epoch) just matched on every peer - credit
   the restore point(s) on either side of it. Caller holds the lock. */
static void ksync_window_verified(unsigned window)
{
   const unsigned per = KSYNC_SNAPSHOT_FRAMES / KSYNC_DIGEST_FRAMES;
   int i;

   for (i = 0; i < KSYNC_SNAPSHOTS; i++)
   {
      ksync_snapshot_t *s = &ksync_snapshots[i];
      bool was_usable;

      if (!s->valid || s->epoch != ksync_epoch)
         continue;
      was_usable = ksync_snapshot_usable(s);
      if (s->id > 0 && window == s->id * per - 1)
         s->ram_before = true;
      if (window == s->id * per)
         s->ram_after = true;
      if (!was_usable && ksync_snapshot_usable(s) && !s->verified_blob)
         ksync_snaps_ram_ok++;
   }
}

/* Compares a peer's pending RAM digests with ours. Caller holds the lock.
   Fills `osd`/`log` (and returns true) only for the first mismatch. */
static bool ksync_compare_digests(ksync_peer_t *peer, char *osd, size_t osd_len,
      char *log, size_t log_len)
{
   bool report = false;
   int k;

   for (k = 0; k < KSYNC_DIGEST_HISTORY; k++)
   {
      ksync_digest_t *theirs = &peer->digests[k];
      ksync_digest_t *ours;

      if (!theirs->valid)
         continue;

      /* From before the last rollback: numbering restarted since, so it
         says nothing about the current timeline. From a newer epoch than
         ours can't happen (resumes run on the same frame everywhere) -
         just keep it until we catch up. */
      if (theirs->epoch != ksync_epoch)
      {
         if (theirs->epoch < ksync_epoch)
            theirs->valid = false;
         continue;
      }

      ours = &ksync_local_digests[theirs->window % KSYNC_DIGEST_HISTORY];

      if (!ours->valid || ours->window < theirs->window)
      {
         /* We haven't finished that window yet (they run ahead of us, or
            our slot still holds an older one) - unless we're so far past it
            that ours was already overwritten. */
         if (     ksync_ram_window != (unsigned)-1
               && ksync_ram_window >= theirs->window + KSYNC_DIGEST_HISTORY)
            theirs->valid = false;
         continue;
      }

      theirs->valid = false; /* consumed either way */
      if (ours->window != theirs->window)
         continue;           /* ours was overwritten by a newer window */

      if (string_is_equal(ours->hash, theirs->hash))
      {
         peer->digests_matched++;
         if (++ours->peers_matched == (unsigned)(ksync_num_players - 1))
            ksync_window_verified(ours->window);
         continue;
      }

      peer->digests_mismatched++;
      if (!peer->desync_reported)
      {
         unsigned first = theirs->window * KSYNC_DIGEST_FRAMES;
         unsigned secs  = first / 60;

         peer->desync_reported = true;
         report                = true;
         snprintf(osd, osd_len, "DESYNC DETECTADO com %s (por volta de "
               "%u:%02u de partida) - o jogo pode ter divergido, confirmem e reiniciem a partida.",
               peer->fp.nick, secs / 60, secs % 60);
         snprintf(log, log_len, "DESYNC entre %s e %s: janela %u (frames "
               "%u-%u, ~%u:%02u) RAM local %s x remota %s - %u janelas iguais "
               "antes disso.",
               ksync_own_nick[0] ? ksync_own_nick : "(eu)", peer->fp.nick,
               theirs->window, first, first + KSYNC_DIGEST_FRAMES - 1,
               secs / 60, secs % 60, ours->hash, theirs->hash,
               peer->digests_matched);
      }
   }

   return report;
}

/* 5./6. - defined further down, next to the rest of the rollback code. */
static int  ksync_compare_states(ksync_peer_t *peer, char (*log)[320], int max_logs);
static bool ksync_on_desync(void);
static void ksync_show_prompt(void);
static void ksync_rollback_tick(void);

void kailleraSyncFrameTick(void)
{
   char send_msg[sizeof(ksync_local_msg)];
   char osd[KSYNC_MAX_REMOTES * 2 + 1][256];
   char log[KSYNC_MAX_REMOTES * 2 + 1][320];
   bool osd_warning[KSYNC_MAX_REMOTES * 2 + 1];
   char state_log[KSYNC_STATE_MISMATCH_LOGS][320];
   int  state_log_count = 0;
   int  desync_idx      = -1;
   int  osd_count = 0;
   int  i;

   /* Unlocked peek - cheap early out for every frame of a game where the
      handshake doesn't apply (solo, playback) or is already settled. */
   if (!ksync_send_enabled)
      return;

   send_msg[0] = '\0';

   KSYNC_LOCK();
   if (ksync_send_enabled && ksync_content_ready)
   {
      int seen = 0;

      if (!ksync_sent)
      {
         ksync_describe_bios(ksync_local.bios, sizeof(ksync_local.bios));
         snprintf(ksync_local_msg, sizeof(ksync_local_msg),
               KSYNC_PREFIX "v=%s n=%s c=%s i=%s b=%s o=%s m=%s",
               ksync_local.proto, ksync_nonce, ksync_local.core,
               ksync_local.content, ksync_local.bios, ksync_local.opts,
               ksync_local.memcard);
         strlcpy(send_msg, ksync_local_msg, sizeof(send_msg));
         ksync_sent = true;
         RARCH_LOG("[Kaillera sync]: %s\n", ksync_local_msg);
      }
      else
         ksync_frames_since_sent++;

      for (i = 0; i < KSYNC_MAX_REMOTES; i++)
      {
         ksync_peer_t *peer = &ksync_peers[i];
         if (!peer->has_fp)
            continue;
         seen++;
         if (!peer->compared && osd_count < KSYNC_MAX_REMOTES)
         {
            peer->compared = true;
            if (ksync_compare(&peer->fp, osd[osd_count],
                     sizeof(osd[0]), &osd_warning[osd_count]))
            {
               strlcpy(log[osd_count], osd[osd_count], sizeof(log[0]));
               osd_count++;
            }
         }
      }

      if (seen < ksync_num_players - 1)
      {
         if (!ksync_resent && ksync_frames_since_sent >= KSYNC_RESEND_FRAMES)
         {
            strlcpy(send_msg, ksync_local_msg, sizeof(send_msg));
            ksync_resent = true;
         }
         else if (!ksync_missing_warned
               && ksync_frames_since_sent >= KSYNC_MISSING_FRAMES)
         {
            snprintf(osd[osd_count], sizeof(osd[0]),
                  "ATENÇÃO: %d jogador(es) não enviaram a verificação de "
                  "sincronia (RetroArch desatualizado?).",
                  ksync_num_players - 1 - seen);
            strlcpy(log[osd_count], osd[osd_count], sizeof(log[0]));
            osd_warning[osd_count++] = true;
            ksync_missing_warned     = true;
         }
      }
   }

   if (ksync_digest_enabled)
   {
      for (i = 0; i < KSYNC_MAX_REMOTES; i++)
      {
         if (!ksync_peers[i].used)
            continue;
         if (     osd_count < KSYNC_MAX_REMOTES * 2 + 1
               && ksync_compare_digests(&ksync_peers[i],
                     osd[osd_count], sizeof(osd[0]),
                     log[osd_count], sizeof(log[0])))
         {
            if (desync_idx < 0)
               desync_idx = osd_count;
            osd_warning[osd_count++] = true;
         }
         state_log_count += ksync_compare_states(&ksync_peers[i],
               state_log + state_log_count,
               KSYNC_STATE_MISMATCH_LOGS - state_log_count);
      }
   }
   KSYNC_UNLOCK();

   for (i = 0; i < state_log_count; i++)
      ksync_log("%s", state_log[i]);

   /* Outside the lock: the DLL may call straight back into our chat hook. */
   if (send_msg[0])
      kailleraChatSendExternal(send_msg);

   for (i = 0; i < osd_count; i++)
   {
      if (osd_warning[i])
         RARCH_WARN("[Kaillera sync]: %s\n", log[i]);
      else
         RARCH_LOG("[Kaillera sync]: %s\n", log[i]);
      ksync_log("%s", log[i]);

      /* The first desync: offer a rollback instead, when there is a safe
         point to go back to (or say nothing new, mid-rollback). */
      if (i == desync_idx && ksync_on_desync())
      {
         if (ksync_r_state == KSYNC_R_PROMPT && !ksync_prompt_shown)
            ksync_show_prompt();
         continue;
      }

      runloop_msg_queue_push(osd[i], osd_warning[i] ? 2 : 1,
            osd_warning[i] ? 15 * 60 : 5 * 60, osd_warning[i], NULL,
            MESSAGE_QUEUE_ICON_DEFAULT,
            osd_warning[i] ? MESSAGE_QUEUE_CATEGORY_WARNING
                           : MESSAGE_QUEUE_CATEGORY_SUCCESS);
   }

   ksync_rollback_tick();
}

/* ------------------------------------------------------------------------ */
/* 4. Desync detector - periodic digest of the core's system RAM            */
/* ------------------------------------------------------------------------ */

/* FNV-1a over 64-bit words: each step is a bijection of the running value,
   so any single changed word always changes the digest. ~1us for a 7KB
   slice. */
static uint64_t ksync_hash_ram(uint64_t h, const uint8_t *p, size_t len)
{
   static const uint64_t prime = 0x100000001B3ULL;

   while (len >= 8)
   {
      uint64_t w;
      memcpy(&w, p, 8);
      h    = (h ^ w) * prime;
      p   += 8;
      len -= 8;
   }
   while (len--)
      h = (h ^ *p++) * prime;
   return h;
}

/* ------------------------------------------------------------------------ */
/* 5. Restore points                                                        */
/* ------------------------------------------------------------------------ */

/* Serializes the core into restore point `id` (current epoch) and tells the
   others its hash. Runs on the same frame on every machine - right after
   retro_run(), before any networked command - so while the game is in sync
   every machine's copy should be identical. Main thread. */
static void ksync_take_snapshot(unsigned id)
{
   const unsigned per  = KSYNC_SNAPSHOT_FRAMES / KSYNC_DIGEST_FRAMES;
   ksync_snapshot_t *s = &ksync_snapshots[id % KSYNC_SNAPSHOTS];
   size_t size         = core_serialize_size();
   retro_time_t t0     = cpu_features_get_time_usec();
   retro_ctx_serialize_info_t info;
   const ksync_digest_t *before;
   uint64_t h;
   char msg[96];

   if (!size)
      return;

   if (!s->data || s->size != size)
   {
      free(s->data);
      s->data = malloc(size);
      s->size = s->data ? size : 0;
   }
   s->valid = false;
   if (!s->data)
      return;

   info.data       = s->data;
   info.data_const = NULL;
   info.size       = size;
   if (!core_serialize(&info))
      return;

   h = ksync_hash_ram(0xCBF29CE484222325ULL, (const uint8_t*)s->data, size);

   KSYNC_LOCK();
   s->id            = id;
   s->epoch         = ksync_epoch;
   s->peers_same    = 0;
   s->peers_diff    = 0;
   s->verified_blob = false;
   s->ram_after     = false;
   /* The RAM window right before this point may already be settled. */
   before           = &ksync_local_digests[(id * per - 1) % KSYNC_DIGEST_HISTORY];
   s->ram_before    = id > 0 && before->valid && before->epoch == ksync_epoch
      && before->window == id * per - 1
      && before->peers_matched == (unsigned)(ksync_num_players - 1);
   snprintf(s->hash, sizeof(s->hash), "%08X%08X",
         (unsigned)(h >> 32), (unsigned)h);
   s->valid = true;
   snprintf(msg, sizeof(msg), KSYNC_PREFIX "v=%s n=%s e=%u s=%u:%s",
         KSYNC_PROTOCOL, ksync_nonce, ksync_epoch, id, s->hash);
   KSYNC_UNLOCK();

   ksync_snapshot_last_id = id;
   ksync_snaps_taken++;
   ksync_snaps_usec += cpu_features_get_time_usec() - t0;

   kailleraChatSendExternal(msg);
}

/* Compares a peer's restore point hashes with ours. Caller holds the lock.
   Writes up to `max_logs` detailed mismatch lines; returns how many. */
static int ksync_compare_states(ksync_peer_t *peer, char (*log)[320], int max_logs)
{
   int k, n = 0;

   for (k = 0; k < KSYNC_SNAPSHOTS; k++)
   {
      ksync_digest_t *theirs = &peer->states[k];
      ksync_snapshot_t *ours;

      if (!theirs->valid)
         continue;
      if (theirs->epoch != ksync_epoch)
      {
         if (theirs->epoch < ksync_epoch)
            theirs->valid = false;
         continue;
      }

      ours = &ksync_snapshots[theirs->window % KSYNC_SNAPSHOTS];
      if (!ours->valid || ours->epoch != ksync_epoch || ours->id < theirs->window)
      {
         /* Not taken here yet (they're ahead of us) - unless we've stopped
            taking them, in which case it never will be. */
         if (ksync_capture_stopped
               || ksync_snapshot_last_id >= theirs->window + KSYNC_SNAPSHOTS)
            theirs->valid = false;
         continue;
      }

      theirs->valid = false; /* consumed either way */
      if (ours->id != theirs->window)
         continue;           /* ours was already replaced by a newer point */

      if (string_is_equal(ours->hash, theirs->hash))
      {
         if (     ++ours->peers_same == (unsigned)(ksync_num_players - 1)
               && !ours->peers_diff)
         {
            ours->verified_blob = true;
            ksync_snaps_blob_ok++;
         }
         continue;
      }

      if (ours->peers_diff++ == 0)
         ksync_snaps_diff++;
      if (n < max_logs && ksync_state_mismatch_logs < KSYNC_STATE_MISMATCH_LOGS)
      {
         unsigned secs = ours->id * KSYNC_SNAPSHOT_FRAMES / 60;
         ksync_state_mismatch_logs++;
         snprintf(log[n++], 320, "Ponto de volta %u (~%u:%02u%s): state "
               "diferente do de %s (%s x %s).", ours->id, secs / 60, secs % 60,
               ksync_epoch ? " após a última volta" : " de partida",
               peer->fp.nick, ours->hash, theirs->hash);
      }
   }

   return n;
}

/* ------------------------------------------------------------------------ */
/* 4. (cont.) Placar ao vivo - the match, read from the game's RAM           */
/* ------------------------------------------------------------------------ */

/* Where each Winning Eleven engine keeps the match in the PS1's RAM - found
   by replaying .krec matches against the screen; kaillera-client's
   tools/score_probe/README.md has the maps and live_sim.py the same logic
   as below in Python (keep them alike). The game is told by its executable
   name, which the kernel keeps at WE_SERIAL; patches (CAN 2025, Euro vs
   Copa America, BR Turbo...) keep the engine's. Side 0 = the game's LOCAL
   team. */
#define WE_RAM_SIZE     0x200000 /* PS1 main RAM: pcsx_rearmed only */
#define WE_SCRATCH      0x1F800000u /* the PS1 scratchpad (1 KB), in the core's memory map */
#define WE_SCRATCH_SIZE 0x400
#define WE_SERIAL       0x9E19   /* "SLPM_870.56" */
#define WE_TEAMS        128

/* 16 bytes of the engine's own code at a fixed RAM address: patches that
   rename the executable (World Cup 26 "WC26_5MX2") keep the engine's code. */
typedef struct
{
   size_t  addr;
   uint8_t bytes[16];
} ksync_we_sig;

typedef struct
{
   const char *serial;
   const char *label;
   ksync_we_sig sig[2];   /* the engine, when the executable name is another */
   bool     scratch;      /* team_id, period, clock and score are in the scratchpad (WE3) */
   size_t   team_id;      /* team slot, LOCAL then VISITA */
   size_t   period;       /* 0 = 1st half, 1 = 2nd */
   size_t   clock;        /* u16: frames left in the half (the half's length at kickoff) */
   size_t   in_play;      /* 1 while the ball rolls, 0 at half time/the end (0 = engine without it) */
   size_t   score;        /* goals, LOCAL then VISITA */
   size_t   goals[2];     /* 32 x {scorer, assist (FF none), minute[, 0]}, minute FF = end;
                             scorer FF = own goal (the player isn't kept) */
   unsigned goal_size;    /* bytes per goal: 4 (WE3: 3) */
   size_t   roster[2];    /* player names, roster_len x name_len bytes */
   size_t   roster_team;  /* WE3: one roster per team at roster[0] + team x this, not per side */
   unsigned roster_len;
   unsigned name_len;
   size_t   pads;         /* pads 1-4: 0 LOCAL, 1 VISITA, FF none (0 = no table: 1P LOCAL, 2P VISITA) */
   /* Team select screen only (the match overwrites them): a pointer per team
      to its name and either a pointer per team to its 3-letter code or the
      codes themselves, team 0 last, 4 bytes each ending at sel_codes. */
   size_t   sel_names;
   size_t   sel_codes;
   bool     code_ptrs;
   bool     mixed_case;   /* team names in mixed case ("Portugal") */
   /* WE3: the clock is set at the formation screen and only runs from the
      kickoff on; at the end the score goes back to 0 with the period, and in
      the menu after the final whistle (team still on screen) the teams can
      already change. */
   bool     kick_on_run;  /* kickoff = the clock starting to run, after the jump */
   bool     end_resets;   /* the final = the sample before the period goes back to 0; teams kept from the kickoff */
   size_t   extra;        /* u16: the time added after the clock reaches 0, counting down (0 = none) */
   /* WE2002: the match's phase (0/1 halves, 2/3 extra time, 4 penalties),
      the penalty shootout's score (LOCAL, VISITA) and the kicker (FF once
      the shootout is over) - 0 = engine without them (not seen yet). */
   size_t   phase;
   size_t   pens;
   size_t   kicker;
   /* WE2002: the last player to touch the ball - a field slot, 1-11 one
      side and 12-22 the other (swapped each half), the roster index being
      (slot - 1) % 11 - and the set piece being taken (2 = a penalty kick,
      until the kick's goal or the play after it): an own goal's player and
      the penalty goals (0 = engine without them). */
   size_t   touch;
   size_t   setpiece;
   /* WE2002: the match's state - 1 ball in play, 7 goal, 11 celebration,
      0 kickoff setup, 10 the referee ended the period (half time, the end
      of the match or of extra time, a golden goal, the shootout's end) -
      0 = engine without it. */
   size_t   state;
} ksync_we_map;
#define WE_STATE_OVER 10

static const ksync_we_map ksync_we_maps[] = {
   { "SLPM_870.56", "WE2002",
     { { 0x1701C, { 0x07, 0x00, 0x40, 0x12, 0x21, 0x20, 0xC0, 0x02, 0x21, 0x98, 0x40, 0x02, 0x21, 0x90, 0x00, 0x00 } },
       { 0x1D038, { 0xB0, 0x70, 0x00, 0x0C, 0x20, 0x08, 0x06, 0x24, 0x0C, 0x00, 0x40, 0x14, 0x01, 0x00, 0x03, 0x24 } } },
     false, 0x75564, 0x75566, 0x7591C, 0, 0x75702,
     { 0x755C4, 0x75644 }, 4, { 0x64570, 0x646E0 }, 0, 23, 16, 0x758D9,
     0xBF0E8, 0x800DC, false, false, false, false, 0, 0x7570E, 0x7570C, 0x7571C, 0x7B841, 0x7B499, 0x75782 },
   { "SLPM_862.91", "WE4",
     { { 0x17034, { 0x60, 0x00, 0x09, 0x24, 0x20, 0x00, 0x08, 0x24, 0x1E, 0x00, 0x07, 0x24, 0x21, 0x28, 0x27, 0x01 } },
       { 0x1D03C, { 0x01, 0x00, 0x84, 0x34, 0x00, 0x20, 0x05, 0x24, 0x00, 0x20, 0x06, 0x24, 0x07, 0x80, 0x01, 0x3C } } },
     false, 0x750FB, 0x750FD, 0x754A8, 0x754B9, 0x75293,
     { 0x75157, 0x751D7 }, 4, { 0x7490C, 0x749E8 }, 0, 22, 10, 0x75469,
     0xA67C0, 0xA6AC0, true, true, false, false, 0, 0, 0, 0, 0, 0, 0 },
   /* WE3 Final Version - its patches too ("Camera torre TV mode"); the
      scratchpad offsets are from WE_SCRATCH. */
   { "SLPM_861.62", "WE3",
     { { 0x1714C, { 0x80, 0x1F, 0x02, 0x3C, 0x98, 0x02, 0x42, 0x90, 0x80, 0x1F, 0x01, 0x3C, 0x9A, 0x02, 0x20, 0xA0 } },
       { 0x1D19C, { 0x1C, 0x00, 0xB3, 0xAF, 0x4C, 0x00, 0xB3, 0x93, 0x11, 0x80, 0x07, 0x3C, 0x78, 0xC1, 0xE7, 0x24 } } },
     true, 0x2DD, 0x2DF, 0x2C4, 0, 0x2D1,
     { 0x107FE5, 0x108045 }, 3, { 0xC8504, 0 }, 0xB0, 22, 8, 0,
     0x1D1B48, 0x189B58, false, true, true, true, 0x2C8, 0, 0, 0, 0, 0, 0 },
};

#define KSYNC_LIVE_SAMPLE_FRAMES    30   /* read twice a second            */
#define KSYNC_LIVE_HEARTBEAT_FRAMES 900  /* and send at least every 15 s   */
#define KSYNC_LIVE_MIN_KICKOFF      1800 /* a half of 30 s or more         */
#define KSYNC_LIVE_OVER_FRAMES      600  /* WE3: 10 s with no time left = the end */
#define KSYNC_LIVE_JSON_LEN         8192

static struct
{
   const ksync_we_map *map; /* the engine, once recognized (the game doesn't change mid-session) */
   bool     active;      /* a match is being played (kickoff seen, or joined mid-match) */
   bool     have_prev;
   unsigned kickoff;     /* frame of the 1st half's kickoff - the same on every PC; 0 = joined mid-match */
   unsigned half;        /* the half's length in frames (the clock at kickoff) */
   unsigned armed;       /* kick_on_run: the clock set for a kickoff (0 = none) */
   unsigned team[2];     /* end_resets: the teams at the kickoff */
   unsigned over_since;  /* extra: frame the 2nd half's time ran out (0 = it hasn't) */
   bool     extra;       /* in extra time (WE2002 plays it as periods 0 and 1 again) */
   unsigned prev_period;
   unsigned prev_clock;
   unsigned prev_in_play;
   uint32_t sent_sig;    /* what the last state sent had (without the clock) */
   unsigned sent_frame;
   char     last_json[KSYNC_LIVE_JSON_LEN]; /* end_resets: the last sample's state */
   /* The teams' names and codes, read from the team select screen (the
      last time it was up this game) - the match itself only has the slot. */
   bool     have_names;
   char     names[WE_TEAMS][17];
   char     codes[WE_TEAMS][4];
   /* map->touch: watched every frame (ksync_live_goal_watch()) - the goal
      record doesn't say who scored an own goal nor that it was a penalty. */
   bool     watching;
   unsigned last_touch;     /* the last player to touch the ball (slot 1-22) */
   unsigned pen_frame;      /* last frame a penalty kick was being taken (0 = none) */
   unsigned goal_count[2];  /* goals already seen, per side */
   uint8_t  goal_og[2][32]; /* own goal: the scorer's roster index + 1 (0 = unknown) */
   bool     goal_pen[2][32];
   uint32_t so_state;       /* WE2002 shootout: pens + state byte, and since when */
   unsigned so_since;
   unsigned et_goals;       /* WE2002 extra time: goals seen (both teams) ... */
   unsigned gg_clock;       /* ... and the clock + 1 when the last one came in, while it hasn't
                               moved since (a golden goal ends the match there) - 0 = none */
   bool     decided;        /* the last sample: a decided shootout, a golden goal or a period the
                               referee ended that may be the match's end (map->state) */
   unsigned prev_phase;
   unsigned prev_score[2];
   unsigned over_frame;     /* map->state: the frame a tied 2nd half / extra time ended (0 = not) */
} ksync_live;

static void ksync_live_reset(void)
{
   memset(&ksync_live, 0, sizeof(ksync_live));
}

/* A goal right after a penalty kick (the set piece is still "penalty" until
   the goal's frame) is a penalty goal. */
#define KSYNC_LIVE_PEN_FRAMES 30
/* WE2002's shootout: its state at a miss's value this long, score decided = over (20 s). */
#define KSYNC_LIVE_SHOOTOUT_IDLE 1200
/* WE2002: a tied 2nd half / extra time the referee ended - extra time or the
   shootout starts within ~13 s; none this long (25 s) = the match ends a draw. */
#define KSYNC_LIVE_DRAW_FRAMES 1500

/* Every frame of a match (map->touch only): who touched the ball last and
   whether a penalty kick is being taken, so a goal that just showed up in
   the records gets its own-goal scorer and its penalty mark - the sample
   twice a second would be too late for either. */
static void ksync_live_goal_watch(const uint8_t *ram, unsigned frame)
{
   const ksync_we_map *map = ksync_live.map;
   unsigned side, i, n;

   if (!ksync_live.active || !map || !map->touch)
      return;
   if (ram[map->touch] >= 1 && ram[map->touch] <= 22)
      ksync_live.last_touch = ram[map->touch];
   if (map->setpiece && ram[map->setpiece] == 2)
      ksync_live.pen_frame = frame;
   for (side = 0; side < 2; side++)
   {
      const uint8_t *goals = ram + map->goals[side];
      for (n = 0; n < 32 && goals[map->goal_size * n + 2] != 0xFF; n++)
         ;
      /* Joined mid-match: the goals already there stay as they are. */
      for (i = ksync_live.watching ? ksync_live.goal_count[side] : n; i < n; i++)
      {
         bool own = goals[map->goal_size * i] == 0xFF;
         ksync_live.goal_og[side][i]  = own && ksync_live.last_touch
               ? (uint8_t)((ksync_live.last_touch - 1) % 11 + 1) : 0;
         ksync_live.goal_pen[side][i] = !own && ksync_live.pen_frame
               && frame - ksync_live.pen_frame <= KSYNC_LIVE_PEN_FRAMES;
      }
      for (i = n; i < ksync_live.goal_count[side] && i < 32; i++)
      {
         ksync_live.goal_og[side][i]  = 0;
         ksync_live.goal_pen[side][i] = false;
      }
      ksync_live.goal_count[side] = n;
   }
   ksync_live.watching = true;
}

/* The engine running - from the executable name the kernel keeps or, for
   patches that renamed it, from two pieces of the engine's code - kept for
   the session once found. NULL for any other game (or WE3 without the
   scratchpad). */
static const ksync_we_map *ksync_live_map(const uint8_t *ram, const uint8_t *scratch)
{
   size_t i;

   for (i = 0; !ksync_live.map && i < sizeof(ksync_we_maps) / sizeof(ksync_we_maps[0]); i++)
   {
      const ksync_we_map *m = &ksync_we_maps[i];
      if (memcmp(ram + WE_SERIAL, m->serial, 11) == 0
            || (memcmp(ram + m->sig[0].addr, m->sig[0].bytes, 16) == 0
               && memcmp(ram + m->sig[1].addr, m->sig[1].bytes, 16) == 0))
         ksync_live.map = m;
   }
   return ksync_live.map && ksync_live.map->scratch && !scratch ? NULL : ksync_live.map;
}

/* The PS1 scratchpad from the core's memory map (pcsx_rearmed publishes it)
   - NULL if it isn't there. */
static const uint8_t *ksync_live_scratchpad(void)
{
   const rarch_memory_map_t *mmaps = &runloop_state_get_ptr()->system.mmaps;
   unsigned i;

   for (i = 0; i < mmaps->num_descriptors; i++)
   {
      const struct retro_memory_descriptor *d = &mmaps->descriptors[i].core;
      if (d->start == WE_SCRATCH && d->ptr && d->len >= WE_SCRATCH_SIZE)
         return (const uint8_t*)d->ptr + d->offset;
   }
   return NULL;
}

/* A team of the match: from the kickoff where the end resets them
   (end_resets), from the RAM (st = the RAM or the scratchpad) otherwise. */
static unsigned ksync_live_team(const uint8_t *st, const ksync_we_map *map, unsigned side)
{
   return map->end_resets ? ksync_live.team[side] : st[map->team_id + side];
}

static unsigned ksync_live_u16(const uint8_t *ram, size_t addr)
{
   return ram[addr] | (ram[addr + 1] << 8);
}

static uint32_t ksync_live_u32(const uint8_t *ram, size_t addr)
{
   return ram[addr] | (ram[addr + 1] << 8) | (ram[addr + 2] << 16) | ((uint32_t)ram[addr + 3] << 24);
}

/* A NUL-terminated (or cap-long) name of printable ASCII from the RAM (""
   otherwise), without the characters JSON would need escaped. */
static void ksync_live_name(const uint8_t *ram, size_t addr, size_t cap,
      char *out, size_t out_cap)
{
   size_t i, n = 0;

   out[0] = '\0';
   for (i = 0; i < cap && addr + i < WE_RAM_SIZE && ram[addr + i]; i++)
   {
      unsigned char c = ram[addr + i];
      if (c < 32 || c > 126)
      {
         out[0] = '\0';
         return;
      }
      if (c != '"' && c != '\\' && n + 1 < out_cap)
         out[n++] = (char)c;
   }
   out[n] = '\0';
}

/* A team name as the select screen keeps it: 1 to cap-1 printable
   characters, no lowercase where the engine has none (anything else is
   some other screen's data). */
static bool ksync_live_plain(const char *s, size_t cap, bool mixed_case)
{
   size_t i, len = strlen(s);

   if (len < 1 || len >= cap)
      return false;
   for (i = 0; i < len && !mixed_case; i++)
      if (islower((unsigned char)s[i]))
         return false;
   return true;
}

/* A string the RAM points to at addr (0 when the pointer isn't in RAM). */
static size_t ksync_live_ptr(const uint8_t *ram, size_t addr)
{
   uint32_t ptr = ksync_live_u32(ram, addr);
   return ptr >= 0x80000000u && ptr < 0x80000000u + WE_RAM_SIZE ? ptr & 0x1FFFFF : 0;
}

/* While the team select screen is up: every team's name and code, kept for
   the match (which overwrites them). Taken only when the whole table reads
   as one - from team 0 on, past both teams picked now. */
static void ksync_live_capture_names(const uint8_t *ram, const uint8_t *st,
      const ksync_we_map *map)
{
   static char names[WE_TEAMS][17], codes[WE_TEAMS][4];
   unsigned id, count = 0, need = st[map->team_id];

   if (st[map->team_id + 1] > need)
      need = st[map->team_id + 1];
   for (id = 0; id < WE_TEAMS; id++)
   {
      size_t name = ksync_live_ptr(ram, map->sel_names + 4 * id);
      size_t code = map->code_ptrs ? ksync_live_ptr(ram, map->sel_codes + 4 * id)
                  : map->sel_codes - 4 * id;
      if (!name || !code || (!map->code_ptrs && ram[code + 3] != 0))
         break;
      ksync_live_name(ram, name, 24, names[id], sizeof(names[id]));
      ksync_live_name(ram, code, 4, codes[id], sizeof(codes[id]));
      if (!ksync_live_plain(names[id], sizeof(names[id]), map->mixed_case) || strlen(codes[id]) != 3)
         break;
      count++;
   }
   if (count < 8 || count <= need)
      return;
   memcpy(ksync_live.names, names, sizeof(names));
   memcpy(ksync_live.codes, codes, sizeof(codes));
   for (id = count; id < WE_TEAMS; id++)
      ksync_live.names[id][0] = ksync_live.codes[id][0] = '\0';
   ksync_live.have_names = true;
}

/* Does the RAM hold a match of this engine? (Its menus, with leftovers
   there, fail one of these.) st = where team_id/period/clock/score are. */
static bool ksync_live_valid(const uint8_t *ram, const uint8_t *st,
      const ksync_we_map *map, unsigned *period, unsigned *clock)
{
   unsigned side, i, gs = map->goal_size;

   *period = st[map->period];
   *clock  = ksync_live_u16(st, map->clock);
   if (*period > 4)
      return false;
   for (i = 0; i < 4 && map->pads; i++)
      if (ram[map->pads + i] != 0 && ram[map->pads + i] != 1 && ram[map->pads + i] != 0xFF)
         return false;
   for (side = 0; side < 2; side++)
   {
      const uint8_t *goals = ram + map->goals[side];
      if (st[map->team_id + side] >= WE_TEAMS || st[map->score + side] > 30)
         return false;
      for (i = 0; i < 32 && goals[gs * i + 2] != 0xFF; i++)
         if ((goals[gs * i] != 0xFF && goals[gs * i] >= map->roster_len) || goals[gs * i + 2] > 200)
            return false;
   }
   return true;
}

/* What changed since the last send: everything but the clock. */
static uint32_t ksync_live_sig(const uint8_t *ram, const uint8_t *st, const ksync_we_map *map,
      unsigned clock)
{
   uint32_t h = 2166136261u;
   size_t i;
#define KSYNC_LIVE_MIX(m, a, n) for (i = 0; i < (n); i++) h = (h ^ (m)[(a) + i]) * 16777619u
   KSYNC_LIVE_MIX(st, map->period, 1);
   /* The half's clock starting to run (the card leaves "Intervalo" then) and
      the referee ending the period (it goes into "Intervalo"). */
   h = (h ^ (unsigned)(ksync_live.half && clock >= ksync_live.half)) * 16777619u;
   if (map->state)
      h = (h ^ (unsigned)(ram[map->state] == WE_STATE_OVER)) * 16777619u;
   /* An own goal's scorer / a penalty mark (ksync_live_goal_watch()). */
   KSYNC_LIVE_MIX(ksync_live.goal_og[0], 0, 32);
   KSYNC_LIVE_MIX(ksync_live.goal_og[1], 0, 32);
   if (map->pads)
      KSYNC_LIVE_MIX(ram, map->pads, 4);
   h = (h ^ ksync_live_team(st, map, 0)) * 16777619u;
   h = (h ^ ksync_live_team(st, map, 1)) * 16777619u;
   KSYNC_LIVE_MIX(st, map->score, 2);
   if (map->phase)
      KSYNC_LIVE_MIX(ram, map->phase, 1);
   if (map->pens)
      KSYNC_LIVE_MIX(ram, map->pens, 2);
   KSYNC_LIVE_MIX(ram, map->goals[0], 32 * map->goal_size);
   KSYNC_LIVE_MIX(ram, map->goals[1], 32 * map->goal_size);
#undef KSYNC_LIVE_MIX
   return h;
}

/* The state for WE Camp (wg-camp's app/live.py parse_state()). v2: the
   teams' names come from the team select screen (what the players saw);
   engine = the game's executable name. */
static bool ksync_live_json(const uint8_t *ram, const uint8_t *st, const ksync_we_map *map,
      unsigned frame, unsigned period, unsigned clock, bool final, char *out, size_t cap)
{
   static const uint8_t no_pads[4] = { 0, 1, 0xFF, 0xFF }; /* 1P LOCAL, 2P VISITA */
   const uint8_t *pads = map->pads ? ram + map->pads : no_pads;
   unsigned side, i, gs = map->goal_size;
   bool shootout = map->phase && ram[map->phase] == 4;
   /* Period sent: 0/1 halves, 2/3 extra time, 4 penalties - WE2002's phase
      itself; the others, extra time as seen (ksync_live.extra). */
   unsigned sent = final ? (shootout ? 4 : period)
                 : map->phase ? ram[map->phase] : ksync_live.extra ? period + 2 : period;
   int n = snprintf(out, cap, "{\"v\":2,\"engine\":\"%s\",\"content\":\"%s\",\"kickoff\":%u,"
         "\"frame\":%u,\"period\":%u,\"clock\":%u,\"half\":%u,\"final\":%d,"
         "\"pads\":[%u,%u,%u,%u],",
         map->serial, ksync_local.content, ksync_live.kickoff, frame, sent, clock,
         ksync_live.half, final ? 1 : 0, pads[0], pads[1], pads[2], pads[3]);
   /* The penalty shootout's score (LOCAL, VISITA), once it starts. */
   if (map->pens && n > 0 && (size_t)n < cap && (shootout || ram[map->pens] || ram[map->pens + 1]))
      n += snprintf(out + n, cap - n, "\"pens\":[%u,%u],", ram[map->pens], ram[map->pens + 1]);
   /* The referee ended the period (map->state) - "Intervalo" only from then. */
   if (map->state && n > 0 && (size_t)n < cap)
      n += snprintf(out + n, cap - n, "\"over\":%d,", ram[map->state] == WE_STATE_OVER ? 1 : 0);
   if (n > 0 && (size_t)n < cap)
      n += snprintf(out + n, cap - n, "\"teams\":[");

   for (side = 0; side < 2 && n > 0 && (size_t)n < cap; side++)
   {
      const uint8_t *goals = ram + map->goals[side];
      unsigned team        = ksync_live_team(st, map, side);
      size_t roster        = map->roster_team ? map->roster[0] + map->roster_team * team
                           : map->roster[side];
      bool known           = ksync_live.have_names && team < WE_TEAMS;

      n += snprintf(out + n, cap - n, "%s{\"id\":%u,\"name\":\"%s\",\"code\":\"%s\","
            "\"goals\":%u,\"scorers\":[", side ? "," : "", team,
            known ? ksync_live.names[team] : "", known ? ksync_live.codes[team] : "",
            st[map->score + side]);
      for (i = 0; i < 32 && goals[gs * i + 2] != 0xFF && n > 0 && (size_t)n < cap; i++)
      {
         char scorer[20], assist[20];
         const char *kind = "";
         assist[0] = '\0';
         /* Own goal: in the list of the team it counted for, without the
            player - who touched the ball last, from the other side's roster
            (ksync_live_goal_watch()), when it was seen, goes in the assist's
            place (an older WE Camp shows it as the assist). */
         if (goals[gs * i] == 0xFF)
         {
            strlcpy(scorer, "Gol contra", sizeof(scorer));
            if (ksync_live.goal_og[side][i] && !map->roster_team)
            {
               ksync_live_name(ram, map->roster[!side] + map->name_len * (ksync_live.goal_og[side][i] - 1),
                     map->name_len, assist, sizeof(assist));
               kind = "og";
            }
         }
         else
         {
            ksync_live_name(ram, roster + map->name_len * goals[gs * i], map->name_len, scorer, sizeof(scorer));
            if (ksync_live.goal_pen[side][i])
               kind = "pen";
            if (goals[gs * i + 1] < map->roster_len)
               ksync_live_name(ram, roster + map->name_len * goals[gs * i + 1], map->name_len, assist, sizeof(assist));
         }
         /* v2's 4th item: "og" (own goal, its player in the assist's place) or "pen". */
         n += snprintf(out + n, cap - n, "%s[\"%s\",%u,\"%s\"%s%s%s]", i ? "," : "",
               scorer, goals[gs * i + 2], assist, *kind ? ",\"" : "", kind, *kind ? "\"" : "");
      }
      if (n > 0 && (size_t)n < cap)
         n += snprintf(out + n, cap - n, "]}");
   }
   if (n > 0 && (size_t)n < cap)
      n += snprintf(out + n, cap - n, "]}");
   return n > 0 && (size_t)n < cap;
}

/* Twice a second during a Kaillera match (frame = the lockstep frame, the
   same on every PC): kickoff, goals, period changes and the end go to the
   DLL (kailleraLiveMatch(), which sends them to WE Camp), plus a state
   every 15 s for the card's clock. */
static void ksync_live_start(const uint8_t *st, const ksync_we_map *map,
      unsigned kickoff, unsigned half)
{
   ksync_live.active       = true;
   ksync_live.kickoff      = kickoff;
   ksync_live.half         = half;
   ksync_live.armed        = 0;
   ksync_live.over_since   = 0;
   ksync_live.extra        = false;
   ksync_live.team[0]      = st[map->team_id];
   ksync_live.team[1]      = st[map->team_id + 1];
   ksync_live.last_json[0] = '\0';
   /* This match's score from here (a new kickoff isn't a score going down). */
   ksync_live.prev_score[0] = st[map->score];
   ksync_live.prev_score[1] = st[map->score + 1];
   ksync_live.et_goals      = st[map->score] + st[map->score + 1];
   ksync_live.gg_clock      = 0;
   ksync_live.decided       = false;
   ksync_live.so_since      = 0;
}

static void ksync_live_sample(const uint8_t *ram, size_t size, const uint8_t *scratch,
      unsigned frame)
{
   char json[KSYNC_LIVE_JSON_LEN];
   const ksync_we_map *map = size == WE_RAM_SIZE ? ksync_live_map(ram, scratch) : NULL;
   const uint8_t *st       = map && map->scratch ? scratch : ram;
   unsigned period, clock, in_play;
   bool final = false, final_before = false;
   uint32_t sig;

   if (map && !ksync_live.active)
      ksync_live_capture_names(ram, st, map);
   if (!map || !ksync_live_valid(ram, st, map, &period, &clock))
   {
      ksync_live.active    = false;
      ksync_live.have_prev = false;
      return;
   }
   in_play = map->in_play ? ram[map->in_play] : 1;
   if (ksync_live.have_prev)
   {
      /* Kickoff of the 1st half: the clock jumps up to the half's length
         (from 0, or - WE4, after a match left from the pause menu - from
         where it went on counting down in the menus). WE3 sets it at the
         formation screen - its kickoff is when it starts running. */
      if (period == 0 && clock >= KSYNC_LIVE_MIN_KICKOFF
            && clock >= ksync_live.prev_clock + KSYNC_LIVE_MIN_KICKOFF)
      {
         /* Extra time: right after the 2nd half (period 1, clock 0) of the
            match being played, WE2002 starts it as a new 1st half (period 0,
            a shorter clock) with the score kept - the same match, sent as
            periods 2 and 3. Its phase is 2 by then: a sample may land on the
            period already back to 0 with the clock still at 0. */
         if (ksync_live.active && ((ksync_live.prev_period == 1 && ksync_live.prev_clock == 0)
               || (map->phase && (ram[map->phase] == 2 || ram[map->phase] == 3))))
         {
            ksync_live.extra = true;
            ksync_live.half  = clock;
         }
         else if (map->kick_on_run)
         {
            ksync_live.active = false;
            ksync_live.armed  = clock;
         }
         else
            ksync_live_start(st, map, frame, clock);
      }
      else if (ksync_live.armed && period == 0 && clock < ksync_live.prev_clock)
         ksync_live_start(st, map, frame, ksync_live.armed);
      /* Joined mid-match (retry-connect): the clock is already running. */
      else if (!ksync_live.active && clock > 0 && clock < ksync_live.prev_clock && in_play)
         ksync_live_start(st, map, 0, 18000);
      else if (ksync_live.active && map->in_play)
      {
         /* WE4: the ball stops after the 2nd half with the clock at 0 - the
            end; with the clock still running - left from the pause menu. */
         if (ksync_live.prev_in_play && !in_play && period == 1)
         {
            if (clock == 0)
               final = true;
            else
               ksync_live.active = false;
         }
      }
      /* WE2002, WE3: after the 2nd half the period goes back to 0, clock at
         0. WE3 resets the score with it - the end is the sample before. */
      else if (ksync_live.active && ksync_live.prev_period == 1 && period == 0 && clock == 0)
      {
         /* WE2002: the period also goes back to 0 when a tied match goes to
            extra time (phase 2, already set) or to the penalty shootout
            (phase 4) - the end comes after them. */
         if (map->phase && (ram[map->phase] == 2 || ram[map->phase] == 4))
         {
            /* extra time / shootout: not the end yet */
         }
         else if (!map->end_resets)
            final = true;
         else if (ksync_live.prev_clock == 0 && ksync_live.last_json[0])
            final_before = true;
         else
            ksync_live.active = false;
      }
   }
   /* WE2002: the referee ended the period (map->state). The 2nd half or extra
      time with a winner, a golden goal (extra time's clock still running) or
      the decided shootout - the end; tied, extra time or the shootout follow
      (the phase goes up) or, KSYNC_LIVE_DRAW_FRAMES later, it's a draw. */
   if (ksync_live.active && map->state && map->phase && !final && !final_before
         && ram[map->state] == WE_STATE_OVER)
   {
      unsigned phase = ram[map->phase];
      if (phase == 4)
      {
         if (ram[map->pens] != ram[map->pens + 1])
            final = true;
      }
      else if (phase == 1 || phase == 3 || (phase == 2 && clock > 0))
      {
         if (st[map->score] != st[map->score + 1])
            final = true;
         else if (!ksync_live.over_frame)
            ksync_live.over_frame = frame;
         else if (frame - ksync_live.over_frame >= KSYNC_LIVE_DRAW_FRAMES)
            final = true;
      }
   }
   else
      ksync_live.over_frame = 0;
   /* WE2002 extra time: a goal after which the clock never runs again is a
      golden goal (the game shows the result and goes back to the menus). */
   if (ksync_live.active && map->phase && (ram[map->phase] == 2 || ram[map->phase] == 3))
   {
      unsigned goals = st[map->score] + st[map->score + 1];
      if (goals > ksync_live.et_goals)
         ksync_live.gg_clock = clock + 1;
      else if (ksync_live.gg_clock && clock + 1 != ksync_live.gg_clock)
         ksync_live.gg_clock = 0; /* the match went on */
      ksync_live.et_goals = goals;
   }
   else if (ksync_live.active)
   {
      ksync_live.et_goals = st[map->score] + st[map->score + 1];
      ksync_live.gg_clock = 0;
   }
   /* The game left the match - back to the menus (other teams), "restart"
      (WE2002's phase goes back to 0 with the same teams, then the score) or
      any score going down: a decided shootout (its state byte doesn't
      always go to FF) or a golden goal ends like that - the end is the
      sample before, with the match's teams and score; otherwise the match
      was left, and a reset score is never sent. WE3 keeps the kickoff's
      teams and resets the score at its end (end_resets). */
   if (ksync_live.active && !map->end_resets && !final && !final_before && ksync_live.have_prev
         && (st[map->team_id] != ksync_live.team[0] || st[map->team_id + 1] != ksync_live.team[1]
            || st[map->score] < ksync_live.prev_score[0] || st[map->score + 1] < ksync_live.prev_score[1]
            || (ksync_live.decided && map->phase && ram[map->phase] < ksync_live.prev_phase)))
   {
      if (ksync_live.decided && ksync_live.last_json[0])
         final_before = true;
      else
         ksync_live.active = false;
   }
   ksync_live.decided = map->phase
         && ((ram[map->phase] == 4 && ram[map->pens] != ram[map->pens + 1])
            || ((ram[map->phase] == 2 || ram[map->phase] == 3) && ksync_live.gg_clock > 1 /* clock running */
               && st[map->score] != st[map->score + 1])
            || (map->state && ram[map->state] == WE_STATE_OVER
               && (ram[map->phase] == 1 || ram[map->phase] == 3 || ram[map->phase] == 4
                  || (ram[map->phase] == 2 && clock > 0))));
   ksync_live.prev_phase    = map->phase ? ram[map->phase] : 0;
   ksync_live.prev_score[0] = st[map->score];
   ksync_live.prev_score[1] = st[map->score + 1];
   /* WE2002 penalty shootout: over when its state byte (map->kicker: 9 while
      the kicks go on, 11/13 for ~6 s after a miss) goes to FF or, the score
      decided, stays at a miss's value for KSYNC_LIVE_SHOOTOUT_IDLE frames
      (the last kick missed - the game shows the result). */
   if (ksync_live.active && !final_before && map->kicker && map->phase && ram[map->phase] == 4)
   {
      uint32_t so = (uint32_t)ram[map->pens] | ram[map->pens + 1] << 8 | (uint32_t)ram[map->kicker] << 16;
      if (so != ksync_live.so_state || !ksync_live.so_since)
      {
         ksync_live.so_state = so;
         ksync_live.so_since = frame;
      }
      if (ram[map->pens] != ram[map->pens + 1] && (ram[map->kicker] == 0xFF
            || (ram[map->kicker] != 0 && ram[map->kicker] != 9
               && frame - ksync_live.so_since >= KSYNC_LIVE_SHOOTOUT_IDLE)))
         final = true;
   }
   /* WE3: the 2nd half's clock and added time at 0 for 10 s - the end (the
      game may still add a little after the first 0; the period only goes
      back to 0 when the players leave the menu after the result). */
   if (ksync_live.active && map->extra && !final && !final_before)
   {
      if (period == 1 && clock == 0 && ksync_live_u16(st, map->extra) == 0)
      {
         if (!ksync_live.over_since)
            ksync_live.over_since = frame;
         else if (frame - ksync_live.over_since >= KSYNC_LIVE_OVER_FRAMES)
            final = true;
      }
      else
         ksync_live.over_since = 0;
   }
   ksync_live.have_prev    = true;
   ksync_live.prev_period  = period;
   ksync_live.prev_clock   = clock;
   ksync_live.prev_in_play = in_play;
   if (!ksync_live.active)
      return;

   if (final_before)
   {
      char *f = strstr(ksync_live.last_json, "\"final\":0");
      ksync_live.active = false;
      if (f)
      {
         f[8] = '1';
         kailleraLiveMatchExternal(ksync_live.last_json);
      }
      return;
   }
   /* The sample before the end: WE3's, and WE2002's extra time and shootout
      (a golden goal / the shootout left from the result screen - other teams). */
   if ((map->end_resets || map->phase)
         && !ksync_live_json(ram, st, map, frame, period, clock, false,
            ksync_live.last_json, sizeof(ksync_live.last_json)))
      ksync_live.last_json[0] = '\0';
   sig = ksync_live_sig(ram, st, map, clock);
   if (!final && sig == ksync_live.sent_sig
         && frame - ksync_live.sent_frame < KSYNC_LIVE_HEARTBEAT_FRAMES)
      return;
   ksync_live.sent_sig   = sig;
   ksync_live.sent_frame = frame;
   if (final)
   {
      /* The score stays; WE2002's period went back to 0. */
      ksync_live.active = false;
      period            = 1;
      clock             = 0;
   }
   if (ksync_live_json(ram, st, map, frame, period, clock, final, json, sizeof(json)))
      kailleraLiveMatchExternal(json);
}

/* ------------------------------------------------------------------------ */
/* 4. (cont.) Per-frame RAM digest + restore point capture                   */
/* ------------------------------------------------------------------------ */

void kailleraSyncAfterFrame(unsigned frame)
{
   struct retro_core_t *core = &runloop_state_get_ptr()->current_core;
   char msg[96];
   const uint8_t *ram;
   size_t size, slice, start;
   unsigned window, idx;

   /* Numbering restarts - retry-connect go-live or a rollback's resume. The
      first frame run after either is frame 1 on every machine, whether it
      was processed while paused or at the top of an already-running frame. */
   if (ksync_renumber_pending)
   {
      bool go_live = false;

      ksync_renumber_pending = false;
      ksync_frame_base       = frame - 1;
      ksync_ram_window       = (unsigned)-1;
      KSYNC_LOCK();
      memset(ksync_local_digests, 0, sizeof(ksync_local_digests));
      if (!ksync_digest_enabled)
      {
         ksync_digest_enabled = true;
         go_live              = true;
      }
      KSYNC_UNLOCK();
      if (go_live)
         ksync_log("Retry-connect: ao vivo a partir do frame %u - detector de "
               "RAM ativado.", frame);
   }

   if (!ksync_digest_enabled || !core->retro_get_memory_data
         || !core->retro_get_memory_size)
      return;

   ram  = (const uint8_t*)core->retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
   size = core->retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
   if (!ram || !size)
      return;

   frame           -= ksync_frame_base;
   ksync_last_frame = frame;
   /* Placar ao vivo: only with the player logged into the WE Camp account
      (the DLL says so) - otherwise the game's RAM isn't read for it. */
   ksync_live_goal_watch(ram, frame); /* only during a match the DLL allows (ksync_live.active) */
   if (frame % KSYNC_LIVE_SAMPLE_FRAMES == 0)
   {
      if (kailleraLiveEnabledExternal())
         ksync_live_sample(ram, size, ksync_live_scratchpad(), frame);
      else if (ksync_live.active || ksync_live.have_prev)
         ksync_live_reset();
   }
   window           = frame / KSYNC_DIGEST_FRAMES;
   idx              = frame % KSYNC_DIGEST_FRAMES;

   if (window != ksync_ram_window)
   {
      ksync_ram_window = window;
      ksync_ram_acc    = 0xCBF29CE484222325ULL; /* FNV offset basis */
   }

   /* Slice `idx` of the RAM - the same one on every machine this frame. */
   slice = (size + KSYNC_DIGEST_FRAMES - 1) / KSYNC_DIGEST_FRAMES;
   start = (size_t)idx * slice;
   if (start < size)
      ksync_ram_acc = ksync_hash_ram(ksync_ram_acc, ram + start,
            (size - start < slice) ? size - start : slice);

   /* A restore point every KSYNC_SNAPSHOT_FRAMES - but not once a desync
      was seen: the ring has to keep the points from before it. */
   if (     frame >= KSYNC_SNAPSHOT_FRAMES
         && frame % KSYNC_SNAPSHOT_FRAMES == 0
         && !ksync_capture_stopped
         && ksync_r_state == KSYNC_R_NONE)
      ksync_take_snapshot(frame / KSYNC_SNAPSHOT_FRAMES);

   if (idx != KSYNC_DIGEST_FRAMES - 1)
      return;

   /* Window complete: keep ours for comparison and tell the others. */
   KSYNC_LOCK();
   {
      ksync_digest_t *d = &ksync_local_digests[window % KSYNC_DIGEST_HISTORY];
      d->window        = window;
      d->epoch         = ksync_epoch;
      d->peers_matched = 0;
      d->valid         = true;
      snprintf(d->hash, sizeof(d->hash), "%08X%08X",
            (unsigned)(ksync_ram_acc >> 32), (unsigned)ksync_ram_acc);
      snprintf(msg, sizeof(msg), KSYNC_PREFIX "v=%s n=%s e=%u d=%u:%s",
            KSYNC_PROTOCOL, ksync_nonce, ksync_epoch, window, d->hash);
   }
   KSYNC_UNLOCK();

   kailleraChatSendExternal(msg);
}

void kailleraSyncRetryConnectGoLive(void)
{
   /* Only for a live room with someone to compare against, and only if the
      detector isn't already running (a normal game never gets here). The
      actual start is latched by the next kailleraSyncAfterFrame(). */
   KSYNC_LOCK();
   if (ksync_send_enabled && !ksync_digest_enabled)
      ksync_renumber_pending = true;
   KSYNC_UNLOCK();
}

/* ------------------------------------------------------------------------ */
/* 6. Rollback                                                              */
/* ------------------------------------------------------------------------ */

static unsigned ksync_r_back; /* seconds from the desync back to ksync_r_id */

static void ksync_osd(const char *msg, bool warning, unsigned frames)
{
   runloop_msg_queue_push(msg, warning ? 2 : 1, frames, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT,
         warning ? MESSAGE_QUEUE_CATEGORY_WARNING : MESSAGE_QUEUE_CATEGORY_INFO);
}

/* Newest usable restore point with an id below `below` ((unsigned)-1 for
   the newest of all), among the newest KSYNC_SNAPSHOT_OFFER taken this
   epoch - the ring's oldest slot is a spare, see KSYNC_SNAPSHOTS. */
static ksync_snapshot_t *ksync_pick_snapshot(unsigned below)
{
   ksync_snapshot_t *best = NULL;
   int i;

   for (i = 0; i < KSYNC_SNAPSHOTS; i++)
   {
      ksync_snapshot_t *s = &ksync_snapshots[i];
      if (!ksync_snapshot_usable(s) || s->id >= below)
         continue;
      if (s->id + KSYNC_SNAPSHOT_OFFER <= ksync_snapshot_last_id)
         continue;
      if (!best || s->id > best->id)
         best = s;
   }

   return best;
}

/* The rollback key just went down, with RetroArch's own window in front -
   the Kaillera dialogs share this process, and typing there must not count. */
static bool ksync_rollback_key_pressed(void)
{
   static bool was_down = false;
   bool down, edge;

#if defined(_WIN32)
   down = (GetAsyncKeyState(KSYNC_ROLLBACK_VK) & 0x8000)
      && GetForegroundWindow() == win32_get_window();
#else
   down = false;
#endif

   edge     = down && !was_down;
   was_down = down;
   return edge;
}

/* Only the host (player 1 - the room's creator, who also resumes everyone
   at the end of the countdown) decides whether and how far to go back. */
static bool ksync_is_host(void)
{
   return kPlayerNumber == 1;
}

static void ksync_show_prompt(void)
{
   if (ksync_is_host())
      ksync_osd("Possível desync encontrado, caso queiram retornar a momentos "
            "antes aperte " KSYNC_ROLLBACK_KEY "!", true, 15 * 60);
   else
      ksync_osd("Possível desync encontrado - o host pode voltar a partida "
            "para momentos antes.", true, 15 * 60);
   ksync_prompt_shown++;
   ksync_prompt_frames = 0;
}

/* The detector just reported a desync with some peer (main thread, lock not
   held). Returns true when a rollback prompt takes the place of the plain
   detector message - or nothing should be shown, mid-rollback. */
static bool ksync_on_desync(void)
{
   if (ksync_r_state != KSYNC_R_NONE)
      return true;

   ksync_capture_stopped = true;
   if (!ksync_pick_snapshot((unsigned)-1))
   {
      ksync_log("Nenhum ponto de volta confirmado em todos os PCs - sem "
            "opção de voltar.");
      return false;
   }

   ksync_r_state      = KSYNC_R_PROMPT;
   ksync_prompt_total = 0;
   ksync_log("Pontos de volta disponíveis - aguardando " KSYNC_ROLLBACK_KEY ".");
   return true;
}

/* The host's rollback key: ask everyone - through the input stream - to load point
   `id`. Takes effect (here too) once the command comes back around. */
static void ksync_request_restore(unsigned id)
{
   kailleraCommands        = COMMAND_DESYNC_RESTORE | ((id & 0xFFF) << 4);
   ksync_r_request_pending = true;
   ksync_r_command_frames  = 0;
}

/* COMMAND_DESYNC_RESTORE, after the same frame on every machine. */
static void ksync_restore_to(unsigned id)
{
   ksync_snapshot_t *s = &ksync_snapshots[id % KSYNC_SNAPSHOTS];
   retro_ctx_serialize_info_t info;
   char msg[160];

   ksync_r_request_pending = false;

   if (!ksync_digest_enabled)
   {
      /* A replay or Watch Live never takes restore points of its own. */
      ksync_log("Volta para o ponto %u ignorada (replay/espectador não tem "
            "pontos de volta).", id);
      return;
   }

   info.data_const = s->data;
   info.data       = NULL;
   info.size       = s->size;
   if (     !s->valid || s->id != id || s->epoch != ksync_epoch || !s->data
         || !core_unserialize(&info))
   {
      snprintf(msg, sizeof(msg), "Falha ao voltar: este PC não tem o ponto "
            "%u - reiniciem a partida.", id);
      ksync_osd(msg, true, 10 * 60);
      ksync_log("%s", msg);
      return;
   }

   if (ksync_r_state != KSYNC_R_FROZEN)
      ksync_r_back = ksync_last_frame > id * KSYNC_SNAPSHOT_FRAMES
         ? (ksync_last_frame - id * KSYNC_SNAPSHOT_FRAMES) / 60 : 0;
   else
      ksync_r_back += (ksync_r_id - id) * (KSYNC_SNAPSHOT_FRAMES / 60);

   ksync_r_state         = KSYNC_R_FROZEN;
   ksync_r_id            = id;
   ksync_r_render        = true;
   ksync_r_deadline      = cpu_features_get_time_usec() + KSYNC_COUNTDOWN_USEC;
   ksync_r_last_second   = -1;
   ksync_r_resume_sent   = false;
   ksync_capture_stopped = true;
   ksync_rollbacks++;
   ksync_log("Voltando para o ponto %u (~%u s antes do desync).", id,
         ksync_r_back);
}

/* COMMAND_DESYNC_RESUME, after the same frame on every machine. */
static void ksync_resume(void)
{
   ksync_snapshot_t base;
   int keep, i;

   if (ksync_r_state != KSYNC_R_FROZEN)
      return; /* a duplicate - already resumed */

   /* The point we resume from becomes point 0 of the new epoch, so a quick
      second desync can come straight back here. */
   keep                        = ksync_r_id % KSYNC_SNAPSHOTS;
   base                        = ksync_snapshots[keep];
   ksync_snapshots[keep].data  = NULL;
   for (i = 0; i < KSYNC_SNAPSHOTS; i++)
   {
      free(ksync_snapshots[i].data);
      memset(&ksync_snapshots[i], 0, sizeof(ksync_snapshots[i]));
   }

   KSYNC_LOCK();
   ksync_epoch++;
   base.id            = 0;
   base.epoch         = ksync_epoch;
   base.peers_same    = 0;
   base.peers_diff    = 0;
   ksync_snapshots[0] = base; /* verified flags carry over - it was picked */
   for (i = 0; i < KSYNC_MAX_REMOTES; i++)
      ksync_peers[i].desync_reported = false;
   KSYNC_UNLOCK();

   ksync_snapshot_last_id = 0;
   ksync_capture_stopped  = false;
   ksync_r_state          = KSYNC_R_NONE;
   ksync_prompt_shown     = 0;
   ksync_renumber_pending = true; /* digests restart from the next frame */

   ksync_osd("Partida retomada!", false, 3 * 60);
   ksync_log("Partida retomada a partir do ponto de volta (%u s antes do "
         "desync).", ksync_r_back);
}

void kailleraSyncOnCommand(int command)
{
   unsigned c = (unsigned)command & 0xFFFF;

   switch (c & 0xF)
   {
      case COMMAND_DESYNC_RESTORE:
         ksync_restore_to((c >> 4) & 0xFFF);
         break;
      case COMMAND_DESYNC_RESUME:
         ksync_resume();
         break;
      default:
         break;
   }
}

int kailleraSyncFrameMode(void)
{
   if (ksync_r_state != KSYNC_R_FROZEN)
      return KSYNC_FRAME_RUN;
   if (ksync_r_render)
   {
      ksync_r_render = false;
      return KSYNC_FRAME_RENDER;
   }
   return KSYNC_FRAME_FROZEN;
}

/* Prompt repeats, the rollback key and the countdown - every Kaillera frame (frozen
   ones included), main thread, lock not held. */
static void ksync_rollback_tick(void)
{
   bool key = ksync_rollback_key_pressed(); /* every tick, to track the edge */

   if (key && !ksync_is_host())
   {
      if (ksync_r_state != KSYNC_R_NONE)
         ksync_osd("Só o host (jogador 1) pode voltar a partida.", false, 2 * 60);
      key = false;
   }

   /* Our RESTORE never came back around - let the key send another. */
   if (     ksync_r_request_pending
         && ++ksync_r_command_frames >= KSYNC_COMMAND_RETRY)
      ksync_r_request_pending = false;

   if (ksync_r_state == KSYNC_R_PROMPT)
   {
      if (++ksync_prompt_total >= KSYNC_PROMPT_EXPIRE && !ksync_r_request_pending)
      {
         ksync_r_state = KSYNC_R_NONE; /* the ring stays stopped: no new offer */
         ksync_osd("A opção de voltar a partida expirou.", false, 3 * 60);
         ksync_log("Ninguém apertou " KSYNC_ROLLBACK_KEY " - opção de voltar expirou.");
         return;
      }

      if (     ksync_prompt_shown < KSYNC_PROMPT_REPEATS
            && ++ksync_prompt_frames >= KSYNC_PROMPT_EVERY)
         ksync_show_prompt();

      if (key && !ksync_r_request_pending)
      {
         ksync_snapshot_t *s = ksync_pick_snapshot((unsigned)-1);
         if (s)
            ksync_request_restore(s->id);
         else
            ksync_osd("Não há ponto seguro para voltar.", true, 3 * 60);
      }
   }
   else if (ksync_r_state == KSYNC_R_FROZEN)
   {
      retro_time_t now = cpu_features_get_time_usec();

      if (key && !ksync_r_request_pending)
      {
         ksync_snapshot_t *s = ksync_pick_snapshot(ksync_r_id);
         if (s)
            ksync_request_restore(s->id);
         else
            ksync_osd("Não há ponto mais antigo para voltar.", false, 2 * 60);
      }

      if (now >= ksync_r_deadline)
      {
         /* Player 1's machine resumes everyone (automatically - nobody has
            to press anything); a RESTORE still in flight restarts the
            countdown instead. */
         if (ksync_is_host() && !ksync_r_request_pending)
         {
            if (!ksync_r_resume_sent || ++ksync_r_command_frames >= KSYNC_COMMAND_RETRY)
            {
               kailleraCommands       = COMMAND_DESYNC_RESUME;
               ksync_r_resume_sent    = true;
               ksync_r_command_frames = 0;
            }
         }
      }
      else
      {
         int secs = (int)((ksync_r_deadline - now + 999999) / 1000000);
         if (secs != ksync_r_last_second)
         {
            char msg[160];
            ksync_r_last_second = secs;
            if (ksync_is_host())
               snprintf(msg, sizeof(msg), "Voltou %u s. Retomando em %d... "
                     "(" KSYNC_ROLLBACK_KEY " = voltar mais %u s)", ksync_r_back,
                     secs, KSYNC_SNAPSHOT_FRAMES / 60);
            else
               snprintf(msg, sizeof(msg), "Voltou %u s. Retomando em %d... "
                     "(o host pode voltar mais)", ksync_r_back, secs);
            ksync_osd(msg, false, 70);
         }
      }
   }
}
