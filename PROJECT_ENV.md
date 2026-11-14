# 项目环境速查（本机工具链与产物路径）

> 生成于 2026-10-27 深夜转译会话。所有路径均已验证存在。

## 鸿蒙工具链

| 项 | 路径 |
|---|---|
| command-line-tools 根 | `D:\HarmonyOS\clt\command-line-tools` |
| SDK 根（openharmony+hms 双套） | `D:\HarmonyOS\clt\command-line-tools\sdk\default` |
| 环境变量 | `DEVECO_SDK_HOME=D:\HarmonyOS\clt\command-line-tools\sdk`（已 setx 到用户级） |
| hvigorw | `D:\HarmonyOS\clt\command-line-tools\bin\hvigorw.bat` |
| ohpm | `D:\HarmonyOS\clt\command-line-tools\bin\ohpm.bat` |
| node | `D:\HarmonyOS\clt\command-line-tools\tool\node\node.exe` |
| hdc | `D:\HarmonyOS\clt\command-line-tools\sdk\default\openharmony\toolchains\hdc.exe` |
| DevEco Studio | `C:\Program Files\Huawei\DevEco Studio\bin\devecostudio64.exe`（26.0.0，欢迎窗已能启动） |

## 转译产物（game-work）

### 超级马里奥奥德赛 v1.4.1（title 0100000000010000，main build b424be15）
| 产物 | 路径 |
|---|---|
| 提取目录 | `C:\Users\macvi\VibeCoding\switch2hos\game-work\smo_0100000000010000` |
| ExeFS（NSO×4 + npdm） | `...\smo_0100000000010000\ExeFS\`（main 9.9MB / sdk 5.3MB / subsdk0 3.6MB / rtld） |
| 合并 RomFS 镜像 | `...\smo_0100000000010000\RomFS.img`（5.4GB，4972 文件） |
| 转译后代码 | `...\game-work\smo_build\patched\<module>\<module>.patched.bin`（main 21.8MB / sdk 14MB / subsdk0 8MB / rtld 66KB） |
| 转译元数据 | 同目录 `<module>.sites.tsv / gates.tsv / fixups.tsv / cuts.tsv` |
| 图标/名称 | `...\smo_0100000000010000\icon.jpg`、`control.nacp`（display version 1.4.1） |

### 塞尔达传说：王国之泪 v1.1.2（title 0100F2C0115B6000，main build 9a10ed94）
| 产物 | 路径 |
|---|---|
| 提取目录 | `C:\Users\macvi\VibeCoding\switch2hos\game-work\totk_0100F2C0115B6000` |
| ExeFS | `...\totk_0100F2C0115B6000\ExeFS\`（main 44.4MB 一组） |
| RomFS 镜像 | `...\totk_0100F2C0115B6000\RomFS.img`（16.2GB，304471 文件） |
| 转译后代码 | `...\game-work\totk_build\patched\<module>\` |

## 基础架构（infra，均为 GitHub clone）

- `infra/nxnative` —— 主管线（NSP→提取→NSO重写→宿主C→打包）。宿主 C 代码 `dm/host`+`dm/gfx`；游戏档案 `dm/games/0100000000010000`（奥德赛）、`dm/games/0100F2C0115B6000`（TotK）。
- `infra/MacNativeGames`、`infra/LASRecomp`、`infra/mush8`、`infra/ssbu-recomp` —— 参考实现。

## 密钥

- `E:\Yuzu\user\keys\prod.keys`（提取用，勿外传勿入库）

## 构建工具（PATH 可能需新 shell 生效）

- Python 3.13（lz4/capstone/fonttools/nsz 已装）、dotnet 10.0.403、cmake/ninja（winget 装，新终端生效）

## 待办锚点

- 鸿蒙工程：`mario-odessy-harmony`、`zelda-tk-harmony`（尚未创建，workflow 施工阶段产出）
- 适配方案文档：`PORT_PLAN.md`（workflow 产出后落在本目录）
- DevEco IDE 内 SDK 路径确认（周末用户在场时）：设置 → HarmonyOS SDK → 指向上面 SDK 根
