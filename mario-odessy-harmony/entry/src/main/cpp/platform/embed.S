// embed.S —— 把转译后的游戏代码镜像嵌入 libgame.so（宿主启动时 mmap 到 Switch 地址布局）
// 启用条件：周末确定分发方式（HAP 内嵌 vs 沙箱拷贝）后，
// 由 CMake 传入 GAME_BIN_DIR（指向 game-work/smo_build/patched），取消下列注释。
// 注意：main.patched.bin 21.8MB，BIG_TEXT 布局由 patch_nso 已处理。

#if 0
    .section .rodata.game,"a"
    .global g_bin_main, g_bin_main_end
g_bin_main:
    .incbin GAME_BIN_DIR "/main/main.patched.bin"
g_bin_main_end:
    .global g_bin_sdk, g_bin_sdk_end
g_bin_sdk:
    .incbin GAME_BIN_DIR "/sdk/sdk.patched.bin"
g_bin_sdk_end:
    .global g_bin_subsdk0, g_bin_subsdk0_end
g_bin_subsdk0:
    .incbin GAME_BIN_DIR "/subsdk0/subsdk0.patched.bin"
g_bin_subsdk0_end:
    .global g_bin_rtld, g_bin_rtld_end
g_bin_rtld:
    .incbin GAME_BIN_DIR "/rtld/rtld.patched.bin"
g_bin_rtld_end:
#endif
