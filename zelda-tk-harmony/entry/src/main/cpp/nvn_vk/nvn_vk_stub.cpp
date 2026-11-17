// nvn_vk_stub.cpp —— 编译占位：今晚保证 libgame.so 可出包；周末按 nvn_vk.h 分块实装
#include "nvn_vk.h"

namespace nvn_vk {
bool init(void *native_window, uint32_t width, uint32_t height) {
    (void)native_window; (void)width; (void)height;
    return false; // 未接通
}
void shutdown() {}
void present_frame() {}
} // namespace nvn_vk
