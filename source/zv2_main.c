/* zv2_main.c -- Zombieville USA 2's part of the boot (the runtime's main.c runs
 * the rest: runtime/source/main.c).
 *
 * port_load() goes from the player's APK to the game's first code: the APK
 * rewritten stored and the three libraries unpacked when needed (the setup
 * plan), the libraries loaded and bound, Mono's hooks and the JIT's memory put
 * in before any engine code runs. port_run() runs their constructors and the
 * activity (zv2_boot.c). Nothing of the game is in this program: the APK is the
 * player's own. The order is the Crossy Road port's. MIT.
 */
#include <stdio.h>
#include <string.h>

#include "zv2.h"
#include "config.h"
#include "dcr_path.h"
#include "dcr_setup.h"
#include "error.h"
#include "jit_arena.h"
#include "rt_boot.h"

const char *port_apk_help(void) {
  return "Copy the APK of your own Zombieville USA 2 (com.MikaMobile.Zombieville2,\n"
         "armeabi-v7a) into that folder, next to the .nro, under any name ending\n"
         "in .apk. The game reads its data from it: the first launch rewrites it\n"
         "(assets stored) and unpacks the game's libraries from it.";
}

/* The paths worth a log line: Unity's asset bundles and its reads inside the
 * APK. Its routine probes under assets/bin/Data are not news. */
int dcr_path_traced(const char *p) {
  if (!p || strstr(p, "/assets/bin/Data/"))
    return 0;
  return strstr(p, "AssetBundles") || strstr(p, "jar:") || strstr(p, ".apk/") ||
         strstr(p, ".apk!") || strstr(p, "StreamingAssets");
}

/* The runtime's default would run the shared audout test; dcr_audio.c has its
 * own output (FMOD's pump), tested the same way. */
void dcr_audio_selftest(void);
void port_audio_selftest(void) { dcr_audio_selftest(); }

int port_load(const char *apk) {
  dcr_setup_from_apk(apk);
  if (zv2_load_modules() != 0)
    fatal_error("Could not load the game libraries from %s.\n\n"
                "They are unpacked from the APK (lib/armeabi-v7a/) on launch: delete\n"
                "libmain.so, libunity.so, libmono.so and .setup there to unpack them again.",
                dcr_game_root());
  /* hooks go in before any engine code runs (the constructors first) */
  dcr_mono_install(); /* mono_arch_flush_icache -> jit_flush, and the Boehm GC bridge */
  if (jit_arena_init() != 0)
    fatal_error("Could not reserve executable memory for Mono's JIT.");
  dcr_mono_hook_exceptions(); /* names the scripts behind "NullReferenceException" lines */
  return 0;
}

void port_run(void) {
  zv2_run_constructors();
  zv2_boot_run();
}
