// M48A / 色调修复：**两个缺陷的开关 + 探针**（全部默认 0 = 逐字节旧行为、零 [m48a] 输出）。
//
// 缺陷 A（世界地图层单层 R/B 换位）：
//   `smo_m48a_rb10=1`  —— 只对「被*采样*的 10-bit 目标（NVN 0x3d / VkFormat 64）」的视图做 R↔B 修正。
//                         不动上屏 blit 通道、不动 8-bit 层（马里奥/logo/HUD 都在 8-bit），所以是单层修。
//   `smo_m48a_fmt10=1` —— top1 判别实验（**需重启**，RT 格式无法热切）：把 NVN 0x3d 的映射整体换成
//                         VK_FORMAT_A2R10G10B10_UNORM_PACK32（R 在高位）。若地图不变色 ⇒ 坐实「格式映射不是根因」
//                         （同一个 10-bit 目标上写/读同格式自成闭环）。
//
// 缺陷 B（整屏过曝/白雾）：
//   `smo_m48a_passdump=N` —— 把 N 写到 DM_VK_PASSDUMP，触发「下一次提交逐 pass dump」（M43A 已有机制，本文件只做触发）。
//   `smo_m48a_srgb=1`     —— 判别实验（**需重启**）：sRGB 格式（0x37/0x6D/0x99 的 SRGB 项）按 UNORM 建/用，
//                            用来判「sRGB 双重编码」是否就是白雾来源。
//
// 开关文件与 smo_m45a/smo_m47a 同目录同权限（构造期自种 0666，2 s 重读）。
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern void vk_boot_logf(const char *fmt, ...);

#define M48A_A "/data/storage/el2/base/files/"
#define M48A_B "/data/app/el2/100/base/com.vibecc.smo/files/"

static const char *const g_names[] = {
    "smo_m48a", "smo_m48a_rb10", "smo_m48a_fmt10", "smo_m48a_passdump", "smo_m48a_srgb", "smo_m48a_swzlog",
    "smo_m48a_rbpool",
    /* M50A：三个「进程内 env」旋钮（hdc 设不进进程，只能靠开关文件 → setenv）。全部默认 0 = 关。 */
    "smo_m50a_pdskip", "smo_m50a_trace_pass", "smo_m50a_ubo_pass",
    /* M51A：DM_VK_WATCH 的进程内旋钮（"POOLOFF" 的 offset 十六进制，可带 #count）。把某张图的每次使用（attachment/sampled/copy）
       连同 present+pass 打出来 —— 用来找「1024² shadow map 到底哪个 pass 写它」。默认 0 = 关。 */
    "smo_m50a_watch",
    /* M51A：smo_m50a_sh_pass=1 → DM_VK_SH_PASS：凡是「深度附件是 1024² 或 256²」的 pass（shadow map 候选），逐 draw 打一行
       状态（cull/front/clamp/bias/pofs/深度测试/视口/剪裁/深度范围/深度附件 pool+off），pass 结束再打一行汇总（draw 条数 + 清屏值 + 剪裁 bbox）。 */
    "smo_m50a_sh_pass",
    /* M51A：smo_m50a_pddepth=1 → DM_VK_PASSDUMP_DEPTH：passdump 同时把深度写出来（shadow map 远平面占比要它量化）。 */
    "smo_m50a_pddepth",
    /* M55A：smo_m55a=1 → DM_VK_M55A：别名判定探针（[m55a-img]/[m55a-pass]/[m55a-dump]/[m55a-stat]/[m55a-skip]）。
       默认 0 = 一行不打、一个 dump 不做（一键回退）。DM_VK_M55A_OFF 可改被查的 pool offset（十六进制，默认 228600）。 */
    "smo_m55a",
    /* M56A：smo_m56a → DM_VK_M56A。bit0 = 采样重定向（被采样图本 submission 没被画过时，绑它 ovl[] 里本 submission 已画过的重叠图）；
       bit1 = 探针（1024² 深度图创建 / 目标深度 pass 的 loadOp+清屏值 / 有没有 GPU 写入落进目标 pool 区间）。默认 0 = 零日志零行为变化。 */
    "smo_m56a",
    /* M57A：smo_m57a → DM_VK_M57A。bit0 = 一个 pool 一块 VkDeviceMemory（slab），每张图按自己的 pool offset 绑进去 = 字节级真别名
       （真机"同一段显存"语义，不是 copy）；bit1 = 探针（[m57a-probe] 对齐/尺寸、[m57a-alias] 配对、[m57a-skip]、[m57a-evict]）；
       bit2 = slab 的布局类只按格式匹配（忽略 extent/levels）；bit3 = 跳过这种被别名深度图的整块 clear（保住兄弟图渲染的字节）。默认 0 = 零日志零行为变化。 */
    "smo_m57a",
    /* M59A：smo_m59a → DM_VK_M59A，默认 0 = 零日志零行为变化。bit0(1) = **修**：与 A 共享 slab 字节的 B（pool+0x228600，1024² D32）的
       "整目标 CLEAR" 改成 loadOp=LOAD（保留 A 本帧渲染的字节；只有"本帧 A 尚未渲染"时才允许显式 clear），同时打开 slab（= M57A bit0）；
       bit1(2) = 打开 slab 真别名（= M57A bit0/bit1）；bit2(4) = 保住被别名深度图的字节（跳过 vk_tex_prepare 零填充与整块 render-pass clear）；
       bit3(8) = A→B copy 同步（备用修）；bit4(16) = 采样侧身份日志（被采样的 1024² 深度句柄 vs 按 pool+off 找到的真身）；
       bit5(32) = **一帧四时刻**探针：(i) A 的 caster pass 结束、(ii)(iii) B 第一次/第二次整目标 clear 之后、(iv) 采样 B 的 pass，
       每时刻只用 transfer copy 读回（不走 render-pass 读回），打印 far%/geom%/min/max + VkImage/VkDeviceMemory/memoryOffset/slab。
       基线 = 34（bit1+bit5，只探不修）；修 = 35（bit0+bit1+bit5）。 */
    "smo_m59a"
};
enum { K_MASTER, K_RB10, K_FMT10, K_PD, K_SRGB, K_SWZLOG, K_RBPOOL, K_PDSKIP, K_TRACEPASS, K_UBOPASS, K_WATCH, K_SHPASS, K_PDDEPTH, K_M55A, K_M56A, K_M57A, K_M59A, K_N };

/* 每个开关一份 2 s 缓存：读 B 视图优先，退 A 视图。 */
int m48a_knob(int i)
{
    static int cache[K_N]; static long last[K_N]; static int first = 1, last_ok = 0;
    if (i < 0 || i >= K_N) return 0;
    if (first) { first = 0; for (int k = 0; k < K_N; k++) { cache[k] = 0; last[k] = 0; } }
    long now = (long)time(NULL);
    if (now < last[i] + 2 && (last[i] || cache[i])) return cache[i];
    last[i] = now; cache[i] = 0;
    char p[256];
    for (int v = 0; v < 2; v++) {
        snprintf(p, sizeof p, "%s%s", v ? M48A_A : M48A_B, g_names[i]);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        char b[32]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
        if (n) { b[n] = 0; cache[i] = atoi(b); }
        break;
    }
    if (!last_ok) { last_ok = 1; }   // 读不到文件 = 保持 0（不报，避免每 2 s 刷屏）
    return cache[i];
}

/* 字符串开关（同一 2 s 缓存策略） */
static const char *m48a_knob_str(int i, char *buf, size_t n)
{
    buf[0] = 0;
    char p[256];
    for (int v = 0; v < 2; v++) {
        snprintf(p, sizeof p, "%s%s", v ? M48A_A : M48A_B, g_names[i]);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        size_t k = fread(buf, 1, n - 1, f); fclose(f);
        buf[k] = 0;
        break;
    }
    for (char *q = buf; *q; q++) if (*q == '\n' || *q == '\r') { *q = 0; break; }
    return buf;
}

/* 缺陷 A 定向实验：smo_m48a_rbpool = "POOLHEX+OFFHEX"（只换这张纹理） 或 "f:0x87"（换该 NVN 格式的全部视图）。
   用来把「地图底图那一张」与「logo/标签图集」分开 —— 整层换会把 logo 一起换掉（M47A E1 已证）。 */
static uint64_t g_rbp_pool; static int64_t g_rbp_off; static int g_rbp_fmt;
static long g_rbp_ts; static char g_rbp_raw[64];
int m48a_rb_this(uint64_t pool, int64_t off, int nvn_fmt)
{
    long now = (long)time(NULL);
    if (now >= g_rbp_ts) {
        char b[64]; g_rbp_ts = now;
        m48a_knob_str(K_RBPOOL, b, sizeof b);
        if (strcmp(b, g_rbp_raw)) {
            g_rbp_pool = 0; g_rbp_off = -1; g_rbp_fmt = 0;
            if (!strncmp(b, "f:", 2)) g_rbp_fmt = (int)strtol(b + 2, NULL, 0);
            else { const char *pl = strchr(b, '+'); if (pl) { g_rbp_pool = strtoull(b, NULL, 16); g_rbp_off = strtoll(pl + 1, NULL, 16); } }
            snprintf(g_rbp_raw, sizeof g_rbp_raw, "%s", b);
        }
    }
    if (g_rbp_fmt && g_rbp_fmt == nvn_fmt) return 1;
    if (g_rbp_off >= 0 && g_rbp_pool == pool && g_rbp_off == off) return 1;
    return 0;
}

/* 缺陷 A 单层修：被采样的 10-bit 目标视图上做 R↔B。 */
int m48a_rb10(void) { return m48a_knob(K_RB10) ? 1 : 0; }
/* top1 判别实验：0x3d 映射整体换成 A2R10G10B10（需重启）。 */
int m48a_fmt10(void) { return m48a_knob(K_FMT10) ? 1 : 0; }
/* 缺陷 B 判别实验：sRGB → UNORM 双胞胎（需重启）。 */
int m48a_srgb_off(void) { return m48a_knob(K_SRGB) ? 1 : 0; }
int m48a_swzlog(void) { return m48a_knob(K_SWZLOG) ? 1 : 0; }
int m48a_on(void) { return m48a_knob(K_MASTER) ? 1 : 0; }

/* 采样侧 R/B 修正 + 一行诊断（每种 (nvn fmt, vk fmt) 只打一次）。 */
static int g_rb_n;
static int g_rbt_n;
void m48a_rbtex_note(int nvn_fmt, int vk_fmt, unsigned long long pool, long long off)
{
    if (g_rbt_n >= 8) return;
    g_rbt_n++;
    vk_boot_logf("[m48a] rbpool: sampled view R<->B fixed (nvn 0x%x vkfmt %d) at %#llx+%#llx #%d", nvn_fmt, vk_fmt, pool, off, g_rbt_n);
}
void m48a_rb10_note(int nvn_fmt, int vk_fmt)
{
    if (g_rb_n >= 8) return;
    g_rb_n++;
    vk_boot_logf("[m48a] rb10: sampled 10-bit view R<->B fixed (nvn 0x%x, vkfmt %d) #%d", nvn_fmt, vk_fmt, g_rb_n);
}

/* DM_VK_PASSDUMP 触发：把 smo_m48a_passdump 的值转成一次 dump（写完自己归 0 由外部 echo 控制）。 */
void m48a_poll(void)
{
    static long last; static int armed = 1;
    static long last_tp, last_up = -1; static int up_state = -2;
    long now = (long)time(NULL);
    if (now <= last) return;
    last = now;
    /* M50A ①：smo_m50a_pdskip=N —— passdump 跳过每个提交的前 N 个 pass（绕开 PD_MAX=96，专抓 80-91）。
       必须比 DM_VK_PASSDUMP arm 更早落进 env（读点在 gfx_vk_exec.c vk_pass_dump_poll，arm 时读一次）。 */
    { int sk = m48a_knob(K_PDSKIP); char b[16]; snprintf(b, sizeof b, "%d", sk > 0 ? sk : 0); setenv("DM_VK_PASSDUMP_SKIP", b, 1); }
    /* M50A ②：smo_m50a_trace_pass=N（1 起；= pass N-1）→ DM_VK_TRACE_PASS，每 15 s 再 arm 一次（引擎 trace 完自己置 -1）。 */
    { int tp = m48a_knob(K_TRACEPASS);
      if (tp > 0 && now >= last_tp + 15) { last_tp = now; char b[16]; snprintf(b, sizeof b, "%d", tp - 1); setenv("DM_VK_TRACE_PASS", b, 1);
          vk_boot_logf("[m50a] trace armed: pass %d", tp - 1); } }
    /* M50A ③：smo_m50a_ubo_pass=N（1 起；0=关）→ DM_VK_UBO_LOG_PASS：该 pass 每笔 draw 的 UBO 逐 word float 落日志（默认 600 行）。 */
    /* 值编码 1 起：DM_VK_UBO_LOG_PASS = 值（引擎侧再 -1），这样 0 才是「关」——pass 0 与「关」不再撞码（M50A 第一版踩过）。 */
    { int up = m48a_knob(K_UBOPASS);
      if (up != up_state) { up_state = up; char b[16]; snprintf(b, sizeof b, "%d", up > 0 ? up : 0); setenv("DM_VK_UBO_LOG_PASS", b, 1);
          vk_boot_logf("[m50a] ubo pass log %s: pass %d", up > 0 ? "armed" : "off", up > 0 ? up - 1 : -1); } }
    (void)last_up;
    /* M51A：smo_m50a_shpass=1 → DM_VK_SH_PASS（每次 poll 落 env，引擎每次提交重读）。 */
    { static int sp_state = -1; int sp = m48a_knob(K_SHPASS);
      if (sp != sp_state) { sp_state = sp; char b[8]; snprintf(b, sizeof b, "%d", sp ? 1 : 0); setenv("DM_VK_SH_PASS", b, 1);
          vk_boot_logf("[m51a] shpass %s (DM_VK_SH_PASS=%s)", sp ? "armed" : "off", b); } }
    /* M51A：smo_m50a_watch=OFFSET_HEX[#count] → DM_VK_WATCH（引擎读点 gfx_vk_exec.c:180-182，每次提交重读；空/0 = 关）。 */
    { char wb[41]; m48a_knob_str(K_WATCH, wb, sizeof wb);
      static char last_w[41]; static int first_w = 1;
      if (first_w || strcmp(wb, last_w)) { first_w = 0; snprintf(last_w, sizeof last_w, "%s", wb);
          setenv("DM_VK_WATCH", wb, 1);
          vk_boot_logf("[m51a] watch %s: DM_VK_WATCH=[%s]", wb[0] && strcmp(wb, "0") ? "armed" : "off", wb); } }
    /* M51A：smo_m50a_sh_pass=1 → DM_VK_SH_PASS（shadow 候选 pass 的逐 draw 状态表）；smo_m50a_pddepth=1 → DM_VK_PASSDUMP_DEPTH。 */
    { static int sh_state = -2; int sh = m48a_knob(K_SHPASS);
      if (sh != sh_state) { sh_state = sh; char b[16]; snprintf(b, sizeof b, "%d", sh > 0 ? 1 : 0); setenv("DM_VK_SH_PASS", b, 1);
          vk_boot_logf("[m51a] sh_pass %s (DM_VK_SH_PASS=%s)", sh > 0 ? "armed" : "off", b); } }
    { static int dd_state = -2; int dd = m48a_knob(K_PDDEPTH);
      if (dd != dd_state) { dd_state = dd; char b[16]; snprintf(b, sizeof b, "%d", dd > 0 ? 1 : 0); setenv("DM_VK_PASSDUMP_DEPTH", b, 1);
          vk_boot_logf("[m51a] passdump depth %s", dd > 0 ? "on" : "off"); } }
    /* M55A：smo_m55a=1 → DM_VK_M55A（别名判定探针，一键开关，默认 0 = 旧行为）。 */
    { static int m55_state = -2; int m55 = m48a_knob(K_M55A);
      if (m55 != m55_state) { m55_state = m55; char b[16]; snprintf(b, sizeof b, "%d", m55 > 0 ? 1 : 0); setenv("DM_VK_M55A", b, 1);
          vk_boot_logf("[m55a] probe %s (DM_VK_M55A=%s)", m55 > 0 ? "on" : "off", b); } }
    /* M56A：smo_m56a → DM_VK_M56A（bit0 重定向 / bit1 探针；默认 0 = 零日志、零行为变化）。 */
    { static int m56_state = -2; int m56 = m48a_knob(K_M56A);
      if (m56 != m56_state) { m56_state = m56; char b[16]; snprintf(b, sizeof b, "%d", m56 > 0 ? m56 : 0); setenv("DM_VK_M56A", b, 1);
          vk_boot_logf("[m56a] knob %s (DM_VK_M56A=%s: retarget %s, probes %s)", m56 > 0 ? "on" : "off", b,
                       (m56 & 1) ? "on" : "off", (m56 & 2) ? "on" : "off"); } }
    /* M57A：smo_m57a → DM_VK_M57A（bit0 slab 真别名 / bit1 探针 / bit2 松弛布局键 / bit3 跳过被别名深度图的 clear；默认 0 = 零日志、零行为变化）。 */
    { static int m57_state = -2; int m57 = m48a_knob(K_M57A);
      if (m57 != m57_state) { m57_state = m57; char b[16]; snprintf(b, sizeof b, "%d", m57 > 0 ? m57 : 0); setenv("DM_VK_M57A", b, 1);
          vk_boot_logf("[m57a] knob %s (DM_VK_M57A=%s: alias %s, probes %s, relax %s, clear keep %s)", m57 > 0 ? "on" : "off", b,
                       (m57 & 1) ? "on" : "off", (m57 & 2) ? "on" : "off", (m57 & 4) ? "on" : "off", (m57 & 8) ? "on" : "off"); } }
    /* M59A：smo_m59a → DM_VK_M59A（bit0 修 CLEAR→LOAD / bit1 slab 真别名 / bit2 保住字节 / bit3 A→B copy / bit4 采样侧身份 / bit5 一帧四时刻；
       默认 0 = 零日志、零行为变化）。 */
    { static int m59_state = -2; int m59 = m48a_knob(K_M59A);
      if (m59 != m59_state) { m59_state = m59; char b[16]; snprintf(b, sizeof b, "%d", m59 > 0 ? m59 : 0); setenv("DM_VK_M59A", b, 1);
          vk_boot_logf("[m59a] knob %s (DM_VK_M59A=%s: fix-clear %s, alias %s, keep-bytes %s, copy-sync %s, sample-id %s, four-moments %s)", m59 > 0 ? "on" : "off", b,
                       (m59 & 1) ? "on" : "off", (m59 & 2) ? "on" : "off", (m59 & 4) ? "on" : "off", (m59 & 8) ? "on" : "off", (m59 & 16) ? "on" : "off", (m59 & 32) ? "on" : "off"); } }
    int v = m48a_knob(K_PD);    if (v <= 0) { if (!armed) { armed = 1; } return; }
    if (!armed) return;
    armed = 0;
    char n[16]; snprintf(n, sizeof n, "%d", v);
    setenv("DM_VK_PASSDUMP", n, 1);
    vk_boot_logf("[m48a] passdump armed: DM_VK_PASSDUMP=%s", n);
}

/* 种 0666 开关文件（el2 沙箱里 hdc 只能覆盖、不能新建）：A/B 两个视图都种。
   由 m45a_switch.c 的构造期调用（M45A/M47A 已证明那条种法可用；本文件自己的 constructor 实测没生效）。 */
void m48a_seed_files(void)
{
    char p[256];
    for (int i = 0; i < K_N; i++) {
        for (int v = 0; v < 2; v++) {
            snprintf(p, sizeof p, "%s%s", v ? M48A_A : M48A_B, g_names[i]);
            if (access(p, F_OK) == 0) { chmod(p, 0666); continue; }
            int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0666);
            if (fd >= 0) { ssize_t w = write(fd, "0\n", 2); (void)w; close(fd); chmod(p, 0666); }
        }
    }
}
__attribute__((constructor(160))) static void m48a_seed(void) { m48a_seed_files(); }
