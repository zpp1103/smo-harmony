/* M4ao 单轮实验的固定档（只给本轮 boot 用；默认全 0 = 生产行为）。
 *
 * 为什么不用运行时开关：目标机 hdc shell 是 uid 2000，**不能在应用沙箱里新建文件**
 * （M4ae 实测 Permission denied），所以 files/smo_replay_flags 这类运行期旋钮在「首次启动」
 * 无法由主机写入；脚本只能覆盖应用自己建出来的 666 文件（smo_gpu_write 就是这么工作的）。
 * 本轮要在**进程第一笔 draw 之前**就生效，故用编译期固定档，一次 boot 一个组合，避免竞态。
 *
 * 用法：跑 boot 前改这里两个宏 → 重新 assembleHap → install。跑完恢复 0/0。
 *   SMO_M4AO_BASEV=1  等同 SMO_REPLAY_BASEV=1（BaseVertex 类整类重放）
 *   SMO_M4AO_PAIR=1   等同 SMO_SKIP_PAIR=1（按 vs/fs 对门控 M4ae 那对全屏 quad）
 */
#ifndef SMO_M4AO_TEST_CFG_H
#define SMO_M4AO_TEST_CFG_H

#ifndef SMO_M4AO_BASEV
#define SMO_M4AO_BASEV 0
#endif
#ifndef SMO_M4AO_PAIR
#define SMO_M4AO_PAIR 0
#endif

#endif
