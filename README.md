# smo-harmony（switch2hos）

《超级马里奥 奥德赛》→ 鸿蒙（HarmonyOS）原生移植。本仓是**从私有开发仓过滤生成的公开镜像**，
不是可发布的成品。

- **架构**：HLE —— 不模拟 CPU，客体的 ARM64 机器码直跑；宿主实现 `nn::*` 系统服务；
  NVN 图形调用重放到 Vulkan（OHOS 上当前走 SwiftShader）。
- **目标**：title `0100000000010000`，v1.4.1。
- **内容**：宿主与移植层源码（`mario-odessy-harmony/`）、NVN→Vulkan 翻译层（`platform/nvn_vk/`）、
  各里程碑最小补丁存档（`patches/`，47 组）、取证与跑轮工具（`tools/`）、文档。

## 重要：本仓**不包含**游戏数据

以下一律不入库（版权 + 法律风险）：

- `game-work/`（NSP / RomFS.img / romfs 分片 / ExeFS 等）
- `infra/`、`third/`（外部参考实现的克隆）
- `*.nsp *.xci *.ncz *.nca *.img *.hap *.so *.bin`
- 任何 `prod.keys` / `title.keys` / 签名与密钥文件
- 游戏原版美术（应用图标使用中性占位图）
- 系统字体转出的字体 blob（`shared_fonts_data.h`，需本地生成）

运行本工程需要**自备**已购游戏与你自己的密钥，仓库只提供移植层代码。

## 仓库结构（节选）

| 路径 | 内容 |
|---|---|
| `mario-odessy-harmony/` | 鸿蒙工程主体（entry + C++ 移植层，含 `platform/nvn_vk/nvn_vk.c`） |
| `nxnative-ohos/`、`zelda-tk-harmony/`、`minimal-test/` | 相关工程、派生档案与最小验证样例 |
| `patches/` | 各里程碑的最小补丁存档（47 组，按 round 命名） |
| `tools/` | 跑轮/取证工具：`m*_boot_run.sh`、`m*_an.py`、`m41b/probe-hap` 等 |
| `docs/` | 路线与轮次笔记，`docs/DERIVED.md` 说明派生接口信息 |
| `round-notes.md`、`PORT_PLAN.md`、`PROJECT_ENV.md` | 轮次流水、总路线、环境速查 |
| `LICENSE`、`NOTICE` | MIT 许可、第三方代码归属 |

## 当前进度

最近的公开快照对应里程碑 **M92A–M105A**（`platform/` 同步 m104a 链）。逐轮结论见
`round-notes.md` 与 `docs/round-notes.md`，最新一手的取证记录在各 round 的补丁
与 `tools/` 下对应脚本里。

历史节点（2026-11-09 的 M39–M40 阶段）曾记录：present 通路已打通、720×405 的 RT 是无人写入的死槽、
真正被绘制的是 800×450 的槽、主线程每帧与自家 draw 提交线程 1:1 同步（≈185 ms/帧）。
这些结论可能已被后续里程碑推翻，以补丁与笔记为准。

## 首次构建要自己补两样东西

1. **字体 blob**：`platform/generated/shared_fonts_data.h` 未入库（字体不可再分发），
   按 `docs/DERIVED.md` 的契约用自己有权的字体生成。
2. **签名**：各工程 `build-profile.json5` 的 `signingConfigs` 为空，
   在 DevEco Studio → Project Structure → Signing Configs 里自动配置即可。

## 发布前自检

```bash
# 不该出现的四类东西：游戏数据/密钥、签名口令、游戏美术、生成物
git ls-files | grep -Ei '\.(nsp|xci|ncz|nca|img|hap|p12|p7b|keys)$|^game-work/'
grep -rn 'keyPassword\|storePassword|certpath' --include=build-profile.json5 .
git ls-files '*.jpg' '*.png' '*.webp' | xargs -r grep -la 'Nintendo\|AuthoringTool'
git ls-files | grep -E 'shared_fonts_data\.h|__pycache__|\.pyc$'
```

四条都无输出即通过。允许保留的派生接口信息见 `docs/DERIVED.md`。

## 许可

移植层代码 MIT（见 `LICENSE`）；其中若干部分派生自 nxnative / d2r-mobile 等 MIT 项目，
版权与许可声明见 `NOTICE` 与 `third_party/` 下各 LICENSE。
《超级马里奥 奥德赛》为任天堂作品，本项目与其无任何关联。
