# 0100F2C0115B6000 —— The Legend of Zelda: Tears of the Kingdom（鸿蒙档案笔记）

塞尔达王国之泪。**本档案是 1.1.2（v262144）derived profile**：nxnative 上游注册 1.4.3（v720896），
本机转译产物是 1.1.2。键值出处与核对记录见 `game.tsv` 各组注释；本 README 记：为什么档案长这样、
host_override 放了什么/没放什么、以及待真机阶段验证清单（M3 后半）。

## 版本事实（2026-10-31 核验）

- 本地 `game-work/totk_0100F2C0115B6000/ExeFS/` 五件（main/rtld/sdk/subsdk0/main.npdm）SHA256 与
  MacNativeGames `games/totk/assets/expected-v1.1.2.json`（已拷入本目录）**逐一相符**——本地 1.1.2
  与 Mac 侧已验证的 1.1.2 逐字节同源，Mac 的 TOTK_GAME_V112 位点表对本镜像直接成立。
- 本地 RomFS.img 17,045,762,015 B（16 卷 @1GiB）**小于** Mac pin 记录的 17,051,988,992 B（差 ~6MB；
  ExeFS 却全同）。两者谁的抽取更完整未查——待真机 RomFS 校验阶段对齐（prepare 侧 IMAGE_SHA 以本地镜像为准）。
- 重写状态：`game-work/totk_build/patched/` 四模块 cuts/obs 全为空表头——1.1.2 零指令裁剪。
  nxnative 1.4.3 的 FRAME_STEP 按 build ID 键控，未命中 1.1.2；sdk（b9046c31…）也不在
  dm/tools/sdk_patches.py 的 CUTS 表。

## host_override 清单（放入 4 件，均为数据/参数类）

| 文件 | 内容 | 依据 |
|---|---|---|
| `entry/src/main/cpp/host/game_version.h` | TOTK_GAME_VERSION "1.1.2" + TOTK_GAME_DIR + GAME_TITLE_ID + **V112 已验证地址表**（GAME_ADDR 宏族：60fps 位点、帧步进读点、输出分辨率位点、慢动作蹦床位、swkbd PLT/GOT 等） | MacNativeGames game_version.h 的 TOTK_GAME_V112 块；本地 ExeFS 哈希全同故直接适用 |
| `entry/src/main/cpp/host/game_patches.c` | 运行时补丁最小 OHOS 移植：fps60_sites 7 条（指令字校验后写入）+ 高帧率蹦床 veneer + TOTK_TRAP_AT 诊断；其余钩子（frame_step/output_res/aspect/survey）无操作收口 | MacNativeGames totk game_patches.c 的可移植子集；见文件头"已移植/待 OHOS 化"注释 |
| `AppScope/resources/base/media/icon.jpg` | **应用图标（HAP 里生效的是这份**——pack 时 AppScope 图标覆盖 entry 同名图标，只换 entry 侧不生效，已实测） | game-work/totk_0100F2C0115B6000/icon.jpg |
| `entry/src/main/resources/base/media/icon.jpg` | entry 模块图标（startWindowIcon 等引用） | 同上 |

## 暂缓清单（代码类差异，"待 OHOS 化"，勿盲拷 Mac 版进构建）

MacNativeGames totk runtime 72 文件 vs odyssey runtime 59：55 同名中仅 19 逐字节相同。
**鸿蒙宿主模板是 odyssey 血统的 OHOS 移植**，Mac 侧 totk 差异文件含 macOS 专属调用
（TargetConditionals、frontend.m 设置面板、iOS 导出分支等），逐文件人工 OHOS 化后才能进 override。

- **totk 独有 17 文件**：`game_patches_v143.c`/`game_patches_botw.c`（非本版本，不需要）、
  `game_patches.c`（已取可移植子集，其余见下）、`world_survey.c`+`world_survey.h`（测试剖面，
  含 runtime_bench_cam——模板 world_survey.h 无此符号，随 world_survey 一起移植）、
  `aspect_patch.h`+`aspect_math.h`（超宽屏；上游 1.1.2 本就不支持，优先级低）、
  `aoc_romfs.c`（DLC RomFS 挂载）、`game_data.c/h`（存档数据布局）、`settings_service.c`/
  `settings_panel.h`（Mac 设置面板服务）、`game_language.h`、`bow_state.h`、`contrib_submit.h`、
  `feedback_settings.h`、`hid_imu.h`、`mem_scan.h`、`switch_left_fallback.h`、`text_keyboard.h`。
- **36 个同名差异文件**（totk vs odyssey，按影响排序）：`services.c`（内存布局/GetInfo 面，~152 行差异——
  内存键已进 game.tsv，M4+ 参数化注入后宿主侧收敛）、`loader.c`（~199 行，含 per-version 数据页/补丁区）、
  `threads.c`（~659 行，调度差异）、`runtime.h`（~74 行，钩子面差异——模板钩子面是 odyssey 版）、
  `romfs.c`（~106 行，overlay/aoc）、`fs_host.c`、`account.c`、`applet_host.c`、`input.c`、`ipc.c/h`、
  `movie_host.c/h`、`swkbd_host.c`、`time_host.c`、`shared_font.c`、`game_version.h`（已按数据类先行）、
  `game_patches.c`（已取子集）、`world_survey.c/h`、`gamepad.m/.h` 系、`frontend*.m`、`gfx_probe.c`、
  `gpu_path_probe.m`、`hid_feedback.c`、`motion_fusion.c`、`audio_mixer/renderer`、`autoload.c`、
  `controller_map.h`、`gamepad_layout.h`、`mouse_look.h`、`aspect_math.h`。
- **game_patches.c 内未移植的特性块**（依赖上述文件或 Mac 语义）：patch_realtime+patch_cloth_clock
  （真实帧步进 + 布料/毛发时钟；需 loader 数据页集成——OHOS loader 已留 0x4000 页，M3 真机阶段移植）、
  patch_render_res/patch_output_res（TOTK_RENDER_RES/TOTK_OUTPUT_RES）、patch_pchtxt、
  1.2.1 的 cursor_speed/bench_cam（非本版本）。

## 待真机阶段（M3 后半）验证清单

0. **签名**：DevEco Signing Configs → Fix 为 com.vibecc.totk 生成调试签名材料后重跑构建（见文末签名说明）。
1. **boot 序列**：rawfile 四件 + runtime_tables（gates 862 条 spin）能否过 nninitStartup——重点看
   `SetHeapSize(0x86000000)` 后的 alias 布局（mem_reserve 0x140000000 / mem_alias 0x98000000 是 1.4.3
   heap check 产物，1.1.2 未实测）。
2. **sdk node area 未裁剪**：1.1.2 sdk 无 1GB→62MB 裁剪，若初期下洞遇 MSF/血月回位（内存不足），
   为 sdk b9046c31… 定位 AddressRegionManager 构造子同型裁剪（1.4.3 在 sdk+0x145d0c/0x145d78，
   1.1.2 偏移需在镜像里找 `08fca752`/`09fca752` 型 mov 对）。
3. **60fps 运行时补丁**：fps60_sites 7 条 + 蹦床 veneer 的指令字校验是否全过（本地 ExeFS 与 Mac 同源，
   理论必过；若重写器将来动了这些位点的字节则停机报告）；TOTK_FPS60=0 可关闭对照。
4. **RomFS 16 卷导入**：DocumentViewPicker 整包 + HTTP 拉取清单（模板已按 ROMFS_VOLUMES 泛化）；
   romfs_config 的 IMAGE/INDEX SHA 校验。
5. **字体**：embedded 伪造 BFTTF 六槽（与游戏无关、smo 同源）；TOTK_SHARED_FONTS=<dir> 真固件通道备用
   （expected-v1.1.2.json 里另有固件字体哈希，fonts_mode=firmware 时用）。
6. **着色器/图形**：M4 nvn_vk 实装后（render_cond/tex_flush_upload/tex_dirty_upload 三键在真机才见效果；
   Vulkan 侧 BC 纹理格式支持待验）。
7. RomFS 与 Mac pin 的 ~6MB 差异归属（可 diff 两侧索引）。

## 怎么构建（M1 管线，PC 侧）

```
python nxnative-ohos/tools/make_romfs_runtime.py --archive nxnative-ohos/games/0100F2C0115B6000 \
    --romfs game-work/totk_0100F2C0115B6000/RomFS.img --verify-marker     # 16 卷 → .build/0100F2C0115B6000/romfs_parts/
python nxnative-ohos/scripts/build_hap_from_profile.py nxnative-ohos/games/0100F2C0115B6000 \
    --work game-work/totk_build                                            # → .build/…/entry-default-unsigned.hap
```

2026-10-31 验收结果：**BUILD SUCCESSFUL，未签名 HAP 130,943,339 字节**（bundle/label/图标/rawfile 四件/
romfs_config 全部核验）。签名说明：本机调试签名材料（~/.ohos/config/default_mario-odessy-harmony_*.p7b）
内嵌 bundle-name **com.vibecc.smo**，与 com.vibecc.totk 不符——p7b 由华为 HOS Profile Management Debug
服务签发，无法 PC 侧无头铸造；管线检测到不匹配时自动剥离签名配置出未签名包。**装机前**：DevEco Studio
打开 .build/0100F2C0115B6000 → File > Project Structure > Signing Configs → 勾 Auto-sign 点 Fix（为
com.vibecc.totk 生成新材料）→ 重跑上面的 build 命令即得签名包。smo 档案不受影响（回归验证仍出签名包
63,319,811 字节）。
