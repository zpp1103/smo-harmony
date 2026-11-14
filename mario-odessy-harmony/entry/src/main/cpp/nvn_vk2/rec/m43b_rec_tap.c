// M43B / S2b-1：**全量可镜像 + 整帧对账**探针（旁路录制，绝不切换执行路径）。
//
// 与 M43A 版（m43a_rec_tap.c）的差别，正是本单的三件事：
//   ① 不再依赖 Mach-O 跳板：沿用 "swnvn" 段按名直调 nx2 的 static 实现（跳板 ELF 版另有其件，见 nvn_thunks_elf.S）。
//   ② **状态类 OP_* 也镜像**：M43A 只镜像了 7 个 draw 记录器；本单把状态类（BIND_* / SET_* / CLEAR_* /
//      BARRIER / DISPATCH / COPY_* …）也走同一条探针。直喂侧的挂点由 platform/nvn_replay/cmdbuf.c 里
//      每个记录器末尾的一行 M43B_TAP(id) 提供（**不是**挂在 record() 里：record() 挂点看不到
//      "单参数 vs 多参数"变体与 record 之后才补写的字段，会造出假的参数不符）。
//   ③ **帧内缓冲 + 帧末一次性落盘**：命令按 g_beat_frames 归帧，帧内全部累积在内存里，
//      帧边界处一次性写 m43b_frame.tsv + 一行 m43b_frame_summary.tsv，然后**重置私有录制缓冲**复用 arena。
//
// 判定口径（与 M43A 一脉相承，C 类必须为 0）：
//   A_ENUM  枚举编号差：直喂写 CMD_*、录制写 OP_*，两套枚举各自编号；**参数逐字段全等**、录制侧恰好 1 条记录。
//   B_SEMEQ 语义等价：参数全等，但录制侧把 1 条 NVN 调用拆成 N 条记录（BindUniformBuffers/StorageBuffers 每槽一条）
//           或把字段换了编码位置。
//   C_MISS  真实缺失：直喂侧这条命令进了玩家命令流，录制侧**一条记录都没产生**、或产生了但 opcode 对不上、
//           或参数不符且无法用映射解释。→ 本单要求 0 条。
//   D_PARAM 参数不符：opcode 对上了，但比较的关键字段有差异。→ 本单要求 0 条。
//   E_NOREC 录制侧**按设计不记录**（nx2 显式 no-op，如 SetAlphaRef/SetLineWidth/SetPointSize/SetSampleMask；
//           或被 game profile 门控，如 SetRenderEnable 在 DM_GAME_RENDER_COND 关时不记录）。
//           不是数据丢失，但**必须在覆盖矩阵里逐条点名**，所以单列一栏，绝不混进 A/B。
//
// 开关：/data/.../files/DM_M43B_REC 内容为 '0' 关闭；文件缺失默认开（对账要它开，规模有硬上限）。
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nvn_obj.h"   // rec/ 隔离 shim → nvn_vk2/abi/nvn_obj.h（nx2 的 OP_ 枚举 + 命令记录格式 + p_* 载荷）

// 隔离证明（验收 2）：录制侧这个 TU 只可能看到 nx2 的枚举（OP_COUNT_=50）；若有人把 third_party/d2r_gfx
// 的同名头塞进 rec/ 的包含路径（那边 OP_COUNT_=43），这里立刻炸掉。
_Static_assert(OP_COUNT_ == 50, "M43B: recording side must compile against nx2android's op enum (OP_COUNT_ = 50)");
_Static_assert(OP_DRAW == 25 && OP_DRAW_IDX == 26, "M43B: OP_ numbering moved; update the reconciliation mapping");
_Static_assert(OP_CLEAR_TEX == 38 && OP_DEPTH_CLAMP == 40, "M43B: nx2 numbering (S1 §8-2 measured) changed");

// 直喂侧 opcode 号：platform/nvn_replay/nvn.h:124-141 的 CMD_* 枚举（BOTWRecomp 命令流格式）。
// 这里**逐值写死**并带名字，供 TSV 直接打印；与 nvn.h 的一致性由下方 _Static_assert 在 cmdbuf.c 侧保证
// （cmdbuf.c 的 M43B_TAP 传进来的就是 r->op，本文件的常量只用于"我期望看到哪个号"的自检表）。
enum {
    C_END = 0, C_JUMP, C_CALL,
    C_BIND_PROGRAM, C_BIND_BLEND, C_BIND_CHANNEL_MASK, C_BIND_COLOR, C_BIND_DEPTH_STENCIL,
    C_BIND_MULTISAMPLE, C_BIND_POLYGON, C_BIND_VERTEX_ATTRIB, C_BIND_VERTEX_STREAM,
    C_BIND_VERTEX_BUFFERS, C_BIND_UNIFORM_BUFFERS, C_BIND_STORAGE_BUFFERS, C_BIND_TEXTURES,
    C_BIND_IMAGES, C_SET_TEXTURE_POOL, C_SET_SAMPLER_POOL, C_SET_RENDER_TARGETS,
    C_SET_VIEWPORTS, C_SET_SCISSORS, C_SET_DEPTH_RANGES, C_SET_BLEND_COLOR, C_SET_STENCIL,
    C_SET_PRIMITIVE_RESTART, C_SET_ALPHA_REF, C_SET_DEPTH_BOUNDS, C_SET_POLYGON_OFFSET,
    C_SET_LINE_WIDTH, C_SET_POINT_SIZE, C_SET_SAMPLE_MASK, C_SET_RENDER_ENABLE,
    C_CLEAR_COLOR, C_CLEAR_DEPTH_STENCIL, C_CLEAR_TEXTURE, C_CLEAR_BUFFER,
    C_DRAW_ARRAYS, C_DRAW_ELEMENTS, C_DRAW_ARRAYS_INDIRECT, C_DRAW_ELEMENTS_INDIRECT,
    C_DISPATCH, C_DISPATCH_INDIRECT,
    C_COPY_BUFFER_TO_BUFFER, C_COPY_BUFFER_TO_TEXTURE, C_COPY_TEXTURE_TO_BUFFER,
    C_COPY_TEXTURE_TO_TEXTURE, C_DOWNSAMPLE, C_UPDATE_UNIFORM_BUFFER, C_FENCE_SYNC, C_WAIT_SYNC,
    C_BARRIER, C_SIGNAL_EVENT, C_WAIT_EVENT, C_REPORT_COUNTER, C_DISCARD_COLOR,
    C_DISCARD_DEPTH_STENCIL, C_SET_COPY_STRIDES, C_DRAW_TEXTURE, C_RESOLVE_DEPTH, C_SET_DEPTH_CLAMP,
    C_BIND_TEXTURES_DEFERRED,
    C_COUNT
};

// 主循环帧号（platform/vk_boot.h:29，nvn_window_acquire_texture 里 ++）：用来把同一次调用归到同一帧。
extern _Atomic unsigned long g_beat_frames;

// ---- "swnvn" 段查表：nx2 录制层注册在段里的 {name, fn} 数组（hle_nvn.c:66-73 的 nvn_impl）----
typedef struct { const char *name; void *fn; } m43b_sect_ent;
extern const m43b_sect_ent __start_swnvn[] __attribute__((weak, visibility("hidden")));
extern const m43b_sect_ent __stop_swnvn[]  __attribute__((weak, visibility("hidden")));

static void *sect_find(const char *name) {
    if (!__start_swnvn || !__stop_swnvn) return NULL;
    for (const m43b_sect_ent *p = __start_swnvn; p < __stop_swnvn; p++)
        if (p->name && !strcmp(p->name, name)) return p->fn;
    return NULL;
}

// ============================ 行缓冲 + 逐帧落盘 ============================
enum { M_A_ENUM = 0, M_B_SEMEQ, M_C_MISS, M_D_PARAM, M_E_NOREC, M_STATUS_COUNT };
static const char *const g_status_name[M_STATUS_COUNT] = {
    "A_enumnum", "B_semeq", "C_real_missing", "D_param_mismatch", "E_norec_by_design"
};

#define M43B_ROWS_PER_FLUSH 4096     // 一帧最多缓冲这么多行，超出就"mid-frame 落盘"（计数继续累加，不丢行）
#define M43B_MAX_FRAMES     100000   // 帧数上限（实际由行数上限收口；早期 boot 帧只有 27 行/帧）
#define M43B_MAX_ROWS       300000   // 总行数上限（对账窗口实际边界；约 36 MB TSV）

typedef struct {
    uint32_t frame, seq;
    uint16_t id;        // 本文件内部命令族 id（见 enum m43b_tap）
    uint16_t nrec;      // 录制侧这一笔产生了多少条记录
    int16_t  rop;       // 录制侧第一条记录的 opcode（-1 = 一条都没有）
    uint8_t  st, ncmp, nmis, cap;   // cap: 1 = 本条因录制 arena 空间不足而跳过（必须为 0）
    int64_t  d[4], r[4];            // 比较的关键字段（直喂侧 / 录制侧）
} m43b_row;

static m43b_row g_rows[M43B_ROWS_PER_FLUSH];
static int g_nrow, g_rows_total, g_frames_done;
static uint64_t g_cur_frame, g_seq;
static int g_have_frame;
static FILE *g_ftsv, *g_fsum;
static uint64_t g_cnt[M_STATUS_COUNT];      // 当前帧的计数
static uint64_t g_tot[M_STATUS_COUNT];      // 全程
static uint64_t g_cap_skip, g_cmd_seen[C_COUNT];
/* 直喂侧 record() 对每一条写进玩家命令流的记录都调一次（全量计数，不挑 opcode）：
   与对账行数逐 opcode 相等 = "没有任何一条命令绕过了对账通道"。 */
void m43b_note_cmd(int op) { if (op >= 0 && op < C_COUNT) __atomic_fetch_add(&g_cmd_seen[op], 1, __ATOMIC_RELAXED); }

/* 游戏是**多线程**录制命令流的（实测 300k 行里有 5 行读出别人的记录 = 两线程同时进私有录制缓冲）。
   私有录制缓冲与行缓冲都是单实例，必须串行化；否则会出现"假真实缺失"（读到别人写的 opcode）。 */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int    g_on = -1, g_ready;
static char   g_paths[2][256];

#define M43B_DIRS 2
static const char *const g_dirs[M43B_DIRS] = {
    "/data/app/el2/100/base/com.vibecc.smo/files/",
    "/data/storage/el2/base/files/",
};
static FILE *files_open_read(char *buf, size_t n, const char *name) {
    for (int i = 0; i < M43B_DIRS; i++) {
        snprintf(buf, n, "%s%s", g_dirs[i], name);
        FILE *f = fopen(buf, "rb");
        if (f) return f;
    }
    return NULL;
}
static FILE *files_open_write(char *buf, size_t n, const char *name) {
    for (int i = 0; i < M43B_DIRS; i++) {
        snprintf(buf, n, "%s%s", g_dirs[i], name);
        FILE *f = fopen(buf, "wb");
        if (f) return f;
    }
    return NULL;
}

extern void vk_boot_logf(const char *fmt, ...);   /* platform/vk_boot.c：直喂侧原有的日志出口 */
extern int  dm_bigstack_selftest(void);            /* M44A：nvn_bigstack_elf.c（512 KB 大栈自检） */
extern void dm_bigstack_selftest_async(void);      /* M44A：等日志就绪后再跑（constructor 阶段日志还没开） */
static void tap_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tap_log(const char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    vk_boot_logf("[m43b] %s", b);
}

// ---- 私有录制命令缓冲（game 的 NVNcommandBuffer 一个字节都不碰）----
static cmd_t g_cb_obj;
static uint8_t g_arena[4 << 20];
static pool_t g_pool;
static uint8_t *g_seg_start;

static void recorder_reset(void) {
    if (!g_ready) return;
    g_cb_obj.cur = g_seg_start;
    g_cb_obj.begin = NULL;
    g_cb_obj.used = 0;
    g_cb_obj.recording = 2;   // 2 = "在录但还没写第一条"（hle_nvn.c BeginRecording 的同一状态）
}

// ============================ 命令族表 ============================
// 命令族 id 的唯一事实源：m43b_tap_ids.h（直喂侧 cmdbuf.c 的 M43B_TAP(id) 用的是同一份编号）。
#include "m43b_tap_ids.h"

typedef struct { const char *fn; uint16_t cmd; int16_t op; } m43b_ent;
// op = 期望的录制侧 opcode；-1 = 录制侧按设计不记录（E_NOREC）
static m43b_ent g_ent[T_COUNT] = {
    [T_DRAW_ARRAYS]      = { "nvnCommandBufferDrawArrays",                C_DRAW_ARRAYS,              OP_DRAW },
    [T_DRAW_ARRAYS_I]    = { "nvnCommandBufferDrawArraysInstanced",       C_DRAW_ARRAYS,              OP_DRAW },
    [T_DRAW_ELEMENTS]    = { "nvnCommandBufferDrawElements",              C_DRAW_ELEMENTS,            OP_DRAW_IDX },
    [T_DRAW_ELEMENTS_BV] = { "nvnCommandBufferDrawElementsBaseVertex",    C_DRAW_ELEMENTS,            OP_DRAW_IDX },
    [T_DRAW_ELEMENTS_I]  = { "nvnCommandBufferDrawElementsInstanced",     C_DRAW_ELEMENTS,            OP_DRAW_IDX },
    [T_DRAW_ARRAYS_IND]  = { "nvnCommandBufferDrawArraysIndirect",        C_DRAW_ARRAYS_INDIRECT,     OP_DRAW },
    [T_DRAW_ELEMENTS_IND]= { "nvnCommandBufferDrawElementsIndirect",      C_DRAW_ELEMENTS_INDIRECT,   OP_DRAW_IDX },
    [T_PROGRAM]     = { "nvnCommandBufferBindProgram",           C_BIND_PROGRAM,           OP_BIND_PROGRAM },
    [T_BLEND]       = { "nvnCommandBufferBindBlendState",        C_BIND_BLEND,             OP_BIND_STATE },
    [T_CHMASK]      = { "nvnCommandBufferBindChannelMaskState",  C_BIND_CHANNEL_MASK,      OP_BIND_STATE },
    [T_COLOR]       = { "nvnCommandBufferBindColorState",        C_BIND_COLOR,             OP_BIND_STATE },
    [T_DSST]        = { "nvnCommandBufferBindDepthStencilState",  C_BIND_DEPTH_STENCIL,     OP_BIND_STATE },
    [T_MS]          = { "nvnCommandBufferBindMultisampleState",  C_BIND_MULTISAMPLE,       OP_BIND_STATE },
    [T_POLY]        = { "nvnCommandBufferBindPolygonState",       C_BIND_POLYGON,           OP_BIND_STATE },
    [T_VATTR]       = { "nvnCommandBufferBindVertexAttribState", C_BIND_VERTEX_ATTRIB,     OP_BIND_VATTR },
    [T_VSTREAM]     = { "nvnCommandBufferBindVertexStreamState", C_BIND_VERTEX_STREAM,     OP_BIND_VSTREAM },
    [T_VBUF]        = { "nvnCommandBufferBindVertexBuffers",     C_BIND_VERTEX_BUFFERS,    OP_BIND_VBUF },
    [T_VBUF1]       = { "nvnCommandBufferBindVertexBuffer",      C_BIND_VERTEX_BUFFERS,    OP_BIND_VBUF },
    [T_UBO]         = { "nvnCommandBufferBindUniformBuffers",    C_BIND_UNIFORM_BUFFERS,   OP_BIND_UBO },
    [T_UBO1]        = { "nvnCommandBufferBindUniformBuffer",     C_BIND_UNIFORM_BUFFERS,   OP_BIND_UBO },
    [T_SSBO]        = { "nvnCommandBufferBindStorageBuffers",    C_BIND_STORAGE_BUFFERS,   OP_BIND_SSBO },
    [T_SSBO1]       = { "nvnCommandBufferBindStorageBuffer",     C_BIND_STORAGE_BUFFERS,   OP_BIND_SSBO },
    [T_TEX]         = { "nvnCommandBufferBindTextures",          C_BIND_TEXTURES,          OP_BIND_TEX },
    [T_TEX1]        = { "nvnCommandBufferBindTexture",           C_BIND_TEXTURES,          OP_BIND_TEX },   // 单槽按 1 条的复数形态镜像 → 录制侧 OP_BIND_TEX
    [T_IMG]         = { "nvnCommandBufferBindImages",            C_BIND_IMAGES,            OP_BIND_IMG },
    [T_IMG1]        = { "nvnCommandBufferBindImage",             C_BIND_IMAGES,            OP_BIND_IMG },   // 单槽按 1 条复数形态镜像
    [T_TEXPOOL]     = { "nvnCommandBufferSetTexturePool",        C_SET_TEXTURE_POOL,       OP_SET_TEX_POOL },
    [T_SAMPPOOL]    = { "nvnCommandBufferSetSamplerPool",        C_SET_SAMPLER_POOL,       OP_SET_SAMP_POOL },
    [T_RT]          = { "nvnCommandBufferSetRenderTargets",      C_SET_RENDER_TARGETS,     OP_SET_RT },
    [T_VIEWPORTS]   = { "nvnCommandBufferSetViewports",          C_SET_VIEWPORTS,          OP_VIEWPORTS },
    [T_VIEWPORT1]   = { "nvnCommandBufferSetViewport",           C_SET_VIEWPORTS,          OP_VIEWPORTS },
    [T_SCISSOR]     = { "nvnCommandBufferSetScissors",           C_SET_SCISSORS,           OP_SCISSOR },
    [T_SCISSOR1]    = { "nvnCommandBufferSetScissor",            C_SET_SCISSORS,           OP_SCISSOR },
    [T_DRANGE]      = { "nvnCommandBufferSetDepthRanges",        C_SET_DEPTH_RANGES,       OP_DEPTH_RANGE },
    [T_DRANGE1]     = { "nvnCommandBufferSetDepthRange",         C_SET_DEPTH_RANGES,       OP_DEPTH_RANGE },
    [T_BLENDCOLOR]  = { "nvnCommandBufferSetBlendColor",         C_SET_BLEND_COLOR,        OP_BLEND_COLOR },
    [T_STENCIL_REF] = { "nvnCommandBufferSetStencilRef",         C_SET_STENCIL,            OP_STENCIL },
    [T_STENCIL_MASK]= { "nvnCommandBufferSetStencilMask",        C_SET_STENCIL,            OP_STENCIL },
    [T_STENCIL_VMASK]={ "nvnCommandBufferSetStencilValueMask",   C_SET_STENCIL,            OP_STENCIL },
    [T_PRIMRESTART] = { "nvnCommandBufferSetPrimitiveRestart",   C_SET_PRIMITIVE_RESTART,  OP_PRIM_RESTART },
    [T_POLYOFF]     = { "nvnCommandBufferSetPolygonOffsetClamp", C_SET_POLYGON_OFFSET,     OP_POLY_OFFSET },
    [T_RENDEREN]    = { "nvnCommandBufferSetRenderEnable",       C_SET_RENDER_ENABLE,      -1 },   // profile 门控
    [T_CLEARCOLOR]  = { "nvnCommandBufferClearColor",            C_CLEAR_COLOR,            OP_CLEAR_COLOR },
    [T_CLEARCOLOR_I]= { "nvnCommandBufferClearColori",           C_CLEAR_COLOR,            OP_CLEAR_COLOR },
    [T_CLEARCOLOR_UI]={ "nvnCommandBufferClearColorui",          C_CLEAR_COLOR,            OP_CLEAR_COLOR },
    [T_CLEARDS]     = { "nvnCommandBufferClearDepthStencil",     C_CLEAR_DEPTH_STENCIL,    OP_CLEAR_DS },
    [T_CLEARTEX]    = { "nvnCommandBufferClearTexture",          C_CLEAR_TEXTURE,          OP_CLEAR_TEX },
    [T_CLEARTEX_I]  = { "nvnCommandBufferClearTexturei",         C_CLEAR_TEXTURE,          OP_CLEAR_TEX },
    [T_CLEARTEX_UI] = { "nvnCommandBufferClearTextureui",        C_CLEAR_TEXTURE,          OP_CLEAR_TEX },
    [T_CLEARBUF]    = { "nvnCommandBufferClearBuffer",           C_CLEAR_BUFFER,           OP_CLEAR_BUF },
    [T_BARRIER]     = { "nvnCommandBufferBarrier",               C_BARRIER,                OP_BARRIER },
    [T_DISPATCH]    = { "nvnCommandBufferDispatchCompute",       C_DISPATCH,               OP_DISPATCH },
    [T_DISPATCH_IND]= { "nvnCommandBufferDispatchComputeIndirect", C_DISPATCH_INDIRECT,    OP_DISPATCH },
    [T_COPYB2B]     = { "nvnCommandBufferCopyBufferToBuffer",    C_COPY_BUFFER_TO_BUFFER,  OP_COPY_B2B },
    [T_UPDUB]       = { "nvnCommandBufferUpdateUniformBuffer",   C_UPDATE_UNIFORM_BUFFER,  OP_UPDATE_UB },
    [T_COPYROW]     = { "nvnCommandBufferSetCopyRowStride",      C_SET_COPY_STRIDES,       OP_COPY_STRIDE },
    [T_COPYIMG]     = { "nvnCommandBufferSetCopyImageStride",    C_SET_COPY_STRIDES,       OP_COPY_STRIDE },
    [T_DEPTHCLAMP]  = { "nvnCommandBufferSetDepthClamp",         C_SET_DEPTH_CLAMP,        OP_DEPTH_CLAMP },
    /* ---- M44A / S2b-2 前置：执行链必需的 4 个 opcode ----------------
       nx2 侧的生产者都是"只写命令记录"的纯记录函数（hle_nvn.c:904-922），没有一个在录制期回写客体对象：
         · CopyCommands   只写 OP_CALL{target=handle}；
         · SignalEvent    只读 ev->val 再写 OP_SIGNAL_EVENT{val,mask,v}；
         · FenceSync       只写 OP_FENCE_SYNC{sync}（fence 值的赋值在 exec 侧 OP_FENCE_SYNC，不在录制侧）；
         · ReportCounter  只写 OP_REPORT_COUNTER{type,addr}（计数器落到 GPU 内存在 exec 侧）。
       所以镜像它们**不会**改变执行语义 —— 直喂侧的 CALL 仍然是"只记录、不重放"（call_commands 一行未改）。 */
    [T_CALL]          = { "nvnCommandBufferCopyCommands",        C_CALL,           OP_CALL },
    [T_SIGNAL_EVENT]  = { "nvnCommandBufferSignalEvent",         C_SIGNAL_EVENT,   OP_SIGNAL_EVENT },
    [T_FENCE_SYNC]    = { "nvnCommandBufferFenceSync",           C_FENCE_SYNC,     OP_FENCE_SYNC },
    [T_REPORT_COUNTER]= { "nvnCommandBufferReportCounter",       C_REPORT_COUNTER, OP_REPORT_COUNTER },
};

typedef void (*fn_gen)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef void (*fn_gen7)(void *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef void (*fn_f2)(void *, float, float);
typedef void (*fn_f3)(void *, float, float, float);
typedef void (*fn_cleards)(void *, float, uint32_t, int, int);

static fn_gen g_fn[T_COUNT];

// ============================ 落盘 ============================
static void flush_rows(void) {
    if (!g_ftsv || !g_nrow) { g_nrow = 0; return; }
    for (int i = 0; i < g_nrow; i++) {
        const m43b_row *r = &g_rows[i];
        fprintf(g_ftsv, "%u\t%u\t%s\t%u\t%d\t%d\t%s\t%u\t%u\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%u\n",
                r->frame, r->seq, g_ent[r->id].fn, r->id, g_ent[r->id].cmd, (int)r->rop,
                g_status_name[r->st], r->ncmp, r->nmis,
                (long long)r->d[0], (long long)r->d[1], (long long)r->d[2], (long long)r->d[3],
                (long long)r->r[0], (long long)r->r[1], (long long)r->r[2], (long long)r->r[3], r->cap);
        g_rows_total++;
    }
    fflush(g_ftsv);
    g_nrow = 0;
}

static void flush_frame(uint64_t frame) {
    flush_rows();
    if (g_fsum) {
        fprintf(g_fsum, "%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n", (unsigned long long)frame,
                (unsigned long long)(g_cnt[0] + g_cnt[1] + g_cnt[2] + g_cnt[3] + g_cnt[4]),
                (unsigned long long)g_cnt[0], (unsigned long long)g_cnt[1], (unsigned long long)g_cnt[2],
                (unsigned long long)g_cnt[3], (unsigned long long)g_cnt[4], (unsigned long long)g_cap_skip);
        fflush(g_fsum);
    }
    tap_log("frame %llu: total=%llu A=%llu B=%llu C_MISSING=%llu D_PARAM=%llu E_NOREC=%llu cap_skip=%llu",
            (unsigned long long)frame,
            (unsigned long long)(g_cnt[0] + g_cnt[1] + g_cnt[2] + g_cnt[3] + g_cnt[4]),
            (unsigned long long)g_cnt[0], (unsigned long long)g_cnt[1], (unsigned long long)g_cnt[2],
            (unsigned long long)g_cnt[3], (unsigned long long)g_cnt[4], (unsigned long long)g_cap_skip);
    for (int i = 0; i < M_STATUS_COUNT; i++) g_tot[i] += g_cnt[i];
    memset(g_cnt, 0, sizeof g_cnt);
    g_cap_skip = 0;
    g_frames_done++;
    if (g_frames_done >= M43B_MAX_FRAMES || g_rows_total >= M43B_MAX_ROWS) {
        /* 记录窗关闭的这一刻打全量计数器：与上面刚落盘的那些行覆盖**同一时间窗**，
           所以"每个 CMD_ 的计数器 == 该 opcode 的对账行数"是严格可比的口径（不需要等进程优雅退出）。 */
        { char line[1024]; int o = 0;
          for (int i = 0; i < C_COUNT; i++) if (g_cmd_seen[i] && o < (int)sizeof line - 32)
              o += snprintf(line + o, sizeof line - o, " %d=%llu", i, (unsigned long long)g_cmd_seen[i]);
          tap_log("WINDOW CLOSED after %d frames / %d rows; direct-side CMD_* occurrences:%s",
                  g_frames_done, g_rows_total, o ? line : " (none)");
          char path[256]; FILE *cs = files_open_write(path, sizeof path, "m43b_cmd_seen.tsv");
          if (cs) { fprintf(cs, "cmd\tcount\n");
                    for (int i = 0; i < C_COUNT; i++) fprintf(cs, "%d\t%llu\n", i, (unsigned long long)g_cmd_seen[i]);
                    fclose(cs); } }
        if (g_ftsv) { fclose(g_ftsv); g_ftsv = NULL; }
        if (g_fsum) { fclose(g_fsum); g_fsum = NULL; }
        tap_log("reconcile window closed after %d frames / %d rows; files flushed", g_frames_done, g_rows_total);
        g_ready = 0;
    }
}

// 帧边界：切帧时把上一帧一次性落盘，并重置私有录制缓冲（arena 逐帧复用，不会越用越少）。
static void maybe_roll(const char *why) {
    (void)why;
    uint64_t f = (uint64_t)atomic_load_explicit(&g_beat_frames, memory_order_relaxed);
    if (g_have_frame && f == g_cur_frame) return;
    if (g_have_frame) flush_frame(g_cur_frame);
    if (!g_ready) return;
    g_cur_frame = f; g_seq = 0; g_have_frame = 1;
    recorder_reset();
}

// ============================ 一行 ============================
static void emit(int id, int16_t rop, int nrec, int st, int ncmp, int nmis,
                 int64_t d0, int64_t d1, int64_t d2, int64_t d3,
                 int64_t r0, int64_t r1, int64_t r2, int64_t r3, int cap) {
    if (!g_ready) return;
    if (g_nrow >= M43B_ROWS_PER_FLUSH) flush_rows();   // mid-frame 落盘：计数继续累加，不丢行
    m43b_row *r = &g_rows[g_nrow++];
    r->frame = (uint32_t)g_cur_frame; r->seq = (uint32_t)g_seq++;
    r->id = (uint16_t)id; r->rop = rop; r->nrec = (uint16_t)nrec;
    r->st = (uint8_t)st; r->ncmp = (uint8_t)ncmp; r->nmis = (uint8_t)nmis; r->cap = (uint8_t)cap;
    r->d[0] = d0; r->d[1] = d1; r->d[2] = d2; r->d[3] = d3;
    r->r[0] = r0; r->r[1] = r1; r->r[2] = r2; r->r[3] = r3;
    g_cnt[st]++;
}

// 从 [from, cur) 里取第一条记录（op + payload 指针）与记录条数
typedef struct { int n; int op; const uint8_t *pl; uint32_t size; } rec_view;
static rec_view take_recs(uint8_t *from, uint8_t *to) {
    rec_view v = { 0, -1, NULL, 0 };
    uint8_t *p = from;
    while (p && to && p + 8 <= to) {
        const op_hdr *h = (const op_hdr *)p;
        if (!h->size || h->size > 0x1000 || p + h->size > to) break;
        if (v.n == 0) { v.op = (int)h->op; v.pl = p + 8; v.size = h->size >= 8 ? h->size - 8 : 0; }
        v.n++;
        p += h->size;
    }
    return v;
}

// 比较：把所有 A/B/D 的判定收敛到"want 与 got 的 ncmp 个字段"
static void judge(int id, const rec_view *v, int ncmp, const int64_t *want, const int64_t *got) {
    int nmis = 0;
    for (int i = 0; i < ncmp; i++) if (want[i] != got[i]) nmis++;
    int st;
    if (v->n == 0)                       st = (g_ent[id].op < 0) ? M_E_NOREC : M_C_MISS;
    else if (v->op != g_ent[id].op)      st = M_C_MISS;
    else if (nmis)                       st = M_D_PARAM;
    else                                 st = (v->n > 1) ? M_B_SEMEQ : M_A_ENUM;
    emit(id, (int16_t)v->op, v->n, st, ncmp, nmis,
         want[0], want[1], want[2], want[3], got[0], got[1], got[2], got[3], 0);
}

// ============================ 调用录制侧 ============================
static inline int room_ok(void) {
    if (!g_cb_obj.cur || !g_cb_obj.end) return 0;
    if ((size_t)(g_cb_obj.end - g_cb_obj.cur) >= 0x10000) return 1;
    g_cap_skip++;
    return 0;
}

// 按 id 调 nx2 实现。参数一律从客体寄存器 c->x / c->v 取（NVN 调用该有的那些值）。
static void call_nx(int id, const uint64_t *x, const float *f) {
    void *cb = &g_cb_obj;
    switch (id) {
    case T_DRAW_ARRAYS:      g_fn[id](cb, x[1], x[2], x[3], 0, 0, 0); break;
    case T_DRAW_ARRAYS_I:    g_fn[id](cb, x[1], x[2], x[3], x[4], x[5], 0); break;
    case T_DRAW_ELEMENTS:    g_fn[id](cb, x[1], x[2], x[3], x[4], 0, 0); break;
    case T_DRAW_ELEMENTS_BV: g_fn[id](cb, x[1], x[2], x[3], x[4], x[5], 0); break;
    case T_DRAW_ELEMENTS_I:  ((fn_gen7)g_fn[id])(cb, x[1], x[2], x[3], x[4], x[5], x[6], x[7]); break;
    case T_DRAW_ARRAYS_IND:  g_fn[id](cb, x[1], x[2], 0, 0, 0, 0); break;
    case T_DRAW_ELEMENTS_IND:g_fn[id](cb, x[1], x[2], x[3], 0, 0, 0); break;
    // 单参数形态：把单槽改写成 1 条的复数形态，录制侧载荷与复数形态逐字段一致
    case T_VBUF1: { NVNbufferRange r = { (NVNbufferAddress)x[2], (size_t)x[3] }; g_fn[T_VBUF](cb, x[1], 1, (uint64_t)(uintptr_t)&r, 0, 0, 0); break; }
    case T_UBO1:  { NVNbufferRange r = { (NVNbufferAddress)x[3], (size_t)x[4] }; g_fn[T_UBO](cb, x[1], x[2], 1, (uint64_t)(uintptr_t)&r, 0, 0); break; }
    case T_SSBO1: { NVNbufferRange r = { (NVNbufferAddress)x[3], (size_t)x[4] }; g_fn[T_SSBO](cb, x[1], x[2], 1, (uint64_t)(uintptr_t)&r, 0, 0); break; }
    case T_TEX1:  { uint64_t h = x[3]; g_fn[T_TEX](cb, x[1], x[2], 1, (uint64_t)(uintptr_t)&h, 0, 0); break; }
    case T_IMG1:  { uint64_t h = x[3]; g_fn[T_IMG](cb, x[1], x[2], 1, (uint64_t)(uintptr_t)&h, 0, 0); break; }
    case T_VIEWPORT1: { float v[4] = { (float)(int32_t)x[1], (float)(int32_t)x[2], (float)(int32_t)x[3], (float)(int32_t)x[4] };
                        g_fn[T_VIEWPORTS](cb, 0, 1, (uint64_t)(uintptr_t)v, 0, 0, 0); break; }
    case T_POLYOFF: ((fn_f3)g_fn[id])(cb, f[0], f[1], f[2]); break;
    case T_DRANGE1: ((fn_f2)g_fn[id])(cb, f[0], f[1]); break;
    /* OP_CALL：nx2 的 CopyCommands(cmd, int n, const NVNcommandHandle *h) 逐个句柄写一条 OP_CALL。
       直喂侧每写一条 CMD_CALL 就是"一个句柄"，所以这里恒以 n=1 调用，与直喂侧 1:1。 */
    case T_CALL: { uint64_t h = x[1]; ((void (*)(void *, int, const uint64_t *))g_fn[id])(cb, 1, &h); break; }
    case T_CLEARDS: ((fn_cleards)g_fn[id])(cb, f[0], (uint32_t)x[2], (int)x[3], (int)x[4]); break;
    default:        g_fn[id](cb, x[1], x[2], x[3], x[4], x[5], x[6]); break;
    }
}

// ============================ 比较的"期望值" ============================
// 直喂侧真值来自客体寄存器；录制侧真值从刚写下的记录载荷里**真读回来**（不是"我以为它写了什么"）。
static int64_t L64(const void *p) { int64_t v; memcpy(&v, p, 8); return v; }
static int64_t L32(const void *p) { int32_t v; memcpy(&v, p, 4); return (int64_t)v; }
static int64_t F32bits(float f) { uint32_t u; memcpy(&u, &f, 4); return (int64_t)(int64_t)(uint32_t)u; }

/* 状态类一笔：调录制侧 → 读回 → 比较 → 出一行。x = c->x（x[0]=cmd 句柄），f = c->v 前 4 个 float。 */
void m43b_cb_state(int id, const uint64_t *x, const float *f) {
    if (id < 0 || id >= T_COUNT) return;
    if (g_on < 0) {
        char p[256]; g_on = 1;
        FILE *fp = files_open_read(p, sizeof p, "DM_M43B_REC");
        if (fp) { int ch = fgetc(fp); fclose(fp); if (ch == '0') g_on = 0; }
    }
    if (!g_on || !g_ready) return;
    pthread_mutex_lock(&g_mu);
    maybe_roll("state");
    if (!g_ready) { pthread_mutex_unlock(&g_mu); return; }
    if (!g_fn[id]) { g_fn[id] = (fn_gen)sect_find(g_ent[id].fn); }
    if (!g_fn[id]) {   // 录制层没这个实现：明确记一行，不静默
        int64_t z[4] = {0,0,0,0};
        emit(id, -1, 0, M_C_MISS, 0, 0, x[1], x[2], x[3], x[4], z[0], z[1], z[2], z[3], 0);
        pthread_mutex_unlock(&g_mu);
        return;
    }
    if (!room_ok()) {
        emit(id, -1, 0, M_E_NOREC, 0, 0, 0,0,0,0, 0,0,0,0, 1);
        pthread_mutex_unlock(&g_mu);
        return;
    }

    uint8_t *at = g_cb_obj.cur;
    call_nx(id, x, f);
    rec_view v = take_recs(at, g_cb_obj.cur);
    const uint8_t *p = v.pl;

    int64_t want[4] = {0,0,0,0}, got[4] = {0,0,0,0};
    int ncmp = 2;
    switch (id) {
    case T_PROGRAM:
        /* 直喂侧：x1=program*, x2=stages。录制侧 p_bind_program{prog,stages}。
           prog 是 tex_snap 之外的裸指针：nx2 h_nvnCommandBufferBindProgram 直接存 prog 指针。 */
        want[0] = L64(&x[1]); want[1] = L32(&x[2]);
        if (p) { got[0] = L64((const uint8_t *)p + offsetof(p_bind_program, prog)); got[1] = L32((const uint8_t *)p + offsetof(p_bind_program, stages)); }
        break;
    case T_BLEND: case T_CHMASK: case T_COLOR: case T_DSST: case T_MS: case T_POLY: {
        /* 直喂侧：x1 = 状态结构指针（4..0x18 字节）。录制侧 p_bind_state{kind, bytes[0x18]}。
           kind 由调用点是哪一个函数决定（0 blend 1 color 2 chmask 3 ms 4 poly 5 ds）。 */
        /* 录制侧 p_bind_state.bytes 存的是 nx2 的 **packed 结构**（blend_st/color_st/chmask_st/ms_st/poly_st/ds_st），
           不是客体原字节。所以直喂侧的期望值也用同一 packed 类型就地重新编码后再比 —— 这是"同一份信息、
           两种表示"的严格对账，而不是拿原字节硬比（那会永远不相等）。 */
        static const int kind_of[6] = { 0 /*blend*/, 2 /*chmask*/, 1 /*color*/, 5 /*ds*/, 3 /*ms*/, 4 /*poly*/ };
        int k = (id == T_BLEND) ? 0 : (id == T_CHMASK) ? 1 : (id == T_COLOR) ? 2 : (id == T_DSST) ? 3 : (id == T_MS) ? 4 : 5;
        want[0] = kind_of[k];
        uint64_t mask = ~0ull;
        if (x[1]) {
            const void *g = (const void *)(uintptr_t)x[1];
            switch (k) {
            case 0: { blend_st t = *(const blend_st *)g; memcpy(&want[1], &t, sizeof t); if (t.adv) mask = 0xffull; break; }  // blend_adv 会重写，只比 blend_en
            case 1: { chmask_st t = *(const chmask_st *)g; memcpy(&want[1], &t, sizeof t); break; }
            case 2: { color_st t = *(const color_st *)g; memcpy(&want[1], &t, sizeof t); break; }
            case 3: { ms_st t = *(const ms_st *)g; memcpy(&want[1], &t, sizeof t); break; }
            case 4: { poly_st t = *(const poly_st *)g; memcpy(&want[1], &t, sizeof t); break; }
            default:{ ds_st t = *(const ds_st *)g; memcpy(&want[1], &t, sizeof t); break; }
            }
        }
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_bind_state, kind));
                 uint64_t b = (uint64_t)L64((const uint8_t *)p + offsetof(p_bind_state, bytes));
                 want[1] = (int64_t)((uint64_t)want[1] & mask); got[1] = (int64_t)(b & mask); }
        break;
    }
    case T_VATTR:
        /* nvnCommandBufferBindVertexAttribState(cmd, int count, const NVNvertexAttribState*)：first 恒 0 */
        want[0] = 0; want[1] = L32(&x[1]); want[2] = x[2] ? L32((const void *)(uintptr_t)x[2]) : 0;
        ncmp = 3;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_bind_vattr, first)); got[1] = L32((const uint8_t *)p + offsetof(p_bind_vattr, n)); got[2] = L32((const uint8_t *)p + offsetof(p_bind_vattr, st)); }
        break;
    case T_VSTREAM:
        /* nvnCommandBufferBindVertexStreamState(cmd, int count, const NVNvertexStreamState*)：first 恒 0；
           vs_st 是 8 字节 {stride, divisor}，所以首位按 64 位比 */
        want[0] = 0; want[1] = L32(&x[1]); want[2] = x[2] ? L64((const void *)(uintptr_t)x[2]) : 0;
        ncmp = 3;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_bind_vstream, first)); got[1] = L32((const uint8_t *)p + offsetof(p_bind_vstream, n)); got[2] = L64((const uint8_t *)p + offsetof(p_bind_vstream, st)); }
        break;
    case T_VBUF: case T_VBUF1: {
        const uint64_t *rg = (id == T_VBUF1) ? NULL : (const uint64_t *)(uintptr_t)x[3];
        want[0] = L32(&x[1]); want[1] = (id == T_VBUF1) ? 1 : L32(&x[2]);
        /* 单参数形态 NVNbufferRange **按值**传（x2=address x3=size），直接拿 x[2]，不解引用 */
        want[2] = (id == T_VBUF1) ? (int64_t)x[2] : (rg ? L64(rg) : 0);
        ncmp = 3;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_bind_vbuf, first)); got[1] = L32((const uint8_t *)p + offsetof(p_bind_vbuf, n)); got[2] = L64((const uint8_t *)p + offsetof(p_bind_vbuf, r)); }
        break;
    }
    case T_UBO: case T_UBO1: case T_SSBO: case T_SSBO1: {
        int single = (id == T_UBO1 || id == T_SSBO1);
        const uint64_t *rg = single ? NULL : (const uint64_t *)(uintptr_t)x[4];
        want[0] = L32(&x[1]); want[1] = single ? L32(&x[2]) : L32(&x[2]);
        want[2] = single ? x[3] : (rg ? L64(rg) : 0);
        ncmp = 3;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_bind_buf, stage)); got[1] = L32((const uint8_t *)p + offsetof(p_bind_buf, index)); got[2] = L64((const uint8_t *)p + offsetof(p_bind_buf, addr)); }
        break;
    }
    case T_TEX: case T_TEX1: case T_IMG: case T_IMG1: {
        int single = (id == T_TEX1 || id == T_IMG1);
        const uint64_t *h = single ? NULL : (const uint64_t *)(uintptr_t)x[4];
        want[0] = L32(&x[1]); want[1] = L32(&x[2]); want[2] = single ? L64(&x[3]) : (h ? L64(h) : 0);
        ncmp = 3;
        if (p) {
            p_bind_handles t; const p_bind_handles *b = bind_rec((uint32_t)v.op, p, &t);
            got[0] = L32(&b->stage); got[1] = L32(&b->first); got[2] = L64(&b->h[0]);
        }
        break;
    }
    case T_TEXPOOL: case T_SAMPPOOL:
        want[0] = L64(&x[1]);
        if (p) got[0] = L64((const uint8_t *)p + offsetof(p_set_pool, pool));
        ncmp = 1;
        /* 池绑定只用来产生 OP_SET_TEX_POOL / OP_SET_SAMP_POOL 记录；对账用的私有 CB 立刻把
           tex_base/samp_base 复位 0 —— 否则后续 BindTextures 的 handle 会被 bind_handles 按宿主池
           编号重编号，比较基准就不再是客体原值（真身执行时**不能**做这个复位）。 */
        g_cb_obj.tex_base = 0; g_cb_obj.samp_base = 0;
        break;
    case T_RT:
        want[0] = L32(&x[1]);
        want[1] = x[2] ? L64((const void *)(uintptr_t)x[2]) : 0;   // colors[0] 是游戏自己的 NVNtexture*（不是句柄槽）
        want[2] = L64(&x[4]);                                      // depth
        ncmp = 3;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_set_rt, n)); got[1] = L64((const uint8_t *)p + offsetof(p_set_rt, color)); got[2] = L64((const uint8_t *)p + offsetof(p_set_rt, depth)); }
        break;
    case T_VIEWPORTS: case T_VIEWPORT1:
        want[0] = (id == T_VIEWPORT1) ? 0 : L32(&x[1]);
        want[1] = (id == T_VIEWPORT1) ? 1 : L32(&x[2]);
        want[2] = (id == T_VIEWPORT1) ? F32bits((float)(int32_t)x[1]) : (x[3] ? F32bits(*(const float *)(uintptr_t)x[3]) : 0);
        ncmp = 3;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_viewports, first)); got[1] = L32((const uint8_t *)p + offsetof(p_viewports, n)); got[2] = L32((const uint8_t *)p + offsetof(p_viewports, v)); }
        break;
    case T_SCISSOR: case T_SCISSOR1:
        want[0] = (id == T_SCISSOR1) ? (int64_t)(int32_t)x[1] : (x[3] ? L32((const void *)(uintptr_t)x[3]) : 0);
        want[1] = (id == T_SCISSOR1) ? (int64_t)(int32_t)x[2] : (x[3] ? L32((const uint8_t *)(uintptr_t)x[3] + 4) : 0);
        want[2] = (id == T_SCISSOR1) ? (int64_t)(int32_t)x[3] : (x[3] ? L32((const uint8_t *)(uintptr_t)x[3] + 8) : 0);
        ncmp = 3;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_scissor, x)); got[1] = L32((const uint8_t *)p + offsetof(p_scissor, y)); got[2] = L32((const uint8_t *)p + offsetof(p_scissor, w)); }
        break;
    case T_DRANGE: case T_DRANGE1:
        want[0] = (id == T_DRANGE1) ? F32bits(f[0]) : (x[3] ? F32bits(*(const float *)(uintptr_t)x[3]) : 0);
        want[1] = (id == T_DRANGE1) ? F32bits(f[1]) : (x[3] ? F32bits(*((const float *)(uintptr_t)x[3] + 1)) : 0);
        ncmp = 2;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_depth_range, n)); got[1] = L32((const uint8_t *)p + offsetof(p_depth_range, f)); }
        break;
    case T_BLENDCOLOR:
        want[0] = x[1] ? L32((const void *)(uintptr_t)x[1]) : 0;
        want[1] = x[1] ? L32((const uint8_t *)(uintptr_t)x[1] + 4) : 0;
        ncmp = 2;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_blend_color, rgba)); got[1] = L32((const uint8_t *)p + offsetof(p_blend_color, rgba) + 4); }
        break;
    case T_STENCIL_REF: case T_STENCIL_MASK: case T_STENCIL_VMASK: {
        int which = (id == T_STENCIL_REF) ? 2 : (id == T_STENCIL_MASK) ? 1 : 0;
        want[0] = L32(&x[1]);          // face
        want[1] = which;
        want[2] = L32(&x[2]);          // 值
        ncmp = 3;
        if (p) {
            got[0] = L32((const uint8_t *)p + offsetof(p_stencil, face));
            got[1] = L32((const uint8_t *)p + offsetof(p_stencil, which));
            const size_t off = (which == 0) ? offsetof(p_stencil, value_mask) : (which == 1) ? offsetof(p_stencil, mask) : offsetof(p_stencil, ref);
            got[2] = L32((const uint8_t *)p + off);
        }
        break;
    }
    case T_PRIMRESTART:
        want[0] = (int64_t)((x[1] & 0xff) != 0); want[1] = L32(&x[2]);
        ncmp = 2;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_prim_restart, on)); got[1] = L32((const uint8_t *)p + offsetof(p_prim_restart, index)); }
        break;
    case T_POLYOFF:
        want[0] = F32bits(f[0]); want[1] = F32bits(f[1]); want[2] = F32bits(f[2]);
        ncmp = 3;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_poly_offset, factor)); got[1] = L32((const uint8_t *)p + offsetof(p_poly_offset, units)); got[2] = L32((const uint8_t *)p + offsetof(p_poly_offset, clamp)); }
        break;
    case T_RENDEREN:
        /* nx2 只在 DM_GAME_RENDER_COND 打开时才记 OP_RENDER_COND；关着就是 E_NOREC（g_ent.op = -1 已表达）。 */
        want[0] = (x[1] & 0xff) ? 1 : 0;
        if (p) got[0] = L32((const uint8_t *)p + offsetof(p_render_cond, mode));
        ncmp = 1;
        break;
    case T_CLEARCOLOR: case T_CLEARCOLOR_I: case T_CLEARCOLOR_UI:
        want[0] = L32(&x[1]);                       // index
        want[1] = L32(&x[3]);                       // mask
        want[2] = x[2] ? L32((const void *)(uintptr_t)x[2]) : 0;   // rgba[0] 的 32 位
        ncmp = 3;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_clear_color, index)); got[1] = L32((const uint8_t *)p + offsetof(p_clear_color, mask)); got[2] = L32((const uint8_t *)p + offsetof(p_clear_color, rgba)); }
        break;
    case T_CLEARDS:
        want[0] = F32bits(f[0]); want[1] = L32(&x[3]); want[2] = L32(&x[4]);
        ncmp = 3;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_clear_ds, depth)); got[1] = L32((const uint8_t *)p + offsetof(p_clear_ds, stencil)); got[2] = L32((const uint8_t *)p + offsetof(p_clear_ds, stencil_mask)); }
        break;
    case T_CLEARTEX: case T_CLEARTEX_I: case T_CLEARTEX_UI:
        want[0] = L32(&x[5]);                                        // mask
        want[1] = x[3] ? L32((const void *)(uintptr_t)x[3]) : 0;      // region.x
        want[2] = (id == T_CLEARTEX_UI) ? 1 : 0;                      // is_uint
        ncmp = 3;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_clear_tex, mask)); got[1] = L32((const uint8_t *)p + offsetof(p_clear_tex, r)); got[2] = (int64_t)(uint8_t)p[offsetof(p_clear_tex, is_uint)]; }
        break;
    case T_CLEARBUF:
        want[0] = L64(&x[1]); want[1] = L64(&x[2]); want[2] = L32(&x[3]);
        ncmp = 3;
        if (p) { got[0] = L64((const uint8_t *)p + offsetof(p_clear_buf, addr)); got[1] = L64((const uint8_t *)p + offsetof(p_clear_buf, size)); got[2] = L32((const uint8_t *)p + offsetof(p_clear_buf, value)); }
        break;
    case T_BARRIER:
        want[0] = L32(&x[1]);
        ncmp = 1;
        if (p) got[0] = L32(p);
        break;
    case T_DISPATCH:
        want[0] = L32(&x[1]); want[1] = L32(&x[2]); want[2] = L32(&x[3]);
        ncmp = 3;
        if (p) { got[0] = L32((const uint8_t *)p + offsetof(p_dispatch, x)); got[1] = L32((const uint8_t *)p + offsetof(p_dispatch, y)); got[2] = L32((const uint8_t *)p + offsetof(p_dispatch, z)); }
        break;
    case T_DISPATCH_IND:
        want[0] = L64(&x[1]);
        ncmp = 1;
        if (p) got[0] = L64((const uint8_t *)p + offsetof(p_dispatch, ind));
        break;
    case T_COPYB2B:
        want[0] = L64(&x[1]); want[1] = L64(&x[2]); want[2] = L64(&x[3]);
        ncmp = 3;
        if (p) { got[0] = L64((const uint8_t *)p + 0); got[1] = L64((const uint8_t *)p + 8); got[2] = L64((const uint8_t *)p + 16); }
        break;
    case T_UPDUB:
        /* nx2 记 {dst = buf + off, n}，所以直喂侧期望 dst = x1 + x3 */
        want[0] = (int64_t)(x[1] + x[3]); want[1] = L64(&x[4]);
        ncmp = 2;
        if (p) { got[0] = L64((const uint8_t *)p + 0); got[1] = L64((const uint8_t *)p + 8); }
        break;
    case T_COPYROW: case T_COPYIMG:
        want[0] = L64(&x[1]); want[1] = (id == T_COPYIMG) ? 1 : 0;
        ncmp = 2;
        if (p) { got[0] = L64((const uint8_t *)p + offsetof(p_copy_stride, stride)); got[1] = L32((const uint8_t *)p + offsetof(p_copy_stride, image)); }
        break;
    case T_DEPTHCLAMP:
        want[0] = (int64_t)((x[1] & 0xff) != 0);
        ncmp = 1;
        if (p) got[0] = L32((const uint8_t *)p + offsetof(p_depth_clamp, on));
        break;
    /* ---- M44A：执行链必需 4 个 opcode 的逐字段比较 ---- */
    case T_CALL:
        /* 直喂侧 x[1] = 被接进来的命令句柄（call_commands 每句柄一条 CMD_CALL）；
           录制侧 OP_CALL 的载荷就是目标句柄（hle_nvn.c:904 的 memcpy(p+8,&h[i],8)）。
           这里**只读**录制侧写下的 8 字节，不解析句柄内容 —— 保证"只记录、不重放"。 */
        want[0] = (int64_t)x[1];
        ncmp = 1;
        if (p) got[0] = L64(p);
        break;
    case T_SIGNAL_EVENT:
        /* 直喂侧参数：x1 = const NVNevent*，x2 = 信号模式，x5 = 值；
           录制侧载荷 p_bind_handles 之外的 {volatile uint32_t *val; uint32_t mask; uint32_t v;}：
           val 是客体 NVNevent 里 ev->val 指针（ev_t = {uint32 magic; volatile uint32 *val}，val 在 +8）。 */
        want[0] = x[1] ? L64((const void *)(uintptr_t)(x[1] + 8)) : 0;
        want[1] = L32(&x[2]);
        want[2] = L32(&x[5]);
        ncmp = 3;
        if (p) { got[0] = L64((const uint8_t *)p + 0); got[1] = L32((const uint8_t *)p + 8); got[2] = L32((const uint8_t *)p + 12); }
        break;
    case T_FENCE_SYNC:
        /* 录制侧只记 NVNsync* 指针（fence 值在 exec 侧算），所以裸指针逐值比较 */
        want[0] = (int64_t)x[1];
        ncmp = 1;
        if (p) got[0] = L64(p);
        break;
    case T_REPORT_COUNTER:
        /* 直喂侧 x1 = NVNcounterType，x2 = 计数器落点地址（非客体内存时直喂侧按设计清零并记一行日志 ——
           本探针沿用同一约定：cmdbuf.c 传进来的 tx[2] 是**清零后**的值，所以这条对账口径 = "直喂侧实际记下的东西"）。
           录制侧载荷 {uint64 t; uint64 a;} 24 字节。 */
        want[0] = L64(&x[1]);
        want[1] = L64(&x[2]);
        ncmp = 2;
        if (p) { got[0] = L64((const uint8_t *)p + 0); got[1] = L64((const uint8_t *)p + 8); }
        break;
    default:
        ncmp = 1;
        break;
    }
    judge(id, &v, ncmp, want, got);
    pthread_mutex_unlock(&g_mu);
}

/* ============================ draw 侧（M43A 原样保留） ============================ */
void m43a_cb_draw(int kind, uint32_t d_op, int prim, int first, int count, int itype, uint64_t ibuf,
                  int base_v, int base_i, int inst) {
    if (g_on < 0) {
        char p[256]; g_on = 1;
        FILE *fp = files_open_read(p, sizeof p, "DM_M43B_REC");
        if (fp) { int ch = fgetc(fp); fclose(fp); if (ch == '0') g_on = 0; }
    }
    if (!g_on || !g_ready) return;
    pthread_mutex_lock(&g_mu);
    maybe_roll("draw");
    if (!g_ready) { pthread_mutex_unlock(&g_mu); return; }
    int id = (kind == 0) ? T_DRAW_ARRAYS : (kind == 1) ? T_DRAW_ARRAYS_I : (kind == 2) ? T_DRAW_ELEMENTS
           : (kind == 3) ? T_DRAW_ELEMENTS_BV : (kind == 4) ? T_DRAW_ELEMENTS_I
           : (kind == 5) ? T_DRAW_ARRAYS_IND : T_DRAW_ELEMENTS_IND;
    if (!g_fn[id]) g_fn[id] = (fn_gen)sect_find(g_ent[id].fn);
    if (!g_fn[id]) { emit(id, -1, 0, M_C_MISS, 0, 0, d_op, prim, count, inst, 0,0,0,0, 0); pthread_mutex_unlock(&g_mu); return; }
    if (!room_ok()) { emit(id, -1, 0, M_E_NOREC, 0, 0, 0,0,0,0, 0,0,0,0, 1); pthread_mutex_unlock(&g_mu); return; }

    /* 直喂侧 7 种 draw 记录器 → nx2 的 7 个 Draw* 函数，参数顺序**逐个按各自签名摆**（M43B 修：
       原实现只在 kind==0 摆了 first，arrays_inst 的 base_i/instances 与 elements_inst 的 instances 会错位）。 */
    uint64_t x[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    switch (kind) {
    case 0: x[1] = (uint64_t)(int64_t)prim; x[2] = (uint64_t)(int64_t)first; x[3] = (uint64_t)(int64_t)count; break;
    case 1: x[1] = (uint64_t)(int64_t)prim; x[2] = (uint64_t)(int64_t)first; x[3] = (uint64_t)(int64_t)count;
            x[4] = (uint64_t)(int64_t)base_i; x[5] = (uint64_t)(int64_t)inst; break;
    case 2: x[1] = (uint64_t)(int64_t)prim; x[2] = (uint64_t)(int64_t)itype; x[3] = (uint64_t)(int64_t)count; x[4] = ibuf; break;
    case 3: x[1] = (uint64_t)(int64_t)prim; x[2] = (uint64_t)(int64_t)itype; x[3] = (uint64_t)(int64_t)count; x[4] = ibuf;
            x[5] = (uint64_t)(int64_t)base_v; break;
    case 4: x[1] = (uint64_t)(int64_t)prim; x[2] = (uint64_t)(int64_t)itype; x[3] = (uint64_t)(int64_t)count; x[4] = ibuf;
            x[5] = (uint64_t)(int64_t)base_v; x[6] = (uint64_t)(int64_t)base_i; x[7] = (uint64_t)(int64_t)inst; break;
    case 5: x[1] = (uint64_t)(int64_t)prim; x[2] = ibuf; break;                                    /* DrawArraysIndirect(pr, ind) */
    default:x[1] = (uint64_t)(int64_t)prim; x[2] = (uint64_t)(int64_t)itype; x[3] = ibuf; break;    /* DrawElementsIndirect(pr, it, ind) */
    }
    float f[4] = { 0, 0, 0, 0 };
    uint8_t *at = g_cb_obj.cur;
    call_nx(id, x, f);
    rec_view v = take_recs(at, g_cb_obj.cur);

    int64_t want[4] = { prim, count, (int64_t)inst, (int64_t)itype }, got[4] = {0,0,0,0};
    if (v.pl) {
        const op_hdr *h = (const op_hdr *)(v.pl - 8);
        if (v.op == OP_DRAW) { p_draw t; const p_draw *d = draw_rec(v.pl, h->size, &t);
            got[0] = d->prim; got[1] = d->count; got[2] = d->instances ? d->instances : 1; got[3] = 0; }
        else if (v.op == OP_DRAW_IDX) { p_draw_idx t; const p_draw_idx *d = draw_idx_rec(v.pl, h->size, &t);
            got[0] = d->prim; got[1] = d->count; got[2] = d->instances ? d->instances : 1; got[3] = d->itype; }
    }
    int ncmp = 3 + (kind == 2 || kind == 3 || kind == 4);
    judge(id, &v, ncmp, want, got);
    pthread_mutex_unlock(&g_mu);
}

/* ============================ 初始化 / 收尾 ============================ */
/* M44A：把两个开关文件 seed 成 0666 空文件（el2 沙箱目录 shell 建不了文件，只能由应用自己建；
   与 nvn_vk2/selftest_tri.c 的 vk2_seed_switches 同一约定）。之后 run 脚本就能
   `hdc shell "echo 1 > .../DM_M44A_BIGSTACK_TEST"` 打开大栈自检、`echo 0 > .../DM_M43B_REC` 关探针。
   注意 O_CREAT|O_WRONLY **不截断**：已存在的开关内容不被覆盖。 */
__attribute__((constructor(150))) static void m44a_seed_switches(void) {
    /* M44B：DM_M44B_NODRAW / DM_M44B_PIPE_PACE 也必须由应用先 seed —— 实测 hdc shell 在 el2 目录
       **只能写已存在的 0666 文件、不能新建**（`echo 1 > .../DM_M44B_NODRAW` = Permission denied），
       而 hdc shell 不把内部失败反映成退出码，M44B 的第一版 run 脚本因此误报"knob 已写"。 */
    static const char *const names[] = { "DM_M43B_REC", "DM_M44A_BIGSTACK_TEST",
                                        "DM_M44B_NODRAW", "DM_M44B_PIPE_PACE" };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
        for (int d = 0; d < M43B_DIRS; d++) {
            char p[300];
            snprintf(p, sizeof p, "%s%s", g_dirs[d], names[i]);
            int fd = open(p, O_CREAT | O_WRONLY, 0666);
            if (fd >= 0) { close(fd); chmod(p, 0666); }
        }
}
__attribute__((constructor(200))) static void m43b_start(void) {
    char p[256];
#ifdef M44A_PROBE_OFF
    /* M44A：probe-OFF 对照构建 —— 探针整体不启动（不建 cb、不分配 arena、不开文件）。
       这个构建只用于崩溃归因的对照实验，与 ON 构建的唯一差别就是探针 + 挂点。 */
    (void)p;
    return;
#endif
    /* M44A：dm_bigstack_enter（512 KB 大栈）自检 —— 文件门控、跑在该 constructor 线程上、**不碰执行路径**。
       放在开关判断之前：探针关掉也应该能验收大栈桩。 */
    {
        FILE *bf = files_open_read(p, sizeof p, "DM_M44A_BIGSTACK_TEST");
        if (bf) { int ch = fgetc(bf); fclose(bf); if (ch == '1') dm_bigstack_selftest_async(); }
    }
    /* 开关 + 私有录制缓冲的初始化（nvnCommandBufferInitialize / AddCommandMemory / BeginRecording 三个原语
       也从 swnvn 段取，取不到就整体关闭并明确记一行） */
    g_on = 1;
    {
        FILE *fp = files_open_read(p, sizeof p, "DM_M43B_REC");
        if (fp) { int ch = fgetc(fp); fclose(fp); if (ch == '0') g_on = 0; }
    }
    if (!g_on) return;
    if (!__start_swnvn || !__stop_swnvn) { tap_log("X swnvn section missing -> recording OFF"); g_on = 0; return; }
    void *f_init = sect_find("nvnCommandBufferInitialize");
    void *f_add  = sect_find("nvnCommandBufferAddCommandMemory");
    void *f_begin= sect_find("nvnCommandBufferBeginRecording");
    for (int i = 0; i < T_COUNT; i++) g_fn[i] = (fn_gen)sect_find(g_ent[i].fn);
    if (!f_init || !f_add || !f_begin) {
        tap_log("X swnvn lookup failed (init=%p addmem=%p begin=%p) -> recording OFF", f_init, f_add, f_begin);
        g_on = 0; return;
    }
    static NVNdevice fake_dev;
    g_pool.magic = 0x504f4f4c; g_pool.flags = 0;
    g_pool.mem = g_arena; g_pool.size = sizeof g_arena;
    if (!((uint32_t (*)(void *, void *))f_init)((void *)&g_cb_obj, &fake_dev)) { tap_log("X nvnCommandBufferInitialize refused -> recording OFF"); g_on = 0; return; }
    ((void (*)(void *, const void *, ptrdiff_t, size_t))f_add)((void *)&g_cb_obj, (const void *)&g_pool, 0, (size_t)sizeof g_arena);
    ((void (*)(void *))f_begin)((void *)&g_cb_obj);
    if (!g_cb_obj.cur || !g_cb_obj.end) { tap_log("X recording buffer has no segment -> recording OFF"); g_on = 0; return; }
    g_seg_start = g_cb_obj.cur;
    g_cur_frame = (uint64_t)atomic_load(&g_beat_frames);
    g_ftsv = files_open_write(g_paths[0], sizeof g_paths[0], "m43b_frame.tsv");
    g_fsum = files_open_write(g_paths[1], sizeof g_paths[1], "m43b_frame_summary.tsv");
    if (!g_ftsv || !g_fsum) { tap_log("X cannot open output files -> recording OFF"); if (g_ftsv) fclose(g_ftsv); if (g_fsum) fclose(g_fsum); g_ftsv = g_fsum = NULL; g_on = 0; return; }
    fprintf(g_ftsv, "frame\tseq\tnx_fn\tid\td_cmd\tr_op\tstatus\tncmp\tnmis\td0\td1\td2\td3\tr0\tr1\tr2\tr3\tcap_skip\n");
    fprintf(g_fsum, "frame\ttotal\tA_enumnum\tB_semeq\tC_real_missing\tD_param_mismatch\tE_norec\tcap_skip\n");
    g_ready = 1;
    int nfn = 0; for (int i = 0; i < T_COUNT; i++) if (g_fn[i]) nfn++;
    tap_log("recording side ON (M43B): %d/%d families resolved, arena=%u bytes, cb=%p seg=[%p,%p)",
            nfn, T_COUNT, (unsigned)sizeof g_arena, (void *)&g_cb_obj, (void *)g_cb_obj.cur, (void *)g_cb_obj.end);
}

__attribute__((destructor)) static void m43b_stop(void) {
    if (!g_ready) return;
    if (g_have_frame) flush_frame(g_cur_frame);
    for (int i = 0; i < M_STATUS_COUNT; i++) g_tot[i] += 0;
    tap_log("done: frames=%d rows=%d A=%llu B=%llu C_MISSING=%llu D_PARAM=%llu E_NOREC=%llu",
            g_frames_done, g_rows_total,
            (unsigned long long)g_tot[0], (unsigned long long)g_tot[1], (unsigned long long)g_tot[2],
            (unsigned long long)g_tot[3], (unsigned long long)g_tot[4]);
    {   // 直喂侧 CMD_* 出现次数（由每笔 tap 累加）：覆盖矩阵里"未镜像"的 opcode 若这里非 0 就必须解释
        char line[1024]; int o = 0;
        for (int i = 0; i < C_COUNT; i++) if (g_cmd_seen[i] && o < (int)sizeof line - 32)
            o += snprintf(line + o, sizeof line - o, " %d=%llu", i, (unsigned long long)g_cmd_seen[i]);
        tap_log("direct-side CMD_* occurrences:%s", o ? line : " (none)");
    }
    if (g_ftsv) fclose(g_ftsv);
    if (g_fsum) fclose(g_fsum);
    g_ftsv = g_fsum = NULL;
    g_ready = 0;
}
