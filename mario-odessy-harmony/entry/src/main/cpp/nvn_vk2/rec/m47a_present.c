// M47A / S4-lite：**把 nx2 回放真正画出来的合成 RT 接到我们已有的 offscreen 上屏链**。
//
// 背景（M46A 已证）：smo_m45a=1 时 nx2 后端真的在渲染（pipes 946 / issued 207297 / 44/54 pass 非纯值，
// pass_53 = 奥德赛标题画面本体），但 S4 present 仍是空桩（rec/m43a_s4_stubs.c 的 mtl_present 返回 0），
// 所以屏幕全黑。本文件不搬 nx2 的 gfx_vk_present.c（1103 行、绑窗口/交换链/帧调步），只做「最小上屏」：
//
//   ① 每次提交开始时清空候选（m47a_frame_begin）；
//   ② 每个 pass 结束时（gfx_vk_exec.c pass_dump 顶部）记下候选 RT（m47a_pass_note）：
//        - ext == 窗口尺寸（nvnWindowInitialize 里学到，1600x900）
//        - 本 pass 有 draw（g_pass_draws > 0）⇒ 「本帧被写过」
//        - 颜色格式可读（RGBA8/BGRA8/SRGB/A2B10G10R10/RGBA16F）
//        - 优先级 strong：该 RT 的 (pool,off) == 之前某次 nvnQueuePresentTexture 递上来的交换链纹理
//          （标题画面最后一 pass 直接画进交换链纹理；命中它 = 真的 present 源）
//          否则 fallback：本提交里**最后一个**窗口尺寸且被写过的 pass（M46A 实测 = 合成目标）
//   ③ 提交结束前（hle_nvn.c submit_now，仍在 recording 中）把选中的 vkimg_t 拷进 host-visible ring buffer，
//      随本提交一起上 GPU；fence = mtl_last_fence()+1（mtl_sub_commit 即将分配的号）；
//   ④ 下一次 nvnQueuePresentTexture（游戏/渲染线程，与 S4 原版同一线程）poll fence：
//        未完成 → 跳过（屏幕保留上一帧，fail-closed）；完成 → CPU 转 RGBA8、算 mean/纯色判据，
//        纯灰/空 → 保留上一帧；否则 nvn_win_present_rgba(win, rgba, w, h, stride) —— 就是 M42F/M42G
//        的 win_fill.c offscreen 上屏链（RequestBuffer→等比缩放→Flush）。
//
// 开关 smo_m47a（B 视图优先，2 s 重读；**缺省 0 = 逐字节旧行为、零 [m47a] 输出**），构造期自种 0666 文件。
// 每 10 s 一行：[m47a] pres t=%lds src=pass%u slot=%d ext=%dx%d mean=%.0f/%.0f/%.0f hit=%u skip_keep=%lu
//
// 红偏定性用的两个只读探针（同样受 smo_m47a 门控、关态零输出）：
//   · clear 值：m47a_clear_note（gfx_vk_exec.c clear_color 调用）—— 判「某 pass 的 clear 就是红」；
//   · BC 路径：开态首行打 device BC 支持 + nx2 的 CPU 解码条件（f->dec 只在设备不支持 BC 时才置位），
//     判「=1 时走的是 nx2 native BC 还是我方 M42E nvn_bc_decode」。
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gfx_vk.h"   // rec shim → ../gfx_vk.h：V / vkimg_t / vk_tex / vk_cmd / vk_img_barrier_all / mtl_last_fence

extern void vk_boot_logf(const char *fmt, ...);
extern int mtl_wait(uint64_t fence, uint64_t timeout_ns);   // abi/nvn_mtl.h（gfx_vk.h 已带）

#define M47A_SW_A "/data/storage/el2/base/files/smo_m47a"
#define M47A_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m47a"

/* ---------------- 开关（2 s 重读；缺省 0） ---------------- */
int m47a_on(void)
{
    static int cached = -1; static long last;
    long now = (long)time(NULL);
    if (cached >= 0 && now < last + 2) return cached;
    last = now; cached = 0;
    { const char *paths[2] = { M47A_SW_B, M47A_SW_A };
      for (int i = 0; i < 2; i++) { FILE *f = fopen(paths[i], "r");
          if (f) { char b[16]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
                   if (n > 0) { b[n] = 0; cached = atoi(b); } break; } } }
    return cached & 1;
}

/* ---------------- 状态 ---------------- */
typedef struct {
    int pass; uint32_t w, h; VkFormat fmt; vkimg_t *im; int strong;
} cand_t;

static uint32_t g_win_w, g_win_h;          /* nvnWindowInitialize 学到的窗口尺寸（交换链纹理 ext） */
static int g_inited;
static long g_t0;
static cand_t g_cand_strong, g_cand_fb;
static int g_have_strong, g_have_fb, g_strong_this_sub;

/* 之前递上来的交换链纹理身份：(pool, off)。strong 候选 = 本 pass 的 RT 命中这个集合。 */
typedef struct { uint64_t pool; int64_t off; } ptex_t;
static ptex_t g_ptex[8]; static int g_nptex;

/* 上屏统计 */
static unsigned long g_enq, g_pres, g_skip_none, g_skip_pure, g_skip_full, g_zero_slot;
static unsigned long g_sub_nocand;
static long g_last_log;

static int m47a_supported_fmt(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 1;
    default: return 0;
    }
}
static int m47a_bpp(VkFormat f) { return f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4; }

static void m47a_init_once(void)
{
    if (g_inited) return;
    g_inited = 1; g_t0 = (long)time(NULL);
    vk_boot_logf("[m47a] on: t0=%ld device_bc=%d (CPU BC decode = gfx_vk_tex.c bc_decode, only when the device lacks BC; "
                 "our M42E nvn_bc_decode belongs to the old nvn_vk.c path) win=%ux%u",
                 g_t0, V.has_bc, g_win_w, g_win_h);
}

/* nvnWindowInitialize：交换链纹理 ext = 游戏窗口尺寸（就是合成目标的尺寸）。 */
void m47a_window_ext(uint32_t w, uint32_t h)
{
    if (!m47a_on() || !w || !h) return;
    if (g_win_w != w || g_win_h != h) {
        g_win_w = w; g_win_h = h;
        vk_boot_logf("[m47a] window ext %ux%u (from nvnWindowInitialize swap-chain texture)", w, h);
    }
}

/* 红偏探针 ①a'：smo_m47a_trace = N（pass 号），每 30 s 重新 arm 一次 DM_VK_TRACE_PASS=N，
   这样能在标题/关卡等不同时刻各抓一帧 pass N 的逐 draw 日志（纹理 nvn format + image fmt + pool/off + blend）。 */
static long g_last_trace;
static void m47a_trace_maybe(void)
{
    static long last;
    long now = (long)time(NULL);
    if (now < last + 2) return;
    last = now;
    char b[16] = { 0 };
    { const char *paths[2] = { "/data/app/el2/100/base/com.vibecc.smo/files/smo_m47a_trace", "/data/storage/el2/base/files/smo_m47a_trace" };
      for (int i = 0; i < 2; i++) { FILE *f = fopen(paths[i], "r"); if (!f) continue;
          size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0; break; } }
    int enc = atoi(b);   /* 编码：1 = pass 0，12 = pass 11；0 = 不开 */
    if (enc <= 0 || now < g_last_trace + 30) return;
    g_last_trace = now;
    char pv[8]; snprintf(pv, sizeof pv, "%d", enc - 1);
    setenv("DM_VK_TRACE_PASS", pv, 1);
    vk_boot_logf("[m47a] trace armed: pass %d at t=%lds", enc - 1, now - g_t0);
}

static int m47a_save_count(void)
{
    static int cached = -1; static long last;
    long now = (long)time(NULL);
    if (cached >= 0 && now < last + 2) return cached;
    last = now; cached = 0;
    { const char *paths[2] = { "/data/app/el2/100/base/com.vibecc.smo/files/smo_m47a_save", "/data/storage/el2/base/files/smo_m47a_save" };
      for (int i = 0; i < 2; i++) { FILE *f = fopen(paths[i], "r"); if (!f) continue;
          char b[16]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
          if (n > 0) { b[n] = 0; int v = atoi(b); if (v > 0 && v <= 16) cached = v; } break; } }
    return cached;
}

/* 红偏探针 ①b：smo_m47a_texdump = "POOLHEX+OFFHEX"（trace 打出来的 texture 身份），2 s 轮询；
   出现即把 DM_VK_TEXDUMP / DM_VK_PASSDUMP_PASS=0 设进进程，t>=90 s 由 m45a ticker 正式开 dump，
   pass 0 采到该纹理时 texdump_flush 把它单独写成 pass_*.ppm（一帧一次）。 */
static char g_td_applied[80];
static void m47a_texdump_maybe(void)
{
    static long last;
    long now = (long)time(NULL);
    if (now < last + 2) return;
    last = now;
    char b[80] = { 0 };
    { const char *paths[2] = { "/data/app/el2/100/base/com.vibecc.smo/files/smo_m47a_texdump", "/data/storage/el2/base/files/smo_m47a_texdump" };
      for (int i = 0; i < 2; i++) { FILE *f = fopen(paths[i], "r"); if (!f) continue;
          size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); while (n && (b[n - 1] == '\n' || b[n - 1] == '\r' || b[n - 1] == ' ')) b[--n] = 0; break; } }
    if (!b[0] || !strcmp(b, "0") || !strcmp(b, g_td_applied)) return;
    snprintf(g_td_applied, sizeof g_td_applied, "%s", b);
    setenv("DM_VK_TEXDUMP", b, 1);
    setenv("DM_VK_PASSDUMP", "1", 1);
    setenv("DM_VK_PASSDUMP_PASS", "0", 1);
    setenv("DM_VK_PASSDUMP_FRAMES", "1", 1);
    vk_boot_logf("[m47a] texdump armed: %s (pass 0 only)", b);
}

/* ---------------- 候选 RT（pass 结束） ---------------- */
void m47a_frame_begin(void)
{
    /* 一个 present 周期可能包含多个 nvnQueueSubmitCommands：present 用的那一 pass 常是提交靠前的 pass 1，
       后面的提交继续渲染下一帧。strong 候选因此跨提交保留，只在 present 消费后由 submit_end 清标记。 */
    g_have_fb = 0;
}

void m47a_pass_note(const tex_t *t, vkimg_t *im, int pass, int draws)
{
    (void)t;
    if (!m47a_on()) return;
    m47a_texdump_maybe();
    m47a_trace_maybe();
    if (!im) return;
    if (!(im->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) return;
    if (!m47a_supported_fmt(im->fmt)) return;
    if (!g_win_w || im->w != g_win_w || im->h != g_win_h) return;   /* ext == 窗口尺寸 */
    if (draws <= 0) return;                                          /* 本帧被写过（有 draw） */
    int strong = 0;
    for (int i = 0; i < g_nptex; i++) if (g_ptex[i].pool == im->pool && g_ptex[i].off == im->off) { strong = 1; break; }
    cand_t c = { pass, im->w, im->h, im->fmt, im, strong };
    if (strong) { g_cand_strong = c; g_have_strong = 1; g_strong_this_sub = 1; }
    else { g_cand_fb = c; g_have_fb = 1; }
    { static int n; static long last; long now = (long)time(NULL);   /* 诊断：前 48 条 + 每 5 s 一条候选身份 */
      if (n < 48 || now >= last + 5) { if (n < 48) n++; last = now;
          vk_boot_logf("[m47a] cand pass=%d %ux%u fmt=%d draws=%d pool=%#llx+%#llx strong=%d",
                       pass, im->w, im->h, im->fmt, draws, (unsigned long long)im->pool, (unsigned long long)(uint64_t)im->off, strong); } }
}

/* ---------------- 回读 ring（3 槽，host-visible） ---------------- */
enum { M47A_RS = 3 };
typedef struct {
    VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize cap;
    uint32_t w, h; int bpp; VkFormat fmt; int pending, ready;
    uint64_t fence, seq; int pass, strong;
} slot_t;
static slot_t g_slot[M47A_RS];
static int g_rr; static uint64_t g_seq;

static int slot_ensure(slot_t *s, uint32_t w, uint32_t h, int bpp)
{
    VkDeviceSize n = (VkDeviceSize)w * h * (VkDeviceSize)bpp;
    if (s->b && s->cap >= n) return 1;
    /* 尺寸只会稳定在窗口 ext（1600x900）；真变大时旧 buffer 不 unmap 也不销毁（nx2 的 VK_DEV_FNS 没导出 vkUnmapMemory），
       最多泄漏一两块 5.7 MB 的映射，换来绝不碰未定义用法。 */
    s->cap = 0; s->p = NULL; s->b = VK_NULL_HANDLE; s->m = VK_NULL_HANDLE;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer(V.dev, &bi, NULL, &s->b) != VK_SUCCESS) { s->b = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(V.dev, s->b, &rq);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size,
        vk_mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0) };
    if (vkAllocateMemory(V.dev, &ai, NULL, &s->m) != VK_SUCCESS) { vkDestroyBuffer(V.dev, s->b, NULL); s->b = VK_NULL_HANDLE; s->m = VK_NULL_HANDLE; return 0; }
    if (vkBindBufferMemory(V.dev, s->b, s->m, 0) != VK_SUCCESS ||
        vkMapMemory(V.dev, s->m, 0, VK_WHOLE_SIZE, 0, &s->p) != VK_SUCCESS) {
        s->p = NULL; vkFreeMemory(V.dev, s->m, NULL); vkDestroyBuffer(V.dev, s->b, NULL);
        s->b = VK_NULL_HANDLE; s->m = VK_NULL_HANDLE; return 0;
    }
    s->cap = n; s->w = w; s->h = h; s->bpp = bpp;
    return 1;
}

static int slot_acquire(void)
{
    for (int k = 0; k < M47A_RS; k++) {
        int i = (g_rr + k) % M47A_RS;
        slot_t *s = &g_slot[i];
        if (s->pending) {
            if (!mtl_wait(s->fence, 0)) continue;   /* GPU 还没到：本帧跳过这一槽 */
            s->pending = 0; s->ready = 1;
        }
        if (s->ready) continue;                     /* 已回读、还没上屏：别覆盖 */
        g_rr = (i + 1) % M47A_RS;
        return i;
    }
    return -1;
}

/* ---------------- 提交结束：把候选 RT 拷进 ring ---------------- */
void m47a_submit_end(void)
{
    if (!m47a_on()) return;
    if (!vk_sub_open()) return;
    m47a_init_once();
    cand_t *c = (g_strong_this_sub && g_have_strong) ? &g_cand_strong : (g_have_fb ? &g_cand_fb : NULL);
    g_sub_nocand += c ? 0 : 1;
    if (!c) return;
    int si = slot_acquire();
    if (si < 0) { g_skip_full++; return; }
    slot_t *s = &g_slot[si];
    int bpp = m47a_bpp(c->fmt);
    if (!slot_ensure(s, c->w, c->h, bpp)) return;
    VkCommandBuffer cb = vk_cmd();
    vk_img_barrier_all(cb);
    VkBufferImageCopy rg = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { c->w, c->h, 1 } };
    vkCmdCopyImageToBuffer(cb, c->im->img, VK_IMAGE_LAYOUT_GENERAL, s->b, 1, &rg);
    vk_img_barrier_all(cb);
    s->w = c->w; s->h = c->h; s->bpp = bpp; s->fmt = c->fmt; s->pass = c->pass; s->strong = c->strong;
    s->pending = 1; s->ready = 0; s->fence = mtl_last_fence() + 1; s->seq = ++g_seq;
    g_strong_this_sub = 0;   /* 本次已入队；下一次 strong 候选再入队 */
    g_enq++;
}

/* ---------------- 呈现：poll → CPU 转 RGBA8 → win_fill ---------------- */
extern int nvn_win_present_rgba(void *win, const uint8_t *src, int sw, int sh, int src_stride) __attribute__((weak));
extern void *nvn_vk_native_window(void) __attribute__((weak));

static inline float m47a_h2f(uint16_t h)
{
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, f;
    if (!e) { if (!m) f = s; else { e = 1; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; f = s | ((e + 112) << 23) | (m << 13); } }
    else if (e == 31) f = s | 0x7f800000 | (m << 13);
    else f = s | ((e + 112) << 23) | (m << 13);
    float r; memcpy(&r, &f, 4); return r;
}

static void slot_convert(const slot_t *s, uint8_t *dst, double *mr, double *mg, double *mb, int *pure)
{
    const uint32_t w = s->w, h = s->h;
    const int bgr = (s->fmt == VK_FORMAT_B8G8R8A8_UNORM || s->fmt == VK_FORMAT_B8G8R8A8_SRGB);
    double sr = 0, sg = 0, sb = 0; long ns = 0;
    int lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)s->p + (size_t)y * w * (size_t)s->bpp;
        uint8_t *d = dst + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++) {
            int r, g, b;
            if (s->bpp == 8) {
                const uint16_t *p = (const uint16_t *)row + (size_t)x * 4;
                float rr = m47a_h2f(p[0]), gg = m47a_h2f(p[1]), bb = m47a_h2f(p[2]);
                r = (int)(rr <= 0 ? 0 : rr >= 1 ? 255 : rr * 255 + .5f);
                g = (int)(gg <= 0 ? 0 : gg >= 1 ? 255 : gg * 255 + .5f);
                b = (int)(bb <= 0 ? 0 : bb >= 1 ? 255 : bb * 255 + .5f);
            } else if (s->fmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || s->fmt == VK_FORMAT_A2R10G10B10_UNORM_PACK32) {
                uint32_t v = ((const uint32_t *)row)[x];
                if (s->fmt == VK_FORMAT_A2R10G10B10_UNORM_PACK32) { r = (int)(((v >> 20) & 0x3ff) >> 2); g = (int)(((v >> 10) & 0x3ff) >> 2); b = (int)((v & 0x3ff) >> 2); }
                else { r = (int)((v & 0x3ff) >> 2); g = (int)(((v >> 10) & 0x3ff) >> 2); b = (int)(((v >> 20) & 0x3ff) >> 2); }
            } else {
                const uint8_t *p = row + (size_t)x * 4;
                r = p[bgr ? 2 : 0]; g = p[1]; b = p[bgr ? 0 : 2];
            }
            d[4 * x + 0] = (uint8_t)r; d[4 * x + 1] = (uint8_t)g; d[4 * x + 2] = (uint8_t)b; d[4 * x + 3] = 255;
            if (((x | y) & 7) == 0) {   /* 1/64 采样：mean + 通道极差（纯色判据） */
                sr += r; sg += g; sb += b; ns++;
                if (r < lo[0]) lo[0] = r; if (r > hi[0]) hi[0] = r;
                if (g < lo[1]) lo[1] = g; if (g > hi[1]) hi[1] = g;
                if (b < lo[2]) lo[2] = b; if (b > hi[2]) hi[2] = b;
            }
        }
    }
    *mr = ns ? sr / ns : 0; *mg = ns ? sg / ns : 0; *mb = ns ? sb / ns : 0;
    int spread = (hi[0] - lo[0]) + (hi[1] - lo[1]) + (hi[2] - lo[2]);
    int gray128 = *mr > 122 && *mr < 134 && *mg > 122 && *mg < 134 && *mb > 122 && *mb < 134;
    *pure = (spread < 9) || (gray128 && spread < 24);
}

void m47a_present_texture(const tex_t *t, int idx)
{
    (void)idx;
    if (!m47a_on()) return;
    m47a_init_once();
    /* 记下交换链纹理身份：下次 strong 候选靠它认「真的 present 源」 */
    if (t && t->magic == MAGIC('T', 'E', 'X', 0) && t->pool) {
        uint64_t pool = (uint64_t)(uintptr_t)t->pool; int64_t off = t->off; int known = 0;
        for (int i = 0; i < g_nptex; i++) if (g_ptex[i].pool == pool && g_ptex[i].off == off) { known = 1; break; }
        if (!known && g_nptex < 8) { g_ptex[g_nptex].pool = pool; g_ptex[g_nptex].off = off; g_nptex++; }
        if (!known) vk_boot_logf("[m47a] present texture identity #%d pool=%#llx+%#llx %ux%u fmt=%#x (new)", g_nptex, (unsigned long long)pool, (unsigned long long)(uint64_t)off, t->d.width, t->d.height, t->d.format);
        if (!g_win_w) { g_win_w = t->d.width; g_win_h = t->d.height; }
    }
    /* poll + 选最新的 ready 槽 */
    int best = -1, beststrong = -1; uint64_t bestseq = 0;
    for (int i = 0; i < M47A_RS; i++) {
        slot_t *s = &g_slot[i];
        if (s->pending && mtl_wait(s->fence, 0)) { s->pending = 0; s->ready = 1; }
        if (!s->ready) continue;
        /* strong（真的 present 源）优先；同等级取最新 seq。 */
        if (s->strong > beststrong || (s->strong == beststrong && s->seq > bestseq)) { best = i; beststrong = s->strong; bestseq = s->seq; }
    }
    if (best < 0) { g_skip_none++; return; }
    slot_t *s = &g_slot[best];
    uint8_t *rgba = (uint8_t *)malloc((size_t)s->w * s->h * 4);
    if (!rgba) { s->ready = 0; return; }
    double mr, mg, mb; int pure = 0;
    slot_convert(s, rgba, &mr, &mg, &mb, &pure);
    { static int nsaved; int want = m47a_save_count();   /* 判据2：存连续 presented 帧，供逐对 diff8 */
      if (want > 0 && nsaved < want && !pure && (long)time(NULL) - g_t0 >= 30) {
          const char *root = getenv("DM_ROOT"); char path[512];
          snprintf(path, sizeof path, "%s/m47a_pres_%02d.ppm", root ? root : ".", nsaved);
          FILE *f = fopen(path, "wb");
          if (f) { fprintf(f, "P6\n%u %u\n255\n", s->w, s->h);
              for (uint32_t y = 0; y < s->h; y++) { const uint8_t *row = rgba + (size_t)y * s->w * 4;
                  for (uint32_t x = 0; x < s->w; x++) { fputc(row[4 * x], f); fputc(row[4 * x + 1], f); fputc(row[4 * x + 2], f); } }
              fclose(f); nsaved++;
              vk_boot_logf("[m47a] saved frame %d -> %s", nsaved, path); }
      } }
    (void)pure;
    int ok = 0;
    if (pure) { g_skip_pure++; }
    else if (!nvn_win_present_rgba || !nvn_vk_native_window) { g_zero_slot++; }
    else {
        void *win = nvn_vk_native_window();
        if (!win) g_zero_slot++;
        else { ok = nvn_win_present_rgba(win, rgba, (int)s->w, (int)s->h, (int)s->w * 4); if (ok) g_pres++; }
    }
    s->ready = 0;
    free(rgba);
    long now = (long)time(NULL);
    if (now >= g_last_log + 10) {
        g_last_log = now;
        vk_boot_logf("[m47a] pres t=%lds src=pass%d slot=%d ext=%ux%u mean=%.0f/%.0f/%.0f hit=%d skip_keep=%lu "
                     "(enq=%lu pres=%lu pure=%lu none=%lu full=%lu nocand=%lu winfail=%lu)",
                     now - g_t0, s->pass, best, s->w, s->h, mr, mg, mb, s->strong, g_skip_pure,
                     g_enq, g_pres, g_skip_pure, g_skip_none, g_skip_full, g_sub_nocand, g_zero_slot);
    }
}

/* ---------------- 红偏探针 ①：clear 值 ---------------- */
void m47a_clear_note(int pass, uint32_t w, uint32_t h, int fmt, const float rgba[4], int mask)
{
    static int n, nr;
    if (!m47a_on()) return;
    int red = rgba[0] > 0.5f && rgba[1] < 0.35f && rgba[2] < 0.35f && rgba[0] > rgba[1] + 0.3f;
    if (n < 32 || (red && nr < 16)) {
        if (n < 32) n++;
        if (red && nr < 16) nr++;
        vk_boot_logf("[m47a] clear pass=%d %ux%u fmt=%d rgba=%.3f,%.3f,%.3f,%.3f mask=%#x%s",
                     pass, w, h, fmt, rgba[0], rgba[1], rgba[2], rgba[3], mask, red ? "  <-- RED" : "");
    }
}

/* ---------------- 构造期：种 0666 开关文件（hdc 不能在 app 目录新建） ---------------- */
__attribute__((constructor)) static void m47a_seed(void)
{
    const char *p[8] = { M47A_SW_A, M47A_SW_B, M47A_SW_A "_trace", M47A_SW_B "_trace", M47A_SW_A "_texdump", M47A_SW_B "_texdump", M47A_SW_A "_save", M47A_SW_B "_save" };
    for (int i = 0; i < 8; i++) {
        if (access(p[i], F_OK) == 0) { chmod(p[i], 0666); continue; }
        int fd = open(p[i], O_CREAT | O_WRONLY | O_TRUNC, 0666);
        if (fd >= 0) { ssize_t w = write(fd, "0\n", 2); (void)w; close(fd); chmod(p[i], 0666); }
    }
    /* 红偏探针 ①a：smo_m47a_trace=1 ⇒ 打开 nx2 的 pass 0 逐 draw 追踪（纹理 nvn format / image fmt / pool+off / blend）。
       只在开态生效；关态零副作用。 */
    if (m47a_on()) {
        const char *paths[2] = { M47A_SW_B "_trace", M47A_SW_A "_trace" };
        for (int i = 0; i < 2; i++) {
            FILE *f = fopen(paths[i], "r");
            if (!f) continue;
            char b[16]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
            if (n > 0) { b[n] = 0; if (atoi(b) == 1) setenv("DM_VK_TRACE_PASS", "0", 1); }
            break;
        }
    }
}
