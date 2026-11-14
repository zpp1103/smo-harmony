/* game_id.h —— 游戏身份单一常量（模板参数化核心，设计 §4.4）。
   本文件由 scripts/build_hap_from_profile.py 实例化：{{...}} 占位来自 games/<titleID>/game.tsv。
   cpp 侧所有游戏身份引用一律 include 本头，不得散落硬编码：
     GAME_ID_DIR    沙箱/rawfile 里的游戏目录名（title_id，如 "0100000000010000"）
     GAME_TITLE_ID  svcGetInfo(ProgramId) 应答值（program_id 键）
     GAME_LOG_TAG   hilog 标签（run_cycle.sh 验尸 grep 依赖，smo 沿用 "SmoGame"）
   ets 侧（EntryAbility/Index）无法 include C 头，用同值的 {{GAME_ID_DIR}} 常量占位替换。 */
#pragma once
#define GAME_ID_DIR "{{GAME_ID_DIR}}"
#define GAME_TITLE_ID {{GAME_TITLE_ID}}ULL
#define GAME_LOG_TAG "{{GAME_LOG_TAG}}"
