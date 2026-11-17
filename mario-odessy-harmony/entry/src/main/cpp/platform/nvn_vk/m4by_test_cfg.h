/* M4by 单轮实验的固定档（只给本轮 boot 用）。
 *
 * 为什么是编译期档：hdc shell 是 uid 2000，不能在应用沙箱里**新建**文件；运行期旋钮
 * （files/smo_replay_flags）只能覆盖应用自己种出来的 666 文件，而本轮要改的判定
 * 必须在本进程**第一笔 draw 之前**生效 ⇒ 用编译期默认值，一次 build 一个组合。
 *
 * 本轮组合（派活单 P1 指定）：pair 门仍然开（SMO_M4AP_PAIR=1），但**这一对**放行。
 *   SMO_M4BY_PAIR_UNGATE 1 = vs=46342a76690ceba3 / fs=b9f0ca8022fbf9c2 不再被 pair 门丢弃
 *                        （M4bx 已证 11144 笔 100% 落 texid=282、kv/kf/spv 全在、4 顶点全屏 quad）
 *                       0 = 恢复历史行为（这一对继续被门掉）
 * 运行期可用 SMO_PAIR_UNGATE 环境变量或 flags 文件 `ungate=` 覆盖（见 r_flags_resolve）。
 * 跑完必须改回 0 才是生产行为。
 */
#ifndef SMO_M4BY_TEST_CFG_H
#define SMO_M4BY_TEST_CFG_H

#ifndef SMO_M4BY_PAIR_UNGATE
#define SMO_M4BY_PAIR_UNGATE 1
#endif

#endif
