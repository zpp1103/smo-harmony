// M43A / S2a：录制层搬入后**唯一还允许为空**的一组符号，全部有明确的归属单据。
//
// 判读方法：S2a 落地前的 ohos_s2_stubs.c 里，exec 层 8 个函数 + 8 个诊断全局是「S1 切片故意留空」；
// 本单把 hle_nvn.c / gfx_vk_exec.c / gfx_common.c 真身搬入后，那些符号**全部有了真实定义**，
// ohos_s2_stubs.c 已删除。剩下的只可能是下面三类，且都不在录制/回放主链上：
//
//   A. present 层（gfx_vk_present.c）—— 归属 S4，本单明确不搬（与窗口/交换链/上屏绑定）。
//      录制侧从不调它们：hle_nvn 的录制函数只写命令流；提交/present 仍走老路 nvn_vk.c。
//   B. darwin host 的 crash 钩子（hle_nvn 只在崩溃/诊断分支里用）—— 归属宿主运行时，本单不接。
//   C. ~~nvn_thunks.S / nvn_entry.S 的跳板表~~ —— **M43B / S2b-1 已落地真身**：
//      nvn_thunks_elf.S（537 条跳板 + nvn_thunk[] + nvn_names[]）与 nvn_entry_elf.S
//      （nvn_unimpl_entry / nvn_prof_entry / nvn_big_entry）由 nx2 gen_protos.py + darwin2elf.py 生成，
//      本文件里对应的 5 个强符号占位**已删除**（真身进来后若还留着会撞重复定义）。
//      仍然不接执行：nvn_slot[].fn 全 0（constructor 停用），跳板只是落进链接、nm 可见。
//
// 与 S1 的 ohos_s2_stubs.c 的关键差别：那些桩是 `__attribute__((weak))`——它会在「真身缺席」时静默顶上，
// 让调用看起来成功。这里一律**强符号**：真身一旦搬入就会撞重复定义、当场编译/链接失败，不会静默。
#include <stddef.h>
#include <stdint.h>

#include "dm.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"

// ---------- A. present 层（S4）----------
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) { (void)t; (void)period_s; (void)crop; return 0; }
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
uint32_t mtl_present_qos(void) { return 0; }
double mtl_presented_time(uint64_t n) { (void)n; return 0.0; }
int mtl_has_window(void) { return 0; }
int mtl_fps_target(void) { return 60; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
int mtl_display_hz(void) { return 0; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_prog_release(prog_t *p) { (void)p; }

// ---------- B. 宿主崩溃诊断钩子 ----------
int  crash_addr_str(char *b, size_t n, uint64_t a) { (void)a; if (n) b[0] = 0; return 0; }
void crash_thread_name(unsigned t, char *b, size_t n) { (void)t; if (n) b[0] = 0; }
void crash_add_note_hook(void (*fn)(void)) { (void)fn; }

// ---------- B2. darwin_symbols.c 未进编译单元后留下的一个符号 ----------
// thread_selfcounts()：Darwin 私有调用（本线程累计指令/周期），平台层没有等价物；nx2 的实现在
// dm/android/compat/darwin_symbols.c 里也是「返回 -1 = 不可用」。这里同样返回 -1，调用方已按不可用处理。
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; (void)buf; (void)nbytes; return -1; }

// ---------- C. 跳板表占位 —— M43B 起**已删除**（真身：nvn_thunks_elf.S / nvn_entry_elf.S / nvn_bigstack_elf.c）----------
// nvn_thunk[] / nvn_names[] / nvn_unimpl_entry / nvn_prof_entry / nvn_big_entry 现在全部来自生成件。
