/* M6r: "is the ~45 s per step a host service call timing out?" -- a read-only ledger.
 *
 * The task asks for one thing: name the call that fails/times out, or prove that every call
 * succeeds.  Three pieces of evidence, all recording only, none deciding anything:
 *
 *   (1) FAILURE ledger.  Every kernel SVC whose result (x0) comes back non-zero, and every IPC
 *       reply whose nn::Result is non-zero, is recorded once in an aggregate row keyed by
 *       (call id, result) and -- self-limited -- in a 128-slot ring with the host time, the four
 *       dispatch arguments, the calling guest thread and the guest LR of the call site.
 *       The ring insert is throttled by the aggregate count (all of the first 64, then powers of
 *       two, then every 256th), so a 10k/s noisy failure cannot evict the rare ones; the
 *       aggregate keeps the true count.  Nothing calls runtime_now_ns() unless a record is
 *       actually written.
 *   (2) WAIT timing.  svc 0x18 (WaitSynchronization, the only kernel call with a caller-supplied
 *       timeout) records requested-ns / measured-duration / result; svc 0x0b (SleepThread)
 *       records its request.  Both only when the wait was long (>=200 ms) or timed out, plus
 *       whole-boot totals.  This is the piece that can *show* a 45 s timeout if one exists.
 *   (3) CLOCK callers.  Every nn::os::GetSystemTick read goes through loader.c's timer gate
 *       (runtime_dispatch); the guest LR of that read is counted per call site (16 slots).  A
 *       state machine that waits out a 45 s wall-clock deadline by polling the tick shows up
 *       here as one call site with a huge count.
 *
 * Alignment: runtime_fa_advance() is called from the 2 s change detector when `opens` moves or
 * the live guest thread count moves; it prints every failure recorded since the previous advance
 * with "ago=" so the report's alignment table is a straight read of the log.
 *
 * Nothing here changes a return value, a reply, a wait, or a thread state.
 */
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

extern void vk_boot_logf(const char *fmt, ...);
void runtime_describe_address(uint64_t a, char *out, size_t n);

#define FA_IPC_ID 0xFFFFu   /* aggregate key for an IPC reply (SVC numbers are < 0x100) */
#define FA_KIND_SLEEP 0x100u /* wait-ring tag: SleepThread */
#define FA_KIND_WAIT  0x101u /* wait-ring tag: WaitSynchronization */

/* ---------------- aggregate: (call, result) -> count ----------------
   key = bit48 ipc-flag | bits32..47 call id (SVC number, or IPC command) | bits0..31 result.
   A record always has result != 0, so the key is never 0 (0 marks an unused slot). */
#define FA_AGG 48
typedef struct { _Atomic uint64_t key; _Atomic uint64_t n; const char *name; } FaAgg;
static FaAgg fa_agg[FA_AGG];
static _Atomic unsigned fa_agg_used;
static _Atomic uint64_t fa_agg_over;

static uint64_t fa_agg_add(int ipc, uint32_t id, uint32_t ret, const char *name) {
    uint64_t f_id = id & 0xFFFFu;
    uint64_t key = (ipc ? 1ull << 48 : 0ull) | (f_id << 32) | (uint64_t)ret;
    if (!key) key = 1;   /* 0 marks an unused slot */
    unsigned used = atomic_load_explicit(&fa_agg_used, memory_order_acquire);
    for (unsigned i = 0; i < used && i < FA_AGG; i++)
        if (atomic_load_explicit(&fa_agg[i].key, memory_order_relaxed) == key)
            return atomic_fetch_add_explicit(&fa_agg[i].n, 1, memory_order_relaxed) + 1;
    unsigned id_slot = atomic_fetch_add_explicit(&fa_agg_used, 1, memory_order_relaxed);
    if (id_slot >= FA_AGG) { atomic_fetch_add_explicit(&fa_agg_over, 1, memory_order_relaxed); return 1; }
    fa_agg[id_slot].name = name;   /* written before the key that publishes the row */
    atomic_store_explicit(&fa_agg[id_slot].key, key, memory_order_relaxed);
    atomic_store_explicit(&fa_agg[id_slot].n, 1, memory_order_relaxed);
    return 1;
}

/* ---------------- ring: the last 128 recorded failures ---------------- */
#define FA_RING 128
typedef struct {
    uint64_t t;              /* host monotonic ns; 0 = slot not committed */
    uint64_t a[4];           /* dispatch arguments at the call */
    uint64_t lr;             /* guest LR of the call */
    const char *name;        /* IPC service name, NULL for a kernel SVC */
    uint32_t ret, thr, id, aux;
} FaRec;
static FaRec fa_ring[FA_RING];
static _Atomic unsigned fa_head;
static _Atomic uint64_t fa_svc_n, fa_ipc_n, fa_ring_n;

static void fa_rec(uint32_t id, uint32_t aux, const char *name, const uint64_t *a, uint32_t ret,
                   uint64_t lr, uint32_t thr) {
    uint64_t n = fa_agg_add(name != NULL, name ? aux : id, ret, name);
    if (!(n <= 64u || (n & (n - 1)) == 0 || (n % 256u) == 0)) return;   /* self-limiting ring */
    unsigned i = atomic_fetch_add_explicit(&fa_head, 1, memory_order_relaxed) % FA_RING;
    FaRec *r = &fa_ring[i];
    r->t = 0;
    for (int k = 0; k < 4; k++) r->a[k] = a ? a[k] : 0;
    r->lr = lr; r->name = name; r->ret = ret; r->thr = thr; r->id = id; r->aux = aux;
    r->t = runtime_now_ns();   /* commit last: a torn read at worst prints an older entry */
    atomic_fetch_add_explicit(&fa_ring_n, 1, memory_order_relaxed);
}

void runtime_fa_svc(uint32_t num, const uint64_t *a, uint32_t ret, uint64_t lr, uint32_t thr) {
    if (!ret) return;
    atomic_fetch_add_explicit(&fa_svc_n, 1, memory_order_relaxed);
    fa_rec(num, 0, NULL, a, ret, lr, thr);
}
void runtime_fa_ipc(const char *name, uint32_t cmd, uint32_t ret, uint64_t lr, uint32_t thr) {
    if (!ret) return;
    atomic_fetch_add_explicit(&fa_ipc_n, 1, memory_order_relaxed);
    fa_rec(FA_IPC_ID, cmd, name, NULL, ret, lr, thr);
}

/* ---------------- wait / sleep timing ---------------- */
#define FA_WRING 64
typedef struct { uint64_t t, req, dur, lr; uint64_t blk, core; uint32_t thr, n, h0, ret, kind; } FaWait;
static FaWait fa_wring[FA_WRING];
static _Atomic unsigned fa_whead;
static _Atomic uint64_t fa_wait_calls, fa_wait_slow, fa_wait_max, fa_sleep_calls, fa_sleep_max;

static void fa_long_add(const FaWait *w);

void runtime_fa_wait(uint32_t kind, uint32_t thr, uint32_t n, uint32_t h0, uint64_t req,
                     uint64_t dur, uint64_t blk, uint64_t core, uint32_t ret, uint64_t lr) {
    if (kind == FA_KIND_SLEEP) {
        atomic_fetch_add_explicit(&fa_sleep_calls, 1, memory_order_relaxed);
        uint64_t m = atomic_load_explicit(&fa_sleep_max, memory_order_relaxed);
        while (req > m && !atomic_compare_exchange_weak_explicit(&fa_sleep_max, &m, req,
                 memory_order_relaxed, memory_order_relaxed)) {}
        if (req < 200000000ull) return;                    /* only a long sleep is interesting */
    } else {
        atomic_fetch_add_explicit(&fa_wait_calls, 1, memory_order_relaxed);
        if (dur >= 200000000ull || ret == 0xEA01u) atomic_fetch_add_explicit(&fa_wait_slow, 1, memory_order_relaxed);
        uint64_t m = atomic_load_explicit(&fa_wait_max, memory_order_relaxed);
        while (dur > m && !atomic_compare_exchange_weak_explicit(&fa_wait_max, &m, dur,
                 memory_order_relaxed, memory_order_relaxed)) {}
        if (dur < 200000000ull && ret != 0xEA01u) return;  /* only a long or expired wait */
    }
    unsigned i = atomic_fetch_add_explicit(&fa_whead, 1, memory_order_relaxed) % FA_WRING;
    FaWait *w = &fa_wring[i];
    w->t = 0;
    w->req = req; w->dur = dur; w->blk = blk; w->core = core; w->lr = lr; w->thr = thr; w->n = n; w->h0 = h0;
    w->ret = ret; w->kind = kind;
    w->t = runtime_now_ns();
    if (kind != FA_KIND_SLEEP) fa_long_add(w);
}

/* ---------------- the longest waits of the whole boot ----------------
   The 64-slot ring above keeps the *newest* long waits, so a 30 s dump can miss the single long
   wait that matters (measured: a 36.9 s svc 0x18 wait ended between two dumps and was already
   evicted by the 0.3 s audio-worker waits). This table keeps the 8 longest ever, insert-sorted,
   so the answer survives to the end of the boot. */
#define FA_LONG 8
typedef struct { uint64_t dur, req, lr, t, blk, core; uint32_t thr, n, h0, ret, kind; } FaLong;
static FaLong fa_long[FA_LONG];
static _Atomic int fa_long_n;
static _Atomic uint64_t fa_long_guard;   /* simple spinlock: 8 rows, written on a long wait only */

static void fa_long_add(const FaWait *w) {
    while (atomic_exchange(&fa_long_guard, 1u)) { }
    int n = atomic_load_explicit(&fa_long_n, memory_order_relaxed);
    if (n < FA_LONG) {
        int at = n;
        while (at > 0 && fa_long[at - 1].dur < w->dur) { fa_long[at] = fa_long[at - 1]; at--; }
        fa_long[at] = (FaLong){ w->dur, w->req, w->lr, w->t, w->blk, w->core, w->thr, w->n, w->h0, w->ret, w->kind };
        atomic_store_explicit(&fa_long_n, n + 1, memory_order_relaxed);
    } else if (fa_long[FA_LONG - 1].dur < w->dur) {
        int at = FA_LONG - 1;
        while (at > 0 && fa_long[at - 1].dur < w->dur) { fa_long[at] = fa_long[at - 1]; at--; }
        fa_long[at] = (FaLong){ w->dur, w->req, w->lr, w->t, w->blk, w->core, w->thr, w->n, w->h0, w->ret, w->kind };
    }
    atomic_store_explicit(&fa_long_guard, 0u, memory_order_relaxed);
}

/* ---------------- clock gate callers ---------------- */
#define FA_TK 16
typedef struct { _Atomic uint64_t lr, n; } FaTk;
static FaTk fa_tk[FA_TK];
static _Atomic uint64_t fa_tk_n, fa_tk_over;

void runtime_fa_tick(uint64_t lr) {
    atomic_fetch_add_explicit(&fa_tk_n, 1, memory_order_relaxed);
    if (lr) {
        for (unsigned i = 0; i < FA_TK; i++)
            if (atomic_load_explicit(&fa_tk[i].lr, memory_order_relaxed) == lr) {
                atomic_fetch_add_explicit(&fa_tk[i].n, 1, memory_order_relaxed); return;
            }
        for (unsigned i = 0; i < FA_TK; i++) {
            uint64_t z = 0;
            if (atomic_compare_exchange_strong_explicit(&fa_tk[i].lr, &z, lr,
                    memory_order_relaxed, memory_order_relaxed)) {
                atomic_fetch_add_explicit(&fa_tk[i].n, 1, memory_order_relaxed); return;
            }
        }
    }
    atomic_fetch_add_explicit(&fa_tk_over, 1, memory_order_relaxed);
}

/* ---------------- printing ---------------- */
static _Atomic uint64_t fa_t0;
static uint64_t fa_base(void) {
    uint64_t b = atomic_load(&fa_t0);
    if (!b) { uint64_t e = 0; atomic_compare_exchange_strong(&fa_t0, &e, runtime_now_ns()); b = atomic_load(&fa_t0); }
    return b;
}
/* One compact "what did this call ask for" hint, per call id.  a3 of 0x18 is the requested
   timeout in ns; a0 of 0x0b is the requested sleep in ns.  Anything else prints the raw words
   because a wrong guess here must never look like a fact. */
static void fa_hint(char *out, size_t cap, uint32_t id, const uint64_t *a) {
    if (!a) { out[0] = 0; return; }
    if (id == 0x18) snprintf(out, cap, " n=%llu req=%.2fs hs=%#llx", (unsigned long long)a[2],
                             a[3] > 0 && a[3] < (1ull << 62) ? (double)a[3] / 1e9 : -1.0, (unsigned long long)a[1]);
    else if (id == 0x0b) snprintf(out, cap, " req=%.2fs", (double)a[0] / 1e9);
    else snprintf(out, cap, " a0=%#llx a2=%#llx a3=%#llx", (unsigned long long)a[0], (unsigned long long)a[2], (unsigned long long)a[3]);
}

static void fa_pick_top(uint64_t *vals, uint64_t *cnts, unsigned *idx, unsigned n) {
    for (unsigned i = 0; i < n; i++) idx[i] = i;
    for (unsigned i = 1; i < n; i++) {   /* insertion sort by count, descending; n<=40, once per 30 s */
        unsigned iv = idx[i], j = i;
        while (j > 0 && cnts[iv] > cnts[idx[j - 1]]) { idx[j] = idx[j - 1]; j--; }
        idx[j] = iv;
    }
    (void)vals;
}

void runtime_fa_dump(unsigned tsec) {
    uint64_t base = fa_base();
    uint64_t now = runtime_now_ns();
    double t = base ? (double)(now - base) / 1e9 : 0.0;
    uint64_t svc = atomic_load(&fa_svc_n), ipc = atomic_load(&fa_ipc_n), head = atomic_load(&fa_head);
    uint64_t rc = head < FA_RING ? head : FA_RING;
    vk_boot_logf("[fa] t=%us fails svc=%llu ipc=%llu slots=%llu/128 agg=%u/%u over=%llu "
                 "waits=%llu slow=%llu wmax=%.2fs sleeps=%llu smax=%.2fs tick=%llu(+%llu) per_s=%.2f",
                 tsec, (unsigned long long)svc, (unsigned long long)ipc, (unsigned long long)rc,
                 atomic_load(&fa_agg_used), (unsigned)FA_AGG, (unsigned long long)atomic_load(&fa_agg_over),
                 (unsigned long long)atomic_load(&fa_wait_calls), (unsigned long long)atomic_load(&fa_wait_slow),
                 (double)atomic_load(&fa_wait_max) / 1e9, (unsigned long long)atomic_load(&fa_sleep_calls),
                 (double)atomic_load(&fa_sleep_max) / 1e9, (unsigned long long)atomic_load(&fa_tk_n),
                 (unsigned long long)atomic_load(&fa_tk_over),
                 t > 0.5 ? (double)atomic_load(&fa_tk_n) / t : -1.0);
    {   /* aggregate top 8 by count: service+command+result for IPC, svc number+result for kernel */
        uint64_t keys[FA_AGG], ns[FA_AGG]; unsigned idx[FA_AGG];
        unsigned n = 0, used = atomic_load(&fa_agg_used);
        if (used > FA_AGG) used = FA_AGG;
        for (unsigned i = 0; i < used; i++) {
            uint64_t k = atomic_load_explicit(&fa_agg[i].key, memory_order_relaxed);
            uint64_t c = atomic_load_explicit(&fa_agg[i].n, memory_order_relaxed);
            if (!k || !c) continue;
            keys[n] = k; ns[n] = c; idx[n] = i; n++;
        }
        {
            unsigned o[FA_AGG];
            for (unsigned i = 0; i < n; i++) o[i] = i;
            for (unsigned i = 1; i < n; i++) {
                unsigned iv = o[i], j = i;
                while (j > 0 && ns[iv] > ns[o[j - 1]]) { o[j] = o[j - 1]; j--; }
                o[j] = iv;
            }
            for (unsigned i = 0; i < n && i < 8; i++) {
                unsigned r = o[i];
                uint64_t k = keys[r];
                uint32_t id = (uint32_t)((k >> 32) & 0xFFFFu), ret = (uint32_t)k;
                int ipc = (k >> 48) & 1;
                if (ipc) vk_boot_logf("[fa]   top#%u ipc=%s cmd=%u ret=%#x n=%llu", i,
                                      fa_agg[idx[r]].name ? fa_agg[idx[r]].name : "?", id, ret,
                                      (unsigned long long)ns[r]);
                else vk_boot_logf("[fa]   top#%u svc=%#x ret=%#x n=%llu%s", i, id, ret,
                                  (unsigned long long)ns[r], ret == 0xEA01u ? " TIMEOUT(0xEA01)" : "");
            }
        }
    }
    /* the last eight recorded failures, newest first */
    for (unsigned k = 0; k < 8 && head > k; k++) {
        const FaRec *r = &fa_ring[(head - 1 - k) % FA_RING];
        if (!r->t) continue;
        char h[96], d[80]; fa_hint(h, sizeof h, r->id, r->a);
        runtime_describe_address(r->lr, d, sizeof d);
        if (r->name) vk_boot_logf("[fa] last#%u ago=%.1fs ipc=%s cmd=%u ret=%#x thr=%#x lr=%s",
                                  k, base ? (double)(now - r->t) / 1e9 : -1.0, r->name, r->aux, r->ret, r->thr, d);
        else vk_boot_logf("[fa] last#%u ago=%.1fs svc=%#x ret=%#x thr=%#x%s lr=%s",
                          k, base ? (double)(now - r->t) / 1e9 : -1.0, r->id, r->ret, r->thr, h, d);
    }
    /* long / expired waits, newest first */
    unsigned wh = atomic_load(&fa_whead);
    for (unsigned k = 0; k < 6 && wh > k; k++) {
        const FaWait *w = &fa_wring[(wh - 1 - k) % FA_WRING];
        if (!w->t) continue;
        char d[80]; runtime_describe_address(w->lr, d, sizeof d);
        if (w->kind == FA_KIND_SLEEP)
            vk_boot_logf("[fa] wait#%u ago=%.1fs SLEEP req=%.2fs thr=%#x lr=%s",
                         k, base ? (double)(now - w->t) / 1e9 : -1.0, (double)w->req / 1e9, w->thr, d);
        else
            vk_boot_logf("[fa] wait#%u ago=%.1fs WAIT dur=%.2fs blk=%.2fs core=%.2fs req=%.2fs ret=%#x thr=%#x n=%u h0=%#x lr=%s",
                         k, base ? (double)(now - w->t) / 1e9 : -1.0, (double)w->dur / 1e9,
                         (double)w->blk / 1e9, (double)w->core / 1e9,
                         w->req < (1ull << 62) ? (double)w->req / 1e9 : -1.0, w->ret, w->thr, w->n, w->h0, d);
    }
    {   /* the 8 longest svc 0x18 waits of the whole boot, longest first */
        int n = atomic_load(&fa_long_n);
        for (int i = 0; i < n && i < FA_LONG; i++) {
            const FaLong *w = &fa_long[i];
            char d[80], hd[96]; runtime_describe_address(w->lr, d, sizeof d);
            { extern void runtime_handle_desc(uint32_t h, char *out, unsigned cap);
              runtime_handle_desc(w->h0, hd, sizeof hd); }
            vk_boot_logf("[fa] LONG#%d dur=%.2fs blk=%.2fs core=%.2fs req=%.2fs ret=%#x thr=%#x n=%u h0=%#x (%s) ended_ago=%.1fs lr=%s",
                         i, (double)w->dur / 1e9, (double)w->blk / 1e9, (double)w->core / 1e9,
                         w->req < (1ull << 62) ? (double)w->req / 1e9 : -1.0,
                         w->ret, w->thr, w->n, w->h0, hd, base ? (double)(now - w->t) / 1e9 : -1.0, d);
        }
    }
    /* who reads the game clock: top four call sites (live counts; the delta is in the caller) */
    {
        uint64_t lrs[FA_TK], ns[FA_TK]; unsigned idx[FA_TK], n = 0;
        for (unsigned i = 0; i < FA_TK; i++) {
            uint64_t l = atomic_load(&fa_tk[i].lr), c = atomic_load(&fa_tk[i].n);
            if (!l || !c) continue;
            lrs[n] = l; ns[n] = c; n++;
        }
        fa_pick_top(lrs, ns, idx, n);
        for (unsigned i = 0; i < n && i < 4; i++) {
            char d[80]; runtime_describe_address(lrs[idx[i]], d, sizeof d);
            vk_boot_logf("[fa]   tk#%u lr=%s n=%llu", i, d, (unsigned long long)ns[idx[i]]);
        }
    }
}

/* Called from the 2 s change detector when `opens` or the live thread count moves: everything
   recorded since the previous advance, with the age at the advance, so the alignment table is
   this block. */
static _Atomic uint64_t fa_adv_t;
static _Atomic uint64_t fa_adv_n;
void runtime_fa_advance(unsigned tsec, unsigned long long opens, int threads, const char *why) {
    uint64_t now = runtime_now_ns();
    uint64_t prev = atomic_exchange(&fa_adv_t, now);
    unsigned head = atomic_load(&fa_head);
    unsigned nshow = 0, ntot = 0;
    FaRec pick[8];
    for (unsigned k = 0; k < FA_RING && head > k; k++) {
        const FaRec *r = &fa_ring[(head - 1 - k) % FA_RING];
        if (!r->t) continue;
        if (prev && r->t <= prev) break;                  /* older than the previous advance */
        ntot++;
        if (nshow < 8) pick[nshow++] = *r;                /* newest first */
    }
    atomic_fetch_add(&fa_adv_n, 1);
    vk_boot_logf("[fa] ADVANCE#%llu t=%us why=%s opens=%llu thr=%d dt=%.1fs fails_since=%u",
                 (unsigned long long)atomic_load(&fa_adv_n), tsec, why, opens, threads,
                 prev ? (double)(now - prev) / 1e9 : -1.0, ntot);
    for (unsigned k = 0; k < nshow; k++) {
        const FaRec *r = &pick[nshow - 1 - k];            /* print oldest -> newest */
        char h[96], d[80]; fa_hint(h, sizeof h, r->id, r->a);
        if (r->name) vk_boot_logf("[fa] adv ago=%.1fs ipc=%s cmd=%u ret=%#x thr=%#x",
                                  (double)(now - r->t) / 1e9, r->name, r->aux, r->ret, r->thr);
        else { runtime_describe_address(r->lr, d, sizeof d);
            vk_boot_logf("[fa] adv ago=%.1fs svc=%#x ret=%#x thr=%#x%s lr=%s",
                         (double)(now - r->t) / 1e9, r->id, r->ret, r->thr, h, d); }
    }
    /* M6r: long waits that ENDED inside this window -- the other half of the alignment question
       ("was the step preceded by a wait finishing just before it?"). >=1 s only: the 0.3 s audio
       poll would bury it. */
    {
        unsigned wh = atomic_load(&fa_whead);
        unsigned shown = 0;
        for (unsigned k = 0; k < FA_WRING && wh > k; k++) {
            const FaWait *w = &fa_wring[(wh - 1 - k) % FA_WRING];
            if (!w->t) continue;
            if (prev && w->t <= prev) break;
            if (w->kind == FA_KIND_SLEEP ? w->req < 1000000000ull : w->dur < 1000000000ull) continue;
            if (shown >= 6) continue;
            char d[80], hd[96]; runtime_describe_address(w->lr, d, sizeof d);
            { extern void runtime_handle_desc(uint32_t h, char *out, unsigned cap);
              runtime_handle_desc(w->h0, hd, sizeof hd); }
            shown++;
            if (w->kind == FA_KIND_SLEEP)
                vk_boot_logf("[fa] adv SLEEP req=%.2fs ended_ago=%.1fs thr=%#x lr=%s",
                             (double)w->req / 1e9, (double)(now - w->t) / 1e9, w->thr, d);
            else
                vk_boot_logf("[fa] adv WAIT dur=%.2fs req=%.2fs ret=%#x thr=%#x n=%u h0=%#x (%s) ended_ago=%.1fs lr=%s",
                             (double)w->dur / 1e9, w->req < (1ull << 62) ? (double)w->req / 1e9 : -1.0,
                             w->ret, w->thr, w->n, w->h0, hd, (double)(now - w->t) / 1e9, d);
        }
    }
}
