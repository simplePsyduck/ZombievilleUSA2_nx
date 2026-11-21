/* zv2_boot.c -- plays the part of Zombieville's UnityPlayerActivity and
 * UnityPlayer, for Unity 5.4.0b21. Structure and order from the Crossy Road
 * port's dcr_boot.c (Unity 5.6.4f1); the natives are this APK's own.
 *
 * UnityPlayerActivity.onCreate, in Unity 5.4's order, and what happens here:
 *   System.loadLibrary("main")   libmain's JNI_OnLoad (the CPU check), which
 *                          forwards to libunity's; libunity RegisterNatives
 *                          the UnityPlayer methods (the runtime records them:
 *                          jni_native). When nothing is registered after
 *                          libmain's, libunity's own JNI_OnLoad is called.
 *   initJni(Context)       the activity
 *   nativeFile(apkPath)    where the game's data is: the APK
 *   nativeInitWWW/WebRequest(Class)
 *   surfaceChanged         nativeRecreateGfxState(0, Surface): libunity makes
 *                          its own EGL context on the window the runtime hands
 *                          ANativeWindow_fromSurface
 *   onResume / focus       nativeResume, nativeFocusChanged(true)
 *   render thread          nativeRender() until it returns false
 *   touch                  nativeInjectEvent(MotionEvent)   (dcr_input.c)
 *   HOME / sleep           nativeFocusChanged(false), nativePause
 *   exit                   nativePause, nativeDone
 * This (the main) thread IS Unity's render thread ("UnityMain"): the whole
 * frame, scripts included, runs inside nativeRender(). Before each frame the
 * Choreographer callback is run (zv2_choreo.c) and the engine's own looper is
 * drained (jni_looper_run_engine). MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "zv2.h"
#include "config.h"
#include "dcr_config.h"
#include "dcr_input.h"
#include "dcr_jni_unity.h"
#include "dcr_path.h"
#include "error.h"
#include "gl_layer.h"
#include "jit_arena.h"
#include "rt_applet.h"
#include "so_util.h"
#include "util.h"
#include "watchdog.h"

void dcr_watchdog_start(void);
void dcr_boost_poll(void);
void dcr_boost_launch_end(void);
void zv2_choreo_pump(void);
unsigned zv2_choreo_pumped(void);
extern volatile int g_dcr_quit_requested; /* Activity.finish / Application.Quit (jni_android.c) */

typedef jint (*fn_onload)(void *vm, void *reserved);
typedef void (*fn_obj)(void *env, void *thiz, void *a);
typedef void (*fn_gfx)(void *env, void *thiz, jint a, void *surface);
typedef void (*fn_v)(void *env, void *thiz);
typedef jboolean (*fn_z)(void *env, void *thiz);
typedef void (*fn_vz)(void *env, void *thiz, jboolean a);

static struct {
  fn_obj init_jni, file, init_www, init_webreq;
  fn_gfx recreate_gfx;
  fn_v resume, done;
  fn_z render, pause;
  fn_vz focus;
} U;

static void *g_thiz, *g_ctx, *g_surface;
static int g_resumed;
static volatile uint64_t g_frames_done;

/* for the watchdog (the runtime's watchdog.c) */
uint64_t dcr_boot_frames(void) { return g_frames_done; }

static void *native(const char *name, int required) {
  void *p = jni_native(ZV2_PLAYER, name);
  if (!p && required)
    debugPrintf("[boot] MISSING required native %s\n", name);
  else if (p)
    debugPrintf("[boot]   %-26s libunity+0x%lx\n", name,
                (unsigned long)((uintptr_t)p - (uintptr_t)unity_mod.load_virtbase));
  return p;
}

/* ------------------------------------------------------------- lifecycle */
static void do_resume(void) {
  if (g_resumed)
    return;
  debugPrintf("[boot] nativeResume, nativeFocusChanged(true)\n");
  if (U.resume)
    U.resume(g_jni_env, g_thiz);
  if (U.focus)
    U.focus(g_jni_env, g_thiz, 1);
  g_resumed = 1;
}

static void do_pause(void) {
  if (!g_resumed)
    return;
  debugPrintf("[boot] nativeFocusChanged(false), nativePause\n");
  dcr_input_release_all(); /* nothing stays held while the game is away */
  if (U.focus)
    U.focus(g_jni_env, g_thiz, 0);
  if (U.pause)
    U.pause(g_jni_env, g_thiz);
  g_resumed = 0;
}

/* HOME, sleep, an overlay: rt_applet.c calls these from rt_applet_poll in the
 * frame loop (it writes out the log ring and stops the game's clocks around
 * them). */
void port_focus_lost(void) { do_pause(); }
void port_focus_gained(void) { do_resume(); }

static int resolve_natives(void) {
  debugPrintf("[boot] UnityPlayer natives:\n");
  U.init_jni = (fn_obj)native("initJni", 1);
  U.recreate_gfx = (fn_gfx)native("nativeRecreateGfxState", 1);
  U.render = (fn_z)native("nativeRender", 1);
  U.file = (fn_obj)native("nativeFile", 0);
  U.resume = (fn_v)native("nativeResume", 0);
  U.pause = (fn_z)native("nativePause", 0);
  U.done = (fn_v)native("nativeDone", 0);
  U.focus = (fn_vz)native("nativeFocusChanged", 0);
  U.init_www = (fn_obj)native("nativeInitWWW", 0);
  U.init_webreq = (fn_obj)native("nativeInitWebRequest", 0);
  return (U.init_jni && U.recreate_gfx && U.render) ? 0 : -1;
}

/* System.loadLibrary("main"): libmain's JNI_OnLoad, which forwards to libunity's */
static int load_engine(void) {
  fn_onload m = (fn_onload)so_try_find_addr_rx(&main_mod, "JNI_OnLoad");
  if (m)
    debugPrintf("[boot] libmain JNI_OnLoad -> 0x%lx\n", (unsigned long)m(g_jni_vm, NULL));
  else
    debugPrintf("[boot] libmain has no JNI_OnLoad\n");
  if (!jni_native(ZV2_PLAYER, "nativeRender")) {
    fn_onload u = (fn_onload)so_try_find_addr_rx(&unity_mod, "JNI_OnLoad");
    if (u)
      debugPrintf("[boot] libunity JNI_OnLoad (libmain did not forward) -> 0x%lx\n",
                  (unsigned long)u(g_jni_vm, NULL));
  }
  return resolve_natives();
}

/* ------------------------------------------------------------- exit */
static void exit_guard(void *arg) {
  (void)arg;
  svcSleepThread(5000000000ll);
  debugPrintf("[boot] the game did not close within 5 s: ending the process\n");
  log_flush_ring();
  svcExitProcess();
}

static void exit_guard_start(void) {
  static Thread t;
  if (R_FAILED(threadCreate(&t, exit_guard, NULL, NULL, 0x4000, 0x2B, -2)) || R_FAILED(threadStart(&t)))
    debugPrintf("[boot] no exit backstop thread\n");
}

static void report(uint64_t frames) {
  static u64 last_tick;
  static unsigned long last_presented;
  u64 tick = armGetSystemTick();
  unsigned long presented = (unsigned long)dcr_gl_frames();
  double fps = last_tick ? (double)(presented - last_presented) * 1e9 /
                               (double)armTicksToNs(tick - last_tick)
                         : 0.0;
  last_tick = tick;
  last_presented = presented;
  debugPrintf("[boot] frame %llu (presented %lu, %.1f fps, %u vsync callbacks run; %d Java objects, "
              "JIT arena %u KB used)\n",
              (unsigned long long)frames, presented, fps, zv2_choreo_pumped(), jni_live_objects(),
              (unsigned)jit_used_kb());
  dcr_mono_report();
}

/* ------------------------------------------------------------- boot */
int zv2_boot_run(void) {
  jni_init();
  jni_looper_bind_engine(); /* this thread plays UnityMain: Handler() here posts to it */
  dcr_watchdog_start();     /* log flushing + hang reports from here on */
  if (load_engine() != 0)
    fatal_error("The engine did not start (see debug.log).\n\n"
                "Check that libmain.so, libunity.so and libmono.so come from the same\n"
                "copy of Zombieville USA 2 (1.6.1, Unity 5.4) as game.apk.");

  dcr_input_init();
  g_thiz = jni_singleton(ZV2_PLAYER);
  g_ctx = jni_singleton(ZV2_ACTIVITY);
  g_surface = jni_singleton("android/view/Surface");

  debugPrintf("[boot] initJni\n");
  U.init_jni(g_jni_env, g_thiz, g_ctx);
  debugPrintf("[boot] initJni returned\n");
  if (U.file) {
    JObj *apk = jni_str(DCR_ANDROID_APK); /* dcr_path.c maps it to game.apk */
    debugPrintf("[boot] nativeFile(%s)\n", DCR_ANDROID_APK);
    U.file(g_jni_env, g_thiz, apk);
    jni_release(apk);
  }
  if (U.init_www)
    U.init_www(g_jni_env, g_thiz, jni_class("com/unity3d/player/WWW")->obj);
  if (U.init_webreq)
    U.init_webreq(g_jni_env, g_thiz, jni_class("com/unity3d/player/UnityWebRequest")->obj);

  debugPrintf("[boot] nativeRecreateGfxState(0, surface)\n");
  U.recreate_gfx(g_jni_env, g_thiz, 0, g_surface);
  do_resume();
  debugPrintf("[boot] entering the frame loop\n");
  log_flush_ring();

  uint64_t frames = 0;
  int launch_done = 0;
  unsigned long quiet_at = 0;
  while (!g_dcr_quit_requested && appletMainLoop()) {
    rt_applet_poll(); /* focus: port_focus_lost / port_focus_gained */
    if (!rt_focused()) {
      svcSleepThread(50000000ll);
      continue;
    }
    dcr_input_frame();
    zv2_choreo_pump();       /* the display's refresh, for WaitVSync */
    jni_looper_run_engine(); /* what the engine posted to its own looper */
    dcr_boost_poll();
    jboolean alive = U.render(g_jni_env, g_thiz);
    frames++;
    g_frames_done = frames;

    unsigned long presented = (unsigned long)dcr_gl_frames();
    if (!launch_done && presented > 0) {
      launch_done = 1;
      dcr_boost_launch_end();
      debugPrintf("[boot] first frame presented (frame %llu)\n", (unsigned long long)frames);
      quiet_at = presented + 180;
    }
    /* From ~3 s after the first picture the log goes to a RAM ring (util.c),
     * written out every 300 frames and on HOME / exit / crash. */
    if (quiet_at && presented >= quiet_at) {
      quiet_at = 0;
      log_set_quiet(1);
    }
    if (frames <= 3 || frames % 600 == 0)
      report(frames);
    if (frames % 300 == 0)
      log_flush_ring();
    if (!alive && frames > 1) {
      debugPrintf("[boot] nativeRender reported the player finished\n");
      break;
    }
  }
  debugPrintf("[boot] leaving the frame loop after %llu frames\n", (unsigned long long)frames);
  log_set_quiet(0);
  log_flush_ring();
  rt_applet_stop();
  exit_guard_start();
  do_pause();
  if (U.done)
    U.done(g_jni_env, g_thiz);
  debugPrintf("[boot] the game has closed\n");
  log_flush_ring();
  return 0;
}
