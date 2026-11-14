# 奥德赛 → 鸿蒙 NEXT 移植总账（跨会话台账）

> 目标：用 nxnative 引擎把 Super Mario Odyssey 1.4.1 移植到 HarmonyOS NEXT，真机 Mate 80 Pro Max（16G+1T，麒麟芯片 / Maleoon GPU）。
> 分工约束（用户指定）：**引擎/原生层先做；鸿蒙 App 层（ArkTS 壳）等用户明确指令才开工**。23:00 后有 3 亿免费 token，重活排在该窗口；有难度可用 REA skill 逆向。

## 基础设施（已就位 ✅）

| 项 | 路径 / 状态 |
|---|---|
| 移植工作区 | `~/switch-to-harmony/nxnative-ohos`（worktree，分支 `ohos-port`，基于上游 1963fab） |
| 上游参考仓库 | `~/switch-to-harmony/third/{nxnative,MacNativeGames,LASRecomp,mush8,ssbu-recomp}` |
| 游戏包 | `~/Downloads/马里奥奥德赛 [版本1.4.1]/`：base v0 NSP 5.6G + upd v1.4.1 NSZ 312M（title id 0100000000010000 / upd 0100000000010800） |
| OHOS NDK | `/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native`；clang 冒烟编译 aarch64-linux-ohos ELF ✅；`ohos.toolchain.cmake` 在 `native/build/cmake/` |
| DevEco Studio | 6.0.1 Release（兼容 SDK 6.0.0(20)），可先行调试 |
| **关键参考仓库** | 按用途分层（2026-10-27 定稿）：① **平台+图形引擎首选 `~/switch-to-harmony/third/d2r-mobile`**（jhaizhou-ops/d2r-mobile，4797★，家族始祖）：`dm/harmony/`（ArkTS app + napi_ohos.c + touch_ohos.c + front_ohos.c + gameperf_ohos.c）+ `dm/gfx/vulkan/`（NVN→Vulkan 引擎，与安卓共享）+ `android_shim/`（ANativeWindow→OHNativeWindow、VK_OHOS_surface，安卓 Vulkan 文件不改就能编）+ **模块 patcher 已支持 `DM_TARGET=ohos`**（musl 线程槽 `tpidr_el0−272` 已探测）。产品级：Mate 90 Pro Max（Maleoon 955）实测 59.7fps，p50 16.6ms/p99 18.1ms；`__OHOS__` 门控纪律规范。② **OHOS 踩坑清单 `third/dontstarve-harmony`**（rootrd，即 biglawine）：Maleoon 无 BC → 设备端 BC→ASTC 软转码（地形图集被绑 266,425 次）；OC_ES_ESR 非 Apple 恒 0 → page-fault hook 重写；真 BSD socket；hilog 丢长行走文件取证；每帧 draw 数做场景指纹；DST 实测仅 23.7–30fps、p99 250ms（性能警示）。③ **游戏侧档案 nxnative**（本仓上游）：奥德赛 0100000000010000 + TOTK（~17GB，1.4.3 可 60fps）等 80 个 profile，NVN 层修复（nvnTextureCompare/点精灵/-fpreserve-invariance）平台无关可直接继承。④ 可选 `third/TOTK-PS4`（TOTK 静态重编译到 PS4，patches/totk-ps4.patch；无独立 NVN→Vulkan 实现，优先级低）。**结论：无人做过奥德赛/TOTK 的鸿蒙原生移植**，奥德赛/TOTK 的游戏侧知识只在 nxnative |

## 阻塞项

- **[B1] prod.keys 未找到**（已搜 Downloads 与 switch-to-harmony，均无）。提取/解密 NSP+NSZ 硬依赖 → **需要用户把 prod.keys 放到 NSP 文件夹里**（仓库规则：密钥不进 repo、不进家目录、不打印值）。缺失不阻塞 P0/P1/P2 的编译移植，只阻塞 P3。

## 平台耦合审计（初查结论）

- `dm/host`（1.56 万行）：约 20 个文件带 Apple API —— crash.c、hle_thread.c、hle_fpslock.c、hle_uemalloc.c、hle_audio.c、hle_os.c、hle_fs.c、loader.c、probe.c、hle_audren.c、hle_movie_avc.c(VideoToolbox)、hle_haptics/haptics_apple.h 等 → 移植 = 逐个加 OHOS 分支。
- `dm/gfx`（3.9 万行）：Metal 后端**不是**独立的 442 行目录，而是编织在 NVN 层里（hle_nvn.c + nvn_mtl.h + 一批 nvn_*.h）。仓库里的 "Vulkan" 指**游戏侧** Vulkan API（Hades 等），不是宿主后端 → **宿主 NVN→Vulkan 后端以 dontstarve-harmony 的 nvn_vulkan.c 为底改**，不从零写。
- **[2026-10-27 重大更新] 两大风险已由参考仓库回答**：① Maleoon GPU 可跑 Vulkan（Maleoon 920 真机验证）；② 无 BC 支持 → 设备端 **BC→ASTC 软转码**方案已实现并验证（"software decoding of the original BC assets"，105 个再生 shader stage 可从 ROM 资产逐字节复现）。
- 媒体层：视频 H.264 走 VideoToolbox → OHCodec（奥德赛 44 条 B 帧视频里的 41 条是 High/Main profile）；音频 CoreAudio → OHAudio（参考 audio_ohos.c）。
- 输入：奥德赛需要 16 条控制器历史 + 六轴晃动（上游已修好 HID 层，鸿蒙侧接触摸/陀螺仪即可，App 层职责）。

## 阶段与状态

- **P0 可行性验证**（进行中）：
  - [x] OHOS NDK 冒烟编译（21:30）
  - [x] 找到同宗 HarmonyOS 参考实现（21:35，dontstarve-harmony）
  - [ ] dm/host + dm/gfx/nvn 的 C 代码整体编译到 aarch64-linux-ohos，逐个修编译错（23:00 窗口任务；**优先对照 dm/ohos/ 的做法，不重复造轮子**）
- **P1 图形后端**（策略已改）：**移植/吸收 nvn_vulkan.c（7402 行）+ dm/ohos 渲染接入**到 nxnative-ohos，而非从零写。剩余难点是奥德赛的 NVN 用量深度远超饥荒（27,984 shader / 32,864 管线对 vs 105~222；conditional render、同帧反馈绘制、point sprite、几何着色器都是饥荒没有的），状态保真层要按奥德赛的已知修复清单（nvnTextureCompare 映射比较、-fpreserve-invariance 等价物、c0+0x610 点精灵）补齐
- **P2 平台层**（策略已改）：dm/ohos 五件套（xcomp/app/front/pad/audio_ohos）+ 页错误 hook（OC_ES_ESR 在非 Apple 平台恒 0 的坑他们踩过）+ BC→ASTC 转码链，对齐到 nxnative 当前 host
- **输入层（已查明，低成本）**：nxnative 平台接口极简（`dm_input.h`：`dm_pad_set(buttons,lx,ly,rx,ry)`，按键位=nn::hid::NpadButton 一一对应，摇杆 ±32767，另有 DM_BTN_SHAKE 位；多人至 8 手柄）。OHOS 侧先例 = dontstarve `pad_ohos.c`（~180 行）：实体手柄经 ArkTS 页面级按键事件（键码 2301~2315）+ padAxis napi 喂入；**不能用原生监听**（OH_Input_AddMonitor/AddKeyEventHook 要系统权限，普通 HAP 拿不到）；面键按位置收再经 `dm_padmap_face` 换排法，`DM_PAD_LAYOUT=switch` 指认 Switch 布局（用户手柄 LeadJoy 风刃Plus 可切 Switch 模式）。**缺口：六轴/体感 pad_ohos.c 没做**（饥荒不需要）→ 奥德赛甩帽、TOTK 体感瞄准需补 OHOS 传感器服务→DM_BTN_SHAKE/六轴（iPhone 版先例=摇手机；夹子柄夹着手机，手机陀螺仪即手柄陀螺仪，体验自然）；震动同理走手机马达（haptics 层已有接口）。奥德赛挑控制器历史的坑（16 条 GetNpadStates）上游已修，host 侧无需再管
- **P3 提取 + bring-up**（依赖 [B1]）：`scripts/build_ipa_from_nsp.py` 流程改为 OHOS 产物；奥德赛 profile（game.tsv / mem_reserve 0x100000000 4G 布局 / pad_layout full / sdk_patches）直接继承
- **P4 App 层 + 真机 HAP**：**等用户指令**（hap/ 打包参考现成）

## 已知风险

1. ~~Maleoon GPU 的 BC 压缩纹理支持~~ **已解决（参考仓库）**：Maleoon 无 BC，设备端 BC→ASTC 软转码已实现并在 Maleoon 920 上验证。
2. Vulkan 驱动对超大 shader 管线的编译耗时未知（Apple GPU 上 27,984 个要 30 分钟+；Maleoon 无实测数据）。
3. 奥德赛 3 个几何着色器（雷云特效）上游"不翻译"，Vulkan 侧同样要另做模拟。
4. 上游已知缺口照单继承：conditional render 总是画、MultiDraw*IndirectCount CPU 等待、无 amiibo。
5. 饥荒移植自身还有一个开放图形问题（白天地面斑点/半分辨率 RT 链），说明 nvn_vulkan.c 并非无坑，奥德赛更复杂的渲染只会更多。

## 执行环境变更（2026-10-27 21:40）

- **重活移交用户台式机，本 Mac 不再跑自动化**（原 23:04 自动化已删除）。台账与仓库需带到台式机：PORT_PLAN.md 必带；third/ 下 6 个仓库可重新 clone；ohos-port 分支尚无 commit，无需同步。

## 仓库规则（上游 AGENTS.md，必须遵守）

- 游戏文件/密钥/一切提取物只放 NSP 文件夹（`<NSP>/<id>-work/`），**永不进 repo、不进 ~**；不打印密钥值
- 仓库内一切产物（代码/注释/日志/文档/脚本输出）**英文**；红色日志 `[RED] ` 前缀
- 只在里程碑 commit（`git -c user.name=seedds -c user.email=seedds@gmail.com`，ohos-port 分支），改链后跑 `dmsh_chain.py --header` 并同步 `dmsh_chain.h`
- 不在用户设备上签名/安装构建
- 通用修复优先；修根因不打补丁；per-build 知识按 NSO build ID 记录

## Token 预算备注

- 付费时段只做地基（已完成）；23:00 后 3 亿免费 token 窗口做编译修错与图形后端（已排 23:05 一次性自动化）
- 全项目预估 1500 万～3000 万 token（中性），见会话 2026-10-27 估算
