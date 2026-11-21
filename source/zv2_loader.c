/* zv2_loader.c -- loading Zombieville USA 2's three Unity libraries.
 *
 *   libmono.so   the Mono runtime (a JIT: it writes code at run time -- jit_arena.c)
 *   libmain.so   Unity's loader stub (CPU check; JNI_OnLoad)
 *   libunity.so  the engine (imports libmono's mono_* and libmain)
 * Loaded in the APK's order, bound once all three are staged (libunity imports
 * libmono's exports directly), sealed together. Java loads them with
 * System.loadLibrary, which runs the constructors: zv2_run_constructors, libmain,
 * libunity, libmono, as the Crossy Road port does. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "zv2.h"
#include "config.h"
#include "dcr_path.h"
#include "error.h"
#include "imports.h"
#include "so_util.h"
#include "util.h"

so_module main_mod, unity_mod, mono_mod;

static int load_one(so_module *m, const char *name) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dcr_game_root(), name);
  int rc = so_load(m, path, NULL, PORT_SO_REGION_BYTES);
  if (rc < 0) {
    const char *why = rc == -1 ? "cannot open it, or it is not a 32-bit ARM ELF"
                    : rc == -2 ? "out of memory"
                    : rc == -3 ? "larger than PORT_SO_REGION_BYTES"
                    : rc == -4 ? "too many program headers" : "?";
    debugPrintf("[boot] so_load(%s) failed rc=%d: %s\n", path, rc, why);
    return -1;
  }
  so_relocate(m);
  return 0;
}

static void resolve_one(so_module *m, const char *name) {
  int missing = so_resolve(m, dcr_imports, dcr_imports_count, 1);
  debugPrintf("[boot] %-12s %6u KB  staged %p -> code %p  (%d unresolved imports)\n", name,
              (unsigned)(m->load_size >> 10), m->load_base, m->load_virtbase, missing);
}

int zv2_load_modules(void) {
  if (load_one(&main_mod, DCR_LIB_MAIN) < 0 || load_one(&unity_mod, DCR_LIB_UNITY) < 0 ||
      load_one(&mono_mod, DCR_LIB_MONO) < 0)
    return -1;
  /* bound once every module is loaded: libunity's imports of libmono are addresses */
  resolve_one(&main_mod, DCR_LIB_MAIN);
  resolve_one(&unity_mod, DCR_LIB_UNITY);
  resolve_one(&mono_mod, DCR_LIB_MONO);
  so_finalize(&main_mod);
  so_finalize(&unity_mod);
  so_finalize(&mono_mod);
  so_flush_caches(&main_mod);
  so_flush_caches(&unity_mod);
  so_flush_caches(&mono_mod);
  debugPrintf("[boot] modules mapped RX/RW\n");
  return 0;
}

void zv2_run_constructors(void) {
  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&mono_mod);
  debugPrintf("[boot] module constructors done\n");
}
