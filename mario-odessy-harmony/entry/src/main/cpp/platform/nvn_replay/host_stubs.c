/* 宿主侧桩：补齐 infra NVN 层引用、但我们尚未移植的两个符号。
 *
 * 1) guest_call(Cpu*, uint64_t) —— infra 用它从宿主回调进客体代码（rt.h:113）。
 *    cmdbuf.c:180 在录制 nvnCommandBufferInit 之类时用它做 guest callback；
 *    M2 第一版不做客体回调（回调只用于少数 NVN 的完成通知），打桩记一次日志即可。
 *
 * 2) nvn_watch_texid(void) —— infra objects.c:134 的「纹理监视」调试勾子，返回要监视的纹理 id；
 *    -1 表示不监视（cmdbuf.c:319 用它做一次 id 比较，不监视时永不相等）。
 *
 * 这两个符号在 infra 里分别属于 runtime/dispatch.c 与 runtime/nvn/objects.c，都不在我们已移植的范围内。
 */
#include <stdint.h>
#include <stdio.h>

#include "compat.h"

static unsigned g_guest_call_dropped;

void guest_call(Cpu *c, uint64_t addr)
{
    (void)c;
    if (g_guest_call_dropped++ < 4)
        fprintf(stderr, "[nvn_replay] guest_call(0x%llx) 未实现，已跳过（M2 第一版不做客体回调）\n",
                (unsigned long long)addr);
}

int nvn_watch_texid(void) { return -1; }

unsigned nvn_replay_guest_call_dropped(void) { return g_guest_call_dropped; }
