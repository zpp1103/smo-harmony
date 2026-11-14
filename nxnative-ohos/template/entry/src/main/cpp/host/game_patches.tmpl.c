/* game_patches.tmpl.c —— 游戏钩子空实现（模板兜底，设计 §4.6 / §7）。
   模板构建时由 scripts/build_hap_from_profile.py 重命名为 game_patches.c 编入；
   档案带 host_override/host/{game_patches.c,odyssey_aspect.c,world_survey.c,game_version.h}
   时整文件覆盖（逃生门：宿主 runtime 分叉度 ~65%，模板不可能参数化覆盖全部）。
   全部符号 weak：generic 宿主（loader/input/services/romfs）只认 runtime.h /
   game_patches.h / world_survey.h 声明的钩子面，覆盖文件的强符号自动胜出。 */
#include "game_patches.h"
#include "game_version.h"
#include "world_survey.h"
#include <stdatomic.h>
#include <stdint.h>

/* ---- game_patches.h：main 模块装载后的 per-build 指令补丁（odyssey 走 host_override） ---- */
__attribute__((weak)) void runtime_game_patches(Module *main_module) { (void)main_module; }

/* ---- runtime.h：帧步进 / 输出分辨率面 ---- */
__attribute__((weak)) void runtime_frame_step_pin(double ms) { (void)ms; }
__attribute__((weak)) void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
__attribute__((weak)) int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
__attribute__((weak)) uint64_t runtime_output_res_extra_bytes(void) { return 0; }
__attribute__((weak)) uint64_t runtime_render_pool_extra(void) { return 0; }
__attribute__((weak)) const char *runtime_picture_name(void) { return 0; }
__attribute__((weak)) int runtime_output_res_toggle(void) { return 0; }
__attribute__((weak)) int runtime_output_mode_cycle(void) { return 0; }
__attribute__((weak)) uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }

/* ---- runtime.h：画面比例切换（odyssey_aspect.c 的钩子位） ---- */
__attribute__((weak)) int runtime_aspect_toggle(void) { return 0; }

/* ---- world_survey.h：测试剖面控制（默认全拒绝，同 MacNativeGames 非 TotK 游戏） ---- */
__attribute__((weak)) _Atomic int runtime_survey_pending;
__attribute__((weak)) const int32_t runtime_survey_sync[2] = {0, -1};
__attribute__((weak)) void runtime_survey_init(Module *main_module) { (void)main_module; }
__attribute__((weak)) int runtime_survey_command(const char *line) { (void)line; return 0; }
__attribute__((weak)) void runtime_survey_tick(uint64_t frame) { (void)frame; }
__attribute__((weak)) void runtime_survey_cancel_user(void) {}
