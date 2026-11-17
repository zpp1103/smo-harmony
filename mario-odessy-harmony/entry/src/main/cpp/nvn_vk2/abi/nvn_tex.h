// NVN texture storage layout (Tegra X1 block-linear), see nvn_tex.c.
//
// M43A / S2a（2026-11-11）**布局统一**：这份头是 nx2android 的 nvn_tex.h 搬入件，原版第 36 字节是
// `int pkg_block_h;`（整 4 字节当一个 int），而我方 third_party/d2r_gfx/gfx/nvn/nvn_tex.h 把它拆成
// `int8 pkg_block_h:8 + swizzle_r/g/b/a:3×4 + pad:12`（同样 4 字节）。S1 报告 §8-1 记的就是这处分叉。
//
// 实测结论（logs/m43a/texdesc_check.txt，两 TU 各编一次打印全部 offsetof）：**两边尺寸/偏移完全一致**
// （sizeof=40，target 0 / format 4 / flags 8 / samples 12 / width 16 / height 20 / depth 24 / levels 28 /
// stride 32 / 第 36 字节 4 字节），分叉只在**第 36 字节这 32 bit 的语义**：nx2 当整 int 读，我方按位域拆。
//
// 统一方案：abi/ 这份采用**我方（超集）布局**——位域版本能同时表达「块高（byte0）」和「通道 swizzle
// （byte1..）」，而 nx2 侧只有两处接触该字段：
//   1. hle_nvn.c:313 `d.pkg_block_h = l->layout[0] + 1`（写入，值 1..8，塞进 8 bit 位域无损）；
//   2. nvn_tex.c:115 `(unsigned)d.pkg_block_h - 1`（读取，位域下拿到的就是 byte0 的有符号 8 bit，
//      与 nx2 原意「byte 0 = 块高 log2」一致；原版按整 int 读，一旦 byte1..3 带 swizzle 位就会读到垃圾）。
// 即：换成位域后，nx2 的两处代码**不改一行**语义就对了。若将来某处确实要整 32 bit 原值，用
// nvn_texdesc_pkg_word()/nvn_texdesc_set_pkg_word()（本头提供）显式读写，禁止再直接依赖 int 别名。
//
// 布局由下面的字段级 _Static_assert 双向锁死（改任何字段/顺序/位宽都会编译失败）；
// 与 third_party 那份的一致性由 tools/m43a_texdesc_check.c 逐字段比对（logs/m43a/texdesc_check.txt）。
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int target, format, flags, samples;   // NVNtextureTarget / NVNformat / NVNtextureFlags / sample count (0 or 1 = single-sampled)
    uint32_t width, height, depth, levels, stride;
    // Byte 0 of the packaged texture layout parameters (log2 of block height) + 1; 0 = use the automatic algorithm.
    // Bytes 1..3 carry the per-channel swizzle selectors the game passes to TextureBuilderSetSwizzle
    // (identity 2,3,4,5 = R,G,B,A when unset). This is the *unified* interpretation (M43A): both trees read
    // byte 0 as the block height, and the swizzle bits are the superset this project's replay path needs.
    signed int pkg_block_h:8;
    unsigned int swizzle_r:3, swizzle_g:3, swizzle_b:3, swizzle_a:3, swizzle_pad:12;
} nvn_texdesc;

typedef struct {
    uint64_t size, layer_size;            // Total storage bytes, bytes per layer
    uint32_t align, layers, bpb;          // Alignment, layer count, bytes per block
    unsigned tileH, tileD;                // Block height / block depth (log2 of the GOB count)
} nvn_texlayout;

// ---- M43A: layout locked field by field (both trees must agree on every one of these) ----
_Static_assert(sizeof(nvn_texdesc) == 40, "nvn_texdesc must be 40 bytes (both trees)");
_Static_assert(offsetof(nvn_texdesc, target) == 0,  "nvn_texdesc.target");
_Static_assert(offsetof(nvn_texdesc, format) == 4,  "nvn_texdesc.format");
_Static_assert(offsetof(nvn_texdesc, flags) == 8,   "nvn_texdesc.flags");
_Static_assert(offsetof(nvn_texdesc, samples) == 12,"nvn_texdesc.samples");
_Static_assert(offsetof(nvn_texdesc, width) == 16,  "nvn_texdesc.width");
_Static_assert(offsetof(nvn_texdesc, height) == 20, "nvn_texdesc.height");
_Static_assert(offsetof(nvn_texdesc, depth) == 24,  "nvn_texdesc.depth");
_Static_assert(offsetof(nvn_texdesc, levels) == 28, "nvn_texdesc.levels");
_Static_assert(offsetof(nvn_texdesc, stride) == 32, "nvn_texdesc.stride");
// 位域不能 offsetof（C 标准禁止），所以用「前驱字段末端 + 总尺寸」夹逼：stride 是最后一个普通字段，
// 它结束于 36；总尺寸 40 ⇒ 剩下的 4 字节位域字必然落在 36..39。位域存储单元宽度另用无名结构体断言。
_Static_assert(offsetof(nvn_texdesc, stride) + sizeof(uint32_t) == 36, "nvn_texdesc: the packed word starts at byte 36");
_Static_assert(sizeof(nvn_texdesc) == 40, "nvn_texdesc: the packed word is the last 4 bytes");
_Static_assert(sizeof(struct { signed int h : 8; unsigned r : 3, g : 3, b : 3, a : 3, pad : 12; }) == 4,
               "the pkg/swizzle bitfields must pack into one 32-bit word");
_Static_assert(sizeof(nvn_texlayout) == 40, "nvn_texlayout must be 40 bytes (both trees)");
_Static_assert(offsetof(nvn_texlayout, size) == 0 && offsetof(nvn_texlayout, layer_size) == 8 &&
               offsetof(nvn_texlayout, align) == 16 && offsetof(nvn_texlayout, layers) == 20 &&
               offsetof(nvn_texlayout, bpb) == 24 && offsetof(nvn_texlayout, tileH) == 28 &&
               offsetof(nvn_texlayout, tileD) == 32, "nvn_texlayout field offsets");

// The whole packed 32-bit word (byte 0 = block height+1, bytes 1..3 = swizzle selectors), for the rare
// caller that must move it verbatim (packaged-layout capture, object copies) instead of field by field.
#define NVN_TEXDESC_PKG_WORD_OFF 36   /* == offsetof(stride) + 4; locked by the asserts above */
static inline uint32_t nvn_texdesc_pkg_word(const nvn_texdesc *d) {
    uint32_t w; __builtin_memcpy(&w, (const uint8_t *)d + NVN_TEXDESC_PKG_WORD_OFF, 4); return w;
}
static inline void nvn_texdesc_set_pkg_word(nvn_texdesc *d, uint32_t w) {
    __builtin_memcpy((uint8_t *)d + NVN_TEXDESC_PKG_WORD_OFF, &w, 4);
}

void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o);
uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level);
uint32_t nvn_tex_level_gobs(const nvn_texdesc *d, unsigned level);   // GOB columns level `level` is laid out with (its width in GOBs; padded to whole 64 KB tiles in a sparse texture's tiled levels)
int nvn_fmt_bpb(int format, int *bw, int *bh);   // Bytes per block (0 = unknown format), block width and height
// Block height / block depth actually used by level `level` (log2 of the GOB count): the per-level block height shrinks with that level's row count (block rows), and deswizzling uses it level by level
void nvn_tex_level_tiles(const nvn_texdesc *d, unsigned level, unsigned *tileH, unsigned *tileD);
// A texture buffer of n elements of bpb bytes read as 2D rows of NVN_TBUF_ROW texels (the translator's wrapped 1D fetch, x & 0x3FFF / x >> 14): width, height and bytes per row of the
// linear 2D texture over the same memory. One row is n texels, its byte length rounded up to align (the linear-texture row alignment); more rows are full 16384-texel rows. 0 when n or bpb is 0
#define NVN_TBUF_ROW 16384u
int nvn_tbuf_rows(uint64_t n, uint32_t bpb, uint32_t align, uint32_t *w, uint32_t *h, uint32_t *row);
