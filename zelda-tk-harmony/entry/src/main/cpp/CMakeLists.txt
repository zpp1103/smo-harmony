cmake_minimum_required(VERSION 3.22)
project(game C CXX ASM)

set(CMAKE_C_STANDARD 17)
set(NODE_MODULE libgame)

# OHOS Vulkan：NDK 自带 vulkan 头与 libvulkan.so
find_library(VULKAN_LIB vulkan)
find_library(HLOG_LIB hilog_ndk.z)

add_library(${NODE_MODULE} SHARED
  game_napi.cpp
  nvn_vk/nvn_vk_stub.cpp
)

target_include_directories(${NODE_MODULE} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  # 周末移植：MacNativeGames/games/odyssey/runtime 的 C 源码放 host/ 后取消下一行
  # ${CMAKE_CURRENT_SOURCE_DIR}/host
)

target_link_libraries(${NODE_MODULE} PUBLIC
  ${VULKAN_LIB}
  ${HLOG_LIB}
  EGL
  GLESv3
)

# 游戏代码镜像（smo_build/patched/*/​*.patched.bin）经 embed.S 嵌入：
# 周末定 RomFS/binary 分发方式后启用（HAP 内嵌 or 沙箱拷贝）。
# target_sources(${NODE_MODULE} PRIVATE embed.S)
# target_compile_definitions(${NODE_MODULE} PRIVATE GAME_BIN_DIR="${GAME_BIN_DIR}")
