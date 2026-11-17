#ifndef VK_BOOT_H
#define VK_BOOT_H
/* vk_boot_init: 在任何 vk* 符号被真正使用前，把随包的 SwiftShader ICD 接给随包 Khronos loader。
 * 返回 0 = 三件套齐全且 env 已设；负值 = 缺件（调用方照常回落 stub 模式）。 */
int vk_boot_init(void);
/* vk_boot_icd_path: 随包 SwiftShader ICD 的绝对路径（<libs>/libvk_swiftshader.so）。
 * M3b 新增：VK_LUNARG_direct_driver_loading 直加驱动时用，不经过 manifest/env。
 * 未初始化前返回 NULL。 */
const char *vk_boot_icd_path(void);
/* M3c 取证通道：关键行同时落应用私有文件（hilog 首波会流控丢行），append + 每行 fflush。
 * vk_boot_log_open: 打开成功返回 0（幂等，重复调用无害）；全候选不可写返回 -1。
 * vk_boot_logf: 一行一条，行首为 gettimeofday 的 sec.usec 时间戳；未打开时静默丢弃。 */
int vk_boot_log_open(void);
void vk_boot_logf(const char *fmt, ...);
/* M4n P1 配套：与 vk_boot_logf 同格式，但用 trylock —— 拿不到写锁就返回 -1、不写这一行。
 * 只给进程退出阶段（atexit）的兜底行用：那时别的线程可能正持锁写日志，用阻塞锁会让进程卡在 exit。 */
int vk_boot_logf_try(const char *fmt, ...);
/* 实际落盘路径（未打开返回 NULL）——取回时按它拼 hdc 路径。 */
const char *vk_boot_log_path(void);
/* M3n：把应用可写目录经 SMO_JIT_DIR 交给 SwiftShader Reactor（JIT 代码页文件背衬）。
 * 需在 vk_boot_log_open() 之后调用（复用日志所在目录）；成功返回 0。 */
int vk_boot_jit_env_setup(void);

/* ---------------- M3g：主循环探针（只观测，不参与任何控制流） ----------------
 * 六个原子计数各自的自然入口自增，插入点与「为什么每帧必过」写在各 .c 处；
 * vk_beat_start() 起一个每 2 秒的定时线程，用上面的文件日志打一行 [beat]。
 * 定时线程独立于渲染线程：主循环卡死时 [beat] 仍会刷新，计数不动即为卡死证据。 */
#include <stdatomic.h>
extern _Atomic unsigned long g_beat_frames;       /* 主循环心跳：nvn_window_acquire_texture */
extern _Atomic unsigned long g_beat_present_try;  /* present 尝试：nvn_vk_present_clear 入口 */
extern _Atomic unsigned long g_beat_nvn_submit;   /* NVN 提交：nvn_queue_submit_commands */
extern _Atomic unsigned long g_beat_img;          /* vkCreateImage 调用 */
extern _Atomic unsigned long g_beat_buf;          /* vkCreateBuffer 调用 */
extern _Atomic unsigned long g_beat_cmd;          /* vkCmd* 调用 */
void vk_beat_start(void);   /* 幂等；需先 vk_boot_log_open() 成功，否则计数照走但无行 */
/* M40P：建图洪流分桶统计（nvn_vk.c 实现；smo_m40p 关态直接 return，由心跳线程每 60 s 调一次） */
void nvn_vk_m40p_stat(void);
/* M40Z：场景主 pass 深度附件实验的每 60 s 一行状态（nvn_vk.c 实现；smo_m40z 关态直接 return） */
void nvn_vk_m40z_poll(unsigned t);
#endif
