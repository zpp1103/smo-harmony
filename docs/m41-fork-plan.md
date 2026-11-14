# M41A：nx2android `gfx_vk_*` → OHOS 移植可行性评估与施工方案

- 单号：M41A（评估轮，本轮**不改代码、不构建、不跑 boot**）
- 日期：2026-11-10
- 决策前提（已定）：用 `infra/nx2android/nxnative/dm/android/gfx_vk_*.c`（6,986 行）替换自研
  `mario-odessy-harmony/entry/src/main/cpp/platform/nvn_vk/nvn_vk.c`（30,198 行）。
- 我方症状基线：几何正确、颜色塌成纯值、上屏靠猜槽（`nvn_vk.c` 里 M40AU…M40AZ 一整串 `pres_pick` 探针即为此而生，
  见 `nvn_vk.c:276-604`）。

> 证据约定：本文所有 `file:line` 均相对 `/Users/zpp/VibeCoding/switch2hos/`。
> `NX/dm/...` = `infra/nx2android/nxnative/dm/...`；`OUR/cpp/...` = `mario-odessy-harmony/entry/src/main/cpp/...`。

---

## 0. 结论先行

**移植可行，且比预想的近得多 —— 因为两边是同一支 d2r-mobile 血统，共用同一条 C ABI（`nvn_mtl.h` 的 `mtl_*`）。**

但有一个必须先讲清的结构事实（它决定工作量）：

| 层 | nx2android | 我方现状 | 关系 |
|---|---|---|---|
| NVN 入口 / 录制（`hle_nvn.c`） | `NX/dm/gfx/nvn/hle_nvn.c` 2,633 行，393 个 `NVN()` 跳板 | `OUR/cpp/platform/gfx_bridge.c` 6,883 行，98 条手写符号表（`gfx_bridge.c:6435`） | **不同实现、同一血统**；nx2 是标准 d2r HLE，我方是自建薄 HLE |
| 命令记录 ABI | `NX/dm/gfx/nvn/nvn_obj.h`（219 行）+ `nvn_rec.h`（303 行） | `OUR/cpp/third_party/d2r_gfx/gfx/nvn/nvn_obj.h`（**315 行，BOTW/TOTK 分支**），**无 `nvn_rec.h`** | **二进制不兼容**（见 §3.3 的 opcode 枚举实证） |
| 回放后端 | `gfx_vk_*.c` 6,986 行（Android Vulkan） | `nvn_vk.c` 30,198 行（OHOS Vulkan，血统来自 `third/dontstarve-harmony/dm/gfx/nvn/nvn_vulkan.c` 7,402 行） | 同一 `mtl_*` 接口的两个实现；我方已偏离（0 个 `mtl_*`，151 个 `nvn_vk_*`） |
| 宿主契约 | `NX/dm/host/dm.h` 146 行 | `OUR/cpp/third_party/d2r_gfx/host/dm.h` **20 行（我方自写桩）** | 缺口集中在这里 |

**所以这不是「引入外来后端」，而是「把后端 + 录制层一起换成 nx2android 那一支，再补一层 dm 宿主桩」。**
关键交点是 `nvn_mtl.h` 的 `mtl_*`，而这一层我方**已经有一份**（`OUR/cpp/third_party/d2r_gfx/gfx/nvn/nvn_mtl.h`，76 行，是 nx2 47 行的**超集**）。

**判定：需胶水层（约 1,200–1,600 行宿主桩 + OHOS 前端），不接受「零胶水直编」。**
理由：`gfx_vk_*` 依赖的 38 个项目内外部符号里有 30 个由 `dm/host` 提供，而我方 `dm.h` 只有 20 行、缺 22 个钩子（§1.3 清单）。

---

## 1. Q1 宿主契约：`gfx_vk_*.c` 期望谁提供什么

### 1.1 契约总头：`nvn_mtl.h`（唯一的 ABI 面）

`NX/dm/android/gfx_vk.h:1-4` 自述：*"the Vulkan backend behind the replay interface of nvn_mtl.h (what nvn_mtl.m implements on Metal)"*。
全后端对外只暴露 `mtl_*`：

| 函数 | 由哪个文件实现（nx2） | 谁调用 |
|---|---|---|
| `mtl_init` | `gfx_vk_core.c:1107` → `vk_init()` | `dm/host/main.c`（我方：`OUR/cpp/platform/vk_boot.c`） |
| `mtl_pool_add` / `mtl_pool_del` | `gfx_vk_core.c:590` / `:649` | `hle_nvn.c`（`nvnMemoryPoolInitialize` / `Finalize`） |
| `mtl_alias_drop_pool` | `gfx_vk_core.c:697` → `vk_tex_drop_pool` | `hle_nvn.c` 池 Finalize |
| `mtl_addr` | `gfx_vk_core.c:692` | `hle_nvn.c` 反查缓冲（CPU 地址 → VkBuffer） |
| `mtl_sub_begin` | `gfx_vk_core.c:1115` | `hle_nvn.c` `nvnQueueSubmitCommands` 前 |
| `mtl_sub_event` / `mtl_sub_counter` | `gfx_vk_core.c:1142` / `:1143` | 同上（完成后写事件 / 计数器） |
| `mtl_sub_copy` / `mtl_sub_fill` | `gfx_vk_core.c:1157` / `:1166` | 同上 |
| `mtl_sub_index` / `mtl_exec_rest` | `gfx_vk_core.c:1286` / `:1287`（空实现） | 同上 |
| `mtl_sub_commit` | `gfx_vk_core.c:1357` | 同上，返回栅栏值 |
| `mtl_last_fence` / `mtl_wait` / `mtl_finish` / `mtl_drain` | `gfx_vk_core.c:1403` / `:1404` / `:1413` / `:1414` | 同步对象 / 事件等待 |
| `mtl_exec` | `gfx_vk_exec.c:2410`（`exec_ops` 逐 op 回放） | `hle_nvn.c` 提交时 |
| `mtl_set_depth_zero` / `mtl_set_origin_lower_left` | `gfx_vk_exec.c:39` / `:40` | `mtl_init` 后按游戏配置 |
| `mtl_set_frame` / `mtl_gpu_ms` | `gfx_vk_core.c:1296` / `:1297` | `dm/host` 每拍 |
| `mtl_reg_tex` / `mtl_tex_release` / `mtl_tex_init` / `mtl_tex_cpu_written` / `mtl_tex_gpu_owned` / `mtl_read_texels` | `gfx_vk_tex.c:673` / `:516` / `:509` / `:452` / `:534` / `:892` | `hle_nvn.c` 纹理对象生命周期 |
| `mtl_reg_samp` / `mtl_samp_seq` / `mtl_set_tex_pool_n` | `gfx_vk_tex.c:732` / `:669` / `:686` | 同上 |
| `mtl_present` / `mtl_present_wait` / `mtl_presented_time` | `gfx_vk_present.c:1010` / `:1087` / `:1094` | `nvnWindowPresentTexture` |
| `mtl_present_qos` / `mtl_has_window` / `mtl_display_hz` / `mtl_fps_target` | `gfx_vk_present.c:1096` / `:1099` / `:1100` / `:1101` | `dm/host` 节拍器 |
| `mtl_link_active` / `mtl_link_wait_tick` | `gfx_vk_present.c:1097` / `:1098`（桩：返回 0） | LDN 联机（SMO 不用） |
| `mtl_xv_window_tex` / `mtl_prog_release` | `gfx_vk_present.c:1102` / `:1103`（桩） | 窗口贴图登记 / 程序释放 |
| `mtl_res_counts` / `mtl_res_snapshot` | **未在 `gfx_vk_*` 中实现**（`nvn_mtl.h:27-28` 有声明） | `dm_res_report_register`（诊断，可空桩） |
| `bf_want_note` | **未实现**（`nvn_mtl.h:12`） | 盲测清单（`bf_want.tsv` 为空，可空桩） |

> `NIX`→ 这 2 个是 nx2android 自己也没实现的：`nvn_mtl.h` 里声明了但 `gfx_vk_*` 没有定义，只能由宿主桩兜。
> 我方 `OUR/cpp/third_party/d2r_gfx/gfx/nvn/nvn_mtl.h`（76 行）比对结果：nx2 的 47 行全部被包含，
> 我方多出 29 行（`mtl_probe_*` 24 个 + `mtl_sub_capture_ubos` + `mtl_ui_ghost*` + `mtl_drain` + `mtl_vbuf_stats` 等 TOTK/BOTW 期能力）。
> **结论：把 nx2 后端接上我方 76 行头不会有 ABI 冲突，多出的 29 个入口做成空桩即可。**

### 1.2 `gfx_vk_*.c` 调出去的其他符号（真实外部依赖，共 38 个）

用「调用 − 本目录定义」做集合差，项目前缀符号 38 个，逐条定位定义处：

| 符号 | 定义处 | 性质 |
|---|---|---|
| `nvn_hm_put/get/del` | `NX/dm/gfx/nvn/nvn_hmap.h`（`static inline`） | **头内联，白送** |
| `nvn_fmt_bpb` | `NX/dm/gfx/nvn/nvn_tex.h` | 头/`nvn_tex.c`，我方 `OUR/cpp/platform/nvn_tex.c` 已有同义实现 |
| `vk_tex_slot_handle` | `NX/dm/android/gfx_vk_slot.h:5`（`static inline`） | 白送 |
| `nvn_dirty_since_span` / `_span` / `_protect` / `_release` / `_released` / `_runs` / `_now` / `_defrag` / `_stats` | `NX/dm/gfx/nvn/nvn_dirty.c`（290 行） | **需一并移植**（脏页 mprotect 追踪） |
| `nvn_dpool_base` | `NX/dm/gfx/nvn/hle_nvn.c`（`gfx_vk_exec.c:1116` 显式 extern） | 由 HLE 提供 |
| `nvn_ft_written` | `NX/dm/gfx/nvn/hle_nvn.c`（`gfx_vk_tex.c:372`） | 由 HLE 提供 |
| `nvn_jobs_queued` / `nvn_replaying_job` | `NX/dm/gfx/nvn/hle_nvn.c`（`gfx_vk_tex.c:513`） | 由 HLE 提供 |
| `nvn_bc_decode` | `NX/dm/gfx/tex/nvn_bc.c` + `.h` | **需移植**（BC 软解码） |
| `nvn_deswizzle` | `NX/dm/gfx/tex/nvn_deswizzle.c`（67 行） | 我方已有（`OUR/cpp/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c`，已在 arm64 构建，`CMakeLists.txt:37`） |
| `game_profile.h` | **构建期生成**：`NX/dm/games/game.py header <id> <out.h>`（`NX/dm/android/build.py:37`） | **需跑一遍生成器**，不是手工文件 |
| `dm_front_window` | `gfx_vk_present.c:18`（**weak**，宿主可覆盖） | OHOS 侧必须提供 |
| `dm_external_active` | `gfx_vk_present.c:18` 附近 weak | 桩 0 |
| `dm_perf_frame` / `dm_perf_window` / `dm_perf_frame_cost` | `NX/dm/android/perf_android.c`（`gfx_vk_present.c:1009`） | **需 OHOS 实现**（可先空桩） |
| `dm_picture_rect` / `dm_overlay_redraw` | **本目录自己定义**（`gfx_vk_present.c:66` / `:972`） | 内部 |
| `dm_menu_state` / `dm_text_entry` / `dm_save_note` / `dm_save_rows` / `dm_overlay_*`（15 个） | `NX/dm/android/front_android.c` + `overlay_layout.h` | **UI/覆盖层**，OHOS 需自建或整块裁掉 |
| `LOG` / `die` / `VKCHK` | `NX/dm/host/dm.h`（`gfx_vk.h:18-20`） | 我方 `hilog` 适配（已有 `vk_boot_logf`） |

### 1.3 `dm/host/dm.h`（146 行）里 `gfx_vk_*` + `hle_nvn.c` 真正用到的钩子

对 `NX/dm/gfx/nvn/hle_nvn.c` 做同样集合差，宿主必须提供的 `dm_*` / `front_*` 钩子共 **28 个**：

```
dm_backtrace            dm_brightness_now        dm_detclock_enabled
dm_fault_hook_register  dm_fps_lock_tick         dm_fps_state
dm_gfx_enter            dm_gfx_leave             dm_gfx_suspend
dm_gl_drain             dm_glslc_gpu_code        dm_guest_frame_tick
dm_guest_work_qos       dm_guest_write_prepare   dm_host_work
dm_longframe_note       dm_low_power_now         dm_main_yield_stats
dm_nvn_frame_count      dm_nvn_present_hold_ms   dm_nvn_sub_cpu_ns
dm_perf_frame_cost      dm_perf_frame_start      dm_perf_present
dm_perf_replay_thread   dm_present_period_adapt  dm_sdk_major
dm_thermal_now          dm_thread_cpu            dm_vk_drain
dm_yield_top            front_screen_max_fps
```

加上 `gfx_vk_*` 侧：`dm_front_window`、`dm_external_active`、`dm_perf_window`、`dm_menu_state` 一族 → **约 34 个**。

**我方现有对照（`OUR/cpp/third_party/d2r_gfx/host/dm.h`，仅 20 行）**：只有 4 个命中
（`dm_detclock_enabled`、`dm_fault_hook_register`、`dm_guest_frame_tick`、`dm_guest_write_prep_register`、`dm_guest_write_prepare` → 5 个）。
**缺口 ≈ 29 个钩子，全部是空桩级（返回值即可），不是算法。这是胶水层的主体，但也是最便宜的部分。**

### 1.4 结论（Q1）

- 契约面**只有一层**：`nvn_mtl.h` 的 `mtl_*`（约 45 个函数），我方已有同源头文件。
- 后端向外的真实依赖**只有 38 个项目内符号**，其中 3 个是头内联（白送）、5 个由 `hle_nvn.c` 自供（随录制层一起进来）、
  2 个是构建期生成（`game_profile.h`）、**28 个是宿主钩子**。
- 因此「宿主契约差集 = 28–34 个 `dm_*` 钩子 + `dm_front_window`」这一条，是唯一需要我方新写的接口层。

---

## 2. Q2 我方对应物：`nvn_vk.c` 里对应的实现点

我方 30,198 行按段落点（用 `grep -n` 段落注释取证）：

| 能力 | 我方落点（`OUR/cpp/platform/nvn_vk/nvn_vk.c`） | nx2 对应 | 可直接复用？ |
|---|---|---|---|
| 设备建立 / 扩展试探 | `:1502` 段「设备建立」，`:1639` `vk_boot()` | `gfx_vk_core.c` `vk_init()` | **不复用**（被 `g_ohos_nb_stripped` 逻辑污染，见 §4） |
| `VK_OHOS_native_buffer` 剥离 | `:1515` `g_ohos_nb_stripped`，`:1516` `#define PROX_EXT`，`:1576-1586` 剥离逻辑 | 无（Android 有 `AHardwareBuffer`） | **必须保留**（OHOS 独家知识，见 §4 分支 B） |
| 交换链 | `:1992` 段，`:1993` `swapchain_destroy()`，`:2014` `swapchain_build()` | `gfx_vk_present.c` `swap_make()`（原 `:162`） | **部分**：OHOS surface 创建可复用，acquire 必须绕开 |
| BC/ASTC 能力探测 | `:2116` `probe_bc()` | `gfx_vk_core.c` `has_bc/has_astc`（`gfx_vk.h:53`） | 逻辑可参考，代码不复用 |
| `NVNformat`→`VkFormat` 映射 | `:2141` 段 | `gfx_vk_tex.c` `vk_fmt()`（表 + 108 行格式表） | **不复用**（nx2 表更全，含 `dec` 软解字段） |
| staging / 上传缓冲 | `:2211` 段，`:2215` `stage_grow()` | `gfx_vk_core.c` `vk_scratch()`（`gfx_vk.h:81`） | 不复用 |
| 纹理建图 / 上传 | `:2261` 段，`:2580` `r_view_for_dim()`，`:2713` `nvn_vk_tex_upload_level()` | `gfx_vk_tex.c` `vk_tex()`（`gfx_vk.h:125`） | 不复用 |
| 渲染目标槽表 | `g_rt_tab`（`:535` 附近声明，M40AZ 段 `:539-604`） | `gfx_vk_exec.c` `rt_*` | 不复用 |
| 着色器加载 | `:6355` `r_bin_spv()`，`:6785` `%s/%s.%d.spv` 路径拼接，`:6825` magic 校验 | `gfx_vk_shader.c` `vk_shader()`（`gfx_vk.h:165`），读 DMSV bundle | 不复用（**这是「颜色不对」的核心，见 §5**） |
| 管线创建 | `:929` / `:996` / `:1045` `vkCreateGraphicsPipelines` | `gfx_vk_exec.c` `pipe_get()` 一族 | 不复用 |
| 描述符集 | `:8910` / `:9288` / `:16828` `vkUpdateDescriptorSets` | `gfx_vk_exec.c` `stage_set()`（`gfx_vk.h:162` 注释） | 不复用 |
| 绘制下发 | `:239-240` / `:1207` `vkCmdDrawIndexed` | `gfx_vk_exec.c` `exec_ops` | 不复用 |
| 出图 | `:29603` `nvn_vk_present_clear()`，`:29612` `if (g_ohos_nb_stripped)` 绕开 | `gfx_vk_present.c` `mtl_present`（`:1010`） | **语义不同，必须重写桥** |
| 上屏兜底（CPU 填窗） | `OUR/cpp/platform/win_fill.c:30-64`（RGB）与 `:94-266`（BGRA）+ 调用点 `gfx_bridge.c:5308-5312` | 无 | **可直接复用**（§4 分支 B 的落点） |
| NVN 符号表（HLE） | `OUR/cpp/platform/gfx_bridge.c:6435` `nvn_lookup()`，98 条 | `NX/dm/gfx/nvn/hle_nvn.c`，393 条 + `nvn_thunks.S` | **不复用**（表本身要换） |

### 2.1 「能直接复用」的白名单（我方资产，别丢）

1. `OUR/cpp/platform/win_fill.c`（288 行）——OHOS `OH_NativeWindow_*` 请求/刷缓冲；**这是模拟器上唯一能把像素送上屏的路**。
2. `OUR/cpp/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c` —— 已在 arm64 构建（`CMakeLists.txt:37`），与 nx2 的 `gfx/tex/nvn_deswizzle.c`（67 行）同源，**替换时只需核对 sha**。
3. `OUR/cpp/platform/nvn_tex.c`（166 行）——`nvn_fmt_bpb` / `nvn_tex_layout` 布局数学，nx2 的 `gfx_vk_tex.c:8` 也 `include "nvn_tex.h"`。
4. `g_ohos_nb_stripped` 的整段知识（`nvn_vk.c:1504-1590`）——反汇编结论（OHOS 隐式层 `libvulkan_swapchain.so` 在 `vkCreateDevice` 强制要求
   `VK_OHOS_native_buffer`，否则 return -3；且层里 `acquire` 会解引用 NULL）。**这段是本轮移植最贵的领域知识，任何方案都不能丢。**
5. `OUR/cpp/platform/vk_boot.c`（1,130 行）/ `vk_boot.h`——我方已有的 Vulkan 起层 + 能力探测的审计面（日志格式、开关回读）。

### 2.2 「语义不同」必须改写的点

| 我方语义 | nx2 语义 | 处理 |
|---|---|---|
| 宿主逐调用 `nvn_vk_*(...)`（151 个入口，宿主 `gfx_bridge.c` 直接喂对象/字节） | 宿主把 NVN 调用**录成 op 记录**，`mtl_exec(p)` 整批回放 | **这是最大的架构差**：`gfx_bridge.c` 的直喂模式必须让位给录制模式（或做一个「直喂 → 记录」适配层，约 400 行，见 §7 切片 2） |
| 上屏 = `nvn_vk_present_clear()` 清色 + 失败回落 `win_fill` | 上屏 = 从游戏窗口纹理 blit 到 swapchain，带 60 拍节流/预旋转/截图 | 重写桥（§4） |
| 无命令缓冲所有权概念（同步提交） | 每提交一条主 CB + 一条上传 CB，栅栏批次（`gfx_vk.h:76-89`） | 直接采用 |

---

## 3. Q3 血统判定：同源，但不同分支、不同层

### 3.1 判定：**同一支 d2r-mobile 血统，四条分支**

同一支系的硬证据 —— 四份 `hle_nvn.c` 与三份 `nvn_obj.h` 的谱系：

| 树 | `hle_nvn.c` 行数 | `NVN()` 跳板数 | `nvn_obj.h` | `nvn_mtl.h` |
|---|---|---|---|---|
| `third/d2r-mobile/dm/gfx/nvn/` | 1,605 | 204 | — | — |
| `third/dontstarve-harmony/dm/gfx/nvn/` | 1,915 | 265 | — | — |
| `OUR/cpp/third_party/d2r_gfx/gfx/nvn/` | 3,058 | 270 | 315 行 | 76 行 |
| `NX/dm/gfx/nvn/`（nx2android） | 2,633 | **393** | **219 行** | 47 行 |

`OUR/cpp/third_party/d2r_gfx/gfx/nvn/nvn_mtl.h` 与 `NX/dm/gfx/nvn/nvn_mtl.h` 的 `diff`：
**nx2 的 47 行逐行被包含**，我方仅多出中文/英文双行注释 + 29 行 TOTK/BOTW 期新增声明（§1.1）。
文件头注释（"Metal execution layer of the NVN null device (C interface)… Ordering convention … GO_MAP_SHARE corrupted-picture mistake of the x86 route"）
**两棵树逐字相同**——这是同一支系的指纹。

### 3.2 我方 `nvn_vk.c` 的血统（更早的分叉）

`OUR/cpp/platform/nvn_vk/nvn_vk.h:1-5` 自述：*"移植自 `infra/dontstarve-harmony/dm/gfx/nvn/nvn_vulkan.c`（7402 行产品级 NVN→Vulkan 重放层，
宿主 d2r-mobile dm 工程，与 MacNativeGames odyssey runtime 同血统不同分支）"*。

实证：
- `third/dontstarve-harmony/dm/gfx/nvn/nvn_vulkan.c` 7,402 行，**实现了 `mtl_*`**：`:1928 mtl_sub_begin`、`:1977 mtl_sub_commit`、
  `:2453 mtl_present`、`:2430 mtl_wait`、`:1299 mtl_reg_tex` 等；`grep -c '^int nvn_vk_\|^void nvn_vk_'` = **0**。
- 我方 `nvn_vk.c`：`grep -c '\bmtl_'` = **2**（且非定义），`grep -c '^int nvn_vk_\|^void nvn_vk_'` = **151**。

**即：从 dontstarve 到我们这 40 轮，`mtl_*` 接口被整体拆掉，换成了 `nvn_vk_*` 直喂 API。**
这是本轮的根因层：一旦离开 `mtl_*`，就离开了 d2r 那套「录制 + 整体回放 + 逐 op 语义」的正确性基线，
于是颜色/上屏只能靠探针猜（M40AU…M40AZ 全是这个补偿）。

### 3.3 层间差异（谁和谁不能混）

| 层 | 同源？ | 能否混用 | 证据 |
|---|---|---|---|
| HLE/录制（`hle_nvn.c`） | 同源不同分支 | **不能混**：nx2 393 跳板 vs 我方 98 条手写表 | `NX/.../hle_nvn.c` vs `gfx_bridge.c:6435` |
| **命令记录 ABI（`nvn_obj.h` opcode）** | 同源但**已分叉** | **绝对不能混**：`OP_CLEAR_TEX` 之后枚举即分叉 | 见下 |
| 翻译/回放（`gfx_vk_*`） | 同源不同实现 | 只能整体取用 | §1 |
| 宿主（`dm/host`） | 同源，我方**自建替代** | 不混，写胶水 | `dm.h` 146 vs 20 行 |

**opcode 分叉实证（`nvn_obj.h` 的 `enum op`）**：

```
... OP_CLEAR_TEX,
我方 (OUR/cpp/third_party/d2r_gfx/.../nvn_obj.h):  OP_DEPTH_CLAMP, OP_RENDER_ENABLE, OP_RASTER_DISCARD, OP_RENDER_COND, OP_COUNT_
nx2  (NX/dm/gfx/nvn/nvn_obj.h:101-107):           OP_CLEAR_BUF, OP_DEPTH_CLAMP, OP_RENDER_COND, OP_FENCE_SYNC,
                                                  OP_TF_BIND, OP_TF_STATE, OP_RAST_DISCARD, OP_VP_SWIZZLE,
                                                  OP_BIND_TEX1, OP_PRIM_RESTART, OP_DRAW_TF, OP_COUNT_
```

精确编号（脚本对两份头的 `enum` 逐项计数）：`OP_CLEAR_TEX` 我方 **38** / nx2 **39**；`OP_DEPTH_CLAMP` 我方 **39** / nx2 **41**；
`OP_COUNT_` 我方 **43** / nx2 **51**。连名字都分叉了：我方 `OP_RASTER_DISCARD`，nx2 写成 `OP_RAST_DISCARD`。
**⇒ 混用两份头：名字不同的会在编译期炸（好事），名字相同但编号不同的（`OP_DEPTH_CLAMP` 及之后全部）若两边都有同名成员就会
静默错画**——所以 §8 R4 的「新目录 + 编译期锚定」是硬要求。

> **铁律：`hle_nvn.c` + `nvn_obj.h` + `nvn_rec.h` + `gfx_vk_*` 必须作为**一套**整体搬入，绝不能只搬 `gfx_vk_*` 而沿用我方 `third_party/d2r_gfx` 的头。**

### 3.4 结论（Q3）

**同源**（d2r-mobile），但差异在**两层**：
1. **HLE/录制层**：nx2 是标准 d2r HLE（393 跳板 + 录制）；我方是自建 `gfx_bridge.c`（98 条 + 直喂）。
2. **翻译/回放层**：nx2 完整 `gfx_vk_*`；我方是 dontstarve 分支演化出的 30K 行 `nvn_vk.c`（已丢掉 `mtl_*` 面）。

「颜色塌成纯值、上屏靠猜槽」正是**丢失 `mtl_*` 面之后**在翻译层和 present 层分别产生的两个症状，不是一个 bug。

---

## 4. Q4 present 层：Android 专有行 + OHOS 映射方案

### 4.1 `gfx_vk_present.c`（1,103 行）里的 Android 专有行（逐行）

| 行号 | 内容 | 是否 Android 专有 | OHOS 处理 |
|---|---|---|---|
| `:2` | 注释「the app's ANativeWindow, dm_front_window」 | 注释 | 改写 |
| `:7` | `#include <jni.h>` | **是** | 删（或 OHOS NAPI 适配） |
| `:18` | `__attribute__((weak)) ANativeWindow *dm_front_window(void) { return NULL; }` | **是** | 换 `#include <native_window/external_window.h>`，改 `OHNativeWindow *dm_front_window(void)` |
| `:18`(下) | `weak int dm_external_active(void)` | 否（弱桩） | 保留 |
| `:60` | `static ANativeWindow *g_win;` | **是** | `OHNativeWindow *g_win;` |
| `:162-176` | `swap_make()`：`ANativeWindow_getWidth/Height`（`:166`）、`VkAndroidSurfaceCreateInfoKHR` + `vkCreateAndroidSurfaceKHR`、`g_sext` 取 `ANativeWindow_getWidth/Height`（`:176`） | **是** | 分支 A：`VkSurfaceCreateInfoOHOS` + `vkCreateSurfaceOHOS`；分支 B：整段不编译（见 4.2） |
| `:424-460` | 预旋转（pre-rotation）：`rot_vert/rot_frag` + `VkSurfaceTransformFlagBitsKHR` | 否（Vulkan 通用） | 保留；OHOS 的 `currentTransform` 语义需实测 |
| `:495-496` | `JNIEXPORT jint JNICALL Java_org_nx2a_ExternalActivity_externalMenuState/externalMenuPixels` | **是**（JNI 导出） | 删，改 OHOS NAPI 或直接不导出（菜单 UI 见 `overlay_layout.h` 一族） |
| `:932-961` | `shot_wanted()` / `shot()`：`DM_VK_SHOT=n1,n2` 把指定帧写 PPM | 否（**纯文件 IO，模拟器上极有用**） | **保留（重要）**：模拟器无上屏时的唯一验证出口 |
| `:993` | `present_enqueue()`：出图线程队列 | 否 | 保留 |
| `:1009` | `void dm_perf_window(ANativeWindow *w, double fps)` | **是** | 改签名或空桩 |
| `:1019` | `static ANativeWindow *rate_win;`（帧率档位投票，避免频繁改显示模式） | **是** | 可整段删（OHOS 无 ADPF 等价物，见 `:1017 dm_perf_frame`） |
| `:1099` | `mtl_has_window()` → `dm_front_window() != NULL` | 间接 | 改指针类型 |
| `:1087` | `mtl_present_wait(tk)` 等 blit 完成 | 否 | 保留 |

另有两处不在 `present.c` 但同属 Android 专有：

| 位置 | 内容 | OHOS 处理 |
|---|---|---|
| `gfx_vk.h:7` | `#define VK_USE_PLATFORM_ANDROID_KHR` | → `#define VK_USE_PLATFORM_OHOS 1`（我方 `nvn_vk.c` 的写法） |
| `gfx_vk.h:8-9` | `#include <android/hardware_buffer.h>` / `<android/native_window.h>` | → `<native_window/external_window.h>`；`AHardwareBuffer` 段见 4.3 |
| `gfx_vk.h:27` | `X(vkCreateAndroidSurfaceKHR)` | → `X(vkCreateSurfaceOHOS)`（分支 A） |
| `gfx_vk_core.c:190` | 设备扩展白名单含 `VK_ANDROID_external_memory_android_hardware_buffer` + `VK_EXT_queue_family_foreign` | → 删/降级为可选（分支 B 必须删，否则 `vkCreateDevice` 被 OHOS 层拒绝） |
| `gfx_vk_core.c:398-555` | `zcfree_t` / `slab_t` / `pool_zero_copy()`：每块内存池页用 `AHardwareBuffer_allocate` + `vkCreateBuffer` + `VkImportAndroidHardwareBufferInfoANDROID` 导入 | **整段不编译**（分支 B）；见 4.3 |

### 4.2 硬约束与两分支方案

**硬约束（已实测，我方 `nvn_vk.c:1504-1590` 的证据）**：
OHOS 隐式设备层 `libvulkan_swapchain.so`（`VK_LAYER_OHOS_surface`）在 `vkCreateDevice` 里枚举驱动设备扩展，
**找不到 `VK_OHOS_native_buffer` 就 `return -3`**；即使绕过建链，`vkAcquireNextImageKHR` 也会进层里解引用一个 NULL 函数指针
（cppcrash `present_clear+244 → 层+0xf9b0 → pc=0`）。所以我方现在整段跳过 swapchain（`nvn_vk.c:29612-29618`）。

#### 分支 A —— 驱动/真机**有** `VK_OHOS_native_buffer`（能力位可变 ⇒ 必须运行时探测）

改动点（少，只碰 present 层）：

1. `gfx_vk.h:7-9` 换 OHOS 平台宏与窗口头。
2. `gfx_vk.h:27` 的 `vkCreateAndroidSurfaceKHR` → `vkCreateSurfaceOHOS`（`VkSurfaceCreateInfoOHOS{ .window = OHNativeWindow* }`）。
3. `gfx_vk_present.c:162-176` `swap_make()` 内 surface 创建换掉；`ANativeWindow_getWidth/Height` → `OH_NativeWindow_NativeWindowHandleOpt(w, GET_BUFFER_GEOMETRY, &wd, &ht)`。
4. `gfx_vk_core.c:190` 扩展白名单保留 `VK_OHOS_native_buffer`（分支 A 正是要靠它），删掉 Android 两项。
5. `gfx_vk_present.c:495-496` JNI 导出删掉；`:1009` `dm_perf_window` 改签名。
6. **风险**：`vkAcquireNextImageKHR` 是否仍崩 —— 由能力探针门控（`g_ohos_nb_stripped` 的同款判定），不稳就退分支 B。

验收：`present shown` 计数增长 + `DM_VK_SHOT` 输出 PPM 与屏幕一致。

#### 分支 B —— **无** `VK_OHOS_native_buffer`（模拟器 = 当前实测状态；真机若无此扩展同此）**这是本轮主线**

思路：`gfx_vk_*` **完全不建 swapchain、不建 surface**（`gfx_vk_present.c` 的 `swap_make/swap_destroy/swap_blit` 整段 `#ifndef DM_OHOS_NB` 排除），
只保留 `present_enqueue()` 的**离屏渲染 + 帧计数 + 截图**；像素经 `OUR/cpp/platform/win_fill.c` 拷到 OHOS native window。

具体落点：

```
新增  OUR/cpp/platform/ohos_present_bridge.c   (≈260 行)
  ← gfx_vk_present.c 提供的两个钩子（在 nx2 那边是 mtl_present 末尾的 present_enqueue）
  → win_fill.c:94-266 的「BGRA 请求缓冲 → memcpy → FlushBuffer」路径（已存在，直接调）

数据流：
  gfx_vk_exec 渲染进 vkimg_t（离屏 RT，GENERAL 布局）
   → vkCmdCopyImageToBuffer 到 host-visible VkBuffer（1600x900x4 = 5.76 MB）
   → vkWaitForFences（present 线程已有栅栏，`present_enqueue` 结构里就有）
   → 帧缓冲映射到 OHNativeWindow buffer（win_fill 已做 RequestBuffer/Map/Flush）
   → 颜色顺序：Vulkan 侧选 VK_FORMAT_B8G8R8A8_UNORM 直接 memcpy；若驱动不支持则 CPU swizzle（win_fill 的 RGB 变体已含）
```

**为什么这条路可行（关键证据）**：`gfx_vk_core.c:626` 的内存池创建处本来就有非零拷贝回退：

```c
int zc = ((uintptr_t)mem & 0xfff) || (size & 0xfff) ? -3
        : getenv("DM_VK_NO_ZEROCOPY") ? -4 : zc_over(end - base) ? -5
        : pool_zero_copy(p, (uint8_t *)mem, size);
```

`DM_VK_NO_ZEROCOPY=1` 时每块池子退化为「host-visible 影子拷贝 + 每提交前同步」（`gfx_vk.h:71-72` 的 `vk_pools_sync`，
`gfx_vk_core.c:564` 的 `p->shadow` 分支）。**即「无 AHardwareBuffer」是 nx2 后端的一等公民模式，不是我方要新写的。**
⇒ 分支 B 只需 `DM_VK_NO_ZEROCOPY=1` + 不建 surface/swapchain + 末尾换我们的 copy-to-window 桥。

**分支 B 的验收点（可测，不需真机）**：
1. `vk boot` 日志出现 `zero_copy=0`、pool 数量正确、无 `VK_ANDROID_*` 扩展请求；
2. `DM_VK_SHOT=600` 产出 PPM，且**颜色正确**（这是「颜色塌成纯值」的直接反证）；
3. `win_fill` 调用计数 > 0 且 `FlushBuffer` 返回 0；
4. 帧计数 60 秒内 ≥ 3,000（不掉帧到个位数）。

### 4.3 内存模型（必须一起决策）

| 项 | nx2 (Android) | OHOS 分支 A | OHOS 分支 B |
|---|---|---|---|
| 内存池零拷贝 | `AHardwareBuffer` + `vkImportAndroidHardwareBuffer`（`gfx_vk_core.c:505-555`） | `OH_NativeBuffer` + `VK_OHOS_native_buffer` 导入（写法类似，API 名不同） | **不做**：影子拷贝 + `vk_pools_sync` |
| 纹理解排布 | 池内直接 deswizzle 到导入缓冲 | 同 | 同（走 host-visible staging，`stage_grow` 我方已有） |
| 成本 | 0 拷贝 | 0 拷贝 | 每提交前同步（**需实测**：SMO 一个世界帧 ≈1,230 draws，池同步 = 全量 dirty 页 memcpy，见 §8 风险 3） |

**建议：先走分支 B 打通正确性，再在真机上探分支 A 拿性能。两条路的 `gfx_vk_exec/tex/shader` 完全相同，只有 `core` 的内存池与 `present` 不同 —— 切换成本低。**

---

## 5. Q5 着色器与 PSO 复用：能不能一次性解决「颜色不对」

### 5.1 六张表各是什么

| 文件 | 行数 | 内容 | 直接可用？ |
|---|---|---|---|
| `shader_map.tsv` | **27,987**（含 2 行注释 + 1 行表头 ⇒ 27,984 条着色器） | `package / index / stage / dm_ctl_sha16 / source`。key = `sha256(控制段前 0x880 字节 + 代码区)[:16]`（`NX/dm/gfx/tools/shader_split.py:38,82`） | **可直接用** |
| `shader_pairs.tsv` | 20,741（含 1 注释 ⇒ 20,740 对） | 同一 BNSH 变体内的 vertex↔pixel 配对 | **可直接用**（providence 用） |
| `shader_facts.tsv` | 42（含 1 注释 ⇒ 41 条） | 计算着色器的 `{"local":[x,y,z],"shared":n}` 答案表 | **可直接用** |
| `pso_manifest.tsv` | **2（空，只有注释）** | 预热门清单（`S<tab>key<tab>stage` / `R<tab>pso_skey` / `C<tab>key`）。文件自述："Provisional profile: empty until recorded on the Mac (`DM_PSO_MANIFEST=off DM_PSO_RECORD`)" | 可用但**空**（无收益，无害） |
| `bf_want.tsv` | **1（`# generated`，空）** | 盲测清单 | 空 |
| `android.tsv` | 34 | **Android 宿主配置**（测试机 Y700/Adreno 830 的实测记录 + 键值），注释里写明 "the upstream profile as it is" | **需转换**（键名是 Android/`dm_host` 口径，OHOS 要对一遍） |
| `game.tsv` | 37 | 游戏身份/构建/SDK（`title_id 0100000000010000`、SDK 7.3.2、update 1.4.1） | **可直接用**（且已被 `game.py header` 消费） |

### 5.2 结论：表能直接用，**但真正的着色器不在表里**

**这是本轮最重要的一条**：
`shader_map.tsv` 只是「key → stage + 出处」的**索引**，**不含 SPIR-V**。
真正被 `gfx_vk_shader.c` 读取的是 **DMSV shader bundle**（`gfx_vk_shader.c:21 vk_shaders_open()`，
magic `"DMSV"`，version 1/2，每项 = 一个 Switch 着色器的 SPIR-V + 二进制反射；`gfx_vk_shader.c:2-3`）。

**这个 bundle 不在仓库里**，必须现场构建：

```
NX/dm/android/tools/shader_vk.py  (435 行)
  ← 输入：shader_split.py 从游戏 RomFS/ExeFS 解出的 package 目录（控制段 + 代码区）
  ← 翻译器：NX/dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate.dll （Ryujinx 系 .NET，4.8 MB，自包含）
  ← 表：shader_map.tsv / shader_facts.tsv / shader_pairs.tsv / bf_want.tsv
  ← 参数：--jobs N、--zstd 等级、--capture <dir>（运行期捕获的缺失着色器）
  → 输出：<out> DMSV bundle
```

**「颜色不对」的因果链**：我方当前 `nvn_vk.c:6785` 从 `shader_cache/<key>.<stage>.spv` 逐个读松散文件，
`:6355 r_bin_spv()` 从 `spv.bin` 取字节，`gfx_bridge.c` 侧的 key 生成与 nx2 的 `dm_ctl_sha16` 口径**未经交叉验证**；
一旦 key 对不上，就落到「按 stage 猜一个能用的 SPIR-V」——**这正是「颜色塌成纯值」**。
（我方 `nvn_vk.c:6806` / `:6821` 的 `M4bz`/`M7O` 两个「FS UV-patch 补丁」探针可直接视为该病的化石证据：
明明是着色器不对，却在后端打 SPIR-V 补丁。）

### 5.3 「颜色不对能不能一次性解决」——判定：**能，条件是 bundle 从零重建**

必须同时满足三条：

1. **key 口径统一**：全部走 `shader_split.py` 的 `dm_ctl_sha16`（`sha256(ctl[:ctl_len] + code)[:16]`）。
   我方现有 `spv.bin` / `shader_cache/*.spv` 的键是 16 hex（`nvn_vk.c:6785` 的 `%s.%d.spv`），**大概率同口径但不保证**。
   → 切片 0（§7）用 `shader_map.tsv` 做**交叉校验**：抽 200 个我方日志里出现过的 key，查表命中率；
   命中 ≥ 99% ⇒ 口径一致，可复用我方已有的控制段/代码区 dump；否则必须从 RomFS 重解。
2. **bundle 重建**：用 `shader_vk.py` + `dmsh_xlate.dll` 跑全部 27,984 条（Adreno 上是 27,984 全翻译，`android.tsv:2` 自述
   "27,984 shaders, all translated"）。本机是 macOS arm64，**`.NET` 自包含 dll 可跑**（`shader-conv/INVENTORY.md:13` 已烟测
   同族 `dmsh_xlate` 二进制可执行）。
3. **反射也要对上**：bundle 里每项的 reflection（`dmsv_bind`，`gfx_vk.h:146` `_Static_assert(sizeof(dmsv_bind)==20)`）
   是描述符绑定的唯一来源 —— 这是「按槽猜」变成「按反射绑定」的关键，也是颜色正确的必要条件。

> **副产品**：`pso_manifest.tsv` 为空 ⇒ **首帧卡顿会存在**（`android.tsv:7` 实测首段镜头 40 fps，p90 work 15.4 ms，
> 原因就是 pipeline 首次创建）。真机验收时若卡在开场，允许打开 `DM_PSO_RECORD` 在我们机器上录一份再回填，不是阻塞项。

---

## 6. Q6 依赖闭包：编 `gfx_vk_*.c` 需要的头/源

### 6.1 权威来源：`NX/dm/android/build.py`

`build.py:54-72` 是唯一权威的编译单元清单：

```python
HOST = srcs('host/*.c')            # 整个 dm 宿主（我方用 platform/*.c 替代）
GFX  = srcs('gfx/*.c', 'gfx/nvn/*.c', 'gfx/tex/*.c')
PORT = srcs('android/compat/*.c', 'android/*.c')  # 去掉 gfx_null.c 与非所选后端的 gfx_vk*
ASM  = srcs('host/entry.S', 'gfx/nvn/nvn_entry.S', 'gfx/nvn/nvn_thunks.S')
ZSTD = srcs('third_party/zstd/zstddeclib.c')      # bundle 里的 SPIR-V 是 zstd 帧
INC  = ['-include android/compat/include/darwin_compat.h', '-Iandroid/compat/include',
        '-Ihost', '-Igfx/nvn', '-Igfx/tex', '-Igfx/nvn/include',
        '-Ithird_party/vulkan-headers/include', '-Igen/<game>', '-Ithird_party/libvpx',
        '-Ithird_party/stb_vorbis', '-Ithird_party/zstd', '-Iandroid']
CFLAGS = [... '-DDM_ANDROID=1', '-ffixed-x18', '-Werror=implicit-function-declaration' ...]
LIBS   = ['-landroid', '-llog', '-lvulkan', '-laaudio', '-lnativewindow', '-lmediandk', '-lz', '-lm', '-ldl']
```

注意 `-ffixed-x18`、`linux/` 系头、`darwin_compat.h` 前置注入 —— **nx2 是「用 Darwin 兼容层编 Mach-O 风格源码到 ELF」的工程**，这套 shim 我方已有对应物（`OUR/cpp/platform/ohos_compat/`）。

### 6.2 最小闭包清单（只算 `gfx_vk_*` 真正需要的）

| 类别 | 文件 | 我方状态 |
|---|---|---|
| **后端本体** | `NX/dm/android/gfx_vk_core.c` 1,414 / `gfx_vk_exec.c` 2,495 / `gfx_vk_present.c` 1,103 / `gfx_vk_tex.c` 909 / `gfx_vk_shader.c` 592 / `gfx_vk_dq.c` 323 / `gfx_vk_subgroup.c` 143 | ❌ **全缺**（要搬入） |
| **后端头** | `gfx_vk.h` 183 / `gfx_vk_slot.h` 7 / `gfx_vk_pipeline_cache.inc` | ❌ 全缺 |
| **ABI 头（**必须整取**）** | `NX/dm/gfx/nvn/nvn_obj.h` 219 / `nvn_rec.h` 303 / `nvn_tex.h` 26 / `nvn_hmap.h` 70 / `nvn_dirty.h` 21 / `nvn_pso_skey.h` 48 / `nvn_mtl.h` 47 / `nvn_builtin.h` / `nvn_cover.h` / `nvn_geometry.h` + `.inc` / `nvn_fstep.h` 122 / `nvn_pace.h` 16 / `dmsh_chain.h` / `nvn/refl.h` | ⚠️ **半缺**：我方 `third_party/d2r_gfx/gfx/nvn/` 有这些名字但**版本不同**（`nvn_obj.h` 315 vs 219、无 `nvn_rec.h`）⇒ **必须整体替换，不能沿用** |
| **NVN 入口/录制** | `NX/dm/gfx/nvn/hle_nvn.c` 2,633 + `nvn_entry.S` + `nvn_thunks.S` + `names.txt` | ❌ 缺（我方是 `gfx_bridge.c` 自建） |
| **脏页/哈希** | `NX/dm/gfx/nvn/nvn_dirty.c` 290 + `nvn_hmap.h`（内联） | ❌ 缺（我方 `nvn_vk.c` 用 mprotect 自建一套，不兼容） |
| **纹理工具** | `NX/dm/gfx/nvn/nvn_tex.c` 172 + `NX/dm/gfx/tex/nvn_deswizzle.c` 67 + `nvn_bc.c` 60 + `nvn_fmt_mtl.c` 83 | ⚠️ 我方有 `nvn_deswizzle.c`（已编，`CMakeLists.txt:37`）、`nvn_tex.c`（`OUR/cpp/platform/nvn_tex.c` 166 行）、`nvn_bcdec.c`/`nvn_bc1astc.c`；**`nvn_bc.c` 需核对是否等价**（nx2 用 `third_party/bcdec`，我方有 `nvn_bcdec.c`） |
| **zstd** | `NX/dm/third_party/zstd/zstddeclib.c`（单文件 decompressor） | ❌ **缺**（我方 `third_party/` 下无 zstd；必须搬 1 个文件） |
| **构建期生成：game_profile.h** | `NX/dm/games/game.py header <id> <out.h>`（消费 `game.tsv`） | ❌ 缺工具（Python，可直接跑） |
| **构建期生成：内置 SPIR-V 头** | `NX/dm/android/shaders/` 8 个源（`overlay.vert/frag`、`rot.vert/frag`、`maskclear.vert/frag`、`flatdbg.frag`、`depthscale.comp`）→ `build.py:40-50` 用 NDK `glslc` 生成 `<name>_<stage>.spv.h` | ⚠️ **本机无 `glslc`**（只有 `glslangValidator`）。替代：`glslangValidator -V -o x.spv` 后转 `uint32_t[]` 头（脚本 ≈20 行）。**这是必须自建的一步** |
| **宿主钩子** | `dm.h` 28–34 个 `dm_*`（§1.3） | ❌ 我方 `dm.h` 仅 20 行/命中 5 个 ⇒ **胶水层主体** |
| **UI 覆盖层** | `overlay_layout.h` 215 / `pixel_font.h` 60 + `pixel_font_latin1.h` 170 / `overlay_theme.c` 119 | ⚠️ 若第一切片不做覆盖层，可整块不编（`gfx_vk_present.c` 里对应段用 `#ifdef DM_OHOS_OVERLAY` 包） |
| **Vulkan 头** | `third_party/vulkan-headers/include` | ✅ 我方用 OHOS NDK 自带 `vulkan/vulkan.h`（`CMakeLists.txt:20`） |
| **Vulkan 库** | `-lvulkan` | ✅ 已有（`CMakeLists.txt:19`） |
| **HiLog / native_window** | `-llog` → `-lhilog_ndk.z`；`-lnativewindow` → `-lnative_window` | ✅ 已有（`CMakeLists.txt:21,103`） |

### 6.3 缺失清单（要搬入的文件，按块计）

```
A. 后端 7 个 .c + 3 个头                      ≈ 7.1 k 行   （重心，纯搬入 + OHOS 改 present/core）
B. NVN 录制层 hle_nvn.c + 2 个 .S + names.txt  ≈ 2.7 k 行   （纯搬入）
C. ABI 头 6 个（nvn_obj/nvn_rec/nvn_tex/nvn_hmap/nvn_dirty/nvn_pso_skey）
                                              ≈ 0.7 k 行   （整体替换我方 d2r_gfx 内同名头）
D. 纹理工具 nvn_dirty.c/nvn_bc.c/nvn_tex.c     ≈ 0.5 k 行
E. zstddeclib.c                               1 文件
F. 内置 shader 生成脚本 + OHOS present 桥      ≈ 0.3 k 行   （我方新写）
G. dm 宿主钩子胶水                            ≈ 1.2–1.6 k 行（我方新写，最贵）
```

---

## 7. Q7 第一个可编译切片与验收点

### 7.1 切片划分（每片独立可验收，任一片挂了不影响前一片的证据）

#### 切片 0 —— 数据口径校验（**不写代码**，0.5 人日）

| 动作 | 命令/落点 | 通过标准 |
|---|---|---|
| 生成 `game_profile.h` | `python3 NX/dm/games/game.py header 0100000000010000 <out>/game_profile.h` | 文件生成、含 `DM_*` 宏、无异常 |
| 抽 key 交叉校验 | 从 `logs/` 里我方现有 `spv missing key=<hex>` / `[r] ... key=` 行抽 200 个 key，grep `shader_map.tsv` 第 4 列 | **命中率 ≥ 99%** ⇒ key 口径与我方 `spv.bin` 一致 |
| 翻译器烟测 | `dotnet NX/dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate.dll`（无参应报用法） | 能跑起来（`shader-conv/INVENTORY.md:13` 已烟测同族） |
| 内置 shader 生成链 | `glslangValidator -V` 编 `NX/dm/android/shaders/*.{vert,frag,comp}` → `uint32_t[]` 头 | 8 个 `.spv.h` 生成，`spirv-val` 通过 |

**产物**：`/tmp/m41a_slice0.md`（命中率 + 生成物清单）。这一步决定要不要从 RomFS 重解控制段，**成本极低但影响后续全部**。

#### 切片 1 —— 「空后端 + 固定 PSO 画满屏三角形」（最小可编译里程碑，2 人日）

目标：**在完全不碰 NVN 录制层、不碰 present swapchain 的前提下**，证明「nx2 的 core+tex+shader 能在 OHOS 编出来、能建管线、能画、能回读」。

要搬/要改（精确到文件）：

```
搬入（不改逻辑）：
  NX/dm/android/gfx_vk_core.c      → OUR/cpp/platform/nvn_vk2/gfx_vk_core.c
  NX/dm/android/gfx_vk_tex.c       → .../gfx_vk_tex.c
  NX/dm/android/gfx_vk_subgroup.c  → .../gfx_vk_subgroup.c
  NX/dm/android/gfx_vk_dq.c        → .../gfx_vk_dq.c
  NX/dm/android/gfx_vk_shader.c    → .../gfx_vk_shader.c
  NX/dm/android/gfx_vk.h           → .../gfx_vk.h        （按 §4.2 分支 B 改 7 行）
  NX/dm/android/gfx_vk_slot.h      → .../gfx_vk_slot.h
  NX/dm/android/gfx_vk_pipeline_cache.inc → .../（原样）
  NX/dm/gfx/nvn/nvn_obj.h / nvn_rec.h / nvn_tex.h / nvn_hmap.h / nvn_dirty.h / nvn_pso_skey.h / nvn_mtl.h
                                   → .../abi/（独立 include 目录，**不覆盖 third_party/d2r_gfx**）
  NX/dm/gfx/nvn/nvn_dirty.c        → .../nvn_dirty.c
  NX/dm/gfx/tex/nvn_bc.c           → .../nvn_bc.c
  NX/dm/third_party/zstd/zstddeclib.c → OUR/cpp/third_party/zstd/（单文件）

新写：
  OUR/cpp/platform/nvn_vk2/ohos_dm_stub.c   ≈360 行  28 个 dm_* 空桩 + LOG/die 接 hilog
  OUR/cpp/platform/nvn_vk2/ohos_present_bridge.c ≈260 行  （切片 3 才启用；切片 1 只放 offscreen+shot）
  OUR/cpp/platform/nvn_vk2/selftest_tri.c   ≈220 行  「固定 PSO 画满屏三角形」自测路径
                                                    （复用我方已有 OUR/cpp/platform/nvn_vk/tri_spv.h 的 SPIR-V blob，
                                                     不必先建 DMSV bundle）

改 CMake：
  OUR/cpp/CMakeLists.txt   arm64 分支加 nvn_vk2/*.c + zstddeclib.c；
                           include 顺序：nvn_vk2/abi 在 third_party/d2r_gfx/gfx/nvn 之前
                           （否则会误取我方旧 nvn_obj.h，见 §3.3 铁律）
                           x86_64 壳层完全不动（沿用 nvn_vk_stub.cpp 弱符号）
```

**切片 1 验收点（四条全部要有证据）**：

1. `libgame.so` 链接通过，`nm -D` 里能看到 `mtl_init/mtl_pool_add/mtl_sub_begin/mtl_sub_commit/vk_present` 等导出；
2. 冷启 60 秒不崩：`grep -c "\[vk\]" hilog` > 0 且无 `SIGSEGV`；日志出现
   `vk boot ... zero_copy=0`（证明走了 `DM_VK_NO_ZEROCOPY=1` 分支，§4.2）；
3. **满屏三角形 PPM 正确**：`DM_VK_SHOT=1` 产出的 PPM 采样 16×16 网格，三角形覆盖处颜色 = 期望色（RGB 误差 ≤ 2），
   覆盖外 = 清屏色；用现成 `OUR/cpp/tools` 的读图脚本比对（不需要真机）；
4. `mtl_wait` 的栅栏在 16.7 ms 内完成（记录 `g_ns_submit`），说明提交/等待链没死锁。

> 这一片**同时验证了我方最怕的三件事**：OHOS 上能不能建 device（已验过）、无零拷贝路径能不能跑通（未验）、
> nx2 的 ABI 头能不能和我方编译环境共存（未验）。**它不需要任何游戏资源、不需要 bundle、不需要 hle_nvn，因此是最便宜的排雷。**

#### 切片 2 —— 「录制层通电」（3.5 人日）

```
搬入：
  NX/dm/gfx/nvn/hle_nvn.c        → OUR/cpp/platform/nvn_vk2/hle_nvn.c
  NX/dm/gfx/nvn/nvn_entry.S / nvn_thunks.S / names.txt → .../
  NX/dm/gfx/nvn/nvn_tex.c        → .../（替换我方 OUR/cpp/platform/nvn_tex.c 的同名符号，需先摘我方旧实现）
  NX/dm/android/gfx_vk_exec.c    → .../gfx_vk_exec.c

改：
  OUR/cpp/platform/gfx_bridge.c:6435 nvn_lookup()
    → 由「98 条手写实现」改为「转发到 hle_nvn.c 的导出表」（393 条），
       保留 98 条里 SMO 专用的补丁（如 M14B blend_en 字段位）作为 forward 前后的钩子
  OUR/cpp/platform/vk_boot.c        → mtl_init()/mtl_set_origin_lower_left(1) 接入
  OUR/cpp/platform/nvn_vk/nvn_vk.c  → **本切片起不再编入 arm64**（在 CMake 里注释掉，保留文件做回退）
```

**验收点**：
1. 一次 `nvnQueueSubmitCommands` 后日志出现 `replay up to present N: draws X, passes Y, skipped(no program/no target/no pipeline) ...`（格式见 `gfx_vk_present.c:1030-1031`）；
2. **`draws > 0` 且 `skipped` 三项都为 0 或极小**（`skip_prog` 大 = 程序没录对，`skip_pipe` 大 = 管线键没配上）；
3. 帧率 ≥ 30 fps（模拟器可接受的下限，用于判断有没有架构级错误）；
4. `DM_VK_SHOT` 出图**不是纯色**（哪怕颜色还是错的，也说明几何+绑定已经工作）。

#### 切片 3 —— 「bundle 通电 + 颜色正确」（3 人日）

```
新增：
  tools/m41/build_dmsv.sh   ：shader_split.py → shader_vk.py → <bundle>
  （先 --keys 抽 500 个 key 出小 bundle 验证，再全量 27,984）
接入：
  gfx_vk_shader.c:40-47 bundle_open_once() 的路径 → 由 DM_ROOT 指向我们的 bundle
  OUR/cpp/platform/nvn_vk2/ohos_present_bridge.c → 启用，接 win_fill.c:94-266
```

**验收点**（这是本轮真正的验收）：
1. bundle 全部 key 可解析：`vk_shader_keys(stage, ...)` 返回数 = `shader_map.tsv` 对应 stage 行数；
2. **零 `[RED] shader ... is not in the bundle`**（`gfx_vk_shader.c:120-131` 的报红就是缺 key）；
3. `DM_VK_SHOT=600` 的 PPM：**与 nx2android 在 Android 上的同一场景截图对比，主观一致**（Mario 红色帽、肤色、天空渐变都在）；
4. 颜色止损指标：同一帧内不同物体的采样点颜色方差 > 阈值（反证不再是「塌成纯值」）。

### 7.2 里程碑与工作量（人日）

| 里程碑 | 内容 | 人日 | 累积 |
|---|---|---|---|
| S0 | 数据口径校验（key 命中率 / 翻译器烟测 / shader 生成链） | 0.5 | 0.5 |
| S1 | 空后端 + 固定 PSO 三角形 + offscreen PPM（**第一个可编译切片**） | 2.0 | 2.5 |
| S2 | 录制层通电（hle_nvn + thunks + gfx_bridge 转发 + exec） | 3.5 | 6.0 |
| S3 | DMSV bundle 构建 + 全量接入 + 颜色验收 | 3.0 | 9.0 |
| S4 | present 分支 B（win_fill 桥）真机/模拟器上屏 | 1.5 | 10.5 |
| S5 | 裁掉覆盖层/UI + 真机分支 A 探测 + 性能调优 | 3.0 | 13.5 |

**合计 ≈ 13.5 人日（区间 11–16）**，其中**新写代码 ≈ 1.6 k 行（胶水 + 桥 + 生成脚本）**，**搬入 ≈ 11 k 行（不改逻辑）**。

### 7.3 「谁在哪个文件加什么」—— 一页施工单

| # | 文件 | 动作 | 负责层 |
|---|---|---|---|
| 1 | `OUR/cpp/platform/nvn_vk2/gfx_vk_{core,exec,tex,shader,dq,subgroup}.c` | 搬入 nx2 原文件 | 后端 |
| 2 | `OUR/cpp/platform/nvn_vk2/gfx_vk.h` | 改 7 行（平台宏/窗口头/surface/扩展白名单） | 后端 |
| 3 | `OUR/cpp/platform/nvn_vk2/abi/*.h` | 搬入 nx2 的 6 个 ABI 头（**新目录，勿覆盖**） | ABI |
| 4 | `OUR/cpp/platform/nvn_vk2/hle_nvn.c` + `nvn_entry.S` + `nvn_thunks.S` | 搬入 | 录制 |
| 5 | `OUR/cpp/platform/nvn_vk2/ohos_dm_stub.c` | **新写** 28 个 `dm_*` 空桩 + `LOG/die`→hilog | 胶水 |
| 6 | `OUR/cpp/platform/nvn_vk2/ohos_present_bridge.c` | **新写** 离屏 → `win_fill.c` 拷贝桥 | present |
| 7 | `OUR/cpp/platform/nvn_vk2/selftest_tri.c` | **新写** 固定 PSO 自测 | 验证 |
| 8 | `OUR/cpp/platform/gfx_bridge.c:6435` | 改 `nvn_lookup()` 为转发 nx2 表 + 保留 SMO 补丁钩子 | HLE |
| 9 | `OUR/cpp/platform/vk_boot.c` | 接 `mtl_init()` / origin 设置 | 启动 |
| 10 | `OUR/cpp/CMakeLists.txt:34-56` | arm64 分支换源，include 顺序把 `nvn_vk2/abi` 前插 | 构建 |
| 11 | `tools/m41/build_dmsv.sh` + `gen_spv_headers.py` | **新写** bundle 与内置 shader 生成 | 构建 |
| 12 | `OUR/cpp/platform/nvn_vk/nvn_vk.c` | **停编**（保留文件与 `g_ohos_nb_stripped` 知识作回退/参考） | 回退 |

---

## 8. Q8 风险 top 5（按「会卡住多久」排序）

### R1 —— 录制层切换：`gfx_bridge` 直喂 vs nx2 录制（**可卡 5–8 天**）

- **是什么**：我方 `gfx_bridge.c` 的 98 条 NVN 实现在调用点直接改 GPU 状态（`nvn_vk_*`）；nx2 是「调用点只写 op 记录，
  提交时 `mtl_exec` 整批回放」。两套模型**不能并存**：一边写记录一边直改，会出现「记录里有的状态直喂没设 / 直喂设的又被回放覆盖」。
- **为什么最贵**：它不是编译错误，而是**画面错但不报错**——正是过去 40 轮踩过的坑型。
- **缓解（必须做）**：
  1. **物理隔离**：`gfx_bridge.c` 只保留「符号表 + 对象内存管理 + SMO 特化补丁钩子」，**所有 `nvn_vk_*` 调用点全部删除**，
     用 `grep -c "nvn_vk_" gfx_bridge.c` 作为门禁（切片 2 结束时该计数必须 = 0，除 present 桥）；
  2. **记录数对齐**：打开 `DM_VK_ONLY_RECORD`（自己加一个开关：只录 `mtl_exec` 不执行），比对
     「一帧 op 数」与 nx2 在 Android 的日志量级（`android.tsv:6` 给的是 1,230 draws/帧，op 数应为其 3–5 倍）；
  3. **`nvn_obj.h` 整取**，禁止混头（§3.3）。

### R2 —— DMSV bundle 构建链（**可卡 3–6 天**）

- **是什么**：27,984 个着色器要过 Ryujinx 系 .NET 翻译器；任一环节（RomFS 解包、`shader_split.py` 的 `ctl_len` 规则、
  `dmsh_xlate.dll` 在本机 macOS 的 dotnet 运行时、`--zstd` 版本、`shader_facts.tsv` 的 compute 答案）错位都会得到「能跑但花屏」。
- **缓解**：
  1. **切片 0 就先跑 200 key 的子集**（`shader_vk.py --keys a,b,c,...`），验证 dll 能在本机跑起来再谈全量；
  2. **对照物**：`shader_pairs.tsv` 可作为「配对是否正确」的自动断言（每个 vs 必须配到 ps，配不上就报）；
  3. **失败可回退**：我方已有 `spv.bin` + `shader_cache/`（`nvn_vk.c:6355/6785`）；切片 0 的 key 命中率若 ≥99%，
     可先用我方旧语料 + nx2 的 `nvk_tex`/反射重建 bundle，把 RomFS 重解推后。

### R3 —— 无零拷贝的池同步性能（**可卡 2–4 天**）

- **是什么**：`DM_VK_NO_ZEROCOPY=1` 时每提交前要把所有脏池页 memcpy 到 GPU 缓冲（`gfx_vk_core.c:564`、`gfx_vk.h:71-72`）。
  SMO 世界帧 ≈1,230 draws，若每提交都全量同步，CPU 会先于 GPU 饱和。
- **缓解**：
  1. 先测 **`g_ns_submit`（每帧提交耗时）** 与 `queue submit %.2f ms`（`gfx_vk_present.c:1032` 已有打印）；
     阈值：> 6 ms 即判定为瓶颈（60 fps 预算是 16.7 ms）；
  2. 增量同步：nx2 已有 `pkg_stamp / pkg_hash / baseline_complete`（`gfx_vk.h:101-107`）机制，只同步变更页；
  3. 若仍不够 → 上分支 A（`OH_NativeBuffer` 导入），**改动面仅 `gfx_vk_core.c` 的 `pool_zero_copy` 约 120 行**。

### R4 —— ABI 头双版本共存导致误编（**可卡 2–3 天**）

- **是什么**：我方 `OUR/cpp/third_party/d2r_gfx/gfx/nvn/` 里已有同名 `nvn_obj.h`（315 行）等 20 个头，
  `CMakeLists.txt:75-77` 已经把该目录放进 include path。**nx2 的 `nvn_obj.h`（219 行）一旦与之同名共存，
  谁先被 include 取决于顺序**，而 opcode 编号不同（§3.3）→ 静默错画。
- **缓解**：
  1. **新目录 `nvn_vk2/abi/`，include 前插**（`CMakeLists.txt` 里放在 `third_party/d2r_gfx/gfx/nvn` 之前）；
  2. 加**编译期断言**：nx2 的 `enum op` 独有 `OP_CLEAR_BUF / OP_FENCE_SYNC / OP_TF_BIND / OP_TF_STATE / OP_VP_SWIZZLE /
     OP_BIND_TEX1 / OP_PRIM_RESTART / OP_DRAW_TF` 这 8 个成员，**我方旧 `nvn_obj.h` 里根本没有**（旧树该枚举共 44 项含哨兵，
     `OP_COUNT_` = 43；nx2 是 52 项，`OP_COUNT_` = 51）。因此只要 `gfx_vk_exec.c` 被编进来，**误取旧头必然在编译期报
     "OP_CLEAR_BUF undeclared"，不会静默错画** —— 这就是天然的防线。再加一条显式锚定更稳：
     `_Static_assert(OP_COUNT_ == 51, "M41A: op 枚举必须是 nx2 那一版");`
  3. 加**链接期断言**：`nm` 里搜不到 `nvn_vk_present_clear` 即证明旧后端真的停编了。

### R5 —— 真机分支 A 的 `acquire` 崩溃 / 分支判定的不确定性（**可卡 2–3 天**）

- **是什么**：我方已有铁证（`nvn_vk.c:1504-1590`）：只要 `VK_OHOS_native_buffer` 缺失，
  OHOS 隐式层在 `vkCreateDevice` 就 `return -3`，且 `acquire` 会解引用 NULL（cppcrash）。
  真机是否具备该扩展**由另一个子代理实测**，本方案按两分支给。
- **缓解**：
  1. **能力探针前置**：`gfx_vk_core.c:190` 的扩展白名单改为「探测后构造」，把 `VK_OHOS_native_buffer` 当可选；
  2. **fail-closed**：`mtl_has_window()` 返回 0 时 present 只计数不碰 swapchain（照抄 `nvn_vk.c:29612` 的绕开逻辑），
     **绝不让图形层把进程拖死**（这条是我方 M1 的血泪约定：`nvn_vk.h:47` "失败返 0 ... 宿主保持 stub 模式不变"）；
  3. **分支 B 为默认交付**，分支 A 只在实测通过后作为性能选项打开（`DM_OHOS_SWAPCHAIN=1`）。

### R5.5（附加，非 top5 但要知道）—— 内置 SPIR-V 缺 `glslc`

本机只有 `glslangValidator`（`/opt/homebrew/bin/`），无 `glslc`。nx2 的 8 个内置 shader 用 NDK `glslc` 生成（`build.py:40-50`）。
**缓解**：写 20 行脚本改用 `glslangValidator -V`，或用 `spirv-as` 从现成 `.spvasm` 生成。不构成阻塞。

---

## 9. 总结论

1. **移植可行**，且比预期近：两边同为 d2r-mobile 血统、共用 `nvn_mtl.h` 的 `mtl_*` ABI（我方已有该头的超集）。
2. **但必须整支替换四件套**：`gfx_vk_*` + `hle_nvn.c` + `nvn_obj.h/nvn_rec.h` + `nvn_dirty.c`。
   **只搬 `gfx_vk_*` 一定会死**（opcode 枚举分叉，§3.3）。
3. **需要胶水层**：宿主 28–34 个 `dm_*` 钩子（≈360 行空桩）+ OHOS present 桥（≈260 行）+ 自测（≈220 行）+ 生成脚本（≈150 行）
   ⇒ **新写 ≈1.6 k 行，搬入 ≈11 k 行**。
4. **「颜色不对」有解且路径明确**：不是修 30K 行探针，而是用 `shader_map.tsv`（27,984 条，SMO 1.4.1 同版本同 title）
   + `shader_vk.py` 重建 DMSV bundle，让描述符绑定来自**反射**而不是猜槽。
5. **第一刀切在最便宜处**：切片 1「空后端 + 固定 PSO 三角形 + offscreen PPM」不碰游戏、不碰 bundle、不碰录制层，
   却同时排掉「OHOS 建链 / 无零拷贝 / ABI 头共存」三个最大未知。
