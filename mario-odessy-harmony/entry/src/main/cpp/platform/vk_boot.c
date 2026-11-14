/* vk_boot.c — 把随包 SwiftShader Vulkan 栈接进应用进程（round：vkCreateInstance 恒 -9 修复）。
 *
 * 背景：应用 CMake find_library(VULKAN_LIB vulkan) 链接期直连系统 libvulkan.so（OHOS fork
 * loader，不认 env 注入的外部 ICD）⇒ vkCreateInstance 恒 VK_ERROR_INCOMPATIBLE_DRIVER(-9)。
 * 方案（最小路）：entry/libs/arm64-v8a/ 放同名 libvulkan.so（内容=打过 OHOS WSI 补丁的
 * Khronos loader）+ libvk_swiftshader.so + vk_swiftshader_icd.json + libc++_shared.so
 * （llvm-readelf -d 查得 libvk_swiftshader.so NEEDED libc++_shared.so、libc.so）。
 * 同名库占位：OHOS 链接器解析 DT_NEEDED 时应用自带 libs 优先于系统库，libgame.so 的
 * libvulkan.so 需求落到随包 loader 上；loader 再按 VK_ICD_FILENAMES 加载 SwiftShader ICD。
 *
 * 时机：constructor 优先级 101，早于 applet_host.c:15 等无优先级（默认）constructor；
 * nvn_vk.c nvn_vk_init 只在窗口/真机运行期被调，远晚于本构造器。loader 读 env 发生在
 * vkCreateInstance 调用时，因此即使构造顺序异常，setenv 本身也无竞态危害（幂等，overwrite=1）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>        /* M3p：jit.log 预建/放宽权限 */
#include <dirent.h>       /* M5w：[hang] 探针读 /proc/self/task/*/stat */
#include <sys/stat.h>     /* fchmod */
#include <sys/time.h>
#include <dlfcn.h>
#include <pthread.h>
#include <hilog/log.h>
#include "vk_boot.h"

/* M3b：诊断行一律走 hilog。此前全用 fprintf(stderr)，实测 boot 抓包里 [vk-boot] 一行都看不到
 * （stderr 未进 hilog 环形缓冲），"setenv 没生效"的判断缺证据。tag 与 nvn_vk.c 同款 SmoVK，
 * 一个 grep 就能把 vk 链路串起来。 */
#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "SmoVK"
#define LOG_DOMAIN 0x0000
#define VB_LOG(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)

/* 写 manifest；返回 1 成功，0 失败并把首个 errno 带出（errno 为 0 表示写/关正常但 fprintf 失败）。 */
static int vb_write_manifest(const char *path, const char *libdir, int *errp)
{
    *errp = 0;
    FILE *f = fopen(path, "w");
    if (!f) { *errp = errno; return 0; }
    int wok = fprintf(f,
        "{\n  \"file_format_version\": \"1.0.0\",\n  \"ICD\": {\n"
        "    \"library_path\": \"%s/libvk_swiftshader.so\",\n"
        "    \"api_version\": \"1.0.5\"\n  }\n}\n", libdir) > 0;
    int werr = errno;
    int cok = fclose(f) == 0;
    int cerr = errno;
    if (!wok) { *errp = werr; return 0; }
    if (!cok) { *errp = cerr; return 0; }
    return 1;
}

static char g_icd_so[PATH_MAX];

const char *vk_boot_icd_path(void) { return g_icd_so[0] ? g_icd_so : NULL; }

/* ---------------- M3c：不可丢的取证通道 ----------------
 * 依据（派活单口径）：hilog 首波实测丢 505 行，vkCreateDevice 前后的关键行只活下来 1/3；
 * 验收行必须走应用自写文件。append + 每行 fflush：进程中途崩/被杀也不丢已写的行。
 * 写点按沙箱常量路径依次试（files 优先），真实落点由 vk_boot_log_path() 反查。 */
static FILE *g_vklog;
static char g_vklog_path[PATH_MAX];
/* M3g：多个线程（客体线程 + 2 秒心跳线程）同时写同一 FILE*，一次 vk_boot_logf 有三段 stdio 调用
 * （前缀 fprintf / 正文 vfprintf / 换行 fputc），不串行化会把两行拼成一行、丢掉可判读性。 */
static pthread_mutex_t g_vklog_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---------------- M4n P2：vk_boot.log 大小上限轮转 ----------------
 * 实测单次 boot 的 vk_boot.log 达 1.4 GB（设备侧 2026-11-03 13:56 读数 1478477377 B），
 * 起因是逐帧取证行（gfx_bridge 的 [h] submit 族），一轮跑完能把盘吃满。这里给文件加硬上限：
 * 每写 VKLOG_CHECK_EVERY 行 fstat 一次（256 行一次 stat，可忽略），st_size 超 VKLOG_MAX_BYTES
 * 就把当前文件滚成 <path>.1（只留一段，故稳定态占用上界 ≈ 2×200 MB），再以 append 重开 <path>。
 * 关键取舍：
 *   ①在 g_vklog_mu 内执行，滚动与写入不会交叉；
 *   ②任何一步失败都降级为「继续写原文件」（rename 失败 ⇒ 直接返回；重开失败 ⇒ 把 .1 改回原名），
 *     绝不让轮转让日志通道消失；
 *   ③跨 boot：打开时先做一次收敛（vklog_trim_at_open），上一段 boot 遗留的超大文件立即滚走/清掉，
 *     本次 boot 一定从空文件写起 —— 「每次 boot 吃满盘」在这一跳被掐断。
 * 为什么落在这个文件而不是 nvn_vk.c：FILE* g_vklog 与候选路径表都是本文件的私有静态量，
 * nvn_vk.c 只经 vk_boot_logf() 间接写盘，在那里既看不到也改不了这个流。 */
#define VKLOG_MAX_BYTES   (200u * 1024u * 1024u)   /* 单段上限；滚动后旧段另占一份，合计 ≤ ~400 MB */
#define VKLOG_CHECK_EVERY 256u
static unsigned g_vklog_writes;
static unsigned long long g_vklog_rotations;
/* 调用前提：持有 g_vklog_mu 且 g_vklog/g_vklog_path 已就绪。 */
static void vklog_maybe_rotate_locked(void)
{
    if (!g_vklog || !g_vklog_path[0]) return;
    struct stat st;
    if (fstat(fileno(g_vklog), &st) != 0 || (unsigned long long)st.st_size <= VKLOG_MAX_BYTES) return;
    char oldp[PATH_MAX + 8];
    snprintf(oldp, sizeof oldp, "%s.1", g_vklog_path);
    fflush(g_vklog);
    remove(oldp);                                   /* 只保留一段：丢掉上一次滚走的那段 */
    if (rename(g_vklog_path, oldp) != 0) return;    /* 滚不动 ⇒ 继续写原文件 */
    FILE *nf = fopen(g_vklog_path, "a");
    if (!nf) { rename(oldp, g_vklog_path); return; }/* 重开失败 ⇒ 恢复原名，旧的 FILE* 继续写 */
    fclose(g_vklog);
    g_vklog = nf;
    g_vklog_rotations++;
    /* 标记行直接写：走 vk_boot_logf 会在已持锁的情况下再次加锁（非递归锁）。 */
    struct timeval tv; gettimeofday(&tv, NULL);
    fprintf(g_vklog, "%ld.%06ld [log] rotated #%llu: previous %llu bytes -> %s (cap=%uMB)\n",
            (long)tv.tv_sec, (long)tv.tv_usec, g_vklog_rotations,
            (unsigned long long)st.st_size, oldp, (unsigned)(VKLOG_MAX_BYTES / (1024u * 1024u)));
    fflush(g_vklog);
}

/* 打开后（g_vklog/g_vklog_path 已就绪）立即收敛一次：旧版本不轮转留下的超大 .1 永久占盘，
 * 这里在「.1 也超上限」时删掉它（正常轮转产物不会超过上限）；当前文件超上限则立刻滚走。 */
static void vklog_trim_at_open(void)
{
    if (!g_vklog || !g_vklog_path[0]) return;
    char oldp[PATH_MAX + 8];
    snprintf(oldp, sizeof oldp, "%s.1", g_vklog_path);
    struct stat st;
    if (stat(oldp, &st) == 0 && (unsigned long long)st.st_size > VKLOG_MAX_BYTES) remove(oldp);
    pthread_mutex_lock(&g_vklog_mu);
    g_vklog_writes = 0;
    vklog_maybe_rotate_locked();
    pthread_mutex_unlock(&g_vklog_mu);
}

int vk_boot_log_open(void)
{
    if (g_vklog) return 0;
    static const char *cand[] = {
        "/data/storage/el2/base/files/vk_boot.log",
        "/data/storage/el2/base/haps/entry/files/vk_boot.log",
        "/data/storage/el2/base/cache/vk_boot.log",
        "/data/storage/el2/base/temp/vk_boot.log",
        "/data/storage/el1/base/files/vk_boot.log",
    };
    for (unsigned i = 0; i < sizeof cand / sizeof *cand; i++) {
        FILE *f = fopen(cand[i], "a");
        if (!f) continue;
        g_vklog = f;
        snprintf(g_vklog_path, sizeof g_vklog_path, "%s", cand[i]);
        vklog_trim_at_open();       /* M4n P2：旧段超限先滚走，本次 boot 从空文件写起 */
        vk_boot_logf("=== vk_boot.log opened (append) ===");
        VB_LOG("[vk-boot] file log at %{public}s", g_vklog_path);
        return 0;
    }
    VB_LOG("[vk-boot] file log open FAILED (all candidates)");
    return -1;
}

void vk_boot_logf(const char *fmt, ...)
{
    if (!g_vklog) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    va_list ap;
    pthread_mutex_lock(&g_vklog_mu);
    /* M4n P2：每 VKLOG_CHECK_EVERY 行查一次文件大小，超上限就地滚动（见 vklog_maybe_rotate_locked）。 */
    if (++g_vklog_writes >= VKLOG_CHECK_EVERY) { g_vklog_writes = 0; vklog_maybe_rotate_locked(); }
    if (!g_vklog) { pthread_mutex_unlock(&g_vklog_mu); return; }
    fprintf(g_vklog, "%ld.%06ld ", (long)tv.tv_sec, (long)tv.tv_usec);
    va_start(ap, fmt);
    vfprintf(g_vklog, fmt, ap);
    va_end(ap);
    fputc('\n', g_vklog);
    fflush(g_vklog);   // 每行刷盘：崩溃/被杀不丢
    pthread_mutex_unlock(&g_vklog_mu);
}

const char *vk_boot_log_path(void) { return g_vklog ? g_vklog_path : NULL; }

/* M4n P1 配套：退出阶段的兜底行（gfx_bridge 的 [h] submit tail）。用 trylock：别的线程正持锁写日志时
 * 直接放弃这一行，绝不把进程卡在 exit() 里。正常路径（atexit 时无人持锁）行为与 vk_boot_logf 一致。 */
int vk_boot_logf_try(const char *fmt, ...)
{
    if (!g_vklog) return -1;
    if (pthread_mutex_trylock(&g_vklog_mu) != 0) return -1;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    va_list ap;
    fprintf(g_vklog, "%ld.%06ld ", (long)tv.tv_sec, (long)tv.tv_usec);
    va_start(ap, fmt);
    vfprintf(g_vklog, fmt, ap);
    va_end(ap);
    fputc('\n', g_vklog);
    fflush(g_vklog);
    pthread_mutex_unlock(&g_vklog_mu);
    return 0;
}

/* ---------------- M3n：SwiftShader JIT 代码页目录 ----------------
 * emulator 拒绝匿名/memfd 映射取指（EACCES），文件背衬可以；Reactor 改成文件背衬后
 * 需要一个应用可写目录。这里只把目录通过 env 交给它，so 里不写死路径。
 * 目录优先取 vk_boot.log 所在目录（同一沙箱域、已证实可写），再退候选表。 */
int vk_boot_jit_env_setup(void)
{
    char dir[PATH_MAX];
    dir[0] = '\0';
    if (g_vklog_path[0]) {
        snprintf(dir, sizeof dir, "%s", g_vklog_path);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0';
    }
    static const char *cand[] = {
        "/data/storage/el2/base/files",
        "/data/storage/el2/base/haps/entry/files",
        "/data/storage/el2/base/cache",
        "/data/storage/el2/base/temp",
    };
    int wrote = 0, werrno = 0;
    if (!dir[0]) {
        for (unsigned i = 0; i < sizeof cand / sizeof *cand; i++) {
            char probe[PATH_MAX];
            snprintf(probe, sizeof probe, "%s/.jitprobe", cand[i]);
            FILE *f = fopen(probe, "a");
            if (!f) { werrno = errno; continue; }
            fclose(f);
            remove(probe);
            snprintf(dir, sizeof dir, "%s", cand[i]);
            wrote = 1;
            break;
        }
    } else {
        wrote = 1;                      /* 日志文件所在目录已证实可写 */
    }
    if (!wrote) {
        vk_boot_logf("[jit] SMO_JIT_DIR setup FAILED (no writable dir) errno=%d", werrno);
        return -1;
    }
    int rv = setenv("SMO_JIT_DIR", dir, 1);
    /* M44B（2026-11-11）：smo_jit_icache=1 ⇒ 让 SwiftShader 在「代码页发布成可执行」时显式刷 I-cache
       （M3n 为了保 RWX 而吃掉的那次 mprotect 隐式 flush，见 patches/swiftshader/ExecutableMemory.cpp
       的 M44B 段）。缺省（文件不存在/内容不是 1）⇒ 不设这个 env ⇒ 逐字节旧行为。
       注意：这一行只有**重编 SwiftShader**之后才有意义；本轮没重建 so，所以开关默认关、行为不变。 */
    { char kp[PATH_MAX]; int kfd;
      snprintf(kp, sizeof kp, "%s/smo_jit_icache", dir);
      kfd = open(kp, O_RDONLY);
      if (kfd >= 0) {
          char kb[8]; ssize_t kn = read(kfd, kb, sizeof kb - 1); close(kfd);
          if (kn > 0) { kb[kn] = 0;
              if (atoi(kb) == 1) {
                  setenv("SMO_JIT_ICACHE", "1", 1);
                  vk_boot_logf("[jit] SMO_JIT_ICACHE=1 (explicit icache flush on code publish)");
              } } } }
    /* M3p 顺手项：jit.log 原来是 SwiftShader 自己 O_CREAT(0600) 建的、属 app uid 拉不回来。
     * 这里预建同一路径并 fchmod 0666（open O_CREAT 对已存在文件不改 mode），下一轮 hdc file recv 直接可取证。
     * 不改 so：swiftshader/src/src/Reactor/ExecutableMemory.cpp:243 的 0600 也已改 0666，下次重建生效。 */
    {char jl[PATH_MAX];snprintf(jl,sizeof jl,"%s/jit.log",dir);
     int jfd=open(jl,O_WRONLY|O_CREAT|O_APPEND,0666);if(jfd>=0){fchmod(jfd,0666);close(jfd);}}
    const char *back = getenv("SMO_JIT_DIR");
    vk_boot_logf("[jit] SMO_JIT_DIR=[%s] setenv rv=%d readback=[%s]",
                 dir, rv, back ? back : "(null)");
    return rv == 0 ? 0 : -1;
}

/* ---------------- M3g：四个维度的探针计数 ----------------
 * 只写不读（读取只发生在下面的心跳线程），全部 relaxed 原子：不建立任何同步关系，
 * 也就不可能改变渲染路径的时序。volatile 单独用会丢自增，这里统一用 _Atomic。 */
_Atomic unsigned long g_beat_frames;
_Atomic unsigned long g_beat_present_try;
_Atomic unsigned long g_beat_nvn_submit;
_Atomic unsigned long g_beat_img;
_Atomic unsigned long g_beat_buf;
_Atomic unsigned long g_beat_cmd;

/* ---------------- M5b：冻结判定通道 ----------------
 * 派活单口径：屏幕 md5 不变不能当死；要看游戏侧活动（frames/nvn_submit/cmd），并用 guest 线程状态定阻塞点。
 * 这里补两条 boot 日志里原来没有的通道：
 *   ①[beat] 追加 cpu=<本进程累计 CPU 秒>/<近 2 s 占单核百分比> 与 stall=<frames 连续多少秒没动>。
 *     CPU 还在涨 = 在算（SwiftShader JIT/光栅化慢），CPU 掉到 ~0 = 全体在等；
 *   ②frames 连续 stall>=8 s 时调 runtime_guest_dump()（threads.c，走同一个文件日志）打紧凑线程表，
 *     两次表之间的 nsvc 差就是「谁还在跑」。节流 12 s、单次 boot 至多 20 份，绝不改任何线程状态。 */
#include <sys/resource.h>
__attribute__((weak)) void runtime_guest_dump(void);
__attribute__((weak)) void runtime_guest_chain_dump(unsigned h);   /* M5j: main thread guest frame chain, into vk_boot.log */
__attribute__((weak)) void runtime_m5k_dump(void);                 /* M5k: window/snapshot of main's 0x1C key object */
__attribute__((weak)) int  runtime_rss_guard(void);   /* M5c：RSS 护栏（nvn_vk.c 实现）——心跳线程独立观察者 */
__attribute__((weak)) void runtime_mem_attrib_dump(void);   /* M5s：每 30 s 一行 [mem] 自建计数器 + RSS */
__attribute__((weak)) void runtime_mem_probe10(unsigned tsec);  /* M6s：每 10 s [mem]/[grow]/[malloc]/[cpu] 归因 */
__attribute__((weak)) void runtime_core_stats_dump(unsigned tsec);   /* M6s：每 30 s 虚拟核占用画像 */
__attribute__((weak)) void runtime_sh_stats_dump(unsigned tsec);    /* M6t B：每 10 s [sh] 编译定量（nvn_vk.c） */
__attribute__((weak)) void runtime_hang_batch_dump(void);   /* M5w：在飞批次成分 + 隔离表（nvn_vk.c） */
__attribute__((weak)) void runtime_perf_dump(unsigned tsec); /* M6e：每 60 s 一行 [perf]（nvn_vk.c） */
__attribute__((weak)) void runtime_m40x_poll(unsigned tsec); /* M40X：每 60 s 一行 [m40x] cmd 分类 + 状态重复率（nvn_vk.c） */
__attribute__((weak)) void nvn_vk_m40aa_poll(unsigned tsec, unsigned long frames, double cpu); /* M40AA：每 beat 重读 smo_m40aa（分辨率除数）+ 每 60 s 一行 [m40aa] st（nvn_vk.c） */
__attribute__((weak)) void nvn_vk_m40ab_poll(unsigned tsec, unsigned long frames, double cpu); /* M40AB：每 60 s 一行 [m40ab] st + key uniq/repeat（nvn_vk.c） */
__attribute__((weak)) void nvn_vk_m40ac_poll(unsigned tsec, unsigned long frames, double cpu); /* M40AC：每 64 帧落盘 pipeline cache + 每 60 s 一行 [m40ac] st（nvn_vk.c） */
__attribute__((weak)) void nvn_vk_m40ad_poll(unsigned tsec, unsigned long frames, double cpu); /* M40AD：每 60 s 一行 [m40ad] st + REMAP/top（gfx_bridge.c，存储→id 稳定性） */
__attribute__((weak)) void nvn_vk_m40ae_poll(unsigned tsec, unsigned long frames, double cpu); /* M40AE：每 60 s 一行 [m40ae] st（gfx_bridge.c，共享图/别名 refcount） */
__attribute__((weak)) void nvn_vk_m40ag_poll(unsigned tsec, unsigned long frames); /* M40AG：拒绝建图/名额口径 */
__attribute__((weak)) void nvn_vk_m40ah_poll(unsigned tsec, unsigned long frames); /* M40AH：每 20 s 一行 [m40ah] 线程 CPU/冻结归因（gfx_bridge.c） */
__attribute__((weak)) void nvn_vk_m40ai_poll(unsigned tsec, unsigned long frames); /* M40AI：每 20 s 一行 [m40ai] 建管线耗时/串行锁状态（nvn_vk.c） */
__attribute__((weak)) void nvn_vk_m40aj_poll(unsigned tsec, unsigned long frames); /* M40AJ：每 20 s 一行 [m40aj] 冻结四分解（建图/上传/建管线/剩余）（nvn_vk.c） */
__attribute__((weak)) void nvn_vk_m40al_poll(unsigned tsec, unsigned long frames); /* M40AL：每 20 s 一行 [m40al] 几何量（draws/verts/vpd）+ 栅栏/排空等待（wait_n/wait_ms）（nvn_vk.c） */
__attribute__((weak)) void nvn_vk_m40am_poll(unsigned tsec, unsigned long frames); /* M40AM：每 20 s 一行提交频次（submit/sub_f/wait/present/acq/spf/wpf/fenc）（nvn_vk.c） */
__attribute__((weak)) void nvn_vk_m40ao_poll(unsigned tsec, unsigned long frames); /* M40AO：每 20 s 一行 GPU 工作量归属（rp/rp0/px_mp/clear/copy/bar/jit/wait_ms/draws）（nvn_vk.c） */
__attribute__((weak)) void nvn_vk_m40at_poll(unsigned tsec, unsigned long frames); /* M40AT：每 20 s 一行上屏探针（blit/blit_skip/skip_why/src/frames/hchg）（nvn_vk.c） */
__attribute__((weak)) void nvn_vk_m40au_poll(unsigned tsec, unsigned long frames); /* M40AU：每 20 s 一行 post-draw 取源探针（pick_slot/pick_mean/pick_spread/pick_age/present_slot/hchg/blit）（nvn_vk.c） */
__attribute__((weak)) void nvn_vk_m40aw_poll(unsigned tsec, unsigned long frames); /* M40AW：每 20 s 一行 submit 边界签名 + 内容门探针（n_sig/n_nonflat/best_spread/gate_ok/skip_*）（nvn_vk.c） */
__attribute__((weak)) void nvn_vk_m40ax_poll(unsigned tsec, unsigned long frames); /* M40AX：每 20 s 一行 flush+fence 之后的签名 + 内容门探针（n_sig/n_nonflat/best_spread/gate_ok/skip_*）（nvn_vk.c） */
__attribute__((weak)) void nvn_vk_m40av_poll(unsigned tsec, unsigned long frames); /* M40AV：每 20 s 一行「游戏 present 的 image」上屏源探针（cur/ok/ext/lay/pres/skip_empty/skip_why）（nvn_vk.c） */
/* M67B（2026-11-12）：上屏三真值 + 兜底链状态。
   · dm_nvn_frame_count()   = gfx_bridge.c 的 g_nvn_presents（**游戏真调 present 的次数**）
   · dm_nvn_screen_count()  = 同一个 present 里「确实有画面交给了某条显示路径」的帧数
   · nvn_vk_m67b_poll()     = smo_m67b 兜底链的逐级计数（=0 且没跑过时必须零输出）
   三者都走弱符号：缺任何一条（x86_64 壳层）时按 0 打，不改变旧行为。 */
__attribute__((weak)) uint64_t dm_nvn_frame_count(void);
__attribute__((weak)) uint64_t dm_nvn_screen_count(void);
__attribute__((weak)) void nvn_vk_m67b_poll(unsigned tsec, unsigned long frames); /* M67B：每 20 s 一行 [m67b] st（nvn_vk.c） */
/* M68B（2026-11-12）：把「present 没被叫」与「present 被我们吞了」分开的三个入口量 + 上屏泵心跳。
   · dm_nvn_present_enter() = 进 nvn_queue_present_texture 的次数（入口第一行 ++）
   · dm_nvn_present_done()  = 该入口里「画面真交给某条显示路径」的次数（≡ M67B 的 onscreen，同一计数器）
   · dm_nvn_present_proc()  = nvnGetProcAddress 里被要过 "nvnQueuePresentTexture" 的次数
   · nvn_vk_m68b_pump_tick() = smo_m67b bit4 的上屏泵起停/心跳（=0 时零输出，泵线程不创建）
   全部走弱符号：缺失时按 0 打 / 不调，不改变旧行为。 */
__attribute__((weak)) uint64_t dm_nvn_present_enter(void);
__attribute__((weak)) uint64_t dm_nvn_present_done(void);
__attribute__((weak)) uint64_t dm_nvn_present_proc(void);
__attribute__((weak)) void nvn_vk_m68b_pump_tick(unsigned tsec, unsigned long frames);
/* M104A（2026-11-13）：可见面写者台账的 30 s 汇总（= 每条通道 writes/fail/首末时间 + 最近一次
   源与目标的像素统计）。挂心跳 ⇒ 渲染线程卡死也照打；缺失时整条不调，旧链逐字不变。 */
__attribute__((weak)) void nvn_vk_m104a_poll(unsigned tsec, unsigned long frames);
__attribute__((weak)) int  runtime_tid_info(int tid, unsigned *handle, int *op, int *wk, unsigned long long *waddr,
                                            int *core, int *prio, unsigned long long *nsvc);   /* M6f: host tid -> KThread (threads.c) */
__attribute__((weak)) const char *runtime_op_name(int op);   /* threads.c: op enum -> short name */

/* ---------------- M5f: three-in-one read-only snapshot of the static loading frame ----------------
 * Measured in M5e: while the loading frame is on screen `frames` still advances (~3.4 fps), so the
 * "frames did not move for >=8 s" trigger above never fires and the whole boot produced only the four
 * thread tables of the early JIT stall (see /tmp/m5f_report.md P0.1). This adds a file-driven trigger;
 * the heartbeat thread reads it every 2 s (fopen + one integer + fclose, microseconds):
 *   smo_gdump=<n>      dump once whenever n changes (hdc writes 1, 2, 3... to hit an exact window)
 *   smo_gdump_sec=<s>  dump every s seconds when s>0 (>=2 tables per window, >=60 s apart)
 * One dump = thread table ([gst], threads.c) + IPC service call/failure ledger ([svc], ipc.c) +
 * RomFS I/O ledger ([io], romfs.c). All three are read-only: no thread state, no render/present/UV/
 * geometry/gate is touched. */
#define VB_GD_REQ_A "/data/storage/el2/base/files/smo_gdump"
#define VB_GD_REQ_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_gdump"
#define VB_GD_SEC_A "/data/storage/el2/base/files/smo_gdump_sec"
#define VB_GD_SEC_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_gdump_sec"
#define VB_MK_A "/data/storage/el2/base/files/smo_movie_kick"       /* M5h movie escape hatch (default 0 = off) */
#define VB_MK_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_movie_kick"
#define VB_FN_A "/data/storage/el2/base/files/smo_fn_adv"           /* M5o: unfenced NVNsync wait -> already-satisfied (0 = off) */
#define VB_FN_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_fn_adv"
#define VB_SW_A "/data/storage/el2/base/files/smo_sw_threads"        /* reserved: a file of its own cannot be created from adb shell (SELinux) */
#define VB_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_sw_threads"
#define VB_FILES_A "/data/storage/el2/base/files"                    /* the app's own el2 files dir (writable) */
#define VB_FILES_B "/data/app/el2/100/base/com.vibecc.smo/files"
__attribute__((weak)) void runtime_ipc_stat_dump(void);    /* ipc.c */
__attribute__((weak)) void runtime_romfs_stat_dump(void);  /* romfs.c */
__attribute__((weak)) void runtime_romfs_load_line(char *, unsigned);   /* romfs.c, M6d */
__attribute__((weak)) void runtime_load_dump(unsigned);    /* threads.c, M6d */
__attribute__((weak)) void runtime_m6q_poll(unsigned, int);/* threads.c, M6q: [st] stage table + [tk] clock */
__attribute__((weak)) void runtime_m7r_poll(unsigned);     /* threads.c, M7r: 100 s clip/main time-series (switch smo_m7r) */
__attribute__((weak)) void runtime_m7s_poll(unsigned);     /* threads.c, M7s: clip wait-key + 64 B window pin (switch smo_m7s) */
__attribute__((weak)) void runtime_m7s2_poll(unsigned);    /* threads.c, M7S2 (2026-11-08): pool-B ctor entry 0x681b30 + 0x74ca00 + pre-bl 0x74cadc (switch smo_m7s2) */
__attribute__((weak)) void runtime_m7t2_poll(unsigned);    /* threads.c, M7T2 (2026-11-08): WHOLE scene-ctor family, 9 heads 0x445640/0x447b70/0x449c50/0x44b720/0x46e7a0/0x4a1100/0x4a6b10/0x4a8820/0x4af0a0, all tail=0 4w (switch smo_m7t2) */
__attribute__((weak)) void runtime_m7t_poll(unsigned);     /* threads.c M7t + audio_renderer.c: audio-position readers + main svc8 window (switch smo_m7t) */
__attribute__((weak)) void runtime_m7u_poll(unsigned);     /* threads.c, M7u: clip-window transient guard + 0x1D signaller call-site capture (switch smo_m7u) */
__attribute__((weak)) void runtime_m7u2_poll(unsigned);    /* threads.c, M7U2 (2026-11-08): scene-dispatch hub 3 gates - A 0x7acd50 entry (tail0 4w) / B 0x7acd94 dispatch-return-addr (tail2) / C 0x7acd70 stage-name point (tail2, dumps the stage name ASCII) (switch smo_m7u2) */
__attribute__((weak)) void runtime_m7v_poll(unsigned);     /* threads.c, M7V (2026-11-08): dispatch IDENTITY - x8 0x7acd8c (tail2, [vtable]->real dispatch target ring) + Rsv siblings r1..r7 0x4a6a90/0x46e770/0x445590/0x447ae0/0x449bc0/0x44b6f0/0x4af020 (tail0 4w) (switch smo_m7v) */
__attribute__((weak)) void runtime_m7w_poll(unsigned);     /* threads.c, M7w: stuck-vs-slow verdict probes (switch smo_m7w) */
__attribute__((weak)) void runtime_m7w2_poll(unsigned);    /* threads.c, M7W2 (2026-11-08): ADAPTER recalc - gate id 94 at the generic mfp adapter main+0x7724d0 (tail2); recomputes q=[obj+8]+(tag>>1) and tgt=(tag&1)?*(*(q)+[obj+0x10]):[obj+0x10] and names the real ctor (switch smo_m7w2, name kept apart from the live smo_m7w baseline) */
__attribute__((weak)) void runtime_m7x2_poll(unsigned);    /* threads.c, M7X2 (2026-11-08): LAST HOP - G1 id 95 main+0x74c9e0 slot-3 thunk (tail2, q=[x0] -> vt=[q] -> tgt3=[[q]+0x18], all described) + G2 id 96 main+0x772500 descriptor clone (tail0 4w, src=entry x0, the 32B {+8,+0x10,+0x18,+0x20} it writes) (switch smo_m7x2, name kept apart from the 2026-11-07 smo_m7x round) */
__attribute__((weak)) void runtime_m7x_poll(unsigned);     /* threads.c, M7x: name main's two alternating wait objects (switch smo_m7x) */
__attribute__((weak)) void runtime_m7y_poll(unsigned);     /* threads.c, M7y: main's loading-screen loop + feeder + role-key release sides (switch smo_m7y) */
__attribute__((weak)) void runtime_m7z2_poll(unsigned);    /* threads.c, M7Z2 (2026-11-08): THE BRANCH AT THE SELECTION POINT - G1 id 99 main+0x6c7d30 (generic "state_time <= N" predicate, 114 callers; 0x21c = 540 frames, NOT an enum), G2 id 100 main+0x4b8e98 (skip path), G3 id 101 main+0x4b8ea4 (work path: calls scene slot+0x20 then goes to HostTypeNrvFirstSequence), G4 id 102 main+0x4b8e88 (x8 = [this+0x58] raw byte), G5 id 103 main+0x4b8e20 (HostTypeNrvLoadFirstSequence::execute entry: reads [this]/[[this]]/[this+0x58]/[this+0x20]/[this+0x60]) (switch smo_m7z2) */
__attribute__((weak)) void runtime_m7y2_poll(unsigned);    /* threads.c, M7Y2 (2026-11-08): NAME THE OTHER OBJECT - G1 id 97 main+0x44adb0 (the device's out-of-family slot-3 terminal, tail0 4w) reads q/vt/tgt3 and the CLASS NAME at [q+0xb8]; G2 id 98 main+0x44af80 (same class, slot +0x20 scene-layer setup) (switch smo_m7y2, name kept apart from the 2026-11-07 smo_m7y round) */
__attribute__((weak)) void runtime_m8b_poll(unsigned);    /* threads.c, M8B (2026-11-08): THE NERVE STATE MACHINE - G1 id 104 main+0x6c7750 NerveKeeper::setNerve (tail0 4w; x1=new nerve token, [keeper+0x10]=old token, [keeper+0x18]=old step, LR, tick, plus the nerve NAME at token+0x10; first-128 event ring + 40-slot per-token count table + 16-slot per-LR table) + G2 id 105 main+0x4b8d30 HostTypeNrvBoot::execute entry (tail0 4w; count + LR only) (switch smo_m8b) */
__attribute__((weak)) void runtime_m40w_poll(unsigned);   /* threads.c M40W (2026-11-10): read-only event-pair forensics for h=0x101f (creator/src/pair write end + its signal+reset ledgers + FIRSTSIG/NOSIG) and its 200 Hz poller h=0x1023 (switch smo_m40w) */
__attribute__((weak)) void runtime_m7a_poll(unsigned);     /* threads.c, M7a: M7z runtime adjudication probes reg/trio/null/got/kick (switch smo_m7a) */
__attribute__((weak)) void runtime_m8d_poll(unsigned);    /* threads.c, M8D (2026-11-08): THE TWO TWIN TABLE-BINDER SITES - G1 id 106 main+0x4b8cb0 (binds nerve table 0x12d0fe8 = FirstSequence family) + G2 id 107 main+0x4b7380 (binds 0x12d06d8 = TitleMenu family); hits>0 => that family's HostType instance was constructed on this machine (both have ZERO static bl/b callers, vtable only) (switch smo_m8d) */
__attribute__((weak)) void runtime_m8e2_poll(unsigned);   /* threads.c, M8E2 (2026-11-08): THE FIRST FORK (DemoOpening vs LoadStage) - G1 id 108 main+0x4b4bc0 = the word before `tbz w0,#0` at 0x4b4bc4; its predicate 0x4d2bd0 returns u8([[seq+0xb8]+0x20]+0x5f8) bit0 (1 => fall through => setNerve(0x12d0428 DemoOpening), 0 => return at 0x4b4d2c with NO DemoOpening) + G2 id 109 main+0x4b4bb8 (ret addr = fork entry 0x4b4bbc) + G3 id 110 main+0x59ac30 scene-graph create entry ('Stage' arg) + G4 id 111 main+0x59accc ret (return x0) (switch smo_m8e2) */
__attribute__((weak)) void runtime_m15a_poll(unsigned);  /* threads.c, M15A (2026-11-08): WHO PAINTED THE BLACK - gate 113 at main+0x988a20 (NVN submit entry: binds the same 64KB constant ring to stage 0/1/2/5 before every draw; x30 = caller return address). M14A static: 190 bl / 150 host functions reach it, so the x30 histogram (plus a per-draw drain queue) is the only way to collapse 150 branches to one; early/late columns split at sub>=1536 = the permanent-black segment (switch smo_m15a) */
__attribute__((weak)) void runtime_m16a_poll(unsigned);  /* threads.c, M16A (2026-11-08): THE "GORP." TRAMPOLINE - gate 114 at main+0x991a30 (FDE size 0x80). Static: 16 call sites (14 bl + 2 b) / 15 host functions all land on the first word; the 0x80 B body has ZERO conditional branches (1 bl 0x988a20 = gate 113 bind, 1 tail b 0x991ab0 = the common 2D executor), so "who should stop painting" lives in one of the 15 hosts, not here. Payload: x0/x1/x2/x3/x30 via a 30-word 7-arg stub variant; per-frame frequency + x30 histogram split at sub>=1536 (switch smo_m16a) */__attribute__((weak)) void runtime_m16a_poll(unsigned);  /* threads.c, M16A (2026-11-08): THE "GORP." TRAMPOLINE - gate 114 at main+0x991a30 (FDE size 0x80). Static: 16 call sites (14 bl + 2 b) / 15 host functions all land on the first word; the 0x80 B body has ZERO conditional branches (1 bl 0x988a20 = gate 113 bind, 1 tail b 0x991ab0 = the common 2D executor), so "who should stop painting" lives in one of the 15 hosts, not here. Payload: x0/x1/x2/x3/x30 via a 30-word 7-arg stub variant; per-frame frequency + x30 histogram split at sub>=1536 (switch smo_m16a) */
__attribute__((weak)) void runtime_m17a_poll(unsigned);  /* threads.c, M17A (2026-11-08): NAME EVERY DRAW - gate 115 at main+0x98b4b0 = the 2D quad record leaf (fn 0x98b490 +0x20). 277 static call sites / 130 host funcs reach the fn entry (0 reach +0x20); one hit per drawn textured quad, so a per-draw drain names the host function of every draw, the 12 opaque-black ones included. Payload: x0/x1/x2/x3/x30, [x2] = texture key (-1 = skip, leaves at +0x8 before the gate) (switch smo_m17a) */
__attribute__((weak)) void runtime_m18a_poll(unsigned);
__attribute__((weak)) void runtime_m19a_poll(unsigned);  /* threads.c, M19A (2026-11-08): gate 118 at main+0x916a28 (2D batch processor fn 0x915dfc +0xc2c: ldr d0,[x8,#0x700] / ldr w8,[x8,#0x708] / str q0,[x19,#0x320]) - the per-batch point where the state object's RGB fields (+0x700/+0x704/+0x708) and the three "this channel has a color" flags (+0x8b0/+0x8b1/+0x8b2) can be read on the SAME record, plus the 12-byte color buffer behind [ctx+0xb8]+0x658. Verdict (a) vs (b). Payload x0/x8(state)/x1(ctx)/x2/x3/x30; per-draw drain with safe reads in nvn_vk (switch smo_m19a) */  /* threads.c, M18A (2026-11-08): UI COLOR ARGUMENTS - gate 116 at main+0x8fea2c (the 2D color-draw helper: its caller computes x3 = [ctx+0xb8]+0x658 and the helper tail-branches via br x6, so x3 IS the 16-byte color block of that draw) + gate 117 at main+0x9106fc (the 2D panel/text renderer entry: x0 = UI layer whose packed RGBA8 colors sit at +0xf0..+0xfc and were loaded by ctor 0x90f450 from a resource block, x1 = the render context whose [x1+0xb8]+0x658 is used as the color of every quad it emits). Payload x0/x1/x2/x3/x30; per-draw drain in nvn_vk with safe reads (switch smo_m18a) */
__attribute__((weak)) void runtime_m10a_poll(unsigned);  /* threads.c, M10A (2026-11-08): FLIP THE FIRST-BOOT PREDICATE - id 108 main+0x4b4bc0 gets a 35-word stub variant (only when smo_m10a!=0 at module load) that replays the real bl 0x4d2bd0 then `orr w0,w0,mask`, i.e. ONLY w0 bit0 can change (mask=0 == identity; mode 0 installs the untouched 27-word skeleton, byte-identical to M9C) + new gate id 112 main+0x4b4bc8 (tbz fall-through first word, zero branch landings in the whole image) counts 'A branch entered' and records x0 = what the caller actually saw (switch smo_m10a) */
__attribute__((weak)) void runtime_m8c_poll(unsigned);    /* threads.c, M8C (2026-11-08): CONDITIONAL INJECTION - id 104 main+0x6c7750 gets the 30-word stub variant (w20 mov x17,x0 / w25 cbz x17,+8 / w26 mov x1,x17 / quad = observer_m8c) so the sequence HostType keeper's new nerve token can be rewritten to base+0x12d0e08 (HostTypeNrvLoadFirstSequence); smo_m8c=0 => observer returns 0 => original x1 kept (switch smo_m8c) */
__attribute__((weak)) void runtime_m7b_poll(unsigned);     /* threads.c, M7b: why was batch#3 never sent - poster LR ring + 13 site counters + fnA gate TRUE/FALSE (switch smo_m7b) */
__attribute__((weak)) void runtime_m7c_poll(unsigned);     /* threads.c, M7c: main-side svc call-site histogram + fnA 0x6156b0 entry gate + loader 0x1007 park point (switch smo_m7c) */
__attribute__((weak)) void runtime_m7e_poll(unsigned);     /* threads.c, M7e: create-request 0x746020 entry gate - creator thread/LR distribution + freeze moment (switch smo_m7e) */
__attribute__((weak)) void runtime_m7g_poll(unsigned);     /* threads.c, M7g: layer above the creator - 0x7488f0 head + entry-A/B blocks + sibling 0x7461a0 (switch smo_m7g) */
__attribute__((weak)) void runtime_m7h_poll(unsigned);     /* threads.c, M7h: batch-loop stop mode (0x4be0d0/0x4be2ec/0x4be2f4/0x4be2fc/0x4be30c/0x4be260) + 0x748e80 per-caller (switch smo_m7h) */
__attribute__((weak)) void runtime_m7n_poll(unsigned);     /* threads.c, M7N: who posted to whom - G1 factory 0x964260 + G2 PostToken 0x9646d0, role-keyed lr/obj ledgers + first-64 rows + code-window word compare (switch smo_m7n) */
__attribute__((weak)) void runtime_m7p_poll(unsigned);     /* threads.c, M7P: ONE gate - worker Start main+0x963e50 (15 ctor sites converge here): per-site LR table (site=lr-4) + per-object first-site ledger + first-64 rows + code window, same code both sides (switch smo_m7p) */
__attribute__((weak)) void runtime_m7q_poll(unsigned);     /* threads.c, M7Q: why the device skipped the 5 worker-ctor sites - WRAP main+0x964540 (16 bl callers, tail0 4w) + FO main+0x65b468 (fanout w1 entry, tail2) + FOB main+0x65b470 (bl 0x5b8710 = builds the 3 lanes, tail3), per-caller LR tables (site=lr-4) + w1 value distribution + code windows (switch smo_m7q) */
__attribute__((weak)) void runtime_ldrd_poll(unsigned);     /* threads.c, M7i: 2048-slot per-open drain + guest-frame stacks of 0x1001/0x1007/0x1025/0x1026 (switch smo_ldrd) */
__attribute__((weak)) void runtime_aud_poll(unsigned);      /* threads.c, M7i: audio thread 0x1020 ledger + park point (switch smo_aud) */
__attribute__((weak)) void runtime_frz_poll(unsigned);      /* threads.c, M7L: t~150 s freeze scene - all-thread table + main stack + lock owner (switch smo_frz) */
__attribute__((weak)) void runtime_movie_stat_dump(void);  /* movie_host.c: movie::Decoder call ledger (M5h) */
__attribute__((weak)) void runtime_io_thread_dump(unsigned);   /* threads.c, M6i: [io] thr/wait/svc */
__attribute__((weak)) void runtime_fa_dump(unsigned);          /* fa_probe.c, M6r: failure/timeout ledger */

/* ---------- M6g: the M6f SwiftShader worker-count experiment is REMOVED (rolled back) ----------
   Hypothesis H was falsified by direct measurement in M6f, so the switch has no reason to exist and
   its only process-wide side effect (a chdir around Vulkan bring-up) goes away with it. Nothing in
   this build ever writes SwiftShader.ini, chdir()s, or reads "sw=" from the dump-cadence file;
   smo_gdump_sec is again a plain integer cadence. */
static int vb_probe_int(const char *a, const char *b, int *out);

static int vb_probe_int(const char *a, const char *b, int *out)
{
    const char *p[2] = { a, b };
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(p[i], "r");
        if (!f) continue;
        int v = 0, ok = (fscanf(f, "%d", &v) == 1);
        fclose(f);
        if (ok) { *out = v; return 1; }
        return 0;
    }
    return 0;
}
/* Seed the defaults: `hdc shell` can only overwrite files that already exist (measured 2026-11-05:
   creating one inside the sandbox = Permission denied), so the app must create both knobs itself
   before hdc can change them mid-boot. Written once, only when neither path exists. */
static void vb_probe_seed(void)
{
    const char *p[2] = { VB_GD_REQ_A, VB_GD_REQ_B };
    if (vb_probe_int(VB_GD_REQ_A, VB_GD_REQ_B, &(int){0})) goto sec;
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(p[i], "w");
        if (f) { fputs("0\n", f); fclose(f); vk_boot_logf("[probe] seeded %s = 0", p[i]); break; }
    }
sec:
    if (vb_probe_int(VB_GD_SEC_A, VB_GD_SEC_B, &(int){0})) goto kick;
    for (int i = 0; i < 2; i++) {
        const char *q = i ? VB_GD_SEC_B : VB_GD_SEC_A;
        FILE *f = fopen(q, "w");
        if (f) { fputs("60\n", f); fclose(f); vk_boot_logf("[probe] seeded %s = 60", q); break; }
    }
kick:
    /* M5h: movie escape hatch (movie_host.c reads it; 0 = probe only, 1 = error event, 2 = + fail fast) */
    if (!vb_probe_int(VB_MK_A, VB_MK_B, &(int){0})) {
        const char *q[2] = { VB_MK_A, VB_MK_B };
        for (int i = 0; i < 2; i++) {
            FILE *f = fopen(q[i], "w");
            if (f) { fputs("0\n", f); fclose(f); vk_boot_logf("[probe] seeded %s = 0", q[i]); break; }
        }
    }
    /* M5o: NVNsync advance switch (gfx_bridge.c reads it; 0 = ledger only, 1 = an unfenced wait
       returns "already satisfied"). Seeded 0 so the baseline boot is the honest one; the boot
       script flips it mid-run only after the ledger names a never-fenced object. */
    if (!vb_probe_int(VB_FN_A, VB_FN_B, &(int){0})) {
        const char *q[2] = { VB_FN_A, VB_FN_B };
        for (int i = 0; i < 2; i++) {
            FILE *f = fopen(q[i], "w");
            if (f) { fputs("0\n", f); fclose(f); vk_boot_logf("[probe] seeded %s = 0", q[i]); break; }
        }
    }
}
static double vb_now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9; }
static double vb_cpu_s(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru)) return -1.0;
    return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6
         + (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
}
/* 常驻集（KB）。M5b b1 的进程是在冻结期把 2.6 GB 涨到 14 GB 被 LowMemoryKill 杀掉的——
   冻结期内存曲线必须和 CPU 一起看，否则「在算」和「漏内存」分不开。读失败返回 -1。 */
static long vb_rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long size = 0, rss = 0;
    if (fscanf(f, "%ld %ld", &size, &rss) != 2) { fclose(f); return -1; }
    fclose(f);
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    return rss * (pg / 1024);
}

/* ---------------- M5w P0：[hang] 判定探针 ----------------
 * 任务口径：帧不再前进时，必须用**数字**判「真在算」还是「丢 fence」：
 *   · SwiftShader 线程 CPU 持续 ~100% ⇒ 真在算（像素/编译在跑，只是慢）
 *   · 全线程 idle 且我们自己的栅栏计数不动 ⇒ 丢 fence
 * 做法：frames 连续 >=VB_HANG_STALL_S 秒不动时，每 5 s 采一次 /proc/self/task/<tid>/stat 的
 * utime+stime（每线程 CPU 增量）+ comm（线程名），打一行 [hang]。首次采样只建基线，
 * 之后每个线程的 CPU% = Δtick/hz/Δwall。**只读**：不碰任何线程、不改任何状态。
 * 第二行（在飞批次是谁/隔离表）由 nvn_vk.c 的 runtime_hang_batch_dump 打，弱符号缺失就跳过。
 * 注：SwiftShader 没有可读的 GPU 队列深度，故本行不报 gpu_queue，改为报 submit 增量与在飞批次。 */
#define VB_HANG_STALL_S 20.0
#define VB_HANG_PERIOD_S 5.0
#define VB_HANG_MAXT 512
static int vb_task_cpu(int tid, unsigned long long *u, unsigned long long *s, char *comm, size_t cap)
{
    char p[64], b[512];
    snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    char *op = strchr(b, '(');
    char *lp = strrchr(b, ')');          /* comm 里可能有空格/括号：取第一个 '(' 和最后一个 ')' */
    if (!op || !lp || lp <= op) return 0;
    size_t cl = (size_t)(lp - op - 1); if (cl >= cap) cl = cap - 1;
    memcpy(comm, op + 1, cl); comm[cl] = 0;
    char *q = lp + 1;                    /* 之后是 state(3)... ：token 12 = utime(14)，token 13 = stime(15) */
    int idx = 3, got = 0; unsigned long long uu = 0, ss = 0;
    while (*q) {
        while (*q == ' ') q++;
        if (!*q) break;
        if (idx == 14) uu = strtoull(q, NULL, 10);
        if (idx == 15) { ss = strtoull(q, NULL, 10); got = 1; break; }
        while (*q && *q != ' ') q++;
        idx++;
    }
    if (!got) return 0;
    *u = uu; *s = ss;
    return 1;
}
static void vb_hang_probe(double now, double stall, unsigned long tsec)
{
    static double next, prev_now;
    static struct { int tid; unsigned long long u, s; char comm[16]; } pv[VB_HANG_MAXT];
    static int pvn;
    if (stall < VB_HANG_STALL_S) { next = 0; prev_now = 0; pvn = 0; return; }   /* 帧动了：基线作废 */
    if (next > 0 && now < next) return;
    next = now + VB_HANG_PERIOD_S;
    double dt = (prev_now > 0) ? (now - prev_now) : 0;
    prev_now = now;
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
    int nts = 0, busy = 0, have_prev = (pvn > 0) && (dt > 0.2);
    double maxcpu = -1.0, sumcpu = 0.0;
    struct { double cpu; char comm[16]; } top[3] = { { -1.0, "" }, { -1.0, "" }, { -1.0, "" } };
    int nt_new = 0;
    static struct { int tid; unsigned long long u, s; char comm[16]; } nw[VB_HANG_MAXT];
    DIR *d = opendir("/proc/self/task");
    if (!d) { vk_boot_logf("[hang] t=%lus stall=%.0fs probe: /proc/self/task unreadable", tsec, stall); return; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        int tid = atoi(de->d_name);
        unsigned long long u = 0, s = 0; char comm[16] = "";
        if (!vb_task_cpu(tid, &u, &s, comm, sizeof comm)) continue;
        nts++;
        if (nt_new < VB_HANG_MAXT) {
            nw[nt_new].tid = tid; nw[nt_new].u = u; nw[nt_new].s = s;
            snprintf(nw[nt_new].comm, sizeof nw[nt_new].comm, "%s", comm);
            nt_new++;
        }
        if (!have_prev) continue;
        for (int i = 0; i < pvn; i++) if (pv[i].tid == tid) {
            double cpu = 100.0 * (double)((u - pv[i].u) + (s - pv[i].s)) / (double)hz / dt;
            if (cpu < 0) cpu = 0;
            sumcpu += cpu;
            if (cpu > maxcpu) maxcpu = cpu;
            if (cpu >= 50.0) busy++;
            for (int k = 0; k < 3; k++) if (cpu > top[k].cpu) {
                for (int j = 2; j > k; j--) top[j] = top[j - 1];
                top[k].cpu = cpu; snprintf(top[k].comm, sizeof top[k].comm, "%s", comm);
                break;
            }
            break;
        }
    }
    closedir(d);
    if (nt_new > 0) { memcpy(pv, nw, sizeof nw[0] * (size_t)nt_new); pvn = nt_new; }
    unsigned long long sub = atomic_load_explicit(&g_beat_nvn_submit, memory_order_relaxed);
    static unsigned long long sub0; static double sub_t0;
    if (sub_t0 <= 0) { sub_t0 = now; sub0 = sub; }
    double rate = (now > sub_t0) ? (double)(sub - sub0) / (now - sub_t0) : 0.0;
    vk_boot_logf("[hang] t=%lus stall=%.0fs threads=%d busy=%d maxcpu=%.0f%% total=%.0f%% submit=%llu rate=%.2f/s tops=%s:%.0f%%,%s:%.0f%%,%s:%.0f%%",
                 tsec, stall, nts, busy, have_prev ? maxcpu : -1.0, have_prev ? sumcpu : -1.0, sub, rate,
                 top[0].comm[0] ? top[0].comm : "-", top[0].cpu,
                 top[1].comm[0] ? top[1].comm : "-", top[1].cpu,
                 top[2].comm[0] ? top[2].comm : "-", top[2].cpu);
    if (runtime_hang_batch_dump) runtime_hang_batch_dump();
}

/* M6d: per-thread CPU attribution over the window between two dumps. Same /proc/self/task reader
 * as vb_hang_probe, own baseline: that one only accumulates while `frames` has been frozen for
 * >=20 s, which never happens here (the loop keeps submitting at ~1-2 fps). Read-only. */
static void vb_load_cpu(unsigned tsec)
{
    static struct { int tid; unsigned long long u, s; } pv[VB_HANG_MAXT];
    static int pvn; static double pnow;
    struct { int tid; double cpu; char comm[16]; } cur[VB_HANG_MAXT];
    int ncur = 0, busy = 0, nold = 0; double total = 0.0;
    double now = vb_now_s(), dt = (pnow > 0) ? now - pnow : 0.0;
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
    DIR *d = opendir("/proc/self/task");
    if (!d) { vk_boot_logf("[load] cpu t=%us probe: /proc/self/task unreadable", tsec); return; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        int tid = atoi(de->d_name);
        unsigned long long u = 0, s = 0; char comm[16] = "";
        if (!vb_task_cpu(tid, &u, &s, comm, sizeof comm)) continue;
        if (ncur >= VB_HANG_MAXT) continue;
        cur[ncur].tid = tid; cur[ncur].cpu = -1.0;
        snprintf(cur[ncur].comm, sizeof cur[ncur].comm, "%s", comm);
        if (dt > 0.2) {
            for (int i = 0; i < pvn; i++) if (pv[i].tid == tid) {
                double c = 100.0 * (double)((u - pv[i].u) + (s - pv[i].s)) / (double)hz / dt;
                if (c < 0) c = 0; nold++;
                cur[ncur].cpu = c; total += c; if (c >= 50.0) busy++;
                break;
            }
        }
        pv[ncur].tid = tid; pv[ncur].u = u; pv[ncur].s = s;
        ncur++;
    }
    closedir(d);
    pvn = ncur; pnow = now;
    { double t0 = -1, t1 = -1, t2 = -1; int i0 = -1, i1 = -1, i2 = -1;
      for (int i = 0; i < ncur; i++) { double c = cur[i].cpu; if (c < 0) continue;
          if (c > t0) { t2 = t1; i2 = i1; t1 = t0; i1 = i0; t0 = c; i0 = i; }
          else if (c > t1) { t2 = t1; i2 = i1; t1 = c; i1 = i; }
          else if (c > t2) { t2 = c; i2 = i; } }
      vk_boot_logf("[load] cpu t=%us threads=%d matched=%d busy=%d total=%.0f%% tops=%s:%.0f%%,%s:%.0f%%,%s:%.0f%%",
                   tsec, ncur, nold, busy, total,
                   i0 >= 0 ? cur[i0].comm : "-", t0 > 0 ? t0 : 0.0,
                   i1 >= 0 ? cur[i1].comm : "-", t1 > 0 ? t1 : 0.0,
                   i2 >= 0 ? cur[i2].comm : "-", t2 > 0 ? t2 : 0.0); }
}

/* M6f: state + wchan for one host tid. Both files are tiny; read failures just leave the fields
   empty (the row still carries utime/stime, which is the primary evidence). */
static int vb_task_state(int tid, char *st, char *wch, size_t cap)
{
    char p[64], b[256];
    wch[0] = 0; *st = '?';
    snprintf(p, sizeof p, "/proc/self/task/%d/wchan", tid);
    FILE *f = fopen(p, "r");
    if (f) {
        size_t n = fread(b, 1, sizeof b - 1, f);
        fclose(f); b[n] = 0;
        char *nl = strchr(b, '\n'); if (nl) *nl = 0;
        snprintf(wch, cap, "%s", b);
    }
    snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid);
    f = fopen(p, "r");
    if (f) {
        size_t n = fread(b, 1, sizeof b - 1, f);
        fclose(f); b[n] = 0;
        char *lp = strrchr(b, ')');
        if (lp && lp[1] == ' ' && lp[2]) *st = lp[2];
    }
    return 1;
}

/* M6f: the per-thread CPU table the task asks for (one [cpu] row per thread, every dump).
   Same /proc/self/task scan as vb_load_cpu (own baseline: the window since the previous dump), plus
   (a) process state (R/S/D) and wchan so "not running" can be told from "running but no progress",
   (b) the KThread handle/op/wait for guest threads (weak runtime_tid_info, threads.c), and
   (c) the SVC counter delta, so a thread that burns CPU but makes no SVC progress is visible.
   Read-only: two extra small files per thread per 30 s, no thread state touched. */
struct vb_ct { int tid; double cpu; char comm[16]; unsigned long long u, s; long long du, ds;
               char st; char wch[32]; unsigned gh; int gop, gwk, gcore, gprio; unsigned long long gnsvc, gdnsvc; int isg; };
static int vb_ct_cmp(const void *a, const void *b)
{
    double x = ((const struct vb_ct *)a)->cpu, y = ((const struct vb_ct *)b)->cpu;
    return (x < y) - (x > y);
}
static void vb_cpu_table(unsigned tsec)
{
    static struct { int tid; unsigned long long u, s; } pv[256];
    static struct { unsigned h; unsigned long long nsvc; } ph[64];
    static int pvn, phn; static double pnow;
    struct vb_ct cur[96];
    int ncur = 0, nshow = 0;
    double now = vb_now_s(), dt = (pnow > 0) ? now - pnow : 0.0;
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
    DIR *d = opendir("/proc/self/task");
    if (!d) { vk_boot_logf("[cpu] t=%us probe: /proc/self/task unreadable", tsec); return; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        int tid = atoi(de->d_name);
        unsigned long long u = 0, s = 0; char comm[16] = "";
        if (!vb_task_cpu(tid, &u, &s, comm, sizeof comm)) continue;
        if (ncur >= 96) continue;
        struct vb_ct *c = &cur[ncur];
        c->tid = tid; c->u = u; c->s = s; c->cpu = -1.0; c->du = -1; c->ds = -1;
        snprintf(c->comm, sizeof c->comm, "%s", comm);
        vb_task_state(tid, &c->st, c->wch, sizeof c->wch);
        c->isg = 0; c->gh = 0; c->gop = 0; c->gwk = 0; c->gcore = -99; c->gprio = 0;
        c->gnsvc = 0; c->gdnsvc = -1;
        if (runtime_tid_info) {
            unsigned h = 0; unsigned long long nsvc = 0;
            if (runtime_tid_info(tid, &h, &c->gop, &c->gwk, NULL, &c->gcore, &c->gprio, &nsvc)) {
                c->isg = 1; c->gh = h; c->gnsvc = nsvc;
                for (int i = 0; i < phn; i++) if (ph[i].h == h) { c->gdnsvc = (long long)(nsvc - ph[i].nsvc); break; }
            }
        }
        if (dt > 0.2) {
            for (int i = 0; i < pvn; i++) if (pv[i].tid == tid) {
                double cc = 100.0 * (double)((u - pv[i].u) + (s - pv[i].s)) / (double)hz / dt;
                if (cc < 0) cc = 0;
                c->cpu = cc; c->du = (long long)((u - pv[i].u) * 1000ull / (unsigned long long)hz);
                c->ds = (long long)((s - pv[i].s) * 1000ull / (unsigned long long)hz);
                break;
            }
        }
        ncur++;
    }
    closedir(d);
    /* refresh both baselines for the next window (positional, matched by tid on read) */
    for (int i = 0; i < ncur && i < 256; i++) { pv[i].tid = cur[i].tid; pv[i].u = cur[i].u; pv[i].s = cur[i].s; }
    pvn = ncur < 256 ? ncur : 256;
    phn = 0;
    for (int i = 0; i < ncur; i++) if (cur[i].isg && phn < 64) { ph[phn].h = cur[i].gh; ph[phn].nsvc = cur[i].gnsvc; phn++; }
    pnow = now;
    qsort(cur, (size_t)ncur, sizeof cur[0], vb_ct_cmp);
    for (int i = 0; i < ncur && nshow < 64; i++) {
        if (!cur[i].isg && strncmp(cur[i].comm, "Thread<", 7) != 0 && cur[i].cpu < 1.0) continue;
        nshow++;
        if (cur[i].isg)
            vk_boot_logf("[cpu] t=%us tid=%d comm=%s st=%c wch=%s u=%llums du=%lldms s=%llums ds=%lldms cpu=%.0f%% g=%#x op=%d(%s) wk=%d core=%d pr=%d nsvc=%llu dnsvc=%lld",
                         tsec, cur[i].tid, cur[i].comm, cur[i].st, cur[i].wch[0] ? cur[i].wch : "-",
                         cur[i].u, cur[i].du, cur[i].s, cur[i].ds, cur[i].cpu < 0 ? 0.0 : cur[i].cpu,
                         cur[i].gh, cur[i].gop, runtime_op_name ? runtime_op_name(cur[i].gop) : "?",
                         cur[i].gwk, cur[i].gcore, cur[i].gprio, cur[i].gnsvc, cur[i].gdnsvc);
        else
            vk_boot_logf("[cpu] t=%us tid=%d comm=%s st=%c wch=%s u=%llums du=%lldms s=%llums ds=%lldms cpu=%.0f%% g=- op=- wk=-",
                         tsec, cur[i].tid, cur[i].comm, cur[i].st, cur[i].wch[0] ? cur[i].wch : "-",
                         cur[i].u, cur[i].du, cur[i].s, cur[i].ds, cur[i].cpu < 0 ? 0.0 : cur[i].cpu);
    }
    vk_boot_logf("[cpu] t=%us threads=%d shown=%d window=%.1fs", tsec, ncur, nshow, dt);
}

/* 2 秒一行 [beat]：t 为本线程启动后的秒数（文件行首另有 gettimeofday 绝对时间）。
 * 独立线程的意义：主循环真卡死时这一行照常刷新，而计数定格 —— 「进程活着但循环停了」
 * 与「日志通道死了」由此可以分开判读。 */
static void *beat_thread(void *arg)
{
    (void)arg;
    unsigned long last_frames = atomic_load_explicit(&g_beat_frames, memory_order_relaxed);
    double moved = vb_now_s(), cpu_prev = vb_cpu_s(), last_dump = 0;
    int dumps = 0;
    int gd_req_last = -1; double gd_sec_next = 0; int gd_seeded = 0;
    for (unsigned long n = 1;; n++) {
        sleep(2);
        unsigned long frames = atomic_load_explicit(&g_beat_frames, memory_order_relaxed);
        double now = vb_now_s(), cpu = vb_cpu_s();
        if (frames != last_frames) { last_frames = frames; moved = now; }
        /* M67B：present 的两个真值直接进 [beat]（M63A 的建议）。presents=「游戏真调 present 几次」，
           onscreen=「其中有几次确实把画面交给了显示路径」。二者都为 0 ⇒ 全黑不在上屏实现里，
           得往 submit→present 之间找（见 logs/m67b/summary.md）。弱符号缺失时打 0。 */
        unsigned long long presents = dm_nvn_frame_count ? (unsigned long long)dm_nvn_frame_count() : 0ull;
        unsigned long long onscreen = dm_nvn_screen_count ? (unsigned long long)dm_nvn_screen_count() : 0ull;
        /* M68B：入口三个量并列进 [beat]（判别表见 logs/m68b/summary.md）。
           enter = 客侧真调进 present 入口的次数；done ≡ onscreen（同一计数器，只为让判别表三项一眼可读）；
           proc = 客侧有没有来 nvnGetProcAddress 取过 present 这枚地址。 */
        unsigned long long enter = dm_nvn_present_enter ? (unsigned long long)dm_nvn_present_enter() : 0ull;
        unsigned long long done  = dm_nvn_present_done ? (unsigned long long)dm_nvn_present_done() : 0ull;
        unsigned long long proc  = dm_nvn_present_proc ? (unsigned long long)dm_nvn_present_proc() : 0ull;
        vk_boot_logf("[beat] t=%lus n=%lu frames=%lu presents=%llu onscreen=%llu enter=%llu done=%llu proc=%llu present_try=%lu nvn_submit=%lu img=%lu buf=%lu cmd=%lu cpu=%.1f pct=%.0f stall=%.0f rss=%ld",
                     n * 2UL, n, frames, presents, onscreen, enter, done, proc,
                     atomic_load_explicit(&g_beat_present_try, memory_order_relaxed),
                     atomic_load_explicit(&g_beat_nvn_submit, memory_order_relaxed),
                     atomic_load_explicit(&g_beat_img, memory_order_relaxed),
                     atomic_load_explicit(&g_beat_buf, memory_order_relaxed),
                     atomic_load_explicit(&g_beat_cmd, memory_order_relaxed),
                     cpu, (cpu >= 0 && cpu_prev >= 0) ? (cpu - cpu_prev) * 50.0 : -1.0, now - moved,
                     vb_rss_kb());
        /* M40AA：每 beat(2 s) 重读 smo_m40aa（值=世界 RT 除数，缺省 2）+ 每 60 s 一行 [m40aa] st。只做文件读与一次原子写。 */
        if (nvn_vk_m40aa_poll) nvn_vk_m40aa_poll((unsigned)(n * 2UL), frames, cpu);
        /* M40AB：每 60 s 一行 [m40ab] st（new/reuse/draws/tbl/full/full_new/fps60）+ key uniq/repeat。只读原子量。 */
        if (nvn_vk_m40ab_poll) nvn_vk_m40ab_poll((unsigned)(n * 2UL), frames, cpu);
        /* M40AC：每 64 帧 vkGetPipelineCacheData 落盘 + 每 60 s 一行 [m40ac] st（smo_m40ac 关态空转）。 */
        if (nvn_vk_m40ac_poll) nvn_vk_m40ac_poll((unsigned)(n * 2UL), frames, cpu);
        /* M40AD：每 60 s 一行 [m40ad] st（imgs/keys/id_uniq/remap/bound，存储→id 稳定性）。挂心跳 ⇒ 渲染线程卡死照打。 */
        if (nvn_vk_m40ad_poll) nvn_vk_m40ad_poll((unsigned)(n * 2UL), frames, cpu);
        /* M40AE：每 60 s 一行 [m40ae] st（imgs/imgpf/uniq/ids/keys/remap/hit/fps60，共享图 A/B 读数）。挂心跳 ⇒ 渲染卡死照打。 */
        if (nvn_vk_m40ae_poll) nvn_vk_m40ae_poll((unsigned)(n * 2UL), frames, cpu);
        /* M40AG：每 60 s 一行 [m40ag] st（dcap/idimg/cap/ev/deny/hit，拒绝建图 A/B 口径）。挂心跳 ⇒ 渲染卡死照打。 */
        if (nvn_vk_m40ag_poll) nvn_vk_m40ag_poll((unsigned)(n * 2UL), frames);
        /* M40AH：每 20 s 一行 [m40ah] st（dtw/frames/dimg + 进程 CPU% + 线程 top3 tid/comm/state/cpu%）。
           挂心跳 ⇒ 渲染线程卡死照打；smo_m40ah 关态空转（只读开关文件，不扫 /proc）。 */
        if (nvn_vk_m40ah_poll) nvn_vk_m40ah_poll((unsigned)(n * 2UL), frames);
        /* M40AI：每 20 s 一行 [m40ai] st（dcreate/dms/avg_ms/max_ms/dsh_new/dsh_hit/dsh_dup/dsh_ms + lock）。
           挂心跳 ⇒ 渲染线程卡死照打；smo_m40ai 关态空转（只读开关文件，零输出）。
           对照读法：lock=1 时若 [m40ah] 的 top3 CPU 仍 ~80% 但 dcreate 骤降 ⇒ 并发 JIT 竞态成立。 */
        if (nvn_vk_m40ai_poll) nvn_vk_m40ai_poll((unsigned)(n * 2UL), frames);
        /* M40AJ：每 20 s 一行 [m40aj] st（dimg/img_ms/dup/up_ms/up_MB/dcreate/cp_ms/rest_ms）。
           挂心跳 ⇒ 渲染线程卡死照打；smo_m40aj 关态空转（只读开关文件，零输出）。
           对照读法：rest_ms 占绝对多数 ⇒ 冻结在引擎线程（JIT 主循环/命令重放），不在建图/上传/建管线。 */
        if (nvn_vk_m40aj_poll) nvn_vk_m40aj_poll((unsigned)(n * 2UL), frames);
        /* M40AL：每 20 s 一行 [m40al] st（draws/verts/vpd/wait_n/wait_ms/frames）。
           挂心跳 ⇒ 渲染线程卡死照打；smo_m40al 关态空转（只读开关文件，零输出）。
           对照读法：wait_ms ≈ 同窗 [m40aj] rest_ms ⇒ 冻结就是栅栏/排空等待；wait_ms≈0 而 rest_ms≈dtw ⇒ 冻结在引擎线程。 */
        if (nvn_vk_m40al_poll) nvn_vk_m40al_poll((unsigned)(n * 2UL), frames);
        if (nvn_vk_m40am_poll) nvn_vk_m40am_poll((unsigned)(n * 2UL), frames);
        /* M40AO：每 20 s 一行 [m40ao] st（rp/rp0/px_mp/clear/copy/bar/jit/wait_ms/draws）。
           挂心跳 ⇒ 渲染线程卡死照打；smo_m40ao 关态空转（只读开关文件，零输出）。
           对照读法：rp0/rp ≥50% 或 px_mp/帧 ≫ 0.36 MPx ⇒ 空转附件；copy 每帧数十 ⇒ 回读；
           bar 每帧 ≥100 ⇒ 屏障；jit 页数在冻结窗陡增 ⇒ SwiftShader JIT。 */
        if (nvn_vk_m40ao_poll) nvn_vk_m40ao_poll((unsigned)(n * 2UL), frames);
        /* M40AT：每 20 s 一行上屏探针（blit/blit_skip/skip_why/src/hchg）。关态空转零输出。 */
        if (nvn_vk_m40at_poll) nvn_vk_m40at_poll((unsigned)(n * 2UL), frames);
        /* M40AU：每 20 s 一行 [m40au] st（post-draw 记账的槽 + 便宜签名 + 上屏实际槽 + hchg/blit）。
           挂心跳 ⇒ 渲染线程卡死照打；只读，不改选源（行为由 smo_m40au 在 nvn_vk 内部控）。 */
        if (nvn_vk_m40au_poll) nvn_vk_m40au_poll((unsigned)(n * 2UL), frames);
        if (nvn_vk_m40av_poll) nvn_vk_m40av_poll((unsigned)(n * 2UL), frames);   /* M40AV：上屏源=游戏 present 的 image（关态空转零输出） */
        if (nvn_vk_m40aw_poll) nvn_vk_m40aw_poll((unsigned)(n * 2UL), frames);   /* M40AW：submit 边界签名 + 内容门（关态空转零输出） */
        if (nvn_vk_m40ax_poll) nvn_vk_m40ax_poll((unsigned)(n * 2UL), frames);   /* M40AX：flush+fence 之后的签名 + 内容门（关态空转零输出） */
        /* M67B：兜底上屏链的逐级计数。/ 挂心跳 ⇒ present 根本没被调用时也能读出「兜底一次都没进」
           ⇒ 直接区分「present 没被调」（n=0）与「present 调了但上屏失败」（n>0 且 blit_ok/fill_ok=0）。 */
        if (nvn_vk_m67b_poll) nvn_vk_m67b_poll((unsigned)(n * 2UL), frames);
        /* M68B：上屏泵起停（bit4=16）+ 泵心跳（bit5=32）。挂心跳 = present 从不被调也能起停，
           2 s 内响应 hdc 改位；bit4=0 时该函数只做一次 smo_m67b 文件读 + 两个比较，零输出、不建线程。 */
        if (nvn_vk_m68b_pump_tick) nvn_vk_m68b_pump_tick((unsigned)(n * 2UL), frames);
        /* M104A：每 30 s 一行「谁在写可见面」的汇总（函数自带 30 s 门；关态也照打，因为它是账本不是实验）。 */
        if (nvn_vk_m104a_poll) nvn_vk_m104a_poll((unsigned)(n * 2UL), frames);
        cpu_prev = cpu;
        if ((n % 30u) == 0u) nvn_vk_m40p_stat();   /* M40P：每 60 s 一行建图分桶（smo_m40p 关态空转） */
        if ((n % 30u) == 0u && runtime_m40x_poll) runtime_m40x_poll((unsigned)(n * 2UL));   /* M40X：每 60 s 一行命令分类（挂心跳，smo_m40x 关态空转） */
        if ((n % 30u) == 0u) nvn_vk_m40z_poll((unsigned)(n * 2UL));   /* M40Z：每 60 s 一行深度改道状态（挂心跳，smo_m40z 关态空转） */
        vb_hang_probe(now, now - moved, n * 2UL);   /* M5w P0：帧冻结 >=20 s 时每 5 s 一行 [hang]（只读） */
        /* M5c：内存护栏的**独立观察者**。渲染线程若卡在驱动内部（SwiftShader 里），它自己不会回到任何
           安全点 ⇒ 只有心跳线程能把 rss + 阶段环落盘。这里只调用 runtime_rss_guard()（nvn_vk.c 实现，
           超阈值时置位并打 [j] RSS GUARD 行），然后照常打线程表——不改任何线程状态、不 abort。 */
        if (runtime_rss_guard) runtime_rss_guard();
        /* M5s：每 30 s 一行归因（自建计数器 + 内核 RSS/VmSize/VmData）。只读，不改任何状态。
           M6s：同一条路上加 10 s 的 [mem]/[grow]/[malloc]/[cpu]（映射增量点名）与 30 s 的 [core]
           虚拟核占用画像；两者都只读，各自独立节拍，互不影响。 */
        {
            static double mem_next;
            if (mem_next <= 0) mem_next = now + 10.0;
            else if (now >= mem_next) {
                mem_next = now + 10.0;
                if (runtime_mem_probe10) runtime_mem_probe10((unsigned)(n * 2UL));
            }
        }
        {
            static double ma_next;
            if (ma_next <= 0) ma_next = now + 30.0;
            else if (now >= ma_next) { ma_next = now + 30.0; if (runtime_mem_attrib_dump) runtime_mem_attrib_dump(); }
        }
        {
            static double sh_next;
            if (sh_next <= 0) sh_next = now + 10.0;
            else if (now >= sh_next) { sh_next = now + 10.0; if (runtime_sh_stats_dump) runtime_sh_stats_dump((unsigned)(n * 2UL)); }
        }
        {
            static double co_next;
            if (co_next <= 0) co_next = now + 30.0;
            else if (now >= co_next) { co_next = now + 30.0; if (runtime_core_stats_dump) runtime_core_stats_dump((unsigned)(n * 2UL)); }
        }
        /* M6q: the stage table + the game's own clock. Two cadences in one probe: the 30 s table
           (full=1, the [st]/[tk] lines) and a 2 s change check (full=0) that prints an immediate
           [st] CHANGE line when opens/reads or the post-0x1C/0x1D counters move, so a burst inside
           a 30 s window cannot be missed. Read-only; own cadence, not the file-driven probe. */
        {
            static double tk_next;
            if (tk_next <= 0) tk_next = now + 30.0;
            else if (now >= tk_next) { tk_next = now + 30.0; if (runtime_m6q_poll) runtime_m6q_poll((unsigned)(n * 2UL), 1); }
            else if (runtime_m6q_poll) runtime_m6q_poll((unsigned)(n * 2UL), 0);
        }
        /* M7r: every beat, own 100 s cadence inside (switch smo_m7r, hard cap 24 data lines). */
        if (runtime_m7r_poll) runtime_m7r_poll((unsigned)(n * 2UL));
        /* M7s: every beat, own 100 s cadence inside (switch smo_m7s, hard cap 20 data lines). */
        if (runtime_m7s_poll) runtime_m7s_poll((unsigned)(n * 2UL));
        /* M7t: every beat, own 100 s cadence inside (switch smo_m7t, hard cap 12 lines). */
        if (runtime_m7t_poll) runtime_m7t_poll((unsigned)(n * 2UL));
        /* M7u: every beat, switch refresh + occupancy sample (2 s) + ring drains + 100 s series inside (switch smo_m7u). */
        if (runtime_m7u_poll) runtime_m7u_poll((unsigned)(n * 2UL));
        /* M7w: every beat, switch refresh + brc/pres drains + 100 s series inside (switch smo_m7w). NOTE: the mwk window sleeps 2 s inside a 100 s poll -- heartbeat-only, guest threads untouched. */
        if (runtime_m7w_poll) runtime_m7w_poll((unsigned)(n * 2UL));
        /* M7W2: every beat, switch refresh + adapter gate code/ring/sum inside (switch smo_m7w2). */
        if (runtime_m7w2_poll) runtime_m7w2_poll((unsigned)(n * 2UL));
        /* M7X2 (2026-11-08): THE LAST HOP. G1 id 95 at main+0x74c9e0 - the slot-3 forwarder thunk
           M7W2 named (both platforms, byte-identical): x0=q in, so [q] / [[q]+0x18] name the function
           that actually does the work. G2 id 96 at main+0x772500 - the descriptor clone: src = entry x0,
           and the 32B {+8,+0x10,+0x18,+0x20} it writes is exactly what binds +8. (switch smo_m7x2) */
        if (runtime_m7x2_poll) runtime_m7x2_poll((unsigned)(n * 2UL));
        /* M7x: every beat, switch refresh + wobj row drain + 60 s ldr + 100 s wsrc/sem/wown inside (switch smo_m7x). */
        if (runtime_m7x_poll) runtime_m7x_poll((unsigned)(n * 2UL));
        /* M7y: every beat, switch refresh + 60 s st + 100 s pc/feeder/rel/ms inside (switch smo_m7y). */
        if (runtime_m7y_poll) runtime_m7y_poll((unsigned)(n * 2UL));
        /* M7Y2: every beat, switch refresh + code1/code2 (10 s) + g1/g2 rings + sum1/sum2/per-LR (10 s).
           G1 id 97 main+0x44adb0 = the device's out-of-family slot-3 terminal; its [q+0xb8] cell holds the
           class-name pointer (three-family-identical layout from 161,696 loader reloc rows) so every hit
           self-reports the class. G2 id 98 main+0x44af80 = same class, slot +0x20. (switch smo_m7y2) */
        if (runtime_m7z2_poll) runtime_m7z2_poll((unsigned)(n * 2UL));
        /* M8B (2026-11-08): THE NERVE STATE MACHINE. G1 id 104 at main+0x6c7750 - the ONE write point of
           [keeper+0x10] (setNerve); every event logs the new token, the old token/step, LR and the nerve NAME
           read from token+0x10, so the simulator's whole nerve timeline is named. G2 id 105 at main+0x4b8d30
           (HostTypeNrvBoot::execute entry) counts how much the host state machine runs. (switch smo_m8b) */
        if (runtime_m8b_poll) runtime_m8b_poll((unsigned)(n * 2UL));
        /* M8C (2026-11-08): CONDITIONAL INJECTION at the same id-104 point (main+0x6c7750). The 30-word
           stub variant can rewrite x1 (= the new nerve token) to base+0x12d0e08 (HostTypeNrvLoadFirstSequence)
           but ONLY for the latched sequence HostType keeper and only when smo_m8c != 0; with smo_m8c=0 the
           observer returns 0 and the stub keeps the original x1 (byte-identical behaviour to M8B). (switch smo_m8c) */
        if (runtime_m8c_poll) runtime_m8c_poll((unsigned)(n * 2UL));
        /* M8D (2026-11-08): THE TWO TWIN TABLE-BINDER SITES. G1 id 106 at main+0x4b8cb0 binds the nerve
           table 0x12d0fe8 (FirstSequence family); G2 id 107 at main+0x4b7380 binds 0x12d06d8 (TitleMenu
           family). Both sites have ZERO static bl/b callers (reached through a vtable slot only), so a
           single hit proves that family's HostType instance was actually constructed on this machine:
           this is the DEVICE-side confirmation/denial of M8B's static inference. (switch smo_m8d) */
        if (runtime_m8d_poll) runtime_m8d_poll((unsigned)(n * 2UL));
        /* M8E2 (2026-11-08): THE FIRST FORK - who decides DemoOpening vs LoadStage. G1 id 108 at
           main+0x4b4bc0 (the word before `tbz w0,#0` at 0x4b4bc4) records the JUDGMENT VALUE: its
           predicate main+0x4d2bd0 is `ldr x8,[x0,#0x20]; ldrb w0,[x8,#0x5f8]; ret`, i.e. bit0 of
           u8([[seq+0xb8]+0x20]+0x5f8). bit0=1 => fall through => setNerve(0x12d0428 DemoOpening);
           bit0=0 => return at 0x4b4d2c, no DemoOpening at all. G2 id 109 main+0x4b4bb8 (its return
           address IS the fork entry 0x4b4bbc). G3 id 110 main+0x59ac30 (scene-graph create entry,
           'Stage' arg) + G4 id 111 main+0x59accc (its ret => return x0) answer the draws=0 question.
           (switch smo_m8e2) */
        if (runtime_m8e2_poll) runtime_m8e2_poll((unsigned)(n * 2UL));
        /* M10A (2026-11-08): FLIP THE FIRST-BOOT PREDICATE BIT - gate 108 main+0x4b4bc0 gets a 35-word
           variant stub (installed only when smo_m10a!=0 at module load) that replays the real bl 0x4d2bd0
           and then `orr w0,w0,mask` before returning => ONLY w0's bit0 can change (mask=0 is the identity).
           New gate 112 main+0x4b4bc8 = the tbz fall-through first word (zero branch landings in the whole
           image) counts "the A branch was entered" and records x0 = what the caller actually saw.
           Watch output: [m10a] probe/code/sum/judge/judgeA/ev. (switch smo_m10a) */
        if (runtime_m10a_poll) runtime_m10a_poll((unsigned)(n * 2UL));
        if (runtime_m15a_poll) runtime_m15a_poll((unsigned)(n * 2UL));
        if (runtime_m16a_poll) runtime_m16a_poll((unsigned)(n * 2UL));
        /* M17A (2026-11-08): THE PER-QUAD RECORD LEAF. Gate 115 at main+0x98b4b0 (fn 0x98b490 +0x20)
           is the 2D quad record leaf (277 static call sites / 130 host funcs), i.e. essentially one
           hit per draw quad; draining it per draw pins every quad -- the 12 black ones included --
           to its host function, and its x2 descriptor's [x2] is the texture key. (switch smo_m17a) */
        if (runtime_m17a_poll) runtime_m17a_poll((unsigned)(n * 2UL));
        /* M18A (2026-11-08): UI COLOR ARGUMENTS - gate 116 main+0x8fea2c (x3 = color ptr
           = [ctx+0xb8]+0x658) + gate 117 main+0x9106fc (UI layer packed colors at +0xf0,
           render ctx x1). Per-draw drain with safe reads lives in nvn_vk. (switch smo_m18a) */
        if (runtime_m18a_poll) runtime_m18a_poll((unsigned)(n * 2UL));
        /* M19A (2026-11-08): gate 118 main+0x916a28 = 2D batch processor +0xc2c. RGB source (+0x700)
           + flags (+0x8b0..b2) on the same record, plus the final 12-byte color buffer. (switch smo_m19a) */
        if (runtime_m19a_poll) runtime_m19a_poll((unsigned)(n * 2UL));
        if (runtime_m7y2_poll) runtime_m7y2_poll((unsigned)(n * 2UL));
        /* M7a: every beat, switch refresh + 30 s reg census + 60 s trio/null drains + one-shot got/kick inside (switch smo_m7a). */
        /* M40W: every beat, 2 s sample of the 0x101f event pair (creator, write-end signals, reset call site) + its poller 0x1023 (switch smo_m40w). */
        if (runtime_m40w_poll) runtime_m40w_poll((unsigned)(n * 2UL));
        if (runtime_m7a_poll) runtime_m7a_poll((unsigned)(n * 2UL));
        /* M7b: every beat, switch refresh + poster br (30 s + on-change) + 100 s c13/g56 tables inside (switch smo_m7b). */
        if (runtime_m7b_poll) runtime_m7b_poll((unsigned)(n * 2UL));
        /* M7c: every beat, switch refresh + fna status (30 s) + ldr/key (60 s) + lr hist tables (100 s) inside (switch smo_m7c). */
        if (runtime_m7c_poll) runtime_m7c_poll((unsigned)(n * 2UL));
        /* M7e: every beat, switch refresh + 60 s th/lrd + 30 s crt + on-change crtrow inside (switch smo_m7e). */
        if (runtime_m7e_poll) runtime_m7e_poll((unsigned)(n * 2UL));
        /* M7g: every beat, switch refresh + 30 s per-gate sum + 60 s top-3 LR + 120/240 s ring tails inside (switch smo_m7g). */
        if (runtime_m7g_poll) runtime_m7g_poll((unsigned)(n * 2UL));
        /* M7h: every beat, switch refresh + 30 s per-gate sum + verdict (derived cancel/exh/inflight) + 60 s per-LR last tick + 120/240 s ring tails inside (switch smo_m7h). */
        if (runtime_m7h_poll) runtime_m7h_poll((unsigned)(n * 2UL));
        /* M7N: who posted to whom — G1 factory main+0x964260 / G2 PostToken main+0x9646d0,
           10 s sums + 20 s role-keyed lr/obj ledgers + code-window word compare + first-64 rows
           at 20/100 s. The two gates arm themselves for the first 256 hits so the bootstrap
           post is not lost before the switch file is read. (switch smo_m7n). */
        if (runtime_m7n_poll) runtime_m7n_poll((unsigned)(n * 2UL));
        /* M7P: one gate on the worker Start main+0x963e50 - the single convergence point of all 15 ctor sites - prints the per-site LR table and the worker->first-site ledger. (switch smo_m7p) */
        if (runtime_m7p_poll) runtime_m7p_poll((unsigned)(n * 2UL));
        /* M7Q: the layer above the missing Start sites - WRAP ctor entry main+0x964540 (per-caller
           LR diff between the two sides), FO/FOB fanout gates on the 3-lane builder path (w1). (switch smo_m7q) */
        if (runtime_m7q_poll) runtime_m7q_poll((unsigned)(n * 2UL));
        /* M7S2: three read-only gates that pin the breakpoint above pool B's 5 missing ctor sites:
           G1 main+0x681b30 (the single entry of that whole branch, only static caller bl@0x74cae0),
           G2 main+0x74ca00 (the layer above; 8 static bl callers found offline), and G3 main+0x74cadc
           = the word BEFORE bl@0x74cae0 (the bl itself is never overwritten). (switch smo_m7s2) */
        if (runtime_m7s2_poll) runtime_m7s2_poll((unsigned)(n * 2UL));
        /* M7T2: the whole family M7S pointed at - 9 stage/scene construction entry points gated in
           one build (M7S warned that one gate alone risks a false negative). All nine take tail=0
           4-word windows (every first word rewrites sp). 10 s sums + per-caller 32-slot LR ring +
           one word-by-word window readback at t>=10 s. (switch smo_m7t2) */
        if (runtime_m7t2_poll) runtime_m7t2_poll((unsigned)(n * 2UL));
        /* M7U2 (2026-11-08): the scene-DISPATCH hub the M7T2 family sits under. A 0x7acd50 = the
           vtable-slot-0 dispatcher (H5/H7's return address 0x7acd94 lives inside it), B 0x7acd94 =
           its return address (did the dispatch RETURN), C 0x7acd70 = the stage-name use point right
           before bl 0x9564f0 (dumps name pointer + first 32 ASCII bytes). (switch smo_m7u2) */
        if (runtime_m7u2_poll) runtime_m7u2_poll((unsigned)(n * 2UL));
        /* M7V (2026-11-08): WHO the device dispatches to. X8 0x7acd8c = `ldr x8,[x8]` one instruction
           before `blr x8`: the gate's original x8 is the vtable pointer, so one more deref = the exact
           function the dispatch jumps to (described as main+offset). R1..R7 = the seven Rsv sibling
           instance entries (0x4a6a90/0x46e770/0x445590/0x447ae0/0x449bc0/0x44b6f0/0x4af020, tail0 4w)
           that were never gated before - whichever is nonzero names the out-of-family instance.
           Name ring v2 (no new gate): 4 raw-hex 32B slots, never rewritten. (switch smo_m7v) */
        if (runtime_m7v_poll) runtime_m7v_poll((unsigned)(n * 2UL));
        /* M7i: every beat, switch refresh + per-open ring drain (96 lines/beat); guest stacks of
           main/load/poster/feed at 10/20/30/45/60/90/124/150/180/240/300 s inside (switch smo_ldrd).
           Audio thread 0x1020 ledger every 30 s inside (switch smo_aud). */
        if (runtime_ldrd_poll) runtime_ldrd_poll((unsigned)(n * 2UL));
        if (runtime_aud_poll) runtime_aud_poll((unsigned)(n * 2UL));
        if (runtime_frz_poll) runtime_frz_poll((unsigned)(n * 2UL));   /* M7L: F1/F2/F3/A6, 5 s in t=100..200 s */
        /* M6e：每 60 s 一行 [perf]（帧率 + 一帧的钱花在哪 + top3 hot key）。只读。 */
        {
            static double pf_next;
            if (pf_next <= 0) pf_next = now + 60.0;
            else if (now >= pf_next) { pf_next = now + 60.0; if (runtime_perf_dump) runtime_perf_dump((unsigned)(n * 2UL)); }
        }
        /* M6i: every 30 s, name the threads that actually do the file I/O and the IPC. Its own cadence
           (not the file-driven probe), because the M6i question is a time series: which thread issued
           the last open/read before the loader stopped, and what it did in the 60 s before that. */
        {
            static double io_next;
            if (io_next <= 0) io_next = now + 30.0;
            else if (now >= io_next) { io_next = now + 30.0; if (runtime_io_thread_dump) runtime_io_thread_dump((unsigned)(n * 2UL)); }
        }
        /* M6r: every 30 s, the failure ledger: totals, the aggregate top 8 (call id + result +
           count), the last 8 failures, the long/expired waits and who reads the game clock. Its own
           cadence, read-only; the ADVANCE lines come from the 2 s change detector in threads.c. */
        {
            static double fa_next;
            if (fa_next <= 0) fa_next = now + 30.0;
            else if (now >= fa_next) { fa_next = now + 30.0; if (runtime_fa_dump) runtime_fa_dump((unsigned)(n * 2UL)); }
        }
        /* M5f: file-driven three-in-one snapshot (see the comment above). Only the heartbeat thread
           reads the files, and it only calls read-only dumps. */
        {
            if (!gd_seeded) { gd_seeded = 1; vb_probe_seed(); }   /* seed once, see vb_probe_seed */
            int gd_req = 0, gd_sec = 0;
            int gd_have = vb_probe_int(VB_GD_REQ_A, VB_GD_REQ_B, &gd_req);
            if (!vb_probe_int(VB_GD_SEC_A, VB_GD_SEC_B, &gd_sec)) gd_sec = 60;   /* default 60 s if absent */
            int gd_fire = 0; const char *gd_why = "-";
            if (gd_have && gd_req_last < 0) gd_req_last = gd_req;              /* first read: baseline only */
            else if (gd_have && gd_req != gd_req_last) { gd_req_last = gd_req; gd_fire = 1; gd_why = "request"; }
            if (gd_sec > 0) {
                if (gd_sec_next <= 0) gd_sec_next = now + (double)gd_sec;      /* first period starts now */
                else if (now >= gd_sec_next) {
                    gd_sec_next = now + (double)gd_sec;
                    if (!gd_fire) { gd_fire = 1; gd_why = "periodic"; }
                }
            }
            if (gd_fire) {
                vk_boot_logf("[probe] dump why=%s req=%d sec=%d frames=%lu cpu=%.1f rss=%ld",
                             gd_why, gd_req, gd_sec, frames, cpu, vb_rss_kb());
                /* M6d: the three answers the task asks for, each a number, all read-only:
                   (a) IO timeline with the delta since the previous dump and the newest open path,
                   (b) frame/submit/CPU curve for the same window,
                   (c) which thread holds which wait for how long, and who owns the waited object. */
                {
                    static unsigned long pframes, psub, ptry, pimg, pcmd;
                    char io[640];
                    if (runtime_romfs_load_line) { runtime_romfs_load_line(io, sizeof io); vk_boot_logf("[load] io t=%lus %s", n * 2UL, io); }
                    vk_boot_logf("[load] frame t=%lus frames=%lu(+%lu) submit=%lu(+%lu) present_try=%lu(+%lu) img=%lu(+%lu) cmd=%lu(+%lu) cpu=%.1f pct=%.0f stall=%.0f rss=%ld",
                                 n * 2UL, frames, frames - pframes,
                                 atomic_load_explicit(&g_beat_nvn_submit, memory_order_relaxed),
                                 atomic_load_explicit(&g_beat_nvn_submit, memory_order_relaxed) - psub,
                                 atomic_load_explicit(&g_beat_present_try, memory_order_relaxed),
                                 atomic_load_explicit(&g_beat_present_try, memory_order_relaxed) - ptry,
                                 atomic_load_explicit(&g_beat_img, memory_order_relaxed),
                                 atomic_load_explicit(&g_beat_img, memory_order_relaxed) - pimg,
                                 atomic_load_explicit(&g_beat_cmd, memory_order_relaxed),
                                 atomic_load_explicit(&g_beat_cmd, memory_order_relaxed) - pcmd,
                                 cpu, (cpu >= 0 && cpu_prev >= 0) ? (cpu - cpu_prev) * 50.0 : -1.0,
                                 now - moved, vb_rss_kb());
                    pframes = frames; psub = atomic_load_explicit(&g_beat_nvn_submit, memory_order_relaxed);
                    ptry = atomic_load_explicit(&g_beat_present_try, memory_order_relaxed);
                    pimg = atomic_load_explicit(&g_beat_img, memory_order_relaxed);
                    pcmd = atomic_load_explicit(&g_beat_cmd, memory_order_relaxed);
                }
                vb_load_cpu((unsigned)(n * 2UL));
                vb_cpu_table((unsigned)(n * 2UL));   /* M6f: per-thread CPU/state table (read-only) */
                if (runtime_load_dump) runtime_load_dump((unsigned)(n * 2UL));
                if (runtime_guest_dump) runtime_guest_dump();
                if (runtime_guest_chain_dump) runtime_guest_chain_dump(0);   /* M5j: what the main thread is waiting on */
                /* M6k-A: the guest frame chain of 0x102f (the thread whose task never finishes).
                   n counts 2 s beats, so n%45==0 is every third 30 s dump (~90 s). */
                if (runtime_guest_chain_dump && (n % 45u) == 0u) runtime_guest_chain_dump(0x102f);
                if (runtime_m5k_dump) runtime_m5k_dump();                    /* M5k: object around main's 0x1C key */
                if (runtime_ipc_stat_dump) runtime_ipc_stat_dump();
                if (runtime_romfs_stat_dump) runtime_romfs_stat_dump();
                if (runtime_movie_stat_dump) runtime_movie_stat_dump();
            }
        }
        if (now - moved >= 8.0 && dumps < 20 && (dumps == 0 || now - last_dump >= 12.0)) {
            dumps++;
            last_dump = now;
            vk_boot_logf("[frz] stall=%.0fs frames=%lu cpu=%.1f rss=%ld dump %d/20", now - moved, frames, cpu, vb_rss_kb(), dumps);
            if (runtime_guest_dump) runtime_guest_dump();
            if (runtime_guest_chain_dump) runtime_guest_chain_dump(0);   /* M5j: what the main thread is waiting on */
            if (runtime_m5k_dump) runtime_m5k_dump();                    /* M5k: object around main's 0x1C key */
        }
    }
    return NULL;
}

/* M7L F4: the 1 s frame sampler in threads.c needs the same counter the [beat] line prints. One
   relaxed atomic load, no side effects; the beat thread stays the only writer of the interval state. */
unsigned long vb_frames_now(void) { return atomic_load_explicit(&g_beat_frames, memory_order_relaxed); }

void vk_beat_start(void)
{
    static _Atomic int started;
    if (atomic_exchange(&started, 1)) return;
    pthread_t th;
    if (pthread_create(&th, NULL, beat_thread, NULL)) {
        atomic_store(&started, 0);
        vk_boot_logf("[beat] thread create FAILED (probe dead, counters still counted)");
        return;
    }
    pthread_detach(th);
    vk_boot_logf("[beat] probe thread up (2 s cadence)");
}

int vk_boot_init(void)
{
    /* 1) dladdr 拿本模块（libgame.so）自身路径 → 推出 libs 目录（不硬编码安装路径） */
    Dl_info info;
    if (!dladdr((void *)&vk_boot_init, &info) || !info.dli_fname) {
        fprintf(stderr, "[vk-boot] dladdr failed\n");
        return -1;
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", info.dli_fname);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        fprintf(stderr, "[vk-boot] bad module path %s\n", info.dli_fname);
        return -2;
    }
    *slash = '\0';   /* dir = <bundle>/libs/arm64 */

    /* 2) 三件套存在性检查（access） */
    char icd[PATH_MAX], loader[PATH_MAX], icd_so[PATH_MAX], cpp[PATH_MAX];
    snprintf(icd,    sizeof icd,    "%s/vk_swiftshader_icd.json", dir);
    snprintf(loader, sizeof loader, "%s/libvulkan.so", dir);
    snprintf(icd_so, sizeof icd_so, "%s/libvk_swiftshader.so", dir);
    snprintf(cpp,    sizeof cpp,    "%s/libc++_shared.so", dir);
    int e_loader = access(loader, R_OK) == 0;
    int e_so     = access(icd_so, R_OK) == 0;
    int e_icd    = access(icd, R_OK) == 0;
    int e_cpp    = access(cpp, R_OK) == 0;
    snprintf(g_icd_so, sizeof g_icd_so, "%s", icd_so);   /* 供 VK_LUNARG 直加驱动使用 */

    /* 3) 把 ICD manifest 喂给随包 loader。
     * 实测（2026-11-02 构建）：hvigor ProcessLibs 只把 entry/libs/<abi>/*.so 打进 HAP，
     * vk_swiftshader_icd.json 不入包 ⇒ 这里运行期生成 manifest，library_path 用绝对路径，
     * 同时绕开 loader 对 "./相对路径" 解析行为的不确定性。
     * M3b：候选从 1 个扩成 6 个并逐个打 access/fopen/errno——上一版只有一个写死路径，
     * 写失败时 env 静默为空（20:01:24.940 落了 manifest、老板 loader 在同一秒读 env 仍是空，
     * 就是「写没写成功」这一跳没有证据）。先认「已在 libs 里的现成 manifest」，再依次写。 */
    char cand[6][PATH_MAX];
    int ncand = 0;
    const char *tmpdir = getenv("TMPDIR");
    const char *home = getenv("HOME");
    snprintf(cand[ncand++], PATH_MAX, "%s/vk_swiftshader_icd.json", dir);   /* 若将来打进包：只读即用 */
    snprintf(cand[ncand++], PATH_MAX, "/data/storage/el2/base/files/vk_swiftshader_icd.json");
    if (tmpdir && *tmpdir)
        snprintf(cand[ncand++], PATH_MAX, "%s/vk_swiftshader_icd.json", tmpdir);
    snprintf(cand[ncand++], PATH_MAX, "/data/storage/el2/base/cache/vk_swiftshader_icd.json");
    snprintf(cand[ncand++], PATH_MAX, "/data/storage/el2/base/temp/vk_swiftshader_icd.json");
    snprintf(cand[ncand++], PATH_MAX, "/data/storage/el2/base/haps/entry/files/vk_swiftshader_icd.json");

    char gen[PATH_MAX]; gen[0] = '\0';
    int env_set = 0, rv_df = -1, rv_ic = -1;
    for (int i = 0; i < ncand && !env_set; i++) {
        if (access(cand[i], R_OK) == 0) {           /* 现成文件：不写，直接用 */
            VB_LOG("[vk-boot] cand[%{public}d] %{public}s ready (read-only)", i, cand[i]);
            snprintf(gen, sizeof gen, "%s", cand[i]);
            env_set = 1;
            break;
        }
        int werr = 0;
        int wok = vb_write_manifest(cand[i], dir, &werr);
        VB_LOG("[vk-boot] cand[%{public}d] %{public}s write=%{public}d errno=%{public}d",
               i, cand[i], wok, werr);
        if (wok) { snprintf(gen, sizeof gen, "%s", cand[i]); env_set = 1; }
    }

    /* 4) setenv + 立刻读回实际值：这一行是「setenv 到底生效没有」的唯一判据 */
    if (env_set) {
        rv_df = setenv("VK_DRIVER_FILES", gen, 1);
        rv_ic = setenv("VK_ICD_FILENAMES", gen, 1);
        setenv("VK_ADD_DRIVER_FILES", gen, 1);
    }
    {
        const char *df = getenv("VK_DRIVER_FILES");
        const char *ic = getenv("VK_ICD_FILENAMES");
        char libpath[PATH_MAX];
        snprintf(libpath, sizeof libpath, "%s/libvk_swiftshader.so", dir);
        VB_LOG("[vk-boot] dir=%{public}s icd_in_libs=%{public}d loader=%{public}d so=%{public}d cpp=%{public}d",
               dir, e_icd, e_loader, e_so, e_cpp);
        VB_LOG("[vk-boot] setenv rv=%{public}d/%{public}d VK_DRIVER_FILES=[%{public}s] VK_ICD_FILENAMES=[%{public}s]",
               rv_df, rv_ic, df ? df : "(null)", ic ? ic : "(null)");
        VB_LOG("[vk-boot] icd_so=%{public}s access=%{public}d TMPDIR=[%{public}s] HOME=[%{public}s]",
               libpath, access(libpath, R_OK) == 0,
               tmpdir ? tmpdir : "(null)", home ? home : "(null)");
    }

    /* 5) 诊断行（stderr 保留：x86_64/本地壳层仍按老方式看） */
    fprintf(stderr, "[vk-boot] dir=%s icd=%d loader=%d so=%d icdjson=%d (cpp=%d env=%d manifest=%s)\n",
            dir, e_icd, e_loader, e_so, e_icd, e_cpp, env_set, env_set ? gen : "-");
    return (e_loader && e_so && e_cpp && env_set) ? 0 : -3;
}

/* 库构造期执行：必须在任何 vk 符号使用（= nvn_vk_init → vkCreateInstance）之前。
 * 优先级 101 < 默认 65535，先于 applet_host.c / system_services.c 等其它 constructor。 */
__attribute__((constructor(101)))
static void vk_boot_ctor(void) { vk_boot_init(); }
