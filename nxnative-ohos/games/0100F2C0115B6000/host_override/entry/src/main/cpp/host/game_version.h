/* Tears of the Kingdom 1.1.2 (v262144), main build ID 9a10ed9435c06733da597d8094d9000a —— 鸿蒙档案
   host_override 整文件覆盖（数据类差异；scripts/build_hap_from_profile.py 在实例化时拷入构建区）。
   出处：MacNativeGames games/totk/runtime/game_version.h 的 TOTK_GAME_V112 块（2026-10-31 摘取）。
   适用性已核实：本机 game-work/totk_0100F2C0115B6000/ExeFS 五件 SHA256 与 MacNativeGames
   expected-v1.1.2.json 逐一相符（main f04fbebd… / rtld 28317605… / sdk 91573d44… / subsdk0 9d04c989… /
   main.npdm 0b1f6cf9…），故下表对本地 1.1.2 镜像直接成立（Mac 侧 1.1.2 已验证）。
   GAME_ADDR 语义：键是 1.0.0 的原始偏移、值是本版本对应偏移——host_override/game_patches.c 的运行时补丁
   用 GAME_ADDR(0x…) 取本版本位点；每处写入前都先校验原指令字，不匹配即 runtime_fail 停机（不盲写）。 */
#pragma once
#define TOTK_GAME_VERSION "1.1.2"
#define TOTK_GAME_DIR "v112"
#define GAME_TITLE_ID 0x0100F2C0115B6000ULL
/* Survey (world_survey)：actor 字段位（1.1.2/1.2.1/1.4.3 同）——world_survey.c OHOS 化后使用 */
#define GAME_ACTOR_NAME 0x218
#define GAME_ACTOR_POS 0x2b4
#define GAME_ACTOR_ROT 0x2c0
#define GAME_ADDR_JOIN(x) GAME_ADDR_##x
#define GAME_ADDR(x) GAME_ADDR_JOIN(x)
/* 60 FPS（game_patches.c fps60_sites）：自适应 present-interval 控制器把下限 2（30fps）降到 1（60fps） */
#define GAME_ADDR_0x8f6c60 0x8f66e4
#define GAME_ADDR_0x8f6c64 0x8f66e8
#define GAME_ADDR_0x8f6c88 0x8f670c
#define GAME_ADDR_0x8f6d18 0x8f679c
#define GAME_ADDR_0x8f6d28 0x8f67ac
#define GAME_ADDR_0x8f6d7c 0x8f6800
#define GAME_ADDR_0x8f6d98 0x8f681c
/* 高帧率修正的帧率步进读点（real-time frame-step，待 OHOS 化后使用） */
#define GAME_ADDR_0x7a83f4 0x7f7a38
#define GAME_ADDR_0x7a83f8 0x7f7a3c
#define GAME_ADDR_0x7a83fc 0x7f7a40
#define GAME_ADDR_0x7a8400 0x7f7a44
#define GAME_ADDR_0x7a840c 0x7f7a50
#define GAME_ADDR_0x7a8410 0x7f7a54
#define GAME_ADDR_0x8f68b8 0x8f633c
#define GAME_ADDR_0x8f68bc 0x8f6340
#define GAME_ADDR_0x8f68cc 0x8f6350
#define GAME_ADDR_0x8f68d0 0x8f6354
/* 输出分辨率（TOTK_OUTPUT_RES，patch_output_res——待 OHOS 化后使用） */
#define GAME_ADDR_0xc630bc 0xcdd3e8
#define GAME_ADDR_0xc630c4 0xcdd3f0
#define GAME_ADDR_0xc630c8 0xcdd3f4
#define GAME_ADDR_0xe996ac 0xea7efc
#define GAME_ADDR_0xe996b0 0xea7f00
#define GAME_ADDR_0xc630a4 0xcdd3d0
#define GAME_ADDR_0xc630a8 0xcdd3d4
#define GAME_ADDR_0xc630ac 0xcdd3d8
/* 杂项：survey 矩阵/焦点/软键盘 PLT+GOT/慢动作蹦床检查 */
#define GAME_ADDR_0x69a18c 0x70376c
#define GAME_ADDR_0x15ecb0c 0x1627f10
#define GAME_ADDR_0xe995c4 0xea7e14
#define GAME_ADDR_0x4663f08 0x4725058
#define GAME_ADDR_0x2aa4fa0 0x2b16100
#define GAME_ADDR_0x453f648 0x46129e0
#define GAME_ADDR_0x8f6cbc 0x8f6740
#define GAME_ADDR_0xce3fec 0xbbb4c0   /* trampoline slow-motion check (game_patches.c patch_slowmo_bounce) */
#define GAME_OUTPUT_CALL_WORD 0x94000083u
