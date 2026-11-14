# nxnative-ohos

Play your own Nintendo Switch games natively on HarmonyOS phones and tablets. Bring your game (its NSP files) and your `prod.keys`; one script turns them into a signed HAP you install with hdc.

It is not an emulator: the game's own code runs directly on the phone's ARM cores, with Switch graphics rebuilt on Vulkan. Nothing from any game is in this repository, and the project is not affiliated with Nintendo or any game publisher.

> 血统：d2r-mobile → nxnative (iOS/Metal) / MacNativeGames (Mac/Metal) → **本项目 (鸿蒙/Vulkan)**。
> 设计决策见 `../game-work/nxnative_ohos_design.md`，调研依据见 `../game-work/logs/nxnative_ohos_survey.md`。

## Status（2026-10-31）

| 里程碑 | 状态 |
|---|---|
| M1 模板工程抽取（mario 工程参数化 + smo 档案） | **完成（2026-10-31）**：`scripts/build_hap_from_profile.py games/0100000000010000 --work <smo_build>` 一键出 63.3MB 签名 HAP（生产线等价；generated 三头与生产线逐字节一致） |
| M2 一条脚本 build_hap_from_nsp.py | **完成（2026-10-31）**：真实 smo NSP（5.6GB 本体+397MB 更新）全流程 32s → 签名 HAP；**从原始 NSP 字节级复现既有转译产物**（patched 24 文件 sha256 全一致）；指纹缓存复跑 0.2s；`new_game.py` 从未知 NSP 自动起草档案（build ID/romfs_marker/模块序自动识别） |
| M3 第二游戏（TotK 1.1.2 derived profile） | **前半完成（2026-10-31）**：`games/0100F2C0115B6000/` 档案（25 键落值，1.1.2 derived；ExeFS 五件与 MacNativeGames expected-v1.1.2.json 哈希全同）+ `tools/make_romfs_runtime.py` 档案化分卷（17,045,762,015 B → 16 卷，marker 校验过）+ M1 管线构建 **BUILD SUCCESSFUL，未签名 HAP 130,943,339 B**（本机调试签名 p7b 绑定 com.vibecc.smo，com.vibecc.totk 需 DevEco Signing Configs → Fix 后重跑；smo 回归仍出签名包）。真机点火待设备（清单见档案 README） |
| M4 图形后端 nvn_vk（Vulkan 实装） | 与主线合并推进 |
| M5 文档定稿（README/PITFALLS/frontends） | 持续 |

**快速上手（M2 已可用）**：

```bash
# 已支持的游戏（games/ 里有档案的）
python scripts/build_hap_from_nsp.py <NSP目录> -k <prod.keys> --archive-root games
# → .build/<titleID>/entry/build/default/outputs/default/entry-default-signed.hap

# 新游戏（首次）: 先起草档案，人工补内存/图形键，再走上行
python scripts/new_game.py <NSP目录> -k <prod.keys> -o games/<titleID>-draft
```

主线进展（奥德赛真机 boot 逐轮推进）见 `../game-work/logs/round*.log` 与 `../game-work/logs/crash_1031_font_round.md`。

## Layout

```
scripts/   build_hap_from_nsp.py（总入口，M2）· new_game.py
template/  鸿蒙宿主模板工程（M1 从 mario-odessy-harmony 泛化）
games/     每游戏档案（game.tsv + patches.py + 着色器表）；键表见 games/README.md
tools/     prepare_runtime / make_romfs_runtime / make_shared_fonts / run_cycle（从 game-work 泛化迁入）
docs/      frontends.md（JSON progress 协议 + env 链）
```

大文件（patched.bin / RomFS 分卷 / HAP）一律不入库，走 `<titleID>_build/` 工作区（指纹缓存，输入不变即复用）。

## Architecture rule（继承 nxnative）

**Everything game-specific belongs in `games/<titleID>/`. Code reads the profile; it does not hard-code a game.**

游戏差异的三种表达，按优先级：
1. `game.tsv` 键值（内存布局/呈现行为/纹理策略/字体模式…）
2. `patches.py` per-build 指令补丁（CUTS/OBS/FRAME_STEP，按 NSO build ID 键控）
3. `host_override/` 整文件覆盖（逃生门：runtime 分叉度 ~65%，模板不可能参数化覆盖全部）
