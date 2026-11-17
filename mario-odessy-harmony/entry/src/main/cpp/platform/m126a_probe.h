/* m126a_probe.h — M126A（2026-11-14）RX 权限判决探针 / seal 明文取证 / 直接 exec mmap 变体的公共声明。
 *
 * 只用 extern 声明（同 loader.c 里 smo_obs_rx_range / smo_obs_gate_fatal 的风格），
 * 不把 knob 语义塞进任何公共头；实现全在 platform/m126a_probe.c（CMake 的
 * CMake 的 platform 目录 .c 通配会自动收编，不需要改 CMakeLists）。
 *
 * 两个 knob（都是 app 自己先建文件，hdc 只能覆盖已有文件；缺省值写在文件里）：
 *   smo_rxprobe   缺省 1：探针开启即打印；写 0 = 一条探针行都不打（判据 5 的关态口径）。
 *   smo_exec_mmap 缺省 0：=1 时 map_module 改成「文件背衬 + 创建即 R|W|X」，失败回落 build #7 旧路径。
 */
#ifndef M126A_PROBE_H
#define M126A_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* knob 读点（一次性缓存；首次读会打一行 seed） */
int m126a_rxprobe_on(void);
int m126a_exec_mmap_on(void);

/* 极早探针：anon RW→RX / anon 创建即 X / 文件背衬 R→RX / 文件背衬 RW→RX / 文件背衬创建即 X，
   外加一行 verdict。整个进程只跑一次（内部 once 守卫），第一行就打开 vk_boot.log。 */
void m126a_rxprobe_early(void);

/* seal() 取证：mprotect 的精确入参（addr/len/prot 全十六进制）+ rc/errno + mmap 返回值；
   rc != 0 时立即 dump 该窗口附近 /proc/self/maps（最多 8 行，另打命中总数）。 */
void m126a_seal_args(const char *what, uint64_t addr, uint64_t len, unsigned prot,
                     int rc, int err, uint64_t mmap_base);

/* 任意窗口的 /proc/self/maps dump（最多 8 行）。 */
void m126a_maps_dump(const char *tag, uint64_t addr, uint64_t len);

/* smo_exec_mmap=1 的映射结果行：mod / path / mmap 返回值 / len / prot / ok / errno / fallback。 */
void m126a_execmap_note(const char *mod, const char *path, uint64_t base, uint64_t len,
                        unsigned prot, int ok, int err, int fallback, uint64_t reserve);

/* smo_exec_mmap=1 的收口行：fallback 次数（映射创建失败回落旧路径）/ 降权失败次数。 */
void m126a_execmap_summary(int fallback, int demote_fail);

#ifdef __cplusplus
}
#endif

#endif /* M126A_PROBE_H */
