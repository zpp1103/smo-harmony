# 2026-11-12 里程碑补丁（模拟器画质 + 真机上屏/UBO）

> 全部开关默认 0 = 逐字节旧行为；本目录只是补丁存档，游戏 ROM/派生产物不入库。

| 补丁 | 作用 | 基线 |
|---|---|---|
| `m73a.patch` | 雾链 pass71 换算式重标定：`t=clamp(f·A+B)`，A=1/B=0 线性视距（t 由二值变连续） | pristine |
| `m74a.patch` | 剩余白第二来源：`luma`（c0 头 4 float = Rec.601×10−2）+ 8 级累加末级压缩 | M73A |
| `m75a.patch` | 把 M73A+M74A 最佳值**烘成缺省**（斑区 22.96%→5.09%、整屏 nw 0.88%/mean 67.5），并修 `smo_m74a_p="0"` 解析阻断项 | pristine |
| `m82a.patch` | 真机黑屏根因：小 UBO 超额读 4096B 跨 VMA 页尾 EFAULT ⇒ cp=0 ⇒ 整槽 memset 清零；改为 VMA 钳位读+保留前缀+只清尾部 | pristine（含 M67B/M68B/M80A） |
| `m83a_combined.patch` | 上屏修复 + 雾链画质修复的**合并版**（真机目标 HAP 用） | pristine |

补丁根不同：`m82a.patch` 以仓库根为基准（`mario-odessy-harmony/...`），`m75a.patch`/`m73a.patch`/`m74a.patch` 以 `entry/src/main/cpp` 为基准（`nvn_vk2/...`），`m83a_combined.patch` 已按合并内容生成。
