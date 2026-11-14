/* movie::Decoder for VP9 video on the host with libvpx.
   TOTK's pre-rendered cutscenes (the .webm files in Event/Movie, 28 of them: VP9 profile 0, 1920x1080, 30 fps, video only) play through
   the SDK movie library in subsdk0, which is Android's MediaCodec over NVIDIA's NvMM. Its video decoder opens Tegra's
   NVDEC through nvdrv (NvMMLiteOpenVideo -> NvRmChannelOpen -> NvOsDrvOpen), which this port does not have, so the SDK
   aborted at the first cutscene (the blood moon at midnight).
   main imports the constructor and calls the 17 virtual methods through the vtable for movie::Decoder, whose slots are
   relocated by symbol name: overriding the names replaces the slots. A VP9 buffer-mode decoder (type 3, mode 1) built by
   the constructor is a host object, +0 the library's vtable and +8 a host Movie where the library keeps its DecoderImpl;
   every other decoder goes to the original code. Behaviour follows DecoderImpl (analysis/movie-decoder-impl-named.c):
   - output format 4 is packed NV12 (Y plane, then interleaved UV; stride = width), written into the client's Buffer;
   - input and output buffers are indices handed out FIFO, as MediaCodec does; input flag 4 / output flag 4 = end of stream;
   - client events {input available, output available, format changed, error} (DecoderEvents, 4 EventType pointers) are
     signalled with nn::os::SignalEvent; the game waits on the first three (main+0x240d570).
   Decoding is synchronous in SendInputBufferForDecode. While the game holds every output slot, inputs wait and are decoded
   when it releases one (ReleaseOutputBufferIndex).
   Pacing: the game shows one decoded frame per game frame (main+0x240266c draws the movie texture every frame and puts a
   new frame into it whenever one is queued), which plays a 30 fps movie at 30 fps only at the original 30 fps. At our
   60-100 fps a cutscene ran 3x too fast. So decoded frames are handed to the game when their presentation time is due,
   counted from the first frame (movie_host_tick, called at every present on the game's thread); in between the movie
   texture keeps the last frame. At most one frame is handed over per game frame (the game takes one per frame, so more
   would pile up in its own queue), and when the game falls behind (a loading hitch: ~15 fps for a few seconds under the
   blood moon) a frame more than 1.5 frame periods late is dropped if the next one is due too: the picture stays in time
   with the cutscene's sound instead of lagging and then running fast to catch up. TOTK_MOVIE_PACE=0 hands every frame
   over as soon as it is decoded.
   Odyssey (2026-10-22): its 44 movies (the .mp4 files in MovieData, 41 with an AAC track: the PlayGuide clips Cappy shows, the world postcards) are H.264
   (Baseline / Main / High, 480x272 to 640x360, 4-byte NAL lengths, most with B-frames). The game asks for decoder
   type 1 ("video/avc"), which aborted in NvMMLiteOpenBlock -> NvRmChannelOpen -> NvOsDrvOpen right after the skipped
   intro. Type 1 in buffer mode runs here on VideoToolbox: SPS / PPS come from the format's csd-0 / csd-1 or from the
   stream, samples may be Annex B or length-prefixed, and decoded pictures wait in `held` until they can leave in
   presentation order (AVC_REORDER pictures behind, all at end of stream). */
#include "runtime.h"
#include "movie_host.h"
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !__has_include(<vpx/vpx_decoder.h>)
/* No libvpx for this target: the SDK's own movie decoder runs (NvMM/NVDEC over nvdrv, which this port
   does not have). M5h adds a read-only ledger here instead of a decoder.

   Why the ledger has to live at the vtable: `main` imports only 16 movie symbols (the Decoder ctor,
   the Extractor ctor, Buffer/MediaData helpers, SetAllocator/SetCoreMask/GetThreadPriority); it then
   drives the decoder through virtual calls, which are not imports and never pass through this file.
   So we wrap the imported ctor, copy the guest vtable (19 slots; layout extracted offline from
   _ZTVN5movie7DecoderE at subsdk0+0x5d02f8) into a per-object host copy and replace the calls we care
   about with counting forwarders.

   Every forwarder calls the original slot with the same arguments and returns its value unchanged.
   The only side effect is a counter (and, for the two Acquire slots, a histogram of the returned
   status plus how often output flags carried end-of-stream bit 4). Nothing here decodes, drops or
   synthesises a frame, so if the game still freezes after this probe the movie path is not being
   altered by it. Counters are dumped by runtime_movie_stat_dump() from the boot heartbeat thread. */
extern void vk_boot_logf(const char *fmt, ...);   /* vk_boot.c: only path into the boot log (stderr is lost) */
#include <sys/uio.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { MVD_SLOTS = 19, MVD_MAXOBJ = 8, MVD_STATUS = 6 };
typedef struct {
    void *obj;
    void *slot[MVD_SLOTS];         /* original guest slot functions (copied from the object vtable) */
    void *vt[MVD_SLOTS];           /* the copy we hand back to the guest */
    int type, format, mode, used;
    void *events;                  /* DecoderEvents* handed to slot 3; NEVER dereferenced (see M5h P4 #3) */
    unsigned long calls[MVD_SLOTS];
    unsigned long ent[MVD_SLOTS], ret[MVD_SLOTS];   /* entered / returned: a park shows ent>ret */
    struct { int v; unsigned long n; } st[MVD_SLOTS][MVD_STATUS];
    unsigned long out_frames, out_eos, reg_events;
    /* M5i synthetic decoder state (only used when smo_movie_synth = 1) */
    int syn_cfg, syn_started, syn_w, syn_h, syn_frames, syn_in, syn_out, syn_done, syn_fmt_sent;
    unsigned long syn_in_ok, syn_out_ok, syn_eos, syn_post_eos;
    unsigned char *syn_in_buf, *syn_out_buf;
    int syn_in_cap, syn_out_bytes;
    void *ev[4];                   /* DecoderEvents pointers captured at RegisterClientEvents time */
    int ev_ok[4];                  /* 1 = the pointer was readable then and passed the shape check */
    unsigned long syn_sig[4];
    unsigned long ev_q[8];         /* first 64 bytes of the struct, captured at registration (diagnostic) */
    int syn_eos_logged;            /* M6j: one-shot "[mv] eos-done" line after the EOS frame was released */
} MvDec;
static MvDec g_dec[MVD_MAXOBJ];
static unsigned g_dec_gen;
static unsigned g_dec_ctor_calls, g_dec_reused;
static int g_fail_fast;             /* M5h escape hatch: forwarders fail immediately (see mv_kick_apply); __atomic_* on a plain int */
static int g_kick;                  /* last smo_movie_kick value acted on */
/* helper imports (main): name -> calls, so the ledger can show which part of the movie API was reached */
static unsigned long g_hl[8];
static void *g_orig_ctor, *g_orig_d1, *g_orig_extractor;
static void *g_hl_orig[8];

static MvDec *mv_find(void *obj) {
    for (int i = 0; i < MVD_MAXOBJ; i++) if (g_dec[i].used && g_dec[i].obj == obj) return &g_dec[i];
    return NULL;
}
/* M6j: one trace line per decoder-slot call, with the calling guest thread and the boot time, so the
   call order/timing and the driver thread are readable in vk_boot.log (M6h/P0: the counters alone
   cannot say who drove the movie or how long it took). Rate limited per slot: the first 24 calls of a
   slot, then every 16th, so a long movie cannot flood the log. Also prints a one-shot line when the
   EOS frame has been released, i.e. the player took the whole synthetic stream. */
static double g_mvlog_t0;
static void mv_log_t(double *t) {
    uint64_t now = runtime_now_ns();
    if (g_mvlog_t0 <= 0) g_mvlog_t0 = (double)now;
    *t = (double)(now - (uint64_t)g_mvlog_t0) / 1e9;
}
static void mv_bump(MvDec *d, int slot, int st) {
    d->calls[slot]++;
    if (d->calls[slot] <= 8 || (d->calls[slot] % 32) == 0) {
        double t; mv_log_t(&t);
        vk_boot_logf("[mv] call sl=%d st=%#x by=thr%#x n=%lu t=%.1fs", slot, (unsigned)st,
                     runtime_thread_current_handle(), d->calls[slot], t);
    }
    if (!d->syn_eos_logged && d->out_eos && d->calls[15] >= d->out_frames) {
        d->syn_eos_logged = 1;
        double t; mv_log_t(&t);
        vk_boot_logf("[mv] eos-done frames=%lu eos=%lu released=%lu acqout=%lu (no Stop yet) by=thr%#x t=%.1fs",
                     d->out_frames, d->out_eos, d->calls[15], d->calls[12],
                     runtime_thread_current_handle(), t);
    }
    for (int i = 0; i < MVD_STATUS; i++)
        if (d->st[slot][i].n && d->st[slot][i].v == st) { d->st[slot][i].n++; return; }
    for (int i = 0; i < MVD_STATUS; i++)
        if (!d->st[slot][i].n) { d->st[slot][i].v = st; d->st[slot][i].n = 1; return; }
    /* more than MVD_STATUS distinct values: fold into the last bucket */
}

/* ---- M5i: synthetic decoder ------------------------------------------------------------
   Switch file smo_movie_synth, 0 = ledger only (default), 1 = answer every decoder call on
   the host (path A), 2 = fail Configure (0x1000) / Start (0x2001) only (path B).
   Why: the player parks between Configure() and Start(), and the M5h ledger could not say
   inside which call, because slots 4/10/13/14/17/18 were not instrumented. Every slot is
   wrapped here with an entered/returned pair, so a park shows as ent>ret, and in mode 1 the
   guest original is never entered for the calls we answer. DecoderEvents is never read: M5h
   crashed dereferencing it (P4 #3), so nothing here signals client events. */
#define MV_SYNTH_A  "/data/storage/el2/base/files/smo_movie_synth"
#define MV_SYNTH_B  "/data/app/el2/100/base/com.vibecc.smo/files/smo_movie_synth"
#define MV_FRAMES_A "/data/storage/el2/base/files/smo_movie_frames"
#define MV_FRAMES_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_movie_frames"
enum { ST_OK2 = 0, ST_ERROR2 = 0x1000, ST_NULL2 = 0x100b, ST_NO_OUTPUT2 = 0x100e,
       ST_NO_INPUT2 = 0x1011, ST_UNSUPPORTED2 = 0x2001, ST_NOT_CONFIGURED2 = 0x2002 };
static int g_synth;                     /* __atomic_* on a plain int: read from foreign threads */
static int g_synth_frames = -1;
static void *g_md_find_i32, *g_md_find_str, *g_md_set_i32, *g_md_set_rect, *g_md_set_str;
static void *g_buf_set_range, *g_buf_set_data;
static int mv_read_int(const char *a, const char *b, int dflt) {
    FILE *f = fopen(a, "r"); if (!f) f = fopen(b, "r"); if (!f) return dflt;
    int v = dflt; if (fscanf(f, "%d", &v) != 1) v = dflt; fclose(f);
    return v;
}
static int mv_synth_get(void) { return __atomic_load_n(&g_synth, __ATOMIC_RELAXED); }
static int mv_synth_nframes(void) {
    int v = __atomic_load_n(&g_synth_frames, __ATOMIC_RELAXED);
    if (v < 0) {
        v = mv_read_int(MV_FRAMES_A, MV_FRAMES_B, 8);
        if (v < 1) v = 8;
        if (v > 600) v = 600;
        __atomic_store_n(&g_synth_frames, v, __ATOMIC_RELAXED);
    }
    return v;
}
static void mv_synth_check(void) {
    /* Default 1: the synthetic decoder is the point of M5i. The shell cannot create new files in
       the app files dir (SELinux: only existing files are writable), so the app seeds the switch
       itself on first use; after that `echo 0 > smo_movie_synth` from hdc takes effect at the next
       decoder construction or dump. */
    int v = mv_read_int(MV_SYNTH_A, MV_SYNTH_B, 1);
    if (v < 0 || v > 2) v = 1;
    FILE *f = fopen(MV_SYNTH_B, "r");
    if (!f) { f = fopen(MV_SYNTH_B, "w"); if (f) { fprintf(f, "%d\n", v); fclose(f); } } else fclose(f);
    int old = __atomic_exchange_n(&g_synth, v, __ATOMIC_RELAXED);
    if (old != v) vk_boot_logf("[mv] synth=%d (was %d) frames=%d", v, old, mv_synth_nframes());
}
static void mv_synth_sym(void) {
    if (g_md_find_i32) return;
    g_md_find_i32   = runtime_symbol("_ZNK5movie9MediaData9FindInt32EPKcPi");
    g_md_find_str   = runtime_symbol("_ZNK5movie9MediaData10FindStringEPKcPS2_");
    g_md_set_i32    = runtime_symbol("_ZN5movie9MediaData8SetInt32EPKci");
    g_md_set_rect   = runtime_symbol("_ZN5movie9MediaData7SetRectEPKciiii");
    g_md_set_str    = runtime_symbol("_ZN5movie9MediaData9SetStringEPKcS2_");
    g_buf_set_range = runtime_symbol("_ZN5movie6Buffer8SetRangeEii");
    g_buf_set_data  = runtime_symbol("_ZN5movie6Buffer18SetDataAndCapacityEPvm");
}
/* Safe read of guest/host memory in our own address space: returns 1 only if all bytes arrived. */
static int mv_peek(void *addr, void *dst, size_t n) {
    struct iovec l = { dst, n }, r = { addr, n };
    return syscall(__NR_process_vm_readv, getpid(), &l, 1ul, &r, 1ul, 0ul) == (long)n;
}
static int mv_ascii8(unsigned long v) {
    int printable = 0;
    for (int i = 0; i < 8; i++) {
        unsigned c = (v >> (8 * i)) & 0xff;
        if (c >= 0x20 && c < 0x7f) printable++;
    }
    return printable >= 6;   /* the M5h crash value was text: never treat such a word as an EventType* */
}
/* DecoderEvents is registered by the player. Its exact layout is unverified and M5h crashed reading it
   long after registration (dangling memory), so the four candidate pointers are copied here, while the
   struct is still fresh, and each is accepted only if it is now readable and is not text. */
static void syn_capture_events(MvDec *d, void *events) {
    memset(d->ev, 0, sizeof d->ev); memset(d->ev_ok, 0, sizeof d->ev_ok); memset(d->ev_q, 0, sizeof d->ev_q);
    if (!events || !mv_peek(events, d->ev_q, sizeof d->ev_q)) return;
    int ok[8], n0 = 0;
    for (int k = 0; k < 8; k++) {
        unsigned long p = d->ev_q[k], probe = 0;
        ok[k] = 0;
        if (!p || p < 0x10000ul || (p & 3) || mv_ascii8(p)) continue;
        if (!mv_peek((void *)p, &probe, 8)) continue;
        if (mv_ascii8(probe)) continue;
        ok[k] = 1; if (k < 4) n0++;
    }
    /* four EventType* at offset 0 when that reads as four real pointers; otherwise try words 4..7
       (a struct with a header word would otherwise send us off by one). */
    int base = (n0 >= 2) ? 0 : 4;
    for (int k = 0; k < 4; k++) if (ok[base + k]) { d->ev[k] = (void *)d->ev_q[base + k]; d->ev_ok[k] = 1; }
    vk_boot_logf("[mv] events captured: %lx %lx %lx %lx %lx %lx %lx %lx -> ok=%d%d%d%d%d%d%d%d base=%d",
                 d->ev_q[0], d->ev_q[1], d->ev_q[2], d->ev_q[3], d->ev_q[4], d->ev_q[5], d->ev_q[6], d->ev_q[7],
                 ok[0], ok[1], ok[2], ok[3], ok[4], ok[5], ok[6], ok[7], base);
}
static void syn_signal(MvDec *d, int which) {
    static void *sig;
    if (which < 0 || which > 3 || !d->ev_ok[which]) return;
    if (!sig) sig = runtime_symbol("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    if (!sig) return;
    ((void (*)(void *))sig)(d->ev[which]);
    d->syn_sig[which]++;
}
static int syn_frame_bytes(const MvDec *d) {
    return d->syn_w * d->syn_h + (d->syn_w / 2) * (d->syn_h / 2) * 2;   /* packed NV12, stride = width */
}
static void syn_black(MvDec *d) {
    int fb = syn_frame_bytes(d);
    if (!d->syn_out_buf || d->syn_out_bytes != fb || fb <= 0) return;
    memset(d->syn_out_buf, 16, (size_t)d->syn_w * d->syn_h);                       /* luma: video-range black */
    memset(d->syn_out_buf + (size_t)d->syn_w * d->syn_h, 128, (size_t)fb - (size_t)d->syn_w * d->syn_h);   /* chroma */
}
static int syn_alloc(MvDec *d) {
    int fb = syn_frame_bytes(d);
    if (fb <= 0) return 0;
    if (!d->syn_in_buf) { d->syn_in_cap = 2 << 20; d->syn_in_buf = malloc((size_t)d->syn_in_cap); }
    if (!d->syn_out_buf || d->syn_out_bytes != fb) {
        unsigned char *p = realloc(d->syn_out_buf, (size_t)fb);
        if (p) { d->syn_out_buf = p; d->syn_out_bytes = fb; }
    }
    if (!d->syn_in_buf || !d->syn_out_buf) { d->syn_out_bytes = 0; return 0; }
    syn_black(d);
    return 1;
}
static void syn_free(MvDec *d) {
    free(d->syn_in_buf); free(d->syn_out_buf);
    d->syn_in_buf = d->syn_out_buf = NULL; d->syn_in_cap = d->syn_out_bytes = 0;
    d->syn_cfg = d->syn_started = d->syn_done = 0;
    d->syn_w = d->syn_h = 0; d->syn_frames = d->syn_in = d->syn_out = d->syn_fmt_sent = 0;
}
static void syn_set_format(MvDec *d, void *md) {
    if (!md) return;
    if (g_md_set_str)  ((void (*)(void *, const char *, const char *))g_md_set_str)(md, "mime", "video/raw");
    if (g_md_set_i32) {
        int (*si)(void *, const char *, int) = (int (*)(void *, const char *, int))g_md_set_i32;
        si(md, "width", d->syn_w);   si(md, "height", d->syn_h);
        si(md, "stride", d->syn_w);  si(md, "slice-height", d->syn_h);
        si(md, "color-format", 21);  /* COLOR_FormatYUV420SemiPlanar */
        si(md, "crop-width", d->syn_w); si(md, "crop-height", d->syn_h);
        si(md, "nv12-y-stride", d->syn_w); si(md, "nv12-uv-stride", d->syn_w);
        si(md, "nv12-y-offset", 0); si(md, "nv12-uv-offset", d->syn_w * d->syn_h);
    }
    if (g_md_set_rect)
        ((int (*)(void *, const char *, int, int, int, int))g_md_set_rect)(md, "crop", 0, 0, d->syn_w - 1, d->syn_h - 1);
}
static int syn_configure(MvDec *d, void *md) {
    mv_synth_sym();
    int w = 0, h = 0; const char *mime = NULL;
    if (md && g_md_find_i32) {
        ((int (*)(void *, const char *, int *))g_md_find_i32)(md, "width", &w);
        ((int (*)(void *, const char *, int *))g_md_find_i32)(md, "height", &h);
    }
    if (md && g_md_find_str) ((int (*)(void *, const char *, const char **))g_md_find_str)(md, "mime", &mime);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { w = 1280; h = 720; }   /* no usable geometry: fixed 720p */
    d->syn_w = w & ~1; d->syn_h = h & ~1;
    d->syn_cfg = 1; d->syn_started = 0; d->syn_done = 0; d->syn_fmt_sent = 0;
    d->syn_in = d->syn_out = 0;
    d->syn_frames = mv_synth_nframes();   /* frames to hand out; the last one carries end of stream */
    d->syn_in_ok = d->syn_out_ok = d->syn_eos = d->syn_post_eos = 0;
    int ok = syn_alloc(d);
    vk_boot_logf("[mv] synth cfg: mime=%s %dx%d in=%dKiB out=%d bytes ok=%d frames=%d",
                 mime ? mime : "(none)", d->syn_w, d->syn_h, d->syn_in_cap >> 10, d->syn_out_bytes, ok, mv_synth_nframes());
    return ok ? ST_OK2 : ST_ERROR2;
}
static int syn_get_output_format(MvDec *d, void *md) {
    if (!md) return ST_NULL2;
    if (!d->syn_cfg) return ST_ERROR2;
    syn_set_format(d, md);
    d->syn_fmt_sent++;
    return ST_OK2;
}
static int syn_acquire_input(MvDec *d, int *idx, int *rem) {
    if (!idx) return ST_NULL2;
    if (!d->syn_started || d->syn_done) { if (rem) *rem = 0; return ST_NO_INPUT2; }
    *idx = (d->syn_in++) & 3;
    if (rem) *rem = 0;
    d->syn_in_ok++;
    return ST_OK2;
}
static int syn_send(MvDec *d, int index, int offset, int size, unsigned flags) {
    (void)index; (void)offset; (void)size; (void)flags;
    if (!d->syn_started) return ST_ERROR2;
    /* Output is handed out by syn_acquire_output on a fixed frame budget, so a player that
       never feeds input still sees the movie "finish" instead of waiting for a frame. */
    return ST_OK2;
}
static int syn_acquire_output(MvDec *d, int *idx, int64_t *tm, unsigned *fl, int *rem) {
    if (!idx || !tm || !fl) return ST_NULL2;
    *idx = -1; *tm = 0; *fl = 0; if (rem) *rem = 0;
    if (!d->syn_started) return ST_NO_OUTPUT2;
    if (d->syn_out < d->syn_frames) {
        int k = d->syn_out++;
        *idx = k & 7;
        *tm = (int64_t)k * 33333;                     /* 30 fps in microseconds */
        *fl = (k + 1 >= d->syn_frames) ? 4u : 0u;     /* last frame carries end of stream */
        if (rem) *rem = d->syn_frames - k - 1;
        if (*fl & 4u) d->syn_eos++;
        d->syn_out_ok++;
        return ST_OK2;
    }
    if (d->syn_out >= d->syn_frames && !d->syn_done) {
        d->syn_done = 1;
        vk_boot_logf("[mv] synth eos: %d frames handed over, %d bytes each", d->syn_frames, d->syn_out_bytes);
    }
    if (d->syn_done) d->syn_post_eos++;
    return ST_NO_OUTPUT2;
}
static int syn_get_input_buffer(MvDec *d, int index, void *buf) {
    if (!buf) return ST_NULL2;
    if (index < 0 || index >= 4 || !d->syn_in_buf) return ST_ERROR2;
    if (g_buf_set_range) ((int (*)(void *, int, int))g_buf_set_range)(buf, 0, d->syn_in_cap);
    if (g_buf_set_data)  ((int (*)(void *, void *, unsigned long))g_buf_set_data)(buf, d->syn_in_buf, (unsigned long)d->syn_in_cap);
    return ST_OK2;
}
static int syn_get_output_buffer(MvDec *d, void *buf) {
    if (!buf) return ST_NULL2;
    if (g_buf_set_range) ((int (*)(void *, int, int))g_buf_set_range)(buf, 0, 0);
    if (!d->syn_out_buf) return ST_ERROR2;
    if (g_buf_set_range) ((int (*)(void *, int, int))g_buf_set_range)(buf, 0, d->syn_out_bytes);
    if (g_buf_set_data)  ((int (*)(void *, void *, unsigned long))g_buf_set_data)(buf, d->syn_out_buf, (unsigned long)d->syn_out_bytes);
    return ST_OK2;
}

typedef int (*mvd_v_fn)(void *);
typedef int (*mvd_md_fn)(void *, void *);
typedef int (*mvd_ii_fn)(void *, int *, int *);
typedef int (*mvd_i_fn)(void *, int);
typedef int (*mvd_ip_fn)(void *, int, void *);
typedef int (*mvd_ipp_fn)(void *, int, void *, void *);
typedef int (*mvd_send_fn)(void *, int, int, int, int64_t, unsigned);
typedef int (*mvd_out_fn)(void *, int *, int64_t *, unsigned *, int *);

static void mv_retire(MvDec *d) { syn_free(d); d->used = 0; d->obj = NULL; }
static int f_slot0(void *o) { MvDec *d = mv_find(o); if (!d) return 0; int st = ((mvd_v_fn)d->slot[0])(o); mv_retire(d); return st; }
static int f_slot1(void *o) { MvDec *d = mv_find(o); if (!d) return 0; int st = ((mvd_v_fn)d->slot[1])(o); mv_retire(d); return st; }
static int f_cfg(void *o, void *md) { MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[2]++;
    int mode = mv_synth_get(), st;
    if (mode == 0 && __atomic_load_n(&g_fail_fast, __ATOMIC_RELAXED)) mode = 2;   /* M5h kick=2 keeps its meaning */
    if (mode == 1) st = syn_configure(d, md);
    else if (mode == 2) st = ST_ERROR2;   /* path B: Configure fails, nothing else touched */
    else st = ((mvd_md_fn)d->slot[2])(o, md);
    d->ret[2]++; mv_bump(d, 2, st); return st; }
static int f_start(void *o) { MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[5]++;
    int mode = mv_synth_get(), st;
    if (mode == 0 && __atomic_load_n(&g_fail_fast, __ATOMIC_RELAXED)) mode = 2;
    if (mode == 1) {
        d->syn_started = d->syn_cfg ? 1 : 0;
        if (d->syn_started) { d->syn_out = 0; d->syn_done = 0; }   /* Start without Configure again: play the same 8 frames */
        st = d->syn_cfg ? ST_OK2 : ST_NOT_CONFIGURED2;
        if (st == ST_OK2) {
            int any = d->ev_ok[0] | d->ev_ok[1] | d->ev_ok[2] | d->ev_ok[3];
            if (d->reg_events && !any) {
                /* The player waits on a client event that we could not capture; returning success here
                   would park it forever exactly as the library does. Fail instead (path B at Start). */
                d->syn_started = 0; st = ST_UNSUPPORTED2;
                vk_boot_logf("[mv] synth start: no usable client event -> 0x2001 (path B)");
            } else if (any) {
                syn_signal(d, 0);   /* EV_INPUT: the feed thread waits for the first input slot */
                syn_signal(d, 2);   /* EV_FORMAT: the player needs the output format before it drops buffers */
                syn_signal(d, 1);   /* EV_OUTPUT: first frame is available on demand */
            }
        }
    }
    else if (mode == 2) st = ST_UNSUPPORTED2;
    else st = ((mvd_v_fn)d->slot[5])(o);
    d->ret[5]++; mv_bump(d, 5, st); return st; }
static int f_stop(void *o) { MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[6]++;
    int st = (mv_synth_get() == 1) ? ST_OK2 : ((mvd_v_fn)d->slot[6])(o);
    if (mv_synth_get() == 1) d->syn_started = 0;
    d->ret[6]++; mv_bump(d, 6, st); return st; }
static int f_reset(void *o) { MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[7]++;
    int st;
    if (mv_synth_get() == 1) { d->syn_cfg = d->syn_started = d->syn_done = 0; d->syn_in = d->syn_out = 0; st = ST_OK2; }
    else st = ((mvd_v_fn)d->slot[7])(o);
    d->ret[7]++; mv_bump(d, 7, st); return st; }
static int f_flush(void *o) { MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[8]++;
    int st;
    if (mv_synth_get() == 1) { d->syn_done = 0; d->syn_in = d->syn_out = 0; st = ST_OK2; }
    else st = ((mvd_v_fn)d->slot[8])(o);
    d->ret[8]++; mv_bump(d, 8, st); return st; }
static int f_reg_events(void *o, void *events) { MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[3]++;
    /* Store only. Never dereference: M5h P4 #3 crashed reading this struct as four EventType*. */
    int st;
    if (mv_synth_get() == 1) st = ST_OK2;
    else st = ((mvd_md_fn)d->slot[3])(o, events);
    d->events = events; d->reg_events++;
    if (mv_synth_get() == 1) syn_capture_events(d, events);
    d->ret[3]++; mv_bump(d, 3, st); return st; }
static int f_get_out_fmt(void *o, void *md) {
    MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[4]++;
    int st = (mv_synth_get() == 1) ? syn_get_output_format(d, md) : ((mvd_md_fn)d->slot[4])(o, md);
    d->ret[4]++; mv_bump(d, 4, st); return st;
}
static int f_acq_in(void *o, int *idx, int *rem) {
    MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[9]++;
    int st;
    if (mv_synth_get() == 1) st = syn_acquire_input(d, idx, rem);
    else st = ((mvd_ii_fn)d->slot[9])(o, idx, rem);
    d->ret[9]++; mv_bump(d, 9, st); return st;
}
static int f_get_in(void *o, int i, void *buf) {
    MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[10]++;
    int st = (mv_synth_get() == 1) ? syn_get_input_buffer(d, i, buf) : ((mvd_ip_fn)d->slot[10])(o, i, buf);
    d->ret[10]++; mv_bump(d, 10, st); return st;
}
static int f_send(void *o, int a, int b, int c, int64_t t, unsigned fl) {
    MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[11]++;
    int st;
    if (mv_synth_get() == 1) { (void)t; st = syn_send(d, a, b, c, fl); if (st == 0) syn_signal(d, 0); }
    else st = ((mvd_send_fn)d->slot[11])(o, a, b, c, t, fl);
    if (mv_synth_get() == 1 && (d->calls[11] < 8 || (d->calls[11] % 32) == 0)) { double tv; mv_log_t(&tv);
        vk_boot_logf("[mv] synth send in=%d len=%d off=%d fl=%#x st=%#x by=thr%#x t=%.1fs",
                     a, b, c, (unsigned)fl, (unsigned)st, runtime_thread_current_handle(), tv); }
    d->ret[11]++; mv_bump(d, 11, st); return st;
}
static int f_acq_out(void *o, int *idx, int64_t *tm, unsigned *fl, int *rem) {
    MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[12]++;
    int st;
    if (mv_synth_get() == 1) st = syn_acquire_output(d, idx, tm, fl, rem);
    else st = ((mvd_out_fn)d->slot[12])(o, idx, tm, fl, rem);
    if (st == 0) { d->out_frames++; if (fl && (*fl & 4)) d->out_eos++; }
    if (mv_synth_get() == 1 && (d->calls[12] < 8 || (d->calls[12] % 32) == 0)) { double tv; mv_log_t(&tv);
        vk_boot_logf("[mv] synth req=%lu frames=%d eos=%d idx=%d fl=%#x tm=%lld st=%#x by=thr%#x t=%.1fs",
                     d->calls[12] + 1, d->syn_frames, (fl && (*fl & 4)) ? 1 : 0,
                     idx ? *idx : -1, fl ? (unsigned)*fl : 0, (long long)(tm ? *tm : 0),
                     (unsigned)st, runtime_thread_current_handle(), tv); }
    d->ret[12]++; mv_bump(d, 12, st); return st;
}
static int f_get_out(void *o, int i, void *buf) {
    MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[13]++;
    int st = (mv_synth_get() == 1) ? syn_get_output_buffer(d, buf) : ((mvd_ip_fn)d->slot[13])(o, i, buf);
    d->ret[13]++; mv_bump(d, 13, st); return st;
}
static int f_out_size(void *o, int *size, int *count) {
    MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[14]++;
    int st;
    if (mv_synth_get() == 1) { if (!size || !count) st = ST_NULL2; else { *count = 8; *size = d->syn_out_bytes; st = ST_OK2; } }
    else st = ((mvd_ii_fn)d->slot[14])(o, size, count);
    d->ret[14]++; mv_bump(d, 14, st); return st;
}
static int f_release(void *o, int i) { MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[15]++;
    int st = (mv_synth_get() == 1) ? ST_OK2 : ((mvd_i_fn)d->slot[15])(o, i);
    if (mv_synth_get() == 1) syn_signal(d, 1);
    d->ret[15]++; mv_bump(d, 15, st); return st; }
static int f_last_error(void *o) { MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[16]++;
    int st = (mv_synth_get() == 1) ? ST_OK2 : ((mvd_v_fn)d->slot[16])(o);
    d->ret[16]++; mv_bump(d, 16, st); return st; }
static int f_get_out_md(void *o, int i, void *buf, void *md) {
    MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[17]++;
    int st;
    if (mv_synth_get() == 1) { st = syn_get_output_buffer(d, buf); if (st == 0 && md) syn_set_format(d, md); }
    else st = ((mvd_ipp_fn)d->slot[17])(o, i, buf, md);
    d->ret[17]++; mv_bump(d, 17, st); return st;
}
static int f_out_buf_format(void *o, int i, void *md) {
    MvDec *d = mv_find(o); if (!d) return 0x1000;
    d->ent[18]++;
    int st;
    if (mv_synth_get() == 1) { (void)i; if (!md) st = ST_NULL2; else { syn_set_format(d, md); st = ST_OK2; } }
    else st = ((mvd_ip_fn)d->slot[18])(o, i, md);
    d->ret[18]++; mv_bump(d, 18, st); return st;
}

static void mv_register(void *obj, int type, int format, int mode) {
    MvDec *d = NULL; int slot = -1;
    for (int i = 0; i < MVD_MAXOBJ; i++) {
        if (g_dec[i].used && g_dec[i].obj == obj) { d = &g_dec[i]; g_dec_reused++; break; }
        if (!g_dec[i].used && slot < 0) slot = i;
    }
    if (!d) {
        if (slot < 0) { slot = (int)(g_dec_gen++ % MVD_MAXOBJ); g_dec_reused++; }
        d = &g_dec[slot];
        memset(d, 0, sizeof *d);
    }
    void **vt = *(void ***)obj;           /* guest vptr the real ctor installed */
    d->obj = obj; d->type = type; d->format = format; d->mode = mode; d->used = 1;
    for (int i = 0; i < MVD_SLOTS; i++) { d->slot[i] = vt[i]; d->vt[i] = vt[i]; }
    d->vt[0] = (void *)f_slot0;  d->vt[1] = (void *)f_slot1;  d->vt[2] = (void *)f_cfg;
    d->vt[3] = (void *)f_reg_events;
    d->vt[4] = (void *)f_get_out_fmt; d->vt[5] = (void *)f_start;
    d->vt[6] = (void *)f_stop;   d->vt[7] = (void *)f_reset;
    d->vt[8] = (void *)f_flush;  d->vt[9] = (void *)f_acq_in; d->vt[10] = (void *)f_get_in;
    d->vt[11] = (void *)f_send;  d->vt[12] = (void *)f_acq_out;
    d->vt[13] = (void *)f_get_out; d->vt[14] = (void *)f_out_size;
    d->vt[15] = (void *)f_release; d->vt[16] = (void *)f_last_error;
    d->vt[17] = (void *)f_get_out_md; d->vt[18] = (void *)f_out_buf_format;
    __atomic_store_n((void **)obj, (void *)d->vt, __ATOMIC_RELEASE);
    vk_boot_logf("[mv] decoder %p created: type=%d format=%d mode=%d | vtable hooked at slot 2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17/18",
                 obj, type, format, mode);
}
static void probe_ctor(void *obj, int type, int format, int mode, int prio) {
    if (!g_orig_ctor) g_orig_ctor = runtime_symbol("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi");
    g_dec_ctor_calls++;
    mv_synth_check();
    if (!g_orig_ctor) { vk_boot_logf("[mv] FATAL ledger: decoder ctor symbol missing"); return; }
    ((void (*)(void *, int, int, int, int))g_orig_ctor)(obj, type, format, mode, prio);
    mv_register(obj, type, format, mode);
}
static void probe_d1(void *obj) {
    if (!g_orig_d1) g_orig_d1 = runtime_symbol("_ZN5movie7DecoderD1Ev");
    if (!g_orig_d1) return;
    ((void (*)(void *))g_orig_d1)(obj);
}
/* Generic helper forwarder: every wrapped helper gets its own thunk, but all of them only count and
   then call the guest original. Declared with three pointer/host argument slots, which covers all of
   them (this, this+int+int, this+ptr+size, this+str+out); a callee that takes fewer simply ignores the
   extra registers, and a callee that returns a value gets it back untouched in x0. */
#define MVD_HELPER(id, sym)                                                        \
    static unsigned long mv_hl_##id(void *a, void *b, void *c) {                   \
        if (!g_hl_orig[id]) g_hl_orig[id] = runtime_symbol(sym);                   \
        g_hl[id]++;                                                                \
        if (!g_hl_orig[id]) return 0;                                              \
        return ((unsigned long (*)(void *, void *, void *))g_hl_orig[id])(a, b, c); \
    }
MVD_HELPER(0, "_ZN5movie6BufferC1Ev")
MVD_HELPER(1, "_ZN5movie6BufferD1Ev")
MVD_HELPER(2, "_ZN5movie6Buffer8SetRangeEii")
MVD_HELPER(3, "_ZN5movie6Buffer18SetDataAndCapacityEPvm")
MVD_HELPER(4, "_ZN5movie9MediaData5ClearEv")
MVD_HELPER(5, "_ZNK5movie9MediaData9FindInt32EPKcPi")
MVD_HELPER(6, "_ZNK5movie9MediaData10FindStringEPKcPS2_")
static unsigned long g_hl_ext, g_hl_coremask, g_hl_alloc, g_hl_prio;
static unsigned long mv_hl_extractor(void *a, void *b, void *c) {
    if (!g_orig_extractor) g_orig_extractor = runtime_symbol("_ZN5movie9ExtractorC1ENS_13ContainerTypeENS_9CacheSizeE");
    g_hl_ext++;
    if (!g_orig_extractor) return 0;
    return ((unsigned long (*)(void *, void *, void *))g_orig_extractor)(a, b, c);
}
static unsigned long mv_hl_coremask(void *a, void *b, void *c) { g_hl_coremask++; void *p = runtime_symbol("_ZN5movie11SetCoreMaskEm"); if (!p) return 0; return ((unsigned long (*)(void *, void *, void *))p)(a, b, c); }
static unsigned long mv_hl_alloc(void *a, void *b, void *c, void *d) {   /* movie::SetAllocator(alloc, free, realloc, user): 4 args, no this */
    g_hl_alloc++;
    void *p = runtime_symbol("_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_");
    if (!p) return 0;
    return ((unsigned long (*)(void *, void *, void *, void *))p)(a, b, c, d);
}
static unsigned long mv_hl_prio(void *a, void *b, void *c) { g_hl_prio++; void *p = runtime_symbol("_ZN5movie17GetThreadPriorityEv"); if (!p) return 0; return ((unsigned long (*)(void *, void *, void *))p)(a, b, c); }

static int mv_seen_name;
void *runtime_movie_override(const char *name) {
    static struct { const char *n; void *f; unsigned id; } const tab[] = {
        { "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", (void *)probe_ctor, 0 },
        { "_ZN5movie7DecoderD1Ev", (void *)probe_d1, 0 },
        { "_ZN5movie9ExtractorC1ENS_13ContainerTypeENS_9CacheSizeE", (void *)mv_hl_extractor, 0 },
        { "_ZN5movie6BufferC1Ev", (void *)mv_hl_0, 0 },
        { "_ZN5movie6BufferD1Ev", (void *)mv_hl_1, 0 },
        { "_ZN5movie6Buffer8SetRangeEii", (void *)mv_hl_2, 0 },
        { "_ZN5movie6Buffer18SetDataAndCapacityEPvm", (void *)mv_hl_3, 0 },
        { "_ZN5movie9MediaData5ClearEv", (void *)mv_hl_4, 0 },
        { "_ZNK5movie9MediaData9FindInt32EPKcPi", (void *)mv_hl_5, 0 },
        { "_ZNK5movie9MediaData10FindStringEPKcPS2_", (void *)mv_hl_6, 0 },
        { "_ZN5movie11SetCoreMaskEm", (void *)mv_hl_coremask, 0 },
        { "_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", (void *)mv_hl_alloc, 0 },
        { "_ZN5movie17GetThreadPriorityEv", (void *)mv_hl_prio, 0 },
    };
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (!strcmp(name, tab[i].n)) {
            vk_boot_logf("[mv] resolved import %s -> ledger wrapper %p", name, tab[i].f);
            mv_seen_name = 1;
            return tab[i].f;
        }
    return NULL;
}

/* M5h escape hatch, default off (smo_movie_kick in the app files dir, seeded 0 by vk_boot.c):
   1 = tell the guest decoder its stream failed: signal the DecoderEvents error event once, so a player
       blocked in nn::os::WaitEvent takes its error path instead of waiting for a frame forever;
   2 = additionally make every following decoder call fail immediately (0x1000) and wake all four
       decoder events, so a player that ignores the error event still leaves the movie.
   Read only from the heartbeat dump path (once per dump), never from a decoder call. */
#define MV_KICK_A "/data/storage/el2/base/files/smo_movie_kick"
#define MV_KICK_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_movie_kick"
static void mv_kick_apply(int v) {
    void *sig = runtime_symbol("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    g_kick = v;
    if (v <= 0) { __atomic_store_n(&g_fail_fast, 0, __ATOMIC_RELAXED); vk_boot_logf("[mv] kick=0: probes only"); return; }
    if (v >= 2) __atomic_store_n(&g_fail_fast, 1, __ATOMIC_RELAXED);
    int fired = 0;
    for (int i = 0; i < MVD_MAXOBJ; i++) {
        MvDec *d = &g_dec[i];
        if (!d->used) continue;
        if (!d->events || !sig) { vk_boot_logf("[mv] kick=%d: dec#%d has no registered events (events=%p sig=%p)", v, i, d->events, sig); continue; }
        void **ev = (void **)d->events;
        int lo = v >= 2 ? 0 : 3, hi = 3;   /* v1: error only; v2: input/output/format/error */
        for (int k = lo; k <= hi; k++) if (ev[k]) { ((void (*)(void *))sig)(ev[k]); fired++; }
        vk_boot_logf("[mv] kick=%d: dec#%d events signalled=%d (fail_fast=%d)", v, i, fired,
                     __atomic_load_n(&g_fail_fast, __ATOMIC_RELAXED));
    }
    if (!fired) vk_boot_logf("[mv] kick=%d: no live decoder to signal", v);
}
static void mv_kick_check(void) {
    FILE *f = fopen(MV_KICK_A, "r"); if (!f) f = fopen(MV_KICK_B, "r");
    if (!f) return;
    int v = 0; if (fscanf(f, "%d", &v) != 1) v = 0;
    fclose(f);
    if (v != g_kick) mv_kick_apply(v);
}

void runtime_movie_stat_dump(void) {
    enum { NB = 400 };
    char b[1024];
    int o = 0, any = 0;
    mv_kick_check();
    mv_synth_check();
    for (int i = 0; i < MVD_MAXOBJ; i++) {
        MvDec *d = &g_dec[i];
        if (!d->used) continue;
        any = 1;
        o = snprintf(b, sizeof b, "[mv] dec#%d obj=%p type=%d fmt=%d mode=%d | cfg=%lu", i, d->obj, d->type, d->format, d->mode, d->calls[2]);
        for (int k = 0; k < MVD_STATUS && o < (int)sizeof b - NB; k++)
            if (d->st[2][k].n) o += snprintf(b + o, sizeof b - (size_t)o, " 0x%x:%lu", d->st[2][k].v, d->st[2][k].n);
        o += snprintf(b + o, sizeof b - (size_t)o, " | start=%lu stop=%lu reset=%lu flush=%lu", d->calls[5], d->calls[6], d->calls[7], d->calls[8]);
        o += snprintf(b + o, sizeof b - (size_t)o, " | acqin=%lu", d->calls[9]);
        o += snprintf(b + o, sizeof b - (size_t)o, " send=%lu", d->calls[11]);
        o += snprintf(b + o, sizeof b - (size_t)o, " acqout=%lu", d->calls[12]);
        for (int k = 0; k < MVD_STATUS && o < (int)sizeof b - NB; k++)
            if (d->st[12][k].n) o += snprintf(b + o, sizeof b - (size_t)o, " 0x%x:%lu", d->st[12][k].v, d->st[12][k].n);
        o += snprintf(b + o, sizeof b - (size_t)o, " | frames=%lu eos=%lu rel=%lu geterr=%lu ev=%lu",
                      d->out_frames, d->out_eos, d->calls[15], d->calls[16], d->reg_events);
        vk_boot_logf("%s", b);
        /* entered/returned per slot: a park inside a slot shows ent > ret (M5i) */
        vk_boot_logf("[mv] bp#%d s2=%lu/%lu s3=%lu/%lu s4=%lu/%lu s5=%lu/%lu s9=%lu/%lu s10=%lu/%lu "
                     "s11=%lu/%lu s12=%lu/%lu s13=%lu/%lu s14=%lu/%lu s15=%lu/%lu s17=%lu/%lu s18=%lu/%lu",
                     i, d->ent[2], d->ret[2], d->ent[3], d->ret[3], d->ent[4], d->ret[4], d->ent[5], d->ret[5],
                     d->ent[9], d->ret[9], d->ent[10], d->ret[10], d->ent[11], d->ret[11], d->ent[12], d->ret[12],
                     d->ent[13], d->ret[13], d->ent[14], d->ret[14], d->ent[15], d->ret[15],
                     d->ent[17], d->ret[17], d->ent[18], d->ret[18]);
        vk_boot_logf("[mv] syn#%d mode=%d cfg=%d %dx%d start=%d done=%d | in=%lu out=%lu/%d eos=%lu post_eos=%lu fmtsent=%d bytes=%d",
                     i, mv_synth_get(), d->syn_cfg, d->syn_w, d->syn_h, d->syn_started, d->syn_done,
                     d->syn_in_ok, d->syn_out_ok, d->syn_frames, d->syn_eos, d->syn_post_eos,
                     d->syn_fmt_sent, d->syn_out_bytes);
        vk_boot_logf("[mv] ev#%d ok=%d%d%d%d sig=%lu/%lu/%lu/%lu | raw=%lx,%lx,%lx,%lx reg=%lu",
                     i, d->ev_ok[0], d->ev_ok[1], d->ev_ok[2], d->ev_ok[3],
                     d->syn_sig[0], d->syn_sig[1], d->syn_sig[2], d->syn_sig[3],
                     d->ev_q[0], d->ev_q[1], d->ev_q[2], d->ev_q[3], d->reg_events);
    }
    if (!any)
        vk_boot_logf("[mv] no movie::Decoder object alive (ctor calls so far=%u, reused=%u)", g_dec_ctor_calls, g_dec_reused);
    vk_boot_logf("[mv] helpers: buf_ctor=%lu buf_dtor=%lu setrange=%lu setdata=%lu md_clear=%lu find32=%lu findstr=%lu "
                 "extractor=%lu coremask=%lu alloc=%lu prio=%lu | imports_wrapped=%d ctor_calls=%u",
                 g_hl[0], g_hl[1], g_hl[2], g_hl[3], g_hl[4], g_hl[5], g_hl[6],
                 g_hl_ext, g_hl_coremask, g_hl_alloc, g_hl_prio, mv_seen_name, g_dec_ctor_calls);
}
void movie_host_tick(void) {}
#else
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <VideoToolbox/VideoToolbox.h>

enum { IN_BUFS = 4, IN_CAP = 2 << 20, OUT_BUFS = 8, MAX_MOVIES = 16 };
enum { TYPE_AVC = 1, TYPE_VP9 = 3, MODE_BUFFER = 1, FORMAT_NV12 = 4, FLAG_EOS = 4 };
enum { AVC_HELD = 24, AVC_REORDER = 4, AVC_PARAM = 512 };
enum { ST_OK = 0, ST_ERROR = 0x1000, ST_NULL = 0x100b, ST_NOT_READY = 0x100c, ST_NO_OUTPUT = 0x100e, ST_NO_INPUT = 0x1011,
       ST_UNSUPPORTED = 0x2001, ST_NOT_CONFIGURED = 0x2002 };
enum { EV_INPUT, EV_OUTPUT, EV_FORMAT, EV_ERROR };

typedef struct { int index; unsigned flags; int64_t time; } Ready;
typedef struct { int index, offset, size; int64_t time; unsigned flags; int decoded; } Pending;   /* decoded: AVC input already sent to VideoToolbox */
typedef struct { CVPixelBufferRef px; int64_t time; } Held;
typedef struct {
    void *guest;                         /* the movie::Decoder object */
    int format, configured, started, w, h, eos_in, format_sent, last_error;
    void **events;                       /* DecoderEvents: EventType* per EV_* */
    vpx_codec_ctx_t vpx; int vpx_live;
    int codec;                           /* TYPE_VP9 or TYPE_AVC */
    VTDecompressionSessionRef vt; CMVideoFormatDescriptionRef vfmt;
    uint8_t sps[AVC_PARAM], pps[AVC_PARAM]; size_t nsps, npps; int params_new;
    uint8_t *sample; size_t nsample, sample_cap;
    Held held[AVC_HELD]; int nheld;
    uint8_t *in[IN_BUFS]; int in_free[IN_BUFS], nin_free;
    Pending pending[IN_BUFS]; int npending;
    uint8_t *out[OUT_BUFS]; int out_bytes[OUT_BUFS], out_held[OUT_BUFS];
    Ready ready[OUT_BUFS]; int nready;
    Ready decoded[OUT_BUFS]; int ndecoded;   /* paced: decoded, not due yet */
    uint64_t t0; int64_t pts0;               /* paced: host time and presentation time of the first frame handed over */
    uint64_t frames, decode_ns, errors, dropped;
    pthread_mutex_t mu;
} Movie;

static MovieGuest G; static int g_ready;
static int g_negative = -1;   /* probe-movie negative controls only: 1 = U and V swapped, 2 = no format-changed event */
static int negative(void) { if (g_negative < 0) { const char *e = getenv("TOTK_PROBE_NEGATIVE"); g_negative = e ? atoi(e) : 0; } return g_negative; }
static uint64_t now_ns(void);
static uint64_t (*g_clock)(void) = now_ns;
void movie_host_set_clock(uint64_t (*clock)(void)) { g_clock = clock ? clock : now_ns; }
static int pace_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_MOVIE_PACE"); v = !(e && !strcmp(e, "0")); } return v; }
static Movie *g_movies[MAX_MOVIES];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *const NAMES[MOVIE_ENTRIES] = {
    "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", "_ZN5movie7DecoderD2Ev",
    "_ZN5movie7DecoderD0Ev", "_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE",
    "_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", "_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE",
    "_ZN5movie7Decoder5StartEv", "_ZN5movie7Decoder4StopEv", "_ZN5movie7Decoder5ResetEv", "_ZN5movie7Decoder5FlushEv",
    "_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", "_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE",
    "_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", "_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_",
    "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", "_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_",
    "_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", "_ZN5movie7Decoder12GetLastErrorEv",
    "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", "_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE"};

#define LOG(...) do { fprintf(stderr, "[movie] " __VA_ARGS__); fputc('\n', stderr); } while (0)
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static void need_symbol(void **slot, const char *name) {
    *slot = runtime_symbol(name);
    if (!*slot) { fprintf(stderr, "[movie] missing guest symbol %s\n", name); runtime_fail("movie decoder: guest symbol missing"); }
}
/* The guest functions are resolved the first time any entry runs (the modules are loaded and relocated by then). */
static void guest_init(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_ready) {
        need_symbol((void **)&G.signal_event, "_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
        need_symbol((void **)&G.buf_set_range, "_ZN5movie6Buffer8SetRangeEii");
        need_symbol((void **)&G.buf_set_data, "_ZN5movie6Buffer18SetDataAndCapacityEPvm");
        need_symbol((void **)&G.buf_base, "_ZNK5movie6Buffer4BaseEv");
        need_symbol((void **)&G.buf_capacity, "_ZNK5movie6Buffer8CapacityEv");
        need_symbol((void **)&G.md_find_int32, "_ZNK5movie9MediaData9FindInt32EPKcPi");
        need_symbol((void **)&G.md_find_string, "_ZNK5movie9MediaData10FindStringEPKcPS2_");
        need_symbol((void **)&G.md_set_int32, "_ZN5movie9MediaData8SetInt32EPKci");
        need_symbol((void **)&G.md_set_rect, "_ZN5movie9MediaData7SetRectEPKciiii");
        need_symbol((void **)&G.md_set_string, "_ZN5movie9MediaData9SetStringEPKcS2_");
        G.md_find_buffer = runtime_symbol("_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE");
        G.buf_data = runtime_symbol("_ZNK5movie6Buffer4DataEv"); G.buf_size = runtime_symbol("_ZNK5movie6Buffer4SizeEv");
        void *vt; need_symbol(&vt, "_ZTVN5movie7DecoderE"); G.vptr = (uint8_t *)vt + 0x10;
        for (int i = 0; i < MOVIE_ENTRIES; i++) need_symbol(&G.orig[i], NAMES[i]);
        g_ready = 1;
    }
    pthread_mutex_unlock(&g_lock);
}
void movie_host_set_guest(const MovieGuest *api) { pthread_mutex_lock(&g_lock); G = *api; g_ready = 1; pthread_mutex_unlock(&g_lock); }

static Movie *find(const void *guest) {
    if (!g_ready) guest_init();
    pthread_mutex_lock(&g_lock); Movie *m = NULL;
    for (int i = 0; i < MAX_MOVIES; i++) if (g_movies[i] && g_movies[i]->guest == guest) m = g_movies[i];
    pthread_mutex_unlock(&g_lock);
    return m;
}

/* Events are signalled after the decoder's lock is released: the woken game thread calls straight back in. */
typedef struct { void *ev[8]; int n; } Signals;
static void note(Movie *m, Signals *s, int which) { if (m->events && m->events[which] && s->n < 8) s->ev[s->n++] = m->events[which]; }
static void fire(const Signals *s) { for (int i = 0; i < s->n; i++) G.signal_event(s->ev[i]); }

static int frame_bytes(const Movie *m) { return m->w * m->h + (m->w / 2) * (m->h / 2) * 2; }

/* I420 (libvpx) -> packed NV12: Y rows, then rows of interleaved U, V. */
static void to_nv12(const vpx_image_t *img, uint8_t *dst, int w, int h) {
    for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * w, img->planes[VPX_PLANE_Y] + (size_t)y * img->stride[VPX_PLANE_Y], (size_t)w);
    uint8_t *uv = dst + (size_t)w * h;
    for (int y = 0; y < h / 2; y++) {
        const uint8_t *u = img->planes[VPX_PLANE_U] + (size_t)y * img->stride[VPX_PLANE_U];
        const uint8_t *v = img->planes[VPX_PLANE_V] + (size_t)y * img->stride[VPX_PLANE_V];
        uint8_t *o = uv + (size_t)y * w; int x = 0;
        if (negative() == 1) { const uint8_t *t = u; u = v; v = t; }
#if defined(__aarch64__)
        for (; x + 16 <= w / 2; x += 16) { uint8x16x2_t p = {{vld1q_u8(u + x), vld1q_u8(v + x)}}; vst2q_u8(o + 2 * x, p); }
#else
        for (; x + 16 <= w / 2; x += 16) { memcpy(o + 2 * x, u + x, 16); memcpy(o + 2 * x + 16, v + x, 16); }
#endif
        for (; x < w / 2; x++) { o[2 * x] = u[x]; o[2 * x + 1] = v[x]; }
    }
}
static int free_out(const Movie *m) { for (int i = 0; i < OUT_BUFS; i++) if (!m->out_held[i]) return i; return -1; }
static void push_ready(Movie *m, Signals *s, int slot, unsigned flags, int64_t time, int bytes) {
    m->out_held[slot] = 1; m->out_bytes[slot] = bytes;
    if (pace_on()) { m->decoded[m->ndecoded++] = (Ready){slot, flags, time}; return; }
    m->ready[m->nready++] = (Ready){slot, flags, time};
    note(m, s, EV_OUTPUT);
}
/* Host time a decoded frame is due (ns); end of stream is due as soon as it is first in line. */
static uint64_t due_at(Movie *m, const Ready *d) {
    int64_t ahead = d->time - m->pts0; if (ahead < 0) ahead = 0;
    return m->t0 + (uint64_t)ahead * 1000;
}
/* One game frame (a present): drop leading frames that are more than 1.5 frame periods late while the next one is due
   too, then hand over at most one due frame (end of stream right after the last frame). */
static void release_due(Movie *m, Signals *s, uint64_t now) {
    while (m->ndecoded > 1 && !(m->decoded[0].flags & FLAG_EOS) && !(m->decoded[1].flags & FLAG_EOS) && m->t0) {
        uint64_t due = due_at(m, &m->decoded[0]), next = due_at(m, &m->decoded[1]);
        if (now < next || now - due <= (next - due) * 3 / 2) break;
        m->out_held[m->decoded[0].index] = 0; m->dropped++;
        memmove(m->decoded, m->decoded + 1, (size_t)--m->ndecoded * sizeof m->decoded[0]);
    }
    if (!m->ndecoded) return;
    Ready d = m->decoded[0];
    if (!(d.flags & FLAG_EOS)) {
        if (!m->t0) { m->t0 = now; m->pts0 = d.time; }
        if (now < due_at(m, &d)) return;
    }
    memmove(m->decoded, m->decoded + 1, (size_t)--m->ndecoded * sizeof m->decoded[0]);
    m->ready[m->nready++] = d; note(m, s, EV_OUTPUT);
}
void movie_host_tick(void) {   /* lock order g_lock -> Movie.mu, as in destroy; events fire after both are released */
    Signals s = {0};
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_MOVIES; i++) {
        Movie *m = g_movies[i]; if (!m) continue;
        pthread_mutex_lock(&m->mu); if (m->started) release_due(m, &s, g_clock()); pthread_mutex_unlock(&m->mu);
    }
    pthread_mutex_unlock(&g_lock);
    fire(&s);
}
/* ---- H.264 on VideoToolbox (Odyssey) ---- */
static void avc_drop_held(Movie *m) { for (int i = 0; i < m->nheld; i++) CVPixelBufferRelease(m->held[i].px); m->nheld = 0; }
static void avc_release(Movie *m) {
    if (m->vt) { VTDecompressionSessionWaitForAsynchronousFrames(m->vt); VTDecompressionSessionInvalidate(m->vt); CFRelease(m->vt); m->vt = NULL; }
    if (m->vfmt) { CFRelease(m->vfmt); m->vfmt = NULL; }
    avc_drop_held(m); free(m->sample); m->sample = NULL; m->nsample = m->sample_cap = 0;
}
static void avc_param(uint8_t *dst, size_t *n, const uint8_t *nal, size_t len, int *changed) {
    if (len > AVC_PARAM || (len == *n && !memcmp(dst, nal, len))) return;
    memcpy(dst, nal, len); *n = len; *changed = 1;
}
/* One NAL unit: parameter sets are kept for the format description, delimiters dropped, the rest appended to the sample
   as a 4-byte big-endian length and the unit (the layout VideoToolbox's AVC format description expects). */
static void avc_nal(Movie *m, const uint8_t *nal, size_t len) {
    if (!len) return;
    int type = nal[0] & 0x1f;
    if (type == 7) { avc_param(m->sps, &m->nsps, nal, len, &m->params_new); return; }
    if (type == 8) { avc_param(m->pps, &m->npps, nal, len, &m->params_new); return; }
    if (type == 9) return;
    if (m->nsample + 4 + len > m->sample_cap) {
        size_t cap = (m->nsample + 4 + len) * 2; uint8_t *s = realloc(m->sample, cap);
        if (!s) runtime_fail("movie H.264 sample buffer"); m->sample = s; m->sample_cap = cap;
    }
    uint8_t *o = m->sample + m->nsample;
    o[0] = (uint8_t)(len >> 24); o[1] = (uint8_t)(len >> 16); o[2] = (uint8_t)(len >> 8); o[3] = (uint8_t)len;
    memcpy(o + 4, nal, len); m->nsample += 4 + len;
}
/* Length-prefixed when 4-byte lengths chain exactly to the end, otherwise Annex B (start codes). */
static void avc_parse(Movie *m, const uint8_t *d, size_t n) {
    size_t off = 0;
    while (off + 4 <= n) {
        size_t len = (size_t)d[off] << 24 | (size_t)d[off + 1] << 16 | (size_t)d[off + 2] << 8 | d[off + 3];
        if (!len || len > n - off - 4 || d[off + 4] & 0x80) break;
        off += 4 + len;
    }
    if (off == n && n) {
        for (off = 0; off < n;) { size_t len = (size_t)d[off] << 24 | (size_t)d[off + 1] << 16 | (size_t)d[off + 2] << 8 | d[off + 3]; avc_nal(m, d + off + 4, len); off += 4 + len; }
        return;
    }
    size_t start = SIZE_MAX;
    for (size_t i = 0; i + 3 <= n; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
        if (start != SIZE_MAX) { size_t end = i; while (end > start && !d[end - 1]) end--; avc_nal(m, d + start, end - start); }
        start = i + 3; i += 2;
    }
    if (start != SIZE_MAX && start < n) avc_nal(m, d + start, n - start);
}
/* csd-0 / csd-1 as an Android extractor gives them (Annex B parameter sets), or an avcC record. */
static void avc_csd(Movie *m, const uint8_t *d, size_t n) {
    if (n > 7 && d[0] == 1) {
        size_t off = 6; int nsps = d[5] & 0x1f;
        for (int k = 0; k < 2; k++) {
            int count = k ? (off < n ? d[off++] : 0) : nsps;
            for (int i = 0; i < count && off + 2 <= n; i++) {
                size_t len = (size_t)d[off] << 8 | d[off + 1]; off += 2;
                if (len > n - off) return;
                avc_nal(m, d + off, len); off += len;
            }
        }
        return;
    }
    avc_parse(m, d, n);
}
static void avc_output(void *refcon, void *frame, OSStatus st, VTDecodeInfoFlags flags, CVImageBufferRef img, CMTime pts, CMTime dur) {
    (void)frame; (void)flags; (void)dur;
    Movie *m = refcon;
    if (st != noErr || !img) { if (++m->errors <= 4) LOG("【红】H.264 picture not decoded (VideoToolbox %d)", (int)st); return; }
    if (m->nheld == AVC_HELD) { if (++m->errors <= 4) LOG("【红】H.264 reorder queue full: picture dropped"); return; }
    m->held[m->nheld++] = (Held){CVPixelBufferRetain(img), pts.value};
}
static int avc_session(Movie *m) {
    if (!m->params_new && m->vt) return 1;
    if (!m->nsps || !m->npps) return 0;
    if (m->vt) { VTDecompressionSessionWaitForAsynchronousFrames(m->vt); VTDecompressionSessionInvalidate(m->vt); CFRelease(m->vt); m->vt = NULL; }
    if (m->vfmt) { CFRelease(m->vfmt); m->vfmt = NULL; }
    const uint8_t *sets[2] = {m->sps, m->pps}; const size_t sizes[2] = {m->nsps, m->npps};
    OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, sets, sizes, 4, &m->vfmt);
    if (st != noErr) { LOG("【红】H.264 parameter sets rejected by CoreMedia (%d)", (int)st); return 0; }
    int fmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef pf = CFNumberCreate(NULL, kCFNumberIntType, &fmt);
    const void *keys[] = {kCVPixelBufferPixelFormatTypeKey}, *vals[] = {pf};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    VTDecompressionOutputCallbackRecord cb = {avc_output, m};
    st = VTDecompressionSessionCreate(kCFAllocatorDefault, m->vfmt, NULL, attrs, &cb, &m->vt);
    CFRelease(attrs); CFRelease(pf);
    if (st != noErr) { LOG("【红】VideoToolbox H.264 session not created (%d)", (int)st); m->vt = NULL; return 0; }
    CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(m->vfmt);
    LOG("VideoToolbox H.264 session %dx%d (configured %dx%d)", dim.width, dim.height, m->w, m->h);
    m->params_new = 0; return 1;
}
/* VideoToolbox NV12 picture -> packed NV12 at the configured size (rows / columns beyond the picture left black). */
static void px_to_nv12(CVPixelBufferRef px, uint8_t *dst, int w, int h) {
    CVPixelBufferLockBaseAddress(px, kCVPixelBufferLock_ReadOnly);
    int pw = (int)CVPixelBufferGetWidthOfPlane(px, 0), ph = (int)CVPixelBufferGetHeightOfPlane(px, 0);
    int cw = pw < w ? pw : w, ch = ph < h ? ph : h;
    if (cw < w || ch < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * (h / 2)); }
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(px, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(px, 1);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(px, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(px, 1);
    for (int r = 0; r < ch; r++) memcpy(dst + (size_t)r * w, y + r * ys, (size_t)cw);
    uint8_t *o = dst + (size_t)w * h;
    for (int r = 0; r < ch / 2; r++) memcpy(o + (size_t)r * w, uv + r * uvs, (size_t)(cw & ~1));
    CVPixelBufferUnlockBaseAddress(px, kCVPixelBufferLock_ReadOnly);
}
/* Hand held pictures over in presentation order while more than `keep` are held and output slots are free. */
static void avc_emit(Movie *m, Signals *s, int keep) {
    while (m->nheld > keep) {
        int slot = free_out(m); if (slot < 0) return;
        int first = 0; for (int i = 1; i < m->nheld; i++) if (m->held[i].time < m->held[first].time) first = i;
        Held h = m->held[first]; m->held[first] = m->held[--m->nheld];
        if (!m->format_sent) { m->format_sent = 1; if (negative() != 2) note(m, s, EV_FORMAT); }
        px_to_nv12(h.px, m->out[slot], m->w, m->h); CVPixelBufferRelease(h.px);
        push_ready(m, s, slot, 0, h.time, frame_bytes(m)); m->frames++;
    }
}
static int avc_decode_one(Movie *m, Signals *s, Pending *p) {
    if (p->size > 0 && !p->decoded) {
        if (m->nheld >= AVC_HELD - 4) return 0;   /* wait until the game takes pictures */
        uint64_t t0 = now_ns();
        m->nsample = 0; avc_parse(m, m->in[p->index] + p->offset, (size_t)p->size);
        if (m->nsample && avc_session(m)) {
            CMBlockBufferRef block = NULL; CMSampleBufferRef sample = NULL;
            CMSampleTimingInfo timing = {kCMTimeInvalid, CMTimeMake(p->time, 1000000), kCMTimeInvalid};
            OSStatus st = CMBlockBufferCreateWithMemoryBlock(NULL, m->sample, m->nsample, kCFAllocatorNull, NULL, 0, m->nsample, 0, &block);
            if (st == noErr) st = CMSampleBufferCreateReady(NULL, block, m->vfmt, 1, 1, &timing, 1, &m->nsample, &sample);
            if (st == noErr) st = VTDecompressionSessionDecodeFrame(m->vt, sample, 0, NULL, NULL);
            if (st != noErr && ++m->errors <= 4) LOG("【红】H.264 decode of a %d-byte sample at %lld us failed (%d)", p->size, (long long)p->time, (int)st);
            if (sample) CFRelease(sample);
            if (block) CFRelease(block);
        } else if (m->nsample && ++m->errors <= 4) LOG("【红】H.264 sample at %lld us before its parameter sets: skipped", (long long)p->time);
        p->decoded = 1; m->decode_ns += now_ns() - t0;
    }
    int eos = p->flags & FLAG_EOS && !m->eos_in;
    if (eos && m->vt) VTDecompressionSessionWaitForAsynchronousFrames(m->vt);
    avc_emit(m, s, eos ? 0 : AVC_REORDER);
    if (eos) {
        int slot = free_out(m);
        if (m->nheld || slot < 0) return 0;   /* the rest leaves as the game releases output slots */
        m->eos_in = 1; push_ready(m, s, slot, FLAG_EOS, p->time, 0);
        LOG("H.264 end of stream after %llu frames (%.2f ms per input, %llu errors)", (unsigned long long)m->frames, m->frames ? m->decode_ns / 1e6 / (double)m->frames : 0.0, (unsigned long long)m->errors);
    }
    m->in_free[m->nin_free++] = p->index; note(m, s, EV_INPUT);
    return 1;
}

/* Decode one input into output slots; returns 0 when it had to wait for a free output slot. */
static int decode_one(Movie *m, Signals *s, Pending *p) {
    if (m->codec == TYPE_AVC) return avc_decode_one(m, s, p);
    int need = p->size > 0 ? 1 : 0;
    if (p->flags & FLAG_EOS && !m->eos_in) need++;
    int freeslots = 0; for (int i = 0; i < OUT_BUFS; i++) freeslots += !m->out_held[i];
    if (freeslots < need) return 0;
    if (p->size > 0) {
        uint64_t t0 = now_ns();
        vpx_codec_err_t e = vpx_codec_decode(&m->vpx, m->in[p->index] + p->offset, (unsigned)p->size, NULL, 0);
        if (e != VPX_CODEC_OK) {
            m->errors++; m->last_error = ST_ERROR; note(m, s, EV_ERROR);
            if (m->errors <= 4) LOG("【红】VP9 decode error on a %d-byte frame at %lld us: %s", p->size, (long long)p->time, vpx_codec_error_detail(&m->vpx) ? vpx_codec_error_detail(&m->vpx) : vpx_codec_err_to_string(e));
        } else {
            vpx_codec_iter_t it = NULL; const vpx_image_t *img;
            while ((img = vpx_codec_get_frame(&m->vpx, &it)) != NULL) {
                if (img->fmt != VPX_IMG_FMT_I420 || (int)img->d_w != m->w || (int)img->d_h != m->h) {
                    LOG("【红】VP9 frame %ux%u format %d differs from the configured %dx%d I420", img->d_w, img->d_h, img->fmt, m->w, m->h);
                    runtime_fail("movie decoder: unexpected VP9 frame layout");
                }
                int slot = free_out(m); if (slot < 0) runtime_fail("movie decoder: output slots counted wrong");
                if (!m->format_sent) { m->format_sent = 1; if (negative() != 2) note(m, s, EV_FORMAT); }
                to_nv12(img, m->out[slot], m->w, m->h);
                push_ready(m, s, slot, 0, p->time, frame_bytes(m));
                m->frames++;
            }
        }
        m->decode_ns += now_ns() - t0;
        if (m->frames && m->frames % 300 == 0) LOG("%llu frames, decode %.2f ms per frame", (unsigned long long)m->frames, m->decode_ns / 1e6 / (double)m->frames);
    }
    if (p->flags & FLAG_EOS && !m->eos_in) {
        m->eos_in = 1; int slot = free_out(m); if (slot < 0) runtime_fail("movie decoder: no slot for end of stream");
        push_ready(m, s, slot, FLAG_EOS, p->time, 0);
        LOG("end of stream after %llu frames (%.2f ms per frame, %llu errors)", (unsigned long long)m->frames, m->frames ? m->decode_ns / 1e6 / (double)m->frames : 0.0, (unsigned long long)m->errors);
    }
    m->in_free[m->nin_free++] = p->index; note(m, s, EV_INPUT);
    return 1;
}
static void drain_pending(Movie *m, Signals *s) {
    while (m->npending && decode_one(m, s, &m->pending[0])) { memmove(m->pending, m->pending + 1, (size_t)--m->npending * sizeof m->pending[0]); }
}
static void release_codec(Movie *m) {
    if (m->vpx_live) { vpx_codec_destroy(&m->vpx); m->vpx_live = 0; }
    avc_release(m);
    for (int i = 0; i < IN_BUFS; i++) { free(m->in[i]); m->in[i] = NULL; }
    for (int i = 0; i < OUT_BUFS; i++) { free(m->out[i]); m->out[i] = NULL; }
    m->configured = m->started = 0; m->nin_free = m->npending = m->nready = m->ndecoded = 0; m->t0 = 0;
}
static void reset_queues(Movie *m) {
    m->nin_free = 0; for (int i = 0; i < IN_BUFS; i++) m->in_free[m->nin_free++] = i;
    memset(m->out_held, 0, sizeof m->out_held); m->npending = m->nready = m->ndecoded = 0; m->eos_in = 0; m->t0 = 0;
}

/* ---- entries ---- */
typedef void (*ctor_fn)(void *, int, int, int, int);
static void h_ctor(void *obj, int type, int format, int mode, int priority) {
    if (!g_ready) guest_init();
    if ((type != TYPE_VP9 && type != TYPE_AVC) || mode != MODE_BUFFER) {
        LOG("decoder type %d mode %d format %d: left to the library (only VP9 / H.264 in buffer mode run on the host)", type, mode, format);
        ((ctor_fn)G.orig[MOVIE_CTOR])(obj, type, format, mode, priority); return;
    }
    Movie *m = calloc(1, sizeof *m); if (!m) runtime_fail("movie decoder allocation");
    m->guest = obj; m->format = format; m->codec = type; pthread_mutex_init(&m->mu, NULL);
    pthread_mutex_lock(&g_lock); int i = 0;
    while (i < MAX_MOVIES && g_movies[i] && g_movies[i]->guest != obj) i++;
    if (i == MAX_MOVIES) runtime_fail("movie decoder table full");
    if (g_movies[i]) { LOG("【红】decoder object %p reused without destruction", obj); release_codec(g_movies[i]); free(g_movies[i]); }
    g_movies[i] = m; pthread_mutex_unlock(&g_lock);
    ((void **)obj)[0] = G.vptr; ((void **)obj)[1] = m;
    if (type == TYPE_AVC) LOG("H.264 decoder %p on the host (VideoToolbox), output format %d", obj, format);
    else LOG("VP9 decoder %p on the host (libvpx %s), output format %d", obj, vpx_codec_version_str(), format);
}
static void destroy(void *obj) {
    pthread_mutex_lock(&g_lock); Movie *m = NULL;
    for (int i = 0; i < MAX_MOVIES; i++) if (g_movies[i] && g_movies[i]->guest == obj) { m = g_movies[i]; g_movies[i] = NULL; }
    if (m) { pthread_mutex_lock(&m->mu); pthread_mutex_unlock(&m->mu); }   /* wait for an operation in progress (movie_host_tick) */
    pthread_mutex_unlock(&g_lock);
    if (!m) return;
    LOG("decoder %p destroyed after %llu frames", obj, (unsigned long long)m->frames);
    release_codec(m); pthread_mutex_destroy(&m->mu); free(m);
    ((void **)obj)[0] = G.vptr; ((void **)obj)[1] = NULL;   /* what the library's destructor leaves (no DecoderImpl) */
}
typedef void (*obj_fn)(void *);
static void h_d2(void *obj) { if (!find(obj)) { ((obj_fn)G.orig[MOVIE_D2])(obj); return; } destroy(obj); }
/* Deleting destructor: free the host state, then the library's own deleting destructor frees the object (impl now 0). */
static void h_d0(void *obj) { if (find(obj)) destroy(obj); ((obj_fn)G.orig[MOVIE_D0])(obj); }

typedef int (*md_fn)(void *, void *);
static int h_configure(void *obj, void *format) {
    Movie *m = find(obj); if (!m) return ((md_fn)G.orig[MOVIE_CONFIGURE])(obj, format);
    if (!format) return ST_NULL;
    if (m->format != FORMAT_NV12) { LOG("【红】output format %d not implemented on the host (NV12 = 4 only)", m->format); return ST_UNSUPPORTED; }
    const char *mime = NULL; int w = 0, h = 0;
    G.md_find_string(format, "mime", &mime); G.md_find_int32(format, "width", &w); G.md_find_int32(format, "height", &h);
    const char *want = m->codec == TYPE_AVC ? "video/avc" : "video/x-vnd.on2.vp9";
    if (!mime || strcmp(mime, want) || w <= 0 || h <= 0 || w > 8192 || h > 8192 || (w | h) & 1) {
        LOG("【红】configure: mime %s %dx%d not supported", mime ? mime : "(none)", w, h); return ST_UNSUPPORTED;
    }
    pthread_mutex_lock(&m->mu);
    release_codec(m); m->w = w; m->h = h;
    if (m->codec == TYPE_AVC) {
        m->nsps = m->npps = 0; m->params_new = 0;
        /* The library's MP4 reader keeps the avcC record under "avcc" (the NVIDIA decoder derived csd-0 / csd-1 from it);
           an Android-style extractor gives csd-0 / csd-1. Take whichever is there. */
        static const char *const keys[] = {"csd-0", "csd-1", "avcc", "avcC", "csd"};
        for (int k = 0; k < 5 && G.md_find_buffer && G.buf_data && G.buf_size; k++) {
            void *b = NULL;
            if (!G.md_find_buffer(format, keys[k], &b) || !b) continue;
            const uint8_t *d = G.buf_data(b); size_t n = G.buf_size(b);
            LOG("H.264 format buffer \"%s\": %zu bytes%s", keys[k], n, d && n ? (d[0] == 1 ? " (avcC record)" : "") : " (empty)");
            if (d && n && n < 4096) avc_csd(m, d, n);
        }
        m->nsample = 0;   /* csd carries parameter sets only */
        LOG("H.264 csd: SPS %zu bytes, PPS %zu bytes%s", m->nsps, m->npps, m->nsps && m->npps ? "" : " (waiting for in-band parameter sets)");
    } else {
        vpx_codec_dec_cfg_t cfg = {.threads = 4, .w = (unsigned)w, .h = (unsigned)h};
        if (vpx_codec_dec_init(&m->vpx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) runtime_fail("libvpx VP9 decoder initialisation");
        m->vpx_live = 1;
    }
    for (int i = 0; i < IN_BUFS; i++) if (!(m->in[i] = malloc(IN_CAP))) runtime_fail("movie input buffers");
    for (int i = 0; i < OUT_BUFS; i++) if (!(m->out[i] = malloc((size_t)frame_bytes(m)))) runtime_fail("movie output buffers");
    reset_queues(m); m->format_sent = 0; m->configured = 1; m->last_error = 0;
    pthread_mutex_unlock(&m->mu);
    LOG("configured %dx%d %s, %d input buffers of %d KiB, %d output frames of %d bytes", w, h, m->codec == TYPE_AVC ? "H.264" : "VP9", IN_BUFS, IN_CAP >> 10, OUT_BUFS, frame_bytes(m));
    return ST_OK;
}
static int h_register_events(void *obj, void *events) {
    Movie *m = find(obj); if (!m) return ((md_fn)G.orig[MOVIE_REGISTER_EVENTS])(obj, events);
    m->events = events; return ST_OK;
}
static void set_format(Movie *m, void *md) {
    G.md_set_string(md, "mime", "video/raw");
    G.md_set_int32(md, "width", m->w); G.md_set_int32(md, "height", m->h);
    G.md_set_int32(md, "stride", m->w); G.md_set_int32(md, "slice-height", m->h);
    G.md_set_int32(md, "color-format", 21);   /* COLOR_FormatYUV420SemiPlanar */
    G.md_set_rect(md, "crop", 0, 0, m->w - 1, m->h - 1);
    G.md_set_int32(md, "crop-width", m->w); G.md_set_int32(md, "crop-height", m->h);
    G.md_set_int32(md, "nv12-y-stride", m->w); G.md_set_int32(md, "nv12-uv-stride", m->w);
    G.md_set_int32(md, "nv12-y-offset", 0); G.md_set_int32(md, "nv12-uv-offset", m->w * m->h);
}
static int h_get_output_format(void *obj, void *md) {
    Movie *m = find(obj); if (!m) return ((md_fn)G.orig[MOVIE_GET_OUTPUT_FORMAT])(obj, md);
    if (!md) return ST_NULL;
    if (!m->configured) return ST_ERROR;
    set_format(m, md); return ST_OK;
}
typedef int (*int_fn)(void *);
static int h_start(void *obj) {
    Movie *m = find(obj); if (!m) return ((int_fn)G.orig[MOVIE_START])(obj);
    Signals s = {0};
    pthread_mutex_lock(&m->mu);
    if (!m->configured) { pthread_mutex_unlock(&m->mu); return ST_NOT_CONFIGURED; }
    m->started = 1; note(m, &s, EV_INPUT);
    pthread_mutex_unlock(&m->mu); fire(&s);
    return ST_OK;
}
static int h_stop(void *obj) {
    Movie *m = find(obj); if (!m) return ((int_fn)G.orig[MOVIE_STOP])(obj);
    pthread_mutex_lock(&m->mu); LOG("stop after %llu frames (%llu dropped to keep time)", (unsigned long long)m->frames, (unsigned long long)m->dropped); release_codec(m); pthread_mutex_unlock(&m->mu);
    return ST_OK;
}
static int h_reset(void *obj) {
    Movie *m = find(obj); if (!m) return ((int_fn)G.orig[MOVIE_RESET])(obj);
    pthread_mutex_lock(&m->mu); release_codec(m); m->last_error = 0; pthread_mutex_unlock(&m->mu);
    return ST_OK;
}
static int h_flush(void *obj) {
    Movie *m = find(obj); if (!m) return ((int_fn)G.orig[MOVIE_FLUSH])(obj);
    pthread_mutex_lock(&m->mu);
    if (m->configured && m->codec == TYPE_AVC) {   /* parameter sets and session stay; held pictures belong to the old position */
        if (m->vt) VTDecompressionSessionWaitForAsynchronousFrames(m->vt);
        avc_drop_held(m); reset_queues(m);
    } else if (m->configured) {   /* drop queued work; the next input starts a new decode (after a seek: a key frame) */
        vpx_codec_destroy(&m->vpx); m->vpx_live = 0;
        vpx_codec_dec_cfg_t cfg = {.threads = 4, .w = (unsigned)m->w, .h = (unsigned)m->h};
        if (vpx_codec_dec_init(&m->vpx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) runtime_fail("libvpx VP9 decoder re-initialisation");
        m->vpx_live = 1; reset_queues(m);
    }
    pthread_mutex_unlock(&m->mu);
    return ST_OK;
}
typedef int (*pp_fn)(void *, int *, int *);
static int h_acquire_input(void *obj, int *index, int *remaining) {
    Movie *m = find(obj); if (!m) return ((pp_fn)G.orig[MOVIE_ACQUIRE_INPUT])(obj, index, remaining);
    if (!index || !remaining) return ST_NULL;
    pthread_mutex_lock(&m->mu);
    *index = -1; *remaining = 0; int st = ST_NO_INPUT;
    if (m->started && m->nin_free > 0) {
        *index = m->in_free[0]; memmove(m->in_free, m->in_free + 1, (size_t)--m->nin_free * sizeof m->in_free[0]);
        *remaining = m->nin_free; st = ST_OK;
    }
    pthread_mutex_unlock(&m->mu); return st;
}
typedef int (*ib_fn)(void *, int, void *);
static int h_get_input(void *obj, int index, void *buf) {
    Movie *m = find(obj); if (!m) return ((ib_fn)G.orig[MOVIE_GET_INPUT])(obj, index, buf);
    if (!buf) return ST_NULL;
    if (index < 0 || index >= IN_BUFS || !m->in[index]) { G.buf_set_range(buf, 0, 0); G.buf_set_data(buf, NULL, 0); return ST_ERROR; }
    G.buf_set_range(buf, 0, IN_CAP); G.buf_set_data(buf, m->in[index], IN_CAP);
    return ST_OK;
}
typedef int (*send_fn)(void *, int, int, int, int64_t, unsigned);
static int h_send_input(void *obj, int index, int offset, int size, int64_t time, unsigned flags) {
    Movie *m = find(obj); if (!m) return ((send_fn)G.orig[MOVIE_SEND_INPUT])(obj, index, offset, size, time, flags);
    if (index < 0 || index >= IN_BUFS || offset < 0 || size < 0 || (int64_t)offset + size > IN_CAP) return ST_ERROR;
    Signals s = {0};
    pthread_mutex_lock(&m->mu);
    if (!m->started) { pthread_mutex_unlock(&m->mu); return ST_ERROR; }
    Pending p = {index, offset, size, time, flags, 0};
    if (m->eos_in) { m->in_free[m->nin_free++] = index; note(m, &s, EV_INPUT); }   /* after end of stream: hand the buffer back */
    else if (m->npending || !decode_one(m, &s, &p)) m->pending[m->npending++] = p;
    pthread_mutex_unlock(&m->mu); fire(&s);
    return ST_OK;
}
typedef int (*acq_fn)(void *, int *, int64_t *, unsigned *, int *);
static int h_acquire_output(void *obj, int *index, int64_t *time, unsigned *flags, int *remaining) {
    Movie *m = find(obj); if (!m) return ((acq_fn)G.orig[MOVIE_ACQUIRE_OUTPUT])(obj, index, time, flags, remaining);
    if (!index || !time || !flags || !remaining) return ST_NULL;
    pthread_mutex_lock(&m->mu);   /* paced frames become ready only at presents (movie_host_tick): one per game frame */
    *index = -1; *time = 0; *flags = 0; *remaining = 0; int st = ST_NO_OUTPUT;
    if (m->nready > 0) {
        Ready r = m->ready[0]; memmove(m->ready, m->ready + 1, (size_t)--m->nready * sizeof m->ready[0]);
        *index = r.index; *time = r.time; *flags = r.flags; *remaining = m->nready; st = ST_OK;
    }
    pthread_mutex_unlock(&m->mu);
    return st;
}
static int h_get_output(void *obj, int index, void *buf) {
    Movie *m = find(obj); if (!m) return ((ib_fn)G.orig[MOVIE_GET_OUTPUT])(obj, index, buf);
    if (!buf) return ST_NULL;
    G.buf_set_range(buf, 0, 0);
    pthread_mutex_lock(&m->mu);
    int st = ST_ERROR;
    if (index >= 0 && index < OUT_BUFS && m->out_held[index]) {
        int bytes = m->out_bytes[index]; uint8_t *dst = G.buf_base(buf);
        if (!bytes) st = ST_OK;
        else if (dst && G.buf_capacity(buf) >= (size_t)bytes) { memcpy(dst, m->out[index], (size_t)bytes); G.buf_set_range(buf, 0, bytes); st = ST_OK; }
        else LOG("【红】output buffer of %zu bytes for a %d-byte frame", G.buf_capacity(buf), bytes);
    }
    pthread_mutex_unlock(&m->mu); return st;
}
static int h_get_output_size(void *obj, int *size, int *count) {
    Movie *m = find(obj); if (!m) return ((pp_fn)G.orig[MOVIE_GET_OUTPUT_SIZE])(obj, size, count);
    if (!size || !count) return ST_NULL;
    *count = 1; *size = m->configured ? frame_bytes(m) : 0;
    return ST_OK;
}
typedef int (*i_fn)(void *, int);
static int h_release_output(void *obj, int index) {
    Movie *m = find(obj); if (!m) return ((i_fn)G.orig[MOVIE_RELEASE_OUTPUT])(obj, index);
    Signals s = {0};
    pthread_mutex_lock(&m->mu);
    int st = ST_ERROR;
    if (index >= 0 && index < OUT_BUFS && m->out_held[index]) {
        int queued = 0; for (int i = 0; i < m->nready; i++) queued |= m->ready[i].index == index;
        for (int i = 0; i < m->ndecoded; i++) queued |= m->decoded[i].index == index;
        if (!queued) { m->out_held[index] = 0; if (m->codec == TYPE_AVC && !m->npending) avc_emit(m, &s, AVC_REORDER); drain_pending(m, &s); st = ST_OK; }
    }
    pthread_mutex_unlock(&m->mu); fire(&s);
    return st;
}
static int h_get_last_error(void *obj) {
    Movie *m = find(obj); if (!m) return ((int_fn)G.orig[MOVIE_GET_LAST_ERROR])(obj);
    return m->last_error;
}
typedef int (*ibm_fn)(void *, int, void *, void *);
static int h_get_output_buffer_format(void *obj, int index, void *md) {
    Movie *m = find(obj); if (!m) return ((ib_fn)G.orig[MOVIE_GET_OUTPUT_BUFFER_FORMAT])(obj, index, md);
    if (!md) return ST_NULL;
    if (index < 0 || index >= OUT_BUFS || !m->out_held[index]) return ST_ERROR;
    set_format(m, md); return ST_OK;
}
static int h_get_output_md(void *obj, int index, void *buf, void *md) {
    Movie *m = find(obj); if (!m) return ((ibm_fn)G.orig[MOVIE_GET_OUTPUT_MD])(obj, index, buf, md);
    if (!buf && !md) return ST_NULL;
    int st = md ? h_get_output_buffer_format(obj, index, md) : ST_OK;
    return st == ST_OK && buf ? h_get_output(obj, index, buf) : st;
}

static void *const ENTRIES[MOVIE_ENTRIES] = {
    h_ctor, h_d2, h_d0, h_configure, h_register_events, h_get_output_format, h_start, h_stop, h_reset, h_flush,
    h_acquire_input, h_get_input, h_send_input, h_acquire_output, h_get_output, h_get_output_size, h_release_output,
    h_get_last_error, h_get_output_md, h_get_output_buffer_format};
void *movie_host_entry(int which) { return which >= 0 && which < MOVIE_ENTRIES ? ENTRIES[which] : NULL; }

void *runtime_movie_override(const char *name) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("TOTK_MOVIE_HOST"); off = e && !strcmp(e, "0"); }
    if (off || strncmp(name, "_ZN5movie7Decoder", 17)) return NULL;
    for (int i = 0; i < MOVIE_ENTRIES; i++) if (!strcmp(name, NAMES[i])) return ENTRIES[i];
    /* aliases: the base-object constructor and the complete-object destructor share code with the entries above */
    if (!strcmp(name, "_ZN5movie7DecoderC2ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi")) return h_ctor;
    if (!strcmp(name, "_ZN5movie7DecoderD1Ev")) return h_d2;
    return NULL;
}
#endif   /* libvpx */
