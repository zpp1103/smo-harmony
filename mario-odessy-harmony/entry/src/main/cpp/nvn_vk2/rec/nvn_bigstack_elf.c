// M44A / S2b-2 前置：dm_bigstack_enter 的**真身**（512 KB 大栈保护），取代 M43B 的直通实现。
//
// nx2 原实现在 dm/host/hle_bigstack.c（Mach-O 汇编）：进来看 sp 是否落在客体线程栈区，是就切到
// 本线程 512 KB 的宿主大栈再调实现。宿主侧 NVN 实现在提交/上屏/建纹理时做 Metal 工作，而 Metal 驱动
// 会在深栈上临时编译内部程序（fast-clear blit program 等）——BOTW 的渲染线程栈装不下（AGX 编译器里
// SIGBUS，地址落在客体栈区）。本移植没有 nx2 的"客体栈区"建模，所以这里不照抄那个判据，
// 改用本单能证明的三条：
//   ① **每个线程各自一份** 512 KB 大栈（nx2 也是 per-thread："this thread's 512 KB host stack"）；
//   ② 大栈**下沿有 4 KB PROT_NONE 守卫页**：真的踩穿会立刻 SIGSEGV 在守卫页上，不会静默改写相邻映射；
//   ③ **可重入安全**：若当前 sp 已经在本线程大栈区间内（= 实现里又调了一次 NVN），不再切栈（否则会
//     把栈顶/栈底搞反），按 nx2 "sp 不在需要切的区间" 的同一分支直通。
//
// 为什么不用 pthread_attr_setstack / sigaltstack（两条都不能用）：
//   · pthread_attr_setstack 只在 pthread_create 时生效，而被包装的 NVN 实现是在**已经在跑的**
//     游戏线程上被调用的（线程由游戏自己创建），没法事后改它的栈 —— 除非把整个宿主改成"每个 NVN
//     调用都扔进自建线程"，那是改执行模型，不是本单的"就绪"。
//   · sigaltstack 只给信号处理函数换栈，替不了普通调用。
// 所以走**自管栈**：mmap(512K+4K) → 4 KB 守卫页 PROT_NONE → 页对齐顶（天然 16 字节对齐）。
//
// 回退路径（任何一步失败都不改变语义）：mmap/mprotect 失败、对齐断言失败、或强制失能时，
//   dm_bigstack_prepare 返回 top=0 → 入口桩在**调用方原栈**上 blr 过去，即 M43B 的直通语义。
//
// 切换时机：只有执行切换（S2b-2 把 nvn_slot[].fn 接上）后，nvn_big_entry 才会 `b dm_bigstack_enter`。
// 本单跳板不接执行（nvn_slot[].fn 全 0、nvn_big_real[] 全 0），所以本函数在正常游戏流程里**从不被调用**；
// 它只由 DM_M44A_BIGSTACK_TEST=1 的**自检**驱动（见 dm_bigstack_selftest，不动任何执行路径）。
//
// 入口寄存器约定（nvn_entry_elf.S 的 nvn_big_entry）：进来时 x16 = 本函数的 slot 地址，它已把
// nvn_big_real[i] 换进 x16 再 `b` 到这里；参数 x0-x7 / q0-q7 原样，栈上无参数（生成器已验证）。
// 本桩必须**原样**把 x0-x7 / q0-q7 / x8（间接返回指针）传给实现，并把返回值 x0/x1 + q0-q3 交回，
// 因此全程保存/恢复这些寄存器（x16/x17 是 IP0/IP1，非参数寄存器，可作暂存）。
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DM_BIGSTACK_BYTES (512u * 1024u)     // 512 KB：nx2 的同一数值
#define DM_BIGSTACK_GUARD 4096u              // 守卫页（下沿）
#define DM_BIGSTACK_ALIGN 16u                // AAPCS64：公共接口处 sp 必须 16 字节对齐

_Static_assert(DM_BIGSTACK_BYTES == 524288u, "M44A: big stack must be exactly 512 KB (nx2)");
_Static_assert((DM_BIGSTACK_BYTES % DM_BIGSTACK_ALIGN) == 0, "M44A: big stack size must be a multiple of 16");
_Static_assert((DM_BIGSTACK_BYTES % DM_BIGSTACK_GUARD) == 0, "M44A: big stack size must be a multiple of the guard page");
_Static_assert((DM_BIGSTACK_GUARD % DM_BIGSTACK_ALIGN) == 0, "M44A: guard page must be a multiple of 16");
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "M44A: AArch64 little-endian only");

extern void vk_boot_logf(const char *fmt, ...);
static void bs_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void bs_log(const char *fmt, ...) {
    char b[512]; va_list ap;
    __builtin_va_start(ap, fmt); __builtin_vsnprintf(b, sizeof b, fmt, ap); __builtin_va_end(ap);
    vk_boot_logf("[m44a] %s", b);
}

// ---------------------------------------------------------------------------
// 每线程状态。__thread：本桩由游戏自己的多线程调用，一份栈不够（两个线程同时进来会互相踩栈）。
// ---------------------------------------------------------------------------
static __thread uint8_t *t_base;   // 可用区间下沿（守卫页之上）
static __thread uint8_t *t_top;    // 可用区间上沿（切栈时的新 sp）
static __thread int      t_state;  // 0 = 未初始化, 1 = 就绪, -1 = 初始化失败（回退）
static __thread unsigned t_seq;    // 本线程第几次进入（日志节流用）

static _Atomic uint32_t g_enter, g_switch, g_fallback, g_reenter;   // 全程计数（自检读）
static _Atomic int      g_force_fail;                              // 自检用：强制回退分支

typedef struct { uint64_t top, status; } bs_prep;
// status: 0 = 不切（回退/失败），1 = 切到本线程大栈，2 = 已在大栈内（重入，不切）
// 返回值用 {u64,u64} 结构体：AAPCS64 下正好落在 x0/x1，汇编侧不用碰内存。
bs_prep dm_bigstack_prepare(uint64_t cur_sp) __attribute__((visibility("hidden")));

static void bs_init_tls(void) {
    size_t want = (size_t)DM_BIGSTACK_BYTES + DM_BIGSTACK_GUARD;
    void *m = mmap(NULL, want, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        t_state = -1;
        bs_log("bigstack init FAIL mmap(%u) errno=%d -> fallback", (unsigned)want, errno);
        return;
    }
    if (((uintptr_t)m & (DM_BIGSTACK_GUARD - 1)) != 0) {   // mmap 必然页对齐；不对就是环境异常
        t_state = -1;
        bs_log("bigstack init FAIL map base %p not page aligned -> fallback", m);
        munmap(m, want);
        return;
    }
    if (mprotect(m, DM_BIGSTACK_GUARD, PROT_NONE) != 0) {
        t_state = -1;
        bs_log("bigstack init FAIL mprotect(guard) errno=%d -> fallback", errno);
        munmap(m, want);
        return;
    }
    uint8_t *base = (uint8_t *)m + DM_BIGSTACK_GUARD;
    uint8_t *top = base + DM_BIGSTACK_BYTES;
    // 断言：区间大小恰好 512 KB、栈顶 16 字节对齐、栈顶在映射内
    if ((size_t)(top - base) != DM_BIGSTACK_BYTES || ((uintptr_t)top & (DM_BIGSTACK_ALIGN - 1)) != 0) {
        t_state = -1;
        bs_log("bigstack init FAIL assert base=%p top=%p span=%u align_off=%u -> fallback",
               (void *)base, (void *)top, (unsigned)(size_t)(top - base), (unsigned)((uintptr_t)top & (DM_BIGSTACK_ALIGN - 1)));
        munmap(m, want);
        return;
    }
    t_base = base; t_top = top; t_state = 1;
    bs_log("bigstack init tid=%p base=%p top=%p size=%u align=%u guard=%u ok=1",
           (void *)(uintptr_t)pthread_self(), (void *)base, (void *)top,
           (unsigned)DM_BIGSTACK_BYTES, (unsigned)DM_BIGSTACK_ALIGN, (unsigned)DM_BIGSTACK_GUARD);
}

__attribute__((visibility("hidden"))) bs_prep dm_bigstack_prepare(uint64_t cur_sp) {
    bs_prep r = { 0, 0 };
    unsigned n = ++t_seq;   // 单线程内自增（本函数只在该线程自己的调用里跑）
    (void)atomic_fetch_add(&g_enter, 1);
    if (t_state == 0) bs_init_tls();
    int status;
    if (t_state != 1 || atomic_load(&g_force_fail)) {
        status = 0;
        atomic_fetch_add(&g_fallback, 1);
    } else if (cur_sp >= (uint64_t)(uintptr_t)t_base && cur_sp < (uint64_t)(uintptr_t)t_top) {
        status = 2;   // 已经在本线程大栈内：重入，不再切（否则会把新栈顶当旧栈顶）
        atomic_fetch_add(&g_reenter, 1);
    } else {
        status = 1;
        r.top = (uint64_t)(uintptr_t)t_top;
        atomic_fetch_add(&g_switch, 1);
    }
    // 断言/日志：前 8 次 + 每 65536 次 + 所有非 1 状态（失败与重入不能被静默吞掉）
    if (n <= 8 || status != 1 || (n % 65536u) == 0)
        bs_log("bigstack enter size=%u align=%u ok=%d", (unsigned)DM_BIGSTACK_BYTES,
               (unsigned)DM_BIGSTACK_ALIGN, status == 1);
    r.status = (uint64_t)status;
    return r;
}

// 读 sp（AArch64）
static inline uint64_t bs_rd_sp(void) { uint64_t v; __asm__ volatile("mov %0, sp" : "=r"(v)); return v; }

// ---------------------------------------------------------------------------
// 入口桩：dm_bigstack_enter（x16 = 实现地址，x0-x7 / q0-q7 参数，栈上无参数）
//
// 帧布局（288 字节，16 对齐）：
//    0 x29,x30 | 16 x0,x1 | 32 x2,x3 | 48 x4,x5 | 64 x6,x7 | 80 x8,x9 | 96 x16,x17
//  112 x18 | 120 x19 | 128 q0,q1 | 160 q2,q3 | 192 q4,q5 | 224 q6,q7 | 256 top | 264 status
// x19 在切栈路径里存"旧帧 sp"，用来回来；x17 是 IP1（非参数寄存器），不保存也不会伤语义。
// ---------------------------------------------------------------------------
__asm__(
    "    .text\n"
    "    .globl dm_bigstack_enter\n"
    "    .hidden dm_bigstack_enter\n"
    "    .p2align 4\n"
    "    .type dm_bigstack_enter, %function\n"
    "dm_bigstack_enter:\n"
    "    sub     sp, sp, #288\n"
    "    stp     x29, x30, [sp, #0]\n"
    "    mov     x29, sp\n"
    "    stp     x0, x1, [sp, #16]\n"
    "    stp     x2, x3, [sp, #32]\n"
    "    stp     x4, x5, [sp, #48]\n"
    "    stp     x6, x7, [sp, #64]\n"
    "    stp     x8, x9, [sp, #80]\n"
    "    stp     x16, x17, [sp, #96]\n"
    "    str     x18, [sp, #112]\n"
    "    str     x19, [sp, #120]\n"
    "    stp     q0, q1, [sp, #128]\n"
    "    stp     q2, q3, [sp, #160]\n"
    "    stp     q4, q5, [sp, #192]\n"
    "    stp     q6, q7, [sp, #224]\n"
    "    mov     x0, sp\n"                       // cur_sp：本帧上沿（> 帧内所有槽）
    "    bl      dm_bigstack_prepare\n"
    "    str     x0, [sp, #256]\n"                // top（0 = 不切）
    "    str     x1, [sp, #264]\n"                // status
    "    cbz     x0, .Lbs_no_switch\n"
    "    mov     x19, sp\n"                       // 旧帧（切栈后靠它回来）
    "    ldr     x29, [x19, #256]\n"              // x29 暂存新栈顶
    "    mov     sp, x29\n"                       // ← 切栈
    "    ldp     x0, x1, [x19, #16]\n"
    "    ldp     x2, x3, [x19, #32]\n"
    "    ldp     x4, x5, [x19, #48]\n"
    "    ldp     x6, x7, [x19, #64]\n"
    "    ldp     x8, x9, [x19, #80]\n"
    "    ldp     x16, x17, [x19, #96]\n"
    "    ldr     x18, [x19, #112]\n"
    "    ldr     x29, [x19, #0]\n"                // 恢复调用方帧指针（与普通调用一致的可回溯链）
    "    ldp     q0, q1, [x19, #128]\n"
    "    ldp     q2, q3, [x19, #160]\n"
    "    ldp     q4, q5, [x19, #192]\n"
    "    ldp     q6, q7, [x19, #224]\n"
    "    blr     x16\n"                           // 实现跑在 512 KB 大栈上
    "    mov     sp, x19\n"                       // 回到旧栈
    "    ldr     x19, [sp, #120]\n"
    "    ldp     x29, x30, [sp, #0]\n"
    "    add     sp, sp, #288\n"
    "    ret\n"
    "    .p2align 4\n"
    ".Lbs_no_switch:\n"                           // 回退：在调用方原栈上直通（= M43B 语义）
    "    ldp     x0, x1, [sp, #16]\n"
    "    ldp     x2, x3, [sp, #32]\n"
    "    ldp     x4, x5, [sp, #48]\n"
    "    ldp     x6, x7, [sp, #64]\n"
    "    ldp     x8, x9, [sp, #80]\n"
    "    ldp     x16, x17, [sp, #96]\n"
    "    ldr     x18, [sp, #112]\n"
    "    ldp     q0, q1, [sp, #128]\n"
    "    ldp     q2, q3, [sp, #160]\n"
    "    ldp     q4, q5, [sp, #192]\n"
    "    ldp     q6, q7, [sp, #224]\n"
    "    ldr     x29, [sp, #0]\n"
    "    blr     x16\n"                           // 栈不切，帧保留到返回（sp 仍 16 对齐）
    "    ldr     x19, [sp, #120]\n"
    "    ldp     x29, x30, [sp, #0]\n"
    "    add     sp, sp, #288\n"
    "    ret\n"
    "    .size dm_bigstack_enter, . - dm_bigstack_enter\n");

// 自检用入口：把 x16 设成 g_bs_test_target 再 `b dm_bigstack_enter`（参数寄存器原样透传）。
// C 侧按被调函数的签名声明，就能以**完全相同的**寄存器布局驱动入口桩。
__attribute__((visibility("hidden"))) uint64_t g_bs_test_target;
__asm__(
    "    .text\n"
    "    .globl dm_bigstack_test_call\n"
    "    .hidden dm_bigstack_test_call\n"
    "    .p2align 4\n"
    "    .type dm_bigstack_test_call, %function\n"
    "    .hidden g_bs_test_target\n"
    "dm_bigstack_test_call:\n"
    "    adrp    x16, g_bs_test_target\n"
    "    add     x16, x16, :lo12:g_bs_test_target\n"
    "    ldr     x16, [x16]\n"
    "    b       dm_bigstack_enter\n"
    "    .size dm_bigstack_test_call, . - dm_bigstack_test_call\n");

// ---------------------------------------------------------------------------
// 自检（只在 DM_M44A_BIGSTACK_TEST 打开时由探针 constructor 调用；不碰执行路径）
// ---------------------------------------------------------------------------
extern uint64_t dm_bigstack_test_call(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                                      double f0, double f1, double f2, double f3);

static uint64_t g_t_sp, g_t_deep_sp, g_t_ret, g_t_re_sp_outer, g_t_re_sp_inner;
static volatile int g_t_deep_ok;

static uint64_t bs_deep(int depth) {   // 每帧 96*8=768 B 的易失槽，踩实页
    volatile uint64_t pad[96];
    pad[0] = (uint64_t)depth; pad[95] = 0x5a5a5a5a5a5a5a5aull;
    if (depth > 0) { uint64_t r = bs_deep(depth - 1); return r + pad[0] + pad[95]; }
    g_t_deep_sp = bs_rd_sp();
    g_t_deep_ok = (pad[95] == 0x5a5a5a5a5a5a5a5aull);
    return pad[0];
}

#define BS_EXPECT(a0,a1,a2,a3,a4,a5,a6,a7,d0,d1,d2,d3) \
    ((uint64_t)(a0) + ((uint64_t)(a1) << 1) + ((uint64_t)(a2) << 2) + ((uint64_t)(a3) << 3) + \
     ((uint64_t)(a4) << 4) + ((uint64_t)(a5) << 5) + ((uint64_t)(a6) << 6) + ((uint64_t)(a7) << 7) + \
     (uint64_t)((d0) * 2) + (uint64_t)((d1) * 4) + (uint64_t)((d2) * 8) + (uint64_t)((d3) * 16))

static uint64_t bs_test_target(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                               double f0, double f1, double f2, double f3) {
    g_t_sp = bs_rd_sp();
    bs_deep(400);   // ≈ 400 × 800 B ≈ 320 KB：只有真在大栈上才不踩穿
    g_t_ret = BS_EXPECT(a0, a1, a2, a3, a4, a5, a6, a7, f0, f1, f2, f3);
    return g_t_ret;
}

static uint64_t bs_inner_target(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                                double f0, double f1, double f2, double f3) {
    g_t_re_sp_inner = bs_rd_sp();
    return BS_EXPECT(a0, a1, a2, a3, a4, a5, a6, a7, f0, f1, f2, f3);
}

static uint64_t bs_reenter_target(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                                  double f0, double f1, double f2, double f3) {
    g_t_re_sp_outer = bs_rd_sp();
    g_bs_test_target = (uint64_t)(uintptr_t)&bs_inner_target;   // 重入：入口桩应报 status=2、不再切栈
    uint64_t inner = dm_bigstack_test_call(1, 2, 3, 4, 5, 6, 7, 8, 1.0, 2.0, 3.0, 4.0);
    g_bs_test_target = (uint64_t)(uintptr_t)&bs_test_target;
    return inner + a0;
}

typedef struct { uint64_t base, top; int state; } bs_thread_info;
static bs_thread_info g_thr_info;
static void *bs_thread_main(void *arg) {
    (void)arg;
    g_bs_test_target = (uint64_t)(uintptr_t)&bs_test_target;
    uint64_t r = dm_bigstack_test_call(9, 9, 9, 9, 9, 9, 9, 9, 0.5, 0.5, 0.5, 0.5);
    g_thr_info.base = (uint64_t)(uintptr_t)t_base;
    g_thr_info.top  = (uint64_t)(uintptr_t)t_top;
    g_thr_info.state = t_state;
    return (void *)(uintptr_t)r;
}

static int bs_check(int idx, int ok, const char *what) {
    bs_log("bigstack selftest #%d %s=%d %s", idx, what, ok ? 1 : 0, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int dm_bigstack_selftest(void);   // 前置声明（异步启动器在它前面）

// 自检的**异步启动**：constructor 阶段 vk_boot.log 还没打开（实测 constructor 里 vk_boot_logf 直接丢弃），
// 所以起一个短线程等日志就绪（vk_boot_log_path() 非空）再跑，最多等 10 s。只由探针 constructor 在
// DM_M44A_BIGSTACK_TEST=1 时调用。线程自己 detach，不阻塞库加载。
static void *bs_selftest_thread(void *a) {
    (void)a;
    extern const char *vk_boot_log_path(void);
    for (int i = 0; i < 500; i++) {
        const char *lp = vk_boot_log_path();
        if (lp && *lp) break;
        usleep(20000);
    }
    dm_bigstack_selftest();
    return NULL;
}

void dm_bigstack_selftest_async(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, bs_selftest_thread, NULL) == 0) pthread_detach(t);
    else bs_log("bigstack selftest thread create failed errno=%d", errno);
}

int dm_bigstack_selftest(void) {
    int fails = 0;
    bs_log("bigstack selftest start (bytes=%u align=%u guard=%u)",
           (unsigned)DM_BIGSTACK_BYTES, (unsigned)DM_BIGSTACK_ALIGN, (unsigned)DM_BIGSTACK_GUARD);

    // #1 初始化与断言：prepare（当前 sp 必不在本线程大栈内）→ status 1，区间与对齐符合常量
    bs_prep p = dm_bigstack_prepare(bs_rd_sp());
    fails += bs_check(1, p.status == 1 && t_state == 1 && p.top == (uint64_t)(uintptr_t)t_top &&
                         ((size_t)(t_top - t_base) == DM_BIGSTACK_BYTES) &&
                         ((p.top & (DM_BIGSTACK_ALIGN - 1)) == 0), "init_switch_ok");

    // #2/#3 真正切栈 + 参数透传：目标函数读自己的 sp、深递归 320 KB、用全部 8 GP + 4 FP 参数算校验和
    g_bs_test_target = (uint64_t)(uintptr_t)&bs_test_target;
    uint64_t want = BS_EXPECT(11, 22, 33, 44, 55, 66, 77, 88, 1.5, 2.5, 3.5, 4.5);
    uint64_t got = dm_bigstack_test_call(11, 22, 33, 44, 55, 66, 77, 88, 1.5, 2.5, 3.5, 4.5);
    int in_region = g_t_sp >= (uint64_t)(uintptr_t)t_base && g_t_sp < (uint64_t)(uintptr_t)t_top;
    int deep_in_region = g_t_deep_sp >= (uint64_t)(uintptr_t)t_base && g_t_deep_sp < (uint64_t)(uintptr_t)t_top;
    unsigned used = (unsigned)((uintptr_t)t_top - g_t_deep_sp);
    fails += bs_check(2, in_region && deep_in_region && g_t_deep_ok && used > 250000 && used < DM_BIGSTACK_BYTES,
                      "on_bigstack_deep_ok");
    bs_log("bigstack selftest #2 detail target_sp=%#llx deep_sp=%#llx used=%u region=[%#llx,%#llx)",
           (unsigned long long)g_t_sp, (unsigned long long)g_t_deep_sp, used,
           (unsigned long long)(uintptr_t)t_base, (unsigned long long)(uintptr_t)t_top);
    fails += bs_check(3, got == want && g_t_ret == want, "args_passthrough_ok");

    // #4 重入：实现里再进一次 NVN → status 2、不切栈；内层 sp 仍在同一区间且更深（更小）
    g_bs_test_target = (uint64_t)(uintptr_t)&bs_reenter_target;
    uint64_t re = dm_bigstack_test_call(5, 5, 5, 5, 5, 5, 5, 5, 1.0, 1.0, 1.0, 1.0);
    int outer_in = g_t_re_sp_outer >= (uint64_t)(uintptr_t)t_base && g_t_re_sp_outer < (uint64_t)(uintptr_t)t_top;
    int inner_in = g_t_re_sp_inner >= (uint64_t)(uintptr_t)t_base && g_t_re_sp_inner < (uint64_t)(uintptr_t)t_top;
    fails += bs_check(4, atomic_load(&g_reenter) >= 1 && outer_in && inner_in &&
                         g_t_re_sp_inner <= g_t_re_sp_outer && re == BS_EXPECT(1, 2, 3, 4, 5, 6, 7, 8, 1.0, 2.0, 3.0, 4.0) + 5,
                      "reentrant_no_switch");

    // #5 回退路径：强制失能 → 目标跑在调用方原栈上（不在大栈区间），参数/返回值仍然正确
    atomic_store(&g_force_fail, 1);
    g_bs_test_target = (uint64_t)(uintptr_t)&bs_test_target;
    uint64_t fb = dm_bigstack_test_call(1, 2, 3, 4, 5, 6, 7, 8, 4.0, 3.0, 2.0, 1.0);
    int fb_in_region = g_t_sp >= (uint64_t)(uintptr_t)t_base && g_t_sp < (uint64_t)(uintptr_t)t_top;
    atomic_store(&g_force_fail, 0);
    fails += bs_check(5, !fb_in_region && atomic_load(&g_fallback) >= 1 &&
                         fb == BS_EXPECT(1, 2, 3, 4, 5, 6, 7, 8, 4.0, 3.0, 2.0, 1.0), "fallback_on_caller_stack_ok");

    // #6 每线程一份：另起线程跑同一入口，它的大栈必须与主线程不同
    memset(&g_thr_info, 0, sizeof g_thr_info);
    pthread_t th; void *thr_ret = NULL;
    int tc = pthread_create(&th, NULL, bs_thread_main, NULL);
    if (tc == 0) { pthread_join(th, &thr_ret); }
    int distinct = (tc == 0) && g_thr_info.state == 1 && g_thr_info.base != 0 &&
                   g_thr_info.base != (uint64_t)(uintptr_t)t_base && g_thr_info.top != (uint64_t)(uintptr_t)t_top;
    fails += bs_check(6, tc == 0 ? distinct : 1, tc == 0 ? "thread_tls_distinct" : "thread_tls_skipped(create_failed)");
    if (tc == 0)
        bs_log("bigstack selftest #6 detail main=[%#llx,%#llx) thr=[%#llx,%#llx) thr_ret_ok=%d",
               (unsigned long long)(uintptr_t)t_base, (unsigned long long)(uintptr_t)t_top,
               (unsigned long long)g_thr_info.base, (unsigned long long)g_thr_info.top,
               (uintptr_t)thr_ret == BS_EXPECT(9, 9, 9, 9, 9, 9, 9, 9, 0.5, 0.5, 0.5, 0.5));

    bs_log("bigstack selftest verdict=%s fails=%d enter=%u switch=%u fallback=%u reenter=%u",
           fails ? "FAIL" : "PASS", fails,
           (unsigned)atomic_load(&g_enter), (unsigned)atomic_load(&g_switch),
           (unsigned)atomic_load(&g_fallback), (unsigned)atomic_load(&g_reenter));
    return fails ? -1 : 0;
}

// ---------------------------------------------------------------------------
// nvn_entry_elf.S 的 nvn_prof_entry（DM_NVN_PROF 计时包装）需要一个**符号版** mach_absolute_time
// （汇编里是 `bl mach_absolute_time`，不能是头文件里的 static inline）。按名全树 grep：platform/*.c 只**调用**它、
// 没有定义；nx2 的定义在 rec/compat/darwin_symbols.c，而那个文件按 M43A 的重复符号处置**不进编译单元**。
// 所以这里给出全库唯一实现：CLOCK_MONOTONIC 纳秒（与 darwin_symbols.c 逐字相同的语义，只是换了时钟源精度单位）。
// 只在 DM_NVN_PROF 打开时才被走到；本单不打开 profiling，跳板也不接执行。
uint64_t mach_absolute_time(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
