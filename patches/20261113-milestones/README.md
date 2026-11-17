# 2026-11-13 里程碑补丁包（M92A → M105A）

真机目标：HarmonyOS 手机上把《超级马里奥 奥德赛》跑出**可辨识的 3D 画面**。
本目录 = 当天全部**代码改动**（全部为 `platform/nvn_vk/nvn_vk.c` 与 `platform/win_fill.c` 内的探针/修复，
**新增行为默认关闭**，knob 缺省 0，关态逐字节等于旧行为）。

## 一、补丁清单（按时间顺序，`git apply -p0`，无 a/ b/ 前缀）
| 单 | 主题 | 结论 |
|---|---|---|
| m92a | 真机 dump「缺失 shader」诊断 | 拿到 88 个键（33 VS + 55 FS） |
| m97a | present 直选源 + 同槽写入计数 | **画面第一次出真内容 62 s**，随后被 2884 笔写入烧白 |
| m98a | 三个通道假设 | 三假设证伪；到「黑底 + 1 个不规则白影」 |
| m99a | 顶点/draw 归宿普查 | 2297 万顶点 / 2 万笔 draw 全走 `HOSTFALLBACK`（真正上游根因） |
| m100a | 兜底归因 | 唯一判定点 `nvn_vk_rt_bind → h_rt_ensure()=NULL`（游戏声明 n=0 颜色目标） |
| m101a | 堵掉 host 兜底 | draw 落回真槽；画面=带透视纯黄梯形，缺的只剩贴图/着色 |
| m102b | 贴图链审计 | 3D 段**确有** 8 个带贴图 pass（`ntex_decl≥1 && ntex_real≥1`），但都不写最终可见槽 |
| m103a | id 图名额 D1 修复 | 384→4096、512 MiB 活体硬顶、每 submit 让位 64 ⇒ `why=-31` 80→0 行、`kind=4` 0% |
| m104a | 「屏幕像素是谁写的」定谳 | **(a)** 由 `win_fill.c:nvn_win_present_rgba()` 写入（三方对齐误差 ≤2%）；swapchain 从不写；槽扫描负结论=3D 段无着色内容 |
| m105a | 帧内写入时序 / 最后覆盖 | **NO**：最终可见 RT（slot 60, 800×450）每帧 18 笔=15 常量色 + **3 带贴图**，末笔即带贴图笔 ⇒ 「先着色后被常量色覆盖」被**否证** |

## 二、五层定谳（黑屏拆解结果）
1. **shader 语料** ✅ `nokey=0`、`cache_keys=14510`
2. **场景几何** ✅ draw 落回真槽
3. **贴图名额/绑定** ✅ `idimg 690/4096`、`why=-31` 0 行、`kind=4` 0%、`ntex_real=ntex_decl=27/27`
4. **上屏通道** ✅ 屏幕像素由我们自己的 `nvn_win_present_rgba()` 写入（≈1 write/present）
5. **内容** ❌ 唯一未解：最终可见 RT 只有常量色几何（纯黄/黑），带贴图 pass 写的是 12/31/37/39/40/45/55/**75**/76 等槽

## 三、可复现产物
- 源码快照 = 本提交（m104a 链）；同一构建树 HAP `795,044,393 B / md5 3db7b154a1a30f2acf9d7c88e57d1b44`
- M105A HAP `795,052,500 B / md5 0079a5773774b005353e34f5d5b06212`
- 语料：`index.json` / `spv.bin`(673,412,976) / `index_m27a.json`（不入库，只进 HAP 打包）
- 下一跳：带贴图 pass 的真实目标槽 **slot 75（400×225，每帧 237 笔带贴图 draw）** ⇒ 在 present 取源处加一次 `vkCmdBlitImage` 75→800×450，走既有 CH1 通道

---

# 增补：M106A → M148A（M149A 归档，2026-11-15）

上一节只到 M105A。M106A–M148A 的补丁此前**只存在于 `logs/<单>/<单>.patch` 与 `/tmp`**，
`/tmp` 清一次就没有了；M149A 把它们原样复制进本目录（只复制、不改内容，md5 见
`logs/m149a/patch_inventory.tsv`），并按 build #11/#12 的真实建树顺序排好。

## 一、顺序（`git apply -p0`，无 `a/ b/` 前缀）

```
m105a → m106a → m107a → m108a → m109a → m113a → m115a → m119a → m122a
      → m119b（必须 cd entry/src/main/cpp）
      → m126a → m132a → m135a → m140a        ← 到此 = build #11 施工树（nvn_vk.c 37,310 行 / 49fcaf19…）
      → m148a                                ← build #12 增量（37,388 行 / 9e618e96…）
```

## 二、逐单（★ = 在 build #11/#12 链上）

| 单 | 主题 | 链 | 落点文件 |
|---|---|---|---|
| m105a ★ | 帧内写入时序 / 最后覆盖 | ✓ | `nvn_vk.c` |
| m106a ★ | blit 75→800×450 通道 | ✓ | `nvn_vk.c` |
| m107a ★ | 帧内分段计时（骨架） | ✓ | `nvn_vk.c` |
| m108a ★ | 性能剖面：一帧 4 s 花在哪 | ✓ | `nvn_vk.c` |
| m109a ★ | 槽位/几何普查推进 | ✓ | `nvn_vk.c` |
| m110a | 抓 4 笔状态探针 | **✗ 弃用** | `nvn_vk.c` —— 引入「选槽自举死锁」（M111A 定谳），M113A 起绕开 |
| m113a ★ | MRT / 状态抓取（+648 行） | ✓ | `nvn_vk.c` |
| m115a ★ | seal text/patch RX 校验 | ✓ | `game_patches.c`、`loader.c` |
| m119a ★ | 声明历史探针 | ✓ | `nvn_vk.c` |
| m119b ★ | submerge/layout 大改（含 `nvn_vk.h`/`gfx_bridge.c`/`cmd_trace.*`） | ✓ | 5 个文件，**路径相对 cpp 目录** |
| m122a ★ | 管线/描述符 RTT 探针 | ✓ | `nvn_vk.c` |
| m126a ★ | seal/exec mmap 修复 + 新探针 | ✓ | `loader.c`、`vk_boot.c`、新增 `m126a_probe.c/.h` |
| m132a ★ | 建管线守卫（`smo_pso_guard`） | ✓ | `nvn_vk.c`、`loader.c` |
| m135a ★ | `ci->layout == NULL` 根因修复（`smo_pso_layout` 缺省 1） | ✓ | `nvn_vk.c` |
| m140a ★ | RT 合并（`smo_rt_submerge`） | ✓ | `nvn_vk.c` |
| m143a | 降频替代方案 | **✗ 提案，未实施** | `nvn_vk.c` —— 只做过 `--check`/回放，**没进任何 build**；误应用会污染基线 |
| m148a ★ | present 同刻回读 + passthru（build #12） | ✓ | `nvn_vk.c` |

## 三、没有补丁的里程碑（诊断/取证单，**未改源码**）

m111a、m112a、m114a、m116a、m117a（公开仓卫生评审）、m118a、m120a、m121a、m123a、m124a、m125a、
m127a、m128a、m129a、m130a（只出守卫方案，代码由 m132a 落）、m131a、m133a、m134a、m137a、m138a、
m139a、m141a、m142a、m144a、m145a、m145b、m146a、m147a —— 均为零 build 的取证/分析单。
它们的结论在各 `logs/<单>/report.md`，`logs/` 不入库。

## 四、完整建树 / 出包步骤

见仓库根 `BUILD-RECIPE.md`（含环境变量、`--stop-daemon`、非入库素材清单、knob 缺省表、
fmtcheck 与 warnings==20 门禁、#9–#12 的 `libgame.so` md5 对照表）。
