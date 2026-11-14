// nvn_vk_stub.cpp —— 编译占位：今晚保证 libgame.so 可出包；周末按 nvn_vk.h 分块实装
#include "nvn_vk.h"
#include <cstdio>
#include <cstdint>
#include <stddef.h>
#include "../third_party/d2r_gfx/gfx/nvn/nvn_tex.h"   /* nvn_texdesc（nvn_tex.h:6）：nvn_swizzle 桩签名用；原型头 nvn_deswizzle.h 不能直接 include——C++ 下会给 nvn_swizzle 以 C++ 链接声明，与本处 extern "C" 定义冲突 */

namespace nvn_vk {
bool init(void *native_window, uint32_t width, uint32_t height) {
    (void)native_window; (void)width; (void)height;
    return false; // 未接通
}
void shutdown() {}
void present_frame() {}
} // namespace nvn_vk

// ---- NVN Metal 后端符号桩（宿主 runtime 引用；周末由 nvn_vk 后端提供真实现） ----
extern "C" {
int mtl_init(void) {
    fprintf(stderr, "[ohos-shim] mtl_init -> Vulkan backend not wired yet\n");
    return -1;
}
unsigned long long mtl_present_count(void) { return 0; }
void mtl_wait_state_dump(void) {}

// 图形诊断/状态变量（Metal 后端定义；nvn_vk 实装后由后端接管）
// round24 根因：dm_host_write_pc 在 donor（d2r_gfx nvn/nvn_dirty.c:87）是 uint64_t 数据——
// gfx_bridge.c 的 dm_guest_write_prepare 在每次宿主写客体内存前往里存写者 PC，RomFS 读文件
// 也走它；此前误写成函数桩，符号落只读 .text，首次 runtime_romfs_read 即 str x30,[符号地址]
// sig=11、fault=符号自身地址（round24.log pc=dm_guest_write_prepare+0x20）。改为数据定义。
// mfxup_enabled 同病反向：donor totk_mfx_up.h:15 是函数、game_patches.c:222 直接调用，
// 数据桩会被当代码跳进去执行。totk_vpool_backing/nvn_swizzle 桩签名对齐 donor 头
// （nvn/totk_vpool.h:26、tex/nvn_deswizzle.h:19），返回值语义不变（0=无后备/成功）。
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
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_linear, size_t src_pitch, void *dst_storage)
{ (void)d; (void)level; (void)layer; (void)src_linear; (void)src_pitch; (void)dst_storage; return 0; }
}
