/* 命令流跟踪器：把 cmdbuf.c 录下来的 CmdRec 列表走一遍，统计 opcode 直方图。
 *
 * 目的：在不具备 Vulkan 驱动的模拟器上，先回答「SMO 实际发哪些 opcode、比例多少」，
 * 从而把周末要实现的最小回放集从「猜」变成「按实测裁剪」。
 *
 * 契约来自 nvn.h（录制端与回放端共用同一头文件，格式不存在不匹配问题）：
 *   CmdRec { uint32_t op, size; uint64_t x[7]; float f[4]; uint8_t extra[]; }
 *   CMD_END=0 … CMD_COUNT，共 62 个（nvn.h:117-141）
 *
 * 2026-11-02 task A/B/C：
 *   A 跨 chunk —— 走流语义照 infra cmdbuf.c 的 nvn_check_list / copy_list（:679-712）：列表不连续，
 *     chunk 之间由 CMD_JUMP 链接（cmdbuf.c:88/188），JUMP.x[0] 是下一段首条记录的宿主地址，
 *     顺着它走即按命令内存的真实分块走完，不自己发明分块规则。
 *   B 累计 —— nvn_cmd_trace_merge 供调用方把每个 submit 的结果累加成一份直方图。
 *   C 边界安全 —— 每读一条记录（含 JUMP 目标落地处）先问 readable(p)（cmdbuf.c
 *     nvn_port_cmdmem_readable：shadow 页块 / 宿主 chunk；glue_cmdbuf.c 补池存储区间），
 *     可读长度不足即停并计 truncated。size 字段只用来在同一可读区域内前进，且上限 1 MiB。
 *
 * 依赖：nvn.h（已在本目录）、compat.h（HLE_FUNC / Cpu / 日志宏）。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvn.h"
#include "cmd_trace.h"   /* CmdTrace 契约（与 platform/gfx_bridge.c 共用，防布局漂移） */

/* 防漂移：cmd_trace.h:10 给 gfx_bridge 用的 CLEAR_COLOR 号必须与 nvn.h 枚举一致。 */
_Static_assert(NVN_CMD_OP_CLEAR_COLOR == CMD_CLEAR_COLOR, "cmd_trace.h CLEAR_COLOR opcode drifted from nvn.h");
/* M3i：SET_RENDER_TARGETS 同款防漂移（cmdbuf.c 录制端与回放端必须同号）。 */
_Static_assert(NVN_CMD_OP_SET_RENDER_TARGETS == CMD_SET_RENDER_TARGETS, "cmd_trace.h SET_RENDER_TARGETS opcode drifted from nvn.h");
/* M4an：CALL 同款防漂移。 */
_Static_assert(NVN_CMD_OP_CALL == CMD_CALL, "cmd_trace.h CALL opcode drifted from nvn.h");

static const char *const kOpNames[CMD_TRACE_MAX_OPS] = {
    "END", "JUMP", "CALL",
    "BIND_PROGRAM", "BIND_BLEND", "BIND_CHANNEL_MASK", "BIND_COLOR", "BIND_DEPTH_STENCIL",
    "BIND_MULTISAMPLE", "BIND_POLYGON", "BIND_VERTEX_ATTRIB", "BIND_VERTEX_STREAM",
    "BIND_VERTEX_BUFFERS", "BIND_UNIFORM_BUFFERS", "BIND_STORAGE_BUFFERS", "BIND_TEXTURES",
    "BIND_IMAGES", "SET_TEXTURE_POOL", "SET_SAMPLER_POOL", "SET_RENDER_TARGETS",
    "SET_VIEWPORTS", "SET_SCISSORS", "SET_DEPTH_RANGES", "SET_BLEND_COLOR", "SET_STENCIL",
    "SET_PRIMITIVE_RESTART", "SET_ALPHA_REF", "SET_DEPTH_BOUNDS", "SET_POLYGON_OFFSET",
    "SET_LINE_WIDTH", "SET_POINT_SIZE", "SET_SAMPLE_MASK", "SET_RENDER_ENABLE",
    "CLEAR_COLOR", "CLEAR_DEPTH_STENCIL", "CLEAR_TEXTURE", "CLEAR_BUFFER",
    "DRAW_ARRAYS", "DRAW_ELEMENTS", "DRAW_ARRAYS_INDIRECT", "DRAW_ELEMENTS_INDIRECT",
    "DISPATCH", "DISPATCH_INDIRECT",
    "COPY_BUFFER_TO_BUFFER", "COPY_BUFFER_TO_TEXTURE", "COPY_TEXTURE_TO_BUFFER",
    "COPY_TEXTURE_TO_TEXTURE", "DOWNSAMPLE", "UPDATE_UNIFORM_BUFFER", "FENCE_SYNC", "WAIT_SYNC",
    "BARRIER", "SIGNAL_EVENT", "WAIT_EVENT", "REPORT_COUNTER", "DISCARD_COLOR",
    "DISCARD_DEPTH_STENCIL", "SET_COPY_STRIDES", "DRAW_TEXTURE", "RESOLVE_DEPTH", "SET_DEPTH_CLAMP",
    "BIND_TEXTURES_DEFERRED",
};

/* 记录级合法性：与 infra nvn_check_list（cmdbuf.c:678）同一判据——
   op 越界，或非 END/JUMP 记录的 size 不在 [sizeof(CmdRec), 1MiB]。 */
/* M17A（2026-11-08）：逐笔宿主点名。draw 记录把客体 LR/FP 藏在 CmdRec.f[0..3]（cmdbuf.c 录制端），
   这里在调 sink 之前抄出来；nvn_vk 的 draw 台账在 sink 内被调，读这两个全局即得"这一笔是哪个客体
   函数录的"。**只读**，不改回放语义（draw 的 f[] 本来就没有消费者）。 */
uint64_t g_m17a_cur_lr, g_m17a_cur_fp;   /* 当前正在回放的 draw 记录的录制者 */
static void m17a_take_lr(const CmdRec *r)
{
    uint64_t a = 0, b = 0;
    memcpy(&a, &r->f[0], 8);
    memcpy(&b, &r->f[2], 8);
    g_m17a_cur_lr = a; g_m17a_cur_fp = b;
}
static int rec_bad(const CmdRec *r)
{
    return r->op >= CMD_COUNT ||
           (r->op != CMD_END && r->op != CMD_JUMP &&
            (r->size < sizeof(CmdRec) || r->size > 0x100000));
}

/* task A+C：跨 chunk 走完整条命令流。语义照 infra copy_list/nvn_check_list：
   顺着 CMD_JUMP 走到 CMD_END；每一步（含跳转目标）先经 readable 验边界。 */
int nvn_cmd_trace_walk(const void *start, nvn_cmd_readable_fn readable, CmdTrace *out)
{
    memset(out, 0, sizeof *out);
    out->stop_why = "未开始";
    if (!readable) { out->stop_why = "无可读性查询"; out->truncated++; return 0; }
    const uint8_t *p = (const uint8_t *)start;
    uint64_t avail = 0;                       /* 从 p 起在当前区域还能安全读多少（0 = 待查） */
    for (uint64_t steps = 0, jumps = 0; steps < (1u << 20); steps++) {
        if (avail < sizeof(CmdRec)) {         /* 换了区域，或首次：先验边界再碰内存 */
            avail = readable(p);
            if (avail < sizeof(CmdRec)) {
                out->stop_why = "被边界截断（记录头落在未知/耗尽区域）";
                out->truncated++; out->first_bad = out->total; out->first_bad_why = out->stop_why;
                return 0;
            }
        }
        const CmdRec *r = (const CmdRec *)p;
        if (rec_bad(r)) {
            out->stop_why = "非法记录（op 越界或 size 非法）";
            out->bad++; out->first_bad = out->total; out->first_bad_why = out->stop_why;
            return 0;
        }
        if (r->op == CMD_END) { out->total++; out->per_op[CMD_END]++; out->bytes += sizeof(CmdRec); out->stop_why = "END"; return 1; }
        if (r->op == CMD_JUMP) {
            if (r->x[0] <= 0x10000) { out->stop_why = "JUMP 目标非宿主地址"; out->bad++; out->first_bad = out->total; out->first_bad_why = out->stop_why; return 0; }
            if (++jumps > 4096) { out->stop_why = "JUMP 过多（疑似环）"; out->truncated++; out->first_bad = out->total; out->first_bad_why = out->stop_why; return 0; }
            out->jumps++; out->bytes += sizeof(CmdRec);
            p = (const uint8_t *)(uintptr_t)r->x[0];   /* 目标的边界下一轮头部先验 */
            avail = 0;
            continue;
        }
        if ((r->size & 7) || (uint64_t)r->size > avail) {  /* size 只在已验区域内前进 */
            out->stop_why = "被边界截断（记录体超出区域）";
            out->truncated++; out->first_bad = out->total; out->first_bad_why = out->stop_why;
            return 0;
        }
        out->total++;
        out->per_op[r->op]++;
        out->bytes += r->size;
        p += r->size;
        avail -= r->size;
    }
    out->stop_why = "步数守卫（1Mi 条）";
    out->truncated++;
    return 0;
}

/* M3h（2026-11-03）：执行式走流。与上面的 walk 逐行同构（同一 rec_bad/边界/跳转/守卫），
   唯一增加：CMD_CLEAR_COLOR 交给 clear_cb 执行；其余 opcode 只记进 per_op。
   M3i（2026-11-03）：CMD_SET_RENDER_TARGETS 追加交 rt_cb（负载口径见下方 nvn_cmd_exec 注释）。
   CLEAR_COLOR 的负载布局来自录制侧 cmdbuf.c:509 record_copy(c,CMD_CLEAR_COLOR,2,16)：
   record() 把 s.x+1 拷进 r->x ⇒ x[0]=颜色槽 index、x[1]=客体 color4 指针、x[2]=mask，
   16 字节颜色在 r->extra（record_copy 从 x[1] 指的那块内存拷）。 */
const char *nvn_cmd_op_name(int op)
{
    return (op >= 0 && op < CMD_TRACE_MAX_OPS) ? kOpNames[op] : NULL;
}

/* M3i（2026-11-03）：SET_RENDER_TARGETS 也交回调执行；负载布局来自录制侧 cmdbuf.c:413-420：
   record() 把 s.x+1 拷进 r->x ⇒ x[0]=n（录制端随后显式重写为 n），extra = RtRec
   { u64 tex[8]; NvnTextureView views[8]; u8 has_view[8]; u64 depth; NvnTextureView dview; u8 has_dview }。
   本层只把 n 与前 8 字节（tex[0]，NVN_CMD_RT_FIRST_TEX_OFF）交给回调，其余结构不解读。

   M3r（2026-11-03）：本函数变成 nvn_cmd_exec_sink 的薄包装（只挂 clear+rt 两个回调），
   走流语义/边界/跳转逻辑只在 sink 版里有一份，不会再分叉。 */
/* M3s（2026-11-03）：首 draw 前的 opcode 取证。
   目的：如果 attrib/stream 前 20 次全是零，需要确认「这条路径上到底有没有非零顶点状态」——
   把首笔 DRAW_ELEMENTS 之前的记录按顺序打出来（每段流的前 64 条 + 全局最近 8 条），
   含每条的前 4 个 word。只输出一次；出口走 sink->note（gfx_bridge 转发到 vk_boot.log）。 */
typedef struct { uint32_t op; uint64_t x[4]; } M3sRec;
static M3sRec g_m3s_ring[8];
static int g_m3s_ring_i;
static int g_m3s_dumped;

static void m3s_note1(const nvn_cmd_sink *sink, const char *line)
{
    if (sink->note) sink->note(line, sink->user);
}

static void m3s_dump(const nvn_cmd_sink *sink, const M3sRec *first, int first_n)
{
    char b[192];
    snprintf(b, sizeof b, "[v] pre_draw dump (first draw reached): stream_first=%d", first_n);
    m3s_note1(sink, b);
    for (int i = 0; i < first_n; i++) {
        const char *n = nvn_cmd_op_name((int)first[i].op);
        snprintf(b, sizeof b, "[v] pre_draw first#%02d %s x=%#llx,%#llx,%#llx,%#llx", i,
                 n ? n : "?", (unsigned long long)first[i].x[0], (unsigned long long)first[i].x[1],
                 (unsigned long long)first[i].x[2], (unsigned long long)first[i].x[3]);
        m3s_note1(sink, b);
    }
    for (int k = 0; k < 8; k++) {
        const M3sRec *r = &g_m3s_ring[(g_m3s_ring_i - 8 + k) & 7];
        const char *n = nvn_cmd_op_name((int)r->op);
        snprintf(b, sizeof b, "[v] pre_draw prev-%d %s x=%#llx,%#llx,%#llx,%#llx", 8 - k,
                 n ? n : "?", (unsigned long long)r->x[0], (unsigned long long)r->x[1],
                 (unsigned long long)r->x[2], (unsigned long long)r->x[3]);
        m3s_note1(sink, b);
    }
}

int nvn_cmd_exec_sink(const void *start, nvn_cmd_readable_fn readable,
                      const nvn_cmd_sink *sink, CmdExecStat *out)
{
    /* M5c：两个弱钩子（没定义也能跑，本文件不依赖 runtime/）：
       ① runtime_cmd_step_note_(steps)：每 256 条记一次阶段环（op=34）——冻结时能读出「走流走到第几条」；
       ② runtime_rss_guard()：内存护栏，触发即停走流（本体在 nvn_vk.c，只放弃本笔工作，绝不 abort 进程）。 */
    extern void runtime_cmd_step_note_(unsigned long long steps) __attribute__((weak));
    extern int  runtime_rss_guard(void) __attribute__((weak));
    memset(out, 0, sizeof *out);
    out->trace.stop_why = "未开始";
    if (!readable) { out->trace.stop_why = "无可读性查询"; out->trace.truncated++; return 0; }
    const uint8_t *p = (const uint8_t *)start;
    uint64_t avail = 0;
    /* M4ao P1：CALL inline 体的边界栈。录制端在每个 handle 的体前插一条 CMD_CALL 标记，
       并把「该体拷了多少条记录」写进 x[1]（cmdbuf.c call_commands）。这里按记录数把体耗完即弹栈，
       于是每条 draw 都能判「它是不是 M4an 新接进来的那部分（in-call）」——这是把
       55.7k 新增 draw 的拒因拆开的唯一依据。嵌套 call 用栈（体里可能还有标记），深度 8 足够。 */
    #define CALL_STK 8
    uint64_t call_rem[CALL_STK];
    int call_depth = 0;
    M3sRec first[64];
    int first_n = 0;
    if (!g_m3s_dumped) { memset(g_m3s_ring, 0, sizeof g_m3s_ring); g_m3s_ring_i = 0; }
    for (uint64_t steps = 0, jumps = 0; steps < (1u << 20); steps++) {
        /* M5q A'：轮询从每 256 条加密到每 64 条（派活单 P1）——越过 trip 后当前这笔重活要尽快退让，
           把 overshoot 从 23 MB 压下去。r_rss_mb() 自带 100 ms 缓存，读取代价不变，变的只是判定延迟。 */
        if ((steps & 63u) == 0) {
            if (runtime_cmd_step_note_) runtime_cmd_step_note_(steps);
            if (runtime_rss_guard && runtime_rss_guard()) {
                out->trace.stop_why = "RSS 护栏（M5c）"; out->trace.truncated++;
                out->trace.first_bad = out->trace.total; out->trace.first_bad_why = out->trace.stop_why;
                return 0;
            }
        }
        if (avail < sizeof(CmdRec)) {
            avail = readable(p);
            if (avail < sizeof(CmdRec)) {
                out->trace.stop_why = "被边界截断（记录头落在未知/耗尽区域）";
                out->trace.truncated++; out->trace.first_bad = out->trace.total; out->trace.first_bad_why = out->trace.stop_why;
                return 0;
            }
        }
        const CmdRec *r = (const CmdRec *)p;
        if (rec_bad(r)) {
            out->trace.stop_why = "非法记录（op 越界或 size 非法）";
            out->trace.bad++; out->trace.first_bad = out->trace.total; out->trace.first_bad_why = out->trace.stop_why;
            return 0;
        }
        if (r->op == CMD_END) { out->trace.total++; out->trace.per_op[CMD_END]++; out->trace.bytes += sizeof(CmdRec); out->trace.stop_why = "END"; return 1; }
        if (r->op == CMD_JUMP) {
            if (r->x[0] <= 0x10000) { out->trace.stop_why = "JUMP 目标非宿主地址"; out->trace.bad++; out->trace.first_bad = out->trace.total; out->trace.first_bad_why = out->trace.stop_why; return 0; }
            if (++jumps > 4096) { out->trace.stop_why = "JUMP 过多（疑似环）"; out->trace.truncated++; out->trace.first_bad = out->trace.total; out->trace.first_bad_why = out->trace.stop_why; return 0; }
            out->trace.jumps++; out->trace.bytes += sizeof(CmdRec);
            p = (const uint8_t *)(uintptr_t)r->x[0];
            avail = 0;
            continue;
        }
        if ((r->size & 7) || (uint64_t)r->size > avail) {
            out->trace.stop_why = "被边界截断（记录体超出区域）";
            out->trace.truncated++; out->trace.first_bad = out->trace.total; out->trace.first_bad_why = out->trace.stop_why;
            return 0;
        }
        out->trace.total++;
        out->trace.per_op[r->op]++;
        out->trace.bytes += r->size;
        /* M4ao P1：CALL 体归因栈（见循环前的注释）。in_call = **本条记录**是否在某个 inline 体内。
           标记自身算父体的一条记录，所以先减父再压栈；x[1]==0（旧记录/空体）不计入栈。 */
        int in_call = (call_depth > 0);
        if (r->op == CMD_CALL) {
            out->call_markers++;
            if (call_depth > 0 && --call_rem[call_depth - 1] == 0) call_depth--;
            if (r->x[1] > 0) { if (call_depth < CALL_STK) call_rem[call_depth++] = r->x[1]; }
            else out->call_body_missing++;
        } else if (call_depth > 0) {
            if (--call_rem[call_depth - 1] == 0) call_depth--;
        }
        if (!g_m3s_dumped) {   /* M3s：留首 64 条 + 最近 8 条，供首 draw 时 dump */
            M3sRec *d = (first_n < 64) ? &first[first_n] : NULL;
            if (d) { d->op = r->op; d->x[0] = r->x[0]; d->x[1] = r->x[1]; d->x[2] = r->x[2]; d->x[3] = r->x[3]; first_n++; }
            M3sRec *q = &g_m3s_ring[g_m3s_ring_i++ & 7];
            q->op = r->op; q->x[0] = r->x[0]; q->x[1] = r->x[1]; q->x[2] = r->x[2]; q->x[3] = r->x[3];
        }
        const uint32_t payload = r->size - (uint32_t)sizeof(CmdRec);   /* extra 里的字节数（记录体已验 ≥ 头） */
        void *user = sink ? sink->user : NULL;
        if (r->op == CMD_CLEAR_COLOR && sink && sink->clear) {
            float rgba[4];
            if (r->x[1] && r->size >= sizeof(CmdRec) + 16) memcpy(rgba, r->extra, 16);
            else { memset(rgba, 0, sizeof rgba); out->clear_noptr++; }
            int rv = sink->clear(rgba, user);
            if (rv == 0) out->clears++; else out->clear_fail++;
        }
        if (r->op == CMD_SET_RENDER_TARGETS && sink && sink->rt_bind) {
            int n = (int)(int64_t)r->x[0];
            uint64_t tex0 = 0, depth = 0;
            if (r->size >= sizeof(CmdRec) + NVN_CMD_RT_FIRST_TEX_OFF + 8)
                memcpy(&tex0, r->extra + NVN_CMD_RT_FIRST_TEX_OFF, 8);
            /* M4q P1：深度目标一起交出去——此前 n=0 的纯深度 pass 在回放侧等于「没有目标」，
               连它写的是哪张 1024×1024 深度图都无从得知（m4p_b1 的 1024×1024 全 0 就此而来）。 */
            if (r->size >= sizeof(CmdRec) + NVN_CMD_RT_DEPTH_OFF + 8)
                memcpy(&depth, r->extra + NVN_CMD_RT_DEPTH_OFF, 8);
            int rv = sink->rt_bind(n, tex0, depth, user);
            if (rv == 0) out->rt_binds++; else out->rt_fail++;
        }
        /* ---- M3r：状态绑定 + 绘制（负载长度逐条校验，不够就跳过该条并计数，绝不越界读） ---- */
        if (r->op == CMD_BIND_PROGRAM && sink && sink->bind_program) {
            out->programs++;
            sink->bind_program(r->x[0], user);
        }
        /* ---- M14B（2026-11-08）：blend / color state 分派 ----
           回调总是装着（同一份二进制做 B0/B1 对比，走流结构完全相同，唯一变量是 nvn_vk 里的
           smo_m14b 开关）。nvn_vk 侧在 smo_m14b=0 时第一行就返回 ⇒ 不存状态、不打印、不改管线。 */
        /* 注意 x[] 的下标：CmdRec.x[] 存的是**游客 x1..x7**（record() 把 s.x+1 拷进 r->x），
           所以「录制时 record_copy 的 arg=1」= 游客 x1 = 现在的 r->x[0]，不是 r->x[1]。
           （M14B 首版写错成 r->x[1]，探针把 guest 指针打成 0 —— 只有打印受影响，payload 是对的。） */
        if (r->op == CMD_BIND_BLEND && sink && sink->bind_blend_state) {
            out->blends++;
            sink->bind_blend_state(r->x[0], payload >= 8u ? (const void *)r->extra : NULL,
                                   payload >= 8u ? 8u : 0u, user);
        }
        if (r->op == CMD_BIND_COLOR && sink && sink->bind_color_state) {
            out->colors++;
            sink->bind_color_state(r->x[0], payload >= 4u ? (const void *)r->extra : NULL,
                                   payload >= 4u ? 4u : 0u, user);
        }
        if (r->op == CMD_BIND_VERTEX_ATTRIB && sink && sink->bind_vertex_attrib) {
            int cnt = (int)(int64_t)r->x[0];
            if (cnt < 0 || (uint32_t)cnt * 4u > payload) { out->draw_skip++; }
            else { out->attribs++; sink->bind_vertex_attrib(cnt, r->extra, user); }
        }
        if (r->op == CMD_BIND_VERTEX_STREAM && sink && sink->bind_vertex_stream) {
            int cnt = (int)(int64_t)r->x[0];
            if (cnt < 0 || (uint32_t)cnt * 8u > payload) { out->draw_skip++; }
            else { out->streams++; sink->bind_vertex_stream(cnt, r->extra, user); }
        }
        if (r->op == CMD_BIND_VERTEX_BUFFERS && sink && sink->bind_vertex_buffers) {
            int first = (int)(int64_t)r->x[0], cnt = (int)(int64_t)r->x[1];
            if (cnt < 0 || first < 0 || (uint32_t)cnt * 16u > payload) { out->draw_skip++; }
            else { out->vbufs++; sink->bind_vertex_buffers(first, cnt, r->extra, user); }
        }
        if (r->op == CMD_BIND_UNIFORM_BUFFERS && sink && sink->bind_uniform_buffers) {
            int stage = (int)(int64_t)r->x[0], first = (int)(int64_t)r->x[1], cnt = (int)(int64_t)r->x[2];
            if (cnt < 0 || first < 0 || (uint32_t)cnt * 16u > payload) { out->draw_skip++; }
            else { out->ubufs++; sink->bind_uniform_buffers(stage, first, cnt, r->extra, user); }
        }
        if (r->op == CMD_BIND_STORAGE_BUFFERS && sink && sink->bind_storage_buffers) {
            int stage = (int)(int64_t)r->x[0], first = (int)(int64_t)r->x[1], cnt = (int)(int64_t)r->x[2];
            if (cnt < 0 || first < 0 || (uint32_t)cnt * 16u > payload) { out->draw_skip++; }
            else { out->sbufs++; sink->bind_storage_buffers(stage, first, cnt, r->extra, user); }
        }
        if (r->op == CMD_BIND_TEXTURES && sink && sink->bind_textures) {
            int stage = (int)(int64_t)r->x[0], first = (int)(int64_t)r->x[1], cnt = (int)(int64_t)r->x[2];
            if (cnt < 0 || first < 0 || (uint32_t)cnt * 8u > payload) { out->draw_skip++; }
            else { out->textures++; sink->bind_textures(stage, first, cnt, (const uint64_t *)(const void *)r->extra, user); }
        }
        if (r->op == CMD_DRAW_ELEMENTS && sink && sink->draw_elements) {
            int mode = (int)(int64_t)r->x[0], type = (int)(int64_t)r->x[1];
            int cnt = (int)(int64_t)r->x[2];
            if (cnt <= 0) { out->draw_skip++; out->draw_skip_cnt++; }
            else if (r->x[3] <= 0x10000) { out->draw_skip++; out->draw_skip_idx++; }
            else if (cnt > (1 << 20)) { out->draw_skip_big++; /* 只取证：行为与此前一致，仍然放行 */ }
            if (cnt > 0 && r->x[3] > 0x10000) {
                if (!g_m3s_dumped) { g_m3s_dumped = 1; m3s_dump(sink, first, first_n); }
                m17a_take_lr(r);   /* M17A：本笔的录制者 */
                /* M4ae P2：x[5] 高 32 位 = ABI 类别（0 普通 / 1 BaseVertex / 2 Instanced，见 cmdbuf.c 录制端）
                   M4ao P1：传出 dclass bit8 = 本笔在 CMD_CALL inline 体内（nvn_vk 据此把 skip 拆成「新增/原有」）。 */
                int dcls = (int)(r->x[5] >> 32);
                if (in_call) out->draw_incall++;
                /* M4ap P1：baseInstance 取 x[5] 的低 32 位（高 32 位是类别标记）——
                   vkCmdDrawIndexed 的 firstInstance 必须带上，否则 shader 的 instance-id 起点错。 */
                int bi = (int)(int32_t)(uint32_t)(r->x[5] & 0xFFFFFFFFull);
                int rv = sink->draw_elements(mode, type, cnt, r->x[3],
                                             (int)(int64_t)r->x[4], bi, (int)(int64_t)r->x[6],
                                             dcls | (in_call ? (1 << 8) : 0), user);
                if (rv == 0) { out->draws++; if (in_call) out->draw_incall_ok++; }
                else {
                    out->draw_fail++;
                    if (rv == 1) out->draw_fail_skip++;
                    else if (rv < 0) out->draw_fail_err++;
                    else out->draw_fail_other++;
                }
            }
        }
        /* M4ad P2：无索引 draw。此前这里**没有分支**——CMD_DRAW_ARRAYS 只进 per_op 直方图，
           于是 m4ac 的 1582 条 DrawArrays 一条都没到过回放侧（不是 skip，是没接线）。
           口径 = cmdbuf.c:566：x[0]=mode, x[1]=first, x[2]=count, x[4]=instances（0 当 1，同
           vkCmdDraw 语义：0 实例 = 不画，这里按 1 兜底并在下方注释里说明）。 */
        if (r->op == CMD_DRAW_ARRAYS && sink && sink->draw_arrays) {
            int mode = (int)(int64_t)r->x[0];
            int first_i = (int)(int64_t)r->x[1];
            int cnt = (int)(int64_t)r->x[2];
            int inst = (int)(int64_t)r->x[4];
            if (cnt <= 0 || cnt > (1 << 20) || first_i < 0) { out->draw_arrays_skip++; }
            else {
                if (!g_m3s_dumped) { g_m3s_dumped = 1; m3s_dump(sink, first, first_n); }
                m17a_take_lr(r);   /* M17A：本笔的录制者 */
                int rv = sink->draw_arrays(mode, first_i, cnt, inst > 0 ? inst : 1, user);
                if (rv == 0) out->draw_arrays++; else out->draw_arrays_fail++;
            }
        }
        p += r->size;
        avail -= r->size;
    }
    out->trace.stop_why = "步数守卫（1Mi 条）";
    out->trace.truncated++;
    return 0;
}

int nvn_cmd_exec(const void *start, nvn_cmd_readable_fn readable,
                 int (*clear_cb)(const float rgba[4], void *user),
                 nvn_cmd_rt_cb rt_cb, void *user,
                 CmdExecStat *out)
{
    nvn_cmd_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.clear = clear_cb;
    sink.rt_bind = rt_cb;
    sink.user = user;
    return nvn_cmd_exec_sink(start, readable, &sink, out);
}

/* task B：把一次 walk 累加进 dst。 */
void nvn_cmd_trace_merge(CmdTrace *dst, const CmdTrace *src)
{
    dst->total += src->total;
    dst->bytes += src->bytes;
    dst->jumps += src->jumps;
    dst->truncated += src->truncated;
    dst->bad += src->bad;
    for (int i = 0; i < CMD_TRACE_MAX_OPS; i++) dst->per_op[i] += src->per_op[i];
    if (!dst->first_bad_why && src->first_bad_why) { dst->first_bad = src->first_bad; dst->first_bad_why = src->first_bad_why; }
}

/* 旧接口：单段直走、不跟跳转（固定 bytes 预算内）。保留为兼容。 */
int nvn_cmd_trace(const void *base, uint64_t bytes, CmdTrace *out)
{
    memset(out, 0, sizeof *out);
    const uint8_t *p = (const uint8_t *)base, *end = p + bytes;
    for (;;) {
        if (p + sizeof(CmdRec) > end) { out->first_bad_why = "记录头超出命令流"; out->first_bad = out->total; out->stop_why = out->first_bad_why; return 0; }
        const CmdRec *r = (const CmdRec *)p;
        if (r->op >= CMD_COUNT) { out->first_bad_why = "opcode 越界"; out->first_bad = out->total; out->stop_why = out->first_bad_why; return 0; }
        if (r->size < sizeof(CmdRec) || (r->size & 7)) { out->first_bad_why = "size 非法（非 8 对齐或过小）"; out->first_bad = out->total; out->stop_why = out->first_bad_why; return 0; }
        if (p + r->size > end) { out->first_bad_why = "记录体超出命令流"; out->first_bad = out->total; out->stop_why = out->first_bad_why; return 0; }
        out->total++;
        out->per_op[r->op]++;
        out->bytes += r->size;
        if (r->op == CMD_END) { out->stop_why = "END"; return 1; }
        p += r->size;
    }
}

/* 打印直方图（只列非零项，按次数降序）。 */
void nvn_cmd_trace_report(const CmdTrace *t)
{
    fprintf(stderr, "[cmdtrace] 总记录=%llu 字节=%llu 跳转=%llu 截断=%llu 非法=%llu 停因=%s 首条非法=第%llu条(%s)\n",
            (unsigned long long)t->total, (unsigned long long)t->bytes,
            (unsigned long long)t->jumps, (unsigned long long)t->truncated,
            (unsigned long long)t->bad, t->stop_why ? t->stop_why : "?",
            (unsigned long long)t->first_bad, t->first_bad_why ? t->first_bad_why : "无");
    /* 简单选择排序，最多 64 项 */
    int idx[CMD_TRACE_MAX_OPS], n = 0;
    for (int i = 0; i < CMD_TRACE_MAX_OPS; i++) if (t->per_op[i]) idx[n++] = i;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (t->per_op[idx[j]] > t->per_op[idx[i]]) { int tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp; }
    for (int i = 0; i < n; i++) {
        int op = idx[i];
        fprintf(stderr, "[cmdtrace]   %-24s %8llu\n", kOpNames[op] ? kOpNames[op] : "?",
                (unsigned long long)t->per_op[op]);
    }
}
