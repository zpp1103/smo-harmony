#include "world_survey.h"

/* The TotK world survey reads TotK's actor and scene structures; Bayonetta 3 has none of them. Commands are refused.
   分线后全部符号改弱定义（平台默认空桩）：TotK 的真实现放 games/<tid>/ 同名强符号即可覆盖，platform 零改动。 */
__attribute__((weak)) _Atomic int runtime_survey_pending;
__attribute__((weak)) const int32_t runtime_survey_sync[2] = {0, -1};   /* read by hle_nvn.c only while a survey is pending, which never happens here */
__attribute__((weak)) void runtime_survey_init(Module *main_module) { (void)main_module; }
__attribute__((weak)) int runtime_survey_command(const char *line) { (void)line; return 0; }
__attribute__((weak)) void runtime_survey_tick(uint64_t frame) { (void)frame; }
__attribute__((weak)) void runtime_survey_cancel_user(void) {}
