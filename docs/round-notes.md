# SMO 移植 round 流水 notes（构建留存等）

## 构建留存（HAP 备份）

| round | HAP 备份路径 | 状态 |
|---|---|---|
| round16 | ——（无备份） | **找不到**：构建输出 `mario-odessy-harmony/entry/build/default/outputs/default/entry-default-signed.hap` 每轮被覆盖，round16 产物已被 round17/round18 构建逐轮覆盖。round19 查证时全项目 `*.hap` 仅剩 build outputs 内 signed/unsigned 两份（mtime 2026-11-01 11:32，与 round18 patch 生成时间一致 ⇒ round18 产物），logs/emu_round16*.log 仅运行日志不含 HAP。时间戳不可辨回 round16。 |
| round19 | `hap-archive/entry-default-signed.round19.hap`（构建 2026-11-01 12:03，63,478,052 B） | 本轮构建后归档（round19 = thunk 双门×2 + BC310 first16 x0 环观测，见 `patches/round19-thunk2gates-bc310x0ring-*.patch`；装机后 hilog 实证 16 门全装：`0x587da4 tail b->main+0x587ea0` / `0x587eac tail ret replayed` / `0x587db4 tail b->main+0x587eb0` / `0x587ec4 tail ret replayed`，tail page 1920 bytes used）。 |
| round21 | `hap-archive/entry-default-signed.round21.hap`（构建 2026-11-01 14:03，63,464,995 B） | 注册调用者两门（FN_A=0x8b65e0/FN_B=0x8b6920，三条 bl 实属两函数）+ BC310 LR 捕获（观察器参5=门点 x30，首/末 LR+首 64 采样环），10 门退役，见 `patches/round21-regcaller-fna-fnb-bc310lr-*.patch`；装机 hilog 实证 12 门全装（id 4/5/6/9/12-19，tail page 1552 bytes used）。首跑读数：bc310 hits=45807、first_lr→main+0x748198（`bl 0x93e810` w0=0xc0 后）、采样环几十个不同调用点（0x93exx0 分配器族尾跳进入）——"外层循环"前提证伪：45807 hits=boot 期分配次数，调用点遍布全镜像。 |

> 惯例：round19 起新建 `hap-archive/`，每轮构建后把 signed HAP 归档一份（`entry-default-signed.round<N>.hap`），避免再出现无法回溯的覆盖。
