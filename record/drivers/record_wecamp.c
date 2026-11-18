/*  WE Camp broadcaster output (record driver "wecamp").
 *
 *  Sends the core's frames and audio to an external ffmpeg.exe through two
 *  named pipes (raw rgb565le video + s16le stereo audio). ffmpeg encodes
 *  H.264/AAC and pushes to YouTube (RTMP) or writes a file.
 *
 *  Why not RetroArch's own record_ffmpeg: this fork is built without FFmpeg
 *  (HAVE_FFMPEG = 0). An external ffmpeg also lets the encoder settings change
 *  without a rebuild.
 *
 *  Timing: during a Watch Live game the main thread blocks inside the DLL
 *  whenever the live input has not arrived yet, so frames come in bursts and
 *  gaps. The pipes are therefore fed by two threads paced by the wall clock:
 *  the video thread sends the most recent frame every 1/fps s (repeating it
 *  during a stall), and the audio thread sends exactly "elapsed time x sample
 *  rate" samples (silence on underrun, a +-0.5% resampling to keep the
 *  queue near its target). The stream keeps flowing at real time no matter
 *  what the emulation does, and the main thread never waits on ffmpeg.
 *  Video is delayed by the audio queue's target latency to keep A/V in sync.
 *
 *  If ffmpeg dies or stops reading for 30 s (e.g. the RTMP connection
 *  dropped) it is restarted with new pipes (backoff up to 30 s).
 *
 *  Only enabled when WECAMP_BROADCASTER is set; configuration comes from
 *  the environment:
 *    WECAMP_STREAM_URL       rtmp(s)://... or a file path (required)
 *    WECAMP_FFMPEG           ffmpeg.exe path            (default ffmpeg.exe)
 *    WECAMP_STREAM_SIZE      WxH                        (default 640x480)
 *    WECAMP_STREAM_FPS       output fps                 (default 30)
 *    WECAMP_STREAM_VBITRATE  x264 bitrate               (default 1500k)
 *    WECAMP_STREAM_ABITRATE  AAC bitrate                (default 128k)
 *    WECAMP_STREAM_PRESET    x264 preset                (default veryfast)
 *    WECAMP_FFMPEG_LOG       ffmpeg's stderr log        (default wecamp-ffmpeg.log)
 */

#if defined(_WIN32) && !defined(_XBOX) && !defined(__WINRT__)

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <compat/strl.h>

#include "../record_driver.h"
#include "../../runloop.h"
#include "../../verbosity.h"

#define WC_SLOTS          8     /* raw frames kept for the video thread */
#define WC_AUDIO_TARGET   0.080 /* s - steady audio queue (= video delay) */
#define WC_AUDIO_HIGH     0.400 /* s - above this, drop down to the target */
#define WC_AUDIO_RING_SEC 4
#define WC_RATE_ADJ       0.005 /* max resampling correction */
#define WC_IO_TIMEOUT_MS  30000 /* ffmpeg not reading for this long = dead */

typedef struct
{
   uint8_t *buf;
   unsigned w, h;
   size_t   pitch;  /* packed: w * bpp */
   double   ts;
} wc_slot_t;

typedef struct
{
   /* config */
   char     ffmpeg[MAX_PATH];
   char     url[2048];
   char     log_path[MAX_PATH];
   char     vbitrate[32], abitrate[32], preset[32];
   unsigned out_w, out_h, fps;
   unsigned rate;      /* core sample rate */
   unsigned bpp;       /* input bytes per pixel: 2 (RGB565) or 4 (XRGB8888) */
   size_t   slot_cap;

   /* frames: main thread writes slot[seq % N], video thread copies one out */
   CRITICAL_SECTION v_cs;
   wc_slot_t slot[WC_SLOTS];
   unsigned  seq;

   /* audio ring, stereo frames */
   CRITICAL_SECTION a_cs;
   int16_t *ring;
   size_t   ring_cap, ring_r, ring_fill;

   /* threads */
   HANDLE        stop_evt;
   HANDLE        supervisor;
   volatile LONG stopping;
   double        t0;   /* wall clock of output sample/frame 0, per ffmpeg run */

   /* stats */
   volatile LONG v_skipped, a_underruns, a_resyncs, restarts;
} wc_t;

static double wc_now(void)
{
   static LARGE_INTEGER fq;
   LARGE_INTEGER c;
   if (!fq.QuadPart)
      QueryPerformanceFrequency(&fq);
   QueryPerformanceCounter(&c);
   return (double)c.QuadPart / (double)fq.QuadPart;
}

static void wc_env(const char *name, char *out, size_t len, const char *def)
{
   DWORD n = GetEnvironmentVariableA(name, out, (DWORD)len);
   if (!n || n >= len)
      strlcpy(out, def, len);
}

/* "1500k" -> "3000k" (bufsize = 2 x bitrate); anything else is passed as is */
static void wc_double_rate(const char *in, char *out, size_t len)
{
   char *end = NULL;
   long  v   = strtol(in, &end, 10);
   if (v > 0 && end && (*end == 'k' || *end == 'K' || *end == 'M' || *end == 'm' || !*end))
      snprintf(out, len, "%ld%s", v * 2, end);
   else
      strlcpy(out, in, len);
}

/* --- overlapped pipe I/O that gives up when ffmpeg dies or we stop ------- */

static bool wc_wait_io(HANDLE pipe, OVERLAPPED *ov, HANDLE proc, HANDLE stop, DWORD timeout)
{
   HANDLE w[3];
   DWORD  r;
   w[0] = ov->hEvent;
   w[1] = proc;
   w[2] = stop;
   r    = WaitForMultipleObjects(3, w, FALSE, timeout);
   if (r == WAIT_OBJECT_0)
      return true;
   CancelIoEx(pipe, ov);
   {
      DWORD n;
      GetOverlappedResult(pipe, ov, &n, TRUE);
   }
   return false;
}

static bool wc_connect(HANDLE pipe, HANDLE ev, HANDLE proc, HANDLE stop)
{
   OVERLAPPED ov;
   DWORD n;
   memset(&ov, 0, sizeof(ov));
   ov.hEvent = ev;
   ResetEvent(ev);
   if (ConnectNamedPipe(pipe, &ov))
      return true;
   switch (GetLastError())
   {
      case ERROR_PIPE_CONNECTED:
         return true;
      case ERROR_IO_PENDING:
         if (!wc_wait_io(pipe, &ov, proc, stop, 20000))
            return false;
         return GetOverlappedResult(pipe, &ov, &n, FALSE) ? true : false;
      default:
         return false;
   }
}

static bool wc_write(HANDLE pipe, HANDLE ev, HANDLE proc, HANDLE stop,
      const void *data, size_t len)
{
   const uint8_t *p = (const uint8_t*)data;
   while (len)
   {
      OVERLAPPED ov;
      DWORD n     = 0;
      DWORD chunk = len > (1u << 20) ? (1u << 20) : (DWORD)len;
      memset(&ov, 0, sizeof(ov));
      ov.hEvent = ev;
      ResetEvent(ev);
      if (!WriteFile(pipe, p, chunk, NULL, &ov))
      {
         if (GetLastError() != ERROR_IO_PENDING)
            return false;
         if (!wc_wait_io(pipe, &ov, proc, stop, WC_IO_TIMEOUT_MS))
            return false;
      }
      if (!GetOverlappedResult(pipe, &ov, &n, FALSE) || !n)
         return false;
      p   += n;
      len -= n;
   }
   return true;
}

/* --- video ---------------------------------------------------------------- */

/* Nearest-neighbour scale of one raw frame onto the fixed RGB565 canvas.
 * PS1 output is always shown as 4:3, whatever its pixel size. */
static void wc_scale(const wc_t *wc, const wc_slot_t *s, uint16_t *dst)
{
   unsigned x, y;
   if (!s->buf || !s->w || !s->h)
   {
      memset(dst, 0, (size_t)wc->out_w * wc->out_h * 2);
      return;
   }
   for (y = 0; y < wc->out_h; y++)
   {
      const uint8_t *row = s->buf + (size_t)(y * s->h / wc->out_h) * s->pitch;
      uint16_t        *o = dst + (size_t)y * wc->out_w;
      if (wc->bpp == 4)
         for (x = 0; x < wc->out_w; x++)
         {
            uint32_t c = ((const uint32_t*)row)[x * s->w / wc->out_w];
            o[x] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
         }
      else
         for (x = 0; x < wc->out_w; x++)
            o[x] = ((const uint16_t*)row)[x * s->w / wc->out_w];
   }
}

/* Copies out the newest frame at least WC_AUDIO_TARGET old (or the oldest
 * kept one if all are newer). Returns false while no frame exists yet. */
static bool wc_pick_frame(wc_t *wc, wc_slot_t *out, double now)
{
   bool ok = false;
   EnterCriticalSection(&wc->v_cs);
   if (wc->seq)
   {
      unsigned n    = wc->seq < WC_SLOTS ? wc->seq : WC_SLOTS;
      unsigned pick = (wc->seq - n) % WC_SLOTS; /* oldest kept */
      unsigned i;
      for (i = 1; i <= n; i++)
      {
         unsigned idx = (wc->seq - i) % WC_SLOTS;
         if (wc->slot[idx].ts <= now - WC_AUDIO_TARGET)
         {
            pick = idx;
            break;
         }
      }
      out->w     = wc->slot[pick].w;
      out->h     = wc->slot[pick].h;
      out->pitch = wc->slot[pick].pitch;
      memcpy(out->buf, wc->slot[pick].buf, out->pitch * out->h);
      ok = true;
   }
   LeaveCriticalSection(&wc->v_cs);
   return ok;
}

/* --- audio ---------------------------------------------------------------- */

typedef struct
{
   wc_t  *wc;
   HANDLE pipe, proc;
   volatile LONG failed;
} wc_audio_job_t;

static DWORD WINAPI wc_audio_thread(LPVOID arg)
{
   wc_audio_job_t *job   = (wc_audio_job_t*)arg;
   wc_t           *wc    = job->wc;
   HANDLE          ev    = CreateEventA(NULL, TRUE, FALSE, NULL);
   const double    rate  = (double)wc->rate;
   size_t          cap   = wc->rate;              /* 1 s of output per write, max */
   int16_t        *out   = (int16_t*)malloc(cap * 4);
   uint64_t        n_out = 0;
   double          phase = 0.0;

   if (!ev || !out || !wc_connect(job->pipe, ev, job->proc, wc->stop_evt))
      goto fail;

   while (!wc->stopping)
   {
      double  now    = wc_now();
      int64_t target = (int64_t)((now - wc->t0) * rate);
      int64_t need   = target - (int64_t)n_out;
      size_t  i, used;
      double  fill, step;

      if (need < (int64_t)(rate * 0.010))
      {
         if (WaitForSingleObject(wc->stop_evt, 10) == WAIT_OBJECT_0)
            break;
         continue;
      }
      if (need > (int64_t)cap)
      {
         /* We fell >1 s behind (ffmpeg was slow): skip ahead. */
         n_out = (uint64_t)(target - (int64_t)(rate * 0.020));
         need  = target - (int64_t)n_out;
      }

      EnterCriticalSection(&wc->a_cs);
      if (wc->ring_fill > (size_t)(rate * WC_AUDIO_HIGH))
      {
         size_t drop   = wc->ring_fill - (size_t)(rate * WC_AUDIO_TARGET);
         wc->ring_r    = (wc->ring_r + drop) % wc->ring_cap;
         wc->ring_fill -= drop;
         phase         = 0.0;
         InterlockedIncrement(&wc->a_resyncs);
      }
      fill = (double)wc->ring_fill / rate;
      step = 1.0 + (fill - WC_AUDIO_TARGET) / WC_AUDIO_TARGET * WC_RATE_ADJ;
      if (step > 1.0 + WC_RATE_ADJ) step = 1.0 + WC_RATE_ADJ;
      if (step < 1.0 - WC_RATE_ADJ) step = 1.0 - WC_RATE_ADJ;

      /* Linear-interpolation resample from the ring into `out`. */
      for (i = 0; i < (size_t)need; i++)
      {
         size_t k = (size_t)phase;
         if (k + 1 < wc->ring_fill)
         {
            const int16_t *a = wc->ring + ((wc->ring_r + k) % wc->ring_cap) * 2;
            const int16_t *b = wc->ring + ((wc->ring_r + k + 1) % wc->ring_cap) * 2;
            double f = phase - (double)k;
            out[i * 2]     = (int16_t)(a[0] + (b[0] - a[0]) * f);
            out[i * 2 + 1] = (int16_t)(a[1] + (b[1] - a[1]) * f);
            phase += step;
         }
         else
         {
            /* Underrun (stall or late audio): silence for the rest. */
            memset(out + i * 2, 0, ((size_t)need - i) * 4);
            InterlockedIncrement(&wc->a_underruns);
            break;
         }
      }
      used = (size_t)phase;
      if (used > wc->ring_fill)
         used = wc->ring_fill;
      wc->ring_r     = (wc->ring_r + used) % wc->ring_cap;
      wc->ring_fill -= used;
      phase         -= (double)used;
      if (i < (size_t)need)
         phase = 0.0;
      LeaveCriticalSection(&wc->a_cs);

      if (!wc_write(job->pipe, ev, job->proc, wc->stop_evt, out, (size_t)need * 4))
         goto fail;
      n_out += (uint64_t)need;
   }
   free(out);
   CloseHandle(ev);
   return 0;

fail:
   if (!wc->stopping)
      InterlockedExchange(&job->failed, 1);
   free(out);
   if (ev)
      CloseHandle(ev);
   return 1;
}

/* --- ffmpeg process + supervisor ------------------------------------------ */

static HANDLE wc_spawn_ffmpeg(wc_t *wc, const char *vpipe, const char *apipe, int run)
{
   char cmd[8192], bufsize[32];
   const char *fmt = (!strncmp(wc->url, "rtmp://", 7) || !strncmp(wc->url, "rtmps://", 8))
      ? "-f flv -flvflags no_duration_filesize " : "";
   SECURITY_ATTRIBUTES sa;
   STARTUPINFOA si;
   PROCESS_INFORMATION pi;
   HANDLE log_h, nul_h;
   BOOL ok;

   wc_double_rate(wc->vbitrate, bufsize, sizeof(bufsize));
   snprintf(cmd, sizeof(cmd),
         "\"%s\" -hide_banner -nostdin -loglevel warning -stats -stats_period 10 "
         "-probesize 32 -analyzeduration 0 -thread_queue_size 64 "
         "-f rawvideo -pix_fmt rgb565le -s %ux%u -framerate %u -i \"%s\" "
         "-probesize 32 -analyzeduration 0 -thread_queue_size 1024 "
         "-f s16le -ar %u -ac 2 -i \"%s\" "
         "-map 0:v -map 1:a "
         "-c:v libx264 -preset %s -b:v %s -maxrate %s -bufsize %s "
         "-g %u -keyint_min %u -sc_threshold 0 -pix_fmt yuv420p "
         "-c:a aac -b:a %s -ar %u "
         "%s\"%s\"",
         wc->ffmpeg,
         wc->out_w, wc->out_h, wc->fps, vpipe,
         wc->rate, apipe,
         wc->preset, wc->vbitrate, wc->vbitrate, bufsize,
         wc->fps * 2, wc->fps * 2,
         wc->abitrate, wc->rate,
         fmt, wc->url);

   memset(&sa, 0, sizeof(sa));
   sa.nLength        = sizeof(sa);
   sa.bInheritHandle = TRUE;
   log_h = CreateFileA(wc->log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
         &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   nul_h = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
         &sa, OPEN_EXISTING, 0, NULL);
   if (log_h != INVALID_HANDLE_VALUE)
   {
      char line[256];
      SYSTEMTIME st;
      DWORD n;
      GetLocalTime(&st);
      snprintf(line, sizeof(line), "\r\n==== %04u-%02u-%02u %02u:%02u:%02u ffmpeg run %d (%ux%u@%u %s %s) ====\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            run, wc->out_w, wc->out_h, wc->fps, wc->vbitrate, wc->preset);
      WriteFile(log_h, line, (DWORD)strlen(line), &n, NULL);
   }

   memset(&si, 0, sizeof(si));
   si.cb         = sizeof(si);
   si.dwFlags    = STARTF_USESTDHANDLES;
   si.hStdInput  = nul_h;
   si.hStdOutput = log_h;
   si.hStdError  = log_h;
   memset(&pi, 0, sizeof(pi));
   /* Below normal: if the CPU is short, the emulation must win - a slow
    * encoder only costs skipped output frames, a slow emulation stalls it. */
   ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
         CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi);
   if (log_h != INVALID_HANDLE_VALUE)
      CloseHandle(log_h);
   if (nul_h != INVALID_HANDLE_VALUE)
      CloseHandle(nul_h);
   if (!ok)
   {
      RARCH_ERR("[WE Camp]: could not start ffmpeg (\"%s\"): error %lu\n",
            wc->ffmpeg, GetLastError());
      return NULL;
   }
   CloseHandle(pi.hThread);
   return pi.hProcess;
}

static HANDLE wc_make_pipe(const char *name, DWORD out_buf)
{
   return CreateNamedPipeA(name,
         PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
         PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
         1, out_buf, 0, 0, NULL);
}

/* One ffmpeg run: returns true if it ended because we are stopping. */
static bool wc_run_once(wc_t *wc, int run)
{
   char vname[128], aname[128];
   HANDLE vpipe = INVALID_HANDLE_VALUE, apipe = INVALID_HANDLE_VALUE;
   HANDLE proc = NULL, athread = NULL, ev = NULL;
   wc_audio_job_t job;
   size_t frame_bytes = (size_t)wc->out_w * wc->out_h * 2;
   uint16_t *canvas = (uint16_t*)malloc(frame_bytes);
   wc_slot_t cur;
   uint64_t k = 0;
   bool stopped = false;

   memset(&job, 0, sizeof(job));
   memset(&cur, 0, sizeof(cur));
   cur.buf = (uint8_t*)malloc(wc->slot_cap);
   snprintf(vname, sizeof(vname), "\\\\.\\pipe\\wecamp-v-%lu-%d", GetCurrentProcessId(), run);
   snprintf(aname, sizeof(aname), "\\\\.\\pipe\\wecamp-a-%lu-%d", GetCurrentProcessId(), run);

   if (!canvas || !cur.buf)
      goto done;
   memset(canvas, 0, frame_bytes);
   vpipe = wc_make_pipe(vname, 1u << 20);
   apipe = wc_make_pipe(aname, 64u << 10);
   ev    = CreateEventA(NULL, TRUE, FALSE, NULL);
   if (vpipe == INVALID_HANDLE_VALUE || apipe == INVALID_HANDLE_VALUE || !ev)
   {
      RARCH_ERR("[WE Camp]: could not create the ffmpeg pipes (error %lu)\n", GetLastError());
      goto done;
   }
   if (!(proc = wc_spawn_ffmpeg(wc, vname, aname, run)))
      goto done;

   /* ffmpeg opens the video input first and probes it before opening the
    * audio one, so video must already flow when the audio thread connects. */
   if (!wc_connect(vpipe, ev, proc, wc->stop_evt))
   {
      RARCH_ERR("[WE Camp]: ffmpeg did not open the video pipe - see %s\n", wc->log_path);
      goto done;
   }
   wc->t0      = wc_now();
   job.wc      = wc;
   job.pipe    = apipe;
   job.proc    = proc;
   athread     = CreateThread(NULL, 0, wc_audio_thread, &job, 0, NULL);
   if (!athread)
      goto done;
   RARCH_LOG("[WE Camp]: streaming (ffmpeg run %d)\n", run);

   while (!wc->stopping && !job.failed)
   {
      double due = wc->t0 + (double)k / wc->fps;
      double now = wc_now();
      if (due > now)
      {
         DWORD ms = (DWORD)((due - now) * 1000.0);
         if (ms && WaitForSingleObject(wc->stop_evt, ms) == WAIT_OBJECT_0)
            break;
         now = wc_now();
      }
      if (wc_pick_frame(wc, &cur, now))
         wc_scale(wc, &cur, canvas);
      if (!wc_write(vpipe, ev, proc, wc->stop_evt, canvas, frame_bytes))
         break;
      k++;
      now = wc_now();
      if (now - (wc->t0 + (double)k / wc->fps) > 1.0)
      {
         /* >1 s behind (ffmpeg could not keep up): drop to real time. */
         uint64_t nk = (uint64_t)((now - wc->t0) * wc->fps);
         InterlockedExchangeAdd(&wc->v_skipped, (LONG)(nk - k));
         k = nk;
      }
   }
   stopped = wc->stopping ? true : false;

done:
   /* Closing our ends gives ffmpeg EOF on both inputs: it flushes the
    * encoder, closes the RTMP connection/file and exits. */
   if (athread)
   {
      if (!stopped)
         SetEvent(wc->stop_evt); /* unblock the audio thread... */
      WaitForSingleObject(athread, INFINITE);
      if (!stopped && !wc->stopping)
         ResetEvent(wc->stop_evt); /* ...and re-arm it for the next run */
      CloseHandle(athread);
   }
   if (vpipe != INVALID_HANDLE_VALUE)
      CloseHandle(vpipe);
   if (apipe != INVALID_HANDLE_VALUE)
      CloseHandle(apipe);
   if (proc)
   {
      DWORD code = 0;
      if (WaitForSingleObject(proc, stopped ? 15000 : 3000) != WAIT_OBJECT_0)
      {
         RARCH_WARN("[WE Camp]: ffmpeg did not exit, terminating it\n");
         TerminateProcess(proc, 1);
         WaitForSingleObject(proc, 5000);
      }
      GetExitCodeProcess(proc, &code);
      RARCH_LOG("[WE Camp]: ffmpeg run %d ended (exit %lu)\n", run, code);
      CloseHandle(proc);
   }
   if (ev)
      CloseHandle(ev);
   free(canvas);
   free(cur.buf);
   return stopped;
}

static DWORD WINAPI wc_supervisor(LPVOID arg)
{
   wc_t *wc  = (wc_t*)arg;
   int   run = 0;
   while (!wc->stopping)
   {
      double started = wc_now();
      if (wc_run_once(wc, ++run))
         break;
      if (wc->stopping)
         break;
      InterlockedIncrement(&wc->restarts);
      {
         /* Quick retry after a long healthy run, backoff when it keeps failing. */
         DWORD wait = (wc_now() - started > 60.0) ? 1000 : (DWORD)(run < 5 ? run * 2000 : 30000);
         RARCH_WARN("[WE Camp]: ffmpeg stopped - restarting in %lu ms\n", wait);
         if (WaitForSingleObject(wc->stop_evt, wait) == WAIT_OBJECT_0)
            break;
      }
   }
   return 0;
}

/* --- record_driver_t ------------------------------------------------------ */

static void wc_free(void *data)
{
   wc_t *wc = (wc_t*)data;
   unsigned i;
   if (!wc)
      return;
   if (wc->supervisor)
   {
      InterlockedExchange(&wc->stopping, 1);
      SetEvent(wc->stop_evt);
      WaitForSingleObject(wc->supervisor, INFINITE);
      CloseHandle(wc->supervisor);
   }
   if (wc->stop_evt)
      CloseHandle(wc->stop_evt);
   for (i = 0; i < WC_SLOTS; i++)
      free(wc->slot[i].buf);
   free(wc->ring);
   DeleteCriticalSection(&wc->v_cs);
   DeleteCriticalSection(&wc->a_cs);
   free(wc);
}

static void *wc_init(const struct record_params *params)
{
   char tmp[64], size[32];
   unsigned i;
   wc_t *wc;

   if (!GetEnvironmentVariableA("WECAMP_BROADCASTER", tmp, sizeof(tmp)))
      return NULL;
   if (!(wc = (wc_t*)calloc(1, sizeof(*wc))))
      return NULL;
   InitializeCriticalSection(&wc->v_cs);
   InitializeCriticalSection(&wc->a_cs);

   wc_env("WECAMP_STREAM_URL", wc->url, sizeof(wc->url), "");
   if (!*wc->url)
   {
      RARCH_ERR("[WE Camp]: WECAMP_STREAM_URL is not set - nothing to stream to\n");
      goto error;
   }
   wc_env("WECAMP_FFMPEG",          wc->ffmpeg,   sizeof(wc->ffmpeg),   "ffmpeg.exe");
   wc_env("WECAMP_FFMPEG_LOG",      wc->log_path, sizeof(wc->log_path), "wecamp-ffmpeg.log");
   wc_env("WECAMP_STREAM_VBITRATE", wc->vbitrate, sizeof(wc->vbitrate), "1500k");
   wc_env("WECAMP_STREAM_ABITRATE", wc->abitrate, sizeof(wc->abitrate), "128k");
   wc_env("WECAMP_STREAM_PRESET",   wc->preset,   sizeof(wc->preset),   "veryfast");
   wc_env("WECAMP_STREAM_SIZE",     size,         sizeof(size),         "640x480");
   wc_env("WECAMP_STREAM_FPS",      tmp,          sizeof(tmp),          "30");
   if (sscanf(size, "%ux%u", &wc->out_w, &wc->out_h) != 2
         || wc->out_w < 64 || wc->out_h < 64 || wc->out_w > 1920 || wc->out_h > 1080)
      wc->out_w = 640, wc->out_h = 480;
   wc->out_w &= ~1u; /* yuv420p needs even sizes */
   wc->out_h &= ~1u;
   wc->fps = (unsigned)atoi(tmp);
   if (wc->fps < 10 || wc->fps > 60)
      wc->fps = 30;

   wc->rate = (unsigned)(params->samplerate + 0.5);
   if (wc->rate < 8000 || wc->rate > 192000)
      wc->rate = 44100;
   wc->bpp      = (params->pix_fmt == FFEMU_PIX_ARGB8888) ? 4 : 2;
   wc->slot_cap = (size_t)(params->fb_width ? params->fb_width : 1024)
                * (params->fb_height ? params->fb_height : 512) * wc->bpp;
   for (i = 0; i < WC_SLOTS; i++)
      if (!(wc->slot[i].buf = (uint8_t*)malloc(wc->slot_cap)))
         goto error;
   wc->ring_cap = (size_t)wc->rate * WC_AUDIO_RING_SEC;
   if (!(wc->ring = (int16_t*)malloc(wc->ring_cap * 4)))
      goto error;

   if (!(wc->stop_evt = CreateEventA(NULL, TRUE, FALSE, NULL)))
      goto error;
   if (!(wc->supervisor = CreateThread(NULL, 0, wc_supervisor, wc, 0, NULL)))
      goto error;

   {
      /* Log the destination without the stream key. */
      char shown[64];
      const char *p = strstr(wc->url, "://");
      if (p && (p = strchr(p + 3, '/')))
         snprintf(shown, sizeof(shown), "%.*s/...", (int)(p - wc->url), wc->url);
      else
         strlcpy(shown, wc->url, sizeof(shown));
      RARCH_LOG("[WE Camp]: broadcaster output %ux%u@%u %s/%s %s, audio %u Hz, input %s -> %s\n",
            wc->out_w, wc->out_h, wc->fps, wc->vbitrate, wc->abitrate, wc->preset,
            wc->rate, wc->bpp == 4 ? "XRGB8888" : "RGB565", shown);
   }
   return wc;

error:
   wc_free(wc);
   return NULL;
}

static bool wc_push_video(void *data, const struct record_video_data *vd)
{
   wc_t *wc = (wc_t*)data;
   wc_slot_t *s;
   size_t row;
   unsigned y;

   /* Catch-up fast-forward is not shown; a dupe keeps the previous frame. */
   if (!wc || !vd->data || vd->is_dupe || vd->pitch <= 0
         || (runloop_get_flags() & RUNLOOP_FLAG_FASTMOTION))
      return true;
   row = (size_t)vd->width * wc->bpp;
   if (!vd->width || !vd->height || row * vd->height > wc->slot_cap || row > (size_t)vd->pitch)
      return true;

   EnterCriticalSection(&wc->v_cs);
   s = &wc->slot[wc->seq % WC_SLOTS];
   for (y = 0; y < vd->height; y++)
      memcpy(s->buf + y * row, (const uint8_t*)vd->data + (size_t)y * vd->pitch, row);
   s->w     = vd->width;
   s->h     = vd->height;
   s->pitch = row;
   s->ts    = wc_now();
   wc->seq++;
   LeaveCriticalSection(&wc->v_cs);
   return true;
}

static bool wc_push_audio(void *data, const struct record_audio_data *ad)
{
   wc_t *wc = (wc_t*)data;
   const int16_t *in;
   size_t n, w;

   if (!wc || !ad->data || !ad->frames
         || (runloop_get_flags() & RUNLOOP_FLAG_FASTMOTION))
      return true;
   in = (const int16_t*)ad->data;
   n  = ad->frames;

   EnterCriticalSection(&wc->a_cs);
   if (n > wc->ring_cap)
   {
      in += (n - wc->ring_cap) * 2;
      n   = wc->ring_cap;
   }
   if (wc->ring_fill + n > wc->ring_cap)
   {
      /* Nobody is reading (ffmpeg restarting): drop the oldest. */
      size_t drop    = wc->ring_fill + n - wc->ring_cap;
      wc->ring_r     = (wc->ring_r + drop) % wc->ring_cap;
      wc->ring_fill -= drop;
   }
   w = (wc->ring_r + wc->ring_fill) % wc->ring_cap;
   while (n)
   {
      size_t chunk = wc->ring_cap - w;
      if (chunk > n)
         chunk = n;
      memcpy(wc->ring + w * 2, in, chunk * 4);
      wc->ring_fill += chunk;
      in            += chunk * 2;
      n             -= chunk;
      w              = 0;
   }
   LeaveCriticalSection(&wc->a_cs);
   return true;
}

static bool wc_finalize(void *data)
{
   wc_t *wc = (wc_t*)data;
   if (!wc || !wc->supervisor)
      return true;
   InterlockedExchange(&wc->stopping, 1);
   SetEvent(wc->stop_evt);
   WaitForSingleObject(wc->supervisor, INFINITE);
   CloseHandle(wc->supervisor);
   wc->supervisor = NULL;
   RARCH_LOG("[WE Camp]: stream finished - skipped video frames %ld, audio underruns %ld, "
         "audio resyncs %ld, ffmpeg restarts %ld\n",
         wc->v_skipped, wc->a_underruns, wc->a_resyncs, wc->restarts);
   return true;
}

const record_driver_t record_wecamp = {
   wc_init,
   wc_free,
   wc_push_video,
   wc_push_audio,
   wc_finalize,
   "wecamp",
};

#endif
