// M43A / S2a：nvn_tex_level_gobs 的唯一提供者。
//
// 我方 platform/nvn_tex.c 与 nx2 的 nvn_tex.c 同源（deko3d 布局算法），布局已在 abi/nvn_tex.h 统一；
// 差异只有一支导出函数：我方树没有 nvn_tex_level_gobs（只有 nx2 的 nvn_deswizzle/hle_nvn 用它算
// 「这一级按多少 GOB 列排布」）。与其重写算法，这里把 nx2 的 nvn_tex.c 原文（nvn_tex_nx2.inc）整体
// 前缀化编进来，避免与 platform/nvn_tex.c 的同名符号冲突，只把 nvn_tex_level_gobs 以原名暴露。
#define nvn_tex_layout       m43a_nvn_tex_layout
#define nvn_tex_level_offset m43a_nvn_tex_level_offset
#define nvn_fmt_bpb          m43a_nvn_fmt_bpb
#define nvn_tex_level_tiles  m43a_nvn_tex_level_tiles
#define nvn_tex_level_gobs   m43a_nvn_tex_level_gobs
#define nvn_tbuf_rows        m43a_nvn_tbuf_rows
#include "nvn_tex_nx2.inc"
#undef nvn_tex_layout
#undef nvn_tex_level_offset
#undef nvn_fmt_bpb
#undef nvn_tex_level_tiles
#undef nvn_tex_level_gobs
#undef nvn_tbuf_rows

uint32_t nvn_tex_level_gobs(const nvn_texdesc *d, unsigned level)
{
    return m43a_nvn_tex_level_gobs(d, level);
}
