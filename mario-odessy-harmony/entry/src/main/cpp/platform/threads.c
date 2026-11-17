/* Adapted from pinned D2R Mobile host/hle_thread.c, MIT.
 * Game-specific diagnostics and optional timing accounting removed.
 * Core affinity is virtual bookkeeping; Darwin schedules pthreads. */
// Horizon 内核的线程与同步 SVC，原生实现：每个客体线程 = 一个宿主 pthread，跑在 SDK 给的客体栈上。
// SDK 的 nn::os 原样运行，只把它下面的「内核」换成宿主：
//   线程   0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D 优先级 0x0E/0x0F 核掩码
//          0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   锁     0x1A ArbitrateLock 0x1B ArbitrateUnlock（标签字 = 持有者句柄 | 0x40000000 有等待者）
//   条件   0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   地址   0x34 WaitForAddress 0x35 SignalToAddress
//   事件   0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// 语义按公开 Horizon 内核行为（Atmosphère mesosphère 的 KConditionVariable / KAddressArbiter / KSynchronization）。
// 所有等待挂在一把全局锁 G 上，每线程一个条件变量；先求正确，性能后量。
// Thread and synchronization SVCs of the Horizon kernel, implemented natively: each guest thread = one host pthread, running on the guest stack given by the SDK.
// The SDK's nn::os runs as is; only the "kernel" underneath it is replaced by the host:
//   Threads    0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D priority 0x0E/0x0F core mask
//              0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   Locks      0x1A ArbitrateLock 0x1B ArbitrateUnlock (tag word = owner handle | 0x40000000 has waiters)
//   Condition  0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   Address    0x34 WaitForAddress 0x35 SignalToAddress
//   Events     0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// Semantics follow the public Horizon kernel behavior (KConditionVariable / KAddressArbiter / KSynchronization of Atmosphère mesosphère).
// All waits hang on one global lock G, with one condition variable per thread; correctness first, performance to be measured later.
#include <sys/stat.h>   /* M8C: stat() 判开关文件存在 */
#include "thread_compat.h"
#include "ipc.h"
#include "runtime.h"   /* M5l: runtime_safe_read / runtime_maps_* / runtime_addr_mapped（[fx] 取证） */
/* round20：SMO_OBS_HEAVY 最小扰动开关（early1d 等重探针编译开关）。分线后不再 include
   games/<tid>/game_patches.h（原宿主 host/game_patches.h）——平台侧 #ifndef 下沉默认值，
   行为与原值一致（0）；游戏侧头若重定义同值宏，此处亦兼容（相同替换表不报错）。 */
#ifndef SMO_OBS_HEAVY
#define SMO_OBS_HEAVY 0
#endif
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <string.h>
#include <time.h>
#if defined(__MUSL__) || defined(__OHOS__) || defined(__linux__)
#include <sys/syscall.h>   // __NR_gettid（thread_main 记 htid）/ __NR_process_vm_readv（采样帧读）
#include <sys/uio.h>       /* M6s: process_vm_readv in the stack-probe handler */
#include <unistd.h>        /* M6s: getpid() in the stack-probe handler */
#endif
#include <mach/mach.h>
#include <pthread/qos.h>
#include <dlfcn.h>

#define R_TIMEOUT     0xEA01u   // 内核模块 1，描述 117 | Kernel module 1, description 117
#define R_CANCELLED   0xEC01u   // 118
#define R_BAD_HANDLE  0xE401u   // 114
#define R_BAD_STATE   0xFA01u   // 125
#define R_OUT_OF_RES  0xCE01u   // 103
#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_SESSION, O_TMEM };   // O_SESSION / O_TMEM: IPC objects owned by ipc.c
/* M6h: the pair a readable event actually comes from. Until now an event_t was just {signaled, refs},
   so "who owns the write end of 0x1024" was unanswerable from the record. hw/hr are the two handles the
   kernel call returned (game side); a service-side event (runtime_event_new) has hw only, and a readable
   copy handed to the client (runtime_event_readable) fills hr. cre_* is the creator's identity. */
typedef struct event { int signaled; int refs;
                       uint32_t hw, hr;                 /* write-end / read-end handle as created (0 = none) */
                       uint32_t cre_thr;                /* guest thread handle that created it (0 = host/HLE) */
                       uint64_t cre_lr, born_ns;        /* creator LR + birth time */
                       int src;                         /* 0 = svc 0x45, 1 = HLE service write end, 2 = HLE readable copy */
} event_t;
typedef struct { uint32_t num; unsigned k; uint64_t a0, a1, a2, a3, lr; } ps_svc8_t;   /* M6m: one SVC as seen at entry */
/* M6o: the same record plus where the SVC actually was and how it was reached. `lr` above is useless as a
   call site (it is the patch-area gate stub, sdk+0xd3f3f4 for every 0x34 on this build); `pc` is the real
   module offset of the svc instruction (gates.tsv column 4) and bt[] is the frame-pointer chain read at
   entry, so "who called this syscall" no longer needs the SIGUSR2 sampler (which cannot hit main: signal
   masked, 5 of 13898 deliveries landed and none on main in M6n). */
typedef struct { uint32_t num; unsigned k; uint64_t a0, a1, a2, a3, lr; uint64_t pc; const char *mod; uint64_t bt[3]; } ps_svc8x_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; uint64_t host_context[2]; int prio, core; uint64_t mask;
    uint64_t born_ns, exit_ns; uint32_t born_tick, exit_tick;   // 建立 / 退出的时刻与当时的游戏拍（现场导出用：作业工人是不是在卡住前退的） | Time of creation / exit and the game tick at that time (for the state dump: did the job worker exit before the stall)
    int started, exited, cancel;
    pthread_t pt; pthread_cond_t cv;
    // 当前等待：kind 0=无 1=锁 2=条件 3=地址 4=同步对象
    // Current wait: kind 0=none 1=lock 2=condition 3=address 4=synchronization object
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    uint64_t wseq;             // 登记等待的先后序号：同优先级先到先醒（与 Horizon 内核 KThread 等待树同序） | Sequence number of wait registration: at equal priority first come, first woken (same order as the Horizon kernel's KThread wait tree)
    struct thr *next;          // 等待链 | Wait chain
    struct thr *all;           // 全部线程链（现场导出用） | All-threads chain (for the state dump)
    int last_svc; uint64_t last_lr, last_fp;
    uint32_t *svch;            // M6h: per-thread 256-slot SVC histogram (null until the first SVC); the [own] line's svc_top. M7y: u16 -> u32 (the u16 form saturated at 65535 on the 700k-SVC/min pollers, misreading later counts as "rare")
    int closed;                // 句柄已关：与 exited 同时成立才释放对象（Horizon：线程对象随最后一个引用消失） | Handle closed: the object is freed only when this holds together with exited (Horizon: the thread object goes away with the last reference)
    uint64_t nsvc;             // SVC 次数（现场导出用） | SVC count (for the state dump)
    mach_port_t mport;         // 宿主线程端口（取 CPU 时间） | Host thread port (to get CPU time)
    uint32_t qos;
    _Atomic int qos_pending;   // 他线程 SetThreadPriority 改了 prio：目标线程下次 SVC 入口自套用（OH_QoS 只能设自身线程；donor hle_thread.c:77）
    _Atomic int cur_core;              // virtual core held while running guest code, -1 while blocked / not started
    _Atomic int preempt;               // a higher-priority thread waits for this thread's core
    _Atomic int kicked;
    uint64_t preempt_ns, cwait_ns;
    uint64_t cseq, core_since; struct thr *cnext;
    /* M6s: time this thread spent inside core_take's handoff wait, and how many times. Written only by
       the thread itself on the slow path (it was already parked there); a wait session reads the delta,
       so the fast path gets no extra timestamp. */
    uint64_t cw_n, cw_ns;
    uint64_t rel_t0;           /* M6t A1: now_ns() when this thread gave its core up around a blocking host wait */
    uint32_t cre_h; uint64_t cre_lr;   /* M6t C7: who created this thread (handle + guest LR), for the exit evidence */
    uint32_t wh[4]; int nwh;   // handles of the current WaitSynchronization (state dump)
    int last_hle;              // 1 + wrapper index of the last host call made while "hletrace" was on (state dump)
    int spin_pin;              // round3 自旋采样器选中且尚未放回：thr_free 跳过回收（防放锁后 use-after-free，泄露=取证可接受） | Pinned by the spin sampler between G sections: thr_free must not free it yet
    int htid;                  // 宿主 tid（/proc/self/task 对应用；round3 采样失败诊断） | Host tid for /proc/self/task lookups
    uint64_t yrun_ns, yrun_svc;   // the current run of yields: when it started, and nsvc at its last yield (yield_backoff_ns)
    int op; uint64_t op_ns;    // M5b：本线程最后进入的那个宿主 NVN 重活（1 submit / 2 pipeline / 3 tex upload），只记入口；卡住时「是哪个 + 多久没换」就是卡在哪个宿主调用里的证据 | M5b: last heavy host NVN call this thread entered (1 submit, 2 pipeline, 3 tex upload); entry only, so while a thread is stuck inside one this stays put and its age says how long
    int mvs;                   // M6l: index into the [lp] mover table (0..4), -1 = not one of the observed threads. Set once when the handle is assigned, so the per-SVC cost is one compare | M6l: slot index for the [lp] mover table, -1 = not observed
    /* M6m: the newest eight SVCs this thread issued (num, x0..x3, guest LR, dump#). A rolling ring, so a
       thread that stops keeps its last eight forever -- that is what "what was the poster doing just
       before it stopped" needs. Written only by the thread itself, read by the dump without a lock. */
    ps_svc8_t svc8[8];
    unsigned svc8i;
    /* M6o: the same ring slots with the SVC pc and the frame-pointer backtrace. Filled for main (0x1001)
       only: main issues ~26 SVC/s so the three stack reads cost nothing, while a 700k-SVC/min poller
       would turn a forensic probe into a slowdown. Same index as svc8. */
    ps_svc8x_t svc8x[8];
    /* M6n: total SVCs ever written into svc8 (monotonic). svc8i is a wrapped ring index, so a dump that
       printed min(svc8i,8) entries could print fewer than eight and misread it as "the ring is not full".
       Readers use min(svc8t,8) instead; svc8i stays the write position. */
    unsigned svc8t;
    // M5c：阶段环。op 只有「最后一个」一个值，分不清「卡在这一段里」与「早就出了这一段、卡在下一个没打点的地方」。
    // 环记最近 16 个 {op, arg, 时刻}；冻结 dump 打出末 6 个 ⇒ 停在哪一步 + 停之前走过哪几步一目了然。
    // 只本线程写、无锁；dump 侧容忍撕裂（诊断用）。seq 单调增：两次 dump 之间 seq 不变 = 卡在一个调用里，
    // seq 在涨 = 在「很多次小调用」里循环，两者是完全不同的病。
    struct { int op; uint64_t arg, ns; } phev[16];
    unsigned phe_i; uint64_t phe_seq;
    /* M6i：I/O 与 IPC 归属账。调用者自己是唯一写者（每次 open/read/IPC 请求约四个原子加），
       心跳线程 dump 时无锁读；撕裂只会印错一个诊断数值，不参与任何判定。
       M6i: per-thread I/O + IPC ledger. The calling guest thread is the only writer (about four
       atomics per open/read/IPC request); the heartbeat thread reads it without a lock, so a torn
       value can only misprint a diagnostic number and never decides anything. */
    _Atomic uint64_t io_open, io_read, io_bytes, io_ipc;
    _Atomic uint64_t io_last_io_ns;     /* 该线程最后一次 open/read 的时刻 | last open/read by this thread */
    _Atomic uint64_t io_last_lr;        /* 那次 I/O 所在 SVC 的客体返回地址 | guest LR of the SVC in flight then */
    _Atomic uint32_t io_last_svc;       /* 那次 I/O 所在的 SVC 号 | SVC number then */
    char io_path[96];                   /* 该线程最近一次打开的路径（无锁拷贝，撕裂可接受） | newest path opened here */
    /* M7i: the guest (sp, fp, lr) at the svc that parked this thread. entry.S already hands the svc
       handler regs[29]/[30]/[31] (guest x29/x30 and the guest sp before its frame, M7u's +248 pad
       slot); storing them at the park lets a dump walk the guest frame chain instead of stopping at
       the svc trampoline -- M6k proved the 0x1C/0x1D LR is always sdk+0xd3f14c, the trampoline, so
       "who called this wait" was unanswerable until now. */
    uint64_t gsp, gfp, glr;
} thr_t;
static thr_t *g_all;
/* M6i: retired-thread ledger. A guest thread that ends is freed from g_all (thr_free), and its I/O
   counters would vanish with it - exactly the wrong blind spot when the question is "did the loader
   thread die". thr_free copies the counts here first, together with the guest entry point and the
   last LR, so a short-lived loader is still named after it is gone. */
#define IO_RETIRED 8
typedef struct { uint32_t h; uint64_t open, rd, bytes, ipc, entry, llr, exit_ns; } io_ret_t;
static io_ret_t g_io_ret[IO_RETIRED];
static _Atomic unsigned g_io_ret_n;      /* monotonically counts retired threads with I/O */
static _Atomic uint64_t g_io_ret_open, g_io_ret_read, g_io_ret_bytes, g_io_ret_ipc;

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
/* M6l: which of the observed threads a handle belongs to (definition of the table is below, next
   to the rest of the M6l block). Declared here because the two thr_t constructors set the field. */
static int lp_mv_slot(uint32_t h);
#define MAX_H 4096
static struct { int type; void *obj; uint64_t creator; } g_h[MAX_H];   // creator：登记时的调用者 LR——客体 SVC（线程/事件）=客体 LR，IPC 服务对象=宿主 ipc.c 返回址；dump 全景指认 0x1003/0x101f 这类句柄的身世 | creator: the caller's LR at registration
static pthread_key_t g_keythr;
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;          // 全部在等的线程（锁 / 条件 / 地址 / 同步） | All waiting threads (lock / condition / address / synchronization)

static uint32_t h_new(int type, void *obj, uint64_t creator) {     // 调用方持 G | Caller holds G
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) { g_h[i].type = type; g_h[i].obj = obj; g_h[i].creator = creator; return 0x1000 + i; }
    die("句柄表满");
}
static void *h_peek(uint32_t h, int type) {      // 只查不报 | Lookup only, no report
    if (h == H_CUR_THREAD && type == O_THREAD) return (g_keythr ? pthread_getspecific(g_keythr) : NULL);
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {       // 查不到即报（句柄错是真错误） | Reports if not found (a wrong handle is a real error)
    void *o = h_peek(h, type);
    if (!o) LOG("句柄无效 %#x（要类型 %d）", h, type);
    return o;
}
static thr_t *cur(void) {
    thr_t *t = (g_keythr ? pthread_getspecific(g_keythr) : NULL);
    if (!t) die("非客体线程调用了线程 SVC");
    return t;
}
/* M6r：给「最长等待」那几行点名句柄的身世（类型 + 创建者 LR）。只读；dump 线程调用，
   trylock 拿不到就报 unknown，绝不阻塞心跳。 */
void runtime_describe_address(uint64_t a, char *out, size_t n);
void runtime_handle_desc(uint32_t h, char *out, unsigned cap) {
    static const char *const kinds[] = { "free", "thread", "event_r", "event_w", "session", "tmem" };
    char d[80]; d[0] = 0;
    out[0] = 0;
    if (h == H_CUR_THREAD) { snprintf(out, cap, "cur_thread"); return; }
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H) { snprintf(out, cap, "h=%#x out-of-range", h); return; }
    int locked = 0;
    for (int k = 0; k < 50 && !(locked = pthread_mutex_trylock(&G) == 0); k++) usleep(1000);
    if (!locked) { snprintf(out, cap, "h=%#x (G busy)", h); return; }
    int ty = g_h[i].type; uint64_t cr = g_h[i].creator;
    pthread_mutex_unlock(&G);
    if (cr) runtime_describe_address(cr, d, sizeof d);
    snprintf(out, cap, "%s creator=%s", (ty > 0 && ty <= 5) ? kinds[ty] : "?", d[0] ? d : "-");
}

// ---------- 等待原语（持 G 调用）----------
// ---------- Wait primitives (called holding G) ----------
static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
// 按内核顺序挑等待者：优先级数值小者优先，同优先级先登记者优先（持 G）。kind 1 按锁地址、2 按条件键、3 按地址匹配
// Pick a waiter in kernel order: numerically lower priority first, at equal priority the earlier registered first (holding G). kind 1 matches by lock address, 2 by condition key, 3 by address
static uint64_t g_wseq;
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static void ldr_note_wake(thr_t *t);   /* M6g: per-wake window, defined next to svc_thread */
static void wake(thr_t *t, int64_t res) { ldr_note_wake(t); wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; pthread_cond_signal(&t->cv); }
// 挂起直到被唤醒或超时（ns<0 无限、0 立即超时）；返回 wres 或 R_TIMEOUT
// Suspend until woken or timed out (ns<0 infinite, 0 immediate timeout); returns wres or R_TIMEOUT
static int64_t block_inner(thr_t *t, int kind, int64_t ns);
// ---------- Virtual cores ----------
// Horizon runs at most one thread per core, and TOTK keys per-core data (allocators, job queues) by
// GetCurrentCoreNumber without locks. Guest threads therefore hold one of the three application cores while they
// run guest code; a core is given up only where Horizon would switch threads (waits, sleep, yield, exit, blocking
// host services) and handed to the best waiting thread (lowest priority value, then first come). A thread that a
// higher-priority waiter wants to preempt gives its core up at its next SVC. TOTK_FREE_CORES=1 disables this.
#define NCORE 3
static thr_t *g_core_owner[NCORE];
static thr_t *g_core_wait;
static int g_cores_on = -1;
static int cores_enabled(void) { if (g_cores_on < 0) g_cores_on = getenv("TOTK_FREE_CORES") ? 0 : 1; return g_cores_on; }
static int core_allowed(const thr_t *t, int c) { return (int)((t->mask >> c) & 1); }
/* M6t A2: soft affinity. `mask` was a hard binding: a thread whose mask held only core1 waited for core1
   even while core0 and core2 sat idle (M6s: `[core] busy=[1.8% 99.1% 1.1%]` while h=0x1023 accumulated
   29.3 s of handoff wait per 30 s window). Horizon's mask is not a hard binding either -- it says which
   cores the thread *may* run on, and the scheduler migrates onto an idle core. Here every application core
   runs the same code and the guest only reads GetCurrentProcessorNumber, which this shim answers from the
   core actually held, so falling back to a free core is safe. mask keeps its meaning as a *preference*:
   the preferred core and then the mask's free cores are tried first, the rest of the cores only after.
   TOTK_CORE_SOFT=0 restores the old hard binding for A/B comparison. */
static int g_core_soft = -1;
static int core_soft(void) { if (g_core_soft < 0) { const char *e = getenv("TOTK_CORE_SOFT"); g_core_soft = e ? atoi(e) : 1; } return g_core_soft; }
/* M6t A1/A2 counters: how many times a core was given up around a blocking host wait and for how long,
   and how many times a thread had to run off-mask (soft fallback). Read by the [core] line, no lock. */
static _Atomic unsigned long long g_core_rel_n, g_core_rel_ns, g_core_soft_n;
/* Adapted from the MIT LM3 port (#218, 714e4f3): bounded starvation and guest-only signal preemption.
   Anti-starvation (Odyssey): a thread that has waited CORE_STARVE_NS for a core gets the next free one before
   higher-priority waiters, and core_watch preempts an owner for it. Horizon has no such rule, but the guest's busier-than-
   hardware threads kept a priority-45 job worker off its only core while the main thread waited for its job. */
#define CORE_STARVE_NS 30000000
static void core_grant(int c) {   // holding G: give free core c to the best waiter
    thr_t *b = NULL, **bp = NULL;
    /* M6m: nobody is waiting for a core -> nothing to grant, and the timestamp is not needed. This is
       the common case on the blocking path (block -> core_put -> here), where the guest stack may be
       within a few dozen bytes of its guard page: runtime_now_ns alone needs 48 bytes of it. */
    if (!g_core_wait) return;
    uint64_t now = now_ns();
    /* M6t A2: two passes. Pass A honours the mask (the old behaviour, and the preference order); pass B
       only runs when soft affinity is on and pass A found nobody, so an off-mask waiter can use this
       core instead of leaving it idle. Both keep the anti-starvation rule first. */
    for (int pass = 0; pass < 2 && !b; pass++) {
        int any = pass == 0 || core_soft();
        if (!any) break;
        for (thr_t **p = &g_core_wait; *p; p = &(*p)->cnext)
            if ((pass == 1 || core_allowed(*p, c)) && now - (*p)->cwait_ns > CORE_STARVE_NS && (!b || (*p)->cwait_ns < b->cwait_ns)) { b = *p; bp = p; }
        if (!b) for (thr_t **p = &g_core_wait; *p; p = &(*p)->cnext)
            if ((pass == 1 || core_allowed(*p, c)) && (!b || (*p)->prio < b->prio || ((*p)->prio == b->prio && (*p)->cseq < b->cseq))) { b = *p; bp = p; }
    }
    if (!b) return;
    if (b->cur_core < 0 && !core_allowed(b, c)) atomic_fetch_add_explicit(&g_core_soft_n, 1ull, memory_order_relaxed);
    *bp = b->cnext; b->cnext = NULL; b->cur_core = c; b->core_since = now_ns(); g_core_owner[c] = b; pthread_cond_signal(&b->cv);
}
static void core_put(thr_t *t) {   // holding G
    if (t->cur_core < 0) return;
    int c = t->cur_core; t->cur_core = -1; t->preempt = 0; t->kicked = 0; g_core_owner[c] = NULL; core_grant(c);
}
static void core_watch_start(void);
static void cw_add(uint32_t h, uint64_t ns);   /* M6s: global core-wait table, defined with the [core] dump */
static void core_take(thr_t *t) {   // holding G; waits until a core in t's mask is handed over
    if (!cores_enabled() || t->cur_core >= 0) return;
    if (!(t->mask & ((1u << NCORE) - 1))) die("thread %#x core mask %#llx has no application core", t->handle, (unsigned long long)t->mask);
    int c = t->core >= 0 && t->core < NCORE && core_allowed(t, t->core) && !g_core_owner[t->core] ? t->core : -1;
    for (int i = 0; c < 0 && i < NCORE; i++) if (core_allowed(t, i) && !g_core_owner[i]) c = i;
    /* M6t A2: nothing free inside the mask -- soft affinity lets it sit on any idle application core
       rather than wait for the one core its mask names. */
    if (c < 0 && core_soft()) {
        for (int i = 0; c < 0 && i < NCORE; i++) if (!g_core_owner[i]) c = i;
        if (c >= 0) atomic_fetch_add_explicit(&g_core_soft_n, 1ull, memory_order_relaxed);
    }
    if (c >= 0) { t->cur_core = c; t->core_since = now_ns(); g_core_owner[c] = t; return; }
    core_watch_start();
    t->cseq = ++g_wseq; t->cwait_ns = now_ns(); t->cnext = g_core_wait; g_core_wait = t;
    /* Higher priority preempts; at equal priority a thread running away from its ideal core gives that core back to a
       waiter whose ideal core it is (Horizon migrates a thread only onto an idle core). Both at the owner's next SVC. */
    for (int pass = 0; pass < 2; pass++) {
        int set = 0;
        if (pass == 1 && !core_soft()) break;
        for (int i = 0; i < NCORE; i++) { thr_t *o = g_core_owner[i];
            if ((pass == 1 || core_allowed(t, i)) && o && !o->preempt && (o->prio > t->prio || (o->prio == t->prio && o->core != i && t->core == i))) { o->preempt = 1; o->preempt_ns = now_ns(); set = 1; } }
        if (set) break;
    }
    while (t->cur_core < 0) pthread_cond_wait(&t->cv, &G);
    /* M6s: the handoff wait is the "core starvation" half of every instrumented wait. One extra
       timestamp here, on a path that already took two (cwait_ns above, core_since in core_grant). */
    {   uint64_t d = now_ns() - t->cwait_ns;
        t->cw_n++; t->cw_ns += d;
        if (t->handle) cw_add(t->handle, d); }
}
/* ---------- M6s: stack probe for a thread that will not stop running (diagnostic) ----------
   M6s boot 1 named the 60 MB/s category (jemalloc) and the victim (0x1004 in vkWaitForFences) but not
   the *code* that allocates: the grower is a host thread (no guest handle) sitting at 100% CPU with no
   syscall, so no guest ledger can name it. This samples that thread's own stack: the heartbeat thread
   sends SIGURG to one tid, the handler records pc/lr/sp/fp and walks the frame chain into a static
   buffer, and the next [cpu] line prints it symbolised. SIGURG's default action is "ignore" and this
   process never used it, so a delivery without a handler is harmless.
   Every stack read goes through process_vm_readv (EFAULT instead of SIGSEGV), so a bogus frame pointer
   can only shorten the chain. The handler allocates nothing, takes no lock and writes only statics. */
#define STK_SIG SIGURG
#define STK_FR 12
#define STK_SCAN 256
typedef struct { int tid; uint64_t pc, lr, sp, fp; uint64_t fr[STK_FR]; int n;
                 uint64_t scan[STK_SCAN]; int nscan; } stk_cap_t;
static stk_cap_t g_stk;
static _Atomic unsigned g_stk_gen;
static _Atomic int g_stk_ready;
static long stk_pvread(uint64_t a, void *dst, unsigned n) {
    struct iovec l = { dst, n }, r = { (void *)(uintptr_t)a, n };
    long k = syscall(__NR_process_vm_readv, getpid(), &l, 1ul, &r, 1ul, 0ul);
    return k;
}
static void stk_handler(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si; int saved = errno;
    uint64_t pc = 0, lr = 0, sp = 0, fp = 0;
#ifdef __MUSL__
    ucontext_t *u = (ucontext_t *)ctx;
    pc = u->uc_mcontext.pc; lr = u->uc_mcontext.regs[30];
    sp = u->uc_mcontext.sp; fp = u->uc_mcontext.regs[29];
#else
    ucontext_t *u = (ucontext_t *)ctx;
    pc = arm_thread_state64_get_pc(u->uc_mcontext->__ss);
    lr = arm_thread_state64_get_lr(u->uc_mcontext->__ss);
    sp = arm_thread_state64_get_sp(u->uc_mcontext->__ss);
    fp = arm_thread_state64_get_fp(u->uc_mcontext->__ss);
#endif
    g_stk.tid = (int)syscall(SYS_gettid);
    g_stk.pc = pc; g_stk.lr = lr; g_stk.sp = sp; g_stk.fp = fp; g_stk.n = 0;
    uint64_t cur = fp;
    for (int i = 0; i < STK_FR && cur; i++) {
        if ((cur & 7u) || cur < sp || cur > sp + (4u << 20)) break;   /* outside this thread's own frame area */
        uint64_t fr[2];
        if (stk_pvread(cur, fr, 16) != 16) break;
        g_stk.fr[g_stk.n++] = fr[1];
        if (fr[0] <= cur) break;
        cur = fr[0];
    }
    /* Poor man's unwinder for code built without frame pointers: copy the first 2 KB of the stack as
       raw words and let the *dump* (a normal thread context) decide which of them are code addresses.
       pvread makes this safe even if [sp, sp+2KB) crosses the top of the stack. */
    g_stk.nscan = 0;
    if (sp) {
        int words = (int)(stk_pvread(sp, g_stk.scan, sizeof g_stk.scan) / 8);
        if (words > 0) {
            int k = 0;
            for (int i = 0; i < words; i++)
                if (g_stk.scan[i] > 0x1000ull) g_stk.scan[k++] = g_stk.scan[i];   /* drop obvious non-pointers */
            g_stk.nscan = k;
        }
    }
    atomic_store_explicit(&g_stk_gen, atomic_load_explicit(&g_stk_gen, memory_order_relaxed) + 1u,
                          memory_order_relaxed);
    errno = saved;
}
void runtime_stack_probe_init(void) {
    if (atomic_exchange_explicit(&g_stk_ready, 1, memory_order_relaxed)) return;
    struct sigaction a = {0};
    a.sa_sigaction = stk_handler; a.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&a.sa_mask);
    if (sigaction(STK_SIG, &a, NULL)) { atomic_store_explicit(&g_stk_ready, 0, memory_order_relaxed); return; }
}
int runtime_stack_probe_req(int tid) {
    if (tid <= 0) return -1;
    runtime_stack_probe_init();
    if (!atomic_load_explicit(&g_stk_ready, memory_order_relaxed)) return -1;
    long r = syscall(SYS_tgkill, getpid(), tid, STK_SIG);
    return (int)r;
}
unsigned runtime_stack_probe_gen(void) { return atomic_load_explicit(&g_stk_gen, memory_order_relaxed); }
/* Called from the heartbeat thread: prints the newest capture (never from a signal context). */
static void stk_sym(char *out, size_t cap, uint64_t pc) {
    if (!pc) { snprintf(out, cap, "-"); return; }
    Dl_info di;
    if (dladdr((void *)(uintptr_t)pc, &di) && di.dli_fname) {
        const char *b = strrchr(di.dli_fname, '/'); b = b ? b + 1 : di.dli_fname;
        snprintf(out, cap, "%s+%#llx", b, (unsigned long long)(pc - (uint64_t)(uintptr_t)di.dli_fbase));
    } else snprintf(out, cap, "host%#llx", (unsigned long long)pc);
}
void runtime_stack_probe_dump(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    char pc[64], lr[64];
    stk_sym(pc, sizeof pc, g_stk.pc);
    stk_sym(lr, sizeof lr, g_stk.lr);
    vk_boot_logf("[stk] t=%us tid=%d pc=%s lr=%s sp=%#llx fp=%#llx frames=%d",
                 tsec, g_stk.tid, pc, lr, (unsigned long long)g_stk.sp,
                 (unsigned long long)g_stk.fp, g_stk.n);
    for (int i = 0; i < g_stk.n; i++) {
        char b[64];
        stk_sym(b, sizeof b, g_stk.fr[i]);
        vk_boot_logf("[stk]   #%d %s", i, b);
    }
}
static thr_t *me_or_null(void) { return g_keythr ? pthread_getspecific(g_keythr) : NULL; }
/* Odyssey's audio thread can spin on IsMemoryPoolAttached without an SVC while the renderer update that completes
   the detach waits for its virtual core. Horizon's scheduler preempts at once; here a watcher interrupts a
   core owner that a higher-priority waiter has been waiting on for 2 ms. The signal handler gives the core up only when
   the thread was interrupted in guest code (inside a loaded module), where it holds no host lock; elsewhere it gives it
   up at its next SVC as before. Guest threads run their handlers on their own signal stack (loader.c). */
#define CORE_PREEMPT_NS 2000000
#define CORE_PREEMPT_SIGNAL SIGUSR2
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off);
extern void dm_nvn_fn_dump(int dno) __attribute__((weak));   /* M5o: nvn sync/event ledger (gfx_bridge.c) */
static void core_preempt_handler(int sig, siginfo_t *info, void *ctx) {
    (void)sig; (void)info; int saved = errno;
    thr_t *t = me_or_null(); const char *name; uint64_t off;
#ifdef __MUSL__   /* OHOS: musl aarch64 mcontext 值类型，pc = regs[32] */
    uint64_t pc = ((ucontext_t *)ctx)->uc_mcontext.pc;
#else             /* macOS: Apple __ss 指针风格 */
    uint64_t pc = arm_thread_state64_get_pc(((ucontext_t *)ctx)->uc_mcontext->__ss);
#endif
    if (t && t->cur_core >= 0 && runtime_module_offset(pc, &name, &off)) {
        pthread_mutex_lock(&G); if (t->preempt && t->cur_core >= 0) { t->preempt = 0; core_put(t); core_take(t); } else t->kicked = 0; pthread_mutex_unlock(&G);
    } else if (t) t->kicked = 0;   /* host code: the next SVC checkpoint gives the core up */
    errno = saved;
}
/* ---------- M6s: virtual-core occupancy profile (pure sampling, read-only) ----------
   Who holds the three virtual cores, and how often. One sample every 10 watcher iterations (~10 ms)
   taken while G is already held, so no lock, no timestamp and nothing on any guest path. The tables
   are atomics only because the beat thread reads them without G; a torn read can misprint a
   percentage and never decides anything. Handles, not thr_t*, are stored: a slot keyed by a live
   pointer would be a use-after-free after thr_free. */
#define CSMP_SLOTS 12
static _Atomic unsigned g_csmp_tot;                 /* samples taken (all cores) */
static _Atomic unsigned g_csmp_idle[NCORE];         /* samples with no owner */
static _Atomic unsigned g_csmp_cnt[NCORE][CSMP_SLOTS];
static _Atomic unsigned g_csmp_h[NCORE][CSMP_SLOTS];   /* 0 = empty slot (handles start at 0x1001) */
static void csmp_take(void) {   /* holding G */
    atomic_fetch_add_explicit(&g_csmp_tot, 1, memory_order_relaxed);
    for (int c = 0; c < NCORE; c++) {
        thr_t *o = g_core_owner[c];
        if (!o) { atomic_fetch_add_explicit(&g_csmp_idle[c], 1, memory_order_relaxed); continue; }
        int slot = -1;
        for (int i = 0; i < CSMP_SLOTS; i++) {
            unsigned h = atomic_load_explicit(&g_csmp_h[c][i], memory_order_relaxed);
            if (h == o->handle) { slot = i; break; }
            if (!h && slot < 0) slot = i;
        }
        if (slot < 0) slot = CSMP_SLOTS - 1;   /* more than 12 holders: the last slot is the overflow bucket */
        atomic_store_explicit(&g_csmp_h[c][slot], o->handle, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_csmp_cnt[c][slot], 1, memory_order_relaxed);
    }
}
/* ---------- M6s: per-thread core-wait totals, free of use-after-free ----------
   A fixed table keyed by handle, never freed, so the beat thread can print it without G and without
   chasing a thr_t that may already be gone. Two threads that reuse a handle share a row: the row is
   a profile, not a ledger. */
#define CW_SLOTS 24
static struct { _Atomic unsigned h; _Atomic unsigned long long n, ns; } g_cw[CW_SLOTS];
static _Atomic unsigned long long g_cw_max_ns;
static void cw_add(uint32_t h, uint64_t ns) {
    int slot = -1;
    for (int i = 0; i < CW_SLOTS; i++) {
        unsigned cur = atomic_load_explicit(&g_cw[i].h, memory_order_relaxed);
        if (cur == h) { slot = i; break; }
        if (!cur && slot < 0) slot = i;
    }
    if (slot < 0) slot = CW_SLOTS - 1;
    atomic_store_explicit(&g_cw[slot].h, h, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_cw[slot].n, 1ull, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_cw[slot].ns, ns, memory_order_relaxed);
    unsigned long long m = atomic_load_explicit(&g_cw_max_ns, memory_order_relaxed);
    while (ns > m && !atomic_compare_exchange_weak_explicit(&g_cw_max_ns, &m, ns, memory_order_relaxed, memory_order_relaxed)) {}
}
/* One [core] block per call (every 30 s from the heartbeat thread). Never takes G.
   The two tables are read with a snapshot index list, so each row names a distinct holder; the
   core-wait rows are printed as a delta against the previous line (a window), keeping the boot
   totals intact. */
void runtime_core_stats_dump(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    unsigned tot = atomic_load_explicit(&g_csmp_tot, memory_order_relaxed);
    if (!tot) { vk_boot_logf("[core] t=%us no sample yet", tsec); return; }
    vk_boot_logf("[core] t=%us samples=%u busy=[%.1f%% %.1f%% %.1f%%] of 3 virtual cores, ~10ms sampling",
                 tsec, tot,
                 100.0 - (double)atomic_load_explicit(&g_csmp_idle[0], memory_order_relaxed) * 100.0 / (double)tot,
                 100.0 - (double)atomic_load_explicit(&g_csmp_idle[1], memory_order_relaxed) * 100.0 / (double)tot,
                 100.0 - (double)atomic_load_explicit(&g_csmp_idle[2], memory_order_relaxed) * 100.0 / (double)tot);
    /* M6t: A1 give-up accounting (how often a blocking host wait handed its core to someone else, and the
       total time spent off-core) + A2 off-mask fallbacks. Totals since boot, computed without G. */
    vk_boot_logf("[core]   release: n=%llu off_core=%.1fs soft_offmask=%llu soft=%d",
                 atomic_load_explicit(&g_core_rel_n, memory_order_relaxed),
                 (double)atomic_load_explicit(&g_core_rel_ns, memory_order_relaxed) / 1e9,
                 atomic_load_explicit(&g_core_soft_n, memory_order_relaxed), core_soft());
    for (int c = 0; c < NCORE; c++) {
        int pick[3] = { -1, -1, -1 };
        for (int k = 0; k < 3; k++) {
            unsigned bc = 0;
            for (int i = 0; i < CSMP_SLOTS; i++) {
                unsigned h = atomic_load_explicit(&g_csmp_h[c][i], memory_order_relaxed);
                if (!h) continue;
                int dup = 0;
                for (int j = 0; j < k; j++) if (pick[j] == i) dup = 1;
                if (dup) continue;
                unsigned cnt = atomic_load_explicit(&g_csmp_cnt[c][i], memory_order_relaxed);
                if (cnt > bc) { bc = cnt; pick[k] = i; }
            }
        }
        char buf[200]; unsigned off = 0;
        for (int k = 0; k < 3 && pick[k] >= 0; k++) {
            unsigned h = atomic_load_explicit(&g_csmp_h[c][pick[k]], memory_order_relaxed);
            unsigned cnt = atomic_load_explicit(&g_csmp_cnt[c][pick[k]], memory_order_relaxed);
            off += (unsigned)snprintf(buf + off, sizeof buf - off, " h=%#x %.1f%%", h, (double)cnt * 100.0 / (double)tot);
        }
        if (!off) snprintf(buf, sizeof buf, " (idle)");
        vk_boot_logf("[core]   core%d top:%s", c, buf);
    }
    {   /* per-thread core waits, top 4 by accumulated handoff time, printed as the delta since the
           previous [core] line so a burst cannot hide behind the boot total */
        static unsigned long long prev_n[CW_SLOTS], prev_ns[CW_SLOTS];
        unsigned long long now_ns_v[CW_SLOTS], now_n_v[CW_SLOTS];
        for (int i = 0; i < CW_SLOTS; i++) {
            now_ns_v[i] = atomic_load_explicit(&g_cw[i].ns, memory_order_relaxed);
            now_n_v[i] = atomic_load_explicit(&g_cw[i].n, memory_order_relaxed);
        }
        int pick[4] = { -1, -1, -1, -1 };
        for (int k = 0; k < 4; k++) {
            unsigned long long bd = 0;
            for (int i = 0; i < CW_SLOTS; i++) {
                if (!atomic_load_explicit(&g_cw[i].h, memory_order_relaxed)) continue;
                int dup = 0;
                for (int j = 0; j < k; j++) if (pick[j] == i) dup = 1;
                if (dup) continue;
                unsigned long long d = now_ns_v[i] - prev_ns[i];
                if (d >= bd) { bd = d; pick[k] = i; }
            }
            if (pick[k] < 0) break;
        }
        char buf[240]; unsigned off = 0;
        for (int k = 0; k < 4 && pick[k] >= 0; k++) {
            int i = pick[k];
            unsigned h = atomic_load_explicit(&g_cw[i].h, memory_order_relaxed);
            off += (unsigned)snprintf(buf + off, sizeof buf - off, " h=%#x +%llu/%.0fms", h,
                                      now_n_v[i] - prev_n[i], (double)(now_ns_v[i] - prev_ns[i]) / 1e6);
        }
        for (int i = 0; i < CW_SLOTS; i++) { prev_ns[i] = now_ns_v[i]; prev_n[i] = now_n_v[i]; }
        vk_boot_logf("[core]   waits(window):%s max_single_ms=%.0f", off ? buf : " none",
                     (double)atomic_load_explicit(&g_cw_max_ns, memory_order_relaxed) / 1e6);
    }
}
static void *core_watch(void *arg) {
    (void)arg;
    unsigned tick = 0;
    for (;;) {
        struct timespec d = {0, 1000000}; nanosleep(&d, NULL);
        pthread_mutex_lock(&G); uint64_t now = now_ns();
        if ((++tick % 10) == 0) csmp_take();   /* M6s: ~10 ms occupancy sample, G already held */
        for (thr_t *w = g_core_wait; w; w = w->cnext) if (now - w->cwait_ns > CORE_STARVE_NS)   /* a starving waiter: preempt an owner of one of its cores */
            for (int i = 0; i < NCORE; i++) { thr_t *o = g_core_owner[i]; if (o && (core_allowed(w, i) || core_soft()) && !o->preempt) { o->preempt = 1; o->preempt_ns = now - CORE_PREEMPT_NS - 1; break; } }
        for (int i = 0; i < NCORE; i++) { thr_t *o = g_core_owner[i];
            if (o && o->preempt && !o->kicked && now - o->preempt_ns > CORE_PREEMPT_NS) { o->kicked = 1; (void)pthread_kill(o->pt, CORE_PREEMPT_SIGNAL); } }
        pthread_mutex_unlock(&G);
    }
    return NULL;
}
static void core_watch_init(void) {
    struct sigaction a = {0}; a.sa_sigaction = core_preempt_handler; a.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART; sigemptyset(&a.sa_mask);
    if (sigaction(CORE_PREEMPT_SIGNAL, &a, NULL)) runtime_fail("core preemption signal");
    pthread_t w; if (pthread_create(&w, NULL, core_watch, NULL)) runtime_fail("core preemption watcher");
    pthread_detach(w);
}
static void core_watch_start(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, core_watch_init); }
void runtime_thread_note_hle(unsigned i) { thr_t *t = me_or_null(); if (t) t->last_hle = (int)i + 1; }   // hle_stack.c, while tracing
/* M5b：标记「本线程正要进哪个宿主 NVN 重活」。只写入口不写出口——线程若卡在这个调用里，op 就一直是它，
   年龄（beat 线程算）随冻结时长增长；正常跑动时下一笔很快把它换掉。零锁、两次普通写。
   M5c：同时把 (op, arg, 时刻) 记进 16 槽环（phe_seq++），arg 由调用方给（步骤号/计数/键值），
   冻结 dump 打末 6 条 —— 见 [gst] ph= 行。 */
void runtime_thread_note_op2(int op, uint64_t arg) {
    thr_t *t = me_or_null();
    if (!t) return;
    t->op = op; t->op_ns = now_ns();
    unsigned i = t->phe_i & 15u;
    t->phev[i].op = op; t->phev[i].arg = arg; t->phev[i].ns = t->op_ns;
    t->phe_i = i + 1u;
    t->phe_seq++;
}
void runtime_thread_note_op(int op) { runtime_thread_note_op2(op, 0); }
/* M5c：op 编号 → 可读名（冻结表里 `op=21(bind/pipe)` 比 `op=21` 有用得多）。 */
const char *runtime_op_name(int op) {
    switch (op) {
    case 0: return "-";
    case 1: return "submit";      case 2: return "pipeline";   case 3: return "tex_up";
    case 4: return "submit_walk"; case 5: return "submit_rtfl";case 6: return "submit_dflush";
    case 7: return "submit_rcpt"; case 8: return "present";
    case 21: return "pipe_enter"; case 22: return "pipe_module_ok"; case 23: return "pipe_layout";
    case 24: return "pipe_create"; case 25: return "pipe_create_ret";
    case 26: return "pipe_cached"; case 27: return "draw_post_pipe"; case 28: return "draw_vexpand";
    case 29: return "draw_record"; case 30: return "draw_exit";  case 31: return "draw_flush";
    case 32: return "flush_wait";  case 33: return "flush_post";  case 34: return "cmd_step";
    case 11: return "pipe_gone";   /* M5c: placeholder, unused */
    default: break;
    }
    /* M5c：60+step = 管线细分步（nvn_vk.c j_mark），60..96。只在冻结 dump 里调用，静态缓冲即可。 */
    if (op >= 60 && op < 97) { static char b[8]; snprintf(b, sizeof b, "p%d", op - 60); return b; }
    return "?";
}
uint32_t runtime_thread_current_handle(void) { thr_t *t = me_or_null(); return t ? t->handle : 0; }
/* M7c: per-thread guest handle in TLS, refreshed at every svc entry (+ thread_main start).
   The gate observers (game_patches.c, e.g. the fnA entry gate) run on the guest thread's own
   host stack in user mode and read this instead of calling pthread_getspecific (observer
   discipline: no libc). Guest code cannot switch threads, so the value is current between
   svcs; a thread's very first fnA before any svc falls back to thread_main's store. */
__thread uint32_t runtime_cur_handle_tls;
/* M6r: live guest threads, lock-free atomic count. The 2 s change detector needs "a new thread
   appeared" as an advance event without taking G to walk the list every 2 s. +1 where a thr_t
   joins g_all, -1 in thread_exit_on_host. Pure counter, never a scheduling input. */
static _Atomic int g_ntlive;
int runtime_live_threads(void) { return atomic_load_explicit(&g_ntlive, memory_order_relaxed); }
/* ---------- M6i: who does the I/O (read-only ledger, always on) ----------
   The filesystem services in this port are in-process calls made on the guest thread that issued
   SendSyncRequest (fs_host.c -> romfs.c; there is no I/O worker pool), so counting at the call site
   names the thread directly. Three kinds: 1 = open, 2 = read, 3 = an IPC/service request dispatched.
   The host bucket (handle 0) keeps whatever a host thread does, so the per-thread rows add up to the
   global [io] romfs totals instead of silently losing those calls. Nothing here reads guest memory,
   allocates, or takes a lock; the counters are pure adds on the caller's own thr_t. */
static _Atomic uint64_t g_hio_open, g_hio_read, g_hio_bytes, g_hio_ipc;
static _Atomic uint64_t g_hio_last_ns;
static char g_hio_path[96];
void runtime_io_note(int kind, uint64_t bytes, const char *path) {
    thr_t *t = me_or_null();
    if (!t) {
        if (kind == 1) atomic_fetch_add_explicit(&g_hio_open, 1, memory_order_relaxed);
        else if (kind == 2) { atomic_fetch_add_explicit(&g_hio_read, 1, memory_order_relaxed);
                              atomic_fetch_add_explicit(&g_hio_bytes, bytes, memory_order_relaxed); }
        else atomic_fetch_add_explicit(&g_hio_ipc, 1, memory_order_relaxed);
        if (kind != 3) atomic_store_explicit(&g_hio_last_ns, now_ns(), memory_order_relaxed);
        /* Only an open names a file: an IPC request carries a service name, and writing that here made
           the first boot print last_path=IAudioRenderer for a thread that never touched a file. */
        if (path && kind == 1) { size_t k = 0; while (k + 1 < sizeof g_hio_path && path[k]) { g_hio_path[k] = path[k]; k++; } g_hio_path[k] = 0; }
        return;
    }
    if (kind == 1) atomic_fetch_add_explicit(&t->io_open, 1, memory_order_relaxed);
    else if (kind == 2) { atomic_fetch_add_explicit(&t->io_read, 1, memory_order_relaxed);
                          atomic_fetch_add_explicit(&t->io_bytes, bytes, memory_order_relaxed); }
    else atomic_fetch_add_explicit(&t->io_ipc, 1, memory_order_relaxed);
    if (kind != 3) {
        atomic_store_explicit(&t->io_last_io_ns, now_ns(), memory_order_relaxed);
        /* 当时在飞的那个 SVC：send-sync 的 lr 就是客体「发起 I/O 之后会回到哪」。只拷不判。 */
        atomic_store_explicit(&t->io_last_lr, t->last_lr, memory_order_relaxed);
        atomic_store_explicit(&t->io_last_svc, (uint32_t)(t->last_svc & 0xff), memory_order_relaxed);
    }
    if (path && kind == 1) { size_t k = 0; while (k + 1 < sizeof t->io_path && path[k]) { t->io_path[k] = path[k]; k++; } t->io_path[k] = 0; }
}
/* ---------- M6j: [mv] 影片文件生命周期（派活单 A1）----------
   只认 path 里带 "MovieData" 的 RomFS 文件（SMO 的 .mp4 都在这个目录），给：
     [mv] open path=… h=… by=thr0x… t=…s
     [mv] read h=… off=… len=… ret=… n=… tot=… by=thr0x… t=…s   （前 32 条全打，之后每 8 条一条）
     [mv] close h=… reads=… bytes=… by=thr0x… t=…s
   句柄表 4 槽（游戏同时只开一个影片）；open 才做一次 strstr，read/close 是 O(4) 查表。
   日志走 vk_boot_logf（逐行 flush、自带锁），一局几十行。不改任何 I/O 计数口径。 */
static struct { uint64_t h; uint32_t by; int n; uint64_t bytes; } g_mvf[4];
static double g_mv_t0;
void runtime_movie_fs_note(int kind, uint64_t h, int64_t off, uint64_t len, uint64_t ret, const char *path) {
    extern void vk_boot_logf(const char *fmt, ...);
    if (kind == 0) {
        if (!path || !strstr(path, "MovieData")) return;
        uint64_t now = now_ns();
        if (g_mv_t0 <= 0) g_mv_t0 = (double)now;
        int slot = -1;
        for (int i = 0; i < 4; i++) if (!g_mvf[i].h) { slot = i; break; }
        if (slot < 0) slot = 0;
        g_mvf[slot].h = h; g_mvf[slot].by = runtime_thread_current_handle();
        g_mvf[slot].n = 0; g_mvf[slot].bytes = 0;
        vk_boot_logf("[mv] open path=%s h=%#llx by=thr%#x t=%.1fs", path, (unsigned long long)h,
                     g_mvf[slot].by, (now - (uint64_t)g_mv_t0) / 1e9);
        return;
    }
    for (int i = 0; i < 4; i++) if (g_mvf[i].h == h) {
        uint64_t now = now_ns();
        if (g_mv_t0 <= 0) g_mv_t0 = (double)now;
        if (kind == 1) {
            g_mvf[i].n++; g_mvf[i].bytes += ret;
            if (g_mvf[i].n <= 32 || (g_mvf[i].n % 8) == 0)
                vk_boot_logf("[mv] read h=%#llx off=%lld len=%llu ret=%llu n=%d tot=%llu by=thr%#x t=%.1fs",
                             (unsigned long long)h, (long long)off, (unsigned long long)len,
                             (unsigned long long)ret, g_mvf[i].n, (unsigned long long)g_mvf[i].bytes,
                             runtime_thread_current_handle(), (now - (uint64_t)g_mv_t0) / 1e9);
        } else {
            vk_boot_logf("[mv] close h=%#llx reads=%d bytes=%llu by=thr%#x t=%.1fs",
                         (unsigned long long)h, g_mvf[i].n, (unsigned long long)g_mvf[i].bytes,
                         runtime_thread_current_handle(), (now - (uint64_t)g_mv_t0) / 1e9);
            g_mvf[i].h = 0;
        }
        return;
    }
}
// Host services that wait for other threads or the GPU give the core up around the wait (no host locks held).
int runtime_core_away(void) {
    thr_t *t = me_or_null(); if (!t || t->cur_core < 0) return 0;
    pthread_mutex_lock(&G); t->rel_t0 = now_ns(); core_put(t); pthread_mutex_unlock(&G); return 1;
}
void runtime_core_back(int *away) {
    thr_t *t = me_or_null(); if (!*away || !t) return;
    *away = 0;   /* M6t: cleanup attribute may call this twice on some paths; the second call is a no-op */
    pthread_mutex_lock(&G);
    uint64_t d = t->rel_t0 ? now_ns() - t->rel_t0 : 0;
    t->rel_t0 = 0;
    if (d) { atomic_fetch_add_explicit(&g_core_rel_n, 1ull, memory_order_relaxed);
             atomic_fetch_add_explicit(&g_core_rel_ns, d, memory_order_relaxed); }
    core_take(t);
    pthread_mutex_unlock(&G);
}
// Preemption point at SVC entry: a higher-priority thread waits for this core.
void runtime_core_checkpoint(void) {
    thr_t *t = me_or_null(); if (!t || !t->preempt) return;
    pthread_mutex_lock(&G); t->preempt = 0; core_put(t); core_take(t); pthread_mutex_unlock(&G);
}
int runtime_current_core(void) { thr_t *t = me_or_null(); return t ? (t->cur_core >= 0 ? t->cur_core : t->core) : -1; }

static int64_t block(thr_t *t,int kind,int64_t ns) {
    if (ns == 0) return block_inner(t, kind, ns);
    core_put(t); int64_t r = block_inner(t, kind, ns); core_take(t); return r;
}
static int64_t block_inner(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    struct timespec dl;
    int det_wait = ns > 0 && dm_detclock_enabled();
    uint64_t guest_deadline = det_wait ? guest_ns() + (uint64_t)ns : 0;
    if (ns > 0 && !det_wait) { clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000;
                  if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } }
    while (!t->woke) {
        int rc;
        if (det_wait) {
            uint64_t now = guest_ns();
            if (now >= guest_deadline) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
            (void)(guest_deadline - now);
            rc = pthread_cond_wait(&t->cv, &G);
        } else rc = ns > 0 ? pthread_cond_timedwait(&t->cv, &G, &dl) : pthread_cond_wait(&t->cv, &G);
        if (rc == ETIMEDOUT && !t->woke && (!det_wait || guest_ns() >= guest_deadline)) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    }
    return t->wres;
}

// ---------- 锁 ----------
// ---------- Locks ----------
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

// 释放 addr 处的锁：交给等在它上面的第一个线程，或清零（持 G）
// Release the lock at addr: hand it to the first thread waiting on it, or clear it to zero (holding G)
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
// 让 t 去拿 addr 处的锁：空闲就直接拿到并唤醒，否则置等待者位并转为锁等待（持 G）
// Make t take the lock at addr: if free, it gets it directly and is woken, otherwise set the waiters bit and turn into a lock wait (holding G)
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;      // 仍在 g_wait 里，改为锁等待 | Still in g_wait, changed to a lock wait
    t->wseq = ++g_wseq;                                  // 内核把它新插进锁持有者的等待队列：同优先级排在已有锁等待者之后 | The kernel inserts it anew into the lock owner's wait queue: at equal priority it goes after the existing lock waiters
}

// ---------- 线程 ----------
// ---------- Threads ----------
extern void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp,uint64_t *host_context,void *tcb);
extern _Noreturn void runtime_thread_leave(const uint64_t *host_context,void *arg,void(*fn)(void *));   // entry.S
uint32_t (*dm_thread_tick_fn)(void);   // 取当前游戏拍；diag.c 初始化时登记（单测不登记，拍号记 0） | Gets the current game tick; registered when diag.c initializes (unit tests do not register it, the tick number is recorded as 0)
void crash_thread_init(void);
void guest_thread_init(void);   // thread.c：每线程 TSD 槽（门、x18、TLS、TLR）唯一设置点 | thread.c: the single place that sets the per-thread TSD slots (gate, x18, TLS, TLR)
__attribute__((weak)) void runtime_hle_stack_set(uintptr_t top) { (void)top; }   // standalone thread probe: no host-stack switch
// ---------- 线程 QoS（donor dm/host/hle_thread.c:273-292 同构，round20b）----------
// Switch 优先级 → OHOS QoS 级。SMO_QOS_MAP 带状映射（默认开；=0 回 donor 缺省：客体线程一律
// USER_INTERACTIVE，性能控制器全压快核——donor 的 DM_QOS_MAP 真机 A/B 前默认关，本宿主反向默认开）。
// 带状：prio≤e1（默认 44，主线程即 44）USER_INTERACTIVE、≤e2（默认 50）USER_INITIATED、其余 UTILITY
// （SetThreadPriority 合法域 28-59 的 51-59 落 UTILITY）；SMO_QOS_EDGES=e1,e2 可改（env 法，非编译期）。
// svc_thread_main_init 里读一次，早于任何其它客体线程。真实现见 ohos_stub.c（dlopen libqos.so）。
static int g_qos_map = 1, g_qos_e1 = 44, g_qos_e2 = 50;
static void qos_map_init(void) {
    const char *e = getenv("SMO_QOS_MAP"), *d = getenv("SMO_QOS_EDGES");
    g_qos_map = e ? atoi(e) : 1;
    if (d) sscanf(d, "%d,%d", &g_qos_e1, &g_qos_e2);
    if (g_qos_map) LOG("SMO_QOS_MAP=1: guest threads get QoS by priority (≤%d interactive, ≤%d user-initiated, the rest utility)", g_qos_e1, g_qos_e2);
}
static qos_class_t prio_qos(int prio) {
    if (!g_qos_map) return QOS_CLASS_USER_INTERACTIVE;
    return prio <= g_qos_e1 ? QOS_CLASS_USER_INTERACTIVE : prio <= g_qos_e2 ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY;
}
static void qos_apply(thr_t *t) {   // 在目标线程自身上调用、持 G（prio/qos 与调度读者共享；donor hle_thread.c:287-290）
    qos_class_t q = prio_qos(t->prio);
    if ((uint32_t)q != t->qos && !pthread_set_qos_class_self_np(q, 0)) t->qos = (uint32_t)q;
}
// 宿主辅助线程（音频 notify/静音池/capture、vsync）从客体线程创建会继承客体类；SMO_QOS_MAP 下自设
// （donor dm_host_helper_qos，hle_thread.c:291-292）。本宿主一律 QOS_CLASS_DEFAULT：别继承客体高类。
void dm_host_helper_qos(int q) { if (g_qos_map) pthread_set_qos_class_self_np((qos_class_t)q, 0); }
static void *thread_main(void *p) {
    thr_t *t = p;
    runtime_cur_handle_tls = t->handle;   /* M7c: identity for the gate observers before the thread's first svc */
#if defined(__MUSL__) || defined(__OHOS__) || defined(__linux__)
    /* M6f: OHOS clang defines __OHOS__/__linux__, not __MUSL__ (the tree says so itself at
       game_slot.c:22), so the old guard left htid==0 for every guest thread: no join between a
       /proc/self/task row and its KThread. */
    t->htid = (int)syscall(__NR_gettid);
#endif
    {   /* emu3 定谳：采样器 pthread_kill(SIGUSR2) 全超时 + SigBlk 含 bit12 = 客体线程继承了创建者
           （appspawn 主线程）的屏蔽掩码。SIGUSR2 同时是 CORE_PREEMPT_SIGNAL（核抢占），屏蔽它
           也会让核抢占静默失效。客体线程出生即解除继承屏蔽（信号 handler 进程级已注册）。 */
        sigset_t us; sigemptyset(&us); sigaddset(&us, SIGUSR2);
        pthread_sigmask(SIG_UNBLOCK, &us, NULL);
    }
    {   /* round3：客体线程装独立信号栈——SIGUSR2 采样 handler 若落在客体栈上，SP 贴保护页的
           深递归线程一压栈即 SEGV_ACCERR（emu_round3 第二次崩溃，0x6e03a47ffc 即栈保护区）。
           16KiB 备用栈随线程泄漏（取证构建可接受），SA_ONSTACK 已在 spin_sampler_start 注册。 */
        stack_t ss; ss.ss_sp = malloc(16384);
        if (ss.ss_sp) { ss.ss_size = 16384; ss.ss_flags = 0; sigaltstack(&ss, NULL); }
    }
    pthread_setspecific(g_keythr, t);
    // donor hle_thread.c:296-303：CreateThread 可能已把 t 链入 g_all（读者与 SetThreadPriority 持 G），
    // 持 G 设自身 QoS 并读回；QoS 级按带状映射取（SMO_QOS_MAP=0 时即全 USER_INTERACTIVE）
    pthread_mutex_lock(&G);
    pthread_set_qos_class_self_np(prio_qos(t->prio), 0);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    t->mport = pthread_mach_thread_np(pthread_self());
    pthread_mutex_unlock(&G);

    guest_thread_init();
    pthread_mutex_lock(&G); while (!t->started) pthread_cond_wait(&t->cv, &G); core_take(t); pthread_mutex_unlock(&G);
    { volatile char marker = 0; runtime_hle_stack_set((uintptr_t)&marker - 1024); }   // host services of this thread run below this frame
    /* fw_kick() disabled: creating a pthread right before dm_thread_enter kills the guest entry (SIGSEGV ~120 ms later) */
    dm_thread_enter(t->entry, t->arg, t->stack_top,t->host_context,runtime_current_tcb());   // 不返回：客体须经 ExitThread 退出 | Does not return: the guest must exit through ExitThread
    die("客体线程入口返回");
}
static void thr_free(thr_t *t) {   // 持 G；从全部线程链摘下并释放 | Holding G; unlink from the all-threads chain and free
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    /* M6i: keep the I/O ledger of a thread that is about to disappear. Without this, a loader thread
       that ended would take its open/read counts with it and the per-thread totals would silently
       stop matching the [io] romfs totals. Copy only; the atomics stay the same, nothing decides. */
    uint64_t ro = atomic_load_explicit(&t->io_open, memory_order_relaxed);
    uint64_t rr = atomic_load_explicit(&t->io_read, memory_order_relaxed);
    uint64_t rb = atomic_load_explicit(&t->io_bytes, memory_order_relaxed);
    uint64_t ri = atomic_load_explicit(&t->io_ipc, memory_order_relaxed);
    if (ro || rr || ri) {
        atomic_fetch_add_explicit(&g_io_ret_open, ro, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_io_ret_read, rr, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_io_ret_bytes, rb, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_io_ret_ipc, ri, memory_order_relaxed);
        unsigned idx = atomic_fetch_add_explicit(&g_io_ret_n, 1, memory_order_relaxed);
        if (idx < IO_RETIRED) {
            g_io_ret[idx].h = t->handle; g_io_ret[idx].open = ro; g_io_ret[idx].rd = rr;
            g_io_ret[idx].bytes = rb; g_io_ret[idx].ipc = ri;
            g_io_ret[idx].entry = t->entry;
            g_io_ret[idx].llr = atomic_load_explicit(&t->io_last_lr, memory_order_relaxed);
            g_io_ret[idx].exit_ns = t->exit_ns;
        }
    }
    if (t->spin_pin) return;   // round3：采样器正引用（放锁 kill/等期间），只摘链不回收，泄露一次 | Sampler still references it outside G: unlink only, leak this one
    pthread_cond_destroy(&t->cv); free(t);
}

static _Noreturn void thread_exit_on_host(void *argument) {
    thr_t *t=argument;
    t->exit_ns = now_ns(); t->exit_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
    // 这里不打日志：退出发生在客体线程自己的栈上（有的只有十几 KiB），带浮点的 printf 会把栈撑爆（真机冒烟：LibcurlResolver 退出时 SIGBUS，回溯 fprintf → __dtoa）。时刻只记下来，现场导出时再打
    // No logging here: the exit happens on the guest thread's own stack (some are only a dozen or so KiB), and a printf with floating point would blow the stack (real-device smoke test: SIGBUS when LibcurlResolver exits, backtrace fprintf → __dtoa). Only the time is recorded, and it is printed later in the state dump
    pthread_mutex_lock(&G);
    core_put(t);
    t->exited = 1;
    atomic_fetch_sub_explicit(&g_ntlive, 1, memory_order_relaxed);   /* M6r: advance detector */
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }   // 同步等待者重查 | Synchronization waiters re-check
    pthread_setspecific(g_keythr, NULL);
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
    pthread_exit(NULL);
}
static _Noreturn void thread_exit(thr_t *t) {
 if(t->host_context[0])runtime_thread_leave(t->host_context,t,thread_exit_on_host);
 thread_exit_on_host(t);
}
void dm_thread_returned(void) { die("客体线程入口函数返回（Horizon 要求 ExitThread）"); }

// 主线程登记，返回其句柄（交给 nnosInitialize 作主线程句柄）
// Register the main thread and return its handle (passed to nnosInitialize as the main thread handle)
uint32_t svc_thread_main_init(void) {
    pthread_key_create(&g_keythr, NULL);
    qos_map_init();   // donor hle_thread.c:480：早于任何其它客体线程读一次开关与带边
    thr_t *t = calloc(1, sizeof *t);
    t->qos = (uint32_t)QOS_CLASS_UNSPECIFIED;   /* calloc 零值在本 shim 里恰是 INTERACTIVE（Apple 原值 0=UNSPECIFIED），标「未读回」防 qos_apply 首设被去重跳过 */
    pthread_cond_init(&t->cv, NULL);
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 7; t->pt = pthread_self(); t->mport = pthread_mach_thread_np(t->pt);
#if defined(__MUSL__) || defined(__OHOS__) || defined(__linux__)
    t->htid = (int)syscall(__NR_gettid);   /* M6f: the main guest thread is the caller's own thread (never enters thread_main), so without this it had no /proc tid and dropped out of the [cpu] table */
#endif
    t->cur_core = -1;
    t->svch = calloc(256, sizeof(uint32_t));   /* M6h: [own] svc_top needs the main thread's own histogram too */
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t, 0); t->id = g_next_id++; t->all = g_all; g_all = t; core_take(t); pthread_mutex_unlock(&G);
    atomic_fetch_add_explicit(&g_ntlive, 1, memory_order_relaxed);   /* M6r: advance detector */
    t->mvs = lp_mv_slot(t->handle);   /* M6l: the main thread is mover slot 0 */
    pthread_setspecific(g_keythr, t);
    qos_apply(t);   // donor hle_thread.c:490：发布主线程前先按客体策略设级（继承自 appspawn 的宿主缺省不可靠）
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    LOG("main guest thread QoS %#x (priority %d)", t->qos, t->prio);   // donor hle_thread.c:497 生效行（真机探针 grep "QoS"）
    return t->handle;
}

static _Atomic uint64_t g_main_yield_n, g_main_yield_ns;   // 只有客体主线程写；诊断行在同一线程读 | Written only by the guest main thread; the diagnostic line reads it on the same thread
void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = g_main_yield_n; *ns = g_main_yield_ns; }
uint32_t dm_guest_work_qos(unsigned *count) {
    uint32_t qos = QOS_CLASS_UNSPECIFIED; unsigned n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->handle == 0x1001) continue;
        if (!n) qos = t->qos; else if (qos != t->qos) qos = UINT32_MAX;
        n++;
    }
    pthread_mutex_unlock(&G);
    if (count) *count = n;
    return qos;
}

// ---------- 同步对象 ----------
// ---------- Synchronization objects ----------
static int sig_obj(uint32_t h, int *bad) {    // 1=已触发（持 G） | 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
// Wake only the waiters one of whose objects is now signaled (all of them when the wait has more handles than the 4 kept;
// also those holding a handle that is no longer valid, so they return the error). Upstream MacNativeGames dab728a (from
// nxnative df7badd): semantics unchanged, a waiter none of whose objects is signaled would only go back to sleep. Holding G.
static void wake_sync_waiters(void) {
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next;
        if (w->wkind == 4) { int hit = w->nwh > 4, bad = 0; for (int i = 0; !hit && i < w->nwh; i++) hit = sig_obj(w->wh[i], &bad) || bad; if (hit) wake(w, -1); }
        w = n; }
}

static int64_t now_mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ll + ts.tv_nsec; }
void dm_guest_clock_advanced(void) {
    if (!dm_detclock_enabled()) return;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_wait; t; t = t->next) if (t->wkind) pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&G);
}

// 服务层 SystemEvent 的绑定（见下面 hle_bind_system_event）：客体 SystemEventType 地址 → 它那个内核事件写端的句柄。
// 绑定与写端句柄同生共死：CloseHandle 关掉写端时摘掉它（se_unbind），同一地址上销毁后重建的 SystemEvent 会得到新的内核事件。
// 原先绑定永不结束：第二次绑定见到旧项直接返回，新的内核事件根本没建，触发发给已关的句柄（王国之泪的句柄编号会复用，于是发给了占了那个编号的别的事件）。
// 例如 nfp::AttachAvailabilityChangeEvent：游戏每次用 amiibo 都可能在同一地址重建这个事件（nxnative 49ee623 同理）。锁的次序：先 G 后 g_se_lock（持 g_se_lock 时不取 G）
// Service-layer SystemEvent bindings (see hle_bind_system_event below): guest SystemEventType address -> the handle of its kernel event's
// write end. A binding lives as long as that handle: CloseHandle on the write end removes it (se_unbind), so a SystemEvent destroyed and
// created again at the same address gets a new kernel event. Bindings used to stay forever: the second bind found the old entry and
// returned, the new kernel event was never created, and signals went to the closed handle (Tears of the Kingdom reuses handle numbers,
// so to whatever event took that number). nfp::AttachAvailabilityChangeEvent, for one, may create the event again at the same address
// each time the game uses amiibo (nxnative 49ee623 found the same). Lock order: G, then g_se_lock (nothing takes G holding g_se_lock).
static struct { void *ev; uint32_t wh; } g_se[512];
static int g_nse;
/* 终局八段后 F-H 取证（round42 现场复核轮）：svc 0x18 等待句柄含 0x1003（装载器 SystemEvent 读端）
   时记录的现场——g_loader_wait_x19=网关保存区的客体重寄存器 x19（被调保留寄存器：SDK 的
   WaitSystemEvent 薄包装不改写它，等于游戏等待函数 0x942370-fn 里的装载器对象 [x19] 本尊，
   终局二段布局 SystemEvent@+0x1d8/state@+0xa0/progress-out@+0xa4）；g_loader_wait_hs=句柄数组
   客体地址（若 SDK 直接传 &type->read_handle 则它落在 loader+0x1d8 域内，可互证）。dump_threads
   据此直读 state/progress 两字段现值。 */
static uint64_t g_loader_wait_x19, g_loader_wait_hs;
static pthread_mutex_t g_se_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t se_find(void *ev) {   // 调用方持 g_se_lock | Caller holds g_se_lock
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) return g_se[i].wh;
    return 0;
}
static void se_unbind(uint32_t wh) {   // 调用方持 G | Caller holds G
    pthread_mutex_lock(&g_se_lock);
    for (int i = 0; i < g_nse; ) if (g_se[i].wh == wh) g_se[i] = g_se[--g_nse]; else i++;
    pthread_mutex_unlock(&g_se_lock);
}

// 处理线程 / 同步类 SVC；返回 1 = 已处理
// Handle thread / synchronization SVCs; returns 1 = handled
/* Yield back-off (after nxnative e06a8ec / b703be2). On the Switch a yield with nothing else runnable costs nothing; here
   a guest loop that polls and yields becomes a host busy spin (on the iPhone the system logged about 1,300 wake-ups a
   second for the game). A run of yields with no other SVC between them stays plain sched_yield for its first 2 ms:
   shorter waits are handoffs between working threads, such as the main thread waiting for its ModuleSystem workers on
   the frame's critical path (nxnative started at 200 µs and 1.4.3 then dropped itself to 20 fps). After that each yield
   sleeps an eighth of the run so far, 10 to 250 µs, with the virtual core given up, so a wait ends at most an eighth late.
   Off by default: on the Mac at about 100 fps (1.2.1 field save, 300 s each) it saved 3% CPU (708 -> 685 s) but frames
   dropped steadily, 10-15 per 300 instead of 0-3. TOTK_YIELD_BACKOFF=on turns it on (250:2000); =<cap µs>[:<start µs>]
   sets the cap and the start. Returns the sleep for a yield whose run started run_ns ago, 0 for a plain yield. */
static uint64_t yield_backoff_ns(uint64_t run_ns) {
    static int64_t cap = -1, start = 2000000;
    if (cap < 0) {
        const char *e = getenv("TOTK_YIELD_BACKOFF"); int64_t c = 0;
        if (e && !strcmp(e, "on")) c = 250000;
        else if (e && *e) { c = atoll(e) * 1000; const char *colon = strchr(e, ':'); if (colon) start = atoll(colon + 1) * 1000; }
        if (c) LOG("TOTK_YIELD_BACKOFF: a run of yields backs off after %lld us, sleeping at most %lld us", (long long)(start / 1000), (long long)(c / 1000));
        cap = c;
    }
    if (!cap || run_ns < (uint64_t)start) return 0;
    uint64_t s = run_ns / 8; if (s < 10000) s = 10000;
    return s < (uint64_t)cap ? s : (uint64_t)cap;
}

// ---------- 事件 SVC 环形日志（挂起取证：0x1003/0x101f 的身世）----------
// 容量 64，溢出覆盖最旧；无锁写（多线程同时写竞态可接受：只为 dump 时刻的最近历史）。
// 记 {op, 句柄, 第二句柄(仅 create 的另一端), 调用者 LR 的 24 字符摘要}；摘要写入时经
// runtime_describe_address 符号化（"main+0x9424e8" 一类），dump 直接打印存量、不再反查。
// dump_threads 末尾回放最后 16 条：谁建的、谁 signal/clear/reset 过、写端被谁关了。
// ---------- Event SVC ring log (hang forensics: the identity of 0x1003/0x101f) ----------
enum { EVLOG_CREATE, EVLOG_SIGNAL, EVLOG_CLEAR, EVLOG_RESET, EVLOG_CLOSE };
static const char *const evlog_names[] = { "create", "signal", "clear", "reset", "close" };
typedef struct { uint32_t op, handle, extra; uint64_t lr; } evlog_t;   // round8: 存裸 LR（uint64 替代 char[24]）——0x1026 reset 洪 12-16k/s，热路径省掉每条 runtime_describe_address，dump 时再符号化
static evlog_t g_evlog[64];
static unsigned g_evlog_head, g_evlog_n;   // head=下一写位置；n=累计条数（回放窗口 min(n,16)）
void runtime_describe_address(uint64_t a, char *out, size_t n);   // 定义在诊断段（weak），此处前向声明
void runtime_watch_add(uintptr_t addr, size_t len, int read);   // gfx_bridge.c：客体地址 READ/WRITE watch（fault 打印 pc/lr/帧链；round3 自旋期取证用）
void spin_sampler_start(void);   // threads.c 诊断段：SIGUSR2 pc 链采样器（OHOS 上 mprotect watch 被 DFX 判死后的替代取证）

/* ---------- round6 观测（只计数，dump 时输出；svc 热路径零 I/O——持 G 的 fprintf 即死，round3 四跑实证）----------
   1) svc 0x34/0x35 裸计数：总数 + per-addr 前几名 + 0x35 调用者句柄。round47 dump 全程「0x35 零命中」
      （活线程无一个以 0x35 收尾），下一轮 dump 直接给出调用总量 / 地址分布 / 释放者身份。
      表与计数在两个 svc case 内、全程持 G 更新；dump 尾部放锁后无锁读（与 evlog/线程段同级取证风险）。
   2) 事件账本 per-handle signal/reset 累计：evlog 环 64 槽被 0x1026（audio_device_handler 事件读端，
      sdk+0xd3f084 每 5ms 一轮 reset，round3 三分钟 320 万条）淹没，尾部 16 条回放只见它——dump 时按
      句柄打累计值表，环回放保留作最近时序。计数在 evlog() 内（该函数本就无锁写、竞态可接受）；
      数组按句柄编号直接索引（与句柄表同为 MAX_H），游戏传入的坏句柄越界防护后不入账；
      句柄编号会复用（CloseHandle 后同号可给别的事件），长时间运行后同号计数会合并——取证窗口（分钟级）内无碍。 */
/* M6f: per-address wake attribution for the address family. The task needs, for one waited-on
   address (main's `obj=`), (a) how many signals it got, (b) how many waiters those signals actually
   woke, (c) how many signals woke nobody, and (d) WHO signed it last. n34/n35/caller already existed
   (first signaller only); the three new fields are the 1:1 test. All plain counters updated under G
   in the two svc cases, read without G at dump time (same risk class as the fields above). */
#define SIGADDR_TOP 16
static struct { uint64_t addr, n34, n35; uint32_t caller;
                uint64_t wake;        /* waiters actually woken by 0x35 on this addr */
                uint64_t zero;        /* 0x35 calls on this addr that woke nobody */
                uint32_t lastcaller;  /* most recent 0x35 caller handle */
                uint64_t lastlr;      /* that caller's guest LR at the svc (its call site) */
              } g_sigaddr[SIGADDR_TOP];
static unsigned g_sigaddr_n;
static uint64_t g_ovf34, g_ovf35;
static _Atomic uint64_t g_n34, g_n34_blk, g_n35, g_n35_woke, g_n35_bad;
static _Atomic uint64_t g_evcnt[MAX_H][2];   /* [h-0x1000][0]=signal(svc 0x11 + 宿主服务侧) [1]=reset(svc 0x17) */
static _Atomic uint64_t g_evfirst[MAX_H][2]; /* round8 归因：同上索引，signal/reset 各存首见裸 LR（照 sigaddr_note 的 caller 首见捕获范式）——0=空槽；dump 侧 who= 单列可见 */
/* ---------- M5l 同步事件账本（句柄族）：谁在等、谁发过信号 ----------
   动机（派活单 B.2）：请一个「waits 在涨而 signals 不涨」的对象点名。
   条件键族（0x1C/0x1D）与地址族（0x34/0x35）的 per-key 账本仓里已有（g_pkey / g_sigaddr），
   唯独**句柄族**（WaitSynchronization 0x18 / SignalEvent 0x11）没有 waits 累计——g_evcnt 只记
   signal/reset。这里按句柄编号直接索引补齐（与 g_evcnt 同构图、同 MAX_H 上界）：
     waits   = svc 0x18 每进一次等待就对等待数组里每个句柄 +1（同一笔里同一句柄只加一次）
     signals = svc 0x11（SignalEvent 成功）
   热路径代价：0x18 每次 ≤8 次 relaxed 原子加，0x11 每次 1 次；零 I/O、零新锁。
   句柄编号会复用（CloseHandle 后同号可给别的对象），分钟级取证窗口内无碍（g_evcnt 同注）。 */
static _Atomic uint64_t g_syhw[MAX_H], g_syhs[MAX_H];
/* 3) h=0x1023（round3 dump 画像：prio 28 音频工作线程，wait=4 等 0x101f 事件读端，nsvc ~700k/min）
      醒来计数——svc 0x18 返回路径打点：0x101f 是否持续把它唤醒。持 G 更新，plain 计数。 */
static uint64_t g_w1023_calls, g_w1023_wake, g_w1023_wake_101f;
static void sigaddr_note(int is35, uint64_t addr, uint32_t caller) {   // 调用方持 G
    if (is35) atomic_fetch_add_explicit(&g_n35, 1, memory_order_relaxed);
    else      atomic_fetch_add_explicit(&g_n34, 1, memory_order_relaxed);
    for (unsigned i = 0; i < g_sigaddr_n; i++) if (g_sigaddr[i].addr == addr) {
        if (is35) { g_sigaddr[i].n35++; if (!g_sigaddr[i].caller) g_sigaddr[i].caller = caller; }   // 首位 0x35 调用者=释放者画像
        else g_sigaddr[i].n34++;
        return;
    }
    if (g_sigaddr_n < SIGADDR_TOP) {   // 首见地址入表（16 槽后计入溢出桶：客体地址型对象本就寥寥）
        g_sigaddr[g_sigaddr_n].addr = addr; g_sigaddr[g_sigaddr_n].n34 = !is35; g_sigaddr[g_sigaddr_n].n35 = !!is35;
        g_sigaddr[g_sigaddr_n].caller = is35 ? caller : 0; g_sigaddr_n++;
        return;
    }
    if (is35) g_ovf35++; else g_ovf34++;
}
/* M6f: called from the 0x35 case after the wake loop (holding G). `woke` is the number of waiters
   this single signal actually released; `lr` is the signaller's guest call site (its own last_lr,
   already stored by the svc entry). Pure counters, no I/O. */
static void sigaddr_woke_note(uint64_t addr, int woke, uint32_t caller, uint64_t lr) {
    for (unsigned i = 0; i < g_sigaddr_n; i++) if (g_sigaddr[i].addr == addr) {
        g_sigaddr[i].wake += (uint64_t)(woke > 0 ? woke : 0);
        if (woke <= 0) g_sigaddr[i].zero++;
        g_sigaddr[i].lastcaller = caller; g_sigaddr[i].lastlr = lr;
        return;
    }
}
/* ---------- round9 观测（条件变量 SVC）：svc 0x1C/0x1D per-key 计数表（照上方 sigaddr 范式：
   {key,n1c,n1d} 前 16 槽 + 溢出桶，dump 时打）。0x1C 记等待次数、0x1D 记 signal 次数与首位调用者
   句柄（释放者画像）；唤醒总数 g_n1d_woke 对照 total（total≫woke=发信号没人等）。dump 侧连同
   该 key 当前等待者数一并打（现场数 g_wait 的 wkind==2）——等待有没有配对的 signal 一表可读。
   两个 svc case 内全程持 G 更新，dump 尾部无锁读（与 sigaddr 同级取证风险）。 */
#define PKEY_TOP 64   /* round9b：16 槽→49% 的 0x1D 落溢出桶；round8b：32 槽仍溢出 8.79M（≈2.05× 乒乓周期率=主线程每周期多打 2 发、活锁期才首见的第 33+ key）——64 槽把空打主力 key 的值/调用者钉死 */
static struct { uint64_t key, n1c, n1d, n1dk; uint32_t caller; } g_pkey[PKEY_TOP];   /* n1dk=该 key 上实际转醒数（round8b：空打 key 的 woke 恒 0 一列可读） */
static unsigned g_pkey_n;
static uint64_t g_ovf1c, g_ovf1d;
static _Atomic uint64_t g_n1c, g_n1d, g_n1d_woke;
/* round8c 实测：boot 期一次性 key（sig=1 族）把 64 槽主表吃光，活锁期主力空打 key（7.3M 次）
   首见时已无槽→落溢出桶无身份。次级表专收「主表满后首见」的 key——活锁期 key 全部落这里，身份可读。 */
static struct { uint64_t key, n1c, n1d, n1dk; uint32_t caller; } g_pkey2[16];
static unsigned g_pkey2_n;
/* round9b 第三表（E4 族大小确认）：pkey2 也满后首见的 key——溢出桶只有总数没有身份，
   64+16+32=112 个 key 以内族成员全可读，仍溢出才落回纯计数桶。 */
#define PKEY3_TOP 32
static struct { uint64_t key, n1c, n1d, n1dk; uint32_t caller; } g_pkey3[PKEY3_TOP];
static unsigned g_pkey3_n;
/* ---------- M5k: memory forensics for the object the main thread blocks on ----------
   Read-only probes, default ON (SELinux: the app sandbox cannot create new switch files,
   so a compile-time default is the only way to enable a new probe).
   Captured at the SVC sites, printed by runtime_m5k_dump() into vk_boot.log:
     - last 0x1C (key, lock, guest LR) of h=0x1001 and a 4-entry ring of its waits,
     - per-key 0x1C/0x1D counters already kept by pkey (row for that key is printed),
     - 0x34/0x1A counters of main (spin or block), last 0x34 addr/type/want/actual,
     - a 0x200-byte window around the key, snapshotted at the first dump and diffed later,
     - pointers into that window (who owns the object). */
static uint64_t g_m5k_key, g_m5k_lock, g_m5k_lr1c;
static uint64_t g_m5k_n1c, g_m5k_n1d, g_m5k_sig_h, g_m5k_sig_lr;
static uint64_t g_m5k_c34, g_m5k_c34_imm, g_m5k_c34_blk, g_m5k_last34, g_m5k_last34v;
static int g_m5k_last34t, g_m5k_last34want;
static uint64_t g_m5k_c1a, g_m5k_c1a_blk;
static uint64_t g_m5k_ring[4][4];   /* {key, addr, result, waited_ns} */
static unsigned g_m5k_ri;
static int g_m5k_dumps;
static uint64_t g_m5k_snap_key, g_m5k_snap[64];
static int g_m5k_snap_ok;
/* ---------- M5l：探针总开关 + 安全读 + [fx]/[sy] 两段取证 ----------
   smo_forensic：C 默认 1（SELinux 下 app 沙箱不能新建开关文件，新开关只能给编译期默认值）。
   只在 dump 里读，热路径零成本；0 = 只留 [m5k] 骨架、不做对象窗口/账本转储。
   安全读一律走 loader.c 的 runtime_safe_read（/proc/self/maps 闸门 + runtime_fault_guard
   的 SIGSEGV 兜底）——m5k 两轮都死在「拿 guest 指针直接 memcpy」，这条路径不许再裸读。 */
int smo_forensic = 1;
static int fx_read64(uint64_t a, uint64_t *out) {
    if (!out) return 0;
    *out = 0;
    return runtime_safe_read(out, a, 8);
}
/* ---------- M6j: [sy] keysig —— 0x1D 的「谁发的」归属（线程句柄 + 客体 LR + 末次 dump 号） ----------
   动机（M6j 派活单 B）：key 0x6e2a5c6368 的 1145 次 signal 究竟来自哪个线程、末次在何时。pkey 三表
   只记「首个 caller 句柄」，答不出同一 key 的多个发信号者。这里独立一张 64 槽表，与 pkey 三表
   完全不共享槽位/行为（不改任何既有账本口径）：
     sig    = 该 key 的 0x1D 总数（含下面 top3 之外的发信号者）
     s[k]   = 前 3 个 (线程句柄, 客体 LR) 的精确计数（第 4 个及以后只进 other）
     lastk  = 末次 0x1D 时本账本的 dump 序号（不是时间戳：见下）
   热路径铁律（M6j 第一次 boot 实测）：svc 0x1D 跑在客体自己的栈上，某些线程到这儿时栈只剩
   几十字节 —— 上一次给这里加了 now_ns() 就在 13 s 内 SIGSEGV(SEGV_ACCERR)@sp-8 崩掉
   （cppcrash t=13s, runtime_now_ns+32）。所以本表热路径只允许「纯算 + 纯存」：无 now_ns、
   无 I/O、无 memset 大对象。last 时刻改用 dump 序号（dump 侧换算成「约 N 秒前」）。
   dump：按 sig 取前 8 打 [sy] keysig 行，并带当前 0x1C 等待者数（g_wait 里 wkind==2 + wkey）。
   读侧与既有 dump 同级：表在 G 下更新、dump 时无锁读。 */
#define KS_TOP 64
#define KS_SIG 3
typedef struct { uint32_t h; uint64_t lr, n; } ks_sig;
static struct { uint64_t key, sig, other; unsigned lastk; ks_sig s[KS_SIG]; } g_ks[KS_TOP];
static unsigned g_ks_n;
static uint64_t g_ks_ovf;
static unsigned g_ks_dumpno;   /* 每次 dump 由 fx_keysig_dump 自增；ks_note 只读 */
static void ks_note(uint64_t key, uint32_t h, uint64_t lr) {   // 调用方持 G（svc 0x1D case 内）；纯算+纯存
    unsigned i;
    for (i = 0; i < g_ks_n; i++) if (g_ks[i].key == key) break;
    if (i == g_ks_n) {
        if (g_ks_n >= KS_TOP) { g_ks_ovf++; return; }   // 64 个 distinct key 之外：只留总数
        g_ks[i].key = key; g_ks[i].sig = 0; g_ks[i].other = 0; g_ks[i].lastk = 0;
        for (int k = 0; k < KS_SIG; k++) { g_ks[i].s[k].h = 0; g_ks[i].s[k].lr = 0; g_ks[i].s[k].n = 0; }
        g_ks_n++;
    }
    g_ks[i].sig++; g_ks[i].lastk = g_ks_dumpno;
    for (int k = 0; k < KS_SIG; k++)
        if (g_ks[i].s[k].n && g_ks[i].s[k].h == h && g_ks[i].s[k].lr == lr) { g_ks[i].s[k].n++; return; }
    for (int k = 0; k < KS_SIG; k++)
        if (!g_ks[i].s[k].n) { g_ks[i].s[k].h = h; g_ks[i].s[k].lr = lr; g_ks[i].s[k].n = 1; return; }
    g_ks[i].other++;
}
extern void runtime_describe_address(uint64_t a, char *out, size_t n);
static void fx_keysig_dump(void) {
    extern void vk_boot_logf(const char *fmt, ...);
    unsigned dno_now = ++g_ks_dumpno;
    if (!g_ks_n) { if (!g_ks_ovf) vk_boot_logf("[sy] keysig: no 0x1D signal recorded yet"); return; }
    /* 等待者快照：trylock G 拷一份 (key,count)，避免在日志里持锁 */
    struct { uint64_t key; int n; } wt[8]; int nwt = 0;
    int locked = 0; for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    if (locked) {
        for (unsigned k = 0; k < g_ks_n && nwt < 8; k++) {
            int n = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == g_ks[k].key) n++;
            if (!n) continue;
            int dup = 0; for (int j = 0; j < nwt; j++) if (wt[j].key == g_ks[k].key) { dup = 1; break; }
            if (!dup) { wt[nwt].key = g_ks[k].key; wt[nwt].n = n; nwt++; }
        }
        pthread_mutex_unlock(&G);
    }
    unsigned used[KS_TOP]; memset(used, 0, sizeof used);
    for (int r = 0; r < 8; r++) {   // 前 8 大 key（选择排序；<=64 槽）
        int best = -1;
        for (unsigned i = 0; i < g_ks_n; i++) {
            if (used[i]) continue;
            if (best < 0 || g_ks[i].sig > g_ks[best].sig) best = (int)i;
        }
        if (best < 0) break;
        used[best] = 1;
        int nw = 0; for (int j = 0; j < nwt; j++) if (wt[j].key == g_ks[best].key) { nw = wt[j].n; break; }
        unsigned ago = dno_now > g_ks[best].lastk ? dno_now - g_ks[best].lastk : 0;
        char b[512]; int o = 0;
        o += snprintf(b + o, sizeof b - (size_t)o, "[sy] keysig key=%#llx signals=%llu pending=%d last_t=~%us(%ud dumps ago)",
                      (unsigned long long)g_ks[best].key, (unsigned long long)g_ks[best].sig, nw, ago * 30u, ago);
        for (int k = 0; k < KS_SIG && o < (int)sizeof b - 96; k++) {
            if (!g_ks[best].s[k].n) continue;
            char lr[64]; lr[0] = 0;
            runtime_describe_address(g_ks[best].s[k].lr, lr, sizeof lr);
            o += snprintf(b + o, sizeof b - (size_t)o, " by%d=h%#x/%s n=%llu", k,
                          g_ks[best].s[k].h, lr[0] ? lr : "-", (unsigned long long)g_ks[best].s[k].n);
        }
        if (g_ks[best].other && o < (int)sizeof b - 40)
            o += snprintf(b + o, sizeof b - (size_t)o, " other=%llu", (unsigned long long)g_ks[best].other);
        b[(o < (int)sizeof b) ? o : (int)sizeof b - 1] = 0;
        vk_boot_logf("%s", b);
    }
    if (g_ks_ovf) vk_boot_logf("[sy] keysig overflow: %llu signal(s) on keys beyond slot %d",
                               (unsigned long long)g_ks_ovf, KS_TOP);
    /* M6j 附带（零运行时成本：数据本来就在 g_sigaddr 里）：地址族 0x34/0x35 的释放者归属。
       main 卡在 0x6ea8bf3ca8 type=2 want=1 value=1（[m5k] ledger），这里给该地址与全表前 8 的
       wait/sig/woke/zero + 首个与末个 0x35 调用者（线程句柄 + 客体 LR）。sig 不涨 = 释放者没跑到；
       woke=0 而 sig>0 = 发了信号没人接（丢失唤醒嫌疑）。 */
    {
        char lr[64];
        int done[8]; for (int j = 0; j < 8; j++) done[j] = -1;
        for (int k = 0; k < 8; k++) {
            int best = -1;
            for (unsigned i = 0; i < g_sigaddr_n; i++) {
                int used = 0;
                for (int j = 0; j < k; j++) if (done[j] == (int)i) { used = 1; break; }
                if (used) continue;
                if (best < 0 || g_sigaddr[i].n35 > g_sigaddr[best].n35 ||
                    (g_sigaddr[i].n35 == g_sigaddr[best].n35 && g_sigaddr[i].n34 > g_sigaddr[best].n34)) best = (int)i;
            }
            if (best < 0) break;
            done[k] = best;
            lr[0] = 0; if (g_sigaddr[best].lastlr) runtime_describe_address(g_sigaddr[best].lastlr, lr, sizeof lr);
            vk_boot_logf("[sy] addrsig addr=%#llx wait=%llu sig=%llu woke=%llu zero=%llu first=h%#x last=h%#x/%s",
                         (unsigned long long)g_sigaddr[best].addr, (unsigned long long)g_sigaddr[best].n34,
                         (unsigned long long)g_sigaddr[best].n35, (unsigned long long)g_sigaddr[best].wake,
                         (unsigned long long)g_sigaddr[best].zero, g_sigaddr[best].caller,
                         g_sigaddr[best].lastcaller, lr[0] ? lr : "-");
        }
        if (g_m5k_last34) {   /* the address main is waiting on right now (0x34 type 2) */
            int f = -1;
            for (unsigned i = 0; i < g_sigaddr_n; i++) if (g_sigaddr[i].addr == g_m5k_last34) { f = (int)i; break; }
            lr[0] = 0; if (f >= 0 && g_sigaddr[f].lastlr) runtime_describe_address(g_sigaddr[f].lastlr, lr, sizeof lr);
            if (f >= 0)
                vk_boot_logf("[sy] addrsig main34 addr=%#llx wait=%llu sig=%llu woke=%llu zero=%llu first=h%#x last=h%#x/%s type=%d want=%d seen=%#x",
                             (unsigned long long)g_sigaddr[f].addr, (unsigned long long)g_sigaddr[f].n34,
                             (unsigned long long)g_sigaddr[f].n35, (unsigned long long)g_sigaddr[f].wake,
                             (unsigned long long)g_sigaddr[f].zero, g_sigaddr[f].caller, g_sigaddr[f].lastcaller,
                             lr[0] ? lr : "-", g_m5k_last34t, g_m5k_last34want, (unsigned)g_m5k_last34v);
            else
                vk_boot_logf("[sy] addrsig main34 addr=%#llx NOT in g_sigaddr table (slot overflow) type=%d want=%d seen=%#x",
                             (unsigned long long)g_m5k_last34, g_m5k_last34t, g_m5k_last34want, (unsigned)g_m5k_last34v);
        }
    }
}
/* [sy] 同步账本：与上次 dump 的对比（两个时间点之间的增量）。 */
typedef struct { uint64_t id, w, s; } fx_row;
static fx_row g_sy_prev_key[16], g_sy_prev_h[16];
static int g_sy_prev_key_n, g_sy_prev_h_n;
static void fx_sync_ledger(int dno) {
    extern void vk_boot_logf(const char *fmt, ...);
    fx_row top[10];
    int ntop = 0;
    /* 1) 条件键族（0x1C 等 / 0x1D 发）+ 地址族（0x34 等 / 0x35 发）：合池后按 waits 取前 10。 */
    {
        static fx_row all[96];
        int n = 0;
        for (int t = 0; t < 3 && n < 48; t++) {
            unsigned cnt = t == 0 ? g_pkey_n : t == 1 ? g_pkey2_n : g_pkey3_n;
            for (unsigned i = 0; i < cnt && n < 48; i++) {
                all[n].id = t == 0 ? g_pkey[i].key : t == 1 ? g_pkey2[i].key : g_pkey3[i].key;
                all[n].w  = t == 0 ? g_pkey[i].n1c : t == 1 ? g_pkey2[i].n1c : g_pkey3[i].n1c;
                all[n].s  = t == 0 ? g_pkey[i].n1d : t == 1 ? g_pkey2[i].n1d : g_pkey3[i].n1d;
                n++;
            }
        }
        for (unsigned i = 0; i < g_sigaddr_n && n < 80; i++) { all[n].id = g_sigaddr[i].addr; all[n].w = g_sigaddr[i].n34; all[n].s = g_sigaddr[i].n35; n++; }
        for (int k = 0; k < 10; k++) {   // 选择排序取前 10（n<=80，代价可忽略；不持锁读，同既有 dump 约定）
            int best = -1;
            for (int i = 0; i < n; i++) {
                int used = 0; for (int j = 0; j < ntop; j++) if (top[j].id == all[i].id) { used = 1; break; }
                if (used) continue;
                if (best < 0 || all[i].w > all[best].w) best = i;
            }
            if (best < 0 || !all[best].w) break;
            top[ntop++] = all[best];
        }
        for (int i = 0; i < ntop; i++) {
            uint64_t pw = 0, ps = 0; int had = 0;
            for (int j = 0; j < g_sy_prev_key_n; j++) if (g_sy_prev_key[j].id == top[i].id) { pw = g_sy_prev_key[j].w; ps = g_sy_prev_key[j].s; had = 1; break; }
            if (had) vk_boot_logf("[sy] key=%#llx waits=%llu signals=%llu (+%llu/+%llu since dump#%d)%s",
                                  (unsigned long long)top[i].id, (unsigned long long)top[i].w, (unsigned long long)top[i].s,
                                  (unsigned long long)(top[i].w - pw), (unsigned long long)(top[i].s - ps), dno - 1,
                                  (top[i].w > pw && top[i].s == ps) ? "  <== waits grow, signals flat" : "");
            else vk_boot_logf("[sy] key=%#llx waits=%llu signals=%llu (first seen in this ledger)",
                              (unsigned long long)top[i].id, (unsigned long long)top[i].w, (unsigned long long)top[i].s);
        }
        g_sy_prev_key_n = ntop; for (int i = 0; i < ntop; i++) g_sy_prev_key[i] = top[i];
    }
    /* 2) 句柄族（0x18 等 / 0x11 发）：扫 MAX_H 句柄槽，按 waits 取前 10。 */
    {
        memset(top, 0, sizeof top); ntop = 0;
        for (int k = 0; k < 10; k++) {
            int best = -1; uint64_t bw = 0;
            for (unsigned i = 0; i < MAX_H; i++) {
                uint64_t w = atomic_load_explicit(&g_syhw[i], memory_order_relaxed);
                if (!w || w <= bw) continue;
                int used = 0; for (int j = 0; j < ntop; j++) if (top[j].id == (uint64_t)i + 0x1000u) { used = 1; break; }
                if (used) continue;
                best = (int)i; bw = w;
            }
            if (best < 0) break;
            top[ntop].id = (uint64_t)best + 0x1000u;
            top[ntop].w = bw;
            top[ntop].s = atomic_load_explicit(&g_syhs[best], memory_order_relaxed);
            ntop++;
        }
        for (int i = 0; i < ntop; i++) {
            uint64_t pw = 0, ps = 0; int had = 0;
            for (int j = 0; j < g_sy_prev_h_n; j++) if (g_sy_prev_h[j].id == top[i].id) { pw = g_sy_prev_h[j].w; ps = g_sy_prev_h[j].s; had = 1; break; }
            if (had) vk_boot_logf("[sy] h=%#llx waits=%llu signals=%llu (+%llu/+%llu since dump#%d)%s",
                                  (unsigned long long)top[i].id, (unsigned long long)top[i].w, (unsigned long long)top[i].s,
                                  (unsigned long long)(top[i].w - pw), (unsigned long long)(top[i].s - ps), dno - 1,
                                  (top[i].w > pw && top[i].s == ps) ? "  <== waits grow, signals flat" : "");
            else vk_boot_logf("[sy] h=%#llx waits=%llu signals=%llu (first seen in this ledger)",
                              (unsigned long long)top[i].id, (unsigned long long)top[i].w, (unsigned long long)top[i].s);
        }
        g_sy_prev_h_n = ntop; for (int i = 0; i < ntop; i++) g_sy_prev_h[i] = top[i];
    }
    vk_boot_logf("[sy] ledger totals waitaddr(0x34)=%llu wakeaddr(0x35)=%llu woke(0x35)=%llu waitkey(0x1C)=%llu sigkey(0x1D)=%llu woke(0x1D)=%llu",
                 (unsigned long long)atomic_load(&g_n34), (unsigned long long)atomic_load(&g_n35),
                 (unsigned long long)atomic_load(&g_n35_woke), (unsigned long long)atomic_load(&g_n1c),
                 (unsigned long long)atomic_load(&g_n1d), (unsigned long long)atomic_load(&g_n1d_woke));
    fx_keysig_dump();   /* M6j: 0x1D 发信号者归属（线程+LR+末次时刻），独立表，不改上面任何口径 */
}
static void pkey_note(int is1d, uint64_t key, uint32_t caller) {   // 调用方持 G
    if (is1d) atomic_fetch_add_explicit(&g_n1d, 1, memory_order_relaxed);
    else      atomic_fetch_add_explicit(&g_n1c, 1, memory_order_relaxed);
    for (unsigned i = 0; i < g_pkey_n; i++) if (g_pkey[i].key == key) {
        if (is1d) { g_pkey[i].n1d++; if (!g_pkey[i].caller) g_pkey[i].caller = caller; }   // 首位 0x1D 调用者=发信号者画像
        else g_pkey[i].n1c++;
        return;
    }
    if (g_pkey_n < PKEY_TOP) {   // 首见 key 入主表（64 槽）
        g_pkey[g_pkey_n].key = key; g_pkey[g_pkey_n].n1c = !is1d; g_pkey[g_pkey_n].n1d = !!is1d;
        g_pkey[g_pkey_n].n1dk = 0; g_pkey[g_pkey_n].caller = is1d ? caller : 0; g_pkey_n++;
        return;
    }
    for (unsigned i = 0; i < g_pkey2_n; i++) if (g_pkey2[i].key == key) {   // 主表满后首见的 key：次级表
        if (is1d) { g_pkey2[i].n1d++; if (!g_pkey2[i].caller) g_pkey2[i].caller = caller; }
        else g_pkey2[i].n1c++;
        return;
    }
    if (g_pkey2_n < 16) {
        g_pkey2[g_pkey2_n].key = key; g_pkey2[g_pkey2_n].n1c = !is1d; g_pkey2[g_pkey2_n].n1d = !!is1d;
        g_pkey2[g_pkey2_n].n1dk = 0; g_pkey2[g_pkey2_n].caller = is1d ? caller : 0; g_pkey2_n++;
        return;
    }
    for (unsigned i = 0; i < g_pkey3_n; i++) if (g_pkey3[i].key == key) {
        if (is1d) { g_pkey3[i].n1d++; if (!g_pkey3[i].caller) g_pkey3[i].caller = caller; }
        else g_pkey3[i].n1c++;
        return;
    }
    if (g_pkey3_n < PKEY3_TOP) {
        g_pkey3[g_pkey3_n].key = key; g_pkey3[g_pkey3_n].n1c = !is1d; g_pkey3[g_pkey3_n].n1d = !!is1d;
        g_pkey3[g_pkey3_n].n1dk = 0; g_pkey3[g_pkey3_n].caller = is1d ? caller : 0; g_pkey3_n++;
        return;
    }
    if (is1d) g_ovf1d++; else g_ovf1c++;
}
/* round16b 早 boot 键全录：boot 后前 10 秒的 0x1D 目标键全量专用表（256 槽+溢出计数），
   与 pkey 三表完全独立（不挤槽、不改其行为——「全录不打扰」）。动机：round8c 实测 boot 期
   一次性 key（sig=1 族）把 64 槽主表吃光，早窗到底有多少 distinct key、哪些是活锁期空打
   key 的前身，主表结构里读不出；本表按窗口收录，dump 单独一段。窗口起点=libgame 装载
   （dump_init 构造器），首个 0x1D 早于构造器打点时兜底以首见时刻为 t0。窗口过期后
   early1d_closed 置位，热路径零额外开销（一次 flag 读）。持 G 更新，dump 无锁读（同 pkey）。
   round20 最小扰动构建：整段随 SMO_OBS_HEAVY=0 编译关闭——该账本在 svc 0x1D 持全局锁 G
   的临界区内做 256 槽线性扫+now_ns()，boot 10s 窗口内每次 0x1D 都付此代价且阻塞其它
   svc 线程，是宿主侧 boot 扰动最大源；注册时序假说要求先剥掉它。恢复观测改回 1 即可。 */
#if SMO_OBS_HEAVY
#define EARLY1D_TOP 256
static struct { uint64_t key; uint64_t n; uint32_t caller; } g_early1d[EARLY1D_TOP];
static unsigned g_early1d_n; static uint64_t g_early1d_ovf;
static uint64_t g_early1d_t0; static int g_early1d_t0_set, g_early1d_closed;
static void early1d_note(uint64_t key, uint32_t caller) {   // 调用方持 G（svc 0x1D case 内）
    if (g_early1d_closed) return;
    uint64_t t = now_ns();
    if (!g_early1d_t0_set) { g_early1d_t0 = t; g_early1d_t0_set = 1; }   // 兜底（构造器先到则不触发）
    if (t - g_early1d_t0 >= 10000000000ull) { g_early1d_closed = 1; return; }   // 前 10s 窗口关闭
    for (unsigned i = 0; i < g_early1d_n; i++) if (g_early1d[i].key == key) { g_early1d[i].n++; return; }
    if (g_early1d_n < EARLY1D_TOP) {
        g_early1d[g_early1d_n].key = key; g_early1d[g_early1d_n].n = 1;
        g_early1d[g_early1d_n].caller = caller; g_early1d_n++;
        return;
    }
    g_early1d_ovf++;   // 10s 内 distinct key 超 256：计数留痕（本身即发现）
}
#endif
/* ---------- round9b CreateThread/StartThread 全量账本（E2 宿主侧排查项）----------
   修复方案定的宿主侧唯一排查项：核对 CreateThread/StartThread 门的全量账本（每个线程的
   entry+arg+返回码），确认派发池消费者线程是「从未创建」而非「创建失败」。热路径零 I/O
   （svc 0x08 成功路径本就持 G 登记；拒绝路径只加计数+末次样本），dump 时打全表与
   线程名册（dump 逐线程行已带 entry/arg）对 diff。 */
#define CT_LEDGER_CAP 64
static struct { uint64_t entry, arg, stack_top; int prio, core; uint32_t handle; int started; uint32_t ret; } g_ct[CT_LEDGER_CAP];
static unsigned g_ct_n;
static uint64_t g_ct_calls, g_ct_rejects, g_ct_start_calls, g_ct_start_bad;
static struct { uint64_t entry, arg; int prio, core; uint32_t ret; } g_ct_last_reject;
static void ct_note_create(uint64_t entry, uint64_t arg, uint64_t sp, int prio, int core, uint32_t handle, uint32_t ret) {   // 调用方持 G（成功路径）
    g_ct_calls++;
    unsigned i = g_ct_n < CT_LEDGER_CAP ? g_ct_n++ : (unsigned)-1;
    if (i == (unsigned)-1) return;   // 表满：只留总数（>64 个客体线程本就异常）
    g_ct[i].entry = entry; g_ct[i].arg = arg; g_ct[i].stack_top = sp;
    g_ct[i].prio = prio; g_ct[i].core = core; g_ct[i].handle = handle; g_ct[i].ret = ret; g_ct[i].started = 0;
}
static void ct_note_reject(uint64_t entry, uint64_t arg, int prio, int core, uint32_t ret) {   // 拒绝路径（无 G）：计数+末次样本，竞态可接受（取证类）
    g_ct_calls++; g_ct_rejects++;
    g_ct_last_reject.entry = entry; g_ct_last_reject.arg = arg; g_ct_last_reject.prio = prio; g_ct_last_reject.core = core; g_ct_last_reject.ret = ret;
}
static void ct_note_start(uint32_t handle, uint32_t ret) {   // 调用方持 G
    g_ct_start_calls++;
    if (ret) { g_ct_start_bad++; return; }
    for (unsigned i = 0; i < g_ct_n; i++) if (g_ct[i].handle == handle) { g_ct[i].started = 1; return; }
}
/* ---------- M6h: event pair forensics (read-only; no I/O on the SVC path) ----------
   The task: find the WRITE end of the event that main polls with svc 0x17 on handle 0x1024, and say
   whether it was ever signalled. Three records, all plain stores (a torn read only spoils one line of
   a diagnostic dump -- same risk class as g_evcnt/evlog):
     g_evpair  : every CreateEvent / service-event creation, in order (rd, wr, creator, LR)
     g_evlast  : per handle, the last signal/reset time + caller (handle + LR)
     g_evclose : per handle, the last close time + LR (the write end dying is exactly what we need to date)
     g_evsig   : (handle, thread) -> signal count, so the top-3 signallers of one handle are nameable
   None of this changes any SVC semantics. */
#define EVPAIR_N 256
typedef struct { uint32_t rd, wr, by, src; uint64_t lr, ns; _Atomic uint32_t seq; } evpair_t;
static evpair_t g_evpair[EVPAIR_N];
static _Atomic unsigned g_evpair_n;
static uint64_t g_evlast_ns[MAX_H][2], g_evlast_lr[MAX_H][2];
static uint32_t g_evlast_thr[MAX_H][2];
static uint64_t g_evclose_ns[MAX_H], g_evclose_lr[MAX_H];
#define EVSIG_TOP 64
static struct { uint32_t h, thr; uint64_t n, last_lr, last_ns; } g_evsig[EVSIG_TOP];
static int g_evsig_n;
static uint32_t g_trk_rd = 0x1024u;   /* the handle under investigation (the task's target) */
static void evpair_note(uint32_t rd, uint32_t wr, uint32_t by, uint64_t lr, int src) {
    unsigned i = atomic_fetch_add_explicit(&g_evpair_n, 1, memory_order_relaxed);
    if (i >= EVPAIR_N) return;
    evpair_t *p = &g_evpair[i];
    p->seq = 0; p->rd = rd; p->wr = wr; p->by = by; p->src = src; p->lr = lr; p->ns = now_ns();
    atomic_store_explicit(&p->seq, 1, memory_order_release);
}
static void evsig_note(uint32_t h, uint32_t by, uint64_t lr) {
    for (int i = 0; i < g_evsig_n; i++) if (g_evsig[i].h == h && g_evsig[i].thr == by) {
        g_evsig[i].n++; g_evsig[i].last_lr = lr; g_evsig[i].last_ns = now_ns(); return; }
    if (g_evsig_n < EVSIG_TOP) { g_evsig[g_evsig_n].h = h; g_evsig[g_evsig_n].thr = by; g_evsig[g_evsig_n].n = 1;
                                 g_evsig[g_evsig_n].last_lr = lr; g_evsig[g_evsig_n].last_ns = now_ns(); g_evsig_n++; }
}
static void evclose_note(uint32_t h, uint64_t lr) {
    unsigned i = h - 0x1000;
    if (i > 0 && i < MAX_H) { g_evclose_ns[i] = now_ns(); g_evclose_lr[i] = lr; }
}
/* M6h: an IPC request (or a control command) carried an event handle. In Horizon a sent handle moves
   into the receiver's process; our emulated services do not implement that transfer, so a write end
   sent to a service is exactly the way a signal source can silently disappear. Recorded on the SVC
   path (no I/O) and printed by the 30 s dump. */
#define EVXFER_N 64
typedef struct { uint32_t h, dir, type, cmd; char sess[10]; uint64_t ns; _Atomic uint32_t seq; } evxfer_t;
static evxfer_t g_evxfer[EVXFER_N];
static _Atomic unsigned g_evxfer_n;
/* M6h-B: session closes, with the guest LR that issued CloseHandle (0x16). For the IAudioDevice session
   this says whether the GAME abandoned the device or our own bookkeeping killed it -- the difference
   between "the write end legitimately went away" and "we broke a live event". */
#define EVSCLOSE_N 32
typedef struct { char name[20]; uint32_t h, by; uint64_t lr, ns; _Atomic uint32_t seq; } evsclose_t;
static evsclose_t g_evsclose[EVSCLOSE_N];
static _Atomic unsigned g_evsclose_n;
uint64_t g_svc_close_lr;          /* set by case 0x16 right before the session/domain teardown runs */
void runtime_evsclose_note(const char *name, uint32_t h) {
    unsigned i = atomic_fetch_add_explicit(&g_evsclose_n, 1, memory_order_relaxed);
    if (i >= EVSCLOSE_N) return;
    evsclose_t *x = &g_evsclose[i];
    thr_t *me = g_keythr ? pthread_getspecific(g_keythr) : NULL;
    x->seq = 0; x->h = h; x->by = me ? me->handle : 0; x->lr = g_svc_close_lr; x->ns = now_ns();
    snprintf(x->name, sizeof x->name, "%s", name ? name : "?");
    atomic_store_explicit(&x->seq, 1, memory_order_release);
}
static void evxfer_put(uint32_t h, int dir, int type, uint32_t cmd, const char *sess) {
    unsigned i = atomic_fetch_add_explicit(&g_evxfer_n, 1, memory_order_relaxed);
    if (i >= EVXFER_N) return;
    evxfer_t *x = &g_evxfer[i];
    x->seq = 0; x->h = h; x->dir = dir; x->type = type; x->cmd = cmd; x->ns = now_ns();
    snprintf(x->sess, sizeof x->sess, "%s", sess ? sess : "?");   /* snprintf into a local array: no I/O */
    atomic_store_explicit(&x->seq, 1, memory_order_release);
}
void runtime_evxfer_note(const char *sess, uint32_t cmd, const uint32_t *copy, unsigned ncopy,
                         const uint32_t *move, unsigned nmove) {
    int locked = 0;
    for (int i = 0; i < 50 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(500);
    if (locked) {
        for (unsigned i = 0; i < ncopy && i < 8; i++) { int ty = 0; unsigned idx = copy[i] - 0x1000;
            if (idx > 0 && idx < MAX_H && (g_h[idx].type == O_EVENT_W || g_h[idx].type == O_EVENT_R)) ty = g_h[idx].type;
            if (ty) evxfer_put(copy[i], 0, ty, cmd, sess); }
        for (unsigned i = 0; i < nmove && i < 8; i++) { int ty = 0; unsigned idx = move[i] - 0x1000;
            if (idx > 0 && idx < MAX_H && (g_h[idx].type == O_EVENT_W || g_h[idx].type == O_EVENT_R)) ty = g_h[idx].type;
            if (ty) evxfer_put(move[i], 1, ty, cmd, sess); }
        pthread_mutex_unlock(&G);
    }
}
static void evlog(uint32_t op, uint32_t h, uint32_t h2, uint64_t lr) {
    /* round6 账本 + round8 归因：per-handle signal/reset 累计与首见裸 LR（0x1024 的 signal 源=宿主构造侧、
       0x1026 的 reset 源=SDK svc 包装层 LR，分钟 dump 的 evc who= 单列给出）。计数/首见先于环写去重——
       洪流条目照样入账，只是不再占环。首见捕获用 CAS(0→lr)，sigaddr_note caller 范式的原子版。 */
    if (op == EVLOG_SIGNAL || op == EVLOG_RESET) {
        unsigned hi = h - 0x1000;
        if (hi > 0 && hi < MAX_H) {   // 无符号回绕的坏句柄自然被挡
            int k = (op == EVLOG_RESET);
            atomic_fetch_add_explicit(&g_evcnt[hi][op == EVLOG_RESET], 1, memory_order_relaxed);
            uint64_t first = 0;
            atomic_compare_exchange_strong_explicit(&g_evfirst[hi][op == EVLOG_RESET], &first, lr, memory_order_relaxed, memory_order_relaxed);
            /* M6h: last-actor ledger. The caller's thread handle comes from TLS (0 = host-side call, i.e. a
               runtime_event_signal from a host thread), which is what makes "signalled by the guest" vs
               "signalled by our own HLE" tellable apart. Plain stores, no lock (see the block comment). */
            { thr_t *me = g_keythr ? pthread_getspecific(g_keythr) : NULL;
              g_evlast_ns[hi][k] = now_ns(); g_evlast_lr[hi][k] = lr; g_evlast_thr[hi][k] = me ? me->handle : 0;
              if (op == EVLOG_SIGNAL) evsig_note(h, me ? me->handle : 0, lr); }
        }
    }
    /* round8 环写去重：连续同 (op,handle,lr) 只记账不占环——0x1026（sdk+0xd3f084 每 5ms 一轮 reset）
       不再淹没 64 槽回放窗口，dump 环留给其它句柄/操作的最近时序；竞态可接受（本函数既往即无锁写）。 */
    static uint32_t p_op, p_h; static uint64_t p_lr;
    if (op == p_op && h == p_h && lr == p_lr) return;
    p_op = op; p_h = h; p_lr = lr;
    evlog_t *e = &g_evlog[g_evlog_head]; e->op = op; e->handle = h; e->extra = h2; e->lr = lr;
    g_evlog_head = (g_evlog_head + 1) & 63; g_evlog_n++;
}

/* ---------- M6g: what the main (loading) thread 0x1001 does right after each wake ----------
   The task asks one question: which condition is never satisfied. The measurement is a per-wake
   window: each wake of guest thread 0x1001 opens a window, the first 20 SVCs of it are recorded
   verbatim (number, a0..a3, SVC return address, return values), and the window is closed at the
   next wake with a category histogram of everything it did in between (so the summary is not
   limited to the 20 sampled steps).
   Everything here runs on the guest SVC path, i.e. on the guest stack (see the 0x34 case note and
   round3's SEGV_ACCERR from an fprintf on this path). Therefore: no libc I/O and no snprintf in
   these hooks. Entries go into a fixed static ring with one atomic write cursor; the 30 s
   heartbeat (runtime_load_dump) formats and writes them from its own thread via vk_boot_logf.
   Read-only w.r.t. thread state; a full ring only increments drop. */
#define LDR_RING 16384u
#define LDR_STEPS 20u
typedef struct {
    uint64_t ns, lr, a0, a1, a2, a3, ret, ret2;
    uint32_t num, kind, wake;
    _Atomic uint32_t seq;
} ldr_ent_t;
#define LDR_K_STEP 0   /* num=SVC number, a0..a3=r[0..3], lr=SVC return address, ret/ret2=r[0]/r[1] after the switch */
#define LDR_K_PRE  1   /* before main blocks on 0x34: num=type|(rdfail<<8), ret=addr, ret2=wanted value, lr=current value, a0..a3=4 words */
#define LDR_K_POST 2   /* at the instant of the wake: num=signaller handle|(rdfail<<16), ret=addr, lr=signaller LR, a0..a3=4 words */
#define LDR_K_LOOP 3   /* window summary: num=total SVC, a0/a1=category counts (4x16 per word), a2/a3=top-2 SVC, ret=duration ns, ret2=distinct count */
#define LDR_K_WOKE 4   /* wake-time snapshot: who released main and what the object held then (standalone, not paired) */
static ldr_ent_t g_ldr_ring[LDR_RING];
static _Atomic unsigned g_ldr_head, g_ldr_drop, g_ldr_wakes, g_ldr_caps, g_ldr_cap_left, g_ldr_wins;
static uint16_t g_ldr_cat[8];          /* 0 ipc, 1 wait_handle, 2 wait_addr, 3 wait_cond, 4 events, 5 threads, 6 memory, 7 other */
static uint32_t g_ldr_svc[256];
static uint64_t g_ldr_win_ns;          /* 0 = no window open yet */
/* M6g: main polls svc 0x17 (ResetSignal) on handle 0x1024 in every window. Record what that slot
   actually is (thread / event read end / event write end) and its state, so the report can say which
   "not satisfied" 0xFA01 means. Read-only, no allocation. */
static int g_p1024_kind = -1, g_p1024_sig = -1; static unsigned g_p1024_n;
static uint64_t g_ldr_win_svc;
static unsigned g_ldr_win_wake;
static int dump_read_u32(uint64_t a, uint32_t *out);   /* mapping-checked guest read, defined further below */

static ldr_ent_t *ldr_put(unsigned kind, unsigned num) {
    unsigned i = atomic_fetch_add_explicit(&g_ldr_head, 1, memory_order_relaxed);
    if (i >= LDR_RING) { atomic_fetch_add_explicit(&g_ldr_drop, 1, memory_order_relaxed); return NULL; }
    ldr_ent_t *e = &g_ldr_ring[i];
    e->seq = 0;
    e->kind = kind; e->num = num; e->ns = now_ns();
    e->wake = atomic_load_explicit(&g_ldr_wakes, memory_order_relaxed);
    e->lr = 0; e->a0 = e->a1 = e->a2 = e->a3 = 0; e->ret = e->ret2 = 0;
    return e;
}
static void ldr_pub(ldr_ent_t *e) { if (e) atomic_store_explicit(&e->seq, 1, memory_order_release); }

static int ldr_cat(unsigned num) {
    switch (num) {
    case 0x1f: case 0x21: case 0x22: return 0;                        /* IPC / service calls (file reads go through here) */
    case 0x18: return 1;                                              /* WaitSynchronization (event handle) */
    case 0x34: return 2;                                              /* WaitForAddress (barrier) */
    case 0x1c: return 3;                                              /* WaitProcessWideKey (condition variable) */
    case 0x11: case 0x12: case 0x17: case 0x1d: case 0x45: return 4;  /* events / signals */
    case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d:
    case 0x0e: case 0x0f: case 0x10: case 0x16: case 0x19: return 5;  /* thread / handle management */
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x29: return 6;                                              /* memory info / permissions */
    default: return 7;
    }
}
/* one SVC of main inside an open window: histogram only (the sampled ring entries are separate) */
static void ldr_win_count(unsigned num) {
    if (!g_ldr_win_ns) return;
    unsigned c = (unsigned)ldr_cat(num);
    if (g_ldr_cat[c] != 0xffffu) g_ldr_cat[c]++;
    g_ldr_svc[num & 0xffu]++;
    g_ldr_win_svc++;
}
static void ldr_window_close(void) {
    if (!g_ldr_win_ns) return;
    ldr_ent_t *e = ldr_put(LDR_K_LOOP, (unsigned)g_ldr_win_svc);
    if (e) {
        int b1 = -1, b2 = -1; unsigned nd = 0;
        for (int i = 0; i < 256; i++) {
            if (!g_ldr_svc[i]) continue;
            nd++;
            if (b1 < 0 || g_ldr_svc[i] > g_ldr_svc[b1]) { b2 = b1; b1 = i; }
            else if (b2 < 0 || g_ldr_svc[i] > g_ldr_svc[b2]) b2 = i;
        }
        e->a0 = (uint64_t)g_ldr_cat[0] | ((uint64_t)g_ldr_cat[1] << 16) | ((uint64_t)g_ldr_cat[2] << 32) | ((uint64_t)g_ldr_cat[3] << 48);
        e->a1 = (uint64_t)g_ldr_cat[4] | ((uint64_t)g_ldr_cat[5] << 16) | ((uint64_t)g_ldr_cat[6] << 32) | ((uint64_t)g_ldr_cat[7] << 48);
        if (b1 >= 0) e->a2 = ((uint64_t)b1 << 32) | g_ldr_svc[b1];
        if (b2 >= 0) e->a3 = ((uint64_t)b2 << 32) | g_ldr_svc[b2];
        e->ret = now_ns() - g_ldr_win_ns;
        e->ret2 = nd;
        e->wake = g_ldr_win_wake;
        ldr_pub(e);
    }
    atomic_fetch_add_explicit(&g_ldr_wins, 1, memory_order_relaxed);
    memset(g_ldr_cat, 0, sizeof g_ldr_cat);
    memset(g_ldr_svc, 0, sizeof g_ldr_svc);
    g_ldr_win_svc = 0;
}
static void ldr_obj_words(uint64_t addr, ldr_ent_t *e, unsigned *fails) {
    uint32_t w[4] = {0, 0, 0, 0}; unsigned f = 0;
    for (int k = 0; k < 4; k++) if (!dump_read_u32(addr + 4u * (uint64_t)k, &w[k])) f++;
    e->a0 = w[0]; e->a1 = w[1]; e->a2 = w[2]; e->a3 = w[3];
    *fails = f;
}
/* called from wake() before the thread is unlinked, so waddr/wkey/wkind still describe the wait */
static void ldr_note_wake(thr_t *t) {
    if (t->handle != 0x1001u) return;
    unsigned w = atomic_fetch_add_explicit(&g_ldr_wakes, 1, memory_order_relaxed) + 1u;
    if (t->waddr) {
        ldr_ent_t *e = ldr_put(LDR_K_WOKE, 0x1001u);
        if (e) {
            unsigned f = 0; ldr_obj_words(t->waddr, e, &f);
            e->ret = t->waddr; e->lr = t->last_lr;
            e->num = 0x1001u | (f << 16);   /* safe-read failures in the high half: 0..4 */
            e->wake = w;
            ldr_pub(e);
        }
    }
    ldr_window_close();
    g_ldr_win_wake = w;
    g_ldr_win_ns = now_ns();   /* open the next window here: ldr_window_close only closes */
    /* N=1 for the first 30 wakes, then every 10th; 400 captures is a hard log bound */
    if (w <= 30u || (w % 10u) == 0u) {
        unsigned c = atomic_load_explicit(&g_ldr_caps, memory_order_relaxed);
        if (c < 400u) { atomic_fetch_add_explicit(&g_ldr_caps, 1, memory_order_relaxed); atomic_store_explicit(&g_ldr_cap_left, LDR_STEPS, memory_order_relaxed); }
    }
}
/* main just came back from block() on 0x34: snapshot the object after the wait (always pairs with PRE) */
static void ldr_obj_post(uint64_t addr, int64_t res) {
    ldr_ent_t *e = ldr_put(LDR_K_POST, 0x1001u);
    if (!e) return;
    unsigned f = 0;
    ldr_obj_words(addr, e, &f);
    e->ret = addr; e->ret2 = (uint64_t)res;
    e->num = 0x1001u | (f << 16);
    ldr_pub(e);
}
/* main is about to block on 0x34: snapshot the object before the wait */
static void ldr_obj_pre(uint64_t addr, int type, int32_t val, int32_t cur) {
    ldr_ent_t *e = ldr_put(LDR_K_PRE, (unsigned)type);
    if (!e) return;
    unsigned f = 0;
    ldr_obj_words(addr, e, &f);
    e->num = (unsigned)type | (f << 8);
    e->ret = addr; e->ret2 = (uint64_t)(uint32_t)val; e->lr = (uint64_t)(uint32_t)cur;
    ldr_pub(e);
}
static int ldr_step_begin(unsigned num, uint64_t *regs) {
    ldr_ent_t *e = ldr_put(LDR_K_STEP, num);
    if (!e) return -1;
    e->lr = regs[30]; e->a0 = regs[0]; e->a1 = regs[1]; e->a2 = regs[2]; e->a3 = regs[3];
    atomic_fetch_sub_explicit(&g_ldr_cap_left, 1, memory_order_relaxed);
    return (int)(e - g_ldr_ring);
}
static void ldr_step_end(int slot, uint64_t *regs) {
    if (slot < 0) return;
    g_ldr_ring[slot].ret = regs[0]; g_ldr_ring[slot].ret2 = regs[1];
    ldr_pub(&g_ldr_ring[slot]);
}

/* ---------- M6k: thread 0x102f forensics (SVC trail / waits / signals / object words) ----------
   Task M6k-A. Facts from M6j: 0x102f is the first and last signaller of 0x6ea8bf3ca8 (the word main
   0x1001 blocks on with type=2 want=1), it signals with type 0 (wake only, never stores a new value),
   it is alive and running (wk=0, nsvc +39/30 s), and it also owns the sibling flag 0x6ec3d6e8b8 that
   already reads 2. This block answers "what is its unfinished task" with observations only:
     [t102f] k=.. svc=.. pc=..   every SVC of that thread, capped at M6K_WIN per 30 s dump
     [t102f] hist t=.. ..        per-SVC histogram plus the top caller LRs of each class
     [t102f] sig obj=.. ..       every 0x35 it issues: prev SVC, type/val, word before and after, woke
     [t102f] wait t=.. ..        its wait tuple once per dump plus the words of the owning structure
   Nothing here writes guest memory, changes a return value, allocates, or does libc I/O on the SVC
   path (same rule as the ldr ring: entries are filled on the guest stack, drained by the heartbeat). */
#define M6K_H 0x102fu
#define M6K_RING 8192u
#define M6K_WIN 200u          /* log budget per 30 s window: the ring is not allowed to flood the log */
#define M6K_SIG 128u
typedef struct {
    uint64_t ns, lr, a0, a1, a2, a3, ret, ret2;
    uint32_t num, prev;
    _Atomic uint32_t seq;
} m6k_ent_t;
static m6k_ent_t g_m6k_ring[M6K_RING];
static _Atomic unsigned g_m6k_head, g_m6k_drop, g_m6k_cap;
static _Atomic int g_m6k_prev;          /* previous SVC number of 0x102f (single writer) */
static uint32_t g_m6k_hist[256];
#define M6K_TOP 8
static struct { uint64_t lr; uint32_t n; } g_m6k_top[256][M6K_TOP];
static uint32_t g_m6k_oth[256];         /* calls of this class whose caller LR did not fit the 8 slots */
typedef struct {
    uint64_t ns, addr, lr;
    uint32_t type, prev, wb, wa;
    uint32_t idx;              /* absolute signal index: the drain prints entries newer than its cursor */
    int32_t val;
    int woke;
    _Atomic uint32_t seq;
} m6k_sig_t;
static m6k_sig_t g_m6k_sig[M6K_SIG];
static _Atomic unsigned g_m6k_sig_head;
/* Called from svc_thread() before the body: one histogram bump + one top-LR bump, then (only while
   the window budget lasts) one ring entry. `prev` is the thread's SVC number before this one. */
static int m6k_svc_begin(unsigned num, uint64_t *regs, int prev) {
    unsigned n = num & 0xffu;
    atomic_store_explicit(&g_m6k_prev, prev, memory_order_relaxed);
    if (g_m6k_hist[n] != 0xffffffffu) g_m6k_hist[n]++;
    uint64_t lr = regs[30];
    int k = -1, mn = 0;
    for (int i = 0; i < M6K_TOP; i++) {
        if (g_m6k_top[n][i].lr == lr) { k = i; break; }
        if (g_m6k_top[n][i].n == 0) { mn = i; break; }
        if (g_m6k_top[n][i].n < g_m6k_top[n][mn].n) mn = i;
    }
    if (k >= 0) { if (g_m6k_top[n][k].n != 0xffffffffu) g_m6k_top[n][k].n++; }
    else if (g_m6k_top[n][mn].n == 0) { g_m6k_top[n][mn].lr = lr; g_m6k_top[n][mn].n = 1; }
    else if (g_m6k_oth[n] != 0xffffffffu) g_m6k_oth[n]++;   /* table full: counted, never silently dropped */
    if (atomic_load_explicit(&g_m6k_cap, memory_order_relaxed) >= M6K_WIN) return -1;
    unsigned i = atomic_fetch_add_explicit(&g_m6k_head, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_m6k_cap, 1, memory_order_relaxed);
    if (i >= M6K_RING) { atomic_fetch_add_explicit(&g_m6k_drop, 1, memory_order_relaxed); return -1; }
    m6k_ent_t *e = &g_m6k_ring[i];
    e->seq = 0;
    e->ns = now_ns(); e->num = n; e->prev = (uint32_t)prev;
    e->lr = lr; e->a0 = regs[0]; e->a1 = regs[1]; e->a2 = regs[2]; e->a3 = regs[3];
    e->ret = e->ret2 = 0;
    return (int)i;
}
static void m6k_svc_end(int slot, uint64_t *regs) {
    if (slot < 0) return;
    g_m6k_ring[slot].ret = regs[0]; g_m6k_ring[slot].ret2 = regs[1];
    atomic_store_explicit(&g_m6k_ring[slot].seq, 1, memory_order_release);
}
/* One 0x35 issued by 0x102f, with the word it signalled before and after (type 0 wakes without
   writing; this line is what proves it per event instead of per 30 s dump).
   M6k boot A lesson: a monotonic table died after 128 signals (the title-screen loop signs ~12/s),
   so the exact stall-phase signals were never recorded. Now it is a rolling ring: index = head %
   M6K_SIG, each entry keeps its absolute index, and the drain prints whatever is newer than the
   last index it printed. A 30 s window carries ~25 signals, well inside 128 slots. */
static void m6k_sig_note(uint64_t addr, int type, int32_t val, int prev, uint64_t lr,
                         uint32_t wb, uint32_t wa, int woke, int rdb, int rda) {
    unsigned i = atomic_fetch_add_explicit(&g_m6k_sig_head, 1, memory_order_relaxed);
    m6k_sig_t *e = &g_m6k_sig[i % M6K_SIG];
    atomic_store_explicit(&e->seq, 0, memory_order_relaxed);
    e->idx = i;
    e->ns = now_ns(); e->addr = addr; e->lr = lr;
    e->type = (uint32_t)type | ((uint32_t)(rdb ? 1 : 0) << 8) | ((uint32_t)(rda ? 1 : 0) << 9);
    e->val = val; e->prev = (uint32_t)prev; e->wb = wb; e->wa = wa; e->woke = woke;
    atomic_store_explicit(&e->seq, 1, memory_order_release);
}
/* ---------- M6l: role-key tables ("who feeds the load queue") + the mover table ----------
   M6k left two blind spots, both of them capacity problems, not unknowns:
     - the per-key ledger has 96 slots total and the 0x1D signaller table 64 keys; the boot writes
       thousands of one-shot keys, so the key the load thread parks on was `intable=0` and the
       signaller table reported `31219 signal(s) on keys beyond slot 64`;
     - attribution by guest LR is useless for 0x1D: every game-side call goes through the SDK
       trampoline `svc #0x1d; ret`, so the LR is the trampoline (M6k P0.1), never the call site.
   So the tables below are keyed by ROLE, not by count: a key becomes "the loader key" the moment
   thread 0x1007 waits on it (svc 0x1C), "the clipping key" the moment 0x102f does. They never
   share a slot with the generic tables and never change their behaviour. Every later 0x1C/0x1D on
   such a key, from ANY thread, is counted with the signaller's *thread handle* (the attribution
   that actually works) plus its LR as a secondary label, and the last events go into a rolling
   ring (M6k boot A proved a monotonic table dies before the interesting window).
   Hot-path discipline (M6j: now_ns on this path crashed at t=13 s): integer compares and plain
   stores only -- no now_ns, no I/O, no memset of a large object. Callers hold G. The dump side is
   formatted by the heartbeat thread and decides nothing. */
#define LP_ROLE 3
#define LP_KEY 8
#define LP_SIG 3
#define LP_LAST 8
#define LP_RING 64
#define LP_H_LOAD 0x1007u      /* the thread that loads (M6k: parked on a CV key for the last 600 s) */
#define LP_H_CLIP M6K_H        /* 0x102f, the clipping asynchronous execution task (control group) */
#define LP_H_POST 0x1025u      /* M6l boot A: the thread that signed the load key 1068 of 1145 times */
typedef struct { uint32_t h; uint64_t lr, n; } lp_sig_t;
typedef struct { uint32_t h, w, n; unsigned k; uint64_t lr, addr; } lp_ev_t;   /* w: 0=0x1C wait, 1=0x1D signal */
typedef struct {
    uint64_t key;
    uint64_t n1c, n1d, n1dk;          /* waits / signals / waiters actually woken on this key */
    uint32_t first_h, last_h; uint64_t first_lr, last_lr; unsigned first_k, last_k; int have_first;
    lp_sig_t sig[LP_SIG]; uint64_t oth;
    lp_ev_t last[LP_LAST]; unsigned lasti, lastn;
} lp_key_t;
typedef struct {
    lp_key_t k[LP_KEY]; unsigned n;
    uint64_t ovf;                     /* events on role keys that did not fit a slot */
    unsigned act_k;                   /* dump number of the last event on any of this role's keys */
    lp_ev_t ring[LP_RING]; unsigned ringi; unsigned long ring_n;
} lp_role_t;
static lp_role_t g_lp[LP_ROLE];
static unsigned g_lp_dno;             /* bumped once per dump by lp_dump; stamps carry dumps, not time */
static const uint32_t g_lp_roleh[LP_ROLE] = { LP_H_LOAD, LP_H_CLIP, LP_H_POST };
static const char *const g_lp_rolen[LP_ROLE] = { "load", "clip", "poster" };
static const uint32_t g_lp_mvh[6] = { 0x1001u, 0x1007u, 0x100cu, 0x1010u, 0x1023u, LP_H_POST };
#define LP_MV 6
static uint32_t g_lp_mvn[LP_MV][256];     /* per-class SVC count (saturating) */
static uint64_t g_lp_mvlr[LP_MV][256];    /* last guest LR seen in that class */
static uint64_t g_lp_mv0ba[LP_MV];        /* svc 0x0B: last x0 (ns, or <=0 for a yield) */
static uint64_t g_lp_mv0blr[LP_MV];       /* svc 0x0B: last LR */
static uint32_t g_lp_mv0bn[LP_MV];        /* svc 0x0B: call count */
static int lp_mv_slot(uint32_t h) {
    for (int i = 0; i < LP_MV; i++) if (g_lp_mvh[i] == h) return i;
    return -1;
}
/* One SVC of an observed thread (svc_thread_body already holds `me`). Plain stores only. */
static void lp_mover_note(int slot, unsigned num, uint64_t *regs) {
    unsigned n = num & 0xffu;
    if (g_lp_mvn[slot][n] != 0xffffffffu) g_lp_mvn[slot][n]++;
    g_lp_mvlr[slot][n] = regs[30];
    if (n == 0x0b) {   /* SleepThread: x0 = ns, <=0 means one of the yield forms */
        if (g_lp_mv0bn[slot] != 0xffffffffu) g_lp_mv0bn[slot]++;
        g_lp_mv0ba[slot] = regs[0]; g_lp_mv0blr[slot] = regs[30];
    }
}
static lp_key_t *lp_find(int role, uint64_t key) {
    for (unsigned i = 0; i < g_lp[role].n; i++) if (g_lp[role].k[i].key == key) return &g_lp[role].k[i];
    return NULL;
}
static lp_key_t *lp_add(int role, uint64_t key) {
    if (g_lp[role].n >= LP_KEY) { g_lp[role].ovf++; return NULL; }
    lp_key_t *e = &g_lp[role].k[g_lp[role].n++];
    e->key = key; e->n1c = e->n1d = e->n1dk = 0;
    e->first_h = e->last_h = 0; e->first_lr = e->last_lr = 0; e->first_k = e->last_k = 0; e->have_first = 0;
    e->oth = 0; e->lasti = e->lastn = 0;
    for (int i = 0; i < LP_SIG; i++) { e->sig[i].h = 0; e->sig[i].lr = 0; e->sig[i].n = 0; }
    for (int i = 0; i < LP_LAST; i++) { e->last[i].h = 0; e->last[i].lr = 0; e->last[i].addr = 0; e->last[i].w = 0; e->last[i].n = 0; e->last[i].k = 0; }
    return e;
}
static void lp_ring_push(int role, uint32_t h, int w, uint64_t lr, uint64_t addr, int n) {
    unsigned i = g_lp[role].ringi & (LP_RING - 1u);
    g_lp[role].ring[i].h = h; g_lp[role].ring[i].w = (uint32_t)w; g_lp[role].ring[i].lr = lr;
    g_lp[role].ring[i].addr = addr; g_lp[role].ring[i].n = (uint32_t)n; g_lp[role].ring[i].k = g_lp_dno;
    g_lp[role].ringi = i + 1u; g_lp[role].ring_n++;
}
/* svc 0x1C issued by a role thread: this key is the queue that thread consumes. */
static void lp_wait_note(uint32_t h, uint64_t key, uint64_t lock, uint64_t lr) {
    int role = -1;
    for (int i = 0; i < LP_ROLE; i++) if (g_lp_roleh[i] == h) { role = i; break; }
    if (role < 0) return;
    lp_key_t *e = lp_find(role, key);
    if (!e) e = lp_add(role, key);
    g_lp[role].act_k = g_lp_dno;
    lp_ring_push(role, h, 0, lr, lock, 0);
    if (!e) return;
    e->n1c++;
    unsigned i = e->lasti & (LP_LAST - 1u);
    e->last[i].h = h; e->last[i].w = 0; e->last[i].n = 0; e->last[i].k = g_lp_dno;
    e->last[i].lr = lr; e->last[i].addr = lock;
    e->lasti = i + 1u; if (e->lastn < LP_LAST) e->lastn++;
}
/* svc 0x1D on any key: count it if the key belongs to a role, whoever the signaller is. */
static void lp_sig_note(uint64_t key, uint32_t h, uint64_t lr, int woke) {
    for (int role = 0; role < LP_ROLE; role++) {
        lp_key_t *e = lp_find(role, key);
        if (!e) continue;
        g_lp[role].act_k = g_lp_dno;
        lp_ring_push(role, h, 1, lr, key, woke);
        e->n1d++; e->n1dk += (uint64_t)(woke > 0 ? woke : 0);
        if (!e->have_first) { e->have_first = 1; e->first_h = h; e->first_lr = lr; e->first_k = g_lp_dno; }
        e->last_h = h; e->last_lr = lr; e->last_k = g_lp_dno;
        {   /* top-3 (thread, LR) with exact counts; the rest only into oth */
            int k = -1, mn = 0;
            for (int i = 0; i < LP_SIG; i++) {
                if (e->sig[i].n && e->sig[i].h == h && e->sig[i].lr == lr) { k = i; break; }
                if (!e->sig[i].n) { mn = i; break; }
                if (e->sig[i].n < e->sig[mn].n) mn = i;
            }
            if (k >= 0) { if (e->sig[k].n != 0xffffffffffffffffull) e->sig[k].n++; }
            else if (!e->sig[mn].n) { e->sig[mn].h = h; e->sig[mn].lr = lr; e->sig[mn].n = 1; }
            else e->oth++;
        }
        {   /* rolling last-8: the newest eight events whatever the rate (no slot exhaustion) */
            unsigned i = e->lasti & (LP_LAST - 1u);
            e->last[i].h = h; e->last[i].w = 1; e->last[i].n = (uint32_t)(woke > 0 ? woke : 0);
            e->last[i].k = g_lp_dno; e->last[i].lr = lr; e->last[i].addr = key;
            e->lasti = i + 1u; if (e->lastn < LP_LAST) e->lastn++;
        }
    }
}
/* ---------- M6m: the poster's own condition key (dedicated ledger) ----------
   M6l proved the delivery chain (poster -> 0x1D x1068 -> load key -> 0x1C x1102 -> loader) and that the
   poster then parked on its own CV with dnsvc=0 for 600 s, but it could not name the thread that should
   have signalled that key: the key is neither in the generic [sy] key top-10 nor in [sy] keysig top-8
   (the smallest printed count is 2) and the keysig table overflowed, so "at most 2 signals" and
   "beyond the 64 slots" could not be told apart.
   This is therefore a ledger for ONE key, resolved at run time and never by handle number (M6l: 0x1010
   does not exist in boot A; handles drift between boots):
     1. load thread = the live thread with the most open+read I/O (M6i counters; M6l boot A: 0x1007,
        open=1145 read=1354, rank 0 of [io] iotop). Resolved on the dump side, once.
     2. load key    = the condition key that thread waits on (its first 0x1C after step 1).
     3. poster      = the thread that signalled the load key most often (M6l: 1068 of 1145).
     4. poster key  = the condition key the poster itself waits on (its first 0x1C after step 3, or the
        key it is parked on when the dump resolves it).
   Every 0x1C/0x1D on the poster key is then counted here, from ANY thread, with the caller's handle and
   guest LR, a rolling last-8, and first/last stamps kept as dump numbers (M6j: now_ns on this path
   crashed a thread whose stack was nearly exhausted; the dump side converts them to seconds).
   Hot path: integer compares and plain stores only, callers hold G. The dump side decides nothing. */
#define PS_SIG 8
typedef struct { uint32_t h; uint64_t lr, n; } ps_sig_t;
typedef struct { uint32_t h, w, woke; uint64_t lr, key; unsigned k; } ps_ev_t;
static unsigned g_ps_dno;             /* bumped once per dump by ps_dump; the hot path only reads it */
static struct {
    uint32_t load_h;                  /* the thread that does the real I/O (dump-resolved) */
    uint64_t load_key;                /* the CV key it consumes (registered on its 0x1C) */
    ps_sig_t sig[PS_SIG]; uint64_t sig_ovf;   /* top-8 signallers of the load key: who feeds the loader */
    uint32_t poster_h;                /* top signaller of the load key (dump-resolved) */
    uint64_t pkey;                    /* the poster's own condition key */
    uint64_t n1c, n1d, n1dk;          /* waits / signals / waiters actually woken on pkey */
    uint32_t first_h, last_h; uint64_t first_lr, last_lr; unsigned first_k, last_k; int have_first;
    ps_sig_t psig[3]; uint64_t oth;   /* top-3 signallers of pkey + everything beyond them */
    ps_ev_t last[8]; unsigned lasti, lastn;
    uint64_t pkey_chg;                /* the poster waited on some other key this many times */
} g_ps;
static void ps_ev_push(uint32_t h, int w, uint64_t lr, uint64_t key, int woke) {
    unsigned i = g_ps.lasti & 7u;
    g_ps.last[i].h = h; g_ps.last[i].w = (uint32_t)w;
    g_ps.last[i].woke = (uint32_t)(woke > 0 ? woke : 0);
    g_ps.last[i].lr = lr; g_ps.last[i].key = key; g_ps.last[i].k = g_ps_dno;
    g_ps.lasti = i + 1u; if (g_ps.lastn < 8) g_ps.lastn++;
}
/* svc 0x1C by any thread (caller holds G): the load key comes from the loader, the poster key from the
   poster. Nothing here decides anything -- it only keeps the ledger the dump prints. */
static void ps_wait_note(uint32_t h, uint64_t key, uint64_t lr) {
    if (!key) return;
    if (g_ps.load_h && h == g_ps.load_h) g_ps.load_key = key;
    if (g_ps.poster_h && h == g_ps.poster_h) {
        if (!g_ps.pkey) g_ps.pkey = key;
        if (key == g_ps.pkey) { g_ps.n1c++; ps_ev_push(h, 0, lr, key, 0); }
        else g_ps.pkey_chg++;
    }
}
/* svc 0x1D on any key (caller holds G): count the load key's signallers (to find the poster) and the
   poster key's signallers (the question this round asks). */
static void ps_sig_note(uint64_t key, uint32_t h, uint64_t lr, int woke) {
    if (g_ps.load_key && key == g_ps.load_key) {
        int k = -1, mn = 0;
        for (int i = 0; i < PS_SIG; i++) {
            if (g_ps.sig[i].n && g_ps.sig[i].h == h && g_ps.sig[i].lr == lr) { k = i; break; }
            if (!g_ps.sig[i].n) { mn = i; break; }
            if (g_ps.sig[i].n < g_ps.sig[mn].n) mn = i;
        }
        if (k >= 0) g_ps.sig[k].n++;
        else if (!g_ps.sig[mn].n) { g_ps.sig[mn].h = h; g_ps.sig[mn].lr = lr; g_ps.sig[mn].n = 1; }
        else g_ps.sig_ovf++;
    }
    if (g_ps.pkey && key == g_ps.pkey) {
        g_ps.n1d++; g_ps.n1dk += (uint64_t)(woke > 0 ? woke : 0);
        if (!g_ps.have_first) { g_ps.have_first = 1; g_ps.first_h = h; g_ps.first_lr = lr; g_ps.first_k = g_ps_dno; }
        g_ps.last_h = h; g_ps.last_lr = lr; g_ps.last_k = g_ps_dno;
        int k = -1, mn = 0;
        for (int i = 0; i < 3; i++) {
            if (g_ps.psig[i].n && g_ps.psig[i].h == h && g_ps.psig[i].lr == lr) { k = i; break; }
            if (!g_ps.psig[i].n) { mn = i; break; }
            if (g_ps.psig[i].n < g_ps.psig[mn].n) mn = i;
        }
        if (k >= 0) g_ps.psig[k].n++;
        else if (!g_ps.psig[mn].n) { g_ps.psig[mn].h = h; g_ps.psig[mn].lr = lr; g_ps.psig[mn].n = 1; }
        else g_ps.oth++;
        ps_ev_push(h, 1, lr, key, woke);
    }
}
/* ---------- M6n: whole-process wait graph (key side) ----------
   M6m could answer "who should signal THIS key" only for the poster's key, because every counter it had
   was either top-N or created on the first signal -- and a key that was NEVER signalled is invisible to
   both. This round asks the same question of every parked thread at once, so the key side needs one
   property: "sig=0 with waiters" must mean "never signalled", not "beyond slot N".
   Attempt 1 (M6n boot 1, aborted) registered a key on its first 0x1D: the game signals thousands of
   per-frame keys, the 256 slots filled within 62 s (led_slots=256, ovf=1710) and 18 of 19 waiting keys
   became UNK -- useless. Registration is therefore driven by the WAIT side:
     - svc 0x1C registers the key on its first wait (sig=0 at that moment) and counts waits;
     - svc 0x1D counts the signal only if the key is registered; a signal on a key nobody has ever
       waited on is not part of the graph and only increments g_wg_untracked.
   So the table holds the keys that actually function as queues (tens, not thousands), every signal on
   them is counted from the first wait onward, and "absent" is no longer possible for a key that has
   waiters unless the table itself is full (g_wg_ovf), which is reported and yields UNK.
   What this counter does not see: signals that predate the key's first wait. For the SDK's condition
   variable that case cannot leave a thread parked (the key word is already 1, so the wait returns), and
   the boot's data is cross-checked against [sy] key, which counts from t=0 (P3.4).
   Hot path: one hash, up to WG_PROBE integer compares, two stores; callers hold G. */
#define WG_KEYS 1024
#define WG_PROBE 8
typedef struct { uint64_t key, sig, n1c; uint32_t last_h, first_h; unsigned last_d, first_d; uint64_t last_lr; } wg_key_t;
static wg_key_t g_wg[WG_KEYS];
static uint64_t g_wg_ovf;       /* waits on keys that had no free slot: their key cannot be judged */
static uint64_t g_wg_untracked; /* signals on keys that never had a wait: out of scope, counted only */
static unsigned g_wg_slots;     /* keys registered */
static unsigned g_wg_dno = 1;   /* dump counter, written only by wg_dump; notes only read it (1 = "before the first dump") */
static unsigned wg_hash(uint64_t key) { return (unsigned)(((key >> 4) * 2654435761u) >> 24) & (WG_KEYS - 1u); }
static wg_key_t *wg_key(uint64_t key) {
    if (!key) return NULL;
    unsigned i = wg_hash(key);
    for (unsigned p = 0; p < WG_PROBE; p++, i = (i + 1u) & (WG_KEYS - 1u)) {
        if (g_wg[i].key == key) return &g_wg[i];
        if (!g_wg[i].key) return NULL;
    }
    return NULL;
}
static void wg_wait_note(uint64_t key, uint32_t h) {   /* svc 0x1C, caller holds G */
    if (!key) return;
    wg_key_t *e = wg_key(key);
    if (e) { e->n1c++; return; }
    unsigned i = wg_hash(key);
    for (unsigned p = 0; p < WG_PROBE; p++, i = (i + 1u) & (WG_KEYS - 1u)) {
        if (g_wg[i].key) continue;
        g_wg[i].key = key; g_wg[i].sig = 0; g_wg[i].n1c = 1;
        g_wg[i].last_h = 0; g_wg[i].last_lr = 0; g_wg[i].last_d = 0;
        g_wg[i].first_h = h; g_wg[i].first_d = g_wg_dno;
        g_wg_slots++;
        return;
    }
    g_wg_ovf++;
}
static void wg_sig_note(uint64_t key, uint32_t h, uint64_t lr) {   /* svc 0x1D, caller holds G */
    if (!key) return;
    wg_key_t *e = wg_key(key);
    if (!e) { g_wg_untracked++; return; }
    e->sig++; e->last_h = h; e->last_lr = lr; e->last_d = g_wg_dno;
}
/* main's own 0x1D targets: "it still sends 169 per 30 s -- to whom?" (main = guest thread 0x1001, the
   loader's own thread and the first handle ever allocated; every other thread is named structurally.)
   This side must catch every key, not just queues, so it is a top-32 table; the tail beyond 32 keys is
   counted as ovf and the top-8 (which is what the question asks) is never affected. */
#define MN_SIG 32
static struct { uint64_t key, n, lr; } g_mn_sig[MN_SIG];
static unsigned g_mn_n;
static uint64_t g_mn_ovf, g_mn_total;
static void mn_sig_note(uint64_t key, uint32_t h, uint64_t lr) {
    if (h != 0x1001u || !key) return;
    g_mn_total++;
    for (unsigned i = 0; i < g_mn_n; i++) if (g_mn_sig[i].key == key) { g_mn_sig[i].n++; g_mn_sig[i].lr = lr; return; }
    if (g_mn_n < MN_SIG) { g_mn_sig[g_mn_n].key = key; g_mn_sig[g_mn_n].n = 1; g_mn_sig[g_mn_n].lr = lr; g_mn_n++; return; }
    g_mn_ovf++;
}
/* ---------- M7u (2026-11-07): name the demo/clip upstream submit path ----------
   P0 static xref (this round): every svc#0x1D in the process enters through ONE veneer
   (sdk+0x37eb44 -> gate stub sdk+0xd3f154) whose only callers are
   nn::os::detail::InternalConditionVariableImplByHorizon::{Signal,Broadcast}; the gate stub
   pushes {caller fp, caller ret} at [sp]/[sp+8] and the LR at trap (sdk+0xd3f174) is the
   shared epilogue -- the TRUE call site sits at [sp+8]. entry.S now stores the guest sp into
   the regs[31] pad slot (+248) so the capture below can read it.
   One switch (smo_m7u, C default 0; rollback = echo 0, takes effect <=2 s), two probes:
   [m7u] qchg - shadow memcmp of the clip window (lock-0x40, 25 u64 = 200 B) at every
                svc 0x1C/0x1D entry + every present frame (gfx_bridge beat_frames hook).
                Changes go into a 200-row ring; the heartbeat drains it (no hot-path I/O).
                Every 2 s an occupancy bitmap (words differing from the first-seen baseline)
                is recorded; the two self-referential u64 pairs = the empty intrusive heads.
   [m7u] sig  - on role-key 0x1D (load/clip/poster keys): frame0 = [sp+8] (caller's return
                address), fp chain <=6 hops (fault-safe, bad frame stops), aggregated by
                (handle, frame0, frame1), top6 printed per 100 s window (cap 12 lines).
                Fallback: main's first 200 0x1D (t<=300 s) capture the raw 16 B at sp.
   Hot-path discipline: reads via runtime_safe_read_ungated, integer stores only, no I/O /
   now_ns / large memset on the capture path; all printing happens in runtime_m7u_poll. */
#define M7U_WW 25                     /* 200 B = 25 u64 */
#define M7U_QEV 200
#define M7U_SIGROWS 32
#define M7U_SPX 200
#define M7U_OCC 250
#define M7U_PATH_A "/data/storage/el2/base/files/smo_m7u"
#define M7U_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7u"
__attribute__((constructor)) static void m7u_seed(void) {
    if (fopen(M7U_PATH_A, "r")) return;
    FILE *f = fopen(M7U_PATH_A, "w");
    if (!f) f = fopen(M7U_PATH_B, "w");
    if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7u] seeded = 0 (off)\n"); }
}
static int m7u_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7U_PATH_B : M7U_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
static volatile int g_m7u_on;                     /* refreshed by runtime_m7u_poll every 2 s */
static _Atomic unsigned g_m7u_tsec;               /* heartbeat clock for capture-side stamps */
static _Atomic uint64_t g_m7u_qbase;              /* pinned window base (clip lock - 0x40) */
static uint64_t g_m7u_base_w[M7U_WW];             /* first-seen window contents (baseline) */
static int g_m7u_base_ok;                         /* written under G by the clip thread only */
static uint64_t g_m7u_shadow[M7U_WW];             /* last-seen contents (may race: disclosed) */
static int g_m7u_shadow_ok;
typedef struct { unsigned tsec, src; uint32_t mlo, mhi; uint64_t w[M7U_WW]; } m7u_qev_t;
static m7u_qev_t g_m7u_qev[M7U_QEV];
static _Atomic unsigned g_m7u_qev_n;              /* reserved rows; reader clamps to cap */
static _Atomic unsigned long long g_m7u_chk_svc, g_m7u_chk_frame, g_m7u_qbad;
typedef struct { uint32_t h; uint64_t f0, f1; uint64_t n; } m7u_sigrow_t;
static m7u_sigrow_t g_m7u_sig[M7U_SIGROWS];
static unsigned g_m7u_sig_n; static uint64_t g_m7u_sig_ovf;   /* capture under G: no races */
static struct { unsigned tsec; uint32_t h; uint64_t key, sp0, sp1, fp, lr; int woke; } g_m7u_spx[M7U_SPX];
static _Atomic unsigned g_m7u_spx_n;
static struct { unsigned tsec; uint32_t occ; } g_m7u_occr[M7U_OCC];
static _Atomic unsigned g_m7u_occ_n;
static volatile unsigned g_m7u_pin_tsec;          /* pin event, printed by the poll */
static volatile uint64_t g_m7u_pin_lock;
/* pin the clip window (svc 0x1C by the clip thread, under G): base = lock-0x40, snapshot
   baseline + shadow in one go. The [m7s] neighbour words (lock-0x40+0x28/+0x60) sit inside. */
static void m7u_qpin(uint64_t lock) {
    uint64_t base = lock >= 0x40 ? lock - 0x40 : lock;
    if (atomic_load_explicit(&g_m7u_qbase, memory_order_relaxed) == base) return;
    atomic_store_explicit(&g_m7u_qbase, base, memory_order_relaxed);
    uint64_t w[M7U_WW];
    if (runtime_safe_read_ungated(w, base, sizeof w)) {
        memcpy(g_m7u_base_w, w, sizeof w); memcpy(g_m7u_shadow, w, sizeof w);
        g_m7u_base_ok = 1; g_m7u_shadow_ok = 1;
    } else { g_m7u_base_ok = 0; g_m7u_shadow_ok = 0; }
    g_m7u_pin_tsec = atomic_load_explicit(&g_m7u_tsec, memory_order_relaxed);
    g_m7u_pin_lock = lock;
}
/* the queue-window shadow check (svc entry under G + frame hook on the render thread).
   Plain stores + one safe read; a torn frame-side read can fabricate one event (rare, benign,
   evidence-grade). No I/O here. */
static void m7u_qchg_check(int src) {
    if (!g_m7u_on) return;
    uint64_t base = atomic_load_explicit(&g_m7u_qbase, memory_order_relaxed);
    if (!base) return;
    uint64_t cur[M7U_WW];
    if (!runtime_safe_read_ungated(cur, base, sizeof cur)) { atomic_fetch_add_explicit(&g_m7u_qbad, 1, memory_order_relaxed); return; }
    if (src == 1) atomic_fetch_add_explicit(&g_m7u_chk_svc, 1, memory_order_relaxed);
    else atomic_fetch_add_explicit(&g_m7u_chk_frame, 1, memory_order_relaxed);
    if (!g_m7u_shadow_ok) { memcpy(g_m7u_shadow, cur, sizeof cur); g_m7u_shadow_ok = 1; return; }
    if (memcmp(g_m7u_shadow, cur, sizeof cur) == 0) return;
    unsigned i = atomic_fetch_add_explicit(&g_m7u_qev_n, 1, memory_order_relaxed);
    if (i < M7U_QEV) {
        uint32_t lo = 0, hi = 0;
        for (int k = 0; k < M7U_WW; k++) if (cur[k] != g_m7u_shadow[k]) { if (k < 32) lo |= 1u << k; else hi |= 1u << (k - 32); }
        g_m7u_qev[i].tsec = atomic_load_explicit(&g_m7u_tsec, memory_order_relaxed);
        g_m7u_qev[i].src = (unsigned)src;
        g_m7u_qev[i].mlo = lo; g_m7u_qev[i].mhi = hi;
        memcpy(g_m7u_qev[i].w, cur, sizeof cur);
    }
    memcpy(g_m7u_shadow, cur, sizeof cur);
}
void runtime_m7u_frame_check(void) { m7u_qchg_check(2); }   /* gfx_bridge beat_frames hook */
/* M7u sig side: role-key 0x1D capture (under G). frame0 = [sp+8] = the gate-saved caller
   return address (the shared-trampoline LR is useless); fp chain <=6 for the frames above. */
static void m7u_sig_note(uint64_t key, uint32_t h, const uint64_t *regs, int woke) {
    if (!g_m7u_on) return;
    unsigned tsec = atomic_load_explicit(&g_m7u_tsec, memory_order_relaxed);
    uint64_t f[2] = { 0, 0 };
    uint64_t sp = regs[31];
    if (sp >= 0x100000000ull && !(sp & 7u)) runtime_safe_read_ungated(f, sp, sizeof f);
    /* fallback capture: main's first 200 0x1D in t<=300 s (raw 16 B at sp = {saved fp, ret}) */
    if (h == 0x1001u && tsec <= 300u) {
        unsigned i = atomic_fetch_add_explicit(&g_m7u_spx_n, 1, memory_order_relaxed);
        if (i < M7U_SPX) {
            g_m7u_spx[i].tsec = tsec; g_m7u_spx[i].h = h; g_m7u_spx[i].key = key;
            g_m7u_spx[i].sp0 = f[0]; g_m7u_spx[i].sp1 = f[1];
            g_m7u_spx[i].fp = regs[29]; g_m7u_spx[i].lr = regs[30]; g_m7u_spx[i].woke = woke;
        }
    }
    /* role-key scope: any of the load/clip/poster role keys */
    int role = 0;
    for (int r = 0; r < LP_ROLE && !role; r++)
        for (unsigned i = 0; i < g_lp[r].n; i++) if (g_lp[r].k[i].key == key) { role = 1; break; }
    if (!role) return;
    uint64_t frames[6]; int nf = 0;
    for (uint64_t fp = regs[29]; nf < 6 && fp && !(fp & 7u); ) {
        uint64_t w[2] = { 0, 0 };
        if (!runtime_safe_read_ungated(w, fp, sizeof w)) break;
        if (!w[1] || (w[1] & 3u)) break;
        frames[nf++] = w[1]; fp = w[0];
    }
    uint64_t f1 = nf ? frames[0] : 0;
    for (unsigned i = 0; i < g_m7u_sig_n; i++)
        if (g_m7u_sig[i].h == h && g_m7u_sig[i].f0 == f[1] && g_m7u_sig[i].f1 == f1) {
            if (g_m7u_sig[i].n != 0xffffffffffffffffull) g_m7u_sig[i].n++; return;
        }
    if (g_m7u_sig_n < M7U_SIGROWS) {
        g_m7u_sig[g_m7u_sig_n].h = h; g_m7u_sig[g_m7u_sig_n].f0 = f[1];
        g_m7u_sig[g_m7u_sig_n].f1 = f1; g_m7u_sig[g_m7u_sig_n].n = 1; g_m7u_sig_n++;
        return;
    }
    g_m7u_sig_ovf++;
}
/* dump side: switch refresh (2 s) + occupancy sample (2 s) + ring drains + the 100 s series. */
void runtime_m7u_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    static int ann; static double next_at; static unsigned last_t;
    static unsigned qprint;                        /* qchg rows already drained */
    static unsigned qpin_seen;                     /* pin stamp already announced */
    static unsigned occ_last;                      /* occ rows already counted for the summary */
    static unsigned long long p_ev;
    const unsigned PERIOD = 100u, CAP = 12u;
    static int lines;
    atomic_store_explicit(&g_m7u_tsec, tsec, memory_order_relaxed);
    g_m7u_on = m7u_on();
    if (!g_m7u_on) { ann = 0; return; }
    if (!ann) { ann = 1; vk_boot_logf("[m7u] probe on period=%us cap=%u qwin=200B qev=%u (t=%us)", PERIOD, CAP, M7U_QEV, tsec);
                next_at = (double)tsec + (double)PERIOD; last_t = tsec; return; }
    /* pin announce (once per pin) + the baseline hex (the [m7s] obj span read as 25 u64) */
    if (g_m7u_pin_tsec && g_m7u_pin_tsec != qpin_seen) {
        qpin_seen = g_m7u_pin_tsec;
        vk_boot_logf("[m7u] qpin t=%us pin_t=%us lock=%#llx base=%#llx words=%du64 ok=%d",
                     tsec, g_m7u_pin_tsec, (unsigned long long)g_m7u_pin_lock,
                     (unsigned long long)atomic_load_explicit(&g_m7u_qbase, memory_order_relaxed), M7U_WW, g_m7u_base_ok);
        lines++;
        if (g_m7u_base_ok) {
            char b[768]; int o = 0;
            o += snprintf(b + o, sizeof b - o, "[m7u] qbase0 t=%us", tsec);
            for (int i = 0; i < M7U_WW; i++) o += snprintf(b + o, sizeof b - o, " w%02d=%016llx", i, (unsigned long long)g_m7u_base_w[i]);
            vk_boot_logf("%s", b); lines++;
        }
    }
    /* occupancy sample every 2 s (heartbeat context, safe reads only) */
    uint64_t base = atomic_load_explicit(&g_m7u_qbase, memory_order_relaxed);
    if (base && g_m7u_base_ok) {
        uint64_t cur[M7U_WW];
        if (runtime_safe_read_ungated(cur, base, sizeof cur)) {
            uint32_t occ = 0;
            for (int i = 0; i < M7U_WW; i++) if (cur[i] != g_m7u_base_w[i]) occ |= 1u << i;
            unsigned oi = atomic_load_explicit(&g_m7u_occ_n, memory_order_relaxed);
            if (oi < M7U_OCC) { g_m7u_occr[oi].tsec = tsec; g_m7u_occr[oi].occ = occ;
                                atomic_store_explicit(&g_m7u_occ_n, oi + 1, memory_order_relaxed); }
        }
    }
    /* qchg ring drain: full 25-word hex per event, change indices in the mask */
    unsigned qn = atomic_load_explicit(&g_m7u_qev_n, memory_order_relaxed);
    if (qn > M7U_QEV) qn = M7U_QEV;
    while (qprint < qn && lines < 220) {
        m7u_qev_t *e = &g_m7u_qev[qprint];
        char b[832]; int o = 0;
        o += snprintf(b + o, sizeof b - o, "[m7u] qchg k=%u t=%us src=%s idx=[", qprint, e->tsec, e->src == 1 ? "svc" : "frame");
        int first = 1;
        for (int k = 0; k < M7U_WW; k++) {
            int bit = k < 32 ? (e->mlo >> k) & 1u : (e->mhi >> (k - 32)) & 1u;
            if (bit) { o += snprintf(b + o, sizeof b - o, "%s%d", first ? "" : ",", k); first = 0; }
        }
        o += snprintf(b + o, sizeof b - o, "]");
        for (int i = 0; i < M7U_WW; i++) o += snprintf(b + o, sizeof b - o, " w%02d=%016llx", i, (unsigned long long)e->w[i]);
        vk_boot_logf("%s", b);
        qprint++; lines++;
    }
    /* the 100 s series */
    if ((double)tsec < next_at || lines >= 220) return;
    next_at += (double)PERIOD;
    if (last_t && tsec <= last_t) return;
    last_t = tsec;
    unsigned long long evn = atomic_load_explicit(&g_m7u_qev_n, memory_order_relaxed);
    unsigned long long evc = evn > M7U_QEV ? M7U_QEV : evn;
    unsigned on_ = atomic_load_explicit(&g_m7u_occ_n, memory_order_relaxed); if (on_ > M7U_OCC) on_ = M7U_OCC;
    unsigned occset = 0; unsigned occ_first_t = 0; uint32_t occ_lastmask = 0;
    for (unsigned i = occ_last; i < on_; i++) {
        if (g_m7u_occr[i].occ) { if (!occset) occ_first_t = g_m7u_occr[i].tsec; occset++; occ_lastmask = g_m7u_occr[i].occ; }
    }
    occ_last = on_;
    vk_boot_logf("[m7u] qsum t=%us ev=%llu(+%llu) svcchk=%llu framechk=%llu bad=%llu occn=%u occset=%u occ_t0=%u occ_last=%#x",
                 tsec, evc, evc > p_ev ? evc - p_ev : 0llu,
                 (unsigned long long)atomic_load_explicit(&g_m7u_chk_svc, memory_order_relaxed),
                 (unsigned long long)atomic_load_explicit(&g_m7u_chk_frame, memory_order_relaxed),
                 (unsigned long long)atomic_load_explicit(&g_m7u_qbad, memory_order_relaxed),
                 on_, occset, occ_first_t, (unsigned)occ_lastmask);
    p_ev = evc;
    lines++;
    /* sig top6 (cumulative counts; window deltas are derived offline between lines) */
    {
        m7u_sigrow_t snap[M7U_SIGROWS];
        unsigned n = g_m7u_sig_n;
        memcpy(snap, g_m7u_sig, sizeof snap);
        unsigned long long ovf = g_m7u_sig_ovf;
        char b[1024]; int o = 0;
        o += snprintf(b + o, sizeof b - o, "[m7u] sig t=%us rows=%u ovf=%llu top=[", tsec, n, (unsigned long long)ovf);
        int used = 0;
        unsigned taken[M7U_SIGROWS]; memset(taken, 0, sizeof taken);
        for (int pass = 0; pass < 6 && used < 6; pass++) {
            int bi = -1;
            for (unsigned i = 0; i < n; i++) if (!taken[i] && (bi < 0 || g_m7u_sig[i].n > g_m7u_sig[bi].n)) bi = (int)i;
            if (bi < 0 || !g_m7u_sig[bi].n) break;
            taken[bi] = 1;
            char d0[64], d1[64]; d0[0] = 0; d1[0] = 0;
            if (snap[bi].f0) runtime_describe_address(snap[bi].f0, d0, sizeof d0);
            if (snap[bi].f1) runtime_describe_address(snap[bi].f1, d1, sizeof d1);
            o += snprintf(b + o, sizeof b - o, "%sh%#x@%s|%s x%llu", used ? " " : "", snap[bi].h,
                          d0[0] ? d0 : "-", d1[0] ? d1 : "-", (unsigned long long)snap[bi].n);
            used++;
        }
        o += snprintf(b + o, sizeof b - o, "]");
        vk_boot_logf("%s", b); lines++;
    }
    /* sp-hex fallback summary: distinct (key, h, sp1) combos so far (cap 6 lines total) */
    {
        static int spx_lines;
        unsigned sn = atomic_load_explicit(&g_m7u_spx_n, memory_order_relaxed); if (sn > M7U_SPX) sn = M7U_SPX;
        for (unsigned i = 0; i < sn && spx_lines < 6; i++) {
            int dup = 0;
            for (unsigned j = 0; j < i; j++)
                if (g_m7u_spx[j].key == g_m7u_spx[i].key && g_m7u_spx[j].sp1 == g_m7u_spx[i].sp1 && g_m7u_spx[j].h == g_m7u_spx[i].h) { dup = 1; break; }
            if (dup) continue;
            unsigned long long cnt = 0; unsigned t0 = 0, t1 = 0;
            for (unsigned j = 0; j < sn; j++)
                if (g_m7u_spx[j].key == g_m7u_spx[i].key && g_m7u_spx[j].sp1 == g_m7u_spx[i].sp1 && g_m7u_spx[j].h == g_m7u_spx[i].h) {
                    if (!cnt) t0 = g_m7u_spx[j].tsec; t1 = g_m7u_spx[j].tsec; cnt++;
                }
            char d0[64]; d0[0] = 0;
            if (g_m7u_spx[i].sp1) runtime_describe_address(g_m7u_spx[i].sp1, d0, sizeof d0);
            vk_boot_logf("[m7u] spx t=%us key=%#llx h=%#x f0=%s raw=%016llx/%016llx n=%llu t0=%us t1=%us woke=%d",
                         tsec, (unsigned long long)g_m7u_spx[i].key, g_m7u_spx[i].h, d0[0] ? d0 : "-",
                         (unsigned long long)g_m7u_spx[i].sp0, (unsigned long long)g_m7u_spx[i].sp1,
                         cnt, t0, t1, g_m7u_spx[i].woke);
            spx_lines++; lines++;
        }
    }
}
/* ---------- M7w (2026-11-07): "really stuck vs just slow" quantitative verdict ----------
   Question: is main BLOCKED on one wait (real stall) or RENDERING the same scene at 0.5 fps
   (just slow), and which upstream condition stopped the broadcaster main+0x5b8d54.
   Switch smo_m7w (C default 0; rollback = echo 0 > files/smo_m7w, <=2 s). Read-only probes,
   all printing here in the heartbeat poll:
     [m7w] w2   worker-manager census: walk [[main+0x1369a20]]+0x28 intrusive list (member
                node = W2+0x90, next at node+0; W2 ctor 0x963c3c push_front 0x93eda0), per
                W2 read +0x28 name char* (<=24 B), +0x38 embedded MQ address, +0xd4 state,
                +0xd8 core mask. Clip MQ bases = clip role keys (key-0x48) and 0x102f's
                current lock-0x40. Fires at t>=150/250/300/350, <=12 node lines per walk.
     [m7w] gate W2::sendTo (main+0x964680) ledger: NEW obs gate id 24 at the reject-converge
                0x9646a0 (entry gate impossible: first word cbz imm19 not relocatable) with
                reject-class buckets (msg0/state/dedup/odd reconstructed from msg/state/last),
                + pass bucket on the existing TP4A gate (id 23, this=x0-0x38), + last-20 rows.
                Verdict input: after t~272 s, zero new entries vs entries-rejected-by-state.
     [m7w] pres present ledger (capture in gfx_bridge.c nvn_queue_present_texture): per 10 s
                bucket calls/ok/cpu/off/fail, present duration us avg/max, present-interval
                us avg/max, draws+frames+submit deltas -> frame-time decomposition.
     [m7w] mwk  main wait profile: one 2 s window at 10 Hz per 100 s (heartbeat sleeps in
                here -- guest threads untouched), sampling main's wk/wkey/last_svc under
                trylock(G): run%/parked%, top3 wait keys, longest single 0x1C wait (ns) from
                the M5k hook, >100 ms rows carry the [sp+8] caller frame (entry.S regs[31]).
     [m7w] brc  broadcaster identity: NEW obs gate id 25 at main+0x5b8d54 entry, per call
                {this, caller LR} (LR = the virtual caller's return site, unchanged on entry);
                dump side derefs [[this+0x38][i]] sink vtables -> module offsets, one line
                per call, cap 200.
   Hot-path discipline unchanged: observer = 1 relaxed atomic add + (switch on) 2 guarded
   guest loads + ring plain stores; I/O and formatting only here. */
#define M7W_PATH_A "/data/storage/el2/base/files/smo_m7w"
#define M7W_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7w"
__attribute__((constructor)) static void m7w_seed(void) {
    if (fopen(M7W_PATH_A, "r")) return;
    FILE *f = fopen(M7W_PATH_A, "w");
    if (!f) f = fopen(M7W_PATH_B, "w");
    if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7w] seeded = 0 (off)\n"); }
}
static int m7w_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7W_PATH_B : M7W_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
/* switch flag lives next to the observers (game_patches.c); refreshed every 2 s here. */
extern _Atomic int g_m7w_on;
extern uint64_t smo_obs_hits(int gate);   /* game_patches.c dump accessors (gate ids: tp4r=22 tp4a=23 sendto_rej=24 brc=25) */
_Atomic unsigned g_m7w_tsec;   /* heartbeat seconds, stamped on the 0x1C long-wait rows */
/* ---- main long-wait capture (case 0x1C hook; main thread only, plain statics are
   single-writer). begin stores the wait key + the [sp+8] caller frame (gate-saved caller
   ret, same read as m7u_sig_note); end is called with the measured duration: keeps
   all-boot n/sum/max and rows >100 ms in a 32-row ring. ---- */
static uint64_t g_m7w_wb_key, g_m7w_wb_f0;
static _Atomic unsigned long long g_m7w_w_n, g_m7w_w_sum, g_m7w_w_max;
static _Atomic unsigned g_m7w_wbig_n;
static struct { unsigned tsec; uint64_t key, f0, dur; } g_m7w_wbig[32];
static void runtime_m7w_wait_begin(uint64_t key, const uint64_t *regs) {
    uint64_t f[2] = { 0, 0 }, sp = regs[31];
    if (sp >= 0x100000000ull && !(sp & 7u)) runtime_safe_read_ungated(f, sp, sizeof f);
    g_m7w_wb_key = key; g_m7w_wb_f0 = f[1];
}
static void runtime_m7w_wait_end(unsigned tsec, uint64_t dur_ns) {
    atomic_fetch_add_explicit(&g_m7w_w_n, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_m7w_w_sum, dur_ns, memory_order_relaxed);
    uint64_t m = atomic_load_explicit(&g_m7w_w_max, memory_order_relaxed);
    while (dur_ns > m && !atomic_compare_exchange_weak_explicit(&g_m7w_w_max, &m, dur_ns, memory_order_relaxed, memory_order_relaxed)) {}
    if (dur_ns > 100000000ull) {
        unsigned i = atomic_fetch_add_explicit(&g_m7w_wbig_n, 1, memory_order_relaxed);
        if (i < 32) { g_m7w_wbig[i].tsec = tsec; g_m7w_wbig[i].key = g_m7w_wb_key;
                      g_m7w_wbig[i].f0 = g_m7w_wb_f0; g_m7w_wbig[i].dur = dur_ns; }
    }
}
/* the 2 s @10 Hz main-wait window (blocks THIS heartbeat thread only, guest never) */
static void m7w_mwk_window(unsigned tsec, int *lines) {
    unsigned run = 0, park = 0, wk1 = 0, wk2 = 0, wk3 = 0, miss = 0;
    struct { uint64_t key; unsigned n; } kc[8]; unsigned kcn = 0;
    struct { int svc; unsigned n; } sc[6]; unsigned scn = 0;
    for (int s = 0; s < 20; s++) {
        int wk = -1, svc = -1; uint64_t key = 0;
        if (pthread_mutex_trylock(&G) == 0) {
            for (thr_t *t = g_all; t; t = t->all)
                if (t->handle == 0x1001u && !t->exited) { wk = t->wkind; key = t->wkey; svc = t->last_svc; break; }
            pthread_mutex_unlock(&G);
        }
        if (wk < 0) miss++;
        else {
            if (wk == 0) run++; else park++;
            if (wk == 1) wk1++; else if (wk == 2) wk2++; else if (wk > 0) wk3++;
            if (wk != 0 && key) {
                unsigned u; for (u = 0; u < kcn; u++) if (kc[u].key == key) break;
                if (u == kcn && kcn < 8) { kc[kcn].key = key; kc[kcn].n = 0; kcn++; }
                if (u < 8) kc[u].n++;
            }
            if (svc >= 0) {
                unsigned u; for (u = 0; u < scn; u++) if (sc[u].svc == svc) break;
                if (u == scn && scn < 6) { sc[scn].svc = svc; sc[scn].n = 0; scn++; }
                if (u < 6) sc[u].n++;
            }
        }
        if (s < 19) usleep(100000);
    }
    char kb[160]; int ko = 0;
    for (unsigned u = 0; u < kcn && u < 3; u++)
        ko += snprintf(kb + ko, sizeof kb - ko, "%skey=%#llx x%u", u ? " " : "", (unsigned long long)kc[u].key, kc[u].n);
    char sb[96]; int so = 0;
    for (unsigned u = 0; u < scn && u < 2; u++)
        so += snprintf(sb + so, sizeof sb - so, "%s0x%x x%u", u ? " " : "", sc[u].svc, sc[u].n);
    unsigned long long wn = atomic_load_explicit(&g_m7w_w_n, memory_order_relaxed);
    unsigned long long wm = atomic_load_explicit(&g_m7w_w_max, memory_order_relaxed);
    vk_boot_logf("[m7w] mwk t=%us n=20 run=%u park=%u (lock=%u cv=%u addr=%u) miss=%u top=[%s] svc=[%s] waits=%llu maxwait_us=%llu",
                 tsec, run, park, wk1, wk2, wk3, miss, ko ? kb : "-", so ? sb : "-",
                 wn, (unsigned long long)(wm / 1000ull));
    (*lines)++;
    /* >100 ms rows: key + caller frame ([sp+8]) + duration */
    unsigned bn = atomic_load_explicit(&g_m7w_wbig_n, memory_order_relaxed); if (bn > 32) bn = 32;
    static unsigned wbig_printed;
    for (unsigned i = wbig_printed; i < bn && *lines < 8; i++) {
        char d0[64]; d0[0] = 0;
        if (g_m7w_wbig[i].f0) runtime_describe_address(g_m7w_wbig[i].f0, d0, sizeof d0);
        vk_boot_logf("[m7w] wbig t=%us key=%#llx f0=[sp+8]=%s dur_us=%llu", g_m7w_wbig[i].tsec,
                     (unsigned long long)g_m7w_wbig[i].key, d0[0] ? d0 : "-", (unsigned long long)(g_m7w_wbig[i].dur / 1000ull));
        (*lines)++;
    }
    if (bn > wbig_printed) wbig_printed = bn;
}
/* W2 census: walk the worker-manager list, prioritise clip-named / clip-MQ owners */
static void m7w_w2_census(unsigned tsec, int *lines) {
    extern uint64_t smo_obs_main_base(void);
    uint64_t base = smo_obs_main_base();
    if (!base) { vk_boot_logf("[m7w] w2 t=%us main base unknown", tsec); (*lines)++; return; }
    uint64_t clipk[2] = { 0, 0 }; unsigned clipn = 0; uint64_t cliplock = 0;
    int locked = 0;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    if (locked) {
        for (unsigned i = 0; i < g_lp[1].n && i < 8 && clipn < 2; i++) clipk[clipn++] = g_lp[1].k[i].key;
        for (thr_t *t = g_all; t; t = t->all)
            if (t->handle == LP_H_CLIP && !t->exited) { cliplock = t->waddr; break; }
        pthread_mutex_unlock(&G);
    }
    uint64_t p1 = 0, mgr = 0, node = 0;
    int ok = runtime_safe_read_ungated(&p1, base + 0x1369a20, 8) && p1 &&
             runtime_safe_read_ungated(&mgr, p1, 8) && mgr &&
             runtime_safe_read_ungated(&node, mgr + 0x28, 8);
    if (!ok) { vk_boot_logf("[m7w] w2 t=%us manager unreadable slot=%#llx mgr=%#llx", tsec,
                            (unsigned long long)p1, (unsigned long long)mgr); (*lines)++; return; }
    struct { uint64_t w2, mq; unsigned st, mask, clip; char nm[26]; } rows[32];
    unsigned total = 0;
    while (node && total < 32) {
        uint64_t next = 0;
        runtime_safe_read_ungated(&next, node, 8);
        uint64_t w2 = node - 0x90, nameptr = 0;
        unsigned state = 0, mask = 0;
        if (w2 > 0x100000000ULL && w2 < 0x800000000000ULL) {
            runtime_safe_read_ungated(&nameptr, w2 + 0x28, 8);
            runtime_safe_read_ungated(&state, w2 + 0xd4, 4);
            runtime_safe_read_ungated(&mask, w2 + 0xd8, 4);
        }
        rows[total].w2 = w2; rows[total].mq = w2 + 0x38;
        rows[total].st = state; rows[total].mask = mask; rows[total].clip = 0;
        memcpy(rows[total].nm, "-\0", 2);
        if (nameptr > 0x100000000ULL && nameptr < 0x800000000000ULL) {
            unsigned char b[24] = { 0 };
            if (runtime_safe_read_ungated(b, nameptr, 24)) {
                char *o = rows[total].nm;
                for (int i = 0; i < 24 && b[i]; i++) *o++ = (b[i] >= 0x20 && b[i] < 0x7f) ? (char)b[i] : '.';
                *o = 0;
            }
        }
        for (unsigned k = 0; k < clipn; k++)
            if (clipk[k] && rows[total].mq == clipk[k] - 0x48) { rows[total].clip = 1; break; }
        if (cliplock && rows[total].mq == cliplock - 0x40) rows[total].clip = 1;
        total++;
        if (!next || next == node) break;
        node = next;
    }
    unsigned shown = 0;
    for (int pass = 0; pass < 2 && shown < 12; pass++)
        for (unsigned i = 0; i < total && shown < 12; i++) {
            if (pass == 0 && !rows[i].clip) continue;
            if (pass == 1 && rows[i].clip) continue;
            vk_boot_logf("[m7w] w2 t=%us [%u] w2=%#llx mq=%#llx st=%u mask=%u clip=%d name=\"%s\"",
                         tsec, i, (unsigned long long)rows[i].w2, (unsigned long long)rows[i].mq,
                         rows[i].st, rows[i].mask, rows[i].clip, rows[i].nm);
            shown++; (*lines)++;
        }
    uint64_t cw[2] = { 0, 0 };
    for (unsigned i = 0; i < total; i++) if (rows[i].clip) { if (!cw[0]) cw[0] = rows[i].w2; else if (!cw[1]) cw[1] = rows[i].w2; }
    vk_boot_logf("[m7w] w2 t=%us total=%u shown=%u clip_keys=%u clip_mq1=%#llx->w2=%#llx clip_mq2=%#llx->w2=%#llx cliplock=%#llx",
                 tsec, total, shown, clipn, clipn > 0 ? (unsigned long long)(clipk[0] - 0x48) : 0ull,
                 (unsigned long long)cw[0], clipn > 1 ? (unsigned long long)(clipk[1] - 0x48) : 0ull,
                 (unsigned long long)cw[1], (unsigned long long)cliplock);
    (*lines)++;
}
void runtime_m7w_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern void runtime_m7w_pres_dump(unsigned tsec);
    static int ann; static double next_at; static unsigned last_t; static int lines;
    const unsigned PERIOD = 100u;
    atomic_store_explicit(&g_m7w_tsec, tsec, memory_order_relaxed);
    atomic_store_explicit(&g_m7w_on, m7w_on(), memory_order_relaxed);
    if (!atomic_load_explicit(&g_m7w_on, memory_order_relaxed)) { ann = 0; return; }
    if (!ann) { ann = 1; vk_boot_logf("[m7w] probe on period=%us (t=%us) gates: sendto_rej@main+0x9646a0(id24) brc@main+0x5b8d54(id25) pass=tp4a(id23)", PERIOD, tsec);
                next_at = (double)tsec + (double)PERIOD; last_t = tsec; return; }
    /* brc drain: one line per broadcaster call, cap 200 (runs every 2 s poll) */
    {
        static unsigned brc_printed;
        uint64_t rows[200][3]; uint64_t tot = 0;
        extern int smo_obs_m7w_brc_rows(uint64_t out[200][3], uint64_t *total);
        int n = smo_obs_m7w_brc_rows(rows, &tot);
        for (unsigned i = brc_printed; i < (unsigned)n && brc_printed < 200; i++) {
            uint64_t th = rows[i][1], lr = rows[i][2];
            char dlr[48]; dlr[0] = 0; if (lr) runtime_describe_address(lr, dlr, sizeof dlr);
            uint64_t arr = 0, vt[3] = { 0, 0, 0 };
            if (th > 0x100000000ULL && th < 0x800000000000ULL) {
                if (runtime_safe_read_ungated(&arr, th + 0x38, 8) && arr > 0x100000000ULL && arr < 0x800000000000ULL)
                    for (int k = 0; k < 3; k++) {
                        uint64_t sink = 0;
                        if (runtime_safe_read_ungated(&sink, arr + 8ull * k, 8) && sink > 0x100000000ULL && sink < 0x800000000000ULL)
                            runtime_safe_read_ungated(&vt[k], sink, 8);
                    }
            }
            char dv[3][44];
            for (int k = 0; k < 3; k++) { dv[k][0] = 0; if (vt[k]) runtime_describe_address(vt[k], dv[k], sizeof dv[k]); }
            vk_boot_logf("[m7w] brc t=%us #%u this=%#llx lr=%s vt=[%s %s %s]", tsec, i,
                         (unsigned long long)th, dlr[0] ? dlr : "-", dv[0][0] ? dv[0] : "-", dv[1][0] ? dv[1] : "-", dv[2][0] ? dv[2] : "-");
            brc_printed++;
        }
        if ((unsigned)n > brc_printed && brc_printed >= 200) { static int once; if (!once) { once = 1; vk_boot_logf("[m7w] brc cap 200 reached (total claims=%llu)", (unsigned long long)tot); } }
    }
    runtime_m7w_pres_dump(tsec);   /* its own 10 s cadence */
    if ((double)tsec < next_at || lines >= 120) return;
    next_at += (double)PERIOD;
    if (last_t && tsec <= last_t) return;
    last_t = tsec;
    /* ---- [m7w] gate series + new ring rows ---- */
    {
        extern uint64_t smo_obs_m7w_rej(uint64_t out[4]);
        extern uint64_t smo_obs_m7w_pass_n(void);
        extern int smo_obs_m7w_rej_rows(uint64_t out[20][6], uint64_t *total);
        extern int smo_obs_m7w_pass_rows(uint64_t out[20][5], uint64_t *total);
        static unsigned long long p_srej, p_pass;
        uint64_t rej[4]; uint64_t rejtot = smo_obs_m7w_rej(rej);
        uint64_t passn = smo_obs_m7w_pass_n();
        unsigned long long srej = smo_obs_hits(24), tp4a = smo_obs_hits(23), tp4r = smo_obs_hits(22);
        vk_boot_logf("[m7w] gate t=%us srej=%llu(+%llu) pass=%llu(+%llu) tp4r=%llu tp4a=%llu rejcls=[msg0=%llu state=%llu dedup=%llu odd=%llu]",
                     tsec, srej, srej > p_srej ? srej - p_srej : 0ull, passn, passn > p_pass ? passn - p_pass : 0ull,
                     tp4r, tp4a, (unsigned long long)rej[0], (unsigned long long)rej[1],
                     (unsigned long long)rej[2], (unsigned long long)rej[3]);
        p_srej = srej; p_pass = passn; lines++;
        uint64_t rr[20][6]; uint64_t rtot = 0;
        static unsigned reju_printed;
        int rn = smo_obs_m7w_rej_rows(rr, &rtot);
        for (unsigned i = reju_printed; i < (unsigned)rn; i++) {
            const char *cls = rr[i][2] == 0 ? "msg0" : (rr[i][3] < 3 || rr[i][3] > 5) ? "state" :
                              (rr[i][4] == rr[i][2]) ? "dedup" : "odd";
            char d0[48]; d0[0] = 0; if (rr[i][5]) runtime_describe_address(rr[i][5], d0, sizeof d0);
            vk_boot_logf("[m7w] grej t=%us #%u this=%#llx msg=%#llx st=%u last=%#llx cls=%s lr=%s",
                         tsec, i, (unsigned long long)rr[i][1], (unsigned long long)rr[i][2],
                         (unsigned)rr[i][3], (unsigned long long)rr[i][4], cls, d0[0] ? d0 : "-");
            lines++;
        }
        if ((unsigned)rn > reju_printed) reju_printed = (unsigned)rn;
        uint64_t pr[20][5]; uint64_t ptot = 0;
        static unsigned pass_printed;
        int pn = smo_obs_m7w_pass_rows(pr, &ptot);
        for (unsigned i = pass_printed; i < (unsigned)pn; i++) {
            char d0[48]; d0[0] = 0; if (pr[i][4]) runtime_describe_address(pr[i][4], d0, sizeof d0);
            vk_boot_logf("[m7w] gpass t=%us #%u this=%#llx msg=%#llx st=%u lr=%s",
                         tsec, i, (unsigned long long)pr[i][1], (unsigned long long)pr[i][2],
                         (unsigned)pr[i][3], d0[0] ? d0 : "-");
            lines++;
        }
        if ((unsigned)pn > pass_printed) pass_printed = (unsigned)pn;
    }
    /* ---- [m7w] w2 census at t>=150/250/300/350 (once each) ---- */
    {
        static unsigned char done[4]; static const unsigned at[4] = { 150, 250, 300, 350 };
        for (int i = 0; i < 4; i++)
            if (!done[i] && tsec >= at[i]) { done[i] = 1; m7w_w2_census(tsec, &lines); break; }
    }
    /* ---- [m7w] mwk window every 100 s (2 s @10 Hz, blocks this heartbeat thread only) ---- */
    m7w_mwk_window(tsec, &lines);
}
/* ---- M7x: name main's two alternating wait objects (task 2026-11-07). Switch smo_m7x, C
   default 0. Hot path (main only, switch-gated): one now_ns() + one 32 B safe read at block
   entry, plain stores on return; the release sides (0x1B unlock / 0x35 / 0x1D signal) add one
   addr compare each under G. All printing lives in runtime_m7x_poll (heartbeat thread). ---- */
#define M7X_PATH_A "/data/storage/el2/base/files/smo_m7x"
#define M7X_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7x"
__attribute__((constructor)) static void m7x_seed(void) {
    if (fopen(M7X_PATH_A, "r")) return;
    FILE *f = fopen(M7X_PATH_A, "w");
    if (!f) f = fopen(M7X_PATH_B, "w");
    if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7x] seeded = 0 (off)\n"); }
}
static int m7x_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7X_PATH_B : M7X_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
_Atomic int g_m7x_on;                        /* refreshed every 2 s by runtime_m7x_poll */
_Atomic unsigned g_m7x_tsec;
#define M7X_NOBJ 6
#define M7X_ROWS 512
typedef struct {
    uint64_t addr; unsigned kind;            /* kind: 1 = 0x1A lock, 2 = 0x1C cv (addr = lock), 3 = 0x34 addr */
    uint64_t cv_key;                         /* kind 2: the condition key main waited with */
    uint64_t n_blk, n_imm;                   /* blocked / immediate-return entries (main's calls only) */
    uint64_t sum_ns, max_ns;                 /* blocked duration stats */
    uint64_t res0, res_tmo, res_bad, res_oth;/* blocked result histogram */
    uint64_t tmo_min, tmo_max;               /* duration of the timeout-result waits */
    uint64_t rel_n;                          /* releases seen: kind1/2 lock addr unlock (0x1B), kind2 cv key signal (0x1D), kind3 addr signal (0x35) */
    uint32_t rel_h; uint64_t rel_lr;         /* first releaser (thread handle + its guest LR) */
    uint64_t rel_last_t;                     /* tsec of the last release */
    uint32_t win[8];                         /* last good 32 B window read at block entry */
    unsigned win_good, win_bad;              /* window read outcome counters */
    uint64_t win_t;                          /* tsec of the last captured window */
    uint64_t first_t, last_t;                /* first / last main wait on this object */
} m7x_obj_t;
static m7x_obj_t g_m7x_obj[M7X_NOBJ]; static unsigned g_m7x_nobj; static uint64_t g_m7x_ovf;
typedef struct { unsigned tsec, kind; uint64_t addr, a0, lr, dur; int64_t res;
                 uint32_t win[8]; unsigned win_bad; } m7x_row_t;
static m7x_row_t g_m7x_row[M7X_ROWS];
static uint64_t g_m7x_rn;                    /* rows ever written (main is the only writer) */
/* pending block-entry capture: main is the only writer (same discipline as m7w_wait_begin) */
static uint64_t g_m7x_p_t0; static uint32_t g_m7x_p_win[8]; static unsigned g_m7x_p_wbad;
/* 32 B window at the object, read BEFORE blocking (fault-safe; a bad read is recorded as bad) */
static void m7x_cap_win(uint64_t addr) {
    g_m7x_p_wbad = runtime_safe_read_ungated(g_m7x_p_win, addr, 32) ? 0u : 1u;
}
static m7x_obj_t *m7x_obj(uint64_t addr, unsigned kind, uint64_t key) {
    for (unsigned i = 0; i < g_m7x_nobj; i++)
        if (g_m7x_obj[i].addr == addr && g_m7x_obj[i].kind == kind) {
            if (kind == 2 && !g_m7x_obj[i].cv_key) g_m7x_obj[i].cv_key = key;
            return &g_m7x_obj[i];
        }
    if (g_m7x_nobj >= M7X_NOBJ) { g_m7x_ovf++; return NULL; }
    m7x_obj_t *o = &g_m7x_obj[g_m7x_nobj++];
    memset(o, 0, sizeof *o);                 /* ~150 B, <=6 times per boot, not a hot path */
    o->addr = addr; o->kind = kind; o->cv_key = key;
    o->first_t = atomic_load_explicit(&g_m7x_tsec, memory_order_relaxed);
    return o;
}
/* main-only, G held, after the wait returned. kinds 1 (0x1A) and 3 (0x34) also emit rows. */
static void m7x_note(unsigned kind, uint64_t addr, uint64_t key, const uint64_t *regs, int blocked, int64_t r) {
    m7x_obj_t *o = m7x_obj(addr, kind, key);
    unsigned tsec = atomic_load_explicit(&g_m7x_tsec, memory_order_relaxed);
    uint64_t dur = g_m7x_p_t0 ? now_ns() - g_m7x_p_t0 : 0;
    g_m7x_p_t0 = 0;
    if (!o) return;
    o->last_t = tsec;
    if (!blocked) { o->n_imm++; return; }
    o->n_blk++; o->sum_ns += dur; if (dur > o->max_ns) o->max_ns = dur;
    if (r == 0) o->res0++;
    else if (r == (int64_t)R_TIMEOUT) { o->res_tmo++;
        if (!o->tmo_min || dur < o->tmo_min) o->tmo_min = dur;
        if (dur > o->tmo_max) o->tmo_max = dur; }
    else if (r == (int64_t)R_BAD_STATE) o->res_bad++;
    else o->res_oth++;
    o->win_t = tsec;
    if (g_m7x_p_wbad) o->win_bad++; else { memcpy(o->win, g_m7x_p_win, sizeof o->win); o->win_good++; }
    if (kind != 1 && kind != 3) return;      /* kind 2 rows would burn the 400 cap; m5k ring covers it */
    /* M7y fix (task defect: the t<8 s startup burst ate the 400 printed rows, skipped_earlier=478):
       rows only from t>=60 s. Aggregates above still cover the whole boot; the interesting rows are
       the late-phase ones. */
    if (tsec < 60u) return;
    m7x_row_t *w = &g_m7x_row[g_m7x_rn & (M7X_ROWS - 1u)];
    w->tsec = tsec; w->kind = kind; w->addr = addr; w->a0 = regs[0]; w->dur = dur; w->res = r;
    memcpy(w->win, g_m7x_p_win, sizeof w->win); w->win_bad = g_m7x_p_wbad;
    { uint64_t sp = regs[31], f[2] = { 0, 0 };   /* [sp+8] = the caller frame's return address (entry.S regs[31]) */
      if (sp >= 0x100000000ull && !(sp & 7u)) runtime_safe_read_ungated(f, sp, sizeof f);
      w->lr = f[1]; }
    g_m7x_rn++;
}
/* release side: any thread unlocking (0x1B) / signalling (0x35 / 0x1D) a tracked object */
static void m7x_rel_mark(m7x_obj_t *o, uint32_t h, uint64_t lr) {
    o->rel_n++;
    if (!o->rel_h) { o->rel_h = h; o->rel_lr = lr; }
    o->rel_last_t = atomic_load_explicit(&g_m7x_tsec, memory_order_relaxed);
}
static void m7x_rel_lock(uint64_t addr, uint32_t h, uint64_t lr) {   /* 0x1B: wakes kind-1 waiters; a kind-2 lock shares the addr */
    for (unsigned i = 0; i < g_m7x_nobj; i++) {
        m7x_obj_t *o = &g_m7x_obj[i];
        if (o->kind == 3 || o->addr != addr) continue;
        m7x_rel_mark(o, h, lr);
    }
}
static void m7x_rel_addr(uint64_t addr, uint32_t h, uint64_t lr) {   /* 0x35: wakes kind-3 address waiters */
    for (unsigned i = 0; i < g_m7x_nobj; i++) {
        m7x_obj_t *o = &g_m7x_obj[i];
        if (o->kind != 3 || o->addr != addr) continue;
        m7x_rel_mark(o, h, lr);
    }
}
static void m7x_rel_cv(uint64_t key, uint32_t h, uint64_t lr) {      /* 0x1D: wakes kind-2 cv waiters */
    for (unsigned i = 0; i < g_m7x_nobj; i++) {
        m7x_obj_t *o = &g_m7x_obj[i];
        if (o->kind != 2 || !o->cv_key || o->cv_key != key) continue;
        m7x_rel_mark(o, h, lr);
    }
}
void runtime_m7x_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    static int ann; static double next_ldr, next_series, next_own; static unsigned last_t;
    atomic_store_explicit(&g_m7x_tsec, tsec, memory_order_relaxed);
    atomic_store_explicit(&g_m7x_on, m7x_on(), memory_order_relaxed);
    if (!atomic_load_explicit(&g_m7x_on, memory_order_relaxed)) { ann = 0; return; }
    if (!ann) { ann = 1; vk_boot_logf("[m7x] probe on (t=%us) wobj rows cap=400 win=32B rel=0x1B/0x35/0x1D ldr=60s sem/wsrc=100s wown=3x", tsec); }
    /* ---- [m7x] wobj row drain (cap 400 printed rows; ring keeps the newest 512) ---- */
    {
        static uint64_t printed; static unsigned long long skipped; static int cap_note;
        for (int budget = 24; budget-- > 0 && printed < 400; ) {
            if (g_m7x_rn <= printed) break;
            if (g_m7x_rn - printed > M7X_ROWS) { skipped += g_m7x_rn - printed - M7X_ROWS; printed = g_m7x_rn - M7X_ROWS; }
            m7x_row_t *w = &g_m7x_row[printed & (M7X_ROWS - 1u)];
            char dlr[48]; dlr[0] = 0; if (w->lr) runtime_describe_address(w->lr, dlr, sizeof dlr);
            char hex[73]; int ho = 0;
            for (int k = 0; k < 8; k++) ho += snprintf(hex + ho, sizeof hex - ho, "%08x", w->win[k]);
            vk_boot_logf("[m7x] wobj #%llu t=%us kind=%u addr=%#llx a0=%#llx lr=%s res=0x%llx dur_us=%llu win=%s%s",
                         (unsigned long long)(printed + 1), w->tsec, w->kind,
                         (unsigned long long)w->addr, (unsigned long long)w->a0, dlr[0] ? dlr : "-",
                         (unsigned long long)w->res, (unsigned long long)(w->dur / 1000ull),
                         hex, w->win_bad ? " bad" : "");
            printed++;
        }
        if (printed >= 400 && !cap_note) { cap_note = 1;
            vk_boot_logf("[m7x] wobj row cap 400 reached (written=%llu skipped_earlier=%llu)", (unsigned long long)g_m7x_rn, skipped); }
    }
    /* ---- [m7x] ldr: loader/poster/main state every 60 s ---- */
    if (next_ldr <= 0) next_ldr = (double)tsec + 60.0;
    else if ((double)tsec >= next_ldr) {
        next_ldr += 60.0;
        static const struct { uint32_t h; const char *nm; } tv[3] = { { 0x1001u, "main" }, { LP_H_LOAD, "load" }, { LP_H_POST, "post" } };
        int locked = 0;
        for (int i = 0; i < 50 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
        if (locked) {
            for (int k = 0; k < 3; k++) {
                thr_t *t = 0;
                for (thr_t *q = g_all; q; q = q->all) if (q->handle == tv[k].h && !q->exited) { t = q; break; }
                if (!t) { vk_boot_logf("[m7x] ldr t=%us %s h=%#x absent", tsec, tv[k].nm, tv[k].h); continue; }
                unsigned n1c = t->svch ? t->svch[0x1c] : 0, n1d = t->svch ? t->svch[0x1d] : 0;
                char dlr[48]; dlr[0] = 0; if (t->last_lr) runtime_describe_address(t->last_lr, dlr, sizeof dlr);
                vk_boot_logf("[m7x] ldr t=%us %s h=%#x wk=%d waddr=%#llx wkey=%#llx last_svc=%#x n1c=%u n1d=%u lr=%s",
                             tsec, tv[k].nm, t->handle, t->wkind, (unsigned long long)t->waddr,
                             (unsigned long long)t->wkey, t->last_svc, n1c, n1d, dlr[0] ? dlr : "-");
            }
            pthread_mutex_unlock(&G);
        }
    }
    /* ---- [m7x] wsrc + sem series every 100 s ---- */
    if (next_series <= 0) next_series = (double)tsec + 100.0;
    else if ((double)tsec >= next_series) {
        next_series += 100.0;
        static unsigned long long p_rel[M7X_NOBJ];
        for (unsigned i = 0; i < g_m7x_nobj; i++) {
            m7x_obj_t *o = &g_m7x_obj[i];
            unsigned long long rel = o->rel_n, prel = p_rel[i]; p_rel[i] = rel;
            char dlr[48]; dlr[0] = 0; if (o->rel_lr) runtime_describe_address(o->rel_lr, dlr, sizeof dlr);
            vk_boot_logf("[m7x] wsrc t=%us obj#%u kind=%u addr=%#llx rel=%llu(+%llu) by0=%#x lr=%s last_rel_t=%us",
                         tsec, i, o->kind, (unsigned long long)o->addr, rel, rel > prel ? rel - prel : 0ull,
                         o->rel_h, dlr[0] ? dlr : "-", o->rel_last_t);
            uint64_t avg = o->n_blk ? o->sum_ns / o->n_blk : 0;
            vk_boot_logf("[m7x] sem t=%us obj#%u kind=%u addr=%#llx blk=%llu imm=%llu avg_us=%llu max_us=%llu tmo_us=[%llu..%llu] res0=%llu tmo=%llu bad=%llu oth=%llu",
                         tsec, i, o->kind, (unsigned long long)o->addr,
                         (unsigned long long)o->n_blk, (unsigned long long)o->n_imm,
                         (unsigned long long)(avg / 1000ull), (unsigned long long)(o->max_ns / 1000ull),
                         (unsigned long long)(o->tmo_min / 1000ull), (unsigned long long)(o->tmo_max / 1000ull),
                         (unsigned long long)o->res0, (unsigned long long)o->res_tmo,
                         (unsigned long long)o->res_bad, (unsigned long long)o->res_oth);
        }
        if (!g_m7x_nobj) vk_boot_logf("[m7x] sem t=%us no objects yet (main has not blocked on 0x1A/0x1C/0x34)", tsec);
        last_t = tsec;
    }
    /* ---- [m7x] wown: owner-resolve the last window + live 64 B neighbourhood (max 3 firings) ---- */
    if (next_own <= 0) next_own = (double)tsec + 100.0;
    else if ((double)tsec >= next_own && g_m7x_nobj) {
        next_own += 100.0;
        static int fired;
        if (fired < 3) {
            fired++;
            for (unsigned i = 0; i < g_m7x_nobj; i++) {
                m7x_obj_t *o = &g_m7x_obj[i];
                if (!o->win_good) { vk_boot_logf("[m7x] wown t=%us obj#%u kind=%u addr=%#llx no good window yet (blk=%llu bad=%u)",
                                                 tsec, i, o->kind, (unsigned long long)o->addr,
                                                 (unsigned long long)o->n_blk, o->win_bad); continue; }
                char hex[73]; int ho = 0;
                for (int k = 0; k < 8; k++) ho += snprintf(hex + ho, sizeof hex - ho, "%08x", o->win[k]);
                vk_boot_logf("[m7x] wown t=%us obj#%u kind=%u addr=%#llx win@t=%us %s", tsec, i, o->kind,
                             (unsigned long long)o->addr, o->win_t, hex);
                char d[4][44];
                for (int k = 0; k < 4; k++) {
                    uint64_t v = ((uint64_t)o->win[2 * k + 1] << 32) | o->win[2 * k];
                    if (v > 0x100000000ull && v < 0x800000000000ull) runtime_describe_address(v, d[k], sizeof d[k]);
                    else snprintf(d[k], sizeof d[k], "-");
                }
                vk_boot_logf("[m7x] wown t=%us obj#%u words [0]=%s [1]=%s [2]=%s [3]=%s", tsec, i, d[0], d[1], d[2], d[3]);
                uint8_t nb[64]; uint64_t na = o->addr >= 32 ? o->addr - 32 : o->addr;
                if (runtime_safe_read_ungated(nb, na, sizeof nb)) {
                    char hx[65], asc[24]; int a2 = 0, ao = 0;
                    for (int k = 0; k < 32; k++) { a2 += snprintf(hx + a2, sizeof hx - a2, "%02x", nb[k]);
                                                   if (nb[k] >= 0x20 && nb[k] < 0x7f && ao < 23) asc[ao++] = (char)nb[k]; }
                    hx[a2] = 0; asc[ao] = 0;
                    vk_boot_logf("[m7x] wown t=%us obj#%u nb-32@%#llx %s ascii=\"%s\"", tsec, i, (unsigned long long)na, hx, asc);
                    a2 = 0; ao = 0;
                    for (int k = 32; k < 64; k++) { a2 += snprintf(hx + a2, sizeof hx - a2, "%02x", nb[k]);
                                                    if (nb[k] >= 0x20 && nb[k] < 0x7f && ao < 23) asc[ao++] = (char)nb[k]; }
                    hx[a2] = 0; asc[ao] = 0;
                    vk_boot_logf("[m7x] wown t=%us obj#%u nb+0@%#llx %s ascii=\"%s\"", tsec, i, (unsigned long long)(na + 32), hx, asc);
                } else vk_boot_logf("[m7x] wown t=%us obj#%u nb64 unreadable", tsec, i);
            }
        }
    }
    (void)last_t;
}
/* ---- M7y (task 2026-11-07): what main loops on at the loading screen, is it still feeding the
   load pipeline, and who stopped feeding the loader/poster CVs. Switch smo_m7y, C default 0;
   rollback = echo 0 > .../files/smo_m7y (<=2 s). Five read-only probes:
   [m7y] pc     - guest PC histogram at SVC entry: main full, other guest threads 1/16 (nsvc&15).
                  PC = runtime_svc_pc_tls = the module-relative offset of the svc instruction (the
                  gate passes the ORIGINAL site; LR at trap is only the shared stub). 4096-entry
                  open-addressing table {module, offset, n_main, n_oth}; top10 printed per 100 s.
   [m7y] feeder - svc 0x1D call-site match: frame1 of the fp chain == main+0x61582c (poster job
                  post) or main+0x615cd0 (loader completion post) -> per-site counts with a
                  t<152 / t>=152 split + last call (t, handle, frame0, frame1).
   [m7y] rel    - ROLE-KEY release tracking: every 0x1C/0x1A/0x34 of a role thread (main 0x1001,
                  draw 0x1004, load 0x1007, post 0x1025) registers its (kind,value) into a small
                  table; every 0x1B unlock / 0x35 / 0x1D signal of a registered key counts a
                  release with the releaser's handle + caller frames, pre/post-152 split.
   [m7y] ms     - main's individual 0x1C waits (key, addr, duration, result): 64-row ring +
                  per-key aggregation (the M7x kind-2 aggregate only, no rows, was defect).
   [m7y] st     - every 60 s: main 0x1001 / draw 0x1004 / load 0x1007 / post 0x1025 state
                  {wk, waddr, wkey, last_svc, n1c, n1d, lr} on one screen.
   Hot path: switch-gated, integer ops + fault-safe reads only, no I/O, no now_ns (ms duration
   reuses the m5k timestamp already taken), no large memset. All printing in runtime_m7y_poll. ---- */
#define M7Y_PATH_A "/data/storage/el2/base/files/smo_m7y"
#define M7Y_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7y"
__attribute__((constructor)) static void m7y_seed(void) {
    if (fopen(M7Y_PATH_A, "r")) return;
    FILE *f = fopen(M7Y_PATH_A, "w");
    if (!f) f = fopen(M7Y_PATH_B, "w");
    if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7y] seeded = 0 (off)\n"); }
}
static int m7y_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7Y_PATH_B : M7Y_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
_Atomic int g_m7y_on;                        /* refreshed every 2 s by runtime_m7y_poll */
_Atomic unsigned g_m7y_tsec;
#define M7Y_T152 152u                        /* main->loader job posts stop at t~152 (M7q/M7u) */
/* caller frames at an SVC: frame0 = [sp+8] (the gate-saved caller return), frame1 = one fp hop
   above it (the module call site). Same discipline as m7u_sig_note. */
static void m7y_frames(const uint64_t *regs, uint64_t *f0, uint64_t *f1) {
    uint64_t w[2] = { 0, 0 }, sp = regs[31];
    if (sp >= 0x100000000ull && !(sp & 7u)) runtime_safe_read_ungated(w, sp, sizeof w);
    *f0 = w[1]; *f1 = 0;
    uint64_t fp = regs[29];
    if (fp >= 0x100000000ull && !(fp & 7u)) {
        uint64_t v[2] = { 0, 0 };
        if (runtime_safe_read_ungated(v, fp, sizeof v) && v[1] && !(v[1] & 3u)) *f1 = v[1];
    }
}
/* ---- [m7y] pc: guest PC histogram ---- */
#define M7Y_PCN 4096
typedef struct { const char *mod; uint32_t off; uint64_t n_main, n_oth; } m7y_pc_t;
static m7y_pc_t g_m7y_pc[M7Y_PCN]; static unsigned g_m7y_pc_n; static uint64_t g_m7y_pc_drop;
static pthread_mutex_t g_m7y_pc_lk = PTHREAD_MUTEX_INITIALIZER;
static void m7y_pc_note(thr_t *me) {
    extern __thread uint64_t runtime_svc_pc_tls; extern __thread const char *runtime_svc_mod_tls;
    uint64_t off = runtime_svc_pc_tls; const char *mod = runtime_svc_mod_tls;
    if (!mod || !off) return;
    int is_main = me->handle == 0x1001u;
    if (!is_main && (me->nsvc & 15u)) return;          /* other threads: 1/16 sampling */
    uint32_t o32 = (uint32_t)off;
    pthread_mutex_lock(&g_m7y_pc_lk);
    unsigned h = (unsigned)((o32 >> 2) * 2654435761u + (o32 << 7));
    for (unsigned i = 0; i < 128; i++) {
        m7y_pc_t *e = &g_m7y_pc[(h + i) & (M7Y_PCN - 1u)];
        if (!e->mod) { e->mod = mod; e->off = o32; e->n_main = 0; e->n_oth = 0; g_m7y_pc_n++; }
        if (e->off == o32 && !strcmp(e->mod, mod)) {
            if (is_main) e->n_main++; else e->n_oth++;
            pthread_mutex_unlock(&g_m7y_pc_lk); return;
        }
    }
    g_m7y_pc_drop++;
    pthread_mutex_unlock(&g_m7y_pc_lk);
}
/* ---- [m7y] feeder: the two load-pipeline post sites in main ---- */
typedef struct { uint64_t n, n_pre, n_post, n_line, last_t; uint32_t last_h;
                 uint64_t last_f0, last_f1; } m7y_feed_t;
static m7y_feed_t g_m7y_feed[2];                     /* [0]=main+0x61582c [1]=main+0x615cd0 */
static uint64_t g_m7y_oth_main;                      /* 0x1D whose frame1 is in main but at neither site */
static void m7y_feeder_note(thr_t *me, const uint64_t *regs) {
    if (!atomic_load_explicit(&g_m7y_on, memory_order_relaxed)) return;
    uint64_t f0, f1; m7y_frames(regs, &f0, &f1);
    if (!f1) return;
    extern int runtime_module_lookup(uint64_t, const char **, uint64_t *);
    const char *mod = 0; uint64_t off = 0;
    if (!runtime_module_lookup(f1, &mod, &off) || strcmp(mod, "main")) return;
    unsigned site; uint64_t soff;
    if (off == 0x61582cull) { site = 0; soff = off; }
    else if (off == 0x615cd0ull) { site = 1; soff = off; }
    else { g_m7y_oth_main++; return; }
    (void)soff;
    unsigned tsec = atomic_load_explicit(&g_m7y_tsec, memory_order_relaxed);
    m7y_feed_t *e = &g_m7y_feed[site];
    e->n++; e->n_line++;
    if (tsec < M7Y_T152) e->n_pre++; else e->n_post++;
    e->last_t = tsec; e->last_h = me ? me->handle : 0; e->last_f0 = f0; e->last_f1 = f1;
}
/* ---- [m7y] rel: role-key registration + release tracking ---- */
#define M7Y_NKEY 10
typedef struct {
    unsigned kind;                                   /* 1 = lock addr (0x1A/0x1B), 2 = cv key (0x1C/0x1D), 3 = addr (0x34/0x35) */
    uint64_t val; char tag[12];
    uint64_t n, n_pre, n_post, n_line, t_first, t_last;
    uint32_t h_first, h_last;
    uint64_t f0_first, f1_first, f0_last, f1_last;
} m7y_key_t;
static m7y_key_t g_m7y_key[M7Y_NKEY]; static unsigned g_m7y_key_n; static uint64_t g_m7y_key_ovf;
static int m7y_role(uint32_t h) {
    return h == 0x1001u || h == 0x1004u || h == LP_H_LOAD || h == LP_H_POST;
}
static const char *m7y_rolename(uint32_t h) {
    return h == 0x1001u ? "main" : h == 0x1004u ? "draw" : h == LP_H_LOAD ? "load" : "post";
}
/* called with G held (all three SVC sites hold it) */
static void m7y_key_reg(unsigned kind, uint64_t val, uint32_t h) {
    if (!atomic_load_explicit(&g_m7y_on, memory_order_relaxed)) return;
    for (unsigned i = 0; i < g_m7y_key_n; i++)
        if (g_m7y_key[i].kind == kind && g_m7y_key[i].val == val) return;
    if (g_m7y_key_n >= M7Y_NKEY) { g_m7y_key_ovf++; return; }
    m7y_key_t *k = &g_m7y_key[g_m7y_key_n++];
    memset(k, 0, sizeof *k);                         /* ~90 B, only on first sight of a key */
    k->kind = kind; k->val = val;
    snprintf(k->tag, sizeof k->tag, "%s.%u", m7y_rolename(h), kind);
}
static void m7y_rel(unsigned kind, uint64_t val, uint32_t h, const uint64_t *regs) {
    if (!atomic_load_explicit(&g_m7y_on, memory_order_relaxed)) return;
    for (unsigned i = 0; i < g_m7y_key_n; i++) {
        m7y_key_t *k = &g_m7y_key[i];
        if (k->kind != kind || k->val != val) continue;
        unsigned tsec = atomic_load_explicit(&g_m7y_tsec, memory_order_relaxed);
        k->n++; k->n_line++;
        if (tsec < M7Y_T152) k->n_pre++; else k->n_post++;
        if (!k->t_first) { k->t_first = tsec; k->h_first = h; m7y_frames(regs, &k->f0_first, &k->f1_first); }
        k->t_last = tsec; k->h_last = h; m7y_frames(regs, &k->f0_last, &k->f1_last);
        return;
    }
}
/* ---- [m7y] ms: main's individual 0x1C waits (rows) + per-key aggregation ---- */
#define M7Y_MSROWS 64
#define M7Y_MSK 8
typedef struct { unsigned tsec; uint64_t key, addr, dur; int64_t res; } m7y_msrow_t;
static m7y_msrow_t g_m7y_ms[M7Y_MSROWS]; static uint64_t g_m7y_msn;
typedef struct { uint64_t key, n, sum, max, tmo, last_t; } m7y_mskey_t;
static m7y_mskey_t g_m7y_msk[M7Y_MSK]; static unsigned g_m7y_msk_n; static uint64_t g_m7y_msk_ovf;
static void m7y_ms_note(uint64_t key, uint64_t addr, uint64_t dur, int64_t r) {
    if (!atomic_load_explicit(&g_m7y_on, memory_order_relaxed)) return;
    unsigned tsec = atomic_load_explicit(&g_m7y_tsec, memory_order_relaxed);
    m7y_msrow_t *w = &g_m7y_ms[g_m7y_msn & (M7Y_MSROWS - 1u)];
    w->tsec = tsec; w->key = key; w->addr = addr; w->dur = dur; w->res = r;
    g_m7y_msn++;
    unsigned i;
    for (i = 0; i < g_m7y_msk_n; i++) if (g_m7y_msk[i].key == key) break;
    if (i >= g_m7y_msk_n) {
        if (g_m7y_msk_n >= M7Y_MSK) { g_m7y_msk_ovf++; return; }
        i = g_m7y_msk_n++;
        g_m7y_msk[i].key = key; g_m7y_msk[i].n = 0; g_m7y_msk[i].sum = 0;
        g_m7y_msk[i].max = 0; g_m7y_msk[i].tmo = 0; g_m7y_msk[i].last_t = 0;
    }
    g_m7y_msk[i].n++; g_m7y_msk[i].sum += dur;
    if (dur > g_m7y_msk[i].max) g_m7y_msk[i].max = dur;
    if (r == (int64_t)R_TIMEOUT) g_m7y_msk[i].tmo++;
    g_m7y_msk[i].last_t = tsec;
}
/* ---- dump side ---- */
void runtime_m7y_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    static int ann; static double next_100, next_st;
    atomic_store_explicit(&g_m7y_tsec, tsec, memory_order_relaxed);
    atomic_store_explicit(&g_m7y_on, m7y_on(), memory_order_relaxed);
    if (!atomic_load_explicit(&g_m7y_on, memory_order_relaxed)) { ann = 0; return; }
    if (!ann) { ann = 1; vk_boot_logf("[m7y] probe on (t=%us) pc=main-full/oth-1:16 tbl=%u feed=main+0x61582c,main+0x615cd0 rel=reg@0x1A/1C/34 keys<=%u t152=%us ms=rows%u st=60s",
                                      tsec, M7Y_PCN, M7Y_NKEY, M7Y_T152, M7Y_MSROWS); }
    /* ---- [m7y] st: the four roles on one screen, every 60 s ---- */
    if (next_st <= 0) next_st = (double)tsec + 60.0;
    else if ((double)tsec >= next_st) {
        next_st += 60.0;
        static const struct { uint32_t h; const char *nm; } tv[4] = {
            { 0x1001u, "main" }, { 0x1004u, "draw" }, { LP_H_LOAD, "load" }, { LP_H_POST, "post" } };
        int locked = 0;
        for (int i = 0; i < 50 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
        if (locked) {
            for (int k = 0; k < 4; k++) {
                thr_t *t = 0;
                for (thr_t *q = g_all; q; q = q->all) if (q->handle == tv[k].h && !q->exited) { t = q; break; }
                if (!t) { vk_boot_logf("[m7y] st t=%us %s h=%#x absent", tsec, tv[k].nm, tv[k].h); continue; }
                unsigned n1c = t->svch ? t->svch[0x1c] : 0, n1d = t->svch ? t->svch[0x1d] : 0;
                char dlr[48]; dlr[0] = 0; if (t->last_lr) runtime_describe_address(t->last_lr, dlr, sizeof dlr);
                vk_boot_logf("[m7y] st t=%us %s h=%#x wk=%d waddr=%#llx wkey=%#llx last_svc=%#x n1c=%u n1d=%u nsvc=%llu lr=%s",
                             tsec, tv[k].nm, t->handle, t->wkind, (unsigned long long)t->waddr,
                             (unsigned long long)t->wkey, t->last_svc, n1c, n1d,
                             (unsigned long long)t->nsvc, dlr[0] ? dlr : "-");
            }
            pthread_mutex_unlock(&G);
        }
    }
    /* ---- 100 s series: feeder, rel, ms, pc ---- */
    if (next_100 <= 0) next_100 = (double)tsec + 100.0;
    else if ((double)tsec < next_100) return;
    next_100 += 100.0;
    /* feeder */
    for (unsigned s = 0; s < 2; s++) {
        m7y_feed_t *e = &g_m7y_feed[s];
        char d0[48], d1[48]; d0[0] = 0; d1[0] = 0;
        if (e->last_f0) runtime_describe_address(e->last_f0, d0, sizeof d0);
        if (e->last_f1) runtime_describe_address(e->last_f1, d1, sizeof d1);
        vk_boot_logf("[m7y] feeder t=%us site=%u n=%llu(+%llu) pre152=%llu post152=%llu last_t=%us h=%#x f0=%s f1=%s",
                     tsec, s, (unsigned long long)e->n, (unsigned long long)e->n_line,
                     (unsigned long long)e->n_pre, (unsigned long long)e->n_post,
                     (unsigned)e->last_t, e->last_h, d0[0] ? d0 : "-", d1[0] ? d1 : "-");
        e->n_line = 0;
    }
    vk_boot_logf("[m7y] feeder t=%us oth_main=%llu (0x1D with frame1 in main at neither site)",
                 tsec, (unsigned long long)g_m7y_oth_main);
    /* rel */
    if (!g_m7y_key_n) vk_boot_logf("[m7y] rel t=%us no role keys registered yet", tsec);
    for (unsigned i = 0; i < g_m7y_key_n; i++) {
        m7y_key_t *k = &g_m7y_key[i];
        char d0[48], d1[48], e0[48], e1[48]; d0[0] = 0; d1[0] = 0; e0[0] = 0; e1[0] = 0;
        if (k->f0_first) runtime_describe_address(k->f0_first, d0, sizeof d0);
        if (k->f1_first) runtime_describe_address(k->f1_first, d1, sizeof d1);
        if (k->f0_last) runtime_describe_address(k->f0_last, e0, sizeof e0);
        if (k->f1_last) runtime_describe_address(k->f1_last, e1, sizeof e1);
        vk_boot_logf("[m7y] rel t=%us #%u %s kind=%u key=%#llx n=%llu(+%llu) pre152=%llu post152=%llu "
                     "first t=%us h=%#x f0=%s f1=%s | last t=%us h=%#x f0=%s f1=%s",
                     tsec, i, k->tag, k->kind, (unsigned long long)k->val,
                     (unsigned long long)k->n, (unsigned long long)k->n_line,
                     (unsigned long long)k->n_pre, (unsigned long long)k->n_post,
                     (unsigned)k->t_first, k->h_first, d0[0] ? d0 : "-", d1[0] ? d1 : "-",
                     (unsigned)k->t_last, k->h_last, e0[0] ? e0 : "-", e1[0] ? e1 : "-");
        k->n_line = 0;
    }
    if (g_m7y_key_ovf) vk_boot_logf("[m7y] rel t=%us key table ovf=%llu (slots=%u)", tsec, (unsigned long long)g_m7y_key_ovf, M7Y_NKEY);
    /* ms: per-key aggregation + newest rows */
    if (!g_m7y_msk_n) vk_boot_logf("[m7y] ms t=%us main has no 0x1C waits yet", tsec);
    for (unsigned i = 0; i < g_m7y_msk_n; i++)
        vk_boot_logf("[m7y] ms t=%us key=%#llx n=%llu avg_us=%llu max_us=%llu tmo=%llu last_t=%us",
                     tsec, (unsigned long long)g_m7y_msk[i].key, (unsigned long long)g_m7y_msk[i].n,
                     (unsigned long long)(g_m7y_msk[i].n ? g_m7y_msk[i].sum / g_m7y_msk[i].n : 0),
                     (unsigned long long)(g_m7y_msk[i].max / 1000ull),
                     (unsigned long long)g_m7y_msk[i].tmo, (unsigned)g_m7y_msk[i].last_t);
    if (g_m7y_msk_ovf) vk_boot_logf("[m7y] ms t=%us key table ovf=%llu (slots=%u)", tsec, (unsigned long long)g_m7y_msk_ovf, M7Y_MSK);
    {
        static uint64_t printed;
        for (int budget = 12; budget-- > 0 && printed < g_m7y_msn; ) {
            m7y_msrow_t *w = &g_m7y_ms[printed & (M7Y_MSROWS - 1u)];
            vk_boot_logf("[m7y] msrow #%llu t=%us key=%#llx addr=%#llx dur_us=%llu res=0x%llx",
                         (unsigned long long)(printed + 1), w->tsec, (unsigned long long)w->key,
                         (unsigned long long)w->addr, (unsigned long long)(w->dur / 1000ull),
                         (unsigned long long)w->res);
            printed++;
        }
    }
    /* pc: top10 by n_main, top10 by n_main+n_oth (static snapshot: 128 KB, heartbeat thread only) */
    {
        static m7y_pc_t snap[M7Y_PCN]; static unsigned char taken[(M7Y_PCN + 7) / 8];
        unsigned n;
        pthread_mutex_lock(&g_m7y_pc_lk);
        n = g_m7y_pc_n; if (n > M7Y_PCN) n = M7Y_PCN;
        memcpy(snap, g_m7y_pc, sizeof snap);
        unsigned long long drop = g_m7y_pc_drop;
        pthread_mutex_unlock(&g_m7y_pc_lk);
        char b[640]; int o;
        o = snprintf(b, sizeof b, "[m7y] pc t=%us main_top10 n=%u drop=%llu [", tsec, n, drop);
        memset(taken, 0, sizeof taken);
        for (int pass = 0; pass < 10; pass++) {
            int bi = -1;
            for (unsigned i = 0; i < M7Y_PCN; i++) {
                if (!snap[i].mod || !snap[i].n_main) continue;
                if (taken[i >> 3] & (1u << (i & 7))) continue;
                if (bi < 0 || snap[i].n_main > snap[bi].n_main) bi = (int)i;
            }
            if (bi < 0) break;
            taken[bi >> 3] |= (unsigned char)(1u << (bi & 7));
            o += snprintf(b + o, sizeof b - (size_t)o, "%s%s+0x%x=%llu", pass ? " " : "", snap[bi].mod,
                          snap[bi].off, (unsigned long long)snap[bi].n_main);
        }
        if (o > (int)sizeof b - 16) o = (int)sizeof b - 16;
        snprintf(b + o, sizeof b - (size_t)o, "]");
        vk_boot_logf("%s", b);
        o = snprintf(b, sizeof b, "[m7y] pc t=%us all_top10 [", tsec);
        memset(taken, 0, sizeof taken);
        for (int pass = 0; pass < 10; pass++) {
            int bi = -1; uint64_t bt = 0;
            for (unsigned i = 0; i < M7Y_PCN; i++) {
                if (!snap[i].mod) continue;
                uint64_t tot = snap[i].n_main + snap[i].n_oth;
                if (!tot) continue;
                if (taken[i >> 3] & (1u << (i & 7))) continue;
                if (bi < 0 || tot > bt) { bi = (int)i; bt = tot; }
            }
            if (bi < 0) break;
            taken[bi >> 3] |= (unsigned char)(1u << (bi & 7));
            o += snprintf(b + o, sizeof b - (size_t)o, "%s%s+0x%x=%llu", pass ? " " : "", snap[bi].mod,
                          snap[bi].off, (unsigned long long)bt);
        }
        if (o > (int)sizeof b - 16) o = (int)sizeof b - 16;
        snprintf(b + o, sizeof b - (size_t)o, "]");
        vk_boot_logf("%s", b);
    }
}
/* ---- M7a (task 2026-11-07): M7z verdict runtime adjudication - which of 1) sequencer-stop /
   2) stranded-request / 3) NULL-result-abort stopped the load pipeline. Switch smo_m7a, C default 0;
   rollback = echo 0 > .../files/smo_m7a (<=2 s). Guest-side gates (game_patches.c, both read-only):
   gate 26 = fnB main+0x615cb0 entry (x0=req: every completion passes here AFTER the processor
   stored [req+0x110]; NULL/non-NULL tally + last-16 ring), gate 27 = find main+0x615e50 entry
   (x0 = request registry vector {cap u32@+0, base@+8, count i32@+0x10}, stride 0x118, latched <=4).
   [m7a] reg  - every 30 s: walk each latched vector; per slot {state@+0x60, result@+0x110,
                MQ@+0x68 {buf@+0x20, cap@+0x28, count@+0x2c, head@+0x30}, name@+0xb8 (+1 deref)};
                full rows from t>=200 (<=40 slots), else one summary line per vector.
   [m7a] trio - every 60 s: 0x1025/0x102a/0x1026 park attribution {wk, waddr, wkey} + describe +
                match against the last census MQ windows (waddr==mq+0x38 => parked inside fnC).
   [m7a] null - every 60 s: drain the fnB ring (tally + last 16 rows).
   [m7a] got  - once at t>=30: walk main GOT window 0x135f000..0x136c000 (read-only), nonzero
                slots -> runtime_module_lookup; print load-pipeline key slots + the os sync family
                (sdk+0x324a00..0x326600) + digest + compact table (cap 480 slots).
   [m7a] kick - switch smo_m7a_kick (C default 0), one-shot at t>200: for the first census
                stranded slot (state<=1) with MQ count<cap: emulate the guest TrySend
                (buffer[(head+count)%cap]=1, count++) then wake the parked fnC waiter through the
                same path svc 0x1D uses (lock_acquire_for under G). No stranded census slot =>
                logged as "not executed", never sent (task rule).
   Hot path: two svc-entry gates, fault-guarded reads + atomics only, no I/O, no now_ns, no large
   memset; all printing in runtime_m7a_poll (heartbeat thread). ---- */
#define M7A_PATH_A "/data/storage/el2/base/files/smo_m7a"
#define M7A_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7a"
#define M7A_PATH_KA "/data/storage/el2/base/files/smo_m7a_kick"
#define M7A_PATH_KB "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7a_kick"
__attribute__((constructor)) static void m7a_seed(void) {
    if (!fopen(M7A_PATH_A, "r")) {
        FILE *f = fopen(M7A_PATH_A, "w");
        if (!f) f = fopen(M7A_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7a] seeded = 0 (off)\n"); }
    }
    if (!fopen(M7A_PATH_KA, "r")) {
        FILE *f = fopen(M7A_PATH_KA, "w");
        if (!f) f = fopen(M7A_PATH_KB, "w");
        if (f) { fputs("0\n", f); fclose(f); }
    }
}
static int m7a_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7A_PATH_B : M7A_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
static int m7a_kick_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7A_PATH_KB : M7A_PATH_KA;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7a_on;                     /* game_patches.c, refreshed here every 2 s */
extern uint64_t smo_obs_main_base(void);         /* game_patches.c (set at gate install time) */
static inline void m7a_st64(uint64_t a, uint64_t v) { __atomic_store_n((uint64_t *)a, v, __ATOMIC_SEQ_CST); }
#define M7A_MQC 40
static struct { uint64_t slot, mq; unsigned state; } g_m7a_mqc[M7A_MQC];
static unsigned g_m7a_mqc_n;
static void m7a_ascii(char *out, size_t on, const uint64_t *w, int nw) {   /* sanitize guest name bytes */
    size_t o = 0;
    for (int k = 0; k < nw && o + 1 < on; k++)
        for (int b = 0; b < 8 && o + 1 < on; b++) {
            unsigned char c = (unsigned char)(w[k] >> (8 * b));
            if (!c) { k = nw; break; }                   /* NUL terminates the name */
            out[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
        }
    out[o] = 0;
}
/* one census pass over one latched vector; caches stranded slots into g_m7a_mqc; returns count */
static int m7a_census_vec(unsigned tsec, uint64_t vec, int vec_i, int full) {
    extern void vk_boot_logf(const char *fmt, ...);
    uint64_t hdr[3];
    if (!runtime_safe_read_ungated(hdr, vec, sizeof hdr)) {
        vk_boot_logf("[m7a] reg t=%us vec=%#llx unreadable", tsec, (unsigned long long)vec);
        return 0;
    }
    unsigned cap = (unsigned)hdr[0]; uint64_t base = hdr[1]; int count = (int)(int32_t)hdr[2];
    int stranded = 0;
    unsigned stc[5] = { 0, 0, 0, 0, 0 };             /* state 0/1/2/3/other buckets */
    char idx[96]; unsigned idx_o = 0;
    if (count < 0) count = 0;
    g_m7a_mqc_n = 0;
    int bad = base <= 0x100000000ULL || base >= 0x800000000000ULL;
    for (int i = 0; i < count && i < M7A_MQC; i++) {
        uint64_t slot = base + (uint64_t)i * 0x118ull;
        if (bad) break;
        uint64_t hi[3], lo[3], rs[1], mqw[4];
        if (!runtime_safe_read_ungated(hi, slot + 0x60, sizeof hi) ||      /* +0x60 state */
            !runtime_safe_read_ungated(lo, slot + 0xb8, sizeof lo) ||      /* +0xb8 name window */
            !runtime_safe_read_ungated(rs, slot + 0x110, sizeof rs) ||     /* +0x110 result */
            !runtime_safe_read_ungated(mqw, slot + 0x88, sizeof mqw)) {    /* mq+0x20 buf/+0x28 cap,cnt/+0x30 head */
            vk_boot_logf("[m7a] reg t=%us v%d [%d] slot=%#llx unreadable", tsec, vec_i, i, (unsigned long long)slot);
            continue;
        }
        unsigned st = (unsigned)(uint32_t)hi[0];
        uint64_t mq = slot + 0x68;
        unsigned mcap = (unsigned)(uint32_t)(mqw[1] & 0xffffffffu);
        unsigned mcnt = (unsigned)(uint32_t)(mqw[1] >> 32);
        unsigned mhead = (unsigned)(uint32_t)mqw[2];
        if (st <= 3) stc[st]++; else stc[4]++;
        if (st <= 1) {
            stranded++;
            if (idx_o < sizeof idx - 12) idx_o += (unsigned)snprintf(idx + idx_o, sizeof idx - idx_o, "%s%d", idx_o ? "," : "", i);
            if (g_m7a_mqc_n < M7A_MQC) {
                g_m7a_mqc[g_m7a_mqc_n].slot = slot;
                g_m7a_mqc[g_m7a_mqc_n].mq = mq;
                g_m7a_mqc[g_m7a_mqc_n].state = st;
                g_m7a_mqc_n++;
            }
        }
        if (full) {
            char nm[25], dn[25];
            m7a_ascii(nm, sizeof nm, lo, 3);
            uint64_t d[2] = { 0, 0 };
            if (lo[0] > 0x100000000ULL && lo[0] < 0x800000000000ULL &&
                runtime_safe_read_ungated(d, lo[0], sizeof d))
                m7a_ascii(dn, sizeof dn, d, 2);
            else dn[0] = 0;
            vk_boot_logf("[m7a] reg t=%us v%d [%d/%d] slot=%#llx st=%u res=%#llx mq={buf=%#llx cap=%u cnt=%u head=%u} name=\"%s\" deref=\"%s\"",
                         tsec, vec_i, i, count, (unsigned long long)slot, st,
                         (unsigned long long)rs[0], (unsigned long long)mqw[0], mcap, mcnt, mhead, nm, dn);
        }
    }
    idx[idx_o] = 0;
    vk_boot_logf("[m7a] reg t=%us vec=%#llx cap=%u count=%d st0=%u st1=%u st2=%u st3=%u oth=%u stranded_idx=[%s] full=%d",
                 tsec, (unsigned long long)vec, cap, count, stc[0], stc[1], stc[2], stc[3], stc[4], idx, full);
    return stranded;
}
static void m7a_trio(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    static const uint32_t tv[3] = { 0x1025u, 0x102au, 0x1026u };
    for (int k = 0; k < 3; k++) {
        thr_t *t = 0;
        int locked = 0;
        for (int i = 0; i < 50 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
        if (!locked) { vk_boot_logf("[m7a] trio t=%us h=%#x G-busy", tsec, tv[k]); continue; }
        for (thr_t *q = g_all; q; q = q->all) if (q->handle == tv[k] && !q->exited) { t = q; break; }
        if (!t) { pthread_mutex_unlock(&G); vk_boot_logf("[m7a] trio t=%us h=%#x absent", tsec, tv[k]); continue; }
        unsigned n1c = t->svch ? t->svch[0x1c] : 0, n1d = t->svch ? t->svch[0x1d] : 0;
        uint64_t waddr = t->waddr, wkey = t->wkey; int wk = t->wkind;
        unsigned nsvc = (unsigned)t->nsvc; unsigned last_svc = t->last_svc;
        pthread_mutex_unlock(&G);
        char d1[48], d2[48]; d1[0] = 0; d2[0] = 0;
        if (waddr) runtime_describe_address(waddr, d1, sizeof d1);
        if (wkey) runtime_describe_address(wkey, d2, sizeof d2);
        /* match against the last census MQ windows: mutex@mq+0x38 / receiver-cv@+0x40 (block recv) */
        const char *mt = "-";
        for (unsigned i = 0; i < g_m7a_mqc_n; i++) {
            uint64_t mq = g_m7a_mqc[i].mq;
            if (waddr == mq + 0x38) mt = "fnC-mutex";
            else if (wkey == mq + 0x40) mt = "fnC-cv";
            else if (waddr >= mq && waddr < mq + 0x48) mt = "fnC-mq";
        }
        vk_boot_logf("[m7a] trio t=%us h=%#x wk=%d waddr=%#llx(%s) wkey=%#llx(%s) match=%s last_svc=%#x n1c=%u n1d=%u nsvc=%u",
                     tsec, tv[k], wk, (unsigned long long)waddr, d1[0] ? d1 : "-",
                     (unsigned long long)wkey, d2[0] ? d2 : "-", mt, last_svc, n1c, n1d, nsvc);
    }
}
static void m7a_null_drain(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7a_null_rows(uint64_t out[16][12], uint64_t *total, uint64_t *n_null, uint64_t *n_res);
    uint64_t rows[16][12], tot = 0, nn = 0, nr = 0;
    int n = smo_obs_m7a_null_rows(rows, &tot, &nn, &nr);
    vk_boot_logf("[m7a] null t=%us total=%llu null=%llu res=%llu ring=%d (last-16)", tsec,
                 (unsigned long long)tot, (unsigned long long)nn, (unsigned long long)nr, n);
    for (int i = 0; i < n; i++) {
        uint64_t nw[6] = { rows[i][4], rows[i][5], rows[i][6], rows[i][7], rows[i][8], rows[i][9] };
        uint64_t d[2] = { rows[i][10], rows[i][11] };
        char nm[49], dn[25];
        m7a_ascii(nm, sizeof nm, nw, 6);                 /* M7b fix: name = find key @slot+0x08 */
        m7a_ascii(dn, sizeof dn, d, 2);
        vk_boot_logf("[m7a] nullrow t=%us #%d ns=%llus vt=%#llx st=%u res=%#llx name=\"%s\" deref=\"%s\"",
                     tsec, i, (unsigned long long)(rows[i][0] / 1000000000ull),
                     (unsigned long long)rows[i][1], (unsigned)rows[i][2],
                     (unsigned long long)rows[i][3], nm, dn);
    }
}
/* [m7a] got: one-shot at t>=30, main GOT window walk (read-only) */
static void m7a_got(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int runtime_module_lookup(uint64_t, const char **, uint64_t *);
    uint64_t mb = smo_obs_main_base();
    if (!mb) { vk_boot_logf("[m7a] got t=%us main_base=0 (gates not installed) skip", tsec); return; }
    static uint64_t tbl[0xD000 / 8];
    const uint64_t lo = 0x135f000ull, hi = 0x136c000ull;
    unsigned nslot = 0, nz = 0, n_sdk = 0, n_main = 0, n_oth = 0;
    for (uint64_t a = lo; a < hi; a += 0x1000) {
        if (!runtime_safe_read_ungated(&tbl[nslot], mb + a, 0x1000))
            vk_boot_logf("[m7a] got t=%us chunk %#llx unreadable", tsec, (unsigned long long)a);
        nslot += 0x1000 / 8;
    }
    /* key slots first: the load-pipeline PLT stubs + the registry/guard data slots */
    static const struct { uint64_t off; const char *what; } key[] = {
        { 0x135f2a0, "plt 0xa285d0 guard-lock" }, { 0x135f2a8, "plt 0xa285e0 guard-unlock" },
        { 0x135f748, "plt 0xa28f20 gate predicate" },
        { 0x135ffe8, "plt 0xa2a060 FinalizeMQ" }, { 0x135fff0, "plt 0xa2a070 InitMQ" },
        { 0x135fff8, "plt 0xa2a080 TryRecvMQ" }, { 0x1360000, "plt 0xa2a090 TrySendMQ" },
        { 0x1360008, "plt 0xa2a0a0 JamMQ" }, { 0x1360010, "plt 0xa2a0b0 RecvMQ" },
        { 0x1369a20, "fnA gate obj" }, { 0x1369a28, "processor find-registry" },
        { 0x1369a38, "req flavor A vtable" }, { 0x1369a58, "req flavor B vtable" },
        { 0x1366470, "guard obj" }, { 0x1366478, "list head*" }, { 0x1366488, "static node" },
        { 0x1360738, "guard byte*" }, { 0x1360740, "str c" }, { 0x1360748, "str d" }, { 0x1360750, "str e" },
    };
    for (unsigned k = 0; k < sizeof key / sizeof key[0]; k++) {
        uint64_t v = tbl[(key[k].off - lo) / 8];
        const char *mod = 0; uint64_t off = 0;
        char tgt[48];
        if (v > 0x100000000ULL && v < 0x800000000000ULL && runtime_module_lookup(v, &mod, &off))
            snprintf(tgt, sizeof tgt, "%s+0x%llx", mod, (unsigned long long)off);
        else snprintf(tgt, sizeof tgt, "-");
        vk_boot_logf("[m7a] got t=%us slot=%#llx val=%#llx -> %s (%s)", tsec,
                     (unsigned long long)key[k].off, (unsigned long long)v, tgt, key[k].what);
    }
    /* os sync family window + digest + compact table */
    int fam = 0;
    for (unsigned i = 0; i < nslot; i++) {
        uint64_t v = tbl[i];
        if (!v) continue;
        nz++;
        const char *mod = 0; uint64_t off = 0;
        if (!runtime_module_lookup(v, &mod, &off)) { n_oth++; continue; }
        if (!strcmp(mod, "sdk")) { n_sdk++;
            if (off >= 0x324a00ull && off <= 0x326600ull) {
                if (fam < 24)
                    { vk_boot_logf("[m7a] got t=%us slot=%#llx -> sdk+0x%llx (os sync family)", tsec,
                                   (unsigned long long)(lo + (uint64_t)i * 8), (unsigned long long)off); }
                fam++;
            }
        } else if (!strcmp(mod, "main")) n_main++;
        else n_oth++;
    }
    vk_boot_logf("[m7a] got t=%us digest slots=%u nonzero=%u sdk=%u main=%u oth=%u sync_family=%d", tsec, nslot, nz, n_sdk, n_main, n_oth, fam);
    char b[720];
    unsigned shown = 0;
    for (unsigned i = 0; i < nslot && shown < 480; ) {
        int o = snprintf(b, sizeof b, "[m7a] gottbl t=%us", tsec);
        for (int k = 0; k < 6 && i < nslot && shown < 480; k++, i++) {
            uint64_t v = tbl[i];
            const char *mod = 0; uint64_t off = 0;
            if (v > 0x100000000ULL && v < 0x800000000000ULL && runtime_module_lookup(v, &mod, &off))
                o += snprintf(b + o, sizeof b - (size_t)o, " %#llx=%s+0x%llx", (unsigned long long)(lo + (uint64_t)i * 8), mod, (unsigned long long)off);
            else if (v)
                o += snprintf(b + o, sizeof b - (size_t)o, " %#llx=%#llx", (unsigned long long)(lo + (uint64_t)i * 8), (unsigned long long)v);
            if (v) shown++;
        }
        if (o > (int)sizeof b - 8) o = (int)sizeof b - 8;
        b[o > 0 ? o : 0] = 0;
        vk_boot_logf("%s", b);
    }
    if (shown < nz) vk_boot_logf("[m7a] gottbl t=%us truncated (%u of %u shown)", tsec, shown, nz);
}
/* [m7a] kick: one-shot TrySend emulation + svc-0x1D-style wake on a census-confirmed stranded slot */
static void m7a_kick(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    static int fired;
    if (fired) return;
    fired = 1;
    if (!g_m7a_mqc_n) { vk_boot_logf("[m7a] kick t=%us no stranded census slot - not executed (task rule)", tsec); return; }
    uint64_t slot = g_m7a_mqc[0].slot, mq = g_m7a_mqc[0].mq;
    unsigned st = g_m7a_mqc[0].state;
    uint64_t mqw[4];
    if (mq <= 0x100000000ULL || mq >= 0x800000000000ULL || !runtime_safe_read_ungated(mqw, mq + 0x20, sizeof mqw)) {
        vk_boot_logf("[m7a] kick t=%us slot=%#llx mq unreadable - aborted", tsec, (unsigned long long)slot);
        return;
    }
    uint64_t buf = mqw[0]; unsigned mcap = (unsigned)(uint32_t)(mqw[1] & 0xffffffffu);
    unsigned mcnt = (unsigned)(uint32_t)(mqw[1] >> 32), mhead = (unsigned)(uint32_t)mqw[2];
    if (!buf || buf <= 0x100000000ULL || buf >= 0x800000000000ULL || !mcap || mcap > 0x40u || mcnt >= mcap) {
        vk_boot_logf("[m7a] kick t=%us slot=%#llx st=%u mq={buf=%#llx cap=%u cnt=%u} guard-reject - not sent", tsec,
                     (unsigned long long)slot, st, (unsigned long long)buf, mcap, mcnt);
        return;
    }
    uint64_t pos = (mhead + mcnt) % mcap;
    /* find the parked fnC waiter (wkind 2 on the MQ mutex @mq+0x38); send + wake under G like svc 0x1D */
    int locked = 0;
    for (int i = 0; i < 50 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    if (!locked) { vk_boot_logf("[m7a] kick t=%us G-busy - aborted (one-shot spent)", tsec); return; }
    thr_t *v = 0;
    for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->waddr == mq + 0x38) { v = w; break; }
    m7a_st64(buf + 8 * pos, 1);                       /* message value 1, same as fnB sends */
    st32(mq + 0x2c, mcnt + 1);                        /* count++ - the consumer's recheck sees it */
    unsigned vh = 0; uint64_t vkey = 0;
    if (v) { vh = v->handle; vkey = v->wkey; lock_acquire_for(v, v->waddr, v->wtag); st32(v->wkey, 0); }
    pthread_mutex_unlock(&G);
    vk_boot_logf("[m7a] kick t=%us slot=%#llx st=%u mq=%#llx pos=%llu cnt=%u->%u bufw=%#llx victim=%s h=%#x wkey=%#llx",
                 tsec, (unsigned long long)slot, st, (unsigned long long)mq,
                 (unsigned long long)pos, mcnt, mcnt + 1, (unsigned long long)(buf + 8 * pos),
                 v ? "yes" : "none", vh, (unsigned long long)vkey);
}
void runtime_m7a_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7a_vec(uint64_t out[4]);
    static int ann; static double next_reg, next_60; static int got_done, kick_checked;
    atomic_store_explicit(&g_m7a_on, m7a_on(), memory_order_relaxed);
    if (!atomic_load_explicit(&g_m7a_on, memory_order_relaxed)) { ann = 0; return; }
    if (!ann) { ann = 1; vk_boot_logf("[m7a] probe on (t=%us) reg=30s trio=60s null=60s got@30s kick=switch-smo_m7a_kick@t>200", tsec); }
    if (!got_done && tsec >= 30) { got_done = 1; m7a_got(tsec); }
    if (next_reg <= 0) next_reg = (double)tsec + 30.0;
    else if ((double)tsec >= next_reg) {
        next_reg += 30.0;
        uint64_t vecs[4]; int nv = smo_obs_m7a_vec(vecs);
        if (!nv) vk_boot_logf("[m7a] reg t=%us no registry vector latched yet (gate 27 silent)", tsec);
        for (int i = 0; i < nv; i++) m7a_census_vec(tsec, vecs[i], i, tsec >= 200 ? 1 : 0);
        if (tsec >= 200 && !kick_checked) {
            kick_checked = 1;
            if (m7a_kick_on()) m7a_kick(tsec);
            else vk_boot_logf("[m7a] kick t=%us switch smo_m7a_kick=0 - not executed", tsec);
        }
    }
    if (next_60 <= 0) next_60 = (double)tsec + 60.0;
    else if ((double)tsec >= next_60) {
        next_60 += 60.0;
        m7a_trio(tsec);
        m7a_null_drain(tsec);
    }
}
/* ---- M7b (task 2026-11-07 16:45): answer "why was batch#3 never sent". Switch smo_m7b,
   C default 0; rollback = echo 0 > .../files/smo_m7b (<=2 s). Guest-side gates (game_patches.c,
   all read-only; safety scan /tmp/m7b_safety.py safety_bad=0):
   gate 28 = poster main+0x7aca60 ENTRY (x0=batch obj {[x0+8]=pump, [x0+0x18]=dirty flag},
             x4=LR=caller site; first-32 LR ring + first/last {t,x0,flag,pump,lr}),
   gates 29..41 = the 13 static bl call sites of the poster (NEW tail=3 = single-bl gate:
   stub replays the relocated bl, callee rets to stub+0x54 = b site+4; call semantics
   identical), gate 42/43 = fnA TLS-gate TRUE 0x61570c (sync resolve, bypasses
   registry/pump) / FALSE 0x6157f0 (submit path).
   [m7b] br  - every 30 s + immediately on change: poster hit count, first/last snapshot,
               LR ring rows with the stub->site translation.
   [m7b] c13 - every 100 s: per-site cumulative counts + window deltas + rows for hit sites.
   [m7b] g56 - every 100 s: fnA TRUE/FALSE counts + first/last.
   Hot path: 16 svc-entry gates, atomics + 2 guarded reads only; no I/O, no now_ns; all
   printing here on the heartbeat thread. ---- */
#define M7B_PATH_A "/data/storage/el2/base/files/smo_m7b"
#define M7B_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7b"
__attribute__((constructor)) static void m7b_seed(void) {
    if (!fopen(M7B_PATH_A, "r")) {
        FILE *f = fopen(M7B_PATH_A, "w");
        if (!f) f = fopen(M7B_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7b] seeded = 0 (off)\n"); }
    }
}
static int m7b_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7B_PATH_B : M7B_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7b_on;                     /* game_patches.c, refreshed here every 2 s */
static const uint32_t g_m7b_sites[13] = {
    0x31219cu, 0x4b057cu, 0x4b0614u, 0x4b2d7cu, 0x4b79d0u, 0x4b7d30u, 0x4b814cu,
    0x4b8dbcu, 0x4bdf38u, 0x4bdf9cu, 0x74b788u, 0x74b9d4u, 0x74ba3cu
};
static const char *const g_m7b_site_desc[13] = {
    "guard=6c7cc0 obj=[x19+0x540]",  "guard=flag+6154b0 obj=[x19+8]",
    "guard=2xflag+6154b0 obj=[x20+0x10]", "guard=byte+0x32a obj=[x19+0x1d0]",
    "guard=6c7cc0 obj=[x19+0x108]",  "guard=6c7cc0 obj=[x19+0x28]",
    "guard=6c7cc0 obj=[x19+0x28]",   "unguarded obj=[x19+0x68]",
    "unguarded obj=[x19+8]",         "guard=flag obj=[x20+8]",
    "tail obj=[x19+0x98]",           "tail obj=[x19+0x98]",
    "tail obj=[x19+0x98]"
};
void runtime_m7b_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern uint64_t smo_obs_main_base(void);
    extern int smo_obs_m7b_br(uint64_t lr[32], uint64_t *total, uint64_t first[5], uint64_t last[5]);
    extern int smo_obs_m7b_c13(uint64_t per[13][4]);
    extern int smo_obs_m7b_g56(uint64_t per[2][3]);
    extern uint64_t smo_obs_m7b_lr_site(uint64_t lr_off);
    static int ann; static double next_c, next_br;
    static uint64_t prev_c[13]; static uint64_t printed_br_n; static int br_rows_done;
    atomic_store_explicit(&g_m7b_on, m7b_on(), memory_order_relaxed);
    if (!atomic_load_explicit(&g_m7b_on, memory_order_relaxed)) { ann = 0; return; }
    if (!ann) {
        ann = 1;
        char b[512];
        int o = snprintf(b, sizeof b, "[m7b] probe on (t=%us) br=poster0x7aca60(ring32) c13=13sites g56=TRUE/FALSE br=30s+on-change c13/g56=100s sites=", tsec);
        for (int i = 0; i < 13 && o > 0 && o < (int)sizeof b - 16; i++)
            o += snprintf(b + o, sizeof b - (size_t)o, "%s%x", i ? "," : "", g_m7b_sites[i]);
        if (o > 0 && o < (int)sizeof b) { b[o > 0 ? o : 0] = 0; vk_boot_logf("%s", b); }
    }
    /* [m7b] br: poster entry - print on change + every 30 s (detail) */
    uint64_t lr[32], tot = 0, first[5] = { 0, 0, 0, 0, 0 }, last[5] = { 0, 0, 0, 0, 0 };
    int nlr = smo_obs_m7b_br(lr, &tot, first, last);
    int changed = tot != printed_br_n;
    if (next_br <= 0) next_br = (double)tsec + 30.0;
    if (changed || (double)tsec >= next_br) {
        next_br = (double)tsec + 30.0;
        printed_br_n = tot;
        if (first[0]) {
            vk_boot_logf("[m7b] br t=%us n=%llu ring=%d first={t=%llus x0=%#llx flag=%llu pump=%#llx lr=%#llx} last={t=%llus x0=%#llx flag=%llu pump=%#llx lr=%#llx}",
                         tsec, (unsigned long long)tot, nlr,
                         (unsigned long long)(first[0] / 1000000000ull), (unsigned long long)first[1],
                         (unsigned long long)first[2], (unsigned long long)first[3], (unsigned long long)first[4],
                         (unsigned long long)(last[0] / 1000000000ull), (unsigned long long)last[1],
                         (unsigned long long)last[2], (unsigned long long)last[3], (unsigned long long)last[4]);
        } else {
            vk_boot_logf("[m7b] br t=%us n=0 (poster never called yet)", tsec);
        }
        if (!br_rows_done || changed) {          /* ring rows once per change (cap 32) */
            br_rows_done = 1;
            uint64_t mb = smo_obs_main_base();
            for (int i = 0; i < nlr; i++) {
                char w[64] = "raw";
                if (mb) {
                    uint64_t lo = lr[i] - mb;
                    uint64_t site = smo_obs_m7b_lr_site(lo);
                    if (site) snprintf(w, sizeof w, "site=main+0x%llx(+4)", (unsigned long long)site);
                    else for (int k = 0; k < 13; k++)
                        if (lo == (uint64_t)g_m7b_sites[k] + 4) { snprintf(w, sizeof w, "site=main+0x%llx(+4,ungated?!)", (unsigned long long)g_m7b_sites[k]); break; }
                }
                vk_boot_logf("[m7b] brr t=%us #%d lr=%#llx %s", tsec, i, (unsigned long long)lr[i], w);
            }
        }
    }
    /* [m7b] c13 + g56: 100 s incremental tables */
    if (next_c <= 0) next_c = (double)tsec + 100.0;
    if ((double)tsec >= next_c) {
        next_c += 100.0;
        uint64_t per[13][4], g[2][3];
        smo_obs_m7b_c13(per);
        smo_obs_m7b_g56(g);
        char b[400], d[400];
        int o = snprintf(b, sizeof b, "[m7b] c13 t=%us cum", tsec);
        uint64_t sum = 0;
        for (int i = 0; i < 13 && o > 0; i++) { sum += per[i][0]; o += snprintf(b + o, sizeof b - (size_t)o, " %d=%llu", i, (unsigned long long)per[i][0]); }
        if (o > 0 && o < (int)sizeof b) { b[o] = 0; vk_boot_logf("%s sum=%llu br_total=%llu", b, (unsigned long long)sum, (unsigned long long)tot); }
        o = snprintf(d, sizeof d, "[m7b] c13 t=%us win", tsec);
        for (int i = 0; i < 13 && o > 0; i++) { uint64_t dd = per[i][0] >= prev_c[i] ? per[i][0] - prev_c[i] : 0; o += snprintf(d + o, sizeof d - (size_t)o, " %d=+%llu", i, (unsigned long long)dd); }
        if (o > 0 && o < (int)sizeof d) { d[o] = 0; vk_boot_logf("%s", d); }
        for (int i = 0; i < 13; i++) prev_c[i] = per[i][0];
        for (int i = 0; i < 13; i++)
            if (per[i][0])
                vk_boot_logf("[m7b] c13row t=%us site=%d off=main+0x%llx n=%llu first=%llus last=%llus x0=%#llx (%s)",
                             tsec, i, (unsigned long long)g_m7b_sites[i], (unsigned long long)per[i][0],
                             (unsigned long long)(per[i][1] / 1000000000ull), (unsigned long long)(per[i][2] / 1000000000ull),
                             (unsigned long long)per[i][3], g_m7b_site_desc[i]);
        vk_boot_logf("[m7b] g56 t=%us TRUE_sync(0x61570c)=%llu(first=%llus last=%llus) FALSE_submit(0x6157f0)=%llu(first=%llus last=%llus)",
                     tsec, (unsigned long long)g[0][0],
                     g[0][1] ? (unsigned long long)(g[0][1] / 1000000000ull) : 0ull,
                     g[0][2] ? (unsigned long long)(g[0][2] / 1000000000ull) : 0ull,
                     (unsigned long long)g[1][0],
                     g[1][1] ? (unsigned long long)(g[1][1] / 1000000000ull) : 0ull,
                     g[1][2] ? (unsigned long long)(g[1][2] / 1000000000ull) : 0ull);
    }
}
/* ---- M7c (task 2026-11-07 17:20): main-side real call sites + fnA silence + loader stop point ----
   Switch smo_m7c, C default 0; rollback = echo 0 > .../files/smo_m7c (<=2 s). One new gate
   (game_patches.c): fnA 0x6156b0 entry (4-word window, task-verified + /tmp/m7c_safety.py).
   Hot path: read-only counters + guarded reads only, no I/O / now_ns / memset; all printing
   happens in runtime_m7c_poll on the heartbeat thread.
   [m7c] lr  - main's svc-entry call-site histogram (m7c_svc_note, armed from svc_thread_body):
               frame0 = [sp+8] (gate-saved caller LR -> sdk-wrapper histogram), frame1 = first
               fp-chain (<=4 hops, bad frame stops, stack-range check) address landing in main's
               code segment (off < 0x14C3000 excludes the tail stub page) -> main histogram.
               Rows carry a per-svc split (0x1A/0x1B/0x1D/0x34/0x17). 100 s print: totals +
               main top10 + sdk top5 + f0-main top5 + per-window top3 deltas.
   [m7c] fna - fnA 0x6156b0 gate ledger (game_patches.c): total + per-thread counts + last-16
               {t,thr,x1,lr} ring; 30 s status + 100 s ring rows. Silence after t>140 = the task
               acceptance; caller-LR distribution = who calls fnA.
   [m7c] ldr - 60 s: loader thread 0x1007 wk/waddr/wkey/n1c/n1d/last_svc/last_lr + describe(waddr)
               (+ the M7A census MQ match) - where the loader parks after the last open.
   [m7c] key - one-shot: nullrow k0/k1 numerically (slot+0x08 key layout side item). ---- */
#define M7C_MAIN_CODE_END 0x14C3000ull   /* main.patched.bin = 21,833,968 B = 0x14C28F0; page_up = the tail stub page */
#define M7C_PATH_A "/data/storage/el2/base/files/smo_m7c"
#define M7C_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7c"
__attribute__((constructor)) static void m7c_seed(void) {
    if (!fopen(M7C_PATH_A, "r")) {
        FILE *f = fopen(M7C_PATH_A, "w");
        if (!f) f = fopen(M7C_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7c] seeded = 0 (off)\n"); }
    }
}
static int m7c_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7C_PATH_B : M7C_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7c_on;                     /* game_patches.c (fnA gate), refreshed here every 2 s */
#define M7C_NSV 5
#define M7C_HM 160
#define M7C_HS 64
#define M7C_HF 32
typedef struct { uint64_t off; uint64_t n[M7C_NSV]; } m7c_hist_t;
static m7c_hist_t g_m7c_hm[M7C_HM];   /* frame1 main-offset histogram */
static m7c_hist_t g_m7c_hs[M7C_HS];   /* frame0 sdk-offset histogram */
static m7c_hist_t g_m7c_hf[M7C_HF];   /* frame0 main-offset histogram */
static uint64_t g_m7c_hm_ovf, g_m7c_hs_ovf, g_m7c_hf_ovf;
static uint64_t g_m7c_c[M7C_NSV];                /* per-svc totals: 0=1A 1=1B 2=1D 3=34 4=17 */
static uint64_t g_m7c_n_sp, g_m7c_n_spbad;       /* svc entries armed / unusable sp or first read */
static uint64_t g_m7c_n_f0sdk, g_m7c_n_f0main, g_m7c_n_f0miss;   /* frame0 module coverage */
static uint64_t g_m7c_n_f1ok, g_m7c_n_f1miss;    /* fp-chain frame1 coverage */
/* main only (handle 0x1001) -> single-writer plain stores; the dump side reads them with the
   usual disclosed observation semantics. runtime_module_lookup walks the fixed boot-time module
   table (stable after mapping; same helper the fault path uses). */
static void m7c_hist_add(m7c_hist_t *tbl, int cap, uint64_t *ovf, uint64_t off, int si) {
    for (int i = 0; i < cap; i++) {
        if (tbl[i].off == off) { tbl[i].n[si]++; return; }
        if (!tbl[i].off) { tbl[i].off = off; tbl[i].n[si] = 1; return; }
    }
    (*ovf)++;
}
static void m7c_svc_note(int num, const uint64_t *regs, const thr_t *me) {
    int si = num == 0x1a ? 0 : num == 0x1b ? 1 : num == 0x1d ? 2 : num == 0x34 ? 3 : num == 0x17 ? 4 : -1;
    if (si < 0) return;
    g_m7c_c[si]++;
    g_m7c_n_sp++;
    uint64_t sp = regs[31];
    if (sp < 0x100000000ull || (sp & 7u)) { g_m7c_n_spbad++; return; }
    uint64_t w[2] = { 0, 0 };
    if (!runtime_safe_read_ungated(w, sp, sizeof w)) { g_m7c_n_spbad++; return; }
    uint64_t f0 = w[1];
    const char *mod; uint64_t off;
    if (f0 && runtime_module_lookup(f0, &mod, &off)) {
        if (!strcmp(mod, "sdk")) { m7c_hist_add(g_m7c_hs, M7C_HS, &g_m7c_hs_ovf, off, si); g_m7c_n_f0sdk++; }
        else if (!strcmp(mod, "main") && off < M7C_MAIN_CODE_END) { m7c_hist_add(g_m7c_hf, M7C_HF, &g_m7c_hf_ovf, off, si); g_m7c_n_f0main++; }
        else g_m7c_n_f0miss++;
    } else g_m7c_n_f0miss++;
    /* frame1: first fp-chain address in main's code segment (<=4 hops; a bad frame stops the
       walk; fp must sit inside the thread's guest stack span when stack_top is known) */
    uint64_t fp = regs[29], top = me->stack_top;
    for (int hop = 0; hop < 4; hop++) {
        if (!fp || (fp & 7u)) break;
        if (top && (fp > top || fp < top - 0x1000000ull)) break;
        uint64_t fw[2] = { 0, 0 };
        if (!runtime_safe_read_ungated(fw, fp, sizeof fw)) break;
        uint64_t ret = fw[1];
        if (ret && !(ret & 3u) && runtime_module_lookup(ret, &mod, &off) &&
            !strcmp(mod, "main") && off < M7C_MAIN_CODE_END) {
            m7c_hist_add(g_m7c_hm, M7C_HM, &g_m7c_hm_ovf, off, si);
            g_m7c_n_f1ok++;
            return;
        }
        fp = fw[0];
    }
    g_m7c_n_f1miss++;
}
void runtime_m7c_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern uint64_t smo_obs_main_base(void);
    extern int smo_obs_m7c_fna(uint64_t rows[16][4], uint64_t *total, uint64_t thr[8][2],
                               uint64_t *ovf, uint64_t first[2], uint64_t last[4]);
    extern int smo_obs_m7a_null_rows(uint64_t out[16][12], uint64_t *total, uint64_t *n_null, uint64_t *n_res);
    static int ann; static int key_done;
    static double next_status, next_hist, next_ldr;
    typedef struct { uint64_t off; uint64_t tot; } m7c_snap_t;
    static m7c_snap_t prev_hm[M7C_HM], prev_hs[M7C_HS], prev_hf[M7C_HF];
    atomic_store_explicit(&g_m7c_on, m7c_on(), memory_order_relaxed);
    if (!atomic_load_explicit(&g_m7c_on, memory_order_relaxed)) { ann = 0; return; }
    if (!ann) { ann = 1; vk_boot_logf("[m7c] probe on (t=%us) lr=svc{1a,1b,1d,34,17}@main hist=100s fna=0x6156b0(status30s+ring100s) ldr=0x1007@60s key=one-shot", tsec);
                next_status = (double)tsec + 30.0; next_hist = (double)tsec + 100.0; next_ldr = (double)tsec + 60.0; return; }
    /* [m7c] fna status every 30 s: total + first/last + per-thread counts */
    if ((double)tsec >= next_status) {
        next_status += 30.0;
        uint64_t rows[16][4], thr[8][2], ovf = 0, first[2] = { 0, 0 }, last[4] = { 0, 0, 0, 0 };
        uint64_t tot2 = 0;
        (void)smo_obs_m7c_fna(rows, &tot2, thr, &ovf, first, last);
        char b[512]; int o = snprintf(b, sizeof b, "[m7c] fna t=%us tot=%llu first=%llus last=%llus",
                     tsec, (unsigned long long)tot2,
                     (unsigned long long)(first[0] / 1000000000ull),
                     (unsigned long long)(last[0] / 1000000000ull));
        uint64_t psum = 0;
        for (int i = 0; i < 8 && o > 0; i++)
            if (thr[i][0]) { psum += thr[i][1];
                o += snprintf(b + o, sizeof b - (size_t)o, " h%#llx=%llu", (unsigned long long)thr[i][0], (unsigned long long)thr[i][1]); }
        if (o > 0 && o < (int)sizeof b) { b[o] = 0;
            vk_boot_logf("%s pt_sum=%llu pt_ovf=%llu last_thr=%#llx last_lr=%#llx", b,
                         (unsigned long long)psum, (unsigned long long)ovf,
                         (unsigned long long)last[1], (unsigned long long)last[3]); }
    }
    /* [m7c] ldr every 60 s: where is the loader thread parked (the task's stop-point acceptance) */
    if ((double)tsec >= next_ldr) {
        next_ldr += 60.0;
        thr_t *t = 0; int locked = 0;
        for (int i = 0; i < 50 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
        if (!locked) { vk_boot_logf("[m7c] ldr t=%us G-busy", tsec); }
        else {
            for (thr_t *q = g_all; q; q = q->all) if (q->handle == 0x1007u && !q->exited) { t = q; break; }
            if (!t) vk_boot_logf("[m7c] ldr t=%us h=0x1007 absent", tsec);
            else {
                unsigned n1c = t->svch ? t->svch[0x1c] : 0, n1d = t->svch ? t->svch[0x1d] : 0;
                uint64_t waddr = t->waddr, wkey = t->wkey; int wk = t->wkind;
                unsigned nsvc = (unsigned)t->nsvc; unsigned last_svc = t->last_svc;
                uint64_t last_lr = t->last_lr;
                pthread_mutex_unlock(&G);
                char d1[48], d2[48], dl[48]; d1[0] = 0; d2[0] = 0; dl[0] = 0;
                if (waddr) runtime_describe_address(waddr, d1, sizeof d1);
                if (wkey) runtime_describe_address(wkey, d2, sizeof d2);
                if (last_lr) runtime_describe_address(last_lr, dl, sizeof dl);
                const char *mt = "-";
                for (unsigned i = 0; i < g_m7a_mqc_n; i++) {
                    uint64_t mq = g_m7a_mqc[i].mq;
                    if (waddr == mq + 0x38) mt = "fnC-mutex";
                    else if (wkey == mq + 0x40) mt = "fnC-cv";
                    else if (waddr >= mq && waddr < mq + 0x48) mt = "fnC-mq";
                }
                vk_boot_logf("[m7c] ldr t=%us h=0x1007 wk=%d waddr=%#llx(%s) wkey=%#llx(%s) match=%s n1c=%u n1d=%u nsvc=%u last_svc=%#x last_lr=%s",
                             tsec, wk, (unsigned long long)waddr, d1[0] ? d1 : "-",
                             (unsigned long long)wkey, d2[0] ? d2 : "-", mt, n1c, n1d, nsvc, last_svc,
                             dl[0] ? dl : "-");
            }
        }
        /* [m7c] key one-shot: nullrow k0/k1 numerically (slot+0x08 key layout, side item) */
        if (!key_done && tsec >= 60) {
            key_done = 1;
            uint64_t rows[16][12], tot = 0, nn = 0, nr2 = 0;
            int n = smo_obs_m7a_null_rows(rows, &tot, &nn, &nr2);
            char b[1600]; int o = snprintf(b, sizeof b, "[m7c] key t=%us rows=%d tot=%llu null=%llu res=%llu k0k1:", tsec, n,
                                          (unsigned long long)tot, (unsigned long long)nn, (unsigned long long)nr2);
            for (int i = 0; i < n && o > 0 && o < (int)sizeof b - 64; i++)
                o += snprintf(b + o, sizeof b - (size_t)o, " #%d k0=%016llx k1=%016llx", i,
                              (unsigned long long)rows[i][4], (unsigned long long)rows[i][5]);
            if (o > 0 && o < (int)sizeof b) { b[o] = 0; vk_boot_logf("%s", b); }
        }
    }
    /* [m7c] lr tables every 100 s: cumulative top lists + per-window deltas (prev tables
       matched by offset; row offsets never move once inserted) */
    if ((double)tsec >= next_hist) {
        next_hist += 100.0;
        vk_boot_logf("[m7c] lr t=%us armed=%llu spbad=%llu f0: sdk=%llu main=%llu miss=%llu f1: ok=%llu miss=%llu svc: 1a=%llu 1b=%llu 1d=%llu 34=%llu 17=%llu",
                     tsec, (unsigned long long)g_m7c_n_sp, (unsigned long long)g_m7c_n_spbad,
                     (unsigned long long)g_m7c_n_f0sdk, (unsigned long long)g_m7c_n_f0main,
                     (unsigned long long)g_m7c_n_f0miss, (unsigned long long)g_m7c_n_f1ok,
                     (unsigned long long)g_m7c_n_f1miss, (unsigned long long)g_m7c_c[0],
                     (unsigned long long)g_m7c_c[1], (unsigned long long)g_m7c_c[2],
                     (unsigned long long)g_m7c_c[3], (unsigned long long)g_m7c_c[4]);
        struct tblsnap { m7c_hist_t *t; int cap; uint64_t ovf; m7c_snap_t *prev; const char *tag; const char *pfx; };
        struct tblsnap sn[3] = {
            { g_m7c_hm, M7C_HM, g_m7c_hm_ovf, prev_hm, "lrmain", "main" },
            { g_m7c_hs, M7C_HS, g_m7c_hs_ovf, prev_hs, "lrsdk", "sdk" },
            { g_m7c_hf, M7C_HF, g_m7c_hf_ovf, prev_hf, "lrf0m", "main" },
        };
        for (int s = 0; s < 3; s++) {
            int cap = sn[s].cap, topn = s == 0 ? 10 : 5;
            /* top-K by total (selection, no sort) */
            int idx[12]; uint64_t val[12];
            int used = 0;
            for (int i = 0; i < cap; i++) {
                uint64_t tot = 0; for (int k = 0; k < M7C_NSV; k++) tot += sn[s].t[i].n[k];
                if (!sn[s].t[i].off || !tot) continue;
                if (used < topn) { idx[used] = i; val[used] = tot; used++; }
                else { int mi = 0; for (int k = 1; k < used; k++) if (val[k] < val[mi]) mi = k;
                       if (tot > val[mi]) { idx[mi] = i; val[mi] = tot; } }
            }
            char b[1200]; int o = snprintf(b, sizeof b, "[m7c] %s t=%us top%d n=%d ovf=%llu:", sn[s].tag, tsec, topn, used, (unsigned long long)sn[s].ovf);
            for (int k = 0; k < used && o > 0; k++) {
                uint64_t off = sn[s].t[idx[k]].off;
                if (!strcmp(sn[s].pfx, "main"))
                    o += snprintf(b + o, sizeof b - (size_t)o, "%s main+0x%llx x%llu(1a%llu,1b%llu,1d%llu,34%llu,17%llu)", k ? "," : "",
                                  (unsigned long long)off, (unsigned long long)val[k],
                                  (unsigned long long)sn[s].t[idx[k]].n[0], (unsigned long long)sn[s].t[idx[k]].n[1],
                                  (unsigned long long)sn[s].t[idx[k]].n[2], (unsigned long long)sn[s].t[idx[k]].n[3],
                                  (unsigned long long)sn[s].t[idx[k]].n[4]);
                else
                    o += snprintf(b + o, sizeof b - (size_t)o, "%s sdk+0x%llx x%llu(1d%llu,34%llu)", k ? "," : "",
                                  (unsigned long long)off, (unsigned long long)val[k],
                                  (unsigned long long)sn[s].t[idx[k]].n[2], (unsigned long long)sn[s].t[idx[k]].n[3]);
            }
            if (o > 0 && o < (int)sizeof b) { b[o] = 0; vk_boot_logf("%s", b); }
            /* per-window top3 deltas (matched by offset against the previous snapshot) */
            int didx[3]; uint64_t dval[3]; int dused = 0;
            for (int i = 0; i < cap; i++) {
                uint64_t tot = 0; for (int k = 0; k < M7C_NSV; k++) tot += sn[s].t[i].n[k];
                if (!sn[s].t[i].off || !tot) continue;
                uint64_t p = 0;
                for (int j = 0; j < cap; j++) if (sn[s].prev[j].off == sn[s].t[i].off) { p = sn[s].prev[j].tot; break; }
                uint64_t d = tot >= p ? tot - p : 0;
                if (dused < 3) { didx[dused] = i; dval[dused] = d; dused++; }
                else { int mi = 0; for (int k = 1; k < 3; k++) if (dval[k] < dval[mi]) mi = k;
                       if (d > dval[mi]) { didx[mi] = i; dval[mi] = d; } }
            }
            char d[400]; int od = snprintf(d, sizeof d, "[m7c] lrwin t=%us %s top3:", tsec, sn[s].pfx);
            for (int k = 0; k < dused && od > 0; k++)
                od += snprintf(d + od, sizeof d - (size_t)od, " %s+0x%llx +%llu", sn[s].pfx,
                               (unsigned long long)sn[s].t[didx[k]].off, (unsigned long long)dval[k]);
            if (od > 0 && od < (int)sizeof d) { d[od] = 0; vk_boot_logf("%s", d); }
            /* advance the snapshot */
            for (int i = 0; i < cap; i++) {
                sn[s].prev[i].off = sn[s].t[i].off;
                uint64_t tot = 0; for (int k = 0; k < M7C_NSV; k++) tot += sn[s].t[i].n[k];
                sn[s].prev[i].tot = tot;
            }
        }
        /* fna ring rows (last-16 by claim order) once per 100 s window */
        {
            uint64_t rows[16][4], thr[8][2], ovf = 0, first[2] = { 0, 0 }, last[4] = { 0, 0, 0, 0 };
            uint64_t tot3 = 0;
            int nr = smo_obs_m7c_fna(rows, &tot3, thr, &ovf, first, last);
            vk_boot_logf("[m7c] fnaring t=%us rows=%d (last-16 by claim)", tsec, nr);
            for (int i = 0; i < nr; i++) {
                char dl[48]; dl[0] = 0;
                if (rows[i][3]) runtime_describe_address(rows[i][3], dl, sizeof dl);
                vk_boot_logf("[m7c] fnarow t=%us #%d ns=%llus thr=%#llx x1=%#llx lr=%s", tsec, i,
                             (unsigned long long)(rows[i][0] / 1000000000ull),
                             (unsigned long long)rows[i][1], (unsigned long long)rows[i][2],
                             dl[0] ? dl : "-");
            }
        }
    }
}
/* ---- M7e (task 2026-11-07 18:35): name the CREATE layer ----
   Switch smo_m7e, C default 0; rollback = echo 0 > .../files/smo_m7e (<=2 s). One new gate
   (game_patches.c): create-request function entry main+0x746020 (4-word window; head pinned
   by capstone after the task's ret@0x746014 + udf padding 0x746018/0x74601c; /tmp/m7e_safety.py
   safety_bad=0; exactly one static caller `bl @0x74897c`, zero adrp+add materialisations).
   The gate's x4 = LR = the creator function's return address, so this round answers "who
   still builds requests, and when did it stop".
   [m7e] th    - every 60 s from t=2 (t=2/62/122/182/242/...): total + first/last tick and
                 per-thread create counts (pt[8] + overflow).
   [m7e] lrd   - same beat: distinct creator LR slots with counts + describe.
   [m7e] crt   - every 30 s: total, ring fill, first/last {t, x0, lr} + distinct-LR count.
   [m7e] crtrow- last-32 ring rows in chronological order, on change (>=60 s apart, hard cap
                 8 prints): idx / t / thread / x0 / lr / describe(lr).
   FnA baseline (1237) stays on gate 44 via smo_m7c - reused, not rebuilt.
   Hot path: atomics + fixed-size writes only, no I/O. Printing all on the heartbeat thread. */
#define M7E_PATH_A "/data/storage/el2/base/files/smo_m7e"
#define M7E_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7e"
__attribute__((constructor)) static void m7e_seed(void) {
    if (!fopen(M7E_PATH_A, "r")) {
        FILE *f = fopen(M7E_PATH_A, "w");
        if (!f) f = fopen(M7E_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7e] seeded = 0 (off)\n"); }
    }
}
static int m7e_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7E_PATH_B : M7E_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7e_on;                     /* game_patches.c (create-request gate) */
void runtime_m7e_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7e_crt(uint64_t rows[32][4], int *nrows, uint64_t *total,
                               uint64_t thr[8][2], uint64_t *ptovf, uint64_t first[4],
                               uint64_t last[4], uint64_t lrd[8][2], uint64_t *lrovf);
    static int ann; static double next_th, next_crt, last_rows_t;
    static uint64_t printed_tot; static int ring_prints;
    atomic_store_explicit(&g_m7e_on, m7e_on(), memory_order_relaxed);
    if (!atomic_load_explicit(&g_m7e_on, memory_order_relaxed)) { ann = 0; return; }
    uint64_t rows[32][4], thr[8][2], ptovf = 0, first[4] = { 0, 0, 0, 0 }, last[4] = { 0, 0, 0, 0 };
    uint64_t lrd[8][2], lrovf = 0, tot = 0;
    int nrows = 0;
    int nlr = smo_obs_m7e_crt(rows, &nrows, &tot, thr, &ptovf, first, last, lrd, &lrovf);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m7e] probe on (t=%us) crt=main+0x746020(entry/4w) th=60s-from-t2 crt=30s ring=last32 lrd=60s fna=gate44(smo_m7c)", tsec);
        next_th = (double)tsec + 2.0; next_crt = (double)tsec + 30.0;
    }
    /* [m7e] th: total + first/last + per-thread create counts (t=2/62/122/182/242/...). */
    if ((double)tsec >= next_th) {
        next_th += 60.0;
        char b[512];
        int o = snprintf(b, sizeof b, "[m7e] th t=%us tot=%llu first=%llus last=%llus", tsec,
                         (unsigned long long)tot,
                         first[0] ? (unsigned long long)(first[0] / 1000000000ull) : 0ull,
                         last[0] ? (unsigned long long)(last[0] / 1000000000ull) : 0ull);
        uint64_t psum = 0;
        for (int i = 0; i < 8 && o > 0 && o < (int)sizeof b - 48; i++)
            if (thr[i][0]) { psum += thr[i][1];
                o += snprintf(b + o, sizeof b - (size_t)o, " h%#llx=%llu",
                              (unsigned long long)thr[i][0], (unsigned long long)thr[i][1]); }
        if (o > 0 && o < (int)sizeof b) { b[o] = 0;
            vk_boot_logf("%s pt_sum=%llu pt_ovf=%llu last_thr=%#llx last_lr=%#llx", b,
                         (unsigned long long)psum, (unsigned long long)ptovf,
                         (unsigned long long)last[1], (unsigned long long)last[3]); }
        /* [m7e] lrd: distinct creator LR slots with counts + describe (top-8 = capacity). */
        for (int i = 0; i < nlr; i++) {
            char dl[64]; dl[0] = 0;
            if (lrd[i][0]) runtime_describe_address(lrd[i][0], dl, sizeof dl);
            vk_boot_logf("[m7e] lrd t=%us #%d lr=%#llx n=%llu desc=%s", tsec, i,
                         (unsigned long long)lrd[i][0], (unsigned long long)lrd[i][1],
                         dl[0] ? dl : "-");
        }
        if (nlr) vk_boot_logf("[m7e] lrd t=%us distinct=%d ovf=%llu", tsec, nlr, (unsigned long long)lrovf);
    }
    /* [m7e] crt: 30 s status (tot / distinct LR / first / last). */
    if ((double)tsec >= next_crt) {
        next_crt += 30.0;
        char df[64], dl[64]; df[0] = 0; dl[0] = 0;
        if (first[2]) runtime_describe_address(first[2], df, sizeof df);
        if (last[3]) runtime_describe_address(last[3], dl, sizeof dl);
        vk_boot_logf("[m7e] crt t=%us tot=%llu distinct_lr=%d first={t=%llus x0=%#llx lr=%#llx(%s) thr=%#llx} last={t=%llus x0=%#llx lr=%#llx(%s) thr=%#llx}",
                     tsec, (unsigned long long)tot, nlr,
                     first[0] ? (unsigned long long)(first[0] / 1000000000ull) : 0ull,
                     (unsigned long long)first[1], (unsigned long long)first[2], df[0] ? df : "-",
                     (unsigned long long)first[3],
                     last[0] ? (unsigned long long)(last[0] / 1000000000ull) : 0ull,
                     (unsigned long long)last[2], (unsigned long long)last[3], dl[0] ? dl : "-",
                     (unsigned long long)last[1]);
    }
    /* [m7e] crtrow: last-32 ring rows, chronological, on change (>=60 s apart, cap 8 prints). */
    if (tot != printed_tot && ring_prints < 8 &&
        (ring_prints == 0 || (double)tsec - last_rows_t >= 60.0)) {
        printed_tot = tot; ring_prints++; last_rows_t = (double)tsec;
        vk_boot_logf("[m7e] crtrow t=%us rows=%d (last-32 overwrite, tot=%llu)", tsec, nrows,
                     (unsigned long long)tot);
        for (int i = 0; i < nrows; i++) {
            char dl[64]; dl[0] = 0;
            if (rows[i][3]) runtime_describe_address(rows[i][3], dl, sizeof dl);
            vk_boot_logf("[m7e] crtrow t=%us #%d ns=%llus thr=%#llx x0=%#llx lr=%#llx desc=%s", tsec, i,
                         (unsigned long long)(rows[i][0] / 1000000000ull),
                         (unsigned long long)rows[i][1], (unsigned long long)rows[i][2],
                         (unsigned long long)rows[i][3], dl[0] ? dl : "-");
        }
    }
}
/* ---- M7i (task 2026-11-08 00:46): the DEVICE's loader (0x1007) stopped asking for files ----
   Two switches, both C default 0; rollback = `echo 0 > .../files/smo_ldrd` (`smo_aud`) <= 2 s.
   What was already answered without new code (M6k/M6l/M6n, always on, same run):
     [lp] poster role=load -> 0x1007 parks in svc 0x1C on cv 0x5f926d2368 (lock 0x5f926d2360),
       n1c=10 / n1d=12 / n1dk=9 -- one wait never woken, last signal ~t=30 s, nsvc frozen at 155;
     [wg] key -> cls=PARKED chain=h0x1026>h0x1001, and [wg] sum ring=0 on every sample (no cycle);
     [wg] side by side -> the only thread that differs from the emulator is 0x1025 (poster, the
       thread M6l named "signed the load key 1068 of 1145 times"): 2 SVCs on the device vs 1145 on
       the emulator, parked on a cv that was NEVER signalled in the whole device boot (cls=ZERO).
   What is still missing, and what this probe adds:
     (1) the per-open list. romfs.c's ring was 8 slots, so only the last 8 opens were ever visible;
         it is now 2048 slots carrying {seq, ns, owning thread}, and this probe drains it once,
         oldest first: `[ldrd] open #<seq> t=<uptime>s thr=<handle> path=<...>`.
     (2) the guest STACK of the four threads that matter. M6k proved the 0x1C/0x1D LR is always the
         trampoline (sdk+0xd3f14c/0xd3f174), so "who called this wait" could not be named. entry.S
         hands the handler regs[29]/[30]/[31] = guest fp/lr/sp; case 0x1C and the 0x34 blocking arm
         now store that triple on the thread, and this probe walks the guest frame chain from it:
         `[ldrd] stk t=.. tag=.. #0 main+0x... lr=.. fp=..` (falling back to a 1 KB stack scan).
     (3) the words of the object a thread is parked on, so the report can quote them.
   Snapshots at t = 10/20/30/45/60/90/124/150/180/240/300 s, threads main/load/poster/feed.
   smo_aud is the same snapshot machinery aimed at the audio thread 0x1020 (why the device never
   opened RsBgmTitle.nk.48.dspadspcm.bfstm): its io ledger + park point every 30 s. */
#define LDRD_PATH_A "/data/storage/el2/base/files/smo_ldrd"
#define LDRD_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_ldrd"
#define AUD_PATH_A  "/data/storage/el2/base/files/smo_aud"
#define AUD_PATH_B  "/data/app/el2/100/base/com.vibecc.smo/files/smo_aud"
__attribute__((constructor)) static void m7i_seed(void) {
    const char *pa[2] = { LDRD_PATH_A, AUD_PATH_A }, *pb[2] = { LDRD_PATH_B, AUD_PATH_B };
    for (int k = 0; k < 2; k++) {
        if (fopen(pa[k], "r")) continue;
        FILE *f = fopen(pa[k], "w");
        if (!f) f = fopen(pb[k], "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7i] seeded = 0 (off)\n"); }
    }
}
static int m7i_switch_on(const char *pa, const char *pb) {
    char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        if ((f = fopen(i ? pb : pa, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                             return n > 0 && b[0] == '1'; }
    }
    return 0;
}
extern unsigned runtime_romfs_open_ring_dump(unsigned long long *cursor, unsigned max_lines);
extern int runtime_module_lookup(uint64_t, const char **, uint64_t *);   /* loader.c: guest image table */
typedef struct { int found, wk, ns8; uint32_t s8[4];
                 uint64_t key, addr, nsvc, gsp, gfp, glr, entry, last_lr;
                 uint64_t io_open, io_read, io_bytes; char path[96]; } m7i_snap_t;
static int m7i_snap(uint32_t h, m7i_snap_t *o) {
    int locked = 0;
    memset(o, 0, sizeof *o); o->wk = -1;
    for (int i = 0; i < 50 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    if (!locked) return 0;
    for (thr_t *t = g_all; t; t = t->all) if (t->handle == h && !t->exited) {
        o->found = 1; o->wk = t->wkind; o->key = t->wkey; o->addr = t->waddr; o->nsvc = t->nsvc;
        o->gsp = t->gsp; o->gfp = t->gfp; o->glr = t->glr; o->entry = t->entry; o->last_lr = t->last_lr;
        o->io_open = atomic_load_explicit(&t->io_open, memory_order_relaxed);
        o->io_read = atomic_load_explicit(&t->io_read, memory_order_relaxed);
        o->io_bytes = atomic_load_explicit(&t->io_bytes, memory_order_relaxed);
        memcpy(o->path, t->io_path, sizeof o->path); o->path[sizeof o->path - 1] = 0;
        unsigned have = t->svc8t < 8u ? t->svc8t : 8u;
        o->ns8 = (int)(have < 4u ? have : 4u);
        for (int k = 0; k < o->ns8; k++)
            o->s8[k] = t->svc8[(t->svc8i + 8u - 1u - (unsigned)k) % 8u].num & 0xffu;   /* newest first */
        break;
    }
    pthread_mutex_unlock(&G);
    return 1;
}
/* Walk the guest frame chain from the triple entry.S recorded at the park. Every read is guarded, so a
   bogus frame pointer can only shorten the chain. If the chain is flat (guest built without frame
   pointers) fall back to scanning the first 1 KB above the guest sp for guest code addresses. */
static void m7i_stack(unsigned tsec, const char *tag, const m7i_snap_t *s) {
    uint64_t fp = s->gfp, lr = s->glr;
    int n = 0, hits = 0;
    while (n < 6 && lr) {
        const char *nm = 0; uint64_t off = 0;
        if (!runtime_module_lookup(lr, &nm, &off)) break;
        vk_boot_logf("[ldrd] stk t=%us tag=%s #%d %s+0x%llx lr=%#llx fp=%#llx", tsec, tag, n,
                     nm ? nm : "?", (unsigned long long)off, (unsigned long long)lr, (unsigned long long)fp);
        n++;
        if (!fp || (fp & 7u)) break;
        uint64_t f[2] = { 0, 0 };
        if (!runtime_safe_read_ungated(f, fp, sizeof f)) break;
        lr = f[1];
        if (f[0] <= fp || f[0] - fp > (64u << 20)) { fp = 0; continue; }   /* chain ends: print that lr once more */
        fp = f[0];
    }
    if (!n && s->gsp) {
        uint64_t w[128];
        if (runtime_safe_read_ungated(w, s->gsp, sizeof w))
            for (int i = 0; i < 128 && hits < 6; i++) {
                const char *nm = 0; uint64_t off = 0;
                if (!runtime_module_lookup(w[i], &nm, &off)) continue;
                vk_boot_logf("[ldrd] sstk t=%us tag=%s #%d %s+0x%llx w=%#llx", tsec, tag, hits,
                             nm ? nm : "?", (unsigned long long)off, (unsigned long long)w[i]);
                hits++;
            }
    }
}
void runtime_ldrd_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int runtime_module_lookup(uint64_t, const char **, uint64_t *);
    static int ann, on; static unsigned long long cur; static unsigned ti;
    static const unsigned tps[] = { 10, 20, 30, 45, 60, 90, 124, 150, 180, 240, 300 };
    static const struct { uint32_t h; const char *nm; } tv[4] = {
        { 0x1001u, "main" }, { 0x1007u, "load" }, { 0x1025u, "poster" }, { 0x1026u, "feed" } };
    on = m7i_switch_on(LDRD_PATH_A, LDRD_PATH_B);
    if (!on) { ann = 0; return; }
    if (!ann) { ann = 1;
        vk_boot_logf("[ldrd] probe on (t=%us) ring=2048 drain=96/beat stacks t=10/20/30/45/60/90/124/150/180/240/300s thr=main,load,poster,feed", tsec); }
    (void)runtime_romfs_open_ring_dump(&cur, 96);
    if (ti < sizeof tps / sizeof tps[0] && tsec >= tps[ti]) {
        unsigned at = tps[ti];
        for (int k = 0; k < 4; k++) {
            m7i_snap_t s;
            if (!m7i_snap(tv[k].h, &s)) continue;
            if (!s.found) { vk_boot_logf("[ldrd] thr t=%us tag=%s h=%#x absent", at, tv[k].nm, tv[k].h); continue; }
            char dl[64]; dl[0] = 0; if (s.last_lr) runtime_describe_address(s.last_lr, dl, sizeof dl);
            char en[64]; en[0] = 0; if (s.entry) runtime_describe_address(s.entry, en, sizeof en);
            char s8[40]; int o = 0; s8[0] = 0;
            for (int q = 0; q < s.ns8; q++) o += snprintf(s8 + o, sizeof s8 - (size_t)o, "%s%02x", q ? "," : "", s.s8[q]);
            vk_boot_logf("[ldrd] thr t=%us tag=%s h=%#x wk=%d obj=%#llx key=%#llx nsvc=%llu svc8=%s en=%s lr=%s gsp=%#llx gfp=%#llx glr=%#llx",
                         at, tv[k].nm, tv[k].h, s.wk, (unsigned long long)s.addr, (unsigned long long)s.key,
                         (unsigned long long)s.nsvc, s8, en[0] ? en : "-", dl[0] ? dl : "-",
                         (unsigned long long)s.gsp, (unsigned long long)s.gfp, (unsigned long long)s.glr);
            m7i_stack(at, tv[k].nm, &s);
            uint64_t base = s.key ? s.key : s.addr;
            if (base) {
                uint32_t w[4] = { 0, 0, 0, 0 }; int got = 0;
                for (int q = 0; q < 4; q++) got += dump_read_u32(base + 4u * (uint64_t)q, &w[q]);
                vk_boot_logf("[ldrd] obj t=%us tag=%s base=%#llx w=%08x,%08x,%08x,%08x got=%d", at, tv[k].nm,
                             (unsigned long long)base, w[0], w[1], w[2], w[3], got);
            }
        }
        ti++;
    }
}
void runtime_aud_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    static int ann, on; static double next;
    on = m7i_switch_on(AUD_PATH_A, AUD_PATH_B);
    if (!on) { ann = 0; return; }
    if (!ann) { ann = 1; next = (double)tsec + 30.0;
        vk_boot_logf("[aud] probe on (t=%us) audio thread 0x1020 ledger + park point every 30 s", tsec); }
    if ((double)tsec < next) return;
    next += 30.0;
    static const struct { uint32_t h; const char *nm; } av[2] = { { 0x1020u, "aud" }, { 0x1023u, "aud2" } };
    for (int k = 0; k < 2; k++) {
        m7i_snap_t s;
        if (!m7i_snap(av[k].h, &s)) continue;
        if (!s.found) { vk_boot_logf("[aud] thr t=%us tag=%s h=%#x absent", tsec, av[k].nm, av[k].h); continue; }
        char dl[64]; dl[0] = 0; if (s.last_lr) runtime_describe_address(s.last_lr, dl, sizeof dl);
        char en[64]; en[0] = 0; if (s.entry) runtime_describe_address(s.entry, en, sizeof en);
        vk_boot_logf("[aud] thr t=%us tag=%s h=%#x wk=%d key=%#llx obj=%#llx nsvc=%llu open=%llu read=%llu bytes=%llu en=%s lr=%s last=%s",
                     tsec, av[k].nm, av[k].h, s.wk, (unsigned long long)s.key, (unsigned long long)s.addr,
                     (unsigned long long)s.nsvc, (unsigned long long)s.io_open, (unsigned long long)s.io_read,
                     (unsigned long long)s.io_bytes, en[0] ? en : "-", dl[0] ? dl : "-", s.path[0] ? s.path : "-");
        if (k == 0) m7i_stack(tsec, av[k].nm, &s);
    }
}
/* ---- M7L (task 2026-11-08 01:08): the t~150 s freeze scene, emulator line ----
   Switch smo_frz, C default 0; rollback = `echo 0 > .../files/smo_frz` (<= 2 s).
   Reuse first (see M7I's sync): the [lp] poster / [wg] key,sum probes already carry the loader's
   wait objects, so this probe only adds the three things they do not print:
     F1  every 5 s in t=95..205 s: ONE row per LIVE thread, running threads included (the [wg] table
         skips them), with {handle, host tid, wait kind, wait obj/key, last SVC, guest LR, nsvc,
         core, last host NVN op}. Two dumps' nsvc delta = "who was doing work in this window".
     F2  at every F1 point: main's guest frame chain via runtime_guest_chain_dump(0x1001) ([gst]
         chain, <=12 frames) -- nothing is walked here, the walker already exists (M5j).
     F3  lock ledger: the 4 words at main's wait object, plus the SAME words at obj-8 / obj+8
         (a cv's mutex sits 8 B below it on this SDK, see M7I's device sync), decode the tag word
         (owner handle | 0x40000000 waiters bit, threads.c:18) into an owner handle, then print what
         THAT thread is itself waiting on -> the chain the task asks for, or the honest word "0".
     A6  the same F1 snapshot also prints the handle->nsvc ledger for the 9 handles the device
         side-by-side table needs (main 0x1001, loader 0x1007, poster 0x1025, 0x1026..0x102a).
     F4  1 s frames sampler (own thread, see runtime_frz_poll): `[fz] f` lines carry both the frame
         counter and process-relative seconds (monotonic base taken in the constructor), so the
         stall start/end are readable at 1 s granularity without a 30 s rollup.
   Read-only: every snapshot copies under the usual bounded trylock, every guest read goes through
   runtime_safe_read_ungated, and all printing happens after the lock is released. */
#define FRZ_PATH_A "/data/storage/el2/base/files/smo_frz"
#define FRZ_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_frz"
static struct timespec g_frz_t0; static int g_frz_t0ok;
static _Atomic int g_frz_ft_run;
__attribute__((constructor)) static void m7l_seed(void) {
    clock_gettime(CLOCK_MONOTONIC, &g_frz_t0); g_frz_t0ok = 1;
    const char *pa[2] = { FRZ_PATH_A, FRZ_PATH_B };
    if (!fopen(pa[0], "r")) {
        FILE *f = fopen(pa[0], "w");
        if (!f) f = fopen(pa[1], "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7l] seeded = 0 (off)\n"); }
    }
}
static int frz_on(void) {
    const char *p[2] = { FRZ_PATH_A, FRZ_PATH_B }; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++)
        if ((f = fopen(p[i], "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); return n > 0 && b[0] == '1'; }
    return 0;
}
static double frz_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    if (!g_frz_t0ok) return -1;
    return (double)(ts.tv_sec - g_frz_t0.tv_sec) + (double)(ts.tv_nsec - g_frz_t0.tv_nsec) / 1e9;
}
extern unsigned long vb_frames_now(void) __attribute__((weak));
typedef struct { uint32_t h, svc, wh0, wh1; int htid, wk, core, op, nwh; uint64_t obj, key, lr, fp, nsvc, op_ns; } frz_row_t;
#define FRZ_MAX 96
static const struct { uint32_t h; const char *nm; } frz_hv[] = {
    { 0x1001u, "main" }, { 0x1007u, "load" }, { 0x1025u, "poster" }, { 0x1026u, "h1026" },
    { 0x1027u, "h1027" }, { 0x1028u, "h1028" }, { 0x1029u, "h1029" }, { 0x102au, "h102a" },
    { 0x1020u, "aud" }, { 0x1023u, "aud2" } };
#define FRZ_NH ((int)(sizeof frz_hv / sizeof frz_hv[0]))
/* one shot of every live thread (running ones included) */
static unsigned frz_snap(frz_row_t *v, unsigned cap) {
    unsigned n = 0; int locked = 0;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    if (locked) {
        for (thr_t *t = g_all; t && n < cap; t = t->all) {
            if (t->exited) continue;
            frz_row_t *r = &v[n++];
            r->h = t->handle; r->htid = t->htid; r->wk = t->wkind; r->core = atomic_load_explicit(&t->cur_core, memory_order_relaxed);
            r->obj = t->waddr; r->key = t->wkey; r->lr = t->last_lr; r->fp = t->last_fp;
            r->nsvc = t->nsvc; r->svc = (uint32_t)t->last_svc; r->nwh = t->nwh;
            r->wh0 = t->wh[0]; r->wh1 = t->wh[1]; r->op = t->op;
            r->op_ns = t->op ? (now_ns() - t->op_ns) / 1000000ull : 0;
        }
        pthread_mutex_unlock(&G);
    }
    return n;
}
static uint64_t frz_ld64(uint64_t a) { uint64_t w = 0; return runtime_safe_read_ungated(&w, a, 8) ? w : 0; }
static const char *frz_nm(uint32_t h, char *b, size_t cap) {
    for (int i = 0; i < FRZ_NH; i++) if (frz_hv[i].h == h) { snprintf(b, cap, "%s", frz_hv[i].nm); return b; }
    snprintf(b, cap, "h%#x", h); return b;
}
void runtime_frz_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int runtime_module_lookup(uint64_t, const char **, uint64_t *);
    static frz_row_t v[FRZ_MAX], pv[FRZ_MAX];
    static uint64_t pnsvc[FRZ_MAX]; static uint32_t ph[FRZ_MAX]; static unsigned pn;
    static int ann; static double next;
    if (!frz_on()) { ann = 0; pn = 0; return; }
    if (!ann) {
        ann = 1; next = 100.0;
        vk_boot_logf("[fz] probe on (t=%us) F1/F2/F3/A6 every 5 s in t=100..200 s, F4 1 s frames sampler", tsec);
        atomic_store(&g_frz_ft_run, 1);
        { extern void runtime_frz_ft_start(void); runtime_frz_ft_start(); }
    }
    /* F4: the 1 s sampler lives in its own thread; nothing to do here */
    if (tsec < 100u || tsec > 205u) return;
    if ((double)tsec < next) return;
    next = (double)tsec + 5.0;
    unsigned n = frz_snap(v, FRZ_MAX);
    if (!n) { vk_boot_logf("[fz] t=%us no live thread (snapshot failed)", tsec); return; }
    /* F1: one row per live thread, running ones included (that is what [wg] drops) */
    for (unsigned i = 0; i < n; i++) {
        frz_row_t *x = &v[i];
        long long dn = -1;
        for (unsigned j = 0; j < pn; j++) if (ph[j] == x->h) { dn = (long long)(x->nsvc - pnsvc[j]); break; }
        char dl[64], nm[24]; dl[0] = 0;
        if (x->lr) runtime_describe_address(x->lr, dl, sizeof dl);
        vk_boot_logf("[fz] th t=%us h=%#x nm=%s tid=%d wk=%d obj=%#llx key=%#llx svc=%02x nsvc=%llu dnsvc=%lld "
                     "core=%d op=%d opage=%llums wh=%#x,%#x lr=%s",
                     tsec, x->h, frz_nm(x->h, nm, sizeof nm), x->htid, x->wk,
                     (unsigned long long)(x->wk ? x->obj : 0), (unsigned long long)(x->wk == 2 ? x->key : 0),
                     x->svc, (unsigned long long)x->nsvc, dn, x->core, x->op,
                     (unsigned long long)x->op_ns, x->wh0, x->wh1, dl[0] ? dl : "-");
    }
    pn = 0;
    for (unsigned i = 0; i < n && pn < FRZ_MAX; i++) { ph[pn] = v[i].h; pnsvc[pn] = v[i].nsvc; pn++; }
    /* A6: the device side-by-side ledger (same handles, same numbers, one line) */
    for (int k = 0; k < FRZ_NH; k++) {
        uint64_t ns = 0, dn = 0; int found = 0, wk = -1; uint64_t obj = 0, key = 0;
        for (unsigned i = 0; i < n; i++) if (v[i].h == frz_hv[k].h) {
            found = 1; ns = v[i].nsvc; wk = v[i].wk; obj = v[i].obj; key = v[i].key;
            for (unsigned j = 0; j < pn; j++) if (ph[j] == v[i].h && pnsvc[j] <= ns) { dn = ns - pnsvc[j]; break; }
            break;
        }
        vk_boot_logf("[fz] nsvc t=%us h=%#x nm=%s found=%d wk=%d nsvc=%llu d5=%llu obj=%#llx key=%#llx",
                     tsec, frz_hv[k].h, frz_hv[k].nm, found, wk, (unsigned long long)ns,
                     (unsigned long long)dn, (unsigned long long)obj, (unsigned long long)key);
    }
    /* F3: the lock ledger. main's wait object, its 8 B neighbours, the tag word -> owner handle,
       then what the owner is itself waiting on (one hop; a second hop is printed when it is a
       different parked thread, so a ring is visible without a graph walk). */
    for (unsigned i = 0; i < n; i++) {
        frz_row_t *x = &v[i];
        if (x->h != 0x1001u) continue;
        uint64_t base = x->key ? x->key : x->obj;
        if (!base) { vk_boot_logf("[fz] lock t=%us main wk=%d no wait object", tsec, x->wk); break; }
        for (int d = -2; d <= 2; d++) {
            uint64_t a = base + (uint64_t)(d * 8), w = frz_ld64(a);
            if (!d) {
                uint32_t lo = (uint32_t)w, own = lo & ~0x40000000u;
                char ob[24]; frz_nm(own, ob, sizeof ob);
                vk_boot_logf("[fz] lock t=%us main wk=%d base=%#llx tag=%08x own=%#x(%s) waiters=%d",
                             tsec, x->wk, (unsigned long long)base, lo,
                             (own && own != 0xffffffffu) ? own : 0, own ? ob : "-", (lo & 0x40000000u) ? 1 : 0);
                if (own) {
                    for (unsigned j = 0; j < n; j++) if (v[j].h == own) {
                        char dl2[64], ob2[24]; dl2[0] = 0;
                        if (v[j].lr) runtime_describe_address(v[j].lr, dl2, sizeof dl2);
                        vk_boot_logf("[fz] ownwait t=%us own=%#x(%s) wk=%d obj=%#llx key=%#llx nsvc=%llu lr=%s",
                                     tsec, own, frz_nm(own, ob2, sizeof ob2), v[j].wk,
                                     (unsigned long long)v[j].obj, (unsigned long long)v[j].key,
                                     (unsigned long long)v[j].nsvc, dl2[0] ? dl2 : "-");
                        char ob3[24], dl3[64]; dl3[0] = 0;
                        if (v[j].lr) runtime_describe_address(v[j].lr, dl3, sizeof dl3);
                        (void)ob3;
                        break;
                    }
                }
            } else {
                vk_boot_logf("[fz] obj t=%us base%+d=%#llx w=%016llx lo=%08x", tsec, d * 8,
                             (unsigned long long)a, (unsigned long long)w, (uint32_t)w);
            }
        }
        /* the waiters parked on the same key (who else is in this queue) */
        for (unsigned i2 = 0; i2 < n; i2++) {
            frz_row_t *y = &v[i2];
            if (y->h == 0x1001u || y->wk != 2) continue;
            if (y->key != base) continue;
            char nm2[24]; frz_nm(y->h, nm2, sizeof nm2);
            vk_boot_logf("[fz] wq t=%us key=%#llx waiter=%#x(%s) nsvc=%llu core=%d op=%d",
                         tsec, (unsigned long long)base, y->h, nm2, (unsigned long long)y->nsvc, y->core, y->op);
        }
        break;
    }
    /* F2: main's guest frame chain (existing walker, <=12 frames) */
    { extern void runtime_guest_chain_dump(unsigned h); runtime_guest_chain_dump(0x1001u); }
}
static void *frz_ft_thread(void *arg) {
    extern void vk_boot_logf(const char *fmt, ...);
    (void)arg;
    unsigned long pf = 0; int have = 0, moving = 1; double st = 0; unsigned seq = 0;
    for (;;) {
        usleep(1000000);
        if (!atomic_load(&g_frz_ft_run)) continue;
        double t = frz_s();
        unsigned long f = vb_frames_now ? vb_frames_now() : 0;
        seq++;
        /* only 1 line per second; the whole boot at 450 s is ~450 lines (~25 KB) */
        vk_boot_logf("[fz] f t=%.1f frames=%lu d=%ld", t, f, have ? (long)(f - pf) : -1L);
        if (have) {
            if (f != pf) { if (!moving) { vk_boot_logf("[fz] stall end t=%.1f frames=%lu dur=%.1f", t, f, t - st); moving = 1; } }
            else { if (moving) { st = t - 1.0; moving = 0; vk_boot_logf("[fz] stall begin t=%.1f frames=%lu", t, f); } }
        }
        pf = f; have = 1;
    }
    return NULL;
}
void runtime_frz_ft_start(void) {
    static _Atomic int started;
    if (atomic_exchange(&started, 1)) return;
    pthread_t th;
    if (pthread_create(&th, NULL, frz_ft_thread, NULL) == 0) pthread_detach(th);
}

/* ---- M7g (task 2026-11-07 23:12): the layer ABOVE the creator (0x7488f0) ----
   Switch smo_m7g, C default 0; rollback = echo 0 > .../files/smo_m7g (<=2 s). Four gates
   (game_patches.c): master fn head 0x7488f0 (union of all 3 static entry paths; its LR
   splits the bl path 0x74889c from the two tail calls), entry-A block 0x7485a8 (tail=2
   plain-word gate), entry-B block 0x748fb0 (tail=2), sibling ctor 0x7461a0 (the x3!=0 arm
   of 0x7488f0). Answers "which entry stops, and who calls it".
   [m7z] sum - every 30 s, one row per gate: tot / delta / first / last / last thread+LR /
               per-thread sum.
   [m7z] lr  - every 60 s, top-3 distinct LR per gate with describe (the driver names).
   [m7z] row - at 120 s and 240 s, last 8 ring rows per gate (the freeze tail).
   Hot path lives in game_patches.c (atomics + fixed-size writes, no deref); printing here. */
#define M7G_PATH_A "/data/storage/el2/base/files/smo_m7g"
#define M7G_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7g"
__attribute__((constructor)) static void m7g_seed(void) {
    if (!fopen(M7G_PATH_A, "r")) {
        FILE *f = fopen(M7G_PATH_A, "w");
        if (!f) f = fopen(M7G_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7z] seeded = 0 (off)\n"); }
    }
}
static int m7g_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7G_PATH_B : M7G_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7g_on;                     /* game_patches.c (4 gates above the creator) */
void runtime_m7g_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7g(unsigned gi, uint64_t rows[32][4], int *nrows, uint64_t *total,
                           uint64_t thr[8][2], uint64_t *ptovf, uint64_t first[4],
                           uint64_t last[5], uint64_t lrd[8][2], uint64_t *lrovf);
    static const char *const gn[4] = { "crt", "a", "b", "sib" };
    static double next_sum, next_lr, next_row;
    static uint64_t prev[4];
    static int ann, rp[4];
    atomic_store_explicit(&g_m7g_on, m7g_on(), memory_order_relaxed);
    if (!atomic_load_explicit(&g_m7g_on, memory_order_relaxed)) { ann = 0; return; }
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m7z] probe on (t=%us) crt=main+0x7488f0(4w) a=+0x7485a8(t2) b=+0x748fb0(t2) sib=+0x7461a0(4w) sum=30s lr=60s row=120/240s", tsec);
        next_sum = (double)tsec + 30.0; next_lr = (double)tsec + 60.0; next_row = (double)tsec + 120.0;
    }
    if ((double)tsec >= next_sum) {
        next_sum += 30.0;
        for (unsigned gi = 0; gi < 4; gi++) {
            uint64_t rows[32][4], thr[8][2], ptovf = 0, first[4] = { 0, 0, 0, 0 }, last[5] = { 0, 0, 0, 0, 0 };
            uint64_t lrd[8][2], lrovf = 0, tot = 0, psum = 0;
            int nrows = 0;
            int nl = smo_obs_m7g(gi, rows, &nrows, &tot, thr, &ptovf, first, last, lrd, &lrovf);
            for (int i = 0; i < 8; i++) psum += thr[i][1];
            vk_boot_logf("[m7z] sum t=%us g=%s tot=%llu d=%llu first=%llus last=%llus last_thr=%#llx last_lr=%#llx pt_sum=%llu/%llu nl=%d",
                         tsec, gn[gi], (unsigned long long)tot,
                         (unsigned long long)(tot - prev[gi]),
                         (unsigned long long)(first[0] / 1000000000ull),
                         (unsigned long long)(last[0] / 1000000000ull),
                         (unsigned long long)last[1], (unsigned long long)last[4],
                         (unsigned long long)psum, (unsigned long long)ptovf, nl);
            prev[gi] = tot;
        }
    }
    if ((double)tsec >= next_lr) {
        next_lr += 60.0;
        for (unsigned gi = 0; gi < 4; gi++) {
            uint64_t rows[32][4], thr[8][2], ptovf = 0, first[4] = { 0, 0, 0, 0 }, last[5] = { 0, 0, 0, 0, 0 };
            uint64_t lrd[8][2], lrovf = 0, tot = 0;
            int nrows = 0, used = 0;
            int nl = smo_obs_m7g(gi, rows, &nrows, &tot, thr, &ptovf, first, last, lrd, &lrovf);
            for (int r = 0; r < 3; r++) {
                int best = -1;
                for (int i = 0; i < nl; i++)
                    if (!(used & (1 << i)) && lrd[i][0] && (best < 0 || lrd[i][1] > lrd[best][1])) best = i;
                if (best < 0) break;
                used |= 1 << best;
                char dl[64]; dl[0] = 0;
                runtime_describe_address(lrd[best][0], dl, sizeof dl);
                vk_boot_logf("[m7z] lr t=%us g=%s #%d lr=%#llx n=%llu desc=%s", tsec, gn[gi], r,
                             (unsigned long long)lrd[best][0], (unsigned long long)lrd[best][1],
                             dl[0] ? dl : "-");
            }
            if (nl) vk_boot_logf("[m7z] lr t=%us g=%s distinct=%d ovf=%llu tot=%llu", tsec, gn[gi], nl,
                                 (unsigned long long)lrovf, (unsigned long long)tot);
        }
    }
    if ((double)tsec >= next_row) {
        next_row += 120.0;
        for (unsigned gi = 0; gi < 4; gi++) {
            uint64_t rows[32][4], thr[8][2], ptovf = 0, first[4] = { 0, 0, 0, 0 }, last[5] = { 0, 0, 0, 0, 0 };
            uint64_t lrd[8][2], lrovf = 0, tot = 0;
            int nrows = 0;
            if (rp[gi] >= 2) continue;
            (void)smo_obs_m7g(gi, rows, &nrows, &tot, thr, &ptovf, first, last, lrd, &lrovf);
            if (!nrows) continue;
            rp[gi]++;
            int lo = nrows > 8 ? nrows - 8 : 0;
            vk_boot_logf("[m7z] row t=%us g=%s tot=%llu rows %d..%d of %d (chronological tail)", tsec, gn[gi],
                         (unsigned long long)tot, lo, nrows - 1, nrows);
            for (int i = lo; i < nrows; i++) {
                char dl[64]; dl[0] = 0;
                if (rows[i][3]) runtime_describe_address(rows[i][3], dl, sizeof dl);
                vk_boot_logf("[m7z] row t=%us g=%s #%d ns=%llus thr=%#llx x0=%#llx lr=%#llx desc=%s", tsec, gn[gi], i,
                             (unsigned long long)(rows[i][0] / 1000000000ull),
                             (unsigned long long)rows[i][1], (unsigned long long)rows[i][2],
                             (unsigned long long)rows[i][3], dl[0] ? dl : "-");
            }
        }
    }
}
/* ---- M7h（2026-11-08）：批循环 main+0x4be0c8 **是怎么停的** + 查表函数 main+0x748e80 的 12 个调用者 ----
   Switch smo_m7h（C 默认 0；`echo 0 > .../files/smo_m7h` ≤2 s 回退）。10 只门见 game_patches.c 的 M7h 段。
   ★ 判定口径（本轮最容易糊过去的地方，写死在这里，不许事后改）★
     「未到达」  = 该门 tot 不增；「到达且不跳」 = 前一门增且本门（落点/落空侧）增；
     「到达且跳」 = 前一门增、本门不增。
     派生量（全是精确减法，无估计）：
       ble      = ent − tasks        ：进入时任务数<=0（b.le@0x4be1c8 直达出口）
       l2iter   = head − l2e         ：loop2 迭代次数（每次迭代从 head 0x4be294 进）
       l2exh    = exh − exh1 − ble   ：loop2 耗尽次数（EXH 0x4be30c 收了三条路：
                                       b.le@0x4be1c8 / b@0x4be260(loop1 耗尽) / loop2 b.lt 落空）
       cancel   = ret − cont − l2exh ：取消位置位次数（cbnz@0x4be2f8 跳了）
       inflight = look − ret         ：发起了查表但**没返回** ⇒ 卡在 0x7471f0 里面
       bodygap  = l2iter − look      ：进了 loop2 体但没走到查表 ⇒ 卡在拼 key 的 5 个 bl 里
     ⇒ 若最后触发门 = cont 且 cancel=0/l2exh=0/inflight=0/bodygap=0 → **调用者线程自己停了**（③）；
        若 cancel≥1 且冻结点在它之后 → **取消位置位**（①）；
        若 l2exh≥1 且冻结点在它之后 → **任务耗尽**（②）。
   [m7h2] sum     - 每 30 s，10 门各一行（tot/delta/first/last/last_thr/last_lr）
   [m7h2] verdict - 每 30 s，一行派生量 + 最后触发的门名（谁最后动过）
   [m7h2] lr      - 每 60 s，BQ(12 调用者) 与 ENT(2 调用者) 每 LR 一行：n / 窗 delta / **末次 tick** / describe
                    ⇒ 「谁先停」= 末次 tick 最早的那个（不是计数最少的那个）
   [m7h2] row     - 120 s / 240 s，BQ 与 ENT 的末 8 行环（冻结瞬间的时间序尾） */
#define M7H_NG 15   /* M7L: 15 = the 10 M7h gates + the 5 M7L pump gates (ids 50..64, index = gate-50).
                       Was 10 here while game_patches.c already had 15 -> the [m7h2] printer silently
                       truncated to 10 and pump/dp1/dp2/pdone/pexit never printed in the M7L boot. */
#define M7H_PATH_A "/data/storage/el2/base/files/smo_m7h"
#define M7H_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7h"
__attribute__((constructor)) static void m7h_seed(void) {
    if (!fopen(M7H_PATH_A, "r")) {
        FILE *f = fopen(M7H_PATH_A, "w");
        if (!f) f = fopen(M7H_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7h2] seeded = 0 (off)\n"); }
    }
}
static int m7h_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7H_PATH_B : M7H_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7h_on;                     /* game_patches.c（10 门：批循环 + 0x748e80 头） */
void runtime_m7h_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7h(unsigned gi, uint64_t rows[16][4], int *nrows, uint64_t *total,
                           uint64_t thr[8][2], uint64_t *ptovf, uint64_t first[5],
                           uint64_t last[5], uint64_t lrd[16][3], uint64_t *lrovf);
    static const char *const gn[M7H_NG] = { "bq", "ent", "tasks", "l2e", "head",
                                            "look", "ret", "cont", "exh", "exh1",
                                            /* M7L: the pump above the batch loop, nsvc/派批 points */
                                            "pump", "dp1", "dp2", "pdone", "pexit" };
    static double next_sum, next_lr, next_row;
    static uint64_t prev[M7H_NG], cur[M7H_NG], clr[M7H_NG], cthr[M7H_NG], cns[M7H_NG];
    static uint64_t lprev[16], lprev_ent[16];
    static int ann, rp;
    atomic_store_explicit(&g_m7h_on, m7h_on(), memory_order_relaxed);
    if (!atomic_load_explicit(&g_m7h_on, memory_order_relaxed)) { ann = 0; return; }
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m7h2] probe on (t=%us) bq=main+0x748e80(4w) ent=+0x4be0d0(4w) tasks=+0x4be1cc "
                     "l2e=+0x4be284 head=+0x4be294 look=+0x4be2ec ret=+0x4be2f4 cont=+0x4be2fc "
                     "exh=+0x4be30c exh1=+0x4be260 pump=+0x4bd8a0(4w) dp1=+0x4bdb98 dp2=+0x4bdbcc "
                     "pdone=+0x4bdb7c pexit=+0x4bdbd4 sum=30s verdict=30s lr=60s row=120/240s", tsec);
        next_sum = (double)tsec + 30.0; next_lr = (double)tsec + 60.0; next_row = (double)tsec + 120.0;
    }
    if ((double)tsec >= next_sum) {
        next_sum += 30.0;
        for (unsigned gi = 0; gi < M7H_NG; gi++) {
            uint64_t rows[16][4], thr[8][2], ptovf = 0, first[5] = { 0, 0, 0, 0, 0 }, last[5] = { 0, 0, 0, 0, 0 };
            uint64_t lrd[16][3], lrovf = 0, tot = 0, ps = 0;
            int nrows = 0;
            (void)smo_obs_m7h(gi, rows, &nrows, &tot, thr, &ptovf, first, last, lrd, &lrovf);
            for (int i = 0; i < 8; i++) ps += thr[i][1];
            vk_boot_logf("[m7h2] sum t=%us g=%s tot=%llu d=%llu first=%llus last=%llus last_thr=%#llx last_lr=%#llx pt=%llu/%llu",
                         tsec, gn[gi], (unsigned long long)tot, (unsigned long long)(tot - prev[gi]),
                         (unsigned long long)(first[0] / 1000000000ull),
                         (unsigned long long)(last[0] / 1000000000ull),
                         (unsigned long long)last[1], (unsigned long long)last[4],
                         (unsigned long long)ps, (unsigned long long)ptovf);
            prev[gi] = cur[gi] = tot;
            cns[gi] = last[0]; cthr[gi] = last[1]; clr[gi] = last[4];
        }
        {   /* 最后触发的门 + 派生量（同一瞬快照） */
            int lg = -1;
            for (unsigned gi = 0; gi < M7H_NG; gi++) if (cur[gi] && (lg < 0 || cns[gi] > cns[lg])) lg = (int)gi;
            long long ent = (long long)cur[1], tasks = (long long)cur[2], l2e = (long long)cur[3];
            long long head = (long long)cur[4], look = (long long)cur[5], ret = (long long)cur[6];
            long long cont = (long long)cur[7], exh = (long long)cur[8], exh1 = (long long)cur[9];
            long long ble = ent - tasks, l2iter = head - l2e;
            long long l2exh = exh - exh1 - ble;
            long long cancel = ret - cont - l2exh;
            vk_boot_logf("[m7h2] verdict t=%us last_g=%s last_ns=%llus last_thr=%#llx last_lr=%#llx "
                         "ent=%lld ble=%lld l2iter=%lld cancel=%lld l2exh=%lld inflight=%lld bodygap=%lld",
                         tsec, lg >= 0 ? gn[lg] : "-",
                         (unsigned long long)((lg >= 0 ? cns[lg] : 0) / 1000000000ull),
                         (unsigned long long)(lg >= 0 ? cthr[lg] : 0),
                         (unsigned long long)(lg >= 0 ? clr[lg] : 0),
                         ent, ble, l2iter, cancel, l2exh, look - ret, l2iter - look);
        }
    }
    if ((double)tsec >= next_lr) {
        next_lr += 60.0;
        for (unsigned gi = 0; gi <= 1 || gi == 10; gi++) {
            uint64_t rows[16][4], thr[8][2], ptovf = 0, first[5], last[5];
            uint64_t lrd[16][3], lrovf = 0, tot = 0;
            int nrows = 0, nl;
            nl = smo_obs_m7h(gi, rows, &nrows, &tot, thr, &ptovf, first, last, lrd, &lrovf);
            uint64_t *lp = gi ? lprev_ent : lprev;
            for (int i = 0; i < nl; i++) {
                char dl[64]; dl[0] = 0;
                if (lrd[i][0]) runtime_describe_address(lrd[i][0], dl, sizeof dl);
                vk_boot_logf("[m7h2] lr t=%us g=%s #%d lr=%#llx n=%llu d=%llu last=%llus desc=%s",
                             tsec, gn[gi], i, (unsigned long long)lrd[i][0],
                             (unsigned long long)lrd[i][1],
                             (unsigned long long)(lrd[i][1] - lp[i]),
                             (unsigned long long)(lrd[i][2] / 1000000000ull), dl[0] ? dl : "-");
                lp[i] = lrd[i][1];
            }
            for (int i = nl; i < 16; i++) lp[i] = 0;
            if (nl >= 16) vk_boot_logf("[m7h2] lr t=%us g=%s SLOTS FULL (16) ovf=%llu", tsec, gn[gi],
                                       (unsigned long long)lrovf);
        }
    }
    if ((double)tsec >= next_row) {
        next_row += 120.0;
        if (rp < 2) {
            rp++;
            for (unsigned gi = 0; gi <= 1 || gi == 10; gi++) {
                uint64_t rows[16][4], thr[8][2], ptovf = 0, first[5], last[5];
                uint64_t lrd[16][3], lrovf = 0, tot = 0;
                int nrows = 0;
                (void)smo_obs_m7h(gi, rows, &nrows, &tot, thr, &ptovf, first, last, lrd, &lrovf);
                if (!nrows) continue;
                int lo = nrows > 8 ? nrows - 8 : 0;
                vk_boot_logf("[m7h2] row t=%us g=%s tot=%llu rows %d..%d of %d (chronological tail)", tsec, gn[gi],
                             (unsigned long long)tot, lo, nrows - 1, nrows);
                for (int i = lo; i < nrows; i++) {
                    char dl[64]; dl[0] = 0;
                    if (rows[i][2]) runtime_describe_address(rows[i][2], dl, sizeof dl);
                    vk_boot_logf("[m7h2] row t=%us g=%s #%d ns=%llus thr=%#llx lr=%#llx x0=%#llx desc=%s",
                                 tsec, gn[gi], i, (unsigned long long)(rows[i][0] / 1000000000ull),
                                 (unsigned long long)rows[i][1], (unsigned long long)rows[i][2],
                                 (unsigned long long)rows[i][3], dl[0] ? dl : "-");
                }
            }
        }
    }
}
/* ---- M7N（2026-11-08）：**谁 post 了谁**（两只只读门 G1=工厂 / G2=PostToken） ----
   动机（M7M P6.1）：静态侧已证能把 token 投进某个 worker 自己 MQ 的只有 worker 对象的虚方法
   `vtable+0x38 = main+0x9646d0`（PostToken），且 main 是唯一持有全池指针的一侧；但"谁真的调了
   它、调的是哪个对象、什么时候"只能实测。两只门都装在函数**入口**、tail=2（只换入口 1 个字，
   stub 里原义重放后 b 回 off+4 ⇒ 函数体内没有一条 bl/blr 被覆盖或移动）。
   开关 smo_m7n：**C 默认 0**，本文件 constructor 先 seed 出 "0" 让 hdc 能写；每 2 s 刷新
   g_m7n_on。为了让**开关就绪之前**的 bootstrap 派活不丢（M7M 实测 poster 的第一次 signal 在
   guest tick ≈3633 ⇒ t≈2.2 s，正好与首次 poll 竞争），G1/G2 的前 256 次命中**无条件捕获**，
   之后严格按开关（超限后只做 1 次宽松原子加）。2 只门都是低频事件（工厂=线程数个位/十位、
   PostToken=每次派活一次），首 256 次的成本可忽略。
   输出（全部只在 dump 线程打印）：
     [m7n] probe 一行（装门地址/口径/节奏）
     [m7n] code  一次（t≈20 s）：两个门点 ±窗的内存逐字对比 —— 只有入口那 1 个字变了，
                 窗内每条 bl/blr 逐字节相同（"bl 未被改动"的运行期证明）+ 差异掩码
     [m7n] sum   每 10 s：每门 tot/首见/末见/按线程/溢出
     [m7n] lr    每 20 s：每门按**角色键控**的 distinct-LR 前 16（n / 末次 ns / describe）
     [m7n] obj   每 20 s：每门 distinct-对象前 16（对象指针 n / 首次 ns / 首次 LR）
                 ⇒ G2 的这张表就是"post 给了哪个 worker 对象"的答案；cv = 对象+0x78
     [m7n] row   t≈20 与 t≈100 s：每门首 **64** 条逐事件行（认领式、不改写，保"第一次"）
   判决口径：**只看这张按角色键控的台账**，不用 "0x1D 槽位差集"（M7M P4.3 已证它在本项目
   探针下不具判别力：真机 24%/模拟器 55% 的 signal 落在 overflow 桶）。 */
#define M7N_PATH_A "/data/storage/el2/base/files/smo_m7n"
#define M7N_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7n"
__attribute__((constructor)) static void m7n_seed(void) {
    if (!fopen(M7N_PATH_A, "r")) {
        FILE *f = fopen(M7N_PATH_A, "w");
        if (!f) f = fopen(M7N_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7n] seeded = 0 (off)\n"); }
    }
}
static int m7n_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7N_PATH_B : M7N_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7n_on;
/* 出厂镜像的窗字（main.patched.bin，xxd 逐字实测）。G1 窗 16 字含 4 条 bl（+0x1c/+0x24/
   +0x2c/+0x34）；G2 窗 28 字含 1 条 blr（+0x6c）以及 2 条 b 尾跳、1 条 ret、1 条 b.hs
   （PC 相对字，本机制不重放它）。 */
static const uint32_t m7n_g1_exp[16] = {
    0xf81c0ff7, 0xa90157f6, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd, 0xaa0103f3, 0xaa0003f4, 0x94031549,
    0xaa0003f5, 0x94031547, 0x321c03e1, 0x9403154d, 0xaa1503e0, 0x94031547, 0xaa0003f6, 0x321803e0 };
static const uint32_t m7n_g2_exp[28] = {
    0xb940d408, 0x51000d09, 0x7100093f, 0x54000042, 0xd65f03c0, 0x7100051f, 0x54000061, 0x321f03e8,
    0xb900d408, 0xf9406409, 0x9100e008, 0xaa0803e0, 0x36000081, 0xaa0903e1, 0x2a1f03e2, 0x17ffff85,
    0xaa0903e1, 0x2a1f03e2, 0x17ffff66, 0x00000000, 0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xf9400008,
    0xf9401d08, 0x12000021, 0xaa0003f3, 0xd63f0100 };
static inline int m7n_is_bl_or_blr(uint32_t w) {
    if ((w >> 26) == 0x25u) return 1;                 /* bl */
    if ((w & 0xfffffc1fu) == 0xd63f0000u) return 1;   /* blr xN */
    return 0;
}
void runtime_m7n_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7n(unsigned gi, uint64_t *total, uint64_t first[7], uint64_t last[4],
                           uint64_t rows[][8], int *nrows, uint64_t lrd[][3], int *nlr,
                           uint64_t wpd[][4], int *nwp, uint64_t thr[][2], int *nthr, uint64_t ovf[4]);
    extern uint64_t smo_obs_main_base(void);
    static const char *const gn[2] = { "g1", "g2" };
    static double next_sum, next_lr, next_obj, next_row;
    static int ann, code_done, rp;
    static uint64_t rows[2][64][8];
    atomic_store_explicit(&g_m7n_on, m7n_on(), memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m7n] probe on (t=%us) g1=main+0x964260(tail2 factory) g2=main+0x9646d0(tail2 PostToken) "
                     "sw=%d armed=always-first256 sum=10s lr/obj=20s row=20,100s code=20s", tsec,
                     (int)atomic_load_explicit(&g_m7n_on, memory_order_relaxed));
        next_sum = (double)tsec + 10.0; next_lr = (double)tsec + 20.0;
        next_obj = (double)tsec + 20.0; next_row = (double)tsec + 20.0;
    }
    if ((double)tsec >= next_sum) {
        next_sum += 10.0;
        for (unsigned gi = 0; gi < 2; gi++) {
            uint64_t tot = 0, first[7] = { 0 }, last[4] = { 0 }, lrd[16][3], wpd[16][4];
            uint64_t thr[8][2], ovf[4] = { 0 };
            int nrows = 0, nlr = 0, nwp = 0, nthr = 0;
            (void)smo_obs_m7n(gi, &tot, first, last, rows[gi], &nrows, lrd, &nlr, wpd, &nwp, thr, &nthr, ovf);
            uint64_t ps = 0; for (int i = 0; i < nthr; i++) ps += thr[i][1];
            char dl[64]; dl[0] = 0; if (first[5]) runtime_describe_address(first[5], dl, sizeof dl);
            vk_boot_logf("[m7n] sum t=%us g=%s tot=%llu first_ns=%llus first_obj=%#llx aux=%llu tok=%#llx "
                         "state=%lu lr=%#llx lr_desc=%s thr=%#x last_ns=%llus last_obj=%#llx uniq_lr=%d uniq_obj=%d "
                         "pt=%llu ovf r/lr/obj/pt=%llu/%llu/%llu/%llu",
                         tsec, gn[gi], (unsigned long long)tot,
                         (unsigned long long)(first[0] / 1000000000ull), (unsigned long long)first[1],
                         (unsigned long long)first[3], (unsigned long long)first[4],
                         (unsigned long)(first[6] >> 32), (unsigned long long)first[5], dl[0] ? dl : "-",
                         (unsigned)(first[6] & 0xffffffffu),
                         (unsigned long long)(last[0] / 1000000000ull), (unsigned long long)last[1],
                         nlr, nwp, (unsigned long long)ps,
                         (unsigned long long)ovf[0], (unsigned long long)ovf[1],
                         (unsigned long long)ovf[2], (unsigned long long)ovf[3]);
        }
    }
    if (!code_done && (double)tsec >= next_lr) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base();
        uint32_t w1[16], w2[28];
        int ok1 = mb && runtime_safe_read_ungated(w1, mb + 0x964260, sizeof w1);
        int ok2 = mb && runtime_safe_read_ungated(w2, mb + 0x9646d0, sizeof w2);
        unsigned m1 = 0, m2 = 0, ext1 = 0, ext2 = 0, bl1 = 0, bl2 = 0, bldiff = 0;
        if (ok1) for (int i = 0; i < 16; i++) {
            if (m7n_is_bl_or_blr(m7n_g1_exp[i])) bl1++;
            if (w1[i] != m7n_g1_exp[i]) { m1 |= 1u << i; if (m7n_is_bl_or_blr(m7n_g1_exp[i])) bldiff++; }
            else ext1++;
        }
        if (ok2) for (int i = 0; i < 28; i++) {
            if (m7n_is_bl_or_blr(m7n_g2_exp[i])) bl2++;
            if (w2[i] != m7n_g2_exp[i]) { m2 |= 1u << i; if (m7n_is_bl_or_blr(m7n_g2_exp[i])) bldiff++; }
            else ext2++;
        }
        vk_boot_logf("[m7n] code t=%us g1@main+0x964260 ok=%d same=%u/16 diffmask=%#x nsame=%u nbl=%u "
                     "g2@main+0x9646d0 ok=%d same=%u/28 diffmask=%#x nsame=%u nbl=%u BL_DIFF=%u "
                     "w1[0..3]=%08x %08x %08x %08x w2[0..3]=%08x %08x %08x %08x",
                     tsec, ok1, ext1, m1, (unsigned)(16 - ext1), bl1,
                     ok2, ext2, m2, (unsigned)(28 - ext2), bl2, bldiff,
                     ok1 ? w1[0] : 0, ok1 ? w1[1] : 0, ok1 ? w1[2] : 0, ok1 ? w1[3] : 0,
                     ok2 ? w2[0] : 0, ok2 ? w2[1] : 0, ok2 ? w2[2] : 0, ok2 ? w2[3] : 0);
    }
    if ((double)tsec >= next_lr) {
        next_lr += 20.0;
        for (unsigned gi = 0; gi < 2; gi++) {
            uint64_t tot = 0, first[7], last[4], lrd[16][3], wpd[16][4], thr[8][2], ovf[4];
            int nrows = 0, nlr = 0, nwp = 0, nthr = 0;
            (void)smo_obs_m7n(gi, &tot, first, last, rows[gi], &nrows, lrd, &nlr, wpd, &nwp, thr, &nthr, ovf);
            for (int i = 0; i < nlr; i++) {
                char dl[64]; dl[0] = 0;
                if (lrd[i][0]) runtime_describe_address(lrd[i][0], dl, sizeof dl);
                vk_boot_logf("[m7n] lr t=%us g=%s #%d lr=%#llx site=%#llx n=%llu last_ns=%llus desc=%s",
                             tsec, gn[gi], i, (unsigned long long)lrd[i][0],
                             (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0),
                             (unsigned long long)lrd[i][1],
                             (unsigned long long)(lrd[i][2] / 1000000000ull), dl[0] ? dl : "-");
            }
            if (nlr >= 16) vk_boot_logf("[m7n] lr t=%us g=%s SLOTS FULL ovf=%llu", tsec, gn[gi],
                                        (unsigned long long)ovf[1]);
        }
    }
    if ((double)tsec >= next_obj) {
        next_obj += 20.0;
        for (unsigned gi = 0; gi < 2; gi++) {
            uint64_t tot = 0, first[7], last[4], lrd[16][3], wpd[16][4], thr[8][2], ovf[4];
            int nrows = 0, nlr = 0, nwp = 0, nthr = 0;
            (void)smo_obs_m7n(gi, &tot, first, last, rows[gi], &nrows, lrd, &nlr, wpd, &nwp, thr, &nthr, ovf);
            for (int i = 0; i < nwp; i++) {
                char dl[64]; dl[0] = 0;
                if (wpd[i][3]) runtime_describe_address(wpd[i][3], dl, sizeof dl);
                vk_boot_logf("[m7n] obj t=%us g=%s #%d obj=%#llx n=%llu first_ns=%llus first_lr=%#llx "
                             "cv=%#llx probe_obj=%#llx desc=%s",
                             tsec, gn[gi], i, (unsigned long long)wpd[i][0],
                             (unsigned long long)wpd[i][1],
                             (unsigned long long)(wpd[i][2] / 1000000000ull),
                             (unsigned long long)wpd[i][3],
                             (unsigned long long)(wpd[i][0] ? wpd[i][0] + 0x78 : 0),
                             (unsigned long long)(wpd[i][0] ? wpd[i][0] + 0x70 : 0),
                             dl[0] ? dl : "-");
            }
            if (nwp >= 16) vk_boot_logf("[m7n] obj t=%us g=%s SLOTS FULL ovf=%llu", tsec, gn[gi],
                                        (unsigned long long)ovf[2]);
        }
    }
    if ((double)tsec >= next_row) {
        next_row += 80.0;
        if (rp < 2) {
            rp++;
            for (unsigned gi = 0; gi < 2; gi++) {
                uint64_t tot = 0, first[7], last[4], lrd[16][3], wpd[16][4], thr[8][2], ovf[4];
                int nrows = 0, nlr = 0, nwp = 0, nthr = 0;
                (void)smo_obs_m7n(gi, &tot, first, last, rows[gi], &nrows, lrd, &nlr, wpd, &nwp, thr, &nthr, ovf);
                vk_boot_logf("[m7n] row t=%us g=%s tot=%llu rows=%d (first-%d, claim no-overwrite)", tsec, gn[gi],
                             (unsigned long long)tot, nrows, nrows);
                for (int i = 0; i < nrows; i++) {
                    char dl[64]; dl[0] = 0;
                    if (rows[gi][i][5]) runtime_describe_address(rows[gi][i][5], dl, sizeof dl);
                    vk_boot_logf("[m7n] row t=%us g=%s #%d ns=%llus thr=%#llx obj=%#llx x1=%#llx tok=%#llx "
                                 "state=%lu aux=%lu lr=%#llx desc=%s",
                                 tsec, gn[gi], i,
                                 (unsigned long long)(rows[gi][i][0] / 1000000000ull),
                                 (unsigned long long)rows[gi][i][1], (unsigned long long)rows[gi][i][2],
                                 (unsigned long long)rows[gi][i][3], (unsigned long long)rows[gi][i][4],
                                 (unsigned long)rows[gi][i][6], (unsigned long)rows[gi][i][7],
                                 (unsigned long long)rows[gi][i][5], dl[0] ? dl : "-");
                }
            }
        }
    }
}
/* ---- M7P（2026-11-08）：**一只门、两边跑** —— worker Start main+0x963e50 ----
   门点：worker 线程的公共启动口（M7O 静态扫出的 15 个 ctor 站点全部汇于此：4 基类直调 +
   12 包装器 + 工厂派生路径间接）。只读门、四字窗，门记 LR + x0(worker obj) + epoch + tick。
   ★ 本文件的判定口径（两边用**同一份代码**出表，逐站对照就是 diff 两张 lr 表）★
     [m7p] lr  ← **每站计数表**：一行一个 distinct LR。门点 LR = bl 站点 + 4，故
                   site = lr − 4；拿出模拟器与真机的 site 集合做差集 = "真机缺哪几站"。
     [m7p] obj ← **哪个 worker 对象由哪一站建出**：一行一个对象，first_lr = 首次调 Start
                   的那一站 ⇒ 6 个框架线程的"出生站点"直接从这张表读。
   节拍：sum/lr/obj 每 10 s；row（首 64 条逐事件行，认领式不改写）在 t≈26 s 与 t≈120 s 各一次。
   Switch smo_m7p（C 默认 0；`echo 0 > .../files/smo_m7p` ≤2 s 回退）。 */
#define M7P_PATH_A "/data/storage/el2/base/files/smo_m7p"
#define M7P_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7p"
__attribute__((constructor)) static void m7p_seed(void) {
    if (!fopen(M7P_PATH_A, "r")) {
        FILE *f = fopen(M7P_PATH_A, "w");
        if (!f) f = fopen(M7P_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7p] seeded = 0 (off)\n"); }
    }
}
static int m7p_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7P_PATH_B : M7P_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7p_on;                     /* game_patches.c（单门：worker Start） */
/* 出厂镜像窗字（main.patched.bin 逐字实测）：4 条 stp/stp/add/ldr，无 bl/blr、无 PC 相对字。
   [m7p] code 行在运行期逐字回读这 16 字节，证明"门的装法没动 bl"（窗内 4 条无一条 bl/blr）。 */
static const uint32_t m7p_exp[4] = { 0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xb9400028 };
void runtime_m7p_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7p(uint64_t *total, uint64_t first[6], uint64_t last[4],
                           uint64_t rows[][7], int *nrows, uint64_t lrd[][4], int *nlr,
                           uint64_t obd[][5], int *nob, uint64_t thr[][2], int *nthr, uint64_t ovf[4]);
    extern uint64_t smo_obs_main_base(void);
    static double next_sum, next_lr, next_row;
    static int ann, code_done, rowshots;
    static uint64_t rows[64][7];
    atomic_store_explicit(&g_m7p_on, m7p_on(), memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m7p] probe on (t=%us) gate=main+0x963e50(worker Start, tail0 4w) sw=%d "
                     "ledger: lr=per-site count (site=lr-4) obj=worker->first_site "
                     "sum/lr/obj=10s row=26,120s code=20s",
                     tsec, (int)atomic_load_explicit(&g_m7p_on, memory_order_relaxed));
        next_sum = (double)tsec + 10.0; next_lr = (double)tsec + 10.0;
        next_row = (double)tsec + 26.0;
    }
    if ((double)tsec >= next_sum) {
        next_sum += 10.0;
        uint64_t tot = 0, first[6] = { 0 }, last[4] = { 0 }, lrd[32][4], obd[24][5], thr[8][2], ovf[4] = { 0 };
        int nrows = 0, nlr = 0, nob = 0, nthr = 0;
        (void)smo_obs_m7p(&tot, first, last, rows, &nrows, lrd, &nlr, obd, &nob, thr, &nthr, ovf);
        uint64_t ps = 0; for (int i = 0; i < nthr; i++) ps += thr[i][1];
        char dl[64]; dl[0] = 0; if (first[2]) runtime_describe_address(first[2], dl, sizeof dl);
        vk_boot_logf("[m7p] sum t=%us tot=%llu first_ns=%llus first_obj=%#llx first_ep=%#llx "
                     "first_st=%llu first_lr=%#llx first_site=%#llx lr_desc=%s thr=%#x "
                     "last_ns=%llus last_obj=%#llx uniq_site=%d uniq_obj=%d pt=%llu "
                     "ovf r/lr/obj/pt=%llu/%llu/%llu/%llu",
                     tsec, (unsigned long long)tot,
                     (unsigned long long)(first[0] / 1000000000ull), (unsigned long long)first[1],
                     (unsigned long long)first[3], (unsigned long long)first[4],
                     (unsigned long long)first[2],
                     (unsigned long long)(first[2] ? first[2] - 4 : 0), dl[0] ? dl : "-",
                     (unsigned)first[5],
                     (unsigned long long)(last[0] / 1000000000ull), (unsigned long long)last[1],
                     nlr, nob, (unsigned long long)ps,
                     (unsigned long long)ovf[0], (unsigned long long)ovf[1],
                     (unsigned long long)ovf[2], (unsigned long long)ovf[3]);
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base();
        uint32_t w[8]; int ok = mb && runtime_safe_read_ungated(w, mb + 0x963e50, sizeof w);
        unsigned same = 0, nbl = 0;
        if (ok) for (int i = 4; i < 8; i++) {
            if (m7n_is_bl_or_blr(w[i])) nbl++;
            /* 门点之后第 2..5 字（+0x10..，即重放窗后第一条 bl 所在处）应与出厂一字不差 */
            static const uint32_t tail_exp[4] = { 0xb900d808, 0xb940d814, 0xaa0003f3, 0x94031291 };
            if (w[i] == tail_exp[i - 4]) same++;
        }
        vk_boot_logf("[m7p] code t=%us @main+0x963e50 ok=%d win[0..3]=%08x %08x %08x %08x "
                     "exp=%08x %08x %08x %08x after+0x10 same=%u/4 bl_after=%u (the shipped bl@+0x1c, unmodified) (win 4 words have 0 bl/blr)",
                     tsec, ok, ok ? w[0] : 0, ok ? w[1] : 0, ok ? w[2] : 0, ok ? w[3] : 0,
                     m7p_exp[0], m7p_exp[1], m7p_exp[2], m7p_exp[3], same, nbl);
    }
    if ((double)tsec >= next_lr) {
        next_lr += 10.0;
        uint64_t tot = 0, first[6], last[4], lrd[32][4], obd[24][5], thr[8][2], ovf[4];
        int nrows = 0, nlr = 0, nob = 0, nthr = 0;
        (void)smo_obs_m7p(&tot, first, last, rows, &nrows, lrd, &nlr, obd, &nob, thr, &nthr, ovf);
        for (int i = 0; i < nlr; i++) {
            char dl[64]; dl[0] = 0;
            if (lrd[i][0]) runtime_describe_address(lrd[i][0], dl, sizeof dl);
            vk_boot_logf("[m7p] lr t=%us #%d lr=%#llx site=%#llx n=%llu first_ns=%llus last_ns=%llus desc=%s",
                         tsec, i, (unsigned long long)lrd[i][0],
                         (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0),
                         (unsigned long long)lrd[i][1],
                         (unsigned long long)(lrd[i][2] / 1000000000ull),
                         (unsigned long long)(lrd[i][3] / 1000000000ull), dl[0] ? dl : "-");
        }
        if (nlr >= 32) vk_boot_logf("[m7p] lr t=%us SLOTS FULL ovf=%llu", tsec, (unsigned long long)ovf[1]);
        for (int i = 0; i < nob; i++) {
            char dl[64]; dl[0] = 0;
            if (obd[i][3]) runtime_describe_address(obd[i][3], dl, sizeof dl);
            vk_boot_logf("[m7p] obj t=%us #%d obj=%#llx n=%llu first_ns=%llus ep=%#llx first_lr=%#llx "
                         "first_site=%#llx desc=%s",
                         tsec, i, (unsigned long long)obd[i][0], (unsigned long long)obd[i][1],
                         (unsigned long long)(obd[i][2] / 1000000000ull),
                         (unsigned long long)obd[i][4],
                         (unsigned long long)obd[i][3],
                         (unsigned long long)(obd[i][3] ? obd[i][3] - 4 : 0), dl[0] ? dl : "-");
        }
        if (nob >= 24) vk_boot_logf("[m7p] obj t=%us SLOTS FULL ovf=%llu", tsec, (unsigned long long)ovf[2]);
    }
    if ((double)tsec >= next_row) {
        next_row += 94.0;   /* 26 s → 120 s */
        if (rowshots < 2) {
            rowshots++;
            uint64_t tot = 0, first[6], last[4], lrd[32][4], obd[24][5], thr[8][2], ovf[4];
            int nrows = 0, nlr = 0, nob = 0, nthr = 0;
            (void)smo_obs_m7p(&tot, first, last, rows, &nrows, lrd, &nlr, obd, &nob, thr, &nthr, ovf);
            vk_boot_logf("[m7p] row t=%us tot=%llu rows=%d (first-%d, claim no-overwrite)", tsec,
                         (unsigned long long)tot, nrows, nrows);
            for (int i = 0; i < nrows; i++) {
                char dl[64]; dl[0] = 0;
                if (rows[i][2]) runtime_describe_address(rows[i][2], dl, sizeof dl);
                vk_boot_logf("[m7p] row t=%us #%d ns=%llus thr=%#llx site=%#llx lr=%#llx obj=%#llx "
                             "ep=%#llx tok=%#llx state=%llu desc=%s",
                             tsec, i,
                             (unsigned long long)(rows[i][0] / 1000000000ull),
                             (unsigned long long)rows[i][1],
                             (unsigned long long)(rows[i][2] ? rows[i][2] - 4 : 0),
                             (unsigned long long)rows[i][2], (unsigned long long)rows[i][3],
                             (unsigned long long)rows[i][4], (unsigned long long)rows[i][5],
                             (unsigned long long)rows[i][6], dl[0] ? dl : "-");
            }
        }
    }
}
/* ---- M7Q（2026-11-08）：**为什么真机没走到那 5 个建 worker 的站点** ----
   三层门（全部只读，门号 68..70）：
     WRAP = main+0x964540 包装 ctor 入口（tail=0 四字窗；静态 16 条 bl 调用者的公共上层）
     FO   = main+0x65b468 mov w1,w23（tail=2 普通字尾门；扇出数入口，只读）
     FOB  = main+0x65b470 bl 0x5b8710（tail=3 单 bl 门；真正造 3 条 lane 的调用）
   ★ 判定口径（两边同一份代码出表，逐表 diff）★
     [m7q] lr  ← WRAP 的**每调用者计数表**（一行一个 distinct LR，site = lr − 4）：
                  两张表做差集 = "真机哪条 ctor 站点所在的函数根本没被调用"。
     [m7q] v   ← FO/FOB 的 x1 取值分布 = **扇出数 w1**（FOB 处 x1 就是 w23，被调者入口原样）。
   节拍：probe 一次；sum/lr/v 每 10 s；row（首 64 条逐事件行）在 t≈26 s 与 t≈120 s 各一次；
         code（运行期逐字回读三处门窗）在 t≥10 s 一次。
   Switch smo_m7q（C 默认 0；`echo 0 > .../files/smo_m7q` ≤2 s 回退）。 */
#define M7Q_PATH_A "/data/storage/el2/base/files/smo_m7q"
#define M7Q_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7q"
__attribute__((constructor)) static void m7q_seed(void) {
    if (!fopen(M7Q_PATH_A, "r")) {
        FILE *f = fopen(M7Q_PATH_A, "w");
        if (!f) f = fopen(M7Q_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7q] seeded = 0 (off)\n"); }
    }
}
static int m7q_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7Q_PATH_B : M7Q_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7q_on;
/* 出厂镜像窗字（main.patched.bin 三份副本逐字一致，/tmp/m7q_safety.py）：
   WRAP 四字 = stp x20,x19,[sp,#-0x20]! / stp x29,x30,[sp,#0x10] / add x29,sp,#0x10 / mov w8,w7
   （无 bl/blr、无 PC 相对字）⇒ tail=0 四字窗，窗后 +0x10 起 4 字（含函数体第一条 bl@+0x30）
   必须与出厂逐字相同。FO/FOB 各 1 字 + 后继 1 字（tail=2/3）。 */
static const uint32_t m7q_w_exp[4] = { 0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0x2a0703e8 };
static const uint32_t m7q_w_tail[4] = { 0xb94013a7, 0xaa0203f3, 0xaa0303e2, 0x2a0403e3 };
static const uint32_t m7q_fo_exp[2] = { 0x2a1703e1, 0xaa0003f6 };
static const uint32_t m7q_fob_exp[2] = { 0x97fd74a8, 0x321c03e0 };
void runtime_m7q_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7q(int gi, uint64_t *total, uint64_t rows[][6], int *nrows,
                           uint64_t lrd[][4], int *nlr, uint64_t vd[][3], int *nv,
                           uint64_t first[6], uint64_t last[5], uint64_t ovf[3]);
    extern uint64_t smo_obs_main_base(void);
    static const char *const gn[3] = { "wrap", "fo", "fob" };
    static const uint32_t gsite[3] = { 0x964540, 0x65b468, 0x65b470 };
    static double next_sum, next_row;
    static int ann, code_done, rowshots;
    static uint64_t rows[64][6];
    atomic_store_explicit(&g_m7q_on, m7q_on(), memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m7q] probe on (t=%us) gates=wrap main+0x964540(tail0 4w) fo main+0x65b468"
                     "(tail2 plain) fob main+0x65b470(tail3 bl) sw=%d ledger: lr=per-caller "
                     "(site=lr-4) v=x1=fanout w1 sum/lr/v=10s row=26,120s code=20s",
                     tsec, (int)atomic_load_explicit(&g_m7q_on, memory_order_relaxed));
        next_sum = (double)tsec + 10.0;
        next_row = (double)tsec + 26.0;
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base();
        uint32_t w[8]; int okw = mb && runtime_safe_read_ungated(w, mb + 0x964540, sizeof w);
        unsigned same = 0;
        if (okw) for (int i = 4; i < 8; i++) if (w[i] == m7q_w_tail[i - 4]) same++;
        uint32_t f[2]; int okf = mb && runtime_safe_read_ungated(f, mb + 0x65b468, sizeof f);
        uint32_t b2[2]; int okb = mb && runtime_safe_read_ungated(b2, mb + 0x65b470, sizeof b2);
        vk_boot_logf("[m7q] code t=%us wrap@main+0x964540 ok=%d win[0..3]=%08x %08x %08x %08x "
                     "exp=%08x %08x %08x %08x after+0x10 same=%u/4 | fo@0x65b468 ok=%d w0=%08x"
                     "(veneer b?) succ=%08x exp=%08x | fob@0x65b470 ok=%d w0=%08x(veneer b?) "
                     "succ=%08x exp=%08x (fob bl replayed in stub; successor word untouched)",
                     tsec, okw, okw ? w[0] : 0, okw ? w[1] : 0, okw ? w[2] : 0, okw ? w[3] : 0,
                     m7q_w_exp[0], m7q_w_exp[1], m7q_w_exp[2], m7q_w_exp[3], same,
                     okf, okf ? f[0] : 0, okf ? f[1] : 0, m7q_fo_exp[1],
                     okb, okb ? b2[0] : 0, okb ? b2[1] : 0, m7q_fob_exp[1]);
    }
    if ((double)tsec >= next_sum) {
        next_sum += 10.0;
        for (int gi = 0; gi < 3; gi++) {
            uint64_t tot = 0, first[6] = { 0 }, last[5] = { 0 }, lrd[24][4], vd[8][3], ovf[3] = { 0 };
            int nrows = 0, nlr = 0, nv = 0;
            (void)smo_obs_m7q(gi, &tot, rows, &nrows, lrd, &nlr, vd, &nv, first, last, ovf);
            char dl[64]; dl[0] = 0;
            if (first[1]) runtime_describe_address(first[1], dl, sizeof dl);
            vk_boot_logf("[m7q] sum t=%us g=%s site=%#x tot=%llu first_ns=%llus first_lr=%#llx "
                         "first_desc=%s first_obj=%#llx first_x1=%#llx last_ns=%llus last_lr=%#llx "
                         "uniq_lr=%d uniq_v=%d ovf r/lr/v=%llu/%llu/%llu",
                         tsec, gn[gi], gsite[gi], (unsigned long long)tot,
                         (unsigned long long)(first[0] / 1000000000ull),
                         (unsigned long long)first[1], dl[0] ? dl : "-",
                         (unsigned long long)first[2], (unsigned long long)first[3],
                         (unsigned long long)(last[0] / 1000000000ull),
                         (unsigned long long)last[1], nlr, nv,
                         (unsigned long long)ovf[0], (unsigned long long)ovf[1],
                         (unsigned long long)ovf[2]);
            for (int i = 0; i < nlr; i++) {
                char d2[64]; d2[0] = 0;
                if (lrd[i][0]) runtime_describe_address(lrd[i][0], d2, sizeof d2);
                vk_boot_logf("[m7q] lr t=%us g=%s #%d lr=%#llx site=%#llx n=%llu first_ns=%llus "
                             "last_ns=%llus desc=%s",
                             tsec, gn[gi], i, (unsigned long long)lrd[i][0],
                             (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0),
                             (unsigned long long)lrd[i][1],
                             (unsigned long long)(lrd[i][2] / 1000000000ull),
                             (unsigned long long)(lrd[i][3] / 1000000000ull), d2[0] ? d2 : "-");
            }
            if (nlr >= 24) vk_boot_logf("[m7q] lr t=%us g=%s SLOTS FULL ovf=%llu", tsec, gn[gi],
                                        (unsigned long long)ovf[1]);
            for (int i = 0; i < nv; i++)
                vk_boot_logf("[m7q] v t=%us g=%s #%d val=%llu (=%#llx) n=%llu first_ns=%llus",
                             tsec, gn[gi], i, (unsigned long long)vd[i][0],
                             (unsigned long long)vd[i][0], (unsigned long long)vd[i][1],
                             (unsigned long long)(vd[i][2] / 1000000000ull));
            if (nv >= 8) vk_boot_logf("[m7q] v t=%us g=%s SLOTS FULL ovf=%llu", tsec, gn[gi],
                                      (unsigned long long)ovf[2]);
        }
    }
    if ((double)tsec >= next_row) {
        next_row += 94.0;   /* 26 s -> 120 s */
        if (rowshots < 2) {
            rowshots++;
            for (int gi = 0; gi < 3; gi++) {
                uint64_t tot = 0, first[6], last[5], lrd[24][4], vd[8][3], ovf[3];
                int nrows = 0, nlr = 0, nv = 0;
                (void)smo_obs_m7q(gi, &tot, rows, &nrows, lrd, &nlr, vd, &nv, first, last, ovf);
                vk_boot_logf("[m7q] row t=%us g=%s tot=%llu rows=%d (first-%d, claim no-overwrite)",
                             tsec, gn[gi], (unsigned long long)tot, nrows, nrows);
                for (int i = 0; i < nrows; i++) {
                    char dl[64]; dl[0] = 0;
                    if (rows[i][2]) runtime_describe_address(rows[i][2], dl, sizeof dl);
                    vk_boot_logf("[m7q] row t=%us g=%s #%d ns=%llus thr=%#llx site=%#llx lr=%#llx "
                                 "x0=%#llx x1=%#llx x8=%#llx desc=%s",
                                 tsec, gn[gi], i,
                                 (unsigned long long)(rows[i][0] / 1000000000ull),
                                 (unsigned long long)rows[i][1],
                                 (unsigned long long)(rows[i][2] ? rows[i][2] - 4 : 0),
                                 (unsigned long long)rows[i][2], (unsigned long long)rows[i][3],
                                 (unsigned long long)rows[i][4], (unsigned long long)rows[i][5],
                                 dl[0] ? dl : "-");
                }
            }
        }
    }
}
/* ---- M7S2（2026-11-08）：池 B 构造总入口为什么没被调用 —— 三层只读门（门号 71..73） ----
     G1 = main+0x681b30 池 B 构造总入口（tail=0 四字窗；唯一静态调用者 = bl@0x74cae0）
     G2 = main+0x74ca00 上一层候选（tail=0 四字窗；离线重扫出 **8 条**静态 bl，不是 0）
     G3 = main+0x74cadc = bl@0x74cae0 的**前一字**（tail=2 普通字尾门；bl 本身绝不覆盖）
   ★ 判据（派活单第 3 节，一眼可读）★
     G1 tot=0                ⇒ 断点在调用者一侧（G2/G3 定量区分：G3 tot≥1 而 G1=0 物理上不可能）
     G1 tot≥1                ⇒ 断点在函数体内某条 bl 之前（配合 M7Q 的三条 bl 下游全空）
   节拍：probe 一次；sum/lr 每 10 s；code（三处门窗逐字回读）t≥10 s 一次；
         row（G1/G2 首 64 条逐事件）在 t≈26 s 与 t≈120 s 各一次。
   Switch smo_m7s2（C 默认 0；`echo 0 > .../files/smo_m7s2` ≤2 s 回退）。 */
#define M7S2_PATH_A "/data/storage/el2/base/files/smo_m7s2"
#define M7S2_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7s2"
__attribute__((constructor)) static void m7s2_seed(void) {
    if (!fopen(M7S2_PATH_A, "r")) {
        FILE *f = fopen(M7S2_PATH_A, "w");
        if (!f) f = fopen(M7S2_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7s2] seeded = 0 (off)\n"); }
    }
}
static int m7s2_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7S2_PATH_B : M7S2_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7s2_on;
/* 出厂窗字（main.patched.bin 三份副本逐字一致，本轮 /tmp/m7s_safety.py 重扫）：
   G1 0x681b30 四字 = str x23,[sp,#-0x40]! / stp x22,x21,[sp,#0x10] / stp x20,x19,[sp,#0x20] /
                       stp x29,x30,[sp,#0x30]（无 bl/blr、无 PC 相对字）⇒ tail=0 四字窗；
      窗后 +0x10 起 4 字（add x29,sp,#0x30 / mov x19,x0 / mov w0,#8 / mov w20,w1）必须与出厂逐字相同。
   G2 0x74ca00 四字 = stp x22,x21,[sp,#-0x30]! / stp x20,x19,[sp,#0x10] / stp x29,x30,[sp,#0x20] /
                       add x29,sp,#0x20；窗后 +0x10 起 4 字（mov x21,x0 / mov w0,#0x18 /
                      mov w19,w2 / mov x20,x1）同验。
   G3 0x74cadc 一字 = mov w1,w19；**后继 0x74cae0 = 97fcd414（bl 0x681b30）必须仍是出厂字**
      —— 这是「bl 未被改动」的运行期证明。 */
static const uint32_t m7s2_g1_exp[4] = { 0xf81c0ff7, 0xa90157f6, 0xa9024ff4, 0xa9037bfd };
static const uint32_t m7s2_g1_aft[4] = { 0x9100c3fd, 0xaa0003f3, 0x321d03e0, 0x2a0103f4 };
static const uint32_t m7s2_g2_exp[4] = { 0xa9bd57f6, 0xa9014ff4, 0xa9027bfd, 0x910083fd };
static const uint32_t m7s2_g2_aft[4] = { 0xaa0003f5, 0x321d07e0, 0x2a0203f3, 0xaa0103f4 };
static const uint32_t m7s2_g3_exp[2] = { 0x2a1303e1, 0x97fcd414 };
void runtime_m7s2_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7s2(int gi, uint64_t *total, uint64_t rows[][6], int *nrows,
                            uint64_t lrd[][4], int *nlr, uint64_t first[6], uint64_t last[4],
                            uint64_t ovf[2]);
    extern uint64_t smo_obs_main_base(void);
    static const char *const gn[3] = { "g1", "g2", "g3" };
    static const uint32_t gsite[3] = { 0x681b30, 0x74ca00, 0x74cadc };
    static double next_sum, next_row;
    static int ann, code_done, rowshots;
    static uint64_t rows[64][6];
    atomic_store_explicit(&g_m7s2_on, m7s2_on(), memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m7s2] probe on (t=%us) g1 main+0x681b30(tail0 4w, pool-B ctor entry) "
                     "g2 main+0x74ca00(tail0 4w, layer above) g3 main+0x74cadc(tail2 plain, the "
                     "word BEFORE bl@0x74cae0) sw=%d ledger: lr=per-caller (site=lr-4) "
                     "sum/lr=10s row=26,120s code=10s", tsec,
                     (int)atomic_load_explicit(&g_m7s2_on, memory_order_relaxed));
        next_sum = (double)tsec + 10.0;
        next_row = (double)tsec + 26.0;
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base();
        uint32_t a[8]; int oka = mb && runtime_safe_read_ungated(a, mb + 0x681b30, sizeof a);
        unsigned sa = 0;
        if (oka) for (int i = 0; i < 4; i++) if (a[4 + i] == m7s2_g1_aft[i]) sa++;
        uint32_t b[8]; int okb = mb && runtime_safe_read_ungated(b, mb + 0x74ca00, sizeof b);
        unsigned sb = 0;
        if (okb) for (int i = 0; i < 4; i++) if (b[4 + i] == m7s2_g2_aft[i]) sb++;
        uint32_t c[2]; int okc = mb && runtime_safe_read_ungated(c, mb + 0x74cadc, sizeof c);
        vk_boot_logf("[m7s2] code t=%us g1@main+0x681b30 ok=%d win[0..3]=%08x %08x %08x %08x "
                     "exp=%08x %08x %08x %08x after+0x10 same=%u/4 | g2@main+0x74ca00 ok=%d "
                     "win[0..3]=%08x %08x %08x %08x exp=%08x %08x %08x %08x after+0x10 same=%u/4 | "
                     "g3@main+0x74cadc ok=%d w0=%08x(veneer b) w1=%08x exp_bl=%08x (bl 0x681b30 "
                     "UNTOUCHED=%d)",
                     tsec, oka, oka ? a[0] : 0, oka ? a[1] : 0, oka ? a[2] : 0, oka ? a[3] : 0,
                     m7s2_g1_exp[0], m7s2_g1_exp[1], m7s2_g1_exp[2], m7s2_g1_exp[3], sa,
                     okb, okb ? b[0] : 0, okb ? b[1] : 0, okb ? b[2] : 0, okb ? b[3] : 0,
                     m7s2_g2_exp[0], m7s2_g2_exp[1], m7s2_g2_exp[2], m7s2_g2_exp[3], sb,
                     okc, okc ? c[0] : 0, okc ? c[1] : 0, m7s2_g3_exp[1],
                     (okc && c[1] == m7s2_g3_exp[1]) ? 1 : 0);
    }
    if ((double)tsec >= next_sum) {
        next_sum += 10.0;
        for (int gi = 0; gi < 3; gi++) {
            uint64_t tot = 0, first[6] = { 0 }, last[4] = { 0 }, lrd[32][4], ovf[2] = { 0 };
            int nrows = 0, nlr = 0;
            (void)smo_obs_m7s2(gi, &tot, rows, &nrows, lrd, &nlr, first, last, ovf);
            char dl[64]; dl[0] = 0;
            if (first[1]) runtime_describe_address(first[1], dl, sizeof dl);
            vk_boot_logf("[m7s2] sum t=%us g=%s site=%#x tot=%llu first_ns=%llus first_lr=%#llx "
                         "first_desc=%s first_x0=%#llx first_x1=%#llx first_x8=%#llx "
                         "last_ns=%llus last_lr=%#llx last_x0=%#llx uniq_lr=%d ovf r/lr=%llu/%llu",
                         tsec, gn[gi], gsite[gi], (unsigned long long)tot,
                         (unsigned long long)(first[0] / 1000000000ull),
                         (unsigned long long)first[1], dl[0] ? dl : "-",
                         (unsigned long long)first[2], (unsigned long long)first[3],
                         (unsigned long long)first[4],
                         (unsigned long long)(last[0] / 1000000000ull),
                         (unsigned long long)last[1], (unsigned long long)last[2], nlr,
                         (unsigned long long)ovf[0], (unsigned long long)ovf[1]);
            for (int i = 0; i < nlr; i++) {
                char d2[64]; d2[0] = 0;
                if (lrd[i][0]) runtime_describe_address(lrd[i][0], d2, sizeof d2);
                vk_boot_logf("[m7s2] lr t=%us g=%s #%d lr=%#llx site=%#llx n=%llu first_ns=%llus "
                             "last_ns=%llus desc=%s",
                             tsec, gn[gi], i, (unsigned long long)lrd[i][0],
                             (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0),
                             (unsigned long long)lrd[i][1],
                             (unsigned long long)(lrd[i][2] / 1000000000ull),
                             (unsigned long long)(lrd[i][3] / 1000000000ull), d2[0] ? d2 : "-");
            }
            if (nlr >= 32) vk_boot_logf("[m7s2] lr t=%us g=%s SLOTS FULL ovf=%llu", tsec, gn[gi],
                                        (unsigned long long)ovf[1]);
        }
    }
    if ((double)tsec >= next_row) {
        next_row += 94.0;   /* 26 s -> 120 s */
        if (rowshots < 2) {
            rowshots++;
            for (int gi = 0; gi < 2; gi++) {
                uint64_t tot = 0, first[6], last[4], lrd[32][4], ovf[2];
                int nrows = 0, nlr = 0;
                (void)smo_obs_m7s2(gi, &tot, rows, &nrows, lrd, &nlr, first, last, ovf);
                vk_boot_logf("[m7s2] row t=%us g=%s tot=%llu rows=%d (first-%d, claim no-overwrite)",
                             tsec, gn[gi], (unsigned long long)tot, nrows, nrows);
                for (int i = 0; i < nrows; i++) {
                    char dl[64]; dl[0] = 0;
                    if (rows[i][2]) runtime_describe_address(rows[i][2], dl, sizeof dl);
                    vk_boot_logf("[m7s2] row t=%us g=%s #%d ns=%llus thr=%#llx site=%#llx lr=%#llx "
                                 "x0=%#llx x1=%#llx x8=%#llx desc=%s",
                                 tsec, gn[gi], i,
                                 (unsigned long long)(rows[i][0] / 1000000000ull),
                                 (unsigned long long)rows[i][1],
                                 (unsigned long long)(rows[i][2] ? rows[i][2] - 4 : 0),
                                 (unsigned long long)rows[i][2], (unsigned long long)rows[i][3],
                                 (unsigned long long)rows[i][4], (unsigned long long)rows[i][5],
                                 dl[0] ? dl : "-");
                }
            }
        }
    }
}
/* ---- M7T2（2026-11-08）：**整族九个入口函数一起装门**（门号 74..82）----
     H1 0x445640 / H2 0x447b70 / H3 0x449c50 / H4 0x44b720 / H5 0x46e7a0 /
     H6 0x4a1100 / H7 0x4a6b10 / H8 0x4a8820 / H9 0x4af0a0
   九门全是 tail=0 四字窗（九个首字全部改写 sp：8 个 pre-index 存 + 0x4a1100 的 sub sp）。
   ★ 判据（M7S 给的下一层，一眼可读）★
     九门全 0                          ⇒ 断点在**整族的上一层**（描述表/vtable 装载者）
     某个 Hi tot≥1 而 G2/G3 仍 0        ⇒ 断在 Hi 函数体内、它那条 bl 之前
     H7 tot≥1 而 G2/G3=0               ⇒ 断在 0x4a6b10 体内 +0x4c 之前（M7S 推荐的单点）
   节拍：probe 一次；code（九门窗逐字回读 + 门后 4 字）t≥10 s 一次；sum/lr 每 10 s。
   Switch smo_m7t2（C 默认 0；`echo 0 > .../files/smo_m7t2` ≤2 s 回退）。
   ⚠️ 名字用 m7t2 而不是 m7t：`smo_m7t`/`runtime_m7t_poll`/`[m7t]` 已被 2026-11-07 那轮占用。 */
#define M7T2_PATH_A "/data/storage/el2/base/files/smo_m7t2"
#define M7T2_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7t2"
__attribute__((constructor)) static void m7t2_seed(void) {
    if (!fopen(M7T2_PATH_A, "r")) {
        FILE *f = fopen(M7T2_PATH_A, "w");
        if (!f) f = fopen(M7T2_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7t2] seeded = 0 (off)\n"); }
    }
}
static int m7t2_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7T2_PATH_B : M7T2_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7t2_on;
/* 出厂窗字（本轮 /tmp/m7t2_safety.py 逐门重扫，三份镜像副本逐字一致；窗内 PC 相对字 0、
   窗内中间字落点 0）与门后 off+0x10 起 4 字（运行期 after 校验，门不碰这 4 字）。 */
static const uint32_t m7t2_win[9][4] = {
    { 0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6 },   /* H1 0x445640 stp x28,x27,[sp,#-0x60]! */
    { 0xf81b0ffc, 0xa9015ff8, 0xa90257f6, 0xa9034ff4 },   /* H2 0x447b70 str x28,[sp,#-0x50]! */
    { 0xf81b0ffc, 0xa9015ff8, 0xa90257f6, 0xa9034ff4 },   /* H3 0x449c50 同形 */
    { 0xfc190fe8, 0xa9016ffc, 0xa90267fa, 0xa9035ff8 },   /* H4 0x44b720 str d8,[sp,#-0x70]! */
    { 0xfc190fe8, 0xa9016ffc, 0xa90267fa, 0xa9035ff8 },   /* H5 0x46e7a0 同形 */
    { 0xd10603ff, 0x6d1123e9, 0xa9126ffc, 0xa91367fa },   /* H6 0x4a1100 sub sp,sp,#0x180 */
    { 0xf81b0ffc, 0xa9015ff8, 0xa90257f6, 0xa9034ff4 },   /* H7 0x4a6b10 同 H2/H3 */
    { 0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6 },   /* H8 0x4a8820 同 H1 */
    { 0xa9bc5ffc, 0xa90157f6, 0xa9024ff4, 0xa9037bfd },   /* H9 0x4af0a0 stp x28,x23,[sp,#-0x40]! */
};
static const uint32_t m7t2_aft[9][4] = {
    { 0xa9044ff4, 0xa9057bfd, 0x910143fd, 0xd111c3ff },
    { 0xa9047bfd, 0x910103fd, 0xd10a43ff, 0xaa0103f4 },
    { 0xa9047bfd, 0x910103fd, 0xd10a43ff, 0xaa0103f4 },
    { 0xa90457f6, 0xa9054ff4, 0xa9067bfd, 0x910183fd },
    { 0xa90457f6, 0xa9054ff4, 0xa9067bfd, 0x910183fd },
    { 0xa9145ff8, 0xa91557f6, 0xa9164ff4, 0xa9177bfd },
    { 0xa9047bfd, 0x910103fd, 0xd13b83ff, 0xaa0103f4 },
    { 0xa9044ff4, 0xa9057bfd, 0x910143fd, 0xd10b43ff },
    { 0x9100c3fd, 0xd10a43ff, 0xaa0103f4, 0xaa0003f3 },
};
void runtime_m7t2_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7t2(int gi, uint64_t *total, uint64_t lrd[][4], int *nlr,
                            uint64_t first[6], uint64_t last[4], uint64_t *lr_ovf);
    extern uint64_t smo_obs_main_base(void);
    static const char *const gn[9] = { "h1", "h2", "h3", "h4", "h5", "h6", "h7", "h8", "h9" };
    static const uint32_t gsite[9] = { 0x445640, 0x447b70, 0x449c50, 0x44b720, 0x46e7a0,
                                       0x4a1100, 0x4a6b10, 0x4a8820, 0x4af0a0 };
    static double next_sum;
    static int ann, code_done;
    uint64_t lrd[32][4];
    atomic_store_explicit(&g_m7t2_on, m7t2_on(), memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m7t2] probe on (t=%us) whole scene-ctor family, 9 heads tail0 4w: "
                     "h1 0x445640 h2 0x447b70 h3 0x449c50 h4 0x44b720 h5 0x46e7a0 "
                     "h6 0x4a1100 h7 0x4a6b10 h8 0x4a8820 h9 0x4af0a0 sw=%d ledger: "
                     "count+first/last tick+per-LR 32 slots (site=lr-4) sum/lr=10s code=10s",
                     tsec, (int)atomic_load_explicit(&g_m7t2_on, memory_order_relaxed));
        next_sum = (double)tsec + 10.0;
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base();
        for (int gi = 0; gi < 9; gi++) {
            uint32_t a[8]; int ok = mb && runtime_safe_read_ungated(a, mb + gsite[gi], sizeof a);
            unsigned same = 0;
            if (ok) for (int i = 0; i < 4; i++) if (a[4 + i] == m7t2_aft[gi][i]) same++;
            uint32_t w0 = ok ? a[0] : 0, w1 = ok ? a[1] : 0, w2 = ok ? a[2] : 0, w3 = ok ? a[3] : 0;
            vk_boot_logf("[m7t2] code t=%us g=%s site=main+%#x ok=%d win[0..3]=%08x %08x %08x %08x "
                         "exp=%08x %08x %08x %08x after+0x10 same=%u/4",
                         tsec, gn[gi], gsite[gi], ok, w0, w1, w2, w3,
                         m7t2_win[gi][0], m7t2_win[gi][1], m7t2_win[gi][2], m7t2_win[gi][3], same);
        }
    }
    if ((double)tsec >= next_sum) {
        next_sum += 10.0;
        for (int gi = 0; gi < 9; gi++) {
            uint64_t tot = 0, first[6] = { 0 }, last[4] = { 0 }, ov = 0;
            int nlr = 0;
            (void)smo_obs_m7t2(gi, &tot, lrd, &nlr, first, last, &ov);
            char dl[64]; dl[0] = 0;
            if (first[1]) runtime_describe_address(first[1], dl, sizeof dl);
            vk_boot_logf("[m7t2] sum t=%us g=%s site=%#x tot=%llu first_ns=%llus first_lr=%#llx "
                         "first_desc=%s first_x0=%#llx first_x1=%#llx first_x8=%#llx "
                         "last_ns=%llus last_lr=%#llx last_x0=%#llx uniq_lr=%d lr_ovf=%llu",
                         tsec, gn[gi], gsite[gi], (unsigned long long)tot,
                         (unsigned long long)(first[0] / 1000000000ull),
                         (unsigned long long)first[1], dl[0] ? dl : "-",
                         (unsigned long long)first[2], (unsigned long long)first[3],
                         (unsigned long long)first[4],
                         (unsigned long long)(last[0] / 1000000000ull),
                         (unsigned long long)last[1], (unsigned long long)last[2], nlr,
                         (unsigned long long)ov);
            for (int i = 0; i < nlr; i++) {
                char d2[64]; d2[0] = 0;
                if (lrd[i][0]) runtime_describe_address(lrd[i][0], d2, sizeof d2);
                vk_boot_logf("[m7t2] lr t=%us g=%s #%d lr=%#llx site=%#llx n=%llu first_ns=%llus "
                             "last_ns=%llus desc=%s",
                             tsec, gn[gi], i, (unsigned long long)lrd[i][0],
                             (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0),
                             (unsigned long long)lrd[i][1],
                             (unsigned long long)(lrd[i][2] / 1000000000ull),
                             (unsigned long long)(lrd[i][3] / 1000000000ull), d2[0] ? d2 : "-");
            }
        }
    }
}
/* ---- M7U2（2026-11-08）：**场景派发枢纽 main+0x7acd50** 三门（门号 83..85）----
     A 0x7acd50（tail=0 四字窗）派发枢纽入口 —— M7T2 实测 H5/H7 的 LR = 0x7acd94 ⇒ 它就是
       "对 [this+0x10] 做 vtable slot0 派发"的那层；零静态调用者。
     B 0x7acd94（tail=2 单字门）**派发返回址** ⇒ 计数 = "slot0 派发成功返回了几次"；
       x0 = 被派发者返回值（**不是** vtable 指针 —— 派活单该措辞在此纠正，vtable 指针改由
       C 门从 [this+0x10] 解一层拿到，在 name 行里打印）。
     C 0x7acd70（tail=2 单字门）**stage 名取用点**（0x7acd5c 取到名指针之后、0x7acd74
       `bl 0x9564f0` 查表之前）⇒ 名指针 + 前 32B ASCII 进 [m7u2] name 行；**该 bl 一字不碰**
       （code 行把 0x7acd74 的字逐字回读证明）。
   节拍：probe 一次；code（三门窗逐字回读 + 门后 4 字 + C 门那条 bl）t≥10 s 一次；
   sum/lr 每 10 s；name（stage 名环，最多 8 条，逐条只打一次）。
   判据（派活单三选一）：A=0 ⇒ ①入口没进；A≥1 ∧ B=0 ⇒ ②进了但 slot0 没派发（或没返回）；
   A≥1 ∧ B≥1 ⇒ 派发发生了 —— 看 C 的 name 行判 ③stage 名对不对。
   Switch smo_m7u2（C 默认 0；`echo 0 > .../files/smo_m7u2` ≤2 s 回退）。
   ⚠️ 名字用 m7u2 而不是 m7u：`smo_m7u`/`runtime_m7u_poll`/`[m7u]` 属 2026-11-07 那轮。 */
#define M7U2_PATH_A "/data/storage/el2/base/files/smo_m7u2"
#define M7U2_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7u2"
__attribute__((constructor)) static void m7u2_seed(void) {
    if (!fopen(M7U2_PATH_A, "r")) {
        FILE *f = fopen(M7U2_PATH_A, "w");
        if (!f) f = fopen(M7U2_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7u2] seeded = 0 (off)\n"); }
    }
}
static int m7u2_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7U2_PATH_B : M7U2_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7u2_on;
/* 出厂字（/tmp/m7u_safety.py 逐门重扫：三份镜像副本逐字一致、被换字无 PC 相对字段、
   全镜像分支落点首字与窗内中间字全 0、与既有 75 行站表零重叠）。 */
static const struct { const char *nm; uint32_t site; uint32_t win[4]; uint32_t aft[4]; uint32_t prev; } m7u2_g[3] = {
    { "a", 0x7acd50, { 0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xf9400401 },   /* stp x20,x19,[sp,#-0x20]! ; stp x29,x30 ; add x29 ; ldr x1,[x0,#8] */
                      { 0xaa0003f4, 0xb40000e1, 0xb0005dc8, 0xf9462d08 }, 0xd65f03c0 },  /* prev = ret */
    { "b", 0x7acd94, { 0xf9400280, 0xf9400008, 0xf9401d08, 0x2a1f03e1 },   /* ldr x0,[x20] ; ldr x8,[x0] ; ldr x8,[x8,#0x38] ; mov w1,wzr */
                      { 0xd63f0100, 0xf100067f, 0x54000100, 0xb0005dc8 }, 0xd63f0100 },  /* prev = blr x8（0x7acd90） */
    { "c", 0x7acd70, { 0xf9400100, 0x9406a5df, 0xaa0003f3, 0x14000002 },   /* ldr x0,[x8] ; **bl 0x9564f0（不换）** ; mov x19,x0 ; b 0x7acd84 */
                      { 0x320003f3, 0xf9400a80, 0xf9400008, 0xf9400108 }, 0xf9462d08 },  /* prev = ldr x8,[x8,#0xc58] */
};
static void m7u2_ascii(const unsigned char *b, char *out, size_t n) {
    size_t k = 0;
    for (int i = 0; i < 32 && k + 1 < n; i++) {
        unsigned char c = b[i];
        if (c == 0) break;
        out[k++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '.';
    }
    out[k] = 0;
    if (!k) { out[0] = '-'; out[1] = 0; }
}
void runtime_m7u2_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7u2(int gi, uint64_t *total, uint64_t lrd[][4], int *nlr,
                            uint64_t first[6], uint64_t last[4], uint64_t *lr_ovf);
    extern int smo_obs_m7u2_name(int i, uint64_t meta[8], unsigned char bytes[32]);
    extern int smo_obs_m7u2_name_n(void);
    extern uint64_t smo_obs_main_base(void);
    static double next_sum;
    static int ann, code_done, name_done;
    uint64_t lrd[16][4];
    atomic_store_explicit(&g_m7u2_on, m7u2_on(), memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m7u2] probe on (t=%us) scene-dispatch hub 3 gates: a 0x7acd50 entry "
                     "(tail0 4w) b 0x7acd94 dispatch-return-addr (tail2) c 0x7acd70 stage-name "
                     "point (tail2) sw=%d ledger: count+first/last+per-LR 16 + stage-name ring 8 "
                     "sum/lr=10s code=10s name=10s",
                     tsec, (int)atomic_load_explicit(&g_m7u2_on, memory_order_relaxed));
        next_sum = (double)tsec + 10.0;
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base();
        for (int gi = 0; gi < 3; gi++) {
            uint32_t a[8]; int ok = mb && runtime_safe_read_ungated(a, mb + m7u2_g[gi].site, sizeof a);
            unsigned same = 0, pv = 0;
            if (ok) for (int i = 0; i < 4; i++) if (a[4 + i] == m7u2_g[gi].aft[i]) same++;
            if (ok) { uint32_t p; pv = runtime_safe_read_ungated(&p, mb + m7u2_g[gi].site - 4, 4) ? p : 0; }
            uint32_t w0 = ok ? a[0] : 0, w1 = ok ? a[1] : 0, w2 = ok ? a[2] : 0, w3 = ok ? a[3] : 0;
            vk_boot_logf("[m7u2] code t=%us g=%s site=main+%#x ok=%d win[0..3]=%08x %08x %08x %08x "
                         "exp=%08x %08x %08x %08x after+0x10 same=%u/4 prev(main+%#x)=%08x exp_prev=%08x "
                         "w1=%08x exp_w1=%08x (gate c: w1 = 0x7acd74 the bl 0x9564f0, must stay untouched)",
                         tsec, m7u2_g[gi].nm, m7u2_g[gi].site, ok, w0, w1, w2, w3,
                         m7u2_g[gi].win[0], m7u2_g[gi].win[1], m7u2_g[gi].win[2], m7u2_g[gi].win[3],
                         same, m7u2_g[gi].site - 4, pv, m7u2_g[gi].prev, w1, m7u2_g[gi].win[1]);
        }
    }
    if (!name_done && (double)tsec >= 10.0) {
        int n = smo_obs_m7u2_name_n();
        if (n > 0) {
            name_done = 1;
            for (int i = 0; i < n; i++) {
                uint64_t m[8] = { 0 }; unsigned char by[32] = { 0 }; char asc[40];
                if (!smo_obs_m7u2_name(i, m, by)) continue;
                m7u2_ascii(by, asc, sizeof asc);
                char dn[64]; dn[0] = 0;
                if (m[2]) runtime_describe_address(m[2], dn, sizeof dn);
                vk_boot_logf("[m7u2] name #%d g=%s ns=%llus site_lr=%#llx desc=%s this=%#llx "
                             "name_ptr=%#llx name2_ptr=%#llx ascii=\"%s\" scene=%#llx vtbl=%#llx",
                             i, m[1] == 83 ? "a" : (m[1] == 85 ? "c" : "?"),
                             (unsigned long long)(m[0] / 1000000000ull),
                             (unsigned long long)m[2], dn[0] ? dn : "-",
                             (unsigned long long)m[3], (unsigned long long)m[4],
                             (unsigned long long)m[5], asc,
                             (unsigned long long)m[6], (unsigned long long)m[7]);
            }
        }
    }
    if ((double)tsec >= next_sum) {
        next_sum += 10.0;
        for (int gi = 0; gi < 3; gi++) {
            uint64_t tot = 0, first[6] = { 0 }, last[4] = { 0 }, ov = 0;
            int nlr = 0;
            (void)smo_obs_m7u2(gi, &tot, lrd, &nlr, first, last, &ov);
            char dl[64]; dl[0] = 0;
            if (first[1]) runtime_describe_address(first[1], dl, sizeof dl);
            vk_boot_logf("[m7u2] sum t=%us g=%s site=%#x tot=%llu first_ns=%llus first_lr=%#llx "
                         "first_desc=%s first_x0=%#llx first_x1=%#llx first_x8=%#llx thr=%llu "
                         "last_ns=%llus last_lr=%#llx last_x0=%#llx last_x1=%#llx uniq_lr=%d lr_ovf=%llu",
                         tsec, m7u2_g[gi].nm, m7u2_g[gi].site, (unsigned long long)tot,
                         (unsigned long long)(first[0] / 1000000000ull),
                         (unsigned long long)first[1], dl[0] ? dl : "-",
                         (unsigned long long)first[2], (unsigned long long)first[3],
                         (unsigned long long)first[4], (unsigned long long)first[5],
                         (unsigned long long)(last[0] / 1000000000ull),
                         (unsigned long long)last[1], (unsigned long long)last[2],
                         (unsigned long long)last[3], nlr, (unsigned long long)ov);
            for (int i = 0; i < nlr; i++) {
                char d2[64]; d2[0] = 0;
                if (lrd[i][0]) runtime_describe_address(lrd[i][0], d2, sizeof d2);
                vk_boot_logf("[m7u2] lr t=%us g=%s #%d lr=%#llx site=%#llx n=%llu first_ns=%llus "
                             "last_ns=%llus desc=%s",
                             tsec, m7u2_g[gi].nm, i, (unsigned long long)lrd[i][0],
                             (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0),
                             (unsigned long long)lrd[i][1],
                             (unsigned long long)(lrd[i][2] / 1000000000ull),
                             (unsigned long long)(lrd[i][3] / 1000000000ull), d2[0] ? d2 : "-");
            }
        }
    }
}
/* ---- M7V（2026-11-08）：**点名"真机派发到谁"**（门号 86..93）----
   X8 0x7acd8c（tail=2 单字门）`ldr x8,[x8]`：门点原 x8 = vtable 指针 ⇒ 观察器再解一层 = 0x7acd90
     `blr x8` 真要跳的函数地址；进 8 槽环，逐条打印 时刻/LR/scene/vtbl/**tgt**（全部 describe 成
     main+偏移）⇒ "派发到谁"是读数不是假说。
   R1..R7 = Rsv 兄弟实例入口（0x4a6a90/0x46e770/0x445590/0x447ae0/0x449bc0/0x44b6f0/0x4af020，
     tail=0 四字窗）：只记 count/首末见/per-LR 16 槽 ⇒ "谁非零"直接指认族外实例。
   名环 v2（**不加门**，挂既有门 A(83)/C(85)）：只留原始四槽 hex、不改写不覆盖。
   节拍：probe 一次；code（八门窗逐字回读 + 门后 4 字 + 前一字）t≥10 s 一次；d/rsv 每 10 s；
   name t≥10 s 后一次（最多 8 条）。
   Switch smo_m7v（C 默认 0；`echo 0 > .../files/smo_m7v` ≤2 s 回退）。 */
#define M7V_PATH_A "/data/storage/el2/base/files/smo_m7v"
#define M7V_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7v"
__attribute__((constructor)) static void m7v_seed(void) {
    if (!fopen(M7V_PATH_A, "r")) {
        FILE *f = fopen(M7V_PATH_A, "w");
        if (!f) f = fopen(M7V_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7v] seeded = 0 (off)\n"); }
    }
}
static int m7v_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7V_PATH_B : M7V_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7v_on;
/* 出厂字（/tmp/m7v_safety.py 逐门重扫：三镜像逐字一致、被换字只有 X8 的 word[0] 与 R1..R7 的 adrp、
   全镜像分支落点首字/窗内中间字全 0、与既有 78 行站表零重叠）。 */
static const struct { const char *nm; uint32_t site; uint32_t win[4]; uint32_t aft[3]; uint32_t prev; } m7v_g[8] = {
    { "x8", 0x7acd8c, { 0xf9400108, 0xd63f0100, 0xf9400280, 0xf9400008 },   /* ldr x8,[x8] ; **blr x8（不换）** ; ldr x0,[x20] ; ldr x8,[x0] */
                       { 0xf9401d08, 0x2a1f03e1, 0xd63f0100 }, 0xf9400008 }, /* prev = ldr x8,[x0]（0x7acd88） */
    { "r1", 0x4a6a90, { 0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x90007608 },   /* 0x4a6a90 Rsv（H7 兄弟侧） */
                       { 0xf9410908, 0x91004109, 0xf9000009 }, 0x140a9755 },
    { "r2", 0x46e770, { 0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3 },   /* 0x46e770 Rsv（H5 兄弟侧） */
                       { 0x97ffff8c, 0xa9417bfd, 0xaa1303e0 }, 0x00000000 },
    { "r3", 0x445590, { 0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0x90007908 },   /* 0x445590 Rsv（H1 兄弟侧） */
                       { 0xf945f108, 0x91004109, 0xf9000009 }, 0x00000000 },
    { "r4", 0x447ae0, { 0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xd00078e8 },   /* 0x447ae0 Rsv（H2 兄弟侧） */
                       { 0xf945f908, 0x91004109, 0x9102610a }, 0x00000000 },
    { "r5", 0x449bc0, { 0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x900078e8 },   /* 0x449bc0 Rsv（H3 兄弟侧） */
                       { 0xf9460508, 0x91004109, 0x9102610a }, 0x00000000 },
    { "r6", 0x44b6f0, { 0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3 },   /* 0x44b6f0 Rsv（H4 兄弟侧） */
                       { 0x97ffffb0, 0xa9417bfd, 0xaa1303e0 }, 0x00000000 },
    { "r7", 0x4af020, { 0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xf00075a8 },   /* 0x4af020 Rsv（H9 兄弟侧） */
                       { 0xf9416908, 0x91004109, 0x9102210a }, 0x00000000 },
};
/* 32B → 64 个 hex 字符（内存序，不加分隔、不做 ASCII 过滤） */
static void m7v_hex32(const unsigned char *b, char *out) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = h[b[i] >> 4]; out[2 * i + 1] = h[b[i] & 15]; }
    out[64] = 0;
}
void runtime_m7v_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7v(int gi, uint64_t *total, uint64_t lrd[][4], int *nlr,
                           uint64_t first[6], uint64_t last[4], uint64_t *lr_ovf);
    extern int smo_obs_m7v_d(int i, uint64_t meta[5], uint32_t *ok, uint32_t *thr);
    extern int smo_obs_m7v_d_n(void);
    extern int smo_obs_m7v_name(int i, uint64_t meta[8], unsigned char bytes[128],
                                uint32_t *avail, uint32_t *readable, uint32_t *thr);
    extern int smo_obs_m7v_name_n(void);
    extern uint64_t smo_obs_main_base(void);
    static double next_sum;
    static int ann, code_done, name_done, d_done;
    uint64_t lrd[16][4];
    atomic_store_explicit(&g_m7v_on, m7v_on(), memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m7v] probe on (t=%us) dispatch-identity 8 gates: x8 0x7acd8c (tail2, "
                     "[vtable]->target = who we dispatch to) + rsv siblings r1..r7 "
                     "0x4a6a90/0x46e770/0x445590/0x447ae0/0x449bc0/0x44b6f0/0x4af020 (tail0 4w) "
                     "sw=%d ledger: count+first/last+per-LR16 + x8 ring8 + name ring8 x 4 slots raw hex "
                     "sum/rsv/d=10s code=10s name=10s",
                     tsec, (int)atomic_load_explicit(&g_m7v_on, memory_order_relaxed));
        next_sum = (double)tsec + 10.0;
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base();
        for (int gi = 0; gi < 8; gi++) {
            uint32_t a[8]; int ok = mb && runtime_safe_read_ungated(a, mb + m7v_g[gi].site, sizeof a);
            unsigned same = 0, pv = 0;
            if (ok) for (int i = 0; i < 3; i++) if (a[4 + i] == m7v_g[gi].aft[i]) same++;
            if (ok) { uint32_t p; pv = runtime_safe_read_ungated(&p, mb + m7v_g[gi].site - 4, 4) ? p : 0; }
            vk_boot_logf("[m7v] code t=%us g=%s site=main+%#x ok=%d win[0..3]=%08x %08x %08x %08x "
                         "exp=%08x %08x %08x %08x after(main+%#x..+8) same=%u/3 "
                         "prev(main+%#x)=%08x exp_prev=%08x "
                         "(g=x8: win[1] = 0x7acd90 the blr x8, must stay untouched)",
                         tsec, m7v_g[gi].nm, m7v_g[gi].site, ok,
                         ok ? a[0] : 0, ok ? a[1] : 0, ok ? a[2] : 0, ok ? a[3] : 0,
                         m7v_g[gi].win[0], m7v_g[gi].win[1], m7v_g[gi].win[2], m7v_g[gi].win[3],
                         m7v_g[gi].site + 0x10, same, m7v_g[gi].site - 4, pv, m7v_g[gi].prev);
        }
    }
    if (!d_done && (double)tsec >= 10.0) {
        int n = smo_obs_m7v_d_n();
        if (n > 0) {
            d_done = 1;
            for (int i = 0; i < n; i++) {
                uint64_t m[6] = { 0 }; uint32_t ok = 0, thr = 0;
                if (!smo_obs_m7v_d(i, m, &ok, &thr)) continue;
                char dl[64], dv[64], dt[64];
                runtime_describe_address(m[1], dl, sizeof dl);
                runtime_describe_address(m[3], dv, sizeof dv);
                if (m[4]) runtime_describe_address(m[4], dt, sizeof dt); else snprintf(dt, sizeof dt, "-");
                vk_boot_logf("[m7v] x8 #%d ns=%llus lr=%#llx(%s) scene=%#llx vtbl=%#llx(%s) "
                             "tgt=%#llx(%s) rd_ok=%u thr=%u",
                             i, (unsigned long long)(m[0] / 1000000000ull),
                             (unsigned long long)m[1], dl,
                             (unsigned long long)m[2], (unsigned long long)m[3], dv,
                             (unsigned long long)m[4], dt, ok, thr);
            }
        }
    }
    if (!name_done && (double)tsec >= 10.0) {
        int n = smo_obs_m7v_name_n();
        if (n > 0) {
            name_done = 1;
            for (int i = 0; i < n; i++) {
                uint64_t m[8] = { 0 }; unsigned char by[128] = { 0 };
                uint32_t av = 0, rd = 0, thr = 0; char h0[80], h1[80], h2[80], h3[80], dn[64];
                if (!smo_obs_m7v_name(i, m, by, &av, &rd, &thr)) continue;
                m7v_hex32(by, h0); m7v_hex32(by + 32, h1); m7v_hex32(by + 64, h2); m7v_hex32(by + 96, h3);
                if (m[2]) runtime_describe_address(m[2], dn, sizeof dn); else snprintf(dn, sizeof dn, "-");
                vk_boot_logf("[m7v] name #%d g=%s ns=%llus site_lr=%s this=%#llx name_ptr=%#llx "
                             "q_ptr=%#llx scene=%#llx vtbl=%#llx avail=%#x readable=%#x thr=%u "
                             "(raw hex of 4 slots, no rewrite)",
                             i, m[1] == 83 ? "a" : (m[1] == 85 ? "c" : "?"),
                             (unsigned long long)(m[0] / 1000000000ull), dn,
                             (unsigned long long)m[3], (unsigned long long)m[4],
                             (unsigned long long)m[5], (unsigned long long)m[6],
                             (unsigned long long)m[7], av, rd, thr);
                vk_boot_logf("[m7v] name #%d s0 name_ptr       =%s", i, h0);
                vk_boot_logf("[m7v] name #%d s1 name_ptr+0x10  =%s", i, h1);
                vk_boot_logf("[m7v] name #%d s2 name_ptr+0x18  =%s", i, h2);
                vk_boot_logf("[m7v] name #%d s3 [name_ptr]+0x10=%s", i, h3);
            }
        }
    }
    if ((double)tsec >= next_sum) {
        next_sum += 10.0;
        for (int gi = 0; gi < 8; gi++) {
            uint64_t tot = 0, first[6] = { 0 }, last[4] = { 0 }, ov = 0;
            int nlr = 0;
            (void)smo_obs_m7v(gi, &tot, lrd, &nlr, first, last, &ov);
            char dl[64]; dl[0] = 0;
            if (first[1]) runtime_describe_address(first[1], dl, sizeof dl);
            vk_boot_logf("[m7v] sum t=%us g=%s site=%#x tot=%llu first_ns=%llus first_lr=%s "
                         "first_x0=%#llx first_x1=%#llx first_x8=%#llx thr=%llu last_ns=%llus "
                         "last_lr=%#llx last_x0=%#llx last_x1=%#llx uniq_lr=%d lr_ovf=%llu",
                         tsec, m7v_g[gi].nm, m7v_g[gi].site,
                         (unsigned long long)tot, (unsigned long long)(first[0] / 1000000000ull),
                         dl[0] ? dl : "-",
                         (unsigned long long)first[2], (unsigned long long)first[3],
                         (unsigned long long)first[4], (unsigned long long)first[5],
                         (unsigned long long)(last[0] / 1000000000ull),
                         (unsigned long long)last[1], (unsigned long long)last[2],
                         (unsigned long long)last[3], nlr, (unsigned long long)ov);
            for (int i = 0; i < nlr && gi == 0; i++) {
                char d2[64]; d2[0] = 0;
                if (lrd[i][0]) runtime_describe_address(lrd[i][0], d2, sizeof d2);
                vk_boot_logf("[m7v] lr t=%us g=%s #%d lr=%#llx site=%#llx n=%llu first_ns=%llus "
                             "last_ns=%llus desc=%s",
                             tsec, m7v_g[gi].nm, i, (unsigned long long)lrd[i][0],
                             (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0),
                             (unsigned long long)lrd[i][1],
                             (unsigned long long)(lrd[i][2] / 1000000000ull),
                             (unsigned long long)(lrd[i][3] / 1000000000ull), d2[0] ? d2 : "-");
            }
        }
    }
}
/* ---- M7X2（2026-11-08）：**最后一跳** —— 两门（95 = main+0x74c9e0 slot-3 转调 thunk /
   96 = main+0x772500 描述符克隆）。M7W2 已把"两边派发链到 thunk 为止完全一致"钉死
   （真机 = 模拟器 = main+0x74c9e0，主相对逐字相同），并把真问题收窄成"谁把哪个对象绑进 +8"。
   本轮两门就是这两问：
   G1（id 95）门点 x0 = q（适配器 0x7724f0 `br x2` 尾跳进来、x0 已被 0x7724ec `mov x0,x8` 换成真 this）
      观察器解两层：vt=[q]、tgt3=[[q]+0x18]（= 0x74c9e4 `ldr x2,[x8,#0x18]` + 0x74c9e8 `br x2` 的真目标），
      再记字面字段 o18=[q+0x18] 与 arg=门点原 x1（stub 参4）⇒ **"真正干活的人"可 describe**。
   G2（id 96）门点 x0 = 源描述符（0x77250c `mov x19,x0`，函数体里的 x19 就是源），
      记 src / {+8,+0x10,+0x18,+0x20} 32 B（= 被写进新对象的 32 B，0x77252c/0x772530 的 ldp →
      0x772538/0x772534 的 stp 是纯拷贝）/ [src] / LR（调用者返回址，site=lr-4）
      ⇒ 回答"谁把哪个对象绑进 +8"。
   节拍：probe 一次；code1/code2（门窗逐字回读）t≥10 s 一次；g1/g2 环（≤16 条）每 10 s 增量；
   sum1/sum2 + per-LR 每 10 s。
   Switch smo_m7x2（C 默认 0；`echo 0 > .../files/smo_m7x2` ≤2 s 回退）。
   ⚠️ 名字用 m7x2：`smo_m7x` / `[m7x]` / `runtime_m7x_poll` / `m7x_boot_run.sh` /
   `/tmp/m7x_report.md` / `logs/m7x_b1` 是 2026-11-07 那轮的，全部已被占用，不覆盖。 */
#define M7X2_PATH_A "/data/storage/el2/base/files/smo_m7x2"
#define M7X2_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7x2"
__attribute__((constructor)) static void m7x2_seed(void) {
    if (!fopen(M7X2_PATH_A, "r")) {
        FILE *f = fopen(M7X2_PATH_A, "w");
        if (!f) f = fopen(M7X2_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7x2] seeded = 0 (off)\n"); }
    }
}
static int m7x2_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7X2_PATH_B : M7X2_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7x2_on;
/* 出厂字（/tmp/m7x2_safety.py 本轮逐字重扫：三镜像 md5 89d568fd… 全等、两门窗内 4 字全无 PC 相对、
   全镜像分支落点 FIRST=0/INTERIOR=0、与既有 87 行站表零重叠、两门前一字都是 00000000）。 */
static const uint32_t m7x2_g1_win1 = 0xf9400d02;                        /* 0x74c9e4 ldr x2,[x8,#0x18]（窗校验，不换） */
static const uint32_t m7x2_g1_br   = 0xd61f0040;                        /* 0x74c9e8 br x2（窗外，一个字节都不碰） */
static const uint32_t m7x2_g1_aft[2] = { 0xf9005001, 0xd65f03c0 };      /* 0x74c9f0 起下一个函数（str x1,[x0,#0xa0]; ret） */
static const uint32_t m7x2_g2_ven[2] = { 0x58000051, 0xd61f0220 };      /* tail=0 两字 veneer（ldr x17,#8; br x17） */
static const uint32_t m7x2_g2_aft[4] = { 0x52800500, 0x940730bf, 0xb0005fc8, 0xf9450908 };   /* 0x772510..0x77251c */
static const uint32_t m7x2_g2_neigh = 0xd61f0040;                      /* 0x7724f0 适配器 br x2（M7W2 的门，证明互不干扰） */
void runtime_m7x2_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7x2_g1(int i, uint64_t meta[7], uint32_t *rd, uint32_t *ok, uint32_t *thr);
    extern int smo_obs_m7x2_g1_n(void);
    extern int smo_obs_m7x2_g1_sum(uint64_t *total, uint64_t lrd[][4], int *nlr,
                                   uint64_t first[3], uint64_t last[3], uint64_t *lr_ovf);
    extern int smo_obs_m7x2_g2(int i, uint64_t meta[8], uint32_t *rd, uint32_t *thr);
    extern int smo_obs_m7x2_g2_n(void);
    extern int smo_obs_m7x2_g2_sum(uint64_t *total, uint64_t lrd[][4], int *nlr,
                                   uint64_t first[3], uint64_t last[3], uint64_t *lr_ovf);
    extern uint64_t smo_obs_main_base(void);
    static double next_sum, next_g1, next_g2;
    static int ann, code_done, g1_printed, g2_printed;
    uint64_t lrd[8][4];
    atomic_store_explicit(&g_m7x2_on, m7x2_on(), memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m7x2] probe on (t=%us) G1 id=95 main+0x74c9e0 slot-3 thunk (tail2: only word0 "
                     "'ldr x8,[x0]' replaced; 0x74c9e4 ldr x2,[x8,#0x18] + 0x74c9e8 br x2 untouched) "
                     "-> q/vt=[q]/tgt3=[[q]+0x18]/o18=[q+0x18]/arg, all described | G2 id=96 "
                     "main+0x772500 descriptor clone (tail0 4w: word0 'str x19,[sp,#-0x20]!' rewrites sp, "
                     "replayed after the stub epilogue; src = entry x0, 32B {+8,+0x10,+0x18,+0x20}) sw=%d "
                     "| code1/code2=10s g1/g2 ring=10s sum=10s",
                     tsec, (int)atomic_load_explicit(&g_m7x2_on, memory_order_relaxed));
        next_sum = (double)tsec + 10.0;
        next_g1 = (double)tsec + 10.0;
        next_g2 = (double)tsec + 10.0;
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base();
        uint32_t a[4] = { 0 }, aft[2] = { 0 }, pv = 0;
        int ok1 = mb && runtime_safe_read_ungated(a, mb + 0x74c9e0, sizeof a);
        if (mb) pv = runtime_safe_read_ungated(&pv, mb + 0x74c9e0 - 4, 4) ? pv : 0;
        int af1 = mb && runtime_safe_read_ungated(aft, mb + 0x74c9f0, sizeof aft);
        int w1s = ok1 && a[1] == m7x2_g1_win1;
        int brs = ok1 && a[2] == m7x2_g1_br;
        int afs = af1 && aft[0] == m7x2_g1_aft[0] && aft[1] == m7x2_g1_aft[1];
        vk_boot_logf("[m7x2] code1 t=%us site=main+0x74c9e0 ok=%d win[0]=%08x(veneer b) win[1]=%08x "
                     "exp_win1=%08x same=%d | br(main+0x74c9e8)=%08x exp=%08x same=%d | "
                     "prev(main+0x74c9dc)=%08x exp_prev=00000000 | next_fn(main+0x74c9f0..+4)=%08x %08x same=%d",
                     tsec, ok1, ok1 ? a[0] : 0, ok1 ? a[1] : 0, m7x2_g1_win1, w1s,
                     ok1 ? a[2] : 0, m7x2_g1_br, brs, pv,
                     af1 ? aft[0] : 0, af1 ? aft[1] : 0, afs);
        uint32_t b[4] = { 0 }, af2[4] = { 0 }, pv2 = 0, nb = 0;
        int ok2 = mb && runtime_safe_read_ungated(b, mb + 0x772500, sizeof b);
        if (mb) pv2 = runtime_safe_read_ungated(&pv2, mb + 0x7724fc, 4) ? pv2 : 0;
        if (mb) nb = runtime_safe_read_ungated(&nb, mb + 0x7724f0, 4) ? nb : 0;
        int af2ok = mb && runtime_safe_read_ungated(af2, mb + 0x772510, sizeof af2);
        int vens = ok2 && b[0] == m7x2_g2_ven[0] && b[1] == m7x2_g2_ven[1];
        unsigned same2 = 0;
        if (af2ok) for (int i = 0; i < 4; i++) if (af2[i] == m7x2_g2_aft[i]) same2++;
        vk_boot_logf("[m7x2] code2 t=%us site=main+0x772500 ok=%d win[0..3]=%08x %08x %08x %08x "
                     "exp_ven0..1=%08x %08x veneer_same=%d (win[2..3]=stub ptr) | "
                     "after(main+0x772510..+0x1c) same=%u/4 %08x %08x %08x %08x | "
                     "prev(main+0x7724fc)=%08x exp_prev=00000000 | neigh main+0x7724f0 br x2=%08x exp=%08x same=%d",
                     tsec, ok2, ok2 ? b[0] : 0, ok2 ? b[1] : 0, ok2 ? b[2] : 0, ok2 ? b[3] : 0,
                     m7x2_g2_ven[0], m7x2_g2_ven[1], vens,
                     same2, af2ok ? af2[0] : 0, af2ok ? af2[1] : 0, af2ok ? af2[2] : 0, af2ok ? af2[3] : 0,
                     pv2, nb, m7x2_g2_neigh, nb == m7x2_g2_neigh);
    }
    if ((double)tsec >= next_g1) {
        next_g1 += 10.0;
        int n = smo_obs_m7x2_g1_n();
        if (n > g1_printed) {
            for (int i = g1_printed; i < n; i++) {
                uint64_t m[7] = { 0 }; uint32_t rd = 0, ok = 0, thr = 0;
                if (!smo_obs_m7x2_g1(i, m, &rd, &ok, &thr)) continue;
                char dv[72], dt[72], do_[72], da[72], dl[72];
                runtime_describe_address(m[3], dv, sizeof dv);    /* vt = [q] */
                runtime_describe_address(m[4], dt, sizeof dt);    /* tgt3 = [[q]+0x18] = 真正干活的人 */
                runtime_describe_address(m[5], do_, sizeof do_);  /* o18 = [q+0x18]（字面字段） */
                runtime_describe_address(m[6], da, sizeof da);    /* arg = 门点原 x1 */
                runtime_describe_address(m[1], dl, sizeof dl);    /* lr */
                vk_boot_logf("[m7x2] g1 #%d ns=%llus q=%#llx vt=[q]=%#llx(%s) tgt3=[[q]+0x18]=%#llx(%s) "
                             "o18=[q+0x18]=%#llx(%s) arg=%#llx(%s) lr=%#llx(%s) rd=%#x ok=%u thr=%u",
                             i, (unsigned long long)(m[0] / 1000000000ull),
                             (unsigned long long)m[2],
                             (unsigned long long)m[3], dv,
                             (unsigned long long)m[4], dt,
                             (unsigned long long)m[5], do_,
                             (unsigned long long)m[6], da,
                             (unsigned long long)m[1], dl, rd, ok, thr);
            }
            g1_printed = n;
        }
    }
    if ((double)tsec >= next_g2) {
        next_g2 += 10.0;
        int n = smo_obs_m7x2_g2_n();
        if (n > g2_printed) {
            for (int i = g2_printed; i < n; i++) {
                uint64_t m[8] = { 0 }; uint32_t rd = 0, thr = 0;
                if (!smo_obs_m7x2_g2(i, m, &rd, &thr)) continue;
                char ds[72], dv[72], dl[72], dsite[72];
                runtime_describe_address(m[2], ds, sizeof ds);              /* src */
                runtime_describe_address(m[7], dv, sizeof dv);              /* [src] */
                runtime_describe_address(m[1], dl, sizeof dl);              /* lr */
                runtime_describe_address(m[1] ? m[1] - 4 : 0, dsite, sizeof dsite);   /* 调用点 = lr-4 */
                vk_boot_logf("[m7x2] g2 #%d ns=%llus src=%#llx(%s) [src]=%#llx(%s) "
                             "w+8=%#llx w+0x10=%#llx w+0x18=%#llx w+0x20=%#llx "
                             "(= the 32B written into the clone) lr=%#llx(%s) site=%s rd=%#x thr=%u",
                             i, (unsigned long long)(m[0] / 1000000000ull),
                             (unsigned long long)m[2], ds,
                             (unsigned long long)m[7], dv,
                             (unsigned long long)m[3], (unsigned long long)m[4],
                             (unsigned long long)m[5], (unsigned long long)m[6],
                             (unsigned long long)m[1], dl, dsite, rd, thr);
            }
            g2_printed = n;
        }
    }
    if ((double)tsec >= next_sum) {
        next_sum += 10.0;
        uint64_t tot = 0, first[3] = { 0 }, last[3] = { 0 }, ov = 0;
        int nlr = 0;
        (void)smo_obs_m7x2_g1_sum(&tot, lrd, &nlr, first, last, &ov);
        char df[72], dfl[72], dq[72];
        runtime_describe_address(first[1], df, sizeof df);
        runtime_describe_address(last[1], dfl, sizeof dfl);
        runtime_describe_address(last[2], dq, sizeof dq);
        vk_boot_logf("[m7x2] sum1 t=%us gate=95 hits=%llu first_ns=%llus first_lr=%#llx(%s) first_q=%#llx "
                     "last_ns=%llus last_lr=%#llx(%s) last_q=%#llx(%s) uniq_lr=%d lr_ovf=%llu ring_n=%d",
                     tsec, (unsigned long long)tot,
                     (unsigned long long)(first[0] / 1000000000ull), (unsigned long long)first[1], df,
                     (unsigned long long)first[2],
                     (unsigned long long)(last[0] / 1000000000ull), (unsigned long long)last[1], dfl,
                     (unsigned long long)last[2], dq, nlr, (unsigned long long)ov, smo_obs_m7x2_g1_n());
        for (int i = 0; i < nlr; i++) {
            char d2[72] = { 0 };
            if (lrd[i][0]) runtime_describe_address(lrd[i][0], d2, sizeof d2);
            vk_boot_logf("[m7x2] lr1 t=%us #%d lr=%#llx site=%#llx n=%llu first_ns=%llus last_ns=%llus desc=%s",
                         tsec, i, (unsigned long long)lrd[i][0],
                         (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0),
                         (unsigned long long)lrd[i][1],
                         (unsigned long long)(lrd[i][2] / 1000000000ull),
                         (unsigned long long)(lrd[i][3] / 1000000000ull), d2[0] ? d2 : "-");
        }
        uint64_t tot2 = 0, f2[3] = { 0 }, l2[3] = { 0 }, ov2 = 0;
        int nlr2 = 0;
        (void)smo_obs_m7x2_g2_sum(&tot2, lrd, &nlr2, f2, l2, &ov2);
        char ds2[72], dsl[72], dq2[72];
        runtime_describe_address(f2[1], ds2, sizeof ds2);
        runtime_describe_address(l2[1], dsl, sizeof dsl);
        runtime_describe_address(l2[2], dq2, sizeof dq2);
        vk_boot_logf("[m7x2] sum2 t=%us gate=96 hits=%llu first_ns=%llus first_lr=%#llx(%s) first_src=%#llx "
                     "last_ns=%llus last_lr=%#llx(%s) last_src=%#llx(%s) uniq_lr=%d lr_ovf=%llu ring_n=%d",
                     tsec, (unsigned long long)tot2,
                     (unsigned long long)(f2[0] / 1000000000ull), (unsigned long long)f2[1], ds2,
                     (unsigned long long)f2[2],
                     (unsigned long long)(l2[0] / 1000000000ull), (unsigned long long)l2[1], dsl,
                     (unsigned long long)l2[2], dq2, nlr2, (unsigned long long)ov2, smo_obs_m7x2_g2_n());
        for (int i = 0; i < nlr2; i++) {
            char d2[72] = { 0 };
            if (lrd[i][0]) runtime_describe_address(lrd[i][0], d2, sizeof d2);
            vk_boot_logf("[m7x2] lr2 t=%us #%d lr=%#llx site=%#llx n=%llu first_ns=%llus last_ns=%llus desc=%s",
                         tsec, i, (unsigned long long)lrd[i][0],
                         (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0),
                         (unsigned long long)lrd[i][1],
                         (unsigned long long)(lrd[i][2] / 1000000000ull),
                         (unsigned long long)(lrd[i][3] / 1000000000ull), d2[0] ? d2 : "-");
        }
    }
}

/* ---- M7Y2（2026-11-08）：**认那个"另一个类的对象"** —— 两门（97 = main+0x44adb0 /
   98 = main+0x44af80）。M7X2 已判定"绑定点/绑定者/源类/32 B 语义两边完全一致，变的是被绑进 +8 的
   q 属于不同的类"。本轮两门把真机那个族外终点变成时间序列，并在门点**直接读类名**：
   离线用 161,696 条加载期重定位行解出三族同构布局 —— 类名指针固定在 [q]+0xb8：
     真机 0x12bd470 → "18FirstSequenceScene"；模拟器 H7 0x12cd510 → "14TitleMenuScene"；
     H5 0x12c4dc8 该偏移不是名字（布局多一格，本轮如实记"未定"）。
   节拍：probe 一次；code1/code2（门窗逐字回读）t≥10 s 一次；g1/g2 环（≤16 条）每 10 s 增量；
   sum1/sum2 + per-LR 每 10 s。
   Switch smo_m7y2（C 默认 0；`echo 0 > .../files/smo_m7y2` ≤2 s 回退）。
   ⚠️ 名字用 m7y2：`smo_m7y` / `[m7y]` / `runtime_m7y_poll` / `m7y_boot_run.sh` /
   `/tmp/m7y_report.md` / `logs/m7y_m7y_b1` 是 2026-11-07 那轮的，全部已被占用，不覆盖。 */
#define M7Y2_PATH_A "/data/storage/el2/base/files/smo_m7y2"
#define M7Y2_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7y2"
__attribute__((constructor)) static void m7y2_seed(void) {
    if (!fopen(M7Y2_PATH_A, "r")) {
        FILE *f = fopen(M7Y2_PATH_A, "w");
        if (!f) f = fopen(M7Y2_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7y2] seeded = 0 (off)\n"); }
    }
}
static int m7y2_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7Y2_PATH_B : M7Y2_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7y2_on;
/* 出厂字（/tmp/m7y2_safety.py 本轮逐字重扫：三镜像 md5 89d568fd… 全等、两门窗内 4 字全无 PC 相对、
   全镜像分支落点 FIRST=0/INTERIOR=0、与既有 89 行站表零重叠、两门前一字 = 1413cf85(一条 b) / 00000000）。 */
static const uint32_t m7y2_g1_aft[4] = { 0xa9147bfd, 0x910503fd, 0xaa0103f4, 0xaa0003f3 };  /* 0x44adc0..0x44adcc */
static const uint32_t m7y2_g2_aft[4] = { 0xaa0003f3, 0x940c0648, 0xf00078c8, 0xf9460908 };  /* 0x44af8c..0x44af98 */
static const uint32_t m7y2_g1_prev = 0x1413cf85;   /* 0x44adac = 上一条函数的尾调 b（窗外、不碰） */
void runtime_m7y2_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7y2_g1(int i, uint64_t meta[7], uint32_t *rd, uint32_t *ok, uint32_t *thr,
                               unsigned char *nm, uint32_t *nmlen);
    extern int smo_obs_m7y2_g1_n(void);
    extern int smo_obs_m7y2_g1_sum(uint64_t *total, uint64_t lrd[][4], int *nlr,
                                   uint64_t first[3], uint64_t last[3], uint64_t *lr_ovf);
    extern int smo_obs_m7y2_g2(int i, uint64_t meta[6], uint32_t *rd, uint32_t *thr,
                               unsigned char *nm, uint32_t *nmlen);
    extern int smo_obs_m7y2_g2_n(void);
    extern int smo_obs_m7y2_g2_sum(uint64_t *total, uint64_t lrd[][4], int *nlr,
                                   uint64_t first[3], uint64_t last[3], uint64_t *lr_ovf);
    extern uint64_t smo_obs_main_base(void);
    static double next_sum, next_g1, next_g2;
    static int ann, code_done, g1_printed, g2_printed;
    uint64_t lrd[8][4];
    atomic_store_explicit(&g_m7y2_on, m7y2_on(), memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m7y2] probe on (t=%us) G1 id=97 main+0x44adb0 device slot3 terminal (tail0 4w: "
                     "sub sp,sp,#0x150 rewrites sp, replayed after the stub epilogue) -> q/vt/tgt3/"
                     "name=[q+0xb8]/arg/LR | G2 id=98 main+0x44af80 same class slot +0x20 (scene-layer setup) "
                     "-> vt/name/arg/LR | code1/code2=10s g1/g2 ring=10s sum=10s sw=%d",
                     tsec, (int)atomic_load_explicit(&g_m7y2_on, memory_order_relaxed));
        next_sum = (double)tsec + 10.0;
        next_g1 = (double)tsec + 10.0;
        next_g2 = (double)tsec + 10.0;
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base();
        uint32_t a[4] = { 0 }, af[4] = { 0 }, pv = 0, rw = 0;
        int ok1 = mb && runtime_safe_read_ungated(a, mb + 0x44adb0, sizeof a);
        if (mb) pv = runtime_safe_read_ungated(&pv, mb + 0x44adb0 - 4, 4) ? pv : 0;
        if (mb) rw = runtime_safe_read_ungated(&rw, mb + 0x44af70, 4) ? rw : 0;
        int af1 = mb && runtime_safe_read_ungated(af, mb + 0x44adc0, sizeof af);
        unsigned same1 = 0;
        if (af1) for (int i = 0; i < 4; i++) if (af[i] == m7y2_g1_aft[i]) same1++;
        vk_boot_logf("[m7y2] code1 t=%us site=main+0x44adb0 ok=%d win[0..3]=%08x %08x %08x %08x "
                     "exp= under g1 window | after(main+0x44adc0..+0x1cc) same=%u/4 %08x %08x %08x %08x | "
                     "prev(main+0x44adac)=%08x exp_prev=%08x same=%d | ret(main+0x44af70)=%08x exp=d65f03c0",
                     tsec, ok1, ok1 ? a[0] : 0, ok1 ? a[1] : 0, ok1 ? a[2] : 0, ok1 ? a[3] : 0,
                     same1, af1 ? af[0] : 0, af1 ? af[1] : 0, af1 ? af[2] : 0, af1 ? af[3] : 0,
                     pv, m7y2_g1_prev, pv == m7y2_g1_prev, rw);
        uint32_t b[4] = { 0 }, bf[4] = { 0 }, pv2 = 0;
        int ok2 = mb && runtime_safe_read_ungated(b, mb + 0x44af80, sizeof b);
        if (mb) pv2 = runtime_safe_read_ungated(&pv2, mb + 0x44af80 - 4, 4) ? pv2 : 0;
        int bfok = mb && runtime_safe_read_ungated(bf, mb + 0x44af8c, sizeof bf);
        unsigned same2 = 0;
        if (bfok) for (int i = 0; i < 4; i++) if (bf[i] == m7y2_g2_aft[i]) same2++;
        vk_boot_logf("[m7y2] code2 t=%us site=main+0x44af80 ok=%d win[0..3]=%08x %08x %08x %08x "
                     "after(main+0x44af8c..+0x198) same=%u/4 %08x %08x %08x %08x | prev(main+0x44af7c)=%08x exp=00000000",
                     tsec, ok2, ok2 ? b[0] : 0, ok2 ? b[1] : 0, ok2 ? b[2] : 0, ok2 ? b[3] : 0,
                     same2, bfok ? bf[0] : 0, bfok ? bf[1] : 0, bfok ? bf[2] : 0, bfok ? bf[3] : 0, pv2);
    }
    if ((double)tsec >= next_g1) {
        next_g1 += 10.0;
        int n = smo_obs_m7y2_g1_n();
        if (n > g1_printed) {
            for (int i = g1_printed; i < n; i++) {
                uint64_t m[7] = { 0 }; uint32_t rd = 0, ok = 0, thr = 0, nmlen = 0;
                unsigned char nm[24] = { 0 }; char ns[28];
                if (!smo_obs_m7y2_g1(i, m, &rd, &ok, &thr, nm, &nmlen)) continue;
                int kk = 0;
                for (uint32_t k = 0; k < nmlen && k < 24 && kk < 24; k++) {
                    unsigned char c = nm[k];
                    ns[kk++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
                }
                ns[kk] = 0;
                char dv[72], dt[72], dl[72], dn[72], da[72];
                runtime_describe_address(m[3], dv, sizeof dv);
                runtime_describe_address(m[4], dt, sizeof dt);
                runtime_describe_address(m[5], dn, sizeof dn);
                runtime_describe_address(m[1], dl, sizeof dl);
                runtime_describe_address(m[6], da, sizeof da);
                vk_boot_logf("[m7y2] g1 #%d ns=%llus q=%#llx vt=[q]=%#llx(%s) tgt3=[[q]+0x18]=%#llx(%s) "
                             "name_ptr=[q+0xb8]=%#llx(%s) CLS=%s len=%u arg=%#llx(%s) lr=%#llx(%s) rd=%#x ok=%u thr=%u",
                             i, (unsigned long long)(m[0] / 1000000000ull),
                             (unsigned long long)m[2], (unsigned long long)m[3], dv,
                             (unsigned long long)m[4], dt,
                             (unsigned long long)m[5], dn, ns[0] ? ns : "-", nmlen,
                             (unsigned long long)m[6], da,
                             (unsigned long long)m[1], dl, rd, ok, thr);
            }
            g1_printed = n;
        }
    }
    if ((double)tsec >= next_g2) {
        next_g2 += 10.0;
        int n = smo_obs_m7y2_g2_n();
        if (n > g2_printed) {
            for (int i = g2_printed; i < n; i++) {
                uint64_t m[6] = { 0 }; uint32_t rd = 0, thr = 0, nmlen = 0;
                unsigned char nm[24] = { 0 }; char ns[28];
                if (!smo_obs_m7y2_g2(i, m, &rd, &thr, nm, &nmlen)) continue;
                int kk = 0;
                for (uint32_t k = 0; k < nmlen && k < 24 && kk < 24; k++) {
                    unsigned char c = nm[k];
                    ns[kk++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
                }
                ns[kk] = 0;
                char dv[72], dl[72], dn[72], da[72];
                runtime_describe_address(m[3], dv, sizeof dv);
                runtime_describe_address(m[4], dn, sizeof dn);
                runtime_describe_address(m[1], dl, sizeof dl);
                runtime_describe_address(m[5], da, sizeof da);
                vk_boot_logf("[m7y2] g2 #%d ns=%llus q=%#llx vt=[q]=%#llx(%s) name_ptr=%#llx(%s) CLS=%s len=%u "
                             "arg=%#llx(%s) lr=%#llx(%s) rd=%#x thr=%u",
                             i, (unsigned long long)(m[0] / 1000000000ull),
                             (unsigned long long)m[2], (unsigned long long)m[3], dv,
                             (unsigned long long)m[4], dn, ns[0] ? ns : "-", nmlen,
                             (unsigned long long)m[5], da, (unsigned long long)m[1], dl, rd, thr);
            }
            g2_printed = n;
        }
    }
    if ((double)tsec >= next_sum) {
        next_sum += 10.0;
        uint64_t tot = 0, f[3] = { 0 }, l[3] = { 0 }, ov = 0;
        int nlr = 0;
        (void)smo_obs_m7y2_g1_sum(&tot, lrd, &nlr, f, l, &ov);
        char df[72], dl[72], dq[72];
        runtime_describe_address(f[1], df, sizeof df);
        runtime_describe_address(l[1], dl, sizeof dl);
        runtime_describe_address(l[2], dq, sizeof dq);
        vk_boot_logf("[m7y2] sum1 t=%us gate=97 hits=%llu first_ns=%llus first_lr=%#llx(%s) first_q=%#llx "
                     "last_ns=%llus last_lr=%#llx(%s) last_q=%#llx(%s) uniq_lr=%d lr_ovf=%llu ring_n=%d",
                     tsec, (unsigned long long)tot,
                     (unsigned long long)(f[0] / 1000000000ull), (unsigned long long)f[1], df,
                     (unsigned long long)f[2],
                     (unsigned long long)(l[0] / 1000000000ull), (unsigned long long)l[1], dl,
                     (unsigned long long)l[2], dq, nlr, (unsigned long long)ov, smo_obs_m7y2_g1_n());
        for (int i = 0; i < nlr; i++) {
            char d2[72] = { 0 };
            if (lrd[i][0]) runtime_describe_address(lrd[i][0], d2, sizeof d2);
            vk_boot_logf("[m7y2] lr1 t=%us #%d lr=%#llx site=%#llx n=%llu first_ns=%llus last_ns=%llus desc=%s",
                         tsec, i, (unsigned long long)lrd[i][0],
                         (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0),
                         (unsigned long long)lrd[i][1],
                         (unsigned long long)(lrd[i][2] / 1000000000ull),
                         (unsigned long long)(lrd[i][3] / 1000000000ull), d2[0] ? d2 : "-");
        }
        uint64_t tot2 = 0, f2[3] = { 0 }, l2[3] = { 0 }, ov2 = 0;
        int nlr2 = 0;
        (void)smo_obs_m7y2_g2_sum(&tot2, lrd, &nlr2, f2, l2, &ov2);
        char ds2[72], dsl[72], dq2[72];
        runtime_describe_address(f2[1], ds2, sizeof ds2);
        runtime_describe_address(l2[1], dsl, sizeof dsl);
        runtime_describe_address(l2[2], dq2, sizeof dq2);
        vk_boot_logf("[m7y2] sum2 t=%us gate=98 hits=%llu first_ns=%llus first_lr=%#llx(%s) first_q=%#llx "
                     "last_ns=%llus last_lr=%#llx(%s) last_q=%#llx(%s) uniq_lr=%d lr_ovf=%llu ring_n=%d",
                     tsec, (unsigned long long)tot2,
                     (unsigned long long)(f2[0] / 1000000000ull), (unsigned long long)f2[1], ds2,
                     (unsigned long long)f2[2],
                     (unsigned long long)(l2[0] / 1000000000ull), (unsigned long long)l2[1], dsl,
                     (unsigned long long)l2[2], dq2, nlr2, (unsigned long long)ov2, smo_obs_m7y2_g2_n());
        for (int i = 0; i < nlr2; i++) {
            char d2[72] = { 0 };
            if (lrd[i][0]) runtime_describe_address(lrd[i][0], d2, sizeof d2);
            vk_boot_logf("[m7y2] lr2 t=%us #%d lr=%#llx site=%#llx n=%llu first_ns=%llus last_ns=%llus desc=%s",
                         tsec, i, (unsigned long long)lrd[i][0],
                         (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0),
                         (unsigned long long)lrd[i][1],
                         (unsigned long long)(lrd[i][2] / 1000000000ull),
                         (unsigned long long)(lrd[i][3] / 1000000000ull), d2[0] ? d2 : "-");
        }
    }
}

/* ---- M7Z2（2026-11-08）：**选择点分叉** —— 五门（99..103）。
   离线定性（/tmp/m7z2_dis_a.py..k.py，镜像 md5 89d568fd…）：
     HostType 的 nerve 链（.bss 0x12d0d28..，加载期 reloc 行填出，token = 名字格 −0x10）：
       NrvBoot(0x12d0d48, exec 0x4b93a0→0x4b8d30) → NrvLoadFirstSequence(0x12d0e08, exec 0x4b93d0→0x4b8e20)
       → NrvFirstSequence(0x12d0e48, exec 0x4b93e0→0x4b8ef0) → NrvLoad(0x12d0e88) → …
     选择点在 **HostTypeNrvLoadFirstSequence::execute (0x4b8e20)**：
       0x4b8e80 ldrb w8,[x19,#0x58] / 0x4b8e84 cbnz w8,#0x4b8ea4
       0x4b8e88 mov w1,#0x21c / 0x4b8e8c mov x0,x19 / 0x4b8e90 bl 0x6c7d30
       0x4b8e94 tbz w0,#0,#0x4b8ea4   ← 查询返回 0 才去做活
       0x4b8e98 ldp/ldr/ret            ← 跳过（每帧一次，直到条件成立）
       0x4b8ea4 … blr [[this+0x20]+0x20] … b 0x6c7b20(this,0x12d0e48) 转 NrvFirstSequence
     ⇒ 做活条件 = **[this+0x58] 非 0 或 state_time > 0x21c(540 帧)**；
       [this+0x58] 的唯一"置 1"写点 = 0x4b8d20（`mov w8,#1; strb w8,[x0,#0x58]; ret`），
       两个调用者 0x4b51dc / 0x4b6a54 都以 x0 = [宿主+0xc8] 传入。
   节拍：probe 一次；code1..code5（门窗逐字回读）t≥10 s 一次；sum/lr 每 10 s；环 t≈20 s 与 t≈110 s 各一次。
   Switch smo_m7z2（C 默认 0；`echo 0 > .../files/smo_m7z2` ≤2 s 回退）。
   ⚠️ 名字用 m7z2：`smo_m7z`/`[m7z]`（threads.c 活代码）/`m7z_boot_run.sh`/`/tmp/m7z_report.md`/`logs/m7z_*`
   五类开工前已核，`[m7z]` 被 2026-11-07 那轮的活代码占用 ⇒ 本轮一律 m7z2，旧的一字未动。 */
#define M7Z2_PATH_A "/data/storage/el2/base/files/smo_m7z2"
#define M7Z2_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7z2"
__attribute__((constructor)) static void m7z2_seed(void) {
    if (!fopen(M7Z2_PATH_A, "r")) {
        FILE *f = fopen(M7Z2_PATH_A, "w");
        if (!f) f = fopen(M7Z2_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7z2] seeded = 0 (off)\n"); }
    }
}
static int m7z2_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7Z2_PATH_B : M7Z2_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7z2_on;
/* 出厂字（/tmp/m7z2_safety.py 逐门重扫：三镜像 md5 89d568fd… 全等、五门字节零重叠、
   与既有 91 行站表 ±0x20 零重叠、G1/G3/G5 窗内 0/4 PC 相对、G3 的 bl 由 replay_fix 重定位、
   INTERIOR 落点全 0）。post[] = 门占位段之后的 4 个字（运行期逐字回读，证明窗外一字未动）。 */
static const uint32_t m7z2_post1[4] = { 0xf9400108, 0x2a0103f3, 0xd63f0100, 0xb9401808 };  /* 0x6c7d40..0x6c7d4c */
static const uint32_t m7z2_post2[4] = { 0xf84207f3, 0xd65f03c0, 0xa9418660, 0x940ae502 };  /* 0x4b8e9c..0x4b8ea8 */
static const uint32_t m7z2_post3[4] = { 0xf9401108, 0xd63f0100, 0xa941a668, 0xf9004109 };  /* 0x4b8eb4..0x4b8ec0 */
static const uint32_t m7z2_post4[4] = { 0xaa1303e0, 0x94083ba8, 0x36000080, 0xa9417bfd };  /* 0x4b8e8c..0x4b8e98 */
static const uint32_t m7z2_post5[4] = { 0xf9400c00, 0x97ffece7, 0xaa1303e0, 0x94083ba1 };  /* 0x4b8e30..0x4b8e3c */
void runtime_m7z2_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7z2_n(int gi);
    extern int smo_obs_m7z2_rec(int gi, int i, uint64_t meta[10], uint32_t *thr);
    extern int smo_obs_m7z2_sum(int gi, uint64_t *k_all, uint64_t *k_site, uint64_t lrd[][4], int *nlr,
                                uint64_t first[10], uint64_t last[10], uint64_t *lr_ovf, uint64_t *rot);
    extern int smo_obs_m7z2_lrlast(int gi, int slot, uint64_t *lr, uint64_t *n, uint64_t *x0, uint64_t *x8,
                                   uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d, uint64_t *e);
    extern uint64_t smo_obs_main_base(void);
    static const uint32_t site[5] = { 0x6c7d30, 0x4b8e98, 0x4b8ea4, 0x4b8e88, 0x4b8e20 };
    static const uint32_t gateid[5] = { 99, 100, 101, 102, 103 };
    static const uint32_t tail[5] = { 0, 2, 0, 2, 0 };
    static const uint32_t *post[5] = { m7z2_post1, m7z2_post2, m7z2_post3, m7z2_post4, m7z2_post5 };
    static double next_sum;
    static int ann, code_done, ring_early, ring_late;
    uint64_t lrd[8][4];
    atomic_store_explicit(&g_m7z2_on, m7z2_on(), memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m7z2] probe on (t=%us) G1 id=99 main+0x6c7d30 predicate-in (tail0 4w) | "
                     "G2 id=100 main+0x4b8e98 skip-path (tail2 1w) | G3 id=101 main+0x4b8ea4 work-path (tail0 4w) | "
                     "G4 id=102 main+0x4b8e88 cbnz-not-taken (tail2 1w, x8=[this+0x58]) | "
                     "G5 id=103 main+0x4b8e20 NrvLoadFirstSequence::execute entry (tail0 4w, reads host+0x58/+0x20/+0x60) "
                     "| code=10s sum=10s ring=20/110s sw=%d",
                     tsec, (int)atomic_load_explicit(&g_m7z2_on, memory_order_relaxed));
        next_sum = (double)tsec + 10.0;
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base();
        for (int gi = 0; gi < 5; gi++) {
            uint32_t span = tail[gi] ? 1u : 4u;
            uint32_t win[4] = { 0 }, aft[4] = { 0 }, pv = 0;
            int ok = mb && runtime_safe_read_ungated(win, mb + site[gi], span * 4);
            if (mb) pv = runtime_safe_read_ungated(&pv, mb + site[gi] - 4, 4) ? pv : 0;
            int aok = mb && runtime_safe_read_ungated(aft, mb + site[gi] + span * 4, sizeof aft);
            unsigned same = 0;
            if (aok) for (int i = 0; i < 4; i++) if (aft[i] == post[gi][i]) same++;
            vk_boot_logf("[m7z2] code%d t=%us site=main+0x%x gate=%u tail=%u ok=%d win=%08x %08x %08x %08x | "
                         "post(main+0x%x..) same=%u/4 %08x %08x %08x %08x | prev(main+0x%x)=%08x",
                         gi + 1, tsec, site[gi], gateid[gi], tail[gi], ok,
                         ok ? win[0] : 0, ok && span > 1 ? win[1] : 0,
                         ok && span > 2 ? win[2] : 0, ok && span > 3 ? win[3] : 0,
                         site[gi] + span * 4, same,
                         aok ? aft[0] : 0, aok ? aft[1] : 0, aok ? aft[2] : 0, aok ? aft[3] : 0,
                         site[gi] - 4, pv);
        }
    }
    if ((double)tsec >= next_sum) {
        next_sum += 10.0;
        for (int gi = 0; gi < 5; gi++) {
            uint64_t kall = 0, ksite = 0, f[10] = { 0 }, l[10] = { 0 }, ov = 0, rot = 0;
            int nlr = 0;
            (void)smo_obs_m7z2_sum(gi, &kall, &ksite, lrd, &nlr, f, l, &ov, &rot);
            char dl[72] = { 0 }, dfl[72] = { 0 };
            if (l[1]) runtime_describe_address(l[1], dl, sizeof dl);
            if (f[1]) runtime_describe_address(f[1], dfl, sizeof dfl);
            vk_boot_logf("[m7z2] sum%d t=%us gate=%u hits=%llu site_hits=%llu "
                         "first_ns=%llus first_lr=%#llx(%s) first_x0=%#llx first_x1=%#llx first_x8=%#llx "
                         "first_a=%#llx first_b=%#llx first_c=%#llx first_d=%#llx first_e=%#llx | "
                         "last_ns=%llus last_lr=%#llx(%s) last_x0=%#llx last_x1=%#llx last_x8=%#llx "
                         "last_a=%#llx last_b=%#llx last_c=%#llx last_d=%#llx last_e=%#llx | "
                         "uniq_lr=%d lr_ovf=%llu ring=%llu",
                         gi + 1, tsec, gateid[gi], (unsigned long long)kall, (unsigned long long)ksite,
                         (unsigned long long)(f[0] / 1000000000ull), (unsigned long long)f[1], dfl[0] ? dfl : "-",
                         (unsigned long long)f[2], (unsigned long long)f[3], (unsigned long long)f[4],
                         (unsigned long long)f[5], (unsigned long long)f[6], (unsigned long long)f[7],
                         (unsigned long long)f[8], (unsigned long long)f[9],
                         (unsigned long long)(l[0] / 1000000000ull), (unsigned long long)l[1], dl[0] ? dl : "-",
                         (unsigned long long)l[2], (unsigned long long)l[3], (unsigned long long)l[4],
                         (unsigned long long)l[5], (unsigned long long)l[6], (unsigned long long)l[7],
                         (unsigned long long)l[8], (unsigned long long)l[9],
                         nlr, (unsigned long long)ov, (unsigned long long)rot);
            for (int i = 0; i < nlr; i++) {
                uint64_t rlr = 0, rn = 0, x0 = 0, x8 = 0, a = 0, b = 0, c = 0, d = 0, e = 0;
                if (!smo_obs_m7z2_lrlast(gi, i, &rlr, &rn, &x0, &x8, &a, &b, &c, &d, &e)) continue;
                char d2[72] = { 0 };
                if (rlr) runtime_describe_address(rlr, d2, sizeof d2);
                vk_boot_logf("[m7z2] lr%d t=%us gate=%u #%d lr=%#llx site=%#llx(%s) n=%llu "
                             "first_ns=%llus last_ns=%llus last_x0=%#llx last_x8=%#llx "
                             "last_a=%#llx last_b=%#llx last_c=%#llx last_d=%#llx last_e=%#llx",
                             gi + 1, tsec, gateid[gi], i, (unsigned long long)rlr,
                             (unsigned long long)(rlr ? rlr - 4 : 0), d2[0] ? d2 : "-",
                             (unsigned long long)rn,
                             (unsigned long long)(lrd[i][2] / 1000000000ull),
                             (unsigned long long)(lrd[i][3] / 1000000000ull),
                             (unsigned long long)x0, (unsigned long long)x8,
                             (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
                             (unsigned long long)d, (unsigned long long)e);
            }
        }
    }
    {   /* 环：t≈20 s（早期 16 条）与 t≈110 s（最近 16 条）各打一次 */
        int want = 0;
        if (!ring_early && (double)tsec >= 20.0) { ring_early = 1; want = 1; }
        if (!ring_late && (double)tsec >= 110.0) { ring_late = 1; want = 1; }
        if (want) {
            for (int gi = 0; gi < 5; gi++) {
                int n = smo_obs_m7z2_n(gi);
                if (n > 16) n = 16;
                for (int i = 0; i < n; i++) {
                    uint64_t m[10] = { 0 }; uint32_t thr = 0;
                    if (!smo_obs_m7z2_rec(gi, i, m, &thr)) continue;
                    char dl[72] = { 0 };
                    if (m[1]) runtime_describe_address(m[1], dl, sizeof dl);
                    vk_boot_logf("[m7z2] g%d t=%us gate=%u #%d ns=%llus lr=%#llx(%s) x0=%#llx x1=%#llx x8=%#llx "
                                 "a=%#llx b=%#llx c=%#llx d=%#llx e=%#llx thr=%u",
                                 gi + 1, tsec, gateid[gi], i, (unsigned long long)(m[0] / 1000000000ull),
                                 (unsigned long long)m[1], dl[0] ? dl : "-",
                                 (unsigned long long)m[2], (unsigned long long)m[3], (unsigned long long)m[4],
                                 (unsigned long long)m[5], (unsigned long long)m[6], (unsigned long long)m[7],
                                 (unsigned long long)m[8], (unsigned long long)m[9], thr);
                }
            }
        }
    }
}

/* ---- M8B（2026-11-08）：**序列对象 nerve 状态机** —— 两门（104 = main+0x6c7750 NerveKeeper::setNerve
   / 105 = main+0x4b8d30 HostTypeNrvBoot::execute 入口）。
   离线定性（/tmp/m8b_work/*.py，镜像 md5 89d568fd…，161,696 行加载期 reloc 全量扫）：
     * nerve 对象 base B：+0x00 exec 入口 / +0x08 0x2020 / +0x10（派生型自指针）/ **+0x20 名字串** /
       +0x28 0x119a308；**token = B+0x10 ⇒ 名字格 = token+0x10**（M8A 的"token = 名字格 − 0x10"复核通过）。
     * 名字串是 Itanium 型名（`N12_GLOBAL__N_128HostTypeNrvLoadFirstSequenceE`）。日志里给"短名"
       = 去掉前缀到 `GLOBAL__N_1<长度>` 之后、再去掉末尾 `E` 终止符（纯字符串变换，可逐字回溯原串）。
     * 三条 nerve 写入路径都落在 0x6c7750：① 直调 bl（12 条）② 0x6c7b48 = setNerveByState(0x6c7b20) 尾跳
       ③ 0x6c8da0/0x6c8e0c = tryChangeNerve(0x6c8d80) ⇒ G1 是全量 nerve 写入的唯一观测点。
   节拍：probe 一次；code（门窗逐字回读）t≥10 s 一次；sum 每 5 s；token 表 t≥12 s 与每 20 s；
        环（首 128 条 + 最近 16 条）t≈20/60/100/140 s。
   Switch smo_m8b（C 默认 0；`echo 0 > .../files/smo_m8b` ≤2 s 回退）。
   ⚠️ 名字用 m8b：五类开工前全 0 命中（2026-11-08）；`smo_m7b`/`[m7b]` 是 2026-11-07 那轮
   batch#3 的活代码，本轮一字未动。 */
#define M8B_NAMELEN_LOG 80
#define M8B_PATH_A "/data/storage/el2/base/files/smo_m8b"
#define M8B_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m8b"
__attribute__((constructor)) static void m8b_seed(void) {
    if (!fopen(M8B_PATH_A, "r")) {
        FILE *f = fopen(M8B_PATH_A, "w");
        if (!f) f = fopen(M8B_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m8b] seeded = 0 (off)\n"); }
    }
}
static int m8b_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M8B_PATH_B : M8B_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m8b_on;
/* Itanium 型名 → 短名（纯字符串变换，不改字节、可逐字回溯）：去 `GLOBAL__N_1<len>` 前缀与末尾 `E`。
   例：`N12_GLOBAL__N_128HostTypeNrvLoadFirstSequenceE` → `HostTypeNrvLoadFirstSequence`。 */
static void m8b_short(const char *raw, char *out, size_t n) {
    if (n == 0) return;
    out[0] = 0;
    if (!raw || !raw[0]) return;
    const char *p = strstr(raw, "GLOBAL__N_1");
    if (p) {
        p += 11;
        while (*p >= '0' && *p <= '9') p++;
    } else p = raw;
    size_t L = strlen(p);
    if (L && p[L - 1] == 'E') L--;
    if (L >= n) L = n - 1;
    memcpy(out, p, L); out[L] = 0;
}
/* 出厂字（/tmp/m8b_work/s_safety.py：两门窗与镜像逐字相同、窗内 PC 相对 0/4、INTERIOR 落点 0、
   与既有 96 行站表 ±0x20 零重叠）。post[] = 门占位段之后 4 字（运行期逐字回读，证明窗外一字未动）。 */
static const uint32_t m8b_post1[4] = { 0xaa0103f4, 0xaa0003f3, 0x37f800e8, 0xf9400660 };  /* 0x6c7760..0x6c776c */
static const uint32_t m8b_post2[4] = { 0xaa0003f3, 0x94083bdf, 0x360004a0, 0x320003e1 };  /* 0x4b8d40..0x4b8d4c */
void runtime_m8b_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m8b_n(int gi);
    extern int smo_obs_m8b_rec(int gi, int i, uint64_t meta[6], char *name, int namelen);
    extern int smo_obs_m8b_last(int gi, int i, uint64_t meta[6], char *name, int namelen);
    extern int smo_obs_m8b_sum(int gi, uint64_t *k, uint64_t first[6], uint64_t last[6],
                               char *fname, char *lname, int nl, uint64_t lrd[][4], int *nlr,
                               uint64_t *lr_ovf, uint64_t *rot, uint64_t *lrot);
    extern int smo_obs_m8b_tokn(int gi);
    extern int smo_obs_m8b_tok(int gi, int i, uint64_t *token, uint64_t *n, uint64_t *ft, uint64_t *lt,
                               uint64_t *llr, uint64_t *keeper, char *name, int namelen);
    extern uint64_t smo_obs_m8b_tokovf(int gi);
    extern uint64_t smo_obs_main_base(void);
    static const uint32_t site[2] = { 0x6c7750, 0x4b8d30 };
    static const uint32_t gateid[2] = { 104, 105 };
    static const uint32_t *post[2] = { m8b_post1, m8b_post2 };
    static double next_sum, next_tok;
    static int ann, code_done, ring20, ring60, ring100, ring140, tok_first;
    uint64_t lrd[16][4];
    atomic_store_explicit(&g_m8b_on, m8b_on(), memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m8b] probe on (t=%us) G1 id=104 main+0x6c7750 NerveKeeper::setNerve (tail0 4w; "
                     "x1=new token / [keeper+0x10]=old token / [keeper+0x18]=old step / LR / tick; "
                     "name cell = token+0x10) | G2 id=105 main+0x4b8d30 HostTypeNrvBoot::execute entry "
                     "(tail0 4w; count + LR only) | code=10s sum=5s tok=12s+20s ring=20/60/100/140s sw=%d",
                     tsec, (int)atomic_load_explicit(&g_m8b_on, memory_order_relaxed));
        next_sum = (double)tsec + 5.0;
        next_tok = (double)tsec + 12.0;
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base();
        for (int gi = 0; gi < 2; gi++) {
            uint32_t win[4] = { 0 }, aft[4] = { 0 }, pv = 0;
            int ok = mb && runtime_safe_read_ungated(win, mb + site[gi], sizeof win);
            if (mb) pv = runtime_safe_read_ungated(&pv, mb + site[gi] - 4, 4) ? pv : 0;
            int aok = mb && runtime_safe_read_ungated(aft, mb + site[gi] + 0x10, sizeof aft);
            unsigned same = 0;
            if (aok) for (int i = 0; i < 4; i++) if (aft[i] == post[gi][i]) same++;
            vk_boot_logf("[m8b] code%d t=%us site=main+0x%x gate=%u tail=0 ok=%d win=%08x %08x %08x %08x | "
                         "post(main+0x%x..) same=%u/4 %08x %08x %08x %08x | prev(main+0x%x)=%08x",
                         gi + 1, tsec, site[gi], gateid[gi], ok,
                         ok ? win[0] : 0, ok ? win[1] : 0, ok ? win[2] : 0, ok ? win[3] : 0,
                         site[gi] + 0x10, same,
                         aok ? aft[0] : 0, aok ? aft[1] : 0, aok ? aft[2] : 0, aok ? aft[3] : 0,
                         site[gi] - 4, pv);
        }
    }
    if ((double)tsec >= next_sum) {
        next_sum += 5.0;
        for (int gi = 0; gi < 2; gi++) {
            uint64_t k = 0, f[6] = { 0 }, l[6] = { 0 }, ov = 0, rot = 0, lrot = 0;
            int nlr = 0;
            char fr[M8B_NAMELEN_LOG] = { 0 }, lr_[M8B_NAMELEN_LOG] = { 0 }, fs[M8B_NAMELEN_LOG] = { 0 }, ls[M8B_NAMELEN_LOG] = { 0 };
            (void)smo_obs_m8b_sum(gi, &k, f, l, fr, lr_, (int)sizeof fr, lrd, &nlr, &ov, &rot, &lrot);
            m8b_short(fr, fs, sizeof fs);
            m8b_short(lr_, ls, sizeof ls);
            char dfl[72] = { 0 }, dl[72] = { 0 };
            if (f[4]) runtime_describe_address(f[4], dfl, sizeof dfl);
            if (l[4]) runtime_describe_address(l[4], dl, sizeof dl);
            vk_boot_logf("[m8b] sum%d t=%us gate=%u hits=%llu events=%llu | "
                         "first_ns=%llus first_token=%#llx(%s) first_old10=%#llx first_old18=%#llx(int32 %d) "
                         "first_lr=%#llx(%s) first_keeper=%#llx | last_ns=%llus last_token=%#llx(%s) last_old10=%#llx "
                         "last_old18=%#llx(int32 %d) last_lr=%#llx(%s) last_keeper=%#llx | uniq_lr=%d lr_ovf=%llu",
                         gi + 1, tsec, gateid[gi], (unsigned long long)k, (unsigned long long)rot,
                         (unsigned long long)(f[0] / 1000000000ull), (unsigned long long)f[1], fs[0] ? fs : "-",
                         (unsigned long long)f[2], (unsigned long long)f[3], (int)(int32_t)f[3],
                         (unsigned long long)f[4], dfl[0] ? dfl : "-", (unsigned long long)f[5],
                         (unsigned long long)(l[0] / 1000000000ull), (unsigned long long)l[1], ls[0] ? ls : "-",
                         (unsigned long long)l[2], (unsigned long long)l[3], (int)(int32_t)l[3],
                         (unsigned long long)l[4], dl[0] ? dl : "-", (unsigned long long)l[5],
                         nlr, (unsigned long long)ov);
            for (int i = 0; i < nlr; i++) {
                char d2[72] = { 0 };
                if (lrd[i][0]) runtime_describe_address(lrd[i][0], d2, sizeof d2);
                vk_boot_logf("[m8b] lr%d t=%us gate=%u #%d lr=%#llx site=%#llx(%s) n=%llu first_ns=%llus last_ns=%llus",
                             gi + 1, tsec, gateid[gi], i, (unsigned long long)lrd[i][0],
                             (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0), d2[0] ? d2 : "-",
                             (unsigned long long)lrd[i][1],
                             (unsigned long long)(lrd[i][2] / 1000000000ull),
                             (unsigned long long)(lrd[i][3] / 1000000000ull));
            }
        }
    }
    {   /* 每 token 计数表：t≈12 s 首次（boot 段），之后每 20 s 一次 */
        int want = 0;
        if (!tok_first && (double)tsec >= 12.0) { tok_first = 1; want = 1; }
        if ((double)tsec >= next_tok) { next_tok += 20.0; want = 1; }
        if (want) {
            int nt = smo_obs_m8b_tokn(0);
            for (int i = 0; i < nt; i++) {
                uint64_t tv = 0, n = 0, ft = 0, lt = 0, llr = 0, kp = 0;
                char nm[M8B_NAMELEN_LOG] = { 0 }, sh[M8B_NAMELEN_LOG] = { 0 }, d2[72] = { 0 };
                if (!smo_obs_m8b_tok(0, i, &tv, &n, &ft, &lt, &llr, &kp, nm, (int)sizeof nm)) continue;
                m8b_short(nm, sh, sizeof sh);
                if (llr) runtime_describe_address(llr, d2, sizeof d2);
                vk_boot_logf("[m8b] tok%d t=%us gate=104 #%d token=%#llx name=%s n=%llu first_ns=%llus "
                             "last_ns=%llus last_lr=%#llx(%s) keeper=%#llx",
                             i + 1, tsec, i, (unsigned long long)tv, sh[0] ? sh : "-",
                             (unsigned long long)n, (unsigned long long)(ft / 1000000000ull),
                             (unsigned long long)(lt / 1000000000ull), (unsigned long long)llr,
                             d2[0] ? d2 : "-", (unsigned long long)kp);
            }
            vk_boot_logf("[m8b] tokend t=%us gate=104 slots=%d ovf=%llu", tsec, nt,
                         (unsigned long long)smo_obs_m8b_tokovf(0));
        }
    }
    {   /* 逐事件环：首 128 条（完整 boot 序列）+ 最近 16 条，t≈20/60/100/140 s 各一次 */
        int want = 0;
        if (!ring20 && (double)tsec >= 20.0)  { ring20 = 1;  want = 1; }
        if (!ring60 && (double)tsec >= 60.0)  { ring60 = 1;  want = 1; }
        if (!ring100 && (double)tsec >= 100.0) { ring100 = 1; want = 1; }
        if (!ring140 && (double)tsec >= 140.0) { ring140 = 1; want = 1; }
        if (want) {
            int n = smo_obs_m8b_n(0);
            for (int i = 0; i < n; i++) {
                uint64_t m[6] = { 0 };
                char nm[M8B_NAMELEN_LOG] = { 0 }, sh[M8B_NAMELEN_LOG] = { 0 }, d2[72] = { 0 };
                if (!smo_obs_m8b_rec(0, i, m, nm, (int)sizeof nm)) continue;
                m8b_short(nm, sh, sizeof sh);
                if (m[4]) runtime_describe_address(m[4], d2, sizeof d2);
                vk_boot_logf("[m8b] ev t=%us #%d ns=%llus token=%#llx(%s) old10=%#llx old18=%d lr=%#llx(%s) keeper=%#llx",
                             tsec, i, (unsigned long long)(m[0] / 1000000000ull),
                             (unsigned long long)m[1], sh[0] ? sh : "-",
                             (unsigned long long)m[2], (int)(int32_t)m[3],
                             (unsigned long long)m[4], d2[0] ? d2 : "-", (unsigned long long)m[5]);
            }
            for (int i = 0; i < 16; i++) {
                uint64_t m[6] = { 0 };
                char nm[M8B_NAMELEN_LOG] = { 0 }, sh[M8B_NAMELEN_LOG] = { 0 }, d2[72] = { 0 };
                if (!smo_obs_m8b_last(0, i, m, nm, (int)sizeof nm)) continue;
                m8b_short(nm, sh, sizeof sh);
                if (m[4]) runtime_describe_address(m[4], d2, sizeof d2);
                vk_boot_logf("[m8b] lv t=%us #%d ns=%llus token=%#llx(%s) old10=%#llx old18=%d lr=%#llx(%s) keeper=%#llx",
                             tsec, i, (unsigned long long)(m[0] / 1000000000ull),
                             (unsigned long long)m[1], sh[0] ? sh : "-",
                             (unsigned long long)m[2], (int)(int32_t)m[3],
                             (unsigned long long)m[4], d2[0] ? d2 : "-", (unsigned long long)m[5]);
            }
        }
    }
}

/* ---- M8D（2026-11-08）：**真机回归第一轮** —— 两只孪生绑表点（106/107）----
   M8B 只给了**静态推断**：真机走 FirstSequence 族的 2 槽表 `0x12d0fe8`（绑表点 main+0x4b8cb0），
   模拟器走 TitleMenu 族的 `0x12d06d8`（绑表点 main+0x4b7380），两绑表点都**零静态调用者**。
   本轮把这两只绑表点各装一只**入口门**（tail=0 四字窗，只读寄存器）：
     G1 id 106 = main+0x4b8cb0 → `add x20,#0xfe8` ⇒ 绑 0x12d0fe8（FirstSequence 族）
     G2 id 107 = main+0x4b7380 → `add x20,#0x6d8` ⇒ 绑 0x12d06d8（TitleMenu 族）
   **判读口径**：某门 hits > 0 ⇔ 该族 HostType 实例在本机**确实被构造过**（这只函数只有 vtable 进得来，
   静态 bl/b = 0）。两门都 0 ⇒ 两族都没构造（不可能，除非镜像不同）。
   记录：计数 / 首末 tick / 16 槽 LR 台账（谁调进来的 —— 用来指认"是哪个 vtable 槽"）。
   Switch smo_m8d（C 默认 0；`echo 0 > .../files/smo_m8d` ≤2 s 回退；关着仍有"前 16 次必记"）。
   ⚠️ 名字用 m8d：五类开工前全 0 命中（2026-11-08 15:0x）；`smo_m8b`/`[m8b]`/`smo_m8c`/`[m8c]` 一字未动。 */
#define M8D_PATH_A "/data/storage/el2/base/files/smo_m8d"
#define M8D_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m8d"
__attribute__((constructor)) static void m8d_seed(void) {
    if (!fopen(M8D_PATH_A, "r")) {
        FILE *f = fopen(M8D_PATH_A, "w");
        if (!f) f = fopen(M8D_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m8d] seeded = 0 (off)\n"); }
    }
}
static int m8d_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M8D_PATH_B : M8D_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
/* 门占位段之后 4 字（= 绑表调用的前 4 条：add x20,#表偏移 / mov w2,#2 / mov x1,x20 / mov x19,x0），
   运行期逐字回读，证明门外一字未动（离线取自 main.patched.bin，md5 89d568fd…）。 */
static const uint32_t m8d_post1[4] = { 0x913fa294, 0x321f03e2, 0xaa1403e1, 0xaa0003f3 };  /* 0x4b8cc0..0x4b8ccc */
static const uint32_t m8d_post2[4] = { 0x911b6294, 0x321f03e2, 0xaa1403e1, 0xaa0003f3 };  /* 0x4b7390..0x4b739c */
void runtime_m8d_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m8b_sum(int gi, uint64_t *k, uint64_t first[6], uint64_t last[6],
                               char *fname, char *lname, int nl, uint64_t lrd[][4], int *nlr,
                               uint64_t *lr_ovf, uint64_t *rot, uint64_t *lrot);
    extern uint64_t smo_obs_main_base(void);
    extern _Atomic int g_m8d_on;
    static const uint32_t site[2] = { 0x4b8cb0, 0x4b7380 };
    static const uint32_t gateid[2] = { 106, 107 };
    static const char *fam[2] = { "FirstSequence(0x12d0fe8)", "TitleMenu(0x12d06d8)" };
    static const uint32_t *post[2] = { m8d_post1, m8d_post2 };
    static double next_sum;
    static int ann, code_done;
    uint64_t lrd[16][4];
    atomic_store_explicit(&g_m8d_on, m8d_on(), memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m8d] probe on (t=%us) G1 id=106 main+0x4b8cb0 binds nerve table 0x12d0fe8 "
                     "(FirstSequence family) | G2 id=107 main+0x4b7380 binds 0x12d06d8 (TitleMenu family) | "
                     "hits>0 => that family's HostType instance WAS constructed on this machine "
                     "(both sites have zero static bl/b callers: vtable only) | code=10s sum=5s sw=%d",
                     tsec, (int)atomic_load_explicit(&g_m8d_on, memory_order_relaxed));
        next_sum = (double)tsec + 5.0;
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base();
        for (int gi = 0; gi < 2; gi++) {
            uint32_t win[4] = { 0 }, aft[4] = { 0 }, pv = 0;
            int ok = mb && runtime_safe_read_ungated(win, mb + site[gi], sizeof win);
            if (mb) pv = runtime_safe_read_ungated(&pv, mb + site[gi] - 4, 4) ? pv : 0;
            int aok = mb && runtime_safe_read_ungated(aft, mb + site[gi] + 0x10, sizeof aft);
            unsigned same = 0;
            if (aok) for (int i = 0; i < 4; i++) if (aft[i] == post[gi][i]) same++;
            vk_boot_logf("[m8d] code%d t=%us site=main+0x%x gate=%u tail=0 ok=%d win=%08x %08x %08x %08x | "
                         "post(main+0x%x..) same=%u/4 %08x %08x %08x %08x | prev(main+0x%x)=%08x",
                         gi + 1, tsec, site[gi], gateid[gi], ok,
                         ok ? win[0] : 0, ok ? win[1] : 0, ok ? win[2] : 0, ok ? win[3] : 0,
                         site[gi] + 0x10, same,
                         aok ? aft[0] : 0, aok ? aft[1] : 0, aok ? aft[2] : 0, aok ? aft[3] : 0,
                         site[gi] - 4, pv);
        }
    }
    if ((double)tsec >= next_sum) {
        next_sum += 5.0;
        for (int gi = 0; gi < 2; gi++) {
            uint64_t k = 0, f[6] = { 0 }, l[6] = { 0 }, ov = 0, rot = 0, lrot = 0;
            int nlr = 0;
            char dfl[72] = { 0 }, dl[72] = { 0 };
            (void)smo_obs_m8b_sum(2 + gi, &k, f, l, NULL, NULL, 0, lrd, &nlr, &ov, &rot, &lrot);
            if (f[4]) runtime_describe_address(f[4], dfl, sizeof dfl);
            if (l[4]) runtime_describe_address(l[4], dl, sizeof dl);
            vk_boot_logf("[m8d] sum%d t=%us gate=%u site=main+0x%x family=%s hits=%llu | "
                         "first_ns=%llus first_x0=%#llx first_lr=%#llx(%s) | "
                         "last_ns=%llus last_x0=%#llx last_lr=%#llx(%s) | uniq_lr=%d lr_ovf=%llu",
                         gi + 1, tsec, gateid[gi], site[gi], fam[gi], (unsigned long long)k,
                         (unsigned long long)(f[0] / 1000000000ull), (unsigned long long)f[5],
                         (unsigned long long)f[4], dfl[0] ? dfl : "-",
                         (unsigned long long)(l[0] / 1000000000ull), (unsigned long long)l[5],
                         (unsigned long long)l[4], dl[0] ? dl : "-",
                         nlr, (unsigned long long)ov);
            for (int i = 0; i < nlr; i++) {
                char d2[72] = { 0 };
                if (lrd[i][0]) runtime_describe_address(lrd[i][0], d2, sizeof d2);
                vk_boot_logf("[m8d] lr%d t=%us gate=%u #%d lr=%#llx site=%#llx(%s) n=%llu "
                             "first_ns=%llus last_ns=%llus",
                             gi + 1, tsec, gateid[gi], i, (unsigned long long)lrd[i][0],
                             (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0), d2[0] ? d2 : "-",
                             (unsigned long long)lrd[i][1],
                             (unsigned long long)(lrd[i][2] / 1000000000ull),
                             (unsigned long long)(lrd[i][3] / 1000000000ull));
            }
        }
    }
}

/* ---- M8E2（2026-11-08）：**追第一个分叉** —— 谁决定 DemoOpening 还是 LoadStage ----
   静态定位（main.patched.bin md5 89d568fd…）：
     函数 main+0x4b4230（HakoniwaSequence 初始化）里
       0x4b4bb8 bl 0x6c7b20 (setNerve(0x12d0420 BootLoadData))   返回址 = 0x4b4bbc（M8D 的"分叉入口"）
       0x4b4bbc ldr x0,[x19,#0xb8]                              A = [seq+0xb8]
       0x4b4bc0 bl 0x4d2bd0                                      ★ 判断点的**前一字**
       0x4b4bc4 tbz w0,#0,#0x4b4d2c                              ★★ 判断指令
       0x4b4d1c..0x4b4d28 只有"位 0 == 1"这一侧才走 ⇒ setNerve(0x12d0428 DemoOpening)
       0x4b4d2c 尾声（ret）                                      位 0 == 0 直接从这里返回（不写 DemoOpening）
     被调者 main+0x4d2bd0 = `ldr x8,[x0,#0x20]; ldrb w0,[x8,#0x5f8]; ret`
       ⇒ **判定值 = u8([[A+0x20]]+0x5f8) 的 bit0**（本轮核心读数）
   四门（门号 108..111，接 M8D 的 107 之后）：
     108 = main+0x4b4bc0（tail=3 单 bl 门）：记 A / B / **判定字节原始值** / tick / LR
     109 = main+0x4b4bb8（tail=3 单 bl 门）：返回址正是 0x4b4bbc ⇒ 计数 + seq + 新 token + 首末 tick + LR
     110 = main+0x59ac30（tail=0 四字窗）：场景图创建入口，记 x1（"Stage" 入参，抄字符串）+ LR
     111 = main+0x59accc（tail=1 单 ret 门）：同一函数的返回点，记 **返回 x0** + LR
   判读口径：108 的 x2 是**原始字节**（不预做位运算）；bit0=1 ⇒ 走 DemoOpening 那侧，
   bit0=0 ⇒ 在 0x4b4d2c 直接返回（模拟器那一侧），DemoOpening 一次都不会写。
   开关 smo_m8e2（C 默认 0；`echo 0 > .../files/smo_m8e2` ≤2 s 回退；关着仍有"前 16 次必记"）。
   ⚠️ 命名：`m8e` 五类**已被另一轮 M8E（模拟器 nerve 时间线）占用**（`/tmp/m8e_report.md` 15:19、
   `m8e_boot_run.sh`、`logs/m8e_e1|e2|e3`）⇒ 本轮按纪律改用 **m8e2** 全套，绝不覆盖。 */
#define M8E2_PATH_A "/data/storage/el2/base/files/smo_m8e2"
#define M8E2_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m8e2"
__attribute__((constructor)) static void m8e2_seed(void) {
    struct stat st;
    if (stat(M8E2_PATH_A, &st) != 0 && stat(M8E2_PATH_B, &st) != 0) {
        FILE *f = fopen(M8E2_PATH_B, "w");
        if (!f) f = fopen(M8E2_PATH_A, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m8e2] seeded = 0 (off)\n"); }
    }
}
static int m8e2_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M8E2_PATH_A : M8E2_PATH_B;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
/* 门占位段之后的 4 字 + 门前一字（离线取自 main.patched.bin，md5 89d568fd…）：
   108/109/111 只换 1 个字（tail≠0）⇒ post 从 site+4 起；110 换 4 字 ⇒ post 从 site+0x10 起。 */
static const uint32_t m8e2_post[4][4] = {
    { 0x36000b40, 0xf9400288, 0xf9401108, 0xf9402514 },   /* 108 post@0x4b4bc4：tbz / ldr x8,[x20] / ldr x8,[x8,#0x20] / ldr x20,[x8,#0x48] */
    { 0xf9405e60, 0x94007804, 0x36000b40, 0xf9400288 },   /* 109 post@0x4b4bbc：ldr x0,[x19,#0xb8] / bl 0x4d2bd0 / tbz / ldr x8,[x20] */
    /* 110 post@0x59ac40：mov w8,#-1 / stp x1,xzr,[sp] / str xzr,[sp,#0x10] / mov x20,x1
       ⚠️ 收工修正：首格曾误抄成 0x52800008（movz），镜像实为 **0x12800008**（movn w8,#0 = mov w8,#-1）
       ⇒ 本轮证据 HAP 的 code3 行因此显示 same=3/4（实际 4 字与镜像逐字相同）。此修正**不在**该 HAP 里。 */
    { 0x12800008, 0xa9007fe1, 0xf9000bff, 0xaa0103f4 },
    { 0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd },   /* 111 post@0x59acd0：下一函数首字起 4 字（只作窗校验） */
};
static const uint32_t m8e2_prev[4] = { 0xf9405e60, 0xaa1303e0, 0xd65f03c0, 0x910143ff };
void runtime_m8e2_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m8e2_sum(int gi, uint64_t *k, uint64_t first[5], uint64_t last[5],
                                uint64_t lrd[][4], int *nlr, uint64_t *lr_ovf, uint64_t *rot);
    extern int smo_obs_m8e2_rec(int gi, int i, uint64_t meta[5], char *name, int namelen);
    extern uint64_t smo_obs_main_base(void);
    extern _Atomic int g_m8e2_on;
    static const uint32_t site[4] = { 0x4b4bc0, 0x4b4bb8, 0x59ac30, 0x59accc };
    static const uint32_t gateid[4] = { 108, 109, 110, 111 };
    static const uint32_t tail[4] = { 3, 3, 0, 1 };
    static const char *what[4] = {
        "judgment-operand u8([[A+0x20]]+0x5f8) bit0: 1=>fall through=>setNerve(0x12d0428 DemoOpening); 0=>return at 0x4b4d2c (no DemoOpening)",
        "fork-entry(ret-addr=0x4b4bbc): x0=seq x1=new token(expect 0x12d0420)",
        "scene-graph-create entry: x1='Stage' arg",
        "same fn return: x0=return value"
    };
    static double next_sum;
    static int ann, code_done;
    static uint64_t printed[4];
    uint64_t lrd[8][4];
    atomic_store_explicit(&g_m8e2_on, m8e2_on(), memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m8e2] probe on (t=%us) G1 id=108 main+0x4b4bc0 bl 0x4d2bd0 (word before tbz w0,#0 at "
                     "0x4b4bc4) | G2 id=109 main+0x4b4bb8 bl 0x6c7b20 (ret addr = fork entry 0x4b4bbc) | "
                     "G3 id=110 main+0x59ac30 entry ('Stage' arg) | G4 id=111 main+0x59accc ret (return x0) | "
                     "field = u8([[seq+0xb8]+0x20]+0x5f8) | code=10s sum=5s sw=%d",
                     tsec, (int)atomic_load_explicit(&g_m8e2_on, memory_order_relaxed));
        next_sum = (double)tsec + 5.0;
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base();
        for (int gi = 0; gi < 4; gi++) {
            uint32_t win[4] = { 0 }, aft[4] = { 0 }, pv = 0;
            int ok = mb && runtime_safe_read_ungated(win, mb + site[gi], sizeof win);
            if (mb) pv = runtime_safe_read_ungated(&pv, mb + site[gi] - 4, 4) ? pv : 0;
            uint32_t postbase = site[gi] + (tail[gi] ? 4 : 0x10);
            int aok = mb && runtime_safe_read_ungated(aft, mb + postbase, sizeof aft);
            unsigned same = 0;
            if (aok) for (int i = 0; i < 4; i++) if (aft[i] == m8e2_post[gi][i]) same++;
            vk_boot_logf("[m8e2] code%d t=%us site=main+0x%x gate=%u tail=%u ok=%d win=%08x %08x %08x %08x | "
                         "post(main+0x%x..) same=%u/4 %08x %08x %08x %08x | prev(main+0x%x)=%08x (expect %08x)",
                         gi + 1, tsec, site[gi], gateid[gi], tail[gi], ok,
                         ok ? win[0] : 0, ok ? win[1] : 0, ok ? win[2] : 0, ok ? win[3] : 0,
                         postbase, same,
                         aok ? aft[0] : 0, aok ? aft[1] : 0, aok ? aft[2] : 0, aok ? aft[3] : 0,
                         site[gi] - 4, pv, m8e2_prev[gi]);
        }
    }
    if ((double)tsec >= next_sum) {
        next_sum += 5.0;
        for (int gi = 0; gi < 4; gi++) {
            uint64_t k = 0, f[5] = { 0 }, l[5] = { 0 }, ov = 0, rot = 0;
            int nlr = 0;
            char dfl[72] = { 0 }, dl[72] = { 0 };
            (void)smo_obs_m8e2_sum(gi, &k, f, l, lrd, &nlr, &ov, &rot);
            if (f[4]) runtime_describe_address(f[4], dfl, sizeof dfl);
            if (l[4]) runtime_describe_address(l[4], dl, sizeof dl);
            vk_boot_logf("[m8e2] sum%d t=%us gate=%u site=main+0x%x what=%s | hits=%llu | "
                         "first_ns=%llus f_x0=%#llx f_x1=%#llx f_x2=%#llx f_lr=%#llx(%s) | "
                         "last_ns=%llus l_x0=%#llx l_x1=%#llx l_x2=%#llx l_lr=%#llx(%s) | "
                         "uniq_lr=%d lr_ovf=%llu",
                         gi + 1, tsec, gateid[gi], site[gi], what[gi], (unsigned long long)k,
                         (unsigned long long)(f[0] / 1000000000ull),
                         (unsigned long long)f[1], (unsigned long long)f[2], (unsigned long long)f[3],
                         (unsigned long long)f[4], dfl[0] ? dfl : "-",
                         (unsigned long long)(l[0] / 1000000000ull),
                         (unsigned long long)l[1], (unsigned long long)l[2], (unsigned long long)l[3],
                         (unsigned long long)l[4], dl[0] ? dl : "-",
                         nlr, (unsigned long long)ov);
            if (gi == 0 && k) {
                /* 判定结论（位运算只在 dump 侧做，原始字节在上面两格） */
                unsigned byte = (unsigned)(l[3] & 0xff);
                vk_boot_logf("[m8e2] judge t=%us gate=108 raw_byte=%#04x bit0=%u side=%s",
                             tsec, byte, byte & 1u,
                             (byte & 1u) ? "FALL-THROUGH=>setNerve(0x12d0428 DemoOpening)"
                                         : "SKIP=>return at 0x4b4d2c (no DemoOpening)");
            }
            for (int i = 0; i < nlr; i++) {
                char d2[72] = { 0 };
                if (lrd[i][0]) runtime_describe_address(lrd[i][0], d2, sizeof d2);
                vk_boot_logf("[m8e2] lr%d t=%us gate=%u #%d lr=%#llx site=%#llx(%s) n=%llu "
                             "first_ns=%llus last_ns=%llus",
                             gi + 1, tsec, gateid[gi], i, (unsigned long long)lrd[i][0],
                             (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0), d2[0] ? d2 : "-",
                             (unsigned long long)lrd[i][1],
                             (unsigned long long)(lrd[i][2] / 1000000000ull),
                             (unsigned long long)(lrd[i][3] / 1000000000ull));
            }
            /* 逐事件环：只打新增的（每门 16 槽，打印游标 = 上次已打到哪） */
            if (rot > printed[gi]) {
                uint64_t from = printed[gi];
                uint64_t to = rot > 16 ? 16 : rot;
                for (uint64_t i = from; i < to; i++) {
                    uint64_t m[5] = { 0 };
                    char nm[40] = { 0 };
                    if (!smo_obs_m8e2_rec(gi, (int)i, m, nm, sizeof nm)) continue;
                    vk_boot_logf("[m8e2] ev%d t=%us gate=%u #%d ns=%llus x0=%#llx x1=%#llx x2=%#llx "
                                 "lr=%#llx name=\"%s\"",
                                 gi + 1, tsec, gateid[gi], (int)i,
                                 (unsigned long long)(m[0] / 1000000000ull),
                                 (unsigned long long)m[1], (unsigned long long)m[2],
                                 (unsigned long long)m[3], (unsigned long long)m[4], nm);
                }
                printed[gi] = to;
            }
        }
    }
}

/* ---- M10A（2026-11-08）：**把首启判据 bit0 扳成 1**（在模拟器上看 A 族链条会不会自己长出来）----
   机制在 game_patches.c：门 108（main+0x4b4bc0 bl 0x4d2bd0）的 tail=3 单 bl 门在安装期若
   smo_m10a≠0 就换成 **35 字变体** —— 先原样重放那条 bl（= 真调谓词，原始返回值进 w0），
   再 `orr w0,w0,mask` 后返回调用方 ⇒ **只改 w0 的 bit0**（mask=0 时语义恒等）。
   新门 112 = main+0x4b4bc8（tbz 落穿侧首字 = A 支入口，全镜像零旁路落点）：命中数 =
   "A 支被走进"的次数，x0 = **调用方真正看到的值**。
   开关 smo_m10a（C 默认 0；`echo 0` ≤2 s 回退）：
     0 = 完全关（安装期也不装变体 ⇒ 与 M9C 逐字节相同）；1 = 强制（默认只对**前 1 次**命中生效，
     文件第二字符可给 1..3）；2 = 只记录不改变（mask 恒 0，作对照）。
   节拍：probe 一次；code（stub 变体字回读）t≥10 s 一次；sum 每 5 s；逐次命中 ev 一次。
   ⚠️ 命名：五类开工前全 0 命中（2026-11-08 15:5x）；`smo_m8e2`/`smo_m8c` 那两套一字未动。 */
#define M10A_PATH_A "/data/storage/el2/base/files/smo_m10a"
#define M10A_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m10a"
__attribute__((constructor)) static void m10a_seed(void) {
    struct stat st;
    if (stat(M10A_PATH_A, &st) != 0 && stat(M10A_PATH_B, &st) != 0) {
        FILE *f = fopen(M10A_PATH_B, "w");
        if (!f) f = fopen(M10A_PATH_A, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m10a] seeded = 0 (off)\n"); }
    }
}
/* 返回模式 0/1/2；`limit` 取文件第二字符 '1'..'3'（缺省 1）。 */
static int m10a_mode(int *limit) {
    const char *p; char b[16]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M10A_PATH_A : M10A_PATH_B;
        if ((f = fopen(p, "r"))) {
            size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
            if (!n) return 0;
            if (limit) *limit = (n > 1 && b[1] >= '1' && b[1] <= '3') ? b[1] - '0' : 1;
            return (b[0] >= '0' && b[0] <= '2') ? b[0] - '0' : 0;
        }
    }
    return 0;
}
void runtime_m10a_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m10a_sum(int *mode, int *limit, int *install, int *hits, int *forced,
                                uint64_t *raw, uint64_t *ret, uint64_t *f_ns, uint64_t *l_ns);
    extern int smo_obs_m10a_rec(int i, uint64_t *ns, int *forced, int *raw, int *ret);
    extern int smo_obs_m10a_rn(void);
    extern int smo_obs_m10a_a(uint64_t *k, uint64_t *f_x0, uint64_t *l_x0, uint64_t *f_lr, uint64_t *l_lr,
                              uint64_t *f_ns, uint64_t *l_ns);
    extern _Atomic int g_m10a_mode;
    extern _Atomic int g_m10a_limit;
    extern int runtime_safe_read_ungated(void *dst, uint64_t addr, size_t n);
    extern uint64_t smo_obs_main_base(void);
    extern uint64_t smo_obs_stub_off_gate(int gate);
    static double next_sum;
    static int ann, code_done, last_hits, last_a;
    uint64_t a_k = 0, a_fx0 = 0, a_lx0 = 0, a_flr = 0, a_llr = 0, a_fns = 0, a_lns = 0;
    int lim = 1;
    int mode = m10a_mode(&lim);
    atomic_store_explicit(&g_m10a_mode, mode, memory_order_relaxed);
    atomic_store_explicit(&g_m10a_limit, lim, memory_order_relaxed);
    if (!ann) {
        int im = -1, m2 = -1, l2 = -1;
        (void)smo_obs_m10a_sum(&m2, &l2, &im, NULL, NULL, NULL, NULL, NULL, NULL);
        ann = 1;
        vk_boot_logf("[m10a] probe on (t=%us) gate=108 main+0x4b4bc0 (bl 0x4d2bd0 before `tbz w0,#0`): "
                     "install_mode=%d stub=%s | gate=112 main+0x4b4bc8 (tbz fall-through first word = A branch) "
                     "| mode now=%d limit=%d | 0=off(byte-identical to M9C) 1=force(first N hits) 2=record-only",
                     tsec, im, im ? "M10A 35-word force variant" : "plain tail=3 skeleton (byte-identical to M9C)",
                     mode, lim);
        next_sum = (double)tsec + 5.0;
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base(), so = smo_obs_stub_off_gate(108);
        uint32_t win[4] = { 0 }, sw[15] = { 0 };
        int ok = mb && runtime_safe_read_ungated(win, mb + 0x4b4bc0, sizeof win);
        int sok = mb && so && runtime_safe_read_ungated(sw, mb + so + 20 * 4, sizeof sw);
        vk_boot_logf("[m10a] code t=%us site=main+0x4b4bc0 ok=%d win=%08x %08x %08x %08x | stub=main+%#llx read=%d "
                     "w20..w34=%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                     tsec, ok, win[0], win[1], win[2], win[3],
                     (unsigned long long)so, sok,
                     sw[0], sw[1], sw[2], sw[3], sw[4], sw[5], sw[6], sw[7], sw[8], sw[9], sw[10], sw[11], sw[12], sw[13], sw[14]);
    }
    if ((double)tsec >= next_sum) {
        next_sum += 5.0;
        int m2 = -1, l2 = -1, im = -1, hits = 0, forced = 0;
        uint64_t raw = 0, ret = 0, fns = 0, lns = 0;
        (void)smo_obs_m10a_sum(&m2, &l2, &im, &hits, &forced, &raw, &ret, &fns, &lns);
        (void)smo_obs_m10a_a(&a_k, &a_fx0, &a_lx0, &a_flr, &a_llr, &a_fns, &a_lns);
        char dl[72] = { 0 }, dl2[72] = { 0 };
        if (a_flr) runtime_describe_address(a_flr, dl, sizeof dl);
        if (a_llr && a_llr != a_flr) runtime_describe_address(a_llr, dl2, sizeof dl2);
        vk_boot_logf("[m10a] sum t=%us mode=%d limit=%d install_mode=%d stub=%s | g108 hits=%d forced=%d | "
                     "last_raw=%#04x last_ret=%#04x f_ns=%llus l_ns=%llus | g112(A-branch entry) hits=%llu "
                     "f_x0=%#llx l_x0=%#llx f_lr=%#llx(%s) l_lr=%#llx(%s) f_ns=%llus l_ns=%llus",
                     tsec, m2, l2, im, (im ? "variant" : "plain"), hits, forced,
                     (unsigned)raw, (unsigned)ret,
                     (unsigned long long)(fns / 1000000000ull), (unsigned long long)(lns / 1000000000ull),
                     (unsigned long long)a_k, (unsigned long long)a_fx0, (unsigned long long)a_lx0,
                     (unsigned long long)a_flr, dl[0] ? dl : "-",
                     (unsigned long long)a_llr, dl2[0] ? dl2 : (dl[0] ? dl : "-"),
                     (unsigned long long)(a_fns / 1000000000ull), (unsigned long long)(a_lns / 1000000000ull));
        if (im && (hits != last_hits)) {
            last_hits = hits;
            vk_boot_logf("[m10a] judge t=%us gate=108 raw_byte=%#04x returned=%#04x bit0_returned=%u side=%s",
                         tsec, (unsigned)raw, (unsigned)ret, (unsigned)ret & 1u,
                         ((unsigned)ret & 1u) ? "FALL-THROUGH=>0x4b4bc8 (A branch: demo/opening family)"
                                              : "SKIP=>return at 0x4b4d2c (no DemoOpening)");
        }
        if (a_k != (uint64_t)last_a) {
            last_a = (int)a_k;
            vk_boot_logf("[m10a] judgeA t=%us gate=112 hits=%llu x0(returned to caller)=%#llx lr=%#llx(%s) "
                         "| A-branch entry counted (zero other landings in the whole image)",
                         tsec, (unsigned long long)a_k, (unsigned long long)a_lx0, (unsigned long long)a_llr,
                         dl2[0] ? dl2 : (dl[0] ? dl : "-"));
        }
        int rn = smo_obs_m10a_rn();
        for (int i = 0; i < rn && i < 8; i++) {
            uint64_t ns = 0; int fc = 0, rw = 0, rt = 0;
            if (!smo_obs_m10a_rec(i, &ns, &fc, &rw, &rt)) continue;
            vk_boot_logf("[m10a] ev #%d ns=%llus forced=%d raw_prev=%#04x ret_prev=%#04x (prev = the PREVIOUS hit's "
                         "stub read-back; the last hit's numbers are last_raw/last_ret above)",
                         i, (unsigned long long)(ns / 1000000000ull), fc, (unsigned)rw, (unsigned)rt);
        }
    }
}

/* ---- M15A（2026-11-08）：**x30 直方图**（门 113 = main+0x988a20，NVN submit 入口）----
   观察器在 game_patches.c（只做原子计数/定长写）；这里负责**开关刷新 + 周期性出证**：
     · probe 一次：门点/口径/开关状态；
     · 每 5 s 一行 sum：hits / distinct(used) / 桶溢出 / 待领队列溢出 / 首末 x30 / 首末 A；
     · 每 5 s（有变化时）top-20 直方图：`[m15a] h #i lr=.. main+.. n=.. (early=.. late=..) nm=".."`，
       其中 early = sub<1536（黑层出现前）、late = sub>=1536（永久盖黑段）——**两栏差 = 谁在黑层段多出来了**；
     · 名字串从 A+0x358 读（runtime_safe_read_ungated，SIGSEGV 兜底；**观察器里不读**）。
   开关 `smo_m15a`（C 默认 0；`echo 1` ≤2 s 生效）。命名：smo_m15a / [m15a] / m15a_boot_run.sh /
   /tmp/m15a_report.md / logs/m15a_* —— 五类开工前全 0 命中（2026-11-08 18:1x 三查）。 */
#define M15A_PATH_A "/data/storage/el2/base/files/smo_m15a"
#define M15A_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m15a"
static int m15a_mode(void)
{
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M15A_PATH_A : M15A_PATH_B;
        if ((f = fopen(p, "r"))) {
            size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
            return (n > 0 && b[0] == '1') ? 1 : 0;
        }
    }
    return 0;
}
void runtime_m15a_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern uint64_t smo_obs_m15a_k(void);
    extern void smo_obs_m15a_meta(uint64_t *k, uint64_t *first_tick, uint64_t *last_tick,
                                  uint64_t *first_lr, uint64_t *last_lr, uint64_t *a_first, uint64_t *a_last,
                                  uint64_t *h_used, uint64_t *h_ovf, uint64_t *pend_ovf);
    extern int smo_obs_m15a_hist(int N, uint64_t *lr, uint64_t *n_early, uint64_t *n_late, uint64_t *a_last);
    extern int runtime_safe_read_ungated(void *dst, uint64_t addr, size_t n);
    extern uint64_t smo_obs_main_base(void);
    extern _Atomic int g_m15a_on;
    static double next_sum = -1.0;
    static int ann, last_hits = -1;
    int on = m15a_mode();
    atomic_store_explicit(&g_m15a_on, on, memory_order_relaxed);
    if (!ann) {
        ann = 1;
        next_sum = (double)tsec + 5.0;
        vk_boot_logf("[m15a] probe on (t=%us) gate=113 main+0x988a20 (NVN submit entry: binds the same 64KB "
                     "constant ring for stage 0/1/2/5; x30 = caller return address, x0 = A, name buffer at A+0x358) "
                     "| switch smo_m15a=%d (0=off, 1=on) | M14A static callers = 190 bl / 150 host funcs",
                     tsec, on);
    }
    if ((double)tsec < next_sum) return;
    next_sum += 5.0;
    uint64_t k = 0, ft = 0, lt = 0, flr = 0, llr = 0, af = 0, al = 0, used = 0, hovf = 0, povf = 0;
    smo_obs_m15a_meta(&k, &ft, &lt, &flr, &llr, &af, &al, &used, &hovf, &povf);
    if ((int)k == last_hits) return;            /* 没新命中就不刷屏 */
    last_hits = (int)k;
    uint64_t mb = smo_obs_main_base();
    char nf[64]; memset(nf, 0, sizeof nf);
    if (al) {
        char raw[64];
        if (runtime_safe_read_ungated(raw, al + 0x358, 63)) {
            int o = 0;
            for (int i = 0; i < 63 && raw[i]; i++) {
                unsigned char c = (unsigned char)raw[i];
                nf[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
            }
            nf[o] = 0;
        }
    }
    vk_boot_logf("[m15a] sum t=%us on=%d gate=113 hits=%llu distinct=%llu hist_ovf=%llu pend_ovf=%llu "
                 "first_lr=%#llx(main+%#llx) last_lr=%#llx(main+%#llx) a_first=%#llx a_last=%#llx nm=\"%s\" "
                 "first_tick=%llu last_tick=%llu main_base=%#llx",
                 tsec, on, (unsigned long long)k, (unsigned long long)used,
                 (unsigned long long)hovf, (unsigned long long)povf,
                 (unsigned long long)flr, (unsigned long long)(mb && flr > mb ? flr - mb : 0ull),
                 (unsigned long long)llr, (unsigned long long)(mb && llr > mb ? llr - mb : 0ull),
                 (unsigned long long)af, (unsigned long long)al, nf,
                 (unsigned long long)ft, (unsigned long long)lt, (unsigned long long)mb);
    /* top-20 直方图（每次 sum 都打；行数固定 ≤20，便于按时间序列看结构变化） */
    {
        uint64_t HLR[20], HNE[20], HNL[20], HAL[20];
        int hn = smo_obs_m15a_hist(20, HLR, HNE, HNL, HAL);
        for (int i = 0; i < hn; i++) {
            vk_boot_logf("[m15a] h #%d lr=%#llx main+%#llx n=%llu (early=%llu late=%llu)",
                         i + 1, (unsigned long long)HLR[i],
                         (unsigned long long)(mb && HLR[i] > mb ? HLR[i] - mb : 0ull),
                         (unsigned long long)(HNE[i] + HNL[i]),
                         (unsigned long long)HNE[i], (unsigned long long)HNL[i]);
        }
    }
}

/* ---- M16A（2026-11-08）：**"GORP." trampoline**（门 114 = main+0x991a30）的载荷与调用者 ----
   观察器在 game_patches.c（30 字 7 参变体 stub：x0/x1/x2/x3 + x30 全交过来）。这里负责出证：
     · probe 一次：门点 / 口径 / 开关；
     · 每 5 s：`[m16a] sum` = 累计命中 k / distinct x30 / 桶溢出 / 待领溢出 / **每帧最大命中数** /
       首末 (lr, sub, x0)；`[m16a] h #i` = top-10 x30 直方图（early = sub<1536、late = sub>=1536，
       两栏差 = 黑层段多出来的调用者）；
     · 每 15 s：`[m16a] nm` = 三处名字 + x2 的 u16 尺寸 + **执行器对象 A 的 24 字 hexdump**
       （A 由门点函数自己那条 deref 链算出：[[[[[main+0x136b0df8]+0x30]+0x10]+8]+0x18]）；
       全部走 runtime_safe_read_ungated（SIGSEGV 兜底），**观察器里不解引用**。
   开关 smo_m16a（C 默认 0；`echo 1` ≤2 s 生效）。命名：smo_m16a / [m16a] / m16a_boot_run.sh /
   /tmp/m16a_report.md / logs/m16a_* —— 五类开工前三查。 */
#define M16A_PATH_A "/data/storage/el2/base/files/smo_m16a"
#define M16A_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m16a"
static int m16a_mode(void)
{
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M16A_PATH_A : M16A_PATH_B;
        if ((f = fopen(p, "r"))) {
            size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
            return (n > 0 && b[0] == '1') ? 1 : 0;
        }
    }
    return 0;
}
/* 安全读一个 u64（失败 ⇒ *ok=0）。 */
static uint64_t m16a_ld(uint64_t a, int *ok)
{
    extern int runtime_safe_read_ungated(void *dst, uint64_t addr, size_t n);
    uint64_t v = 0;
    if (!*ok || a < 0x100000000ull || a >= 0x800000000000ull) { *ok = 0; return 0; }
    if (!runtime_safe_read_ungated(&v, a, 8)) { *ok = 0; return 0; }
    return v;
}
/* 把 ≤63 B 的可打印化（非可打印 → '.'），与 M15A 同款。 */
static void m16a_str(char *nf, size_t nfl, uint64_t a)
{
    extern int runtime_safe_read_ungated(void *dst, uint64_t addr, size_t n);
    char raw[64]; int o = 0;
    memset(nf, 0, nfl);
    if (!a) return;
    memset(raw, 0, sizeof raw);
    if (!runtime_safe_read_ungated(raw, a, 63)) return;
    for (int i = 0; i < 63 && raw[i] && (size_t)(o + 1) < nfl; i++) {
        unsigned char c = (unsigned char)raw[i];
        nf[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    nf[o] = 0;
}
void runtime_m16a_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern uint64_t smo_obs_m16a_k(void);
    extern void smo_obs_m16a_meta(uint64_t *k, uint64_t *first_tick, uint64_t *last_tick,
                                  uint64_t *first_lr, uint64_t *last_lr,
                                  uint64_t *h_used, uint64_t *h_ovf, uint64_t *pend_ovf,
                                  uint64_t *max_per_frame, uint64_t *cur_sub,
                                  uint64_t *x0_last, uint64_t *x1_last, uint64_t *x2_last, uint64_t *x3_last,
                                  uint64_t *x0_first, uint64_t *sub_first, uint64_t *sub_last);
    extern int smo_obs_m16a_hist(int N, uint64_t *lr, uint64_t *n_early, uint64_t *n_late);
    extern int runtime_safe_read_ungated(void *dst, uint64_t addr, size_t n);
    extern uint64_t smo_obs_main_base(void);
    extern _Atomic int g_m16a_on;
    static double next_sum = -1.0, next_nm = -1.0;
    static int ann, last_hits = -1;
    int on = m16a_mode();
    atomic_store_explicit(&g_m16a_on, on, memory_order_relaxed);
    if (!ann) {
        ann = 1;
        next_sum = (double)tsec + 5.0;
        next_nm = (double)tsec + 10.0;
        vk_boot_logf("[m16a] probe on (t=%us) gate=114 main+0x991a30 = the \"GORP.\" trampoline "
                     "(windows a9bc5ff8 a90157f6 a9024ff4 a9037bfd, tail=0, prev=ret untouched) "
                     "| switch smo_m16a=%d (0=off, 1=on) | payload x0/x1/x2/x3/x30 (7-arg variant stub) "
                     "| static: 16 call sites / 15 host funcs, 0 branches inside the 0x80 B body",
                     tsec, on);
    }
    if ((double)tsec >= next_sum) {
        next_sum += 5.0;
        uint64_t k = 0, ft = 0, lt = 0, flr = 0, llr = 0, used = 0, hovf = 0, povf = 0;
        uint64_t mpf = 0, csub = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0, f0 = 0, sf = 0, sl = 0;
        smo_obs_m16a_meta(&k, &ft, &lt, &flr, &llr, &used, &hovf, &povf, &mpf, &csub,
                          &a0, &a1, &a2, &a3, &f0, &sf, &sl);
        if ((int)k != last_hits) {
            last_hits = (int)k;
            uint64_t mb = smo_obs_main_base();
            vk_boot_logf("[m16a] sum t=%us on=%d gate=114 hits=%llu distinct=%llu hist_ovf=%llu "
                         "pend_ovf=%llu per_frame_max=%llu cur_sub=%llu sub_first=%llu sub_last=%llu "
                         "first_lr=%#llx(main+%#llx) last_lr=%#llx(main+%#llx) x0_first=%#llx x0_last=%#llx "
                         "main_base=%#llx",
                         tsec, on, (unsigned long long)k, (unsigned long long)used,
                         (unsigned long long)hovf, (unsigned long long)povf,
                         (unsigned long long)mpf, (unsigned long long)csub,
                         (unsigned long long)sf, (unsigned long long)sl,
                         (unsigned long long)flr, (unsigned long long)(mb && flr > mb ? flr - mb : 0ull),
                         (unsigned long long)llr, (unsigned long long)(mb && llr > mb ? llr - mb : 0ull),
                         (unsigned long long)f0, (unsigned long long)a0, (unsigned long long)mb);
            uint64_t HLR[10], HNE[10], HNL[10];
            int hn = smo_obs_m16a_hist(10, HLR, HNE, HNL);
            for (int i = 0; i < hn; i++)
                vk_boot_logf("[m16a] h #%d lr=%#llx main+%#llx n=%llu (early=%llu late=%llu)",
                             i + 1, (unsigned long long)HLR[i],
                             (unsigned long long)(mb && HLR[i] > mb ? HLR[i] - mb : 0ull),
                             (unsigned long long)(HNE[i] + HNL[i]),
                             (unsigned long long)HNE[i], (unsigned long long)HNL[i]);
        }
    }
    if ((double)tsec < next_nm) return;
    next_nm = (double)tsec + 15.0;
    /* 名字与对象内容：全部在 port 侧读（观察器里不解引用）。 */
    {
        extern uint64_t smo_obs_m16a_k(void);
        if (!smo_obs_m16a_k()) return;
        uint64_t k = 0, ft = 0, lt = 0, flr = 0, llr = 0, used = 0, hovf = 0, povf = 0;
        uint64_t mpf = 0, csub = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0, f0 = 0, sf = 0, sl = 0;
        smo_obs_m16a_meta(&k, &ft, &lt, &flr, &llr, &used, &hovf, &povf, &mpf, &csub,
                          &a0, &a1, &a2, &a3, &f0, &sf, &sl);
        uint64_t mb = smo_obs_main_base();
        int ok = 1;
        /* 门点函数自己的 deref 链：adrp x8,0x136b000 / ldr x8,[x8,#0xdf8] / ldr x8,[x8] /
           ldr x8,[x8,#0x30] / ldr x8,[x8,#0x10] / ldr x8,[x8,#8] / ldr x24,[x8,#0x18] */
        uint64_t g = m16a_ld(mb + 0x136b0df8ull, &ok);
        uint64_t A = m16a_ld(g, &ok);
        A = m16a_ld(A + 0x30, &ok);
        A = m16a_ld(A + 0x10, &ok);
        A = m16a_ld(A + 8, &ok);
        A = m16a_ld(A + 0x18, &ok);
        char n0[64], nA[64];
        m16a_str(n0, sizeof n0, a0 ? a0 + 0x358 : 0);            /* arg0 + 0x358 */
        m16a_str(nA, sizeof nA, A ? A + 0x358 : 0);              /* 命令缓冲 A + 0x358（M15A 那条） */
        /* M16A-fix：上面那条 deref 链在本 boot 里读到 0（第一跳就失败），改从**门 113 观察器记下的
           x0 = 命令对象 A** 取同一个对象（M15A 的 "GORP." 正是从这里读到的，A=0x6e08950dd0@M15A）。
           同时把 0x2b8..0x358 共 40 个 u32 原样打出：字符串附近还有没有别的层名，一眼可读。 */
        extern uint64_t smo_obs_last_x0(int gate);
        uint64_t A2 = smo_obs_last_x0(113 /* = SMO_OBS_M15A_X30, game_patches.c 的枚举 */);
        char nA2[64];
        m16a_str(nA2, sizeof nA2, A2 ? A2 + 0x358 : 0);
        /* x2 = 执行器的 surface 参数对象（u16 w/h 在 +0x40/+0x42）；x3 = 执行器 x3（+0x288/+0x290）。 */
        unsigned char sb[8]; unsigned q2w = 0, q2h = 0;
        memset(sb, 0, sizeof sb);
        if (a2 && runtime_safe_read_ungated(sb, a2 + 0x40, 4)) {
            q2w = (unsigned)sb[0] | ((unsigned)sb[1] << 8);
            q2h = (unsigned)sb[2] | ((unsigned)sb[3] << 8);
        }
        uint64_t x3w0 = m16a_ld(a3, &ok);
        vk_boot_logf("[m16a] nm t=%us x0=%#llx x1=%#llx x2=%#llx x3=%#llx | nm(x0+358)=\"%s\" "
                     "| A(chain)=%#llx nm(A+358)=\"%s\" | A2(g113 x0)=%#llx nm(A2+358)=\"%s\" "
                     "| x2[+0x40]=%u x2[+0x42]=%u x3[0]=%#llx",
                     tsec, (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2,
                     (unsigned long long)a3, n0, (unsigned long long)A, nA,
                     (unsigned long long)A2, nA2, q2w, q2h, (unsigned long long)x3w0);
        if (A2) {
            char hex[40 * 9 + 1]; int o = 0;
            for (int i = 0; i < 40; i++) {
                int ok2 = 1;
                uint64_t w = m16a_ld(A2 + 0x2b8 + (uint64_t)i * 4, &ok2);
                if (!ok2) break;
                o += snprintf(hex + o, sizeof hex - (size_t)o, " %08x", (unsigned)(w & 0xffffffffu));
            }
            hex[o] = 0;
            vk_boot_logf("[m16a] obj A2+0x2b8..0x358 (40x u32, name at +0x358):%s", hex);
        }
        if (A) {
            char hex[24 * 9 + 1]; int o = 0;
            for (int i = 0; i < 24; i++) {
                uint64_t w = m16a_ld(A + 0x2f8 + (uint64_t)i * 8, &ok);
                if (!ok) break;
                o += snprintf(hex + o, sizeof hex - (size_t)o, " %08llx", (unsigned long long)(w & 0xffffffffull));
            }
            hex[o] = 0;
            vk_boot_logf("[m16a] obj A+0x2f8..0x358 (24x u32):%s", hex);
        }
    }
}

/* ---- M17A（2026-11-08）：**2D 四边形记录叶**（门 115 = main+0x98b4b0，fn 0x98b490 的 +0x20）----
   观察器在 game_patches.c（30 字 7 参变体 stub：x0/x1/x2/x3 + x30 全交过来）。这里负责出证：
     · probe 一次：门点 / 口径 / 开关 / 静态调用者数；
     · 每 5 s：`[m17a] sum` = 累计命中 k / distinct x30 / 待领溢出 / **每帧最大命中数** /
       首末 (lr, sub, x0, x2) + **末见 [x2]（port 侧安全读，= 贴图键）**；
       `[m17a] h #i` = top-12 x30 直方图（early = sub<1536、late = sub>=1536）。
   逐笔归属由 nvn_vk.c 的 draw 台账每笔 drain（和门 113/114 同款），所以本文件只做汇总。
   开关 smo_m17a（C 默认 0；`echo 1` ≤2 s 生效）。命名：smo_m17a / [m17a] / m17a_boot_run.sh /
   /tmp/m17a_report.md / logs/m17a_* —— 五类开工前三查（19:16 全 0 命中）。 */
#define M17A_PATH_A "/data/storage/el2/base/files/smo_m17a"
#define M17A_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m17a"
static int m17a_mode(void)
{
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M17A_PATH_A : M17A_PATH_B;
        if ((f = fopen(p, "r"))) {
            size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
            return (n > 0 && b[0] == '1') ? 1 : 0;
        }
    }
    return 0;
}
void runtime_m17a_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern uint64_t smo_obs_m17a_k(void);
    extern void smo_obs_m17a_meta(uint64_t *k, uint64_t *first_tick, uint64_t *last_tick,
                                  uint64_t *first_lr, uint64_t *last_lr,
                                  uint64_t *h_used, uint64_t *h_ovf, uint64_t *pend_ovf,
                                  uint64_t *max_per_frame, uint64_t *cur_sub,
                                  uint64_t *x0_last, uint64_t *x1_last, uint64_t *x2_last, uint64_t *x3_last,
                                  uint64_t *x0_first, uint64_t *sub_first, uint64_t *sub_last);
    extern int smo_obs_m17a_hist(int N, uint64_t *lr, uint64_t *n_early, uint64_t *n_late);
    extern int runtime_safe_read_ungated(void *dst, uint64_t addr, size_t n);
    extern uint64_t smo_obs_main_base(void);
    extern _Atomic int g_m17a_on;
    static double next_sum = -1.0;
    static int ann, last_hits = -1;
    int on = m17a_mode();
    atomic_store_explicit(&g_m17a_on, on, memory_order_relaxed);
    if (!ann) {
        ann = 1;
        next_sum = (double)tsec + 5.0;
        vk_boot_logf("[m17a] probe on (t=%us) gate=115 main+0x98b4b0 = 2D quad record leaf "
                     "(fn 0x98b490 +0x20; words aa0203f3 aa0103f4 aa0003f5 91058017, tail=0, "
                     "prev 9100c3fd untouched) | switch smo_m17a=%d (0=off, 1=on) "
                     "| payload x0/x1/x2/x3/x30 (7-arg variant stub) | [x2] = texture key, -1 = skip at +0x8 "
                     "| static: 277 call sites / 130 host funcs to fn entry, 0 to +0x20",
                     tsec, on);
    }
    if ((double)tsec < next_sum) return;
    next_sum += 5.0;
    uint64_t k = 0, ft = 0, lt = 0, flr = 0, llr = 0, used = 0, hovf = 0, povf = 0;
    uint64_t mpf = 0, csub = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0, f0 = 0, sf = 0, sl = 0;
    smo_obs_m17a_meta(&k, &ft, &lt, &flr, &llr, &used, &hovf, &povf, &mpf, &csub,
                      &a0, &a1, &a2, &a3, &f0, &sf, &sl);
    if ((int)k == last_hits) return;
    last_hits = (int)k;
    uint64_t mb = smo_obs_main_base();
    unsigned tk = 0xffffffffu; int tk_ok = 0;
    if (a2) { uint32_t wv = 0; if (runtime_safe_read_ungated(&wv, a2, 4)) { tk = wv; tk_ok = 1; } }
    vk_boot_logf("[m17a] sum t=%us on=%d gate=115 hits=%llu distinct=%llu hist_ovf=%llu "
                 "pend_ovf=%llu per_frame_max=%llu cur_sub=%llu sub_first=%llu sub_last=%llu "
                 "first_lr=%#llx(main+%#llx) last_lr=%#llx(main+%#llx) x0_first=%#llx x0_last=%#llx "
                 "x2_last=%#llx tk_last=%#x(tk_ok=%d) main_base=%#llx",
                 tsec, on, (unsigned long long)k, (unsigned long long)used,
                 (unsigned long long)hovf, (unsigned long long)povf,
                 (unsigned long long)mpf, (unsigned long long)csub,
                 (unsigned long long)sf, (unsigned long long)sl,
                 (unsigned long long)flr, (unsigned long long)(mb && flr > mb ? flr - mb : 0ull),
                 (unsigned long long)llr, (unsigned long long)(mb && llr > mb ? llr - mb : 0ull),
                 (unsigned long long)f0, (unsigned long long)a0,
                 (unsigned long long)a2, tk, tk_ok, (unsigned long long)mb);
    uint64_t HLR[12], HNE[12], HNL[12];
    int hn = smo_obs_m17a_hist(12, HLR, HNE, HNL);
    for (int i = 0; i < hn; i++)
        vk_boot_logf("[m17a] h #%d lr=%#llx main+%#llx n=%llu (early=%llu late=%llu)",
                     i + 1, (unsigned long long)HLR[i],
                     (unsigned long long)(mb && HLR[i] > mb ? HLR[i] - mb : 0ull),
                     (unsigned long long)(HNE[i] + HNL[i]),
                     (unsigned long long)HNE[i], (unsigned long long)HNL[i]);
}

/* ---- M18A（2026-11-08）：**UI 颜色入参**两门（116 = main+0x8fea2c / 117 = main+0x9106fc）----
   观察器在 game_patches.c（30 字 7 参变体 stub：x0/x1/x2/x3 + x30 全交过来）；本文件只做开关刷新
   与 5 s 汇总，**逐笔出证在 nvn_vk.c 的 draw 台账里**（每笔 draw 领走自上一笔以来的命中）。
   开关 smo_m18a（C 默认 0；`echo 1` ≤2 s 生效）。
   命名：smo_m18a / [m18a] / m18a_boot_run.sh / /tmp/m18a_report.md / logs/m18a_* —— 五类开工前三查。 */
#define M18A_PATH_A "/data/storage/el2/base/files/smo_m18a"
#define M18A_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m18a"
static int m18a_mode(void)
{
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M18A_PATH_A : M18A_PATH_B;
        if ((f = fopen(p, "r"))) {
            size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
            return (n > 0 && b[0] == '1') ? 1 : 0;
        }
    }
    return 0;
}
void runtime_m18a_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern void smo_obs_m18a_meta(uint64_t *k116, uint64_t *k117, uint64_t *ovf116, uint64_t *ovf117,
                                  uint64_t *x0_last, uint64_t *x1_last, uint64_t *x3_last, uint64_t *lr_last,
                                  uint64_t *sub_first, uint64_t *sub_last);
    extern uint64_t smo_obs_main_base(void);
    extern _Atomic int g_m18a_on;
    static double next_sum = -1.0;
    static int ann, lastk = -1;
    int on = m18a_mode();
    atomic_store_explicit(&g_m18a_on, on, memory_order_relaxed);
    if (!ann) {
        ann = 1;
        next_sum = (double)tsec + 5.0;
        vk_boot_logf("[m18a] probe on (t=%us) gates: 116 main+0x8fea2c (2D color-draw helper entry, "
                     "x3 = color ptr = [ctx+0xb8]+0x658, tail br x6) + 117 main+0x9106fc (2D panel/text "
                     "renderer entry, x0 = UI layer with packed colors at +0xf0..+0xfc, x1 = render ctx) "
                     "| switch smo_m18a=%d (0=off, 1=on) | payload x0/x1/x2/x3/x30 (7-arg variant stub) "
                     "| per-draw drain in nvn_vk (safe reads: [x3]16B, [[ctx+0xb8]+0x658]16B, [x0+0xf0]16B)",
                     tsec, on);
    }
    if ((double)tsec < next_sum) return;
    next_sum += 5.0;
    uint64_t k0 = 0, k1 = 0, o0 = 0, o1 = 0, a0 = 0, a1 = 0, a3 = 0, lr = 0, sf = 0, sl = 0;
    smo_obs_m18a_meta(&k0, &k1, &o0, &o1, &a0, &a1, &a3, &lr, &sf, &sl);
    if ((int)(k0 + k1) == lastk) return;
    lastk = (int)(k0 + k1);
    uint64_t mb = smo_obs_main_base();
    vk_boot_logf("[m18a] sum t=%us on=%d g116=%llu g117=%llu ovf=%llu/%llu x0_last=%#llx x1_last=%#llx "
                 "x3_last=%#llx lr_last=%#llx(main+%#llx) sub=%llu..%llu main_base=%#llx",
                 tsec, on, (unsigned long long)k0, (unsigned long long)k1,
                 (unsigned long long)o0, (unsigned long long)o1,
                 (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a3,
                 (unsigned long long)lr, (unsigned long long)(mb && lr > mb ? lr - mb : 0ull),
                 (unsigned long long)sf, (unsigned long long)sl, (unsigned long long)mb);
}

/* ---- M19A（2026-11-08）：**门 118 = main+0x916a28**（2D 批处理器 fn 0x915dfc 的 +0xc2c）----
   判 (a) vs (b)：同一笔上同时读「RGB 源（st+0x700/704/708）」与「三通道"有颜色"标志
   （st+0x8b0/8b1/8b2）」，再从 ctx+0xb8 → st → st+0x658 取 12 B 颜色缓冲区**最终值**对照。
   观察器在 game_patches.c（30 字 7 参变体 stub：x0/x8/x1/x2/x3 + x30 全交过来）；本文件只做
   开关刷新与 5 s 汇总，**逐笔出证在 nvn_vk.c 的 draw 台账里**。
   开关 smo_m19a（C 默认 0；`echo 1` ≤2 s 生效）。
   命名：smo_m19a / [m19a] / m19a_boot_run.sh / /tmp/m19a_report.md / logs/m19a_* —— 五类开工前三查。 */
#define M19A_SW_A "/data/storage/el2/base/files/smo_m19a"
#define M19A_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m19a"
static int m19a_mode(void)
{
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M19A_SW_A : M19A_SW_B;
        if ((f = fopen(p, "r"))) {
            size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
            return (n > 0 && b[0] == '1') ? 1 : 0;
        }
    }
    return 0;
}
void runtime_m19a_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern void smo_obs_m19a_meta(uint64_t *k, uint64_t *ovf, uint64_t *lr_last, uint64_t *ctx_last,
                                  uint64_t *st_last, uint64_t *x0_last, uint64_t *x3_last,
                                  uint64_t *sub_first, uint64_t *sub_last);
    extern uint64_t smo_obs_main_base(void);
    extern _Atomic int g_m19a_on;
    static double next_sum = -1.0;
    static int ann, lastk = -1;
    int on = m19a_mode();
    atomic_store_explicit(&g_m19a_on, on, memory_order_relaxed);
    if (!ann) {
        ann = 1;
        next_sum = (double)tsec + 5.0;
        vk_boot_logf("[m19a] probe on (t=%us) gate 118 main+0x916a28 (2D batch proc fn 0x915dfc +0xc2c: "
                     "ldr d0,[x8,#0x700]; ldr w8,[x8,#0x708]; str q0,[x19,#0x320]) "
                     "| switch smo_m19a=%d (0=off, 1=on) | payload x0/x8(state)/x1(ctx)/x2/x3/x30 "
                     "| per-draw drain in nvn_vk (safe reads: st+0x700 RGB src, st+0x8b0..b2 flags, "
                     "ctx+0x320, [[ctx+0xb8]+0x658] -> 12B color buffer)",
                     tsec, on);
    }
    if ((double)tsec < next_sum) return;
    next_sum += 5.0;
    uint64_t k = 0, ovf = 0, lr = 0, ctx = 0, st = 0, x0 = 0, x3 = 0, sf = 0, sl = 0;
    smo_obs_m19a_meta(&k, &ovf, &lr, &ctx, &st, &x0, &x3, &sf, &sl);
    if ((int)k == lastk) return;
    lastk = (int)k;
    uint64_t mb = smo_obs_main_base();
    vk_boot_logf("[m19a] sum t=%us on=%d g118=%llu ovf=%llu ctx_last=%#llx st_last=%#llx x0_last=%#llx "
                 "x3_last=%#llx lr_last=%#llx(main+%#llx) sub=%llu..%llu main_base=%#llx",
                 tsec, on, (unsigned long long)k, (unsigned long long)ovf,
                 (unsigned long long)ctx, (unsigned long long)st, (unsigned long long)x0,
                 (unsigned long long)x3, (unsigned long long)lr,
                 (unsigned long long)(mb && lr > mb ? lr - mb : 0ull),
                 (unsigned long long)sf, (unsigned long long)sl, (unsigned long long)mb);
}

/* ---- M8C（2026-11-08）：**条件注入真机 nerve token**（本项目第一次"改状态"实验）----
   在唯一 nerve 写点 main+0x6c7750 处，把**序列 HostType keeper** 的新 token 有条件地换成
   真机的 HostTypeNrvLoadFirstSequence（main+0x12d0e08，运行期按 base 现算）。
   机制在 game_patches.c：id 104 的 stub 换成 30 字变体（w20 mov x17,x0 / w25 cbz x17,+8 /
   w26 mov x1,x17 / quad = smo_obs_observer_m8c）⇒ **只改 x1**，其它寄存器照原骨架复原；
   观察器返回 0 时 cbz 跳过 ⇒ x1 保持原值（**对照 boot 走的就是这条**）。
   开关 smo_m8c（C 默认 0；`echo 0` ≤2 s 回退，等于完全什么都不做）：
     0 = 关；1/2/3 = 从第 N 次 keeper 命中起注入（每 boot 注入上限 3）。
   节拍：probe 一次；code（30 字变体注入字回读）t≥10 s 一次；sum 每 5 s；注入记录 t≈20/60/100/140
        与"注入数变化"时各一次。
   ⚠️ 名字用 m8c：五类开工前全 0 命中（2026-11-08 14:3x）；`smo_m8b`/`[m8b]` 那套一字未动。 */
#define M8C_NAMELEN 72
#define M8C_PATH_A "/data/storage/el2/base/files/smo_m8c"
#define M8C_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m8c"
/* 种子：**只在物理路径 B 不存在时**建 0。
   ⚠️ M8C 首版照 m8b 的写法以 A（沙箱别名）判存在，实测在模拟器上 A 常 open 失败 ⇒ 每次启动都重建，
   把脚本刚写进 B 的开关值冲成 0（s3one 那次 boot 的 `sw=0` 就是这么来的，已作废）。
   改成以 B 判存在/建文件，脚本与 app 看的是同一个 inode。 */
__attribute__((constructor)) static void m8c_seed(void) {
    struct stat st;   /* 用 stat 判存在：fopen("r") 在沙箱别名/物理路径之一上会假失败，
                         首版因此**每次启动都把脚本刚写的开关值冲成 0**（s3one/s4one 两次 boot 作废）。 */
    if (stat(M8C_PATH_A, &st) != 0 && stat(M8C_PATH_B, &st) != 0) {
        FILE *f = fopen(M8C_PATH_B, "w");
        if (!f) f = fopen(M8C_PATH_A, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m8c] seeded = 0 (off)\n"); }
    }
}
/* 读开关值：支持 1~2 位十进制（1..3 / 11..13 / 21..23；0 = 关）。
   ⚠️ M8C 首版只取 b[0] 一位 ⇒ "21" 被读成 2、语义与日志不符（s2one 那次 boot 因此作废）。 */
static int m8c_val(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M8C_PATH_A : M8C_PATH_B;     /* B（脚本写的物理路径）优先，A 只兜底 */
        if ((f = fopen(p, "r"))) {
            size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
            int v = 0, d = 0;
            for (size_t k = 0; k < n && d < 2; k++) {
                if (b[k] < '0' || b[k] > '9') break;
                v = v * 10 + (b[k] - '0'); d++;
            }
            return d ? v : 0;
        }
    }
    return 0;
}
/* 7 个判别 token 的短名（全部由加载期 reloc 行定名，见 game_patches.c M8C 注） */
static const char *m8c_tok_name(uint64_t rel) {
    switch (rel) {
    case 0x12d06d8: return "HostType[slot0->Boot]";
    case 0x12d0678: return "HostTypeNrvBootLayout";
    case 0x12d06e8: return "HostType[slot2->BootLayoutInitScene]";
    case 0x12d06b8: return "HostTypeNrvMenu";
    case 0x12d06e0: return "HostType[slot1->DestroyMenu]";
    case 0x12d0e08: return "HostTypeNrvLoadFirstSequence";
    case 0x12d0e48: return "HostTypeNrvFirstSequence";
    case 0x12d0d48: return "HostTypeNrvBoot(f1, exec 0x4b8d30)";
    default: return "-";
    }
}
void runtime_m8c_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m8c_nrec(void);
    extern int smo_obs_m8c_rec(int i, uint64_t m[7]);
    extern void smo_obs_m8c_sum(uint64_t *ident, uint64_t *inj, uint64_t *latch, uint64_t *latchns, int *want);
    extern uint64_t smo_obs_main_base(void);
    extern uint64_t smo_obs_stub_off_gate(int gate);
    extern _Atomic int g_m8c_on;
    static double next_sum;
    static int ann, code_done, ring20, ring60, ring100, ring140;
    static uint64_t last_inj = ~0ULL;
    int v = m8c_val();
    atomic_store_explicit(&g_m8c_on, v, memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m8c] probe on (t=%us) sw=%d cap=3 tgt=main+0x12d0e08(HostTypeNrvLoadFirstSequence) "
                     "site=id104 main+0x6c7750 keeper-match=[+0x08 cur | +0x10 pend] rel 7-token set | "
                     "code=10s sum=5s rec=20/60/100/140s | 0=off 1..3=0x12d0e08 from N-th match / "
                     "11..13=f1 Boot 0x12d0d48 from N-10 / 21..23=0x12d0e08 one-shot",
                     tsec, v);
        next_sum = (double)tsec + 5.0;
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base(), so = smo_obs_stub_off_gate(104);
        uint32_t win[19] = { 0 }, pre[4] = { 0 };      /* win[0..18] = stub 的 w9..w27；pre[0..3] = w11..w14 */
        int ok = 0, okp = 0;
        if (mb && so) {
            ok = runtime_safe_read_ungated(win, mb + so + 9 * 4, sizeof win);
            okp = runtime_safe_read_ungated(pre, mb + so + 0x2c, sizeof pre);
        }
        vk_boot_logf("[m8c] code t=%us gate=104 stub=main+%#llx ok=%d w9=%08x w11=%08x w20=%08x w21=%08x w22=%08x w23=%08x w24=%08x | "
                     "w25=%08x w26=%08x w27=%08x | expect w20=aa0003f1(mov x17,x0) w25=b4000051(cbz x17,+8) "
                     "w26=aa1103e1(mov x1,x17) w11=58000230(ldr x16,#0x44) w27=1400xxxx(b entry+0x10)",
                     tsec, (unsigned long long)so, ok, ok ? win[0] : 0, okp ? pre[0] : 0,
                     ok ? win[11] : 0, ok ? win[12] : 0, ok ? win[13] : 0, ok ? win[14] : 0, ok ? win[15] : 0,
                     ok ? win[16] : 0, ok ? win[17] : 0, ok ? win[18] : 0);
    }
    if ((double)tsec >= next_sum) {
        next_sum += 5.0;
        uint64_t ident = 0, inj = 0, latch = 0, ln = 0;
        int want = 0;
        uint64_t mb = smo_obs_main_base();
        smo_obs_m8c_sum(&ident, &inj, &latch, &ln, &want);
        vk_boot_logf("[m8c] sum t=%us sw=%d ident_writes=%llu inj=%llu cap=3 latch_keeper=%#llx(rel main+%#llx) "
                     "latch_ns=%llus", tsec, want, (unsigned long long)ident, (unsigned long long)inj,
                     (unsigned long long)latch,
                     (unsigned long long)(latch && mb && latch > mb ? latch - mb : 0),
                     (unsigned long long)(ln / 1000000000ull));
    }
    {   /* 注入记录：每次注入数变化 + 20/60/100/140 s 各一次 */
        int want = 0;
        uint64_t inj = 0, ident = 0, latch = 0, ln = 0; int sw = 0;
        smo_obs_m8c_sum(&ident, &inj, &latch, &ln, &sw);
        if (inj != last_inj) { last_inj = inj; want = 1; }
        if (!ring20 && (double)tsec >= 20.0)  { ring20 = 1;  want = 1; }
        if (!ring60 && (double)tsec >= 60.0)  { ring60 = 1;  want = 1; }
        if (!ring100 && (double)tsec >= 100.0) { ring100 = 1; want = 1; }
        if (!ring140 && (double)tsec >= 140.0) { ring140 = 1; want = 1; }
        if (want) {
            uint64_t mb = smo_obs_main_base();
            int n = smo_obs_m8c_nrec();
            for (int i = 0; i < n; i++) {
                uint64_t m[7] = { 0 };
                char d2[M8C_NAMELEN] = { 0 }, d3[M8C_NAMELEN] = { 0 };
                if (!smo_obs_m8c_rec(i, m)) continue;
                if (m[6]) runtime_describe_address(m[6], d2, sizeof d2);
                if (m[1]) runtime_describe_address(m[1], d3, sizeof d3);
                uint64_t rc = (m[2] && mb && m[2] > mb) ? m[2] - mb : 0;
                uint64_t rp = (m[3] && mb && m[3] > mb) ? m[3] - mb : 0;
                vk_boot_logf("[m8c] inj t=%us #%d ns=%llus keeper=%#llx(%s) cur8=main+%#llx(%s) "
                             "pend10=main+%#llx(%s) old_x1=%#llx new_x1=%#llx lr=%#llx(%s)",
                             tsec, i, (unsigned long long)(m[0] / 1000000000ull),
                             (unsigned long long)m[1], d3[0] ? d3 : "-",
                             (unsigned long long)rc, m8c_tok_name(rc),
                             (unsigned long long)rp, m8c_tok_name(rp),
                             (unsigned long long)m[4], (unsigned long long)m[5],
                             (unsigned long long)m[6], d2[0] ? d2 : "-");
            }
        }
    }
}

/* M8C：崩溃现场转储（由 loader.c 的 SIGSEGV/SIGBUS handler 在写完 [fx] FAULT 那一行后立刻调）。
   目的：注入 boot 一旦跑飞，**注入记录（keeper/原值/新值/时刻）不能随进程一起消失**。
   只读静态计数器 + 一次 trylock 日志，不解引用任何客体指针 ⇒ 在 handler 里安全。 */
void runtime_m8c_fault_dump(void) {
    extern void vk_boot_logf_try(const char *fmt, ...);
    extern int smo_obs_m8c_nrec(void);
    extern int smo_obs_m8c_rec(int i, uint64_t m[7]);
    extern void smo_obs_m8c_sum(uint64_t *ident, uint64_t *inj, uint64_t *latch, uint64_t *latchns, int *want);
    extern uint64_t smo_obs_main_base(void);
    uint64_t ident = 0, inj = 0, latch = 0, ln = 0; int sw = 0;
    smo_obs_m8c_sum(&ident, &inj, &latch, &ln, &sw);
    uint64_t mb = smo_obs_main_base();
    vk_boot_logf_try("[m8c] faultdump sw=%d ident_writes=%llu inj=%llu latch_keeper=%#llx(rel main+%#llx) "
                     "latch_ns=%llus nrec=%d", sw, (unsigned long long)ident, (unsigned long long)inj,
                     (unsigned long long)latch,
                     (unsigned long long)(latch && mb && latch > mb ? latch - mb : 0),
                     (unsigned long long)(ln / 1000000000ull), smo_obs_m8c_nrec());
    int n = smo_obs_m8c_nrec();
    for (int i = 0; i < n; i++) {
        uint64_t m[7] = { 0 };
        if (!smo_obs_m8c_rec(i, m)) continue;
        char d2[M8C_NAMELEN] = { 0 };
        if (m[6]) runtime_describe_address(m[6], d2, sizeof d2);
        uint64_t rc = (m[2] && mb && m[2] > mb) ? m[2] - mb : 0;
        uint64_t rp = (m[3] && mb && m[3] > mb) ? m[3] - mb : 0;
        uint64_t ro = (m[4] && mb && m[4] > mb) ? m[4] - mb : 0;
        uint64_t rn = (m[5] && mb && m[5] > mb) ? m[5] - mb : 0;
        vk_boot_logf_try("[m8c] faultinj #%d ns=%llus keeper=%#llx cur8=main+%#llx(%s) pend10=main+%#llx(%s) "
                         "old_x1=main+%#llx(%s) new_x1=main+%#llx(%s) lr=%#llx(%s)",
                         i, (unsigned long long)(m[0] / 1000000000ull), (unsigned long long)m[1],
                         (unsigned long long)rc, m8c_tok_name(rc),
                         (unsigned long long)rp, m8c_tok_name(rp),
                         (unsigned long long)ro, m8c_tok_name(ro),
                         (unsigned long long)rn, m8c_tok_name(rn),
                         (unsigned long long)m[6], d2[0] ? d2 : "-");
    }
}

/* M6g: the wrapper is what services.c calls; the body keeps every existing return. */
static int svc_thread_body(int num, uint64_t *regs);int svc_thread(int num, uint64_t *regs) {
    thr_t *me = me_or_null();
    int slot = -1, mslot = -1;
    if (me && atomic_load_explicit(&g_m7y_on, memory_order_relaxed)) m7y_pc_note(me);   /* M7y: guest PC at every svc entry (main full, others 1/16) */
    if (me && me->handle == 0x1001u) {
        ldr_win_count((unsigned)num);
        if (atomic_load_explicit(&g_ldr_cap_left, memory_order_relaxed)) slot = ldr_step_begin((unsigned)num, regs);
    } else if (me && me->handle == M6K_H) {
        mslot = m6k_svc_begin((unsigned)num, regs, me->last_svc);
    }
    int r = svc_thread_body(num, regs);
    if (slot >= 0) ldr_step_end(slot, regs);
    if (mslot >= 0) m6k_svc_end(mslot, regs);
    return r;
}
/* ---- M7W2（2026-11-08）：**在适配器里复算真 ctor**（门号 94，main+0x7724d0）----
   M7V 的硬结论：两平台 slot 0 落点**逐字相同**（都是这个通用 mfp 适配器），所以
   "slot0 目标不同"不成立；真正的 ctor 由适配器按描述符对象 {+8 基址, +0x10 槽/函数指针,
   +0x18 tag, +0x20 实参} 在**运行期**解出。本轮的活儿就是把这两步算术在观察器里复算一遍，
   把 obj / tag / p / o / q / vt / tgt 全部落日志，并把 tgt 交给 runtime_describe_address 点名。
     q   = [obj+8] + (tag>>1)                 （真 this）
     tgt = (tag&1) ? *(*(q)+[obj+0x10]) : [obj+0x10]
   节拍：probe 一次；code（门窗逐字回读 + 门后 3 字 + prev + 0x7724f0 的 br x2）t≥10 s 一次；
   ad（环里新记录，≤16 条）每 10 s 增量打印；sum 每 10 s。
   Switch smo_m7w2（C 默认 0；`echo 0 > .../files/smo_m7w2` ≤2 s 回退）。
   ⚠️ 名字用 m7w2：`smo_m7w` / `[m7w]` / `runtime_m7w_poll` 是 2026-11-07 那轮的活基线，不覆盖。 */
#define M7W2_PATH_A "/data/storage/el2/base/files/smo_m7w2"
#define M7W2_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7w2"
__attribute__((constructor)) static void m7w2_seed(void) {
    if (!fopen(M7W2_PATH_A, "r")) {
        FILE *f = fopen(M7W2_PATH_A, "w");
        if (!f) f = fopen(M7W2_PATH_B, "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7w2] seeded = 0 (off)\n"); }
    }
}
static int m7w2_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7W2_PATH_B : M7W2_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
extern _Atomic int g_m7w2_on;
/* 出厂字（/tmp/m7w2_safety.py 本轮重扫：三镜像逐字一致、word[0] 非分支非 PC 相对、
   全镜像分支落点首字/窗内中间字全 0、与既有 86 行站表零重叠）。 */
static const struct { uint32_t site, win[4], aft[3], br[1], prev; } m7w2_g = {
    0x7724d0,
    { 0xa9408808, 0xf9400c09, 0x8b890508, 0x36000069 },   /* ldp x8,x2,[x0,#8] ; ldr x9,[x0,#0x18] ; add ; tbz */
    { 0xf9400109, 0xf8626922, 0xf9401001 },               /* 0x7724e0 ldr x9,[x8] ; 0x7724e4 ldr x2,[x9,x2] ; 0x7724e8 ldr x1,[x0,#0x20] */
    { 0xd61f0040 },                                       /* 0x7724f0 br x2（尾跳，一个字节都不碰） */
    0x00000000                                            /* prev = 0x7724cc（未用填充） */
};
void runtime_m7w2_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern int smo_obs_m7w2_ad(int i, uint64_t meta[11], uint32_t *kind, uint32_t *ok,
                               uint32_t *rd, uint32_t *thr);
    extern int smo_obs_m7w2_ad_n(void);
    extern int smo_obs_m7w2_sum(uint64_t *total, uint64_t lrd[][4], int *nlr,
                                uint64_t first[3], uint64_t last[3], uint64_t *lr_ovf);
    extern uint64_t smo_obs_main_base(void);
    static double next_sum, next_ad;
    static int ann, code_done, ad_printed;
    uint64_t lrd[8][4];
    atomic_store_explicit(&g_m7w2_on, m7w2_on(), memory_order_relaxed);
    if (!ann) {
        ann = 1;
        vk_boot_logf("[m7w2] probe on (t=%us) ADAPTER recalc gate id=94 at main+0x7724d0 (tail2: only "
                     "word0 'ldp x8,x2,[x0,#8]' replaced; word1 'ldr x9,[x0,#0x18]' + 0x7724f0 'br x2' "
                     "untouched) sw=%d | q=[obj+8]+(tag>>1); tgt=(tag&1)?*(*(q)+[obj+0x10]):[obj+0x10] "
                     "| ring16 = obj/tag/p/o/q/vt/tgt/arg all described | ad=10s sum=10s code=10s",
                     tsec, (int)atomic_load_explicit(&g_m7w2_on, memory_order_relaxed));
        next_sum = (double)tsec + 10.0;
        next_ad = (double)tsec + 10.0;
    }
    if (!code_done && (double)tsec >= 10.0) {
        code_done = 1;
        uint64_t mb = smo_obs_main_base();
        uint32_t a[11]; int ok = mb && runtime_safe_read_ungated(a, mb + m7w2_g.site, sizeof a);
        uint32_t pv = 0;
        if (mb) pv = runtime_safe_read_ungated(&pv, mb + m7w2_g.site - 4, 4) ? pv : 0;
        unsigned same = 0;
        if (ok) for (int i = 0; i < 3; i++) if (a[4 + i] == m7w2_g.aft[i]) same++;
        int w123 = ok && a[1] == m7w2_g.win[1] && a[2] == m7w2_g.win[2] && a[3] == m7w2_g.win[3];
        int brx2 = ok && a[8] == m7w2_g.br[0];
        vk_boot_logf("[m7w2] code t=%us site=main+%#x ok=%d win[0..3]=%08x %08x %08x %08x "
                     "exp_win0=%08x(veneer b) exp_win123=%08x %08x %08x win123_same=%d "
                     "after(main+%#x..+8) same=%u/3 %08x %08x %08x | main+%#x br x2=%08x exp=%08x "
                     "same=%d | prev(main+%#x)=%08x exp_prev=%08x",
                     tsec, m7w2_g.site, ok,
                     ok ? a[0] : 0, ok ? a[1] : 0, ok ? a[2] : 0, ok ? a[3] : 0,
                     m7w2_g.win[0], m7w2_g.win[1], m7w2_g.win[2], m7w2_g.win[3], w123,
                     m7w2_g.site + 0x10, same,
                     ok ? a[4] : 0, ok ? a[5] : 0, ok ? a[6] : 0,
                     m7w2_g.site + 0x20, ok ? a[8] : 0, m7w2_g.br[0], brx2,
                     m7w2_g.site - 4, pv, m7w2_g.prev);
    }
    if ((double)tsec >= next_ad) {
        next_ad = (double)tsec + 10.0;
        int n = smo_obs_m7w2_ad_n();
        if (n > ad_printed) {
            for (int i = ad_printed; i < n; i++) {
                uint64_t m[11] = { 0 }; uint32_t kind = 0, ok = 0, rd = 0, thr = 0;
                if (!smo_obs_m7w2_ad(i, m, &kind, &ok, &rd, &thr)) continue;
                char dl[64], do_[64], dq[64], dv[64], dt[64], dp[64], dd[64], da[64];
                runtime_describe_address(m[1], dl, sizeof dl);   /* lr */
                runtime_describe_address(m[2], do_, sizeof do_);  /* obj */
                runtime_describe_address(m[3], dp, sizeof dp);    /* p  */
                runtime_describe_address(m[4], da, sizeof da);    /* o  */
                runtime_describe_address(m[7], dq, sizeof dq);    /* q  */
                runtime_describe_address(m[8], dv, sizeof dv);    /* vt */
                runtime_describe_address(m[9], dt, sizeof dt);    /* tgt */
                runtime_describe_address(m[10], dd, sizeof dd);   /* dtbl = [obj] */
                vk_boot_logf("[m7w2] ad #%d ns=%llus lr=%#llx(%s) obj=%#llx(%s) dtbl=%#llx(%s) "
                             "tag=%#llx(%s) kind=%s p=%#llx(%s) o=%#llx(%s) q=%#llx(%s) "
                             "vt=%#llx(%s) tgt=%#llx(%s) arg=%#llx rd=%#x ok=%u thr=%u",
                             i, (unsigned long long)(m[0] / 1000000000ull),
                             (unsigned long long)m[1], dl, (unsigned long long)m[2], do_,
                             (unsigned long long)m[10], dd, (unsigned long long)m[5],
                             (m[5] & 1) ? "V" : "n", kind == 1 ? "virtual" : (kind == 0 ? "nonvirt" : "unknown"),
                             (unsigned long long)m[3], dp, (unsigned long long)m[4], da,
                             (unsigned long long)m[7], dq, (unsigned long long)m[8], dv,
                             (unsigned long long)m[9], dt, (unsigned long long)m[6], rd, ok, thr);
            }
            ad_printed = n;
        }
    }
    if ((double)tsec >= next_sum) {
        next_sum += 10.0;
        uint64_t tot = 0, first[3] = { 0 }, last[3] = { 0 }, ov = 0;
        int nlr = 0;
        (void)smo_obs_m7w2_sum(&tot, lrd, &nlr, first, last, &ov);
        char df[64], dfl[64];
        runtime_describe_address(first[1], df, sizeof df);
        runtime_describe_address(last[1], dfl, sizeof dfl);
        vk_boot_logf("[m7w2] sum t=%us gate=94 hits=%llu first_ns=%llus first_lr=%#llx(%s) "
                     "first_obj=%#llx last_ns=%llus last_lr=%#llx(%s) last_obj=%#llx "
                     "uniq_lr=%d lr_ovf=%llu ring_n=%d",
                     tsec, (unsigned long long)tot,
                     (unsigned long long)(first[0] / 1000000000ull), (unsigned long long)first[1], df,
                     (unsigned long long)first[2],
                     (unsigned long long)(last[0] / 1000000000ull), (unsigned long long)last[1], dfl,
                     (unsigned long long)last[2], nlr, (unsigned long long)ov, smo_obs_m7w2_ad_n());
        for (int i = 0; i < nlr; i++) {
            char d2[64] = { 0 };
            if (lrd[i][0]) runtime_describe_address(lrd[i][0], d2, sizeof d2);
            vk_boot_logf("[m7w2] lr t=%us #%d lr=%#llx site=%#llx n=%llu first_ns=%llus last_ns=%llus desc=%s",
                         tsec, i, (unsigned long long)lrd[i][0],
                         (unsigned long long)(lrd[i][0] ? lrd[i][0] - 4 : 0),
                         (unsigned long long)lrd[i][1],
                         (unsigned long long)(lrd[i][2] / 1000000000ull),
                         (unsigned long long)(lrd[i][3] / 1000000000ull), d2[0] ? d2 : "-");
        }
    }
}

static int svc_thread_body(int num, uint64_t *regs) {
	{ thr_t *me = (g_keythr ? pthread_getspecific(g_keythr) : NULL); if (me) { me->last_svc = num; me->last_lr = regs[30]; me->last_fp = regs[29]; me->nsvc++; me->op = 0;   /* M5b：回到客体代码（能走 SVC）= 已离开宿主 NVN 重活，op 清零，冻结时仍非 0 的才算「卡在里面」 */
	       runtime_cur_handle_tls = me->handle;   /* M7c: gate-observer identity, refreshed at every svc */
	       /* M6h: two-instruction per-thread SVC histogram; the [own] line reports its top entry, so
	          "this thread is a poller / this thread does IPC" is readable for the event owner too. */
	       /* M6i: saturate instead of wrapping - a 900 s boot lets a poller pass 65535 calls on one
	          slot, and a wrapped counter would be read as "this SVC is rare" by [own]/[io] svc. */
	       if (me->svch && me->svch[num & 0xffu] != 0xffffffffu) me->svch[num & 0xffu]++;
	       if (me->mvs >= 0) lp_mover_note(me->mvs, (unsigned)num, regs);   /* M6l: per-class count + last LR of the observed threads */
	       {   /* M6m: rolling last-8 SVCs of this thread (arguments at entry + guest LR), so a thread that
	              stops keeps the eight calls that led to the stop. Plain stores, own thread only. */
	           unsigned i8 = me->svc8i & 7u;
	           me->svc8[i8].num = (uint32_t)(num & 0xff); me->svc8[i8].k = g_ps_dno;
	           me->svc8[i8].a0 = regs[0]; me->svc8[i8].a1 = regs[1]; me->svc8[i8].a2 = regs[2];
	           me->svc8[i8].a3 = regs[3]; me->svc8[i8].lr = regs[30];
	           /* M6o: real SVC pc + frame-pointer backtrace, main only (see ps_svc8x_t). The gate saved the
	              caller's fp untouched (the svc stub is a leaf), so [fp] = parent fp and [fp+8] = the return
	              address of the frame above, i.e. the call stack of the code that issued the syscall.
	              Every hop is a fault-safe read and every value is printed through runtime_describe_address,
	              so a bogus chain shows as "-" instead of killing the process. */
	           if (me->handle == 0x1001u) {
	               extern __thread uint64_t runtime_svc_pc_tls; extern __thread const char *runtime_svc_mod_tls;
	               ps_svc8x_t *x = &me->svc8x[i8];
	               x->num = (uint32_t)(num & 0xff); x->a0 = regs[0]; x->a1 = regs[1]; x->a2 = regs[2];
	               x->a3 = regs[3]; x->lr = regs[30]; x->pc = runtime_svc_pc_tls; x->mod = runtime_svc_mod_tls;
	               x->bt[0] = x->bt[1] = x->bt[2] = 0;   /* a reused ring slot must not show the previous call's chain */
	               uint64_t fp = regs[29];
	               for (int b = 0; b < 3; b++) {
	                   uint64_t w[2] = { 0, 0 };
	                   if (!fp || (fp & 7u) || !runtime_safe_read_ungated(w, fp, sizeof w)) break;
	                   if (!w[1] || (w[1] & 3u)) break;
	                   x->bt[b] = w[1]; fp = w[0];
	               }
	           }
	           me->svc8i = i8 + 1u;
	           me->svc8t++;
	       }
	       /* M7c: main's svc-entry call-site histogram (read-only counters; printing on the
	          heartbeat thread). regs untouched here - the capture reads fp/sp/stack via safe reads. */
	       if (me->handle == 0x1001u && atomic_load_explicit(&g_m7c_on, memory_order_relaxed))
	           m7c_svc_note(num, regs, me);
	       /* services.c 对每个 SVC 都先走这里 ⇒ 即 donor 的 thr_svc_enter（hle_thread.c:361-366）：
	          他线程改过本线程 prio（qos_pending）就在此自套用——OH_QoS 只能设自身线程 */
	       if (atomic_load_explicit(&me->qos_pending, memory_order_relaxed) && atomic_exchange(&me->qos_pending, 0)) {
	           pthread_mutex_lock(&G); qos_apply(me); pthread_mutex_unlock(&G);
	       } } }

    switch (num) {
    case 0x08: {  // CreateThread(→w1 句柄; x1 入口, x2 参数, x3 栈顶, w4 优先级, w5 核) | CreateThread(→w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core)
        if(!regs[1]||!regs[3]||(regs[3]&15)||(int)regs[4]<28||(int)regs[4]>59||(int)regs[5]<-3||(int)regs[5]>2){
            static unsigned rejected;   // 一次性保险：前 8 次拒绝打出命中的子句（SVC#8 只会在 MapAliasStack 成功后才发出） | one-shot: the first 8 rejections log which clause hit
            if(rejected++<8){
                char why[96];int w=0;
                if(!regs[1])w+=snprintf(why+w,sizeof why-w,"entry=0 ");
                if(!regs[3])w+=snprintf(why+w,sizeof why-w,"sp=0 ");
                if(regs[3]&15)w+=snprintf(why+w,sizeof why-w,"sp&15 ");
                if((int)regs[4]<28)w+=snprintf(why+w,sizeof why-w,"prio<28 ");
                if((int)regs[4]>59)w+=snprintf(why+w,sizeof why-w,"prio>59 ");
                if((int)regs[5]<-3)w+=snprintf(why+w,sizeof why-w,"core<-3 ");
                if((int)regs[5]>2)w+=snprintf(why+w,sizeof why-w,"core>2 ");
                fprintf(stderr,"[totk-stack] CreateThread rejected entry=%#llx sp=%#llx prio=%d core=%d why=%s\n",
                    (unsigned long long)regs[1],(unsigned long long)regs[3],(int)regs[4],(int)regs[5],why);
            }
            ct_note_reject(regs[1], regs[2], (int)regs[4], (int)regs[5], R_BAD_STATE);   /* round9b CreateThread 账本：拒绝路径（无 G，竞态可接受） */
            regs[0]=R_BAD_STATE;return 1;
        }
        thr_t *t = calloc(1, sizeof *t);
        if(!t)die("thread allocation");
        t->svch = calloc(256, sizeof(uint32_t));   /* M6h: per-thread SVC histogram for the [own] line (M7y: u32, u16 saturated) */
        t->cur_core = -1;
        pthread_cond_init(&t->cv, NULL);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0x3Full;
        t->prio = (int)regs[4]; t->core = (int)regs[5]; t->mask = t->core >= 0 ? 1ull << t->core : 7;
        {   /* M6t C7: record who created this thread. "0x102a exited at t=629 while main still waited for
               it" cannot be answered from a ledger that only knows the entry point, so the creator's
               handle and guest LR go into the row the state dump prints. */
            thr_t *me = me_or_null();
            t->cre_h = me ? me->handle : 0; t->cre_lr = regs[30]; }
        t->born_ns = now_ns(); t->born_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
        pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t, regs[30]); t->id = g_next_id++; t->all = g_all; g_all = t;
        t->mvs = lp_mv_slot(t->handle);   /* M6l: -1 unless this is one of the observed threads */
        ct_note_create(regs[1], regs[2], t->stack_top, t->prio, t->core, t->handle, 0);   /* round9b CreateThread 账本：成功路径（持 G） */
        pthread_mutex_unlock(&G);
        atomic_fetch_add_explicit(&g_ntlive, 1, memory_order_relaxed);   /* M6r: advance detector */
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);   // also the host-service stack of this guest thread
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   // 退出即回收宿主资源（客体经句柄等待，不 join） | Host resources are reclaimed on exit (the guest waits through the handle, no join)
        if (pthread_create(&t->pt, &a, thread_main, t)) die("pthread_create");
        pthread_attr_destroy(&a);
        LOG("CreateThread h=%#x entry=%#llx sp=%#llx", t->handle,t->entry,t->stack_top);
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {  // StartThread(w0)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->started = 1; pthread_cond_signal(&t->cv); }
        ct_note_start((uint32_t)regs[0], t ? 0 : R_BAD_HANDLE);   /* round9b StartThread 账本（持 G） */
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x0A: {  // ExitThread：SVC 派发上下文打点（无浮点 snprintf；绝不进 thread_exit_on_host 的客体小栈日志禁区——那里 printf 会 SIGBUS，见其注释）。entry+最后 LR 钉短命线程身份（0x102b nsvc=21 即退：若它是队列处理线程，早退=无生产者） | ExitThread: log in the SVC dispatch context only; entry + last LR pin the identity of short-lived threads
        thr_t *t = cur(); char where[96]; runtime_describe_address(regs[30], where, sizeof where);
        fprintf(stderr, "[totk-thr] ExitThread h=%#x nsvc=%llu entry=%#llx last_lr=%s\n", t->handle, (unsigned long long)t->nsvc, (unsigned long long)t->entry, where);
        thread_exit(t); return 1;
    }
    case 0x0B: {  // SleepThread(x0 ns)；0/-1/-2 = 各类让出 | SleepThread(x0 ns); 0/-1/-2 = the various kinds of yield
        int64_t ns = (int64_t)regs[0];

        thr_t *self = cur();
        { extern void runtime_fa_wait(uint32_t kind,uint32_t thr,uint32_t n,uint32_t h0,uint64_t req,uint64_t dur,uint64_t blk,uint64_t core,uint32_t ret,uint64_t lr);
          if (ns > 0) runtime_fa_wait(0x100u, self->handle, 0, 0, (uint64_t)ns, 0, 0, 0, 0, regs[30]); }   /* M6r: a long sleep is a delay source too */
        if (ns > 0) { pthread_mutex_lock(&G); core_put(self); pthread_mutex_unlock(&G); }
        if (ns > 0 && dm_detclock_enabled()) {
            uint64_t deadline = guest_ns() + (uint64_t)ns;
            while (guest_ns() < deadline) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
        } else if (ns > 0) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, 0); }
        else {   // 让出（0 / -1 / -2）。主线程的让出次数与耗时记账：游戏渲染任务图「等全部做完」是纯轮询 + 让出（main+0x15ee898），量它占一帧多少 | Yield (0 / -1 / -2). Count and time of the main thread's yields are recorded: the game's render task graph "wait for everything to finish" is pure polling + yield (main+0x15ee898); measure how much of a frame it takes
            thr_t *me = (g_keythr ? pthread_getspecific(g_keythr) : NULL);
            uint64_t y0 = now_ns();
            if (me && me->nsvc != me->yrun_svc + 1) me->yrun_ns = y0;   // another SVC since the last yield: a new run
            if (me) me->yrun_svc = me->nsvc;
            pthread_mutex_lock(&G);   // Horizon yield: hand the core to a waiting thread of equal or higher priority
            int give = 0; for (thr_t *w = g_core_wait; w && !give; w = w->cnext) give = me->cur_core >= 0 && core_allowed(w, me->cur_core) && w->prio <= me->prio;
            if (give) { core_put(me); core_take(me); }
            pthread_mutex_unlock(&G);
            uint64_t backoff = !give && me ? yield_backoff_ns(y0 - me->yrun_ns) : 0;
            if (backoff) {   // a long poll: sleep with the core given up, as SleepThread(ns) does
                pthread_mutex_lock(&G); core_put(me); pthread_mutex_unlock(&G);
                struct timespec ts = {0, (long)backoff}; nanosleep(&ts, 0);
                pthread_mutex_lock(&G); core_take(me); pthread_mutex_unlock(&G);
            } else if (!give) sched_yield();
            if (me && me->handle == 0x1001) { g_main_yield_ns += now_ns() - y0; g_main_yield_n++; }
        }
        if (ns > 0) { pthread_mutex_lock(&G); core_take(self); pthread_mutex_unlock(&G); }

        return 1;
    }
    case 0x0C: { pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; pthread_mutex_unlock(&G);return 1; }
    case 0x0D: { if(regs[1]<28||regs[1]>59){regs[0]=R_BAD_STATE;return 1;}pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[0], O_THREAD); if (t) { t->prio = (int)regs[1];
               /* donor hle_thread.c:894-896：SMO_QOS_MAP 下 QoS 级随 prio——设自身线程立即生效，
                  改他线程只置 qos_pending，目标线程下次 SVC 入口自套用（OH_QoS 无跨线程入口） */
               if (g_qos_map) { if (t == cur()) qos_apply(t); else atomic_store(&t->qos_pending, 1); } }
               regs[0] = t ? 0 : R_BAD_HANDLE;pthread_mutex_unlock(&G); return 1; }
    case 0x0E: { pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0;pthread_mutex_unlock(&G); return 1; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 理想核, x2 掩码)：-1 = 用进程缺省、-2/-3 = 不改理想核；宿主只记账 | SetThreadCoreMask(w0, w1 ideal core, x2 mask): -1 = use the process default, -2/-3 = leave the ideal core unchanged; the host only keeps the bookkeeping
        pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { int c = (int)regs[1]; if (c >= 0) t->core = c; else if (c == -1) t->core = 0; if (regs[2]) t->mask = regs[2];
                 if (t == cur() && t->cur_core >= 0 && !core_allowed(t, t->cur_core)) { core_put(t); core_take(t); } }   // migrate off a core no longer allowed
        regs[0] = t ? 0 : R_BAD_HANDLE;pthread_mutex_unlock(&G); return 1;
    }
    case 0x10: { thr_t *t = cur(); regs[0] = (uint64_t)(uint32_t)(t->cur_core >= 0 ? t->cur_core : t->core); return 1; }   // GetCurrentProcessorNumber
    case 0x25: { pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0;pthread_mutex_unlock(&G); return 1; }

    case 0x1A: {  // ArbitrateLock(w0 持有者句柄, x1 地址, w2 本线程标签) | ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag)
        thr_t *t = cur(); uint64_t addr = regs[1];
        int m7x_arm = atomic_load_explicit(&g_m7x_on, memory_order_relaxed) && t->handle == 0x1001u;   /* M7x: main only */
        pthread_mutex_lock(&G);
        int64_t r = 0; int m5k_blk = 0;
        int m7x_blk = ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT);
        if (m7x_arm && m7x_blk) { g_m7x_p_t0 = now_ns(); m7x_cap_win(addr); }   /* M7x: window before the wait */
        if (atomic_load_explicit(&g_m7y_on, memory_order_relaxed) && m7y_role(t->handle)) m7y_key_reg(1, addr, t->handle);   /* M7y: register the role's lock addr */
        if (m7x_blk) { t->waddr = addr; t->wtag = (uint32_t)regs[2]; m5k_blk = 1; r = block(t, 1, -1); }
        if (t->handle == 0x1001) { g_m5k_c1a++; if (m5k_blk) g_m5k_c1a_blk++; }   /* M5k: main's lock waits */
        if (m7x_arm) m7x_note(1, addr, 0, regs, m5k_blk, r);                     /* M7x */
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: pthread_mutex_lock(&G); lock_handoff(regs[0]);
               if (atomic_load_explicit(&g_m7x_on, memory_order_relaxed)) { thr_t *ub = me_or_null(); m7x_rel_lock(regs[0], ub ? ub->handle : 0, ub ? ub->last_lr : 0); }   /* M7x: who releases the tracked locks */
               if (atomic_load_explicit(&g_m7y_on, memory_order_relaxed)) { thr_t *ub = me_or_null(); m7y_rel(1, regs[0], ub ? ub->handle : 0, regs); }   /* M7y: role-key unlock tracking */
               pthread_mutex_unlock(&G); regs[0] = 0; return 1;
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 锁地址, x1 条件键, w2 标签, x3 超时) | WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout)
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        pthread_mutex_lock(&G);
        pkey_note(0, key, t->handle);   // round9 观测：条件等待 per-key 计数（持 G、零 I/O）
        lp_wait_note(t->handle, key, addr, regs[30]);   /* M6l: this key is the queue this thread consumes */
        if (g_m7u_on && t->handle == LP_H_CLIP) m7u_qpin(addr);   /* M7u: pin the clip window (lock-0x40, 200 B) */
        if (g_m7u_on) m7u_qchg_check(1);                          /* M7u: window state at svc entry */
        ps_wait_note(t->handle, key, regs[30]);         /* M6m: register the load key / the poster's own key */
        wg_wait_note(key, t->handle);                   /* M6n: first wait on a key opens its ledger slot */
        // 顺序要紧：先置条件键 = 1，再放互斥锁。SDK 的 InternalConditionVariable::Signal 在用户态先读条件键、为 0 就不发 svc 0x1D；
        // 用户态抢互斥锁也不经过 G。原先先 lock_handoff 后置键：放锁后、置键前，别的线程可在用户态抢到锁、改完条件、读到键 0 就不唤醒，
        // 本线程随后挂上去永远等不到（并行负载下资源表读写锁 main+0x320e5e0 卡死：读者计数 0、锁空闲、写者条件键 1、写者仍在等）。
        // 挂进等待链在 block 里、仍持 G：svc 0x1D 要取 G，不会落在这之前
        // The order matters: first set the condition key = 1, then release the mutex. The SDK's InternalConditionVariable::Signal first reads the condition key in user mode, and does not issue svc 0x1D if it is 0;
        // grabbing the mutex in user mode does not go through G either. It used to do lock_handoff first and set the key after: after the release and before the key was set, another thread could grab the lock in user mode, change the condition, read key 0 and not wake anyone,
        // and this thread would then link itself in and wait forever (under parallel load the resource table read-write lock main+0x320e5e0 deadlocked: reader count 0, lock free, writer condition key 1, writer still waiting).
        // Linking into the wait chain happens inside block, still holding G: svc 0x1D has to take G, so it cannot land before this
#ifdef DM_TEST_HOOKS
#define CV_TEST_HOOK() { extern void (*dm_test_cv_after_release)(void); if (dm_test_cv_after_release) dm_test_cv_after_release(); }   // 单测：紧跟放锁之后、挂链之前 | Unit test: right after releasing the lock, before linking into the wait chain
#else
#define CV_TEST_HOOK()
#endif
#ifndef DM_TEST_CV_OLD_ORDER
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        t->gsp = regs[31]; t->gfp = regs[29]; t->glr = regs[30];   /* M7i: guest stack triple at the park */
        if (atomic_load_explicit(&g_m7y_on, memory_order_relaxed) && m7y_role(t->handle)) { m7y_key_reg(2, key, t->handle); m7y_key_reg(1, addr, t->handle); }   /* M7y: register the role's cv key + lock addr */
        lock_handoff(addr); CV_TEST_HOOK();
#else   // 仅单测 t_condvar 的负对照：还原修复前「先放锁、后置键」的顺序 | Negative control for the unit test t_condvar only: restores the pre-fix order "release the lock first, set the key after"
        lock_handoff(addr); CV_TEST_HOOK();
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
#endif
        /* M5k: record main's condition wait (key/an owner-visible lock/guest LR) + wait duration */
        uint64_t m5k_t0 = 0; int m5k_is_main = t->handle == 0x1001;
        if (m5k_is_main) { m5k_t0 = now_ns(); g_m5k_key = key; g_m5k_lock = addr; g_m5k_lr1c = regs[30]; g_m5k_n1c++;
                           if (atomic_load_explicit(&g_m7w_on, memory_order_relaxed)) runtime_m7w_wait_begin(key, regs); }   /* M7w: [sp+8] caller frame for >100 ms rows */
        int m7x_arm = m5k_is_main && atomic_load_explicit(&g_m7x_on, memory_order_relaxed);   /* M7x: cv waits -> aggregates only */
        if (m7x_arm) { g_m7x_p_t0 = m5k_t0; m7x_cap_win(addr); }
        int64_t r = block(t, 2, (int64_t)regs[3]);   // 超时：不重新拿锁（用户态自己拿） | Timeout: the lock is not re-acquired (user mode takes it itself)
        if (m7x_arm) m7x_note(2, addr, key, regs, 1, r);   /* M7x */
        if (m5k_is_main) {
            unsigned i = g_m5k_ri & 3u;
            g_m5k_ring[i][0] = key; g_m5k_ring[i][1] = addr; g_m5k_ring[i][2] = (uint64_t)r;
            g_m5k_ring[i][3] = now_ns() - m5k_t0; g_m5k_ri = i + 1u;
            m7y_ms_note(key, addr, g_m5k_ring[i][3], r);   /* M7y: individual rows + per-key aggregation (duration reuses the m5k timestamp) */
            if (atomic_load_explicit(&g_m7w_on, memory_order_relaxed))   /* M7w: wait duration stats + >100 ms ring */
                runtime_m7w_wait_end((unsigned)atomic_load_explicit(&g_m7w_tsec, memory_order_relaxed), g_m5k_ring[i][3]);
        }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 条件键, w1 个数，<=0 全部) | SignalProcessWideKey(x0 condition key, w1 count, <=0 all)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        pthread_mutex_lock(&G);
        { thr_t *me = me_or_null(); pkey_note(1, key, me ? me->handle : 0);   // round9 观测：signal per-key 计数+首位调用者（sigaddr 0x35 同范式）
#if SMO_OBS_HEAVY
          early1d_note(key, me ? me->handle : 0); }   // round16b：boot 前 10s 的 0x1D 目标键全录（独立表；窗口外一次 flag 读即返回）
#else
          (void)me; }   // round20 最小扰动：early1d 账本编译关闭（pkey 轻量计数保留）
#endif
        { thr_t *ks = me_or_null(); ks_note(key, ks ? ks->handle : 0, regs[30]); }   /* M6j: 0x1D 发信号者归属（线程句柄+客体 LR），独立表 */
        /* M5k: did anyone ever signal the key main waits on? (first signaler handle + its guest LR) */
        if (g_m5k_key && key == g_m5k_key) { thr_t *s = me_or_null(); g_m5k_n1d++; if (!g_m5k_sig_h) { g_m5k_sig_h = s ? s->handle : 0; g_m5k_sig_lr = s ? s->last_lr : 0; } }
        if (atomic_load_explicit(&g_m7x_on, memory_order_relaxed)) { thr_t *s = me_or_null(); m7x_rel_cv(key, s ? s->handle : 0, s ? s->last_lr : 0); }   /* M7x: signals of tracked cv keys */
        if (atomic_load_explicit(&g_m7y_on, memory_order_relaxed)) { thr_t *s = me_or_null(); m7y_rel(2, key, s ? s->handle : 0, regs); m7y_feeder_note(s, regs); }   /* M7y: role-key signal tracking + feeder call-site match */
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);   // 按内核顺序；转为锁等待或被唤醒后不再是 kind 2
        for (unsigned i = 0; i < g_pkey_n; i++) if (g_pkey[i].key == key) { g_pkey[i].n1dk += (uint64_t)n; break; }   // round8b：per-key 实际转醒数（n1d≫n1dk 且 waiters=0 = 空打；与 g_n1d_woke 总账对照）
        for (unsigned i = 0; i < g_pkey2_n; i++) if (g_pkey2[i].key == key) { g_pkey2[i].n1dk += (uint64_t)n; break; }   // 次级表同账
        for (unsigned i = 0; i < g_pkey3_n; i++) if (g_pkey3[i].key == key) { g_pkey3[i].n1dk += (uint64_t)n; break; }   // round9b 第三表同账
        atomic_fetch_add_explicit(&g_n1d_woke, (uint64_t)n, memory_order_relaxed);   // round9 观测：实际转醒的等待者数（signal 有没有人接） | In kernel order; once turned into a lock wait or woken it is no longer kind 2
        { thr_t *sg = me_or_null(); lp_sig_note(key, sg ? sg->handle : 0, regs[30], n); }   /* M6l: who signals the role keys (loader / clipping) */
        if (g_m7u_on) { thr_t *sg = me_or_null(); m7u_sig_note(key, sg ? sg->handle : 0, regs, n); m7u_qchg_check(1); }   /* M7u: signal-side capture (frame0 = [sp+8]) + window check */
        { thr_t *sg = me_or_null(); ps_sig_note(key, sg ? sg->handle : 0, regs[30], n); }   /* M6m: signallers of the load key and of the poster key */
        { thr_t *sg = me_or_null(); wg_sig_note(key, sg ? sg->handle : 0, regs[30]);      /* M6n: whole-process key ledger */
                                   mn_sig_note(key, sg ? sg->handle : 0, regs[30]); }     /* M6n: main's 0x1D targets */
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }

    case 0x34: {  // WaitForAddress(x0 地址, w1 类型, w2 值, x3 超时)：0 小于则等、1 小于则减一再等、2 相等则等 | WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 wait if less than, 1 decrement if less than then wait, 2 wait if equal
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        pthread_mutex_lock(&G);
        sigaddr_note(0, addr, t->handle);   // round6 观测：0x34 配对计数（总数+per-addr；持 G、零 I/O）
        int64_t r = 0; uint32_t v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        /* M5m regression fix (the real cause of this round's 2.6 s SIGILL): M5k put main's ledger between
           `if (!ok)` and its `else`, so the `else` bound to the ledger's `if` -- h=0x1001 never entered
           block() and returned an uninitialised r. The guest's LightEvent wait (WaitLightEventImpl ends in
           svc 0x34 type=0 val=1 tmo=-1) then got garbage, took the SDK assert branch (a noreturn import that
           we return from normally) and fell into udf#0 @ sdk+0x324dc4. Ledger moved into the blocking arm,
           `else` re-bound to `!ok`, and r initialised (its "always assigned" premise is gone). */
        if (!ok) { r = R_BAD_STATE; if (t->handle == 0x1001) { g_m5k_c34++; g_m5k_c34_imm++;
                   if (atomic_load_explicit(&g_m7x_on, memory_order_relaxed)) m7x_note(3, addr, 0, regs, 0, r); } }   /* M7x: immediate return -> n_imm only */
        /* round48 submit-chain 取证（F-I）：任务屏障等待侧。round47 dump 证 h=0x1007 卡在本 svc
           （addr=0x6ecb5b9218，栈 main+0x95e188←main+0x6159bc 装载分发），屏障现值是定位
           「哪些子任务没完成」的唯一缺失量——阻塞时打前 3 次：want/cur/tmo 全量。 */
        else { t->waddr = addr;
            t->gsp = regs[31]; t->gfp = regs[29]; t->glr = regs[30];   /* M7i: guest stack triple at the address-wait park */
            if (atomic_load_explicit(&g_m7y_on, memory_order_relaxed) && m7y_role(t->handle)) m7y_key_reg(3, addr, t->handle);   /* M7y: register the role's addr-wait */
            int m7x_arm = t->handle == 0x1001 && atomic_load_explicit(&g_m7x_on, memory_order_relaxed);   /* M7x: main only */
            if (m7x_arm) { g_m7x_p_t0 = now_ns(); m7x_cap_win(addr); }   /* M7x: window before the wait */
            if (t->handle == 0x1001) {   /* M5m: blk arm ledger (the wait that really blocks); last34 only here */
                g_m5k_c34++; g_m5k_c34_blk++;
                g_m5k_last34 = addr; g_m5k_last34v = v; g_m5k_last34t = type; g_m5k_last34want = val;
                ldr_obj_pre(addr, type, val, (int32_t)v);   /* M6g: object contents before the wait */
            }
            /* round3 自旋期取证：首次阻塞在该屏障即挂 READ watch（地址随运行漂移，round1↔round2 实测
               -0xa00000，硬编码必脱靶；env 注入 OHOS 上 appspawn 不继承 shell 环境亦不可行——只能在
               打点处动态武装）。round3 实测 OHOS DFX 对页面 fault 判死整进程 → watch 在 __MUSL__ 下
               仅登记地址；实际取证由 SIGUSR2 自旋采样器承担（每 200ms 抓 runnable 客体线程 pc 链）。
               注意：此处运行在客体小栈上（:312 警告），只调 write(2) 级打点——再一个 fprintf 就会像
               round3 第四跑那样被 vfprintf 撑爆栈（cppcrash-11638: fprintf←svc_thread）。 */
            { static int fi_w; if (!fi_w) { fi_w = 1; runtime_watch_add((uintptr_t)addr, 4, 1); spin_sampler_start(); } }
            /* round3：fi34/fi35 及 F-H 的 fprintf 全在客体小栈 SVC 路径上（guard 页上仅剩几十字节，
               round3 多跑实证 SignalToAddr 一打即 SEGV_ACCERR@...7ffc）——OHOS 上禁打；屏障/loader
               地址已由挂 watch 的 write(2) 级日志安全留痕，线程画像由 60s dump 承担。macOS 宿主照打。 */
#ifndef __MUSL__
            {static unsigned fi34;if(fi34++<3)fprintf(stderr,"[F-I] WaitAddr block h=%#x addr=%#llx type=%d want=%d cur=%d tmo=%lld\n",t->handle,(unsigned long long)addr,type,val,(int32_t)v,(long long)(int64_t)regs[3]);}
#endif
            atomic_fetch_add_explicit(&g_n34_blk, 1, memory_order_relaxed);   // round6：真挂上等待的 0x34（与总数差=立即 BAD_STATE 返回）
            r = block(t, 3, (int64_t)regs[3]);
            if (t->handle == 0x1001) { ldr_obj_post(addr, r);   /* M6g: object contents right after the wait ends */
                                       if (m7x_arm) m7x_note(3, addr, 0, regs, 1, r); } }   /* M7x */
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 地址, w1 类型, w2 值, w3 个数)：0 唤醒、1 相等则加一并唤醒、2 相等则按等待数改值并唤醒 | SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal and wake, 2 if equal modify the value by waiter count and wake
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        /* M6k: snapshot the word before the signal; the pair (before, after) is what proves that a
           type-0 signal wakes waiters without writing the value they are waiting for. */
        uint32_t m6kwb = 0; int m6krb = dump_read_u32(addr, &m6kwb);
        pthread_mutex_lock(&G);
        { thr_t *me = me_or_null(); sigaddr_note(1, addr, me ? me->handle : 0);   // round6 观测：0x35 裸计数+调用者句柄（释放者画像）
          if (atomic_load_explicit(&g_m7x_on, memory_order_relaxed)) m7x_rel_addr(addr, me ? me->handle : 0, me ? me->last_lr : 0);   /* M7x: who signals the tracked address objects */
          if (atomic_load_explicit(&g_m7y_on, memory_order_relaxed)) m7y_rel(3, addr, me ? me->handle : 0, regs); }   /* M7y: role-key addr-signal tracking */
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                // 全部唤醒（个数 <= 0）且有等待者时减 2，不是减 1（mesosphère KAddressArbiter::SignalAndModifyByWaitingCountIfEqual，Ryujinx、yuzu 一致；nxnative d47f899 在 Engage 上查到）
                // Waking all (count <= 0) with waiters subtracts 2, not 1 (mesosphère KAddressArbiter::SignalAndModifyByWaitingCountIfEqual; Ryujinx and yuzu agree; found by nxnative d47f899 in Engage)
                if (cnt <= 0) nv = nw > 0 ? val - 2 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        int n = 0;
        if (!r)
            for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0);
        { thr_t *sg = me_or_null(); sigaddr_woke_note(addr, n, sg ? sg->handle : 0, sg ? sg->last_lr : 0); }   /* M6f: 1:1 wake accounting per waited-on address */
        /* M6k: per-event record for the one thread the task names (0x102f), including prev SVC. */
        { thr_t *sg = me_or_null();
          if (sg && sg->handle == M6K_H) {
              uint32_t m6kwa = 0; int m6kra = dump_read_u32(addr, &m6kwa);
              m6k_sig_note(addr, type, val, atomic_load_explicit(&g_m6k_prev, memory_order_relaxed),
                           sg->last_lr, m6kwb, m6kwa, n, m6krb, m6kra);
          } }
        atomic_fetch_add_explicit(&g_n35_woke, (uint64_t)n, memory_order_relaxed);   // round6：累计实际唤醒的等待者数（waiters≠0 而 woke=0=没人等它）
        if (r) atomic_fetch_add_explicit(&g_n35_bad, 1, memory_order_relaxed);   // round6：CAS 失败（R_BAD_STATE，值不符）
        /* round48 submit-chain 取证（F-I）配套：屏障释放侧。0x35 全程 0 次出现在 round47 dump 的
           last_svc 里（活线程无一个以 0x35 收尾）——若下一轮仍 0 命中即证明屏障从未被游戏释放，
           堵点在「释放者没跑到」而非本宿主 svc 语义；命中则给出 waiters/woke 对照。 */
#ifndef __MUSL__
        {static unsigned fi35;if(fi35++<3)fprintf(stderr,"[F-I] SignalToAddr addr=%#llx type=%d val=%d cnt=%d waiters=%d woke=%d r=%lld\n",(unsigned long long)addr,type,val,cnt,nw,n,(long long)r);}
#endif
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }

    case 0x45: {  // CreateEvent(→w1 写端, w2 读端) | CreateEvent(→w1 write end, w2 read end)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        { thr_t *me = g_keythr ? pthread_getspecific(g_keythr) : NULL;
          e->cre_thr = me ? me->handle : 0; e->cre_lr = regs[30]; e->born_ns = now_ns(); e->src = 0; }
        pthread_mutex_lock(&G); uint32_t hw = h_new(O_EVENT_W, e, regs[30]), hr = h_new(O_EVENT_R, e, regs[30]); pthread_mutex_unlock(&G);
        e->hw = hw; e->hr = hr;
        evpair_note(hr, hw, e->cre_thr, regs[30], 0);   /* M6h: the pair table (0x1024's write end is named here) */
        evlog(EVLOG_CREATE, hw, hr, regs[30]);   // 挂起取证：谁在哪建的事件对（0x1003/0x101f 的身世）
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent(写端) / ClearEvent(任一端) / ResetSignal(读端，未触发报错) | SignalEvent(write end) / ClearEvent(either end) / ResetSignal(read end, error if not signaled)
        evlog(num == 0x11 ? EVLOG_SIGNAL : num == 0x12 ? EVLOG_CLEAR : EVLOG_RESET, (uint32_t)regs[0], 0, regs[30]);   // 取锁前记（句柄来自入参，不依赖 G）
        pthread_mutex_lock(&G);
        // SignalEvent 只认写端；ClearEvent 两端都认；ResetSignal 只认读端
        // SignalEvent accepts only the write end; ClearEvent accepts both ends; ResetSignal accepts only the read end
        uint32_t h = (uint32_t)regs[0];
        if (num == 0x17 && h == 0x1024u && (g_p1024_n < 8 || (g_p1024_n & 63u) == 0)) {   /* M6g: identify the polled handle */
            thr_t *pth = h_peek(h, O_THREAD); event_t *per = h_peek(h, O_EVENT_R), *pew = h_peek(h, O_EVENT_W);
            g_p1024_kind = pth ? 1 : per ? 2 : pew ? 3 : 0;
            g_p1024_sig = pth ? (pth->exited ? 1 : 0) : per ? (per->signaled ? 1 : 0) : pew ? (pew->signaled ? 1 : 0) : -1;
        }
        if (num == 0x17 && h == 0x1024u) g_p1024_n++;
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
                  if (r == R_BAD_HANDLE) LOG("事件 SVC %#x 句柄无效 %#x", num, h); }
        else if (num == 0x11) {
            e->signaled = 1; wake_sync_waiters();
            if (h >= 0x1000u && (unsigned)(h - 0x1000u) < MAX_H) atomic_fetch_add_explicit(&g_syhs[h - 0x1000u], 1, memory_order_relaxed);   /* M5l：句柄族 signals 账本 */
        }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  // WaitSynchronization(→w1 下标; x1 句柄数组, w2 个数, x3 超时) | WaitSynchronization(→w1 index; x1 handle array, w2 count, x3 timeout)
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        if(n<0||n>64||(!hs&&n)){regs[0]=R_BAD_STATE;return 1;}
        uint64_t fa_t0 = now_ns();   /* M6r：只有 0x18 有「调用方给的超时」，量一下真实等了多久 */
        int64_t deadline = ns > 0 ? (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns()) + ns : 0;
        pthread_mutex_lock(&G);
        if (t->handle == 0x1023u) g_w1023_calls++;   // round6 观测（任务3）：0x1023 的 svc 0x18 调用总数
        t->nwh = n; for (int i = 0; i < n && i < 4; i++) t->wh[i] = hs[i];
        /* M5l 句柄族 waits 账本：这笔等待在等哪些句柄（≤8，去重），每个 +1。与下面的 fh/布局取证无关。 */
        for (int i = 0; i < n && i < 8; i++) {
            uint32_t hh = hs[i];
            if (hh < 0x1000u || (unsigned)(hh - 0x1000u) >= MAX_H) continue;
            int dup = 0;
            for (int j = 0; j < i; j++) if (hs[j] == hh) { dup = 1; break; }
            if (!dup) atomic_fetch_add_explicit(&g_syhw[hh - 0x1000u], 1, memory_order_relaxed);
        }
        /* F-H 取证（round48 补齐轮）：等待句柄含 0x1003（装载器 SystemEvent 读端；两轮日志已证恒为它）
           → 用等待者传入的事件对象地址（=SDK WaitSystemEvent 的句柄数组 hs）反推装载器基址
           （loader = hs - 0x1d8，round44 实测网关 x19 路线失效恒 -1），直读终局二段布局三字段
           state@+0xa0 / progress@+0xa4 / 倒数@+0x250，再快照 libgame 进度簇
           （runtime_module_base("main")+0x92e390..+0x934c98 每 0x100 取 1 个 u32，别全 dump）。
           找不到 0x1003 字面值时的退化判据：等待句柄里 type=event_r 且 refs=2 的那个（0x1003 画像）。
           一段打印限前 3 次；x19/hs 每次命中都刷新（dump_threads 消费）。持 G 打印安全（fprintf 不取 G），
           g_se 反查按锁序 G→g_se_lock（hle_bind 注释约定）；se_bind 意外命中是 round44 已见证据，继续收集。
           直读用 __builtin_memcpy（OHOS 下与 mach_vm_read_overwrite 等价，ohos_compat.h:235 先例；
           loader 对象在客体堆、进度簇在客体模块数据段，均有效映射，风险与 dump_threads :904 同级；
           该 include 在本文件 :808 之后才可见，故此处用 memcpy）。 */
        { uint32_t fh_hit = 0;
          for (int i = 0; i < n && i < 4 && !fh_hit; i++) if (t->wh[i] == 0x1003u) fh_hit = 0x1003u;
          if (!fh_hit) for (int i = 0; i < n && i < 4 && !fh_hit; i++) {   // 退化判据：event_r 且 refs=2（h_peek O(1)）
              event_t *e = h_peek(t->wh[i], O_EVENT_R);
              if (e && e->refs == 2) fh_hit = t->wh[i];
          }
          if (fh_hit) {
            g_loader_wait_x19 = regs[19]; g_loader_wait_hs = (uint64_t)(uintptr_t)hs;
            /* round3 自旋期取证（配套 F-I）：loader 态字段 state@+0xa0/progress@+0xa4 登记 watch
               （8 字节窗口盖两字段；OHOS 上 mprotect watch 禁用仅登记，macOS 宿主实挂）。 */
            { static int fh_w; uint64_t hs_a2 = (uint64_t)(uintptr_t)hs;
              uint64_t ld2 = (hs_a2 > 0x100000000ULL && hs_a2 < 0x800000000000ULL) ? hs_a2 - 0x1d8 : 0;
              if (!fh_w && ld2) { fh_w = 1; runtime_watch_add((uintptr_t)(ld2 + 0xa0), 8, 1); } }   // 地址留痕走 write(2)，栈安全
#ifndef __MUSL__
            static unsigned said; if (said < 3) { said++;
              uint64_t hs_a = (uint64_t)(uintptr_t)hs;
              int ok_hs = hs_a > 0x100000000ULL && hs_a < 0x800000000000ULL;
              uint64_t loader = ok_hs ? hs_a - 0x1d8 : 0;   // SystemEvent@loader+0x1d8 → 基址=事件对象地址-0x1d8
              void *bind = 0; {   // se_bind 反查（round44 实测 "(service-bound, unexpected)" 命中，证据保留）
                void *target = h_peek(0x1003u, O_EVENT_R);
                pthread_mutex_lock(&g_se_lock);
                for (int i = 0; i < g_nse; i++) if (target && h_peek(g_se[i].wh, O_EVENT_W) == target) { bind = g_se[i].ev; break; }
                pthread_mutex_unlock(&g_se_lock);
              }
              fprintf(stderr, "[F-H] wait h=%#x waits %#x: hs=%#llx x19=%#llx loader(hs-0x1d8)=%#llx se_bind=%s\n",
                      t->handle, fh_hit, (unsigned long long)hs_a, (unsigned long long)regs[19],
                      (unsigned long long)loader,
                      bind ? "(service-bound, unexpected)" : "none (game-created)");
              /* 三字段：主候选用 hs 反推；x19 仍有效时补一份互证（round44 前 x19 路线曾可用） */
              uint64_t cand[2]; int nc = 0;
              if (loader) cand[nc++] = loader;
              if (regs[19] > 0x100000000ULL && regs[19] < 0x800000000000ULL && (!loader || regs[19] != loader)) cand[nc++] = regs[19];
              for (int c = 0; c < nc; c++) {
                  uint32_t st, pg, cd;
                  __builtin_memcpy(&st, (const void *)(uintptr_t)(cand[c] + 0xa0), 4);
                  __builtin_memcpy(&pg, (const void *)(uintptr_t)(cand[c] + 0xa4), 4);
                  __builtin_memcpy(&cd, (const void *)(uintptr_t)(cand[c] + 0x250), 4);
                  fprintf(stderr, "[F-H] loader%s %#llx: state(+0xa0)=%u progress(+0xa4)=%u countdown(+0x250)=%u\n",
                          c ? "(x19)" : "", (unsigned long long)cand[c], st, pg, cd);
              }
              if (!nc) fprintf(stderr, "[F-H] loader base unavailable: hs/x19 both outside guard window\n");
              /* libgame 进度簇：0x92e390..0x934c98 每 0x100 一点（106 个 u32，一行收口） */
              { uint8_t *mb = runtime_module_base("main");
                if (!mb) fprintf(stderr, "[F-H] libgame module base unavailable (module table not registered)\n");
                else {
                  fprintf(stderr, "[F-H] libgame cluster base=%p step=0x100:", (void *)mb);
                  for (uint64_t off = 0x92e390; off <= 0x934c98; off += 0x100) {
                      uint32_t v; __builtin_memcpy(&v, (const void *)(uintptr_t)(mb + off), 4);
                      fprintf(stderr, " %llx=%u", (unsigned long long)off, v);
                  }
                  fputc('\n', stderr);
                }
              }
            }
#endif   /* !__MUSL__：F-H 打印簇（含 106 点循环）在客体小栈上一打即死，OHOS 禁打；loader 地址已由上方 watch 登记留痕 */
          }
        }
        int64_t r; int idx = -1;
        uint64_t cw0 = t->cw_ns;   /* M6s: core-handoff time already spent by this thread before this wait session */
        for (;;) {
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED; break; }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) { r = R_BAD_HANDLE; break; }
            if (idx >= 0) { r = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns());
            if (ns > 0 && left <= 0) { r = R_TIMEOUT; break; }
            if (block(t, 4, left) == R_TIMEOUT) { r = R_TIMEOUT; break; }
        }
        if (t->handle == 0x1023u && idx >= 0) {   // round6 观测（任务3）：0x101f wait 返回路径——0x1023 醒来计数（谁把它唤醒）
            g_w1023_wake++;
            if (idx < n && hs && hs[idx] == 0x101fu) g_w1023_wake_101f++;   // 被目标事件 0x101f 唤醒（vs 其它句柄）
        }
        pthread_mutex_unlock(&G);
        { extern void runtime_fa_wait(uint32_t kind,uint32_t thr,uint32_t n,uint32_t h0,uint64_t req,uint64_t dur,uint64_t blk,uint64_t core,uint32_t ret,uint64_t lr);
          uint64_t dur = now_ns() - fa_t0, core = t->cw_ns - cw0;
          runtime_fa_wait(0x101u, t->handle, (uint32_t)n, (n > 0 && hs) ? hs[0] : 0u,
                          ns < 0 ? ~0ull : (uint64_t)ns, dur, dur > core ? dur - core : 0, core, (uint32_t)r, regs[30]); }
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  // CancelSynchronization(w0 线程) | CancelSynchronization(w0 thread)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x16: {  // CloseHandle(w0)：事件两端都关才释放；线程句柄关闭后槽位回收，对象在线程也退出后释放 | CloseHandle(w0): an event is freed only when both ends are closed; after a thread handle is closed the slot is recycled, and the object is freed once the thread has also exited
        uint32_t evh = 0;   // 事件分支才记 evlog（"写端被谁关了"）：锁内判型，锁外打点
        pthread_mutex_lock(&G);
        unsigned i = (uint32_t)regs[0] - 0x1000;int valid=0;
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            evh = (uint32_t)regs[0];
            if (g_h[i].type == O_EVENT_W) se_unbind((uint32_t)regs[0]);   // 绑定随写端结束 | the binding ends with its write end
            valid=1;event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && (g_h[i].type == O_SESSION || g_h[i].type == O_TMEM)) {
            valid=1;void *o = g_h[i].obj; int session = g_h[i].type == O_SESSION;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            g_svc_close_lr = regs[30];   /* M6h-B: attribute the session teardown to this guest close */
            pthread_mutex_unlock(&G);
            if (session) runtime_ipc_session_closed(o); else runtime_ipc_tmem_closed(o);
            pthread_mutex_lock(&G);
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            valid=1;thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (t->exited) thr_free(t);
        }
        pthread_mutex_unlock(&G);
        if (evh) { evlog(EVLOG_CLOSE, evh, 0, regs[30]); evclose_note(evh, regs[30]); }   /* M6h: date the close */
        regs[0] = valid?0:R_BAD_HANDLE; return 1;
    }
    }
    return 0;
}

// ---------- 服务层的 SystemEvent ----------
// 真机：服务把内核事件的读端交给客户端，客体经 SDK nn::os 等待。宿主：在客体线程上调 SDK 自己的
// CreateSystemEvent(跨进程=true)，SDK 经 SVC 0x45 建出内核事件；记下写端句柄，之后任何线程（含宿主音频线程）都可触发。
// ---------- SystemEvent of the service layer ----------
// Real device: the service hands the read end of a kernel event to the client, and the guest waits on it through SDK nn::os. Host: on a guest thread, call the SDK's own
// CreateSystemEvent(inter-process=true), and the SDK creates the kernel event through SVC 0x45; the write-end handle is recorded, after which any thread (including the host audio thread) can signal it.
// 绑定表 g_se 在 svc_thread 之前：CloseHandle 经它解绑 | The binding table g_se sits before svc_thread: CloseHandle unbinds through it
void hle_bind_system_event(void *ev, int autoclear) {      // 必须在客体线程上调（要执行 SDK 代码） | Must be called on a guest thread (it has to execute SDK code)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) return;
    if (!(g_keythr ? pthread_getspecific(g_keythr) : NULL)) die("hle_bind_system_event 须在客体线程上调用");
    static uint32_t (*create)(void *, int, int); static uint32_t (*getw)(const void *);
    if (!create) { create = guest_sym("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
                   getw = guest_sym("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE"); }
    uint32_t r = create(ev, autoclear, 1);
    if (r) die("CreateSystemEvent 失败 %#x", r);
    wh = getw(ev);
    pthread_mutex_lock(&g_se_lock);
    if (g_nse >= (int)(sizeof g_se / sizeof g_se[0])) die("SystemEvent 绑定表满");
    g_se[g_nse].ev = ev; g_se[g_nse++].wh = wh;
    pthread_mutex_unlock(&g_se_lock);
}
// 未绑定的在客体线程上按手动清除即时绑定；宿主线程（显示链接的垂直同步回调等）上未绑定说明游戏已销毁了它，这次触发没有对象可发，跳过。
// 查找与触发在同一次持 G 内完成：CloseHandle（会解绑）也在 G 下，句柄不会在两者之间被关掉、编号被别的事件占去
// An unbound one is bound on the spot as manual-clear on a guest thread; on a host thread (the display link's vsync callback and the
// like) unbound means the game destroyed it, and this signal has nothing to go to: skipped. The lookup and the signal happen under one
// hold of G: CloseHandle (which unbinds) also runs under G, so the handle cannot be closed and its number taken by another event between them
static uint64_t g_se_skipped;
void hle_signal_system_event(void *ev) {
    for (int bound = 0; ; bound = 1) {
        pthread_mutex_lock(&G);
        pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
        if (wh) {
            event_t *e = h_get(wh, O_EVENT_W);
            if (!e) die("SystemEvent 写端句柄失效 %#x", wh);
            /* round13 fixPlan(3) 审计：宿主 HLE 侧对客体 SystemEvent 的 signal 此前只改
               signaled、不入 evlog/evcnt——装载器事件 0x1002「signaled=1 但 evc signal=0」
               无法区分『创建即置位』（hle_bind_system_event 的 CreateSystemEvent(ev,·,1)）
               与『被 HLE 发过』。入环对齐 runtime_event_signal（round5 同款），纯观测。 */
            evlog(EVLOG_SIGNAL, wh, 0, (uint64_t)(uintptr_t)__builtin_return_address(0));
            e->signaled = 1; wake_sync_waiters();
            pthread_mutex_unlock(&G);
            return;
        }
        int guest = (g_keythr ? pthread_getspecific(g_keythr) : NULL) != NULL;
        if (!guest && g_se_skipped++ < 8) LOG("SystemEvent %p 未绑定（游戏已销毁），宿主线程的触发跳过 | not bound (destroyed by the game): signal from a host thread skipped", ev);
        pthread_mutex_unlock(&G);
        if (!guest) return;
        if (bound) die("SystemEvent %p 绑定后仍查不到 | still not bound after binding", ev);
        hle_bind_system_event(ev, 0);   // 执行客体代码（其 SVC 要取 G）：不能持 G | runs guest code (its SVCs take G): not under G
    }
}

// ---------- IPC support (ipc.c): sessions, transfer memory and service-owned events ----------
// Weak fallbacks keep standalone thread tests linkable; the runtime links the real ipc.c versions.
__attribute__((weak)) void runtime_ipc_session_closed(void *s) { (void)s; die("IPC session closed without ipc.c"); }
__attribute__((weak)) void runtime_ipc_tmem_closed(void *t) { (void)t; die("transfer memory closed without ipc.c"); }
uint32_t runtime_handle_new(int session, void *obj) { pthread_mutex_lock(&G); uint32_t h = h_new(session ? O_SESSION : O_TMEM, obj, (uint64_t)(uintptr_t)__builtin_return_address(0)); pthread_mutex_unlock(&G); return h; }
void *runtime_handle_get(uint32_t h, int session) { pthread_mutex_lock(&G); void *o = h_peek(h, session ? O_SESSION : O_TMEM); pthread_mutex_unlock(&G); return o; }
/* Removes a session handle without closing the session (ipc.c: a session made inside a CMIF domain becomes a domain object). */
void *runtime_handle_take_session(uint32_t h) {
    pthread_mutex_lock(&G); void *o = h_peek(h, O_SESSION);
    if (o) { unsigned i = h - 0x1000; g_h[i].type = O_FREE; g_h[i].obj = NULL; }
    pthread_mutex_unlock(&G); return o;
}
// Service-side event: returns the writable handle kept by the service.
uint32_t runtime_event_new(void) {
    event_t *e = calloc(1, sizeof *e); if (!e) die("event allocation"); e->refs = 1;
    e->src = 1; e->cre_lr = (uint64_t)(uintptr_t)__builtin_return_address(0); e->born_ns = now_ns();
    pthread_mutex_lock(&G); uint32_t hw = h_new(O_EVENT_W, e, e->cre_lr); pthread_mutex_unlock(&G);
    e->hw = hw; evpair_note(0, hw, 0, e->cre_lr, 1);
    return hw;
}
// Copy-handle transfer of the readable end: each call gives the client its own handle to the same event.
uint32_t runtime_event_readable(uint32_t hw) {
    pthread_mutex_lock(&G); event_t *e = h_get(hw, O_EVENT_W); if (!e) die("service event handle %#x", hw);
    e->refs++; uint32_t hr = h_new(O_EVENT_R, e, (uint64_t)(uintptr_t)__builtin_return_address(0)); pthread_mutex_unlock(&G);
    e->hr = hr;
    evpair_note(hr, hw, 0, (uint64_t)(uintptr_t)__builtin_return_address(0), 2);   /* M6h: which service event each client read end is */
    return hr;
}
void runtime_event_signal(uint32_t hw) {
    pthread_mutex_lock(&G); event_t *e = h_get(hw, O_EVENT_W); if (!e) die("service event handle %#x", hw);
    /* evlog 可观测性（round5）：宿主服务侧的 signal 一直只在 SVC 0x11 路径入环，audio renderer 每帧
       （5ms）经此处无条件触发（audio_renderer.c render_thread），环里却看不到——被误读成「evlog 无 signal
       = 无信号源」。此处入环后 dump 环常驻 signal 条目（renderer 写端 0x1021 与游戏等的读端同一 event_t、
       refs=2，可互证信号源活着）。evlog（round8 起）只存裸 LR+原子计数，无锁无 I/O，持 G 调用安全。 */
    evlog(EVLOG_SIGNAL, hw, 0, (uint64_t)(uintptr_t)__builtin_return_address(0));
    e->signaled = 1; wake_sync_waiters(); pthread_mutex_unlock(&G);
}
/* round8：宿主服务侧写端释放（IAudioDevice 会话 free 钩子用）——镜像 SVC 0x16 事件分支：解绑、
   减一引用（游戏侧每份读端拷贝各持一引用）、回收句柄槽。事件对象在读端也全关后才真正释放。 */
void runtime_event_close(uint32_t hw) {
    uint32_t evh = 0;
    pthread_mutex_lock(&G);
    unsigned i = hw - 0x1000;
    if (i > 0 && i < MAX_H && g_h[i].type == O_EVENT_W) {
        evh = hw;
        se_unbind(hw);
        event_t *e = g_h[i].obj;
        if (--e->refs == 0) free(e);
        g_h[i].type = O_FREE; g_h[i].obj = NULL;
    }
    pthread_mutex_unlock(&G);
    if (evh) { evlog(EVLOG_CLOSE, evh, 0, (uint64_t)(uintptr_t)__builtin_return_address(0));
               evclose_note(evh, (uint64_t)(uintptr_t)__builtin_return_address(0)); }
}

// ---------- Diagnostics: TOTK_DUMP_AFTER=<seconds> prints every guest thread's wait state and guest return chain ----------
#include "mach_vm_compat.h"
#include <unistd.h>
__attribute__((weak)) void runtime_describe_address(uint64_t a, char *out, size_t n) { snprintf(out, n, "%#llx", (unsigned long long)a); }   // loader.c names modules
__attribute__((weak)) uint64_t runtime_account_lock_owner(void) { return 0; }   // account.c is not linked into the thread probe
__attribute__((weak)) void runtime_hle_describe(unsigned i, char *out, size_t n) { snprintf(out, n, "wrapper %u", i); }   // hle_stack.c names it
/* round49：dump 里读客体地址必须走安全读。spin_read16 在 __MUSL__ 下是 process_vm_readv
   （PROT_NONE/未映射返 EFAULT 而不 fault，理由见 :1805 那段），定义在文件后部，这里先声明。 */
static int spin_read16(uint64_t a, uint64_t out[2]);
/* 4 字节客体读（线程行里的 wkind word）。取所在 16B 行再截取；跨行（off>12）按「读不到」处理，
   不为了 4 字节再多读一行。失败返回 0 且 *out=0——原来的裸 memcpy 在保护页上会直接 SIGSEGV。 */
static int dump_read_u32(uint64_t a, uint32_t *out) {
    uint64_t base = a & ~0xfULL, line[2];
    unsigned off = (unsigned)(a - base);
    *out = 0;
    if (off > 12) return 0;
    if (!spin_read16(base, line)) return 0;
    memcpy(out, (const uint8_t *)line + off, 4);
    return 1;
}
/* 8 字节客体读（F-H 段 state/progress@+0xa0），同上：16B 行内截取，跨行按读不到处理。 */
static int dump_read_u64(uint64_t a, uint64_t *out) {
    uint64_t base = a & ~0xfULL, line[2];
    unsigned off = (unsigned)(a - base);
    *out = 0;
    if (off > 8) return 0;
    if (!spin_read16(base, line)) return 0;
    memcpy(out, (const uint8_t *)line + off, 8);
    return 1;
}
static void dump_chain(uint64_t lr, uint64_t fp) {
    char where[96]; runtime_describe_address(lr, where, sizeof where); fprintf(stderr, "    %s", where);
    for (int i = 0; i < 10 && fp && !(fp & 15); i++) {
        uint64_t f[2];
        /* round49 崩溃修复（m4n_b1 第三次 hang dump，cppcrash-…150125775）：原来走
           mach_vm_read_overwrite —— 本镜像上它是**裸 memcpy**（:1784 注释同款），fp 落进被 mprotect
           成 PROT_NONE 的页就 SEGV_ACCERR 即死。证据：:20 FA=0x6e07dc4680、:23 memcpy+40（x2=0x10
           拷 16B）、:24-27 dump_chain+0xd4 ← mach_vm_read_overwrite+0x2c ← dump_threads+0x6d4
           ← runtime_dump_threads+12。改用 spin_read16：16B、16 对齐（本循环已要求 !(fp & 15)），
           读不到就 break（与原语义一致）。 */
        if (!spin_read16(fp, f)) break;
        runtime_describe_address(f[1], where, sizeof where); fprintf(stderr, " <- %s", where);
        if (f[0] == fp) break; fp = f[0];   // frames may move between the guest and host stacks
    }
    fputc('\n', stderr);
}
static void dump_threads(unsigned secs);
static int spin_read16(uint64_t a, uint64_t out[2]);   /* round9b：dump 的 E1/E4 段用（定义在诊断段） */
static uint64_t r22_spin_pc(void), r22_spin_x19(void);   /* round22：SIGUSR2 采样器末次成功样本（定义在诊断段 g_spin_cap 后） */
static uint64_t r22_walk_pc(void), r22_walk_x19(void); static unsigned r22_walk_set(void);   /* round22b：walker 窗首个成功样本锁存 */
/* round22 donor 黄金清单对拍（logs/donor_golden.txt）：8 字节粒度安全读——spin_read16 要求
   16 对齐，guest 对象字段多落 8 对齐半字（outer=…c8、node=…a8），取所在 16B 行再选半字。 */
static int r22_read64(uint64_t a, uint64_t *out) {
    uint64_t w[2];
    if (a < 0x100000000ULL || a >= 0x800000000000ULL || (a & 7)) return 0;
    if (!spin_read16(a & ~0xfULL, w)) return 0;
    *out = (a & 8) ? w[1] : w[0];
    return 1;
}
/* round22：mq/node 候选黄金三字（donor 黄金：链头 [+0xc8]=0x7fffffff、[+0xd4]=1、[+0x108]=0；
   [+0xd4] 取 [+0xd0] 的 u64 高 32 位）。失败字打 0 并尾缀 ?。 */
static void r22_node_fields(uint64_t base, const char *tag) {
    uint64_t c8 = 0, d0 = 0, e8 = 0;
    int gc = r22_read64(base + 0xc8, &c8), gd = r22_read64(base + 0xd0, &d0), ge = r22_read64(base + 0x100, &e8);
    fprintf(stderr, "[totk-dump] r22 %s %#llx: [+0xc8]=%#llx%s [+0xd4]=%#llx%s [+0x108]=%#llx%s\n",
            tag, (unsigned long long)base,
            (unsigned long long)(gc ? c8 : 0), gc ? "" : "?",
            (unsigned long long)(gd ? d0 >> 32 : 0), gd ? "" : "?",
            (unsigned long long)(ge ? e8 : 0), ge ? "" : "?");
}
static void *dump_after(void *arg) { unsigned secs = (unsigned)(uintptr_t)arg; sleep(secs); dump_threads(secs); return NULL; }
/* TOTK_LISTCHECK=<heap-relative object offset>: watch the two intrusive lists at +0x48/+0x60 of one game allocator
   object (seen corrupted after the startup logo) and dump all threads the moment a node's next link is null. */
uint64_t runtime_heap_base(void) __attribute__((weak));
uint64_t runtime_heap_base(void) { return 0; }
static void *list_check(void *arg) {
    uint64_t off = (uint64_t)(uintptr_t)arg;
    for (;;) {
        usleep(200);
        uint64_t base = runtime_heap_base(); if (!base) continue;
        const uint64_t obj = base + off;
        for (int l = 0; l < 2; l++) {
            uint64_t sentinel = obj + (l ? 0x60 : 0x48), head[2]; mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(mach_task_self(), sentinel, 16, (mach_vm_address_t)(uintptr_t)head, &got) != KERN_SUCCESS) continue;
            uint64_t node = head[1];
            for (int n = 0; n < 100000 && node && node != sentinel; n++) {
                uint64_t link[2];
                if (mach_vm_read_overwrite(mach_task_self(), node, 16, (mach_vm_address_t)(uintptr_t)link, &got) != KERN_SUCCESS) break;
                if (!link[1]) {
                    usleep(50); uint64_t again[2];
                    if (mach_vm_read_overwrite(mach_task_self(), node, 16, (mach_vm_address_t)(uintptr_t)again, &got) == KERN_SUCCESS && !again[1]) {
                        fprintf(stderr, "[totk-listcheck] list +%#x node %#llx has null next (prev %#llx)\n", l ? 0x60 : 0x48, (unsigned long long)node, (unsigned long long)again[0]);
                        dump_threads(0); return NULL;
                    }
                    break;
                }
                node = link[1];
            }
        }
    }
}
static void dump_threads(unsigned secs) {
    int locked = 0; for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(10000);
    fprintf(stderr, "[totk-dump] global thread lock %s\n", locked ? "acquired" : "HELD BY ANOTHER THREAD (dumping without it)");
    fprintf(stderr, "[totk-dump] guest threads after %u s\n", secs);
    {   /* round9 观测：模块基址头行（每次 dump 带头）。宿主 libgame.so（dladdr 自身函数取装载基址——
           OHOS linker 完成 relocate 后 dladdr 即可查）+ 客体三模块（宿主 loader relocate 完成时另打
           [loader-base] 一份）。evlog/evc 的 who=0x7f81… 裸 LR（宿主侧 __builtin_return_address，
           runtime_describe_address 不落客体模块时打原值）用本行基址减去即得 offset，离线拿 obj 副本
           （带 debug_info）符号化。 */
        Dl_info di;
        uint64_t host_base = dladdr((void *)(uintptr_t)&dump_threads, &di) ? (uint64_t)(uintptr_t)di.dli_fbase : 0;
        uint8_t *bmain = runtime_module_base("main"), *bsdk = runtime_module_base("sdk"), *bsub = runtime_module_base("subsdk0");
        fprintf(stderr, "[totk-dump] bases: host-libgame=%#llx main=%#llx sdk=%#llx subsdk0=%#llx\n",
                (unsigned long long)host_base, (unsigned long long)(uintptr_t)bmain,
                (unsigned long long)(uintptr_t)bsdk, (unsigned long long)(uintptr_t)bsub);
    }
    { fprintf(stderr, "[totk-dump] account lock owner host tid %llu\n", (unsigned long long)runtime_account_lock_owner()); }
    for (int c = 0; c < NCORE; c++) fprintf(stderr, "[totk-dump] core %d owner %#x for %.1f ms\n", c, g_core_owner[c] ? g_core_owner[c]->handle : 0,
                                            g_core_owner[c] ? (now_ns() - g_core_owner[c]->core_since) / 1e6 : 0.0);
    for (thr_t *w = g_core_wait; w; w = w->cnext) fprintf(stderr, "[totk-dump] core waiter %#x prio %d mask %#llx ideal %d\n", w->handle, w->prio, (unsigned long long)w->mask, w->core);
    thr_t *run[64]; int nrun = 0;
    for (thr_t *t = g_all; t; t = t->all) {
        /* round48 F-I：wkind=3 时 word=任务屏障计数现值（0x1007 画像）。
           round49：改安全读——裸 memcpy 在 PROT_NONE 页（DM 写观察点/栈 guard）会 SIGSEGV_ACCERR，
           与 dump_chain 同一类死法；读不到时 word 保持 0。 */
        uint32_t word = 0; if ((t->wkind == 1 || t->wkind == 3) && t->waddr) dump_read_u32(t->waddr, &word);
        uint64_t htid = 0; if (t->pt) pthread_threadid_np(t->pt, &htid);
        fprintf(stderr, "[totk-dump] tid=%llu ", (unsigned long long)htid);
        fprintf(stderr, "core=%d mask=%#llx ", t->cur_core, (unsigned long long)t->mask);
        fprintf(stderr, "h=%#x prio=%d %s wait=%d addr=%#llx key=%#llx tag=%#x word=%#x last_svc=%#x nsvc=%llu entry=%#llx arg=%#llx\n", t->handle, t->prio,
                t->exited ? "exited" : t->started ? "run" : "new", t->wkind, (unsigned long long)t->waddr, (unsigned long long)t->wkey, t->wtag, word, t->last_svc, (unsigned long long)t->nsvc,
                (unsigned long long)t->entry, (unsigned long long)t->arg);   /* round9b：entry/arg 进名册行——与 CreateThread 账本 diff 找缺席线程 */
        if (t->wkind == 4) { fprintf(stderr, "    waiting on %d handle(s):", t->nwh); for (int i = 0; i < t->nwh && i < 4; i++) fprintf(stderr, " %#x", t->wh[i]); fputc('\n', stderr); }
        if (t->last_hle) { char nm[256]; runtime_hle_describe((unsigned)t->last_hle - 1, nm, sizeof nm); fprintf(stderr, "    last host call while tracing: %s\n", nm); }
        dump_chain(t->last_lr, t->last_fp);
        if (!t->wkind && !t->exited && t->mport && nrun < 64) run[nrun++] = t;
    }
    // 挂起取证（crash_1031 挂起轮 §E.1.a）：句柄表全景——0x1003/0x101f 的类型（事件读/写端）、
    // signaled、refs、等待者数、创建者（登记时的客体 LR 符号）一目了然。客体互斥/条件变量是
    // 地址型对象、无句柄表：以等待者按 wkind 的汇总行代之（逐线程 addr/key 见上方线程段）。
    for (int i = 1; i < MAX_H; i++) {
        if (!g_h[i].type) continue;
        int waiters = 0;
        for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 4) {
            int hit = w->nwh > 4;   // 只存前 4 个句柄：超过 4 个的等待按"可能等它"计
            for (int k = 0; !hit && k < w->nwh && k < 4; k++) hit = w->wh[k] == (uint32_t)(0x1000 + i);
            if (hit) waiters++;
        }
        char where[96]; runtime_describe_address(g_h[i].creator, where, sizeof where);
        const char *ty = g_h[i].type == O_THREAD ? "thread" : g_h[i].type == O_EVENT_R ? "event_r"
                       : g_h[i].type == O_EVENT_W ? "event_w" : g_h[i].type == O_SESSION ? "session" : "tmem";
        if (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)
            fprintf(stderr, "[totk-dump] h=%#x type=%s signaled=%d refs=%d waiters=%d creator=%s\n",
                    0x1000 + i, ty, ((event_t *)g_h[i].obj)->signaled, ((event_t *)g_h[i].obj)->refs, waiters, where);
        else if (g_h[i].type == O_THREAD)
            fprintf(stderr, "[totk-dump] h=%#x type=%s exited=%d waiters=%d creator=%s\n",
                    0x1000 + i, ty, ((thr_t *)g_h[i].obj)->exited, waiters, where);
        else fprintf(stderr, "[totk-dump] h=%#x type=%s waiters=%d creator=%s\n", 0x1000 + i, ty, waiters, where);
    }
    { int wl = 0, wc = 0, wa = 0, ws = 0;
      for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 1) wl++; else if (w->wkind == 2) wc++; else if (w->wkind == 3) wa++; else if (w->wkind == 4) ws++;
      fprintf(stderr, "[totk-dump] waiters by kind: lock=%d cond=%d addr=%d sync=%d (guest mutex/condvar are address-based; per-thread addr/key above)\n", wl, wc, wa, ws); }
    /* F-H 取证：svc 0x18 记录的装载器对象 x19 → 直读终局二段布局的 state@+0xa0 / progress-out@+0xa4
       现值（一次 8 字节读覆盖相邻两 u32）；地址护栏不过或页读失败即一行说明后跳过，不拖垮 dump。 */
    if (g_loader_wait_x19 > 0x100000000ULL && g_loader_wait_x19 < 0x800000000000ULL) {
        uint64_t sp = 0;
        /* round49：原来这里也是 mach_vm_read_overwrite（本镜像=裸 memcpy），保护页上同样会打死进程。
           与 dump_chain 同款死法，一并改安全读；走 else 那行（unreadable）语义不变。 */
        if (dump_read_u64(g_loader_wait_x19 + 0xa0, &sp))
            fprintf(stderr, "[totk-dump] loader obj %#llx (x19@svc0x18, ev slot %#llx): state=%u progress=%u\n",
                    (unsigned long long)g_loader_wait_x19, (unsigned long long)(g_loader_wait_x19 + 0x1d8),
                    (unsigned)(sp & 0xffffffffu), (unsigned)(sp >> 32));
        else fprintf(stderr, "[totk-dump] loader obj %#llx: state/progress unreadable (page read failed)\n", (unsigned long long)g_loader_wait_x19);
    }
    if (locked) pthread_mutex_unlock(&G);
    for (int i = 0; i < nrun; i++) { thr_t *t = run[i]; fprintf(stderr, "[totk-dump] live h=%#x\n", t->handle);
#ifndef __MUSL__  /* OHOS: 无 mach 线程端口取寄存器，live dump 跳过 */
            for (int k = 0; k < 3; k++) {
                arm_thread_state64_t st; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
                thread_suspend(t->mport);
                kern_return_t kr = thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt);
                thread_resume(t->mport);
                if (kr != KERN_SUCCESS) break;
                char a[96], b[96]; runtime_describe_address(arm_thread_state64_get_pc(st), a, sizeof a); runtime_describe_address(arm_thread_state64_get_lr(st), b, sizeof b);
                fprintf(stderr, "    live pc=%s lr=%s x0=%#llx x1=%#llx x8=%#llx\n", a, b, (unsigned long long)st.__x[0], (unsigned long long)st.__x[1], (unsigned long long)st.__x[8]);
                dump_chain(arm_thread_state64_get_lr(st), arm_thread_state64_get_fp(st));
                usleep(20000);
            }
#endif
    }
    // 事件 SVC 环形日志尾部 16 条（挂起取证：谁建的 0x1003/0x101f、谁 signal/clear/reset 过、写端被谁关了）
    { unsigned from = g_evlog_n > 16 ? g_evlog_n - 16 : 0;
      fprintf(stderr, "[totk-dump] event svc ring, last %u of %u:\n", g_evlog_n - from, g_evlog_n);
      for (unsigned k = from; k < g_evlog_n; k++) {
        evlog_t *e = &g_evlog[k & 63];
        const char *opn = e->op <= EVLOG_CLOSE ? evlog_names[e->op] : "?";
        char who[64]; runtime_describe_address(e->lr, who, sizeof who);   // round8: 环里存裸 LR，dump 时符号化（热路径零 describe）
        fprintf(stderr, "[totk-dump] ev#%u %s h=%#x", k, opn, e->handle);
        if (e->extra) fprintf(stderr, " h2=%#x", e->extra);
        fprintf(stderr, " who=%s\n", who);
      }
    }
    /* round6 观测（任务2）+ round8 归因：事件账本 per-handle signal/reset 累计表（自启动起全量）。
       上方环回放被 0x1026 洪流淹没时这里给出全量分布；round8 起每句柄补 signal/reset 首见 LR 的符号
       （who=）：0x1024 的 signal 源（宿主 IAudioDevice 构造侧）与 0x1026 的 reset 源（sdk+0xd3f084，
       SDK svc 包装层 LR，其上真实调用循环=main+0x8b8xxx 音频线程）在每次分钟 dump 单列可见。
       signal 列含宿主服务侧（runtime_event_signal）。选择在本地副本上做，全局计数不清零。 */
    { struct { uint32_t h; uint64_t s, r, slr, rlr; } top[256]; unsigned nt = 0;
      uint64_t tsig = 0, trst = 0;
      for (unsigned i = 1; i < MAX_H && nt < 256; i++) {
          uint64_t s = atomic_load(&g_evcnt[i][0]), rv = atomic_load(&g_evcnt[i][1]);
          if (s || rv) { tsig += s; trst += rv; top[nt].h = 0x1000 + i; top[nt].s = s; top[nt].r = rv;
                         top[nt].slr = atomic_load(&g_evfirst[i][0]); top[nt].rlr = atomic_load(&g_evfirst[i][1]); nt++; }
      }
      fprintf(stderr, "[totk-dump] event svc per-handle ledger (cumulative): %u handle(s) signal=%llu reset=%llu%s:\n",
              nt, (unsigned long long)tsig, (unsigned long long)trst, nt == 256 ? " (capped at 256)" : "");
      for (int k = 0; k < 8; k++) {
          int best = -1; uint64_t bv = 0;
          for (unsigned i = 0; i < nt; i++) { uint64_t v = top[i].s + top[i].r; if (v > bv) { bv = v; best = (int)i; } }
          if (best < 0) break;
          char ws[96], wr[96];
          if (top[best].slr) runtime_describe_address(top[best].slr, ws, sizeof ws); else ws[0] = 0;
          if (top[best].rlr) runtime_describe_address(top[best].rlr, wr, sizeof wr); else wr[0] = 0;
          fprintf(stderr, "[totk-dump] evc h=%#x signal=%llu%s%s reset=%llu%s%s\n", top[best].h,
                  (unsigned long long)top[best].s, top[best].slr ? " who=" : "", ws,
                  (unsigned long long)top[best].r, top[best].rlr ? " who=" : "", wr);
          top[best].s = top[best].r = 0;
      }
    }
    /* round6 观测（任务1）：svc 0x34/0x35 裸计数——总数/真挂起/唤醒数/CAS 失败 + per-addr 前几名
       （n35 降序、n34 并列）+ 0x35 调用者句柄。round47「0x35 零命中」之后，此表直接回答：
       屏障有没有被释放过、谁在释放、0x34 的等待有没有配对的 0x35。表在 G 下更新、此处无锁读
       （与上方线程段/环回放同级取证风险）。 */
    { fprintf(stderr, "[totk-dump] svc 0x34 WaitForAddress: total=%llu blocked=%llu\n",
              (unsigned long long)atomic_load(&g_n34), (unsigned long long)atomic_load(&g_n34_blk));
      fprintf(stderr, "[totk-dump] svc 0x35 SignalToAddress: total=%llu woke=%llu bad-state=%llu\n",
              (unsigned long long)atomic_load(&g_n35), (unsigned long long)atomic_load(&g_n35_woke),
              (unsigned long long)atomic_load(&g_n35_bad));
      unsigned done = 0;
      for (int k = 0; k < 5; k++) {
          int best = -1; uint64_t b35 = 0, b34 = 0;
          for (unsigned i = 0; i < g_sigaddr_n && i < 32; i++) {
              if ((done >> i) & 1u) continue;
              uint64_t s35 = g_sigaddr[i].n35, s34 = g_sigaddr[i].n34;
              if (s35 > b35 || (s35 == b35 && s34 > b34)) { b35 = s35; b34 = s34; best = (int)i; }
          }
          if (best < 0 || (!b35 && !b34)) break;
          done |= 1u << best;
          fprintf(stderr, "[totk-dump] sigaddr %#llx wait(0x34)=%llu sig(0x35)=%llu caller=h%#x\n",
                  (unsigned long long)g_sigaddr[best].addr, (unsigned long long)g_sigaddr[best].n34,
                  (unsigned long long)g_sigaddr[best].n35, g_sigaddr[best].caller);
      }
      if (g_ovf34 || g_ovf35)
          fprintf(stderr, "[totk-dump] sigaddr overflow (beyond %d addrs): wait=%llu sig=%llu\n",
                  SIGADDR_TOP, (unsigned long long)g_ovf34, (unsigned long long)g_ovf35);
    }
    /* round9 观测（条件变量 SVC）：svc 0x1C/0x1D per-key 表（sigaddr 范式）——总数/唤醒数 +
       前 12 名 key 的 wait(0x1C)/sig(0x1D)/woke(per-key)/当前等待者数（现场数 g_wait 的 wkind==2）/
       首位 signal 调用者句柄 + 溢出桶。等待无配对 signal（唤醒丢失嫌疑）/signal 无人接在此一表可读。
       round8b 起 32 槽 + per-key woke（n1d≫n1dk=空打一列可读），覆盖 round9b 观测到的全部 ~24 个 distinct key。 */
    { fprintf(stderr, "[totk-dump] svc 0x1C WaitProcessWideKey: total=%llu\n",
              (unsigned long long)atomic_load(&g_n1c));
      fprintf(stderr, "[totk-dump] svc 0x1D SignalProcessWideKey: total=%llu woken=%llu\n",
              (unsigned long long)atomic_load(&g_n1d), (unsigned long long)atomic_load(&g_n1d_woke));
      unsigned long long done = 0;
      for (int k = 0; k < 16; k++) {
          int best = -1; uint64_t bd = 0, bc = 0;
          for (unsigned i = 0; i < g_pkey_n && i < PKEY_TOP; i++) {
              if ((done >> i) & 1u) continue;
              uint64_t sd = g_pkey[i].n1d, sc = g_pkey[i].n1c;
              if (sd > bd || (sd == bd && sc > bc)) { bd = sd; bc = sc; best = (int)i; }
          }
          if (best < 0 || (!bd && !bc)) break;
          done |= 1u << best;
          int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == g_pkey[best].key) nw++;
          fprintf(stderr, "[totk-dump] pkey %#llx wait(0x1C)=%llu sig(0x1D)=%llu woke=%llu waiters=%d sig-caller=h%#x\n",
                  (unsigned long long)g_pkey[best].key, (unsigned long long)g_pkey[best].n1c,
                  (unsigned long long)g_pkey[best].n1d, (unsigned long long)g_pkey[best].n1dk, nw, g_pkey[best].caller);
      }
      if (g_ovf1c || g_ovf1d)
          fprintf(stderr, "[totk-dump] pkey overflow (beyond %d+16 keys): wait=%llu sig=%llu\n",
                  PKEY_TOP, (unsigned long long)g_ovf1c, (unsigned long long)g_ovf1d);
      /* round8d：次级表——主表满后（=活锁期）首见的 key。round8c 的 7.3M 空打主力在这里现出 key 值与调用者。 */
      for (unsigned i = 0; i < g_pkey2_n; i++) {
          int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == g_pkey2[i].key) nw++;
          fprintf(stderr, "[totk-dump] pkey2 %#llx wait(0x1C)=%llu sig(0x1D)=%llu woke=%llu waiters=%d sig-caller=h%#x\n",
                  (unsigned long long)g_pkey2[i].key, (unsigned long long)g_pkey2[i].n1c,
                  (unsigned long long)g_pkey2[i].n1d, (unsigned long long)g_pkey2[i].n1dk, nw, g_pkey2[i].caller);
      }
      /* round9b 第三表：pkey2 也满后首见的 key（族大小确认）。 */
      for (unsigned i = 0; i < g_pkey3_n; i++) {
          int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == g_pkey3[i].key) nw++;
          fprintf(stderr, "[totk-dump] pkey3 %#llx wait(0x1C)=%llu sig(0x1D)=%llu woke=%llu waiters=%d sig-caller=h%#x\n",
                  (unsigned long long)g_pkey3[i].key, (unsigned long long)g_pkey3[i].n1c,
                  (unsigned long long)g_pkey3[i].n1d, (unsigned long long)g_pkey3[i].n1dk, nw, g_pkey3[i].caller);
      }
      /* round16b 早 boot 键全录段：boot 后前 10 秒的 0x1D 目标键专用全量表（独立于 pkey 三表，
         不受其槽位挤压）。n=窗口内该 key 的 signal 次数（boot 一次性 key 预期 n=1），
         first-caller=窗口内首位发信号线程——与 pkey 主表同 key 行对照即知哪些活锁期 key
         是 boot 期键的延续。
         round20 最小扰动构建：探针编译关闭，dump 只打 off 标记（日志可证本轮确实没跑它）。 */
#if SMO_OBS_HEAVY
      fprintf(stderr, "[totk-dump] early-boot 0x1D keys (first 10s, dedicated full table): %u key(s)%s\n",
              g_early1d_n, g_early1d_ovf ? ", OVERFLOW beyond 256" : "");
      for (unsigned i = 0; i < g_early1d_n; i++)
          fprintf(stderr, "[totk-dump] early1d[%u] key=%#llx n=%llu first-caller=h%#x\n",
                  i, (unsigned long long)g_early1d[i].key, (unsigned long long)g_early1d[i].n, g_early1d[i].caller);
      if (g_early1d_ovf)
          fprintf(stderr, "[totk-dump] early1d overflow: %llu signal(s) on keys beyond slot 256\n",
                  (unsigned long long)g_early1d_ovf);
#else
      fprintf(stderr, "[totk-dump] early1d: probe OFF this build (round20 minimal-perturbation; SMO_OBS_HEAVY=0)\n");
#endif
    }
    /* ---------- round9b CreateThread/StartThread 全量账本（E2 宿主侧排查项）----------
       与上方线程名册（每行已带 entry=… arg=…）diff：账本里没有的 entry=从未创建；
       有 entry 无 started=建了没 Start；rejects>0=创建失败（末次样本随行）。 */
    fprintf(stderr, "[totk-dump] CreateThread ledger: calls=%llu entries=%u rejects=%llu (StartThread calls=%llu bad=%llu)\n",
            (unsigned long long)g_ct_calls, g_ct_n, (unsigned long long)g_ct_rejects,
            (unsigned long long)g_ct_start_calls, (unsigned long long)g_ct_start_bad);
    for (unsigned i = 0; i < g_ct_n; i++)
        fprintf(stderr, "[totk-dump] ct[%u] h=%#x entry=%#llx arg=%#llx sp=%#llx prio=%d core=%d started=%d ret=%#x\n",
                i, g_ct[i].handle, (unsigned long long)g_ct[i].entry, (unsigned long long)g_ct[i].arg,
                (unsigned long long)g_ct[i].stack_top, g_ct[i].prio, g_ct[i].core, g_ct[i].started, g_ct[i].ret);
    if (g_ct_rejects)
        fprintf(stderr, "[totk-dump] ct last reject entry=%#llx arg=%#llx prio=%d core=%d ret=%#x\n",
                (unsigned long long)g_ct_last_reject.entry, (unsigned long long)g_ct_last_reject.arg,
                g_ct_last_reject.prio, g_ct_last_reject.core, g_ct_last_reject.ret);
    /* ---------- round9b 观测门 dump（round21 集合收敛：E1/E2a/E2b/CELL2/DRAIN_ENTER/
       DRAIN_LOOP/KICKVT/KICKBR/MQ_RET/MQ_ADV 十门退役；E4 两族合流检验保留——等侧
       wkind==2 账本与 pkey 表仍打印，E1 绑定侧随门退役恒 0）。 ---------- */
    {
      extern uint64_t smo_obs_hits(int);
      extern uint64_t smo_obs_last_x0(int);
      /* round13 fixPlan(2)：kick 断链指令级计数（donor 0x8ad1f0 族预期 ≈920+410+舞台数，与 wait#921/920/410 对账）。
         kick1=派发函数全部入口（含 al 尾调）；al-kick=al 层调用（0x587ea8 单字门，仅过 NULL 检查的真实调用）；
         kick3=三参变体合流点 0x8ad318（单字门）。kick1≈0 ⇒ kick 从未运行（断链在注册上游，走 fixPlan(4)）。 */
      fprintf(stderr, "[totk-dump] obs kick1 0x8ad1f4 hits=%llu last_x0=%#llx\n",
              (unsigned long long)smo_obs_hits(4), (unsigned long long)smo_obs_last_x0(4));
      fprintf(stderr, "[totk-dump] obs al-kick 0x587ea8 hits=%llu last_x0=%#llx\n",
              (unsigned long long)smo_obs_hits(5), (unsigned long long)smo_obs_last_x0(5));
      fprintf(stderr, "[totk-dump] obs kick3 0x8ad318 hits=%llu last_x0=%#llx\n",
              (unsigned long long)smo_obs_hits(6), (unsigned long long)smo_obs_last_x0(6));
      /* round21 注册调用者两门（FN_A=0x8b65e0 含 0x8b6688/0x8b6708 两循环、FN_B=0x8b6920
         含 0x8b6990——三条 bl 实属两函数，反汇编实证见 game_patches.c round21 注）：与
         REG230 hits 对账=「进调用者函数的次数」vs「注册动作次数」（FN_A 两循环每节点一次
         REG230+0x8ae1e0；FN_B 每桶每节点一对）。首见见下方 fs[] 段。 */
      fprintf(stderr, "[totk-dump] obs fn_a 0x8b65e0 (bl 0x8b6688+0x8b6708) hits=%llu last_x0=%#llx\n",
              (unsigned long long)smo_obs_hits(18), (unsigned long long)smo_obs_last_x0(18));
      fprintf(stderr, "[totk-dump] obs fn_b 0x8b6920 (bl 0x8b6990) hits=%llu last_x0=%#llx\n",
              (unsigned long long)smo_obs_hits(19), (unsigned long long)smo_obs_last_x0(19));
      /* round17 ①：KICK1 三条件快照（DRAIN-LOOP 门随排空函数 0 次进入而永不命中，改挂
         kick 真入口 0x8ad1f4——this=x0、w1=原 x1）：三分支交叉分布直接回答「216 次真 kick
         为何派不到 worker」（w1<1 与 w1>=1∧f9==2 走 0x8ad214 直虚调；w1>=1∧f9!=2 走
         0x8ad22c 动画路径；o20==0 ⇒ 动画路径 0x8ad28c cbz 跳过 bl 0x8ac800）。
         round20 最小扰动构建：三条件快照编译关闭（观察器侧 #if），本段只打 off 标记+
         门本体计数（kick1 纯计数门保留）。 */
#if SMO_OBS_HEAVY
      {
      extern int smo_obs_k1_stats(uint64_t[5], uint64_t[3], uint64_t[4]);
      uint64_t kf[5], kl[3], kd[4];
      if (smo_obs_k1_stats(kf, kl, kd))
          fprintf(stderr, "[totk-dump] kick1 cond: first this=%#llx w1=%llu [this+0xf9]=%#llx [this+0x20]=%#llx at +%lluns\n"
                          "[totk-dump] kick1 cond: last w1=%llu f9=%#llx o20=%#llx | dist: w1<1=%llu w1>=1&f9==2=%llu w1>=1&f9!=2=%llu o20==0=%llu (hits=%llu)\n",
                  (unsigned long long)kf[0], (unsigned long long)kf[1], (unsigned long long)kf[2],
                  (unsigned long long)kf[3], (unsigned long long)kf[4],
                  (unsigned long long)kl[0], (unsigned long long)kl[1], (unsigned long long)kl[2],
                  (unsigned long long)kd[0], (unsigned long long)kd[1], (unsigned long long)kd[2],
                  (unsigned long long)kd[3], (unsigned long long)smo_obs_hits(4));
      else fprintf(stderr, "[totk-dump] kick1 cond: none (gate never hit)\n");
      }
      /* round18 ①：KICK1 首 16 次逐次快照环 + 六边际计数（与上方三分支交叉互补：
         边际不受交叉条件限制，f9/o20 的分布在 w1==0 直虚调路径上也可见）。 */
      {
      extern unsigned smo_obs_k1_hist16(uint64_t[16], uint64_t[16], uint64_t[16], uint64_t[16], uint64_t[6], uint64_t *);
      uint64_t rh[16], rw[16], rf[16], ro[16], rm[6], rtot = 0;
      unsigned rn = smo_obs_k1_hist16(rh, rw, rf, ro, rm, &rtot);
      fprintf(stderr, "[totk-dump] kick1 first16: recorded=%u/%llu | marg: w1==0=%llu w1!=0=%llu f9==2=%llu f9!=2=%llu o20==0=%llu o20!=0=%llu\n",
              rn, (unsigned long long)rtot,
              (unsigned long long)rm[0], (unsigned long long)rm[1], (unsigned long long)rm[2],
              (unsigned long long)rm[3], (unsigned long long)rm[4], (unsigned long long)rm[5]);
      for (unsigned i = 0; i < rn; i++)
          fprintf(stderr, "[totk-dump] kick1 first16[%u]: this=%#llx w1=%llu [this+0xf9]=%#llx [this+0x20]=%#llx\n",
                  i, (unsigned long long)rh[i], (unsigned long long)rw[i],
                  (unsigned long long)rf[i], (unsigned long long)ro[i]);
      }
#else
      fprintf(stderr, "[totk-dump] kick1 cond/first16: probes OFF this build (round20 minimal-perturbation; SMO_OBS_HEAVY=0) | kick1 counting gate hits=%llu last_x0=%#llx\n",
              (unsigned long long)smo_obs_hits(4), (unsigned long long)smo_obs_last_x0(4));
#endif
      /* round17 ②：BC310 广播调用点（donor main+0x954310，CreateThread→StartThread→svc0x1d
         广播序列现场 x3 恒=该入口值，且全镜像无直接分支落此＝只经指针调用）。emu hits>0 ⇒
         emu main 执行了同一 boot 广播链；first/last x0=该函数收的对象（InitSystem 收件箱归属）。
         round21 增调用者 LR 读数：blr 调用⇒LR=调用点+4、br 尾跳⇒LR=更上层返回地址；首 64
         采样环分布（符号化）=外层循环调用点定位的主读数；末见 {x1,ts}=45807 次后循环耗尽
         时的末次现场（循环头/退出分支门待 LR 定位调用者后下轮装）。 */
      {
      extern int smo_obs_bc_snapshot(uint64_t *, uint64_t *, uint64_t *);
      uint64_t bfx0 = 0, bfx1 = 0, bfts = 0;
      int bchit = smo_obs_bc_snapshot(&bfx0, &bfx1, &bfts);
      fprintf(stderr, "[totk-dump] obs bc310 0x954310 hits=%llu first_x0=%s%#llx x1=%s%#llx (at +%lluns) last_x0=%#llx\n",
              (unsigned long long)smo_obs_hits(9), bchit ? "" : "0/", bchit ? (unsigned long long)bfx0 : 0ULL,
              bchit ? "" : "0/", bchit ? (unsigned long long)bfx1 : 0ULL,
              (unsigned long long)bfts, (unsigned long long)smo_obs_last_x0(9));
      }
      {
      extern int smo_obs_bc_caller(uint64_t *, uint64_t *, uint64_t *, uint64_t *, int *, uint64_t[64], uint64_t *);
      uint64_t flr = 0, llr = 0, lx1 = 0, lts = 0, lrv[64], ltot = 0; int lraw = 0;
      if (smo_obs_bc_caller(&flr, &llr, &lx1, &lts, &lraw, lrv, &ltot)) {
          const char *nm; uint64_t off;
          fprintf(stderr, "[totk-dump] bc310 caller: first_lr=%#llx", (unsigned long long)flr);
          if (runtime_module_offset(flr, &nm, &off)) fprintf(stderr, " -> %s+%#llx", nm, (unsigned long long)off);
          fprintf(stderr, " | last_lr=%#llx", (unsigned long long)llr);
          if (runtime_module_offset(llr, &nm, &off)) fprintf(stderr, " -> %s+%#llx", nm, (unsigned long long)off);
          fprintf(stderr, " | last x1=%#llx at %s%llu | lr-samples=%llu/64\n",
                  (unsigned long long)lx1, lraw ? "tick " : "+", (unsigned long long)lts, (unsigned long long)ltot);
          for (unsigned i = 0; i < 64 && i < ltot; i++) {
              if (!lrv[i]) continue;
              fprintf(stderr, "[totk-dump] bc310 lr[%u]=%#llx", i, (unsigned long long)lrv[i]);
              if (runtime_module_offset(lrv[i], &nm, &off)) fprintf(stderr, " -> %s+%#llx", nm, (unsigned long long)off);
              fputc('\n', stderr);
          }
      } else fprintf(stderr, "[totk-dump] bc310 caller: none (gate never hit)\n");
      }
      /* round21：kickvt/kickbr 两段随门退役删除（round17/18 的虚路径分叉问题已闭环）。 */
      /* round19 ①②：al thunk 双门×2（thunk1=0x587ea0 两参 kick 蹦床、thunk2=0x587eb0
         三参变体蹦床）。入口门装在唯一进入分支（0x587da4/0x587db4 的 b，全镜像扫描
         唯一落点+门址零落点）计全部进入；NULL 出口门装在 cbz 早退落点（0x587eac/
         0x587ec4 的 ret，寄存器分支重放）计 [x0]==NULL 早退。联读：thunk1_in−alkick
         ≈thunk1_null（自洽校验，残差=装门窗口外的进入路径，理论应为 0）；thunk2_in
         是 KICK3(0x8ad310 族) 路径 thunk 层计数。门 id：thunk1_in=12/null=13，
         thunk2_in=14/null=15（game_patches.c round19 enum）。 */
      fprintf(stderr, "[totk-dump] thunk gates: thunk1 in(0x587da4)=%llu null(0x587eac)=%llu | thunk2 in(0x587db4)=%llu null(0x587ec4)=%llu\n",
              (unsigned long long)smo_obs_hits(12), (unsigned long long)smo_obs_hits(13),
              (unsigned long long)smo_obs_hits(14), (unsigned long long)smo_obs_hits(15));
      /* round22 donor 黄金清单对拍（只读；黄金值=logs/donor_golden.txt）：
         ①注册槽：thunk1(0x587da0)首字 add x0,x0,#8、门 0x587da4 装其后 b 上 ⇒ 入口门末见
           x0 = outer+8 槽址（donor outer=0x708d6c29c8=thunk1 入参 x0）；NULL 出口门(0x587eac)
           末见 x0 = 早退时 [outer+8] 槽值（donor 黄金：注册后=H=0x704225b000；[outer+0]=
           base+0x12dd510 vtable）。非 NULL 则读 H 反向指针族：donor [H]=base+0x131ffb8、
           [H+0x14]=0x70、[H+0x20]=outer+8 槽址、[H+0x40]=arr（=等待容器[C+0x48]）、[H+0x108]=
           0x1000000，并读首桶（donor bucket[0]=arr，[bucket+8]=H+0x1c0 单节点环，+0x10 第二链头
           H+0x1d0）。
         ②mq 链头：0x964680 walker 正走的链——SIGUSR2 采样器末次成功样本 x19（round21 全程仅
           1 次成功样本，恰为 walker 家族线程，pc 链 main+0x964640/0x97e468 帧）。donor 语义
           node=owner(x19)+0x88（采样帧不同，x19 可能已在 node 上），候选 {x19, x19+0x88,
           x19-0x88} 读黄金三字，命中者按 stride 0x38 走 k=0..4 深环样例（donor node[4]+0xc8=5、
           node[5]+0xc8=0x13）。另对 last_lr∈main[0x963d00,0x965000) 的 walker 家族线程读
           arg+0x88 同三字（emu 名册 arg 呈 …020 形态=donor owner 同款）。
         ③等待容器 C（donor C=0x7041f01f20：count[C+0x40]=1、arr[C+0x48]、桶 stride 0x78）：
           emu [outer+8]=NULL ⇒ 0x8b5ca0 注册体未跑（reg 门 0 命中）、donor 唯一已知入容器指针
           路径（[H+0x40]=arr）断链；C 类 vtable 静态窗由 TOTK_DUMP_AT 默认表读
           （game_napi r22：0x131f318 族模板 + 0x12dc7xx 运行期表 + 0x12dd510）。 */
      {
      extern uint64_t smo_obs_last_x0(int);
      uint64_t oslot = smo_obs_last_x0(12), onull = smo_obs_last_x0(13);
      fprintf(stderr, "[totk-dump] r22 outer: thunk1-in last-x0(=outer+8 slot addr)=%#llx | thunk1-null last-x0(=[outer+8] at exit)=%#llx\n",
              (unsigned long long)oslot, (unsigned long long)onull);
      if (oslot >= 0x100000008ULL && oslot < 0x800000000000ULL) {
          uint64_t outer = oslot - 8, v0 = 0, v8 = 0;
          int g0 = r22_read64(outer, &v0), g8 = r22_read64(outer + 8, &v8);
          char d0[96]; d0[0] = 0;
          if (g0 && v0 >= 0x100000000ULL) runtime_describe_address(v0, d0, sizeof d0);
          fprintf(stderr, "[totk-dump] r22 outer obj %#llx: [outer+0]=%#llx%s%s%s [outer+8]=%#llx%s\n",
                  (unsigned long long)outer, (unsigned long long)(g0 ? v0 : 0), g0 ? "" : "(unread)",
                  d0[0] ? " = " : "", d0, (unsigned long long)(g8 ? v8 : 0), g8 ? "" : "(unread)");
          if (g8 && v8 >= 0x100000000ULL && v8 < 0x800000000000ULL) {
              uint64_t hv = 0, h10 = 0, h20 = 0, h40 = 0, h108 = 0;
              r22_read64(v8, &hv); r22_read64(v8 + 0x10, &h10); r22_read64(v8 + 0x20, &h20);
              r22_read64(v8 + 0x40, &h40); r22_read64(v8 + 0x100, &h108);
              char dh[96]; dh[0] = 0;
              if (hv >= 0x100000000ULL) runtime_describe_address(hv, dh, sizeof dh);
              fprintf(stderr, "[totk-dump] r22 H %#llx: [H]=%#llx(%s) [H+0x10]=%#llx ([H+0x14]=%#llx) [H+0x20]=%#llx [H+0x40]=%#llx [H+0x108]=%#llx\n",
                      (unsigned long long)v8, (unsigned long long)hv, dh, (unsigned long long)h10,
                      (unsigned long long)(h10 >> 32), (unsigned long long)h20, (unsigned long long)h40, (unsigned long long)h108);
              if (h40 >= 0x100000000ULL && h40 < 0x800000000000ULL) {
                  uint64_t b0 = 0, b8 = 0, b10 = 0, b18 = 0;
                  r22_read64(h40, &b0); r22_read64(h40 + 8, &b8); r22_read64(h40 + 0x10, &b10); r22_read64(h40 + 0x18, &b18);
                  fprintf(stderr, "[totk-dump] r22 bucket[0]=arr %#llx: [b+0]=%#llx [b+8]=%#llx [b+0x10]=%#llx [b+0x18]=%#llx\n",
                          (unsigned long long)h40, (unsigned long long)b0, (unsigned long long)b8, (unsigned long long)b10, (unsigned long long)b18);
              }
          } else fprintf(stderr, "[totk-dump] r22 H: [outer+8] NULL/越界 ⇒ emu 注册未发生，入容器指针路径（[H+0x40]=arr）断链\n");
      } else fprintf(stderr, "[totk-dump] r22 outer: thunk1-in 门无样本（x0=0）\n");
      {
          /* round22b：优先 walker 窗锁存样本（末次成功样本会被宿主帧覆盖，见 g_r22_walk 注释） */
          uint64_t sx19, spc;
          if (r22_walk_set()) { spc = r22_walk_pc(); sx19 = r22_walk_x19(); }
          else { spc = r22_spin_pc(); sx19 = r22_spin_x19(); }
          char dp[96]; dp[0] = 0;
          if (spc >= 0x100000000ULL) runtime_describe_address(spc, dp, sizeof dp);
          fprintf(stderr, "[totk-dump] r22 mq sample(%s): pc=%#llx(%s) x19=%#llx\n",
                  r22_walk_set() ? "walker-window latch" : "last-success latch",
                  (unsigned long long)spc, dp, (unsigned long long)sx19);
          for (int ci = 0; ci < 3; ci++) {
              uint64_t cand = ci == 0 ? sx19 : ci == 1 ? sx19 + 0x88 : sx19 - 0x88;
              if (cand < 0x100000000ULL || cand >= 0x800000000000ULL) continue;
              r22_node_fields(cand, ci == 0 ? "mq cand0=x19    " : ci == 1 ? "mq cand1=x19+88 " : "mq cand2=x19-88 ");
              for (int k = 1; k <= 4; k++) {
                  char tag[48]; snprintf(tag, sizeof tag, "mq cand%d ring k=%d  ", ci, k);
                  r22_node_fields(cand + 0x38ULL * (unsigned)k, tag);
              }
          }
          for (thr_t *t = g_all; t; t = t->all) {
              /* r22b：名册 arg 定位（mq 家族线程 arg 呈 donor owner 同款 …020 形态）。首轮用
                 last_lr∈main[0x964xxx] 过滤——实测等待线程 last_lr 全落 sdk+ svc 包装（dump_chain
                 链里 main+0x964xxx 是更上层帧），0 行输出；改为纯 arg 域过滤（<0x7f0000000000
                 排除 0x7f7+ 宿主栈），walker 家族认定离线用上方链行互证。 */
              if (t->arg < 0x100000000ULL || t->arg >= 0x7f0000000000ULL) continue;
              char tag[64]; snprintf(tag, sizeof tag, "mq-thread h=%#x arg+0x88", t->handle);
              r22_node_fields(t->arg + 0x88, tag);
          }
      }
      }
      /* round20 注册两门 + round21 调用者两门（game_patches.c 门 id 16/17/18/19）：
         entry(0x8ae230)=注册函数（str x1,[x0] 写句柄入槽）全部进入；done(0x8ae258)=注册体
         跑到尾声（str 与回指针写都在其上游线性路径上；全镜像扫描 23c/240/244 零落点 ⇒
         str 执行 ⇔ entry 命中）；entry==done ⇔ 每次进入都完整注册，entry>done=中途异常。
         mq walker 双尾门（18/19）round21 退役，id 改派 FN_A/FN_B（上方 fn_a/fn_b 行）。
         首见 {x0,x1,ts}：REG230 x1=被注册句柄；FN_A x1=版本容器；FN_B x0=容器本体——
         与 REG230 首见 ts 相减=调用者函数 vs 注册动作时序。 */
      fprintf(stderr, "[totk-dump] reg gates: entry(0x8ae230)=%llu done(0x8ae258)=%llu\n",
              (unsigned long long)smo_obs_hits(16), (unsigned long long)smo_obs_hits(17));
      {
      extern int smo_obs_fs_snapshot(int, uint64_t *, uint64_t *, uint64_t *);
      static const char *fsn[3] = { "reg-entry 0x8ae230 (x0=slot x1=handler)", "fn_a 0x8b65e0 (x0=timebase x1=ver-container)", "fn_b 0x8b6920 (x0=container x1=arg2)" };
      for (int k = 0; k < 3; k++) {
          uint64_t fx0 = 0, fx1 = 0, fts = 0;
          if (smo_obs_fs_snapshot(k, &fx0, &fx1, &fts))
              fprintf(stderr, "[totk-dump] fs[%d] %s: first x0=%#llx x1=%#llx at +%lluns\n",
                      k, fsn[k], (unsigned long long)fx0, (unsigned long long)fx1, (unsigned long long)fts);
          else fprintf(stderr, "[totk-dump] fs[%d] %s: none (gate never hit)\n", k, fsn[k]);
      }
      }
      /* round19 ③：BC310 first-16 唯一 x0 环——45807 hits 的收件对象分布（首 16 个唯一
         x0 枚举+各计数；x0 是对象指针，非代码地址，不做模块符号化）。
         round20 最小扰动构建：x0 环编译关闭（观察器侧 #if），本段只打 off 标记；BC310
         门本身保留首见 {x0,x1}+hits 计数（上方 bc310 行继续输出）。 */
#if SMO_OBS_HEAVY
      {
      extern int smo_obs_bc_x0ring(uint64_t[16], uint64_t[16], uint64_t *, uint64_t *);
      uint64_t xv[16], xc[16], xover = 0, xzero = 0;
      if (smo_obs_bc_x0ring(xv, xc, &xover, &xzero)) {
          fprintf(stderr, "[totk-dump] bc310 first16 x0: zero=%llu over=%llu\n",
                  (unsigned long long)xzero, (unsigned long long)xover);
          for (int i = 0; i < 16; i++)
              if (xv[i]) fprintf(stderr, "[totk-dump] bc310 first16[%d]: x0=%#llx cnt=%llu\n",
                                 i, (unsigned long long)xv[i], (unsigned long long)xc[i]);
      } else fprintf(stderr, "[totk-dump] bc310 first16 x0: none (gate never hit)\n");
      }
#else
      fprintf(stderr, "[totk-dump] bc310 first16 x0: probe OFF this build (round20 minimal-perturbation; SMO_OBS_HEAVY=0)\n");
#endif
      /* round24 解引用首字探针（evidence_e1e2/probe_patch.md）：TP1=0x57f950、TP2=0x8aef34
         入口门；TP4R/TP4A=0x964680 出口双门（hits 相加=该函数全部进入；TP4A 的 x0 已由
         观察器还原 −0x38）。[x0]= 值为命中时实读 8 字节（首见/末见），非 dump 时回读。 */
      {
      extern int smo_obs_tp_snapshot(int, uint64_t *, uint64_t *, uint64_t *, uint64_t *);
      static const char *tpn[4] = { "TP1 main+0x57f950 (entry)",
                                    "TP2 main+0x8aef34 (entry)",
                                    "TP4 main+0x9646a4 (ret exit, x0=entry)",
                                    "TP4 main+0x9646ac (adv exit, x0=entry via -0x38)" };
      for (int k = 0; k < 4; k++) {
          uint64_t t[3], nok = 0, nbad = 0, ld = 0;
          if (smo_obs_tp_snapshot(k, t, &nok, &nbad, &ld))
              fprintf(stderr, "[totk-dump] %s: hits=%llu first x0=%#llx [x0]=0x%llx at +%lluns | last [x0]=0x%llx deref-ok=%llu bad=%llu\n",
                      tpn[k], (unsigned long long)smo_obs_hits(20 + k),
                      (unsigned long long)t[0], (unsigned long long)t[1], (unsigned long long)t[2],
                      (unsigned long long)ld, (unsigned long long)nok, (unsigned long long)nbad);
          else fprintf(stderr, "[totk-dump] %s: none (gate never hit) hits=%llu\n",
                       tpn[k], (unsigned long long)smo_obs_hits(20 + k));
      }
      /* round25 TP5：DecompThread 出口状态字序列（挂 TP4R/TP4A 观察器；前 8 次+第 10000 次）。
         state=[x0+0xd4]（u32）、val=[x0+0xc8]（u64），均为命中时实读非 dump 回读；
         x0=0（TP4R 的 w0 返回值）=护栏拒读，state/val 无效。 */
      {
      extern int smo_obs_tp5_seq(int, uint64_t out[9][3]);
      static const char *tp5n[2] = { "TP5 ret main+0x9646a4", "TP5 adv main+0x9646ac" };
      for (int q = 0; q < 2; q++) {
          uint64_t s[9][3];
          int n = smo_obs_tp5_seq(q, s);
          if (!n)
              fprintf(stderr, "[totk-dump] %s: none (gate never hit or all rejected) hits=%llu\n",
                      tp5n[q], (unsigned long long)smo_obs_hits(22 + q));
          else {
              fprintf(stderr, "[totk-dump] %s: %d sample(s) of %llu hit(s):\n",
                      tp5n[q], n, (unsigned long long)smo_obs_hits(22 + q));
              for (int i = 0; i < n; i++)
                  fprintf(stderr, "[totk-dump] TP5 state=%#llx val=%#llx (x0=%#llx%s)\n",
                          (unsigned long long)s[i][1], (unsigned long long)s[i][2],
                          (unsigned long long)s[i][0],
                          i == 8 ? ", 10000th" : s[i][0] ? "" : ", REJECTED (x0=0)");
          }
      }
      }
      }
      /* round21：E2 池归属段随 E2a/E2b 门退役删除；cell2/E1 snapshot/E1 变迁史三段随门退役
         删除（问题均已闭环）。 */
      /* round15b 回执写入门（gfx_bridge 回执环）：writes=退休时真实落内存次数（emu14 遗留①验收位：
         writes>0=回执落地）；recorded 分 kind 对照未退休 pending；unknown_handle=重录后旧句柄再提交。 */
      {
      extern uint64_t dm_nvn_receipt_stat(int);
      fprintf(stderr, "[totk-dump] receipt ring: writes=%llu pending=%llu recorded(counter=%llu value=%llu signal=%llu) dropped=%llu unknown_handle=%llu submits=%llu\n",
              (unsigned long long)dm_nvn_receipt_stat(0), (unsigned long long)dm_nvn_receipt_stat(1),
              (unsigned long long)dm_nvn_receipt_stat(2), (unsigned long long)dm_nvn_receipt_stat(3),
              (unsigned long long)dm_nvn_receipt_stat(4), (unsigned long long)dm_nvn_receipt_stat(5),
              (unsigned long long)dm_nvn_receipt_stat(6), (unsigned long long)dm_nvn_receipt_stat(7));
      }
      /* E4：等侧两族推导表 + 交集判定（worker=key-0x78；池对象=key-0x58）。
         round21：E1 绑定侧（[el+0x10]==wkey-0x58）随 E1 门退役恒 0——round17 已实证
         confluence=1/e1-binding=0，保留等侧账本供对账。 */
      int confluence = 0, binding = 0;
      for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2) {
          uint64_t worker = w->wkey - 0x78;
          uint64_t wv[2] = {0, 0};
          int got = worker > 0x100000000ULL && spin_read16(worker + 0x20, wv);
          /* round24 TP3：worker 池对象（=wkey-0x58，1689 行注）首 8 字节——r22_read64
             8 字节粒度安全读（process_vm_readv/mincore 护栏同款，threads.c:1181）；
             失败打 0 尾缀 ?（r22 先例）。 */
          uint64_t pobj = w->wkey - 0x58, pv = 0;
          int pg = r22_read64(pobj, &pv);
          fprintf(stderr, "[totk-dump] E4 waiter h=%#x wkey=%#llx worker=%#llx [worker+0x20]=%#llx TP3 [obj]=0x%llx%s\n",
                  w->handle, (unsigned long long)w->wkey, (unsigned long long)worker,
                  (unsigned long long)(got ? wv[0] : 0), (unsigned long long)(pg ? pv : 0), pg ? "" : "?");
      }
      for (unsigned t = 0; t < 3; t++) {
          unsigned cnt = t == 0 ? g_pkey_n : t == 1 ? g_pkey2_n : g_pkey3_n;
          for (unsigned i = 0; i < cnt; i++) {
              uint64_t key = t == 0 ? g_pkey[i].key : t == 1 ? g_pkey2[i].key : g_pkey3[i].key;
              fprintf(stderr, "[totk-dump] E4 sig-key %#llx (pkey%s) pool-obj(key-0x58)=%#llx\n",
                      (unsigned long long)key, t == 0 ? "" : t == 1 ? "2" : "3", (unsigned long long)(key - 0x58));
              for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) confluence = 1;
          }
      }
      fprintf(stderr, "[totk-dump] E4 verdict: key-confluence(sig-key==wait-key)=%d e1-binding(this==worker+0x20)=%d (E1 retired round21)\n",
              confluence, binding);
    }
    /* round6 观测（任务3）：h=0x1023 的 svc 0x18 醒来计数——calls=进入等待次数，woken=被唤醒返回次数，
       woken-by-0x101f=其中被目标事件 0x101f 唤醒的（woken 与之恒等差=被别的句柄或超时喂醒）。 */
    fprintf(stderr, "[totk-dump] svc 0x18 h=0x1023 wakeups: calls=%llu woken=%llu woken-by-0x101f=%llu\n",
            (unsigned long long)g_w1023_calls, (unsigned long long)g_w1023_wake, (unsigned long long)g_w1023_wake_101f);
    /* TOTK_DUMP_AT=rva1,rva2,...：静态地址取证通道（round33）——对每个 main 模块 RVA 转储
       该处 4 个 u64，像指针的值经 describe 符号化。客体数据段有效映射，直读安全。 */
    {const char *e=getenv("TOTK_DUMP_AT");uint8_t *mb=e?runtime_module_base("main"):NULL;
     if(e&&mb){char spec[256];snprintf(spec,sizeof spec,"%s",e);char *save=NULL;
      for(char *tok=strtok_r(spec,",",&save);tok;tok=strtok_r(NULL,",",&save)){
       uint64_t rva=strtoull(tok,NULL,0),*a=(uint64_t *)(mb+rva);if(!rva)continue;
       uint64_t w[4];for(int q=0;q<4;q++)w[q]=__atomic_load_n(a+q,__ATOMIC_RELAXED);
       fprintf(stderr,"[totk-dump] at main+%#llx: %016llx %016llx %016llx %016llx\n",(unsigned long long)rva,
               (unsigned long long)w[0],(unsigned long long)w[1],(unsigned long long)w[2],(unsigned long long)w[3]);
       for(int q=0;q<4;q++){char d[96];if(w[q]>=0x100000000ULL){runtime_describe_address(w[q],d,sizeof d);fprintf(stderr,"[totk-dump]   +%#llx[%d]=%s\n",(unsigned long long)rva,q,d);} } } } }
}
void runtime_dump_threads(void) { dump_threads(0); }   // TOTK_INPUT_FILE "threads"

/* ================= M40U（2026-11-10，只读取证 `smo_m40u` 的唯一一次定向 dump） =================
   问题：资源已载完（opens 冻结 1244、inflight=0），画面却白屏空转（cmd/帧 ×13、img/帧 ×28）。
   主线程到底在等什么？本函数由 romfs.c 的 m40u_tick() 在「opens 冻结 15 s 后再等 15 s」时调一次。
   通道选择：**不复用 runtime_dump_threads()**。dump_threads() 打的是 stderr，而 OHOS 的 stderr
   不进 hilog 环形缓冲（见 vk_boot.c:31 的 M3b 注与 M5b 的「M5i 只剩 [gst] 紧凑表」）——线上抓包里
   [totk-dump] 一行都看不到。所以这里把同一批字段（句柄/等待种类/等待地址/SVC/owner）按 [m40u]
   前缀重打到**文件通道** vk_boot_logf。
   输出（≤200 行 [m40u]，一次）：
     DUMP  —— 帧/呈现/img/cmd/stall（stall = 帧计数最后一次变化的秒数，单位在 UNITS 行自证）
     TH    —— 每条 guest 线程：h / 状态 / wk / wa / key / svc / nsvc / 客体 LR
     WH    —— wk=4 线程等的句柄号
     OWN   —— 每条等待线程的 owner 画像：地址型（wk=1/2/3）取 [sy] addrsig 的首/末 0x35 调用者，
              句柄型（wk=4）取句柄表 creator
     RT    —— RT 槽表前 8（nvn_vk.c 里实现，弱符号）
     SY    —— main(0x1001) 与 draw(0x1004) 的 1:1 同步账本（同 [sy] addrsig 字段）
   只读：G 上不做任何写入，线程字段是同一类无锁读（与 dump_threads 同风险等级）。 */
void runtime_m40u_dump(unsigned long frames, unsigned long present, unsigned long img,
                       unsigned long cmd, unsigned stall_s)
{
    extern void vk_boot_logf(const char *fmt, ...);
    extern void nvn_vk_rt_tab_dump_m40u(void) __attribute__((weak));
    uint64_t now = now_ns();
    unsigned long lines = 0;
    vk_boot_logf("[m40u] DUMP t=%us frames=%llu present=%llu img=%llu cmd=%llu stall=%llu",
                 (unsigned)(now / 1000000000ull), (unsigned long long)frames,
                 (unsigned long long)present, (unsigned long long)img,
                 (unsigned long long)cmd, (unsigned long long)stall_s);
    /* 单位自证：ns 原值与 /1e9 后同打一行（M40T 的 winRate 曾把 ns→s 写成 ×1e6，全错还不报错）。 */
    vk_boot_logf("[m40u] UNITS t_ns=%llu t_s=%u stall_s=%u frames=%llu (ns/1e9 -> s)",
                 (unsigned long long)now, (unsigned)(now / 1000000000ull), (unsigned)stall_s,
                 (unsigned long long)frames);
    lines += 2;
    for (int c = 0; c < NCORE; c++) {
        thr_t *o = g_core_owner[c];
        char lr[64]; lr[0] = 0;
        if (o && o->last_lr) runtime_describe_address(o->last_lr, lr, sizeof lr);
        vk_boot_logf("[m40u] CORE c=%d owner=%#x held_s=%llu lr=%s", c, o ? o->handle : 0,
                     (unsigned long long)(o ? (now - o->core_since) / 1000000000ull : 0), lr[0] ? lr : "-");
        lines++;
    }
    /* ---- 全部 guest 线程：h / wk / wa / svc ---- */
    int nth = 0, nwait = 0;
    for (thr_t *t = g_all; t; t = t->all) {
        if (!t->exited && t->wkind) nwait++;
        nth++;
        if (lines >= 120) continue;
        char lr[64]; lr[0] = 0;
        if (t->last_lr) runtime_describe_address(t->last_lr, lr, sizeof lr);
        vk_boot_logf("[m40u] TH h=%#x %s prio=%d core=%d wk=%d wa=%#llx key=%#llx tag=%#x svc=%#x nsvc=%llu lr=%s",
                     t->handle, t->exited ? "exited" : t->started ? "run" : "new", t->prio, t->cur_core,
                     t->wkind, (unsigned long long)t->waddr, (unsigned long long)t->wkey, t->wtag,
                     t->last_svc, (unsigned long long)t->nsvc, lr[0] ? lr : "-");
        lines++;
        if (t->wkind == 4 && lines < 130) {   /* 同步等待：等的是哪几个句柄 */
            char b[192]; int o = 0; b[0] = 0;
            for (int i = 0; i < t->nwh && i < 4 && o < (int)sizeof b - 16; i++)
                o += snprintf(b + o, sizeof b - (size_t)o, " %#x", t->wh[i]);
            vk_boot_logf("[m40u] WH h=%#x nwh=%d waited:%s", t->handle, t->nwh, b);
            lines++;
        }
    }
    vk_boot_logf("[m40u] SUMMARY threads=%d waiting=%d lines=%lu", nth, nwait, (unsigned long)lines);
    lines++;
    /* ---- owner 画像：谁该叫醒它 ---- */
    {
        int own = 0;
        for (thr_t *t = g_all; t && own < 20; t = t->all) {
            if (t->exited || !t->wkind) continue;
            uint64_t a = (t->wkind == 2) ? t->wkey : t->waddr;
            unsigned long long w34 = 0, w35 = 0, wake = 0, zero = 0;
            unsigned first = 0, last = 0; char lrw[64]; lrw[0] = 0;
            for (unsigned i = 0; i < g_sigaddr_n; i++)
                if (g_sigaddr[i].addr == a) {
                    w34 = g_sigaddr[i].n34; w35 = g_sigaddr[i].n35; wake = g_sigaddr[i].wake;
                    zero = g_sigaddr[i].zero; first = g_sigaddr[i].caller; last = g_sigaddr[i].lastcaller;
                    if (g_sigaddr[i].lastlr) runtime_describe_address(g_sigaddr[i].lastlr, lrw, sizeof lrw);
                    break;
                }
            unsigned oh = first ? first : last;
            char wh[96]; wh[0] = 0;
            if (t->wkind == 4 && t->nwh > 0) {   /* 句柄型：owner = 该句柄的创建者 */
                unsigned long hh = (unsigned long)t->wh[0];
                unsigned i = (hh >= 0x1000u && hh < 0x1000u + MAX_H) ? (unsigned)(hh - 0x1000u) : MAX_H;
                if (i < MAX_H && g_h[i].type) {
                    char where[96]; runtime_describe_address(g_h[i].creator, where, sizeof where);
                    snprintf(wh, sizeof wh, "objcreator=%s", where[0] ? where : "-");
                }
            }
            vk_boot_logf("[m40u] OWN h=%#x wk=%d addr=%#llx owner=%#x wait=%llu sig=%llu woke=%llu zero=%llu svc=%#x siglr=%s %s",
                         t->handle, t->wkind, (unsigned long long)a, oh, w34, w35, wake, zero,
                         t->last_svc, lrw[0] ? lrw : "-", wh);
            lines++; own++;
        }
    }
    /* ---- RT 槽表（nvn_vk.c 实现；弱符号，缺实现时只留一行说明） ---- */
    if (nvn_vk_rt_tab_dump_m40u) nvn_vk_rt_tab_dump_m40u();
    else vk_boot_logf("[m40u] RT unavailable (nvn_vk not linked)");
    /* ---- main / draw 的 1:1 同步账本（与 [sy] addrsig 同字段；main 当前等待地址优先用 0x34 账本） ---- */
    {
        static const unsigned want[2] = { 0x1001u, 0x1004u };
        for (int w = 0; w < 2; w++) {
            thr_t *t = NULL;
            for (thr_t *q = g_all; q; q = q->all) if (q->handle == want[w]) { t = q; break; }
            if (!t) continue;
            uint64_t a = (t->wkind == 2) ? t->wkey : t->waddr;
            if (w == 0 && g_m5k_last34) a = g_m5k_last34;   /* main 卡的那个 0x34 地址（[m5k] 账本） */
            unsigned long long w34 = 0, w35 = 0, wake = 0, zero = 0;
            unsigned first = 0, last = 0; char lr[64]; lr[0] = 0;
            for (unsigned i = 0; i < g_sigaddr_n; i++)
                if (g_sigaddr[i].addr == a) {
                    w34 = g_sigaddr[i].n34; w35 = g_sigaddr[i].n35; wake = g_sigaddr[i].wake;
                    zero = g_sigaddr[i].zero; first = g_sigaddr[i].caller; last = g_sigaddr[i].lastcaller;
                    if (g_sigaddr[i].lastlr) runtime_describe_address(g_sigaddr[i].lastlr, lr, sizeof lr);
                    break;
                }
            vk_boot_logf("[m40u] SY h=%#x wait=%llu sig=%llu woke=%llu zero=%llu svc=%#x owner=%#x wk=%d addr=%#llx nsvc=%llu siglr=%s",
                         t->handle, w34, w35, wake, zero, t->last_svc, first ? first : last,
                         t->wkind, (unsigned long long)a, (unsigned long long)t->nsvc, lr[0] ? lr : "-");
            lines++;
        }
    }
    vk_boot_logf("[m40u] DONE lines=%lu (one-shot; no repeat this boot)", (unsigned long)lines);
}

/* M5j：单个客体线程的 guest 帧链，写 vk_boot.log。
   dump_chain()（:1224）走的就是这条路，但用 fprintf(stderr)，OHOS 把 stderr 丢掉（见下一段 M5b 注），
   所以 M5i 只剩 [gst] 紧凑表、答不出「主线程到底在等什么」。这里同一次走帧，改走文件通道：
   - 取线程最后一次进 SVC 时保存的 lr/fp（:723），这正是「阻塞调用的返回点」；
   - 帧记录用 spin_read16（process_vm_readv 读自身，PROT_NONE 页返 EFAULT 不死）逐帧读；
   - 每帧 runtime_describe_address 出 模块+偏移。
   只读：G 上 trylock 拷走字段后立刻放锁，不写任何线程状态。h=0 = 客体主线程（handle 0x1001，
   threads.c:803 的 yield 记账已确认 0x1001 就是主线程），否则按 handle 找。 */
void runtime_guest_chain_dump(unsigned h) {
    extern void vk_boot_logf(const char *fmt, ...);
    if (!h) h = 0x1001;
    int locked = 0; for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    thr_t *pick = NULL;
    for (thr_t *t = g_all; t; t = t->all) if (!t->exited && t->handle == h) { pick = t; break; }
    uint32_t pwh0 = 0, pwh1 = 0, psvc = 0; int pnw = 0, pwk = 0, pcore = 0;
    uint64_t plr = 0, pfp = 0, pnsvc = 0;
    if (pick) { pwh0 = pick->wh[0]; pwh1 = pick->wh[1]; pnw = pick->nwh; pwk = pick->wkind; pcore = pick->cur_core;
                plr = pick->last_lr; pfp = pick->last_fp; pnsvc = pick->nsvc; psvc = (uint32_t)pick->last_svc; }
    if (locked) pthread_mutex_unlock(&G);
    if (!pick) { vk_boot_logf("[gst] chain h=%#x not found (exited or never created)", h); return; }
    char where[96]; runtime_describe_address(plr, where, sizeof where);
    vk_boot_logf("[gst] chain h=%#x wk=%d core=%d wh=%#x,%#x nwh=%d svc=%#x nsvc=%llu lr=%#llx %s",
                 h, pwk, pcore, pwh0, pwh1, pnw, psvc, (unsigned long long)pnsvc, (unsigned long long)plr, where);
    int n = 0; uint64_t fp = pfp;
    for (int i = 0; i < 12 && fp && !(fp & 15u); i++) {
        uint64_t f[2];
        if (!spin_read16(fp, f)) { vk_boot_logf("[gst] chain h=%#x fp=%#llx unreadable, stop after %d frame(s)", h, (unsigned long long)fp, n); break; }
        runtime_describe_address(f[1], where, sizeof where);
        vk_boot_logf("[gst] chain h=%#x #%d fp=%#llx pc=%#llx %s", h, n, (unsigned long long)fp, (unsigned long long)f[1], where);
        n++;
        if (f[0] == fp) break; fp = f[0];
    }
    if (!pfp) vk_boot_logf("[gst] chain h=%#x no saved fp (no SVC taken yet), frames=0", h);
}

/* M5b：给冻结判定用的**紧凑线程表**，走 vk_boot.log（OHOS 上 stderr 不进任何文件，dump_threads 的
   [totk-dump] 在 boot 日志里一行都留不下——M5a 的 9502 行日志可证）。每条活线程一行：
   宿主 tid / 占的虚拟核 / 等待类别(0 跑 1 锁 2 条件 3 地址 4 同步) / 等待地址与 key / 前两个等待句柄 /
   最后一次 SVC 号与累计次数 / 最后一次 SVC 的客体 LR / 最后进入的宿主 NVN 重活及其年龄。
   两次 dump 的 nsvc 差即「这段时间该线程执行了多少 SVC」：全 0 = 全体在等，不是慢。
   只在 G 内拷贝、出锁再写盘（写盘可能毫秒级，不占 G）；不改变任何线程状态。 */
void runtime_guest_dump(void) {
    extern void vk_boot_logf(const char *fmt, ...);
    struct gst_row { uint32_t h; uint64_t htid, nsvc, lr, wa, key, opns, arg; uint32_t svc, wh0, wh1; int nwh, core, wk, op;
                     char who[40];   /* M5h: module+offset of the last SVC's guest LR (dump_threads' version goes to stderr, which OHOS drops) */
                     uint64_t seq; struct { int op; uint64_t arg, ns; } ph[6]; int phn; };
    enum { GST_MAX = 48 };
    static struct gst_row rows[GST_MAX];
    /* M6t C7: exited threads used to be counted (exited=N) and then dropped, so "0x102a exited at t=629
       while main was still waiting for it" had no evidence at all: no creator, no exit time, no join.
       Keep the last eight rows of threads that are gone (or exited and still in g_all) and print them,
       plus a scan of live waiters whose WaitSynchronization handle list names one of them. Record only. */
    enum { GX_MAX = 8 };
    static struct { uint32_t h, cre_h; uint64_t entry, cre_lr, nsvc, exit_ns; uint32_t tick; int closed; } gx[GX_MAX];
    int ngx = 0;
    int n = 0, nthr = 0, nex = 0, nrun = 0, nlock = 0, ncond = 0, naddr = 0, nsync = 0, locked = 0;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    uint64_t now = now_ns();
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->exited) {
            nex++;
            if (ngx < GX_MAX) {
                gx[ngx].h = t->handle; gx[ngx].cre_h = t->cre_h; gx[ngx].entry = t->entry;
                gx[ngx].cre_lr = t->cre_lr; gx[ngx].nsvc = t->nsvc; gx[ngx].exit_ns = t->exit_ns;
                gx[ngx].tick = t->exit_tick; gx[ngx].closed = t->closed; ngx++;
            }
            continue;
        }
        nthr++;
        if (t->wkind == 1) nlock++; else if (t->wkind == 2) ncond++;
        else if (t->wkind == 3) naddr++; else if (t->wkind == 4) nsync++; else nrun++;
        if (n >= GST_MAX) continue;
        rows[n].h = t->handle; rows[n].nsvc = t->nsvc; rows[n].svc = (uint32_t)t->last_svc; rows[n].lr = t->last_lr;
        rows[n].wk = t->wkind; rows[n].wa = t->waddr; rows[n].key = t->wkey;
        rows[n].nwh = t->nwh; rows[n].wh0 = t->wh[0]; rows[n].wh1 = t->wh[1];
        rows[n].core = t->cur_core; rows[n].op = t->op; rows[n].opns = t->op_ns;
        rows[n].arg = t->arg;   /* M7P: the CreateThread argument - the worker object pointer these 6 pool threads were created with, so a thread handle can be tied to the m7p per-object (Start) ledger by name instead of by guess. */
        rows[n].who[0] = 0; if (t->last_lr) runtime_describe_address(t->last_lr, rows[n].who, sizeof rows[n].who);
        rows[n].seq = t->phe_seq; rows[n].phn = 0;
        /* M5c：阶段环末 6 条（新→旧）。只拷贝，不写线程状态。 */
        for (int k = 0; k < 6 && k < 16; k++) {
            unsigned idx = (t->phe_i - 1u - (unsigned)k) & 15u;
            if ((unsigned)k >= t->phe_i && t->phe_seq <= 16) break;
            rows[n].ph[k].op = t->phev[idx].op; rows[n].ph[k].arg = t->phev[idx].arg; rows[n].ph[k].ns = t->phev[idx].ns;
            rows[n].phn++;
        }
        rows[n].htid = 0; if (t->pt) pthread_threadid_np(t->pt, &rows[n].htid);
        n++;
    }
    if (locked) pthread_mutex_unlock(&G);
    vk_boot_logf("[gst] threads=%d exited=%d run=%d lock=%d cond=%d addr=%d sync=%d g=%s",
                 nthr, nex, nrun, nlock, ncond, naddr, nsync, locked ? "ok" : "trylock-failed");
    for (int i = 0; i < n; i++)
        vk_boot_logf("[gst] h=%#x tid=%llu core=%d wk=%d wa=%#llx key=%#llx wh=%#x,%#x nwh=%d svc=%#x nsvc=%llu lr=%#llx who=%s op=%d(%s) age=%.0fms seq=%llu arg=%#llx",
                     rows[i].h, (unsigned long long)rows[i].htid, rows[i].core, rows[i].wk,
                     (unsigned long long)rows[i].wa, (unsigned long long)rows[i].key,
                     rows[i].wh0, rows[i].wh1, rows[i].nwh, rows[i].svc, (unsigned long long)rows[i].nsvc,
                     (unsigned long long)rows[i].lr, rows[i].who[0] ? rows[i].who : "-",
                     rows[i].op, runtime_op_name(rows[i].op),
                     rows[i].opns ? (double)(now - rows[i].opns) / 1e6 : -1.0,
                     (unsigned long long)rows[i].seq,
                     (unsigned long long)rows[i].arg);   /* M7P: CreateThread arg */

    /* M6t C7: exited-thread rows (creator / entry / exit time / SVC count) + "who is waiting on a ghost".
       A WaitSynchronization (wk=4) whose handle list names an exited thread is the literal "waits for a
       thread that is gone" signature; printed once per dump, never acted upon. */
    for (int i = 0; i < ngx; i++) {
        char who[64]; who[0] = 0;
        if (gx[i].cre_lr) runtime_describe_address(gx[i].cre_lr, who, sizeof who);
        vk_boot_logf("[gst] EXITED h=%#x cre=%#x cre_lr=%s nsvc=%llu exit_ago=%.1fs tick=%u closed=%d",
                     gx[i].h, gx[i].cre_h, who[0] ? who : "-", (unsigned long long)gx[i].nsvc,
                     gx[i].exit_ns ? (double)(now - gx[i].exit_ns) / 1e9 : -1.0, gx[i].tick, gx[i].closed);
        for (int k = 0; k < n; k++) {
            if (rows[k].wk != 4 || rows[k].nwh <= 0) continue;
            if (rows[k].wh0 == gx[i].h || rows[k].wh1 == gx[i].h)
                vk_boot_logf("[gst] WAIT-ON-EXITED waiter=%#x target=%#x wk=4 nwh=%d nsvc=%llu (still blocked after target exit)",
                             rows[k].h, gx[i].h, rows[k].nwh, (unsigned long long)rows[k].nsvc);
        }
    }
    /* M5c：非空闲线程的阶段环（新→旧）：op(arg)@age。判断「卡在一个调用里」还是「在循环小调用」看 seq。 */
    for (int i = 0; i < n; i++) {
        if (rows[i].wk != 0 || rows[i].op == 0 || rows[i].phn <= 0) continue;
        char b[512]; int o = 0;
        o += snprintf(b + o, sizeof b - (size_t)o, "[gst] ph h=%#x seq=%llu", rows[i].h, (unsigned long long)rows[i].seq);
        for (int k = rows[i].phn - 1; k >= 0; k--)   /* 旧→新，最右是最后一步 */
            o += snprintf(b + o, sizeof b - (size_t)o, " %s(%llu)@%.0fms",
                          runtime_op_name(rows[i].ph[k].op), (unsigned long long)rows[i].ph[k].arg,
                          rows[i].ph[k].ns ? (double)(now - rows[i].ph[k].ns) / 1e6 : -1.0);
        vk_boot_logf("%s", b);
    }
}

/* ---------- M6f: host tid -> KThread lookup (read-only) ----------
   The heartbeat thread reads /proc/self/task (vk_boot.c) and must say which guest thread a row is
   and what that thread is doing. This is the only side that owns thr_t, so it answers by handle.
   Takes G with the usual bounded trylock (never blocks a guest thread for more than ~100 ms of
   contended spinning, and never holds it while printing). Returns 1 when the tid is a live KThread. */
int runtime_tid_info(int tid, unsigned *handle, int *op, int *wk, unsigned long long *waddr,
                     int *core, int *prio, unsigned long long *nsvc) {
    int got = 0, found = 0;
    for (int i = 0; i < 20 && !(got = pthread_mutex_trylock(&G) == 0); i++) usleep(500);
    if (got) {
        for (thr_t *t = g_all; t; t = t->all) {
            if (t->exited || t->htid != tid) continue;
            if (handle) *handle = t->handle;
            if (op) *op = t->op;
            if (wk) *wk = t->wkind;
            if (waddr) *waddr = t->waddr;
            if (core) *core = atomic_load_explicit(&t->cur_core, memory_order_relaxed);
            if (prio) *prio = t->prio;
            if (nsvc) *nsvc = t->nsvc;
            found = 1; break;
        }
        pthread_mutex_unlock(&G);
    }
    return found;
}

/* ---------- M6d: "what is the loader waiting for" (read-only, once per dump) ----------
   Called from the heartbeat's dump path (vk_boot.c) next to [gst]/[io]/[svc]. Every number here
   already exists in this file; the point is to put the three answers on one line:
     (1) which thread has held the SAME wait tuple (wk,addr,key,lr,seq) for how many dumps,
     (2) which object it waits on, who created it, and whether that object is signaled,
     (3) how many waits that object has against how many signals - svc-side (0x11/0x18) and
         host-service-side (runtime_event_signal -> evlog) counted separately, because a host
         signal never shows up in the svc ledger (m6c_b h=0x101f "signals=0" is such a case).
   Nothing here writes thread state, changes a return value, or holds G beyond a copy. */
/* M6g: format and write the ring from the heartbeat thread (never from the SVC path). PRE/POST
   snapshots of the same address are paired into one line with before=, after= and the wait
   predicate; a PRE whose POST has not happened yet is held over to the next dump. */
static void ldr_flush(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    static unsigned rd;
    static struct { uint64_t addr, w[4], ns; int type, val, cur, rdf; } pnd[4];
    static int np;
    unsigned head = atomic_load_explicit(&g_ldr_head, memory_order_acquire);
    if (head > LDR_RING) head = LDR_RING;   /* the cursor keeps counting past a full ring; the array does not */
    char pcbuf[64];
    for (; rd < head; rd++) {
        ldr_ent_t *e = &g_ldr_ring[rd];
        if (!atomic_load_explicit(&e->seq, memory_order_acquire)) break;   /* entry still being filled */
        switch (e->kind) {
        case LDR_K_STEP:
            pcbuf[0] = 0; runtime_describe_address(e->lr, pcbuf, sizeof pcbuf);
            vk_boot_logf("[ldr] step k=%u w=%u svc=%#x pc=%s a0=%#llx a1=%#llx a2=%#llx a3=%#llx ret=%#llx ret2=%#llx",
                         rd, e->wake, e->num, pcbuf, (unsigned long long)e->a0, (unsigned long long)e->a1,
                         (unsigned long long)e->a2, (unsigned long long)e->a3,
                         (unsigned long long)e->ret, (unsigned long long)e->ret2);
            break;
        case LDR_K_PRE: {
            {
                int k = -1;
                for (int j = 0; j < np; j++) if (pnd[j].addr == e->ret) { k = j; break; }   /* one pending slot per address */
                if (k < 0 && np < 4) k = np++;
                if (k < 0) k = 3;                                                            /* table full: reuse the last slot */
                pnd[k].addr = e->ret; pnd[k].ns = e->ns; pnd[k].type = (int)(e->num & 0xff);
                pnd[k].rdf = (int)(e->num >> 8); pnd[k].val = (int)e->ret2; pnd[k].cur = (int)(uint32_t)e->lr;
                for (int j = 0; j < 4; j++) pnd[k].w[j] = (j == 0 ? e->a0 : j == 1 ? e->a1 : j == 2 ? e->a2 : e->a3);
            }
            break; }
        case LDR_K_WOKE:
            vk_boot_logf("[ldr] objaddr w=%u a=%#llx what=wake-at-signal words=[%#x,%#x,%#x,%#x] was_waiting=(wk-or-lr=%#llx) rdfail=%d",
                         e->wake, (unsigned long long)e->ret, (unsigned)e->a0, (unsigned)e->a1,
                         (unsigned)e->a2, (unsigned)e->a3, (unsigned long long)e->lr, (int)(e->num >> 16));
            break;
        case LDR_K_POST: {
            int k = -1;
            for (int j = 0; j < np; j++) if (pnd[j].addr == e->ret) { k = j; break; }
            if (k >= 0) {
                vk_boot_logf("[ldr] objaddr w=%u a=%#llx before=[%#x,%#x,%#x,%#x] after=[%#x,%#x,%#x,%#x] expect=(type=%d val=%d cur_at_wait_entry=%#x) ret=%lld wait_ms=%.0f rdfail=%d/%d changed=%d",
                             e->wake, (unsigned long long)e->ret,
                             (unsigned)pnd[k].w[0], (unsigned)pnd[k].w[1], (unsigned)pnd[k].w[2], (unsigned)pnd[k].w[3],
                             (unsigned)e->a0, (unsigned)e->a1, (unsigned)e->a2, (unsigned)e->a3,
                             pnd[k].type, pnd[k].val, pnd[k].cur, (long long)e->ret2,
                             e->ns > pnd[k].ns ? (double)(e->ns - pnd[k].ns) / 1e6 : 0.0,
                             pnd[k].rdf, (int)(e->num >> 16),
                             (pnd[k].w[0] != (uint64_t)(uint32_t)e->a0 || pnd[k].w[1] != (uint64_t)(uint32_t)e->a1 ||
                              pnd[k].w[2] != (uint64_t)(uint32_t)e->a2 || pnd[k].w[3] != (uint64_t)(uint32_t)e->a3) ? 1 : 0);
                np--; pnd[k] = pnd[np];
            } else {
                vk_boot_logf("[ldr] objaddr w=%u a=%#llx what=wake after=[%#x,%#x,%#x,%#x] signer=%#x rdfail=%d",
                             e->wake, (unsigned long long)e->ret, (unsigned)e->a0, (unsigned)e->a1,
                             (unsigned)e->a2, (unsigned)e->a3, e->num & 0xffffu, (int)(e->num >> 16));
            }
            break; }
        case LDR_K_LOOP: {
            unsigned c[8];
            for (int j = 0; j < 4; j++) { c[j] = (unsigned)((e->a0 >> (16 * j)) & 0xffff); c[4 + j] = (unsigned)((e->a1 >> (16 * j)) & 0xffff); }
            vk_boot_logf("[ldr] loop w=%u svc=%u dur_ms=%.1f cats=ipc%u,whand%u,waddr%u,wcond%u,ev%u,thr%u,mem%u,other%u top1=%#x:%u top2=%#x:%u distinct=%u",
                         e->wake, e->num, (double)e->ret / 1e6, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
                         (unsigned)(e->a2 >> 32), (unsigned)(e->a2 & 0xffffffffu),
                         (unsigned)(e->a3 >> 32), (unsigned)(e->a3 & 0xffffffffu), (unsigned)e->ret2);
            break; }
        default: break;
        }
    }
    for (int j = 0; j < np; j++)   /* PRE with no POST yet: the wait is still in progress */
        vk_boot_logf("[ldr] objaddr a=%#llx what=still-waiting before=[%#x,%#x,%#x,%#x] expect=(type=%d val=%d cur_at_wait_entry=%#x) age_ms=%.0f rdfail=%d",
                     (unsigned long long)pnd[j].addr, (unsigned)pnd[j].w[0], (unsigned)pnd[j].w[1],
                     (unsigned)pnd[j].w[2], (unsigned)pnd[j].w[3], pnd[j].type, pnd[j].val, pnd[j].cur,
                     (double)(now_ns() - pnd[j].ns) / 1e6, pnd[j].rdf);
    vk_boot_logf("[ldr] h1024 t=%us kind=%d(1=thread,2=event_r,3=event_w,0=none) state=%d calls_sampled=%u",
                 tsec, g_p1024_kind, g_p1024_sig, g_p1024_n);
    vk_boot_logf("[ldr] ring t=%us head=%u drop=%u wakes=%u caps=%u wins=%u win_svc=%llu open=%d",
                 tsec, head, atomic_load_explicit(&g_ldr_drop, memory_order_relaxed),
                 atomic_load_explicit(&g_ldr_wakes, memory_order_relaxed),
                 atomic_load_explicit(&g_ldr_caps, memory_order_relaxed),
                 atomic_load_explicit(&g_ldr_wins, memory_order_relaxed),
                 (unsigned long long)g_ldr_win_svc, g_ldr_win_ns ? 1 : 0);
}

/* ---------- M6k: drain of the 0x102f rings and the wait/signal lines (heartbeat thread only) ----------
   Formatting and file I/O happen here, never on the SVC path. Every counter printed is a plain
   observation; nothing decides on it. [t102f] k= rows are the thread's SVC trail (one per SVC while
   the 30 s budget lasts), hist= is the SVC histogram with the caller LRs of the busiest classes. */
static void m6k_drain(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    static unsigned rd;
    unsigned head = atomic_load_explicit(&g_m6k_head, memory_order_acquire);
    if (head > M6K_RING) head = M6K_RING;
    uint64_t now = now_ns();
    char pc[64];
    unsigned nl = 0;
    for (; rd < head; rd++) {
        m6k_ent_t *e = &g_m6k_ring[rd];
        if (!atomic_load_explicit(&e->seq, memory_order_acquire)) break;
        pc[0] = 0; runtime_describe_address(e->lr, pc, sizeof pc);
        vk_boot_logf("[t102f] k=%u svc=%#x pc=%s prev=%#x a0=%#llx a1=%#llx a2=%#llx a3=%#llx ret=%#llx ret2=%#llx age=%.1fs",
                     rd, e->num, pc[0] ? pc : "-", e->prev,
                     (unsigned long long)e->a0, (unsigned long long)e->a1,
                     (unsigned long long)e->a2, (unsigned long long)e->a3,
                     (unsigned long long)e->ret, (unsigned long long)e->ret2,
                     e->ns && e->ns <= now ? (double)(now - e->ns) / 1e9 : -1.0);
        nl++;
    }
    /* histogram: total + the six busiest classes, then the caller LRs of the three busiest */
    uint64_t tot = 0; for (int i = 0; i < 256; i++) tot += g_m6k_hist[i];
    static uint64_t ptot;
    int top6[6] = { -1, -1, -1, -1, -1, -1 };
    for (int pass = 0; pass < 6; pass++) {
        int best = -1;
        for (int i = 0; i < 256; i++) {
            if (!g_m6k_hist[i]) continue;
            int used = 0; for (int k = 0; k < pass; k++) if (top6[k] == i) used = 1;
            if (used) continue;
            if (best < 0 || g_m6k_hist[i] > g_m6k_hist[best]) best = i;
        }
        top6[pass] = best;
    }
    char tl[512]; int to = 0;
    for (int k = 0; k < 6 && top6[k] >= 0; k++)
        to += snprintf(tl + to, sizeof tl - (size_t)to, " %#x:%u", top6[k], g_m6k_hist[top6[k]]);
    vk_boot_logf("[t102f] hist t=%us traced=%u total=%llu(+%llu) drop=%u top:%s",
                 tsec, nl, (unsigned long long)tot, (unsigned long long)(tot - ptot),
                 atomic_load_explicit(&g_m6k_drop, memory_order_relaxed), to ? tl : " none");
    ptot = tot;
    for (int k = 0; k < 3 && top6[k] >= 0; k++) {
        int c = top6[k];
        char b[512]; int o = 0; int shown = 0;
        for (int i = 0; i < M6K_TOP && shown < 3; i++) {
            if (!g_m6k_top[c][i].n) continue;
            char d[64]; runtime_describe_address(g_m6k_top[c][i].lr, d, sizeof d);
            o += snprintf(b + o, sizeof b - (size_t)o, " lr%d=%s:%u", shown + 1, d, g_m6k_top[c][i].n);
            shown++;
        }
        vk_boot_logf("[t102f] hist svc=%#x n=%u%s oth=%u", c, g_m6k_hist[c], o ? b : " lr=-", g_m6k_oth[c]);
    }
    /* signals issued by 0x102f, one line each, with the word before/after (rolling ring: print
       every absolute index newer than the cursor, skipping slots a later signal already reused) */
    {
        static unsigned long srd;
        unsigned long sh = atomic_load_explicit(&g_m6k_sig_head, memory_order_acquire);
        if (sh < srd) srd = sh;
        for (unsigned long i = srd; i < sh; i++) {
            m6k_sig_t *e = &g_m6k_sig[i % M6K_SIG];
            if (e->idx != (uint32_t)i) continue;   /* overwritten before we got here: counted by drop */
            if (!atomic_load_explicit(&e->seq, memory_order_acquire)) continue;
            char d[64]; runtime_describe_address(e->lr, d, sizeof d);
            vk_boot_logf("[t102f] sig obj=%#llx type=%u val=%d prev_svc=%#x lr=%s woke=%d word_before=%#x word_after=%#x rdb=%d rda=%d age=%.1fs",
                         (unsigned long long)e->addr, e->type & 0xffu, e->val, e->prev, d, e->woke,
                         e->wb, e->wa, (int)((e->type >> 8) & 1u), (int)((e->type >> 9) & 1u),
                         e->ns && e->ns <= now ? (double)(now - e->ns) / 1e9 : -1.0);
        }
        srd = sh;
    }
    atomic_store_explicit(&g_m6k_cap, 0, memory_order_relaxed);   /* next 30 s window gets a fresh budget */
}

/* M6k-A2: 0x102f's own wait tuple once per dump, plus the words of the structure it waits in.
   The anchor is chosen only from what the thread already told us: for an address wait the object
   itself is a state word, for a condition wait the state word sits before the CV (0x6ea8bf3ca8 is
   exactly obj-0x80 of 0x102f's CV 0x6ea8bf3d28). The 0x180-byte window is printed on the first dump
   and whenever any word in it changes; otherwise one line says "same". Read-only, mapping-checked. */
static void m6k_wait_dump(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    int locked = 0;
    for (int i = 0; i < 60 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    thr_t *p = NULL;
    for (thr_t *t = g_all; t; t = t->all) if (!t->exited && t->handle == M6K_H) { p = t; break; }
    int wk = -1, op = 0, core = -1, svc = 0, prio = 0, found = p ? 1 : 0;
    uint64_t wa = 0, key = 0, lr = 0, nsvc = 0, seq = 0;
    if (p) { wk = p->wkind; op = p->op; core = p->cur_core; svc = (int)(uint32_t)p->last_svc;
             wa = p->waddr; key = p->wkey; lr = p->last_lr; nsvc = p->nsvc; seq = p->phe_seq; prio = p->prio; }
    if (locked) pthread_mutex_unlock(&G);
    static int pwk = -99; static uint64_t pwa = 0, pkey = 0, plr = 0, pseq = 0, pnsvc = 0;
    static unsigned sn, first_t;
    if (wk == pwk && wa == pwa && key == pkey && lr == plr && seq == pseq) sn++; else { sn = 1; first_t = tsec; }
    pwk = wk; pwa = wa; pkey = key; plr = lr; pseq = seq;
    char wl[64]; runtime_describe_address(lr, wl, sizeof wl);
    vk_boot_logf("[t102f] wait t=%us found=%d wk=%d obj=%#llx key=%#llx op=%d(%s) svc=%#x nsvc=%llu dn=%lld seq=%llu static=%udumps secs=%u core=%d prio=%d lr=%s",
                 tsec, found, wk, (unsigned long long)wa, (unsigned long long)key, op, runtime_op_name(op),
                 svc, (unsigned long long)nsvc, (long long)(nsvc - pnsvc), (unsigned long long)seq,
                 sn, tsec - first_t, core, prio, wl);
    pnsvc = nsvc;
    /* M6k-A3b: the node/key ledger of the object 0x102f waits on. If it is parked in a condition
       wait (ReceiveMessageQueue), the only thing that can release it is a 0x1d on this key; the
       counts below say whether that ever happens, and the first caller names who did it.
       n1c/n1d are the per-key counters already maintained in the two svc cases. */
    if (key) {
        uint64_t n1c = 0, n1d = 0, n1dk = 0, who = 0; int tbl = -1, slot = -1;
        for (int t = 0; t < 3 && slot < 0; t++) {
            unsigned cnt = t == 0 ? g_pkey_n : t == 1 ? g_pkey2_n : g_pkey3_n;
            for (unsigned i = 0; i < cnt; i++) {
                uint64_t k = t == 0 ? g_pkey[i].key : t == 1 ? g_pkey2[i].key : g_pkey3[i].key;
                if (k != key) continue;
                tbl = t; slot = (int)i;
                n1c = t == 0 ? g_pkey[i].n1c : t == 1 ? g_pkey2[i].n1c : g_pkey3[i].n1c;
                n1d = t == 0 ? g_pkey[i].n1d : t == 1 ? g_pkey2[i].n1d : g_pkey3[i].n1d;
                n1dk = t == 0 ? g_pkey[i].n1dk : t == 1 ? g_pkey2[i].n1dk : g_pkey3[i].n1dk;
                who = t == 0 ? g_pkey[i].caller : t == 1 ? g_pkey2[i].caller : g_pkey3[i].caller;
                break;
            }
        }
        static struct { uint64_t key, n1c, n1d, n1dk; int n; } pk[8]; static int pkn;
        int pkidx = -1;
        for (int j = 0; j < pkn; j++) if (pk[j].key == key) { pkidx = j; break; }
        if (pkidx < 0 && pkn < 8) { pkidx = pkn++; pk[pkidx].key = key; }
        long long dc = 0, dd = 0, dk = 0;
        if (pkidx >= 0) {
            if (pk[pkidx].n) { dc = (long long)(n1c - pk[pkidx].n1c); dd = (long long)(n1d - pk[pkidx].n1d); dk = (long long)(n1dk - pk[pkidx].n1dk); }
            pk[pkidx].n1c = n1c; pk[pkidx].n1d = n1d; pk[pkidx].n1dk = n1dk; pk[pkidx].n++;
        }
        vk_boot_logf("[t102f] key t=%us key=%#llx intable=%d(tbl%d) n1c=%llu(+%lld) n1d=%llu(+%lld) n1d_woke=%llu(+%lld) first_sig_h=%#x",
                     tsec, (unsigned long long)key, slot >= 0 ? 1 : 0, tbl,
                     (unsigned long long)n1c, dc, (unsigned long long)n1d, dd, (unsigned long long)n1dk, dk, (unsigned)who);
    }
    /* words around the object: CV-0x100 .. CV+0x200 (192 words). Widened after boot A: one object
       keeps its state word at CV-0x80 and another at CV+0xB8, so one window must cover both. */
    if (wa < 0x1000) return;
    enum { M6K_W = 192 };
    static uint32_t prev[M6K_W]; static int have_prev;
    uint32_t cur[M6K_W]; int rdf = 0;
    uint64_t base = wa - 0x100;
    for (int i = 0; i < M6K_W; i++) { cur[i] = 0; if (!dump_read_u32(base + 4u * (uint64_t)i, &cur[i])) rdf++; }
    int chg = 0, firstchg = -1;
    for (int i = 0; i < M6K_W; i++) if (!have_prev || cur[i] != prev[i]) { if (firstchg < 0) firstchg = i; chg++; }
    if (chg || !have_prev) {
        for (int part = 0; part < 3; part++) {
            char b[700]; int o = 0;
            for (int i = part * 64; i < part * 64 + 64; i++) {
                uint32_t pv = have_prev ? prev[i] : 0;
                o += snprintf(b + o, sizeof b - (size_t)o, " %+d:%x%s", i * 4 - 0x100, cur[i],
                              (have_prev && cur[i] != pv) ? "!" : "");
            }
            vk_boot_logf("[t102f] obj t=%us base=%#llx part=%d/%d chg=%d firstchg=%+d rdfail=%d words:%s",
                         tsec, (unsigned long long)base, part + 1, 3, chg,
                         firstchg < 0 ? 0 : firstchg * 4 - 0x100, rdf, b);
        }
        if (have_prev) {
            char b[512]; int o = 0;
            for (int i = 0; i < M6K_W && o < (int)sizeof b - 48; i++)
                if (cur[i] != prev[i])
                    o += snprintf(b + o, sizeof b - (size_t)o, " %+d:%x->%x", i * 4 - 0x100, prev[i], cur[i]);
            vk_boot_logf("[t102f] objdiff t=%us%s", tsec, o ? b : " none");
        }
        for (int i = 0; i < M6K_W; i++) prev[i] = cur[i];
        have_prev = 1;
    } else {
        vk_boot_logf("[t102f] obj t=%us base=%#llx same=1 (no word changed in %u dumps)", tsec,
                     (unsigned long long)base, sn);
    }
    /* which of the 11 tracked flag words fall in this window, and what they hold now */
    {
        char b[512]; int o = 0;
        for (unsigned i = 0; i < g_sigaddr_n; i++) {
            uint64_t a = g_sigaddr[i].addr;
            if (a < base || a >= base + 4u * (uint64_t)M6K_W) continue;
            uint32_t v = 0; int ok = dump_read_u32(a, &v);
            o += snprintf(b + o, sizeof b - (size_t)o, " [%+d]=%#x%s", (int)((int64_t)a - (int64_t)base), v, ok ? "" : "?");
        }
        vk_boot_logf("[t102f] inflag t=%us%s", tsec, o ? b : " none-in-window");
    }
}

/* ---------- M6l: the poster / mover report (heartbeat thread only, no SVC-path cost) ----------
   Per dump, for each role (load = 0x1007, clip = 0x102f):
     [lp] poster   totals of the role's keys with the per-dump delta, the role thread's wait state,
                   how many consecutive dumps the role saw zero 0x1C/0x1D, and the age of the last one
     [lp] postkey  per key: waits / signals / woken, first and last signaller, top-3 (thread,LR)
     [lp] last8    the newest eight events on the key that carries most of the traffic
     [lp] obj      words around the lock the role thread waits on, printed when any word changes
   and for each observed thread (main, loader, both sleep loops, audio):
     [lp] mover    per-class SVC delta with the class's last LR, plus the last SleepThread argument
   Every number is a plain read of a counter filled on the SVC path; nothing here decides. */
/* M6m: dump side of the poster ledger (30 s cadence). Resolves the load thread (most open+read I/O),
   the top signaller of the load key (= the poster) and the poster's own key, then prints the ledger.
   Decides nothing; every judgement is left to the reader. */
static void ps_dump(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    struct act_t { uint32_t h; int found, wk, exited; uint64_t wkey, waddr, lr, nsvc, entry, open, rd;
                   long long dnsvc; unsigned s8i, s8t; ps_svc8_t s8[8]; };
    struct act_t ld, po;
    static uint64_t pld_nsvc, ppo_nsvc;
    static unsigned dzero, ndumps;
    memset(&ld, 0, sizeof ld); memset(&po, 0, sizeof po);
    ld.h = g_ps.load_h; po.h = g_ps.poster_h; ld.wk = po.wk = -1;
    g_ps_dno++; ndumps++;
    int locked = 0;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    /* (1) the load thread: the live thread with the most open+read I/O -- never a handle number.
       M6n: the argmax must be the same in two consecutive dumps before it is believed. M6m resolved
       once, on the first dump, and boot B picked main (0x1001) there because main still led early I/O
       (204) while the loader had not yet done its work. A single dump is not a measurement. */
    if (!g_ps.load_h) {
        static uint32_t cand_h; static unsigned cand_n;
        uint64_t best = 0; uint32_t bh = 0;
        for (thr_t *t = g_all; t; t = t->all) {
            if (t->exited) continue;
            uint64_t io = atomic_load_explicit(&t->io_open, memory_order_relaxed) +
                          atomic_load_explicit(&t->io_read, memory_order_relaxed);
            if (io >= 32 && io > best) { best = io; bh = t->handle; }
        }
        if (bh && bh == cand_h) cand_n++; else { cand_h = bh; cand_n = bh ? 1u : 0u; }
        if (bh && cand_n >= 2u) {
            g_ps.load_h = bh;
            vk_boot_logf("[ps] resolve t=%us load_h=%#x (open+read=%llu, same argmax in %u consecutive dumps) "
                         "load_key=%#llx poster=pending",
                         tsec, g_ps.load_h, (unsigned long long)best, cand_n,
                         (unsigned long long)g_ps.load_key);
        } else if (bh) {
            vk_boot_logf("[ps] candidate t=%us load_cand=%#x (open+read=%llu, dump %u of 2 required)",
                         tsec, bh, (unsigned long long)best, cand_n);
        }
        ld.h = g_ps.load_h;
    }
    /* (2) the poster: the load key's top signaller, once the choice is unambiguous (>= 8 signals) */
    if (g_ps.load_key && !g_ps.poster_h) {
        int bi = -1; uint64_t bn = 0;
        for (int i = 0; i < PS_SIG; i++) if (g_ps.sig[i].n > bn) { bn = g_ps.sig[i].n; bi = i; }
        if (bi >= 0 && bn >= 8) {
            g_ps.poster_h = g_ps.sig[bi].h; po.h = g_ps.poster_h;
            vk_boot_logf("[ps] resolve t=%us poster=%#x (top signaller of load_key=%#llx, n=%llu of %llu)",
                         tsec, g_ps.poster_h, (unsigned long long)g_ps.load_key, (unsigned long long)bn,
                         (unsigned long long)(bn + g_ps.sig_ovf));
        }
    }
    /* (3) snapshot the two actors; the poster's key comes from where it is parked when step 2 missed it */
    for (thr_t *t = g_all; t; t = t->all) {
        struct act_t *x = NULL;
        if (ld.h && t->handle == ld.h) x = &ld;
        else if (po.h && t->handle == po.h) x = &po;
        if (!x || x->found) continue;
        x->found = 1; x->wk = t->wkind; x->wkey = t->wkey; x->waddr = t->waddr; x->lr = t->last_lr;
        x->nsvc = t->nsvc; x->entry = t->entry; x->exited = t->exited;
        x->open = atomic_load_explicit(&t->io_open, memory_order_relaxed);
        x->rd = atomic_load_explicit(&t->io_read, memory_order_relaxed);
        x->dnsvc = (long long)(t->nsvc - (t->handle == ld.h ? pld_nsvc : ppo_nsvc));
        x->s8i = t->svc8i;
        x->s8t = t->svc8t;
        memcpy(x->s8, t->svc8, sizeof x->s8);
    }
    /* M6n: the delayed resolution above can miss the loader's first 0x1C (before the second dump). If
       that happened and the loader is parked on a condition key now, take that key -- but only after
       six dumps, so the live 0x1C registration always wins when it exists. */
    if (g_ps.load_h && !g_ps.load_key && ndumps >= 6 && ld.found && ld.wk == 2 && ld.wkey) {
        g_ps.load_key = ld.wkey;
        vk_boot_logf("[ps] resolve t=%us load_key=%#llx (fallback: from the loader's parked CV, obj=%#llx, ndumps=%u)",
                     tsec, (unsigned long long)g_ps.load_key, (unsigned long long)ld.waddr, ndumps);
    }
    if (g_ps.poster_h && !g_ps.pkey && po.found && po.wk == 2 && po.wkey) {
        g_ps.pkey = po.wkey;
        vk_boot_logf("[ps] resolve t=%us poster_key=%#llx (from the poster's parked CV, obj=%#llx)",
                     tsec, (unsigned long long)g_ps.pkey, (unsigned long long)po.waddr);
    }
    if (locked) pthread_mutex_unlock(&G);
    pld_nsvc = ld.nsvc; ppo_nsvc = po.nsvc;
    /* dzero = consecutive dumps that saw no new 0x1C and no new 0x1D on the poster key */
    { static uint64_t pn1c, pn1d;
      if (g_ps.pkey && g_ps.n1c == pn1c && g_ps.n1d == pn1d) dzero++; else dzero = 0;
      pn1c = g_ps.n1c; pn1d = g_ps.n1d; }
    {
        char lr[64], en[64], f[64], l[64], b[1024]; int o = 0;
        lr[0] = en[0] = f[0] = l[0] = 0;
        if (po.lr) runtime_describe_address(po.lr, lr, sizeof lr);
        if (po.entry) runtime_describe_address(po.entry, en, sizeof en);
        if (g_ps.have_first) runtime_describe_address(g_ps.first_lr, f, sizeof f);
        if (g_ps.last_lr) runtime_describe_address(g_ps.last_lr, l, sizeof l);
        o += snprintf(b + o, sizeof b - (size_t)o,
                      "[ps] poster t=%us h=%#x found=%d load_h=%#x load_key=%#llx pkey=%#llx "
                      "n1c=%llu n1d=%llu n1dk=%llu chg=%llu",
                      tsec, g_ps.poster_h, po.found, g_ps.load_h, (unsigned long long)g_ps.load_key,
                      (unsigned long long)g_ps.pkey, (unsigned long long)g_ps.n1c,
                      (unsigned long long)g_ps.n1d, (unsigned long long)g_ps.n1dk,
                      (unsigned long long)g_ps.pkey_chg);
        o += snprintf(b + o, sizeof b - (size_t)o, " first_by=%s last_by=%s",
                      g_ps.have_first ? f : "-", g_ps.last_lr ? l : "-");
        if (g_ps.last_lr) o += snprintf(b + o, sizeof b - (size_t)o, "@~%us", (g_ps_dno - g_ps.last_k) * 30u);
        o += snprintf(b + o, sizeof b - (size_t)o,
                      " dzero=%u dumps=%u wk=%d obj=%#llx wkey=%#llx on_pkey=%d nsvc=%llu dnsvc=%lld open=%llu read=%llu entry=%s lr=%s ex=%d",
                      dzero, ndumps, po.wk, (unsigned long long)po.waddr, (unsigned long long)po.wkey,
                      (g_ps.pkey && po.wkey == g_ps.pkey) ? 1 : 0,
                      (unsigned long long)po.nsvc, po.dnsvc, (unsigned long long)po.open,
                      (unsigned long long)po.rd, en[0] ? en : "-", lr[0] ? lr : "-", po.exited);
        b[(o < (int)sizeof b) ? o : (int)sizeof b - 1] = 0;
        vk_boot_logf("%s", b);
    }
    {   /* who signals which key: load key (who feeds the loader) and poster key (this round's question) */
        char b[1024]; int o = 0;
        o += snprintf(b + o, sizeof b - (size_t)o, "[ps] psig t=%us load_key=%#llx",
                      tsec, (unsigned long long)g_ps.load_key);
        for (int k = 0; k < PS_SIG && o < (int)sizeof b - 96; k++) {
            if (!g_ps.sig[k].n) continue;
            char l3[64]; l3[0] = 0; runtime_describe_address(g_ps.sig[k].lr, l3, sizeof l3);
            o += snprintf(b + o, sizeof b - (size_t)o, " s%d=h%#x/%s:n=%llu", k, g_ps.sig[k].h,
                          l3[0] ? l3 : "-", (unsigned long long)g_ps.sig[k].n);
        }
        if (g_ps.sig_ovf) o += snprintf(b + o, sizeof b - (size_t)o, " ovf=%llu", (unsigned long long)g_ps.sig_ovf);
        o += snprintf(b + o, sizeof b - (size_t)o, " | pkey=%#llx", (unsigned long long)g_ps.pkey);
        for (int k = 0; k < 3 && o < (int)sizeof b - 96; k++) {
            if (!g_ps.psig[k].n) continue;
            char l4[64]; l4[0] = 0; runtime_describe_address(g_ps.psig[k].lr, l4, sizeof l4);
            o += snprintf(b + o, sizeof b - (size_t)o, " s%d=h%#x/%s:n=%llu", k, g_ps.psig[k].h,
                          l4[0] ? l4 : "-", (unsigned long long)g_ps.psig[k].n);
        }
        if (g_ps.oth) o += snprintf(b + o, sizeof b - (size_t)o, " oth=%llu", (unsigned long long)g_ps.oth);
        b[(o < (int)sizeof b) ? o : (int)sizeof b - 1] = 0;
        vk_boot_logf("%s", b);
    }
    {   /* the poster's newest eight SVCs, arguments at entry -- what it was doing when it stopped */
        char b[1024]; int o = 0;
        unsigned n = po.s8t < 8u ? po.s8t : 8u;
        o += snprintf(b + o, sizeof b - (size_t)o, "[ps] inflight t=%us h=%#x n=%u svc8i=%u svc8t=%u",
                      tsec, g_ps.poster_h, n, po.s8i, po.s8t);
        for (unsigned j = 0; j < n && o < (int)sizeof b - 128; j++) {
            unsigned idx = (po.s8i - 1u - j) & 7u;
            char l5[64]; l5[0] = 0;
            if (po.s8[idx].lr) runtime_describe_address(po.s8[idx].lr, l5, sizeof l5);
            o += snprintf(b + o, sizeof b - (size_t)o, " | %#x a0=%#llx a1=%#llx a2=%#llx a3=%#llx lr=%s k=%u",
                          po.s8[idx].num, (unsigned long long)po.s8[idx].a0, (unsigned long long)po.s8[idx].a1,
                          (unsigned long long)po.s8[idx].a2, (unsigned long long)po.s8[idx].a3,
                          l5[0] ? l5 : "-", po.s8[idx].k);
        }
        b[(o < (int)sizeof b) ? o : (int)sizeof b - 1] = 0;
        vk_boot_logf("%s", b);
    }
    {   /* the newest eight events ever seen on the poster key (rolling, so a flood cannot erase them) */
        char b[1024]; int o = 0;
        if (!g_ps.lastn) { vk_boot_logf("[ps] last8 t=%us pkey=%#llx no event yet", tsec, (unsigned long long)g_ps.pkey); return; }
        o += snprintf(b + o, sizeof b - (size_t)o, "[ps] last8 t=%us pkey=%#llx n=%u", tsec,
                      (unsigned long long)g_ps.pkey, g_ps.lastn);
        for (unsigned j = 0; j < g_ps.lastn && o < (int)sizeof b - 128; j++) {
            unsigned idx = (g_ps.lasti - 1u - j) & 7u;
            char l6[64]; l6[0] = 0; runtime_describe_address(g_ps.last[idx].lr, l6, sizeof l6);
            o += snprintf(b + o, sizeof b - (size_t)o, " | k%u %s h%#x/%s woke=%u",
                          g_ps.last[idx].k, g_ps.last[idx].w ? "sig" : "wait", g_ps.last[idx].h,
                          l6[0] ? l6 : "-", g_ps.last[idx].woke);
        }
        b[(o < (int)sizeof b) ? o : (int)sizeof b - 1] = 0;
        vk_boot_logf("%s", b);
    }
}
/* ---------- M6n: the whole-process wait graph (dump side, 30 s cadence) ----------
   Per dump:
     [wg]    one row per live thread: handle, role, wait kind, obj/key, the guest LR of the SVC that put
             it there, SVC count and its delta, the newest eight SVC numbers, the guest entry point.
             A thread that is running (wk=0) prints obj=key=0: it holds no wait object at this instant.
     [wg] key  one row per distinct condition key that has waiters: waiters and their handles, the
             signal total from the ledger, who signalled last (handle + guest LR) and how long ago,
             whether that signaller is running / parked and where, a verdict word, and the last-signaller
             chain (following "who last signalled the key I am parked on" up to four hops).
     [wg] sum  the counts behind the verdicts, plus the ledger's own health (slots used, signal overflow).
   Verdict words: ZERO = waiters exist and the key was never signalled in this boot; RING = the chain
   returns to a key already on it; RUN = the last signaller is running right now (no stall on this key);
   PARKED = the last signaller is itself parked and the chain does not close; GONE = the last signaller
   no longer exists; UNK = the ledger was full, so "never signalled" cannot be claimed.
   Read-only: the snapshot is taken under the usual bounded trylock and printed after releasing it. */
#define WG_THREADS 96
#define WG_WAITK 40
typedef struct { uint32_t h; int wk, ex; uint64_t wkey, waddr, lr, nsvc, entry; unsigned s8i, s8t; uint8_t s8n[8]; } wg_thr_t;
static int wg_thr_find(const wg_thr_t *v, unsigned n, uint32_t h) {
    for (unsigned i = 0; i < n; i++) if (v[i].h == h) return (int)i;
    return -1;
}
static void wg_dump(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    static wg_thr_t v[WG_THREADS];
    static struct { uint32_t h; uint64_t n; } pv[WG_THREADS];
    static unsigned pvn;
    unsigned n = 0;
    int locked = 0;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    g_wg_dno++;
    for (thr_t *t = g_all; t && n < WG_THREADS; t = t->all) {
        if (t->exited) continue;
        wg_thr_t *x = &v[n];
        x->h = t->handle; x->wk = t->wkind; x->ex = 0;
        x->wkey = t->wkey; x->waddr = t->waddr; x->lr = t->last_lr;
        x->nsvc = t->nsvc; x->entry = t->entry;
        x->s8i = t->svc8i; x->s8t = t->svc8t;
        for (int k = 0; k < 8; k++) x->s8n[k] = (uint8_t)(t->svc8[k].num & 0xffu);
        n++;
    }
    if (locked) pthread_mutex_unlock(&G);
    /* the per-thread rows */
    unsigned npark = 0, nrun = 0;
    for (unsigned i = 0; i < n; i++) {
        wg_thr_t *x = &v[i];
        int is_main = x->h == 0x1001u;
        if (x->wk) npark++; else nrun++;
        if (!x->wk && !is_main) continue;   /* running and not main: nothing to graph (the count is in [wg] sum) */
        long long dns = -1;
        for (unsigned j = 0; j < pvn; j++) if (pv[j].h == x->h) { dns = (long long)(x->nsvc - pv[j].n); break; }
        char lr[64], en[64], s8[64];
        lr[0] = en[0] = s8[0] = 0;
        if (x->lr) runtime_describe_address(x->lr, lr, sizeof lr);
        if (x->entry) runtime_describe_address(x->entry, en, sizeof en);
        int o8 = 0; unsigned have = x->s8t < 8u ? x->s8t : 8u;
        for (unsigned j = 0; j < have && o8 < (int)sizeof s8 - 8; j++) {
            unsigned idx = (x->s8i - 1u - j) & 7u;
            o8 += snprintf(s8 + o8, sizeof s8 - (size_t)o8, "%s%02x", j ? "," : "", x->s8n[idx]);
        }
        const char *role = is_main ? "main" :
                           (g_ps.load_h && x->h == g_ps.load_h) ? "load" :
                           (g_ps.poster_h && x->h == g_ps.poster_h) ? "poster" : "w";
        vk_boot_logf("[wg] t=%us h=%#x role=%s wk=%d obj=%#llx key=%#llx lr=%s nsvc=%llu dnsvc=%lld svc8=%s en=%s",
                     tsec, x->h, role, x->wk,
                     (unsigned long long)(x->wk ? x->waddr : 0),
                     (unsigned long long)(x->wk == 2 ? x->wkey : 0),
                     lr[0] ? lr : "-", (unsigned long long)x->nsvc, dns, s8[0] ? s8 : "-", en[0] ? en : "-");
    }
    pvn = 0;
    for (unsigned i = 0; i < n && pvn < WG_THREADS; i++) { pv[pvn].h = v[i].h; pv[pvn].n = v[i].nsvc; pvn++; }
    /* distinct condition keys with waiters */
    struct { uint64_t key; unsigned n; uint32_t h[4]; uint64_t ns[4]; } wk[WG_WAITK];
    unsigned nwk = 0, nwait = 0;
    for (unsigned i = 0; i < n; i++) {
        if (v[i].wk != 2 || !v[i].wkey) continue;
        nwait++;
        unsigned k; for (k = 0; k < nwk; k++) if (wk[k].key == v[i].wkey) break;
        if (k == nwk) { if (nwk >= WG_WAITK) continue; wk[nwk].key = v[i].wkey; wk[nwk].n = 0; nwk++; }
        if (wk[k].n < 4) { wk[k].h[wk[k].n] = v[i].h; wk[k].ns[wk[k].n] = v[i].nsvc; }
        wk[k].n++;
    }
    unsigned czero = 0, cring = 0, crun = 0, cpark = 0, cgone = 0, cunk = 0, czero_fresh = 0, czero_used = 0;
    for (unsigned k = 0; k < nwk; k++) {
        wg_key_t *e = wg_key(wk[k].key);
        int cls = 0;                       /* 1 ZERO 2 RING 3 RUN 4 PARKED 5 GONE 6 UNK */
        int bywk = 0; uint64_t bykey = 0;
        char chain[200]; int co = 0; chain[0] = 0;
        if (!e) cls = 6;                   /* waiters exist but the key never got a slot: cannot be judged */
        else if (!e->sig) cls = 1;
        else {
            uint64_t seen[6]; unsigned nseen = 1; seen[0] = wk[k].key;
            uint64_t ck = wk[k].key;
            cls = 4;                       /* a parked chain is the default until something else is proven */
            for (unsigned hop = 0; hop < 4; hop++) {
                wg_key_t *ce = wg_key(ck);
                if (!ce) { cls = 6; break; }
                co += snprintf(chain + co, sizeof chain - (size_t)co, "%sh%#x", co ? ">" : "", ce->last_h);
                if (co > (int)sizeof chain - 24) break;
                int ti = wg_thr_find(v, n, ce->last_h);
                if (ti < 0) { cls = 5; co += snprintf(chain + co, sizeof chain - (size_t)co, ">gone"); break; }
                if (v[ti].wk == 0) { cls = 3; co += snprintf(chain + co, sizeof chain - (size_t)co, ">run"); break; }
                bywk = v[ti].wk; bykey = v[ti].wkey;
                if (v[ti].wk != 2 || !v[ti].wkey) { cls = 4; break; }
                uint64_t nk = v[ti].wkey;
                int dup = 0; for (unsigned s = 0; s < nseen; s++) if (seen[s] == nk) dup = 1;
                if (dup) { cls = 2; co += snprintf(chain + co, sizeof chain - (size_t)co, ">ring"); break; }
                if (nseen < 6) seen[nseen++] = nk;
                ck = nk;
            }
        }
        czero += cls == 1; cring += cls == 2; crun += cls == 3; cpark += cls == 4; cgone += cls == 5; cunk += cls == 6;
        if (cls == 1) {   /* a zero-signal key whose waiters never ran is an unused pool slot, not a stall */
            int fresh = 1; for (unsigned j = 0; j < wk[k].n && j < 4; j++) if (wk[k].ns[j] >= 16u) fresh = 0;
            czero_fresh += fresh; czero_used += !fresh;
        }
        char lb[64]; lb[0] = 0; if (e && e->last_lr) runtime_describe_address(e->last_lr, lb, sizeof lb);
        char ofs[80]; int oo = 0; ofs[0] = 0;
        for (unsigned j = 0; j < wk[k].n && j < 4; j++) oo += snprintf(ofs + oo, sizeof ofs - (size_t)oo, "%sh%#x", j ? "," : "", wk[k].h[j]);
        if (wk[k].n > 4 && oo < (int)sizeof ofs - 16) oo += snprintf(ofs + oo, sizeof ofs - (size_t)oo, ",+%u", wk[k].n - 4);
        char wsv[96]; int wo = 0; wsv[0] = 0;
        for (unsigned j = 0; j < wk[k].n && j < 4; j++)
            wo += snprintf(wsv + wo, sizeof wsv - (size_t)wo, "%sh%#x:%llu", j ? "," : "", wk[k].h[j], (unsigned long long)wk[k].ns[j]);
        char bys[96]; int bo = 0; bys[0] = 0;
        if (cls == 1) bo += snprintf(bys, sizeof bys, "never");
        else if (cls == 3) bo += snprintf(bys, sizeof bys, "run");
        else if (cls == 5) bo += snprintf(bys, sizeof bys, "gone");
        else if (cls == 6) bo += snprintf(bys, sizeof bys, "unk(ledger)");
        else bo += snprintf(bys, sizeof bys, "wk=%d/key=%#llx", bywk, (unsigned long long)bykey);
        vk_boot_logf("[wg] key t=%us key=%#llx waiters=%u of=%s wsvc=%s w=%llu sig=%llu last_by=h%#x/%s last_t=~%us first_by=h%#x@~%us byst=%s cls=%s chain=%s",
                     tsec, (unsigned long long)wk[k].key, wk[k].n, ofs[0] ? ofs : "-", wsv[0] ? wsv : "-",
                     (unsigned long long)(e ? e->n1c : 0), (unsigned long long)(e ? e->sig : 0),
                     e ? e->last_h : 0, lb[0] ? lb : "-",
                     (e && e->last_d && g_wg_dno >= e->last_d) ? (g_wg_dno - e->last_d) * 30u : 0,
                     e ? e->first_h : 0,
                     (e && e->first_d && g_wg_dno >= e->first_d) ? (g_wg_dno - e->first_d) * 30u : 0,
                     bys[0] ? bys : "-",
                     cls == 1 ? "ZERO" : cls == 2 ? "RING" : cls == 3 ? "RUN" : cls == 4 ? "PARKED" : cls == 5 ? "GONE" : "UNK",
                     chain[0] ? chain : "-");
    }
    vk_boot_logf("[wg] sum t=%us threads=%u parked=%u run=%u keys_w=%u waiters=%u zero=%u ring=%u run_ok=%u parked_chain=%u gone=%u unk=%u "
                 "zero_fresh=%u zero_used=%u led_slots=%u led_ovf=%llu untracked=%llu main_sig=%llu",
                 tsec, n, npark, nrun, nwk, nwait, czero, cring, crun, cpark, cgone, cunk, czero_fresh, czero_used,
                 g_wg_slots, (unsigned long long)g_wg_ovf, (unsigned long long)g_wg_untracked,
                 (unsigned long long)g_mn_total);
}
/* ---------- M6n: what main is actually doing (dump side, 30 s cadence) ----------
   [mn] state   wait kind / object / key / LR / core / SVC count + delta
   [mn] svc8    the newest eight SVC numbers with the ring's total, and the arguments of the newest three
   [mn] sig174  main's 0x1D targets: top-8 by count with the last guest LR of each, plus distinct/overflow
   The 0x1D LR is the trampoline (sdk+0xd3f174) on this SDK, so the LR alone cannot name the call site;
   [mn] pc lines from the SIGUSR2 sampler (tagged in spin_sampler) carry main's pc/backtrace instead. */
static void mn_dump(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    struct { int found, wk, core; uint64_t wkey, waddr, lr, nsvc, entry; unsigned s8i, s8t; ps_svc8_t s8[8]; ps_svc8x_t s8x[8]; } m;
    static uint64_t pnsvc; static int have;
    memset(&m, 0, sizeof m); m.core = -9;
    int locked = 0;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->handle != 0x1001u || t->exited) continue;
        m.found = 1; m.wk = t->wkind; m.wkey = t->wkey; m.waddr = t->waddr; m.lr = t->last_lr;
        m.nsvc = t->nsvc; m.entry = t->entry; m.core = t->cur_core;
        m.s8i = t->svc8i; m.s8t = t->svc8t;
        memcpy(m.s8, t->svc8, sizeof m.s8);
        memcpy(m.s8x, t->svc8x, sizeof m.s8x);
        break;
    }
    if (locked) pthread_mutex_unlock(&G);
    if (!m.found) { vk_boot_logf("[mn] t=%us main h=0x1001 not found", tsec); return; }
    char lr[64]; lr[0] = 0; if (m.lr) runtime_describe_address(m.lr, lr, sizeof lr);
    char en[64]; en[0] = 0; if (m.entry) runtime_describe_address(m.entry, en, sizeof en);
    vk_boot_logf("[mn] state t=%us h=0x1001 wk=%d obj=%#llx key=%#llx lr=%s core=%d nsvc=%llu dnsvc=%lld en=%s",
                 tsec, m.wk, (unsigned long long)(m.wk ? m.waddr : 0),
                 (unsigned long long)(m.wk == 2 ? m.wkey : 0), lr[0] ? lr : "-", m.core,
                 (unsigned long long)m.nsvc, have ? (long long)(m.nsvc - pnsvc) : -1LL, en[0] ? en : "-");
    pnsvc = m.nsvc; have = 1;
    {   /* svc8: newest first, exactly min(total,8) entries (defect 7 of M6m) */
        char b[256]; int o = 0; b[0] = 0;
        unsigned n = m.s8t < 8u ? m.s8t : 8u;
        for (unsigned j = 0; j < n && o < (int)sizeof b - 8; j++)
            o += snprintf(b + o, sizeof b - (size_t)o, "%s%02x", j ? "," : "", (unsigned)m.s8[(m.s8i - 1u - j) & 7u].num & 0xffu);
        vk_boot_logf("[mn] svc8 t=%us tot=%u n=%u svc8=%s", tsec, m.s8t, n, b[0] ? b : "-");
        o = 0; b[0] = 0;
        for (unsigned j = 0; j < n && j < 3; j++) {
            unsigned idx = (m.s8i - 1u - j) & 7u; char l5[48]; l5[0] = 0;
            if (m.s8[idx].lr) runtime_describe_address(m.s8[idx].lr, l5, sizeof l5);
            o += snprintf(b + o, sizeof b - (size_t)o, " | %#x a0=%#llx a1=%#llx a2=%#llx a3=%#llx lr=%s",
                          m.s8[idx].num, (unsigned long long)m.s8[idx].a0, (unsigned long long)m.s8[idx].a1,
                          (unsigned long long)m.s8[idx].a2, (unsigned long long)m.s8[idx].a3, l5[0] ? l5 : "-");
        }
        vk_boot_logf("[mn] svc8d t=%us newest-first%s", tsec, b);
    }
    {   /* M6o: for the newest three SVCs of main, the real syscall pc and the frame-pointer chain read at
           entry. This is the answer to "which function is main spinning in" without any signal sampler:
           pc is the svc instruction inside the SDK, bt[0..2] are the return addresses of the frames above
           it, so a chain of sdk+0x32... then main+0x... names both the SDK primitive and the game caller. */
        char b[640]; int o = 0; b[0] = 0;
        unsigned n = m.s8t < 8u ? m.s8t : 8u;
        for (unsigned j = 0; j < n && j < 3 && o < (int)sizeof b - 200; j++) {
            unsigned idx = (m.s8i - 1u - j) & 7u;
            ps_svc8x_t *x = &m.s8x[idx];
            char p[48]; p[0] = 0;
            if (x->pc) {
                char t2[48]; t2[0] = 0;
                {   /* pc is a module OFFSET (gates.tsv column 4), so print it as module+offset directly */
                    size_t k = 0; if (x->mod) for (; x->mod[k] && k < sizeof t2 - 24; k++) t2[k] = x->mod[k];
                    snprintf(t2 + k, sizeof t2 - k, "+%#llx", (unsigned long long)x->pc);
                }
                snprintf(p, sizeof p, "%s", t2);
            }
            char f[3][48]; 
            for (int q = 0; q < 3; q++) { f[q][0] = 0; if (x->bt[q]) runtime_describe_address(x->bt[q], f[q], sizeof f[q]); }
            o += snprintf(b + o, sizeof b - (size_t)o, " | %#x pc=%s bt=%s<%s<%s",
                          x->num, p[0] ? p : "-", f[0][0] ? f[0] : "-", f[1][0] ? f[1] : "-", f[2][0] ? f[2] : "-");
        }
        vk_boot_logf("[mn] svcbt t=%us newest-first%s", tsec, b[0] ? b : " -");
    }
    {   /* main's 0x1D targets, top-8 by count */
        int idx[MN_SIG]; unsigned nx = 0;
        for (unsigned i = 0; i < g_mn_n; i++) if (g_mn_sig[i].n) idx[nx++] = (int)i;
        for (unsigned a = 0; a < nx; a++) for (unsigned bb = a + 1; bb < nx; bb++)
            if (g_mn_sig[idx[bb]].n > g_mn_sig[idx[a]].n) { int t = idx[a]; idx[a] = idx[bb]; idx[bb] = t; }
        char b[768]; int o = 0;
        o += snprintf(b + o, sizeof b - (size_t)o, "[mn] sig174 t=%us total=%llu distinct=%u ovf=%llu",
                      tsec, (unsigned long long)g_mn_total, g_mn_n, (unsigned long long)g_mn_ovf);
        for (unsigned a = 0; a < nx && a < 8 && o < (int)sizeof b - 96; a++) {
            char l5[48]; l5[0] = 0;
            if (g_mn_sig[idx[a]].lr) runtime_describe_address(g_mn_sig[idx[a]].lr, l5, sizeof l5);
            o += snprintf(b + o, sizeof b - (size_t)o, " s%u=%#llx:%llu/%s", a,
                          (unsigned long long)g_mn_sig[idx[a]].key, (unsigned long long)g_mn_sig[idx[a]].n,
                          l5[0] ? l5 : "-");
        }
        b[(o < (int)sizeof b) ? o : (int)sizeof b - 1] = 0;
        vk_boot_logf("%s", b);
    }
}
/* M6o: one snapshot of main (0x1001) for the input-injection probe in input.c: SVC count, last SVC
   number and the current wait state, so every injected press records "did this move main at all".
   Same trylock discipline as the dumps; on a busy lock the caller gets zeros (read as "no data"),
   never a partial mix of two different threads' fields. */
void runtime_main_snap(unsigned long long *nsvc, unsigned *svc, int *wk, unsigned long long *obj) {
    unsigned long long n = 0; unsigned s = 0; int w = -1; unsigned long long o = 0;
    int locked = 0;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    for (thr_t *t = g_all; t; t = t->all)
        if (t->handle == 0x1001u && !t->exited) { n = t->nsvc; s = (unsigned)t->last_svc; w = t->wkind; o = t->waddr; break; }
    if (locked) pthread_mutex_unlock(&G);
    if (nsvc) *nsvc = n; if (svc) *svc = s; if (wk) *wk = w; if (obj) *obj = o;
}
static void lp_dump(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    static struct { uint64_t n1c, n1d, n1dk; unsigned quiet; unsigned long ring_n; } pv[LP_ROLE];
    static uint32_t pmvn[LP_MV][256];
    static uint64_t pmvnsvc[LP_MV];
    struct mvs_t { uint32_t h; int wk, last, found; uint64_t key, lr, nsvc; } ms[LP_MV];
    struct rls_t { uint32_t h; int wk, found; uint64_t key, lock, lr, nsvc; } rs[LP_ROLE];
    g_lp_dno++;
    for (int i = 0; i < LP_MV; i++) { ms[i].h = g_lp_mvh[i]; ms[i].wk = -1; ms[i].last = 0; ms[i].found = 0;
                                      ms[i].key = 0; ms[i].lr = 0; ms[i].nsvc = 0; }
    for (int i = 0; i < LP_ROLE; i++) { rs[i].h = g_lp_roleh[i]; rs[i].wk = -1; rs[i].found = 0;
                                        rs[i].key = 0; rs[i].lock = 0; rs[i].lr = 0; rs[i].nsvc = 0; }
    int locked = 0;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->exited) continue;
        for (int i = 0; i < LP_MV; i++) if (t->handle == ms[i].h) {
            ms[i].found = 1; ms[i].wk = t->wkind; ms[i].last = t->last_svc;
            ms[i].key = t->wkey ? t->wkey : t->waddr; ms[i].lr = t->last_lr; ms[i].nsvc = t->nsvc;
        }
        for (int i = 0; i < LP_ROLE; i++) if (t->handle == rs[i].h) {
            rs[i].found = 1; rs[i].wk = t->wkind; rs[i].key = t->wkey;
            rs[i].lock = t->waddr; rs[i].lr = t->last_lr; rs[i].nsvc = t->nsvc;
        }
    }
    if (locked) pthread_mutex_unlock(&G);
    /* ---- roles ---- */
    for (int role = 0; role < LP_ROLE; role++) {
        const char *rn = g_lp_rolen[role];
        uint64_t n1c = 0, n1d = 0, n1dk = 0;
        for (unsigned i = 0; i < g_lp[role].n; i++) {
            n1c += g_lp[role].k[i].n1c; n1d += g_lp[role].k[i].n1d; n1dk += g_lp[role].k[i].n1dk;
        }
        uint64_t d1c = n1c - pv[role].n1c, d1d = n1d - pv[role].n1d, dk = n1dk - pv[role].n1dk;
        if (d1c || d1d) pv[role].quiet = 0; else pv[role].quiet++;
        unsigned ago = g_lp_dno > g_lp[role].act_k ? g_lp_dno - g_lp[role].act_k : 0;
        char lr[64]; lr[0] = 0; runtime_describe_address(rs[role].lr, lr, sizeof lr);
        char age[40];
        if (n1c + n1d == 0) snprintf(age, sizeof age, "never");
        else snprintf(age, sizeof age, "~%us(%ud dumps ago)", ago * 30u, ago);
        vk_boot_logf("[lp] poster t=%us role=%s thr=%#x found=%d wk=%d key=%#llx lock=%#llx nsvc=%llu keys=%u "
                     "n1c=%llu(+%llu) n1d=%llu(+%llu) n1dk=%llu(+%llu) ovf=%llu last_act=%s quiet=%udumps lr=%s",
                     tsec, rn, rs[role].h, rs[role].found, rs[role].wk,
                     (unsigned long long)rs[role].key, (unsigned long long)rs[role].lock,
                     (unsigned long long)rs[role].nsvc, g_lp[role].n,
                     (unsigned long long)n1c, (unsigned long long)d1c,
                     (unsigned long long)n1d, (unsigned long long)d1d,
                     (unsigned long long)n1dk, (unsigned long long)dk,
                     (unsigned long long)g_lp[role].ovf, age, pv[role].quiet, lr[0] ? lr : "-");
        pv[role].n1c = n1c; pv[role].n1d = n1d; pv[role].n1dk = n1dk;
        /* per key: totals, first/last signaller, exact top-3 (thread,LR) split */
        for (unsigned i = 0; i < g_lp[role].n; i++) {
            lp_key_t *e = &g_lp[role].k[i];
            char b[1024]; int o = 0;
            o += snprintf(b + o, sizeof b - (size_t)o, "[lp] postkey role=%s key=%#llx n1c=%llu n1d=%llu n1dk=%llu",
                          rn, (unsigned long long)e->key, (unsigned long long)e->n1c,
                          (unsigned long long)e->n1d, (unsigned long long)e->n1dk);
            if (e->have_first) {
                char f[64]; f[0] = 0; runtime_describe_address(e->first_lr, f, sizeof f);
                o += snprintf(b + o, sizeof b - (size_t)o, " first=h%#x/%s@~%us",
                              e->first_h, f[0] ? f : "-", (g_lp_dno - e->first_k) * 30u);
                char l2[64]; l2[0] = 0; runtime_describe_address(e->last_lr, l2, sizeof l2);
                o += snprintf(b + o, sizeof b - (size_t)o, " last=h%#x/%s@~%us",
                              e->last_h, l2[0] ? l2 : "-", (g_lp_dno - e->last_k) * 30u);
            }
            for (int k = 0; k < LP_SIG && o < (int)sizeof b - 96; k++) {
                if (!e->sig[k].n) continue;
                char l3[64]; l3[0] = 0; runtime_describe_address(e->sig[k].lr, l3, sizeof l3);
                o += snprintf(b + o, sizeof b - (size_t)o, " by%d=h%#x/%s:n=%llu", k,
                              e->sig[k].h, l3[0] ? l3 : "-", (unsigned long long)e->sig[k].n);
            }
            if (e->oth && o < (int)sizeof b - 40)
                o += snprintf(b + o, sizeof b - (size_t)o, " oth=%llu", (unsigned long long)e->oth);
            b[(o < (int)sizeof b) ? o : (int)sizeof b - 1] = 0;
            vk_boot_logf("%s", b);
            /* the newest eight events ever seen on this key (rolling, so a flood cannot erase them) */
            if (e->lastn) {
                int o2 = 0; char c[1024];
                o2 += snprintf(c + o2, sizeof c - (size_t)o2, "[lp] last8 role=%s key=%#llx n=%u",
                               rn, (unsigned long long)e->key, e->lastn);
                for (unsigned j = 0; j < e->lastn && o2 < (int)sizeof c - 96; j++) {
                    unsigned idx = (e->lasti - 1u - j) & (LP_LAST - 1u);
                    char l4[64]; l4[0] = 0; runtime_describe_address(e->last[idx].lr, l4, sizeof l4);
                    o2 += snprintf(c + o2, sizeof c - (size_t)o2, " | k%d %s h%#x/%s n=%u a=%#llx",
                                   e->last[idx].k, e->last[idx].w ? "sig" : "wait",
                                   e->last[idx].h, l4[0] ? l4 : "-", e->last[idx].n,
                                   (unsigned long long)e->last[idx].addr);
                }
                c[(o2 < (int)sizeof c) ? o2 : (int)sizeof c - 1] = 0;
                vk_boot_logf("%s", c);
            }
        }
        /* events on role keys inside this window (ring delta) and the newest four of them */
        {
            unsigned long rn2 = g_lp[role].ring_n;
            unsigned long dn = rn2 - pv[role].ring_n;
            pv[role].ring_n = rn2;
            vk_boot_logf("[lp] ring t=%us role=%s new=%lu (of %lu total, %u slots)",
                         tsec, rn, dn, rn2, (unsigned)LP_RING);
            if (dn) {
                int o = 0; char c[1024];
                o += snprintf(c + o, sizeof c - (size_t)o, "[lp] ringnew t=%us role=%s", tsec, rn);
                for (unsigned long j = 1; j <= dn && j <= 4 && o < (int)sizeof c - 96; j++) {
                    unsigned idx = (g_lp[role].ringi - j) & (LP_RING - 1u);
                    char l5[64]; l5[0] = 0; runtime_describe_address(g_lp[role].ring[idx].lr, l5, sizeof l5);
                    o += snprintf(c + o, sizeof c - (size_t)o, " | k%d %s h%#x/%s n=%u a=%#llx",
                                  g_lp[role].ring[idx].k, g_lp[role].ring[idx].w ? "sig" : "wait",
                                  g_lp[role].ring[idx].h, l5[0] ? l5 : "-", g_lp[role].ring[idx].n,
                                  (unsigned long long)g_lp[role].ring[idx].addr);
                }
                c[(o < (int)sizeof c) ? o : (int)sizeof c - 1] = 0;
                vk_boot_logf("%s", c);
            }
        }
        /* the words around the lock, so "queue empty" and "queue full but nobody wakes" differ */
        if (rs[role].lock) {
            static uint32_t pw2[LP_ROLE][25]; static uint64_t pbase[LP_ROLE]; static int phave[LP_ROLE];
            uint64_t base = rs[role].lock - 0x40u;
            uint32_t w[25]; int rdf = 0;
            for (int i = 0; i < 25; i++) { w[i] = 0; if (!dump_read_u32(base + 4u * (uint64_t)i, &w[i])) rdf++; }
            int chg = 0;
            if (phave[role] && pbase[role] == base) { for (int i = 0; i < 25; i++) if (w[i] != pw2[role][i]) chg++; }
            else chg = -1;
            char c[768]; int o = 0;
            o += snprintf(c + o, sizeof c - (size_t)o, "[lp] obj t=%us role=%s base=%#llx rdf=%d chg=%d",
                          tsec, rn, (unsigned long long)base, rdf, chg);
            for (int i = 0; i < 25 && o < (int)sizeof c - 20; i++) {
                int ch = phave[role] && pbase[role] == base ? (w[i] != pw2[role][i]) : (w[i] != 0);
                if (ch || (chg < 0 && w[i])) o += snprintf(c + o, sizeof c - (size_t)o, " [%+d]=%#x", i * 4 - 0x40, w[i]);
            }
            if (rdf) o += snprintf(c + o, sizeof c - (size_t)o, " (unreadable=%d)", rdf);
            c[(o < (int)sizeof c) ? o : (int)sizeof c - 1] = 0;
            vk_boot_logf("%s", c);
            for (int i = 0; i < 25; i++) pw2[role][i] = w[i];
            pbase[role] = base; phave[role] = 1;
        }
    }
    /* ---- movers: per-class SVC delta with the class's last LR, and the last SleepThread argument ---- */
    for (int i = 0; i < LP_MV; i++) {
        unsigned d[256]; int top[3] = { -1, -1, -1 };
        for (int c = 0; c < 256; c++) {
            uint32_t now = g_lp_mvn[i][c];
            d[c] = now - pmvn[i][c];
            pmvn[i][c] = now;
            if (!d[c]) continue;
            for (int k = 0; k < 3; k++) {
                if (top[k] < 0) { top[k] = c; break; }
                if (d[c] > d[top[k]]) { for (int j = 2; j > k; j--) top[j] = top[j - 1]; top[k] = c; break; }
            }
        }
        char b[768]; int o = 0;
        char lr[64]; lr[0] = 0; runtime_describe_address(ms[i].lr, lr, sizeof lr);
        o += snprintf(b + o, sizeof b - (size_t)o,
                      "[lp] mover t=%us h=%#x found=%d wk=%d key=%#llx nsvc=%llu(+%llu) last=%#x lr=%s",
                      tsec, ms[i].h, ms[i].found, ms[i].wk, (unsigned long long)ms[i].key,
                      (unsigned long long)ms[i].nsvc, (unsigned long long)(ms[i].nsvc - pmvnsvc[i]),
                      (unsigned)ms[i].last, lr[0] ? lr : "-");
        pmvnsvc[i] = ms[i].nsvc;
        for (int k = 0; k < 3 && top[k] >= 0; k++) {
            char l2[64]; l2[0] = 0; runtime_describe_address(g_lp_mvlr[i][top[k]], l2, sizeof l2);
            o += snprintf(b + o, sizeof b - (size_t)o, " | %#x:+%u/%u lr=%s", top[k], d[top[k]],
                          g_lp_mvn[i][top[k]], l2[0] ? l2 : "-");
        }
        {
            int64_t a0 = (int64_t)g_lp_mv0ba[i];
            char l3[64]; l3[0] = 0; runtime_describe_address(g_lp_mv0blr[i], l3, sizeof l3);
            if (a0 > 0) o += snprintf(b + o, sizeof b - (size_t)o, " | slp0b=%u a0=%lldns lr=%s",
                                      g_lp_mv0bn[i], (long long)a0, l3[0] ? l3 : "-");
            else o += snprintf(b + o, sizeof b - (size_t)o, " | slp0b=%u a0=%lld(yield) lr=%s",
                               g_lp_mv0bn[i], (long long)a0, l3[0] ? l3 : "-");
        }
        b[(o < (int)sizeof b) ? o : (int)sizeof b - 1] = 0;
        vk_boot_logf("%s", b);
    }
}

/* ---------- M6h: event pair / write-end / owner-thread report (once per 30 s dump) ----------
   Runs on the heartbeat thread (host), so libc I/O is safe here; the SVC side only filled plain
   arrays. Three line families, matching the task's A1/A2/A3:
     [ev] pair  every CreateEvent/service event creation: rd, wr, creator thread, creator LR
     [ev] txt   the ledger for the event whose read end is g_trk_rd (0x1024): write-end handle, whether
                that slot still holds the same event, signals ever delivered, last actor + time, close
                time/actor of either end, and the top-3 signallers
     [own]      one row per owner candidate thread (creator / waiter / signaler / main): state, wchan,
                utime per second, top SVC, wait object, how many dumps its wait tuple is unchanged, LR
   Read-only. */
static void evown_task(int tid, char *st, char *wch, size_t wcap, unsigned long long *u, unsigned long long *s, long *hz)
{
    char p[64], b[512];
    wch[0] = 0; *st = '?'; *u = 0; *s = 0;
    if (*hz <= 0) *hz = sysconf(_SC_CLK_TCK); if (*hz <= 0) *hz = 100;
    snprintf(p, sizeof p, "/proc/self/task/%d/wchan", tid);
    FILE *f = fopen(p, "r");
    if (f) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0; char *nl = strchr(b, '\n'); if (nl) *nl = 0;
             snprintf(wch, wcap, "%s", b); }
    snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    f = fopen(p, "r");
    if (f) {
        size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
        char *lp = strrchr(b, ')');
        if (lp && lp[1] == ' ' && lp[2]) *st = lp[2];
        char *q = lp ? lp + 1 : b; int idx = 3, got = 0;
        while (*q) {
            while (*q == ' ') q++;
            if (!*q) break;
            if (idx == 14) *u = strtoull(q, NULL, 10);
            if (idx == 15) { *s = strtoull(q, NULL, 10); got = 1; break; }
            while (*q && *q != ' ') q++;
            idx++;
        }
        (void)got;
    }
}
void runtime_event_forensics(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    unsigned tot = atomic_load_explicit(&g_evpair_n, memory_order_relaxed);
    unsigned end = tot < EVPAIR_N ? tot : EVPAIR_N;
    /* A1: drain the pair table (the un-drained remainder is only the racing tail of the newest call) */
    static unsigned drained;
    while (drained < end) {
        evpair_t *p = &g_evpair[drained];
        if (!atomic_load_explicit(&p->seq, memory_order_acquire)) break;
        char cl[64]; runtime_describe_address(p->lr, cl, sizeof cl);
        vk_boot_logf("[ev] pair t=%us rd=%#x wr=%#x by=%#x src=%d lr=%s", tsec, p->rd, p->wr, p->by, p->src, p->lr ? cl : "-");
        drained++;
    }
    if (tot > EVPAIR_N) vk_boot_logf("[ev] pair t=%us overflow: %u creations beyond the %u-slot table", tsec, tot - EVPAIR_N, (unsigned)EVPAIR_N);
    /* A0b: event handles that went out in an IPC request (a write end leaving the process is the
       "signal source walked away" case the task asks about) */
    { unsigned xt = atomic_load_explicit(&g_evxfer_n, memory_order_relaxed);
      unsigned xe = xt < EVXFER_N ? xt : EVXFER_N; static unsigned xd;
      while (xd < xe) {
          evxfer_t *x = &g_evxfer[xd];
          if (!atomic_load_explicit(&x->seq, memory_order_acquire)) break;
          vk_boot_logf("[ev] xfer t=%us h=%#x dir=%s type=%d sess=%s cmd=%u", tsec, x->h, x->dir ? "move" : "copy", x->type, x->sess, x->cmd);
          xd++;
      }
      if (xt > EVXFER_N) vk_boot_logf("[ev] xfer t=%us overflow: %u beyond %u slots", tsec, xt - EVXFER_N, (unsigned)EVXFER_N);
    }
    /* M6h-B: who closed which session (guest LR at svc 0x16) */
    { unsigned st = atomic_load_explicit(&g_evsclose_n, memory_order_relaxed);
      unsigned se = st < EVSCLOSE_N ? st : EVSCLOSE_N; static unsigned sd;
      while (sd < se) {
          evsclose_t *x = &g_evsclose[sd];
          if (!atomic_load_explicit(&x->seq, memory_order_acquire)) break;
          char cl[64]; runtime_describe_address(x->lr, cl, sizeof cl);
          vk_boot_logf("[ev] sclose t=%us name=%s h=%#x by=%#x at=%llds lr=%s", tsec, x->name, x->h, x->by,
                       (long long)tsec - (long long)((now_ns() - x->ns) / 1000000000ull), x->lr ? cl : "-");
          sd++;
      }
      if (st > EVSCLOSE_N) vk_boot_logf("[ev] sclose t=%us overflow: %u beyond %u slots", tsec, st - EVSCLOSE_N, (unsigned)EVSCLOSE_N);
    }
    /* A2/A3: snapshot of the tracked pair + the owner candidates (under G, printed after) */
    struct { uint32_t h; int found, exited, wk, op, core, prio; int htid; uint64_t waddr, key, lr, nsvc, born, seq; int closed;
             int tsvc; uint64_t tsvn; } own[8];
    int nowner = 0;
    uint32_t wr = 0, cre = 0; int src = -1, rd_live = 0, rd_refs = 0, rd_sig = 0;
    int wrexists = 0, wralive = 0, wrslot_type = -1, wrslot_thr = 0, wrslot_exited = -1; uint64_t wrborn = 0; uint32_t wrslot_cre = 0;
    uint64_t nsig = 0, nrst = 0, last_ns = 0, last_lr = 0, close_ns = 0, close_lr = 0; uint32_t last_thr = 0;
    uint64_t rnsig = 0, rnrst = 0, rclose_ns = 0; uint64_t rdclose_lr = 0;
    struct { uint32_t thr; uint64_t n; } top[3]; int ntop = 0;
    {   int locked = 0;
        for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
        /* authoritative: the live read end's own event object */
        event_t *er = h_peek(g_trk_rd, O_EVENT_R);
        if (er) { rd_live = 1; wr = er->hw; cre = er->cre_thr; src = er->src; rd_refs = er->refs; rd_sig = er->signaled;
                  last_lr = er->cre_lr; wrborn = er->born_ns; }
        else {  /* fall back to the creation record (read end closed or reused) */
            for (unsigned i = 0; i < end; i++) if (g_evpair[i].rd == g_trk_rd) { wr = g_evpair[i].wr; cre = g_evpair[i].by; src = g_evpair[i].src; last_lr = g_evpair[i].lr; wrborn = g_evpair[i].ns; }
        }
        if (wr) {
            unsigned wi = wr - 0x1000;
            if (wi > 0 && wi < MAX_H) {
                wrslot_type = g_h[wi].type; wrslot_cre = (uint32_t)g_h[wi].creator;
                event_t *ew = h_peek(wr, O_EVENT_W);
                wralive = (ew && (!er || ew == er)) ? 1 : 0;
                if (g_h[wi].type == O_THREAD) { thr_t *tw = g_h[wi].obj; if (tw) { wrslot_thr = 1; wrslot_exited = tw->exited; } }
                nsig = (uint64_t)atomic_load_explicit(&g_evcnt[wi][0], memory_order_relaxed);
                last_ns = g_evlast_ns[wi][0]; last_lr = g_evlast_lr[wi][0]; last_thr = g_evlast_thr[wi][0];
                close_ns = g_evclose_ns[wi]; close_lr = g_evclose_lr[wi];
            }
        }
        if (rd_live) { unsigned ri = g_trk_rd - 0x1000;
            rnsig = (uint64_t)atomic_load_explicit(&g_evcnt[ri][0], memory_order_relaxed);
            rnrst = (uint64_t)atomic_load_explicit(&g_evcnt[ri][1], memory_order_relaxed);
            rclose_ns = g_evclose_ns[ri]; rdclose_lr = g_evclose_lr[ri]; }
        /* top-3 signallers of the write end */
        for (int k = 0; k < 3; k++) {
            int best = -1; uint64_t bv = 0;
            for (int i = 0; i < g_evsig_n; i++) { if (g_evsig[i].h != wr) continue;
                int skip = 0; for (int j = 0; j < ntop; j++) if (top[j].thr == g_evsig[i].thr) skip = 1;
                if (!skip && g_evsig[i].n > bv) { bv = g_evsig[i].n; best = i; } }
            if (best < 0) break;
            top[ntop].thr = g_evsig[best].thr; top[ntop].n = g_evsig[best].n; ntop++;
        }
        /* owner candidates: creator, current waiters on either end, top signallers, main */
        unsigned cand[8]; int nc = 0;
        if (cre) cand[nc++] = cre;
        if (0x1001u != cre) cand[nc++] = 0x1001u;
        for (int i = 0; i < ntop && nc < 6; i++) { int dup = 0; for (int j = 0; j < nc; j++) if (cand[j] == top[i].thr) dup = 1; if (!dup) cand[nc++] = top[i].thr; }
        for (thr_t *t = g_all; t && nc < 8; t = t->all) {
            if (t->exited || t->wkind != 4) continue;
            int hit = 0;
            for (int i = 0; i < t->nwh && i < 4; i++) if (t->wh[i] == g_trk_rd || (wr && t->wh[i] == wr)) hit = 1;
            if (!hit) continue;
            int dup = 0; for (int j = 0; j < nc; j++) if (cand[j] == t->handle) dup = 1;
            if (!dup) cand[nc++] = t->handle;
        }
        for (int i = 0; i < nc && nowner < 8; i++) {
            thr_t *t = NULL;
            for (thr_t *q = g_all; q; q = q->all) if (!q->exited && q->handle == cand[i]) { t = q; break; }
            if (!t) { own[nowner].h = cand[i]; own[nowner].found = 0; nowner++; continue; }
            own[nowner].h = t->handle; own[nowner].found = 1; own[nowner].exited = t->exited;
            own[nowner].wk = t->wkind; own[nowner].op = t->op; own[nowner].core = atomic_load_explicit(&t->cur_core, memory_order_relaxed);
            own[nowner].prio = t->prio; own[nowner].htid = t->htid; own[nowner].waddr = t->waddr; own[nowner].key = t->wkey;
            own[nowner].lr = t->last_lr; own[nowner].nsvc = t->nsvc; own[nowner].born = t->born_ns; own[nowner].seq = t->phe_seq;
            own[nowner].closed = t->closed;
            /* M6h: this thread's most frequent SVC since it started (256-slot histogram, filled on the
               guest SVC path). Read under G so the counters cannot move mid-scan. */
            own[nowner].tsvc = -1; own[nowner].tsvn = 0;
            if (t->svch) { int bn = -1; uint16_t bv = 0;
                for (int k = 0; k < 256; k++) if (t->svch[k] > bv) { bv = t->svch[k]; bn = k; }
                own[nowner].tsvc = bn; own[nowner].tsvn = bv; }
            nowner++;
        }
        if (locked) pthread_mutex_unlock(&G);
    }
    /* A2 line: one per dump. at_t is the wall position of the event relative to this dump, so the
       report can quote "signalled/closed at t=X" without a second clock. */
    { char lls[64], cls[64], cts[64];
      runtime_describe_address(last_lr, lls, sizeof lls);
      runtime_describe_address(close_lr, cls, sizeof cls);
      runtime_describe_address(rdclose_lr, cts, sizeof cts);
      char tops[96]; tops[0] = 0;
      for (int i = 0; i < ntop; i++) { size_t off = strlen(tops); snprintf(tops + off, sizeof tops - off, "%s%#x:%llu", i ? "," : "", top[i].thr, (unsigned long long)top[i].n); }
      vk_boot_logf("[ev] txt t=%us rd=%#x wr=%#x src=%d rd_live=%d rd_refs=%d rd_sig=%d rdsig=%llu rdrst=%llu "
                   "wr_alive=%d wr_slot=%d(wr_slot_thread=%d exited=%d) wrborn_at=%llds wrsignals=%llu last_by=%#x last_at=%llds last_lr=%s "
                   "wr_close_at=%llds wr_close_lr=%s rd_close_at=%llds top=%s",
                   tsec, g_trk_rd, wr, src, rd_live, rd_refs, rd_sig, (unsigned long long)rnsig, (unsigned long long)rnrst,
                   wralive, wrslot_type, wrslot_thr, wrslot_exited,
                   wrborn ? (long long)tsec - (long long)((now_ns() - wrborn) / 1000000000ull) : -1LL,
                   (unsigned long long)nsig, last_thr,
                   last_ns ? (long long)tsec - (long long)((now_ns() - last_ns) / 1000000000ull) : -1LL, last_lr ? lls : "-",
                   close_ns ? (long long)tsec - (long long)((now_ns() - close_ns) / 1000000000ull) : -1LL, close_lr ? cls : "-",
                   rclose_ns ? (long long)tsec - (long long)((now_ns() - rclose_ns) / 1000000000ull) : -1LL,
                   tops[0] ? tops : "none");
    }
    /* A3 lines */
    { static struct { uint32_t h; int wk; uint64_t wa, lr, nsvc; unsigned n; } pv[8]; static int pvn;
      static struct { uint32_t h; unsigned long long u, s; } pc[8]; static int pcn;
      static unsigned long long psec_ns; unsigned long long now = now_ns();
      double dt = psec_ns ? (double)(now - psec_ns) / 1e9 : 30.0; psec_ns = now;
      long hz = 0;
      for (int i = 0; i < nowner; i++) {
          if (!own[i].found) { vk_boot_logf("[own] t=%us thr=%#x gone=1 (exited/before it ever ran)", tsec, own[i].h); continue; }
          unsigned nst = 1; long long dns = -1;
          for (int j = 0; j < pvn; j++) if (pv[j].h == own[i].h) {
              dns = (long long)(own[i].nsvc - pv[j].nsvc);
              if (pv[j].wk == own[i].wk && pv[j].wa == own[i].waddr && pv[j].lr == own[i].lr) nst = pv[j].n + 1;
              pv[j].wk = own[i].wk; pv[j].wa = own[i].waddr; pv[j].lr = own[i].lr; pv[j].n = nst; pv[j].nsvc = own[i].nsvc;
              break;
          }
          if (pvn < 8 && dns < 0) { pv[pvn].h = own[i].h; pv[pvn].wk = own[i].wk; pv[pvn].wa = own[i].waddr;
                                    pv[pvn].lr = own[i].lr; pv[pvn].n = nst; pv[pvn].nsvc = own[i].nsvc; pvn++; }
          char st; char wch[40]; unsigned long long u, s;
          evown_task(own[i].htid, &st, wch, sizeof wch, &u, &s, &hz);
          double dus = -1.0; int pc_i = -1;
          for (int j = 0; j < pcn; j++) if (pc[j].h == own[i].h) { pc_i = j; break; }
          if (pc_i >= 0 && dt > 0.2) dus = 1000.0 * (double)(u - pc[pc_i].u) / (double)hz / dt;
          if (pc_i < 0 && pcn < 8) { pc_i = pcn++; pc[pc_i].h = own[i].h; }
          if (pc_i >= 0) { pc[pc_i].u = u; pc[pc_i].s = s; }
          char lrs[64]; runtime_describe_address(own[i].lr, lrs, sizeof lrs);
          const char *role = own[i].h == cre ? "creator" : own[i].h == 0x1001u ? "main" : "waiter/signaler";
          char tps[16]; if (own[i].tsvc >= 0) snprintf(tps, sizeof tps, "0x%x", own[i].tsvc); else snprintf(tps, sizeof tps, "-");
          vk_boot_logf("[own] t=%us thr=%#x role=%s tid=%d state=%c wchan=%s utime_ms=%llu us_per_s=%.0f svc_top=%s(%llu) nsvc=%llu dnsvc=%lld "
                       "wait_obj=%#llx wk=%d op=%d core=%d prio=%d static=%udumps born_at=%llds lr=%s",
                       tsec, own[i].h, role, own[i].htid, st, wch[0] ? wch : "-", u, dus < 0 ? -1.0 : dus,
                       tps, (unsigned long long)own[i].tsvn,
                       (unsigned long long)own[i].nsvc, dns,
                       (unsigned long long)own[i].waddr, own[i].wk, own[i].op, own[i].core, own[i].prio, nst,
                       own[i].born ? (long long)tsec - (long long)((now - own[i].born) / 1000000000ull) : -1LL, lrs);
      }
    }
}
/* ---------- M6i: who really does the I/O, and what that thread waits on (once per dump) ----------
   The main thread was shown (M6h) to be a polling worker bee with no IPC, so the loader must be
   some other thread. These three line families answer it without touching any state:
     [io] thr   one row per live guest thread (top-8 by IPC, plus the host bucket thr=0x0), with the
                open/read/bytes/IPC totals this thread itself incurred, its newest opened path, the
                guest LR of the SVC that was in flight when it last did I/O, its /proc state/wchan,
                and the deltas of the last window (w_open/w_read/w_ipc) so "what did it do in the
                last 30 s" is a number instead of an impression.
     [io] wait  the rank-1 thread by IPC only: the object/kind/LR it is parked on, how many dumps that
                wait tuple has not changed (static=Ndumps) and for how long (secs), which is the
                difference between "waiting on X" and "polling X in a loop".
     [io] svc   that same thread's SVC histogram, top 5 with the total, read under G from its own
                saturating counters.
   Read-only: only atomics/copies under the usual bounded trylock. The per-thread rows are written by
   the owning thread; a torn copy can misprint a number, never change a decision. */
#define IO_ROWS 64
#define IO_WINDOWS 64
/* M6i: row of the per-thread I/O + IPC table (filled under G, printed without it). */
typedef struct io_probe_row {
    uint32_t h, last_svc, iosvc; int tid, wk, prio, ex, ishost;
    uint64_t ipc, open, rd, bytes, lio, llr, wa, key, nsvc, entry;
    uint32_t d_open, d_rd, d_ipc;          /* totals minus the previous dump's, per handle */
    char path[96];
    uint32_t svh[256]; int havesv;   /* M7y: widened with thr_t.svch (u16 -> u32) */
} io_probe_row;
typedef struct { uint32_t h, wk; uint64_t wa, key, llr; unsigned n; uint64_t first_ns; } io_pv_t;
/* One "[io] wait"/"[io] waitipc" line plus that thread's SVC histogram in one place, so both rank-1
   threads (by IPC and by I/O) are described with exactly the same rules. `pv` keeps per-handle how
   many consecutive dumps the wait tuple has not changed - "waiting on X" vs "polling X in a loop". */
static void io_wait_emit(const char *tag, unsigned tsec, const io_probe_row *x, io_pv_t *pv, unsigned *pvn) {
    extern void vk_boot_logf(const char *fmt, ...);
    int pk = -1;
    for (unsigned j = 0; j < *pvn; j++) if (pv[j].h == x->h) { pk = (int)j; break; }
    if (pk >= 0 && pv[pk].wk == (uint32_t)x->wk && pv[pk].wa == x->wa && pv[pk].key == x->key && pv[pk].llr == x->llr) pv[pk].n++;
    else {
        if (pk < 0 && *pvn < 8) pk = (int)(*pvn)++;
        if (pk >= 0) { pv[pk].h = x->h; pv[pk].wk = (uint32_t)x->wk; pv[pk].wa = x->wa; pv[pk].key = x->key;
                       pv[pk].llr = x->llr; pv[pk].n = 1; pv[pk].first_ns = now_ns(); }
    }
    unsigned nd = pk >= 0 ? pv[pk].n : 0;
    double secs = pk >= 0 ? (double)(now_ns() - pv[pk].first_ns) / 1e9 : -1.0;
    char lrs[64]; lrs[0] = 0; if (x->llr) runtime_describe_address(x->llr, lrs, sizeof lrs);
    vk_boot_logf("[io] %s t=%us thr=%#x obj=%#llx wk=%d lr=%s static=%udumps secs=%.1f ipc=%llu read=%llu open=%llu last_path=%s ex=%d tid=%d",
                 tag, tsec, x->h, (unsigned long long)(x->wk == 4 ? x->key : x->wa), x->wk,
                 lrs[0] ? lrs : "-", nd, secs, (unsigned long long)x->ipc, (unsigned long long)x->rd,
                 (unsigned long long)x->open, x->path[0] ? x->path : "-", x->ex, x->tid);
    if (!x->havesv) return;
    int b5[5] = {-1, -1, -1, -1, -1};
    for (int k = 0; k < 256; k++) {
        if (!x->svh[k]) continue;
        for (int s = 0; s < 5; s++)
            if (b5[s] < 0 || x->svh[k] > x->svh[b5[s]]) { for (int q = 4; q > s; q--) b5[q] = b5[q - 1]; b5[s] = k; break; }
    }
    char b[256]; int o = 0;
    for (int s = 0; s < 5 && b5[s] >= 0; s++)
        o += snprintf(b + o, sizeof b - (size_t)o, "%s0x%x:%u", s ? "," : "", b5[s], x->svh[b5[s]]);
    vk_boot_logf("[io] svc t=%us thr=%#x nsvc=%llu top=%s", tsec, x->h, (unsigned long long)x->nsvc, b[0] ? b : "-");
}
void runtime_io_thread_dump(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern void runtime_romfs_io_totals(uint64_t *, uint64_t *, uint64_t *, uint64_t *);
    static io_probe_row r[IO_ROWS], s[IO_ROWS];
    static unsigned prev_open[IO_WINDOWS], prev_rd[IO_WINDOWS], prev_ipc[IO_WINDOWS];
    static uint32_t prev_h[IO_WINDOWS]; static unsigned prev_n;
    static io_pv_t pv_io[8], pv_ipc[8]; static unsigned pvn_io, pvn_ipc;
    int n = 0, locked = 0;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    for (thr_t *t = g_all; t && n < IO_ROWS; t = t->all) {
        io_probe_row *x = &r[n];
        memset(x, 0, sizeof *x);
        /* exited but not yet freed: included, because its numbers are the evidence */
        x->ex = t->exited ? 1 : 0;
        x->h = t->handle; x->tid = t->htid; x->wk = t->wkind; x->prio = t->prio; x->nsvc = t->nsvc;
        x->entry = t->entry;
        x->ipc = atomic_load_explicit(&t->io_ipc, memory_order_relaxed);
        x->open = atomic_load_explicit(&t->io_open, memory_order_relaxed);
        x->rd = atomic_load_explicit(&t->io_read, memory_order_relaxed);
        x->bytes = atomic_load_explicit(&t->io_bytes, memory_order_relaxed);
        x->lio = atomic_load_explicit(&t->io_last_io_ns, memory_order_relaxed);
        x->llr = atomic_load_explicit(&t->io_last_lr, memory_order_relaxed);
        x->iosvc = atomic_load_explicit(&t->io_last_svc, memory_order_relaxed);
        x->last_svc = (uint32_t)(t->last_svc & 0xff);
        x->wa = t->waddr; x->key = t->wkey;
        for (int k = 0; k < 95; k++) { x->path[k] = t->io_path[k]; if (!x->path[k]) break; }
        x->havesv = 0;
        if (t->svch) { for (int k = 0; k < 256; k++) x->svh[k] = t->svch[k]; x->havesv = 1; }
        n++;
    }
    if (locked) pthread_mutex_unlock(&G);
    /* host bucket: I/O performed by a host thread (handle 0) must not vanish from the accounts */
    if (n < IO_ROWS) {
        io_probe_row *x = &r[n];
        memset(x, 0, sizeof *x); x->h = 0; x->tid = -1; x->wk = -1; x->ishost = 1;
        x->ipc = atomic_load_explicit(&g_hio_ipc, memory_order_relaxed);
        x->open = atomic_load_explicit(&g_hio_open, memory_order_relaxed);
        x->rd = atomic_load_explicit(&g_hio_read, memory_order_relaxed);
        x->bytes = atomic_load_explicit(&g_hio_bytes, memory_order_relaxed);
        x->lio = atomic_load_explicit(&g_hio_last_ns, memory_order_relaxed);
        snprintf(x->path, sizeof x->path, "%s", g_hio_path);
        n++;
    }
    /* window deltas, by handle: what each thread did since the previous dump */
    for (int i = 0; i < n; i++) {
        int k = -1;
        for (unsigned j = 0; j < prev_n; j++) if (prev_h[j] == r[i].h) { k = (int)j; break; }
        if (k >= 0 && r[i].open >= prev_open[k] && r[i].rd >= prev_rd[k] && r[i].ipc >= prev_ipc[k]) {
            r[i].d_open = (uint32_t)(r[i].open - prev_open[k]);
            r[i].d_rd = (uint32_t)(r[i].rd - prev_rd[k]);
            r[i].d_ipc = (uint32_t)(r[i].ipc - prev_ipc[k]);
        } else { r[i].d_open = (uint32_t)r[i].open; r[i].d_rd = (uint32_t)r[i].rd; r[i].d_ipc = (uint32_t)r[i].ipc; }   /* a reused handle (or a tiny source): treat as fresh */
    }
    prev_n = 0;
    for (int i = 0; i < n && (int)prev_n < IO_WINDOWS; i++, prev_n++) {
        prev_h[prev_n] = r[i].h; prev_open[prev_n] = (unsigned)r[i].open;
        prev_rd[prev_n] = (unsigned)r[i].rd; prev_ipc[prev_n] = (unsigned)r[i].ipc;
    }
    /* (0) totals: the per-thread rows plus the retired ledger must close against the global romfs
       counters. If they do not, some caller does I/O outside a guest thread and the answer is the
       host bucket row; if they do, the per-thread ranking is complete. */
    uint64_t T_open = 0, T_rd = 0, T_bytes = 0, T_ipc = 0;
    for (int i = 0; i < n; i++) { T_open += r[i].open; T_rd += r[i].rd; T_bytes += r[i].bytes; T_ipc += r[i].ipc; }
    uint64_t ro = 0, rr = 0, rb = 0, rnf = 0;
    runtime_romfs_io_totals(&ro, &rr, &rb, &rnf);
    vk_boot_logf("[io] tot t=%us rows=%d open=%llu read=%llu bytes=%llu ipc=%llu | ret n=%u open=%llu read=%llu bytes=%llu ipc=%llu "
                 "| romfs opens=%llu reads=%llu bytes=%llu notfound=%llu",
                 tsec, n, (unsigned long long)T_open, (unsigned long long)T_rd, (unsigned long long)T_bytes,
                 (unsigned long long)T_ipc,
                 atomic_load_explicit(&g_io_ret_n, memory_order_relaxed),
                 (unsigned long long)atomic_load_explicit(&g_io_ret_open, memory_order_relaxed),
                 (unsigned long long)atomic_load_explicit(&g_io_ret_read, memory_order_relaxed),
                 (unsigned long long)atomic_load_explicit(&g_io_ret_bytes, memory_order_relaxed),
                 (unsigned long long)atomic_load_explicit(&g_io_ret_ipc, memory_order_relaxed),
                 (unsigned long long)ro, (unsigned long long)rr, (unsigned long long)rb, (unsigned long long)rnf);
    if (!n) { vk_boot_logf("[io] thr t=%us no guest thread row", tsec); return; }
    /* (1) top-8 by IPC count, as the task asks, with the /proc state and wchan of each */
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
        if (r[j].ipc > r[i].ipc) { io_probe_row tmp = r[i]; r[i] = r[j]; r[j] = tmp; }
    for (int i = 0; i < n && i < 8; i++) {
        io_probe_row *x = &r[i];
        char st = '-'; char wch[48]; wch[0] = 0;
        if (x->tid > 0) { unsigned long long u, s2; long hz = 0; evown_task(x->tid, &st, wch, sizeof wch, &u, &s2, &hz); }
        char lrs[64]; lrs[0] = 0;
        if (x->llr) runtime_describe_address(x->llr, lrs, sizeof lrs);
        double age = x->lio ? (double)(now_ns() - x->lio) / 1e9 : -1.0;
        vk_boot_logf("[io] thr=%#x ipc=%llu read=%llu open=%llu bytes=%llu last_path=%s last_lr=%s state=%c wchan=%s "
                     "tid=%d rank=%d w_ipc=+%u w_read=+%u w_open=+%u last_io_age=%.1fs last_svc=%#x wait_obj=%#llx wk=%d nsvc=%llu ex=%d",
                     x->h, (unsigned long long)x->ipc, (unsigned long long)x->rd, (unsigned long long)x->open,
                     (unsigned long long)x->bytes, x->path[0] ? x->path : "-", lrs[0] ? lrs : "-",
                     st, wch[0] ? wch : "-", x->tid, i, x->d_ipc, x->d_rd, x->d_open, age, x->iosvc,
                     (unsigned long long)x->wa, x->wk, (unsigned long long)x->nsvc, x->ex);
    }
    /* (2) top-8 by I/O (open+read): the loader is named here even if its IPC count is low or it died */
    memcpy(s, r, sizeof(io_probe_row) * (size_t)n);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
        if (s[j].rd + s[j].open > s[i].rd + s[i].open) { io_probe_row tmp = s[i]; s[i] = s[j]; s[j] = tmp; }
    for (int i = 0; i < n && i < 8; i++) {
        io_probe_row *x = &s[i];
        char lrs[64]; lrs[0] = 0;
        if (x->llr) runtime_describe_address(x->llr, lrs, sizeof lrs);
        char en[64]; en[0] = 0; if (x->entry) runtime_describe_address(x->entry, en, sizeof en);
        double age = x->lio ? (double)(now_ns() - x->lio) / 1e9 : -1.0;
        vk_boot_logf("[io] iotop t=%us thr=%#x rank=%d open=%llu read=%llu bytes=%llu ipc=%llu last_path=%s last_lr=%s "
                     "last_io_age=%.1fs w_open=+%u w_read=+%u w_ipc=+%u tid=%d ex=%d wk=%d entry=%s",
                     tsec, x->h, i, (unsigned long long)x->open, (unsigned long long)x->rd, (unsigned long long)x->bytes,
                     (unsigned long long)x->ipc, x->path[0] ? x->path : "-", lrs[0] ? lrs : "-",
                     age, x->d_open, x->d_rd, x->d_ipc, x->tid, x->ex, x->wk, en[0] ? en : "-");
    }
    /* (3) wait + SVC histogram for the rank-1 thread by I/O, and for the rank-1 by IPC */
    io_wait_emit("wait", tsec, &s[0], pv_io, &pvn_io);
    if (r[0].h != s[0].h) io_wait_emit("waitipc", tsec, &r[0], pv_ipc, &pvn_ipc);
    if (atomic_load_explicit(&g_io_ret_n, memory_order_relaxed)) {   /* a thread that did I/O and ended */
        unsigned rn = atomic_load_explicit(&g_io_ret_n, memory_order_relaxed);
        if (rn > IO_RETIRED) rn = IO_RETIRED;
        for (unsigned i = 0; i < rn; i++) {
            io_ret_t *q = &g_io_ret[i];
            char en[64], lr[64];
            runtime_describe_address(q->entry, en, sizeof en);
            runtime_describe_address(q->llr, lr, sizeof lr);
            vk_boot_logf("[io] ret t=%us slot=%u thr=%#x open=%llu read=%llu bytes=%llu ipc=%llu entry=%s last_lr=%s exited_at=%llds",
                         tsec, i, q->h, (unsigned long long)q->open, (unsigned long long)q->rd,
                         (unsigned long long)q->bytes, (unsigned long long)q->ipc, en, q->llr ? lr : "-",
                         (long long)(q->exit_ns ? (long long)(now_ns() - q->exit_ns) / 1000000000LL : -1));
        }
    }
}
/* ---------- M6q: tick-rate probe + stage table (heartbeat thread only, decides nothing) ----------
   Question the task asks: is the game's own clock wall-clock driven or frame driven?
   Offline answer for the exact word the task names (`global` in `timeout = global*800/1e6` at
   main+0x6253d0): it is NOT a counter.  Its only writer is main+0x96c220, which does
   `bl <import>; *(*(<main+0x1369690>)) = x0`, and the three ticks->ns helpers at
   main+0x96c180 / 0x96c1bc / 0x96c1fc all *divide by* that same word.  The sdk's answer is
   `mov w0,#0x124F800` (= 19200000) for the frequency, and its `mrs x0,CNTPCT_EL0` is REPLACED by
   this port with a host gate: loader.c:359-363 writes `mach_timebase ns * 12/625` (= ns scaled to
   19.2 MHz) into the destination register.  A real `mrs cntpct_el0` from host code is illegal at EL0
   on this device (measured: SIGILL, addr==pc -- see M6q P4), so the probe reproduces the gate's own
   formula instead of executing the instruction.  So the slot holds the tick FREQUENCY (a constant)
   and the game's clock is the host monotonic clock, i.e. the wall clock.
   Hence two things are printed, both read-only:
     global                 the task's word, straight out of the guest (its rate must be 0: frequency)
     tick / gns / crate     tick = the exact value the gate hands the game (host ns * 12/625);
                            gns = the game's own ticks/freq*1e9 conversion. crate = game-seconds per
                            wall-second: ~1.0 => wall clock, ~fps/60 => frame driven.  tpf = ticks per
                            frame: a clock locked to frames would hold 320000 (=19.2e6/60).
   Every guest read goes through runtime_safe_read (fault guarded, mapping reported) and gates
   nothing.  The 2 s change check reads the counters without G on purpose: a torn word here is
   printed as evidence, never used as a decision. */
#define M6Q_SLOT_OFF 0x1369690ull   /* main's .bss slot holding the pointer to the cached frequency */
/* The value nn::os::GetSystemTick returns in this port (loader.c:359-363): host monotonic ns scaled
   to the 19.2 MHz the sdk reports as the frequency.  Pure arithmetic -- no mrs, no svc, no fault. */
static uint64_t m6q_gate_tick(uint64_t host_ns) { return (uint64_t)((__uint128_t)host_ns * 12u / 625u); }
/* 1 = the slot resolved and its target word was read; the mapping bits are reported, not enforced. */
static int m6q_tick_freq(uint64_t *freq, uint64_t *slot, int *slm, int *rd)
{
    *freq = 0; *slot = 0; *slm = 0; *rd = 0;
    uint8_t *mb = runtime_module_base("main");
    if (!mb) return 0;
    uint64_t a = 0;
    if (!runtime_safe_read(&a, (uint64_t)(uintptr_t)(mb + (uintptr_t)M6Q_SLOT_OFF), 8) || a < 4096u) return 0;
    *slot = a; *slm = runtime_addr_mapped(a) ? 1 : 0;
    uint64_t v = 0;
    if (!runtime_safe_read(&v, a, 8)) return 0;
    *rd = 1; *freq = v;
    return 1;
}
void runtime_m6q_poll(unsigned tsec, int full) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern void runtime_romfs_io_totals(uint64_t *, uint64_t *, uint64_t *, uint64_t *);
    extern unsigned long long runtime_draws_total(void);
    extern void runtime_romfs_io_snapshot(unsigned long long *, unsigned long long *, unsigned *, char *, unsigned);
    extern _Atomic unsigned long g_beat_frames, g_beat_nvn_submit, g_beat_present_try;
    static uint64_t p_opens, p_rd, p_n1c, p_n1d, p_tick, p_freq;
    static unsigned long p_frames;
    static double p_ns;
    static unsigned nchg;
    static int have, p_nt;
    uint64_t opens = 0, reads = 0, bytes = 0, nf = 0;
    runtime_romfs_io_totals(&opens, &reads, &bytes, &nf);
    uint64_t n1c[LP_ROLE], n1d[LP_ROLE], t1c = 0, t1d = 0;
    for (int r = 0; r < LP_ROLE; r++) {
        n1c[r] = n1d[r] = 0;
        for (unsigned i = 0; i < g_lp[r].n; i++) { n1c[r] += g_lp[r].k[i].n1c; n1d[r] += g_lp[r].k[i].n1d; }
        t1c += n1c[r]; t1d += n1d[r];
    }
    double ns = (double)now_ns();
    unsigned long frames = atomic_load_explicit(&g_beat_frames, memory_order_relaxed);
    unsigned long sub = atomic_load_explicit(&g_beat_nvn_submit, memory_order_relaxed);
    unsigned long pre = atomic_load_explicit(&g_beat_present_try, memory_order_relaxed);
    /* immediate notice when either gate word moves (2 s grid -- not a 30 s sample that can miss it) */
    int ch_o = have && (opens != p_opens || reads != p_rd);
    int ch_p = have && (t1c != p_n1c || t1d != p_n1d);
    /* M6r: an "advance" is a new open or a change in the live thread count (not a plain read: the
       BGM stream reads move every 4-6 s and would bury the real advances). Passed to the failure
       ledger together with what failed since the previous one. */
    int ch_open = have && opens != p_opens;
    int nt = runtime_live_threads();
    int ch_t = have && nt != p_nt;
    if (!full && (ch_o || ch_p)) {
        char lp[160]; lp[0] = 0; unsigned long long o2 = 0, r2 = 0; unsigned inf = 0;
        runtime_romfs_io_snapshot(&o2, &r2, &inf, lp, sizeof lp);
        nchg++;
        vk_boot_logf("[st] CHANGE#%u t=%us opens=%llu->%llu reads=%llu->%llu n1c=%llu->%llu n1d=%llu->%llu "
                     "frames=%lu submit=%lu draws=%llu inflight=%u last=%s",
                     nchg, tsec, (unsigned long long)p_opens, (unsigned long long)opens,
                     (unsigned long long)p_rd, (unsigned long long)reads,
                     (unsigned long long)p_n1c, (unsigned long long)t1c,
                     (unsigned long long)p_n1d, (unsigned long long)t1d,
                     frames, sub, runtime_draws_total(), inf, lp[0] ? lp : "-");
        if (ch_open || ch_t) {   /* M6r: align the failure ledger with this advance */
            extern void runtime_fa_advance(unsigned tsec, unsigned long long opens, int threads, const char *why);
            runtime_fa_advance(tsec, opens, nt, ch_open ? "opens" : "threads");
        }
    }
    if (full) {
        unsigned long long main_svc = 0, ldr_svc = 0; int main_wk = -1, alive = 0;
        uint32_t ldr_h = 0; uint64_t ldr_lr = 0, best = 0;
        int locked = 0;
        for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
        if (locked) {
            for (thr_t *t = g_all; t; t = t->all) {
                if (t->exited) continue;
                alive++;
                /* main = the entry thread (same convention runtime_main_snap already uses);
                   the loader = the live thread with the most open+read I/O (structure, not a handle). */
                if (t->handle == 0x1001u) { main_svc = t->nsvc; main_wk = t->wkind; }
                uint64_t io = (uint64_t)atomic_load_explicit(&t->io_open, memory_order_relaxed) +
                              (uint64_t)atomic_load_explicit(&t->io_read, memory_order_relaxed);
                if (io > best) { best = io; ldr_h = t->handle; ldr_svc = t->nsvc; ldr_lr = t->last_lr; }
            }
            pthread_mutex_unlock(&G);
        }
        char lr[64]; lr[0] = 0; if (ldr_lr) runtime_describe_address(ldr_lr, lr, sizeof lr);
        vk_boot_logf("[st] t=%us opens=%llu reads=%llu bytes=%llu frames=%lu submit=%lu present=%lu draws=%llu "
                     "main_svc=%llu main_wk=%d n1c=%llu(%llu/%llu/%llu) n1d=%llu(%llu/%llu/%llu) "
                     "loader_h=%#x loader_svc=%llu loader_lr=%s threads=%d",
                     tsec, (unsigned long long)opens, (unsigned long long)reads, (unsigned long long)bytes,
                     frames, sub, pre, runtime_draws_total(), main_svc, main_wk,
                     (unsigned long long)t1c, (unsigned long long)n1c[0], (unsigned long long)n1c[1],
                     (unsigned long long)n1c[2], (unsigned long long)t1d, (unsigned long long)n1d[0],
                     (unsigned long long)n1d[1], (unsigned long long)n1d[2],
                     ldr_h, ldr_svc, lr[0] ? lr : "-", alive);
        /* ---- the clock itself ---- */
        uint64_t freq = 0, slot = 0; int slm = 0, rdd = 0;
        int ok = m6q_tick_freq(&freq, &slot, &slm, &rdd);
        uint64_t host = runtime_now_ns();          /* mach_absolute_time ns: what the gate scales */
        uint64_t tick = m6q_gate_tick(host);       /* exactly the value the game receives */
        uint64_t gns = 0;
        if (freq > 0 && freq < 100000000000ull) gns = (uint64_t)(((__uint128_t)tick * 1000000000ull) / freq);
        double dt = p_ns > 0 ? (ns - p_ns) / 1e9 : -1.0;
        double frate = (ok && dt > 0) ? (double)((double)freq - (double)p_freq) / dt : -1.0;
        double crate = (gns && p_tick && freq && p_freq == freq && dt > 0)
                     ? ((double)(gns - (uint64_t)(((__uint128_t)p_tick * 1000000000ull) / freq)) / 1e9) / dt : -1.0;
        double tpf = (p_frames && frames > p_frames && p_tick && tick > p_tick)
                   ? (double)(tick - p_tick) / (double)(frames - p_frames) : -1.0;
        vk_boot_logf("[tk] t=%us global=%llu rate=%.3f frames=%lu submit=%lu present=%lu "
                     "tick=%llu gns=%llu crate=%.4f tpf=%.0f dt=%.1fs ok=%d ind=%d/%d is19=%d slot=%#llx",
                     tsec, (unsigned long long)freq, frate, frames, sub, pre,
                     (unsigned long long)tick, (unsigned long long)gns, crate, tpf, dt,
                     ok, slm, rdd, freq == 19200000ull ? 1 : 0, (unsigned long long)slot);
        p_tick = tick; p_freq = freq; p_ns = ns; p_frames = frames;
    }
    if (!have || full) { have = 1; p_opens = opens; p_rd = reads; p_n1c = t1c; p_n1d = t1d; p_nt = nt; }
    /* after a CHANGE line, rebase at once so the next notice is the next 2 s delta, not the same
       30 s-old comparison repeated every beat */
    if (!full && (ch_o || ch_p || ch_t)) { p_opens = opens; p_rd = reads; p_n1c = t1c; p_n1d = t1d; p_nt = nt; }
}

/* ---------- M7r (2026-11-07): the 100 s time-series probe for the "does the cutscene's own
   clock advance" boot. Two lines per 100 s, hard-capped at 24 data lines per boot (the task's
   probe budget), switch `smo_m7r` (C default OFF; `echo 1 >` enables, `echo 0 >` is the one-key
   rollback). Record-only, read-only:
     [m7r] clip  0x102f (demo/clip): park/run state, its wait key/lock, SVC count + delta since
                 the last sample, the role-key 0x1C/0x1D totals, and the words around its lock
                 that CHANGED since the last sample -- the only honest stand-in for "the cutscene
                 clock value it reads" until the 5.3 disassembly names the field. No changed word
                 across 100 s windows is itself the finding (a static object next to a 1/frame
                 kick = nothing in this window is counting).
     [m7r] main  0x1001: frame-loop alive (SVC + frame/submit deltas), its signals on the
                 poster-role keys (the job posts the task's chain main->poster->loader counts),
                 and the romfs opens/last snapshot (the acceptance metric on the same line).
   Same conventions as the other dumps: thread records under a trylock of G, tolerated tears on
   the lock-free counters, guest words via dump_read_u32 (mapping-checked). */
#define M7R_PATH_A "/data/storage/el2/base/files/smo_m7r"
#define M7R_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7r"
/* The switch has to exist before hdc can overwrite it (shell domain cannot create app-data
   files); same "seed then let hdc overwrite" contract as input.c tk_seed. */
__attribute__((constructor)) static void m7r_seed(void) {
    static const char *p[2] = { M7R_PATH_A, M7R_PATH_B };
    FILE *f = fopen(M7R_PATH_A, "r");
    if (f) { fclose(f); return; }
    for (int i = 0; i < 2; i++) {
        f = fopen(p[i], "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7r] seeded %s = 0 (off)\n", p[i]); return; }
    }
}
static int m7r_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7R_PATH_B : M7R_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
void runtime_m7r_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern void runtime_romfs_io_totals(uint64_t *, uint64_t *, uint64_t *, uint64_t *);
    extern void runtime_romfs_io_snapshot(unsigned long long *, unsigned long long *, unsigned *, char *, unsigned);
    extern _Atomic unsigned long g_beat_frames, g_beat_nvn_submit, g_beat_present_try;
    static int ann; static int lines;              /* announce once; hard cap 24 data lines */
    static double next_at; static unsigned last_t;
    static unsigned long p_fr, p_sub;
    static uint64_t p_clip_svc, p_main_svc, p_jobs, p_post1d, p_clip1c, p_clip1d;
    static uint32_t pcw[16]; static uint64_t pbase; static int phave;
    const unsigned PERIOD = 100u, CAP = 24u;
    int on = m7r_on();
    if (!on) { ann = 0; return; }
    if (!ann) { ann = 1; vk_boot_logf("[m7r] probe on period=%us cap=%u (t=%us)", PERIOD, CAP, tsec);
                next_at = (double)tsec + (double)PERIOD; last_t = tsec; return; }
    if ((double)tsec < next_at || lines >= (int)CAP) return;
    next_at += (double)PERIOD;
    if (last_t && tsec <= last_t) return;
    last_t = tsec;
    /* thread records for 0x102f and 0x1001 under the usual trylock */
    struct { int found, wk; uint32_t h; uint64_t key, lock, nsvc, lr; } c = {0}, m = {0};
    c.h = 0x102fu; m.h = 0x1001u;
    int locked = 0;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->exited) continue;
        if (t->handle == c.h) { c.found = 1; c.wk = t->wkind; c.key = t->wkey; c.lock = t->waddr; c.nsvc = t->nsvc; c.lr = t->last_lr; }
        if (t->handle == m.h) { m.found = 1; m.wk = t->wkind; m.key = t->wkey; m.lock = t->waddr; m.nsvc = t->nsvc; m.lr = t->last_lr; }
    }
    if (locked) pthread_mutex_unlock(&G);
    /* role-key totals (post-0x1007 era layout: g_lp[role].k[i], lock-free read, tears tolerated) */
    uint64_t clip1c = 0, clip1d = 0, jobs = 0, post1d = 0;
    for (int role = 0; role < LP_ROLE; role++)
        for (unsigned i = 0; i < g_lp[role].n; i++) {
            lp_key_t *e = &g_lp[role].k[i];
            if (role == 1) { clip1c += e->n1c; clip1d += e->n1d; }
            if (role == 2) { post1d += e->n1d;
                for (int k = 0; k < LP_SIG; k++) if (e->sig[k].h == 0x1001u) jobs += e->sig[k].n; }
        }
    unsigned long fr = atomic_load_explicit(&g_beat_frames, memory_order_relaxed);
    unsigned long sub = atomic_load_explicit(&g_beat_nvn_submit, memory_order_relaxed);
    uint64_t opens = 0, reads = 0, bytes = 0, nf = 0;
    runtime_romfs_io_totals(&opens, &reads, &bytes, &nf);
    char lp[160]; lp[0] = 0; unsigned long long o2 = 0, r2 = 0; unsigned inf = 0;
    runtime_romfs_io_snapshot(&o2, &r2, &inf, lp, sizeof lp);
    /* clip line: state + deltas + changed words around the lock (the clock candidates) */
    char cb[512]; int co = 0, nchg = 0;
    uint32_t w[16]; int rdf = 0;
    if (c.lock) {
        uint64_t base = c.lock + 0x24u;   /* just past the [lp] obj window (lock-0x40 .. lock+0x24) */
        for (int i = 0; i < 16; i++) { w[i] = 0; if (!dump_read_u32(base + 4u * (uint64_t)i, &w[i])) rdf++; }
        for (int i = 0; i < 16 && co < (int)sizeof cb - 48; i++) {
            int ch = phave && pbase == base && w[i] != pcw[i];
            if (ch) { nchg++; co += snprintf(cb + co, sizeof cb - (size_t)co, " +%#x=%#x(was %#x)", 0x24u + 4u * (uint64_t)i, w[i], pcw[i]); }
        }
        for (int i = 0; i < 16; i++) pcw[i] = w[i];
        pbase = base; phave = 1;
    } else co += snprintf(cb + co, sizeof cb - (size_t)co, " nolock");
    if (!nchg && !rdf && c.lock) co += snprintf(cb + co, sizeof cb - (size_t)co, " static");
    if (rdf) co += snprintf(cb + co, sizeof cb - (size_t)co, " unreadable=%d", rdf);
    cb[(co < (int)sizeof cb) ? co : (int)sizeof cb - 1] = 0;
    vk_boot_logf("[m7r] clip t=%us st=%s found=%d wk=%d key=%#llx lock=%#llx nsvc=%llu(+%llu) "
                 "n1c=%llu(+%llu) n1d=%llu(+%llu) chg=%d%s",
                 tsec, c.wk >= 2 ? "park" : "run", c.found, c.wk,
                 (unsigned long long)c.key, (unsigned long long)c.lock,
                 (unsigned long long)c.nsvc, (unsigned long long)(c.nsvc - p_clip_svc),
                 (unsigned long long)clip1c, (unsigned long long)(clip1c - p_clip1c),
                 (unsigned long long)clip1d, (unsigned long long)(clip1d - p_clip1d),
                 nchg, cb);
    p_clip_svc = c.nsvc; p_clip1c = clip1c; p_clip1d = clip1d;
    vk_boot_logf("[m7r] main t=%us loop=%s found=%d nsvc=%llu(+%llu) wk=%d frames=%lu(+%lu) submit=%lu(+%lu) "
                 "jobs=%llu(+%llu) post1d=%llu(+%llu) opens=%llu last=%s",
                 tsec, (m.nsvc != p_main_svc) ? "run" : "stall", m.found,
                 (unsigned long long)m.nsvc, (unsigned long long)(m.nsvc - p_main_svc), m.wk,
                 fr, fr >= p_fr ? fr - p_fr : 0ul, sub, sub >= p_sub ? sub - p_sub : 0ul,
                 (unsigned long long)jobs, (unsigned long long)(jobs - p_jobs),
                 (unsigned long long)post1d, (unsigned long long)(post1d - p_post1d),
                 (unsigned long long)opens, lp[0] ? lp : "-");
    p_main_svc = m.nsvc; p_jobs = jobs; p_post1d = post1d; p_fr = fr; p_sub = sub;
    lines++;
}
void runtime_load_dump(unsigned tsec) {    extern void vk_boot_logf(const char *fmt, ...);
    runtime_event_forensics(tsec);   /* M6h: [ev] pair / [ev] txt / [own] first, so they lead each dump */
    ldr_flush(tsec);   /* M6g: the per-wake trace lines come next in each dump */
    enum { LD_MAX = 48, LD_OBJ = 12 };
    struct ld_row { uint32_t h; int wk, op, nwh; uint64_t wa, key, lr, seq, nsvc;
                    uint32_t wh0; int wtype, wsig; uint64_t wcre, wwaits, wsigs; };
    struct ld_obj { unsigned idx; int type, sig, refs; uint64_t w, s, evs, evr, first, cre; };
    static struct { uint32_t h; int wk; uint64_t wa, key, lr, seq, nsvc; int n; } pv[LD_MAX];
    static int pvn; static unsigned pt;
    static uint64_t p34, p35, p1c, p1d;
    struct ld_row r[LD_MAX];
    int n = 0, nthr = 0, nr = 0, ncond = 0, naddr = 0, nsync = 0, nlock = 0, locked = 0;
    /* top-12 objects by svc-side waits, collected under G (type/signaled/refs are read there) */
    struct ld_obj ob[LD_OBJ]; int nob = 0;
    for (int i = 0; i < LD_OBJ; i++) ob[i].w = 0;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->exited) continue;
        nthr++;
        switch (t->wkind) { case 0: nr++; break; case 1: nlock++; break; case 2: ncond++; break; case 3: naddr++; break; default: nsync++; }
        if (n >= LD_MAX) continue;
        r[n].h = t->handle; r[n].wk = t->wkind; r[n].op = t->op; r[n].nwh = t->nwh;
        r[n].wa = t->waddr; r[n].key = t->wkey; r[n].lr = t->last_lr; r[n].seq = t->phe_seq; r[n].nsvc = t->nsvc;
        r[n].wh0 = 0; r[n].wtype = 0; r[n].wsig = -1; r[n].wcre = 0; r[n].wwaits = 0; r[n].wsigs = 0;
        if (t->nwh > 0 && t->wh[0] >= 0x1000u) {
            unsigned idx = t->wh[0] - 0x1000u;
            if (idx < MAX_H && g_h[idx].type) {
                r[n].wh0 = t->wh[0]; r[n].wtype = g_h[idx].type; r[n].wcre = g_h[idx].creator;
                if (g_h[idx].type == O_EVENT_R || g_h[idx].type == O_EVENT_W) { event_t *e = g_h[idx].obj; r[n].wsig = e ? e->signaled : -1; }
                else if (g_h[idx].type == O_THREAD) { thr_t *t2 = g_h[idx].obj; r[n].wsig = t2 ? t2->exited : -1; }
                r[n].wwaits = atomic_load_explicit(&g_syhw[idx], memory_order_relaxed);
                r[n].wsigs = atomic_load_explicit(&g_syhs[idx], memory_order_relaxed);
            }
        }
        n++;
    }
    for (unsigned i = 1; i < MAX_H; i++) {
        uint64_t w = atomic_load_explicit(&g_syhw[i], memory_order_relaxed);
        uint64_t s = atomic_load_explicit(&g_syhs[i], memory_order_relaxed);
        uint64_t es = atomic_load_explicit(&g_evcnt[i][0], memory_order_relaxed);
        uint64_t er = atomic_load_explicit(&g_evcnt[i][1], memory_order_relaxed);
        if (!w && !s && !es && !er) continue;
        int slot = -1;
        if (nob < LD_OBJ) slot = nob++;
        else { int mn = 0; for (int k = 1; k < LD_OBJ; k++) if (ob[k].w < ob[mn].w) mn = k; if (w <= ob[mn].w) continue; slot = mn; }
        ob[slot].idx = i; ob[slot].type = g_h[i].type; ob[slot].cre = g_h[i].creator;
        ob[slot].sig = -1; ob[slot].refs = 0;
        if (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W) { event_t *e = g_h[i].obj; if (e) { ob[slot].sig = e->signaled; ob[slot].refs = e->refs; } }
        else if (g_h[i].type == O_THREAD) { thr_t *t2 = g_h[i].obj; if (t2) ob[slot].sig = t2->exited; }
        ob[slot].w = w; ob[slot].s = s; ob[slot].evs = es; ob[slot].evr = er;
        ob[slot].first = atomic_load_explicit(&g_evfirst[i][0], memory_order_relaxed);
    }
    if (locked) pthread_mutex_unlock(&G);
    double span = (pt && tsec > pt) ? (double)(tsec - pt) : 0.0;
    pt = tsec;
    /* M6d: the address family ledger (svc 0x34 wait / 0x35 signal) with the FIRST signaller's handle
       per address. This is the only place that says WHO releases main's frame gate: a worker handle
       means the loop is job-gated, the render thread's handle means it is frame-paced. */
    static struct { uint64_t addr, n34, n35; uint32_t caller, callop; uint64_t calllr; int callcore, callfound;
                    uint64_t wake, zero; uint32_t lastcaller; uint64_t lastlr; int nwait; } sa[16];
    int san = 0;
    {
        int got = 0;
        for (int i = 0; i < 100 && !(got = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
        int nsa = (int)g_sigaddr_n; if (nsa > 16) nsa = 16;
        for (int i = 0; i < nsa; i++) {
            sa[i].addr = g_sigaddr[i].addr; sa[i].n34 = g_sigaddr[i].n34; sa[i].n35 = g_sigaddr[i].n35;
            sa[i].caller = g_sigaddr[i].caller; sa[i].callfound = 0; sa[i].callop = 0; sa[i].calllr = 0; sa[i].callcore = -1;
            sa[i].wake = g_sigaddr[i].wake; sa[i].zero = g_sigaddr[i].zero;
            sa[i].lastcaller = g_sigaddr[i].lastcaller; sa[i].lastlr = g_sigaddr[i].lastlr;
            sa[i].nwait = 0;
            for (thr_t *t = g_all; t; t = t->all)
                if (t->handle == sa[i].caller) { sa[i].callfound = 1; sa[i].callop = t->op; sa[i].calllr = t->last_lr; sa[i].callcore = t->cur_core; break; }
            for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == sa[i].addr) sa[i].nwait++;
        }
        san = nsa;
        if (got) pthread_mutex_unlock(&G);
    }
    for (int i = 0; i < san; i++) {
        if (sa[i].n34 + sa[i].n35 < 20) continue;
        char cl[64]; runtime_describe_address(sa[i].calllr, cl, sizeof cl);
        uint32_t wv = 0; int okr = dump_read_u32(sa[i].addr, &wv);   /* safe read: mapping-checked */
        /* M6k: has the word moved since the previous dump? A flag word that only ever reads 1 while
           its waiters keep waking is "nobody stores 2"; this column is the per-dump proof either way. */
        static struct { uint64_t addr; uint32_t w; int have; } pw[16]; static int pwn;
        int pk = -1;
        for (int j = 0; j < pwn; j++) if (pw[j].addr == sa[i].addr) { pk = j; break; }
        if (pk < 0 && pwn < 16) { pk = pwn++; pw[pk].addr = sa[i].addr; pw[pk].have = 0; }
        int chg = 0; uint32_t pword = 0;
        if (pk >= 0) {
            if (pw[pk].have) { pword = pw[pk].w; chg = (pword != wv) ? 1 : 0; }
            pw[pk].w = wv; pw[pk].have = 1;
        }
        vk_boot_logf("[load] addr t=%us addr=%#llx word=%#x rd=%d chg=%d prev=%#x wait34=%llu sig35=%llu first_sig_h=%#x sigop=%d(%s) siglr=%s sigcore=%d found=%d",
                     tsec, (unsigned long long)sa[i].addr, wv, okr, chg, pword, (unsigned long long)sa[i].n34, (unsigned long long)sa[i].n35,
                     sa[i].caller, sa[i].callop, runtime_op_name(sa[i].callop), sa[i].calllr ? cl : "-",
                     sa[i].callcore, sa[i].callfound);
        /* M6f: 1:1 wake test for this address over the window since the previous dump.
           delta/30s is what the task asks for; one2one=1 means every 0x35 in the window woke
           exactly one waiter (sig35 delta == woke delta, zero-wake signals == 0). */
        static struct { uint64_t addr, n34, n35, wake, zero; int seen; } pa[16];
        static int pan;
        int k = -1;
        for (int j = 0; j < pan; j++) if (pa[j].addr == sa[i].addr) { k = j; break; }
        uint64_t d34 = 0, d35 = 0, dwk = 0, dzero = 0; int one2one = -1;
        if (k >= 0) {
            d34 = sa[i].n34 - pa[k].n34; d35 = sa[i].n35 - pa[k].n35;
            dwk = sa[i].wake - pa[k].wake; dzero = sa[i].zero - pa[k].zero;
            one2one = (d35 > 0 && dwk == d35 && dzero == 0) ? 1 : 0;
        } else if (pan < 16) { k = pan++; pa[k].addr = sa[i].addr; }
        if (k >= 0) { pa[k].n34 = sa[i].n34; pa[k].n35 = sa[i].n35; pa[k].wake = sa[i].wake; pa[k].zero = sa[i].zero; pa[k].seen = 1; }
        char ll[64]; runtime_describe_address(sa[i].lastlr, ll, sizeof ll);
        vk_boot_logf("[load] objsig t=%us obj=%#llx d34=%llu d35=%llu dwoke=%llu dzero=%llu tot34=%llu tot35=%llu totwoke=%llu by=%#x bylr=%s waiters_now=%d one2one=%d",
                     tsec, (unsigned long long)sa[i].addr,
                     (unsigned long long)d34, (unsigned long long)d35, (unsigned long long)dwk, (unsigned long long)dzero,
                     (unsigned long long)sa[i].n34, (unsigned long long)sa[i].n35, (unsigned long long)sa[i].wake,
                     sa[i].lastcaller, sa[i].lastlr ? ll : "-", sa[i].nwait, one2one);
    }
    vk_boot_logf("[load] thr t=%us threads=%d run=%d lock=%d cond=%d addr=%d sync=%d g=%s",
                 tsec, nthr, nr, nlock, ncond, naddr, nsync, locked ? "ok" : "trylock-failed");
    struct { uint32_t h; int wk; uint64_t wa, key, lr, seq, nsvc; int n; } nv[LD_MAX];
    int nn = 0;
    for (int i = 0; i < n; i++) {
        int cnt = 1, havep = 0; long long dns = -1;
        for (int j = 0; j < pvn; j++) if (pv[j].h == r[i].h) {
            havep = 1; dns = (long long)(r[i].nsvc - pv[j].nsvc);
            if (pv[j].wk == r[i].wk && pv[j].wa == r[i].wa && pv[j].key == r[i].key &&
                pv[j].lr == r[i].lr && pv[j].seq == r[i].seq) cnt = pv[j].n + 1;
            break;
        }
        nv[nn].h = r[i].h; nv[nn].wk = r[i].wk; nv[nn].wa = r[i].wa; nv[nn].key = r[i].key;
        nv[nn].lr = r[i].lr; nv[nn].seq = r[i].seq; nv[nn].nsvc = r[i].nsvc; nv[nn].n = cnt; nn++;
        if (r[i].h != 0x1001u && cnt < 2) continue;   /* main always; others only past 2 dumps (>60 s) */
        char wl[64]; runtime_describe_address(r[i].lr, wl, sizeof wl);
        double secs = (span > 0 ? (double)cnt * span : (double)cnt * 60.0);
        char hs[128]; hs[0] = 0;
        if (r[i].wh0) {
            char cr[64]; runtime_describe_address(r[i].wcre, cr, sizeof cr);
            snprintf(hs, sizeof hs, " wh=%#x wtype=%d wsig=%d wwaits=%llu wsigs=%llu wcre=%s",
                     r[i].wh0, r[i].wtype, r[i].wsig, (unsigned long long)r[i].wwaits,
                     (unsigned long long)r[i].wsigs, r[i].wcre ? cr : "-");
        }
        vk_boot_logf("[load] wait t=%us thr=%#x op=%d(%s) wk=%d obj=%#llx key=%#llx nsvc=%llu dnsvc=%lld seq=%llu static=%ddumps secs=%.0f lr=%s%s",
                     tsec, r[i].h, r[i].op, runtime_op_name(r[i].op), r[i].wk,
                     (unsigned long long)r[i].wa, (unsigned long long)r[i].key,
                     (unsigned long long)r[i].nsvc, havep ? dns : -1LL, (unsigned long long)r[i].seq,
                     cnt, secs, wl, hs);
    }
    pvn = nn;
    for (int i = 0; i < nn; i++) {   /* copy field by field: pv and nv are distinct anonymous types */
        pv[i].h = nv[i].h; pv[i].wk = nv[i].wk; pv[i].wa = nv[i].wa; pv[i].key = nv[i].key;
        pv[i].lr = nv[i].lr; pv[i].seq = nv[i].seq; pv[i].nsvc = nv[i].nsvc; pv[i].n = nv[i].n;
    }
    for (int i = 0; i < nob; i++) {
        if (ob[i].w < 50) continue;   /* noise floor: one-shot waits are not attribution */
        char cr[64], fs[64];
        runtime_describe_address(ob[i].cre, cr, sizeof cr);
        runtime_describe_address(ob[i].first, fs, sizeof fs);
        vk_boot_logf("[load] obj h=%#x type=%d sig=%d refs=%d waits=%llu sigs_svc=%llu sigs_any=%llu resets=%llu creator=%s firstsig=%s",
                     ob[i].idx + 0x1000u, ob[i].type, ob[i].sig, ob[i].refs,
                     (unsigned long long)ob[i].w, (unsigned long long)ob[i].s,
                     (unsigned long long)ob[i].evs, (unsigned long long)ob[i].evr,
                     ob[i].cre ? cr : "-", ob[i].first ? fs : "-");
    }
    {
        uint64_t a = atomic_load(&g_n34), b = atomic_load(&g_n35), c = atomic_load(&g_n1c), d = atomic_load(&g_n1d);
        vk_boot_logf("[load] sync t=%us wait34=%llu(+%llu) sig35=%llu(+%llu) wait1c=%llu(+%llu) sig1d=%llu(+%llu) h1023_calls=%llu woken=%llu woken_by_101f=%llu",
                     tsec, (unsigned long long)a, (unsigned long long)(a - p34), (unsigned long long)b, (unsigned long long)(b - p35),
                     (unsigned long long)c, (unsigned long long)(c - p1c), (unsigned long long)d, (unsigned long long)(d - p1d),
                     (unsigned long long)g_w1023_calls, (unsigned long long)g_w1023_wake,
                     (unsigned long long)g_w1023_wake_101f);
        p34 = a; p35 = b; p1c = c; p1d = d;
    }
    /* M6k-A: the 0x102f trail, its own wait tuple and the words of the structure it waits in. */
    m6k_drain(tsec);
    m6k_wait_dump(tsec);
    lp_dump(tsec);   /* M6l: role keys (loader / clipping) + the mover table; independent of the above */
    ps_dump(tsec);   /* M6m: the poster's own condition key -- its waits, its signallers, its last 8 SVCs */
    wg_dump(tsec);   /* M6n: every parked thread's key + who last signalled each key (whole-process graph) */
    mn_dump(tsec);   /* M6n: what main is doing -- its newest SVCs, its 0x1D targets, its wait state */
}

/* ---------- M5k: dump the object the main thread blocks on, into vk_boot.log ----------
   Called from the heartbeat probe/freeze dump. Read-only; no thread state is touched.
   Evidence design (task criteria 1/3): the 0x200-byte window around the key is snapshotted
   at the first dump and diffed at every later dump, so "this field never changes" is a
   statement about two (or more) points in time, not one sample. */
void runtime_m5k_dump(void) {
    extern void vk_boot_logf(const char *fmt, ...);
    int dno = ++g_m5k_dumps;
    uint64_t key = g_m5k_key, lock = g_m5k_lock, lr1c = g_m5k_lr1c;
    /* M5l：每次 dump 重载 /proc/self/maps 区间表（堆会生长/模块会卸，表不能一次定终身），
       并先落一行闸门状态，后面每条 [fx] 都带 mapped 位。smo_forensic=0 时整段记账与对象窗口跳过。 */
    if (smo_forensic) {
        runtime_maps_refresh();
        vk_boot_logf("[fx] dump#%d maps=%s entries=%u total=%u dropped=%u truncated=%d safe_read hits=%llu rejected=%llu guard_saved_faults=%llu ungated=%llu",
                     dno, runtime_maps_usable() ? "on" : "OFF", runtime_maps_entries(),
                     runtime_maps_total(), runtime_maps_dropped(), runtime_maps_dropped() ? 1 : 0,
                     (unsigned long long)runtime_safe_read_hits(), (unsigned long long)runtime_safe_read_rejects(),
                     (unsigned long long)runtime_safe_read_faults(), (unsigned long long)runtime_ungated_reads());
        fx_sync_ledger(dno);
    }
    /* copy thread state under G (do not hold it while logging) */
    int mwk = -99, mcore = -99, nthr = 0, ncond = 0, msvc = 0;
    uint64_t mwa = 0, mkey = 0, mnsvc = 0, mlr = 0;
    struct { uint32_t h; uint64_t key, wa, nsvc, lr; } wt[12]; int nwt = 0;
    int ev_h[4] = { 0x101f, 0x1023, 0x1026, 0x102a };
    int ev_type[4] = { 0 }, ev_sig[4] = { 0 }, ev_refs[4] = { 0 }; uint64_t ev_cr[4] = { 0 };
    int locked = 0; for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    /* M5o: per-thread snapshot for the "who has been parked on the SAME object for N dumps" table.
       Sampled under G, printed after the unlock (same discipline as everything else here). */
    struct { uint32_t h; int wk, op, ok; uint64_t key, addr, lr, nsvc, seq; } ts[48]; int nts = 0;
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->exited) continue;
        nthr++; if (t->wkind == 2) ncond++;
        if (t->handle == 0x1001) { mwk = t->wkind; mwa = t->waddr; mkey = t->wkey; mnsvc = t->nsvc; msvc = (int)(uint32_t)t->last_svc; mlr = t->last_lr; mcore = t->cur_core; }
        /* idle workers: their own wait object is the queue end they are parked on */
        if (t->wkind == 2 && t->nsvc > 1000 && nwt < 12) { wt[nwt].h = t->handle; wt[nwt].key = t->wkey; wt[nwt].wa = t->waddr; wt[nwt].nsvc = t->nsvc; wt[nwt].lr = t->last_lr; nwt++; }
        if (nts < 48) { ts[nts].h = t->handle; ts[nts].wk = t->wkind; ts[nts].op = t->op; ts[nts].ok = 1;
                        ts[nts].key = t->wkey; ts[nts].addr = t->waddr; ts[nts].lr = t->last_lr;
                        ts[nts].nsvc = t->nsvc; ts[nts].seq = t->phe_seq; nts++; }
    }
    for (int k = 0; k < 4; k++) {
        unsigned i = (unsigned)ev_h[k] - 0x1000u;
        if (i == 0 || i >= MAX_H) continue;
        ev_type[k] = g_h[i].type; ev_cr[k] = g_h[i].creator;
        if (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W) { event_t *e = g_h[i].obj; if (e) { ev_sig[k] = e->signaled; ev_refs[k] = e->refs; } }
    }
    if (locked) pthread_mutex_unlock(&G);
    char w1[96], w2[96];
    runtime_describe_address(lr1c, w1, sizeof w1);
    runtime_describe_address(g_m5k_sig_lr, w2, sizeof w2);
    vk_boot_logf("[m5k] dump#%d key=%#llx lock=%#llx lr1c=%#llx %s | threads=%d cond_wait=%d",
                 dno, (unsigned long long)key, (unsigned long long)lock, (unsigned long long)lr1c, key ? w1 : "-", nthr, ncond);
    vk_boot_logf("[m5k] main wk=%d wa=%#llx wkey=%#llx svc=%#x nsvc=%llu lr=%#llx core=%d",
                 mwk, (unsigned long long)mwa, (unsigned long long)mkey, (unsigned)msvc,
                 (unsigned long long)mnsvc, (unsigned long long)mlr, mcore);
    vk_boot_logf("[m5k] ledger 1c=%llu 1d=%llu sig_by=%#x sig_lr=%#llx %s | 34=%llu imm=%llu blk=%llu last34=%#llx type=%d want=%d seen=%#x | 1a=%llu blk=%llu",
                 (unsigned long long)g_m5k_n1c, (unsigned long long)g_m5k_n1d, g_m5k_sig_h,
                 (unsigned long long)g_m5k_sig_lr, g_m5k_sig_h ? w2 : "-",
                 (unsigned long long)g_m5k_c34, (unsigned long long)g_m5k_c34_imm, (unsigned long long)g_m5k_c34_blk,
                 (unsigned long long)g_m5k_last34, g_m5k_last34t, g_m5k_last34want, (unsigned)g_m5k_last34v,
                 (unsigned long long)g_m5k_c1a, (unsigned long long)g_m5k_c1a_blk);
    for (unsigned i = 0; i < g_m5k_ri && i < 4; i++) {
        unsigned k = (g_m5k_ri - 1u - i) & 3u;
        vk_boot_logf("[m5k] wait#%u key=%#llx addr=%#llx ret=%#llx waited=%.3fms", i,
                     (unsigned long long)g_m5k_ring[k][0], (unsigned long long)g_m5k_ring[k][1],
                     (unsigned long long)g_m5k_ring[k][2], (double)g_m5k_ring[k][3] / 1e6);
    }
    /* M6d: the four tracked event handles are printed here, before the 0x1C early exit. In this port
       main never waits on a 0x1C key (the game uses WaitForAddress), so the old placement below the
       `!key` goto meant these rows never appeared in any SMO boot. Type/signaled/refs/creator is
       exactly the "who owns the event the audio worker polls" answer. */
    for (int k = 0; k < 4; k++)
        vk_boot_logf("[m5k] ev h=%#x type=%d signaled=%d refs=%d creator=%#llx sig=%llu rst=%llu", ev_h[k],
                     ev_type[k], ev_sig[k], ev_refs[k], (unsigned long long)ev_cr[k],
                     (unsigned long long)atomic_load_explicit(&g_evcnt[(unsigned)ev_h[k] - 0x1000u][0], memory_order_relaxed),
                     (unsigned long long)atomic_load_explicit(&g_evcnt[(unsigned)ev_h[k] - 0x1000u][1], memory_order_relaxed));
    if (!key) { vk_boot_logf("[m5k] no 0x1C seen on h=0x1001 yet (window skipped)"); goto m5o_tail; }   /* M5o: the two new tables below must still run (they do not need the 0x1C key) */
    /* ---------- M5l A/B：对象 dump（先把「目标地址到底属不属于谁」钉死） ----------
       `key-0x58` 是派活单给的假设（历史注：worker 池对象=wkey-0x58）。上一轮直接解引用它把 guest
       打死了，所以这里先判 mapped 再读，且读一次都不裸 memcpy：
         - [fx] target=… mapped=0|1：闸门结论（来自 /proc/self/maps），
         - mapped=1：从 key-0x58 起 32 个 8 字节（= 派活单要的 32 词），每个 dump 都打 + 与上次
           dump 的逐词 diff ⇒ 两个时间点「这个字段变不变」是实测结论，不是单点采样；
         - mapped=0：说明 key-0x58 假设错，改打 key+0/±0x10/±0x20 各 8 词（找 key 周边的结构）。 */
    if (smo_forensic) {
        uint64_t tgt = (key & ~7ULL) - 0x58ULL;
        int mapped = runtime_addr_mapped(tgt);
        vk_boot_logf("[fx] target=%#llx mapped=%d (key=%#llx key-0x58) dump#%d", (unsigned long long)tgt, mapped,
                     (unsigned long long)key, dno);
        static uint64_t fx_prev[32]; static uint64_t fx_prev_tgt; static int fx_prev_ok;
        if (mapped) {
            uint64_t w[32]; int bad = 0;
            for (int i = 0; i < 32; i++) if (!fx_read64(tgt + (uint64_t)i * 8, &w[i])) { w[i] = 0; bad++; }
            for (int i = 0; i < 32; i += 4)
                vk_boot_logf("[fx] obj %#llx %+lld: %016llx %016llx %016llx %016llx",
                             (unsigned long long)(tgt + (uint64_t)i * 8), (long long)((tgt + (uint64_t)i * 8) - key),
                             (unsigned long long)w[i], (unsigned long long)w[i + 1],
                             (unsigned long long)w[i + 2], (unsigned long long)w[i + 3]);
            if (fx_prev_ok && fx_prev_tgt == tgt) {
                int nch = 0, off = 0; char dl[512];
                for (int i = 0; i < 32; i++) if (w[i] != fx_prev[i]) {
                    nch++;
                    if (off < (int)sizeof dl - 44) off += snprintf(dl + off, sizeof dl - (size_t)off, " %+lld:%016llx->%016llx",
                                                                  (long long)((tgt + (uint64_t)i * 8) - key),
                                                                  (unsigned long long)fx_prev[i], (unsigned long long)w[i]);
                }
                vk_boot_logf("[fx] diff vs dump#%d (60s+ apart): %d of 32 words changed%s", dno - 1, nch, nch ? dl : " (none)");
            }
            memcpy(fx_prev, w, sizeof w); fx_prev_tgt = tgt; fx_prev_ok = 1;
            vk_boot_logf("[fx] obj read ok=%d/32 (unreadable words printed as 0)", 32 - bad);
        } else {
            /* 假设错：改看 key 自己周边（key+0/±0x10/±0x20 各 8 词），同样安全读。 */
            const long long probes[5] = { 0, -0x10, 0x10, -0x20, 0x20 };
            for (int p = 0; p < 5; p++) {
                uint64_t b = (uint64_t)((long long)key + probes[p]) & ~7ULL;
                int mp = runtime_addr_mapped(b);
                uint64_t w[8] = {0};
                if (mp) for (int i = 0; i < 8; i++) fx_read64(b + (uint64_t)i * 8, &w[i]);
                vk_boot_logf("[fx] alt key%+lld=%#llx mapped=%d: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx",
                             probes[p], (unsigned long long)b, mp,
                             (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3],
                             (unsigned long long)w[4], (unsigned long long)w[5], (unsigned long long)w[6], (unsigned long long)w[7]);
            }
        }
    }
    uint64_t k32 = 0; { uint64_t kv = 0, lv = 0; r22_read64(key & ~7ULL, &kv); r22_read64(lock & ~7ULL, &lv);
        uint32_t a = 0, b = 0; memcpy(&a, (const uint8_t *)&kv + (key & 7), 4); memcpy(&b, (const uint8_t *)&lv + (lock & 7), 4);
        k32 = a; vk_boot_logf("[m5k] keyword: [key]=%#x [lock]=%#x", a, b); }
    (void)k32;
    /* window: 64 u64 from key-0x100 to key+0x100 (4 words per line) */
    uint64_t base = (key & ~7ULL) - 0x100ULL, cur[64];
    for (int i = 0; i < 64; i++) { uint64_t v = 0; if (!r22_read64(base + (uint64_t)i * 8, &v)) v = 0; cur[i] = v; }
    for (int i = 0; i < 64; i += 4)
        vk_boot_logf("[m5k] mem %#llx %+lld: %016llx %016llx %016llx %016llx",
                     (unsigned long long)(base + (uint64_t)i * 8), (long long)((base + (uint64_t)i * 8) - key),
                     (unsigned long long)cur[i], (unsigned long long)cur[i + 1],
                     (unsigned long long)cur[i + 2], (unsigned long long)cur[i + 3]);
    if (!g_m5k_snap_ok || g_m5k_snap_key != key) {
        memcpy(g_m5k_snap, cur, sizeof cur); g_m5k_snap_ok = 1; g_m5k_snap_key = key;
        vk_boot_logf("[m5k] snapshot taken at dump#%d (diff baseline)", dno);
    } else {
        char b[512]; int o = 0, nch = 0;
        for (int i = 0; i < 64; i++) if (cur[i] != g_m5k_snap[i]) {
            nch++;
            if (o < (int)sizeof b - 80) o += snprintf(b + o, sizeof b - (size_t)o, " %+lld:%016llx->%016llx",
                                                      (long long)((base + (uint64_t)i * 8) - key),
                                                      (unsigned long long)g_m5k_snap[i], (unsigned long long)cur[i]);
        }
        vk_boot_logf("[m5k] diff vs snapshot: %d of 64 words changed%s", nch, nch ? b : " (none)");
    }
    /* who points into the window: scan +-0x80000 for any word in [key-0x20,key+0x20) */
    { int hits = 0, tried = 0, bad = 0; uint64_t hit_at[3] = { 0, 0, 0 };
      for (uint64_t a = (key - 0x80000ULL) & ~15ULL; a < key + 0x80000ULL && hits < 6; a += 16) {
          uint64_t w[2]; tried++;
          if (!spin_read16(a, w)) { bad++; continue; }
          for (int k = 0; k < 2 && hits < 6; k++)
              if (w[k] >= key - 0x20ULL && w[k] < key + 0x20ULL) {
                  vk_boot_logf("[m5k] ptr %#llx (word %d) -> %#llx [%#llx %#llx]", (unsigned long long)(a + (uint64_t)k * 8), k,
                               (unsigned long long)w[k], (unsigned long long)w[0], (unsigned long long)w[1]);
                  if (hits < 3) hit_at[hits] = a + (uint64_t)k * 8;
                  hits++;
              }
      }
      vk_boot_logf("[m5k] ptrscan +-0x80000: hits=%d tried=%d unreadable=%d", hits, tried, bad);
      /* neighbours of the first owners: the container object around the pointer */
      for (int h = 0; h < 3 && hit_at[h]; h++) {
          uint64_t b0 = (hit_at[h] & ~7ULL) - 0x20ULL;
          for (int i = 0; i < 8; i += 4) {
              uint64_t v[4] = { 0, 0, 0, 0 }; int ok = 1;
              for (int j = 0; j < 4; j++) ok &= r22_read64(b0 + (uint64_t)(i + j) * 8, &v[j]);
              vk_boot_logf("[m5k] own %#llx: %016llx %016llx %016llx %016llx%s", (unsigned long long)(b0 + (uint64_t)i * 8),
                           (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[3], ok ? "" : "?");
          }
      }
    }
    for (int i = 0; i < nwt; i++) {
        uint64_t kv = 0, lv = 0;
        r22_read64(wt[i].key & ~7ULL, &kv); r22_read64(wt[i].wa & ~7ULL, &lv);
        uint32_t a = 0, b = 0;
        memcpy(&a, (const uint8_t *)&kv + (wt[i].key & 7), 4); memcpy(&b, (const uint8_t *)&lv + (wt[i].wa & 7), 4);
        char ww[96]; runtime_describe_address(wt[i].lr, ww, sizeof ww);
        vk_boot_logf("[m5k] waiter h=%#x key=%#llx keyv=%#x wa=%#llx lockv=%#x nsvc=%llu lr=%s",
                     wt[i].h, (unsigned long long)wt[i].key, a, (unsigned long long)wt[i].wa, b,
                     (unsigned long long)wt[i].nsvc, ww);
    }
m5o_tail:   /* M5o: reached either from the normal end of the function or from the key==0 early exit */
    /* M5o: "parked on the SAME object for N dumps" table (criterion 2, general form).
       A thread whose (wkind,key,addr,lr,op,phase-seq) tuple is byte-identical to the previous dump
       has not moved: N dumps x 60 s = how long it has been waiting on that one object. nsvc is
       deliberately NOT part of the tuple (it only says the thread did svc's before parking).
       seq rising with a constant tuple = spinning in many small calls; constant seq AND tuple =
       stuck inside one call. Both are printed. */
    {
        static struct { uint32_t h; int wk, op; uint64_t key, addr, lr, nsvc, seq; int n; } pw[48];
        static int pwn;
        struct { uint32_t h; int wk, op; uint64_t key, addr, lr, nsvc, seq; int n; } cur[48];
        int ncur = 0, changed = 0;
        for (int i = 0; i < nts; i++) {
            int n = 1;
            for (int j = 0; j < pwn; j++) if (pw[j].h == ts[i].h) {
                if (pw[j].wk == ts[i].wk && pw[j].key == ts[i].key && pw[j].addr == ts[i].addr &&
                    pw[j].lr == ts[i].lr && pw[j].op == ts[i].op && pw[j].seq == ts[i].seq)
                    n = pw[j].n + 1;
                break;
            }
            if (n == 1) changed++;
            cur[ncur].h = ts[i].h; cur[ncur].wk = ts[i].wk; cur[ncur].op = ts[i].op;
            cur[ncur].key = ts[i].key; cur[ncur].addr = ts[i].addr; cur[ncur].lr = ts[i].lr;
            cur[ncur].nsvc = ts[i].nsvc; cur[ncur].seq = ts[i].seq; cur[ncur].n = n; ncur++;
        }
        for (int k = 0; k < 6; k++) {
            int best = -1;
            for (int i = 0; i < ncur; i++) {
                if (cur[i].n < 2) continue;
                if (best < 0 || cur[i].n > cur[best].n) best = i;
            }
            if (best < 0) break;
            char wl[96]; runtime_describe_address(cur[best].lr, wl, sizeof wl);
            vk_boot_logf("[wa] dump#%d parked h=%#x static_for=%ddumps(~%ds) wk=%d key=%#llx addr=%#llx op=%d nsvc=%llu seq=%llu lr=%s",
                         dno, cur[best].h, cur[best].n, cur[best].n * 60, cur[best].wk,
                         (unsigned long long)cur[best].key, (unsigned long long)cur[best].addr,
                         cur[best].op, (unsigned long long)cur[best].nsvc,
                         (unsigned long long)cur[best].seq, wl);
            cur[best].n = 0;   /* printed once */
        }
        vk_boot_logf("[wa] dump#%d threads=%d moved_this_dump=%d total_static=%d", dno, nts, changed, ncur - changed);
        pwn = ncur > 48 ? 48 : ncur;
        for (int i = 0; i < pwn; i++) {
            pw[i].h = cur[i].h; pw[i].wk = cur[i].wk; pw[i].op = cur[i].op; pw[i].key = cur[i].key;
            pw[i].addr = cur[i].addr; pw[i].lr = cur[i].lr; pw[i].nsvc = cur[i].nsvc;
            pw[i].seq = cur[i].seq; pw[i].n = (cur[i].n == 0) ? 1 : cur[i].n;   /* reset printed rows to 1: they still existed */
        }
    }
    /* M5o: [fn] ledger -- NVN sync/event objects, per-object waits vs signals (gfx_bridge.c).
       Read-only: counts live in gfx_bridge.c, this only asks for the report. */
    { if (dm_nvn_fn_dump) dm_nvn_fn_dump(dno); }
}

/* ---------- round3-B 自旋期取证：SIGUSR2 pc 链采样器 ----------
   背景：mprotect 型 READ watch 在本 OHOS 镜像不可用——DFX sigchain 截获 SIGSEGV，首个页面 fault 即判死
   整进程（emu_round3.log + cppcrash-9776，fault=0x6e459931b0 屏障同页越窗写）。改用信号采样：
   对「已启动、未退出、不在任何宿主等待（wkind==0）」的客体线程 pthread_kill(SIGUSR2)，handler 只把
   ucontext 的 pc/lr/fp 抓进全局槽（async-signal-safe：仅读寄存器+两次 store），采样线程在普通上下文
   里走帧链——每帧先 mincore 验映射再读（OHOS 上 mach_vm_read_overwrite=裸 memcpy，ohos_compat.h:235，
   野指针即 SEGV 即死，不能裸走）。样本即「自旋线程此刻的 pc 链」，洪流期/静默期都持续输出，
   与 hang watchdog 的 60s 全景 dump 互补。 */
static struct { _Atomic unsigned seq; volatile uint64_t pc, lr, fp, x19, x20, x21, sp; } g_spin_cap;
static volatile int g_m7s_pause;   /* M7s: set while the one-shot clip-thread walk owns the capture slot; the sampler skips its cycle so the two cannot race the same request slot */
static void spin_cap_handler(int num, siginfo_t *si, void *ctx) {
    (void)num; (void)si;
    unsigned s = atomic_load_explicit(&g_spin_cap.seq, memory_order_relaxed);
    if (!(s & 1)) return;   // 无采样请求
    ucontext_t *u = ctx;
#ifdef __MUSL__
    g_spin_cap.pc = u->uc_mcontext.pc; g_spin_cap.lr = u->uc_mcontext.regs[30]; g_spin_cap.fp = u->uc_mcontext.regs[29];
    g_spin_cap.x19 = u->uc_mcontext.regs[19]; g_spin_cap.x20 = u->uc_mcontext.regs[20]; g_spin_cap.x21 = u->uc_mcontext.regs[21];
    g_spin_cap.sp = u->uc_mcontext.sp;
#else
    g_spin_cap.pc = u->uc_mcontext->__ss.__pc; g_spin_cap.lr = u->uc_mcontext->__ss.__lr; g_spin_cap.fp = u->uc_mcontext->__ss.__fp;
    g_spin_cap.x19 = u->uc_mcontext->__ss.__x[19]; g_spin_cap.x20 = u->uc_mcontext->__ss.__x[20]; g_spin_cap.x21 = u->uc_mcontext->__ss.__x[21];
    g_spin_cap.sp = u->uc_mcontext->__ss.__sp;
#endif
    atomic_store_explicit(&g_spin_cap.seq, s + 1, memory_order_release);   // 奇→偶：完成
}
#ifdef __MUSL__
#include <sys/syscall.h>
#include <sys/uio.h>
/* round3 第二次崩溃（SEGV_ACCERR@0x6e03a47ffc，采样器启动 100ms）：mincore 对 PROT_NONE 映射页返回 0
   （只查映射不查权限），帧读 memcpy 裸读保护页即死。改 process_vm_readv 自进程读——对 PROT_NONE/未映射
   一律返回 EFAULT 而不 fault，读自身 pid 永远合法（不受 yama 限制）。 */
static int spin_read16(uint64_t a, uint64_t out[2]) {
    if (a < 0x100000000ULL || a >= 0x800000000000ULL || (a & 15)) return 0;
    struct iovec l = { out, 16 }, r = { (void *)(uintptr_t)a, 16 };
    return syscall(__NR_process_vm_readv, getpid(), &l, 1ul, &r, 1ul, 0ul) == 16;
}
#else
static int spin_read16(uint64_t a, uint64_t out[2]) {
    if (a < 0x100000000ULL || a >= 0x800000000000ULL || (a & 15)) return 0;
    unsigned char v; uintptr_t p = (uintptr_t)(a & ~0xfffull);
    if (mincore((void *)p, 4096, &v) != 0) return 0;
    if (((a + 15) & ~0xfffull) != p && mincore((void *)(uintptr_t)((a + 15) & ~0xfffull), 4096, &v) != 0) return 0;
    memcpy(out, (const void *)(uintptr_t)a, 16); return 1;
}
#endif
/* round22 对拍通道：SIGUSR2 采样器末次成功样本（成功样本极少——round21 全程 1 次，恰为
   walker 家族线程；kill 超时不写槽 ⇒ 值自末次成功后一直留存）。 */
static uint64_t r22_spin_pc(void) { return g_spin_cap.pc; }
static uint64_t r22_spin_x19(void) { return g_spin_cap.x19; }
/* round22b：walker 窗专用锁存——末次成功样本会被后到的宿主帧样本覆盖（round22 两跑实测：
   runA x19=0x7f84…宿主未对齐、runB 早采样 h=0x1008 pc=main+0x97d220 x19=0x6e464960a8 后被
   pc=main+0x9795a0 x19=0x7f7f… 覆盖）。首个 pc∈main[0x963000,0x980000)（0x964xxx walker 环 +
   0x97d/0x97e 版本循环调用者，donor probe lr=main+0x97e314 同族）的样本单独定格不被覆盖。 */
static struct { _Atomic unsigned set; volatile uint64_t pc, lr, x19; } g_r22_walk;
static uint64_t r22_walk_pc(void) { return g_r22_walk.pc; }
static uint64_t r22_walk_x19(void) { return g_r22_walk.x19; }
static unsigned r22_walk_set(void) { return atomic_load(&g_r22_walk.set); }
static void *spin_sampler(void *arg) {
    (void)arg;
    unsigned ticks = 0, tot_ok = 0, tot_fail = 0;
    for (;;) {
        usleep(200000);   // 5Hz：静默期每线程每秒 5 条 pc 链，洪流期也照采
        if (g_m7s_pause) { usleep(200000); continue; }   // M7s walk owns the capture slot this cycle
        ticks++;
        /* round3 第五跑定谳：采样器 0.4s 后必停 = 持 G 做 I/O 的连锁死等——fprintf 要 stderr 的 FILE 锁、
           OH_LOG_Print 要 hilog 客户端锁（与 stderr→hilog 转发线程共享），hilogd 对进程限流后转发线程
           阻塞在 OH_LOG_Print 持客户端锁，采样器持 G 等它 → 全模拟器 SVC(G) 冻结（洪流不需 G 照跑，
           dump 也不出——round3 第三跑 7 分钟活体+零 dump/零样本同证）。
           第六跑定谳：等 handler 期间也不能持 G——目标线程被 G 闸住难以及时应答，kill 全超时
           （124 心跳 cand=3..5 却只 4 样本）。改：持 G 只做「选人+pin」，放锁后 kill/等/取帧（20ms
           预算），再回锁 unpin。pin 住 thr_free 不回收（泄露=取证可接受）。 */
        char buf[4096]; int bn = 0;
        int cand_n = 0, ok = 0, fail = 0;
        pthread_mutex_lock(&G);
        thr_t *cand[32]; int nc = 0;
        for (thr_t *t = g_all; t && nc < 32; t = t->all)
            if (t->started && !t->exited && !t->wkind && t->pt) { cand[nc++] = t; t->spin_pin = 1; }
        pthread_mutex_unlock(&G);
        cand_n = nc;
        for (int i = 0; i < nc; i++) {
            thr_t *t = cand[i];
            atomic_store(&g_spin_cap.seq, atomic_load(&g_spin_cap.seq) + 1);   // 置奇=请求
            if (pthread_kill(t->pt, SIGUSR2) != 0) { atomic_store(&g_spin_cap.seq, atomic_load(&g_spin_cap.seq) + 1); fail++; continue; }
            int got = 0;
            for (int w = 0; w < 40 && !got; w++) { if (!(atomic_load(&g_spin_cap.seq) & 1)) got = 1; else usleep(500); }
            if (!got) { atomic_store(&g_spin_cap.seq, atomic_load(&g_spin_cap.seq) + 1); fail++;
                /* 定谳用：读目标线程的信号位图（SigBlk=屏蔽中/SigIgn=忽略/SigCgt=已捕获），
                   kill 超时 + SigBlk 含 bit12(SIGUSR2) = 目标把它屏蔽了（pthread_create 继承创建者掩码）。 */
#ifdef __MUSL__
                static int said_blk; if (!said_blk && t->htid) { said_blk = 1;
                    char path[64]; snprintf(path, sizeof path, "/proc/self/task/%d/status", t->htid);
                    FILE *f = fopen(path, "r");
                    if (f) { char lbuf[128], out[384]; int on = 0;
                        out[0] = 0;
                        while (fgets(lbuf, sizeof lbuf, f) && on < (int)sizeof out - 128) {
                            if (!strncmp(lbuf, "SigBlk:", 7) || !strncmp(lbuf, "SigIgn:", 7) || !strncmp(lbuf, "SigCgt:", 7)) {
                                char *nl = strchr(lbuf, '\n'); if (nl) *nl = 0;
                                on += snprintf(out + on, sizeof out - on, " %.20s=%.16s", lbuf, lbuf + 7);
                            }
                        }
                        fclose(f);
                        char db[512]; int dn = snprintf(db, sizeof db, "[spin] kill timeout h=%#x tid=%d%s\n", t->handle, t->htid, out);
                        write(2, db, dn); } }
#endif
                continue; }   // 20ms 没应答：撤请求（不撤会错把旧样本当下次命中）
            ok++;
            /* round22b：walker 窗首个成功样本锁存（窗口判据见 g_r22_walk 注释；describe 已在此
               上下文无 G 调用，module_offset 同源安全） */
            if (!atomic_load_explicit(&g_r22_walk.set, memory_order_relaxed)) {
                const char *wnm; uint64_t woff;
                if (runtime_module_offset(g_spin_cap.pc, &wnm, &woff) && !strcmp(wnm, "main")
                    && woff >= 0x963000 && woff < 0x980000) {
                    g_r22_walk.pc = g_spin_cap.pc; g_r22_walk.lr = g_spin_cap.lr; g_r22_walk.x19 = g_spin_cap.x19;
                    atomic_store_explicit(&g_r22_walk.set, 1, memory_order_relaxed);
                }
            }
            char a[96], b[96];
            runtime_describe_address(g_spin_cap.pc, a, sizeof a); runtime_describe_address(g_spin_cap.lr, b, sizeof b);
            /* M6n: main's samples get their own tag ([mn] pc), so "where does main spin" is one grep
               instead of a filter on [spin]. Same sample, same volume -- only the prefix differs. */
            if (bn < (int)sizeof buf - 720)
                bn += snprintf(buf + bn, sizeof buf - bn, "%s h=%#x pc=%s lr=%s x19=%#llx",
                               t->handle == 0x1001u ? "[mn] pc" : "[spin]",
                               t->handle, a, b, (unsigned long long)g_spin_cap.x19);
            uint64_t fp = g_spin_cap.fp;
            for (int d = 0; d < 8 && fp && bn < (int)sizeof buf - 128; d++) {
                uint64_t fr[2];
                if (!spin_read16(fp, fr)) break;   // 非故障读：PROT_NONE/未映射/野指针一律安全失败
                char c[96]; runtime_describe_address(fr[1], c, sizeof c);
                bn += snprintf(buf + bn, sizeof buf - bn, " <- %s", c);
                if (fr[0] == fp) break; fp = fr[0];
            }
            if (bn < (int)sizeof buf - 2) { buf[bn++] = '\n'; }
        }
        pthread_mutex_lock(&G);
        for (int i = 0; i < cand_n; i++) cand[i]->spin_pin = 0;
        pthread_mutex_unlock(&G);
        tot_ok += ok; tot_fail += fail;
        if (ticks % 10 == 1) {   // 2s 心跳：采样器活着 + 候选/命中/未命中累计（诊断用）
            char hb[112]; int hn = snprintf(hb, sizeof hb, "[spin] beat ticks=%u cand=%d ok=%u fail=%u\n", ticks, cand_n, tot_ok, tot_fail);
            write(2, hb, hn);
        }
        if (bn > 0) write(2, buf, bn);
    }
    return NULL;
}
void spin_sampler_start(void) {
    static int started; if (started) return; started = 1;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = spin_cap_handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR2, &sa, NULL) != 0) return;   // 失败即静默放弃：调用点在客体小栈上，不能 fprintf
    pthread_t th;
    if (pthread_create(&th, NULL, spin_sampler, NULL) == 0) pthread_detach(th);
}
/* ---------- M7s (2026-11-07): pin the clip thread 0x102f wait key + its 64 B state window.
   Task: decide who should advance the cutscene "clock". M7r proved the thread parks (wk=2),
   is kicked ~1x/frame, and its 64 B window past the CV lock is byte-static; what is missing
   is (a) the key/cond identity verbatim, (b) whether ANYONE besides main ever signals the
   key, (c) the raw window bytes (M7r only printed CHANGED words, so a never-changing window
   was never dumped), (d) who the thread is (its nn::os::ThreadType arg + a parked-stack fp
   walk naming the guest frames that park it). Read-only; switch `smo_m7s` (C default off,
   `echo 1 >` enables, `echo 0 >` is the one-key rollback), hard cap 20 data lines.
   Line plan (<=20): 1 pin + 1 obj + 1 win-baseline + 1 nb + 2 arg/ascii (per new CV),
   1 walk, <=4 key-ledger lines (2 samples x <=2 clip keys), <=6 win-change lines, 1 end.
   Walk mechanics: one-shot SIGUSR2 via the shared g_spin_cap slot with G HELD (blocks the
   spin sampler from racing the same slot; ~20 ms, twice per boot), then a spin_read16 fp
   walk in this (heartbeat) context -- process_vm_readv on OHOS, safe on unmapped pages. */
#define M7S_PATH_A "/data/storage/el2/base/files/smo_m7s"
#define M7S_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7s"
__attribute__((constructor)) static void m7s_seed(void) {
    if (fopen(M7S_PATH_A, "r")) return;
    FILE *f = fopen(M7S_PATH_A, "w");
    if (!f) f = fopen(M7S_PATH_B, "w");
    if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[m7s] seeded = 0 (off)\n"); }
}
static int m7s_on(void) {
    const char *p; char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        p = i ? M7S_PATH_B : M7S_PATH_A;
        if ((f = fopen(p, "r"))) { size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                                   if (n > 0 && b[0] == '1') return 1; return 0; }
    }
    return 0;
}
void runtime_m7s_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    static int ann, lines;                       /* announce once; hard cap 20 data lines */
    static double next_at; static unsigned last_t;
    static uint64_t plock, pkey; static int pins, pinhave;
    static uint32_t pwin[16]; static uint64_t pwinbase; static int pwinhave, winchg, walkn, sn;
    const unsigned PERIOD = 100u, CAP = 20u;
    int on = m7s_on();
    if (!on) { ann = 0; return; }
    if (!ann) { ann = 1; vk_boot_logf("[m7s] probe on period=%us cap=%u (t=%us)", PERIOD, CAP, tsec);
                next_at = (double)tsec + (double)PERIOD; last_t = tsec; return; }
    if ((double)tsec < next_at || lines >= (int)CAP) return;
    next_at += (double)PERIOD;
    if (last_t && tsec <= last_t) return;
    last_t = tsec;
    if (sn < 7) sn++;
    /* thread record for 0x102f under the usual trylock (entry/arg/pt come from CreateThread) */
    struct { int found, wk; uint64_t key, lock, nsvc, lr, entry, arg; pthread_t pt; } c = {0};
    int locked = 0;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->exited) continue;
        if (t->handle == 0x102fu) { c.found = 1; c.wk = t->wkind; c.key = t->wkey; c.lock = t->waddr;
            c.nsvc = t->nsvc; c.lr = t->last_lr; c.entry = t->entry; c.arg = t->arg;
            c.pt = t->pt; t->spin_pin = 1; }
    }
    if (locked) pthread_mutex_unlock(&G);
    if (!c.found || !c.key) return;
    /* [m7s] key ledger: from the role table (complete for clip keys, unlike the 112-slot [sy]
       tables which overflowed in every M7 boot) + [sy] membership check, 2 of the 7 samples */
    if (sn == 1 || sn == 5) {
        for (unsigned i = 0; i < g_lp[1].n && lines < (int)CAP - 2; i++) {
            lp_key_t *e = &g_lp[1].k[i];
            int insy = 0; uint64_t syw = 0, sys = 0;
            for (unsigned j = 0; j < g_pkey_n && !insy; j++) if (g_pkey[j].key == e->key) { insy = 1; syw = g_pkey[j].n1c; sys = g_pkey[j].n1d; }
            for (unsigned j = 0; j < g_pkey2_n && !insy; j++) if (g_pkey2[j].key == e->key) { insy = 1; syw = g_pkey2[j].n1c; sys = g_pkey2[j].n1d; }
            for (unsigned j = 0; j < g_pkey3_n && !insy; j++) if (g_pkey3[j].key == e->key) { insy = 1; syw = g_pkey3[j].n1c; sys = g_pkey3[j].n1d; }
            vk_boot_logf("[m7s] key t=%us key=%#llx waits=%llu signals=%llu woke=%llu "
                         "by0=h%#x:n=%llu by1=h%#x:n=%llu by2=h%#x:n=%llu oth=%llu sytab=%d(syw=%llu sys=%llu)",
                         tsec, e->key, e->n1c, e->n1d, e->n1dk,
                         e->sig[0].h, e->sig[0].n, e->sig[1].h, e->sig[1].n, e->sig[2].h, e->sig[2].n,
                         e->oth, insy, syw, sys);
            lines++;
        }
    }
    /* pin + full hex on a NEW (lock,key): the identity evidence the task asks for verbatim */
    if (!pinhave || c.lock != plock || c.key != pkey) {
        pinhave = 1; plock = c.lock; pkey = c.key; pins++;
        pwinhave = 0;   /* re-baseline the window on the new CV */
        uint32_t kv = 0, lv = 0; int kr = dump_read_u32(c.key, &kv), lrr = dump_read_u32(c.lock, &lv);
        char lrd[64], end[64]; lrd[0] = 0; end[0] = 0;
        runtime_describe_address(c.lr, lrd, sizeof lrd);
        if (c.entry) runtime_describe_address(c.entry, end, sizeof end);
        vk_boot_logf("[m7s] pin t=%us lock=%#llx key=%#llx keyv=%#x(%s) lockv=%#x nsvc=%llu lr=%s en=%s arg=%#llx",
                     tsec, c.lock, c.key, kv, kr ? (kv ? "parked" : "clear") : "unread", lv,
                     c.nsvc, lrd[0] ? lrd : "-", end[0] ? end : "-", c.arg);
        lines++;
        {   /* full 25-word object window lock-0x40 .. lock+0x24 (same span as [lp] obj, raw hex) */
            char b[560]; int o = 0;
            o += snprintf(b + o, sizeof b - o, "[m7s] obj t=%us base=%#llx", tsec, c.lock - 0x40ull);
            for (int i = 0; i < 25; i++) { uint32_t w = 0; dump_read_u32(c.lock - 0x40ull + 4u * (uint64_t)i, &w);
                o += snprintf(b + o, sizeof b - o, " w%02d=%#x", i, w); }
            vk_boot_logf("%s", b); lines++;
        }
        if (pins == 1) {   /* the M7r 64 B window (lock+0x24 .. lock+0x64) + neighbour pointers, once */
            char b[560]; int o = 0;
            o += snprintf(b + o, sizeof b - o, "[m7s] win t=%us base=%#llx", tsec, c.lock + 0x24ull);
            for (int i = 0; i < 16; i++) { uint32_t w = 0; dump_read_u32(c.lock + 0x24ull + 4u * (uint64_t)i, &w);
                o += snprintf(b + o, sizeof b - o, " w%02d=%#x", i, w); }
            vk_boot_logf("%s", b); lines++;
            for (int i = 0; i < 16; i++) { pwin[i] = 0; dump_read_u32(c.lock + 0x24ull + 4u * (uint64_t)i, &pwin[i]); }
            pwinbase = c.lock + 0x24ull; pwinhave = 1;
            {   /* the two u64 pointers the obj window holds: obj+0x28 and obj+0x60, 4 words at each */
                uint64_t p1 = 0, p2 = 0; uint32_t a = 0, bb = 0;
                if (dump_read_u32(c.lock - 0x40ull + 0x28u, &a) && dump_read_u32(c.lock - 0x40ull + 0x2cu, &bb)) p1 = (uint64_t)a | ((uint64_t)bb << 32);
                a = 0; bb = 0;
                if (dump_read_u32(c.lock - 0x40ull + 0x60u, &a) && dump_read_u32(c.lock - 0x40ull + 0x64u, &bb)) p2 = (uint64_t)a | ((uint64_t)bb << 32);
                char nb2[384]; int n2 = 0;
                n2 += snprintf(nb2 + n2, sizeof nb2 - n2, "[m7s] nb t=%us p1=%#llx p2=%#llx", tsec, p1, p2);
                if (p1 >= 0x100000000ull) { n2 += snprintf(nb2 + n2, sizeof nb2 - n2, " @p1:"); for (int i = 0; i < 4; i++) { uint32_t w = 0; dump_read_u32(p1 + 4u * (uint64_t)i, &w); n2 += snprintf(nb2 + n2, sizeof nb2 - n2, " %08x", w); } }
                if (p2 >= 0x100000000ull) { n2 += snprintf(nb2 + n2, sizeof nb2 - n2, " @p2:"); for (int i = 0; i < 4; i++) { uint32_t w = 0; dump_read_u32(p2 + 4u * (uint64_t)i, &w); n2 += snprintf(nb2 + n2, sizeof nb2 - n2, " %08x", w); } }
                vk_boot_logf("%s", nb2); lines++;
            }
        }
        if (c.arg >= 0x100000000ull && pins <= 2) {   /* ThreadType guess: arg + ASCII scan 0x100 B */
            char b[560]; int o = 0;
            o += snprintf(b + o, sizeof b - o, "[m7s] arg t=%us arg=%#llx:", tsec, c.arg);
            for (int i = 0; i < 32; i++) { uint32_t w = 0; dump_read_u32(c.arg + 4u * (uint64_t)i, &w);
                o += snprintf(b + o, sizeof b - o, " w%02d=%#x", i, w); }
            vk_boot_logf("%s", b); lines++;
            {   /* printable ASCII runs >= 4 in the first 0x100 bytes (thread name / RTTI hint) */
                unsigned char raw[256]; int n = 0;
                for (int i = 0; i < 64; i++) { uint32_t w = 0; if (dump_read_u32(c.arg + 4u * (uint64_t)i, &w)) { raw[n++] = w & 0xff; raw[n++] = (w >> 8) & 0xff; raw[n++] = (w >> 16) & 0xff; raw[n++] = (w >> 24) & 0xff; } else { raw[n++] = 0; raw[n++] = 0; raw[n++] = 0; raw[n++] = 0; } }
                char b2[160]; int o2 = 0;
                o2 += snprintf(b2 + o2, sizeof b2 - o2, "[m7s] ascii t=%us arg=%#llx:", tsec, c.arg);
                int run = 0;
                for (int i = 0; i <= n && o2 < (int)sizeof b2 - 8; i++) {
                    int pr = i < n && raw[i] >= 0x20 && raw[i] < 0x7f;
                    if (pr) run++;
                    else { if (run >= 4) o2 += snprintf(b2 + o2, sizeof b2 - o2, " \"%.48s\"", (const char *)&raw[i - run]);
                           run = 0; }
                }
                vk_boot_logf("%s", b2); lines++;
            }
        }
    }
    /* the 64 B window change watch: every 100 s sample, print ONLY on change (<=6 lines) */
    if (pwinhave && pwinbase == c.lock + 0x24ull && winchg < 6) {
        uint32_t w[16]; int diff = 0;
        for (int i = 0; i < 16; i++) { w[i] = 0; dump_read_u32(pwinbase + 4u * (uint64_t)i, &w[i]); if (w[i] != pwin[i]) diff = 1; }
        if (diff) {
            char b[560]; int o = 0;
            o += snprintf(b + o, sizeof b - o, "[m7s] winchg t=%us base=%#llx", tsec, pwinbase);
            for (int i = 0; i < 16; i++) if (w[i] != pwin[i])
                o += snprintf(b + o, sizeof b - o, " +%#x=%#x(was %#x)", 0x24u + 4u * (uint64_t)i, w[i], pwin[i]);
            vk_boot_logf("%s", b); lines++; winchg++;
        }
        for (int i = 0; i < 16; i++) pwin[i] = w[i];
    }
    /* parked-stack walk: ONE per boot, at sample 2, best-effort (capture + fp walk + stack scan).
       The sampler is PAUSED for the capture (g_m7s_pause + 250 ms settle) so no in-flight
       sampler request can race the shared slot (boot #1 lost its capture to exactly that race:
       the sample came back as the PRESENT thread's pc, nvn_win_present_rgba+0x670). */
    if (c.pt && walkn < 1 && sn == 2 && lines < (int)CAP) {
        walkn++;
        const char *mn; uint64_t moff;
        uint8_t *bsdk = runtime_module_base("sdk"), *bmain = runtime_module_base("main");
        int okn = 0; uint64_t pc = 0, fp = 0, sp = 0;
        g_m7s_pause = 1; usleep(250000);   /* let any in-flight sampler request finish */
        for (int att = 0; att < 3 && !okn; att++) {
            atomic_store(&g_spin_cap.seq, atomic_load(&g_spin_cap.seq) + 1);   /* odd = request */
            if (pthread_kill(c.pt, SIGUSR2) != 0) { atomic_store(&g_spin_cap.seq, atomic_load(&g_spin_cap.seq) + 1); break; }
            int got = 0;
            for (int w = 0; w < 40 && !got; w++) { if (!(atomic_load(&g_spin_cap.seq) & 1)) got = 1; else usleep(500); }
            if (!got) { atomic_store(&g_spin_cap.seq, atomic_load(&g_spin_cap.seq) + 1); continue; }
            pc = g_spin_cap.pc; fp = g_spin_cap.fp; sp = g_spin_cap.sp;
            /* sanity: the parked thread must NOT look like a running graphics host thread
               (boot #1 got nvn_win_present_rgba); accept any capture from a paused sampler */
            okn = 1;
        }
        g_m7s_pause = 0;
        char d[72], b[768]; int o = 0;
        runtime_describe_address(pc, d, sizeof d);
        o += snprintf(b + o, sizeof b - o, "[m7s] walk t=%us got=%d wk=%d pc=%s fp=%#llx sp=%#llx chain:", tsec, okn, c.wk,
                      okn ? d : "-", (unsigned long long)fp, (unsigned long long)sp);
        int guest_frames = 0;
        for (uint64_t f = fp; okn && f && o < (int)sizeof b - 96; ) {
            uint64_t fr[2];
            if (!spin_read16(f, fr) || fr[0] == f) break;
            f = fr[0];
            runtime_describe_address(fr[1], d, sizeof d);
            if (runtime_module_offset(fr[1], &mn, &moff) && (!strcmp(mn, "sdk") || !strcmp(mn, "main") || !strcmp(mn, "subsdk0"))) guest_frames++;
            o += snprintf(b + o, sizeof b - o, " <- %s", d);
        }
        /* fallback: scan the stack for guest-module text pointers (works even when the fp
           chain is broken by non-fp musl frames): 0x800 B up from sp, u64s landing inside
           sdk/main/subsdk0 text ranges, first 6 described */
        if (okn && !guest_frames && sp >= 0x100000000ull && o < (int)sizeof b - 96) {
            int shown = 0;
            o += snprintf(b + o, sizeof b - o, " scan:");
            for (uint64_t a = sp & ~15ull; a < sp + 0x800ull && shown < 6 && o < (int)sizeof b - 96; a += 16) {
                uint64_t v[2];
                if (!spin_read16(a, v)) break;
                for (int h = 0; h < 2; h++) {
                    if (runtime_module_offset(v[h], &mn, &moff) && (!strcmp(mn, "sdk") || !strcmp(mn, "main") || !strcmp(mn, "subsdk0"))) {
                        runtime_describe_address(v[h], d, sizeof d);
                        o += snprintf(b + o, sizeof b - o, " +%#llx=%s", (unsigned long long)(a + 8ull * h - sp), d);
                        shown++;
                    }
                }
            }
            if (!shown) o += snprintf(b + o, sizeof b - o, " none");
        }
        (void)bsdk; (void)bmain;
        vk_boot_logf("%s", b); lines++;
    }
    if (sn == 7)
        vk_boot_logf("[m7s] end t=%us winchg=%d pins=%d win_base=%#llx lines=%d", tsec, winchg, pins, pwinbase, lines);
}
/* ---------- M7t: audio-timeline hypothesis + main's SVC/service window (switch smo_m7t) ----------
   Two 100 s series, cap 12 lines each, one switch (smo_m7t, C default off; rollback = echo 0):
   [m7t] aud  -> who calls the audio renderer and whether the returned positions move. Counters
                 live in audio_renderer.c (per-cmd, per-caller, per-voice sums, elapsed_frames);
                 audio_renderer.c owns the line and the smo_m7t_pos lever refresh.
   [m7t] svc8 -> main 0x1001's top-8 SVCs and top-8 services over the last window: "does main ask
                 audio every frame". SVC side reuses the per-thread svch[256] histogram
                 (svc_thread_body, threads.c) diffed between lines -- no new counting on the hot
                 path; service side is ipc.c's main-only table via runtime_m7t_main_top.
   Read-only; the only write outside the counters is none. */
void runtime_m7t_poll(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern void runtime_m7t_aud_tick(unsigned tsec);
    extern int runtime_m7t_on(void);
    extern void runtime_m7t_main_top(char *out, unsigned cap);
    static int ann; static int lines; static double next_at; static unsigned last_t;
    static uint32_t psvch[256]; static int psvhave;   /* M7y: widened with thr_t.svch */
    const unsigned PERIOD = 100u, CAP = 12u;
    runtime_m7t_aud_tick(tsec);   /* refreshes both switch files + prints its own line on its cadence */
    int on = runtime_m7t_on();
    if (!on) { ann = 0; return; }
    if (!ann) { ann = 1; vk_boot_logf("[m7t] svc8 probe on period=%us cap=%u (t=%us)", PERIOD, CAP, tsec);
                next_at = (double)tsec + (double)PERIOD; last_t = tsec; return; }
    if ((double)tsec < next_at || lines >= (int)CAP) return;
    next_at += (double)PERIOD;
    if (last_t && tsec <= last_t) return;
    last_t = tsec;
    /* main's SVC histogram under the usual trylock of G (same walk as [m7r]) */
    uint32_t cur[256] = {0}; int have = 0, found = 0;   /* M7y: widened with thr_t.svch (memcpy sizeof cur must cover the whole u32 histogram) */
    int locked = 0;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->exited || t->handle != 0x1001u) continue;
        found = 1;
        if (t->svch) { memcpy(cur, t->svch, sizeof cur); have = 1; }
        break;
    }
    if (locked) pthread_mutex_unlock(&G);
    /* top-8 delta since the previous line (first line: since boot; svch saturates, never wraps) */
    unsigned long d[256] = {0};   /* delta indexed BY SVC NUMBER (fixed M7t b1: the compact-array
                                     form let the sort print uninitialized stack as deltas) */
    unsigned idx[256]; unsigned n = 0;
    for (int i = 0; i < 256; i++) {
        unsigned long delta = have && psvhave ? (cur[i] >= psvch[i] ? (unsigned long)(cur[i] - psvch[i]) : 0)
                                              : (unsigned long)cur[i];
        if (delta) { d[i] = delta; idx[n] = (unsigned)i; n++; }
    }
    for (unsigned i = 0; i < n; i++) {   /* insertion sort idx by d[idx] desc */
        unsigned k = idx[i]; int j = i;
        while (j > 0 && d[idx[j - 1]] < d[k]) { idx[j] = idx[j - 1]; j--; }
        idx[j] = k;
    }
    char top[192]; unsigned o = 0;
    for (unsigned i = 0; i < n && i < 8 && o + 2 < sizeof top; i++)
        o += (unsigned)snprintf(top + o, sizeof top - o, "%s0x%x=%lu", i ? " " : "", idx[i], d[idx[i]]);
    top[o < sizeof top ? o : sizeof top - 1] = 0;
    memcpy(psvch, cur, sizeof psvch); psvhave = have;
    char ipc[320]; ipc[0] = 0;
    runtime_m7t_main_top(ipc, sizeof ipc);
    vk_boot_logf("[m7t] svc8 t=%us main=%s svc8=[%s] ipc8=[%s]", tsec, found ? (have ? "ok" : "nosvch") : "absent", top, ipc);
    lines++;
}
__attribute__((constructor)) static void dump_init(void) {
#if SMO_OBS_HEAVY
    if (!g_early1d_t0_set) { g_early1d_t0 = now_ns(); g_early1d_t0_set = 1; }   // round16b：早 boot 10s 窗口起点=libgame 装载时刻
#endif
    const char *c = getenv("TOTK_LISTCHECK");
    if (c) { pthread_t th; pthread_create(&th, NULL, list_check, (void *)(uintptr_t)strtoull(c, NULL, 0)); pthread_detach(th); }
    const char *e = getenv("TOTK_DUMP_AFTER"); if (!e) return;
    pthread_t th; pthread_create(&th, NULL, dump_after, (void *)(uintptr_t)strtoul(e, NULL, 10)); pthread_detach(th);
}

/* ================= M40W (2026-11-10, READ-ONLY probe `smo_m40w`) =================
   Question (M40V): h=0x1023 polls h=0x101f at ~200 Hz with a timeout; 0x101f (our handle type 2 =
   O_EVENT_R, i.e. the READ end of an event pair) kept `signals=0` for a whole boot while
   `resets` reached 115479, and main (0x1001) burned only 0.14 % CPU. Who created 0x101f, was its
   WRITE END ever signalled, who calls ResetSignal on it, and what is 0x1023 waiting for?
   Facts this probe adds (no SVC, no wait, no guest memory write is touched):
     1) the pair: 0x101f's event_t gives hw (write end), cre_thr/cre_lr/src/born_ns. src separates
        a guest CreateEvent pair (0) from an HLE service event (1) / readable copy (2), and for
        src 0 the creator LR is a guest address (runtime_describe_address names its module);
     2) the WRITE END's ledger: g_evcnt[wr][0]=signals (svc 0x11 + host runtime_event_signal),
        g_evcnt[wr][1]=resets, g_evlast_thr/lr = last actor. A READ end can never be signalled
        (svc 0x11 accepts O_EVENT_W only), so "0x101f signals=0" on its own proves nothing;
     3) the read end's own reset counter plus the FIRST reset call site (g_evfirst[h][1]) = the
        200 Hz poll loop's guest LR, so the poller can be named;
     4) the waiter: every live thread parked in WaitSynchronization on 0x101f or on its write
        end; for 0x1023 the pre-existing calls/woken/woken_by_0x101f counters ride along.
   Cadence: one line when a tracked field changes, plus a 15 s quiet line; OWNER once, FIRSTSIG
   once, NOSIG at 240 s and 540 s when the write end never carried a signal. Hard cap 240 ev
   lines so a whole boot stays under the 250-line probe budget. Heartbeat thread only.
   With smo_m40w=0 this is one cached switch read per 8 ticks and nothing else. */
#define M40W_SW_A "/data/storage/el2/base/files/smo_m40w"
#define M40W_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40w"
#define M40W_H 0x101fu
static int m40w_on(void)
{
    static int v = -1; static unsigned tick;
    if (v >= 0 && (tick++ & 7u)) return v;
    v = 0;
    { static const char *const p[2] = { M40W_SW_A, M40W_SW_B };
      for (int i = 0; i < 2; i++) { FILE *f = fopen(p[i], "r");
        if (f) { int x = 0; if (fscanf(f, "%d", &x) == 1 && x > 0) v = 1; fclose(f); break; } } }
    return v;
}
__attribute__((constructor)) static void m40w_seed(void)
{
    static const char *const p[2] = { M40W_SW_A, M40W_SW_B };
    for (int i = 0; i < 2; i++) {
        FILE *r = fopen(p[i], "r");
        if (r) { fclose(r); continue; }
        { FILE *f = fopen(p[i], "w"); if (f) { fputs("0\n", f); fclose(f); } }
    }
}
static unsigned m40w_ticks, m40w_lines, m40w_owner, m40w_firstsig, m40w_nosig1, m40w_nosig2;
void runtime_m40w_poll(unsigned tsec)
{
    extern void vk_boot_logf(const char *fmt, ...);
    if (!m40w_on()) return;
    m40w_ticks++;
    int ty = 0, sg = -1, refs = 0, src = -1, wrslotty = -1; uint32_t wr = 0, cre = 0, wrlastthr = 0;
    uint64_t crelr = 0, born = 0, rdsig = 0, rdrst = 0, rdrstlr = 0, wrsig = 0, wrrst = 0, wrlastlr = 0;
    uint32_t waiter = 0, wnow = 0, wl[4] = {0, 0, 0, 0}; int nwl = 0;
    int t23 = 0, prio23 = 0, core23 = -1, nw23 = 0; uint32_t w23[4] = {0, 0, 0, 0};
    uint64_t lr23 = 0, nsvc23 = 0, c23 = 0, w23ok = 0, w23_101f = 0;
    unsigned tot = atomic_load_explicit(&g_evpair_n, memory_order_relaxed);
    unsigned pend = tot < EVPAIR_N ? tot : EVPAIR_N;
    int locked = 0;
    for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(1000);
    if (!locked) { if (!(m40w_ticks % 5u)) vk_boot_logf("[m40w] t=%us G busy, sample skipped", tsec); return; }
    unsigned ri = M40W_H - 0x1000u;
    ty = g_h[ri].type;
    event_t *e = h_peek(M40W_H, O_EVENT_R);
    if (e) { sg = e->signaled; refs = e->refs; src = e->src; wr = e->hw;
             cre = e->cre_thr; crelr = e->cre_lr; born = e->born_ns; }
    else {   /* read end gone / recycled: fall back to the pair ledger, which keeps the history */
        for (unsigned i = 0; i < pend; i++) if (g_evpair[i].rd == M40W_H) {
            wr = g_evpair[i].wr; cre = g_evpair[i].by; src = g_evpair[i].src; crelr = g_evpair[i].lr; born = g_evpair[i].ns; }
    }
    rdsig = atomic_load_explicit(&g_evcnt[ri][0], memory_order_relaxed);
    rdrst = atomic_load_explicit(&g_evcnt[ri][1], memory_order_relaxed);
    rdrstlr = atomic_load_explicit(&g_evfirst[ri][1], memory_order_relaxed);
    if (wr) {
        unsigned wi = wr - 0x1000u;
        if (wi > 0 && wi < MAX_H) {
            wrslotty = g_h[wi].type;
            wrsig = atomic_load_explicit(&g_evcnt[wi][0], memory_order_relaxed);
            wrrst = atomic_load_explicit(&g_evcnt[wi][1], memory_order_relaxed);
            wrlastthr = g_evlast_thr[wi][0]; wrlastlr = g_evlast_lr[wi][0];
        }
    }
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->exited) continue;
        if (t->handle == 0x1023u) {
            t23 = 1; prio23 = t->prio; core23 = atomic_load_explicit(&t->cur_core, memory_order_relaxed);
            lr23 = t->last_lr; nsvc23 = t->nsvc; nw23 = t->nwh < 4 ? t->nwh : 4;
            for (int i = 0; i < nw23; i++) w23[i] = t->wh[i];
        }
        if (t->wkind != 4) continue;
        int hit = 0;
        for (int i = 0; i < t->nwh && i < 4; i++) if (t->wh[i] == M40W_H || (wr && t->wh[i] == wr)) hit = 1;
        if (hit && !waiter) {
            waiter = t->handle; wnow = t->nwh > 0 ? t->wh[0] : 0;
            nwl = t->nwh < 4 ? t->nwh : 4;
            for (int i = 0; i < nwl; i++) wl[i] = t->wh[i];
        }
    }
    c23 = (uint64_t)g_w1023_calls; w23ok = (uint64_t)g_w1023_wake; w23_101f = (uint64_t)g_w1023_wake_101f;
    pthread_mutex_unlock(&G);
    /* ---- OWNER: one line, as soon as the handle is resolvable ---- */
    if (!m40w_owner && (ty || wr)) {
        char cl[80], pl[80], bl[80];
        runtime_describe_address(crelr, cl, sizeof cl);
        runtime_describe_address(rdrstlr, bl, sizeof bl);
        pl[0] = 0;
        m40w_owner = 1;
        vk_boot_logf("[m40w] OWNER t=%us h=%#x ty=%u src=%d cre_thr=%#x cre_lr=%s born=%us_ago refs=%d wr=%#x wr_slot_ty=%d",
                     tsec, M40W_H, (unsigned)ty, src, cre, crelr ? cl : "-",
                     born ? (unsigned)((now_ns() - born) / 1000000000ull) : 0u, refs, wr, wrslotty);
        {   /* every creation record that names this read end: the whole pair history, not just the newest */
            unsigned n = 0;
            for (unsigned i = 0; i < pend && n < 8; i++) {
                char q[80]; q[0] = 0; runtime_describe_address(g_evpair[i].lr, q, sizeof q);
                if (g_evpair[i].rd != M40W_H && g_evpair[i].wr != M40W_H) continue;
                vk_boot_logf("[m40w] PAIR t=%us rd=%#x wr=%#x src=%d by=%#x lr=%s",
                             tsec, g_evpair[i].rd, g_evpair[i].wr, g_evpair[i].src, g_evpair[i].by, q[0] ? q : "-");
                n++;
            }
            if (!n) vk_boot_logf("[m40w] PAIR t=%us no creation record names %#x (%u records)", tsec, M40W_H, tot);
        }
    }
    /* ---- ev line: on change, else every 15 s (cap 240 lines) ---- */
    { static int pl_sg = -2, pl_ty = -2; static uint32_t pl_wr, pl_waiter; static uint64_t pl_wrsig, pl_rdsig;
      static unsigned pl_t;
      int chg = (sg != pl_sg) || ((int)ty != pl_ty) || (wr != pl_wr) || (wrsig != pl_wrsig) ||
                (rdsig != pl_rdsig) || (waiter != pl_waiter) || (wnow != 0 && wnow != pl_waiter);
      if ((chg || !pl_t || tsec - pl_t >= 15u) && m40w_lines < 240u) {
          char rl[80], wll[80], l23[80];
          wll[0] = l23[0] = 0;
          runtime_describe_address(rdrstlr, rl, sizeof rl);
          if (wrlastlr) runtime_describe_address(wrlastlr, wll, sizeof wll);
          if (lr23) runtime_describe_address(lr23, l23, sizeof l23);
          vk_boot_logf("[m40w] ev t=%us h=%#x ty=%u sig=%d refs=%d rdsig=%llu rdrst=%llu rdrst_lr=%s | wr=%#x wrsig=%llu wrrst=%llu wrlast=%#x@%s | wait=%#x now=%#x wl=%#x,%#x,%#x,%#x | t23=%d prio=%d core=%d wh=%#x,%#x,%#x,%#x lr23=%s calls=%llu woken=%llu woken101f=%llu nsvc23=%llu",
                       tsec, M40W_H, (unsigned)ty, sg, refs, (unsigned long long)rdsig, (unsigned long long)rdrst, rdrstlr ? rl : "-",
                       wr, (unsigned long long)wrsig, (unsigned long long)wrrst, wrlastthr, wll[0] ? wll : "-",
                       waiter, wnow, wl[0], wl[1], wl[2], wl[3],
                       t23, prio23, core23, w23[0], w23[1], w23[2], w23[3], l23[0] ? l23 : "-",
                       (unsigned long long)c23, (unsigned long long)w23ok, (unsigned long long)w23_101f, (unsigned long long)nsvc23);
          m40w_lines++; pl_sg = sg; pl_ty = (int)ty; pl_wr = wr; pl_waiter = waiter;
          pl_wrsig = wrsig; pl_rdsig = rdsig; pl_t = tsec;
      }
    }
    /* ---- FIRSTSIG / NOSIG: the one thing the parent hypothesis hangs on ---- */
    if ((wrsig > 0 || sg == 1 || rdsig > 0) && !m40w_firstsig) {
        char wll[80]; wll[0] = 0;
        if (wrlastlr) runtime_describe_address(wrlastlr, wll, sizeof wll);
        m40w_firstsig = 1;
        vk_boot_logf("[m40w] FIRSTSIG t=%us h=%#x wr=%#x wrsig=%llu by=%#x lr=%s rdsig=%llu rdsig_now=%d",
                     tsec, M40W_H, wr, (unsigned long long)wrsig, wrlastthr, wll[0] ? wll : "-",
                     (unsigned long long)rdsig, sg);
    }
    if (!m40w_firstsig && wrsig == 0 && sg != 1) {
        if (!m40w_nosig1 && tsec >= 240u) {
            char rl[80]; rl[0] = 0; runtime_describe_address(rdrstlr, rl, sizeof rl); m40w_nosig1 = 1;
            vk_boot_logf("[m40w] NOSIG t=%us h=%#x wr=%#x wrsig=0 rdsig=0 rdrst=%llu rdrst_lr=%s wr_slot_ty=%d ticks=%u (write end never signalled in %us)",
                         tsec, M40W_H, wr, (unsigned long long)rdrst, rl[0] ? rl : "-", wrslotty, m40w_ticks, tsec);
        }
        if (!m40w_nosig2 && tsec >= 540u) {
            char rl[80]; rl[0] = 0; runtime_describe_address(rdrstlr, rl, sizeof rl); m40w_nosig2 = 1;
            vk_boot_logf("[m40w] NOSIG t=%us h=%#x wr=%#x wrsig=0 rdsig=0 rdrst=%llu rdrst_lr=%s (final check at 540 s)",
                         tsec, M40W_H, wr, (unsigned long long)rdrst, rl[0] ? rl : "-");
        }
    }
}
