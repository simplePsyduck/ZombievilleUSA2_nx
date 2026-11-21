/* zv2_shims.c -- imports of libunity/libmono that the runtime's bionic layer
 * does not cover. Each is a bionic-ABI shim named b_<symbol> (found by
 * tools/gen_imports.py): the smallest honest answers. MIT. */
#include <stdint.h>
#include <stddef.h>

#include "bionic.h"

/* Mono's and Unity's float classification: the C99 function, not newlib's macro */
int b_isnan(double x) { return x != x; }

/* The scheduling policy of a new thread: Horizon schedules by priority only. */
int b_pthread_attr_setschedpolicy(void *attr, int policy) {
  (void)attr; (void)policy;
  return 0;
}

/* Unity's software splash locks the window for pixel access; GL draws here. A
 * failed lock is the answer a phone gives for a window that already has an
 * EGL surface. */
int b_ANativeWindow_lock(void *window, void *out_buffer, void *in_out_dirty) {
  (void)window; (void)out_buffer; (void)in_out_dirty;
  return -1;
}
int b_ANativeWindow_unlockAndPost(void *window) {
  (void)window;
  return -1;
}
