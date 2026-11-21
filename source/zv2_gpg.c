/* zv2_gpg.c -- a stand-in for Google Play Games' native library (libgpg.so).
 *
 * The game's Play Games plugin (GooglePlayGames, C#) talks to a native library
 * named "gpg" by P/Invoke: 507 functions (read from the game's own assembly
 * metadata; none of its code or data is used here). Startup.Start() calls
 * Authenticate, which reaches them. The APK's real libgpg.so needs Google Play
 * services (Java, GoogleApiClient) and is not loaded by this port, so the first
 * console run ended in DllNotFoundException inside Startup.Start and the game
 * never left its logo scene.
 *
 * This module is what the library does on a device with no Google account signed
 * in: sign-in finishes at once with ERROR_NOT_AUTHORIZED, the plugin tells the game
 * "not authenticated" (Authenticate's callback gets false), and nothing else is
 * asked of it. Everything the plugin may call is answered, and the answers say
 * "nothing there":
 *   - AndroidPlatformConfiguration_Construct / GameServices_Builder_Construct /
 *     GameServices_Builder_Create return real (non-null) handles, so the plugin's
 *     "use after dispose" checks pass; the Builder's setters are accepted, the
 *     sign-in callbacks (OnAuthActionStarted / OnAuthActionFinished) kept;
 *   - GameServices_IsAuthorized is false;
 *   - GameServices_StartAuthorizationUI runs the two callbacks, on the calling
 *     (main) thread, as SIGN_IN started and finished with ERROR_NOT_AUTHORIZED;
 *   - every other function returns 0 / false / NULL (void ones do nothing).
 * The enum values and delegate signatures are the plugin's own (AuthOperation
 * SIGN_IN = 1; AuthStatus ERROR_NOT_AUTHORIZED = -3; OnAuthActionStartedCallback
 * (int, void*), OnAuthActionFinishedCallback (int, int, void*)).
 *
 * Reached through the runtime's dlopen/dlsym hooks (bionic_dl.c: port_dl_builtin,
 * port_dl_owns, port_dl_builtin_sym). MIT. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define AUTH_SIGN_IN 1
#define AUTH_ERROR_NOT_AUTHORIZED (-3)

typedef void (*OnAuthStarted)(int op, void *arg);
typedef void (*OnAuthFinished)(int op, int status, void *arg);

/* the Builder, and the GameServices made from it, share this layout */
typedef struct {
  OnAuthStarted started;
  void *started_arg;
  OnAuthFinished finished;
  void *finished_arg;
} Gpg;

static int g_gpg_handle; /* its address is the handle dlopen returns */

/* ---- what has a real answer ---- */
static void *gpg_config_construct(void) { return calloc(1, 32); }
static int gpg_config_valid(void *self) { (void)self; return 1; }
static void *gpg_builder_construct(void) { return calloc(1, sizeof(Gpg)); }

static void gpg_set_started(Gpg *b, OnAuthStarted cb, void *arg) {
  if (b) {
    b->started = cb;
    b->started_arg = arg;
  }
}
static void gpg_set_finished(Gpg *b, OnAuthFinished cb, void *arg) {
  if (b) {
    b->finished = cb;
    b->finished_arg = arg;
  }
}
static void *gpg_builder_create(Gpg *b, void *config) {
  (void)config;
  Gpg *gs = calloc(1, sizeof *gs);
  if (gs && b)
    *gs = *b;
  debugPrintf("[gpg] GameServices created (Play Games stand-in: sign-in will report not authorized)\n");
  return gs;
}
static int gpg_is_authorized(Gpg *gs) { (void)gs; return 0; }
static void gpg_start_auth_ui(Gpg *gs) {
  debugPrintf("[gpg] StartAuthorizationUI: sign-in finishes with ERROR_NOT_AUTHORIZED\n");
  if (!gs)
    return;
  if (gs->started)
    gs->started(AUTH_SIGN_IN, gs->started_arg);
  if (gs->finished)
    gs->finished(AUTH_SIGN_IN, AUTH_ERROR_NOT_AUTHORIZED, gs->finished_arg);
}

/* Everything else: "nothing there". A 64-bit return, so a function returning a
 * 64-bit value (timestamps, scores) is zero in r0 and r1; arguments are ignored. */
static unsigned long long gpg_nothing(void) { return 0; }

static const struct {
  const char *name;
  void *fn;
} k_impl[] = {
    {"AndroidPlatformConfiguration_Construct", gpg_config_construct},
    {"AndroidPlatformConfiguration_Valid", gpg_config_valid},
    {"GameServices_Builder_Construct", gpg_builder_construct},
    {"GameServices_Builder_SetOnAuthActionStarted", gpg_set_started},
    {"GameServices_Builder_SetOnAuthActionFinished", gpg_set_finished},
    {"GameServices_Builder_Create", gpg_builder_create},
    {"GameServices_IsAuthorized", gpg_is_authorized},
    {"GameServices_StartAuthorizationUI", gpg_start_auth_ui},
};

/* ---- the runtime's dl hooks ---- */
void *port_dl_builtin(const char *base) {
  if (!strcmp(base, "gpg") || !strcmp(base, "libgpg") || !strcmp(base, "libgpg.so") ||
      !strcmp(base, "gpg.so")) {
    static int logged;
    if (!logged++)
      debugPrintf("[gpg] dlopen(%s): Play Games stand-in (the game's libgpg.so is not loaded)\n", base);
    return &g_gpg_handle;
  }
  return NULL;
}

int port_dl_owns(const void *h) { return h == &g_gpg_handle; }

uintptr_t port_dl_builtin_sym(void *h, const char *name) {
  (void)h;
  static int logged;
  for (unsigned i = 0; i < sizeof k_impl / sizeof k_impl[0]; i++)
    if (!strcmp(name, k_impl[i].name)) {
      debugPrintf("[gpg] dlsym(%s)\n", name);
      return (uintptr_t)k_impl[i].fn;
    }
  /* every function the plugin declares is a valid name; the first few are logged
   * so the log shows which of the 507 this game's code actually reaches */
  if (logged++ < 80)
    debugPrintf("[gpg] dlsym(%s): nothing there (returns 0)\n", name);
  return (uintptr_t)gpg_nothing;
}
