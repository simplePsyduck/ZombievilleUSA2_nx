/* zv2.h -- what Zombieville USA 2's files share. MIT. */
#ifndef ZV2_H
#define ZV2_H
#include <stddef.h>
#include <stdint.h>

#include "jni.h"
#include "so_util.h"

extern so_module main_mod, unity_mod, mono_mod;

/* zv2_loader.c: libmono, libmain, libunity */
int zv2_load_modules(void);
void zv2_run_constructors(void);

/* zv2_boot.c: UnityPlayer's part, and the frame loop */
int zv2_boot_run(void);

/* mono_rt.c */
int dcr_mono_install(void);
void dcr_mono_hook_exceptions(void);
void dcr_mono_report(void);
#endif
