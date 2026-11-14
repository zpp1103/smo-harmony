/* cmd_trace 契约头：cmd_trace.c 的实现与调用方（platform/gfx_bridge.c）共用，
   避免两侧各写一份 CmdTrace 造成静默 ABI 漂移（直方图会读错偏移）。
   语义见 cmd_trace.c 顶部注释；CmdRec 契约见 nvn.h:117-141。 */
#ifndef NVN_PORT_CMD_TRACE_H
#define NVN_PORT_CMD_TRACE_H
#include <stdint.h>

#define CMD_TRACE_MAX_OPS 64

/* CLEAR_COLOR 的 opcode 号（= nvn.h:133 的 CMD_CLEAR_COLOR）。消费方（platform/gfx_bridge.c）
   只为一个常量不值得 include nvn.h（它的 compat.h 会把 LOG/FATAL 等宏带进 gfx_bridge 的命名域）。
   防漂移：cmd_trace.c 顶部有 _Static_assert 与本值比对。 */
#define NVN_CMD_OP_CLEAR_COLOR 33
/* M3i：SET_RENDER_TARGETS 的 opcode 号（= nvn.h:127 的 CMD_SET_RENDER_TARGETS），同款防漂移。 */
#define NVN_CMD_OP_SET_RENDER_TARGETS 19
/* M4an：CALL 的 opcode 号（= nvn.h 枚举里的 CMD_CALL）。CallCommands 每 inline 一个 handle 插一条，
   直方图里 CALL 从 0 变 >0 就是「二级命令流真的接进命令流」的判据。 */
#define NVN_CMD_OP_CALL 2
/* SET_RENDER_TARGETS 记录体（cmdbuf.c:407 RtRec）的前 8 字节 = 槽 0 贴图句柄（RtRec.tex[0]）。
   回放侧只读这 8 字节，不复制整结构：整结构内嵌 infra 的 NvnTextureView 布局，本头不拉 nvn.h。 */
#define NVN_CMD_RT_FIRST_TEX_OFF 0
/* M4q P1：RtRec.depth 在记录体里的偏移。布局 { u64 tex[8]; NvnTextureView views[8];
   u8 has_view[8]; u64 depth; … }，视图 16B×8=128（nvn_obj.h:46 断言 sizeof(view_t)==16），
   故 64+128+8 = 200（200%8==0，depth 无需补齐）。cmdbuf.c 侧有 offsetof 静态断言对账；
   没有深度目标时该 8 字节为 0（录制端 RtRec 零初始化，rt.depth 只在 s.x[4] 非 0 时写入）。 */
#define NVN_CMD_RT_DEPTH_OFF 200

typedef struct {
    uint64_t total;                     /* 走过的记录总数（不含 JUMP） */
    uint64_t per_op[CMD_TRACE_MAX_OPS]; /* 每个 opcode 的次数（JUMP 计在 jumps，不在表里） */
    uint64_t bytes;                     /* 命令流总字节（含 JUMP/END 条目自身的 size） */
    uint64_t first_bad;                 /* 第一条非法记录的位置（0 = 没遇到） */
    const char *first_bad_why;
    /* 2026-11-02 task A/B/C 新增：跨 chunk 走流 + 边界安全计数 */
    uint64_t jumps;                     /* 跟随过的 CMD_JUMP 数 */
    uint64_t truncated;                 /* 被边界截断 / 目标不可判定的次数 */
    uint64_t bad;                       /* 非法记录（opcode 越界 / size 非法）次数 */
    const char *stop_why;               /* 停因："END"= 走完；其余 = 提前终止原因 */
} CmdTrace;

/* 可读性查询：宿主地址 p 落在我们分配过的命令记录区域内，返回从 p 起可安全读的字节数；
   0 = 未知区域（绝不能读）。实现见 cmdbuf.c nvn_port_cmdmem_readable + glue_cmdbuf.c 池表。 */
typedef uint64_t (*nvn_cmd_readable_fn)(const void *p);

/* 跨 chunk 走完整条命令流（start = EndRecording 句柄 = 首条记录宿主地址）。
   语义照 infra cmdbuf.c 的 nvn_check_list/copy_list（:679-712）：顺着 CMD_JUMP 链走，
   每一步先经 readable 验边界，越界/未知区域即停并计 truncated，绝不越界读。
   走完（遇 CMD_END）返 1；提前终止返 0，停因在 out->stop_why。 */
int nvn_cmd_trace_walk(const void *start, nvn_cmd_readable_fn readable, CmdTrace *out);

/* 把一次 walk 的结果累加进 dst（task B：跨 submit 累计直方图）。 */
void nvn_cmd_trace_merge(CmdTrace *dst, const CmdTrace *src);

/* 旧接口（固定单段、不跟跳转），保留仅为兼容；新代码用 nvn_cmd_trace_walk。 */
int nvn_cmd_trace(const void *base, uint64_t bytes, CmdTrace *out);

/* 打印直方图（总数 + 全部非零 opcode，按次数降序；opcode 名字见 cmd_trace.c:19-36）。 */
void nvn_cmd_trace_report(const CmdTrace *t);

/* opcode 名字（越界返 NULL）——M3h：跳过统计要按名报，名字表只在 cmd_trace.c 里有一份。 */
const char *nvn_cmd_op_name(int op);

/* ---- M3h（2026-11-03）：执行式走流，最小回放只做 CLEAR_COLOR ----
   与 nvn_cmd_trace_walk 共用同一套边界/跳转/合法性语义（先 readable 再读、跟 CMD_JUMP 链、
   size 上限 1MiB），唯一区别：遇到 CMD_CLEAR_COLOR 调 clear_cb 执行，其余 opcode 只计数。 */
typedef struct {
    CmdTrace trace;        /* 本条流的 per_op 直方图（含 CLEAR_COLOR/END，total 已计数） */
    uint64_t clears;       /* 回调返回 0（成功执行）的 CLEAR_COLOR 数 */
    uint64_t clear_fail;   /* 回调返回非 0 的次数 */
    uint64_t clear_noptr;  /* 颜色指针为空（x[1]==0）或记录体不足 16B：按全 0 色处理 */
    /* M3i：SET_RENDER_TARGETS 的执行口径（rt_cb 非空时才有非零值） */
    uint64_t rt_binds;     /* 回调返回 0 的次数（RT 绑定已记录/下发） */
    uint64_t rt_fail;      /* 回调返回非 0 的次数 */
    /* M3r：sink 口径（nvn_cmd_exec_sink；本结构在末尾追加，前 6 个字段布局不变） */
    uint64_t programs;     /* BIND_PROGRAM 交给回调的次数 */
    uint64_t attribs;      /* BIND_VERTEX_ATTRIB 交给回调的次数 */
    uint64_t streams;      /* BIND_VERTEX_STREAM 交给回调的次数 */
    uint64_t vbufs;        /* BIND_VERTEX_BUFFERS 交给回调的次数 */
    uint64_t ubufs;        /* BIND_UNIFORM_BUFFERS 交给回调的次数（只计数，见 nvn_cmd_sink 注释） */
    uint64_t textures;     /* BIND_TEXTURES 交给回调的次数（同上） */
    uint64_t sbufs;        /* M5x: BIND_STORAGE_BUFFERS 交给回调的次数（gmem 地址来源） */
    uint64_t draws;        /* DRAW_ELEMENTS 回调返回 0（真画了）的次数 */
    uint64_t draw_fail;    /* DRAW_ELEMENTS 回调返回非 0 的次数 */
    uint64_t draw_skip;    /* 记录体不足/参数不可用而未交给回调的次数 */
    /* M4ad P2：DRAW_ARRAYS 的处理口径（此前整类没有分支，只进 per_op 直方图） */
    uint64_t draw_arrays;       /* DRAW_ARRAYS 回调返回 0 的次数 */
    uint64_t draw_arrays_fail;  /* DRAW_ARRAYS 回调返回非 0 的次数 */
    uint64_t draw_arrays_skip;  /* count/first 非法而未交给回调的次数 */
    /* ---- M4an（2026-11-04）：sink 级 draw 拒收的**原因拆分** ----
       draw_skip 此前只累加、从没打印（gfx_bridge 里 `g_h_cum.draw_skip+=...` 后再无出口）⇒
       「回放侧 skip」与「nvn_vk 侧 skip」对不上账，属于零证据盲区。boot#1 实测 DRAW_ELEMENTS 记录
       395,605 条、真正执行 50,608 条，而 nvn_vk 的 skip 分项只解释得了 73k ⇒ 有 ~275k 拒绝发生在本 sink。
       这两个字段把「计数<=0」与「索引地址非法」分开，下一轮据此判「内联记录体是不是坏的」。 */
    uint64_t draw_skip_cnt;     /* CMD_DRAW_ELEMENTS: count <= 0 */
    uint64_t draw_skip_idx;     /* CMD_DRAW_ELEMENTS: 索引地址 x[3] <= 0x10000 */
    uint64_t draw_skip_big;     /* CMD_DRAW_ELEMENTS: count > 1<<20 */
    /* ---- M4ao（2026-11-04）：sink 侧 fail 的返回码分档 + CALL 体内归因 ----
       此前 fail=37,155 只有一个总数，说不出「多少是 skip 类、多少是 fail 类」。
       rv 语义（nvn_vk.h 的绘图入口）：1 = 各类 skip（见 [r] skip audit 的理由码）；
       <0 = 真失败（-1 未初始化 / -2 r_common_ensure / -3 管线 / -4 顶点 arena）。 */
    uint64_t draw_fail_skip;    /* DRAW_ELEMENTS 回调返回 1（nvn_vk 的 skip_* 类） */
    uint64_t draw_fail_err;     /* DRAW_ELEMENTS 回调返回 <0（nvn_vk 的 fail 类） */
    uint64_t draw_fail_other;   /* 其它非 0/非 1/非负 的返回（应为 0） */
    uint64_t draw_incall;       /* 来自 CMD_CALL inline 体内的 DRAW_ELEMENTS 记录数（seen 口径） */
    uint64_t draw_incall_ok;    /* 其中回调返回 0（真画了）的笔数 */
    uint64_t call_markers;      /* 走到的 CMD_CALL 标记数（x[1] = 该 handle 体内记录数） */
    uint64_t call_body_missing; /* CMD_CALL 标记的 x[1] == 0（旧记录/空体）次数 —— 归因栈的已知缺口 */
    /* M14B（2026-11-08）：blend / color state 绑定回放计数（sink 回调非 NULL 时才动）。 */
    uint64_t blends;            /* CMD_BIND_BLEND 分派次数 */
    uint64_t colors;            /* CMD_BIND_COLOR 分派次数（blend enable 的唯一来源） */
} CmdExecStat;

/* ---- M3r（2026-11-03）：状态/绘制回调表 ----
   目的：回放不再只认 CLEAR_COLOR / SET_RENDER_TARGETS。所有回调都可为 NULL（则该 opcode 只计数）。
   负载布局取自录制端 cmdbuf.c（record() 把 s.x+1 拷进 r->x，record_copy 再从 x[arg] 拷 len 字节）：
     BIND_PROGRAM          x[0]=program（本移植里就是客体/宿主同值指针，cmdbuf.c:317 已改直存）
     BIND_VERTEX_ATTRIB    x[0]=count，extra = count × 4B（va_st{fmt:8,offset:19,stream:5}）
     BIND_VERTEX_STREAM    x[0]=count，extra = count × 8B（vs_st{u32 stride,u32 divisor}）
     BIND_VERTEX_BUFFERS   x[0]=first, x[1]=count，extra = count × 16B（{u64 addr,u64 size}）
     BIND_UNIFORM_BUFFERS  x[0]=stage, x[1]=first, x[2]=count，extra = count × 16B（同 bufferRange）
     BIND_TEXTURES         x[0]=stage, x[1]=slot, x[2]=count，extra = count × 8B（texture handle）
     DRAW_ELEMENTS         x[0]=mode, x[1]=type, x[2]=count, x[3]=index address,
                           x[4]=baseVertex, x[5]=baseInstance, x[6]=instances（cmdbuf.c:567-579）
     DRAW_ARRAYS           x[0]=mode, x[1]=first, x[2]=count, x[3]=baseInstance,
                           x[4]=instances（cmdbuf.c:566-567）
   idx 是 NVNbufferAddress；本移植 GPU 地址 == CPU 地址（gfx_bridge.c:348 注释），可直接读。 */
typedef struct {
    int (*clear)(const float rgba[4], void *user);
    /* M4q P1：depth = 录制侧 RtRec.depth（NVNtexture*，无深度目标为 0）；n=0 的纯深度 pass 靠它取证。 */
    int (*rt_bind)(int n, uint64_t tex0, uint64_t depth, void *user);
    int (*bind_program)(uint64_t program, void *user);
    int (*bind_vertex_attrib)(int count, const void *states, void *user);
    int (*bind_vertex_stream)(int count, const void *states, void *user);
    int (*bind_vertex_buffers)(int first, int count, const void *ranges, void *user);
    int (*bind_uniform_buffers)(int stage, int first, int count, const void *ranges, void *user);
    int (*bind_textures)(int stage, int first, int count, const uint64_t *handles, void *user);
    int (*draw_elements)(int mode, int type, int count, uint64_t idx,
                         int base_vertex, int base_instance, int instances, int dclass, void *user);
    /* M3s：取证行出口（可为 NULL）。收到的是已经格式化好、不含时间戳的一行；
       调用方（gfx_bridge）只需原样写进 vk_boot.log。用于「首 draw 前 64 条 opcode」dump。 */
    void (*note)(const char *line, void *user);
    void *user;
    /* M4ad P2：无索引 draw。负载见 nvn.h/cmdbuf.c:566-567：
       x[0]=mode, x[1]=first, x[2]=count, x[3]=baseInstance(0), x[4]=instances(≥1)。
       接在结构末尾是为了不动既有字段偏移（两侧同编译，但少一个变量就少一种漂移可能）。 */
    int (*draw_arrays)(int mode, int first, int count, int instances, void *user);
    /* M5x：storage buffer 绑定（gmem 的数据源）。负载与 uniform 同形：count × 16B {addr,size}，
       在 r->extra。语料的 gmem shader 读 cb0[17]，而 c0 是 Maxwell 驱动保留段 —— 地址由这条 bind
       决定，用户 UBO 里本来就没有它（M5x boot#1 的 cb0 dump 已证明）。 */
    int (*bind_storage_buffers)(int stage, int first, int count, const void *ranges, void *user);
    /* ---- M14B（2026-11-08）：blend / color 状态绑定 ----
       录制端 cmdbuf.c:498-500：`record_copy(c, CMD_BIND_BLEND, 1, 8)` / `(CMD_BIND_COLOR, 1, 4)`
       ⇒ x[1] = 客体里的状态对象地址，extra = **录制时刻**从该地址拷下的字节（blend 8B / color 4B）。
       回放侧一律用 extra 这份（录制语义的就是绑定时刻的值），不用 x[1] 现读（客体可能已经改过）。
       bytes = NULL 表示记录体不够长（旧记录），调用方按「未绑定」处理。 */
    int (*bind_blend_state)(uint64_t guest_ptr, const void *bytes, uint32_t nbytes, void *user);
    int (*bind_color_state)(uint64_t guest_ptr, const void *bytes, uint32_t nbytes, void *user);
} nvn_cmd_sink;

/* 与 nvn_cmd_exec 同一条走流语义（同一 rec_bad/边界/跳转/守卫），按 sink 分派各 opcode。 */
int nvn_cmd_exec_sink(const void *start, nvn_cmd_readable_fn readable,
                      const nvn_cmd_sink *sink, CmdExecStat *out);

/* rt_cb：n = 颜色 RT 数（记录体 x[0]，录制侧 cmdbuf.c:418 显式写入），
   tex0 = 记录体前 8 字节（槽 0 贴图句柄，见 NVN_CMD_RT_FIRST_TEX_OFF；无负载时给 0）。
   depth = 记录体 NVN_CMD_RT_DEPTH_OFF 处的 8 字节（NVNtexture*，0 = 无深度目标）。
   返回 0 = 执行成功。 */
typedef int (*nvn_cmd_rt_cb)(int n, uint64_t tex0, uint64_t depth, void *user);

/* clear_cb：rgba = 录制时从客体 color4 拷下的 4 个 float。返回 0 = 执行成功。
   走完（遇 CMD_END）返 1；提前终止返 0，停因在 out->trace.stop_why（同 walk）。 */
int nvn_cmd_exec(const void *start, nvn_cmd_readable_fn readable,
                 int (*clear_cb)(const float rgba[4], void *user),
                 nvn_cmd_rt_cb rt_cb, void *user,
                 CmdExecStat *out);

#endif
