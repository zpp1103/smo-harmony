/* cmdbuf 胶水层：把 gfx_bridge 符号表的「参数式」NVN 入口转成 infra 的「Cpu* 式」实现。
 *
 * 为什么需要：infra 的 HLE 函数签名是 `void hle_xxx(Cpu *c)`（nvn.h:7 `NVN(name) = HLE_FUNC(#name, hle_##name)`；
 * compat.h:15 把它落成 `void fn(Cpu *c)`），参数从 c->x[i] 取（nvn.h:10 `#define A(i) (c->x[i])`），
 * 返回值写回 c->x[0]（nvn.h:13 `#define RET(v) (c->x[0] = (uint64_t)(v))`）。本层只做参数搬运，不含逻辑。
 *
 * 参数顺序的权威来源 = 真实 NVN ABI（third_party/d2r_gfx/gfx/nvn/nvn_protos.h:330-345/355-396/434），
 * 不是 gfx_bridge 里那些记账桩的形参——桩多数已退化成 `(void *c)`，形参不构成 ABI 证据。
 *
 * 接入方式（见 PORTING.md）：gfx_bridge.c 的符号表把相应名字指向本文件的 nvn_rec_* 即可。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvn.h"
#include "../vk_boot.h"   /* M3r 取证：录制侧取值写进 vk_boot.log（与回放侧 [r] apply 行对照） */

/* infra 侧实现（cmdbuf.c；compat.h 已把 HLE_FUNC 改为外部链接）。
   注意返回类型：cmdbuf.c 里的实现**全部是 void**，返回值写在 Cpu.x[0]（RET），
   所以这里只能声明 void，要返回值的入口读 g_cpu.x[0]。 */
void hle_nvnCommandBufferInitialize(Cpu *c);
void hle_nvnCommandBufferAddCommandMemory(Cpu *c);
void hle_nvnCommandBufferAddControlMemory(Cpu *c);
void hle_nvnCommandBufferBeginRecording(Cpu *c);
void hle_nvnCommandBufferEndRecording(Cpu *c);
void hle_nvnCommandBufferDrawArrays(Cpu *c);
void hle_nvnCommandBufferDrawElements(Cpu *c);
void hle_nvnCommandBufferDrawElementsBaseVertex(Cpu *c);   /* M4ad P2② */
void hle_nvnCommandBufferDrawElementsInstanced(Cpu *c);    /* M4ad P2② */
void hle_nvnCommandBufferDrawArraysInstanced(Cpu *c);      /* M5p C：cmdbuf.c:662 */
void hle_nvnCommandBufferSetRenderTargets(Cpu *c);
void hle_nvnCommandBufferClearColor(Cpu *c);
void hle_nvnCommandBufferBindProgram(Cpu *c);
void hle_nvnCommandBufferBindVertexBuffers(Cpu *c);
void hle_nvnCommandBufferBindVertexBuffer(Cpu *c);        /* M3r：单条形式（cmdbuf.c:330） */
void hle_nvnCommandBufferBindUniformBuffer(Cpu *c);       /* M3r：单条形式（cmdbuf.c:338） */
void hle_nvnCommandBufferBindTexture(Cpu *c);             /* M3r：单条形式（cmdbuf.c:356） */
void hle_nvnCommandBufferBindBlendState(Cpu *c);          /* M3i：状态绑定补录制 */
void hle_nvnCommandBufferBindColorState(Cpu *c);
void hle_nvnCommandBufferBindDepthStencilState(Cpu *c);
void hle_nvnCommandBufferBindVertexAttribState(Cpu *c);
void hle_nvnCommandBufferBindVertexStreamState(Cpu *c);
void hle_nvnCommandBufferBindTextures(Cpu *c);
void hle_nvnCommandBufferSetViewports(Cpu *c);
void hle_nvnCommandBufferSetScissors(Cpu *c);
void hle_nvnCommandBufferSetViewport(Cpu *c);              /* M4an P2：单数形式（cmdbuf.c:454） */
void hle_nvnCommandBufferSetScissor(Cpu *c);               /* M4an P2：cmdbuf.c:462 */
void hle_nvnCommandBufferSetDepthRange(Cpu *c);            /* M4an P2：cmdbuf.c:470（float 走 v[]） */
void hle_nvnCommandBufferSetBlendColor(Cpu *c);            /* M4an P2：cmdbuf.c:476 */
void hle_nvnCommandBufferBindUniformBuffers(Cpu *c);
void hle_nvnCommandBufferCallCommands(Cpu *c);            /* M4an：二级命令流 inline（cmdbuf.c） */
void hle_nvnCommandBufferCopyCommands(Cpu *c);            /* M4an：同上（donor 语义相同） */
/* 记账/生命周期族（第 2 步补接）：同一批 infra 实现，见 cmdbuf.c:32 / :134-142 / :249 */
void hle_nvnCommandBufferFinalize(Cpu *c);
void hle_nvnCommandBufferGetCommandMemorySize(Cpu *c);
void hle_nvnCommandBufferGetCommandMemoryUsed(Cpu *c);
void hle_nvnCommandBufferGetCommandMemoryFree(Cpu *c);
void hle_nvnCommandBufferGetControlMemorySize(Cpu *c);
void hle_nvnCommandBufferGetControlMemoryUsed(Cpu *c);
void hle_nvnCommandBufferGetControlMemoryFree(Cpu *c);
void hle_nvnCommandBufferIsRecording(Cpu *c);

/* 把 n 个 64 位参数装进 Cpu.x[] 后调用。参数个数多于 8 的调用（极少）不支持，直接跳过并计数，
   以便实测时能看出「有多少调用没被录制」。 */
/* M3i 崩溃修复（2026-11-03）：SMO 会同时从多个客体线程录制命令流（双缓冲命令列表），而 put() 每次
   memset 整个 Cpu 再填参数——两线程共用一份 g_cpu 时，A 线程在 hle 里读 x[0] 的瞬间被 B 线程 memset
   清零，cb=NULL 就被递进 cmdbuf 的 cb_of() 并解引用 0（cb_of 的 ldr x0,[x8]，x8=0）。
   实测 cppcrash 20261103003622329：submit#638 后第一帧首次 BindProgram 即命中该窗口；M3i 补的
   5 个绑定把录制调用数抬高约 35%，把这个一直存在的竞态窗口暴露了出来。改 __thread：每线程一份，零锁。 */
static __thread Cpu g_cpu;              /* 每线程一份（多线程录制安全） */
static uint64_t g_dropped;              /* 参数超限被丢弃的调用数 */
static uint64_t g_bad_cb;               /* cb 不是指针（0/极小值）被拒绝录制的调用数 */

static int put(Cpu *c, const uint64_t *argv, int argc)
{
    if (argc > 8) { g_dropped++; return 0; }
    /* 防御（同款崩溃的兜底）：cb 参数必须是宿主/客体地址。非指针一律不录制、计数——
       宁可少一条记录，也不让 cb_of(NULL) 解引用 0 把进程打死。 */
    if (argc < 1 || argv[0] <= 0x10000) { g_bad_cb++; return 0; }
    memset(c, 0, sizeof *c);
    for (int i = 0; i < argc; i++) c->x[i] = argv[i];
    return 1;
}

uint64_t nvn_rec_dropped_calls(void) { return g_dropped; }
uint64_t nvn_rec_bad_cb(void) { return g_bad_cb; }   /* M3i：cb 非指针被拒录制的次数（取证/对账） */

/* ---- task A/C（2026-11-02）：池式命令内存 chunk 表 + cmd_trace 的可读性查询 ----
 * 录制端 cmdbuf.c 把记录写进宿主侧：控制内存走 shadow 页块（cmdbuf.c nvn_port_cmdmem_readable
 * 覆盖），无控制内存时直接用池存储（set_chunk 的 mem 路径，cmdbuf.c:121
 * HOST(NvnPool,pool)->storage+off）。后者的宿主区间只有胶水看得见（pool_mirror->mem 是宿主指针），
 * 所以在 AddCommandMemory 处顺手记进全局小表——跟踪器每读一条记录前先问这里，越界即停。
 * 区间只增不减（池生命周期 = 列表生命周期，infra cmdbuf.c:150 注释在案）。 */
extern uint64_t nvn_port_cmdmem_readable(const void *p);   /* cmdbuf.c（shadow 页块 + 宿主 chunk） */
static struct { const void *base; uint64_t bytes; } g_pool_chunks[256];
static unsigned g_pool_chunks_n;

uint64_t nvn_rec_cmdmem_readable(const void *p)
{
    for (unsigned i = 0; i < g_pool_chunks_n; i++) {
        const uint8_t *lo = (const uint8_t *)g_pool_chunks[i].base, *hi = lo + g_pool_chunks[i].bytes;
        if ((const uint8_t *)p >= lo && (const uint8_t *)p < hi) return (uint64_t)(hi - (const uint8_t *)p);
    }
    return nvn_port_cmdmem_readable(p);      /* shadow 页 / add_host_memory chunk */
}

/* 十六进制字 dump 辅助（定义在下方 attrib/stream 取证段；vbuf 取证先用，故前向声明）。 */
static int v_words(char *b, size_t cap, int o, const uint32_t *w, int len);

/* ---- 建立/绑定：infra 侧在 cmdbuf.c:26 / :113 / :123 ----
 *
 * nvnCommandBufferInitialize(cb, device)：cb 是客体 NVNcommandBuffer*，cmdbuf.c:27-30 自己 calloc 一块
 * 宿主 NvnCmdBuf 并把指针写进客体偏移 0（SET_HOST），客体地址与宿主地址同值（本移植的既有前提，
 * gfx_bridge.c 全篇也是把客体指针直接当宿主指针用）。
 *
 * nvnCommandBufferAddCommandMemory(cb, const NVNmemoryPool *pool, ptrdiff_t off, size_t size)：
 * cmdbuf.c:121 用 infra 约定 `HOST(NvnPool, A(1))->storage`，即**客体池偏移 0 存宿主 NvnPool* **。
 * 我们树里的池对象是 gfx_bridge.c:224 的覆盖体 pool_h{magic,flags,mem,size}（客体内存就地存字段，
 * 偏移 0 不是指针），直接透传会被当野指针解引用。所以这里做「客体池 → 宿主 NvnPool」的翻译：
 * 用局部 NvnPool 承载 storage=pool->mem、size=pool->size，再传一个指向它的指针槽（cmdbuf 只读不存）。 */
typedef struct { uint32_t magic, flags; void *mem; uint64_t size; } pool_mirror;  /* = gfx_bridge.c:224 pool_h */

uint64_t nvn_rec_command_buffer_initialize(void *cb, void *device)
{
    uint64_t a[2] = {(uint64_t)(uintptr_t)cb, (uint64_t)(uintptr_t)device};
    if (!put(&g_cpu, a, 2)) return 0;
    hle_nvnCommandBufferInitialize(&g_cpu);
    return g_cpu.x[0];                  /* RET(1)（cmdbuf.c:30） */
}

uint64_t nvn_rec_command_buffer_add_command_memory(void *cb, const void *pool, int64_t off, uint64_t size)
{
    NvnPool p = {0};
    NvnPool *pp = &p;
    if (pool) {
        const pool_mirror *g = (const pool_mirror *)pool;
        p.storage = (uint64_t)(uintptr_t)g->mem;   /* 宿主指针；cmdbuf.c:121 只取 storage */
        p.size = g->size;
        p.flags = g->flags;
        /* chunk 表登记（task A/C）：池存储的宿主区间 [mem+off, +size)，供跟踪器先验边界。
           无控制内存时 cmdbuf 就把记录写进这段；登记总无害（它本就是游戏自己的命令内存）。 */
        if (g_pool_chunks_n < sizeof g_pool_chunks / sizeof g_pool_chunks[0] && (size_t)(off < 0 ? -off : off) <= g->size && size <= g->size - (size_t)(off < 0 ? -off : off)) {
            g_pool_chunks[g_pool_chunks_n].base = (const uint8_t *)g->mem + off;
            g_pool_chunks[g_pool_chunks_n].bytes = size;
            g_pool_chunks_n++;
        }
    }
    uint64_t a[4] = {(uint64_t)(uintptr_t)cb, (uint64_t)(uintptr_t)&pp, (uint64_t)off, size};
    if (!put(&g_cpu, a, 4)) return 0;
    hle_nvnCommandBufferAddCommandMemory(&g_cpu);
    return g_cpu.x[0];
}

uint64_t nvn_rec_command_buffer_add_control_memory(void *cb, void *mem, uint64_t size)
{
    uint64_t a[3] = {(uint64_t)(uintptr_t)cb, (uint64_t)(uintptr_t)mem, size};
    if (!put(&g_cpu, a, 3)) return 0;
    hle_nvnCommandBufferAddControlMemory(&g_cpu);
    return g_cpu.x[0];
}

/* ---- 录制族：参数顺序照 nvn_protos.h（真实 NVN ABI），逐项与 cmdbuf.c 的 A(i) 读取对齐 ---- */

void nvn_rec_begin_recording(void *cb)
{
    uint64_t a[1] = {(uint64_t)(uintptr_t)cb};
    if (put(&g_cpu, a, 1)) hle_nvnCommandBufferBeginRecording(&g_cpu);
}                                                              /* nvn_protos.h:343 */

uint64_t nvn_rec_end_recording(void *cb)
{
    uint64_t a[1] = {(uint64_t)(uintptr_t)cb};
    if (!put(&g_cpu, a, 1)) return 0;
    hle_nvnCommandBufferEndRecording(&g_cpu);
    return g_cpu.x[0];                  /* RET(cb->start)（cmdbuf.c:247）——提交侧拿到的命令流首条记录地址 */
}                                                              /* nvn_protos.h:344 */

/* ---- M4an（2026-11-04）：二级命令流接线（CallCommands / CopyCommands）----
   donor hle_nvn.c:1203-1214 的签名是 (cb, int n, const NVNcommandHandle *h)；cmdbuf.c 里两个 NVN()
   实现读 x[0]=cb、x[1]=n、x[2]=handles 表指针。这里只搬参数，inline 逻辑全在 cmdbuf.c（call_list）。 */
void nvn_rec_call_commands(void *cb, int n, const void *h)
{
    uint64_t a[3] = {(uint64_t)(uintptr_t)cb, (uint64_t)n, (uint64_t)(uintptr_t)h};
    if (put(&g_cpu, a, 3)) hle_nvnCommandBufferCallCommands(&g_cpu);
}

void nvn_rec_copy_commands(void *cb, int n, const void *h)
{
    uint64_t a[3] = {(uint64_t)(uintptr_t)cb, (uint64_t)n, (uint64_t)(uintptr_t)h};
    if (put(&g_cpu, a, 3)) hle_nvnCommandBufferCopyCommands(&g_cpu);
}

/* M17A（2026-11-08）：逐笔宿主点名。每条 draw 录制前记下客体调用点（LR）与帧指针（FP），
   由 cmdbuf.c 的 m17a_stash_lr 写进 CmdRec.f[0..3]；回放时 cmd_trace.c 抄到
   g_m17a_cur_lr/fp，nvn_vk 台账逐笔打印并用 eh_frame_hdr 反查宿主函数。
   为什么在胶水层取：draw 的录制走 nvn_rec_* → hle_nvnCommandBuffer*（cmdbuf.c），而
   put() 会 memset(Cpu)，c->x[30]/x[29] 恒 0（trace_draw 因此一直打 0）；客体 LR 只能从
   宿主帧链上找（同 gfx_bridge.c stub_caller_lr 的做法）。 */
uint64_t g_m17a_rec_lr, g_m17a_rec_fp;
extern uint64_t gfx_guest_caller_lr(void);
extern uint64_t gfx_guest_caller_fp_v;
static void m17a_mark_caller(void)
{
    g_m17a_rec_lr = gfx_guest_caller_lr();
    g_m17a_rec_fp = gfx_guest_caller_fp_v;
}

void nvn_rec_draw_arrays(void *cb, int mode, int first, int count)
{
    m17a_mark_caller();   /* M17A */
    uint64_t a[4] = {(uint64_t)(uintptr_t)cb, (uint64_t)mode, (uint64_t)first, (uint64_t)count};
    if (put(&g_cpu, a, 4)) hle_nvnCommandBufferDrawArrays(&g_cpu);   /* :377 */
}

/* M5p C（2026-11-05）：DrawArraysInstanced 整类接线——与 M4ad P2② 的 DrawElementsInstanced 同一缺口。
   实测 m5n：`[h] norec draws: arrays_inst=306237` 全部落在 gfx_bridge.c:3169 的 cb_account 记账桩上，
   连命令流都没进（[h] opcode 里根本没有这些笔），所以从未重放。
   cmdbuf.c:662 早已实现录制，且 record() 把 s.x+1 拷进 r->x ⇒ x[0]=mode,x[1]=first,x[2]=count,
   x[3]=baseInstance,x[4]=instances，正是 cmd_trace.c:328 回放分支读的布局。
   参数顺序照真实 NVN ABI（nvn_protos.h:380）：
     DrawArraysInstanced(cb, mode, first, count, baseInstance, instances) = 6 参。 */
void nvn_rec_draw_arrays_instanced(void *cb, int mode, int first, int count,
                                   int base_instance, int instances)
{
    m17a_mark_caller();   /* M17A */
    uint64_t a[6] = {(uint64_t)(uintptr_t)cb, (uint64_t)mode, (uint64_t)first, (uint64_t)count,
                     (uint64_t)(int64_t)base_instance, (uint64_t)(int64_t)instances};
    if (put(&g_cpu, a, 6)) hle_nvnCommandBufferDrawArraysInstanced(&g_cpu);   /* cmdbuf.c:662 */
}

void nvn_rec_draw_elements(void *cb, int mode, int type, int count, const void *idx)
{
    m17a_mark_caller();   /* M17A */
    /* :378 DrawElements(cb, mode, type, count, indexBuffer)：count 在第 4 参，
       cmdbuf.c:529 的 trace_draw 也是按 c->x[3] 读 count（x[0]=cb）。 */
    uint64_t a[5] = {(uint64_t)(uintptr_t)cb, (uint64_t)mode, (uint64_t)type,
                     (uint64_t)count, (uint64_t)(uintptr_t)idx};
    if (put(&g_cpu, a, 5)) hle_nvnCommandBufferDrawElements(&g_cpu);
}

/* ---- M4ad P2②：两个「只记账、从没进过命令流」的 draw 形态 ----
   实测（boot#1 `[h] norec draws`）：`DrawElementsBaseVertex` 21,729 次、`DrawElementsInstanced`
   13,626 次，占整 boot 绘制调用的 2/3——它们此前是纯 `cb_account` 桩，连记录都没有，
   所以在 opcode 直方图里完全不可见（零证据）。cmdbuf.c:592-598 其实**早就实现**了这两条的录制，
   缺的只是本层的参数搬运 + gfx_bridge 的表项（与 M3r 给单条绑定补搬运同一类缺口）。

   参数顺序照真实 NVN ABI（nvn_protos.h:370-375）：
     DrawElementsBaseVertex (cb, mode, type, count, indexBuffer, baseVertex)              = 6 参
     DrawElementsInstanced  (cb, mode, type, count, indexBuffer, baseVertex, baseInstance, instances) = 8 参
   put() 的上限正好是 8，instanced 这条是最后一根能放进 Cpu.x[] 的。 */
void nvn_rec_draw_elements_base_vertex(void *cb, int mode, int type, int count,
                                       const void *idx, int base_vertex)
{
    m17a_mark_caller();   /* M17A */
    uint64_t a[6] = {(uint64_t)(uintptr_t)cb, (uint64_t)mode, (uint64_t)type, (uint64_t)count,
                     (uint64_t)(uintptr_t)idx, (uint64_t)(int64_t)base_vertex};
    if (put(&g_cpu, a, 6)) hle_nvnCommandBufferDrawElementsBaseVertex(&g_cpu);   /* cmdbuf.c:592 */
}

void nvn_rec_draw_elements_instanced(void *cb, int mode, int type, int count,
                                     const void *idx, int base_vertex, int base_instance, int instances)
{
    m17a_mark_caller();   /* M17A */
    uint64_t a[8] = {(uint64_t)(uintptr_t)cb, (uint64_t)mode, (uint64_t)type, (uint64_t)count,
                     (uint64_t)(uintptr_t)idx, (uint64_t)(int64_t)base_vertex,
                     (uint64_t)(int64_t)base_instance, (uint64_t)(int64_t)instances};
    if (put(&g_cpu, a, 8)) hle_nvnCommandBufferDrawElementsInstanced(&g_cpu);    /* cmdbuf.c:598 */
}

void nvn_rec_set_render_targets(void *cb, int n, const void *const *colors,
                                const void *const *views, const void *depth, const void *depth_view)
{
    /* :434 SetRenderTargets(cb, n, textures, views, depthTexture, depthView)；
       cmdbuf.c:379-394 读 s.x[1..5]。 */
    uint64_t a[6] = {(uint64_t)(uintptr_t)cb, (uint64_t)n, (uint64_t)(uintptr_t)colors,
                     (uint64_t)(uintptr_t)views, (uint64_t)(uintptr_t)depth, (uint64_t)(uintptr_t)depth_view};
    if (put(&g_cpu, a, 6)) hle_nvnCommandBufferSetRenderTargets(&g_cpu);
}

void nvn_rec_clear_color(void *cb, int index, const float *color, int mask)
{
    /* :386 ClearColor(cb, int index, const float *color4, int colorMask)；
       cmdbuf.c:470 从 s.x[2]（= 客体 color4 指针）拷 16 字节，故 color 指针必须留在 x[2]。 */
    uint64_t a[4] = {(uint64_t)(uintptr_t)cb, (uint64_t)index, (uint64_t)(uintptr_t)color, (uint64_t)mask};
    if (put(&g_cpu, a, 4)) hle_nvnCommandBufferClearColor(&g_cpu);
}

void nvn_rec_bind_program(void *cb, void *program, int stage_mask)
{
    /* :355 BindProgram(cb, const NVNprogram *, int stageMask)；cmdbuf.c:278 取 s.x[1]。 */
    uint64_t a[3] = {(uint64_t)(uintptr_t)cb, (uint64_t)(uintptr_t)program, (uint64_t)stage_mask};
    if (put(&g_cpu, a, 3)) hle_nvnCommandBufferBindProgram(&g_cpu);
}

void nvn_rec_bind_vertex_buffers(void *cb, int first, int count, const void *ranges)
{
    /* :357 BindVertexBuffers(cb, first, count, const NVNbufferRange *)；
       cmdbuf.c:290 从 s.x[3] 拷 A(2)*16 字节。 */
    uint64_t a[4] = {(uint64_t)(uintptr_t)cb, (uint64_t)first, (uint64_t)count, (uint64_t)(uintptr_t)ranges};
    if (put(&g_cpu, a, 4)) hle_nvnCommandBufferBindVertexBuffers(&g_cpu);
}

/* M3r（2026-11-03）：三个「单条绑定」入口此前只有记账桩、不进命令流，而实测命令流里
   BIND_VERTEX_BUFFERS / BIND_UNIFORM_BUFFERS / BIND_TEXTURES 一条都没有——SMO 用的是单条形式
   （protos:355 BindVertexBuffer(cb, int first, NVNbufferAddress, size_t)）。cmdbuf.c 早在
   :330-343 就实现了这三条的单条录制（extra=16B {addr,size} / 8B handle），这里只补参数搬运。 */
void nvn_rec_bind_vertex_buffer(void *cb, int first, uint64_t addr, uint64_t size)
{
    /* M3s 取证：前 8 条真实 vbuf 绑定，含地址/长度与前 8 个字（真实顶点数据是否存在、步长线索）。 */
    static unsigned g_log_vb;
    if (g_log_vb < 8) {
        g_log_vb++;
        char line[256];
        int o = snprintf(line, sizeof line, "[v] vbuf_raw #%u first=%d addr=%#llx size=%llu |",
                         g_log_vb, first, (unsigned long long)addr, (unsigned long long)size);
        if (addr > 0x10000 && size >= 4) {
            uint32_t w[8]; uint64_t n = size / 4; if (n > 8) n = 8;
            memcpy(w, (const void *)(uintptr_t)addr, (size_t)(n * 4));
            o = v_words(line, sizeof line, o, w, (int)n);
        } else o += snprintf(line + o, sizeof line - (size_t)o, " <unreadable>");
        vk_boot_logf("%s", line);
    }
    uint64_t a[4] = {(uint64_t)(uintptr_t)cb, (uint64_t)first, addr, size};
    if (put(&g_cpu, a, 4)) hle_nvnCommandBufferBindVertexBuffer(&g_cpu);   /* cmdbuf.c:330 */
}

void nvn_rec_bind_uniform_buffer(void *cb, int stage, int index, uint64_t addr, uint64_t size)
{
    uint64_t a[5] = {(uint64_t)(uintptr_t)cb, (uint64_t)stage, (uint64_t)index, addr, size};
    if (put(&g_cpu, a, 5)) hle_nvnCommandBufferBindUniformBuffer(&g_cpu);  /* cmdbuf.c:338 */
}

void nvn_rec_bind_texture(void *cb, int stage, int slot, uint64_t handle)
{
    uint64_t a[4] = {(uint64_t)(uintptr_t)cb, (uint64_t)stage, (uint64_t)slot, handle};
    if (put(&g_cpu, a, 4)) hle_nvnCommandBufferBindTexture(&g_cpu);        /* cmdbuf.c:356 */
}

void nvn_rec_bind_textures(void *cb, int stage, int first, int count, const void *handles)
{
    /* :365 BindTextures(cb, stage, first, count, const NVNtextureHandle *)；
       cmdbuf.c:313 从 s.x[4] 拷 A(3)*8 字节。 */
    uint64_t a[5] = {(uint64_t)(uintptr_t)cb, (uint64_t)stage, (uint64_t)first,
                     (uint64_t)count, (uint64_t)(uintptr_t)handles};
    if (put(&g_cpu, a, 5)) hle_nvnCommandBufferBindTextures(&g_cpu);
}

void nvn_rec_bind_uniform_buffers(void *cb, int stage, int first, int count, const void *ranges)
{
    /* :359 BindUniformBuffers(cb, stage, first, count, const NVNbufferRange *)；
       cmdbuf.c:298 从 s.x[4] 拷 A(3)*16 字节。 */
    uint64_t a[5] = {(uint64_t)(uintptr_t)cb, (uint64_t)stage, (uint64_t)first,
                     (uint64_t)count, (uint64_t)(uintptr_t)ranges};
    if (put(&g_cpu, a, 5)) hle_nvnCommandBufferBindUniformBuffers(&g_cpu);
}

/* M5x P1：storage buffer 绑定进命令流（gmem 的地址唯一来源；此前整条被 HLE 表指向本地空实现）。
   nvn_protos.h:362/363：单条 (cb, stage, index, addr, size)；数组 (cb, stage, first, count, ranges)。 */
void nvn_rec_bind_storage_buffer(void *cb, int stage, int index, uint64_t addr, uint64_t size)
{
    uint64_t a[5] = {(uint64_t)(uintptr_t)cb, (uint64_t)stage, (uint64_t)index,
                     (uint64_t)addr, (uint64_t)size};
    if (put(&g_cpu, a, 5)) hle_nvnCommandBufferBindStorageBuffer(&g_cpu);
}

void nvn_rec_bind_storage_buffers(void *cb, int stage, int first, int count, const void *ranges)
{
    uint64_t a[5] = {(uint64_t)(uintptr_t)cb, (uint64_t)stage, (uint64_t)first,
                     (uint64_t)count, (uint64_t)(uintptr_t)ranges};
    if (put(&g_cpu, a, 5)) hle_nvnCommandBufferBindStorageBuffers(&g_cpu);
}

void nvn_rec_set_viewports(void *cb, int first, int count, const float *rects)
{
    uint64_t a[4] = {(uint64_t)(uintptr_t)cb, (uint64_t)first, (uint64_t)count, (uint64_t)(uintptr_t)rects};
    if (put(&g_cpu, a, 4)) hle_nvnCommandBufferSetViewports(&g_cpu);   /* :393，cmdbuf.c:397 */
}

void nvn_rec_set_scissors(void *cb, int first, int count, const int *rects)
{
    uint64_t a[4] = {(uint64_t)(uintptr_t)cb, (uint64_t)first, (uint64_t)count, (uint64_t)(uintptr_t)rects};
    if (put(&g_cpu, a, 4)) hle_nvnCommandBufferSetScissors(&g_cpu);    /* :396，cmdbuf.c:405 */
}

/* ---- M4an P2（2026-11-04）：四个「单数拼写」状态设置接上录制 ----
   cmdbuf.c:454/462/470/476 已有实现（与复数名同一组 opcode），这里只做参数搬运：
     SetViewport   (cb, int x, int y, int w, int h)  → x[1..4]
     SetScissor    (cb, int x, int y, int w, int h)  → x[1..4]
     SetDepthRange (cb, float near, float far)       → snap() 读 c->v[0..1]，**必须写 v[]**（put 会把 v 清零）
     SetBlendColor (cb, const float *rgba)           → x[1] 指针，record_copy 拷 16B
   donor 语义核对：hle_nvn.c:1723 SetViewport 就是把 (x,y,w,h) 当 float 矩形交给 SetViewports(0,1)。 */
void nvn_rec_set_viewport(void *cb, int x, int y, int w, int h)
{
    uint64_t a[5] = {(uint64_t)(uintptr_t)cb, (uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h};
    if (put(&g_cpu, a, 5)) hle_nvnCommandBufferSetViewport(&g_cpu);
}

void nvn_rec_set_scissor(void *cb, int x, int y, int w, int h)
{
    uint64_t a[5] = {(uint64_t)(uintptr_t)cb, (uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h};
    if (put(&g_cpu, a, 5)) hle_nvnCommandBufferSetScissor(&g_cpu);
}

void nvn_rec_set_depth_range(void *cb, float n, float f)
{
    uint64_t a[3] = {(uint64_t)(uintptr_t)cb, 0, 0};
    if (!put(&g_cpu, a, 3)) return;
    uint32_t bn, bf;
    memcpy(&bn, &n, 4); memcpy(&bf, &f, 4);
    g_cpu.v[0] = bn; g_cpu.v[1] = bf;   /* put() 已 memset 整个 Cpu：float 参数必须在 put 之后写 */
    hle_nvnCommandBufferSetDepthRange(&g_cpu);
}

void nvn_rec_set_blend_color(void *cb, const float *rgba)
{
    uint64_t a[2] = {(uint64_t)(uintptr_t)cb, (uint64_t)(uintptr_t)rgba};
    if (put(&g_cpu, a, 2)) hle_nvnCommandBufferSetBlendColor(&g_cpu);
}

/* ---- M3i（2026-11-03）：5 个「只记账、不进命令流」的状态绑定补录制 ----
   录制端 cmdbuf.c:316-326 早就有实现（record_copy 自带负载拷贝），缺的只是本层参数搬运 + gfx_bridge
   表项。参数顺序照 nvn_protos.h:347-354；cmdbuf.c 的读取点：
     BindBlendState        record_copy(CMD_BIND_BLEND,        1, 8)   → 状态指针必须在 x[1]
     BindColorState        record_copy(CMD_BIND_COLOR,        1, 4)   → x[1]
     BindDepthStencilState record_copy(CMD_BIND_DEPTH_STENCIL,1, 8)   → x[1]
     BindVertexAttribState record_copy(CMD_BIND_VERTEX_ATTRIB,2, A(1)*4) → x[1]=count, x[2]=数组指针
     BindVertexStreamState record_copy(CMD_BIND_VERTEX_STREAM,2, A(1)*8) → x[1]=count, x[2]=数组指针
   注意 count 也必须在 x[1]：录制侧用 A(1) 算拷贝长度，不是装饰参数。 */
void nvn_rec_bind_blend_state(void *cb, const void *state)
{
    uint64_t a[2] = {(uint64_t)(uintptr_t)cb, (uint64_t)(uintptr_t)state};
    if (put(&g_cpu, a, 2)) hle_nvnCommandBufferBindBlendState(&g_cpu);
}

void nvn_rec_bind_color_state(void *cb, const void *state)
{
    uint64_t a[2] = {(uint64_t)(uintptr_t)cb, (uint64_t)(uintptr_t)state};
    if (put(&g_cpu, a, 2)) hle_nvnCommandBufferBindColorState(&g_cpu);
}

void nvn_rec_bind_depth_stencil_state(void *cb, const void *state)
{
    uint64_t a[2] = {(uint64_t)(uintptr_t)cb, (uint64_t)(uintptr_t)state};
    if (put(&g_cpu, a, 2)) hle_nvnCommandBufferBindDepthStencilState(&g_cpu);
}

/* M3s 取证：录制侧（=游戏真实传进来的值）前 20 次绑定各打一行；与回放侧 nvn_vk_set_vertex_* 的
   [r] state 行对照，就能判定「录进来就是零」还是「回放丢了」。
   M3s（2026-11-03）追加：数组内容逐字 dump + 首调用看 p-8..p+88 窗口（判断数组是否整体偏移），
   并把「全量 calls/zero/nonzero/big」计数暴露给 gfx_bridge 的周期报告。
   原始 ABI 四参（cb/n/p/a3）与调用点 LR 在 gfx_bridge 的 nvn_cb_rec_* 里打印（那里 level-0 返回地址
   才是游戏调用点；本层再取一级需要 frame pointer，安全起见不跨帧）。 */
static unsigned g_log_va, g_log_vs;
static unsigned long long g_va_calls, g_va_zero, g_va_nz, g_va_big;
static unsigned long long g_vs_calls, g_vs_zero, g_vs_nz, g_vs_big;
unsigned long long nvn_rec_nonzero_attrib(void){return g_va_nz;}
unsigned long long nvn_rec_nonzero_stream(void){return g_vs_nz;}
void nvn_rec_vertex_totals(unsigned long long *ac,unsigned long long *az,unsigned long long *an,
                           unsigned long long *sc,unsigned long long *sz,unsigned long long *sn){
    if(ac)*ac=g_va_calls; if(az)*az=g_va_zero; if(an)*an=g_va_nz;
    if(sc)*sc=g_vs_calls; if(sz)*sz=g_vs_zero; if(sn)*sn=g_vs_nz;
}

/* 把 len 个字拼成 " xxxxxxxx ..."（调用方保证 len 不越过数组）；返回写入后的光标。 */
static int v_words(char *b,size_t cap,int o,const uint32_t *w,int len)
{
    for(int i=0;i<len && o+10<(int)cap;i++) o+=snprintf(b+o,cap-(size_t)o," %08x",w[i]);
    return o;
}

void nvn_rec_bind_vertex_attrib_state(void *cb, int count, const void *states)
{
    g_va_calls++;
    if (count > 16) g_va_big++;
    if (states && count > 0 && count <= 64) {   /* 上限提到 64：count>16 也要参与非零判定（M3r 口径漏了这类） */
        const uint32_t *w = states;
        int nz = 0;
        for (int i = 0; i < count; i++) if (w[i]) nz = 1;
        if (nz) {
            g_va_nz++;
            static unsigned nzlog;
            if (nzlog++ < 3) {   /* 首个非零绑定：判定「修好没有」的直接证据 */
                char line[512];
                int o = snprintf(line, sizeof line, "[v] attrib NONZERO #%u n=%d |", nzlog, count);
                o = v_words(line, sizeof line, o, w, count > 40 ? 40 : count);
                vk_boot_logf("%s", line);
            }
        } else g_va_zero++;
    }
    if (g_log_va < 20 && states) {
        const uint32_t *v = states;
        g_log_va++;
        char line[512];
        int o = snprintf(line, sizeof line, "[v] attrib_words #%u n=%d |", g_log_va, count);
        o = v_words(line, sizeof line, o, v, count > 0 ? (count > 40 ? 40 : count) : 0);
        vk_boot_logf("%s", line);
        if (g_log_va == 1) {   /* 首调用：p-8..p+88 窗口（含 p-8，判断是否整体差一个 slot） */
            o = snprintf(line, sizeof line, "[v] attrib_win p-8:");
            o = v_words(line, sizeof line, o, v - 2, 25);
            vk_boot_logf("%s", line);
        }
    }
    uint64_t a[3] = {(uint64_t)(uintptr_t)cb, (uint64_t)count, (uint64_t)(uintptr_t)states};
    if (put(&g_cpu, a, 3)) hle_nvnCommandBufferBindVertexAttribState(&g_cpu);
}

void nvn_rec_bind_vertex_stream_state(void *cb, int count, const void *states)
{
    g_vs_calls++;
    if (count > 16) g_vs_big++;
    if (states && count > 0 && count <= 64) {
        const uint32_t *w = states;
        int nz = 0;
        for (int i = 0; i < count * 2; i++) if (w[i]) nz = 1;
        if (nz) {
            g_vs_nz++;
            static unsigned nzlog2;
            if (nzlog2++ < 3) {   /* 首个非零 stream：stride/divisor 是否真的来了 */
                char line[256];
                int o = snprintf(line, sizeof line, "[v] stream NONZERO #%u n=%d |", nzlog2, count);
                o = v_words(line, sizeof line, o, w, count > 20 ? 20 : count * 2);
                vk_boot_logf("%s", line);
            }
        } else g_vs_zero++;
    }
    if (g_log_vs < 20 && states) {
        const uint32_t *v = states;
        g_log_vs++;
        char line[512];
        int o = snprintf(line, sizeof line, "[v] stream_words #%u n=%d |", g_log_vs, count);
        o = v_words(line, sizeof line, o, v, count > 0 ? (count > 20 ? 20 : count * 2) : 0);
        vk_boot_logf("%s", line);
    }
    uint64_t a[3] = {(uint64_t)(uintptr_t)cb, (uint64_t)count, (uint64_t)(uintptr_t)states};
    if (put(&g_cpu, a, 3)) hle_nvnCommandBufferBindVertexStreamState(&g_cpu);
}

/* ---- 记账/生命周期（第 2 步）：让「客体偏移 0 = 宿主 NvnCmdBuf*」成为唯一事实 ----
 * 此前这族留在 gfx_bridge 的 cmd_h 覆盖体上（读 guest+0x60/0x88），和 cmdbuf 的间接层并存 = 两套账。
 * 现在统一取 cmdbuf 的真值：用量是「游戏字节」（cmdbuf.c:120/196 记账，Get* 在 :134-142 换算），
 * 游戏正是累加 GetCommandMemoryUsed 定 RSTB 堆尺寸（gfx_bridge.c:616 注释）。 */
void nvn_rec_command_buffer_finalize(void *cb)
{
    uint64_t a[1] = {(uint64_t)(uintptr_t)cb};
    if (put(&g_cpu, a, 1)) hle_nvnCommandBufferFinalize(&g_cpu);   /* cmdbuf.c:32：free + 客体偏移0清 0 */
}

uint64_t nvn_rec_get_command_memory_size(void *cb)
{
    uint64_t a[1] = {(uint64_t)(uintptr_t)cb};
    if (!put(&g_cpu, a, 1)) return 0;
    hle_nvnCommandBufferGetCommandMemorySize(&g_cpu);   /* cmdbuf.c:134 */
    return g_cpu.x[0];
}

uint64_t nvn_rec_get_command_memory_used(void *cb)
{
    uint64_t a[1] = {(uint64_t)(uintptr_t)cb};
    if (!put(&g_cpu, a, 1)) return 0;
    hle_nvnCommandBufferGetCommandMemoryUsed(&g_cpu);   /* cmdbuf.c:135 */
    return g_cpu.x[0];
}

uint64_t nvn_rec_get_command_memory_free(void *cb)
{
    uint64_t a[1] = {(uint64_t)(uintptr_t)cb};
    if (!put(&g_cpu, a, 1)) return 0;
    hle_nvnCommandBufferGetCommandMemoryFree(&g_cpu);   /* cmdbuf.c:136-139 */
    return g_cpu.x[0];
}

uint64_t nvn_rec_get_control_memory_size(void *cb)
{
    uint64_t a[1] = {(uint64_t)(uintptr_t)cb};
    if (!put(&g_cpu, a, 1)) return 0;
    hle_nvnCommandBufferGetControlMemorySize(&g_cpu);   /* cmdbuf.c:140 */
    return g_cpu.x[0];
}

uint64_t nvn_rec_get_control_memory_used(void *cb)
{
    uint64_t a[1] = {(uint64_t)(uintptr_t)cb};
    if (!put(&g_cpu, a, 1)) return 0;
    hle_nvnCommandBufferGetControlMemoryUsed(&g_cpu);   /* cmdbuf.c:141 */
    return g_cpu.x[0];
}

uint64_t nvn_rec_get_control_memory_free(void *cb)
{
    uint64_t a[1] = {(uint64_t)(uintptr_t)cb};
    if (!put(&g_cpu, a, 1)) return 0;
    hle_nvnCommandBufferGetControlMemoryFree(&g_cpu);   /* cmdbuf.c:142 */
    return g_cpu.x[0];
}

uint64_t nvn_rec_is_recording(void *cb)
{
    uint64_t a[1] = {(uint64_t)(uintptr_t)cb};
    if (!put(&g_cpu, a, 1)) return 0;
    hle_nvnCommandBufferIsRecording(&g_cpu);            /* cmdbuf.c:249 */
    return g_cpu.x[0];
}
