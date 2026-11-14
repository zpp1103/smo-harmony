// nvn_vk.h —— NVN 图形 API → Vulkan 后端（骨架）
// 参考：nxnative dm/gfx（NVN slots 与行为）、MacNativeGames Metal 实现（行为等价物）
// OHOS 提供 Vulkan 1.x + libvulkan.so；游戏侧只见到 NVN 函数面。
#pragma once
#include <vulkan/vulkan.h>

namespace nvn_vk {

// 初始化：从 XComponent 拿到 OHNativeWindow*，建 instance/device/swapchain
// （OHOS 上用 VK_OHOS_surface 扩展，头文件在 NDK vulkan/vulkan_ohos.h）
bool init(void *native_window, uint32_t width, uint32_t height);
void shutdown();

// 帧提交：宿主每帧调用；重放录制好的 VkCommandBuffer
void present_frame();

// 以下为周末实装的分块（对应 NVN 函数族）：
// - texture:   nvnTextureInit*  -> VkImage（BC1/3/5/7 走 VK_EXT_bc 或转码回退）
// - sampler / buffer / memory（别名与 pool，对齐 Switch 布局）
// - shader:    游戏着色器已是 SPIR-V（Ryujinx 翻译器产物），直接 VkShaderModule
// - pipeline:  预烘焙到磁盘缓存（首次进新场景会卡一下，与 nxnative 行为一致）
// - command:   NVN 命令流 -> VkCommandBuffer 重放
// - 同步:      对齐 Atmosphère 语义（fence/semaphore + 宿主自管队列）

} // namespace nvn_vk
