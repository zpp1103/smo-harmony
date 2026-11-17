// M42A / S1: the host contract header of the nx2android Vulkan backend, as this slice needs it.
// Vendored from infra/nx2android/nxnative/dm/host/dm.h (146 lines, `NX/dm/host/dm.h`) — the parts the
// slice-1 file set (gfx_vk_core/tex/shader/subgroup/dq + nvn_dirty) actually mentions. The rest of the
// 146-line header (module_t, the loader/gate/RO API) belongs to the recording layer and comes in with S2.
//
// Difference from the original, on purpose:
//   LOG   -> hilog (tag "SMO", plus the vk_boot.log forensic channel our platform already has)
//   die   -> LOG + abort (same as the original)
// The 28 host hooks the plan lists (docs/m41-fork-plan.md §1.3) are declared here and implemented as
// empty stubs in ohos_dm_stub.c ("待 S2/S5 实现").
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "hilog/log.h"

// OHOS' musl does not declare gettid() in <unistd.h>; the vendored gfx_vk_tex.c uses it for its per-thread
// diagnostics. Same value as Android's gettid().
#ifndef gettid
#define gettid() ((pid_t)syscall(SYS_gettid))
#endif

// OHOS musl declares pthread_setname_np only under _GNU_SOURCE; the vendored gfx_vk_dq.c names its recorder
// thread. The declaration matches <pthread.h>:364 exactly (same signature, so a duplicate is harmless).
int pthread_setname_np(pthread_t thread, const char *name);
#include "compat/game_profile.h"   // constants of the current game profile (generated from dm/games/<id>/game.tsv)

#define MAX_MOD 128

// VKB_LOGF is the file-backed forensic channel of our own platform (platform/vk_boot.c). It survives
// hilog flow control, which on this emulator drops the first burst of lines (M3c finding).
void vk_boot_logf(const char *fmt, ...);

#define LOG(...) do {                                                            \
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "SMO", "[dm] " __VA_ARGS__);         \
    vk_boot_logf("[dm] " __VA_ARGS__);                                           \
} while (0)
#define die(...) do { LOG("FATAL " __VA_ARGS__); abort(); } while (0)

// ---- host hooks (empty stubs this slice: ohos_dm_stub.c) ----
uint64_t now_ns(void);
uint64_t guest_ns(void);
int dm_detclock_enabled(void);
void dm_guest_frame_tick(void);
uint64_t dm_nvn_frame_count(void);
uint64_t dm_nvn_sub_cpu_ns(void);
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc));
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len));
void dm_guest_write_prepare(void *addr, size_t len);
void crash_handlers_check(const char *when);
