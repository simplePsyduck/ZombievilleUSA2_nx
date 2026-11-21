/* jit_arena.c -- executable memory for Mono's JIT (hardware path).
 *
 * From the Crossy Road port's jit_arena.c (proven on a console there), with its
 * emulator-only parts left out: the same design, the same self-test.
 *
 * WHY AN ARENA WITH TWO VIEWS
 * ---------------------------
 * Mono's code manager mmap()s PROT_READ|PROT_WRITE|PROT_EXEC chunks and then
 * writes code into them in place: emitted methods, trampolines, patched call
 * sites. Horizon never maps a page writable and executable at once, and
 * Mesosphere lets a code page go RW->RX exactly once, so there is no
 * per-allocation permission toggling either.
 *
 * So: one CodeMemory object (libnx jitCreate; Atmosphere permits mapping one's
 * own code memory) gives the SAME physical pages at two addresses:
 *     rx  -- what Mono is given, executes and reads;
 *     rw  -- where writes actually land.
 * Writes aimed at rx are redirected to rw by:
 *   - memcpy / memmove / memset (the runtime's bionic_mem.c, through cs_write
 *     below), which covers the bulk copy of every compiled method;
 *   - the data-abort handler (dcr_exc_mono.c, via the runtime's exc_handler.c),
 *     which emulates the remaining individual stores (call-site patching,
 *     trampolines) against rw (arm_store_emu.c).
 * Mono's instruction-cache flush -- an inline Linux `cacheflush` svc that
 * Horizon would reject -- is hooked to jit_flush() (mono_rt.c), which cleans rw
 * from the D-cache and invalidates rx in the I-cache.
 *
 * The runtime's codespace hooks (codespace.h) are overridden here: cs_mmap for
 * PROT_EXEC mappings, cs_munmap, cs_mprotect, cs_write, cs_rw_alias. MIT.
 */
#include <malloc.h>
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "bionic_pthread.h"
#include "code_flush.h"
#include "codespace.h"
#include "jit_arena.h"
#include "selfproc.h"
#include "so_util.h"
#include "util.h"

int b_sigaction(int sig, const struct b_sigaction *act, struct b_sigaction *old); /* bionic_signal.c */
uint32_t dcr_exc_jit_stores(void);                                                  /* dcr_exc_mono.c */

#define JIT_ARENA_BYTES (32u * 1024 * 1024)
#define PAGE 0x1000u

static Jit g_jit;
static uintptr_t g_rx, g_rw;
static size_t g_size, g_pages;
static int g_ready;
static Mutex g_lock;
static uint8_t *g_used; /* 1 byte per page: 0 free, 1 used, 2 used + block start */

/* Self-test 4's SIGSEGV handler: does what Mono's does -- edits the Linux ARM
 * ucontext (uc_mcontext at +20: r0 at +32, pc at +92) and returns. */
static volatile int g_sig_test_seen;
static void sig_test_handler(int sig, void *info, void *uc) {
  uint32_t *u = uc;
  const uint32_t *si = info;
  g_sig_test_seen = sig == 11 && si[0] == 11 && si[3] == 0; /* si_signo, si_addr */
  u[92 / 4] += 4;   /* skip the faulting load */
  u[32 / 4] = 1234; /* r0 */
}

/* Prove the things Mono depends on before Mono runs, so a failure is one clear
 * log line instead of a crash deep inside the JIT:
 *   1. a plain store to the rx view traps and is emulated onto rw
 *      (exc_handler.c + dcr_exc_mono.c; needs Atmosphere's user exception handlers);
 *   2. code written through rw executes from rx after jit_flush;
 *   3. code REwritten after it ran executes in its new form (the I-cache);
 *   4. a fault in JIT code is delivered to a SIGSEGV handler. */
static void jit_self_test(void) {
  volatile uint32_t *rx = (volatile uint32_t *)(g_rx + g_size - PAGE);
  volatile uint32_t *rw = (volatile uint32_t *)(g_rw + g_size - PAGE);
  debugPrintf("[jit] self-test 1: store to the execute-only view (expect one emulated fault)\n");
  log_flush_ring();
  uint32_t before = dcr_exc_jit_stores();
  rx[0] = 0xE3A0002Au; /* mov r0, #42 -- via the data-abort handler */
  rw[1] = 0xE12FFF1Eu; /* bx lr       -- directly */
  int t1 = rw[0] == 0xE3A0002Au && dcr_exc_jit_stores() == before + 1;
  debugPrintf("[jit] self-test 1: %s\n", t1 ? "OK (store emulated)" : "FAILED");
  jit_flush((void *)rx, 8);
  int (*fn)(void) = (int (*)(void))(uintptr_t)rx;
  int r = fn();
  debugPrintf("[jit] self-test 2: generated code returned %d: %s\n", r, r == 42 ? "OK" : "FAILED");
  rw[0] = 0xE3A00007u; /* mov r0, #7 */
  jit_flush((void *)rx, 8);
  r = fn();
  debugPrintf("[jit] self-test 3: rewritten code returned %d: %s\n", r,
              r == 7 ? "OK (instruction cache invalidated)" : "FAILED (stale instruction cache)");
  struct b_sigaction act = {(void *)sig_test_handler, 0, L_SA_SIGINFO, NULL}, old;
  b_sigaction(L_SIGSEGV, &act, &old);
  rw[0] = 0xE5900000u; /* ldr r0, [r0] */
  rw[1] = 0xE12FFF1Eu; /* bx lr */
  jit_flush((void *)rx, 8);
  int (*load)(int) = (int (*)(int))(uintptr_t)rx;
  r = load(0);
  b_sigaction(L_SIGSEGV, &old, NULL);
  debugPrintf("[jit] self-test 4: null load in JIT code -> handler -> resumed with %d: %s\n", r,
              r == 1234 && g_sig_test_seen ? "OK (signal delivered)" : "FAILED");
  rw[0] = rw[1] = 0;
  jit_flush((void *)rx, 8);
}

int jit_arena_init(void) {
  if (g_ready)
    return 0;
  g_size = JIT_ARENA_BYTES;
  g_pages = g_size / PAGE;
  g_used = calloc(g_pages, 1);
  if (!g_used)
    return -1;
  Result rc = jitCreate(&g_jit, g_size);
  if (R_FAILED(rc)) {
    debugPrintf("[jit] jitCreate(%u MB) failed 0x%x\n", (unsigned)(g_size >> 20), rc);
    return -1;
  }
  rc = jitTransitionToExecutable(&g_jit);
  if (R_FAILED(rc)) {
    debugPrintf("[jit] jitTransitionToExecutable failed 0x%x\n", rc);
    return -1;
  }
  g_rx = (uintptr_t)jitGetRxAddr(&g_jit);
  g_rw = (uintptr_t)jitGetRwAddr(&g_jit);
  g_ready = 1;
  g_cs_armed = 1; /* the runtime's memcpy/memmove/memset ask cs_write first */
  debugPrintf("[jit] arena %u MB: rx %p rw %p\n", (unsigned)(g_size >> 20), (void *)g_rx, (void *)g_rw);
  if (g_rx != g_rw)
    jit_self_test();
  return 0;
}

/* Page-granular first-fit allocator over the arena. Mono rarely frees code
 * (only dynamic methods and domain unload), so a simple map is enough. */
void *jit_alloc(size_t len) {
  if (!g_ready && jit_arena_init() < 0)
    return NULL;
  size_t need = (len + PAGE - 1) / PAGE;
  mutexLock(&g_lock);
  for (size_t i = 0; i + need <= g_pages;) {
    size_t j = 0;
    while (j < need && !g_used[i + j])
      j++;
    if (j == need) {
      g_used[i] = 2;
      for (size_t k = 1; k < need; k++)
        g_used[i + k] = 1;
      mutexUnlock(&g_lock);
      /* zeroed, as mmap's pages are: Mono relies on it */
      memset((void *)(g_rw + i * PAGE), 0, need * PAGE);
      return (void *)(g_rx + i * PAGE);
    }
    i += j + 1;
  }
  mutexUnlock(&g_lock);
  debugPrintf("[jit] arena exhausted (%u KB requested)\n", (unsigned)(len >> 10));
  return NULL;
}

int jit_free(void *rx, size_t len) {
  if (!jit_contains(rx))
    return -1;
  size_t i = ((uintptr_t)rx - g_rx) / PAGE;
  size_t n = (len + PAGE - 1) / PAGE;
  mutexLock(&g_lock);
  for (size_t k = 0; k < n && i + k < g_pages; k++)
    g_used[i + k] = 0;
  mutexUnlock(&g_lock);
  return 0;
}

int jit_contains(const void *p) {
  uintptr_t a = (uintptr_t)p;
  return g_ready && a >= g_rx && a < g_rx + g_size;
}

int jit_range_contains(const void *p, size_t n) {
  uintptr_t a = (uintptr_t)p;
  return g_ready && n && a < g_rx + g_size && a + n > g_rx;
}

void *jit_rw(const void *rx) {
  if (!jit_contains(rx))
    return (void *)rx;
  return (void *)(g_rw + ((uintptr_t)rx - g_rx));
}

/* pages of the arena in use now, for the periodic report */
size_t jit_used_kb(void) {
  size_t n = 0;
  mutexLock(&g_lock);
  for (size_t i = 0; i < g_pages; i++)
    n += g_used ? g_used[i] != 0 : 0;
  mutexUnlock(&g_lock);
  return n * (PAGE / 1024);
}

int jit_is_split(void) { return g_ready && g_rx != g_rw; }

/* The runtime's dcr_code_flush asks here first (code_flush.h): the arena's
 * ranges are cleaned through their writable view. */
int port_code_flush(void *code, size_t size) {
  if (!jit_contains(code))
    return 0;
  void *d = jit_rw(code);
  Result rc = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u64)(uintptr_t)d, size);
  if (R_FAILED(rc)) {
    static int warned;
    if (!warned++)
      debugPrintf("[jit] data-cache flush of %p+0x%x failed: 0x%x\n", d, (unsigned)size, rc);
  }
  dcr_icache_invalidate();
  return 1;
}

static volatile uint32_t g_jit_flushes;
uint32_t dcr_jit_flushes(void) { return g_jit_flushes; }

/* Replacement for Mono's mono_arch_flush_icache(code, size). */
void jit_flush(void *code, int size) {
  __atomic_add_fetch(&g_jit_flushes, 1, __ATOMIC_RELAXED);
  if (size > 0)
    dcr_code_flush(code, (size_t)size);
}

/* ------------------------------------------------- the runtime's codespace hooks */
void *cs_mmap(size_t len, int prot, const void *caller) {
  if (!(prot & L_PROT_EXEC))
    return NULL; /* plain memory: the shim's heap blocks */
  void *p = jit_alloc((len + PAGE - 1) & ~(size_t)(PAGE - 1));
  static int n;
  if (n++ < 32)
    debugPrintf("[mmap] exec %p+0x%x (from %p)\n", p, (unsigned)len, caller);
  return p;
}

int cs_munmap(void *addr, size_t len) {
  if (!jit_contains(addr))
    return 0;
  jit_free(addr, len);
  return 1;
}

int cs_mprotect(void *addr, size_t len, int prot, const void *caller) {
  (void)len, (void)prot, (void)caller;
  if (jit_contains(addr))
    return 1; /* the arena's pages never change permission */
  return so_find_module_by_addr(addr) != NULL;
}

int cs_write(void *dst, const void *src, size_t n, int c, int kind) {
  if (!jit_range_contains(dst, n))
    return 0;
  void *w = jit_rw(dst);
  if (kind == 0)
    memcpy(w, src, n);
  else if (kind == 1)
    memmove(w, src, n);
  else
    memset(w, c, n);
  return 1;
}

void *cs_rw_alias(void *addr, size_t len) {
  (void)len;
  return jit_rw(addr);
}

