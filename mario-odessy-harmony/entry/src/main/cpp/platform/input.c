/* Native keyboard and macOS gamepad backed full-key controller. State ABI follows libnx HID. */
#include "runtime.h"
#include "autoload.h"
#include "world_survey.h"
#include "vk_boot.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
typedef struct{int64_t sampling;uint64_t buttons;int32_t lx,ly,rx,ry;uint32_t attributes,pad;} NpadState;
_Static_assert(sizeof(NpadState)==40,"Npad full-key ABI");
static pthread_mutex_t inputLock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char keys[128];static uint64_t pending;
static uint32_t styles=1;static bool enabled,allowed=true;static int holdType;
static int64_t sample;static uint64_t padSeen;
/* F/T/H/G map the D-pad left/up/right/down, keeping both analog-stick mappings independent. */
static uint64_t button(unsigned code){switch(code){case 38:return 1;case 40:case 49:return 2;case 34:return 4;case 32:return 8;case 6:return 16;case 8:return 32;case 12:return 64;case 14:return 128;case 58:return 256;case 56:return 512;case 36:return 1024;case 48:case 53:return 2048;case 3:return 4096;case 17:return 8192;case 4:return 16384;case 5:return 32768;default:return 0;}}
/* Real user input (a window key, a controller button, a stick pushed past half) stops TOTK_AUTOLOAD and is logged at most
   once a minute. In an agent's test session (TOTK_SLOT_OWNER other than user) the line says the user is playing it: its
   save is a copy, its scripted measurement is void, and its script must not close the game under the user
   (2026-10-15: the user played 12 minutes in a test window and the script closed it at its time limit). */
static void user_input(const char *what){
 runtime_survey_cancel_user();
 runtime_autoload_user_input();
 static _Atomic uint64_t last;uint64_t now=runtime_now_ns(),prev=atomic_load(&last);
 if(prev&&now-prev<60000000000ull)return;if(!atomic_compare_exchange_strong(&last,&prev,now))return;
 const char *owner=getenv("TOTK_SLOT_OWNER");bool agent=owner&&*owner&&strcmp(owner,"user");
 fprintf(stderr,"[input] user input (%s)%s%s\n",what,agent?": the user is playing this test session of ":"",agent?owner:"");
}
/* Keys from the window are the user's; scripted and auto-load presses use the synthetic entry.
   Key 1 toggles the shortcut reminder in frontend.m. Key 2 (Mac key code 19) cycles MetalFX frame interpolation:
   off -> 30+30 -> half the refresh rate -> uncapped (totk_mfx.m). Key 3 (key code 20) cycles the picture: original
   1600x900 -> with MetalFX -> native 1920x1080 -> native at the bigger render size (runtime_output_mode_cycle). Key 6 (key code 22) cycles the controller type: auto -> Xbox ->
   PlayStation -> Switch (runtime_gamepad_type_cycle, gamepad.m). Key 7 (key code 26) turns mouse mode on / off
   (runtime_mouse_toggle, gamepad.m; the window holds the pointer: frontend.m). Key 5 (key code 23) turns MetalFX edge
   smoothing on / off (taa_toggle, third_party/d2r_gfx/gfx/nvn/totk_taa.m). Held-key repeats do not cycle. */
enum { KEY_INTERP_TOGGLE = 19, KEY_OUTPUT_MODE_CYCLE = 20, KEY_ASPECT_TOGGLE = 21, KEY_PAD_TYPE_CYCLE = 22, KEY_TAA_TOGGLE = 23, KEY_MOUSE_TOGGLE = 26 };   /* 21: key 4, 16:9 -> fit to the window -> 21:9 (odyssey_aspect.c); 22: key 6; 23: key 5 */
static void m20a_arm_press(void);   /* M20A probe: stamp the press edge for the press->read latency */
void runtime_input_key_synthetic(unsigned code,int down){if(code>=128)return;pthread_mutex_lock(&inputLock);int press=down&&!keys[code];if(down&&!keys[code])pending|=button(code);keys[code]=(unsigned char)!!down;pthread_mutex_unlock(&inputLock);
 if(press&&button(code))m20a_arm_press();
 if(press&&code==18){extern void runtime_shortcut_help_toggle(void);runtime_shortcut_help_toggle();}
 if(press&&code==KEY_INTERP_TOGGLE){extern void dm_gfx_set_flag(const char *name,int v);dm_gfx_set_flag("mfx_interp",-1);}
 if(press&&code==KEY_OUTPUT_MODE_CYCLE)runtime_output_mode_cycle();
 if(press&&code==KEY_ASPECT_TOGGLE)runtime_aspect_toggle();
 if(press&&code==KEY_TAA_TOGGLE){extern void taa_toggle(void);taa_toggle();}
 if(press&&code==KEY_PAD_TYPE_CYCLE)runtime_gamepad_type_cycle();
 if(press&&code==KEY_MOUSE_TOGGLE)runtime_mouse_toggle();}
void runtime_input_key(unsigned code,int down){if(down&&code<128)user_input("keyboard");runtime_input_key_synthetic(code,down);}
void runtime_input_clear(void){pthread_mutex_lock(&inputLock);memset(keys,0,sizeof(keys));pending=0;pthread_mutex_unlock(&inputLock);runtime_gamepad_clear();}
static void initialize(void){pthread_mutex_lock(&inputLock);enabled=true;pthread_mutex_unlock(&inputLock);fprintf(stderr,"[totk] native keyboard controller initialized\n");}
/* Docked: one full-key controller as player 1 (id 0, style bit 0). Handheld (TOTK_OPERATION_MODE=handheld, the iPhone):
   the same input is the handheld controller (id 0x20, style bit 1, joy-cons attached) and player 1 is not connected;
   the game reads only the handheld controller in that mode. */
/* TOTK_INPUT_LOG=1: one line per new (state kind, controller id, style set) the game reads (which pad the game uses). */
static void input_log(const char *what,uint32_t id){static int on=-1;static uint64_t seen[64];static unsigned n;if(on<0)on=getenv("TOTK_INPUT_LOG")!=NULL;if(!on)return;
 uint64_t k=(uint64_t)(uintptr_t)what<<32^id^(uint64_t)styles<<48;for(unsigned i=0;i<n;i++)if(seen[i]==k)return;if(n<64)seen[n++]=k;
 fprintf(stderr,"[totk-input] game reads %s for controller %#x (supported styles %#x, ids allowed %d)\n",what,id,styles,allowed);}
static uint32_t pad_id(void){return runtime_handheld_mode()?0x20u:0u;}
static uint32_t pad_style(void){return runtime_handheld_mode()?2u:1u;}
static void supported_ids(const uint32_t *ids,uint64_t n){if(n>10||(!ids&&n))runtime_fail("invalid controller ids");
 if(getenv("TOTK_INPUT_LOG")){char b[128];size_t o=0;for(uint64_t i=0;i<n&&o<100;i++)o+=(size_t)snprintf(b+o,sizeof b-o," %#x",ids[i]);b[o]=0;fprintf(stderr,"[totk-input] game supports controller ids:%s\n",b);}pthread_mutex_lock(&inputLock);allowed=false;for(uint64_t i=0;i<n;i++)if(ids[i]==pad_id())allowed=true;pthread_mutex_unlock(&inputLock);}
static void supported_styles(uint32_t value){pthread_mutex_lock(&inputLock);styles=value;pthread_mutex_unlock(&inputLock);}
static uint32_t style(const uint32_t *id){if(!id)runtime_fail("controller id pointer");input_log("style set",*id);pthread_mutex_lock(&inputLock);uint32_t v=enabled&&allowed&&*id==pad_id()?(styles&pad_style()):0;pthread_mutex_unlock(&inputLock);return v;}

/* ---- M5a (2026-11-05): synthetic key injection, switch `smo_testkey`, default OFF ----
   The guest's only input source is this HLE: main imports the five nn::hid GetNpadStates
   specialisations and the loader binds them here (loader.c:239 beats the sdk module's own
   definition). Nothing on OHOS feeds keys[]: runtime_gamepad_read is an empty stub
   (ohos_stub.c:402), no window keys, no NAPI key export, and the system input path (OHOS MMI)
   never crosses into the guest. So the one injection point is keys[] via
   runtime_input_key_synthetic(), and this switch is the single producer that drives it.
   Files (first readable wins): /data/storage/el2/base/files/smo_testkey,
   /data/app/el2/100/base/com.vibecc.smo/files/smo_testkey.
   Content: "0" or absent = off (the default boot injects nothing); "1" = on with the defaults
   below; "1 <start_s> <down_ms> <up_ms> <count> <keycode>" overrides one by one.
   Read at load and re-polled every 500 ms until the run window closes, so a boot can also be
   turned on in place: the app seeds "0" (hdc cannot create files in the 0700 data dir, only
   overwrite them), then an `echo '1 ...' >` over that file starts the presses without a restart.
   With the switch on, the same presses are also reported for the JoyDual/Left/Right states,
   which are otherwise a disconnected (all-zero) state, so whichever style the game polls sees
   the press; the gate values are logged so a wrong-style poll is visible, not silent. */
/* M5g: the sixth field may be a key list with a repeat count per key, "38:10,36:10,40:10"
   (key:presses), so one boot can try A, then Plus, then B with >=40 s on each. A bare number keeps
   the old meaning (one key, `count` presses). Once a press makes `[io] opens` grow, the producer
   freezes on that key for the rest of the run (the task's "only watch this key" rule). */
/* M6p: 16 slots so one boot can cover A (the known gate) plus every still-untried button
   (X Y L R ZL ZR dpad4 minus) without a second schedule; the parse/log buffers grew with it. */
#define TK_MAXK 16
typedef struct { int start, down, up, count, key; int nkeys; int keys[TK_MAXK]; int per[TK_MAXK]; int nofreeze; } TkCfg;
static TkCfg g_tk = { 70, 500, 500, 200, 38, 1, { 38 }, { 0 } };   /* 38 = Mac key J = NpadButton_A (bit 0) */
static _Atomic int g_tk_live;   /* 1 once the switch file reads on; the only gate the readers use */
static int g_tk_fixed = -1;     /* M5g: key index frozen after a press made opens grow (-1 = not yet) */
#define TK_A "/data/storage/el2/base/files/smo_testkey"
#define TK_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_testkey"
/* The switch has to exist before hdc can overwrite it: the shell domain (u:r:sh:s0) cannot
   create files in an app data dir (verified: touch -> Permission denied), but it can overwrite
   the 0666 files the app created. Inside the process only TK_A resolves -- TK_B is the outside
   view of the same directory and fopen(TK_B,"w") fails there -- so the seed goes to TK_A first.
   Same "seed then let hdc overwrite" pattern as nvn_vk.c p_file_seed. */
static const char *tk_existing(void) {
    static const char *p[2] = { TK_A, TK_B };
    for (unsigned i = 0; i < 2; i++) { FILE *f = fopen(p[i], "r"); if (f) { fclose(f); return p[i]; } }
    return NULL;
}
static void tk_seed(void) {
    static const char *p[2] = { TK_A, TK_B };
    if (tk_existing()) return;
    for (unsigned i = 0; i < 2; i++) {
        FILE *f = fopen(p[i], "w");
        if (f) { fputs("0\n", f); fclose(f); fprintf(stderr, "[i] hid testkey: seeded %s = 0 (off)\n", p[i]); return; }
    }
    fprintf(stderr, "[i] hid testkey: cannot seed a switch file (both dirs refused)\n");
}
/* 1 = the file says on and the parameters are sane; g_tk is then filled in and announced. */
/* M6o: the stall-period experiment needs a fourth "key" that is not a key: full analog deflection.
   Codes 200..203 are sentinels handled by the inject loop (no keys[] bit is set, the stick override in
   runtime_input_stick_synth() is driven instead); every other code must still be a real button, so a
   typo in the schedule is rejected at parse time instead of being silently ignored. */
static int tk_stick(unsigned k) { return k >= 200 && k <= 203; }
static int tk_ok_key(unsigned k) { return tk_stick(k) || (k < 128 && button(k)); }
static int tk_parse(const char *path, int announce) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char line[256], spec[160] = ""; int ok = fgets(line, sizeof line, f) != NULL; fclose(f);
    int on = 0, st = 70, dn = 500, up = 500, ct = 200, key = 38, nf = 0, keep = 0;
    if (!ok) { if (announce) fprintf(stderr, "[i] hid testkey: %s: unreadable, off\n", path); return 0; }
    nf = sscanf(line, "%d %d %d %d %d %159s %d", &on, &st, &dn, &up, &ct, spec, &keep);
    if (nf < 1) { if (announce) fprintf(stderr, "[i] hid testkey: %s: unreadable, off\n", path); return 0; }
    if (on != 1) { if (announce) fprintf(stderr, "[i] hid testkey: %s: on=%d, off\n", path, on); return 0; }
    int nkeys = 0, keys[TK_MAXK], per[TK_MAXK], presses = 0;
    if (nf < 6 || !spec[0]) { keys[0] = 38; per[0] = 0; nkeys = 1; key = 38; }
    else {
        char *save = NULL;
        for (char *t = strtok_r(spec, ",", &save); t && nkeys < TK_MAXK; t = strtok_r(NULL, ",", &save)) {
            int k = -1, c = 1, got = sscanf(t, "%d:%d", &k, &c);
            if (got < 1 || k < 0 || !tk_ok_key((unsigned)k) || c < 1 || c > 5000) {
                if (announce) fprintf(stderr, "[i] hid testkey: %s: bad key item '%s', off\n", path, t);
                return 0;
            }
            keys[nkeys] = k; per[nkeys] = got >= 2 ? c : 0; presses += per[nkeys]; nkeys++;
        }
        if (!nkeys) { if (announce) fprintf(stderr, "[i] hid testkey: %s: empty key list, off\n", path); return 0; }
        key = keys[0];
    }
    /* Per-key counts drive the schedule; a bare key list (or a bare number) uses `ct` presses. */
    if (presses > 0) { ct = presses; if (ct > 5000) ct = 5000; }
    if (st < 0 || st > 600 || dn < 20 || dn > 5000 || up < 20 || up > 5000 ||
        ct < 1 || ct > 5000 || key < 0 || !tk_ok_key((unsigned)key)) {
        if (announce) fprintf(stderr, "[i] hid testkey: %s: bad parameters, off\n", path);
        return 0;
    }
    g_tk.start = st; g_tk.down = dn; g_tk.up = up; g_tk.count = ct; g_tk.key = key;
    g_tk.nkeys = nkeys; for (int i = 0; i < nkeys; i++) { g_tk.keys[i] = keys[i]; g_tk.per[i] = per[i]; }
    g_tk.nofreeze = (nf >= 7 && keep) ? 1 : 0;
    if (announce) {
        char seq[256]; size_t o = 0; seq[0] = 0;
        for (int i = 0; i < nkeys && o + 16 < sizeof seq; i++)
            o += (size_t)snprintf(seq + o, sizeof seq - o, "%s%d:%d", i ? "," : "", g_tk.keys[i], g_tk.per[i]);
        fprintf(stderr, "[i] hid testkey: ON keys=%s presses=%d start=%ds down=%dms up=%dms (%s)\n",
                seq, ct, st, dn, up, path);
    }
    return 1;
}
static int tk_on(void) { return atomic_load(&g_tk_live); }

/* ---- M5j (2026-11-05): synthetic analog stick, switch `smo_teststick`, C default ON ----
   M5i's finding: on the loading screen the game asks for PlayGuideMovieRStick.mp4 ("RStick" =
   right stick, the guide clip that teaches the camera), while every `[i] hid read` line showed
   lx=0 ly=0 rx=0 ry=0 -- this HLE has injected buttons only, never a stick value. This producer
   sweeps all four axes (r quadrant, l inverted) so a stick gate can be satisfied by software.
   Same SELinux fact as smo_testkey (input.c:92): the shell cannot create files in the app data
   dir, so the C default is on and the file exists only to turn it off. Files (first readable
   wins): /data/storage/el2/base/files/smo_teststick,
   /data/app/el2/100/base/com.vibecc.smo/files/smo_teststick.
   Content: absent or "1" = on with 0.7 amplitude / 2 s period; "0" = off;
   "1 <amp_permille> <period_ms>" overrides (amp 0..1000 of full scale, period 200..20000 ms).
   Re-polled every second, so an `echo 0 >` over the seeded file stops it mid-boot. */
#define ST_A "/data/storage/el2/base/files/smo_teststick"
#define ST_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_teststick"
static _Atomic int g_stick_live = 1;         /* 1 = inject; the switch file is only an off switch */
static _Atomic int g_stick_amp = 22937;      /* 0.70 * 32767, libnx full scale */
static _Atomic int g_stick_period_ms = 2000;
static _Atomic unsigned long g_stick_polls;  /* states handed to the guest with the stick applied */
/* cos(2*pi*k/64) * 22937, k = 0..63: the sweep without libm, and identical every run. */
static const int g_stick_cos[64] = {
  22937, 22827, 22496, 21949, 21191, 20229, 19071, 17731,
  16219, 14551, 12743, 10812,  8778,  6658,  4475,  2248,
      0, -2248, -4475, -6658, -8778,-10812,-12743,-14551,
 -16219,-17731,-19071,-20229,-21191,-21949,-22496,-22827,
 -22937,-22827,-22496,-21949,-21191,-20229,-19071,-17731,
 -16219,-14551,-12743,-10812, -8778, -6658, -4475, -2248,
      0,  2248,  4475,  6658,  8778, 10812, 12743, 14551,
  16219, 17731, 19071, 20229, 21191, 21949, 22496, 22827 };
static void stick_now(int *slx, int *sly, int *srx, int *sry) {
    int per = atomic_load(&g_stick_period_ms), amp = atomic_load(&g_stick_amp);
    uint64_t step = (uint64_t)per * 1000000ull / 64ull; if (!step) step = 1;
    unsigned k = (unsigned)((runtime_now_ns() / step) % 64ull);
    int rx = (int)((long long)g_stick_cos[k] * amp / 22937);
    int ry = (int)((long long)g_stick_cos[(k + 16u) & 63u] * amp / 22937);
    *srx = rx; *sry = ry; *slx = -rx; *sly = -ry;   /* left stick = same circle, inverted phase */
}
static const char *st_existing(void) {
    static const char *p[2] = { ST_A, ST_B };
    for (unsigned i = 0; i < 2; i++) { FILE *f = fopen(p[i], "r"); if (f) { fclose(f); return p[i]; } }
    return NULL;
}
static void st_seed(void) {
    if (st_existing()) return;
    static const char *p[2] = { ST_A, ST_B };
    for (unsigned i = 0; i < 2; i++) {
        FILE *f = fopen(p[i], "w");
        if (f) { fputs("1\n", f); fclose(f); fprintf(stderr, "[i] stick: seeded %s = 1 (on)\n", p[i]); return; }
    }
    fprintf(stderr, "[i] stick: cannot seed a switch file (both dirs refused)\n");
}
/* 1 = on (values filled in); 0 = off. `announce` logs transitions only. */
static int st_parse(const char *path, int announce) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    int on = 1, amp = 700, per = 2000, n = fscanf(f, "%d %d %d", &on, &amp, &per); fclose(f);
    if (n < 1) { if (announce) fprintf(stderr, "[i] stick: %s unreadable, off\n", path); atomic_store(&g_stick_live, 0); return 0; }
    if (!on) { if (announce && atomic_load(&g_stick_live)) fprintf(stderr, "[i] stick: %s on=0, off\n", path); atomic_store(&g_stick_live, 0); return 0; }
    if (amp < 0 || amp > 1000) amp = 700;
    if (per < 200 || per > 20000) per = 2000;
    atomic_store(&g_stick_amp, amp * 32767 / 1000);
    atomic_store(&g_stick_period_ms, per);
    atomic_store(&g_stick_live, 1);
    if (announce) fprintf(stderr, "[i] stick: ON amp=%d/1000 period=%dms (%s)\n", amp, per, path);
    return 1;
}
/* A fixed value from outside (kept for the task's API: lx/ly/rx/ry = 0,0,0,0 hands control back
   to the 2 s sweep). Only magnitudes beyond the sweep's reach matter, so this raises the floor. */
static _Atomic int g_stick_fixed[4];
void runtime_input_stick_synth(int lx, int ly, int rx, int ry) {
    atomic_store(&g_stick_fixed[0], lx); atomic_store(&g_stick_fixed[1], ly);
    atomic_store(&g_stick_fixed[2], rx); atomic_store(&g_stick_fixed[3], ry);
}
static void stick_apply(int *lx, int *ly, int *rx, int *ry) {
    int v[4]; stick_now(&v[0], &v[1], &v[2], &v[3]);
    for (int i = 0; i < 4; i++) { int f = atomic_load(&g_stick_fixed[i]); if (f > v[i] || -f > v[i]) v[i] = f; }
    int *d[4] = { lx, ly, rx, ry };
    for (int i = 0; i < 4; i++) { int c = v[i]; if (c > *d[i] || -c > *d[i]) *d[i] = c; }   /* keep the larger deflection */
}
/* The state assembly from keys[]/pending (inputLock held). Shared by every state kind so a poll
   of any style sees the same injection; the analog values keep their only role (stick shortcuts). */
static void assemble_locked(NpadState *state) {
    state->attributes = 1;
    for (unsigned i = 0; i < 128; i++) if (keys[i]) state->buttons |= button(i);
    state->buttons |= pending; pending = 0;
    state->lx = 32767 * ((int)keys[2] - (int)keys[0]);
    state->ly = 32767 * ((int)keys[13] - (int)keys[1]);
    state->rx = 32767 * ((int)keys[124] - (int)keys[123]);
    state->ry = 32767 * ((int)keys[126] - (int)keys[125]);
    /* M5j: the synthetic stick, before the direction bits so they follow it (past half
       deflection on the sweep's extremes, exactly as a real pad reports them). */
    if (atomic_load(&g_stick_live)) {
        stick_apply(&state->lx, &state->ly, &state->rx, &state->ry);
        atomic_fetch_add_explicit(&g_stick_polls, 1, memory_order_relaxed);
    }
    const int t = 16384;   /* NpadButton stick directions (bits 16-23), set past half deflection */
    state->buttons |= (uint64_t)(state->lx < -t) << 16 | (uint64_t)(state->ly > t) << 17 |
                      (uint64_t)(state->lx > t) << 18 | (uint64_t)(state->ly < -t) << 19 |
                      (uint64_t)(state->rx < -t) << 20 | (uint64_t)(state->ry > t) << 21 |
                      (uint64_t)(state->rx > t) << 22 | (uint64_t)(state->ry < -t) << 23;
}
/* `[i] hid read ...`: the value the game itself receives, logged only with the switch on
   (the default boot keeps its old log volume). Emitted on the first poll, on every button
   change, and once per 300 polls so a poll that never changes is still visible. */
static void read_log(const char *which, uint32_t id, uint32_t want, int gate, const NpadState *st) {
    if (!tk_on()) return;
    static unsigned long n; static uint64_t prev; static uint64_t prev_ns;
    n++;
    /* M5j: a moving stick changes buttons only at the extremes, so add a 1 s "whatever it is now"
       line while the stick switch is on - that line is the criterion-1 evidence (the value the
       game itself received). */
    uint64_t now = runtime_now_ns();
    int due = atomic_load(&g_stick_live) && now - prev_ns >= 1000000000ull;
    if (st->buttons == prev && n != 1 && n % 300 && !due) return;
    vk_boot_logf("[i] hid read %s id=%#x style=%#x gate=%d polls=%lu btn=%#llx lx=%d ly=%d rx=%d ry=%d",
                 which, id, want, gate, n, (unsigned long long)st->buttons,
                 st->lx, st->ly, st->rx, st->ry);
    prev = st->buttons; prev_ns = now;
}
/* ---- M20A (2026-11-08): input-delivery probe, switch `smo_m20a`, C default OFF ----
   The question M8A left open: after `smo_testkey`/`smo_m6q_sched` injects, is the guest "not
   reading", "reading 0", or "reading it and ignoring it"? The guest has no HID code of its own to
   gate: main's `nn::hid::GetNpadStates<Npad*State>` imports are ABS64 import cells that loader.c:246
   fills with the host pointer of full_states/handheld_states/joy_states (input.c:389-394), so
   "the guest read" == "pad_states was entered" and "what it read" == the NpadState handed back.
   This probe therefore needs NO gate and NO door: it only counts and records, per read,
   (a) our set bits (keys[]/pending, i.e. what the producer pressed), (b) the bits that actually
   went out in *out, (c) the guest frame counter and sampling number at that instant, (d) the
   reading thread, (e) press->first-read latency. A read whose set bits are nonzero proves delivery;
   a read where the returned bits lack the set bits proves a sync break inside this function.
   Files (first readable wins): /data/storage/el2/base/files/smo_m20a,
   /data/app/el2/100/base/com.vibecc.smo/files/smo_m20a. "1" = on, anything else = off. */
#define M20A_A "/data/storage/el2/base/files/smo_m20a"
#define M20A_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m20a"
#define M20A_SLOTS 1024
typedef struct { uint64_t ns, frame, set, seen; long poll; int style; uint64_t tid; } M20AEdge;
static _Atomic int g_m20a_on;
static _Atomic unsigned long g_m20a_calls[3];            /* 0 fullkey, 1 handheld, 2 joy-con */
static _Atomic unsigned long g_m20a_with[3];             /* reads whose set bits were nonzero */
static _Atomic unsigned long g_m20a_hit, g_m20a_miss;    /* set bits carried out / not carried out */
static _Atomic unsigned long long g_m20a_set_ns;         /* producer press edge (host ns) */
static _Atomic int g_m20a_arm;                           /* 1 = a press is waiting for its first read */
static _Atomic unsigned long g_m20a_lat_n, g_m20a_lat_us, g_m20a_lat_max;
static M20AEdge g_m20a_e[M20A_SLOTS];
static _Atomic unsigned g_m20a_en, g_m20a_eovf;
static unsigned long g_m20a_flushed;
static _Atomic unsigned long long g_m20a_last_set;
static int m20a_file_on(void) {
    static const char *p[2] = { M20A_A, M20A_B };
    for (int i = 0; i < 2; i++) { FILE *f = fopen(p[i], "r"); if (!f) continue;
        int v = 0; if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); return v == 1; }
    return 0;
}
static void m20a_seed(void) {
    static const char *p[2] = { M20A_A, M20A_B };
    for (int i = 0; i < 2; i++) { FILE *f = fopen(p[i], "r"); if (f) { fclose(f); return; } }
    for (int i = 0; i < 2; i++) { FILE *f = fopen(p[i], "w");
        if (f) { fputs("0\n", f); fclose(f); return; } }
}
/* Called from pad_states with the state it just built (probe off = three atomic loads). */
static void m20a_note(int style, uint64_t set, const NpadState *st) {
    if (!atomic_load(&g_m20a_on)) return;
    int s = style < 0 ? 0 : style > 2 ? 2 : style;
    atomic_fetch_add(&g_m20a_calls[s], 1);
    if (set) {
        atomic_fetch_add(&g_m20a_with[s], 1);
        if ((st->buttons & set) == set) atomic_fetch_add(&g_m20a_hit, 1);
        else atomic_fetch_add(&g_m20a_miss, 1);
        if (atomic_load(&g_m20a_arm)) {
            uint64_t now = (uint64_t)runtime_now_ns(), t0 = atomic_load(&g_m20a_set_ns);
            atomic_store(&g_m20a_arm, 0);
            if (now > t0) {
                unsigned long us = (unsigned long)((now - t0) / 1000ull);
                atomic_fetch_add(&g_m20a_lat_n, 1);
                atomic_fetch_add(&g_m20a_lat_us, us);
                if (us > atomic_load(&g_m20a_lat_max)) atomic_store(&g_m20a_lat_max, us);
            }
        }
    }
    uint64_t prev = atomic_exchange(&g_m20a_last_set, set);
    if (set != prev) {   /* one record per edge: the first read that carries a new set, and its release */
        unsigned i = atomic_fetch_add(&g_m20a_en, 1);
        if (i < M20A_SLOTS) {
            g_m20a_e[i].ns = (uint64_t)runtime_now_ns(); g_m20a_e[i].frame = atomic_load(&g_beat_frames);
            g_m20a_e[i].set = set; g_m20a_e[i].seen = st->buttons; g_m20a_e[i].poll = (long)st->sampling;
            g_m20a_e[i].style = s; g_m20a_e[i].tid = (uint64_t)(uintptr_t)pthread_self();
        } else atomic_fetch_add(&g_m20a_eovf, 1);
    }
}
static void m20a_arm_press(void) {
    if (!atomic_load(&g_m20a_on)) return;
    atomic_store(&g_m20a_set_ns, (unsigned long long)runtime_now_ns());
    atomic_store(&g_m20a_arm, 1);
}
/* One line a second whether or not the guest polls: a read count that stops moving is itself the
   answer ("the guest stopped reading"), and only a free-running printer can show that. */
static void *m20a_loop(void *arg) {
    (void)arg;
    uint64_t t0 = (uint64_t)runtime_now_ns();
    unsigned long pc[3] = { 0, 0, 0 }, pw = 0, ph = 0, pm = 0, pn = 0;
    for (;;) {
        usleep(1000000);
        unsigned long c[3], w = 0, h, m, n;
        for (int i = 0; i < 3; i++) c[i] = atomic_load(&g_m20a_calls[i]);
        w = atomic_load(&g_m20a_with[0]) + atomic_load(&g_m20a_with[1]) + atomic_load(&g_m20a_with[2]);
        h = atomic_load(&g_m20a_hit); m = atomic_load(&g_m20a_miss); n = atomic_load(&g_m20a_lat_n);
        unsigned long avg = n ? atomic_load(&g_m20a_lat_us) / n : 0;
        vk_boot_logf("[m20a] sum t=%llus d_full=%lu d_hand=%lu d_joy=%lu n_full=%lu n_hand=%lu n_joy=%lu "
                     "d_set=%lu set=%lu hit=%lu miss=%lu lat_ms_n=%lu lat_ms_avg=%lu lat_ms_max=%lu frame=%llu ovf=%u",
                     (unsigned long long)((runtime_now_ns() - (int64_t)t0) / 1000000000ull),
                     c[0] - pc[0], c[1] - pc[1], c[2] - pc[2], c[0], c[1], c[2],
                     w - pw, w, h, m, n, avg, atomic_load(&g_m20a_lat_max),
                     (unsigned long long)atomic_load(&g_beat_frames), atomic_load(&g_m20a_eovf));
        pc[0] = c[0]; pc[1] = c[1]; pc[2] = c[2]; pw = w; ph = h; pm = m; pn = n;
        unsigned en = atomic_load(&g_m20a_en); if (en > M20A_SLOTS) en = M20A_SLOTS;
        for (unsigned i = g_m20a_flushed; i < en; i++)
            vk_boot_logf("[m20a] edge t=%.3fs poll=%ld frame=%llu style=%d set=%#llx seen=%#llx tid=%#llx",
                         (double)(g_m20a_e[i].ns - t0) / 1e9, g_m20a_e[i].poll, g_m20a_e[i].frame,
                         g_m20a_e[i].style, (unsigned long long)g_m20a_e[i].set,
                         (unsigned long long)g_m20a_e[i].seen, (unsigned long long)g_m20a_e[i].tid);
        g_m20a_flushed = en;
        (void)ph; (void)pm; (void)pn;
    }
    return NULL;
}
__attribute__((constructor)) static void m20a_start(void) {
    m20a_seed();
    if (!m20a_file_on()) return;
    atomic_store(&g_m20a_on, 1);
    fprintf(stderr, "[m20a] input-delivery probe ON at load\n");
    pthread_t t;
    if (!pthread_create(&t, NULL, m20a_loop, NULL)) pthread_detach(t);
}
static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style);
static int full_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,1);}
static int handheld_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,2);}
static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style){if(max<=0)return 0;if(id)input_log(want_style==2?"handheld state":"full-key state",*id);if(!out||!id)runtime_fail("controller output pointer");pthread_mutex_lock(&inputLock);NpadState state={.sampling=++sample};
 uint64_t set_bits=0;if(atomic_load(&g_m20a_on)){for(unsigned i=0;i<128;i++)if(keys[i])set_bits|=button(i);set_bits|=pending;}
 int gate=enabled&&allowed&&*id==pad_id()&&want_style==pad_style()&&(styles&want_style);
 /* M5a: with the switch on the pad in use reports the injected presses even if the game never
    declared our style bit (gate=0 in the log says which side was missing). Off = the old gate. */
 if(gate||(tk_on()&&*id==pad_id()&&want_style==pad_style())){
 assemble_locked(&state);
 RuntimeGamepadState pad={0};runtime_gamepad_read(&pad);state.buttons|=pad.buttons;
 if(pad.buttons&~padSeen||abs(pad.lx)>16384||abs(pad.ly)>16384||abs(pad.rx)>16384||abs(pad.ry)>16384)user_input("controller");padSeen=pad.buttons;
 if(abs(pad.lx)>abs(state.lx))state.lx=pad.lx;if(abs(pad.ly)>abs(state.ly))state.ly=pad.ly;
 if(abs(pad.rx)>abs(state.rx))state.rx=pad.rx;if(abs(pad.ry)>abs(state.ry))state.ry=pad.ry;
 }*out=state;pthread_mutex_unlock(&inputLock);
 m20a_note(want_style==2?1:0,set_bits,&state);
 read_log(want_style==2?"handheld":"fullkey",*id,want_style,gate,&state);
 if(getenv("TOTK_INPUT_TRACE")){
  static uint64_t previous;static int32_t sticks[4];
  int changed=state.lx!=sticks[0]||state.ly!=sticks[1]||state.rx!=sticks[2]||state.ry!=sticks[3];
  if(state.buttons!=previous||changed)fprintf(stderr,"[totk-input] guest controller %#x style %#x buttons %#llx L %d,%d R %d,%d sample %lld\n",*id,want_style,(unsigned long long)state.buttons,state.lx,state.ly,state.rx,state.ry,(long long)state.sampling);
  previous=state.buttons;sticks[0]=state.lx;sticks[1]=state.ly;sticks[2]=state.rx;sticks[3]=state.ry;
 }
 return 1;}
static int disconnected_states(NpadState *out,int max,const uint32_t *id){if(id)input_log("joy-con state",*id);if(max<=0)return 0;if(!out)runtime_fail("controller output pointer");pthread_mutex_lock(&inputLock);*out=(NpadState){.sampling=++sample};pthread_mutex_unlock(&inputLock);return 1;}
/* M5a: JoyDual/JoyLeft/JoyRight. Switch off = the disconnected (all-zero) state exactly as before;
   switch on = the same injected presses, so a poll of this style is not silently empty. */
static int joy_states(NpadState *out,int max,const uint32_t *id){
 if(!tk_on())return disconnected_states(out,max,id);
 if(id)input_log("joy-con state",*id);
 if(max<=0)return 0;if(!out||!id)runtime_fail("controller output pointer");
 pthread_mutex_lock(&inputLock);NpadState state={.sampling=++sample};
 int inuse=*id==pad_id();if(inuse)assemble_locked(&state);
 uint64_t jset=0;{for(unsigned i=0;i<128;i++)if(keys[i])jset|=button(i);jset|=pending;}
 *out=state;pthread_mutex_unlock(&inputLock);
 m20a_note(2,jset,&state);
 read_log("joy",*id,0,inuse,&state);
 return 1;}
static void hold_type(int type){if(type<0||type>1)runtime_fail("invalid joy hold type");holdType=type;(void)holdType;}
static void disconnect(const uint32_t *id){if(!id)runtime_fail("disconnect id");input_log("disconnect",*id);if(*id==pad_id()){   /* only the pad in use: in handheld mode TOTK disconnects player 1 */pthread_mutex_lock(&inputLock);allowed=false;pthread_mutex_unlock(&inputLock);runtime_input_clear();}}
/* Mouse and keyboard devices: the Mac keyboard drives the full-key controller, so the game sees no mouse
   (not connected) and a keyboard with no keys, avoiding duplicated input. Layouts follow the SDK (0x28 / 0x30 bytes). */
typedef struct{int64_t sampling;int32_t x,y,dx,dy,wx,wy;uint32_t buttons,attributes;} MouseState;
typedef struct{int64_t sampling;uint32_t modifiers,attributes;uint64_t keys[4];} KeyboardState;
_Static_assert(sizeof(MouseState)==40&&sizeof(KeyboardState)==48,"mouse/keyboard ABI");
static bool mouseReady,keyboardReady;
static void initialize_mouse(void){pthread_mutex_lock(&inputLock);mouseReady=true;pthread_mutex_unlock(&inputLock);}
static void initialize_keyboard(void){pthread_mutex_lock(&inputLock);keyboardReady=true;pthread_mutex_unlock(&inputLock);}
static int mouse_states(MouseState *out,int max){if(max<=0)return 0;if(!out)runtime_fail("mouse output pointer");pthread_mutex_lock(&inputLock);if(!mouseReady)runtime_fail("mouse used before InitializeMouse");*out=(MouseState){.sampling=++sample};pthread_mutex_unlock(&inputLock);return 1;}
static void keyboard_state(KeyboardState *out){if(!out)runtime_fail("keyboard output pointer");pthread_mutex_lock(&inputLock);if(!keyboardReady)runtime_fail("keyboard used before InitializeKeyboard");*out=(KeyboardState){.sampling=++sample};pthread_mutex_unlock(&inputLock);}
/* Controller support applet (the Switch's "connect controllers" screen). TOTK opens it in handheld mode; there is
   exactly one controller here, so it returns at once: one player, on the controller in use (ControllerSupportResultInfo:
   int8 player count, 3 bytes padding, uint32 selected id). */
typedef struct{int8_t players;uint8_t pad[3];uint32_t selected;} ControllerSupportResult;
_Static_assert(sizeof(ControllerSupportResult)==8,"controller support result ABI");
static uint32_t show_controller_support(ControllerSupportResult *out,const void *arg){
 (void)arg;if(out)*out=(ControllerSupportResult){.players=1,.selected=pad_id()};
 static int once;if(!once++)fprintf(stderr,"[totk] controller support applet: one player on controller %#x\n",pad_id());
 return 0;
}
/* Bayonetta 3 reads one state at a time (GetNpadState), has a touch screen (no touches here: the Mac has none), and sets
   the handheld activation mode. */
static void full_state(NpadState *out,const uint32_t *id){full_states(out,1,id);}
static void handheld_state(NpadState *out,const uint32_t *id){handheld_states(out,1,id);}
static void joy_state(NpadState *out,const uint32_t *id){joy_states(out,1,id);}
static void mouse_state(MouseState *out){mouse_states(out,1);}
static void handheld_activation(int mode){(void)mode;}
static void initialize_touch(void){}
/* TouchScreenState<N>: {int64 sampling, int32 count, int32 reserved, TouchState[N] of 0x28 bytes}; count 0. */
static void touch_states_1(uint8_t *out){if(!out)runtime_fail("touch output pointer");pthread_mutex_lock(&inputLock);memset(out,0,16+1*0x28);int64_t n=++sample;memcpy(out,&n,8);pthread_mutex_unlock(&inputLock);}
static void touch_states_16(uint8_t *out){if(!out)runtime_fail("touch output pointer");pthread_mutex_lock(&inputLock);memset(out,0,16+16*0x28);int64_t n=++sample;memcpy(out,&n,8);pthread_mutex_unlock(&inputLock);}
static uint32_t show_controller_support_arg(const void *arg){return show_controller_support(NULL,arg);}
/* Odyssey initializes the console's separate seven-axis sensor during boot. A
   Mac has no corresponding console sensor; report no samples while retaining
   normal controller motion through the six-axis path in hid_feedback.c. */
static uint32_t seven_axis_init(void *work,size_t size){
 if(!work||size<0x80000)runtime_fail("seven-axis sensor work buffer");
 fprintf(stderr,"[odyssey] console seven-axis sensor unavailable; controller motion remains separate\n");
 return 0;
}
/* Success, not void: VR Start (22 October) checks StartSevenSixAxisSensor's nn::Result and aborts on nonzero
   (main+0x7b255c); a void handler left whatever was in w0. Callers of the void SDK functions ignore w0. */
static uint32_t seven_axis_noop(void){return 0;}
static bool seven_axis_at_rest(void){return true;}
static int seven_axis_states(void *out,int count){(void)out;(void)count;return 0;}
static void initialize_debug_pad(void){fprintf(stderr,"[odyssey] debug pad unavailable; normal controller remains active\n");}
static void debug_pad_state(void *out){if(!out)runtime_fail("debug pad state output");memset(out,0,0x20);}
static void joy_dual(const uint32_t *id){if(!id)runtime_fail("joy-dual controller id");}
/* Vibration devices and the six-axis sensor: runtime/hid_feedback.c (none unless TOTK_RUMBLE=1 / TOTK_GYRO=1). */
void *runtime_input_override(const char *name){
 void *feedback=runtime_hid_feedback_override(name);if(feedback)return feedback;
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn3hid14InitializeNpadEv",initialize},{"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm",supported_ids},{"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",supported_styles},
 {"_ZN2nn3hid15GetNpadStyleSetERKj",style},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj",full_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj",joy_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj",joy_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj",joy_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj",handheld_states},
 {"_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE",hold_type},{"_ZN2nn3hid14DisconnectNpadERKj",disconnect},
 {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",show_controller_support},
 {"_ZN2nn3hid15InitializeMouseEv",initialize_mouse},{"_ZN2nn3hid18InitializeKeyboardEv",initialize_keyboard},
 {"_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi",mouse_states},{"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE",keyboard_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj",full_state},{"_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj",handheld_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj",joy_state},{"_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj",joy_state},
 {"_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj",joy_state},{"_ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE",mouse_state},
 {"_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE",handheld_activation},{"_ZN2nn3hid21InitializeTouchScreenEv",initialize_touch},
 {"_ZN2nn3hid19GetTouchScreenStateILm1EEEvPNS0_16TouchScreenStateIXT_EEE",touch_states_1},{"_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE",touch_states_16},
 {"_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE",show_controller_support_arg},
 {"_ZN2nn3hid28InitializeSevenSixAxisSensorEPvm",seven_axis_init},
 {"_ZN2nn3hid26FinalizeSevenSixAxisSensorEv",seven_axis_noop},
 {"_ZN2nn3hid23StartSevenSixAxisSensorEv",seven_axis_noop},
 {"_ZN2nn3hid22StopSevenSixAxisSensorEv",seven_axis_noop},
 {"_ZN2nn3hid32ResetSevenSixAxisSensorTimestampEv",seven_axis_noop},
 {"_ZN2nn3hid26IsSevenSixAxisSensorAtRestEv",seven_axis_at_rest},
 {"_ZN2nn3hid27GetSevenSixAxisSensorStatesEPNS0_23SevenSixAxisSensorStateEi",seven_axis_states},
 {"_ZN2nn3hid18InitializeDebugPadEv",initialize_debug_pad},
 {"_ZN2nn3hid16GetDebugPadStateEPNS0_13DebugPadStateE",debug_pad_state},
 {"_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj",joy_dual}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}

/* TOTK_INPUT_SCRIPT="sec:keycode:ms,...": press Mac key codes at given seconds after start (diagnostics without a
   picture: does the game react to a button). Key codes are the frontend's (J=38 is A, Return=36 is Plus). */
#include <unistd.h>
static void *input_script(void *arg) {
    const char *p = arg; uint64_t t0 = runtime_now_ns();
    while (p && *p) {
        double at = strtod(p, (char **)&p); if (*p++ != ':') break;
        unsigned code = (unsigned)strtoul(p, (char **)&p, 10); if (*p++ != ':') break;
        unsigned ms = (unsigned)strtoul(p, (char **)&p, 10); if (*p == ',') p++;
        while ((runtime_now_ns() - t0) / 1e9 < at) usleep(10000);
        fprintf(stderr, "[totk-input] script: key %u down for %u ms at %.1f s\n", code, ms, at);
        runtime_input_key_synthetic(code, 1); usleep(ms * 1000); runtime_input_key_synthetic(code, 0);
    }
    return NULL;
}
__attribute__((constructor)) static void input_script_start(void) {
    const char *e = getenv("TOTK_INPUT_SCRIPT"); if (!e) return;
    pthread_t t; pthread_create(&t, NULL, input_script, (void *)e); pthread_detach(t);
}

/* M6o: the injection scoreboard. The task's question is binary ("is the game waiting for input?"), so
   every press records the five counters that would move if it were: romfs opens/reads (the load queue),
   nvn submit + cumulative draws (the render side), and main's SVC count / current wait state (the guest
   loop). Deltas are printed per press by the caller; this snapshot is read-only and free of locks other
   than runtime_main_snap's trylock. */
typedef struct { unsigned long long opens, reads, draws, mnsvc, mobj; unsigned infl; unsigned long frames, submit; unsigned msvc; int mwk; } InjSnap;
static void inj_snap(InjSnap *s) {
    memset(s, 0, sizeof *s); s->mwk = -9;
    char lp[160]; lp[0] = 0;
    extern void runtime_romfs_io_snapshot(unsigned long long *, unsigned long long *, unsigned *, char *, unsigned);
    runtime_romfs_io_snapshot(&s->opens, &s->reads, &s->infl, lp, sizeof lp);
    s->frames = atomic_load(&g_beat_frames); s->submit = atomic_load(&g_beat_nvn_submit);
    extern unsigned long long runtime_draws_total(void);
    s->draws = runtime_draws_total();
    extern void runtime_main_snap(unsigned long long *, unsigned *, int *, unsigned long long *);
    runtime_main_snap(&s->mnsvc, &s->msvc, &s->mwk, &s->mobj);
}
/* M6o: 200..203 = full analog deflection instead of a button (see tk_stick()). The stick override only
   raises the floor (`keep the larger deflection`), so clearing it to 0,0,0,0 hands control back to the
   normal 0.7-amplitude sweep - the same contract runtime_input_stick_synth() already documents. */
static void tk_stick_edge(unsigned kc, int down) {
    static const int v[4][4] = { { 32767, 0, 0, 0 }, { 0, 32767, 0, 0 }, { 0, 0, 32767, 0 }, { 32767, 32767, 32767, 32767 } };
    unsigned i = kc - 200u; if (i > 3u) return;
    if (down) runtime_input_stick_synth(v[i][0], v[i][1], v[i][2], v[i][3]);
    else runtime_input_stick_synth(0, 0, 0, 0);
}
static void tk_press(unsigned kc, int down) { if (tk_stick(kc)) tk_stick_edge(kc, down); else runtime_input_key_synthetic(kc, down); }

/* M5a: the `smo_testkey` producer. One key, a fixed rhythm, both edges logged with the render
   frame counter (g_beat_frames) so the inject lines can be lined up with the picture captures.
   While the switch reads off the thread only re-polls the file (no keys, no log growth); it
   retires once the run window is over so an untouched boot keeps no extra thread alive. */
static void *testkey_loop(void *arg) {
    (void)arg;
    uint64_t t0 = runtime_now_ns();
    while (!atomic_load(&g_tk_live)) {
        if ((runtime_now_ns() - t0) / 1000000000ull > 400) return NULL;   /* past the title/idle window */
        usleep(500000);
        const char *p = tk_existing();
        if (p && tk_parse(p, 1)) {
            atomic_store(&g_tk_live, 1);
            vk_boot_logf("[i] hid testkey: live now (%s), presses start at t=%ds from load", p, g_tk.start);
        }
    }
    while ((runtime_now_ns() - t0) / 1000000000ull < (uint64_t)g_tk.start) usleep(50000);
    unsigned long long f0 = atomic_load(&g_beat_frames);
    {
        char seq[256]; size_t o = 0; seq[0] = 0;
        for (int i = 0; i < g_tk.nkeys && o + 16 < sizeof seq; i++)
            o += (size_t)snprintf(seq + o, sizeof seq - o, "%s%d:%d", i ? "," : "", g_tk.keys[i], g_tk.per[i]);
        vk_boot_logf("[i] hid inject start keys=%s start=%ds down=%dms up=%dms count=%d frame=%llu",
                     seq, g_tk.start, g_tk.down, g_tk.up, g_tk.count, f0);
    }
    for (int n = 1; n <= g_tk.count; n++) {
        double at = (double)(runtime_now_ns() - t0) / 1e9;
        int ki = 0;
        if (g_tk.nkeys > 1) {
            if (g_tk_fixed >= 0 && !g_tk.nofreeze) ki = g_tk_fixed;   /* M5g: adaptive freeze once a key moved opens */
            else if (g_tk.per[0] <= 0) ki = (n - 1) % g_tk.nkeys;   /* bare list: alternate every press */
            else {
                int acc = 0, k = 0;
                for (; k < g_tk.nkeys - 1; k++) { acc += g_tk.per[k]; if (n <= acc) break; }
                ki = k;
            }
        }
        unsigned kc = (unsigned)g_tk.keys[ki];
        unsigned long long op0 = 0, rd0 = 0, op1 = 0, rd1 = 0; unsigned inf0 = 0, inf1 = 0;
        char lp[160] = "";
        extern void runtime_romfs_io_snapshot(unsigned long long *, unsigned long long *, unsigned *, char *, unsigned);
        InjSnap a, b;
        inj_snap(&a);
        runtime_romfs_io_snapshot(&op0, &rd0, &inf0, lp, sizeof lp);
        vk_boot_logf("[i] hid inject btn=%#llx down=1 frame=%llu t=%.1f n=%d key=%u %s io opens=%llu reads=%llu inflight=%u last=%s",
                     (unsigned long long)button(kc), (unsigned long long)atomic_load(&g_beat_frames), at, n, kc,
                     tk_stick(kc) ? "stick=full" : "key",
                     op0, rd0, inf0, lp[0] ? lp : "-");
        tk_press(kc, 1);
        usleep((useconds_t)g_tk.down * 1000);
        tk_press(kc, 0);
        inj_snap(&b);
        runtime_romfs_io_snapshot(&op1, &rd1, &inf1, lp, sizeof lp);
        vk_boot_logf("[i] hid inject btn=%#llx down=0 frame=%llu t=%.1f n=%d key=%u %s io opens=%llu reads=%llu inflight=%u last=%s "
                     "| d: opens=%+lld reads=%+lld frames=%+ld submit=%+ld draws=%+lld mnsvc=%+lld msvc=%#x mwk=%d mobj=%#llx",
                     (unsigned long long)button(kc), (unsigned long long)atomic_load(&g_beat_frames),
                     (double)(runtime_now_ns() - t0) / 1e9, n, kc, tk_stick(kc) ? "stick=full" : "key",
                     op1, rd1, inf1, lp[0] ? lp : "-",
                     (long long)(b.opens - a.opens), (long long)(b.reads - a.reads),
                     (long)b.frames - (long)a.frames, (long)b.submit - (long)a.submit,
                     (long long)b.draws - (long long)a.draws, (long long)b.mnsvc - (long long)a.mnsvc,
                     b.msvc, b.mwk, b.mobj);
        if (op1 > op0 && g_tk.nkeys > 1 && g_tk_fixed < 0) {   /* M5g: this key started a load */
            g_tk_fixed = ki;
            vk_boot_logf("[i] hid adapt: opens %llu->%llu after key=%u n=%d, freezing on this key",
                         op0, op1, kc, n);
        }
        usleep((useconds_t)g_tk.up * 1000);
    }
    vk_boot_logf("[i] hid inject done n=%d frames=%llu", g_tk.count,
                 atomic_load(&g_beat_frames) - f0);
    return NULL;
}
__attribute__((constructor)) static void testkey_load(void) {
    tk_seed();
    const char *p = tk_existing();
    if (p && tk_parse(p, 1)) { atomic_store(&g_tk_live, 1); return; }
    fprintf(stderr, "[i] hid testkey: off at load (%s)\n", p ? p : "no switch file");
}
__attribute__((constructor)) static void testkey_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, testkey_loop, NULL)) return;   /* a diagnostic aid never fails a boot */
    pthread_detach(t);
}

/* ---- M6q (2026-11-06): absolute-time key schedule, switch `smo_m6q_sched`, C default OFF ----
   The M5a producer above presses on a fixed down/up rhythm, and `up` is capped at 5 s, so an
   experiment that needs "one press per 60 s" plus "a 5 s grid of other keys" cannot be expressed
   there. This producer reads an absolute timeline instead: items `"<t_s>:<key>:<hold_ms>"` separated
   by commas or newlines (t_s = seconds after load, float allowed). Same injection point as M5a
   (runtime_input_key_synthetic through tk_press) and the same SELinux file contract: the app seeds
   the file so hdc can overwrite it, first readable of TK_A_M6Q / TK_B_M6Q wins.
   Every press logs the M6o scoreboard (opens/reads/frames/submit/draws/main SVC) before and after,
   so "did THIS key move anything" stays a per-press number. Read-only apart from the injection.
   Content other than a valid list (absent, "0", a parse error) = OFF, and the producer never
   touches the M5a switch or its state. */
#define M6Q_SCHED_A "/data/storage/el2/base/files/smo_m6q_sched"
#define M6Q_SCHED_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m6q_sched"
#define M6Q_ITEMS 256
typedef struct { double at; unsigned key; unsigned hold; } M6QItem;
static M6QItem g_m6q_it[M6Q_ITEMS];
static int g_m6q_n;   /* parsed items, 0 = schedule off */
static const char *m6q_existing(void) {
    static const char *p[2] = { M6Q_SCHED_A, M6Q_SCHED_B };
    for (unsigned i = 0; i < 2; i++) { FILE *f = fopen(p[i], "r"); if (f) { fclose(f); return p[i]; } }
    return NULL;
}
static void m6q_seed(void) {
    static const char *p[2] = { M6Q_SCHED_A, M6Q_SCHED_B };
    if (m6q_existing()) return;
    for (unsigned i = 0; i < 2; i++) {
        FILE *f = fopen(p[i], "w");
        if (f) { fputs("0\n", f); fclose(f); return; }
    }
}
/* "<t>:<key>:<hold>" items; anything else stops the parse and leaves the schedule off. */
static int m6q_parse(const char *path, int announce) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char buf[4096]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    int cnt = 0; char *save = NULL;
    for (char *t = strtok_r(buf, ", \t\r\n", &save); t; t = strtok_r(NULL, ", \t\r\n", &save)) {
        double at = 0; unsigned k = 0, hold = 0; char extra = 0;
        if (sscanf(t, "%lf:%u:%u%c", &at, &k, &hold, &extra) != 3) { cnt = 0; break; }
        if (at < 0 || at > 3600.0 || !tk_ok_key(k) || hold < 20 || hold > 5000 || cnt >= M6Q_ITEMS) { cnt = 0; break; }
        g_m6q_it[cnt].at = at; g_m6q_it[cnt].key = k; g_m6q_it[cnt].hold = hold; cnt++;
    }
    if (cnt <= 0) { if (announce) fprintf(stderr, "[i] m6q sched: %s: no items, off\n", path); return 0; }
    for (int i = 1; i < cnt; i++) {   /* insertion sort by time: a hand-written list need not be ordered */
        M6QItem v = g_m6q_it[i]; int j = i - 1;
        while (j >= 0 && g_m6q_it[j].at > v.at) { g_m6q_it[j + 1] = g_m6q_it[j]; j--; }
        g_m6q_it[j + 1] = v;
    }
    g_m6q_n = cnt;
    if (announce) fprintf(stderr, "[i] m6q sched: ON n=%d t=%.1f..%.1f s (%s)\n",
                          cnt, g_m6q_it[0].at, g_m6q_it[cnt - 1].at, path);
    return 1;
}
static void *m6q_loop(void *arg) {
    (void)arg;
    uint64_t t0 = runtime_now_ns();
    while (!g_m6q_n) {   /* the file may be pushed just after launch: poll for 300 s, then give up */
        if ((runtime_now_ns() - t0) / 1000000000ull > 300) return NULL;
        usleep(500000);
        const char *p = m6q_existing();
        if (p) m6q_parse(p, 1);
    }
    vk_boot_logf("[i] m6q sched start n=%d first=%.1fs last=%.1fs", g_m6q_n,
                 g_m6q_it[0].at, g_m6q_it[g_m6q_n - 1].at);
    for (int i = 0; i < g_m6q_n; i++) {
        M6QItem *it = &g_m6q_it[i];
        while ((double)(runtime_now_ns() - t0) / 1e9 < it->at) usleep(20000);
        double at = (double)(runtime_now_ns() - t0) / 1e9;
        InjSnap a, b;
        inj_snap(&a);
        vk_boot_logf("[i] m6q sched key=%u t=%.1f n=%d/%d down=1 frame=%llu opens=%llu reads=%llu frames=%lu submit=%lu draws=%llu mnsvc=%llu wk=%d",
                     it->key, at, i + 1, g_m6q_n, (unsigned long long)atomic_load(&g_beat_frames),
                     a.opens, a.reads, a.frames, a.submit, a.draws, a.mnsvc, a.mwk);
        tk_press(it->key, 1);
        usleep((useconds_t)it->hold * 1000);
        tk_press(it->key, 0);
        inj_snap(&b);
        vk_boot_logf("[i] m6q sched key=%u t=%.1f n=%d/%d up frame=%llu | d: opens=%+lld reads=%+lld frames=%+ld submit=%+ld draws=%+lld mnsvc=%+lld msvc=%#x mwk=%d",
                     it->key, (double)(runtime_now_ns() - t0) / 1e9, i + 1, g_m6q_n,
                     (unsigned long long)atomic_load(&g_beat_frames),
                     (long long)(b.opens - a.opens), (long long)(b.reads - a.reads),
                     (long)b.frames - (long)a.frames, (long)b.submit - (long)a.submit,
                     (long long)b.draws - (long long)a.draws, (long long)b.mnsvc - (long long)a.mnsvc,
                     b.msvc, b.mwk);
    }
    vk_boot_logf("[i] m6q sched done n=%d t=%.1f", g_m6q_n, (double)(runtime_now_ns() - t0) / 1e9);
    return NULL;
}
__attribute__((constructor)) static void m6q_start(void) {
    m6q_seed();
    const char *p = m6q_existing();
    if (p && !m6q_parse(p, 1)) { fprintf(stderr, "[i] m6q sched: off at load (%s)\n", p); }
    pthread_t t;
    if (pthread_create(&t, NULL, m6q_loop, NULL)) return;
    pthread_detach(t);
}

/* M5j: the `smo_teststick` producer. One line a second with the values the sweep is producing
   and `applied` = how many states the guest actually received with the stick in them (0 = the
   game stopped polling HID, which is itself the answer). Re-polls the switch file every second;
   without the file the C default stays on, so a boot with no shell setup is still the experiment. */
static void *stick_loop(void *arg) {
    (void)arg;
    for (;;) {
        usleep(1000000);
        const char *p = st_existing();
        if (p) {
            int was = atomic_load(&g_stick_live);
            st_parse(p, 0);
            if (was != atomic_load(&g_stick_live))
                vk_boot_logf("[i] stick: switch %s -> %s", p, atomic_load(&g_stick_live) ? "ON" : "OFF");
        }
        if (!atomic_load(&g_stick_live)) continue;
        int lx, ly, rx, ry; stick_now(&lx, &ly, &rx, &ry);
        vk_boot_logf("[i] stick lx=%d ly=%d rx=%d ry=%d applied=%lu amp=%d period=%dms",
                     lx, ly, rx, ry, (unsigned long)atomic_load(&g_stick_polls),
                     (int)atomic_load(&g_stick_amp), (int)atomic_load(&g_stick_period_ms));
    }
    return NULL;
}
__attribute__((constructor)) static void stick_load(void) {
    st_seed();
    const char *p = st_existing();
    if (p) { st_parse(p, 1); return; }
    fprintf(stderr, "[i] stick: ON by C default (0.7 / 2 s), no switch file\n");
}
__attribute__((constructor)) static void stick_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, stick_loop, NULL)) return;
    pthread_detach(t);
}

/* Optional local integration-test command stream. Appending complete lines lets tests inspect each
   actual frame before choosing the next input, instead of guessing load/menu timings. Disabled normally.
   Commands: key <Mac keycode> <milliseconds>, down/up <Mac keycode>, shot <path>, wshot <path>, passdump <frames>, gpucapture <path>.gputrace, querylog <frames>, anomdump,
   kbtest / kbtype <text> / kbenter / kbesc / kbshot <path> (the text entry bar), threads (dump every guest thread's wait), hletrace <0|1|2> (host calls the game makes), framestep <ms> (pin the frame step), close. */
extern void dm_gfx_shot_at(long frames, const char *path);
extern void runtime_front_request_close(void);
static void *input_file(void *arg) {
    FILE *f = fopen((const char *)arg, "r"); if (!f) runtime_fail("cannot open TOTK_INPUT_FILE");
    char line[1200];
    for (;;) {
        if (!fgets(line, sizeof line, f)) { if (ferror(f)) runtime_fail("diagnostic input read"); clearerr(f); usleep(100000); continue; }
        if (runtime_survey_command(line)) continue;
        unsigned code, ms; int used = 0;
        if (sscanf(line, "down %u %n", &code, &used) == 1 && used && !line[used] && code < 128) {
            fprintf(stderr,"[totk-input] file: key %u down\n",code);runtime_input_key_synthetic(code,1);
        } else if (sscanf(line, "up %u %n", &code, &used) == 1 && used && !line[used] && code < 128) {
            fprintf(stderr,"[totk-input] file: key %u up\n",code);runtime_input_key_synthetic(code,0);
        } else if (sscanf(line, "key %u %u %n", &code, &ms, &used) == 2 && used && !line[used] && code < 128 && ms > 0 && ms <= 5000) {
            fprintf(stderr, "[totk-input] file: key %u for %u ms\n", code, ms);
            runtime_input_key_synthetic(code, 1); usleep(ms * 1000); runtime_input_key_synthetic(code, 0);
        } else if (sscanf(line, "passdump %u %n", &code, &used) == 1 && used && !line[used] && code > 0 && code < 16) {
            extern void dm_gfx_passdump_next(long n); dm_gfx_passdump_next((long)code);
        } else if (!strncmp(line, "gpucapture ", 11) && line[11]) {   /* "gpucapture <path>.gputrace": a Metal capture of the next frame (needs MTL_CAPTURE_ENABLED=1) */
            char *e = line + strlen(line); while (e > line + 11 && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
            extern void dm_gfx_gpucapture_next(const char *path); dm_gfx_gpucapture_next(line + 11);
        } else if (!strncmp(line, "anomdump", 8) && (line[8] == '\n' || line[8] == '\r' || !line[8])) {   /* TOTK_ANOMALY=<dir>: write the per-draw records (with GPU sample counts) of the last frames now */
            extern void dm_gfx_anomdump(void); dm_gfx_anomdump();
        } else if (sscanf(line, "querylog %u %n", &code, &used) == 1 && used && !line[used] && code > 0 && code <= 600) {
            extern void dm_gfx_querylog_next(long n); dm_gfx_querylog_next((long)code);   /* occlusion query results of the next n frames */
        } else if (!strncmp(line, "flag ", 5)) {
            /* "flag <name> <0|1>": switch a graphics behaviour while the game runs (A/B without restarting); rumble / gyro mute and unmute those */
            char name[64]; int v; if (sscanf(line + 5, "%63s %d", name, &v) != 2) runtime_fail("diagnostic flag needs <name> <0|1>");
            extern void dm_gfx_set_flag(const char *name, int v); if (!runtime_hid_feedback_flag(name, v)) dm_gfx_set_flag(name, v);
        } else if (!strncmp(line, "texdump ", 8)) {
            extern void dm_gfx_texdump_next(const char *path);
            line[strcspn(line, "\r\n")] = 0; if (!line[8] || !strstr(line + 8, "%d")) runtime_fail("diagnostic texdump path needs %d");
            dm_gfx_texdump_next(line + 8);
        } else if (sscanf(line, "shotn %u %n", &code, &used) == 1 && used && code > 0 && code <= 60) {
            /* "shotn <n> <path with %d>": screenshots of n consecutive game frames (frame flicker); %d = 0..n-1 */
            line[strcspn(line, "\r\n")] = 0; const char *fmt = line + used;
            if (!*fmt || !strstr(fmt, "%d") || strchr(strstr(fmt, "%d") + 2, '%')) runtime_fail("diagnostic shotn path needs one %d");
            for (unsigned i = 0; i < code; i++) { char p[1200]; snprintf(p, sizeof p, fmt, (int)i); dm_gfx_shot_at(2 + (long)i, p); }
        } else if (!strncmp(line, "shot ", 5)) {
            line[strcspn(line, "\r\n")] = 0; if (!line[5]) runtime_fail("diagnostic screenshot path");
            dm_gfx_shot_at(2, line + 5);
        } else if (!strncmp(line, "wshot ", 6)) {   /* "wshot <path>": the next shown picture as the window shows it (FPS box and the bottom-left box included) */
            line[strcspn(line, "\r\n")] = 0; if (!line[6]) runtime_fail("diagnostic window screenshot path");
            extern void dm_gfx_window_shot(const char *path); dm_gfx_window_shot(line + 6);
        } else if (!strcmp(line, "kbtest\n")) {   /* text entry bar the way the game names a horse (runtime/swkbd_host.c) */
            runtime_swkbd_selftest();
        } else if (!strncmp(line, "kbtype ", 7)) {   /* "kbtype <text>": typed into the open bar */
            line[strcspn(line, "\r\n")] = 0; runtime_front_text_test("type", line + 7);
        } else if (!strncmp(line, "kbshot ", 7)) {   /* "kbshot <path>": the open bar as a PNG */
            line[strcspn(line, "\r\n")] = 0; runtime_front_text_test("shot", line + 7);
        } else if (!strcmp(line, "kbenter\n") || !strcmp(line, "kbesc\n")) {
            runtime_front_text_test(line[2] == 'e' && line[3] == 'n' ? "enter" : "esc", NULL);
        } else if (sscanf(line, "hletrace %u %n", &code, &used) == 1 && used && !line[used] && code <= 2) {
            runtime_hle_trace_set((int)code);   /* 1 count host calls, 2 also log the first call of each, 0 stop and print the counts */
        } else if (!strncmp(line, "framestep ", 10)) {   /* "framestep <ms>": pin the real-time frame step, 0 = back to real time */
            char *end; double ms = strtod(line + 10, &end); if (end == line + 10 || ms < 0 || ms > 100) runtime_fail("framestep takes milliseconds (0 = real time)");
            runtime_frame_step_pin(ms);
        } else if (!strcmp(line, "threads\n")) {   /* every guest thread's wait state and guest return chain, now (a hang: what is each thread waiting for) */
            runtime_dump_threads();
        } else if (!strcmp(line, "close\n")) { runtime_front_request_close(); break; }
        else runtime_fail("invalid diagnostic input command");
    }
    fclose(f); return NULL;
}
__attribute__((constructor)) static void input_file_start(void) {
    const char *e = getenv("TOTK_INPUT_FILE"); if (!e || !*e) return;
    pthread_t t; if (pthread_create(&t, NULL, input_file, (void *)e)) runtime_fail("diagnostic input thread"); pthread_detach(t);
}
