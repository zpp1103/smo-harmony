#ifndef SHADER_DUMP_H
#define SHADER_DUMP_H
#include <stdint.h>
/* M3p：把 nvnProgramSetShaders 的真实 shader 字节按 sha256(ctl||code)[:16] 落到
 * <vk_boot.log 所在目录>/shaderdump/<key>.<stage>.{ctl,code}，并 append index.txt。
 * 入参 sd 指向 NVNshaderData 数组（{u64 data; const void *control;}，16 字节/项，nvn.h:148-151）。
 * 全部失败路径只计数/落日志，不抛错、不改返回值——渲染路径不受影响。 */
void shader_dump_program(int n, const void *sd);
/* 结束时打一条 totals（可选；周期行已在每 100 次调用落一条）。 */
void shader_dump_report(void);
/* ---- M3r（2026-11-03）：运行时查表用的同一套 key ----
 * shader_key(ctl, code_addr, out)：校验 + sha256(ctl[0..+0x20])||code[0..+0x704] 前 8 字节 → 16 hex。
 * 返 stage(0/1/5)，失败（magic/长度/头不符）返 -1 且不写 out。判据与落盘侧共用 sd_validate。
 * shader_dump_key_at(sd, i, prog)：从 NVNshaderData 数组第 i 项取 ctl/code 再算 key（i 越界/坏数据返 -1）。
 *   M7D3：`prog` = nvnProgramSetShaders 的 program 指针，仅用于 [m7f] 探针行（smo_m7f=1 时）。 */
int shader_key(const void *ctl, uint64_t code_addr, char out[17]);
int shader_dump_key_at(const void *sd, int i, uint64_t prog, char out[17]);
/* M3v P2：key_bad（shader_key 失败次数）按「原始 stage 字段」与「首个不满足的判据」两维分解。 */
void shader_dump_keybad_report(void);

/* ---- M92A（2026-11-13）：缺失 shader dump（真机 3D 段 nokey 的那 68 对） ----
 * `smo_m92a` 位掩码，缺省 0 = 逐字节旧行为：
 *   bit0 = r_bin_spv 的键比较 15 → 16（比较本体在 nvn_vk.c，用本函数选长度）；
 *   bit1 = 缺失 shader dump：SetShaders 期把「语料里没有」的 (key, ctl, code) 暂存进内存，
 *          draw 判 nokey 时写成 <smo_m92a_dir>/<key>.<stage>.m92a（128 B 自描述头 + ctl + code），
 *          每键只写一次（去重）；同目录 m92a_index.txt / m92a_pairs.txt 为文本台账；
 *   bit2 = 只统计不落盘（只打 `[m92a] stats dumped/pairs/dropped`）。
 * `smo_m92a_dir`：输出目录，缺省 files/shader_dump92（相对路径按 /data/storage/el2/base 解析）。
 * 文件格式见 shader_dump.c 的 M92A 注释块（魔数 "M92ADMP1"）。 */
int  shader_dump_m92a_bits(void);
/* nokey 的 draw：vs/fs = 该 program 的两把键，rt_key = 目标 RT 键（g_rt_cur->key）。
 * 只读、只计数/落盘；**不做任何 RT 绑定或布局切换**；knob=0 时立即返回。 */
void shader_dump_missing_note(const char *vs, const char *fs, uint64_t rt_key, int count);
#endif
