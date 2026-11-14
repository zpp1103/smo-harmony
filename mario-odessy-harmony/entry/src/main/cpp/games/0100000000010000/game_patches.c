#include "game_patches.h"
#include "game_version.h"
#include "totk_ui_lang.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* Odyssey 1.3.0 / 1.4.1 (the same executables): the render size patches below (key 3). Each site is checked against the
   original instruction before it is written, as CONTRIBUTING.md asks. Key 4 writes camera fields at the present instead
   (odyssey_aspect.c). */
void odyssey_aspect_install(const Module *main_module);
void odyssey_aspect_present(void);

/* Docked render size (TOTK_RENDER_RES=<w>x<h>; unset: render-res.txt of the overlay folder, else 1600x900 as shipped).
   Three places hold the docked size (disassembly 2026-10-22): main+0x4bf2a4 puts 1600 | 900 << 32 into the configuration
   of the object made at main+0x624460, from which the window and its three 1600x900 swapchain textures take their size
   (main+0x4bf32c -> main+0x94175c -> nvnTextureBuilderSetSize2D); main+0x4bf34c passes 1600 x 900 and 1280 x 720 to
   main+0x624ad0; main+0x4e27bc gives the window crop (main+0x94dfe0: 0, 0, w, h) 1600 x 900 when docked, 1280 x 720
   otherwise. Patching the last two alone laid the UI out for 1920x1080 on 1600x900 textures: the right of the picture was
   cut off. All docked values are replaced; each instruction is checked before any is written. */
static unsigned g_render_w, g_render_h;
/* Without TOTK_RENDER_RES the size comes from the overlay folder: scripts/make_render_res_overlay.py writes render-res.txt
   next to the scaled ProjectGraphicsQuality.szs, so the code sizes below and the data table always agree (the app carries
   both in its romfs-patches folder, TOTK_ROMFS_PATCHES; 0: neither). */
static const char *render_res_from_overlay(char *buf, size_t n, const char **from) {
    const char *dir = getenv("TOTK_ROMFS_PATCHES");
    if (dir && !strcmp(dir, "0")) return NULL;
    static char path[4096];
    snprintf(path, sizeof path, "%s/render-res.txt", dir && *dir ? dir : "extracted/" TOTK_GAME_DIR "/romfs-patches");
    FILE *f = fopen(path, "r"); if (!f) return NULL;
    char *r = fgets(buf, (int)n, f); fclose(f);
    if (!r) return NULL;
    buf[strcspn(buf, "\r\n")] = 0; *from = path;
    return buf;
}
/* The docked render size asked for, read once (0 = as shipped): patch_render_res and the memory sizes below use it, and
   services.c asks for the memory before the main module is patched. */
static int render_size(unsigned *w, unsigned *h, const char **from) {
    static int done; static unsigned sw, sh; static const char *src = "TOTK_RENDER_RES";
    if (!done) {
        done = 1;
        const char *e = getenv("TOTK_RENDER_RES"); static char buf[32];
        if (!e) e = render_res_from_overlay(buf, sizeof buf, &src);
        if (e && *e && strcmp(e, "0") && strcmp(e, "1600x900")) {
            char tail;
            if (sscanf(e, "%ux%u%c", &sw, &sh, &tail) != 2 || sw < 640 || sh < 360 || sw > 3840 || sh > 2160)
                runtime_fail("TOTK_RENDER_RES must be <w>x<h> between 640x360 and 3840x2160");
        }
    }
    *w = sw; *h = sh; if (from) *from = src;
    return sw != 0;
}
/* Render targets come from a 100 MiB compressible pool (main+0x624b7c, inside a 170 MiB heap made at main+0x624b4c, both
   handed to main+0x983ab0). Its allocator (main+0x9a5440) returns NULL when no free block fits, and the frame buffer that
   wanted it then crashes reading +0x128 (main+0x9917f8). Measured on the title: a 3D scene of 2112x1188 fits, 2176x1224
   does not. Above 1920x1080 the pool grows with the scene area and the heap by the same amount; the application pool
   reported to the game grows with them (services.c). */
#define RT_POOL 0x6400000u
#define RT_HEAP 0xaa00000u
static uint32_t rt_pool_size(void) {
    unsigned w, h;
    if (!render_size(&w, &h, NULL) || (uint64_t)w * h <= 1920u * 1080u) return RT_POOL;
    /* 100 MiB holds a 2112x1188 scene and not 2176x1224, so the pool grows from that point with 8 % on top (2560x1440:
       160 MiB, which ran the Wooded Kingdom). Growing more costs memory elsewhere: with 256 MiB (+156) loading the Wooded
       Kingdom failed an allocation in another fixed heap and stopped with an SDK Break, also with the memory heap grown by
       the same amount; +60 left enough. */
    uint64_t pool = (uint64_t)RT_POOL * w * h * 108 / (2112u * 1188u * 100u);
    if (pool < RT_POOL) pool = RT_POOL;
    return (uint32_t)((pool + 0xffffff) & ~0xffffffULL);   /* 16 MiB steps; a movz with lsl 16 holds it */
}
uint64_t runtime_render_pool_extra(void) { return rt_pool_size() - RT_POOL; }
static void patch_rt_pool(Module *m) {
    uint32_t pool = rt_pool_size();
    if (pool == RT_POOL) return;
    uint32_t heap = RT_HEAP + (pool - RT_POOL);
    /* The game's whole memory heap is fixed too: nninitStartup asks for 0xc3000000 (main+0x4e321c, then
       nn::os::SetMemoryHeapSize), whatever the application pool is. Growing only the render target heap inside it left
       156 MiB less for the rest, and loading the Wooded Kingdom then failed an allocation (main+0x6af5dc) and stopped with
       an SDK Break; a bigger application pool alone did not change that. So it grows by the same amount, in the 2 MiB
       steps SetMemoryHeapSize takes. */
    uint32_t memheap = 0xc3000000u + ((pool - RT_POOL + 0x1fffffu) & ~0x1fffffu);
    const struct { uint32_t off, shipped, value; } site[] = {
        {0x624b4c, 0x52a15400, 0x52a00000 | (heap >> 16) << 5 | 0},      /* mov w0, #heap */
        {0x624b7c, 0x52a0c808, 0x52a00000 | (pool >> 16) << 5 | 8},      /* mov w8, #pool */
        {0x4e321c, 0x52b86000, 0x52a00000 | (memheap >> 16) << 5 | 0},   /* mov w0, #memory heap size */
    };
    for (size_t i = 0; i < 3; i++) {
        uint32_t word; memcpy(&word, m->base + site[i].off, 4);
        if (word != site[i].shipped) {
            fprintf(stderr, "[odyssey] render target pool main+%#x: found %08x expected %08x\n", site[i].off, word, site[i].shipped);
            runtime_fail("the render target pool size needs the instructions it replaces");
        }
    }
    for (size_t i = 0; i < 3; i++) memcpy(m->base + site[i].off, &site[i].value, 4);
    fprintf(stderr, "[odyssey] render target pool %u MiB (shipped 100), its heap %u MiB (shipped 170), memory heap %#x (shipped 0xc3000000)\n",
            pool >> 20, heap >> 20, memheap);
}
static void patch_render_res(Module *m) {
    const char *from; unsigned w, h;
    if (!render_size(&w, &h, &from)) return;
    enum { MOVZ_W, MOVZ_X, MOVK_X32 };
    static const struct { uint32_t off; unsigned rd, shipped, kind; int is_w; } site[] = {
        {0x4bf2a4, 9, 1600, MOVZ_X, 1}, {0x4bf2a8, 9, 900, MOVK_X32, 0},   // configuration: docked width | height << 32
        {0x4bf34c, 4, 1600, MOVZ_W, 1}, {0x4bf350, 5, 900, MOVZ_W, 0},     // main+0x624ad0: docked width, height
        {0x4e27bc, 3, 1600, MOVZ_W, 1}, {0x4e27c0, 4, 900, MOVZ_W, 0},     // window crop when docked
    };
    #define ENC(kind, rd, imm) ((kind) == MOVZ_W ? 0x52800000u : (kind) == MOVZ_X ? 0xd2800000u : 0xf2c00000u) | ((uint32_t)(imm) << 5) | (rd)
    for (size_t i = 0; i < sizeof site / sizeof *site; i++) {
        uint32_t word, want = ENC(site[i].kind, site[i].rd, site[i].shipped); memcpy(&word, m->base + site[i].off, 4);
        if (word != want) {
            fprintf(stderr, "[odyssey] render size main+%#x: found %08x expected %08x\n", site[i].off, word, want);
            runtime_fail("TOTK_RENDER_RES needs the instructions it replaces");
        }
    }
    /* The 3D scene is not set here: the game's dynamic resolution takes its largest docked size (1600x900) and its steps
       from SystemData/ProjectGraphicsQuality.szs, which scripts/make_render_res_overlay.py scales into a RomFS overlay.
       Setting the scene size in code instead (main+0x5efbb0 fixed, or main+0x5efc20 taking the requested side) drew light
       in blocks on the ground, because buffers sized for the largest step stayed 1600x900. */
    for (size_t i = 0; i < sizeof site / sizeof *site; i++) {
        uint32_t word = ENC(site[i].kind, site[i].rd, site[i].is_w ? w : h);
        memcpy(m->base + site[i].off, &word, 4);
    }
    #undef ENC
    g_render_w = w; g_render_h = h;
    fprintf(stderr, "[odyssey] docked render size %ux%u (shipped: 1600x900; from %s)\n", w, h, from);
}

/* Key 3 at a bigger render size: the 3D scene back to 1600x900 while playing, without a restart. The game's dynamic
   resolution picks the scene size each frame at main+0x5efc20 (called once, from main+0x6f144c): the smaller of the
   requested size (x2) and the limit (x0), written to x1, width then height. That leaf function is replaced by a call into
   odyssey_scene_pick, which does the same and then holds the result under a cap. A smaller scene inside the buffers
   sized for the largest step is how the game's own lower steps draw, so it stays clean. The window and the HUD stay at
   the patched size. */
static _Atomic uint32_t g_scene_cap, g_scene_pick;   /* width | height << 16; cap 0 = none */
/* Key 3's states: original 1600x900, the same with MetalFX, then native 1920x1080 and, at a bigger render size, native at
   that size. The game starts as shipped (3D 1600x900); window and HUD keep the patched size in every state, because they
   are only set when the game starts. */
enum { SCENE_ORIGINAL, SCENE_ORIGINAL_MFX, SCENE_1080, SCENE_MAX };
static int g_scene_mode = SCENE_ORIGINAL;
static int scene_states(void) { return g_render_w * g_render_h > 1920u * 1080u ? 4 : 3; }   /* SCENE_1080 is the max at 1080p */
static uint32_t scene_size(int mode) {
    return mode == SCENE_1080 && scene_states() == 4 ? 1920u | 1080u << 16 : mode >= SCENE_1080 ? g_render_w | g_render_h << 16 : 1600u | 900u << 16;
}
/* The shortcut box's key 3 line (frontend.m); NULL without a bigger render size (key 3 is then MetalFX on / off). */
const char *runtime_picture_name(void) {
    if (!g_render_w) return NULL;
    static char native[64];
    uint32_t s = scene_size(g_scene_mode);
    switch (g_scene_mode) {
    case SCENE_ORIGINAL: return TOTK_UI("原版 1600×900", "original 1600×900");
    case SCENE_ORIGINAL_MFX: return TOTK_UI("1600×900，MetalFX 放大到窗口", "1600×900, MetalFX to the window");
    default: snprintf(native, sizeof native, TOTK_UI("原生 %u×%u", "native %u×%u"), s & 0xffff, s >> 16); return native;
    }
}
static void odyssey_scene_pick(const int32_t *limit, int32_t *out, const int32_t *want) {
    int32_t w = want[0] < limit[0] ? want[0] : limit[0], h = want[1] < limit[1] ? want[1] : limit[1];
    uint32_t cap = atomic_load_explicit(&g_scene_cap, memory_order_relaxed);
    if (cap) { if (w > (int32_t)(cap & 0xffff)) w = (int32_t)(cap & 0xffff); if (h > (int32_t)(cap >> 16)) h = (int32_t)(cap >> 16); }
    out[0] = w; out[1] = h;
    atomic_store_explicit(&g_scene_pick, (uint32_t)w | (uint32_t)h << 16, memory_order_relaxed);
}
static void patch_scene_pick(Module *m) {
    static const uint32_t shipped[] = {   /* ldr w8,[x2]; ldr w9,[x0]; cmp; csel lt; str w8,[x1]; the same at +4; ret */
        0xb9400048, 0xb9400009, 0x6b09011f, 0x1a89b108, 0xb9000028, 0xb9400448, 0xb9400409, 0x6b09011f, 0x1a89b108, 0xb9000428, 0xd65f03c0};
    for (size_t i = 0; i < sizeof shipped / sizeof *shipped; i++) {
        uint32_t word; memcpy(&word, m->base + 0x5efc20 + 4 * i, 4);
        if (word != shipped[i]) {
            fprintf(stderr, "[odyssey] scene size pick main+%#zx: found %08x expected %08x\n", (size_t)(0x5efc20 + 4 * i), word, shipped[i]);
            runtime_fail("key 3 scene size needs the instructions it replaces");
        }
    }
    uint64_t target = (uint64_t)(uintptr_t)runtime_hle_wrap_named((void *)odyssey_scene_pick, "odyssey_scene_pick");
    const uint32_t veneer[2] = {0x58000051, 0xd61f0220};   /* ldr x17, #8; br x17 (the 8-byte target follows) */
    memcpy(m->base + 0x5efc20, veneer, sizeof veneer);
    memcpy(m->base + 0x5efc28, &target, sizeof target);
    /* SCENE_ORIGINAL until key 3; a render size picked in the settings window (the launcher sets ODYSSEY_SCENE_START=max,
       after PR #910) starts at that size instead */
    const char *start = getenv("ODYSSEY_SCENE_START");
    if (start && !strcmp(start, "max")) { g_scene_mode = scene_states() - 1; atomic_store(&g_scene_cap, 0); }
    else atomic_store(&g_scene_cap, 1600u | 900u << 16);
}
/* Present thread: one line when the picked scene size changes (the first 40 changes, then every 500th), so a test log says
   which size the game draws after key 3. */
static void scene_log(void) {
    static uint32_t last; static unsigned changes;
    uint32_t now = atomic_load_explicit(&g_scene_pick, memory_order_relaxed);
    if (!now || now == last) return;
    last = now; changes++;
    if (changes <= 40 || changes % 500 == 0)
        fprintf(stderr, "[odyssey] 3D scene %ux%u (change %u)\n", now & 0xffff, now >> 16, changes);
}

/* ===== round9 E-gates：纯观测门（零行为改动）=====
 * E1 main+0x57f950（派发/舞台迭代函数入口——emu8 已证活锁主线程每周期走它：0x1D 链
 *   sdk+0xd3f174←main+0x8af3b8←main+0x57f98c←…，0x57f98c 就在本函数体内）。
 *   x0=容器；[x0+8]→{u32 count@+0, el* arr@+8}；逐元素 this=[el+0x10]，游戏随后对
 *   this+0x58 发 SignalProcessWideKey。门在入口把 {el,this} 快照进静态缓冲——多读的
 *   地址恰是被换出的第 4 条指令（ldr x8,[x0,#8]）之后游戏自己立刻要读的同一批地址
 *   （count/arr/el/[el+0x10]，见 0x57f95c-0x57f97c 反汇编），不新增任何解引用面。
 *   dump 时打表与 threads.c 的 pkey/pkey2 发 key 族对照，验证两族是否同一数组算出。
 * E2a main+0x8aef34、E2b main+0x8af418（两个 stage 排空方法，收 MQ@+0x168 完成包）：
 *   命中计数 + 末次 x0。pkey2 发 key 族 wait(0x1C)=0 + 本门 0 命中 ⇒ 该池消费者线程
 *   从未运行；与 threads.c 的 CreateThread/StartThread 账本（svc 0x08/0x09）对照区分
 *   「从未创建」与「创建失败」。
 * 机制：入口 16 字节换成 veneer（ldr x17,#8; br x17; .quad stub），stub 落在 main 镜像
 *   尾部保留页 [page_up(file_size), +0x4000)（本移植 frame-step 空置；loader.c "Main's
 *   extra private page … stays plain zero-filled memory" 即此页）。stub 保存
 *   x0-x7/x29/x30 → 调 C 观察器（无锁、无 I/O、无 libc 调用）→ 恢复 → 原样重放被换的
 *   4 条出厂指令 → b 回入口+0x10。编码字全部经 scripts_round9b/gen_obs_stub.py 的
 *   capstone 往返校验；安装时先逐一比对 4 条出厂指令字，不符即不装该门（fail-soft）。
 * round13 kick 断链观测门（fixPlan(2)：『kick 从未运行』从推断升级为指令级计数）：
 * KICK1 main+0x8ad1f4（kick 入口+4：donor 语义里带优先队列入队后再 signal 的派发函数；
 *   被 al thunk 0x587ea0 尾调与主线程排空循环 0x8abfa0 bl 调用）。计数=该函数全部真实
 *   执行次数——与 donor 侧 0x8ad1f0 族 force/entry 计数（预期 ≈920+410+舞台数，与
 *   wait#921/920/410 对账）对照：emu ≈0 ⇒ kick 从未运行（断链在注册上游），emu 大而
 *   worker 不醒 ⇒ kick 运行但其虚调用路径分歧。
 * ALKICK main+0x587ea8（al 层 kick 调用点：thunk 0x587ea0 过 NULL 检查后的尾调 b）。
 *   该址仅一条无条件 b、且控制流不再返回（thunk 后继 0x587eac ret 只经 0x587ea4 cbz
 *   到达），故走单字门（tail=1）：出厂 b→stub 计数→stub 尾 b 回原目标 0x8ad1f0，
 *   无重放、不碰相邻 thunk（0x587eb0 起是另一个活 thunk）。KICK1−ALKICK=直接调用路径。
 * KICK3 main+0x8ad318（kick 三参变体 0x8ad310 族的合流点：0x8ad310 and;0x8ad314 mov
 *   顺流至此，0x8a671c cbz 也跳此——全镜像无其它落点，扫描实证）。同为单字门。
 * round15 定向 watch（末三注册分析建议①）：kick 三门 13c/14c 全程 hits=0 后，区分主线程
 * 排空路径「循环没跑」vs「跑了但链表恒空（kick 分支不满足）」——
 * DRAIN-ENTER main+0x8abf70（主线程排空函数入口，其循环体 0x8abf94 的 bl 0x8ad1f0 即 kick 直接
 *   调用源之二；str/stp/stp/add 四寄存器字，全镜像扫描无窗内落点）。与 KICK1 联读三分支：
 *   ENTER=0 → 排空函数从未被调（两个调用源均未到达，上游断链实锤）；
 *   ENTER>0 ∧ KICK1=0 → 每次进排空都走 b.eq 空表捷径（0x8abf84 cmp x0,[x0+8]）＝链表恒空＝
 *   E2b 完成包从未入队的指令级证据；ENTER>0 ∧ KICK1>0 → kick 已被派发（破局）。
 *   非空前置块 0x8abf8c 不可门：循环回边 0x8abfac b.ne 落 0x8abf94＝窗内第 3 字（全镜像扫描
 *   实证），veneer 必炸；bl 本身含 PC 相对字亦不可重放。al thunk 0x587ea0 入口同理不可门
 *   （第 2/3 字=cbz/b 出窗控制流），ALKICK(0x587ea8) 继续承担该路径唯一计数——记录在案。
 * round16b 排空循环块门（DRAIN-LOOP）：KICK1−ALKICK 只剩「排空循环直接调用」无指令级计数，
 *   循环头 0x8abf94（bl 0x8ad1f0@0x8abfa0 所在基本块入口＝b.ne 回边落点）装四字门。窗口第 4
 *   字就是 bl——本轮 smo_obs_replay_fix 增加 bl 重定位（同绝对目标重算 imm26；返回地址语义
 *   原样：重放 bl 的 lr=下一重放字＝w[23] b 回 0x8abfa4，kick 的 br x1 尾调亦回到 w[23]）。
 *   窗内落点全镜像扫描：0x8abf98/9c/a0 零命中（b/cbz/tbz/adrp+add 全族），仅回边落窗口首字。
 *   首见快照 kick 三条件值（0x8ad200 cmp w1,#1 / 0x8ad208 ldrb [x19,#0xf9] / 0x8ad278
 *   ldr x0,[x19,#0x20]，x19=kick this=x8-0x1c0）：循环头处 w1 恒=w19（首进=参数、回边=上一轮
 *   0x8abf9c 所写），x8=当前节点。观察器经新增 stub 参数取到（见下方 prologue 注）。
 * ALKICK 覆盖确认（round16b 复核，指令级）：0x587ea0 ldr x0,[x0]; 0x587ea4 cbz x0,0x587eac;
 *   0x587ea8 b 0x8ad1f0; 0x587eac ret——ALKICK 装在 0x587ea8＝过 NULL 检查后的尾调 b，计的是
 *   「真调用」而非 thunk 入口（NULL 路径→ret 不计）。入口装门实测不可行：四字窗含 cbz
 *   （imm19 ±1MiB），stub 页 0x14d4xxx 距 0x587eac 达 0xF4C254≈15.7MiB，cbz 无法重定位——
 *   ALKICK 仍是该路径唯一可计数点，结论与 round15 记录一致。
 * round17 三门（断链第一未满足条件已定位在游戏侧派发环节——round16 kick1=alkick=216 全走
 * thunk、drain-enter=drain-loop=0、E4 confluence=1/e1-binding=0，宿主同步语义无一处失败）：
 * BC310 main+0x954310（donor 侧「广播调用点」：donor_round2_thr.log:1436-1441 的
 *   CreateThread→StartThread→svc0x1d×2→svc0x1b 序列现场 x3 恒=0x10c954310＝main 基址
 *   0x10c000000（:12）+0x954310＝该函数入口值——全镜像扫描无任何直接分支落此入口（0x954310
 *   no-landing），证明它只经函数指针被调（与 donor 现场 x3 携带入口值互证）。四字窗
 *   sub sp/str x25/stp x24,x23/stp x22,x21 全寄存器字，窗内 0x954314/18/1c 零落点。
 *   emu 命中>0 ⇒ emu main 也执行了这条 boot 广播链；首/末 x0 直接回答「InitSystem 收件箱
 *   boot 窗口内到底有没有被 kick」（上轮 pkey 饱和使 0 行非结论性）。
 * KICKVT main+0x8ad214（kick 虚调尾 0x8ad228 br x1 的取数点：x1=[[this]+0x20]，窗四字
 *   ldr x8,[x19]/ldr x1,[x8,#0x20]/mov x0,x19/ldp x29,x30 全寄存器字；b.lt(0x8ad204) 与
 *   cbz(0x8ad238) 均落窗口首字，0x8ad218/21c/220 全镜像零落点）。观察器读 vt=[x0]、
 *   tgt=[vt+0x20]——与游戏 0x8ad214/0x8ad218 下一拍要读的同一批地址（无新解引用面），
 *   8 槽去重计数出目标分布：donor 的 kick 虚路径入数对账定位分叉点（emu tgt 若指向
 *   HLE thunk 表/错误槽即虚路径分叉实锤）。
 * 门容量：尾部页 0x4000，12 门×104B+0x100 头部=1348B，余量充足。
 * round18 三项升级（只观测零行为改动；round17 首跑 kick1/kickvt hits=0、bc310 hits=45807）：
 * ① KICK1（0x8ad1f4）三条件快照扩容——round17 只存首见+末见+三分支交叉，本轮按规格加
 *   首 16 次逐次快照环 {this,w1,f9,o20}（claim-slot 原子占位，多线程 kick 并发安全）+
 *   六个边际计数（w1==0/!=0、f9==2/!=2、o20==0/!=0——与三分支交叉互补：边际不受交叉
 *   条件限制，f9/o20 的分布在 w1<1 直虚调路径上也可见）。
 * ② BC310（0x954310）首见快照补 x1：观察器参4（x3）=门点原 x1（prologue w8 在 movz
 *   覆盖 x1 前抄走）——该函数第二实参（0x95433c cmp x1,#8 消费它），首见 {x0,x1,tick}。
 * ③ KICKBR main+0x8ad228（br x1 本尊，单字尾门）：出厂字 0xd61f0020=br x1（寄存器
 *   分支，非 PC 相对 b），0x8ad224 ldr x19,[sp],#0x20 已弹栈复原 x19、x1=[[this]+0x20]
 *   即跳转目标（0x8ad218 所载，窗内无写）。veneer 换 br→stub：prologue 抄 x1→x3
 *   （w8）后观察器零解引用拿真目标，32 槽去重计数；stub 尾原义重放 br x1（prologue
 *   已复原 x1，callee 入口caller-saved 全部死寄存器——与 ALKICK 尾跳另一函数入口同
 *   性质）。0x8ad22c（b.ne 0x8ad210 落点）不动。与 KICKVT（0x8ad214 取数点，读
 *   [vt+0x20] 推导）互证：KICKBR 记的是寄存器真值，KICKVT 记的是内存推导值。
 * round19 四门+一环（只观测零行为改动；round18 首跑 kickbr=0、thunk 层无计数）：
 * ①② al thunk 双门×2（thunk1=0x587ea0 两参 kick 蹦床：ldr x0,[x0]; cbz x0,0x587eac;
 *   b 0x8ad1f0；thunk2=0x587eb0 三参变体蹦床：ldr x0,[x0]; cbz x0,0x587ec4; mov w2,w1;
 *   orr w1,wzr,#0; b 0x8ad310——字均实测自 main.patched.bin）。thunk 入口本体四字窗
 *   含 cbz/b 出窗控制流不可门
 *   （round15 已证），但全镜像扫描（round19 实跑复核）：跳 0x587ea0 的唯一分支=b@
 *   0x587da4、跳 0x587eb0 的唯一分支=b@0x587db4，且两门址自身零落点——入口门装在
 *   这条唯一进入分支上（单字尾门换 b→stub 记全部进入、stub 尾 b 回原目标），计数
 *   语义=thunk 全部进入。NULL 出口门装在 cbz 早退落点 0x587eac/0x587ec4（出厂字
 *   ret=0xd65f03c0，寄存器分支重放变体——stub 尾原义重放 ret，x30 由 prologue
 *   保存/复原，跳回调用者；hits 即 [x0]==NULL 早退计数，cbz 只在 NULL 时走此）。
 *   联读：THUNK1_IN−ALKICK(0x587ea8 真调用)≈THUNK1_NULL 自洽校验；THUNK2_IN 是
 *   KICK3(0x8ad310 族) 路径的 thunk 层计数，THUNK2_NULL=三参路径 NULL 早退。
 * ③ BC310（0x954310）加 first-16 唯一 x0 环：g_bc 16 槽 CAS 占位去重（g_br 范式）
 *   枚举首 16 个唯一收件对象+各计数，x0==0/溢出各计数——45807 hits 的对象分布一屏可读。
 * 门容量：尾部页 0x4000，16 门×104B+0x100 头部=1764B，余量充足。
 * round20 最小扰动+注册三门（只观测零行为改动；测注册时序假说：round16 注册发生过、
 *   后续 boot 注册不再发生若是宿主插桩扰动所致即为最简约解释——本轮先剥扰动再定时序）：
 * 【编译关闭的重探针】SMO_OBS_HEAVY=0（game_patches.h，threads.c 共用）：
 *   ① KICK1 门挂的 kick 三条件快照（观察器内 [x0+0xf9]/[x0+0x20] 双读+三分支交叉+
 *      首 16 环+六边际——本轮 KICK1 退化为纯计数门 hits/last_x0）；
 *   ② BC310 first-16 唯一 x0 环（CAS 去重循环编译掉；保留首见 {x0,x1}+hits+xzero）；
 *   ③ threads.c early1d_note（boot 前 10s 持全局锁 G 的 0x1D 键全录，锁内 256 槽线性扫）。
 *   保留：kick1/thunk1 双门（thunk2 双门 0 命中零成本同留）/E1/E2/pkey+ct 账本。
 * 【注册三门】0x8ae230 注册函数（字实测自 main.patched.bin：str x19,[sp,#-0x20]!;
 *   stp x29,x30,[sp,#0x10]; add x29,sp,#0x10; mov x19,x0; str x1,[x0]@+0x10——把句柄 x1
 *   写进容器槽 *x0，再 bl 0x8ad1c0(条件性 bl 0x8ad1d0)，末把 this 写回 [[x19]+0x18]；
 *   三个 bl 调用点 0x8b6688/0x8b6708/0x8b6990 均以 x0=栈槽 sp+8、x1=节点-0x1c0 调它）：
 *   REG230=入口四字门（全寄存器/立即数字，全镜像扫描窗内仅首字有落点=3 个 bl），
 *   hits=注册函数全部真实执行；首见 {x0=槽, x1=被注册句柄, tick}=注册时序主证据。
 *   STR240 预验证不可装：0x8ae240 四字窗第 4 字 0x8ae24c tbz(imm14 ±32KiB，stub 页距
 *   ~12-21MiB) 无法重定位；邻近安全位 0x8ae23c 又与 REG230 的 16B veneer（覆盖
 *   0x8ae230..0x8ae23f）重叠。改装 REG258=0x8ae258 完成块四字门（epilogue：ldr x8,[x19];
 *   str x19,[x8,#0x18]; ldp; ldr x19,[sp],#0x20——全寄存器字，tbz 0x8ae24c 落点=窗首字）：
 *   hits(258)=注册体跑到尾声的次数（str x1,[x0] 与回指针写都在其上游线性路径上）。
 *   联读：hits(230)==hits(258) ⇔ 每次进入都完整注册；230>258=中途异常。全镜像扫描
 *   0x8ae23c/240/244 零直接落点 ⇒ str 执行 ⇔ 230 门命中+尾声到达，无需在 240 重复装门。
 * 【mq 版本比较 walker】0x964680（cbz x1→ret0；[x0+0xd4]∈[3,5]→ret0；[x0+0xc8]==x1
 *   →ret0；不等→x0+=0x38 尾跳 0x9644b0 走下一节点。全镜像零直接落点=仅指针调用）。
 *   本体四字窗首字 cbz(imm19 ±1MiB) 不可门，且 684/688/68c/690/694/698 各窗均含
 *   b.cond/cbz——拆双尾门合成环头计数：MQ_RET=0x9646a4（ret 重放变体，计全部终结
 *   路径：x1==0/版本越界/匹配命中，x0/x1 未写=原节点+版本字）；MQ_ADV=0x9646ac
 *   （b 0x9644b0 重放，计 [x0+0xc8]!=x1 的链前进，x0 已 +0x38=后继节点）。四路径穷尽
 *   （cbz→6a0、b.lo→6a0、匹配→6a0、不等→6a8），hits(RET)+hits(ADV)=0x964680 全部进入。
 *   消费侧时序证据：RET/ADV 首见 tick vs REG230 首见 tick——注册先于/后于 walker 首跑。
 * 门容量：尾部页 0x4000，20 门×104B+0x100 头部=2208B，余量充足。
 * round21 注册调用者两门 + BC310 调用者 LR 捕获（只观测零行为改动）：
 * 【三条 bl 只对应两个函数】round20 记录的注册函数 0x8ae230 三个 bl 调用点
 *   0x8b6688/0x8b6708/0x8b6990 反汇编上溯 prologue 实证分属两个函数（非三个）：
 *   FN_A=0x8b65e0（sub sp,sp,#0x60 + 5 对 stp，epilogue 0x8b6764-0x8b6780 ret）：0x8b6688
 *     与 0x8b6708 是同一函数内两个独立链表循环（循环头 0x8b6678/0x8b66f8，均
 *     ldr next,[x8,#8]→sub x1,x8,#0x1c0→REG230 注册→查 [x8+0x108]==w21→0x8ae1e0 注销）。
 *     函数体 0x8b6608 取时基、0x8b660c 读 [x1+0x14] 版本 cmp #4 后 0x8b6630-0x8b663c
 *     跳转表 br x8 派发 5 臂——即「按容器版本分派的定时等待/扫描」族；0x8b6750 还有
 *     kick(0x8ad1f0) 直调（找到匹配节点且时差小于阈值时）。
 *   FN_B=0x8b6920（stp x28,x27,[sp,#-0x60]! 起，0x8b6918/0x8b691c udf 填充隔断）：
 *     0x8b6990 所在循环遍历 [x0+0x48] 数组（count=[x0+0x40]，stride 0x78，
 *     0x8b6970 madd），每桶走 [x27+8] 链表逐节点 REG230 注册/匹配/注销——与
 *     0x8b5370 finalize 的外层走法（count@+0x40/arr@+0x48/stride 0x78，0x8b539c-0x8b53bc）
 *     完全同一容器布局 ⇒ FN_B 是该容器的全桶扫描成员（finalize 的同族兄弟）。
 *   族对照结论（对照 0x8b5370 finalize / 0x8cbae0 teardown）：FN_B 与 finalize 同容器
 *   同走法（0x78 步进数组+链表）；FN_A/FN_B 都成对调 REG230(0x8ae230)/0x8ae1e0（注册/
 *   注销对），FN_A 另有 kick 直调；0x8cbae0 是 MQ teardown-post 兄弟（共享 0x8aec50
 *   MQ-alloc 与 0x8af410 族、自发自收 0x3d 消息后清 +0x230/+0x238），无链表走法——
 *   与 FN_A/FN_B 不同支。两门 hits+首见 {x0,x1}（g_fs 槽 1/2）：FN_A 首见 x0=时基参、
 *   x1=版本容器（[x1+0x14]∈[0,4]）；FN_B 首见 x0=容器本体、x1=第二实参。
 *   全镜像落点扫描（scripts_round21/safety.py）：两窗内 +4/+8/+c 零落点；FN_A 入口
 *   唯一落点=0x8b5e3c 尾调 b、FN_B 入口唯一落点=0x8b6bb0 尾调 b（各自全镜像仅此一个
 *   直接分支落点，其余进入路径必经函数指针——与 REG230 三 bl 落点互证闭环）。
 * 【BC310 调用者 LR 捕获】0x954310 外层循环静态定位穷尽（全镜像无直接分支落点=round17
 *   已证；adrp+add 材料化=0（findref.py）；唯一 reloc 装载=表 0x1352aa0 槽+0x60（relatab.py），
 *   该表基址唯一代码引用 0x95380c 只读槽 0-2（字符串/数据，非 +0x60）；表地址无任何
 *   reloc 装入对象模板；slot {0x20,0x30,0x40,0x60} 泛型虚派发点 261 个无法唯一化）⇒ 调用
 *   者只能运行时取：stub prologue 增参5=门点 LR（w10 ldr x4,[sp,#8]，x30 在 w0 已入栈，
 *   原 x4 在 w3 已保存、w16 复原），BC310 门记 first/last LR + 首 64 采样环（fetch_add
 *   认领槽位、无 CAS——round20 已裁决 45807×CAS 是 boot 期扰动源，采样环每命中仅 1 次
 *   原子加，首 64 次各一次定长写）+ 末见 {x1,tick} 逐命中覆盖（第 45807 次现场=循环耗尽
 *   时的末见快照）。blr 调用⇒LR=调用点+4；br 尾跳⇒LR=更上层返回地址——采样环分布可直接
 *   分辨。外层循环头/退出分支门待 LR 定位调用者后下轮装（本轮如实记录为未装）。
 * 【门集合收敛】退役 10 门（问题均已闭环）：E1/E2a/E2b/CELL2/DRAIN_ENTER/DRAIN_LOOP/
 *   KICKVT/KICKBR/MQ_RET/MQ_ADV——E1 观察器 26k/s 洪流每次做全容器快照，是现存最大
 *   每命中成本，退役即减扰动。保留：kick 门三（KICK1/ALKICK/KICK3，轻量计数）、
 *   BC310（本轮升级 LR 捕获）、thunk 双门×2、REG230/REG258。门 id 沿用旧值（退役槽
 *   留空），dump 侧硬编码 id 不变；18/19 由 MQ_RET/MQ_ADV 改派 FN_A/FN_B。
 * 门容量：尾部页 0x4000，12 门×108B（27 字 stub）+0x100 头部=1396B，余量充足。 */
enum { SMO_OBS_KICK1 = 4, SMO_OBS_ALKICK = 5, SMO_OBS_KICK3 = 6,       /* kick 门（轻量计数） */
       SMO_OBS_BC310 = 9,                                              /* 广播调用点（round21 增 LR 捕获） */
       SMO_OBS_THUNK1_IN = 12, SMO_OBS_THUNK1_NULL = 13,               /* thunk 双门（round21 保留） */
       SMO_OBS_THUNK2_IN = 14, SMO_OBS_THUNK2_NULL = 15,
       SMO_OBS_REG230 = 16, SMO_OBS_REG258 = 17,                       /* reg 门（round21 保留） */
       SMO_OBS_FN_A = 18, SMO_OBS_FN_B = 19,                           /* round21：注册调用者两门 */
       SMO_OBS_TP1 = 20, SMO_OBS_TP2 = 21,                             /* round24：解引用首字探针（入口门） */
       SMO_OBS_TP4R = 22, SMO_OBS_TP4A = 23,                           /* round24：0x964680 出口双门（入口穷尽） */
       SMO_OBS_SENDTO_REJ = 24, SMO_OBS_BRC = 25,                      /* M7w：sendTo 拒绝合流门 + 广播器身份门 */
       SMO_OBS_M7A_NULL = 26, SMO_OBS_M7A_REG = 27,                    /* M7a: fnB-entry result NULL tally + registry vector latch */
       SMO_OBS_M7B_BR = 28,                                            /* M7b: poster main+0x7aca60 entry (LR ring + first/last) */
       SMO_OBS_M7B_C0 = 29,                                            /* M7b: 13 poster call sites = ids 29..41 (tail=3 bl replay) */
       SMO_OBS_M7B_GTRUE = 42, SMO_OBS_M7B_GFALSE = 43,                /* M7b: fnA TLS-gate TRUE 0x61570c / FALSE 0x6157f0 */
       SMO_OBS_M7C_FNA = 44,                                           /* M7c: fnA 0x6156b0 ENTRY (task-verified window) */
       SMO_OBS_M7E_CRT = 45,                                           /* M7e: create-request 0x746020 ENTRY (creator layer) */
       SMO_OBS_M7G_CRT = 46, SMO_OBS_M7G_A = 47,                       /* M7g: creator fn head 0x7488f0 + entry-A block 0x7485a8 */
       SMO_OBS_M7G_B = 48, SMO_OBS_M7G_SIB = 49,                       /* M7g: entry-B block 0x748fb0 + sibling ctor 0x7461a0 */
       /* M7h（2026-11-08）：批循环的停止方式 + 0x748e80 的 12 个调用者。10 只新门、新号段 50..59，
          fnA 门 44 / 创建者门 45 / smo_m7z 门 46..49 的站表与门号一字未动（跨轮可比性）。 */
       SMO_OBS_M7H_BQ = 50,                                            /* 0x748e80 查表函数头（12 静态调用者按 LR 归类） */
       SMO_OBS_M7H_ENT = 51,                                           /* 0x4be0d0 批循环函数头（2 静态调用者，LR 仍原义） */
       SMO_OBS_M7H_TASKS = 52,                                         /* 0x4be1cc 任务数>0 才到（b.le 落空侧） */
       SMO_OBS_M7H_L2E = 53,                                           /* 0x4be284 loop2 前导（x4==0 模式入口） */
       SMO_OBS_M7H_HEAD = 54,                                          /* 0x4be294 loop2 head（唯一入口 b.lt@0x4be308） */
       SMO_OBS_M7H_LOOK = 55,                                          /* 0x4be2ec 即将发起查表（bl 前一字） */
       SMO_OBS_M7H_RET = 56,                                           /* 0x4be2f4 查表返回 + 取消检查已到达 */
       SMO_OBS_M7H_CONT = 57,                                          /* 0x4be2fc cbnz 落空 = 取消未置位、继续 */
       SMO_OBS_M7H_EXH = 58,                                           /* 0x4be30c b.lt 落空 = 耗尽（与另两条出口合流） */
       SMO_OBS_M7H_EXH1 = 59,                                          /* 0x4be260 loop1 耗尽（从 EXH 里减掉） */
       /* M7L（2026-11-08）：派批泵 main+0x4bd8a0 —— 谁（间接地）调它、它自己的循环走完没有、
          两个派批点 t≈144 s 之后还有没有人进。新号段 60..64；50..59 一字未动（跨轮可比性）。 */
       SMO_OBS_M7L_PUMP = 60,                                          /* 0x4bd8a0 泵函数头：LR = 间接调用者返址（下一层命名） */
       SMO_OBS_M7L_DP1 = 61,                                           /* 0x4bdb98 派批点① 前一字（bl@0x4bdb9c 不碰） */
       SMO_OBS_M7L_DP2 = 62,                                           /* 0x4bdbcc 派批点② 前一字（bl@0x4bdbd0 不碰） */
       SMO_OBS_M7L_PDONE = 63,                                         /* 0x4bdb7c 泵循环耗尽落空 = 置「已做完」标志 */
       SMO_OBS_M7L_PEXIT = 64,                                         /* 0x4bdbd4 泵公共出口 */
       /* M7N（2026-11-08）：谁 post 了谁。新号段 65..66；50..64 一字未动（跨轮可比性）。
          G1=worker 工厂 main+0x964260 入口，G2=PostToken main+0x9646d0 入口。
          两只都是 tail=2「普通字单字门」：只换入口那 1 个字（f81c0ff7 / b940d408 原义
          stub 内重放），**函数体内没有任何一条 bl/blr 被覆盖或移动**（G1 窗内第一条 bl 在
          +0x1c，G2 全身只有 b 尾跳；运行期 [m7n] code 行逐字对比证明）。 */
       SMO_OBS_M7N_G1 = 65,                                            /* 0x964260 工厂入口（ctx 指针 + ctx->0x88 编号 + LR） */
       SMO_OBS_M7N_G2 = 66,                                            /* 0x9646d0 PostToken 入口（worker 对象 + token + state + LR） */
       /* M7P（2026-11-08）：**一只门、两边跑**。65/66 一字未动；新号 67。
          Start = worker 线程公共启动口 main+0x963e50 —— M7O 静态扫出的 15 个 ctor 站点
          （0x458314 0x4588f4 0x5b8910 0x5b8a20 0x5b8b24 0x6821dc 0x6ae2c4 0x6ca358
           0x6cc268 0x7ac958 0x7aed38 0x93f9e8 0x941b88 0x97dd84 0xa0ce30）全部汇于此。
          门记 LR（⇒ 站点 = LR−4）+ x0（worker 对象）+ epoch（[x1]）+ tick。 */
       SMO_OBS_M7P_START = 67,                                         /* 0x963e50 worker Start（LR/x0/epoch/tick） */
               /* M7Q（2026-11-08）：why the device never reached the 5 worker-ctor sites.
           新号段 68..70；65/66/67 三门号与对应站表行一字未动（跨轮可比性）。
           WRAP = 包装 ctor 入口 main+0x964540：静态 16 条 bl 调用者的公共上层，其中 12 条与
             15 个 Start 站点同函数成对；3 个 Start 站点（0x93f9e8/0x97dd84/0xa0ce30）的外层
             函数走 0x963ac0 直调、4 条 bl（0x615fe4/0x6cdb7c/0x6ced2c/0x7acd18）只建不启。
           FO   = main+0x65b468 mov w1,w23：3-lane 建造前的扇出数入口。**只读**：该字之后的
             0x65b46c 与 0x65b470 一个字节都不碰；w23 是 callee-saved、stub 看不到，故扇出数的
             真值在 FOB 门（被调者入口寄存器）取。
           FOB  = main+0x65b470 bl 0x5b8710：真正造 3 条 lane 的调用本身。tail=3 单 bl 门
             （M7b 机制）：只换 bl 这一字，stub 重放重定位后的 bl ⇒ 被调者入口寄存器逐字原样，
             x1 = w23 = 扇出数 w1 当场可读；被调者 LR 只是返回地址（stub+0x54 → b 回 0x65b474），
             不落任何既有门的 LR 口径（0x5b8710 体内 3 个 Start 站点 LR = 0x5b8914/24/28）。 */
        SMO_OBS_M7Q_WRAP = 68,                                          /* 0x964540 包装 ctor 入口（LR/x0/x1/x8/tick） */
        SMO_OBS_M7Q_FO = 69,                                            /* 0x65b468 扇出数入口（x1=w23=w1） */
        SMO_OBS_M7Q_FOB = 70,                                           /* 0x65b470 bl 0x5b8710（造 3 条 lane） */
        /* M7S2（2026-11-08）：把「池 B 构造总入口为什么没被调用」定死。新号段 71..73；
           65/66/67/68/69/70 六个门号与对应站表行一字未动（跨轮可比性）。
           ⚠️ 开关/日志名用 m7s2 而不是 m7s —— 仓库里已有一个 **旧的 M7s 轮**（2026-11-07，
             `smo_m7s` + `runtime_m7s_poll` + `[m7s]` 行，clip 线程 wait-key 主题）。本轮
             不覆盖它，避免同一开关名/同一 tag 两套语义混在一份日志里。
           G1 = main+0x681b30 池 B 构造总入口（唯一静态调用者 = bl@0x74cae0；函数头
                str x23,[sp,#-0x40]! 是 pre-index 存 ⇒ tail=0 四字窗）。
           G2 = main+0x74ca00 上一层候选（入口 stp x22,x21,[sp,#-0x30]! 也是 pre-index ⇒
                tail=0 四字窗）。**离线重扫推翻了 M7Q P0.4 的措辞**：它不是"无静态调用者"，
                有 8 条静态 bl（0x4457cc/0x447c54/0x449d34/0x44b91c/0x46ea20/0x4a1224/
                0x4a6b54/0x4af1ac）⇒ G2 的 per-LR 表能点名「8 个入口函数的哪一个跑了」。
           G3 = main+0x74cadc：**bl@0x74cae0 的前一字**（`mov w1,w19`）。硬约束「只读门装在
                前一宇、绝不覆盖 bl/blr」⇒ 门不装在 bl 上，装在前一字；0x74cadc → 0x74cae0
                之间无分支、且 0x74cae0 全镜像零落点 ⇒ 到达 0x74cadc 等价于那条 bl 执行。
                它还顺带给出 x0 = [x24+0x90]（= G1 的第 1 实参，可互校）与 LR（= 0x74ca60
                的调用者，8 个站点）。 */
        SMO_OBS_M7S2_G1 = 71,                                           /* 0x681b30 池 B 构造总入口（LR/x0/x1/x8） */
        SMO_OBS_M7S2_G2 = 72,                                           /* 0x74ca00 上层候选（LR/x0） */
        SMO_OBS_M7S2_G3 = 73,                                           /* 0x74cadc = bl@0x74cae0 的前一字（计数+LR） */
        /* M7T2（2026-11-08）：**整族九个头一起装门**。新号段 74..82；65…73 九个门号与对应
           站表行一字未动（跨轮可比性）。M7S 的判决是"调用者根本没跑"，且那是**一整族**
           9 个按类型成对的入口函数（7 个零静态调用者、2 个各 1 条 bl）。M7S 明确警告：
           只装 1 个有假阴性风险 ⇒ 本轮 9 个全装，只有"9 个全 0"才能把断点从"某个类型没进"
           收敛到"整族没进"。
           ⚠️ 开关/日志名用 m7t2 而不是 m7t —— `smo_m7t` / `smo_m7t_pos` / `runtime_m7t_poll`
             / `[m7t]` 已被 2026-11-07 那轮（音频时间线假说）占用，`m7t_boot_run.sh` 也在
             仓库根；硬约束"不许覆盖已有脚本/开关名"优先 ⇒ 本轮全部用 m7t2（与 m7s2 同范式）。
           九个头（离线实测，见 P0）：全部是**首字改写 sp**（8 个 pre-index 存 + 1 个
             sub sp,sp,#0x180）⇒ 全部 `tail=0` 四字窗；窗内 PC 相对字 0、窗内中间字落点 0
             （`LAND-INTERIOR` 9×3 = 0）⇒ safety_bad=0。
             命名（见 P4）：这九个头是**按场景类型分的"场景构造入口"族**（SMO 的 Scene/Stage
             构造：布局 + 对象/自然/天空列表 + 资源加载 worker 池），每个头体内嵌一种场景的
             stage/类名字符串（`SandWorldHomeStage` / `WorldMapStage` / `Demo*Stage` /
             `StageScene` / `TitleLoad`/`GameLoad`/`ContinueLoading` / `AocControllerGuide*`）。 */
        SMO_OBS_M7T2_H1 = 74,   /* 0x445640 demo world-move 场景（DemoWorldMove%sStage 族；零静态调用者） */
        SMO_OBS_M7T2_H2 = 75,   /* 0x447b70 demo 开场/坠机场景（DemoOpeningStage…；唯一 bl@0x449970） */
        SMO_OBS_M7T2_H3 = 76,   /* 0x449c50 demo 结局场景（DemoEndingStage/Ending04；零静态调用者） */
        SMO_OBS_M7T2_H4 = 77,   /* 0x44b720 StageScene 侧（HuntingBandManScene/SePlayArea；零静态调用者） */
        SMO_OBS_M7T2_H5 = 78,   /* 0x46e7a0 StageSceneDefault 侧（SnapShotCameraCtrl；零静态调用者） */
        SMO_OBS_M7T2_H6 = 79,   /* 0x4a1100 WorldMap/WorldSelect 场景（唯一 bl@0x4a10d8） */
        SMO_OBS_M7T2_H7 = 80,   /* 0x4a6b10 Title/GameLoad/ContinueLoading 侧 + SandWorldHomeStage（零静态调用者） */
        SMO_OBS_M7T2_H8 = 81,   /* 0x4a8820 AocControllerGuide/ParGoggles 侧 + SandWorldHomeStage（零静态调用者） */
        SMO_OBS_M7T2_H9 = 82,   /* 0x4af0a0 DemoWorldWarpHole + LoadHomeStageResourceThread（零静态调用者） */
        /* M7U2（2026-11-08）：**场景派发枢纽 main+0x7acd50**。门号接 M7T2 之后：83..85；
           74..82 九门与对应站表行一字未动（跨轮可比性）。
           ⚠️ 开关/日志名用 m7u2 而不是 m7u —— `smo_m7u` / `runtime_m7u_poll` / `[m7u]`
             （clip 窗口瞬态护栏 + 0x1D signaller，2026-11-07 那轮）、`m7u_boot_run.sh`、
             `/tmp/m7u_report.md` 全部已被占用；硬约束"不许覆盖"优先 ⇒ 本轮一律 m7u2
             （与 m7s2 / m7t2 同范式）。
           A = main+0x7acd50：派发枢纽入口。M7T2 实测 H5/H7 的 LR = 0x7acd94 ⇒ 它就是
               "对本对象 [this+0x10] 做 vtable slot 0 派发"的那一层。零静态调用者
               （全镜像 word 级 b/bl 扫描 = 0 条）。首字 stp x20,x19,[sp,#-0x20]! 是
               pre-index 存（改 sp）⇒ **必须 tail=0 四字窗**（不能在 stub 帧内原义重放）。
               门点寄存器：x0=this（场景持有对象：+0 池 B 对象、+8 stage 名、+0x10 场景对象）。
           B = main+0x7acd94：**派发返回址**（0x7acd90 `blr x8` 的后继；M7T2 实测的 LR 就是它）
               ⇒ tail=2 普通字门（只换 1 字 `ldr x0,[x20]`，原义重放后 b 回 0x7acd98）。
               计数语义 = "vtable slot0 派发**成功返回**了多少次"。
               ⚠️ 派活单写 "x0(vtable 指针)" 在此纠正：blr 之后 x0 = 被派发者**返回值**，
               vtable 指针在 0x7acd88/0x8c 的 x8 里、blr 后已被 callee 覆盖（x8 非 callee-saved）；
               vtable 指针改由 C 门从 [this+0x10] 解一层拿到（运行期 name 行打印）。
           C = main+0x7acd70：**stage 名取用点**。0x7acd5c `ldr x1,[x0,#8]` 取出 stage 名指针，
               0x7acd74 `bl 0x9564f0` 拿它查表 —— 只能挂在 0x7acd70（0x7acd50..0x5f 属 A 窗；
               0x7acd74 之后 x1 已被 callee 覆盖）。tail=2 普通字门（只换 `ldr x0,[x8]` 1 字），
               **0x7acd74 的 bl 一个字节都不碰**（运行期 code 行逐字回读该 bl 作证明）。
               观察器在该点按 [x0+8] 抓 stage 名指针 + 前 32B ASCII。 */
        SMO_OBS_M7U2_A = 83,    /* 0x7acd50 场景派发枢纽入口（this/LR/首末 tick/per-LR 16 槽） */
        SMO_OBS_M7U2_B = 84,    /* 0x7acd94 派发返回址（slot0 派发返回次数 + 首 tick + LR） */
        SMO_OBS_M7U2_C = 85,    /* 0x7acd70 stage 名取用点（名指针 + 前 32B ASCII + vtable） */
        /* M7V（2026-11-08）：**点名"真机派发到谁"**。门号接 M7U2 之后：86..93；83..85 与
           74..82 一字未动（跨轮可比性）。
           M7U 已把断点从"枢纽入口/blr"推到"slot 0 里装的是哪个 ctor"（真机 A=B=C=1 而 H1..H9 全 0，
           两平台 [scene]=main+0x1310e18 逐字相同 ⇒ 类相同、slot0 目标不同）。
           X8 = main+0x7acd8c（`ldr x8,[x8]`）：派发前**最后一格**能拿到目标的指令 —— 该处原 x8 是
               vtable 指针（= [scene]），观察器把 [x8] 再解一层就得到 0x7acd90 `blr x8` 真正会跳的
               函数地址 ⇒ "真机派发到谁"从假说变读数。tail=2 单字门（只换 `f9400108` 1 字，
               word[1]=后继 `blr x8` 仅作安装期窗校验、**一个字节都不换**）。非分支、非 PC 相对。
           R1..R7 = Rsv 兄弟实例入口（M7T 记录"每型两套实例：正式 + Rsv"，正式那套是 H1..H9，
               兄弟那套上轮**一个都没装门**）：R1 0x4a6a90 / R2 0x46e770 / R3 0x445590 /
               R4 0x447ae0 / R5 0x449bc0 / R6 0x44b6f0 / R7 0x4af020。
               七门**首字全部改写 sp**（`str x19,[sp,#-0x20]!` 六个 + `stp x20,x19,[sp,#-0x20]!` 一个）
               ⇒ 只能 tail=0 四字窗（在 stub epilogue 之后重放 = 入口态）；窗内无 b/blr，
               X8 之外的 4 个字若含 adrp 由安装期 replay_fix 重算 imm（同绝对页）。
               "谁非零"直接指认族外实例。 */
        SMO_OBS_M7V_X8 = 86,    /* 0x7acd8c ldr x8,[x8]：解一层 = 真派发目标（+LR/时刻/vtbl/scene） */
        SMO_OBS_M7V_R1 = 87,    /* 0x4a6a90 Rsv 兄弟（H7 0x4a6b10 的兄弟侧） */
        SMO_OBS_M7V_R2 = 88,    /* 0x46e770 Rsv 兄弟（H5 0x46e7a0 的兄弟侧） */
        SMO_OBS_M7V_R3 = 89,    /* 0x445590 Rsv 兄弟（H1 0x445640 的兄弟侧） */
        SMO_OBS_M7V_R4 = 90,    /* 0x447ae0 Rsv 兄弟（H2 0x447b70 的兄弟侧） */
        SMO_OBS_M7V_R5 = 91,    /* 0x449bc0 Rsv 兄弟（H3 0x449c50 的兄弟侧） */
        SMO_OBS_M7V_R6 = 92,    /* 0x44b6f0 Rsv 兄弟（H4 0x44b720 的兄弟侧） */
        SMO_OBS_M7V_R7 = 93,    /* 0x4af020 Rsv 兄弟（H9 0x4af0a0 的兄弟侧） */
        /* M7W2（2026-11-08）：**在适配器里复算真 ctor**。门号接 M7V 之后：94（不复用 83..93）；
           86..93 与 74..85 一字未动（跨轮可比性）。
           ⚠️ 开关/日志名用 m7w2 而不是 m7w —— `smo_m7w` / `[m7w]` / `runtime_m7w_poll` /
             `g_m7w_*`（2026-11-07 那轮 stuck-vs-slow 判决）、`m7w_boot_run.sh`、
             `/tmp/m7w_report.md`、`logs/m7w_b1/` **全部已被占用**，且 `smo_m7w` 还是
             M7Q…M7V 一路保留的**活基线开关（运行期 = 1）**；硬约束「不许覆盖开关名/报告」
             优先 ⇒ 本轮一律 m7w2（与 m7s2 / m7t2 / m7u2 同范式）。
           AD = main+0x7724d0：M7V 证实的**通用 mfp 适配器**（两平台 slot0 逐字相同都是它）。
             它的 9 条指令（离线逐条读自 main.patched.bin，md5 89d568fd…）：
               0x7724d0  ldp  x8,x2,[x0,#8]      x8 = [obj+8]（基址）, x2 = [obj+0x10]（槽偏移/函数指针）
               0x7724d4  ldr  x9,[x0,#0x18]     x9 = [obj+0x18] = tag（bit0 = 是否虚）
               0x7724d8  add  x8,x8,x9,asr #1   q  = 基址 + (tag>>1)   ← 真 this
               0x7724dc  tbz  w9,#0,#0x7724e8   tag bit0==0 ⇒ 非虚：目标 = [obj+0x10]
               0x7724e0  ldr  x9,[x8]           虚：x9 = *(q)（= 真对象的 vtable）
               0x7724e4  ldr  x2,[x9,x2]        虚：目标 = *(vtable + [obj+0x10])
               0x7724e8  ldr  x1,[x0,#0x20]     实参
               0x7724ec  mov  x0,x8
               0x7724f0  br   x2                ★ 尾跳（保 x30 ⇒ 族头看到的 LR 仍是 0x7acd94）
             ⇒ 门点 x0 = 这个 0x28 字节的描述符对象（M7V 日志里的 `scene`，两边 `[scene]` 都
                = main+0x1310e18）；观察器在门点**自己复算**上面两步算术，就把「真机派发到谁」
                从「族外」变成**具体地址 + describe 名**。这就是本轮的全部价值。
             tail=2 单字门：只换 word[0] `ldp x8,x2,[x0,#8]`（非分支、非 PC 相对、安装期
                branchy 判定 false），stub 内原义重放后 b 回 0x7724d4；word[1] `ldr x9,[x0,#0x18]`
                仅作安装期窗校验、**一个字节都不换**。
             旁证（离线读，非门）：0x772500 是**同一描述符类的拷贝构造**（`mov w0,#0x28` +
                `bl operator new` + 写 [x0]=vtable + 从 x19 拷 {+8,+0x10,+0x18,+0x20} 共 32B）
                ⇒ 0x28 字节布局 {vtbl, +8 基址, +0x10 槽/函数指针, +0x18 tag, +0x20 实参}
                与适配器的读法**逐字段吻合**（对象布局不是猜的）。 */
        SMO_OBS_M7W2_AD = 94,   /* 0x7724d0 通用 mfp 适配器入口（obj/tag/p/o/q/vt/tgt 全落日志） */
        /* M7X2（2026-11-08）：**最后一跳**。门号接 M7W2 之后：95/96（SMO_OBS_N 95→97）；
           94 与 74..93 一字未动（跨轮可比性契约）。
           ⚠️ 开关/日志名用 m7x2 而不是 m7x —— `smo_m7x` / `[m7x]` / `runtime_m7x_poll` /
             `g_m7x_*`（2026-11-07 那轮 wait-object 判决）、`m7x_boot_run.sh`、
             `/tmp/m7x_report.md`（11,371 B / 2026-11-07 / DONE）、`logs/m7x_b1/`
             **全部已被占用**（开工 grep 逐条核过）；硬约束「不许覆盖开关名/报告/证据」
             优先 ⇒ 本轮一律 m7x2（与 m7s2 / m7t2 / m7u2 / m7w2 同范式）。
           G1 = main+0x74c9e0：M7W2 点名的那条 **slot-3 转调 thunk**（3 条指令 + udf 填充）：
               0x74c9e0  ldr x8,[x0]        x8 = *(this) = 对象 q 的 vtable
               0x74c9e4  ldr x2,[x8,#0x18]  x2 = vtable 第 4 槽（+0x18）= 真正的被调者
               0x74c9e8  br  x2             ★ 尾跳（保 x30）
               0x74c9ec  udf #0
             它由适配器（0x7724f0 `br x2`）尾跳进来，**x0 已经是 q（真 this）** ⇒ 门点 x0 = q，
             观察器解两层 [q] 与 [[q]+0x18]（顺带记 [q+0x18] 这个字面字段，避免"两层/一层"
             歧义）⇒ **"真正干活的人"从"一个光栅地址"变成可 describe 的 vtable slot 3**。
             三个实例上 q 各不相同（客体堆），`[obj+0x10]`/`[obj+0x18]` 却是常量 ⇒ 本门是
             "同一 thunk 被不同 this 进入"这一跳的**唯一读数来源**。
             全镜像对 0x74c9e0 **零静态 bl/b**（唯一引用是数据指针 0xd885d8）：装门不截断任何
             调用；同形状 3 指令 thunk 在镜像里还有 5 份（0x512a10 / 0x6cd274 / 0x6d0940 /
             0x8578f4 / 0x91a9dc）⇒ 它是**每类一份的 slot-3 转发器模板**，不是独苗。
           G2 = main+0x772500：**描述符克隆/拷贝构造**（M7W2 的"候选写入点 A"）。离线逐条：
               0x772500  str x19,[sp,#-0x20]!   ; 存调用者的 x19（callee-saved）
               0x772504  stp x29,x30,[sp,#0x10]
               0x772508  add x29,sp,#0x10
               0x77250c  mov x19,x0             ; ★ x19 := 入口 x0 = **源描述符**
               0x772510  mov w0,#0x28
               0x772514  bl  0x93e810           ; operator new(0x28) → x0 = 新对象
               0x772518  adrp x8,#0x136b000 ; ldr x8,[x8,#0xa10] ; add x8,x8,#0x10
               0x772528  str x8,[x0]            ; 新对象 vtbl（运行期注册表 [[0x136b000]+0xa10]+0x10）
               0x77252c  ldp x8,x9,[x19,#8]     ; 源 +8  / +0x10
               0x772530  ldp x10,x11,[x19,#0x18]; 源 +0x18 / +0x20
               0x772538  stp x8,x9,[x0,#8]      ; ★ 写 +8 / +0x10
               0x772534  stp x10,x11,[x0,#0x18] ; ★ 写 +0x18 / +0x20
               0x77253c  ldr x19,[sp],#0x20 ; 0x772540 ret
             ⇒ **源 = 入口 x0（= 0x77250c 的 `mov x19,x0`；不是寄存器猜测）**，被写的 32 B 就是
             `ldp` 从源读出的那 32 B（纯 ldp/stp 拷贝，逐字相同 = 等价可证）；新对象地址在门点
             还不存在（0x772514 才 operator new）⇒ 门点记"源 + 其 32 B"，目标对象由等式给出。
             首字 `str x19,[sp,#-0x20]!` **改写 sp** ⇒ 必须 tail=0 四字窗（在 stub epilogue 之后
             重放 = 入口态），不能 tail=2（stub 帧里再压 sp 会毁掉函数栈）。
             全镜像对 0x772500 也是**零静态 bl/b**（唯一引用是数据指针 0xd88620，与 0x74c9e0 的
             0xd885d8 同处运行期方法注册表 0xd885c8…0xd88648）⇒ 它同样由运行期注册表调用。 */
        SMO_OBS_M7X2_G1 = 95,   /* 0x74c9e0 slot-3 转调 thunk：q=[x0]，vt=[q]，tgt3=[[q]+0x18]，o18=[q+0x18] */
        SMO_OBS_M7X2_G2 = 96,   /* 0x772500 描述符克隆：src=入口 x0，写 {+8,+0x10,+0x18,+0x20} 共 32B */
        /* M7Y2（2026-11-08）：门号接 M7X2 之后 = 97/98（SMO_OBS_N 97→99），M7H_NG 仍 15。
           G1 97 = main+0x44adb0 —— **真机那个"真正干活的人"**。离线逐条（113 条指令 / 0x1c4 B）：
             0x44adb0 sub sp,sp,#0x150；随即两个 setter `bl 0x74ca50`（[this+0xd0]）、
             `bl 0x74c9f0`（[this+0xa0]，紧邻 M7X2 的 G1 thunk）；中段 `mov w0,#0x138; bl operator new`
             造子对象写 [this+0xe0]，`mov w0,#0x78; bl operator new` 写 [this+0xd8]；
             末尾 `bl 0x6c7590(this, main+0x12bd5b8, 1)` —— 0x12bd5b8 正是类名
             "…FirstSequenceSceneNrvSelectModeE" 之后那一格（= 进 SelectMode 状态的类型令牌）；
             唯一 `blr x8` 在 0x44aedc（[x21] slot1，真虚调用）；0x44af70 `ret` 收尾，其后 3 字 udf。
             ⇒ **不是空实现/占位**（判据：长度 0x1c4 B、`ret` 在末尾、21 条 bl + 1 条 blr、
             3 处 `str` 落到 this 的 +0xd8/+0xe0/+0xe8、两次 operator new）。
             首字 `sub sp,sp,#0x150` **改写 sp** ⇒ tail=0 四字窗（在 stub epilogue 之后重放 = 入口态）。
             门窗四字 `d10543ff a9115ffc a91257f6 a9134ff4` 全非 PC 相对、全过 replay_fix 拒装清单。
           G2 98 = main+0x44af80 —— 同一个类（vtable main+0x12bd470）的 slot +0x20（`{0x44af80,0x12bd498}`），
             用 'Stage' / '２Ｄバック（メイン画面）' 等字符串搭场景层；用于回答"这个类除 slot3 之外
             还有没有别的生命周期方法被驱动"。首字 `str x19,[sp,#-0x20]!` 改写 sp ⇒ 同样 tail=0 四字窗，
             窗 `f81e0ff3 a9017bfd 910043fd aa0003f3` 全非 PC 相对。
           两门 safety（/tmp/m7y2_safety.py）：三镜像 md5 89d568fd… 全等、窗内 0/4 PC 相对、
           全镜像分支落点 FIRST=0/INTERIOR=0、与既有 89 行站表零重叠、零静态 bl/b
           （唯一引用是类方法表数据指针 0xcc7420 / 0xcc7438）。 */
        SMO_OBS_M7Y2_G1 = 97,   /* 0x44adb0 真机 slot3 终点：真实现（113 insn/21 bl/1 blr/2 次 new/3 处写 this） */
        SMO_OBS_M7Y2_G2 = 98,   /* 0x44af80 同类 slot +0x20：场景层搭建（'Stage'/'２Ｄバック' 族字符串） */
        /* M7Z2（2026-11-08）：**选择点分叉**。门号接 M7Y2 之后 = 99..103（SMO_OBS_N 99→104），M7H_NG 仍 15。
           `cbnz` 本身不能当门点（replay_fix 拒装 cond-branch），所以门全装在它两侧的目标上。
           离线逐条（/tmp/m7z2_dis_a.py..k.py，镜像 md5 89d568fd…）：
             HostType 的 nerve 链（.bss 0x12d0d28..0x12d0e98，加载期 reloc 行填出，token = 名字格 −0x10）：
               HostTypeNrvBoot            token 0x12d0d48  exec 0x4b93a0 → b 0x4b8d30
               HostTypeNrvDestroyFirstSequence 0x12d0d88  exec 0x4b93b0 → 0x4b8f90
               HostTypeNrvEnd             0x12d0dc8  exec 0x4b93c0 → 0x4b92f0
               HostTypeNrvLoadFirstSequence 0x12d0e08 exec 0x4b93d0 → b 0x4b8e20   ★选择点所在
               HostTypeNrvFirstSequence   0x12d0e48  exec 0x4b93e0 → 0x4b8ef0
               HostTypeNrvLoad            0x12d0e88  exec 0x4b93f0 → 0x4b9030
             宿主函数 0x4b8d30 = NrvBoot::execute（0xf0 堆分配 + [this+0x20]=场景持有者 → 转 NrvLoadFirstSequence）；
             0x4b8e20 = NrvLoadFirstSequence::execute：
               0x4b8e80 ldrb w8,[x19,#0x58] / 0x4b8e84 cbnz w8,#0x4b8ea4      ← [this+0x58] 字节标志
               0x4b8e88 mov w1,#0x21c / 0x4b8e8c mov x0,x19 / 0x4b8e90 bl 0x6c7d30
               0x4b8e94 tbz w0,#0,#0x4b8ea4     ← 查询返回 0（state_time > 0x21c）才去做活
               0x4b8e98 ldp x29,x30,[sp,#0x10] → 0x4b8ea0 ret   ← 跳过
               0x4b8ea4 ldp x0,x1,[x19,#0x18] → bl 0x7722b0 → ldr x0,[x19,#0x20] → blr [[x0]+0x20]
                        → str [x8+0x80]=[this+0x20] → b 0x6c7b20(this, 0x12d0e48) 即转 NrvFirstSequence
             0x6c7d30 = **通用"状态时间 <= N"谓词**（不是 HostType 的 ready/login 查询）：
               ldr x8,[x0]; ldr x8,[x8]; blr x8   → 状态机 = [[this]→slot0]
               ldr w8,[x0,#0x18]                   → 状态机 +0x18 = **当前状态已运行的步数**（0x6c7750 换 nerve 时置 -1）
               cmp w8,w19; cset w0,le              → 返回 (state_time <= N)
               同族 8 个（0x6c7b50 eq / 0x6c7c00 ge / 0x6c7cc0 ==0 / 0x6c7cf0 lt / 0x6c7d30 le /
               0x6c7d70 gt / 0x6c7db0 range / 0x6c7e00 mod），**0x6c7d30 全镜像 114 个调用者**
               ⇒ 0x21c = 540 不是枚举，是**帧阈值**（540 步 ≈ 9.0 s @60 Hz；同一 nerve 的上一步
               NrvBoot 在 0x4b8de4 用的是 0x3c = 60 = 1.0 s）。 */
        SMO_OBS_M7Z2_G1 = 99,   /* 0x6c7d30 谓词入口：全命中计数 + 本站点（LR=0x4b8e94）x0=this / x1=0x21c / LR */
        SMO_OBS_M7Z2_G2 = 100,  /* 0x4b8e98 跳过路径（cbnz 不成立且查询返回 1）：LR 分辨来路 + x0=查询返回 + x8=标志字节 */
        SMO_OBS_M7Z2_G3 = 101,  /* 0x4b8ea4 做活路径：LR=0x4b8e94 ⇒ 经查询放行；LR≠ ⇒ cbnz 直接放行 */
        SMO_OBS_M7Z2_G4 = 102,  /* 0x4b8e88 cbnz 不成立处：x8 = [this+0x58] 原始字节（只在标志为 0 时到这儿） */
        SMO_OBS_M7Z2_G5 = 103,  /* 0x4b8e20 NrvLoadFirstSequence::execute 入口：x0=this → 读 [this] / [[this]] /
                                   [this+0x58] / [this+0x20] / [this+0x60]（门4"观察器补读"，读的是宿主本身） */
        /* M8B（2026-11-08）：**序列对象 nerve 状态机**。门号接 M7Z2 之后 = 104/105（SMO_OBS_N 104→106），
           M7H_NG 仍 15，99..103 与 74..98 一字未动（跨轮可比性契约）。
           M8A 的硬结论：`0x4b8e84` 那对判据在模拟器上是**死代码**（门 5 = 0 次 / site_hits = 0），
           真分叉是"序列对象当前 nerve"，唯一写点是 `main+0x6c7750`。本轮就装这一只 + 宿主入口一只。
           G1 = main+0x6c7750（离线逐条，镜像 md5 89d568fd…，19 条指令）：
             0x6c7750 stp x20,x19,[sp,#-0x20]!   ← 首字改写 sp ⇒ tail=0 四字窗
             0x6c7754 stp x29,x30,[sp,#0x10]
             0x6c7758 add x29,sp,#0x10
             0x6c775c ldr w8,[x0,#0x18]          ← 旧步数（-1 = 刚从别的 nerve 换过来）
             0x6c7760 mov x20,x1                  ← x1 = **新 nerve token**
             0x6c7764 mov x19,x0                  ← x0 = **NerveKeeper**
             0x6c7768 tbnz w8,#0x1f → 0x6c7784    ← 步数 <0 就跳过 exit 钩子
             0x6c776c..0x6c7780 ldr x0,[x19,#8] / cbz / [[x0]+8](state, keeper)  ← 旧 state 的 exit
             0x6c7784 mov w8,#-1
             0x6c7788 str x20,[x19,#0x10]         ★★ **唯一 nerve 写点**（[keeper+0x10] = token）
             0x6c778c str w8,[x19,#0x18]          ← 步数重置 -1
             0x6c7790..0x6c7798 收尾 ret
           三个静态入口（全镜像 bl/b 扫描 = 15 条，其中 3 条在 nerve 机制内部）：
             ① 直调 bl 0x6c7750（12 条，例 0x14b430 / 0x14b4dc …）
             ② 0x6c7b48 `b 0x6c7750`（= **0x6c7b20 setNerveByState 的尾跳**：[[this]] 取 keeper 后转过来）
             ③ 0x6c8e0c `b 0x6c7750` 与 0x6c8da0 `bl 0x6c7750`（= **0x6c8d80 tryChangeNerve**：
                `[[x20]]` 取 keeper、0x6c79e0 判新 nerve 后置位）
           ⇒ 本门覆盖全部三条 nerve 写入路径；LR 用来区分"谁写的"（尾跳时 LR = 0x6c7b20/0x6c8d80 的调用者）。
           观察器读数：x1 = 新 token、[x0+0x10] = 旧 token、[x0+0x18] = 旧步数、LR、tick，
           并把 **[x1+0x10] 处的 nerve 名字串**（SMO 的 nerve 对象 +0x10 = 名字格，见下）前 63 B 抄进记录。
           nerve 对象布局（离线由加载期 reloc 行反推，161,696 行全量扫）：
             对象 base B：+0x00 = exec 入口（`ldr x0,[x1]; b <execute>` 的 thunk 目标）、
             +0x08 = 0x2020、+0x10 = （派生型）自指针 / （基型）0、**+0x20 = 名字串指针**、+0x28 = 0x119a308；
             **token = B+0x10 ⇒ 名字格 = token+0x10**（M8A 的"token = 名字格 − 0x10"本轮由 reloc 行独立复核）。
           G2 = main+0x4b8d30（= `HostTypeNrvBoot::execute`，exec thunk 0x4b93a0 尾跳进来）：
             0x4b8d30 sub sp,sp,#0x30   ← 首字改写 sp ⇒ tail=0 四字窗
             唯一静态入口 = `b 0x4b8d30` @ 0x4b93a4（exec thunk `ldr x0,[x1]` 之后），
             故 LR = 调用该 exec 的那一层的返回址（nerve 派发循环）⇒ 只记计数 + LR + 首末 tick。
           ⚠️ 按 M8A 结论**不再给 0x4b8e84 那一对判据门位**（死代码，已证）。 */
        SMO_OBS_M8B_G1 = 104,   /* 0x6c7750 NerveKeeper::setNerve：x1=新 token / [x0+0x10]=旧 token /
                                   [x0+0x18]=旧步数 / LR / tick + 逐事件环(首 128) + 每 token 计数表(40) */
        SMO_OBS_M8B_G2 = 105,   /* 0x4b8d30 HostTypeNrvBoot::execute 入口：只计数 + LR 台账(16) + 首末 tick */
        /* M8D（2026-11-08）：**真机回归第一轮**。门号接 M8B 之后 = 106/107（SMO_OBS_N 106→108），
           M7H_NG 仍 15，74..105 一字未动（跨轮可比性契约）。
           目的：M8B 对"真机绑哪张 nerve 表"只给了**静态推断**（FirstSequence 族 0x12d0fe8 @
           main+0x4b8cb0 vs TitleMenu 族 0x12d06d8 @ main+0x4b7380，两绑表点零静态调用者）。
           本轮把两只**孪生绑表点**各装一只入口门 ⇒ 用**当机计数**确认/否定，不靠 token 名字反推。
           离线逐条（/tmp/m8d_work/s_safety.py → safety_out.txt，镜像 md5 89d568fd…，21,833,968 B）：
             0x4b8cb0 = `stp x20,x19,[sp,#-0x20]!; stp x29,x30,[sp,#0x10]; add x29,sp,#0x10;
                         adrp x20,0x12d0000` → `add x20,#0xfe8`（= **表 0x12d0fe8** / FirstSequence 族）
                         → `mov w2,#2; mov x1,x20; mov x19,x0; bl 0x6c7590`（绑 2 槽表）
             0x4b7380 = 同款 prologue → `add x20,#0x6d8`（= **表 0x12d06d8** / TitleMenu 族）→ bl 0x6c7590
           两门 safety：窗字与镜像逐字同一；窗内 PC 相对 = **1/4（只有第 4 字 adrp）**，
             `smo_obs_replay_fix` 按"同绝对页重算 imm"处理 adrp（round13b 既有能力）；
             全镜像分支落点 **FIRST=0 / INTERIOR=0**、静态 bl/b 调用者 **total=0**（只有 vtable 进得来）；
             与既有 98 行站表 ±0x20 **零重叠**。两门首字都改写 sp ⇒ **tail=0 四字窗**。
           ⚠️ 这两门**不替代** M8B 判据，也**不做因果归因**：作废判别量⑧"两边绑定器/绑定点不同"
           仍是作废量 —— 本轮只把"该族实例是否在本机被构造过"从推断升级成计数。 */
        SMO_OBS_M8D_G1 = 106,   /* 0x4b8cb0 FirstSequence 族绑表点（表 0x12d0fe8）：计数 + 末见 LR */
        SMO_OBS_M8D_G2 = 107,   /* 0x4b7380 TitleMenu   族绑表点（表 0x12d06d8）：同上 */
        /* M8E2（2026-11-08）：**追第一个分叉** —— 谁决定 `DemoOpening` 还是 `LoadStage`。
           门号接 M8D 之后 = 108..111（SMO_OBS_N 108→112），M7H_NG 仍 15，74..107 一字未动。
           离线定位（main.patched.bin md5 89d568fd…，21,833,968 B）：
             函数 main+0x4b4230（HakoniwaSequence 初始化；M8D 的 LR 0x4b4bbc / 0x4b4d2c 都在它里面）
               0x4b4bac adrp x1,0x12d0000 / 0x4b4bb0 add x1,#0x420   → token 0x12d0420 BootLoadData
               0x4b4bb8 bl 0x6c7b20 (setNerve)                       ★ 返回址 = 0x4b4bbc（M8D 的"分叉入口"）
               0x4b4bbc ldr x0,[x19,#0xb8]                           ← 被判断的对象 A = [seq+0xb8]
               0x4b4bc0 bl 0x4d2bd0                                  ★ 判断点的**前一字**
               0x4b4bc4 tbz w0,#0,#0x4b4d2c                          ★★ **判断指令**（位 0 == 0 ⇒ 跳到尾声）
               0x4b4bc8..0x4b4d18 落到这里的只有"位 0 == 1"这一侧（建场景/调 0x51c7d0）
               0x4b4d1c/0x4b4d20 add x1,#0x428 → token 0x12d0428 DemoOpening
               0x4b4d28 bl 0x6c7b20 (setNerve)                       ★ 返回址 = 0x4b4d2c（M8D 真机读数）
               0x4b4d2c = 尾声（add sp,sp,#0xcb0 / ret）—— **位 0 == 0 时直接从这里返回，
                        不写 DemoOpening**（模拟器那一侧就是这条：之后才由别的写点落到 LoadStage）
             判断点的**被调者** main+0x4d2bd0（3 条指令的 leaf getter）：
               0x4d2bd0 ldr  x8,[x0,#0x20]      ← A 的 +0x20 = 对象 B
               0x4d2bd4 ldrb w0,[x8,#0x5f8]     ★★ **字段 = B+0x5f8 的低字节，判定值 = bit0**
               0x4d2bd8 ret
             ⇒ 判定式：`if ((*(u8*)(B+0x5f8) & 1) == 0) return;  else setNerve(DemoOpening)`
             B+0x5f8 的语义旁证（**推断，不作判据**）：同族偏移 0x5f8/0x5f9/0x81c/0x7c9 由
               0x4c52f8 `strb w8,[x28,#0x5f8]` 写入，其值 = 按字符串键读配置的布尔
               （键 = main+0xe57d4a = "IsEnableCap"，下一格 0x5f9 的键 = "NextWorldId"）；
               0x4c8230 `strb wzr,[x19,#0x5f8]` 清零；0x4d24ac 调试打印器同族读 0x7c9/0x5f8/0x5f9/0x81c。
               这三处与判断点**是否同一台 B 未证**，故本报告只把它列为"候选写者"，不据此下结论。
           G1 108 = main+0x4b4bc0（tail=3 单 bl 门）：判断点前一字，x0 = A ⇒ 记 判定值 bit0 +
             A + B + tick + LR（判定值由观察器**自己按 0x4d2bd0 的同一路径**读出来，与被判断的值同源）。
           G2 109 = main+0x4b4bb8（tail=3 单 bl 门）：返回址正是 0x4b4bbc（分叉入口）⇒ 计数 + x0=seq +
             x1=新 token + 首末 tick + LR（确认两边都进过分叉口）。
           G3 110 = main+0x59ac30（tail=0 四字窗）：场景图创建入口，x1 = "Stage" 入参（观察器抄字符串）。
           G4 111 = main+0x59accc（tail=1 单 ret 门）：同一函数的返回点，x0 = **返回 x0**（场景图创建结果）。
           ⚠️ 0x4b4bbc / 0x4b4bc0 都**不能**做 tail=0 四字窗：窗内含 `tbz`（PC 相对，replay_fix 拒装），
             所以判断点用 tail=3（只换 bl 一个字，后继 tbz 一个字节不动）。
           safety（/tmp/m8e2_work/safety.py）：四门窗字与镜像逐字相同；110 窗内 PC 相对 **0/4**、
             分支落点 FIRST=4/INTERIOR=0；与既有 100 行站表 ±0x20 **零重叠**。 */
        SMO_OBS_M8E2_G1 = 108,  /* 0x4b4bc0 bl 0x4d2bd0（判断点前一字）：判定值 = u8([[A+0x20]]+0x5f8) bit0 */
        SMO_OBS_M8E2_G2 = 109,  /* 0x4b4bb8 bl 0x6c7b20（返回址 = 分叉入口 0x4b4bbc）：计数 + seq + token */
        SMO_OBS_M8E2_G3 = 110,  /* 0x59ac30 场景图创建入口：x1 = "Stage" 入参（抄字符串）+ LR */
        SMO_OBS_M8E2_G4 = 111,  /* 0x59accc 同一函数的 ret：x0 = 返回 x0 + LR */
        /* M10A（2026-11-08）：**A 支入口**（开关 smo_m10a）。门号接 M8E2 之后 = 112（SMO_OBS_N 112→113），
           108..111 一字未动。离线定位（main.patched.bin md5 89d568fd…，21,833,968 B）：
             0x4b4bc4 tbz w0,#0,0x4b4d2c   ← 判断指令；落空（bit0=1）才继续往下
             0x4b4bc8 ldr x8,[x20]         ★ 本条 = 落穿侧**第一条**指令
           全镜像逐字扫（b/bl/b.cond/cbz/cbnz/tbz/tbnz 五族）：**转 0x4b4bc8 的分支 = 0 条**
           ⇒ 该地址只可能由 0x4b4bc4 的 tbz **落空**到达 ⇒ 命中数 = "A 支被走进"的次数（无旁路）。
           窗字实测：word[0]=f9400288（ldr x8,[x20]，非分支、非 PC 相对）⇒ **tail=2 普通字门**
           （stub 内原义重放这 1 字后 b 回 off+4，后继 f9401108 一个字节不动）。
           ⚠️ 本条要解引用 x20；x20 若坏，崩点在 stub 页内（pc 落在 RX 尾页 ⇒ 由 LR/x0 归因），
             本轮只做"进没进 A 支"的计数，不对 x20 合法性下结论。 */
        SMO_OBS_M10A_A = 112,
        /* M15A（2026-11-08）：**谁画了那笔黑**。门号接 M10A 之后 = 113（SMO_OBS_N 113→114），
           74..112 一字未动（跨轮可比性契约）。
           门点 main+0x988a20 = NVN submit 入口（M14A 逐条读出的 0x180 B 函数：把同一个 64 KB
           常量环按 stage 0/1/2/5 绑一遍；`0x988b0c` = stage0(VS) 的 `blr x8` 返回址、
           `0x988b34` = stage1(FS) 的 `blr x8` 返回址）。M14A 的静态结论是**190 个 bl 站点 /
           150 个宿主函数**都能到这里 ⇒ 从 LR 往上两级在静态上是 150 路分叉，**必须动态取 x30**。
           ⚠️ 派活单给的窗字 `f50f1df8 f44f01a9 fd7b02a9 fd830091` 是**内存字节串**（小端显示），
           站表要的是 u32，故此处写 `f81d0ff5 a9014ff4 a9027bfd 910083fd`——本轮开工前用
           /tmp/m15a_work/verify113.py 从镜像逐字读回核对（bytes：f5 0f 1d f8 f4 4f 01 a9
           fd 7b 02 a9 fd 83 00 91，u32 逐字相同 ⇒ 同一组指令，只是口径换了一次）。
           首字 `str x21,[sp,#-0x30]!` **改写 sp** ⇒ tail=0 四字窗（stub epilogue 后在入口态重放）。
           前一字 [0x988a1c] = 00000000（udf 填充），门不触碰 -4。
           窗内 PC 相对 = 0/4；分支落进窗内（G..G+0x10）= **0 条**（全镜像 5.46M 字五族扫）；
           静态调用者 190 bl / 150 函数；与站表 105 行 ±0x20 **零重叠**（最近 0x9646d0，距 0x24350）。
           载荷：x0 = A（NVN 命令对象；名字缓冲 @+0x358，≤63 B）、x1 = B（stub 走参 4 = x3）、
           x30 = 调用者返回址（stub 走参 5 = x4）。
           观察器（smo_obs_observer）只做原子计数 + 定长写：**不解引用 x0+0x358**
           （名字由 port 侧 runtime_safe_read_ungated 读，原因见 M15A 注：stub 上下文里做
           带 setjmp 的安全读会嵌套 fault guard，不值当）。 */
        SMO_OBS_M15A_X30 = 113,
        /* M16A（2026-11-08）：**"GORP." 那条 trampoline 的入口**。门号接 M15A 之后 = 114（SMO_OBS_N 114→115），
           74..113 一字未动（跨轮可比性契约）。
           门点 main+0x991a30 = M15A `x30` top1 的归属函数起始（FDE size 0x80）。本轮用镜像
           main.patched.bin（md5 89d568fd8f62fc7a0777712bd01f4642，21,833,968 B）逐字核对四字窗：
             u32  = a9bc5ff8 a90157f6 a9024ff4 a9037bfd
             bytes= f8 5f bc a9 f6 57 01 a9 f4 4f 02 a9 fd 7b 03 a9
           （派活单 M16A 给的就是这组 u32，与镜像逐字相同；M15A 派活单那串 f50f1df8… 是**门 113**的字节串，
            不是本门——本门两种口径换算后都是同一组指令，详见 /tmp/m16a_work/verify114.py 输出。）
           首字 `stp x24,x23,[sp,#-0x40]!` **改写 sp** ⇒ tail=0 四字窗（stub epilogue 后在入口态重放）。
           前一字 [0x991a2c] = d65f03c0（`ret`，上一函数出口，门不触碰）。
           窗内 PC 相对 **0/4**；分支落进被替换窗 (G,G+0x10) = **0 条**；FIRST=16（14 bl + 2 b，全落首字）
           ⇒ 静态调用者 16 站 / 15 个宿主函数；与站表 106 行 ±0x20 **零重叠**（最近 0x988a20，距 0x9010）。
           载荷（**7 参专用变体**，见 smo_obs_observer_m16a）：x0（arg0，名字串候选 @+0x358）、
           x1、x2、x3、x30（调用者返回址）。函数体本身**无分支**（0x80 B 全直线：只 1 个 bl 0x988a20
           + 1 个尾 b 0x991ab0）⇒ "该停画"的判定不在本函数，在 15 个宿主里。
           观察器只做原子计数 + 定长写，**不解引用任何参数**（名字由 port 侧 runtime_safe_read_ungated 读）。 */
        SMO_OBS_M16A_GORP = 114,
        /* M17A（2026-11-08）：**逐笔四边形记录叶**（2D quad record leaf）。门号接 M16A 之后 = 115
           （SMO_OBS_N 115→116），站表 107→108 行（**只在表尾追加**，74..114 一字未动）。
           门点 main+0x98b4b0 = fn 0x98b490 的 **+0x20**（入口 0x98b490 的 [x2]==-1 早退 b.eq 在
           0x98b498，落在四字窗内 ⇒ 入口装不了，退到 +0x20；也因此本门**只记录带贴图键的四边形**，
           [x2]==-1 的"无贴图"四边形在 +0x8 就早退了，不进本门——这一点必须写进报告）。
           镜像 main.patched.bin（md5 89d568fd8f62fc7a0777712bd01f4642，21,833,968 B）逐字核对：
             u32   = aa0203f3 aa0103f4 aa0003f5 91058017
             bytes = f3 03 02 aa f4 03 01 aa f5 03 00 aa 17 80 05 91
           （派活单 M17A 给的正是这组 u32；两种口径同一组指令，无差异。）
           = mov x19,x2 / mov x20,x1 / mov x21,x0 / add x23,x0,#0x160 —— 四条**寄存器+立即数**，
           窗内 PC 相对 **0/4**；前一字 [0x98b4ac] = 9100c3fd（add x29,sp,#0x30，**门不触碰**）。
           分支落点：FIRST=0 / INTERIOR=0（全镜像 5.46M 字五族扫；本函数内 284 条落点全部落在
           入口 0x98b490 或本函数早期，无一落 +0x20）。
           静态调用者：到 fn 入口 0x98b490 = **277 站 / 130 宿主函数**（12 b + 265 bl）；
           到 +0x20 本身 = **0 条** ⇒ 本门只观测"正常入口"路径。
           与站表 107 行 ±0x20 **零重叠**（最近 0x988a20，距 0x2a90）。
           载荷（沿用 M16A 的 **30 字 7 参变体**）：x0（渲染上下文）、x1、x2（四边形描述符，
           [x2] = 贴图键 / -1 = 不画）、x3、x30（**逐笔的宿主返回址**）。
           观察器只做原子计数 + 定长写，**不解引用任何参数**（[x2] 由 port 侧 drain 时安全读）。 */
        SMO_OBS_M17A_QUAD = 115,
        /* M18A（2026-11-08）：**UI 颜色入参**两门（接 M17A 之后 = 116/117，SMO_OBS_N 116→118，
           站表 108→110 行，**只在表尾追加**，74..115 一字未动）。
           116 = main+0x8fea2c（2D **上色**绘制助手）：调用点由 `ldr x8,[x20,#0xb8];
             add x3,x8,#0x658` 算出 **x3 = 颜色指针**（16 B 颜色块），本函数 `ldp x0,x1,[x3]`
             后 `br x6`（x6=[[0x136e000+0x930]]）尾跳进真正的绘制实现 ⇒ LR 保持调用点返回址
             （M17A 的 rec lr 就是这么来的）。所以本门把"这一笔用的是哪个颜色指针、块里是什么"
             直接带出来。静态调用者 8 站 / 1 宿主（0x9106fc 内 5 处 bl + 0x90a580 的尾 b）。
           117 = main+0x9106fc（2D 面板/文字渲染器入口）：x0 = UI 层对象（颜色表 +0xf0..+0xfc，
             由 ctor 0x90f450 从资源块读入）、x1 = 渲染上下文（本函数取 [x1+0xb8]+0x658 当颜色）、
             x2 = 目标。静态调用者 3 站（0xa179b0 / 0xa189d0 / 0xa17a54 尾 b）。
           两窗户四字与镜像逐字相同（/tmp/m18a_work/safety116.py 读出）；窗内 PC 相对 0/4；
           分支落点 FIRST=8/3（全落函数入口，正常）、**INTERIOR=0**；与既有 108 行站表 ±0x20
           零重叠；两门互不重叠。首字都改写 sp ⇒ tail=0 四字窗（与门 114 同款，prologue 逐字相同）。
           载荷沿用 30 字 7 参变体：x0 / gate / 原 x8 / 原 x1 / LR / 原 x2 / 原 x3。
           观察器只做原子计数 + 定长写，**不解引用任何参数**（内存安全读全在 port 侧 drain）。 */
        SMO_OBS_M18A_UIFILL = 116,
        SMO_OBS_M18A_PANEL = 117,
        /* M19A（2026-11-08）：**门 118 = main+0x916a28**（2D 批处理器 fn main+0x915dfc 的 +0xc2c，
           SMO_OBS_N 118→119，站表 110→111 行，**只在表尾追加**，74..117 一字未动）。
           门点四字 = ldr d0,[x8,#0x700] / ldr w8,[x8,#0x708] / fmov d1,x8 / mov v0.d[1],v1.d[0]
           （u32 fd438100 b9470908 9e670101 6e180420），紧随其后 str q0,[x19,#0x320]。
           静态形态（镜像逐字）：x19 = 入口 x0（mov x19,x0 @+0x10，**全函数只此一次**）；
           x8 = [x19+0x10]（状态对象，@+0x1214 重装）；x1 = x19（@+0x990 mov x1,x19，之后不再改）；
           **x3 全函数从未写过** ⇒ 门点 x3 是调用者的残值（不是颜色描述符；按派活单口径也做一次
           安全读 [[x3]] 作对照，但 ABI 真值走 ctx+0xb8 链）。
           载荷沿用 30 字 7 参变体：x0（门点当时的 x0 = sp+8 栈上 NVN 命令对象）/ gate / **x8 = 状态**
           / **x1 = 批上下文** / LR / x2 / x3。观察器只做原子计数 + 定长写，不解引用任何参数
           （全部内存安全读在 port 侧 drain）。 */
        SMO_OBS_M19A_RGB = 118,
        SMO_OBS_N = 119 };   /* 0-3/7/8/10/11 已退役（round21），id 空洞留给 dump 侧兼容 */
typedef struct { uint32_t off, gate, tail; uint32_t word[4]; } smo_obs_site_t;
static const smo_obs_site_t smo_obs_site[] = {
    /* round21 集合（12 门；退役门与理由见上方注释；窗字全经 scripts_round21/safety.py
       与 main.patched.bin 比对 + 全镜像窗内零落点验证）。 */
    {0x8ad1f4, SMO_OBS_KICK1,   0, {0xa9017bfd, 0x910043fd, 0xaa0003f3, 0x7100043f}},   /* stp x29,x30,[sp,#0x10]; add x29,sp,#0x10; mov x19,x0; cmp w1,#1 */
    {0x587ea8, SMO_OBS_ALKICK,  1, {0x140c94d2, 0xd65f03c0, 0, 0}},                     /* b 0x8ad1f0（后继 0x587eac ret） */
    {0x8ad318, SMO_OBS_KICK3,   1, {0x14000002, 0x00000000, 0, 0}},                     /* b 0x8ad320（后继 0x8ad31c udf 填充） */
    {0x954310, SMO_OBS_BC310,   0, {0xd10203ff, 0xf9001bf9, 0xa9045ff8, 0xa90557f6}},   /* sub sp,sp,#0x80; str x25,[sp,#0x30]; stp x24,x23,[sp,#0x40]; stp x22,x21,[sp,#0x50] */
    {0x587da4, SMO_OBS_THUNK1_IN,   1, {0x1400003f, 0x00000000, 0, 0}},                 /* b 0x587ea0（thunk1 全部进入） */
    {0x587eac, SMO_OBS_THUNK1_NULL, 1, {0xd65f03c0, 0xf9400000, 0, 0}},                 /* ret（thunk1 [x0]==NULL 早退；后继=thunk2 入口） */
    {0x587db4, SMO_OBS_THUNK2_IN,   1, {0x1400003f, 0x00000000, 0, 0}},                 /* b 0x587eb0（thunk2 全部进入） */
    {0x587ec4, SMO_OBS_THUNK2_NULL, 1, {0xd65f03c0, 0x00000000, 0, 0}},                 /* ret（thunk2 [x0]==NULL 早退） */
    {0x8ae230, SMO_OBS_REG230, 0, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3}},   /* str x19,[sp,#-0x20]!; stp x29,x30,[sp,#0x10]; add x29,sp,#0x10; mov x19,x0 */
    {0x8ae258, SMO_OBS_REG258, 0, {0xf9400268, 0xf9000d13, 0xa9417bfd, 0xf84207f3}},   /* ldr x8,[x19]; str x19,[x8,#0x18]; ldp x29,x30,[sp,#0x10]; ldr x19,[sp],#0x20 */
    /* round21 注册调用者两门（字实测自 main.patched.bin；入口落点：FN_A←b@0x8b5e3c 唯一、
       FN_B←b@0x8b6bb0 唯一，窗内零落点——safety.py 全镜像扫描）。 */
    {0x8b65e0, SMO_OBS_FN_A,   0, {0xd10183ff, 0xa90167fa, 0xa9025ff8, 0xa90357f6}},   /* sub sp,sp,#0x60; stp x26,x25,[sp,#0x10]; stp x24,x23,[sp,#0x20]; stp x22,x21,[sp,#0x30] */
    {0x8b6920, SMO_OBS_FN_B,   0, {0xa9ba6ffc, 0xd11283e9, 0xa90167fa, 0xa9025ff8}},   /* stp x28,x27,[sp,#-0x60]!; sub x9,sp,#0x4a0; stp x26,x25,[sp,#0x10]; stp x24,x23,[sp,#0x20] */
    /* round24 解引用首字探针（notes 04-轮次/evidence_e1e2/probe_patch.md）：TP1/TP2 入口
       四字门（窗字实测自 main.patched.bin，safety 全镜像窗内零落点，见 probe_patch.md §5）；
       TP4 双单字门=0x964680 全部出口（round20 MQ_RET/MQ_ADV 同址同窗字先例；入口四字门
       因首字 cbz imm19 ±1MiB 物理跨不到 stub 页而不可重放，blscan 0x964680 零调用点）。 */
    {0x57f950, SMO_OBS_TP1,  0, {0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xf9400408}},   /* stp x20,x19,[sp,#-0x20]!; stp x29,x30,[sp,#0x10]; add x29,sp,#0x10; ldr x8,[x0,#8] */
    {0x8aef34, SMO_OBS_TP2,  0, {0xa9057bfd, 0x910143fd, 0x394c0008, 0xaa0103f5}},   /* stp x29,x30,[sp,#0x50]; add x29,sp,#0x50; ldrb w8,[x0,#0x300]; mov x21,x1 */
    {0x9646a4, SMO_OBS_TP4R, 1, {0xd65f03c0, 0x9100e000, 0, 0}},                     /* ret（0x964680 出口①：三路径合流 0x9646a0 mov w0,wzr 后到此后继 0x9646a8） */
    {0x9646ac, SMO_OBS_TP4A, 1, {0x17ffff81, 0xb940d408, 0, 0}},                     /* b 0x9644b0（0x964680 出口②：b.ne 路径 0x9646a8 add x0,#0x38 后尾跳；后继 0x9646b0 ldr w8,[x0,#0xd4]） */
    /* M7w (2026-11-07) 两门：
       ①SENDTO_REJ=W2::sendTo 的拒绝合流点 0x9646a0（三条拒绝路径 msg==0 的 cbz、
         state∉{3,4,5} 的 b.lo、dedup 的 cmp 等值落空，全部落到 0x9646a0 mov w0,wzr）。
         入口 0x964680 首字 cbz x1 imm19 ±1MiB 不可重放（stub 页 ~16MiB 外），函数体内
         每个 4 字窗都含 b.cond/b——入口门物理不可装；改挂合流点。tail=2 = 新「普通字」
         单字门：原义重放 mov w0,wzr（非分支、无 PC 相对字段，stub 内重放同语义），
         随后 b 回 0x9646a4（那里的 TP4R ret 门原样链式生效）。观察器从
         (msg=x1 经参3、state=[x0+0xd4]、last=[x0+0xc8]) 反推拒绝类（三者互斥、确定性），
         x4=LR 仍是 sendTo 的原始调用者返回址（0x964680..0x9646a0 之间无 bl）。
       ②BRC=3 槽扇出广播器 main+0x5b8d54 入口 4 字门（stp/add/ldr(寄存器)/mov 全部
         寄存器+立即数字形，重放免修正）；x0=this、x4=入口 LR=虚调用者的返回址
         （本函数只有虚调用，M7u 已证）。per-call 身份环在 dump 侧解 vtable 偏移。 */
    {0x9646a0, SMO_OBS_SENDTO_REJ, 2, {0x2a1f03e0, 0xd65f03c0, 0, 0}},               /* mov w0,wzr（word[1]=后继 ret 仅作安装期窗校验） */
    {0x5b8d54, SMO_OBS_BRC, 0, {0xa9017bfd, 0x910043fd, 0xf9401808, 0xaa0003f3}},    /* stp x29,x30,[sp,#0x10]; add x29,sp,#0x10; ldr x8,[x0,#0x30]; mov x19,x0 */
    /* M7a (2026-11-07) two gates (words verified against main.patched.bin; zero
       branch-into-window landings image-wide):
       (1) M7A_NULL = fnB 0x615cb0 entry (completion action: 0x9644b0(this+0x68,1,1) then
         state=2). Two call sites converge here: processor 0x615a3c (stores the result into
         [req+0x110] first) and the flavor-B finisher tail jump 0x616148. x0=req: observer
         reads [x0+0x60] state, [x0+0x110] result and the 24B name window at [x0+0xb8..]
         (one deref through the first word when it passes the guard) - NULL/non-NULL tally
         + 16-slot request ring.
       (2) M7A_REG = find 0x615e50 entry (registry linear scan; sole caller fnA+0x6156dc).
         x0 = the registry vector {cap@+0, base@+8, count@+0x10, stride 0x118}: first 4
         distinct x0 latched for the dump-side census. */
    {0x615cb0, SMO_OBS_M7A_NULL, 0, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3}},   /* str x19,[sp,#-0x20]!; stp x29,x30,[sp,#0x10]; add x29,sp,#0x10; mov x19,x0 */
    {0x615e50, SMO_OBS_M7A_REG,  0, {0xf81c0ff7, 0xa90157f6, 0xa9024ff4, 0xa9037bfd}},   /* str x23,[sp,#-0x40]!; stp x22,x21,[sp,#0x10]; stp x20,x19,[sp,#0x20]; stp x29,x30,[sp,#0x30] */
    /* M7b (2026-11-07) 17 gates (words + zero mid-window landings all verified offline by
       /tmp/m7b_safety.py, safety_bad=0):
       (1) M7B_BR = poster 0x7aca60 entry, same 4-word signature as fnB entry. x0 = batch
         object {[x0+8]=pump, [x0+0x18]=dirty flag}, x4 = LR = caller site (a gated bl site
         shows up as stub_k+0x54; dump side translates via the recorded stub offsets).
       (2) M7B_GTRUE = 0x61570c (tbz 0x615708 fall-through, TLS owner hit -> SYNCHRONOUS
         resolve path, bypasses registry/pump entirely) and M7B_GFALSE = 0x6157f0 (tbz
         target -> submit path allocslot+vsend+fnC). Windows: adrp x2 (relocated by
         replay_fix) + register/immediate words; the FALSE window's trailing bl 0x9564a0
         is also replay_fix-relocated.
       (3) ids 29..41 = the 13 direct bl callers of the poster, NEW tail=3 = single-bl
         gate: the veneer b->stub replaces ONLY the bl word; the stub replays the relocated
         bl (so the callee's LR = stub+0x54) and w[21] = b site+4 catches the callee's ret
         - call semantics identical to the original, caller registers untouched. Window =
         {bl, successor} (successor verified, untouched). */
    {0x7aca60, SMO_OBS_M7B_BR,     0, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3}},  /* poster entry */
    {0x61570c, SMO_OBS_M7B_GTRUE,  0, {0xf0006a48, 0xf943a908, 0x91004117, 0xf0006a48}},  /* adrp x8,0x1360000; ldr x8,[x8,#0x750]; add x23,x8,#0x10; adrp x8,0x1360000 */
    {0x6157f0, SMO_OBS_M7B_GFALSE, 0, {0x90006a88, 0xf9462d08, 0xf9400100, 0x940d0329}},  /* adrp x8,0x1365000; ldr x8,[x8,#0xc58]; ldr x0,[x8]; bl 0x9564a0 */
    {0x31219c, SMO_OBS_M7B_C0 + 0,  3, {0x94126a31, 0xf9400e60, 0, 0}},   /* bl poster; ldr x0,[x19,#0x18] (guard: 0x6c7cc0 tbz) */
    {0x4b057c, SMO_OBS_M7B_C0 + 1,  3, {0x940bf139, 0x320003e0, 0, 0}},   /* bl poster; mov w0,#1 (guard: flag 0x7acaa0 + reset 0x6154b0) */
    {0x4b0614, SMO_OBS_M7B_C0 + 2,  3, {0x940bf113, 0x320003e0, 0, 0}},   /* bl poster; mov w0,#1 (guard: 2x flag 0x7acaa0 + reset 0x6154b0) */
    {0x4b2d7c, SMO_OBS_M7B_C0 + 3,  3, {0x940be739, 0x320003e8, 0, 0}},   /* bl poster; mov w8,#1 (guard: byte [x19+0x32a]) */
    {0x4b79d0, SMO_OBS_M7B_C0 + 4,  3, {0x940bd424, 0x321e0fe1, 0, 0}},   /* bl poster; mov w1,#0x3c (guard: 0x6c7cc0 tbz) */
    {0x4b7d30, SMO_OBS_M7B_C0 + 5,  3, {0x940bd34c, 0xf9401660, 0, 0}},   /* bl poster; ldr x0,[x19,#0x28] (guard: 0x6c7cc0 tbz) */
    {0x4b814c, SMO_OBS_M7B_C0 + 6,  3, {0x940bd245, 0xf9401660, 0, 0}},   /* bl poster; ldr x0,[x19,#0x28] (guard: 0x6c7cc0 tbz) */
    {0x4b8dbc, SMO_OBS_M7B_C0 + 7,  3, {0x940bcf29, 0xf94003e1, 0, 0}},   /* bl poster; ldr x1,[sp] (unguarded, after 0x772160) */
    {0x4bdf38, SMO_OBS_M7B_C0 + 8,  3, {0x940bbaca, 0x14000002, 0, 0}},   /* bl poster; b 0x4bdf44 (unguarded, fields +0x28/+0x30 just written) */
    {0x4bdf9c, SMO_OBS_M7B_C0 + 9,  3, {0x940bbab1, 0x320003e0, 0, 0}},   /* bl poster; mov w0,#1 (guard: flag 0x7acaa0, clears +0x30) */
    {0x74b788, SMO_OBS_M7B_C0 + 10, 3, {0x940184b6, 0xa9437bfd, 0, 0}},   /* bl poster; ldp x29,x30,[sp,#0x30] (unguarded, function tail after 0x74bb80) */
    {0x74b9d4, SMO_OBS_M7B_C0 + 11, 3, {0x94018423, 0xa9447bfd, 0, 0}},   /* bl poster; ldp x29,x30,[sp,#0x40] (unguarded, function tail after 0x74bb80) */
    {0x74ba3c, SMO_OBS_M7B_C0 + 12, 3, {0x94018409, 0xa9417bfd, 0, 0}},   /* bl poster; ldp x29,x30,[sp,#0x10] (unguarded, after 0x74be70) */
    /* M7c (2026-11-07): fnA 0x6156b0 ENTRY 4-word gate. NOTE: the task's quoted words
       (f81e0ff3/a9017bfd/910043fd/aa0003f3 = the fnB/poster signature) do NOT match this
       site - main.patched.bin's real fnA entry prologue is sub sp,#0xf0; str x23,[sp,#0x70];
       stp x22,x21,[sp,#0x80]; stp x20,x19,[sp,#0x90] (caught offline by /tmp/m7c_safety.py,
       which verified the REAL words + zero branch-into-window landings; the runtime installer
       would have refused the task's words fail-soft). All four words are register/immediate
       forms, no replay_fix relocation needed. Observer: x0=this, x3=original x1 (name arg),
       x4=LR (fnA caller return); thread identity from runtime_cur_handle_tls (svc-side store). */
    {0x6156b0, SMO_OBS_M7C_FNA, 0, {0xd102c3ff, 0xf9003bf7, 0xa90857f6, 0xa9094ff4}},   /* fnA entry */
    /* M7e (2026-11-07): create-request function ENTRY main+0x746020 (4-word gate).
       Task gave ret@0x746014 (end of the previous fn, then udf/udf padding 0x746018/0x74601c);
       capstone says the real head is 0x746020 with a full-frame prologue
       stp x24,x23,[sp,#-0x40]! / stp x22,x21,[sp,#0x10] / stp x20,x19,[sp,#0x20] /
       stp x29,x30,[sp,#0x30] and the matching epilogue ending in ret@0x746194 - inside the
       task window 0x746018..0x746050. Body: stp xzr,[x0] (out obj), x1 = source obj (saved in
       x20), bl 0x9564a0 -> [x19+0xa8], stp xzr,[x19,#0xb0], bl 0x6153c0 (= fnA_caller, the
       single bridge to fnA), mov w0,#0x88 + bl 0x93e810 + bl 0x95d380. All four words are
       register/immediate forms => no replay_fix relocation. Static callers: exactly one
       (`bl 0x746020` @0x74897c); zero adrp+add materialisations; /tmp/m7e_safety.py
       safety_bad=0 (no branch lands inside the 0x10 window).
       Observer: x0 = output object, x4 = LR = the creator's return address (that is who
       constructed the request), thread = runtime_cur_handle_tls. */
    {0x746020, SMO_OBS_M7E_CRT, 0, {0xa9bc5ff8, 0xa90157f6, 0xa9024ff4, 0xa9037bfd}},   /* create-request entry */
    /* M7g (2026-11-07): the layer ABOVE the creator. Four gates, all verified against the
       shipped image (/tmp/m7f_safety.py; interior landings=0, no adrp+add materialisation,
       successor landings=0 for the two tail=2 sites):
       - 0x7488f0 = creator function REAL head (udf padding 0x7488e4..0x7488ec, prologue
         str x28,[sp,#-0x50]! + 3 stp). Exactly 3 static callers: b@0x74860c, bl@0x748898,
         b@0x748fe4. All four words are register/immediate => tail=0 4-word replay.
       - 0x7485a8 = entry-A tail block (NOT a function head: only landing is
         tbnz w0,#0,0x7485a8 @0x748548; the block restores the frame and tail-calls the
         creator at 0x74860c). First word ldr w8,[x21,#0x10]; window word 3 is b.cond, so
         tail=2 (plain-word replay + b off+4) is used. Successor 0x7485ac has no branch
         landings => no path bypasses the gate.
       - 0x748fb0 = entry-B fallback block of the fn headed 0x748e80 (entries cbz@0x748eac,
         b@0x748f88, cbz@0x748f90); it calls 0x748ff0 and tail-calls the creator at 0x748fe4.
         First word mov x0,x21 => tail=2. Successor 0x748fb4 has no branch landings.
       - 0x7461a0 = sibling ctor (x3!=0 arm of the creator), real head, 4 prologue words,
         single caller bl@0x74896c. Proves the 45/55 fork at gate level. */
    {0x7488f0, SMO_OBS_M7G_CRT, 0, {0xf81b0ffc, 0xa9015ff8, 0xa90257f6, 0xa9034ff4}},   /* creator fn head */
    {0x7485a8, SMO_OBS_M7G_A,   2, {0xb94012a8, 0xf94007e3, 0, 0}},                     /* entry-A block (tail=2) */
    {0x748fb0, SMO_OBS_M7G_B,   2, {0xaa1503e0, 0xaa1403e1, 0, 0}},                     /* entry-B block (tail=2) */
    {0x7461a0, SMO_OBS_M7G_SIB, 0, {0xa9bc5ff8, 0xa90157f6, 0xa9024ff4, 0xa9037bfd}},   /* sibling ctor */
    /* M7h（2026-11-08）——批循环 main+0x4be0c8 的「怎么停的」+ 查表函数 main+0x748e80 的调用者。
       两处分支点 0x4be2f8(cbnz 取消位) / 0x4be308(b.lt 耗尽) 本身是 PC 相对字、物理不可装
       （cbz/b.cond imm19 ±1 MiB，stub 页在 main+0x14d4xxx ≈ 21 MiB 外），故一律装在**分支的
       邻字或落点**，用 tail=2（单字重放 + b 回 off+4）保持语义全同：
         LOOK(0x4be2ec str) 紧跟 bl 0x7471f0，计「即将发起查表」；**不碰 bl 那一字**，所以
         0x7471f0 的返回址仍是 0x4be2f4，m7z 创建者门的 LR 口径与 M7F **逐字可比**（无污染）。
         RET(0x4be2f4 ldrb) = 查表已返回；CONT(0x4be2fc) = cbnz 落空。三者的差就是
         「未到达 / 到达且不跳 / 到达且跳」的互斥分解（口径见 threads.c 的 [m7h2] verdict 行）。
         EXH(0x4be30c) 是 b.lt 落空后的第一条；它同时收 b.le@0x4be1c8（任务数<=0）与
         b@0x4be260（loop1 耗尽）两条别的路，故配 EXH1(0x4be260) 与 TASKS(0x4be1cc) 做精确减法。
       全镜像 safety 扫描（b/bl/blret/b.cond/cbz/cbnz/tbz/tbnz 逐 word）：tail=0 两门窗内落点=0、
       adrp+add 物化站点=0、与既有 42 门零重叠 ⇒ safety_bad=0（logs/m7g_b1/p0_safety.txt）。 */
    {0x748e80, SMO_OBS_M7H_BQ,    0, {0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6}},   /* 0x748e80 真函数头（前有 udf@0x748e7c） */
    {0x4be0d0, SMO_OBS_M7H_ENT,   0, {0xd103c3ff, 0xa90a67fa, 0xa90b5ff8, 0xa90c57f6}},   /* 批循环真函数头（前有 udf@0x4be0c8/cc） */
    {0x4be1cc, SMO_OBS_M7H_TASKS, 2, {0x2a0003f5, 0xb4000594, 0, 0}},                     /* mov w21,w0（b.le 落空侧） */
    {0x4be284, SMO_OBS_M7H_L2E,   2, {0x9123f6d6, 0x91004338, 0, 0}},                     /* add x22,x22,#0x8fd（loop2 前导） */
    {0x4be294, SMO_OBS_M7H_HEAD,  2, {0x910063e0, 0x940bda92, 0, 0}},                     /* add x0,sp,#0x18（loop2 head） */
    {0x4be2ec, SMO_OBS_M7H_LOOK,  2, {0xf9001be8, 0x940a23c0, 0, 0}},                     /* str x8,[sp,#0x30]（bl 0x7471f0 前一字） */
    {0x4be2f4, SMO_OBS_M7H_RET,   2, {0x3940c668, 0x350000e8, 0, 0}},                     /* ldrb w8,[x19,#0x31]（查表返回处） */
    {0x4be2fc, SMO_OBS_M7H_CONT,  2, {0xb9005274, 0x11000694, 0, 0}},                     /* str w20,[x19,#0x50]（cbnz 落空） */
    {0x4be30c, SMO_OBS_M7H_EXH,   2, {0xb9405668, 0xb9005268, 0, 0}},                     /* ldr w8,[x19,#0x54]（b.lt 落空） */
    {0x4be260, SMO_OBS_M7H_EXH1,  1, {0x1400002b, 0x9100a3e0, 0, 0}},                     /* b 0x4be30c（loop1 耗尽，单字尾跳门） */
    /* M7L（2026-11-08）——派批泵 main+0x4bd8a0 与它的两个派批点。窗字全部实测自 main.patched.bin：
         PUMP  0x4bd8a0 = d10283ff a9065ff8 a90757f6 a9084ff4（4 字全存器/立即数 ⇒ tail=0；M7G P5 已核窗内落点=0）
         PDONE 0x4bdb7c = 320003e8（orr w8,wzr,#1 = mov w8,#1，置「已做完」标志）⇒ tail=2
         DP1   0x4bdb98 = aa1f03e4（mov x4,xzr，紧跟 bl@0x4bdb9c）⇒ tail=2
         DP2   0x4bdbcc = aa1f03e4（mov x4,xzr，紧跟 bl@0x4bdbd0）⇒ tail=2
         PEXIT 0x4bdbd4 = a9497bfd（ldp x29,x30）⇒ tail=2
       两只派批点**不装 bl 本身**（tail=3 单 bl 门会把 0x4be0d0 的返回址改成 stub+0x50，污染 m7h
       ENT 门的 LR 口径）；装在 bl 的前一字，只重放一个字，bl 的返回址仍是 0x4bdba0 / 0x4bdbd4。 */
    {0x4bd8a0, SMO_OBS_M7L_PUMP,  0, {0xd10283ff, 0xa9065ff8, 0xa90757f6, 0xa9084ff4}},   /* 泵函数头（LR=间接调用者） */
    {0x4bdb7c, SMO_OBS_M7L_PDONE, 2, {0x320003e8, 0x39018268, 0, 0}},                     /* 泵循环耗尽落空（置已做完） */
    {0x4bdb98, SMO_OBS_M7L_DP1,   2, {0xaa1f03e4, 0x9400014d, 0, 0}},                     /* 派批点① 前一字 */
    {0x4bdbcc, SMO_OBS_M7L_DP2,   2, {0xaa1f03e4, 0x94000140, 0, 0}},                     /* 派批点② 前一字 */
    {0x4bdbd4, SMO_OBS_M7L_PEXIT, 2, {0xa9497bfd, 0xa9484ff4, 0, 0}},                     /* 泵公共出口 */
    /* M7N（2026-11-08）两只只读门。窗字实测自 main.patched.bin（xxd 逐字）：
         G1 0x964260 = f81c0ff7 (str x23,[sp,#-0x40]!)  后继 a90157f6 (stp x22,x21,[sp,#0x10])
         G2 0x9646d0 = b940d408 (ldr w8,[x0,#0xd4])     后继 51000d09 (sub w9,w8,#3)
       两只都取 tail=2：只把**入口这 1 个字**换成 veneer b→stub，stub 里原义重放该字后
       b 回 off+4。因此 (a) 绝不覆盖 bl/blr（G1 的第一个 bl 在 +0x1c=0x96427c，G2 全身
       只有 b 尾跳、无 bl）；(b) off+4 之后每个字节都与出厂镜像逐字相同（含 G2 的
       b.hs@0x9646dc PC 相对字，本机制不重放它，故无需重定位）；(c) 无中间落点风险
       （只有入口 1 个字被换，跳到它的 b/blr 都照样进 stub）。 */
    {0x964260, SMO_OBS_M7N_G1, 2, {0xf81c0ff7, 0xa90157f6, 0, 0}},                        /* worker 工厂入口 */
    {0x9646d0, SMO_OBS_M7N_G2, 2, {0xb940d408, 0x51000d09, 0, 0}},                        /* PostToken 入口 */
    /* M7P（2026-11-08）一只门：worker Start main+0x963e50。
       **为什么是 tail=0 四字窗，而不是 M7N 那种 tail=2 单字门**：入口首字是
       stp x20,x19,[sp,#-0x20]!（**改 sp**）。tail=2 的语义是"stub 内原义重放该字"，
       而 stub 自己的帧（stp x29,x30,[sp,#-0x60]!）此时已经压栈 ⇒ 重放会把 sp 再压
       一次、函数体的栈布局全错。四字窗门在同一 stub 的 **epilogue 之后**重放 4 条原
       指令（此时 sp/寄存器已逐字复原 = 入口态）⇒ 语义全同。
       窗字实测自 main.patched.bin（/tmp/m7p_safety.py 逐字）：
         0x963e50 a9be4ff4 stp x20,x19,[sp,#-0x20]!
         0x963e54 a9017bfd stp x29,x30,[sp,#0x10]
         0x963e58 910043fd add x29,sp,#0x10
         0x963e5c b9400028 ldr w8,[x1]
       **窗内 4 条全是 stp/stp/add/ldr：无 bl、无 blr、无 PC 相对字**（所以 replay_fix
       无事可做）。全镜像分支落点扫描：命中 0x963e50 的只有 15 条 bl（与 M7O 的站点表
       逐条相同）；0x963e54/0x963e58/0x963e5c **零落点** ⇒ 换成 veneer 后没有分支会落进
       被换掉的 16 字节中间。函数体内第一条 bl 在 0x963e6c（+0x1c），本门一个字节都不碰。 */
    {0x963e50, SMO_OBS_M7P_START, 0, {0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xb9400028}},
    /* M7Q（2026-11-08）三只门。窗字全部实测自 main.patched.bin（/tmp/m7q_safety.py，
       三份镜像副本逐字一致；窗内 PC 相对字=0；窗内落点=0 ⇒ safety_bad=0）。
         WRAP 0x964540 = a9be4ff4 a9017bfd 910043fd 2a0703e8
             即 stp x20,x19,[sp,#-0x20]! / stp x29,x30,[sp,#0x10] / add x29,sp,#0x10 /
             mov w8,w7。四字全为寄存器+立即数字形 ⇒ tail=0 四字窗门（理由与 M7P 同：首字
             是 pre-index 存、改 sp，不能在 stub 帧里原义重放，必须在 epilogue 之后重放）。
             全镜像分支落点：命中 0x964540 的**只有 16 条 bl**（0x4582f4 0x4588d0 0x5b88f0
             0x5b89fc 0x5b8b00 0x615fe4 0x6821bc 0x6ae2a0 0x6ca334 0x6cc22c 0x6cdb7c
             0x6ced2c 0x7ac930 0x7acd18 0x7aed14 0x941b64），窗内 0x964544/48/4c **零落点**。
             观察器：x0 = this（新对象）、x1 = 第 2 实参、x8 = 第 4 实参、x4 = 入口 LR
             （= bl 站点 + 4 ⇒ 站点 = LR − 4）。
         FO   0x65b468 = 2a1703e1（mov w1,w23）后继 aa0003f6（mov x22,x0）。tail=2 普通字
             尾门：只换这 1 个字，stub 原义重放后 b 回 0x65b46c；**不碰 0x65b470 的 bl**。
         FOB  0x65b470 = 97fd74a8（bl 0x5b8710）后继 321c03e0（mov w0,#0x10）。tail=3 单 bl
             门（M7b）：重放重定位后的 bl，被调者 ret 落到 w[21]=b 回 0x65b474。 */
    {0x964540, SMO_OBS_M7Q_WRAP, 0, {0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0x2a0703e8}},
    {0x65b468, SMO_OBS_M7Q_FO,   2, {0x2a1703e1, 0xaa0003f6, 0, 0}},
    {0x65b470, SMO_OBS_M7Q_FOB,  3, {0x97fd74a8, 0x321c03e0, 0, 0}},
    /* M7S2（2026-11-08）三只门。窗字**本轮重新实测**（/tmp/m7s_safety.py：三份镜像副本逐字
       一致、窗内 PC 相对字=0、窗内落点=0、与既有 63 个站表行零重叠）：
         G1 0x681b30 = f81c0ff7 a90157f6 a9024ff4 a9037bfd
              str x23,[sp,#-0x40]! / stp x22,x21,[sp,#0x10] / stp x20,x19,[sp,#0x20] /
              stp x29,x30,[sp,#0x30]。首字是 pre-index 存（改 sp）⇒ **tail=0 四字窗**
              （理由与 M7P/M7Q 同：stub 帧已压栈，不能在帧内原义重放改 sp 的字）。
              落点：命中 0x681b30 的**只有 1 条 bl = 0x74cae0**（= G3 所在处，本轮独立复核）；
              0x681b34/38/3c 零落点。门后 +0x10 起 4 字（0x681b40..0x681b4c = add x29,sp,#0x30 /
              mov x19,x0 / mov w0,#8 / mov w20,w1）作运行期 after 校验，本门一个字节都不碰。
         G2 0x74ca00 = a9bd57f6 a9014ff4 a9027bfd 910083fd
              stp x22,x21,[sp,#-0x30]! / stp x20,x19,[sp,#0x10] / stp x29,x30,[sp,#0x20] /
              add x29,sp,#0x20。同样 pre-index ⇒ tail=0 四字窗。落点：8 条 bl（见枚举注）；
              0x74ca04/08/0c 零落点。
         G3 0x74cadc = 2a1303e1（mov w1,w19）后继 97fcd414（bl 0x681b30，**只作窗校验、不换**）
              ⇒ tail=2 普通字尾门：只换 0x74cadc 这 1 个字，stub 原义重放 mov w1,w19 后
              b 回 0x74cae0 —— 那条 bl 逐字还是出厂字（运行期 code 行会打印它证明）。
              0x74cadc 全镜像**零落点**（只有直执行流经它），0x74cae0 零落点。 */
    {0x681b30, SMO_OBS_M7S2_G1, 0, {0xf81c0ff7, 0xa90157f6, 0xa9024ff4, 0xa9037bfd}},
    {0x74ca00, SMO_OBS_M7S2_G2, 0, {0xa9bd57f6, 0xa9014ff4, 0xa9027bfd, 0x910083fd}},
    {0x74cadc, SMO_OBS_M7S2_G3, 2, {0x2a1303e1, 0x97fcd414, 0, 0}},
    /* M7T2（2026-11-08）**整族九个头一起装门**。窗字**本轮逐门重扫**（/tmp/m7t2_safety.py：
       三份镜像副本逐字一致、窗内 PC 相对字=0、窗内中间字落点=0、与既有 66 个站表行零重叠）。
       九门**全部 tail=0 四字窗**，理由逐门成立：8 个首字是 pre-index 存（`str/stp …,[sp,#-imm]!`），
       第 6 个（0x4a1100）首字是 `sub sp,sp,#0x180` —— **九个首字全部改写 sp**。tail=2 的语义是
       "stub 内原义重放该字"，而 stub 自己的帧已压栈 ⇒ 会二次压栈、函数体栈布局全错；
       tail=0 在 stub epilogue **之后**重放 4 条（此时 sp/寄存器已逐字复原 = 入口态）。
       分支落点（全镜像 b/bl/b.cond/cbz/cbnz/tbz/tbnz 全族）：H2 命中 1 条 bl=0x449970、
       H6 命中 1 条 bl=0x4a10d8，其余 **7 门零落点**（= 派活单说的"7 个零静态调用者"，
       本轮独立复核逐个吻合）；九个门的 3 个中间字**全部零落点**。
       函数体内第一条 bl 都在窗外（+0x14 起），本九门一个 bl/blr 字节都不碰。 */
    {0x445640, SMO_OBS_M7T2_H1, 0, {0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6}},   /* demo world-move 场景 */
    {0x447b70, SMO_OBS_M7T2_H2, 0, {0xf81b0ffc, 0xa9015ff8, 0xa90257f6, 0xa9034ff4}},   /* demo 开场/坠机场景 */
    {0x449c50, SMO_OBS_M7T2_H3, 0, {0xf81b0ffc, 0xa9015ff8, 0xa90257f6, 0xa9034ff4}},   /* demo 结局场景 */
    {0x44b720, SMO_OBS_M7T2_H4, 0, {0xfc190fe8, 0xa9016ffc, 0xa90267fa, 0xa9035ff8}},   /* StageScene 侧 */
    {0x46e7a0, SMO_OBS_M7T2_H5, 0, {0xfc190fe8, 0xa9016ffc, 0xa90267fa, 0xa9035ff8}},   /* StageSceneDefault 侧 */
    {0x4a1100, SMO_OBS_M7T2_H6, 0, {0xd10603ff, 0x6d1123e9, 0xa9126ffc, 0xa91367fa}},   /* WorldMap/WorldSelect（首字 sub sp） */
    {0x4a6b10, SMO_OBS_M7T2_H7, 0, {0xf81b0ffc, 0xa9015ff8, 0xa90257f6, 0xa9034ff4}},   /* Title/GameLoad 侧 */
    {0x4a8820, SMO_OBS_M7T2_H8, 0, {0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6}},   /* AocControllerGuide 侧 */
    {0x4af0a0, SMO_OBS_M7T2_H9, 0, {0xa9bc5ffc, 0xa90157f6, 0xa9024ff4, 0xa9037bfd}},   /* DemoWorldWarpHole + 资源线程 */
    /* M7U2（2026-11-08）**场景派发枢纽**三门。窗字本轮逐门重扫（/tmp/m7u_safety.py）：
       三份镜像副本逐字一致、被换字内 PC 相对字 = 0、全镜像分支落点（b/bl/b.cond/cbz/cbnz/
       tbz/tbnz 全族）**首字与窗内中间字全为 0**、与既有 75 个站表行零重叠（OVERLAP=[]）。
       A 0x7acd50 tail=0 四字窗：窗内 4 条是 stp/stp/add/ldr（无 bl/blr、无 PC 相对字段），
         首字 pre-index 存改 sp ⇒ 只能 tail=0（在 stub epilogue 之后重放，sp 已复原）。
       B 0x7acd94 tail=2 普通字门：word[0]=ldr x0,[x20]（非分支/非 PC 相对），
         word[1]=后继 ldr x8,[x0] **只作安装期窗校验、不换**。
       C 0x7acd70 tail=2 普通字门：word[0]=ldr x0,[x8]，word[1]=**后继 bl 0x9564f0
         只作校验、不换**（那条 bl 运行期逐字回读证明未动）。 */
    {0x7acd50, SMO_OBS_M7U2_A, 0, {0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xf9400401}},   /* stp x20,x19,[sp,#-0x20]!; stp x29,x30,[sp,#0x10]; add x29,sp,#0x10; ldr x1,[x0,#8] */
    {0x7acd94, SMO_OBS_M7U2_B, 2, {0xf9400280, 0xf9400008, 0, 0}},   /* ldr x0,[x20]（后继 ldr x8,[x0] 仅校验） */
    {0x7acd70, SMO_OBS_M7U2_C, 2, {0xf9400100, 0x9406a5df, 0, 0}},   /* ldr x0,[x8]（后继 bl 0x9564f0 仅校验，不换） */
    /* M7V（2026-11-08）八门。窗字本轮逐门重扫（/tmp/m7v_safety.py → /tmp/m7v_safety_out.txt）：
       三份镜像副本逐字一致（md5 89d568fd…）、被换字内 PC 相对字 = 0（X8）/ 仅 adrp（R1..R7，可重定位）、
       全镜像分支落点（b/bl/b.cond/cbz/cbnz/tbz/tbnz 全族）**首字与窗内中间字全为 0**、
       与既有 78 个站表行零重叠（OVERLAP=[]）⇒ WINDOW_BAD=0 / INTERIOR_TOTAL=0。
       X8 0x7acd8c tail=2 单字门：word[0]=`ldr x8,[x8]`（非分支/非 PC 相对），
         word[1]=后继 `blr x8` **只作安装期窗校验、不换**（0x7acd90 运行期逐字回读证明）。
       R1..R7 tail=0 四字窗：七个首字全部改写 sp ⇒ 不能在 stub 帧内原义重放，只能 tail=0
         （在 stub epilogue 之后重放，此时 sp/寄存器已逐字复原 = 入口态）。 */
    {0x7acd8c, SMO_OBS_M7V_X8, 2, {0xf9400108, 0xd63f0100, 0, 0}},   /* ldr x8,[x8]（后继 blr x8 仅校验，不换） */
    {0x4a6a90, SMO_OBS_M7V_R1, 0, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x90007608}},   /* str x19,[sp,#-0x20]!; stp x29,x30; add x29; adrp x8,#0x1366000 */
    {0x46e770, SMO_OBS_M7V_R2, 0, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3}},   /* str x19,[sp,#-0x20]!; stp x29,x30; add x29; mov x19,x0 */
    {0x445590, SMO_OBS_M7V_R3, 0, {0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0x90007908}},   /* stp x20,x19,[sp,#-0x20]!; stp x29,x30; add x29; adrp x8,#0x1365000 */
    {0x447ae0, SMO_OBS_M7V_R4, 0, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xd00078e8}},   /* str x19,[sp,#-0x20]!; stp x29,x30; add x29; adrp x8,#0x1365000 */
    {0x449bc0, SMO_OBS_M7V_R5, 0, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x900078e8}},   /* str x19,[sp,#-0x20]!; stp x29,x30; add x29; adrp x8,#0x1365000 */
    {0x44b6f0, SMO_OBS_M7V_R6, 0, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3}},   /* str x19,[sp,#-0x20]!; stp x29,x30; add x29; mov x19,x0 */
    {0x4af020, SMO_OBS_M7V_R7, 0, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xf00075a8}},   /* str x19,[sp,#-0x20]!; stp x29,x30; add x29; adrp x8,#0x1366000 */
    /* M7W2（2026-11-08）一门。窗字**本轮逐字重扫**（/tmp/m7w2_safety.py → /tmp/m7w2_safety_out.txt）：
       三份镜像副本 md5 全等（89d568fd…）、word[0] 非分支非 PC 相对（安装期 branchy=false）、
       全镜像分支落点（b/bl/b.cond/cbz/cbnz/tbz/tbnz 全族）**首字 0 / 窗内中间字 0**、
       与既有 86 个站表行**零重叠**（OVERLAP=[]）⇒ WINDOW_BAD=0 / INTERIOR_TOTAL=0。
       AD 0x7724d0 tail=2 单字门：word[0]=`ldp x8,x2,[x0,#8]`（非分支/非 PC 相对），
         word[1]=后继 `ldr x9,[x0,#0x18]` **只作安装期窗校验、不换**；
         word[2]/word[3] 记在这里只为「窗内还有哪两条」可读，安装期不校验也不换。 */
    {0x7724d0, SMO_OBS_M7W2_AD, 2, {0xa9408808, 0xf9400c09, 0x8b890508, 0x36000069}},   /* ldp x8,x2,[x0,#8]; ldr x9,[x0,#0x18]; add x8,x8,x9,asr#1; tbz w9,#0 */
    /* M7X2（2026-11-08）两门。窗字**本轮逐字重扫**（/tmp/m7x2_safety.py → /tmp/m7x2_safety_out.txt）：
        三份镜像副本 md5 全等（89d568fd…）、两门窗内 4 字**全部无 PC 相对字段**（pcrel=[None,None,None,None]）、
        全镜像分支落点（b/bl/b.cond/cbz/cbnz/tbz/tbnz 全族）对两门 **FIRST=0 / INTERIOR=0**
        （INTERIOR_TOTAL=0）、与既有 87 行站表**零重叠**（OVERLAP=[]，两门都是）、两门前一字（off−4）
        都是 `00000000` ⇒ WINDOW_BAD=0。
        G1 0x74c9e0 tail=2 单字门：word[0]=`ldr x8,[x0]`（非分支、非 PC 相对、安装期 branchy=false），
          原义在 stub 内重放后 `b` 回 0x74c9e4；word[1]=`ldr x2,[x8,#0x18]` **只作安装期窗校验、不换**；
          0x74c9e8 的 `br x2` 在窗外、一个字节都不碰（运行期逐字回读 d61f0040 证明）。
        G2 0x772500 tail=0 四字窗：首字 `str x19,[sp,#-0x20]!` **改写 sp** ⇒ 只能 tail=0（4 条在 stub
          epilogue 之后重放 = 入口态）；4 条全是寄存器/立即数字形（replay_fix 离线逐条验证可接受，
          无 adrp/无 bl）⇒ 不需重定位。0x772510..0x77251c 在窗外（运行期逐字回读证明未动），
          0x7724f0 的 `br x2`（M7W2 的适配器尾跳）也不碰（同一行里回读）。 */
    {0x74c9e0, SMO_OBS_M7X2_G1, 2, {0xf9400008, 0xf9400d02, 0xd61f0040, 0x00000000}},   /* ldr x8,[x0]（后继 ldr x2,[x8,#0x18] 与 br x2 仅窗校验/根本不在窗内） */
    {0x772500, SMO_OBS_M7X2_G2, 0, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3}},   /* str x19,[sp,#-0x20]!; stp x29,x30,[sp,#0x10]; add x29,sp,#0x10; mov x19,x0 */
    /* M7Y2：两门都 tail=0 四字窗（首字都改写 sp，不能在 stub 帧里重放）；窗内 4 字全寄存器/立即数，
       无 adrp/无 b/bl/b.cond/cbz/tbz/adr/ldr-literal（/tmp/m7y2_safety.py 逐条核过）。 */
    {0x44adb0, SMO_OBS_M7Y2_G1, 0, {0xd10543ff, 0xa9115ffc, 0xa91257f6, 0xa9134ff4}},   /* sub sp,sp,#0x150; stp x28,x23,[sp,#0x110]; stp x22,x21,[sp,#0x120]; stp x20,x19,[sp,#0x130] */
    {0x44af80, SMO_OBS_M7Y2_G2, 0, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3}},   /* str x19,[sp,#-0x20]!; stp x29,x30,[sp,#0x10]; add x29,sp,#0x10; mov x19,x0 */
    /* M7Z2：5 门（99..103）。safety = /tmp/m7z2_safety.py → /tmp/m7z2_safety_out.txt（逐门三镜像逐字、
       窗内 PC 相对、replay_fix、全镜像分支落点 FIRST/INTERIOR、静态 bl/b、与既有 91 行站表 ±0x20 零重叠、
       五门**字节零重叠**）。G1/G3/G5 首字都改写 sp ⇒ tail=0 四字窗（在 stub epilogue 之后重放 = 入口态）；
       G2/G4 装在非分支单字上 ⇒ tail=2（原义重放 1 字后 b 回 off+4，后继字一个字节都不动）。 */
    {0x6c7d30, SMO_OBS_M7Z2_G1, 0, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xf9400008}},   /* str x19,[sp,#-0x20]!; stp x29,x30,[sp,#0x10]; add x29,sp,#0x10; ldr x8,[x0] */
    {0x4b8e98, SMO_OBS_M7Z2_G2, 2, {0xa9417bfd, 0xf84207f3, 0xd65f03c0, 0xa9418660}},   /* ldp x29,x30,[sp,#0x10]（后继 ldr x19,[sp],#0x20 / ret / ldp x0,x1 只作窗校验、不换） */
    {0x4b8ea4, SMO_OBS_M7Z2_G3, 0, {0xa9418660, 0x940ae502, 0xf9401260, 0xf9400008}},   /* ldp x0,x1,[x19,#0x18]; bl 0x7722b0(重定位); ldr x0,[x19,#0x20]; ldr x8,[x0] */
    {0x4b8e88, SMO_OBS_M7Z2_G4, 2, {0x52804381, 0xaa1303e0, 0x94083ba8, 0x36000080}},   /* mov w1,#0x21c（后继 mov x0,x19 / bl / tbz 只作窗校验、不换） */
    {0x4b8e20, SMO_OBS_M7Z2_G5, 0, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3}},   /* str x19,[sp,#-0x20]!; stp x29,x30,[sp,#0x10]; add x29,sp,#0x10; mov x19,x0 */
    /* M8B：2 门（104/105）。safety = /tmp/m8b_work/s_safety.py → /tmp/m8b_work/safety_out.txt
       （镜像 md5 89d568fd…；两门窗四字与镜像逐字相同；窗内 PC 相对字 **0/4**；replay_fix 无需重定位；
       全镜像分支落点 FIRST=15/1（= 静态入口数，落在 veneer 首字上，正常）、**INTERIOR=0**；
       与既有 96 行站表 ±0x20 **零重叠**）。两门首字都改写 sp ⇒ tail=0 四字窗。 */
    {0x6c7750, SMO_OBS_M8B_G1, 0, {0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xb9401808}},   /* stp x20,x19,[sp,#-0x20]!; stp x29,x30,[sp,#0x10]; add x29,sp,#0x10; ldr w8,[x0,#0x18] */
    {0x4b8d30, SMO_OBS_M8B_G2, 0, {0xd100c3ff, 0xa9014ff4, 0xa9027bfd, 0x910083fd}},   /* sub sp,sp,#0x30; stp x20,x19,[sp,#0x10]; stp x29,x30,[sp,#0x20]; add x29,sp,#0x20 */
    /* M8D：2 门（106/107 = 两只孪生**绑表点**）。safety = /tmp/m8d_work/s_safety.py → safety_out.txt
       （镜像 md5 89d568fd…；两门窗四字与镜像逐字相同；窗内 PC 相对 **1/4**（第 4 字 adrp，replay_fix
       按同绝对页重算）；全镜像分支落点 FIRST=0 / **INTERIOR=0**、静态调用者 **0**；
       与既有 98 行站表 ±0x20 **零重叠**）。两门首字都改写 sp ⇒ tail=0 四字窗。 */
    {0x4b8cb0, SMO_OBS_M8D_G1, 0, {0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0x900070d4}},   /* stp x20,x19,[sp,#-0x20]!; stp x29,x30,[sp,#0x10]; add x29,sp,#0x10; adrp x20,0x12d0000（→ +0xfe8 表） */
    {0x4b7380, SMO_OBS_M8D_G2, 0, {0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xb00070d4}},   /* 同上 prologue; adrp x20,0x12d0000（→ +0x6d8 表） */
    /* M8E2：4 门（108..111 = 追第一个分叉）。safety = /tmp/m8e2_work/safety.py
       （镜像 md5 89d568fd…；四门窗字与镜像逐字相同；110 窗内 PC 相对 **0/4**；
       分支落点 FIRST=4（全落在函数入口，正常）/ INTERIOR=0；与既有 100 行站表 ±0x20 **零重叠**）。
       108/109 用 **tail=3 单 bl 门**（只换 bl 一个字）——因为 0x4b4bbc 起的四字窗里有 `tbz`，
       replay_fix 拒装 PC 相对分支族，四字窗物理上不可用；111 用 **tail=1 单 ret 门**。 */
    {0x4b4bc0, SMO_OBS_M8E2_G1, 3, {0x94007804, 0x36000b40, 0, 0}},   /* bl 0x4d2bd0（判断点前一字）；后继 tbz w0,#0 只作窗校验 */
    {0x4b4bb8, SMO_OBS_M8E2_G2, 3, {0x94084bda, 0xf9405e60, 0, 0}},   /* bl 0x6c7b20 setNerve(0x12d0420)；后继 = 0x4b4bbc 分叉入口（ldr x0,[x19,#0xb8]） */
    {0x59ac30, SMO_OBS_M8E2_G3, 0, {0xd10143ff, 0xa9034ff4, 0xa9047bfd, 0x910103fd}},   /* sub sp,sp,#0x50; stp x20,x19,[sp,#0x30]; stp x29,x30,[sp,#0x40]; add x29,sp,#0x40 */
    {0x59accc, SMO_OBS_M8E2_G4, 1, {0xd65f03c0, 0xf81d0ff5, 0, 0}},   /* ret（返回 x0 观测点）；后继 = 下一函数首字，只作窗校验 */
    /* M10A（2026-11-08）：1 门（112 = A 支入口 = tbz 落穿侧首字）。safety = /tmp/m10a_work/dis2.py
       （镜像 md5 89d568fd…；窗字与镜像逐字相同；全镜像转 0x4b4bc8 的分支 **0 条**；
       与既有 100 行站表 ±0x20 零重叠；word[0] 非分支/非 PC 相对 ⇒ tail=2）。 */
    {0x4b4bc8, SMO_OBS_M10A_A, 2, {0xf9400288, 0xf9401108, 0, 0}},   /* ldr x8,[x20]；后继 ldr x8,[x8,#0x20] 只作窗校验 */
    /* M15A 门 113：main+0x988a20 = NVN submit 入口（str x21,[sp,#-0x30]!; stp x20,x19,[sp,#0x10];
       stp x29,x30,[sp,#0x20]; add x29,sp,#0x20）。四个 u32 与镜像逐字相同（verify113.py 读出）。 */
    {0x988a20, SMO_OBS_M15A_X30, 0, {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd}},
    /* M16A 门 114：main+0x991a30 = "GORP." trampoline 入口（stp x24,x23,[sp,#-0x40]!;
       stp x22,x21,[sp,#0x10]; stp x20,x19,[sp,#0x20]; stp x29,x30,[sp,#0x30]）。
       四个 u32 与镜像逐字相同（verify114.py 读出；bytes f8 5f bc a9 f6 57 01 a9 f4 4f 02 a9 fd 7b 03 a9）。
       与上面门 113（0x988a20）相距 0x9010，±0x20 无重叠。首字改写 sp ⇒ tail=0。 */
    {0x991a30, SMO_OBS_M16A_GORP, 0, {0xa9bc5ff8, 0xa90157f6, 0xa9024ff4, 0xa9037bfd}},
    /* M17A 门 115：main+0x98b4b0 = 2D 四边形记录叶 fn 0x98b490 的 +0x20（mov x19,x2; mov x20,x1;
       mov x21,x0; add x23,x0,#0x160）。四个 u32 与镜像逐字相同（/tmp/m17a_work/callers.py 读出；
       bytes f3 03 02 aa f4 03 01 aa f5 03 00 aa 17 80 05 91）。
       前一字 0x98b4ac = 9100c3fd（add x29,sp,#0x30）不触碰；窗内 PC 相对 0；落点 0/0；
       与上面门 114（0x991a30）相距 0x6580，±0x20 无重叠。首字非改 sp ⇒ 仍 tail=0 四字窗。 */
    {0x98b4b0, SMO_OBS_M17A_QUAD, 0, {0xaa0203f3, 0xaa0103f4, 0xaa0003f5, 0x91058017}},
    /* M18A 门 116：main+0x8fea2c = 2D 上色绘制助手入口（stp x24,x23,[sp,#-0x40]!;
       stp x22,x21,[sp,#0x10]; stp x20,x19,[sp,#0x20]; stp x29,x30,[sp,#0x30]）。
       bytes f8 5f bc a9 f6 57 01 a9 f4 4f 02 a9 fd 7b 03 a9，与镜像逐字相同。
       前一字 0x8fea28 = d61f00c0（br x6，上一函数末字，非 PC 相对）；窗内 PC 相对 0；
       落点 FIRST=8 / INTERIOR=0；与站表最近 0x8b6920（距 0x4810c）。首字改写 sp ⇒ tail=0。 */
    {0x8fea2c, SMO_OBS_M18A_UIFILL, 0, {0xa9bc5ff8, 0xa90157f6, 0xa9024ff4, 0xa9037bfd}},
    /* M18A 门 117：main+0x9106fc = 2D 面板/文字渲染器入口（sub sp,sp,#0x60; str x25,[sp,#0x10];
       stp x24,x23,[sp,#0x20]; stp x22,x21,[sp,#0x30]）。
       bytes ff 83 01 d1 f9 0b 00 f9 f8 5f 02 a9 f6 57 03 a9，与镜像逐字相同。
       前一字 0x9106f8 = d65f03c0（ret，上一函数末字，非 PC 相对）；窗内 PC 相对 0；
       落点 FIRST=3 / INTERIOR=0；与站表最近 0x963e50（距 0x53754）。首字改写 sp ⇒ tail=0。 */
    {0x9106fc, SMO_OBS_M18A_PANEL, 0, {0xd10183ff, 0xf9000bf9, 0xa9025ff8, 0xa90357f6}},
    /* M19A 门 118：main+0x916a28 = 2D 批处理器 fn main+0x915dfc 的 +0xc2c（ldr d0,[x8,#0x700];
       ldr w8,[x8,#0x708]; fmov d1,x8; mov v0.d[1],v1.d[0]），即"RGB 从状态字段读出来、马上
       灌进 [x19+0x320]"的那一条。四字与镜像逐字相同（/tmp/m19a_work/chk118.py 读出；
       bytes 00 81 43 fd / 08 09 47 b9 / 01 01 67 9e / 20 04 18 6e）。
       前一字 0x916a24 = ad0f0a64（stp q4,q2,[x19,#0x1e0]，寄存器+立即数，非 PC 相对、非分支）；
       窗内 PC 相对 0/4；分支落点 FIRST=0 / INTERIOR=0（全镜像扫）；
       与站表既有 110 行 ±0x20 **零重叠**；FDE 起点 0x915dfc，size 0x1070。首字不改 sp ⇒ tail=0 四字窗。 */
    {0x916a28, SMO_OBS_M19A_RGB, 0, {0xfd438100, 0xb9470908, 0x9e670101, 0x6e180420}},
};
/* round21：E1 快照/变迁史结构随 E1 门退役删除（26k/s 洪流全容器快照是现存最大每命中成本）。
   tick 基准对与 smo_obs_tick 保留（REG230/FN_A/FN_B/BC310 首末见 ts 换算仍用）。 */
static uint64_t g_obs_tick0, g_obs_ns0; static int g_obs_tick0_set;
static inline uint64_t smo_obs_tick(void) { uint64_t t; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t)); return t; }
/* round17 ①：KICK1 门（0x8ad1f4）加挂 kick 三条件快照——DRAIN-LOOP 门随排空函数 0x8abf70
   永不被调（round16 ENTER=LOOP=0）而永不命中，三条件改在 kick 真入口取：门点原参
   x0=kick this（0x8ad1fc mov x19,x0 的原值）、原 x1=w1（stub 参4 抄走，movz 覆盖前）。
   [this+0xf9]（1B）+[this+0x20]（8B）＝kick 0x8ad208/0x8ad278 自己要读的字段（护栏
   <2^32 拒读 + 指针域 0x1_0000_0000..0x8000_0000_0000，DRAIN-LOOP 快照同款范式）。
   分布标量按 kick 三分支交叉计数（0x8ad200 cmp w1,#1 / b.lt；0x8ad208 ldrb+cmp #2）：
   n_lt1=w1<1 直虚调、n_ge1_f9eq2=w1≥1∧f9==2 直虚调、n_ge1_f9ne2=w1≥1∧f9≠2 动画路径、
   n_o20z=[this+0x20]==0（动画路径 0x8ad28c cbz 跳过 bl 0x8ac800 的条件）。首见一次定格
   {this,w1,f9,o20,tick}，末见逐字段原子覆盖 {w1,f9,o20}——216 次真 kick 的分支去向
   一屏可读。多线程 kick 并发：标量全原子 fetch_add；首见幂等竞争下良性覆盖（观测用途）。 */
static struct {
    _Atomic unsigned set;
    uint64_t this_, w1, f9, o20, tick;                         /* 首见（set=1 定格） */
    _Atomic uint64_t last_w1, last_f9, last_o20;               /* 末见（逐字段原子） */
    _Atomic uint64_t n_lt1, n_ge1_f9eq2, n_ge1_f9ne2, n_o20z;  /* 三分支交叉分布（round17） */
    /* round18 ①：首 16 次逐次快照环 + 六边际计数。ring_k=原子占位计数（fetch_add 认领
       槽位，认领序号<16 才写槽——多线程 kick 并发下每命中恰好占一槽，不覆盖；槽内
       plain 64 位写，与 g_e1 环同款观测语义）。边际与交叉互补：f9/o20 的分布在
       w1==0 直虚调路径上也计数（交叉表只在 w1>=1 分支里分 f9）。 */
    struct { uint64_t this_, w1, f9, o20; } ring[16];
    _Atomic uint64_t ring_k;
    _Atomic uint64_t n_w1z, n_w1nz, n_f9eq2, n_f9ne2, n_o20z2, n_o20nz;
} g_k1;
/* round17 ②（round18 补 x1，round21 增 LR）：BC310 首见 {x0, x1, lr, tick} + 末见逐命中
   覆盖 {lr, x1, tick}（g_obs_x0[gate] 承担末见 x0）+ 首 64 LR 采样环（fetch_add 认领槽位，
   无 CAS——round20 已裁决 45807×CAS 是 boot 期扰动源；采样环每命中 1 次原子加，首 64 次
   各一次定长写）。x0==0 计 xzero。round19 ③ 的 first-16 唯一 x0 环仍在（HEAVY 编译开关内）。 */
static struct { _Atomic unsigned set; uint64_t x0, x1, lr0, tick;
    _Atomic uint64_t last_lr, last_x1, last_tick;
    _Atomic uint64_t lr[64], lr_k;
    _Atomic uint64_t xv[16], xc[16], xover, xzero;
} g_bc;
/* round21 三门首见记录（REG230/FN_A/FN_B 各一槽）：{x0, x1(观察器参4=门点原 x1), tick}。
   REG230：x0=容器槽（调用点 sp+8 栈槽）、x1=被注册句柄（节点-0x1c0 族）；
   FN_A(0x8b65e0)：x0=时基参、x1=版本容器（[x1+0x14]∈[0,4] 决定 5 臂）；
   FN_B(0x8b6920)：x0=容器本体（count@+0x40/arr@+0x48）、x1=第二实参。
   FN_A/FN_B 与 REG230 首见 tick 相减＝调用者函数 vs 注册动作时序。 */
static struct { _Atomic unsigned set; uint64_t x0, x1, tick; } g_fs[3];
/* round24 解引用首字探针四门（TP1/TP2/TP4R/TP4A）：命中时读 [x0] 首 8 字节（vptr 候选）。
   指针域护栏=KICK1 同款（0x1_0000_0000..0x8000_0000_0000，越界只计 n_bad 不读）。
   首见一次定格 {x0,deref,tick}；末见逐命中原子覆盖 last_deref；ok/bad 计数。
   每命中成本=1 次护栏内 8B 读+少量原子，与 BC310 last 覆盖同级——不设 HEAVY 门控。 */
static struct { _Atomic unsigned set; uint64_t x0, deref, tick;
                _Atomic uint64_t last_deref, n_ok, n_bad; } g_tp[4];
/* round25 TP5：DecompThread 出口状态字序列（不加新门，挂现有 TP4R/TP4A 观察器）。
   命中时在护栏内读 [x0+0xd4]（u32 状态字）与 [x0+0xc8]（u64 参数）；只记前 8 次
   （槽 0-7）与第 10000 次（槽 8，fetch_add 序号 9999 认领）。k=原子序号计数。
   TP4R 点 x0=w0 返回值 0（0x9646a0 mov w0,wzr 已写）→护栏必拒，state=val=0 且
   x0=0 可辨「拒读」；TP4A 点 x0=已 +0x38 的后继节点，即 0x9646b0 ldr [x0,#0xd4]
   下轮要读的同一对象。q: 0=TP4R 1=TP4A。 */
static struct { _Atomic uint64_t k;
                struct { uint64_t x0, state, val; } r[9]; } g_tp5[2];
/* ---- M7w (2026-11-07) 观察器侧状态（全部 plain 定长写 + 原子计数；打印在 dump 侧） ----
   开关 smo_m7w：threads.c 每 2 s 刷新 g_m7w_on（C 默认 0；echo 0 一键回退）。开关为 0 时
   新门与既有门一样只做 1 次宽松原子加（hits/x0 记录），M7w 数据采集全部短路。 */
_Atomic int g_m7w_on;
/* SENDTO_REJ（0x9646a0 拒绝合流）：拒绝类分桶 + 末 20 次环。类从 (msg,state,last) 反推：
   msg==0 -> msg0；state∉[3,5] -> state；last(=[x0+0xc8])==msg -> dedup；否则 odd（理论不可达，
   出现即说明类判据有洞）。护栏同 TP 族（0x1_0000_0000..0x8000_0000_0000 之外拒读、记 0）。 */
static struct { _Atomic uint64_t n_msg0, n_state, n_dedup, n_odd, k;
                struct { uint64_t tick, this_, msg, state, last, lr; } r[20]; } g_m7w_rej;
/* TP4A（0x9646ac 放行出口）挂账：this=x0-0x38、msg=x3、state=[this+0xd4]、lr=x4。 */
static struct { _Atomic uint64_t n, k;
                struct { uint64_t tick, this_, msg, state, lr; } r[20]; } g_m7w_pass;
/* BRC（广播器 0x5b8d54 入口）：per-call {tick, this, 入口 LR} 环（200 行封顶，M7w 派活单限额）。 */
static struct { _Atomic uint64_t k;
                struct { uint64_t tick, this_, lr; } r[200]; } g_m7w_brc;
/* ---- M7a (2026-11-07) observer-side state (plain fixed-size writes + atomic counters;
   printed on the dump side in threads.c) ----
   Switch smo_m7a: threads.c refreshes g_m7a_on every 2 s (C default 0; echo 0 rolls back).
   With the switch 0 both gates only do the usual loose hits/x0 recording; all M7a capture is
   short-circuited (zero extra reads per fnB).
   M7A_NULL (fnB entry, x0=req): completion-result NULL tally + 16-slot request ring
   (M7b fix: LAST-16 overwrite ring via kk&15; row = {tick, vtable, state, result,
   6 find-key words @slot+0x08 (the inline name string find 0x615e50 strcmps), 2 deref
   words}) - the vtable splits the flavor (A name string / B id pair).
   M7A_REG (find 0x615e50 entry, x0=registry vector): first 4 distinct x0 latched (benign
   idempotent races only). */
_Atomic int g_m7a_on;
static struct { _Atomic uint64_t k, n_null, n_res;
                struct { uint64_t tick, vt, state, result, k0, k1, k2, k3, k4, k5, d0, d1; } r[16]; } g_m7a_null;
static struct { _Atomic unsigned n; uint64_t vec[4]; } g_m7a_reg;
/* ---- M7b (2026-11-07) observer-side state (plain fixed-size writes + atomics; printing on
   the heartbeat thread in threads.c). Switch smo_m7b: threads.c refreshes g_m7b_on every 2 s
   (C default 0; echo 0 rolls back). With the switch 0 the new gates only do the usual loose
   hits/x0 recording; all M7b capture is short-circuited.
   M7B_BR (poster 0x7aca60 entry): x0 = batch object, x4 = LR (caller site; a gated bl site
   appears as stub_k+0x54 - the dump side translates via g_obs_stub_off). First-32 LR claim
   ring (fetch_add, no overwrite) + first/last {tick, x0, flag=[x0+0x18], pump=[x0+8]} with
   guarded reads (flag byte + pump pointer, 0x1_0000_0000..0x8000_0000_0000 window).
   M7B_C0+k (13 bl sites): per-site {k, first tick/x0, last tick/x0} - count only, no reads.
   M7B_GTRUE/GFALSE (fnA 0x61570c/0x6157f0): per-branch {k, first tick, last tick} - the
   TRUE/FALSE split of the TLS-owner gate over time decides the "path switch" hypothesis. */
_Atomic int g_m7b_on;
static struct { _Atomic uint64_t k;
                _Atomic unsigned fset;
                uint64_t f_tick, f_x0, f_flag, f_pump, f_lr;
                _Atomic uint64_t l_tick, l_x0, l_flag, l_pump, l_lr;
                _Atomic uint64_t lr[32]; } g_m7b_br;
static struct { _Atomic uint64_t k, f_tick, l_tick;
                _Atomic unsigned fset;
                uint64_t f_x0; _Atomic uint64_t l_x0; } g_m7b_c[13];
static struct { _Atomic uint64_t k, f_tick, l_tick; } g_m7b_g[2];
/* ---- M7c (2026-11-07) observer-side state (atomics + fixed-size writes; printed on the
   heartbeat thread in threads.c). Switch smo_m7c: threads.c refreshes g_m7c_on every 2 s
   (C default 0; echo 0 rolls back). With the switch 0 the gate only does the usual loose
   hits/x0 recording; all M7c capture is short-circuited.
   M7C_FNA (fnA 0x6156b0 entry): proves fnA full silence after t>140 and gives the first
   "who calls fnA" distribution. x0=this, x3=original x1 (name arg), x4=LR (caller return,
   from the stub's [sp,#8] recovery), thread = runtime_cur_handle_tls (svc-side store;
   pump threads svc regularly so it is current in user-mode fnA runs).
   - total k + first/last tick (f_tick==0 marks "no hit yet"), last thread/x1/lr overwrite.
   - per-thread counts pt[8] {handle, n} (linear scan/insert; fnA callers are the 3 pump
     threads + main; 8 slots, overflow counter pt_ovf).
   - last-16 overwrite ring r[16] via rk fetch_add claim (kk&15): {tick, thread, x1, lr}.
     Dump side may read a torn row (same disclosed observation semantics as g_m7a_null). */
_Atomic int g_m7c_on;
static struct {
    _Atomic uint64_t k;
    _Atomic unsigned fset;
    uint64_t f_tick;
    _Atomic uint64_t l_tick, l_thr, l_x1, l_lr;
    struct { uint32_t thr; _Atomic uint64_t n; } pt[8];
    _Atomic uint64_t pt_ovf;
    _Atomic uint64_t rk;
    struct { uint64_t tick, thr, x1, lr; } r[16];
    _Atomic uint64_t r_ovf;
} g_m7c_fna;
/* ---- M7e (2026-11-07) observer-side state (atomics + fixed-size writes; printed on the
   heartbeat thread in threads.c). Switch smo_m7e: threads.c refreshes g_m7e_on every 2 s
   (C default 0; echo 0 rolls back). With the switch 0 the gate only does the usual loose
   hits/x0 recording; all M7e capture is short-circuited (zero extra reads per create).
   M7E_CRT (create-request 0x746020 entry): the "who still builds requests" ledger.
   x0 = the output request object, x4 = LR (the creator function's return address - the whole
   point: name the creator), thread = runtime_cur_handle_tls.
   - total k + first {tick,x0,lr,thr} + last {tick,x0,lr,thr} (f_tick==0 => no hit yet).
   - per-thread pt[8] {handle, n} + pt_ovf: which thread creates requests.
   - LAST-32 overwrite ring r[32] via kk&31 {tick, thr, x0, lr}: tail of the stream = the
     freeze moment; r_ovf keeps the claim watermark.
   - distinct-LR latch lr[8] {lr, n} + lr_ovf: the creator distribution without depending on
     the ring (linear scan/insert; racy insert only undercounts, benign observation). */
_Atomic int g_m7e_on;
static struct {
    _Atomic uint64_t k;
    _Atomic unsigned fset;
    uint64_t f_tick, f_x0, f_lr; uint32_t f_thr;
    _Atomic uint64_t l_tick, l_x0, l_lr, l_thr;
    struct { uint32_t thr; _Atomic uint64_t n; } pt[8];
    _Atomic uint64_t pt_ovf;
    _Atomic uint64_t rk;
    struct { uint64_t tick, thr, x0, lr; } r[32];
    _Atomic uint64_t r_ovf;
    struct { uint64_t lr; _Atomic uint64_t n; } lr[8];
    _Atomic uint64_t lr_ovf;
} g_m7e_crt;
/* ---- M7g (2026-11-07) observer-side state: the layer ABOVE the creator. Four gates share
   one generic ledger (index = gate - SMO_OBS_M7G_CRT):
     0 = 0x7488f0 creator fn head (union of every path into it; LR splits bl vs the two tails)
     1 = 0x7485a8 entry-A tail block (tbnz path; tail-calls 0x7488f0 at 0x74860c)
     2 = 0x748fb0 entry-B fallback block (tail-calls 0x7488f0 at 0x748fe4)
     3 = 0x7461a0 sibling ctor (the x3!=0 arm: gate-level 45/55 check)
   Per gate: total k, first/last {tick,thr,x0,x1,lr}, per-thread pt[8], last-32 overwrite
   ring {tick,thr,x0,x1,lr}, distinct-LR latch lr[8]. x1 = the stub's arg-4 (original x1) -
   the shared stub does not propagate original x2/x3, so rows carry x0/x1 only.
   Switch smo_m7g: threads.c refreshes g_m7g_on every 2 s (C default 0; echo 0 rolls back);
   with the switch 0 each gate only does the usual loose hits/x0 recording. */
_Atomic int g_m7g_on;
#define M7G_NG 4
typedef struct {
    _Atomic uint64_t k;
    _Atomic unsigned fset;
    uint64_t f_tick, f_x0, f_x1, f_lr; uint32_t f_thr;
    _Atomic uint64_t l_tick, l_x0, l_x1, l_lr, l_thr;
    struct { uint32_t thr; _Atomic uint64_t n; } pt[8];
    _Atomic uint64_t pt_ovf;
    _Atomic uint64_t rk;
    struct { uint64_t tick, x0, x1, lr; uint32_t thr; } r[32];
    _Atomic uint64_t r_ovf;
    struct { uint64_t lr; _Atomic uint64_t n; } lr[8];
    _Atomic uint64_t lr_ovf;
} m7g_g_t;
static m7g_g_t g_m7g[M7G_NG];
/* ---- M7h（2026-11-08）observer 侧状态：**批循环是怎么停的**。index = gate - SMO_OBS_M7H_BQ：
     0 BQ    0x748e80 查表函数头        （12 个静态调用者按 LR 归类；lr[16]+环 16）
     1 ENT   0x4be0d0 批循环函数头      （LR = 调用者返回址，2 个静态调用者）
     2 TASKS 0x4be1cc 任务数>0 才到     （b.le 落空侧）
     3 L2E   0x4be284 loop2 前导        （x4==0 模式入口）
     4 HEAD  0x4be294 loop2 head
     5 LOOK  0x4be2ec 即将发起查表
     6 RET   0x4be2f4 查表返回、取消检查已到达
     7 CONT  0x4be2fc cbnz 落空 = 取消未置位
     8 EXH   0x4be30c b.lt 落空 = 耗尽（三路合流）
     9 EXH1  0x4be260 loop1 耗尽
   每门：tot k、first/last{tick,thr,x0,x1,lr}、per-thread pt[8]、末 16 覆盖环 {tick,lr,x0,thr}、
   distinct-LR 锁存 lr[16]（12 个调用者 + 4 余量）、三个溢出计数。
   **零解引用**：这 10 个门点全部只读寄存器，不读任何指针内容 —— 因此不会像 SENDTO_REJ/
   M7A_NULL 那样带护栏读，退化成纯计数 + LR 归类（派活单 §3 A2 要求的就是这个粒度）。
   Switch smo_m7h：threads.c 每 2 s 刷新 g_m7h_on（C 默认 0；echo 0 ≤2 s 回退）。 */
_Atomic int g_m7h_on;
#define M7H_NG 15   /* M7L 追加 5 只（pump/dp1/dp2/pdone/pexit），索引 = gate - SMO_OBS_M7H_BQ */
typedef struct {
    _Atomic uint64_t k;
    _Atomic uint64_t f_tick, f_x0, f_x1, l_tick, l_x0, l_x1, l_lr, l_thr;
    _Atomic uint64_t rk, r_ovf, lr_ovf, pt_ovf;
    struct { uint32_t thr; _Atomic uint64_t n; } pt[8];
    struct { uint64_t tick, lr, x0; uint32_t thr; } r[16];
    struct { uint64_t lr; _Atomic uint64_t n; _Atomic uint64_t l_tick; } lr[16];
} m7h_g_t;
static m7h_g_t g_m7h[M7H_NG];
/* ---- M7N（2026-11-08）observer 侧状态：**谁 post 了谁**。index = gate - SMO_OBS_M7N_G1：
      0 = G1 工厂 main+0x964260 入口（x0 = ctx 指针，num = [x0+0x88] 编号，LR = 谁调工厂）
      1 = G2 PostToken main+0x9646d0 入口（x0 = worker 对象指针，x1 = 门点原 x1 的 bit0
          "紧急支"标志，tok = [x0+0xc8] 期望 token，state = [x0+0xd4] 1=RUN/2=SIGNALLED）
   每门：tot k、首见 {tick,x0,x1,num/state,tok,lr,thr}、末见 {tick,x0,lr,thr}、
   首 **64** 条逐事件行（fetch_add 认领，kk<64 才写 ⇒ **不改写**，保的就是"第一次"）、
   per-thread pt[8]、distinct-LR 锁存 16 槽（带每 LR 末次 tick）、
   distinct-object（worker 指针 / ctx 指针）锁存 16 槽（带每对象首次 tick）—— 这张
   「按角色键控」的对象台账就是本轮判决依据（**不用 0x1D 槽位差集**）。
   Switch smo_m7n：threads.c 每 2 s 刷新 g_m7n_on（C 默认 0；echo 0 ≤2 s 回退）；
   开关 0 时两只门只做既有宽松 hits/x0 记录。热路径：护栏内定长读 + 原子加，无 I/O。 */
_Atomic int g_m7n_on;
#define M7N_NG 2
#define M7N_ROW 64
#define M7N_LR 16
#define M7N_WP 16
typedef struct {
    _Atomic uint64_t k;
    _Atomic unsigned fset;
    uint64_t f_tick, f_x0, f_x1, f_aux, f_tok;
    uint32_t f_state, f_thr;
    uint64_t f_lr;
    _Atomic uint64_t l_tick, l_x0, l_lr, l_thr;
    _Atomic uint64_t rk, r_ovf, lr_ovf, wp_ovf, pt_ovf;
    struct { uint32_t thr; _Atomic uint64_t n; } pt[8];
    struct { uint64_t tick, x0, x1, tok, lr; uint32_t thr, state, aux; } r[M7N_ROW];
    struct { uint64_t lr; _Atomic uint64_t n; _Atomic uint64_t l_tick; } lr[M7N_LR];
    struct { uint64_t w; _Atomic uint64_t n; _Atomic uint64_t f_tick; _Atomic uint64_t f_lr; } wp[M7N_WP];
} m7n_g_t;
static m7n_g_t g_m7n[M7N_NG];
/* ---- M7P（2026-11-08）observer 侧状态：**一只门、两边跑** —— worker Start
   main+0x963e50，15 个 ctor 站点的唯一公共出口。index = 0（单门）。
   每命中记：**LR**（x4 = 门点 LR = bl 站点 + 4 ⇒ 站点 = LR − 4，直接点名"谁叫的"）、
   **x0**（worker 对象指针）、**epoch** ep = [x1]（函数下一条指令正是 ldr w8,[x1] 再
   str w8,[x0,#0xd8] ⇒ x1 此刻必须在册，读它同语义）、state=[x0+0xd4]、tok=[x0+0xc8]、
   tick、线程 handle。
   台账：tot、首/末见、首 **64** 条逐事件行（fetch_add 认领、kk<64 才写 ⇒ **不改写**，
   保"第一次"）、per-LR 表 **32** 槽（n / 首次 tick / 末次 tick）= 派活单要的"每站计数表"、
   per-object 表 **24** 槽（n / 首次 tick / 首个 LR / epoch）= "哪个对象由哪一站建出"。
   Switch smo_m7p：threads.c 每 2 s 刷新 g_m7p_on（C 默认 0；echo 0 ≤2 s 回退）；
   前 **16** 次无条件捕获（保开关文件被读到之前的极早 Start 不丢；M7P 的 Start 实测都在
   t≈23 s 之后，纯保险，每命中仍只 1 次原子加）。 */
_Atomic int g_m7p_on;
#define M7P_NG 1
#define M7P_ROW 64
#define M7P_LR 32
#define M7P_OBJ 24
typedef struct {
    _Atomic uint64_t k;
    _Atomic unsigned fset;
    uint64_t f_tick, f_x0, f_lr, f_ep;
    uint32_t f_thr, f_state;
    _Atomic uint64_t l_tick, l_x0, l_lr, l_thr;
    _Atomic uint64_t rk, r_ovf, lr_ovf, ob_ovf, pt_ovf;
    struct { uint32_t thr; _Atomic uint64_t n; } pt[8];
    struct { uint64_t tick, lr, x0, ep, tok; uint32_t thr, state; } r[M7P_ROW];
    struct { uint64_t lr; _Atomic uint64_t n; _Atomic uint64_t f_tick, l_tick; } lr[M7P_LR];
    struct { uint64_t w; _Atomic uint64_t n; _Atomic uint64_t f_tick, f_lr, f_ep; } ob[M7P_OBJ];
} m7p_g_t;
static m7p_g_t g_m7p[M7P_NG];
/* ---- M7Q（2026-11-08）：三层门，index = gate - SMO_OBS_M7Q_WRAP（0=WRAP,1=FO,2=FOB）。
   每门台账：tot / 首末见 / 首 64 条逐事件行（认领式，**不改写**）/ per-LR 24 槽
   （n / 首次 tick / 末次 tick ⇒ 两边做差集就是「哪条站点没进」）/ per-value 8 槽
   （FO/FOB 的 x1 = 扇出数 w1 的取值分布 + 计数 + 首次 tick）。
   Switch smo_m7q（threads.c 每 2 s 刷新 g_m7q_on；C 默认 0 ⇒ echo 0 ≤2 s 回退）；
   前 16 次无条件捕获（防开关文件被读到之前的极早 ctor）。观察器零写、零 I/O，
   每命中 = 2 次原子加 + 5 次定长写。 */
_Atomic int g_m7q_on;
#define M7Q_NG 3
#define M7Q_ROW 64
#define M7Q_LR 24
#define M7Q_VAL 8
typedef struct {
    _Atomic uint64_t k;
    _Atomic unsigned fset;
    uint64_t f_tick, f_lr, f_x0, f_x1, f_x8;
    uint32_t f_thr;
    _Atomic uint64_t l_tick, l_lr, l_x0, l_x1;
    _Atomic uint64_t rk, r_ovf, lr_ovf, v_ovf;
    struct { uint64_t tick, lr, x0, x1, x8; uint32_t thr; } r[M7Q_ROW];
    struct { uint64_t lr; _Atomic uint64_t n, f_tick, l_tick; } lr[M7Q_LR];
    struct { uint64_t v; _Atomic uint64_t n, f_tick, l_tick; } v[M7Q_VAL];
} m7q_g_t;
static m7q_g_t g_m7q[M7Q_NG];
/* ---- M7S2（2026-11-08）：三层门，index = gate - SMO_OBS_M7S2_G1（0=G1 0x681b30,
   1=G2 0x74ca00, 2=G3 0x74cadc）。每门台账：tot / 首末见 / per-LR 32 槽（n + 首末 tick
   —— 这是本轮的主判据表：G1 只该有 1 个 LR（唯一静态调用者），G2 最多 8 个，
   G3 最多 8 个（0x74ca60 的调用者））/ 首 64 条逐事件环（认领式、不改写；G3 只计数+LR，
   不写环以省每命中成本）。
   Switch smo_m7s2（threads.c 每 2 s 刷新 g_m7s2_on；C 默认 0 ⇒ echo 0 ≤2 s 回退）。
   观察器零写、零 I/O、零解引用：每命中 = 2 次原子加 + 定长写。
   ⚠️ 字段能力边界（如实）：共享 stub 的 prologue 只把 x0 / 原 x1 / 原 x8 / 原 LR 传给观察器
   （见 smo_obs_observer 注），**原 x2/x3 拿不到**；因此 G1 的台账是 LR/x0/x1/x8/tick/thr，
   不是派活单字面写的 x0..x3。要改这一点必须动共享 stub（60+ 门同时在用）⇒ 不做。 */
_Atomic int g_m7s2_on;
#define M7S2_NG 3
#define M7S2_ROW 64
#define M7S2_LR 32
typedef struct {
    _Atomic uint64_t k;
    _Atomic unsigned fset;
    uint64_t f_tick, f_lr, f_x0, f_x1, f_x8;
    uint32_t f_thr;
    _Atomic uint64_t l_tick, l_lr, l_x0, l_x1;
    _Atomic uint64_t rk, r_ovf, lr_ovf;
    struct { uint64_t tick, lr, x0, x1, x8; uint32_t thr; } r[M7S2_ROW];
    struct { uint64_t lr; _Atomic uint64_t n, f_tick, l_tick; } lr[M7S2_LR];
} m7s2_g_t;
static m7s2_g_t g_m7s2[M7S2_NG];
/* ---- M7T2（2026-11-08）：**整族九个头**（index = gate - SMO_OBS_M7T2_H1，0..8 对应
   0x445640 / 0x447b70 / 0x449c50 / 0x44b720 / 0x46e7a0 / 0x4a1100 / 0x4a6b10 /
   0x4a8820 / 0x4af0a0）。派活单第 1 节要的台账口径："每门只记 count + 首个/末个 tick +
   LR 环（≥8）" ⇒ 本轮按字面实现，**不写逐事件环**（省每命中成本、也让日志可一眼读完）：
     k        = 该门命中总数
     首见     = {tick, LR, x0, 原x1, 原x8, 线程}
     末见     = {tick, LR, x0, 原x1}
     lr[32]   = per-LR（= per-call-site，site = LR−4）环：n + 首末 tick，**32 ≥ 8**
   零静态调用者的 7 门若 tot≥1，LR 就是唯一能说出"谁间接进来的"的字段；有 1 条 bl 的
   H2/H6 则应恰好落在 0x449970 / 0x4a10d8（可自校）。
   Switch smo_m7t2（threads.c 每 2 s 刷新 g_m7t2_on；C 默认 0 ⇒ `echo 0` ≤2 s 回退）。 */
_Atomic int g_m7t2_on;
#define M7T2_NG 9
#define M7T2_LR 32
typedef struct {
    _Atomic uint64_t k;
    _Atomic unsigned fset;
    uint64_t f_tick, f_lr, f_x0, f_x1, f_x8;
    uint32_t f_thr;
    _Atomic uint64_t l_tick, l_lr, l_x0, l_x1;
    _Atomic uint64_t lr_ovf;
    struct { uint64_t lr; _Atomic uint64_t n, f_tick, l_tick; } lr[M7T2_LR];
} m7t2_g_t;
static m7t2_g_t g_m7t2[M7T2_NG];
/* ---- M7U2（2026-11-08）：**场景派发枢纽 0x7acd50** 三门（index = gate - SMO_OBS_M7U2_A，
   0=A 0x7acd50 / 1=B 0x7acd94 / 2=C 0x7acd70）。
   每门台账与 M7T2 同口径（count + 首末见 + per-LR 16 槽，16 ≥ 派活单要求的"LR 环 ≥16"）。
   另加**独立**的 stage 名环（8 槽，门 A / 门 C 共用）：派活单第 1 节 C 门要的
   "stage 名字符串指针 + 前 32 字节 ASCII"进日志 —— 观察器不打印（无 I/O），只把定长字节
   拷进环，由 threads.c 的 dump 侧排版。
   Switch smo_m7u2（threads.c 每 2 s 刷新 g_m7u2_on；C 默认 0 ⇒ `echo 0` ≤2 s 回退）。 */
_Atomic int g_m7u2_on;
#define M7U2_NG 3
#define M7U2_LR 16
#define M7U2_NAME_SLOTS 8
typedef struct {
    _Atomic uint64_t k;
    _Atomic unsigned fset;
    uint64_t f_tick, f_lr, f_x0, f_x1, f_x8;
    uint32_t f_thr;
    _Atomic uint64_t l_tick, l_lr, l_x0, l_x1;
    _Atomic uint64_t lr_ovf;
    struct { uint64_t lr; _Atomic uint64_t n, f_tick, l_tick; } lr[M7U2_LR];
} m7u2_g_t;
static m7u2_g_t g_m7u2[M7U2_NG];
typedef struct {
    uint64_t tick, lr, this_, name_ptr, name2_ptr, scene, vtbl;
    uint32_t gate, thr, printable, reserved;
    unsigned char b[32];          /* stage 名前 32 字节（原始字节，dump 侧过滤可打印字符） */
} m7u2_name_rec_t;
static struct { _Atomic uint64_t n; m7u2_name_rec_t s[M7U2_NAME_SLOTS]; } g_m7u2_name;
/* stage 名抓取（门 A / 门 C 共用；只读、保镖式：指针域护栏 + 定长读，零 I/O、零分配）。
   name_ptr = [this+8]（与 0x7acd5c `ldr x1,[x0,#8]` 同一字段；门 A 在入口抓、门 C 在
             `bl 0x9564f0` 查表前抓 ⇒ 若真机 name 为 NULL，A 仍会记下 name_ptr=0）。
   b        = name_ptr 处前 32B；若首字节不是可打印 ASCII 则再解一层 q=*name_ptr（过护栏）
             把 q 处 32B 填进 b（覆盖两种布局：裸 char* 与 {char*}/小对象）。
   scene    = [this+0x10]（0x7acd30 存进去的场景对象）、vtbl = [scene]（vtable 指针；
             **这是派活单 B 门想要而 blr 后已丢失的那个指针**）。 */
static inline int m7u2_ptr_ok(uint64_t p) { return p > 0x100000000ULL && p < 0x800000000000ULL; }
extern __thread uint32_t runtime_cur_handle_tls;   /* threads.c（下方已有同一 extern 声明） */
static void m7u2_rd(void *dst, uint64_t p, unsigned n) {
    const volatile unsigned char *s = (const volatile unsigned char *)(uintptr_t)p;
    unsigned char *d = (unsigned char *)dst;
    for (unsigned i = 0; i < n; i++) d[i] = s[i];
}
static void m7u2_capture(uint64_t gate, uint64_t thisp, uint64_t lr) {
    uint64_t slot = atomic_fetch_add_explicit(&g_m7u2_name.n, 1, memory_order_relaxed);
    if (slot >= M7U2_NAME_SLOTS) return;
    m7u2_name_rec_t *r = &g_m7u2_name.s[slot];
    r->tick = smo_obs_tick(); r->lr = lr; r->gate = (uint32_t)gate;
    r->thr = runtime_cur_handle_tls; r->this_ = thisp;
    if (!m7u2_ptr_ok(thisp)) return;
    uint64_t name = 0, scene = 0;
    m7u2_rd(&name, thisp + 8, 8);
    m7u2_rd(&scene, thisp + 0x10, 8);
    r->name_ptr = name; r->scene = scene;
    if (m7u2_ptr_ok(scene)) { uint64_t vt = 0; m7u2_rd(&vt, scene, 8); r->vtbl = vt; }
    if (!m7u2_ptr_ok(name)) return;
    m7u2_rd(r->b, name, 32);
    int pr = 1;
    for (int i = 0; i < 31; i++) {
        unsigned char c = r->b[i];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7e) { pr = 0; break; }
    }
    r->printable = (uint32_t)pr;
    if (!pr) {                                  /* 再解一层（{char*}/name 小对象） */
        uint64_t q = 0; m7u2_rd(&q, name, 8);
        if (m7u2_ptr_ok(q)) { r->name2_ptr = q; m7u2_rd(r->b, q, 32); }
    }
}
/* ---- M7V（2026-11-08）：**点名"真机派发到谁"** —— 八门（门号 86..93）----
   X8 = main+0x7acd8c `ldr x8,[x8]`（tail=2 单字门）。门点：x0 = [this+0x10] 场景对象、
    **原 x8 = vtable 指针（= [scene]）**、原 x1 = stage 名指针、LR = 0x7acd94 之外的调用者返回址。
   观察器把 [x8] 再解一层 ⇒ **0x7acd90 `blr x8` 真正会跳的函数地址**（"派发到谁"从假说变读数）。
   R1..R7 = Rsv 兄弟实例入口（tail=0 四字窗），只记 count/首末见/per-LR 16 槽 + 原 x0/x1/x8。
   判据：X8 的 tgt 若落进 H1..H9 ⇒ 假说①否（门口径盲点）；若落进 R1..R7 ⇒ Rsv 假说成立；
          若两者都不是 ⇒ 族外实例（直接点名新函数）。
   stage 名环 v2（**不加门**，挂既有门 A(83)/门 C(85) 的抓取点）：**只留原始 hex、不改写、不覆盖**，
   name_ptr / +0x10 / +0x18 / [name_ptr]+0x10 四个 32B 槽一次取全（M7U 已证首 8B 是
   main+0x1352ac8 的两平台同址指针、不是裸 char*，所以本轮不再做 ASCII 过滤/再解层覆盖）。
   Switch smo_m7v（threads.c 每 2 s 刷新 g_m7v_on；C 默认 0 ⇒ `echo 0` ≤2 s 回退）。 */
_Atomic int g_m7v_on;
#define M7V_NG 8
#define M7V_LR 16
#define M7V_D_SLOTS 8
#define M7V_NAME_SLOTS 8
typedef struct {
    _Atomic uint64_t k;
    _Atomic unsigned fset;
    uint64_t f_tick, f_lr, f_x0, f_x1, f_x8;
    uint32_t f_thr;
    _Atomic uint64_t l_tick, l_lr, l_x0, l_x1;
    _Atomic uint64_t lr_ovf;
    struct { uint64_t lr; _Atomic uint64_t n, f_tick, l_tick; } lr[M7V_LR];
} m7v_g_t;
static m7v_g_t g_m7v[M7V_NG];
/* X8 门专用环：每次命中记 时刻 / LR / 场景对象 / vtable 指针 / **解一层后的派发目标** */
typedef struct {
    uint64_t tick, lr, scene, vtbl, tgt;
    uint32_t gate, thr, ok, reserved;
} m7v_d_rec_t;
static struct { _Atomic uint64_t n; m7v_d_rec_t s[M7V_D_SLOTS]; } g_m7v_d;
/* 名环 v2：四槽原始 32B（不改写、不互相覆盖），avail 位图说明哪些槽读到了 */
typedef struct {
    uint64_t tick, lr, this_, name_ptr, q_ptr, scene, vtbl;
    uint32_t gate, thr, avail, readable;
    unsigned char s0[32];   /* name_ptr 处 32B（原始，不 ASCII 过滤、不覆盖） */
    unsigned char s1[32];   /* name_ptr+0x10 处 32B */
    unsigned char s2[32];   /* name_ptr+0x18 处 32B */
    unsigned char s3[32];   /* [name_ptr]+0x10 处 32B */
} m7v_name_rec_t;
static struct { _Atomic uint64_t n; m7v_name_rec_t s[M7V_NAME_SLOTS]; } g_m7v_name;
static inline int m7v_ptr_ok(uint64_t p) { return p > 0x100000000ULL && p < 0x800000000000ULL; }
extern int runtime_safe_read_ungated(void *dst, uint64_t a, size_t n);   /* loader.c：SIGSEGV 兜底读 */
/* 八门共用的"保镖式"读：域护栏 + runtime_safe_read_ungated（SEGV → 0，不炸 boot） */
static inline int m7v_rd(void *dst, uint64_t p, unsigned n) {
    if (!m7v_ptr_ok(p)) return 0;
    return runtime_safe_read_ungated(dst, p, n);
}
static void m7v_capture_name(uint64_t gate, uint64_t thisp, uint64_t lr) {
    uint64_t slot = atomic_fetch_add_explicit(&g_m7v_name.n, 1, memory_order_relaxed);
    if (slot >= M7V_NAME_SLOTS) return;
    m7v_name_rec_t *r = &g_m7v_name.s[slot];
    r->tick = smo_obs_tick(); r->lr = lr; r->gate = (uint32_t)gate;
    r->thr = runtime_cur_handle_tls; r->this_ = thisp;
    uint64_t name = 0, scene = 0;
    if (!m7v_rd(&name, thisp + 8, 8)) return;
    m7v_rd(&scene, thisp + 0x10, 8);
    r->name_ptr = name; r->scene = scene;
    if (scene) m7v_rd(&r->vtbl, scene, 8);
    if (!m7v_ptr_ok(name)) return;
    if (m7v_rd(r->s0, name, 32))               r->avail |= 1u;   /* name_ptr */
    if (m7v_rd(r->s1, name + 0x10, 32))        r->avail |= 2u;   /* +0x10 */
    if (m7v_rd(r->s2, name + 0x18, 32))        r->avail |= 4u;   /* +0x18 */
    if (m7v_rd(&r->q_ptr, name, 8) && m7v_ptr_ok(r->q_ptr) &&
        m7v_rd(r->s3, r->q_ptr + 0x10, 32))    r->avail |= 8u;   /* [name_ptr]+0x10 */
    /* readable = 四个槽里"含 ≥1 个可打印 ASCII 字节"的槽数（只统计，不改写字节） */
    const unsigned char *sl[4] = { r->s0, r->s1, r->s2, r->s3 };
    for (int i = 0; i < 4; i++) {
        if (!(r->avail & (1u << i))) continue;
        int pr = 0;
        for (int k = 0; k < 32; k++) { unsigned char c = sl[i][k]; if (c >= 0x20 && c <= 0x7e) pr = 1; }
        if (pr) r->readable |= (1u << i);
    }
}
/* ---- M7W2（2026-11-08）：**在适配器里复算真 ctor** —— 一门（门号 94，main+0x7724d0）----
   适配器语义（离线逐条读自 main.patched.bin，见枚举注）：
     p=[obj+8]  o=[obj+0x10]  tag=[obj+0x18]  arg=[obj+0x20]
     q   = p + (int64)tag >> 1                      （= asr #1，算术右移，tag 按 64 位有符号）
     tgt = (tag&1) ? *( *(q) + o ) : o              （虚：先解 vtable 再取槽；非虚：[obj+0x10] 即目标）
   门点 x0 = 描述符对象（= M7V 日志的 scene）。观察器**自己复算**这两步 ⇒ obj/tag/q/tgt 四个值
   落日志，tgt 再交给 threads.c 的 runtime_describe_address 点名。
   开销：每次命中 ≤6 次定长读（都过 m7v_rd：域护栏 + SIGSEGV 兜底）+ 一次认领写。
   环 16 槽（真机 150 s 窗口内只派发 1 次，模拟器 2 次；留 16 是给「后面还会不会再来」留余量）。
   Switch smo_m7w2（threads.c 每 2 s 刷新 g_m7w2_on；C 默认 0 ⇒ `echo 0` ≤2 s 回退）。 */
_Atomic int g_m7w2_on;
#define M7W2_SLOTS 16
#define M7W2_LR 8
typedef struct {
    uint64_t tick, lr, obj, p, o, tag, arg, q, vt, tgt, dtbl;
    uint32_t thr, kind, rd, ok;   /* kind: 0=非虚（tgt=[obj+0x10]）/ 1=虚（tgt=*(*(q)+o)）；rd=读成功位图 */
} m7w2_rec_t;
static struct { _Atomic uint64_t n; m7w2_rec_t s[M7W2_SLOTS]; } g_m7w2_ad;
static struct {
    _Atomic uint64_t k, lr_ovf;
    _Atomic unsigned fset;
    uint64_t f_tick, f_lr, f_obj;
    _Atomic uint64_t l_tick, l_lr, l_obj;
    struct { uint64_t lr; _Atomic uint64_t n, f_tick, l_tick; } lr[M7W2_LR];
} g_m7w2_g;
static void m7w2_capture(uint64_t lr, uint64_t x0, uint32_t thr) {
    uint64_t p = 0, o = 0, tag = 0, arg = 0, q = 0, vt = 0, tgt = 0, dtbl = 0;
    uint32_t rd = 0, kind = 0, ok = 0;
    if (m7v_rd(&dtbl, x0, 8))       rd |= 0x10;          /* [obj] = 描述符 vtable（M7V 的 scene[0]） */
    if (m7v_rd(&p,    x0 + 0x08, 8)) rd |= 0x01;
    if (m7v_rd(&o,    x0 + 0x10, 8)) rd |= 0x02;
    if (m7v_rd(&tag,  x0 + 0x18, 8)) rd |= 0x04;
    if (m7v_rd(&arg,  x0 + 0x20, 8)) rd |= 0x08;
    if ((rd & 0x07) == 0x07) {
        q = p + (uint64_t)((int64_t)tag >> 1);           /* 0x7724d8 add x8,x8,x9,asr #1 */
        if (tag & 1) {                                   /* 虚：0x7724e0/0x7724e4 */
            kind = 1;
            if (m7v_rd(&vt, q, 8) && m7v_ptr_ok(vt) && m7v_rd(&tgt, vt + o, 8)) ok = 1;
        } else {                                         /* 非虚：0x7724dc tbz 跳到 0x7724e8，目标 = [obj+0x10] */
            kind = 0;
            tgt = o;
            ok = (uint32_t)m7v_ptr_ok(o);
        }
    }
    uint64_t slot = atomic_fetch_add_explicit(&g_m7w2_ad.n, 1, memory_order_relaxed);
    if (slot < M7W2_SLOTS) {                             /* 认领即写、单写、不复写 */
        m7w2_rec_t *r = &g_m7w2_ad.s[slot];
        r->tick = smo_obs_tick(); r->lr = lr; r->obj = x0;
        r->p = p; r->o = o; r->tag = tag; r->arg = arg;
        r->q = q; r->vt = vt; r->tgt = tgt; r->dtbl = dtbl;
        r->thr = thr; r->kind = kind; r->rd = rd; r->ok = ok;
    }
    /* 常规台账：count + 首末见 + per-LR 8 槽（site = lr − 4） */
    uint64_t t = smo_obs_tick();
    atomic_fetch_add_explicit(&g_m7w2_g.k, 1, memory_order_relaxed);
    atomic_store_explicit(&g_m7w2_g.l_tick, t, memory_order_relaxed);
    atomic_store_explicit(&g_m7w2_g.l_lr, lr, memory_order_relaxed);
    atomic_store_explicit(&g_m7w2_g.l_obj, x0, memory_order_relaxed);
    if (!atomic_load_explicit(&g_m7w2_g.fset, memory_order_relaxed)) {
        g_m7w2_g.f_tick = t; g_m7w2_g.f_lr = lr; g_m7w2_g.f_obj = x0;
        atomic_store_explicit(&g_m7w2_g.fset, 1, memory_order_relaxed);
    }
    int lslot = -1;
    for (int i = 0; i < M7W2_LR && lslot < 0; i++) if (g_m7w2_g.lr[i].lr == lr) lslot = i;
    if (lslot < 0)
        for (int i = 0; i < M7W2_LR && lslot < 0; i++) if (!g_m7w2_g.lr[i].lr) { g_m7w2_g.lr[i].lr = lr; lslot = i; }
    if (lslot >= 0) {
        if (!atomic_load_explicit(&g_m7w2_g.lr[lslot].n, memory_order_relaxed))
            atomic_store_explicit(&g_m7w2_g.lr[lslot].f_tick, t, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_m7w2_g.lr[lslot].n, 1, memory_order_relaxed);
        atomic_store_explicit(&g_m7w2_g.lr[lslot].l_tick, t, memory_order_relaxed);
    } else atomic_fetch_add_explicit(&g_m7w2_g.lr_ovf, 1, memory_order_relaxed);
}
/* ---- M7X2（2026-11-08）：**最后一跳** —— 两门（门号 95 / 96）----
   G1 main+0x74c9e0（slot-3 转调 thunk，tail=2 单字门）：适配器 0x7724f0 `br x2` 尾跳进来，
      门点 x0 **已经是 q（真 this）**。观察器解两层：
        vt   = [q]            （0x74c9e0 `ldr x8,[x0]`）
        tgt3 = [[q]+0x18]     （0x74c9e4 `ldr x2,[x8,#0x18]` + 0x74c9e8 `br x2` 的真目标）
      顺带记 o18=[q+0x18]（对象 q 自己的字面字段，避免"两层/一层"歧义）与 arg=x3（门点原 x1）。
   G2 main+0x772500（描述符克隆/拷贝构造，tail=0 四字窗）：门点 x0 = **源描述符**
      （0x77250c `mov x19,x0` 逐条可证：x19 在函数体里就是源）。记 src 与其
      {+8,+0x10,+0x18,+0x20} 共 32 B —— 这 32 B 就是被写进新对象的 32 B（0x77252c/0x772530
      `ldp` ← 0x772538/0x772534 `stp`，纯拷贝、逐字相同）——以及 [src]（源 vtable）、
      LR（调用者返回址，site=lr-4）⇒ 回答"谁把哪个对象绑进 +8"。
   开销：G1 每次命中 ≤3 次定长读、G2 ≤5 次（都过 m7v_rd：域护栏 + SIGSEGV 兜底）+ 一次认领写。
   环各 16 槽（M7W2 实测：真机 150 s 窗口只派发 1 次、模拟器 2 次；留 16 是给"还会不会再来"余量）。
   Switch smo_m7x2（threads.c 每 2 s 刷新 g_m7x2_on；C 默认 0 ⇒ `echo 0` ≤2 s 回退）。 */
_Atomic int g_m7x2_on;
#define M7X2_G1_SLOTS 16
#define M7X2_G2_SLOTS 16
#define M7X2_LR 8
typedef struct {
    uint64_t tick, lr, q, vt, tgt3, o18, arg;
    uint32_t thr, rd, ok;      /* rd: bit0=[q] bit1=[q+0x18] bit2=[[q]+0x18] */
} m7x2_g1_rec_t;
typedef struct {
    uint64_t tick, lr, src, w8, w10, w18, w20, svt;
    uint32_t thr, rd;          /* rd: bit0..3 = +8/+0x10/+0x18/+0x20 读到，bit4=[src] */
} m7x2_g2_rec_t;
static struct { _Atomic uint64_t n; m7x2_g1_rec_t s[M7X2_G1_SLOTS]; } g_m7x2_g1;
static struct { _Atomic uint64_t n; m7x2_g2_rec_t s[M7X2_G2_SLOTS]; } g_m7x2_g2;
static struct {
    _Atomic uint64_t k, lr_ovf; _Atomic unsigned fset;
    uint64_t f_tick, f_lr, f_q;
    _Atomic uint64_t l_tick, l_lr, l_q;
    struct { uint64_t lr; _Atomic uint64_t n, f_tick, l_tick; } lr[M7X2_LR];
} g_m7x2_g1t;
static struct {
    _Atomic uint64_t k, lr_ovf; _Atomic unsigned fset;
    uint64_t f_tick, f_lr, f_src;
    _Atomic uint64_t l_tick, l_lr, l_src;
    struct { uint64_t lr; _Atomic uint64_t n, f_tick, l_tick; } lr[M7X2_LR];
} g_m7x2_g2t;
static void m7x2_g1_capture(uint64_t lr, uint64_t q, uint64_t arg, uint32_t thr) {
    uint64_t vt = 0, tgt3 = 0, o18 = 0;
    uint32_t rd = 0, ok = 0;
    if (m7v_rd(&vt, q, 8))                    rd |= 0x01;
    if (m7v_rd(&o18, q + 0x18, 8))            rd |= 0x02;
    if (m7v_ptr_ok(vt) && m7v_rd(&tgt3, vt + 0x18, 8)) { rd |= 0x04; ok = (uint32_t)m7v_ptr_ok(tgt3); }
    uint64_t slot = atomic_fetch_add_explicit(&g_m7x2_g1.n, 1, memory_order_relaxed);
    if (slot < M7X2_G1_SLOTS) {                            /* 认领即写、单写、不复写 */
        m7x2_g1_rec_t *r = &g_m7x2_g1.s[slot];
        r->tick = smo_obs_tick(); r->lr = lr; r->q = q;
        r->vt = vt; r->tgt3 = tgt3; r->o18 = o18; r->arg = arg;
        r->thr = thr; r->rd = rd; r->ok = ok;
    }
    uint64_t t = smo_obs_tick();
    atomic_fetch_add_explicit(&g_m7x2_g1t.k, 1, memory_order_relaxed);
    atomic_store_explicit(&g_m7x2_g1t.l_tick, t, memory_order_relaxed);
    atomic_store_explicit(&g_m7x2_g1t.l_lr, lr, memory_order_relaxed);
    atomic_store_explicit(&g_m7x2_g1t.l_q, q, memory_order_relaxed);
    if (!atomic_load_explicit(&g_m7x2_g1t.fset, memory_order_relaxed)) {
        g_m7x2_g1t.f_tick = t; g_m7x2_g1t.f_lr = lr; g_m7x2_g1t.f_q = q;
        atomic_store_explicit(&g_m7x2_g1t.fset, 1, memory_order_relaxed);
    }
    int lslot = -1;
    for (int i = 0; i < M7X2_LR && lslot < 0; i++) if (g_m7x2_g1t.lr[i].lr == lr) lslot = i;
    if (lslot < 0)
        for (int i = 0; i < M7X2_LR && lslot < 0; i++) if (!g_m7x2_g1t.lr[i].lr) { g_m7x2_g1t.lr[i].lr = lr; lslot = i; }
    if (lslot >= 0) {
        if (!atomic_load_explicit(&g_m7x2_g1t.lr[lslot].n, memory_order_relaxed))
            atomic_store_explicit(&g_m7x2_g1t.lr[lslot].f_tick, t, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_m7x2_g1t.lr[lslot].n, 1, memory_order_relaxed);
        atomic_store_explicit(&g_m7x2_g1t.lr[lslot].l_tick, t, memory_order_relaxed);
    } else atomic_fetch_add_explicit(&g_m7x2_g1t.lr_ovf, 1, memory_order_relaxed);
}
static void m7x2_g2_capture(uint64_t lr, uint64_t src, uint32_t thr) {
    uint64_t w8 = 0, w10 = 0, w18 = 0, w20 = 0, svt = 0;
    uint32_t rd = 0;
    if (m7v_rd(&w8,  src + 0x08, 8)) rd |= 0x01;
    if (m7v_rd(&w10, src + 0x10, 8)) rd |= 0x02;
    if (m7v_rd(&w18, src + 0x18, 8)) rd |= 0x04;
    if (m7v_rd(&w20, src + 0x20, 8)) rd |= 0x08;
    if (m7v_rd(&svt, src, 8))        rd |= 0x10;
    uint64_t slot = atomic_fetch_add_explicit(&g_m7x2_g2.n, 1, memory_order_relaxed);
    if (slot < M7X2_G2_SLOTS) {
        m7x2_g2_rec_t *r = &g_m7x2_g2.s[slot];
        r->tick = smo_obs_tick(); r->lr = lr; r->src = src;
        r->w8 = w8; r->w10 = w10; r->w18 = w18; r->w20 = w20; r->svt = svt;
        r->thr = thr; r->rd = rd;
    }
    uint64_t t = smo_obs_tick();
    atomic_fetch_add_explicit(&g_m7x2_g2t.k, 1, memory_order_relaxed);
    atomic_store_explicit(&g_m7x2_g2t.l_tick, t, memory_order_relaxed);
    atomic_store_explicit(&g_m7x2_g2t.l_lr, lr, memory_order_relaxed);
    atomic_store_explicit(&g_m7x2_g2t.l_src, src, memory_order_relaxed);
    if (!atomic_load_explicit(&g_m7x2_g2t.fset, memory_order_relaxed)) {
        g_m7x2_g2t.f_tick = t; g_m7x2_g2t.f_lr = lr; g_m7x2_g2t.f_src = src;
        atomic_store_explicit(&g_m7x2_g2t.fset, 1, memory_order_relaxed);
    }
    int lslot = -1;
    for (int i = 0; i < M7X2_LR && lslot < 0; i++) if (g_m7x2_g2t.lr[i].lr == lr) lslot = i;
    if (lslot < 0)
        for (int i = 0; i < M7X2_LR && lslot < 0; i++) if (!g_m7x2_g2t.lr[i].lr) { g_m7x2_g2t.lr[i].lr = lr; lslot = i; }
    if (lslot >= 0) {
        if (!atomic_load_explicit(&g_m7x2_g2t.lr[lslot].n, memory_order_relaxed))
            atomic_store_explicit(&g_m7x2_g2t.lr[lslot].f_tick, t, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_m7x2_g2t.lr[lslot].n, 1, memory_order_relaxed);
        atomic_store_explicit(&g_m7x2_g2t.lr[lslot].l_tick, t, memory_order_relaxed);
    } else atomic_fetch_add_explicit(&g_m7x2_g2t.lr_ovf, 1, memory_order_relaxed);
}
/* ---- M7Y2（2026-11-08）：**认那个"另一个类的对象"** —— 两门（97 = main+0x44adb0 / 98 = main+0x44af80）
   M7X2 已判定"绑定点/绑定者/源类/32 B 语义两边完全一致，变的是被绑进 +8 的 q 属于不同的类"：
     真机 q.vt = main+0x12bd470、[[q]+0x18] = main+0x44adb0（族外终点）；
     模拟器 q.vt = main+0x12cd510 / main+0x12c4dc8、[[q]+0x18] = main+0x4a6b10(H7) / main+0x46e7a0(H5)。
   本轮两门把"真机那个终点"从一次性读数变成时间序列，并且**在门点直接读类的名字**：
   离线用 161,696 条加载期重定位行（{value, cell, 0x403}）解出三个类的 .bss 类型块布局，
   三族一致：类名指针固定在 **[q]+0xb8**（该格由重定位行 "cell = [q]+0xc0" 写入，写入落点是 cell-8，
   与 slot 0x18 的落点规律逐字一致，三族同构）：
       真机  [q+0xb8] = main+0xef5550 → "18FirstSequenceScene"
       模拟器 H7 [q+0xb8] = main+0xefdc40 → "14TitleMenuScene"
       模拟器 H5 该偏移不是名字（H5 布局多一格；其名字尚未定位，如实记"未定"）
   ⇒ 门 97 每次命中：q / vt=[q] / tgt3=[[q]+0x18] / [q+0xb8] 与 **其指向的类名字符串（≤24 B）** /
     arg / LR / 时刻 —— 真机与模拟器都能自报"这次派发的是哪个类的对象"。
   门 98 = 同一个类（真机 vtable 0x12bd470）的 slot +0x20（`{0x44af80, 0x12bd498}` 重定位行），
     用 'Stage'/'２Ｄバック（メイン画面）' 族字符串搭场景层；记它是否也被驱动（count/LR/名字）。
   开销：门 97 每次 ≤5 次定长读（全过 m7v_rd：域护栏 + SIGSEGV 兜底），门 98 ≤3 次；各 16 槽环。
   Switch smo_m7y2（threads.c 每 2 s 刷新 g_m7y2_on；C 默认 0 ⇒ `echo 0` ≤2 s 回退）。 */
_Atomic int g_m7y2_on;
#define M7Y2_G1_SLOTS 16
#define M7Y2_G2_SLOTS 16
#define M7Y2_LR 8
#define M7Y2_NAME_MAX 24
typedef struct {
    uint64_t tick, lr, q, vt, tgt3, nm_ptr, arg;
    unsigned char nm[M7Y2_NAME_MAX];   /* [q+0xb8] 指向的类名字符串（原始字节，不做 ASCII 过滤） */
    uint32_t thr, rd, ok, nm_len;      /* rd: b0=[q] b1=[[q]+0x18] b2=[q+0xb8] b3=[q+0xb8] 指向可读 */
} m7y2_g1_rec_t;
typedef struct {
    uint64_t tick, lr, q, vt, nm_ptr, arg;
    unsigned char nm[M7Y2_NAME_MAX];
    uint32_t thr, rd, nm_len;
} m7y2_g2_rec_t;
static struct { _Atomic uint64_t n; m7y2_g1_rec_t s[M7Y2_G1_SLOTS]; } g_m7y2_g1;
static struct { _Atomic uint64_t n; m7y2_g2_rec_t s[M7Y2_G2_SLOTS]; } g_m7y2_g2;
static struct {
    _Atomic uint64_t k, lr_ovf; _Atomic unsigned fset;
    uint64_t f_tick, f_lr, f_q;
    _Atomic uint64_t l_tick, l_lr, l_q;
    struct { uint64_t lr; _Atomic uint64_t n, f_tick, l_tick; } lr[M7Y2_LR];
} g_m7y2_g1t;
static struct {
    _Atomic uint64_t k, lr_ovf; _Atomic unsigned fset;
    uint64_t f_tick, f_lr, f_q;
    _Atomic uint64_t l_tick, l_lr, l_q;
    struct { uint64_t lr; _Atomic uint64_t n, f_tick, l_tick; } lr[M7Y2_LR];
} g_m7y2_g2t;
/* 读类名：p = [q+0xb8]（三族一致的类名格），读 ≤M7Y2_NAME_MAX 字节原始串进 dst */
static uint32_t m7y2_read_name(uint64_t q, uint64_t *nm_ptr, unsigned char *dst) {
    uint64_t p = 0;
    *nm_ptr = 0;
    if (!m7v_rd(&p, q + 0xb8, 8)) return 0;
    *nm_ptr = p;
    if (!m7v_ptr_ok(p)) return 0;
    if (!m7v_rd(dst, p, M7Y2_NAME_MAX)) return 0;
    uint32_t n = 0;
    while (n < M7Y2_NAME_MAX && dst[n]) n++;
    return n ? (n | 0x100u) : 0;   /* bit8 = 指向处可读 */
}
static void m7y2_g1_capture(uint64_t lr, uint64_t q, uint64_t arg, uint32_t thr) {
    uint64_t vt = 0, tgt3 = 0, nm_ptr = 0;
    uint32_t rd = 0, ok = 0, nmlen = 0;
    unsigned char nm[M7Y2_NAME_MAX]; nm[0] = 0;
    if (m7v_rd(&vt, q, 8)) rd |= 0x01;
    if (m7v_ptr_ok(vt) && m7v_rd(&tgt3, vt + 0x18, 8)) { rd |= 0x02; ok = (uint32_t)m7v_ptr_ok(tgt3); }
    uint32_t nr = m7y2_read_name(q, &nm_ptr, nm);
    if (nm_ptr) rd |= 0x04;
    if (nr & 0x100u) { rd |= 0x08; nmlen = nr & 0xffu; }
    uint64_t slot = atomic_fetch_add_explicit(&g_m7y2_g1.n, 1, memory_order_relaxed);
    if (slot < M7Y2_G1_SLOTS) {                            /* 认领即写、单写、不复写 */
        m7y2_g1_rec_t *r = &g_m7y2_g1.s[slot];
        r->tick = smo_obs_tick(); r->lr = lr; r->q = q;
        r->vt = vt; r->tgt3 = tgt3; r->nm_ptr = nm_ptr; r->arg = arg;
        for (uint32_t i = 0; i < M7Y2_NAME_MAX; i++) r->nm[i] = nm[i];
        r->thr = thr; r->rd = rd; r->ok = ok; r->nm_len = nmlen;
    }
    uint64_t t = smo_obs_tick();
    atomic_fetch_add_explicit(&g_m7y2_g1t.k, 1, memory_order_relaxed);
    atomic_store_explicit(&g_m7y2_g1t.l_tick, t, memory_order_relaxed);
    atomic_store_explicit(&g_m7y2_g1t.l_lr, lr, memory_order_relaxed);
    atomic_store_explicit(&g_m7y2_g1t.l_q, q, memory_order_relaxed);
    if (!atomic_load_explicit(&g_m7y2_g1t.fset, memory_order_relaxed)) {
        g_m7y2_g1t.f_tick = t; g_m7y2_g1t.f_lr = lr; g_m7y2_g1t.f_q = q;
        atomic_store_explicit(&g_m7y2_g1t.fset, 1, memory_order_relaxed);
    }
    int ls = -1;
    for (int i = 0; i < M7Y2_LR && ls < 0; i++) if (g_m7y2_g1t.lr[i].lr == lr) ls = i;
    if (ls < 0) for (int i = 0; i < M7Y2_LR && ls < 0; i++) if (!g_m7y2_g1t.lr[i].lr) { g_m7y2_g1t.lr[i].lr = lr; ls = i; }
    if (ls >= 0) {
        if (!atomic_load_explicit(&g_m7y2_g1t.lr[ls].n, memory_order_relaxed))
            atomic_store_explicit(&g_m7y2_g1t.lr[ls].f_tick, t, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_m7y2_g1t.lr[ls].n, 1, memory_order_relaxed);
        atomic_store_explicit(&g_m7y2_g1t.lr[ls].l_tick, t, memory_order_relaxed);
    } else atomic_fetch_add_explicit(&g_m7y2_g1t.lr_ovf, 1, memory_order_relaxed);
}
static void m7y2_g2_capture(uint64_t lr, uint64_t q, uint64_t arg, uint32_t thr) {
    uint64_t vt = 0, nm_ptr = 0;
    uint32_t rd = 0, nmlen = 0;
    unsigned char nm[M7Y2_NAME_MAX]; nm[0] = 0;
    if (m7v_rd(&vt, q, 8)) rd |= 0x01;
    uint32_t nr = m7y2_read_name(q, &nm_ptr, nm);
    if (nm_ptr) rd |= 0x04;
    if (nr & 0x100u) { rd |= 0x08; nmlen = nr & 0xffu; }
    uint64_t slot = atomic_fetch_add_explicit(&g_m7y2_g2.n, 1, memory_order_relaxed);
    if (slot < M7Y2_G2_SLOTS) {
        m7y2_g2_rec_t *r = &g_m7y2_g2.s[slot];
        r->tick = smo_obs_tick(); r->lr = lr; r->q = q;
        r->vt = vt; r->nm_ptr = nm_ptr; r->arg = arg;
        for (uint32_t i = 0; i < M7Y2_NAME_MAX; i++) r->nm[i] = nm[i];
        r->thr = thr; r->rd = rd; r->nm_len = nmlen;
    }
    uint64_t t = smo_obs_tick();
    atomic_fetch_add_explicit(&g_m7y2_g2t.k, 1, memory_order_relaxed);
    atomic_store_explicit(&g_m7y2_g2t.l_tick, t, memory_order_relaxed);
    atomic_store_explicit(&g_m7y2_g2t.l_lr, lr, memory_order_relaxed);
    atomic_store_explicit(&g_m7y2_g2t.l_q, q, memory_order_relaxed);
    if (!atomic_load_explicit(&g_m7y2_g2t.fset, memory_order_relaxed)) {
        g_m7y2_g2t.f_tick = t; g_m7y2_g2t.f_lr = lr; g_m7y2_g2t.f_q = q;
        atomic_store_explicit(&g_m7y2_g2t.fset, 1, memory_order_relaxed);
    }
    int ls = -1;
    for (int i = 0; i < M7Y2_LR && ls < 0; i++) if (g_m7y2_g2t.lr[i].lr == lr) ls = i;
    if (ls < 0) for (int i = 0; i < M7Y2_LR && ls < 0; i++) if (!g_m7y2_g2t.lr[i].lr) { g_m7y2_g2t.lr[i].lr = lr; ls = i; }
    if (ls >= 0) {
        if (!atomic_load_explicit(&g_m7y2_g2t.lr[ls].n, memory_order_relaxed))
            atomic_store_explicit(&g_m7y2_g2t.lr[ls].f_tick, t, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_m7y2_g2t.lr[ls].n, 1, memory_order_relaxed);
        atomic_store_explicit(&g_m7y2_g2t.lr[ls].l_tick, t, memory_order_relaxed);
    } else atomic_fetch_add_explicit(&g_m7y2_g2t.lr_ovf, 1, memory_order_relaxed);
}
/* ---- M7Z2（2026-11-08）：**选择点分叉** —— 五门（99 = main+0x6c7d30 谓词入口 /
   100 = main+0x4b8e98 跳过路径 / 101 = main+0x4b8ea4 做活路径 / 102 = main+0x4b8e88 cbnz 不成立处 /
   103 = main+0x4b8e20 HostTypeNrvLoadFirstSequence::execute 入口）。
   离线定性（见枚举处注释与 /tmp/m7z2_dis_*.py）：
     * 0x6c7d30 是**通用"状态时间 <= N"谓词**（114 个调用者、8 个同族成员），
       不是 HostType 的 ready/login/首启查询；0x21c = 540 = **帧阈值**（≈9 s @60 Hz），不是枚举。
     * 选择点 = HostTypeNrvLoadFirstSequence::execute 里"要不要现在就去调场景的 slot+0x20
       并转 NrvFirstSequence"：`[this+0x58]` 非 0 **或** state_time > 540 就做，否则每帧 ret。
     * [this+0x58] 的**唯一"置 1"写点**（全镜像 strb #0x58 扫描 + bl 上溯）= `0x4b8d20`
       （`mov w8,#1; strb w8,[x0,#0x58]; ret`），调用者只有两处：`0x4b51dc`、`0x4b6a54`，
       两处都以 **x0 = [宿主+0xc8]** 传入（HakoniwaSequence 侧数据加载完成时置位）。
   观测设计（零行为改动，只读寄存器 + m7v_rd 域护栏定长读）：
     G1 全命中计数 + **本站点**（LR=0x4b8e94）环/首末/每-LR（93 个别的静态调用者会污染全量环，
        所以环与台账只认 LR=0x4b8e94；k 仍记全量）。
     G2 x0 = 查询返回（1=跳过）/ x8 = 标志字节 / LR 分辨来路（0x4b8e94 查询跳过 vs 0x4b8e7c/70 早退）。
     G3 x0 = 查询返回（0）/ LR=0x4b8e94 ⇒ 经查询放行，否则 cbnz 直接放行。
     G4 x8 = [this+0x58] 原始字节（只在标志为 0 时到达）。
     G5 x0 = 宿主 this → 读 [this] / [[this]] / [this+0x58] / [this+0x20] / [this+0x60]（门4"补读"，
        读的是宿主本身；M7Y2 的 G2 门点 x0 是场景 q，[q+0x58] 不是选择点读的那一格）。
   开关 smo_m7z2（threads.c 每 2 s 刷新 g_m7z2_on；C 默认 0 ⇒ `echo 0` ≤2 s 回退）。 */
_Atomic int g_m7z2_on;
#define M7Z2_NG 5
#define M7Z2_RING 16
#define M7Z2_LR 8
#define M7Z2_SITE_LR 0x4b8e94ULL      /* 0x4b8e90 bl 0x6c7d30 的返回地址 = 本站点 */
typedef struct {
    uint64_t tick, lr, x0, x1, x8, a, b, c, d, e;
    uint32_t thr, seq;
} m7z2_rec_t;
typedef struct {
    _Atomic uint64_t k;                        /* 全命中（含别的调用者） */
    _Atomic uint64_t ks;                       /* 本站点命中（G1 用 LR 过滤；其余门 ks==k） */
    _Atomic uint64_t lr_ovf;
    _Atomic unsigned fset;
    uint64_t f_tick, f_lr, f_x0, f_x1, f_x8, f_a, f_b, f_c, f_d, f_e;
    _Atomic uint64_t l_tick, l_lr, l_x0, l_x1, l_x8, l_a, l_b, l_c, l_d, l_e;
    _Atomic uint64_t rot;                      /* 旋转环游标（保留最近 16 条） */
    m7z2_rec_t r[M7Z2_RING];
    struct { _Atomic uint64_t lr, n, f_tick, l_tick, l_x0, l_x8, l_a, l_b, l_c, l_d, l_e; } lr[M7Z2_LR];
} m7z2_gate_t;
static m7z2_gate_t g_m7z2[M7Z2_NG];
/* gi: 0..4；x1 = 门点原 x1（w3），x8 = 门点原 x8（w2），lr = 门点 LR（w4）。
   a/b/c = 该门额外解出的定长读（G5 用；其余传 0）。 */
static void m7z2_cap(int gi, uint64_t x0, uint64_t x1, uint64_t x8, uint64_t lr,
                     uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint32_t thr) {
    if (gi < 0 || gi >= M7Z2_NG) return;
    m7z2_gate_t *g = &g_m7z2[gi];
    atomic_fetch_add_explicit(&g->k, 1, memory_order_relaxed);
    if (gi == 0 && lr != M7Z2_SITE_LR) return;   /* G1：环/台账只认本站点（93 个别的静态调用者） */
    atomic_fetch_add_explicit(&g->ks, 1, memory_order_relaxed);
    uint64_t t = smo_obs_tick();
    atomic_store_explicit(&g->l_tick, t, memory_order_relaxed);
    atomic_store_explicit(&g->l_lr, lr, memory_order_relaxed);
    atomic_store_explicit(&g->l_x0, x0, memory_order_relaxed);
    atomic_store_explicit(&g->l_x1, x1, memory_order_relaxed);
    atomic_store_explicit(&g->l_x8, x8, memory_order_relaxed);
    atomic_store_explicit(&g->l_a, a, memory_order_relaxed);
    atomic_store_explicit(&g->l_b, b, memory_order_relaxed);
    atomic_store_explicit(&g->l_c, c, memory_order_relaxed);
    atomic_store_explicit(&g->l_d, d, memory_order_relaxed);
    atomic_store_explicit(&g->l_e, e, memory_order_relaxed);
    if (!atomic_load_explicit(&g->fset, memory_order_relaxed)) {
        g->f_tick = t; g->f_lr = lr; g->f_x0 = x0; g->f_x1 = x1; g->f_x8 = x8;
        g->f_a = a; g->f_b = b; g->f_c = c; g->f_d = d; g->f_e = e;
        atomic_store_explicit(&g->fset, 1, memory_order_relaxed);
    }
    uint64_t rr = atomic_fetch_add_explicit(&g->rot, 1, memory_order_relaxed);
    m7z2_rec_t *r = &g->r[rr % M7Z2_RING];       /* 旋转环：保留最近 16 条（dump 侧可能读到半行 = 观测语义，disclosed） */
    r->seq = (uint32_t)(rr + 1);
    r->tick = t; r->lr = lr; r->x0 = x0; r->x1 = x1; r->x8 = x8;
    r->a = a; r->b = b; r->c = c; r->d = d; r->e = e; r->thr = thr;
    int ls = -1;
    for (int i = 0; i < M7Z2_LR && ls < 0; i++) if (g->lr[i].lr == lr) ls = i;
    if (ls < 0) for (int i = 0; i < M7Z2_LR && ls < 0; i++) if (!g->lr[i].lr) { g->lr[i].lr = lr; ls = i; }
    if (ls >= 0) {
        if (!atomic_load_explicit(&g->lr[ls].n, memory_order_relaxed))
            atomic_store_explicit(&g->lr[ls].f_tick, t, memory_order_relaxed);
        atomic_fetch_add_explicit(&g->lr[ls].n, 1, memory_order_relaxed);
        atomic_store_explicit(&g->lr[ls].l_tick, t, memory_order_relaxed);
        atomic_store_explicit(&g->lr[ls].l_x0, x0, memory_order_relaxed);
        atomic_store_explicit(&g->lr[ls].l_x8, x8, memory_order_relaxed);
        atomic_store_explicit(&g->lr[ls].l_a, a, memory_order_relaxed);
        atomic_store_explicit(&g->lr[ls].l_b, b, memory_order_relaxed);
        atomic_store_explicit(&g->lr[ls].l_c, c, memory_order_relaxed);
        atomic_store_explicit(&g->lr[ls].l_d, d, memory_order_relaxed);
        atomic_store_explicit(&g->lr[ls].l_e, e, memory_order_relaxed);
    } else atomic_fetch_add_explicit(&g->lr_ovf, 1, memory_order_relaxed);
}
/* ---- M8B（2026-11-08）：**序列对象 nerve 状态机** —— 两门（104 = main+0x6c7750 NerveKeeper::setNerve
   / 105 = main+0x4b8d30 HostTypeNrvBoot::execute 入口）。见枚举处注释（门点逐条 / nerve 对象布局 /
   token = 名字格 − 0x10）。
   记录设计（零行为改动：只读寄存器 + ≤3 次 m7v_rd 定长读，无 I/O、无 libc）：
     G1 每事件：tick / x1(新 token) / [keeper+0x10](旧 token) / [keeper+0x18](旧步数) / LR / tick，
        **首 128 条逐事件环**（认领即写、不覆盖 ⇒ 保住 boot 早期序列）+ 最近 16 条旋转环（末见段），
        **每 token 计数表 40 槽**（token / 次数 / 首末 tick / 末见 LR / 名字串前 63 B），
        + 每 LR 台账 16 槽（谁在写 nerve）。
        名字：读 [x1+0x10] = nerve 对象的名字格（char*），再一次性 guarded 读 64 B 原样存下（不 ASCII 过滤）。
     G2 只计数 + 每 LR 台账 16 槽 + 首末 tick（宿主状态机在模拟器上的活动量）。
   开关 smo_m8b（threads.c 每 2 s 刷新 g_m8b_on；C 默认 0 ⇒ `echo 0` ≤2 s 回退）。
   ⚠️ 名字用 m8b：`smo_m8b`/`[m8b]`/`m8b_boot_run.sh`/`/tmp/m8b_report.md`/`logs/m8b_*` 五类开工前
   逐条 grep/stat 全 0 命中（2026-11-08 14:0x）⇒ 直接用 m8b，不启用 m8b2。 */
_Atomic int g_m8b_on;
/* M8D：复用 M8B 的 m8b_g_t/m8b_cap 骨架（gi = 2/3），只加两格 + 一个独立开关。
   gi 0/1 = 104/105（g_m8b_on 控），gi 2/3 = 106/107（g_m8d_on 控）⇒ 两轮开关互不干扰。 */
_Atomic int g_m8d_on;
#define M8B_NG 4
#define M8B_RING 128                 /* 逐事件环（首 128 条，认领即写、不覆盖） */
#define M8B_LAST 16                  /* 最近 16 条旋转环 */
#define M8B_TOK 40                   /* 每 token 计数表槽数 */
#define M8B_LR 16                    /* 每 LR 台账槽数 */
#define M8B_NAMELEN 64
typedef struct {
    uint64_t tick, x1, old10, old18, lr, keeper;
    char name[M8B_NAMELEN];
} m8b_rec_t;
typedef struct {
    uint64_t token;
    _Atomic uint64_t n, f_tick, l_tick, l_lr, l_keeper;
    char name[M8B_NAMELEN];
} m8b_tok_t;
typedef struct {
    _Atomic uint64_t k;                       /* 总命中 */
    _Atomic unsigned fset;
    uint64_t f_tick, f_x1, f_old10, f_old18, f_lr, f_keeper;
    char f_name[M8B_NAMELEN];
    _Atomic uint64_t l_tick, l_x1, l_old10, l_old18, l_lr, l_keeper;
    char l_name[M8B_NAMELEN];
    _Atomic uint64_t rot;                     /* 首环游标（≥M8B_RING 后不再写） */
    m8b_rec_t r[M8B_RING];
    _Atomic uint64_t lrot;                    /* 最近 16 条环游标 */
    m8b_rec_t last[M8B_LAST];
    _Atomic unsigned tok_n;                   /* 已占用 token 槽数（只增） */
    _Atomic uint64_t tok_ovf;
    m8b_tok_t tok[M8B_TOK];
    _Atomic uint64_t lr_ovf;
    struct { uint64_t lr; _Atomic uint64_t n, f_tick, l_tick; } lr[M8B_LR];
} m8b_g_t;
static m8b_g_t g_m8b[M8B_NG];
/* 读 nerve 名字串。**两种 token 形态**（离线 reloc 全量扫 + 运行期首跑各证一半）：
     ① 直接 nerve 对象：token = 对象 base+0x10，[token] = base（自指针），**名字格 = token+0x10**；
     ② nerve 槽（表项）：token = 某 nerve 指针表的槽地址，[token] = 当前 nerve 对象 base，
        **名字格 = [[token]]+0x20**（HakoniwaSequence / HostType / FirstSequenceScene 三族的表都是这个形态）。
   先按 ① 读；首字节不是可打印 ASCII 就按 ② 再读一次。每次 ≤3 次 guarded 定长读。 */
static void m8b_name(char dst[M8B_NAMELEN], uint64_t token) {
    uint64_t np = 0;
    dst[0] = 0;
    if (!m7v_rd(&np, token + 0x10, 8)) return;
    if (m7v_ptr_ok(np) && m7v_rd(dst, np, M8B_NAMELEN - 1)) {
        dst[M8B_NAMELEN - 1] = 0;
        if (dst[0] >= 0x20 && dst[0] <= 0x7e) return;       /* ① 命中 */
    }
    dst[0] = 0;
    uint64_t base = 0, np2 = 0;
    if (!m7v_rd(&base, token, 8) || !m7v_ptr_ok(base)) return;
    if (!m7v_rd(&np2, base + 0x20, 8) || !m7v_ptr_ok(np2)) return;
    if (!m7v_rd(dst, np2, M8B_NAMELEN - 1)) { dst[0] = 0; return; }
    dst[M8B_NAMELEN - 1] = 0;
}
/* gi: 0 = G1(104) / 1 = G2(105)；x1 = 门点原 x1（w3）、x0 = 门点原 x0、lr = 门点 LR（w4） */
static void m8b_cap(int gi, uint64_t x0, uint64_t x1, uint64_t lr) {
    if (gi < 0 || gi >= M8B_NG) return;
    m8b_g_t *g = &g_m8b[gi];
    atomic_fetch_add_explicit(&g->k, 1, memory_order_relaxed);
    uint64_t t = smo_obs_tick();
    uint64_t old10 = 0, old18 = 0;
    char nm[M8B_NAMELEN];
    nm[0] = 0;
    if (gi == 0) {                              /* G1：keeper 的两格 + token 的名字格 */
        if (m7v_ptr_ok(x0)) {
            m7v_rd(&old10, x0 + 0x10, 8);       /* 旧 nerve token */
            m7v_rd(&old18, x0 + 0x18, 8);       /* 旧步数（低 32 位有符号） */
        }
        m8b_name(nm, x1);
    }
    if (!atomic_load_explicit(&g->fset, memory_order_relaxed)) {
        g->f_tick = t; g->f_x1 = x1; g->f_old10 = old10; g->f_old18 = old18; g->f_lr = lr; g->f_keeper = x0;
        memcpy(g->f_name, nm, M8B_NAMELEN);
        atomic_store_explicit(&g->fset, 1, memory_order_relaxed);
    }
    atomic_store_explicit(&g->l_tick, t, memory_order_relaxed);
    atomic_store_explicit(&g->l_x1, x1, memory_order_relaxed);
    atomic_store_explicit(&g->l_old10, old10, memory_order_relaxed);
    atomic_store_explicit(&g->l_old18, old18, memory_order_relaxed);
    atomic_store_explicit(&g->l_lr, lr, memory_order_relaxed);
    atomic_store_explicit(&g->l_keeper, x0, memory_order_relaxed);
    memcpy(g->l_name, nm, M8B_NAMELEN);
    if (gi == 0) {
        uint64_t rr = atomic_fetch_add_explicit(&g->rot, 1, memory_order_relaxed);
        if (rr < M8B_RING) {                    /* 首 128 条：认领即写，绝不覆盖 */
            m8b_rec_t *r = &g->r[rr];
            r->tick = t; r->x1 = x1; r->old10 = old10; r->old18 = old18; r->lr = lr; r->keeper = x0;
            memcpy(r->name, nm, M8B_NAMELEN);
        }
        uint64_t lr2 = atomic_fetch_add_explicit(&g->lrot, 1, memory_order_relaxed);
        m8b_rec_t *q = &g->last[lr2 % M8B_LAST];    /* 最近 16 条（旋转，dump 侧可能读到半行 = 观测语义） */
        q->tick = t; q->x1 = x1; q->old10 = old10; q->old18 = old18; q->lr = lr; q->keeper = x0;
        memcpy(q->name, nm, M8B_NAMELEN);
        /* 每 token 计数表：先找同值，再占空槽（只增不减，溢出计入 tok_ovf） */
        int found = -1;
        unsigned used = atomic_load_explicit(&g->tok_n, memory_order_relaxed);
        if (used > M8B_TOK) used = M8B_TOK;
        for (unsigned i = 0; i < used && found < 0; i++)
            if (g->tok[i].token == x1) found = (int)i;
        if (found < 0) {
            unsigned slot = atomic_fetch_add_explicit(&g->tok_n, 1, memory_order_relaxed);
            if (slot < M8B_TOK) {
                g->tok[slot].token = x1;
                memcpy(g->tok[slot].name, nm, M8B_NAMELEN);
                found = (int)slot;
            } else atomic_fetch_add_explicit(&g->tok_ovf, 1, memory_order_relaxed);
        }
        if (found >= 0) {
            m8b_tok_t *tk = &g->tok[found];
            if (!atomic_load_explicit(&tk->n, memory_order_relaxed))
                atomic_store_explicit(&tk->f_tick, t, memory_order_relaxed);
            atomic_fetch_add_explicit(&tk->n, 1, memory_order_relaxed);
            atomic_store_explicit(&tk->l_tick, t, memory_order_relaxed);
            atomic_store_explicit(&tk->l_lr, lr, memory_order_relaxed);
            atomic_store_explicit(&tk->l_keeper, x0, memory_order_relaxed);
        }
    }
    int ls = -1;
    for (int i = 0; i < M8B_LR && ls < 0; i++) if (g->lr[i].lr == lr) ls = i;
    if (ls < 0) for (int i = 0; i < M8B_LR && ls < 0; i++) if (!g->lr[i].lr) { g->lr[i].lr = lr; ls = i; }
    if (ls >= 0) {
        if (!atomic_load_explicit(&g->lr[ls].n, memory_order_relaxed))
            atomic_store_explicit(&g->lr[ls].f_tick, t, memory_order_relaxed);
        atomic_fetch_add_explicit(&g->lr[ls].n, 1, memory_order_relaxed);
        atomic_store_explicit(&g->lr[ls].l_tick, t, memory_order_relaxed);
    } else atomic_fetch_add_explicit(&g->lr_ovf, 1, memory_order_relaxed);
}
extern __thread uint32_t runtime_cur_handle_tls;   /* threads.c, svc-side store of me->handle */
/* ---- M8E2（2026-11-08）：**第一个分叉**（DemoOpening vs LoadStage）的四门定长记录 ----
   不复用 m8b_g_t：本轮的语义格与 M8B 不同（G1 要"判定值"、G4 要"返回 x0"、G3 要"Stage 入参串"）。
   记录口径（零行为改动：只读寄存器 + ≤3 次 guarded 定长读，无 I/O、无 libc）：
     gi 0 = 108：x0 = A（= [HakoniwaSequence+0xb8]）⇒ 再读 B = [A+0x20]、判定字节 = u8(B+0x5f8)
                 ⇒ 存 x1 = B、x2 = 判定字节（**原始值，不做位运算**，位判断留给 dump 侧）
     gi 1 = 109：x0 = 序列对象、x1 = 新 token（0x12d0420）、LR = 分叉口上游
     gi 2 = 110：x0 = 场景对象、x1 = 名字入参指针 ⇒ 抄前 39 B（"Stage" 就靠这一格认）
     gi 3 = 111：x0 = **函数返回 x0**、LR = 返回地址
   每门：命中数 / 首末 tick / 首末四格 / 16 条环 / 8 槽 LR 台账。开关 smo_m8e2（C 默认 0；
   `echo 0` ≤2 s 回退；关着仍有"前 16 次必记"）。 */
_Atomic int g_m8e2_on;
#define M8E2_NG 4
#define M8E2_RING 16
#define M8E2_LR 8
#define M8E2_NAMELEN 40
typedef struct { uint64_t tick, x0, x1, x2, lr; char name[M8E2_NAMELEN]; } m8e2_rec_t;
typedef struct {
    _Atomic uint64_t k, fset;
    uint64_t f_tick, f_x0, f_x1, f_x2, f_lr;
    _Atomic uint64_t l_tick, l_x0, l_x1, l_x2, l_lr;
    _Atomic uint64_t rot;
    m8e2_rec_t r[M8E2_RING];
    struct { uint64_t lr; _Atomic uint64_t n, f_tick, l_tick; } lr[M8E2_LR];
    _Atomic uint64_t lr_ovf;
} m8e2_g_t;
static m8e2_g_t g_m8e2[M8E2_NG];
static void m8e2_cap(int gi, uint64_t x0, uint64_t x1, uint64_t lr) {
    if (gi < 0 || gi >= M8E2_NG) return;
    m8e2_g_t *g = &g_m8e2[gi];
    uint64_t t = smo_obs_tick();
    uint64_t a = 0, b = 0;
    char nm[M8E2_NAMELEN];
    nm[0] = 0;
    if (gi == 0) {
        /* 判定值：与 main+0x4d2bd0 自己走的同一条路（A → A+0x20 → +0x5f8 低字节） */
        uint64_t inner = 0;
        uint8_t byte = 0;
        if (m7v_ptr_ok(x0) && m7v_rd(&inner, x0 + 0x20, 8) && m7v_ptr_ok(inner) &&
            m7v_rd(&byte, inner + 0x5f8, 1)) { a = inner; b = byte; }
    } else if (gi == 1) {
        a = x1;                                  /* 新 token（应 = 0x12d0420 BootLoadData） */
    } else if (gi == 2) {
        if (m7v_ptr_ok(x1) && m7v_rd(nm, x1, M8E2_NAMELEN - 1)) nm[M8E2_NAMELEN - 1] = 0;
        else nm[0] = 0;
    } else {
        a = x0;                                  /* 111：返回 x0 */
    }
    atomic_fetch_add_explicit(&g->k, 1, memory_order_relaxed);
    if (!atomic_load_explicit(&g->fset, memory_order_relaxed)) {
        g->f_tick = t; g->f_x0 = x0; g->f_x1 = a; g->f_x2 = b; g->f_lr = lr;
        atomic_store_explicit(&g->fset, 1, memory_order_relaxed);
    }
    atomic_store_explicit(&g->l_tick, t, memory_order_relaxed);
    atomic_store_explicit(&g->l_x0, x0, memory_order_relaxed);
    atomic_store_explicit(&g->l_x1, a, memory_order_relaxed);
    atomic_store_explicit(&g->l_x2, b, memory_order_relaxed);
    atomic_store_explicit(&g->l_lr, lr, memory_order_relaxed);
    uint64_t kk = atomic_fetch_add_explicit(&g->rot, 1, memory_order_relaxed);
    if (kk < M8E2_RING) {
        m8e2_rec_t *r = &g->r[kk];
        r->tick = t; r->x0 = x0; r->x1 = a; r->x2 = b; r->lr = lr;
        memcpy(r->name, nm, M8E2_NAMELEN);
    }
    int ls = -1;
    for (int i = 0; i < M8E2_LR && ls < 0; i++) if (g->lr[i].lr == lr) ls = i;
    if (ls < 0) for (int i = 0; i < M8E2_LR && ls < 0; i++) if (!g->lr[i].lr) { g->lr[i].lr = lr; ls = i; }
    if (ls >= 0) {
        if (!atomic_load_explicit(&g->lr[ls].n, memory_order_relaxed))
            atomic_store_explicit(&g->lr[ls].f_tick, t, memory_order_relaxed);
        atomic_fetch_add_explicit(&g->lr[ls].n, 1, memory_order_relaxed);
        atomic_store_explicit(&g->lr[ls].l_tick, t, memory_order_relaxed);
    } else atomic_fetch_add_explicit(&g->lr_ovf, 1, memory_order_relaxed);
}
/* ---- M10A（2026-11-08）：**把首启判据 bit0 扳成 1**（本项目第一次"改返回值"实验）----
   目标：门 108（main+0x4b4bc0 bl 0x4d2bd0）那一次调用上让调用方看到 bit0=1，从而落到
   `0x4b4bc8`（A 支），看 A 族（DemoOpening→…→FirstSequenceSceneNrvSelectMode）会不会自己长出来。
   机制（**只改 w0 的 bit0，别的一律不动**）：
     · 门 108 的 tail=3 单 bl 门换成 **35 字变体**（只在**安装期** smo_m10a≠0 时装；=0 时装原 27 字骨架，
       与 M9C 逐字节相同）。变体在"原样重放那条 bl"（= 真调用 0x4d2bd0，原始返回值在 w0）之后插入：
         w27 ldr x9,[pc,#24] → &smo_obs_m10a_st
         w28 ldr w10,[x9]        ← mask（0/1，由观察器每次命中前写好）
         w29 str w0,[x9,#4]      ← **原始返回值**（不预做位运算）
         w30 orr w0,w0,w10       ← mask=0 时语义上是恒等（bit0 与其它 7 位原样保留）
         w31 str w0,[x9,#8]      ← **返回给调用方的值**
         w32 b 回门点+4
         w33..w34 .quad &smo_obs_m10a_st
       ⇒ 只写 w0；x9/x10 在被换出的那条 bl 处**不是活值**（落穿侧 0x4b4bf0/0x4b4bf4 自己重写 x9/x10；
         跳空侧直进尾声只 ldp/ret）；NZCV 在两条后继路径上**无消费者**。
     · 生效次数：观察器给 mask，`smo_m10a=1` ⇒ 只对**前 N 次**命中置 1（N 默认 1，文件第二字符可给 1..3）；
       `smo_m10a=2` ⇒ mask 恒 0（只记 raw/ret，不改行为，作对照）。
     · 门 112 = `0x4b4bc8`（A 支入口）：命中数 = tbz 落空次数（全镜像零旁路落点），x0 = **调用方真正看到的
       返回值**（门 108 stub 的 orr 结果）⇒ "返回值确实被改了"与"A 支确实被走进"两件事各有独立读数。
   命名：`smo_m10a` / `[m10a]` / `m10a_boot_run.sh` / `/tmp/m10a_report.md` / `logs/m10a_*` 五类开工前全 0 命中
   （2026-11-08 15:5x）。 */
#define M10A_PATH_A "/data/storage/el2/base/files/smo_m10a"
#define M10A_PATH_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m10a"
_Atomic int g_m10a_mode;        /* 运行期：0 关 / 1 强制 / 2 只记（threads.c 每 2 s 刷新） */
_Atomic int g_m10a_limit = 1;   /* 强制次数上限（1..3，随模式文件第二字符） */
_Atomic int g_m10a_install;     /* 安装期读到的模式（决定 stub 形态；0 ⇒ 原骨架 = 与 M9C 字节相同） */
/* 变体 stub 直接读写的格子：mask@+0 / raw@+4 / ret@+8（**偏移写死在机器码里**，勿改字段顺序/类型）。 */
typedef struct {
    _Atomic int mask, raw, ret;
    _Atomic int hits, forced;          /* 门 108 命中数 / 实际置位次数 */
    _Atomic uint64_t f_tick, l_tick;   /* 首末命中 tick */
} m10a_st_t;
static m10a_st_t g_m10a_st;
#define M10A_RING 8
/* 逐次命中环：raw/ret 是"上一次调用"读数（观察器在 bl **之前**跑、拿不到本次返回值），
   所以第 k 格由第 k+1 次命中时的快照填；最后一次的 raw/ret 用 sum 行里的 last_raw/last_ret。 */
static struct { uint64_t tick; int forced; int raw; int ret; } g_m10a_r[M10A_RING];
static _Atomic uint64_t g_m10a_rn;
/* 门 112（A 支入口）计数格 */
static struct {
    _Atomic uint64_t k, f_tick, l_tick, f_x0, l_x0, f_lr, l_lr;
} g_m10a_a;

/* ==================== M15A（2026-11-08）：门 113 = 把 x30 变成直方图 ====================
   目的：M14A 证明 `main+0x988a20`（NVN submit 入口）有 **190 个 bl 站点 / 150 个宿主函数**，
   从 LR 往上两级静态上是 150 路分叉；本轮只记**入口 x30**（= 调用者返回址），用它的分布把
   "谁画了那笔黑"收敛成具体一支。三份互不依赖的读数：
     ① 直方图 `g_m15a_h[]`：按 x30 桶（开放寻址，512 桶）**全量**计数，并分早期/后期两栏
        （`sub<1536` / `sub>=1536`——黑层从 sub≈1536 起永久出现，两栏可直接对比"谁多出来了"）；
     ② 待领队列 `g_m15a_pend[]`：**每笔 draw 领走自上一笔以来的命中**（port 侧 nvn_vk.c 在 draw
        点 drain）⇒ 每笔 draw 的 x30 是**逐笔归属**，不是按帧猜的顺序；
     ③ 首末快照 + 溢出计数（队列溢出 = 该笔的归属丢了，必须计入报告，不能当 0）。
   观察器约束（沿用既有骨架口径）：只做原子计数与定长写，**无锁 / 无 I/O / 无 libc / 不解引用
   门点指针**。`m15a_port_sub()` 是 nvn_vk.c 里的**叶函数**（只返回 g_lw_sub 这个全局），
   stub 上下文调它一次 = 一次普通函数调用，与调 smo_obs_observer 同类，不做任何 I/O。
   ⚠️ 名字串（x0+0x358）**不在观察器里读**：那里没有 fault guard，读坏一页就是整轮 boot 崩。
   观察器只把 A 记进待领队列，port 侧用 runtime_safe_read_ungated 读（SIGSEGV 兜底）。 */
#define M15A_HIST_N   512           /* 直方图桶数（distinct 站点 ≤190，装得下） */
#define M15A_PEND_N   24            /* 每笔 draw 领走的待领队列（实测 ~2 命中/笔） */
_Atomic int g_m15a_on;              /* 运行期开关（threads.c 每 beat 刷新；C 默认 0） */
static _Atomic uint64_t g_m15a_k;   /* 门 113 累计命中数（= submit 入口被调次数） */
static _Atomic uint64_t g_m15a_pend_ovf;   /* 待领队列溢出（归属丢失，必须报） */
static _Atomic uint64_t g_m15a_h_ovf;      /* 直方图桶溢出 */
static _Atomic uint64_t g_m15a_h_used;     /* 已用桶数（= distinct x30 数） */
static _Atomic uint64_t g_m15a_first_tick, g_m15a_last_tick;
static _Atomic uint64_t g_m15a_first_lr, g_m15a_last_lr;
static _Atomic uint64_t g_m15a_a_first, g_m15a_a_last;
typedef struct { uint64_t lr, a; } m15a_pend_t;
static m15a_pend_t g_m15a_pend[M15A_PEND_N];
static _Atomic int g_m15a_pend_n;
typedef struct { uint64_t lr, n_early, n_late, a_last; } m15a_h_t;
static m15a_h_t g_m15a_h[M15A_HIST_N];
extern unsigned m15a_port_sub(void);   /* nvn_vk.c：当前 guest 帧号（叶函数，只读全局） */
static void m15a_hist_add(uint64_t lr, int late)
{
    unsigned h = (unsigned)((lr >> 2) & (M15A_HIST_N - 1));
    for (unsigned i = 0; i < 64u; i++) {
        m15a_h_t *e = &g_m15a_h[(h + i) & (M15A_HIST_N - 1)];
        uint64_t have = e->lr;
        if (have == lr) {
            if (late) e->n_late++; else e->n_early++;
            return;
        }
        if (have == 0) {
            e->lr = lr;                        /* 单写者（render 线程）；dump 侧容忍读到半行 */
            if (late) e->n_late = 1; else e->n_early = 1;
            atomic_fetch_add_explicit(&g_m15a_h_used, 1, memory_order_relaxed);
            return;
        }
    }
    atomic_fetch_add_explicit(&g_m15a_h_ovf, 1, memory_order_relaxed);
}

/* ==================== M16A（2026-11-08）：门 114 = "GORP." trampoline 入口（7 参版）====================
   门点 main+0x991a30（FDE size 0x80）静态形态（镜像逐字反汇编，capstone）：
     stp x24,x23,[sp,#-0x40]! ; stp x22,x21,[sp,#0x10] ; stp x20,x19,[sp,#0x20] ; stp x29,x30,[sp,#0x30]
     add x29,sp,#0x30 ; adrp x8,0x136b000 ; ldr x8,[x8,#0xdf8] ; …（5 次 ldr：+0/+0x30/+0x10/+8/+0x18）
     x24 = 命令缓冲；x23/x22/x21 = 原 x0/x1/x2；x20/x19 = 原 x3/x4
     mov w2,#1 ; mov x1,x23 ; mov x0,x24 ; bl 0x988a20（= 门 113 的帧常量环绑定）
     x2..x5 = 原 x1,x2,x3,x4 ; 复原全部 callee-saved ; x0 = 原 x0 ; x1 = x24 ; b 0x991ab0（公共 2D 执行器）
   ⇒ **全函数零条件分支**（0x80 B 只有 1 个 bl + 1 个尾 b）⇒ "该停画/该减 alpha"不可能在本函数内。
   观察器（本文件）：只做原子计数 + 定长写。**不解引用任何参数**（名字/对象内容由 port 侧
   runtime_safe_read_ungated 读——stub 上下文里做带 setjmp 的安全读会嵌套 fault guard）。
   载荷 7 参来自 **30 字专用变体 stub**（w11/w12 两条 ldr 把门点原 x2/x3 也交给观察器）：
     x0=arg0 / x1=arg1 / x2=arg2 / x3=arg3 / x30=调用者返回址（+ x8 备查）。
   "每帧几次" = per-frame 计数（按 m15a_port_sub() 换帧清零）+ 记录窗口内**最大值**；
   逐笔归属仍走 port 侧 drain（nvn_vk.c 每笔 draw 领走自上一笔以来的命中）。 */
#define M16A_HIST_N   512
#define M16A_PEND_N   64
_Atomic int g_m16a_on;                    /* 运行期开关（threads.c 每 beat 刷新；C 默认 0） */
static _Atomic uint64_t g_m16a_k, g_m16a_h_ovf, g_m16a_h_used, g_m16a_pend_ovf;
static _Atomic uint64_t g_m16a_cur_sub, g_m16a_per_frame, g_m16a_max_per_frame;
static _Atomic uint64_t g_m16a_first_tick, g_m16a_last_tick, g_m16a_first_lr, g_m16a_last_lr;
static _Atomic uint64_t g_m16a_x0_last, g_m16a_x1_last, g_m16a_x2_last, g_m16a_x3_last;
static _Atomic uint64_t g_m16a_x0_first, g_m16a_sub_first, g_m16a_sub_last;
typedef struct { uint64_t lr, x0, x1, x2, x3; } m16a_pend_t;
static m16a_pend_t g_m16a_pend[M16A_PEND_N];
static _Atomic int g_m16a_pend_n;
typedef struct { uint64_t lr, n_early, n_late; } m16a_h_t;
static m16a_h_t g_m16a_h[M16A_HIST_N];
extern unsigned m15a_port_sub(void);      /* nvn_vk.c 叶函数：当前 guest 帧号（只读 g_lw_sub） */
static void m16a_hist_add(uint64_t lr, int late)
{
    unsigned h = (unsigned)((lr >> 2) & (M16A_HIST_N - 1));
    for (int i = 0; i < 8; i++) {
        m16a_h_t *e = &g_m16a_h[(h + i) & (M16A_HIST_N - 1)];
        uint64_t have = __atomic_load_n(&e->lr, __ATOMIC_RELAXED);
        if (have == 0) {
            uint64_t exp = 0;
            if (!__atomic_compare_exchange_n(&e->lr, &exp, lr, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                if (exp != lr) continue;
            }
            if (late) e->n_late = 1; else e->n_early = 1;
            atomic_fetch_add_explicit(&g_m16a_h_used, 1, memory_order_relaxed);
            return;
        }
        if (have == lr) {
            if (late) e->n_late++; else e->n_early++;
            return;
        }
    }
    atomic_fetch_add_explicit(&g_m16a_h_ovf, 1, memory_order_relaxed);
}
/* 门 114 专用观察器（7 参；只有 30 字变体 stub 指向这里）。 */
static void smo_obs_observer_m16a(uint64_t x0, uint64_t gate, uint64_t x8, uint64_t x1,
                                  uint64_t lr, uint64_t x2, uint64_t x3)
{
    (void)x8;
    if (gate != SMO_OBS_M16A_GORP) return;
    if (!atomic_load_explicit(&g_m16a_on, memory_order_relaxed)) return;
    uint64_t t = smo_obs_tick();
    atomic_fetch_add_explicit(&g_m16a_k, 1, memory_order_relaxed);
    unsigned sub = m15a_port_sub();
    uint64_t cs = atomic_load_explicit(&g_m16a_cur_sub, memory_order_relaxed);
    if ((uint64_t)sub != cs) {
        atomic_store_explicit(&g_m16a_cur_sub, (uint64_t)sub, memory_order_relaxed);
        atomic_store_explicit(&g_m16a_per_frame, 0ull, memory_order_relaxed);
    }
    uint64_t pf = atomic_fetch_add_explicit(&g_m16a_per_frame, 1ull, memory_order_relaxed) + 1ull;
    if (pf > atomic_load_explicit(&g_m16a_max_per_frame, memory_order_relaxed))
        atomic_store_explicit(&g_m16a_max_per_frame, pf, memory_order_relaxed);
    m16a_hist_add(lr, sub >= 1536u);
    if (!atomic_load_explicit(&g_m16a_first_tick, memory_order_relaxed)) {
        atomic_store_explicit(&g_m16a_first_tick, t, memory_order_relaxed);
        atomic_store_explicit(&g_m16a_first_lr, lr, memory_order_relaxed);
        atomic_store_explicit(&g_m16a_x0_first, x0, memory_order_relaxed);
        atomic_store_explicit(&g_m16a_sub_first, (uint64_t)sub, memory_order_relaxed);
    }
    atomic_store_explicit(&g_m16a_last_tick, t, memory_order_relaxed);
    atomic_store_explicit(&g_m16a_last_lr, lr, memory_order_relaxed);
    atomic_store_explicit(&g_m16a_sub_last, (uint64_t)sub, memory_order_relaxed);
    atomic_store_explicit(&g_m16a_x0_last, x0, memory_order_relaxed);
    atomic_store_explicit(&g_m16a_x1_last, x1, memory_order_relaxed);
    atomic_store_explicit(&g_m16a_x2_last, x2, memory_order_relaxed);
    atomic_store_explicit(&g_m16a_x3_last, x3, memory_order_relaxed);
    int pn = atomic_load_explicit(&g_m16a_pend_n, memory_order_relaxed);
    if (pn < M16A_PEND_N) {
        g_m16a_pend[pn].lr = lr; g_m16a_pend[pn].x0 = x0; g_m16a_pend[pn].x1 = x1;
        g_m16a_pend[pn].x2 = x2; g_m16a_pend[pn].x3 = x3;
        atomic_store_explicit(&g_m16a_pend_n, pn + 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&g_m16a_pend_ovf, 1, memory_order_relaxed);
    }
}
/* ---- M17A（2026-11-08）：门 115 = main+0x98b4b0（2D 四边形记录叶 fn 0x98b490 的 +0x20）----
   与门 114 同款 30 字 7 参变体 stub（w11/w12 把门点原 x2/x3 也交给观察器）。区别只在
   **存储**与**语义**：这是"每画一个带贴图的四边形就到这里一次"的叶子点（静态 277 站/130 宿主，
   每帧 ≈ 每帧 draw 数），所以逐笔归属（port 侧每笔 draw 领走自上一笔以来的命中）能把它
   直接钉到"这一笔是哪个宿主画的"。
   载荷：x0=渲染上下文 / x1 / x2=四边形描述符（**[x2] = 贴图键，-1 = 不画**，-1 在 0x98b498
   就早退、根本到不了本门）/ x3 / x30=**宿主返回址**。
   ⚠️ 观察器里**不解引用 x2**（stub 上下文做带 setjmp 的安全读会嵌套 fault guard，见 M15A 注）；
   [x2] 由 port 侧 drain 时用 runtime_safe_read_ungated 读。 */
#define M17A_PEND_N   64
#define M17A_HIST_N   512
_Atomic int g_m17a_on;                    /* 运行期开关（threads.c 每 beat 刷新；C 默认 0） */
static _Atomic uint64_t g_m17a_k, g_m17a_h_ovf, g_m17a_h_used, g_m17a_pend_ovf;
static _Atomic uint64_t g_m17a_cur_sub, g_m17a_per_frame, g_m17a_max_per_frame;
static _Atomic uint64_t g_m17a_first_tick, g_m17a_last_tick, g_m17a_first_lr, g_m17a_last_lr;
static _Atomic uint64_t g_m17a_x0_last, g_m17a_x1_last, g_m17a_x2_last, g_m17a_x3_last;
static _Atomic uint64_t g_m17a_x0_first, g_m17a_sub_first, g_m17a_sub_last;
typedef struct { uint64_t lr, x0, x1, x2, x3; } m17a_pend_t;
static m17a_pend_t g_m17a_pend[M17A_PEND_N];
static _Atomic int g_m17a_pend_n;
typedef struct { uint64_t lr, n_early, n_late; } m17a_h_t;
static m17a_h_t g_m17a_h[M17A_HIST_N];
static void m17a_hist_add(uint64_t lr, int late)
{
    unsigned h = (unsigned)((lr >> 2) & (M17A_HIST_N - 1));
    for (int i = 0; i < 8; i++) {
        m17a_h_t *e = &g_m17a_h[(h + i) & (M17A_HIST_N - 1)];
        uint64_t have = __atomic_load_n(&e->lr, __ATOMIC_RELAXED);
        if (have == 0) {
            uint64_t exp = 0;
            if (!__atomic_compare_exchange_n(&e->lr, &exp, lr, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                if (exp != lr) continue;
            }
            if (late) e->n_late = 1; else e->n_early = 1;
            atomic_fetch_add_explicit(&g_m17a_h_used, 1, memory_order_relaxed);
            return;
        }
        if (have == lr) {
            if (late) e->n_late++; else e->n_early++;
            return;
        }
    }
    atomic_fetch_add_explicit(&g_m17a_h_ovf, 1, memory_order_relaxed);
}
/* 门 115 专用观察器（7 参；只有 30 字变体 stub 指向这里）。 */
static void smo_obs_observer_m17a(uint64_t x0, uint64_t gate, uint64_t x8, uint64_t x1,
                                  uint64_t lr, uint64_t x2, uint64_t x3)
{
    (void)x8;
    if (gate != SMO_OBS_M17A_QUAD) return;
    if (!atomic_load_explicit(&g_m17a_on, memory_order_relaxed)) return;
    uint64_t t = smo_obs_tick();
    atomic_fetch_add_explicit(&g_m17a_k, 1, memory_order_relaxed);
    unsigned sub = m15a_port_sub();
    uint64_t cs = atomic_load_explicit(&g_m17a_cur_sub, memory_order_relaxed);
    if ((uint64_t)sub != cs) {
        atomic_store_explicit(&g_m17a_cur_sub, (uint64_t)sub, memory_order_relaxed);
        atomic_store_explicit(&g_m17a_per_frame, 0ull, memory_order_relaxed);
    }
    uint64_t pf = atomic_fetch_add_explicit(&g_m17a_per_frame, 1ull, memory_order_relaxed) + 1ull;
    if (pf > atomic_load_explicit(&g_m17a_max_per_frame, memory_order_relaxed))
        atomic_store_explicit(&g_m17a_max_per_frame, pf, memory_order_relaxed);
    m17a_hist_add(lr, sub >= 1536u);
    if (!atomic_load_explicit(&g_m17a_first_tick, memory_order_relaxed)) {
        atomic_store_explicit(&g_m17a_first_tick, t, memory_order_relaxed);
        atomic_store_explicit(&g_m17a_first_lr, lr, memory_order_relaxed);
        atomic_store_explicit(&g_m17a_x0_first, x0, memory_order_relaxed);
        atomic_store_explicit(&g_m17a_sub_first, (uint64_t)sub, memory_order_relaxed);
    }
    atomic_store_explicit(&g_m17a_last_tick, t, memory_order_relaxed);
    atomic_store_explicit(&g_m17a_last_lr, lr, memory_order_relaxed);
    atomic_store_explicit(&g_m17a_sub_last, (uint64_t)sub, memory_order_relaxed);
    atomic_store_explicit(&g_m17a_x0_last, x0, memory_order_relaxed);
    atomic_store_explicit(&g_m17a_x1_last, x1, memory_order_relaxed);
    atomic_store_explicit(&g_m17a_x2_last, x2, memory_order_relaxed);
    atomic_store_explicit(&g_m17a_x3_last, x3, memory_order_relaxed);
    int pn = atomic_load_explicit(&g_m17a_pend_n, memory_order_relaxed);
    if (pn < M17A_PEND_N) {
        g_m17a_pend[pn].lr = lr; g_m17a_pend[pn].x0 = x0; g_m17a_pend[pn].x1 = x1;
        g_m17a_pend[pn].x2 = x2; g_m17a_pend[pn].x3 = x3;
        atomic_store_explicit(&g_m17a_pend_n, pn + 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&g_m17a_pend_ovf, 1, memory_order_relaxed);
    }
}
/* ---- M18A（2026-11-08）：门 116/117 = 「UI 颜色入参」两门 ----
   目的：回答"谁把 UI 文字/面板的颜色写成 (0,0,0,1)"。门 116 直接把**颜色指针 x3**（= [ctx+0xb8]+0x658）
   与"这一笔的调用点 LR"一起带出来；门 117 把**上层 UI 层对象 x0 与渲染上下文 x1**一起带出来。
   两条都在 port 侧 drain 时做安全读（观察器在 stub 上下文里不解引用，理由同 M15A 注）：
     · 116：读 [x3] 16 B ⇒ 4×u32 + 4×float（= 这一笔的颜色块）；
            读 [x0+0xb8] ⇒ 设备态指针 st，再读 [st+0x658] 16 B（应当 == [x3]，用来证 ABI）；
     · 117：读 [x0+0xf0..0xfc] 4×u32（UI 层颜色表，ctor 0x90f450 从资源块灌进来的 4 个 RGBA8）；
            读 [x1+0xb8] ⇒ st，再读 [st+0x658] 16 B（= 本函数马上要拿去当颜色画的那块）。
   队列是两个门共用的（带 gate 字段），条数 256，溢出只计数不覆盖（观测用途，不保证不丢）。 */
#define M18A_PEND_N   256
_Atomic int g_m18a_on;                    /* 运行期开关（threads.c 每 beat 刷新；C 默认 0） */
static _Atomic uint64_t g_m18a_k[2], g_m18a_ovf[2];
static _Atomic uint64_t g_m18a_x0_last, g_m18a_x1_last, g_m18a_x3_last, g_m18a_lr_last;
static _Atomic uint64_t g_m18a_sub_first, g_m18a_sub_last;
typedef struct { uint32_t gate, sub; uint64_t lr, x0, x1, x2, x3; } m18a_pend_t;
static m18a_pend_t g_m18a_pend[M18A_PEND_N];
static _Atomic int g_m18a_pend_n;
static void smo_obs_observer_m18a(uint64_t x0, uint64_t gate, uint64_t x8, uint64_t x1,
                                  uint64_t lr, uint64_t x2, uint64_t x3)
{
    (void)x8;
    int gi = (gate == (uint64_t)SMO_OBS_M18A_UIFILL) ? 0
           : (gate == (uint64_t)SMO_OBS_M18A_PANEL) ? 1 : -1;
    if (gi < 0) return;
    if (!atomic_load_explicit(&g_m18a_on, memory_order_relaxed)) return;
    unsigned sub = m15a_port_sub();
    atomic_fetch_add_explicit(&g_m18a_k[gi], 1, memory_order_relaxed);
    if (!atomic_load_explicit(&g_m18a_sub_first, memory_order_relaxed))
        atomic_store_explicit(&g_m18a_sub_first, (uint64_t)sub, memory_order_relaxed);
    atomic_store_explicit(&g_m18a_sub_last, (uint64_t)sub, memory_order_relaxed);
    atomic_store_explicit(&g_m18a_x0_last, x0, memory_order_relaxed);
    atomic_store_explicit(&g_m18a_x1_last, x1, memory_order_relaxed);
    atomic_store_explicit(&g_m18a_x3_last, x3, memory_order_relaxed);
    atomic_store_explicit(&g_m18a_lr_last, lr, memory_order_relaxed);
    int pn = atomic_load_explicit(&g_m18a_pend_n, memory_order_relaxed);
    if (pn < M18A_PEND_N) {
        g_m18a_pend[pn].gate = (uint32_t)gate; g_m18a_pend[pn].sub = sub;
        g_m18a_pend[pn].lr = lr; g_m18a_pend[pn].x0 = x0; g_m18a_pend[pn].x1 = x1;
        g_m18a_pend[pn].x2 = x2; g_m18a_pend[pn].x3 = x3;
        atomic_store_explicit(&g_m18a_pend_n, pn + 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&g_m18a_ovf[gi], 1, memory_order_relaxed);
    }
}
/* ---- M19A（2026-11-08）：门 118 = main+0x916a28（2D 批处理器 main+0x915dfc 的 +0xc2c）----
   目的：把 (a)「该通道没被启用（+0x8b0..8b2 标志=0 ⇒ RGB 被无条件清零）」与
   (b)「元素级颜色取到 0」在**同一笔上**分开。门点紧跟在 +0x9161c8/+0x9161f0/+0x916218 的
   条件清零之后、`str q0,[x19,#0x320]` 之前 ⇒ 读到的 x8+0x700/704/708 就是**已经过清零**的
   RGB 源；标志字节 x8+0x8b0/8b1/8b2 决定"该通道是否被启用"。
   载荷（30 字 7 参变体）：x0（门点当时的 x0）/ gate / **x8 = [x19+0x10] 状态对象** /
   **x1 = 批上下文 x19** / LR / x2 / x3。全部解引用留给 port 侧 drain（安全读）。 */
#define M19A_PEND_N   4096
_Atomic int g_m19a_on;                    /* 运行期开关（threads.c 每 beat 刷新；C 默认 0） */
static _Atomic uint64_t g_m19a_k, g_m19a_ovf;
static _Atomic uint64_t g_m19a_lr_last, g_m19a_ctx_last, g_m19a_st_last;
static _Atomic uint64_t g_m19a_x0_last, g_m19a_x3_last, g_m19a_sub_first, g_m19a_sub_last;
typedef struct { uint32_t sub; uint64_t lr, x0, x1, x2, x3, x8; } m19a_pend_t;
static m19a_pend_t g_m19a_pend[M19A_PEND_N];
static _Atomic int g_m19a_pend_n;
static void smo_obs_observer_m19a(uint64_t x0, uint64_t gate, uint64_t x8, uint64_t x1,
                                  uint64_t lr, uint64_t x2, uint64_t x3)
{
    if (gate != (uint64_t)SMO_OBS_M19A_RGB) return;
    if (!atomic_load_explicit(&g_m19a_on, memory_order_relaxed)) return;
    unsigned sub = m15a_port_sub();
    atomic_fetch_add_explicit(&g_m19a_k, 1, memory_order_relaxed);
    if (!atomic_load_explicit(&g_m19a_sub_first, memory_order_relaxed))
        atomic_store_explicit(&g_m19a_sub_first, (uint64_t)sub, memory_order_relaxed);
    atomic_store_explicit(&g_m19a_sub_last, (uint64_t)sub, memory_order_relaxed);
    atomic_store_explicit(&g_m19a_lr_last, lr, memory_order_relaxed);
    atomic_store_explicit(&g_m19a_ctx_last, x1, memory_order_relaxed);
    atomic_store_explicit(&g_m19a_st_last, x8, memory_order_relaxed);
    atomic_store_explicit(&g_m19a_x0_last, x0, memory_order_relaxed);
    atomic_store_explicit(&g_m19a_x3_last, x3, memory_order_relaxed);
    int pn = atomic_load_explicit(&g_m19a_pend_n, memory_order_relaxed);
    if (pn < M19A_PEND_N) {
        g_m19a_pend[pn].sub = sub;
        g_m19a_pend[pn].lr = lr; g_m19a_pend[pn].x0 = x0; g_m19a_pend[pn].x1 = x1;
        g_m19a_pend[pn].x2 = x2; g_m19a_pend[pn].x3 = x3; g_m19a_pend[pn].x8 = x8;
        atomic_store_explicit(&g_m19a_pend_n, pn + 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&g_m19a_ovf, 1, memory_order_relaxed);
    }
}
static _Atomic uint64_t g_obs_hits[SMO_OBS_N], g_obs_x0[SMO_OBS_N];
static uint64_t g_obs_main_base;   /* 安装期在宿主侧写好（main 模块基址），观察器只读（round21 备用） */
static uint64_t g_obs_stub_off[SMO_OBS_N];   /* M7b: tail=3 门安装后的 stub main 偏移（dump 侧 LR→site 翻译；0=未装） */
static uint64_t g_obs_rx_lo, g_obs_rx_hi;   /* 尾部 stub 页的 RX 区间（安装成功后填；check_permissions 查询） */
/* 客体小栈上被 veneer 调进来的观察器：只做原子计数与定长拷贝，无锁/无 I/O/无 libc。
   gate=x1（stub 里 movz 立即数），x0=入口原参；round16b 起 x2=门点原 x8、x3=门点原 x1
   （被 movz gate 覆盖前的原值）；round21 起 x4=门点 LR（prologue w10 从 [sp,#8] 取回
   w0 已入栈的 x30：blr 调用⇒调用点+4，br 尾跳⇒更上层返回地址）。其余门不读 x2/x3/x4，
   传了也是废参。round21：E1/CELL2/DRAIN_LOOP/KICKVT/KICKBR 路径随门退役删除。 */
static void smo_obs_observer(uint64_t x0, uint64_t gate, uint64_t x2, uint64_t x3, uint64_t x4) {
    if (gate >= SMO_OBS_N) return;
    atomic_fetch_add_explicit(&g_obs_hits[gate], 1, memory_order_relaxed);
    atomic_store_explicit(&g_obs_x0[gate], x0, memory_order_relaxed);
    (void)x2; (void)g_obs_main_base;
    /* M15A ①（2026-11-08）：门 113 = NVN submit 入口 main+0x988a20。**每个 draw 前都会到这里**
       （M14A 静态：190 bl / 150 宿主），所以本分支是全项目命中率最高的一只门（实测 ~70 次/帧）。
       载荷：x0 = A（NVN 命令对象）、x4 = x30 = 调用者返回址（stub 参 5 从 [sp,#8] 取回的 LR）。
       只做：全局计数 / 直方图桶 +1 / 待领队列追加 / 首末快照。**不解引用 x0**（名字留 port 侧读）。
       早期/后期分栏靠 m15a_port_sub()（叶函数，读 g_lw_sub）：<1536 = 黑层出现前，>=1536 = 永久盖黑段。 */
    if (gate == SMO_OBS_M15A_X30) {
        if (!atomic_load_explicit(&g_m15a_on, memory_order_relaxed)) return;
        uint64_t t = smo_obs_tick();
        uint64_t kk = atomic_fetch_add_explicit(&g_m15a_k, 1, memory_order_relaxed);
        (void)kk;
        m15a_hist_add(x4, m15a_port_sub() >= 1536u);
        if (!atomic_load_explicit(&g_m15a_first_tick, memory_order_relaxed)) {
            atomic_store_explicit(&g_m15a_first_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g_m15a_first_lr, x4, memory_order_relaxed);
            atomic_store_explicit(&g_m15a_a_first, x0, memory_order_relaxed);
        }
        atomic_store_explicit(&g_m15a_last_tick, t, memory_order_relaxed);
        atomic_store_explicit(&g_m15a_last_lr, x4, memory_order_relaxed);
        atomic_store_explicit(&g_m15a_a_last, x0, memory_order_relaxed);
        int pn = atomic_load_explicit(&g_m15a_pend_n, memory_order_relaxed);
        if (pn < M15A_PEND_N) {
            g_m15a_pend[pn].lr = x4; g_m15a_pend[pn].a = x0;
            atomic_store_explicit(&g_m15a_pend_n, pn + 1, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&g_m15a_pend_ovf, 1, memory_order_relaxed);
        }
        return;
    }
    /* M7w ①：SENDTO_REJ（0x9646a0 拒绝合流）。x0=this（三条拒绝路径在 0x9646a0 前都不写
       x0）、x3=门点原 x1=msg、x4=LR（仍是 sendTo 调用者的返回址）。拒绝类在护栏内
       反推（state/last 各 1 次定长读），分桶 + 末 20 次环；环行 fetch_add 认领、单写、
       不复写（dump 侧可能读到半行 = 观测语义， disclosed）。 */
    if (gate == SMO_OBS_SENDTO_REJ) {
        if (atomic_load_explicit(&g_m7w_on, memory_order_relaxed)) {
            uint64_t msg = x3, state = 0, last = 0;
            if (x0 > 0x100000000ULL && x0 < 0x800000000000ULL) {
                state = *(const volatile uint32_t *)(const void *)(uintptr_t)(x0 + 0xd4);
                last  = *(const volatile uint64_t *)(const void *)(uintptr_t)(x0 + 0xc8);
            }
            if (msg == 0)                        atomic_fetch_add_explicit(&g_m7w_rej.n_msg0, 1, memory_order_relaxed);
            else if (state < 3 || state > 5)     atomic_fetch_add_explicit(&g_m7w_rej.n_state, 1, memory_order_relaxed);
            else if (last == msg)                atomic_fetch_add_explicit(&g_m7w_rej.n_dedup, 1, memory_order_relaxed);
            else                                 atomic_fetch_add_explicit(&g_m7w_rej.n_odd, 1, memory_order_relaxed);
            uint64_t kk = atomic_fetch_add_explicit(&g_m7w_rej.k, 1, memory_order_relaxed);
            if (kk < 20) {
                g_m7w_rej.r[kk].tick = smo_obs_tick(); g_m7w_rej.r[kk].this_ = x0;
                g_m7w_rej.r[kk].msg = msg; g_m7w_rej.r[kk].state = state;
                g_m7w_rej.r[kk].last = last; g_m7w_rej.r[kk].lr = x4;
            }
        }
        return;
    }
    /* M7w ②：BRC（广播器 0x5b8d54 入口）。x0=this、x4=入口 LR=虚调用者返回址。
       只记环，dump 侧再解 sink vtable（本观察器不做任何解引用）。 */
    if (gate == SMO_OBS_BRC) {
        if (atomic_load_explicit(&g_m7w_on, memory_order_relaxed)) {
            uint64_t kk = atomic_fetch_add_explicit(&g_m7w_brc.k, 1, memory_order_relaxed);
            if (kk < 200) {
                g_m7w_brc.r[kk].tick = smo_obs_tick(); g_m7w_brc.r[kk].this_ = x0; g_m7w_brc.r[kk].lr = x4;
            }
        }
        return;
    }
    /* M7a (1): M7A_NULL (fnB entry, x0=req). Guard like the TP family; 3 name-window
       words + 2 deref words. Per-hit cost = 8 guarded fixed reads + 2 atomic adds,
       same class as SENDTO_REJ. */
    if (gate == SMO_OBS_M7A_NULL) {
        if (atomic_load_explicit(&g_m7a_on, memory_order_relaxed)) {
            uint64_t vt = 0, st = 0, res = 0, k[6] = { 0, 0, 0, 0, 0, 0 }, d0 = 0, d1 = 0;
            if (x0 > 0x100000000ULL && x0 < 0x800000000000ULL) {
                vt  = *(const volatile uint64_t *)(const void *)(uintptr_t)x0;
                st  = *(const volatile uint32_t *)(const void *)(uintptr_t)(x0 + 0x60);
                res = *(const volatile uint64_t *)(const void *)(uintptr_t)(x0 + 0x110);
                /* M7b fix: the real request name is the find key - an inline C string at
                   slot+0x08 (find 0x615e50 strcmps exactly slot+0x8 through helper
                   0x615ca0 = add x0,x0,#8). 48 bytes, NUL inside for normal names. */
                k[0] = *(const volatile uint64_t *)(const void *)(uintptr_t)(x0 + 0x08);
                k[1] = *(const volatile uint64_t *)(const void *)(uintptr_t)(x0 + 0x10);
                k[2] = *(const volatile uint64_t *)(const void *)(uintptr_t)(x0 + 0x18);
                k[3] = *(const volatile uint64_t *)(const void *)(uintptr_t)(x0 + 0x20);
                k[4] = *(const volatile uint64_t *)(const void *)(uintptr_t)(x0 + 0x28);
                k[5] = *(const volatile uint64_t *)(const void *)(uintptr_t)(x0 + 0x30);
                if (k[0] > 0x100000000ULL && k[0] < 0x800000000000ULL) {   /* non-inline flavor: k0 is a pointer */
                    d0 = *(const volatile uint64_t *)(const void *)(uintptr_t)k[0];
                    d1 = *(const volatile uint64_t *)(const void *)(uintptr_t)(k[0] + 8);
                }
            }
            if (res) atomic_fetch_add_explicit(&g_m7a_null.n_res, 1, memory_order_relaxed);
            else     atomic_fetch_add_explicit(&g_m7a_null.n_null, 1, memory_order_relaxed);
            uint64_t kk = atomic_fetch_add_explicit(&g_m7a_null.k, 1, memory_order_relaxed);
            /* M7b fix: last-16 overwrite ring (kk&15) instead of the first-16 claim - the
               tail of the completion stream near the stop is the interesting part. */
            unsigned row = (unsigned)(kk & 15);
            g_m7a_null.r[row].tick = smo_obs_tick(); g_m7a_null.r[row].vt = vt;
            g_m7a_null.r[row].state = st; g_m7a_null.r[row].result = res;
            g_m7a_null.r[row].k0 = k[0]; g_m7a_null.r[row].k1 = k[1];
            g_m7a_null.r[row].k2 = k[2]; g_m7a_null.r[row].k3 = k[3];
            g_m7a_null.r[row].k4 = k[4]; g_m7a_null.r[row].k5 = k[5];
            g_m7a_null.r[row].d0 = d0; g_m7a_null.r[row].d1 = d1;
        }
        return;
    }
    /* M7a (2): M7A_REG (find entry, x0=registry vector). First 4 distinct latched. */
    if (gate == SMO_OBS_M7A_REG) {
        if (atomic_load_explicit(&g_m7a_on, memory_order_relaxed)) {
            unsigned n = atomic_load_explicit(&g_m7a_reg.n, memory_order_relaxed);
            for (unsigned i = 0; i < n && i < 4; i++)
                if (g_m7a_reg.vec[i] == x0) return;
            if (n < 4) { g_m7a_reg.vec[n] = x0; atomic_store_explicit(&g_m7a_reg.n, n + 1, memory_order_relaxed); }
        }
        return;
    }
    /* M7b (1): M7B_BR (poster 0x7aca60 entry). x0 = batch object {[x0+8]=pump,
       [x0+0x18]=dirty flag}, x4 = LR (caller site; a tail=3-gated site shows up as
       stub_k+0x54, the dump side translates through g_obs_stub_off). First-32 LR ring
       (fetch_add claim, no overwrite) + first/last snapshots with guarded reads. */
    if (gate == SMO_OBS_M7B_BR) {
        if (atomic_load_explicit(&g_m7b_on, memory_order_relaxed)) {
            uint64_t flag = 0, pump = 0;
            if (x0 > 0x100000000ULL && x0 < 0x800000000000ULL) {
                flag = *(const volatile uint8_t *)(const void *)(uintptr_t)(x0 + 0x18);
                pump = *(const volatile uint64_t *)(const void *)(uintptr_t)(x0 + 8);
            }
            uint64_t t = smo_obs_tick();
            uint64_t kk = atomic_fetch_add_explicit(&g_m7b_br.k, 1, memory_order_relaxed);
            if (kk < 32) atomic_store_explicit(&g_m7b_br.lr[kk], x4, memory_order_relaxed);
            if (!atomic_load_explicit(&g_m7b_br.fset, memory_order_relaxed)) {
                g_m7b_br.f_tick = t; g_m7b_br.f_x0 = x0; g_m7b_br.f_flag = flag;
                g_m7b_br.f_pump = pump; g_m7b_br.f_lr = x4;
                atomic_store_explicit(&g_m7b_br.fset, 1, memory_order_relaxed);
            }
            atomic_store_explicit(&g_m7b_br.l_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g_m7b_br.l_x0, x0, memory_order_relaxed);
            atomic_store_explicit(&g_m7b_br.l_flag, flag, memory_order_relaxed);
            atomic_store_explicit(&g_m7b_br.l_pump, pump, memory_order_relaxed);
            atomic_store_explicit(&g_m7b_br.l_lr, x4, memory_order_relaxed);
        }
        return;
    }
    /* M7b (2): the 13 poster call sites (ids C0..C0+12, one tail=3 gate per bl). Count +
       first/last per site; x0 = the batch object the site passes to the poster. */
    if (gate >= SMO_OBS_M7B_C0 && gate < SMO_OBS_M7B_C0 + 13) {
        if (atomic_load_explicit(&g_m7b_on, memory_order_relaxed)) {
            int k = (int)gate - SMO_OBS_M7B_C0;
            uint64_t t = smo_obs_tick();
            atomic_fetch_add_explicit(&g_m7b_c[k].k, 1, memory_order_relaxed);
            if (!atomic_load_explicit(&g_m7b_c[k].fset, memory_order_relaxed)) {
                g_m7b_c[k].f_tick = t; g_m7b_c[k].f_x0 = x0;
                atomic_store_explicit(&g_m7b_c[k].fset, 1, memory_order_relaxed);
            }
            atomic_store_explicit(&g_m7b_c[k].l_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g_m7b_c[k].l_x0, x0, memory_order_relaxed);
        }
        return;
    }
    /* M7b (3): fnA TLS-gate branch counters (0x615708 tbz). TRUE = 0x61570c (owner hit,
       synchronous resolve, bypasses registry/pump), FALSE = 0x6157f0 (submit path).
       f_tick==0 marks "no hit yet". */
    if (gate == SMO_OBS_M7B_GTRUE || gate == SMO_OBS_M7B_GFALSE) {
        if (atomic_load_explicit(&g_m7b_on, memory_order_relaxed)) {
            int k = gate == SMO_OBS_M7B_GTRUE ? 0 : 1;
            uint64_t t = smo_obs_tick();
            atomic_fetch_add_explicit(&g_m7b_g[k].k, 1, memory_order_relaxed);
            atomic_store_explicit(&g_m7b_g[k].l_tick, t, memory_order_relaxed);
            uint64_t f = atomic_load_explicit(&g_m7b_g[k].f_tick, memory_order_relaxed);
            if (!f) atomic_store_explicit(&g_m7b_g[k].f_tick, t, memory_order_relaxed);
        }
        return;
    }
    /* M7g: layer above the creator (4 gates, see the m7g ledger comment). Fixed-size writes +
       atomics only, no dereference at all. x1 comes in via stub arg-4 (original x1). */
    if (gate >= SMO_OBS_M7G_CRT && gate <= SMO_OBS_M7G_SIB) {
        if (atomic_load_explicit(&g_m7g_on, memory_order_relaxed)) {
            m7g_g_t *g = &g_m7g[gate - SMO_OBS_M7G_CRT];
            uint64_t t = smo_obs_tick();
            uint32_t thr = runtime_cur_handle_tls;
            uint64_t x1 = x3;
            atomic_fetch_add_explicit(&g->k, 1, memory_order_relaxed);
            atomic_store_explicit(&g->l_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g->l_x0, x0, memory_order_relaxed);
            atomic_store_explicit(&g->l_x1, x1, memory_order_relaxed);
            atomic_store_explicit(&g->l_lr, x4, memory_order_relaxed);
            atomic_store_explicit(&g->l_thr, thr, memory_order_relaxed);
            if (!atomic_load_explicit(&g->fset, memory_order_relaxed)) {
                g->f_tick = t; g->f_x0 = x0; g->f_x1 = x1; g->f_lr = x4; g->f_thr = thr;
                atomic_store_explicit(&g->fset, 1, memory_order_relaxed);
            }
            int pslot = -1;
            for (int i = 0; i < 8 && pslot < 0; i++)
                if (g->pt[i].thr == thr) pslot = i;
            if (pslot < 0)
                for (int i = 0; i < 8 && pslot < 0; i++)
                    if (!g->pt[i].thr) { g->pt[i].thr = thr; pslot = i; }
            if (pslot >= 0) atomic_fetch_add_explicit(&g->pt[pslot].n, 1, memory_order_relaxed);
            else atomic_fetch_add_explicit(&g->pt_ovf, 1, memory_order_relaxed);
            uint64_t kk = atomic_fetch_add_explicit(&g->rk, 1, memory_order_relaxed);
            if (kk >= 32) atomic_store_explicit(&g->r_ovf, kk - 31, memory_order_relaxed);
            unsigned slot = (unsigned)(kk & 31u);
            g->r[slot].tick = t; g->r[slot].thr = thr;
            g->r[slot].x0 = x0; g->r[slot].x1 = x1; g->r[slot].lr = x4;
            int lslot = -1;
            for (int i = 0; i < 8 && lslot < 0; i++)
                if (g->lr[i].lr == x4) lslot = i;
            if (lslot < 0)
                for (int i = 0; i < 8 && lslot < 0; i++)
                    if (!g->lr[i].lr) { g->lr[i].lr = x4; lslot = i; }
            if (lslot >= 0) atomic_fetch_add_explicit(&g->lr[lslot].n, 1, memory_order_relaxed);
            else atomic_fetch_add_explicit(&g->lr_ovf, 1, memory_order_relaxed);
        }
        return;
    }
    /* M7h：批循环停止方式（10 只门，ids 50..59）+ 查表函数头按 LR 归类。与 M7g 同形，
       但 LR 锁存 16 槽（12 个静态调用者）、环 16 行；全部定长写 + atomic、零解引用。 */
    if (gate >= SMO_OBS_M7H_BQ && gate <= SMO_OBS_M7L_PEXIT) {   /* M7L: ids 50..64 share one record form */
        if (atomic_load_explicit(&g_m7h_on, memory_order_relaxed)) {
            m7h_g_t *g = &g_m7h[gate - SMO_OBS_M7H_BQ];
            uint64_t t = smo_obs_tick();
            uint32_t thr = runtime_cur_handle_tls;
            uint64_t x1 = x3;
            atomic_fetch_add_explicit(&g->k, 1, memory_order_relaxed);
            atomic_store_explicit(&g->l_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g->l_x0, x0, memory_order_relaxed);
            atomic_store_explicit(&g->l_x1, x1, memory_order_relaxed);
            atomic_store_explicit(&g->l_lr, x4, memory_order_relaxed);
            atomic_store_explicit(&g->l_thr, thr, memory_order_relaxed);
            if (!atomic_load_explicit(&g->f_tick, memory_order_relaxed)) {
                g->f_tick = t; g->f_x0 = x0; g->f_x1 = x1;
            }
            int pslot = -1;
            for (int i = 0; i < 8 && pslot < 0; i++)
                if (g->pt[i].thr == thr) pslot = i;
            if (pslot < 0)
                for (int i = 0; i < 8 && pslot < 0; i++)
                    if (!g->pt[i].thr) { g->pt[i].thr = thr; pslot = i; }
            if (pslot >= 0) atomic_fetch_add_explicit(&g->pt[pslot].n, 1, memory_order_relaxed);
            else atomic_fetch_add_explicit(&g->pt_ovf, 1, memory_order_relaxed);
            uint64_t kk = atomic_fetch_add_explicit(&g->rk, 1, memory_order_relaxed);
            if (kk >= 16) atomic_store_explicit(&g->r_ovf, kk - 15, memory_order_relaxed);
            unsigned slot = (unsigned)(kk & 15u);
            g->r[slot].tick = t; g->r[slot].thr = thr;
            g->r[slot].lr = x4; g->r[slot].x0 = x0;
            int lslot = -1;
            for (int i = 0; i < 16 && lslot < 0; i++)
                if (g->lr[i].lr == x4) lslot = i;
            if (lslot < 0)
                for (int i = 0; i < 16 && lslot < 0; i++)
                    if (!g->lr[i].lr) { g->lr[i].lr = x4; lslot = i; }
            if (lslot >= 0) {
                atomic_fetch_add_explicit(&g->lr[lslot].n, 1, memory_order_relaxed);
                atomic_store_explicit(&g->lr[lslot].l_tick, t, memory_order_relaxed);   /* M7h: 每调用者末次 tick = 谁先停 */
            } else atomic_fetch_add_explicit(&g->lr_ovf, 1, memory_order_relaxed);
        }
        return;
    }
    /* M7P：worker Start main+0x963e50 —— 一门数全池。只读寄存器 + 护栏内 3 次定长读
       （[x1] epoch、[x0+0xd4] state、[x0+0xc8] tok）。x3 = 门点原 x1（prologue 的
       mov x3,x1），x4 = 门点 LR。每命中 = 1 次原子加 + 定长写，无 I/O、无锁。 */
    /* M7S2（2026-11-08）：池 B 构造入口三只门 —— G1 0x681b30（总入口）/ G2 0x74ca00（上层
       候选）/ G3 0x74cadc（bl@0x74cae0 前一字）。x0 = 门点原 x0、x3 = 门点原 x1、x2 = 门点
       原 x8、x4 = 门点 LR。只读寄存器、零解引用、零 I/O；每命中 = 2 次原子加 + 定长写。
       G3（index 2）按派活单只记「计数 + LR」：不写逐事件环，只进 per-LR 表。 */
    if (gate >= SMO_OBS_M7S2_G1 && gate <= SMO_OBS_M7S2_G3) {
        int si = gate - SMO_OBS_M7S2_G1;
        m7s2_g_t *g = &g_m7s2[si];
        if (atomic_load_explicit(&g->k, memory_order_relaxed) < 16 ||
            atomic_load_explicit(&g_m7s2_on, memory_order_relaxed)) {
            uint64_t t = smo_obs_tick();
            uint32_t thr = runtime_cur_handle_tls;
            uint64_t xv1 = x3, xv8 = x2;
            atomic_fetch_add_explicit(&g->k, 1, memory_order_relaxed);
            atomic_store_explicit(&g->l_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g->l_lr, x4, memory_order_relaxed);
            atomic_store_explicit(&g->l_x0, x0, memory_order_relaxed);
            atomic_store_explicit(&g->l_x1, xv1, memory_order_relaxed);
            if (!atomic_load_explicit(&g->fset, memory_order_relaxed)) {
                g->f_tick = t; g->f_lr = x4; g->f_x0 = x0; g->f_x1 = xv1; g->f_x8 = xv8;
                g->f_thr = thr;
                atomic_store_explicit(&g->fset, 1, memory_order_relaxed);
            }
            if (si != 2) {                 /* G1/G2：首 64 条逐事件行（认领即写，不覆盖） */
                uint64_t kk = atomic_fetch_add_explicit(&g->rk, 1, memory_order_relaxed);
                if (kk < M7S2_ROW) {
                    g->r[kk].tick = t; g->r[kk].lr = x4; g->r[kk].x0 = x0;
                    g->r[kk].x1 = xv1; g->r[kk].x8 = xv8; g->r[kk].thr = thr;
                } else atomic_fetch_add_explicit(&g->r_ovf, 1, memory_order_relaxed);
            }
            int lslot = -1;                /* per-LR（= per-call-site）计数表 */
            for (int i = 0; i < M7S2_LR && lslot < 0; i++) if (g->lr[i].lr == x4) lslot = i;
            if (lslot < 0)
                for (int i = 0; i < M7S2_LR && lslot < 0; i++) if (!g->lr[i].lr) { g->lr[i].lr = x4; lslot = i; }
            if (lslot >= 0) {
                if (!atomic_load_explicit(&g->lr[lslot].n, memory_order_relaxed))
                    atomic_store_explicit(&g->lr[lslot].f_tick, t, memory_order_relaxed);
                atomic_fetch_add_explicit(&g->lr[lslot].n, 1, memory_order_relaxed);
                atomic_store_explicit(&g->lr[lslot].l_tick, t, memory_order_relaxed);
            } else atomic_fetch_add_explicit(&g->lr_ovf, 1, memory_order_relaxed);
        }
        return;
    }
    /* M7T2（2026-11-08）：**整族九个头一起装门** —— H1..H9 = 0x445640 / 0x447b70 / 0x449c50 /
       0x44b720 / 0x46e7a0 / 0x4a1100 / 0x4a6b10 / 0x4a8820 / 0x4af0a0。x0 = 门点原 x0（= this）、
       x3 = 门点原 x1、x2 = 门点原 x8、x4 = 门点 LR（site = LR−4）。只读寄存器、零解引用、
       零 I/O；每命中 = 3 次原子加 + 定长写（不写逐事件环，按派活单口径只留 count/首末/LR 环）。 */
    if (gate >= SMO_OBS_M7T2_H1 && gate <= SMO_OBS_M7T2_H9) {
        m7t2_g_t *g = &g_m7t2[gate - SMO_OBS_M7T2_H1];
        if (atomic_load_explicit(&g->k, memory_order_relaxed) < 16 ||
            atomic_load_explicit(&g_m7t2_on, memory_order_relaxed)) {
            uint64_t t = smo_obs_tick();
            uint32_t thr = runtime_cur_handle_tls;
            uint64_t xv1 = x3, xv8 = x2;
            atomic_fetch_add_explicit(&g->k, 1, memory_order_relaxed);
            atomic_store_explicit(&g->l_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g->l_lr, x4, memory_order_relaxed);
            atomic_store_explicit(&g->l_x0, x0, memory_order_relaxed);
            atomic_store_explicit(&g->l_x1, xv1, memory_order_relaxed);
            if (!atomic_load_explicit(&g->fset, memory_order_relaxed)) {
                g->f_tick = t; g->f_lr = x4; g->f_x0 = x0; g->f_x1 = xv1; g->f_x8 = xv8;
                g->f_thr = thr;
                atomic_store_explicit(&g->fset, 1, memory_order_relaxed);
            }
            int lslot = -1;                /* per-LR（= per-call-site）32 槽环 */
            for (int i = 0; i < M7T2_LR && lslot < 0; i++) if (g->lr[i].lr == x4) lslot = i;
            if (lslot < 0)
                for (int i = 0; i < M7T2_LR && lslot < 0; i++) if (!g->lr[i].lr) { g->lr[i].lr = x4; lslot = i; }
            if (lslot >= 0) {
                if (!atomic_load_explicit(&g->lr[lslot].n, memory_order_relaxed))
                    atomic_store_explicit(&g->lr[lslot].f_tick, t, memory_order_relaxed);
                atomic_fetch_add_explicit(&g->lr[lslot].n, 1, memory_order_relaxed);
                atomic_store_explicit(&g->lr[lslot].l_tick, t, memory_order_relaxed);
            } else atomic_fetch_add_explicit(&g->lr_ovf, 1, memory_order_relaxed);
        }
        return;
    }
    /* M7U2（2026-11-08）：**场景派发枢纽三门** —— index 0=A 0x7acd50（入口）/
       1=B 0x7acd94（派发返回址，x0=被派发者返回值）/ 2=C 0x7acd70（stage 名取用点，
       x0=this、原 x1=stage 名指针、原 x8=[全局+0xc58] 槽值、LR=0x7acd78 之类的调用者）。
       每门：count + 首末见 + per-LR 16 槽（site = LR−4）。门 A 与门 C 额外做一次 stage 名抓取
       （m7u2_capture：只读、指针域护栏、最多 3 次定长读），结果进 8 槽名环，**不打印**。 */
    if (gate >= SMO_OBS_M7U2_A && gate <= SMO_OBS_M7U2_C) {
        m7u2_g_t *g = &g_m7u2[gate - SMO_OBS_M7U2_A];
        if (atomic_load_explicit(&g->k, memory_order_relaxed) < 16 ||
            atomic_load_explicit(&g_m7u2_on, memory_order_relaxed)) {
            uint64_t t = smo_obs_tick();
            uint32_t thr = runtime_cur_handle_tls;
            uint64_t xv1 = x3, xv8 = x2;
            atomic_fetch_add_explicit(&g->k, 1, memory_order_relaxed);
            atomic_store_explicit(&g->l_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g->l_lr, x4, memory_order_relaxed);
            atomic_store_explicit(&g->l_x0, x0, memory_order_relaxed);
            atomic_store_explicit(&g->l_x1, xv1, memory_order_relaxed);
            if (!atomic_load_explicit(&g->fset, memory_order_relaxed)) {
                g->f_tick = t; g->f_lr = x4; g->f_x0 = x0; g->f_x1 = xv1; g->f_x8 = xv8;
                g->f_thr = thr;
                atomic_store_explicit(&g->fset, 1, memory_order_relaxed);
            }
            int lslot = -1;                /* per-LR（= per-call-site）16 槽环 */
            for (int i = 0; i < M7U2_LR && lslot < 0; i++) if (g->lr[i].lr == x4) lslot = i;
            if (lslot < 0)
                for (int i = 0; i < M7U2_LR && lslot < 0; i++) if (!g->lr[i].lr) { g->lr[i].lr = x4; lslot = i; }
            if (lslot >= 0) {
                if (!atomic_load_explicit(&g->lr[lslot].n, memory_order_relaxed))
                    atomic_store_explicit(&g->lr[lslot].f_tick, t, memory_order_relaxed);
                atomic_fetch_add_explicit(&g->lr[lslot].n, 1, memory_order_relaxed);
                atomic_store_explicit(&g->lr[lslot].l_tick, t, memory_order_relaxed);
            } else atomic_fetch_add_explicit(&g->lr_ovf, 1, memory_order_relaxed);
            if (gate != SMO_OBS_M7U2_B) m7u2_capture(gate, x0, x4);   /* A 入口兜底 + C 取用点 */
            /* M7V 名环 v2（**不加门**）：挂既有 A/C 的同一抓取点，只留原始四槽 hex、不改写 */
            if (gate != SMO_OBS_M7U2_B && atomic_load_explicit(&g_m7v_on, memory_order_relaxed))
                m7v_capture_name(gate, x0, x4);
        }
        return;
    }
    /* M7V（2026-11-08）：**点名"真机派发到谁"**（门号 86..93，index 0=X8，1..7=R1..R7）。
       X8 门（0x7acd8c `ldr x8,[x8]`）：x2（门点原 x8）= vtable 指针 ⇒ 再解一层就是 0x7acd90
         `blr x8` 真要跳的函数地址；进 8 槽专用环（时刻/LR/scene/vtbl/**tgt**）+ 常规台账。
       R1..R7 门（Rsv 兄弟入口，tail=0 四字窗）：只记 count + 首末见 + per-LR 16 槽。
       "谁非零"直接指认族外实例。每次读到的东西都过 m7v_rd（域护栏 + SEGV 兜底）。 */
    if (gate >= SMO_OBS_M7V_X8 && gate <= SMO_OBS_M7V_R7) {
        m7v_g_t *g = &g_m7v[gate - SMO_OBS_M7V_X8];
        if (atomic_load_explicit(&g->k, memory_order_relaxed) < 16 ||
            atomic_load_explicit(&g_m7v_on, memory_order_relaxed)) {
            uint64_t t = smo_obs_tick();
            uint32_t thr = runtime_cur_handle_tls;
            uint64_t xv1 = x3, xv8 = x2;
            atomic_fetch_add_explicit(&g->k, 1, memory_order_relaxed);
            atomic_store_explicit(&g->l_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g->l_lr, x4, memory_order_relaxed);
            atomic_store_explicit(&g->l_x0, x0, memory_order_relaxed);
            atomic_store_explicit(&g->l_x1, xv1, memory_order_relaxed);
            if (!atomic_load_explicit(&g->fset, memory_order_relaxed)) {
                g->f_tick = t; g->f_lr = x4; g->f_x0 = x0; g->f_x1 = xv1; g->f_x8 = xv8;
                g->f_thr = thr;
                atomic_store_explicit(&g->fset, 1, memory_order_relaxed);
            }
            int lslot = -1;
            for (int i = 0; i < M7V_LR && lslot < 0; i++) if (g->lr[i].lr == x4) lslot = i;
            if (lslot < 0)
                for (int i = 0; i < M7V_LR && lslot < 0; i++) if (!g->lr[i].lr) { g->lr[i].lr = x4; lslot = i; }
            if (lslot >= 0) {
                if (!atomic_load_explicit(&g->lr[lslot].n, memory_order_relaxed))
                    atomic_store_explicit(&g->lr[lslot].f_tick, t, memory_order_relaxed);
                atomic_fetch_add_explicit(&g->lr[lslot].n, 1, memory_order_relaxed);
                atomic_store_explicit(&g->lr[lslot].l_tick, t, memory_order_relaxed);
            } else atomic_fetch_add_explicit(&g->lr_ovf, 1, memory_order_relaxed);
            if (gate == SMO_OBS_M7V_X8) {   /* 解一层 = 真派发目标（本轮的核心读数） */
                uint64_t ds = atomic_fetch_add_explicit(&g_m7v_d.n, 1, memory_order_relaxed);
                uint64_t tgt = 0, vt = xv8;
                int ok = m7v_rd(&tgt, vt, 8);
                if (ds < M7V_D_SLOTS) {
                    m7v_d_rec_t *r = &g_m7v_d.s[ds];
                    r->tick = t; r->lr = x4; r->scene = x0; r->vtbl = vt;
                    r->tgt = ok ? tgt : 0; r->ok = (uint32_t)ok;
                    r->gate = (uint32_t)gate; r->thr = thr;
                }
            }
        }
        return;
    }
    /* M7W2（2026-11-08）：**通用 mfp 适配器 main+0x7724d0**（门号 94）。
       门点 x0 = 描述符对象（= M7V 的 scene），x4 = LR（恒 main+0x7acd94，因为适配器尾跳 br x2）。
       观察器在这里**复算** `q = [x0+8] + (tag>>1)` 与
       `tgt = (tag&1) ? *(*(q)+[x0+0x10]) : [x0+0x10]`，把 obj/tag/p/o/q/vt/tgt 全部落环 ⇒
       "真机派发到谁"从「族外」变成可 describe 的具体地址。 */
    if (gate == SMO_OBS_M7W2_AD) {
        if (atomic_load_explicit(&g_m7w2_g.k, memory_order_relaxed) < 16 ||
            atomic_load_explicit(&g_m7w2_on, memory_order_relaxed))
            m7w2_capture(x4, x0, runtime_cur_handle_tls);
        return;
    }
    /* M7X2（2026-11-08）：**最后一跳**两门。
       G1 门号 95 main+0x74c9e0（slot-3 转调 thunk）：门点 x0 = q（真 this），x3 = 门点原 x1 = arg，
          x4 = LR（应当仍是 main+0x7acd94 —— 适配器是 `br` 尾跳，不写 x30）。
          观察器解两层 [q] / [[q]+0x18] + 字面字段 [q+0x18] ⇒ 真正干活的人可 describe。
       G2 门号 96 main+0x772500（描述符克隆）：门点 x0 = 源描述符（0x77250c `mov x19,x0`），
          读其 {+8,+0x10,+0x18,+0x20} 32 B + [src] + LR ⇒ "谁把哪个对象绑进 +8"。
       两门都只读寄存器/定长读，零 I/O。 */
    if (gate == SMO_OBS_M7X2_G1) {
        if (atomic_load_explicit(&g_m7x2_g1t.k, memory_order_relaxed) < 16 ||
            atomic_load_explicit(&g_m7x2_on, memory_order_relaxed))
            m7x2_g1_capture(x4, x0, x3, runtime_cur_handle_tls);
        return;
    }
    if (gate == SMO_OBS_M7X2_G2) {
        if (atomic_load_explicit(&g_m7x2_g2t.k, memory_order_relaxed) < 16 ||
            atomic_load_explicit(&g_m7x2_on, memory_order_relaxed))
            m7x2_g2_capture(x4, x0, runtime_cur_handle_tls);
        return;
    }
    /* M7Y2（2026-11-08）：**真机那个族外终点**两门。
       G1 门号 97 main+0x44adb0：门点 x0 = q（真 this，即被绑进 +8 的那个对象），x3 = 门点原 x1 = arg，
          x4 = LR。观察器读 [q] / [[q]+0x18] / **[q+0xb8] 及其指向的类名字符串** ⇒ 这一行日志
          直接自报"这次派发的是哪个类的对象"（离线从 161,696 条重定位行解出的三族同构布局）。
       G2 门号 98 main+0x44af80：同一个类的 slot +0x20，x0 = q，读 vt + 类名 ⇒ 该类其余生命周期方法是否被驱动。
       两门都只读寄存器 + ≤5 次定长读（m7v_rd 域护栏 + SIGSEGV 兜底），零 I/O。 */
    if (gate == SMO_OBS_M7Y2_G1) {
        if (atomic_load_explicit(&g_m7y2_g1t.k, memory_order_relaxed) < 16 ||
            atomic_load_explicit(&g_m7y2_on, memory_order_relaxed))
            m7y2_g1_capture(x4, x0, x3, runtime_cur_handle_tls);
        return;
    }
    if (gate == SMO_OBS_M7Y2_G2) {
        if (atomic_load_explicit(&g_m7y2_g2t.k, memory_order_relaxed) < 16 ||
            atomic_load_explicit(&g_m7y2_on, memory_order_relaxed))
            m7y2_g2_capture(x4, x0, x3, runtime_cur_handle_tls);
        return;
    }
    /* M7Z2（2026-11-08）：**选择点分叉** 五门（99..103）。零解引用（G5 有 ≤5 次 m7v_rd 定长读）。
       G1 99  main+0x6c7d30 谓词入口：x0=this、x3=门点原 x1=0x21c、x4=LR（本站点=0x4b8e94）。
       G2 100 main+0x4b8e98 跳过路径：x0=查询返回（1）、x2=门点原 x8=[this+0x58] 字节、x4=来路 LR。
       G3 101 main+0x4b8ea4 做活路径：x0=查询返回（0）/上一次谓词结果、x2=标志字节、x4=来路 LR。
       G4 102 main+0x4b8e88 cbnz 不成立处：x2=门点原 x8=[this+0x58] 原始字节。
       G5 103 main+0x4b8e20 NrvLoadFirstSequence::execute 入口：x0=宿主 this ⇒ 读
             [this] / [[this]] / [this+0x58] / [this+0x20] / [this+0x60]（门4 补读）。 */
    if (gate >= SMO_OBS_M7Z2_G1 && gate <= SMO_OBS_M7Z2_G5) {
        int gi = (int)(gate - SMO_OBS_M7Z2_G1);
        if (atomic_load_explicit(&g_m7z2_on, memory_order_relaxed) ||
            atomic_load_explicit(&g_m7z2[gi].k, memory_order_relaxed) < 16) {
            uint64_t a = 0, b = 0, c = 0, d = 0, e = 0;
            if (gi == 4 && m7v_ptr_ok(x0)) {                 /* G5：宿主对象本身，≤5 次护栏读 */
                uint64_t vt = 0, s0 = 0, w58 = 0, w20 = 0, w60 = 0;
                if (m7v_rd(&vt, x0, 8)) a = vt;                       /* [this] = vtable */
                if (m7v_ptr_ok(vt) && m7v_rd(&s0, vt, 8)) b = s0;     /* [[this]] = slot0 目标 */
                if (m7v_rd(&w58, x0 + 0x58, 8)) c = w58;              /* [this+0x58] 原始 8 B（低字节=标志） */
                if (m7v_rd(&w20, x0 + 0x20, 8)) d = w20;              /* [this+0x20] = 场景对象 */
                if (m7v_rd(&w60, x0 + 0x60, 8)) e = w60;              /* [this+0x58+8] 备用 */
            }
            m7z2_cap(gi, x0, x3, x2, x4, a, b, c, d, e, runtime_cur_handle_tls);
        }
        return;
    }
    /* M8B（2026-11-08）：**序列对象 nerve 状态机** 两门（104/105）。
       G1 104 main+0x6c7750 NerveKeeper::setNerve：x0=keeper、x3=门点原 x1=**新 nerve token**、
             x4=LR（尾跳时 = 0x6c7b20/0x6c8d80 的调用者）。观察器读 [keeper+0x10]（旧 token）、
             [keeper+0x18]（旧步数）与 [token+0x10]（名字格 → 名字串前 63 B）。
       G2 105 main+0x4b8d30 HostTypeNrvBoot::execute 入口：x0=宿主 this、x4=LR（nerve 派发循环里的返回址）。
       两门都零行为改动（只读寄存器 + ≤3 次 m7v_rd 定长读，无 I/O）。 */
    if (gate == SMO_OBS_M8B_G1 || gate == SMO_OBS_M8B_G2) {
        int gi = (gate == SMO_OBS_M8B_G1) ? 0 : 1;
        if (atomic_load_explicit(&g_m8b_on, memory_order_relaxed) ||
            atomic_load_explicit(&g_m8b[gi].k, memory_order_relaxed) < 16)
            m8b_cap(gi, x0, x3, x4);
        return;
    }
    /* M8D（2026-11-08）：**两只孪生绑表点**（106/107）。零解引用、零行为改动：只记
       计数 / 首末 tick / LR 台账（复用 m8b_cap 的 gi=2/3 两格；gi!=0 时它不读名字、不建 token 表）。
       G1 106 = main+0x4b8cb0（绑表 0x12d0fe8 = FirstSequence 族），G2 107 = main+0x4b7380（绑表
       0x12d06d8 = TitleMenu 族）。两门一响 = 该族 HostType 实例在本机被构造过（vtable 唯一入口）。 */
    if (gate == SMO_OBS_M8D_G1 || gate == SMO_OBS_M8D_G2) {
        int gi = (gate == SMO_OBS_M8D_G1) ? 2 : 3;
        if (atomic_load_explicit(&g_m8d_on, memory_order_relaxed) ||
            atomic_load_explicit(&g_m8b[gi].k, memory_order_relaxed) < 16)
            m8b_cap(gi, x0, x3, x4);
        return;
    }
    /* M10A（2026-11-08）：`smo_m10a`。两件事，都不碰既有门的行为：
       ① 门 108：给 35 字变体 stub 定本次调用的 mask（观0=不改原样返回 / 1=orr 上 bit0）。观察器在
          重放那条 bl **之前**跑，所以只能"预置掩码"，真正的原始返回值由 stub 在 bl 之后自己存。
       ② 门 112（0x4b4bc8 = tbz 落穿侧首字）：只计数 + 记 x0（= 调用方看到的返回值）+ LR。
       开关 = 0 时整段跳过（字节级无副作用）。 */
    if (gate == SMO_OBS_M8E2_G1) {
        int mode = atomic_load_explicit(&g_m10a_mode, memory_order_relaxed);
        if (mode) {
            int n = (int)atomic_fetch_add_explicit(&g_m10a_st.hits, 1, memory_order_relaxed);
            int lim = atomic_load_explicit(&g_m10a_limit, memory_order_relaxed);
            int force = (mode == 1 && n < lim) ? 1 : 0;
            if (n < M10A_RING) {   /* 把上一次调用的 raw/ret 收进环（本次的由 sum 行兜底） */
                g_m10a_r[n].tick = smo_obs_tick();
                g_m10a_r[n].forced = force;
                g_m10a_r[n].raw = atomic_load_explicit(&g_m10a_st.raw, memory_order_relaxed);
                g_m10a_r[n].ret = atomic_load_explicit(&g_m10a_st.ret, memory_order_relaxed);
                atomic_store_explicit(&g_m10a_rn, (uint64_t)n + 1, memory_order_relaxed);
            }
            atomic_store_explicit(&g_m10a_st.mask, force, memory_order_relaxed);
            if (force) atomic_fetch_add_explicit(&g_m10a_st.forced, 1, memory_order_relaxed);
            uint64_t t = smo_obs_tick();
            if (!atomic_load_explicit(&g_m10a_st.f_tick, memory_order_relaxed))
                atomic_store_explicit(&g_m10a_st.f_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g_m10a_st.l_tick, t, memory_order_relaxed);
        }
        /* 不 return：继续走下面的 M8E2 既有记录（判据字节照旧，一字不改） */
    }
    if (gate == SMO_OBS_M10A_A) {
        uint64_t t = smo_obs_tick();
        uint64_t k = atomic_fetch_add_explicit(&g_m10a_a.k, 1, memory_order_relaxed);
        if (!k) {
            atomic_store_explicit(&g_m10a_a.f_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g_m10a_a.f_x0, x0, memory_order_relaxed);
            atomic_store_explicit(&g_m10a_a.f_lr, x4, memory_order_relaxed);
        }
        atomic_store_explicit(&g_m10a_a.l_tick, t, memory_order_relaxed);
        atomic_store_explicit(&g_m10a_a.l_x0, x0, memory_order_relaxed);
        atomic_store_explicit(&g_m10a_a.l_lr, x4, memory_order_relaxed);
        return;
    }
    /* M8E2（2026-11-08）：**第一个分叉** 四门（108..111）。x0 = 门点原 x0、x3 = 门点原 x1、
       x4 = 门点 LR。零行为改动：G1 只在护栏内读 A/B 两跳取"判定字节"，G3 抄入参串前 39 B，
       其余只记寄存器。 */
    if (gate >= SMO_OBS_M8E2_G1 && gate <= SMO_OBS_M8E2_G4) {
        int gi = (int)gate - (int)SMO_OBS_M8E2_G1;
        if (atomic_load_explicit(&g_m8e2_on, memory_order_relaxed) ||
            atomic_load_explicit(&g_m8e2[gi].k, memory_order_relaxed) < 16)
            m8e2_cap(gi, x0, x3, x4);
        return;
    }
    /* M7Q（2026-11-08）：三层门 —— WRAP 0x964540（包装 ctor 入口）/ FO 0x65b468（扇出数入口）/
       FOB 0x65b470（bl 0x5b8710，造 3 条 lane 的调用）。x0 = 门点原 x0（WRAP: this；
       FO: 上一次 alloc 的块；FOB: 0x40 字节的 lane 池块）、x3 = 门点原 x1（**FOB 处 = w1 =
       扇出数**，WRAP 处 = 第 2 实参）、x2 = 门点原 x8、x4 = 门点 LR。只读寄存器、零解引用。 */
    if (gate >= SMO_OBS_M7Q_WRAP && gate <= SMO_OBS_M7Q_FOB) {
        m7q_g_t *g = &g_m7q[gate - SMO_OBS_M7Q_WRAP];
        if (atomic_load_explicit(&g->k, memory_order_relaxed) < 16 ||
            atomic_load_explicit(&g_m7q_on, memory_order_relaxed)) {
            uint64_t t = smo_obs_tick();
            uint32_t thr = runtime_cur_handle_tls;
            uint64_t xv1 = x3, xv8 = x2;
            atomic_fetch_add_explicit(&g->k, 1, memory_order_relaxed);
            atomic_store_explicit(&g->l_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g->l_lr, x4, memory_order_relaxed);
            atomic_store_explicit(&g->l_x0, x0, memory_order_relaxed);
            atomic_store_explicit(&g->l_x1, xv1, memory_order_relaxed);
            if (!atomic_load_explicit(&g->fset, memory_order_relaxed)) {
                g->f_tick = t; g->f_lr = x4; g->f_x0 = x0; g->f_x1 = xv1; g->f_x8 = xv8;
                g->f_thr = thr;
                atomic_store_explicit(&g->fset, 1, memory_order_relaxed);
            }
            uint64_t kk = atomic_fetch_add_explicit(&g->rk, 1, memory_order_relaxed);
            if (kk < M7Q_ROW) {            /* 首 64 条：认领即写，不覆盖 */
                g->r[kk].tick = t; g->r[kk].lr = x4; g->r[kk].x0 = x0;
                g->r[kk].x1 = xv1; g->r[kk].x8 = xv8; g->r[kk].thr = thr;
            } else atomic_fetch_add_explicit(&g->r_ovf, 1, memory_order_relaxed);
            int lslot = -1;                /* per-LR（= per-bl-site）计数表 */
            for (int i = 0; i < M7Q_LR && lslot < 0; i++) if (g->lr[i].lr == x4) lslot = i;
            if (lslot < 0)
                for (int i = 0; i < M7Q_LR && lslot < 0; i++) if (!g->lr[i].lr) { g->lr[i].lr = x4; lslot = i; }
            if (lslot >= 0) {
                if (!atomic_load_explicit(&g->lr[lslot].n, memory_order_relaxed))
                    atomic_store_explicit(&g->lr[lslot].f_tick, t, memory_order_relaxed);
                atomic_fetch_add_explicit(&g->lr[lslot].n, 1, memory_order_relaxed);
                atomic_store_explicit(&g->lr[lslot].l_tick, t, memory_order_relaxed);
            } else atomic_fetch_add_explicit(&g->lr_ovf, 1, memory_order_relaxed);
            int vslot = -1;                /* per-value 表：FO/FOB 的 x1 = 扇出数 w1 */
            for (int i = 0; i < M7Q_VAL && vslot < 0; i++) if (g->v[i].v == xv1) vslot = i;
            if (vslot < 0)
                for (int i = 0; i < M7Q_VAL && vslot < 0; i++) if (!g->v[i].v) { g->v[i].v = xv1; vslot = i; }
            if (vslot >= 0) {
                if (!atomic_load_explicit(&g->v[vslot].n, memory_order_relaxed))
                    atomic_store_explicit(&g->v[vslot].f_tick, t, memory_order_relaxed);
                atomic_fetch_add_explicit(&g->v[vslot].n, 1, memory_order_relaxed);
                atomic_store_explicit(&g->v[vslot].l_tick, t, memory_order_relaxed);
            } else atomic_fetch_add_explicit(&g->v_ovf, 1, memory_order_relaxed);
        }
        return;
    }
    if (gate == SMO_OBS_M7P_START) {
        m7p_g_t *g = &g_m7p[0];
        if (atomic_load_explicit(&g->k, memory_order_relaxed) < 16 ||
            atomic_load_explicit(&g_m7p_on, memory_order_relaxed)) {
            uint64_t t = smo_obs_tick();
            uint32_t thr = runtime_cur_handle_tls;
            uint64_t ep = 0, tok = 0; uint32_t state = 0;
            if (x0 > 0x100000000ULL && x0 < 0x800000000000ULL) {
                state = *(const volatile uint32_t *)(const void *)(uintptr_t)(x0 + 0xd4);
                tok   = *(const volatile uint64_t *)(const void *)(uintptr_t)(x0 + 0xc8);
            }
            if (x3 > 0x100000000ULL && x3 < 0x800000000000ULL)
                ep = *(const volatile uint32_t *)(const void *)(uintptr_t)x3;
            atomic_fetch_add_explicit(&g->k, 1, memory_order_relaxed);
            atomic_store_explicit(&g->l_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g->l_x0, x0, memory_order_relaxed);
            atomic_store_explicit(&g->l_lr, x4, memory_order_relaxed);
            atomic_store_explicit(&g->l_thr, thr, memory_order_relaxed);
            if (!atomic_load_explicit(&g->fset, memory_order_relaxed)) {
                g->f_tick = t; g->f_x0 = x0; g->f_lr = x4; g->f_ep = ep;
                g->f_thr = thr; g->f_state = state;
                atomic_store_explicit(&g->fset, 1, memory_order_relaxed);
            }
            uint64_t kk = atomic_fetch_add_explicit(&g->rk, 1, memory_order_relaxed);
            if (kk < M7P_ROW) {           /* 首 64 条：认领即写，不覆盖 */
                g->r[kk].tick = t; g->r[kk].lr = x4; g->r[kk].x0 = x0; g->r[kk].ep = ep;
                g->r[kk].tok = tok; g->r[kk].thr = thr; g->r[kk].state = state;
            } else atomic_fetch_add_explicit(&g->r_ovf, 1, memory_order_relaxed);
            int pslot = -1;
            for (int i = 0; i < 8 && pslot < 0; i++) if (g->pt[i].thr == thr) pslot = i;
            if (pslot < 0)
                for (int i = 0; i < 8 && pslot < 0; i++) if (!g->pt[i].thr) { g->pt[i].thr = thr; pslot = i; }
            if (pslot >= 0) atomic_fetch_add_explicit(&g->pt[pslot].n, 1, memory_order_relaxed);
            else atomic_fetch_add_explicit(&g->pt_ovf, 1, memory_order_relaxed);
            int lslot = -1;   /* per-LR（= per-bl-site）计数表 */
            for (int i = 0; i < M7P_LR && lslot < 0; i++) if (g->lr[i].lr == x4) lslot = i;
            if (lslot < 0)
                for (int i = 0; i < M7P_LR && lslot < 0; i++) if (!g->lr[i].lr) { g->lr[i].lr = x4; lslot = i; }
            if (lslot >= 0) {
                if (!atomic_load_explicit(&g->lr[lslot].n, memory_order_relaxed))
                    atomic_store_explicit(&g->lr[lslot].f_tick, t, memory_order_relaxed);
                atomic_fetch_add_explicit(&g->lr[lslot].n, 1, memory_order_relaxed);
                atomic_store_explicit(&g->lr[lslot].l_tick, t, memory_order_relaxed);
            } else atomic_fetch_add_explicit(&g->lr_ovf, 1, memory_order_relaxed);
            int oslot = -1;   /* per-object 台账：这个 worker 对象是哪一站建出/启动的 */
            for (int i = 0; i < M7P_OBJ && oslot < 0; i++) if (g->ob[i].w == x0) oslot = i;
            if (oslot < 0)
                for (int i = 0; i < M7P_OBJ && oslot < 0; i++) if (!g->ob[i].w) { g->ob[i].w = x0; oslot = i; }
            if (oslot >= 0) {
                if (!atomic_load_explicit(&g->ob[oslot].n, memory_order_relaxed)) {
                    atomic_store_explicit(&g->ob[oslot].f_tick, t, memory_order_relaxed);
                    atomic_store_explicit(&g->ob[oslot].f_lr, x4, memory_order_relaxed);
                    atomic_store_explicit(&g->ob[oslot].f_ep, ep, memory_order_relaxed);
                }
                atomic_fetch_add_explicit(&g->ob[oslot].n, 1, memory_order_relaxed);
            } else atomic_fetch_add_explicit(&g->ob_ovf, 1, memory_order_relaxed);
        }
        return;
    }
    /* M7N：谁 post 了谁（G1 工厂 / G2 PostToken）。两只门都只读寄存器 + 护栏内 1..2 次
       定长读：G1 读 ctx->0x88（worker 编号），G2 读 obj->0xd4（state）与 obj->0xc8
       （期望 token）。首 64 条行不改写（认领式，保"第一次"），另按对象/按 LR/按线程
       三张锁存表给分布。 */
    if (gate == SMO_OBS_M7N_G1 || gate == SMO_OBS_M7N_G2) {
        int gi = (gate == SMO_OBS_M7N_G2) ? 1 : 0;
        /* 前 256 次无条件捕获（bootstrap 派活 t≈2.2 s 可能早于开关文件第一次被 poll 读到），
           之后严格按开关。见 threads.c 的 runtime_m7n_poll 注释。 */
        if (atomic_load_explicit(&g_m7n[gi].k, memory_order_relaxed) < 256 ||
            atomic_load_explicit(&g_m7n_on, memory_order_relaxed)) {
            m7n_g_t *g = &g_m7n[gi];
            uint64_t t = smo_obs_tick();
            uint32_t thr = runtime_cur_handle_tls;
            uint64_t aux = 0, tok = 0; uint32_t state = 0;
            if (x0 > 0x100000000ULL && x0 < 0x800000000000ULL) {
                if (!gi) aux = *(const volatile uint32_t *)(const void *)(uintptr_t)(x0 + 0x88);
                else {
                    state = *(const volatile uint32_t *)(const void *)(uintptr_t)(x0 + 0xd4);
                    tok   = *(const volatile uint64_t *)(const void *)(uintptr_t)(x0 + 0xc8);
                }
            }
            atomic_fetch_add_explicit(&g->k, 1, memory_order_relaxed);
            atomic_store_explicit(&g->l_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g->l_x0, x0, memory_order_relaxed);
            atomic_store_explicit(&g->l_lr, x4, memory_order_relaxed);
            atomic_store_explicit(&g->l_thr, thr, memory_order_relaxed);
            if (!atomic_load_explicit(&g->fset, memory_order_relaxed)) {
                g->f_tick = t; g->f_x0 = x0; g->f_x1 = x3; g->f_aux = aux; g->f_tok = tok;
                g->f_state = state; g->f_thr = thr; g->f_lr = x4;
                atomic_store_explicit(&g->fset, 1, memory_order_relaxed);
            }
            uint64_t kk = atomic_fetch_add_explicit(&g->rk, 1, memory_order_relaxed);
            if (kk < M7N_ROW) {           /* 首 64 条：认领即写，不覆盖 */
                g->r[kk].tick = t; g->r[kk].x0 = x0; g->r[kk].x1 = x3; g->r[kk].tok = tok;
                g->r[kk].lr = x4; g->r[kk].thr = thr; g->r[kk].state = state;
                g->r[kk].aux = (uint32_t)aux;
            } else atomic_fetch_add_explicit(&g->r_ovf, 1, memory_order_relaxed);
            int pslot = -1;
            for (int i = 0; i < 8 && pslot < 0; i++) if (g->pt[i].thr == thr) pslot = i;
            if (pslot < 0)
                for (int i = 0; i < 8 && pslot < 0; i++) if (!g->pt[i].thr) { g->pt[i].thr = thr; pslot = i; }
            if (pslot >= 0) atomic_fetch_add_explicit(&g->pt[pslot].n, 1, memory_order_relaxed);
            else atomic_fetch_add_explicit(&g->pt_ovf, 1, memory_order_relaxed);
            int lslot = -1;
            for (int i = 0; i < M7N_LR && lslot < 0; i++) if (g->lr[i].lr == x4) lslot = i;
            if (lslot < 0)
                for (int i = 0; i < M7N_LR && lslot < 0; i++) if (!g->lr[i].lr) { g->lr[i].lr = x4; lslot = i; }
            if (lslot >= 0) {
                atomic_fetch_add_explicit(&g->lr[lslot].n, 1, memory_order_relaxed);
                atomic_store_explicit(&g->lr[lslot].l_tick, t, memory_order_relaxed);
            } else atomic_fetch_add_explicit(&g->lr_ovf, 1, memory_order_relaxed);
            int wslot = -1;
            for (int i = 0; i < M7N_WP && wslot < 0; i++) if (g->wp[i].w == x0) wslot = i;
            if (wslot < 0)
                for (int i = 0; i < M7N_WP && wslot < 0; i++) if (!g->wp[i].w) { g->wp[i].w = x0; wslot = i; }
            if (wslot >= 0) {
                if (!atomic_load_explicit(&g->wp[wslot].n, memory_order_relaxed)) {
                    atomic_store_explicit(&g->wp[wslot].f_tick, t, memory_order_relaxed);
                    atomic_store_explicit(&g->wp[wslot].f_lr, x4, memory_order_relaxed);
                }
                atomic_fetch_add_explicit(&g->wp[wslot].n, 1, memory_order_relaxed);
            } else atomic_fetch_add_explicit(&g->wp_ovf, 1, memory_order_relaxed);
        }
        return;
    }
    /* M7e: create-request 0x746020 entry. x0 = output object, x4 = LR (creator return addr),
       thread = TLS handle. Fixed-size writes + atomics only; no dereference at all (the object
       is freshly allocated and still zeroed here, so reading it would be useless anyway). */
    if (gate == SMO_OBS_M7E_CRT) {
        if (atomic_load_explicit(&g_m7e_on, memory_order_relaxed)) {
            uint64_t t = smo_obs_tick();
            uint32_t thr = runtime_cur_handle_tls;
            atomic_fetch_add_explicit(&g_m7e_crt.k, 1, memory_order_relaxed);
            atomic_store_explicit(&g_m7e_crt.l_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g_m7e_crt.l_x0, x0, memory_order_relaxed);
            atomic_store_explicit(&g_m7e_crt.l_lr, x4, memory_order_relaxed);
            atomic_store_explicit(&g_m7e_crt.l_thr, thr, memory_order_relaxed);
            if (!atomic_load_explicit(&g_m7e_crt.fset, memory_order_relaxed)) {
                g_m7e_crt.f_tick = t; g_m7e_crt.f_x0 = x0; g_m7e_crt.f_lr = x4; g_m7e_crt.f_thr = thr;
                atomic_store_explicit(&g_m7e_crt.fset, 1, memory_order_relaxed);
            }
            int pslot = -1;
            for (int i = 0; i < 8 && pslot < 0; i++)
                if (g_m7e_crt.pt[i].thr == thr) pslot = i;
            if (pslot < 0)
                for (int i = 0; i < 8 && pslot < 0; i++)
                    if (!g_m7e_crt.pt[i].thr) { g_m7e_crt.pt[i].thr = thr; pslot = i; }
            if (pslot >= 0) atomic_fetch_add_explicit(&g_m7e_crt.pt[pslot].n, 1, memory_order_relaxed);
            else atomic_fetch_add_explicit(&g_m7e_crt.pt_ovf, 1, memory_order_relaxed);
            uint64_t kk = atomic_fetch_add_explicit(&g_m7e_crt.rk, 1, memory_order_relaxed);
            if (kk >= 32) atomic_store_explicit(&g_m7e_crt.r_ovf, kk - 31, memory_order_relaxed);
            unsigned slot = (unsigned)(kk & 31u);
            g_m7e_crt.r[slot].tick = t; g_m7e_crt.r[slot].thr = thr;
            g_m7e_crt.r[slot].x0 = x0; g_m7e_crt.r[slot].lr = x4;
            /* distinct-LR latch (creator identity): 8 slots, racy insert is benign */
            int lslot = -1;
            for (int i = 0; i < 8 && lslot < 0; i++)
                if (g_m7e_crt.lr[i].lr == x4) lslot = i;
            if (lslot < 0)
                for (int i = 0; i < 8 && lslot < 0; i++)
                    if (!g_m7e_crt.lr[i].lr) { g_m7e_crt.lr[i].lr = x4; lslot = i; }
            if (lslot >= 0) atomic_fetch_add_explicit(&g_m7e_crt.lr[lslot].n, 1, memory_order_relaxed);
            else atomic_fetch_add_explicit(&g_m7e_crt.lr_ovf, 1, memory_order_relaxed);
        }
        return;
    }
    /* M7c: fnA 0x6156b0 entry (task P1.2). x3 = original x1 (name arg), x4 = LR, thread from
       TLS. Per-thread linear scan/insert is racy across pump threads (benign: a lost insert
       only undercounts; the same handle wins the slot next hit). All fixed-size writes. */
    if (gate == SMO_OBS_M7C_FNA) {
        if (atomic_load_explicit(&g_m7c_on, memory_order_relaxed)) {
            uint64_t t = smo_obs_tick();
            uint32_t thr = runtime_cur_handle_tls;
            atomic_fetch_add_explicit(&g_m7c_fna.k, 1, memory_order_relaxed);
            atomic_store_explicit(&g_m7c_fna.l_tick, t, memory_order_relaxed);
            atomic_store_explicit(&g_m7c_fna.l_thr, thr, memory_order_relaxed);
            atomic_store_explicit(&g_m7c_fna.l_x1, x3, memory_order_relaxed);
            atomic_store_explicit(&g_m7c_fna.l_lr, x4, memory_order_relaxed);
            if (!atomic_load_explicit(&g_m7c_fna.fset, memory_order_relaxed)) {
                g_m7c_fna.f_tick = t;
                atomic_store_explicit(&g_m7c_fna.fset, 1, memory_order_relaxed);
            }
            int pslot = -1;
            for (int i = 0; i < 8 && pslot < 0; i++)
                if (g_m7c_fna.pt[i].thr == thr) pslot = i;
            if (pslot < 0)
                for (int i = 0; i < 8 && pslot < 0; i++)
                    if (!g_m7c_fna.pt[i].thr) { g_m7c_fna.pt[i].thr = thr; pslot = i; }
            if (pslot >= 0) atomic_fetch_add_explicit(&g_m7c_fna.pt[pslot].n, 1, memory_order_relaxed);
            else atomic_fetch_add_explicit(&g_m7c_fna.pt_ovf, 1, memory_order_relaxed);
            uint64_t kk = atomic_fetch_add_explicit(&g_m7c_fna.rk, 1, memory_order_relaxed);
            if (kk >= 16) atomic_store_explicit(&g_m7c_fna.r_ovf, kk - 15, memory_order_relaxed);
            unsigned slot = (unsigned)(kk & 15u);
            g_m7c_fna.r[slot].tick = t; g_m7c_fna.r[slot].thr = thr;
            g_m7c_fna.r[slot].x1 = x3; g_m7c_fna.r[slot].lr = x4;
        }
        return;
    }
    /* round21 首见三门：REG230 {x0=槽, x1=被注册句柄}；FN_A {x0=时基参, x1=版本容器}；
       FN_B {x0=容器本体, x1=第二实参}。观察器参4=门点原 x1。首见一次定格（幂等竞争
       良性覆盖，观测用途）；纯原子+定长写，红线不破。 */
    if (gate == SMO_OBS_REG230 || gate == SMO_OBS_FN_A || gate == SMO_OBS_FN_B) {
        int k = gate == SMO_OBS_REG230 ? 0 : gate == SMO_OBS_FN_A ? 1 : 2;
        if (!atomic_load_explicit(&g_fs[k].set, memory_order_relaxed)) {
            g_fs[k].x0 = x0; g_fs[k].x1 = x3; g_fs[k].tick = smo_obs_tick();
            atomic_store_explicit(&g_fs[k].set, 1, memory_order_relaxed);
        }
        return;
    }
    /* round24 TP 解引用首字：x0=入口原参（stub 参1，prologue 原样传）。TP4A 还原
       0x9646a8 add x0,x0,#0x38（函数内唯一 x0 写点）后取入口对象首 8 字节。
       越护栏拒读 deref=0 只计 n_bad——dump 侧凭 n_bad 区分「真 0」与「拒读」。 */
    if (gate >= SMO_OBS_TP1 && gate <= SMO_OBS_TP4A) {
        int k = (int)gate - SMO_OBS_TP1;
        uint64_t a = gate == SMO_OBS_TP4A ? x0 - 0x38 : x0;   /* x0<0x38 下溢必越护栏，拒读，安全 */
        uint64_t d = 0;
        if (a > 0x100000000ULL && a < 0x800000000000ULL) {
            d = *(const volatile uint64_t *)(const void *)(uintptr_t)a;
            atomic_fetch_add_explicit(&g_tp[k].n_ok, 1, memory_order_relaxed);
        } else atomic_fetch_add_explicit(&g_tp[k].n_bad, 1, memory_order_relaxed);
        atomic_store_explicit(&g_tp[k].last_deref, d, memory_order_relaxed);
        if (!atomic_load_explicit(&g_tp[k].set, memory_order_relaxed)) {
            g_tp[k].x0 = a; g_tp[k].deref = d; g_tp[k].tick = smo_obs_tick();
            atomic_store_explicit(&g_tp[k].set, 1, memory_order_relaxed);
        }
        /* round25 TP5：出口双门加记状态字序列（前 8 次 + 第 10000 次；越护栏只记
           x0 本身、state=val=0——dump 侧凭 x0==0 辨拒读）。纯原子+定长写。 */
        if (gate >= SMO_OBS_TP4R) {
            int q = (int)gate - SMO_OBS_TP4R;
            uint64_t st = 0, vl = 0;
            if (x0 > 0x100000000ULL && x0 < 0x800000000000ULL) {
                st = *(const volatile uint32_t *)(const void *)(uintptr_t)(x0 + 0xd4);
                vl = *(const volatile uint64_t *)(const void *)(uintptr_t)(x0 + 0xc8);
            }
            uint64_t kk = atomic_fetch_add_explicit(&g_tp5[q].k, 1, memory_order_relaxed);
            if (kk < 8) { g_tp5[q].r[kk].x0 = x0; g_tp5[q].r[kk].state = st; g_tp5[q].r[kk].val = vl; }
            else if (kk == 9999) { g_tp5[q].r[8].x0 = x0; g_tp5[q].r[8].state = st; g_tp5[q].r[8].val = vl; }
        }
        /* M7w ③：TP4A（0x9646ac 放行出口）挂账。a=上方已还原的入口 this（x0-0x38），
           x3=门点原 x1=msg（放行路径不改 x1）、x4=LR（仍是 sendTo 调用者返回址）。 */
        if (gate == SMO_OBS_TP4A && atomic_load_explicit(&g_m7w_on, memory_order_relaxed)) {
            uint64_t st = 0;
            if (a > 0x100000000ULL && a < 0x800000000000ULL)
                st = *(const volatile uint32_t *)(const void *)(uintptr_t)(a + 0xd4);
            atomic_fetch_add_explicit(&g_m7w_pass.n, 1, memory_order_relaxed);
            uint64_t kk = atomic_fetch_add_explicit(&g_m7w_pass.k, 1, memory_order_relaxed);
            if (kk < 20) {
                g_m7w_pass.r[kk].tick = smo_obs_tick(); g_m7w_pass.r[kk].this_ = a;
                g_m7w_pass.r[kk].msg = x3; g_m7w_pass.r[kk].state = st; g_m7w_pass.r[kk].lr = x4;
            }
        }
        return;
    }
#if SMO_OBS_HEAVY
    /* round17 ①：KICK1 门每次命中记 kick 三条件（this=x0、w1=x3）＋三分支交叉分布。
       round20：整段随 SMO_OBS_HEAVY=0 编译关闭（最小扰动构建）——KICK1 退化为
       纯计数门（上方 hits/last_x0），dump 侧打 off 标记。 */
    if (gate == SMO_OBS_KICK1) {
        uint64_t f9 = 0, o20 = 0;
        int rd = x0 > 0x100000000ULL && x0 < 0x800000000000ULL;
        if (rd) {
            f9 = *(const volatile uint8_t *)(const void *)(uintptr_t)(x0 + 0xf9);
            o20 = *(const volatile uint64_t *)(const void *)(uintptr_t)(x0 + 0x20);
        }
        if ((uint32_t)x3 < 1) atomic_fetch_add_explicit(&g_k1.n_lt1, 1, memory_order_relaxed);
        else if (f9 == 2)     atomic_fetch_add_explicit(&g_k1.n_ge1_f9eq2, 1, memory_order_relaxed);
        else                  atomic_fetch_add_explicit(&g_k1.n_ge1_f9ne2, 1, memory_order_relaxed);
        if (o20 == 0) atomic_fetch_add_explicit(&g_k1.n_o20z, 1, memory_order_relaxed);
        if ((uint32_t)x3 == 0) atomic_fetch_add_explicit(&g_k1.n_w1z, 1, memory_order_relaxed);
        else                   atomic_fetch_add_explicit(&g_k1.n_w1nz, 1, memory_order_relaxed);
        if (f9 == 2)  atomic_fetch_add_explicit(&g_k1.n_f9eq2, 1, memory_order_relaxed);
        else          atomic_fetch_add_explicit(&g_k1.n_f9ne2, 1, memory_order_relaxed);
        if (o20 == 0) atomic_fetch_add_explicit(&g_k1.n_o20z2, 1, memory_order_relaxed);
        else          atomic_fetch_add_explicit(&g_k1.n_o20nz, 1, memory_order_relaxed);
        uint64_t k = atomic_fetch_add_explicit(&g_k1.ring_k, 1, memory_order_relaxed);
        if (k < 16) {
            g_k1.ring[k].this_ = x0; g_k1.ring[k].w1 = x3;
            g_k1.ring[k].f9 = f9; g_k1.ring[k].o20 = o20;
        }
        if (!atomic_load_explicit(&g_k1.set, memory_order_relaxed)) {
            g_k1.this_ = x0; g_k1.w1 = x3; g_k1.f9 = f9; g_k1.o20 = o20; g_k1.tick = smo_obs_tick();
            atomic_store_explicit(&g_k1.set, 1, memory_order_relaxed);
        }
        atomic_store_explicit(&g_k1.last_w1, x3, memory_order_relaxed);
        atomic_store_explicit(&g_k1.last_f9, f9, memory_order_relaxed);
        atomic_store_explicit(&g_k1.last_o20, o20, memory_order_relaxed);
    }
#endif   /* SMO_OBS_HEAVY：KICK1 三条件快照（round17/18），round20 编译关闭 */
    /* round21 BC310 调用者 LR 捕获：first/last LR + 首 64 采样环（fetch_add 认领槽位，
       无 CAS）+ 末见 {x1,tick} 逐命中覆盖（第 45807 次现场=外层循环耗尽时的末见快照）。
       首见 {x0,x1,lr,tick}；x0==0 只计 xzero（LR 已记，不早退）。round19 ③ 的 first-16
       唯一 x0 环仍在 HEAVY 编译开关内（本轮 0=关）。 */
    if (gate == SMO_OBS_BC310) {
        atomic_store_explicit(&g_bc.last_lr, x4, memory_order_relaxed);
        atomic_store_explicit(&g_bc.last_x1, x3, memory_order_relaxed);
        atomic_store_explicit(&g_bc.last_tick, smo_obs_tick(), memory_order_relaxed);
        uint64_t lk = atomic_fetch_add_explicit(&g_bc.lr_k, 1, memory_order_relaxed);
        if (lk < 64) g_bc.lr[lk] = x4;
        if (!atomic_load_explicit(&g_bc.set, memory_order_relaxed)) {
            g_bc.x0 = x0; g_bc.x1 = x3; g_bc.lr0 = x4; g_bc.tick = smo_obs_tick();
            atomic_store_explicit(&g_bc.set, 1, memory_order_relaxed);
        }
        if (x0 == 0) { atomic_fetch_add_explicit(&g_bc.xzero, 1, memory_order_relaxed); return; }
#if SMO_OBS_HEAVY
        for (int i = 0; i < 16; i++) {
            uint64_t v = atomic_load_explicit(&g_bc.xv[i], memory_order_relaxed);
            if (v == x0) { atomic_fetch_add_explicit(&g_bc.xc[i], 1, memory_order_relaxed); return; }
            if (v == 0) {
                uint64_t exp = 0;
                if (atomic_compare_exchange_strong_explicit(&g_bc.xv[i], &exp, x0, memory_order_relaxed, memory_order_relaxed)) {
                    atomic_fetch_add_explicit(&g_bc.xc[i], 1, memory_order_relaxed); return;
                }
                if (exp == x0) { atomic_fetch_add_explicit(&g_bc.xc[i], 1, memory_order_relaxed); return; }
            }
        }
        atomic_fetch_add_explicit(&g_bc.xover, 1, memory_order_relaxed);
#endif
        return;
    }
}
/* ---- M8C（2026-11-08）：**条件注入真机 token**（本项目第一次"改状态"实验）----
   目标：在唯一 nerve 写点 main+0x6c7750 处，把**序列 HostType keeper** 的新 nerve token
   有条件地换成真机的 `HostTypeNrvLoadFirstSequence`（main+0x12d0e08），看模拟器能否被推进
   FirstSequenceScene / SelectMode，从而在模拟器上复现真机卡点。
   离线逐条确认（/tmp/m8c_work/a_keeper.py / b_disp.py，镜像 md5 89d568fd…，21,833,968 B）：
     * **0x6c7750**：x0=keeper、x1=新 token。0x6c7760 `mov x20,x1`、0x6c7788
       `str x20,[x19,#0x10]` ⇒ token 落在 **keeper+0x10**；要改行为**只需改 x1**，
       其它寄存器一个不动（本轮实现只写 x1）。
     * **keeper 布局**（从 0x6c7650 派发循环逐条读出，不是照抄符号名）：
       `+0x08` = **当前正在派发的 token**（0x6c76a0 `ldr x0,[keeper,#8]`，随后
       0x6c76a4/0x6c76a8 `ldr x8,[x0]; ldr x8,[x8]` ⇒ exec = [[token]]）；
       `+0x10` = **挂起的新 token**（setNerve 写这里；派发时 0x6c7688..0x6c7694 把 +0x10
       搬到 +0x08、并把 +0x10 清 0、+0x18 清 0）；`+0x18` = 步数（派发 0x6c76b4 每拍 +1，
       setNerve 置 -1）。
     * ⇒ **"这个 keeper 是不是序列那个 HostType"必须同时看 +0x08 与 +0x10**：M8B 时间线里
       只有第 1 次是 +0x10 命中（old10 = 0x12d06d8），第 2..5 次的 +0x10 已被消费成 0、
       真值在 +0x08 —— 这就是 M8B 的 old10 后面全为 0 的原因（也说明"照抄 [keeper+0x10]"
       会漏判后 4 次）。
     * **目标 token 0x12d0e08 由加载期 reloc 行独立定名**（不依赖 M8B 的结论）：desc base
       0x12d0df8，`+0x00` exec = 0x4b93d0（`ldr x0,[x1]; b 0x4b8e20`）、`+0x10` = 自指针
       （= 0x12d0df8，形态① 的 token = base+0x10）、`+0x20` 名字格 =
       `N12_GLOBAL__N_128HostTypeNrvLoadFirstSequenceE`。故 `[[base+0x12d0e08]]` = exec，
       与模拟器侧"槽 token"同构 ⇒ 注入后派发链不需要任何额外修补。
   判别集（**main 相对**，运行期按 base 现算，绝不写死绝对地址）：模拟器 TitleMenu 族走过
   的 5 个 token + 注入后自己会出现的 2 个：
     0x12d06d8 / 0x12d0678 / 0x12d06e8 / 0x12d06b8 / 0x12d06e0 / 0x12d0e08 / 0x12d0e48
   命中即**锁定 keeper 指针**（M8B P4.1 的第二种判据），此后只认这一个 keeper —— 否则会把
   全游戏 373 个对象的 nerve 一起改掉（派活单红线）。
   开关 smo_m8c（threads.c 每拍刷新；C 默认 0）：
     0 = 完全不动（对照 boot 必须与 M8B 基线一致 ⇒ "注入实现没污染只读路径"的证明）；
     1..3 = 从第 N 次 keeper 命中起注入（N = 开关值），每次注入**只改 x1**；
     每 boot 注入上限 M8C_MAX_INJ = 3（防连环跑飞）。
   ⚠️ 名字用 m8c：`smo_m8c`/`[m8c]`/`m8c_boot_run.sh`/`/tmp/m8c_report.md`/`logs/m8c_*`
   五类开工前逐条 grep/stat 全 0 命中（2026-11-08 14:3x）⇒ 直接用 m8c，不启用 m8c2。 */
#define M8C_TGT 8
#define M8C_MAX_INJ 3
#define M8C_REC 8
/* 序列 HostType（模拟器侧 TitleMenu 族）实走的 5 个 token + 注入目标 + 下一跳（main 相对） */
static const uint32_t m8c_tgt[M8C_TGT] = {
    0x12d06d8, 0x12d0678, 0x12d06e8, 0x12d06b8, 0x12d06e0,   /* 槽 / BootLayout / 槽 / Menu / 槽 */
    0x12d0e08, 0x12d0e48,                                     /* LoadFirstSequence / FirstSequence */
    0x12d0d48 };                                              /* f1 HostTypeNrvBoot（下一跳：0x4b8d30 的令牌） */
#define M8C_TGT_LOADFIRST 0x12d0e08u
#define M8C_TGT_F1BOOT    0x12d0d48u
/* 开关值 → {注入目标, 从第几次命中起注入, 本 boot 注入上限}
     val  1..3   → 目标 LoadFirstSequence(0x12d0e08)，从第 val 次命中起，上限 3
     val 11..13  → 目标 f1 HostTypeNrvBoot(0x12d0d48)，从第 val-10 次命中起，上限 3（下一跳实验）
     val 21..23  → 目标 LoadFirstSequence(0x12d0e08)，从第 val-20 次命中起，**上限 1**（"只注入 1 次"降级跑法） */
static void m8c_plan(int val, uint32_t *tgt_off, int *skip, int *cap) {
    if (val >= 21)      { *tgt_off = M8C_TGT_LOADFIRST; *skip = val - 20; *cap = 1; }
    else if (val >= 11) { *tgt_off = M8C_TGT_F1BOOT;    *skip = val - 10; *cap = M8C_MAX_INJ; }
    else                { *tgt_off = M8C_TGT_LOADFIRST; *skip = val;      *cap = M8C_MAX_INJ; }
    if (*skip < 1) *skip = 1;
}
typedef struct { uint64_t tick, keeper, cur8, pend10, old_x1, new_x1, lr; } m8c_rec_t;
_Atomic int g_m8c_on;                 /* threads.c 每拍刷新：开关值 0..3（0 = 关；1..3 = 从第 N 次命中起注入） */
static struct {
    _Atomic uint64_t ident;           /* 命中的 keeper 写入次数（不论是否注入） */
    _Atomic uint64_t inj;             /* 真正注入次数（≤ M8C_MAX_INJ） */
    _Atomic uint64_t latch;           /* 锁定的 keeper 指针（0 = 未锁） */
    _Atomic uint64_t latchns;         /* 锁定时刻 */
    _Atomic unsigned nrec;            /* 注入记录数（认领即写，不覆盖） */
    m8c_rec_t r[M8C_REC];
} g_m8c;
/* 条件注入。返回 0 = 不改（x1 保持原值）；非 0 = 新的 x1。
   只在 keeper 命中（且已锁定同一个）且未超上限时返回注入值；写前把
   原 token（keeper+0x10）/ keeper / LR / tick / 旧 x1 记进记录环。 */
static uint64_t m8c_inject(uint64_t keeper, uint64_t newtok, uint64_t lr) {
    if (atomic_load_explicit(&g_m8c_on, memory_order_relaxed) < 1) return 0;   /* 0 = 关（对照 boot 走这条） */
    if (!m7v_ptr_ok(keeper)) return 0;
    uint64_t base = g_obs_main_base;
    if (!base) return 0;
    uint64_t cur = 0, pend = 0;
    if (!m7v_rd(&cur, keeper + 0x08, 8)) return 0;     /* +0x08 = 当前正在派发的 token */
    m7v_rd(&pend, keeper + 0x10, 8);                   /* +0x10 = 挂起的新 token（写前原值） */
    uint64_t lt = atomic_load_explicit(&g_m8c.latch, memory_order_relaxed);
    if (!lt) {                                          /* 首次：按"当前/挂起 token ∈ 判别集"认 keeper */
        int hit = 0;
        for (int i = 0; i < M8C_TGT && !hit; i++) {
            if (cur >= base && (cur - base) == (uint64_t)m8c_tgt[i]) hit = 1;
            if (!hit && pend >= base && (pend - base) == (uint64_t)m8c_tgt[i]) hit = 1;
        }
        if (!hit) return 0;
        uint64_t exp = 0;
        atomic_compare_exchange_strong_explicit(&g_m8c.latch, &exp, keeper, memory_order_relaxed, memory_order_relaxed);
        if (exp && exp != keeper) return 0;             /* 并发下别人先锁了别的 keeper：不抢 */
        if (!exp) atomic_store_explicit(&g_m8c.latchns, smo_obs_tick(), memory_order_relaxed);
    } else if (lt != keeper) return 0;                  /* 已锁定：只认这一个 keeper */
    uint64_t idx = atomic_fetch_add_explicit(&g_m8c.ident, 1, memory_order_relaxed);   /* 第 idx+1 次命中 */
    uint32_t toff = 0; int skip = 1, cap = M8C_MAX_INJ;
    m8c_plan(atomic_load_explicit(&g_m8c_on, memory_order_relaxed), &toff, &skip, &cap);
    if (idx + 1 < (uint64_t)skip) return 0;             /* 还没到起始命中 */
    uint64_t nv = base + (uint64_t)toff;
    if (newtok == nv) return 0;                         /* 已经是目标值：不改（不占注入额） */
    uint64_t n = atomic_fetch_add_explicit(&g_m8c.inj, 1, memory_order_relaxed);
    if (n >= (uint64_t)cap) return 0;                   /* 上限：超了就只观察 */
    unsigned k = atomic_fetch_add_explicit(&g_m8c.nrec, 1, memory_order_relaxed);
    if (k < M8C_REC) {
        m8c_rec_t *r = &g_m8c.r[k];
        r->tick = smo_obs_tick(); r->keeper = keeper; r->cur8 = cur; r->pend10 = pend;
        r->old_x1 = newtok; r->new_x1 = nv; r->lr = lr;
    }
    return nv;
}
/* 注入门专用观察器：**先按 M8B 原样记账，再算要不要改 x1**（返回 0 = 不改）。
   只有 id 104 的 stub（30 字变体）指向这里；其它门仍指 smo_obs_observer ⇒ 零影响。 */
static uint64_t smo_obs_observer_m8c(uint64_t x0, uint64_t gate, uint64_t x2, uint64_t x3, uint64_t x4) {
    smo_obs_observer(x0, gate, x2, x3, x4);
    if (gate != SMO_OBS_M8B_G1) return 0;
    return m8c_inject(x0, x3, x4);
}
/* dump 侧访问器（threads.c 的 [m8c] 行用；定长、无锁读、无 I/O） */
int smo_obs_m8c_nrec(void) {
    unsigned v = atomic_load_explicit(&g_m8c.nrec, memory_order_relaxed);
    return (int)(v > M8C_REC ? M8C_REC : v);
}
int smo_obs_m8c_rec(int i, uint64_t m[7]) {
    if (i < 0 || i >= M8C_REC) return 0;
    if ((unsigned)i >= atomic_load_explicit(&g_m8c.nrec, memory_order_relaxed)) return 0;
    const m8c_rec_t *r = &g_m8c.r[i];
    m[0] = r->tick; m[1] = r->keeper; m[2] = r->cur8; m[3] = r->pend10;
    m[4] = r->old_x1; m[5] = r->new_x1; m[6] = r->lr;
    return 1;
}
void smo_obs_m8c_sum(uint64_t *ident, uint64_t *inj, uint64_t *latch, uint64_t *latchns, int *want) {
    *ident = atomic_load_explicit(&g_m8c.ident, memory_order_relaxed);
    *inj = atomic_load_explicit(&g_m8c.inj, memory_order_relaxed);
    *latch = atomic_load_explicit(&g_m8c.latch, memory_order_relaxed);
    *latchns = atomic_load_explicit(&g_m8c.latchns, memory_order_relaxed);
    *want = atomic_load_explicit(&g_m8c_on, memory_order_relaxed);
}
/* stub 骨架字（gen_obs_stub.py capstone 校验过）：w0-w19 固定，w9 随 gate 变，
   w20-w23=被换出的 4 条原指令，w24=b 入口+0x10（安装时算），w25/w26=.quad 观察器。
   x18（合成 TCB，OHOS 载体方案）与 entry.S 的 runtime_gate 同样进栈保存。
   round16b：新增 w6 str x8 + w7 mov x2,x8 / w8 mov x3,x1（观察器第 3/4 参=门点原
   x8/x1——movz gate 会覆盖 x1，先抄走）+ w14 复原 x8。
   round21：新增 w10 ldr x4,[sp,#8]——观察器第 5 参=门点 LR（w0 已把 x30 存 [sp,#8]；
   原 x4 在 w3 已入栈、w16 ldp 复原，LR 值仅存在于观察器调用窗口）。ldr x16 字面量
   距离仍 0x38（w11→w25）。x19-x28 是 callee-saved，C 观察器按 ABI 自存自复。 */
static const uint32_t smo_obs_prologue[20] = {
    0xa9ba7bfd,   /* stp x29, x30, [sp, #-0x60]!      */
    0xa90107e0,   /* stp x0, x1, [sp, #0x10]          */
    0xa9020fe2,   /* stp x2, x3, [sp, #0x20]          */
    0xa90317e4,   /* stp x4, x5, [sp, #0x30]          */
    0xa9041fe6,   /* stp x6, x7, [sp, #0x40]          */
    0xf9002bf2,   /* str x18, [sp, #0x50]             */
    0xf9002fe8,   /* str x8, [sp, #0x58]（round16b）  */
    0xaa0803e2,   /* mov x2, x8（round16b：参3）      */
    0xaa0103e3,   /* mov x3, x1（round16b：参4）      */
    0,            /* movz x1, #<gate>（按门填）       */
    0xf94007e4,   /* ldr x4, [sp, #8]（round21：参5=LR）*/
    0x580001d0,   /* ldr x16, #0x38 → w25 字面量      */
    0xd63f0200,   /* blr x16                          */
    0xf9402bf2,   /* ldr x18, [sp, #0x50]             */
    0xf9402fe8,   /* ldr x8, [sp, #0x58]（round16b）  */
    0xa9441fe6,   /* ldp x6, x7, [sp, #0x40]          */
    0xa94317e4,   /* ldp x4, x5, [sp, #0x30]          */
    0xa9420fe2,   /* ldp x2, x3, [sp, #0x20]          */
    0xa94107e0,   /* ldp x0, x1, [sp, #0x10]          */
    0xa8c67bfd,   /* ldp x29, x30, [sp], #0x60        */
};
/* round13b 修复（崩溃定因）：四字门的重放字里出现 PC 相对指令时不能原样重放——adrp/分支族
   会按 stub 的 PC 计算目标。13b 首跑即崩：round12e cell2 门（0x9b9bc4）重放首字 adrp x8，
   以 stub 页（main+0x14d4xxx）算出 main 尾后地址，w16 读回垃圾 0x540007017100051f，w17
   ldr x8,[x8] SEGV@0x000007017100051f（fault log：pc=stub+0x44 即 w17、x8=该垃圾、
   lr=main+0x9b9bac 恰为门上游，三者互证）。修法：adrp 在安装期把 imm 重算到『stub 内该字
   位置』仍指向原绝对页（同语义）；其余 PC 相对字形（b/bl/b.cond/cbz/cbnz/tbz/tbnz/adr/
   ldr-literal）本机制不支持，直接拒装该门（fail-soft）。E1/E2/kick1 等四字门经此扫描
   全为寄存器+立即数字形，不受影响。w[4] 为 4 个重放字（就地改写）；off=门在 main 的偏移，
   stub_off=stub 在 main 的偏移。返回 0=拒装（why 填原因）。
   round16b：bl 加入重定位（DRAIN-LOOP 窗口第 4 字就是 bl 0x8ad1f0）——同绝对目标重算
   imm26（±128MiB；stub 页与 main 代码最远 ~21MiB，足够）。返回地址语义原样：重放 bl 的
   lr=下一重放字，bl 在末字时 lr=&w[24]=b 回入口+0x10，kick（含 0x8ad228 br x1 尾调链）
   最终都回到原 bl 的后继 0x8abfa4。b/b.cond/cbz 族仍拒装（cbz imm19 仅 ±1MiB，stub 页
   距代码 ~16MiB，物理不可重定位）。
   round21：重放字位移 w19→w20（prologue 增 LR 参 1 字），wo 基准随之 +4。 */
static int smo_obs_replay_fix(uint32_t w[4], uint32_t off, uint64_t stub_off, int wbase, char *why, size_t whyn) {
    for (int i = 0; i < 4; i++) {
        uint32_t v = w[i];
        uint64_t wo = stub_off + (uint64_t)(20 + wbase + i) * 4;   /* 该字在 stub 内的 main 相对地址（M8C：wbase=0 原样 / 1 = 30 字变体） */
        if ((v & 0x9f000000u) == 0x90000000u) {             /* adrp：重算 imm（同绝对页） */
            int64_t imm = (int64_t)((v >> 29) & 3) | ((int64_t)((v >> 5) & 0x7ffff) << 2);
            if (imm & (1 << 20)) imm -= 1 << 21;
            uint64_t page_abs = (off & ~0xfffu) + ((uint64_t)imm << 12);   /* 原绝对页 */
            int64_t want = (int64_t)(page_abs - (wo & ~0xfffu)) >> 12;
            if (want < -(1 << 20) || want >= (1 << 20)) {
                snprintf(why, whyn, "adrp relocation out of range (%+lld pages)", (long long)want);
                return 0;
            }
            w[i] = (v & 0x9f00001fu) | ((uint32_t)(want & 3) << 29) | ((uint32_t)((want >> 2) & 0x7ffff) << 5);
        } else if ((v >> 26) == 0x25u) {                    /* bl（round16b）：重算 imm26（同绝对目标） */
            uint32_t imm26 = v & 0x3ffffffu;
            int64_t disp = (imm26 & (1u << 25)) ? (int64_t)imm26 - (1 << 26) : (int64_t)imm26;
            int64_t tgt = (int64_t)off + 4 * i + (disp << 2);   /* 原 target（main 相对） */
            int64_t want = tgt - (int64_t)wo;
            if (want < -(1 << 27) || want >= (1 << 27) || want & 3) {
                snprintf(why, whyn, "bl relocation out of range (%+#llx)", (long long)want);
                return 0;
            }
            w[i] = (v & 0xfc000000u) | (((uint64_t)want >> 2) & 0x3ffffff);
        } else if ((v >> 26) == 0x05 || (v >> 26) == 0x25   /* b / bl */
                   || (v & 0xff000000u) == 0x54000000u      /* b.cond */
                   || (v & 0x7f000000u) >= 0x34000000u && (v & 0x7f000000u) <= 0x37000000u   /* cbz/cbnz/tbz/tbnz（含 sf 位屏蔽） */
                   || (v & 0x9f000000u) == 0x10000000u      /* adr */
                   || (v & 0xbf000000u) == 0x18000000u) {   /* ldr-literal */
            snprintf(why, whyn, "PC-relative word %#x at slot %d unsupported", v, i);
            return 0;
        }
    }
    return 1;
}
/* M10A：安装期读开关文件（threads.c 的构造函数保证文件存在；读不到 ⇒ 0 = 完全关）。
   返回 0/1/2；`limit` = 文件第二字符（'1'..'3'，缺省 1）。 */
static int m10a_read_file(int *limit) {
    const char *pa[2] = { M10A_PATH_B, M10A_PATH_A };
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(pa[i], "r");
        if (!f) continue;
        char b[16] = { 0 };
        size_t n = fread(b, 1, sizeof b - 1, f);
        fclose(f);
        if (!n) return 0;
        int mode = (b[0] >= '0' && b[0] <= '2') ? b[0] - '0' : 0;
        if (limit) *limit = (n > 1 && b[1] >= '1' && b[1] <= '3') ? b[1] - '0' : 1;
        return mode;
    }
    return 0;
}
static void smo_obs_gates_install(const Module *m) {
    /* M10A：安装期读一次开关（决定门 108 装原骨架还是 35 字强制变体）。读不到 ⇒ 0 = 完全关。
       运行期开关只改 mask（强制次数），不改 stub 形态；mode 0 时装出的字节与 M9C 逐字节相同。 */
    {
        int lim = 1;
        int mode = m10a_read_file(&lim);
        atomic_store_explicit(&g_m10a_install, mode, memory_order_relaxed);
        atomic_store_explicit(&g_m10a_limit, lim, memory_order_relaxed);
        atomic_store_explicit(&g_m10a_mode, mode, memory_order_relaxed);
        fprintf(stderr, "[odyssey] m10a: install mode=%d limit=%d (0=off / 1=force / 2=record-only)\n", mode, lim);
    }
    if (strcmp(m->name, "main")) return;
    g_obs_main_base = (uint64_t)(uintptr_t)m->base;   /* round12e：cell 链快照用（任何 veneer 生效前已就位） */
    {   /* round16b：tick→ns 换算基准点（安装时刻；dump 侧取第二点内插） */
        extern uint64_t runtime_now_ns(void);
        g_obs_tick0 = smo_obs_tick(); g_obs_ns0 = runtime_now_ns(); g_obs_tick0_set = 1;
    }
    uint64_t tail = (m->file_size + 0x3fff) & ~0x3fffULL;   /* main 的尾部保留页（loader.c:184 特批的 +0x4000） */
    if (m->size < tail + 0x4000) { fprintf(stderr, "[odyssey] obs gates: no tail page (size=%#llx)\n", (unsigned long long)m->size); return; }
    uint8_t *page = m->base + tail;
    uint8_t *p = page + 0x100;
    unsigned installed = 0;
    for (size_t s = 0; s < sizeof smo_obs_site / sizeof *smo_obs_site; s++) {
        const smo_obs_site_t *site = &smo_obs_site[s];
        uint32_t have[4]; memcpy(have, m->base + site->off, sizeof have);
        /* 单字门（tail=1）只占 1 条 b：校验该字与其后继原字（word[1]），不碰再后指令。 */
        int ok = site->tail ? (have[0] == site->word[0] && have[1] == site->word[1])
                            : (have[0] == site->word[0] && have[1] == site->word[1] &&
                               have[2] == site->word[2] && have[3] == site->word[3]);
        if (!ok) {
            fprintf(stderr, "[odyssey] obs gate %#x: found %08x %08x %08x %08x, expected the four shipped words — NOT installed\n",
                    site->off, have[0], have[1], have[2], have[3]);
            continue;
        }
        /* M8C：id 104 用 30 字变体（多 3 字：w20 mov x17,x0 / w25 cbz x17,+8 / w26 mov x1,x17），
           其余门仍是 27 字原样（字节逐一不变）。
           M10A：id 108 在安装期 smo_m10a≠0 时改 35 字变体（见下）。
           ncopy 固定 30 字 = 与原实现逐字节相同（原实现永远拷 30 字，多出的 3 字落到下一个 stub
           的预留区/页尾死区，安装顺序保证被后写覆盖）；35 字变体只拷自己的 140 B。 */
        int nw = site->gate == SMO_OBS_M8B_G1 ? 30
               : (site->gate == SMO_OBS_M16A_GORP || site->gate == SMO_OBS_M17A_QUAD ||
                  site->gate == SMO_OBS_M18A_UIFILL || site->gate == SMO_OBS_M18A_PANEL ||
                  site->gate == SMO_OBS_M19A_RGB) ? 30 : 27;
        if (site->gate == SMO_OBS_M8E2_G1 && atomic_load_explicit(&g_m10a_install, memory_order_relaxed))
            nw = 35;
        uint8_t *stub = p; p += (size_t)nw * 4;
        size_t ncopy = (site->gate == SMO_OBS_M8E2_G1 && nw == 35) ? 35u * 4 : 30u * 4;
        uint32_t w[35];
        memcpy(w, smo_obs_prologue, sizeof smo_obs_prologue);
        w[27] = w[28] = w[29] = 0xd503201fu;   /* M8C：27 字门永不执行这 3 字（nop 填充，随 memcpy 一起写成确定值）；30 字变体会覆盖 */
        w[9] = 0xd2800000u | ((uint32_t)site->gate << 5) | 1;                /* movz x1, #gate */
        uint64_t fn = (uint64_t)(uintptr_t)&smo_obs_observer;
        memcpy(w + 25, &fn, 8);                                             /* .quad 观察器地址 */
        if (site->tail == 2) {
            /* M7w 普通字尾门（tail=2）：word[0] 是非分支、无 PC 相对字段的指令（本机制
               只此一处使用，mov 类），原义在 stub 内重放（同语义），随后 b 回 off+4；
               后继字 word[1] 只作安装期窗校验，不重放（后继地址原样存在，链式门/原指令
               都照旧）。veneer 与单字 b 门同款：1 条 b→stub。 */
            uint32_t v = site->word[0];
            int branchy = (v >> 26) == 0x05u || (v >> 26) == 0x25u || (v >> 24) == 0x54u ||
                          (v >> 24) == 0x34u || (v >> 24) == 0x35u || (v >> 24) == 0xb4u ||
                          (v >> 24) == 0xb5u || (v >> 24) == 0x36u || (v >> 24) == 0x37u ||
                          (v >> 24) == 0xb6u || (v >> 24) == 0xb7u || (v >> 24) == 0xd6u ||
                          (v & 0x1f000000u) == 0x10000000u || (v & 0x3b000000u) == 0x18000000u;
            if (branchy) {
                fprintf(stderr, "[odyssey] obs gate %#x: plain-word tail got a branch-family word %08x -- NOT installed\n", site->off, v);
                continue;
            }
            w[20] = v;                                                       /* mov w0,wzr 原义重放 */
            int64_t back = (int64_t)(m->base + site->off + 4) - (int64_t)(stub + 21 * 4);
            if (back < -(1 << 27) || back >= (1 << 27) || back & 3) {
                fprintf(stderr, "[odyssey] obs gate %#x: plain-word back-branch out of range (%+#llx) -- NOT installed\n", site->off, (long long)back);
                continue;
            }
            w[21] = 0x14000000u | (((uint64_t)back >> 2) & 0x3ffffff);       /* b off+4 */
            for (int i = 22; i <= 24; i++) w[i] = 0xd503201fu;               /* nop 填充 */
            memcpy(stub, w, ncopy);
            int64_t vb = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(m->base + site->off);
            if (vb < -(1 << 27) || vb >= (1 << 27) || vb & 3) {
                fprintf(stderr, "[odyssey] obs gate %#x: veneer branch out of range (%+#llx) -- NOT installed\n", site->off, (long long)vb);
                continue;
            }
            uint32_t veneer1 = 0x14000000u | (((uint64_t)vb >> 2) & 0x3ffffff);
            memcpy(m->base + site->off, &veneer1, 4);
            installed++;
            fprintf(stderr, "[odyssey] obs gate main+%#x (id %u, tail plain-word replayed + b back) installed, stub at main+%#llx\n",
                    site->off, site->gate, (unsigned long long)(uintptr_t)(stub - m->base));
            continue;
        }
        if (site->tail == 3) {
            /* M7b 单 bl 门：word[0] 必须是 bl（op=100101）。veneer 1 条 b→stub 只换 bl 这
               一个字；stub 里重放重定位后的 bl（被调者 ret 落到 w[21]），w[21]=b 回 site+4
               ——调用语义与原 bl 全同（caller 的 LR/寄存器由 prologue 原样保/恢复，观察器
               只读）。word[1]=后继字仅作安装期窗校验，不换。 */
            uint32_t v = site->word[0];
            if ((v >> 26) != 0x25u) {
                fprintf(stderr, "[odyssey] obs gate %#x: tail=3 word %08x is not a bl — NOT installed\n", site->off, v);
                continue;
            }
            uint32_t imm26 = v & 0x3ffffffu;
            int64_t disp = (imm26 & (1u << 25)) ? (int64_t)imm26 - (1 << 26) : (int64_t)imm26;
            int64_t tgt = (int64_t)site->off + (disp << 2);            /* 原 bl 目标（main 相对） */
            if (tgt <= 0 || (uint64_t)tgt >= m->file_size) {
                fprintf(stderr, "[odyssey] obs gate %#x: tail=3 bl target main+%#llx out of module — NOT installed\n",
                        site->off, (unsigned long long)tgt);
                continue;
            }
            uint64_t so = (uint64_t)(uintptr_t)(stub - m->base);
            int64_t want = tgt - (int64_t)(so + 20 * 4);               /* 重放 bl 的 imm 重算 */
            if (want < -(1 << 27) || want >= (1 << 27) || want & 3) {
                fprintf(stderr, "[odyssey] obs gate %#x: tail=3 bl replay out of range (%+#llx) — NOT installed\n", site->off, (long long)want);
                continue;
            }
            w[20] = (v & 0xfc000000u) | (((uint64_t)want >> 2) & 0x3ffffff);
            if (site->gate == SMO_OBS_M8E2_G1 && nw == 35) {
                /* ---- M10A（2026-11-08）：门 108 的 **35 字强制变体** ----
                   在"原样重放那条 bl（= 真调 0x4d2bd0，原始返回值落在 w0）"之后插 6 条：
                     w27 ldr  x9,[pc,#24] → w33   （stub 内固定位移，与 stub 落哪无关；w25/w26 的
                                                   观察器 .quad 与 w33/w34 的 &state .quad 同为
                                                   4 对齐 8 字节字面量读 —— 与既有骨架同款）
                     w28 ldr  w10,[x9]        mask（0=恒等 / 1=置 bit0）
                     w29 str  w0,[x9,#4]      **原始返回值**
                     w30 orr  w0,w0,w10       mask=0 时 bit0..bit7 全同 ⇒ 语义恒等
                     w31 str  w0,[x9,#8]      **返回给调用方的值**
                     w32 b    门点+4
                     w33..w34 .quad &g_m10a_st
                   w21 = b → w27（跳过 w25/w26 观察器 .quad 数据）；w22..w24 = nop。
                   寄存器代价：x9/x10（被换出的 bl 之后都不是活值）、NZCV（后继两条路径都无消费者）。
                   ⚠️ 不用 adrp：libgame.so 的 .data 与 main 模块可能相距 >4GiB，字面量没有这个限制。 */
                w[21] = 0x14000006u;                                             /* b stub+27*4 */
                for (int i = 22; i <= 24; i++) w[i] = 0xd503201fu;               /* nop */
                w[27] = 0x580000c9u;                                             /* ldr x9,[pc,#24] → w33 */
                w[28] = 0xb940012au;                                             /* ldr w10,[x9] */
                w[29] = 0xb9000520u;                                             /* str w0,[x9,#4] */
                w[30] = 0x2a0a0000u;                                             /* orr w0,w0,w10 */
                w[31] = 0xb9000920u;                                             /* str w0,[x9,#8] */
                {
                    int64_t back35 = (int64_t)(site->off + 4) - (int64_t)(so + 32 * 4);
                    if (back35 < -(1 << 27) || back35 >= (1 << 27) || back35 & 3) {
                        fprintf(stderr, "[odyssey] obs gate %#x: m10a back-branch out of range (%#llx) — NOT installed\n",
                                site->off, (long long)back35);
                        continue;
                    }
                    w[32] = 0x14000000u | (((uint64_t)back35 >> 2) & 0x3ffffff); /* b 门点+4 */
                }
                {
                    uint64_t st_addr = (uint64_t)(uintptr_t)&g_m10a_st;
                    memcpy(w + 33, &st_addr, 8);                                 /* w33..w34 = .quad &state */
                }
                memcpy(stub, w, ncopy);
                int64_t vb35 = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(m->base + site->off);
                if (vb35 < -(1 << 27) || vb35 >= (1 << 27) || vb35 & 3) {
                    fprintf(stderr, "[odyssey] obs gate %#x: m10a veneer branch out of range (%#llx) — NOT installed\n",
                            site->off, (long long)vb35);
                    continue;
                }
                uint32_t veneer35 = 0x14000000u | (((uint64_t)vb35 >> 2) & 0x3ffffff);
                memcpy(m->base + site->off, &veneer35, 4);                   /* 仍只换 bl 这 1 个字 */
                installed++;
                g_obs_stub_off[site->gate] = so;
                fprintf(stderr, "[odyssey] obs gate main+%#x (id %u, M10A 35-word force variant: bl replayed then "
                        "orr w0,w0,mask / raw->[st+4] ret->[st+8]) installed, stub at main+%#llx\n",
                        site->off, site->gate, (unsigned long long)so);
                continue;
            }
            int64_t back = (int64_t)(site->off + 4) - (int64_t)(so + 21 * 4);   /* 被调者 ret 落点 = b site+4 */
            if (back < -(1 << 27) || back >= (1 << 27) || back & 3) {
                fprintf(stderr, "[odyssey] obs gate %#x: tail=3 back-branch out of range (%+#llx) — NOT installed\n", site->off, (long long)back);
                continue;
            }
            w[21] = 0x14000000u | (((uint64_t)back >> 2) & 0x3ffffff);
            for (int i = 22; i <= 24; i++) w[i] = 0xd503201fu;         /* nop 填充 */
            memcpy(stub, w, ncopy);
            int64_t vb = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(m->base + site->off);
            if (vb < -(1 << 27) || vb >= (1 << 27) || vb & 3) {
                fprintf(stderr, "[odyssey] obs gate %#x: tail=3 veneer branch out of range (%+#llx) — NOT installed\n", site->off, (long long)vb);
                continue;
            }
            uint32_t veneer1 = 0x14000000u | (((uint64_t)vb >> 2) & 0x3ffffff);   /* 单字 b→stub */
            memcpy(m->base + site->off, &veneer1, 4);
            installed++;
            g_obs_stub_off[site->gate] = so;                           /* dump 侧 LR→site 翻译用 */
            fprintf(stderr, "[odyssey] obs gate main+%#x (id %u, tail bl->main+%#llx replayed) installed, stub at main+%#llx\n",
                    site->off, site->gate, (unsigned long long)tgt, (unsigned long long)so);
            continue;
        }
        if (site->tail) {
            /* 单字尾跳门：出厂 word[0] 是无条件 b（解出目标，stub 末尾原义重定向）或
               round18 ③ 的 br x1 / round19 ② 的 ret（寄存器分支，无静态目标可解码——
               stub 末尾原样重放该字：prologue 已把 x1/x30 复原，重放 br x1 跳寄存器
               目标、重放 ret 返回调用者；观察器经参4（w8 mov x3,x1 先抄）拿 x1 真值）。
               veneer 只换这 1 个字（b→stub），相邻指令一个不动。 */
            int brx1 = site->word[0] == 0xd61f0020u;
            int reto = site->word[0] == 0xd65f03c0u;                       /* round19：ret（thunk NULL 出口门） */
            uint64_t dst = 0;                                              /* 寄存器分支：目标在寄存器里 */
            if (brx1 || reto) {
                w[20] = site->word[0];                                     /* 原义重放 br x1 / ret */
            } else {
                uint32_t imm26 = site->word[0] & 0x3ffffffu;
                int64_t disp = (imm26 & (1u << 25)) ? (int64_t)imm26 - (1 << 26) : (int64_t)imm26;
                int64_t tofs = (int64_t)site->off + (disp << 2);            /* 原目标（main 相对） */
                if (disp == 0 || tofs <= 0 || (uint64_t)tofs >= m->file_size) {
                    fprintf(stderr, "[odyssey] obs gate %#x: tail word not an in-module b (target main+%#llx) — NOT installed\n",
                            site->off, (unsigned long long)tofs);
                    continue;
                }
                dst = (uint64_t)(uintptr_t)(m->base + tofs);               /* 原目标绝对地址 */
                int64_t branch = (int64_t)dst - (int64_t)(stub + 20 * 4);  /* stub 尾 b 原目标 */
                if (branch < -(1 << 27) || branch >= (1 << 27) || branch & 3) {
                    fprintf(stderr, "[odyssey] obs gate %#x: stub tail branch out of range (%+#llx) — NOT installed\n", site->off, (long long)branch);
                    continue;
                }
                w[20] = 0x14000000u | (((uint64_t)branch >> 2) & 0x3ffffff);
            }
            for (int i = 21; i <= 24; i++) w[i] = 0xd503201fu;              /* nop 填充（永不执行） */
            memcpy(stub, w, ncopy);
            int64_t vb = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(m->base + site->off);
            if (vb < -(1 << 27) || vb >= (1 << 27) || vb & 3) {
                fprintf(stderr, "[odyssey] obs gate %#x: veneer branch out of range (%+#llx) — NOT installed\n", site->off, (long long)vb);
                continue;
            }
            uint32_t veneer1 = 0x14000000u | (((uint64_t)vb >> 2) & 0x3ffffff);   /* 单字 b→stub */
            memcpy(m->base + site->off, &veneer1, 4);
            installed++;
            if (brx1)
                fprintf(stderr, "[odyssey] obs gate main+%#x (id %u, tail br x1 replayed) installed, stub at main+%#llx\n",
                        site->off, site->gate, (unsigned long long)(uintptr_t)(stub - m->base));
            else if (reto)
                fprintf(stderr, "[odyssey] obs gate main+%#x (id %u, tail ret replayed) installed, stub at main+%#llx\n",
                        site->off, site->gate, (unsigned long long)(uintptr_t)(stub - m->base));
            else
                fprintf(stderr, "[odyssey] obs gate main+%#x (id %u, tail b->main+%#llx) installed, stub at main+%#llx\n",
                        site->off, site->gate, (unsigned long long)(dst - (uint64_t)(uintptr_t)m->base),
                        (unsigned long long)(uintptr_t)(stub - m->base));
            continue;
        }
        if (site->gate == SMO_OBS_M8B_G1) {
            /* ---- M8C（2026-11-08）：id 104 的 **30 字可注入变体** ----
               只把"重放完 4 条出厂指令之后、跳回入口+0x10 之前"这一段加 3 字，让观察器能
               有条件地改 **x1**（= setNerve 的新 nerve token），其它寄存器一律走原骨架复原：
                 **★ 关键：`mov x17,x0` 必须紧跟在 `blr` 之后**——原骨架 w18 的
                   `ldp x0,x1,[sp,#0x10]` 会把 x0 复原成门点原 x0（= keeper），
                   放到它后面拿到的就是 keeper 而不是观察器返回值。
                   （M8C 首版正是把这条放在 w20，于是 cbz 恒不成立、x1 被写成 keeper，
                    boot 在 ~2 s 处 SIGSEGV：pc=main+0x12cf9e8、lr=main+0x6c76b4、
                    x0=x1=keeper 三者自证。见报告 P1.5 无效 boot。）
                 w0..w12 = 原骨架前 13 字（w9 = movz x1,#gate 照旧）
                 w13 = mov x17, x0     ← 观察器返回值（0 = 不改 / 非 0 = 新 x1）
                 w14..w20 = 原骨架第 13..19 字（整体后移 1 字；w20 的
                            `ldp x29,x30,[sp],#0x60` 仍是最后一字）
                 w11 的 ldr x16 字面量距离 0x38 → 0x44（.quad 仍挪到 w28；两处都是
                     stub 内部固定位移，与 stub 落在哪个地址无关）
                 w21..w24 = 原样重放被换出的 4 条出厂指令（与 tail=0 门同一语义，replay_fix
                            的 wbase=1 让 PC 相对字按 w21 起的真实位置重算）
                 w25 = cbz x17, +8   ← 返回 0 ⇒ 跳过 w26，x1 保持复原值（**对照 boot 走这条**）
                 w26 = mov x1, x17   ← **注入点：只改 x1**
                 w27 = b 入口+0x10
                 w28..w29 = .quad smo_obs_observer_m8c
               字面编码经 capstone 往返校验（/tmp/m8c_work/c_stub.py：58000230 = ldr x16,#0x44 /
               aa0003f1 = mov x17,x0 / b4000051 = cbz x17,#8 / aa1103e1 = mov x1,x17）。 */
            memcpy(w, smo_obs_prologue, 13 * 4);                        /* w0..w12（w9 下面重填） */
            w[9] = 0xd2800000u | ((uint32_t)site->gate << 5) | 1;       /* movz x1, #gate */
            w[13] = 0xaa0003f1u;                                        /* mov x17, x0（紧贴 blr 之后） */
            memcpy(w + 14, smo_obs_prologue + 13, 7 * 4);               /* w14..w20 = 骨架 13..19 */
            w[11] = 0x58000230u;                                        /* ldr x16, #0x44（→ w28 = stub+0x70） */
            for (int i = 0; i < 4; i++) w[21 + i] = site->word[i];
            {
                char why[96];
                if (!smo_obs_replay_fix(&w[21], site->off, (uint64_t)(uintptr_t)(stub - m->base), 1, why, sizeof why)) {
                    fprintf(stderr, "[odyssey] obs gate %#x: m8c replay %s — NOT installed\n", site->off, why);
                    continue;
                }
            }
            w[25] = 0xb4000051u;                                        /* cbz x17, +8 */
            w[26] = 0xaa1103e1u;                                        /* mov x1, x17 */
            uint64_t fni = (uint64_t)(uintptr_t)&smo_obs_observer_m8c;
            memcpy(w + 28, &fni, 8);
            int64_t br2 = (int64_t)(m->base + site->off + 0x10) - (int64_t)(stub + 27 * 4);
            if (br2 < -(1 << 27) || br2 >= (1 << 27) || br2 & 3) {
                fprintf(stderr, "[odyssey] obs gate %#x: m8c branch out of range (%#llx) — NOT installed\n", site->off, (long long)br2);
                continue;
            }
            w[27] = 0x14000000u | (((uint64_t)br2 >> 2) & 0x3ffffff);    /* b 入口+0x10 */
            memcpy(stub, w, ncopy);
            {
                uint32_t veneer[2] = {0x58000051, 0xd61f0220};           /* ldr x17,#8; br x17 */
                uint64_t target = (uint64_t)(uintptr_t)stub;
                memcpy(m->base + site->off, veneer, 8);
                memcpy(m->base + site->off + 8, &target, 8);
            }
            installed++;
            g_obs_stub_off[site->gate] = (uint64_t)(uintptr_t)(stub - m->base);   /* dump 侧回读注入字用 */
            fprintf(stderr, "[odyssey] obs gate main+%#x (id %u, M8C 30-word inject variant: w13 mov x17,x0 / "
                    "w25 cbz x17,+8 / w26 mov x1,x17 / quad=observer_m8c) installed, stub at main+%#llx\n",
                    site->off, site->gate, (unsigned long long)(uintptr_t)(stub - m->base));
            continue;
        }
        if (site->gate == SMO_OBS_M16A_GORP || site->gate == SMO_OBS_M17A_QUAD ||
            site->gate == SMO_OBS_M18A_UIFILL || site->gate == SMO_OBS_M18A_PANEL ||
            site->gate == SMO_OBS_M19A_RGB) {
            /* ---- M16A（2026-11-08）：门 114 的 **30 字 7 参变体** ----
               目的：把门点原 x2/x3 也交给观察器（原骨架只给 x0 / 原 x8 / 原 x1 / LR）。
               做法：只替换骨架里"blr 之前那一段"的三个字位，其余逐字照抄：
                 w0..w10 = 骨架 0..10（w9 = movz x1,#gate 照旧；w10 = ldr x4,[sp,#8] = LR）
                 w11 = ldr x5,[sp,#0x20]   ← 观察器第 6 参 = 门点原 x2（w2 已把它存进 [sp,#0x20]）
                 w12 = ldr x6,[sp,#0x28]   ← 观察器第 7 参 = 门点原 x3
                 w13 = ldr x16,#0x38       ← 字面量读 w27（stub 内部固定位移，与落址无关）
                 w14 = blr x16
                 w15..w21 = 骨架 13..19（ldr x18 / ldr x8 / ldp x6,x7 / ldp x4,x5 / ldp x2,x3 /
                            ldp x0,x1 / ldp x29,x30 —— x5/x6 在这两步里被复原，观察器已拿到真值）
                 w22..w25 = 原样重放被换出的 4 条出厂指令（wbase=2 让 PC 相对字按实际位置重算）
                 w26 = b 入口+0x10
                 w27..w28 = .quad smo_obs_observer_m16a
                 w29 = nop 填充
               字面编码（capstone 往返校验，/tmp/m16a_work/stub_check.py）：
                 f94013e5 = ldr x5,[sp,#0x20] / f94017e6 = ldr x6,[sp,#0x28]
                 580001d0 = ldr x16,#0x38（w13 在 stub+0x34，0x34+0x38 = 0x6c = w27）
               veneer 与 tail=0 门同款（ldr x17,#8 / br x17 + 8 字节目标 = 16 B，正好盖住四字窗）。
               ⚠️ 本条**只对本门生效**：其它 106 行站表的字节不变（nw/ncopy 分支互斥）。 */
            memcpy(w, smo_obs_prologue, 11 * 4);
            w[9] = 0xd2800000u | ((uint32_t)site->gate << 5) | 1;              /* movz x1, #gate */
            w[11] = 0xf94013e5u;                                               /* ldr x5,[sp,#0x20] */
            w[12] = 0xf94017e6u;                                               /* ldr x6,[sp,#0x28] */
            w[13] = 0x580001d0u;                                               /* ldr x16, #0x38 → w27 */
            w[14] = 0xd63f0200u;                                               /* blr x16 */
            memcpy(w + 15, smo_obs_prologue + 13, 7 * 4);                       /* w15..w21 复原 */
            for (int i = 0; i < 4; i++) w[22 + i] = site->word[i];
            {
                char why[96];
                if (!smo_obs_replay_fix(&w[22], site->off, (uint64_t)(uintptr_t)(stub - m->base), 2, why, sizeof why)) {
                    fprintf(stderr, "[odyssey] obs gate %#x: m16a replay %s — NOT installed\n", site->off, why);
                    continue;
                }
            }
            {
                int64_t b16 = (int64_t)(site->off + 0x10) - (int64_t)((uint64_t)(uintptr_t)(stub - m->base) + 26 * 4);
                if (b16 < -(1 << 27) || b16 >= (1 << 27) || b16 & 3) {
                    fprintf(stderr, "[odyssey] obs gate %#x: m16a back-branch out of range (%#llx) — NOT installed\n",
                            site->off, (long long)b16);
                    continue;
                }
                w[26] = 0x14000000u | (((uint64_t)b16 >> 2) & 0x3ffffff);       /* b 入口+0x10 */
            }
            {
                uint64_t f16 = (uint64_t)(uintptr_t)(site->gate == SMO_OBS_M17A_QUAD
                                                    ? &smo_obs_observer_m17a
                                                    : (site->gate == SMO_OBS_M18A_UIFILL ||
                                                       site->gate == SMO_OBS_M18A_PANEL)
                                                      ? &smo_obs_observer_m18a
                                                      : site->gate == SMO_OBS_M19A_RGB
                                                        ? &smo_obs_observer_m19a
                                                        : &smo_obs_observer_m16a);
                memcpy(w + 27, &f16, 8);                                        /* w27..w28 = .quad observer */
            }
            w[29] = 0xd503201fu;
            memcpy(stub, w, ncopy);
            {
                uint32_t veneer[2] = {0x58000051, 0xd61f0220};                  /* ldr x17,#8; br x17 */
                uint64_t target = (uint64_t)(uintptr_t)stub;
                memcpy(m->base + site->off, veneer, 8);
                memcpy(m->base + site->off + 8, &target, 8);
            }
            installed++;
            g_obs_stub_off[site->gate] = (uint64_t)(uintptr_t)(stub - m->base);
            fprintf(stderr, "[odyssey] obs gate main+%#x (id %u, 30-word 7-arg variant: w11 ldr x5 / "
                    "w12 ldr x6 / quad=observer_%s) installed, stub at main+%#llx\n",
                    site->off, site->gate,
                    site->gate == SMO_OBS_M17A_QUAD ? "m17a"
                    : (site->gate == SMO_OBS_M18A_UIFILL || site->gate == SMO_OBS_M18A_PANEL) ? "m18a"
                    : site->gate == SMO_OBS_M19A_RGB ? "m19a" : "m16a",
                    (unsigned long long)(uintptr_t)(stub - m->base));
            continue;
        }
        for (int i = 0; i < 4; i++) w[20 + i] = site->word[i];               /* 原样重放被换出的 4 条指令（PC 相对字见下方修正/拒装） */
        {   /* round13b：重放字 PC 相对指令修正/拒装（cell2 门 adrp 13b 首跑即崩，见 smo_obs_replay_fix 注） */
            char why[96];
            if (!smo_obs_replay_fix(&w[20], site->off, (uint64_t)(uintptr_t)(stub - m->base), 0, why, sizeof why)) {
                fprintf(stderr, "[odyssey] obs gate %#x: replay %s — NOT installed\n", site->off, why);
                continue;
            }
        }
        int64_t branch = (int64_t)(m->base + site->off + 0x10) - (int64_t)(stub + 24 * 4);
        if (branch < -(1 << 27) || branch >= (1 << 27) || branch & 3) {
            fprintf(stderr, "[odyssey] obs gate %#x: branch out of range (%+#llx) — NOT installed\n", site->off, (long long)branch);
            continue;
        }
        w[24] = 0x14000000u | (((uint64_t)branch >> 2) & 0x3ffffff);         /* b 入口+0x10 */
        memcpy(stub, w, ncopy);
        uint32_t veneer[2] = {0x58000051, 0xd61f0220};                      /* ldr x17,#8; br x17（patch_scene_pick 同款） */
        uint64_t target = (uint64_t)(uintptr_t)stub;
        memcpy(m->base + site->off, veneer, 8);
        memcpy(m->base + site->off + 8, &target, 8);
        installed++;
        fprintf(stderr, "[odyssey] obs gate main+%#x (id %u) installed, stub at main+%#llx\n",
                site->off, site->gate, (unsigned long long)(uintptr_t)(stub - m->base));
    }
    if (!installed) return;
    size_t used = (size_t)(p - page);
    __builtin___clear_cache((char *)page, (char *)p);                       /* 新 stub 指令进 icache（文本侧 veneer 由 seal() 的 sys_icache_invalidate 覆盖） */
    if (mprotect(page, 0x4000, PROT_READ | PROT_EXEC) != 0) {               /* 与 seal() 给补丁区的权限一致；check_permissions 经 smo_obs_rx_range 认得此页 */
        fprintf(stderr, "[odyssey] obs gates: tail page mprotect RX FAILED (gates dead: veneer will fault)\n");
        /* 装不上就把 veneer 撤掉：留一个跳进不可执行页的 veneer 必崩，回退到原指令才安全 */
        for (size_t s = 0; s < sizeof smo_obs_site / sizeof *smo_obs_site; s++) {
            const smo_obs_site_t *site = &smo_obs_site[s];
            uint32_t have[4]; memcpy(have, m->base + site->off, sizeof have);
            if (site->tail) {
                /* 单字门：have[0]=我们写的 veneer b、have[1] 仍为出厂后继字 → 只回写 word[0]。 */
                if (have[0] == site->word[0] || have[1] != site->word[1]) continue;
                uint32_t imm26 = have[0] & 0x3ffffffu;
                int64_t disp = (imm26 & (1u << 25)) ? (int64_t)imm26 - (1 << 26) : (int64_t)imm26;
                uint64_t t = (uint64_t)(uintptr_t)(m->base + site->off) + ((uint64_t)disp << 2);
                if (t < (uint64_t)(uintptr_t)page || t >= (uint64_t)(uintptr_t)(page + 0x4000)) continue;   /* 非 ours */
                memcpy(m->base + site->off, &site->word[0], 4);
                continue;
            }
            int ok = 1; for (int i = 0; i < 4; i++) ok &= have[i] == site->word[i];
            if (ok) continue;   /* 没装上这门（字不符或分支越界），veneer 也没写 */
            uint32_t veneer[2] = {0x58000051, 0xd61f0220};
            uint32_t now[2]; memcpy(now, m->base + site->off, sizeof now);
            if (now[0] == veneer[0] && now[1] == veneer[1])
                memcpy(m->base + site->off, site->word, sizeof site->word);   /* 原样写回 4 条出厂指令 */
        }
        return;
    }
    g_obs_rx_lo = (uint64_t)(uintptr_t)page; g_obs_rx_hi = g_obs_rx_lo + 0x4000;
    fprintf(stderr, "[odyssey] obs gates: %u gate(s), tail page main+%#llx RX, %zu bytes used\n",
            installed, (unsigned long long)tail, used);
}
/* ---- dump 侧访问器（threads.c 的 dump_threads 调） ---- */
uint64_t smo_obs_hits(int gate) { return gate >= 0 && gate < SMO_OBS_N ? atomic_load_explicit(&g_obs_hits[gate], memory_order_relaxed) : 0; }
/* ---- M15A（2026-11-08）：门 113 的三个 dump 侧入口 ----
   ① 累计命中数（= submit 入口被调次数）；
   ② 逐笔归属：port 侧在每笔 draw 点 drain 一次（自上一笔以来的命中，按到达顺序）；
   ③ 直方图：按 (n_early+n_late) 取 top-N（含 x30 = lr、两栏计数、最近一次见到的 A）。 */
uint64_t smo_obs_m15a_k(void) { return atomic_load_explicit(&g_m15a_k, memory_order_relaxed); }
void smo_obs_m15a_meta(uint64_t *k, uint64_t *first_tick, uint64_t *last_tick,
                       uint64_t *first_lr, uint64_t *last_lr, uint64_t *a_first, uint64_t *a_last,
                       uint64_t *h_used, uint64_t *h_ovf, uint64_t *pend_ovf)
{
    if (k) *k = smo_obs_m15a_k();
    if (first_tick) *first_tick = atomic_load_explicit(&g_m15a_first_tick, memory_order_relaxed);
    if (last_tick) *last_tick = atomic_load_explicit(&g_m15a_last_tick, memory_order_relaxed);
    if (first_lr) *first_lr = atomic_load_explicit(&g_m15a_first_lr, memory_order_relaxed);
    if (last_lr) *last_lr = atomic_load_explicit(&g_m15a_last_lr, memory_order_relaxed);
    if (a_first) *a_first = atomic_load_explicit(&g_m15a_a_first, memory_order_relaxed);
    if (a_last) *a_last = atomic_load_explicit(&g_m15a_a_last, memory_order_relaxed);
    if (h_used) *h_used = atomic_load_explicit(&g_m15a_h_used, memory_order_relaxed);
    if (h_ovf) *h_ovf = atomic_load_explicit(&g_m15a_h_ovf, memory_order_relaxed);
    if (pend_ovf) *pend_ovf = atomic_load_explicit(&g_m15a_pend_ovf, memory_order_relaxed);
}
int smo_obs_m15a_drain(uint64_t *lr, uint64_t *a, int max)
{
    if (max <= 0) { atomic_store_explicit(&g_m15a_pend_n, 0, memory_order_relaxed); return 0; }
    int n = atomic_exchange_explicit(&g_m15a_pend_n, 0, memory_order_relaxed);
    int m = n < max ? n : max;
    for (int i = 0; i < m; i++) {
        if (lr) lr[i] = g_m15a_pend[i].lr;
        if (a)  a[i]  = g_m15a_pend[i].a;
    }
    return n;      /* 返回**实际命中数**（>max 时调用方按 max 取用；差值已在 pend_ovf 里单列） */
}
/* top-N 直方图（按 early+late 降序；插入排序，N≤64、桶 512 ⇒ 只在 5 s 的 dump 里跑一次）。 */
int smo_obs_m15a_hist(int N, uint64_t *lr, uint64_t *n_early, uint64_t *n_late, uint64_t *a_last)
{
    int have = 0;
    for (int i = 0; i < N; i++) { lr[i] = 0; n_early[i] = 0; n_late[i] = 0; if (a_last) a_last[i] = 0; }
    for (unsigned i = 0; i < M15A_HIST_N; i++) {
        m15a_h_t *e = &g_m15a_h[i];
        if (!e->lr) continue;
        uint64_t tot = e->n_early + e->n_late;
        int pos = have < N ? have : -1;
        if (pos < 0) {
            if (tot <= (n_early[N - 1] + n_late[N - 1])) continue;
            pos = N - 1;
        }
        int j = pos;
        while (j > 0 && (n_early[j - 1] + n_late[j - 1]) < tot) {
            lr[j] = lr[j - 1]; n_early[j] = n_early[j - 1]; n_late[j] = n_late[j - 1];
            if (a_last) a_last[j] = a_last[j - 1];
            j--;
        }
        lr[j] = e->lr; n_early[j] = e->n_early; n_late[j] = e->n_late;
        if (a_last) a_last[j] = e->a_last;
        if (have < N) have++;
    }
    return have;
}
/* ---- M16A（2026-11-08）：门 114 的 dump 侧入口 ----
   ① 累计命中（= "GORP." trampoline 被调次数）；② 元数据 + 末见载荷；③ top-N x30 直方图；
   ④ 逐笔归属 drain（port 侧每笔 draw 领走自上一笔以来的命中，用来把门 114 钉到具体某笔）。 */
uint64_t smo_obs_m16a_k(void) { return atomic_load_explicit(&g_m16a_k, memory_order_relaxed); }
void smo_obs_m16a_meta(uint64_t *k, uint64_t *first_tick, uint64_t *last_tick,
                       uint64_t *first_lr, uint64_t *last_lr,
                       uint64_t *h_used, uint64_t *h_ovf, uint64_t *pend_ovf,
                       uint64_t *max_per_frame, uint64_t *cur_sub,
                       uint64_t *x0_last, uint64_t *x1_last, uint64_t *x2_last, uint64_t *x3_last,
                       uint64_t *x0_first, uint64_t *sub_first, uint64_t *sub_last)
{
    if (k) *k = smo_obs_m16a_k();
    if (first_tick) *first_tick = atomic_load_explicit(&g_m16a_first_tick, memory_order_relaxed);
    if (last_tick) *last_tick = atomic_load_explicit(&g_m16a_last_tick, memory_order_relaxed);
    if (first_lr) *first_lr = atomic_load_explicit(&g_m16a_first_lr, memory_order_relaxed);
    if (last_lr) *last_lr = atomic_load_explicit(&g_m16a_last_lr, memory_order_relaxed);
    if (h_used) *h_used = atomic_load_explicit(&g_m16a_h_used, memory_order_relaxed);
    if (h_ovf) *h_ovf = atomic_load_explicit(&g_m16a_h_ovf, memory_order_relaxed);
    if (pend_ovf) *pend_ovf = atomic_load_explicit(&g_m16a_pend_ovf, memory_order_relaxed);
    if (max_per_frame) *max_per_frame = atomic_load_explicit(&g_m16a_max_per_frame, memory_order_relaxed);
    if (cur_sub) *cur_sub = atomic_load_explicit(&g_m16a_cur_sub, memory_order_relaxed);
    if (x0_last) *x0_last = atomic_load_explicit(&g_m16a_x0_last, memory_order_relaxed);
    if (x1_last) *x1_last = atomic_load_explicit(&g_m16a_x1_last, memory_order_relaxed);
    if (x2_last) *x2_last = atomic_load_explicit(&g_m16a_x2_last, memory_order_relaxed);
    if (x3_last) *x3_last = atomic_load_explicit(&g_m16a_x3_last, memory_order_relaxed);
    if (x0_first) *x0_first = atomic_load_explicit(&g_m16a_x0_first, memory_order_relaxed);
    if (sub_first) *sub_first = atomic_load_explicit(&g_m16a_sub_first, memory_order_relaxed);
    if (sub_last) *sub_last = atomic_load_explicit(&g_m16a_sub_last, memory_order_relaxed);
}
int smo_obs_m16a_drain(uint64_t *lr, uint64_t *x0, uint64_t *x1, uint64_t *x2, uint64_t *x3, int max)
{
    if (max <= 0) { atomic_store_explicit(&g_m16a_pend_n, 0, memory_order_relaxed); return 0; }
    int n = atomic_exchange_explicit(&g_m16a_pend_n, 0, memory_order_relaxed);
    int m = n < max ? n : max;
    for (int i = 0; i < m; i++) {
        if (lr) lr[i] = g_m16a_pend[i].lr;
        if (x0) x0[i] = g_m16a_pend[i].x0;
        if (x1) x1[i] = g_m16a_pend[i].x1;
        if (x2) x2[i] = g_m16a_pend[i].x2;
        if (x3) x3[i] = g_m16a_pend[i].x3;
    }
    return n;
}
/* top-N（按 early+late 降序；N≤64、桶 512 ⇒ 只在 5 s 的 dump 里跑一次）。 */
int smo_obs_m16a_hist(int N, uint64_t *lr, uint64_t *n_early, uint64_t *n_late)
{
    int have = 0;
    for (int i = 0; i < N; i++) { lr[i] = 0; n_early[i] = 0; n_late[i] = 0; }
    for (unsigned i = 0; i < M16A_HIST_N; i++) {
        m16a_h_t *e = &g_m16a_h[i];
        if (!e->lr) continue;
        uint64_t tot = e->n_early + e->n_late;
        int pos = have < N ? have : -1;
        if (pos < 0) {
            if (tot <= (n_early[N - 1] + n_late[N - 1])) continue;
            pos = N - 1;
        }
        int j = pos;
        while (j > 0 && (n_early[j - 1] + n_late[j - 1]) < tot) {
            lr[j] = lr[j - 1]; n_early[j] = n_early[j - 1]; n_late[j] = n_late[j - 1];
            j--;
        }
        lr[j] = e->lr; n_early[j] = e->n_early; n_late[j] = e->n_late;
        if (have < N) have++;
    }
    return have;
}
/* ---- M17A（2026-11-08）：门 115 的 dump 侧入口（与 M16A 同构，但逐笔带 x2 = 四边形描述符）----
   ① 累计命中（= "带贴图四边形被记录"的次数）；② 元数据 + 末见载荷；③ top-N x30 直方图；
   ④ 逐笔 drain：port 侧每笔 draw 领走自上一笔以来的命中，再由 port 侧安全读 [x2]（贴图键）。 */
uint64_t smo_obs_m17a_k(void) { return atomic_load_explicit(&g_m17a_k, memory_order_relaxed); }
void smo_obs_m17a_meta(uint64_t *k, uint64_t *first_tick, uint64_t *last_tick,
                       uint64_t *first_lr, uint64_t *last_lr,
                       uint64_t *h_used, uint64_t *h_ovf, uint64_t *pend_ovf,
                       uint64_t *max_per_frame, uint64_t *cur_sub,
                       uint64_t *x0_last, uint64_t *x1_last, uint64_t *x2_last, uint64_t *x3_last,
                       uint64_t *x0_first, uint64_t *sub_first, uint64_t *sub_last)
{
    if (k) *k = smo_obs_m17a_k();
    if (first_tick) *first_tick = atomic_load_explicit(&g_m17a_first_tick, memory_order_relaxed);
    if (last_tick) *last_tick = atomic_load_explicit(&g_m17a_last_tick, memory_order_relaxed);
    if (first_lr) *first_lr = atomic_load_explicit(&g_m17a_first_lr, memory_order_relaxed);
    if (last_lr) *last_lr = atomic_load_explicit(&g_m17a_last_lr, memory_order_relaxed);
    if (h_used) *h_used = atomic_load_explicit(&g_m17a_h_used, memory_order_relaxed);
    if (h_ovf) *h_ovf = atomic_load_explicit(&g_m17a_h_ovf, memory_order_relaxed);
    if (pend_ovf) *pend_ovf = atomic_load_explicit(&g_m17a_pend_ovf, memory_order_relaxed);
    if (max_per_frame) *max_per_frame = atomic_load_explicit(&g_m17a_max_per_frame, memory_order_relaxed);
    if (cur_sub) *cur_sub = atomic_load_explicit(&g_m17a_cur_sub, memory_order_relaxed);
    if (x0_last) *x0_last = atomic_load_explicit(&g_m17a_x0_last, memory_order_relaxed);
    if (x1_last) *x1_last = atomic_load_explicit(&g_m17a_x1_last, memory_order_relaxed);
    if (x2_last) *x2_last = atomic_load_explicit(&g_m17a_x2_last, memory_order_relaxed);
    if (x3_last) *x3_last = atomic_load_explicit(&g_m17a_x3_last, memory_order_relaxed);
    if (x0_first) *x0_first = atomic_load_explicit(&g_m17a_x0_first, memory_order_relaxed);
    if (sub_first) *sub_first = atomic_load_explicit(&g_m17a_sub_first, memory_order_relaxed);
    if (sub_last) *sub_last = atomic_load_explicit(&g_m17a_sub_last, memory_order_relaxed);
}
int smo_obs_m17a_drain(uint64_t *lr, uint64_t *x0, uint64_t *x1, uint64_t *x2, uint64_t *x3, int max)
{
    if (max <= 0) { atomic_store_explicit(&g_m17a_pend_n, 0, memory_order_relaxed); return 0; }
    int n = atomic_exchange_explicit(&g_m17a_pend_n, 0, memory_order_relaxed);
    int m = n < max ? n : max;
    for (int i = 0; i < m; i++) {
        if (lr) lr[i] = g_m17a_pend[i].lr;
        if (x0) x0[i] = g_m17a_pend[i].x0;
        if (x1) x1[i] = g_m17a_pend[i].x1;
        if (x2) x2[i] = g_m17a_pend[i].x2;
        if (x3) x3[i] = g_m17a_pend[i].x3;
    }
    return n;
}
/* ---- M18A（2026-11-08）：门 116/117（UI 颜色入参）的 dump 侧入口 ----
   ① 每门累计命中 + 溢出；② 末见载荷（x0/x1/x3/LR/sub）；③ 逐笔 drain：port 侧每笔 draw 领走
   自上一笔以来的命中，再由 port 侧安全读 [x3] / [x0+0xf0] / [[x?+0xb8]+0x658]。
   观察器里一律不解引用（stub 上下文不做带 setjmp 的安全读，理由见 M15A 注）。 */
uint64_t smo_obs_m18a_k(int gi)
{
    return (gi == 0 || gi == 1) ? atomic_load_explicit(&g_m18a_k[gi], memory_order_relaxed) : 0;
}
void smo_obs_m18a_meta(uint64_t *k116, uint64_t *k117, uint64_t *ovf116, uint64_t *ovf117,
                       uint64_t *x0_last, uint64_t *x1_last, uint64_t *x3_last, uint64_t *lr_last,
                       uint64_t *sub_first, uint64_t *sub_last)
{
    if (k116) *k116 = smo_obs_m18a_k(0);
    if (k117) *k117 = smo_obs_m18a_k(1);
    if (ovf116) *ovf116 = atomic_load_explicit(&g_m18a_ovf[0], memory_order_relaxed);
    if (ovf117) *ovf117 = atomic_load_explicit(&g_m18a_ovf[1], memory_order_relaxed);
    if (x0_last) *x0_last = atomic_load_explicit(&g_m18a_x0_last, memory_order_relaxed);
    if (x1_last) *x1_last = atomic_load_explicit(&g_m18a_x1_last, memory_order_relaxed);
    if (x3_last) *x3_last = atomic_load_explicit(&g_m18a_x3_last, memory_order_relaxed);
    if (lr_last) *lr_last = atomic_load_explicit(&g_m18a_lr_last, memory_order_relaxed);
    if (sub_first) *sub_first = atomic_load_explicit(&g_m18a_sub_first, memory_order_relaxed);
    if (sub_last) *sub_last = atomic_load_explicit(&g_m18a_sub_last, memory_order_relaxed);
}
int smo_obs_m18a_drain(uint32_t *gate, uint32_t *sub, uint64_t *lr, uint64_t *x0, uint64_t *x1,
                       uint64_t *x2, uint64_t *x3, int max)
{
    if (max <= 0) { atomic_store_explicit(&g_m18a_pend_n, 0, memory_order_relaxed); return 0; }
    int n = atomic_exchange_explicit(&g_m18a_pend_n, 0, memory_order_relaxed);
    int m = n < max ? n : max;
    for (int i = 0; i < m; i++) {
        if (gate) gate[i] = g_m18a_pend[i].gate;
        if (sub) sub[i] = g_m18a_pend[i].sub;
        if (lr) lr[i] = g_m18a_pend[i].lr;
        if (x0) x0[i] = g_m18a_pend[i].x0;
        if (x1) x1[i] = g_m18a_pend[i].x1;
        if (x2) x2[i] = g_m18a_pend[i].x2;
        if (x3) x3[i] = g_m18a_pend[i].x3;
    }
    return n;
}
/* ---- M19A（2026-11-08）：门 118（2D 批处理器 0x916a28）的 dump 侧入口 ----
   ① 累计命中/溢出；② 末见载荷（LR/ctx/state/x0/x3/sub）；③ 逐笔 drain：port 侧每笔 draw 领走
   自上一笔以来的命中，再由 port 侧安全读 st+0x700（RGB 源）、st+0x8b0..8b2（标志）、ctx+0x320、
   ctx+0xb8 → st → st+0x658 的 16 B 描述符 → 12 B 颜色缓冲区（**最终值**）。
   观察器里一律不解引用（理由同 M15A 注）。 */
uint64_t smo_obs_m19a_k(void)
{
    return atomic_load_explicit(&g_m19a_k, memory_order_relaxed);
}
void smo_obs_m19a_meta(uint64_t *k, uint64_t *ovf, uint64_t *lr_last, uint64_t *ctx_last,
                       uint64_t *st_last, uint64_t *x0_last, uint64_t *x3_last,
                       uint64_t *sub_first, uint64_t *sub_last)
{
    if (k) *k = atomic_load_explicit(&g_m19a_k, memory_order_relaxed);
    if (ovf) *ovf = atomic_load_explicit(&g_m19a_ovf, memory_order_relaxed);
    if (lr_last) *lr_last = atomic_load_explicit(&g_m19a_lr_last, memory_order_relaxed);
    if (ctx_last) *ctx_last = atomic_load_explicit(&g_m19a_ctx_last, memory_order_relaxed);
    if (st_last) *st_last = atomic_load_explicit(&g_m19a_st_last, memory_order_relaxed);
    if (x0_last) *x0_last = atomic_load_explicit(&g_m19a_x0_last, memory_order_relaxed);
    if (x3_last) *x3_last = atomic_load_explicit(&g_m19a_x3_last, memory_order_relaxed);
    if (sub_first) *sub_first = atomic_load_explicit(&g_m19a_sub_first, memory_order_relaxed);
    if (sub_last) *sub_last = atomic_load_explicit(&g_m19a_sub_last, memory_order_relaxed);
}
int smo_obs_m19a_drain(uint32_t *sub, uint64_t *lr, uint64_t *x0, uint64_t *x1,
                       uint64_t *x2, uint64_t *x3, uint64_t *x8, int max)
{
    if (max <= 0) { atomic_store_explicit(&g_m19a_pend_n, 0, memory_order_relaxed); return 0; }
    int n = atomic_exchange_explicit(&g_m19a_pend_n, 0, memory_order_relaxed);
    int m = n < max ? n : max;
    for (int i = 0; i < m; i++) {
        if (sub) sub[i] = g_m19a_pend[i].sub;
        if (lr) lr[i] = g_m19a_pend[i].lr;
        if (x0) x0[i] = g_m19a_pend[i].x0;
        if (x1) x1[i] = g_m19a_pend[i].x1;
        if (x2) x2[i] = g_m19a_pend[i].x2;
        if (x3) x3[i] = g_m19a_pend[i].x3;
        if (x8) x8[i] = g_m19a_pend[i].x8;
    }
    return n;
}
int smo_obs_m17a_hist(int N, uint64_t *lr, uint64_t *n_early, uint64_t *n_late)
{
    int have = 0;    for (int i = 0; i < N; i++) { lr[i] = 0; n_early[i] = 0; n_late[i] = 0; }
    for (unsigned i = 0; i < M17A_HIST_N; i++) {
        m17a_h_t *e = &g_m17a_h[i];
        if (!e->lr) continue;
        uint64_t tot = e->n_early + e->n_late;
        int pos = have < N ? have : -1;
        if (pos < 0) {
            if (tot <= (n_early[N - 1] + n_late[N - 1])) continue;
            pos = N - 1;
        }
        int j = pos;
        while (j > 0 && (n_early[j - 1] + n_late[j - 1]) < tot) {
            lr[j] = lr[j - 1]; n_early[j] = n_early[j - 1]; n_late[j] = n_late[j - 1];
            j--;
        }
        lr[j] = e->lr; n_early[j] = e->n_early; n_late[j] = e->n_late;
        if (have < N) have++;
    }
    return have;
}
/* M8C：门安装后的 stub 在 main 的偏移（0 = 未装）。dump 侧用它回读 30 字变体的注入字。 */
uint64_t smo_obs_stub_off_gate(int gate) { return gate >= 0 && gate < SMO_OBS_N ? g_obs_stub_off[gate] : 0; }
uint64_t smo_obs_last_x0(int gate) { return gate >= 0 && gate < SMO_OBS_N ? atomic_load_explicit(&g_obs_x0[gate], memory_order_relaxed) : 0; }
/* loader.c 的 check_permissions 用：尾部页里被本模块 mprotect 成 RX 的绝对地址区间
   （未安装时 hi=0）。check_permissions 对 ≥page_up(file_size) 的页缺省期待 RW，
   仅此区间期待 RX——权限巡检的「不期即报」语义保持不变。 */
void smo_obs_rx_range(uint64_t *lo, uint64_t *hi) { *lo = g_obs_rx_lo; *hi = g_obs_rx_hi; }
/* round21：e1_hist/e1_summary/dl_snapshot 随 E1/DRAIN-LOOP 门退役删除。
   round17 ①（dump 侧）：KICK1 三条件首/末见 + 三分支交叉分布。
   first_out={this,w1,f9,o20,tsns}（set=0 时全 0 返回 0）；last_out={w1,f9,o20}；
   dist_out={n_lt1, n_ge1_f9eq2, n_ge1_f9ne2, n_o20z}。 */
int smo_obs_k1_stats(uint64_t first_out[5], uint64_t last_out[3], uint64_t dist_out[4]) {
    extern uint64_t runtime_now_ns(void);
    if (!atomic_load_explicit(&g_k1.set, memory_order_relaxed)) return 0;
    first_out[0] = g_k1.this_; first_out[1] = g_k1.w1; first_out[2] = g_k1.f9; first_out[3] = g_k1.o20;
    int is_raw = 0; double npt = 0;
    if (g_obs_tick0_set) {
        uint64_t t1 = smo_obs_tick(), ns1 = runtime_now_ns();
        if (t1 > g_obs_tick0) npt = (double)(ns1 - g_obs_ns0) / (double)(t1 - g_obs_tick0);
        else is_raw = 1;
    } else is_raw = 1;
    first_out[4] = is_raw ? g_k1.tick : g_obs_ns0 + (uint64_t)((double)(g_k1.tick - g_obs_tick0) * npt + 0.5);
    last_out[0] = atomic_load_explicit(&g_k1.last_w1, memory_order_relaxed);
    last_out[1] = atomic_load_explicit(&g_k1.last_f9, memory_order_relaxed);
    last_out[2] = atomic_load_explicit(&g_k1.last_o20, memory_order_relaxed);
    dist_out[0] = atomic_load_explicit(&g_k1.n_lt1, memory_order_relaxed);
    dist_out[1] = atomic_load_explicit(&g_k1.n_ge1_f9eq2, memory_order_relaxed);
    dist_out[2] = atomic_load_explicit(&g_k1.n_ge1_f9ne2, memory_order_relaxed);
    dist_out[3] = atomic_load_explicit(&g_k1.n_o20z, memory_order_relaxed);
    return 1;
}
/* round17 ②（dump 侧；round18 补 x1，round21 补 LR）：BC310 首见 {x0, x1, tsns}
   （末见 x0=smo_obs_last_x0(SMO_OBS_BC310)；x1=门点原 x1=该函数第二实参）。
   round21 调用者读取走 smo_obs_bc_caller。 */
int smo_obs_bc_snapshot(uint64_t *x0, uint64_t *x1, uint64_t *ts_ns) {
    extern uint64_t runtime_now_ns(void);
    if (!atomic_load_explicit(&g_bc.set, memory_order_relaxed)) return 0;
    *x0 = g_bc.x0; *x1 = g_bc.x1;
    int is_raw = 0; double npt = 0;
    if (g_obs_tick0_set) {
        uint64_t t1 = smo_obs_tick(), ns1 = runtime_now_ns();
        if (t1 > g_obs_tick0) npt = (double)(ns1 - g_obs_ns0) / (double)(t1 - g_obs_tick0);
        else is_raw = 1;
    } else is_raw = 1;
    *ts_ns = is_raw ? g_bc.tick : g_obs_ns0 + (uint64_t)((double)(g_bc.tick - g_obs_tick0) * npt + 0.5);
    return 1;
}
/* round21（dump 侧）：BC310 调用者 LR 读数——first/last LR、末见 {x1, tsns}（逐命中覆盖，
   =45807 次后外层循环耗尽时的末次现场）、首 64 LR 采样环（lr_out 按命中序；*total=全部
   命中认领数，>64 表示环只含首 64）。blr 调用⇒LR=调用点+4、br 尾跳⇒LR=更上层返回地址，
   采样分布 dump 侧符号化（runtime_module_offset）。返回 0=门未命中。 */
int smo_obs_bc_caller(uint64_t *first_lr, uint64_t *last_lr, uint64_t *last_x1, uint64_t *last_ts, int *raw,
                      uint64_t lr_out[64], uint64_t *total) {
    extern uint64_t runtime_now_ns(void);
    if (!atomic_load_explicit(&g_obs_hits[SMO_OBS_BC310], memory_order_relaxed)) return 0;
    *first_lr = atomic_load_explicit(&g_bc.set, memory_order_relaxed) ? g_bc.lr0 : 0;
    *last_lr = atomic_load_explicit(&g_bc.last_lr, memory_order_relaxed);
    *last_x1 = atomic_load_explicit(&g_bc.last_x1, memory_order_relaxed);
    uint64_t k = atomic_load_explicit(&g_bc.lr_k, memory_order_relaxed);
    if (total) *total = k;
    unsigned n = k < 64 ? (unsigned)k : 64;
    for (unsigned i = 0; i < n; i++) lr_out[i] = atomic_load_explicit(&g_bc.lr[i], memory_order_relaxed);
    for (unsigned i = n; i < 64; i++) lr_out[i] = 0;
    int is_raw = 0; double npt = 0;
    if (g_obs_tick0_set) {
        uint64_t t1 = smo_obs_tick(), ns1 = runtime_now_ns();
        if (t1 > g_obs_tick0) npt = (double)(ns1 - g_obs_ns0) / (double)(t1 - g_obs_tick0);
        else is_raw = 1;
    } else is_raw = 1;
    if (raw) *raw = is_raw;
    uint64_t tk = atomic_load_explicit(&g_bc.last_tick, memory_order_relaxed);
    *last_ts = is_raw ? tk : g_obs_ns0 + (uint64_t)((double)(tk - g_obs_tick0) * npt + 0.5);
    return 1;
}
/* round19 ③（dump 侧）：BC310 first-16 唯一 x0 环。vals/cnts 各 16 槽（val==0 的槽
   未用）、over=16 槽满后溢出、n_zero=x0==0 计数。返回 1=门命中过（环/计数有效）。 */
int smo_obs_bc_x0ring(uint64_t vals[16], uint64_t cnts[16], uint64_t *over, uint64_t *n_zero) {
    if (!atomic_load_explicit(&g_obs_hits[SMO_OBS_BC310], memory_order_relaxed)) return 0;
    for (int i = 0; i < 16; i++) {
        vals[i] = atomic_load_explicit(&g_bc.xv[i], memory_order_relaxed);
        cnts[i] = atomic_load_explicit(&g_bc.xc[i], memory_order_relaxed);
    }
    *over = atomic_load_explicit(&g_bc.xover, memory_order_relaxed);
    *n_zero = atomic_load_explicit(&g_bc.xzero, memory_order_relaxed);
    return 1;
}
/* round18 ①（dump 侧）：KICK1 首 16 次逐次快照环 + 六边际计数。
   返回已记录条数 n（0=门未命中；>16 不会发生——ring_k 认领序号≥16 不写槽）；
   this_/w1/f9/o20 各 cap=16 槽按命中序填充；marg={w1==0,w1!=0,f9==2,f9!=2,o20==0,o20!=0}。 */
unsigned smo_obs_k1_hist16(uint64_t this_[16], uint64_t w1[16], uint64_t f9[16], uint64_t o20[16],
                           uint64_t marg[6], uint64_t *total) {
    uint64_t k = atomic_load_explicit(&g_k1.ring_k, memory_order_relaxed);
    if (total) *total = k;
    unsigned n = k < 16 ? (unsigned)k : 16;
    for (unsigned i = 0; i < n; i++) {
        this_[i] = g_k1.ring[i].this_; w1[i] = g_k1.ring[i].w1;
        f9[i] = g_k1.ring[i].f9; o20[i] = g_k1.ring[i].o20;
    }
    marg[0] = atomic_load_explicit(&g_k1.n_w1z, memory_order_relaxed);
    marg[1] = atomic_load_explicit(&g_k1.n_w1nz, memory_order_relaxed);
    marg[2] = atomic_load_explicit(&g_k1.n_f9eq2, memory_order_relaxed);
    marg[3] = atomic_load_explicit(&g_k1.n_f9ne2, memory_order_relaxed);
    marg[4] = atomic_load_explicit(&g_k1.n_o20z2, memory_order_relaxed);
    marg[5] = atomic_load_explicit(&g_k1.n_o20nz, memory_order_relaxed);
    return n;
}
/* round21（dump 侧）：kickbr_dist 随 KICKBR 门退役删除。
   round21（dump 侧）：三门首见 {x0, x1, tsns}。which：0=REG230（x0=槽/x1=被注册句柄）、
   1=FN_A 0x8b65e0（x0=时基参/x1=版本容器）、2=FN_B 0x8b6920（x0=容器本体/x1=第二实参）。
   ts 同 bc_snapshot 的 tick→ns 两点内插（raw=1 时给裸 tick）。返回 set（0=门未命中）。 */
int smo_obs_fs_snapshot(int which, uint64_t *x0, uint64_t *x1, uint64_t *ts_ns) {
    extern uint64_t runtime_now_ns(void);
    if (which < 0 || which > 2 || !atomic_load_explicit(&g_fs[which].set, memory_order_relaxed)) return 0;
    *x0 = g_fs[which].x0; *x1 = g_fs[which].x1;
    int is_raw = 0; double npt = 0;
    if (g_obs_tick0_set) {
        uint64_t t1 = smo_obs_tick(), ns1 = runtime_now_ns();
        if (t1 > g_obs_tick0) npt = (double)(ns1 - g_obs_ns0) / (double)(t1 - g_obs_tick0);
        else is_raw = 1;
    } else is_raw = 1;
    *ts_ns = is_raw ? g_fs[which].tick : g_obs_ns0 + (uint64_t)((double)(g_fs[which].tick - g_obs_tick0) * npt + 0.5);
    return 1;
}

/* round24（dump 侧）：TP 解引用首字快照。out={x0, deref, tsns}（tsns 换算同
   bc_snapshot：安装期 tick0/ns0 基准内插）；ok/bad=护栏内/外命中计数；last=末见值。
   返回 0=门未命中（首见未定格）。 */
int smo_obs_tp_snapshot(int k, uint64_t out[3], uint64_t *ok, uint64_t *bad, uint64_t *last) {
    extern uint64_t runtime_now_ns(void);
    if (k < 0 || k > 3 || !atomic_load_explicit(&g_tp[k].set, memory_order_relaxed)) return 0;
    out[0] = g_tp[k].x0; out[1] = g_tp[k].deref;
    int is_raw = 0; double npt = 0;
    if (g_obs_tick0_set) {
        uint64_t t1 = smo_obs_tick(), ns1 = runtime_now_ns();
        if (t1 > g_obs_tick0) npt = (double)(ns1 - g_obs_ns0) / (double)(t1 - g_obs_tick0);
        else is_raw = 1;
    } else is_raw = 1;
    out[2] = is_raw ? g_tp[k].tick : g_obs_ns0 + (uint64_t)((double)(g_tp[k].tick - g_obs_tick0) * npt + 0.5);
    *ok = atomic_load_explicit(&g_tp[k].n_ok, memory_order_relaxed);
    *bad = atomic_load_explicit(&g_tp[k].n_bad, memory_order_relaxed);
    *last = atomic_load_explicit(&g_tp[k].last_deref, memory_order_relaxed);
    return 1;
}

/* round25（dump 侧）：TP5 状态字序列。q: 0=TP4R(ret) 1=TP4A(adv)。out 填 {x0, state, val}
   序列；返回定格槽数 n（0=门未命中；8=只前 8 次；9=前 8 次+第 10000 次在槽 8）。 */
int smo_obs_tp5_seq(int q, uint64_t out[9][3]) {
    if (q < 0 || q > 1) return 0;
    uint64_t kk = atomic_load_explicit(&g_tp5[q].k, memory_order_relaxed);
    int n = kk < 8 ? (int)kk : 8 + (kk >= 10000 ? 1 : 0);
    for (int i = 0; i < n; i++) { out[i][0] = g_tp5[q].r[i].x0; out[i][1] = g_tp5[q].r[i].state; out[i][2] = g_tp5[q].r[i].val; }
    return n;
}

/* ---- M7w (dump 侧访问器)：tick→ns 用 round21 的两点插值（smo_obs_k1_stats 同款）。 ---- */
static uint64_t m7w_tick2ns(uint64_t tick) {
    extern uint64_t runtime_now_ns(void);
    if (!g_obs_tick0_set || tick < g_obs_tick0) return 0;
    uint64_t t1 = smo_obs_tick(), ns1 = runtime_now_ns();
    if (t1 <= g_obs_tick0) return 0;
    double npt = (double)(ns1 - g_obs_ns0) / (double)(t1 - g_obs_tick0);
    return g_obs_ns0 + (uint64_t)((double)(tick - g_obs_tick0) * npt + 0.5);
}
/* SENDTO_REJ：拒绝类分桶（all-time）+ 末 20 次环行（out[i]={ns,this,msg,state,last,lr}，
   *total=环认领总数，>20 表示环只含前 20）。 */
uint64_t smo_obs_m7w_rej(uint64_t out[4]) {
    out[0] = atomic_load_explicit(&g_m7w_rej.n_msg0, memory_order_relaxed);
    out[1] = atomic_load_explicit(&g_m7w_rej.n_state, memory_order_relaxed);
    out[2] = atomic_load_explicit(&g_m7w_rej.n_dedup, memory_order_relaxed);
    out[3] = atomic_load_explicit(&g_m7w_rej.n_odd, memory_order_relaxed);
    return atomic_load_explicit(&g_m7w_rej.k, memory_order_relaxed);
}
int smo_obs_m7w_rej_rows(uint64_t out[20][6], uint64_t *total) {
    uint64_t k = atomic_load_explicit(&g_m7w_rej.k, memory_order_relaxed);
    if (total) *total = k;
    unsigned n = k < 20 ? (unsigned)k : 20;
    for (unsigned i = 0; i < n; i++) {
        out[i][0] = m7w_tick2ns(g_m7w_rej.r[i].tick); out[i][1] = g_m7w_rej.r[i].this_;
        out[i][2] = g_m7w_rej.r[i].msg; out[i][3] = g_m7w_rej.r[i].state;
        out[i][4] = g_m7w_rej.r[i].last; out[i][5] = g_m7w_rej.r[i].lr;
    }
    return (int)n;
}
/* TP4A 放行账：计数 + 末 20 次环行（out[i]={ns,this,msg,state,lr}）。 */
uint64_t smo_obs_m7w_pass_n(void) { return atomic_load_explicit(&g_m7w_pass.n, memory_order_relaxed); }
int smo_obs_m7w_pass_rows(uint64_t out[20][5], uint64_t *total) {
    uint64_t k = atomic_load_explicit(&g_m7w_pass.k, memory_order_relaxed);
    if (total) *total = k;
    unsigned n = k < 20 ? (unsigned)k : 20;
    for (unsigned i = 0; i < n; i++) {
        out[i][0] = m7w_tick2ns(g_m7w_pass.r[i].tick); out[i][1] = g_m7w_pass.r[i].this_;
        out[i][2] = g_m7w_pass.r[i].msg; out[i][3] = g_m7w_pass.r[i].state; out[i][4] = g_m7w_pass.r[i].lr;
    }
    return (int)n;
}
/* BRC：per-call 环（out[i]={ns,this,lr}，*total=认领总数，>200 表示环只含前 200）。 */
int smo_obs_m7w_brc_rows(uint64_t out[200][3], uint64_t *total) {
    uint64_t k = atomic_load_explicit(&g_m7w_brc.k, memory_order_relaxed);
    if (total) *total = k;
    unsigned n = k < 200 ? (unsigned)k : 200;
    for (unsigned i = 0; i < n; i++) {
        out[i][0] = m7w_tick2ns(g_m7w_brc.r[i].tick); out[i][1] = g_m7w_brc.r[i].this_; out[i][2] = g_m7w_brc.r[i].lr;
    }
    return (int)n;
}
/* M7a: fnB completion ledger (M7b fix: LAST-16 overwrite ring; row 12 words
   out[i]={ns,vt,state,result,k0..k5(key name @slot+0x08),d0,d1}) + NULL/non-NULL totals.
   *total = the true completion count; rows = the last min(total,16) completions. */
int smo_obs_m7a_null_rows(uint64_t out[16][12], uint64_t *total, uint64_t *n_null, uint64_t *n_res) {
    uint64_t k = atomic_load_explicit(&g_m7a_null.k, memory_order_relaxed);
    if (total) *total = k;
    if (n_null) *n_null = atomic_load_explicit(&g_m7a_null.n_null, memory_order_relaxed);
    if (n_res) *n_res = atomic_load_explicit(&g_m7a_null.n_res, memory_order_relaxed);
    unsigned n = k < 16 ? (unsigned)k : 16;
    for (unsigned i = 0; i < n; i++) {
        out[i][0] = m7w_tick2ns(g_m7a_null.r[i].tick); out[i][1] = g_m7a_null.r[i].vt;
        out[i][2] = g_m7a_null.r[i].state; out[i][3] = g_m7a_null.r[i].result;
        out[i][4] = g_m7a_null.r[i].k0; out[i][5] = g_m7a_null.r[i].k1;
        out[i][6] = g_m7a_null.r[i].k2; out[i][7] = g_m7a_null.r[i].k3;
        out[i][8] = g_m7a_null.r[i].k4; out[i][9] = g_m7a_null.r[i].k5;
        out[i][10] = g_m7a_null.r[i].d0; out[i][11] = g_m7a_null.r[i].d1;
    }
    return (int)n;
}
/* M7a: latched registry vector addresses (returns the count, <=4). */
int smo_obs_m7a_vec(uint64_t out[4]) {
    unsigned n = atomic_load_explicit(&g_m7a_reg.n, memory_order_relaxed);
    for (unsigned i = 0; i < n && i < 4; i++) out[i] = g_m7a_reg.vec[i];
    return (int)(n > 4 ? 4 : n);
}
/* M7b: poster entry ledger. lr[] = first-32 caller LRs (absolute guest addresses, raw),
   *total = call count, first/last = {ns, x0 batch obj, [x0+0x18] flag, [x0+8] pump, LR}
   (first[0]==0 => no call yet). Returns ring rows (<=32). */
int smo_obs_m7b_br(uint64_t lr[32], uint64_t *total, uint64_t first[5], uint64_t last[5]) {
    uint64_t k = atomic_load_explicit(&g_m7b_br.k, memory_order_relaxed);
    if (total) *total = k;
    unsigned n = k < 32 ? (unsigned)k : 32;
    for (unsigned i = 0; i < n; i++) lr[i] = atomic_load_explicit(&g_m7b_br.lr[i], memory_order_relaxed);
    if (first) {
        if (atomic_load_explicit(&g_m7b_br.fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g_m7b_br.f_tick); first[1] = g_m7b_br.f_x0;
            first[2] = g_m7b_br.f_flag; first[3] = g_m7b_br.f_pump; first[4] = g_m7b_br.f_lr;
        } else first[0] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g_m7b_br.l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g_m7b_br.l_x0, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g_m7b_br.l_flag, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g_m7b_br.l_pump, memory_order_relaxed);
        last[4] = atomic_load_explicit(&g_m7b_br.l_lr, memory_order_relaxed);
    }
    return (int)n;
}
/* M7b: per-site poster call counters, per[i] = {count, first_ns, last_ns, last_x0 batch obj}. */
int smo_obs_m7b_c13(uint64_t per[13][4]) {
    for (int i = 0; i < 13; i++) {
        per[i][0] = atomic_load_explicit(&g_m7b_c[i].k, memory_order_relaxed);
        per[i][1] = m7w_tick2ns(atomic_load_explicit(&g_m7b_c[i].f_tick, memory_order_relaxed));
        per[i][2] = m7w_tick2ns(atomic_load_explicit(&g_m7b_c[i].l_tick, memory_order_relaxed));
        per[i][3] = atomic_load_explicit(&g_m7b_c[i].l_x0, memory_order_relaxed);
    }
    return 13;
}
/* M7b: fnA gate branch counters, per[0] = TRUE (sync 0x61570c), per[1] = FALSE (submit
   0x6157f0); per[i] = {count, first_ns, last_ns} (first_ns==0 => no hit yet). */
int smo_obs_m7b_g56(uint64_t per[2][3]) {
    for (int i = 0; i < 2; i++) {
        per[i][0] = atomic_load_explicit(&g_m7b_g[i].k, memory_order_relaxed);
        per[i][1] = m7w_tick2ns(atomic_load_explicit(&g_m7b_g[i].f_tick, memory_order_relaxed));
        per[i][2] = m7w_tick2ns(atomic_load_explicit(&g_m7b_g[i].l_tick, memory_order_relaxed));
    }
    return 2;
}
/* M7b: translate a poster-entry LR (main-relative offset) to the tail=3-gated bl site it
   came through: the stub replays the bl at stub+0x50, so the callee's LR = stub+0x54.
   Returns the site's main-relative offset (the bl itself), or 0 when not a gated return. */
uint64_t smo_obs_m7b_lr_site(uint64_t lr_off) {
    if (!lr_off) return 0;
    for (size_t s = 0; s < sizeof smo_obs_site / sizeof *smo_obs_site; s++) {
        const smo_obs_site_t *site = &smo_obs_site[s];
        if (site->tail != 3) continue;
        uint64_t so = g_obs_stub_off[site->gate];
        if (so && lr_off >= so + 0x50 && lr_off < so + 0x6c) return site->off;
    }
    return 0;
}
/* main 模块基址（W2 普查把 main+0x1369a20 换算成绝对地址用；未安装时 0）。 */
uint64_t smo_obs_main_base(void) { return g_obs_main_base; }
/* M7c: fnA entry ledger. rows[16][4] = last-16 overwrite ring {ns, thread, x1 name arg, LR};
   *total = entry count; thr[8][2] = per-thread {handle, count} (handle 0 slots skipped);
   *ovf = overflow (threads beyond 8 slots) + ring overwrite watermark; first[1] = {first_ns,
   first_set}; last[4] = {last_ns, last_thr, last_x1, last_lr}. Returns ring rows (<=16). */
int smo_obs_m7c_fna(uint64_t rows[16][4], uint64_t *total, uint64_t thr[8][2],
                    uint64_t *ovf, uint64_t first[2], uint64_t last[4]) {
    uint64_t k = atomic_load_explicit(&g_m7c_fna.k, memory_order_relaxed);
    if (total) *total = k;
    unsigned n = k < 16 ? (unsigned)k : 16;
    for (unsigned i = 0; i < n; i++) {
        rows[i][0] = m7w_tick2ns(g_m7c_fna.r[i].tick);
        rows[i][1] = g_m7c_fna.r[i].thr;
        rows[i][2] = g_m7c_fna.r[i].x1;
        rows[i][3] = g_m7c_fna.r[i].lr;
    }
    int nt = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t h = g_m7c_fna.pt[i].thr;
        if (h) { thr[nt][0] = h; thr[nt][1] = atomic_load_explicit(&g_m7c_fna.pt[i].n, memory_order_relaxed); nt++; }
    }
    for (int i = nt; i < 8; i++) { thr[i][0] = 0; thr[i][1] = 0; }
    if (ovf) *ovf = atomic_load_explicit(&g_m7c_fna.pt_ovf, memory_order_relaxed);
    if (first) {
        if (atomic_load_explicit(&g_m7c_fna.fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g_m7c_fna.f_tick); first[1] = 1;
        } else first[0] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g_m7c_fna.l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g_m7c_fna.l_thr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g_m7c_fna.l_x1, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g_m7c_fna.l_lr, memory_order_relaxed);
    }
    return (int)n;
}
/* M7e: create-request entry ledger. rows[32][4] = LAST-32 overwrite ring {ns, thr, x0, lr};
   *nrows = valid ring rows (<=32); *total = create count; thr[8][2] = per-thread {handle,
   count}; *ptovf = thread-slot overflow; first[4] = {ns, x0, lr, thr} (first[0]==0 => none);
   last[4] = {ns, thr, x0, lr}; lrd[8][2] = distinct LR {lr, count}; *lrovf = LR-slot overflow.
   Returns the distinct-LR count. */
int smo_obs_m7e_crt(uint64_t rows[32][4], int *nrows, uint64_t *total, uint64_t thr[8][2],
                    uint64_t *ptovf, uint64_t first[4], uint64_t last[4], uint64_t lrd[8][2],
                    uint64_t *lrovf) {
    uint64_t k = atomic_load_explicit(&g_m7e_crt.k, memory_order_relaxed);
    if (total) *total = k;
    unsigned n = k < 32 ? (unsigned)k : 32;
    if (nrows) *nrows = (int)n;
    /* chronological order: once the ring wrapped, the slot about to be overwritten is the oldest */
    unsigned start = k > 32 ? (unsigned)(k & 31u) : 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned j = (start + i) & 31u;
        rows[i][0] = m7w_tick2ns(g_m7e_crt.r[j].tick);
        rows[i][1] = g_m7e_crt.r[j].thr;
        rows[i][2] = g_m7e_crt.r[j].x0;
        rows[i][3] = g_m7e_crt.r[j].lr;
    }
    int nt = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t h = g_m7e_crt.pt[i].thr;
        if (h) { thr[nt][0] = h; thr[nt][1] = atomic_load_explicit(&g_m7e_crt.pt[i].n, memory_order_relaxed); nt++; }
    }
    for (int i = nt; i < 8; i++) { thr[i][0] = 0; thr[i][1] = 0; }
    if (ptovf) *ptovf = atomic_load_explicit(&g_m7e_crt.pt_ovf, memory_order_relaxed);
    if (first) {
        if (atomic_load_explicit(&g_m7e_crt.fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g_m7e_crt.f_tick);
            first[1] = g_m7e_crt.f_x0; first[2] = g_m7e_crt.f_lr; first[3] = g_m7e_crt.f_thr;
        } else { first[0] = 0; first[1] = first[2] = first[3] = 0; }
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g_m7e_crt.l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g_m7e_crt.l_thr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g_m7e_crt.l_x0, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g_m7e_crt.l_lr, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t v = g_m7e_crt.lr[i].lr;
        if (v) { lrd[nl][0] = v; lrd[nl][1] = atomic_load_explicit(&g_m7e_crt.lr[i].n, memory_order_relaxed); nl++; }
    }
    for (int i = nl; i < 8; i++) { lrd[i][0] = 0; lrd[i][1] = 0; }
    if (lrovf) *lrovf = atomic_load_explicit(&g_m7e_crt.lr_ovf, memory_order_relaxed);
    return nl;
}
/* M7g (dump side): generic ledger accessor, gi = 0 master(0x7488f0) / 1 entry-A(0x7485a8) /
   2 entry-B(0x748fb0) / 3 sibling(0x7461a0). rows[32][4] = LAST-32 overwrite ring
   {ns, thr, x0, lr}; *nrows = valid rows; *total = gate count; thr[8][2] = per-thread
   {handle, count}; *ptovf = thread-slot overflow; first[4] = {ns, x0, x1, thr} (0 => none);
   last[5] = {ns, thr, x0, x1, lr}; lrd[8][2] = distinct LR {lr, count}; *lrovf = LR-slot
   overflow. Returns the distinct-LR count. */
int smo_obs_m7g(unsigned gi, uint64_t rows[32][4], int *nrows, uint64_t *total,
                uint64_t thr[8][2], uint64_t *ptovf, uint64_t first[4], uint64_t last[5],
                uint64_t lrd[8][2], uint64_t *lrovf) {
    if (gi >= M7G_NG) return 0;
    m7g_g_t *g = &g_m7g[gi];
    uint64_t k = atomic_load_explicit(&g->k, memory_order_relaxed);
    if (total) *total = k;
    unsigned n = k < 32 ? (unsigned)k : 32;
    if (nrows) *nrows = (int)n;
    unsigned start = k > 32 ? (unsigned)(k & 31u) : 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned j = (start + i) & 31u;
        rows[i][0] = m7w_tick2ns(g->r[j].tick);
        rows[i][1] = g->r[j].thr;
        rows[i][2] = g->r[j].x0;
        rows[i][3] = g->r[j].lr;
    }
    int nt = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t h = g->pt[i].thr;
        if (h) { thr[nt][0] = h; thr[nt][1] = atomic_load_explicit(&g->pt[i].n, memory_order_relaxed); nt++; }
    }
    for (int i = nt; i < 8; i++) { thr[i][0] = 0; thr[i][1] = 0; }
    if (ptovf) *ptovf = atomic_load_explicit(&g->pt_ovf, memory_order_relaxed);
    if (first) {
        if (atomic_load_explicit(&g->fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g->f_tick);
            first[1] = g->f_x0; first[2] = g->f_x1; first[3] = g->f_thr;
        } else { first[0] = first[1] = first[2] = first[3] = 0; }
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g->l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g->l_thr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g->l_x0, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g->l_x1, memory_order_relaxed);
        last[4] = atomic_load_explicit(&g->l_lr, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t v = g->lr[i].lr;
        if (v) { lrd[nl][0] = v; lrd[nl][1] = atomic_load_explicit(&g->lr[i].n, memory_order_relaxed); nl++; }
    }
    for (int i = nl; i < 8; i++) { lrd[i][0] = 0; lrd[i][1] = 0; }
    if (lrovf) *lrovf = atomic_load_explicit(&g->lr_ovf, memory_order_relaxed);
    return nl;
}

/* M7h dump 侧：拷一门（定长、无锁读；读到的半行 = 观测语义，disclosed）。
   rows[][0]=ns（已用 tick0 内插换回）、[1]=thread、[2]=LR、[3]=x0。 */
int smo_obs_m7h(unsigned gi, uint64_t rows[16][4], int *nrows, uint64_t *total,
                uint64_t thr[8][2], uint64_t *ptovf, uint64_t first[5], uint64_t last[5],
                uint64_t lrd[16][3], uint64_t *lrovf) {
    if (gi >= M7H_NG) return 0;
    m7h_g_t *g = &g_m7h[gi];
    uint64_t k = atomic_load_explicit(&g->k, memory_order_relaxed);
    if (total) *total = k;
    unsigned n = k < 16 ? (unsigned)k : 16;
    if (nrows) *nrows = (int)n;
    unsigned start = k > 16 ? (unsigned)(k & 15u) : 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned j = (start + i) & 15u;
        rows[i][0] = m7w_tick2ns(g->r[j].tick);
        rows[i][1] = g->r[j].thr;
        rows[i][2] = g->r[j].lr;
        rows[i][3] = g->r[j].x0;
    }
    for (unsigned i = n; i < 16; i++) { rows[i][0] = rows[i][1] = rows[i][2] = rows[i][3] = 0; }
    int nt = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t h = g->pt[i].thr;
        if (h) { thr[nt][0] = h; thr[nt][1] = atomic_load_explicit(&g->pt[i].n, memory_order_relaxed); nt++; }
    }
    for (int i = nt; i < 8; i++) { thr[i][0] = 0; thr[i][1] = 0; }
    if (ptovf) *ptovf = atomic_load_explicit(&g->pt_ovf, memory_order_relaxed);
    if (first) {
        uint64_t ft = atomic_load_explicit(&g->f_tick, memory_order_relaxed);
        if (ft) { first[0] = m7w_tick2ns(ft); first[1] = g->f_x0; first[2] = g->f_x1; first[3] = 0; first[4] = 0; }
        else { for (int i = 0; i < 5; i++) first[i] = 0; }
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g->l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g->l_thr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g->l_x0, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g->l_x1, memory_order_relaxed);
        last[4] = atomic_load_explicit(&g->l_lr, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < 16; i++) {
        uint64_t v = g->lr[i].lr;
        if (v) {
            lrd[nl][0] = v;
            lrd[nl][1] = atomic_load_explicit(&g->lr[i].n, memory_order_relaxed);
            lrd[nl][2] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].l_tick, memory_order_relaxed));
            nl++;
        }
    }
    for (int i = nl; i < 16; i++) { lrd[i][0] = lrd[i][1] = lrd[i][2] = 0; }
    if (lrovf) *lrovf = atomic_load_explicit(&g->lr_ovf, memory_order_relaxed);
    return nl;
}

/* M7N dump 侧：拷一门（定长、无锁读；读到半行 = 观测语义，disclosed）。tick 统一用
   m7w_tick2ns 换回 ns（与 [m7h2]/[m7w] 同口径，可直接和 [lp]/[st] 的 guest tick 对齐）。
     rows[i] = {ns, thr, x0(object), x1(门点原 x1), tok([obj+0xc8]), lr, state([obj+0xd4]), aux}
       aux: G1=ctx->0x88 编号 / G2=0
     lrd[i]  = {lr, n, last_ns}     按角色键控的"谁调它"分布（top16 distinct LR）
     wpd[i]  = {w, n, first_ns, first_lr}  按角色键控的"哪个对象"台账（top16 distinct 对象）
     first[] = {ns, x0, x1, aux, tok, lr, thr_state_packed}（set=0 时全 0）
     last[]  = {ns, x0, lr, thr}
     ovf[]   = {r_ovf, lr_ovf, wp_ovf, pt_ovf} */
int smo_obs_m7n(unsigned gi, uint64_t *total, uint64_t first[7], uint64_t last[4],
                uint64_t rows[][8], int *nrows, uint64_t lrd[][3], int *nlr,
                uint64_t wpd[][4], int *nwp, uint64_t thr[][2], int *nthr, uint64_t ovf[4]) {
    if (gi >= M7N_NG) return 0;
    m7n_g_t *g = &g_m7n[gi];
    uint64_t k = atomic_load_explicit(&g->k, memory_order_relaxed);
    if (total) *total = k;
    unsigned n = k < M7N_ROW ? (unsigned)k : (unsigned)M7N_ROW;
    if (nrows) *nrows = (int)n;
    for (unsigned i = 0; i < n; i++) {
        rows[i][0] = m7w_tick2ns(g->r[i].tick);
        rows[i][1] = g->r[i].thr;
        rows[i][2] = g->r[i].x0;
        rows[i][3] = g->r[i].x1;
        rows[i][4] = g->r[i].tok;
        rows[i][5] = g->r[i].lr;
        rows[i][6] = g->r[i].state;
        rows[i][7] = g->r[i].aux;
    }
    for (unsigned i = n; i < M7N_ROW; i++)
        for (int j = 0; j < 8; j++) rows[i][j] = 0;
    if (first) {
        if (atomic_load_explicit(&g->fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g->f_tick);
            first[1] = g->f_x0; first[2] = g->f_x1; first[3] = g->f_aux; first[4] = g->f_tok;
            first[5] = g->f_lr;
            first[6] = ((uint64_t)g->f_state << 32) | g->f_thr;
        } else for (int i = 0; i < 7; i++) first[i] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g->l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g->l_x0, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g->l_lr, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g->l_thr, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < M7N_LR; i++) {
        uint64_t v = g->lr[i].lr;
        if (v) {
            lrd[nl][0] = v;
            lrd[nl][1] = atomic_load_explicit(&g->lr[i].n, memory_order_relaxed);
            lrd[nl][2] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].l_tick, memory_order_relaxed));
            nl++;
        }
    }
    for (int i = nl; i < M7N_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = 0;
    if (nlr) *nlr = nl;
    int nw = 0;
    for (int i = 0; i < M7N_WP; i++) {
        uint64_t v = g->wp[i].w;
        if (v) {
            wpd[nw][0] = v;
            wpd[nw][1] = atomic_load_explicit(&g->wp[i].n, memory_order_relaxed);
            wpd[nw][2] = m7w_tick2ns(atomic_load_explicit(&g->wp[i].f_tick, memory_order_relaxed));
            wpd[nw][3] = atomic_load_explicit(&g->wp[i].f_lr, memory_order_relaxed);
            nw++;
        }
    }
    for (int i = nw; i < M7N_WP; i++) wpd[i][0] = wpd[i][1] = wpd[i][2] = wpd[i][3] = 0;
    if (nwp) *nwp = nw;
    int nt = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t h = g->pt[i].thr;
        if (h) { thr[nt][0] = h; thr[nt][1] = atomic_load_explicit(&g->pt[i].n, memory_order_relaxed); nt++; }
    }
    for (int i = nt; i < 8; i++) thr[i][0] = thr[i][1] = 0;
    if (nthr) *nthr = nt;
    if (ovf) {
        ovf[0] = atomic_load_explicit(&g->r_ovf, memory_order_relaxed);
        ovf[1] = atomic_load_explicit(&g->lr_ovf, memory_order_relaxed);
        ovf[2] = atomic_load_explicit(&g->wp_ovf, memory_order_relaxed);
        ovf[3] = atomic_load_explicit(&g->pt_ovf, memory_order_relaxed);
    }
    return nt;
}

/* M7P dump 侧：拷 worker Start 门（定长、无锁读；读到半行 = 观测语义，disclosed）。
   tick 统一 m7w_tick2ns 换回 ns（与 [m7h2]/[m7w] 同口径）。
     rows[i] = {ns, thr, lr, x0(obj), ep(epoch), tok, state}
     lrd[i]  = {lr, n, first_ns, last_ns}          **每站计数表**（site = lr − 4）
     obd[i]  = {obj, n, first_ns, first_lr, ep}    **哪个对象由哪一站建出**
     first[] = {ns, x0, lr, ep, state, thr}
     last[]  = {ns, x0, lr, thr}
     ovf[]   = {r_ovf, lr_ovf, ob_ovf, pt_ovf} */
int smo_obs_m7p(uint64_t *total, uint64_t first[6], uint64_t last[4],
                uint64_t rows[][7], int *nrows, uint64_t lrd[][4], int *nlr,
                uint64_t obd[][5], int *nob, uint64_t thr[][2], int *nthr, uint64_t ovf[4]) {
    m7p_g_t *g = &g_m7p[0];
    uint64_t k = atomic_load_explicit(&g->k, memory_order_relaxed);
    if (total) *total = k;
    unsigned n = k < M7P_ROW ? (unsigned)k : (unsigned)M7P_ROW;
    if (nrows) *nrows = (int)n;
    for (unsigned i = 0; i < n; i++) {
        rows[i][0] = m7w_tick2ns(g->r[i].tick);
        rows[i][1] = g->r[i].thr;
        rows[i][2] = g->r[i].lr;
        rows[i][3] = g->r[i].x0;
        rows[i][4] = g->r[i].ep;
        rows[i][5] = g->r[i].tok;
        rows[i][6] = g->r[i].state;
    }
    for (unsigned i = n; i < M7P_ROW; i++)
        for (int j = 0; j < 7; j++) rows[i][j] = 0;
    if (first) {
        if (atomic_load_explicit(&g->fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g->f_tick);
            first[1] = g->f_x0; first[2] = g->f_lr; first[3] = g->f_ep;
            first[4] = g->f_state; first[5] = g->f_thr;
        } else for (int i = 0; i < 6; i++) first[i] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g->l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g->l_x0, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g->l_lr, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g->l_thr, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < M7P_LR; i++) {
        uint64_t v = g->lr[i].lr;
        if (v) {
            lrd[nl][0] = v;
            lrd[nl][1] = atomic_load_explicit(&g->lr[i].n, memory_order_relaxed);
            lrd[nl][2] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].f_tick, memory_order_relaxed));
            lrd[nl][3] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].l_tick, memory_order_relaxed));
            nl++;
        }
    }
    for (int i = nl; i < M7P_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = lrd[i][3] = 0;
    if (nlr) *nlr = nl;
    int no = 0;
    for (int i = 0; i < M7P_OBJ; i++) {
        uint64_t v = g->ob[i].w;
        if (v) {
            obd[no][0] = v;
            obd[no][1] = atomic_load_explicit(&g->ob[i].n, memory_order_relaxed);
            obd[no][2] = m7w_tick2ns(atomic_load_explicit(&g->ob[i].f_tick, memory_order_relaxed));
            obd[no][3] = atomic_load_explicit(&g->ob[i].f_lr, memory_order_relaxed);
            obd[no][4] = atomic_load_explicit(&g->ob[i].f_ep, memory_order_relaxed);
            no++;
        }
    }
    for (int i = no; i < M7P_OBJ; i++) obd[i][0] = obd[i][1] = obd[i][2] = obd[i][3] = obd[i][4] = 0;
    if (nob) *nob = no;
    int nt = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t h = g->pt[i].thr;
        if (h) { thr[nt][0] = h; thr[nt][1] = atomic_load_explicit(&g->pt[i].n, memory_order_relaxed); nt++; }
    }
    for (int i = nt; i < 8; i++) thr[i][0] = thr[i][1] = 0;
    if (nthr) *nthr = nt;
    if (ovf) {
        ovf[0] = atomic_load_explicit(&g->r_ovf, memory_order_relaxed);
        ovf[1] = atomic_load_explicit(&g->lr_ovf, memory_order_relaxed);
        ovf[2] = atomic_load_explicit(&g->ob_ovf, memory_order_relaxed);
        ovf[3] = atomic_load_explicit(&g->pt_ovf, memory_order_relaxed);
    }
    return nt;
}

/* M7Q dump 侧：拷三层门（定长、无锁读；读到半行 = 观测语义，disclosed）。
   gi = 0 WRAP(0x964540) / 1 FO(0x65b468) / 2 FOB(0x65b470)。
     rows[i] = {ns, thr, lr, x0, x1, x8}
     lrd[i]  = {lr, n, first_ns, last_ns}          每站计数表（WRAP: site = lr − 4）
     vd[i]   = {value, n, first_ns}                FO/FOB 的 x1 = 扇出数 w1 的取值分布
     first[] = {ns, lr, x0, x1, x8, thr}
     last[]  = {ns, lr, x0, x1, thr}
     ovf[]   = {r_ovf, lr_ovf, v_ovf} */
int smo_obs_m7q(int gi, uint64_t *total, uint64_t rows[][6], int *nrows,
                uint64_t lrd[][4], int *nlr, uint64_t vd[][3], int *nv,
                uint64_t first[6], uint64_t last[5], uint64_t ovf[3]) {
    if (gi < 0 || gi >= M7Q_NG) return 0;
    m7q_g_t *g = &g_m7q[gi];
    uint64_t k = atomic_load_explicit(&g->k, memory_order_relaxed);
    if (total) *total = k;
    unsigned n = k < M7Q_ROW ? (unsigned)k : (unsigned)M7Q_ROW;
    if (nrows) *nrows = (int)n;
    for (unsigned i = 0; i < n; i++) {
        rows[i][0] = m7w_tick2ns(g->r[i].tick);
        rows[i][1] = g->r[i].thr;
        rows[i][2] = g->r[i].lr;
        rows[i][3] = g->r[i].x0;
        rows[i][4] = g->r[i].x1;
        rows[i][5] = g->r[i].x8;
    }
    for (unsigned i = n; i < M7Q_ROW; i++)
        for (int j = 0; j < 6; j++) rows[i][j] = 0;
    if (first) {
        if (atomic_load_explicit(&g->fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g->f_tick);
            first[1] = g->f_lr; first[2] = g->f_x0; first[3] = g->f_x1;
            first[4] = g->f_x8; first[5] = g->f_thr;
        } else for (int i = 0; i < 6; i++) first[i] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g->l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g->l_lr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g->l_x0, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g->l_x1, memory_order_relaxed);
        last[4] = atomic_load_explicit(&g->l_tick, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < M7Q_LR; i++) {
        uint64_t v = g->lr[i].lr;
        if (v) {
            lrd[nl][0] = v;
            lrd[nl][1] = atomic_load_explicit(&g->lr[i].n, memory_order_relaxed);
            lrd[nl][2] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].f_tick, memory_order_relaxed));
            lrd[nl][3] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].l_tick, memory_order_relaxed));
            nl++;
        }
    }
    for (int i = nl; i < M7Q_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = lrd[i][3] = 0;
    if (nlr) *nlr = nl;
    int nvv = 0;
    for (int i = 0; i < M7Q_VAL; i++) {
        uint64_t v = g->v[i].v;
        if (v) {
            vd[nvv][0] = v;
            vd[nvv][1] = atomic_load_explicit(&g->v[i].n, memory_order_relaxed);
            vd[nvv][2] = m7w_tick2ns(atomic_load_explicit(&g->v[i].f_tick, memory_order_relaxed));
            nvv++;
        }
    }
    for (int i = nvv; i < M7Q_VAL; i++) vd[i][0] = vd[i][1] = vd[i][2] = 0;
    if (nv) *nv = nvv;
    if (ovf) {
        ovf[0] = atomic_load_explicit(&g->r_ovf, memory_order_relaxed);
        ovf[1] = atomic_load_explicit(&g->lr_ovf, memory_order_relaxed);
        ovf[2] = atomic_load_explicit(&g->v_ovf, memory_order_relaxed);
    }
    return nvv;
}

/* M7S2 dump 侧：拷三只门（定长、无锁读；读到半行 = 观测语义，disclosed）。
   gi = 0 G1(0x681b30) / 1 G2(0x74ca00) / 2 G3(0x74cadc)。
     rows[i] = {ns, thr, lr, x0, x1, x8}          G3 恒为空（只计数 + LR）
     lrd[i]  = {lr, n, first_ns, last_ns}         每站点计数表（site = lr − 4）
     first[] = {ns, lr, x0, x1, x8, thr}
     last[]  = {ns, lr, x0, x1}
     ovf[]   = {r_ovf, lr_ovf} */
int smo_obs_m7s2(int gi, uint64_t *total, uint64_t rows[][6], int *nrows,
                 uint64_t lrd[][4], int *nlr, uint64_t first[6], uint64_t last[4], uint64_t ovf[2]) {
    if (gi < 0 || gi >= M7S2_NG) return 0;
    m7s2_g_t *g = &g_m7s2[gi];
    uint64_t k = atomic_load_explicit(&g->k, memory_order_relaxed);
    if (total) *total = k;
    unsigned n = 0;
    if (gi != 2) {
        n = k < M7S2_ROW ? (unsigned)k : (unsigned)M7S2_ROW;
        if (n > (unsigned)atomic_load_explicit(&g->rk, memory_order_relaxed))
            n = (unsigned)atomic_load_explicit(&g->rk, memory_order_relaxed);
        if (n > M7S2_ROW) n = M7S2_ROW;
    }
    if (nrows) *nrows = (int)n;
    for (unsigned i = 0; i < n; i++) {
        rows[i][0] = m7w_tick2ns(g->r[i].tick);
        rows[i][1] = g->r[i].thr;
        rows[i][2] = g->r[i].lr;
        rows[i][3] = g->r[i].x0;
        rows[i][4] = g->r[i].x1;
        rows[i][5] = g->r[i].x8;
    }
    for (unsigned i = n; i < M7S2_ROW; i++)
        for (int j = 0; j < 6; j++) rows[i][j] = 0;
    if (first) {
        if (atomic_load_explicit(&g->fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g->f_tick);
            first[1] = g->f_lr; first[2] = g->f_x0; first[3] = g->f_x1;
            first[4] = g->f_x8; first[5] = g->f_thr;
        } else for (int i = 0; i < 6; i++) first[i] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g->l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g->l_lr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g->l_x0, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g->l_x1, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < M7S2_LR; i++) {
        uint64_t v = g->lr[i].lr;
        if (v) {
            lrd[nl][0] = v;
            lrd[nl][1] = atomic_load_explicit(&g->lr[i].n, memory_order_relaxed);
            lrd[nl][2] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].f_tick, memory_order_relaxed));
            lrd[nl][3] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].l_tick, memory_order_relaxed));
            nl++;
        }
    }
    for (int i = nl; i < M7S2_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = lrd[i][3] = 0;
    if (nlr) *nlr = nl;
    if (ovf) {
        ovf[0] = atomic_load_explicit(&g->r_ovf, memory_order_relaxed);
        ovf[1] = atomic_load_explicit(&g->lr_ovf, memory_order_relaxed);
    }
    return nl;
}

/* M7T2 dump 侧：拷九只门（定长、无锁读；读到半行 = 观测语义，disclosed）。
   gi = 0..8 → 0x445640 / 0x447b70 / 0x449c50 / 0x44b720 / 0x46e7a0 / 0x4a1100 / 0x4a6b10 /
              0x4a8820 / 0x4af0a0。
     lrd[i]  = {lr, n, first_ns, last_ns}   每站点计数表（site = lr − 4），32 槽
     first[] = {ns, lr, x0, x1, x8, thr}
     last[]  = {ns, lr, x0, x1}
     *lr_ovf = 槽溢出次数 */
int smo_obs_m7t2(int gi, uint64_t *total, uint64_t lrd[][4], int *nlr,
                 uint64_t first[6], uint64_t last[4], uint64_t *lr_ovf) {
    if (gi < 0 || gi >= M7T2_NG) return 0;
    m7t2_g_t *g = &g_m7t2[gi];
    if (total) *total = atomic_load_explicit(&g->k, memory_order_relaxed);
    if (first) {
        if (atomic_load_explicit(&g->fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g->f_tick);
            first[1] = g->f_lr; first[2] = g->f_x0; first[3] = g->f_x1;
            first[4] = g->f_x8; first[5] = g->f_thr;
        } else for (int i = 0; i < 6; i++) first[i] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g->l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g->l_lr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g->l_x0, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g->l_x1, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < M7T2_LR; i++) {
        uint64_t v = g->lr[i].lr;
        if (v) {
            lrd[nl][0] = v;
            lrd[nl][1] = atomic_load_explicit(&g->lr[i].n, memory_order_relaxed);
            lrd[nl][2] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].f_tick, memory_order_relaxed));
            lrd[nl][3] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].l_tick, memory_order_relaxed));
            nl++;
        }
    }
    for (int i = nl; i < M7T2_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = lrd[i][3] = 0;
    if (nlr) *nlr = nl;
    if (lr_ovf) *lr_ovf = atomic_load_explicit(&g->lr_ovf, memory_order_relaxed);
    return nl;
}

/* M7U2 dump 侧：拷三只门（定长、无锁读；读到半行 = 观测语义，disclosed）。
   gi = 0/1/2 → A 0x7acd50 / B 0x7acd94 / C 0x7acd70。
     lrd[i]  = {lr, n, first_ns, last_ns}（site = lr − 4），16 槽
     first[] = {ns, lr, x0, x1, x8, thr}；last[] = {ns, lr, x0, x1}；*lr_ovf = 槽溢出 */
int smo_obs_m7u2(int gi, uint64_t *total, uint64_t lrd[][4], int *nlr,
                 uint64_t first[6], uint64_t last[4], uint64_t *lr_ovf) {
    if (gi < 0 || gi >= M7U2_NG) return 0;
    m7u2_g_t *g = &g_m7u2[gi];
    if (total) *total = atomic_load_explicit(&g->k, memory_order_relaxed);
    if (first) {
        if (atomic_load_explicit(&g->fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g->f_tick);
            first[1] = g->f_lr; first[2] = g->f_x0; first[3] = g->f_x1;
            first[4] = g->f_x8; first[5] = g->f_thr;
        } else for (int i = 0; i < 6; i++) first[i] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g->l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g->l_lr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g->l_x0, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g->l_x1, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < M7U2_LR; i++) {
        uint64_t v = g->lr[i].lr;
        if (v) {
            lrd[nl][0] = v;
            lrd[nl][1] = atomic_load_explicit(&g->lr[i].n, memory_order_relaxed);
            lrd[nl][2] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].f_tick, memory_order_relaxed));
            lrd[nl][3] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].l_tick, memory_order_relaxed));
            nl++;
        }
    }
    for (int i = nl; i < M7U2_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = lrd[i][3] = 0;
    if (nlr) *nlr = nl;
    if (lr_ovf) *lr_ovf = atomic_load_explicit(&g->lr_ovf, memory_order_relaxed);
    return nl;
}
/* M7U2 stage 名环（8 槽）：返回已抓取条数；第 i 条填 meta[8]={ns,gate,lr,this,name_ptr,name2_ptr,
   scene,vtbl}、bytes[32]=该 stage 名前 32 字节原始字节。meta[1]=gate（83=A/85=C）。 */
int smo_obs_m7u2_name(int i, uint64_t meta[8], unsigned char bytes[32]) {
    if (i < 0 || i >= M7U2_NAME_SLOTS) return 0;
    uint64_t have = atomic_load_explicit(&g_m7u2_name.n, memory_order_relaxed);
    if ((uint64_t)i >= have) return 0;
    const m7u2_name_rec_t *r = &g_m7u2_name.s[i];
    if (meta) {
        meta[0] = m7w_tick2ns(r->tick); meta[1] = r->gate; meta[2] = r->lr; meta[3] = r->this_;
        meta[4] = r->name_ptr; meta[5] = r->name2_ptr; meta[6] = r->scene; meta[7] = r->vtbl;
    }
    if (bytes) for (int k = 0; k < 32; k++) bytes[k] = r->b[k];
    return 1;   /* 1 = 该槽有效（字节内容由 dump 侧按可打印字符过滤） */
}
int smo_obs_m7u2_name_n(void) {
    uint64_t have = atomic_load_explicit(&g_m7u2_name.n, memory_order_relaxed);
    return have > M7U2_NAME_SLOTS ? M7U2_NAME_SLOTS : (int)have;
}

/* M7V dump 侧（定长、无锁读；读到半行 = 观测语义，disclosed）。
   gi = 0..7 → 0=X8 0x7acd8c / 1..7=R1..R7（0x4a6a90/0x46e770/0x445590/0x447ae0/0x449bc0/0x44b6f0/0x4af020）。
     lrd[i] = {lr, n, first_ns, last_ns}（site = lr − 4），16 槽
     first[] = {ns, lr, x0, x1, x8, thr}；last[] = {ns, lr, x0, x1} */
int smo_obs_m7v(int gi, uint64_t *total, uint64_t lrd[][4], int *nlr,
                uint64_t first[6], uint64_t last[4], uint64_t *lr_ovf) {
    if (gi < 0 || gi >= M7V_NG) return 0;
    m7v_g_t *g = &g_m7v[gi];
    if (total) *total = atomic_load_explicit(&g->k, memory_order_relaxed);
    if (first) {
        if (atomic_load_explicit(&g->fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g->f_tick);
            first[1] = g->f_lr; first[2] = g->f_x0; first[3] = g->f_x1;
            first[4] = g->f_x8; first[5] = g->f_thr;
        } else for (int i = 0; i < 6; i++) first[i] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g->l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g->l_lr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g->l_x0, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g->l_x1, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < M7V_LR; i++) {
        uint64_t v = g->lr[i].lr;
        if (v) {
            lrd[nl][0] = v;
            lrd[nl][1] = atomic_load_explicit(&g->lr[i].n, memory_order_relaxed);
            lrd[nl][2] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].f_tick, memory_order_relaxed));
            lrd[nl][3] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].l_tick, memory_order_relaxed));
            nl++;
        }
    }
    for (int i = nl; i < M7V_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = lrd[i][3] = 0;
    if (nlr) *nlr = nl;
    if (lr_ovf) *lr_ovf = atomic_load_explicit(&g->lr_ovf, memory_order_relaxed);
    return nl;
}
/* M7V X8 门专用环：meta[5] = {ns, lr, scene, vtbl, tgt}（tgt = 解一层后的真派发目标） */
int smo_obs_m7v_d(int i, uint64_t meta[5], uint32_t *ok, uint32_t *thr) {
    if (i < 0 || i >= M7V_D_SLOTS) return 0;
    uint64_t have = atomic_load_explicit(&g_m7v_d.n, memory_order_relaxed);
    if ((uint64_t)i >= have) return 0;
    const m7v_d_rec_t *r = &g_m7v_d.s[i];
    if (meta) {
        meta[0] = m7w_tick2ns(r->tick); meta[1] = r->lr; meta[2] = r->scene;
        meta[3] = r->vtbl; meta[4] = r->tgt;
    }
    if (ok) *ok = r->ok;
    if (thr) *thr = r->thr;
    return 1;
}
int smo_obs_m7v_d_n(void) {
    uint64_t have = atomic_load_explicit(&g_m7v_d.n, memory_order_relaxed);
    return have > M7V_D_SLOTS ? M7V_D_SLOTS : (int)have;
}
/* M7V 名环 v2：meta[8] = {ns, gate, lr, this, name_ptr, q_ptr, scene, vtbl}；
   bytes[128] = s0||s1||s2||s3 各 32B **原始字节（不改写、不 ASCII 过滤）**；
   avail 位图 = 四槽读成功位；readable 位图 = 四槽中含可打印 ASCII 的槽位。 */
int smo_obs_m7v_name(int i, uint64_t meta[8], unsigned char bytes[128],
                     uint32_t *avail, uint32_t *readable, uint32_t *thr) {
    if (i < 0 || i >= M7V_NAME_SLOTS) return 0;
    uint64_t have = atomic_load_explicit(&g_m7v_name.n, memory_order_relaxed);
    if ((uint64_t)i >= have) return 0;
    const m7v_name_rec_t *r = &g_m7v_name.s[i];
    if (meta) {
        meta[0] = m7w_tick2ns(r->tick); meta[1] = r->gate; meta[2] = r->lr; meta[3] = r->this_;
        meta[4] = r->name_ptr; meta[5] = r->q_ptr; meta[6] = r->scene; meta[7] = r->vtbl;
    }
    if (bytes) {
        for (int k = 0; k < 32; k++) bytes[k] = r->s0[k];
        for (int k = 0; k < 32; k++) bytes[32 + k] = r->s1[k];
        for (int k = 0; k < 32; k++) bytes[64 + k] = r->s2[k];
        for (int k = 0; k < 32; k++) bytes[96 + k] = r->s3[k];
    }
    if (avail) *avail = r->avail;
    if (readable) *readable = r->readable;
    if (thr) *thr = r->thr;
    return 1;
}
int smo_obs_m7v_name_n(void) {
    uint64_t have = atomic_load_explicit(&g_m7v_name.n, memory_order_relaxed);
    return have > M7V_NAME_SLOTS ? M7V_NAME_SLOTS : (int)have;
}

/* M7W2 dump 侧（定长、无锁读；读到半行 = 观测语义，disclosed）。
   AD 门 main+0x7724d0：meta[11] = {ns, lr, obj, p, o, tag, arg, q, vt, tgt, dtbl}
     （ns 由 tick→ns 两点插值；q = [obj+8]+(tag>>1) 真 this；vt = *(q)；tgt = 真派发目标） */
int smo_obs_m7w2_ad(int i, uint64_t meta[11], uint32_t *kind, uint32_t *ok,
                    uint32_t *rd, uint32_t *thr) {
    if (i < 0 || i >= M7W2_SLOTS) return 0;
    uint64_t have = atomic_load_explicit(&g_m7w2_ad.n, memory_order_relaxed);
    if ((uint64_t)i >= have) return 0;
    const m7w2_rec_t *r = &g_m7w2_ad.s[i];
    if (meta) {
        meta[0] = m7w_tick2ns(r->tick); meta[1] = r->lr;  meta[2] = r->obj;
        meta[3] = r->p;                meta[4] = r->o;   meta[5] = r->tag;
        meta[6] = r->arg;              meta[7] = r->q;   meta[8] = r->vt;
        meta[9] = r->tgt;              meta[10] = r->dtbl;
    }
    if (kind) *kind = r->kind;
    if (ok) *ok = r->ok;
    if (rd) *rd = r->rd;
    if (thr) *thr = r->thr;
    return 1;
}
int smo_obs_m7w2_ad_n(void) {
    uint64_t have = atomic_load_explicit(&g_m7w2_ad.n, memory_order_relaxed);
    return have > M7W2_SLOTS ? M7W2_SLOTS : (int)have;
}
/* 台账：total + per-LR 8 槽 {lr, n, first_ns, last_ns} + 首末见 {ns, lr, obj} */
int smo_obs_m7w2_sum(uint64_t *total, uint64_t lrd[][4], int *nlr,
                     uint64_t first[3], uint64_t last[3], uint64_t *lr_ovf) {
    if (total) *total = atomic_load_explicit(&g_m7w2_g.k, memory_order_relaxed);
    if (first) {
        if (atomic_load_explicit(&g_m7w2_g.fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g_m7w2_g.f_tick);
            first[1] = g_m7w2_g.f_lr; first[2] = g_m7w2_g.f_obj;
        } else first[0] = first[1] = first[2] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g_m7w2_g.l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g_m7w2_g.l_lr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g_m7w2_g.l_obj, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < M7W2_LR; i++) {
        uint64_t v = g_m7w2_g.lr[i].lr;
        if (!v) continue;
        lrd[nl][0] = v;
        lrd[nl][1] = atomic_load_explicit(&g_m7w2_g.lr[i].n, memory_order_relaxed);
        lrd[nl][2] = m7w_tick2ns(atomic_load_explicit(&g_m7w2_g.lr[i].f_tick, memory_order_relaxed));
        lrd[nl][3] = m7w_tick2ns(atomic_load_explicit(&g_m7w2_g.lr[i].l_tick, memory_order_relaxed));
        nl++;
    }
    for (int i = nl; i < M7W2_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = lrd[i][3] = 0;
    if (nlr) *nlr = nl;
    if (lr_ovf) *lr_ovf = atomic_load_explicit(&g_m7w2_g.lr_ovf, memory_order_relaxed);
    return nl;
}

/* M7X2 dump 侧（定长、无锁读；读到半行 = 观测语义，disclosed）。
   G1 门 95 main+0x74c9e0：meta[7] = {ns, lr, q, vt, tgt3, o18, arg}
     q = 门点 x0（适配器算好的真 this）；vt = [q]；tgt3 = [[q]+0x18] = thunk 真正会跳的地址；
     o18 = [q+0x18]（q 自己的字面字段，与 tgt3 区分开，避免"两层/一层"歧义）。
   G2 门 96 main+0x772500：meta[8] = {ns, lr, src, w8, w10, w18, w20, svt}
     src = 门点 x0（= 0x77250c `mov x19,x0` 的源）；w8/w10/w18/w20 = 源 {+8,+0x10,+0x18,+0x20}
     （= 被写进新对象的 32 B，纯 ldp/stp 拷贝）；svt = [src]。 */
int smo_obs_m7x2_g1(int i, uint64_t meta[7], uint32_t *rd, uint32_t *ok, uint32_t *thr) {
    if (i < 0 || i >= M7X2_G1_SLOTS) return 0;
    uint64_t have = atomic_load_explicit(&g_m7x2_g1.n, memory_order_relaxed);
    if ((uint64_t)i >= have) return 0;
    const m7x2_g1_rec_t *r = &g_m7x2_g1.s[i];
    if (meta) {
        meta[0] = m7w_tick2ns(r->tick); meta[1] = r->lr;  meta[2] = r->q;
        meta[3] = r->vt;                meta[4] = r->tgt3; meta[5] = r->o18;
        meta[6] = r->arg;
    }
    if (rd) *rd = r->rd;
    if (ok) *ok = r->ok;
    if (thr) *thr = r->thr;
    return 1;
}
int smo_obs_m7x2_g1_n(void) {
    uint64_t have = atomic_load_explicit(&g_m7x2_g1.n, memory_order_relaxed);
    return have > M7X2_G1_SLOTS ? M7X2_G1_SLOTS : (int)have;
}
int smo_obs_m7x2_g1_sum(uint64_t *total, uint64_t lrd[][4], int *nlr,
                        uint64_t first[3], uint64_t last[3], uint64_t *lr_ovf) {
    if (total) *total = atomic_load_explicit(&g_m7x2_g1t.k, memory_order_relaxed);
    if (first) {
        if (atomic_load_explicit(&g_m7x2_g1t.fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g_m7x2_g1t.f_tick);
            first[1] = g_m7x2_g1t.f_lr; first[2] = g_m7x2_g1t.f_q;
        } else first[0] = first[1] = first[2] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g_m7x2_g1t.l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g_m7x2_g1t.l_lr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g_m7x2_g1t.l_q, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < M7X2_LR; i++) {
        uint64_t v = g_m7x2_g1t.lr[i].lr;
        if (!v) continue;
        lrd[nl][0] = v;
        lrd[nl][1] = atomic_load_explicit(&g_m7x2_g1t.lr[i].n, memory_order_relaxed);
        lrd[nl][2] = m7w_tick2ns(atomic_load_explicit(&g_m7x2_g1t.lr[i].f_tick, memory_order_relaxed));
        lrd[nl][3] = m7w_tick2ns(atomic_load_explicit(&g_m7x2_g1t.lr[i].l_tick, memory_order_relaxed));
        nl++;
    }
    for (int i = nl; i < M7X2_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = lrd[i][3] = 0;
    if (nlr) *nlr = nl;
    if (lr_ovf) *lr_ovf = atomic_load_explicit(&g_m7x2_g1t.lr_ovf, memory_order_relaxed);
    return nl;
}
int smo_obs_m7x2_g2(int i, uint64_t meta[8], uint32_t *rd, uint32_t *thr) {
    if (i < 0 || i >= M7X2_G2_SLOTS) return 0;
    uint64_t have = atomic_load_explicit(&g_m7x2_g2.n, memory_order_relaxed);
    if ((uint64_t)i >= have) return 0;
    const m7x2_g2_rec_t *r = &g_m7x2_g2.s[i];
    if (meta) {
        meta[0] = m7w_tick2ns(r->tick); meta[1] = r->lr;   meta[2] = r->src;
        meta[3] = r->w8;               meta[4] = r->w10;  meta[5] = r->w18;
        meta[6] = r->w20;              meta[7] = r->svt;
    }
    if (rd) *rd = r->rd;
    if (thr) *thr = r->thr;
    return 1;
}
int smo_obs_m7x2_g2_n(void) {
    uint64_t have = atomic_load_explicit(&g_m7x2_g2.n, memory_order_relaxed);
    return have > M7X2_G2_SLOTS ? M7X2_G2_SLOTS : (int)have;
}
int smo_obs_m7x2_g2_sum(uint64_t *total, uint64_t lrd[][4], int *nlr,
                        uint64_t first[3], uint64_t last[3], uint64_t *lr_ovf) {
    if (total) *total = atomic_load_explicit(&g_m7x2_g2t.k, memory_order_relaxed);
    if (first) {
        if (atomic_load_explicit(&g_m7x2_g2t.fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g_m7x2_g2t.f_tick);
            first[1] = g_m7x2_g2t.f_lr; first[2] = g_m7x2_g2t.f_src;
        } else first[0] = first[1] = first[2] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g_m7x2_g2t.l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g_m7x2_g2t.l_lr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g_m7x2_g2t.l_src, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < M7X2_LR; i++) {
        uint64_t v = g_m7x2_g2t.lr[i].lr;
        if (!v) continue;
        lrd[nl][0] = v;
        lrd[nl][1] = atomic_load_explicit(&g_m7x2_g2t.lr[i].n, memory_order_relaxed);
        lrd[nl][2] = m7w_tick2ns(atomic_load_explicit(&g_m7x2_g2t.lr[i].f_tick, memory_order_relaxed));
        lrd[nl][3] = m7w_tick2ns(atomic_load_explicit(&g_m7x2_g2t.lr[i].l_tick, memory_order_relaxed));
        nl++;
    }
    for (int i = nl; i < M7X2_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = lrd[i][3] = 0;
    if (nlr) *nlr = nl;
    if (lr_ovf) *lr_ovf = atomic_load_explicit(&g_m7x2_g2t.lr_ovf, memory_order_relaxed);
    return nl;
}

/* ---- M7Y2 访问器（定长、无锁读、无 I/O） ---- */
int smo_obs_m7y2_g1(int i, uint64_t meta[7], uint32_t *rd, uint32_t *ok, uint32_t *thr, unsigned char *nm, uint32_t *nmlen) {
    if (i < 0 || i >= M7Y2_G1_SLOTS) return 0;
    uint64_t have = atomic_load_explicit(&g_m7y2_g1.n, memory_order_relaxed);
    if ((uint64_t)i >= have) return 0;
    const m7y2_g1_rec_t *r = &g_m7y2_g1.s[i];
    if (meta) {
        meta[0] = m7w_tick2ns(r->tick); meta[1] = r->lr;  meta[2] = r->q;
        meta[3] = r->vt;                meta[4] = r->tgt3; meta[5] = r->nm_ptr;
        meta[6] = r->arg;
    }
    if (nm) for (uint32_t k = 0; k < M7Y2_NAME_MAX; k++) nm[k] = r->nm[k];
    if (nmlen) *nmlen = r->nm_len;
    if (rd) *rd = r->rd;
    if (ok) *ok = r->ok;
    if (thr) *thr = r->thr;
    return 1;
}
int smo_obs_m7y2_g1_n(void) {
    uint64_t have = atomic_load_explicit(&g_m7y2_g1.n, memory_order_relaxed);
    return have > M7Y2_G1_SLOTS ? M7Y2_G1_SLOTS : (int)have;
}
int smo_obs_m7y2_g1_sum(uint64_t *total, uint64_t lrd[][4], int *nlr,
                        uint64_t first[3], uint64_t last[3], uint64_t *lr_ovf) {
    if (total) *total = atomic_load_explicit(&g_m7y2_g1t.k, memory_order_relaxed);
    if (first) {
        if (atomic_load_explicit(&g_m7y2_g1t.fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g_m7y2_g1t.f_tick);
            first[1] = g_m7y2_g1t.f_lr; first[2] = g_m7y2_g1t.f_q;
        } else first[0] = first[1] = first[2] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g_m7y2_g1t.l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g_m7y2_g1t.l_lr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g_m7y2_g1t.l_q, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < M7Y2_LR; i++) {
        uint64_t v = g_m7y2_g1t.lr[i].lr;
        if (!v) continue;
        lrd[nl][0] = v;
        lrd[nl][1] = atomic_load_explicit(&g_m7y2_g1t.lr[i].n, memory_order_relaxed);
        lrd[nl][2] = m7w_tick2ns(atomic_load_explicit(&g_m7y2_g1t.lr[i].f_tick, memory_order_relaxed));
        lrd[nl][3] = m7w_tick2ns(atomic_load_explicit(&g_m7y2_g1t.lr[i].l_tick, memory_order_relaxed));
        nl++;
    }
    for (int i = nl; i < M7Y2_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = lrd[i][3] = 0;
    if (nlr) *nlr = nl;
    if (lr_ovf) *lr_ovf = atomic_load_explicit(&g_m7y2_g1t.lr_ovf, memory_order_relaxed);
    return nl;
}
int smo_obs_m7y2_g2(int i, uint64_t meta[6], uint32_t *rd, uint32_t *thr, unsigned char *nm, uint32_t *nmlen) {
    if (i < 0 || i >= M7Y2_G2_SLOTS) return 0;
    uint64_t have = atomic_load_explicit(&g_m7y2_g2.n, memory_order_relaxed);
    if ((uint64_t)i >= have) return 0;
    const m7y2_g2_rec_t *r = &g_m7y2_g2.s[i];
    if (meta) {
        meta[0] = m7w_tick2ns(r->tick); meta[1] = r->lr;  meta[2] = r->q;
        meta[3] = r->vt;                meta[4] = r->nm_ptr; meta[5] = r->arg;
    }
    if (nm) for (uint32_t k = 0; k < M7Y2_NAME_MAX; k++) nm[k] = r->nm[k];
    if (nmlen) *nmlen = r->nm_len;
    if (rd) *rd = r->rd;
    if (thr) *thr = r->thr;
    return 1;
}
int smo_obs_m7y2_g2_n(void) {
    uint64_t have = atomic_load_explicit(&g_m7y2_g2.n, memory_order_relaxed);
    return have > M7Y2_G2_SLOTS ? M7Y2_G2_SLOTS : (int)have;
}
int smo_obs_m7y2_g2_sum(uint64_t *total, uint64_t lrd[][4], int *nlr,
                        uint64_t first[3], uint64_t last[3], uint64_t *lr_ovf) {
    if (total) *total = atomic_load_explicit(&g_m7y2_g2t.k, memory_order_relaxed);
    if (first) {
        if (atomic_load_explicit(&g_m7y2_g2t.fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g_m7y2_g2t.f_tick);
            first[1] = g_m7y2_g2t.f_lr; first[2] = g_m7y2_g2t.f_q;
        } else first[0] = first[1] = first[2] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g_m7y2_g2t.l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g_m7y2_g2t.l_lr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g_m7y2_g2t.l_q, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < M7Y2_LR; i++) {
        uint64_t v = g_m7y2_g2t.lr[i].lr;
        if (!v) continue;
        lrd[nl][0] = v;
        lrd[nl][1] = atomic_load_explicit(&g_m7y2_g2t.lr[i].n, memory_order_relaxed);
        lrd[nl][2] = m7w_tick2ns(atomic_load_explicit(&g_m7y2_g2t.lr[i].f_tick, memory_order_relaxed));
        lrd[nl][3] = m7w_tick2ns(atomic_load_explicit(&g_m7y2_g2t.lr[i].l_tick, memory_order_relaxed));
        nl++;
    }
    for (int i = nl; i < M7Y2_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = lrd[i][3] = 0;
    if (nlr) *nlr = nl;
    if (lr_ovf) *lr_ovf = atomic_load_explicit(&g_m7y2_g2t.lr_ovf, memory_order_relaxed);
    return nl;
}

/* ---- M7Z2 访问器（定长、无锁读、无 I/O）。gi = 0..4 对应门 99..103 ---- */
int smo_obs_m7z2_n(int gi) {
    if (gi < 0 || gi >= M7Z2_NG) return 0;
    return (int)atomic_load_explicit(&g_m7z2[gi].rot, memory_order_relaxed);
}
int smo_obs_m7z2_rec(int gi, int i, uint64_t meta[10], uint32_t *thr) {
    if (gi < 0 || gi >= M7Z2_NG || i < 0 || i >= M7Z2_RING) return 0;
    const m7z2_rec_t *r = &g_m7z2[gi].r[i];
    if (!r->seq) return 0;
    if (meta) {
        meta[0] = m7w_tick2ns(r->tick); meta[1] = r->lr;   meta[2] = r->x0;
        meta[3] = r->x1;                meta[4] = r->x8;   meta[5] = r->a;
        meta[6] = r->b;                 meta[7] = r->c;    meta[8] = r->d;
        meta[9] = r->e;
    }
    if (thr) *thr = r->thr;
    return 1;
}
int smo_obs_m7z2_sum(int gi, uint64_t *k_all, uint64_t *k_site, uint64_t lrd[][4], int *nlr,
                     uint64_t first[10], uint64_t last[10], uint64_t *lr_ovf, uint64_t *rot) {
    if (gi < 0 || gi >= M7Z2_NG) return 0;
    m7z2_gate_t *g = &g_m7z2[gi];
    if (k_all) *k_all = atomic_load_explicit(&g->k, memory_order_relaxed);
    if (k_site) *k_site = atomic_load_explicit(&g->ks, memory_order_relaxed);
    if (rot) *rot = atomic_load_explicit(&g->rot, memory_order_relaxed);
    if (first) {
        if (atomic_load_explicit(&g->fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g->f_tick); first[1] = g->f_lr; first[2] = g->f_x0;
            first[3] = g->f_x1; first[4] = g->f_x8; first[5] = g->f_a;
            first[6] = g->f_b; first[7] = g->f_c; first[8] = g->f_d; first[9] = g->f_e;
        } else for (int i = 0; i < 10; i++) first[i] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g->l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g->l_lr, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g->l_x0, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g->l_x1, memory_order_relaxed);
        last[4] = atomic_load_explicit(&g->l_x8, memory_order_relaxed);
        last[5] = atomic_load_explicit(&g->l_a, memory_order_relaxed);
        last[6] = atomic_load_explicit(&g->l_b, memory_order_relaxed);
        last[7] = atomic_load_explicit(&g->l_c, memory_order_relaxed);
        last[8] = atomic_load_explicit(&g->l_d, memory_order_relaxed);
        last[9] = atomic_load_explicit(&g->l_e, memory_order_relaxed);
    }
    int nl = 0;
    for (int i = 0; i < M7Z2_LR; i++) {
        uint64_t v = atomic_load_explicit(&g->lr[i].lr, memory_order_relaxed);
        if (!v) continue;
        lrd[nl][0] = v;
        lrd[nl][1] = atomic_load_explicit(&g->lr[i].n, memory_order_relaxed);
        lrd[nl][2] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].f_tick, memory_order_relaxed));
        lrd[nl][3] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].l_tick, memory_order_relaxed));
        nl++;
    }
    for (int i = nl; i < M7Z2_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = lrd[i][3] = 0;
    if (nlr) *nlr = nl;
    if (lr_ovf) *lr_ovf = atomic_load_explicit(&g->lr_ovf, memory_order_relaxed);
    return nl;
}
/* 每-LR 台账的末见 x0/x8/a/b/c/d/e（dump 侧按 LR 取"本站点那一路"的末值） */
int smo_obs_m7z2_lrlast(int gi, int slot, uint64_t *lr, uint64_t *n, uint64_t *x0, uint64_t *x8,
                        uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d, uint64_t *e) {
    if (gi < 0 || gi >= M7Z2_NG || slot < 0 || slot >= M7Z2_LR) return 0;
    m7z2_gate_t *g = &g_m7z2[gi];
    uint64_t v = atomic_load_explicit(&g->lr[slot].lr, memory_order_relaxed);
    if (!v) return 0;
    if (lr) *lr = v;
    if (n) *n = atomic_load_explicit(&g->lr[slot].n, memory_order_relaxed);
    if (x0) *x0 = atomic_load_explicit(&g->lr[slot].l_x0, memory_order_relaxed);
    if (x8) *x8 = atomic_load_explicit(&g->lr[slot].l_x8, memory_order_relaxed);
    if (a) *a = atomic_load_explicit(&g->lr[slot].l_a, memory_order_relaxed);
    if (b) *b = atomic_load_explicit(&g->lr[slot].l_b, memory_order_relaxed);
    if (c) *c = atomic_load_explicit(&g->lr[slot].l_c, memory_order_relaxed);
    if (d) *d = atomic_load_explicit(&g->lr[slot].l_d, memory_order_relaxed);
    if (e) *e = atomic_load_explicit(&g->lr[slot].l_e, memory_order_relaxed);
    return 1;
}

/* ---- M8B 访问器（定长、无锁读、无 I/O）。gi = 0..1 对应门 104..105 ---- */
int smo_obs_m8b_n(int gi) {
    if (gi < 0 || gi >= M8B_NG) return 0;
    uint64_t v = atomic_load_explicit(&g_m8b[gi].rot, memory_order_relaxed);
    return (int)(v > M8B_RING ? M8B_RING : v);
}
int smo_obs_m8b_rec(int gi, int i, uint64_t meta[6], char *name, int namelen) {
    if (gi < 0 || gi >= M8B_NG || i < 0 || i >= M8B_RING) return 0;
    const m8b_rec_t *r = &g_m8b[gi].r[i];
    if (!r->tick) return 0;
    if (meta) {
        meta[0] = m7w_tick2ns(r->tick); meta[1] = r->x1;   meta[2] = r->old10;
        meta[3] = r->old18;             meta[4] = r->lr;   meta[5] = r->keeper;
    }
    if (name && namelen > 0) {
        int n = namelen - 1; if (n > M8B_NAMELEN - 1) n = M8B_NAMELEN - 1;
        memcpy(name, r->name, (size_t)n); name[n] = 0;
    }
    return 1;
}
int smo_obs_m8b_last(int gi, int i, uint64_t meta[6], char *name, int namelen) {
    if (gi < 0 || gi >= M8B_NG || i < 0 || i >= M8B_LAST) return 0;
    const m8b_rec_t *r = &g_m8b[gi].last[i];
    if (!r->tick) return 0;
    if (meta) {
        meta[0] = m7w_tick2ns(r->tick); meta[1] = r->x1;   meta[2] = r->old10;
        meta[3] = r->old18;             meta[4] = r->lr;   meta[5] = r->keeper;
    }
    if (name && namelen > 0) {
        int n = namelen - 1; if (n > M8B_NAMELEN - 1) n = M8B_NAMELEN - 1;
        memcpy(name, r->name, (size_t)n); name[n] = 0;
    }
    return 1;
}
int smo_obs_m8b_sum(int gi, uint64_t *k, uint64_t first[6], uint64_t last[6],
                    char *fname, char *lname, int nl, uint64_t lrd[][4], int *nlr,
                    uint64_t *lr_ovf, uint64_t *rot, uint64_t *lrot) {
    if (gi < 0 || gi >= M8B_NG) return 0;
    m8b_g_t *g = &g_m8b[gi];
    if (k) *k = atomic_load_explicit(&g->k, memory_order_relaxed);
    if (rot) *rot = atomic_load_explicit(&g->rot, memory_order_relaxed);
    if (lrot) *lrot = atomic_load_explicit(&g->lrot, memory_order_relaxed);
    if (first) {
        if (atomic_load_explicit(&g->fset, memory_order_relaxed)) {
            first[0] = m7w_tick2ns(g->f_tick); first[1] = g->f_x1; first[2] = g->f_old10;
            first[3] = g->f_old18;             first[4] = g->f_lr; first[5] = g->f_keeper;
        } else for (int i = 0; i < 6; i++) first[i] = 0;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g->l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g->l_x1, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g->l_old10, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g->l_old18, memory_order_relaxed);
        last[4] = atomic_load_explicit(&g->l_lr, memory_order_relaxed);
        last[5] = atomic_load_explicit(&g->l_keeper, memory_order_relaxed);
    }
    if (fname && nl > 0) { int n = nl - 1; if (n > M8B_NAMELEN - 1) n = M8B_NAMELEN - 1;
                           memcpy(fname, g->f_name, (size_t)n); fname[n] = 0; }
    if (lname && nl > 0) { int n = nl - 1; if (n > M8B_NAMELEN - 1) n = M8B_NAMELEN - 1;
                           memcpy(lname, g->l_name, (size_t)n); lname[n] = 0; }
    int nl2 = 0;
    for (int i = 0; i < M8B_LR; i++) {
        uint64_t v = g->lr[i].lr;
        if (!v) continue;
        lrd[nl2][0] = v;
        lrd[nl2][1] = atomic_load_explicit(&g->lr[i].n, memory_order_relaxed);
        lrd[nl2][2] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].f_tick, memory_order_relaxed));
        lrd[nl2][3] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].l_tick, memory_order_relaxed));
        nl2++;
    }
    for (int i = nl2; i < M8B_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = lrd[i][3] = 0;
    if (nlr) *nlr = nl2;
    if (lr_ovf) *lr_ovf = atomic_load_explicit(&g->lr_ovf, memory_order_relaxed);
    return nl2;
}
int smo_obs_m8b_tokn(int gi) {
    if (gi < 0 || gi >= M8B_NG) return 0;
    unsigned v = atomic_load_explicit(&g_m8b[gi].tok_n, memory_order_relaxed);
    return (int)(v > M8B_TOK ? M8B_TOK : v);
}
int smo_obs_m8b_tok(int gi, int i, uint64_t *token, uint64_t *n, uint64_t *ft, uint64_t *lt,
                    uint64_t *llr, uint64_t *keeper, char *name, int namelen) {
    if (gi < 0 || gi >= M8B_NG || i < 0 || i >= M8B_TOK) return 0;
    m8b_tok_t *tk = &g_m8b[gi].tok[i];
    uint64_t tv = tk->token;
    if (!tv) return 0;
    if (token) *token = tv;
    if (n) *n = atomic_load_explicit(&tk->n, memory_order_relaxed);
    if (ft) *ft = m7w_tick2ns(atomic_load_explicit(&tk->f_tick, memory_order_relaxed));
    if (lt) *lt = m7w_tick2ns(atomic_load_explicit(&tk->l_tick, memory_order_relaxed));
    if (llr) *llr = atomic_load_explicit(&tk->l_lr, memory_order_relaxed);
    if (keeper) *keeper = atomic_load_explicit(&tk->l_keeper, memory_order_relaxed);
    if (name && namelen > 0) {
        int nn = namelen - 1; if (nn > M8B_NAMELEN - 1) nn = M8B_NAMELEN - 1;
        memcpy(name, tk->name, (size_t)nn); name[nn] = 0;
    }
    return 1;
}
uint64_t smo_obs_m8b_tokovf(int gi) {
    if (gi < 0 || gi >= M8B_NG) return 0;
    return atomic_load_explicit(&g_m8b[gi].tok_ovf, memory_order_relaxed);
}

/* ---- M8E2 访问器（定长、无锁读、无 I/O）。gi = 0..3 对应门 108..111 ---- */
int smo_obs_m8e2_sum(int gi, uint64_t *k, uint64_t first[5], uint64_t last[5],
                     uint64_t lrd[][4], int *nlr, uint64_t *lr_ovf, uint64_t *rot) {
    if (gi < 0 || gi >= M8E2_NG) return 0;
    m8e2_g_t *g = &g_m8e2[gi];
    if (k) *k = atomic_load_explicit(&g->k, memory_order_relaxed);
    if (rot) *rot = atomic_load_explicit(&g->rot, memory_order_relaxed);
    if (first) {
        first[0] = m7w_tick2ns(g->f_tick);
        first[1] = g->f_x0; first[2] = g->f_x1; first[3] = g->f_x2; first[4] = g->f_lr;
    }
    if (last) {
        last[0] = m7w_tick2ns(atomic_load_explicit(&g->l_tick, memory_order_relaxed));
        last[1] = atomic_load_explicit(&g->l_x0, memory_order_relaxed);
        last[2] = atomic_load_explicit(&g->l_x1, memory_order_relaxed);
        last[3] = atomic_load_explicit(&g->l_x2, memory_order_relaxed);
        last[4] = atomic_load_explicit(&g->l_lr, memory_order_relaxed);
    }
    int nl2 = 0;
    for (int i = 0; i < M8E2_LR; i++) {
        if (!g->lr[i].lr) continue;
        lrd[nl2][0] = g->lr[i].lr;
        lrd[nl2][1] = atomic_load_explicit(&g->lr[i].n, memory_order_relaxed);
        lrd[nl2][2] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].f_tick, memory_order_relaxed));
        lrd[nl2][3] = m7w_tick2ns(atomic_load_explicit(&g->lr[i].l_tick, memory_order_relaxed));
        nl2++;
    }
    for (int i = nl2; i < M8E2_LR; i++) lrd[i][0] = lrd[i][1] = lrd[i][2] = lrd[i][3] = 0;
    if (nlr) *nlr = nl2;
    if (lr_ovf) *lr_ovf = atomic_load_explicit(&g->lr_ovf, memory_order_relaxed);
    return nl2;
}
int smo_obs_m8e2_rec(int gi, int i, uint64_t meta[5], char *name, int namelen) {
    if (gi < 0 || gi >= M8E2_NG || i < 0 || i >= M8E2_RING) return 0;
    const m8e2_rec_t *r = &g_m8e2[gi].r[i];
    if (!r->tick) return 0;
    if (meta) {
        meta[0] = m7w_tick2ns(r->tick);
        meta[1] = r->x0; meta[2] = r->x1; meta[3] = r->x2; meta[4] = r->lr;
    }
    if (name && namelen > 0) {
        int n = namelen - 1;
        if (n > M8E2_NAMELEN - 1) n = M8E2_NAMELEN - 1;
        memcpy(name, r->name, (size_t)n);
        name[n] = 0;
    }
    return 1;
}

/* ---- M10A 访问器（定长、无锁读、无 I/O）。tick 一律换算成 ns 再返回 ---- */
int smo_obs_m10a_sum(int *mode, int *limit, int *install, int *hits, int *forced,
                     uint64_t *raw, uint64_t *ret, uint64_t *f_ns, uint64_t *l_ns) {
    if (mode)    *mode    = atomic_load_explicit(&g_m10a_mode, memory_order_relaxed);
    if (limit)   *limit   = atomic_load_explicit(&g_m10a_limit, memory_order_relaxed);
    if (install) *install = atomic_load_explicit(&g_m10a_install, memory_order_relaxed);
    if (hits)    *hits    = atomic_load_explicit(&g_m10a_st.hits, memory_order_relaxed);
    if (forced)  *forced  = atomic_load_explicit(&g_m10a_st.forced, memory_order_relaxed);
    /* raw/ret 是变体 stub 直接写的格子（原骨架模式下恒 0 ⇒ 说明"没装变体"） */
    if (raw)     *raw     = (uint64_t)(uint32_t)atomic_load_explicit(&g_m10a_st.raw, memory_order_relaxed);
    if (ret)     *ret     = (uint64_t)(uint32_t)atomic_load_explicit(&g_m10a_st.ret, memory_order_relaxed);
    if (f_ns)    *f_ns    = m7w_tick2ns(atomic_load_explicit(&g_m10a_st.f_tick, memory_order_relaxed));
    if (l_ns)    *l_ns    = m7w_tick2ns(atomic_load_explicit(&g_m10a_st.l_tick, memory_order_relaxed));
    return (int)atomic_load_explicit(&g_m10a_st.hits, memory_order_relaxed);
}
int smo_obs_m10a_rn(void) { return (int)atomic_load_explicit(&g_m10a_rn, memory_order_relaxed); }
int smo_obs_m10a_rec(int i, uint64_t *ns, int *forced, int *raw, int *ret) {
    if (i < 0 || i >= M10A_RING) return 0;
    if (ns)     *ns     = m7w_tick2ns(g_m10a_r[i].tick);
    if (forced) *forced = g_m10a_r[i].forced;
    if (raw)    *raw    = g_m10a_r[i].raw;
    if (ret)    *ret    = g_m10a_r[i].ret;
    return g_m10a_r[i].tick ? 1 : 0;
}
/* 门 112（A 支入口）：k = 命中数；x0 = 调用方看到的返回值（首末各一）。 */
int smo_obs_m10a_a(uint64_t *k, uint64_t *f_x0, uint64_t *l_x0, uint64_t *f_lr, uint64_t *l_lr,
                   uint64_t *f_ns, uint64_t *l_ns) {
    if (k)    *k    = atomic_load_explicit(&g_m10a_a.k, memory_order_relaxed);
    if (f_x0) *f_x0 = atomic_load_explicit(&g_m10a_a.f_x0, memory_order_relaxed);
    if (l_x0) *l_x0 = atomic_load_explicit(&g_m10a_a.l_x0, memory_order_relaxed);
    if (f_lr) *f_lr = atomic_load_explicit(&g_m10a_a.f_lr, memory_order_relaxed);
    if (l_lr) *l_lr = atomic_load_explicit(&g_m10a_a.l_lr, memory_order_relaxed);
    if (f_ns) *f_ns = m7w_tick2ns(atomic_load_explicit(&g_m10a_a.f_tick, memory_order_relaxed));
    if (l_ns) *l_ns = m7w_tick2ns(atomic_load_explicit(&g_m10a_a.l_tick, memory_order_relaxed));
    return (int)atomic_load_explicit(&g_m10a_a.k, memory_order_relaxed);
}

void runtime_game_patches(Module *main_module) {
    patch_render_res(main_module);
    if (g_render_w) { patch_scene_pick(main_module); patch_rt_pool(main_module); }
    fprintf(stderr, "[odyssey] game patches: %s\n", g_render_w ? "docked render size, key 3 scene size" : "none");
    odyssey_aspect_install(main_module);
    smo_obs_gates_install(main_module);
}
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; odyssey_aspect_present(); scene_log(); }   /* key 4, every present */
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
/* Key 3 (runtime/input.c). At a bigger render size: original 1600x900 -> with MetalFX -> native 1920x1080 -> native at that
   size. Without one: MetalFX upscaling of the picture to the window, on / off (the app starts it on, Odyssey Data/no-metalfx
   off). Odyssey draws its docked picture straight into the canvas the present shows: MetalFX works at the output stage
   (nvn_mtl.m output_upscale; nothing happens while the window is not bigger than the picture). */
int runtime_output_mode_cycle(void) {
    extern int mfxup_enabled(void);
    extern void dm_gfx_set_flag(const char *name, int value);
    if (g_render_w) {
        g_scene_mode = (g_scene_mode + 1) % scene_states();
        atomic_store(&g_scene_cap, g_scene_mode == scene_states() - 1 ? 0 : scene_size(g_scene_mode));   /* the largest: no cap */
        dm_gfx_set_flag("mfx_output", g_scene_mode == SCENE_ORIGINAL_MFX);
        dm_gfx_set_flag("mfx_upscale", g_scene_mode == SCENE_ORIGINAL_MFX);
        fprintf(stderr, "[odyssey] key 3: %s\n", runtime_picture_name());
        return 1;
    }
    int next = !mfxup_enabled();
    dm_gfx_set_flag("mfx_output", next);
    dm_gfx_set_flag("mfx_upscale", next);
    fprintf(stderr, "[odyssey] key 3: %s\n", next ? "MetalFX upscales the picture to the window" : "MetalFX off");
    return 1;
}
