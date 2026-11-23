/* Stubs for kaillera_sync.c, so the real file runs in a harness DLL. One copy of the DLL = one
   player's machine (its own globals); driver.c loads two copies and plays them in lockstep. The
   "core" is 2 MB of RAM updated deterministically from the input every frame; a savestate is that
   RAM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <windows.h>

#include <boolean.h>
#include <libretro.h>
#include <streams/file_stream.h>
#include <file/file_path.h>
#include <string/stdstring.h>
#include <rthreads/rthreads.h>
#include <features/features_cpu.h>
#include <encodings/utf.h>

#include "kaillera/kaillera.h"
#include "kaillera/kaillera_sync.h"
#include "core.h"
#include "runloop.h"
#include "paths.h"
#include "verbosity.h"

#define H_RAM_SIZE (2 * 1024 * 1024)

int          kailleraCommands;
volatile int kailleraInitialisedInternal = 1;
int          kPlayerNumber = 1;

typedef struct
{
   void (*chat)(int machine, const char *text);
   void (*osd)(int machine, const char *msg);
} h_callbacks_t;

static h_callbacks_t h_cb;
static int           h_machine;
static char          h_dir[MAX_PATH];
static char          h_content[MAX_PATH];
static retro_time_t  h_now;
static uint8_t      *h_ram;
static runloop_state_t h_rs;

static void *h_mem_data(unsigned id) { return id == RETRO_MEMORY_SYSTEM_RAM ? h_ram : NULL; }
static size_t h_mem_size(unsigned id) { return id == RETRO_MEMORY_SYSTEM_RAM ? H_RAM_SIZE : 0; }

__declspec(dllexport) void h_init(int machine, int player, const char *dir, const char *content,
      h_callbacks_t *cb)
{
   h_machine     = machine;
   kPlayerNumber = player;
   strncpy(h_dir, dir, sizeof(h_dir) - 1);
   strncpy(h_content, content, sizeof(h_content) - 1);
   h_cb  = *cb;
   h_ram = (uint8_t*)calloc(1, H_RAM_SIZE);
   h_rs.system.info.library_name    = "PCSX-ReARMed";
   h_rs.system.info.library_version = "r26 harness";
   h_rs.current_core.retro_get_memory_data = h_mem_data;
   h_rs.current_core.retro_get_memory_size = h_mem_size;
}

__declspec(dllexport) void h_set_time(retro_time_t t) { h_now = t; }
__declspec(dllexport) uint8_t *h_get_ram(void) { return h_ram; }
__declspec(dllexport) int *h_commands(void) { return &kailleraCommands; }

/* One emulated frame: the frame counter lives in the RAM (so a loaded state
   carries on from its own frame), and 64 bytes change depending on it and on
   the input. */
__declspec(dllexport) void h_run(const uint8_t *input, int len)
{
   uint32_t counter, h;
   int i;
   memcpy(&counter, h_ram, 4);
   h = counter * 2654435761u;
   for (i = 0; i < len; i++)
      h = h * 31 + input[i];
   for (i = 0; i < 64; i++)
   {
      uint32_t idx = 16 + (h + (uint32_t)i * 104729u) % (H_RAM_SIZE - 16);
      h_ram[idx] = (uint8_t)(h_ram[idx] * 7 + h + i);
   }
   counter++;
   memcpy(h_ram, &counter, 4);
}

/* ---- core.h ---- */
size_t core_serialize_size(void) { return H_RAM_SIZE; }
bool core_serialize(retro_ctx_serialize_info_t *info)
{
   if (!info || !info->data || info->size < H_RAM_SIZE)
      return false;
   memcpy(info->data, h_ram, H_RAM_SIZE);
   return true;
}
bool core_unserialize(retro_ctx_serialize_info_t *info)
{
   if (!info || !info->data_const || info->size < H_RAM_SIZE)
      return false;
   memcpy(h_ram, info->data_const, H_RAM_SIZE);
   return true;
}

retro_time_t cpu_features_get_time_usec(void) { return h_now; }

uint32_t encoding_crc32(uint32_t crc, const uint8_t *buf, size_t len)
{
   size_t i;
   int k;
   crc = ~crc;
   for (i = 0; i < len; i++)
   {
      crc ^= buf[i];
      for (k = 0; k < 8; k++)
         crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
   }
   return ~crc;
}

/* ---- streams/file_stream.h (stdio) ---- */
RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
   const char *m = "rb";
   FILE *f;
   (void)hints;
   if (mode & RETRO_VFS_FILE_ACCESS_WRITE)
      m = (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) ? "r+b" : "wb";
   f = fopen(path, m);
   if (!f && (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING))
      f = fopen(path, "w+b");
   return (RFILE*)f;
}
int64_t filestream_read(RFILE *s, void *data, int64_t len) { return (int64_t)fread(data, 1, (size_t)len, (FILE*)s); }
int64_t filestream_write(RFILE *s, const void *data, int64_t len) { return (int64_t)fwrite(data, 1, (size_t)len, (FILE*)s); }
int64_t filestream_seek(RFILE *s, int64_t offset, int whence)
{
   int w = whence == RETRO_VFS_SEEK_POSITION_END ? SEEK_END
         : whence == RETRO_VFS_SEEK_POSITION_CURRENT ? SEEK_CUR : SEEK_SET;
   return _fseeki64((FILE*)s, offset, w) == 0 ? _ftelli64((FILE*)s) : -1;
}
int filestream_close(RFILE *s) { return s ? fclose((FILE*)s) : -1; }
int filestream_delete(const char *path) { return remove(path); }
int64_t filestream_read_file(const char *path, void **buf, int64_t *len)
{
   FILE *f = fopen(path, "rb");
   long n;
   if (!f) { *buf = NULL; *len = 0; return 0; }
   fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
   *buf = malloc((size_t)n + 1);
   fread(*buf, 1, (size_t)n, f);
   ((char*)*buf)[n] = 0;
   fclose(f);
   *len = n;
   return 1;
}

/* ---- file/file_path.h ---- */
void fill_pathname_application_dir(char *s, size_t len) { strncpy(s, h_dir, len - 1); s[len - 1] = 0; }
size_t fill_pathname_join(char *out, const char *dir, const char *path, size_t size)
{
   return (size_t)snprintf(out, size, "%s\\%s", dir, path);
}
void fill_pathname_resolve_relative(char *out, const char *ref, const char *in, size_t size)
{
   (void)ref;
   strncpy(out, in, size - 1);
   out[size - 1] = 0;
}
const char *path_basename(const char *path)
{
   const char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
   const char *s = a > b ? a : b;
   return s ? s + 1 : path;
}
const char *path_get_extension(const char *path)
{
   const char *e = strrchr(path_basename(path), '.');
   return e ? e + 1 : "";
}
bool path_is_directory(const char *path)
{
   DWORD a = GetFileAttributesA(path);
   return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool path_is_valid(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }
bool path_mkdir(const char *dir) { return CreateDirectoryA(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS; }
const char *path_get(enum rarch_path_type type) { return type == RARCH_PATH_CONTENT ? h_content : ""; }

size_t strlcpy_retro__(char *dst, const char *src, size_t size)
{
   size_t n = strlen(src);
   if (size)
   {
      size_t c = n >= size ? size - 1 : n;
      memcpy(dst, src, c);
      dst[c] = 0;
   }
   return n;
}
size_t strlcat_retro__(char *dst, const char *src, size_t size)
{
   size_t l = strlen(dst);
   return l + strlcpy_retro__(dst + l, src, size > l ? size - l : 0);
}
wchar_t *utf8_to_utf16_string_alloc(const char *str)
{
   int n = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
   wchar_t *w = (wchar_t*)calloc((size_t)n + 1, sizeof(wchar_t));
   MultiByteToWideChar(CP_UTF8, 0, str, -1, w, n);
   return w;
}

/* ---- verbosity / runloop ---- */
void RARCH_LOG(const char *fmt, ...) { (void)fmt; }
void RARCH_WARN(const char *fmt, ...) { (void)fmt; }
void runloop_msg_queue_push(const char *msg, unsigned prio, unsigned duration, bool flush,
      char *title, enum message_queue_icon icon, enum message_queue_category category)
{
   (void)prio; (void)duration; (void)flush; (void)title; (void)icon; (void)category;
   if (h_cb.osd)
      h_cb.osd(h_machine, msg);
}
runloop_state_t *runloop_state_get_ptr(void) { return &h_rs; }

/* ---- rthreads: single-threaded harness, threads run inline ---- */
static int h_lock_dummy;
slock_t *slock_new(void) { return (slock_t*)&h_lock_dummy; }
void slock_lock(slock_t *l) { (void)l; }
void slock_unlock(slock_t *l) { (void)l; }
sthread_t *sthread_create(void (*fn)(void*), void *ud) { fn(ud); return (sthread_t*)&h_lock_dummy; }
int sthread_detach(sthread_t *t) { (void)t; return 0; }

void *win32_get_window(void) { return (void*)1; } /* never the foreground window: no key */

/* ---- the DLL side (kaillera.h) ---- */
void kailleraChatSendExternal(const char *msg)
{
   if (h_cb.chat)
      h_cb.chat(h_machine, msg);
}
int kailleraMemcardPrepareExternal(const char *c, const char *g, const char *d) { (void)c; (void)g; (void)d; return 0; }
int kailleraMemcardEmptySlotsExternal(void) { return 0; }
int kailleraLiveEnabledExternal(void) { return 0; }
void kailleraLiveMatchExternal(const char *s) { (void)s; }
bool kailleraRetryConnectActive(void) { return false; }
