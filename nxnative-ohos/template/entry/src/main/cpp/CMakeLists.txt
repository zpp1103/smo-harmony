cmake_minimum_required(VERSION 3.22)
project(game C CXX ASM)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# OHOS CMake 会给目标名加 lib 前缀：NODE_MODULE 必须是 game（产物 libgame.so），
# 与 Index.ets import 'libgame.so'、XComponent libraryname:'game'、nm_modname "game" 对齐。
# 此前写 libgame 产物为 liblibgame.so，导致 MUSL-LDSO: load libgame.so failed errno=2（boot_round1）。
set(NODE_MODULE game)

# OHOS Vulkan：NDK 自带 vulkan 头与 libvulkan.so
find_library(VULKAN_LIB vulkan)
find_library(HLOG_LIB hilog_ndk.z)
find_library(ACE_NAPI ace_napi.z)

# 宿主源码：MacNativeGames games/odyssey/runtime（1.4.1 匹配）；opus_codec 需 libopus，电影音频周末接 oh_audio
# x86_64（模拟器）：游戏本体是 ARM64 重写镜像无法执行，壳层验证专用（game_napi 返回 2）
file(GLOB HOST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/host/*.c)
list(FILTER HOST_SOURCES EXCLUDE REGEX "opus_codec\\.c$")

if(OHOS_ARCH STREQUAL "arm64-v8a")
  add_library(${NODE_MODULE} SHARED
    game_napi.cpp
    nvn_vk/nvn_vk_stub.cpp
    ohos_compat/ohos_stub.c
    host/hle_stack.S
    host/entry.S
    ${HOST_SOURCES}
  )
  set(HOST_ARCH 1)
else()
  add_library(${NODE_MODULE} SHARED
    game_napi.cpp
    nvn_vk/nvn_vk_stub.cpp
    ohos_compat/ohos_stub.c
  )
endif()

if(HOST_ARCH)
  target_compile_definitions(${NODE_MODULE} PRIVATE HOST_ARCH=1)
endif()

# shim 层：伪造 Apple 头（include 最优先）+ 生成的绑定头（runtime_tables 等）+ 强制注入 mach→OHOS 映射
target_include_directories(${NODE_MODULE} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/ohos_compat/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/d2r_gfx/gfx/nvn/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/d2r_gfx/gfx/nvn
  ${CMAKE_CURRENT_SOURCE_DIR}/generated
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_options(${NODE_MODULE} PRIVATE
  -include ohos_compat/ohos_compat.h
  -Wno-unknown-pragmas -Wno-unused-function
)

target_link_libraries(${NODE_MODULE} PUBLIC
  ${VULKAN_LIB}
  ${HLOG_LIB}
  ${ACE_NAPI}
  ace_ndk.z
  EGL
  GLESv3
)

# 游戏代码镜像（smo_build/patched/*/​*.patched.bin）经 embed.S 嵌入：
# 待定 RomFS/binary 分发方式后启用。
# target_sources(${NODE_MODULE} PRIVATE embed.S)
# target_compile_definitions(${NODE_MODULE} PRIVATE GAME_BIN_DIR="${GAME_BIN_DIR}")
