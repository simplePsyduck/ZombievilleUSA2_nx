/* dcr_patches.h -- guarded hooks into the game's modules (dcr_patches.c). */
#ifndef DCR_PATCHES_H
#define DCR_PATCHES_H
#include <stdint.h>
#include "so_util.h"

/* 1 when the four words at mod+rva are the guard (what the offsets were found on) */
int dcr_guard_ok(so_module *mod, uint32_t rva, const uint32_t guard[4], const char *what);
/* an absolute branch from mod+rva to dst, when the guard matches; 1 = hooked */
int dcr_hook(so_module *mod, uint32_t rva, const uint32_t guard[4], void *dst, const char *what);
#endif
