# Switch 游戏 → 鸿蒙（HarmonyOS NEXT）转译方案

> 2026-10-27 深夜会话产出。基于 nxnative / MacNativeGames 双管线实地勘察 + 两款游戏已完成的本地转译。

## 一、已完成的转译工作（本机已验证）

| 步骤 | 奥德赛 v1.4.1 | 王国之泪 v1.1.2 |
|---|---|---|
| 提取（nx_extract，.NET10） | ✅ NSO×4 + RomFS.img 5.4GB | ✅ NSO×4 + RomFS.img 16.2GB |
| NSO 代码重写（patch_nso.py） | ✅ 4 模块 exit 0，main 21.8MB | ✅ 4 模块 exit 0 |
| 产物位置 | `game-work/smo_0100000000010000` + `smo_build/patched/` | `game-work/totk_0100F2C0115B6000` + `totk_build/patched/` |

转译后的 `.patched.bin` 是重写好的 ARM64 代码镜像（宿主启动时映射到游戏地址空间直接执行），**平台无关**，通过汇编 `embed.S`（.incbin）嵌入宿主可执行文件。鸿蒙侧同样处理，无需重跑。

## 二、目标架构（鸿蒙 HAP）

```
mario-odessy-harmony.hap
├── ArkTS 层（entry 模块）
│   ├── EntryAbility        —— 窗口/生命周期/权限
│   └── pages/Index.ets     —— 启动页（设置：帧率/渲染精度/语言；触控虚拟手柄；之后跳 native）
├── Native C++ 模块（cpp/）
│   ├── game_napi.cpp       —— NAPI 桥：start/stop/touch/gsensor/帧循环回调
│   ├── host/               —— Horizon HLE 宿主（C，从 MacNativeGames odyssey runtime 移植）
│   ├── nvn_vk/             —— NVN→Vulkan 后端（重写部分，见下）
│   ├── shaders/            —— 游戏着色器（SPIR-V，见下）
│   └── embed.S             —— .incbin 游戏四个 patched.bin
└── resources/rawfile/
    └── romfs.img           —— 5.4GB RomFS 镜像（或首启时拷到沙箱，见风险 3）
```

## 三、宿主代码选型（关键决策）

**主参考：MacNativeGames/games/odyssey/runtime/**（比 nxnative 通用宿主更优）：
- 奥德赛专用、版本 1.4.1 **与我们的游戏文件完全匹配**（nxnative 通用管线适合提取/转译，宿主层它是 iOS 目标）
- 纯 C 实现（account.c / audio_*.c / applet_host.c / autoload.c…），POSIX 面向 OHOS libc 可编译
- 已知 60fps 调优结论可直接复用

**辅助参考：nxnative/dm/host/**（通用 svc/内核语义，Atmosphère 对齐）与 `dm/gfx/nvn/nvn_tex.c`（纹理布局，平台无关）。

**王国之泪同理**：MacNativeGames/games/totk/ 提供宿主参考。

### 平台映射表
| Apple 侧 | OHOS 侧 | 说明 |
|---|---|---|
| Metal 4 / MetalFX | **Vulkan**（OHOS 原生支持，NDK 带 vulkan 头） | NVN 层重写点，工作量最大 |
| Mach 线程/信号/计时 | OHOS pthread / sys/prctl / clock_gettime | 大多可直接编译 |
| AppKit/UIKit 窗口与输入 | ArkTS XComponent + NAPI | 壳层重写 |
| AVFoundation 音视频 | OHOS NativeAudio (oh_audio) / AVPlayer | 音频重接，视频后续 |
| 文件/Documents | 沙箱目录 context.filesDir | 保存/日志路径改传参 |
| 传感器（甩动投帽） | @ohos.sensor（accelerometer） | ArkTS 侧采集转发 |

## 四、图形：NVN → Vulkan

- 好消息：两管线的着色器翻译器都是 **Ryujinx.Graphics.Shader → SPIR-V**。Apple 侧还要 SPIRV-Cross 转 MSL，**鸿蒙侧 Vulkan 直接吃 SPIR-V，反而少一步**。
- 需要实现的 NVN 函数面（nxnative dm/gfx 已列 slots）：device/queue/command buffer、texture+BC 压缩（OHOS Vulkan 支持 BC 扩展，Mate60+ 验证过）、sampler、shader/pipe、memory（buffer/image 别名）、swapper/present（XComponent surface → VK_KHR_android_surface 等价物）。
- 实施顺序：先 swapper+clear 三角（验证 surface）→ 纹理/采样 → 命令流重放 → 同步原语。

## 五、构建与调试

- 工具链：DevEco Studio 26.0 + `D:\HarmonyOS\clt\command-line-tools`（SDK 双套已解压，`DEVECO_SDK_HOME` 已 setx）
- NDK clang：`sdk/default/openharmony/native/llvm/bin/aarch64-unknown-linux-ohos-clang`
- 宿主+Vulkan 后端用 CMake（`externalNativeOptions`）编译进 `libgame.so`
- hvigor 备注：CLT 的 bin/hvigorw.bat shim 指向缺失目录；CLI 构建可用 DevEco 内置 hvigor（`C:\Program Files\Huawei\DevEco Studio\tools\hvigor\`）或 IDE 内构建（周末在 IDE 里第一次 Sync 即可）

## 六、今晚交付 vs 周末调试

**今晚（已完成/进行中）**：两款游戏提取+代码转译 ✅；两个鸿蒙工程骨架（ArkTS 壳 + NAPI + CMake + Vulkan 骨架 + embed 占位）✅；本方案 ✅。
**周末（需真机）**：宿主 C 源码移植编译修错 → Vulkan 后端逐步点亮 → RomFS 挂载与保存路径 → 触控/传感器联调 → 性能（Mate80 Pro Max 16GB 余量充足，注意 5.4GB/16.2GB RomFS 的装载方式）。

## 七、风险与未知（按影响排序）

1. **NVN→Vulkan 后端是最大工作量**（估 1-2 个周末的迭代）；nxnative 的 Metal 层是行为参考而非可直接编译的代码。
2. **OHOS Vulkan 的 BC 纹理压缩支持**需真机验证（游戏贴图是 BC 格式）；不支持则需运行时转码（内存/耗时代价大）。
3. **RomFS 5.4GB/16.2GB 打包**：HAP 包体积上限约 4GB，需首启时从外部存储拷入沙箱或分卷 rawfile；周末定。
4. patched.bin 镜像要求宿主按 Switch 地址布局映射内存（0x8000000 起，BIG_TEXT 时更大）；OHOS 64 位用户态 mmap 无压力，但需关闭对映射地址的 ASLR 干扰假设（宿主用 MAP_FIXED 自管）。
5. 王国之泪 1.1.2 的 nxnative 档案成熟度低于奥德赛；MacNativeGames/totk 可补。
6. CLT hvigor CLI shim 断链——周末用 IDE 内构建规避。

## 八、参考仓库索引

| 仓库 | 用途 |
|---|---|
| `infra/nxnative` | 主管线（提取/NSO重写/游戏档案/着色器工具） |
| `infra/MacNativeGames` | **宿主 C 主参考**（games/odyssey、games/totk） |
| `infra/LASRecomp`、`infra/mush8`、`infra/ssbu-recomp` | 旁证（静态重编译/打包形态参考） |
