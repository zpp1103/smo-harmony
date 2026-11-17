// M42A / S1: forward declaration of the block-linear decoder this slice links from our own tree
// (platform/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c, already built for arm64 — CMakeLists.txt:37).
//
// Why a local declaration instead of including that tree's header: its header pulls the OLD nvn_tex.h
// (d2r_gfx/gfx/nvn/nvn_tex.h), whose nvn_texdesc differs from nx2android's abi/nvn_tex.h that this whole
// directory compiles against — see the hazard note in the M42A report (§卡点). Declaring only the entry point
// keeps the include graph single-headed.
//
// ⚠ HAZARD for S2/S3 (documented, not exercised by this slice): the two nvn_texdesc layouts differ
//   (ours: `signed int pkg_block_h:8; unsigned swizzle_r:3...`; nx2: `int pkg_block_h;`), so the moment a
//   real guest texture is deswizzled, the struct handed to this function must be the layout it was compiled
//   against. Slice 1 never creates a tex_t, so this path is unreachable here.
#pragma once
#include <stddef.h>
#include "abi/nvn_tex.h"

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_storage, void *dst_linear, size_t dst_pitch);
