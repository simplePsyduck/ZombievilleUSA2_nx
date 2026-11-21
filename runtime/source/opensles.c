/* opensles.c -- OpenSL ES for the game's sound, played through audout.
 * Compiled in when the port sets RT_OPENSLES to 1 (a8r); otherwise
 * android_ndk.c refuses slCreateEngine and this file is empty.
 *
 * An engine that plays through OpenSL ES (Gameloft's vox, which takes
 * vox::DriverAndroid::_InitOSL on every phone) does, disassembled:
 *
 *   slCreateEngine -> Realize -> GetInterface(SL_IID_ENGINE)
 *   CreateOutputMix -> Realize
 *   CreateAudioPlayer(src = buffer queue locator + PCM 44.1 kHz s16 stereo,
 *                     sink = the output mix, 1 interface: the buffer queue)
 *   -> Realize -> GetInterface(SL_IID_PLAY), GetInterface(<the queue>)
 *   -> RegisterCallback(callback, driver)
 *   callback() (mixes 1024 frames, Enqueue), SetPlayState(PLAYING)
 *
 * and from then on mixes and enqueues the next block inside the buffer-queue
 * callback. So this file is that much of OpenSL ES, honestly implemented:
 * objects with their interfaces (each an SLxxxItf, a pointer to a vtable
 * pointer), a player whose queue an audio thread drains -- each buffer
 * resampled to the device's 48 kHz, queued to audout (rt_audout_submit,
 * which blocks while its three buffers are pending: the pacing), then
 * dequeued, and the callback called, as Android's OpenSL does when a buffer
 * has played. The
 * microphone recorder is refused.
 *
 * The audout half is rt_audout.c, every port's; dcr_audio_out_* lend it to a
 * port that plays sound before the game has a player (a start screen).
 *
 * FMOD Ex's OpenSL output (ducktales_nx, hardware 2026-10-02) also needs:
 *   - SLAndroidConfigurationItf on the player: it asks CreateAudioPlayer for
 *     it as REQUIRED, and GetInterface's it before Realize (Android allows
 *     that for this interface) to set the stream type. Settings are taken
 *     and ignored: there is one stream here. Without it the player is
 *     refused and FMOD has no output at all.
 *   - its entry points by name: it does not import OpenSL ES, it dlopens
 *     libOpenSLES.so and dlsyms slCreateEngine and the SL_IID_* objects.
 *     rt_opensles_lookup() answers those for bionic_dl.c's dlsym.
 * A refused required interface is logged with its GUID.
 *
 * RT_OPENSLES_MIX 1 (soulcalibur_nx): many players at once, as a game with
 * a player per sound effect has them (SoulCalibur: one BGM player, a dozen
 * SE / voice players, each a mono or stereo buffer queue). Every playing
 * player is resampled into one 48 kHz block, at its own volume, pan and
 * playback rate, summed and clamped; a player's callback is called as each
 * of its buffers is used up. SLVolumeItf, SLPlaybackRateItf and SLSeekItf
 * (the last a stub, as it is for a buffer queue on Android) are provided on
 * players. 0 (the default): the one player above, unchanged. MIT.
 */
#include "rt_settings.h"

#ifndef RT_OPENSLES /* the same default as android_ndk.c's */
#define RT_OPENSLES 0
#endif

#ifndef RT_OPENSLES_MIX
#define RT_OPENSLES_MIX 0
#endif

#if RT_OPENSLES
#include <malloc.h>
#include <math.h>
#include <string.h>
#include <switch.h>

#include "rt_audout.h"
#include "util.h"

/* =============================================================== audout */
#define FRAMES_PER_BUF RT_AUDOUT_FRAMES /* one rt_audout_submit */

/* For a start screen, before the game has a player: the same audout, 48 kHz
 * stereo s16, FRAMES_PER_BUF frames per dcr_audio_out_submit. */
int dcr_audio_out_open(void) { return rt_audout_open(); }
unsigned dcr_audio_out_rate(void) { return rt_audout_rate(); }
void dcr_audio_out_submit(const int16_t *frames) { rt_audout_submit(frames); }

void dcr_audio_selftest(void) { rt_audout_selftest(); }

/* ================================================================ OpenSL */
typedef uint32_t SLresult;
typedef uint32_t SLuint32;
typedef uint8_t SLboolean_t; /* SLboolean is a 32-bit value in the ABI: passed as SLuint32 */

#define SL_RESULT_SUCCESS 0x0
#define SL_RESULT_PRECONDITIONS_VIOLATED 0x1
#define SL_RESULT_PARAMETER_INVALID 0x2
#define SL_RESULT_MEMORY_FAILURE 0x3
#define SL_RESULT_BUFFER_INSUFFICIENT 0x7
#define SL_RESULT_CONTENT_UNSUPPORTED 0x9
#define SL_RESULT_FEATURE_UNSUPPORTED 0xC

#define SL_OBJECT_STATE_UNREALIZED 1
#define SL_OBJECT_STATE_REALIZED 2
#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED 2
#define SL_PLAYSTATE_PLAYING 3
#define SL_DATALOCATOR_BUFFERQUEUE 0x6
#define SL_DATALOCATOR_OUTPUTMIX 0x4
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE 0x800007BDu
#define SL_DATAFORMAT_PCM 0x2
#define SL_BYTEORDER_LITTLEENDIAN 0x2

/* SLInterfaceID values: pointers to GUIDs (the SL_IID_* imports hold them). */
typedef struct { uint32_t d1; uint16_t d2, d3, d4; uint8_t n[6]; } SLGuid;
static const SLGuid k_iid_engine = {0x8d97c260, 0xddd4, 0x11db, 0x958f, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
static const SLGuid k_iid_play = {0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
static const SLGuid k_iid_bq = {0x2bc99cc0, 0xddd4, 0x11db, 0x8d99, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
static const SLGuid k_iid_asbq = {0x198e4940, 0xc5d7, 0x11df, 0xa2a6, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
static const SLGuid k_iid_record = {0xc5657aa0, 0xdddb, 0x11db, 0x82f7, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
static const SLGuid k_iid_cfg = {0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
static const SLGuid k_iid_volume = {0x09e8ede0, 0xddde, 0x11db, 0xb4f6, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
static const SLGuid k_iid_rate = {0x2e3b2a40, 0xddda, 0x11db, 0xa349, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
static const SLGuid k_iid_seek = {0xd43135a0, 0xdddc, 0x11db, 0xb458, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
const void *b_SL_IID_ENGINE = &k_iid_engine;
const void *b_SL_IID_PLAY = &k_iid_play;
const void *b_SL_IID_BUFFERQUEUE = &k_iid_bq;
const void *b_SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &k_iid_asbq;
const void *b_SL_IID_RECORD = &k_iid_record;
const void *b_SL_IID_ANDROIDCONFIGURATION = &k_iid_cfg;
const void *b_SL_IID_VOLUME = &k_iid_volume;
const void *b_SL_IID_PLAYBACKRATE = &k_iid_rate;
const void *b_SL_IID_SEEK = &k_iid_seek;

static int iid_is(const void *iid, const SLGuid *g) {
  return iid == g || (iid && !memcmp(iid, g, sizeof *g));
}

typedef struct SLObj SLObj;
/* One interface: `self` in every call is the address of such a slot. */
typedef struct {
  const void *vtbl;
  SLObj *o;
} Slot;

enum { K_ENGINE, K_MIX, K_PLAYER };

typedef void (*BqCallback)(const void *caller, void *ctx);

#define QCAP 16

struct SLObj {
  Slot obj;      /* SLObjectItf */
  Slot engine;   /* SLEngineItf (engine) */
  Slot play;     /* SLPlayItf (player) */
  Slot bq;       /* SLBufferQueueItf / SLAndroidSimpleBufferQueueItf (player) */
  Slot cfg;      /* SLAndroidConfigurationItf (player) */
  Slot vol;      /* SLVolumeItf (player, RT_OPENSLES_MIX) */
  Slot prate;    /* SLPlaybackRateItf (player, RT_OPENSLES_MIX) */
  Slot seek;     /* SLSeekItf (player, RT_OPENSLES_MIX) */
  int kind, state;
  /* player */
  Mutex lock;
  CondVar cv;
  volatile uint32_t play_state;
  struct { const void *buf; uint32_t size; } q[QCAP];
  uint32_t qcap, head, count, index;
  BqCallback cb;
  void *cb_ctx;
  int channels, bits;
  uint32_t rate;
  volatile int dead;
  /* the mixer's (RT_OPENSLES_MIX) */
  int16_t level_mb, pan_pm;   /* SLVolumeItf: millibels (<= 0), stereo position -1000..1000 */
  uint8_t mute, pan_on;
  int16_t rate_pm;            /* SLPlaybackRateItf: permille, 1000 = as recorded */
  double pos;                 /* in the head buffer, frames */
  int16_t prev[2];            /* the previous buffer's last frame */
  unsigned long frames_used;
};

/* ------------------------------------------------------- the audio thread */
static SLObj *volatile g_player;  /* the one player being played */
static volatile int g_paused, g_closing;
static volatile uint32_t g_writes;
static Thread g_thread;
static int g_thread_up;
static u64 g_t_first;
#if !RT_OPENSLES_MIX /* the one-player path's */
static volatile int g_in_callback;
static int16_t g_out[FRAMES_PER_BUF * 2];
static int g_out_n;
static double g_pos;
static int16_t g_prev[2];
static unsigned long g_frames_in;
#endif

uint32_t dcr_audio_writes(void) { return g_writes; }
void dcr_audio_pause(int paused) { g_paused = paused; }
void dcr_audio_close(void) {
  g_closing = 1;
  rt_audout_cancel(1); /* a submit waiting for a buffer returns */
  SLObj *p = g_player;
  if (p) {
    mutexLock(&p->lock);
    condvarWakeAll(&p->cv);
    mutexUnlock(&p->lock);
  }
}

#if !RT_OPENSLES_MIX /* the one-player path: resampling into g_out */
static inline void frame_at(const uint8_t *in, int i, int ch, int bits, int16_t out[2]) {
  if (i < 0) {
    out[0] = g_prev[0], out[1] = g_prev[1];
    return;
  }
  if (bits == 16) {
    const int16_t *s = (const int16_t *)in + i * ch;
    out[0] = s[0];
    out[1] = ch == 2 ? s[1] : s[0];
  } else {
    const uint8_t *s = in + i * ch;
    out[0] = (int16_t)(((int)s[0] - 128) << 8);
    out[1] = ch == 2 ? (int16_t)(((int)s[1] - 128) << 8) : out[0];
  }
}

/* Linear resampling to the device rate, continuous across buffers (index -1
 * is the previous buffer's last frame). rt_audout_submit blocks: the pacing. */
static void play_pcm(const uint8_t *in, uint32_t size, uint32_t rate, int ch, int bits) {
  const int frame_bytes = ch * (bits / 8);
  const int frames = (int)(size / (uint32_t)frame_bytes);
  if (frames <= 0)
    return;
  g_frames_in += (unsigned long)frames;
  const double step = (double)rate / (double)rt_audout_rate();
  int16_t a[2], b[2];
  while (g_pos < (double)(frames - 1)) {
    int i0 = (int)floor(g_pos);
    double t = g_pos - (double)i0;
    frame_at(in, i0, ch, bits, a);
    frame_at(in, i0 + 1, ch, bits, b);
    g_out[g_out_n * 2] = (int16_t)((double)a[0] + (double)(b[0] - a[0]) * t);
    g_out[g_out_n * 2 + 1] = (int16_t)((double)a[1] + (double)(b[1] - a[1]) * t);
    if (++g_out_n == FRAMES_PER_BUF) {
      rt_audout_submit(g_out);
      g_out_n = 0;
    }
    g_pos += step;
  }
  g_pos -= (double)frames;
  frame_at(in, frames - 1, ch, bits, g_prev);
}
#endif


#if RT_OPENSLES_MIX
/* ------------------------------------------------------------ the mixer */
#define MAX_PLAYERS 64
static RMutex g_list_lock; /* recursive: a callback may create or destroy players */
static SLObj *g_list[MAX_PLAYERS];
static int g_nlist;
static int32_t g_acc[FRAMES_PER_BUF * 2];

static void list_add(SLObj *o) {
  rmutexLock(&g_list_lock);
  if (g_nlist < MAX_PLAYERS)
    g_list[g_nlist++] = o;
  else
    debugPrintf("[audio] more than %d players: this one is not heard\n", MAX_PLAYERS);
  rmutexUnlock(&g_list_lock);
}
static void list_remove(SLObj *o) {
  rmutexLock(&g_list_lock);
  for (int i = 0; i < g_nlist; i++)
    if (g_list[i] == o) {
      g_list[i] = g_list[--g_nlist];
      break;
    }
  rmutexUnlock(&g_list_lock);
}

static inline void pframe(const SLObj *p, const uint8_t *in, int i, int16_t out[2]) {
  if (i < 0) {
    out[0] = p->prev[0], out[1] = p->prev[1];
    return;
  }
  if (p->bits == 16) {
    const int16_t *s = (const int16_t *)in + i * p->channels;
    out[0] = s[0];
    out[1] = p->channels == 2 ? s[1] : s[0];
  } else {
    const uint8_t *s = in + i * p->channels;
    out[0] = (int16_t)(((int)s[0] - 128) << 8);
    out[1] = p->channels == 2 ? (int16_t)(((int)s[1] - 128) << 8) : out[0];
  }
}

/* The gains of a player: its level and mute, then its stereo position (a
 * linear balance, as Android's for a stereo source). */
static void gains(const SLObj *p, float g[2]) {
  float v = p->mute ? 0.0f : p->level_mb <= -9600 ? 0.0f : powf(10.0f, (float)p->level_mb / 2000.0f);
  g[0] = g[1] = v;
  if (p->pan_on && p->pan_pm) {
    float pan = (float)p->pan_pm / 1000.0f;
    if (pan > 0)
      g[0] *= 1.0f - pan;
    else
      g[1] *= 1.0f + pan;
  }
}

/* Adds up to FRAMES_PER_BUF frames of p to g_acc; its buffers used up are
 * dequeued and the callback called (the lock let go meanwhile). 1 if it gave
 * anything. */
static int mix_player(SLObj *p) {
  int n = 0, any = 0;
  mutexLock(&p->lock);
  while (n < FRAMES_PER_BUF && !p->dead && !g_closing && p->play_state == SL_PLAYSTATE_PLAYING && p->count) {
    const uint8_t *buf = p->q[p->head].buf;
    const int fb = p->channels * (p->bits / 8);
    const int frames = (int)(p->q[p->head].size / (uint32_t)fb);
    const double step = (double)p->rate * (double)p->rate_pm / 1000.0 / (double)rt_audout_rate();
    float g[2];
    gains(p, g);
    int16_t a[2], b[2];
    while (n < FRAMES_PER_BUF && p->pos < (double)(frames - 1)) {
      int i0 = (int)floor(p->pos);
      float t = (float)(p->pos - (double)i0);
      pframe(p, buf, i0, a);
      pframe(p, buf, i0 + 1, b);
      g_acc[n * 2] += (int32_t)(((float)a[0] + (float)(b[0] - a[0]) * t) * g[0]);
      g_acc[n * 2 + 1] += (int32_t)(((float)a[1] + (float)(b[1] - a[1]) * t) * g[1]);
      n++;
      any = 1;
      p->pos += step;
    }
    if (p->pos < (double)(frames - 1))
      break; /* the block is full: this buffer goes on in the next */
    /* used up */
    p->pos -= (double)(frames > 0 ? frames : 1);
    if (p->pos < -1.0)
      p->pos = -1.0;
    if (frames > 0)
      pframe(p, buf, frames - 1, p->prev);
    p->frames_used += (unsigned long)(frames > 0 ? frames : 0);
    p->head = (p->head + 1) % p->qcap;
    p->count--;
    p->index++;
    BqCallback cb = p->cb;
    void *ctx = p->cb_ctx;
    mutexUnlock(&p->lock);
    if (cb && !p->dead)
      cb(&p->bq, ctx); /* the game enqueues its next buffer (or stops) */
    mutexLock(&p->lock);
  }
  mutexUnlock(&p->lock);
  return any;
}

static void audio_thread(void *arg) {
  (void)arg;
  static int16_t out[FRAMES_PER_BUF * 2];
  SLObj *snap[MAX_PLAYERS];
  int silent_blocks = 0;
  while (!g_closing) {
    if (g_paused) {
      svcSleepThread(10000000ll);
      continue;
    }
    rmutexLock(&g_list_lock);
    int n = g_nlist;
    memcpy(snap, g_list, (size_t)n * sizeof snap[0]);
    rmutexUnlock(&g_list_lock);
    memset(g_acc, 0, sizeof g_acc);
    int any = 0;
    for (int i = 0; i < n; i++)
      any |= mix_player(snap[i]); /* a destroyed player is kept in memory: see o_Destroy */
    if (!any) {
      /* nothing playing: a few blocks of silence keep audout fed across a
       * gap between sounds, then the thread rests */
      if (++silent_blocks > 8) {
        svcSleepThread(5000000ll);
        continue;
      }
    } else
      silent_blocks = 0;
    for (int i = 0; i < FRAMES_PER_BUF * 2; i++) {
      int32_t v = g_acc[i];
      out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    if (any && !g_writes++) {
      g_t_first = armGetSystemTick();
      debugPrintf("[audio] mixer: first block (%d player(s))\n", n);
    }
    rt_audout_submit(out); /* blocks while audout's buffers are full: the pacing */
    if (any && g_writes % 3000 == 0) {
      RtAudoutStats st;
      rt_audout_stats(&st);
      debugPrintf("[audio] %lu blocks mixed, %d player(s); %lu underruns, %lu failed submits\n",
                  (unsigned long)g_writes, n, st.underruns, st.append_fails);
    }
  }
}
#else /* !RT_OPENSLES_MIX */
static void audio_thread(void *arg) {
  (void)arg;
  while (!g_closing) {
    SLObj *p = g_player;
    if (!p || g_paused) {
      svcSleepThread(10000000ll);
      continue;
    }
    mutexLock(&p->lock);
    while (!g_closing && !p->dead && (p->play_state != SL_PLAYSTATE_PLAYING || !p->count || g_paused))
      condvarWaitTimeout(&p->cv, &p->lock, 20000000ll);
    if (g_closing || p->dead) {
      mutexUnlock(&p->lock);
      continue;
    }
    const void *buf = p->q[p->head].buf;
    uint32_t size = p->q[p->head].size, rate = p->rate;
    int ch = p->channels, bits = p->bits;
    mutexUnlock(&p->lock);

    if (!g_writes++) {
      g_t_first = armGetSystemTick();
      debugPrintf("[audio] first buffer: %u bytes (%u Hz, %d ch, %d bit)\n", (unsigned)size,
                  (unsigned)rate, ch, bits);
    }
    play_pcm(buf, size, rate, ch, bits);

    mutexLock(&p->lock);
    BqCallback cb = NULL;
    void *ctx = NULL;
    if (p->count) { /* not Clear()ed meanwhile */
      p->head = (p->head + 1) % p->qcap;
      p->count--;
      p->index++;
      cb = p->cb;
      ctx = p->cb_ctx;
    }
    g_in_callback = 1;
    mutexUnlock(&p->lock);
    if (cb && !p->dead)
      cb(&p->bq, ctx); /* the engine mixes the next block and enqueues it */
    g_in_callback = 0;

    if (g_writes % 3000 == 0) {
      double secs = (double)armTicksToNs(armGetSystemTick() - g_t_first) / 1e9;
      RtAudoutStats st;
      rt_audout_stats(&st);
      debugPrintf("[audio] %lu buffers; pulled at %.0f Hz over %.0f s; %lu underruns, %lu failed "
                  "submits (%lu dropped)\n",
                  (unsigned long)g_writes, secs > 0 ? (double)g_frames_in / secs : 0.0, secs,
                  st.underruns, st.append_fails, st.dropped);
    }
  }
}

#endif /* RT_OPENSLES_MIX */

static void start_thread(void) {
  if (g_thread_up)
    return;
  /* Above the game's threads (priority 59, dcr_sched.c): the mixing callback
   * is short, and a late buffer is an audible click. */
  if (R_FAILED(threadCreate(&g_thread, audio_thread, NULL, NULL, 0x10000, 0x2A, 2)) ||
      R_FAILED(threadStart(&g_thread))) {
    debugPrintf("[audio] could not start the audio thread\n");
    return;
  }
  g_thread_up = 1;
}

/* ---------------------------------------------------------- SLObjectItf */
static SLObj *obj_of(const void *self) { return self ? ((const Slot *)self)->o : NULL; }

static SLresult o_Realize(const void *self, SLuint32 async) {
  SLObj *o = obj_of(self);
  if (!o)
    return SL_RESULT_PARAMETER_INVALID;
  if (o->state == SL_OBJECT_STATE_REALIZED)
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  o->state = SL_OBJECT_STATE_REALIZED;
  if (o->kind == K_ENGINE)
    rt_audout_open();
  if (o->kind == K_PLAYER)
    start_thread();
  return SL_RESULT_SUCCESS;
}
static SLresult o_Resume(const void *self, SLuint32 async) { return SL_RESULT_SUCCESS; }
static SLresult o_GetState(const void *self, SLuint32 *state) {
  SLObj *o = obj_of(self);
  if (!o || !state)
    return SL_RESULT_PARAMETER_INVALID;
  *state = (SLuint32)o->state;
  return SL_RESULT_SUCCESS;
}
static SLresult o_GetInterface(const void *self, const void *iid, void *out) {
  SLObj *o = obj_of(self);
  if (!o || !out)
    return SL_RESULT_PARAMETER_INVALID;
  if (o->kind == K_PLAYER && iid_is(iid, &k_iid_cfg)) { /* before Realize too, as Android */
    *(const void **)out = &o->cfg;
    return SL_RESULT_SUCCESS;
  }
  if (o->state != SL_OBJECT_STATE_REALIZED)
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  const void *itf = NULL;
  if (o->kind == K_ENGINE && iid_is(iid, &k_iid_engine))
    itf = &o->engine;
  else if (o->kind == K_PLAYER && iid_is(iid, &k_iid_play))
    itf = &o->play;
  else if (o->kind == K_PLAYER && (iid_is(iid, &k_iid_bq) || iid_is(iid, &k_iid_asbq)))
    itf = &o->bq;
#if RT_OPENSLES_MIX
  else if (o->kind == K_PLAYER && iid_is(iid, &k_iid_volume))
    itf = &o->vol;
  else if (o->kind == K_PLAYER && iid_is(iid, &k_iid_rate))
    itf = &o->prate;
  else if (o->kind == K_PLAYER && iid_is(iid, &k_iid_seek))
    itf = &o->seek;
#endif
  if (!itf) {
    *(const void **)out = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  *(const void **)out = itf;
  return SL_RESULT_SUCCESS;
}
static SLresult o_RegisterCallback(const void *self, void *cb, void *ctx) { return SL_RESULT_SUCCESS; }
static void o_AbortAsyncOperation(const void *self) {}
static void o_Destroy(const void *self) {
  SLObj *o = obj_of(self);
  if (!o)
    return;
  if (o->kind == K_PLAYER) {
    mutexLock(&o->lock);
    o->dead = 1;
    o->play_state = SL_PLAYSTATE_STOPPED;
    o->count = 0;
    condvarWakeAll(&o->cv);
    mutexUnlock(&o->lock);
#if RT_OPENSLES_MIX
    list_remove(o); /* the mixer may hold it a block longer: it is not freed */
    static int ndestroyed;
    if (ndestroyed++ < 8)
      debugPrintf("[audio] player destroyed\n");
#else
    if (g_player == o) {
      g_player = NULL;
      while (g_in_callback)
        svcSleepThread(1000000ll);
    }
    debugPrintf("[audio] player destroyed\n");
#endif
    return; /* kept: the audio thread may still hold a pointer to it */
  }
  free(o);
}
static SLresult o_SetPriority(const void *self, int32_t prio, SLuint32 preempt) { return SL_RESULT_SUCCESS; }
static SLresult o_GetPriority(const void *self, int32_t *prio, SLuint32 *preempt) {
  if (prio)
    *prio = 0;
  if (preempt)
    *preempt = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult o_SetLossOfControlInterfaces(const void *self, int16_t n, const void **ids, SLuint32 on) {
  return SL_RESULT_SUCCESS;
}
static const void *const k_obj_vt[] = {
    o_Realize, o_Resume, o_GetState, o_GetInterface, o_RegisterCallback, o_AbortAsyncOperation,
    o_Destroy, o_SetPriority, o_GetPriority, o_SetLossOfControlInterfaces,
};

/* ------------------------------------------------------------ SLPlayItf */
static SLresult p_SetPlayState(const void *self, SLuint32 st) {
  SLObj *o = obj_of(self);
  if (!o || st < SL_PLAYSTATE_STOPPED || st > SL_PLAYSTATE_PLAYING)
    return SL_RESULT_PARAMETER_INVALID;
  mutexLock(&o->lock);
  o->play_state = st;
  if (st == SL_PLAYSTATE_STOPPED) { /* stopping returns every queued buffer */
    o->count = 0;
    o->index = 0;
    o->pos = 0;
    o->prev[0] = o->prev[1] = 0;
  }
  if (st == SL_PLAYSTATE_PLAYING)
    g_player = o;
  condvarWakeAll(&o->cv);
  mutexUnlock(&o->lock);
  static int logged;
  if (logged++ < 6)
    debugPrintf("[audio] SetPlayState(%s)\n",
                st == SL_PLAYSTATE_PLAYING ? "playing" : st == SL_PLAYSTATE_PAUSED ? "paused" : "stopped");
  return SL_RESULT_SUCCESS;
}
static SLresult p_GetPlayState(const void *self, SLuint32 *st) {
  SLObj *o = obj_of(self);
  if (!o || !st)
    return SL_RESULT_PARAMETER_INVALID;
  *st = o->play_state;
  return SL_RESULT_SUCCESS;
}
static SLresult p_GetDuration(const void *self, SLuint32 *ms) {
  if (ms)
    *ms = 0xFFFFFFFFu; /* SL_TIME_UNKNOWN */
  return SL_RESULT_SUCCESS;
}
static SLresult p_GetPosition(const void *self, SLuint32 *ms) {
  SLObj *o = obj_of(self);
  if (ms)
#if RT_OPENSLES_MIX
    *ms = o && o->rate ? (SLuint32)((u64)o->frames_used * 1000ull / o->rate) : 0;
#else
    *ms = o && o->rate ? (SLuint32)((u64)g_frames_in * 1000ull / o->rate) : 0;
#endif
  return SL_RESULT_SUCCESS;
}
static SLresult p_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult p_ok(void) { return SL_RESULT_SUCCESS; }
static const void *const k_play_vt[] = {
    p_SetPlayState, p_GetPlayState, p_GetDuration, p_GetPosition,
    p_ok /* RegisterCallback */, p_ok /* SetCallbackEventsMask */, p_ok /* GetCallbackEventsMask */,
    p_unsupported /* SetMarkerPosition */, p_ok /* ClearMarkerPosition */,
    p_unsupported /* GetMarkerPosition */, p_ok /* SetPositionUpdatePeriod */,
    p_unsupported /* GetPositionUpdatePeriod */,
};

/* ------------------------------------------------ SL(Android)BufferQueueItf */
static SLresult q_Enqueue(const void *self, const void *buf, SLuint32 size) {
  SLObj *o = obj_of(self);
  if (!o || !buf || !size)
    return SL_RESULT_PARAMETER_INVALID;
  mutexLock(&o->lock);
  if (o->count >= o->qcap) {
    mutexUnlock(&o->lock);
    return SL_RESULT_BUFFER_INSUFFICIENT;
  }
  uint32_t tail = (o->head + o->count) % o->qcap;
  o->q[tail].buf = buf;
  o->q[tail].size = size;
  o->count++;
  condvarWakeAll(&o->cv);
  mutexUnlock(&o->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult q_Clear(const void *self) {
  SLObj *o = obj_of(self);
  if (!o)
    return SL_RESULT_PARAMETER_INVALID;
  mutexLock(&o->lock);
  o->count = 0;
  o->index = 0;
  o->pos = 0;
  mutexUnlock(&o->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult q_GetState(const void *self, SLuint32 *st) {
  SLObj *o = obj_of(self);
  if (!o || !st)
    return SL_RESULT_PARAMETER_INVALID;
  mutexLock(&o->lock);
  st[0] = o->count;
  st[1] = o->index;
  mutexUnlock(&o->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult q_RegisterCallback(const void *self, BqCallback cb, void *ctx) {
  SLObj *o = obj_of(self);
  if (!o)
    return SL_RESULT_PARAMETER_INVALID;
  mutexLock(&o->lock);
  o->cb = cb;
  o->cb_ctx = ctx;
  mutexUnlock(&o->lock);
  return SL_RESULT_SUCCESS;
}
static const void *const k_bq_vt[] = {q_Enqueue, q_Clear, q_GetState, q_RegisterCallback};

/* ------------------------------------------- SLAndroidConfigurationItf */
/* SetConfiguration(key, value, size) / GetConfiguration(key, *size, value):
 * the stream type, the performance mode... taken, and ignored. */
static SLresult c_SetConfiguration(const void *self, const char *key, const void *value, SLuint32 size) {
  static int logged;
  if (logged++ < 4)
    debugPrintf("[audio] Android configuration: %s = %u (ignored)\n", key ? key : "?",
                value && size >= 4 ? (unsigned)*(const SLuint32 *)value : 0u);
  return SL_RESULT_SUCCESS;
}
static SLresult c_GetConfiguration(const void *self, const char *key, SLuint32 *size, void *value) {
  if (size) {
    if (value && *size >= 4)
      *(SLuint32 *)value = 0;
    *size = 4;
  }
  return SL_RESULT_SUCCESS;
}
static const void *const k_cfg_vt[] = {c_SetConfiguration, c_GetConfiguration};

#if RT_OPENSLES_MIX
/* --------------------------------------------------------- SLVolumeItf */
typedef int16_t SLmillibel;
typedef int16_t SLpermille;
static SLresult v_SetVolumeLevel(const void *self, SLmillibel level) {
  SLObj *o = obj_of(self);
  if (!o)
    return SL_RESULT_PARAMETER_INVALID;
  o->level_mb = level > 0 ? 0 : level;
  return SL_RESULT_SUCCESS;
}
static SLresult v_GetVolumeLevel(const void *self, SLmillibel *level) {
  SLObj *o = obj_of(self);
  if (!o || !level)
    return SL_RESULT_PARAMETER_INVALID;
  *level = o->level_mb;
  return SL_RESULT_SUCCESS;
}
static SLresult v_GetMaxVolumeLevel(const void *self, SLmillibel *max) {
  if (!max)
    return SL_RESULT_PARAMETER_INVALID;
  *max = 0; /* no gain above the source, as Android */
  return SL_RESULT_SUCCESS;
}
static SLresult v_SetMute(const void *self, SLuint32 mute) {
  SLObj *o = obj_of(self);
  if (!o)
    return SL_RESULT_PARAMETER_INVALID;
  o->mute = mute != 0;
  return SL_RESULT_SUCCESS;
}
static SLresult v_GetMute(const void *self, SLuint32 *mute) {
  SLObj *o = obj_of(self);
  if (!o || !mute)
    return SL_RESULT_PARAMETER_INVALID;
  *mute = o->mute;
  return SL_RESULT_SUCCESS;
}
static SLresult v_EnableStereoPosition(const void *self, SLuint32 on) {
  SLObj *o = obj_of(self);
  if (!o)
    return SL_RESULT_PARAMETER_INVALID;
  o->pan_on = on != 0;
  return SL_RESULT_SUCCESS;
}
static SLresult v_IsEnabledStereoPosition(const void *self, SLuint32 *on) {
  SLObj *o = obj_of(self);
  if (!o || !on)
    return SL_RESULT_PARAMETER_INVALID;
  *on = o->pan_on;
  return SL_RESULT_SUCCESS;
}
static SLresult v_SetStereoPosition(const void *self, SLpermille pos) {
  SLObj *o = obj_of(self);
  if (!o || pos < -1000 || pos > 1000)
    return SL_RESULT_PARAMETER_INVALID;
  o->pan_pm = pos;
  return SL_RESULT_SUCCESS;
}
static SLresult v_GetStereoPosition(const void *self, SLpermille *pos) {
  SLObj *o = obj_of(self);
  if (!o || !pos)
    return SL_RESULT_PARAMETER_INVALID;
  *pos = o->pan_pm;
  return SL_RESULT_SUCCESS;
}
static const void *const k_vol_vt[] = {
    v_SetVolumeLevel,       v_GetVolumeLevel,          v_GetMaxVolumeLevel,
    v_SetMute,              v_GetMute,                 v_EnableStereoPosition,
    v_IsEnabledStereoPosition, v_SetStereoPosition,    v_GetStereoPosition,
};

/* --------------------------------------------------- SLPlaybackRateItf */
#define RATE_MIN 500
#define RATE_MAX 2000
static SLresult r_SetRate(const void *self, SLpermille rate) {
  SLObj *o = obj_of(self);
  if (!o || rate < RATE_MIN || rate > RATE_MAX)
    return SL_RESULT_PARAMETER_INVALID;
  o->rate_pm = rate;
  return SL_RESULT_SUCCESS;
}
static SLresult r_GetRate(const void *self, SLpermille *rate) {
  SLObj *o = obj_of(self);
  if (!o || !rate)
    return SL_RESULT_PARAMETER_INVALID;
  *rate = o->rate_pm;
  return SL_RESULT_SUCCESS;
}
static SLresult r_SetPropertyConstraints(const void *self, SLuint32 c) { return SL_RESULT_SUCCESS; }
static SLresult r_GetProperties(const void *self, SLuint32 *c) {
  if (c)
    *c = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult r_GetCapabilitiesOfRate(const void *self, SLpermille rate, SLuint32 *c) {
  if (c)
    *c = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult r_GetRateRange(const void *self, uint8_t index, SLpermille *min, SLpermille *max,
                               SLpermille *step, SLuint32 *caps) {
  if (index > 0)
    return SL_RESULT_PARAMETER_INVALID;
  if (min)
    *min = RATE_MIN;
  if (max)
    *max = RATE_MAX;
  if (step)
    *step = 1;
  if (caps)
    *caps = 0;
  return SL_RESULT_SUCCESS;
}
static const void *const k_rate_vt[] = {r_SetRate, r_GetRate, r_SetPropertyConstraints,
                                        r_GetProperties, r_GetCapabilitiesOfRate, r_GetRateRange};

/* -------------------------------------------------------------- SLSeekItf */
/* A buffer queue has no position to seek to: the calls are taken. */
static SLresult s_SetPosition(const void *self, SLuint32 ms, SLuint32 mode) { return SL_RESULT_SUCCESS; }
static SLresult s_SetLoop(const void *self, SLuint32 on, SLuint32 a, SLuint32 b) { return SL_RESULT_SUCCESS; }
static SLresult s_GetLoop(const void *self, SLuint32 *on, SLuint32 *a, SLuint32 *b) {
  if (on)
    *on = 0;
  if (a)
    *a = 0;
  if (b)
    *b = 0;
  return SL_RESULT_SUCCESS;
}
static const void *const k_seek_vt[] = {s_SetPosition, s_SetLoop, s_GetLoop};
#endif /* RT_OPENSLES_MIX */

/* ---------------------------------------------------------- SLEngineItf */
static SLObj *new_obj(int kind) {
  SLObj *o = calloc(1, sizeof *o);
  if (!o)
    return NULL;
  o->kind = kind;
  o->state = SL_OBJECT_STATE_UNREALIZED;
  o->obj = (Slot){k_obj_vt, o};
  o->engine = (Slot){NULL, o};
  o->play = (Slot){k_play_vt, o};
  o->bq = (Slot){k_bq_vt, o};
  o->cfg = (Slot){k_cfg_vt, o};
#if RT_OPENSLES_MIX
  o->vol = (Slot){k_vol_vt, o};
  o->prate = (Slot){k_rate_vt, o};
  o->seek = (Slot){k_seek_vt, o};
  o->rate_pm = 1000;
#endif
  mutexInit(&o->lock);
  condvarInit(&o->cv);
  o->play_state = SL_PLAYSTATE_STOPPED;
  return o;
}

static SLresult e_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult e_CreateOutputMix(const void *self, const void **out, SLuint32 n, const void **ids,
                                  const SLuint32 *req) {
  if (!out)
    return SL_RESULT_PARAMETER_INVALID;
  SLObj *o = new_obj(K_MIX);
  if (!o)
    return SL_RESULT_MEMORY_FAILURE;
  *out = &o->obj;
  return SL_RESULT_SUCCESS;
}

typedef struct { void *locator, *format; } SLDataSource;
typedef struct {
  SLuint32 type, channels, milli_hz, bits, container, mask, endian;
} SLDataFormat_PCM;

static SLresult e_CreateAudioPlayer(const void *self, const void **out, const SLDataSource *src,
                                    const SLDataSource *snk, SLuint32 n, const void **ids,
                                    const SLuint32 *req) {
  if (!out || !src || !src->locator || !src->format)
    return SL_RESULT_PARAMETER_INVALID;
  const SLuint32 *loc = src->locator;
  const SLDataFormat_PCM *fmt = src->format;
  if ((loc[0] != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE && loc[0] != SL_DATALOCATOR_BUFFERQUEUE) ||
      fmt->type != SL_DATAFORMAT_PCM || (fmt->channels != 1 && fmt->channels != 2) ||
      (fmt->bits != 8 && fmt->bits != 16)) {
    debugPrintf("[audio] CreateAudioPlayer: locator 0x%x format %u (%u ch, %u bit) not supported\n",
                (unsigned)loc[0], (unsigned)fmt->type, (unsigned)fmt->channels, (unsigned)fmt->bits);
    return SL_RESULT_CONTENT_UNSUPPORTED;
  }
  for (SLuint32 i = 0; i < n; i++) {
    int known = iid_is(ids[i], &k_iid_bq) || iid_is(ids[i], &k_iid_asbq) || iid_is(ids[i], &k_iid_play) ||
                iid_is(ids[i], &k_iid_cfg)
#if RT_OPENSLES_MIX
                || iid_is(ids[i], &k_iid_volume) || iid_is(ids[i], &k_iid_rate) || iid_is(ids[i], &k_iid_seek)
#endif
        ;
    if (!known && req && req[i]) {
      const SLGuid *g = ids[i];
      debugPrintf("[audio] CreateAudioPlayer: a required interface is not provided (%08x-%04x-%04x-%04x)\n",
                  g ? (unsigned)g->d1 : 0u, g ? g->d2 : 0, g ? g->d3 : 0, g ? g->d4 : 0);
      return SL_RESULT_FEATURE_UNSUPPORTED;
    }
  }
  SLObj *o = new_obj(K_PLAYER);
  if (!o)
    return SL_RESULT_MEMORY_FAILURE;
  o->qcap = loc[1] ? (loc[1] < QCAP ? loc[1] : QCAP) : 2;
  o->channels = (int)fmt->channels;
  o->bits = (int)fmt->bits;
  o->rate = fmt->milli_hz / 1000u;
  if (!o->rate)
    o->rate = 44100;
  *out = &o->obj;
#if RT_OPENSLES_MIX
  list_add(o);
  static int nlogged;
  if (nlogged++ >= 12)
    return SL_RESULT_SUCCESS;
#endif
  debugPrintf("[audio] OpenSL player: %u Hz, %d ch, %d bit, %u buffers queued at most -> audout %u Hz\n",
              (unsigned)o->rate, o->channels, o->bits, (unsigned)o->qcap, rt_audout_rate());
  return SL_RESULT_SUCCESS;
}

static SLresult e_CreateAudioRecorder(void) {
  debugPrintf("[audio] CreateAudioRecorder: no microphone\n");
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult e_QueryNumSupportedInterfaces(const void *self, SLuint32 id, SLuint32 *n) {
  if (n)
    *n = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult e_QueryNumSupportedExtensions(const void *self, SLuint32 *n) {
  if (n)
    *n = 0;
  return SL_RESULT_SUCCESS;
}
static SLresult e_IsExtensionSupported(const void *self, const char *name, SLuint32 *yes) {
  if (yes)
    *yes = 0;
  return SL_RESULT_SUCCESS;
}
static const void *const k_engine_vt[] = {
    e_unsupported,         /* CreateLEDDevice */
    e_unsupported,         /* CreateVibraDevice */
    e_CreateAudioPlayer,   e_CreateAudioRecorder,
    e_unsupported,         /* CreateMidiPlayer */
    e_unsupported,         /* CreateListener */
    e_unsupported,         /* Create3DGroup */
    e_CreateOutputMix,
    e_unsupported,         /* CreateMetadataExtractor */
    e_unsupported,         /* CreateExtensionObject */
    e_QueryNumSupportedInterfaces,
    e_unsupported,         /* QuerySupportedInterfaces */
    e_QueryNumSupportedExtensions,
    e_unsupported,         /* QuerySupportedExtension */
    e_IsExtensionSupported,
};

SLresult b_slCreateEngine(const void **out, SLuint32 nopts, const void *opts, SLuint32 n,
                          const void **ids, const SLuint32 *req) {
  if (!out)
    return SL_RESULT_PARAMETER_INVALID;
  SLObj *o = new_obj(K_ENGINE);
  if (!o)
    return SL_RESULT_MEMORY_FAILURE;
  o->engine.vtbl = k_engine_vt;
  *out = &o->obj;
  debugPrintf("[audio] slCreateEngine: OpenSL ES over audout\n");
  return SL_RESULT_SUCCESS;
}

/* ---------------------------------------------------------- by name */
/* For dlsym on libOpenSLES.so (bionic_dl.c): an engine that dlopens OpenSL
 * instead of importing it finds the entry point and the interface IDs here
 * (the import table holds only what modules import). A data symbol's
 * address is the object's, as dlsym returns it. */
uintptr_t rt_opensles_lookup(const char *name) {
  static const struct {
    const char *name;
    const void *addr;
  } k[] = {
      {"slCreateEngine", (const void *)b_slCreateEngine},
      {"SL_IID_ENGINE", &b_SL_IID_ENGINE},
      {"SL_IID_PLAY", &b_SL_IID_PLAY},
      {"SL_IID_BUFFERQUEUE", &b_SL_IID_BUFFERQUEUE},
      {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", &b_SL_IID_ANDROIDSIMPLEBUFFERQUEUE},
      {"SL_IID_RECORD", &b_SL_IID_RECORD},
      {"SL_IID_ANDROIDCONFIGURATION", &b_SL_IID_ANDROIDCONFIGURATION},
      {"SL_IID_VOLUME", &b_SL_IID_VOLUME},
      {"SL_IID_PLAYBACKRATE", &b_SL_IID_PLAYBACKRATE},
      {"SL_IID_SEEK", &b_SL_IID_SEEK},
  };
  for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
    if (!strcmp(name, k[i].name))
      return (uintptr_t)k[i].addr;
  return 0;
}

#endif /* RT_OPENSLES */
