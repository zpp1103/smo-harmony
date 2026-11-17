# NVN 回放层移植（来自 BOTWRecompAndroid@4bad6dc，MIT）

## 许可证
源文件取自 https://github.com/mamcjr/BOTWRecompAndroid commit `4bad6dcb0390ae81d0e5d2e174809422d7198d97`，
MIT License, Copyright (c) 2026 Peiikai。LICENSE 全文见 `infra/BOTWRecompAndroid/LICENSE`。

## 本目录内容

| 文件 | 来源 | 改动 |
|---|---|---|
| `cmdbuf.c` | `runtime/nvn/cmdbuf.c`（747 行） | **未改一行** |
| `nvn.h` | `runtime/nvn/nvn.h` | 删掉 `#include "../hle.h"`，改为包含 `compat.h` |
| `tegra.h` | `runtime/nvn/tegra.h` | 未改 |
| `compat.h` | 自写 | 提供 infra 依赖的 5 个宏/类型（见下） |
| `plat_ohos.c` | 自写 | NVN 层需要的 9 个 plat_* 函数（OHOS 版） |

## `compat.h` 替代了什么

infra 用链接段注册（`.hle$m`）+ 一套运行时宏；我们只取函数体，注册走我们自己的 `gfx_bridge.c` 符号表。

| infra 依赖 | 我们的替代 |
|---|---|
| `HLE_FUNC(sym, fn)` | `static void fn(Cpu *c)`（不注册） |
| `Cpu` 结构 | 最小声明（只 `x[32]` / `v[32]`，`cmdbuf.c` 只用到这两个） |
| `GUEST_LO/GUEST_HI` | `0x7000000000` / `0x7320000000`（infra `rt.h:41-42` 同值） |
| `LOG/FATAL/ENV` | `fprintf(stderr,...)` / `abort()` / 缓存式 `getenv` |

## 编译验证（2026-11-02）

```
clang -fsyntax-only -target aarch64-linux-ohos --sysroot=<OHOS SDK> -I. cmdbuf.c
→ 0 error, 5 warning（全部为 "unused function"，因去掉段注册后无调用者，接入时会消失）
```

## 接入状态（2026-11-02 已接线，原「尚未接入」）

`nvn_replay/` 已进 CMakeLists（`CMakeLists.txt:41-46`），录制族已在 `gfx_bridge.c` 的符号表里接线：
录制 15 项 + 记账/生命周期 9 项（第 2 步），明细与限制见文末「接线（2026-11-02）」。

仍**未**接的是回放侧（`vulkan/exec.c`，边界见下），所以录制出来的命令流目前只有 `cmd_trace` 消费。

## 第二块：Maxwell 命令翻译（2026-11-02）

| 文件 | 来源 | 改动 | 编译 |
|---|---|---|---|
| `maxwell/translate.c` | `runtime/nvn/maxwell/translate.c`（3,336 行） | **未改一行** | **0 error** |
| `maxwell/shader.h` | `runtime/nvn/maxwell/shader.h`（151 行） | 未改 | — |

它只依赖 3 个标准头 + `shader.h` + 一个外部符号 `rt_kind()`（infra 的 runtime kind 查询），
移植时打桩即可（`int rt_kind(void){return 0;}`），**连 `compat.h` 都不需要**。

验证命令：
```
clang -fsyntax-only -target aarch64-linux-ohos --sysroot=<OHOS SDK> -I. maxwell/translate.c
→ 0 error
```

## 边界：`exec.c` 为什么停在这里（2026-11-02）

`exec.c`（7,929 行，opcode → Vulkan 调用）**不是**像前两块那样能独立编译的：

| 依赖 | 牵扯 |
|---|---|
| `../../hle.h` | 他们的 HLE 派发层 |
| `../../kernel.h` | 他们的客体内核（线程/句柄） |
| `../renderer.h` | 渲染器接口 |
| `vk.h` + `shaders.h` + `timing_trace.h` | Vulkan 后端主体（含 VMA） |
| `../../settings.h` `../../paths.h` | 设置/路径 |

**SDL 依赖很小（只有 5 个符号）**，可轻易 shim：
```
SDL_GetTicksNS(31 处)        → plat_now_ns()
SDL_GetNumLogicalCPUCores    → sysconf(_SC_NPROCESSORS_ONLN)
SDL_DelayPrecise             → nanosleep()
```
真正的工作量是 `vk.h` / `renderer.h` / `kernel.h` 这条链——即整个 Vulkan 后端。

**结论**：`exec.c` 要跟 `vulkan/` 后端（12,662 行）一起搬，是一个**独立的大块**，不适合在周末前硬塞。
周末的最小验证路径应该是：**只接 `cmdbuf.c` 的录制 + 一个 draw opcode 的最小回放**，不搬整个 exec.c。

## 当前进度总表（2026-11-02）

| 块 | 行数 | 状态 |
|---|---|---|
| `cmdbuf.c` 命令录制 | 747 | ✅ 落树，0 error |
| `maxwell/translate.c` 命令翻译 | 3,336 | ✅ 落树，0 error |
| `plat_ohos.c` 平台层 | ~140 | ✅ 落树，0 error |
| `vulkan/exec.c` 回放执行 | 7,929 | ⛔ 需连 Vulkan 后端一起（边界） |
| `vulkan/` 后端 | 12,662 | ⛔ 同上 |
| `objects.c` NVN 对象模型 | 1,617 | ⬜ 依赖 vk.h，同边界 |

**已可编译总量：4,223 行**（录制 + 翻译 + 平台层）。

## 命令流契约与跟踪器（2026-11-02）

### 契约（`nvn.h:117-141`，录制端与回放端共用）

```c
typedef struct {
    uint32_t op, size;   // size 含头，8 字节对齐
    uint64_t x[7];       // 该次调用的 x1..x7
    float    f[4];       // s0..s3
    uint8_t  extra[];    // 拷贝数据（纹理/缓冲等）
} CmdRec;

enum { CMD_END = 0, CMD_JUMP, CMD_CALL, CMD_BIND_PROGRAM, … CMD_BIND_TEXTURES_DEFERRED, CMD_COUNT };
```

**62 个 opcode**。因为录制端（`cmdbuf.c`）与回放端（`exec.c`）都吃这个头文件，
**格式不存在不匹配风险**——要裁的只是「支持哪些 opcode」。

### `cmd_trace.c`（本目录，自写）

走一遍录制好的命令流，输出 opcode 直方图 + 首条非法记录位置。

```c
int  nvn_cmd_trace(const void *base, uint64_t bytes, CmdTrace *out);
void nvn_cmd_trace_report(const CmdTrace *t);
```

**它的用途**：在**没有 Vulkan 驱动的模拟器**上就能回答「SMO 实际发哪些 opcode、各占多少」，
从而把周末要实现的最小回放集从「按 BOTW 猜」变成「按 SMO 实测裁剪」。

编译：`0 error`（aarch64-linux-ohos）。

### 接入步骤（1-4 已完成，5 待设备轮）
1. ✅ `nvn_replay/` 已进 `HOST_SOURCES`（`CMakeLists.txt:41-46`）
2. ✅ `compat.h:15` 的 `HLE_FUNC` 已去掉 `static`，`cmdbuf.c` 的 `hle_nvn*` 可从胶水层调用
3. ✅ `gfx_bridge.c` 符号表已转发：录制 15 项 + 记账/生命周期 9 项（见文末）
4. ✅ `nvnQueueSubmitCommands` 里已调 `nvn_cmd_trace()` + `nvn_cmd_trace_report()`（submit #200 一次）
5. ⬜ 跑一次模拟器 boot，拿到 SMO 的真实 opcode 分布

## 胶水层 `glue_cmdbuf.c`（2026-11-02，自写）

把我们的**参数式**桩转成 infra 的 **`Cpu*` 式**实现。

参数约定（`nvn.h:10-12`）：`A(i) = c->x[i]`、`AF(i) = *(float*)&c->v[i]`。
所以胶水只做一件事：把普通 C 参数搬进 `Cpu.x[]` / `Cpu.v[]`，再调用 `hle_nvn*`。

已接 23 个入口（`glue_cmdbuf.c`）：
- 录制 15：Initialize、AddCommandMemory、AddControlMemory、Begin/EndRecording、DrawArrays、DrawElements、
  SetRenderTargets、ClearColor、BindProgram、BindVertexBuffers、BindTextures、BindUniformBuffers、SetViewports、SetScissors
- 记账/生命周期 8：Finalize、IsRecording、GetCommandMemory{Size,Used,Free}、GetControlMemory{Size,Used,Free}

参数顺序以**真实 NVN ABI** 为准（`third_party/d2r_gfx/gfx/nvn/nvn_protos.h:330-345/355-396/434`），
不是 `gfx_bridge.c` 里那些已退化成 `(void *c)` 的记账桩形参。第 1 轮接线时修正了 7 处参数序
（DrawElements 的 count 在第 4 参、ClearColor(index,color*,mask)、BindProgram(+stageMask)、
BindVertexBuffers、BindUniformBuffers、BindTextures、SetRenderTargets 6 参）。

`nvn_rec_dropped_calls()` 返回「参数超过 8 个被丢弃」的调用数——实测时用来判断有没有漏录。

**已参与编译**（`nvn_replay/` 已入 CMakeLists）。

### 五个文件全部 0 error（aarch64-linux-ohos）

```
cmdbuf.c           0 error   命令录制（原样）
glue_cmdbuf.c      0 error   胶水层（自写）
cmd_trace.c        0 error   命令流跟踪器（自写）
maxwell/translate.c 0 error  命令翻译（原样）
plat_ohos.c        0 error   平台层（自写）
```

## 接线（2026-11-02）—— 唯一事实：客体偏移 0 = 宿主 `NvnCmdBuf*`

`cmdbuf.c:26-31` 的 `nvnCommandBufferInitialize` 自己 `calloc` 一块宿主 `NvnCmdBuf`，
并把指针 `SET_HOST` 写进客体 `NVNcommandBuffer` 偏移 0（宿主地址==客体地址，既有前提）。
因此**所有**命令缓冲入口都必须走 `cb_of()` 取那块结构，不能再按 `gfx_bridge.c` 的 `cmd_h` 覆盖体读字段。

已接线（`gfx_bridge.c` 符号表；前→后 `nvn_command_buffer_*`/记账桩 → `nvn_rec_*`）：

| 类别 | 项数与去向 |
|---|---|
| 录制 | 15 项 → `nvn_rec_*`（Initialize/AddCommandMemory/AddControlMemory 在 `glue_cmdbuf.c:73-98`，其余 12 在 :107-201） |
| 记账/生命周期 | 9 项：Finalize→`nvn_cb_finalize_wired`；EndRecording→`nvn_cb_end_recording_wired`；IsRecording/GetCommandMemory{Size,Used,Free}/GetControlMemory{Size,Used,Free}→`nvn_rec_*` |

- 用量是**游戏字节**：`cmdbuf.c:168/196` 按 `SHADOW_K` 记账，`Get*`（`cmdbuf.c:134-142`）换回游戏字节；
  游戏正是累加 `GetCommandMemoryUsed` 定 RSTB 堆尺寸（`gfx_bridge.c:640` 注释）。
- `nvn_cb_finalize_wired` 先 `nvn_rcpt_drop` 再 `nvn_rec_command_buffer_finalize`（`cmdbuf.c:32` free + 清偏移 0），
  否则 Finalize 后残留的句柄映射会让旧句柄提交时写错回执。

### 回执契约保留（不静默丢）
回执**是被消费的**，不是纯记账：`nvn_rcpt_retire_locked`（`gfx_bridge.c:766-790`）在提交退休时
把 `ReportCounter` 的 16 字节 `{counter,timestamp}` `memcpy` 进客体内存（`gfx_bridge.c:772`），
`ReportValue`/`SignalEvent` 经 `nvn_cb_event_write`（`gfx_bridge.c:713`）写客体 event 值（:774-779）；
另有 `threads.c:1726-1731` 读 `dm_nvn_receipt_stat` 的调试计数。
由于 `EndRecording` 已改走 cmdbuf（句柄从 `g_nvn_handle_gen` 变成命令流首条记录地址），
补了 `nvn_cb_end_recording_wired`（`gfx_bridge.c:1018-1024`）把「新句柄 → CB 客体」写回 `nvn_rcpt_map`，
并保留 donor 的 `g_nvn_ctl_total += 64`/CB 记账，提交侧 `nvn_rcpt_lookup` 才能继续反查到回执。

### 三个已知限制（本轮未解，设备轮再评估）
1. **`g_cpu` 单线程**：`glue_cmdbuf.c:47` 一个静态 `Cpu`，录制若跨线程会互相覆盖；
   现在游戏录制在主线程（提交也在同一客体线程）可接受，多线程录制要改 `__thread` 或加锁。
2. **跨 shadow 页的 `CMD_JUMP` 不跟跳**：`cmd_trace.c:40-56` 只按 `size` 线性前进，
   列表跨 64KB 页时中间那条 `CMD_JUMP`（`cmdbuf.c:88`）的跳转目标不会被跟随，
   直方图会在那里报「首条非法记录」——是跟踪器的限制，不是录制失败。
3. **4 KiB bound**：`gfx_bridge.c:572` 的 `nvn_cmd_trace((void*)h₀, 0x1000, &tr)` 固定上限；
   正常情况下列表以 `CMD_END` 收尾（`cmdbuf.c:234`）远早于此，但若 4 KiB 内没有 `CMD_END`
   会继续读到该页后续数据（同一次 malloc 内，通常安全；极端越界未验证）。

### 构建（2026-11-02）
```
./build_hap_mac.sh  → 0 error / 3 warning（都在未改行 gfx_bridge.c:115/1005/1011）
产物 …/intermediates/libs/default/arm64-v8a/libgame.so
```
