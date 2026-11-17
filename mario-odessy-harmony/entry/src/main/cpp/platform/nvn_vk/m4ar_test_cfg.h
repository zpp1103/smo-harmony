/* M4ar 单轮实验的固定档（只给本轮 boot 用；与 m4ap_test_cfg.h 同一约定：编译期固定，避免运行期竞态）。
 *
 * 本轮问题（派活单 P1）：M4aq 补烘 29 条后 draws 3.87×（32,658 → 126,442），但新增的 draw 全落在
 * `tgt=hostRT ext=1280x720`，该 RT 读回 mean=0/0/0/0（98,882 笔之后），画面 md5 也逐字节没变。
 *
 * 三条判据（本文件只控制「取证开关」，不改任何行为分支）：
 *   SMO_M4AR_NEWDRAW  1 = 对「含 M4aq 本轮新烘 key」的 draw 做逐槽/几何取证（默认开）
 *   SMO_M4AR_ND_MAX   最多取证几笔（每笔 = 1 条 [n] newdraw + ≤8 条 [n] slot + 一份 p1_dump）
 *   SMO_M4AR_BISECT   1 = 对第一笔「非实例化 + 有内容 FS(ntex>=5)」的新 draw 跑 p2 A~J 二分
 *                     （debug RT 会按该笔的 1280×720 重建一次；见 p2_ensure 的 allow_resize）
 */
#ifndef SMO_M4AR_TEST_CFG_H
#define SMO_M4AR_TEST_CFG_H

#ifndef SMO_M4AR_NEWDRAW
#define SMO_M4AR_NEWDRAW 1
#endif
#ifndef SMO_M4AR_ND_MAX
#define SMO_M4AR_ND_MAX 3
#endif
#ifndef SMO_M4AR_BISECT
#define SMO_M4AR_BISECT 1
#endif

#endif
