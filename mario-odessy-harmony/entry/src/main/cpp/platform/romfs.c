/* Read-only game RomFS, backed by the player's fully IVFC-verified image. */
#include "runtime.h"
#include "totk_ui_lang.h"
#include "romfs_config.h"
#include "game_version.h"
#include "fs_host.h"
#include "romfs_path.h"
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#define NOT_FOUND 0x202u
#define UNSUPPORTED 0x6402u
#define INVALID 0xe402u
#define NH 65536
static uint8_t *index_bytes;
static size_t index_size;
static uint64_t file_count,strings_offset;
static int image_fd=-1;
/* Split image: romfs.bin.000, .001, ... of equal size (the last one shorter), read in place. The phone gets the 17 GB
   image this way: devicectl drops the connection on a single file of that size, and joining the parts on the device
   would need the space twice. image_fd is then the first part; src_pread routes image offsets to their part. */
#define MAX_CHUNKS 64
static int chunk_fds[MAX_CHUNKS];
static unsigned nchunks;
static uint64_t chunk_size;
static ssize_t src_pread(int fd,void *buf,size_t n,off_t off) {
 if(!nchunks||fd!=image_fd)return pread(fd,buf,n,off);
 uint64_t c=(uint64_t)off/chunk_size,o=(uint64_t)off%chunk_size;if(c>=nchunks)return 0;
 if(n>chunk_size-o)n=(size_t)(chunk_size-o);
 return pread(chunk_fds[c],buf,n,(off_t)o);
}
static const uint8_t *image_map;
static char mount_name[32];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t read_calls,read_bytes,read_ns,read_prepare_ns,read_report_ns;
/* ---------------- M5f P1: RomFS I/O ledger (read-only counters, always on) ----------------
   Question: what the guest is stuck on during the static loading frame. Three pieces of evidence:
   (1) opens/notfound/reads/bytes for the whole boot; (2) inflight + inflight_since: how many reads are
   in progress right now and when the oldest started - inflight>0 for tens of seconds means "a read on
   some fd is waiting"; (3) a ring of the last 8 opened paths (newest first) so a dump can say which file
   was last opened and how long ago. Only atomic adds and one ring store are added; the read semantics and
   the return codes are untouched. */
static _Atomic uint64_t g_io_opens, g_io_notfound, g_io_rd, g_io_bytes;
static _Atomic unsigned g_io_inflight;
static _Atomic uint64_t g_io_inflight_since;
/* M7i: the ring that answers "which files were opened, in order" was 8 slots, so the whole boot's
   per-open list could never be printed (M7H: 108 opens on the device, 1244 on the emulator, only the
   last 8 visible on either side). Storage is now 2048 slots; the legacy dump keeps printing 8 so every
   existing log line stays byte-identical, and runtime_romfs_open_ring_dump() below drains the rest.
   Each slot also records the owning guest thread and a monotonic sequence number, so a reader can tell
   "which thread opened what, when" and detect a reused slot. Cost per open is unchanged (one snprintf,
   one timestamp, two stores); BSS grows by (2048-8) * sizeof(slot) ≈ 280 KB. */
#define IO_PATH_RING 2048
#define IO_PATH_DUMP 8
#define IO_PATH_MAX 128
static struct { char path[IO_PATH_MAX]; uint64_t ns; unsigned seq; uint32_t thr; } g_io_last[IO_PATH_RING];
static _Atomic unsigned g_io_last_i;
static void io_note_open(const char *path) {
    unsigned n = atomic_fetch_add_explicit(&g_io_last_i, 1, memory_order_relaxed);
    unsigned i = n % IO_PATH_RING;
    /* Must copy: the path comes from the guest and is usually a temporary heap buffer whose storage has
       been reused by the time a dump runs (tens of seconds later). The first version stored the pointer
       and printed 7 lines of garbage ("\x7f") in the M5f b1 boot ([io] open#-1..-7). */
    g_io_last[i].seq = n + 1u;
    g_io_last[i].thr = runtime_thread_current_handle();
    snprintf(g_io_last[i].path, IO_PATH_MAX, "%s", path ? path : "(null)");
    g_io_last[i].ns = runtime_now_ns();
    runtime_io_note(1, 0, path ? path : "(null)");   /* M6i: charge the open to the calling guest thread */
}
/* M7i: drain every recorded open once, oldest first, with #, process-uptime, owning thread and path.
   *cursor is owned by the caller (0 on its first call). Slots the ring has already reused are skipped
   by comparing the stored sequence number, so a reader never prints a stale row as if it were new.
   Bounded by max_lines so a late drain cannot stall the heartbeat thread. Returns the lines printed. */
unsigned runtime_romfs_open_ring_dump(unsigned long long *cursor, unsigned max_lines) {
    extern void vk_boot_logf(const char *fmt, ...);
    if (!cursor || !max_lines) return 0;
    unsigned top = atomic_load_explicit(&g_io_last_i, memory_order_relaxed);
    unsigned long long c = *cursor;
    unsigned printed = 0;
    if (c > (unsigned long long)top) c = top > IO_PATH_RING ? (unsigned long long)top - IO_PATH_RING : 0ull;
    if ((unsigned long long)top - c > IO_PATH_RING) c = (unsigned long long)top - IO_PATH_RING;
    for (; c < (unsigned long long)top && printed < max_lines; c++) {
        unsigned i = (unsigned)(c % IO_PATH_RING);
        if (!g_io_last[i].seq || g_io_last[i].seq != (unsigned)(c + 1u)) continue;   /* slot reused */
        vk_boot_logf("[ldrd] open #%u t=%.3fs thr=%#x path=%s", g_io_last[i].seq,
                     g_io_last[i].ns ? (double)g_io_last[i].ns / 1e9 : -1.0,
                     g_io_last[i].thr, g_io_last[i].path);
        printed++;
    }
    *cursor = c;
    return printed;
}
static uint64_t handles[NH]; /* file index + 1; 0 is free */
/* ================= M40T (2026-11-10, read-only probe `smo_m40t`) =================
   Question: why does a RomFS read cost ~6.4 s for ~217 KB (34-44 KB/s) in the same boot that
   once did 16.5 MB/s (t≈1118), with inflight=0? Per-read (path,off,size,us) timing, p50/p95/max,
   slow top5, duplicate-range hits, OPENS_FROZEN and BURST events. Everything below only reads
   counters and adds its own bookkeeping: with smo_m40t=0 the read path is byte-identical (one
   integer compare per read), no guest read, no log line. */
#define M40T_SW_A "/data/storage/el2/base/files/smo_m40t"
#define M40T_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40t"
#define M40T_N    4096u    /* per-read io-us ring (cumulative distribution source) */
#define M40T_DUP  1024u    /* (name,off,size) duplicate-range table */
#define M40T_SLOW 5u
#define M40T_RATE 64u
#define M40T_DUMP_NS 60000000000ull
static int m40t_on(void)
{
    static int v = -1; static unsigned tick;
    if (v >= 0 && (tick++ & 7u)) return v;
    v = 0;
    { static const char *const p[2] = { M40T_SW_A, M40T_SW_B };
      for (int i = 0; i < 2; i++) { FILE *f = fopen(p[i], "r");
        if (f) { int x = 0; if (fscanf(f, "%d", &x) == 1 && x > 0) v = 1; fclose(f); break; } } }
    return v;
}
__attribute__((constructor)) static void m40t_seed(void)
{
    static const char *const p[2] = { M40T_SW_A, M40T_SW_B };
    for (int i = 0; i < 2; i++) {
        FILE *r = fopen(p[i], "r");
        if (r) { fclose(r); continue; }
        { FILE *f = fopen(p[i], "w"); if (f) { fputs("0\n", f); fclose(f); } }
    }
}
/* ================= M40U (2026-11-10, read-only probe `smo_m40u`) =================
   Question: the loader is done (M40T: opens frozen at 1244, 1.06 GB/s, inflight=0) yet the game
   spins on a white screen (cmd/frame x13, img/frame x28). What is the main thread waiting on?
   The detector rides the same tick M40T uses: the DummyBlank audio stream keeps reading ~34 KB/s
   after the loader stops, so the RomFS read path is a heartbeat even while `opens` is frozen
   (the [io] romfs counter line only fires on a gdump request, so it cannot be the only hook).
     - `opens` unchanged for 15 s -> one [m40u] FROZEN line, remember frozen_at;
     - frozen_at + 15 s         -> ONE directed dump (every guest thread h/wk/wa/svc + owner,
                                   RT table, main/draw_flush sync ledger). Never repeats.
   Unit self-check (M40T lesson: a wrong ns->s factor is silent, and winRate came out 1e6 too big):
   the first [m40u] line prints the raw nanosecond counter AND the /1e9 value, so the unit can be
   verified from the log alone without trusting the formatter.
   With smo_m40u=0: one predictable integer compare per read, no log line, no guest read. */
#define M40U_SW_A "/data/storage/el2/base/files/smo_m40u"
#define M40U_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40u"
#define M40U_WIN_NS 15000000000ull   /* 15 s freeze window, then 15 s more before the dump */
static int m40u_on(void)
{
    static int v = -1; static unsigned tick;
    if (v >= 0 && (tick++ & 7u)) return v;
    v = 0;
    { static const char *const p[2] = { M40U_SW_A, M40U_SW_B };
      for (int i = 0; i < 2; i++) { FILE *f = fopen(p[i], "r");
        if (f) { int x = 0; if (fscanf(f, "%d", &x) == 1 && x > 0) v = 1; fclose(f); break; } } }
    return v;
}
__attribute__((constructor)) static void m40u_seed(void)
{
    static const char *const p[2] = { M40U_SW_A, M40U_SW_B };
    for (int i = 0; i < 2; i++) {
        FILE *r = fopen(p[i], "r");
        if (r) { fclose(r); continue; }
        { FILE *f = fopen(p[i], "w"); if (f) { fputs("0\n", f); fclose(f); } }
    }
}
static uint64_t m40u_opens_last, m40u_opens_chg_ns, m40u_frozen_ns;
static uint64_t m40u_frames_last, m40u_frames_chg_ns;
static int m40u_state;   /* 0=watching 1=frozen logged 2=dumped 3=aborted (opens moved after FROZEN) */
static void m40u_tick(void)
{
    extern void vk_boot_logf(const char *fmt, ...);
    extern _Atomic unsigned long g_beat_frames, g_beat_present_try, g_beat_img, g_beat_cmd;
    if (!m40u_on()) return;
    uint64_t now = runtime_now_ns();
    uint64_t opens = atomic_load(&g_io_opens);
    uint64_t frames = (uint64_t)atomic_load_explicit(&g_beat_frames, memory_order_relaxed);
    if (!m40u_opens_chg_ns) { m40u_opens_chg_ns = now; m40u_opens_last = opens; }
    if (!m40u_frames_chg_ns) { m40u_frames_chg_ns = now; m40u_frames_last = frames; }
    if (opens != m40u_opens_last) {   /* loader moved again: restart the window, drop a pending dump */
        m40u_opens_last = opens; m40u_opens_chg_ns = now;
        if (m40u_state == 1) m40u_state = 3;
        return;
    }
    if (frames != m40u_frames_last) { m40u_frames_last = frames; m40u_frames_chg_ns = now; }
    if (m40u_state == 0 && now - m40u_opens_chg_ns >= M40U_WIN_NS) {
        m40u_state = 1; m40u_frozen_ns = now;
        vk_boot_logf("[m40u] UNITS now_ns=%llu now_s=%u opens=%llu reads=%llu bytes=%llu win_ns=%llu win_s=%u",
                     (unsigned long long)now, (unsigned)(now / 1000000000ull),
                     (unsigned long long)opens, (unsigned long long)atomic_load(&g_io_rd),
                     (unsigned long long)atomic_load(&g_io_bytes),
                     (unsigned long long)M40U_WIN_NS, (unsigned)(M40U_WIN_NS / 1000000000ull));
        vk_boot_logf("[m40u] FROZEN t=%us opens=%llu reads=%llu bytes=%llu frozen_since_s=%u",
                     (unsigned)(now / 1000000000ull), (unsigned long long)opens,
                     (unsigned long long)atomic_load(&g_io_rd), (unsigned long long)atomic_load(&g_io_bytes),
                     (unsigned)((now - m40u_opens_chg_ns) / 1000000000ull));
        return;
    }
    if (m40u_state == 1 && now - m40u_frozen_ns >= M40U_WIN_NS) {
        m40u_state = 2;   /* one shot, whatever the dump does below */
        { extern void runtime_m40u_dump(unsigned long, unsigned long, unsigned long, unsigned long, unsigned) __attribute__((weak));
          if (runtime_m40u_dump)
              runtime_m40u_dump((unsigned long)frames,
                                (unsigned long)atomic_load_explicit(&g_beat_present_try, memory_order_relaxed),
                                (unsigned long)atomic_load_explicit(&g_beat_img, memory_order_relaxed),
                                (unsigned long)atomic_load_explicit(&g_beat_cmd, memory_order_relaxed),
                                (unsigned)((now - m40u_frames_chg_ns) / 1000000000ull)); }
    }
}
static unsigned m40t_us[M40T_N]; static unsigned m40t_usi;
static uint64_t m40t_n, m40t_bytes, m40t_io_us_sum, m40t_prep_us_sum, m40t_slow100;
static uint64_t m40t_win_n, m40t_win_bytes, m40t_win_us;
static struct { uint32_t us; uint64_t off; uint32_t size; const char *name; } m40t_slow[M40T_SLOW];
static unsigned m40t_nslow;
static struct { const char *name; uint64_t off; uint32_t size; uint32_t hits; unsigned used; } m40t_dup[M40T_DUP];
static uint64_t m40t_dup_reread, m40t_dup_reads, m40t_dup_last_hits; static unsigned m40t_dup_lines;
static struct { uint64_t ns, bytes, reads; } m40t_rate[M40T_RATE]; static unsigned m40t_nrate, m40t_ratei;
static uint64_t m40t_last_dump_ns, m40t_last_slow_ns, m40t_cfg_done;
static uint64_t m40t_opens_seen, m40t_frozen_ns; static int m40t_frozen_done;
static uint64_t m40t_burst_ns; static unsigned m40t_burst_lines;
static int m40t_us_cmp(const void *a, const void *b)
{ unsigned x = *(const unsigned *)a, y = *(const unsigned *)b; return x < y ? -1 : (x > y); }
static void m40t_dump(uint64_t now)
{
    extern void vk_boot_logf(const char *fmt, ...);
    unsigned ns = m40t_n < M40T_N ? (unsigned)m40t_n : M40T_N;
    static unsigned sorted[M40T_N];
    unsigned samples = 0;
    /* the ring is written under the read path only; a torn read here only shifts a percentile */
    for (unsigned i = 0; i < ns && i < M40T_N; i++) { unsigned v = m40t_us[i]; if (v || i < ns) sorted[samples++] = v; }
    qsort(sorted, samples, sizeof sorted[0], m40t_us_cmp);
    unsigned p50 = samples ? sorted[samples / 2] : 0;
    unsigned p95 = samples ? sorted[(samples * 95u) / 100u >= samples ? samples - 1 : (samples * 95u) / 100u] : 0;
    unsigned mx = samples ? sorted[samples - 1] : 0;
    uint64_t totkb = m40t_bytes / 1024ull;
    vk_boot_logf("[m40t] rd t=%us n=%u totKB=%llu p50us=%u p95us=%u maxus=%u slow(>100ms)=%llu",
                 (unsigned)(now / 1000000000ull), (unsigned)m40t_n, (unsigned long long)totkb,
                 p50, p95, mx, (unsigned long long)m40t_slow100);
    {
        uint64_t mean = m40t_n ? m40t_bytes / m40t_n : 0;
        unsigned dur_s = m40t_nrate >= 2 ? (unsigned)((m40t_rate[(m40t_ratei + M40T_RATE - 1u) % M40T_RATE].ns
                                                      - m40t_rate[(m40t_ratei + M40T_RATE - 2u) % M40T_RATE].ns) / 1000000000ull) : 0;
        unsigned wrate = 0;
        if (m40t_nrate >= 2) {
            uint64_t b1 = m40t_rate[(m40t_ratei + M40T_RATE - 1u) % M40T_RATE].bytes;
            uint64_t b0 = m40t_rate[(m40t_ratei + M40T_RATE - 2u) % M40T_RATE].bytes;
            uint64_t n1 = m40t_rate[(m40t_ratei + M40T_RATE - 1u) % M40T_RATE].ns;
            uint64_t n0 = m40t_rate[(m40t_ratei + M40T_RATE - 2u) % M40T_RATE].ns;
            if (n1 > n0) wrate = (unsigned)((b1 - b0) * 1000000ull / (n1 - n0) / 1024ull);
        }
        unsigned infl = atomic_load_explicit(&g_io_inflight, memory_order_relaxed);
        vk_boot_logf("[m40t] x win=%us winN=%llu winKB=%llu winRate=%uKB/s meanKB=%llu prepMS=%llu ioMS=%llu dup=%llu/%llu opens=%llu reads=%llu bytesMB=%llu inflight=%u mount=%s",
                     dur_s, (unsigned long long)m40t_win_n, (unsigned long long)(m40t_win_bytes / 1024ull), wrate,
                     (unsigned long long)(mean / 1024ull), (unsigned long long)(m40t_prep_us_sum / 1000ull),
                     (unsigned long long)(m40t_io_us_sum / 1000ull), (unsigned long long)m40t_dup_reread,
                     (unsigned long long)m40t_dup_reads,
                     (unsigned long long)atomic_load(&g_io_opens), (unsigned long long)atomic_load(&g_io_rd),
                     (unsigned long long)(atomic_load(&g_io_bytes) >> 20), infl, mount_name[0] ? mount_name : "-");
        m40t_win_n = 0; m40t_win_bytes = 0; m40t_win_us = 0;
    }
    if (!m40t_cfg_done) {
        m40t_cfg_done = 1;
        vk_boot_logf("[m40t] cfg chunks=%u chunkMB=%llu mmap=%d image_fd=%d indexMB=%llu files=%llu",
                     nchunks, (unsigned long long)(chunk_size >> 20), image_map ? 1 : 0, image_fd,
                     (unsigned long long)(index_size >> 20), (unsigned long long)file_count);
    }
    /* OPENS_FROZEN: opens stopped growing for >20 s (needs ≥2 dumps to be a stable observation) */
    {
        uint64_t o = atomic_load(&g_io_opens);
        if (o == m40t_opens_seen) {
            if (!m40t_frozen_ns) m40t_frozen_ns = now;
            else if (!m40t_frozen_done && now - m40t_frozen_ns >= 20000000000ull) {
                m40t_frozen_done = 1;
                vk_boot_logf("[m40t] OPENS_FROZEN t=%us reads=%llu bytes=%llu",
                             (unsigned)(now / 1000000000ull), (unsigned long long)atomic_load(&g_io_rd),
                             (unsigned long long)atomic_load(&g_io_bytes));
            }
        } else { m40t_opens_seen = o; m40t_frozen_ns = 0; }
    }
    /* slow top5 (at most every 120 s, and only if entries exist) */
    if (m40t_nslow && (!m40t_last_slow_ns || now - m40t_last_slow_ns >= 120000000000ull)) {
        m40t_last_slow_ns = now;
        for (unsigned i = 0; i < m40t_nslow; i++)
            vk_boot_logf("[m40t] slow off=%llu size=%u us=%u path=%s", (unsigned long long)m40t_slow[i].off,
                         m40t_slow[i].size, m40t_slow[i].us, m40t_slow[i].name ? m40t_slow[i].name : "?");
    }
    /* dup: up to 8 lines total, only ranges reread ≥3 times */
    if (m40t_dup_lines < 8) {
        for (unsigned i = 0; i < M40T_DUP && m40t_dup_lines < 8; i++) {
            if (!m40t_dup[i].used || m40t_dup[i].hits < 3 || m40t_dup[i].hits <= m40t_dup_last_hits) continue;
            m40t_dup_last_hits = m40t_dup[i].hits; m40t_dup_lines++;
            vk_boot_logf("[m40t] dup path=%s off=%llu size=%u hits=%u", m40t_dup[i].name ? m40t_dup[i].name : "?",
                         (unsigned long long)m40t_dup[i].off, m40t_dup[i].size, m40t_dup[i].hits);
        }
    }
}
/* BURST: a trailing-window rate >1 MB/s. The window is the newest rate sample vs the newest one
   at least 4 s older, so a 6.4 s-per-read crawl can never be mistaken for a burst. */
static void m40t_burst_check(uint64_t now, uint64_t bytes, uint64_t reads, uint64_t opens)
{
    extern void vk_boot_logf(const char *fmt, ...);
    if (m40t_nrate < 2) return;
    unsigned newest = (m40t_ratei + M40T_RATE - 1u) % M40T_RATE;
    unsigned pick = newest;
    for (unsigned k = 2; k <= m40t_nrate && k < M40T_RATE; k++) {
        unsigned j = (m40t_ratei + M40T_RATE - k) % M40T_RATE;
        if (m40t_rate[newest].ns - m40t_rate[j].ns >= 4000000000ull) pick = j; else break;
    }
    if (pick == newest) return;
    uint64_t dt = m40t_rate[newest].ns - m40t_rate[pick].ns, db = m40t_rate[newest].bytes - m40t_rate[pick].bytes;
    if (!dt) return;
    uint64_t kbps = db * 1000000ull / dt / 1024ull;
    if (kbps <= 1024ull) return;
    if (m40t_burst_ns && now - m40t_burst_ns < 60000000000ull) return;
    m40t_burst_ns = now;
    if (m40t_burst_lines >= 8) return;
    m40t_burst_lines++;
    vk_boot_logf("[m40t] BURST t=%us rate=%uKB/s winMS=%llu dKB=%llu opens=%llu reads=%llu bytesMB=%llu",
                 (unsigned)(now / 1000000000ull), (unsigned)kbps, (unsigned long long)(dt / 1000000ull),
                 (unsigned long long)(db / 1024ull), (unsigned long long)opens, (unsigned long long)reads,
                 (unsigned long long)(bytes >> 20));
}
static void m40t_record(const char *name, uint64_t off, uint64_t size, uint32_t io_us, uint32_t prep_us)
{
    uint64_t now = runtime_now_ns();
    m40t_us[m40t_usi++ % M40T_N] = io_us;
    m40t_n++; m40t_bytes += size; m40t_io_us_sum += io_us; m40t_prep_us_sum += prep_us;
    m40t_win_n++; m40t_win_bytes += size; m40t_win_us += io_us;
    if (io_us > 100000u) m40t_slow100++;
    if (m40t_nslow < M40T_SLOW || io_us > m40t_slow[M40T_SLOW - 1].us) {
        unsigned k = m40t_nslow < M40T_SLOW ? m40t_nslow : M40T_SLOW - 1u;
        if (m40t_nslow < M40T_SLOW) m40t_nslow++;
        while (k > 0 && m40t_slow[k - 1].us < io_us) { m40t_slow[k] = m40t_slow[k - 1]; k--; }
        m40t_slow[k].us = io_us; m40t_slow[k].off = off; m40t_slow[k].size = (uint32_t)size; m40t_slow[k].name = name;
    }
    m40t_dup_reads++;
    {
        uint64_t h = ((uint64_t)(uintptr_t)name >> 4) * 0x9E3779B97F4A7C15ull;
        h ^= off * 0xC2B2AE3D27D4EB4Full ^ (size << 17);
        unsigned slot = (unsigned)(h >> 40) % M40T_DUP, found = 0;
        for (unsigned probe = 0; probe < 8; probe++) {
            unsigned s = (slot + probe) % M40T_DUP;
            if (!m40t_dup[s].used) {
                m40t_dup[s].used = 1; m40t_dup[s].name = name; m40t_dup[s].off = off;
                m40t_dup[s].size = (uint32_t)size; m40t_dup[s].hits = 1; found = 2; break;
            }
            if (m40t_dup[s].name == name && m40t_dup[s].off == off && m40t_dup[s].size == (uint32_t)size) {
                m40t_dup[s].hits++; if (m40t_dup[s].hits >= 2) m40t_dup_reread++;
                found = 1; break;
            }
        }
        (void)found;
    }
    {   /* rate sample at ≥2 s spacing, then the burst test */
        unsigned newest = m40t_nrate ? (m40t_ratei + M40T_RATE - 1u) % M40T_RATE : 0;
        uint64_t opens = atomic_load(&g_io_opens), reads = atomic_load(&g_io_rd), bytes = atomic_load(&g_io_bytes);
        if (!m40t_nrate || now - m40t_rate[newest].ns >= 2000000000ull) {
            m40t_rate[m40t_ratei++ % M40T_RATE].ns = now;
            m40t_rate[(m40t_ratei + M40T_RATE - 1u) % M40T_RATE].bytes = bytes;
            m40t_rate[(m40t_ratei + M40T_RATE - 1u) % M40T_RATE].reads = reads;
            if (m40t_nrate < M40T_RATE) m40t_nrate++;
        } else {
            m40t_rate[newest].bytes = bytes; m40t_rate[newest].reads = reads;
        }
        m40t_burst_check(now, bytes, reads, opens);
    }
    if (!m40t_last_dump_ns) m40t_last_dump_ns = now;
    else if (now - m40t_last_dump_ns >= M40T_DUMP_NS) { m40t_last_dump_ns = now; m40t_dump(now); }
}
/* ================= M40Y (2026-11-10, read-only probe `smo_m40y`) =================
   Question: what are the 5 RomFS open failures (notfound=5, constant from early boot to the end) —
   the full request path, the normalized path, which reject site, when, and whether anyone retries
   them? The counter line is the ordered hook: `[io] romfs` prints on the heartbeat thread, so the
   60 s summary rides there, never on the read path (M40U: reads can stop while the frame freezes).
   With smo_m40y=0 nothing is logged and every return code stays byte-identical. */
#define M40Y_SW_A "/data/storage/el2/base/files/smo_m40y"
#define M40Y_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40y"
#define M40Y_PATHS 16u
#define M40Y_DUMP_NS 60000000000ull
static _Atomic unsigned m40y_nf, m40y_retry;
static char m40y_p[M40Y_PATHS][IO_PATH_MAX * 4];
static unsigned m40y_n[M40Y_PATHS], m40y_np;
static uint64_t m40y_last_ns;
extern void vk_boot_logf(const char *fmt, ...);
static int m40y_on(void)
{
    static int v = -1; static unsigned tick;
    if (v >= 0 && (tick++ & 7u)) return v;
    v = 0;
    { static const char *const p[2] = { M40Y_SW_A, M40Y_SW_B };
      for (int i = 0; i < 2; i++) { FILE *f = fopen(p[i], "r");
        if (f) { int x = 0; if (fscanf(f, "%d", &x) == 1 && x > 0) v = 1; fclose(f); break; } } }
    return v;
}
__attribute__((constructor)) static void m40y_seed(void)
{
    /* The switch has to exist before hdc can write it: the shell domain cannot create files in the
       app data dir, so the app seeds both views once (same pattern as M40U/M6q). */
    static const char *const p[2] = { M40Y_SW_A, M40Y_SW_B };
    for (int i = 0; i < 2; i++) {
        FILE *r = fopen(p[i], "r");
        if (r) { fclose(r); continue; }
        { FILE *f = fopen(p[i], "w"); if (f) { fputs("0\n", f); fclose(f); } }
    }
    (void)m40y_on();
}
static void m40y_tick(void)
{
    if (!m40y_on()) return;
    uint64_t now = runtime_now_ns();
    if (!m40y_last_ns) { m40y_last_ns = now; return; }
    if (now - m40y_last_ns < M40Y_DUMP_NS) return;
    m40y_last_ns = now;
    vk_boot_logf("[m40y] sum t=%.1fs nf=%u retry=%u opens=%llu notfound=%llu bytesMB=%llu inflight=%u",
                 (double)now / 1e9, atomic_load_explicit(&m40y_nf, memory_order_relaxed),
                 atomic_load_explicit(&m40y_retry, memory_order_relaxed),
                 (unsigned long long)atomic_load(&g_io_opens),
                 (unsigned long long)atomic_load(&g_io_notfound),
                 (unsigned long long)(atomic_load(&g_io_bytes) >> 20),
                 atomic_load_explicit(&g_io_inflight, memory_order_relaxed));
}
void runtime_romfs_stat_dump(void) {
    extern void vk_boot_logf(const char *fmt, ...);
    unsigned long long hsum = 0;
    pthread_mutex_lock(&lock);
    for (unsigned i = 1; i < NH; i++) if (handles[i]) hsum++;
    pthread_mutex_unlock(&lock);
    uint64_t now = runtime_now_ns();
    unsigned infl = atomic_load_explicit(&g_io_inflight, memory_order_relaxed);
    uint64_t sinc = atomic_load_explicit(&g_io_inflight_since, memory_order_relaxed);
    m40u_tick();   /* M40U: the [io] romfs counter is the ordered hook; the read path is the real heartbeat */
    m40y_tick();   /* M40Y: heartbeat-thread 60 s notfound summary (never on the read path) */
    vk_boot_logf("[io] romfs opens=%llu notfound=%llu reads=%llu bytes=%llu handles=%llu inflight=%u oldest=%.1fms mount=%s",
                 (unsigned long long)atomic_load(&g_io_opens), (unsigned long long)atomic_load(&g_io_notfound),
                 (unsigned long long)atomic_load(&g_io_rd), (unsigned long long)atomic_load(&g_io_bytes),
                 hsum, infl, (infl && sinc) ? (double)(now - sinc) / 1e6 : -1.0, mount_name[0] ? mount_name : "-");
    unsigned top = atomic_load_explicit(&g_io_last_i, memory_order_relaxed);
    unsigned n = top < IO_PATH_DUMP ? top : IO_PATH_DUMP;   /* M7i: legacy 8 lines, byte-identical */
    for (unsigned k = 0; k < n; k++) {   /* newest -> oldest: k=0 is the latest open */
        unsigned i = (top + IO_PATH_RING - 1u - k) % IO_PATH_RING;
        if (!g_io_last[i].path[0]) continue;
        vk_boot_logf("[io]   open#-%u %s (age=%.1fs)", k, g_io_last[i].path, (double)(now - g_io_last[i].ns) / 1e9);
    }
}
/* M5g: the press-time snapshot for the injection probe. Read-only. opens/reads are whole-boot totals,
   inflight is the current depth, and lastpath is a bounded copy of the newest open-ring entry (the ring
   is written without a lock, so a torn copy is possible; it is a diagnostic string, never a decision). */
void runtime_romfs_io_snapshot(unsigned long long *opens, unsigned long long *reads,
                               unsigned *inflight, char *lastpath, unsigned lastcap) {
    if (opens) *opens = (unsigned long long)atomic_load_explicit(&g_io_opens, memory_order_relaxed);
    if (reads) *reads = (unsigned long long)atomic_load_explicit(&g_io_rd, memory_order_relaxed);
    if (inflight) *inflight = atomic_load_explicit(&g_io_inflight, memory_order_relaxed);
    if (lastpath && lastcap) {
        lastpath[0] = 0;
        unsigned top = atomic_load_explicit(&g_io_last_i, memory_order_relaxed);
        if (top) {
            unsigned src = (top - 1u) % IO_PATH_RING, k = 0;
            while (k + 1u < lastcap && g_io_last[src].path[k]) { lastpath[k] = g_io_last[src].path[k]; k++; }
            lastpath[k] = 0;
        }
    }
}
/* M6i: whole-ledger totals, so the per-thread accounting ([io] tot, threads.c) can be checked for
   completeness against the same counters the [io] romfs line prints. Read-only atomics, no lock. */
void runtime_romfs_io_totals(uint64_t *o, uint64_t *r, uint64_t *b, uint64_t *nf) {
    if (o)  *o  = atomic_load_explicit(&g_io_opens, memory_order_relaxed);
    if (r)  *r  = atomic_load_explicit(&g_io_rd, memory_order_relaxed);
    if (b)  *b  = atomic_load_explicit(&g_io_bytes, memory_order_relaxed);
    if (nf) *nf = atomic_load_explicit(&g_io_notfound, memory_order_relaxed);
}
/* M6d: one line per dump with the whole I/O ledger and the delta since the previous call, so
   "did the loader ask for anything in the last window" is a number, not an impression. The newest
   open-ring entry and its age are included (the ring itself is written without a lock; a torn path
   is only ever printed, never acted on - same rule as runtime_romfs_io_snapshot). */
void runtime_romfs_load_line(char *out, unsigned cap) {
    static uint64_t p_opens, p_reads, p_bytes, p_nf;
    uint64_t opens = atomic_load_explicit(&g_io_opens, memory_order_relaxed);
    uint64_t rd = atomic_load_explicit(&g_io_rd, memory_order_relaxed);
    uint64_t by = atomic_load_explicit(&g_io_bytes, memory_order_relaxed);
    uint64_t nf = atomic_load_explicit(&g_io_notfound, memory_order_relaxed);
    unsigned infl = atomic_load_explicit(&g_io_inflight, memory_order_relaxed);
    unsigned long long hsum = 0;
    pthread_mutex_lock(&lock);
    for (unsigned i = 1; i < NH; i++) if (handles[i]) hsum++;
    pthread_mutex_unlock(&lock);
    char last[160]; last[0] = 0; double age = -1.0;
    unsigned top = atomic_load_explicit(&g_io_last_i, memory_order_relaxed);
    if (top) {
        unsigned src = (top - 1u) % IO_PATH_RING, k = 0;
        while (k + 1u < sizeof last && g_io_last[src].path[k]) { last[k] = g_io_last[src].path[k]; k++; }
        last[k] = 0;
        uint64_t ns = g_io_last[src].ns;
        if (ns) age = (double)(runtime_now_ns() - ns) / 1e9;
    }
    snprintf(out, cap,
             "opens=%llu(+%llu) reads=%llu(+%llu) bytes=%llu(+%llu) notfound=%llu(+%llu) handles=%llu inflight=%u last=%s age=%.1fs",
             (unsigned long long)opens, (unsigned long long)(opens - p_opens),
             (unsigned long long)rd, (unsigned long long)(rd - p_reads),
             (unsigned long long)by, (unsigned long long)(by - p_bytes),
             (unsigned long long)nf, (unsigned long long)(nf - p_nf),
             hsum, infl, last[0] ? last : "-", age);
    p_opens = opens; p_reads = rd; p_bytes = by; p_nf = nf;
}
/* Overlays: files listed in a directory's manifest.tsv are served in place of the originals. Each line names a RomFS
   path, the overlay's size and SHA-256 and the original's SHA-256; a missing path, a size or hash mismatch or a changed
   original stops the start. The directories, in this order: the build's patches (scripts/make_romfs_patches.py;
   TOTK_ROMFS_PATCHES, default extracted/<version>/romfs-patches, 0 = none), the true-2K HeapDef overlay
   (scripts/make_output_res_overlay.py) and the player's mods (TOTK_ROMFS_MODS, which the launcher sets to
   <data>/mods/<version>; no manifest there = none). Each file can be replaced once. An overlay file is opened when the
   game opens it, so a mod can replace thousands of files (a dub's voice streams) without holding a descriptor each; the
   file's identity (device, inode, size, modification time) is kept from the start check, and an open that finds another
   file there stops the game instead of serving bytes nobody checked. scripts/make_mod_manifest.py writes a mod's manifest.
   A line whose original SHA-256 is "-" adds a file the RomFS lacks (issue #48: The Second Quest adds 874): it is numbered
   after the RomFS's own files (file_count + k, k in path order, added[]), opens and reads like a replacement, and its
   folders exist for runtime_romfs_entry_type. The RomFS has no directory listing here, so nothing else needs to know. */
typedef struct { uint64_t index,size; char *path; dev_t dev; ino_t ino; struct timespec mtime; char *name; } Overlay;
static Overlay *overlays; /* sorted by RomFS index after overlay_init */
static size_t noverlay,overlay_cap;
#define ADDED UINT64_MAX   /* an added file's index until overlay_init numbers it */
static char **added;       /* RomFS paths of the added files, sorted; added[k] is index file_count + k */
static size_t nadded;
static uint32_t handle_ovl[NH]; /* overlay slot + 1 of an open handle, 0 = original */
static int handle_fd[NH];       /* the handle's open overlay file, when handle_ovl is set */
typedef struct { uint64_t offset,size;uint32_t name,len; } Entry;
_Static_assert(sizeof(Entry)==24,"index entry layout");
static Entry entry(uint64_t i) {Entry e;memcpy(&e,index_bytes+24+i*24,24);return e;}
static const char *entry_name(Entry e) {return (char *)index_bytes+strings_offset+e.name;}
static void need(int c,const char *m) {if(!c)runtime_fail(m);}
static void sha_fd(int fd,const char *expected,uint64_t total,const char *stage) {
 CC_SHA256_CTX c;CC_SHA256_Init(&c);uint8_t *buffer=malloc(8<<20);need(buffer!=NULL,"hash buffer");
 for(off_t off=0;;){ssize_t n=src_pread(fd,buffer,8<<20,off);need(n>=0,"read hash input");if(!n)break;CC_SHA256_Update(&c,buffer,(CC_LONG)n);off+=n;
  if(stage&&((uint64_t)off%(256u<<20)<(8u<<20)||!n))runtime_boot_progress(stage,(uint64_t)off,total);}
 uint8_t d[32];char hex[65];CC_SHA256_Final(d,&c);free(buffer);for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",d[i]);need(!strcmp(hex,expected),"RomFS integrity mismatch");
}
/* A successful full hash may be reused only while the same regular file has
   unchanged filesystem identity and modification metadata. This is a startup
   shortcut, not a fresh integrity proof; TOTK_ROMFS_VERIFY=full forces one. */
typedef struct {
 char magic[16], sha256[65];
 uint64_t dev,ino,size;
 int64_t mtime_sec,mtime_nsec,ctime_sec,ctime_nsec,birth_sec,birth_nsec;
} RomfsProof;
static RomfsProof proof_for(const struct stat *s) {
 RomfsProof p={0};memcpy(p.magic,"TOTKRFS-PROOF-1",16);memcpy(p.sha256,ROMFS_IMAGE_SHA256,65);
 p.dev=(uint64_t)s->st_dev;p.ino=(uint64_t)s->st_ino;p.size=(uint64_t)s->st_size;
 p.mtime_sec=s->st_mtimespec.tv_sec;p.mtime_nsec=s->st_mtimespec.tv_nsec;
 p.ctime_sec=s->st_ctimespec.tv_sec;p.ctime_nsec=s->st_ctimespec.tv_nsec;
 p.birth_sec=s->st_birthtimespec.tv_sec;p.birth_nsec=s->st_birthtimespec.tv_nsec;
 return p;
}
static int proof_equal(const RomfsProof *a,const RomfsProof *b) {
 return !memcmp(a->magic,b->magic,sizeof a->magic)&&!memcmp(a->sha256,b->sha256,sizeof a->sha256)
  &&a->dev==b->dev&&a->ino==b->ino&&a->size==b->size
  &&a->mtime_sec==b->mtime_sec&&a->mtime_nsec==b->mtime_nsec
  &&a->ctime_sec==b->ctime_sec&&a->ctime_nsec==b->ctime_nsec
  &&a->birth_sec==b->birth_sec&&a->birth_nsec==b->birth_nsec;
}
/* Split image: one record per part, all of which must be unchanged for the earlier full hash to be reused. */
static void chunk_proofs(RomfsProof *out) {
 /* ctime is left out: reinstalling the iOS app touches the data container's file metadata (ctime) without changing a
    byte, which made every launch hash 17 GB again. A content change still moves mtime (and size). */
 for(unsigned c=0;c<nchunks;c++){struct stat st;need(fstat(chunk_fds[c],&st)==0&&S_ISREG(st.st_mode),"RomFS part stat");out[c]=proof_for(&st);out[c].ctime_sec=out[c].ctime_nsec=0;}
}
static void verify_chunks(const char *dir) {
 RomfsProof want[MAX_CHUNKS],saved[MAX_CHUNKS],after[MAX_CHUNKS];size_t bytes=nchunks*sizeof(RomfsProof);chunk_proofs(want);
 const char *pdir=getenv("TOTK_ROMFS_PROOF_DIR");if(pdir&&*pdir)dir=pdir;   /* iOS: directories devicectl creates belong to root */
 char path[4096],tmp[4096];need(snprintf(path,sizeof(path),"%s/.romfs-verified-parts-v1",dir)<(int)sizeof(path),"RomFS proof path");
 int force=getenv("TOTK_ROMFS_VERIFY")&&!strcmp(getenv("TOTK_ROMFS_VERIFY"),"full");
 int proof_fd=open(path,O_RDONLY|O_NOFOLLOW);
 if(proof_fd>=0){struct stat ps;int ok=fstat(proof_fd,&ps)==0&&S_ISREG(ps.st_mode)&&(size_t)ps.st_size==bytes&&read(proof_fd,saved,bytes)==(ssize_t)bytes;close(proof_fd);
  if(ok&&!force&&!memcmp(saved,want,bytes)){fprintf(stderr,"[totk] RomFS parts unchanged; reused full SHA256 verification record (%u parts)\n",nchunks);runtime_boot_progress(TOTK_UI("游戏资源校验记录有效","Game data verification record is valid"),ROMFS_IMAGE_SIZE,ROMFS_IMAGE_SIZE);return;}}
 fprintf(stderr,"[totk] verifying local RomFS SHA256 over %u parts (%llu bytes)%s\n",nchunks,(unsigned long long)ROMFS_IMAGE_SIZE,force?" [forced]":"");
 runtime_boot_progress(TOTK_UI("校验游戏资源","Verifying game data"),0,ROMFS_IMAGE_SIZE);sha_fd(image_fd,ROMFS_IMAGE_SHA256,ROMFS_IMAGE_SIZE,TOTK_UI("校验游戏资源","Verifying game data"));
 chunk_proofs(after);need(!memcmp(after,want,bytes),"RomFS changed during verification");
 if(snprintf(tmp,sizeof(tmp),"%s/.romfs-verified-parts-v1.%ld.tmp",dir,(long)getpid())>=(int)sizeof(tmp))return;
 int out=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
 if(out>=0){ssize_t n=write(out,want,bytes);int ok=n==(ssize_t)bytes&&fsync(out)==0;close(out);if(ok){if(rename(tmp,path)!=0)unlink(tmp);}else unlink(tmp);}
}
static void open_chunks(const char *dir) {
 uint64_t total=0;
 for(;nchunks<MAX_CHUNKS;nchunks++){char path[4096];need(snprintf(path,sizeof(path),"%s/romfs.bin.%03u",dir,nchunks)<(int)sizeof(path),"RomFS part path");
  int fd=open(path,O_RDONLY);if(fd<0)break;struct stat st;need(fstat(fd,&st)==0&&st.st_size>0,"RomFS part stat");
  if(!nchunks)chunk_size=(uint64_t)st.st_size;else need(chunk_fds[nchunks-1]>=0&&total==(uint64_t)nchunks*chunk_size,"RomFS parts must have equal sizes except the last");
  chunk_fds[nchunks]=fd;total+=(uint64_t)st.st_size;}
 need(nchunks>0,"RomFS image missing (romfs.bin or romfs.bin.000...)");
 need(total==ROMFS_IMAGE_SIZE,"RomFS parts do not add up to the image size");
 image_fd=chunk_fds[0];
 fprintf(stderr,"[totk] RomFS image in %u parts of %llu bytes\n",nchunks,(unsigned long long)chunk_size);
}
static void verify_image(int fd,const char *dir) {
 if(nchunks){verify_chunks(dir);return;}
 const char *pdir=getenv("TOTK_ROMFS_PROOF_DIR");if(pdir&&*pdir)dir=pdir;
 struct stat before,after;need(fstat(fd,&before)==0&&S_ISREG(before.st_mode),"RomFS image stat");
 RomfsProof want=proof_for(&before),saved;
 char path[4096],tmp[4096];need(snprintf(path,sizeof(path),"%s/.romfs-verified-v1",dir)<(int)sizeof(path),"RomFS proof path");
 int force=getenv("TOTK_ROMFS_VERIFY")&&!strcmp(getenv("TOTK_ROMFS_VERIFY"),"full");
 int proof_fd=open(path,O_RDONLY|O_NOFOLLOW);
 if(proof_fd>=0){struct stat ps;int ok=fstat(proof_fd,&ps)==0&&S_ISREG(ps.st_mode)&&ps.st_size==sizeof(saved)&&read(proof_fd,&saved,sizeof(saved))==sizeof(saved);close(proof_fd);
  if(ok&&!force&&proof_equal(&saved,&want)){fprintf(stderr,"[totk] RomFS unchanged; reused full SHA256 verification record (%llu bytes)\n",(unsigned long long)want.size);runtime_boot_progress(TOTK_UI("游戏资源校验记录有效","Game data verification record is valid"),want.size,want.size);return;}}
 fprintf(stderr,"[totk] verifying local RomFS SHA256 (%llu bytes)%s\n",(unsigned long long)want.size,force?" [forced]":"");
 runtime_boot_progress(TOTK_UI("校验游戏资源","Verifying game data"),0,want.size);sha_fd(fd,ROMFS_IMAGE_SHA256,want.size,TOTK_UI("校验游戏资源","Verifying game data"));
 need(fstat(fd,&after)==0,"RomFS image stat after hash");
 RomfsProof finished=proof_for(&after);need(proof_equal(&want,&finished),"RomFS changed during verification");
 if(snprintf(tmp,sizeof(tmp),"%s/.romfs-verified-v1.%ld.tmp",dir,(long)getpid())>=(int)sizeof(tmp))return;
 int out=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
 if(out>=0){ssize_t n=write(out,&want,sizeof(want));int ok=n==sizeof(want)&&fsync(out)==0;close(out);
  if(ok){if(rename(tmp,path)!=0)unlink(tmp);}else unlink(tmp);}
}
static void sha_range(int fd,uint64_t offset,uint64_t size,const char *expected,const char *what) {
 size_t cap=size<(8u<<20)?(size_t)size:(8u<<20);CC_SHA256_CTX c;CC_SHA256_Init(&c);uint8_t *buffer=malloc(cap?cap:1);need(buffer!=NULL,"hash buffer");
 for(uint64_t done=0;done<size;){size_t chunk=size-done>cap?cap:(size_t)(size-done);ssize_t n=src_pread(fd,buffer,chunk,(off_t)(offset+done));need(n>0,"read hash input");CC_SHA256_Update(&c,buffer,(CC_LONG)n);done+=(uint64_t)n;}
 uint8_t d[32];char hex[65];CC_SHA256_Final(d,&c);free(buffer);for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",d[i]);
 if(strcmp(hex,expected)){fprintf(stderr,"[totk] RomFS overlay: %s SHA-256 %s, manifest says %s\n",what,hex,expected);runtime_fail("RomFS overlay integrity mismatch");}
}
static uint64_t lower(const char *name);
static int path_ok(const char *p);
static void overlay_load(const char *dir,int required) {
 char path[4096];need(snprintf(path,sizeof(path),"%s/manifest.tsv",dir)<(int)sizeof(path),"RomFS overlay path");FILE *m=fopen(path,"r");
 if(!m&&required){fprintf(stderr,"[totk] RomFS overlay: %s missing; build it with scripts/make_output_res_overlay.py <w>x<h>\n",path);runtime_fail("true-2K HeapDef overlay missing (make romfs-patches-out OUTPUT_RES=<w>x<h>)");}
 if(!m){fprintf(stderr,"[totk] RomFS overlay: none (%s)\n",path);return;}
 char line[8192];size_t first=noverlay;uint64_t hashed=0,t0=runtime_now_ns();
 while(fgets(line,sizeof(line),m)){
  if(line[0]=='#'||line[0]=='\n')continue;char name[4096],sha[65],orig[65];unsigned long long size=0;
  need(sscanf(line,"%4095[^\t]\t%llu\t%64[0-9a-f]\t%64[0-9a-f-]",name,&size,sha,orig)==4&&strlen(sha)==64&&(strlen(orig)==64||!strcmp(orig,"-")),"RomFS overlay manifest line");
  need(path_ok(name),"RomFS overlay path");uint64_t i=lower(name);int known=i<file_count&&!strcmp(entry_name(entry(i)),name),add=!strcmp(orig,"-");
  if(!known&&!add){fprintf(stderr,"[totk] RomFS overlay: %s/manifest.tsv names %s, which the RomFS lacks; a new file has \"-\" as its original's SHA-256\n",dir,name);runtime_fail("RomFS overlay names a file the RomFS lacks");}
  if(known&&add){fprintf(stderr,"[totk] RomFS overlay: %s/manifest.tsv adds %s, which the RomFS already has; give its original's SHA-256\n",dir,name);runtime_fail("RomFS overlay adds a file the RomFS already has");}
  Entry e={0};if(known){e=entry(i);sha_range(image_fd,e.offset,e.size,orig,"original");}else i=ADDED;
  char file[8200];need(snprintf(file,sizeof(file),"%s/%s",dir,name)<(int)sizeof(file),"RomFS overlay file path");int fd=open(file,O_RDONLY|O_CLOEXEC);struct stat st;
  need(fd>=0&&fstat(fd,&st)==0&&(uint64_t)st.st_size==size,"RomFS overlay file missing or wrong size");sha_range(fd,0,size,sha,name);close(fd);
  hashed+=e.size+size;
  if(noverlay==overlay_cap){overlay_cap=overlay_cap?overlay_cap*2:16;overlays=realloc(overlays,overlay_cap*sizeof(*overlays));need(overlays!=NULL,"RomFS overlay table");}
  char *copy=strdup(file),*rel=strdup(name);need(copy&&rel,"RomFS overlay path copy");overlays[noverlay++]=(Overlay){i,size,copy,st.st_dev,st.st_ino,st.st_mtimespec,rel};
  if(noverlay-first<=16)fprintf(stderr,"[totk] RomFS overlay: %s (%llu bytes) %s\n",name,size,add?"is added":"replaces the original");
 }
 fclose(m);
 if(noverlay-first>16)fprintf(stderr,"[totk] RomFS overlay: %zu files from %s (%zu more not listed)\n",noverlay-first,dir,noverlay-first-16);
 if(noverlay>first)fprintf(stderr,"[totk] RomFS overlay: %s checked, %zu files, %.1f MB hashed in %.2f s\n",dir,noverlay-first,hashed/1048576.0,(runtime_now_ns()-t0)/1e9);
}
static int overlay_order(const void *a,const void *b) {uint64_t x=((const Overlay *)a)->index,y=((const Overlay *)b)->index;return (x>y)-(x<y);}
static int added_order(const void *a,const void *b) {const Overlay *x=*(Overlay *const *)a,*y=*(Overlay *const *)b;return strcmp(x->name,y->name);}
/* Number the added files in path order after the RomFS's own (file_count + k); two overlays adding one path stop the start. */
static void added_init(void) {
 Overlay **list=NULL;for(size_t k=0;k<noverlay;k++)if(overlays[k].index==ADDED){list=realloc(list,(nadded+1)*sizeof(*list));need(list!=NULL,"RomFS added files");list[nadded++]=&overlays[k];}
 if(!nadded)return;
 qsort(list,nadded,sizeof(*list),added_order);added=malloc(nadded*sizeof(*added));need(added!=NULL,"RomFS added files");
 for(size_t k=0;k<nadded;k++){
  if(k&&!strcmp(list[k]->name,list[k-1]->name)){fprintf(stderr,"[totk] RomFS overlay: %s and %s both add %s\n",list[k-1]->path,list[k]->path,list[k]->name);runtime_fail("RomFS overlay: two overlays add the same file (see the log; remove it from one of the manifests)");}
  list[k]->index=file_count+k;added[k]=list[k]->name;}
 free(list);fprintf(stderr,"[totk] RomFS overlay: %zu files added to the RomFS\n",nadded);
}
/* The index of RomFS path p (an added file: file_count + k), or UINT64_MAX. */
static uint64_t file_index(const char *p) {
 uint64_t i=lower(p);if(i<file_count&&!strcmp(p,entry_name(entry(i))))return i;
 size_t lo=0,hi=nadded;while(lo<hi){size_t m=lo+(hi-lo)/2;if(strcmp(added[m],p)<0)lo=m+1;else hi=m;}
 return lo<nadded&&!strcmp(added[lo],p)?file_count+lo:UINT64_MAX;
}
/* 1 when a RomFS file or an added one lies under folder (path ending in '/'). */
static int folder_exists(const char *folder) {
 size_t n=strlen(folder);uint64_t i=lower(folder);if(i<file_count&&!strncmp(entry_name(entry(i)),folder,n))return 1;
 size_t lo=0,hi=nadded;while(lo<hi){size_t m=lo+(hi-lo)/2;if(strcmp(added[m],folder)<0)lo=m+1;else hi=m;}
 return lo<nadded&&!strncmp(added[lo],folder,n);
}
/* The overlay slot + 1 that replaces RomFS file i, or 0. */
static uint32_t overlay_slot(uint64_t i) {size_t lo=0,hi=noverlay;while(lo<hi){size_t m=lo+(hi-lo)/2;if(overlays[m].index<i)lo=m+1;else hi=m;}return lo<noverlay&&overlays[lo].index==i?(uint32_t)(lo+1):0;}
/* TOTK_ROMFS_PATCHES (default extracted/<version>/romfs-patches; 0 = none); with a true-2K output bigger than 1920x1080
   also <TOTK_ROMFS_PATCHES_OUT, default extracted/<version>/romfs-patches-out>/<w>x<h> (the HeapDef sizes,
   scripts/make_output_res_overlay.py); then the player's mods in TOTK_ROMFS_MODS (0 = none). */
static void overlay_init(void) {
 const char *dir=getenv("TOTK_ROMFS_PATCHES");if(!dir||strcmp(dir,"0"))overlay_load(dir?dir:"extracted/" TOTK_GAME_DIR "/romfs-patches",0);
 unsigned w,h;if(runtime_output_res(&w,&h)&&runtime_output_res_extra_bytes()){const char *out=getenv("TOTK_ROMFS_PATCHES_OUT");char d[4096];
  need(snprintf(d,sizeof(d),"%s/%ux%u",out?out:"extracted/" TOTK_GAME_DIR "/romfs-patches-out",w,h)<(int)sizeof(d),"RomFS output overlay path");overlay_load(d,1);}
 const char *mods=getenv("TOTK_ROMFS_MODS");if(mods&&*mods&&strcmp(mods,"0"))overlay_load(mods,0);
 added_init();
 if(noverlay)qsort(overlays,noverlay,sizeof(*overlays),overlay_order);
 for(size_t k=1;k<noverlay;k++)if(overlays[k].index==overlays[k-1].index){
  fprintf(stderr,"[totk] RomFS overlay: %s and %s replace the same file %s\n",overlays[k-1].path,overlays[k].path,overlays[k].name);
  runtime_fail("RomFS overlay: two overlays replace the same file (see the log; remove it from one of the manifests)");}
}
void runtime_fs_init(void) {
 if(image_fd>=0)return;
 const char *dir=getenv("TOTK_ROMFS");if(!dir)dir="extracted/" TOTK_GAME_DIR "/romfs";char path[4096];
 need(snprintf(path,sizeof(path),"%s/runtime-index.bin",dir)<(int)sizeof(path),"RomFS path");int fd=open(path,O_RDONLY);need(fd>=0,"RomFS index missing");
 struct stat st;need(fstat(fd,&st)==0&&st.st_size>=24&&st.st_size<128*1024*1024,"RomFS index size");runtime_boot_progress(TOTK_UI("读取资源索引","Reading the data index"),0,0);sha_fd(fd,ROMFS_INDEX_SHA256,0,NULL);
 index_size=(size_t)st.st_size;index_bytes=mmap(NULL,index_size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);need(index_bytes!=MAP_FAILED,"map RomFS index");
 need(!memcmp(index_bytes,"TOTKRFS1",8),"RomFS index format");memcpy(&file_count,index_bytes+8,8);memcpy(&strings_offset,index_bytes+16,8);
 need(file_count<1000000&&strings_offset==24+file_count*24&&strings_offset<index_size,"RomFS index layout");
 const char *previous=NULL;
 for(uint64_t i=0;i<file_count;i++){Entry e=entry(i);need(e.offset<=ROMFS_IMAGE_SIZE&&e.size<=ROMFS_IMAGE_SIZE-e.offset,"RomFS file bounds");need(e.name<index_size-strings_offset&&e.len>1&&e.len<=index_size-strings_offset-e.name,"RomFS name bounds");const char *name=entry_name(e);need(name[e.len-1]==0&&strlen(name)==e.len-1&&(!previous||strcmp(previous,name)<0),"RomFS sorted paths");previous=name;}
 need(snprintf(path,sizeof(path),"%s/romfs.bin",dir)<(int)sizeof(path),"RomFS image path");image_fd=open(path,O_RDONLY);
 if(image_fd<0)open_chunks(dir);
 else need(fstat(image_fd,&st)==0&&(uint64_t)st.st_size==ROMFS_IMAGE_SIZE,"RomFS image size");
 verify_image(image_fd,dir);
 if(!nchunks&&getenv("TOTK_ROMFS_MMAP")&&strcmp(getenv("TOTK_ROMFS_MMAP"),"0")){
  void *mapped=mmap(NULL,(size_t)ROMFS_IMAGE_SIZE,PROT_READ,MAP_PRIVATE,image_fd,0);
  if(mapped!=MAP_FAILED){image_map=mapped;fprintf(stderr,"[totk] RomFS mmap read path enabled\n");}
  else fprintf(stderr,"[totk] RomFS mmap failed (%s); using pread\n",strerror(errno));
 }
 runtime_boot_status(TOTK_UI("准备游戏资源","Preparing game data"),TOTK_UI("检查分辨率补丁并挂载只读游戏资源","Checking the resolution patch and mounting the read-only game data"));
 overlay_init();
 fprintf(stderr,"[totk] native read-only RomFS ready: %llu files\n",(unsigned long long)file_count);
}
static int path_ok(const char *p) {return romfs_path_valid_relative(p);}
static const char *rom_path(const char *path) {
 return romfs_path_relative(mount_name,path);
}
static uint64_t lower(const char *name) {uint64_t lo=0,hi=file_count;while(lo<hi){uint64_t m=lo+(hi-lo)/2;if(strcmp(entry_name(entry(m)),name)<0)lo=m+1;else hi=m;}return lo;}
static unsigned mount_rom(const char *name,void *cache,uint64_t size) {
 (void)cache;(void)size;runtime_fs_init();if(!name||!path_ok(name)||strlen(name)>=sizeof(mount_name)||strchr(name,'/')||strchr(name,':'))return INVALID;
 if(*mount_name&&strcmp(name,mount_name))runtime_fail("second RomFS mount not implemented");strcpy(mount_name,name);return 0;
}
static unsigned mount_rom_name(const char *name) {return mount_rom(name,NULL,0);}   /* SDK 13 MountRom without a cache buffer (Bayonetta 3) */
static unsigned cache_size(uint64_t *out) {if(!out)return INVALID;*out=0;return 0;}
/* TOTK_FS_LOG=1: log every RomFS open with the time since start (shows how far the game gets without a picture). */
static void fs_log(const char *what,const char *path){static int on=-1;static uint64_t t0;if(on<0){on=getenv("TOTK_FS_LOG")!=NULL;t0=runtime_now_ns();}if(on)fprintf(stderr,"[totk-fs] %8.3f %s %s\n",(runtime_now_ns()-t0)/1e9,what,path);}
void runtime_fs_log(const char *what,const char *path){fs_log(what,path);}
/* M40Y: one failed RomFS open. Called from the two reject sites only (mount prefix, index lookup). */
static void m40y_fail(const char *raw, const char *norm, const char *why)
{
    if (!m40y_on()) return;
    unsigned i = atomic_fetch_add_explicit(&m40y_nf, 1, memory_order_relaxed) + 1u;
    int seen = -1;
    for (unsigned k = 0; k < m40y_np; k++) if (!strcmp(m40y_p[k], raw ? raw : "(null)")) { seen = (int)k; break; }
    if (seen >= 0) { m40y_n[seen]++; atomic_fetch_add_explicit(&m40y_retry, 1, memory_order_relaxed); }
    else if (m40y_np < M40Y_PATHS) { seen = (int)m40y_np++; snprintf(m40y_p[seen], sizeof m40y_p[seen], "%s", raw ? raw : "(null)"); m40y_n[seen] = 1u; }
    vk_boot_logf("[m40y] NF i=%u t=%.3fs why=%s mount=%s req=%s abs=%s n=%u tid=%lu",
                 i, (double)runtime_now_ns() / 1e9, why, mount_name[0] ? mount_name : "-",
                 raw ? raw : "(null)", norm ? norm : "-", seen >= 0 ? m40y_n[seen] : 1u, (unsigned long)pthread_self());
    if (i == 1u) {   /* the first failure: what the mount looks like now + the caller's recent opens */
        unsigned top = atomic_load_explicit(&g_io_last_i, memory_order_relaxed);
        vk_boot_logf("[m40y] vol mount=%s files=%llu added=%lu image_fd=%d chunks=%u chunkMB=%llu idxMB=%llu dir=%s",
                     mount_name[0] ? mount_name : "-", (unsigned long long)file_count, (unsigned long)nadded, image_fd,
                     nchunks, (unsigned long long)(chunk_size >> 20), (unsigned long long)(index_size >> 20),
                     getenv("TOTK_ROMFS") ? getenv("TOTK_ROMFS") : "extracted/content/romfs");
        for (unsigned k = 0; k < 3u; k++) {
            unsigned j = (top + IO_PATH_RING - 1u - k) % IO_PATH_RING;
            vk_boot_logf("[m40y] ctx #-%u %s age=%.1fs", k, g_io_last[j].path[0] ? g_io_last[j].path : "-",
                         g_io_last[j].ns ? (double)(runtime_now_ns() - g_io_last[j].ns) / 1e9 : -1.0);
        }
    }
    if (seen >= 0 && m40y_n[seen] == 10u)
        vk_boot_logf("[m40y] LOOP path=%s n=%u t=%.3fs", m40y_p[seen], m40y_n[seen], (double)runtime_now_ns() / 1e9);
}
unsigned runtime_romfs_open(uint64_t *out,const char *path,int mode) {
 fs_log("rom",path?path:"(null)");
 atomic_fetch_add_explicit(&g_io_opens,1,memory_order_relaxed); io_note_open(path?path:"(null)");
 if(!out||mode!=1)return UNSUPPORTED;const char *p=rom_path(path);if(!p){m40y_fail(path,NULL,"prefix");atomic_fetch_add_explicit(&g_io_notfound,1,memory_order_relaxed);return NOT_FOUND;}uint64_t i=file_index(p);if(i==UINT64_MAX){m40y_fail(path,p,"index");atomic_fetch_add_explicit(&g_io_notfound,1,memory_order_relaxed);return NOT_FOUND;}
 uint32_t ov=overlay_slot(i);int fd=-1;
 if(ov){const Overlay *o=&overlays[ov-1];fd=open(o->path,O_RDONLY|O_CLOEXEC);struct stat st;
  if(fd<0||fstat(fd,&st)!=0||(uint64_t)st.st_size!=o->size||st.st_dev!=o->dev||st.st_ino!=o->ino||st.st_mtimespec.tv_sec!=o->mtime.tv_sec||st.st_mtimespec.tv_nsec!=o->mtime.tv_nsec){
   fprintf(stderr,"[totk] RomFS overlay: %s is missing or changed since the start check\n",o->path);runtime_fail("RomFS overlay file changed while the game ran (restart the game)");}}
 pthread_mutex_lock(&lock);for(unsigned j=1;j<NH;j++)if(!handles[j]){handles[j]=i+1;handle_ovl[j]=ov;handle_fd[j]=fd;*out=0x100000000ULL+j;pthread_mutex_unlock(&lock);runtime_movie_fs_note(0,*out,0,0,0,path);return 0;}pthread_mutex_unlock(&lock);runtime_fail("file handle table full");return INVALID;
}
static int handle_entry(uint64_t h,Entry *e) {if(h<=0x100000000ULL||h>=0x100000000ULL+NH)return 0;pthread_mutex_lock(&lock);uint64_t i=handles[h-0x100000000ULL];pthread_mutex_unlock(&lock);if(!i)return 0;*e=i-1<file_count?entry(i-1):(Entry){0};return 1;}   /* an added file's bytes are all in its overlay */
/* Where an open handle's bytes come from: the overlay file, or the original's range of the image. */
static void handle_source(uint64_t h,const Entry *e,int *fd,uint64_t *base,uint64_t *size) {
 pthread_mutex_lock(&lock);uint32_t ov=handle_ovl[h-0x100000000ULL];int ofd=handle_fd[h-0x100000000ULL];pthread_mutex_unlock(&lock);
 if(ov){*fd=ofd;*base=0;*size=overlays[ov-1].size;}else{*fd=image_fd;*base=e->offset;*size=e->size;}
}
unsigned runtime_romfs_read(uint64_t *out,uint64_t h,int64_t offset,void *buffer,uint64_t size) {
 Entry e;if(!out||offset<0||!handle_entry(h,&e))return INVALID;int fd;uint64_t base,fsize;handle_source(h,&e,&fd,&base,&fsize);if((uint64_t)offset>fsize||(!buffer&&size))return INVALID;
 uint64_t n=size;if(n>fsize-(uint64_t)offset)n=fsize-(uint64_t)offset;uint64_t done=0;
 static _Atomic int profile_mode=-1;int profile=atomic_load(&profile_mode);if(profile<0){profile=getenv("TOTK_FS_TIMING")!=NULL;atomic_store(&profile_mode,profile);}uint64_t prep_start=profile?runtime_now_ns():0;
 int m40t=m40t_on();   /* M40T: one integer compare when off (see the probe block near the top) */
 uint64_t m40t_prep0=0,m40t_io0=0;if(m40t)m40t_prep0=runtime_now_ns();
 dm_guest_write_prepare(buffer,(size_t)n);
 { extern void nvn_vk_m40j_cp(unsigned,const void *,uint64_t,uint64_t) __attribute__((weak));   /* M40J */
   if(nvn_vk_m40j_cp&&n) nvn_vk_m40j_cp(1u,buffer,(uint64_t)n,base+(uint64_t)offset); }
 if(m40t)m40t_io0=runtime_now_ns();
 uint64_t t0=profile?runtime_now_ns():0;
 /* M5f: inflight only reports "is a read in progress and since when" (read-only; the read path is unchanged). */
 if(atomic_fetch_add_explicit(&g_io_inflight,1,memory_order_relaxed)==0)atomic_store_explicit(&g_io_inflight_since,runtime_now_ns(),memory_order_relaxed);
 if(fd==image_fd&&image_map){memcpy(buffer,image_map+base+(uint64_t)offset,(size_t)n);done=n;}
 else while(done<n){size_t chunk=(size_t)(n-done);if(chunk>8<<20)chunk=8<<20;ssize_t r=src_pread(fd,(uint8_t *)buffer+done,chunk,(off_t)(base+(uint64_t)offset+done));if(r<=0)runtime_fail("RomFS read failure");done+=(uint64_t)r;}
 atomic_fetch_sub_explicit(&g_io_inflight,1,memory_order_relaxed);
 atomic_fetch_add_explicit(&g_io_rd,1,memory_order_relaxed); atomic_fetch_add_explicit(&g_io_bytes,done,memory_order_relaxed);
 runtime_io_note(2, done, NULL);   /* M6i: charge the read (and its bytes) to the calling guest thread */
 runtime_movie_fs_note(1, h, offset, size, done, NULL);   /* M6j: [mv] read line for a MovieData handle only */
 if(m40t){   /* M40T: per-read (path,off,size,us) + duplicate-range detection, see m40t_record */
  uint64_t io1=runtime_now_ns();
  uint32_t us=(uint32_t)((io1-m40t_io0)/1000ull),pus=(uint32_t)((m40t_io0-m40t_prep0)/1000ull);
  m40t_record(e.len?entry_name(e):"?",base+(uint64_t)offset,n,us,pus);
 }
 m40u_tick();   /* M40U: opens-frozen detector + the one-shot directed dump (see the probe block) */
 if(profile){uint64_t now=runtime_now_ns();atomic_fetch_add(&read_calls,1);atomic_fetch_add(&read_bytes,done);atomic_fetch_add(&read_ns,now-t0);atomic_fetch_add(&read_prepare_ns,t0-prep_start);
  uint64_t last=atomic_load(&read_report_ns);
  if(now-last>1000000000ULL&&atomic_compare_exchange_strong(&read_report_ns,&last,now))
   fprintf(stderr,"[totk-fs] read window t=%.3f calls=%llu bytes=%llu wall_ms=%.3f prepare_ms=%.3f\n",now/1e9,
    (unsigned long long)atomic_exchange(&read_calls,0),(unsigned long long)atomic_exchange(&read_bytes,0),atomic_exchange(&read_ns,0)/1e6,atomic_exchange(&read_prepare_ns,0)/1e6);}
 *out=done;return 0;
}
unsigned runtime_romfs_size(int64_t *out,uint64_t h) {Entry e;if(!out||!handle_entry(h,&e))return INVALID;int fd;uint64_t base,size;handle_source(h,&e,&fd,&base,&size);*out=(int64_t)size;return 0;}
void runtime_romfs_close(uint64_t h) {Entry e;if(!handle_entry(h,&e))runtime_fail("close invalid file handle");uint64_t j=h-0x100000000ULL;pthread_mutex_lock(&lock);int fd=handle_ovl[j]?handle_fd[j]:-1;handles[j]=0;handle_ovl[j]=0;handle_fd[j]=-1;pthread_mutex_unlock(&lock);if(fd>=0)close(fd);runtime_movie_fs_note(2,h,0,0,0,NULL);}
unsigned runtime_romfs_entry_type(int *out,const char *path) {
 if(!out)return INVALID;const char *p=rom_path(path);if(!p)return NOT_FOUND;if(file_index(p)!=UINT64_MAX){*out=1;return 0;}
 char dir[4096];if(snprintf(dir,sizeof(dir),"%s/",p)>=(int)sizeof(dir))return INVALID;if(folder_exists(dir)){*out=0;return 0;}return NOT_FOUND;
}
/* Directory listing (Bayonetta 3 lists Contents:/): the immediate children of a folder, from the sorted index and the
   added files. A name with no further '/' is a file; the first segment of a longer one is a subfolder, and the sorted
   order keeps a subfolder's paths together, so each is reported once per source. */
static void list_source(const char *prefix,size_t n,int mode,const char *(*name_at)(uint64_t),uint64_t (*size_at)(uint64_t),uint64_t from,uint64_t to,
                        void (*add)(void *,const char *,int,int64_t),void *ctx,char *last_dir) {
 for(uint64_t i=from;i<to;i++){const char *full=name_at(i);if(strncmp(full,prefix,n))break;const char *rest=full+n,*slash=strchr(rest,'/');
  if(!slash){if(mode&2)add(ctx,rest,0,(int64_t)size_at(i));continue;}
  size_t k=(size_t)(slash-rest);if(k>=768)continue;if(!strncmp(last_dir,rest,k)&&!last_dir[k])continue;
  memcpy(last_dir,rest,k);last_dir[k]=0;if(mode&1)add(ctx,last_dir,1,0);}
}
static const char *list_index_name(uint64_t i){return entry_name(entry(i));}
static uint64_t list_index_size(uint64_t i){return entry(i).size;}
static const char *added_name(uint64_t k){return added[k];}
static uint64_t added_size(uint64_t k){char full[4200];snprintf(full,sizeof full,"%s:/%s",mount_name,added[k]);uint64_t h;int64_t size=0;if(runtime_romfs_open(&h,full,1))return 0;runtime_romfs_size(&size,h);runtime_romfs_close(h);return (uint64_t)size;}
unsigned runtime_romfs_list(const char *path,int mode,void (*add)(void *ctx,const char *name,int is_dir,int64_t size),void *ctx) {
 /* A folder may be named with trailing slashes, and the mount root ("Contents:/") is the empty path; files keep the strict check. */
 char trimmed[4096];if(!path||snprintf(trimmed,sizeof trimmed,"%s",path)>=(int)sizeof trimmed)return INVALID;
 size_t tl=strlen(trimmed),ml=strlen(mount_name);while(tl>ml+2&&trimmed[tl-1]=='/')trimmed[--tl]=0;
 int root=!strncmp(trimmed,mount_name,ml)&&trimmed[ml]==':'&&(!trimmed[ml+1]||(trimmed[ml+1]=='/'&&!trimmed[ml+2]));
 const char *p=root?"":rom_path(trimmed);if(!p)return NOT_FOUND;char prefix[4096];
 if(snprintf(prefix,sizeof prefix,"%s%s",p,*p&&p[strlen(p)-1]!='/'?"/":"")>=(int)sizeof prefix)return INVALID;
 size_t n=strlen(prefix);if(n&&!folder_exists(prefix))return NOT_FOUND;
 char last[768]="";list_source(prefix,n,mode,list_index_name,list_index_size,n?lower(prefix):0,file_count,add,ctx,last);
 size_t lo=0,hi=nadded;while(lo<hi){size_t m=lo+(hi-lo)/2;if(strcmp(added[m],prefix)<0)lo=m+1;else hi=m;}
 last[0]=0;list_source(prefix,n,mode,added_name,added_size,lo,nadded,add,ctx,last);
 return 0;
}
int runtime_romfs_owns(const char *path) {size_t n=strlen(mount_name);return n&&path&&!strncmp(path,mount_name,n)&&path[n]==':';}
int runtime_romfs_handle(uint64_t h) {return h>0x100000000ULL&&h<0x100000000ULL+NH;}
unsigned runtime_romfs_read_options(uint64_t *out,uint64_t h,int64_t off,void *buf,uint64_t size) {return runtime_romfs_read(out,h,off,buf,size);}
void *runtime_fs_override(const char *name) {
 static const struct{const char *name;void *fn;} map[]={
 {"_ZN2nn2fs8MountRomEPKcPvm",mount_rom},{"_ZN2nn2fs8MountRomEPKc",mount_rom_name},{"_ZN2nn2fs22QueryMountRomCacheSizeEPm",cache_size}};
 for(size_t i=0;i<sizeof(map)/sizeof(map[0]);i++)if(!strcmp(name,map[i].name))return map[i].fn;return runtime_fs_host_override(name);
}
void runtime_fs_probe(void) {
 runtime_fs_init();need(mount_rom("rom",NULL,0)==0,"mount RomFS");unsigned count=0;uint8_t buffer[257];uint64_t sum=0;
 /* Audio stream paths include an extra slash after the mount root. Verify
    actual resource lookup and bytes, while still rejecting path traversal. */
 {
  const char *name="Sound/Resource/Stream/Dm_OP_0001.bwav";
  char path[256];uint8_t expected[257],actual[257];uint64_t h,n;
  snprintf(path,sizeof path,"rom:/%s",name);
  need(!runtime_romfs_open(&h,path,1),"open normal audio path");
  need(!runtime_romfs_read(&n,h,0,expected,sizeof expected)&&n==sizeof expected,"read normal audio path");runtime_romfs_close(h);
  for(int slashes=2;slashes<=3;slashes++){
   snprintf(path,sizeof path,slashes==2?"rom://%s":"rom:///%s",name);
   int type=-1;need(!runtime_romfs_entry_type(&type,path)&&type==1,"normalized audio entry type");
   need(!runtime_romfs_open(&h,path,1),"open normalized audio stream path");
   need(!runtime_romfs_read(&n,h,0,actual,sizeof actual)&&n==sizeof actual&&!memcmp(actual,expected,sizeof actual),"normalized audio stream bytes");runtime_romfs_close(h);
  }
  need(runtime_romfs_open(&h,"rom://../Sound/Resource/Stream/Dm_OP_0001.bwav",1)!=0,"reject traversal after repeated root separator");
 }
 for(uint64_t i=0;i<file_count;i+=file_count/64){Entry e=entry(i);char p[4096];need(snprintf(p,sizeof(p),"rom:/%s",entry_name(e))<(int)sizeof(p),"probe path");uint64_t h=0,n=0;int64_t size;need(!runtime_romfs_open(&h,p,1)&&!runtime_romfs_size(&size,h)&&(uint64_t)size==e.size,"probe open/size");need(!runtime_romfs_read(&n,h,0,buffer,sizeof(buffer)),"probe partial read");for(uint64_t j=0;j<n;j++)sum=sum*131+buffer[j];need(!runtime_romfs_read(&n,h,size,buffer,1)&&n==0,"probe EOF");runtime_romfs_close(h);count++;}
 for(size_t k=0;k<noverlay;k++){char p[4096];need(snprintf(p,sizeof(p),"rom:/%s",overlays[k].name)<(int)sizeof(p),"probe overlay path");
  if(overlays[k].index>=file_count){int type=-1;char *slash=strchr(overlays[k].name,'/')?strrchr(p,'/'):NULL;
   if(slash){*slash=0;need(!runtime_romfs_entry_type(&type,p)&&type==0,"probe added file's folder");*slash='/';}
   need(!runtime_romfs_entry_type(&type,p)&&type==1,"probe added file type");}uint64_t oh=0,n=0;int64_t size=0;uint8_t a[64],b[64];
  need(!runtime_romfs_open(&oh,p,1)&&!runtime_romfs_size(&size,oh)&&(uint64_t)size==overlays[k].size,"probe overlay size");uint64_t tail=overlays[k].size<64?overlays[k].size:64;int fd=open(overlays[k].path,O_RDONLY|O_CLOEXEC);
  need(fd>=0&&!runtime_romfs_read(&n,oh,size-(int64_t)tail,a,tail)&&n==tail&&pread(fd,b,tail,(off_t)(size-(int64_t)tail))==(ssize_t)tail&&!memcmp(a,b,tail),"probe overlay bytes");close(fd);runtime_romfs_close(oh);}
 uint64_t h=0;need(runtime_romfs_open(&h,"rom:/../main",1)==NOT_FOUND,"reject traversal");need(runtime_romfs_open(&h,"rom:/absent",2)==UNSUPPORTED,"reject write open");
 printf("{\"status\":\"pass\",\"files\":%llu,\"sampled_files\":%u,\"overlays\":%zu,\"added\":%zu,\"read_digest\":\"%016llx\",\"scope\":\"read-only mount/open/size/read/EOF, verified overlays and added files; no game startup\"}\n",(unsigned long long)file_count,count,noverlay,nadded,(unsigned long long)sum);
}
