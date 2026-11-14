// M45A / S2b-2：**执行路径切换开关**（我方直喂 nvn_vk.c → nx2 录制/回放后端 rec/ → gfx_vk_exec → gfx_vk_*）
//
// 这一刀只做一件事：把 gfx_bridge.c 的 nvn_lookup() 变成二选一的路口。
//   · smo_m45a = 0（缺省，构造期自种 0666 文件）：逐字节旧路 —— nvn_lookup 原表原样生效，本文件除种文件外零副作用。
//   · smo_m45a = 1：537 个 NVN 名字一律转发 nx2 的 ELF 跳板 nvn_thunk[i]（rec/nvn_thunks_elf.S）。
//     跳板进来先 stadd 计数，再 br 到 nvn_slot[i].fn；该槽由 nvn_nx2_wire()（rec/hle_nvn.c 新导出）一次性接好：
//       语义实现 393 + 空实现 13（g_nops）+ 非 CommandBuffer 函数套大栈入口 nvn_big_entry + 其余落 nvn_unimpl_entry（红报桩）。
//     于是 NVN 调用 → nx2 hle_nvn.c 录制（写 nx2 自己的命令缓冲）→ nvnQueueSubmitCommands → mtl_exec → gfx_vk_exec 回放。
//
// 为什么切在 nvn_lookup（证据）：客体的 nvn 导入有两条解析道 —— ① 装载期 ELF 导入 → runtime_gfx_override()
//   → gfx_bridge.c:6856 nvn_lookup()；② 运行期 nvnDeviceGetProcAddress → nvn_get_proc_address() → nvn_lookup()。
//   两条都只过这一个函数，所以它是唯一咽喉（不是 entries[] 表本身）。
//   包装层仍按旧路：nvn_lookup 返回**裸**跳板指针，由两个调用点各自 runtime_hle_wrap_named()（同 fn→同包装⇒指针比较一致）。
//
// 本文件同时负责三件"证据/夹具"事（都不改回放语义）：
//   ① 每 5 s 一行（限频）`[m45a] exec backend=nx2 frames=… draws=… skip_prog=… skip_pipe=… skip_rt=… passes=…`
//      frames = 跳板计数里的 nvnQueueAcquireTexture 次数；draws = 各 draw 记录器的跳板计数之和（记录=会回放）；
//      三个 skip_* 由 gfx_vk_exec.c 新增的只读快照 vk_exec_stats() 给出（skip_prog 大 = DMSV shader bundle 缺）。
//   ② RT dump：DM_ROOT 指到沙箱可写目录，t≥90 s 把 nx2 的 pass dump 打开一次（DM_VK_PASSDUMP=1 / _FRAMES=1）。
//      nx2 原版由 present 层每帧驱动 vk_pass_dump_poll()，本移植 present 还是 S4 空桩 ⇒ 由本文件的 ticker 驱动。
//   ③ DM_ROOT 里会顺带留下 nx2 的 captured_shaders/<key>.bin（游戏真正用到的着色器控制段+代码区），
//      这是 S3「bundle 通电」的直接输入。
//
// 纪律：M45A 不写游戏内存、不改客体代码；除 nvn_lookup 的分流外只增加日志与文件。
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nvn_count.h"   // NVN_NFUNC = 537（nx2 gen_protos.py 生成，与 nvn_thunks_elf.S 同源）

#define M45A_SW_A "/data/storage/el2/base/files/smo_m45a"                          /* app 沙箱视图（构造期只能在此种植） */
#define M45A_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m45a"           /* hdc 唯一可写视图（优先读） */
#define M45A_DRT_A "/data/storage/el2/base/files/smo_m45a_dirty"                     /* 脏页写保护（缺省 0 = 不跟踪） */
#define M45A_DRT_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m45a_dirty"
#define M45A_ROOT_A "/data/storage/el2/base/files/m45a_dump"                       /* DM_ROOT：pass dump / captured_shaders */
#define M45A_DUMP_AT 90                                                            /* 秒：第一次 dump 的时刻（场景已起） */
/* M49A：dump 时刻改成运行期旋钮（默认仍是 90 s，可 30..900）——把 dump 窗口从世界地图挪到 gameplay 用。
   这也修掉「一个进程只能 dump 一次」带来的取样时刻死角（g_pd_state 到 2 后不再复位）。 */
#define M45A_DAT_A "/data/storage/el2/base/files/smo_m45a_dump_at"
#define M45A_DAT_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m45a_dump_at"
#define M45A_DFR_A "/data/storage/el2/base/files/smo_m45a_dump_frames"             /* dump 连续几个 present（相邻帧 diff8 用） */
#define M45A_DFR_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m45a_dump_frames"

extern void *nvn_thunk[NVN_NFUNC];          /* rec/nvn_thunks_elf.S（.hidden，DSO 内可见） */
extern const char *nvn_names[NVN_NFUNC];
typedef struct { _Atomic uint64_t cnt; void *fn; } m45a_slot_t;
extern m45a_slot_t nvn_slot[NVN_NFUNC];    /* rec/hle_nvn.c（布局与跳板的 i*16 步长一致） */
extern int nvn_nx2_wire(void);             /* rec/hle_nvn.c 新增导出：接槽表，返回语义实现数 */
extern void vk_boot_logf(const char *fmt, ...);
extern void vk_pass_dump_poll(uint64_t done_fence);   /* rec/gfx_vk_exec.c（原版由 present 层驱动） */
extern uint64_t mtl_last_fence(void);                  /* nvn_vk2/abi/nvn_mtl.h */
extern void vk_exec_stats(uint64_t out[8]);            /* rec/gfx_vk_exec.c 新增：只读快照 */

static _Atomic int g_ticker_up;
static _Atomic int g_nsem = -1;

static int m45a_idx(const char *n)
{
    if (!n) return -1;
    for (int i = 0; i < NVN_NFUNC; i++) if (!strcmp(nvn_names[i], n)) return i;
    return -1;
}

/* ---- 开关：B 视图优先（hdc 可写），A 视图兜底；2 s 重读 ⇒ 写 0 即秒级回退，不用重装 ---- */
int m45a_on(void)
{
    static int cached = -1; static long last;
    long now = (long)time(NULL);
    if (cached >= 0 && now < last + 2) return cached;
    last = now; cached = 0;
    { const char *paths[2] = { M45A_SW_B, M45A_SW_A };
      for (int i = 0; i < 2; i++) { FILE *f = fopen(paths[i], "r");
        if (f) { char b[16]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                 if (n > 0) { b[n] = 0; cached = atoi(b); } break; } } }
    return cached & 1;
}

/* ---- dump 连续帧数（缺省 3；文件 smo_m45a_dump_frames 可改，1..8） ---- */
static int m45a_dump_frames(void)
{
    static int cached = -1; static long last;
    long now = (long)time(NULL);
    if (cached >= 0 && now < last + 2) return cached;
    last = now; cached = 3;
    { const char *paths[2] = { M45A_DFR_B, M45A_DFR_A };
      for (int i = 0; i < 2; i++) { FILE *f = fopen(paths[i], "r");
        if (f) { char b[16]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                 if (n > 0) { b[n] = 0; int v = atoi(b); if (v >= 1 && v <= 8) cached = v; } break; } } }
    return cached;
}

/* ---- M49A：第一次 dump 的时刻（秒；缺省 M45A_DUMP_AT=90；B 视图优先，2 s 重读） ---- */
static int m45a_dump_at(void)
{
    static int cached = -1; static long last;
    long now = (long)time(NULL);
    if (cached >= 0 && now < last + 2) return cached;
    last = now; cached = M45A_DUMP_AT;
    { const char *paths[2] = { M45A_DAT_B, M45A_DAT_A };
      for (int i = 0; i < 2; i++) { FILE *f = fopen(paths[i], "r");
        if (f) { char b[16]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                 if (n > 0) { b[n] = 0; int v = atoi(b); if (v >= 30 && v <= 900) cached = v; } break; } } }
    return cached;
}

/* ---- 脏页写保护开关（缺省 0）。nx2 的 nvn_dirty 把池页 mprotect 成只读、靠缺页回填继续写；
       OHOS 上这条回填路径未验证（DFX 会在页面保护 fault 后判死进程），所以默认不注册跟踪范围 ⇒ 池页永远可写。
       打开 = smo_m45a_dirty=1（S3 贴图重传时才需要）。 ---- */
int m45a_dirty_track(void)
{
    static int cached = -1; static long last;
    long now = (long)time(NULL);
    if (cached >= 0 && now < last + 2) return cached;
    last = now; cached = 0;
    { const char *paths[2] = { M45A_DRT_B, M45A_DRT_A };
      for (int i = 0; i < 2; i++) { FILE *f = fopen(paths[i], "r");
        if (f) { char b[16]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                 if (n > 0) { b[n] = 0; cached = atoi(b); } break; } } }
    return cached & 1;
}

/* ---- 槽表接入（一次）。nx2 原版由 constructor 做，M43A 搬入时停用（那时不切执行）；
       现在 M45A 真正需要它，改为**第一次命中时**做，且结果 <300 视为接线失败 ⇒ 整体不切（宁可不切，也不跳空指针）。 ---- */
static void m45a_wire_slots(void)
{
    int n = nvn_nx2_wire();
    atomic_store(&g_nsem, n);
    vk_boot_logf("[m45a] nx2 slot table wired: %d semantic impls over %d names%s", n, NVN_NFUNC, n < 300 ? " (TOO FEW -> switch disabled)" : "");
}

void *m45a_thunk_for(const char *name)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    if (!m45a_on()) return NULL;
    pthread_once(&once, m45a_wire_slots);
    if (atomic_load(&g_nsem) < 300) return NULL;
    int i = m45a_idx(name);
    return i < 0 ? NULL : nvn_thunk[i];
}

/* ---- 跳板计数 → 证据口径 ---- */
static uint64_t slot_cnt(const char *n)
{
    int i = m45a_idx(n);
    return i < 0 ? 0 : atomic_load(&nvn_slot[i].cnt);
}
static uint64_t m45a_draws(void)
{
    static const char *const d[] = {
        "nvnCommandBufferDrawArrays", "nvnCommandBufferDrawArraysIndirect", "nvnCommandBufferDrawArraysInstanced",
        "nvnCommandBufferDrawElements", "nvnCommandBufferDrawElementsBaseVertex", "nvnCommandBufferDrawElementsIndirect",
        "nvnCommandBufferDrawElementsInstanced", "nvnCommandBufferDrawTexture", "nvnCommandBufferDrawTransformFeedback",
        "nvnCommandBufferMultiDrawArraysIndirectCount", "nvnCommandBufferMultiDrawElementsIndirectCount" };
    uint64_t s = 0;
    for (unsigned i = 0; i < sizeof d / sizeof *d; i++) s += slot_cnt(d[i]);
    return s;
}

static void *m45a_ticker(void *arg)
{
    (void)arg;
    time_t t0 = time(NULL), last_log = 0;
    int dumped = 0;
    for (;;) {
        struct timespec ts = { 0, 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (!m45a_on()) continue;                 /* 关态：静默（开关可以再打开，线程不退出） */
        long t = (long)(time(NULL) - t0);
        if (!dumped && t >= m45a_dump_at()) {
            setenv("DM_VK_PASSDUMP", "1", 1);     /* 下一笔提交开始时开 dump（读取见 gfx_vk_exec.c:474） */
            dumped = 1;
            vk_boot_logf("[m45a] RT dump armed at t=%lds (DM_VK_PASSDUMP=1, FRAMES=%d, DM_ROOT=%s)", t, m45a_dump_frames(), getenv("DM_ROOT") ? getenv("DM_ROOT") : "(unset)");
        }
        if (dumped) vk_pass_dump_poll(mtl_last_fence());   /* nx2 原版由 present 层驱动；S4 还是空桩，故此处代驱动 */
        { time_t now = time(NULL);
          if (now >= last_log + 5) {
              uint64_t st[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
              last_log = now;
              vk_exec_stats(st);
              /* draws = 跳板计数（游戏真正录进 nx2 命令流的 draw 笔数）；issued = 回放实际发出的 draw（需要 shader bundle）；
                 skip_prog = 因「程序的两级 shader 不在 DMSV bundle 里」被丢弃的回放尝试（含 OP_CALL 复制命令流的重复回放）。 */
              vk_boot_logf("[m45a] exec backend=nx2 frames=%lu draws=%lu issued=%lu skip_prog=%lu skip_pipe=%lu skip_rt=%lu passes=%lu pipes=%lu rec=%lu submits=%lu presents=%lu t=%lds",
                           (unsigned long)slot_cnt("nvnQueuePresentTexture"), (unsigned long)m45a_draws(), (unsigned long)st[0],
                           (unsigned long)st[1], (unsigned long)st[3], (unsigned long)st[2], (unsigned long)st[4], (unsigned long)st[5],
                           (unsigned long)slot_cnt("nvnCommandBufferBeginRecording"),
                           (unsigned long)slot_cnt("nvnQueueSubmitCommands"), (unsigned long)slot_cnt("nvnQueuePresentTexture"), t);
          } }
    }
    return NULL;
}

static void m45a_start_ticker(void)
{
    if (atomic_exchange(&g_ticker_up, 1)) return;
    pthread_t th;
    if (pthread_create(&th, NULL, m45a_ticker, NULL) == 0) pthread_detach(th);
    else atomic_store(&g_ticker_up, 0);
}

/* 构造期：① 种 0666 开关文件（hdc 无法在 app 目录新建）② 开态时备 RT dump 的落点。不在这里打日志（vk_boot 可能还没就绪）。 */
__attribute__((constructor)) static void m45a_seed(void)
{
    static const char *const p[8] = { M45A_SW_A, M45A_SW_B, M45A_DRT_A, M45A_DRT_B, M45A_DFR_A, M45A_DFR_B, M45A_DAT_A, M45A_DAT_B };
    for (int i = 0; i < 8; i++) {
        if (access(p[i], F_OK) == 0) { chmod(p[i], 0666); continue; }
        int fd = open(p[i], O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (fd >= 0) { ssize_t w = write(fd, "0\n", 2); (void)w; close(fd); chmod(p[i], 0666); }
    }
    { extern void m48a_seed_files(void); m48a_seed_files(); }   // M48A：本单 6 个开关也在这条已验证的种法里种
    if (!m45a_on()) return;
    if (mkdir(M45A_ROOT_A, 0755) == 0 || access(M45A_ROOT_A, F_OK) == 0) { setenv("DM_ROOT", M45A_ROOT_A, 1); chmod(M45A_ROOT_A, 0777); }   /* M49A：0777 让 hdc(shell uid 2000) 能删旧 dump（M48A 的「glob 不展开」其实是目录不可写） */
    // M46A：bundle 落点占位。hdc 不能在 app 沙箱里"新建"文件，只能"覆盖已存在文件的内容"（M44B 实测），
    // 所以开态时先由 app 自己建好一个空的 <DM_ROOT>/shaders.dmsv（**不截断**已存在的，否则重装/重启会把投好的 bundle 清零），
    // 之后 `hdc file send full.dmsv <B 视图路径>/shaders.dmsv` 覆盖其内容即可。gfx_vk_shader.c:42 用 DM_ROOT 拼的正是这个路径。
    {
        const char *bp = M45A_ROOT_A "/shaders.dmsv";
        if (access(bp, F_OK) != 0) { int fd = open(bp, O_CREAT | O_WRONLY, 0666); if (fd >= 0) close(fd); }
        chmod(bp, 0666);
    }
    { char fb[8]; snprintf(fb, sizeof fb, "%d", m45a_dump_frames()); setenv("DM_VK_PASSDUMP_FRAMES", fb, 1); }
    m45a_start_ticker();
}
