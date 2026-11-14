/* game_patches.c —— Tears of the Kingdom 1.1.2 (v262144) 运行时游戏补丁，鸿蒙最小移植版（档案 host_override）。
   覆盖模板 game_patches.tmpl.c（弱符号兜底）——本文件必须提供宿主引用的全部钩子符号，故下方其余钩子
   全部以无操作实现收口。
   血统：MacNativeGames games/totk/runtime/game_patches.c（766 行）。本文件只移植**数据表驱动、自校验**
   的三块，其余特性列入"待 OHOS 化"清单（见档案 README）：
     已移植（位点出自 game_version.h 的 TOTK_GAME_V112 表；本机 ExeFS 与 Mac 1.1.2 逐字节同源）：
       - fps60_sites       7 条指令字替换：present-interval 下限 2（30fps）→1（60fps），逐条先校验原字
       - patch_slowmo_bounce  >100fps 蹦床慢动作误判修正（veneer 写入 main text padding +128，自含 branch）
       - patch_trap        诊断：TOTK_TRAP_AT=<off> 写 brk #0（真机验尸用）
     待 OHOS 化（不在本文件，勿盲拷 Mac 版）：
       - patch_realtime + patch_cloth_clock（真实帧步进 + 布料/毛发时钟；依赖 loader 在 main 后保留的
         0x4000 数据页——OHOS loader.c 已有该页（size=page_up(file_size)+0x4000），但整块补丁含 ADRP/
         veneer 与 present 线程集成，M3 真机阶段移植）
       - patch_render_res / patch_output_res（TOTK_RENDER_RES/TOTK_OUTPUT_RES 输出分辨率面）
       - patch_pchtxt（模拟器 pchtxt 补丁加载，Mac 文件系统路径语义）
       - aspect_patch.h 超宽屏（上游 1.1.2 本就不支持：仅 1.2.1/1.4.3）
       - world_survey.c（totk 变体的测试剖面）
   注意：fps60 运行时补丁与档案 patches.py 的重写器 FRAME_STEP 是**两条并行机制**（后者仅登记 1.4.3）；
   若未来为 1.1.2 登记了重写器帧步进，须删掉这里的 fps60 或用 TOTK_FPS60=0 关闭，勿双份叠加。 */
#include "game_patches.h"
#include "game_version.h"
#include "world_survey.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t offset; uint32_t original, replacement; const char *what; } GamePatch;

/* 60 FPS（Mac 上游 PR #61 前身 / FPSLocker 同族）：TOTK 的自适应 present-interval 控制器把主线程实测
   帧工作量换算成 60Hz tick 数并钳到 [min,max]，同时交给 nvnWindowSetPresentInterval 与变帧步进。
   出厂 min=2（30fps）。补丁把下限降到 1：控制器仍会在单帧工作量放不下时自动升到 2/3（30/20fps，带
   自家迟滞），慢场景以正确速度回落。位点经 GAME_ADDR() 取 1.1.2 偏移；每条先校验原指令字，任何一条
   不匹配即停机（不盲写）。TOTK_FPS60=0 关闭（保持出厂 2..3）。 */
static const GamePatch fps60_sites[] = {
    {GAME_ADDR(0x8f6c60), 0x6b08015f, 0x7100055f, "clamp: cmp w10, #1 (was cmp w10, w8 = minimum)"},
    {GAME_ADDR(0x8f6c64), 0x1a89b10a, 0x1a9fa52a, "clamp: csinc w10, w9, wzr, ge (desired = max(desired, 1))"},
    {GAME_ADDR(0x8f6c88), 0xb9401a8a, 0xd503201f, "lower trigger: nop the stored-candidate load (try lower when desired < current)"},
    {GAME_ADDR(0x8f6d18), 0x6b08015f, 0x7100055f, "raise path: cmp w10, #1"},
    {GAME_ADDR(0x8f6d28), 0x1a88c1ac, 0x1a9fc5ac, "raise path: csinc w12, w13, wzr, gt (next lower candidate floors at 1)"},
    {GAME_ADDR(0x8f6d7c), 0xb9400288, 0x52800028, "lower path: mov w8, #1 (minimum interval 1)"},
    {GAME_ADDR(0x8f6d98), 0x2a0a03f5, 0x1a9fa555, "lower path: csinc w21, w10, wzr, ge (result never below 1)"},
};

static int fps60_enabled(void) {
    const char *e = getenv("TOTK_FPS60");
    return !(e && !strcmp(e, "0"));
}

static uint32_t branch_to(uint64_t site, uint64_t target) {   /* b <target>, both offsets in main */
    int64_t delta = (int64_t)target - (int64_t)site;
    if ((delta & 3) || delta < -(1 << 27) || delta >= (1 << 27)) runtime_fail("patch branch outside range");
    return 0x14000000u | ((uint32_t)(delta >> 2) & 0x3ffffffu);
}

/* 高帧率修正（#61，天空岛蹦床）：出厂检查 X+0x28>100 在 >100fps 时把正常运行误判成时间减速，
   蹦床不弹。veneer 改比 X+0x28*X+0x38（=30/时间倍率，30Hz 下同值、与帧率无关）。
   veneer 放 main text padding +128（上游 1.2.1 ultrawide veneers 之后的位置；1.1.2 无 ultrawide，
   padding 从 text_end 起空）。TOTK_HIGHFPS_FIX=0 关闭。 */
static void patch_slowmo_bounce(Module *m) {
    const char *e = getenv("TOTK_HIGHFPS_FIX");
    if (e && !strcmp(e, "0")) return;
    const uint64_t site = GAME_ADDR(0xce3fec), stub = ((m->text_end + 15) & ~15ULL) + 128;
    if (stub + 20 > ((m->text_end + 15) & ~15ULL) + 0x400) runtime_fail("bounce veneer layout");
    static const uint32_t original[3] = {0xbd402900,   /* ldr s0, [x8, #0x28] */
                                         0x52a85908,   /* mov w8, #0x42c80000 (100.0) */
                                         0x1e270101};  /* fmov s1, w8 */
    for (unsigned i = 0; i < 3; i++) {
        uint32_t w; memcpy(&w, m->base + site + 4 * i, 4);
        if (w != original[i]) runtime_fail("bounce check: original instruction mismatch");
    }
    for (unsigned i = 0; i < 20; i++) if (m->base[stub + i]) runtime_fail("bounce veneer padding not empty");
    const uint32_t veneer[5] = {0xbd402900,   /* ldr s0, [x8, #0x28]: X+0x28 */
                                0xbd403901,   /* ldr s1, [x8, #0x38]: the 30 Hz ratio (the next original instruction sets s1 again) */
                                0x1e210800,   /* fmul s0, s0, s1 */
                                0x52a85908,   /* mov w8, #0x42c80000: the original 100.0 */
                                branch_to(stub + 16, site + 8)};
    for (unsigned i = 0; i < 5; i++) memcpy(m->base + stub + 4 * i, &veneer[i], 4);
    uint32_t jump = branch_to(site, stub);
    memcpy(m->base + site, &jump, 4);
    fprintf(stderr, "[totk] %s high-FPS fix: trampoline slow-motion check uses the time scale, veneer +%#llx\n",
            TOTK_GAME_VERSION, (unsigned long long)stub);
}

/* 诊断：TOTK_TRAP_AT=<main offset> 写 brk #0，崩在现场吐寄存器/调用链后退出（真机验尸用） */
static void patch_trap(Module *m) {
    const char *e = getenv("TOTK_TRAP_AT");
    if (!e || !*e) return;
    uint64_t off = strtoull(e, NULL, 0);
    if (off & 3 || off + 4 > m->text_end) runtime_fail("TOTK_TRAP_AT must be a 4-byte aligned offset in main's text");
    uint32_t brk = 0xd4200000, was;
    memcpy(&was, m->base + off, 4);
    memcpy(m->base + off, &brk, 4);
    fprintf(stderr, "[totk] TOTK_TRAP_AT: brk at main+%#llx (was %08x); the game stops there with a register dump\n", (unsigned long long)off, was);
}

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) runtime_fail("game patches expect the main module");
    patch_slowmo_bounce(m);
    patch_trap(m);
    if (!fps60_enabled()) {
        fprintf(stderr, "[totk] 60 FPS patch off (TOTK_FPS60=0): shipping present interval range 2..3\n");
        return;
    }
    const size_t n = sizeof fps60_sites / sizeof fps60_sites[0];
    for (size_t i = 0; i < n; i++) {   /* verify all before writing any */
        const GamePatch *p = &fps60_sites[i];
        if (p->offset + 4 > m->text_end) runtime_fail("60 FPS patch site outside the main text segment");
        uint32_t word; memcpy(&word, m->base + p->offset, sizeof word);
        if (word != p->original) {
            fprintf(stderr, "[totk] 60 FPS patch main+%#llx: found %08x, expected %08x (%s)\n",
                    (unsigned long long)p->offset, word, p->original, p->what);
            runtime_fail("60 FPS patch: main does not match the selected game version");
        }
    }
    for (size_t i = 0; i < n; i++) memcpy(m->base + fps60_sites[i].offset, &fps60_sites[i].replacement, 4);
    fprintf(stderr, "[totk] %s 60 FPS patch: %zu instructions (interval range 1..3)\n", TOTK_GAME_VERSION, n);
}

/* ---- 其余钩子面：无操作收口（待 OHOS 化清单里的特性落位时逐个替换；语义见模板 game_patches.tmpl.c） ---- */
void runtime_frame_step_pin(double ms) { (void)ms; }                 /* real-time frame step 未移植（见文件头） */
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }   /* TOTK_OUTPUT_RES 未移植 */
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_render_pool_extra(void) { return 0; }
const char *runtime_picture_name(void) { return 0; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_aspect_toggle(void) { return 0; }                        /* 1.1.2 无 ultrawide（上游同） */
_Atomic int runtime_survey_pending;                                  /* world_survey 未移植：默认全拒绝 */
const int32_t runtime_survey_sync[2] = {0, -1};
void runtime_survey_init(Module *main_module) { (void)main_module; }
int runtime_survey_command(const char *line) { (void)line; return 0; }
void runtime_survey_tick(uint64_t frame) { (void)frame; }
void runtime_survey_cancel_user(void) {}
