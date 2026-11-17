# round23 · nvn_vk 对抄接线方案（WiringPlan · r2 修订版）

r1：2026-11-01 午后。r2：2026-11-01 晚，吸收复审反对意见 9 条（处置表见 §8）。行号一律以 **HEAD a6d02a6**（"round23: extract platform/ + games/…"）树为准快照于 2026-11-01 21:3x；**实施时锚函数名，不锚裸行号**（本轮已实证行号在并发提交间漂移）。
参照 donor：`third/d2r-mobile`（生产线）+ `third/dontstarve-harmony`（单体线，仅作批 1 历史出处与陷阱注释库）。
雷达引用：`/Users/zpp/VibeCoding/tools/d2r-port-radar`（只读）——ts-0009/ts-0010/ts-0011/ts-0012、组件卡 nvn-shader-pipeline / nvn-compute / nvn-virtual-pool、games/0100CC80140F8000.md、data/notes.json（Seeyajack/d2r-mobile）。
树内布局（a6d02a6 实测）：`mario-odessy-harmony/entry/src/main/cpp/{CMakeLists.txt, platform/(host/* 全部+game_napi.cpp+nvn_vk/+ohos_compat/), games/0100000000010000/, types/, third_party/}`。

---

## 0. 一句话方案

把「CPU 回声 HLE（platform/gfx_bridge.c）+ 902 行 vk 骨架（platform/nvn_vk/）」接到 d2r-mobile 生产线的五段架构
**游戏 → HLE 录制层 → nvn_obj.h 命令契约 → 薄适配层（mtl_* ABI）→ xbg 回放引擎 → 前端独占 WSI**。
8 个批次按依赖排序；批 1 做窗口接线 + 出图链通电 + **boot 路径 present 可达性取证**（小改动，当前树构建已实测绿）。

## 1. 参照系切换（相对 game-work/logs/m4_port_plan.md 的主要变化）

m4 计划写作时 donor 引用 `infra/dontstarve-harmony/dm/gfx/nvn/nvn_vulkan.c`（7402 行单体）；2497e66 收库后两条 donor 血统都在树内（本轮 `find`/`ls` 实证）：

| 血统 | 库内路径 | 形态 | 证据 |
|---|---|---|---|
| dontstarve-harmony（单体线） | `third/dontstarve-harmony/dm/gfx/nvn/nvn_vulkan.c` | 7402 行单体 NVN→Vulkan 重放层 | `find third/dontstarve-harmony -name 'nvn_vulkan*'` 命中 |
| **d2r-mobile（生产线，本方案主线）** | `third/d2r-mobile/dm/gfx/vulkan/` | 13KB 薄适配层 `nvn_vk.c`（13033 B 实测）+ `engine/` 回放引擎（xbg_game.cpp 492047 B 实测，目录 55 项 `ls | wc -l`）+ `INTEGRATION.md` | `ls third/d2r-mobile/dm/gfx/vulkan` |

**选型决策：主线照抄 d2r-mobile 线（薄适配层 + 引擎），单体线不再续写。** 理由：

1. 真机出图证据只在 d2r-mobile 线：radar notes.json 记 Seeyajack/d2r-mobile「鸿蒙移植（dm/ohos），XComponent→Vulkan swapchain，真机已出图，score 80」（本会话 python 读 notes.json 实证）；交接材料记 vivo X200 Ultra（Adreno 830）已出图（donor 库内 `third/d2r-mobile/docs/ANDROID_COLLABORATION.zh-CN.md` 实存）。单体线无出图证据。
2. 引擎把命令回放、管线缓存、描述符、判脏、回收站、SPIR-V 改写整套解耦好；我方 902 行骨架逐批长成 7402 行单体的路径在 m4 批 2/3 处已断（m4_port_plan.md:78-88 自己也只列到第三批）。
3. 引擎与前端解耦的 ABI（`xbg_game.h:12-35` `xbg_game_config{physical,device,queue,present 回调}`，`shader_directory` @:21）恰好接我方已有的 vk_boot/swapchain：前端独占 WSI、引擎吃 device+queue（xbg_game.h:5-8 注释「Guest addresses are directly accessible 64-bit pointers… One game/device per process」）。

**契约头血统修正（r2 重写，取代 r1 的「我方超集头当引擎契约」结论）**：我方 `third_party/d2r_gfx/gfx/nvn/`（65 文件）与 donor d2r-mobile 的 nvn 层是**不同变体**（`diff -rq` 70 行差异；我方 = TOTK/odyssey 扩展，donor = D2R 生产）。超集结论在 mtl_ 函数名层面成立（donor 全部 mtl_ 名都在我方头内，复审 comm -23 复核 + 本会话 grep -o 实测 donor 20 处/我方 48 处），**但在结构体层面不成立且危险**——最关键的是 cmd_t：

- donor d2r-mobile `nvn_obj.h:63-69`：`cmd_t = {magic, recording, cur, end, begin, cb, cb_data, dev}`，注释明言「命令写进游戏给的命令内存（AddCommandMemory），句柄 = 段首地址」（:60-61）；`hle_nvn.c:631` 录 OP_BEGIN 时写 `o->begin = p`；引擎经 `xbg_game_internal.h:34` 声明、`xbg_game.cpp:4073` 调 `xbg_record_command` 按字段访问 `c->cur/c->end/c->cb`（nvn_vk.c:97 实现同）。`grep guest_flags|inline_cur` 在 donor hle_nvn.c **零命中**。
- 我方 `third_party/.../nvn_obj.h:74-89`：`cmd_t` 带 `seg`（注释「records never go into game memory」）、`game_size`（「accounting only」）、`guest_flags/inline_cur/inline_end`（:88-89 静态断言 guest_flags@0x10、inline_*@0x20/0x28）——**语义与 donor 相反**（宿主暂存缓冲 vs 游戏命令内存），且 :88-89 的偏移断言与 donor 布局（end@16）互斥。
- r1 里「cmd_h 字段序已与 cmd_t 一致（guest_flags@0x10…同 donor 断言）」的说法**错误**：其真正出处是 m4_port_plan.md:33（旧单体线口径），对现役两血统 donor 均不成立（单体线 `nvn_obj.h` cmd_t 同样无 guest_flags/inline_*，本会话 sed :83-90 实证）。

结论：**跨界对象（cmd_t/tex_t/pool_t/win_t/prog_t/samp_par 等经 mtl_*/xbg_* ABI 传递的类型）的布置权必须单一化到 donor 契约**，详见批 3c；我方 third_party 超集头保留非跨界用途（nvn_tex.h 布局数学等）。我方既有资产其余部分不推倒：`platform/gfx_bridge.c`（1305 行）继续扮演 donor `hle_nvn.c` 的角色。

## 2. 目标架构（接线完成后的数据流）

```
SMO 客体（唯一 NVN 导入 nvnBootstrapLoader）
  → platform/gfx_bridge.c      NVN 名字调度 + 对象语义 + 命令录制（= donor hle_nvn.c 角色；
                                 名字表 entries[] :934-1262，327 项，本会话 awk/grep 实数）
  → mtl_* ABI                  契约 = donor 血统 nvn_obj.h（{op,size 8 对齐头}+载荷；op 枚举体
                                 :89-97，OP_END@90…OP_COUNT_@96 = 40 个真实 op）+ nvn_mtl.h；
                                 跨界对象按 donor 布局布置（批 3c 单一布置权威）
  → platform/nvn_vk/（收编 donor 薄适配层）  mtl_sub_begin/mtl_exec 收句柄、mtl_sub_commit →
                                 xbg_submit(n,handles)、CPU 副作用包 direct 记录（donor
                                 dm/gfx/vulkan/nvn_vk.c:50-61 实读）
  → engine/（收编 donor 引擎）     xbg_game.cpp execute() 沿 OP_JUMP/OP_CALL 解码（OP_JUMP 分支
                                 xbg_game.cpp:3769），每帧槽一个 VkCommandBuffer 延迟提交+全内存屏障
  → 前端独占 WSI                platform/nvn_vk/nvn_vk.c 的 vk_boot/swapchain_build 持 instance/device/
                                 queue/swapchain，经 xbg_game_config 交出 device+queue；present 回调把
                                 引擎「已转 TRANSFER_SRC」的图 blit 上屏（donor 模式：INTEGRATION.md present 段）
```

与 donor 的结构性差异（前两处有意保留，第三处需批 3 对账）：

- **池内存模型**：donor engine 的 guest 地址是可直接访问的 64 位指针（xbg_game.h:8-9），我方池本就是宿主内存，同模型零改动；donor 的写保护/脏页追踪（wtrack）整段不需要，判脏用比对法（批 6）。
- **窗口来源**：donor 安卓走 JNI `dm_android_vk_set_window`（INTEGRATION.md）；我方走 XComponent（platform/game_napi.cpp surface_set）。
- **契约与对象布置**：跨界类型以 donor 契约为单一布置权威（批 3c）；我方私有扩展挂尾段或随录制语义切换退役。

## 3. 批次（依赖排序；批 1 小而可构建）

### 批 1 · boot 路径 present 可达性取证 + 窗口接线 + 出图链通电（首批）

- **目标**：打通「XComponent 窗口 → vk_boot 成功 → 游戏出图调用驱动真 present」。四件事，**第一件是取证前置**：
  - **(0) present 可达性先核实（r2 新增，复审反对 2 采纳）**：`platform/game_napi.cpp:112` 无条件 `setenv("TOTK_HEADLESS","1",1)`；`platform/loader.c:580` 的 "boot" 分支据此走裸 `boot()`（:537）而非 `runtime_front_run(boot)`。且 `runtime_front_run` 在 OHOS 只有**错签名空桩**：声明 `void runtime_front_run(void(*callback)(void))`（platform/runtime.h:91）vs 定义 `void runtime_front_run(void) {}`（platform/ohos_compat/ohos_stub.c:398）——即 headless 不是可拆开关，是当前唯一可行 boot 路径。旁证：gfx_bridge 注释记录 round48 审计「四提交入口首用 0 次」（提交链当时未被进入）。**故批 1 开工先在真机取证：headless boot 下 `dm_nvn_frame_count`（gfx_bridge.c:488）是否 > 0**；若 present 不可达，批 1 的出图验收前移为「补 frontend 语义或 runtime_front_run 真身」（小批扩容，不并进批 1 首刀）。静态证据，未真机实测，列「先核实」而非「必然断」。
  - (1) `platform/game_napi.cpp` surface_set（:80 起，file-static `g_window` @:77）增转发调 `nvn_vk_set_native_window(window,w,h)`（销毁传 NULL）——当前只存不转发（实读核实）；
  - (2) `platform/nvn_vk/nvn_vk.c` `nvn_vk_set_native_window`（:881-892）在「窗口到达且 !g_on」时触发懒起层。**死锁陷阱（r2 新增，复审反对 4 采纳）：`g_lock` 是非递归 pthread 锁，本函数全程持锁（:883），而 `nvn_vk_init` 入口 lock 同一把锁（:831）——照字面在持锁段内调 init 必死锁**。实施：先 `pthread_mutex_unlock` 再调 `nvn_vk_init`，或提内部无锁变体 `boot_if_window_locked()`（检查 g_tried/g_win 后自锁）。同时重置 `g_tried`（:834 一次性闩）；
  - (3) `platform/gfx_bridge.c` `nvn_queue_present_texture`（:509）接 `nvn_vk_present_clear`（先清色帧证明链路；带源图 blit 属批 2）。窗口路线按 m4 尾巴①定 game_napi 路线，`win_h->native`（nvn_window_builder_set_native_window :438）留作 XComponent 重建对账。
- **源**：donor 窗口生命周期模式 `third/d2r-mobile/dm/android/vulkan_present.cpp`（retain/release、UI 线程不做 Vulkan）；我方既有 swapchain_build 与 nvn_vk_present_clear（nvn_vk.c）。
- **验收**：`./build_hap_mac.sh` exit 0（当前树本会话复跑绿，见 §6）；取证项(0)有真机日志结论；装机 hilog 依序出 `[vk] native window set`、`[vk] NVN->Vulkan backend up`（nvn_vk.c :848/:887 日志串实存）、`QueuePresentTexture … present #N` 且屏幕出清色帧。**真机装机本会话未执行（无设备）**。
- **依赖/风险**：无前置。风险=(0) 取证结论为不可达时本批出图验收顺延。

### 批 2 · 纹理对象真建 + 块线性解排布收编（尾巴③）

- **目标**：贴图从「记名句柄」变真 VkImage：TextureInitialize/Finalize 接 `nvn_vk_tex_create/destroy`（nvn_vk.c 已有建图+staging 上传），upload 路径接线。
- **源**：我方 nvn_vk.c 纹理段（批 1 已移植）；解排布 `third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c`（树内版 sha256 d8f66514…，donor `third/d2r-mobile/dm/gfx/tex/` 同名文件 989859a8…，以树内为准）。
- **落点**：`platform/gfx_bridge.c` TextureInitialize/Finalize/upload 族接线；`CMakeLists.txt` arm64 分支收编 nvn_deswizzle.c；**同摘两处假桩**：`platform/ohos_compat/ohos_stub.c`（nvn_deswizzle 假桩，复审确认仍在）与 `platform/nvn_vk/nvn_vk_stub.cpp:51-52` 空 `nvn_swizzle`（不摘则重复符号，m4_port_plan.md:45 已警告）。
- **验收**：build 过；boot 日志 `upload_storage: block-linear storage needs deswizzle` 拒收红行消失；真机 boot 后纹理计数非零。
- **依赖**：批 1。

### 批 3 · 命令录制语义切换 + 引擎收编 + 跨界对象布置权统一（最大一刀，三子步）

- **目标**：从「cb_account 记账」（cb_account :549，bind/draw 族只累 hw_used）切到 donor 语义的真录制，并整体收编 donor 引擎。
  - **3a 录制语义切换**：我方现状 = 「records never go into game memory」（我方 nvn_obj.h:81 注释）+ game_size 仅记账（:83）；donor = 「命令写进游戏给的命令内存，句柄=段首，用完回调补」（donor nvn_obj.h:60-61 注释；hle_nvn.c:545 段 + hostseg :589-605 实存）。切换内容：gfx_bridge 录制族（EndRecording/Draw/Bind/Copy/UpdateUB…）按 donor `nvn_obj.h:89-97` op 枚举真写命令内存；`AddCommandMemory`/`AddControlMemory` 从记账升级为 donor 语义（这两者是 donor 概念，我方已有记账版）；游戏可见的 cmd_h 布局随 3c 重排。
  - **3b 引擎接入**：收编 `third/d2r-mobile/dm/gfx/vulkan/nvn_vk.c`（13KB）+ `engine/` 15 源进 `third_party/d2r_gfx/gfx/vulkan/`（或 cpp/nvn_engine/）；**donor 血统 nvn_obj.h/nvn_mtl.h 随引擎自包含打包**（引擎 ABI，见 3c）；我方 vk_boot 填 `xbg_game_config`（xbg_game.h:12-35）；present 回调做交换链 blit（替换批 1 清色过渡）；nvn_vk_stub.cpp 的 mtl_* 空桩按批删。
  - **3c 跨界对象布置权统一（r2 重写，复审反对 1 采纳；取代 r1 的「引擎改 include 我方头」方案）**：
    - **规则：凡经 mtl_*/xbg_* ABI 跨界的对象类型，布置权威 = donor 契约头（随引擎自包含的那份）**。gfx_bridge 的 _h 结构按 donor 字段序重排（我方私有扩展挂尾段，FITS 允许范围内），不再由 gfx_bridge 自由定义。对象在 guest 内存里就地布置，guest 侧对字段不透明（按值拷贝也只拷尺寸），布局切换对 guest 安全；FITS 断言随 donor 头编译复验。
    - **cmd_t 清单（漏项补齐）**：donor `cmd_t = {magic, recording, cur, end, begin, cb, cb_data, dev}`（nvn_obj.h:63-69）；donor hle 写 `o->begin`（hle_nvn.c:631），引擎/适配层读 `c->recording/cur/end/cb`（nvn_vk.c:97、engine xbg_game.cpp:4073→xbg_record_command）。我方 cmd_h 的 `guest_flags/inline_cur/inline_end`（我方 nvn_obj.h:88-89 断言 @0x10/0x20/0x28）是我方血统私有、与 donor 布局互斥——**随 3a 录制语义切换一并退役**（inline 快路径是 TOTK 宿主录制缓冲时代的产物；若 SMO 实测需要可挂尾段重造）。我方 `seg/ctl/cmd_base/cmd_used/ctl_used/game_size` 大半在 donor 语义里有对应（AddCommandMemory/AddControlMemory/控制内存记账），按 donor 字段名重排进 cmd_t 后的尾段；`rec_start/hw_used/last_rec/hw_start` 等纯宿主统计挂尾段。
    - **tex_t/pool_t 等同规则**：我方 pool_t 多 `game_mem/host/span`（TOTK vpool 扩展）、tex_t 字段序不同——跨界布置一律按 donor 序，扩展字段挂尾段；vpool 扩展字段当前无人消费（MapVirtual 是指针重定向），批 8 取证后再定去留。
    - **两套头共存的纪律**：donor 契约头只服务引擎+适配层+gfx_bridge 的跨界类型定义；我方 third_party 超集头保留给非跨界代码（nvn_tex.h 布局数学、totk 工具头）。同一对象指针绝不允许两种布局解释——批 3 验收含「grep 全工程跨界类型的第二定义归零」。
- **先同步后异步**：照 INTEGRATION.md 红线「禁异步翻译」（:22），引擎接入即同步翻译基线；donor hle 翻译队列（交接材料引 hle_nvn.c:899-1055）留性能轮。
- **验收**：build 过 + `nm` 检查 `xbg_submit` 已解析（改造 donor check_build.py，`third/d2r-mobile/dm/gfx/vulkan/check_build.py` 在库）；boot 期 submit→引擎解码、5 分钟无 fatal；`SMO_VK=0` 回退完整；跨界类型第二定义归零检查。
- **依赖**：批 2。
- **风险**：SMO 表外 op 按红桩记名不猜语义（donor 40 op 是 D2R 实测集）。

### 批 4 · 描述符与绑定层

- **目标**：TexturePool/SamplerPool Register 与 Bind 族从记名空操作（register_texture :915 只打日志；bind 族 :584 起只 cb_account）变为真登记 + 真录制。
- **源**：donor 语义 = 全局 ID 兼容表（donor nvn_vk.c:66-67 `mtl_reg_tex/mtl_reg_samp → xbg_legacy_register_texture/sampler`）；INTEGRATION.md 差异第 1 条（多同号池歧义为已知限制继承）。
- **落点**：`platform/gfx_bridge.c` Register 族调 mtl_reg_*（对象指针经批 3 snap 口径）；Bind 族真录 OP_BIND_*（donor op 枚举 :93-96）。
- **验收**：build 过；Register 计数=池条目数。**引用断链修正（r2，复审反对 6 采纳）**：gfx_bridge.c:907-909 注释引的「donor :274/:277」对不上任何现役 donor 文件——`grep texture_pool_get_size|TexturePoolGetSize` 在 d2r-mobile 与 dontstarve-harmony 的实现文件均零命中（只有 names.txt/nvn_protos.h/nvn_thunks.S 的注册名，本会话实跑）；「GetSize 返回创建时条目数」按 NVN 语义自证，不依赖该引用，验收以自证口径执行。依赖：批 3。

### 批 5 · 管线与着色器链

- **目标**：ProgramSetShaders（platform/gfx_bridge.c:898，r1 行号 :905-910 已漂，实锚函数名）从「只记阶段数」到 SPIR-V 直载 VkShaderModule + 管线缓存 + 状态重放（引擎自带）。
- **SMO 着色器资产链（前置取证）**：上游 odyssey 港链路 = `totk_xlate`（Maxwell→SPIR-V）→ `.spv` → SPIRV-Cross→MSL（`third/MacNativeGames/games/odyssey/scripts/prepare_odyssey_shader_seed.py` :88 totk_xlate、:221 *.spv 中间产物，实读）。我方 Vulkan 线直接吃 `.spv`：走 donor `.xgs` 包线（键=NVN 控制段 SHA-256 前 16 hex，xbg_game.cpp:1776 注释明写 0x880；管线缓存 xbg_game.cpp:897）或按程序 serial 直载自建键。**批首取证：SMO 控制段实际长度**（radar ts-0012：D2R 按 0x880、TS 0x878，错长度哈希必不命中；donor 该长度硬编码须参数化）。SMO 库版本/控制段长度/几何着色器有无均未取证（ts-0011 是 TS 侧 68 GS）。
- **落点**：gfx_bridge ProgramSetShaders 拷 sd+control 快照（donor hle 同位语义）；引擎 shader_directory 指 HAP 内预置 .xgs/.spv（xbg_game.h:21）。
- **验收**：build 过；真机首帧管线创建无 shader-not-found 红行；管线缓存二次启动命中。**块外前置**：SMO romfs/着色器提取链（Mac 侧）。依赖：批 4。

### 批 6 · 贴图补全（深度上传 / BC 软解 / 判脏 / 回收站 / 异步上传）

- **现状断点**：深度纹理拒收（nvn_vk.c is_depth 分支，批 1 移植时注释引 donor do_copy_b2t 同位）；**措辞修正（r2，复审反对 7 采纳）**：soft_bc 在 upload_level 不是拒收——按 `vkfmt_bpp_guess` 把 BC 块当未压缩字节直传（内容错、不返 0，nvn_vk.c :660-663 快照）；真正拒收在 upload_storage（:713-716）。Maleoon 920 BC 全不认（nvn_vk.h bc=0x0 注）；nvn_bcdec.c 在树未编入；上传全同步等栅栏、tex_destroy 整队列 WaitIdle。
- **源**：donor 引擎 BC 策略（engine/xbg_bc_policy.h + third_party/bcdec；INTEGRATION.md「BC 路径依 enabled_core.textureCompressionBC，无 Maleoon 特判」）；树内备选 `nvn_bc1astc.c`（BC1→ASTC 转码）；判脏/回收站语义（m4_port_plan.md:31-32 摘录 (池,偏移,描述符) 认图比对）。
- **落点**：CMake 收编 bcdec；深度走重放 OP_COPY_B2T；soft_bc 上传前 CPU 解码（upload_level 也要拦——把「错传」改成「先解码」）；判脏 memcmp 摘要；GRAVE 延迟销毁；暂存环。
- **验收**：build 过；真机深度/BC 红行消失、颜色正确。依赖：批 3 + 批 2。

### 批 7 · 提交同步语义换真值

- **目标**：从「提交即退休」换真 GPU 时序。现状（新布局快照）：SyncWait 恒 ALREADY_SIGNALED（nvn_sync_wait :494）、fence_sync 记代际即退休（:492）、回执环 submit 时写真内存（submit :498 + :637-730 段）。
- **源**：donor：栅栏依据真 VkFence；OP_SIGNAL_EVENT 先完成此前 GPU 工作再发布 CPU 事件值；有限 timeout 返 0（INTEGRATION.md 同步段）；引擎 xbg_legacy_wait/last_fence（donor nvn_vk.c:62-63）。
- **落点**：FenceSync/SyncWait/QueueWaitSync 接 xbg_legacy_*；回执环改挂真实完成时序；retire-immediate 注释族整段更新。
- **验收**：build 过；真机长会话无「事件值先于 GPU 工作」竞态；present 计数（dm_nvn_frame_count :488）持续递增。依赖：批 3。

### 批 8 ·（条件批）Compute 与虚拟池——先取证再开工

- **Compute**：现状只记账（dispatch_compute/indirect :755-756 在 CB_REC 记名族，r1 行号 :762-763 已漂）；引擎原生支持 OP_DISPATCH（op 枚举 :95）。SMO compute 依赖度未取证——radar ts-0010 是 TS 侧数据（694 计算作业）；先对 SMO boot 日志普查 dispatch 计数，零则保持红桩，非零随批 4 真录（引擎零新增）。
- **虚拟池**：radar ts-0009（blocker，open）：宿主「GPU 地址=CPU 地址」与 MapVirtual 稀疏池冲突；我方现状 = donor-lite 指针重定向（MapVirtual 分支，注释在案 round12a SMO 150s 未调）。维持大声报告不崩；若踩到按 ts-0009 fix.approach（先分析 MemoryPoolBuilderSetFlags 与页粒度）评估。批 3c 的 pool_t 尾段 vpool 扩展字段（game_mem/host/span）届时一并定去留。
- **验收**：取证报告进 round-notes；两条都不踩则零代码。

## 4. 不搬运清单（d2r/Adreno 绑定物）

| 项 | 出处 | 处置 |
|---|---|---|
| Adreno 830 专项策略 | engine/xbg_adreno_policy.h | 不收编；Maleoon 920 结论自证（本项目尚无 arm64 真机 vk 执行记录） |
| D2R 格式回退 | donor 轮次注释 | 仅作陷阱注释，回退表以我方 probe_bc 实测为准 |
| host-game-coupling 钩子 | xbg_game.cpp:96-103、xbg_cam.h、xbg_bright_spv.h | 弱链接不链即关，随引擎原样带入，SMO 侧无需实现 |
| D2R .xgs 资产 | donor 着色器目录 | 不搬；SMO 自产 |
| Metal 线 | third/d2r-mobile/dm/gfx/nvn/nvn_mtl.m | 不编入（OHOS 无 Metal）；仅当契约参照 |
| SPIR-V 改写五模块 | xbg_spirv_quad/warp/ldc/temps/raster | 随引擎整包收编（批 3），SMO 触发与否未取证，先带全量按日志关 |

## 5. 红线与陷阱合并清单

1. 只开枚举到场的设备扩展（装载器对没开的留 NULL → SIGABRT）。
2. 交换链 usage 加 TRANSFER_DST 打掉设备；动态渲染写交换链图 GPU 故障，必须经典 render pass（我方 swapchain_build 已是绿写法）。
3. 上传后必须转 SHADER_READ_ONLY（第 85 轮 1131 次绑定全错）。
4. preTransform 声明≠内容转动。
5. 通道开着发不了 transfer。
6. hilog 环形缓冲会被刷屏挤掉，关键取证文件双写；日志标记用 ASCII。
7. 安卓红线整组继承（INTEGRATION.md）：禁 SIGSEGV/mprotect 脏页追踪与 remap 探针（:21 多同号池、:22 禁异步翻译、:25 TRANSFER_SRC——复审已逐条复核行号）；present 只收已转 TRANSFER_SRC 的图；CPU 单调钟近似上屏时刻（donor nvn_vk.c:73-85），不得冒充真实上屏时间。
8. 单队列假设（donor nvnCommandBufferWaitEvent 单队列空实现）；批 3 盯队列数。
9. 每游戏每进程一个 device（xbg_game.h:8）；fatal 后需重启进程。
10. 假桩收编纪律：nvn_deswizzle/nvn_swizzle/totk_vpool_backing 假桩（ohos_stub.c、nvn_vk_stub.cpp:50-52）收编真身同轮摘除。
11. 着色器键哈希长度按库版本参数化（radar ts-0012）。
12. **死锁陷阱（r2 新增）**：nvn_vk.c 的 `g_lock` 非递归，持锁段内禁调 `nvn_vk_init`（批 1 落点 2 的实施纪律）。
13. **契约单一权威（r2 新增）**：跨界对象只许一份布局定义（donor 契约）；批 3 验收含第二定义归零 grep。
14. **TOTK_HEADLESS 前提（r2 新增）**：OHOS 壳当前只有 headless boot 一条活路（runtime_front_run 是错签名空桩，runtime.h:91 vs ohos_stub.c:398）；涉及出图/渲染循环的验收前必须先证 present 可达（批 1 第 0 项）。

## 6. 构建基线（本会话实跑证据）

- **post-refactor 树（HEAD a6d02a6）复跑**：`./build_hap_mac.sh` → `BUILD SUCCESSFUL in 211 ms`，exit 0（增量；本会话 2026-11-01 晚）。复审方独立实跑 exit 0（BUILD SUCCESSFUL in 4s217ms，记录在复审意见 8）。
- **pre-refactor 树（2497e66）**：本会话早前两跑 exit 0（第二跑 touch nvn_vk.c+gfx_bridge.c 强制重编，ninja 557ms，signed HAP 19:56 更新，63,468,834 B）——证明批 1 两个关键文件在 arm64 分支可编译可链接；布局漂移不影响该证据的内容有效性。
- 未跑/未证（如实声明）：真机部署与出图验证未执行（无设备）；clean 全量构建未跑（均增量；x86_64 壳分支未触发）；donor 引擎 OHOS 侧试编未做（批 3 第一验收项）；SMO 控制段长度/compute 依赖度/几何着色器未取证；hle_nvn.c:545-668 只抽查起止未逐行；SOURCE.json 29/34 一致性未复核。

## 7. 与交接材料/r1 的出入修正（r2 汇总）

1. 「donor 不在仓库」blocker 已过时（2497e66 收库；r1 已修）。
2. r1 的「cmd_h 字段序与 donor 一致」**错误**：出处是 m4_port_plan.md:33 旧单体线口径，对 d2r-mobile 与 dontstarve-harmony 现役头均不成立（§1 详证）；批 3c 据此重写。
3. r1 的「引擎改 include 我方超集头」方案**撤销**：跨界布置权统一到 donor 契约（批 3c），超集只保非跨界用途。
4. 引用断链：gfx_bridge 注释的「donor :274/:277」（TexturePoolGetSize）在现役两血统均无实现出处（grep 实证），批 4 改自证口径。
5. 计数修正：mtl_ 记号 donor 20/我方 48（r1 写 19/47，系 grep -c 行数 vs grep -o 次数口径）；engine/ 55 项（r1 写 60 文件）；名字表 327 项、范围 :934-1262（r1 写 :941-1270 为旧布局行号，327 计数成立）。
6. 批 6 措辞修正：upload_level 对 soft_bc 是「按未压缩口径错传」非拒收；真拒收在 upload_storage。
7. 行号漂移声明：本文行号为 a6d02a6 快照，实施一律锚函数名（本会话实测 gfx_bridge 1312→1305 行、多处 ±7 行漂移）。

## 8. 复审意见处置表（9 条）

| # | 意见 | 处置 |
|---|---|---|
| 1 | 批3 cmd_t 契约冲突（donor 无 guest_flags/begin 缺失/语义相反/3c 漏 cmd_t） | **成立，全部实证采纳**：donor nvn_obj.h:63-69 + hle_nvn.c:631 + engine :4073 + 我方 nvn_obj.h:74-89 与 :88-89 断言；3c 重写为跨界对象单一布置权威=donor 契约，cmd_t 专项清单补齐 |
| 2 | 批1 TOTK_HEADLESS 上游开关未论证 | **成立，采纳为批1 第 0 项**：loader.c:580 分支 + runtime_front_run 错签名空桩（runtime.h:91 vs ohos_stub.c:398）+ round48 提交链 0 首用旁证；列「先核实」 |
| 3 | 施工路径漂移 platform/ | **成立**：实为已提交重构 a6d02a6（非未提交）；全文档落点已刷 platform/ 前缀并锚函数名 |
| 4 | 批1 落点2 死锁陷阱 | **成立，采纳**：g_lock 非递归 + init 同锁（nvn_vk.c :883/:831）；批1 落点写明解锁后调或无锁变体；陷阱清单 +12 |
| 5 | 行号错位清单 + 批3b 路径笔误 | **成立**：全部按新树 grep 重锚（ProgramSetShaders :898、dispatch :755-756、op 枚举 :89-97、config :12-35、shader_directory :21）；md 路径笔误已改 third/d2r-mobile |
| 6 | 批4 donor :274 断链 | **成立**：grep 实证两血统均无 TexturePoolGetSize 实现；批4 改自证口径、删该引用 |
| 7 | 计数偏差 + 批6 措辞 | **成立**：20/48、55 项、327/新范围已更正；批6 措辞改「错传 vs 拒收」 |
| 8 | 已核实为真清单 | 收录进 §6/各批验收，无需改动 |
| 9 | 未验证项声明 | 收录进 §6 未跑/未证，维持如实声明 |
