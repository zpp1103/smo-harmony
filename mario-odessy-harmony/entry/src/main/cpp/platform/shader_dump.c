/* M3p：真实 shader 语料落盘（为离线预烘 SPIR-V 备料）。
 * 布局事实（notes 04-轮次/evidence_m2/m3k_shader_route.md:22-29，离线 shader_split.py/nvn_mtl.m 同源）：
 *   NVNshaderData{u64 data; const void *control;} 无长度字段（nvn.h:148-151）；
 *   control 段：+0x00 magic=0x98761234、+0x20 自述长度(0x760..0x880)、+0x704 代码长度、+0x714 stage(0/1/5)；
 *   code  段：首 4 字节 0x12345678(图形)/0x12345679(计算)。
 * 键 = sha256(ctl||code) 前 8 字节的 16 个 hex 字符 —— 与离线 shader-conv/out 的 key 口径一致，可对账。
 * 纪律：先做边界校验，任一不符即计入 bad 并拒收（绝不越界读）；落盘失败只计数，不进渲染路径。 */
#include "shader_dump.h"
#include "vk_boot.h"
#include "runtime.h"   /* M5l: runtime_safe_read / runtime_addr_range_mapped（防坏 ctl/code 指针打死进程） */
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>   /* M92A：strtol / malloc（缺失 shader 暂存区） */
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

/* M3w P3：语料扩容（200 / 8192 / 64MB 在 boot1 已打满：dumped=200 full=7993 loseen=21083）。
   上限仍必须存在且计数，别把设备写爆：4096 键 / 256MB 硬顶，超限只进 full / loseen 计数。 */
#define SD_MAX_KEYS   4096ull                             /* 唯一键上限：超出只计数（[s] dump full） */
#define SD_MAX_BYTES  (256ull<<20)                        /* 总字节上限 */
#define SD_SEEN_MAX   65536u                              /* 判重表容量；槽位 = 2 倍，开放寻址 */
#define SD_SEEN_SLOTS (SD_SEEN_MAX * 2u)                  /* 线性扫 65536 会退化（6e9 次比较），改哈希 */
#define SD_CTL_MAX    0x2000u                             /* 控制段上限（实测 0x760..0x880，留冗余） */
#define SD_CODE_MAX   0x10000u                            /* 代码段上限（实测 max 15056B，派活单口径 <64KB） */

static char g_dir[512];
static int  g_init;                        /* 0=未试 1=可用 -1=不可用（不重试，避免每次调用都 mkdir） */
static unsigned g_seen_n;
static uint64_t g_seen[SD_SEEN_SLOTS];     /* 0 = 空槽（键 0 映射成 1，2^-64 可忽略） */
/* M3w P3：落盘语料的形状统计（平均/最大 code 长度、stage 分布），只统计真正落盘的键 */
static unsigned long long g_code_sum;
static unsigned g_code_max;
static unsigned long long g_stage_n[3];    /* 0=vert 1=frag 2=compute(原 stage=5 归到这一桶) */
static unsigned long g_calls, g_uniq, g_dumped, g_bad, g_bytes, g_full, g_loseen, g_exists;
static unsigned g_logged_ok, g_logged_bad;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;


/* 目录：与 vk_boot.log 同域（已证实可写，且 hdc file recv 拉得回——PPM/log 已证）。 */
static void sd_dir_init(void)
{
    const char *lp = vk_boot_log_path();
    if (!lp) { g_init = -1; return; }
    snprintf(g_dir, sizeof g_dir, "%s", lp);
    char *slash = strrchr(g_dir, '/');
    if (!slash) { g_init = -1; return; }
    snprintf(slash, sizeof g_dir - (size_t)(slash - g_dir), "/shaderdump");
    if (mkdir(g_dir, 0777) != 0 && errno != EEXIST) {
        vk_boot_logf("[s] dump dir mkdir failed path=%s errno=%d", g_dir, errno);
        g_init = -1;
        return;
    }
    chmod(g_dir, 0777);   /* app 私有目录默认 0700 也能 recv（同 log/PPM），这里放宽便于取证 */
    g_init = 1;
}

static void hex16(const uint8_t *d, char out[17])
{
    for (int i = 0; i < 8; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
    out[16] = '\0';
}

/* 判重：开放寻址哈希（键 = sha256 前 8 字节）。原线性扫在 8192 时已是 O(n)，65536 会到 1e9 量级比较，
   直接拖慢渲染线程，所以容量放大必须同时换实现。容量语义不变：超过 SD_SEEN_MAX 个唯一键后计 loseen。 */
static int sd_seen(uint64_t k)
{
    if (!k) k = 1;                                   /* 0 是空槽标记 */
    uint64_t h = k * 0x9E3779B97F4A7C15ull;
    const unsigned mask = SD_SEEN_SLOTS - 1u;
    unsigned base = (unsigned)(h >> 17);
    for (unsigned i = 0; i < SD_SEEN_SLOTS; i++) {
        uint64_t *s = &g_seen[(base + i) & mask];
        if (*s == k) return 1;
        if (*s == 0) {
            if (g_seen_n >= SD_SEEN_MAX) { g_loseen++; return 1; }   /* 表满：按已见处理，不重复落盘 */
            *s = k;
            g_seen_n++;
            return 0;
        }
    }
    g_loseen++;
    return 1;
}

static int sd_write(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(p, 1, n, f);
    int closed = fclose(f);
    return (w == n && closed == 0) ? 0 : -1;
}

static void sd_totals(const char *why)
{
    unsigned long long avg = g_dumped ? g_code_sum / g_dumped : 0ull;
    vk_boot_logf("[s] shader totals n=%lu uniq=%lu dumped=%lu bad=%lu bytes=%lu full=%lu loseen=%lu "
                 "exists=%lu avg_code=%llu max_code=%u s0=%llu s1=%llu s5=%llu seen=%u (%s)",
                 g_calls, g_uniq, g_dumped, g_bad, g_bytes, g_full, g_loseen, g_exists,
                 avg, g_code_max, g_stage_n[0], g_stage_n[1], g_stage_n[2], g_seen_n, why);
}

/* M3r（2026-11-03）：校验抽成一处，落盘侧（sd_one）与运行时查表侧（shader_key）共用同一判据，
   防止两边判据漂移导致「dump 有、查表没有」。 */
/* M3v P2：why = 首个不满足的判据（sd_validate 按原 `&&` 链的顺序逐个判，第一个失败就停），
   让 key_bad 能按原因归因，而不是只知道「1011 条不好」。 */
enum { SD_OK = 0, SD_BAD_PTR, SD_BAD_MAGIC, SD_BAD_CTL_LEN, SD_BAD_CODE_LEN, SD_BAD_STAGE, SD_BAD_HDR };
typedef struct { uint32_t magic, ctl_len, code_len, stage, hdr; int why; int no_sha; } sd_hdr;
static unsigned long long g_cs_nohdr;   /* M3v P2：stage=5 无图形 magic 头而放行的笔数 */
static unsigned g_mapgate_logged;       /* M5m：只记不判的映射结论样本数（前 8 笔） */

/* M5n：shader-key 这条路径读 guest 内存是**为了算 key**，所以一次假"读不动"就等于丢一条管线。
   统一走 runtime_safe_read_ungated（硬边界 + SIGSEGV 兜底，不把截断/冻结的 maps 表当判据）：
   m5m 实测该表把 104216 次可读访问判成不可读、真 fault 0 次，结果 52030 次 stage key 全 -1。
   取证侧（threads.c 的 fx_read64）仍用带闸门的 runtime_safe_read，那里少一个样本无所谓。 */
static int sd_read(void *dst, uint64_t a, size_t n) { return runtime_safe_read_ungated(dst, a, n); }

/* ---- M7D3（2026-11-07）：真机 key=0 的取证与单点修（两个文件开关，C 默认 0） ----
   · smo_m7f = 探针：每 stage 一条 `[m7f] set` + `[m7f] key`（各硬上限 64 行），
     外加每 5 s 一次 `[m7f] badwhy stage=… reason=… n=…`（key_bad 的 stage×reason 直方图）。
   · smo_sha_ungated = 单点修（旧名 smo_m7fix 仍被认作别名）：`no_sha` 不再由
     /proc/self/maps 预测表（最多 768 条、首次快照，loader.c:396/411/457）决定，
     改成**真读**（M5n 的 runtime_safe_read_ungated）决定：读得动 ⇒ no_sha=0 ⇒ 算真 SHA；
     真读不动 ⇒ no_sha=1。整段 SHA 也走分块安全读，任一块读不动才退化成零 key。
     0 = 逐字恢复旧行为（旧表判据 + 整段裸 CC_SHA256_Update + 未映射时填空零 key）。
   回退：`echo 0 > …/smo_m7f` / `smo_sha_ungated`（≤2 s 生效），不重建也能关。 */
#define D3F_PATH_A "/data/storage/el2/base/files/smo_m7f"
#define D3F_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7f"
#define D3SHA_PATH_A "/data/storage/el2/base/files/smo_sha_ungated"
#define D3SHA_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_sha_ungated"
#define D3FIX_PATH_A "/data/storage/el2/base/files/smo_m7fix"     /* 旧名，别名 */
#define D3FIX_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m7fix"
/* M42C（2026-11-10）：真机 key 全零的**根因**与默认值反转。
   证据链（真机 192.168.0.112:42323，v1.4.1）：
     `[s] mapgate ... mapped=0` ×24 → `[s] sha skip ... (segment unmapped)` → `no_sha=1`
     → `sd_sha_key` 走 sd_zero_key ⇒ key="0000000000000000"（`[r] skip nokey vs=0…`）
     → index.json 里没有这个 key（真机实测 `grep -c 0000000000000000 index.json` = 0）
     → 全部 draw `skip_noprog`（205,942 笔）、draws=0。
   同 hap 模拟器 0 条 mapgate（表判 mapped）⇒ no_sha=0 ⇒ 真 key ⇒ draws=3251。
   结论：/proc/self/maps 预测表（768 条、首次快照，loader.c:405-424/457）在真机把 5.4 GB romfs 的
   guest 映射挤掉，**不能当判据**（M5n 已实测 104,216 次假"不可读"）；M7D3 真机实测真读判据
   no_sha=0、真 key、draws=9133。所以把**真读判据设成默认**，旧口径改为显式开关 smo_maps_gate=1。
   开关语义：smo_sha_ungated=1 强制真读（旧名 smo_m7fix）；smo_maps_gate=1 退回旧 maps 判据。 */
#define D3MG_PATH_A "/data/storage/el2/base/files/smo_maps_gate"
#define D3MG_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_maps_gate"
static unsigned long long d3_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)(ts.tv_nsec / 1000000L);
}
/* -1 = 两个路径都没有这个文件；否则读第一个字符（'1' = on）。 */
static int d3_file_tri(const char *pa, const char *pb)
{
    char b[8]; FILE *f;
    for (int i = 0; i < 2; i++) {
        if ((f = fopen(i ? pb : pa, "r"))) {
            size_t n = fread(b, 1, sizeof b - 1, f);
            fclose(f);
            return (n > 0 && b[0] == '1') ? 1 : 0;
        }
    }
    return -1;
}
static int d3_file_on(const char *pa, const char *pb) { return d3_file_tri(pa, pb) == 1; }
/* 2 s 缓存：hot path 上只有一次单调时钟读，稳态零 I/O。 */
static int d3_cache(int (*raw)(void), int *v, unsigned long long *stamp)
{
    unsigned long long t = d3_now_ms();
    if (*v < 0 || t >= *stamp + 2000ull) { *v = raw() ? 1 : 0; *stamp = t; }
    return *v;
}
static int g_d3f = -1, g_d3sha = -1, g_d3mg = -1;
static unsigned long long g_d3f_stamp, g_d3sha_stamp, g_d3mg_stamp;
static int d3_f_raw(void) { return d3_file_on(D3F_PATH_A, D3F_PATH_B); }
static int d3_sha_raw(void)
{
    int v = d3_file_tri(D3SHA_PATH_A, D3SHA_PATH_B);
    if (v < 0) v = d3_file_tri(D3FIX_PATH_A, D3FIX_PATH_B);   /* 旧名别名 */
    return v == 1;
}
/* M42C：maps 当判据的逃生阀（默认关 = 不用 maps 表，用真读）。 */
static int d3_mg_raw(void) { return d3_file_on(D3MG_PATH_A, D3MG_PATH_B); }
static int d3_vf(void) { return d3_cache(d3_f_raw, &g_d3f, &g_d3f_stamp); }
static int d3_vsha(void) { return d3_cache(d3_sha_raw, &g_d3sha, &g_d3sha_stamp); }
static int d3_vmg(void) { return d3_cache(d3_mg_raw, &g_d3mg, &g_d3mg_stamp); }
/* M42C：no_sha 的判据选择 —— 默认真读；smo_maps_gate=1 才用 maps 预测表；smo_sha_ungated=1 永久真读。 */
static int sd_ungated(void) { return d3_vsha() || !d3_vmg(); }
__attribute__((constructor)) static void d3_seed(void)
{
    static const char *const p[8] = { D3F_PATH_A, D3F_PATH_B, D3SHA_PATH_A, D3SHA_PATH_B,
                                      D3FIX_PATH_A, D3FIX_PATH_B, D3MG_PATH_A, D3MG_PATH_B };
    for (int i = 0; i < 8; i++) {
        FILE *r = fopen(p[i], "r");
        if (r) { fclose(r); continue; }
        FILE *f = fopen(p[i], "w");
        if (f) { fputs("0\n", f); fclose(f); }
    }
}
/* 分块安全读 → SHA256。返回 0 = 某一段真读不动（此时调用方填零 key，与旧行为同口径）。 */
static int sd_sha_seg(CC_SHA256_CTX *c, uint64_t a, size_t n)
{
    uint8_t buf[512];
    while (n) {
        size_t k = n > sizeof buf ? sizeof buf : n;
        if (!runtime_safe_read_ungated(buf, a, k)) return 0;
        CC_SHA256_Update(c, buf, k);
        a += (uint64_t)k; n -= k;
    }
    return 1;
}

static int sd_validate(const void *ctl, uint64_t code_addr, sd_hdr *h)
{
    h->why = SD_BAD_PTR;
    h->hdr = 0;
    h->no_sha = 0;
    if (!ctl || (uintptr_t)ctl < 0x1000 || code_addr < 0x1000) return 0;
    /* M5m：闸门改「只记不判」。M5l 曾把 /proc/self/maps 表当**判据**（表只收 768 条，截断就会把合法
       shader 判成 SD_BAD_PTR）。现在判据恢复 pre-M5l 原口径（只有上面三个条件 + 下面逐字段检查）：
         · 映射表结论只打日志（前 8 笔 [s] mapgate，绝不改 h->why）；
         · 读仍走 runtime_safe_read —— 这是读法不是判据，读不动返回 0 不 fault（pre-M5l 是裸读=崩）。 */
    if (g_mapgate_logged < 8 && !runtime_addr_range_mapped((uintptr_t)ctl, 0x718)) {
        g_mapgate_logged++;
        vk_boot_logf("[s] mapgate ctl=%p code=%#llx range=0x718 mapped=0 (log only; criterion unchanged)",
                     ctl, (unsigned long long)code_addr);
    }
    uint32_t f0 = 0, f1 = 0, f2 = 0, f3 = 0;
    if (!sd_read(&f0, (uint64_t)(uintptr_t)ctl, 4) ||
        !sd_read(&f1, (uint64_t)(uintptr_t)ctl + 0x20, 4) ||
        !sd_read(&f2, (uint64_t)(uintptr_t)ctl + 0x704, 4) ||
        !sd_read(&f3, (uint64_t)(uintptr_t)ctl + 0x714, 4)) {
        /* 读不动 = 指针真的不可读（pre-M5l 这里直接 SEGV 打死进程）。只记一笔，判据仍是 SD_BAD_PTR。 */
        if (g_mapgate_logged < 8) { g_mapgate_logged++; vk_boot_logf("[s] sd readfail ctl=%p (would have faulted pre-M5l)", ctl); }
        return 0;
    }
    h->magic = f0;
    h->ctl_len = f1;
    h->code_len = f2;
    h->stage = f3;
    /* 先只验控制段，全过了才去碰 code 指针——不然一个坏 data 指针就是段错误，坏数据只该计数。
       判据本身与本轮之前逐字等价（同一条链、同一顺序），只是把「第一个不过的」记进 h->why。 */
    if (h->magic != 0x98761234u)                          h->why = SD_BAD_MAGIC;
    else if (h->ctl_len < 0x40u || h->ctl_len > SD_CTL_MAX)  h->why = SD_BAD_CTL_LEN;
    else if (h->code_len < 0x30u || h->code_len > SD_CODE_MAX) h->why = SD_BAD_CODE_LEN;
    else if (!(h->stage == 0 || h->stage == 1 || h->stage == 5)) {
        /* M42C：判据不变（仍然拒），但**先读 hdr 再判**——旧顺序让真机日志永远打 hdr=00000000，
           把"stage 不合白名单"误读成"hdr 读不到"，本轮排查就被这条假线索带偏过一次。 */
        uint32_t hd0 = 0;
        if (sd_read(&hd0, code_addr, 4)) h->hdr = hd0;
        h->why = SD_BAD_STAGE;
        return 0;
    }
    else {
        uint32_t hd = 0;
        if (!sd_read(&hd, code_addr, 4)) { h->hdr = 0; h->why = SD_BAD_HDR; return 0; }
        h->hdr = hd;
        /* M3v P2（取证后修）：stage=5（compute）的 code 段**没有**图形那两枚 magic 头。
           boot1 的 3 条样本一致：magic=98761234 / ctl_len=2120 / code_len=11520|12032 / stage=5 /
           code 首字 = 0xe22007f0（真实 SASS 首指令，不是 0x1234567x），判据却在 hdr 这关把 100%
           的计算 shader 判成坏数据（key_bad 1011 全部来自 stage=5，s0/s1 都是 0）。
           所以计算段只验已通过的控制段 + 长度，不再要求图形 magic 头；命中的笔数单独计数。 */
        if (h->stage == 5) { h->why = SD_OK; g_cs_nohdr++; }
        else h->why = (h->hdr == 0x12345678u || h->hdr == 0x12345679u) ? SD_OK : SD_BAD_HDR;
    }
    /* M5m：控制段过判据之后还有两段整块读（sd_sha_key / sd_one 的 SHA256 会整段 touch）。这里只把
       「整段是否映射」记进 h->no_sha，**不改判据**；no_sha=1 时两处 SHA256 都跳过（那是我们自己的
       取证计算，不是 shader 的合法性判据），这样既不会误判合法 shader，也不会自己撞进未映射页。
       M7D3（smo_sha_ungated=1）：no_sha 改由**真读**决定 —— 各段尾字节走一遍 M5n ungated 读，
       读得动就是 mapped（旧表只用来预报，不再当判据）；真读不动才置 1。0 = 旧行为逐字不变。
       M42C（2026-11-10）：**默认反转** —— 真读判据是默认（sd_ungated()==1），maps 预测表只在
       smo_maps_gate=1 时当判据。理由见上面 M42C 注释块（真机 mapgate mapped=0 ⇒ no_sha=1 ⇒ 零 key）。 */
    if (h->why == SD_OK) {
        if (sd_ungated()) {
            uint8_t pb = 0;
            if (!sd_read(&pb, (uint64_t)(uintptr_t)ctl + (uint64_t)h->ctl_len - 1u, 1) ||
                !sd_read(&pb, code_addr + (uint64_t)h->code_len - 1u, 1))
                h->no_sha = 1;
        } else if (!(runtime_addr_range_mapped((uintptr_t)ctl, h->ctl_len) &&
                     runtime_addr_range_mapped(code_addr, h->code_len))) {
            h->no_sha = 1;
        }
    }
    return h->why == SD_OK;
}

/* ===================== M92A（2026-11-13）：缺失 shader dump =====================
   背景（M91A 实测定论）：真机 3D 帧段 9,895 笔 draw 因为 r_key() 在 shader 语料里查不到 vs/fs 键
   被整笔跳过（`noentry=0 / nokey=9895 / cache_keys=14422`），而语料本身完好（14,422 条、与本机逐字节一致）
   ⇒ 真机跑了**语料里从没烘过的 shader 变体**。本块把这些"缺的"shader 的控制码原样抓出来，
   供下一单装机跑 3D 段 → 离线烘 spv → 重建 index.json/spv.bin。

   开关 `smo_m92a`（位掩码；缺省 0 = 本块整段不进，逐字节旧行为）：
     bit0 = r_bin_spv 的键比较 15 → 16（真正的比较在 nvn_vk.c:6512，这里只提供读位掩码）；
     bit1 = 缺失 shader dump：SetShaders 期把「语料里没有」的 (key, ctl, code) **拷进内存暂存区**
            （见 sd_one 里的 m92a_maybe_stash），真机 draw 判 nokey 时再把该键写成自描述文件
            （见 shader_dump_missing_note）。**draw 路径里零 guest 读、零 RT 绑定/布局切换** ——
            M89A 曾在 draw 录制路径里切 RT 布局，把设备拖死 ~27 min，这次只在 SetShaders 期取字节；
     bit2 = 只统计不落盘（不暂存、不写文件，只打 `[m92a] stats` 汇总，供先跑一轮看规模）。
   `smo_m92a_dir`：输出目录（缺省 files/shader_dump92；相对路径按 /data/storage/el2/base 解析）。

   落盘格式（每键一个文件 `<dir>/<key>.<stage>.m92a`，**自描述**；stage 0=VS 1=FS）：
     +0   char magic[8] = "M92ADMP1"
     +8   u32 version = 1        +12 u32 header_bytes = 128
     +16  u16 stage              +18 u16 flags (bit0=1：ctl/code 两个段都在)
     +20  u32 ctl_len            +24 u32 code_len
     +28  u32 n_draws（首见时该笔的索引数）  +32 u64 rt_key（目标 RT 键 nw_tgt_key 口径）
     +40  char key[17]（本 stage 键，16 hex + NUL）
     +57  char vs[17]            +74 char fs[17]
     +128 ctl 字节（ctl_len）    +128+ctl_len code 字节（code_len）
   同目录另有 `m92a_index.txt`（每键一行：key/stage/ctl/code/bytes/rt/vs/fs/draws/文件名）与
   `m92a_pairs.txt`（每对 (vs,fs,rt) 一行），离线烘焙按文本行对账即可。
   上限：语料落盘侧 SD_MAX_KEYS(4096)/SD_MAX_BYTES(256MB) 在真机 3D 段覆盖完前就会打满
   ⇒ bit1 时抬到 60,000 键 / 1 GiB（仍是硬顶，且"已满"时点名丢弃哪个 key）；暂存区另给
   4,096 键 / 64 MiB（实测 68 对 ≈ 2.8 MB 量级，够用且不惊动 LMK）。
   纪律：所有失败路径只计数 + 一行日志，绝不改渲染行为、绝不阻塞 draw。 */
#define M92A_A     "/data/storage/el2/base/files/smo_m92a"
#define M92A_B     "/data/app/el2/100/base/com.vibecc.smo/files/smo_m92a"
#define M92A_DIR_A "/data/storage/el2/base/files/smo_m92a_dir"
#define M92A_DIR_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m92a_dir"
#define M92A_DIR_DEF "files/shader_dump92"
#define M92A_STASH_KEYS   4096u                    /* 暂存键上限（去重后） */
#define M92A_STASH_BYTES  (64ull << 20)            /* 暂存区字节上限 */
#define M92A_PAIR_MAX     1024u                    /* (vs,fs,rt) 去重表容量 */
#define M92A_HDR          128u                     /* 自描述头固定长度 */
#define M92A_KEY_TAG      0x4d393241u              /* 'M92A'：只用于日志/自检辨识 */

extern int nvn_vk_key_in_corpus(const char *key);   /* nvn_vk.c：r_key() 命中即"在语料里" */
extern int nvn_vk_corpus_ready(void);               /* nvn_vk.c：语料是否已装好（g_rk_n>0） */

typedef struct {
    char     key[17];
    uint32_t off;          /* 暂存区偏移（ctl 紧跟 code） */
    uint32_t ctl_len, code_len;
    uint16_t stage;
    uint8_t  dumped;       /* 1 = 已落盘（每键只落一次） */
} m92a_rec;
typedef struct { char vs[17], fs[17]; uint64_t rt; unsigned long long n; } m92a_pair;

static pthread_mutex_t g_mu92 = PTHREAD_MUTEX_INITIALIZER;
static int      g_m92a_bits = -2;          /* -2 = 还没读过 */
static unsigned long long g_m92a_stamp, g_m92a_stat_stamp;
static unsigned long long g_m92a_notes, g_m92a_dumped, g_m92a_dropped, g_m92a_bytes;
static unsigned      g_m92a_stash_n, g_m92a_full_logged, g_m92a_miss_logged, g_m92a_cap_logged;
static unsigned long long g_m92a_notready;
static uint8_t      *g_m92a_pool;          /* 惰性 malloc 的暂存区 */
static size_t        g_m92a_pool_used;
static m92a_rec      g_m92a_rec[M92A_STASH_KEYS];
static char          g_m92a_dir[512];
static int           g_m92a_dir_state;     /* 0=未建 1=可用 -1=不可用 */
static m92a_pair     g_m92a_pair[M92A_PAIR_MAX];
static unsigned      g_m92a_pair_n;
static unsigned long long g_m92a_pair_ovf;

/* 读一个整型开关文件（缺省 -1；与 d3_file_tri 不同，这里要的是位掩码整数）。 */
static int m92a_int(const char *a, const char *b)
{
    char buf[32];
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(i ? b : a, "r");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        const char *p = buf;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        return (int)strtol(p, NULL, 0);
    }
    return -1;
}
/* 缺的开关文件先由应用种成缺省值（hdc 只能改已存在的文件）。 */
static void m92a_seed_file(const char *a, const char *b, const char *val)
{
    FILE *f;
    for (int i = 0; i < 2; i++) if ((f = fopen(i ? b : a, "r"))) { fclose(f); return; }
    for (int i = 0; i < 2; i++)
        if ((f = fopen(i ? b : a, "w"))) { fputs(val, f); fclose(f); return; }
}
__attribute__((constructor)) static void m92a_seed(void)
{
    m92a_seed_file(M92A_A, M92A_B, "0\n");
    m92a_seed_file(M92A_DIR_A, M92A_DIR_B, M92A_DIR_DEF "\n");
}

int shader_dump_m92a_bits(void)
{
    unsigned long long t = d3_now_ms();
    if (g_m92a_bits == -2 || t >= g_m92a_stamp + 2000ull) {
        g_m92a_stamp = t;
        int v = m92a_int(M92A_A, M92A_B);
        if (v < 0) v = 0;
        v &= 7;                                   /* 只认 bit0/1/2 */
        if (v != g_m92a_bits) {
            int was = g_m92a_bits;
            g_m92a_bits = v;
            if (v || was > 0)   /* 缺省 0 不打日志（默认路径零输出），只有变化时才说一句 */
                vk_boot_logf("[m92a] knob smo_m92a=%d (0x%x) strict16=%d dump=%d stats_only=%d dir=%s",
                             v, v, v & 1, (v & 2) ? 1 : 0, (v & 4) ? 1 : 0, M92A_DIR_DEF);
        }
    }
    return g_m92a_bits;
}

/* 输出目录：smo_m92a_dir（缺省 files/shader_dump92）。绝对路径直接用；相对路径按
   /data/storage/el2/base 解析（与 files/ 同一沙箱视角，hdc file recv 拉得回）。 */
static const char *m92a_dir(void)
{
    if (g_m92a_dir_state) return g_m92a_dir_state > 0 ? g_m92a_dir : NULL;
    char v[256];
    int has = 0;
    for (int i = 0; i < 2 && !has; i++) {
        FILE *f = fopen(i ? M92A_DIR_B : M92A_DIR_A, "r");
        if (!f) continue;
        size_t n = fread(v, 1, sizeof v - 1, f);
        fclose(f);
        v[n] = 0;
        char *s = v;
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        size_t l = strlen(s);
        while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
        if (l) { memmove(v, s, l + 1); has = 1; }
    }
    if (!has) snprintf(v, sizeof v, "%s", M92A_DIR_DEF);
    if (v[0] == '/') {
        snprintf(g_m92a_dir, sizeof g_m92a_dir, "%s", v);
    } else {
        char base[512] = "/data/storage/el2/base";
        const char *lp = vk_boot_log_path();
        if (lp && lp[0]) {
            snprintf(base, sizeof base, "%s", lp);
            char *s = strrchr(base, '/');
            if (s) { *s = 0; }                    /* 去掉文件名 → …/files */
            s = strrchr(base, '/');
            if (s) { *s = 0; }                    /* 去掉 files → …/base */
            if (!base[0]) snprintf(base, sizeof base, "/data/storage/el2/base");
        }
        snprintf(g_m92a_dir, sizeof g_m92a_dir, "%s/%s", base, v);
    }
    if (mkdir(g_m92a_dir, 0777) != 0 && errno != EEXIST) {
        vk_boot_logf("[m92a] dir mkdir FAILED path=%s errno=%d -> dump disabled", g_m92a_dir, errno);
        g_m92a_dir_state = -1;
        return NULL;
    }
    chmod(g_m92a_dir, 0777);   /* app 私有目录 0700 也能 recv，这里放宽便于取证 */
    g_m92a_dir_state = 1;
    vk_boot_logf("[m92a] dir=%s", g_m92a_dir);
    return g_m92a_dir;
}

/* `[m92a] stats dumped=… pairs=… dropped=…`（周期 + 每次新 dump 之后 + 收尾各一行）。 */
static void m92a_stats(const char *why)
{
    vk_boot_logf("[m92a] stats dumped=%llu pairs=%llu dropped=%llu notes=%llu stash=%u/%u "
                 "stash_bytes=%llu/%llu pair_ovf=%llu notready=%llu bytes=%llu mode=%s dir=%s why=%s",
                 g_m92a_dumped, (unsigned long long)g_m92a_pair_n, g_m92a_dropped, g_m92a_notes,
                 g_m92a_stash_n, M92A_STASH_KEYS, (unsigned long long)g_m92a_pool_used,
                 M92A_STASH_BYTES, g_m92a_pair_ovf, g_m92a_notready, g_m92a_bytes,
                 (g_m92a_bits & 4) ? "stats_only" : "dump",
                 g_m92a_dir_state > 0 ? g_m92a_dir : "-", why);
}

/* (vs,fs,rt) 去重表：判覆盖率用；落盘模式同步追加 m92a_pairs.txt（离线烘培要知道哪对一起用）。 */
static void m92a_pair_note(const char *vs, const char *fs, uint64_t rt, int count)
{
    if (!vs || !*vs || !fs || !*fs) return;
    unsigned long long n = (count > 0) ? (unsigned long long)count : 0ull;
    for (unsigned i = 0; i < g_m92a_pair_n; i++)
        if (g_m92a_pair[i].rt == rt && !strcmp(g_m92a_pair[i].vs, vs) && !strcmp(g_m92a_pair[i].fs, fs)) {
            g_m92a_pair[i].n += n;
            return;
        }
    if (g_m92a_pair_n >= M92A_PAIR_MAX) {
        if (g_m92a_pair_ovf++ < 4)
            vk_boot_logf("[m92a] pairs FULL cap=%u -> 丢弃 pair vs=%s fs=%s rt=%#llx",
                         M92A_PAIR_MAX, vs, fs, (unsigned long long)rt);
        return;
    }
    m92a_pair *p = &g_m92a_pair[g_m92a_pair_n++];
    snprintf(p->vs, sizeof p->vs, "%s", vs);
    snprintf(p->fs, sizeof p->fs, "%s", fs);
    p->rt = rt;
    p->n = n;
    if (g_m92a_dir_state > 0 && (shader_dump_m92a_bits() & 2) && !(shader_dump_m92a_bits() & 4)) {
        char path[704];
        snprintf(path, sizeof path, "%s/m92a_pairs.txt", g_m92a_dir);
        FILE *f = fopen(path, "a");
        if (f) {
            fprintf(f, "vs=%s fs=%s rt=%016llx draws=%llu\n", vs, fs, (unsigned long long)rt, p->n);
            fclose(f);
        }
    }
}

/* SetShaders 期：只把"语料里没有的键"拷进暂存区（每键一次）。调用方已持 g_mu（shader_dump_program）。 */
static void m92a_maybe_stash(const char *key, int stage, const void *ctl, uint64_t code_addr,
                             uint32_t ctl_len, uint32_t code_len)
{
    if (!key || !*key) return;
    if (!(shader_dump_m92a_bits() & 2)) return;          /* bit2/0：不暂存（省内存） */
    if (stage != 0 && stage != 1) return;                /* 只要图形两阶段（compute 不在本题） */
    if (!nvn_vk_corpus_ready()) { g_m92a_notready++; return; }   /* 语料还没装好时别把全量 shader 吞进内存 */
    if (nvn_vk_key_in_corpus(key)) return;               /* 语料里有 ⇒ 不抓（本次只补缺的） */
    pthread_mutex_lock(&g_mu92);
    for (unsigned i = 0; i < g_m92a_stash_n; i++)
        if (g_m92a_rec[i].stage == (uint16_t)stage && !strcmp(g_m92a_rec[i].key, key)) {
            pthread_mutex_unlock(&g_mu92);
            return;
        }
    if (g_m92a_stash_n >= M92A_STASH_KEYS ||
        g_m92a_pool_used + (size_t)ctl_len + (size_t)code_len > M92A_STASH_BYTES) {
        g_m92a_dropped++;
        if (++g_m92a_full_logged <= 8)
            vk_boot_logf("[m92a] stash FULL keys=%u/%u bytes=%llu/%llu -> 丢弃 key=%s stage=%d "
                         "(该键不入暂存区，dump 时会计 MISS)", g_m92a_stash_n, M92A_STASH_KEYS,
                         (unsigned long long)g_m92a_pool_used, M92A_STASH_BYTES, key, stage);
        pthread_mutex_unlock(&g_mu92);
        return;
    }
    if (!g_m92a_pool) {
        g_m92a_pool = (uint8_t *)malloc((size_t)M92A_STASH_BYTES);
        if (!g_m92a_pool) {
            g_m92a_dropped++;
            if (++g_m92a_full_logged <= 8)
                vk_boot_logf("[m92a] stash malloc FAILED bytes=%llu -> dump disabled",
                             (unsigned long long)M92A_STASH_BYTES);
            pthread_mutex_unlock(&g_mu92);
            return;
        }
    }
    uint8_t *dst = g_m92a_pool + g_m92a_pool_used;
    if (ctl_len && !sd_read(dst, (uint64_t)(uintptr_t)ctl, ctl_len)) {
        g_m92a_dropped++;
        if (++g_m92a_full_logged <= 8) vk_boot_logf("[m92a] stash read FAILED key=%s ctl=%u", key, ctl_len);
        pthread_mutex_unlock(&g_mu92);
        return;
    }
    if (code_len && !sd_read(dst + ctl_len, code_addr, code_len)) {
        g_m92a_dropped++;
        if (++g_m92a_full_logged <= 8) vk_boot_logf("[m92a] stash read FAILED key=%s code=%u", key, code_len);
        pthread_mutex_unlock(&g_mu92);
        return;
    }
    m92a_rec *r = &g_m92a_rec[g_m92a_stash_n++];
    snprintf(r->key, sizeof r->key, "%s", key);
    r->off = (uint32_t)g_m92a_pool_used;
    r->ctl_len = ctl_len;
    r->code_len = code_len;
    r->stage = (uint16_t)stage;
    r->dumped = 0;
    g_m92a_pool_used += (size_t)ctl_len + code_len;
    if (g_m92a_stash_n <= 32 || (g_m92a_stash_n & 255u) == 0u)
        vk_boot_logf("[m92a] stash key=%s stage=%d ctl=%u code=%u n=%u/%u (tag=%08x)",
                     key, stage, ctl_len, code_len, g_m92a_stash_n, M92A_STASH_KEYS, M92A_KEY_TAG);
    pthread_mutex_unlock(&g_mu92);
}

/* 把一个暂存键写成自描述文件（每键一次）。调用方持 g_mu92。返回 1 = 已落盘/本来就已落盘。 */
static int m92a_dump_key(const char *key, uint64_t rt, const char *vs, const char *fs, int n)
{
    m92a_rec *r = NULL;
    for (unsigned i = 0; i < g_m92a_stash_n; i++)
        if (!strcmp(g_m92a_rec[i].key, key)) { r = &g_m92a_rec[i]; break; }
    if (!r) {
        g_m92a_dropped++;
        if (++g_m92a_miss_logged <= 8)
            vk_boot_logf("[m92a] dump MISS key=%s (暂存区没有：语料里本来有 / 暂存已满 / SetShaders 没经过) rt=%#llx",
                         key, (unsigned long long)rt);
        return 0;
    }
    if (r->dumped) return 1;
    const char *d = m92a_dir();
    if (!d) { g_m92a_dropped++; return 0; }

    uint8_t hdr[M92A_HDR];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "M92ADMP1", 8);
    uint32_t u32;
    uint16_t u16;
    u32 = 1u;         memcpy(hdr + 8,  &u32, 4);          /* version */
    u32 = M92A_HDR;   memcpy(hdr + 12, &u32, 4);          /* header_bytes */
    u16 = (uint16_t)r->stage; memcpy(hdr + 16, &u16, 2);  /* 0=VS 1=FS */
    u16 = 1u;         memcpy(hdr + 18, &u16, 2);          /* flags bit0 = ctl/code 都在 */
    memcpy(hdr + 20, &r->ctl_len, 4);
    memcpy(hdr + 24, &r->code_len, 4);
    u32 = (uint32_t)(n > 0 ? n : 0);
    memcpy(hdr + 28, &u32, 4);                            /* n_draws（首见时） */
    memcpy(hdr + 32, &rt, 8);                             /* 目标 RT 键 */
    memcpy(hdr + 40, r->key, strlen(r->key) + 1);
    if (vs) memcpy(hdr + 57, vs, strlen(vs) + 1);
    if (fs) memcpy(hdr + 74, fs, strlen(fs) + 1);

    char path[704];
    snprintf(path, sizeof path, "%s/%s.%u.m92a", d, r->key, (unsigned)r->stage);
    FILE *f = fopen(path, "wb");
    if (!f) {
        g_m92a_dropped++;
        vk_boot_logf("[m92a] dump write FAILED key=%s stage=%u path=%s errno=%d",
                     r->key, (unsigned)r->stage, path, errno);
        return 0;
    }
    size_t total = (size_t)r->ctl_len + (size_t)r->code_len;
    size_t w1 = fwrite(hdr, 1, sizeof hdr, f);
    size_t w2 = total ? fwrite(g_m92a_pool + r->off, 1, total, f) : 0;
    int ce = fclose(f);
    if (w1 != sizeof hdr || w2 != total || ce != 0) {
        g_m92a_dropped++;
        vk_boot_logf("[m92a] dump write SHORT key=%s stage=%u want=%zu got=%zu/%zu",
                     r->key, (unsigned)r->stage, sizeof hdr + total, w1, w2);
        return 0;
    }
    r->dumped = 1;
    g_m92a_dumped++;
    g_m92a_bytes += (unsigned long long)total;
    vk_boot_logf("[m92a] dump key=%s stage=%u bytes=%zu ctl=%u code=%u n=%d rt=%#llx vs=%s fs=%s file=%s",
                 r->key, (unsigned)r->stage, sizeof hdr + total, r->ctl_len, r->code_len,
                 n, (unsigned long long)rt, vs ? vs : "-", fs ? fs : "-", path);
    char ipath[704];
    snprintf(ipath, sizeof ipath, "%s/m92a_index.txt", d);
    FILE *ix = fopen(ipath, "a");
    if (ix) {
        fprintf(ix, "key=%s stage=%u ctl=%u code=%u bytes=%zu rt=%016llx vs=%s fs=%s draws=%d file=%s.%u.m92a\n",
                r->key, (unsigned)r->stage, r->ctl_len, r->code_len, sizeof hdr + total,
                (unsigned long long)rt, vs ? vs : "-", fs ? fs : "-", n, r->key, (unsigned)r->stage);
        fclose(ix);
    }
    return 1;
}

/* nokey 的 draw 调这里（nvn_vk.c:22525 分支）。只读 g_rt_cur->key，不做任何绑定/布局改动。 */
void shader_dump_missing_note(const char *vs, const char *fs, uint64_t rt_key, int count)
{
    int bits = shader_dump_m92a_bits();
    if (!bits) return;                          /* 缺省 0：一次分支都不多（只有 2 s 缓存读） */
    pthread_mutex_lock(&g_mu92);
    g_m92a_notes++;
    m92a_pair_note(vs, fs, rt_key, count);
    if ((bits & 2) && !(bits & 4)) {            /* bit1 落盘；bit2 在时只统计 */
        if (vs && *vs) m92a_dump_key(vs, rt_key, vs, fs, count);
        if (fs && *fs) m92a_dump_key(fs, rt_key, vs, fs, count);
    }
    unsigned long long t = d3_now_ms();
    if ((g_m92a_notes & 255ull) == 0ull || t >= g_m92a_stat_stamp + 5000ull) {
        g_m92a_stat_stamp = t;
        m92a_stats("periodic");
    }
    pthread_mutex_unlock(&g_mu92);
}

/* M92A：语料落盘侧（shader_dump_program）的上限。bit1 时抬到 60,000 键 / 1 GiB；
   其它情况逐字沿用旧常量（默认路径行为不变）。 */
static unsigned long long sd_max_keys(void)
{
    return (shader_dump_m92a_bits() & 2) ? 60000ull : SD_MAX_KEYS;
}
static unsigned long long sd_max_bytes(void)
{
    return (shader_dump_m92a_bits() & 2) ? (1024ull << 20) : SD_MAX_BYTES;
}

static unsigned long long g_d3_zero_key, g_d3_real_key;   /* M7D3：占位零 key vs 真 SHA key 的计数 */

static void sd_zero_key(char out[17]) { memset(out, '0', 16); out[16] = 0; g_d3_zero_key++; }

static void sd_sha_key(const void *ctl, uint64_t code_addr, const sd_hdr *h, char out[17])
{
    if (h->no_sha) { sd_zero_key(out); return; }   /* M5m: whole-segment unmapped -> skip our own hash, never touch it */
    if (sd_ungated()) {   /* M7D3/M42C：真读分块（不信任 maps 预测表），读不动才退化成零 key */
        CC_SHA256_CTX c;
        uint8_t d[32];
        CC_SHA256_Init(&c);
        if (!sd_sha_seg(&c, (uint64_t)(uintptr_t)ctl, h->ctl_len) ||
            !sd_sha_seg(&c, code_addr, h->code_len)) { sd_zero_key(out); return; }
        CC_SHA256_Final(d, &c);
        hex16(d, out);
        g_d3_real_key++;
        return;
    }
    CC_SHA256_CTX c;
    uint8_t d[32];
    CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, ctl, h->ctl_len);
    CC_SHA256_Update(&c, (const void *)(uintptr_t)code_addr, h->code_len);
    CC_SHA256_Final(d, &c);
    hex16(d, out);
    g_d3_real_key++;
}

/* M3v P2：运行时查表侧（= gfx_bridge 的 key_bad）的失败归因。两维：
   ① 按 sd_validate 读到的原始 stage 字段分桶（0/1/5/其它/指针不可读）；
   ② 按首个不满足的判据分桶。样本：前 3 条带全字段落盘，够回答「magic/长度/stage/hdr 哪一条」。 */
static unsigned long long g_kb_reason[8];    /* 下标 = SD_BAD_* */
static unsigned long long g_kb_stage[5];     /* 0/1/5/其它/指针不可读 */
static unsigned long long g_kb_x[5][8];      /* M7D3：stage 桶 × reason 桶交叉表（[m7f] badwhy） */
static unsigned g_kb_samples;

static void sd_keybad_note(const sd_hdr *h)
{
    if (h->why >= 0 && h->why < 8) g_kb_reason[h->why]++;
    int sb;
    if (h->why == SD_BAD_PTR) sb = 4;
    else if (h->stage == 0) sb = 0;
    else if (h->stage == 1) sb = 1;
    else if (h->stage == 5) sb = 2;
    else sb = 3;
    g_kb_stage[sb]++;
    if (h->why >= 0 && h->why < 8) g_kb_x[sb][h->why]++;
    if (g_kb_samples < 3) {
        g_kb_samples++;
        vk_boot_logf("[s] key_bad sample #%u why=%s magic=%08x ctl_len=%u code_len=%u stage=%u hdr=%08x",
                     g_kb_samples,
                     h->why == SD_BAD_PTR ? "ptr" : h->why == SD_BAD_MAGIC ? "magic" :
                     h->why == SD_BAD_CTL_LEN ? "ctl_len" : h->why == SD_BAD_CODE_LEN ? "code_len" :
                     h->why == SD_BAD_STAGE ? "stage" : "hdr",
                     h->magic, h->ctl_len, h->code_len, h->stage, h->hdr);
    }
}

/* 运行时用：校验并算出 key。返 stage(0/1/5)，失败返 -1（out 不写）。 */
static int shader_key_h(const void *ctl, uint64_t code_addr, char out[17], sd_hdr *hout)
{
    sd_hdr h;
    memset(&h, 0, sizeof h);
    if (!out || !sd_validate(ctl, code_addr, &h)) {
        sd_keybad_note(&h);
        if (hout) *hout = h;
        return -1;
    }
    sd_sha_key(ctl, code_addr, &h, out);
    if (hout) *hout = h;
    return (int)h.stage;
}
int shader_key(const void *ctl, uint64_t code_addr, char out[17])
{
    return shader_key_h(ctl, code_addr, out, NULL);
}

/* M7D3：`[m7f] badwhy stage=… reason=… n=…` —— key_bad 的 stage×reason 直方图，5 s 一次。
   同一行末尾带 zero_key/real_key（sd_sha_key 的两条出口计数），这是"key 到底是占位零还是真 SHA"的
   直接读数。只读、无参数、无 I/O（除开关文件 2 s 缓存）。 */
static void d3_badwhy_tick(void)
{
    static unsigned long long last;
    static int seeded;
    if (!d3_vf()) return;
    unsigned long long t = d3_now_ms();
    if (seeded && t < last + 5000ull) return;
    last = t; seeded = 1;
    static const char *const stn[5] = { "0", "1", "5", "other", "unread" };
    static const char *const rsn[8] = { "ok", "ptr", "magic", "ctl_len", "code_len", "stage", "hdr", "?" };
    int lines = 0;
    for (int s = 0; s < 5 && lines < 12; s++)
        for (int r = 1; r < 8 && lines < 12; r++)
            if (g_kb_x[s][r]) {
                lines++;
                vk_boot_logf("[m7f] badwhy stage=%s reason=%s n=%llu", stn[s], rsn[r], g_kb_x[s][r]);
            }
    vk_boot_logf("[m7f] badwhy stage=all reason=all n=%llu zero_key=%llu real_key=%llu no_sha_gate=%d sha_ungated=%d",
                 g_kb_reason[1] + g_kb_reason[2] + g_kb_reason[3] + g_kb_reason[4] +
                 g_kb_reason[5] + g_kb_reason[6] + g_kb_reason[7],
                 g_d3_zero_key, g_d3_real_key, d3_vsha() ? 0 : 1, d3_vsha());
}

/* M3v P2：key_bad 分解报告（由 gfx_bridge 的 [p] program totals 之后调用）。 */
void shader_dump_keybad_report(void)
{
    unsigned long long tot = 0;
    for (int i = 0; i < 8; i++) tot += g_kb_reason[i];
    d3_badwhy_tick();   /* M7D3：即使 key_bad=0 也要打（zero_key/real_key 是主读数） */
    /* M42C：key 来源一行读数 —— 真机"draws=0 / skip_noprog 全量"的直接判据就是 zero 是否在涨。
       real>0 且 zero==0 = 真 SHA 生效；zero 在涨 = 又退化成占位零 key（maps 判据或读不动）。 */
    vk_boot_logf("[s] key src: real=%llu zero=%llu ungated=%d maps_gate=%d maps_entries=%u maps_dropped=%u ungated_reads=%llu",
                 g_d3_real_key, g_d3_zero_key, sd_ungated(), d3_vmg(),
                 runtime_maps_entries(), runtime_maps_dropped(), (unsigned long long)runtime_ungated_reads());
    if (!tot && !g_cs_nohdr) return;
    vk_boot_logf("[s] key_bad by stage: s0=%llu s1=%llu s5=%llu other=%llu unreadable=%llu total=%llu",
                 g_kb_stage[0], g_kb_stage[1], g_kb_stage[2], g_kb_stage[3], g_kb_stage[4], tot);
    vk_boot_logf("[s] key_bad by reason: ptr=%llu magic=%llu ctl_len=%llu code_len=%llu stage=%llu hdr=%llu cs_no_header_ok=%llu",
                 g_kb_reason[SD_BAD_PTR], g_kb_reason[SD_BAD_MAGIC], g_kb_reason[SD_BAD_CTL_LEN],
                 g_kb_reason[SD_BAD_CODE_LEN], g_kb_reason[SD_BAD_STAGE], g_kb_reason[SD_BAD_HDR], g_cs_nohdr);
}

/* 单条 shader：校验 → 算键 → 首见落盘。返回 0 = 已处理（落盘或去重命中），-1 = 判为 bad。 */
static int sd_one(const void *ctl, uint64_t code_addr, int idx)
{
    sd_hdr h = {0};
    int valid = sd_validate(ctl, code_addr, &h);
    if (!valid) {
        g_bad++;
        /* 指针本身不可信时不读任何字段（sd_validate 已在碰 ctl 前做过地址闸门，这里只是防日志崩） */
        /* M5l：这里的 raw16 原来也是裸读，闸门同样只有下界（`ctl=0xffffffffffffffff` 能过）——
           既判为 bad，就只在**整段映射**时才读样本，且读也走 runtime_safe_read。 */
        if (g_logged_bad < 8 && ctl && runtime_addr_range_mapped((uintptr_t)ctl, 16)) {
            uint8_t raw[16] = {0};
            if (runtime_safe_read(raw, (uint64_t)(uintptr_t)ctl, sizeof raw)) {
                g_logged_bad++;
                vk_boot_logf("[s] layout BAD idx=%d ctl=%p magic=%08x ctl_len=%u code_len=%u stage=%u hdr=%08x raw16=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                             idx, ctl, h.magic, h.ctl_len, h.code_len, h.stage, h.hdr,
                             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
                             raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
            } else {
                g_logged_bad++;
                vk_boot_logf("[s] layout BAD idx=%d ctl=%p (unreadable, no raw sample) why=%d", idx, ctl, (int)h.why);
            }
        }
        return -1;
    }
    /* P1 闸门证据：前 6 条通过校验的实读值（行号=本文件 sd_validate 的 +0x20/+0x704/+0x714）。 */
    if (g_logged_ok < 6) {
        g_logged_ok++;
        vk_boot_logf("[s] layout ok idx=%d ctl=%p magic=%08x ctl_len=%u code_len=%u stage=%u hdr=%08x",
                     idx, ctl, h.magic, h.ctl_len, h.code_len, h.stage, h.hdr);
    }

    uint8_t d[32];
    {   /* 键：沿用既有口径（此处与 sd_sha_key 等价；保留 d[] 是因为下面既要用 hex 又要用前 8 字节判重） */
        if (h.no_sha) {   /* M5m: valid but whole segment unmapped -> do not touch it (log once) */
            static int n_sha; if (!n_sha) { n_sha = 1; vk_boot_logf("[s] sha skip idx=%d ctl=%p (segment unmapped; criterion said OK)", idx, ctl); }
            return 0;
        }
        CC_SHA256_CTX c;
        CC_SHA256_Init(&c);
        /* M7D3：落盘侧同样改分块安全读（旧版是整段裸 Update，未映射时会真的 SEGV）。 */
        if (!sd_sha_seg(&c, (uint64_t)(uintptr_t)ctl, h.ctl_len) ||
            !sd_sha_seg(&c, code_addr, h.code_len)) {
            static int n_sf; if (!n_sf) { n_sf = 1; vk_boot_logf("[s] sha skip idx=%d ctl=%p (chunked read failed)", idx, ctl); }
            return 0;
        }
        CC_SHA256_Final(d, &c);
    }
    char key[17], path[640], path2[640];
    hex16(d, key);
    uint64_t k;
    memcpy(&k, d, sizeof k);

    /* M92A bit1：语料里没有的键在这里拷进内存暂存区（SetShaders 期读一次；draw 期零 guest 读）。 */
    m92a_maybe_stash(key, (int)h.stage, ctl, code_addr, h.ctl_len, h.code_len);

    g_uniq++;                                   /* 口径：通过校验的不同键数（含超出上限未落盘的） */
    if (sd_seen(k)) return 0;
    if (g_dumped >= sd_max_keys()) {
        if (!g_full) { vk_boot_logf("[s] dump full (keys=%lu) -> counting only", g_dumped); g_full = 1; }
        if ((shader_dump_m92a_bits() & 2) && ++g_m92a_cap_logged <= 8)
            vk_boot_logf("[m92a] corpus dump FULL key=%s stage=%u dumped=%lu cap=%llu -> 丢弃该键",
                         key, h.stage, g_dumped, sd_max_keys());
        g_full++;
        return 0;
    }
    if (g_bytes + h.ctl_len + h.code_len > sd_max_bytes()) {
        if (!g_full) { vk_boot_logf("[s] dump full (bytes=%lu) -> counting only", g_bytes); g_full = 1; }
        if ((shader_dump_m92a_bits() & 2) && ++g_m92a_cap_logged <= 16)
            vk_boot_logf("[m92a] corpus dump FULL key=%s stage=%u bytes=%lu cap=%llu -> 丢弃该键",
                         key, h.stage, g_bytes, sd_max_bytes());
        g_full++;
        return 0;
    }

    snprintf(path, sizeof path, "%s/%s.%u.ctl", g_dir, key, h.stage);
    snprintf(path2, sizeof path2, "%s/%s.%u.code", g_dir, key, h.stage);
    /* M3w P3：跨进程去重。判重表只在本次进程内有效，而语料目录是跨 boot 累积的；
       已有同名文件就只计数（g_exists），不再重写、也不再往 index.txt 追一行——
       否则每次 boot 都把 index.txt 灌一遍重复键，下一轮烘焙会把同一条翻两遍。 */
    if (access(path, F_OK) == 0 && access(path2, F_OK) == 0) { g_exists++; return 0; }

    int e1 = sd_write(path, ctl, h.ctl_len);
    snprintf(path, sizeof path, "%s/%s.%u.code", g_dir, key, h.stage);
    int e2 = sd_write(path, (const void *)(uintptr_t)code_addr, h.code_len);
    if (e1 || e2) {
        g_bad++;
        vk_boot_logf("[s] dump write FAILED key=%s stage=%u e1=%d e2=%d errno=%d", key, h.stage, e1, e2, errno);
        return -1;
    }
    g_dumped++;
    g_bytes += h.ctl_len + h.code_len;
    g_code_sum += h.code_len;
    if (h.code_len > g_code_max) g_code_max = h.code_len;
    g_stage_n[h.stage == 5 ? 2 : (h.stage < 3 ? h.stage : 0)]++;
    vk_boot_logf("[s] shader key=%s stage=%u ctl=%u code=%u", key, h.stage, h.ctl_len, h.code_len);

    snprintf(path, sizeof path, "%s/index.txt", g_dir);
    FILE *ix = fopen(path, "a");
    if (ix) {
        fprintf(ix, "%s %u ctl=%u code=%u magic=%08x hdr=%08x\n", key, h.stage, h.ctl_len, h.code_len, h.magic, h.hdr);
        fclose(ix);
    }
    return 0;
}

/* M7D3：`prog` 只用于探针行（谁在 SetShaders 里算 key）。宽高上限：
   `[m7f] set` 与 `[m7f] key` 各 64 行 —— 够覆盖前 32 次 SetShaders 的两阶段，
   之后静默（只留 badwhy 汇总），不会把 boot 日志刷爆。 */
static unsigned g_d3_set_lines, g_d3_key_lines;
int shader_dump_key_at(const void *sd, int i, uint64_t prog, char out[17])
{
    if (!sd || i < 0) return -1;
    const uint8_t *e = (const uint8_t *)sd + (size_t)i * 16;   /* NVNshaderData = {u64 data; const void *control;} */
    uint64_t a = 0, b = 0;
    /* M5l：描述符数组本身是 guest 指针（NVNshaderData[]），原来两个 rd64 是裸读。 */
    if (!sd_read(&a, (uint64_t)(uintptr_t)(e + 8), 8)) { if (d3_vf() && g_d3_set_lines < 64) { g_d3_set_lines++; vk_boot_logf("[m7f] set ptr=%#llx idx=%d ctl=unreadable result=-1", (unsigned long long)prog, i); } return -1; }
    if (!sd_read(&b, (uint64_t)(uintptr_t)(e + 0), 8)) { if (d3_vf() && g_d3_set_lines < 64) { g_d3_set_lines++; vk_boot_logf("[m7f] set ptr=%#llx idx=%d code=unreadable result=-1", (unsigned long long)prog, i); } return -1; }
    sd_hdr h;
    int st = shader_key_h((const void *)(uintptr_t)a, b, out, &h);
    if (d3_vf()) {
        if (g_d3_set_lines < 64) {
            g_d3_set_lines++;
            vk_boot_logf("[m7f] set ptr=%#llx idx=%d ctl=%p magic=%08x ctl_len=%u code_len=%u stage=%u hdr=%08x no_sha=%d result=%d",
                         (unsigned long long)prog, i, (const void *)(uintptr_t)a, h.magic, h.ctl_len,
                         h.code_len, h.stage, h.hdr, h.no_sha, st);
        }
        if (g_d3_key_lines < 64) {
            g_d3_key_lines++;
            vk_boot_logf("[m7f] key ptr=%#llx idx=%d vs=%s fs=%s ok=%d why=%s no_sha=%d key=%s",
                         (unsigned long long)prog, i, st == 0 ? out : "-", st == 1 ? out : "-",
                         st >= 0, st >= 0 ? "ok" : (h.why == SD_BAD_PTR ? "ptr" : h.why == SD_BAD_MAGIC ? "magic" :
                         h.why == SD_BAD_CTL_LEN ? "ctl_len" : h.why == SD_BAD_CODE_LEN ? "code_len" :
                         h.why == SD_BAD_STAGE ? "stage" : "hdr"), h.no_sha, st >= 0 ? out : "-");
        }
    }
    return st;
}

void shader_dump_program(int n, const void *sd)
{
    if (!sd || n <= 0) return;
    pthread_mutex_lock(&g_mu);
    g_calls++;
    if (!g_init) sd_dir_init();
    static unsigned g_desc_bad;   /* M5l：描述符数组读不动的笔数（前 3 笔留地址样本） */
    for (int i = 0; i < n; i++) {
        const uint8_t *e = (const uint8_t *)sd + (size_t)i * 16;   /* NVNshaderData = {u64 data; const void *control;} */
        uint64_t data = 0, ctl = 0;
        if (!sd_read(&data, (uint64_t)(uintptr_t)e, 8) ||
            !sd_read(&ctl, (uint64_t)(uintptr_t)(e + 8), 8)) {
            g_desc_bad++;
            if (g_desc_bad <= 3)
                vk_boot_logf("[s] desc unreadable sd=%p i=%d/%d (skip; mapped=%d) — M5l safe-read, not a crash",
                             sd, i, n, runtime_addr_mapped((uint64_t)(uintptr_t)e));
            continue;
        }
        sd_one((const void *)(uintptr_t)ctl, data, i);
    }
    if (g_calls % 100 == 0) sd_totals("periodic");
    pthread_mutex_unlock(&g_mu);
}

void shader_dump_report(void)
{
    pthread_mutex_lock(&g_mu);
    if (g_calls) sd_totals("final");
    pthread_mutex_unlock(&g_mu);
    /* M92A：收尾补一行缺失-shader 汇总（knob=0 时不打、不建目录、不碰任何东西）。 */
    if (shader_dump_m92a_bits()) {
        pthread_mutex_lock(&g_mu92);
        m92a_stats("final");
        pthread_mutex_unlock(&g_mu92);
    }
}
