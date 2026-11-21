/* dcr_patches.c -- guarded hooks into the game's modules. MIT. */
#include <string.h>

#include "dcr_patches.h"
#include "util.h"

int dcr_guard_ok(so_module *mod, uint32_t rva, const uint32_t guard[4], const char *what) {
  const uint32_t *p = (const uint32_t *)((uintptr_t)mod->load_virtbase + rva);
  if (!memcmp(p, guard, 16))
    return 1;
  debugPrintf("[patch] %s at +0x%lx: GUARD MISMATCH (found %08lx %08lx %08lx %08lx) -- "
              "not touched: another build of the game needs its offsets found again "
              "(dcr_offsets.h)\n",
              what, (unsigned long)rva, (unsigned long)p[0], (unsigned long)p[1],
              (unsigned long)p[2], (unsigned long)p[3]);
  return 0;
}

int dcr_hook(so_module *mod, uint32_t rva, const uint32_t guard[4], void *dst, const char *what) {
  if (!dcr_guard_ok(mod, rva, guard, what))
    return 0;
  hook_arm((uintptr_t)mod->load_virtbase + rva, (uintptr_t)dst);
  return 1;
}
