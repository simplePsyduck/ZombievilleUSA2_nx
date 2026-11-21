/* dcr_offsets.h -- the places in Zombieville USA 2's libmono.so (the APK's Unity
 * 5.4.0b21 build) that mono_rt.c hooks or reads. Found by hand from the shipped
 * library (this project's NOTES.md says how), not by the Crossy Road port's
 * tools/offsets/derive_offsets.py, which needs capstone. The code is the same
 * as in that port's Mono: every guard below is identical to Crossy Road's.
 *
 * Every RVA carries the function's leading words as a guard; consumers verify
 * the guard before they hook or read and SKIP on mismatch, so another build of
 * the game is a logged line, not a wild write.
 *
 * Only libmono's offsets are here: the Time and vsync hooks of the Crossy Road
 * port are for Unity 5.6/2017's libunity and are not used (see NOTES.md). */
#ifndef DCR_OFFSETS_H
#define DCR_OFFSETS_H
#include <stdint.h>

/* mono_arch_flush_icache(code, size): the Linux cacheflush svc, `svc #0` with
 * r7 = 0xF0002 (at +0x10e134) */
#define DCR_MONO_RVA_FlushIcache 0x010e0f0u
static const uint32_t DCR_MONO_GUARD_FlushIcache[4] = {0xe92d0890u, 0xe28db008u, 0xe24dd014u, 0xe50b0018u};

/* GC_suspend_handler's body: the signal handler (SIGPWR 30 suspends, SIGXCPU
 * 24 restarts) that mono_rt.c does the work of for each thread. It reads
 * GC_stop_count and posts GC_suspend_ack_sem through the GOT; its stores
 * are me->stop_info.{signal +0xc, last_stop_count +0x10, stack_ptr +0x14}. */
#define DCR_MONO_RVA_SuspendHandler 0x02bc5d4u
static const uint32_t DCR_MONO_GUARD_SuspendHandler[4] = {0xe92d4810u, 0xe28db008u, 0xe24dd01cu, 0xe50b0020u};
/* GC_lookup_thread(pthread_t): exported, at this RVA */
#define DCR_MONO_RVA_LookupThread 0x02bb3dcu
static const uint32_t DCR_MONO_GUARD_LookupThread[4] = {0xe92d4810u, 0xe28db008u, 0xe24dd014u, 0xe50b0018u};
/* GOT slots (R_ARM_RELATIVE) -> &GC_stop_count, &GC_suspend_ack_sem */
#define DCR_MONO_GOT_StopCount 0x0392be4u
#define DCR_MONO_GOT_AckSem    0x0392becu

#define DCR_MONO_GC_SUSPEND_SIG 30u
#define DCR_MONO_GC_RESTART_SIG 24u
#define DCR_MONO_GCT_SIGNAL    0xc
#define DCR_MONO_GCT_LAST_STOP 0x10
#define DCR_MONO_GCT_STACK_PTR 0x14

#endif
