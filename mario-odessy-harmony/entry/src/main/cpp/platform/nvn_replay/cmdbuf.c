// NVN command buffers: commands are recorded as CmdRec records into a host shadow of the game's own memory, so the
// game's memory management and out-of-memory callbacks work as designed.
//
// Where: in NVN a command handle refers to the list in *control* memory (nvnCommandBufferAddControlMemory), and that
// is the memory whose lifetime is the list's: BOTW gives a small command buffer its own control memory per object
// (kept as long as the list is called) but the same scratch command memory every time. So records go to the control
// memory when the command buffer has some, else to its command memory. Command memory then is only accounted.
//
// How much: our records are bigger than NVN's own tokens, which the game sizes its memory for, so each byte of the
// game's memory stands for SHADOW_K bytes of a host shadow of it (shadow_at: by 64 KB page of guest address). A
// command buffer reports its use in the game's bytes (ours / SHADOW_K); when the game reuses a piece of its memory,
// the shadow of that piece is reused too. A list never crosses a shadow page in one piece: a jump continues it.
// A command handle is the (host) address of a list's first record.
#include <pthread.h>
#include <stddef.h>   /* M4q P1：offsetof（RtRec 与 cmd_trace.h 的偏移对账） */
#include <stdlib.h>
#include <string.h>

#include "nvn.h"
#include "cmd_trace.h"   /* M4q P1：NVN_CMD_RT_FIRST_TEX_OFF / NVN_CMD_RT_DEPTH_OFF（录制/回放偏移单一来源） */

#define SHADOW_K 8                                                  // shadow bytes per byte of the game's memory
#define SHADOW_PAGE 0x10000ull                                      // (game bytes)
#define JUMP_RESERVE ((sizeof(CmdRec) + SHADOW_K - 1) / SHADOW_K)  // (game bytes) the chaining jump at a chunk's end

/* ---- M43B / S2b-1：状态类镜像挂点（探针在 nvn_vk2/rec/m43b_rec_tap.c）
   M43B_TAP(id) 写在每个记录器**写完记录之后**：此时 c->x / c->v 仍是这一笔 NVN 调用的原参数，
   r->x / r->extra 也已补齐。**不能**把挂点放进 record()：那里看不到"单参数 vs 多参数"变体，
   也看不到 record() 之后才回填的字段（Stencil 的 which、ClearColor 的变体…），会造出假的参数不符。
   命令族 id 的唯一事实源是 nvn_vk2/rec/m43b_tap_ids.h（两个隔离编译域共用同一份编号）。 */
extern void m43b_cb_state(int id, const uint64_t *x, const float *f);
extern void m43b_note_cmd(int op);
/* M44A：**probe-OFF 对照构建**（-DM44A_PROBE_OFF=1，只在 /tmp 隔离树里打开，用于崩溃归因的对照实验）。
   把三处挂点在编译期整体消掉 —— 对照构建里的 cmdbuf.c 与"探针引入之前"逐条等价（没有函数调用、
   没有互斥、没有写盘），这样 probe-ON / probe-OFF 的差异只可能来自探针本身。默认（不定义该宏）= 探针在。 */
#ifdef M44A_PROBE_OFF
#define M43B_TAP(id) ((void)0)
#define M43B_TAP_X(id, xp) ((void)(xp))   /* 消掉变量未使用的告警，同时不产生任何调用 */
#define M43B_NOTE_CMD(op) ((void)0)
#else
#define M43B_TAP(id) m43b_cb_state((id), c->x, (const float *)c->v)
/* M43B_TAP_X：当直喂侧一条 NVN 调用会写**多条**记录、且每条记录的"直喂侧真值"不是 c->x 原样时
   （M44A：OP_CALL 每个句柄一条、OP_REPORT_COUNTER 的地址按直喂侧口径清零过），用新造的 x 调探针。 */
#define M43B_TAP_X(id, xp) m43b_cb_state((id), (xp), (const float *)c->v)
#define M43B_NOTE_CMD(op) m43b_note_cmd((int)(op))
#endif
#define PLAIN_T(id, name, op) NVN(name) { Snap s = snap(c); record(c, op, 0, &s); M43B_TAP(id); }
#include "../nvn_vk2/rec/m43b_tap_ids.h"

static NvnCmdBuf *cb_of(uint64_t guest) { return HOST(NvnCmdBuf, guest); }

NVN(nvnCommandBufferInitialize) {
    NvnCmdBuf *cb = calloc(1, sizeof *cb);
    cb->guest = A(0);
    SET_HOST(A(0), cb);
    RET(1);
}
NVN(nvnCommandBufferFinalize) { free(cb_of(A(0))); SET_HOST(A(0), 0); }
NVN(nvnCommandBufferSetDebugLabel) { (void)c; }

static CmdRec *raw_rec(uint64_t at, uint32_t op, uint32_t size) {
    CmdRec *r = (CmdRec *)(uintptr_t)at;
    memset(r, 0, sizeof *r);
    r->op = op;
    r->size = size;
    return r;
}

// ---- shadow pages: guest page -> host block of SHADOW_PAGE * SHADOW_K bytes (allocated at first use, kept)
// Asked for every record the game's threads write, several at once: a lookup takes no lock - slots are only ever
// added (mem published before page). The lock is only for adding one: under it every record contended (futex wakes and
// bionic's contention trace writes, ~15% of the game's threads, the critical path of the frame; simpleperf
// 2026-10-31). No per-thread cache of the last page: a thread-local costs a dynamic TLS resolve in this library.
enum { SHADOW_SLOTS = 1 << 16 };
static struct { uint64_t page; uint8_t *mem; } g_shadow[SHADOW_SLOTS];
static pthread_mutex_t g_shadow_lock = PTHREAD_MUTEX_INITIALIZER;
// M5r: the two things that really cost host memory, counted so the CallCommands budget can be a memory
// guard instead of a byte-throughput counter (see CALL_TOTAL_MAX below). A shadow page is kept forever (one
// per 64 KB of game address ever written); add_host_memory chunks are never freed either.
static uint64_t g_shadow_pages;    // * (SHADOW_PAGE*SHADOW_K) bytes
static uint64_t g_hostmem_bytes;   // add_host_memory chunks, host bytes
// M5r: CB begin/end pairing and the last CB as it left EndRecording (its first 32 bytes are what the
// submit walker sees first). Written by the record threads, read by the submit-time report.
static uint64_t g_cb_begin, g_cb_end, g_cb_end_nonempty;
static uint64_t g_last_cb_start, g_last_cb_bytes;
static uint32_t g_last_cb_words[8];
// M5r: the handle handed to nvnQueueSubmitCommands and its first 32 bytes. This is the CB the walker
// actually replays, so it is the honest place to answer "guest did not write" vs "we did not capture".
static uint64_t g_submit_n;
static uint64_t g_last_sub_h;
static uint32_t g_last_sub_words[8];

static uint8_t *shadow_find(uint64_t page, uint32_t *at) {
    uint32_t i = (uint32_t)(page * 0x9E3779B97F4A7C15ull >> 48) & (SHADOW_SLOTS - 1);
    for (uint64_t p; (p = __atomic_load_n(&g_shadow[i].page, __ATOMIC_ACQUIRE)); i = (i + 1) & (SHADOW_SLOTS - 1))
        if (p == page) return g_shadow[i].mem;
    *at = i;
    return NULL;
}

static uint64_t shadow_at(uint64_t guest) {
    uint64_t page = guest / SHADOW_PAGE + 1;  // (+1: 0 marks an empty slot)
    uint32_t i;
    uint8_t *m = shadow_find(page, &i);
    if (!m) {
        pthread_mutex_lock(&g_shadow_lock);
        if (!(m = shadow_find(page, &i))) {  // (still missing: added at the empty slot it ended on)
            m = malloc(SHADOW_PAGE * SHADOW_K + sizeof(CmdRec));
            if (!m) FATAL("nvn: no memory for a command shadow page\n");
            g_shadow[i].mem = m;
            __atomic_store_n(&g_shadow[i].page, page, __ATOMIC_RELEASE);
            __atomic_fetch_add(&g_shadow_pages, 1, __ATOMIC_RELAXED);
        }
        pthread_mutex_unlock(&g_shadow_lock);
    }
    return (uint64_t)(uintptr_t)m + guest % SHADOW_PAGE * SHADOW_K;
}

// ---- PORT(port-only, additive; PORTING.md "cmd_trace 边界查询"): host-chunk registry + readable-bounds query
// for the cmd_trace walker (cmd_trace.c). Purely additive: nothing above changes behavior. The walker follows
// CMD_JUMP chains through host shadow pages / host chunks, and must bound every read by a region we allocated —
// a shadow page block (SHADOW_PAGE*SHADOW_K + one spare CmdRec of slack, line ~65) or a host chunk from
// add_host_memory. Pool-storage chunks are registered by glue_cmdbuf.c on top of this.
static struct { uint8_t *mem; uint64_t bytes; } g_host_chunks[4096];
static uint32_t g_host_chunks_n;
static void note_host_chunk(uint8_t *m, uint64_t bytes) {
    if (g_host_chunks_n < sizeof g_host_chunks / sizeof g_host_chunks[0]) {  // (submit thread + record threads;
        g_host_chunks[g_host_chunks_n].mem = m;                             //  entries are only ever added, and a
        g_host_chunks[g_host_chunks_n].bytes = bytes;                       //  torn read at worst sees a stale n)
        g_host_chunks_n++;
    }
}
// How many bytes are safely readable at host address p within one region we allocated for command records?
// 0 = p is in no known region: the caller must stop and count, never read.
uint64_t nvn_port_cmdmem_readable(const void *p) {
    static uint32_t last;  // records cluster inside a page/chunk: cache the last hit (see line 44 note)
    uint8_t *q = (uint8_t *)p;
    uint32_t l = __atomic_load_n(&last, __ATOMIC_RELAXED);
    if (l) {
        if (__atomic_load_n(&g_shadow[l].page, __ATOMIC_ACQUIRE)) {
            uint8_t *m = g_shadow[l].mem, *hi = m + SHADOW_PAGE * SHADOW_K + sizeof(CmdRec);
            if (q >= m && q < hi) return (uint64_t)(hi - q);
        }
    }
    for (uint32_t i = 1; i < SHADOW_SLOTS; i++) {  // (slot 0 never holds: page numbers start at 1)
        if (!__atomic_load_n(&g_shadow[i].page, __ATOMIC_ACQUIRE)) continue;
        uint8_t *m = g_shadow[i].mem, *hi = m + SHADOW_PAGE * SHADOW_K + sizeof(CmdRec);
        if (q >= m && q < hi) { __atomic_store_n(&last, i, __ATOMIC_RELAXED); return (uint64_t)(hi - q); }
    }
    for (uint32_t i = 0; i < g_host_chunks_n; i++) {
        uint8_t *m = g_host_chunks[i].mem, *hi = m + g_host_chunks[i].bytes;
        if (q >= m && q < hi) return (uint64_t)(hi - q);
    }
    return 0;
}

// The chunk records go to: cb->gmem (guest address; 0: host memory at cb->mem), cb->size and cb->used in game bytes.
// (the page last written kept in the command buffer: a page's shadow never moves, so it can't go stale)
static uint64_t write_at(NvnCmdBuf *cb) {
    if (!cb->gmem) return cb->mem + cb->used * SHADOW_K;
    uint64_t guest = cb->gmem + cb->used, page = guest / SHADOW_PAGE + 1;
    if (page != cb->shadow_page) cb->shadow_mem = shadow_at(guest - guest % SHADOW_PAGE), cb->shadow_page = page;
    return cb->shadow_mem + guest % SHADOW_PAGE * SHADOW_K;
}

// The list being recorded goes on at `to` (a jump to it).
static void jump_from_here(NvnCmdBuf *cb, uint64_t to) {
    if (cb->recording && (cb->gmem || cb->mem)) raw_rec(write_at(cb), CMD_JUMP, sizeof(CmdRec))->x[0] = to;
}

static void set_chunk(NvnCmdBuf *cb, uint64_t gmem, uint64_t mem, uint64_t size) {
    if (gmem) {
        uint64_t in_page = gmem % SHADOW_PAGE;
        if (in_page + JUMP_RESERVE > SHADOW_PAGE) {
            uint64_t skip = SHADOW_PAGE - in_page;
            gmem += skip;
            size = size > skip ? size - skip : 0;
        }
    }
    if (cb->recording && ENV("LARECOMP_CHECK_HANDLES")) {
        static int n;
        if (n++ < 40) LOG("[nvn] command buffer %llx: new memory %llx (host %llx) while recording, %llu of %llu used at %llx\n",
                          (unsigned long long)cb->guest, (unsigned long long)gmem, (unsigned long long)mem,
                          (unsigned long long)cb->used, (unsigned long long)cb->size, (unsigned long long)cb->gmem);
    }
    jump_from_here(cb, gmem ? shadow_at(gmem) : mem);
    cb->gmem = gmem;
    cb->mem = mem;
    cb->size = size;
    cb->used = 0;
}

NVN(nvnCommandBufferAddCommandMemory) {
    NvnCmdBuf *cb = cb_of(A(0));
    if (ENV("LARECOMP_CHECK_HANDLES")) {
        static int n;
        if (n++ < 300) LOG("[nvn] AddCommandMemory %llx: pool %llx +%llx size %llx (lr %llx)\n", (unsigned long long)A(0),
                           (unsigned long long)A(1), (unsigned long long)A(2), (unsigned long long)A(3), (unsigned long long)c->x[30]);
    }
    cb->cmd_size = A(3), cb->cmd_used = 0;
    if (!cb->has_ctrl) set_chunk(cb, HOST(NvnPool, A(1))->storage + A(2), 0, A(3));
}
NVN(nvnCommandBufferAddControlMemory) {
    NvnCmdBuf *cb = cb_of(A(0));
    if (ENV("LARECOMP_CHECK_HANDLES")) {
        static int n;
        if (n++ < 300) LOG("[nvn] AddControlMemory %llx: %llx size %llx (lr %llx)\n", (unsigned long long)A(0), (unsigned long long)A(1),
                           (unsigned long long)A(2), (unsigned long long)c->x[30]);
    }
    cb->has_ctrl = true;
    set_chunk(cb, A(1), 0, A(2));
}
static uint64_t chunk_free(const NvnCmdBuf *cb) { return cb->size > cb->used + JUMP_RESERVE ? cb->size - cb->used - JUMP_RESERVE : 0; }
NVN(nvnCommandBufferGetCommandMemorySize) { NvnCmdBuf *cb = cb_of(A(0)); RET(cb->has_ctrl ? cb->cmd_size : cb->size); }
NVN(nvnCommandBufferGetCommandMemoryUsed) { NvnCmdBuf *cb = cb_of(A(0)); RET(cb->has_ctrl ? cb->cmd_used : cb->used); }
NVN(nvnCommandBufferGetCommandMemoryFree) {
    NvnCmdBuf *cb = cb_of(A(0));
    RET(cb->has_ctrl ? cb->cmd_size - cb->cmd_used : chunk_free(cb));
}
NVN(nvnCommandBufferGetControlMemorySize) { NvnCmdBuf *cb = cb_of(A(0)); RET(cb->has_ctrl ? cb->size : 0x10000); }
NVN(nvnCommandBufferGetControlMemoryUsed) { NvnCmdBuf *cb = cb_of(A(0)); RET(cb->has_ctrl ? cb->used : 0); }
NVN(nvnCommandBufferGetControlMemoryFree) { NvnCmdBuf *cb = cb_of(A(0)); RET(cb->has_ctrl ? chunk_free(cb) : 0x10000); }
NVN(nvnCommandBufferSetMemoryCallback) { cb_of(A(0))->callback = A(1); }
NVN(nvnCommandBufferGetMemoryCallback) { RET(cb_of(A(0))->callback); }
NVN(nvnCommandBufferSetMemoryCallbackData) { cb_of(A(0))->callback_data = A(1); }
NVN(nvnCommandBufferGetMemoryCallbackData) { RET(cb_of(A(0))->callback_data); }
NVN(nvnCommandBufferSetCommandMemoryCallbackEnabled) { (void)c; }

// Memory of our own, when the game's runs out and its callback adds none (BOTW: an empty callback on small command
// buffers). Lists live as long as the game keeps calling them, so these chunks are never freed; LARECOMP_LOG_CMDMEM
// logs each one.
static void add_host_memory(NvnCmdBuf *cb, uint32_t need) {
    uint64_t size = need + JUMP_RESERVE > 0x800 ? need + JUMP_RESERVE : 0x800;  // (game bytes)
    uint8_t *mem = malloc(size * SHADOW_K);
    if (!mem) FATAL("nvn: out of command memory (host, %llu bytes)\n", (unsigned long long)(size * SHADOW_K));
    uint64_t total = __atomic_add_fetch(&g_hostmem_bytes, size * SHADOW_K, __ATOMIC_RELAXED);  // M5r: real host bytes
    note_host_chunk(mem, size * SHADOW_K);  // PORT(port-only, additive): bounds registry for cmd_trace (PORTING.md)
    if (ENV("LARECOMP_LOG_CMDMEM"))
        LOG("[nvn] command buffer %llx: %llu bytes of host command memory (%llu KB in all)\n",
            (unsigned long long)cb->guest, (unsigned long long)(size * SHADOW_K), (unsigned long long)(total / 1024));
    set_chunk(cb, 0, (uint64_t)(uintptr_t)mem, size);
}

// Reserve a record of `real` bytes (header included). May call back into the guest for more memory, so
// callers snapshot their arguments first. (M4an: split out of rec() so CallCommands can inline a record
// of exactly the size the called list used.)
static CmdRec *rec_alloc(Cpu *c, NvnCmdBuf *cb, uint32_t real) {
    uint32_t size = (real + SHADOW_K - 1) / SHADOW_K;  // (game bytes)
    for (int tries = 0; (!cb->gmem && !cb->mem) || cb->used + size + JUMP_RESERVE > cb->size; tries++) {
        if (!cb->callback || tries == 2) {
            add_host_memory(cb, size);
            break;
        }
        uint64_t saved[8];
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = cb->guest;
        c->x[1] = cb->has_ctrl;  // NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY / _CONTROL_MEMORY
        c->x[2] = size + JUMP_RESERVE;
        c->x[3] = cb->callback_data;
        guest_call(c, cb->callback);
        memcpy(c->x, saved, sizeof saved);
    }
    if (cb->gmem) {  // (a record doesn't cross a shadow page: the rest of the page is skipped)
        uint64_t in_page = (cb->gmem + cb->used) % SHADOW_PAGE;
        if (in_page + size + JUMP_RESERVE > SHADOW_PAGE) {
            uint64_t skip = SHADOW_PAGE - in_page;
            if (cb->used + skip + size + JUMP_RESERVE <= cb->size) {
                raw_rec(write_at(cb), CMD_JUMP, sizeof(CmdRec))->x[0] = shadow_at(cb->gmem + cb->used + skip);
                cb->used += skip;
            } else {
                add_host_memory(cb, size);
            }
        }
    }
    CmdRec *r = raw_rec(write_at(cb), 0, real);
    cb->used += size;
    return r;
}
static CmdRec *rec(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra) {
    CmdRec *r = rec_alloc(c, cb, (uint32_t)((sizeof(CmdRec) + extra + 7) & ~7u));
    r->op = op;
    return r;
}

NVN(nvnCommandBufferBeginRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    cb->recording = true;
    cb->start = (uint64_t)(uintptr_t)rec(c, cb, CMD_BARRIER, 0);  // makes sure memory exists; the list starts here
    __atomic_fetch_add(&g_cb_begin, 1, __ATOMIC_RELAXED);         // M5r: begin/end pairing (report P3 #2)
}
// LARECOMP_CHECK_HANDLES: the last lists recorded (where they start and end in the shadow, by whom, from which game
// memory), to tell who recorded over a list that is called later
typedef struct { uint64_t start, end, cb, gmem, gused, lr, seq; } ListRec;
enum { HIST = 1 << 16 };
static ListRec g_hist[HIST];
static _Atomic uint64_t g_hist_n;
static void hist_report(uint64_t h) {
    uint64_t n = g_hist_n, found = ~0ull;
    for (uint64_t k = n; k-- > (n > HIST ? n - HIST : 0);)
        if (g_hist[k % HIST].start == h) { found = k; break; }
    if (found == ~0ull) { LOG("[nvn]   %llx: not among the last lists recorded\n", (unsigned long long)h); return; }
    const ListRec *l = &g_hist[found % HIST];
    LOG("[nvn]   %llx: list #%llu by %llx (lr %llx), game memory %llx+%llx, %llu lists ago; recorded over by:\n",
        (unsigned long long)h, (unsigned long long)found, (unsigned long long)l->cb, (unsigned long long)l->lr,
        (unsigned long long)l->gmem, (unsigned long long)l->gused, (unsigned long long)(n - found));
    int shown = 0;
    for (uint64_t k = found + 1; k < n && shown < 6; k++) {
        const ListRec *o = &g_hist[k % HIST];
        if (o->start < l->end && o->end > l->start) {
            LOG("[nvn]     #%llu by %llx (lr %llx): %llx-%llx, game memory %llx+%llx\n", (unsigned long long)k,
                (unsigned long long)o->cb, (unsigned long long)o->lr, (unsigned long long)o->start, (unsigned long long)o->end,
                (unsigned long long)o->gmem, (unsigned long long)o->gused);
            shown++;
        }
    }
}

NVN(nvnCommandBufferEndRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    rec(c, cb, CMD_END, 0);
    cb->recording = false;
    if (ENV("LARECOMP_CHECK_HANDLES")) {
        uint64_t k = g_hist_n++;
        ListRec *l = &g_hist[k % HIST];
        l->start = cb->start, l->end = write_at(cb), l->cb = cb->guest, l->gmem = cb->gmem, l->gused = cb->used;
        l->lr = c->x[30], l->seq = k;
    }
    if (ENV("LARECOMP_CHECK_HANDLES")) {
        static int n;
        if (n++ < 400) LOG("[nvn] EndRecording %llx -> %llx (lr %llx)\n", (unsigned long long)cb->guest,
                           (unsigned long long)cb->start, (unsigned long long)c->x[30]);
    }
    __atomic_fetch_add(&g_cb_end, 1, __ATOMIC_RELAXED);              // M5r: begin/end pairing
    {   // M5r: snapshot the CB the walker will see: start, span, and its first 32 bytes (safe-read bounded)
        uint64_t s = cb->start, e = write_at(cb);
        uint64_t span = e > s ? e - s : 0;
        g_last_cb_start = s, g_last_cb_bytes = span;
        if (span > sizeof(CmdRec)) __atomic_fetch_add(&g_cb_end_nonempty, 1, __ATOMIC_RELAXED);
        uint64_t avail = s ? nvn_port_cmdmem_readable((const void *)(uintptr_t)s) : 0;
        memset(g_last_cb_words, 0, sizeof g_last_cb_words);
        if (avail >= sizeof g_last_cb_words) memcpy(g_last_cb_words, (const void *)(uintptr_t)s, sizeof g_last_cb_words);
    }
    RET(cb->start);
}
NVN(nvnCommandBufferIsRecording) { RET(cb_of(A(0))->recording); }

// ---- M4an (2026-11-04): CallCommands / CopyCommands
//
// Until now both were flat accounting stubs in gfx_bridge.c, so the lists they call (47,648 calls per boot)
// never entered any command stream: nothing they recorded was ever replayed, and CALL did not even appear in
// the opcode histogram. donor semantics (third_party/d2r_gfx/gfx/nvn/hle_nvn.c:1172-1215): for every handle,
// copy the already-recorded record bodies byte for byte into the caller's stream, stopping at END and skipping
// BEGIN (the loop below is that one). Every read is bounded by nvn_port_cmdmem_readable first; a handle that
// is not a readable list only counts as bad and is skipped, it never takes the process down.
// Per-call inline budget 1 MiB (truncate + count past it); a global budget keeps a runaway call pattern from
// growing command memory without bound (add_host_memory chunks are never freed).
#define CALL_INLINE_MAX (1u << 20)
#define CALL_TOTAL_MAX  (4096ull << 20)  // M4an: boot#1 的 512 MiB 在 #4000 左右打满（capped=26024）遮住了受试尾部
                                         // 注意必须用 ull：`4096u << 20` 在 32 位上溢出成 0（boot#2 因此全量 capped）
/* M5r（2026-11-05）: THE 4 GiB CUMULATIVE CAP WAS THE GATE.
   m5q_b2 hit CALL_TOTAL_MAX at t~541 s and from then on every nvnCommandBufferCallCommands returned 0
   without inlining anything and without writing the CMD_CALL marker: 10.26 M of 11.74 M calls dropped,
   CALL opcode frozen at 179129, draws frozen at 91982, screen stuck on the load screen. The game itself
   never stopped (33 threads, identical wait kinds on both sides, io finished at t~422, 30 k calls/s).
   And the counter is not memory: inlining 2.6 GiB moved RSS by ~0 while 300 MB of RSS appeared with
   inlining switched off (report P0.6). So:
     - the cumulative counter stays as a headline number only (1 TiB: a boot cannot reach it);
     - the budget that can stop inlining now measures host command memory actually allocated
       (shadow pages + add_host_memory chunks), which is the thing that used to run away. */
#define CMDMEM_HOST_MAX (3072ull << 20)  // 3 GiB of real command host memory; RSS guard is 4200 MB
static uint64_t g_cc_calls, g_cc_bytes, g_cc_recs, g_cc_bad, g_cc_end, g_cc_trunc, g_cc_notrec, g_cc_capped;
static uint64_t g_cc_hostcap;   // M5r: calls refused by the REAL memory cap (distinct from g_cc_capped)
static uint64_t g_cc_total_cap = (1ull << 40);
static uint64_t g_cmdmem_host_cap = CMDMEM_HOST_MAX;
static uint64_t g_cc_last[4];  // bytes, recs, bad, stop_end of the last call (the ledger's per-call line)

// M5r: real command-stream host memory, the only honest budget input.
static uint64_t cmdmem_host_bytes(void) {
    return __atomic_load_n(&g_hostmem_bytes, __ATOMIC_RELAXED) +
           __atomic_load_n(&g_shadow_pages, __ATOMIC_RELAXED) * (SHADOW_PAGE * SHADOW_K + sizeof(CmdRec));
}
// M5r: the CB as it leaves EndRecording - its first 32 bytes are what the submit walker sees first.
// (state declared at the top of the file)
// M5r ledger: host_mem_bytes, shadow_pages, host_chunks, hostcap, cb_begin, cb_end, cb_end_nonempty, 0
void nvn_port_cmdmem_stats(uint64_t *o) {
    o[0] = cmdmem_host_bytes();
    o[1] = __atomic_load_n(&g_shadow_pages, __ATOMIC_RELAXED);
    o[2] = __atomic_load_n(&g_hostmem_bytes, __ATOMIC_RELAXED);
    o[3] = g_cc_hostcap;
    o[4] = g_cb_begin;
    o[5] = g_cb_end;
    o[6] = g_cb_end_nonempty;
    o[7] = 0;
}
void nvn_port_last_cb(uint64_t *o) {  // submit_n, last_submit_handle, span_of_last_ended_cb, then 8 words
    o[0] = __atomic_load_n(&g_submit_n, __ATOMIC_RELAXED);
    o[1] = __atomic_load_n(&g_last_sub_h, __ATOMIC_RELAXED);
    o[2] = g_last_cb_bytes;
    for (int i = 0; i < 8; i++) o[3 + i] = g_last_sub_words[i];
}
// M5r: called from nvn_queue_submit_commands with the guest's handle array (read-only, bounded).
void nvn_port_note_submit_h(const void *h, int n) {
    __atomic_fetch_add(&g_submit_n, 1, __ATOMIC_RELAXED);
    if (n <= 0 || !h) return;
    uint64_t a = ((const uint64_t *)h)[0];
    __atomic_store_n(&g_last_sub_h, a, __ATOMIC_RELAXED);
    uint64_t avail = a ? nvn_port_cmdmem_readable((const void *)(uintptr_t)a) : 0;
    memset(g_last_sub_words, 0, sizeof g_last_sub_words);
    if (avail >= sizeof g_last_sub_words) memcpy(g_last_sub_words, (const void *)(uintptr_t)a, sizeof g_last_sub_words);
}

void nvn_port_callcmd_totals(uint64_t *o) {  // calls, bytes, recs, bad, stop_end, trunc, notrec, capped
    o[0] = g_cc_calls, o[1] = g_cc_bytes, o[2] = g_cc_recs, o[3] = g_cc_bad;
    o[4] = g_cc_end, o[5] = g_cc_trunc, o[6] = g_cc_notrec, o[7] = g_cc_capped;
}
void nvn_port_callcmd_last(uint64_t *o) {  // bytes, recs, bad, stop_end
    memcpy(o, g_cc_last, sizeof g_cc_last);
}
static void call_list(Cpu *c, NvnCmdBuf *cb, uint64_t h, uint64_t *bytes, uint64_t *recs, uint64_t *bad,
                      uint64_t *end, uint64_t budget) {
    if (h <= 0x10000) { (*bad)++; return; }
    const uint8_t *p = (const uint8_t *)(uintptr_t)h;
    uint64_t avail = 0;
    for (uint64_t steps = 0; steps < (1u << 16); steps++) {
        if (avail < sizeof(CmdRec)) {  // (a new region, or the first record: bound the read before touching it)
            avail = nvn_port_cmdmem_readable(p);
            if (avail < sizeof(CmdRec)) { (*bad)++; return; }
        }
        const CmdRec *r = (const CmdRec *)p;
        if (r->op >= CMD_COUNT) { (*bad)++; return; }
        if (r->op == CMD_END) { (*end)++; return; }  // (the caller's own END stays the terminator)
        if (r->op == CMD_JUMP) {
            if (r->x[0] <= 0x10000) { (*bad)++; return; }
            p = (const uint8_t *)(uintptr_t)r->x[0];
            avail = 0;
            continue;
        }
        if ((r->size & 7) || r->size < sizeof(CmdRec) || r->size > 0x100000 || (uint64_t)r->size > avail) {
            (*bad)++;
            return;
        }
        if (*bytes + r->size > budget) { g_cc_trunc++; return; }
        CmdRec *d = rec_alloc(c, cb, r->size);
        memcpy(d, r, r->size);
        *bytes += r->size;
        (*recs)++;
        p += r->size;
        avail -= r->size;
    }
    g_cc_trunc++;  // step guard (a cyclic list recorded by the game)
}
static uint64_t call_commands(Cpu *c, int n, const uint64_t *hs) {
    NvnCmdBuf *cb = cb_of(A(0));
    uint64_t bytes = 0, recs = 0, bad = 0, end = 0;
    g_cc_calls++;
    g_cc_last[0] = g_cc_last[1] = g_cc_last[2] = g_cc_last[3] = 0;
    if (!cb || !cb->recording || !hs || n <= 0) {  // (not recording: nothing to inline into; counted, not silent)
        if (cb && !cb->recording) g_cc_notrec++;
        else g_cc_bad++;
        return 0;
    }
    if (g_cc_bytes >= g_cc_total_cap) {  // M5r: headline counter only, 1 TiB (was 4 GiB = the gate)
        g_cc_capped++;
        return 0;
    }
    if (cmdmem_host_bytes() >= g_cmdmem_host_cap) {  // M5r: the real guard (shadow pages + host chunks)
        g_cc_hostcap++;
        return 0;
    }
    if (n > 64) { g_cc_trunc += (uint64_t)(n - 64); n = 64; }  // (infra capped handles[] at 64 too)
    for (int i = 0; i < n; i++) {
        CmdRec *m = rec(c, cb, CMD_CALL, 0);  // (opcode histogram evidence: CALL must go from 0 to >0)
        m->x[0] = hs[i];
        m->x[1] = 0;
        uint64_t r0 = recs;
        call_list(c, cb, hs[i], &bytes, &recs, &bad, &end, bytes + CALL_INLINE_MAX);
        // M4ao P1: x[1] = how many records this handle's body appended. The walker needs the body length to tell
        // "new" inlined draws from the top-level stream (donor CMD_CALL carries no such field, x[1] was unused).
        // Patched after the body lands: m points into the same host chunk, so the pointer stays valid.
        m->x[1] = recs - r0;
        /* M44A / S2b-2 前置：OP_CALL 镜像。直喂侧每写一条 CMD_CALL 就是"接进来一个命令句柄"，
           探针拿**同一个句柄**去调录制侧 CopyCommands（nx2 侧它只写一条 OP_CALL{target=handle}，
           不解析句柄内容、不回写客体、不动本命令流）；c->x 一个字节都没改，call_list 的语义照旧。 */
        { uint64_t tx[8]; memcpy(tx, c->x, sizeof tx); tx[1] = hs[i]; M43B_TAP_X(T_CALL, tx); }
    }
    g_cc_bytes += bytes, g_cc_recs += recs, g_cc_bad += bad, g_cc_end += end;
    g_cc_last[0] = bytes, g_cc_last[1] = recs, g_cc_last[2] = bad, g_cc_last[3] = end;
    return 1;
}
NVN(nvnCommandBufferCallCommands) { RET(call_commands(c, (int)A(1), (const uint64_t *)(uintptr_t)A(2))); }
NVN(nvnCommandBufferCopyCommands) { RET(call_commands(c, (int)A(1), (const uint64_t *)(uintptr_t)A(2))); }

// ---- generic recorders
typedef struct { uint64_t x[8]; float f[4]; } Snap;
static Snap snap(Cpu *c) {
    Snap s;
    memcpy(s.x, c->x, sizeof s.x);
    for (int i = 0; i < 4; i++) s.f[i] = *(float *)&c->v[i];
    return s;
}
static CmdRec *record(Cpu *c, uint32_t op, uint32_t extra, Snap *s) {
    M43B_NOTE_CMD(op);   /* M43B：全量命令计数（每一条直喂侧记录算一次；与对账行数应逐 opcode 相等） */
    CmdRec *r = rec(c, cb_of(s->x[0]), op, extra);
    memcpy(r->x, s->x + 1, sizeof r->x);
    memcpy(r->f, s->f, sizeof r->f);
    return r;
}
/* M17A（2026-11-08）：**逐笔宿主点名**。draw 录制完成后把客体返回址（LR）与帧指针（FP）塞进
   CmdRec 的 f[0..3] —— 回放侧从不用 draw 的 f[]（cmd_trace.c 只有 x[] 参与语义，grep 全 0 命中），
   所以这是纯观测、不改任何行为。好处：信息**随记录走**，回放时逐笔精确对应，不受
   skip/合并/延迟回放影响，也不需要"每笔领走队列"的对齐假设。
   回放侧（cmd_trace.c）在 CMD_DRAW_ELEMENTS/CMD_DRAW_ARRAYS 分支把它抄到 g_m17a_cur_lr/fp，
   nvn_vk 台账随即打印（宿主函数 = eh_frame_hdr FDE 反查）。 */
extern uint64_t g_m17a_rec_lr, g_m17a_rec_fp;   /* glue_cmdbuf.c：录制前取的客体 LR/FP */
static void m17a_stash_lr(CmdRec *r, Cpu *c)
{
    (void)c;
    memcpy(&r->f[0], &g_m17a_rec_lr, 8);   /* guest LR = 录这笔画面的宿主返回址 */
    memcpy(&r->f[2], &g_m17a_rec_fp, 8);   /* guest FP = 帧链（上游调用者） */
}
// record with a copy of `len` bytes from the guest pointer in argument `arg`
static CmdRec *record_copy(Cpu *c, uint32_t op, int arg, uint32_t len) {
    Snap s = snap(c);
    CmdRec *r = record(c, op, len, &s);
    if (s.x[arg] && len) memcpy(r->extra, (void *)(uintptr_t)s.x[arg], len);
    return r;
}
#define PLAIN(name, op) NVN(name) { Snap s = snap(c); record(c, op, 0, &s); }

// ---- state binding
NVN(nvnCommandBufferBindProgram) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_PROGRAM, 0, &s);
    /* M3r（2026-11-03）：donor 的 HOST(NvnProgram, guest) 读客体偏移 0 的指针；本移植的 Program 对象
       是 gfx_bridge.c 的 prog_h{magic='PROG', nstage} 就地存在客体内存里，偏移 0 是 magic 不是指针，
       解引用会得到 0x50524f47。客体地址 == 宿主地址是既有前提，直接存 s.x[1] 即为程序对象指针，
       也正是 ProgramSetShaders 登记 shader key 时用的同一个指针。 */
    r->x[0] = s.x[1];
    M43B_TAP(T_PROGRAM);   /* M43B */
}
NVN(nvnCommandBufferBindBlendState) { record_copy(c, CMD_BIND_BLEND, 1, 8);     M43B_TAP(T_BLEND);   /* M43B */
}
NVN(nvnCommandBufferBindChannelMaskState) { record_copy(c, CMD_BIND_CHANNEL_MASK, 1, 4);     M43B_TAP(T_CHMASK);   /* M43B */
}
NVN(nvnCommandBufferBindColorState) { record_copy(c, CMD_BIND_COLOR, 1, 4);     M43B_TAP(T_COLOR);   /* M43B */
}
NVN(nvnCommandBufferBindDepthStencilState) { record_copy(c, CMD_BIND_DEPTH_STENCIL, 1, 8);     M43B_TAP(T_DSST);   /* M43B */
}
NVN(nvnCommandBufferBindMultisampleState) { record_copy(c, CMD_BIND_MULTISAMPLE, 1, 0x18);     M43B_TAP(T_MS);   /* M43B */
}
NVN(nvnCommandBufferBindPolygonState) { record_copy(c, CMD_BIND_POLYGON, 1, 4);     M43B_TAP(T_POLY);   /* M43B */
}
NVN(nvnCommandBufferBindVertexAttribState) { record_copy(c, CMD_BIND_VERTEX_ATTRIB, 2, (uint32_t)A(1) * 4);     M43B_TAP(T_VATTR);   /* M43B */
}
NVN(nvnCommandBufferBindVertexStreamState) { record_copy(c, CMD_BIND_VERTEX_STREAM, 2, (uint32_t)A(1) * 8);     M43B_TAP(T_VSTREAM);   /* M43B */
}

// (cmd, first, count, const NVNbufferRange*) and the single-binding forms
NVN(nvnCommandBufferBindVertexBuffers) { record_copy(c, CMD_BIND_VERTEX_BUFFERS, 3, (uint32_t)A(2) * 16);     M43B_TAP(T_VBUF);   /* M43B */
}
NVN(nvnCommandBufferBindVertexBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_VERTEX_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = 1;
    memcpy(r->extra, &s.x[2], 16);
    M43B_TAP(T_VBUF1);   /* M43B */
}
// (cmd, stage, first, count, const NVNbufferRange*)
NVN(nvnCommandBufferBindUniformBuffers) { record_copy(c, CMD_BIND_UNIFORM_BUFFERS, 4, (uint32_t)A(3) * 16);     M43B_TAP(T_UBO);   /* M43B */
}
NVN(nvnCommandBufferBindUniformBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_UNIFORM_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
    M43B_TAP(T_UBO1);   /* M43B */
}
NVN(nvnCommandBufferBindStorageBuffers) { record_copy(c, CMD_BIND_STORAGE_BUFFERS, 4, (uint32_t)A(3) * 16);     M43B_TAP(T_SSBO);   /* M43B */
}
NVN(nvnCommandBufferBindStorageBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_STORAGE_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
    M43B_TAP(T_SSBO1);   /* M43B */
}
// (cmd, stage, first, count, const handle*)
NVN(nvnCommandBufferBindTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8);     M43B_TAP(T_TEX);   /* M43B */
}
// BindTexturesDeferred(cmd, stage, first, count, handles): the handles are read when the command runs, not now -
// BOTW 1.8.2 binds its deferred lighting's inputs this way (normals, albedo, depth, light buffers; SavageWindRecomp)
NVN(nvnCommandBufferBindTexturesDeferred) { Snap s = snap(c); record(c, CMD_BIND_TEXTURES_DEFERRED, 0, &s); }
NVN(nvnCommandBufferBindTexture) {
    {   extern int nvn_watch_texid(void);
        if ((int)(A(3) & 0xFFFFF) == nvn_watch_texid())
            LOG("[watch] BindTexture stage %llu slot %llu handle %llx (lr %llx)\n", (unsigned long long)A(1), (unsigned long long)A(2), (unsigned long long)A(3), (unsigned long long)c->x[30]);
    }
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
    M43B_TAP(T_TEX1);   /* M43B */
}
NVN(nvnCommandBufferBindImages) { record_copy(c, CMD_BIND_IMAGES, 4, (uint32_t)A(3) * 8);     M43B_TAP(T_IMG);   /* M43B */
}
NVN(nvnCommandBufferBindImage) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_IMAGES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
    M43B_TAP(T_IMG1);   /* M43B */
}
NVN(nvnCommandBufferBindSeparateTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindSeparateTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
// (not translated yet: logged, the first calls and a count - does the game override a slot's sampler this way?)
NVN(nvnCommandBufferBindSeparateSamplers) {
    static unsigned n;
    if (n++ < 20 || (n & (n - 1)) == 0)
        LOG("[nvn] BindSeparateSamplers #%u stage %llu first %llu count %llu: %llx...\n", n, (unsigned long long)A(1), (unsigned long long)A(2),
            (unsigned long long)A(3), A(3) ? (unsigned long long)*(const uint64_t *)AP(4) : 0ull);
}
NVN(nvnCommandBufferBindSeparateSampler) {
    static unsigned n;
    if (n++ < 20 || (n & (n - 1)) == 0)
        LOG("[nvn] BindSeparateSampler #%u stage %llu slot %llu handle %llx\n", n, (unsigned long long)A(1), (unsigned long long)A(2),
            (unsigned long long)A(3));
}
NVN(nvnCommandBufferBindTransformFeedbackBuffers) { (void)c; }
NVN(nvnCommandBufferBindTransformFeedbackBuffer) { (void)c; }
NVN(nvnCommandBufferBindCoverageModulationTable) { (void)c; }

NVN(nvnCommandBufferSetTexturePool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_TEXTURE_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
    M43B_TAP(T_TEXPOOL);   /* M43B */
}
NVN(nvnCommandBufferSetSamplerPool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SAMPLER_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
    M43B_TAP(T_SAMPPOOL);   /* M43B */
}

// SetRenderTargets(cmd, n, colors[], colorViews[], depth, depthView)
// extra: { u64 tex[8]; NvnTextureView views[8]; u8 has_view[8]; u64 depth; NvnTextureView dview; u8 has_dview }
typedef struct {
    uint64_t tex[8];
    NvnTextureView views[8];
    uint8_t has_view[8];
    uint64_t depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtRec;
/* M4q P1：回放侧按裸偏移读 depth（cmd_trace.h:NVN_CMD_RT_DEPTH_OFF=200），这里把两侧钉死。
   views 是 16B（nvn_obj.h:46 sizeof(view_t)==16），所以 64+128+8=200 且 8 字节对齐。 */
_Static_assert(offsetof(RtRec, depth) == NVN_CMD_RT_DEPTH_OFF,
               "RtRec.depth offset drifted from cmd_trace.h NVN_CMD_RT_DEPTH_OFF");
_Static_assert(offsetof(RtRec, tex) == NVN_CMD_RT_FIRST_TEX_OFF,
               "RtRec.tex offset drifted from cmd_trace.h NVN_CMD_RT_FIRST_TEX_OFF");
NVN(nvnCommandBufferSetRenderTargets) {
    Snap s = snap(c);
    int n = (int)s.x[1];
    if (n > 8) n = 8;
    RtRec rt = {0};
    const uint64_t *colors = (const uint64_t *)(uintptr_t)s.x[2], *views = (const uint64_t *)(uintptr_t)s.x[3];
    /* M4o P1：colors[i]/depth 是**游戏自己分配的 NVNtexture 对象指针**（NVN SDK 口径：
       const NVNtexture* const*），不是「句柄槽」。原来这里套了 HOST() 又解引用一次，读到的
       是 tex_h 头 8 字节（magic 0x54455800 | d.target<<32），m4n_b1 的 RT key
       0x154455800/0x254455800/0x854455800/0x454455800 就是这个——导致侧表必 miss、
       所有 RT 退回 1600×900 探针、1024×1024 中间 RT 的后处理画到屏上（整屏白）。 */
    for (int i = 0; i < n; i++) {
        rt.tex[i] = colors && colors[i] ? colors[i] : 0;
        if (views && views[i]) { rt.has_view[i] = 1; memcpy(&rt.views[i], (void *)(uintptr_t)views[i], sizeof(NvnTextureView)); }
    }
    rt.depth = s.x[4] ? s.x[4] : 0;
    if (s.x[5]) { rt.has_dview = 1; memcpy(&rt.dview, (void *)(uintptr_t)s.x[5], sizeof(NvnTextureView)); }
    CmdRec *r = record(c, CMD_SET_RENDER_TARGETS, sizeof rt, &s);
    r->x[0] = (uint64_t)n;
    memcpy(r->extra, &rt, sizeof rt);
    M43B_TAP(T_RT);   /* M43B */
}

// viewports/scissors/depth ranges
NVN(nvnCommandBufferSetViewports) { record_copy(c, CMD_SET_VIEWPORTS, 3, (uint32_t)A(2) * 16);     M43B_TAP(T_VIEWPORTS);   /* M43B */
}
NVN(nvnCommandBufferSetViewport) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_VIEWPORTS, 16, &s);
    float v[4] = {(float)(int32_t)s.x[1], (float)(int32_t)s.x[2], (float)(int32_t)s.x[3], (float)(int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
    M43B_TAP(T_VIEWPORT1);   /* M43B */
}
NVN(nvnCommandBufferSetScissors) { record_copy(c, CMD_SET_SCISSORS, 3, (uint32_t)A(2) * 16);     M43B_TAP(T_SCISSOR);   /* M43B */
}
NVN(nvnCommandBufferSetScissor) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SCISSORS, 16, &s);
    int32_t v[4] = {(int32_t)s.x[1], (int32_t)s.x[2], (int32_t)s.x[3], (int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
    M43B_TAP(T_SCISSOR1);   /* M43B */
}
NVN(nvnCommandBufferSetDepthRanges) { record_copy(c, CMD_SET_DEPTH_RANGES, 3, (uint32_t)A(2) * 8);     M43B_TAP(T_DRANGE);   /* M43B */
}
NVN(nvnCommandBufferSetDepthRange) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_DEPTH_RANGES, 8, &s);
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, s.f, 8);
    M43B_TAP(T_DRANGE1);   /* M43B */
}
NVN(nvnCommandBufferSetBlendColor) { record_copy(c, CMD_SET_BLEND_COLOR, 1, 16);     M43B_TAP(T_BLENDCOLOR);   /* M43B */
}
NVN(nvnCommandBufferSetStencilRef) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 0;     M43B_TAP(T_STENCIL_REF);   /* M43B */
}
NVN(nvnCommandBufferSetStencilMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 1;     M43B_TAP(T_STENCIL_MASK);   /* M43B */
}
NVN(nvnCommandBufferSetStencilValueMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 2;     M43B_TAP(T_STENCIL_VMASK);   /* M43B */
}
PLAIN_T(T_PRIMRESTART, nvnCommandBufferSetPrimitiveRestart, CMD_SET_PRIMITIVE_RESTART)
PLAIN(nvnCommandBufferSetAlphaRef, CMD_SET_ALPHA_REF)
PLAIN(nvnCommandBufferSetDepthBounds, CMD_SET_DEPTH_BOUNDS)
PLAIN_T(T_POLYOFF, nvnCommandBufferSetPolygonOffsetClamp, CMD_SET_POLYGON_OFFSET)
PLAIN(nvnCommandBufferSetLineWidth, CMD_SET_LINE_WIDTH)
PLAIN(nvnCommandBufferSetPointSize, CMD_SET_POINT_SIZE)
PLAIN(nvnCommandBufferSetSampleMask, CMD_SET_SAMPLE_MASK)
PLAIN_T(T_RENDEREN, nvnCommandBufferSetRenderEnable, CMD_SET_RENDER_ENABLE)
NVN(nvnCommandBufferSetRenderEnableConditional) { (void)c; }
PLAIN_T(T_DEPTHCLAMP, nvnCommandBufferSetDepthClamp, CMD_SET_DEPTH_CLAMP)
NVN(nvnCommandBufferSetRasterizerDiscard) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterEnable) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterDilate) { (void)c; }
NVN(nvnCommandBufferSetSubpixelPrecisionBias) { (void)c; }
NVN(nvnCommandBufferSetViewportSwizzles) { (void)c; }
NVN(nvnCommandBufferSetStencilCullCriteria) { (void)c; }
NVN(nvnCommandBufferSetColorReductionEnable) { (void)c; }
NVN(nvnCommandBufferSetColorReductionThresholds) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheAction) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheTileSize) { (void)c; }
NVN(nvnCommandBufferSetShaderScratchMemory) { (void)c; }
NVN(nvnCommandBufferSetPatchSize) { (void)c; }
NVN(nvnCommandBufferSetInnerTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetOuterTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetProgramSubroutines) { (void)c; }
NVN(nvnCommandBufferSaveZCullData) { (void)c; }
NVN(nvnCommandBufferRestoreZCullData) { (void)c; }

static uint64_t copy_row_stride, copy_image_stride;
RT_ON_RESET(cmdbuf_reset) { copy_row_stride = copy_image_stride = 0; }
NVN(nvnCommandBufferSetCopyRowStride) {
    Snap s = snap(c);
    copy_row_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
    M43B_TAP(T_COPYROW);   /* M43B */
}
NVN(nvnCommandBufferSetCopyImageStride) {
    Snap s = snap(c);
    copy_image_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
    M43B_TAP(T_COPYIMG);   /* M43B */
}
NVN(nvnCommandBufferGetCopyRowStride) { RET(copy_row_stride); }
NVN(nvnCommandBufferGetCopyImageStride) { RET(copy_image_stride); }

// ---- clears
NVN(nvnCommandBufferClearColor) { record_copy(c, CMD_CLEAR_COLOR, 2, 16);     M43B_TAP(T_CLEARCOLOR);   /* M43B */
}
NVN(nvnCommandBufferClearColori) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 1;     M43B_TAP(T_CLEARCOLOR_I);   /* M43B */
}
NVN(nvnCommandBufferClearColorui) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 2;     M43B_TAP(T_CLEARCOLOR_UI);   /* M43B */
}
PLAIN_T(T_CLEARDS, nvnCommandBufferClearDepthStencil, CMD_CLEAR_DEPTH_STENCIL)
// ClearTexture(cmd, texture, view, region, color, mask)
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } ClearTexRec;
static void clear_texture(Cpu *c, int kind) {
    Snap s = snap(c);
    ClearTexRec ct = {0};
    /* M4p P1：同 SetRenderTargets（:429 上方注释）——texture 参数是游戏自己的 NVNtexture*，
       客体内存里那颗对象就是 tex_h（magic 在偏移 0），再套 HOST() 会把 magic|target 当指针。 */
    ct.tex = s.x[1];
    if (s.x[2]) { ct.has_view = 1; memcpy(&ct.view, (void *)(uintptr_t)s.x[2], sizeof ct.view); }
    if (s.x[3]) memcpy(ct.region, (void *)(uintptr_t)s.x[3], sizeof ct.region);
    if (s.x[4]) memcpy(ct.color, (void *)(uintptr_t)s.x[4], 16);
    CmdRec *r = record(c, CMD_CLEAR_TEXTURE, sizeof ct, &s);
    r->x[6] = (uint64_t)kind;
    memcpy(r->extra, &ct, sizeof ct);
}
NVN(nvnCommandBufferClearTexture) { clear_texture(c, 0);     M43B_TAP(T_CLEARTEX);   /* M43B */
}
NVN(nvnCommandBufferClearTexturei) { clear_texture(c, 1);     M43B_TAP(T_CLEARTEX_I);   /* M43B */
}
NVN(nvnCommandBufferClearTextureui) { clear_texture(c, 2);     M43B_TAP(T_CLEARTEX_UI);   /* M43B */
}
PLAIN_T(T_CLEARBUF, nvnCommandBufferClearBuffer, CMD_CLEAR_BUFFER)
// DiscardColor(cmdbuf, index): the target's contents become undefined. On the Switch a discarded (compressed)
// target reads back cleared; areas the game then leaves undrawn (e.g. a pit after a floor tile falls away) stay
// black there. Treated as a clear to (0,0,0,0) (LARECOMP_KEEP_DISCARD=1: old no-op behaviour).
NVN(nvnCommandBufferDiscardColor) {
    static int keep = -1, logged;
    if (keep < 0) keep = getenv("LARECOMP_KEEP_DISCARD") != NULL;
    if (logged < 5) { logged++; LOG("[nvn] DiscardColor index %llu%s\n", (unsigned long long)c->x[1], keep ? " (kept)" : " -> clear"); }
    if (keep) return;
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_CLEAR_COLOR, 16, &s);
    memset(r->extra, 0, 16);
    r->x[2] = 0xF;  // all channels
    r->x[5] = 0;    // float clear
}
NVN(nvnCommandBufferDiscardDepthStencil) { (void)c; }


/* ---- M43A / S2a：只录不跑对账探针的直喂侧挂点（探针在 nvn_vk2/rec/m43a_rec_tap.c）
   为什么挂在这里而不是 gfx_bridge 的符号表：实测游戏的 NVN 调用走 **swhle 段的 HLE 直绑**
   （HLE_FUNC），gfx_bridge.c 的 entries[] 只服务 nvnDeviceGetProcAddress；S1/S2a 实测在 entries[]
   上挂 tap 一笔都收不到（logs/m43a/b 无 [m43a] 行）。这里是直喂侧唯一真正的咽喉：
   每一笔 draw 都在此写进玩家命令流，随后由 nvn_vk.c 直喂出图。
   传的 opcode 就是 r->op（刚写下的 CmdRec 头），不是我方"推断"的值。 */
extern void m43a_cb_draw(int kind, uint32_t d_op, int prim, int first, int count, int itype, uint64_t ibuf,
                         int base_v, int base_i, int inst);
#ifdef M44A_PROBE_OFF   /* M44A：对照构建里 draw 侧挂点同样在编译期消掉 */
#define m43a_cb_draw(kind, d_op, ...) ((void)(d_op))   /* d_op 恒为 r->op：消费它以免 unused-variable */
#endif

enum { M43A_K_ARRAYS = 0, M43A_K_ARRAYS_INST, M43A_K_ELEMENTS, M43A_K_ELEMENTS_BV, M43A_K_ELEMENTS_INST,
       M43A_K_ARRAYS_INDIRECT, M43A_K_ELEMENTS_INDIRECT };
// ---- draws (all integer arguments fit in x1..x7)
NVN(nvnCommandBufferDrawArrays) { Snap s = snap(c); CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0, &s); r->x[3] = 0; r->x[4] = 1; m17a_stash_lr(r, c);
    m43a_cb_draw(M43A_K_ARRAYS, r->op, (int)c->x[1], (int)c->x[2], (int)c->x[3], 0, 0, 0, 0, 1); }   /* M17A; M43A tap */
NVN(nvnCommandBufferDrawArraysInstanced) { Snap s = snap(c); CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0, &s); m17a_stash_lr(r, c);
    m43a_cb_draw(M43A_K_ARRAYS_INST, r->op, (int)c->x[1], (int)c->x[2], (int)c->x[3], 0, 0, 0, (int)c->x[4], (int)c->x[5]); }   /* M17A; M43A tap */
// Debug: LARECOMP_TRACE_COUNT=<n> logs the guest call chain of indexed draws with n indices.
static void trace_draw(Cpu *c, uint64_t count) {
    static long want = -2;
    if (want == -2) want = getenv("LARECOMP_TRACE_COUNT") ? atol(getenv("LARECOMP_TRACE_COUNT")) : -1;
    if (want < 0 || (long)count != want) return;
    static int n;
    if (n++ > 40) return;
    LOG("[trace] draw count %llu lr %llx chain:", (unsigned long long)count, (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 14 && fp >= GUEST_LO && fp < GUEST_LO + 0x1000000000ull && (fp & 7) == 0; i++) {
        uint64_t next = *(uint64_t *)(uintptr_t)fp, lr = *(uint64_t *)(uintptr_t)(fp + 8);
        LOG(" %llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    LOG("\n");
}

NVN(nvnCommandBufferDrawElements) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[4] = 0; r->x[5] = 0; r->x[6] = 1;  // base vertex, base instance, instances
    m17a_stash_lr(r, c);                    /* M17A */
    m43a_cb_draw(M43A_K_ELEMENTS, r->op, (int)c->x[1], 0, (int)c->x[3], (int)c->x[2], c->x[4], 0, 0, 1);   /* M43A tap */
}
NVN(nvnCommandBufferDrawElementsBaseVertex) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    /* M4ae P2：x[5] 高 32 位当「ABI 类别」标记（1=BaseVertex / 2=Instanced，0=普通 DrawElements），
       低 32 位仍是 baseInstance。回放侧按这个类别决定是否整类重放（SMO_REPLAY_BASEV/INST）。
       为什么要标记：实测这些 BaseVertex 调用的 baseVertex 大多为 0，只看值分不出「这一类」。 */
    r->x[5] = (1ull << 32); r->x[6] = 1;
    m17a_stash_lr(r, c);                    /* M17A */
    m43a_cb_draw(M43A_K_ELEMENTS_BV, r->op, (int)c->x[1], 0, (int)c->x[3], (int)c->x[2], c->x[4], (int)c->x[5], 0, 1);   /* M43A tap */
}
NVN(nvnCommandBufferDrawElementsInstanced) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[5] = (r->x[5] & 0xFFFFFFFFull) | (2ull << 32);   /* class=2，保留 baseInstance 低 32 位 */
    m17a_stash_lr(r, c);                    /* M17A */
    m43a_cb_draw(M43A_K_ELEMENTS_INST, r->op, (int)c->x[1], 0, (int)c->x[3], (int)c->x[2], c->x[4], (int)c->x[5], (int)c->x[6], (int)c->x[7]);   /* M43A tap */
}
NVN(nvnCommandBufferDrawArraysIndirect) { Snap s = snap(c); CmdRec *r = record(c, CMD_DRAW_ARRAYS_INDIRECT, 0, &s);
    m43a_cb_draw(M43A_K_ARRAYS_INDIRECT, r->op, (int)c->x[1], 0, 0, 0, c->x[2], 0, 0, 1); }
NVN(nvnCommandBufferDrawElementsIndirect) { Snap s = snap(c); CmdRec *r = record(c, CMD_DRAW_ELEMENTS_INDIRECT, 0, &s);
    m43a_cb_draw(M43A_K_ELEMENTS_INDIRECT, r->op, (int)c->x[1], 0, 0, (int)c->x[2], c->x[3], 0, 0, 1); }
NVN(nvnCommandBufferMultiDrawArraysIndirectCount) { (void)c; }
NVN(nvnCommandBufferMultiDrawElementsIndirectCount) { (void)c; }
NVN(nvnCommandBufferDrawTransformFeedback) { (void)c; }
NVN(nvnCommandBufferDrawTexture) { (void)c; }
PLAIN_T(T_DISPATCH, nvnCommandBufferDispatchCompute, CMD_DISPATCH)
PLAIN_T(T_DISPATCH_IND, nvnCommandBufferDispatchComputeIndirect, CMD_DISPATCH_INDIRECT)
NVN(nvnCommandBufferBeginTransformFeedback) { (void)c; }
NVN(nvnCommandBufferEndTransformFeedback) { (void)c; }
NVN(nvnCommandBufferPauseTransformFeedback) { (void)c; }
NVN(nvnCommandBufferResumeTransformFeedback) { (void)c; }

// ---- copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRef;
static void tex_ref(TexRef *t, uint64_t tex, uint64_t view, uint64_t region) {
    /* M4p P1：Copy* 族 / Downsample 的 texture 实参同样是游戏 NVNtexture*（tex_h 就在该地址），
       与 SetRenderTargets 同因同修：保留原始指针，不再 HOST() 二次解引用。 */
    t->tex = tex;
    if (view) { t->has_view = 1; memcpy(&t->view, (void *)(uintptr_t)view, sizeof t->view); }
    if (region) memcpy(t->region, (void *)(uintptr_t)region, sizeof t->region);
}
NVN(nvnCommandBufferCopyBufferToTexture) {  // (cmd, src, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[2], s.x[3], s.x[4]);
    CmdRec *r = record(c, CMD_COPY_BUFFER_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToBuffer) {  // (cmd, src, srcView, srcRegion, dst, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[1], s.x[2], s.x[3]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_BUFFER, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToTexture) {  // (cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], s.x[3]);
    tex_ref(&t[1], s.x[4], s.x[5], s.x[6]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
PLAIN_T(T_COPYB2B, nvnCommandBufferCopyBufferToBuffer, CMD_COPY_BUFFER_TO_BUFFER)
NVN(nvnCommandBufferDownsample) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], 0, 0);
    tex_ref(&t[1], s.x[2], 0, 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferDownsampleTextureView) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], 0);
    tex_ref(&t[1], s.x[3], s.x[4], 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferTiledDownsample) { (void)c; }
NVN(nvnCommandBufferTiledDownsampleTextureView) { (void)c; }
NVN(nvnCommandBufferResolveDepthBuffer) { (void)c; }
// UpdateUniformBuffer(cmd, buffer, size, offset, dataSize, data)
NVN(nvnCommandBufferUpdateUniformBuffer) {
    static uint64_t watch = ~0ull;  // LARECOMP_LOG_UBO_UPDATE=<hex address>: the updates that write it (caller, source)
    if (watch == ~0ull) watch = getenv("LARECOMP_LOG_UBO_UPDATE") ? strtoull(getenv("LARECOMP_LOG_UBO_UPDATE"), NULL, 16) : 0;
    if (watch && A(1) + A(3) <= watch && watch < A(1) + A(3) + A(4)) {
        static int n;
        if (n++ % 30 == 0 && n < 600) {
            const float *f = (const float *)(uintptr_t)(A(5) + (watch - A(1) - A(3)));
            LOG("[nvn] ubo update %llx+%#llx size %#llx from %llx (lr %llx): %g %g %g %g\n", (unsigned long long)A(1),
                (unsigned long long)A(3), (unsigned long long)A(4), (unsigned long long)A(5), (unsigned long long)c->x[30],
                f[0], f[1], f[2], f[3]);
        }
    }
#ifdef __ANDROID__
    extern void vk_ubo_inline_mark(uint64_t addr, uint64_t size);
    vk_ubo_inline_mark(A(1) + A(3), A(4));  // (that range stays copied per draw: vulkan/exec.c ubo_direct)
#endif
    record_copy(c, CMD_UPDATE_UNIFORM_BUFFER, 5, (uint32_t)A(4));
    M43B_TAP(T_UPDUB);   /* M43B */
}

// ---- synchronization and misc
NVN(nvnCommandBufferFenceSync) {
    // work executes at submit time, so the sync is satisfied once this list has run
    Snap s = snap(c);
    record(c, CMD_FENCE_SYNC, 0, &s);
    M43B_TAP(T_FENCE_SYNC);   /* M44A：录制侧只记 NVNsync*（fence 值在 exec 侧算），无客体回写 */
}
PLAIN(nvnCommandBufferWaitSync, CMD_WAIT_SYNC)
PLAIN_T(T_BARRIER, nvnCommandBufferBarrier, CMD_BARRIER)
PLAIN_T(T_SIGNAL_EVENT, nvnCommandBufferSignalEvent, CMD_SIGNAL_EVENT)   /* M44A：接进对账探针 */
PLAIN(nvnCommandBufferWaitEvent, CMD_WAIT_EVENT)
// ReportCounter(cmd, type, address): the counter goes to that GPU address, which here is the guest address of the
// memory (nvnMemoryPoolGetBufferAddress). An address outside guest memory is logged (once per caller) and dropped.
NVN(nvnCommandBufferReportCounter) {
    Snap s = snap(c);
    if (s.x[2] && (s.x[2] < GUEST_LO || s.x[2] >= GUEST_HI)) {
        static uint64_t seen[8];
        int k = 0;
        while (k < 8 && seen[k] && seen[k] != c->x[30]) k++;
        if (k < 8 && !seen[k]) {
            seen[k] = c->x[30];
            LOG("[nvn] ReportCounter type %llu to %llx: not guest memory, dropped (lr %llx)\n",
                (unsigned long long)s.x[1], (unsigned long long)s.x[2], (unsigned long long)c->x[30]);
        }
        s.x[2] = 0;
    }
    record(c, CMD_REPORT_COUNTER, 0, &s);
    /* M44A：tx[2] = 直喂侧**实际记下的**地址（非客体内存已被上面清零并记日志），
       探针拿同一口径去比对录制侧载荷 {type,addr}，不会把"按设计丢弃"误判成参数不符。 */
    { uint64_t tx[8]; memcpy(tx, c->x, sizeof tx); tx[2] = s.x[2]; M43B_TAP_X(T_REPORT_COUNTER, tx); }
}
NVN(nvnCommandBufferReportValue) { (void)c; }
NVN(nvnCommandBufferResetCounter) { (void)c; }
// A command handle is the address of a list's first record (a CMD_BARRIER: nvnCommandBufferBeginRecording). Handles
// that aren't one are logged (LARECOMP_CHECK_HANDLES=1).
void nvn_check_handles(Cpu *c, const char *what, int n, const uint64_t *h) {
    if (!ENV("LARECOMP_CHECK_HANDLES")) return;
    for (int i = 0; i < n; i++) {
        bool guest = h[i] >= GUEST_LO && h[i] < GUEST_HI;
        bool host = h[i] >= 0x6000000000ull;  // (malloc'd overflow memory: high host addresses)
        if ((!guest && !host) || ((const CmdRec *)(uintptr_t)h[i])->op != CMD_BARRIER) {
            static int n_logged;
            if (n_logged++ < 20) {
                LOG("[nvn] %s: handle %d of %d is %llx, not a command list (lr %llx)\n", what, i, n,
                    (unsigned long long)h[i], (unsigned long long)c->x[30]);
                hist_report(h[i]);
            }
        }
    }
}
// LARECOMP_CHECK_HANDLES: a submitted list's records are walked first; the first that isn't one is logged with the
// records before it.
static bool plausible(uint64_t a) { return (a >= GUEST_LO && a < GUEST_HI) || a >= 0x6000000000ull; }
void nvn_check_list(uint64_t h, int depth) {
    if (!ENV("LARECOMP_CHECK_HANDLES") || depth > 4) return;
    uint64_t p = h, prev[8] = {0};
    int np = 0;
    for (int i = 0; i < 1000000; i++) {
        if (!plausible(p)) { LOG("[nvn] list %llx: record %d at %llx: bad address\n", (unsigned long long)h, i, (unsigned long long)p); break; }
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        bool bad = r->op >= CMD_COUNT || (r->op != CMD_END && r->op != CMD_JUMP && (r->size < sizeof(CmdRec) || r->size > 0x100000));
        if (bad) {
            LOG("[nvn] list %llx: record %d at %llx: op %u size %u; before:", (unsigned long long)h, i, (unsigned long long)p, r->op, r->size);
            for (int k = 0; k < np && k < 8; k++) {
                const CmdRec *q = (const CmdRec *)(uintptr_t)prev[(np - 1 - k) % 8];
                LOG(" [%llx op %u size %u]", (unsigned long long)prev[(np - 1 - k) % 8], q->op, q->size);
            }
            LOG("\n");
            return;
        }
        prev[np++ % 8] = p;
        if (r->op == CMD_END) return;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        if (r->op == CMD_CALL) {
            const uint64_t *hh = (const uint64_t *)r->extra;
            for (uint64_t k = 0; k < r->x[0] && k < 64; k++) {
                if (!plausible(hh[k])) LOG("[nvn] list %llx: record %d (call) handle %llu is %llx\n", (unsigned long long)h, i, (unsigned long long)k, (unsigned long long)hh[k]);
                else nvn_check_list(hh[k], depth + 1);
            }
        }
        p += r->size;
    }
}

// M4an: CallCommands/CopyCommands used to live here (infra's copy_list + two NVN functions). They were unreachable
// from our symbol table (gfx_bridge mapped both names to flat accounting stubs), so they never ran. Moved up to the
// hardened call_commands() above (readable-bounded, budgeted, CALL marker for the opcode histogram) and removed here
// so there is exactly one implementation. The LARECOMP_CALL_REFS reference-mode knob is dropped with it.
NVN(nvnCommandBufferInsertDebugMarker) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerDynamic) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerStatic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroup) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupDynamic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupStatic) { (void)c; }
NVN(nvnCommandBufferPopDebugGroup) { (void)c; }
NVN(nvnCommandBufferPopDebugGroupId) { (void)c; }
