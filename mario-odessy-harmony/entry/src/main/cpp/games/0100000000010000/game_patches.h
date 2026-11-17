#pragma once
#include "runtime.h"

/* TOTK version-specific game-code patches, applied to the loaded main module after relocation and before its text is sealed.
   Every site is checked against the original instruction word first; a mismatch stops the boot. */
void runtime_game_patches(Module *main_module);

/* round20 最小扰动构建开关（threads.c 与 game_patches.c 共用）。
   0＝编译关闭三个重探针：early1d_note（boot 前 10s 持全局锁 G 的 0x1D 键
   全录账本——256 槽线性扫在锁内，boot 期扰动最大源）、KICK1 门挂的 kick 三条件快照
   （[this+0xf9]/[this+0x20] 双读+首 16 环+6 边际计数）、BC310 first-16 唯一 x0 环
   （45807 hits×CAS 去重）。1＝恢复重观测。
   round21 门集合收敛：E1/E2a/E2b/CELL2/DRAIN_ENTER/DRAIN_LOOP/KICKVT/KICKBR/MQ_RET/
   MQ_ADV 十门退役（E1 观察器 26k/s 洪流全容器快照是现存最大每命中成本）；保留 kick
   三门（KICK1/ALKICK/KICK3 轻量计数）、BC310（升级 LR 捕获：观察器参5=门点 x30，
   首/末 LR+首 64 采样环+末见 {x1,tick}）、thunk 双门×2、REG230/REG258，新增注册调用者
   两门 FN_A=0x8b65e0（含 0x8b6688/0x8b6708 两循环）/FN_B=0x8b6920（含 0x8b6990）——
   三条 bl 实属两函数（round21 反汇编实证）。门 id 沿用旧值，18/19 由 MQ 改派 FN_A/FN_B。 */
#define SMO_OBS_HEAVY 0
