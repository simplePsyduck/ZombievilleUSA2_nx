/* port_config.h -- Zombieville USA 2's settings for the android32 runtime.
 *
 * Macros only: the runtime's C files, its assembly and the launcher all read
 * this (runtime/source/rt_settings.h). The Unity values follow the runtime's
 * notes for the one other Unity / Mono port on it (Disney Crossy Road). MIT.
 */
#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

/* ------------------------------------------------------------------ the game */
#define PORT_TITLE    "Zombieville USA 2"
#define PORT_NAME     "zombievilleusa2_nx"
#define PORT_PACKAGE  "com.MikaMobile.Zombieville2"
#define PORT_BANNER   "zombievilleusa2_nx: Zombieville USA 2 (Unity 5.4 / Mono, armeabi-v7a)"

/* The player's own APK, any name, in the game folder next to the .nro. It is
 * found by what is in it (the three Unity libraries) and the package. The
 * runtime renames it to game.apk (RT_APK_ADOPT) and the first launch rewrites
 * it with every asset stored (see zv2_setup_plan.c): Unity reads its data from
 * the APK in place, and would otherwise inflate each file at every read. */
#define PORT_APK_DESC "Zombieville USA 2 (com.MikaMobile.Zombieville2, armeabi-v7a)"
#define PORT_APK_ROLES                                                                      \
  {.what = "the game", .name = PORT_APK_DEFAULT_NAME,                                       \
   .need = (const char *const[]){"lib/armeabi-v7a/libunity.so", "lib/armeabi-v7a/libmono.so", \
                                 "lib/armeabi-v7a/libmain.so", NULL},                        \
   .package = PORT_PACKAGE, .flags = RT_APK_ADOPT | RT_APK_PACKAGE_BONUS}
#define PORT_LAUNCHER_START_NOTE "(the first start unpacks the game's libraries from the APK)"

/* libunity + libmono + libmain are ~20 MB mapped: one 32 MB region each */
#define PORT_SO_REGION_BYTES (32u * 1024 * 1024)

/* ------------------------------------------------------------------ Mono */
/* Mono's null checks are by explicit comparison, not by SIGSEGV: load-bearing
 * on the Crossy Road port, where the managed NullReferenceException path is
 * not taken through the fault handler. */
#define RT_EXTRA_ENV "MONO_DEBUG=explicit-null-checks"

/* ------------------------------------------------------------------ graphics */
/* Unity makes its own EGL context (GLES2) and swaps inside nativeRender(). */
#define RT_GL_SWAP_ENDS_FRAME 0
#define RT_MAIN_THREAD_NAME "UnityMain"

/* ------------------------------------------------------------------ audio */
/* libunity carries FMOD with an OpenSL ES output and a Java one
 * (org.fmod.FMODAudioDevice). The first log on a console showed it taking the
 * Java one -- no dlopen of libOpenSLES.so at all -- whose loop dcr_audio.c plays
 * through audout. With RT_DL_HAS_OPENSLES 0 libOpenSLES.so is reported absent, as
 * on the Crossy Road port, so FMOD does not try the other. */
#define RT_DL_HAS_OPENSLES 0

/* ------------------------------------------------------------------ input */
#define RT_PAD_MAX_PLAYERS 1

/* ------------------------------------------------------------------ Java */
#define RT_JNI_UNHANDLED_INSTANCE_SINGLETON 1
#define RT_JNI_UNHANDLED_BUILDER_RETURNS_SELF 1
#define RT_LOOPER_MAIN_IMPLICIT 0

/* ------------------------------------------------------------------ files */
#define RT_PATH_CWD_RELATIVE 0
#define RT_DIRCACHE 0
#define RT_MANIFEST_LOG_PREFIX "unity"
#define RT_EMU_FIXUPS 0

#endif
