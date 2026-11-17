# BUILD-RECIPE —— 从干净 clone 到出包（build #12 基线）

> 适用分支：`baseline-20261115-build12`（= build #12 可运行基线快照，M149A 落盘）
> 本文件由 M149A 写于 2026-11-15，所有数字都是本机实测值（命令见各节，证据在 `logs/m149a/`，`logs/` 不入库）
> 历史基线：`fc9b20e`（`nightly-20261113`，`nvn_vk.c` 33,043 行）

---

## 0. 一句话

本分支的工作树**就是** build #12 的施工树源码（`nvn_vk.c` **37,388 行 / md5 `9e618e96608bb75f22368d86ed87e053`**）。
clone 下来补上 §4 的三类不入库素材即可构建；若要从 `fc9b20e` 用补丁链复现同一棵树，按 §3.2 顺序
`git apply -p0`，每步用 §3.3 的"行数 / md5 阶梯"验收 —— **链末值与快照值必须逐字节相同**。

---

## 1. 产物身份（认版本只认包内 `libgame.so`，HAP md5 每次签名都变）

| build | 里程碑 | HAP 大小 | HAP md5 | 包内 `libs/arm64-v8a/libgame.so` | 未 strip 中间产物 |
|---|---|---|---|---|---|
| #9 | M132A | 795,175,760 B | — | 20,034,784 B / `044743eebe72b8a5593d5d899c1d9190` | — |
| #10 | M135A | 795,173,808 B | `c6660382d7cf602dbe9e90e93242e26f` | 20,036,736 B / `3f8eccbbc7e8292877498d45c1d0ac18` | 26,032,976 B / `a56e6c69a8272db7ef79cd2486a819c6` |
| #11 | M140A | 795,179,744 B | `16294dc4934770392fc6351f0b2640dc` | 20,038,984 B / `9cdc71fd637cd0e05b1bac838b048909` | 26,038,048 B / `ab693b9f1c5cbd405d4989f60f10bd61` |
| **#12** | **M148A** | **795,177,224 B** | `0f013f77325400729e2d2de1738af7e8` | **20,041,504 B / `b86632b4d92c9871990c10d0359296b0`** | 26,043,136 B / `310e0521ebbbbe3628dd16679f435a48` |

- HAP md5 每次签名都会变（签名时间戳），**判定"是不是同一份代码"只看包内 `libgame.so` 的 md5**。
- ⚠ **`libgame.so` 的 md5 不是严格可复现量：换构建目录就会变 20 字节**（M149A 实测，2026-11-15）。从本分支干净导出、原样拷入不入库素材、在 `/tmp/m149a_verify` 构建，得到 `20,041,504 B / a87063d7740b3ecf53a12d735941e3e9`，与 #12 的 `b86632b4…` **只有 20 字节不同，且全部落在 ELF `.note.gnu.build-id`（offset 724–743）**，其余 20,041,484 字节逐字节相同；把这 20 字节换回 #12 的值，md5 **精确等于 `b86632b4…`**。
  ⇒ **认版本的正确口径**：(a) 大小必须相等；(b) `cmp` 差异只允许在 `.note.gnu.build-id`；(c) 真机侧以启动日志为准 —— `[m148a] pres srcsig on=1` + `[m135a] layout fix on=1`。**不要**因为 md5 不等就判"包不对"，也**不要**为了对齐 md5 回到 `/tmp/m148a_tree` 里重建（那会污染基线输入树）。
- 取包内 so：
  ```bash
  unzip -o -q entry/build/default/outputs/default/entry-default-signed.hap \
        libs/arm64-v8a/libgame.so -d /tmp/pkg && md5 -q /tmp/pkg/libs/arm64-v8a/libgame.so
  ```

---

## 2. 工具链与环境（实测）

| 项 | 实测值 |
|---|---|
| 主机 | macOS 27.2.1 / arm64（Apple Silicon） |
| DevEco Studio | `/Applications/DevEco-Studio.app`，版本 **6.0.2.642**（`buildNumber 243.24978.46.36.602642`） |
| bundled Node | `v18.20.1`（`$DEVECO_APP/Contents/tools/node/bin/node`） |
| SDK | `$DEVECO_APP/Contents/sdk`（`compatibleSdkVersion 6.0.0(20)` / `targetSdkVersion 6.0.0(20)`） |
| 构建器 | `$DEVECO_APP/Contents/tools/hvigor/bin/hvigorw` |

环境变量（照抄即可）：

```bash
export DEVECO_APP="/Applications/DevEco-Studio.app"
export NODE_HOME="$DEVECO_APP/Contents/tools/node"
export PATH="$NODE_HOME/bin:$PATH"
export DEVECO_SDK_HOME="$DEVECO_APP/Contents/sdk"
export HVIGORW="$DEVECO_APP/Contents/tools/hvigor/bin/hvigorw"
```

构建（**前后各停一次 daemon**；脏 daemon 会让产物不刷新，历史踩过）：

```bash
cd mario-odessy-harmony
"$HVIGORW" --stop-daemon
"$HVIGORW" --mode module -p product=default assembleHap     # 一次性构建约 10–15 s（增量）/ 首次全量更久
"$HVIGORW" --stop-daemon
```

产物路径：

- HAP：`entry/build/default/outputs/default/entry-default-signed.hap`
- 包内 so：HAP 里的 `libs/arm64-v8a/libgame.so`
- 未 strip 中间产物：`entry/build/default/intermediates/libs/default/arm64-v8a/libgame.so`

---

## 3. 两条等价的建树路径

### 3.1 A：直接 clone 本分支（止血后的常态，推荐）

```bash
git clone <origin>/switch2hos && cd switch2hos
git switch baseline-20261115-build12
md5 -q mario-odessy-harmony/entry/src/main/cpp/platform/nvn_vk/nvn_vk.c   # 必须 = 9e618e96608bb75f22368d86ed87e053
wc -l < mario-odessy-harmony/entry/src/main/cpp/platform/nvn_vk/nvn_vk.c   # 必须 = 37388
# 再按 §4 补齐三类不入库素材 → 按 §2 构建
```

### 3.2 B：从 `fc9b20e` 用补丁链复现（历史可复现性自证）

顺序（**逐条 `git apply -p0`，没有 `a/ b/` 前缀**）：

```
m105a → m106a → m107a → m108a → m109a → m113a → m115a → m119a → m122a     （树根）
      → m119b                                                             （必须 cd entry/src/main/cpp）
      → m126a → m132a → m135a → m140a                                     （树根）
      → m148a                                                             （树根，#11 → #12 的唯一增量）
```

```bash
git -C <clone> archive fc9b20e | tar -x -C /tmp/tree && cd /tmp/tree
P=/path/to/switch2hos/patches/20261113-milestones
for p in m105a m106a m107a m108a m109a m113a m115a m119a m122a; do git apply -p0 $P/$p.patch; done
( cd mario-odessy-harmony/entry/src/main/cpp && git apply -p0 $P/m119b.patch )   # ← 路径相对 cpp
for p in m126a m132a m135a m140a m148a; do git apply -p0 $P/$p.patch; done
```

**不在链上的两个补丁（存档但不要应用）**：

| 补丁 | 状态 | 为什么不在链上 |
|---|---|---|
| `m110a.patch` | **弃用** | 它引入「选槽自举死锁」（M111A 定位：`m110a.patch:390` 的"是否等于已选槽"把所有笔拦掉）⇒ 自 M113A 起绕开，chain 直接 m109a → m113a。它只能回放到 `m105a..m109a` 状态（35,061 行），对 #11/#12 树 `--check` 失败 |
| `m143a.patch` | **提案，未实施** | M143A 只写方案 + 提案 patch（降频 `w_scan_tick`），**没有 build、没有进任何 build**。它在 #11 链状态上 `--check` 通过，所以**误应用会污染基线** |

### 3.3 链上"行数 / md5 阶梯"（逐步验收，任何一步对不上就是补丁错位）

| 步骤 | `nvn_vk.c` 行数 | md5 | 交叉验证 |
|---|---|---|---|
| base（`fc9b20e`） | 33,043 | `967ec0465850f8b52a471243d60da70f` | 主仓 HEAD 值 |
| m105a | 33,313 | `822e33e0b69e8bcc617111538cd36a79` | |
| m106a | 33,653 | `2dce9bb561d5155199f02c6153894dd8` | |
| m107a | 34,095 | `cdc86a943e963c4072a8289b03284bff` | |
| m108a | 34,481 | `dde692d071e8004224ea74ffaf753d78` | |
| m109a | 35,061 | `7aa447bd1ba922ef57dc187fd65e1add` | = m110a 的预期祖先 |
| m113a | 35,709 | `d45ec0e3d51fcc4939bce6e7a026d4dc` | |
| m115a | 35,709 | `d45ec0e3d51fcc4939bce6e7a026d4dc` | 不动 nvn_vk.c（改 `game_patches.c` / `loader.c`） |
| m119a | 36,034 | `ad65e5406381457d9f28d339ea4c3d41` | |
| m122a | 36,400 | `d882c14318eb19f1fdfab3de882ed8a0` | |
| m119b | 36,983 | `779c1ed52dcea3d6ee7ec72255016fab` | = M147A 记录的 `[P]` 树值 |
| m126a | 36,983 | `779c1ed52dcea3d6ee7ec72255016fab` | 不动 nvn_vk.c（改 `loader.c`/`vk_boot.c`/新增 `m126a_probe.c`） |
| m132a | 37,113 | `411fe752ca9ebe8367865979abd5af62` | = build #9（M135A 记录） |
| m135a | 37,208 | `85277a8207775b6383a1a7a1b599b731` | = build #10（M135A 记录） |
| m140a | 37,310 | `49fcaf191eb9d6a7613df92b523467f1` | = build #11 施工树值 |
| **m148a** | **37,388** | **`9e618e96608bb75f22368d86ed87e053`** | = **build #12 施工树值 / 本分支快照** |

---

## 4. 不入库素材（构建前必须自备；本仓不含游戏内容）

**按 `docs/DERIVED.md` 与本仓策略，以下三类不入库**。要与 build #12 **逐字节**一致，就须是**同一批**素材
（md5 逐项对得上）；只是想跑起来则可用自己游戏副本重新生成（数值会变，`libgame.so` md5 也会变）。

| 路径 | 大小 | md5（build #12 用的那一批） | 用途 / 来源 |
|---|---|---|---|
| `entry/src/main/cpp/platform/generated/shared_fonts_data.h` | 18,275,504 B | `bc75f9f8bdcfc55c638bd129d70b2a43` | 6 槽位共享字体 blob（**字体不可再分发**）；契约为 `docs/DERIVED.md`，影子表 `shared_fonts_config.h` 已在库 |
| `entry/src/main/cpp/games/0100000000010000/generated/runtime_tables.h` | 11,664,242 B | `be487cd6b5d3befa519ee465f60d0e6d` | 重定位表 / 符号地址；由 `nxnative-ohos/tools/prepare_runtime.py`（档案驱动）生成 |
| `entry/src/main/resources/rawfile/shadercache/index.json` | 20,011,781 B | `2a298d1fe617be6358f65afb805357ba` | M95B 烘焙语料（14,510 条） |
| `entry/src/main/resources/rawfile/shadercache/spv.bin` | 673,412,976 B | `643ed107c67d695a392b2eb5fbd2d3ae` | 同上（只进 HAP 打包，不参与编译） |
| `entry/src/main/resources/rawfile/shadercache/index_m27a.json` | 14,196,090 B | `de1255ec5a2a0fe4e18f1997f647074a` | 同上 |
| `entry/src/main/resources/rawfile/smo/main.patched.bin` | 21,833,968 B | `89d568fd8f62fc7a0777712bd01f4642` | **游戏数据**（自己 dump + `game_patches.c` 打补丁后生成，勿入库） |
| `entry/src/main/resources/rawfile/smo/sdk.patched.bin` | 13,960,000 B | `6aef6400b98df49532b01bf79bc61ce9` | 同上 |
| `entry/src/main/resources/rawfile/smo/subsdk0.patched.bin` | 7,982,324 B | `94013f736873bf22e86e231c682ae8c1` | 同上 |
| `entry/src/main/resources/rawfile/smo/rtld.patched.bin` | 66,616 B | `750c05fa8858b5b7876dbb5b964fe114` | 唯一**已入库**的 patched.bin（宿主自造 rtld） |
| `entry/libs/arm64-v8a/libvulkan.so` | 692,752 B | `eb0089df4ce0bbf381876f530d6d5abb` | SwiftShader/Vulkan 预编译件（`*.so` 不入库） |
| `entry/libs/arm64-v8a/libvk_swiftshader.so` | 29,655,968 B | `1ad799194e7634d4b0a21e64eb874c24` | 同上 |
| `entry/libs/arm64-v8a/libc++_shared.so` | 1,262,248 B | `1b580c1be2722ecd726803680a85a0a1` | 同上 |
| `signing-mario/` + `build-profile.json5` 的 `signingConfigs` | — | — | 签名材料（`signing-mario/` 已 gitignore；`build-profile.json5` 里的口令是私有仓内既有状态，**绝不进公开镜像**） |

> 已知缺口（诚实记录）：M149A 落盘时 `shared_fonts_data.h` 的生成脚本已随旧 `game-work/` 丢失，
> 只剩 `docs/DERIVED.md` 的契约。要**逐字节**复现 build #12 的 `libgame.so`，必须持有上表这份 blob
> （或同一批 Windows TTF + 同脚本重建）；换字体就会换 md5 —— 这是**唯一**无法从本仓单独重建的素材。

---

## 5. knob（运行时开关）缺省表

机制：每个里程碑自己的开关文件，**由 app 首次启动时自己创建**（`hdc` 在应用沙箱里建不出新文件），
写入 `"0\n"` = 关；运行期每 2 s 重读，值变化立刻打 `sw knob -> val=`，未变化也每 10 s 打一行心跳。
路径双写（`hdc` 视角 / 沙箱视角）：

```
/data/storage/el2/base/files/<knob>
/data/app/el2/100/base/com.vibecc.smo/files/<knob>
```

| knob | 缺省 | 语义（只列与当前基线相关的） |
|---|---|---|
| `smo_pso_layout` | **1（开）** | M135A：`ci->layout == VK_NULL_HANDLE` 根因修复。置 0 = 回 build #9 行为 |
| `smo_pso_guard` | 0（关） | M132A/M130A：建管线统一守卫 + FAULT 行线程归属；**开它不等于稳**（M133A：同一 knob 一次不崩一次崩） |
| `smo_rt_submerge` | 0（关） | M140A：RT 合并通道（缺省关 ⇒ 关态与 #10 逐字节同） |
| `smo_pres_sig` | 0（关） | M148A：`[m104a] p` 行尾追加 `srcsig[...]`（同刻同图回读） |
| `smo_m80a` | 0（关） | 位掩码（M148A 起 `&15`）：bit0 离屏通道、bit2 调试打印、**bit3 passthru（只开通道不改选源）** |
| `smo_present_auto` / `smo_present_src` / `smo_present_texid` | 0 | 上屏源选择，优先级见 `logs/m147a/knob_recipe.md` |
| `smo_m104a` | 0 | `999` = 合成彩条直送窗口（判"上屏通道通不通"） |
| `smo_m105a`…`smo_m119a` | 0 | 各里程碑探针（0 关 / 1 摘要 / 2+ 明细，逐个见对应补丁头部注释） |

枚举全部 knob（工作树里）：
```bash
grep -rhoE '"/smo_[a-z0-9_]+"' mario-odessy-harmony/entry/src/main/cpp/ | sort -u
```

---

## 6. 门禁（提交 / 出包前自检）

| 门 | 命令 | build #12 实测 |
|---|---|---|
| 格式串 | `python3 tools/gates/fmtcheck.py <nvn_vk.c>` | `calls=1177 ok=1177 bad=0` |
| 语法 | `python3 tools/gates/syntax_gate.py <repo> <tree> platform/nvn_vk/nvn_vk.c` | `rc=0 errors=0 warnings=20`（**20 是基线，不许涨**）；`loader.c` 基线 `warnings=4` |
| 补丁形态 | `grep -qE '^(---\|\+\+\+) [ab]/' patch && echo BAD` | 全部补丁无 `a/ b/` 前缀，`-p0` 可回放 |
| 产物串 | `strings <包内 libgame.so> \| grep -c '<串>'` | `smo_pres_sig`=2、`[m148a]`≥1、`smo_rt_submerge`=2、`[m140a]`=2、`[m135a]`=4、`[m119b]`=15、`[m122a]`=5、`[m119a]`=9、`[m113a]`=13、`[m109a]`=21、`[m107a]`=8、`[m105a]`=9、`[m104a]`=6、`[m102b]`=7、`smo_exec_mmap`=3 |
| 公开前 | `bash tools/public/publish.sh --dry-run` | 见 `AGENTS.md`；**本分支不得直推 github** |

> 语法门需要一次本地 configure 产出的 `entry/.cxx/default/default/debug/arm64-v8a/compile_commands.json`
> （`build/`、`.cxx/` 都不入库，故第一次跑门前先跑一次 `assembleHap` 或在 DevEco 里 sync 一次）。

---

## 7. 已知坑

1. **`patched.bin` 缓存路径**：设备侧 `files/smo/*.patched.bin` 有"缓存命中"路径，历史上能出帧的 boot 都走"未命中/现打补丁"（M123A）。
2. **认版本只认包内 `libgame.so`**：HAP md5 每次签名都变。
3. **knob 必须在 app 启动后才存在**：`hdc` 只能覆盖已存在的 666 文件；写前 `ls`、写后 `od -c` 回读。
4. **`smo_m80a=0` + `smo_m42f=0` + `smo_m7d=0`** ⇒ 离屏通道一次都不调（M147A 实证），别把"空台账"当"黑"。
5. **build daemon**：build 前后都要 `--stop-daemon`，否则可能拿到旧产物。
6. 语料 `shadercache/` 673 MB，**只进 HAP、不入库**；`git status` 里它不会出现（已 ignore）。
