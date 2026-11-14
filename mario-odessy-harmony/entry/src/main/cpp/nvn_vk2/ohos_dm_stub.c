// M42A / S1: the host hooks the nx2android backend expects from `dm/host` — **empty stubs, 全待 S2/S5 实现**.
//
// docs/m41-fork-plan.md §1.3 lists 28 `dm_*` hooks that `hle_nvn.c` needs (they arrive with the recording
// layer, S2), plus the `gfx_vk_*` side's `dm_front_window` / `dm_external_active` / `dm_perf_window` family.
// This slice links against none of hle_nvn and never presents, so every hook below returns a plausible
// default and implements no algorithm. They are declared here (and not taken from our
// third_party/d2r_gfx/host/dm.h, which has only 20 lines and is a different ABI tree) so that S2 can fill
// them in one place.
//
// Two hook groups are NOT stubs on purpose:
//   * dm_fault_hook_register / dm_guest_write_prep_register / dm_guest_write_prepare — the vendored
//     nvn_dirty.c really calls these. They store the callbacks, and dm_guest_write_prepare forwards to the
//     registered one; that is bookkeeping, not an algorithm, and the zero-copy dirty-page path that would
//     use it is off in this slice (DM_VK_NO_ZEROCOPY=1).
//   * AHardwareBuffer_* — these must fail, see compat/android/hardware_buffer.h.
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dm.h"
#include "gfx_vk.h"

// ---- time ----
__attribute__((weak)) uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
// Guest clock. S1 has no guest; the backend only wants a monotonic value that advances.
__attribute__((weak)) uint64_t guest_ns(void) { return now_ns(); }

// ---- the dirty-page hooks nvn_dirty.c really registers into ----
static int (*g_fault_hook)(void *addr, int is_write, void *uc);
static void (*g_write_prep)(void *addr, size_t len);

__attribute__((weak)) void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) { g_fault_hook = fn; }
__attribute__((weak)) int dm_fault_hook(void *addr, int is_write, void *uc) { return g_fault_hook ? g_fault_hook(addr, is_write, uc) : 0; }
__attribute__((weak)) void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) { g_write_prep = fn; }
__attribute__((weak)) void dm_guest_write_prepare(void *addr, size_t len) { if (g_write_prep) g_write_prep(addr, len); }

// ---- clock / frame hooks (hle_nvn.c, S2) ----
__attribute__((weak)) int dm_detclock_enabled(void) { return 0; }
__attribute__((weak)) void dm_guest_frame_tick(void) {}
static _Atomic uint64_t g_frames;
__attribute__((weak)) uint64_t dm_nvn_frame_count(void) { return atomic_load(&g_frames); }
__attribute__((weak)) uint64_t dm_nvn_sub_cpu_ns(void) { return 0; }
__attribute__((weak)) void dm_fps_lock_tick(void) {}
__attribute__((weak)) uint8_t *dm_fps_state = NULL;
__attribute__((weak)) void dm_fps_state_apply(void) {}

// ---- graphics lifecycle (S4/S5: real suspend/resume) ----
__attribute__((weak)) void dm_gfx_enter(void) {}
__attribute__((weak)) void dm_gfx_leave(void) {}
__attribute__((weak)) void dm_gfx_suspend(int on) { (void)on; }
__attribute__((weak)) void dm_vk_drain(void) {}
__attribute__((weak)) void dm_gl_drain(void) {}
__attribute__((weak)) __thread _Atomic int dm_host_work;

// ---- platform sensors: "no measurement available" ----
__attribute__((weak)) double dm_brightness_now(void) { return -1; }
__attribute__((weak)) int dm_thermal_now(void) { return -1; }
__attribute__((weak)) int dm_low_power_now(void) { return 0; }
__attribute__((weak)) int front_screen_max_fps(void) { return 0; }
__attribute__((weak)) int dm_sdk_major(void) { return 7; }   // SMO 1.4.1 ships SDK 7.3.2 (dm/games/0100000000010000/game.tsv)
__attribute__((weak)) int dm_glslc_gpu_code(int *major, int *minor) { if (major) *major = 0; if (minor) *minor = 0; return 1; }
__attribute__((weak)) uint32_t dm_guest_work_qos(unsigned *count) { if (count) *count = 0; return 0; }
__attribute__((weak)) uint64_t dm_present_period_adapt(uint64_t period) { return period; }

// ---- diagnostics / perf: no-ops (S5 fills these in) ----
__attribute__((weak)) void dm_backtrace(uint64_t fp) { (void)fp; }
__attribute__((weak)) void crash_handlers_check(const char *when) { (void)when; }
__attribute__((weak)) void dm_longframe_note(const char *what) { (void)what; }
__attribute__((weak)) void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { if (n) *n = 0; if (ns) *ns = 0; }
__attribute__((weak)) double dm_nvn_present_hold_ms(void) { return 0; }
__attribute__((weak)) void dm_perf_frame_start(uint64_t pace_ns, uint64_t pq_ns) { (void)pace_ns; (void)pq_ns; }
__attribute__((weak)) void dm_perf_frame_cost(int64_t replay_ns, int64_t gpu_ns) { (void)replay_ns; (void)gpu_ns; }
__attribute__((weak)) void dm_perf_present(double iv_ms) { (void)iv_ms; }
__attribute__((weak)) void dm_perf_replay_thread(void) {}
__attribute__((weak)) void dm_thread_cpu(char *out, size_t n, double secs) { (void)secs; if (out && n) out[0] = 0; }
__attribute__((weak)) void dm_yield_top(char *out, size_t n) { if (out && n) out[0] = 0; }

// ---- window: branch B has no window; the backend must see "no window" (fail-closed, M1's rule) ----
__attribute__((weak)) void *dm_front_window(void) { return NULL; }
__attribute__((weak)) int dm_external_active(void) { return 0; }
__attribute__((weak)) void dm_perf_window(void *w, double fps) { (void)w; (void)fps; }

// ---- AHardwareBuffer: never succeeds (branch B, DM_VK_NO_ZEROCOPY=1) ----
__attribute__((weak)) int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out) {
    (void)desc;
    if (out) *out = NULL;
    return -1;
}
__attribute__((weak)) void AHardwareBuffer_release(AHardwareBuffer *buf) { (void)buf; }
__attribute__((weak)) int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *buf, int fd) { (void)buf; (void)fd; return -1; }

// ---- M42A: every definition above is weak ----
// This project already implements some of these hooks (platform/gfx_bridge.c: now_ns, guest_ns,
// dm_fault_hook_register, dm_guest_write_prep_register, dm_guest_write_prepare; platform/threads.c:
// dm_main_yield_stats, dm_guest_work_qos). A stub must only fill what is missing, so the real ones win at link
// time and the S1 slice stays behaviour-neutral for the running game.
