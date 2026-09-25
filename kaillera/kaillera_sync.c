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

#ifdef KAILLERA_SYNC_TEST
extern bool ksync_test_key_down; /* test harness seam - see ksync_rollback_key_pressed() */
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
   { "pcsx_rearmed_cd_turbo",             "disabled" },
   { "pcsx_rearmed_noxadecoding",         "enabled"  },
   { "pcsx_rearmed_nocdaudio",            "enabled"  },
   { "pcsx_rearmed_spu_reverb",           "enabled"  },
   { "pcsx_rearmed_frameskip_type",       "disabled" },
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
   return kailleraSyncActive() && ksync_no_memcard;
}

bool kailleraSyncBlockSramSave(void)
{
   /* A replay, Watch Live or retry-connect replaying a match with memory
      cards must not write that match's saves over the viewer's own card. */
   return kailleraSyncActive() && (ksync_no_memcard || ksync_playback);
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

const char *kailleraSyncForcedCoreOption(const char *key)
{
   size_t i;

   if (!key || !kailleraSyncActive())
      return NULL;

   /* The room turned "Sem M. Card" off: everyone keeps their own cards (and
      the fingerprint's m= field checks card 1 matches). */
   if (!ksync_no_memcard && strncmp(key, "pcsx_rearmed_memcard", 20) == 0)
      return NULL;

   for (i = 0; i < sizeof(ksync_forced_options) / sizeof(ksync_forced_options[0]); i++)
      if (string_is_equal(key, ksync_forced_options[i].key))
         return ksync_forced_options[i].value;

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
            ksync_forced_options[i].key, ksync_forced_options[i].value);
      if (n > 0)
         crc = encoding_crc32(crc, (const uint8_t*)line, (size_t)n);
   }

   return encoding_crc32(crc, (const uint8_t*)ksync_forced_extra,
         sizeof(ksync_forced_extra) - 1);
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

/* .cue/.m3u only describe the real data - hash the files they point at (in
   order), so two different .bin behind identically named .cue still differ. */
static bool ksync_hash_content(const char *path, unsigned gen,
      uint32_t *crc, uint64_t *size, int depth)
{
   const char *ext = path_get_extension(path);
   bool is_cue     = string_is_equal_noncase(ext, "cue");
   bool is_m3u     = string_is_equal_noncase(ext, "m3u");
   void *raw       = NULL;
   int64_t raw_len = 0;
   char *line, *next;
   bool any        = false;
   bool ok         = true;

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
      ok  = ksync_hash_content(full, gen, crc, size, depth + 1);
      any = true;
   }

   free(raw);

   if (!any) /* malformed playlist - fall back to the file itself */
      return ksync_hash_file(path, gen, crc, size);

   return ok;
}

static void ksync_crc_thread(void *userdata)
{
   ksync_crc_job_t *job = (ksync_crc_job_t*)userdata;
   uint32_t crc         = 0;
   uint64_t size        = 0;
   bool ok;

#ifdef _WIN32
   /* Don't compete with the emulator for CPU during the first seconds. */
   SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

   ok = ksync_hash_content(job->path, job->gen, &crc, &size, 0);

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

void kailleraSyncGameBegin(int num_players, bool playback, bool no_memcard)
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
   ksync_send_enabled      = false;
   ksync_digest_enabled    = false;
   ksync_content_ready     = false;
   ksync_sent              = false;
   ksync_resent            = false;
   ksync_missing_warned    = false;
   ksync_frames_since_sent = 0;
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
   if (ksync_no_memcard)
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
      else if ((job = (ksync_crc_job_t*)malloc(sizeof(*job))))
      {
         job->gen = ksync_gen;
         strlcpy(job->path, content, sizeof(job->path));
      }
   }
   KSYNC_UNLOCK();

   if (ksync_send_enabled)
      ksync_log("=== Partida: %s | %s | %d jogadores | %s%s",
            string_is_empty(content) ? "?" : path_basename(content), core,
            ksync_num_players,
            ksync_no_memcard ? "sem memory card" : "com memory card",
            ksync_digest_enabled ? ""
            : " (retry-connect: detector de RAM liga no go-live)");

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
/* 4. (cont.) Per-frame RAM digest + restore point capture                   */
/* ------------------------------------------------------------------------ */

/* Test hook, same spirit as kaillera-client's N02_KREC_TEST_PAUSE_FRAME: with
   the environment variable KAILLERA_SYNC_TEST_DESYNC set, F10 (RetroArch
   window focused) flips one byte of THIS machine's RAM - a real, deliberate
   desync to exercise the detector and the rollback end to end. Inert unless
   explicitly enabled. */
static void ksync_test_desync_hotkey(uint8_t *ram, size_t size)
{
#if defined(_WIN32) && !defined(KAILLERA_SYNC_TEST)
   static int  enabled  = -1;
   static bool was_down = false;
   bool down;

   if (enabled < 0)
      enabled = getenv("KAILLERA_SYNC_TEST_DESYNC") != NULL;
   if (!enabled)
      return;

   down = (GetAsyncKeyState(VK_F10) & 0x8000)
      && GetForegroundWindow() == win32_get_window();
   if (down && !was_down)
   {
      ram[size / 2] ^= 0xFF;
      runloop_msg_queue_push("TESTE: desync provocado neste PC (F10).", 2,
            3 * 60, true, NULL, MESSAGE_QUEUE_ICON_DEFAULT,
            MESSAGE_QUEUE_CATEGORY_WARNING);
      ksync_log("TESTE: desync provocado com F10 no frame %u.", ksync_last_frame);
   }
   was_down = down;
#else
   (void)ram;
   (void)size;
#endif
}

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

   ksync_test_desync_hotkey((uint8_t*)ram, size);

   frame           -= ksync_frame_base;
   ksync_last_frame = frame;
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

#if defined(KAILLERA_SYNC_TEST)
   down = ksync_test_key_down;
#elif defined(_WIN32)
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
