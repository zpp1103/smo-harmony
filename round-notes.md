# SMO 移植 round 流水 notes（构建留存等）

## 构建留存（HAP 备份）

| round | HAP 备份路径 | 状态 |
|---|---|---|
| round16 | ——（无备份） | **找不到**：构建输出 `mario-odessy-harmony/entry/build/default/outputs/default/entry-default-signed.hap` 每轮被覆盖，round16 产物已被 round17/round18 构建逐轮覆盖。round19 查证时全项目 `*.hap` 仅剩 build outputs 内 signed/unsigned 两份（mtime 2026-11-01 11:32，与 round18 patch 生成时间一致 ⇒ round18 产物），logs/emu_round16*.log 仅运行日志不含 HAP。时间戳不可辨回 round16。 |
| round19 | `hap-archive/entry-default-signed.round19.hap`（构建 2026-11-01 12:03，63,478,052 B） | 本轮构建后归档（round19 = thunk 双门×2 + BC310 first16 x0 环观测，见 `patches/round19-thunk2gates-bc310x0ring-*.patch`；装机后 hilog 实证 16 门全装：`0x587da4 tail b->main+0x587ea0` / `0x587eac tail ret replayed` / `0x587db4 tail b->main+0x587eb0` / `0x587ec4 tail ret replayed`，tail page 1920 bytes used）。 |

> 惯例：round19 起新建 `hap-archive/`，每轮构建后把 signed HAP 归档一份（`entry-default-signed.round<N>.hap`），避免再出现无法回溯的覆盖。
