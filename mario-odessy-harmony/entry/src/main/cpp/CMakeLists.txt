cmake_minimum_required(VERSION 3.22)
project(game C CXX ASM)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# OHOS CMake 会给目标名加 lib 前缀：NODE_MODULE 必须是 game（产物 libgame.so），
# 与 Index.ets import 'libgame.so'、XComponent libraryname:'game'、nm_modname "game" 对齐。
# 此前写 libgame 产物为 liblibgame.so，导致 MUSL-LDSO: load libgame.so failed errno=2（boot_round1）。
set(NODE_MODULE game)

# 游戏选择单变量：platform/ = 引擎（Switch 系统服务/装载/HLE/兼容层，换游戏零改动），
# games/<tid> = 游戏专属（镜像表/补丁/版本身份/标题自动化 + game_env.c）。
# TOTK 落地时唯一必改行：把目录名换成新 title-id。
set(GAME_DIR games/0100000000010000)

# OHOS Vulkan：NDK 自带 vulkan 头与 libvulkan.so
find_library(VULKAN_LIB vulkan)
find_library(HLOG_LIB hilog_ndk.z)
find_library(ACE_NAPI ace_napi.z)
# round7：虚拟默认输出设备的真机出口（ohos_stub.c AudioUnit 段 → OH_AudioRenderer 写端）
find_library(OHAUDIO_LIB ohaudio)

# 平台引擎源码：Switch 系统服务/装载引擎/HLE/兼容层（游戏无关）；opus_codec 需 libopus，电影音频周末接 oh_audio
# x86_64（模拟器）：游戏本体是 ARM64 重写镜像无法执行，壳层验证专用（runtime_main 返回 2）
file(GLOB HOST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/*.c)
list(FILTER HOST_SOURCES EXCLUDE REGEX "opus_codec\\.c$")

# 游戏侧实现（autoload/game_patches/odyssey_aspect/game_env），随 GAME_DIR 切换；换游戏时改 GAME_DIR 单变量即可
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${GAME_DIR}/*.c)

if(OHOS_ARCH STREQUAL "arm64-v8a")
  add_library(${NODE_MODULE} SHARED
    platform/game_napi.cpp
    platform/nvn_vk/nvn_vk_stub.cpp
    platform/nvn_vk/nvn_vk.c
    third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c   # round23批2 收编尾巴③：块线性解排布真身（树内 sha256 d8f66514…，donor 989859a8…，以树内为准）；同摘 ohos_stub.c/nvn_vk_stub.cpp 两假桩防重复符号
    platform/ohos_compat/ohos_stub.c
    platform/hle_stack.S
    platform/entry.S
    platform/nvn_replay/cmdbuf.c                  # M2：NVN 命令录制（infra/BOTWRecompAndroid@4bad6dc，MIT）
    platform/nvn_replay/maxwell/translate.c       # M2：Maxwell 命令流翻译（同上）
    platform/nvn_replay/plat_ohos.c               # M2：NVN 层所需 plat_*（OHOS 版）
    platform/nvn_replay/cmd_trace.c               # M2：命令流跟踪器（opcode 直方图）
    platform/nvn_replay/glue_cmdbuf.c             # M2：参数式桩 → Cpu* 式实现的胶水
    platform/nvn_replay/host_stubs.c              # M2：未移植符号的宿主桩（guest_call / nvn_watch_texid）
    # ---- M42A / S1：nx2android gfx_vk_* 后端切片（docs/m41-fork-plan.md §7.1 切片 1）----
    # 物理隔离在 nvn_vk2/（含 nvn_vk2/abi 独立 ABI 头），绝不覆盖 third_party/d2r_gfx：
    # 两边 nvn_obj.h 的 op 枚举已分叉（我方 OP_COUNT_=43 / nx2=51），混头会静默错画。
    # 本切片不进 gfx_vk_exec.c（录制层 S2）与 gfx_vk_present.c（present S4）。
    nvn_vk2/gfx_vk_core.c
    nvn_vk2/gfx_vk_tex.c
    nvn_vk2/gfx_vk_shader.c
    nvn_vk2/gfx_vk_subgroup.c
    nvn_vk2/gfx_vk_dq.c
    nvn_vk2/nvn_dirty.c
    nvn_vk2/nvn_bc.c
    nvn_vk2/ohos_vk_boot.c                        # M42A 新写：direct-driver-loading + OHOS 隐式层代理
    nvn_vk2/ohos_dm_stub.c                        # M42A 新写：28+ 宿主钩子空桩（待 S2/S5）
    # M43A / S2a：录制层（nx2android hle_nvn + exec）搬入 —— **只录不跑**，执行仍走 nvn_vk.c 直喂。
    # rec/ 是物理隔离区：只看得见 nvn_vk2/abi/* 与自己的 shim，绝不 include third_party/d2r_gfx 的同名头
    # （两边 OP_ 枚举 OP_COUNT_ 50 vs 43）。rec/ 的 include 目录与本文件其余部分**不共享**（见下方
    # set_source_files_properties），否则 rec/nvn.h 之类的 shim 会反向污染 platform/*.c。
    nvn_vk2/rec/hle_nvn.c                         # 搬入：nx2 录制层接口层（393 实现 + 537 名表，static，靠 swnvn 段暴露）
    nvn_vk2/rec/gfx_vk_exec.c                     # 搬入：nx2 回放层（2495 行，本单只编进来、不驱动）
    nvn_vk2/rec/gfx_common.c                      # 搬入：Android 后端统计/诊断零值实现
    nvn_vk2/rec/nvn_tex_gobs.c                    # 新写：nvn_tex_level_gobs（我方树缺这一支），nx2 nvn_tex.c 前缀化包一层
    nvn_vk2/rec/m43b_rec_tap.c                    # M43B 新写：全量镜像（draws + 状态类）+ 帧内缓冲整帧对账（取代 m43a_rec_tap.c，后者留档不编译）
    nvn_vk2/rec/m43a_s4_stubs.c                   # 新写：仅 present(S4)/crash 两类，强符号（撞车即失败，不静默）
    nvn_vk2/rec/m45a_switch.c                     # M45A 新写：执行路径切换（smo_m45a；nvn_lookup → nx2 跳板）+ 证据行 + RT dump 驱动
    nvn_vk2/rec/m47a_present.c                    # M47A 新写：S4-lite（nx2 合成 RT → win_fill.c 既有 offscreen 上屏链）+ 红偏探针
    nvn_vk2/rec/m48a_probe.c                      # M48A 新写：色调修复开关 + 探针（缺陷 A 单层 R/B、缺陷 B passdump/sRGB 判别）
    # M43B / S2b-1：ELF 版跳板。**.S 刻意不进 M43A_REC_SRC**（那组会 force-include darwin_compat.h，汇编吃不了）；
    # rec/compat/include 也不给它们 —— 这两个文件是纯汇编、零 include。生成方式见各自文件头 banner。
    nvn_vk2/rec/nvn_thunks_elf.S                  # 生成：nx2 gen_protos.py + dm/android/tools/darwin2elf.py（537 条跳板）
    nvn_vk2/rec/nvn_entry_elf.S                   # 生成：同上（nvn_unimpl_entry / nvn_prof_entry / nvn_big_entry）
    nvn_vk2/rec/nvn_bigstack_elf.c                # 新写：dm_bigstack_enter 的直通实现（不切大栈，S2b-2 再补）
    nvn_vk2/rec/compat/darwin_compat.c            # 搬入：dm/android/compat（Darwin→Linux/OHOS 垫层）；sha256.c 与 darwin_symbols.c
                                                  # **不进列表**：CC_SHA256_* 由 platform/ohos_compat/ohos_stub.c 提供、
                                                  # mach_absolute_time 由 platform/loader.c 提供（重复符号处置）
    nvn_vk2/selftest_tri.c                        # M42A 新写：固定 PSO 满屏三角形 + offscreen PPM
    third_party/zstd/zstddeclib.c                 # nx2 同源单文件 zstd 解压（Meta BSD/GPLv2 双许可）
    ${HOST_SOURCES}
    ${GAME_SOURCES}
  )
  # nvn_replay/ 目前只编译、未接线：gfx_bridge 的符号表仍指向记账桩。
  # 接线（符号表逐项转发 + nvn_cmd_trace 直方图）在真机验证窗口做，见 PORTING.md。
  # nvn_vk.c 只进 arm64：它链接 platform/nvn_tex.c 的布局数学（nvn_fmt_bpb/nvn_tex_layout），
  # 而 platform/*.c glob 只在真机分支生效；x86_64 壳层保持 nvn_vk_stub.cpp 单桩（弱符号回落）。
  # M4 第一批：device/surface/swapchain/纹理上传；窗口接线（game_napi surface_set →
  # nvn_vk_set_native_window）与 gfx_bridge 的 present 清色链已接（round23批1）；
  # mtl_* 全量接线在批 3（docs/round23-nvn-wiring-plan.md）。
  set(HOST_ARCH 1)
else()
  add_library(${NODE_MODULE} SHARED
    platform/game_napi.cpp
    platform/nvn_vk/nvn_vk_stub.cpp
    platform/ohos_compat/ohos_stub.c
  )
endif()

if(HOST_ARCH)
  target_compile_definitions(${NODE_MODULE} PRIVATE HOST_ARCH=1)
endif()

# M43A / S2a：rec/ 的隔离编译域。只在 rec 源文件上追加 include/宏，**不动**整个 target 的包含路径：
#  · rec/（自己的 shim：nvn_tex.h/nvn_obj.h/nvn.h/... → ../abi/*，dm.h 用 nx2 原版 146 行）
#  · rec/gen（gfx_vk_exec 要的 8 个 .spv.h，m43a 用 glslangValidator 重生成）
#  · rec/compat/include（Darwin 头垫层）+ force-include darwin_compat.h
set(M43A_REC_SRC
  nvn_vk2/rec/hle_nvn.c nvn_vk2/rec/gfx_vk_exec.c nvn_vk2/rec/gfx_common.c
  nvn_vk2/rec/nvn_tex_gobs.c nvn_vk2/rec/m43b_rec_tap.c nvn_vk2/rec/m43a_s4_stubs.c
  nvn_vk2/rec/m45a_switch.c
  nvn_vk2/rec/m47a_present.c
  nvn_vk2/rec/m48a_probe.c
  nvn_vk2/rec/nvn_bigstack_elf.c
  nvn_vk2/rec/compat/darwin_compat.c)
foreach(_s ${M43A_REC_SRC})
  set_source_files_properties(${_s} PROPERTIES
    INCLUDE_DIRECTORIES
      "${CMAKE_CURRENT_SOURCE_DIR}/nvn_vk2/rec;${CMAKE_CURRENT_SOURCE_DIR}/nvn_vk2/rec/gen;${CMAKE_CURRENT_SOURCE_DIR}/nvn_vk2/rec/compat/include"
    COMPILE_OPTIONS
      "-include;${CMAKE_CURRENT_SOURCE_DIR}/nvn_vk2/rec/compat/include/darwin_compat.h;-DM43A_REC_TU=1;-DDM_ANDROID=1;-D_GNU_SOURCE")
endforeach()

# shim 层：伪造 Apple 头（include 最优先）+ donor nvn 头 + 平台/游戏生成的绑定头
# （platform/generated=shared_fonts 系统资源；games/<tid>/{,generated}=runtime_tables/romfs_config/game_version/game_patches——
#  platform 的 loader.c:21-23、services.c:5、romfs.c:5、fs_host.c:29、swkbd_host.c:200 按名跨目录消费，games 侧经 -I platform 解析 runtime.h）
target_include_directories(${NODE_MODULE} PRIVATE
  # M42A：nvn_vk2 的 ABI 头**不**进全局 include path（否则会顶掉我方 platform/*.c 用的 d2r_gfx 同名头，
  # 反向污染老后端）。nvn_vk2 内部一律用 "abi/xxx.h" 相对引用；gfx_vk.h 里另有 _Static_assert(OP_COUNT_ == 50) 锚定。
  ${CMAKE_CURRENT_SOURCE_DIR}/nvn_vk2
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/ohos_compat/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/d2r_gfx/gfx/nvn/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/d2r_gfx/gfx/nvn
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/generated
  ${CMAKE_CURRENT_SOURCE_DIR}/${GAME_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/${GAME_DIR}/generated
  ${CMAKE_CURRENT_SOURCE_DIR}/platform
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_options(${NODE_MODULE} PRIVATE
  -include platform/ohos_compat/ohos_compat.h
  -Wno-unknown-pragmas -Wno-unused-function
)

target_link_libraries(${NODE_MODULE} PUBLIC
  ${VULKAN_LIB}
  ${HLOG_LIB}
  ${ACE_NAPI}
  ${OHAUDIO_LIB}
  ace_ndk.z
  EGL
  GLESv3
  native_window   # CPU present 回落（OH_NativeWindow 填色）：模拟器无 Vulkan ICD 时的出图路径
)

# 游戏代码镜像（smo_build/patched/*/​*.patched.bin）经 platform/embed.S 嵌入：
# 待定 RomFS/binary 分发方式后启用；GAME_BIN_DIR 定义点已移交 games/<tid> 侧（game_env.c 或
# per-game cmake 片段），platform 不再持有游戏镜像路径。
# target_sources(${NODE_MODULE} PRIVATE platform/embed.S)
# target_compile_definitions(${NODE_MODULE} PRIVATE GAME_BIN_DIR="${GAME_BIN_DIR}")
