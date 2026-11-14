/* M4ap 单轮实验的固定档（只给本轮 boot 用）。
 *
 * 与 M4ao 同样的理由：目标机 hdc shell 是 uid 2000，不能在应用沙箱里**新建**文件，
 * 运行期旋钮（files/smo_replay_flags）只能覆盖应用自己建出来的 666 文件；而本轮组合
 * 必须在**进程第一笔 draw 之前**生效 ⇒ 用编译期固定档，一次 build 一个组合，避免竞态。
 *
 * 本轮组合（派活单 P2 指定）：basev=1 + pair=1（规避 M4ae 那对全屏 quad 刷红）+ inst=1。
 * 跑完应把它们改回 0/0/0 = 生产行为。
 *
 *   SMO_M4AP_BASEV     1 = BaseVertex 类整类重放（= SMO_REPLAY_BASEV）
 *   SMO_M4AP_PAIR      1 = 按 (vs,fs) 对门控 M4ae 那对全屏 quad（= SMO_SKIP_PAIR）
 *   SMO_M4AP_INST      1 = Instanced 类整类重放（= SMO_REPLAY_INST）
 *   SMO_M4AP_INST_CAP  单笔实例数上限（软渲保守值；超了按 fb=cap 记账，可用 SMO_INST_CAP 覆盖）
 */
#ifndef SMO_M4AP_TEST_CFG_H
#define SMO_M4AP_TEST_CFG_H

#ifndef SMO_M4AP_BASEV
#define SMO_M4AP_BASEV 1
#endif
#ifndef SMO_M4AP_PAIR
#define SMO_M4AP_PAIR 1
#endif
#ifndef SMO_M4AP_INST
#define SMO_M4AP_INST 1
#endif
#ifndef SMO_M4AP_INST_CAP
#define SMO_M4AP_INST_CAP 32
#endif

/* M4aq（2026-11-04）：本轮往语料里补烘的条目数，**只作台账打印**，不参与任何行为分支。
   值 = /tmp/m4aq_bake/merge_m4aq.py 的 `added entries`：31 个新 key 里剔除 2 个声明 Int64 的 VS
   （SwiftShader shaderInt64=0，与 slim_bin.py 同口径）后 = 29（19 VS + 12 FS → 17 VS + 12 FS）。 */
#ifndef SMO_M4AQ_BAKED
#define SMO_M4AQ_BAKED 29
#endif

#endif
