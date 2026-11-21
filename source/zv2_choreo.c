/* zv2_choreo.c -- Unity 5.4's frame pacing through Choreographer, answered here.
 *
 * libunity prints "Choreographer available: Enabling VSYNC timing" and from
 * then on every frame ends in a wait for a counter that only a
 * Choreographer.FrameCallback's doFrame(long) raises: on a phone, once per
 * display refresh. The callback is a Java interface proxy that libunity made
 * itself (jni_proxy.c), so there is no Java to run it: this file keeps the
 * callback that postFrameCallback registered and the frame loop runs its
 * doFrame (zv2_choreo_pump) once per frame, before nativeRender -- the same
 * pulse the Crossy Road port's dcr_vsync.c gives Unity 2017.4 (which it finds by
 * offsets in libunity; the proxy needs none). The pacing itself is then the
 * swap's: the GL layer's eglSwapBuffers waits for the display.
 *
 * The callback is one-shot, as Android's is: Unity registers it again from
 * inside doFrame. MIT. */
#include <switch.h>

#include "dcr_jni_unity.h"
#include "jni.h"
#include "util.h"

static JObj *g_cb;
static Mutex g_lock;
static unsigned g_pumped, g_posted;

jvalue zv2_h_choreo_getInstance(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(jni_singleton("android/view/Choreographer"));
}

jvalue zv2_h_choreo_post(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)m;
  mutexLock(&g_lock);
  JObj *old = g_cb;
  g_cb = a[0].l ? jni_retain(a[0].l) : NULL;
  g_posted++;
  mutexUnlock(&g_lock);
  if (old)
    jni_release(old);
  if (g_posted == 1)
    debugPrintf("[choreo] first postFrameCallback (%s)\n",
                a[0].l && jni_is_proxy(a[0].l) ? "an interface proxy: pumped per frame"
                                               : "NOT a proxy: cannot be run");
  return jv_none();
}

jvalue zv2_h_choreo_remove(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  mutexLock(&g_lock);
  JObj *old = g_cb;
  g_cb = NULL;
  mutexUnlock(&g_lock);
  if (old)
    jni_release(old);
  return jv_none();
}

/* Once per frame, on the frame loop's thread, before nativeRender. */
void zv2_choreo_pump(void) {
  mutexLock(&g_lock);
  JObj *cb = g_cb;
  g_cb = NULL;
  mutexUnlock(&g_lock);
  if (!cb)
    return;
  if (jni_is_proxy(cb)) {
    JMethod *m = jni_method(jni_class("android/view/Choreographer$FrameCallback"), "doFrame", "(J)V", 0);
    jvalue arg;
    arg.j = (jlong)armTicksToNs(armGetSystemTick());
    if (m)
      jni_proxy_call(cb, m, &arg);
    g_pumped++;
  }
  jni_release(cb);
}

unsigned zv2_choreo_pumped(void) { return g_pumped; }
