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
#include "runloop.h"
#include "paths.h"
#include "verbosity.h"

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
} ksync_fingerprint_t;

typedef struct
{
   unsigned window;
   char     hash[17];
   bool     valid;
} ksync_digest_t;

/* Another player in the game, keyed by nick. */
typedef struct
{
   ksync_fingerprint_t fp;
   ksync_digest_t      digests[KSYNC_DIGEST_HISTORY]; /* by window % HISTORY */
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
static bool                ksync_golive_pending;
static ksync_digest_t      ksync_local_digests[KSYNC_DIGEST_HISTORY];

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

void kailleraSyncGameBegin(int num_players, bool playback)
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
   ksync_send_enabled      = false;
   ksync_digest_enabled    = false;
   ksync_content_ready     = false;
   ksync_sent              = false;
   ksync_resent            = false;
   ksync_missing_warned    = false;
   ksync_frames_since_sent = 0;
   ksync_ram_window        = (unsigned)-1;
   ksync_frame_base        = 0;
   ksync_golive_pending    = false;

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
}

void kailleraSyncGameEnd(void)
{
   char summary[KSYNC_MAX_REMOTES][160];
   int  summary_count = 0;
   int  i;

   KSYNC_LOCK();
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

   for (i = 0; i < summary_count; i++)
      ksync_log("%s", summary[i]);
}

void kailleraSyncContentLoaded(void)
{
   struct retro_system_info *info = &runloop_state_get_ptr()->system.info;
   const char *content            = path_get(RARCH_PATH_CONTENT);
   ksync_crc_job_t *job           = NULL;
   char core[KSYNC_FIELD_LEN];
   char *c;

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
      ksync_log("=== Partida: %s | %s | %d jogadores%s",
            string_is_empty(content) ? "?" : path_basename(content), core,
            ksync_num_players,
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
       && string_is_equal(a->opts,    b->opts);
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

/* "d=<window>:<16 hex>" - a peer's RAM digest. Caller holds the lock. */
static void ksync_store_digest(ksync_peer_t *peer, const char *value)
{
   char *end;
   unsigned long window = strtoul(value, &end, 10);
   ksync_digest_t *d;

   if (end == value || *end != ':' || strlen(end + 1) != 16)
      return;

   d         = &peer->digests[window % KSYNC_DIGEST_HISTORY];
   d->window = (unsigned)window;
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
         case 'd': strlcpy(digest,     tok + 2, sizeof(digest));     break;
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
         ksync_store_digest(peer, digest);
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
      const ksync_digest_t *ours;

      if (!theirs->valid)
         continue;

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

void kailleraSyncFrameTick(void)
{
   char send_msg[sizeof(ksync_local_msg)];
   char osd[KSYNC_MAX_REMOTES * 2 + 1][256];
   char log[KSYNC_MAX_REMOTES * 2 + 1][320];
   bool osd_warning[KSYNC_MAX_REMOTES * 2 + 1];
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
               KSYNC_PREFIX "v=%s n=%s c=%s i=%s b=%s o=%s",
               ksync_local.proto, ksync_nonce, ksync_local.core,
               ksync_local.content, ksync_local.bios, ksync_local.opts);
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
         if (     ksync_peers[i].used
               && osd_count < KSYNC_MAX_REMOTES * 2 + 1
               && ksync_compare_digests(&ksync_peers[i],
                     osd[osd_count], sizeof(osd[0]),
                     log[osd_count], sizeof(log[0])))
            osd_warning[osd_count++] = true;
      }
   }
   KSYNC_UNLOCK();

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

      runloop_msg_queue_push(osd[i], osd_warning[i] ? 2 : 1,
            osd_warning[i] ? 15 * 60 : 5 * 60, osd_warning[i], NULL,
            MESSAGE_QUEUE_ICON_DEFAULT,
            osd_warning[i] ? MESSAGE_QUEUE_CATEGORY_WARNING
                           : MESSAGE_QUEUE_CATEGORY_SUCCESS);
   }
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

void kailleraSyncAfterFrame(unsigned frame)
{
   struct retro_core_t *core = &runloop_state_get_ptr()->current_core;
   char msg[96];
   const uint8_t *ram;
   size_t size, slice, start;
   unsigned window, idx;

   /* Retry-connect go-live: the first frame run after it is live frame 1 on
      every machine - whether go-live was processed while paused (the normal
      case) or at the top of an already-running frame. */
   if (ksync_golive_pending)
   {
      ksync_golive_pending = false;
      ksync_frame_base     = frame - 1;
      ksync_ram_window     = (unsigned)-1;
      KSYNC_LOCK();
      memset(ksync_local_digests, 0, sizeof(ksync_local_digests));
      ksync_digest_enabled = true;
      KSYNC_UNLOCK();
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

   frame -= ksync_frame_base;
   window = frame / KSYNC_DIGEST_FRAMES;
   idx    = frame % KSYNC_DIGEST_FRAMES;

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

   if (idx != KSYNC_DIGEST_FRAMES - 1)
      return;

   /* Window complete: keep ours for comparison and tell the others. */
   KSYNC_LOCK();
   {
      ksync_digest_t *d = &ksync_local_digests[window % KSYNC_DIGEST_HISTORY];
      d->window = window;
      d->valid  = true;
      snprintf(d->hash, sizeof(d->hash), "%08X%08X",
            (unsigned)(ksync_ram_acc >> 32), (unsigned)ksync_ram_acc);
      snprintf(msg, sizeof(msg), KSYNC_PREFIX "v=%s n=%s d=%u:%s",
            KSYNC_PROTOCOL, ksync_nonce, window, d->hash);
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
      ksync_golive_pending = true;
   KSYNC_UNLOCK();
}
