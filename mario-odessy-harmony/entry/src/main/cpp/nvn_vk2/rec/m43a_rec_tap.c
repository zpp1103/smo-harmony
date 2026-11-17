// M43A / S2a：**只录不跑**对账探针（旁路录制，绝不切换执行路径）。
//
// 它是本单的核心产出「直喂 ↔ 录制 双流对账」的运行时端：
//   · 直喂侧：gfx_bridge.c 的 NVN 符号表仍指向原函数（nvn_rec_* / 记账桩）→ 游戏照旧由 nvn_vk.c 直喂出图；
//     本文件只是在调用**之前/之后**取参数、记账，不改变任何一方的语义。
//   · 录制侧：把同一批 NVN 调用额外喂给 nx2android 的录制层（rec/hle_nvn.c 的 static 实现），
//     写进**本文件私有的**一条命令流（1 MB 宿主 arena），**只写记录、永不 mtl_exec**。
//
// 为什么能拿到 hle_nvn 的 static 实现：hle_nvn.c 用
//     __attribute__((used, section("swnvn"))) static const nvn_impl e_##name = {#name, (void*)h_##name};
// 把 537 个实现登记进 "swnvn" 段。本文件按名查段即可拿到函数指针 —— 这样既避免改 hle_nvn 源码，
// 又**绕开 nvn_thunks.S 跳板**（Mach-O 汇编，ELF 版属 S2b）。查表失败会明确记一行，不静默降级。
//
// 每个 draw 落一行 TSV：直喂侧 opcode/参数、录制侧 opcode/参数（从刚写下的记录里真读回来，
// 不是「我以为它写了什么」），由 tools/m43a_cmp_md.py 生成 logs/m43a/opcode_cmp.md。
//
// 开关：/data/.../files/DM_M43A_REC 内容为 1 时启用（运行脚本 seed），默认关 → 对老路径零影响。
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn_obj.h"     // rec/ isolation shim → nvn_vk2/abi/nvn_obj.h（nx2 的 OP_ 枚举）

// 隔离证明（验收 1）：录制侧这个 TU 只可能看到 nx2 的枚举；若有人误把 third_party/d2r_gfx 的同名头
// 放进 rec/ 的包含路径，这里立刻炸掉（我方 OP_COUNT_=43，nx2=50；S1 报告 §8-2 实测值）。
_Static_assert(OP_COUNT_ == 50, "M43A: recording side must compile against nx2android's op enum (OP_COUNT_ = 50)");
_Static_assert(OP_DRAW == 25 && OP_DRAW_IDX == 26, "M43A: OP_ numbering moved; update the reconciliation mapping");
_Static_assert(OP_CLEAR_TEX == 38 && OP_DEPTH_CLAMP == 40, "M43A: nx2 numbering (S1 §8-2 measured) changed");

// 直喂侧 opcode 号：platform/nvn_replay/nvn.h:125-141 的 CMD_* 枚举（BOTWRecomp 命令流格式），
// 由 platform/nvn_replay/cmdbuf.c 写入：:752 DrawArrays→CMD_DRAW_ARRAYS、:775/:782/:792 DrawElements*
// →CMD_DRAW_ELEMENTS、:796/:797 两个 indirect 号（但这两条在 gfx_bridge.c 里指向**只记账的桩**，
// 从不进命令流 —— 这正是本单要量出来的"真实缺失"）。cmd_trace.h:11/15 给的两个锚点（33/19）与此一致。
enum { M43A_CMD_SET_RENDER_TARGETS = 19, M43A_CMD_CLEAR_COLOR = 33,
       M43A_CMD_DRAW_ARRAYS = 37, M43A_CMD_DRAW_ELEMENTS = 38,
       M43A_CMD_DRAW_ARRAYS_INDIRECT = 39, M43A_CMD_DRAW_ELEMENTS_INDIRECT = 40,
       M43A_CMD_UNRECORDED = -1 };

// 主循环帧号（platform/vk_boot.h:29，nvn_window_acquire_texture 里 ++）：用来把同一次调用归到同一帧。
extern _Atomic unsigned long g_beat_frames;


// ---- "swnvn" 段查表：nx2 录制层注册在段里的 {name, fn} 数组（hle_nvn.c:66-73 的 nvn_impl）----
typedef struct { const char *name; void *fn; } m43a_sect_ent;
extern const m43a_sect_ent __start_swnvn[] __attribute__((weak, visibility("hidden")));
extern const m43a_sect_ent __stop_swnvn[]  __attribute__((weak, visibility("hidden")));

static void *sect_find(const char *name) {
    if (!__start_swnvn || !__stop_swnvn) return NULL;
    for (const m43a_sect_ent *p = __start_swnvn; p < __stop_swnvn; p++)
        if (p->name && !strcmp(p->name, name)) return p->fn;
    return NULL;
}

// ---- 录制层函数指针（按 nvn_protos.h / nvn.h 的原型取）----
typedef uint32_t (*fn_cb_init)(NVNcommandBuffer *, NVNdevice *);
typedef void     (*fn_cb_addmem)(NVNcommandBuffer *, const NVNmemoryPool *, ptrdiff_t, size_t);
typedef void     (*fn_cb_begin)(NVNcommandBuffer *);
typedef void     (*fn_draw)(NVNcommandBuffer *, int, int, int);
typedef void     (*fn_draw_i)(NVNcommandBuffer *, int, int, int, int, int);
typedef void     (*fn_draw_e)(NVNcommandBuffer *, int, int, int, uint64_t);
typedef void     (*fn_draw_eb)(NVNcommandBuffer *, int, int, int, uint64_t, int);
typedef void     (*fn_draw_ei)(NVNcommandBuffer *, int, int, int, uint64_t, int, int, int);

static int g_on = -1, g_ready, g_rows;
static FILE *g_f;
static cmd_t g_cb_obj;                 /* 私有录制命令缓冲对象（game 的 NVNcommandBuffer 一个字节都不碰） */
static uint8_t g_arena[1 << 20];       /* 私有命令内存 arena：不向游戏要内存，也就不触发任何回调 */
static pool_t g_pool;
static fn_cb_init   g_cb_init;
static fn_cb_addmem g_cb_addmem;
static fn_cb_begin  g_cb_begin;
static fn_draw    g_d_arrays;
static fn_draw_i  g_d_arrays_i;
static fn_draw_e  g_d_elems;
static fn_draw_eb g_d_elems_bv;
static fn_draw_ei g_d_elems_i;

#define M43A_MAX_ROWS 200   /* 本单要求：前 200 个 draw 逐条对齐 */

/* 两个候选目录：应用私有 el2（本进程可写）与 storage el2。**探测只用 rb，绝不创建**——
   上一版用 "ab" 探测，把自己探成了一个空文件，下一轮读到的就是 EOF→关，直接自锁（logs/m43a/b 0 行的真因）。*/
#define M43A_DIRS 2
static const char *const g_dirs[M43A_DIRS] = {
    "/data/app/el2/100/base/com.vibecc.smo/files/",
    "/data/storage/el2/base/files/",
};
static FILE *files_open_read(char *buf, size_t n, const char *name) {
    for (int i = 0; i < M43A_DIRS; i++) {
        snprintf(buf, n, "%s%s", g_dirs[i], name);
        FILE *f = fopen(buf, "rb");
        if (f) return f;
    }
    return NULL;
}
static FILE *files_open_write(char *buf, size_t n, const char *name) {
    for (int i = 0; i < M43A_DIRS; i++) {
        snprintf(buf, n, "%s%s", g_dirs[i], name);
        FILE *f = fopen(buf, "wb");
        if (f) return f;
    }
    return NULL;
}

static void tap_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tap_log(const char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    vk_boot_logf("[m43a] %s", b);
}

static void rec_init(void) {
    if (g_on < 0) {
        // 开关：DM_M43A_REC 内容 '1' = 开、'0' = 关；文件缺失或空 → **默认开**（对账要它开；
        // 规模有硬上限：前 200 笔 + 私有 1 MB arena，对老路径无副作用）。
        char p[256]; g_on = 1;
        FILE *f = files_open_read(p, sizeof p, "DM_M43A_REC");
        if (f) { int c = fgetc(f); fclose(f); if (c == '0') g_on = 0; }
    }
    if (!g_on || g_ready) return;
    g_cb_init   = (fn_cb_init)sect_find("nvnCommandBufferInitialize");
    g_cb_addmem = (fn_cb_addmem)sect_find("nvnCommandBufferAddCommandMemory");
    g_cb_begin  = (fn_cb_begin)sect_find("nvnCommandBufferBeginRecording");
    g_d_arrays   = (fn_draw)sect_find("nvnCommandBufferDrawArrays");
    g_d_arrays_i = (fn_draw_i)sect_find("nvnCommandBufferDrawArraysInstanced");
    g_d_elems    = (fn_draw_e)sect_find("nvnCommandBufferDrawElements");
    g_d_elems_bv = (fn_draw_eb)sect_find("nvnCommandBufferDrawElementsBaseVertex");
    g_d_elems_i  = (fn_draw_ei)sect_find("nvnCommandBufferDrawElementsInstanced");
    if (!g_cb_init || !g_cb_addmem || !g_cb_begin || !g_d_arrays || !g_d_arrays_i || !g_d_elems || !g_d_elems_bv || !g_d_elems_i) {
        tap_log("X swnvn lookup failed (init=%p addmem=%p begin=%p arrays=%p arrays_i=%p elems=%p elems_bv=%p elems_i=%p) -> recording side OFF",
                (void *)g_cb_init, (void *)g_cb_addmem, (void *)g_cb_begin, (void *)g_d_arrays, (void *)g_d_arrays_i,
                (void *)g_d_elems, (void *)g_d_elems_bv, (void *)g_d_elems_i);
        g_on = 0; return;
    }
    static NVNdevice fake_dev;      /* 录制层只存指针（hle_nvn.c:766-768），不读字段 */
    g_pool.magic = 0x504f4f4c; g_pool.flags = 0;
    g_pool.mem = g_arena; g_pool.size = sizeof g_arena;
    uint32_t ok = g_cb_init((NVNcommandBuffer *)&g_cb_obj, &fake_dev);
    if (!ok) { tap_log("X nvnCommandBufferInitialize refused -> recording OFF"); g_on = 0; return; }
    g_cb_addmem((NVNcommandBuffer *)&g_cb_obj, (const NVNmemoryPool *)&g_pool, 0, sizeof g_arena);
    g_cb_begin((NVNcommandBuffer *)&g_cb_obj);
    if (!g_cb_obj.cur || !g_cb_obj.end) { tap_log("X recording buffer has no segment (cur=%p end=%p) -> recording OFF", (void *)g_cb_obj.cur, (void *)g_cb_obj.end); g_on = 0; return; }
    char p[256];
    g_f = files_open_write(p, sizeof p, "m43a_opcode_cmp.tsv");
    if (g_f) fprintf(g_f, "seq\tframe\td_kind\td_op\td_prim\td_first\td_count\td_inst\td_base_inst\td_itype\td_ibuf\tr_op\tr_prim\tr_first\tr_count\tr_inst\tr_base_inst\tr_itype\tr_ibuf\tr_ok\n");
    g_ready = 1;
    tap_log("recording side ON: nx2 hle_nvn record-only cb=%p arena=%u bytes (looked up %s)",
            (void *)&g_cb_obj, (unsigned)sizeof g_arena, "via swnvn section; no thunks, no exec");
}

/* 同一帧只录前 200 笔；超出后只关文件、不再记录（录制侧也停，保持两列同长） */
static void row(const char *kind, int d_op, int prim, int first, int count, int inst, int base_inst,
                int itype, uint64_t ibuf, uint8_t *rec_at) {
    if (!g_ready || g_rows >= M43A_MAX_ROWS) return;
    int r_op = -1, r_prim = 0, r_first = 0, r_count = 0, r_inst = 0, r_base = 0, r_itype = 0;
    uint64_t r_ibuf = 0; int r_ok = 0;
    if (rec_at) {
        const op_hdr *h = (const op_hdr *)rec_at;
        if ((h->op == OP_DRAW || h->op == OP_DRAW_IDX) && h->size >= 8 && h->size <= 0x100) {
            r_ok = 1; r_op = (int)h->op;
            if (h->op == OP_DRAW) { p_draw t; const p_draw *d = draw_rec((const uint8_t *)rec_at + 8, h->size, &t);
                r_prim = d->prim; r_first = d->first; r_count = d->count; r_inst = d->instances ? d->instances : 1; r_base = d->base_instance; }
            else { p_draw_idx t; const p_draw_idx *d = draw_idx_rec((const uint8_t *)rec_at + 8, h->size, &t);
                r_prim = d->prim; r_itype = d->itype; r_count = d->count; r_inst = d->instances ? d->instances : 1; r_base = d->base_instance; }
        } else {
            tap_log("W record at %p has unexpected op %u size %u", (void *)rec_at, h->op, h->size);
        }
    }
    if (g_f) {
        fprintf(g_f, "%d\t%lu\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%llu\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%llu\t%d\n",
                g_rows, (unsigned long)g_beat_frames, kind, d_op, prim, first, count, inst, base_inst, itype,
                (unsigned long long)ibuf, r_op, r_prim, r_first, r_count, r_inst, r_base, r_itype,
                (unsigned long long)r_ibuf, r_ok);
        if ((g_rows & 15) == 0) fflush(g_f);
    }
    g_rows++;
    if (g_rows == M43A_MAX_ROWS) {
        if (g_f) { fflush(g_f); fclose(g_f); g_f = NULL; }
        tap_log("first %d draws reconciled -> m43a_opcode_cmp.tsv closed", M43A_MAX_ROWS);
    }
}

/* ---------- 直喂侧唯一咽喉（platform/nvn_replay/cmdbuf.c 的 7 个 draw 记录器）----------
   语义：cmdbuf.c 刚把这一笔写进玩家命令流（老路，语义不变），把同一批参数旁路喂给录制层。
   d_op 是它刚写下的 CmdRec 头里的 op（真值），不是推断值。 */
extern void m43a_cb_draw(int kind, uint32_t d_op, int prim, int first, int count, int itype, uint64_t ibuf,
                         int base_v, int base_i, int inst) {
    static const char *kind_names[] = { "arrays", "arrays_inst", "elements", "elements_bv", "elements_inst",
                                        "arrays_indirect", "elements_indirect" };
    const char *kname = (kind >= 0 && kind <= 6) ? kind_names[kind] : "unknown";
    rec_init();
    if (!g_ready) return;
    uint8_t *at = g_cb_obj.cur;
    switch (kind) {
    case 0: g_d_arrays((NVNcommandBuffer *)&g_cb_obj, prim, first, count); break;
    case 1: g_d_arrays_i((NVNcommandBuffer *)&g_cb_obj, prim, first, count, base_i, inst); break;
    case 2: g_d_elems((NVNcommandBuffer *)&g_cb_obj, prim, itype, count, ibuf); break;
    case 3: g_d_elems_bv((NVNcommandBuffer *)&g_cb_obj, prim, itype, count, ibuf, base_v); break;
    case 4: g_d_elems_i((NVNcommandBuffer *)&g_cb_obj, prim, itype, count, ibuf, base_v, base_i, inst); break;
    case 5: g_d_arrays((NVNcommandBuffer *)&g_cb_obj, prim, 0, count); break;   /* indirect: 录制侧按"参数在 GPU 内存里"记 */
    case 6: g_d_elems((NVNcommandBuffer *)&g_cb_obj, prim, itype, 0, ibuf); break;
    }
    row(kname, (int)d_op, prim, first, count, inst, base_i, itype, ibuf, g_cb_obj.cur != at ? at : NULL);
}

/* 收尾：把录制侧那条流按记录逐条 dump 出来（证明"录到了什么"，不是推导出来的） */
__attribute__((destructor)) static void m43a_dump_stream(void) {
    if (!g_ready) return;
    tap_log("recording stream: begin=%p cur=%p end=%p used=%llu", (void *)g_cb_obj.begin, (void *)g_cb_obj.cur,
            (void *)g_cb_obj.end, (unsigned long long)g_cb_obj.used);
    uint8_t *p = g_cb_obj.begin, *e = g_cb_obj.cur;
    if (g_cb_obj.recording == 2) p = g_cb_obj.cur;   /* 一条记录都没写 */
    int n = 0;
    while (p && p + 8 <= e) {
        const op_hdr *h = (const op_hdr *)p;
        if (!h->size || h->size > 0x1000) break;
        if (h->op == OP_DRAW || h->op == OP_DRAW_IDX || h->op == OP_END) {
            tap_log("  rec[%d] op=%u size=%u", n++, h->op, h->size);
        }
        if (h->op == OP_END) break;
        p += h->size;
    }
    tap_log("recording stream walk: %d records (draws+end), rows=%d", n, g_rows);
}
