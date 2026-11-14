# Round23 · TOTK 1.1.2 derived profile 输入盘点

> 任务：盘点 TOTK 1.1.2 derived profile 需要哪些输入（title-id / 内存布局 / 符号与偏移来源 / 与 SMO 壳的复用点），给出 findings 与 blockers。
> 信息源：① 仓库内 `zelda-tk-harmony/`、`nxnative-ohos/`、`game-work/`；② 移植雷达本地克隆 `/Users/zpp/VibeCoding/tools/d2r-port-radar`（只读）；③ donor 基座 `/Users/zpp/ZCodeProject/switch2mac/infra/nxnative`（只读）；另用 `third/TOTK-PS4`、`third/MacNativeGames` 参考克隆。
> 本文档所有"实测"条目均在 2026-11-01 本次会话中实际执行（命令见各条"复现"）；未执行的一律标明"未运行"。
> 工具说明：本会话工具列表中**没有** `mcp__port-atlas__*` 六件套，按任务指令回退文件数据面（knowledge/findings、games、components、data/notes.json、cache.json、titles.json）；`mcp__rea__*` 可用，但本轮取证用纯 Python 解析 NSO/NACP/NPDM 已拿到字节级结论，未启动 REA 会话（见 §6 复现命令，可随时用 REA 复核）。

---

## 0. 结论速览

| # | 需求项 | 状态 |
|---|---|---|
| F1 | title-id 与版本钉（五件 ExeFS SHA256 = expected-v1.1.2.json） | ✅ 已满足且本机复验通过 |
| F2 | build 行（main/rtld/subsdk0/sdk 四模块 build ID） | ✅ 已满足且本机复验通过 |
| F3 | 内存布局键（mem_reserve/alias/stack/app_pool/sys_resource） | ⚠️ 值已写，**1.1.2 未 heap/region check**（B2） |
| F4 | 符号与偏移来源 A：MacNativeGames `TOTK_GAME_V112` 已验证位点表 | ✅ 已移植进 host_override，本机逐字复验通过 |
| F5 | 符号与偏移来源 B：1.4.3 `patches.py` FRAME_STEP 对 1.1.2 **不适用** | ✅ 本机实证（位点指令字不同）；接入二选一（勿叠加） |
| F6 | 1.1.2 sdk node-area 裁剪位点（1.4.3 裁剪未落 1.1.2 的 sdk） | ❌ 缺；本机已给出候选 sdk+0x12e0f0/0x12e184 与复现办法（B3） |
| F7 | 渲染行为三键 render_cond / tex_flush_upload / tex_dirty_upload | ⚠️ 值已写（继承 1.4.3 结论），真机才见效果 |
| F8 | 1.1.2 着色器表（shader_map/facts/pairs/pso_pairs + c1_full + cb_extent） | ❌ 缺，M4 前置（B4） |
| F9 | SMO 壳复用点（zelda-tk-harmony ↔ mario-odessy-harmony ↔ nxnative-ohos/template） | ✅ 结构已就位；36 个宿主差异文件待 OHOS 化（B6） |
| F10 | RomFS 资产与运行时分发（16 卷 + TOTKRFS1 索引） | 🔴 **本地 RomFS.img 与档案/工程声明矛盾**（4.16GB/307,138 条 vs 17.05GB/16 卷 vs 16.2GB/304,471 条）（B1） |
| F11 | 雷达情报面（TOTK fork 进展） | ✅ 已查清：无 TOTK game 条目，cache.json 有 3 个 TOTK fork |
| F12 | 60fps 运行时补丁机制与待 OHOS 化特性清单 | ⚠️ 最小集已落，其余列入清单 |

Blockers：B1 RomFS 完整性、B2 1.1.2 内存实测、B3 sdk 裁剪位点、B4 着色器表、B5 HAP 签名材料、B6 宿主差异文件 OHOS 化、B7 nvn_vk Vulkan 实装。详见 §5。

---

## 1. 档案现状：仓库里已经有一个 1.1.2 derived profile

"derived profile" 的机制定义在 donor 的 `infra/nxnative/docs/development.md:257`：档案未注册的版本（build ID 不同）**照常可构建**——"该档案的键 + 该版本自己的 build 行 + 该版本自己文件做出的着色器表、链头、`c1_full.tsv`、`cb_extent.tsv`（同 provisional profile，不做 heap check，但做地址区检查）"。红线：注册 build（1.4.3）的重写裁剪与宿主修复**触及不到**另一个 build 的模块。鸿蒙侧把这套语义落进了 `nxnative-ohos/games/README.md` §3，并把 **totk 1.1.2 列为现成案例**。

仓库内现成产物（全部本机实测存在）：

```
nxnative-ohos/games/0100F2C0115B6000/     ← derived profile 本体
  game.tsv                  25 键已写（键表共 43 键，缺省用默认）
  patches.py                FRAME_STEP 仅登记 1.4.3 build ID（159 行，1.1.2 零生效）
  host_override/            game_version.h + game_patches.c + 2 张图标（构建时整文件覆盖）
  expected-v1.1.2.json      版本钉（title_id/version/display_version + 五件 ExeFS SHA256）
  README.md                 档案笔记（版本事实 / host_override 清单 / 待真机验证清单）
game-work/totk_0100F2C0115B6000/   本机抽取：ExeFS 五件 + RomFS.img + control.nacp + icon
game-work/totk_build/patched/      重写产物：四模块 patched.bin + cuts/sites/fixups/obs/gates TSV + 日志
zelda-tk-harmony/                  鸿蒙壳工程（SMO 壳剥壳克隆，23 文件）
```

derived profile 状态自述与键值依据见 `nxnative-ohos/games/0100F2C0115B6000/game.tsv:1-8` 与 `README.md:1-16`；donor 对应物是 `infra/nxnative/dm/games/0100F2C0115B6000/`（**1.4.3 / v720896**，README:1-3）。

---

## 2. Findings（每条 = 一项需求）

### F1 title-id 与版本钉 —— 已满足，本机复验通过

- **title-id**：`0100F2C0115B6000`（main.npdm / game.tsv:11-12 / 目录名一致）。
- **版本**：本机 `control.nacp` 解析 display_version = **"1.1.2"**；`expected-v1.1.2.json` 记 `version=262144`（=1.1.2 的版本号编码）。
- **钉文件**：`nxnative-ohos/games/0100F2C0115B6000/expected-v1.1.2.json`（源出 `third/MacNativeGames/games/totk/assets/expected-v1.1.2.json`，注释 "Verified 1.1.2 outputs from base plus cumulative update. Hashes only; no game data."）。
- **本机复验**（2026-11-01，python3.14 -I 计算五件 SHA256 并与 json 比对）：`main=f04fbebd…`、`rtld=28317605…`、`sdk=91573d44…`、`subsdk0=9d04c989…`、`main.npdm=0b1f6cf9…` **全部 MATCH**。意义：本机镜像与 Mac 侧已验证 1.1.2 **逐字节同源**，Mac 的 1.1.2 位点表可直接适用（这是 F4 成立的前提）。
- 复现命令见 §6.1。

### F2 build 行（四模块 build ID）—— 已满足，本机复验通过

`game.tsv:26` 记：`build v262144 1.1.2 main=9a10ed9435c06733da597d8094d9000a rtld=7501abfe55fa41cffeb46bd619bdcbf4 subsdk0=a6bb7c6ffa9673769fa74ed3d7b05419 sdk=b9046c31eb5d31271be970fe732d38df`（build ID 按 nxnative 惯例截 32 hex）。

本机解析 NSO 头（LZ4 解包前后均可）得到全量 40 hex：

| 模块 | build ID（40 hex） | text 解压大小 | 备注 |
|---|---|---|---|
| main | `9a10ed9435c06733da597d8094d9000ab5d3ee60` | 0x2b17b30 (45.1MB) | text@0x0 / ro@0x2b18000 (0x168e0fc) / data@0x41a7000 (0x4c5fe0) / bss 0xba020 |
| rtld | `7501abfe55fa41cffeb46bd619bdcbf45b9cae3a` | 0x17c0 | |
| sdk | `b9046c31eb5d31271be970fe732d38df49c6aa21` | 0x5e02e0 | 1.1.2 的 sdk **不在** donor `dm/tools/sdk_patches.py` CUTS 表（F6） |
| subsdk0 | `a6bb7c6ffa9673769fa74ed3d7b054191f25d29c` | 0x3440c0 | 电影库（NintendoSDK_movie） |

对照 donor：1.4.3 的 main=`277178b7dba1b6d4949a84778d4abc58…`、sdk=`574284b4…`（`infra/nxnative/dm/games/0100F2C0115B6000/game.tsv:23`）——build ID 完全不同，版本红线成立。

**重写产物一致性**（本机核对）：`game-work/totk_build/patched/main/main.fixups.tsv` 头部记 `PATCH_OFF 0x4730000 / KEY_PAGE_OFF 0x4728000 / KEY_PAGE_SIZE 0x4000 / TEXT_END 0x2b17b30 / BUILD_ID 9a10ed9435c06733da597d8094d9000a`；`main.patched.bin` 80,775,812 B = 镜像区 0x4730000 (74,645,504) + 补丁区 6,130,308 B，与 `main.log` 行尾 "patch 6130308 PATCH_OFF 0x4730000" 逐数相符；`gates.tsv` 862 行（spin 门），`cuts.tsv`/`obs.tsv` **空表头**——1.1.2 零指令裁剪。产出该产物的重写器为 d2r 血统 `patch_nso.py`，Windows 侧执行（`build_src.sha256` 里路径为 `..\games\0100F2C0115B6000\patches.py` 反斜杠风格，且包含 27 份游戏 patches.py + `sdk_patches.py`；PROJECT_ENV.md:33-36 记 Windows 抽取路径）。

### F3 内存布局键 —— 值已写；1.1.2 未实测（→B2）

`game.tsv` 内存组（36-52 行）与依据：

| 键 | 值 | 出处 |
|---|---|---|
| mem_reserve | 0x140000000 (5 GiB) | donor 1.4.3 档案 heap check 产物（README:11「5 GB 是手机上证实过的总量」）；鸿蒙宿主另按 GUEST_ADDRESS_LIMIT 2^39 预留 + TOTK_ALIAS_GIB 搜缩，本键为 M4+ 参数化注入目标 |
| mem_alias | 0x98000000 (2.375 GiB) | 同上（1.4.3 heap check 手调）；宿主 services.c `alias_requested()` 默认 64 GiB 搜缩 |
| mem_stack | 0x10000000 | 与宿主 services.c STACK_SIZE 一致 |
| app_pool | 0xCD500000 | 与宿主 services.c `app_pool()` 缺省一致 |
| sys_resource | 0x1800000 (24 MiB) | **本机实测**：main.npdm META+0x14 = 0x01800000（§6.3 命令），与 1.4.3 相同 → `IsVirtualAddressMemoryEnabled` 为真、游戏内存走 alias 区；宿主 services.c 现答 32 MiB 常量，本键为参数化目标 |

两个版本相关事实（donor README:30-31）：游戏在 `nninitStartup` 调 `SetHeapSize(0x86000000)`（2.1 GB）；alias 区里 SDK 的 ~1 GB node area 需裁到 62 MB（F6），否则 1.4.3 上实测初期下洞内存不足（MSF/血月回位）。**这组键对 1.1.2 是"继承值 + 静态同源推断"，没有 1.1.2 的 heap check / region check 运行数据**——档案 README:56-58 自己把 "SetHeapSize(0x86000000) 后的 alias 布局（1.1.2 未实测）" 列为真机验证项 #1。

### F4 符号与偏移来源 A：MacNativeGames `TOTK_GAME_V112` 已验证位点表（主力来源）

- **来源**：`third/MacNativeGames/games/totk/runtime/game_version.h:54`（`#elif defined(TOTK_GAME_V112)` 块，行 60 起 `#define GAME_ADDR_0x8f6c60 0x8f66e4` …）。语义：键 = 1.0.0 原始偏移，值 = 1.1.2 对应偏移。
- **移植落点**：`nxnative-ohos/games/0100F2C0115B6000/host_override/entry/src/main/cpp/host/game_version.h`（整表 36 条 GAME_ADDR + `GAME_TITLE_ID`/actor 字段偏移），配 `host_override/entry/src/main/cpp/host/game_patches.c`（fps60_sites 7 条 + slowmo veneer + TOTK_TRAP_AT 诊断，**每条写入前先校验原指令字，不匹配即 runtime_fail 停机，不盲写**）。
- **本机逐字复验（本次会话执行）**：把 `ExeFS/main` 按段 LZ4 解包（得到未补丁真值），核对：
  - 7 条 fps60 位点（main+0x8f66e4 / 0x8f66e8 / 0x8f670c / 0x8f679c / 0x8f67ac / 0x8f6800 / 0x8f681c）：指令字 **0x6b08015f / 0x1a89b10a / 0xb9401a8a / 0x6b08015f / 0x1a88c1ac / 0xb9400288 / 0x2a0a03f5 全部 OK**；
  - slowmo 蹦床位点（main+0xbbb4c0 三字 0xbd402900 / 0x52a85908 / 0x1e270101）**全部 OK**。
- **含义**：fps60（present-interval 下限 2→1）与蹦床慢动作修正对本地镜像**静态成立**；真机阶段 game_patches.c 还会再做一次运行时校验兜底（TOTK_FPS60=0 可关闭对照）。
- 该表覆盖面之外、待 OHOS 化的 1.1.2 位点组（同文件注释 27-46 行）：real-time frame-step 读点组（0x7f7a38…0x7f7a54、0x8f633c…0x8f6354）、输出分辨率组（0xcdd3d0…0xcdd3f4、0xea7e14/0xea7efc/0xea7f00）、survey 杂项（0x70376c/0x1627f10/0x4725058/0x2b16100/0x46129e0/0x8f6740）。

### F5 符号与偏移来源 B：donor 1.4.3 `patches.py` —— 对 1.1.2 不适用（已实证），接入二选一

- `nxnative-ohos/games/0100F2C0115B6000/patches.py:159`：`FRAME_STEP = {'277178b7dba1b6d4949a84778d4abc58': frame_step}`——只键控 1.4.3 的 main build ID，文件头（6-17 行）明确"1.1.2 地址未验证/未登记"。
- **本机实证**：解包后的 1.1.2 main 在 1.4.3 的 fs_site(0x2db18c) 处指令字为 **0x94161171（一条 BL）**，≠ 1.4.3 预期的 0x09604139（ldrb w9,[x0,#0x58]）——两版本代码布局不同，盲套 1.4.3 偏移必然写坏（这也正是 nxnative `AGENTS.md:19`"never let fixed addresses run on a build they were not found in"红线的原因）。
- 两条接入路线（patches.py:11-15 自述，二选一**勿双份叠加**）：
  a) `MATCH_CUTS` 指纹登记（donor `AGENTS.md:19` / `docs/reference/rewriter.md` Function-matched cuts）：按函数指纹在未注册 build 里定位已验证位点；
  b) 运行时补丁路线（已选）：`host_override/game_patches.c` 的 fps60 表（F4）——重写器 FRAME_STEP 与运行时 FPS60 是两条并行机制，当前 1.1.2 走 b。
- 若未来为 1.1.2 登记重写器帧步进，须删 fps60 或 TOTK_FPS60=0 关闭（game_patches.c:18 注释）。

### F6 1.1.2 sdk node-area 裁剪位点 —— 缺失（→B3），本机已产出候选与复现办法

- 需求：1.4.3 依赖 `dm/tools/sdk_patches.py` 把 sdk 的 AddressRegionManager node area 1GB→62MB（1.4.3 位点 sdk+0x145d0c/0x145d78，SDK build `574284b4…`；donor README:10、memory.md:177）。1.1.2 的 sdk（`b9046c31…`）不在 CUTS 表，本次重写未裁（game.tsv:50-52 风险备注）。症状若现：初期下洞 MSF/血月回位。
- **本机候选线索（本次会话产出）**：对 1.1.2 sdk text（0x5e02e0）全量扫描 `MOVZ wX, #imm, LSL #16`（0x52800000 族，imm∈{0x3FE0, 0x4000}）命中 65 处；最贴合档案 README:61 所述 "找 `08fca752`/`09fca752` 型 mov 对"（w8/w9 各装 0x3FE00000 ≈ 1016 MiB）的候选是：
  - **sdk+0x12e0f0**（`52a7fc08` = movz w8,#0x3FE0,lsl16）与 **sdk+0x12e184**（`52a7fc09` = movz w9,#0x3FE0,lsl16），同处一个函数（0x12e0e0 起，prologue `a9bd7bfd`=stp x29,x30,[sp,#-48]!）；1.4.3 的两点相距 0x6c，候选两点相距 0x94，同数量级。
  - 佐证：字符串 `AddressRegionManager` 在 1.1.2 sdk ro+0x8061f0（符号名存在）。
- **复现/确认办法**（写进 B3）：对解包后的 1.1.2 sdk 在 sdk+0x12e0e0 起反汇编（可在 REA 里以 raw-binary 打开解包段，或 Ghidra），确认该函数是 AddressRegionManager 构造/初始化（结合 ro 符号与调用面），再对照 1.4.3 sdk_patches.py 的两条 CUT 原字节断言形式登记进 1.1.2 的裁剪（或 sdk_patches.py 加 build 分支）。**候选≠已验证**：未做反汇编语义确认前不得写补丁。

### F7 渲染行为三键 —— 值已写（继承 1.4.3 结论），真机验证

`game.tsv:54-60`：`render_cond on`（1.4.3 上不加则 3D 场景黑：条件渲染回放求值）、`tex_flush_upload on`（流送纹理 CPU 原地写+FlushMappedRange 发布）、`tex_dirty_upload on`（CPU 无 flush 改写的 ASTC 地形阵列；不加洞里品红）。三键的发现过程与机理见 donor `dm/games/0100F2C0115B6000/README.md:37-44`（Conditional rendering / magenta cave / torch light 三案）。profile_guess.py:12-13 明确这类键"一分钟 boot 决不了"，属 bring-up 实测项——真机 M3 后半验证（档案 README 待办 #6）。

### F8 1.1.2 着色器表 —— 缺失（→B4），M4 前置

- `game.tsv:28`：`shader_versions v262144`，注释"着色器表尚无（M4 自本机 1.1.2 文件生成）；1.4.3 表不可直接复用"。
- 需要的输入（donor `dm/games/README.md` 档案格式 + nxnative-ohos/games/README.md §4 第 6 步）：从 **1.1.2 自己的 RomFS** `Shader/*.bfsha.zs`（BNSH；material.bfsha 为主）与 `Shader/ApplicationPackage.Nin_NX_NVN.release.sarc.zs` 生成 `shader_map.tsv`(+conflicts) / `shader_facts.tsv` / `shader_pairs.tsv` / `pso_pairs.tsv`，以及链头 `dmsh_chain.h`、`c1_full.tsv`、`cb_extent.tsv`（development.md:257 明确这些必须"made from that version's own files"）；TOTK 无 PC 版所以还需 `shader_facts_manual.tsv` 手写整数输出行（donor README:15：agl BC4 编码器 `0544282c162305db` 与 Ultrahand 掩码 `e3fb0bb58a691e10`）。
- 鸿蒙侧后端差异：donor 链是 Maxwell→SPIR-V→MSL→metallib（雷达组件 `nvn-shader-pipeline`），鸿蒙 `nvn_backend vulkan` 可直用 SPIR-V（`zelda-tk-harmony/entry/src/main/cpp/nvn_vk/nvn_vk.h:17`："游戏着色器已是 SPIR-V，直接 VkShaderModule"），但 BC/ASTC 纹理格式与整数输出语义要在 Vulkan 侧复验（B7）。

### F9 与 SMO 壳的复用点 —— 结构已就位（一层壳 + 一层模板宿主）

三层复用关系（均为本机 diff/读取实证）：

1. **zelda-tk-harmony（壳工程）= mario-odessy-harmony 的剥壳克隆**：`diff -rq` 两目录——结构同构（AppScope/entry/cpp/ets/hvigor），zelda 侧缺 `host/`、`generated/`（SMO 已生成的宿主 C 与三张表头）；差异文件集中在 app.json5（bundleName `com.vibecc.totk`）、CMakeLists（宿主源未挂）、embed.S（incbin 已按 totk_build/patched 四模块写好但仍 `#if 0` 注释态）、game_napi.cpp（host_main 仍是 stub，注释"mmap 四个 .patched.bin → rtld 自举 → svc 表 → NVN 转发 nvn_vk"）。UI 面 Index.ets（按钮 + 提示"周末接入 XComponent + Vulkan"）。小瑕疵：app.json5 `versionName "1.4.1"` 是 SMO 版本号残留，与游戏 1.1.2 无关（仅观感）。
2. **宿主模板 = nxnative-ohos/template**（odyssey 血统的 OHOS 移植）：build_hap_from_profile.py 实例化 template 并用 `host_override/` 整文件覆盖（game_patches.c / game_version.h / 图标）——totk 档案 README:18-25 的 host_override 清单即此机制。2026-10-31 验收：**未签名 HAP 130,943,339 B 构建成功**（README:81-87）。
3. **Mac 宿主参考 = third/MacNativeGames/games/totk/runtime/**（72 文件）：与 odyssey runtime 59 文件相比，55 个同名中仅 19 个逐字节相同；totk 独有 17 文件、同名差异 36 文件（services.c ~152 行差异 / threads.c ~659 行 / loader.c ~199 行 / runtime.h 钩子面等）——**这些是"待 OHOS 化"清单的主体**，不能盲拷进构建（档案 README:27-51 已逐文件列明）。

另一个可借鉴但**位点不通用**的参考：`third/TOTK-PS4`（README:1-3）目标是 TOTK **1.0.0** 的 PS4 静态重编译 + RADV Vulkan 回放（2-10 fps，开发停在 v00.72）——它的 1.0.0 偏移/补丁对 1.1.2 不适用，但其内存管理（4.5GB 直存 + zstd 冷页压缩）、pass 抽帧（NX_PASS_EVERY）与多核录制的教训对鸿蒙性能阶段有参考价值。

### F10 RomFS 资产与运行时分发 —— 🔴 本地资产与声明矛盾（→B1）

机制面（一致，无需再查）：

- `game.tsv:33,69-78`：romfs_marker `Pack/ZsDic.pack.zs`（zstd 词典包，donor README:68）；`romfs_volume_mib 1024`（16 卷 @1GiB 预期）；`fonts_mode embedded`（make_shared_fonts.py 伪造 BFTTF 六槽，真固件字体走 TOTK_SHARED_FONTS 备用通道）；`nvn_backend vulkan`。
- 工具链：`nxnative-ohos/tools/make_romfs_runtime.py`（DMROMFS1 → TOTKRFS1 索引 + 等大分卷，输出 `.build/<titleID>/romfs_parts/`，大文件不入库）+ `prepare_runtime.py` 生成 `romfs_config.h`（SIZE/IMAGE_SHA256/INDEX_SHA256）。
- **矛盾（本机实测，2026-11-01）**：
  - 本地 `game-work/totk_0100F2C0115B6000/RomFS.img`：逻辑大小 **4,160,860,127 B（3.87 GiB，非稀疏）**，DMROMFS1 头 ioff=0x3f6d5386e、**条目 n=307,138**，mtime 2026-10-31 18:10:08；
  - 档案 `README.md:12-13` 声称本地 RomFS.img **17,045,762,015 B（16 卷）**（与 `make_romfs_runtime.py` docstring 的 17,045,762,015 同源）；
  - `PROJECT_ENV.md:35` 声称 **16.2GB / 304,471 文件**（Windows 侧抽取，C:\Users\macvi\…）；
  - `game-work/romfs_parts/` 现存 5×1GiB + 309MB 分卷（mtime 18:09:33-54，**早于**当前 RomFS.img 的 18:10:08）——本机核对 `romfs.bin.000` 首部 DMROMFS1 头（ioff=0x1526ca2b7, n=5028）与 **SMO** 的 RomFS.img 完全一致：该分卷目录是 **SMO 的**，不是 TOTK 的；TOTK 的 16 卷输出应在 `.build/0100F2C0115B6000/romfs_parts/`，该目录当前不存在。
  - 三方数字（4.16GB/307,138 条、17.05GB/16 卷、16.2GB/304,471 条）互不相容；且 4.16GB 与 17GB 相差 4 倍以上，远超档案 README:70 自述的"~6MB 差异"。

### F11 雷达情报面（TOTK fork 进展）—— 已查清

- `knowledge/games/` 仅两游戏：D2R（0100726014352000）、TRIANGLE STRATEGY（0100CC80140F8000）；**无 TOTK 条目**。`knowledge/findings/` ts-0001~0014、gen-0001~0002 全部挂在这两游戏上，grep totk/zelda/kingdom **零命中**。
- `data/notes.json`：无 TOTK fork 条目；Zelda 相关仅 `TommyLau/botw-mobile`（BotW，65 分，"已能启动跑到第一个未实现的系统服务"，附"新游戏接入工具"可作流程参考）。
- `data/cache.json`（420 个仓库快照）里有 **3 个 TOTK fork**（notes.json 未收录，即雷达未出正式评注）：
  - `NicholasDominici/totk-ios`（2026-10-24）："Publish **source-only** TOTK iPad and iPhone port tooling"——只有工具没有游戏数据；
  - `OJZen/totk-mobile`（2026-10-21）：RomFS standard-crypto 支持、着色器翻译器四偏移纹理 gather 修复、iOS 离线着色器库复用、Mac 启动着色器屏平滑——1.4.3 世代思路，可借鉴其"离线着色器库"与"标准加密底包识别"；
  - `valentderah/totk-pc-recomp`（v0.2.8，2026-10-31）：注册 **1.4.3 + 1.1.1** 两个游戏版本（"Support the 1.1.1 update as a third game version"）——与本项目不同版本组合，但其"按版本找着色器"（narrowed shader walk）与 installer 版本校验文案值得参考。
- 结论：雷达对 TOTK 的直接情报为零增量；本项目 TOTK 1.1.2 的独特组合（nxnative 血统 + 鸿蒙 + 1.1.2）在雷达视野里无先例，**没有可抄的现成 fork 进展**。

### F12 60fps 补丁与待 OHOS 化特性清单 —— 最小集已落

已落（host_override/game_patches.c）：fps60_sites 7 条指令字替换（含全部原字校验）、patch_slowmo_bounce（veneer 写 text padding +128，自含 branch）、patch_trap 诊断（TOTK_TRAP_AT 写 brk）。未 OHOS 化清单（同文件头 10-17 行 + 档案 README:33-51）：patch_realtime+patch_cloth_clock（需 loader 数据页集成——OHOS loader.c 已留 0x4000 页）、patch_render_res/patch_output_res、patch_pchtxt、aspect（1.1.2 无 ultrawide，上游同，优先级低）、world_survey totk 变体、settings 面板等。

---

## 3. 输入清单（derived profile 的"需要什么"汇总）

按 development.md:257 的 derived profile 构成逐项对账：

| 构成项 | 1.1.2 现状 | 输入来源 |
|---|---|---|
| profile 键（game.tsv） | ✅ 已写 25 键 | donor 1.4.3 档案 + 本机 npdm 实测 + 宿主 services.c 语义核对 |
| 该版本 build 行 | ✅ 四模块 build ID | 本机 NSO 头 + patched fixups.tsv（二者一致） |
| 着色器表/链头/c1/cb_extent | ❌ 未生成 | 必须出自 **1.1.2 自己的 RomFS** shader 文件（F8/B4） |
| 重写补丁（CUTS/OBS/FRAME_STEP） | ⚠️ 空（零裁剪）；FRAME_STEP 未登记 | 路线 a) MATCH_CUTS 指纹 或 b) 运行时补丁（已选 b）（F5） |
| sdk 模块裁剪 | ❌ 1.1.2 sdk 不在 sdk_patches CUTS | 候选 sdk+0x12e0f0/0x12e184 待语义确认（F6/B3） |
| heap/region check（provisional 才做，derived 免） | —— 免做，但 5GB 布局是 1.4.3 推断值 | 真机 boot 验证（B2） |
| 版本钉 expected-v<ver>.json | ✅ v1.1.2 已拷入且本机五件哈希复验通过 | MacNativeGames assets（F1） |
| 运行时位点表 | ✅ V112 已验证表已移植 + 本机逐字复验 | MacNativeGames TOTK_GAME_V112（F4） |
| RomFS 分卷 + TOTKRFS1 索引 + romfs_config | 🔴 资产矛盾未解 | 见 F10/B1 |
| 宿主（SMO 壳 + template + host_override） | ✅ 机制就位；36 差异文件待 OHOS 化 | F9/B6 |

## 4. 依赖与复用关系（一句话版）

- 本机镜像同源性（F1）→ 成立则 Mac V112 位点表（F4）直接适用 → fps60/slowmo 静态成立（已复验）→ 1.1.2 重写零补丁是**有意状态**（F5）→ 运行时补丁与重写补丁二选一。
- 内存键（F3）与 sdk 裁剪（F6）是同一条因果链：SetHeapSize(0x86000000) + alias 区 + node area 未裁 → 初期下洞 MSF/血月风险 → B2/B3 是同一轮真机验证的两个观察点。
- 着色器表（F8）依赖 RomFS 资产（F10）正确——**B1 不解，B4 无法开工**。

## 5. Blockers

- **B1 RomFS 完整性未定（最高优先）**：本地 RomFS.img 4.16GB/307,138 条 vs 档案声称 17.05GB/16 卷 vs PROJECT_ENV 16.2GB/304,471 条，三方矛盾；且 game-work/romfs_parts 是 SMO 的分卷（DMROMFS1 头实match SMO），TOTK 分卷目录 `.build/0100F2C0115B6000/romfs_parts/` 不存在。需先确认本地这份 DMROMFS1 是完整 1.1.2 RomFS 还是截断副本（对比 Windows/Mac pin 的 IMAGE_SHA256，或对索引 307,138 条做体积求和），再决定以哪份为准重切分卷、重算 romfs_config 的 IMAGE_SHA/INDEX_SHA。档案 README:70 的"~6MB 差异归属"待办实际掩盖了 4 倍量级差异。
- **B2 1.1.2 内存布局未实测**：mem_reserve/mem_alias 是 1.4.3 heap check 产物；1.1.2 的 SetHeapSize(0x86000000) 行为与 alias 布局需真机 boot 观察（档案 README 待办 #1）。
- **B3 1.1.2 sdk node-area 裁剪位点未确认**：候选 sdk+0x12e0f0/0x12e184（1016 MiB mov 对，同函数 0x12e0e0 起）待反汇编语义确认后才能登记（F6 的复现办法）。
- **B4 1.1.2 着色器表未生成**：需 shader_split/dmsh 链路对 1.1.2 RomFS 的 BNSH 产出全表 + 手写整数输出行；被 B1 阻塞；M4（nvn_vk Vulkan 实装）前置。
- **B5 HAP 签名材料**：本机调试 p7b 内嵌 bundle-name 是 com.vibecc.smo，与 com.vibecc.totk 不符，PC 侧无法无头铸造；需 DevEco Studio 为 com.vibecc.totk 生成签名材料后重跑构建（档案 README:83-87）。
- **B6 宿主差异文件 OHOS 化**：MacNativeGames totk runtime 与 odyssey 模板 36 个同名差异文件 + 17 个 totk 独有文件，需逐文件 OHOS 化（threads.c ~659 行、loader.c ~199 行、services.c ~152 行为大头）；不阻塞 M1 管线出包，阻塞真机特性面。
- **B7 nvn_vk Vulkan 后端实装**：当前仅骨架（nvn_vk.h 注释级分块清单 + nvn_vk_stub.cpp）；NVN→Vulkan 的 BC/ASTC 纹理格式、别名池、条件渲染回放、pipeline 预烘焙等全部待实装；F7 三键只有在其上才可见效果。

## 6. 本机已执行的复现命令（全部 2026-11-01 实跑）

```bash
# 6.1 五件 SHA256 与 expected-v1.1.2.json 比对（F1）
/opt/homebrew/opt/python@3.14/bin/python3.14 -I - <<'EOF'   # 摘要：对 ExeFS/{main,rtld,sdk,subsdk0,main.npdm} sha256
#   并与 nxnative-ohos/games/0100F2C0115B6000/expected-v1.1.2.json 的 exefs_sha256 逐项 MATCH
EOF
# 6.2 NSO 头解析（F2）：/tmp/totk_verify/nso_check.py 内 nso_load() 读 NSO0 魔数/三段三元组/bss/build_id
# 6.3 npdm 实测（F3）：python3.14 -I 读 main.npdm[0x14:0x18] = 0x01800000；control.nacp[0x3060:0x3070]="1.1.2"
# 6.4 位点逐字校验（F4/F5）：/tmp/totk_verify/nso_check.py —— LZ4 解包 ExeFS/main，
#     7 条 fps60 位点 + slowmo 3 字全 OK；1.4.3 位点 0x2db18c=0x94161171(≠0x09604139)
# 6.5 sdk 常量扫描（F6）：MOVZ(0x52800000 族, LSL16, imm∈{0x3FE0,0x4000}) → 65 命中，
#     候选对 sdk+0x12e0f0/0x12e184；ro+0x8061f0 有 "AddressRegionManager"
# 6.6 RomFS 取证（F10）：stat -f %z RomFS.img（4,160,860,127）；解析 DMROMFS1 头（n=307,138）；
#     romfs.bin.000 头 == SMO RomFS.img 头（romfs_parts 归属 SMO）
# 6.7 壳对比（F9）：diff -rq zelda-tk-harmony mario-odessy-harmony
```

## 7. 出处索引（关键证据 → 文件）

| 断言 | 出处 |
|---|---|
| 1.1.2 版本钉/哈希 | `nxnative-ohos/games/0100F2C0115B6000/expected-v1.1.2.json`（本机五件哈希复验 MATCH） |
| derived profile 键值与出处 | `nxnative-ohos/games/0100F2C0115B6000/game.tsv`（各组注释） |
| FRAME_STEP 仅 1.4.3 | `nxnative-ohos/games/0100F2C0115B6000/patches.py:157-159` |
| V112 位点表 | `nxnative-ohos/.../host_override/.../game_version.h:19-56`；源头 `third/MacNativeGames/games/totk/runtime/game_version.h:54-` |
| fps60 补丁实现 | `nxnative-ohos/.../host_override/.../game_patches.c:36-44,98-119` |
| derived profile 机制 | `infra/nxnative/docs/development.md:257`；`infra/nxnative/AGENTS.md:19`；`nxnative-ohos/games/README.md` §3 |
| 1.4.3 档案（donor） | `infra/nxnative/dm/games/0100F2C0115B6000/{game.tsv,README.md}` |
| 内存三键机理 | donor README:37-44；render_cond/tex_flush/tex_dirty 的发现记录 |
| 重写产物/键表 | `game-work/totk_build/patched/main/{main.fixups.tsv,main.log,main.gates.tsv,build_src.sha256}` |
| RomFS 工具 | `nxnative-ohos/tools/make_romfs_runtime.py`（docstring 17,045,762,015 B → 16 卷）；`tools/prepare_runtime.py` |
| RomFS 矛盾 | 本机 stat/DMROMFS1 头实测（§6.6）；`PROJECT_ENV.md:33-36`；档案 README:12-13 |
| SMO 壳克隆 | `diff -rq zelda-tk-harmony mario-odessy-harmony`；`zelda-tk-harmony/entry/src/main/cpp/{game_napi.cpp,embed.S,CMakeLists.txt}` |
| 雷达情报 | `/Users/zpp/VibeCoding/tools/d2r-port-radar/{knowledge/games/*,data/notes.json,data/cache.json}`（games 无 TOTK；cache 三 fork） |
| TOTK-PS4 定位 | `third/TOTK-PS4/README.md:1-3`（1.0.0，静态重编，非 1.1.2 位点来源） |
