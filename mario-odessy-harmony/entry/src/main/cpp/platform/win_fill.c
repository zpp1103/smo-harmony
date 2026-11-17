/* CPU present fallback — fill the XComponent native window directly, no GPU involved.
 *
 * Why this exists: the OHOS emulator ships a Vulkan loader and advertises the full instance extension set
 * (VK_KHR_surface / VK_OHOS_surface / VK_KHR_portability_enumeration) but has no ICD, so vkCreateInstance returns
 * VK_ERROR_INCOMPATIBLE_DRIVER (-9) and the whole NVN->Vulkan present chain degrades to a counter. G1's criterion is
 * "real pixels on screen", not "pixels produced by Vulkan", so the present path falls back to writing the clear colour
 * into the window buffer on the CPU. This also decouples the display chain from the renderer: when NVN command replay
 * lands, only the source pixels change, not this hand-off.
 *
 * Colour alternates every 16 presents (caller's choice) so the screen visibly changes — a constant frame cannot be told
 * apart from a frozen one in a screenshot. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <native_window/external_window.h>
#include <native_window/buffer_handle.h>

/* 文件日志（nvn_vk.c 提供）。几何行必须进 boot 日志：stderr/hilog 首波会被流控丢。 */
extern void vk_boot_logf(const char *fmt, ...);

/* ================= M104A（2026-11-13）：可见面写者台账 ==================================
   问题（M103A 留下的最锐事实）：present 前段 `[p] sig src=tex staging mean=0/0/0`，屏幕却有
   16% 纯黄 ⇒ 「可见帧不是我们这条拷贝路径产出的」这句话必须被证或证伪。
   事实链：**本文件是唯一直接写 XComponent 窗口缓冲区的模块**（两条通道：
     ① nvn_win_present_rgba —— 真图 blit（staging→窗口），
     ② nvn_win_fill —— 纯色 CPU 填色兜底。
   所以「谁写可见面」的计数器必须落在这里。通道号：
     0=未知 1=主 present 线程（nvn_vk_present_offscreen）2=上屏泵（m68b）3=m67b 兜底 4=init 一次性
   统计口径（与截图判据同口径）：目标面 = 窗口 buffer 全幅；黄 = R>=200 && G>=150 && B<=60
   （M103A 交付图的量化口径）；nz = 非零像素占比。采样 64x36 网格（2304 点，几十 us，只读）。
   =0 关态不存在：本块是纯观测设施，不读开关、不改一个像素，热路径只多一次网格采样。 */
#include <stdatomic.h>
#define M104A_NCH 6
static _Atomic int g_m104a_tag;                            /* 当前写者（调用方在 blit 前置） */
void nvn_win_m104a_tag(int t) { atomic_store_explicit(&g_m104a_tag, t, memory_order_relaxed); }
static double m104a_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
/* 一次采样：均值/最大值/非零占比/黄色占比（‰，整数避免浮点误差与格式串风险）。 */
typedef struct { unsigned mr, mg, mb, mxr, mxg, mxb, nz, yel, ns; } m104a_sig_t;
static void m104a_sig(m104a_sig_t *s, const uint8_t *base, int stride, int w, int h)
{
    memset(s, 0, sizeof *s);
    if (!base || stride <= 0 || w <= 0 || h <= 0) return;
    const int sx = (w > 64) ? (w / 64) : 1;
    const int sy = (h > 36) ? (h / 36) : 1;
    unsigned long long sr = 0, sg = 0, sb = 0, nz = 0, yel = 0, ns = 0;
    for (int y = 0; y < h; y += sy) {
        const uint8_t *row = base + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x += sx) {
            const uint8_t *q = row + (size_t)x * 4;
            const unsigned r = q[0], g = q[1], b = q[2];
            sr += r; sg += g; sb += b; ns++;
            if (r || g || b) nz++;
            if (r >= 200 && g >= 150 && b <= 60) yel++;
            if (r > s->mxr) s->mxr = r;
            if (g > s->mxg) s->mxg = g;
            if (b > s->mxb) s->mxb = b;
        }
    }
    if (!ns) return;
    s->mr = (unsigned)(sr / ns); s->mg = (unsigned)(sg / ns); s->mb = (unsigned)(sb / ns);
    s->nz = (unsigned)(nz * 1000ull / ns); s->yel = (unsigned)(yel * 1000ull / ns);
    s->ns = (unsigned)ns;
}
/* 通道台账：写次数 / 失败次数 / 首次 / 末次（monotonic 秒）/ 最近一次 源与目标 的 6 个统计量。 */
static _Atomic unsigned long long g_m104a_wr[M104A_NCH], g_m104a_fl[M104A_NCH];
static _Atomic unsigned long long g_m104a_first_us[M104A_NCH], g_m104a_last_us[M104A_NCH];
static _Atomic unsigned long long g_m104a_sigsrc[M104A_NCH][7];   /* mr,mg,mb,mx,nz‰,yel‰,ext */
static _Atomic unsigned long long g_m104a_sigtgt[M104A_NCH][7];
static _Atomic unsigned long long g_m104a_fillc[M104A_NCH][3];    /* 纯色通道的 rgb */
/* 失败计数（RequestBuffer / 句柄不可用 / mmap / Flush 拒绝）—— 「没写」与「写了但被拒」分开记。 */
static void m104a_count_fail(int tg0)
{
    int tg = tg0;
    if (tg <= 0 || tg >= M104A_NCH) tg = 1;
    atomic_fetch_add_explicit(&g_m104a_fl[tg], 1ull, memory_order_relaxed);
}
/* 30 s 汇总（nvn_vk.c 心跳线程调用；返回写入 out 的字符数，格式与派活单一致）。 */
int nvn_win_m104a_report(char *out, int cap)
{
    if (!out || cap <= 0) return 0;
    static const char *nm[M104A_NCH] = { "none", "wbuf/main", "wbuf/pump", "wbuf/m67b", "wbuf/init", "other" };
    double t0 = 0; int o = 0;
    for (int c = 0; c < M104A_NCH; c++) {
        const double f = (double)atomic_load_explicit(&g_m104a_first_us[c], memory_order_relaxed) / 1e6;
        if (f > 0 && (t0 <= 0 || f < t0)) t0 = f;
    }
    for (int c = 0; c < M104A_NCH; c++) {
        const unsigned long long n = atomic_load_explicit(&g_m104a_wr[c], memory_order_relaxed);
        const unsigned long long bad = atomic_load_explicit(&g_m104a_fl[c], memory_order_relaxed);
        if (!n && !bad) continue;
        const double f = (double)atomic_load_explicit(&g_m104a_first_us[c], memory_order_relaxed) / 1e6 - t0;
        const double l = (double)atomic_load_explicit(&g_m104a_last_us[c], memory_order_relaxed) / 1e6 - t0;
        const unsigned long long *s = (const unsigned long long *)&g_m104a_sigsrc[c];
        const unsigned long long *g = (const unsigned long long *)&g_m104a_sigtgt[c];
        const unsigned long long *fc = (const unsigned long long *)&g_m104a_fillc[c];
        o += snprintf(out + o, (size_t)(cap - o),
                      " | %s writes=%llu fail=%llu first=%.1fs last=%.1fs src[mean=%llu/%llu/%llu max=%llu nz=%llu.%llu%% yel=%llu.%llu%%] "
                      "tgt[mean=%llu/%llu/%llu max=%llu nz=%llu.%llu%% yel=%llu.%llu%%] col=%llu,%llu,%llu",
                      nm[c], n, bad, f, l, s[0], s[1], s[2], s[3], s[4] / 10ull, s[4] % 10ull, s[5] / 10ull, s[5] % 10ull,
                      g[0], g[1], g[2], g[3], g[4] / 10ull, g[4] % 10ull, g[5] / 10ull, g[5] % 10ull,
                      fc[0], fc[1], fc[2]);
        if (o >= cap - 64) break;
    }
    out[(o < cap) ? o : (cap - 1)] = 0;
    return o;
}

/* Returns 1 when the window was filled and flushed, 0 when the buffer could not be obtained. */
int nvn_win_fill(void *win, unsigned r, unsigned g, unsigned b)
{
    OHNativeWindow *w = (OHNativeWindow *)win;
    if (!w) return 0;

    OHNativeWindowBuffer *buf = NULL;
    int fence = -1;
    if (OH_NativeWindow_NativeWindowRequestBuffer(w, &buf, &fence) != 0 || !buf) return 0;

    BufferHandle *h = OH_NativeWindow_GetBufferHandleFromNative(buf);
    if (!h || h->size <= 0 || h->stride <= 0 || h->height <= 0) {
        OH_NativeWindow_NativeWindowAbortBuffer(w, buf);
        return 0;
    }

    uint8_t *base = (uint8_t *)h->virAddr;
    int mapped = 0;
    if (!base) {
        void *p = mmap(NULL, (size_t)h->size, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, 0);
        if (p == MAP_FAILED) { OH_NativeWindow_NativeWindowAbortBuffer(w, buf); return 0; }
        base = (uint8_t *)p;
        mapped = 1;
    }

    /* BGRA8888 is the surface format the XComponent gets by default; channel order is irrelevant for G1 (any non-black
       pixel counts) but keeping it right costs nothing. */
    uint32_t px = 0xFF000000u | ((uint32_t)(b & 0xFF) << 16) | ((uint32_t)(g & 0xFF) << 8) | (uint32_t)(r & 0xFF);
    int rows = h->height, words_per_row = h->stride / 4;
    for (int y = 0; y < rows; y++) {
        uint32_t *row = (uint32_t *)(base + (size_t)y * (size_t)h->stride);
        for (int x = 0; x < words_per_row; x++) row[x] = px;
    }

    if (mapped) munmap(base, (size_t)h->size);

    Region region = {0};   /* rects=nullptr + rectNumber 0 = whole buffer dirty */
    if (OH_NativeWindow_NativeWindowFlushBuffer(w, buf, -1, region) != 0) {
        /* M104A：纯色通道的失败也要计数（「窗口拒绝这次写」与「根本没调」是两种结论）。 */
        int tg = atomic_load_explicit(&g_m104a_tag, memory_order_relaxed);
        if (tg <= 0 || tg >= M104A_NCH) tg = 3;
        atomic_fetch_add_explicit(&g_m104a_fl[tg], 1ull, memory_order_relaxed);
        return 0;
    }
    /* M104A：纯色调试通道（wfill）计数 —— 每次写都把颜色记进台账，30 s 汇总一行。 */
    {
        int tg = atomic_load_explicit(&g_m104a_tag, memory_order_relaxed);
        if (tg <= 0 || tg >= M104A_NCH) tg = 3;
        const unsigned long long n = atomic_fetch_add_explicit(&g_m104a_wr[tg], 1ull, memory_order_relaxed) + 1ull;
        atomic_store_explicit(&g_m104a_last_us[tg], (unsigned long long)(m104a_now() * 1e6), memory_order_relaxed);
        if (!atomic_load_explicit(&g_m104a_first_us[tg], memory_order_relaxed))
            atomic_store_explicit(&g_m104a_first_us[tg], (unsigned long long)(m104a_now() * 1e6), memory_order_relaxed);
        atomic_store_explicit(&g_m104a_fillc[tg][0], r, memory_order_relaxed);
        atomic_store_explicit(&g_m104a_fillc[tg][1], g, memory_order_relaxed);
        atomic_store_explicit(&g_m104a_fillc[tg][2], b, memory_order_relaxed);
        if (n <= 3 || (n % 60) == 0)
            vk_boot_logf("[m104a] w wfill/t%d writes=%llu col=%u,%u,%u geo=%dx%d (纯色 CPU 填色通道)",
                         tg, n, r, g, b, rows, words_per_row * 4);
    }
    return 1;
}

/* ---- M67B (2026-11-12): 只读能力探测 —— 窗口 API 到底能不能用 ----
 *
 * 为什么需要：native_buffer 缺失时，「上屏失败」有两种完全不同的原因 ——
 *   ①窗口拿不到 buffer（RequestBuffer / FlushBuffer 挂）；
 *   ②Vulkan 侧没有可拷的源（离屏 RT 还没画过 / 取源判据不命中）。
 * 不分开就只能靠猜。本函数**不取 buffer、不写一个像素**：只问窗口当前的几何与格式，
 * 把 OH_NativeWindow 的真返回码交给调用方落日志。
 *
 * 返回 1 = 两个查询都成功；0 = 至少一个失败（*prc_geo / *prc_fmt 带真返回码，-3 = 未调用）。
 * GET_BUFFER_GEOMETRY 的出参顺序按 SDK 头文件 external_window.h:111 的注释是 (height, width)。 */
int nvn_win_probe(void *win, int *pw, int *ph, int *pfmt, int *prc_geo, int *prc_fmt)
{
    if (pw) *pw = 0;
    if (ph) *ph = 0;
    if (pfmt) *pfmt = -1;
    if (prc_geo) *prc_geo = -3;
    if (prc_fmt) *prc_fmt = -3;
    OHNativeWindow *w = (OHNativeWindow *)win;
    if (!w) return 0;
    int32_t h = 0, wd = 0, fmt = -1;
    int32_t rg = OH_NativeWindow_NativeWindowHandleOpt(w, GET_BUFFER_GEOMETRY, &h, &wd);
    int32_t rf = OH_NativeWindow_NativeWindowHandleOpt(w, GET_FORMAT, &fmt);
    if (prc_geo) *prc_geo = (int)rg;
    if (prc_fmt) *prc_fmt = (int)rf;
    if (ph) *ph = (int)h;
    if (pw) *pw = (int)wd;
    if (pfmt) *pfmt = (int)fmt;
    return (rg == 0 && rf == 0) ? 1 : 0;
}

/* ---- M3o (2026-11-03): real frame onto the window — 离屏 RT 的像素走这里上屏 ----
 *
 * src = 顶朝下的 RGBA8 图像（离屏 RT 回读后的宿主内存），src_stride = 源行字节数。
 *
 * M4cc P1 (2026-11-04, this round's geometry fix)：**整幅源等比铺满目标面，不裁**。
 * 旧行为（已删）：`cw=min(sw,ow), ch=min(sh,oh)` + 居中 (ox,oy) 的 1:1 memcpy —— 1280x720 源
 * 被原样贴在 2856x1320 面中央，屏幕上是中间一小块图 + 四边黑；且 nvn_vk.c 上游还先居中裁了
 * 1280x720（left 160 / top 90），logo 左缘就是这样被切掉的。
 * 现在：
 *   · 源矩形 = 全幅（调用方保证 staging 里就是 0,0..sw,sh）；
 *   · 等比缩放 scale = min(ow/sw, oh/sh)（取整用整数算术，避免浮点给小尺寸带来 ±1 抖动）；
 *     本机的源 1600x900 与面 2856x1320 比例不同（1.778 vs 2.164）⇒ 高度受限，
 *     内容 2347x1320、左右各 254 px 黑边（letterbox），不裁任何像素；
 *   · 补边像素每帧重填（buffer 池会轮换，不能假设上一帧的黑边还在）；
 *   · 缩放用最近邻：模拟器本来就是 SwiftShader 软渲，双线性只是多几倍 CPU，几何判读不需要它。
 *   · 目标面尺寸**只来自运行时真值**（BufferHandle 的 width/height/stride），没有任何写死的面尺寸。
 *
 * 通道顺序不猜：BufferHandle.format 先打印再决定——GRAPHIC_PIXEL_FMT_RGBA_8888 = 12（直拷）、
 * GRAPHIC_PIXEL_FMT_BGRA_8888 = 13（交换 R/B）。M3o 首 boot 实测 fmt=12、屏幕出蓝三角，即 12 是
 * RGBA 字节序（详见 swap 那一行的注释）。
 *
 * 调用线程：必须是既有线程（gfx_bridge 的 nvnQueuePresentTexture 同一个游戏/渲染线程）。历史三次
 * 判别实验里从这里之外的路径（客体其它线程 / 新建 pthread）调原生 UI API 会崩；本函数刻意不自己
 * 起线程、不加锁，只做一次 RequestBuffer→缩放拷贝→Flush。 */
int nvn_win_present_rgba(void *win, const uint8_t *src, int sw, int sh, int src_stride)
{
    OHNativeWindow *w = (OHNativeWindow *)win;
    const int m104a_tg0 = atomic_load_explicit(&g_m104a_tag, memory_order_relaxed);   /* M104A：写者归属 */
    if (!w || !src || sw <= 0 || sh <= 0 || src_stride < sw * 4) { m104a_count_fail(m104a_tg0); return 0; }

    OHNativeWindowBuffer *buf = NULL;
    int fence = -1;
    int rq = OH_NativeWindow_NativeWindowRequestBuffer(w, &buf, &fence);
    if (rq != 0 || !buf) {
        fprintf(stderr, "[p-red] RequestBuffer rv=%d errno=%d\n", rq, errno);
        m104a_count_fail(m104a_tg0);
        return 0;
    }
    BufferHandle *h = OH_NativeWindow_GetBufferHandleFromNative(buf);
    if (!h || h->size <= 0 || h->stride <= 0 || h->height <= 0 || h->width <= 0) {
        fprintf(stderr, "[p-red] BufferHandle unusable size=%d stride=%d %dx%d\n",
                h ? h->size : -1, h ? h->stride : -1, h ? h->width : -1, h ? h->height : -1);
        OH_NativeWindow_NativeWindowAbortBuffer(w, buf);
        m104a_count_fail(m104a_tg0);
        return 0;
    }

    static int logged;
    if (!logged) {
        logged = 1;
        fprintf(stderr, "[p] window=%p %dx%d fmt=%d stride=%d size=%d virAddr=%p tid=%d\n",
                (void *)w, h->width, h->height, h->format, h->stride, h->size, h->virAddr,
                (int)syscall(__NR_gettid));
    }

    uint8_t *base = (uint8_t *)h->virAddr;
    int mapped = 0;
    if (!base) {
        void *p = mmap(NULL, (size_t)h->size, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, 0);
        if (p == MAP_FAILED) {
            fprintf(stderr, "[p-red] mmap(%d) errno=%d\n", h->size, errno);
            OH_NativeWindow_NativeWindowAbortBuffer(w, buf);
            m104a_count_fail(m104a_tg0);
            return 0;
        }
        base = (uint8_t *)p;
        mapped = 1;
    }

    const int ow = h->width, oh = h->height, dst_stride = h->stride;
    /* M104A：写之前的缓冲内容（= 上一轮谁留在池里的像素）。这一项是「有没有第二个写者」的
       直接证据：若它既不是纯黑也不是我们上一次写的内容，就有别人在写同一块面。 */
    m104a_sig_t m104a_prev;
    m104a_sig(&m104a_prev, base, dst_stride, ow, oh);
    /* M4cc P1：等比适配（fit）—— 目标面尺寸全部来自运行时 BufferHandle，源矩形恒为全幅。
       dw/dh = 源按 scale 映射到面上的像素尺寸；受限轴取满，另一轴居中留黑边（letterbox）。
       整数算术保证「同一对 (src,dst) 永远得到同一组数」，便于逐帧比对日志。 */
    int dw = ow, dh = (int)(((long long)sh * (long long)ow) / (long long)sw);
    if (dh > oh) { dh = oh; dw = (int)(((long long)sw * (long long)oh) / (long long)sh); }
    if (dw > ow) dw = ow;    /* 防整数取整越界 */
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    const int bx = (ow - dw) / 2, by = (oh - dh) / 2;      /* 内容区左上角 */
    const int bar_l = bx, bar_r = ow - dw - bx;
    const int bar_t = by, bar_b = oh - dh - by;
    /* 通道顺序由 BufferHandle.format 决定（GRAPHIC_PIXEL_FMT 枚举，不是 pixelmap 的 PIXEL_FORMAT）：
       12 = GRAPHIC_PIXEL_FMT_RGBA_8888（本模拟器实测值）→ 直拷；13 = GRAPHIC_PIXEL_FMT_BGRA_8888 → 交换 R/B。
       M3o 首次 boot 用了 pixelmap 枚举（3=RGBA）判断，fmt=12 被误判成 BGRA，屏幕上出来的是**蓝**三角形
       （截图 (0,0,254)），反证 12 就是 RGBA。此后：只有 13 交换，其余未知值一律直拷，fmt 打进日志备查。 */
    const int swap = (h->format == 13);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const long long t0 = (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

    /* 整面黑：只在前 8 帧（覆盖 buffer 池里可能存在的未初始化/旧内容，池一般 2~3 张）。 */
    static int bar_frames = 8;
    if (bar_frames > 0) {
        bar_frames--;
        uint32_t *p = (uint32_t *)base;
        size_t n = (size_t)(dst_stride / 4) * (size_t)oh;
        for (size_t i = 0; i < n; i++) p[i] = 0xFF000000u;
    }

    /* M4cc P1：补边每帧重填（池轮换 ⇒ 不能假设上一帧的黑边还在）。只写边条，不碰内容区。 */
    const uint32_t black = 0xFF000000u;
    if (bar_t > 0 || bar_b > 0 || bar_l > 0 || bar_r > 0) {
        const int words = dst_stride / 4;
        /* 上下整条 */
        for (int y = 0; y < bar_t; y++) {
            uint32_t *row = (uint32_t *)(base + (size_t)y * (size_t)dst_stride);
            for (int x = 0; x < words; x++) row[x] = black;
        }
        for (int y = oh - bar_b; y < oh; y++) {
            uint32_t *row = (uint32_t *)(base + (size_t)y * (size_t)dst_stride);
            for (int x = 0; x < words; x++) row[x] = black;
        }
        /* 左右竖条（只覆盖内容区那几条行） */
        if (bar_l > 0 || bar_r > 0) {
            for (int y = by; y < by + dh; y++) {
                uint32_t *row = (uint32_t *)(base + (size_t)y * (size_t)dst_stride);
                for (int x = 0; x < bar_l; x++) row[x] = black;
                for (int x = ow - bar_r; x < ow; x++) row[x] = black;
            }
        }
    }

    /* M4cc P1：最近邻等比缩放。sxmap 只在 (sw,dw) 变化时重建 —— 每帧省 2347 次除法，几乎免费。 */
    static int *sxmap;
    static int sxmap_cap, sxmap_sw, sxmap_dw;
    if (sxmap_cap < dw) {
        int *np = (int *)realloc(sxmap, (size_t)dw * sizeof(int));
        if (np) { sxmap = np; sxmap_cap = dw; sxmap_sw = 0; }
    }
    if (sxmap && (sxmap_sw != sw || sxmap_dw != dw)) {
        sxmap_sw = sw; sxmap_dw = dw;
        for (int x = 0; x < dw; x++) {
            int sxi = (int)(((long long)x * (long long)sw) / (long long)dw);
            if (sxi >= sw) sxi = sw - 1;
            sxmap[x] = sxi;
        }
    }
    if (!sxmap) {
        /* 分配失败（极不可能）：退回 1:1 居中，绝不写越界。 */
        const int cw = sw < ow ? sw : ow, ch = sh < oh ? sh : oh;
        const int ox = (ow - cw) / 2, oy = (oh - ch) / 2;
        for (int y = 0; y < ch; y++)
            memcpy((uint32_t *)(base + (size_t)(oy + y) * (size_t)dst_stride + (size_t)ox * 4),
                   src + (size_t)y * (size_t)src_stride, (size_t)cw * 4);
    } else if (!swap) {
        for (int y = 0; y < dh; y++) {
            int sy = (int)(((long long)y * (long long)sh) / (long long)dh);
            if (sy >= sh) sy = sh - 1;
            const uint32_t *s = (const uint32_t *)(src + (size_t)sy * (size_t)src_stride);
            uint32_t *d = (uint32_t *)(base + (size_t)(by + y) * (size_t)dst_stride + (size_t)bx * 4);
            for (int x = 0; x < dw; x++) d[x] = s[sxmap[x]];
        }
    } else {
        for (int y = 0; y < dh; y++) {
            int sy = (int)(((long long)y * (long long)sh) / (long long)dh);
            if (sy >= sh) sy = sh - 1;
            const uint32_t *s = (const uint32_t *)(src + (size_t)sy * (size_t)src_stride);
            uint32_t *d = (uint32_t *)(base + (size_t)(by + y) * (size_t)dst_stride + (size_t)bx * 4);
            for (int x = 0; x < dw; x++) {
                uint32_t v = s[sxmap[x]];
                d[x] = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu);
            }
        }
    }

    /* M4x P1：窗口存活/方向标记。只在**上边条够高**时画（绝不盖画面）。M4cc 起内容按高度取满
       （by=0），所以本机默认不再画这两块 —— 判据里也不再依赖它们。 */
    {
        const int ms = 48;
        if (by > ms + 16 && ow > ms * 2 + 32) {
            const int mx0 = 8, mx1 = ow - 8 - ms, my = 8;
            for (int y = 0; y < ms; y++) {
                uint32_t *row = (uint32_t *)(base + (size_t)(my + y) * (size_t)dst_stride);
                for (int x = 0; x < ms; x++) { row[mx0 + x] = 0xFFFF00FFu; row[mx1 + x] = 0xFF00FFFFu; }
            }
        }
    }

    /* M4cc P1 判据①：几何原文行。crop=none 是本轮的核心断言（源矩形恒 0,0..sw,sh）。
       scale 打印到 1e-6（scale_ppm），bars 是四边补边像素数，barpx 是补边总像素数。 */
    {
        static unsigned long long geo_n;
        geo_n++;
        if (geo_n <= 3 || (geo_n % 120) == 0) {
            const double sc = (double)dw / (double)sw;
            vk_boot_logf("[w] present geo src=%dx%d dst=%dx%d content=%dx%d+%d+%d scale=%.6f scale_ppm=%lld "
                         "bars=%d,%d,%d,%d barpx=%lld crop=none fmt=%d swap=%d",
                         sw, sh, ow, oh, dw, dh, bx, by, sc, (long long)(sc * 1000000.0),
                         bar_l, bar_t, bar_r, bar_b,
                         (long long)((long long)bar_l * dh + (long long)bar_r * dh +
                                     (long long)bar_t * ow + (long long)bar_b * ow),
                         h->format, swap);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    const long long t1 = (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

    /* M104A：写完之后目标面（= 屏幕上会出现的那张）与源（= staging 里要送屏的像素）各采一次。
       判据③「结论通道的 mean/色数与截图一致」就是拿 tgt 这组数去对设备侧截图。 */
    m104a_sig_t m104a_src, m104a_tgt;
    m104a_sig(&m104a_src, src, src_stride, sw, sh);
    m104a_sig(&m104a_tgt, base, dst_stride, ow, oh);

    if (mapped) munmap(base, (size_t)h->size);

    Region region = {0};
    int fr = OH_NativeWindow_NativeWindowFlushBuffer(w, buf, -1, region);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const long long t2 = (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    if (fr != 0) {
        fprintf(stderr, "[p-red] FlushBuffer rv=%d errno=%d\n", fr, errno);
        m104a_count_fail(m104a_tg0);
        return 0;
    }

    static unsigned long long n;
    n++;
    if (n <= 3 || n % 60 == 0)
        fprintf(stderr, "[p] present #%llu ok copy us=%lld flush us=%lld dst=%dx%d+%d+%d src=%dx%d "
                        "content=%dx%d fmt=%d stride=%d swap=%d\n",
                n, t1 - t0, t2 - t1, ow, oh, bx, by, sw, sh, dw, dh, h->format, dst_stride, swap);
    /* M4x P1：把窗口 buffer 的真实几何/格式写进文件日志（stderr 只进 hilog，首波会被流控丢），
       这是「送屏目标到底多大、什么格式、通道要不要换」的唯一可核对来源。 */
    { if (n <= 2 || n % 120 == 0)
          vk_boot_logf("[p] win #%llu buf=%dx%d fmt=%d stride=%d size=%d virAddr=%p tid=%d "
                       "src=%dx%d off=%d,%d copy=%dx%d swap=%d",
                       n, ow, oh, h->format, dst_stride, h->size, (void *)h->virAddr,
                       (int)syscall(__NR_gettid), sw, sh, bx, by, dw, dh, swap); }
    /* ===== M104A：可见面写者台账（真图 blit 通道） =====================================
       每写一次记一次；日志按 ≤1 Hz 限流（**计数不丢**：writes 是精确累计，行是采样）。
       行格式按派活单：`[m104a] w <chan> writes=<n> last=<ts> img=<handle> mean/max ...`。
       另给 prev（写之前池里的内容）、tgt（写完的目标面）、src（staging 源）三组统计 ——
       判据②「三方对齐」里，tgt 对截图、src 对我们的 staging、prev 对「有没有第二个写者」。 */
    {
        int tg = m104a_tg0;
        if (tg <= 0 || tg >= M104A_NCH) tg = 1;
        const unsigned long long nw = atomic_fetch_add_explicit(&g_m104a_wr[tg], 1ull, memory_order_relaxed) + 1ull;
        const unsigned long long us = (unsigned long long)(m104a_now() * 1e6);
        atomic_store_explicit(&g_m104a_last_us[tg], us, memory_order_relaxed);
        if (!atomic_load_explicit(&g_m104a_first_us[tg], memory_order_relaxed))
            atomic_store_explicit(&g_m104a_first_us[tg], us, memory_order_relaxed);
        atomic_store_explicit(&g_m104a_sigsrc[tg][0], m104a_src.mr, memory_order_relaxed);
        atomic_store_explicit(&g_m104a_sigsrc[tg][1], m104a_src.mg, memory_order_relaxed);
        atomic_store_explicit(&g_m104a_sigsrc[tg][2], m104a_src.mb, memory_order_relaxed);
        atomic_store_explicit(&g_m104a_sigsrc[tg][3], m104a_src.mxr > m104a_src.mxg ?
                              (m104a_src.mxr > m104a_src.mxb ? m104a_src.mxr : m104a_src.mxb) :
                              (m104a_src.mxg > m104a_src.mxb ? m104a_src.mxg : m104a_src.mxb), memory_order_relaxed);
        atomic_store_explicit(&g_m104a_sigsrc[tg][4], m104a_src.nz, memory_order_relaxed);
        atomic_store_explicit(&g_m104a_sigsrc[tg][5], m104a_src.yel, memory_order_relaxed);
        atomic_store_explicit(&g_m104a_sigsrc[tg][6], (unsigned long long)sw * (unsigned long long)sh, memory_order_relaxed);
        atomic_store_explicit(&g_m104a_sigtgt[tg][0], m104a_tgt.mr, memory_order_relaxed);
        atomic_store_explicit(&g_m104a_sigtgt[tg][1], m104a_tgt.mg, memory_order_relaxed);
        atomic_store_explicit(&g_m104a_sigtgt[tg][2], m104a_tgt.mb, memory_order_relaxed);
        atomic_store_explicit(&g_m104a_sigtgt[tg][3], m104a_tgt.mxr > m104a_tgt.mxg ?
                              (m104a_tgt.mxr > m104a_tgt.mxb ? m104a_tgt.mxr : m104a_tgt.mxb) :
                              (m104a_tgt.mxg > m104a_tgt.mxb ? m104a_tgt.mxg : m104a_tgt.mxb), memory_order_relaxed);
        atomic_store_explicit(&g_m104a_sigtgt[tg][4], m104a_tgt.nz, memory_order_relaxed);
        atomic_store_explicit(&g_m104a_sigtgt[tg][5], m104a_tgt.yel, memory_order_relaxed);
        atomic_store_explicit(&g_m104a_sigtgt[tg][6], (unsigned long long)ow * (unsigned long long)oh, memory_order_relaxed);
        {
            static double lastline[M104A_NCH];
            const double nws = m104a_now();
            if (nw <= 3 || nws - lastline[tg] >= 1.0) {
                lastline[tg] = nws;
                vk_boot_logf("[m104a] w wbuf/t%d writes=%llu last=%.1fs img=%p tid=%d src=%dx%d dst=%dx%d+%d+%d "
                             "src[mean=%u/%u/%u max=%u/%u/%u nz=%u.%u%% yel=%u.%u%%] "
                             "tgt[mean=%u/%u/%u max=%u/%u/%u nz=%u.%u%% yel=%u.%u%%] "
                             "prev[mean=%u/%u/%u nz=%u.%u%% yel=%u.%u%%]",
                             tg, nw, nws, (const void *)src, (int)syscall(__NR_gettid), sw, sh, ow, oh, bx, by,
                             m104a_src.mr, m104a_src.mg, m104a_src.mb, m104a_src.mxr, m104a_src.mxg, m104a_src.mxb,
                             m104a_src.nz / 10u, m104a_src.nz % 10u, m104a_src.yel / 10u, m104a_src.yel % 10u,
                             m104a_tgt.mr, m104a_tgt.mg, m104a_tgt.mb, m104a_tgt.mxr, m104a_tgt.mxg, m104a_tgt.mxb,
                             m104a_tgt.nz / 10u, m104a_tgt.nz % 10u, m104a_tgt.yel / 10u, m104a_tgt.yel % 10u,
                             m104a_prev.mr, m104a_prev.mg, m104a_prev.mb,
                             m104a_prev.nz / 10u, m104a_prev.nz % 10u, m104a_prev.yel / 10u, m104a_prev.yel % 10u);
            }
        }
    }
    return 1;
}
