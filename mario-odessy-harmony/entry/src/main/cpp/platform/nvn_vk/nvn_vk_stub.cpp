// nvn_vk_stub.cpp —— 宿主符号桩 + Vulkan 后端弱挂钩（M4 第一批起与 nvn_vk.c 共存）
// 职责一：platform/*.c 引用的 Metal 后端符号仍在此兜底（真实现逐步由 nvn_vk.c/nvn_vk.h 提供）；
// 职责二：mtl_init 经弱符号尝试新 Vulkan 后端（arm64 分支链接 nvn_vk.c 真身；x86_64 壳层
//         弱符号为 NULL 自动回落桩行为），起层与否只影响日志一行，宿主行为不变（gfx_bridge.c:89
//         不看 mtl_init 返回值，实测无行为耦合）。
// 第二批接线（主线 gfx_bridge 稳定后）：mtl_present/mtl_reg_tex 等真实现迁入 nvn_vk.c 时，
// 从本文件删除对应桩即可（nvn_vk.h 已备好 C 接口）。
#include <cstdio>
#include <cstdint>
#include <stddef.h>
#include "nvn_vk.h"   /* 第一批后的对外 C 接口（nvn_vk_init/nvn_vk_set_native_window 等；尾巴①接线点） */
#include "../../third_party/d2r_gfx/gfx/nvn/nvn_tex.h"   /* 迁入 platform/nvn_vk/ 深度 +1 改 ../../（nvn_vk.h:19 同款）；round23批2 起 nvn_swizzle 桩已摘（真身 nvn_deswizzle.c），本头仅余 nvn_texdesc 等 desc 类型可见 */

// arm64 链 nvn_vk.c 真身；x86_64 壳层无定义 → 弱符号 NULL，走桩
extern "C" __attribute__((weak)) int nvn_vk_init(void);

extern "C" {
// M43A / S2a：mtl_init 的让位结束 —— nvn_vk2/gfx_vk_core.c 里现在是唯一真身（它调同一个 nvn_vk_init，
// 行为与这里逐字等价）。保留 x86_64 壳层（不编 nvn_vk2/*，没人定义 mtl_init 会漏符号）的兜底版本。
#ifndef HOST_ARCH
int mtl_init(void) {
    if (nvn_vk_init != nullptr) { return nvn_vk_init() ? 0 : -1; }
    fprintf(stderr, "[ohos-shim] mtl_init -> Vulkan backend not linked (x86_64 shell)\n");
    return -1;
}
#endif
unsigned long long mtl_present_count(void) { return 0; }
void mtl_wait_state_dump(void) {}

// 图形诊断/状态变量（Metal 后端定义；nvn_vk 实装后由后端接管）
// round24 根因：dm_host_write_pc 在 donor（d2r_gfx nvn/nvn_dirty.c:87）是 uint64_t 数据——
// gfx_bridge.c 的 dm_guest_write_prepare 在每次宿主写客体内存前往里存写者 PC，RomFS 读文件
// 也走它；此前误写成函数桩，符号落只读 .text，首次 runtime_romfs_read 即 str x30,[符号地址]
// sig=11、fault=符号自身地址（round24.log pc=dm_guest_write_prepare+0x20）。改为数据定义。
// mfxup_enabled 同病反向：donor totk_mfx_up.h:15 是函数、game_patches.c:222 直接调用，
// 数据桩会被当代码跳进去执行。totk_vpool_backing 桩签名对齐 donor 头
// （nvn/totk_vpool.h:26），返回值语义不变（0=无后备/成功）。nvn_swizzle 桩批2 已摘。
unsigned long long g_frame_pub = 0;
int dm_submit_busy = 0;
unsigned long long dm_host_write_pc = 0;
int mfxup_enabled(void) { return 0; }
void dm_gfx_set_flag(const char *name, int value) { (void)name; (void)value; }
void dm_nvn_probe_commands(void) {}
void dm_nvn_probe_copy_marks(void) {}
void dm_nvn_probe_deferred_binds(void) {}
void dm_nvn_probe_inline_bulk(void) {}
void dm_nvn_probe_prediction(void) {}
void dm_nvn_probe_present_interval(void) {}
void dm_nvn_probe_texture_pins(void) {}
int totk_vpool_backing(uintptr_t va, uint64_t len, uintptr_t *cpu) { (void)va; (void)len; (void)cpu; return 0; }
/* nvn_swizzle 假桩已摘（round23批2）：真身=third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c（arm64 编入，
   同时定义 nvn_deswizzle/nvn_swizzle/nvn_bl_offset/nvn_deswizzle_geom）；本文件残留空桩即重复符号。
   x86_64 壳层不编 gfx_probe.c/nvn_vk.c，无引用点，不需要桩。 */
}
