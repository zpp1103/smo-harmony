/* OHOS 侧 plat_* 实现 —— 供 infra/BOTWRecompAndroid 的 NVN 回放层使用（M2 预备，暂不参与编译）。
 *
 * 依据：runtime/nvn 反查出的 9 个必需函数（2026-11-02 实测）：
 *   plat_frame_work_gpu  plat_now_ns  plat_render_work  plat_reset_guest
 *   plat_sleep_until_ns  plat_terminate  plat_thread_unpin
 *   plat_write_tracking_enable  plat_written_pages
 *
 * 参考实现：infra/BOTWRecompAndroid/runtime/platform/android.c（MIT, Copyright (c) 2026 Peiikai）
 * 本文件的归属与约束见 notes/05-参考/BOTWRecompAndroid.md。
 *
 * 分类（按移植难度）：
 *   A. 纯 POSIX，逐行可搬           → plat_now_ns / plat_sleep_until_ns / plat_terminate / plat_thread_unpin
 *   B. ADPF（Android 动态性能框架） → plat_frame_work / plat_render_work / plat_frame_work_gpu：OHOS 无对应，空实现
 *   C. 缺页写追踪                   → plat_reset_guest / plat_write_tracking_enable / plat_written_pages：
 *                                     参考实现是 mprotect 只读 + SIGSEGV 标记；OHOS 同样支持，但**第一版先关掉**
 *                                     （参考实现自己也留了 LARECOMP_NO_WRITE_TRACKING=1 的关闭路径）。
 *                                     关掉的代价：每帧多拷顶点/索引缓冲（BOTW 实测 ~90MB / ~7ms），功能正确性不受影响。
 *
 * 编译：M2 接入时把本目录加入 CMakeLists 的 HOST_SOURCES，或单独 target 链接进 libgame.so。
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

/* ---- A. 纯 POSIX ---- */

uint64_t plat_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t)
{
    struct timespec ts = { .tv_sec = (time_t)(t / 1000000000ull), .tv_nsec = (long)(t % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) { }
}

void plat_terminate(void) { _exit(0); }

/* OHOS 上不需要把线程钉在特定核上；保留符号，空实现。 */
void plat_thread_unpin(void) { }

/* ---- B. ADPF 等价物：OHOS 无对应，空实现（只影响调频提示，不影响正确性）---- */

void plat_frame_work(uint64_t work_ns, uint64_t target_ns) { (void)work_ns; (void)target_ns; }
void plat_render_work(uint64_t work_ns, uint64_t target_ns) { (void)work_ns; (void)target_ns; }
void plat_frame_work_gpu(uint64_t cpu_ns, uint64_t gpu_ns, uint64_t target_ns, uint64_t game_ns)
{
    (void)cpu_ns; (void)gpu_ns; (void)target_ns; (void)game_ns;
}

/* ---- C. 客体内存窗口与写追踪 ---- */

static uint64_t wt_lo, wt_hi;
static uint8_t *wt_state;          /* 每 4KB 一字节；NULL = 写追踪关闭 */
static int wt_enabled;

bool plat_reset_guest(uint64_t lo, uint64_t hi)
{
    if (wt_state && lo < wt_hi && hi > wt_lo) {
        uint64_t a = lo < wt_lo ? wt_lo : lo, e = hi > wt_hi ? wt_hi : hi;
        memset(wt_state + ((a - wt_lo) >> 12), 0, (size_t)((e - a) >> 12));
    }
    /* 参考实现：一次性映射整个客体地址窗，MAP_NORESERVE 避免立即占页。 */
    void *p = mmap((void *)(uintptr_t)lo, (size_t)(hi - lo), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == (void *)(uintptr_t)lo;
}

/* 第一版：不启用写追踪（等价参考实现的 LARECOMP_NO_WRITE_TRACKING=1）。
   启用后需要配套 SIGSEGV 处理器把页标脏再恢复可写——M2 第二版再做。 */
void plat_write_tracking_enable(uint64_t lo, uint64_t hi)
{
    (void)lo; (void)hi;
    wt_enabled = 0;
    wt_state = NULL;
}

/* 返回写入过的页。写追踪关闭时：调用方约定「未追踪 = 视为已写」，
   所以这里返回 0 并把 *page 置 0，等价于「全部算已写」，正确性优先于性能。 */
int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page)
{
    (void)addr; (void)size; (void)pages; (void)max;
    if (page) *page = 0;
    return 0;
}
