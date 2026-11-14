/* Bind the donor graphics interface to this native runtime, without donor game hooks. */
#include "runtime.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>          /* M40AH：每 20 s 扫 /proc/self/task/*/stat 做线程 CPU 归因 */
#include <sys/resource.h>    /* M40AH：getrusage(RUSAGE_SELF) 取进程 CPU 增量（utime+stime） */
#include <unistd.h>          /* M40AH：sysconf(_SC_CLK_TCK) / access */
#include <fcntl.h>           /* M40AH：seeder 的 open(O_CREAT) */
#include "../third_party/d2r_gfx/gfx/nvn/nvn_mtl.h"
#include "../third_party/d2r_gfx/gfx/nvn/nvn_tex.h"   /* nvn_texdesc/nvn_texlayout + nvn_tex_layout（platform/nvn_tex.c，round15 拷自 donor） */
#include "../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"   /* M4v P1：块线性解排布几何/寻址（探针对照用；真身同 nvn_vk.c 引用的一份） */
#include "nvn_vk/nvn_vk.h"   /* round23批1落点3：present 链出图口（nvn_vk_present_clear；arm64 真身，x86_64 壳不编本文件） */
#include "vk_boot.h"         /* M3g：四个探针计数的原子变量（同 nvn_vk.c 用的那一组） */
#include <stdatomic.h>
#include "nvn_replay/cmd_trace.h"   /* 改动 3：opcode 直方图（nvn_cmd_trace / nvn_cmd_trace_report，契约头见该文件） */
#include "shader_dump.h"    /* M3p：ProgramSetShaders 真语料落盘（平台/*.c 被 CMake glob 收编） */
#ifdef __MUSL__
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
/* M4o P2：安全读 4 字节（PROT_NONE/未映射返 EFAULT 而不 fault）。用途：侧表漏登记的 RT 认亲
   —— 读 tex0 首字是不是我们自己的 tex_h.magic。m4o_b1 证据：VK 起后 ~16s 侧表(1024)就满了，
   之后所有 SET_RENDER_TARGETS 的 tex0 都 miss，1024×1024/UI 目标全被塞回 1280×720 宿主图。 */
static int rt_peek_u32(uint64_t a, uint32_t *out)
{
 if (a < 0x100000000ULL || a >= 0x800000000000ULL) return 0;
 struct iovec l = { out, 4 }, r = { (void *)(uintptr_t)a, 4 };
 return syscall(__NR_process_vm_readv, getpid(), &l, 1ul, &r, 1ul, 0ul) == 4;
}
/* M4q P1：同款安全读 8 字节（第四级「顺着指针再解一层」用）。 */
static int rt_peek_u64(uint64_t a, uint64_t *out)
{
 if (a < 0x100000000ULL || a >= 0x800000000000ULL) return 0;
 struct iovec l = { out, 8 }, r = { (void *)(uintptr_t)a, 8 };
 return syscall(__NR_process_vm_readv, getpid(), &l, 1ul, &r, 1ul, 0ul) == 8;
}
/* M6b b【修】：整段可读校验。boot B 的 SIGSEGV(SEGV_ACCERR)@0x6de939ca44 就死在
   h_rt_lookup 的 magic 快路：只验了前 4 字节 magic，随后**直接解引用** tex_h 的 d.width 等字段；
   候选对象尾部落在已取消映射的页上 ⇒ 读越界崩。这里一次 0xe0 字节读把整块证明可读，
   之后同一调用内的字段解引用才是安全的。 */
static int rt_peek_span(uint64_t a, void *buf, size_t n)
{
 if (a < 0x100000000ULL || a >= 0x800000000000ULL) return 0;
 struct iovec l = { buf, n }, r = { (void *)(uintptr_t)a, n };
 return syscall(__NR_process_vm_readv, getpid(), &l, 1ul, &r, 1ul, 0ul) == (ssize_t)n;
}
#endif
typedef struct{const char *name;void *fn;const char *why;} GfxEntry;
/* 改动 2（2026-11-02）：nvn_replay/glue_cmdbuf.c 的录制族入口。参数顺序照真实 NVN ABI
   （third_party/d2r_gfx/gfx/nvn/nvn_protos.h:330-345/355-396/434），实现与 A(i) 对齐见 glue_cmdbuf.c。 */
extern uint64_t nvn_rec_command_buffer_initialize(void *cb,void *device);
extern uint64_t nvn_rec_command_buffer_add_command_memory(void *cb,const void *pool,int64_t off,uint64_t size);
extern uint64_t nvn_rec_command_buffer_add_control_memory(void *cb,void *mem,uint64_t size);
extern void nvn_rec_begin_recording(void *cb);
extern uint64_t nvn_rec_end_recording(void *cb);
extern void nvn_rec_draw_arrays(void *cb,int mode,int first,int count);
/* M5p C：DrawArraysInstanced 的 6 参录制搬运（nvn_protos.h:380：cb,mode,first,count,baseInstance,instances） */
extern void nvn_rec_draw_arrays_instanced(void *cb,int mode,int first,int count,int base_instance,int instances);
extern void nvn_rec_draw_elements(void *cb,int mode,int type,int count,const void *idx);
extern void nvn_rec_draw_elements_base_vertex(void *cb,int mode,int type,int count,const void *idx,int base_vertex);
extern void nvn_rec_draw_elements_instanced(void *cb,int mode,int type,int count,const void *idx,int base_vertex,int base_instance,int instances);
extern void nvn_rec_set_render_targets(void *cb,int n,const void *const *colors,const void *const *views,const void *depth,const void *depth_view);
extern void nvn_rec_clear_color(void *cb,int index,const float *color,int mask);
extern void nvn_rec_bind_program(void *cb,void *program,int stage_mask);
extern void nvn_rec_bind_vertex_buffers(void *cb,int first,int count,const void *ranges);
/* M3r（2026-11-03）：单条绑定形式也进命令流——顶点地址的唯一来源（SMO 走的是单条形式） */
extern void nvn_rec_bind_vertex_buffer(void *cb,int first,uint64_t addr,uint64_t size);
extern void nvn_rec_bind_uniform_buffer(void *cb,int stage,int index,uint64_t addr,uint64_t size);
extern void nvn_rec_bind_texture(void *cb,int stage,int slot,uint64_t handle);
extern void nvn_rec_bind_textures(void *cb,int stage,int first,int count,const void *handles);
extern void nvn_rec_bind_uniform_buffers(void *cb,int stage,int first,int count,const void *ranges);
extern void nvn_rec_bind_storage_buffer(void *cb,int stage,int index,uint64_t addr,uint64_t size);            /* M5x */
extern void nvn_rec_bind_storage_buffers(void *cb,int stage,int first,int count,const void *ranges);          /* M5x */
extern void nvn_rec_set_viewports(void *cb,int first,int count,const float *rects);
extern void nvn_rec_set_scissors(void *cb,int first,int count,const int *rects);
/* M4an P2：四个单数拼写状态设置（glue_cmdbuf.c wrapper → cmdbuf.c:454/462/470/476 同组 opcode） */
extern void nvn_rec_set_viewport(void *cb,int x,int y,int w,int h);
extern void nvn_rec_set_scissor(void *cb,int x,int y,int w,int h);
extern void nvn_rec_set_depth_range(void *cb,float n,float f);
extern void nvn_rec_set_blend_color(void *cb,const float *rgba);
/* M3i（2026-11-03）：5 个「只记账、不进命令流」的状态绑定补录制（glue_cmdbuf.c 新 wrapper，
   录制端 cmdbuf.c:316-326 早已存在）。参数顺序照 nvn_protos.h:347-354。 */
extern void nvn_rec_bind_blend_state(void *cb,const void *state);
extern void nvn_rec_bind_color_state(void *cb,const void *state);
extern void nvn_rec_bind_depth_stencil_state(void *cb,const void *state);
extern void nvn_rec_bind_vertex_attrib_state(void *cb,int count,const void *states);
extern void nvn_rec_bind_vertex_stream_state(void *cb,int count,const void *states);
extern uint64_t nvn_rec_dropped_calls(void);
extern uint64_t nvn_rec_bad_cb(void);   /* M3i：cb 非指针被拒录制次数（对账，见 glue_cmdbuf.c put） */
/* M3s：顶点状态全量计数（判定「游戏真传零 vs 我们搬错」的量化依据）与 UBO 上传统计。 */
extern void nvn_rec_vertex_totals(unsigned long long *ac,unsigned long long *az,unsigned long long *an,
                                  unsigned long long *sc,unsigned long long *sz,unsigned long long *sn);
extern void nvn_vk_bind_uniform_buffers(int stage,int first,int count,const void *ranges);
extern void nvn_vk_bind_storage_buffers(int stage,int first,int count,const void *ranges);   /* M5x */
/* M14B（2026-11-08）：NVN blend / color state 的回放侧入口（nvn_vk.c）。
   返回 0 = 已受理（哪怕内容非法），非 0 = 本层不认（当前恒 0，签名留返回值便于以后否决）。
   只传**录制时刻**拷贝的那份字节（cmd_trace.c 的 extra），不传指针让 nvn_vk 去现读客体。 */
extern int nvn_vk_bind_blend_state(uint64_t guest_ptr,const void *bytes,unsigned nbytes);
extern void nvn_vk_m14b_ensure(void);   /* M14B：开关文件落盘（首次运行建 0666 空文件） */
extern int  nvn_vk_m14b_on(void);       /* M14B：开关值（0 = setter 与空桩等价） */
extern int nvn_vk_bind_color_state(uint64_t guest_ptr,const void *bytes,unsigned nbytes);
static int g_rp_bind_blend(uint64_t gp,const void *b,unsigned n,void *u);   /* M14B：定义在本文件 sink 区之前 */
static int g_rp_bind_color(uint64_t gp,const void *b,unsigned n,void *u);
extern void nvn_vk_ubo_stats(unsigned long long *binds,unsigned long long *up,unsigned long long *trunc);
extern unsigned long long nvn_vk_ubo_stage_count(int s);
extern void nvn_vk_m82a_stats(unsigned long long *clamped,unsigned long long *partial,unsigned long long *fail,unsigned long long *bytes,unsigned long long *rb);   /* M82A */
extern int nvn_vk_m82a_mode(void);   /* M82A */
extern void nvn_vk_m86a_stats(unsigned long long *two_step,unsigned long long *win,unsigned long long *bytes,unsigned long long *trunc,unsigned long long *fail);   /* M86A */
extern int nvn_vk_m86a_mode(void);   /* M86A */
extern void nvn_vk_m86a_report(void);   /* M86A：bit1 探针（内部 60 s 节流，关态零输出） */
extern uint64_t nvn_rec_cmdmem_readable(const void *p);   /* nvn_replay/glue_cmdbuf.c：池 chunk 表 + shadow 页块（task A/C） */
/* 第 2 步（2026-11-02）：记账/生命周期族也指向 cmdbuf，客体偏移 0 = 宿主 NvnCmdBuf* 成为唯一事实 */
extern void nvn_rec_command_buffer_finalize(void *cb);
extern uint64_t nvn_rec_get_command_memory_size(void *cb);
extern uint64_t nvn_rec_get_command_memory_used(void *cb);
extern uint64_t nvn_rec_get_command_memory_free(void *cb);
extern uint64_t nvn_rec_get_control_memory_size(void *cb);
extern uint64_t nvn_rec_get_control_memory_used(void *cb);
extern uint64_t nvn_rec_get_control_memory_free(void *cb);
extern uint64_t nvn_rec_is_recording(void *cb);
uint64_t g_t0_ns;
/* M3u（2026-11-03）统计计数器——定义在此是因为周期报告 nvn_h_report 在文件前部就要引用。 */
static unsigned long long g_ub_n,g_ub_ok,g_ub_bad,g_ub_bytes;   /* P1 UpdateUniformBuffer 落地口径 */
static unsigned long long g_pg_calls,g_pg_key_ok,g_pg_key_bad,g_pg_vs_only,g_pg_fs_only,g_pg_both;   /* P3 program→key 六计数 */

uint64_t now_ns(void){return runtime_now_ns();}
uint64_t guest_ns(void){return runtime_now_ns();}
void *module_of(const void *p){(void)p;return NULL;}
static int (*fault_hook)(void *,int,void *);
static void (*prepare_hook)(void *,size_t);
static void (*report_hook)(uint32_t,const char *);
void dm_fault_hook_register(int (*fn)(void *,int,void *)){if(fault_hook)runtime_fail("second gfx fault hook");fault_hook=fn;}
void dm_guest_write_prep_register(void (*fn)(void *,size_t)){if(prepare_hook)runtime_fail("second gfx prepare hook");prepare_hook=fn;}
void dm_res_report_register(void (*fn)(uint32_t,const char *)){report_hook=fn;(void)report_hook;}
extern uint64_t dm_host_write_pc;   /* nvn_dirty.c: writer of a host write, for its DM_NVN_TILELOG diagnostic */
void dm_guest_write_prepare(void *addr,size_t n){dm_host_write_pc=(uint64_t)__builtin_return_address(0);if(prepare_hook)prepare_hook(addr,n);}
/* TOTK diagnostic write watchpoint: TOTK_WATCH_ADDR=<hex guest address> [TOTK_WATCH_FRAME=<first frame>, default 300].
   From that frame on, the 16 KiB page holding the address is made read-only after every present; a guest write faults,
   the writer (pc / lr / frame chain as module+offset) is logged, the page is unprotected and the write re-executes. */
#include <sys/mman.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <sys/ucontext.h>
/* round3 自旋期取证：单地址槽 → 双槽。客体地址空间每次运行整体漂移（round1↔round2 实测两候选
   同偏 -0xa00000：F-I 0x6e46393218→0x6e45993218、loader 0x6e045a0d04→0x6e03ba0d04），env/绝对地址
   硬编码都会脱靶；改为宿主打点处动态挂 watch（runtime_watch_add，threads.c F-I/F-H）。env 槽（槽 0）
   仍由 TOTK_WATCH_ADDR(/TOTK_WATCH_ADDR2) 填，动态槽 from=0（挂上即武装——静默期 present 停滞、
   runtime_watch_tick 不再跑也不失效：rearm 线程持续保护页面）。 */
typedef struct{uintptr_t lo,hi,addr,end;long from;int hits,read;}watch_t;
static watch_t watch_s[2];static int watch_n,watch_init;
static long watch_from=300;static int watch_read_env;
static unsigned char watch_snap[2][32];static int watch_snap_valid[2];static _Atomic int watch_armed;   /* TOTK: content poll at every present + periodic re-arm (an unrelated write in the page unprotects it; without re-arm the block's own write in the same frame is missed) */
static void *watch_rearm_thread(void *arg){(void)arg;for(;;){usleep(100);if(atomic_load(&watch_armed))for(int i=0;i<watch_n;i++)if(watch_s[i].lo)mprotect((void *)watch_s[i].lo,watch_s[i].hi-watch_s[i].lo,watch_s[i].read?PROT_NONE:PROT_READ);}return NULL;}
int runtime_module_offset(uint64_t pc,const char **name,uint64_t *off);
static void watch_hex(char *b,size_t *n,const char *label,uint64_t v){while(*label)b[(*n)++]=*label++;b[(*n)++]='0';b[(*n)++]='x';for(int i=15;i>=0;i--)b[(*n)++]="0123456789abcdef"[(v>>(i*4))&15];}
static void watch_sym(char *b,size_t *n,const char *label,uint64_t pc){const char *nm;uint64_t off;while(*label)b[(*n)++]=*label++;if(runtime_module_offset(pc,&nm,&off)){while(*nm)b[(*n)++]=*nm++;watch_hex(b,n,"+",off);}else watch_hex(b,n,"host:",pc);}
/* TOTK: watch a guest range [addr, addr+len); TOTK_WATCH_READ=1 also catches reads (pages become inaccessible after each present).
   Set from the environment (TOTK_WATCH_ADDR) or at runtime by the graphics layer (DM_NVN_WATCH_SB: a storage buffer bound to a given shader). */
int totk_vpool_backing(uintptr_t va,uint64_t len,uintptr_t *cpu);
/* 挂一个 watch 槽（READ/WRITE 由 read 参数定；threads.c F-I/F-H 打点处调用，from=0 立即武装）。 */
void runtime_watch_add(uintptr_t addr,size_t len,int read){
#ifdef __MUSL__
 /* round3 实测（emu_round3.log + cppcrash-9776）：OHOS DFX sigchain 截获 SIGSEGV，页面保护 fault 的
    「handler 解除保护后恢复执行」路径不成立——首个越窗 fault（0x6e459931b0，屏障同页）28ms 后即被
    DFX 判死整进程。mprotect 型 watch 在本设备一律禁用（macOS 宿主不受影响），改由 threads.c 的
    SIGUSR2 自旋采样器取证（spin_cap_handler）。此处仅登记地址留痕。 */
 { static int said; if(!said){said=1;char b[160];size_t n=0;watch_hex(b,&n,"[watch] mprotect watch DISABLED on OHOS (DFX kills on page fault, round3); log-only addr=",addr);b[n++]=10;write(2,b,n);} }
 return;
#else
 if(watch_n>=2)return;   /* 双槽满：F-I/F-H 各占一，env 槽与其互斥（先到先得） */
 { uintptr_t cpu=0; if(totk_vpool_backing(addr,len?len:256,&cpu)&&cpu){char b[160];size_t n=0;watch_hex(b,&n,"[watch] virtual pool address ",addr);watch_hex(b,&n," -> CPU backing ",cpu);b[n++]=10;write(2,b,n);addr=cpu;} }
 watch_t *s=&watch_s[watch_n];
 s->addr=addr;s->end=addr+(len?len:256);s->lo=addr&~(uintptr_t)0x3fff;s->hi=(s->end+0x3fff)&~(uintptr_t)0x3fff;s->hits=0;s->from=0;s->read=read;
 { char b[160];size_t n=0;watch_hex(b,&n,read?"[watch] READ slot armed addr=":"[watch] slot armed addr=",addr);watch_hex(b,&n," end=",s->end);watch_hex(b,&n," pages=",(s->hi-s->lo)>>14);b[n++]=10;write(2,b,n); }
 { static int started;if(!started){started=1;pthread_t t;pthread_create(&t,NULL,watch_rearm_thread,NULL);} atomic_store(&watch_armed,1); }
 mprotect((void *)s->lo,s->hi-s->lo,read?PROT_NONE:PROT_READ);
 watch_n++;
#endif
}
void runtime_watch_set(uintptr_t addr,size_t len){ if(watch_n==0)runtime_watch_add(addr,len,watch_read_env); }
void runtime_watch_tick(uint64_t frame){
 if(!watch_init){watch_init=1;const char *e=getenv("TOTK_WATCH_ADDR");const char *f=getenv("TOTK_WATCH_FRAME");if(f)watch_from=atol(f);watch_read_env=getenv("TOTK_WATCH_READ")!=NULL;
  if(e){const char *l=getenv("TOTK_WATCH_LEN");runtime_watch_set((uintptr_t)strtoull(e,NULL,16),l?(size_t)strtoull(l,NULL,0):256);} if(e)watch_s[0].from=watch_from; }
 for(int i=0;i<watch_n;i++){ if(!watch_s[i].lo||(long)frame<watch_s[i].from||watch_s[i].hits>=200)continue;
  unsigned char cur[32];memcpy(cur,(const void *)watch_s[i].addr,32);
  if(!watch_snap_valid[i]||memcmp(cur,watch_snap[i],32)){char b[256];size_t n=0;watch_hex(b,&n,"[watch] content at present frame=",frame);for(int k=0;k<32;k+=4){uint32_t w;memcpy(&w,cur+k,4);b[n++]=' ';for(int d=7;d>=0;d--)b[n++]="0123456789abcdef"[(w>>(d*4))&15];}b[n++]=10;write(2,b,n);memcpy(watch_snap[i],cur,32);watch_snap_valid[i]=1;}
  { static int started;if(!started){started=1;pthread_t t;pthread_create(&t,NULL,watch_rearm_thread,NULL);} atomic_store(&watch_armed,1); }
  if(mprotect((void *)watch_s[i].lo,watch_s[i].hi-watch_s[i].lo,watch_s[i].read?PROT_NONE:PROT_READ)!=0){char b[96];size_t n=0;watch_hex(b,&n,"[watch] mprotect failed page=",watch_s[i].lo);b[n++]=10;write(2,b,n);} }
}
static int watch_fault(void *addr,int is_write,void *context){
 uintptr_t a=(uintptr_t)addr;
 for(int i=0;i<watch_n;i++){ if(!watch_s[i].lo||a<watch_s[i].lo||a>=watch_s[i].hi)continue;
 if(a<watch_s[i].addr||a>=watch_s[i].end){mprotect((void *)watch_s[i].lo,watch_s[i].hi-watch_s[i].lo,PROT_READ|PROT_WRITE);return 1;}   /* other data in the pages: silently unprotect (re-armed at the next present) */
 ucontext_t *u=context;char b[1024];size_t n=0;
 { extern uint64_t g_frame_pub; extern _Atomic int dm_submit_busy; uint64_t th=0; pthread_threadid_np(NULL,&th);
   watch_hex(b,&n,"[watch] F",g_frame_pub);watch_hex(b,&n," t_us=",clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000);watch_hex(b,&n," th=",th);watch_hex(b,&n," submit_busy=",(uint64_t)atomic_load(&dm_submit_busy)); }
 watch_hex(b,&n,is_write?" write hit addr=":" read hit addr=",a);watch_hex(b,&n," off=",a-watch_s[i].addr);watch_hex(b,&n," slot=",i);
#ifdef __MUSL__   /* OHOS: musl aarch64 mcontext 为值类型，regs[31]/sp/pc */
 watch_sym(b,&n," pc=",u->uc_mcontext.pc);watch_sym(b,&n," lr=",u->uc_mcontext.regs[30]);
 uint64_t fp=u->uc_mcontext.regs[29];
#else             /* macOS: Apple arm64 mcontext 指针风格 __ss */
 watch_sym(b,&n," pc=",u->uc_mcontext->__ss.__pc);watch_sym(b,&n," lr=",u->uc_mcontext->__ss.__lr);
 uint64_t fp=u->uc_mcontext->__ss.__fp;
#endif
for(int d=0;d<8&&fp&&!(fp&15);d++){uint64_t fr[2];mach_vm_size_t got=0;if(mach_vm_read_overwrite(mach_task_self(),fp,16,(uintptr_t)fr,&got)!=KERN_SUCCESS||got!=16)break;watch_sym(b,&n," <",fr[1]);fp=fr[0];}
 b[n++]='\n';write(2,b,n);watch_s[i].hits++;
 mprotect((void *)watch_s[i].lo,watch_s[i].hi-watch_s[i].lo,PROT_READ|PROT_WRITE);
 if(watch_s[i].hits>=200)watch_s[i].lo=0;   /* round3: 上限后彻底解除该槽（页面已 RW、rearm 不再保护、后续访问不再 fault）——原 tick 端上限对 rearm 线程无效，自旋热点下会无界打印拖死客体 */
 return 1;}
 return 0;
}
int runtime_gfx_fault(void *addr,int is_write,void *context){if(watch_fault(addr,is_write,context))return 1;return fault_hook?fault_hook(addr,is_write,context):0;}
void crash_set_wait_dump(void (*fn)(void)){(void)fn;}
void *runtime_gfx_override(const char *name){
 unsigned long size=0;const GfxEntry *entries=(void *)getsectiondata(&_mh_execute_header,"__DATA","totkgfx",&size);
 for(unsigned i=0;entries&&i<size/sizeof(*entries);i++)if(!strcmp(entries[i].name,name))return entries[i].fn;return NULL;
}
typedef void *(*Alloc)(unsigned long,unsigned long,void *);
typedef void (*Free)(void *,void *);
typedef void *(*Realloc)(void *,unsigned long,void *);
struct Allocator{Alloc alloc;Free free;Realloc realloc;void *user;};
struct Allocator g_nv_alloc,g_nv_devtools_alloc;
static void graphics_allocator(Alloc a,Free f,Realloc r,void *u){g_nv_alloc=(struct Allocator){a,f,r,u};}
static void graphics_dev_allocator(Alloc a,Free f,Realloc r,void *u){g_nv_devtools_alloc=(struct Allocator){a,f,r,u};}
static void graphics_initialize(void *mem,unsigned long size){
 if(!mem||!size)runtime_fail("invalid graphics work memory");g_t0_ns=runtime_now_ns();mtl_init();fprintf(stderr,"[totk] native Metal graphics context initialized\n");
}
/* nvnBootstrapLoader 拦截（游戏唯一 nvn 导入；gfx_probe.c:91 探针同机制）：SDK 内部 nvn/nvdrv
   路径彻底不跑（nv::InitializeGraphics 断言即由它触发）。按名查宿主小实现表；表外一律给包装后的
   返-0 桩（记名）。round8/round9 教训：游戏把 bootstrap / GetProcAddress 的返回值直接 blr 且
   461 槽填完无空检查——回 NULL 即 pc=0，GetProcAddress 表外也必须给可调桩（返 0 桩可被调用，
   只是结果为 0）。后续轮次按日志逐个补关键项。
   真实渲染后端仍是 Vulkan（nvn_vk_stub.cpp 占位），boot 推进与渲染接线是两件事。 */
static uint64_t g_nvn_stub_area[64] __attribute__((used));   /* 后续小实现的共享暂存区 */
/* round19-B：返-0 桩按调用者一次性探针。包装层（hle_stack.c 按函数指针去重）把所有 stub 折叠成同一
   thunk、名字不可区分；改在桩内取调用点：包装栈路径（hle_stack.S runtime_hle_switch 的 blr）下
   __builtin_return_address(0) 是 thunk 内地址，沿帧记录上走一级即客体调用点（runtime_hle_switch 的
   stp x29,x30 存的是客体 fp/lr）；嵌套 br 尾跳路径 LR 已是客体地址，上走一级得其调用者（仍定位区域）。
   16 项去重首见打一行，一轮日志即知纹理路径还踩了哪些返-0 桩。 */
/* M17A（2026-11-08）：把 stub_caller_lr 的"沿帧链找可解析的客体返回址"逻辑暴露给 nvn_replay 的胶水层。
   录制侧（glue_cmdbuf.c）在每条 draw 录制前调一次，把「客体里发这笔画面的那个函数」的返回址
   存进全局，cmdbuf.c 再把它塞进 CmdRec.f[0..3]，回放时 cmd_trace.c 抄给 nvn_vk 台账 —— 于是
   每笔 draw 都能用 eh_frame_hdr FDE 反查到宿主函数（与门 115 的 4 字窗无关，也不依赖队列对齐）。 */
uint64_t gfx_guest_caller_fp_v;   /* 找到返回址的那一帧的 frame pointer（打印调用链用） */
uint64_t gfx_guest_caller_lr(void)
{
 uintptr_t fp=(uintptr_t)__builtin_frame_address(0),lr=0;
 gfx_guest_caller_fp_v=0;
 for(int d=0;d<6&&fp&&!(fp&15);d++){
  uintptr_t up=*(uintptr_t *)fp;if(!up||(up&15))break;
  uintptr_t cand=*(uintptr_t *)(up+8);const char *nm;uint64_t off;
  if(cand&&!lr)lr=cand;
  if(cand&&runtime_module_offset(cand,&nm,&off)){gfx_guest_caller_fp_v=(uint64_t)up;return (uint64_t)cand;}
  fp=up;
 }
 return (uint64_t)lr;
}
static uintptr_t stub_caller_lr(void){
 /* round21-D：round20 分槽桩比 round19 多一层桩函数帧，固定上走一级落在宿主 thunk（hle_stack.S blr
    后地址，round20.log 实测 0x5ed7084xxx）。改为沿帧链（至多 6 级）取首个能被 runtime_module_offset
    解析成客体模块的返回地址；嵌套 br 尾跳路径（LR 已是客体地址）同样命中；全不解析则退回首见候选。 */
 uintptr_t fp=(uintptr_t)__builtin_frame_address(0),lr=0;
 for(int d=0;d<6&&fp&&!(fp&15);d++){
  uintptr_t up=*(uintptr_t *)fp;if(!up||(up&15))break;
  uintptr_t cand=*(uintptr_t *)(up+8);const char *nm;uint64_t off;
  if(cand&&!lr)lr=cand;
  if(cand&&runtime_module_offset(cand,&nm,&off))return cand;
  fp=up;
 }
 return lr;
}
static uint64_t nvn_unimplemented(void){
 static uintptr_t stub_seen[16];static unsigned stub_n;
 uintptr_t lr=stub_caller_lr();
 for(unsigned i=0;i<stub_n;i++)if(stub_seen[i]==lr)return 0;
 if(stub_n<16)stub_seen[stub_n++]=lr;
 {const char *nm;uint64_t off;char loc[96];
  if(runtime_module_offset(lr,&nm,&off))snprintf(loc,sizeof loc,"%s+%#llx",nm,(unsigned long long)off);
  else snprintf(loc,sizeof loc,"%p",(void *)lr);
  fprintf(stderr,"[nvn] stub-call lr=%p (%s)\n",(void *)lr,loc);}
 return 0;
}
/* round20-A：按名分槽桩——包装层按函数指针去重，共用一个 nvn_unimplemented 时 384 个表外名（round19.log
   去重实测）全折叠进同一 thunk、名字丢失。登记时给每个名字一个互不相同的 X-macro 桩（500 槽 ≥ 384），
   桩内首调打 [nvn] stub#i name=… lr=…（模块+偏移），下轮日志直接给 0x9a3da8/0x9a5d60 链上每桩名字。
   溢出/无名路径退回上面的 nvn_unimplemented（LR 探针仍在）。同名→同槽→同 thunk，指针比较语义不变。 */
#define NVN_MAX_STUBS 500
static char *nvn_stub_names[NVN_MAX_STUBS];static unsigned n_nvn_stubs;
static void nvn_stub_log(unsigned i){
 static unsigned char logged[NVN_MAX_STUBS];
 uintptr_t lr=stub_caller_lr();   /* round21-D：沿帧链找客体调用点（round20 固定一级落在宿主 thunk） */
 if(logged[i])return;logged[i]=1;
 {const char *nm;uint64_t off;char loc[96];
  if(runtime_module_offset(lr,&nm,&off))snprintf(loc,sizeof loc,"%s+%#llx",nm,(unsigned long long)off);
  else snprintf(loc,sizeof loc,"%p",(void *)lr);
  fprintf(stderr,"[nvn] stub#%u name=%s lr=%p (%s)\n",i,nvn_stub_names[i]?nvn_stub_names[i]:"?",(void *)lr,loc);}
}
/* 终局 B 计划（crash_1031 终局四段）：表外桩按名调用计数——hang dump 时输出高频桩名单，
   ">100 次的桩"即下一嫌疑（48k map pool 的伴生高频桩）。counts 只增不减，无锁竞态可接受。 */
static uint32_t nvn_stub_counts[NVN_MAX_STUBS];
static uint64_t nvn_stub_hit(unsigned i){nvn_stub_log(i);if(i<NVN_MAX_STUBS)nvn_stub_counts[i]++;return 0;}
void runtime_nvn_stub_report(void){
 unsigned n=0;for(unsigned i=0;i<NVN_MAX_STUBS&&n<NVN_MAX_STUBS;i++)if(nvn_stub_counts[i])n++;
 fprintf(stderr,"[totk-dump] nvn stub calls, %u distinct:\n",n);
 for(;;){unsigned best=NVN_MAX_STUBS;uint32_t bv=0;
  for(unsigned i=0;i<NVN_MAX_STUBS;i++)if(nvn_stub_counts[i]>bv){bv=nvn_stub_counts[i];best=i;}
  if(best==NVN_MAX_STUBS||!bv)break;
  fprintf(stderr,"[totk-dump] stub %5u x%-8u %s\n",best,bv,nvn_stub_names[best]?nvn_stub_names[best]:"?");
  nvn_stub_counts[best]=0;}   /* 打完清零：下次 dump 只报增量 */
}
/* M4am P1-2：A 类（dynstr 请求了、entries[] 无条目 ⇒ 落本表外桩）**不破坏性** Top 报表。
   与上面 runtime_nvn_stub_report 的区别：不清零、写进 vk_boot.log（应用自写日志），供每 1800 submit 排名。
   resolved = 游戏实际拿到过桩的名字数（= 真正被请求过的表外名字）；calls = 这些桩的调用总数。 */
static void nvn_a_stub_report(unsigned sub){
 unsigned resolved=n_nvn_stubs,nz=0;uint64_t total=0;
 for(unsigned i=0;i<NVN_MAX_STUBS;i++)if(nvn_stub_counts[i]){nz++;total+=nvn_stub_counts[i];}
 char b[640];int o=snprintf(b,sizeof b,"[h] A-stub calls @submit#%u: resolved=%u called=%u total=%llu top:",
  sub,resolved,nz,(unsigned long long)total);
 unsigned used[16];uint32_t best[16];unsigned n=0;
 for(unsigned k=0;k<16;k++){   /* M40C：10 -> 16，把 called=13 的第 11-13 名点出来（used/best[16] 原本就够） */
  unsigned bi=NVN_MAX_STUBS;uint32_t bv=0;
  for(unsigned i=0;i<NVN_MAX_STUBS;i++){uint32_t v=nvn_stub_counts[i];if(!v)continue;
   int skip=0;for(unsigned j=0;j<n;j++)if(used[j]==i){skip=1;break;}
   if(skip)continue;if(v>bv){bv=v;bi=i;}}
  if(bi==NVN_MAX_STUBS)break;used[n]=bi;best[n]=bv;n++;
 }
 for(unsigned i=0;i<n&&o<(int)sizeof b-48;i++)
  o+=snprintf(b+o,sizeof b-(size_t)o," %s=%u",nvn_stub_names[used[i]]?nvn_stub_names[used[i]]:"?",best[i]);
 vk_boot_logf("%s",b);
 /* 未被调用的已解析名字（= 游戏只是问了地址、从未调用）——一次一行，超过 40 个截断。 */
 static unsigned last_n;
 if(n_nvn_stubs>last_n){
  char z[512];int zo=snprintf(z,sizeof z,"[h] A-stub resolved (never-called may follow) @submit#%u:",sub);
  unsigned shown=0;
  for(unsigned i=0;i<n_nvn_stubs&&zo<(int)sizeof z-40;i++)
   if(!nvn_stub_counts[i]){zo+=snprintf(z+zo,sizeof z-(size_t)zo," %s",nvn_stub_names[i]?nvn_stub_names[i]:"?");shown++;}
  last_n=n_nvn_stubs;
  if(shown)vk_boot_logf("%s",z);
 }
}

/* ---- M4am P1-2：B 类空桩（有条目但实现只记账/空/日志）按名调用计数 ----
   为什么要有：离线集合差给出的 B 类有 110 个名字，但「被引用」不等于「被调用」——只有真调用了
   才是「本该发生却没发生」的事故点。计数只在桩内自增（一次 uint32 ++ + 首调登记），
   报表由 nvn_h_report 限流调用（每 1800 submit 一次，Top15），不逐次打日志。
   用 nm_sc_add 登记 {名字, 计数器地址}：函数体内 static 计数器的地址在首次调用时登记一次即可，
   报表侧只读指针，不改桩的行为（本块**只观测**，不改语义）。 */
#define NM_SC_MAX 160
typedef struct{const char *name;uint32_t *c;} nm_sc_rec;
static nm_sc_rec g_sc[NM_SC_MAX];static unsigned g_sc_n;
static void nm_sc_add(const char *n,uint32_t *c){
 for(unsigned i=0;i<g_sc_n;i++)if(g_sc[i].c==c)return;
 if(g_sc_n<NM_SC_MAX){g_sc[g_sc_n].name=n;g_sc[g_sc_n].c=c;g_sc_n++;}
}
#define NM_SC_N(LIT) do{static uint32_t nm_c;static int nm_r;if(!nm_r){nm_r=1;nm_sc_add((LIT),&nm_c);}nm_c++;}while(0)
#define NM_SC(NAME) NM_SC_N(#NAME)
/* Top-N 选择（不排序全表、不清零：报表可反复出）。返回打印到的字节数。 */
static void nm_sc_report(unsigned sub){
 if(!g_sc_n)return;
 /* M4bv P2：top 从 15 抬到 24（boot 实测 distinct=28，top15 之外看不到剩下的 13 个名字——
    本轮要确认「游戏有没有调 DrawTexture/Downsample 这类只在记账的 final blit」，
    它们每帧 1 次量级，正好落在 top15 的截断线以下）。 */
 unsigned used[32];uint32_t best[32];unsigned n=0;uint32_t total=0;
 for(unsigned i=0;i<g_sc_n;i++)total+=*g_sc[i].c;
 for(unsigned k=0;k<24;k++){
  unsigned bi=NM_SC_MAX;uint32_t bv=0;
  for(unsigned i=0;i<g_sc_n;i++){
   uint32_t v=*g_sc[i].c;if(!v)continue;
   int skip=0;for(unsigned j=0;j<n;j++)if(used[j]==i){skip=1;break;}
   if(skip)continue;
   if(v>bv){bv=v;bi=i;}
  }
  if(bi==NM_SC_MAX)break;used[n]=bi;best[n]=bv;n++;
 }
 char b[1200];int o=snprintf(b,sizeof b,"[h] B-stub calls @submit#%u: distinct=%u total=%llu top%u:",sub,g_sc_n,(unsigned long long)total,n);
 for(unsigned i=0;i<n&&o<(int)sizeof b-40;i++)
  o+=snprintf(b+o,sizeof b-(size_t)o," %s=%u",g_sc[used[i]].name,best[i]);
 vk_boot_logf("%s",b);
}
/* round20-A 修编译：数字各位分参传递（h,t,u），只在标识符处 ## 粘接，数值用 h*100+t*10+u 算——
   旧版 P2(0) 粘出 008/009/018…089 等前导零八进制字面量，遇 8/9 直接 invalid digit 编译失败。 */
#define P1(h,t) X(h,t,0)X(h,t,1)X(h,t,2)X(h,t,3)X(h,t,4)X(h,t,5)X(h,t,6)X(h,t,7)X(h,t,8)X(h,t,9)
#define P2(h) P1(h,0)P1(h,1)P1(h,2)P1(h,3)P1(h,4)P1(h,5)P1(h,6)P1(h,7)P1(h,8)P1(h,9)
#define NVN_SLOTS P2(0)P2(1)P2(2)P2(3)P2(4)   /* 500 个互不相同的桩函数 */
#define X(h,t,u) static uint64_t nvn_stub_##h##t##u(void){return nvn_stub_hit(h*100+t*10+u);}
NVN_SLOTS
#undef X
#define X(h,t,u) nvn_stub_##h##t##u,
static void *const nvn_stub_fns[NVN_MAX_STUBS]={NVN_SLOTS};
#undef X
static void *nvn_named_stub(const char *name){
 for(unsigned i=0;i<n_nvn_stubs;i++)if(nvn_stub_names[i]&&!strcmp(name,nvn_stub_names[i]))return nvn_stub_fns[i];
 if(n_nvn_stubs<NVN_MAX_STUBS){nvn_stub_names[n_nvn_stubs]=(char *)strdup(name);return nvn_stub_fns[n_nvn_stubs++];}
 return (void *)nvn_unimplemented;   /* 槽满退回无名桩（round19-B LR 探针兜底） */
}
static uint64_t nvn_device_initialize(void *device,const void *builder){(void)device;(void)builder;fprintf(stderr,"[nvn] device initialize -> 1\n");return 1;}   /* NVNboolean 成功=1：main+0x94147c tst w0,#0xff 实测要求（donor hle_nvn.c:140-145 同返 1） */
static void nvn_device_builder_set_defaults(void *b){memset(b,0,4);}   /* dev_builder_t={uint32_t flags}（donor nvn_obj.h:16），同 donor hle_nvn.c:138 memset sizeof(dev_builder_t) */
static void nvn_device_builder_set_flags(void *b,int f){*(uint32_t *)b=(uint32_t)f;}   /* 同 donor hle_nvn.c:139 */
/* donor hle_nvn.c g_devinfo 的 per-game NVN GetInteger 应答表：数值表本体已外移
   games/0100000000010000/game_env.c（SMO 1.3.0 全表，Mac 上验证过），平台经弱 getter 取表——
   TOTK 落地时其 game_env.c 提供同签名强符号（TotK 是 55/15 ABI，gfx_probe.c:103 注释），platform 零改动。
   弱默认（无游戏侧实现时）= 空表，全部答 0。 */
__attribute__((weak)) const int *game_nvn_devinfo(unsigned *out_count){
 if(out_count)*out_count=0;
 return NULL;
}
static void nvn_device_get_integer(const void *device,int what,int *out){(void)device;   /* main+0x9414c4 cmp #0x35、+0x9414d0 cmp #0x137（minor≥311） */
 unsigned w=(unsigned)what;
 unsigned nvn_devinfo_n=0;
 const int *nvn_devinfo=game_nvn_devinfo(&nvn_devinfo_n);
 static uint64_t seen[2];   /* 每个 what 首查打点（0x00-0x7F bitmap） */
 if(w<0x80){if(!((seen[w>>6]>>(w&63))&1)){seen[w>>6]|=1ULL<<(w&63);fprintf(stderr,"[nvn] GetInteger %#x -> %d\n",w,w<nvn_devinfo_n?nvn_devinfo[w]:0);} }
 *out=w<nvn_devinfo_n?nvn_devinfo[w]:0;
}
static void nvn_device_finalize(void *device){NM_SC_N("nvnDeviceFinalize");(void)device;}   /* 槽 e30；donor hle_nvn.c:146 清 magic，此处无对象账本，空操作 */
/* MemoryPool 层（round10 悬崖：main+0x9a3f60 把 Map 返回值当 CPU 基址写；donor hle_nvn.c:226-268，
   布局=nvn_obj.h:23/25 前半，本地定义免头纠缠。存储由游戏经 SetStorage 自备，宿主不分配） */
typedef struct{void *dev,*mem;uint64_t size;uint32_t flags;} pool_builder_h;   /* 0x20，=pool_builder_t */
typedef struct{uint32_t magic,flags;void *mem;uint64_t size;} pool_h;         /* 0x18 头部，=pool_t 前半，游戏分配 0x38 够放 */
/* crash_1031 毒化源判定：nvn 池账本（容量 256，溢出覆盖最旧）。Initialize 时记 {mem,size,flags}
   （host-alloc 分支之后写入，游戏自备/宿主分配两类存储都覆盖；VIRTUAL 池 ctor 时 mem=NULL、
   map-virtual 后才有效，NULL 槽位永不匹配）。读侧（crash handler）只读+算术、无锁，
   async-signal-safe。 */
#define NVN_POOL_LEDGER 256
typedef struct{uintptr_t mem;uint64_t size;unsigned flags;} NvnPoolRec;
static NvnPoolRec nvn_pool_ledger[NVN_POOL_LEDGER];static unsigned nvn_pool_ledger_head;
int runtime_nvn_pool_hit(uint64_t addr,uint64_t *lo,uint64_t *hi,unsigned *flags_out){
 for(unsigned i=0;i<NVN_POOL_LEDGER;i++){const NvnPoolRec *r=&nvn_pool_ledger[i];
  if(r->mem&&addr>=r->mem-0x4000&&addr<r->mem+r->size+0x4000){if(lo)*lo=r->mem;if(hi)*hi=r->mem+r->size;if(flags_out)*flags_out=r->flags;return 1;}}
 return 0;
}
static void nvn_pool_builder_set_defaults(void *b){memset(b,0,sizeof(pool_builder_h));}   /* =memset 0x20 */
static void nvn_pool_builder_set_device(void *b,void *d){((pool_builder_h *)b)->dev=d;}
static void nvn_pool_builder_set_storage(void *b,void *m,uint64_t n){((pool_builder_h *)b)->mem=m;((pool_builder_h *)b)->size=n;if(!n)fprintf(stderr,"[nvn] pool builder SetStorage size=0 (mem=%p)：查询桩喂 0 或游戏真要 0，计数变化指示 0 尺寸源\n",m);}   /* round16 插桩：区分"游戏真要 0"与"查询桩喂 0" */
static void nvn_pool_builder_set_flags(void *b,int f){((pool_builder_h *)b)->flags=(uint32_t)f;}
static uint64_t nvn_pool_initialize(void *p,const void *b){
 pool_h *o=p;const pool_builder_h *s=b;memset(o,0,0x20);o->magic=0x504f4f4c;o->flags=s->flags;o->mem=s->mem;o->size=s->size;   /* 'POOL'；清 0x20（含 donor 的 game_mem 槽位）在游戏 0x38 分配内安全 */
 if(!o->mem&&o->size){   /* 驱动自备存储：游戏对 VIRTUAL 池 SetStorage(NULL)（round13 实测 flags=0x2a1=VIRTUAL|COMPRESSIBLE|GPU_CACHED|CPU_NO_ACCESS），真机由驱动分配，donor 同位走 totk_vpool_init（hle_nvn.c:241）；boot 期宿主匿名页即可，后续 Vulkan 后端接线再换真 GPU 存储 */
  size_t n=(o->size+0xfff)&~0xfffULL;o->mem=mmap(NULL,n,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
  if(o->mem==MAP_FAILED)o->mem=NULL;
  fprintf(stderr,"[nvn] pool %p host-alloc %#llx -> %p\n",p,(unsigned long long)n,o->mem);
 }
 fprintf(stderr,"[nvn] pool %p mem=%p size=%#llx flags=%#x\n",p,o->mem,(unsigned long long)o->size,o->flags);   /* 自带诊断：mem=0 即 ctor 没走 SetStorage 且宿主分配失败 */
 nvn_pool_ledger[nvn_pool_ledger_head]=(NvnPoolRec){(uintptr_t)o->mem,o->size,o->flags};nvn_pool_ledger_head=(nvn_pool_ledger_head+1)%NVN_POOL_LEDGER;   /* 毒化源账本（crash handler 经 runtime_nvn_pool_hit 只读查询） */
 return 1;
}
static void *nvn_pool_map(const void *p){const pool_h *o=p;   /* round14 插桩：magic≠0x504f4f4c 即 foreign 对象（从未过宿主 Initialize）。round12b 起对齐 donor hle_nvn.c:1986——flags&CPU_NO_ACCESS 返 NULL（=0x1，donor include/nvn.h:605；nvnBufferMap :1977 同款）。
   round13 教训（游戏拿 Map 当写基址 main+0x9a3f38/0x9a3f60，当时宿主无 vpool 层裸 NULL 必崩）不冲突：CPU_NO_ACCESS 池（0x21/0x61/0xa1/0x2a1）的写基址经 MapVirtual 取物理后备别名（map_virtual 把 v->mem 指到 pp->mem+poff，宿主匿名页可写），NULL 即真机语义。
   round12a 实测洪流（3.88M 次/147s≈26k/s）194 条采样全落 flags=0x12/0x24（CPU 可见池，不吃 NULL 分支），返回值不变 */
 /* round3 限流：该行是 map-pool 洪流本体（13k 行/s，56 万行/43s），打满 stderr 管道+hilog 进程级
    socket——[spin] 采样/全景 dump 全被挤掉，且 hilog 流自身在 43s 处断流（emu_round1/2/3 同证）。
    前 200 条全打，之后每 2 万条打 1 条带累计序号：洪流起点/终点/总速可见，日志通道留活。 */
 static unsigned pn,ps;pn++;
 /* round12c 排障（donor 基准：SMO init 全程 Map=409 次/帧 2 次，本宿主 402 万次/150s=万倍病态）：
    ①调用点一次性 LR 探针（stub_caller_lr 同款帧链走法，24 个去重首见）——洪流调用链检举；
    ②池账本反查一次性打点——洪流主池两轮 ctor 行皆缺（12a 0x6e44573e90/12b 0x6e43476e90），按 mem+size
      查 nvn_pool_ledger：命中=ctor 行只是被 hilog 丢弃，未命中=struct 拷贝/foreign 对象；
    ③每 50 万次打当前 lr+pool（洪流期取样）。全部有界输出，不添新洪流。 */
 {uintptr_t lr=stub_caller_lr();
  static uintptr_t seen_lr[24];static unsigned nsl;
  unsigned f=1;for(unsigned i=0;i<nsl;i++)if(seen_lr[i]==lr)f=0;
  if(f&&nsl<24){seen_lr[nsl++]=lr;const char *nm;uint64_t off;char loc[96];
   if(runtime_module_offset(lr,&nm,&off))snprintf(loc,sizeof loc,"%s+%#llx",nm,(unsigned long long)off);else snprintf(loc,sizeof loc,"%p",(void *)lr);
   fprintf(stderr,"[nvn] map-caller lr=%p (%s) pool=%p flags=%#x (#%u first)\n",(void *)lr,loc,p,o->flags,pn);}
  static uintptr_t seen_pool[16];static unsigned nsp;
  f=1;for(unsigned i=0;i<nsp;i++)if(seen_pool[i]==(uintptr_t)p)f=0;
  if(f&&nsp<16){seen_pool[nsp++]=(uintptr_t)p;int led=0;
   for(unsigned i=0;i<NVN_POOL_LEDGER;i++)if(nvn_pool_ledger[i].mem==(uintptr_t)o->mem&&nvn_pool_ledger[i].size==o->size){led=1;break;}
   fprintf(stderr,"[nvn] map-pool-seen pool=%p mem=%p size=%#llx flags=%#x ledger=%s (#%u)\n",p,o->mem,(unsigned long long)o->size,o->flags,led?"hit":"MISS",pn);}
  if(pn%500000==0)fprintf(stderr,"[nvn] map-sample lr=%p pool=%p flags=%#x (#%u)\n",(void *)lr,p,o->flags,pn);}
 if(ps<200||pn%20000==0){fprintf(stderr,"[nvn] map pool=%p magic=%#x mem=%p size=%#llx flags=%#x (#%u)\n",p,o->magic,o->mem,(unsigned long long)o->size,o->flags,pn);}   /* round12a 观测：洪流池 flags 分布（0x1=CPU_NO_ACCESS / 0x200=VIRTUAL）；NULL 分支命中率可由此行判读 */
 if(ps<200)ps++;
 if(o->flags&0x1)return NULL;   /* donor :1986：CPU_NO_ACCESS(0x1) → NULL；GetBufferAddress（donor :268）仍返 mem，GPU 地址链不受影响 */
 return o->mem;}
typedef struct{const void *physical_pool;int64_t physical_offset,virtual_offset;uint64_t size;int32_t storage_class;uint32_t padding;} nvn_map_req_h;   /* =totk_vpool.h:7-15 NVNmappingRequest，0x28 实测布局 */
static uint64_t nvn_pool_map_virtual(void *p,int n,const void *req){
 pool_h *v=p;const nvn_map_req_h *r=req;
 if(r)fprintf(stderr,"[nvn] map-virtual pool=%p n=%d phys=%p poff=%#llx voff=%#llx size=%#llx class=%d\n",p,n,r->physical_pool,(unsigned long long)r->physical_offset,(unsigned long long)r->virtual_offset,(unsigned long long)r->size,r->storage_class);   /* round14 插桩：dump 请求定位 mystery 池 */
 if(!v||v->magic!=0x504f4f4c||!(v->flags&0x200)||!v->mem||n<0||(n&&!r))return 0;   /* donor totk_vpool.c:107-108 同款校验（0x200=VIRTUAL；count=0 合法空操作） */
 if(!n)return 1;
 {const pool_h *pp=r->physical_pool;   /* donor-lite：不做 mach_vm_remap 页别名（totk_vpool.c:85-97），直接把虚拟池 mem 指到物理后备 p->mem+physical_offset；round12b NULL 语义后游戏写基址=本处 v->mem（物理池 mem=宿主匿名 RW 页或游戏自备存储，均可安全写），Map 虚拟池返 NULL 走 donor :1986，两链不交叉。donor :2025 vpool_reserve（mmap PROT_NONE 预留段，非固定地址）评估=不补：donor 对 Mario 整池单请求（voff=0、size=池大小）在首次 MapVirtual 即 vpool_drop 弃预留改指物理后备（hle_nvn.c:2114-2117），预留段只服务"映射前建对象"与真 sparse（MHR 64GB 分段重映射）；boot 期 Initialize 的宿主匿名 RW 页（上方 host-alloc 分支）已承担同一地址同一性且能力更强（可写），round12a 实测游戏 150s 内未调 MapVirtual，映射前无对象注册，故匿名页已够；boot 版不重映射/不登记 mtl，差异注此 */
  if(n>1)fprintf(stderr,"[nvn] map-virtual n=%d 只处理首请求\n",n);
  if(pp&&pp->magic==0x504f4f4c&&pp->mem){v->mem=(char *)pp->mem+r->physical_offset;fprintf(stderr,"[nvn] map-virtual -> mem=%p (phys %p + %#llx)\n",v->mem,pp->mem,(unsigned long long)r->physical_offset);}
  else fprintf(stderr,"[nvn] map-virtual phys pool invalid/foreign: %p\n",r->physical_pool);   /* 物理池也 foreign 时不动 v->mem，日志留证 */
 }
 return 1;
}
static void *nvn_pool_get_buffer_address(const void *p){return ((const pool_h *)p)->mem;}   /* donor :268 */
static uint64_t nvn_pool_get_size(const void *p){return ((const pool_h *)p)->size;}
static uint64_t nvn_pool_get_flags(const void *p){return ((const pool_h *)p)->flags;}   /* donor :259 */
static void nvn_pool_finalize(void *p){((pool_h *)p)->magic=0;}   /* donor :253-257 的清 magic；无 Metal 账本可清 */
/* Buffer 层（下一悬崖高概率：buf 引 pool 不持有内存，donor hle_nvn.c:270-296 简化掉 magic/dev 校验） */
typedef struct{void *pool;uint64_t off,size;} buf_h;
static void nvn_buffer_builder_set_storage(void *b,void *pool,int64_t off,uint64_t size){((buf_h *)b)->pool=pool;((buf_h *)b)->off=(uint64_t)off;((buf_h *)b)->size=size;}
static uint64_t nvn_buffer_initialize(void *buf,const void *b){*(buf_h *)buf=*(const buf_h *)b;return 1;}   /* 拷贝即成（donor :277-286 校验后拷 'BUF'，此处无账本） */
/* crash_1031 H2 观测：nvn_buffer_map 池边界校验——off/size 越过池尾即一行 write(2) 红日志（不打断、
   照旧回 mem+off，行为不变）。范围取池对象自身 mem/size（与 runtime_nvn_pool_hit 账本同源数据，
   对 foreign 池也生效）：池管理器（main+0x97fac0）放置重叠或 nvn_tex_layout 尺寸偏小时，游戏按
   本返回值写池即越界——红日志行就是越界写起点的现场登记。off 与 size 分开比，防相加回绕漏报。 */
static void *nvn_buffer_map(const void *buf){const buf_h *v=buf;const pool_h *p=v->pool;
 if(p&&p->mem&&(v->off>p->size||v->off+v->size>p->size)){char b[192];size_t n=0;
  watch_hex(b,&n,"[nvn][RED] BufferMap OOB pool=",(uintptr_t)p->mem);watch_hex(b,&n,"-",(uintptr_t)p->mem+p->size);watch_hex(b,&n," off=",v->off);watch_hex(b,&n," size=",v->size);b[n++]='\n';write(2,b,n);}
 return p&&p->mem?(char *)p->mem+(ptrdiff_t)v->off:0;}   /* CPU 指针=池 mem+off（donor :288-292） */
static void *nvn_buffer_get_address(const void *buf){return nvn_buffer_map(buf);}   /* GPU 地址=CPU 地址（donor :293） */
/* round15：尺寸/构建器查询家族换真实现——此前全走返-0 桩，把池管理器（main+0x97fac0）的尺寸计算链短路，
   *(wrapper+0x18) 恒 NULL → main+0x9a3f60 首元素写 NULL（rounds10-14 同址第五次才定论）。全部照 donor 字段回声/公式。 */
typedef struct{void *dev;uint32_t flags;uint64_t cmd_sz,comp_sz,ctl_sz,flush;void *mem;uint64_t mem_sz;} queue_builder_h;   /* =nvn_obj.h:19 queue_builder_t */
static void nvn_queue_builder_set_defaults(void *b){void *dev=((queue_builder_h *)b)->dev;memset(b,0,sizeof(queue_builder_h));queue_builder_h *o=b;o->dev=dev;o->cmd_sz=65536;o->comp_sz=262144;o->ctl_sz=16384;o->flush=4096;}   /* 设备默认值，donor :701-706 */
static void nvn_queue_builder_set_device(void *b,void *d){((queue_builder_h *)b)->dev=d;}
static void nvn_queue_builder_set_flags(void *b,int f){((queue_builder_h *)b)->flags=(uint32_t)f;}
static void nvn_queue_builder_set_command_memory_size(void *b,uint64_t n){((queue_builder_h *)b)->cmd_sz=n;}
static void nvn_queue_builder_set_compute_memory_size(void *b,uint64_t n){((queue_builder_h *)b)->comp_sz=n;}
static void nvn_queue_builder_set_control_memory_size(void *b,uint64_t n){((queue_builder_h *)b)->ctl_sz=n;}
static void nvn_queue_builder_set_command_flush_threshold(void *b,uint64_t n){((queue_builder_h *)b)->flush=n;}
static void nvn_queue_builder_set_queue_memory(void *b,void *m,uint64_t n){((queue_builder_h *)b)->mem=m;((queue_builder_h *)b)->mem_sz=n;}
static uint64_t nvn_queue_builder_get_queue_memory_size(const void *b){const queue_builder_h *o=b;return (o->cmd_sz+o->comp_sz+o->ctl_sz+4095)&~(uint64_t)4095;}   /* donor :715-717：命令+计算+控制之和按 4KiB 取整 */
static uint64_t nvn_pool_builder_get_size(const void *b){return ((const pool_builder_h *)b)->size;}
static void *nvn_pool_builder_get_memory(const void *b){return ((pool_builder_h *)b)->mem;}
static uint64_t nvn_pool_builder_get_flags(const void *b){return ((const pool_builder_h *)b)->flags;}
static uint64_t nvn_buffer_builder_get_size(const void *b){return ((const buf_h *)b)->size;}
static void *nvn_buffer_builder_get_memory_pool(const void *b){return ((buf_h *)b)->pool;}
static int64_t nvn_buffer_builder_get_memory_offset(const void *b){return (int64_t)((const buf_h *)b)->off;}
static uint64_t nvn_buffer_get_size(const void *buf){return ((const buf_h *)buf)->size;}   /* donor :296 */
static void *nvn_buffer_get_memory_pool(const void *buf){return ((buf_h *)buf)->pool;}   /* donor :294 */
static int64_t nvn_buffer_get_memory_offset(const void *buf){return (int64_t)((const buf_h *)buf)->off;}   /* donor :295 */
typedef struct{uint32_t magic,recording;uint8_t *cur;uint64_t guest_flags;uint8_t *end;uint64_t inline_cur,inline_end;void *cb,*cb_data,*dev;uint8_t *seg;void *ctl;uint64_t ctl_size,game_size,cmd_base,cmd_used,ctl_used,rec_start,hw_used,last_rec,hw_start;} cmd_h;   /* =nvn_obj.h:70-87 cmd_t 字段序一致（guest_flags@0x10、inline_cur@0x20、inline_end@0x28 同 donor 断言） */
/* round15b 回执环前置声明（实现块在下方 GPU 值写入族处；record/submit/复用生命周期都挂进去）。 */
static void nvn_rcpt_drop(void *cmd);
static void nvn_rcpt_map(uint64_t h,cmd_h *c);
static void nvn_rcpt_submit(int n,const uint64_t *handles);
static uint64_t nvn_command_buffer_initialize(void *c,void *d){cmd_h *o=c;nvn_rcpt_drop(o);memset(o,0,sizeof(cmd_h));o->magic=0x434d4442;o->dev=d;o->inline_cur=~(uint64_t)0;o->inline_end=0;{static unsigned n;if(n++<8)fprintf(stderr,"[nvn] CommandBufferInitialize %p\n",c);}return 1;}   /* 'CMDB'，donor :773-775；round48 起打点：worker 提交前置判别（boot 期已 2 块，新指针=worker 建 CB）；round15b 同址复用先弃旧回执 */
static void nvn_command_buffer_add_command_memory(void *c,const void *p,int64_t off,uint64_t n){cmd_h *o=c;const pool_h *pp=p;o->cmd_base=(uintptr_t)(pp?pp->mem:NULL)+(uintptr_t)off;o->game_size=n;o->cmd_used=0;o->hw_used=0;{static const void *seen[8];static unsigned ns;int hit=0;for(unsigned i=0;i<ns;i++)if(seen[i]==c)hit=1;if(!hit&&ns<8){seen[ns++]=c;fprintf(stderr,"[nvn] CB %p AddCommandMemory pool=%p off=%#llx n=%#llx base=%#llx\n",c,p,(unsigned long long)off,(unsigned long long)n,(unsigned long long)o->cmd_base);}}}   /* donor AddCommandMemory（accounting only）；round48 打点：命令内存授予是录制前置（缺它=worker CB 未配 */
static void nvn_command_buffer_add_control_memory(void *c,void *m,uint64_t n){cmd_h *o=c;o->ctl=m;o->ctl_size=n;o->ctl_used=0;{static const void *seen[8];static unsigned ns;int hit=0;for(unsigned i=0;i<ns;i++)if(seen[i]==c)hit=1;if(!hit&&ns<8){seen[ns++]=c;fprintf(stderr,"[nvn] CB %p AddControlMemory mem=%p n=%#llx\n",c,m,(unsigned long long)n);}}}   /* donor :840-844；round48 打点同上 */
static uint64_t nvn_command_buffer_get_command_memory_size(const void *c){return ((const cmd_h *)c)->game_size;}   /* donor :858 */
static uint64_t nvn_command_buffer_get_control_memory_size(const void *c){return ((const cmd_h *)c)->ctl_size;}   /* donor :845 */
typedef struct{void *dev;nvn_texdesc d;void *pool;int64_t off;uint8_t has_layout,layout[8];const void *pkg_data;} tex_builder_h;   /* =nvn_obj.h:30-34 tex_builder_t */
static void nvn_texture_builder_set_defaults(void *b){void *dev=((tex_builder_h *)b)->dev;memset(b,0,sizeof(tex_builder_h));tex_builder_h *o=b;o->dev=dev;o->d.swizzle_r=2;o->d.swizzle_g=3;o->d.swizzle_b=4;o->d.swizzle_a=5;o->d.target=1;o->d.width=o->d.height=o->d.depth=o->d.levels=1;}   /* donor :330-336：2D=1（include/nvn.h:778），whdl=1 */
static void nvn_texture_builder_set_device(void *b,void *d){((tex_builder_h *)b)->dev=d;}
static void nvn_texture_builder_set_flags(void *b,int f){((tex_builder_h *)b)->d.flags=f;}
static void nvn_texture_builder_set_target(void *b,int t){((tex_builder_h *)b)->d.target=t;}
static void nvn_texture_builder_set_width(void *b,int v){((tex_builder_h *)b)->d.width=(uint32_t)v;}
static void nvn_texture_builder_set_height(void *b,int v){((tex_builder_h *)b)->d.height=(uint32_t)v;}
static void nvn_texture_builder_set_depth(void *b,int v){((tex_builder_h *)b)->d.depth=(uint32_t)v;}
static void nvn_texture_builder_set_size1d(void *b,int w){tex_builder_h *o=b;o->d.width=(uint32_t)w;o->d.height=o->d.depth=1;}
static void nvn_texture_builder_set_size2d(void *b,int w,int h){tex_builder_h *o=b;o->d.width=(uint32_t)w;o->d.height=(uint32_t)h;o->d.depth=1;}
static void nvn_texture_builder_set_size3d(void *b,int w,int h,int d){tex_builder_h *o=b;o->d.width=(uint32_t)w;o->d.height=(uint32_t)h;o->d.depth=(uint32_t)d;}
static void nvn_texture_builder_set_levels(void *b,int v){((tex_builder_h *)b)->d.levels=(uint32_t)v;}
static void nvn_texture_builder_set_format(void *b,int f){((tex_builder_h *)b)->d.format=f;}
static void nvn_texture_builder_set_samples(void *b,int s){((tex_builder_h *)b)->d.samples=s;}
static void nvn_texture_builder_set_stride(void *b,int64_t s){((tex_builder_h *)b)->d.stride=(uint32_t)s;}
/* M3y P1：纹理/采样器 helper 补表（与 M3s 顶点状态同一类 bug：名字不在 entries[] ⇒ 落返-0 空桩，
   Set* 变空操作、Get* 恒 0，builder→Initialize 的状态搬运永远丢掉）。语义逐个照 donor
   hle_nvn.c:340-470 与 nvn_obj.h 的 tex_builder_t(:30-34)/tex_t(:35-39)/samp_par(:51)，
   全部纯字段存取——这些名字本身不产生 GPU 副作用。 */
static void nvn_texture_builder_set_swizzle(void *b,int r,int g,int bl,int a){tex_builder_h *o=b;
 if((unsigned)r>5||(unsigned)g>5||(unsigned)bl>5||(unsigned)a>5){fprintf(stderr,"[nvn] invalid texture swizzle %d/%d/%d/%d\n",r,g,bl,a);return;}   /* donor :355 范围 0..5（die→日志） */
 o->d.swizzle_r=r;o->d.swizzle_g=g;o->d.swizzle_b=bl;o->d.swizzle_a=a;}
static void nvn_texture_builder_set_pkg_layout(void *b,const void *l){tex_builder_h *o=b;o->has_layout=l!=NULL;
 if(l){memcpy(o->layout,l,8);o->d.pkg_block_h=((const signed char *)l)[0]+1;}else o->d.pkg_block_h=0;}   /* donor :359-363：layout[0]+1 */
static void nvn_texture_builder_set_pkg_data(void *b,const void *d){((tex_builder_h *)b)->pkg_data=d;}   /* donor :365 */
static const void *nvn_texture_builder_get_pkg_data(const void *b){return ((const tex_builder_h *)b)->pkg_data;}
static int nvn_texture_builder_get_depth_stencil_mode(const void *b){NM_SC_N("nvnTextureBuilderGetDepthStencilMode");(void)b;return 0;}   /* donor :354 Set 非 DEPTH 即 die；Get 恒 DEPTH=0（nvn.h:793） */
static void nvn_texture_builder_set_depth_stencil_mode(void *b,int m){NM_SC_N("nvnTextureBuilderSetDepthStencilMode");(void)b;if(m!=0)fprintf(stderr,"[nvn] texture depth-stencil mode %d (stencil-plane sampling not implemented)\n",m);}   /* donor :354 同款拒收 */
static void nvn_texture_builder_set_gl_tex_name(void *b,uint32_t n){NM_SC_N("nvnTextureBuilderSetGLTextureName");(void)b;(void)n;}   /* GL 后端专名，NVN 无对应（Get 恒 0） */
static uint32_t nvn_texture_builder_get_gl_tex_name(const void *b){NM_SC_N("nvnTextureBuilderGetGLTextureName");(void)b;return 0;}
static int nvn_texture_builder_get_width(const void *b){return (int)((const tex_builder_h *)b)->d.width;}
static int nvn_texture_builder_get_height(const void *b){return (int)((const tex_builder_h *)b)->d.height;}
static int nvn_texture_builder_get_depth(const void *b){return (int)((const tex_builder_h *)b)->d.depth;}
static int nvn_texture_builder_get_levels(const void *b){return (int)((const tex_builder_h *)b)->d.levels;}
static int nvn_texture_builder_get_format(const void *b){return ((const tex_builder_h *)b)->d.format;}
static int nvn_texture_builder_get_flags(const void *b){return (int)((const tex_builder_h *)b)->d.flags;}
static int nvn_texture_builder_get_target(const void *b){return ((const tex_builder_h *)b)->d.target;}
static int nvn_texture_builder_get_samples(const void *b){return ((const tex_builder_h *)b)->d.samples;}
static int64_t nvn_texture_builder_get_stride(const void *b){return (int64_t)((const tex_builder_h *)b)->d.stride;}
static void nvn_texture_builder_get_swizzle(const void *b,int *r,int *g,int *bl,int *a){const nvn_texdesc *d=&((const tex_builder_h *)b)->d;
 if(r)*r=(int)d->swizzle_r;if(g)*g=(int)d->swizzle_g;if(bl)*bl=(int)d->swizzle_b;if(a)*a=(int)d->swizzle_a;}
static void *nvn_texture_builder_get_memory_pool(const void *b){return ((const tex_builder_h *)b)->pool;}
static int64_t nvn_texture_builder_get_memory_offset(const void *b){return ((const tex_builder_h *)b)->off;}
static void nvn_texture_builder_get_sparse_tile_layout(const void *b,void *out){(void)b;if(out)memset(out,0,64);}   /* 稀疏贴图本宿主不支持：零填充 */
static uint64_t nvn_texture_builder_get_zcull_storage_size(const void *b){NM_SC_N("nvnTextureBuilderGetZCullStorageSize");(void)b;return 0;}   /* 无 ZCull 硬件 */
static uint64_t nvn_texture_builder_get_storage_size(const void *b){nvn_texlayout l;nvn_tex_layout(&((const tex_builder_h *)b)->d,&l);if(!l.bpb)fprintf(stderr,"[nvn] TextureBuilderGetStorageSize unknown format %#x\n",((const tex_builder_h *)b)->d.format);return l.size;}   /* donor :382-391 走 nvn_tex_layout（简化掉 SIZELOG/预排布日志） */
static uint64_t nvn_texture_builder_get_storage_alignment(const void *b){   /* round22：donor :393-394 原式走 nvn_tex_layout（platform/nvn_tex.c 与 donor 逐字节同）——块线性 512、pitch 32、LINEAR_RENDER_TARGET 0x40 128、BUFFER=bpb、未知格式 512；donor 有公式故照抄、不取 0x1000 假值：池管理器 0x97fb38-0x97fb54 align-up 链吃这颗实值，round21 stub#59 返 0 是 -1MB 放置错位头嫌 */
 nvn_texlayout l;nvn_tex_layout(&((const tex_builder_h *)b)->d,&l);
 {static unsigned n;if(n<8){n++;fprintf(stderr,"[nvn] TextureBuilderGetStorageAlignment tgt=%d fmt=%#x fl=%#x -> %#x (donor nvn_tex_layout: 512 blocklinear / 32|128 pitch / bpb buffer)\n",((const tex_builder_h *)b)->d.target,((const tex_builder_h *)b)->d.format,((const tex_builder_h *)b)->d.flags,l.align);}}
 return l.align;}
static int nvn_texture_builder_get_storage_class(const void *b){   /* round14b 对齐 donor hle_nvn.c:2165：(void)b; return 0——注释原文 "the host has none, 0 as for the builder"，恒 0。旧注（BUFFER→0 其余→1）出自 d2r_gfx 旧版 totk_storage_class，与当前 donor 不符：SMO 2D 池纹理曾误返 1 */
 {static unsigned n;if(n<8){n++;fprintf(stderr,"[nvn] TextureBuilderGetStorageClass tgt=%d -> 0 (donor :2165 恒 0)\n",((const tex_builder_h *)b)->d.target);}}
 return 0;}
/* round19-A：纹理池存储链。此前 SetStorage/Initialize/GetTextureAddress 全是返-0 桩（round18.log:189/227/245），
   纹理 GPU VA=0，池管理器偏移 obj[8]−obj[0x20][0x10]（main+0x97ec90）= 0−1MB 对齐池基址 = -1MB，
   写循环 str w26,[x9,x8,lsl#2]（main+0x9a3efc）x9=池CPU基址+w21 越界 SIGSEGV。现按 donor hle_nvn.c:358/399-408
   存 pool/off 并以与 :145 nvn_buffer_get_address 相同的 CPU 回声约定取址（GPU 地址=池 mem+off，两差同源后
   偏移自然归位）。donor 无 GetTextureAddress（grep 空），自建。tex_h=donor tex_t 共享前缀去 serial
   （nvn_obj.h:35-39），只写游戏 NVNtexture 分配内（FITS(tex_t,NVNtexture)），本表内自洽读写。 */
typedef struct{uint32_t magic;nvn_texdesc d;uint8_t has_layout,layout[8];nvn_texlayout L;void *mem;void *pool;int64_t off;} tex_h;   /* donor tex_t 前缀 */
/* —— round23批2：纹理真建（VkImage）。句柄挂侧表、不动 tex_h 布局（tex_h 写在游戏 NVNtexture 分配内，
   FITS(tex_t,NVNtexture) 边界不扩）。上传触发：SMO boot 578 张 init、字节为游戏 CPU 直写池内存、
   无显式上传通知（真机 shell_round2_params.log :6865+ 时序；池 Flush 仅 init 前 2 次 :5984/:6448）——
   逐帧判脏重传属批6；本批 Initialize 一次性试传（覆盖先填后建）+ WriteTexels 写后重传（boot 实测 8 次小图）。
   Finalize=释放（donor hle_nvn.c:356 mtl_tex_release 同位；单体线 nvn_vulkan.c :1484 同义、:292 记录前端
   每帧 Finalize+Initialize 同池内存——同指针重 init 先毁旧图）。vk 未起（SMO_VK=0/无窗）＝批1 前记名行为。 */
#define TEX_SIDE_N 8192   /* M4o P2：1024 → 8192。m4o_b1 实测 VK 起后 ~16s 就「side table full (1024)」，
                             之后所有 SET_RENDER_TARGETS 的 tex0 全 miss（RT 尺寸退回 1600×900 探针、
                             UI 目标被塞回 1280×720 宿主图）。纯名字表，不占显存。 */
static tex_h *g_vk_tex_key[TEX_SIDE_N];
static nvn_vk_tex *g_vk_tex_val[TEX_SIDE_N];
static uint8_t g_vk_tex_up[TEX_SIDE_N];   /* M3y P2：本纹理是否已在 bind 时上送过（Initialize 的试传不算——那时像素可能还没写进池） */
static unsigned long long g_vk_tex_made;
/* M4c：RT 快照查询——只读侧表，不建对象、不碰字节；未命中返 NULL（句柄可能不是纹理）。 */
static const tex_h *vk_tex_key_hit(const void *p){const tex_h *t=p;if(!t)return NULL;
 for(unsigned i=0;i<TEX_SIDE_N;i++)if(g_vk_tex_key[i]==t)return t;return NULL;}
static int vk_tex_index(const tex_h *t){if(!t)return -1;for(unsigned i=0;i<TEX_SIDE_N;i++)if(g_vk_tex_key[i]==t)return (int)i;return -1;}
/* ---- M4k P1①：侧表 VkImage 不再在 Initialize 处急建/急传 ----
   事实（M4j 日志 + 源码交叉）：真正被 draw 采样的是 g_idvk[tid]（bind 路径按登记快照建图+上传）；
   g_vk_tex_val[] 这套「按纹理指针」的图只在 vk_tex_upload_try 里被自己上传一次，没有任何 draw
   绑定它。前端每帧 Finalize+Initialize 十几次 ⇒ 以前每帧都在做 create→upload→(destroy vkQueueWaitIdle)
   的空转，既占内存（图 + 上送字节）又整帧排空队列。
   现在：只登记指针（vk_tex_key_hit 仍要），图与上传改由 bind 路径负责；保留一处懒创建入口，
   万一将来有路径真的按指针取图（vk_tex_find），仍可在那里按需建。 */
static unsigned long long g_t_side_up_skip;
static unsigned long long g_t_side_evict;
static void vk_tex_attach(tex_h *t){
 /* M4q P1：去掉 `nvn_vk_is_up()` 门禁。这张表从 M4k 起就只是**名字表**（图/上传都归 bind 路径），
   再按 vk 是否起过来筛登记没有意义，却会永久漏掉「vk 起来之前 Initialize 过的对象」——
    这类对象之后不会被重新 Initialize，于是永远查不到（侧表 miss 的成因之一）。
   表满也不再「退化为纯名字」：改成环形覆盖最老的一条，保证任意时刻最近 N 个对象都可查。 */
 if(!t)return;
 for(unsigned i=0;i<TEX_SIDE_N;i++)if(g_vk_tex_key[i]==t)return;
 for(unsigned i=0;i<TEX_SIDE_N;i++)if(!g_vk_tex_key[i]){
  g_vk_tex_key[i]=t;g_vk_tex_up[i]=0;g_vk_tex_val[i]=NULL;return;}
 {static unsigned rr;unsigned i=rr++%TEX_SIDE_N;   /* 环形覆盖（同一指针被重新 Initialize 时会重新登记） */
  const tex_h *old=g_vk_tex_key[i];
  if(g_vk_tex_val[i]){nvn_vk_texture_forget(g_vk_tex_val[i]);nvn_vk_tex_destroy(g_vk_tex_val[i]);}
  g_vk_tex_key[i]=t;g_vk_tex_up[i]=0;g_vk_tex_val[i]=NULL;
  if(g_t_side_evict++<3)
   fprintf(stderr,"[nvn] vk tex side table full (%d) -> evict slot %u (old key=%p)\n",(int)TEX_SIDE_N,i,(const void*)old);}
}
static void vk_tex_detach(tex_h *t){if(!t)return;for(unsigned i=0;i<TEX_SIDE_N;i++)if(g_vk_tex_key[i]==t){if(g_vk_tex_val[i]){nvn_vk_texture_forget(g_vk_tex_val[i]);nvn_vk_tex_destroy(g_vk_tex_val[i]);}g_vk_tex_key[i]=NULL;g_vk_tex_val[i]=NULL;return;}}
static void vk_tex_upload_try(tex_h *t){   /* 整图重传（WriteTexels 为局部写，区域级增量上传随批6 判脏一起做） */
 /* M4k P1①：Initialize 处的「试传」取消。它只能传到侧表那张没人采样的图上；真正被采样的
    g_idvk 图由 bind 路径首绑时整图上传（g_idup 门控），所以这里跳过不丢像素，只计数。 */
 g_t_side_up_skip++;
 (void)t;
}
/* ===== M4aj P1：Initialize 历史台账（判「基址过期」）=====
   M4ai 发现：同一个 NVNtexture 对象在 boot 内被反复 Initialize 到**不同 pool off**（4 KB 递增，
   像逐切片构建体积），而 id 槽（g_texid2[id]）是 RegisterTexture 那一刻从 texrec 拷的**一次拷贝**
   ⇒ 对象之后换 off，id 槽不会跟。若游戏写的是后来的 off，我们读的早期 off 就整块全零 ——
   这与「游戏没写」在 [tp1] 上完全同形，只能用「同一对象全部历史 off 逐个扫 nz」来分开。
   本表按对象指针记完整历史（seq/off/mem/bytes/fmt/ext），RegisterTexture 命中目标 id 时逐 off 扫载荷。
   零渲染风险：只读内存、只打日志；历史行按 id 限流（≤32 行/次，≤2 次/ id）。 */
#define IH_EV_N 32
typedef struct{uint64_t seq;int64_t off;void *mem;uint64_t bytes;uint32_t w,h,d;uint16_t lv,fmt;} ih_ev;
typedef struct{const void *obj;unsigned n;uint64_t n_all;ih_ev e[IH_EV_N];} ih_rec;
/* 三个池：boot 内 Initialize 40 万次，单池必被轮换冲掉（boot#1 实测 id=1900 的 n_ev=0）。
   3D 体纹理与大 2D（≥512×512）各给一个专属池，容量按「本作这类对象 ≤ 数十个」定，不会被小图挤掉。 */
#define IH_N_GEN 1024
#define IH_N_VOL 256
#define IH_N_BIG 192
static ih_rec g_ih[IH_N_GEN],g_ih_vol[IH_N_VOL],g_ih_big[IH_N_BIG];
static unsigned g_ih_used,g_ih_vol_used,g_ih_big_used;
static uint64_t g_ih_seq,g_ih_t0;
static uint64_t ih_now_ms(void){return clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000000ull;}
static ih_rec *ih_get_pool(ih_rec *tab,unsigned n,unsigned *used,const void *obj,int create){
 if(!obj)return NULL;
 unsigned m=n-1,h=(unsigned)(((uintptr_t)obj>>4)*2654435761u)&m;
 for(unsigned i=0;i<16;i++){ih_rec *r=&tab[(h+i)&m];
  if(r->obj==obj)return r;
  if(!r->obj){if(!create)return NULL;memset(r,0,sizeof *r);r->obj=obj;(*used)++;return r;}}
 if(!create)return NULL;
 {static unsigned rr;ih_rec *r=&tab[rr++&m];memset(r,0,sizeof *r);r->obj=obj;return r;}
}
static ih_rec *ih_slot(const void *obj,const nvn_texdesc *d,int create){
 if(d&&d->target==2)return ih_get_pool(g_ih_vol,IH_N_VOL,&g_ih_vol_used,obj,create);
 if(d&&(uint64_t)d->width*d->height>=262144ull)return ih_get_pool(g_ih_big,IH_N_BIG,&g_ih_big_used,obj,create);
 return ih_get_pool(g_ih,IH_N_GEN,&g_ih_used,obj,create);
}
/* 不知道描述符时三池全查（dump/复查路径）。 */
static ih_rec *ih_find(const void *obj){
 ih_rec *r=ih_get_pool(g_ih_vol,IH_N_VOL,&g_ih_vol_used,obj,0);if(r)return r;
 r=ih_get_pool(g_ih_big,IH_N_BIG,&g_ih_big_used,obj,0);if(r)return r;
 return ih_get_pool(g_ih,IH_N_GEN,&g_ih_used,obj,0);
}
static void ih_note(const void *obj,int64_t off,const void *mem,uint64_t bytes,const nvn_texdesc *d);
/* 「换过 off 的对象」单独钉一份（stale_base 候选），64 个对象 × 32 条。 */
#define IH_PIN_N 64
static ih_rec g_ih_pin[IH_PIN_N];static unsigned g_ih_pin_n;static unsigned long long g_ih_pin_drop;
static void ih_pin_take(const ih_rec *r){
 if(!r)return;
 for(unsigned i=0;i<g_ih_pin_n;i++)if(g_ih_pin[i].obj==r->obj)return;
 if(g_ih_pin_n>=IH_PIN_N){g_ih_pin_drop++;return;}
 g_ih_pin[g_ih_pin_n++]=*r;
}
static void ih_note(const void *obj,int64_t off,const void *mem,uint64_t bytes,const nvn_texdesc *d){
 if(!mem)return;
 ih_rec *r=ih_slot(obj,d,1);if(!r)return;
 if(!g_ih_t0)g_ih_t0=ih_now_ms();
 int off_changed=(r->n>0&&(r->e[r->n-1].off!=off||(uint64_t)(uintptr_t)r->e[r->n-1].mem!=(uint64_t)(uintptr_t)mem));
 ih_ev *e;
 if(r->n<IH_EV_N)e=&r->e[r->n++];
 else {memmove(&r->e[0],&r->e[1],sizeof(e[0])*(IH_EV_N-1));e=&r->e[IH_EV_N-1];}   /* 保最近 32 条 */
 e->seq=++g_ih_seq;e->off=off;e->mem=(void*)mem;e->bytes=bytes;
 e->w=d->width;e->h=d->height;e->d=d->depth;e->lv=(uint16_t)d->levels;e->fmt=(uint16_t)d->format;
 if(off_changed)ih_pin_take(r);
 r->n_all++;
}
/* 目标 id 的「注册后再次 Initialize」观察位（见 ih_reinit_probe） */
#define IH_WATCH_N 12
static struct{const void *obj;int id;unsigned lines;} g_ih_watch[IH_WATCH_N];
static unsigned g_ih_watch_n;
static void ih_watch_add(int id,const void *obj){
 if(!obj)return;
 for(unsigned i=0;i<g_ih_watch_n;i++)if(g_ih_watch[i].obj==obj)return;
 if(g_ih_watch_n<IH_WATCH_N){g_ih_watch[g_ih_watch_n].obj=obj;g_ih_watch[g_ih_watch_n].id=id;
  g_ih_watch[g_ih_watch_n].lines=0;g_ih_watch_n++;}
}
/* 前向声明：定义在 g_texid2 之后（要读 id 槽）。 */
static void ih_dump(int id,const char *why,const void *obj,uint64_t slot_mem,uint64_t slot_bytes,const nvn_texdesc *slot_d);
static void ih_reinit_probe(const void *t,int64_t off,const void *mem,uint64_t bytes,const nvn_texdesc *d);
static uint64_t ih_nz(const uint8_t *p,uint64_t n,uint64_t *first);
static void nvn_texture_builder_set_storage(void *b,void *pool,int64_t off){((tex_builder_h *)b)->pool=pool;((tex_builder_h *)b)->off=off;}   /* donor :358 */
/* M3y P2：宿主快照类型前置声明（定义在 Initialize 之后的登记表块；C11 允许重复 typedef）。 */
typedef struct{const void *key;nvn_texdesc d;void *mem;uint64_t avail;uint8_t pin;} texsnap;   /* M4u P1：pin=已被 TexturePoolRegisterTexture 认领，环形覆盖不碰 */
static texsnap *texrec_get(const void *key,int create);
/* M4ai P2：3D 体纹理的**旁路台账** obj→(mem/尺寸)。为什么不复用 texrec：
   boot#1 实测 `[gpu] clear_texture dst id=-1` —— copy/clear 的目标纹理指针在 texrec 的环形表里
   已经被挤掉了（游戏 init 的纹理数远多于环长）。这里用一张只装 3D 图的小表，容量足够、不参与覆盖，
   让「游戏到底清/拷的是不是那 9 张体纹理」可以当场判。 */
#define VOL_LEDGER 256
static struct {const void *obj;void *mem;uint32_t w,h,d,lv;int tgt;unsigned fmt;} g_vol_led[VOL_LEDGER];
static unsigned g_vol_n;
static int g_vol_find(const void *obj,void **mem,uint32_t *w,uint32_t *h,uint32_t *d,unsigned *fmt){
 for(unsigned i=0;i<g_vol_n;i++)if(g_vol_led[i].obj==obj){
  if(mem)*mem=g_vol_led[i].mem;if(w)*w=g_vol_led[i].w;if(h)*h=g_vol_led[i].h;
  if(d)*d=g_vol_led[i].d;if(fmt)*fmt=g_vol_led[i].fmt;return 1;}
 return 0;
}
static unsigned long long g_pf_texinit_t0;
static void pf_texinit_begin(void){
 struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
 g_pf_texinit_t0=(unsigned long long)ts.tv_sec*1000000ull+(unsigned long long)ts.tv_nsec/1000ull;}
static void pf_texinit_end(void){
 extern void nvn_vk_perf_tex_init(unsigned long long);
 struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
 unsigned long long now=(unsigned long long)ts.tv_sec*1000000ull+(unsigned long long)ts.tv_nsec/1000ull;
 if(g_pf_texinit_t0&&now>=g_pf_texinit_t0)nvn_vk_perf_tex_init(now-g_pf_texinit_t0);}   /* M6e：只累加，不改行为 */
static uint64_t nvn_texture_initialize(void *t,const void *b){
 pf_texinit_begin();
 const tex_builder_h *s=b;tex_h *o=t;memset(o,0,sizeof *o);o->magic=0x54455800;o->d=s->d;o->pool=s->pool;o->off=s->off;o->has_layout=s->has_layout;memcpy(o->layout,s->layout,8);   /* 'TEX\0'，donor :405 */
 {const pool_h *p=s->pool;o->mem=p&&p->mem?(char *)p->mem+(ptrdiff_t)o->off:0;}   /* donor :408：CPU 回声 */
 /* M4aj P1：记初始化历史（每对象全部 off），并对「已注册 id 的对象」做再初始化取证。 */
 {nvn_texlayout Lh;nvn_tex_layout(&o->d,&Lh);
  ih_note(t,s->off,o->mem,Lh.size,&o->d);
  if(g_ih_watch_n)ih_reinit_probe(t,s->off,o->mem,Lh.size,&o->d);}
 /* M4ai P2b：3D 体纹理的 **storage 指针对账**。回放侧用的是 Initialize 那一刻快照的 o->mem
    （下面 `r->mem=o->mem`，同一表达式），这里把三样一起打出来，让「我们扫的地址」与
    「游戏 TextureInitialize 给的地址」可以逐字节对上：
      obj=游戏 NVNtexture 对象, mem=池基址+off（= 快照 mem）, pool=池基址, poolsz=池长, off=块线性偏移。
    限流：前 24 张 3D 图。 */
 if(o->d.target==2){
  /* M4ai P2：同一 obj 会被反复 Initialize（游戏常态）⇒ 就地更新，不重复占位 */
  int hit=-1;for(unsigned i=0;i<g_vol_n;i++)if(g_vol_led[i].obj==t){hit=(int)i;break;}
  if(hit<0&&g_vol_n<VOL_LEDGER)hit=(int)g_vol_n++;
  if(hit>=0){g_vol_led[hit].obj=t;g_vol_led[hit].mem=o->mem;g_vol_led[hit].w=o->d.width;
   g_vol_led[hit].h=o->d.height;g_vol_led[hit].d=o->d.depth;g_vol_led[hit].lv=o->d.levels;
   g_vol_led[hit].tgt=o->d.target;g_vol_led[hit].fmt=(unsigned)o->d.format;}
  {static unsigned nv;if(nv<24){const pool_h *pp=s->pool;nv++;
  vk_boot_logf("[t] vol init obj=%p mem=%p pool=%p poolsz=%llu off=%#llx %ux%ux%u lv=%u fmt=0x%x",
   (void*)t,(void*)o->mem,pp?(void*)pp->mem:NULL,pp?(unsigned long long)pp->size:0ull,
   (unsigned long long)s->off,o->d.width,o->d.height,o->d.depth,o->d.levels,(unsigned)o->d.format);}}}
 {static unsigned long long n;if(n<32||n%8192==0)fprintf(stderr,"[nvn] Texture %p init pool=%p off=%#llx -> mem=%p (%llu-th, log rate-limited: boot 实测每帧 ~12 次属游戏正常重建，旧逐条打印 6 分钟 28 万行撑爆日志)\n",t,s->pool,(unsigned long long)s->off,o->mem,n);n++;}
 {static unsigned long long n;   /* M3y P2：纹理描述符真值（前 64 张 + 每 512 张一条；判「有没有纹理/什么格式」） */
  if(n<64||n%512==0)vk_boot_logf("[t] tex init #%llu %p %ux%ux%u lv=%u fmt=0x%x tgt=%d fl=%#x smp=%d pool=%p off=%#llx mem=%p swz=%u%u%u%u bl=%d bpp=%d",
   n,(void*)t,o->d.width,o->d.height,o->d.depth,o->d.levels,o->d.format,o->d.target,o->d.flags,o->d.samples,
   s->pool,(unsigned long long)s->off,(void*)o->mem,
   o->d.swizzle_r,o->d.swizzle_g,o->d.swizzle_b,o->d.swizzle_a,
   nvn_tex_is_linear(&o->d)?0:1,nvn_fmt_bpb(o->d.format,NULL,NULL));   /* M4y P2：线性/块线性与 bpp 台账（bl=1 ⇒ BLOCK_LINEAR） */
  n++;}
 vk_tex_attach(o);   /* 批2：真建 VkImage（vk up 时；失败由 nvn_vk_tex_create 逐类红行） */
 vk_tex_upload_try(o);   /* 批2：一次性试传（块线性经 deswizzle；先建后填的像素由批6 判脏补传） */
 /* M3y P2：宿主快照（descriptor + 池基址 + 可用字节）。回放侧只信这份，不再回读游戏对象内存。 */
 {texsnap *r=texrec_get(t,1);
  if(r){r->d=o->d;r->mem=o->mem;
   nvn_texlayout L;nvn_tex_layout(&o->d,&L);uint64_t avail=L.size;
   const pool_h *p=s->pool;
   if(p&&p->mem&&p->size){uint64_t off=(uint64_t)s->off;if(off<p->size&&p->size-off<avail)avail=p->size-off;}
   r->avail=avail;}}
 pf_texinit_end();
 return 1;
}
/* M3y P2：纹理池登记表（id → 托管快照）。此前 RegisterTexture 是「记名空操作」，
   于是回放侧拿到的 NVNtextureHandle（= tid | sid<<20，见 nvn_device_get_texture_handle）无从还原成
   纹理对象，只能一律采样 1x1 白占位 —— 这就是「整屏纯白」的最后一环。
   NVN 语义：nvnDeviceGetTextureHandle(device, textureId, samplerId) 的 textureId 指的是
   「当前纹理池第 textureId 槽」的登记项；池槽由 nvnTexturePoolRegisterTexture(pool,id,tex,view) 填。
   ★ boot#1 实测（crash 05:09:03 @0x9）：游戏会把同一个 NVNtexture 指针反复 Initialize 成不同纹理
   （log: init #3..#11 全是 0x7f46632bf0），且它的对象内存只在 Initialize 那一刻由我们写过；
   回放时再按指针读字段会读到游戏自己的后续覆盖（tex->pool 实测=1 ⇒ [1+8] SEGV）。
   因此这里 **不再回读游戏对象**：Initialize 时把 descriptor/池基址/可用字节/侧表 VkImage 快照进
   宿主 texrec（按指针），RegisterTexture 时把快照拷进 id 槽。回放只吃宿主数据。 */
#define TEXREG_N 65536   /* M4v P3：16384 → 65536。m4u_b2 实测 id 空间已跑到 16384..17407（`unres=1024` 全是
                           `TexturePoolRegister id=… out of table`），16384 已被用穿。代价 ≈ +8.5 MB BSS
                           （g_texid2/g_texrec 各 72B×65536，g_texreg 16B×65536，g_idvk/g_idup/g_t_idbinds 同步）。
                           注意：表变大后**所有 O(TEXREG_N) 扫描都必须消掉**（见 g_texkey[] 哈希与 g_rt_best）。 */
typedef struct{const void *pool;int id;const void *key;} texreg_e;
static texreg_e g_texreg[TEXREG_N];
static unsigned g_texreg_n;
static texsnap g_texid2[TEXREG_N];               /* id → 登记时刻的纹理快照（回放解析用） */
static texsnap g_texrec[TEXREG_N];                   /* 游戏纹理指针 → 最近一次 Initialize 的快照 */
static unsigned g_texrec_n;
static unsigned long long g_texreg_calls,g_texreg_collisions,g_texrec_full,g_texrec_evict;
static unsigned g_texrec_rr;   /* 满表后的环形覆盖游标（M4q P1） */
/* M4u P1：三条计数（口径见 [t] unres fix 行）。init=Initialize 快照直接命中；reg=注册期对象直读兜底；
   bad=两路都拿不到；pin_evict=被登记认领的条目仍被环形覆盖挤掉（应为 0）。 */
static unsigned long long g_t_unres_fix_init,g_t_unres_fix_reg,g_t_unres_fix_bad,g_t_pin_evict;
/* M4u P1：注册期被丢弃的 id 位图（64 K）。绑定侧 unres 时用它区分两种病根：
   「注册过但没快照被丢」还是「压根没注册」。64 K 覆盖实测 id 上限（22528）。 */
static unsigned char g_regdrop[65536/8];
static void reg_drop_mark(int id){if(id>=0&&id<65536)g_regdrop[id>>3]|=(unsigned char)(1u<<(id&7));}
static int reg_drop_test(int id){return id>=0&&id<65536?(g_regdrop[id>>3]>>(id&7))&1:0;}
static texsnap *texrec_get(const void *key,int create){
 for(unsigned i=0;i<g_texrec_n;i++)if(g_texrec[i].key==key)return &g_texrec[i];
 if(!create)return NULL;
 /* M4q P1：表满改为环形覆盖最老的一条，而不是「之后的新对象永远没有快照」。
   本作 boot 内 Initialize 40 万次（tex init #399872 仍在打），地址在小集合里轮转，
   但「先到先占」的旧策略在满表后对新地址一律返回 NULL ⇒ RT 解析链第二级整段失效。
   M4u P1：已被 TexturePoolRegisterTexture 认领的条目（pin=1）不参与覆盖——它的指针→快照
   是某个 id 槽的唯一来源，冲掉就等价于「注册过又丢」；游标最多跳 64 格找未认领的，
   全被认领时才退回覆盖游标位（计 pin_evict，正常应为 0）。 */
 texsnap *r;
 const unsigned cap=(unsigned)(sizeof g_texrec/sizeof *g_texrec);
 if(g_texrec_n<cap)r=&g_texrec[g_texrec_n++];
 else {r=NULL;
       for(unsigned t=0;t<64&&t<cap;t++){texsnap *c=&g_texrec[(g_texrec_rr+t)%cap];
        if(!c->pin){r=c;g_texrec_rr=(g_texrec_rr+t+1)%cap;break;}}
       if(!r){r=&g_texrec[g_texrec_rr%cap];g_texrec_rr=(g_texrec_rr+1)%cap;g_t_pin_evict++;}
       g_texrec_full++;
       if(g_texrec_evict++<3)fprintf(stderr,"[nvn] texrec full (%u) -> ring reuse slot %u\n",cap,g_texrec_rr%cap);}
 memset(r,0,sizeof *r);r->key=key;return r;
}
/* ---- M4aj P1：初始化历史导出 + 判定（定义放在 g_texid2 / texrec_get 之后） ---- */
static uint64_t ih_nz(const uint8_t *p,uint64_t n,uint64_t *first){
 uint64_t nz=0,i=0;if(first)*first=(uint64_t)-1;
 if(!p)return 0;
 for(;i+8<=n;i+=8){uint64_t w;memcpy(&w,p+i,8);if(!w)continue;
  for(unsigned k=0;k<8;k++)if(p[i+k]){if(first&&*first==(uint64_t)-1)*first=i+k;nz++;}}
 for(;i<n;i++)if(p[i]){if(first&&*first==(uint64_t)-1)*first=i;nz++;}
 return nz;
}
/* 一个 id 的完整初始化历史 + 每个历史 off **自己载荷范围**的 nz。
   判定口径（分层，避免 M4ai 那种「相邻分配尾巴」假阳性）：
     used_nz>0 且 used 是最早那条      → first_off_ok（我们读的位置本来就有数据）
     used_nz>0 但 used 是较晚那条      → used_late_ok（位置对，但不是第一份）
     used_nz==0 且 其它 off 有数据      → stale_base（基址过期：游戏写的是别的 off）
     全部 off 全 0                     → all_zero（游戏确实没写这块，转查「谁该写」） */
static void ih_dump(int id,const char *why,const void *obj,uint64_t slot_mem,uint64_t slot_bytes,const nvn_texdesc *slot_d){
 ih_rec *r=ih_find(obj);
 vk_boot_logf("[hist] id=%d why=%s obj=%p slot_mem=%#llx slot_bytes=%llu fmt=%#x ext=%ux%ux%u lv=%u n_ev=%u n_all=%llu",
  id,why,(void*)obj,(unsigned long long)slot_mem,(unsigned long long)slot_bytes,
  slot_d?(unsigned)slot_d->format:0u,slot_d?slot_d->width:0u,slot_d?slot_d->height:0u,slot_d?slot_d->depth:0u,
  slot_d?(unsigned)slot_d->levels:0u,r?r->n:0u,r?(unsigned long long)r->n_all:0ull);
 if(!r){vk_boot_logf("[hist] id=%d verdict=no_history (该对象未过宿主 Initialize 或历史已被挤掉)",id);return;}
 uint64_t best_nz=0;int used_idx=-1,best_idx=-1;
 for(unsigned i=0;i<r->n;i++){
  const ih_ev *e=&r->e[i];uint64_t first=0;
  uint64_t nz=ih_nz((const uint8_t*)e->mem,e->bytes,&first);
  int used=((uint64_t)(uintptr_t)e->mem==slot_mem);
  if(used)used_idx=(int)i;
  if(nz>best_nz){best_nz=nz;best_idx=(int)i;}
  vk_boot_logf("[hist] id=%d seq=%llu%s off=%#llx mem=%p bytes=%llu fmt=%#x ext=%ux%ux%u lv=%u t=%llu nz=%llu/%llu first_nz=%lld",
   id,(unsigned long long)e->seq,used?" USED":"",(unsigned long long)e->off,(void*)e->mem,
   (unsigned long long)e->bytes,(unsigned)e->fmt,e->w,e->h,e->d,(unsigned)e->lv,
   (unsigned long long)((ih_now_ms()>g_ih_t0)?(ih_now_ms()-g_ih_t0)/1000ull:0ull),
   (unsigned long long)nz,(unsigned long long)e->bytes,first==(uint64_t)-1?-1ll:(long long)first);
 }
 uint64_t used_nz=ih_nz((const uint8_t*)(uintptr_t)slot_mem,slot_bytes,NULL);
 const texsnap *live=texrec_get(obj,0);
 const char *verdict = used_nz>0 ? (used_idx==0?"first_off_ok":"used_late_ok")
                     : (best_nz>0?"stale_base":"all_zero");
 vk_boot_logf("[hist] id=%d verdict=%s best_off=%#llx best_nz=%llu used_off=%#llx used_nz=%llu/%llu live_mem=%p live_same_as_slot=%d",
  id,verdict,best_idx>=0?(unsigned long long)r->e[best_idx].off:0ull,(unsigned long long)best_nz,
  used_idx>=0?(unsigned long long)r->e[used_idx].off:0ull,(unsigned long long)used_nz,(unsigned long long)slot_bytes,
  live?(void*)live->mem:NULL,(live&&(uint64_t)(uintptr_t)live->mem==slot_mem)?1:0);
}
/* 注册之后再 Initialize 同一对象（可能换到新 off）→ 这是我们 id 槽会「过期」的唯一入口。
   限流：每个观察对象 ≤10 行。同一行同时给出「新地址 nz」与「id 槽地址 nz」，可直接判是否过期。 */
static unsigned long long g_ih_reinit_n,g_ih_reinit_stale;
static void ih_reinit_probe(const void *t,int64_t off,const void *mem,uint64_t bytes,const nvn_texdesc *d){
 for(unsigned i=0;i<g_ih_watch_n;i++){
  if(g_ih_watch[i].obj!=t)continue;
  int id=g_ih_watch[i].id;const texsnap *slot=(id>=0&&id<TEXREG_N)?&g_texid2[id]:NULL;
  uint64_t nz=ih_nz((const uint8_t*)mem,bytes,NULL);
  uint64_t snz=slot?ih_nz((const uint8_t*)slot->mem,slot->avail,NULL):0;
  int same=(slot&&(uint64_t)(uintptr_t)slot->mem==(uint64_t)(uintptr_t)mem);
  g_ih_reinit_n++;if(!same)g_ih_reinit_stale++;
  if(g_ih_watch[i].lines<10){g_ih_watch[i].lines++;
   vk_boot_logf("[hist] id=%d reinit#%u off=%#llx mem=%p bytes=%llu fmt=%#x ext=%ux%ux%u lv=%u nz=%llu/%llu same_as_slot=%d slot_mem=%p slot_nz=%llu/%llu t=%llu",
    id,g_ih_watch[i].lines,(unsigned long long)off,(void*)mem,(unsigned long long)bytes,(unsigned)d->format,
    d->width,d->height,d->depth,(unsigned)d->levels,(unsigned long long)nz,(unsigned long long)bytes,same?1:0,
    slot?(void*)slot->mem:NULL,(unsigned long long)snz,(unsigned long long)(slot?slot->avail:0),
    (unsigned long long)((ih_now_ms()>g_ih_t0)?(ih_now_ms()-g_ih_t0)/1000ull:0ull));}
  return;}
}
static const texsnap *texreg_lookup(int id){return (id>=0&&id<TEXREG_N&&g_texid2[id].key)?&g_texid2[id]:NULL;}
/* M4aj P1：换过 off 的对象在 boot 早段一次性导出（前 6 个，每个 ≤16 行）。
   这是「逐切片构建体积」那批的直接证据：同一 obj 的每次 off 各扫一遍 nz。 */
static int g_ih_pins_dumped;
static void ih_dump_pins(unsigned sub){
 if(g_ih_pins_dumped)return;
 g_ih_pins_dumped=1;
 unsigned lim=g_ih_pin_n<6?g_ih_pin_n:6;
 vk_boot_logf("[hist] pins @submit#%u: n=%u dropped=%llu (换过 off 的对象；逐 off 扫 nz)",sub,g_ih_pin_n,g_ih_pin_drop);
 for(unsigned k=0;k<lim;k++){
  const ih_rec *r=&g_ih_pin[k];if(!r->obj||!r->n)continue;
  uint64_t best=0;int bi=-1;
  vk_boot_logf("[hist] pin obj=%p n_ev=%u n_all=%llu ext(first)=%ux%ux%u lv=%u fmt=%#x",
   (void*)r->obj,r->n,(unsigned long long)r->n_all,r->e[0].w,r->e[0].h,r->e[0].d,(unsigned)r->e[0].lv,(unsigned)r->e[0].fmt);
  for(unsigned i=0;i<r->n&&i<16;i++){
   const ih_ev *e=&r->e[i];uint64_t first=0;
   uint64_t nz=ih_nz((const uint8_t*)e->mem,e->bytes,&first);
   if(nz>best){best=nz;bi=(int)i;}
   vk_boot_logf("[hist] pin obj=%p seq=%llu off=%#llx mem=%p bytes=%llu nz=%llu/%llu first_nz=%lld",
    (void*)r->obj,(unsigned long long)e->seq,(unsigned long long)e->off,(void*)e->mem,
    (unsigned long long)e->bytes,(unsigned long long)nz,(unsigned long long)e->bytes,
    first==(uint64_t)-1?-1ll:(long long)first);
  }
  vk_boot_logf("[hist] pin obj=%p verdict=%s best_off=%#llx best_nz=%llu n_ev=%u",
   (void*)r->obj,best>0?"some_off_has_data":"all_offs_zero",
   bi>=0?(unsigned long long)r->e[bi].off:0ull,(unsigned long long)best,r->n);
 }
}
/* M4aj P1：节流汇总——同一 submit 节流点打一行。目标是区分两种「槽已过期」：
   ①登记后对象再 Initialize 到新 off（reinit_stale>0）；②槽 mem 与 texrec 最新 mem 不同（live 差）。
   两侧各扫 ≤64 KB 的 nz（够判 0/非 0，且不拖慢 submit）。 */
static void ih_report(unsigned sub){
 static unsigned last;
 int ids[3]={1900,1910,1911};
 if(sub>=200)ih_dump_pins(sub);
 char b[512];int off=0;
 for(int k=0;k<3;k++){
  int id=ids[k];const texsnap *sn=texreg_lookup(id);if(!sn||!sn->mem)continue;
  const texsnap *live=texrec_get(sn->key,0);
  uint64_t a=sn->avail<65536?sn->avail:65536;
  uint64_t sz=ih_nz((const uint8_t*)sn->mem,a,NULL);
  uint64_t lz=0,lm=0;
  if(live&&live->mem){lm=(uint64_t)(uintptr_t)live->mem;uint64_t lb=live->avail<65536?live->avail:65536;
   lz=ih_nz((const uint8_t*)live->mem,lb,NULL);}
  off+=snprintf(b+off,sizeof b-(size_t)off," id=%d slot_mem=%#llx slot_nz=%llu/%llu live_mem=%#llx live_nz=%llu live_same=%d",
   id,(unsigned long long)(uintptr_t)sn->mem,(unsigned long long)sz,(unsigned long long)sn->avail,
   (unsigned long long)lm,(unsigned long long)lz,
   (lm==(uint64_t)(uintptr_t)sn->mem)?1:0);
  if((size_t)off>=sizeof b)break;
 }
 last++;
 vk_boot_logf("[hist] totals @submit#%u #%u: objs=%u pools=%u/%u/%u events=%llu reinit=%llu reinit_stale=%llu%s",
  sub,last,g_ih_watch_n,g_ih_used,g_ih_vol_used,g_ih_big_used,(unsigned long long)g_ih_seq,(unsigned long long)g_ih_reinit_n,
  (unsigned long long)g_ih_reinit_stale,b);
}
/* M4c：RT 尺寸兜底来源。boot#1 实测 SET_RENDER_TARGETS 的 tex0=0x154455800 在侧表里查不到
   （`[h] rt bind ... snap=0x0`），它既不是已 attach 的 tex_h 指针，也不满足 handle(tid|sid<<20)
   解码（低 20 位 0x55800 越界 4096）。但游戏唯一的 10bit RT 形态在快照表里可判：
   fmt=0x3d(RGB10A2) + 1600x900。这里按「格式优先、面积最大」挑一张，命中即用其尺寸并记 id。
   只读表、不建对象；未命中返 NULL → 后端回落宿主图。下一轮应在 SetRenderTargets 录制端
   直接记 w/h（colors[i] 的对象内存里有 nvn_texdesc），彻底去掉这条启发式。 */
/* M4v P3：TEXREG_N 提到 65536 之后，这两条原本 O(TEXREG_N) 的扫描都变成热路径毒药
   （RT 绑定 ≈24 次/submit，扫 6.5 万条 × 每次 = 每秒上千万次比较）。改法：
     · g_rt_best：注册时增量维护「fmt=0x3d 且面积最大」的那一条，探针 O(1)；
     · g_texkey[]：key→id 开放寻址哈希，texreg_put 时写入，查询 O(1)（16 次探测上限）。
   语义保持不变：同 key 多个 id 时取最小 id（哈希里只在更小时改写）。 */
static const texsnap *g_rt_best;
static uint64_t g_rt_best_area;
static const texsnap *g_rt_snap_probe(uint32_t *pw,uint32_t *ph,int *pid){
 /* 槽可能被后来的注册覆盖成别的纹理：用前重校验，失效就重新挑一次（不是热路径，走不到就 0 次） */
 if(!g_rt_best||!g_rt_best->key||g_rt_best->d.format!=0x3d){
  g_rt_best=NULL;g_rt_best_area=0;
  for(int i=0;i<TEXREG_N;i++){const texsnap *s=&g_texid2[i];
   if(!s->key||!s->d.width||!s->d.height||s->d.format!=0x3d)continue;
   uint64_t a=(uint64_t)s->d.width*s->d.height;
   if(!g_rt_best||a>g_rt_best_area){g_rt_best=s;g_rt_best_area=a;}}
 }
 if(!g_rt_best)return NULL;
 if(pw)*pw=g_rt_best->d.width;if(ph)*ph=g_rt_best->d.height;if(pid)*pid=(int)(g_rt_best-g_texid2);
 return g_rt_best;}
/* M4e：RT 句柄 → 纹理 id 的反查（侧表按 key 命中时用；不命中再走上面的探针）。
   只读表；同一 key 出现在多个 id 槽时取最小的（登记顺序稳定，取证可复现）。 */
#define TEXKEY_HASH 65536
static struct{const void *key;int id;} g_texkey[TEXKEY_HASH];
static void texkey_put(const void *key,int id){
 if(!key)return;
 unsigned h=(unsigned)(((uintptr_t)key>>4)*2654435761u)&(TEXKEY_HASH-1);
 for(unsigned i=0;i<16;i++){unsigned s=(h+i)&(TEXKEY_HASH-1);
  if(g_texkey[s].key==key){if(id<g_texkey[s].id)g_texkey[s].id=id;return;}
  if(!g_texkey[s].key){g_texkey[s].key=key;g_texkey[s].id=id;return;}}
 /* 16 次探测全满（65536 槽在 boot 内达不到）：放弃登记，查询会回落到线性扫 */
}
static int g_tex_id_by_key(const void *key){
 if(!key)return -1;
 unsigned h=(unsigned)(((uintptr_t)key>>4)*2654435761u)&(TEXKEY_HASH-1);
 for(unsigned i=0;i<16;i++){unsigned s=(h+i)&(TEXKEY_HASH-1);
  if(g_texkey[s].key==key)return g_texkey[s].id;
  if(!g_texkey[s].key)break;}
 for(int i=0;i<TEXREG_N;i++)if(g_texid2[i].key==key)return i;
 return -1;}
/* M4t P1：**按存储身份**反查 id。为什么需要它（m4s_b2 实测）：
   `[h] rt chg#1 … id=-1 … src=tex_h` —— RT 对象是游戏自己池里的 NVNtexture，magic 在、尺寸格式都在，
   但它不在 g_texrec（16 K 环形表，boot 内 40 万次 Initialize 早已把这条冲掉）也不在 g_texid2 的 key 上
   ⇒ 槽永远拿不到 tex_id。而 RT 的**存储身份**（池数据地址 0x6e32440000）与 id 282 的快照 mem 完全相等，
   所以按 mem + 描述符反查是精确判据（不是猜）。
   mismatch_fmt/dim>0 表示「mem 命中了但描述符不一致」（同址被复用成别的纹理），此时不认。 */
static unsigned long long g_t_id_by_store_hits, g_t_id_by_store_miss, g_t_id_by_store_bad;
/* 结果按 (mem,w,h,fmt) 记忆：RT 存储身份集合实测 ≤8 个，扫描 16 K 表只发生在「新身份」上，
   否则每次 RT 绑定都扫一遍会把 1000+/s 的绑定路径打穿。 */
static struct { const void *mem; unsigned w,h,fmt; int id; } g_idstore_memo[16];
static unsigned g_idstore_n;
static int g_tex_id_by_store(const void *mem,unsigned w,unsigned h,unsigned fmt){
 if(!mem)return -1;
 for(unsigned i=0;i<g_idstore_n;i++)
  if(g_idstore_memo[i].mem==mem&&g_idstore_memo[i].w==w&&g_idstore_memo[i].h==h&&g_idstore_memo[i].fmt==fmt)
   return g_idstore_memo[i].id;
 int hit=-1;int bad=0;
 for(int i=0;i<TEXREG_N;i++){const texsnap *s=&g_texid2[i];
  if(!s->key||s->mem!=mem)continue;
  if(w&&s->d.width!=w)continue;
  if(h&&s->d.height!=h)continue;
  if(fmt&&(unsigned)s->d.format!=fmt){bad++;continue;}
  if(hit<0||i<hit)hit=i;}
 if(hit>=0)g_t_id_by_store_hits++;else if(bad)g_t_id_by_store_bad++;else g_t_id_by_store_miss++;
 if(g_idstore_n<sizeof g_idstore_memo/sizeof *g_idstore_memo){
  g_idstore_memo[g_idstore_n].mem=mem;g_idstore_memo[g_idstore_n].w=w;
  g_idstore_memo[g_idstore_n].h=h;g_idstore_memo[g_idstore_n].fmt=fmt;
  g_idstore_memo[g_idstore_n].id=hit;g_idstore_n++;}
 return hit;}
/* M3y P2 第二版（boot#3 待验）：id 自建 VkImage。
   boot#2 证据：两类纹理会撞在同一个 NVNtexture 地址上——
     init #29 0x7f46672e30 1600x900 fmt=0x3d poolA / reg id=282
     init #30 0x7f46672e30 1280x720 fmt=0x3d poolB          ← 同址换成另一张（多缓冲/换尺寸）
   于是「按指针查侧表图」只剩最后一张，id=282 的快照（1600x900/poolA）与图（1280x720/poolB）冲突，
   被降级成 1x1 白占位 —— 正是全屏 blit 每帧绑的那张（handle 0x1020011a），所以回读仍纯白。
   修法：登记时刻的快照自己拥有 VkImage（按 id，不按指针）。上限 64 张 / 128MB，超了该 id 回占位并计数。 */
static nvn_vk_tex *g_idvk[TEXREG_N];
static unsigned char g_idup[TEXREG_N];
/* M4bk P1：每 id 最近一次「有没有真数据 / 为什么没有」的归因码（1=已上送；负数=原码）。
   只服务 nvn_vk 的 [m] sentinel 行，不参与任何判决。 */
static signed char g_t_idwhy[TEXREG_N];
static unsigned long long g_t_idimg,g_t_idimg_cap;
/* ================= M40AE (2026-11-10)：按客体存储共享 VkImage + refcount 别名 =================
   依据（M40AD 实证，本轮不重查）：同一块客体存储被反复分配新池槽号 ——
   `key=0x6e3f46a200/1600x900/0x25` 建图 63 次拿 62 个不同 id；remap/keys=56% ⇒ 每帧建 ~5.7 张新图。
   做法：按 (mem, w, h, fmt) 把「同一块存储」塌成**一张** VkImage，多个 id 只做别名（refs++）。
   五条落点逐条落实（M40AD 给的坐标）：
     ① 建图点 `g_idvk[tid]=vk;`：前面先查缓存，命中即别名（不再 vkCreateImage，g_t_idimg 不 ++）；新建后登记 refs=1。
     ② 淘汰 t_evict_one：按**图**为单位（任一名在槽里引用就不动），整张销毁时清零所有别名 id。
     ③ 名额 k_id_img_max 语义 = **唯一图数**（别名命中不占名额）。
     ④ 上传门控 g_idup 挂到图上：图已上送 ⇒ 所有别名一律不整图重传（否则 N 个别名各传一次反而更慢）；
        图未上送 ⇒ 第一个来的 id 传，传完置 e->up=1（只让首个 id 传、其余留空会花屏 = AE3）。
     ⑤ texreg_put 换描述符（:1424）/ 单 id 释放：refs--，只有 refs==0 才真销毁。
   开关 smo_m40ae（构造期自种 0666 文件，2 s 重读；缺省 0 ⇒ 关态逐字节原行为、零 [m40ae] 输出）。 */
#define M40AE_SW_A "/data/storage/el2/base/files/smo_m40ae"
#define M40AE_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40ae"
#define M40AE_EN 768         /* 唯一图条目表：条目只是记账，VkImage 个数仍受 k_id_img_max 限 */
#define M40AE_HIT_LINE_MAX 24
typedef struct {
 unsigned long long mem; unsigned w,h,fmt;
 nvn_vk_tex *vk; unsigned refs; unsigned char up;
 uint64_t fp; unsigned long long bytes;
} m40ae_img;
static m40ae_img g_m40ae_img[M40AE_EN];
static int g_m40ae_eid[TEXREG_N];   /* id → 图条目下标（-1 = 非共享图 / 未登记 ⇒ 走原逐 id 路径） */
static unsigned g_m40ae_nuniq,g_m40ae_hit,g_m40ae_skipup,g_m40ae_evimg,g_m40ae_hitlines;
static unsigned g_m40ae_builds,g_m40ae_keys,g_m40ae_iduniq,g_m40ae_idseen[TEXREG_N/8];
static long g_m40ae_t0;
static int m40ae_on(void)
{
 static int cached=-1; static long last;
 long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40AE_SW_B,M40AE_SW_A};   /* M40AF ③：B=/data/app/… 是 hdc 唯一可写视图 ⇒ 优先读，才能 boot 前置 1 */
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached&1;
}
/* ================= M40AG：smo_m40ag 开关 + smo_m40ag_cap 名额覆盖 =================
   B 路径优先 = /data/app/el2/100/base/com.vibecc.smo/files/…（hdc 唯一可写视图）。
   关态（smo_m40ag 缺省 0 / cap 文件缺省 0）⇒ 逐字节原行为、零 [m40ag] 输出。 */
#define M40AG_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40ag"
#define M40AG_CAP_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40ag_cap"
#define M40AG_SW_A "/data/storage/el2/base/files/smo_m40ag"      /* app 沙箱视图（= 同一物理目录；构造期 app 只能在此种植） */
#define M40AG_CAP_A "/data/storage/el2/base/files/smo_m40ag_cap"
static int m40ag_on(void)
{
 static int cached=-1;static long last;long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40AG_SW_B,M40AG_SW_A};   /* B=/data/app/… 优先（hdc 可写视图），A=/data/storage/… 兜底 */
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached&1;
}
static unsigned m40ag_cap(void)   /* 0 = 不改（沿用 m30a_q/m40o 的结果）；384..1024 才生效 */
{
 static unsigned cached;static long last;long now=(long)time(NULL);
 if(now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40AG_CAP_B,M40AG_CAP_A};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;int v=atoi(b);if(v>=384&&v<=1024)cached=(unsigned)v;}break;}}}
 return cached;
}
/* M40AG ①：两个开关文件必须由 app 自种 0666（hdc 无法在该目录新建）。
   ⚠ 必须走 A 视图（/data/storage/el2/base/files）：构造期 app 在 B 视图（/data/app/…）open(O_CREAT) 被拒
   （b0' 实测：seed launch 43 s、pid 存活、[beat] 164 行，但 B 视图两文件始终 MISSING）。
   A/B 是同一物理目录（证据：app 写 A 视图 vk_boot.log，hdc 在 B 视图读到同一份）⇒ 种 A 后 hdc 即可写 B。 */
__attribute__((constructor)) static void m40ag_seed(void)
{
 static const char *const p[4]={M40AG_SW_A,M40AG_CAP_A,M40AG_SW_B,M40AG_CAP_B};
 for(int i=0;i<4;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}
/* ================= M103A (2026-11-13)：D1 修复 —— id 图名额 384→4096 + 活体字节硬顶 512 MiB ======
   已定谳（M102C，logs/m102c/report.md §3–§5；本单不重验）：
     · `[m7k] ph` 全 boot 80/80 行 why=-31，唯一触发 = `g_t_idimg>=k_id_img_max && nvn_vk_is_up()`；
       3D 段每行 `idimg=384/384`，菜单段 310 s 才 29/384 ⇒ **384 是 3D 段顶满的**，不是全程顶满。
     · 让位在跑（3D 段 ≥16 次真换出 512 B–702 KiB 小图），但被「每 submit ≤8 张」配额挡下。
     · 后果：3D 段要贴图的 id 拿不到 VkImage ⇒ 槽绑 1×1 白（ds 末组 kind=4 占 67.5%：id 2952/2953/
       2954，1280×160 fmt=0x25）⇒ 画面仍是「纯黑底 + 纯黄几何」（15.89%）。
   本开关三件事（**缺省开**；写 0 = 秒级回退到 M102C 口径：名额 384 / 每 submit ≤8 / 不判字节顶）：
     ① 名额 384 → 4096（M103A_CAP）。VkImage 仍**按需建**（唯一建图点 = g_rp_bind_tex 里的
        `nvn_vk_tex_create`，不是开机预分配 4096 张）⇒ 名额只是「缓存容量」，不预占显存。
     ② 活体字节硬顶 M103A_BYTES_MAX = 512 MiB。账 = `g_t_bytes_live`（上送成功 +=bytes、让位换出
        -=vkbytes，M5z P1 起就是这个口径）。越顶 ⇒ 走同一条 LRU 让位（只换「本帧没有任何采样槽
        引用」的图，优先 小图 → 低 binds → 最老）；换不动 ⇒ 拒建图并记 why=-36（g_m103a_deny_bytes）。
        **不允许无上限增长**：上界 = min(4096 张, 512 MiB / 单张)，单张另有 32 MB 上送门兜底。
     ③ 每 submit 让位配额 8 → 64（M103A_EVICT_PER_SUB），并且**只在真压顶时消耗配额**。
        旧代码无条件求值 `ev_sm_ok = ev_small && t_evict_slot_small();` ⇒ 每笔小图 bind 都吃掉一次
        配额，deny_s 涨的是记账假象（M102C 的 16 次真换出 vs deny_s +90 是同一现象的两面）。
   读法：文件 2 s 重读（构造期自种 0666），写 0/1 秒级生效，不用重装。
   与历史旋钮的关系：M30A(384→1024)/M40O(384→1536)/M40AG(cap 覆盖) 是同一根旋钮的旧版本；
   本开关**优先级最高**，关态才回落到那条旧链（逐字不改）。 */
#define M103A_SW_A "/data/storage/el2/base/files/smo_m103a"
#define M103A_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m103a"
#define M103A_CAP 4096u
#define M103A_BYTES_MAX (512ull<<20)
#define M103A_EVICT_PER_SUB 64u
#define M103A_RECLAIM_PER_SUB 8u   /* 回收 tick：每次 bind 最多再换出几张（建图判决处那一次另算） */
static int m103a_on(void)
{
 static int cached=-1;static long last;long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M103A_SW_B,M103A_SW_A};   /* B=/data/app/…（hdc 唯一可写视图）优先，A 兜底 */
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached&1;
}
__attribute__((constructor)) static void m103a_seed(void)
{
 /* 只能在 A 视图建（构造期 app 在 B 视图 O_CREAT 被拒，M40AG b0' 实证）；缺省 1 = 本单要的修复态。 */
 static const char *const p[2]={M103A_SW_A,M103A_SW_B};
 for(int i=0;i<2;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"1\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}
/* 缓存查找：同存储 + 同描述符（含 levels/flags/target/samples）才算同一张图 ⇒ 别名安全。 */
static int m40ae_find(unsigned long long mem,const nvn_texdesc *d)
{
 if(!mem||!d)return -1;
 for(int i=0;i<M40AE_EN;i++){
  m40ae_img *e=&g_m40ae_img[i];
  if(!e->vk||!e->refs)continue;
  if(e->mem!=mem||e->w!=d->width||e->h!=d->height||e->fmt!=(unsigned)d->format)continue;
  if(!nvn_vk_tex_matches_desc(e->vk,d))continue;
  return i;
 }
 return -1;
}
static int m40ae_reg(int tid,unsigned long long mem,unsigned w,unsigned h,unsigned fmt,nvn_vk_tex *vk)
{
 if(tid<0||tid>=TEXREG_N||!vk)return -1;
 g_m40ae_builds++;
 if(!(g_m40ae_idseen[tid>>3]&(unsigned char)(1u<<(tid&7)))){
  g_m40ae_idseen[tid>>3]|=(unsigned char)(1u<<(tid&7));g_m40ae_iduniq++;}
 for(int i=0;i<M40AE_EN;i++){
  m40ae_img *e=&g_m40ae_img[i];
  if(e->vk&&e->refs)continue;
  e->mem=mem;e->w=w;e->h=h;e->fmt=fmt;e->vk=vk;e->refs=1;e->up=0;e->fp=0;e->bytes=0;
  g_m40ae_eid[tid]=i;g_m40ae_nuniq++;g_m40ae_keys++;
  return i;
 }
 return -1;   /* 表满：当作没有共享，退回原逐 id 行为（g_idvk 已设，功能不受影响） */
}
/* 别名命中：refs++；调用方继承「这张图已上送吗 / 指纹」，上传门控据此跳过整图重传。 */
static int m40ae_alias(int tid,int ei)
{
 if(tid<0||tid>=TEXREG_N||ei<0||ei>=M40AE_EN)return 0;
 m40ae_img *e=&g_m40ae_img[ei];
 if(!e->vk||!e->refs)return 0;
 e->refs++;g_m40ae_eid[tid]=ei;g_m40ae_hit++;
 if(g_m40ae_hitlines<M40AE_HIT_LINE_MAX){
  g_m40ae_hitlines++;
  vk_boot_logf("[m40ae] HIT key=%#llx/%ux%u/%#x id=%d refs=%u up=%u",
   (unsigned long long)e->mem,e->w,e->h,(unsigned)e->fmt,tid,e->refs,(unsigned)e->up);
 }
 return 1;
}
/* 释放一个 id 对图的引用。返回 1 = refs 归零（*ovk/*obytes 给出待销毁对象），0 = 仍有人引用，-1 = 未登记。 */
static int m40ae_unref(int tid,nvn_vk_tex **ovk,unsigned long long *obytes)
{
 if(tid<0||tid>=TEXREG_N)return -1;
 int ei=g_m40ae_eid[tid];
 if(ei<0||ei>=M40AE_EN)return -1;
 m40ae_img *e=&g_m40ae_img[ei];
 g_m40ae_eid[tid]=-1;
 if(e->refs)e->refs--;
 if(!e->refs){
  if(ovk)*ovk=e->vk; if(obytes)*obytes=e->bytes;
  e->vk=NULL;e->up=0;e->fp=0;e->mem=0;e->bytes=0;
  if(g_m40ae_nuniq)g_m40ae_nuniq--;
  return 1;
 }
 return 0;
}
/* 上传门控口径：-1 = 该 id 不是共享别名（用原 g_idup）；0/1 = 所属图的 up。 */
static int m40ae_up(int tid)
{
 if(!m40ae_on()||tid<0||tid>=TEXREG_N)return -1;
 int ei=g_m40ae_eid[tid];
 if(ei<0||ei>=M40AE_EN)return -1;
 m40ae_img *e=&g_m40ae_img[ei];
 return e->vk?((int)e->up):-1;
}
/* 上送成功回调：把「这张图已上送 / 指纹 / 字节」记到图上（别名共享同一份）。 */
static void m40ae_setup(int tid,uint64_t fp,unsigned long long bytes)
{
 if(tid<0||tid>=TEXREG_N)return;
 int ei=g_m40ae_eid[tid];
 if(ei<0||ei>=M40AE_EN)return;
 m40ae_img *e=&g_m40ae_img[ei];
 if(!e->vk)return;
 if(!e->up){e->bytes=bytes;}
 e->up=1;e->fp=fp;
}
/* M5z P0：被「我们自己的资源上限」饿死的纹理，按**去重 id** 计数（同一 id 每帧重绑只算一个）。
   它决定修法：如果 cap_ids 只有几十个 → 抬上限就够；如果是几千个 → 必须做 LRU 驱逐（内存不变）。 */
static unsigned char g_t_starve_cap_bit[TEXREG_N/8], g_t_starve_heat_bit[TEXREG_N/8];
static unsigned long long g_t_starve_cap_ids, g_t_starve_heat_ids;
static unsigned long long g_t_starve_cap_bytes, g_t_starve_heat_bytes;
static void t_starve_note(int tid, unsigned long long bytes, int cap)
{
 if(tid<0||tid>=TEXREG_N)return;
 unsigned char *b = cap ? g_t_starve_cap_bit : g_t_starve_heat_bit;
 unsigned m = 1u << (tid & 7);
 unsigned long long *ids = cap ? &g_t_starve_cap_ids : &g_t_starve_heat_ids;
 unsigned long long *by  = cap ? &g_t_starve_cap_bytes : &g_t_starve_heat_bytes;
 if(!(*b & m)){*b |= (unsigned char)m;(*ids)++;*by += bytes;}
}
/* ================= M5z P1：id 图上限从「一次性名额」改成「LRU 缓存」 =================
   事实链（M5y b2 同一 run，提交点对齐）：
     @#1200 idimg=346 cap=0      empty_white=10.2%
     @#1500 idimg=384 cap=122988 empty_white=57.0%
     @#1800 idimg=384 cap=300591 empty_white=73.7%
   `k_id_img_max=384` 是一次性名额：第 385 个 id 起永远拿不到 VkImage，槽被绑成 NULL ⇒
   描述符回落到 1x1 白 ⇒ 白无贴图模型。空槽率跟着 cap 次数一起涨，这就是真因（不是 guest
   没给纹理：被点名的空槽 17/18 是 why=-30，且上送过的纹理 nz=89%）。
   修法（单点）：名额变成**缓存**。上限不变（峰值内存不涨），被挡住时按 LRU 驱逐一张
   「此刻没有任何采样槽引用」的旧图，让新 id 进来。三条安全约束：
     ① 只驱逐 view 不在任何 g_r_tx_view[slot] 里的图（绝不产生悬垂描述符）；
     ② 每 submit 最多 M5Z_EVICT_PER_SUB 次（nvn_vk_tex_destroy 里有 vkQueueWaitIdle，必须限流）；
     ③ 只给热 id 让位（binds>=M5Z_EVICT_MIN_BINDS）：一次性纹理不值得换出。
   字节账拆成累计（g_t_bytes，报表口径不变）与活体（g_t_bytes_live，热度门槛用）：
   驱逐释放活体字节，否则 860 MB 的累计账会永远把门槛压在「只放行 binds>=32」。 */
#define M5Z_EVICT_PER_SUB 8
#define M5Z_EVICT_MIN_BINDS 8
/* M7k P1（2026-11-07）：小图让位直通。开关 M7K_SMALL_EVICT_ON 定义在 nvn_vk/nvn_vk.h（共享头，
   nvn_vk.c 的回读行同源）。M7j boot 实测场景族贴图（id12074/12081 f5 256x256 等）
   t≈200s 才首绑，此时 idimg=384/384 满，让位门槛 binds>=8 对首绑(bind=1)恒拒 ⇒ why=-31 恒 PH
   （miss-class cap_idimg=558908 占空槽 97.5%，budget=0 upfail=0 nosnap=0 unres=0）。
   小图（≤1MB，与 M4bk P2 热度豁免同阈值）建图+上送成本微不足道，不应被 binds 门槛挡在门外；
   驱逐对象仍受约束①（无槽引用）/②（每 submit ≤8 次）保护。 */
/* M103A（2026-11-13）：`g_t_idbinds` 提到这里声明 —— 让位优先序（小图 → 低 binds → 最老）
   要用它，而它的原声明在下方 M40Q 段（:2890 一带）。两处都留会变重复暂定定义，故只保留这一处。 */
static unsigned g_t_idbinds[TEXREG_N];    /* 每 id 被绑次数（M40Q 起用于「一次性 id」判据） */
static uint32_t g_id_lru[TEXREG_N];       /* 每 id 最近一次被绑的序号（LRU 判据） */
static uint32_t g_id_lru_seq;
static unsigned long long g_t_bytes_live; /* 活体纹理字节（驱逐会减；g_t_bytes 仍是累计口径） */
static unsigned long long g_z_evict, g_z_evict_bytes, g_z_evict_noslot, g_z_evict_deny;
static unsigned long long g_z_evict_small;   /* M7k：走小图直通换来的让位次数 */
static unsigned long long g_z_evict_deny_s;  /* M7k：小图独立配额用尽次数 */
static unsigned long g_z_ev_sub; static unsigned g_z_ev_in_sub;
static unsigned g_z_ev_in_sub_s;   /* M7k P2：小图独立配额的每 submit 计数 */
/* M103A P2：每 submit 让位配额。关态 = M5Z 历史值 8；开态 = 64（派活单要求 ≥64）。
   只改上界，不改「谁可以被换出」（那是 t_evict_one / t_evict_one_shared 的候选约束）。 */
static unsigned t_evict_quota(void){ return m103a_on()?M103A_EVICT_PER_SUB:M5Z_EVICT_PER_SUB; }
static unsigned t_evict_slot(void)
{
 unsigned long sub=(unsigned long)atomic_load_explicit(&g_beat_nvn_submit,memory_order_relaxed);
 if(sub!=g_z_ev_sub){g_z_ev_sub=sub;g_z_ev_in_sub=0;g_z_ev_in_sub_s=0;}
 if(g_z_ev_in_sub>=t_evict_quota()){g_z_evict_deny++;return 0;}
 g_z_ev_in_sub++;
 return 1;
}
/* M7k P2（boot2）：小图让位用独立的每 submit 配额。boot1 实测场景首绘窗口 deny=2575——
   8 次/submit 的共享配额被重入洪水（此前被逐出的热 id 回来）吃光，场景小图在同一窗口
   输掉竞速（td 采样 PH，而邻居 id12087/12090 在前 30ms 进来）。小图 ≤1MB，单独 8 次/submit
   只把最坏情况从 8 张大图变成 8 大 + 8 小，仍是硬上界。随 M7K_SMALL_EVICT_ON 一起回退。 */
static unsigned t_evict_slot_small(void)
{
 unsigned long sub=(unsigned long)atomic_load_explicit(&g_beat_nvn_submit,memory_order_relaxed);
 if(sub!=g_z_ev_sub){g_z_ev_sub=sub;g_z_ev_in_sub=0;g_z_ev_in_sub_s=0;}
 if(g_z_ev_in_sub_s>=t_evict_quota()){g_z_evict_deny_s++;return 0;}
 g_z_ev_in_sub_s++;
 return 1;
}
/* M40AE ②：共享图的淘汰必须**按图**做 —— 只清一个别名 id 腾不出名额（同一张图还被别人引用），
   所以先按「图」聚合：该图的所有别名 id 都不在槽里引用才允许整张销毁，销毁后清零全部别名 id。
   O(TEXREG_N + M40AE_EN)，不随别名数膨胀。返回 1 = 真的换掉了一张图。 */
static int t_evict_one_shared(int want_id)
{
 static unsigned char s_used[M40AE_EN],s_have[M40AE_EN]; static uint32_t s_lru[M40AE_EN];
 static unsigned long long s_binds[M40AE_EN];   /* M103A：图级 binds = 该图所有别名 id 的最大 binds */
 int best=-1;uint32_t bestlru=0;int best_small=1;unsigned long long best_binds=~0ull;
 memset(s_used,0,sizeof s_used);memset(s_have,0,sizeof s_have);
 memset(s_binds,0,sizeof s_binds);
 for(int i=0;i<M40AE_EN;i++)s_lru[i]=~0u;
 for(int id=0;id<TEXREG_N;id++){
  int ei=g_m40ae_eid[id];
  if(ei<0||ei>=M40AE_EN||!g_idvk[id])continue;
  s_have[ei]=1;
  if(id==want_id||nvn_vk_tex_id_in_use(id))s_used[ei]=1;   /* 约束①：任一别名还在槽里引用 ⇒ 整张不动 */
  if(g_id_lru[id]<s_lru[ei])s_lru[ei]=g_id_lru[id];
  if(g_t_idbinds[id]>s_binds[ei])s_binds[ei]=g_t_idbinds[id];
 }
 for(int i=0;i<M40AE_EN;i++){
  m40ae_img *e=&g_m40ae_img[i];
  if(!e->vk||!e->refs||!s_have[i]||s_used[i])continue;
  /* M103A P2：换出对象优先序 = 小图（≤1 MB）→ 低 binds → 最老 LRU。
     小图建图/上送成本最低，先换它们对吞吐最划算；关态退回纯 LRU（逐字 = 旧行为）。 */
  if(m103a_on()){
   int sm=(e->bytes&&e->bytes<=(1ull<<20))?0:1;
   unsigned long long bd=s_binds[i];
   if(best<0||sm<best_small||
      (sm==best_small&&(bd<best_binds||(bd==best_binds&&s_lru[i]<bestlru)))){
    best=i;bestlru=s_lru[i];best_small=sm;best_binds=bd;}
  } else if(best<0||s_lru[i]<bestlru){best=i;bestlru=s_lru[i];}
 }
 if(best<0)return 0;
 {m40ae_img *e=&g_m40ae_img[best];
  unsigned long long by=nvn_vk_tex_vkbytes(e->vk);
  nvn_vk_texture_forget(e->vk);
  nvn_vk_tex_destroy_deferred(e->vk);
  for(int id=0;id<TEXREG_N;id++)if(g_m40ae_eid[id]==best){g_m40ae_eid[id]=-1;g_idvk[id]=NULL;g_idup[id]=0;}
  e->vk=NULL;e->refs=0;e->up=0;e->fp=0;e->mem=0;e->bytes=0;
  if(g_m40ae_nuniq)g_m40ae_nuniq--;
  if(g_t_idimg)g_t_idimg--;                      /* 名额按唯一图数还回去 */
  if(g_t_bytes_live>=by)g_t_bytes_live-=by;else g_t_bytes_live=0;
  g_z_evict++;g_z_evict_bytes+=by;g_m40ae_evimg++;}
 return 1;
}
/* 原逐 id 淘汰：开关关态逐字节原行为；开态只处理**未登记**的老图（切开关前就已建好的），
   已登记的共享图一律由 t_evict_one_shared 整张处理，避免拆散别名留下悬垂引用。 */
static void t_evict_one_legacy(int want_id)
{
 int best_id=-1;uint32_t best=0;int best_small=1;unsigned long long best_binds=~0ull;
 for(int id=0;id<TEXREG_N;id++){
  if(!g_idvk[id]||id==want_id)continue;
  if(nvn_vk_tex_id_in_use(id))continue;          /* 约束①：正被槽引用的图一律不动 */
  if(m40ae_on()&&g_m40ae_eid[id]>=0)continue;    /* M40AE：共享图不在这里逐个拆 */
  if(best_id<0){best=g_id_lru[id];best_id=id;best_small=1;best_binds=~0ull;}
  if(m103a_on()){
   /* M103A P2：小图 → 低 binds → 最老（与共享路径同口径）。 */
   unsigned long long by=nvn_vk_tex_vkbytes(g_idvk[id]);
   int sm=(by&&by<=(1ull<<20))?0:1;
   unsigned long long bd=g_t_idbinds[id];
   if(sm<best_small||
      (sm==best_small&&(bd<best_binds||(bd==best_binds&&g_id_lru[id]<best)))){
    best=g_id_lru[id];best_id=id;best_small=sm;best_binds=bd;}
  } else if(g_id_lru[id]<best){best=g_id_lru[id];best_id=id;}
 }
 if(best_id<0){g_z_evict_noslot++;return;}
 {unsigned long long by=nvn_vk_tex_vkbytes(g_idvk[best_id]);
  nvn_vk_texture_forget(g_idvk[best_id]);         /* 防御性：正常情况下该图的 view 已无槽引用 */
  /* M6a B：不再每张一次 vkQueueWaitIdle（M5z 455 次 / 860 MB 重传的吞吐代价）。
     进坟场，由 nvn_vk_draw_flush 在「本批 CB 已过栅栏」那一刻统一销毁。 */
  nvn_vk_tex_destroy_deferred(g_idvk[best_id]);
  g_idvk[best_id]=NULL;g_idup[best_id]=0;
  if(g_t_idimg)g_t_idimg--;                      /* 名额真的还回去（旧实现只增不减） */
  if(g_t_bytes_live>=by)g_t_bytes_live-=by;else g_t_bytes_live=0;
  g_z_evict++;g_z_evict_bytes+=by;}
}
/* M40AE ②：统一入口。开态先试图整张换掉一张共享图，没有可换的再退回原逐 id 逻辑（老图）。 */
static void t_evict_one(int want_id)
{
 if(m40ae_on()&&t_evict_one_shared(want_id))return;
 t_evict_one_legacy(want_id);
}
/* M5z P1：这张 id 的图是「因为 LRU 让位才拿到的」⇒ 没有这个改动它的槽此刻会是白占位。
   draw 侧用它算**同 run 反事实**的 empty（不用两次 boot 猜）。 */
static unsigned char g_z_fix_bit[TEXREG_N/8];
static void t_fix_note(int tid)
{
 if(tid<0||tid>=TEXREG_N)return;
 g_z_fix_bit[tid>>3] |= (unsigned char)(1u << (tid & 7));
}
static int t_fix_saved(int tid)
{
 if(tid<0||tid>=TEXREG_N)return 0;
 return (g_z_fix_bit[tid>>3] >> (tid & 7)) & 1;
}
/* ================= M30A (2026-11-09)：两个文件开关（缺省 0；关态行为逐字不变）=================
   smo_m30a_q bit0：id-image 名额 384 → 1024。
     依据（M28A 残留①）：`[t] idimg=384/384` + `[t] starve cap_ids=8 cap_bytes=3,129,344`。
     名额是"一次性名额"：LRU 让位只给 binds>=8 的热 id 或 ≤1MB 的小图，首绑就来的新 id
     直接拿不到 VkImage（why=-31）。抬到 1024 后仍有 LRU 与 1 GB 字节预算兜底。
   smo_m30a_b bit0：把未压缩的大图（含 1600×900 全屏 scratch）也纳入 big 独立配额。
     依据（M28A 残留② + 本轮唯一假设）：`[t] starve heat_ids=8 heat_bytes=52,428,800`、
     `[m] sentinel slot=13 texid=3237 why=-30(create_heat) ext=1600x900 up=0` ——
     被拒传的全屏 scratch ⇒ 采到 1×1 白占位 ⇒ 整屏平涂。
   读法：两文件各 2 s 重读（构造期建 0666 松手文件、内容 0）⇒ 写 0 即秒级回退，不用重装。 */
#define M30A_SW_Q_A "/data/storage/el2/base/files/smo_m30a_q"
#define M30A_SW_Q_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m30a_q"
#define M30A_SW_B_A "/data/storage/el2/base/files/smo_m30a_b"
#define M30A_SW_B_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m30a_b"
/* which: 0=smo_m30a_q 1=smo_m30a_b。返回文件内容（缺文件 ⇒ 0）。 */
static int m30a_sw(int which)
{
 static int v[2]={-1,-1}; static long t0[2]={0,0};
 static const char *const pa[2]={M30A_SW_Q_A,M30A_SW_B_A};
 static const char *const pb[2]={M30A_SW_Q_B,M30A_SW_B_B};
 long now=(long)time(NULL);
 if(which<0||which>1)return 0;
 if(v[which]>=0&&now<t0[which]+2)return v[which];
 t0[which]=now;v[which]=0;
 for(int i=0;i<2;i++){FILE *f=fopen(i?pb[which]:pa[which],"r");
  if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;v[which]=atoi(b);}break;}}
 return v[which];
}
__attribute__((constructor)) static void m30a_seed(void)
{
 static const char *const p[4]={M30A_SW_Q_A,M30A_SW_Q_B,M30A_SW_B_A,M30A_SW_B_B};
 for(int i=0;i<4;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}
/* ================= M34A (2026-11-09)：hit_tgt 盲区修复 + 族 id↔RT 槽按存储地址对齐 =================
   盲区（M33A P2-4，已证）：`gpuop_hit_probe` 的目标集是硬编码 `{1900,1910,1911}`，被拒族 id
   （b0 3226…3241 / b1r 3597…3669）一个都不在里面 ⇒ 该计数器对族**结构性恒 0**，与"写命令有没有
   真执行"无关（b0/b1r 都是 0）。所以 hit_tgt 从来不是"没命中"，是"根本没在看"。
   本开关**只加取证、不改任何判决**：
     ① 动态族表：`tex_up_gate_ok` 判 -30(create_heat) 的那一刻，把 (id, store, avail, w, h, fmt)
        登记进 g_m34a_fam[]（族 id 每 boot 都会换号，所以只按**池地址**当身份，id 只作标签）。
        hit_tgt 改成"写命令目标存储区间 ∩ 族表任一 entry"；另外两个口径同时留着：
        hit_old3 = 旧三 id 口径（可比性）、hit_id = "本次 opcode 的真实目标 id" 落在族 id 集里。
     ② 对齐输出：`[m] sentinel`（nvn_vk 侧）加 store=/rt_slot=、`[gpu] exec` 加 rt_slot=/fam=，
        把「族 id」与「RT 槽」按池地址对齐（`[h] rt slot #N key=<pool store>` 已带 join 键）。
   读法：2 s 重读文件；关态时新增行/新增字段**全部不输出** ⇒ 日志与 M33A 逐字同形。 */
#define M34A_SW_A "/data/storage/el2/base/files/smo_m34a"
#define M34A_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m34a"
static int m34a_on(void)
{
 static int cached=-1; static long last;
 long now=(long)time(NULL);
 if(cached>=0&&now==last)return cached;
 last=now;cached=0;
 {const char *paths[2]={M34A_SW_A,M34A_SW_B};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){int v=0;if(fscanf(f,"%d",&v)==1&&v>0)cached=1;fclose(f);break;}}}
 return cached;
}
/* 构造期建松手文件（与 smo_gpu_write/m30a 同款：hdc shell 是 uid 2000，建不了 app 数据目录）。 */
__attribute__((constructor)) static void m34a_seed(void)
{
 const char *paths[2]={M34A_SW_A,M34A_SW_B};
 for(int i=0;i<2;i++){
  if(access(paths[i],F_OK)==0){chmod(paths[i],0666);return;}
  int fd=open(paths[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(paths[i],0666);return;}
 }}
/* ---- M35A（2026-11-09）：smo_m35a（缺省 0，2 s 重读）----
   只做一件事：把「RT 槽 800x450（光栅口径 guest÷SMO_RT_DIV） vs 采样声明 1600x900（guest 真值）」的
   ÷2 账在**零内容回退路**上对齐（判据从精确相等换成仓库既有的 h_rs_match），并打一行 [m35a] 归因。
   关态：那两处判据仍是精确相等，且**不打任何新行** ⇒ 日志与 M34A 逐字同形。 */
#define M35A_SW_A "/data/storage/el2/base/files/smo_m35a"
#define M35A_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m35a"
static int m35a_on(void)
{
 static int cached=-1; static long last;
 long now=(long)time(NULL);
 if(cached>=0&&now==last)return cached;
 last=now;cached=0;
 {const char *paths[2]={M35A_SW_A,M35A_SW_B};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){int v=0;if(fscanf(f,"%d",&v)==1&&v>0)cached=1;fclose(f);break;}}}
 return cached;
}
/* 构造期建松手文件（与 m34a/smo_gpu_write 同款：hdc shell 是 uid 2000，建不了 app 数据目录）。 */
__attribute__((constructor)) static void m35a_seed(void)
{
 const char *paths[2]={M35A_SW_A,M35A_SW_B};
 for(int i=0;i<2;i++){
  if(access(paths[i],F_OK)==0){chmod(paths[i],0666);return;}
  int fd=open(paths[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(paths[i],0666);return;}
 }}
/* ---- M36A（2026-11-09）：smo_m36a / smo_m36a_fix（缺省 0，2 s 重读）----
   smo_m36a     = **只读取证**：在 rt-bind 命中处补打该槽的 writers / last_use / lay / s_layout，
                  并在 rt_sample_prepare 之后打第二读数（lay1 / why）与整幅内容回读（均值/distinct/lag1）。
                  关态：一行都不打，日志与 M35A 逐字同形。
   smo_m36a_fix = 修正开关（缺省 0，只有取证判明「写过但 layout 没 transition」时才允许开）：
                  见 nvn_vk.c rt_xfer_run()：账上 UNDEFINED 的图不再把 oldLayout 谎报成 CAO。 */
#define M36A_SW_A "/data/storage/el2/base/files/smo_m36a"
#define M36A_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m36a"
#define M36A_FIX_SW_A "/data/storage/el2/base/files/smo_m36a_fix"
#define M36A_FIX_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m36a_fix"
static int m36a_on(void)
{
 static int cached=-1; static long last;
 long now=(long)time(NULL);
 if(cached>=0&&now==last)return cached;
 last=now;cached=0;
 {const char *paths[2]={M36A_SW_A,M36A_SW_B};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){int v=0;if(fscanf(f,"%d",&v)==1&&v>0)cached=1;fclose(f);break;}}}
 return cached;
}
static int m36a_fix_on(void)
{
 static int cached=-1; static long last;
 long now=(long)time(NULL);
 if(cached>=0&&now==last)return cached;
 last=now;cached=0;
 {const char *paths[2]={M36A_FIX_SW_A,M36A_FIX_SW_B};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){int v=0;if(fscanf(f,"%d",&v)==1&&v>0)cached=1;fclose(f);break;}}}
 return cached;
}
/* 构造期建松手文件（同 m35a 款：hdc shell 是 uid 2000，建不了 app 数据目录）。 */
__attribute__((constructor)) static void m36a_seed(void)
{
 const char *paths[4]={M36A_SW_A,M36A_SW_B,M36A_FIX_SW_A,M36A_FIX_SW_B};
 for(int i=0;i<4;i++){
  if(access(paths[i],F_OK)==0){chmod(paths[i],0666);continue;}
  int fd=open(paths[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(paths[i],0666);}}
}
/* M36A：每 boot 的取证行预算（关态不占任何输出）。 */
static unsigned g_m36a_miss_lines, g_m36a_ph_lines, g_m36a_rb_lines;
static unsigned char g_m36a_miss_bit[TEXREG_N/8], g_m36a_ph_bit[TEXREG_N/8];
static int m36a_first_bit(unsigned char *bits,int tid)
{
 if(tid<0||tid>=TEXREG_N)return 0;
 unsigned char *b=&bits[tid>>3],m=(unsigned char)(1u<<(tid&7));
 if(*b&m)return 0;
 *b|=m;return 1;
}
/* M36A：逐槽取证行（rt-bind 命中处）。up = 该 id 是否上送过；src = 命中来源。 */
static void m36a_rt_bind_line(int tid,int slot,int texid,const texsnap *sn,const char *src,int cands)
{
 unsigned long long wa=0,lu=0,lwf=0,lbf=0,key=0; unsigned ws=0,w=0,h=0;
 int lay=-1,sl=-1,hi=0,cur=0,shl=-1;
 nvn_vk_rt_slot_m36a(slot,&wa,&ws,&lu,&lwf,&lbf,&lay,&sl,&w,&h,&key,&hi,&cur,&shl);
 vk_boot_logf("[m36a] rt-bind id=%d up=%d rt_slot=#%d texid=%d dims=%ux%u store=%#llx src=%s cands=%d "
              "writers=%llu w_sub=%u last_wr_f=%llu last_bind_f=%llu last_use=%llu lay=%d s_layout=%d "
              "img=%d cur=%d sh_lay=%d slot_key=%#llx",
              tid,(tid>=0&&tid<TEXREG_N)?(int)g_idup[tid]:0,slot,texid,
              sn?sn->d.width:0u,sn?sn->d.height:0u,
              (unsigned long long)(uintptr_t)(sn?sn->mem:NULL),src,cands,
              wa,ws,lwf,lbf,lu,lay,sl,hi,cur,shl,key);
}
/* M36A：同一块存储「后来」确实被当过 RT 的占位/零内容纹理（回答「这块面到底该由谁写」）。 */
static void m36a_ph_line(const char *tag,int tid,const texsnap *sn,int cands)
{
 if(!sn||!sn->mem)return;
 unsigned long long store=(unsigned long long)(uintptr_t)sn->mem;
 int slot=-1,txid=-1;
 if(!nvn_vk_rt_store_sibling(store,&slot,&txid))return;
 if(!m36a_first_bit(g_m36a_ph_bit,tid))return;
 if(g_m36a_ph_lines>=48)return;
 g_m36a_ph_lines++;
 unsigned long long wa=0,lu=0,lwf=0,lbf=0,key=0; unsigned ws=0,w=0,h=0;
 int lay=-1,sl=-1,hi=0,cur=0,shl=-1;
 nvn_vk_rt_slot_m36a(slot,&wa,&ws,&lu,&lwf,&lbf,&lay,&sl,&w,&h,&key,&hi,&cur,&shl);
 vk_boot_logf("[m36a] %s id=%d up=%d dims=%ux%u fmt=0x%x store=%#llx cands=%d sib_slot=#%d sib_texid=%d "
              "sib_dims=%ux%u sib_writers=%llu sib_w_sub=%u sib_last_wr_f=%llu sib_last_bind_f=%llu "
              "sib_lay=%d s_layout=%d sib_img=%d sib_cur=%d",
              tag,tid,(tid>=0&&tid<TEXREG_N)?(int)g_idup[tid]:0,sn->d.width,sn->d.height,
              (int)sn->d.format,store,cands,slot,txid,w,h,wa,ws,lwf,lbf,lay,sl,hi,cur);
}
#define M34A_FAM_N 128
typedef struct{int id,fmt;unsigned w,h;uint64_t store,avail;unsigned char hit;} m34a_fam_t;
static m34a_fam_t g_m34a_fam[M34A_FAM_N];
static unsigned g_m34a_fam_n;
static unsigned long long g_m34a_fam_dup,g_m34a_fam_ovf;
/* 池地址 → RT 槽（`[h] rt slot #N key=<store>` 的 join；-1 = 没登记/尺寸或格式不一致）。 */
static int m34a_store_slot(uint64_t store,unsigned w,unsigned h,int fmt,int *txid)
{
 int slot=-1; if(txid)*txid=-1;
 if(!store)return -1;
 nvn_vk_rt_lookup_store(store,w,h,fmt,&slot,txid);
 return slot;
}
/* 按池地址去重登记（族 id 每 boot 换号，存储地址才是身份）。 */
static void m34a_fam_add(int id,const texsnap *sn)
{
 if(!sn||!sn->mem||!sn->avail)return;
 uint64_t store=(uint64_t)(uintptr_t)sn->mem;
 for(unsigned i=0;i<g_m34a_fam_n;i++)
  if(g_m34a_fam[i].store==store){g_m34a_fam_dup++;return;}
 if(g_m34a_fam_n>=(unsigned)M34A_FAM_N){g_m34a_fam_ovf++;return;}
 {int txid=-1;int slot=m34a_store_slot(store,sn->d.width,sn->d.height,(int)sn->d.format,&txid);
  m34a_fam_t *f=&g_m34a_fam[g_m34a_fam_n++];
  f->id=id;f->fmt=(int)sn->d.format;f->w=sn->d.width;f->h=sn->d.height;
  f->store=store;f->avail=sn->avail;f->hit=0;
  vk_boot_logf("[m34a] fam #%u id=%d store=%#llx avail=%llu %ux%u fmt=0x%x why=-30 rt_slot=%d rt_texid=%d",
   g_m34a_fam_n-1,id,(unsigned long long)store,(unsigned long long)sn->avail,
   sn->d.width,sn->d.height,(unsigned)sn->d.format,slot,txid);}
}
static int m34a_fam_id_by_store(uint64_t store)
{
 for(unsigned i=0;i<g_m34a_fam_n;i++)if(g_m34a_fam[i].store==store)return g_m34a_fam[i].id;
 return -1;
}
/* M30A：有效名额（关态恒 384 = M4t P2 的历史值；q bit0 时 1024）。仍是"缓存上限"，不是无界。 */
static unsigned k_id_img_max = 384;   /* M4t P2：256 → 384（m4s_b2 的 `up=256` 正好卡在这个上限上，
                                               第 257 个 id 起全被打回 1×1 白占位：`[t] skip … (no VkImage, id img cap)`。
                                               上送字节预算仍是 1 GB，涨的是「能建图的 id 数」，不是无上限吃内存）。
                                         M30A：384 → 变量，由 smo_m30a_q 在 m30a_q_tick() 里抬到 1024。 */
/* M40O（2026-11-10）：id 图**缓存容量**开关。bit0=1 ⇒ 容量 M40O_CAP（4×384），关态逐字走原 M30A 口径。
   实现故意折进本函数（唯一调用点 g_rp_bind_tex 顶部）：否则两个 tick 会每笔 bind 互相改回，日志爆炸。 */
#define M40O_CAP 1536u
static int m40o_on(void);       /* 定义见下方 M40O 段（在读侧 2 s 重读 smo_m40o） */
static void m40o_stat(void);    /* 每 60 s 一行 [m40o] stat（仅开态） */
static void m30a_q_tick(void)
{
 unsigned want=(m30a_sw(0)&1)?1024u:384u;
 {unsigned ov=m40ag_cap();if(ov)want=ov;}   /* M40AG：smo_m40ag_cap 覆盖（0=不改） */
 int m40o=m40o_on();
 if(m40o&1)want=M40O_CAP;
 if(m103a_on())want=M103A_CAP;   /* M103A P1：4096（最高优先级；关态才看上面那条历史链） */
 if(want==k_id_img_max){m40o_stat();return;}
 if(m103a_on())vk_boot_logf("[m103a] cap %u -> %u (id 图名额；按需建图 + LRU 回收 + 活体字节顶 %lluMB)",
                            k_id_img_max,want,(unsigned long long)(M103A_BYTES_MAX>>20));
 else if(m40o&1)vk_boot_logf("[m40o] on cap=%u lru=%llu resident=%llu deny=%llu (was %u)",
                        want,(unsigned long long)g_z_evict,(unsigned long long)g_t_idimg,
                        (unsigned long long)g_z_evict_deny,k_id_img_max);
 else if(k_id_img_max==M40O_CAP)vk_boot_logf("[m40o] off cap=%u lru=%llu resident=%llu deny=%llu (restore smo_m30a_q=%d)",
                        want,(unsigned long long)g_z_evict,(unsigned long long)g_t_idimg,
                        (unsigned long long)g_z_evict_deny,m30a_sw(0));
 else vk_boot_logf("[m30a] id_img_max %u -> %u (smo_m30a_q=%d)",k_id_img_max,want,m30a_sw(0));
 k_id_img_max=want;
 m40o_stat();
}
/* ================= M103A P1/P2：压顶判据 + 回收 tick（唯一调用点都在 g_rp_bind_tex） ==========
   `need` = 本次要新建这张图的**预估字节**。口径与 g_t_bytes_live 一致（上送成功 +=sn->avail，
   让位换出 -=nvn_vk_tex_vkbytes），所以 `live + need` 与真实占用同量级，可当硬顶判据用。 */
static unsigned long long g_m103a_deny_bytes;   /* 活体字节硬顶挡下的建图请求数（why=-36） */
static unsigned long long g_m103a_reclaim;      /* 回收 tick 换出的张数 */
static unsigned long long g_m103a_evict;        /* 建图判决处的让位张数（开态口径） */
static int t_idimg_bytes_ok(unsigned long long need)
{
 if(!m103a_on())return 1;                        /* 关态：一字不判（原行为逐字不变） */
 if(need>M103A_BYTES_MAX)return 0;               /* 单张自己就超顶 ⇒ 不建（上送门另有 32 MB 兜底） */
 return (g_t_bytes_live+need<=M103A_BYTES_MAX)?1:0;
}
static int m103a_pressure(unsigned long long need)
{
 if(nvn_vk_is_up()&&g_t_idimg>=k_id_img_max)return 1;   /* 名额满（开/关态同义） */
 if(!m103a_on())return 0;
 if(g_t_bytes_live+need>M103A_BYTES_MAX)return 1;       /* 活体字节触顶 */
 return 0;
}
/* 回收 tick：每笔 bind 顶部跑一次。只在真越顶时动，最多 M103A_RECLAIM_PER_SUB 张/submit。
   候选约束与建图判决处同源（t_evict_one：只换无槽引用的图；小图/低 binds/最老优先）。 */
static void m103a_reclaim(void)
{
 if(!m103a_on())return;
 unsigned n=0;
 while(m103a_pressure(0)&&n<M103A_RECLAIM_PER_SUB){
  unsigned long long was=g_z_evict;
  if(!t_evict_slot_small())break;
  t_evict_one(-1);
  if(g_z_evict==was)break;     /* 无候选（全被采样槽引用）⇒ 停止；下一笔再试 */
  g_m103a_reclaim++;n++;
 }
 if(n){static unsigned long long said;if(said<24){said++;
   vk_boot_logf("[m103a] reclaim n=%u idimg=%llu/%u live=%lluMB/%lluMB evict=%llu evict_tot=%llu",
    n,(unsigned long long)g_t_idimg,(unsigned)k_id_img_max,
    (unsigned long long)(g_t_bytes_live>>20),(unsigned long long)(M103A_BYTES_MAX>>20),
    (unsigned long long)g_m103a_reclaim,(unsigned long long)g_z_evict);}}
}

/* M4u P1：注册期对象直读兜底。
   缺口（m4t_b2 实测）：`[t] reg ... snap=0` 在 4096-th 之后全是同一个 tex/view 指针
   （tex=0x6ec86f3c78 view=0x7f6e7af050），id 一路涨到 22528，23596 次注册里只有 2949 条进了表，
   其余 20392 条被当「未过宿主 Initialize」丢掉 ⇒ 这些 id 之后被绑定时整片进 unres（40,984）。
   同一指针反复注册说明它不是「没 Initialize 过」，而是 Initialize 快照已被 16 K 环形表挤掉。
   这里在**注册那一刻**（游戏刚写好的同步点）只读一次 magic 校验通过的四个字段建快照，
   不保留对象指针，因此不违反「回放不 deref 游戏对象」的纪律（见上 :555 的崩溃溯源）。
   addr/handle 两条路为什么不选：addr=GetTextureAddress 也是从对象 read pool/off，与这里同源但
   多一层间接；handle 位段只能反解出 id，而 id→快照正是这里缺的东西，解出来也查不到。 */
static const texsnap *texreg_snap_from_obj(const void *key){
 const tex_h *th=key;
 if(!th)return NULL;
 if(th->magic!=0x54455800)return NULL;                     /* 未过我们 Initialize 的对象：不读余下字段 */
 nvn_texdesc d=th->d;
 if(!d.width||!d.height||d.width>16384||d.height>16384)return NULL;   /* 离谱尺寸=误判，宁缺勿错 */
 if(d.format>0xFF)return NULL;
 void *mem=th->mem;                                        /* Initialize 写下的 CPU 回显地址 */
 texsnap *r=texrec_get(key,1);
 r->d=d;r->mem=mem;
 nvn_texlayout L;nvn_tex_layout(&d,&L);uint64_t avail=L.size;
 if(mem){const pool_h *p=th->pool;
  if(p&&p->mem&&p->size){uint64_t off=(uint64_t)th->off;if(off<p->size&&p->size-off<avail)avail=p->size-off;}}
 r->avail=avail;
 return r;
}
static void texreg_put(const void *pool,int id,const void *key,nvn_vk_tex *vk){
 (void)vk;
 if(id<0||id>=TEXREG_N){static unsigned said;if(said++<3)fprintf(stderr,"[nvn] TexturePoolRegister id=%d out of table\n",id);return;}
 const texsnap *s=texrec_get(key,0);
 int src=s?0:1;
 if(!s){s=texreg_snap_from_obj(key);}   /* M4u P1：Initialize 快照不在 → 注册期直读对象 */
 if(!s){g_texreg_collisions++;reg_drop_mark(id);g_t_unres_fix_bad++;   /* 两路都拿不到：不可信，不登记（回放会 miss 并计数） */
  {static unsigned said;if(said++<3)fprintf(stderr,"[nvn] TexturePoolRegister id=%d tex=%p not initialized by host\n",id,key);}
  return;}
 if(src)g_t_unres_fix_reg++;else g_t_unres_fix_init++;
 {texsnap *p=texrec_get(key,1);if(p)p->pin=1;}   /* M4u P1：认领该指针→快照，环形覆盖不再挤掉它 */
 if(g_texid2[id].key&&g_texid2[id].key!=key)g_texreg_collisions++;
 /* 同 id 换了纹理（描述符不同）⇒ 旧的自建图失效，先清槽引用再销毁 */
 if(g_idvk[id]&&!nvn_vk_tex_matches_desc(g_idvk[id],&s->d)){
  /* M40AE ⑤：共享图只降引用；refs 不为 0 说明还有别的别名 id 在用这张图，绝不能销毁。 */
  nvn_vk_tex *ovk=NULL;unsigned long long oby=0;
  int ae_last=m40ae_on()?m40ae_unref(id,&ovk,&oby):-1;
  if(ae_last>=0){
   if(ae_last==1&&ovk){
    nvn_vk_texture_forget(ovk);nvn_vk_tex_destroy(ovk);
    if(g_t_idimg)g_t_idimg--;
    if(oby){if(g_t_bytes_live>=oby)g_t_bytes_live-=oby;else g_t_bytes_live=0;}
   }
   g_idvk[id]=NULL;g_idup[id]=0;
  } else {
   /* M40AF ②：非共享/未登记图销毁同样要还名额与字节（开态才记账，关态零副作用）。 */
   nvn_vk_tex *lvk=g_idvk[id];
   unsigned long long lby=lvk?nvn_vk_tex_vkbytes(lvk):0;
   if(lvk){nvn_vk_texture_forget(lvk);nvn_vk_tex_destroy(lvk);}
   if(m40ae_on()){
    if(g_t_idimg)g_t_idimg--;
    if(lby){if(g_t_bytes_live>=lby)g_t_bytes_live-=lby;else g_t_bytes_live=0;}
   }
   g_idvk[id]=NULL;g_idup[id]=0;
  }
 }
 g_texid2[id]=*s;   /* 快照拷贝：id 槽的内容从此与游戏对象内存解耦 */
 /* M4aj P1：目标 id 注册即导出该对象的**完整初始化历史**（逐 off 扫 nz），判基址是否过期。
    观察名单：1900/1910/1911（M4ai 已证全零的三张）+ 前 2 张 3D 体纹理（M4ai 里 off 4 KB 递增那批）。
    每个 id 最多导 2 次（首注册 + 一次重注册），避免日志膨胀。 */
 {static unsigned char dumps[TEXREG_N];
  int want=(id==1900||id==1910||id==1911);
  if(!want&&s->d.target==2){static unsigned vol;if(vol<2){vol++;want=1;}}
  if(want&&dumps[id]<2){dumps[id]++;ih_watch_add(id,key);
   ih_dump(id,dumps[id]==1?"reg":"reg2",key,(uint64_t)(uintptr_t)s->mem,s->avail,&s->d);}}
 texkey_put(key,id);   /* M4v P3：key→id 哈希（把 O(TEXREG_N) 反查降成 O(1)） */
 if(s->d.format==0x3d&&s->d.width&&s->d.height){   /* M4v P3：RT 尺寸探针增量维护 */
  uint64_t area=(uint64_t)s->d.width*s->d.height;
  if(!g_rt_best||area>=g_rt_best_area){g_rt_best=&g_texid2[id];g_rt_best_area=area;}}
 if(g_texreg_n<TEXREG_N){g_texreg[g_texreg_n].pool=pool;g_texreg[g_texreg_n].id=id;g_texreg[g_texreg_n].key=key;g_texreg_n++;}
}
static void *nvn_texture_get_texture_address(const void *t){
 const tex_h *o=t;const pool_h *p=o->pool;void *r=p&&p->mem?(char *)p->mem+(ptrdiff_t)o->off:0;
 { static const void *seen[16];static unsigned nseen;int hit=0;   /* round20-C：按纹理指针一次性。返 0 且 pool=NULL ⇒ 坐实『调了但纹理未过宿主 Initialize』；零行 ⇒ obj[8] 另有来源，靠 round20-A 桩名定位 */
   for(unsigned i=0;i<nseen;i++)if(seen[i]==t)hit=1;
   if(!hit&&nseen<16){seen[nseen++]=t;fprintf(stderr,"[nvn] GetTextureAddress tex=%p pool=%p off=%#llx -> %p (pool mem=%p)\n",t,(void *)p,(unsigned long long)o->off,r,p?(void *)p->mem:0);} }
 return r;   /* GPU 地址=CPU 地址（同 :145 BufferGetAddress）；若游戏要独立 GPU VA 空间（回声仍负），下轮上 1MB 对齐单调假 VA 分配器 */
}
/* round20-B：纹理查询全家回声——此前全走返-0 桩（round19.log 唯一化清单），池管理器（main+0x97fac0）拿假
   尺寸放置纹理（round19.log:993-995 三纹理 off=0 重叠在 size=0 池）、obj[8]−放置基址错位成 -1MB（崩溃链）。
   d=Initialize 时从 builder 存下的真描述符；布局量走 nvn_tex.c（round17 起真编译）实算。donor hle_nvn.c
   无此批函数（grep 空），自建；签名照 nvn_protos.h:186-210。 */
static int nvn_texture_get_width(const void *t){return (int)((const tex_h *)t)->d.width;}
static int nvn_texture_get_height(const void *t){return (int)((const tex_h *)t)->d.height;}
static int nvn_texture_get_depth(const void *t){return (int)((const tex_h *)t)->d.depth;}
static int nvn_texture_get_levels(const void *t){return (int)((const tex_h *)t)->d.levels;}
static int nvn_texture_get_format(const void *t){return ((const tex_h *)t)->d.format;}
static int nvn_texture_get_target(const void *t){return ((const tex_h *)t)->d.target;}
static int nvn_texture_get_samples(const void *t){return ((const tex_h *)t)->d.samples;}
static int nvn_texture_get_flags(const void *t){return (int)((const tex_h *)t)->d.flags;}
static void nvn_texture_get_swizzle(const void *t,int *r,int *g,int *b,int *a){const nvn_texdesc *d=&((const tex_h *)t)->d;if(r)*r=(int)d->swizzle_r;if(g)*g=(int)d->swizzle_g;if(b)*b=(int)d->swizzle_b;if(a)*a=(int)d->swizzle_a;}   /* nvn_protos.h:195 四出参 */
static void *nvn_texture_get_memory_pool(const void *t){return ((const tex_h *)t)->pool;}   /* nvn_protos.h:206 */
static int64_t nvn_texture_get_memory_offset(const void *t){return ((const tex_h *)t)->off;}   /* nvn_protos.h:207 */
static int nvn_texture_get_storage_size(const void *t){nvn_texlayout l;nvn_tex_layout(&((const tex_h *)t)->d,&l);return (int)l.size;}   /* nvn_protos.h:208：int 返回，布局总量 */
static int nvn_texture_get_storage_class(const void *t){NM_SC_N("nvnTextureGetStorageClass");(void)t;return 0;}   /* round14b 对齐 donor hle_nvn.c:2171：(void)t; return 0（"storage class (the host has none, 0 as for the builder)"）——与 Builder 同恒 0；旧式 target==0xA?0:1 出自 d2r_gfx 旧版，与当前 donor 不符 */
static int nvn_texture_get_raw_storage_class(const void *t){(void)t;return 1;}   /* round22：donor :462 NOPROTO（头无原型）——调用点 main+0x174899c 与 1 比较不等则置状态位 0x100，donor 恒返 1；stub 返 0 会误置位 */
static int64_t nvn_texture_get_stride(const void *t){const nvn_texdesc *d=&((const tex_h *)t)->d;return nvn_tex_is_linear(d)?(int64_t)nvn_tex_linear_stride(d):0;}   /* nvn_protos.h:197：pitch 存储才有行距（LINEAR 0x10/LINEAR_RENDER_TARGET 0x40，nvn_tex.h:27-33），块线性无单一 stride 返 0 */
static int64_t nvn_texture_get_view_offset(const void *t,const void *view){   /* nvn_protos.h:186：视图级偏移=级偏移+层×每层字节 */
 const nvn_texdesc *d=&((const tex_h *)t)->d;
 if(!view)return 0;
 { const unsigned char *w=(const unsigned char *)view;unsigned base_level=w[1],min_layer=(unsigned)w[4]|((unsigned)w[5]<<8);   /* donor view_t（nvn_obj.h:41-45）：bitfield@0、base_level@1、nlevels@2、target@3、min_layer@4(u16) */
   nvn_texlayout l;nvn_tex_layout(d,&l);
   return (int64_t)(nvn_tex_level_offset(d,base_level)+(uint64_t)min_layer*l.layer_size); }
}
/* M3y P1：Texture 对象侧补表（donor hle_nvn.c:451-512 同口径；GetSparseTileLayout/ZCull 无硬件=0，
   DebugID 用对象地址做稳定值，Flush/InvalidateTexels 是缓存一致性提示、本宿主 CPU/GPU 同内存序故空操作）。 */
static int nvn_texture_get_depth_stencil_mode(const void *t){NM_SC_N("nvnTextureGetDepthStencilMode");(void)t;return 0;}   /* 采样深度面（DEPTH=0） */
static void nvn_texture_get_sparse_tile_layout(const void *t,void *out){(void)t;if(out)memset(out,0,64);}
static uint64_t nvn_texture_get_zcull_storage_size(const void *t){NM_SC_N("nvnTextureGetZCullStorageSize");(void)t;return 0;}
static uint64_t nvn_texture_get_debug_id(const void *t){return (uint64_t)(uintptr_t)t;}
static void nvn_texture_set_debug_label(void *t,const char *l){NM_SC_N("nvnTextureSetDebugLabel");(void)t;(void)l;}
static void nvn_texture_flush_texels(const void *t,const void *v,const void *r){NM_SC_N("nvnTextureFlushTexels");(void)t;(void)v;(void)r;}   /* donor :511 */
static void nvn_texture_invalidate_texels(const void *t,const void *v,const void *r){NM_SC_N("nvnTextureInvalidateTexels");(void)t;(void)v;(void)r;}   /* donor :512 */
/* round21-C：Sampler/Queue/Window 接库——round20 stub 名单证实游戏已在调这三个 Initialize（返 0=失败，
   Queue 对象是后续 submit 侧一切的前提），各 Builder setter 同步。布局照 donor nvn_obj.h:51-67，语义照
   donor hle_nvn.c:517-546（Sampler）/720-726（Queue）/2358-2381（Window），简化掉 mtl/life 层。
   枚举默认值（nvn.h:890/898/905/925）：min=NEAREST_MIPMAP_LINEAR 0x4、mag=LINEAR 0x1、wrap=REPEAT 0x1、cmp=LESS 0x2。 */
typedef struct{uint8_t minf,magf,ws,wt,wr,cmp_mode,cmp_func,reduction;float lod_min,lod_max,lod_bias,aniso,border[4],lod_snap;} samp_par_h;   /* =samp_par（nvn_obj.h:51，44B 断言 :54） */
typedef struct{void *dev;samp_par_h p;} samp_builder_h;   /* =samp_builder_t（:52） */
typedef struct{uint32_t magic;samp_par_h p;} samp_h;      /* =samp_t 前缀（:53） */
/* M4z P2②：sampler 参数台账（sid → wrap/filter/lod）。M4z 只读取证；M4aa P1 起**接线生效**：
   绑定时把该 sid 的 wrap/filter/lod 送进 nvn_vk（nvn_vk_texture_slot_sampler），由它按组合缓存
   VkSampler 并按槽选。表按 id 直址（SamplerPool id 实测 < 8192）。 */
#define SAMP_REG_N 8192
static struct { unsigned char ok, ws, wt, wr, minf, magf; float lod_min, lod_max; } g_samp_reg[SAMP_REG_N];
static unsigned g_t_samp_logged;
static void nvn_sampler_builder_set_defaults(void *b){void *dev=((samp_builder_h *)b)->dev;memset(b,0,sizeof(samp_builder_h));((samp_builder_h *)b)->dev=dev;samp_par_h *o=&((samp_builder_h *)b)->p;
 o->minf=0x4;o->magf=0x1;o->ws=o->wt=o->wr=0x1;o->cmp_func=0x2;o->lod_min=-1000.f;o->lod_max=1000.f;o->aniso=1.f;}   /* donor :517-523 GL 默认 */
static void nvn_sampler_builder_set_device(void *b,void *d){((samp_builder_h *)b)->dev=d;}
static void nvn_sampler_builder_set_min_mag_filter(void *b,int mi,int ma){((samp_builder_h *)b)->p.minf=(uint8_t)mi;((samp_builder_h *)b)->p.magf=(uint8_t)ma;}
static void nvn_sampler_builder_set_wrap_mode(void *b,int s,int t,int r){samp_par_h *o=&((samp_builder_h *)b)->p;o->ws=(uint8_t)s;o->wt=(uint8_t)t;o->wr=(uint8_t)r;}
static void nvn_sampler_builder_set_lod_clamp(void *b,float lo,float hi){((samp_builder_h *)b)->p.lod_min=lo;((samp_builder_h *)b)->p.lod_max=hi;}
static void nvn_sampler_builder_set_lod_bias(void *b,float v){((samp_builder_h *)b)->p.lod_bias=v;}
static void nvn_sampler_builder_set_compare(void *b,int m,int f){((samp_builder_h *)b)->p.cmp_mode=(uint8_t)m;((samp_builder_h *)b)->p.cmp_func=(uint8_t)f;}
static void nvn_sampler_builder_set_border_color(void *b,const float *c){memcpy(((samp_builder_h *)b)->p.border,c,16);}
static void nvn_sampler_builder_set_max_anisotropy(void *b,float v){((samp_builder_h *)b)->p.aniso=v;}
static void nvn_sampler_builder_set_lod_snap(void *b,float v){((samp_builder_h *)b)->p.lod_snap=v;}   /* 存查询用（donor :532 注：Metal 无对应） */
static void nvn_sampler_builder_set_reduction_filter(void *b,int r){if((unsigned)r>2)fprintf(stderr,"[nvn] invalid sampler reduction %d\n",r);else ((samp_builder_h *)b)->p.reduction=(uint8_t)r;}   /* donor :540 die→boot 改日志 */
static uint64_t nvn_sampler_initialize(void *s,const void *b){((samp_h *)s)->magic=0x534D5000;((samp_h *)s)->p=((const samp_builder_h *)b)->p;return 1;}   /* 'SMP\0'，donor :542-544 拷参数即成 */
static void nvn_sampler_finalize(void *s){((samp_h *)s)->magic=0;}   /* donor :551 */
/* M3y P1：Sampler Builder/对象查询与整型边界色补表（donor :536-548 同口径；samp_par 见 nvn_obj.h:51，44B）。
   这些是纯查询：游戏用它们做「参数没变就不重注册」的判重，返 0 会让它每帧重注册（噪声，不影响画面）。 */
static void nvn_sampler_builder_set_border_colori(void *b,const int *c){samp_par_h *o=&((samp_builder_h *)b)->p;
 for(int i=0;i<4;i++)o->border[i]=(float)c[i];}   /* NVN 整型边界色按归一化存 float（donor 只有 float 存储） */
static void nvn_sampler_builder_set_border_colorui(void *b,const uint32_t *c){samp_par_h *o=&((samp_builder_h *)b)->p;
 for(int i=0;i<4;i++)o->border[i]=(float)c[i]/4294967295.0f;}
static void nvn_sampler_builder_get_border_color(const void *b,float *c){memcpy(c,((const samp_builder_h *)b)->p.border,16);}
static void nvn_sampler_builder_get_border_colori(const void *b,int *c){const samp_par_h *o=&((const samp_builder_h *)b)->p;
 for(int i=0;i<4;i++)c[i]=(int)o->border[i];}
static void nvn_sampler_builder_get_border_colorui(const void *b,uint32_t *c){const samp_par_h *o=&((const samp_builder_h *)b)->p;
 for(int i=0;i<4;i++)c[i]=(uint32_t)(o->border[i]*4294967295.0f);}
static void nvn_sampler_builder_get_min_mag_filter(const void *b,int *mi,int *ma){const samp_par_h *o=&((const samp_builder_h *)b)->p;if(mi)*mi=o->minf;if(ma)*ma=o->magf;}
static void nvn_sampler_builder_get_wrap_mode(const void *b,int *s,int *t,int *r){const samp_par_h *o=&((const samp_builder_h *)b)->p;if(s)*s=o->ws;if(t)*t=o->wt;if(r)*r=o->wr;}
static void nvn_sampler_builder_get_lod_clamp(const void *b,float *lo,float *hi){const samp_par_h *o=&((const samp_builder_h *)b)->p;if(lo)*lo=o->lod_min;if(hi)*hi=o->lod_max;}
static float nvn_sampler_builder_get_lod_bias(const void *b){return ((const samp_builder_h *)b)->p.lod_bias;}
static void nvn_sampler_builder_get_compare(const void *b,int *m,int *f){const samp_par_h *o=&((const samp_builder_h *)b)->p;if(m)*m=o->cmp_mode;if(f)*f=o->cmp_func;}
static float nvn_sampler_builder_get_max_anisotropy(const void *b){return ((const samp_builder_h *)b)->p.aniso;}
static int nvn_sampler_builder_get_reduction_filter(const void *b){return ((const samp_builder_h *)b)->p.reduction;}
static void nvn_sampler_set_debug_label(void *s,const char *l){NM_SC_N("nvnSamplerSetDebugLabel");(void)s;(void)l;}
static uint64_t nvn_sampler_get_debug_id(const void *s){return (uint64_t)(uintptr_t)s;}
static void nvn_sampler_get_min_mag_filter(const void *s,int *mi,int *ma){const samp_par_h *o=&((const samp_h *)s)->p;if(mi)*mi=o->minf;if(ma)*ma=o->magf;}
static void nvn_sampler_get_wrap_mode(const void *s,int *w1,int *w2,int *w3){const samp_par_h *o=&((const samp_h *)s)->p;if(w1)*w1=o->ws;if(w2)*w2=o->wt;if(w3)*w3=o->wr;}
static void nvn_sampler_get_lod_clamp(const void *s,float *lo,float *hi){const samp_par_h *o=&((const samp_h *)s)->p;if(lo)*lo=o->lod_min;if(hi)*hi=o->lod_max;}
static float nvn_sampler_get_lod_bias(const void *s){return ((const samp_h *)s)->p.lod_bias;}
static void nvn_sampler_get_compare(const void *s,int *m,int *f){const samp_par_h *o=&((const samp_h *)s)->p;if(m)*m=o->cmp_mode;if(f)*f=o->cmp_func;}
static void nvn_sampler_get_border_color(const void *s,float *c){memcpy(c,((const samp_h *)s)->p.border,16);}
static void nvn_sampler_get_border_colori(const void *s,int *c){const samp_par_h *o=&((const samp_h *)s)->p;for(int i=0;i<4;i++)c[i]=(int)o->border[i];}
static void nvn_sampler_get_border_colorui(const void *s,uint32_t *c){const samp_par_h *o=&((const samp_h *)s)->p;for(int i=0;i<4;i++)c[i]=(uint32_t)(o->border[i]*4294967295.0f);}
static float nvn_sampler_get_max_anisotropy(const void *s){return ((const samp_h *)s)->p.aniso;}
static int nvn_sampler_get_reduction_filter(const void *s){return ((const samp_h *)s)->p.reduction;}
typedef struct{uint32_t magic;void *dev;uint64_t submits;} queue_h;   /* =queue_t（nvn_obj.h:22），游戏按 NVNqueue 分配 */
static uint64_t nvn_queue_initialize(void *q,const void *b){*(queue_h *)q=(queue_h){0x51554555,((const queue_builder_h *)b)->dev,0};fprintf(stderr,"[nvn] QueueInitialize %p\n",q);return 1;}   /* 'QUEU'，donor :720-723；queue_builder_h 全家 round15 已在表 */
static void nvn_queue_finalize(void *q){((queue_h *)q)->magic=0;}   /* donor :726 */
typedef struct{void *dev,*native;int32_t ntex,interval,active;void *tex[4];} win_builder_h;   /* =win_builder_t（nvn_obj.h:62） */
typedef struct{uint32_t magic;void *native;int32_t ntex,interval,next,active,crop[4];void *tex[4];} win_h;   /* =win_t 前缀至 tex[]（:67，0x50 内，尾部 presents/avail/pace/ticket 不写） */
static void nvn_window_builder_set_defaults(void *b){void *dev=((win_builder_h *)b)->dev;memset(b,0,sizeof(win_builder_h));((win_builder_h *)b)->dev=dev;((win_builder_h *)b)->interval=1;}   /* donor :2360 */
static void nvn_window_builder_set_device(void *b,void *d){((win_builder_h *)b)->dev=d;}
static void nvn_window_builder_set_native_window(void *b,void *w){((win_builder_h *)b)->native=w;}
static void nvn_window_builder_set_textures(void *b,int n,void *const *t){
 if(n<0||n>4){fprintf(stderr,"[nvn] WindowBuilderSetTextures count %d\n",n);if(n<0)n=0;else n=4;}   /* donor :2363 LOG【红】→日志 */
 ((win_builder_h *)b)->ntex=n;for(int i=0;i<n;i++)((win_builder_h *)b)->tex[i]=t[i];
}
static void nvn_window_builder_set_num_active_textures(void *b,int n){if(n<2||n>4)fprintf(stderr,"[nvn] WindowBuilderSetNumActiveTextures %d\n",n);else ((win_builder_h *)b)->active=n;}   /* donor :2367 die→boot 改日志（NOPROTO 项） */
static int nvn_window_builder_get_num_active_textures(const void *b){const win_builder_h *v=b;return v->active?v->active:v->ntex;}   /* donor :2368 */
static void nvn_window_builder_set_present_interval(void *b,int v){((win_builder_h *)b)->interval=v;}   /* donor :2370 */
static uint64_t nvn_window_initialize(void *w,const void *b){
 const win_builder_h *s=b;win_h *o=w;
 memset(o,0,sizeof *o);o->magic=0x57494E44;o->native=s->native;o->ntex=s->ntex;o->interval=s->interval;o->active=s->active?s->active:s->ntex;   /* 'WIND'，donor :2373-2375 */
 if(o->ntex<2||o->ntex>4||o->active<2||o->active>o->ntex)return 0;   /* donor :2376 同款校验失败返 0 */
 memcpy(o->tex,s->tex,sizeof o->tex);   /* donor 的 mtl_xv_window_tex(serial) 无 mtl 层，跳过 */
 fprintf(stderr,"[nvn] WindowInitialize %p swapchain %d interval %d active %d first tex %p\n",w,o->ntex,o->interval,o->active,o->tex[0]);
 return 1;
}
static void nvn_window_set_num_active_textures(void *w,int n){win_h *v=w;if(n<2||n>v->ntex)fprintf(stderr,"[nvn] WindowSetNumActiveTextures %d (ntex %d)\n",n,v->ntex);else v->active=n;}   /* donor :2369 die→日志 */
static int nvn_window_get_num_active_textures(const void *w){const win_h *v=w;return v->active?v->active:v->ntex;}   /* donor :2370 */
/* 终局 F-A（crash_1031 终局三段）：装库状态机 kick/notify 链最终落到 WindowSetCrop/GetCrop（loading
   屏 crop 动画），曾为空桩被静默吞 → 链尾 functor（0x1002 signal 藏其中）永不通电 → boot 挂起。 */
static void nvn_window_set_crop(void *w,int x,int y,int ww,int hh){win_h *v=w;int nv[4]={x,y,ww,hh};
 if(memcmp(v->crop,nv,sizeof nv))fprintf(stderr,"[nvn] WindowCrop %d,%d %dx%d\n",x,y,ww,hh);
 memcpy(v->crop,nv,sizeof nv);}   /* donor :2424 同款：存 crop[4]+变化日志（crop 参与 present，暂只存取） */
static void nvn_window_get_crop(const void *w,int *out){const win_h *v=w;memcpy(out,v->crop,sizeof v->crop);}   /* donor :2426 回声 16B；NVNrectangle={x,y,w,h} */
/* 终局 F-D（crash_1031 终局四段）：nvn 句柄族曾全返 0——游戏把 0 当无效句柄，纹理登记表全 0，
   装库完成计数不涨 → 永不 setState(2) → 0x1003 永不 signal。donor hle_nvn.c:192-198 逐行同款
   纯值编码（组合句柄 = tid(20bit) | sid(12bit)<<20，分离采样器放 20 位以上）。 */
static uint64_t nvn_device_get_texture_handle(const void *d,int tid,int sid){(void)d;
 if(tid<0||tid>=(1<<20)||sid<0||sid>=(1<<12)){fprintf(stderr,"[nvn] texture handle ids out of range %d/%d\n",tid,sid);return 0;}
 return (uint64_t)tid|(uint64_t)sid<<20;}   /* donor :199 die→日志+返0（boot 容错） */
static uint64_t nvn_device_get_texel_fetch_handle(const void *d,int tid){(void)d;
 if(tid<0||tid>=(1<<20)){fprintf(stderr,"[nvn] texel fetch id out of range %d\n",tid);return 0;}
 return (uint64_t)tid;}   /* donor :202 */
static uint64_t nvn_device_get_image_handle(const void *d,int id){(void)d;return (uint64_t)id;}   /* donor :194 */
static uint64_t nvn_device_get_separate_texture_handle(const void *d,int id){(void)d;return (uint64_t)id;}   /* donor :203 */
static uint64_t nvn_device_get_separate_sampler_handle(const void *d,int id){(void)d;return (uint64_t)id<<20;}   /* donor :204 */
static void nvn_window_set_present_interval(void *w,int p){win_h *v=w;if(v->interval!=p)fprintf(stderr,"[nvn] present interval %d -> %d\n",v->interval,p);v->interval=p;}   /* donor :2422 */
static int nvn_window_get_present_interval(const void *w){const win_h *v=w;return v->interval;}
static void *nvn_window_get_native_window(const void *w){const win_h *v=w;return v->native;}   /* donor :2421 同款存取 */
static void nvn_window_finalize(void *w){((win_h *)w)->magic=0;}   /* donor :2379 清 magic */
/* 终局 F2（crash_1031 终局章）：提交/栅栏族"提交即退休"。此前这批名字全落表外返-0 空桩——提交"成功"
   但永不产生完成回执，'常駐[シェーダー]'装库状态机（等 0x1003/0x1002 侧的完成链）与渲染协调吃不到回执
   ⇒ 全系统挂起。donor 语义（third_party/d2r_gfx/.../hle_nvn.c:609-729/2471-2521）：sync_t={magic,fenced,fence}
   16B（FITS(sync_t,NVNsync) 已证安全）；FenceSync 记最近提交的栅栏值；SyncWait 未围栏→TIMEOUT_EXPIRED(2)、
   已围栏→等 GPU 完成后 ALREADY_SIGNALED(0)/CONDITION_SATISFIED(1)（nvn.h:1042-1045）；acquire sync 置
   fenced=3/fence=0（"显示不再读该张"，立即成立）。本宿主无 GPU/回放层：CPU 写进池的数据本就就绪，
   一切提交**即刻退休**——FenceSync 只记代际，SyncWait 恒 ALREADY_SIGNALED，等价"GPU 零延迟完成全部工作"
   （donor TOTK 无 replay 层的同款取舍，hle_nvn.c 注释在案）。红线：不预 signal 0x1002/0x1003——nvn fence
   在 donor 也**不**映射 SystemEvent（等待全走 CPU 侧 SyncWait），退休链由游戏任务分发器自己走通，照抄。 */
/* ============ M5o: [fn] NVN sync/event ledger (read-only accounting) ============
   Hypothesis under test (task section 2): the draw wall is a GPU-completion object of NVN that we
   never mark "done", so the guest waits forever. The two guest-visible families are NVNsync
   (fence: {magic,fenced,fence}, :1212) and NVNevent (a value word living in pool memory, :2962);
   there is no separate fence/semaphore object in the app-visible NVN surface (P0 of the report).
   This block only counts. It never changes a return value, never writes guest memory and never
   removes an existing branch (M5k lesson). Per object it keeps the consumer side (waits/queries)
   next to the producer side (fence-signals / value writes) and the value the consumer observed,
   so "waits grow while signals stay flat" becomes a printed fact instead of a guess.
   Default: always on -- SELinux keeps the app sandbox from creating a switch file, so a
   compile-time default is the only enable path. The report goes to vk_boot.log through
   dm_nvn_fn_dump(), called by the 60 s heartbeat dump (threads.c runtime_m5k_dump). */
#define FN_N 128
#define FN_SYNC   1u   /* NVNsync  (fence/timeline)                     */
#define FN_EVT    2u   /* NVNevent (pool value word)                    */
#define FN_CBSIG  3u   /* event reached via a command-buffer SignalEvent */
#define FN_REPVAL 4u   /* ReportValue destination word                  */
typedef struct {
    uint64_t obj; uint32_t type, live;
    uint64_t inits, finz, waits, wait_ok, wait_to, fence_sig;
    uint64_t queries, sig, val_writes, val_chg, recs;
    uint32_t val_first, val_last, last_ret;
    uint64_t last_tmo, last_fence, lr, first_ns, last_ns;
} fn_rec;
static fn_rec g_fn[FN_N];
static unsigned g_fn_n, g_fn_ovf;
static pthread_mutex_t g_fn_mx = PTHREAD_MUTEX_INITIALIZER;
static void fn_bump(uint64_t *p) { __atomic_fetch_add(p, 1, __ATOMIC_RELAXED); }
static fn_rec *fn_get(uint64_t obj, uint32_t type) {
    fn_rec *r = NULL;
    pthread_mutex_lock(&g_fn_mx);
    for (unsigned i = 0; i < g_fn_n; i++)
        if (g_fn[i].obj == obj && g_fn[i].type == type) { r = &g_fn[i]; break; }
    if (!r) {
        if (g_fn_n < FN_N) {
            r = &g_fn[g_fn_n++]; memset(r, 0, sizeof *r);
            r->obj = obj; r->type = type; r->live = 1; r->first_ns = guest_ns();
        } else g_fn_ovf++;
    }
    pthread_mutex_unlock(&g_fn_mx);
    return r;
}
/* create/finalize accounting; obj may be NULL on a bad guest pointer (counted, not dereferenced) */
static void fn_note_life(uint64_t obj, uint32_t type, int up) {
    if (!obj) return;
    fn_rec *r = fn_get(obj, type);
    if (!r) return;
    fn_bump(up ? &r->inits : &r->finz);
    r->live = up ? 1u : 0u;
    r->last_ns = guest_ns();
}
/* NVNsync consumer side: rc is the value the guest is about to receive (0 ok / 2 timeout) */
static void fn_note_wait(uint64_t obj, int rc, uint64_t tmo, uint64_t fence) {
    fn_rec *r = fn_get(obj, FN_SYNC);
    if (!r) return;
    if (__atomic_load_n(&r->waits, __ATOMIC_RELAXED) == 0) r->lr = stub_caller_lr();
    fn_bump(&r->waits);
    if (rc) fn_bump(&r->wait_to); else fn_bump(&r->wait_ok);
    r->last_ret = (uint32_t)rc; r->last_tmo = tmo; r->last_fence = fence;
    r->last_ns = guest_ns();
}
/* NVNsync producer side: a fence value was handed to the guest by FenceSync */
static void fn_note_fsync(uint64_t obj, uint64_t fence) {
    fn_rec *r = fn_get(obj, FN_SYNC);
    if (!r) return;
    fn_bump(&r->fence_sig);
    r->last_fence = fence; r->last_ns = guest_ns();
}
/* NVNevent consumer side: v = the word the guest just read */
static void fn_note_ev_query(uint64_t obj, uint32_t v) {
    fn_rec *r = fn_get(obj, FN_EVT);
    if (!r) return;
    fn_bump(&r->queries);
    if (r->val_chg == 0) r->val_first = v;
    else if (r->val_last != v) fn_bump(&r->val_chg);
    if (r->val_last != v) r->val_last = v;
    r->last_ns = guest_ns();
}
/* NVNevent producer side: CPU Signal (src=FN_EVT) or command-stream SignalEvent (FN_CBSIG) */
static void fn_note_ev_sig(uint64_t obj, uint32_t v, uint32_t src) {
    fn_rec *r = fn_get(obj, src == FN_CBSIG ? FN_CBSIG : FN_EVT);
    if (!r) return;
    fn_bump(&r->sig);
    fn_bump(&r->val_writes);
    if (r->val_chg == 0) r->val_first = v;
    else if (r->val_last != v) fn_bump(&r->val_chg);
    r->val_last = v; r->last_ns = guest_ns();
}
/* ReportValue destination: same "completion token" shape, counted separately */
static void fn_note_repval(uint64_t dst, uint32_t v) {
    fn_rec *r = fn_get(dst, FN_REPVAL);
    if (!r) return;
    fn_bump(&r->val_writes);
    if (r->val_chg == 0) r->val_first = v;
    else if (r->val_last != v) fn_bump(&r->val_chg);
    r->val_last = v; r->last_ns = guest_ns();
}
/* Command-stream SignalEvent recorded for this object (the write itself may never land) */
static void fn_note_ev_rec(uint64_t obj) {
    fn_rec *r = fn_get(obj, FN_CBSIG);
    if (!r) return;
    fn_bump(&r->recs);
    r->last_ns = guest_ns();
}
/* M5o switch (default 0): when 1, a wait on a sync that no FenceSync ever fenced answers
   "already satisfied" instead of "timeout expired". Rationale is pre-registered in the report:
   on this host every submit retires synchronously, so the work such a wait refers to has
   certainly completed; returning 2 claims time ran out, which is false here. It is NOT a blanket
   fence-signal: only objects that were never fenced AND are actually waited on take this branch,
   and only after the ledger shows that shape. The boot script flips the file mid-run; 1 s TTL
   so the fopen never sits in the wait path. */
#define FN_ADV_A "/data/storage/el2/base/files/smo_fn_adv"
#define FN_ADV_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_fn_adv"
static int g_fn_adv_on = -1, g_fn_adv_prev = 0;
static uint64_t g_fn_adv_next_ns, g_fn_adv_hits;
static int fn_adv_on(void) {
    uint64_t now = guest_ns();
    if (g_fn_adv_on >= 0 && now < g_fn_adv_next_ns) return g_fn_adv_on;
    g_fn_adv_next_ns = now + 1000000000ull;
    const char *p[2] = { FN_ADV_A, FN_ADV_B };
    g_fn_adv_on = 0;
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(p[i], "r");
        if (!f) continue;
        int v = 0;
        if (fscanf(f, "%d", &v) == 1) g_fn_adv_on = (v > 0);
        fclose(f);
        break;
    }
    if (g_fn_adv_on != g_fn_adv_prev) {
        g_fn_adv_prev = g_fn_adv_on;
        vk_boot_logf("[fn] switch smo_fn_adv=%d (1 = unfenced NVNsync wait returns already-satisfied)", g_fn_adv_on);
    }
    return g_fn_adv_on;
}
static uint64_t g_nvn_fence_gen,g_nvn_handle_gen,g_nvn_presents,g_nvn_cmd_total,g_nvn_ctl_total;   /* cmd/ctl_total：P0 队列级累计用量（donor 无 QueueGetTotal* 族；单队列游戏，全局累计，ResetMemoryUsageCounts 清零） */
uint64_t dm_nvn_frame_count(void){return g_nvn_presents;}   /* freeze_watchdog.c 弱符号的强定义：游戏自认渲染帧数=出图计数（donor :2469/2550 同位递增）；本宿主无显示链，QueuePresentTexture 处计数 */
/* M67B（2026-11-12）：present **真的把画面交给了某条显示路径**的帧数（= 上面那条链里 vk_ok==1 的帧）。
   与 g_nvn_presents 的区别是本单要判读的第一个问题：「present 被调了吗」vs「present 有东西上屏了吗」。
   两者都要在 [beat] 每 2 s 可读（vk_boot.c 弱符号取本函数），否则只能翻几十 MB 的 vk_boot.log 猜。
   纯观察量：只在既有 present 尾部自增，不参与任何分支判断。 */
static uint64_t g_nvn_screens;
uint64_t dm_nvn_screen_count(void){return g_nvn_screens;}
/* ================= M68B（2026-11-12）：把「present 没被叫」与「present 被我们吞了」分开 =================
   为什么必须是两个计数器而不是一个：
     · g_nvn_presents（M67B 已打进 [beat]）的 ++ 就在 nvn_queue_present_texture 的第一行 —— 它只回答
       「进到我们这层没有」；
     · g_nvn_screens（M67B 的 onscreen）回答「进了之后有没有画面真交给某条显示路径」。
   但 M63A 的疑问比这两者更靠外一层：**客侧到底调没调我们给的地址**？所以这里再补三个入口量：
     g_nvn_present_enter = 进 nvn_queue_present_texture 的次数（入口第一行 ++；同上口径，独立计数）
     dm_nvn_present_proc = nvnGetProcAddress 里被要过 "nvnQueuePresentTexture" 的次数（客侧有没有来取这枚地址）
     dm_nvn_present_done = 复用 g_nvn_screens（**不新开计数器**：done 与 onscreen 必须同口径，否则两套数会漂移）
   判别表（明早照抄 [beat] 三个数即可）：
     proc=0 && enter=0      ⇒ 客侧连这枚地址都没取 ⇒ 引擎根本没用 NVN present 入口（泵是唯一出路）
     proc>0 && enter=0      ⇒ 取了地址但一次没调（引擎自己走了别的出图路径）⇒ 仍靠泵
     enter>0 && done=0      ⇒ 进了我们这层但没上屏 ⇒ **被我们吞了**（nb 分支/降采样/fail-closed/窗口 blit 失败
                              全发生在 done 之前）⇒ 查 M67B 兜底为何没走到
     enter>0 && done>0      ⇒ 上屏有发生 ⇒ 问题在「上屏内容/尺寸/时序」，不在通道存活性
   全部是纯观察量：只自增、只被日志读，不参与任何分支判断。 */
static uint64_t g_nvn_present_enter, g_nvn_present_proc;
uint64_t dm_nvn_present_enter(void){return g_nvn_present_enter;}
uint64_t dm_nvn_present_proc(void){return g_nvn_present_proc;}
uint64_t dm_nvn_present_done(void){return g_nvn_screens;}   /* done ≡ onscreen（同一计数器，避免两套口径漂移） */
static void m68b_addr_str(uint64_t a, char *out, size_t n)
{
    __attribute__((weak)) void runtime_describe_address(uint64_t, char *, size_t);   /* loader.c */
    if (!a) { snprintf(out, n, "n/a"); return; }
    if (runtime_describe_address) { runtime_describe_address(a, out, n); return; }
    snprintf(out, n, "0x%llx", (unsigned long long)a);
}
/* 入口首次进入时的调用者栈（最多 3 层；本函数不内联，所以 c0 就是 present 入口自己，
   c1 通常是我们自己生成的 hle thunk（runtime_hle_switch），c2 才是客体调用点 —— runtime_describe_address
   对客体地址按装载模块给出 `模块名+0x偏移`，所以三层都能读成人话）。 */
static void __attribute__((noinline)) m68b_present_note(void)
{
    const unsigned long long n = (unsigned long long)g_nvn_present_enter;
    if (n == 1ull) {
        char c[3][96];
        void *ra0 = __builtin_return_address(0), *ra1 = __builtin_return_address(1), *ra2 = __builtin_return_address(2);
        m68b_addr_str((uint64_t)(uintptr_t)ra0, c[0], sizeof c[0]);
        m68b_addr_str((uint64_t)(uintptr_t)ra1, c[1], sizeof c[1]);
        m68b_addr_str((uint64_t)(uintptr_t)ra2, c[2], sizeof c[2]);
        vk_boot_logf("[m68b-present] enter=%llu presents=%llu done=%llu first-caller=c0=%s c1=%s c2=%s",
                     n, (unsigned long long)g_nvn_presents, (unsigned long long)g_nvn_screens, c[0], c[1], c[2]);
    } else if ((n % 300ull) == 0ull) {
        vk_boot_logf("[m68b-present] enter=%llu presents=%llu done=%llu proc=%llu",
                     n, (unsigned long long)g_nvn_presents, (unsigned long long)g_nvn_screens,
                     (unsigned long long)g_nvn_present_proc);
    }
}
typedef struct{uint32_t magic,fenced;uint64_t fence;} sync_h;   /* =donor sync_t（hle_nvn.c:611 注释族），'SYNC'=0x434e5953 */
static uint64_t nvn_sync_initialize(void *s,void *d){(void)d;*(sync_h *)s=(sync_h){0x434e5953,0,0};fn_note_life((uint64_t)(uintptr_t)s,FN_SYNC,1);{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] SyncInitialize %p (retire-immediate host)\n",s);}return 1;}   /* donor :609 盖 magic 返 1；此前空桩返 0=Initialize 失败 */
static void nvn_sync_finalize(void *s){((sync_h *)s)->magic=0;fn_note_life((uint64_t)(uintptr_t)s,FN_SYNC,0);}   /* donor :610 */
static void nvn_queue_fence_sync(void *q,void *s,int cond,int flags){(void)q;sync_h *o=s;o->fenced=1;o->fence=++g_nvn_fence_gen;fn_note_fsync((uint64_t)(uintptr_t)s,o->fence);{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] QueueFenceSync sync=%p fence=%llu cond=%d flags=%d -> retired now\n",s,(unsigned long long)o->fence,cond,flags);}}   /* donor :656；无 GPU 即刻退休，等价真机"该提交点已过" */
static void nvn_command_buffer_fence_sync(void *c,void *s,int cond,int flags){(void)c;sync_h *o=s;o->fenced=1;o->fence=g_nvn_fence_gen+1;fn_note_fsync((uint64_t)(uintptr_t)s,o->fence);{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] CommandBufferFenceSync sync=%p fence=%llu cond=%d flags=%d -> next submit, retired now\n",s,(unsigned long long)o->fence,cond,flags);}}   /* donor :661：代表下一次提交（fence_now+1），宿主侧同样即刻成立 */
static int nvn_sync_wait(const void *s,uint64_t timeout){const sync_h *o=s;int fenced=(o&&o->fenced)?1:0;   /* M5o: read-only snapshot (same test as before) */
 fn_note_wait((uint64_t)(uintptr_t)s,fenced?0:2,timeout,(uint64_t)(o?o->fence:0));
 if(!fenced&&fn_adv_on()){   /* M5o switch: see fn_adv_on() rationale; default OFF */
  __atomic_fetch_add(&g_fn_adv_hits,1,__ATOMIC_RELAXED);
  {static unsigned n;if(n++<8)fprintf(stderr,"[nvn] SyncWait sync=%p UNFENCED -> forced ALREADY_SIGNALED (smo_fn_adv=1)\n",s);}
  return 0;
 }
 if(!fenced)return 2;{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] SyncWait sync=%p fence=%llu timeout=%llu -> ALREADY_SIGNALED (host retires instantly)\n",s,(unsigned long long)o->fence,(unsigned long long)timeout);}return 0;}   /* donor :670：未围栏按真机等到超时(2)，已围栏宿主恒 ALREADY_SIGNALED(0) */
static uint64_t nvn_queue_wait_sync(void *q,const void *s){(void)q;fn_note_wait((uint64_t)(uintptr_t)s,0,0,0);{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] QueueWaitSync -> 1 (waited work already done)\n");}return 1;}   /* donor :654：等待的活已在提交序前跑完 */
static void nvn_queue_flush(void *q){NM_SC_N("nvnQueueFlush");(void)q;{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] QueueFlush %p -> immediate (nothing in flight)\n",q);}}   /* donor :728 空实现；提交即退休 */
static void nvn_queue_finish(void *q){NM_SC_N("nvnQueueFinish");(void)q;{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] QueueFinish %p -> immediate (nothing in flight)\n",q);}}   /* donor :729 排空回放；本宿主无在飞工作 */
/* ---------------- M3h（2026-11-03）：最小回放——只做 CLEAR_COLOR ----------------
   此前本入口只记账。本块把每个 submit 的命令流真的走一遍，只回放 CLEAR_COLOR
   （M2 实测极简帧 15 条：BIND_PROGRAM×8 / SET_RENDER_TARGETS×3 / CLEAR_COLOR / DRAW_ELEMENTS /
   BARRIER / END 各 1），其余 opcode 一律计数跳过——本轮不做几何、不碰 present。
   走流语义与 cmd_trace.c:58 nvn_cmd_trace_walk 同源（nvn_cmd_exec：先验可读性、跟 CMD_JUMP 链），
   CLEAR_COLOR 的负载取自录制侧 cmdbuf.c:509 的 record_copy(...,2,16)（布局见 cmd_trace.c:106 注释）。 */
static uint64_t g_h_clear_ok,g_h_clear_err,g_h_clear_noptr,g_h_exec_fail;
static uint64_t g_h_clear_supp;   /* M4f P1：同帧同目标被抑制的 clear 次数（不落 Vk 提交） */
/* M4f P1：帧锚点 = nvn_window_acquire_texture（本文件 :1090，游戏每帧渲染前必过）。
   选它的理由：①它是「一帧开始」的唯一必经点（present 是帧尾，早退分支也可能不 present）；
   ②它与本题要保护的对象同层——帧内多条 submit 的绘制都发生在两次 acquire 之间。 */
static _Atomic unsigned long long g_frame_anchor;
/* M4h P1：nvn_vk 的读回探针要按「帧」调度（首笔游戏 draw 后 + 每 300 帧），
   这里把帧锚点以普通函数暴露出去（弱符号，x86_64 壳不编本文件时退化为 flush 计数）。 */
unsigned long long dm_frame_anchor_get(void){return atomic_load_explicit(&g_frame_anchor,memory_order_relaxed);}
static uint64_t g_h_draw_err;   /* M3m：nvn_vk_draw_tri 返回负值的次数（draw 侧失败，与 clear 分开） */
static uint64_t g_h_rt_bind_ok,g_h_rt_bind_err,g_h_rt_flush_err;
static uint64_t g_h_rt_probe_miss;   /* M4p P1：RT tex0 侧表未命中次数（不受下面 8 条打印限流影响，进 totals 行） */
/* M4q P1：解析来源直方图 + tex0==0（纯深度 pass）计数——验收要看「哪一级接住了」。
   M4r P2：ind 拆成 ind(层1)/ind2(层2)/handle(句柄解码)，另加负缓存命中数。 */
static uint64_t g_h_rt_by_snap,g_h_rt_by_rec,g_h_rt_by_texh,g_h_rt_by_ind,g_h_rt_by_probe,g_h_rt_n0;
static uint64_t g_h_rt_by_ind2,g_h_rt_by_handle,g_h_rt_neg_hit,g_h_rt_by_ind3;
/* M4w P1：RT 槽 key 的来源口径（mem = 池地址，能和采样侧 store join；ptr = 对象指针，join 不上）。
   P1 的覆盖上限直接由 stk_ptr 决定，必须可量化。 */
static uint64_t g_h_rt_stk_mem,g_h_rt_stk_ptr;
static unsigned g_h_rt_norm_log;   /* [h] rt norm 行限流 */
/* M6b b：层 3（包装→结构→对象）的三种结局：唯一命中 / 多身份放弃 / 没有对象。
   RT_DEEP_ACCEPT=0：只保留实现，默认不把层 3 命中当作绑定依据（未验证，见 h_rt_chain 注释）。 */
#define RT_DEEP_ACCEPT 0
static unsigned long long g_rt_deep_hit, g_rt_deep_amb, g_rt_deep_none;
static unsigned g_h_rt_logged;   /* rt bind 行限流（首 16 条 + 每 20000 条一条） */
/* M4w P2：该 id 是否曾作为 RT（SET_RENDER_TARGETS 解析出 id）出现过。never-up top20 要回答
   「这批全 0 的纹理里，有多少本来就是我们的渲染目标、只是没接上采样侧」。 */
static unsigned char g_rt_id_seen[TEXREG_N];
static CmdExecStat g_h_cum;
/* M6o: read-only accessor for the injected-press probe (input.c): the same cumulative draw counter the
   `[r] draw totals @submit#N` line prints. No counter, no state, no side effect. */
unsigned long long runtime_draws_total(void) { return (unsigned long long)g_h_cum.draws; }
/* M4an b2：sink 级 draw 拒收的原因拆分（此前 g_h_cum.draw_skip 只累加、无出口 = 零证据盲区） */
static unsigned long long g_h_skip_cnt,g_h_skip_idx,g_h_skip_big;
/* M4ao P1：sink 侧 fail 的返回码分档 + CALL 体内归因（字段含义见 cmd_trace.h CmdExecStat 末尾） */
static unsigned long long g_h_fail_skip,g_h_fail_err,g_h_fail_other,g_h_incall,g_h_incall_ok;
static unsigned long long g_h_call_markers,g_h_call_body_missing;
static CmdTrace g_h_ops;
static unsigned g_h_sub;   /* 当前 submit 序号（回调日志用；单队列游戏，无并发提交） */
/* ---- M4q P1：RT 身份解析链（侧表 → 快照表 → magic → 探针） ----
   为什么要有第二级：`vk_tex_key_hit` 只查 `g_vk_tex_key[]`（写者 vk_tex_attach，挂在
   nvnTextureInitialize 且带 `nvn_vk_is_up()` 门槛，:520-529）。而 `g_texrec[]` 是**每次**
   Initialize 都写的宿主快照（:531-546，无门禁，含 descriptor/池基址/可用字节），且对象内存
   被游戏轮转复用时无需重新 attach。m4p_b1 的 `侧表未命中` 全部是同一个 key
   `0x6e478bf948`（`:415/417`），它在整份日志里没有任何一条 `[t] tex init` 行——
   要么从没走过 Initialize，要么走了但当时 vk 未起/表未登记；无论哪种，快照表都能兜住后者。 */
typedef struct{const tex_h *th;const texsnap *rec;const void *key;const void *mem;
               unsigned w,h,fmt,flags;int tgt,id;const char *src;} rt_ident;
static int h_rt_lookup(uint64_t key,rt_ident *o,int allow_peek){
 memset(o,0,sizeof *o);o->key=(const void *)(uintptr_t)key;o->id=-1;o->tgt=-1;o->src="none";
 if(!key)return 0;
 const tex_h *th=vk_tex_key_hit(o->key);
 if(th){o->th=th;o->w=th->d.width;o->h=th->d.height;o->fmt=th->d.format;o->tgt=th->d.target;
        o->flags=th->d.flags;o->mem=th->mem;o->id=g_tex_id_by_key(o->key);o->src="snap";
        if(o->w&&o->h&&o->w<=4096&&o->h<=4096)return 1;
        vk_boot_logf("[h] rt size guard key=%#llx snap=%ux%u -> next",(unsigned long long)key,o->w,o->h);
        o->th=NULL;}
 /* 第二级：宿主快照表（按对象指针）。这里是「对象走过 Initialize 但侧表没登记」的正解。 */
 {const texsnap *r=texrec_get(o->key,0);
  if(r&&r->d.width&&r->d.height&&r->d.width<=4096&&r->d.height<=4096){
   o->rec=r;o->w=r->d.width;o->h=r->d.height;o->fmt=r->d.format;o->tgt=r->d.target;
   o->flags=r->d.flags;o->mem=r->mem;o->id=g_tex_id_by_key(o->key);o->src="rec";return 1;}}
 /* 第三级：内存首字是不是我们写的 tex_h.magic（游戏按值拷贝对象时对象指针会变）。 */
 if(allow_peek){
#ifdef __MUSL__
  uint32_t mg=0;tex_h tv;
  if(rt_peek_u32(key,&mg)&&mg==0x54455800u&&rt_peek_span(key,&tv,sizeof tv)){
   const tex_h *t=(const tex_h *)(uintptr_t)key;
   if(t->d.width&&t->d.height&&t->d.width<=4096&&t->d.height<=4096){
    o->th=t;o->w=t->d.width;o->h=t->d.height;o->fmt=t->d.format;o->tgt=t->d.target;
    o->flags=t->d.flags;o->mem=t->mem;o->id=g_tex_id_by_key(o->key);o->src="tex_h";return 1;}}
#endif
 }
 return 0;
}
/* M4q P1：解析不到时的一次性取证（前 8 次）——回答「这个 key 到底是什么内存」：
   ①能不能读；②头 8 个字长什么样；③是不是落在某张纹理的池数据区里（mem..mem+avail）。 */
static unsigned g_h_rt_dump;
/* M4r P2：链式解析的最近一次「取证轨迹」（只给 [h] rt miss dump 用，前 8 条）。
   l1 = tex0 处的字（合法地址的那些），l2 = 第二层读到的字。判断「是数组套数组、还是别的东西」。 */
static uint64_t g_ch_l1[8]; static int g_ch_n1;
static uint64_t g_ch_l2[8]; static int g_ch_n2;
static void h_rt_miss_dump(uint64_t key,int side_miss){
 if(g_h_rt_dump>=8)return;g_h_rt_dump++;
#ifdef __MUSL__
 uint32_t w[8]={0};int ok=1;
 for(int i=0;i<8;i++)if(!rt_peek_u32(key+(uint64_t)i*4,&w[i])){ok=0;break;}
 /* 反查：这个地址是否落在某张已登记纹理的池数据区间里（是 ⇒ 它是数据地址，不是 NVNtexture 对象） */
 int in_pool=-1;unsigned long long pkey=0;
 for(unsigned i=0;i<g_texrec_n;i++){const texsnap *r=&g_texrec[i];
  if(!r->key||!r->mem||!r->avail)continue;
  uint64_t a=(uint64_t)(uintptr_t)r->mem;
  if(key>=a&&key<a+r->avail){in_pool=(int)i;pkey=(unsigned long long)(uintptr_t)r->key;break;}}
 vk_boot_logf("[h] rt miss dump tex0=%#llx side_miss=%d rd=%d words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x in_pool=%d owner=%#llx",
              (unsigned long long)key,side_miss,ok,w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7],in_pool,pkey);
 /* M4r P2：链式解析的轨迹（这些 key 到底长什么样：一层是数组、二层是数组，还是别的东西） */
 vk_boot_logf("[h] rt miss chain tex0=%#llx l1n=%d l1=%#llx,%#llx,%#llx,%#llx l2n=%d l2=%#llx,%#llx,%#llx,%#llx",
              (unsigned long long)key,g_ch_n1,
              (unsigned long long)g_ch_l1[0],(unsigned long long)g_ch_l1[1],
              (unsigned long long)g_ch_l1[2],(unsigned long long)g_ch_l1[3],g_ch_n2,
              (unsigned long long)g_ch_l2[0],(unsigned long long)g_ch_l2[1],
              (unsigned long long)g_ch_l2[2],(unsigned long long)g_ch_l2[3]);
#else
 vk_boot_logf("[h] rt miss dump tex0=%#llx side_miss=%d (peek unavailable on this arch)",(unsigned long long)key,side_miss);
#endif
}
/* ================= M6b A1：probe_miss 的结构化归因（只读，只记不判） =================
   派活单要求「先归因、给数字、不猜」。离线能确定的只有「tex0 不是侧表/快照/magic 对象」；
   剩下的事情必须在设备上量：这些指针后面到底是什么。这里按**读到的字**分六类：
     unread    = tex0 连 8 个字都读不出来（不可读地址）
     magic_l1  = tex0 的字里有一个本身就是合法纹理对象（说明是「窗口/层数」问题而不是身份问题）
     deep      = 层 1 候选的字里有合法纹理对象（老链层 2 窗口 4 字没扫到；M6b 已扩到 8 字）
     pooldata  = 层 1 候选的字落在某张已登记纹理的池数据区（说明拿到的是数据地址，不是对象）
     interior  = 层 1 候选落在某个已登记纹理对象的 ±0x200 内（传递的是内部指针）
     unknown   = 读得出来，但没有任何一条能验证的线索（= 目前无法安全解析）
   成本：每个**不同的** tex0 只归类一次（顶层表 16 项），之后只累加出现次数。 */
#define RT_MISS_TOP 16
static const char *const g_rt_miss_why_s[7] = { "unread","magic_l1","deep","pooldata","interior","unknown","l2_struct" };
static struct {
    uint64_t key;
    unsigned long long n;          /* 该 tex0 作为 tex0 出现并被计为 miss 的次数 */
    unsigned l1n, magic, deep, pool, inside;
    unsigned why;
} g_rt_miss_top[RT_MISS_TOP];
static unsigned g_rt_miss_ntop, g_rt_miss_ovf;
static unsigned long long g_rt_miss_why[7];
static unsigned g_rt_miss_class_log;
static int rt_addr_in_texpool(uint64_t a)
{
    for (unsigned i = 0; i < g_texrec_n; i++) {
        const texsnap *r = &g_texrec[i];
        if (!r->key || !r->mem || !r->avail) continue;
        uint64_t s = (uint64_t)(uintptr_t)r->mem;
        if (a >= s && a < s + r->avail) return 1;
    }
    return 0;
}
static int rt_addr_near_texobj(uint64_t a)
{
    for (unsigned i = 0; i < g_texrec_n; i++) {
        const texsnap *r = &g_texrec[i];
        uint64_t k = (uint64_t)(uintptr_t)r->key;
        if (!k) continue;
        if (a + 0x200u >= k && a <= k + 0x200u) return 1;
    }
    return 0;
}
/* M6c B：present 源基准（用于给层 2 的稳定指针做偏移归因）。
   M6b/M6c 证据：层 2 反复出现的两个值恒等于**第一张 present 源纹理对象**（1600x900
   fmt=0x25 tgt=1）的 +0x4a0 / +0x82000，且跨 boot 偏移一致（m6a_b/m6b_a/m6b_b/m4br/m5q
   五份日志同族）。这里只在**已登记快照表**里取最小 key，不解引用任何东西。 */
static uint64_t g_rt_xbase;
static int g_rt_xbase_tried;
static uint64_t rt_xbase(void)
{
    if (!g_rt_xbase_tried) {
        g_rt_xbase_tried = 1;
        uint64_t best = 0;
        for (unsigned i = 0; i < g_texrec_n; i++) {
            const texsnap *r = &g_texrec[i];
            if (!r->key) continue;
            if (r->d.width != 1600 || r->d.height != 900) continue;
            if (r->d.format != 0x25 || r->d.target != 1) continue;
            uint64_t k = (uint64_t)(uintptr_t)r->key;
            if (!best || k < best) best = k;
        }
        g_rt_xbase = best;
    }
    return g_rt_xbase;
}
#define RT_XOFF_A 0x4a0ULL
#define RT_XOFF_B 0x82000ULL
static void rt_miss_classify(uint64_t tex0)
{
#ifndef __MUSL__
    (void)tex0;
    return;
#else
    /* M6b b：先查顶层表，命中就只累加（boot A 实测每笔 miss 都重跑分类 = 每次 16K×8 次
       表扫描，白烧 CPU）。只有**第一次**见到这个 tex0 才做结构判定。 */
    int e = -1;
    for (unsigned i = 0; i < g_rt_miss_ntop; i++) if (g_rt_miss_top[i].key == tex0) { e = (int)i; break; }
    if (e >= 0) { g_rt_miss_top[e].n++; g_rt_miss_why[g_rt_miss_top[e].why]++; return; }
    if (g_rt_miss_ntop >= RT_MISS_TOP) { g_rt_miss_ovf++; return; }
    unsigned l1n = 0, magic = 0, deep = 0, pool = 0, inside = 0, why = 5;
    uint64_t l2off = 0, l2cand = 0;
    uint64_t v[8]; int rd = 0;
    for (int i = 0; i < 8; i++) if (rt_peek_u64(tex0 + (uint64_t)i * 8, &v[i])) rd = 1;
    if (!rd) why = 0;
    else {
        uint64_t l1[8]; int n1 = 0;
        for (int i = 0; i < 8; i++)
            if (v[i] > 0x100000000ULL && v[i] < 0x800000000000ULL && n1 < 8) l1[n1++] = v[i];
        l1n = (unsigned)n1;
        for (int a = 0; a < n1; a++) {
            rt_ident t;
            if (h_rt_lookup(l1[a], &t, 1)) { magic++; continue; }
            if (rt_addr_near_texobj(l1[a])) inside++;
            uint64_t wv[8];
            for (int k = 0; k < 8; k++) {
                if (!rt_peek_u64(l1[a] + (uint64_t)k * 8, &wv[k]) || !wv[k]) continue;
                if (h_rt_lookup(wv[k], &t, 1)) { deep++; break; }
                if (rt_addr_in_texpool(wv[k])) { pool++; break; }
            }
        }
        if (magic) why = 1; else if (deep) why = 2; else if (pool) why = 3;
        /* M6c B：新桶 l2_struct —— 层 2 里出现「present 源基准 + 0x4a0 / +0x82000」这类
           布局指针。只做**算术比对**（不解引用、不做身份），所以它既不是 magic 也不是 deep。
           证据来源见 rt_xbase() 注释。 */
        if (why == 5 && n1) {
            uint64_t xb = rt_xbase();
            if (xb) {
                for (int a = 0; a < n1 && !l2off; a++) {
                    uint64_t wv[8];
                    for (int k = 0; k < 8; k++) {
                        if (!rt_peek_u64(l1[a] + (uint64_t)k * 8, &wv[k]) || !wv[k]) continue;
                        if (wv[k] == xb + RT_XOFF_A) { l2off = RT_XOFF_A; l2cand = l1[a]; break; }
                        if (wv[k] == xb + RT_XOFF_B) { l2off = RT_XOFF_B; l2cand = l1[a]; break; }
                    }
                }
                if (l2off) why = 6;
            }
        }
        if (why == 5 && inside) why = 4;
    }
    e = (int)g_rt_miss_ntop++;
    memset(&g_rt_miss_top[e], 0, sizeof g_rt_miss_top[e]);
    g_rt_miss_top[e].key = tex0;
    g_rt_miss_top[e].l1n = l1n; g_rt_miss_top[e].magic = magic;
    g_rt_miss_top[e].deep = deep; g_rt_miss_top[e].pool = pool;
    g_rt_miss_top[e].inside = inside; g_rt_miss_top[e].why = why;
    g_rt_miss_top[e].n = 1;
    g_rt_miss_why[why]++;
    if (g_rt_miss_class_log < 24) { g_rt_miss_class_log++;
        vk_boot_logf("[h] rt miss cls key=%#llx l1n=%u magic=%u deep=%u pool=%u in=%u why=%s off=0x%llx cand=%#llx",
                     (unsigned long long)tex0, l1n, magic, deep, pool, inside, g_rt_miss_why_s[why],
                     (unsigned long long)l2off, (unsigned long long)l2cand); }
#endif
}
/* M6c B：id3 —— 层 2 「结构指针」的**只读**解码尝试（span 校验后才读，绝不无条件解引用）。
   对每个 miss（前 8 个不同 tex0）：扫层 1 候选的 8 字，找到 present 源基准 +0x4a0 / +0x82000
   时，把该候选的 8 字原样打出来，并对候选本身做 h_rt_lookup 复核；
   `verified=1` 的唯一条件是候选通过既有解析链（侧表/快照/magic span）——那是真身份；
   否则 verified=0（本文件不会用未验证的值参与绑定，绑定侧仍然 src=miss -> host）。
   同时一次性 dump 基准 +0x4a0 / +0x82000 处的 24 字，作为下一轮结构化的现场证据。 */
static unsigned g_rt_id3_log;
static unsigned g_rt_struct_dump;
static void rt_miss_id3(uint64_t tex0)
{
#ifdef __MUSL__
    if (g_rt_id3_log >= 8) return;
    uint64_t xb = rt_xbase();
    if (!xb) { vk_boot_logf("[h] rt miss id3 tex0=%#llx base=0 verified=0 why=nobase", (unsigned long long)tex0); return; }
    if (g_rt_struct_dump == 0) {
        g_rt_struct_dump = 1;
        for (int q = 0; q < 2; q++) {
            uint64_t at = xb + (q ? RT_XOFF_B : RT_XOFF_A);
            uint64_t w[24]; int ok = rt_peek_span(at, w, sizeof w);
            if (!ok) {
                vk_boot_logf("[h] rt struct base=%#llx off=0x%llx at=%#llx span=UNREADABLE",
                    (unsigned long long)xb, (unsigned long long)(q ? RT_XOFF_B : RT_XOFF_A), (unsigned long long)at);
                continue;
            }
            for (int half = 0; half < 2; half++) {
                const uint64_t *p = w + half * 12;
                vk_boot_logf("[h] rt struct base=%#llx off=0x%llx at=%#llx i=%d..%d %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx",
                    (unsigned long long)xb, (unsigned long long)(q ? RT_XOFF_B : RT_XOFF_A), (unsigned long long)at,
                    half * 12, half * 12 + 11,
                    (unsigned long long)p[0], (unsigned long long)p[1], (unsigned long long)p[2], (unsigned long long)p[3],
                    (unsigned long long)p[4], (unsigned long long)p[5], (unsigned long long)p[6], (unsigned long long)p[7],
                    (unsigned long long)p[8], (unsigned long long)p[9], (unsigned long long)p[10], (unsigned long long)p[11]);
            }
        }
    }
    uint64_t v[8];
    for (int i = 0; i < 8; i++) if (!rt_peek_u64(tex0 + (uint64_t)i * 8, &v[i])) v[i] = 0;
    for (int i = 0; i < 8; i++) {
        if (v[i] <= 0x100000000ULL || v[i] >= 0x800000000000ULL) continue;
        uint64_t w[8]; int any = 0;
        for (int k = 0; k < 8; k++) if (!rt_peek_u64(v[i] + (uint64_t)k * 8, &w[k])) w[k] = 0;
        for (int k = 0; k < 8; k++)
            if (w[k] == xb + RT_XOFF_A || w[k] == xb + RT_XOFF_B) { any = 1; break; }
        if (!any) continue;
        if (g_rt_id3_log++ >= 8) return;
        rt_ident t; int hit = h_rt_lookup(v[i], &t, 1);
        vk_boot_logf("[h] rt miss id3 tex0=%#llx base=%#llx cand=%#llx off=0x%llx texid=%d verified=%d w0..7=%#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx",
            (unsigned long long)tex0, (unsigned long long)xb, (unsigned long long)v[i],
            (unsigned long long)((w[0] == xb + RT_XOFF_A || w[0] == xb + RT_XOFF_B) ? (w[0] - xb)
                                : (w[1] == xb + RT_XOFF_A || w[1] == xb + RT_XOFF_B) ? (w[1] - xb)
                                : (w[2] == xb + RT_XOFF_A || w[2] == xb + RT_XOFF_B) ? (w[2] - xb)
                                : (w[3] == xb + RT_XOFF_A || w[3] == xb + RT_XOFF_B) ? (w[3] - xb)
                                : (w[4] == xb + RT_XOFF_A || w[4] == xb + RT_XOFF_B) ? (w[4] - xb)
                                : (w[5] == xb + RT_XOFF_A || w[5] == xb + RT_XOFF_B) ? (w[5] - xb)
                                : (w[6] == xb + RT_XOFF_A || w[6] == xb + RT_XOFF_B) ? (w[6] - xb)
                                : (w[7] - xb)),
            hit ? t.id : -1, (hit && t.w && t.h) ? 1 : 0,
            (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3],
            (unsigned long long)w[4], (unsigned long long)w[5], (unsigned long long)w[6], (unsigned long long)w[7]);
        return;
    }
    if (g_rt_id3_log++ < 8)
        vk_boot_logf("[h] rt miss id3 tex0=%#llx base=%#llx cand=0 off=0x0 texid=-1 verified=0 why=no_struct",
                     (unsigned long long)tex0, (unsigned long long)xb);
#else
    (void)tex0;
#endif
}
/* M4r P2：句柄级解析（零系统调用）。SET_RENDER_TARGETS 的一些调用点传进来的既不是
   NVNtexture 对象、也不是「对象指针的数组」，而是 **NVN 纹理句柄**（tid | sampler<<20）——
   m4q_b2 的 `rt miss dump` 里 `words=00000080,0000000b,...` 这种「高位 0、低位像 20 位 id」的值
   就是它。这里用已有的 id→快照表（g_texid2）直接还原尺寸/格式/存储地址，判据是「该 id 槽确有
   经过宿主 Initialize 的快照」，所以误判概率极低。 */
static int h_rt_handle_x(uint64_t v, rt_ident *o, int allow_depth)
{
    if (!v || (v >> 32)) return 0;             /* 句柄是 32 位 */
    unsigned tid = (unsigned)(v & 0xFFFFFu);
    if (tid >= TEXREG_N) return 0;
    const texsnap *s = &g_texid2[tid];
    if (!s->key || !s->d.width || !s->d.height) return 0;
    if (s->d.width < 64 || s->d.height < 64) return 0;
    if (s->d.width > 4096 || s->d.height > 4096) return 0;
    /* M4bo P2：这条「深度纹理不是颜色 RT」的拒绝是**颜色侧**的保护（颜色目标不能绑深度图）。
       深度侧正相反——深度声明本来就该解到 0x33/0x34，所以给一个 allow_depth 口子，
       判据仍然完整（id 槽确有真快照 + 尺寸合理），只是不再按格式排除。 */
    if (!allow_depth && (s->d.format == 0x33 || s->d.format == 0x34)) return 0;
    o->th = NULL; o->rec = s; o->key = s->key; o->mem = s->mem;
    o->w = s->d.width; o->h = s->d.height; o->fmt = s->d.format;
    o->tgt = s->d.target; o->flags = s->d.flags; o->id = (int)tid; o->src = "handle";
    return 1;
}
static int h_rt_handle(uint64_t v, rt_ident *o) { return h_rt_handle_x(v, o, 0); }
/* M4bo P2 收口：深度侧的句柄解码**必须**是深度格式。
   boot#2 实测（本链第一版放行了任意格式）：`[n] dk chain depth=0x7f64172570 id=2048 512x512 fmt=0x48 level=5`
   —— level 5 是把 64 位字的高半 0x10 当句柄（tid=16）解出来的**颜色**纹理；随后
   `[n] dk detach … ok=1 dkey=0x6eca41b000 ours_was=0x6e225b1000 why=size_or_slot_mismatch`
   反而把本来挂对的 1600x900 深度**清掉**（attach 1182→890、draws_color_depth 1177→900）。
   ⇒ 深度侧只接受真正解到 0x33/0x34 的候选；解不到就维持旧行为（宁可不挂，也不挂错）。 */
static int h_rt_handle_depth(uint64_t v, rt_ident *o)
{
    if (!h_rt_handle_x(v, o, 1)) return 0;
    return (o->fmt == 0x33 || o->fmt == 0x34) ? 1 : 0;
}
/* M4r P2：多层指针链解析（带验证）。层级：
     1 = tex0 处的第 k 个 64 位字本身就是纹理对象（同时接住「colors 数组基址」与「&colors」两种布局；
         m4q 的旧 ind 只读 2 个字，这里按 n 读到 4 个 —— m4q_b2 的 dump 里 e2/e3 正是 0x6e… 的堆指针）
     2 = 第 1 层的值是**另一层数组/包装结构**的基址，再取它的字
     4 = 值是 NVN 纹理句柄（h_rt_handle；放在最后，只在 1/2 全失败后才试，避免把普通整数误当句柄）
   纪律：层 1/2 的候选必须能在既有解析链（侧表/快照/magic）里命中才接受 ⇒ 正常路径不可能被解坏；
   层 2 只在层 1 失败时发生，而且候选必须是合法用户态地址、可读。
   成本：每笔最多 4+8 次 process_vm_readv，失败结果进负缓存（栈地址逐帧复用）。 */
static int h_rt_chain(uint64_t tex0, int n, rt_ident *ci, int *out_lev)
{
#ifdef __MUSL__
    (void)n;
    g_ch_n1 = 0; g_ch_n2 = 0;
    /* boot#1 证据：这些 miss 的 tex0 指向「包装结构/栈帧」而不是 n 项指针数组（l2 读出来是
       ARM64 指令字），按 n 读根本没摸到目标 ⇒ 固定扫 8 字窗口。命中必须通过 h_rt_lookup 验证，
       所以扫宽只会多花几次读取，不会把正常路径解坏。
       M6b A：8 → 16 字、层 1 候选 4 → 8、层 2 每候选 4 → 8 字（miss 键只有 ~9 个且进负缓存，
       加宽的成本可以忽略；仍然只接受 h_rt_lookup 验证过的对象）。 */
    const int lim = 16;
    uint64_t l1[8]; int n1 = 0;
    uint64_t words[16]; int nw = 0;
    for (int k = 0; k < lim; k++) {
        uint64_t v = 0;
        if (!rt_peek_u64(tex0 + (uint64_t)k * 8, &v) || !v) continue;
        words[nw++] = v;
        if (h_rt_lookup(v, ci, 1)) { *out_lev = 1; return 1; }
        if (v > 0x100000000ULL && v < 0x800000000000ULL && n1 < 8) {
            l1[n1++] = v;
            g_ch_l1[g_ch_n1++] = v;
        }
    }
    for (int a = 0; a < n1; a++)
        for (int k = 0; k < 8; k++) {
            uint64_t v = 0;
            if (!rt_peek_u64(l1[a] + (uint64_t)k * 8, &v) || !v) continue;
            if (g_ch_n2 < 8) g_ch_l2[g_ch_n2++] = v;
            if (h_rt_lookup(v, ci, 1)) { *out_lev = 2; return 1; }
        }
    /* M6b b：层 3（包装 → 中间结构 → 对象指针）。层 1/2 全失败才进入，预算 ≤8 个层 2 值 × 8 字。
       **唯一性守卫**：把这一层所有 magic 验证通过的候选按身份（mem/w/h/fmt）去重，只有
       「恰好 1 个不同身份」才接受。多个不同身份 ⇒ 说明这个结构里挂着多张纹理（池/表），
       按任何一个都是拿别的图顶替 ⇒ 不绑，计数后在 miss 行里说明。
       **默认关闭**（RT_DEEP_ACCEPT=0）。boot B 实测：它能解出 1255 笔（src=ind3），但
       (a) 压出了 h_rt_lookup 的越界崩溃（已修，见 rt_peek_span），(b) 解出的多是
       `1600x900 fmt=0x3d id=-1` 这类**兄弟形状**，正确性没有证据。按纪律不默认启用；
       下一轮用它做 A/B（改成 1 重编即可），并必须核对绑到的身份是否就是该笔的目标。 */
    if (RT_DEEP_ACCEPT) {
        uint64_t l2[8]; int n2 = 0;
        for (int a = 0; a < n1 && n2 < 8; a++)
            for (int k = 0; k < 8 && n2 < 8; k++) {
                uint64_t v = 0;
                if (!rt_peek_u64(l1[a] + (uint64_t)k * 8, &v) || !v) continue;
                if (v > 0x100000000ULL && v < 0x800000000000ULL) l2[n2++] = v;
            }
        rt_ident uniq; int have = 0, ncand = 0;
        memset(&uniq, 0, sizeof uniq);
        for (int a = 0; a < n2; a++)
            for (int k = 0; k < 8; k++) {
                uint64_t v = 0; rt_ident t;
                if (!rt_peek_u64(l2[a] + (uint64_t)k * 8, &v) || !v) continue;
                if (!h_rt_lookup(v, &t, 1)) continue;
                ncand++;
                if (!have) { uniq = t; have = 1; }
                else if (uniq.mem != t.mem || uniq.w != t.w || uniq.h != t.h || uniq.fmt != t.fmt) {
                    g_rt_deep_amb++;
                    have = -1;            /* 多个不同身份：放弃整条层 3 */
                    a = n2; break;
                }
            }
        if (have == 1) { *ci = uniq; *out_lev = 3; g_rt_deep_hit++; return 1; }
        if (have == 0) g_rt_deep_none++;
        (void)ncand;
    }
    for (int i = 0; i < nw; i++)   /* 最后才试句柄解码 */
        if (h_rt_handle(words[i], ci)) { *out_lev = 4; return 1; }
    /* M4s P0②：m4r 的 dump 里 `0x0b00000080` 很像**两个 32 位句柄打包**（lo=0x80、hi=0x0b）。
       一个 64 位字的上/下半各试一次（只在层 1/层 2/整字句柄全失败之后），判据仍是 h_rt_handle
       那四条（id 槽有真快照 / 尺寸合理 / 非深度格式）⇒ 不会把普通整数误当句柄。 */
    for (int i = 0; i < nw; i++) {
        uint32_t lo = (uint32_t)(words[i] & 0xFFFFFFFFu);
        uint32_t hi = (uint32_t)(words[i] >> 32);
        if (hi && h_rt_handle((uint64_t)hi, ci)) { *out_lev = 5; return 1; }
        if (lo && h_rt_handle((uint64_t)lo, ci)) { *out_lev = 6; return 1; }
    }
#endif
    (void)tex0; (void)n; (void)ci; (void)out_lev;
    return 0;
}

/* ===== M4bo P2（本 boot 唯一行为改动）：深度侧解析链补齐 =====
   P1 证据（boot#1）：同一个 n=1 调用点在两种指针之间交替——
     ① `0x6ebd6f30e8`：真 NVNtexture 对象，magic 命中（`src=tex_h`），1600x900 fmt=0x34，解得开；
     ② `0x7f641b2570 / 0x7f641b2be8 / 0x7f641b2a00`：**栈上的包装结构**，
        `h_rt_lookup` 三级 + M4bn 的 8 字链（h_rt_chain）全部解不出（`snap=0 src=none 0x0`）。
   两个缺口：
     a) `h_rt_chain` 的 8 字窗口是按**颜色数组**布局定的；深度是单个对象，包装更深；
     b) `h_rt_handle` 显式拒绝 0x33/0x34（颜色侧的正确保护）⇒ 包装里若是**深度句柄**永远解不出。
   修法（只对深度声明生效，颜色路径一行不动）：
     · 窗口 8 → 32 字（0..0xF8）；
     · 层 1/2 的候选必须解出**深度格式**（0x33/0x34）才接受 —— 深度参数不该指向颜色 RT；
     · 最后放行深度格式句柄（整字 / 高半 / 低半三档，判据仍是 h_rt_handle_x 的完整校验）。 */
static int h_d_chain(uint64_t depth, rt_ident *ci, int *out_lev)
{
#ifdef __MUSL__
    const int lim = 32;
    uint64_t l1[8], words[32];
    int n1 = 0, nw = 0, k, a, i;
    for (k = 0; k < lim; k++) {
        uint64_t v = 0;
        rt_ident t;
        if (!rt_peek_u64(depth + (uint64_t)k * 8, &v) || !v) continue;
        if (nw < lim) words[nw++] = v;
        if (h_rt_lookup(v, &t, 1) && (t.fmt == 0x33 || t.fmt == 0x34)) { *ci = t; *out_lev = 1; return 1; }
        if (v > 0x100000000ULL && v < 0x800000000000ULL && n1 < 8) l1[n1++] = v;
    }
    for (a = 0; a < n1; a++)
        for (k = 0; k < 8; k++) {
            uint64_t v = 0;
            rt_ident t;
            if (!rt_peek_u64(l1[a] + (uint64_t)k * 8, &v) || !v) continue;
            if (h_rt_lookup(v, &t, 1) && (t.fmt == 0x33 || t.fmt == 0x34)) { *ci = t; *out_lev = 2; return 1; }
        }
    for (i = 0; i < nw; i++)
        if (h_rt_handle_depth(words[i], ci)) { *out_lev = 4; return 1; }
    /* 高半/低半句柄解码**不做**：boot#2 证明 64 位字的高半常是小整数（0x10 = tid 16），
       会把颜色纹理解成深度。深度侧宁缺勿滥。 */
#endif
    (void)depth; (void)ci; (void)out_lev;
    return 0;
}
/* M4bo P1/P2（只读）：深度解析彻底失败时的取证（前 2 次）——8 个字 + 是否落在某纹理池数据区内。
   有了这两行，就算 P2 修法没命中，也能直接看出包装结构长什么样（下一步的成本就定了）。 */
static unsigned g_d_miss_dump;
static void h_d_miss_dump(uint64_t key, int n)
{
    if (g_d_miss_dump >= 2) return;
    g_d_miss_dump++;
#ifdef __MUSL__
    uint32_t w[8] = {0};
    int k, ok = 1, in_pool = -1;
    unsigned long long owner = 0;
    for (k = 0; k < 8; k++) if (!rt_peek_u32(key + (uint64_t)k * 4, &w[k])) { ok = 0; break; }
    for (unsigned i = 0; i < g_texrec_n; i++) {
        const texsnap *r = &g_texrec[i];
        if (!r->key || !r->mem || !r->avail) continue;
        uint64_t at = (uint64_t)(uintptr_t)r->mem;
        if (key >= at && key < at + r->avail) { in_pool = (int)i; owner = (unsigned long long)(uintptr_t)r->key; break; }
    }
    vk_boot_logf("[o] dpass decl-miss n=%d depth=%#llx rd=%d words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
                 "in_pool=%d owner=%#llx", n, (unsigned long long)key, ok,
                 w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], in_pool, owner);
#else
    vk_boot_logf("[o] dpass decl-miss n=%d depth=%#llx (peek unavailable)", n, (unsigned long long)key);
#endif
}
/* M4r P2：失败负缓存。栈上那几个「解不出」的 tex0 在帧间反复出现（m4q_b2 的 409,644 次 probe
   集中在 4~6 个地址上），每次都重新跑一遍链会白烧 16 次系统调用。这里只缓存**失败**，
   有效期 512 次 RT 绑定（TTL 短，避免「后来能解了」被缓存挡住）。
   注意：命中缓存的失败仍然计入 probe（口径不变，只是省掉重算）。 */
#define RT_NEGC 128
static struct { uint64_t key; unsigned long long until; } g_rt_neg[RT_NEGC];
static unsigned g_rt_neg_i;
static unsigned long long g_h_rt_seq;
static int rt_neg_hit(uint64_t k)
{
    for (int i = 0; i < RT_NEGC; i++)
        if (g_rt_neg[i].key == k && g_rt_neg[i].until > g_h_rt_seq) return 1;
    return 0;
}
static void rt_neg_put(uint64_t k)
{
    unsigned i = g_rt_neg_i++ % RT_NEGC;
    g_rt_neg[i].key = k; g_rt_neg[i].until = g_h_rt_seq + 512;
}
static int g_h_rt_bridge(int n,uint64_t tex0,uint64_t depth,void *user){
 (void)user;unsigned long long img=0;unsigned w=0,h=0;
 g_h_rt_seq++;
 rt_ident ci;int ok=h_rt_lookup(tex0,&ci,1);
 /* M4q P1·boot#1 实测：还剩一类 key 解不出来——`0x7f6e62f6b8` 这类**指向栈上指针数组**的地址
   （`[h] rt miss dump … words=91a29670,0000007f,91cbb688,0000007f,…` 全是 64 位指针，
   既没有 tex_h.magic 也不在快照表里）。它们是 `NVNtexture* const*` 的**数组地址**被当成元素
   传了下来（n=1 时数组只有 1 项）。M4r P2：把「顺指针解一层」升级成带验证的多层链
   （h_rt_chain），并加 NVN 句柄解码 + 失败负缓存。 */
 int lev=0;
 if(!ok&&tex0){
  if(rt_neg_hit(tex0))g_h_rt_neg_hit++;
  else if(h_rt_chain(tex0,n,&ci,&lev))ok=1;
  else rt_neg_put(tex0);
 }
 if(ok&&lev){
  if(lev==1)ci.src="ind0";else if(lev==2)ci.src="ind2";else if(lev==3)ci.src="ind3";else ci.src="handle";
  if(g_h_rt_norm_log<8){g_h_rt_norm_log++;
   vk_boot_logf("[h] rt norm src=%s tex0=%#llx id=%d %ux%u fmt=0x%x (chain level=%d)",
                ci.src,(unsigned long long)tex0,ci.id,ci.w,ci.h,ci.fmt,lev);}
 }
 if(!ok){
  if(tex0)g_h_rt_probe_miss++;   /* tex0==0 = n=0 的纯深度 pass，不算「找错图」，单独计数 */
  else g_h_rt_n0++;
  /* M6b A：**撤回 M4c 的「格式优先挑最大 1600x900 fmt=0x3d」兜底**。
     M6a 的 `[g] rt table` 证据：那一步把 miss 的 tex0 做成槽（key=栈/暂存区指针），尺寸和 id
     都取自 g_rt_snap_probe（= 别人的纹理），于是 9 个假槽（fmt=0/texid=1930）真收了 draw
     （writers 1..474），还把 id 1930 绑到了别人的图上。这是「拿别的 image 顶替」，本轮撤掉：
     解析不出就**不建身份**，交给调用方按宿主图回落（h_rt_ensure 收 0 尺寸直接返 NULL）。
     仍保留 probe_miss 计数（口径不变）与结构化归因（rt_miss_classify）。 */
  ci.w=0;ci.h=0;ci.id=-1;ci.src="miss";
  if(tex0){
   static unsigned misslog;
   rt_miss_classify(tex0);
   rt_miss_id3(tex0);
   if(misslog<8){misslog++;
    vk_boot_logf("[h] rt miss -> host (no fabricated identity) tex0=%#llx n=%d",(unsigned long long)tex0,n);}
   h_rt_miss_dump(tex0,1);
  }
 }
 /* M4t P1②：**正向补齐**——解析成功（尺寸/格式都在）但 id=-1 时，按存储身份反查 id。
   m4s_b2 的场景 RT 就是这一支：`[h] rt chg#1 … id=-1 … src=tex_h st=0x6e32440000/mem`，
   而 id 282 的快照 mem 正是 0x6e32440000 ⇒ 这一条把「RT 槽 ↔ id 282」在 SET_RENDER_TARGETS
   当刻就接上，比「等 BIND_TEXTURES 反向登记」更早（同一帧里就能命中）。 */
 if(ok&&ci.id<0&&ci.mem){
  static unsigned sid_log;
  if(g_h_rt_seq%4096==0)g_idstore_n=0;   /* 记忆失效（新 id 可能在更晚的 Initialize 里登记） */
  int sid=g_tex_id_by_store(ci.mem,ci.w,ci.h,ci.fmt);
  if(sid>=0){ci.id=sid;
   if(sid_log<8){sid_log++;
    vk_boot_logf("[t] rt id link src=store key=%#llx id=%d ext=%ux%u fmt=0x%x (before rt_bind)",
                 (unsigned long long)tex0,sid,ci.w,ci.h,ci.fmt);}}
 }
 /* M4w P2：记录「这个 id 当过 RT」，供 never-up top20 判定「应为 RT 却没人写」那一类。 */
 if(ok&&ci.id>=0&&ci.id<TEXREG_N)g_rt_id_seen[ci.id]=1;
 /* 解析来源直方图（验收口径：看哪一级在起作用、probe 是否真的归零） */
 if(ok){if(!strcmp(ci.src,"snap"))g_h_rt_by_snap++;else if(!strcmp(ci.src,"rec"))g_h_rt_by_rec++;
        else if(!strcmp(ci.src,"tex_h"))g_h_rt_by_texh++;
        else if(lev==1)g_h_rt_by_ind++;else if(lev==2)g_h_rt_by_ind2++;else if(lev==3)g_h_rt_by_ind3++;else g_h_rt_by_handle++;}
 else if(tex0)g_h_rt_by_probe++;
 if(g_h_rt_logged<8)vk_boot_logf("[h] rt id key=%#llx id=%d src=%s",(unsigned long long)tex0,ci.id,ci.src);
 /* M4q P1：槽 key 用「存储身份」——池数据地址（tex_h.mem / texsnap.mem）在对象轮转重建时不变，
   对象指针每帧换 ⇒ 16 槽必满（m4p_b1 的 595000 次 rt fallback 就是这么来的）。
   拿不到 mem（未登记）才退回对象指针，并在 chg 行里标 st=…/ptr。 */
 unsigned long long stk=(unsigned long long)(uintptr_t)ci.mem;
 const char *stk_src="mem";
 if(!stk){stk=(unsigned long long)tex0;stk_src="ptr";}
 if(!ci.mem)g_h_rt_stk_ptr++; else g_h_rt_stk_mem++;   /* M4w P1：join 覆盖上限的分子/分母 */
 /* 深度目标（M4r P1：先解析、先建槽，再把存储身份交给 rt_bind 挂到颜色槽上）。 */
 rt_ident di;int dok=h_rt_lookup(depth,&di,1);
 /* M4bn P2 实验（boot#2 实测**证伪**，已撤掉行为改动，只留结论注释）：
   曾在这里给深度补上颜色侧的 h_rt_chain（层1/层2 指针链 + 句柄解码），想解出世界 pass 的
   间接深度指针。boot#2 日志 `[n] dk chain depth=0x7f638efbe8 id=-1 1600x900 fmt=0x3d level=1`
   显示这些指针链下去命中的是**颜色**纹理（fmt 0x3d = 场景颜色 RT），不是 D32F；
   `nvn_vk_rt_depth_bind` 全部按格式拒收（depth_badfmt 2 → 1065），draw 侧
   `[n] dk totals frame=638 … wd_game_null=770 wd_game_unres=3` 证明世界 draw 落点槽上
   游戏传的就是 **NULL**（不是解析不出）。⇒ 链式解析对深度无效，撤回。
   M4bo P2 修订：撤过头了。boot#1 里同一 n=1 调用点**交替**出现可解对象（0x6ebd6f30e8，tex_h，
   1600x900 fmt=0x34）与栈包装（0x7f641b2570/0x7f641b2be8/0x7f641b2a00，snap=0）⇒ 不是「游戏传
   NULL」，是「一半的声明解不出」。M4bn 的链之所以只解出颜色（fmt 0x3d），是因为它接受了**第一个
   命中**；这里改成只接受**深度格式**的候选（0x33/0x34），并把窗口 8→32 字、放行深度句柄
   （见 h_d_chain）。仍旧只在深度解析失败时进入，颜色路径一行不动。 */
 {static unsigned dch_log;
  if(!dok&&depth){
   int dlev=0; rt_ident t;
   if(h_d_chain((uint64_t)depth,&t,&dlev)){di=t;dok=1;
    if(dch_log<6){dch_log++;
     vk_boot_logf("[n] dk chain depth=%#llx id=%d %ux%u fmt=0x%x level=%d (M4bo P2)",
                  (unsigned long long)depth,di.id,di.w,di.h,di.fmt,dlev);}}
   else h_d_miss_dump((uint64_t)depth,n);
  }}
 {static unsigned long long s_last_d;static unsigned s_dchg;static int s_dany;
  if(depth||n==0){
   if(!s_dany||depth!=s_last_d){
    s_dany=1;s_last_d=depth;
    if(s_dchg<32){s_dchg++;
     vk_boot_logf("[h] rt depth chg#%u n=%d dkey=%#llx snap=%d id=%d %ux%u fmt=0x%x src=%s",
                  s_dchg,n,(unsigned long long)depth,dok?1:0,di.id,di.w,di.h,di.fmt,di.src);}}}}
 unsigned long long dkey=0;
 if(depth&&dok){
  dkey=(unsigned long long)(uintptr_t)(di.mem?di.mem:(const void *)(uintptr_t)depth);
  nvn_vk_rt_depth_bind(dkey,di.w,di.h,di.fmt,di.id);
 }
 /* M4bn P1（只读）：把「游戏这次到底声明了什么深度 / 我们解出来没有」推给回放层，
    供 draw 侧对账（世界 draw 全 dk=0 的归因）。必须在 nvn_vk_rt_bind 之前。 */
 nvn_vk_dk_note(n,(unsigned long long)depth,dok?1:0,di.w,di.h,di.fmt);
 /* M4bo P1（只读）：同一时刻把「这条声明」交给回放层做深度 pass 对账（next_draws = 到下一条声明
    之间的 draw 笔数）。只记账/限流打印，不参与任何绑定或解析决定。 */
 nvn_vk_dp_decl(n,(unsigned long long)depth,dok?1:0,di.w,di.h,di.fmt);
 /* M4o P1/P2 取证：RT 身份每次变化都落一行（侧表命中与否 + 真实尺寸/格式/target）。 */
 {static unsigned long long s_last_key;static unsigned s_chg;static int s_any;
  if(!s_any||tex0!=s_last_key){
   s_any=1;s_last_key=tex0;
   if(s_chg<64){s_chg++;
    /* M4y P2：格式/layout 台账——swz=swizzle(rgba) bl=1 即 BLOCK_LINEAR bpp=每块字节，
       与 [t] tex init 行同口径对账；RT 侧我们自建的 VkImage 恒为 RGBA8/OPTIMAL（见 [h] rt slot）。 */
    vk_boot_logf("[h] rt chg#%u n=%d tex0=%#llx snap=%d id=%d %ux%u fmt=0x%x tgt=%d fl=%#x src=%s st=%#llx/%s dk=%#llx swz=%u%u%u%u bl=%d bpp=%d",
                 s_chg,n,(unsigned long long)tex0,ci.th?1:0,ci.id,ci.w,ci.h,
                 ci.fmt,ci.tgt,ci.flags,ci.src,stk,stk_src,dkey,
                 ci.th?ci.th->d.swizzle_r:0,ci.th?ci.th->d.swizzle_g:0,
                 ci.th?ci.th->d.swizzle_b:0,ci.th?ci.th->d.swizzle_a:0,
                 ci.th?(nvn_tex_is_linear(&ci.th->d)?0:1):-1,nvn_fmt_bpb((int)ci.fmt,NULL,NULL));}}}
 int rv=nvn_vk_rt_bind(n,stk,ci.w,ci.h,ci.id,ci.fmt,dkey,&img,&w,&h);   /* 记录进按 submit 批次的命令缓冲（不逐次 submit） */
 if(rv==0)g_h_rt_bind_ok++;else g_h_rt_bind_err++;
 if(g_h_rt_logged<16||g_h_rt_logged%20000==0){
  int slots=0;unsigned long long fb=0,sw=0;nvn_vk_rt_targets(&slots,&fb,&sw);
  vk_boot_logf("[h] rt bind n=%d key=%#llx st=%#llx snap=%ux%u img=%#llx ext=%ux%u rv=%d slots=%d fallback=%llu dk=%#llx fmt=0x%x id=%d",
               n,(unsigned long long)tex0,stk,ci.w,ci.h,img,w,h,rv,slots,fb,dkey,ci.fmt,ci.id);}
 g_h_rt_logged++;
 return rv;
}
/* ---- M3u（2026-11-03）P2：同一 submit 内的清屏顺序 ----
   本作每帧先清黑（0,0,0,1）再清白（1,1,1,0）：内存里最终留下的是**最后一次 clear**，
   所以回读差分必须用它做基线（PPM 丢 alpha 正是上一轮「100% 白」判不了真假的原因）。
   这里按 submit 收集 clear 颜色，流走完打一行「谁是生效者」，并把连续两次及以上的 submit 计数。 */
#define H_CLEAR_SEQ 4
static float g_clear_seq[H_CLEAR_SEQ][4];
static unsigned g_clear_seq_n;
static unsigned long long g_clear_dual_submits;
/* ---- M4n P1：submit 行日志瘦身 ----
   旧口径：每帧两笔 clear（先清黑再清白）各落 2 行（[h] submit + [h] vkQueueSubmit）= 4 行/帧。
   434 s 的一次 boot 里 vk_boot.log 被这两行灌到 1.4 GB（设备侧实测 1478477377 B），远超其余取证行。
   新口径：只有「首帧」和「每 H_SUB_LOG_EVERY 帧的采样帧」落行，采样帧内整帧照旧落（保留两笔 clear
   各自的颜色与 rv，行格式一字不改，既有 grep/抽取脚本不用跟着改）；其余帧只更新下面这笔「待落记录」。
   末帧：最后一笔 clear 常驻 g_h_sub_pend，进程正常退出时由 atexit 补一行（带 f=<帧号> 前缀区分）；
   被 SIGKILL 时走不到 atexit —— 那种情况下靠 [beat] 的最后一拍定位停在哪一帧。 */
#define H_SUB_LOG_EVERY 600
static struct { unsigned long long fr,img; unsigned n; int rv,sup,logged; float rgba[4]; } g_h_sub_pend;
static void g_h_sub_tail(void){
 if(!g_h_sub_pend.logged)return;   /* 上一笔已经落过行 ⇒ 它就是「末帧」，不必重复 */
 g_h_sub_pend.logged=0;
 vk_boot_logf_try("[h] submit tail(last clear) f=%llu n=%u clear=%.3f,%.3f,%.3f,%.3f img=%#llx sup=%d rv=%d",
  g_h_sub_pend.fr,g_h_sub_pend.n,g_h_sub_pend.rgba[0],g_h_sub_pend.rgba[1],g_h_sub_pend.rgba[2],g_h_sub_pend.rgba[3],
  g_h_sub_pend.img,g_h_sub_pend.sup,g_h_sub_pend.rv);
}
static int g_h_clear_bridge(const float rgba[4],void *user){
 (void)user;unsigned long long img=0;
 int frv=nvn_vk_rt_flush();   /* M3i：先提交挂起的 RT 批次——clear 的 barrier oldLayout 依赖它先落地 */
 if(frv<0)g_h_rt_flush_err++;
 unsigned long long fr=atomic_load_explicit(&g_frame_anchor,memory_order_relaxed);   /* M4f P1：本笔所属帧 */
 int sup=0;
 nvn_vk_m38f_clear_note(fr,rgba);   /* M38F A（只读）：清屏前一刻的目标身份/尺寸/深度态 + 回读签名；关态零输出 */
 int rv=nvn_vk_clear_color_offscreen_frame(fr,rgba,&img,&sup);   /* 同帧同目标 ⇒ 不提交，返回 0 */
 if(sup)g_h_clear_supp++;
 else if(rv==0)g_h_clear_ok++;else g_h_clear_err++;
 if(g_clear_seq_n<H_CLEAR_SEQ)memcpy(g_clear_seq[g_clear_seq_n++],rgba,sizeof(float)*4);
 /* M4n P1：本笔进「待落记录」（末帧兜底用），再按采样口径决定要不要真落两行。 */
 g_h_sub_pend.fr=fr;g_h_sub_pend.img=img;g_h_sub_pend.n=g_h_sub;g_h_sub_pend.rv=rv;g_h_sub_pend.sup=sup;
 memcpy(g_h_sub_pend.rgba,rgba,sizeof(float)*4);g_h_sub_pend.logged=0;
 if(fr<=1||fr%H_SUB_LOG_EVERY==0){
  if(fr<=1){static int once;if(!once){once=1;atexit(g_h_sub_tail);}}   /* 首帧顺带登记末帧兜底 */
  vk_boot_logf("[h] submit n=%u clear=%.3f,%.3f,%.3f,%.3f img=%#llx sup=%d",g_h_sub,rgba[0],rgba[1],rgba[2],rgba[3],img,sup);
  vk_boot_logf("[h] vkQueueSubmit rv=%d",rv);
  g_h_sub_pend.logged=1;
 }
 /* M3m：draw 拆到 clear 之后单独成 CB/单独 submit（nvn_vk_draw_tri）。放在这一行之后的意义：
    clear 的 rv 一定先落盘，draw 崩了也不会吞掉 clear 的证据。1 = 本帧无 RT/布局不符（不算失败）。
    M3r：真实 draw 默认接管（回放流里的 DRAW_ELEMENTS），三角形对照路径默认关；
    需要复跑旧证据时设 SMO_TRI_DRAW=1（同一处只读一次 env）。 */
 if(rv==0){static int tri=-1;if(tri<0){const char *e=getenv("SMO_TRI_DRAW");tri=(e&&*e&&e[0]=='1')?1:0;
   vk_boot_logf("[r] tri path %s (SMO_TRI_DRAW)",tri?"on":"off");}
  if(tri){int drv=nvn_vk_draw_tri();if(drv<0)g_h_draw_err++;}}
 return rv;
}
/* ---- M3r（2026-11-03）：真实 draw 的回调族 ----
   走流仍在 nvn_replay/cmd_trace.c（nvn_cmd_exec_sink），这里只做「记录体 → nvn_vk C 接口」的搬运，
   与 nvn_rec_* 那条录制链一一对应。任何一条返回非 0 都只是「本笔跳过」，不影响后续记录。 */
static uint64_t g_h_rp_noprogram;   /* BindProgram 查不到 key 的次数（取证：说明 program→key 配对失败） */
static int g_rp_bind_program(uint64_t prog,void *u){(void)u;int rv=nvn_vk_bind_program(prog);if(rv<0)g_h_rp_noprogram++;return rv;}
static int g_rp_bind_attrib(int n,const void *st,void *u){(void)u;nvn_vk_set_vertex_attrib_state(n,st);return 0;}
static int g_rp_bind_stream(int n,const void *st,void *u){(void)u;nvn_vk_set_vertex_stream_state(n,st);return 0;}
static int g_rp_bind_vbuf(int first,int n,const void *rg,void *u){(void)u;nvn_vk_bind_vertex_buffers(first,n,rg);return 0;}
static int g_rp_bind_ubuf(int stage,int first,int n,const void *rg,void *u){(void)u;nvn_vk_bind_uniform_buffers(stage,first,n,rg);return 0;}   /* M3s：真实字节上送（替换零填充占位） */
static int g_rp_bind_sbuf(int stage,int first,int n,const void *rg,void *u){(void)u;nvn_vk_bind_storage_buffers(stage,first,n,rg);return 0;}   /* M5x：gmem 地址来源 */
/* M14B（2026-11-08）：blend / color state 回放搬运。gfx_bridge 这一层**不**做任何判定，
   原样把录制时刻的字节交给 nvn_vk.c（那边按 smo_m14b 决定「只探针」还是「探针+真启用混合」）。
   smo_m14b=0 时 nvn_vk 侧立刻返回，不存状态、不打印 ⇒ B0 与 M12A/M13A 的走流逐条相同。 */
static int g_rp_bind_blend(uint64_t gp,const void *b,unsigned n,void *u)
{(void)u;return nvn_vk_bind_blend_state(gp,b,n);}
/* M14B：录制侧第一次 bind blend state 时把开关文件落盘（这三行只有 gfx_bridge.c 被重新编译才存在）。 */
static void m14b_sink_wire_note(void)
{
    static int once;
    if (once) return;
    once = 1;
    vk_boot_logf("[m14b] sink wired: CMD_BIND_BLEND dispatch live (record-side ensure)");
    nvn_vk_m14b_ensure();
}
static int g_rp_bind_color(uint64_t gp,const void *b,unsigned n,void *u)
{(void)u;return nvn_vk_bind_color_state(gp,b,n);}
/* ---- M3y P2：BindTextures 回放 → 真实纹理 ----
   链路：handle(tid|sid<<20) → texreg_lookup(tid)（登记时刻的宿主快照）
   → 快照 key 对应的 VkImage（Initialize 时已由 vk_tex_attach 建好）
   → 首次绑定时把池内存按 NVN 布局（块线性走 deswizzle）上送一次
   → nvn_vk_bind_texture_slot(first+i)。任何一步失败都只是「该槽回 1x1 白」并计数，
   不拿别的图像冒充。切槽前先 rt_flush（清屏/RT 布局兑现）再 draw_flush（批内已录 draw 用旧槽收口），
   顺序契约同 nvn_vk.c:3653 的注释。
   ★ 快照纪律：这里绝不 deref 游戏对象（boot#1 的 SIGSEGV@0x9 就是回读游戏覆盖后的 tex->pool 造成的）。 */
static unsigned long long g_t_binds,g_t_up,g_t_skip,g_t_bytes,g_t_unres,g_t_noslot,g_t_unchg,g_t_mism;
/* M4u P1：unres 里「注册期被丢过快照」的条数（区分病根用，见 g_regdrop）。
   M4u P2：slot>=16 的绑定条数 + 高槽绑定取证条（限流 48 条）。 */
static unsigned long long g_t_unres_dropped,g_t_slot_hi;
/* M4t P2（2026-11-03）：按 id 的绑定次数 + 「上送失败原因」分布。
   为什么需要：`up=256` 是「不同的纹理张数」而不是「上送次数」，答不了「哪张常用纹理没内容」。
   这里给每个 id 记绑定次数，周期性地把「从未上送过的高频 id」top20 打出来（每 6000 submit 一次，
   最多 4 次），并把 upload_storage 的负码按原因分桶（-2 MSAA/-3 空或超限/-4 预算/-5 深度+模板/
   -6 soft_bc/-7 布局不可解/-8 deswizzle 几何/-9 malloc/-10 越界/-11 入参非法）。 */
/* M103A：`g_t_idbinds` 的声明已上移到 M5z/M7k 段（让位优先序要用），这里不再重复定义。 */
static unsigned long long g_t_upfail[16], g_t_upfail_n;
static void g_t_upfail_add(int ok)
{
    g_t_upfail_n++;
    if (ok < 0 && -ok < (int)(sizeof g_t_upfail/sizeof *g_t_upfail)) g_t_upfail[-ok]++;
    else g_t_upfail[0]++;   /* 0 = 入参非法（旧口径）；正码不该出现在失败分支 */
}
static void tex_bind_stats_dump(unsigned sub)
{
    static unsigned last; static int started;
    if (!started) { if (sub < 3000u) return; started = 1; last = sub; }
    else if (sub - last < 6000u) return;
    else last = sub;
    int top[20]; unsigned long long topn[20]; int nt = 0;
    int nnever = 0, nnever_rt = 0;
    for (int i = 0; i < TEXREG_N; i++) {
        if (!g_t_idbinds[i] || g_idup[i]) continue;
        nnever++;
        if (g_rt_id_seen[i]) nnever_rt++;
        unsigned c = g_t_idbinds[i];
        if (nt < 20) { top[nt] = i; topn[nt] = c; nt++; }
        else {
            int mi = 0;
            for (int k = 1; k < 20; k++) if (topn[k] < topn[mi]) mi = k;
            if (c > topn[mi]) { top[mi] = i; topn[mi] = c; }
        }
    }
    /* 插排（20 项）按次数降序，输出稳定可 diff */
    for (int a = 0; a < nt; a++) for (int b = a + 1; b < nt; b++)
        if (topn[b] > topn[a]) { int ti = top[a]; top[a] = top[b]; top[b] = ti;
                                 unsigned long long tn = topn[a]; topn[a] = topn[b]; topn[b] = tn; }
    vk_boot_logf("[t] tex never-up @submit#%u: distinct_never_up=%d was_rt=%d up=%llu counters below (binds = 该 id 被绑次数)",
                 sub, nnever, nnever_rt, g_t_up);
    for (int a = 0; a < nt; a++) {
        int id = top[a]; const texsnap *s = texreg_lookup(id);
        int bw = 1, bh = 1, bpb = 0;
        if (s) bpb = nvn_fmt_bpb(s->d.format, &bw, &bh);
        vk_boot_logf("[t] tex never-up id=%d binds=%llu ext=%ux%u fmt=0x%x bpp=%d mem=%d was_rt=%d",
                     id, topn[a], s ? s->d.width : 0u, s ? s->d.height : 0u,
                     s ? s->d.format : 0, bpb, (s && s->mem) ? 1 : 0, g_rt_id_seen[id] ? 1 : 0);
    }
    vk_boot_logf("[t] tex up fail: n=%llu why -2(msaa)=%llu -3(empty/big)=%llu -4(budget)=%llu -5(depth+s)=%llu "
                 "-6(soft_bc)=%llu -7(layout)=%llu -8(geom)=%llu -9(malloc)=%llu -10(range)=%llu -11(args)=%llu "
                 "-12(heat)=%llu other=%llu",
                 g_t_upfail_n, g_t_upfail[2], g_t_upfail[3], g_t_upfail[4], g_t_upfail[5], g_t_upfail[6],
                 g_t_upfail[7], g_t_upfail[8], g_t_upfail[9], g_t_upfail[10], g_t_upfail[11], g_t_upfail[12],
                 g_t_upfail[1]);
}
static unsigned long long g_t_fmt_hist[64];      /* 绑定样本的 NVN 格式直方图（低 6 位：fmt&0x3F 分桶，见报告口径） */
static unsigned long long g_t_fmt_other;
/* M5z P1：4096 → 16384。id 图改成 LRU 缓存后，被换出的 id 回来时**必须**重新上送一次，
   4096 张的笔数上限会在 LRU 跑起来之后把上送彻底掐死（图建出来但内容是全零 ⇒ 黑，比白更糟）。
   CPU 的爆炸面改由「每 submit 最多 M5Z_EVICT_PER_SUB 次换出」封顶，笔数上限只留作兜底。 */
static const unsigned k_tex_up_max = 16384;              /* 单 boot 纹理上送上限（防每帧重建把 CPU 打穿） */
static const uint64_t k_tex_up_bytes_max = 1024ull<<20;   /* M4o P2：384MB → 1GB */   /* 上送总字节预算 */
/* M4u P1b：**热度门槛**（m4u_b1 实测暴露的副作用）。
   b1 把 20392 条「注册了但没快照」的 id 修活之后，可上送的 id 从 255 涨到 384（撞 id 图上限），
   1 GB 预算被一次性纹理吃光：`up fail -4(budget)=5877`、`up=241`（反而比 m4t 的 255 少），
   而真正被场景 draw 反复采样的 id（287 binds=6387、3452=6312、3238=4406 …）全在 never-up 里。
   ⇒ 预算是稀缺资源，按「被绑次数」分配，而不是按「第一次被绑的先后」。
      · binds<2：一次性纹理，先不建图/不上送（省 384 张图的名额和预算）；
      · 预算 <50%：正常放行（保持原行为，热/冷都上）；
      · 预算 50~85%：只给 binds>=8 的；
      · 预算 >=85%：只给 binds>=32 的（实测热纹理 binds 都在 4000+，余量足够）。 */
static unsigned long long g_t_heat_gate;   /* 被热度门槛挡下的次数（不是失败：是主动让位） */
static int tex_heat_ok(int tid,const texsnap *sn){
 if(tid<0||tid>=TEXREG_N)return 0;
 /* M5z P1：门槛改看**活体**字节（g_t_bytes_live）。旧口径用累计 g_t_bytes，一旦 LRU 驱逐释放了
    图，累计账仍在（860 MB），门槛就会永远停在「只放行 binds>=32」——修了名额也没用。
    驱逐前 live==累计，所以这条改动在还没触发驱逐时行为完全不变。 */
 uint64_t used=g_t_bytes_live;
 /* M4v P3②：预算 <50% 时「首绑即上送」——binds<2 的门槛只在预算过半后才生效。
    m4u_b2 实测 29/63 条采样取证是 placeholder，且全部是该 id 的**第一次绑定**
    （`binds=1` 被挡 → 槽回 1×1 白占位，第二帧起才 up=1）。预算充足时没有任何理由让第一帧白采样。 */
 if(used<k_tex_up_bytes_max/2)return 1;
 /* M4bk P2：**小图首绑即传**（≤1 MB）—— 门槛拦的是 CPU 成本，而门槛的判据（绑定次数）对
    「每帧只被绑一次、但确实被真实 draw 采样」的纹理永远不公平：它第一帧必须是冷 id，
   于是第一帧的采样就是 1×1 白。
    实测（M4bk boot#1）：被采样的空槽 100% 是 `why=-30(create_heat)`，而它们分别是
    256×256/128×64 的 BC（32 KB/13.8 KB）和 512×512 的 R16F（512 KB）——**成本微不足道**。
    约束不变：字节照样记进同一本账 g_t_bytes（1 GB 上限仍在 -4 判），笔数仍受 k_tex_up_max=4096 约束，
    所以这是"复用配额精神"而不是"绕过预算"。 */
 if(sn&&sn->avail&&sn->avail<=(1ull<<20))return 1;
 unsigned n=g_t_idbinds[tid];
 if(n<2)return 0;
 if(used<k_tex_up_bytes_max*85/100)return n>=8;
 return n>=32;
}

/* ================= M4as P2：大图（≥1024×512）专用上送配额 =================
   缺口（M4ar P2.3）：内容类新 draw 的采样源 —— id=1284(2048×1024 fmt=0x46)/1286(0x4c)/1287(0x49)/
   1285(1024×512 0x49) —— 在首笔 draw 里 7 个槽有 6 个是 `kind=placeholder up=0 view=0 src=none`
   （连 VkImage 都没有）。归因（本报告 P1.2，旧 boot `m4aq_m4ar_bB_nominal`）：
   `[t] tex totals … up=258 skip=1611 … idimg=258 cap=0` 且 `heat_gate=1611 == skip` ⇒ **100% 是热度门槛 -12**；
   共享预算 -4=0、建图上限 cap=0、noslot=0、size -3=0。门槛用 `g_t_idbinds[tid]`，而这类图的**第一次绑定**
   必然 binds=1 ⇒ 首帧必白采样；同一批图 5 s 后（binds≥8）才 `up=1`
   （旧日志 1793747074.925 `[t] tex handle=… 2048x1024 fmt=0x46 up=1`）。
   ⇒ 给大图一条**独立配额**：不受热度门槛约束，字节记在单独一本账（因此也绕过共享 1 GB 的 -4），
     但上限刻意小（64 张 / 64 MB），避免把既有的 735 MB 预算挤爆。计 big_up / big_bytes / big_gate。
   不需要按 id 记忆「只走一次」：上送成功后 g_idup[tid]=1，后续绑定本来就不再进上送分支。
   **口径修正（M4as boot#1 自测发现的自己的 bug）**：首版把「大」定义为 `w>=1024 && h>=512`，
   实测前 200 s 就被 7 张 1600×900 未压缩 RGBA8（id=3136-3142，每张 6.55 MB，都是全屏 scratch）
   吃掉 43 MB，配额在真正的采样源（112 s 才首绑）之前就满了（`big quota up=8 bytes=50MB gate=813`）。
   现在只认**压缩族**（BC 0x41-0x50 / ASTC 0x79-0x94）—— 派活单原话就是「≥1024×512、BC 压缩」，
   这类图每张 0.35~2.8 MB，64 MB 配额够覆盖 30+ 张。 */
static const unsigned k_big_up_max = 64;
static const uint64_t k_big_up_bytes_max = 64ull<<20;
static unsigned long long g_t_big_up, g_t_big_bytes, g_t_big_gate, g_t_big_log;
static int tex_is_big(const texsnap *sn)
{
 if(!sn||!sn->avail||sn->avail>=(32ull<<20))return 0;
 if(sn->d.width<1024||sn->d.height<512)return 0;
 int f=sn->d.format;
 if((f>=0x41&&f<=0x50)||(f>=0x79&&f<=0x94))return 1;   /* BC / ASTC；未压缩大图按旧热度口径 */
 /* M30A smo_m30a_b bit0：未压缩大图（≥1024×512，含 1600×900 全屏 scratch）也走 big 独立配额。
    依据：`[t] starve heat_ids=8 heat_bytes=52,428,800` + `[m] sentinel slot=13 texid=3237
    why=-30(create_heat) ext=1600x900 up=0`。M4as 当年排除它们的原因是「7 张 6.55 MB 在真采样源
    首绑前就吃满 43 MB」；关态保持排除 ⇒ 行为逐字不变。开态字节仍记 big 账（64 张 / 64 MB 硬上界，
    超了计 big_gate），不会无界吃内存。 */
 if(m30a_sw(1)&1){
  static int said;
  if(said!=1){said=1;vk_boot_logf("[m30a] big quota now also takes uncompressed >=1024x512 (smo_m30a_b=1)");}
  return 1;
 }
 return 0;
}
static int tex_big_ok(const texsnap *sn)
{
 if(!tex_is_big(sn))return 0;
 if(g_t_big_up>=k_big_up_max||g_t_big_bytes+sn->avail>k_big_up_bytes_max){g_t_big_gate++;return 0;}
 return 1;
}
/* 上送门总判：1=放行（*big 说明是否走的独立配额）；0=让位（已计 heat_gate）。
   两个调用点（建图 / 上送）共用同一口径，避免「建了图但不上送」的半截状态口径不一。 */
static int tex_up_gate_ok(int tid, const texsnap *sn, int *big)
{
 *big=0;
 if(tex_heat_ok(tid,sn))return 1;
 if(tex_big_ok(sn)){*big=1;return 1;}
 g_t_heat_gate++;
 return 0;
}

/* ================= M4v P1：纹理布局/格式定位探针 =================
   目的：把「已上送、但采样全 0 / 噪点」这一环钉死在*行号*上。对每张被观察的纹理打印：
     ① 描述符与两条独立表给出的 bpb/bw/bh + VkFormat + soft_bc；
     ② 块线性几何（nvn_deswizzle_geom 的 base/rb/rows/slices/th/td）与级偏移；
     ③ 池源字节前 64 B（mem+base）与解排布后前 64 B；
     ④ 整块统计（非零字节占比 / 均值 / min / max）；
     ⑤ **块高候选扫描**：th=auto/4/3/2/1/0 各解一遍，报「横向 16B 段差 / 纵向行差」——
        正确块高应让相邻数据最连续；若 auto 不是最小值，说明 tileH 选错（有实测判据，不靠猜）；
     ⑥ 区间扫描：池 [mem, mem+avail) 的非零占比 + 首 8 个字 + 前后 4 KB 窗口，
        区分「offset 指错地方」与「这块内存就是空的」；
     ⑦ 定时复查（tex_probe_tick）：同一 id 在不同时刻再读一次源字节，
        区分「首绑时还没写」与「永远不写」。
   全部限流：每个 id 首次上送一行组 + 至多 6 次复查；不改变任何渲染行为。 */
/* M4aa P3：把 1131/1142（色带那笔的两张 128×128 源图）加进观察名单：
   上送/首绑时直接打池区间的 nz%（非零字节占比）——「游戏此刻没写」与「我们没上送」由此分开。 */
/* ---- M4am P2：体纹理内容探针（**决定性实验**，默认关）----
   事实链（本机实测）：① id=1900(32×32×32 lv6 R8)/1910/1911 这类 3D 体纹理在池里**恒为 0**
   （M4ai 复查 6 次全 0）；② 全 boot 的 GPU 写命令只有 clear_texture=60 + copy_t2t=5，目标全是
   1600×900 的 2D RT，b2t/copy_b2b/clear_buffer/dispatch **一次都没有**；③ slot 9 那 516 笔
   FS=05896129 用 sampler3D 采体纹理。⇒ 「体纹理零」与「场景黑」在因果上还没被连起来。
   本探针把**源区全零**的 3D 纹理在「送给 VkImage 的那份拷贝」里填成 0x80，**不动游戏池内存**
   （池仍是零，ro 侧自洽），只为回答一个问题：
     · 屏幕因此变亮/变灰 ⇒ 黑是体纹理零造成的（下一步才轮到「谁该写」）；
     · 屏幕完全不变   ⇒ 黑不是体纹理零造成的（A/B 空桩审计已排除写纹理缺桩 ⇒ 转查几何/管线）。
   开法：文件 `smo_vol_probe` = 1（构造期建 0666 松手文件、按秒重读，与 smo_gpu_write 同款）。 */
#define VOL_PROBE_MAX (4u<<20)
static void tp_region(const uint8_t *p,uint64_t n,uint64_t *nz,uint64_t *tot);   /* fwd：定义在 tex_probe 块（下方） */
static int vol_probe_on(void){
 static int cached=-2;static long last;
 const char *paths[2]={"/data/storage/el2/base/files/smo_vol_probe",
                       "/data/app/el2/100/base/com.vibecc.smo/files/smo_vol_probe"};
 long now=(long)time(NULL);
 if(cached!=-2&&now==last)return cached;
 last=now;cached=0;
 for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
  if(f){int v=0;if(fscanf(f,"%d",&v)==1&&v>0)cached=1;fclose(f);break;}}
 return cached;}
__attribute__((constructor)) static void volprobe_seed_flag(void){
 const char *paths[2]={"/data/storage/el2/base/files/smo_vol_probe",
                       "/data/app/el2/100/base/com.vibecc.smo/files/smo_vol_probe"};
 for(int i=0;i<2;i++){
  if(access(paths[i],F_OK)==0){chmod(paths[i],0666);return;}
  int fd=open(paths[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  /* M4am：boot#2 实验时种子写 1（探针默认开，因为体纹理首绑在 t≈5 s，脚本推值来不及）。
     实验结论已出（填 0x80 后 present 逐字节不变）⇒ 改回默认关；要复跑：`hdc file send` 覆盖成 1 即可。 */
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(paths[i],0666);return;}
 }}
/* 源区全零的 3D 纹理 ⇒ 返回填 0x80 的宿主副本（调用方用完 free）；否则返回原指针、*owned=0。 */
static const void *vol_probe_src(const texsnap *sn,int tid,uint64_t len,unsigned char *owned){
 *owned=0;
 if(!sn||!sn->mem||!len||len>VOL_PROBE_MAX)return sn?sn->mem:NULL;
 if(!(sn->d.target==2&&sn->d.depth>1))return sn->mem;   /* 只碰 3D 体纹理 */
 if(!vol_probe_on())return sn->mem;
 uint64_t nz=0,tot=0;tp_region((const uint8_t *)sn->mem,len,&nz,&tot);
 if(nz)return sn->mem;                                   /* 源非零：不干预，正常上送 */
 uint8_t *b=malloc(len);if(!b)return sn->mem;
 memset(b,0x80,(size_t)len);*owned=1;
 {static unsigned char said[TEXREG_N/8];
  if(tid>=0&&tid<TEXREG_N&&!(said[tid>>3]&(1u<<(tid&7)))){
   said[tid>>3]|=1u<<(tid&7);
   vk_boot_logf("[t] vol probe id=%d %ux%ux%u lv=%u fmt=%#x src_nz=0/%llu -> fill 0x80 (pool untouched)",
    tid,sn->d.width,sn->d.height,sn->d.depth,sn->d.levels,(unsigned)sn->d.format,(unsigned long long)len);}}
 return b;}
#define TP_WATCH_N 16
/* M4as P1.3：把内容类新 draw 的 4 个采样源（2048×1024 0x46/0x4c/0x49 + 1024×512 0x49）与同批
   1024×1024 三张加进观察名单：上送那一刻打池区间 nz%（源区有没有被写）+ th 候选扫描（解排布对不对）。
   旧 boot 里这四个 id 不在名单，所以「源是空的」与「没上送」当时无法分开——本轮直接补上。 */
static const int g_tp_watch[TP_WATCH_N]={287,3452,1894,293,1900,288,1131,1142,1910,1911,
                                         1284,1285,1286,1287,1288,1289};
static unsigned short g_tp_lines[TP_WATCH_N];   /* 每个 id 已打印的行数（限流） */
static unsigned short g_tp_re[TP_WATCH_N];      /* 每个 id 已复查次数 */
static unsigned char g_tp_seen[TP_WATCH_N];     /* 首绑已取过证（不管上送成功与否） */
static int tex_watch_idx(int id){
 for(int i=0;i<TP_WATCH_N;i++)if(g_tp_watch[i]==id)return i;
 return -1;
}
/* ---- M7l P1（只读取证）：动态观察名单 + 上传点源字节非零统计 ----
   事实（M7k boot#2）：场景族 draw（vs=24702883）首现 t≈210s，它采的 id2249 在 t≈213s 才被
   准入上传（`[t] tex handle=0x236008c9 256x256 fmt=0x48 bytes=89600 up=1`），而旧观察名单
   （M4as 16 个静态 id）与 [t] up sig 的 24 行预算都被 boot 前段烧完 ⇒ 「绑上了但内容是零」
   的零到底在源头还是在解码，对场景族一条证据都没有。本轮补三件套（全只读）：
     ① [m7l] up：上传成功那一刻对**源字节**（sn->mem，avail 全长）数非零占比 + 全零 4K 页数。
        门 M7L_ON 且 nvn_vk_scene_seen()（场景族 draw 出现过之后才准入的 id 才是本轮对象）。
        预算 24 行/boot：≤1MB 占 16 行（M7k 小图直通准入的正是这一类）、>1MB 占 8 行；
        >4MB 只扫前 4MB（行尾标 scan=4M）。nz_src≈0 ⇒ 源头就没数据（下一跳「谁该写」）；
        nz_src>0 而 zero_content 仍成立 ⇒ 解码/通道问题（对照 [m7l] sig 的解排布后签名）。
     ② 动态观察名单：场景族出现后**首次上传**的前 8 个 id（≤4MB）自动入名单，走全套 [tp1]
        （±4K 窗口/块线性几何/解排布对照统计）+ tex_probe_tick 复查 + 前 3 个 id 走 ±64KB
        存储基址判决扫描（tex_scan_near 同款，回答「基址算错还是真没写」）。
     ③ nvn_vk 侧 [m7l] sig：同批上传的解排布后 level0 内容签名（w0/w1/diff），独立 24 行预算。
   回退：M7L_ON=0 ⇒ 三件套全静默，行为与 M7k 逐位相同（不改任何渲染行为）。 */
#define M7L_DYN_N 8
static int g_m7l_dyn[M7L_DYN_N];                /* 动态名单（场景族出现后首次上传的 id） */
static unsigned g_m7l_dyn_n;
static unsigned short g_m7l_dyn_lines[M7L_DYN_N];   /* [tp1] 行数限流（对齐 g_tp_lines 口径） */
static unsigned short g_m7l_dyn_re[M7L_DYN_N];      /* 复查次数（对齐 g_tp_re 口径，≤6） */
static unsigned long long g_m7l_up_lines, g_m7l_up_small, g_m7l_up_big;
/* M7l P2（boot#2）：零内容观察名单——绑定探针见到的前 6 个零内容 id，进 4096 绑定节拍的
   复查序列（[m7l] zw）。smo_gpu_write=1 时 GPU 写命令会真正落池：若某次写落进这些 id 的
   源区，这里的 nz 序列会从 0% 抬起来——「写命令到底写没写它们」的直接证据。 */
static int g_m7l_zw[6];
static unsigned g_m7l_zw_n;
static unsigned short g_m7l_zw_re[6];
static int m7l_dyn_idx(int id){
 for(unsigned i=0;i<g_m7l_dyn_n;i++)if(g_m7l_dyn[i]==id)return (int)i;
 return -1;
}
/* 场景族出现后的首次上传：入动态名单（≤4MB 限成本，前 8 个）。返回 1 = 本 id 新入名单。 */
static int m7l_dyn_enroll(int tid,const texsnap *sn){
 if(!M7L_ON||!nvn_vk_scene_seen()||tid<0||tid>=TEXREG_N)return 0;
 if(m7l_dyn_idx(tid)>=0)return 0;
 if(g_m7l_dyn_n>=M7L_DYN_N||!sn||!sn->mem||!sn->avail||sn->avail>(4ull<<20))return 0;
 g_m7l_dyn[g_m7l_dyn_n++]=tid;
 vk_boot_logf("[m7l] dyn watch id=%d slot=%u ext=%ux%u fmt=0x%x avail=%llu (post-scene first upload)",
  tid,g_m7l_dyn_n-1,sn->d.width,sn->d.height,(int)sn->d.format,(unsigned long long)sn->avail);
 return 1;
}
static void m7l_up_probe(int tid,const texsnap *sn){
 if(!M7L_ON||!nvn_vk_scene_seen()||!sn||!sn->mem||!sn->avail)return;
 int big=sn->avail>(1ull<<20);
 if(big?g_m7l_up_big>=8:g_m7l_up_small>=16)return;
 if(g_m7l_up_lines>=24)return;
 uint64_t scan=sn->avail,nz=0,zp=0;
 if(scan>(4ull<<20))scan=4ull<<20;
 const uint8_t *m=(const uint8_t *)sn->mem;
 for(uint64_t off=0;off<scan;off+=4096){
  uint64_t n=scan-off<4096?scan-off:4096;
  unsigned pnz=0;
  for(uint64_t k=0;k<n;k++)if(m[off+k])pnz++;
  if(!pnz)zp++;
  nz+=pnz;
 }
 if(big)g_m7l_up_big++;else g_m7l_up_small++;
 g_m7l_up_lines++;
 uint32_t w[4]={0,0,0,0};
 if(sn->avail>=16)memcpy(w,m,16);
 vk_boot_logf("[m7l] up id=%d fmt=0x%x %ux%u bytes=%llu nz_src=%u%% zero_pages=%llu w0=%08x,%08x,%08x,%08x%s",
  tid,(int)sn->d.format,sn->d.width,sn->d.height,(unsigned long long)sn->avail,
  (unsigned)(scan?100*nz/scan:0),(unsigned long long)zp,
  w[0],w[1],w[2],w[3],scan!=sn->avail?" scan=4M":"");
}
static void tp_hex64(const char *tag,int id,const char *what,const uint8_t *p,unsigned n){
 char b[3*64+4];unsigned m=n>64?64u:n;
 /* 长度必须是 3：snprintf 每次只写 "xx\0"，下一次覆盖掉上一个 NUL，最后一个 NUL 正好落在 b[3m]。
    写成 4 会让 %s 在 b[2]（第一个 \0）就停 ⇒ m4v_b2 的 src64/out64 只打印出 1 个字节前缀。 */
 for(unsigned i=0;i<m;i++)snprintf(b+i*3,3,"%02x",p[i]);
 b[m*3]=0;
 vk_boot_logf("[tp1] id=%d %s %s64=%s",id,tag,what,b);
}
/* 指定块高的解排布（与 nvn_deswizzle 的 move() 同一寻址，只是 th/td 由调用方给） */
static void tp_deblin(const uint8_t *bl,uint8_t *lin,uint32_t rb,uint32_t rows,uint32_t slices,unsigned th,unsigned td){
 for(uint32_t z=0;z<slices;z++)for(uint32_t y=0;y<rows;y++){
  uint8_t *row=lin+((size_t)z*rows+y)*rb;
  for(uint32_t x=0;x<rb;x+=16){uint32_t n=rb-x<16?rb-x:16;
   memcpy(row+x,bl+nvn_bl_offset(x,y,z,rb,rows,th,td),n);}}
}
typedef struct{uint64_t nz,total;uint64_t sum;unsigned mn,mx;unsigned long long gx,gy;} tp_stat;
/* 块线性在一级一层上的**实际占用**（= 行数补齐到 8·2^th、切片补齐到 2^td 之后的字节数）。
   与 nvn_bl_offset 的公式同源；用来给解排布的源缓冲定量，不能拿 rb*rows 当上界。 */
static uint64_t tp_bl_size(uint32_t rb,uint32_t rows,uint32_t slices,unsigned th,unsigned td){
 uint32_t bh=1u<<th,bd=1u<<td;
 uint64_t gx=(rb+63)/64;
 uint64_t by=(rows+(uint64_t)8*bh-1)/((uint64_t)8*bh);
 uint64_t bz=(slices+(uint64_t)bd-1)/(uint64_t)bd;
 return gx*by*bz*512ull*bh*bd;
}
static void tp_stats(const uint8_t *lin,uint32_t rb,uint32_t rows,uint32_t slices,tp_stat *s){
 memset(s,0,sizeof *s);s->mn=255;
 size_t total=(size_t)rb*rows*slices;s->total=total;
 for(size_t i=0;i<total;i++){uint8_t v=lin[i];if(v)s->nz++;s->sum+=v;if(v<s->mn)s->mn=v;if(v>s->mx)s->mx=v;}
 for(uint32_t z=0;z<slices;z++)for(uint32_t y=0;y<rows;y++){
  const uint8_t *row=lin+((size_t)z*rows+y)*rb;
  for(uint32_t x=16;x<rb;x+=16)for(uint32_t k=0;k<16;k++){int d=(int)row[x+k]-(int)row[x-16+k];s->gx+=(unsigned long long)(d<0?-d:d);}
  /* 上一行必须用指针减（不能写 row[x-rb]：x-rb 是 unsigned，会回绕成 +4GB —— m4v_b1 的 SIGSEGV 就是它） */
  if(y){const uint8_t *prev=row-rb;
   for(uint32_t x=0;x<rb;x++){int d=(int)row[x]-(int)prev[x];s->gy+=(unsigned long long)(d<0?-d:d);}}
 }
}
/* 池区间非零统计（只统计，不 deref 任何对象） */
static void tp_region(const uint8_t *p,uint64_t n,uint64_t *nz,uint64_t *tot){
 uint64_t z=0;for(uint64_t i=0;i<n;i++)if(p[i])z++;*nz=z;*tot=n;
}
static void tex_probe(int tid,const texsnap *sn,const char *tag){
 int wi=tex_watch_idx(tid);
 if(wi<0){int di=m7l_dyn_idx(tid);if(di>=0)wi=1000000+di;}   /* M7l P1：动态名单走同一套取证，行数记账分开 */
 if(wi<0||!sn||!sn->mem)return;
 const nvn_texdesc *d=&sn->d;
 nvn_texlayout L;nvn_tex_layout(d,&L);
 int bw=1,bh=1,bpb=nvn_fmt_bpb(d->format,&bw,&bh);
 unsigned vf=nvn_vk_tex_vkfmt(g_idvk[tid]);   /* 实际建图用的 VkFormat（soft_bc 时是回退后的未压缩格式） */
 vk_boot_logf("[tp1] id=%d %s %ux%ux%u lv=%u tgt=%d fl=%#x smp=%d fmt=%#x pkgBH=%d bpb=%d bw=%d bh=%d vkfmt=%#x soft=%d layers=%u avail=%llu mem=%p",
  tid,tag,d->width,d->height,d->depth,d->levels,d->target,(unsigned)d->flags,d->samples,
  (unsigned)d->format,(int)d->pkg_block_h,bpb,bw,bh,(unsigned)vf,
  g_idvk[tid]?nvn_vk_tex_soft_bc(g_idvk[tid]):-1,L.layers,(unsigned long long)sn->avail,(void*)sn->mem);
 /* ⑥ 池区间：总量 / 非零 / 首 8 字 / 前后窗口（offset 错位的直接判据） */
 {const uint8_t *m=(const uint8_t *)sn->mem;uint64_t nz=0,tot=0;
  tp_region(m,sn->avail,&nz,&tot);
  uint64_t b_nz=0,b_t=0,a_nz=0,a_t=0;
  /* 前后 4 KB 窗口先探可读（process_vm_readv），避免越出映射把进程打死 */
  if((uintptr_t)m>4096){
#ifdef __MUSL__
   uint32_t t1=0,t2=0;
   if(rt_peek_u32((uintptr_t)m-4096,&t1)&&rt_peek_u32((uintptr_t)m-4,&t2))tp_region(m-4096,4096,&b_nz,&b_t);
#else
   tp_region(m-4096,4096,&b_nz,&b_t);
#endif
  }
  {uint64_t wn=sn->avail<4096?sn->avail:4096;
#ifdef __MUSL__
   uint32_t t3=0,t4=0;
   if(wn&&rt_peek_u32((uintptr_t)m+sn->avail-4,&t3)&&rt_peek_u32((uintptr_t)m+sn->avail+wn-4,&t4))
    tp_region(m+sn->avail,wn,&a_nz,&a_t);
#else
   if(wn)tp_region(m+sn->avail,wn,&a_nz,&a_t);
#endif
  }
  const uint32_t *w=(const uint32_t *)m;uint32_t ww[4]={0,0,0,0};
  if(sn->avail>=16)memcpy(ww,w,16);   /* 小纹理（avail<16）不越界读 */
  vk_boot_logf("[tp1] id=%d %s region nz=%llu/%llu(%u%%) w0=%08x,%08x,%08x,%08x before4k_nz=%llu/%llu after4k_nz=%llu/%llu",
   tid,tag,(unsigned long long)nz,(unsigned long long)tot,
   (unsigned)(tot?100*nz/tot:0),ww[0],ww[1],ww[2],ww[3],
   (unsigned long long)b_nz,(unsigned long long)b_t,(unsigned long long)a_nz,(unsigned long long)a_t);}
 if(nvn_tex_is_linear(d)){   /* 线性（pitch）存储：布局问题不在这里 */
  vk_boot_logf("[tp1] id=%d %s branch=linear stride=%u (非块线性，跳过解排布对照)",tid,tag,nvn_tex_linear_stride(d));
  return;}
 uint32_t rb=0,rows=0,slices=0,b2=0;uint64_t base=0;unsigned th=0,td=0;
 if(nvn_deswizzle_geom(d,0,0,&rb,&rows,&slices,&base,&th,&td,&b2)){
  vk_boot_logf("[tp1] id=%d %s branch=geom_fail (MS/BUFFER/越级/未解格式)",tid,tag);return;}
 vk_boot_logf("[tp1] id=%d %s branch=blocklinear rb=%u rows=%u slices=%u base=%llu th=%u td=%u lvl_bytes=%llu layer_size=%llu lv0_off=%llu lv1_off=%llu",
  tid,tag,rb,rows,slices,(unsigned long long)base,th,td,
  (unsigned long long)((uint64_t)rb*rows*slices),(unsigned long long)L.layer_size,
  (unsigned long long)nvn_tex_level_offset(d,0),(unsigned long long)nvn_tex_level_offset(d,d->levels>1?1:0));
 if(base>=sn->avail){vk_boot_logf("[tp1] id=%d %s OOB base=%llu >= avail=%llu",tid,tag,(unsigned long long)base,(unsigned long long)sn->avail);return;}
 uint64_t need=(uint64_t)rb*rows*slices;
 uint64_t avail_src=sn->avail-base;
 /* 块线性的**实际占用**比 need 大：块高把行数向上补到 8·2^th 的整数倍（1600×900 的 th=4 ⇒
    900 行补到 1024 行），所以解排布的源必须按「补齐后」的尺寸给，否则读越过 need
    （m4v_b1 的第二个坑：ASan 离线复现里 READ of 16 越过 +7168 B）。这里按所有候选块高取最大。 */
 uint64_t pad=0;
 {unsigned c[5];unsigned ncc=0;c[ncc++]=th;static const unsigned alt0[4]={4,3,2,1};
  for(unsigned i=0;i<4;i++)if(alt0[i]!=th)c[ncc++]=alt0[i];
  for(unsigned i=0;i<ncc;i++){uint64_t z=tp_bl_size(rb,rows,slices,c[i],td);if(z>pad)pad=z;}}
 if(need>avail_src||pad>avail_src)
  vk_boot_logf("[tp1] id=%d %s TRUNC need=%llu pad=%llu base=%llu avail=%llu (源尾零填充)",
   tid,tag,(unsigned long long)need,(unsigned long long)pad,(unsigned long long)base,(unsigned long long)sn->avail);
 if(avail_src<16)return;
 { /* 先把本级源字节拷到自备缓冲（超长尾补 0）：解排布只在这个缓冲上做，
      既不会读到池映射之外，也让 64B 取证与解排布看到的是同一份字节。 */
  uint64_t sbuf_sz=pad>need?pad:need;
  uint8_t *srcbuf=calloc(1,(size_t)sbuf_sz);
  if(!srcbuf){vk_boot_logf("[tp1] id=%d %s malloc %llu failed",tid,tag,(unsigned long long)sbuf_sz);return;}
  memcpy(srcbuf,(const uint8_t *)sn->mem+base,(size_t)(sbuf_sz<avail_src?sbuf_sz:avail_src));
  const uint8_t *src=srcbuf;
  tp_hex64(tag,tid,"src",src,(unsigned)(sbuf_sz<64?sbuf_sz:64));
  uint8_t *lin=malloc((size_t)need);
  if(!lin){free(srcbuf);vk_boot_logf("[tp1] id=%d %s malloc %llu failed",tid,tag,(unsigned long long)need);return;}
  /* ⑤ 块高候选（auto 在最前）：判据 = 横向 16B 段差 gx 与纵向行差 gy，越小说明结构越连续 */
  unsigned cand[5];unsigned nc=0;cand[nc++]=th;
  static const unsigned alt[4]={4,3,2,1};
  for(unsigned i=0;i<4;i++)if(alt[i]!=th)cand[nc++]=alt[i];
  for(unsigned i=0;i<nc;i++){
   tp_deblin(src,lin,rb,rows,slices,cand[i],td);
   tp_stat s;tp_stats(lin,rb,rows,slices,&s);
   vk_boot_logf("[tp1] id=%d %s th=%u%s nz=%llu/%llu mean=%llu min=%u max=%u gx=%llu gy=%llu",
    tid,tag,cand[i],cand[i]==th?"(auto)":"",(unsigned long long)s.nz,(unsigned long long)s.total,
    (unsigned long long)(s.total?s.sum/s.total:0),s.mn,s.mx,
    (unsigned long long)(s.total?s.gx/s.total:0),(unsigned long long)(s.total?s.gy/s.total:0));
   if(cand[i]==th){   /* auto 分支：留 src/out 前 64 B 与逐行首 16B 片段（人眼看「是不是重复块」） */
    tp_hex64(tag,tid,"out",lin,(unsigned)(need<64?need:64));
    if(rb>=16)for(unsigned y=0;y<4&&y<rows;y++){
     tp_hex64(tag,tid,(y&1)?"row_odd":"row",lin+(size_t)y*rb,16);}
    /* 层 0 与层 1 的起点各取 16 B：层偏移算错时两张图会一样/差一位 */
    if(slices>1&&L.layers>1)vk_boot_logf("[tp1] id=%d %s slice1 hint: rows=%u slices=%u (3D 切片在解排布里连续)",tid,tag,rows,slices);
   }
  }
  free(lin);free(srcbuf);}
 if(wi>=1000000){int di=wi-1000000;if(++g_m7l_dyn_lines[di]>40)g_m7l_dyn_lines[di]=40;return;}
 if(++g_tp_lines[wi]>40)g_tp_lines[wi]=40;
}
/* ⑦ 定时复查：每 4096 次绑定调一次；每个 id 至多 6 次。只读源字节，不做解排布。 */
static void tex_probe_tick(unsigned long long binds){
 if((binds&4095)!=0)return;
 for(int i=0;i<TP_WATCH_N;i++){
  if(g_tp_re[i]>=6)continue;
  const texsnap *sn=texreg_lookup(g_tp_watch[i]);
  if(!sn||!sn->mem||!sn->avail)continue;
  const uint8_t *m=(const uint8_t *)sn->mem;uint64_t nz=0,tot=0;
  tp_region(m,sn->avail,&nz,&tot);
  uint32_t ww[4]={0,0,0,0};
  if(sn->avail>=16)memcpy(ww,m,16);
  g_tp_re[i]++;
  vk_boot_logf("[tp1] id=%d re%d@binds=%llu nz=%llu/%llu(%u%%) w0=%08x,%08x,%08x,%08x up=%d",
   g_tp_watch[i],(int)g_tp_re[i],binds,(unsigned long long)nz,(unsigned long long)tot,
   (unsigned)(tot?100*nz/tot:0),ww[0],ww[1],ww[2],ww[3],(int)g_idup[g_tp_watch[i]]);
 }
 /* M7l P1：动态名单同款复查（≤6 次/id，同一 4096 绑定节拍）——回答「上传时是零，
    之后源被写了没有」（写了而 reup 没跟上 ⇒ 指纹窗口问题；一直零 ⇒ 上游没写）。 */
 for(unsigned i=0;i<g_m7l_dyn_n;i++){
  if(g_m7l_dyn_re[i]>=6)continue;
  const texsnap *sn=texreg_lookup(g_m7l_dyn[i]);
  if(!sn||!sn->mem||!sn->avail)continue;
  const uint8_t *m=(const uint8_t *)sn->mem;uint64_t nz=0,tot=0;
  tp_region(m,sn->avail,&nz,&tot);
  uint32_t ww[4]={0,0,0,0};
  if(sn->avail>=16)memcpy(ww,m,16);
  g_m7l_dyn_re[i]++;
  vk_boot_logf("[m7l] re id=%d re%d@binds=%llu nz=%llu/%llu(%u%%) w0=%08x,%08x,%08x,%08x up=%d",
   g_m7l_dyn[i],(int)g_m7l_dyn_re[i],binds,(unsigned long long)nz,(unsigned long long)tot,
   (unsigned)(tot?100*nz/tot:0),ww[0],ww[1],ww[2],ww[3],(int)g_idup[g_m7l_dyn[i]]);
 }
 /* M7l P2（boot#2）：零内容名单复查（≤6 次/id）——smo_gpu_write=1 后写命令若落进这些
    源区，nz 会从 0% 抬起；一直 0% ⇒ 执行了的写命令也没碰它们。 */
 for(unsigned i=0;i<g_m7l_zw_n;i++){
  if(g_m7l_zw_re[i]>=6)continue;
  const texsnap *sn=texreg_lookup(g_m7l_zw[i]);
  if(!sn||!sn->mem||!sn->avail)continue;
  const uint8_t *m=(const uint8_t *)sn->mem;uint64_t nz=0,tot=0;
  tp_region(m,sn->avail,&nz,&tot);
  uint32_t ww[4]={0,0,0,0};
  if(sn->avail>=16)memcpy(ww,m,16);
  g_m7l_zw_re[i]++;
  vk_boot_logf("[m7l] zw id=%d re%d@binds=%llu nz=%llu/%llu(%u%%) w0=%08x,%08x,%08x,%08x up=%d",
   g_m7l_zw[i],(int)g_m7l_zw_re[i],binds,(unsigned long long)nz,(unsigned long long)tot,
   (unsigned)(tot?100*nz/tot:0),ww[0],ww[1],ww[2],ww[3],(int)g_idup[g_m7l_zw[i]]);
 }
}
/* ================= M4ai P1：存储基址判决扫描 =================
   判决题：id=1900（32×32×32 3D 体纹理）上送时源内存全零，而**紧邻其前的 4 KB 有数据**
   （`[tp1] … before4k_nz=721/4096 after4k_nz=0/4096`）。到底是
     ①**基址算错**（块线性 base / td·th 偏移指偏了，数据在附近）——改映射即可；
     ②**这块存储真没被写过**（数据在别处，或根本没人生成）。
   做法：以 sn->mem 为 0，按 4 KB 步长扫 ±64 KB，每块报 nz/4096 与首个非零 16 B。
   限流：每 id 至多 2 次（stage0=首次绑定 / stage1=全局绑定序号再走 ≥200000）；每次 1 行 nzmap +
   至多 33 行块细节（只打非零/不可读块）+ 1 行汇总。全部走 process_vm_readv（__MUSL__），
   未映射页返 EFAULT 而不是 fault。**只读，零渲染行为改动。** */
#define SCAN_IDS 4
static const int g_scan_id[SCAN_IDS] = {1900, 1889, 1016, 282};
static unsigned char g_scan_stage[SCAN_IDS];   /* 0=未扫 1=early 已扫 2=late 已扫 */
/* M4ai boot#1 实测：这 4 个 id 的**单 id 绑定次数**并不随 516 笔/帧增长（材质槽只绑几次），
   用 binds>=64 当 stage0 门槛会让 1900/1889/1016 一次都扫不到（只有 282 扫到了）。
   改成：stage0 = 首次见到就扫；stage1 = **全局绑定序号**再走 ≥200000 之后（会话后段）。
   ⇒ 与单 id 热度解耦，4 个 id 都保证拿到 early/late 两个时间点。 */
static unsigned long long g_scan_clock;        /* 全局绑定序号（stage1 的时钟） */
static unsigned long long g_scan_t0[SCAN_IDS]; /* 该 id 首次扫描时的 clock */
static int rt_read_range(uint64_t a, void *out, size_t n)
{
 if (a < 0x100000000ULL || a >= 0x800000000000ULL) return 0;
 if (n == 0 || a + n < a || a + n > 0x800000000000ULL) return 0;
#ifdef __MUSL__
 struct iovec l = { out, n }, r = { (void *)(uintptr_t)a, n };
 return syscall(__NR_process_vm_readv, getpid(), &l, 1ul, &r, 1ul, 0ul) == (long)n;
#else
 memcpy(out, (const void *)(uintptr_t)a, n); return 1;
#endif
}
static void tex_scan_run(int id, const texsnap *sn, unsigned char st,
                         unsigned char *stp, unsigned long long *t0p)
{
 if (st == 0) *t0p = g_scan_clock;
 *stp = (unsigned char)(st + 1);
 const unsigned char *base = (const unsigned char *)sn->mem;
 char map[768];
 int mo = snprintf(map, sizeof map, "[scan] id=%d stage=%d ext=%ux%ux%u tgt=%d fmt=0x%x avail=%llu mem=%p nzmap(KB=nz)v2",
                   id, (int)(st + 1), sn->d.width, sn->d.height, sn->d.depth, sn->d.target,
                   (unsigned)sn->d.format, (unsigned long long)sn->avail, (void *)sn->mem);
 uint64_t base_nz = 0, best_nz = 0, best_adj_nz = 0;
 long long best_off = 0, best_adj_off = 0;
 int nbad = 0, ndet = 0, nfar = 0;
 for (int b = -16; b <= 16; b++) {
  long long off = (long long)b * 4096;
  uint8_t blk[4096];
  uint64_t nz; int first = -1;
  if (!rt_read_range((uint64_t)(uintptr_t)(base + off), blk, sizeof blk)) { nz = (uint64_t)-1; nbad++; }
  else {
   nz = 0;
   for (int i = 0; i < 4096; i++) if (blk[i]) { nz++; if (first < 0) first = i; }
  }
  if (b == 0) base_nz = nz;
  /* 判据分层：|b|<=1（紧邻的前后各 4 KB）通常是**同一池里的相邻分配**，不算「数据在我们附近」；
     真正的「基址算错」会表现为**远离 base 的连续非零块**（整张图整体位移）。 */
  if (nz != (uint64_t)-1 && nz > 0) {
   if (b >= -1 && b <= 1) { if (nz > best_adj_nz) { best_adj_nz = nz; best_adj_off = off; } }
   else { nfar++; if (nz > best_nz) { best_nz = nz; best_off = off; } }
  }
  if (mo < (int)sizeof map - 24)
   mo += snprintf(map + mo, sizeof map - (size_t)mo, " %+lld=%s%llu",
                  off >> 10, nz == (uint64_t)-1 ? "X" : "", nz == (uint64_t)-1 ? 0ull : (unsigned long long)nz);
  if ((nz != 0) && ndet < 33) {
   ndet++;
   if (nz == (uint64_t)-1) vk_boot_logf("[scan] id=%d off=%+lld unreadable (process_vm_readv EFAULT)", id, off);
   else {
    char hx[3 * 16 + 1];
    if (first > 4096 - 16) first = 4096 - 16;
    for (int k = 0; k < 16; k++) snprintf(hx + k * 3, 3, "%02x", blk[first + k]);
    hx[48] = 0;
    vk_boot_logf("[scan] id=%d off=%+lld nz=%llu/4096 first16=@%d %s",
                 id, off, (unsigned long long)nz, first, hx);
   }
  }
 }
 vk_boot_logf("%s", map);
 const char *verdict = (nfar > 0) ? "base_wrong"
                     : (base_nz > 0 ? "base_ok" : (nbad == 33 ? "unreadable" : "unwritten"));
 vk_boot_logf("[scan] id=%d mem=%p base_nz=%llu best_off=%+lld best_nz=%llu far_blocks=%d "
              "adj_off=%+lld adj_nz=%llu unreadable=%d verdict=%s",
              id, (void *)sn->mem, (unsigned long long)(base_nz == (uint64_t)-1 ? 0 : base_nz),
              best_off, (unsigned long long)best_nz, nfar,
              best_adj_off, (unsigned long long)best_adj_nz, nbad, verdict);
}
static unsigned char g_m7l_scan_st[3];          /* M7l P1：动态名单前 3 个 id 的扫描阶段 */
static unsigned long long g_m7l_scan_t0[3];
static void tex_scan_near(int id, const texsnap *sn, unsigned long long binds)
{
 int si = -1;
 for (int i = 0; i < SCAN_IDS; i++) if (g_scan_id[i] == id) si = i;
 if (si >= 0) {
  if (!sn || !sn->mem) return;
  unsigned char st = g_scan_stage[si];
  if (st == 0) { if (binds < 1) return; }
  else if (st == 1) { if (g_scan_clock - g_scan_t0[si] < 200000ull) return; }
  else return;
  tex_scan_run(id, sn, st, &g_scan_stage[si], &g_scan_t0[si]);
  return;
 }
 /* M7l P1：动态名单前 3 个 id 走同款 ±64KB 基址判决扫描（nz_src≈0 时回答
    「数据就在附近=基址算错」还是「整段都没写=上游没给」）。阶段规则与静态表一致。 */
 if (M7L_ON && nvn_vk_scene_seen()) {
  int di = m7l_dyn_idx(id);
  if (di >= 0 && di < 3 && sn && sn->mem) {
   unsigned char st = g_m7l_scan_st[di];
   if (st == 0) { if (binds < 1) return; }
   else if (st == 1) { if (g_scan_clock - g_m7l_scan_t0[di] < 200000ull) return; }
   else return;
   tex_scan_run(id, sn, st, &g_m7l_scan_st[di], &g_m7l_scan_t0[di]);
  }
 }
}

/* ================= M4ai P2：被吃掉的 GPU 写 opcode 台账 =================
   M4ai P2b 的事实：`clear_texture / copy_buffer_to_texture / copy_texture_to_texture /
   clear_buffer / dispatch_compute / downsample / bind_image` 在我们这边全是 **CB_REC 平坦记账桩**
   ——只推 24 B 命令指针、**从不进命令流、从不执行**（gfx_bridge.c:2427 一带）。
   于是「谁该写这张 3D 体纹理」在 `[h] opcode` 直方图里**根本不存在**（那份表里只有 15 种 opcode，
   没有一条 copy/clear-texture/dispatch）⇒ 属于「零证据」盲区，不能当「没人写」。
   这里给每个这类 opcode 加计数（按名报），并把 copy/clear-texture 的**目标纹理**解码成 id + 尺寸 +
   当前存储 nz（每 id 一行），直接回答「游戏到底有没有让我们执行过写这张图的命令」。 */
enum { GPUOP_CLEAR_TEX = 0, GPUOP_CLEAR_TEXI, GPUOP_CLEAR_TEXUI, GPUOP_COPY_B2T, GPUOP_COPY_T2T,
       GPUOP_COPY_T2B, GPUOP_COPY_B2B, GPUOP_CLEAR_BUF, GPUOP_DISPATCH, GPUOP_DISPATCH_IND,
       GPUOP_DOWNSAMPLE, GPUOP_BIND_IMAGE, GPUOP_N };
static unsigned long long g_gpuop_n[GPUOP_N];
static const char *const g_gpuop_name[GPUOP_N] = {
 "clear_texture", "clear_texturei", "clear_textureui", "copy_buffer_to_texture",
 "copy_texture_to_texture", "copy_texture_to_buffer", "copy_buffer_to_buffer", "clear_buffer",
 "dispatch_compute", "dispatch_compute_indirect", "downsample", "bind_image"};
/* M4aj P3：clear_texture / copy_texture_to_texture 的**真执行**入口（定义在 texels_move 之后）。
   本宿主纹理数据即池内存，因此执行 = 按 GOB 寻址直接改/搬池字节（texels_move 同一套排布数学）。
   护栏：只支持可解析目标 + 已知格式；不支持的计数后不写（宁缺勿错，绝不猜排布）。 */
static int gpuop_clear_exec(const void *tex,const void *view,const void *region,const float *rgba,const uint32_t *rgba_u,int is_uint);
static int gpuop_t2t_exec(const void *stex,const void *sview,const void *sregion,const void *dtex,const void *dview,const void *dregion);
static const texsnap *gpuop_resolve(const void *tex);
static unsigned long long g_gpuop_exec,g_gpuop_skip_unres,g_gpuop_skip_fmt,g_gpuop_skip_geom;
static unsigned long long g_gpuop_gated,g_gpuop_skip_unmapped;
/* M4aj P3 安全闸（boot#2 崩溃后加）：**默认关**。
   boot#2 实测：第 3 笔 clear 解析出 fmt=0x25 的真目标后执行 CPU 写入，2 ms 内 SIGSEGV
   （`cppcrash…20261104034052250.log`，`SEGV_MAPERR @0x7f84afd000`，4 帧全在 libgame.so）。
   根因方向：快照里的 `mem/avail` 在「登记之后池被释放/重映射」时会过期，按它写就是写野指针。
   因此执行必须**显式开启**（文件 `smo_gpu_write` = 1）+ 写前做跨度映射校验。 */
static int gpuop_write_enabled(void){
 /* M4ak P2（all_flat 分支）：文件必须由**应用**先建出来 —— hdc shell 是 uid 2000，
    在应用数据目录里 touch 会被拒（实测 Permission denied），只能覆盖已存在的 666 文件。
    所以：①构造期建文件（内容 0，chmod 0666）；②读侧按秒重读，脚本在文件出现后立刻推 1 即生效。 */
 static int cached=-2; static long last;
 {const char *paths[2]={"/data/storage/el2/base/files/smo_gpu_write",
                        "/data/app/el2/100/base/com.vibecc.smo/files/smo_gpu_write"};
  long now=(long)time(NULL);
  if(cached!=-2&&now==last)return cached;
  last=now;cached=0;
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){int v=0;if(fscanf(f,"%d",&v)==1&&v>0)cached=1;fclose(f);break;}}}
 {static int said;if(said!=cached+2){said=cached+2;vk_boot_logf("[gpu] write gate -> %d (smo_gpu_write)",cached);}}
 return cached;}
/* 构造期建松手文件：只在缺文件时创建，绝不清掉脚本已经写进去的值。 */
__attribute__((constructor)) static void gpuop_seed_write_flag(void){
 const char *paths[2]={"/data/storage/el2/base/files/smo_gpu_write",
                       "/data/app/el2/100/base/com.vibecc.smo/files/smo_gpu_write"};
 for(int i=0;i<2;i++){
  if(access(paths[i],F_OK)==0){chmod(paths[i],0666);return;}
  int fd=open(paths[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(paths[i],0666);return;}
 }}
/* 写前跨度校验（抽样 ≤64 个点 + 首尾）：读不通就说明该池区间已不可访问 ⇒ 不写。
   为什么读能代表写：本宿主池要么是游戏自备 RW，要么是我们 map 的匿名 RW；只读映射不会被当池。 */
static int gpuop_span_ok(const void *p,uint64_t n){
 if(!p||!n)return 0;
#ifdef __MUSL__
 uint8_t b[1];uint64_t step=n>64?(n/64):n;
 for(uint64_t o=0;o<n;o+=step)if(!rt_read_range((uintptr_t)p+o,b,1))return 0;
 return rt_read_range((uintptr_t)p+n-1,b,1);
#else
 (void)n;return 1;
#endif
}
/* M34A：hit_tgt 的三个口径（关态只累 hit_old3 ⇒ `[h] gpu write ops` 行与 M33A 逐字同形）
   · hit_tgt  = 写命令目标存储区间 ∩ **动态族表**（-30 create_heat 的池地址）—— 修盲区后的主判据
   · hit_old3 = 旧口径 `{1900,1910,1911}`（M33A 已证对族结构性恒 0；保留只为可比性）
   · hit_id   = 本次 opcode 的**真实目标 id** 落在族 id 集里（按 id 查，不按地址） */
static unsigned long long g_gpuop_hit_tgt,g_gpuop_hit_old3,g_gpuop_hit_id;
static unsigned char g_gpuop_dst_seen[TEXREG_N / 8];   /* 已解码过的目标 id（每 id 至多一行） */
static unsigned g_gpuop_dst_lines;
static void nvn_gpuop_note_dst(int kind, const void *tex, uint64_t src_addr)
{
 if (!tex || g_gpuop_dst_lines >= 60) return;
 const texsnap *r = texrec_get(tex, 0);
 int id = g_tex_id_by_key(tex);
 /* 旁路台账（只装 3D 图）：texrec 环被挤掉时仍能认出目标是谁 */
 void *vm = NULL; uint32_t vw = 0, vh = 0, vd = 0; unsigned vfmt = 0;
 int vol = g_vol_find(tex, &vm, &vw, &vh, &vd, &vfmt);
 if (id >= 0 && id < TEXREG_N) {
  if (g_gpuop_dst_seen[id >> 3] & (1u << (id & 7))) return;
  g_gpuop_dst_seen[id >> 3] |= (unsigned char)(1u << (id & 7));
 }
 g_gpuop_dst_lines++;
 {uint64_t nz = 0, tot = 0, snz = 0;
  const void *mem = r && r->mem ? r->mem : vm;
  uint64_t avail = r && r->avail ? r->avail : (vol ? 4096u : 0u);
  if (mem && avail) { uint64_t n = avail < 65536 ? avail : 65536; tp_region((const uint8_t *)mem, n, &nz, &tot); }
  /* 源缓冲（copy_buffer_to_texture 的 NVNbufferAddress）：能读就读 4 KB 报 nz，不能读就标 X */
  char stos[24]; snprintf(stos, sizeof stos, "X");
  if (src_addr) {
   uint8_t sb[4096];
   if (rt_read_range(src_addr, sb, sizeof sb)) {
    uint64_t k; for (k = 0; k < sizeof sb; k++) if (sb[k]) snz++;
    snprintf(stos, sizeof stos, "%llu/4096", (unsigned long long)snz);
   }
  }
  vk_boot_logf("[gpu] %s dst obj=%p id=%d src=%s %ux%ux%u tgt=%d fmt=0x%x lv=%u mem=%p nz@base=%llu/%llu srcaddr=%#llx src_nz=%s",
   g_gpuop_name[kind], tex, id, r ? "rec" : (vol ? "vol-ledger" : "miss"),
   r ? r->d.width : vw, r ? r->d.height : vh, r ? r->d.depth : vd,
   r ? r->d.target : (vol ? 2 : -1), r ? (unsigned)r->d.format : vfmt, r ? r->d.levels : 0u,
   mem, (unsigned long long)nz, (unsigned long long)tot,
   (unsigned long long)src_addr, stos);}
}
static void nvn_gpuop_report(unsigned sub)
{
 unsigned long long tot = 0;
 char b[512]; int o = 0;
 for (int i = 0; i < GPUOP_N; i++) tot += g_gpuop_n[i];
 {int m34=m34a_on();
  /* 关态：hit_tgt 打印 hit_old3（与 M33A 逐字同值的旧口径）；开态：打印**动态族表**命中数（修盲区后），
     并把两个对照口径一起打出来（hit_old3 旧名单 / hit_id 按真实目标 id）。 */
  unsigned long long ht=m34?g_gpuop_hit_tgt:(g_gpuop_hit_tgt+g_gpuop_hit_old3);
  o = snprintf(b, sizeof b, "[h] gpu write ops @submit#%u: total=%llu executed=%llu dropped=%llu (gated=%llu unres=%llu fmt=%llu geom=%llu unmapped=%llu) hit_tgt=%llu",
              sub, tot, g_gpuop_exec, g_gpuop_skip_unres + g_gpuop_skip_fmt + g_gpuop_skip_geom + g_gpuop_gated + g_gpuop_skip_unmapped,
              g_gpuop_gated, g_gpuop_skip_unres, g_gpuop_skip_fmt, g_gpuop_skip_geom, g_gpuop_skip_unmapped, ht);
  if(m34&&o<(int)sizeof b-96)
   o += snprintf(b + o, sizeof b - (size_t)o, " hit_old3=%llu hit_id=%llu fam_n=%u fam_dup=%llu fam_ovf=%llu",
                 g_gpuop_hit_old3, g_gpuop_hit_id, g_m34a_fam_n, g_m34a_fam_dup, g_m34a_fam_ovf);}
 for (int i = 0; i < GPUOP_N; i++)
  if (g_gpuop_n[i] && o < (int)sizeof b - 40)
   o += snprintf(b + o, sizeof b - (size_t)o, " %s=%llu", g_gpuop_name[i], g_gpuop_n[i]);
 vk_boot_logf("%s", b);
}

/* ---- M4v P2：内容指纹（首绑后若源变了就重传） ---- */
/* 为什么需要：上送是「首绑一次，永不重传」（gfx_bridge.c 的 g_idup 门控）。b2 实测
   id=1894 在上送那一刻池里 0 字节，之后是否被写过完全不可见。这里用 4 个 64 B 窗口的
   FNV 指纹做「这张纹理的源变了没有」，变了就在后续绑定上重传（每 id 至多 8 次）。
   逐帧刷新的 RT 走 route②，不进这条路径；这里只覆盖真纹理。 */
static uint64_t tp_fingerprint(const uint8_t *mem,uint64_t avail){
 uint64_t h=1469598103934665603ull;
 if(!mem||!avail)return 0;
 static const unsigned wpos[4]={0,7,17,31};   /* 64B 窗口起点（块线性/线性的头尾/中段都能覆盖到） */
 for(int i=0;i<4;i++){
  uint64_t off=(avail>64)?((avail-64)*wpos[i]/31):0;if(off>avail-1)off=0;
  uint64_t n=avail-off<64?avail-off:64;
  for(uint64_t k=0;k<n;k++){h^=mem[off+k];h*=1099511628211ull;}
 }
 return h;}
/* ================= M40H（2026-11-09）：纹理内容「晚到」竞态取证 =================
   假设：A/B/C/D 四态随机停 = 首绑时刻源池还没被游戏写（内容快照太早）。
   静态捕获点（本轮实测）：gfx_bridge.c 首次 bind 分支 -> nvn_vk_tex_upload_storage()
     （nvn_vk.c:1753）同步读客体池（块线性先 nvn_deswizzle），**无 fence、无 queue wait、
     不等 submit** => 上传内容 = bind 那一刻的池字节。
   本探针纯只读；关态零客体读、零日志，行为与 M40G 逐位相同。每个首次 bind 的纹理（≤64 个 id）：
     bind 时读源首 64 B -> bind_nz/hash；之后每个 submit 重读同一地址，最多 30 帧，
     记 first_nz（首个 nz>0 的帧号）与 hash_chg；30 帧全零者在收尾节拍复查一次
     （late_after => 只是窗口太短，不是「永零」）。
   排除：RT 纹理（GPU 写、CPU 侧本就是零）、压缩格式（upload 直接 -6）、≥1280×720 窗口/全屏。
   判据：late = bind 时 nz==0 且 30 帧内有非零；never = 30 帧全 0。 */
#define M40H_MAX 64
#define M40H_FRAMES 30
#define M40H_SW_A "/data/storage/el2/base/files/smo_m40h"
#define M40H_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40h"
static int m40h_on(void)
{
    static int v = -1; static unsigned tick;
    if (v >= 0 && (tick++ & 63u)) return v;
    v = 0;
    { const char *p[2] = { M40H_SW_A, M40H_SW_B };
      for (int i = 0; i < 2; i++) { FILE *f = fopen(p[i], "r");
        if (f) { int x = 0; if (fscanf(f, "%d", &x) == 1 && x > 0) v = 1; fclose(f); break; } } }
    return v;
}
__attribute__((constructor)) static void m40h_seed(void)
{
    static const char *const p[2] = { M40H_SW_A, M40H_SW_B };
    for (int i = 0; i < 2; i++) {
        FILE *r = fopen(p[i], "r");
        if (r) { fclose(r); continue; }
        { FILE *f = fopen(p[i], "w"); if (f) { fputs("0\n", f); fclose(f); } }
    }
}
struct m40h_e {
    unsigned id; uint64_t src; uint32_t avail; int fmt; unsigned w, h;
    unsigned long long bind_sub, up_sub;
    int bind_rd, bind_nz, up_nz;
    long first_nz; unsigned hash_chg, last_h, frames, hexd, late_after, done;
};
static struct m40h_e g_m40h[M40H_MAX];
static unsigned g_m40h_n, g_m40h_hex;
static unsigned long long g_m40h_sum_next = 300;
static unsigned m40h_nz(const uint8_t *p, uint32_t n)
{ unsigned k = 0; for (uint32_t i = 0; i < n; i++) if (p[i]) k++; return k; }
static unsigned m40h_h32(const uint8_t *p, uint32_t n)
{ unsigned h = 2166136261u; for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; } return h; }
static void m40h_hex(char *o, size_t cap, const uint8_t *p, uint32_t n)
{ size_t k = 0; for (uint32_t i = 0; i < n && k + 3 < cap; i++) k += (size_t)snprintf(o + k, cap - k, "%02x", (unsigned)p[i]); }
static int m40h_skip(int tid, const texsnap *sn)
{
    unsigned fmt, w, h;
    if (!sn) return 1;
    fmt = (unsigned)sn->d.format; w = sn->d.width; h = sn->d.height;
    if (fmt >= 0x41u && fmt <= 0x50u) return 1;   /* 压缩格式：CPU 不解码（upload 直接 -6） */
    if (fmt >= 0x79u && fmt <= 0x94u) return 1;
    if (w >= 1280u || h >= 720u) return 1;        /* 窗口 / 全屏 scratch */
    if (nvn_vk_rt_tex_kind(tid) != 0) return 1;   /* RT：GPU 写，CPU 池侧从未有内容 */
    return 0;
}
static void m40h_bind(int tid, const texsnap *sn, int up_ok)
{
    struct m40h_e *e; unsigned i, n; unsigned long long sub;
    if (tid < 0 || !m40h_on() || m40h_skip(tid, sn)) return;
    for (i = 0; i < g_m40h_n; i++) if (g_m40h[i].id == (unsigned)tid) break;
    if (i == g_m40h_n) {
        if (g_m40h_n >= M40H_MAX) return;
        memset(&g_m40h[i], 0, sizeof g_m40h[i]);
        g_m40h[i].id = (unsigned)tid;
        g_m40h[i].first_nz = -1;
        g_m40h[i].bind_sub = atomic_load_explicit(&g_beat_nvn_submit, memory_order_relaxed);
        g_m40h_n++;
    }
    e = &g_m40h[i];
    n = sn->avail < 64u ? sn->avail : 64u;
    sub = atomic_load_explicit(&g_beat_nvn_submit, memory_order_relaxed);
    if (!e->bind_rd) {   /* 首次 bind 的内容基线（只记一次） */
        uint8_t b[64]; memset(b, 0, sizeof b);
        e->fmt = (int)sn->d.format; e->w = sn->d.width; e->h = sn->d.height;
        e->src = (uint64_t)(uintptr_t)sn->mem; e->avail = sn->avail; e->bind_rd = 1;
        if (!n || !rt_read_range(e->src, b, n)) { e->bind_nz = 0; e->bind_rd = 2; }
        else e->bind_nz = (int)m40h_nz(b, n);
        if (g_m40h_hex < 3u && n) {   /* 前 3 个 id：bind 时 64 B hex */
            char hx[136]; memset(hx, 0, sizeof hx);
            m40h_hex(hx, sizeof hx, b, n); g_m40h_hex++; e->hexd = 2;
            vk_boot_logf("[m40h] hex id=%u at=bind nz=%d rd=%d b64=%s", e->id, e->bind_nz, e->bind_rd, hx);
        }
    }
    if (up_ok == 1 && !e->up_sub) {   /* 首次「真正上送成功」的那次 bind：捕获时刻的实际地址/内容 */
        uint8_t b[64]; uint32_t k = sn->avail < 64u ? sn->avail : 64u; memset(b, 0, sizeof b);
        e->up_sub = sub;
        e->src = (uint64_t)(uintptr_t)sn->mem; e->avail = sn->avail;
        e->up_nz = (k && rt_read_range(e->src, b, k)) ? (int)m40h_nz(b, k) : -1;
    }
}
static void m40h_tick(void)
{
    unsigned long long sub;
    if (!g_m40h_n || !m40h_on()) return;
    sub = atomic_load_explicit(&g_beat_nvn_submit, memory_order_relaxed);
    for (unsigned i = 0; i < g_m40h_n; i++) {
        struct m40h_e *e = &g_m40h[i];
        uint8_t b[64]; uint32_t n; unsigned nz, hh; unsigned long long df;
        if (e->done || sub <= e->bind_sub) continue;
        df = sub - e->bind_sub;
        n = e->avail < 64u ? e->avail : 64u;
        if (!n) { e->done = 1; continue; }
        memset(b, 0, sizeof b);
        if (!rt_read_range(e->src, b, n)) { e->done = 1; e->first_nz = -1; }
        nz = m40h_nz(b, n); hh = m40h_h32(b, n);
        if (nz > 0 && e->first_nz < 0) e->first_nz = (long)df;
        if (e->frames && hh != e->last_h) e->hash_chg++;
        e->last_h = hh; e->frames = (unsigned)df;
        if (e->hexd == 2 && df >= 10ull) {   /* 前 3 个 id：10 帧后的 64 B hex */
            char hx[136]; memset(hx, 0, sizeof hx);
            m40h_hex(hx, sizeof hx, b, n); e->hexd = 1;
            vk_boot_logf("[m40h] hex id=%u at=f%llu nz=%u b64=%s", e->id, df, nz, hx);
        }
        if (df >= (unsigned long long)M40H_FRAMES) {
            e->done = 1;
            vk_boot_logf("[m40h] id=%u fmt=%#x ext=%ux%u bind_nz=%d first_nz_frame=%d hash_chg=%d bind_frame=%llu up_nz=%d up_frame=%llu",
                         e->id, (unsigned)e->fmt, e->w, e->h, e->bind_nz, (int)e->first_nz,
                         (int)e->hash_chg, (unsigned long long)e->bind_sub, e->up_nz,
                         (unsigned long long)e->up_sub);
        }
    }
    if (sub >= g_m40h_sum_next) {
        unsigned done = 0, zeroat = 0, late = 0, never = 0;
        g_m40h_sum_next = sub + 300ull;
        for (unsigned i = 0; i < g_m40h_n; i++) {
            struct m40h_e *e = &g_m40h[i];
            if (!e->done) continue;
            done++;
            if (e->bind_nz != 0) continue;
            zeroat++;
            if (e->first_nz > 0) { late++; continue; }
            never++;
            if (!e->late_after) {   /* 30 帧窗口之后的复查：内容是否最终到了 */
                uint8_t b[64]; uint32_t n = e->avail < 64u ? e->avail : 64u; unsigned nz;
                if (!n) continue;
                memset(b, 0, sizeof b);
                if (!rt_read_range(e->src, b, n)) continue;
                nz = m40h_nz(b, n);
                if (nz) { e->late_after = 1;
                    vk_boot_logf("[m40h] late_after id=%u at_frame=%llu nz=%u/%u",
                                 e->id, sub - e->bind_sub, nz, n); }
            }
        }
        vk_boot_logf("[m40h] sum n=%u done=%u zero_at_bind=%u late=%u never=%u hex=%u sub=%llu",
                     g_m40h_n, done, zeroat, late, never, g_m40h_hex, sub);
    }
}
static uint64_t g_tp_fp[TEXREG_N];    /* 每 id 上次上送时的指纹（8 B × 65536 = 512 KB） */
#define TP_REUP_MAX 8
static unsigned char g_tp_reup_n[TEXREG_N];
/* M40AE ④：一张图整图上送/补传完成后，把**所有指向它的别名 id** 的 per-id 状态同步一致
   （g_idup=1 + g_tp_fp=图的指纹）。两个作用：① 遗留探针口径（g_idup）不漂移；
   ② 早于首次上送就建立别名的 id 不会因为"自己的 fp 还是 0"把同一张整图再补传一次。 */
static void m40ae_sync_ids(int tid)
{
 int ei=(tid>=0&&tid<TEXREG_N)?g_m40ae_eid[tid]:-1;
 if(ei<0||ei>=M40AE_EN)return;
 m40ae_img *e=&g_m40ae_img[ei];
 if(!e->vk||!e->up)return;
 for(int q=0;q<TEXREG_N;q++)if(g_m40ae_eid[q]==ei){g_idup[q]=1;g_tp_fp[q]=e->fp;}
}
static unsigned long long g_t_reup,g_t_reup_bytes;
/* M4e（route②）：源图替换计数。hit = 「本笔绑的 id == 我们某张 RT 的 id」；
   self = 那张 RT 正是当前渲染目标（走影子副本），other = 别的 RT（直接采样，转布局）；
   copy_ok/copy_fail/throttle = 影子副本结果；view_fail = 替换图拿不到（回落原路径）。
   miss_unres_noid = 句柄解析不到且该 id 不是任何 RT —— 「未命中」的取证分母。 */
static unsigned long long g_t_rt_hit_self,g_t_rt_hit_other,g_t_rt_view_fail,g_t_rt_miss;
/* M4w P1：绑定期「按存储身份」反查的口径。hits = store 命中并改绑 RT 图；miss = store 可用但 RT 表里没有；
   link = 命中时 RT 槽 tex_id 未知、顺手补登的条数（进 rt_link 的 store 桶）；rej 见 nvn_vk_rt2_rejected()。 */
static unsigned long long g_t_rt2_hits,g_t_rt2_miss,g_t_rt2_link;
static unsigned g_t_rt2_logged;
/* M4w P1 验收口径 ②③：命中里 id 从未上送过的（这条路以前只能拿到 0 池/占位）有多少，
   以及命中的不同 id 数（区分「少数热 id 反复命中」与「面真的铺开了」）。 */
static unsigned long long g_t_rt2_noup;
static unsigned char g_rt2_idbit[TEXREG_N/8];
static unsigned g_t_rt2_ids;
static void g_rt2_note_id(int id)
{
    if (id < 0 || id >= TEXREG_N) return;
    unsigned char *b = &g_rt2_idbit[id >> 3]; unsigned m = 1u << (id & 7);
    if (!(*b & m)) { *b |= (unsigned char)m; g_t_rt2_ids++; }
}
static unsigned g_t_rt_logged;                            /* 替换日志限流（首 3 条 + 每 1000 条） */
static unsigned long long g_t_rt_hit_total;               /* 命中总数（限流用） */
/* M6a A：零内容被采样纹理 → 绑我们 RT 图 的口径。bound=真改绑次数；miss=零内容但没有唯一同形 RT
   （明细见 [tex] missing 行）；zmiss_ids=不同 id 数；multi 在 nvn_vk 里计（多候选放弃）。 */
static unsigned long long g_t_rt_zero_bound,g_t_rt_zero_miss;
static unsigned g_t_rt_zero_log,g_t_zmiss_ids,g_t_rt_zero_order_log;
static unsigned char g_zmiss_bit[TEXREG_N/8];
/* M6a：零内容样本按 (ext,fmt) 汇总（16 个形状）。回答「还剩哪些形状确实没有同形 RT」——
   只记不放行，供报告逐条列形状；不参与任何判定。 */
#define R_ZS_N 16
static struct {unsigned w,h;int fmt;unsigned long long n;int cand,img,id0;} g_zs[R_ZS_N];
static int g_zs_n;
static void zs_note(unsigned w,unsigned h,int fmt,int cand,int img,int id)
{
 for(int i=0;i<g_zs_n;i++)
  if(g_zs[i].w==w&&g_zs[i].h==h&&g_zs[i].fmt==fmt){g_zs[i].n++;g_zs[i].cand=cand;g_zs[i].img=img;return;}
 if(g_zs_n>=R_ZS_N)return;
 g_zs[g_zs_n].w=w;g_zs[g_zs_n].h=h;g_zs[g_zs_n].fmt=fmt;g_zs[g_zs_n].n=1;
 g_zs[g_zs_n].cand=cand;g_zs[g_zs_n].img=img;g_zs[g_zs_n].id0=id;g_zs_n++;
}
/* M4u P2：NVN 纹理槽 16 → 32。descriptor 侧 binding=32+slot（nvn_vk.c r_ds_ensure 同口径），
   32+31=63 不会撞 binding 17/18/19（GMem/RScale/Tint）。绑定数组同步扩到 32。 */
#define TX_SLOT_MAX 32
/* M7l P1（只读取证）：绑定时刻的「零内容 id」源字节复查。zero_content 的 bind 事件全部来自
   「上传时解排布首 4 KiB 全零」的位图（g_tu_zero_bit，M5y 口径）；该位图置位后从不清——reup
   换上非零内容它也还是 1（陈旧标志）。本探针在 FS 真要绑它的那一刻重读源区全长，回答两件事：
     ① nz_now≈0 ⇒ 源到此刻仍没被写（历史 totals reup=0 ⇒ 上游从头到尾没给数据）；
     ② nz_now>0 ⇒ 源后来有数据了（上传时是零 =「写晚了」；内容是否跟上看 reu= 重传次数）。
   门：M7L_ON 且场景族 draw 已出现（本轮只关心场景会采到的那批）。深度族（fmt 0x31..0x36）
   跳过——GPU 写的深度/模板图 CPU 侧恒零是已知态，不占预算。每 id 一次，预算 16 行。 */
static unsigned long long g_m7l_bind_lines;
static unsigned char g_m7l_bind_done[TEXREG_N/8];
static void m7l_bind_probe(int tid,const texsnap *sn){
 if(!M7L_ON||!nvn_vk_scene_seen()||!sn||!sn->mem||!sn->avail)return;
 if(tid<0||tid>=TEXREG_N)return;
 if(sn->d.format>=0x31&&sn->d.format<=0x36)return;   /* 深度/模板族：已知 GPU 写，跳过 */
 if(!g_idup[tid]||!nvn_vk_tex_zero_id(tid))return;
 if(g_m7l_bind_done[tid>>3]&(1u<<(tid&7)))return;
 if(g_m7l_bind_lines>=16)return;
 g_m7l_bind_done[tid>>3]|=(unsigned char)(1u<<(tid&7));
 g_m7l_bind_lines++;
 uint64_t nz=0,tot=0;tp_region((const uint8_t *)sn->mem,sn->avail,&nz,&tot);
 uint32_t w[4]={0,0,0,0};
 if(sn->avail>=16)memcpy(w,sn->mem,16);
 vk_boot_logf("[m7l] bind id=%d fmt=0x%x %ux%u bytes=%llu nz_now=%u%% w0=%08x,%08x,%08x,%08x up=%d reu=%d",
  tid,(int)sn->d.format,sn->d.width,sn->d.height,(unsigned long long)sn->avail,
  (unsigned)(sn->avail?100*nz/sn->avail:0),w[0],w[1],w[2],w[3],
  (int)g_idup[tid],(int)g_tp_reup_n[tid]);
 /* M7l P2：进零内容观察名单（前 6 个，复查走 tex_probe_tick 的 4096 绑定节拍） */
 if(g_m7l_zw_n<6&&m7l_dyn_idx(tid)<0){
  g_m7l_zw[g_m7l_zw_n++]=tid;
  vk_boot_logf("[m7l] zw watch id=%d slot=%u fmt=0x%x avail=%llu (zero-content bind)",tid,g_m7l_zw_n-1,
   (int)sn->d.format,(unsigned long long)sn->avail);}
}
/* ================= M40O (2026-11-10)：id 图缓存容量 ×4 —— 单点改动，开关 smo_m40o ==================
   依据（M40O §1 因果性判定，证据 logs/m40n_b1/vk_boot.log @submit#3300）：
     印刷的 `cap_idimg=1504944` 里 `g_tx_was=1254863`（83.4%）是**LRU 让位换来的槽**被
     nvn_vk.c:18761 事后并入（这些槽根本不空、有真图），真 why=-31/-32/-33 只有 250081。
     而 `[t] lru evict=4688 deny=109178 idimg=384/384` + `img`（= vkCreateImage 计数）
     8.1–8.7/帧 指向同一机制：容量 384 装不下工作集 ⇒ 每帧 驱逐→销毁→下帧重建+重上送 ⇒ 吞吐墙。
   ⇒ 本开关**只改一个常量**：id 图缓存容量 384 → 1536（4×）。让位配额（M5Z_EVICT_PER_SUB）、
     门槛（M5Z_EVICT_MIN_BINDS / 热度）、字节账（g_t_bytes_live）一律不动，便于单变量归因。
   读法：文件 2 s 重读（构造期建 0666 松手文件、内容 0）⇒ 写 0 即秒级回退，不用重装。 */
#define M40O_SW_A "/data/storage/el2/base/files/smo_m40o"
#define M40O_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40o"
static int m40o_on(void)
{
 static int cached=-1; static long last;
 long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40O_SW_A,M40O_SW_B};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached;
}
/* 每 60 s 一行；t = 自**首次开启**后的秒数（A/B 对齐用，不是 boot 秒）。关态第一次就返回 ⇒ 基线日志不变。 */
static void m40o_stat(void)
{
 static long t0; static long last;
 if(!(m40o_on()&1))return;
 long now=(long)time(NULL);
 if(!t0)t0=now;
 if(now-last<60)return;
 last=now;
 vk_boot_logf("[m40o] stat t=%lus imgs=%llu evict=%llu reup=%llu cap=%u lru=%llu deny=%llu",
              (unsigned long)(now-t0),(unsigned long long)g_t_up,(unsigned long long)g_z_evict,
              (unsigned long long)g_t_reup,(unsigned)k_id_img_max,(unsigned long long)g_t_idimg,
              (unsigned long long)g_z_evict_deny);
}
__attribute__((constructor)) static void m40o_seed(void)
{
 static const char *const p[2]={M40O_SW_A,M40O_SW_B};
 for(int i=0;i<4;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}
/* ================= M40Q (2026-11-10)：一次性 id 跳过建图 —— 单点判决改动，开关 smo_m40q ==============
   依据（M40P 已证，本轮不重查）：tex 桶占建图 92.6%（墙后 100%），其中 98% 是**不同 id 的首次**建图；
   工作集 ≈3442 个不同 id ≫ 384 槽 ⇒ 墙后每窗 `dst==tex` 1:1：建一张，帧末就被 LRU 让位踢掉一张。
   关键：本文件 `g_t_idbinds[tid]` 在 bind 入口（:3526）已经 +1，所以在建图判决处
   `g_t_idbinds[tid]==1` 的含义是「到此刻为止这个 id 只被绑过这一次」= 一次性 id。
   ⇒ 本开关**只在建图/让位判决段前置一个跳过分支**：不驱逐、不建 VkImage，直接落占位。
     名额 cap(384)、让位配额(M5Z_EVICT_PER_SUB)、门槛/热度、字节账(g_t_bytes_live) 一律不动 ⇒ 单变量归因。
   自我纠正：同一 id 第二次绑定时 g_t_idbinds 已是 2 ⇒ 不再跳过、正常建图 ⇒ 真会被复用的 id 只多白一帧。
   读法：文件 2 s 重读（构造期建 0666 松手文件、内容 0）⇒ 写 0 即秒级回退，不用重装。 */
#define M40Q_SW_A "/data/storage/el2/base/files/smo_m40q"
#define M40Q_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40q"
#define M40Q_THR 1u   /* 跳过判据：g_t_idbinds[tid] <= 1（一次性 id；含本次 bind） */
static unsigned long long g_m40q_tex, g_m40q_skip;   /* 判决次数 / 跳过次数（累计） */
static int m40q_on(void)
{
 static int cached=-1; static long last;
 long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40Q_SW_A,M40Q_SW_B};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached;
}
/* 每 60 s 一行（仅开态）。t = 自**首次开启**后的秒数（A/B 对齐用，不是 boot 秒）；字段全是本窗口增量。
   dst 口径：本文件可见的 id 图销毁（= 让位换掉的那张）；全局 vkDestroyImage 另有 [m40p] cr 行可对齐。 */
static void m40q_stat(void)
{
 static long t0,last; static int announced;
 static unsigned long long tex0,skip0,ev0,deny0,f0;
 long now=(long)time(NULL);
 if(!(m40q_on()&1))return;
 if(!t0)t0=now;
 if(!announced){announced=1;last=now;
  f0=(unsigned long long)atomic_load_explicit(&g_beat_frames,memory_order_relaxed);
  tex0=g_m40q_tex;skip0=g_m40q_skip;ev0=g_z_evict;deny0=g_z_evict_deny;
  vk_boot_logf("[m40q] on cap=%u skip_oneoff=%d thr=%u idimg=%llu evict=%llu deny=%llu",
   (unsigned)k_id_img_max,1,(unsigned)M40Q_THR,(unsigned long long)g_t_idimg,
   (unsigned long long)g_z_evict,(unsigned long long)g_z_evict_deny);
  return;}
 if(now-last<60)return;
 last=now;
 {unsigned long long fnow=(unsigned long long)atomic_load_explicit(&g_beat_frames,memory_order_relaxed);
  unsigned long long texn=g_m40q_tex,skipn=g_m40q_skip,evn=g_z_evict,denyn=g_z_evict_deny;
  unsigned long long df=fnow-f0;
  vk_boot_logf("[m40q] st t=%us tex=%u skip=%u evict=%u dst=%u deny=%u imgpf=%.2f fps60=%u",
   (unsigned)(now-t0),(unsigned)(texn-tex0),(unsigned)(skipn-skip0),(unsigned)(evn-ev0),
   (unsigned)(evn-ev0),(unsigned)(denyn-deny0),df?(double)(texn-tex0)/(double)df:-1.0,
   (unsigned)(df/60ull));
  tex0=texn;skip0=skipn;ev0=evn;deny0=denyn;f0=fnow;}
}
__attribute__((constructor)) static void m40q_seed(void)
{
 static const char *const p[2]={M40Q_SW_A,M40Q_SW_B};
 for(int i=0;i<4;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}

/* ================= M40AD (2026-11-10)：存储→id 映射稳定性（AD1/AD2 的唯一判据） ==================
   由来：M40P 已证建图 92.6% 出自 nvn_vk_tex_create、其中 98% 是**不同 id 的首次**建图，工作集 ≈3442 id
   对 384 个 id-image 名额（k_id_img_max）。待判：这 3442 个 id 是
     ① 同一块客体纹理存储被反复登记/绑到**不同 id**（⇒ id 管理问题：按存储去重复用一张 VkImage 即砍掉建图），
     ② 还是真·3442 块不同存储（⇒ 工作集真实很大，只能回去压冻结区）。
   判据唯一：key=(客体地址 mem, w, h, fmt) 再次建图时拿到的 id 是否与首次相同 ⇒ remap。
     remap 显著（≥100 或 ≥keys 的 30%）⇒ AD1；remap≈0 且 keys≈id_uniq(3400+) ⇒ AD2。
   落点：本文件**唯一**的 id 建图点（g_rp_bind_tex 里 `g_idvk[tid]=vk;` 之后，M40P 同口径）。
   只读、不改任何判决/名额/让位。开关 smo_m40ad（构造期自种 0666 文件，2 s 重读；缺省 0 ⇒ 关态零输出）。
   注意：mem==0（池基址未知）的纹理不参与 key 记账（否则同 w/h/fmt 的无关纹理会被并成一个 key 假报 remap），
        单独计 nomem。输出预算 [m40ad] ≤120 行：on 1 + st ~7 + REMAP ≤60 + top10×2 ≤20。 */
#define M40AD_SW_A "/data/storage/el2/base/files/smo_m40ad"
#define M40AD_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40ad"
#define M40AD_KN 8192      /* key 表（线性探测，探测上限 64）：32 B×8192 = 256 KB .bss */
#define M40AD_LINE_MAX 60  /* REMAP 总行预算 */
#define M40AD_PER_KEY 3    /* 每个 key 最多 3 行 REMAP */
#define M40AD_TOP 10
typedef struct { unsigned long long mem; unsigned w,h,fmt; int first_id; unsigned n,n_diff,nlines; } m40ad_k;
static m40ad_k g_m40ad_k[M40AD_KN];
static unsigned char g_m40ad_idseen[TEXREG_N/8];   /* 出过图的 id 位图（id_uniq） */
static unsigned g_m40ad_idhash[TEXREG_N];          /* id → 首次建图时的 key 32 位哈希（idrekey 用） */
static unsigned g_m40ad_keys,g_m40ad_iduniq,g_m40ad_remap,g_m40ad_remap_lines;
static unsigned g_m40ad_keyfull,g_m40ad_nomem,g_m40ad_builds,g_m40ad_idrekey;
static long g_m40ad_t0;

static int m40ad_on(void)
{
 static int cached=-1; static long last;
 long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40AD_SW_A,M40AD_SW_B};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached;
}
static unsigned m40ad_tsec(void)
{
 long now=(long)time(NULL);
 if(!g_m40ad_t0) g_m40ad_t0=now;
 return (unsigned)(now-g_m40ad_t0);
}
static unsigned m40ad_khash(unsigned long long mem,unsigned w,unsigned h,unsigned fmt)
{
 unsigned long long x=mem*0x9E3779B97F4A7C15ull;
 x^=(unsigned long long)w*0x100000001B3ull;
 x^=((unsigned long long)h<<32)^(unsigned long long)fmt;
 x^=x>>31;x*=0xD6E8FEB86659FD93ull;x^=x>>29;
 return (unsigned)(x^(x>>32));
}
/* 唯一建图点回调：只读记账。tid<0 或 mem==0 不参与 key 表。 */
static void m40ad_img(int tid,unsigned long long mem,unsigned w,unsigned h,unsigned fmt)
{
 if(tid<0) return;
 if(!m40ad_on()) return;
 g_m40ad_builds++;
 if(!(g_m40ad_idseen[tid>>3]&(unsigned char)(1u<<(tid&7)))){
  g_m40ad_idseen[tid>>3]|=(unsigned char)(1u<<(tid&7));g_m40ad_iduniq++;
  g_m40ad_idhash[tid]=m40ad_khash(mem,w,h,fmt);
 } else if(g_m40ad_idhash[tid]!=m40ad_khash(mem,w,h,fmt)) g_m40ad_idrekey++;   /* 同一 id 换了存储（槽复用，正常） */
 if(!mem){g_m40ad_nomem++;return;}
 unsigned k0=m40ad_khash(mem,w,h,fmt)%M40AD_KN;
 for(unsigned k=0;k<64;k++){
  m40ad_k *e=&g_m40ad_k[(k0+k)%M40AD_KN];
  if(e->n==0){e->mem=mem;e->w=w;e->h=h;e->fmt=fmt;e->first_id=tid;e->n=1;g_m40ad_keys++;return;}
  if(e->mem==mem&&e->w==w&&e->h==h&&e->fmt==fmt){
   e->n++;
   if(e->first_id!=tid){
    e->n_diff++;g_m40ad_remap++;
    if(e->nlines<M40AD_PER_KEY&&g_m40ad_remap_lines<M40AD_LINE_MAX){
     e->nlines++;g_m40ad_remap_lines++;
     vk_boot_logf("[m40ad] REMAP key=%#llx/%ux%u/%#x old_id=%u new_id=%u t=%us frame=%llu",
      (unsigned long long)mem,w,h,(unsigned)fmt,(unsigned)e->first_id,(unsigned)tid,
      m40ad_tsec(),(unsigned long long)atomic_load_explicit(&g_beat_frames,memory_order_relaxed));
    }
   }
   return;
  }
 }
 g_m40ad_keyfull++;
}
static void m40ad_top(void)
{
 int top[M40AD_TOP];unsigned tn[M40AD_TOP];
 for(int j=0;j<M40AD_TOP;j++){top[j]=-1;tn[j]=0;}
 for(int i=0;i<M40AD_KN;i++){
  unsigned d=g_m40ad_k[i].n_diff; if(!d) continue;
  for(int j=0;j<M40AD_TOP;j++) if(d>tn[j]){
   for(int k=M40AD_TOP-1;k>j;k--){tn[k]=tn[k-1];top[k]=top[k-1];}
   tn[j]=d;top[j]=i;break;}
 }
 for(int j=0;j<M40AD_TOP;j++){
  if(top[j]<0) break;
  const m40ad_k *e=&g_m40ad_k[top[j]];
  vk_boot_logf("[m40ad] top key=%#llx/%ux%u/%#x first_id=%u n=%u diff=%u",
   (unsigned long long)e->mem,e->w,e->h,(unsigned)e->fmt,(unsigned)e->first_id,e->n,e->n_diff);
 }
}
/* 每 60 s 一行（由 vk_boot.c 心跳线程驱动 ⇒ 渲染线程卡死时照打，可对齐 M40P 的 WALL）。
   字段全为累计值（imgs=vkCreateImage 累计、bound=纹理槽绑定累计），另给窗口 img/frame。 */
void nvn_vk_m40ad_poll(unsigned tsec, unsigned long frames, double cpu)
{
 (void)cpu;(void)tsec;   /* t 轴统一走 m40ad_tsec()（首次开启起算，与 [m40p] 同口径） */
 static long last; static unsigned win;
 static unsigned long long f0,line0;
 if(!m40ad_on()) return;
 long now=(long)time(NULL);
 if(!g_m40ad_t0) g_m40ad_t0=now;
 if(!last){
  last=now;f0=frames;
  vk_boot_logf("[m40ad] on cap=%u kn=%d line_max=%d t=%us frames=%llu",
   (unsigned)k_id_img_max,M40AD_KN,M40AD_LINE_MAX,m40ad_tsec(),(unsigned long long)frames);
  return;
 }
 if(now-last<60) return;
 last=now;win++;
 {unsigned long long imgs=(unsigned long long)atomic_load_explicit(&g_beat_img,memory_order_relaxed);
  unsigned long long df=frames-f0;
  unsigned long long bound=(unsigned long long)g_t_binds;
  vk_boot_logf("[m40ad] st t=%us imgs=%llu keys=%u id_uniq=%u remap=%u bound=%llu builds=%u idrekey=%u keyfull=%u nomem=%u | dframes=%llu imgpf=%.2f",
   m40ad_tsec(),imgs,g_m40ad_keys,g_m40ad_iduniq,g_m40ad_remap,bound,g_m40ad_builds,
   g_m40ad_idrekey,g_m40ad_keyfull,g_m40ad_nomem,df,df?(double)(imgs-line0)/(double)df:-1.0);
  line0=imgs;f0=frames;
  if(win==3||win==6) m40ad_top();
 }
}
__attribute__((constructor)) static void m40ad_seed(void)
{
 static const char *const p[2]={M40AD_SW_A,M40AD_SW_B};
 for(int i=0;i<4;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}
/* ================= M40AE 观测（由 vk_boot.c 心跳线程驱动）=================
   t 轴 = 自**首次开启**起算的秒（A/B 对齐用，与 M40P/M40AD 同口径）；字段全是累计/窗口值。
   imgs = 全局 vkCreateImage 累计（g_beat_img）；imgpf = 本窗口每帧新建图像数 —— A/B 的主读数。
   keys = 命中过的不同存储数（建过图的 key 数）；remap/hit = 同一存储又碰到新 id 的次数。
   预算 [m40ae] ≤ 1(on) + ~7(st) + 24(HIT) ≈ 32 行。 */
void nvn_vk_m40ae_poll(unsigned tsec,unsigned long frames,double cpu)
{
 (void)tsec;(void)cpu;
 static long last;static unsigned long long f0,i0;static unsigned win;
 if(!m40ae_on())return;
 long now=(long)time(NULL);
 if(!g_m40ae_t0)g_m40ae_t0=now;
 {unsigned t=(unsigned)(now-g_m40ae_t0);
  unsigned ids=0,refs_max=0;
  for(int i=0;i<TEXREG_N;i++)if(g_idvk[i])ids++;
  for(int i=0;i<M40AE_EN;i++)if(g_m40ae_img[i].vk&&g_m40ae_img[i].refs>refs_max)refs_max=g_m40ae_img[i].refs;
  if(!last){last=now;f0=frames;i0=(unsigned long long)atomic_load_explicit(&g_beat_img,memory_order_relaxed);
   vk_boot_logf("[m40ae] on=1 uniq_img=%u ids=%u refs_max=%u uniq_ent=%u keys=%u cap=%u en=%d t=%us",
    (unsigned)g_t_idimg,ids,refs_max,g_m40ae_nuniq,g_m40ae_keys,(unsigned)k_id_img_max,M40AE_EN,t);
   return;}
  unsigned dtw=(unsigned)(now-last);          /* 单位=秒；必须在 last=now 之前取 */
  if(dtw<20)return;
  last=now;win++;
  {unsigned long long imgs=(unsigned long long)atomic_load_explicit(&g_beat_img,memory_order_relaxed);
   unsigned long long dframes=frames-f0;
   vk_boot_logf("[m40ae] st t=%us imgs=%llu imgpf=%.2f uniq=%u ids=%u keys=%u id_uniq=%u remap=%u hit=%u skipup=%u evimg=%u fpm=%u rk=%.2f",
    t,imgs,dframes?(double)(imgs-i0)/(double)dframes:-1.0,(unsigned)g_t_idimg,ids,g_m40ae_keys,
    g_m40ae_iduniq,g_m40ae_hit,g_m40ae_hit,g_m40ae_skipup,g_m40ae_evimg,
    (unsigned)(dtw?(double)dframes*60.0/(double)dtw:0.0),
    (double)(g_m40ae_keys?100.0*(double)g_m40ae_hit/(double)g_m40ae_keys:0.0));
   i0=imgs;f0=frames;}
  (void)win;
 }
}
/* M40AG：拒绝建图/名额/驻留图 的独立口径（关态也打，开关 smo_m40ag，默认 0 零输出）。
   dcap = 本窗口 g_t_idimg_cap 增量 = 「本窗口有多少次建图被名额/失败拒掉」= 白占位产生速率。 */
void nvn_vk_m40ag_poll(unsigned tsec,unsigned long frames)
{
 static long last;static unsigned long long c0,i0;static unsigned f0;static int diag;
 if(!diag){diag=1;
  int bo=-1,bv=-1,ao=-1,av=-1;FILE *fb=fopen(M40AG_SW_B,"r");
  if(fb){bo=0;char b[16]={0};size_t n=fread(b,1,sizeof b-1,fb);if(n>0)b[n]=0;bv=atoi(b);fclose(fb);}
  FILE *fa=fopen(M40AG_SW_A,"r");
  if(fa){ao=0;char b[16]={0};size_t n=fread(b,1,sizeof b-1,fa);if(n>0)b[n]=0;av=atoi(b);fclose(fa);}
  vk_boot_logf("[m40ag] diag t=%us B_ok=%d B_val=%d A_ok=%d A_val=%d on=%d cap=%u",
   tsec,bo,bv,ao,av,m40ag_on(),m40ag_cap());}
 if(!m40ag_on())return;
 long now=(long)time(NULL);
 if(!last){last=now;c0=g_t_idimg_cap;i0=atomic_load_explicit(&g_beat_img,memory_order_relaxed);f0=frames;
  vk_boot_logf("[m40ag] on=1 t=%us idimg=%llu cap=%u dcap_total=%llu ev=%llu deny=%llu hit=%u",
   tsec,(unsigned long long)g_t_idimg,(unsigned)k_id_img_max,(unsigned long long)g_t_idimg_cap,
   (unsigned long long)g_z_evict,(unsigned long long)g_z_evict_deny,(unsigned)g_m40ae_hit);
  return;}
 {unsigned dtw=(unsigned)(now-last);if(dtw<20)return;
  last=now;
  {unsigned long long imgs=atomic_load_explicit(&g_beat_img,memory_order_relaxed);
   unsigned long long dcap=g_t_idimg_cap-c0;
   vk_boot_logf("[m40ag] st t=%us dcap=%llu dcap_tot=%llu idimg=%llu cap=%u ev=%llu deny=%llu hit=%u dimg=%llu dframes=%u imgs=%llu frames=%lu bytesMB=%llu",
    tsec,dcap,(unsigned long long)g_t_idimg_cap,(unsigned long long)g_t_idimg,(unsigned)k_id_img_max,
    (unsigned long long)g_z_evict,(unsigned long long)g_z_evict_deny,(unsigned)g_m40ae_hit,
    imgs-i0,(unsigned)(frames-f0),imgs,frames,(unsigned long long)(g_t_bytes_live>>20));
   c0=g_t_idimg_cap;i0=imgs;f0=(unsigned)frames;}
 }
}
/* ================= M40AH：线程 CPU / 冻结归因探针（每 20 s 一行 [m40ah]） =================
   目的：把 t=150–200 的 ~40 s 冻结拆开 ——「有线程在烧 CPU（SwiftShader JIT 编译）」vs
   「全都在阻塞等待」。读数口径：
     cpu = 进程 Δ(utime+stime) / (dtw × CLK_TCK) × 100，**单核 100%** 满格；
     tN  = 该线程同口径单核百分比，按本窗口 Δticks 降序取前 3（/proc/self/task/<tid>/stat 字段 14/15）。
   开关 smo_m40ah：seeder 种 A 视图（/data/storage/el2/base/files/，构造期 app 唯一能 open 的），
   读取 B 视图优先（hdc 唯一可写）+ A 兜底（同 m40ag）。默认 0 ⇒ 零 [m40ah] 输出、不扫 /proc。 */
#define M40AH_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40ah"
#define M40AH_SW_A "/data/storage/el2/base/files/smo_m40ah"
#define M40AH_TMAX 64
static int m40ah_on(void)
{
 static int cached=-1;static long last;long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40AH_SW_B,M40AH_SW_A};   /* B=/data/app/… 优先（hdc 可写视图），A=/data/storage/… 兜底 */
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached&1;
}
/* seeder：必须种 A 视图（构造期 app 在 B 视图 open(O_CREAT) 被 SELinux 拒；A/B 同一物理目录）。 */
__attribute__((constructor)) static void m40ah_seed(void)
{
 static const char *const p[2]={M40AH_SW_A,M40AH_SW_B};
 for(int i=0;i<2;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}
typedef struct { int tid; unsigned long long u,s; char comm[32]; char st; } m40ah_ent;
/* 进程累计 CPU（utime+stime），单位 = CLK_TCK ticks；getrusage 的 timeval → ticks。 */
static unsigned long long m40ah_gut(void)
{
 struct rusage ru; if(getrusage(RUSAGE_SELF,&ru)!=0)return 0;
 long hz=sysconf(_SC_CLK_TCK); if(hz<=0)hz=100;
 unsigned long long t=(unsigned long long)ru.ru_utime.tv_sec+(unsigned long long)ru.ru_stime.tv_sec;
 t=t*(unsigned long long)hz;
 t+=(unsigned long long)ru.ru_utime.tv_usec*(unsigned long long)hz/1000000ULL;
 t+=(unsigned long long)ru.ru_stime.tv_usec*(unsigned long long)hz/1000000ULL;
 return t;
}
/* 扫一次 /proc/self/task：每线程只 fopen/fread 一次 <tid>/stat；线程数上限 M40AH_TMAX，超出忽略。
   comm 在括号内可能含空格/换行 ⇒ strrchr 取最后一个 ')' 定位，其后 sscanf 跳 3..13 取 14/15。
   返回线程数；-1 = /proc 不可读（调用方打一行 err no-proc 后 return，不崩）。 */
static int m40ah_scan(m40ah_ent *out,int max)
{
 DIR *d=opendir("/proc/self/task"); if(!d)return -1;
 struct dirent *de; int n=0;
 while((de=readdir(d))!=NULL&&n<max){
  if(de->d_name[0]<'0'||de->d_name[0]>'9')continue;
  int tid=atoi(de->d_name); if(tid<=0)continue;
  char p[64],b[512];
  snprintf(p,sizeof p,"/proc/self/task/%d/stat",tid);
  FILE *f=fopen(p,"r"); if(!f)continue;
  size_t r=fread(b,1,sizeof b-1,f); fclose(f); b[r]=0;
  char *op=strchr(b,'('); char *lp=strrchr(b,')');
  if(!op||!lp||lp<=op)continue;
  char st=0; unsigned long ut=0,stt=0;
  if(sscanf(lp+1," %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",&st,&ut,&stt)!=3)continue;
  size_t cl=(size_t)(lp-op-1); if(cl>=sizeof out[0].comm)cl=sizeof out[0].comm-1;
  memcpy(out[n].comm,op+1,cl); out[n].comm[cl]=0;
  for(size_t i=0;i<cl;i++) if((unsigned char)out[n].comm[i]<0x20)out[n].comm[i]='_';
  out[n].tid=tid;out[n].u=(unsigned long long)ut;out[n].s=(unsigned long long)stt;out[n].st=st;
  n++;
 }
 closedir(d);
 return n;
}
void nvn_vk_m40ah_poll(unsigned tsec,unsigned long frames)
{
 static long last;static unsigned long f0;static unsigned long long i0,gut0;
 static m40ah_ent pv[M40AH_TMAX];static int pvn;static int err_once;
 if(!m40ah_on()){pvn=0;last=0;return;}   /* 关态：只读开关文件（2 s 缓存），不扫 /proc、零输出 */
 long now=(long)time(NULL);
 if(!last){                              /* 首调用：只建基准 + 一行 on=1 */
  int nc; last=now; f0=frames; gut0=m40ah_gut();
  i0=(unsigned long long)atomic_load_explicit(&g_beat_img,memory_order_relaxed);
  nc=m40ah_scan(pv,M40AH_TMAX);
  if(nc<0){pvn=0;last=0;if(!err_once){err_once=1;vk_boot_logf("[m40ah] err no-proc t=%us",tsec);}return;}
  pvn=nc;
  vk_boot_logf("[m40ah] on=1 t=%us frames=%lu imgs=%llu gut=%llu nthread=%d",tsec,frames,i0,gut0,pvn);
  return;}
 {unsigned dtw=(unsigned)(now-last);
  if(dtw<20)return;                     /* 采样周期 20 s（挂心跳 ⇒ 渲染卡死照打） */
  {m40ah_ent cur[M40AH_TMAX]; int nc=m40ah_scan(cur,M40AH_TMAX);
   if(nc<0){if(!err_once){err_once=1;vk_boot_logf("[m40ah] err no-proc t=%us",tsec);}return;}
   long hz=sysconf(_SC_CLK_TCK); if(hz<=0)hz=100;
   unsigned long long imgs=(unsigned long long)atomic_load_explicit(&g_beat_img,memory_order_relaxed);
   unsigned long long gut=m40ah_gut();
   double dt=(double)dtw*(double)hz;
   unsigned cpu=(unsigned)(dt>0.0?(double)(gut-gut0)*100.0/dt:0.0);
   /* top3：按本窗口 Δ(utime+stime) 降序（并列保持扫描序，结果稳定可 diff） */
   int bt[3]={-1,-1,-1}; double bc[3]={-1.0,-1.0,-1.0};
   char bcm[3][32]={{0},{0},{0}}; char bst[3]={'-','-','-'};
   for(int i=0;i<nc;i++){
    unsigned long long ptu=0; int hit=0;
    for(int j=0;j<pvn;j++) if(pv[j].tid==cur[i].tid){ptu=pv[j].u+pv[j].s;hit=1;break;}
    unsigned long long ctu=cur[i].u+cur[i].s;
    unsigned long long d=(hit&&ctu>ptu)?ctu-ptu:0;
    double cp=dt>0.0?(double)d*100.0/dt:0.0;
    if(cp<=bc[2])continue;
    int k=2;
    while(k>0&&bc[k-1]<cp){bc[k]=bc[k-1];bt[k]=bt[k-1];bst[k]=bst[k-1];
     snprintf(bcm[k],sizeof bcm[k],"%s",bcm[k-1]);k--;}
    bc[k]=cp;bt[k]=cur[i].tid;bst[k]=cur[i].st;
    snprintf(bcm[k],sizeof bcm[k],"%s",cur[i].comm);
   }
   vk_boot_logf("[m40ah] st t=%us dtw=%u frames=%lu dframes=%u dimg=%llu cpu=%u gut=%llu t1=%d/%s/%c/%u t2=%d/%s/%c/%u t3=%d/%s/%c/%u",
    tsec,dtw,frames,(unsigned)(frames-f0),imgs-i0,cpu,gut,
    bt[0],bcm[0],bst[0],(unsigned)(bc[0]>0.0?bc[0]:0.0),
    bt[1],bcm[1],bst[1],(unsigned)(bc[1]>0.0?bc[1]:0.0),
    bt[2],bcm[2],bst[2],(unsigned)(bc[2]>0.0?bc[2]:0.0));
   f0=frames;i0=imgs;gut0=gut;pvn=nc;
   for(int i=0;i<nc;i++)pv[i]=cur[i];}
  last=now;}
}
/* ================= M40AI：建管线串行化开关 smo_m40ai_lock + 探针开关 smo_m40ai =================
   背景（M40AH 实测）：t≈150–200 冻结窗口里 3–4 个线程各 ~80% CPU 在 SwiftShader JIT 编译，
   崩点落在 .smo_jit_* 匿名可执行页。要判的是两件事之一：
     ① 并发 JIT 竞态（多线程同时往同一代码池追加/刷指令缓存）；
     ② 可执行内存耗尽（代码池只增不减，JIT 出来的页把可执行额度吃完）。
   实验手段 = smo_m40ai_lock：
     1 ⇒ 唯一建管线入口 m40ac_create_pipe()（nvn_vk.c）整段用 g_m40ai_mtx 串行，同一时刻只一个 JIT；
     0 ⇒ 逐字节原行为（入口里只多两次 clock_gettime 记账，无锁）。
   smo_m40ai：1 ⇒ 每 20 s 一行 [m40ai] st（dcreate/dms/avg_ms/max_ms/dsh_*），默认 0 ⇒ 零输出。
   两条开关读写口径与 m40ag/m40ah 完全一致：seeder 只种 A 视图（构造期 app 唯一能 open 的），
   读 B 视图优先（hdc 唯一可写）+ A 兜底；2 s 缓存，热路径不反复 fopen。 */
#define M40AI_SW_B  "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40ai_lock"
#define M40AI_SW_A  "/data/storage/el2/base/files/smo_m40ai_lock"
#define M40AI_LOG_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40ai"
#define M40AI_LOG_A "/data/storage/el2/base/files/smo_m40ai"
int smo_m40ai_lock(void)   /* 0 = 不串行（缺省）；1 = 建管线入口全程串行 */
{
 static int cached=-1;static long last;long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40AI_SW_B,M40AI_SW_A};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached&1;
}
int m40ai_on(void)   /* [m40ai] 探针开关：0 = 零输出（缺省）；1 = 每 20 s 一行 */
{
 static int cached=-1;static long last;long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40AI_LOG_B,M40AI_LOG_A};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached&1;
}
/* seeder：必须种 A 视图（构造期 app 在 B 视图 open(O_CREAT) 被 SELinux 拒；A/B 同一物理目录，
   证据同 m40ag：app 写 A 视图 vk_boot.log，hdc 在 B 视图读到同一份）⇒ 种 A 后 hdc 即可写 B。 */
__attribute__((constructor)) static void m40ai_seed(void)
{
 static const char *const p[4]={M40AI_SW_A,M40AI_LOG_A,M40AI_SW_B,M40AI_LOG_B};
 for(int i=0;i<4;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}
/* ================= M40AJ：[m40aj] 探针开关 smo_m40aj =================
   M40AI 已证：冻结期进程 CPU≈348%（3.5 核）而建管线累计只占 ~0.27 核 ⇒ 冻结大头不在建管线。
   smo_m40aj：1 ⇒ 每 20 s 一行 [m40aj] st（dimg/img_ms/dup/up_ms/up_MB/dcreate/cp_ms/rest_ms），
   把冻结期 CPU 拆成「建图 / 上传 / 建管线 / 剩余（引擎线程）」。默认 0 ⇒ 零输出。
   读写口径与 m40ag/m40ah/m40ai 完全一致：seeder 只种 A 视图（构造期 app 唯一能 open 的），
   读 B 视图优先（hdc 唯一可写）+ A 兜底；2 s 缓存，热路径不反复 fopen。 */
#define M40AJ_LOG_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40aj"
#define M40AJ_LOG_A "/data/storage/el2/base/files/smo_m40aj"
int m40aj_on(void)   /* [m40aj] 探针开关：0 = 零输出（缺省）；1 = 每 20 s 一行 */
{
 static int cached=-1;static long last;long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40AJ_LOG_B,M40AJ_LOG_A};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached&1;
}
/* seeder：必须种 A 视图（构造期 app 在 B 视图 open(O_CREAT) 被 SELinux 拒；A/B 同一物理目录，
   证据同 m40ag/m40ai：app 写 A 视图 vk_boot.log，hdc 在 B 视图读到同一份）⇒ 种 A 后 hdc 即可写 B。 */
__attribute__((constructor)) static void m40aj_seed(void)
{
 static const char *const p[2]={M40AJ_LOG_A,M40AJ_LOG_B};
 for(int i=0;i<2;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}
/* ================= M40AL：[m40al] 探针开关 smo_m40al =================
   M40AJ 已证冻结窗 rest_ms=95.25%（Vulkan 三件套 <5%），M40AK 已证 Thread<00..03> 是 SwiftShader 的
   marl worker 在跑软光栅 ⇒ 剩下的问题是「那 95% 是否卡在栅栏/flush 等待」+「每帧交多少几何」。
   smo_m40al：1 ⇒ 每 20 s 一行 [m40al] st（draws/verts/vpd/wait_n/wait_ms/frames），
   其中 wait_ms 直接与同窗 [m40aj] 的 rest_ms 对比判读。默认 0 ⇒ 零输出。
   读写口径与 m40ag/m40ah/m40ai/m40aj 完全一致：seeder 只种 A 视图（构造期 app 唯一能 open 的），
   读 B 视图优先（hdc 唯一可写）+ A 兜底；2 s 缓存，热路径不反复 fopen。 */
#define M40AL_LOG_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40al"
#define M40AL_LOG_A "/data/storage/el2/base/files/smo_m40al"
int m40al_on(void)   /* [m40al] 探针开关：0 = 零输出（缺省）；1 = 每 20 s 一行 */
{
 static int cached=-1;static long last;long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40AL_LOG_B,M40AL_LOG_A};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached&1;
}
/* seeder：必须种 A 视图（构造期 app 在 B 视图 open(O_CREAT) 被 SELinux 拒；A/B 同一物理目录，
   证据同 m40ag/m40ai/m40aj：app 写 A 视图 vk_boot.log，hdc 在 B 视图读到同一份）⇒ 种 A 后 hdc 即可写 B。 */
__attribute__((constructor)) static void m40al_seed(void)
{
 static const char *const p[2]={M40AL_LOG_A,M40AL_LOG_B};
 for(int i=0;i<2;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}
/* ================= M40AM：提交频次探针开关 smo_m40am =================
   读写口径与 m40ag/m40ah/m40ai/m40aj/m40al 完全一致：seeder 只种 A 视图，读 B 优先 + A 兜底，2 s 缓存。
   默认 1（本单必须拿到数据；关态只回 0 即可）。 */
#define M40AM_LOG_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40am"
#define M40AM_LOG_A "/data/storage/el2/base/files/smo_m40am"
int m40am_on(void)
{
 static int cached=-1;static long last;long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40AM_LOG_B,M40AM_LOG_A};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached&1;
}
__attribute__((constructor)) static void m40am_seed(void)
{
 static const char *const p[2]={M40AM_LOG_A,M40AM_LOG_B};
 for(int i=0;i<2;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"1\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}
/* ================= M40AO：GPU 工作量归属探针开关 smo_m40ao =================
   M40AN 已证 draw_wait_us 与 draw 数/submit 数都无关（三配置都≈40–49 s/60 s 窗）⇒ 每帧烧的是一批
   固定非-draw 工作。本开关只控「每 20 s 一行 [m40ao]」的输出，计数器由 nvn_vk.c 的宏旁路无条件累加。
   读写口径与 m40ag/m40ah/m40ai/m40aj/m40al/m40am 完全一致：seeder 只种 A 视图，读 B 优先 + A 兜底，2 s 缓存。
   默认 1（本单必须拿到数据；关态只回 0 即可）。 */
#define M40AO_LOG_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40ao"
#define M40AO_LOG_A "/data/storage/el2/base/files/smo_m40ao"
int m40ao_on(void)
{
 static int cached=-1;static long last;long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40AO_LOG_B,M40AO_LOG_A};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached&1;
}
__attribute__((constructor)) static void m40ao_seed(void)
{
 static const char *const p[2]={M40AO_LOG_A,M40AO_LOG_B};
 for(int i=0;i<2;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"1\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}
/* ================= M40AR：宿主兜底图也按除数缩 开关 smo_m40ar =================
   M40AP 已证 1280x720 宿主兜底图（H_OFF_W/H）在 div=8 时占 renderArea 面积 ~90%，是「面积律」里
   **唯一没被 h_rs_dim() 除过**的一块；M40AQ 的 0.6 fps 阶段（frame_us=1 666 666）高度疑似全压在它上面
   （0.92 MPx/通道，134 MPx/s ⇒ 1.4 s/帧）。
   smo_m40ar：1 ⇒ 该图按 h_rs_dim(H_OFF_W/H) 建/绑/视口/渲染区（div=4 ⇒ 320x180）；
              0 ⇒ 所有落点取回 H_OFF_W/H 原值（**关态逐字不变**）。
   读写口径与 m40ag/m40ah/m40ai/m40aj/m40al/m40am/m40ao 完全一致：seeder 只种 A 视图，
   读 B 优先 + A 兜底，2 s 缓存。默认 **0**（本开关会改渲染尺寸，必须显式打开）。 */
#define M40AR_LOG_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40ar"
#define M40AR_LOG_A "/data/storage/el2/base/files/smo_m40ar"
int m40ar_on(void)
{
 static int cached=-1;static long last;long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40AR_LOG_B,M40AR_LOG_A};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached&1;
}
__attribute__((constructor)) static void m40ar_seed(void)
{
 static const char *const p[2]={M40AR_LOG_A,M40AR_LOG_B};
 for(int i=0;i<2;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}
/* ================= M40AT：强制每帧上屏 开关 smo_m40at =================
   M40AR/as_c 事实：真实帧连续 22 张同 md5 达 84 s，同期 frames +1208（14.4 f/s）；
   `[p] sig` 的 staging 内容指纹（mean/nz/spread）连续 135 次上屏完全相同 ⇒ 冻结点在「上屏源的刷新」
   或「上屏节流」，不在渲染。本开关只控 nvn_vk_present_offscreen 的两件事：
     at=1 ⇒ ①绕过 present_every() 降采样门（默认 8 ⇒ 只有 1/8 次 present 真拷上屏），每次 present
              都走完整上屏（copy → win_fill）；②上屏源改用 p_auto_select() 当帧选中的活图
              （不再吃 M7i 常驻快照；p_auto_select 内部规则一字不动，只是 at=1 时才去问它）。
     at=0 ⇒ 两件事都不做（逐字不变：同一套门、同一套选源）。
   读写口径与 m40ag/m40ah/m40ai/m40aj/m40al/m40am/m40ao/m40ar 完全一致：seeder 只种 A 视图，
   读 B 优先 + A 兜底，2 s 缓存。默认 **0**（本开关把上屏频率抬 8 倍，必须显式打开）。 */
#define M40AT_LOG_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m40at"
#define M40AT_LOG_A "/data/storage/el2/base/files/smo_m40at"
int m40at_on(void)
{
 static int cached=-1;static long last;long now=(long)time(NULL);
 if(cached>=0&&now<last+2)return cached;
 last=now;cached=0;
 {const char *paths[2]={M40AT_LOG_B,M40AT_LOG_A};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n>0){b[n]=0;cached=atoi(b);}break;}}}
 return cached&1;
}
__attribute__((constructor)) static void m40at_seed(void)
{
 static const char *const p[2]={M40AT_LOG_A,M40AT_LOG_B};
 for(int i=0;i<2;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}
__attribute__((constructor)) static void m40ae_seed(void)
{
 for(int i=0;i<TEXREG_N;i++)g_m40ae_eid[i]=-1;   /* M40AF ①：零初始化会误指条目 0 ⇒ 必须显式 -1 */
 static const char *const p[2]={M40AE_SW_A,M40AE_SW_B};
 for(int i=0;i<4;i++){
  if(access(p[i],F_OK)==0){chmod(p[i],0666);continue;}
  int fd=open(p[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(p[i],0666);}
 }}
static int g_rp_bind_tex(int stage,int first,int n,const uint64_t *h,void *u){
 (void)u;(void)stage;
 m30a_q_tick();   /* M30A：smo_m30a_q bit0 时把 id-image 名额抬到 1024（关态恒 384，无副作用） */
 m103a_reclaim(); /* M103A P1：名额/活体字节越顶时先回收（关态第一行就 return，零副作用） */
 m40q_stat();     /* M40Q：每 60 s 一行 [m40q] st（仅 smo_m40q 开态；关态第一行就 return，基线日志不变） */
 if(!h||n<=0)return 0;
 nvn_vk_tex *res[TX_SLOT_MAX];memset(res,0,sizeof res);int nres=0,changed=0;
 void *repl[TX_SLOT_MAX];memset(repl,0,sizeof repl);int rids[TX_SLOT_MAX];int nrepl=0;
 unsigned long long rstore[TX_SLOT_MAX];memset(rstore,0,sizeof rstore);   /* M4w P1：store 路命中的池地址 */
 int m36a_slot[TX_SLOT_MAX];   /* M36A：本槽命中的 RT 槽号（-1 = 没有），只供取证行 */
 for(int i=0;i<TX_SLOT_MAX;i++){rids[i]=-1;m36a_slot[i]=-1;}
 for(int i=0;i<n&&i<TX_SLOT_MAX;i++){
  int slot=first+i;
  if(slot<0||slot>=TX_SLOT_MAX){g_t_noslot++;continue;}
  nres=i+1;
  uint64_t hh=h[i];int tid=(int)(hh&0xFFFFF),sid=(int)(hh>>20);
  /* M4z P2②：句柄里的采样器 id 第一次出现时把游戏给的 wrap/filter 落一行——
     「我们固定 REPEAT 是不是违背了游戏意图」由此直接可判，不再靠猜。 */
  if(sid>=0&&sid<SAMP_REG_N&&g_samp_reg[sid].ok){
   static unsigned char seen[SAMP_REG_N/8];
   if(!(seen[sid>>3]&(1u<<(sid&7)))&&g_t_samp_logged<12){
    seen[sid>>3]|=(unsigned char)(1u<<(sid&7));g_t_samp_logged++;
    vk_boot_logf("[t] samp slot=%d id=%d sid=%d wrap=%d/%d/%d minmag=%d/%d lod=[%g,%g] (NVN 0=CLAMP 1=REPEAT)",
     slot,tid,sid,g_samp_reg[sid].ws,g_samp_reg[sid].wt,g_samp_reg[sid].wr,
     g_samp_reg[sid].minf,g_samp_reg[sid].magf,g_samp_reg[sid].lod_min,g_samp_reg[sid].lod_max);}}
  /* M4aa P1：**接线**——把该 sid 的真实 wrap/filter/lod 送进 nvn_vk，由它按组合选 VkSampler。
     未登记（ok=0）送 -1 ⇒ nvn_vk 保持旧的 NEAREST/REPEAT/maxLod=0 并计入 unreg。
     放在 changed 早退**之前**：纹理没换但采样器变了也必须生效（采样器是静态描述符，见 r_ds_tex_sync）。 */
  if(sid>=0&&sid<SAMP_REG_N&&g_samp_reg[sid].ok)
   nvn_vk_texture_slot_sampler(slot,sid,g_samp_reg[sid].ws,g_samp_reg[sid].wt,g_samp_reg[sid].wr,
    g_samp_reg[sid].minf,g_samp_reg[sid].magf,g_samp_reg[sid].lod_min,g_samp_reg[sid].lod_max);
  else
   nvn_vk_texture_slot_sampler(slot,sid<0?0:sid,-1,-1,-1,0,0,0.f,0.f);
  if(tid>=0&&tid<TEXREG_N)g_t_idbinds[tid]++;   /* M4t P2：按 id 的绑定热度（never-up top20 的分母） */
  const texsnap *sn=texreg_lookup(tid);
  g_scan_clock++;
  if(tid>=0&&tid<TEXREG_N)tex_scan_near(tid,sn,g_t_idbinds[tid]);   /* M4ai P1：±64 KB 存储基址判决扫描（每 id 2 次） */
  /* M4v P1：观察名单里的 id，**首次被绑**就先取一次证（route②/未上送的也能看到池里到底是什么） */
  {int wi=tex_watch_idx(tid);
   if(wi>=0&&!g_tp_seen[wi]&&sn&&sn->mem){g_tp_seen[wi]=1;tex_probe(tid,sn,"bind1");}}
  m7l_bind_probe(tid,sn);   /* M7l P1：零内容 id 绑定时刻源字节复查（每 id 一次，预算 16 行） */
  nvn_vk_tex *vk=NULL;
  int kind=nvn_vk_rt_tex_kind(tid);   /* M4e：该 id 是否就是我们画过的 RT（0/1=self/2=other） */
  /* M4w P1：id 路线不中（RT 槽 tex_id=-1 或槽 key 是对象指针）时，**按存储身份再查一次**。
     采样侧一定知道池地址（快照 mem），RT 侧 key 是同一块存储时一定知道 ⇒ 这条边精确且独立于 id。
     命中就与 id 路线同语义（kind=1/2），下面直接改绑 RT 图；命中时顺手把 id 补登到该 RT 槽，
     让后续 id 路线也能命中（rt_link 的 store 桶）。 */
  {unsigned long long st=(unsigned long long)(uintptr_t)(sn?sn->mem:NULL);
   if(!kind&&st&&sn){
    int rs=-1,rtid=-1;
    int k2=nvn_vk_rt_lookup_store(st,sn->d.width,sn->d.height,(int)sn->d.format,&rs,&rtid);
    if(k2){
     kind=k2;rstore[i]=st;g_t_rt2_hits++;m36a_slot[i]=rs;
     if(tid>=0&&tid<TEXREG_N){g_rt_id_seen[tid]=1;g_rt2_note_id(tid);if(!g_idup[tid])g_t_rt2_noup++;}
     if(rtid<0){nvn_vk_rt_link_id_by_store(st,tid);g_t_rt2_link++;}
     if(g_t_rt2_logged<12||(g_t_rt2_hits&8191)==0){g_t_rt2_logged++;
      vk_boot_logf("[t] bind slot=%d id=%d -> rt-by-store slot=#%d src=rt2 ext=%ux%u fmt=0x%x kind=%s n=%llu",
       slot,tid,rs,sn->d.width,sn->d.height,sn->d.format,k2==1?"self":"other",g_t_rt2_hits);}
    } else g_t_rt2_miss++;
   }}
  /* M6a A：**零内容**被采样纹理再按 RT 身份判一次。
     前两条路（id 链接 / store 精确）都空、而这张 id 我们上送过且首 4 KiB 全零 ⇒ 采到的必然是黑。
     此时若 RT 表里存在**唯一**一张 ext+fmt 完全相同的、已建图的 RT，就把 id 别名到它（nvn_vk 侧登记），
     采样直接绑我们渲染出来的 VkImage。多个同形候选 ⇒ 不猜（计数后留原来的黑图）。
     不许顶替：只有 ext+fmt 完全一致且唯一才算「同一张图」。 */
  {int zsrc=0,zslot=-1,ztexid=-1,zimg=0,zcand=0;
   if(!kind&&sn&&nvn_vk_is_up()&&tid>=0&&nvn_vk_tex_zero_id(tid)){
    kind=nvn_vk_rt_tex_kind_zero(tid,sn->d.width,sn->d.height,(int)sn->d.format,
                                 (unsigned long long)(uintptr_t)sn->mem,1,
                                 &zsrc,&zslot,&ztexid,&zimg,&zcand);
    if(kind){
     g_t_rt_zero_bound++;m36a_slot[i]=zslot;
     if(g_t_rt_zero_log<32||(g_t_rt_zero_bound%2048)==0){g_t_rt_zero_log++;
      vk_boot_logf("[tex] rt-bind id=%d rt=#%d/texid=%d fmt=0x%x ext=%ux%u cands=%d src=%s zero=1 n=%llu",
       tid,zslot,ztexid,sn->d.format,sn->d.width,sn->d.height,zcand,
       zsrc==1?"our_rt(id)":(zsrc==2?"our_rt(store)":(zsrc==3?"our_rt(extfmt)":"our_rt(order)")),g_t_rt_zero_bound);}
     /* M36A：命中处的逐槽取证（只 smo_m36a=1 时打；关态零新增输出）。 */
     if(m36a_on())m36a_rt_bind_line(tid,zslot,ztexid,sn,
       zsrc==1?"our_rt(id)":(zsrc==2?"our_rt(store)":(zsrc==3?"our_rt(extfmt)":"our_rt(order)")),zcand);
     /* M6b B：顺序证据消歧（多候选里本 submit 唯一写者）单独落一行，带 prev_pass/writers。 */
     if(kind&&zsrc==4&&g_t_rt_zero_order_log<24){g_t_rt_zero_order_log++;
      vk_boot_logf("[tex] rt-bind id=%d rt=#%d/texid=%d fmt=0x%x ext=%ux%u cands=%d src=our_rt(order) by=order prev_pass=#%d w=%u",
       tid,zslot,ztexid,sn->d.format,sn->d.width,sn->d.height,zcand,zslot,nvn_vk_rt_slot_writers(zslot));}
    } else {
     g_t_rt_zero_miss++;
     zs_note(sn->d.width,sn->d.height,(int)sn->d.format,zcand,zimg,tid);
     /* M36A：零内容但没 join 上的采样（只 smo_m36a=1）。两级预算：
        ① 同一块池存储**当时/后来确实是我们某张 RT** 的（最可疑：尺寸/格式守卫把它挡了）——独立预算 48 行；
        ② 其余（无同 store 槽）——前 64 个不同 id。 */
     if(m36a_on()){
      int sib=-1,sibtx=-1;
      int has_sib=(sn&&sn->mem)?nvn_vk_rt_store_sibling((unsigned long long)(uintptr_t)sn->mem,&sib,&sibtx):0;
      if(has_sib)m36a_ph_line("miss-sib",tid,sn,zcand);
      else if(m36a_first_bit(g_m36a_miss_bit,tid)&&g_m36a_miss_lines<64){
       g_m36a_miss_lines++;
       vk_boot_logf("[m36a] miss id=%d up=%d dims=%ux%u fmt=0x%x store=%#llx avail=%llu cands=%d "
                    "why=%s sib_slot=-1 s_layout=%d",
                    tid,(tid>=0&&tid<TEXREG_N)?(int)g_idup[tid]:0,sn?sn->d.width:0u,sn?sn->d.height:0u,
                    sn?(int)sn->d.format:0,(unsigned long long)(uintptr_t)(sn?sn->mem:NULL),
                    (unsigned long long)(sn?sn->avail:0ull),zcand,
                    zcand>1?"ambiguous_extfmt":"no_rt_of_this_shape",
                    5);   /* s_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL（本 TU 不含 vulkan.h，写常量） */
      }
     }
     if(tid<TEXREG_N){
      unsigned char *zb=&g_zmiss_bit[tid>>3],zm=(unsigned char)(1u<<(tid&7));
      if(!(*zb&zm)){*zb|=zm;g_t_zmiss_ids++;
       if(g_t_zmiss_ids<=96)
        vk_boot_logf("[tex] missing id=%d ext=%ux%u fmt=0x%x store=%#llx avail=%llu cand=%d cand_texid=%d cand_img=%d why=%s",
         tid,sn->d.width,sn->d.height,sn->d.format,(unsigned long long)(uintptr_t)sn->mem,
         (unsigned long long)sn->avail,zcand,ztexid,zimg,
         zcand>1?"ambiguous_extfmt":"no_rt_of_this_shape");}
     }
    }
   }
  }
  if(!kind)g_t_rt_miss++;             /* 未命中：两条路都不是任何 RT（含句柄解析不到） */
  /* M4u P2 取证：slot 16..31 过去整段被丢（noslot=327,770）。现在放行，打前 48 条说明
     「这些槽绑的是什么、解析成什么」。注意：baked shader 语料里 Maxwell 纹理槽最大只到 10
     （index.json 10351 条实测，slot==dense 且 ntex<=11），所以 16..31 的槽不会被任何 SPIR-V 采样，
     放行的意义是「不再丢绑定、口径归零」，不是「多出可见纹理」。 */
  if(slot>=16){g_t_slot_hi++;
   if(g_t_slot_hi<=48)vk_boot_logf("[t] bind slot=%d (hi) id=%d sid=%d kind=%s src=%s ext=%ux%u fmt=0x%x snap=%d",
    slot,tid,sid,kind?(kind==1?"rt-self":"rt-other"):"tex",
    kind?"rt-img":((sn&&sn->mem)?"id-img":"placeholder"),sn?sn->d.width:0u,sn?sn->d.height:0u,sn?sn->d.format:0,sn?1:0);}

  if(kind){
   rids[i]=tid;
   if(sn&&sn->mem){int fmt=sn->d.format;if(fmt>=0&&fmt<64)g_t_fmt_hist[fmt]++;else g_t_fmt_other++;}
   if(kind==1)g_t_rt_hit_self++;else g_t_rt_hit_other++;
   g_t_rt_hit_total++;
   nrepl++;
   /* M4t P1：**替换槽也要喂元信息**。旧实现在这里直接 continue，`nvn_vk_tex_slot_info()` 从没被
      调用 ⇒ `[fs] slot=0 … id=0 fmt=0 kind=none src=rt`（m4t_b1 实测），全屏取证行失去
      「采的是哪张图」这一半；反向登记也因此看不到这个槽。 */
   nvn_vk_tex_slot_info(slot, tid, sn?sn->d.width:0u, sn?sn->d.height:0u, sn?sn->d.format:0,
                        kind==1?NVN_VK_SLOT_RT_SELF:NVN_VK_SLOT_RT_OTHER,
                        (unsigned long long)(uintptr_t)(sn?sn->mem:NULL), (int)g_idup[tid]);
   nvn_vk_tex_slot_why(slot, 1, 1u, (unsigned long long)(sn?sn->avail:0ull));   /* M4bk P1：RT 槽有真图 */
   if(g_t_rt_logged<3||g_t_rt_hit_total%1000==0){
    g_t_rt_logged++;
    vk_boot_logf("[t] bind slot=%d id=%d -> rt-img src=rt ext=%ux%u kind=%s n=%llu",
                 slot,tid,sn?sn->d.width:0u,sn?sn->d.height:0u,kind==1?"self":"other",g_t_rt_hit_total);
   }
   continue;   /* 供源已是我们的图：不再走 g_idvk/上传（池内容恒 0，上送了也无意义） */
  }
  if(sn&&sn->mem){
   int fmt=sn->d.format;
   int big=0;   /* M4as P2：本槽这次是不是走「大图独立配额」（建图与上送必须同一口径） */
   if(fmt>=0&&fmt<64)g_t_fmt_hist[fmt]++;else g_t_fmt_other++;
   /* M4bk P1（只读取证）：本槽「为什么没有真数据」的归因码，喂给 nvn_vk 的 [m] sentinel 行。
      不改任何判决，只在每条早退分支上留一个原码。 */
   if(tid>=0&&tid<TEXREG_N&&g_t_idwhy[tid]!=1)g_t_idwhy[tid]=0;
   /* M3y P2 第二版：优先用 id 自建图（与快照同源）；建不起来退回侧表图（指针命中时仍有效）。 */
   if(tid>=0&&tid<TEXREG_N)g_id_lru[tid]=++g_id_lru_seq;   /* M5z P1：LRU 打点（每次绑定都算一次使用） */
   vk=g_idvk[tid];
   /* M40AE ①：同一块客体存储已经建过图 ⇒ 只做别名（不建图、不占名额、不整图重传）。 */
   if(!vk&&tid>=0&&tid<TEXREG_N&&sn->mem&&m40ae_on()){
    int ae_e=m40ae_find((unsigned long long)(uintptr_t)sn->mem,&sn->d);
    if(ae_e>=0&&m40ae_alias(tid,ae_e)){
     m40ae_img *ae=&g_m40ae_img[ae_e];
     vk=g_idvk[tid]=ae->vk;
     g_idup[tid]=(unsigned char)(ae->up?1:0);      /* ④ 继承图的已上送态 */
     g_tp_fp[tid]=ae->fp;
     if(ae->up)g_m40ae_skipup++;                    /* 这张图已上送 ⇒ 本别名不整图重传 */
    }
   }
   if(!vk)g_m40q_tex++;   /* M40Q：tex 带「此刻无图 ⇒ 走到建图判决」的次数（开关上下同口径，供 skip/tex） */
   {int m40q_skip=0;      /* M40Q：一次性 id（历史 bind ≤1）跳过建图。默认关 ⇒ 恒 0，关态逐字节原行为 */
    if(!vk&&(m40q_on()&1)&&tid>=0&&tid<TEXREG_N&&g_t_idbinds[tid]<=M40Q_THR){
     m40q_skip=1;g_m40q_skip++;
     if(tid>=0&&tid<TEXREG_N)g_t_idwhy[tid]=-40;   /* M40Q：跳过（一次性 id） */
     if(g_m40q_skip<=24)vk_boot_logf("[m40q] skip id=%d binds=%llu avail=%llu %ux%u fmt=0x%x idimg=%u/%u deny=%llu",
      tid,(unsigned long long)g_t_idbinds[tid],(unsigned long long)sn->avail,
      sn->d.width,sn->d.height,(unsigned)sn->d.format,g_t_idimg,(unsigned)k_id_img_max,
      (unsigned long long)g_z_evict_deny);}
   if(!vk&&!m40q_skip){
    /* M5z P1：名额满了先让位（LRU 驱逐一张无槽引用的旧图），再走原来的建图/门槛判定。 */
    int ev=0;
    /* M7k P1：小图（≤1MB）不受 binds>=8 让位门槛约束（首绑即让位）；其余条件不变。
       M7k P2（boot2）：小图走独立 submit 配额（t_evict_slot_small），不再与热 id 洪水抢 8 次/submit。 */
    int ev_small = M7K_SMALL_EVICT_ON&&sn&&sn->avail&&sn->avail<=(1ull<<20)&&tid>=0&&tid<TEXREG_N;
    /* M103A P2（2026-11-13）：压顶判据 = 名额满 **或** 活体字节顶，并且**只有真压顶才求值配额**
       （旧代码无条件求值 ev_sm_ok ⇒ 每笔小图 bind 吃一次配额，deny_s 涨的是记账假象）。
       开态下任何 id 都能让位（LRU 缓存语义），不再要求 binds>=8 或 avail<=1MB —— 3D 段新到的
       id（2952/2953/2954 那批 1280×160 fmt=0x25）正是被这两条门槛一起挡死的。 */
    int press = (tid>=0&&tid<TEXREG_N&&nvn_vk_is_up())?m103a_pressure(sn?sn->avail:0ull):0;
    if(press&&(((g_t_idbinds[tid]>=M5Z_EVICT_MIN_BINDS)&&t_evict_slot())
               ||(ev_small&&t_evict_slot_small())
               ||(m103a_on()&&t_evict_slot_small()))){
     unsigned long long was=g_z_evict;
     t_evict_one(tid);
     ev=(g_z_evict!=was);
     if(ev&&m103a_on())g_m103a_evict++;
     if(ev&&ev_small&&g_t_idimg<k_id_img_max){g_z_evict_small++;
      if(g_z_evict_small<=16)vk_boot_logf("[m7k] small evict id=%d avail=%llu binds=%llu idimg=%u/%u live=%lluMB deny=%llu deny_s=%llu",
        tid,(unsigned long long)sn->avail,(unsigned long long)g_t_idbinds[tid],
        g_t_idimg,(unsigned)k_id_img_max,(unsigned long long)(g_t_bytes_live>>20),
        (unsigned long long)g_z_evict_deny,(unsigned long long)g_z_evict_deny_s);}
     else if(ev){static unsigned long long m103a_ev_lines;
      if(m103a_ev_lines<16){m103a_ev_lines++;
       vk_boot_logf("[m103a] evict id=%d avail=%llu binds=%llu idimg=%u/%u live=%lluMB/%lluMB deny=%llu deny_s=%llu",
        tid,(unsigned long long)sn->avail,(unsigned long long)g_t_idbinds[tid],
        g_t_idimg,(unsigned)k_id_img_max,(unsigned long long)(g_t_bytes_live>>20),
        (unsigned long long)(M103A_BYTES_MAX>>20),
        (unsigned long long)g_z_evict_deny,(unsigned long long)g_z_evict_deny_s);}}
    }
    if(g_t_idimg<k_id_img_max&&nvn_vk_is_up()&&t_idimg_bytes_ok(sn?sn->avail:0ull)){
     if(!tex_up_gate_ok(tid,sn,&big)){if(tid>=0&&tid<TEXREG_N)g_t_idwhy[tid]=-30;t_starve_note(tid,sn->avail,0);if(m34a_on())m34a_fam_add(tid,sn);}   /* M4u P1b/M4as P2：冷 id 让位，但大图有独立配额。M34A：这一支就是「被拒族」的诞生点 ⇒ 按池地址登记进动态族表（关态不执行） */
     else {vk=nvn_vk_tex_create(&sn->d);
      if(vk){g_idvk[tid]=vk;g_t_idimg++;g_idup[tid]=0;
       /* M40AE ①：登记进共享表（refs=1）。表满或 mem==0 时不登记 ⇒ 这张图退回逐 id 语义。 */
       if(m40ae_on()&&tid>=0&&tid<TEXREG_N&&sn->mem)
        m40ae_reg(tid,(unsigned long long)(uintptr_t)sn->mem,(unsigned)sn->d.width,(unsigned)sn->d.height,(unsigned)fmt,vk);
       nvn_vk_m40p_idrep(tid,(unsigned)sn->d.width,(unsigned)sn->d.height,(unsigned)sn->d.format);   /* M40P：同 id 重复建图计数 */
       m40ad_img(tid,(unsigned long long)(uintptr_t)sn->mem,(unsigned)sn->d.width,(unsigned)sn->d.height,(unsigned)fmt);   /* M40AD：同存储是否换 id（关态零副作用） */
       if(ev)t_fix_note(tid);   /* 这张图是"换出来的" ⇒ 没有本次改动该槽此刻会是白占位 */
       if(g_t_idimg<=8)vk_boot_logf("[t] id img #%llu id=%d %ux%u fmt=0x%x",g_t_idimg,tid,sn->d.width,sn->d.height,fmt);}
      else {g_t_idimg_cap++;if(tid>=0&&tid<TEXREG_N)g_t_idwhy[tid]=-32;}}
    } else {g_t_idimg_cap++;t_starve_note(tid,sn->avail,1);
            /* M103A：区分「名额满」(-31) 与「活体字节顶」(-36) —— 两个都必须能单独读出来。 */
            if(tid>=0&&tid<TEXREG_N&&nvn_vk_is_up()){
             if(g_t_idimg<k_id_img_max&&!t_idimg_bytes_ok(sn?sn->avail:0ull)){g_t_idwhy[tid]=-36;g_m103a_deny_bytes++;}
             else g_t_idwhy[tid]=-31;}
            else if(tid>=0&&tid<TEXREG_N)g_t_idwhy[tid]=-33;}
   }
   }   /* M40Q：m40q_skip 作用域结束（跳过时上面整段让位/建图都没跑） */
   if(vk&&!nvn_vk_tex_matches_desc(vk,&sn->d)){vk=NULL;g_t_mism++;if(tid>=0&&tid<TEXREG_N)g_t_idwhy[tid]=-34;}   /* 理论不发生：自建图即按快照建 */
   if(vk){
    int idx=tid;   /* 上传标记按 id（自建图） */
    /* M4d P2：绑定判据留证——slot=NVN first+i，id=句柄低 20 位，图来自 id 自建快照图 g_idvk[tid]。 */
    /* M4o P2：绑定留证从「首 96 条」放宽到「首 96 条 + 每个槽 id 变化时一条」。
       m4n_b1 的整屏白那笔（id=1910）发生在 bind #64x，旧口径根本打不到。
       上限 512 条，长跑不刷屏。 */
    static int s_bind_last_id[TX_SLOT_MAX];
    static unsigned s_bind_lines;
    int bind_line = (g_t_binds<96) || (s_bind_last_id[slot]!=tid);
    if(bind_line && s_bind_lines<512){
     s_bind_lines++;s_bind_last_id[slot]=tid;
     vk_boot_logf("[t] bind slot=%d -> id=%d src=%s %ux%u fmt=0x%x fl=%#x up=%d ntex=%d",
                  slot,tid,g_idvk[tid]==vk?"id-img":"side",sn->d.width,sn->d.height,fmt,sn->d.flags,(int)g_idup[tid],n);
    }
    /* M40AE ④（本条最容易出错）：上传门控**挂到图上**，不再只看本 id。
       口径：up_eff = 所属图的 up（共享别名）或原 g_idup（非共享 id）。
       - 图为空(0) ⇒ 谁先来谁传整图，传完置 e->up=1（所有别名一起看到）；
       - 图已上送(1) ⇒ 别名一律不做整图上传，只走下面的指纹重传检查（内容变了才补传）。
       这样既不会「N 个别名各传一次」（比不共享更慢），也不会「只让第一个 id 传、其余留空」（花屏）。 */
    int ae_u=(idx>=0&&idx<TEXREG_N)?m40ae_up(idx):-1;
    int up_eff=(ae_u>=0)?ae_u:((idx>=0&&idx<TEXREG_N)?(int)g_idup[idx]:0);
    if(idx>=0&&idx<TEXREG_N&&!up_eff){
     uint64_t bytes=sn->avail;int ok=-9;
     nvn_vk_m40j_reg(tid,(uint64_t)(uintptr_t)sn->mem,(uint64_t)bytes);   /* M40J：登记该 id 的池区间 */
     { extern void nvn_vk_m40k_reg(int,uint64_t,uint64_t);
       nvn_vk_m40k_reg(tid,(uint64_t)(uintptr_t)sn->mem,(uint64_t)bytes); }   /* M40K：同一批区间进轮询表 */
     if(sn->d.samples>1)ok=-2;                                  /* MSAA：与 1-sample VkImage 不匹配，跳过 */
     else if(!bytes||bytes>=(32ull<<20))ok=-3;                  /* 空/超 32MB：跳过并计数（派活单口径） */
     else if(!tex_up_gate_ok(tid,sn,&big)){ok=-12;}             /* M4u P1b/M4as P2：预算按热度分配，大图走独立配额 */
     /* M5z P1：字节预算改判**活体**（驱逐会释放）。用累计账的话，LRU 一跑起来 1 GB 立刻被
        "历史上送量"占满，-4 把后续所有上送掐死 ⇒ 修了名额反而变黑。 */
     else if(g_t_up>=k_tex_up_max||(!big&&g_t_bytes_live+bytes>k_tex_up_bytes_max))ok=-4;
     else {unsigned char vp_owned=0;const void *vp_src=vol_probe_src(sn,tid,bytes,&vp_owned);   /* M4am P2：体纹理零内容探针（默认关） */
           runtime_thread_note_op(3);   /* M5b：纹理上送也是宿主重活，冻结时 op=3 说明卡在这笔上送里 */
           int ur=nvn_vk_tex_upload_storage(vk,vp_src,bytes,(uint32_t)tid);   /* M4a：绑定时带 tid 打内容签名 */
           if(vp_owned)free((void *)vp_src);
           /* M4p P2：负数=失败原因（-5 depth+stencil/-6 soft_bc/-7 布局/-8 几何/-9 malloc/-10 越界），
              原样进 why=，不再塌成 1/0 把原因丢掉。 */
           ok = ur==1 ? 1 : (ur<0 ? ur : 0);
           m40h_bind(idx,sn,ok);   /* M40H：首次 bind 时刻的内容快照（只读；bind_nz/up_nz 归因） */
           if(ok==1){g_idup[idx]=1;g_t_up++;g_t_bytes+=bytes;g_t_bytes_live+=bytes;   /* M5z P1：活体账（驱逐会减） */
            if(big){g_t_big_up++;g_t_big_bytes+=bytes;
             if(g_t_big_log<32){g_t_big_log++;
              vk_boot_logf("[t] big up #%llu id=%d %ux%u fmt=0x%x bytes=%llu big=%llu/%lluMB gate=heat",
                           g_t_big_up,tid,sn->d.width,sn->d.height,fmt,(unsigned long long)bytes,
                           (unsigned long long)(g_t_big_bytes>>20),(unsigned long long)(k_big_up_bytes_max>>20));}}
            g_tp_fp[idx]=tp_fingerprint((const uint8_t *)sn->mem,bytes);   /* M4v P2：记下上送时的源指纹 */
            m40ae_setup(idx,g_tp_fp[idx],bytes);   /* M40AE ④：整图已传 ⇒ 图上记 up/fp/bytes，所有别名共享 */
            m40ae_sync_ids(idx);                  /* ④ 所有别名一起看到 up/fp（不许各自再传整图） */
            m7l_dyn_enroll(tid,sn);   /* M7l P1：场景族出现后的首次上传先入动态名单（让下面 tex_probe 全量取证） */
            tex_probe(tid,sn,"up");                                        /* M4v P1：上送那一刻的全量取证 */
            m7l_up_probe(tid,sn);          /* M7l P1：源字节 nz 统计（在 tex_probe 之后，[m7l] 行紧跟 [tp1] 组） */
            if(g_t_up<=512)vk_boot_logf("[t] tex handle=%#llx %ux%u fmt=0x%x src=%p bytes=%u up=1",
                                        (unsigned long long)hh,sn->d.width,sn->d.height,fmt,(void*)sn->mem,(unsigned)bytes);
           }}
     if(ok!=1){vk=NULL;g_t_skip++;   /* 上送失败：槽回占位（不是「上送了但空」——计数区分） */
      if(idx>=0&&idx<TEXREG_N)g_t_idwhy[idx]=ok;   /* M4bk P1：原码留给 [m] sentinel 归因 */
      g_t_upfail_add(ok);            /* M4t P2：失败原因分桶（不再只有 why=0） */
      if(g_t_skip<=16)vk_boot_logf("[t] skip handle=%#llx %ux%u fmt=0x%x tgt=%d fl=%#x lv=%u smp=%d bytes=%llu why=%d",
                                   (unsigned long long)hh,sn->d.width,sn->d.height,fmt,sn->d.target,sn->d.flags,
                                   sn->d.levels,sn->d.samples,(unsigned long long)bytes,ok);}
     else if(idx>=0&&idx<TEXREG_N)g_t_idwhy[idx]=1;
     /* M30A 观察（只在 smo_m30a_q|b 任一打开时打）：**全屏 scratch 的命运**。
        靶心①要的是「id3237 是否变成 up1」⇒ 对 1600×900 未压缩张量，上送成功/被拒各记一行
        （每 id 每态最多一次，全 boot ≤24 行），并带上当场的名额/配额/饥饿计数。
        门不开时这整段不执行 ⇒ b0 日志与 M28A 逐字同形。 */
     if((m30a_sw(0)|m30a_sw(1))&&sn->d.width==1600&&sn->d.height==900&&
        !((fmt>=0x41&&fmt<=0x50)||(fmt>=0x79&&fmt<=0x94))){
      static unsigned char m30a_said[TEXREG_N];static unsigned m30a_n;
      unsigned bit=(ok==1)?1u:2u;
      if(idx>=0&&idx<TEXREG_N&&!(m30a_said[idx]&bit)&&m30a_n<24){
       m30a_said[idx]|=(unsigned char)bit;m30a_n++;
       vk_boot_logf("[m30a] scratch id=%d %ux%u fmt=0x%x bytes=%llu up=%d ok=%d big=%d idimg=%u/%u "
                    "heat_gate=%llu big_gate=%llu starve_cap=%llu/%lluMB starve_heat=%llu/%lluMB q=%d b=%d",
        idx,sn->d.width,sn->d.height,fmt,(unsigned long long)bytes,(int)g_idup[idx],ok,big,
        g_t_idimg,(unsigned)k_id_img_max,g_t_heat_gate,g_t_big_gate,
        g_t_starve_cap_ids,(unsigned long long)(g_t_starve_cap_bytes>>20),
        g_t_starve_heat_ids,(unsigned long long)(g_t_starve_heat_bytes>>20),m30a_sw(0),m30a_sw(1));}
     }
    } else if(idx>=0&&idx<TEXREG_N&&up_eff&&g_tp_reup_n[idx]<TP_REUP_MAX){
     /* M4v P2：已上送过 —— 但源可能在上送之后才被写（m4u_b2 里 id=1894 上送时整块 0 字节）。
        只做「4 个 64 B 窗口指纹」比较，变了就重传，最多 8 次/id。命中率高的纹理走不到这里时
        成本 = 4 次小窗口哈希（256 B）。 */
     uint64_t fp=tp_fingerprint((const uint8_t *)sn->mem,sn->avail);
     if(fp!=g_tp_fp[idx]){
      unsigned char vp_owned=0;const void *vp_src=vol_probe_src(sn,tid,sn->avail,&vp_owned);   /* M4am P2：同款探针（重传路径） */
      int ur=nvn_vk_tex_upload_storage(vk,vp_src,sn->avail,(uint32_t)tid);
      if(vp_owned)free((void *)vp_src);
      if(ur==1){g_tp_reup_n[idx]++;g_t_reup++;g_t_reup_bytes+=sn->avail;
       g_tp_fp[idx]=tp_fingerprint((const uint8_t *)sn->mem,sn->avail);
       m40ae_setup(idx,g_tp_fp[idx],sn->avail);   /* M40AE ④：补传后同样把指纹记到图上 */
       m40ae_sync_ids(idx);
       if(g_t_reup<=64)vk_boot_logf("[t] reup id=%d %ux%u fmt=0x%x n=%u bytes=%llu why=fp",
                                    tid,sn->d.width,sn->d.height,fmt,(int)g_tp_reup_n[idx],
                                    (unsigned long long)sn->avail);
       if(g_t_reup<=16)tex_probe(tid,sn,"reup");}
      else if(g_t_reup<=64)vk_boot_logf("[t] reup id=%d failed ur=%d",tid,ur);
     }
    } else g_t_unchg++;   /* 已上送且（指纹相同或重传次数用尽）：只重绑槽，不重传 */
    if(idx>=0&&idx<TEXREG_N){tex_probe_tick(g_t_idbinds[idx]);}   /* M4v P1：定时复查源字节 */
   } else if(!g_idvk[tid]) {g_t_skip++;if(g_t_skip<=16)vk_boot_logf("[t] skip handle=%#llx key=%p (no VkImage, id img cap)",(unsigned long long)hh,sn->key);}
  } else {
   g_t_unres++;   /* 池未登记 / 纹理无池内存：句柄无法还原 */
   if(reg_drop_test(tid))g_t_unres_dropped++;   /* M4u P1：其中多少是「注册期被丢过快照」的那一类 */
   if(g_t_unres<=16)vk_boot_logf("[t] miss handle=%#llx tid=%d sid=%d snap=%d regdrop=%d",(unsigned long long)hh,tid,sid,sn?1:0,reg_drop_test(tid));
  }
 res[i]=vk;
  /* M4n P3：把本槽的来源元信息交给 nvn_vk（只有本文件认得游戏纹理表）。全屏 draw 取证行要打
     id/ext/fmt/kind —— 这是「249 哪来的」里「采的是哪张图」那一半；另一半（载荷签名）由 nvn_vk 自己读。
     kind: rt-self/rt-other 走替换图；tex=id 自建快照图；depth-nr=深度纹理存在但没上送；
     placeholder=解析失败回落 1x1 白。 */
  {int dfmt=sn?sn->d.format:0;   /* M4p P2：0x31..0x36 = NVN 深度/模板族（nvn_vk.c fmt_of 同区间） */
   nvn_vk_tex_slot_info(slot, tid, sn?sn->d.width:0u, sn?sn->d.height:0u, sn?sn->d.format:0,
                        kind? (kind==1?NVN_VK_SLOT_RT_SELF:NVN_VK_SLOT_RT_OTHER)
                            : (vk?NVN_VK_SLOT_TEX
                                 : ((dfmt>=0x31&&dfmt<=0x36&&sn)?NVN_VK_SLOT_DEPTH_NR:NVN_VK_SLOT_PLACEHOLDER)),
                        (unsigned long long)(uintptr_t)(sn?sn->mem:NULL), (int)(tid>=0&&tid<TEXREG_N?g_idup[tid]:0));}   /* M4t P1：池地址 → 反向登记的精确判据 */
   /* M7k P2（只读）：PH 槽逐 id 归因行 —— 把「场景族贴图为什么 PH+up0」从计数推断变成 per-id 原文。
      每个命中 id 只打一次（bitmask 去重）；≤1MB 的小图单列 48 行预算（boot1 教训：48 行全被
      t≈31s 的 1600x900 RGBA8 scratch 族吃光，场景小图后到没有预算）。 */
   {static unsigned char m7k_seen[TEXREG_N/8];static unsigned m7k_lines,m7k_lines_small;
    int m7k_is_small = sn&&sn->avail&&sn->avail<=(1ull<<20);
    if(!kind&&!vk&&sn&&sn->mem&&tid>=0&&tid<TEXREG_N&&
       !(m7k_seen[tid>>3]&(1u<<(tid&7)))&&
       (m7k_lines<32||(m7k_is_small&&m7k_lines_small<48))){
     m7k_seen[tid>>3]|=(unsigned char)(1u<<(tid&7));
     if(m7k_is_small)m7k_lines_small++;else m7k_lines++;
     vk_boot_logf("[m7k] ph id=%d why=%d avail=%llu fmt=0x%x %ux%u smp=%d binds=%llu "
                  "idimg=%u/%u live=%lluMB cap=%llu deny=%llu deny_s=%llu evnoref=%llu unres=%llu is_up=%d",
                  tid,(int)g_t_idwhy[tid],(unsigned long long)sn->avail,(int)sn->d.format,
                  sn->d.width,sn->d.height,(int)sn->d.samples,
                  (unsigned long long)g_t_idbinds[tid],g_t_idimg,(unsigned)k_id_img_max,
                  (unsigned long long)(g_t_bytes_live>>20),
                  (unsigned long long)g_t_idimg_cap,(unsigned long long)g_z_evict_deny,
                  (unsigned long long)g_z_evict_deny_s,
                  (unsigned long long)g_z_evict_noslot,(unsigned long long)g_t_unres,nvn_vk_is_up());
    }}
   /* M4bk P1：把「为什么没数据」的归因码一起交给 nvn_vk（只供 [m] sentinel 行）。
      code 口径：RT 槽/已上送=1；无快照=-20；否则取本笔判决留下的原码（-2..-12/-30..-34）。
      快照缺失要单独判：`sn==NULL` 的一条也要有名字，否则归因塌成 unknown。 */
   {int c;
    if(kind) c = 1;
    else if(!sn||!sn->mem) c = -20;
    else if(vk) c = 1;
    else if(tid>=0&&tid<TEXREG_N&&g_t_idwhy[tid]) c = (int)g_t_idwhy[tid];
    else c = (sn&&sn->d.format>=0x31&&sn->d.format<=0x36) ? -5 : -35;
    nvn_vk_tex_slot_why(slot, c, (tid>=0&&tid<TEXREG_N)?g_t_idbinds[tid]:0u,
                        (unsigned long long)(sn?sn->avail:0ull));}
   /* M5z P0（只读）：本槽「空，但 id 有可用快照」⇒ 挡它的只有我们自己的配额/门槛。
      判据只用快照本身：有池地址、字节数在可上送区间、非 MSAA、非深度族、尺寸正常。
      与上送门无关（门槛/上限是"现在给不给"，这里是"给不给得起"）。 */
   {int fx=0;
    if(!kind&&!vk&&sn&&sn->mem){
     int dfmt=sn->d.format;
     if(sn->avail&&sn->avail<(32ull<<20)&&sn->d.samples<=1&&
        !(dfmt>=0x31&&dfmt<=0x36)&&sn->d.width&&sn->d.height&&
        sn->d.width<=16384&&sn->d.height<=16384) fx=1;
    }
    nvn_vk_tex_slot_fixable(slot, fx);}
   /* M5z P1（只读）：本槽现在有图、而且这张图是「LRU 让位换来的」⇒ 反事实口径下该槽此刻是白占位。
      只喂 [tex] bind fix 的 was=，不改任何判定。 */
   nvn_vk_tex_slot_fixsrc(slot, (vk&&tid>=0)?t_fix_saved(tid):0);
 }
 g_t_binds++;
 if(!nrepl){   /* 无替换：行为与 M4d 完全一致（含快速返回） */
  for(int i=0;i<nres;i++){int slot=first+i;if(slot<0||slot>=TX_SLOT_MAX)continue;
   if(!nvn_vk_texture_slot_matches(slot,res[i]))changed=1;}
  if(!changed)return 0;
  nvn_vk_rt_flush();     /* 先兑现 clear/RT 布局（同 :3653 顺序契约） */
  nvn_vk_draw_flush();   /* 批内已录 draw 用旧槽收口，之后切槽才不污染它们 */
  for(int i=0;i<nres;i++){int slot=first+i;if(slot<0||slot>=TX_SLOT_MAX)continue;nvn_vk_bind_texture_slot(slot,res[i]);}
  return 0;
 }
 /* M4e：有替换 ⇒ 先收口（副本/转布局都不能与在飞 CB 交叉），再准备替换图。
    替换图拿不到（建影子失败/节流后仍无）⇒ 该槽走原路径 res[i]（可能是 NULL=1x1 占位）并计 view_fail。 */
 nvn_vk_rt_flush();
 nvn_vk_draw_flush();
 for(int i=0;i<nres;i++){
  int slot=first+i;if(slot<0||slot>=TX_SLOT_MAX)continue;
  if(rids[i]>=0){
   int why=0;int m36a_lay0=-1;
   if(m36a_on()&&m36a_slot[i]>=0){
    unsigned long long a0=0,l0=0,f0=0,f1=0,k0=0;unsigned s0=0,w0=0,h0=0;int sl0=-1,hi0=0,cu0=0,sh0=-1;
    nvn_vk_rt_slot_m36a(m36a_slot[i],&a0,&s0,&l0,&f0,&f1,&m36a_lay0,&sl0,&w0,&h0,&k0,&hi0,&cu0,&sh0);
   }
   void *v=rstore[i]?nvn_vk_rt_sample_prepare_store(rstore[i],&why)
                           :nvn_vk_rt_sample_prepare(rids[i],&why);
   if(v)repl[i]=v;
   else {g_t_rt_view_fail++;if(g_t_rt_view_fail<=8)vk_boot_logf("[t] rt-img unavailable slot=%d id=%d why=%d -> fallback",slot,rids[i],why);}
   /* M36A：第二读数 —— prepare 之后槽的真实布局（lay1）。lay1==5(SRO) 且 why==0 ⇒ 布局确实转过；
      why<0 ⇒ 转布局/取 view 失败（这才是「layout 没 transition」）；关态零输出。 */
   if(m36a_on()&&m36a_slot[i]>=0){
    unsigned long long wa=0,lu=0,lwf=0,lbf=0,key=0;unsigned ws=0,w=0,h=0;
    int lay1=-1,sl=-1,hi=0,cur=0,shl=-1;
    nvn_vk_rt_slot_m36a(m36a_slot[i],&wa,&ws,&lu,&lwf,&lbf,&lay1,&sl,&w,&h,&key,&hi,&cur,&shl);
    vk_boot_logf("[m36a] prep id=%d rt_slot=#%d why=%d lay0=%d lay1=%d s_layout=%d view=%d cur=%d",
                 rids[i],m36a_slot[i],why,m36a_lay0,lay1,sl,v?1:0,cur);
    if(v&&g_m36a_rb_lines<12){
     unsigned long long mr=0,mg=0,mb=0;unsigned nz=0,col=0,dist=0,px0[4]={0,0,0,0};int lag1=0;
     if(nvn_vk_rt_slot_content_m36a(m36a_slot[i],&mr,&mg,&mb,&nz,&col,&dist,&lag1,px0)){
      g_m36a_rb_lines++;
      vk_boot_logf("[m36a] rt-rb id=%d rt_slot=#%d dims=%ux%u lay=%d mean=%llu/%llu/%llu nz=%u%% "
                   "colored=%u%% distinct=%u lag1x100=%d px0=%u,%u,%u,%u",
                   rids[i],m36a_slot[i],w,h,lay1,mr,mg,mb,nz,col,dist,lag1,px0[0],px0[1],px0[2],px0[3]);
     }
    }
   }
  }
  void *want=repl[i]?repl[i]:(void *)0;
  if(want){if(!nvn_vk_texture_slot_matches_view(slot,want))changed=1;}
  else if(!nvn_vk_texture_slot_matches(slot,res[i]))changed=1;
 }
 if(!changed)return 0;
 for(int i=0;i<nres;i++){
  int slot=first+i;if(slot<0||slot>=TX_SLOT_MAX)continue;
  if(repl[i])nvn_vk_bind_texture_slot_view(slot,repl[i]);
  else nvn_vk_bind_texture_slot(slot,res[i]);
 }
 return 0;
}
/* M4ap P1：多一个 base_instance（NVN DrawElementsInstanced 的第 7 参）——交给 vkCmdDrawIndexed 的
   firstInstance，shader 用 instance-id 时起点才对。 */
static int g_rp_draw(int mode,int type,int cnt,uint64_t idx,int bv,int bi,int inst,int dcls,void *u){(void)u;return nvn_vk_draw_elements(mode,type,cnt,idx,bv,bi,inst,dcls);}
/* M4ad P2：无索引 draw 的回放入口（走流端 cmd_trace.c 新增 CMD_DRAW_ARRAYS 分支后才会调到）。
   参数口径与 nvn_vk_draw_arrays 一致：first = 起始顶点，count = 顶点数，inst = 实例数。 */
static int g_rp_draw_arrays(int mode,int first,int cnt,int inst,void *u){(void)u;return nvn_vk_draw_arrays(mode,first,cnt,inst);}
/* M3s：cmd_trace 的取证行出口（首 draw 前 opcode dump）。 */
static void g_rp_note(const char *line,void *u){(void)u;vk_boot_logf("%s",line);}
static uint64_t g_h_rp_noprogram;   /* BindProgram 查不到 key 的次数（取证：说明 program→key 配对失败） */
/* M4i P0：RSS 基线探针——每 100 submit 一行，拆 RssAnon/RssFile 回答「谁在涨」。
   smaps 逐映射扫描只做稀疏点（#1 与每 600 submit），否则 400 MB 映射会让这项开销本身失真。 */
/* ---- M4k P1②：jemalloc arena 运行时收敛 ----
   MALLOC_CONF 只在分配器初始化前读环境（本进程：ArkTS 起、ability 起、再进 libgame，早于我们的任何
   代码），所以「改环境变量」这条路在 HAP 里不可达。可达的是 mallctl：把每个 arena 的 dirty/muzzy
   decay 调短并主动 purge，把空闲页还给 OS。全 best-effort，探测失败只记一行、不假装生效。 */
typedef int (*mallctl_fn)(const char *, void *, size_t *, void *, size_t);
static mallctl_fn g_mallctl;
static unsigned g_alloc_arenas;
static void alloc_tune_once(void)
{
 static int done; if (done) return; done = 1;
 mallctl_fn mc = (mallctl_fn)dlsym(RTLD_DEFAULT, "mallctl");
 if (!mc) { void *h = dlopen("libc.so", RTLD_NOW | RTLD_NOLOAD); if (h) mc = (mallctl_fn)dlsym(h, "mallctl"); }
 if (!mc) { void *h = dlopen("libjemalloc.so", RTLD_NOW | RTLD_NOLOAD); if (h) mc = (mallctl_fn)dlsym(h, "mallctl"); }
 if (!mc) { vk_boot_logf("[mem] allocator: mallctl NOT exported -> MALLOC_CONF/mallctl route unavailable"); return; }
 unsigned n = 0; size_t nl = sizeof n;
 if (mc("arenas.narenas", &n, &nl, NULL, 0) != 0 || !n) {
  vk_boot_logf("[mem] allocator: mallctl found but arenas.narenas unreadable -> no tuning"); return;
 }
 g_mallctl = mc; g_alloc_arenas = n;
 ssize_t dd = -1; size_t dl = sizeof dd; mc("opt.dirty_decay_ms", &dd, &dl, NULL, 0);
 unsigned lim = n > 16 ? 16 : n;
 for (unsigned i = 0; i < lim; i++) {
  char k[64]; ssize_t v = 1000; size_t vl = sizeof v;
  snprintf(k, sizeof k, "arena.%u.dirty_decay_ms", i); mc(k, NULL, NULL, &v, vl);
  v = 0; snprintf(k, sizeof k, "arena.%u.muzzy_decay_ms", i); mc(k, NULL, NULL, &v, vl);
 }
 ssize_t rb = -1; size_t rl = sizeof rb; mc("arena.0.dirty_decay_ms", &rb, &rl, NULL, 0);
 vk_boot_logf("[mem] allocator: mallctl ok arenas=%u opt.dirty_decay_ms=%ld -> arena0 readback=%ld",
              n, (long)dd, (long)rb);
}
static void alloc_purge(void)
{
 if (!g_mallctl) return;
 unsigned lim = g_alloc_arenas > 16 ? 16 : g_alloc_arenas;
 for (unsigned i = 0; i < lim; i++) { char k[64]; snprintf(k, sizeof k, "arena.%u.purge", i); g_mallctl(k, NULL, NULL, NULL, 0); }
 vk_boot_logf("[mem] allocator: purged %u arenas", lim);
}
static void nvn_mem_report(unsigned sub)
{
 long vmrss=-1,vmsize=-1,anon=-1,rfile=-1,shmem=-1;
 FILE *f=fopen("/proc/self/status","r");
 if(f){char line[256];
  while(fgets(line,sizeof line,f)){
   if(!strncmp(line,"VmRSS:",6))vmrss=atol(line+6);
   else if(!strncmp(line,"VmSize:",7))vmsize=atol(line+7);
   else if(!strncmp(line,"RssAnon:",8))anon=atol(line+8);
   else if(!strncmp(line,"RssFile:",8))rfile=atol(line+8);
   else if(!strncmp(line,"RssShmem:",9))shmem=atol(line+9);
  }
  fclose(f);
 }
 int nfd=-1;
 {DIR *d=opendir("/proc/self/fd");
  if(d){int n=0;struct dirent *e;while((e=readdir(d))){if(e->d_name[0]!='.')n++;}closedir(d);nfd=n;}}
 long spv=0,jit=0,heap=0;
 if(sub==1||sub%600==0){
  FILE *m=fopen("/proc/self/smaps","r");
  if(m){char line[512];int in_spv=0,in_jit=0,in_heap=0;
   while(fgets(line,sizeof line,m)){
    char *dash=strchr(line,'-');
    int hdr=(dash&&dash-line>=8&&dash-line<=16&&isxdigit((unsigned char)dash[1]));
    if(hdr){in_spv=strstr(line,"spv.bin")!=NULL;in_jit=strstr(line,".smo_jit")!=NULL;
     in_heap=(strstr(line,"[heap]")!=NULL);continue;}
    if(!strncmp(line,"Rss:",4)){long v=atol(line+4);
     if(in_spv)spv+=v;else if(in_jit)jit+=v;else if(in_heap)heap+=v;}
   }
   fclose(m);}
 }
 vk_boot_logf("[mem] @submit#%u rss_kb=%ld vmsize_kb=%ld anon_kb=%ld file_kb=%ld shmem_kb=%ld fd=%d smaps_spv_kb=%ld jit_kb=%ld heap_kb=%ld",
  sub,vmrss,vmsize,anon,rfile,shmem,nfd,spv,jit,heap);
}
/* M4i P1：一次性映射明细（#1800 打一次）——回答「常驻的 800 MB 分布在哪些映射」。
   只保留 Rss 最大的 12 个映射（按路径 basename 显示）；纯一次，不进曲线。 */
static void mt_push(long rss,const char *name,long *top,char nm[][80],int *nt,int N)
{
 if(rss<=0)return;
 int ins=-1;
 if(*nt<N)ins=(*nt)++;
 else if(rss>top[N-1])ins=N-1;
 if(ins<0)return;
 top[ins]=rss;snprintf(nm[ins],80,"%s",name);
 for(int i=ins;i>0&&top[i]>top[i-1];i--){
  long t=top[i];top[i]=top[i-1];top[i-1]=t;
  char c[80];snprintf(c,sizeof c,"%s",nm[i]);snprintf(nm[i],80,"%s",nm[i-1]);snprintf(nm[i-1],80,"%s",c);
 }
}
static void nvn_mem_top(unsigned sub)
{
 FILE *m=fopen("/proc/self/smaps","r");
 if(!m){vk_boot_logf("[mem-top] @submit#%u smaps open failed",sub);return;}
 enum{N=12};
 long top[N];char nm[N][80];int nt=0;
 char line[512],cur[80]="[anon]";long rss=0;
 for(;;){
  if(!fgets(line,sizeof line,m))break;
  char *dash=strchr(line,'-');
  int hdr=(dash&&dash-line>=8&&dash-line<=16&&isxdigit((unsigned char)dash[1]));
  if(hdr){
   mt_push(rss,cur,top,nm,&nt,N);
   rss=0;
   char *p=strrchr(line,'/');
   if(p){snprintf(cur,sizeof cur,"%s",p);char *nl=strchr(cur,'\n');if(nl)*nl=0;}
   else {char *b=strchr(line,'[');if(b){snprintf(cur,sizeof cur,"%s",b);char *nl=strchr(cur,'\n');if(nl)*nl=0;}else snprintf(cur,sizeof cur,"[anon]");}
   continue;
  }
  if(!strncmp(line,"Rss:",4))rss+=atol(line+4);
 }
 mt_push(rss,cur,top,nm,&nt,N);
 fclose(m);
 vk_boot_logf("[mem-top] @submit#%u n=%d (single mapping, Rss desc)",sub,nt);
 for(int i=0;i<nt;i++)vk_boot_logf("[mem-top] #%d rss_kb=%ld %s",i+1,top[i],nm[i]);
}
static void nvn_a_stub_report(unsigned sub);   /* M4am P1-2：A 类（表外返-0 桩）按名 Top（定义在上方，桩计数块之后） */
static void nm_sc_report(unsigned sub);        /* M4am P1-2：B 类（只记账/空/日志桩）按名 Top（定义在下方 cb_account 之后） */
/* M4an：CallCommands/CopyCommands 台账（计数在此定义；录制接线块在下方 CB_REC 区，报表在上面先用到） */
static unsigned long long g_cc_call_n,g_cc_copy_n;
/* M4an P2：四个单数状态设置的调用量（定义在此，报表在上方，接线块在下方） */
static unsigned long long g_ss_viewport,g_ss_scissor,g_ss_depthrange,g_ss_blendcolor;
extern void nvn_port_callcmd_totals(uint64_t *o);
extern void nvn_port_cmdmem_stats(uint64_t *o);
extern void nvn_port_last_cb(uint64_t *o);
extern void nvn_port_note_submit_h(const void *h,int n);
/* ================= M103A P3（2026-11-13）：两处仪表改成按时间触发 =================
   病根（M102C §5.1/§5）：`[tex] miss class` 与 `[t] lru` 都挂在「每 300 submit」的报表门上。
   3D 段只有 0.2–0.25 fps（整段约 300 submit / 130 s），所以 3D 段的 evict/noslot/deny 与
   cap_idimg/heat_gate/budget 分项**一行都打不出来**，只能拿 `[m7k] ph` 当代理值。
   修法：把这两条读数从 submit 门里解耦，改成**时间门 ≥30 s**（同一 30 s 拍子，不重复打印）。
   触发点 = nvn_queue_submit_commands 入口（每 submit 一次判断，成本 = 一次 time() + 两次比较）。
   关态：m103a 关也照样按时间打（这是纯粹的仪表修正，不改任何渲染/建图行为）。 */
static long g_t_lru_t; static int g_t_lru_seen;
static void t_lru_report(unsigned sub);
static void m103a_time_report(unsigned sub)
{
 long now=(long)time(NULL);
 if(g_t_lru_seen&&now-g_t_lru_t<30)return;   /* 门在 t_lru_report 内部，这里只做早期短路 */
 t_lru_report(sub);
 nvn_vk_m103a_tick(sub);                     /* [tex] miss class（nvn_vk.c 内部同款 30 s 门） */
}
/* M5z P1：LRU 驱逐台账（修的是「id 图名额一次性」这个类）。evict=真换出的张数/字节；
   noslot=想换但所有图都被槽引用（安全侧，宁可不换）；deny=被每 submit 限流挡下。
   M6a B：延迟销毁台账。deferred=进坟场张数；freed=已销毁；sync=坟场满回退同步的；pend=此刻还挂着。
   旧的 evict 每张一次 vkQueueWaitIdle（evict=455 时就是 455 次全队列同步）⇒ 现在每批一次。
   M103A P3：本函数自带 30 s 时间门 ⇒ 低帧率段（3D 0.2 fps）也能出，一行/30 s 封顶。
   M103A 追加 `[m103a] stat`：名额/活体字节/让位/压顶拒绝的同拍读数（判据①②⑥的原始口径）。 */
static void t_lru_report(unsigned sub)
{
 long now=(long)time(NULL);
 if(g_t_lru_seen&&now-g_t_lru_t<30)return;
 g_t_lru_t=now;g_t_lru_seen=1;
 {unsigned long long gd=0,gf=0,gs=0;unsigned gp=0;nvn_vk_tex_graveyard_stats(&gd,&gf,&gs,&gp);
  vk_boot_logf("[t] lru evict=%llu evict_bytes=%lluMB noslot=%llu deny=%llu deny_s=%llu small=%llu idimg=%llu/%u live=%lluMB cum=%lluMB grave deferred=%llu freed=%llu synced=%llu pend=%u",
   g_z_evict,(unsigned long long)(g_z_evict_bytes>>20),g_z_evict_noslot,g_z_evict_deny,g_z_evict_deny_s,g_z_evict_small,
   g_t_idimg,(unsigned)k_id_img_max,(unsigned long long)(g_t_bytes_live>>20),(unsigned long long)(g_t_bytes>>20),
   gd,gf,gs,gp);}
 vk_boot_logf("[m103a] stat on=%d @submit#%u cap=%u idimg=%llu live=%lluMB/%lluMB evict=%llu reclaim=%llu "
              "deny=%llu deny_s=%llu deny_bytes=%llu up=%llu skip=%llu",
  m103a_on(),sub,(unsigned)k_id_img_max,(unsigned long long)g_t_idimg,
  (unsigned long long)(g_t_bytes_live>>20),(unsigned long long)(M103A_BYTES_MAX>>20),
  (unsigned long long)g_m103a_evict,(unsigned long long)g_m103a_reclaim,
  (unsigned long long)g_z_evict_deny,(unsigned long long)g_z_evict_deny_s,
  (unsigned long long)g_m103a_deny_bytes,
  (unsigned long long)g_t_up,(unsigned long long)g_t_skip);
}
static void nvn_h_report(unsigned sub){ int kinds=0;uint64_t skipped=0; for(int i=0;i<CMD_TRACE_MAX_OPS;i++)if(g_h_ops.per_op[i]){kinds++;if(i!=NVN_CMD_OP_CLEAR_COLOR)skipped+=g_h_ops.per_op[i];}

 vk_boot_logf("[h] opcode skipped @submit#%u: skipped=%llu kinds=%d clear_ok=%llu clear_fail=%llu noptr=%llu exec_fail=%llu records=%llu bad_cb=%llu",
  sub,(unsigned long long)skipped,kinds,(unsigned long long)g_h_clear_ok,(unsigned long long)g_h_clear_err,
  (unsigned long long)g_h_clear_noptr,(unsigned long long)g_h_exec_fail,(unsigned long long)g_h_ops.total,
  (unsigned long long)nvn_rec_bad_cb());
 /* M3i：RT 绑定取证（比例用这条：SET_RENDER_TARGETS 条数见下方 opcode 行） */
 {unsigned long long rb=0,rn=0,bo=0,bf=0;nvn_vk_rt_stats(&rb,&rn,&bo,&bf);
  unsigned long long db=0,ds=0,df=0,dbf=0;nvn_vk_rt_depth_stats(&db,&ds,&df,&dbf);
  vk_boot_logf("[h] rt bind totals @submit#%u: binds=%llu n_gt0=%llu rt_fail=%llu batches_ok=%llu batches_fail=%llu flush_err=%llu probe_miss=%llu depth_binds=%llu depth_slots=%llu depth_fail=%llu depth_badfmt=%llu stk_mem=%llu stk_ptr=%llu",
   sub,rb,rn,(unsigned long long)g_h_rt_bind_err,bo,bf,(unsigned long long)g_h_rt_flush_err,
   (unsigned long long)g_h_rt_probe_miss,db,ds,df,dbf,
   (unsigned long long)g_h_rt_stk_mem,(unsigned long long)g_h_rt_stk_ptr);
  vk_boot_logf("[h] rt src totals @submit#%u: snap=%llu rec=%llu tex_h=%llu ind=%llu ind2=%llu ind3=%llu handle=%llu probe=%llu n0=%llu negc=%llu deep(amb=%llu none=%llu)",
   sub,(unsigned long long)g_h_rt_by_snap,(unsigned long long)g_h_rt_by_rec,
   (unsigned long long)g_h_rt_by_texh,(unsigned long long)g_h_rt_by_ind,
   (unsigned long long)g_h_rt_by_ind2,(unsigned long long)g_h_rt_by_ind3,(unsigned long long)g_h_rt_by_handle,
   (unsigned long long)g_h_rt_by_probe,(unsigned long long)g_h_rt_n0,
   (unsigned long long)g_h_rt_neg_hit,(unsigned long long)g_rt_deep_amb,(unsigned long long)g_rt_deep_none);
  /* M6b A1：probe_miss 归因（why 六桶 + 每个 tex0 一行的 top 表；每 300 submit 一次）。 */
  {unsigned long long wt=0; for(int i=0;i<7;i++) wt+=g_rt_miss_why[i]; (void)wt;
   vk_boot_logf("[h] rt miss why @submit#%u: unread=%llu magic_l1=%llu deep=%llu pooldata=%llu interior=%llu unknown=%llu l2_struct=%llu keys=%u ovf=%u",
    sub,g_rt_miss_why[0],g_rt_miss_why[1],g_rt_miss_why[2],g_rt_miss_why[3],g_rt_miss_why[4],g_rt_miss_why[5],g_rt_miss_why[6],
    g_rt_miss_ntop,g_rt_miss_ovf);
   for(unsigned i=0;i<g_rt_miss_ntop;i++)
    vk_boot_logf("[h] rt miss top i=%u key=%#llx n=%llu l1=%u magic=%u deep=%u pool=%u in=%u why=%s",
     i,(unsigned long long)g_rt_miss_top[i].key,g_rt_miss_top[i].n,
     g_rt_miss_top[i].l1n,g_rt_miss_top[i].magic,g_rt_miss_top[i].deep,
     g_rt_miss_top[i].pool,g_rt_miss_top[i].inside,
     g_rt_miss_why_s[g_rt_miss_top[i].why]);}
  /* M4r P1：深度通道使用面（验收口径：attach/only 应为非 0，dcolor/donly 应有真实 draw） */
  {unsigned long long da=0,do_=0,dsk=0,dc=0,don=0,dpo=0,dpf=0;
   nvn_vk_depth_use_stats(&da,&do_,&dsk,&dc,&don,&dpo,&dpf);
   vk_boot_logf("[h] depth totals @submit#%u: attach=%llu dpass_only=%llu skip=%llu draws_color_depth=%llu draws_depth_only=%llu pipe_var_ok=%llu pipe_var_fail=%llu dpass_skipped=%llu dpass_arrays=%llu",
    sub,da,do_,dsk,dc,don,dpo,dpf,nvn_vk_depth_dpass_skip(),nvn_vk_depth_dpass_arrays());}}
 /* M3l：draw/回读取证（与上一行同节流点；draws 为已提交的 vkCmdDraw 次数，readback 应恒 <=1） */
 {unsigned long long dr=0,df=0,rdb=0;nvn_vk_draw_stats(&dr,&df,&rdb);
  vk_boot_logf("[d] draw totals @submit#%u: draws=%llu draw_fail=%llu readback=%llu bridge_err=%llu",
   sub,dr,df,rdb,(unsigned long long)g_h_draw_err);}
 /* M3r：真实 draw 取证（draws = 已录进批次的 vkCmdDrawIndexed 笔数；pipes = 管线建成功数；
    skip_* 解释「为什么没画」：noprog=program 无 VS/FS key，nopipe=管线建失败，attr=顶点状态/数据不可用，topo=拓扑不支持） */
 {unsigned long long dr=0,df=0,sk=0,po=0,pf=0,sn=0,sp=0,sa=0,st=0,nrt=0,bv=0,nvt=0,al=0,pi=0,ph=0;
  nvn_vk_real_draw_stats(&dr,&df,&sk,&po,&pf,&sn,&sp,&sa,&st,&nrt,&bv,&nvt,&al,&pi,&ph);
  vk_boot_logf("[r] draw totals @submit#%u: draws=%llu fail=%llu skip=%llu pipes_ok=%llu pipes_fail=%llu "
               "skip_noprog=%llu skip_nopipe=%llu skip_attr=%llu skip_topo=%llu "
               "skip_nort=%llu skip_basev=%llu skip_nverts=%llu skip_alloc=%llu pipe_skip_int64=%llu attr_ph=%llu pipe_cap=%llu pipe_skip_gmem=%llu pipe_skip_bigfs=%llu attr_over=%llu skip_heavy=%llu skip_memveto=%llu skip_cfg=%llu",
   sub,dr,df,sk,po,pf,sn,sp,sa,st,nrt,bv,nvt,al,pi,ph,(unsigned long long)nvn_vk_pipe_cap_hits(),
   (unsigned long long)nvn_vk_pipe_skip_gmem(),
   (unsigned long long)nvn_vk_pipe_skip_bigfs(),
   (unsigned long long)nvn_vk_attr_over(),
   (unsigned long long)nvn_vk_skip_heavy(),    /* M5w：隔离闸门（黑名单/默认/实测 learned）跳掉笔数 */
   (unsigned long long)nvn_vk_skip_memveto(), /* M5x：submit 前内存爆炸否决跳掉笔数 */
   (unsigned long long)nvn_vk_skip_cfg());    /* M6y：SPIR-V CFG 否决（显式 hash / ExistsPath 代价） */
  {unsigned long long su=0,sd=0;nvn_vk_stall_stats(&su,&sd);   /* M4k P0-2：宿主侧阻塞归因 */
   vk_boot_logf("[r] host stall @submit#%u: upload_fence_gt5s=%llu tex_destroy_waitidle_gt300ms=%llu",
    sub,su,sd);}
  nvn_vk_gm_report(sub);   /* M5v P1：gmem 搬迁台账（heap/reloc/字节/slot 用尽/跨度超限） */
  nvn_vk_memveto_report(sub);   /* M5x：内存爆炸否决台账（每 key 笔数） */
  ih_report(sub);   /* M4aj P1：初始化历史计数 + 目标 id「id 槽 vs 最新 Initialize off」对照 */
  nvn_vk_inst_totals_report(sub);   /* M4ap P1：实例化输入率/draw 台账（divisor 普查 + 提交/回落分档） */
  nvn_vk_attr_fmt_report(sub);     /* M4at P1：顶点属性格式台账（Top8，fmt=ok/ph——补格式表的判据） */
  {unsigned long long ne=0,nk=0;int ck=0;nvn_vk_skip_noprog_split(&ne,&nk,&ck);   /* M3v P3：noprog 两分支 */
   vk_boot_logf("[r] skip_noprog split @submit#%u: noentry=%llu nokey=%llu cache_keys=%d",
    sub,ne,nk,ck);}
  nvn_vk_nw_report(sub);   /* M4bw P1（只读）：noprog/pair 门按目标 RT 身份 + 缺哪一路归因 */
  nvn_vk_skip_nokey_dump(sub);   /* M4h P1：nokey 长尾 (vs,fs)→次数 top20 + bin 有/无 */
  /* M4ad P1：draw 普查（verts/idx 直方图、Top vs、目标分布、DrawArrays 台账）。
     与上面 [r] draw totals 同节流点，不额外刷屏。 */
  nvn_vk_da_census_report(sub);
  /* M4ad P1 之二：**没记录的 draw 形态**（instanced / base-vertex / indirect）——这些符号表项
     仍是 cb_account 记账桩，连命令流都不进 ⇒ 在 [h] opcode 里根本看不见，属于「零证据」盲区。
     这里给它们的调用次数（宿主侧桩内自增），把盲区照亮；本轮不修它们。
     M5p C：arrays_inst 已接线（rec_arrays_inst=真进命令流的笔数）；arrays_inst 应停在接线前的旧值或 0。 */
  {extern unsigned long long g_nr_draw_arrays_inst,g_nr_draw_elems_inst,g_nr_draw_elems_basev,g_nr_draw_indirect;
  extern unsigned long long g_nr_rec_arrays_inst;
  extern unsigned long long g_nr_replayed_arrays_inst;   /* M5q 配对计数：回放侧真下发 vkCmdDraw 的笔数 */
  vk_boot_logf("[h] norec draws: arrays_inst=%llu elems_inst=%llu elems_basev=%llu indirect=%llu rec_arrays_inst=%llu "
               "replayed_arrays_inst=%llu (记账桩，未进命令流 ⇒ 从未重放；rec=录制侧/replayed=回放侧配对)",
   g_nr_draw_arrays_inst,g_nr_draw_elems_inst,g_nr_draw_elems_basev,g_nr_draw_indirect,g_nr_rec_arrays_inst,
   g_nr_replayed_arrays_inst);}}
 /* M3s：顶点状态全量计数（判定依据：nonzero=0 且 big=0 ⇒ 游戏每次传的都真是零数组；
    若 big>0 说明存在 count>16 的调用，M3r 的非零统计漏掉了它们）。 */
 {unsigned long long ac=0,az=0,an=0,sc=0,sz=0,sn=0;nvn_rec_vertex_totals(&ac,&az,&an,&sc,&sz,&sn);
  vk_boot_logf("[v] vertex totals @submit#%u: attrib_calls=%llu zero=%llu nonzero=%llu stream_calls=%llu zero=%llu nonzero=%llu",
   sub,ac,az,an,sc,sz,sn);}
 /* M3s：UBO 真实字节上送（P3） */
 {unsigned long long ub=0,uu=0,ut=0;nvn_vk_ubo_stats(&ub,&uu,&ut);
  vk_boot_logf("[u] ubo totals binds=%llu up=%llu trunc=%llu s0=%llu s1=%llu",ub,uu,ut,
   nvn_vk_ubo_stage_count(0),nvn_vk_ubo_stage_count(1));}
 /* M82A：真机专属根因族（客体 UBO 超额读跨 VMA 边界 ⇒ readv 整段 EFAULT ⇒ 4 KiB 槽被清零）。
    rb = 「修前读不到声明区间、修后读到」的绑定数；clamped/partial/fail = 钳位/前缀保住/一条没读到。 */
 {unsigned long long cl=0,pa=0,fa=0,by=0,rb=0;nvn_vk_m82a_stats(&cl,&pa,&fa,&by,&rb);
  vk_boot_logf("[u] ubo m82a clamped=%llu partial=%llu fail=%llu bytes=%llu rb=%llu mode=%d",
   cl,pa,fa,by,rb,nvn_vk_m82a_mode());}
 /* M86A：小 UBO 两步拷贝（`sz<4096`：readv→静态 4096 B 缓冲→memcpy→arena slot）。
    two_step=走两步拷贝的绑定数；win=真写进 arena 的字节；bytes=读进静态缓冲的字节；
    trunc=连声明区间都没读到的绑定数；fail=一条都没读到的绑定数。bit1 时每 60 s 一行。 */
 nvn_vk_m86a_report();
 /* M3u P1：UpdateUniformBuffer 真落地口径（ok = 真写进客体内存的次数/字节） */
 vk_boot_logf("[u] upd totals n=%llu ok=%llu bad=%llu bytes=%llu",g_ub_n,g_ub_ok,g_ub_bad,g_ub_bytes);
 /* M3y P2：真实纹理上送口径。up=首次上送成功张数 skip=跳过张数(建图/上送失败、超限) cache=已上送再次绑定
    unres=句柄解析不到（池未登记/纹理无 mem） noslot=槽越界 binds=BindTextures 回放次数 slots=当前槽绑定次数
    reg=池登记调用数/表项 col=同 id 换纹理的冲突数 bytes=累计上送字节 */
 vk_boot_logf("[t] tex totals up=%llu skip=%llu cache=%llu bytes=%llu unres=%llu noslot=%llu mism=%llu binds=%llu slots=%llu reg=%llu/%u col=%llu side_init_skip=%llu idimg=%llu cap=%llu hi=%llu reup=%llu reup_bytes=%llu rt_bound=%llu zero_miss=%llu zmiss_ids=%u",
  g_t_up,g_t_skip,g_t_unchg,g_t_bytes,g_t_unres,g_t_noslot,g_t_mism,g_t_binds,
  nvn_vk_texture_slots_bound(),g_texreg_calls,g_texreg_n,g_texreg_collisions,g_t_side_up_skip,
  g_t_idimg,g_t_idimg_cap,g_t_slot_hi,g_t_reup,g_t_reup_bytes,
  g_t_rt_zero_bound,g_t_rt_zero_miss,g_t_zmiss_ids);
 /* M6a A：零内容 → RT 绑定的口径。alias_n=id→RT 槽 别名表条数；hit=别名查中次数；
    zero_link_store=同 store 多 id 补登；zero_link_extfmt=唯一 ext+fmt 补登；zero_multi=多候选放弃；
    bound=真改绑 RT 图的采样次数；miss=零内容但没有唯一同形 RT（明细见 [tex] missing）。 */
 {unsigned long long an=0,ah=0,ao=0,as=0,ae=0,am=0,aord=0;
  nvn_vk_rt_alias_stats(&an,&ah,&ao,&as,&ae,&am);
  nvn_vk_rt_order_stats(&aord);
  vk_boot_logf("[t] rt alias n=%llu hit=%llu ovf=%llu zero_link_store=%llu zero_link_extfmt=%llu zero_multi=%llu bound=%llu miss=%llu",
   an,ah,ao,as,ae,am,g_t_rt_zero_bound,g_t_rt_zero_miss);
  vk_boot_logf("[t] rt alias order by_order=%llu (同 ext+fmt 多候选里靠本 submit 唯一写者认下来的次数)",aord);}
 /* M35A：÷2 分辨率账（只在 smo_m35a=1 时打，关态日志逐字不变）。
    store_* / extfmt_* = 零内容回退路 ②store、③唯一ext+fmt 两条路的 join 笔数，*_div2 = 其中只靠
    h_rs_match 的 ÷2 档（槽 800x450 对采样 1600x900）才 join 上的笔数。div2>0 而 b0 同一字段不存在
    ⇒ 这族"共用 store 但尺寸差 2×"的采样确实被旧判据拒掉了。 */
 if(m35a_on()){unsigned long long sh=0,sd=0,eh=0,ed=0;
  nvn_vk_m35a_stats(&sh,&sd,&eh,&ed);
  vk_boot_logf("[m35a] div2 store_hit=%llu store_div2=%llu extfmt_hit=%llu extfmt_div2=%llu "
               "zero_bound=%llu zero_miss=%llu (store/extfmt = 零内容回退路两条路的 join 数；div2 = 其中靠光栅口径 h_rs_match 才 join 上的)",
   sh,sd,eh,ed,g_t_rt_zero_bound,g_t_rt_zero_miss);}
 /* M6a：零内容样本按形状汇总（cands = 同 ext+fmt 的、已建图 RT 槽个数）。cands=0 = 真缺失
    （guest 里就是零、RT 表里也没有同形目标）；cands>1 = 不唯一，宁可留黑也不顶替。 */
 for(int i=0;i<g_zs_n;i++)
  vk_boot_logf("[tex] zshape i=%d ext=%ux%u fmt=0x%x samples=%llu cands=%d cand_img=%d id0=%d",
   i,g_zs[i].w,g_zs[i].h,g_zs[i].fmt,g_zs[i].n,g_zs[i].cand,g_zs[i].img,g_zs[i].id0);
 /* M4as P2：大图独立配额口径（big_ok = 走配额放行的张数 / 字节；big_gate = 配额满被让位次数） */ vk_boot_logf("[t] big quota up=%llu bytes=%llu/%lluMB gate=%llu (≥1024x512 首绑必传；不受 heat 门槛约束)",
  g_t_big_up,g_t_big_bytes,(unsigned long long)(k_big_up_bytes_max>>20),g_t_big_gate);
 /* M5z P0：被我们自己的上限饿死的**去重 id** 数 + 它们声称需要的字节（决定「抬上限」还是「做 LRU」）。 */
 vk_boot_logf("[t] starve cap_ids=%llu cap_bytes=%llu heat_ids=%llu heat_bytes=%llu idimg=%llu/%u bytes_used=%lluMB",
  g_t_starve_cap_ids,g_t_starve_cap_bytes,g_t_starve_heat_ids,g_t_starve_heat_bytes,
  g_t_idimg,(unsigned)k_id_img_max,(unsigned long long)(g_t_bytes>>20));
 /* M5z P1：LRU 驱逐台账（修的是「id 图名额一次性」这个类）。evict=真换出的张数/字节；
    noslot=想换但所有图都被槽引用（安全侧，宁可不换）；deny=被每 submit 限流挡下。 */
 /* M6a B：延迟销毁台账。deferred=进坟场张数；freed=已销毁；sync=坟场满回退同步的；pend=此刻还挂着。
    旧的 evict 每张一次 vkQueueWaitIdle（evict=455 时就是 455 次全队列同步）⇒ 现在每批一次。 */
 /* M103A P3：`[t] lru` 与 `[tex] miss class` 改成**按时间**触发（≥30 s 一行）——
    3D 段 0.2 fps，旧的「每 300 submit」在整段里只能落一次（M102C §5 实测：最后一行停在
    submit#1200 / app_t≈310 s 菜单段），低帧率段等于仪表全盲。门在 t_lru_report 内部，
    两个调用点（nvn_h_report 的 submit 门 + m103a_time_report 的时间门）共用同一个 30 s 拍子，
    因此不会重复打印。 */
 t_lru_report(sub);
 /* M4u P1：unres 病根三分 + 修复计数（验收口径 src=init|reg|bad；addr/handle 两路弃用理由见 texreg_snap_from_obj 注释）。
    unres_dropped = unres 里「注册期因无快照被丢弃」的那部分，是 P1 要打的靶。 */
 vk_boot_logf("[t] unres fix src=init n=%llu src=reg n=%llu src=bad n=%llu src=addr n=0 src=handle n=0 pin_evict=%llu unres_dropped=%llu heat_gate=%llu",
  g_t_unres_fix_init,g_t_unres_fix_reg,g_t_unres_fix_bad,g_t_pin_evict,g_t_unres_dropped,g_t_heat_gate);
 tex_bind_stats_dump(sub);   /* M4t P2：never-up top20 + 上送失败原因分布（每 6000 submit 一次，最多 4 次） */
 /* M4e（route②）源图替换口径：hit_self=命中当前 RT（走影子副本）hit_other=命中别的 RT（直接采样）
    miss=该 id 不是任何 RT view_fail=替换图准备失败（回落原路径）copy_ok/copy_fail=影子副本结果
    throttle=节流跳过（继续采旧影子）direct/direct_fail=非当前目标的布局转换结果。 */
 {unsigned long long ck=0,cf=0,th=0,do_=0,df=0;nvn_vk_rt_repl_stats(&ck,&cf,&th,&do_,&df);
  unsigned long long lk=0,ls=0,ld=0,la=0;nvn_vk_rt_link_stats(&ls,&ld,&lk,&la);   /* M4t P1：反向登记口径 */
  unsigned long long r2s=0,r2f=0;nvn_vk_rt2_reject_split(&r2s,&r2f);              /* M4w P1：守卫拒因拆分 */
  vk_boot_logf("[t] rt repl @submit#%u: hit_self=%llu hit_other=%llu miss=%llu view_fail=%llu copy_ok=%llu copy_fail=%llu throttle=%llu direct=%llu direct_fail=%llu ds_writes=%llu rt_link=%llu(store=%llu desc=%llu amb=%llu) id_by_store=%llu/%llu rt2=%llu/%llu(ids=%u noup=%llu link=%llu rej=%llu size=%llu fmt=%llu)",
   sub,g_t_rt_hit_self,g_t_rt_hit_other,g_t_rt_miss,g_t_rt_view_fail,ck,cf,th,do_,df,
   nvn_vk_rt_ds_writes(),lk,ls,ld,la,g_t_id_by_store_hits,g_t_id_by_store_hits+g_t_id_by_store_miss+g_t_id_by_store_bad,
   g_t_rt2_hits,g_t_rt2_hits+g_t_rt2_miss,g_t_rt2_ids,g_t_rt2_noup,g_t_rt2_link,
   nvn_vk_rt2_rejected(),r2s,r2f);}
 /* M3y P2：绑定样本的格式分布（fmt&0x3F 分桶；other=fmt>=64）。判「真实纹理有没有进管线」的旁证。 */
 {char b[320];int o=snprintf(b,sizeof b,"[t] tex fmt hist (bind samples):");
  for(int i=0;i<64;i++)if(g_t_fmt_hist[i]&&o<(int)sizeof b-24)o+=snprintf(b+o,sizeof b-(size_t)o," 0x%x=%llu",i,g_t_fmt_hist[i]);
  if(g_t_fmt_other&&o<(int)sizeof b-24)o+=snprintf(b+o,sizeof b-(size_t)o," other=%llu",g_t_fmt_other);
  vk_boot_logf("%s",b);}
 /* M3u P3：program→key 缺口六计数（paired = 表内 VS+FS 齐备的 program 数） */
 vk_boot_logf("[p] program totals @submit#%u: prog_calls=%llu key_ok=%llu key_bad=%llu vs_only=%llu fs_only=%llu both=%llu paired=%u registered=%u cap_hit=%llu bind=%llu bind_miss=%llu",
  sub,g_pg_calls,g_pg_key_ok,g_pg_key_bad,g_pg_vs_only,g_pg_fs_only,g_pg_both,
  nvn_vk_programs_paired(),nvn_vk_programs_registered(),nvn_vk_program_cap_hits(),
  nvn_vk_program_binds(),nvn_vk_program_bind_misses());
 /* M4b P1：pair_want 去重计数（配对重烘的输入；dump 本体由 nvn_vk 每 60s 追加到 files/pair_want.txt） */
 vk_boot_logf("[p] pair_want @submit#%u: pairs=%d",sub,nvn_vk_pair_want_count());
 shader_dump_keybad_report();   /* M3v P2：key_bad 按 stage / 拒绝原因分桶 + 首 3 条样本 */
 {uint64_t T[8]={0,0,0,0,0,0,0,0};
  nvn_port_callcmd_totals(T);   /* M4an：calls,bytes,recs,bad,stop_end,trunc,notrec,capped（call+copy 合计） */
  vk_boot_logf("[h] callcmd totals call=%llu copy=%llu bytes=%llu bad=%llu recs=%llu stop_end=%llu trunc=%llu notrec=%llu capped=%llu",
   g_cc_call_n,g_cc_copy_n,(unsigned long long)T[1],(unsigned long long)T[3],(unsigned long long)T[2],
   (unsigned long long)T[4],(unsigned long long)T[5],(unsigned long long)T[6],(unsigned long long)T[7]);}
 /* M5r：命令流真正的内存占用 + CB begin/end 配对 + 最后一条 CB 的首 32 字节（判「guest 没写」还是「我们没采到」） */
 {uint64_t S[8]={0,0,0,0,0,0,0,0};nvn_port_cmdmem_stats(S);
  vk_boot_logf("[cc] cmdmem host_mb=%llu shadow_pages=%llu host_chunks_mb=%llu hostcap=%llu cb_begin=%llu cb_end=%llu cb_nonempty=%llu",
   (unsigned long long)(S[0]>>20),(unsigned long long)S[1],(unsigned long long)(S[2]>>20),(unsigned long long)S[3],
   (unsigned long long)S[4],(unsigned long long)S[5],(unsigned long long)S[6]);}
 {uint64_t L[12]={0};nvn_port_last_cb(L);
  vk_boot_logf("[cc] submit_cb n=%llu h=%llx last_end_span=%llu w0=%08x w1=%08x w2=%08x w3=%08x w4=%08x w5=%08x w6=%08x w7=%08x",
   (unsigned long long)L[0],(unsigned long long)L[1],(unsigned long long)L[2],(unsigned)L[3],(unsigned)L[4],(unsigned)L[5],
   (unsigned)L[6],(unsigned)L[7],(unsigned)L[8],(unsigned)L[9],(unsigned)L[10]);}
 /* M4an P2：四个单数状态设置的调用量（录制已接，回放侧未消费——见函数定义处注释） */
 vk_boot_logf("[h] single-state viewport=%llu scissor=%llu depthrange=%llu blendcolor=%llu",
  g_ss_viewport,g_ss_scissor,g_ss_depthrange,g_ss_blendcolor);
 /* M4an b2：回放 sink 侧 DRAW_ELEMENTS 口径（ok = 交给 nvn_vk 的笔数；skip 的原因拆分见 cmd_trace.h）
    M4ao P1：补 fail 的返回码分档 + CALL 体内归因（in-call 的才是 M4an 新接进来的那部分）。 */
 vk_boot_logf("[h] drawsink totals @submit#%u: ok=%llu fail=%llu skip=%llu (cnt_le0_=%llu idx_bad=%llu big=%llu) "
              "fail_rv1=%llu fail_err=%llu fail_other=%llu incall=%llu incall_ok=%llu call_mark=%llu call_nolen=%llu",
  sub,(unsigned long long)g_h_cum.draws,(unsigned long long)g_h_cum.draw_fail,
  (unsigned long long)g_h_cum.draw_skip,g_h_skip_cnt,g_h_skip_idx,g_h_skip_big,
  g_h_fail_skip,g_h_fail_err,g_h_fail_other,g_h_incall,g_h_incall_ok,g_h_call_markers,g_h_call_body_missing);
 /* M4ao P1：skip 归属审计（M4an 的 13,686 条无归属就是这一行要把它钉成 0）。
    口径：total = nvn_vk 的 g_r_skip；sum = 各理由码之和；unattributed 必须 0。
    reasons_call = 其中来自 CALL inline 体的（新增 draw 死在哪一类的答案）。
    rej_cap/ph/ens = 不进 skip 口径、但同样让 sink fail 涨的三类拒收（第二类盲区）。 */
 {int nr=nvn_vk_skip_reason_count();
  unsigned long long rs[32], rc[32], rj[4], ic[2], tot=0, sum=0; long long un=0;
  memset(rs,0,sizeof rs); memset(rc,0,sizeof rc); memset(rj,0,sizeof rj); memset(ic,0,sizeof ic);
  nvn_vk_skip_detail(&tot,&sum,&un,rs,rc,rj,ic);
  vk_boot_logf("[r] skip audit @submit#%u: total=%llu sum=%llu unattributed=%lld reasons=%d "
               "rej_cap=%llu rej_ph=%llu rej_ens=%llu incall_seen=%llu incall_ok=%llu skip_idx=%llu pair=%llu",
   sub,tot,sum,un,nr,rj[0],rj[1],rj[2],ic[0],ic[1],
   nvn_vk_skip_idx_total(),nvn_vk_skip_pair_total());
  {char b[512]; int o=0;
   for(int i=0;i<nr && i<32 && o<(int)sizeof b-64;i++)
     if(rs[i]||rc[i]) o+=snprintf(b+o,sizeof b-(size_t)o,"%s%s=%llu(call %llu)",o?", ":"",nvn_vk_skip_reason_name(i),rs[i],rc[i]);
   if(o) vk_boot_logf("[r] skip breakdown @submit#%u: %s",sub,b);}}
 for(int i=0;i<CMD_TRACE_MAX_OPS;i++)if(g_h_ops.per_op[i])
  vk_boot_logf("[h] opcode %s seen=%llu action=%s",nvn_cmd_op_name(i)?nvn_cmd_op_name(i):"?",
   (unsigned long long)g_h_ops.per_op[i],
   i==NVN_CMD_OP_CLEAR_COLOR?"replayed":(i==NVN_CMD_OP_SET_RENDER_TARGETS?"bound-rt":
   (i==NVN_CMD_OP_CALL?"inlined+skipped":"skipped")));
 nvn_gpuop_report(sub);   /* M4ai P2：上面这张表里根本没有的写 opcode（copy/clear-texture/dispatch）按名台账 */
 if(sub%300==0){nvn_a_stub_report(sub);nm_sc_report(sub);}   /* M4am P1-2：空桩排查——A 类（表外返-0 桩）与 B 类（只记账/空/日志桩）的按名调用 Top 排名。每 300 submit 一条（m4am_b1 实测 boot 只跑到 ~1690 submit，#1800 那份永远不会到） */
}
static void nvn_h_replay(int n,const uint64_t *hs,unsigned sub){
 if(!hs)return;int nn=n>16?16:n;
 g_clear_seq_n=0;   /* M3u P2：本 submit 的清屏序列从零开始 */
 nvn_vk_cen_set_submit(sub);   /* M4ad P1：一帧清单/普查的标签 */
 nvn_cmd_sink sink;memset(&sink,0,sizeof sink);
 sink.clear=g_h_clear_bridge;sink.rt_bind=g_h_rt_bridge;
 sink.bind_program=g_rp_bind_program;sink.bind_vertex_attrib=g_rp_bind_attrib;
 sink.bind_vertex_stream=g_rp_bind_stream;sink.bind_vertex_buffers=g_rp_bind_vbuf;
 sink.bind_uniform_buffers=g_rp_bind_ubuf;sink.bind_textures=g_rp_bind_tex;
 sink.bind_storage_buffers=g_rp_bind_sbuf;   /* M5x：storage buffer 绑定（gmem） */
 /* M14B：这两个回调**总是**装上（nvn_vk 侧自己按 smo_m14b 决定做不做事）——
    好处：B0/B1 用同一份二进制，走流结构相同，只差 nvn_vk 内部的开关，A/B 对比没有第二变量。 */
 sink.bind_blend_state=g_rp_bind_blend;
 sink.bind_color_state=g_rp_bind_color;
 sink.draw_elements=g_rp_draw;
 sink.draw_arrays=g_rp_draw_arrays;
 sink.note=g_rp_note;
 nvn_vk_m25a_merge_begin();   /* M25A：开合并窗口（开关全 0 时是空函数） */
 for(int i=0;i<nn;i++){
  uint64_t h0=hs[i];
  if(h0<=0x10000)continue;   /* 整数句柄不是命令流地址（同上方 trace 调用点的过滤口径） */
  CmdExecStat st;g_h_sub=sub;
  runtime_thread_note_op2(4,sub);   /* M5c：开始走这一条命令流（cmd_trace.c 的 nvn_cmd_exec_sink） */
  if(!nvn_cmd_exec_sink((const void *)(uintptr_t)h0,nvn_rec_cmdmem_readable,&sink,&st))g_h_exec_fail++;
  runtime_thread_note_op2(5,sub);   /* M5c：命令流走完 */
  nvn_vk_rt_flush();     /* M3i：流末尾还挂着的 RT 批次（清屏之后的绑定）也要真提交 */
  runtime_thread_note_op2(6,sub);   /* M5c：RT 批次已收口，接下来是 draw 批次收口 */
  nvn_vk_draw_flush();   /* M3r：本 submit 的真实 draw 批次收口（EndRenderPass/Submit/Wait） */
  nvn_cmd_trace_merge(&g_h_ops,&st.trace);
  g_h_cum.clears+=st.clears;g_h_cum.clear_fail+=st.clear_fail;g_h_cum.clear_noptr+=st.clear_noptr;
  g_h_cum.rt_binds+=st.rt_binds;g_h_cum.rt_fail+=st.rt_fail;
  g_h_cum.draws+=st.draws;g_h_cum.draw_fail+=st.draw_fail;g_h_cum.draw_skip+=st.draw_skip;
  g_h_skip_cnt+=st.draw_skip_cnt;g_h_skip_idx+=st.draw_skip_idx;g_h_skip_big+=st.draw_skip_big;
  g_h_fail_skip+=st.draw_fail_skip;g_h_fail_err+=st.draw_fail_err;g_h_fail_other+=st.draw_fail_other;
  g_h_incall+=st.draw_incall;g_h_incall_ok+=st.draw_incall_ok;
  g_h_call_markers+=st.call_markers;g_h_call_body_missing+=st.call_body_missing;
  g_h_cum.programs+=st.programs;g_h_cum.attribs+=st.attribs;g_h_cum.streams+=st.streams;g_h_cum.vbufs+=st.vbufs;
  if(runtime_rss_guard())break;   /* M5p A：护栏 tripped ⇒ 本 submit 余下的命令流不再走（绝不 abort）。
     不再 g_h_exec_fail++：同一次放弃已经在 nvn_cmd_exec_sink 返回 0 时由 :2970 记过一次，
     这里再加就是 M5o 实测的「exec_fail +2/提交」；且 M5p 起护栏带迟滞（回落即解除），不是永久位。 */
 }
 nvn_vk_m25a_merge_end();     /* M25A：窗口收口 —— 合并出来的那一批在这里真提交（开关全 0 时是空函数） */
 /* M3u P2：清屏顺序——同一 submit 里多次 clear 时，只有最后一次在内存里留下颜色。
    日志口径：n=次数，seq=依次颜色，effective=last（回读差分用这个做基线）。 */
 if(g_clear_seq_n>1){
  g_clear_dual_submits++;
  if(sub<=3||sub==600||(sub>600&&sub%1800==0)){
   char b[192];int o=snprintf(b,sizeof b,"[h] clear order @submit#%u: n=%u",sub,g_clear_seq_n);
   for(unsigned k=0;k<g_clear_seq_n&&o<(int)sizeof b-64;k++)
    o+=snprintf(b+o,sizeof b-(size_t)o," seq%u=%.3f,%.3f,%.3f,%.3f",k,g_clear_seq[k][0],g_clear_seq[k][1],g_clear_seq[k][2],g_clear_seq[k][3]);
   vk_boot_logf("%s effective=seq%u (last wins) dual_total=%llu",b,g_clear_seq_n-1,g_clear_dual_submits);
  }
 }
 /* 报告点：M3m 提前到 #1/#10/#100/#600，之后每 1800 次（60fps 下约 30s 一份）——
    #1/#10 让 clear_ok/draws 在崩溃前就落盘，不再出现「零证据」。
    M4n P4：m4n_b1 跑到 submit#1774 就崩，@#600 的下一份是 @#1800 ⇒ pipes_ok/skip_noprog/skip 拆分
    的终值全部「不可判」。改成 #600 之后每 300 一份（#900/#1200/#1500/#1800…），多出的 3 份行数可忽略，
    但让崩溃前的最后一份落点最多晚 300 submit（≈75s@4fps）而不是 1800。 */
 if(sub==1||sub%100==0)nvn_mem_report(sub);   /* M4i P0：每 100 submit 一条 RSS/fd 曲线 */
 if(sub==1800||sub==3600||sub==5400)nvn_mem_top(sub);   /* M4i P1：映射明细（三次足够定位） */
 if(sub==1||sub==10||sub==100||sub==300||sub==600||(sub>600&&sub%300==0))nvn_h_report(sub);
}
static void nvn_queue_submit_commands(void *q,int n,const void *h){(void)q;unsigned long sub=(unsigned long)atomic_fetch_add_explicit(&g_beat_nvn_submit,1,memory_order_relaxed)+1;   /* M3g 探针 #3：NVN 提交计数（入口即计，不参与后面的 dump 判定）；M3h：返回值即 submit 序号 */
 m103a_time_report(sub);   /* M103A P3：按时间触发的 [t] lru / [tex] miss class / [m103a] stat（≥30 s 一行） */
 m40h_tick();   /* M40H：每 submit 重读首绑纹理源首 64 B（≤64 id，开关 smo_m40h） */
 nvn_vk_m40j_tick();   /* M40J：每 600 submit 一条 cpsum（拷贝目标分桶，开关 smo_m40j） */
 { extern void nvn_vk_m40k_tick(unsigned long long);
   nvn_vk_m40k_tick((unsigned long long)sub); }   /* M40K：每 2 s 对 ≤16 个池区间做内容 hash 差分（开关 smo_m40k） */
 nvn_port_note_submit_h(h,n);   /* M5r：记下本笔提交的 handle 及其首 32 字节（判空 CB 归因用，只读边界受 cmdmem_readable 约束） */
 runtime_thread_note_op(1);   /* M5b：冻结线程表里 op=1 且 age 持续增长 = 卡在这笔 submit 的宿主实现里没返回 */
 {static unsigned nlogged;if(nlogged++<1)fprintf(stderr,"[nvn] QueueSubmitCommands %p n=%d -> retired immediately (no replay layer)\n",q,n);}
 /* Black-screen case dump trigger: present is never reached, the freeze watchdog cannot arm on frames, and hdc cannot signal
    the app — but submits happen on a guest thread (the one context where runtime_dump_threads() is proven safe), so dump once
    from here after enough submits to be past loading. */
 {static unsigned sc;unsigned now=++sc;if(now==200&&getenv("SMO_DUMP")){__attribute__((weak)) void runtime_dump_threads(void);if(runtime_dump_threads){fprintf(stderr,"[nvn] submit #200 -> one-shot thread dump\n");runtime_dump_threads();}}
  /* 改动 3（2026-11-02，task A/B/C 重做）：录制族已指向 nvn_replay/cmdbuf.c，EndRecording 返的「句柄」=
     该命令流首条 CmdRec 的宿主地址。本块把每个 submit 的每条句柄都走一遍 nvn_cmd_trace_walk
     （cmd_trace.c：顺着 CMD_JUMP 跨 chunk 走完整条流，语义=infra cmdbuf.c:679-712 的
     nvn_check_list/copy_list；每读一条先问 nvn_rec_cmdmem_readable 验边界，越界即停计数，绝不越界读），
     累加进全局直方图，在 submit #100/#200/#400/#600 各出一份累计报告（含累计 submit/命令数与当前
     presents），最后一份即完整降序表——万一只活到 #200 也已有可用数据。#600 后停走，控成本。 */
  {static CmdTrace cum;static uint64_t ok_l,bad_l;static int rpi,done;
   static const unsigned ckpt[4]={100,200,400,600};
   if(!done&&now>=ckpt[0]&&n>0&&h){
    int nn=n>16?16:n;
    for(int i=0;i<nn;i++){
     uint64_t hs0=((const uint64_t *)h)[i];
     if(hs0<=0x10000)continue;   /* 整数句柄（1/2/3…）不是命令流地址，跳过 */
     CmdTrace t;
     if(nvn_cmd_trace_walk((const void *)(uintptr_t)hs0,nvn_rec_cmdmem_readable,&t))ok_l++;else bad_l++;
     nvn_cmd_trace_merge(&cum,&t);
    }
    if(rpi<4&&now>=ckpt[rpi]){
     fprintf(stderr,"[cmdtrace] ===== report#%d @submit#%u 累计ok列表=%llu 提前停列表=%llu 当前presents=%llu =====\n",
             rpi+1,now,(unsigned long long)ok_l,(unsigned long long)bad_l,(unsigned long long)g_nvn_presents);
     nvn_cmd_trace_report(&cum);
     if(++rpi==4){done=1;fprintf(stderr,"[cmdtrace] 4 份累计报告出齐，跟踪器收工（后续 submit 不再走流）\n");}
    }
   }}
 }
 nvn_h_replay(n,(const uint64_t *)h,(unsigned)sub);   /* M3h：只回放 CLEAR_COLOR（其余 opcode 计数跳过） */
 nvn_rcpt_submit(n,(const uint64_t *)h);}   /* donor :2245 入回放队列；round15b：退休=回放完成，回执环此刻写真内存 */
static uint64_t nvn_command_buffer_end_recording(void *c){cmd_h *o=c;o->recording=0;o->ctl_used+=64;g_nvn_ctl_total+=64;uint64_t h=++g_nvn_handle_gen;nvn_rcpt_map(h,o);{static unsigned n;if(n++<4)fprintf(stderr,"[nvn] CommandBufferEndRecording -> handle %#llx hw_used=%#llx game_size=%#llx ctl_used=%#llx\n",(unsigned long long)h,(unsigned long long)o->hw_used,(unsigned long long)o->game_size,(unsigned long long)o->ctl_used);}return h;}   /* donor :1061/:1086：返句柄 + 控制内存记账 64B；P0 补 recording 清零与用量日志（RSTB 尺寸取证）；round15b 句柄入回执环映射 */
static void nvn_memory_pool_flush_mapped_range(const void *p,int64_t off,uint64_t n){NM_SC_N("nvnMemoryPoolFlushMappedRange");(void)p;(void)off;(void)n;{static unsigned l;if(l++<1)fprintf(stderr,"[nvn] MemoryPoolFlushMappedRange off=%lld n=%llu -> CPU-coherent, no-op\n",(long long)off,(unsigned long long)n);}}   /* donor :258 空实现：CPU 侧写入本就已就绪 */
static void nvn_buffer_flush_mapped_range(const void *b,int64_t off,uint64_t size){const buf_h *v=b;
 if(off<0||(uint64_t)off>v->size||size>v->size-(uint64_t)off){fprintf(stderr,"[nvn][RED] BufferFlushMappedRange OOB off=%lld size=%llu buflen=%llu\n",(long long)off,(unsigned long long)size,(unsigned long long)v->size);return;}   /* donor :297 越界 die→本表风格红日志 */
 dm_guest_write_prepare((uint8_t *)nvn_buffer_map(b)+(uintptr_t)off,(size_t)size);   /* donor 同行：喂 DM 写观察点 */
 {static unsigned n;if(n++<1)fprintf(stderr,"[nvn] BufferFlushMappedRange off=%lld size=%llu -> CPU-coherent\n",(long long)off,(unsigned long long)size);}}
static int nvn_window_acquire_texture(void *w,void *s,int *idx){win_h *o=w;int i=o->active?o->next++%o->active:0;if(idx)*idx=i;
 atomic_fetch_add_explicit(&g_beat_frames,1,memory_order_relaxed);   /* M3g 探针 #1：主循环心跳。nvnQueueAcquireTexture(:597) 与 nvnWindowAcquireTexture 都汇到本函数；每帧渲染前必须先拿窗口纹理下标，present 用同一 idx，故每帧必过此点。 */
 { extern void runtime_m7u_frame_check(void); runtime_m7u_frame_check(); }   /* M7u: clip-window shadow check per frame (no-op unless smo_m7u=1; read-only, no I/O) */
 { unsigned long long fr=atomic_fetch_add_explicit(&g_frame_anchor,1,memory_order_relaxed)+1;   /* M4f P1：帧锚点 */
   if(fr%600==0){ unsigned long long cf=0,ca=0,cs=0,ct=0,cd=0;float cc[4]={0,0,0,0};
     nvn_vk_clear_stats(&cf,&ca,&cs,&ct,&cd,cc);
     vk_boot_logf("[c] frame=%llu clears=%llu (suppressed=%llu) color=%.3f,%.3f,%.3f,%.3f supp_total=%llu dup_color=%llu anchor=%llu",
                  cf,ca,cs,cc[0],cc[1],cc[2],cc[3],ct,cd,fr); } }
 if(s){sync_h *y=s;y->fenced=3;y->fence=0;fn_note_fsync((uint64_t)(uintptr_t)s,0);}   /* donor :2505：acquire sync"显示不再读该张"=立即成立（fenced bit1=acquire 类标记）; M5o: count this as a fence signal */
 {static unsigned n;if(n<4){n++;fprintf(stderr,"[nvn] WindowAcquireTexture -> idx=%d active=%d sync=%p\n",i,o->active,s);} }return 0;}   /* donor :2471：next%active 轮转、SUCCESS=0；无显示链不等待 */
static int nvn_queue_acquire_texture(void *q,void *w,int *idx){(void)q;int r=nvn_window_acquire_texture(w,0,idx);{static unsigned n;if(n<4){n++;fprintf(stderr,"[nvn] QueueAcquireTexture -> idx=%d ret=%d\n",idx?*idx:-1,r);} }return r;}   /* donor :2513（GAME_ODYSSEY 同款）转发窗口获取；此前空桩返 0 但不写 *idx——游戏读未初始化下标 */
/* ---------- M7w (2026-11-07) present 账本：每 10 s 一桶的 present 行为量化。
   采集 = 每次 present 2 次 runtime_now_ns（present 每帧一次、非 SVC 热路径；smo_m7w 开关
   关闭时只多 1 次宽松原子读），打印在 runtime_m7w_pres_dump（threads.c 的 m7w poll 调）。
   dur = nvnQueuePresentTexture 进出墙钟（vk 清色/离屏/CPU 填充全含），dt = 相邻 present
   间隔。计数 plain 写（present 单线程），与 dump 的竞态=观测语义、如实披露。
   判定输入：帧时间 ≈ 绘制时间 还是 ≫ 绘制时间（卡在 present/等待）。 ---------- */
#define M7W_PB 64                      /* 64 x 10 s = 640 s 桶，boot 400 s 不回绕 */
typedef struct { unsigned n, ok, cpu, off, fail; unsigned long long dsum, dmax, isum, imax; } m7w_pb_t;
static m7w_pb_t g_m7w_pb[M7W_PB];
static unsigned long long g_m7w_pt0us; static int g_m7w_phave0;
void runtime_m7w_pres_capture(unsigned long long t0_ns, int ok, int off, int cpu) {
    extern uint64_t runtime_now_ns(void);
    unsigned long long t1 = runtime_now_ns();
    unsigned long long dur = t1 >= t0_ns ? (t1 - t0_ns) / 1000ull : 0, t0us = t0_ns / 1000ull;
    unsigned long long dt = (g_m7w_phave0 && t0us > g_m7w_pt0us) ? t0us - g_m7w_pt0us : 0;
    g_m7w_pt0us = t0us; g_m7w_phave0 = 1;
    m7w_pb_t *p = &g_m7w_pb[(unsigned)((t0us / 10000000ull) % M7W_PB)];
    p->n++; if (ok) p->ok++; if (off) p->off++; if (cpu) p->cpu++; if (!ok) p->fail++;
    p->dsum += dur; if (dur > p->dmax) p->dmax = dur;
    if (dt && dt < 10000000ull) { p->isum += dt; if (dt > p->imax) p->imax = dt; }
}
void runtime_m7w_pres_dump(unsigned tsec) {
    extern void vk_boot_logf(const char *fmt, ...);
    extern _Atomic int g_m7w_on;
    extern _Atomic unsigned long g_beat_frames, g_beat_nvn_submit;
    static unsigned lastb; static unsigned long long pd, pf, ps; static int armed;
    if (!atomic_load_explicit(&g_m7w_on, memory_order_relaxed)) { armed = 0; return; }
    if (!armed) { armed = 1; lastb = tsec / 10u; pd = pf = ps = 0; return; }
    unsigned curb = tsec / 10u;
    for (unsigned b = lastb; b < curb && b < lastb + 4; b++) {
        m7w_pb_t *p = &g_m7w_pb[b % M7W_PB];
        if (!p->n) continue;
        unsigned long long d = runtime_draws_total();
        unsigned long long fr = (unsigned long long)atomic_load_explicit(&g_beat_frames, memory_order_relaxed);
        unsigned long long sb = (unsigned long long)atomic_load_explicit(&g_beat_nvn_submit, memory_order_relaxed);
        vk_boot_logf("[m7w] pres t=%us w=%u-%us n=%u ok=%u cpu=%u off=%u fail=%u dur_avg=%lluus dur_max=%lluus dt_avg=%lluus dt_max=%lluus draws=%llu(+%llu) frames=%llu(+%llu) submit=%llu(+%llu)",
                     tsec, b * 10u, b * 10u + 10u, p->n, p->ok, p->cpu, p->off, p->fail,
                     (unsigned long long)(p->n ? p->dsum / p->n : 0), p->dmax,
                     (unsigned long long)(p->n > 1 ? p->isum / (p->n - 1) : 0), p->imax,
                     d, d > pd ? d - pd : 0, fr, fr > pf ? fr - pf : 0, sb, sb > ps ? sb - ps : 0);
        pd = d; pf = fr; ps = sb;
    }
    if (curb > lastb) lastb = curb;
}
/* ===== M40AV（2026-11-10）：游戏自己 present 的那张纹理 —— **只写不判**的记录全局 =====
   nvn_queue_present_texture 每帧把「这一帧游戏要给人看的那张」的存储身份（pk，与 vk 层 h_rt_slot.key 同域）、
   对象指针、尺寸、帧戳写进这里；nvn_vk.c 的 present_offscreen 只在 smo_m40av=1 时**只读**它当上屏源。
   av=0 时这里照写（4 个 store，无分支），但 vk 侧一个字都不读 ⇒ 行为逐字不变。 */
volatile unsigned long long g_av_cur_img, g_av_cur_tex, g_av_cur_stamp;
volatile unsigned g_av_cur_w, g_av_cur_h, g_av_cur_lookup;
volatile int g_av_cur_texid, g_av_cur_idx;
static void nvn_queue_present_texture(void *q,void *w,int idx){unsigned long long m7w_t0=0;int m7w_off=0,m7w_cpu=0;(void)q;g_nvn_present_enter++;g_nvn_presents++;m68b_present_note();   /* M68B：入口第一行计数（enter=客侧真的调进来了）*/runtime_thread_note_op2(8,(uint64_t)idx); {extern _Atomic int g_m7w_on; extern uint64_t runtime_now_ns(void); if(atomic_load_explicit(&g_m7w_on,memory_order_relaxed)) m7w_t0=runtime_now_ns();}   /* M5c：进上屏路径（stage/合成/回读都在这一段里） */ runtime_watch_tick(g_nvn_presents);/* 内容 watch 的 present 打点（OHOS 上 watch_n 恒 0，runtime_watch_add 提前 return 仅留痕，tick 只走 env 初始化，log-only） */
 /* M4ag P1（2026-11-04）：**游戏自己指定的上屏纹理**。nvnWindowBuilderSetTextures 把 n 张 NVNtexture
    交给窗口（:849 存进 win_builder_h.tex[]、nvn_window_initialize:858 拷进 win_h.tex[]），
    nvnWindowAcquireTexture 每帧轮转给出 idx，present 用同一个 idx ⇒ win->tex[idx] 就是
    「这一帧要给人看的那张」。这里把它解析成**存储身份**（池数据地址，与 vk 层 h_rt_slot.key 同口径；
    拿不到 mem 时退回对象指针，与 rt_bind 的 stk 记账同规则），交给 nvn_vk_present_src()。
    解析失败 ⇒ key=0，vk 层计 texmiss 并原样回落旧逻辑（不硬指）。 */
 { void *ptex=0; const win_h *wh=(const win_h *)w;
   if(w&&idx>=0&&idx<wh->ntex)ptex=wh->tex[idx];
   rt_ident ci;int ok=ptex?h_rt_lookup((uint64_t)(uintptr_t)ptex,&ci,0):0;
   /* M4ag P1b：**按 idx 记住上一次的存储身份**。boot#1 实测 h_rt_lookup 会间歇性 miss
      （侧表/快照表被后来 40 万次 Initialize 挤掉），key 变 0 ⇒ vk 层回落到旧链（屏幕切到别的内容）。
      窗口纹理的池数据地址在一次 boot 内不变，第一次解析成功就把它钉在 idx 上。 */
   static unsigned long long pstk[4];static unsigned char pok[4];static unsigned long long pcache_hit;
   unsigned long long pk=ok?(unsigned long long)(uintptr_t)(ci.mem?ci.mem:(const void*)ptex):0;
   if(pk&&idx>=0&&idx<4){pstk[idx]=pk;pok[idx]=1;}
   else if(idx>=0&&idx<4&&pok[idx]){pk=pstk[idx];pcache_hit++;}
   nvn_vk_present_src(pk,ok?ci.w:0,ok?ci.h:0,ok?ci.id:-1,idx);
   /* M40AV：记录本帧 present 的 image（只写不判；vk 侧 sno_m40av=1 时才读）。 */
   g_av_cur_img=pk; g_av_cur_tex=(unsigned long long)(uintptr_t)ptex;
   g_av_cur_w=ok?(unsigned)ci.w:0u; g_av_cur_h=ok?(unsigned)ci.h:0u; g_av_cur_lookup=ok?1u:0u;
   g_av_cur_texid=ok?ci.id:-1; g_av_cur_idx=idx; g_av_cur_stamp=(unsigned long long)g_nvn_presents;
   {static unsigned n;
    if(n<6||(g_nvn_presents%300)==0){n++;
     vk_boot_logf("[p] present tex=%p id=%d ext=%ux%u stk=%#llx src=%s idx=%d ntex=%d lookup=%d cachehit=%llu",
                  ptex,ok?ci.id:-1,ok?ci.w:0,ok?ci.h:0,pk,ok?ci.src:"none",idx,w?wh->ntex:-1,
                  ok?1:0,pcache_hit);}}}
 uint32_t ph=((uint32_t)g_nvn_presents>>4)&1;   /* 每 16 帧换一组清色：屏幕闪烁=帧在推进的可见证据（恒色帧分不清「出图了」与「黑屏了」） */
 /* M7d (2026-11-07)：实验开关（默认全 0，files/smo_m7d_*，echo 0 即回退；详见 nvn_vk.c M7d 块）。
    bit2=fmt：清色 r/b 对调（验证「红蓝=通道互换」）；bit4=same：跳过清色、强制 present_offscreen
    真图路径（验证「present 的不是画的那张图」）；bit8=clear：清色强制纯绿（验证「纯色=清色」）。
    m7d_pres 记账在 nvn_vk_present_clear 内部（nvn_vk_m7d_flags 每 present 调一次兼作节拍）。 */
 int m7d=nvn_vk_m7d_flags();
 uint32_t m7d_r=ph?0x1a:0xc8,m7d_g=0x5a,m7d_b=ph?0xb4:0x28;
 if(m7d&8){m7d_r=0;m7d_g=255;m7d_b=0;}
 if(m7d&2){uint32_t m7d_t=m7d_r;m7d_r=m7d_b;m7d_b=m7d_t;}
 int vk_ok=0;
 /* ===== M42F（2026-11-11）：present 取源改判 =====
    M42E r1 实证：真机 present_clear（VK_OHOS_native_buffer 清色）每帧都成功 ⇒ 下面的
    nvn_vk_present_offscreen（真图路径）**从不运行**，屏上只剩交替清色（纯橙 #C85A27）。
    这里在清色之前给「取合成 RT 上屏」一次机会：smo_m42f=1 才进（=0 时只是两次 weak 符号查询，
    vk_ok 仍是 0 ⇒ 旧链逐字不变）。返回 1 = 本帧已由真实取源写上屏，跳过清色。 */
 if(!vk_ok){ __attribute__((weak)) int nvn_vk_m42f_on(void);
   __attribute__((weak)) int nvn_vk_present_offscreen(void);
   if(nvn_vk_m42f_on && nvn_vk_m42f_on() && nvn_vk_present_offscreen){
     int m42f_pok=nvn_vk_present_offscreen();
     if(m42f_pok){vk_ok=1;m7w_off=1;} } }
 if(m7d&4){ __attribute__((weak)) int nvn_vk_present_offscreen(void);
   int m7d_pok=nvn_vk_present_offscreen?nvn_vk_present_offscreen():0;
   if(m7d_pok){vk_ok=1;m7w_off=1;vk_boot_logf("[m7d] same: offscreen path presented (真图上屏)");}}
  /* M80A（2026-11-12）：offscreen-first —— 每次 present 先把**真实窗口纹理**当源
     （win->tex[idx]，身份已由上面的 nvn_vk_present_src 按 WindowAcquireTexture 的 idx 解析）：
     取源 → staging → win_fill 等比 blit，成功即跳过清色；失败才落回 present_clear（清色兜底）。
     m80a=0 或 m42f=1 时本块只多两次 weak 符号查询，旧链一行不动。 */
  if(!vk_ok){ __attribute__((weak)) int nvn_vk_m80a_first(void);
    __attribute__((weak)) int nvn_vk_present_offscreen(void);
    if(nvn_vk_m80a_first && nvn_vk_m80a_first() && nvn_vk_present_offscreen){
      int m80a_pok=nvn_vk_present_offscreen();
      if(m80a_pok){vk_ok=1;m7w_off=1;} } }
  if(!vk_ok) vk_ok=nvn_vk_present_clear(m7d_r,m7d_g,m7d_b);
 { /* 模拟器无 Vulkan ICD（vkCreateInstance=-9）→ 显示链回落到 CPU 填色：直接把清屏色写进 XComponent 窗口。
      判据是「屏幕上有真实像素」，不要求像素由 Vulkan 产生；这里只换出图通道，不动 NVN 语义。 */
   if(!vk_ok){ __attribute__((weak)) void *nvn_vk_native_window(void); __attribute__((weak)) int nvn_win_fill(void*,unsigned,unsigned,unsigned);
     __attribute__((weak)) int nvn_vk_present_offscreen(void);
     int pok = nvn_vk_present_offscreen ? nvn_vk_present_offscreen() : 0;   /* M3o：离屏 RT 的三角形走软件上屏（不经 swapchain）。1=本帧屏幕归它，勿再纯色覆盖 */
     if(pok) vk_ok=1,m7w_off=1;
     else if(nvn_vk_native_window&&nvn_win_fill){ static int filled; int ok=nvn_win_fill(nvn_vk_native_window(),m7d_r,m7d_g,m7d_b); m7w_cpu=1;
       if(!filled&&ok){filled=1;fprintf(stderr,"[nvn] present -> CPU fill OK (Vulkan unavailable, window path works)\n");} vk_ok=ok; } } }   /* round23批1落点3：NVN present → vk acquire+经典通道清色+present（donor third/d2r-mobile dm/android/vulkan_present.cpp present() 同位；带源图 blit 属批2）。SMO_VK=0/未起层时内部返 0，宿主行为同此前纯记账版 */
 if(vk_ok)g_nvn_screens++;   /* M67B：本帧确实有画面交给了某条显示路径（present_clear/present_offscreen/win_fill 任一成功）。纯观察量。 */
 {static unsigned n;if(n<4){n++;fprintf(stderr,"[nvn] QueuePresentTexture idx=%d present #%llu vk=%d\n",idx,(unsigned long long)g_nvn_presents,vk_ok);}} {extern void runtime_m7w_pres_capture(unsigned long long,int,int,int); if(m7w_t0) runtime_m7w_pres_capture(m7w_t0,vk_ok,m7w_off,m7w_cpu);}}   /* donor :2909 出图计帧（g_frame++）；批1 接通显示链后计数语义不变 */
/* round48 方向=submit-chain 审计结论：四提交入口首用 0 次 + 全程 stub 普查（round47 dump 35 项）无任何
   fence/sync/queue 状态查询被调 ⇒ 提交链从未被进入，:439-458 现有桩没有一个是堵点（没人问它们）；
   堵点在上游装库状态机（F-H 打点对象，threads.c:663-713）。本块只补"链路一旦通电就会被问到"的
   donor 同语义件，防下一轮被返-0 桩毒化：
   · QueueGetError（donor :722-724）：GPU_NO_ERROR=0x0（include/nvn.h:572）；NVNqueueErrorInfo 是
     1 字节哑元（nvn.h:108-111），清 1 字节=donor memset 等价。表外返-0 桩碰巧同值但不清 info。
   · Event 族（donor :685-695；ev_builder_t/ev_t=nvn_obj.h:59-60）：NVNevent 是提交完成的 CPU 可见
     原语（CommandBufferSignalEvent 从命令流写它）；EventInitialize 返 NVNboolean——返-0 桩=「初始化
     失败」，游戏提交前置若建 event 立即夭折。存储=游戏池内存字，宿主 CPU 直写（零延迟完成同款取舍）。
     SignalMode：WRITE=0x0 / ADD=0x1（nvn.h:661-662）。
   · Buffer/TextureFinalize（donor :285/:443）：上传批次组成件（"pool map flags=0x24+BufferFinalize"），
     清宿主已知前缀，防陈旧 magic 复用误判（NVNbuffer 客体分配 0x30≥buf_h 24B，nvn.h:200，不越界）。 */
static int nvn_queue_get_error(void *q,void *info){(void)q;if(info)memset(info,0,1);{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] QueueGetError -> GPU_NO_ERROR(0) info 1B zeroed\n");}return 0;}   /* GPU_NO_ERROR=0x0 */
typedef struct{const void *pool;int64_t off;} ev_builder_h;   /* =ev_builder_t（nvn_obj.h:59） */
typedef struct{uint32_t magic;volatile uint32_t *val;} ev_h;  /* =ev_t（nvn_obj.h:60） */
static void nvn_event_builder_set_defaults(void *b){memset(b,0,sizeof(ev_builder_h));}   /* donor :685 */
static void nvn_event_builder_set_storage(void *b,const void *p,int64_t off){((ev_builder_h *)b)->pool=p;((ev_builder_h *)b)->off=off;}   /* donor :686 */
static uint64_t nvn_event_initialize(void *e,const void *b){const ev_builder_h *s=b;const pool_h *p=s->pool;
 if(!p||!p->mem){fprintf(stderr,"[nvn][RED] EventInitialize 没有存储 pool=%p\n",s->pool);fn_note_life((uint64_t)(uintptr_t)e,FN_EVT,0);return 0;}   /* donor :688-689 LOG【红】返 0 */
 *(ev_h *)e=(ev_h){0x45564E54,(volatile uint32_t *)((uint8_t *)p->mem+s->off)};   /* 'EVNT'，donor :690 */
 fn_note_life((uint64_t)(uintptr_t)e,FN_EVT,1);
 {uint32_t v0=*(volatile uint32_t *)((uint8_t *)p->mem+s->off);fn_rec *r0=fn_get((uint64_t)(uintptr_t)e,FN_EVT);if(r0){r0->val_first=v0;r0->val_last=v0;}}   /* M5o: record the initial word (same read as nvn_event_get_value, no extra deref) */
 {static unsigned n;if(n++<4)fprintf(stderr,"[nvn] EventInitialize %p pool=%p off=%#llx -> val@%p\n",e,s->pool,(unsigned long long)s->off,(void *)(uintptr_t)((uint8_t *)p->mem+s->off));}
 return 1;}
static void nvn_event_finalize(void *e){((ev_h *)e)->magic=0;fn_note_life((uint64_t)(uintptr_t)e,FN_EVT,0);}   /* donor :692 */
static uint32_t nvn_event_get_value(const void *e){const ev_h *o=e;uint32_t v=o->val?*o->val:0;fn_note_ev_query((uint64_t)(uintptr_t)e,v);return v;}   /* donor :693 read the pool word (NULL val -> 0, normal path never hits it); M5o records queries/value changes */
static void nvn_event_signal(void *e,int mode,uint32_t v){ev_h *o=e;if(!o->val)return;if(mode==1)*o->val+=v;else *o->val=v;fn_note_ev_sig((uint64_t)(uintptr_t)e,*o->val,FN_EVT);{static unsigned n;if(n++<4)fprintf(stderr,"[nvn] EventSignal %p mode=%s v=%u -> val=%u\n",e,mode==1?"ADD":"WRITE",v,*o->val);}}   /* donor :694-695 */
static void nvn_buffer_finalize(void *b){memset(b,0,sizeof(buf_h));}   /* donor :285 memset 全对象→宿主清已知前缀 */
static void nvn_texture_finalize(void *t){vk_tex_detach((tex_h *)t);((tex_h *)t)->magic=0;}   /* donor :356 mtl_tex_release+清 magic 同位（批2：VkImage 一并释放，防同池内存 Finalize+重建泄漏） */
/* 终局五段 P0/P1（crash_1031 终局五段章）：头号断链=命令内存记账返 0——游戏累加 GetCommandMemoryUsed
   定 RSTB 资源堆尺寸（donor :887 注释原文），桩返 0 ⇒ 堆 0 尺寸/判满 ⇒ 录制停摆（时序铁证：
   GetCommandMemoryUsed×1 后一切录制静默）。本块把 CommandBuffer 录制族从分槽空桩升为 entries 真实现：
   cb_account 按 donor Maxwell 字数表记 hw_used——有 hw_fix 实测值的用精确值（hle_nvn.c:887 注释表 +
   :1804-1984 调用点），无 hw_fix 的 setter/清除/拷贝按 donor 记录尺寸平价近似 24B（donor 实际 16-40B，
   RSTB 只需量级）；控制内存记账在 EndRecording（donor :1086 ctl_used+=64）。Used/Free 六件 = donor
   :851-869 的 min/free 公式；Queue Total 族 donor 没有（queue_t 仅 {magic,dev,submits}），按任务定义
   用全局累计（单队列游戏），Compute 恒 0（无计算用量记账）。Compare 三件=donor :456-459 的对象等价
   （描述符逐字节+池指针，非像素比较）；Origin/Depth=设备字段回显（donor :210-221，默认 LOWER_LEFT/
   NEAR_IS_MINUS_W，donor 注释：NVN 默认深度模式即枚举 0）。Texels=真池内存搬运（P1，解除"写空→比对
   失败→重试自旋"）：pitch 存储按真实 stride、块线性按同一行式近似（自洽读写，本宿主无 GPU 消费方，
   donor 走 mtl_read/write_texels 我们直拷池）。Timestamp→runtime_now_ns()（donor 无实现）。 */
static void cb_account(cmd_h *o,uint64_t bytes){o->hw_used+=bytes;g_nvn_cmd_total+=bytes;}
static void nvn_command_buffer_begin_recording(void *c){cmd_h *o=c;if(o->recording)fprintf(stderr,"[nvn][RED] BeginRecording 重入 (recording=1)\n");o->recording=1;nvn_rcpt_drop(o);{static unsigned n;if(n++<4)fprintf(stderr,"[nvn] BeginRecording %p hw_used=%#llx\n",c,(unsigned long long)o->hw_used);}}   /* donor :895 置 recording（重入红日志同 donor）；round15b：重录=命令内存重置，该 CB 回执弃（donor 同语义） */
static uint64_t nvn_command_buffer_is_recording(const void *c){return ((const cmd_h *)c)->recording;}   /* P0-2：此前空桩返 0=恒"未录制" */
static uint64_t nvn_command_buffer_get_command_memory_used(const void *c){const cmd_h *o=c;uint64_t r=o->hw_used<o->game_size?o->hw_used:o->game_size;{static unsigned n;if(n++<4)fprintf(stderr,"[nvn] GetCommandMemoryUsed -> %#llx (hw=%#llx given=%#llx; RSTB sizing input)\n",(unsigned long long)r,(unsigned long long)o->hw_used,(unsigned long long)o->game_size);}return r;}   /* donor :867；游戏累加它定 RSTB 堆尺寸 */
static uint64_t nvn_command_buffer_get_command_memory_free(const void *c){const cmd_h *o=c;return o->game_size-(o->hw_used<o->game_size?o->hw_used:o->game_size);}   /* donor :869 */
static uint64_t nvn_command_buffer_get_control_memory_used(const void *c){const cmd_h *o=c;return o->ctl_used<o->ctl_size?o->ctl_used:o->ctl_size;}   /* donor :855 */
static uint64_t nvn_command_buffer_get_control_memory_free(const void *c){const cmd_h *o=c;return o->ctl_size-(o->ctl_used<o->ctl_size?o->ctl_used:o->ctl_size);}   /* donor :857 */
static void *nvn_command_buffer_get_memory_callback(const void *c){return ((const cmd_h *)c)->cb;}   /* donor :871 */
static void *nvn_command_buffer_get_memory_callback_data(const void *c){return ((const cmd_h *)c)->cb_data;}   /* donor :873（NOPROTO） */
static void nvn_command_buffer_set_memory_callback(void *c,void *fn){((cmd_h *)c)->cb=fn;}
static void nvn_command_buffer_set_memory_callback_data(void *c,void *d){((cmd_h *)c)->cb_data=d;}
static void nvn_command_buffer_set_command_memory_callback_enabled(void *c,int on){NM_SC_N("nvnCommandBufferSetCommandMemoryCallbackEnabled");(void)c;(void)on;{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] SetCommandMemoryCallbackEnabled %d (host never exhausts recording memory)\n",on);}}   /* 宿主不占录制内存，回调永不触发 */
static void nvn_command_buffer_set_debug_label(void *c,const char *l){NM_SC_N("nvnCommandBufferSetDebugLabel");(void)c;(void)l;}
static void nvn_command_buffer_finalize(void *c){cmd_h *o=c;o->magic=0;o->recording=0;}   /* donor :780 清 magic */
static uint64_t nvn_queue_get_total_command_memory_used(const void *q){(void)q;return g_nvn_cmd_total;}   /* donor 无此族；全局累计（单队列），Reset 清零 */
static uint64_t nvn_queue_get_total_control_memory_used(const void *q){(void)q;return g_nvn_ctl_total;}
static uint64_t nvn_queue_get_total_compute_memory_used(const void *q){NM_SC_N("nvnQueueGetTotalComputeMemoryUsed");(void)q;return 0;}   /* 无计算用量记账（真值待游戏取用证据） */
static void nvn_queue_reset_memory_usage_counts(void *q){(void)q;g_nvn_cmd_total=0;g_nvn_ctl_total=0;}
static uint64_t g_nvn_copy_row_stride,g_nvn_copy_image_stride;   /* SetCopyRowStride/GetCopyRowStride 回显对（donor 走记录字段；宿主静态回显） */
static void nvn_command_buffer_set_copy_row_stride(void *c,uint64_t s){(void)c;g_nvn_copy_row_stride=s;}
static uint64_t nvn_command_buffer_get_copy_row_stride(const void *c){(void)c;return g_nvn_copy_row_stride;}
static void nvn_command_buffer_set_copy_image_stride(void *c,uint64_t s){(void)c;g_nvn_copy_image_stride=s;}
static uint64_t nvn_command_buffer_get_copy_image_stride(const void *c){(void)c;return g_nvn_copy_image_stride;}
/* —— 记账录制族：精确/公式件（donor hw_fix 实测字节，签名照 nvn_protos.h）—— */
/* M4ad P1：**盲区计数**。下面这几个入口在符号表里指向的仍是「只记账、不写命令流」的桩
   （draw_elements_instanced / draw_arrays_instanced / draw_elements_base_vertex / indirect 族），
   所以它们既不会出现在 [h] opcode 里，也不会被回放——游戏若真的用这些形态下几何 draw，
   我们连「有没有」都看不见。这里给调用次数，让盲区可判（本轮不修）。
   定义放在使用点（nvn_h_report）之前是 C 的硬要求，故用文件作用域 + 上面的 extern 引用。 */
unsigned long long g_nr_draw_arrays_inst,g_nr_draw_elems_inst,g_nr_draw_elems_basev,g_nr_draw_indirect;
unsigned long long g_nr_rec_arrays_inst;   /* M5p C：真进了命令流的 DrawArraysInstanced 笔数（接线后应与 g_nr_draw_arrays_inst 此消彼长） */
static void nvn_command_buffer_draw_elements(void *c,int pr,int it,int n,uint64_t ib){(void)pr;(void)it;(void)n;(void)ib;cb_account(c,48);}   /* donor :1966：12 字 */
static void nvn_command_buffer_draw_elements_base_vertex(void *c,int pr,int it,int n,uint64_t ib,int bv){(void)pr;(void)it;(void)n;(void)ib;(void)bv;g_nr_draw_elems_basev++;cb_account(c,44);}   /* donor :1969：11 字 */
static void nvn_command_buffer_draw_elements_instanced(void *c,int pr,int it,int n,uint64_t ib,int bv,int bi,int inst){(void)pr;(void)it;(void)n;(void)ib;(void)bv;(void)bi;(void)inst;g_nr_draw_elems_inst++;cb_account(c,44);}   /* donor :1972：同 6 参 draw 宏 */
static void nvn_command_buffer_draw_elements_indirect(void *c,int pr,int it,uint64_t ib,uint64_t args){NM_SC_N("nvnCommandBufferDrawElementsIndirect");(void)pr;(void)it;(void)ib;(void)args;g_nr_draw_indirect++;cb_account(c,44);}   /* donor :1981 */
#define CB_DRAW_ARRAYS(name) static void name(void *c,int pr,int first,int n){(void)pr;(void)first;(void)n;cb_account(c,32);nvn_vk_note_draw_arrays();}   /* donor p_draw 记录 8+20→32B（无 hw_fix，记录尺寸）；M4s P3：顺带计「落在纯深度 pass 里的 DrawArrays」 */
CB_DRAW_ARRAYS(nvn_command_buffer_draw_arrays)
/* M4ad P1：instanced 形态单独计数（宏体的 nvn_vk_note_draw_arrays 只在纯深度 pass 里生效，
   不能当总量用）。M5p C：本桩已从符号表撤下（改指 nvn_cb_rec_draw_arrays_instanced），
   保留定义只为「旧口径对照」——接线后这里应恒 0。 */
static void nvn_command_buffer_draw_arrays_instanced(void *c,int pr,int first,int n){(void)pr;(void)first;(void)n;g_nr_draw_arrays_inst++;cb_account(c,32);nvn_vk_note_draw_arrays();}   /* donor :1961：inst<=0 不画（我们只记账，同额） */
/* M5p C：DrawArraysInstanced 真接线（符号表 :4276）。录制本身由 cmdbuf.c:662 做，命令内存记账也在
   那边的 rec() 里（同 DrawElements 系列，不再 cb_account，否则双记）。这里只留深度 pass 探针 + 接线计数。 */
static void nvn_cb_rec_draw_arrays_instanced(void *c,int pr,int first,int n,int bi,int inst)
{
 g_nr_rec_arrays_inst++;
 nvn_vk_note_draw_arrays();
 nvn_rec_draw_arrays_instanced(c,pr,first,n,bi,inst);
}
CB_DRAW_ARRAYS(nvn_command_buffer_draw_arrays_indirect)
static void nvn_command_buffer_bind_vertex_attrib_state(void *c,int n,const void *st){NM_SC_N("nvnCommandBufferBindVertexAttribState");(void)st;if(n<0)n=0;cb_account(c,68);}   /* donor :1804：驱动恒写全 16 属性格式=17 字（68B 定值） */
static void nvn_command_buffer_bind_vertex_stream_state(void *c,int n,const void *st){NM_SC_N("nvnCommandBufferBindVertexStreamState");(void)st;if(n<0)n=0;cb_account(c,4*(16+(uint64_t)n));}   /* donor :1808：16+n 字 */
static void nvn_command_buffer_bind_vertex_buffer(void *c,int first,uint64_t addr,uint64_t n){NM_SC_N("nvnCommandBufferBindVertexBuffer");(void)first;(void)addr;(void)n;cb_account(c,24);}   /* 单个 6 字（donor :1812 数组版 24*n） */
static void nvn_command_buffer_bind_vertex_buffers(void *c,int first,int n,const void *rg){NM_SC_N("nvnCommandBufferBindVertexBuffers");(void)first;(void)rg;if(n<0)n=0;cb_account(c,24*(uint64_t)n);}   /* donor :1812 */
static void nvn_command_buffer_bind_uniform_buffer(void *c,int st,int i,uint64_t a,uint64_t n){NM_SC_N("nvnCommandBufferBindUniformBuffer");(void)st;(void)i;(void)a;(void)n;cb_account(c,20);}   /* donor :1815：CB_SIZE/ADDRESS 4 字+CB_BIND 1 字 */
static void nvn_command_buffer_bind_uniform_buffers(void *c,int st,int first,int n,const void *rg){NM_SC_N("nvnCommandBufferBindUniformBuffers");(void)st;(void)first;(void)rg;if(n<0)n=0;cb_account(c,20*(uint64_t)n);}   /* donor :1818 */
static void nvn_command_buffer_bind_storage_buffer(void *c,int st,int i,uint64_t a,uint64_t n){NM_SC_N("nvnCommandBufferBindStorageBuffer");(void)st;(void)i;(void)a;(void)n;cb_account(c,20);}   /* donor :1821：macro16 1 字+CB_DATA 4 字 */
static void nvn_command_buffer_bind_storage_buffers(void *c,int st,int first,int n,const void *rg){(void)st;(void)first;(void)rg;if(n<0)n=0;cb_account(c,20*(uint64_t)n);}   /* donor :1824 */
#define CB_BIND_ONE(name) static void name(void *c,int st,int i,uint64_t h){NM_SC(name);(void)st;(void)i;(void)h;cb_account(c,24);}   /* donor :1901-1907：单项 6 字；M4am：按名计数 */
CB_BIND_ONE(nvn_command_buffer_bind_texture)
CB_BIND_ONE(nvn_command_buffer_bind_image)
CB_BIND_ONE(nvn_command_buffer_bind_separate_texture)
CB_BIND_ONE(nvn_command_buffer_bind_separate_sampler)
#define CB_BIND_MANY(name) static void name(void *c,int st,int first,int n,const void *h){NM_SC(name);(void)st;(void)first;(void)h;if(n<0)n=0;cb_account(c,16+8*(uint64_t)n);}   /* donor :1892-1898：64 位句柄 16+8n；M4am：按名计数 */
CB_BIND_MANY(nvn_command_buffer_bind_textures)
CB_BIND_MANY(nvn_command_buffer_bind_images)
CB_BIND_MANY(nvn_command_buffer_bind_separate_textures)
CB_BIND_MANY(nvn_command_buffer_bind_separate_samplers)
/* ---- M3u（2026-11-03）P1：UpdateUniformBuffer 真落地 ----
   语义（签名 nvn_protos.h:428，实现 donor hle_nvn.c:1257-1264）：
     nvnCommandBufferUpdateUniformBuffer(cb, buf:NVNbufferAddress, bufsz:size_t,
                                         off:ptrdiff_t, n:size_t, data:const void*)
   donor 录制期把 data 拷进命令内存、提交时写到 buf+off（调用方录制后改原 data 不影响已录内容）。
   本宿主选「录制期直接写客体内存」：回放侧是在 QueueSubmitCommands 回放 BIND_UNIFORM_BUFFERS 时
   按地址读客体内存再上送（nvn_vk.c nvn_vk_bind_uniform_buffers），而全部录制在 submit 之前结束，
   因此录制期 memcpy 到 buf+off 的字节在 bind 回放时必已可见——不需要再往命令流里存一份数据副本。
   代价（已知近似，非本轮新增）：同一 CB 内「改 A 段 → draw → 再改 A 段」两次都读到最终值；
   回放侧本来就按 submit 粒度读内存。
   边界：n==0/空指针/负 off/超 4 MiB/buf 或 dst 不是可写用户空间地址/off+n 越过 bufsz → 只计数不写、不崩。
   boot#1 实测（m3u）：本作的 NVNbufferAddress 是**宿主堆地址**（buf=0x6e0bcd1000，与 nvnBufferGetAddress
   的「池 mem+off」同源），不是 infra 客体窗口 0x7000000000——上一版按 GUEST_LO/HI 拦截把全部 14315+ 次
   更新判成 bufrange。故闸门改成「用户空间区间 + 本 buffer 声明范围」两条。 */
#define NVN_USER_LO  0x10000ull           /* 低于它的不可能是真实映射（同 glue_cmdbuf.c put 的口径） */
#define NVN_USER_HI  0x800000000000ull    /* 48 位用户空间上界（aarch64 canonical low half） */
#define NVN_UB_MAX   (4u<<20)
static unsigned g_ub_logged;
static void nvn_command_buffer_update_uniform_buffer(void *c,uint64_t buf,uint64_t bufsz,int64_t off,uint64_t n,const void *data){
 cb_account(c,24+n);   /* donor :1259 命令内存 24+n（本宿主只记账，RSTB 尺寸口径不变） */
 g_ub_n++;
 uint64_t dst=0;const char *why=NULL;
 if(!data||!n)why="nulldata";
 else if(n>NVN_UB_MAX)why="toobig";
 else if(off<0)why="negoff";
 else if(buf<NVN_USER_LO||buf>=NVN_USER_HI)why="bufrange";
 else if((uint64_t)off>bufsz||n>bufsz-(uint64_t)off)why="oob";
 else{dst=buf+(uint64_t)off;if(dst<NVN_USER_LO||dst>=NVN_USER_HI||n>NVN_USER_HI-dst)why="dstrange";}
 if(why){
  g_ub_bad++;
  if(g_ub_bad<=8)vk_boot_logf("[u] upd buf=%#llx off=%u sz=%u ok=0 why=%s bufsz=%llu",(unsigned long long)buf,
   (unsigned)off,(unsigned)n,why,(unsigned long long)bufsz);
  return;
 }
 dm_guest_write_prepare((void *)(uintptr_t)dst,(size_t)n);   /* 写观察点（同 BufferFlushMappedRange 用法） */
 nvn_vk_m40j_cp(3u,(void *)(uintptr_t)dst,(uint64_t)n,buf+(uint64_t)off);   /* M40J：UB 写客体 */
 memcpy((void *)(uintptr_t)dst,data,(size_t)n);
 g_ub_ok++;g_ub_bytes+=n;
 if(g_ub_logged<16){g_ub_logged++;vk_boot_logf("[u] upd buf=%#llx off=%u sz=%u ok=1 dst=%#llx",(unsigned long long)buf,
  (unsigned)off,(unsigned)n,(unsigned long long)dst);}
}
/* M4ai P2：ClearBuffer 的实现在下方「GPU 写 opcode 台账」块（带按名计数），此处不再重复定义。 */
static void nvn_command_buffer_barrier(void *c,int bits){NM_SC_N("nvnCommandBufferBarrier");(void)bits;cb_account(c,16);}   /* donor :2015 记录 8+4→16B */
static void nvn_command_buffer_set_viewports(void *c,int first,int n,const float *v){NM_SC_N("nvnCommandBufferSetViewports");(void)first;(void)v;if(n<0)n=0;cb_account(c,24+4*(uint64_t)n);}   /* donor :1515 RECN 4n 浮点 */
/* —— round15b 回执环（receipt ring）：GPU 值写入族对齐 donor 时序——
   donor（hle_nvn.c:873-889/:2450-2454）：录制只记 OP token（OP_SIGNAL_EVENT/OP_REPORT_COUNTER），
   QueueSubmitCommands 入回放队列，回放完成时 post_run(:1474) 才写真内存。14b 的「录制即写」是
   时序近似（值在提交前就可见——donor 不会）；emu14 验收遗留问题①「核实回执写入是否真落地」：
   15b 起 record 存回执环、submit 退休（本宿主无 GPU，提交即回放完成）时写内存：
   · ReportCounter：16B {counter@0, timestamp@8}（NVNcounterData=nvn.h:91-94）memcpy 到 bufferAddress；
     timestamp = guest_ns()×384/625（donor nvn_mtl.m:1280 gpu_ticks + nvn_timestamp.h:6，614.4MHz tick），
     取**退休时刻**（donor=回放完成时刻）；counter 字段 type∈{TIMESTAMP=0, TIMESTAMP_TOP=0x10} 时
     =timestamp 否则 0（post_run kind=2）。SAMPLES_PASSED(0x1) donor 走可见性槽求和（kind=3），本宿主
     无光栅 sum 恒 0 → 落同式。
   · ReportValue/SignalEvent：event 写（post_run kind=1）WRITE(0x0)覆写/ADD(0x1)累加（nvn.h:661-662）；
     ReportValue 写 (volatile uint32_t*)bufferAddress（donor :2450-2454）；SignalEvent 退休时重读
     ev->val（ev_t=nvn_obj.h:60 {magic,val}，donor 回放时才解引用）再写。
   · ResetCounter：donor :884-885 记 16B 回放只累计统计不写 guest 内存 → 纯记账 16B，不入环。
   生命周期（donor 命令内存语义）：BeginRecording 重录=命令内存重置 → 该 CB 回执全弃；同一地址
   CommandBufferInitialize 复用同弃。同句柄重复提交=donor 重复回放 → 回执保留、每次提交重写。
   环=宿主侧静态存储（cmd_h 是 guest 对象 overlay，nvn_obj.h:70-87 字段序锁死不可扩）；低频路径
   （14c 全程 ReportCounter×4）一把 host mutex 足够。溢出丢最旧计数不红日志（重建 CB 段即恢复）。
   计数器（回执写入门，dump 侧 threads.c 打印）：writes=退休真实落内存次数（验收位）、recorded 分
   kind、pending=环内未退休、unknown_handle=提交的句柄无映射（Begin 重录后旧句柄再提交，donor 同
   位置重放新内容的近似为不写——计数暴露即可）、dropped=环溢出。写前喂 dm_guest_write_prepare
   （写观察点，donor sbw 同款；:507 buffer_flush_mapped 同用法）。—— */
static void nvn_cb_event_write(volatile uint32_t *val,uint32_t mode,uint32_t v){
 if(!val){fprintf(stderr,"[nvn][RED] event 写 NULL dst（mode=%u v=%u）\n",mode,v);return;}
 dm_guest_write_prepare((void *)val,4);
 if(mode==1)*val+=v;else *val=v;
}
#define NVN_RCPT_CAP 256
#define NVN_RCPT_HMAP 256   /* round16-A：双键登记后一 CB 占两槽，64 槽在百次级 EndRecording 后即环溢出丢早期键，提到 256 */
typedef struct{void *cmd;uint8_t kind,mode;uint32_t v;uint64_t a;} rcpt_t;   /* kind:1=counter(a=dst,v=type) 2=value(a=dst,v) 3=signal(a=ev*,mode) */
static struct{
 rcpt_t r[NVN_RCPT_CAP];unsigned n;
 uint64_t hmap_h[NVN_RCPT_HMAP];void *hmap_c[NVN_RCPT_HMAP];unsigned hmap_n,hmap_next;
 uint64_t writes,rec_cnt,rec_val,rec_sig,dropped,unknown_handle,submits;
 uint64_t fb_writes,bad_dst;   /* M5p B：句柄未命中时按「提交即退休」兜底写出的笔数 / 目标地址越界被拒的笔数 */
} g_rcpt;
static pthread_mutex_t g_rcpt_mx=PTHREAD_MUTEX_INITIALIZER;
static uint64_t nvn_gpu_ticks(void){return (uint64_t)((__uint128_t)guest_ns()*384/625);}
static void nvn_rcpt_push(cmd_h *o,uint8_t kind,uint8_t mode,uint32_t v,uint64_t a){
 pthread_mutex_lock(&g_rcpt_mx);
 if(g_rcpt.n<NVN_RCPT_CAP)g_rcpt.r[g_rcpt.n++]=(rcpt_t){o,kind,mode,v,a};
 else{memmove(g_rcpt.r,g_rcpt.r+1,sizeof(rcpt_t)*(NVN_RCPT_CAP-1));g_rcpt.r[NVN_RCPT_CAP-1]=(rcpt_t){o,kind,mode,v,a};g_rcpt.dropped++;}
 if(kind==1)g_rcpt.rec_cnt++;else if(kind==2)g_rcpt.rec_val++;else g_rcpt.rec_sig++;
 pthread_mutex_unlock(&g_rcpt_mx);
}
static void nvn_rcpt_drop(void *cmd){   /* BeginRecording 重录 / Initialize 复用：该 CB 回执与句柄映射全弃 */
 pthread_mutex_lock(&g_rcpt_mx);
 unsigned w=0;for(unsigned i=0;i<g_rcpt.n;i++)if(g_rcpt.r[i].cmd!=cmd)g_rcpt.r[w++]=g_rcpt.r[i];
 g_rcpt.n=w;
 for(unsigned i=0;i<g_rcpt.hmap_n;i++)if(g_rcpt.hmap_c[i]==cmd)g_rcpt.hmap_c[i]=NULL;
 pthread_mutex_unlock(&g_rcpt_mx);
}
static void nvn_rcpt_map(uint64_t h,cmd_h *c){   /* EndRecording：句柄→CB 对象（submit 侧回执反查） */
 pthread_mutex_lock(&g_rcpt_mx);
 unsigned k=g_rcpt.hmap_next%NVN_RCPT_HMAP;g_rcpt.hmap_next++;
 g_rcpt.hmap_h[k]=h;g_rcpt.hmap_c[k]=c;
 if(g_rcpt.hmap_n<NVN_RCPT_HMAP)g_rcpt.hmap_n++;
 pthread_mutex_unlock(&g_rcpt_mx);
}
static cmd_h *nvn_rcpt_lookup(uint64_t h){
 /* hmap_next 是下一个要写入的槽：已写入项位于 [hmap_next-hmap_n, hmap_next-1]，须从最近写入的槽往回扫 */
 for(unsigned i=0;i<g_rcpt.hmap_n;i++){unsigned k=(g_rcpt.hmap_next+NVN_RCPT_HMAP-1-i)%NVN_RCPT_HMAP;
  if(g_rcpt.hmap_c[k]&&g_rcpt.hmap_h[k]==h)return (cmd_h *)g_rcpt.hmap_c[k];}
 return NULL;
}
static void nvn_rcpt_retire_locked(cmd_h *c);   /* 定义在下方（保持块内顺序） */
static unsigned nvn_rcpt_retire_all_locked(void);   /* M5p B：未命中兜底（定义在下方） */
static void nvn_rcpt_submit(int n,const uint64_t *hs){
 if(n<=0||!hs)return;
 pthread_mutex_lock(&g_rcpt_mx);
 g_rcpt.submits++;
 for(int i=0;i<n;i++){
  cmd_h *c=nvn_rcpt_lookup(hs[i]);
   if(!c){   /* M5p B：句柄域与 map 键不符（M5o 实测 unknown=613/613、writes=0） */
    /* 探针（只记不判，落 vk_boot.log 保证被采集）：前 3 行打未命中句柄 + 映射表状态，
       后 3 行打最近写入的 3 个键（含 key 是否等于 cb 指针），由此直接看出提交句柄属于哪个域。 */
    static unsigned mn;if(mn<6){mn++;
     if(mn<=3)vk_boot_logf("[fn] rcpt miss#%u handle=%#llx hmap_n=%u next=%llu pending=%u",
      mn,(unsigned long long)hs[i],g_rcpt.hmap_n,(unsigned long long)g_rcpt.hmap_next,g_rcpt.n);
     else{unsigned j=mn-3,k=(g_rcpt.hmap_next+NVN_RCPT_HMAP-1-j)%NVN_RCPT_HMAP;
      vk_boot_logf("[fn] rcpt map#%u key=%#llx cb=%p key_is_cb=%d empty=%d",
       mn,(unsigned long long)g_rcpt.hmap_h[k],g_rcpt.hmap_c[k],
       (int)(g_rcpt.hmap_c[k]&&g_rcpt.hmap_h[k]==(uint64_t)(uintptr_t)g_rcpt.hmap_c[k]),
       (int)(g_rcpt.hmap_c[k]==NULL));}}
    /* 兜底（语义 = donor 的「submit 完成后 post_run」）：本宿主无 GPU，提交即回放完成，
       句柄域对不上时按录制序把环内未退休回执全部写出，而不是永远丢掉（writes=0 的现状）。
       代价：同一时刻存在多个已录制未提交 CB 时可能提前退休（本作实测 n=1/提交、~1.1 CB/提交）。
       命中路径不受影响；兜底单独计 fb_writes，报告里能区分「真键命中」与「兜底」。 */
    unsigned w=nvn_rcpt_retire_all_locked();
    if(w)g_rcpt.fb_writes+=w;else g_rcpt.unknown_handle++;
    continue;}
  nvn_rcpt_retire_locked(c);                   /* 退休=回放完成：写真内存+回执写入门计数 */
 }
 /* 可观测（非修复）：每 1000 次 submit 打一行统计，最多 12 行 */
 if(g_rcpt.submits%1000==0){
  static unsigned pn;if(pn<12){pn++;
   fprintf(stderr,"[nvn-rcpt] submits=%llu unknown=%llu writes=%llu pending=%u rec(c=%llu v=%llu s=%llu) dropped=%llu\n",
    (unsigned long long)g_rcpt.submits,(unsigned long long)g_rcpt.unknown_handle,
    (unsigned long long)g_rcpt.writes,g_rcpt.n,
    (unsigned long long)g_rcpt.rec_cnt,(unsigned long long)g_rcpt.rec_val,(unsigned long long)g_rcpt.rec_sig,
    (unsigned long long)g_rcpt.dropped);}
 }
 pthread_mutex_unlock(&g_rcpt_mx);
}
/* M5p B：单条回执退休（原 nvn_rcpt_retire_locked 的循环体，抽出来给「按 CB」和「兜底全写」共用）。
   目标地址先做用户空间区间校验：兜底路径会写到「不确定属于哪个 CB」的地址，越界一律不写只计数。 */
static void nvn_rcpt_one_locked(rcpt_t *p){
  if(p->a<NVN_USER_LO||p->a>=NVN_USER_HI){g_rcpt.bad_dst++;return;}
  if(p->kind==1){
   uint64_t ts=nvn_gpu_ticks(),rec[2]={(p->v==0||p->v==0x10)?ts:0,ts};
   dm_guest_write_prepare((void *)(uintptr_t)p->a,16);memcpy((void *)(uintptr_t)p->a,rec,16);
   {static unsigned n;if(n++<4)fprintf(stderr,"[nvn] ReportCounter(retired) type=%#x -> %#llx {counter=%#llx, ts=%#llx}\n",p->v,(unsigned long long)p->a,(unsigned long long)rec[0],(unsigned long long)rec[1]);}
  }else if(p->kind==2){
   nvn_cb_event_write((volatile uint32_t *)(uintptr_t)p->a,0,p->v);
   fn_note_repval(p->a,p->v);   /* M5o: count the ReportValue destination word (same "completion token" shape) */
   {static unsigned n;if(n++<4)fprintf(stderr,"[nvn] ReportValue(retired) v=%u -> %#llx\n",p->v,(unsigned long long)p->a);}
  }else{
   ev_h *ev=(ev_h *)(uintptr_t)p->a;nvn_cb_event_write(ev?ev->val:0,p->mode,p->v);
   fn_note_ev_sig(p->a,(uint32_t)p->v,FN_CBSIG);   /* M5o: command-stream SignalEvent that really landed (the word the consumer reads) */
   {static unsigned n;if(n++<4)fprintf(stderr,"[nvn] SignalEvent(retired) ev=%p mode=%s v=%u\n",ev,p->mode==1?"ADD":"WRITE",p->v);}
  }
  g_rcpt.writes++;
}
static void nvn_rcpt_retire_locked(cmd_h *c){   /* 提交退休=回放完成：按录制序写真内存，时戳取此刻 */
 for(unsigned i=0;i<g_rcpt.n;i++){
  if(g_rcpt.r[i].cmd!=(void *)c)continue;
  nvn_rcpt_one_locked(&g_rcpt.r[i]);
 }
}
static unsigned nvn_rcpt_retire_all_locked(void){   /* M5p B：句柄域对不上时的兜底——环内未退休回执按录制序全写，然后清环 */
 unsigned w=0;
 for(unsigned i=0;i<g_rcpt.n;i++){nvn_rcpt_one_locked(&g_rcpt.r[i]);w++;}
 g_rcpt.n=0;
 return w;
}
static void nvn_command_buffer_signal_event(void *c,const void *e,int m,int loc,int flags,uint32_t v){
 (void)loc;(void)flags;g_nvn_cmd_total+=24;   /* donor :873-877 记 24B（OP_SIGNAL_EVENT token）。第 2 步：只累计全局量——cb 客体已归 cmdbuf（偏移0=宿主 NvnCmdBuf*），不能再按 cmd_h 覆盖体记账（原 cb_account 会写 guest+0x88） */
 nvn_rcpt_push((cmd_h *)c,3,(uint8_t)(m==1),v,(uint64_t)(uintptr_t)e);   /* c 仅作不透明键（retire 侧只做指针比较，不解引用） */
 fn_note_ev_rec((uint64_t)(uintptr_t)e);   /* M5o: note "signal was recorded"; FN_CBSIG.sig on the retire side says whether it landed */
 {static unsigned n;if(n++<4)fprintf(stderr,"[nvn] SignalEvent recorded ev=%p mode=%s v=%u (writes at submit)\n",e,m==1?"ADD":"WRITE",v);}
}
static void nvn_command_buffer_report_value(void *c,uint32_t v,uint64_t a){
 g_nvn_cmd_total+=24;   /* donor :2450-2454 记 24B（经 OP_SIGNAL_EVENT WRITE）；同上，不再写 cmd_h 覆盖体 */
 nvn_rcpt_push((cmd_h *)c,2,0,v,a);
 {static unsigned n;if(n++<4)fprintf(stderr,"[nvn] ReportValue recorded v=%u -> %#llx (writes at submit)\n",v,(unsigned long long)a);}
}
static void nvn_command_buffer_report_counter(void *c,int t,uint64_t a){
 g_nvn_cmd_total+=24;   /* donor :887-889 记 24B（OP_REPORT_COUNTER）；同上 */
 if(a)nvn_rcpt_push((cmd_h *)c,1,0,(uint32_t)t,a);
 {static unsigned n;if(n++<4)fprintf(stderr,"[nvn] ReportCounter recorded type=%#x -> %#llx (writes at submit)\n",t,(unsigned long long)a);}
}
static void nvn_command_buffer_reset_counter(void *c,int t){NM_SC_N("nvnCommandBufferResetCounter");
 cb_account((cmd_h *)c,16);   /* donor :884-885 记 16B，回放只累计统计不写内存 */
 {static unsigned n;if(n++<2)fprintf(stderr,"[nvn] ResetCounter type=%#x (stats only, donor :884-885)\n",t);}
}
/* 回执写入门计数（threads.c dump 侧）：0=writes 1=pending 2=rec_cnt 3=rec_val 4=rec_sig
   5=dropped 6=unknown_handle 7=submits */
uint64_t dm_nvn_receipt_stat(int idx){
 uint64_t r=0;pthread_mutex_lock(&g_rcpt_mx);
 switch(idx){case 0:r=g_rcpt.writes;break;case 1:r=g_rcpt.n;break;case 2:r=g_rcpt.rec_cnt;break;
  case 3:r=g_rcpt.rec_val;break;case 4:r=g_rcpt.rec_sig;break;case 5:r=g_rcpt.dropped;break;
  case 6:r=g_rcpt.unknown_handle;break;case 7:r=g_rcpt.submits;break;
  case 8:r=g_rcpt.fb_writes;break;case 9:r=g_rcpt.bad_dst;break;default:r=0;}
 pthread_mutex_unlock(&g_rcpt_mx);return r;
}
/* ============ M5o: [fn] ledger report (into vk_boot.log, 60 s cadence) ============
   Called from threads.c runtime_m5k_dump(), i.e. from the heartbeat thread: it must not touch
   guest memory (it does not) and must not block guest threads for long (the table is 128 rows,
   the selection is O(n*10)). Two time points >=60 s apart are therefore guaranteed by the
   existing dump schedule; each row prints the delta of the previous printed dump, so
   "waits grow / signals flat" is visible per interval, not just cumulatively. */
void dm_nvn_fn_dump(int dno) {
    static struct { uint64_t obj; uint32_t type; uint64_t cons, prod; } prev[12];
    static int prev_n;
    uint64_t t_inits = 0, t_finz = 0, t_waits = 0, t_ok = 0, t_to = 0, t_fsig = 0;
    uint64_t t_q = 0, t_sig = 0, t_vw = 0, t_recs = 0, t_live = 0;
    unsigned n = g_fn_n < FN_N ? g_fn_n : FN_N;
    for (unsigned i = 0; i < n; i++) {
        fn_rec *r = &g_fn[i];
        t_inits += r->inits; t_finz += r->finz; t_waits += r->waits; t_ok += r->wait_ok;
        t_to += r->wait_to; t_fsig += r->fence_sig; t_q += r->queries; t_sig += r->sig;
        t_vw += r->val_writes; t_recs += r->recs; if (r->live) t_live++;
    }
    uint64_t rc_sub = 0, rc_writes = 0, rc_cnt = 0, rc_val = 0, rc_sig = 0, rc_pend = 0, rc_unk = 0, rc_drop = 0;
    uint64_t rc_fb = 0, rc_bad = 0;   /* M5p B：兜底退休笔数 / 越界目标被拒笔数 */
    pthread_mutex_lock(&g_rcpt_mx);
    rc_sub = g_rcpt.submits; rc_writes = g_rcpt.writes; rc_cnt = g_rcpt.rec_cnt; rc_val = g_rcpt.rec_val;
    rc_sig = g_rcpt.rec_sig; rc_pend = g_rcpt.n; rc_unk = g_rcpt.unknown_handle; rc_drop = g_rcpt.dropped;
    rc_fb = g_rcpt.fb_writes; rc_bad = g_rcpt.bad_dst;
    pthread_mutex_unlock(&g_rcpt_mx);
    vk_boot_logf("[fn] dump#%d objs=%u live=%llu ovf=%u | sync: init=%llu finz=%llu wait=%llu ok=%llu timeout=%llu fencesig=%llu | event: queries=%llu sig=%llu valwrites=%llu recs=%llu | smo_fn_adv=%d forced=%llu",
                 dno, n, (unsigned long long)t_live, g_fn_ovf,
                 (unsigned long long)t_inits, (unsigned long long)t_finz, (unsigned long long)t_waits,
                 (unsigned long long)t_ok, (unsigned long long)t_to, (unsigned long long)t_fsig,
                 (unsigned long long)t_q, (unsigned long long)t_sig, (unsigned long long)t_vw,
                 (unsigned long long)t_recs, g_fn_adv_on, (unsigned long long)__atomic_load_n(&g_fn_adv_hits, __ATOMIC_RELAXED));
    vk_boot_logf("[fn] rcpt dump#%d submits=%llu writes=%llu rec(cnt=%llu val=%llu sig=%llu) pending=%llu unknown=%llu dropped=%llu fb=%llu bad_dst=%llu",
                 dno, (unsigned long long)rc_sub, (unsigned long long)rc_writes,
                 (unsigned long long)rc_cnt, (unsigned long long)rc_val, (unsigned long long)rc_sig,
                 (unsigned long long)rc_pend, (unsigned long long)rc_unk, (unsigned long long)rc_drop,
                 (unsigned long long)rc_fb, (unsigned long long)rc_bad);
    /* top-10 by consumer pressure (sync waits + event queries) */
    static const char *tyname[] = { "?", "sync", "evt", "cbsig", "repval" };
    unsigned top[10]; int ntop = 0;
    for (int k = 0; k < 10; k++) {
        int best = -1; uint64_t bw = 0;
        for (unsigned i = 0; i < n; i++) {
            uint64_t w = (g_fn[i].type == FN_SYNC) ? g_fn[i].waits : g_fn[i].queries;
            if (!w) continue;
            int used = 0; for (int j = 0; j < ntop; j++) if (top[j] == i) { used = 1; break; }
            if (used) continue;
            if (w > bw) { bw = w; best = (int)i; }
        }
        if (best < 0) break;
        top[ntop++] = (unsigned)best;
    }
    for (int i = 0; i < ntop; i++) {
        fn_rec *r = &g_fn[top[i]];
        const char *tn = r->type < (FN_REPVAL + 1) ? tyname[r->type] : "?";
        const char *tl = tn; /* keep -Wunused happy in builds without module info */
        char lrs[64] = {0};
        if (r->lr) {
            const char *nm = NULL; uint64_t off = 0;
            if (runtime_module_offset(r->lr, &nm, &off) && nm) snprintf(lrs, sizeof lrs, " %s+%#llx", nm, (unsigned long long)off);
            else snprintf(lrs, sizeof lrs, " lr=%#llx", (unsigned long long)r->lr);
        }
        if (r->type == FN_SYNC) {
            uint64_t pw = 0, ps = 0; int had = 0;
            for (int j = 0; j < prev_n; j++) if (prev[j].obj == r->obj && prev[j].type == r->type) { pw = prev[j].cons; ps = prev[j].prod; had = 1; break; }
            vk_boot_logf("[fn] %s obj=%#llx waits=%llu ok=%llu to=%llu fencesig=%llu lastret=%u tmo=%lld fence=%llu live=%u (+%llu waits/+%llu sig since dump#%d)%s%s",
                         tl, (unsigned long long)r->obj, (unsigned long long)r->waits, (unsigned long long)r->wait_ok,
                         (unsigned long long)r->wait_to, (unsigned long long)r->fence_sig, r->last_ret,
                         (long long)r->last_tmo, (unsigned long long)r->last_fence, r->live,
                         had ? (unsigned long long)(r->waits - pw) : (unsigned long long)r->waits,
                         had ? (unsigned long long)(r->fence_sig - ps) : (unsigned long long)r->fence_sig,
                         had ? dno - 1 : 0,
                         (r->waits && r->fence_sig == 0) ? "  <== waits but NEVER fenced" : "", lrs);
        } else {
            uint64_t pw = 0, ps = 0; int had = 0;
            for (int j = 0; j < prev_n; j++) if (prev[j].obj == r->obj && prev[j].type == r->type) { pw = prev[j].cons; ps = prev[j].prod; had = 1; break; }
            vk_boot_logf("[fn] %s obj=%#llx queries=%llu sig=%llu recs=%llu valwrites=%llu val=%u->%u chg=%llu live=%u (+%llu q/+%llu sig since dump#%d)%s%s",
                         tl, (unsigned long long)r->obj, (unsigned long long)r->queries, (unsigned long long)r->sig,
                         (unsigned long long)r->recs, (unsigned long long)r->val_writes,
                         r->val_first, r->val_last, (unsigned long long)r->val_chg, r->live,
                         had ? (unsigned long long)(r->queries - pw) : (unsigned long long)r->queries,
                         had ? (unsigned long long)(r->sig - ps) : (unsigned long long)r->sig,
                         had ? dno - 1 : 0,
                         (r->queries && r->sig == 0) ? "  <== polled but NEVER signaled" : "", lrs);
        }
    }
    /* explicit verdict rows: any object whose consumer side moves and producer side does not */
    unsigned susp = 0;
    for (unsigned i = 0; i < n; i++) {
        fn_rec *r = &g_fn[i];
        int is_susp = (r->type == FN_SYNC) ? (r->waits && !r->fence_sig) : (r->queries && !r->sig);
        if (!is_susp) continue;
        if (susp < 6)
            vk_boot_logf("[fn] SUSPECT%s obj=%#llx type=%s waits=%llu queries=%llu sig=%llu fencesig=%llu val=0x%x->0x%x live=%u",
                         susp == 0 ? "" : "*", (unsigned long long)r->obj, tyname[r->type < (FN_REPVAL + 1) ? r->type : 0],
                         (unsigned long long)r->waits, (unsigned long long)r->queries, (unsigned long long)r->sig,
                         (unsigned long long)r->fence_sig, r->val_first, r->val_last, r->live);
        susp++;
    }
    vk_boot_logf("[fn] suspect_objs=%u of %u (criterion: consumer moves, producer flat)", susp, n);
    prev_n = ntop > 12 ? 12 : ntop;
    for (int i = 0; i < prev_n; i++) {
        prev[i].obj = g_fn[top[i]].obj; prev[i].type = g_fn[top[i]].type;
        prev[i].cons = (g_fn[top[i]].type == FN_SYNC) ? g_fn[top[i]].waits : g_fn[top[i]].queries;
        prev[i].prod = (g_fn[top[i]].type == FN_SYNC) ? g_fn[top[i]].fence_sig : g_fn[top[i]].sig;
    }
}
/* —— 记账录制族：平价件（donor 无 hw_fix，按其记录尺寸 16-40B 的平价 24B 近似；首用一行日志）—— */
#define CB_REC(NAME) static void NAME(void *c){NM_SC(NAME);cb_account((cmd_h *)c,24);{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] rec %s +24B (flat approx)\n",#NAME);}}   /* M4am：NM_SC 按名计调用次数（判定「单数拼写」是否真的在用） */
CB_REC(nvn_command_buffer_begin_transform_feedback)
CB_REC(nvn_command_buffer_end_transform_feedback)
CB_REC(nvn_command_buffer_pause_transform_feedback)
CB_REC(nvn_command_buffer_resume_transform_feedback)
CB_REC(nvn_command_buffer_bind_blend_state)
CB_REC(nvn_command_buffer_bind_channel_mask_state)
CB_REC(nvn_command_buffer_bind_color_state)
CB_REC(nvn_command_buffer_bind_depth_stencil_state)
CB_REC(nvn_command_buffer_bind_multisample_state)
CB_REC(nvn_command_buffer_bind_polygon_state)
CB_REC(nvn_command_buffer_bind_program)
CB_REC(nvn_command_buffer_bind_coverage_modulation_table)
CB_REC(nvn_command_buffer_bind_transform_feedback_buffer)
CB_REC(nvn_command_buffer_bind_transform_feedback_buffers)
/* ---- M4an（2026-11-04）：CallCommands / CopyCommands 从记账桩升级为真实录制 ----
   此前这两条是 CB_REC（只 +24B 记账），而它们是整 boot 调用量第一的族（Call=47,648 / @#1500），
   被 call 的二级命令缓冲区因此**从未进过任何命令流**（opcode 直方图里连 CALL 都没有）。
   现在走 cmdbuf.c 的 nvnCommandBufferCallCommands/CopyCommands：把每个 handle 的已录记录体
   按原始字节 inline 进当前 cb（遇 END 停、跟 JUMP、先验 nvn_rec_cmdmem_readable 边界）。
   台账：每次 call 一行（限流），末尾一条 totals；CALL 计数由 inline 时插入的 CMD_CALL 记录带进
   [h] opcode 直方图。 */
extern void nvn_rec_call_commands(void *cb,int n,const void *h);
extern void nvn_rec_copy_commands(void *cb,int n,const void *h);
extern void nvn_port_callcmd_totals(uint64_t *o);
extern void nvn_port_callcmd_last(uint64_t *o);
static unsigned g_cc_logged;
static void nvn_cmd_call_common(void *c,int n,const void *h,int is_copy){
 if(is_copy){g_cc_copy_n++;nvn_rec_copy_commands(c,n,h);}else{g_cc_call_n++;nvn_rec_call_commands(c,n,h);}
 {uint64_t L[4]={0,0,0,0};nvn_port_callcmd_last(L);
  unsigned long long k=g_cc_call_n+g_cc_copy_n;
  if(g_cc_logged<8||k%2000==0){if(g_cc_logged<8)g_cc_logged++;
   vk_boot_logf("[h] callcmd n=%d bytes=%llu recs=%llu inline=%llu stop_end=%llu bad=%llu copy=%d",
    n,(unsigned long long)L[0],(unsigned long long)L[1],(unsigned long long)L[0],
    (unsigned long long)L[3],(unsigned long long)L[2],is_copy);}}
}
static void nvn_command_buffer_call_commands(void *c,int n,const void *h){nvn_cmd_call_common(c,n,h,0);}
static void nvn_command_buffer_copy_commands(void *c,int n,const void *h){nvn_cmd_call_common(c,n,h,1);}
CB_REC(nvn_command_buffer_clear_color)
CB_REC(nvn_command_buffer_clear_colori)
CB_REC(nvn_command_buffer_clear_colorui)
CB_REC(nvn_command_buffer_clear_depth_stencil)
/* M4ai P2：写纹理/缓冲的这几条不再用「忽略全部参数」的 CB_REC——按真实 NVN 原型声明，
   既保留原来的 24 B 平坦记账（行为不变），又多记两件事：① 这类命令被游戏调用了多少次（按名）；
   ② copy/clear-texture 的目标纹理是谁（id/尺寸/存储 nz）。**仍然不执行**（本轮只定位不修）。 */
static void nvn_command_buffer_clear_texture(void *c,const void *tex,const void *view,const void *region,const float *color,int mask)
{(void)mask;g_gpuop_n[GPUOP_CLEAR_TEX]++;nvn_gpuop_note_dst(GPUOP_CLEAR_TEX,tex,0);gpuop_clear_exec(tex,view,region,color,NULL,0);cb_account((cmd_h *)c,24);}
static void nvn_command_buffer_clear_texturei(void *c,const void *tex,const void *view,const void *region,const int *color,int mask)
{(void)mask;g_gpuop_n[GPUOP_CLEAR_TEXI]++;nvn_gpuop_note_dst(GPUOP_CLEAR_TEXI,tex,0);gpuop_clear_exec(tex,view,region,NULL,(const uint32_t *)color,1);cb_account((cmd_h *)c,24);}
static void nvn_command_buffer_clear_textureui(void *c,const void *tex,const void *view,const void *region,const uint32_t *color,int mask)
{(void)mask;g_gpuop_n[GPUOP_CLEAR_TEXUI]++;nvn_gpuop_note_dst(GPUOP_CLEAR_TEXUI,tex,0);gpuop_clear_exec(tex,view,region,NULL,color,1);cb_account((cmd_h *)c,24);}
static void nvn_command_buffer_copy_buffer_to_texture(void *c,uint64_t src,const void *tex,const void *view,const void *region,int flags)
{NM_SC_N("nvnCommandBufferCopyBufferToTexture");(void)view;(void)region;(void)flags;g_gpuop_n[GPUOP_COPY_B2T]++;nvn_gpuop_note_dst(GPUOP_COPY_B2T,tex,src);cb_account((cmd_h *)c,24);}
static void nvn_command_buffer_copy_texture_to_texture(void *c,const void *stex,const void *sview,const void *sregion,const void *dtex,const void *dview,const void *dregion,int flags)
{(void)flags;g_gpuop_n[GPUOP_COPY_T2T]++;nvn_gpuop_note_dst(GPUOP_COPY_T2T,dtex,0);gpuop_t2t_exec(stex,sview,sregion,dtex,dview,dregion);cb_account((cmd_h *)c,24);}
static void nvn_command_buffer_copy_texture_to_buffer(void *c,const void *tex,const void *view,const void *region,uint64_t dst,int flags)
{NM_SC_N("nvnCommandBufferCopyTextureToBuffer");(void)view;(void)region;(void)dst;(void)flags;g_gpuop_n[GPUOP_COPY_T2B]++;cb_account((cmd_h *)c,24);}
static void nvn_command_buffer_copy_buffer_to_buffer(void *c,uint64_t s,uint64_t d,uint64_t n){NM_SC_N("nvnCommandBufferCopyBufferToBuffer");(void)s;(void)d;(void)n;g_gpuop_n[GPUOP_COPY_B2B]++;cb_account((cmd_h *)c,24);}
static void nvn_command_buffer_clear_buffer(void *c,uint64_t buf,uint64_t n,uint32_t v){NM_SC_N("nvnCommandBufferClearBuffer");(void)buf;(void)n;(void)v;g_gpuop_n[GPUOP_CLEAR_BUF]++;cb_account((cmd_h *)c,24);}
static void nvn_command_buffer_dispatch_compute(void *c,int gx,int gy,int gz){NM_SC_N("nvnCommandBufferDispatchCompute");(void)gx;(void)gy;(void)gz;g_gpuop_n[GPUOP_DISPATCH]++;cb_account((cmd_h *)c,24);}
static void nvn_command_buffer_dispatch_compute_indirect(void *c,uint64_t a){NM_SC_N("nvnCommandBufferDispatchComputeIndirect");(void)a;g_gpuop_n[GPUOP_DISPATCH_IND]++;cb_account((cmd_h *)c,24);}
static void nvn_command_buffer_downsample(void *c,uint64_t a,uint64_t b){NM_SC_N("nvnCommandBufferDownsample");g_gpuop_n[GPUOP_DOWNSAMPLE]++;cb_account((cmd_h *)c,24);(void)a;(void)b;}
/* ---- M4an P2（2026-11-04）：四个单数拼写状态设置：记账桩 → 真实录制 ----
   boot 实测 SetViewport 30,558 / SetScissor 30,563 / SetDepthRange 30,510 / SetBlendColor 36,622（@#1500），
   此前全是 CB_REC 记账桩 ⇒ 视口/裁剪/深度范围/混合色从未进过命令流，nvn_vk 侧只能吃硬编码的
   1600×900 全幅（nvn_vk.c:2383-2395）。录制端 cmdbuf.c:454-476 早已实现（与复数名同 opcode），
   本轮补本层搬运 + 表项。**回放侧尚未消费**：nvn_cmd_sink 还没有 SET_VIEWPORTS/SET_SCISSORS 回调，
   画机器仍用 RT 尺寸整幅 viewport——本轮只做到「进流 + 可数」，见报告 P2 说明。 */
static void nvn_command_buffer_set_viewport(void *c,int x,int y,int w,int h){g_ss_viewport++;nvn_rec_set_viewport(c,x,y,w,h);}
static void nvn_command_buffer_set_scissor(void *c,int x,int y,int w,int h){g_ss_scissor++;nvn_rec_set_scissor(c,x,y,w,h);}
static void nvn_command_buffer_set_depth_range(void *c,float n,float f){g_ss_depthrange++;nvn_rec_set_depth_range(c,n,f);}
static void nvn_command_buffer_set_blend_color(void *c,const float *rgba){g_ss_blendcolor++;nvn_rec_set_blend_color(c,rgba);}
/* 注意：call_commands / copy_commands 是「命令流嵌套/拷贝」，不是缓冲写；见上方 real 实现，
   不混进 GPUOP_COPY_B2B（否则台账名不副实）。 */
CB_REC(nvn_command_buffer_discard_color)
CB_REC(nvn_command_buffer_discard_depth_stencil)
CB_REC(nvn_command_buffer_downsample_texture_view)
CB_REC(nvn_command_buffer_tiled_downsample)
CB_REC(nvn_command_buffer_tiled_downsample_texture_view)
CB_REC(nvn_command_buffer_draw_transform_feedback)
CB_REC(nvn_command_buffer_draw_texture)
CB_REC(nvn_command_buffer_multi_draw_arrays_indirect_count)
CB_REC(nvn_command_buffer_multi_draw_elements_indirect_count)
CB_REC(nvn_command_buffer_insert_checkpoint_dynamic)
CB_REC(nvn_command_buffer_insert_checkpoint_static)
CB_REC(nvn_command_buffer_insert_debug_marker)
CB_REC(nvn_command_buffer_insert_debug_marker_dynamic)
CB_REC(nvn_command_buffer_insert_debug_marker_static)
CB_REC(nvn_command_buffer_push_debug_group)
CB_REC(nvn_command_buffer_push_debug_group_dynamic)
CB_REC(nvn_command_buffer_push_debug_group_static)
CB_REC(nvn_command_buffer_pop_debug_group)
CB_REC(nvn_command_buffer_pop_debug_group_id)
CB_REC(nvn_command_buffer_resolve_depth_buffer)   /* report_counter/report_value/reset_counter/signal_event 四件已升真实现（上方 round14b 块），不再走 CB_REC 记账 */
CB_REC(nvn_command_buffer_restore_zcull_data)
CB_REC(nvn_command_buffer_save_zcull_data)
CB_REC(nvn_command_buffer_set_alpha_ref)
CB_REC(nvn_command_buffer_set_color_reduction_enable)
CB_REC(nvn_command_buffer_set_color_reduction_thresholds)
CB_REC(nvn_command_buffer_set_conservative_raster_dilate)
CB_REC(nvn_command_buffer_set_conservative_raster_enable)
CB_REC(nvn_command_buffer_set_depth_bounds)
CB_REC(nvn_command_buffer_set_depth_clamp)
CB_REC(nvn_command_buffer_set_depth_ranges)
CB_REC(nvn_command_buffer_set_inner_tessellation_levels)
CB_REC(nvn_command_buffer_set_line_width)
CB_REC(nvn_command_buffer_set_outer_tessellation_levels)
CB_REC(nvn_command_buffer_set_patch_size)
CB_REC(nvn_command_buffer_set_point_size)
CB_REC(nvn_command_buffer_set_polygon_offset_clamp)
CB_REC(nvn_command_buffer_set_primitive_restart)
CB_REC(nvn_command_buffer_set_program_subroutines)
CB_REC(nvn_command_buffer_set_rasterizer_discard)
CB_REC(nvn_command_buffer_set_render_enable)
CB_REC(nvn_command_buffer_set_render_enable_conditional)
CB_REC(nvn_command_buffer_set_render_targets)
CB_REC(nvn_command_buffer_set_sample_mask)
CB_REC(nvn_command_buffer_set_sampler_pool)
CB_REC(nvn_command_buffer_set_scissors)
CB_REC(nvn_command_buffer_set_shader_scratch_memory)
CB_REC(nvn_command_buffer_set_stencil_cull_criteria)
CB_REC(nvn_command_buffer_set_stencil_mask)
CB_REC(nvn_command_buffer_set_stencil_ref)
CB_REC(nvn_command_buffer_set_stencil_value_mask)
CB_REC(nvn_command_buffer_set_subpixel_precision_bias)
CB_REC(nvn_command_buffer_set_texture_pool)
CB_REC(nvn_command_buffer_set_tiled_cache_action)
CB_REC(nvn_command_buffer_set_tiled_cache_tile_size)
CB_REC(nvn_command_buffer_set_viewport_swizzles)
CB_REC(nvn_command_buffer_wait_event)
/* —— P0-3 Compare 三件（donor :456-459：对象描述等价，非像素比较）—— */
static uint64_t nvn_texture_compare(const void *a,const void *b){const tex_h *x=a,*y=b;{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] TextureCompare %p vs %p -> %d\n",a,b,!(memcmp(&x->d,&y->d,sizeof x->d)||x->mem!=y->mem));}return !(memcmp(&x->d,&y->d,sizeof x->d)||x->mem!=y->mem);}   /* 描述符逐字节 + 池内存指针相等；此前空桩恒 0=登记去重全走"不等" */
static uint64_t nvn_texture_view_compare(const void *a,const void *b){return !memcmp(a,b,16);}   /* view_t 16B 值对象（nvn_obj.h:41-45 断言），整块memcmp */
static uint64_t nvn_sampler_compare(const void *a,const void *b){const samp_h *x=a,*y=b;return x->magic==y->magic&&!memcmp(&x->p,&y->p,sizeof x->p);}
/* —— 终局 F-G（crash_1031 终局七段）：TextureView 14 件成对——donor 只实装 6 件（hle_nvn.c:464-469），
   其余按 nvn_protos.h:167-180 原型 + view_h 布局（nvn_obj.h:41-45，16B；定义在此供下方 Set/Get 与
   P1-5 texels_move 共用）同型补齐。
   此前 Set 分槽不存 + Get 恒 0 ⇒ 装载器建 view 回读校验必败 ⇒ 装库完成计数差口（0x1003 永不 signal）。
   bits 位序同 donor view_t：bit0=levels bit1=layers bit2=format bit3=target bit4=swizzle bit5=stencil。
   红线：Set/Get 必须成对实装（只装 Set 回读照样失败）。 —— */
typedef struct{uint8_t bits,base_level,nlevels,target;uint16_t min_layer,nlayers,format;uint8_t swz[4],pad[2];} view_h;   /* =donor view_t（nvn_obj.h:41-45，16B 断言）：bits bit0=set_levels bit1=set_layers bit2=set_format */
static void nvn_texture_view_set_defaults(void *v){memset(v,0,sizeof(view_h));}   /* donor :464 */
static void nvn_texture_view_set_levels(void *v,int base,int n){view_h *o=v;if((unsigned)base>255||(unsigned)n>255){fprintf(stderr,"[nvn] view levels %d/%d\n",base,n);return;}o->bits|=1u;o->base_level=(uint8_t)base;o->nlevels=(uint8_t)n;}   /* donor :465 die→日志 */
static void nvn_texture_view_set_layers(void *v,int min,int n){view_h *o=v;if((unsigned)min>65535||(unsigned)n>65535){fprintf(stderr,"[nvn] view layers %d/%d\n",min,n);return;}o->bits|=2u;o->min_layer=(uint16_t)min;o->nlayers=(uint16_t)n;}   /* donor :466 */
static void nvn_texture_view_set_format(void *v,int f){view_h *o=v;if((unsigned)f>65535){fprintf(stderr,"[nvn] view format %d\n",f);return;}o->bits|=4u;o->format=(uint16_t)f;}   /* donor :467 */
static void nvn_texture_view_set_swizzle(void *v,int a,int b,int c,int d){view_h *o=v;o->bits|=16u;o->swz[0]=(uint8_t)a;o->swz[1]=(uint8_t)b;o->swz[2]=(uint8_t)c;o->swz[3]=(uint8_t)d;}   /* 原型 :171 同型存字段 */
static void nvn_texture_view_set_depth_stencil_mode(void *v,int m){view_h *o=v;o->bits|=32u;o->pad[0]=(uint8_t)m;}   /* 原型 :172（donor set_stencil 位），复用 pad[0] 存 dsm */
static void nvn_texture_view_set_target(void *v,int t){view_h *o=v;if((unsigned)t>255){fprintf(stderr,"[nvn] view target %d\n",t);return;}o->bits|=8u;o->target=(uint8_t)t;}   /* donor :468 */
static uint64_t nvn_texture_view_get_levels(const void *v,int *base,int *n){const view_h *o=v;*base=o->base_level;*n=o->nlevels;return (o->bits>>0)&1;}   /* donor GetLayers :469 同型 */
static uint64_t nvn_texture_view_get_layers(const void *v,int *min,int *n){const view_h *o=v;*min=o->min_layer;*n=o->nlayers;return (o->bits>>1)&1;}   /* donor :469 */
static uint64_t nvn_texture_view_get_format(const void *v,int *f){const view_h *o=v;*f=o->format;return (o->bits>>2)&1;}
static uint64_t nvn_texture_view_get_swizzle(const void *v,int *a,int *b,int *c,int *d){const view_h *o=v;*a=o->swz[0];*b=o->swz[1];*c=o->swz[2];*d=o->swz[3];return (o->bits>>4)&1;}
static uint64_t nvn_texture_view_get_depth_stencil_mode(const void *v,int *m){const view_h *o=v;*m=o->pad[0];return (o->bits>>5)&1;}
static uint64_t nvn_texture_view_get_target(const void *v,int *t){const view_h *o=v;*t=o->target;return (o->bits>>3)&1;}
/* —— P0-4 设备 Origin/Depth 回显（donor :210-221；无设备对象，静态字段；默认 LOWER_LEFT=0 / NEAR_IS_MINUS_W=0）—— */
static int g_nvn_origin_mode,g_nvn_depth_mode;
static void nvn_device_set_window_origin_mode(void *d,int m){(void)d;g_nvn_origin_mode=m;fprintf(stderr,"[nvn] window origin %s\n",m?"UPPER_LEFT":"LOWER_LEFT");}
static uint64_t nvn_device_get_window_origin_mode(const void *d){(void)d;return (uint64_t)g_nvn_origin_mode;}
static void nvn_device_set_depth_mode(void *d,int m){(void)d;g_nvn_depth_mode=m;fprintf(stderr,"[nvn] depth mode %s\n",m?"NEAR_IS_ZERO":"NEAR_IS_MINUS_W");}
static uint64_t nvn_device_get_depth_mode(const void *d){(void)d;return (uint64_t)g_nvn_depth_mode;}
/* —— P1-6 时间戳 —— round13b 定名门（cell 0x136deb8=GetLoadProgress 双间接第二表）后对齐 donor：
   原『donor 无实现；→ 宿主单调钟』注释过时——donor hle_nvn.c:2288 有实现：
   nvnDeviceGetTimestampInNanoseconds = nvn_timestamp_to_ns(c->timestamp)，
   NVNcounterData={u64 counter; u64 timestamp;}（nvn.h:91-94，timestamp 在 +8），
   换算 ×625/384（donor nvn_timestamp.h:7，614.4MHz tick）。原先忽略 cd 返回 runtime_now_ns()：
   游戏装载进度监视 12-34k/s 洪流路径（0x9b9bc4 双间接调用，13b 定名+计数门实测）里的
   f(A)-f(B) 退化成两次取钟之差——每轮抖动、与池/计数数据完全无关。GetCurrentTimestamp
   维持宿主钟（donor 也返『当前时刻』，仅时基不同）。 */
static uint64_t nvn_device_get_timestamp_in_nanoseconds(const void *d,const void *cd){
  (void)d;
  if(!cd){static unsigned n;if(n++<1)fprintf(stderr,"[nvn] GetTimestampInNanoseconds(NULL counterData)\n");return 0;}
  uint64_t ts=((const volatile uint64_t*)cd)[1];   /* +8 = NVNcounterData.timestamp（donor 同款直读） */
  return (uint64_t)((__uint128_t)ts*625/384);
}
static uint64_t nvn_device_get_current_timestamp_in_nanoseconds(const void *d){(void)d;return runtime_now_ns();}
/* —— P1-5 Texels 真搬运（donor :478-510 的 stride 数学 + 池内存直拷；donor 走 mtl_*，本宿主池内存即真值）—— */
typedef struct{int xoffset,yoffset,zoffset,width,height,depth;} copy_region_h;   /* =nvn.h NVNcopyRegion */
/* M4v P2：**块线性存储上的 Texels 读写必须走 GOB 寻址**。
   旧实现无论存储类型都按「线性行式」往池里写（`lstride = wb`，注释自承「块线性按 wb 行式近似」）。
   这在语义上就是错的：游戏一旦用 nvnTextureWriteTexels 往块线性纹理里写像素，池里那段时间就不再是
   块线性排布，我们上送时又 deswizzle 一遍 ⇒ 双错，采样结果必然是噪点/错位（派活单 P1 要区分的正是
   这一类）。这里按 nvn_bl_offset 的 GOB 寻址逐 16 B 段搬；区域起点/长度不是 16 B 对齐时退化为逐字节
   （正确但慢——本作 boot 内 WriteTexels 是个位数次，可接受）。返回 1 = 已搬，0 = 几何不支持（调用方回落旧路径）。 */
static int texels_bl_move(const nvn_texdesc *d,unsigned level,unsigned layer,uint8_t *mem,
                          uint32_t xbytes,uint32_t wb,uint32_t hb,int depth,int yoff,
                          uint8_t *data,ptrdiff_t row,ptrdiff_t img,int write){
 uint32_t rb,rows,slices,b2;uint64_t base;unsigned th,td;
 if(!mem)return 0;
 if(nvn_deswizzle_geom(d,level,layer,&rb,&rows,&slices,&base,&th,&td,&b2))return 0;
 uint8_t *bl=mem+base;
 int seg=((xbytes%16)==0)&&((wb%16)==0);
 int z3d=(d->target==2)&&depth>1;
 if(depth>1&&!z3d)return 0;   /* 2D_ARRAY 的层由 layer 走 base，不在这里叠加 */
 for(int z=0;z<depth;z++)for(uint32_t y=0;y<hb;y++){
  uint8_t *s=data+(uint64_t)z*(uint64_t)img+(uint64_t)y*(uint64_t)row;
  uint32_t yy=(uint32_t)yoff+y;
  if(yy>=rows)continue;
  uint32_t zz=z3d?(uint32_t)z:0;
  if(seg){
   for(uint32_t x=0;x<wb;x+=16){
    uint32_t xx=xbytes+x;   /* 行内字节偏移 */
    uint8_t *p=bl+nvn_bl_offset(xx,yy,zz,rb,rows,th,td);
    if(write)memcpy(p,s+x,16);else memcpy(s+x,p,16);
   }
  } else {
   for(uint32_t i=0;i<wb;i++){
    uint8_t *p=bl+nvn_bl_offset(xbytes+i,yy,zz,rb,rows,th,td);
    if(write)*p=s[i];else s[i]=*p;
   }
  }
 }
 return 1;
}
static void texels_move(tex_h *o,const view_h *v,const copy_region_h *r,uint8_t *data,ptrdiff_t row,ptrdiff_t img,int write){
 int bw,bh,bpb=nvn_fmt_bpb(o->d.format,&bw,&bh);
 if(!bpb||!r->width||!r->height){fprintf(stderr,"[nvn][RED] Texels unknown format %#x / empty region\n",o->d.format);return;}
 uint32_t wb=(uint32_t)((r->width+bw-1)/bw)*bpb,hb=(uint32_t)((r->height+bh-1)/bh);
 if(!row)row=wb; if(!img)img=row*(ptrdiff_t)hb;   /* donor :483-484 默认紧排 */
 if(row<(ptrdiff_t)wb||img<row*(ptrdiff_t)hb){fprintf(stderr,"[nvn][RED] Texels stride smaller than region (row=%lld img=%lld wb=%u hb=%u)\n",(long long)row,(long long)img,wb,hb);return;}   /* donor :485 die→红日志 */
 unsigned level=v&&(v->bits&1)?v->base_level:0,layer=v&&(v->bits&2)?(unsigned)v->min_layer:0;   /* donor :486 视图级/层 */
 nvn_texlayout L;nvn_tex_layout(&o->d,&L);
 /* M4v P2：块线性 + 无 z 偏移 ⇒ 走 GOB 寻址（正确排布）。其余保持原线性搬运（zoffset 的层语义另算，别混）。 */
 if(!nvn_tex_is_linear(&o->d)&&o->d.target!=10&&!r->zoffset){
  uint32_t xbytes=(uint32_t)((r->xoffset+bw-1)/bw)*bpb;
  int depth=r->depth>0?r->depth:1;
  if(texels_bl_move(&o->d,level,layer,(uint8_t *)(uintptr_t)o->mem,xbytes,wb,hb,depth,r->yoffset,
                    data,row,img,write)){
   {static unsigned n;if(n++<4)fprintf(stderr,"[nvn] %s tex=%p fmt=%#x region=%dx%d lvl=%u layer=%u -> %lluB moved (blocklinear GOB)\n",write?"WriteTexels":"ReadTexels",(void *)o,o->d.format,r->width,r->height,level,layer,(unsigned long long)wb*(uint64_t)hb*(uint64_t)depth);}
   return;}}
 uint64_t base=nvn_tex_level_offset(&o->d,level)+(uint64_t)layer*L.layer_size+(uint64_t)(r->zoffset?r->zoffset:0)*L.layer_size;
 uint64_t xbytes=(uint64_t)((r->xoffset+bw-1)/bw)*bpb;
 ptrdiff_t lstride=nvn_tex_is_linear(&o->d)?(ptrdiff_t)nvn_tex_linear_stride(&o->d):(ptrdiff_t)wb;   /* pitch 按真实行距；块线性按 wb 行式近似（自洽读写，无 GPU 消费方） */
 uint64_t need=base+xbytes+wb+(uint64_t)(hb?(hb-1):0)*(uint64_t)lstride+(uint64_t)(r->depth>1?r->depth-1:0)*L.layer_size;
 if(need>L.size){fprintf(stderr,"[nvn][RED] Texels out of texture storage (need=%#llx size=%#llx lvl=%u layer=%u)\n",(unsigned long long)need,(unsigned long long)L.size,level,layer);return;}   /* 越纹理存储界的护栏（红日志不搬运） */
 for(int z=0;z<(r->depth>0?r->depth:1);z++)for(uint32_t y=0;y<hb;y++){
  uint8_t *p=(uint8_t *)(uintptr_t)o->mem+base+(uint64_t)z*L.layer_size+(uint64_t)y*(uint64_t)lstride+xbytes;
  uint8_t *s=data+(uint64_t)z*(uint64_t)img+(uint64_t)y*(uint64_t)row;
  memcpy(write?p:s,write?s:p,wb);
 }
 {static unsigned n;if(n++<4)fprintf(stderr,"[nvn] %s tex=%p fmt=%#x region=%dx%d lvl=%u layer=%u row=%lld -> %lluB moved\n",write?"WriteTexels":"ReadTexels",(void *)o,o->d.format,r->width,r->height,level,layer,(long long)row,(unsigned long long)wb*(uint64_t)hb*(uint64_t)(r->depth>0?r->depth:1));}
}
/* ================= M4aj P3：被吃掉的写命令真执行 =================
   M4ai 照亮：clear_texture(60) / copy_texture_to_texture(5) 一直是「只记账不执行」的桩
   （只推 24 B 指针，从不进命令流）。这里给它们真实现：**直接改池内存**——本宿主的纹理数据
   就是游戏池里的 CPU 内存（上送路径按需读），所以「执行」= 按 GOB 寻址写字节，与 texels_move 同一套
   排布数学（块线性 GOB / 线性 stride 两条都覆盖，含 level/layer 视图语义）。
   护栏（宁可 dropped 也不猜）：
     · 目标必须可解析（texrec 快照 / 我方写过的 NVNtexture 对象 magic 校验）；
     · clear 只支持 RGBA8 族（fmt 0x25..0x28，4 B/px）；其它格式计 skip_fmt；
     · 区域必须非空且落在该级存储内（texels_move 自带 need>L.size 红日志护栏）；
     · copy 要求源/目标同 fmt、同区域尺寸、总量 ≤4 MB。
   台账：`[gpu] exec …`（前 8 条）+ `[h] gpu write ops … executed/dropped`。 */
static const texsnap *gpuop_resolve(const void *tex){
 if(!tex)return NULL;
 const texsnap *s=texrec_get(tex,0);
 if(s&&s->mem)return s;
 {uint32_t mg=0;
#ifdef __MUSL__
  if(!rt_peek_u32((uintptr_t)tex,&mg)||mg!=0x54455800u)return NULL;
#else
  mg=((const tex_h *)tex)->magic;if(mg!=0x54455800u)return NULL;
#endif
 }
 return texreg_snap_from_obj(tex);}
/* 写命令的目标区间是否落在「被拒族」（why=-30 的那些池存储）上 —— 直接回答「这些写命令到底碰没碰它们」。
   M34A 修盲区：旧实现只查硬编码 {1900,1910,1911}，族 id 一个都不在集合里 ⇒ 对族结构性恒 0（M33A P2-4）。
   现在按**本次 opcode 的真实目标**查：① 目标存储区间 ∩ 动态族表（地址，主判据）；② 目标 id ∈ 族 id 集。
   旧三 id 口径保留在 hit_old3（可比性），关态时行为与旧实现逐字等价。 */
static void gpuop_hit_probe(const texsnap *sn,uint64_t extra_off,int dst_id){
 static const int t[3]={1900,1910,1911};
 if(!sn||!sn->mem||!sn->avail)return;
 uint64_t lo=(uint64_t)(uintptr_t)sn->mem+extra_off,hi=lo+sn->avail;
 for(int k=0;k<3;k++){const texsnap *e=texreg_lookup(t[k]);if(!e||!e->mem||!e->avail)continue;
  uint64_t elo=(uint64_t)(uintptr_t)e->mem,ehi=elo+e->avail;
  if(lo<ehi&&elo<hi){g_gpuop_hit_old3++;
   {static unsigned said;if(said++<8)vk_boot_logf("[gpu] hit target id=%d by write-op dst=%p dst_off=%#llx ext=%llu (与 id=%d 存储区间相交)",
     t[k],(void*)sn->mem,(unsigned long long)extra_off,(unsigned long long)sn->avail,t[k]);}
   break;}}
 if(!m34a_on())return;
 /* ① 按存储地址：族 id 与写目标共用同一块池存储 ⇒ 这条写命令打的就是族里某一张。 */
 for(unsigned i=0;i<g_m34a_fam_n;i++){
  m34a_fam_t *f=&g_m34a_fam[i];
  if(!f->store||!f->avail)continue;
  uint64_t elo=f->store,ehi=elo+f->avail;
  if(!(lo<ehi&&elo<hi))continue;
  g_gpuop_hit_tgt++;
  if(!f->hit){f->hit=1;
   int txid=-1;int slot=m34a_store_slot((uint64_t)(uintptr_t)sn->mem,sn->d.width,sn->d.height,(int)sn->d.format,&txid);
   vk_boot_logf("[m34a] hit fam id=%d store=%#llx avail=%llu %ux%u fmt=0x%x <- write dst=%p dst_id=%d %ux%u fmt=0x%x rt_slot=%d rt_texid=%d",
    f->id,(unsigned long long)f->store,(unsigned long long)f->avail,f->w,f->h,f->fmt,
    (void*)sn->mem,dst_id,sn->d.width,sn->d.height,(unsigned)sn->d.format,slot,txid);}
 }
 /* ② 按「本次 opcode 的真实目标 id」查族 id 集。 */
 if(dst_id>=0)for(unsigned i=0;i<g_m34a_fam_n;i++)if(g_m34a_fam[i].id==dst_id){g_gpuop_hit_id++;break;}
}
/* M34A：exec 行尾的对齐后缀（关态返回空串 ⇒ 那几行逐字不变）。 */
static void m34a_align_suffix(char *b,size_t n,const texsnap *sn,int dst_id)
{
 b[0]=0;
 if(!m34a_on()||!sn||!sn->mem)return;
 {int txid=-1;int slot=m34a_store_slot((uint64_t)(uintptr_t)sn->mem,sn->d.width,sn->d.height,(int)sn->d.format,&txid);
  int fam=m34a_fam_id_by_store((uint64_t)(uintptr_t)sn->mem);
  snprintf(b,n," m34a_rt_slot=%d m34a_rt_texid=%d m34a_fam_id=%d m34a_fam_n=%u",slot,txid,fam,g_m34a_fam_n);}
}
/* 写前几何自检：只在 level0/layer0、区域落在该级尺寸内、且快照 avail 覆盖整份布局时放行。
   目的是**不触发 texels_move 的 RED 护栏**（RED 行会污染健康判据），不是放宽语义。 */
static int gpuop_geom_ok(const texsnap *sn,const copy_region_h *r){
 if(!sn||!r||r->width<=0||r->height<=0)return 0;
 if(r->xoffset<0||r->yoffset<0||r->zoffset!=0)return 0;
 if((uint32_t)r->xoffset+(uint32_t)r->width>sn->d.width)return 0;
 if((uint32_t)r->yoffset+(uint32_t)r->height>sn->d.height)return 0;
 nvn_texlayout L;nvn_tex_layout(&sn->d,&L);
 if(!L.size||L.size>sn->avail)return 0;
 return 1;}
static int gpuop_clear_exec(const void *tex,const void *view,const void *region,const float *rgba,const uint32_t *rgba_u,int is_uint){
 if(!gpuop_write_enabled()){g_gpuop_gated++;return 0;}
 const copy_region_h *r=(const copy_region_h *)region;
 const texsnap *sn=gpuop_resolve(tex);
 if(!sn||!sn->mem||!sn->avail){g_gpuop_skip_unres++;return 0;}
 if(!gpuop_geom_ok(sn,r)){g_gpuop_skip_geom++;return 0;}
 {const view_h *v=(const view_h *)view;
  if(v&&v->bits&&((v->bits&1)||(v->bits&2))){g_gpuop_skip_geom++;return 0;}   /* 带级/层视图：不猜，计 geom */ }
 unsigned fmt=(unsigned)sn->d.format;
 int bw=1,bh=1,bpb=nvn_fmt_bpb((int)fmt,&bw,&bh);
 if(fmt<0x25||fmt>0x28||bpb!=4){g_gpuop_skip_fmt++;
  {static unsigned said;if(said++<6)vk_boot_logf("[gpu] clear skip fmt=%#x id-unknown ext=%ux%u (只支持 RGBA8 族 0x25..0x28)",fmt,sn->d.width,sn->d.height);}
  return 0;}
 uint32_t px=0;
 if(is_uint)px=rgba_u?rgba_u[0]:0;
 else {uint8_t c[4]={0,0,0,255};
  for(int i=0;i<4;i++){float f=rgba?rgba[i]:0.f;if(!(f>0.f))f=0.f;else if(f>1.f)f=1.f;c[i]=(uint8_t)(f*255.f+0.5f);}
  px=(uint32_t)c[0]|((uint32_t)c[1]<<8)|((uint32_t)c[2]<<16)|((uint32_t)c[3]<<24);}
 if((uint32_t)r->width>16384u){g_gpuop_skip_geom++;return 0;}
 size_t wb=(size_t)r->width*4;
 uint8_t *rowbuf=(uint8_t *)malloc(wb?wb:1);
 if(!rowbuf){g_gpuop_skip_geom++;return 0;}
 for(uint32_t x=0;x<(uint32_t)r->width;x++)memcpy(rowbuf+(size_t)x*4,&px,4);
 tex_h tmp;memset(&tmp,0,sizeof tmp);tmp.d=sn->d;tmp.mem=sn->mem;
 {nvn_texlayout L;nvn_tex_layout(&sn->d,&L);
  if(!gpuop_span_ok(sn->mem,L.size)){free(rowbuf);g_gpuop_skip_unmapped++;return 0;}}
 texels_move(&tmp,(const view_h *)view,r,rowbuf,0,0,1);
 free(rowbuf);
 g_gpuop_exec++;
 {int d_id=g_tex_id_by_key(tex);
  gpuop_hit_probe(sn,0,d_id);   /* M34A：传本次 opcode 的**真实目标 id**（旧版这里没有 id，只能查死名单） */
  {static unsigned said;if(said++<8){nvn_texlayout Lz;nvn_tex_layout(&sn->d,&Lz);
   char m34a[128];m34a_align_suffix(m34a,sizeof m34a,sn,d_id);
   vk_boot_logf("[gpu] clear exec dst=%p span=%llu id=%d ext=%ux%u fmt=%#x region=%d,%d+%dx%d px=%08x nz_now=%llu/%llu%s",
    (void*)sn->mem,(unsigned long long)Lz.size,d_id,sn->d.width,sn->d.height,fmt,r->xoffset,r->yoffset,r->width,r->height,px,
    (unsigned long long)ih_nz((const uint8_t*)sn->mem,sn->avail<65536?sn->avail:65536,NULL),(unsigned long long)sn->avail,m34a);}}}
 return 1;}
static int gpuop_t2t_exec(const void *stex,const void *sview,const void *sregion,const void *dtex,const void *dview,const void *dregion){
 if(!gpuop_write_enabled()){g_gpuop_gated++;return 0;}
 const copy_region_h *sr=(const copy_region_h *)sregion,*dr=(const copy_region_h *)dregion;
 const texsnap *ss=gpuop_resolve(stex),*ds=gpuop_resolve(dtex);
 if(!ss||!ds||!ss->mem||!ds->mem){g_gpuop_skip_unres++;return 0;}
 if(!gpuop_geom_ok(ss,sr)||!gpuop_geom_ok(ds,dr)){g_gpuop_skip_geom++;return 0;}
 {const view_h *v1=(const view_h *)sview,*v2=(const view_h *)dview;
  if((v1&&v1->bits&&((v1->bits&1)||(v1->bits&2)))||(v2&&v2->bits&&((v2->bits&1)||(v2->bits&2)))){g_gpuop_skip_geom++;return 0;}}
 if(ss->d.format!=ds->d.format){g_gpuop_skip_fmt++;return 0;}
 int bw=1,bh=1,bpb=nvn_fmt_bpb((int)ss->d.format,&bw,&bh);
 if(!bpb){g_gpuop_skip_fmt++;return 0;}
 if(sr->width!=dr->width||sr->height!=dr->height){g_gpuop_skip_geom++;return 0;}
 uint64_t wb=((uint64_t)sr->width+bw-1)/bw*(uint64_t)bpb,hb=((uint64_t)sr->height+bh-1)/bh;
 if(!wb||!hb||wb*hb>4ull*1024*1024){g_gpuop_skip_geom++;return 0;}
 uint8_t *buf=(uint8_t *)malloc((size_t)(wb*hb));
 if(!buf){g_gpuop_skip_geom++;return 0;}
 tex_h st;memset(&st,0,sizeof st);st.d=ss->d;st.mem=ss->mem;
 tex_h dt;memset(&dt,0,sizeof dt);dt.d=ds->d;dt.mem=ds->mem;
 {nvn_texlayout Ll;nvn_tex_layout(&ss->d,&Ll);uint64_t need=Ll.size>ds->avail?ds->avail:Ll.size;
  if(!gpuop_span_ok(ss->mem,Ll.size)||!gpuop_span_ok(ds->mem,need)){free(buf);g_gpuop_skip_unmapped++;return 0;}}
 texels_move(&st,(const view_h *)sview,sr,buf,0,0,0);
 texels_move(&dt,(const view_h *)dview,dr,buf,0,0,1);
 free(buf);
 g_gpuop_exec++;
 {int d_id=g_tex_id_by_key(dtex);
  gpuop_hit_probe(ds,0,d_id);   /* M34A：copy 的判据是**目标**（dst）的存储与身份 */
  {static unsigned said;if(said++<8){char m34a[128];m34a_align_suffix(m34a,sizeof m34a,ds,d_id);
   vk_boot_logf("[gpu] copy_t2t exec src=%p dst=%p fmt=%#x region=%ux%u -> %lluB placed%s",
    (void*)ss->mem,(void*)ds->mem,(unsigned)ss->d.format,sr->width,sr->height,(unsigned long long)(wb*hb),m34a);}}}
 return 1;}
static void nvn_texture_write_texels(const void *t,const void *v,const void *r,const void *src){texels_move((tex_h *)t,(const view_h *)v,(const copy_region_h *)r,(uint8_t *)(uintptr_t)src,0,0,1);vk_tex_upload_try((tex_h *)t);}   /* 批2：写后整图重传（池内自洽读写语义不变；boot 实测 8 次小图，开销可忽略） */
static void nvn_texture_write_texels_strided(const void *t,const void *v,const void *r,const void *src,ptrdiff_t row,ptrdiff_t img){texels_move((tex_h *)t,(const view_h *)v,(const copy_region_h *)r,(uint8_t *)(uintptr_t)src,row,img,1);vk_tex_upload_try((tex_h *)t);}   /* 批2 同上 */
static void nvn_texture_read_texels(const void *t,const void *v,const void *r,void *dst){texels_move((tex_h *)t,(const view_h *)v,(const copy_region_h *)r,dst,0,0,0);}
static void nvn_texture_read_texels_strided(const void *t,const void *v,const void *r,void *dst,ptrdiff_t row,ptrdiff_t img){texels_move((tex_h *)t,(const view_h *)v,(const copy_region_h *)r,dst,row,img,0);}
/* round26：Program 层接库。round25 崩链：常驻 shader 库装库走查（'常駐[シェーダー]' main+0x4bf568 →
   main+0x90a76c 载入 → main+0x902374 走查）首条目两阶段指针（条目 +0x10/+0x0，0x40 步长，字典表
   main+0x132af18 {0x10,0x8,0x0,-1}）全 NULL——boot 期默认 program 创建（main+0x94e608/0x94e760 唯一两次
   调用，round25.log:1564/1566）把 ProgramInitialize/SetShaders 桩返 0 当失败，program 记录未入库；
   走查 0x90245c cbz 后 0x902470 mov w8,#0x68; ldr x0,[x8] 编译器空路径硬解引 → fault=0x68。
   donor hle_nvn.c:553-555/580-587 语义：Initialize 盖 'PROG' 返 1；SetShaders 校验 0..6 阶段记账返 1
   （无 mtl/replay 层，boot 期只保真 NVNboolean 与阶段数）。 */
typedef struct{uint32_t magic;int32_t nstage;} prog_h;   /* prog_t 前缀（donor nvn_obj.h），游戏侧分配尺寸未知故只写 8B 头 */
static uint64_t nvn_program_initialize(void *p,void *d){(void)d;((prog_h *)p)->magic=0x50524f47;((prog_h *)p)->nstage=0;fprintf(stderr,"[nvn] ProgramInitialize %p\n",p);return 1;}   /* 'PROG'，donor :553-555（memset 全对象改为只写头，游戏分配尺寸未证） */
static void nvn_program_finalize(void *p){((prog_h *)p)->magic=0;}   /* donor :556 去 mtl 释放只留清 magic */
static void nvn_program_set_debug_label(void *p,const char *l){NM_SC_N("nvnProgramSetDebugLabel");(void)p;(void)l;}   /* donor 记名空操作 */
/* M3u（2026-11-03）P3：program→key 缺口六计数（只查不改）。
   · prog_calls = SetShaders 调用次数；key_ok/key_bad = 每次调用里逐 stage 的 key 计算成败；
   · vs_only/fs_only = 本次调用只拿到 VS 或只拿到 FS（另一路 key 缺失/失败）；
   · both = 本次调用两路齐备；paired（nvn_vk 侧）= 登记表里最终 VS+FS 都非空的 program 数。
   计数器本体在文件顶部（报告行先于本函数）。 */
static uint64_t nvn_program_set_shaders(void *p,int n,const void *sd){
 if(n<0||n>6){fprintf(stderr,"[nvn] ProgramSetShaders stages=%d【红】\n",n);return 0;}   /* donor :581 同款校验 */
 ((prog_h *)p)->nstage=n;   /* boot 期只记账：shader 本体由后续 Vulkan 后端接线（donor :582-586 拷 sd+control_intern，无 replay 层省略） */
 {static unsigned k;if(k<8){k++;const uint64_t *s=sd;fprintf(stderr,"[nvn] ProgramSetShaders %p n=%d data0=%#llx\n",p,n,n?(unsigned long long)s[0]:0ull);}}   /* 首个 data=GPU code 地址，定位用一次性日志 */
 shader_dump_program(n,sd);   /* M3p：真语料落盘（边界校验先行；失败只计数，返回值/渲染路径不变） */
 /* M3r：把本 program 的两阶段 key 登记到后端（stage0→VS，stage1→FS）。同一 program 的两次
    SetShaders 调用按 p 配对；本轮语料每个 key 只有一阶段，所以必须靠 prog 指针把 VS/FS 拼起来。
    单阶段调用（n=1）的另一路留空——draw 时查不到 FS 的那笔跳过并计数（P2 口径）。 */
 {char k[6][17];const char *vsk=NULL,*fsk=NULL;int nv=(n>6?6:n);
  g_pg_calls++;   /* M3u P3：六计数（口径见 [p] program totals 行） */
  for(int i=0;i<nv;i++){int st=shader_dump_key_at(sd,i,(uint64_t)(uintptr_t)p,k[i]);
   if(st>=0)g_pg_key_ok++;else g_pg_key_bad++;
   if(st==0&&!vsk)vsk=k[i];else if(st==1&&!fsk)fsk=k[i];
   /* M4bd P1：顺手把该 stage 的 c1 常量段登记进后端（只登记 + 一行日志，不改渲染）。
      NVNshaderData = {u64 data(code addr); const void *control}。 */
   if(st>=0){const uint8_t *e=(const uint8_t *)sd+(size_t)i*16;uint64_t ca=0,cv=0;
    memcpy(&ca,e,8);memcpy(&cv,e+8,8);
    nvn_vk_shader_c1(k[i],st,(const void *)(uintptr_t)cv,ca);}}
  if(vsk&&fsk)g_pg_both++;else if(vsk)g_pg_vs_only++;else if(fsk)g_pg_fs_only++;
  nvn_vk_program_set_keys((uint64_t)(uintptr_t)p,vsk,fsk);}
 return 1;
}
/* round16：描述符池（TexturePool/SamplerPool）全家——donor hle_nvn.c:301-324 简化（无 mtl/life 层）。
   round14b 对齐 GetSize：donor :274/:277 签名 (const NVNtexturePool*) 返回 ((const dpool_t*)tp)->n
   ——池对象上的查询（返回创建时条目数 int），不是计数→字节静态换算（donor 注释：Unreal Engine 在
   device setup 时读回池大小）。旧版把入参当条目数返回 (n*0x100+0x1000+0xfff)&~0xfff 字节：游戏传
   的是池对象指针，指针值被当条目数 → 巨大返回值；GetSize(n) 字节换算公式 donor/官方头均无出处，
   已删。同名符号只能有一个：对象级 GetSize 不再另设回声版。 */
typedef struct{const void *pool;int64_t off;int32_t n;} dpool_h;   /* =nvn_obj.h:27 dpool_t */
static uint64_t nvn_texture_pool_get_size(const dpool_h *p){int n=p?p->n:0;{static unsigned k;if(k++<4)fprintf(stderr,"[nvn] TexturePoolGetSize pool=%p -> %d (entries, donor :274)\n",(const void *)p,n);}return (uint64_t)(int64_t)n;}
static uint64_t nvn_sampler_pool_get_size(const dpool_h *p){int n=p?p->n:0;{static unsigned k;if(k++<4)fprintf(stderr,"[nvn] SamplerPoolGetSize pool=%p -> %d (entries, donor :277)\n",(const void *)p,n);}return (uint64_t)(int64_t)n;}
static uint64_t nvn_texture_pool_initialize(void *tp,const void *p,int64_t off,int n){dpool_h *o=tp;o->pool=p;o->off=off;o->n=n;fprintf(stderr,"[nvn] TexturePool %p pool=%p off=%#llx n=%d\n",tp,p,(unsigned long long)off,n);return 1;}   /* donor :301-303（无 mtl_set_tex_pool_n） */
static void nvn_texture_pool_finalize(void *tp){NM_SC_N("nvnTexturePoolFinalize");(void)tp;}   /* donor :304 */
/* M3y P2：登记表本体在 tex_h 定义之后（回放侧 bind_tex 先用它），这里只留登记入口。 */
static void nvn_texture_pool_register_texture(const void *tp,int id,const void *t,const void *v){
 const texsnap *sn=texrec_get(t,0);
 texreg_put(tp,id,t,NULL);
 {static unsigned long long n;g_texreg_calls=n;
  if(n<16||n%4096==0)fprintf(stderr,"[nvn] TexturePoolRegister id=%d tex=%p mem=%p (%llu-th, table=%u)\n",id,t,sn?(void*)sn->mem:NULL,n,g_texreg_n);
  if(n<48||n%2048==0)vk_boot_logf("[t] reg id=%d tex=%p mem=%p view=%p pool=%p (%llu-th) snap=%d",
   id,t,sn?(void*)sn->mem:NULL,v,(const void*)tp,n,sn?1:0);
  n++;}}   /* donor :306-313 只记名；我们额外存 id→tex 快照（P2 需要） */
static void nvn_texture_pool_register_image(const void *tp,int id,const void *t,const void *v){nvn_texture_pool_register_texture(tp,id,t,v);}   /* donor :317-319 转发 RegisterTexture */
static void *nvn_texture_pool_get_memory_pool(const void *tp){return ((dpool_h *)tp)->pool;}
static int64_t nvn_texture_pool_get_memory_offset(const void *tp){return ((const dpool_h *)tp)->off;}
static void nvn_texture_pool_set_debug_label(void *tp,const char *l){NM_SC_N("nvnTexturePoolSetDebugLabel");(void)tp;(void)l;}
static uint64_t nvn_sampler_pool_initialize(void *sp,const void *p,int64_t off,int n){dpool_h *o=sp;o->pool=p;o->off=off;o->n=n;fprintf(stderr,"[nvn] SamplerPool %p pool=%p off=%#llx n=%d\n",sp,p,(unsigned long long)off,n);return 1;}   /* donor :320-322 */
static void nvn_sampler_pool_finalize(void *sp){NM_SC_N("nvnSamplerPoolFinalize");(void)sp;}   /* donor :323 */
static void nvn_sampler_pool_register_sampler(const void *sp,int id,const void *s){(void)sp;{static unsigned long long n;if(n<32||n%8192==0)fprintf(stderr,"[nvn] SamplerPoolRegister id=%d (%llu-th, rate-limited)\n",id,n);n++;}
 /* M4z P2②：认领那一刻的参数就是游戏给这个 sid 的**真实意图**（magic 校验通过才读） */
 if(id>=0&&id<SAMP_REG_N){const samp_h *h=s;if(h&&h->magic==0x534D5000){
  g_samp_reg[id].ok=1;g_samp_reg[id].ws=h->p.ws;g_samp_reg[id].wt=h->p.wt;g_samp_reg[id].wr=h->p.wr;
  g_samp_reg[id].minf=h->p.minf;g_samp_reg[id].magf=h->p.magf;
  g_samp_reg[id].lod_min=h->p.lod_min;g_samp_reg[id].lod_max=h->p.lod_max;}}}   /* donor :324 记名空操作；boot 实测 id 锯齿回绕=槽复用，属每帧正常重注册 */
/* M3y P1：RegisterSamplerBuilder（donor 无实现，grep 空）——把 builder 的 44B 参数按 (池,id) 存一份。
   本轮回放侧强制「最近邻 + REPEAT」（派活单口径），采样参数暂不参与绑定，只留查询/下一轮用。 */
#define SAMPREG_N 4096
static samp_par_h g_sampreg[SAMPREG_N];
static unsigned long long g_sampreg_calls;
static void nvn_sampler_pool_register_sampler_builder(const void *sp,int id,const void *b){
 g_sampreg_calls++;
 if(id<0||id>=SAMPREG_N)return;
 g_sampreg[id]=((const samp_builder_h *)b)->p;
 {static unsigned long long n;if(n<8)fprintf(stderr,"[nvn] SamplerPoolRegisterBuilder id=%d min=%u mag=%u (params stored, replay forces nearest/repeat)\n",id,g_sampreg[id].minf,g_sampreg[id].magf);n++;}}
static void *nvn_sampler_pool_get_memory_pool(const void *sp){return ((dpool_h *)sp)->pool;}
static int64_t nvn_sampler_pool_get_memory_offset(const void *sp){return ((const dpool_h *)sp)->off;}
static void nvn_sampler_pool_set_debug_label(void *sp,const char *l){NM_SC_N("nvnSamplerPoolSetDebugLabel");(void)sp;(void)l;}
/* 第 2 步（2026-11-02）：录制族在 cmdbuf 里，EndRecording 的句柄=命令流首条记录地址（cmdbuf.c:247 RET(cb->start)）。
   回执契约（round15b，本文件 :719-773）：提交退休时按句柄把 ReportCounter/ReportValue/SignalEvent 的
   客体内存写回；nvn_rcpt_submit(:746) 靠 nvn_rcpt_lookup(:740) 反查 CB 对象，所以 EndRecording 必须把
   「新句柄→CB 客体」补登记，否则每次提交都走 unknown_handle，回执静默丢。Finalize 时同步弃该 CB 的回执。 */
static uint64_t nvn_cb_end_recording_wired(void *cb){
 uint64_t h=nvn_rec_end_recording(cb);
 if(h){nvn_rcpt_map(h,(cmd_h *)cb);   /* 键1：EndRecording 返回句柄（=cb->start，cmdbuf.c:286） */
  /* 键2（round16-A 双键登记）：提交侧 hs[i] 传的是客体 CommandBuffer 指针本身（与本文收到的 cb 同地址），
     与键1不同域，lookup 恒 miss（实测 unknown=1000/writes=0）。用客体指针再登记一次。 */
  nvn_rcpt_map((uint64_t)(uintptr_t)cb,(cmd_h *)cb);}
 g_nvn_ctl_total+=64;   /* donor :1086 / 原 stub:570：控制内存记账 64B/CB（QueueGetTotalControlMemoryUsed 的输入） */
 return h;
}
/* M5p B（2026-11-05）：Finalize 不再弃回执/句柄映射。
   实测（boot#1 探针）：游戏对 CB 的顺序是 Begin -> record(ReportCounter) -> End -> **Finalize** -> submit，
   而原实现把 drop 挂在 Finalize 上 ⇒ 提交到来时 hmap_c 已被置 NULL（hmap_h 还在）、回执已被删，
   于是每次提交都 unknown、writes 恒 0（pending 恒 0）。donor 只在**提交完成**时退休回执，
   Finalize 不是「重录」——真正的重录是 BeginRecording（命令内存重置），drop 移过去。 */
static void nvn_cb_begin_recording_wired(void *cb){nvn_rcpt_drop(cb);nvn_rec_begin_recording(cb);}
static void nvn_cb_finalize_wired(void *cb){nvn_rec_command_buffer_finalize(cb);}

/* ---------------- M3i（2026-11-03）：补录制缺口 + SET_RENDER_TARGETS 真绑定 ----------------
   A 补录制：这 5 个绑定此前只走 CB_REC 记账桩（24B 近似），不进命令流——直方图里 0 条顶点绑定，
     所以 DRAW_ELEMENTS 回放没有顶点来源。cmdbuf.c:316-326 早有录制实现，这里只补 wrapper + 表项；
     **原记账保留**（先调旧桩再录制，行为不变、账不变）。
   B RT 绑定：SET_RENDER_TARGETS 早已在录制流里（表项就是 nvn_rec_*，直方图 130419 条），缺的是
     回放侧执行——nvn_cmd_exec 此前只认 CLEAR_COLOR。本轮把 RT 交 nvn_vk_rt_bind（离屏 RT、
     COLOR_ATTACHMENT_OPTIMAL、viewport/scissor 取自 RT），清屏前先 flush 该批次。 */
/* M13A（2026-11-08，只读）：把 guest 最近一次 bind 的 NVN blend state 的 7 个 int32 记下来，给
   nvn_vk.c 的逐笔探针打印 —— 这是 B1 假设（「源 alpha=0 但混合被关 ⇒ 直接写黑」）唯一能实测的
   blend 面：本 port 的管线 blendEnable 恒 VK_FALSE（r_pipe_ensure），从不解读 NVN blend state，
   而 guest 每帧要 bind ~26 次（M13A a2: B-stub calls bind_blend_state=173301）。
   NVNblendState 的前 7 个 int32 = (target, src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a)
   （third_party/d2r_gfx/gfx/nvn/hle_nvn.c:2046-2051 的 blend_st 布局）。只读、不改任何状态。 */
static int g_m13a_bl[8];
static unsigned long long g_m13a_bl_lr;
void m13a_blend_note(const void *st, unsigned long long lr)
{
    g_m13a_bl_lr = lr;
    if (!st) return;
    const int *p = (const int *)st;
    for (int i = 0; i < 7; i++) g_m13a_bl[i] = p[i];
    g_m13a_bl[7]++;
}
int m13a_blend_get(int out[8], unsigned long long *lr)
{ for (int i = 0; i < 8; i++) out[i] = g_m13a_bl[i]; if (lr) *lr = g_m13a_bl_lr; return out[7] ? 1 : 0; }
/* 同一只探针的 UBO 面：guest 每次 bind uniform buffer 时把 (stage, bank, 客体内存地址, size, LR) 记下来，
   并从**客体内存**读前 8 个 float —— 这是「客人生成的常量」与「我们 arena 里给着色器的那份」之间的
   独立对照（B2 假设：uniform 解码/搬运是否把颜色或 alpha 读错）。只读。 */
typedef struct { int st, bank, ok; unsigned size; unsigned long long addr, lr; float f[8]; } m13a_ub_t;
static m13a_ub_t g_m13a_ub[16];
static int g_m13a_ub_n;
void m13a_ub_note(int st, int bank, uint64_t addr, uint64_t size, unsigned long long lr)
{
    if (bank < 0 || bank > 31) return;
    int i = g_m13a_ub_n++ & 15;
    m13a_ub_t *e = &g_m13a_ub[i];
    memset(e, 0, sizeof *e);
    e->st = st; e->bank = bank; e->addr = (unsigned long long)addr; e->size = (unsigned)size; e->lr = lr;
    if (addr > 0x10000ull && size >= 4ull) {
        memcpy(e->f, (const void *)(uintptr_t)addr, sizeof e->f < size ? sizeof e->f : (size_t)size);
        e->ok = 1;
    }
}
int m13a_ub_get(int bank, int st, unsigned long long *lr, unsigned *size, unsigned long long *addr, float out[8])
{
    for (int k = 0; k < 16; k++) {
        m13a_ub_t *e = &g_m13a_ub[(g_m13a_ub_n - 1 - k) & 15];
        if (e->bank != bank || e->st != st || !e->ok) continue;
        if (lr) *lr = e->lr; if (size) *size = e->size; if (addr) *addr = e->addr;
        memcpy(out, e->f, sizeof e->f);
        return 1;
    }
    return 0;
}
static void nvn_cb_rec_bind_blend_state(void *c,const void *st){m14b_sink_wire_note();m13a_blend_note(st,(unsigned long long)(uintptr_t)__builtin_return_address(0));nvn_command_buffer_bind_blend_state(c);nvn_rec_bind_blend_state(c,st);}
static void nvn_cb_rec_bind_color_state(void *c,const void *st){nvn_command_buffer_bind_color_state(c);nvn_rec_bind_color_state(c,st);}
static void nvn_cb_rec_bind_depth_stencil_state(void *c,const void *st){nvn_command_buffer_bind_depth_stencil_state(c);nvn_rec_bind_depth_stencil_state(c,st);}
/* M3s（2026-11-03）：P1 取证——原始 ABI 四参在这里打（本函数 level-0 返回地址就是游戏调用点；
   再往上一级要 frame pointer，glue 层不跨帧取）。前 20 条各一行，之后静默。 */
static unsigned g_v_attr_raw, g_v_str_raw;
static void nvn_cb_rec_bind_vertex_attrib_state(void *c,int n,const void *st)
{
 if(g_v_attr_raw++<20)
  vk_boot_logf("[v] attrib_raw cb=%p n=%llu p=%p a3=%#llx lr=%#llx fn=%p",c,
   (unsigned long long)(uint64_t)n,st,0ull,
   (unsigned long long)(uintptr_t)__builtin_return_address(0),(void *)(uintptr_t)&nvn_rec_bind_vertex_attrib_state);
 nvn_command_buffer_bind_vertex_attrib_state(c,n,st);nvn_rec_bind_vertex_attrib_state(c,n,st);
}
static void nvn_cb_rec_bind_vertex_stream_state(void *c,int n,const void *st)
{
 if(g_v_str_raw++<20)
  vk_boot_logf("[v] stream_raw cb=%p n=%llu p=%p a3=%#llx lr=%#llx fn=%p",c,
   (unsigned long long)(uint64_t)n,st,0ull,
   (unsigned long long)(uintptr_t)__builtin_return_address(0),(void *)(uintptr_t)&nvn_rec_bind_vertex_stream_state);
 nvn_command_buffer_bind_vertex_stream_state(c,n,st);nvn_rec_bind_vertex_stream_state(c,n,st);
}
/* M3r（2026-11-03）：单条绑定形式此前只记账、不进命令流（实测流里 BIND_VERTEX_BUFFERS/BIND_UNIFORM_BUFFERS/
   BIND_TEXTURES 全为 0 条——SMO 用的是 nvnCommandBufferBindVertexBuffer/…Texture 单条形式）。
   这里保留原记账（账不变），再各补一次录制。真实 draw 的顶点地址就来自这三条里的 VertexBuffer。 */
static void nvn_cb_rec_bind_vertex_buffer(void *c,int first,uint64_t addr,uint64_t n){nvn_command_buffer_bind_vertex_buffer(c,first,addr,n);nvn_rec_bind_vertex_buffer(c,first,addr,n);}
static void nvn_cb_rec_bind_uniform_buffer(void *c,int st,int i,uint64_t a,uint64_t n){m13a_ub_note(st,i,a,n,(unsigned long long)(uintptr_t)__builtin_return_address(0));nvn_command_buffer_bind_uniform_buffer(c,st,i,a,n);nvn_rec_bind_uniform_buffer(c,st,i,a,n);}
static void nvn_cb_rec_bind_texture(void *c,int st,int i,uint64_t h){nvn_command_buffer_bind_texture(c,st,i,h);nvn_rec_bind_texture(c,st,i,h);}
/* M3s（2026-11-03）P1 结论定性：顶点状态全零不是 ABI 搬错，也不是游戏「真传零」——
   游戏用 SDK 的 nvnVertexAttribState 与 nvnVertexStreamState 系列 helper 填状态表，而这张表里
   （旧版）**一个都没有**；nvn_get_proc_address 对表外名字一律返 nvn_named_stub（返回 0 的可调桩），
   于是 SetDefaults/SetFormat/SetStreamIndex/SetStride 全是空操作，状态表永远保持 0，
   1.1M 次 BIND 带的全是同一块零数组。这里按 NVN ABI 补齐 10 个 helper：
     va_st{fmt:8, offset:19, stream:5}（nvn_obj.h:104）    vs_st{stride:u32, divisor:u32}（:105） */
static void nvn_va_set_defaults(void *s){*(uint32_t *)s=0;}
static void nvn_va_set_format(void *s,unsigned f,int64_t off){
 uint32_t v=*(uint32_t *)s;
 if(off<0||off>=(1<<19)){static unsigned ow;if(ow++<3)vk_boot_logf("[v-red] attrib offset out of range: %lld",(long long)off);}
 v=(v&~0xFFFFFFu)|((f&0xFFu)|(((uint32_t)off&0x7FFFFu)<<8));
 *(uint32_t *)s=v;
}
static void nvn_va_set_stream(void *s,int i){uint32_t v=*(uint32_t *)s;v=(v&0x07FFFFFFu)|(((uint32_t)i&0x1Fu)<<27);*(uint32_t *)s=v;}
static void nvn_va_get_format(const void *s,unsigned *f,int64_t *off){
 uint32_t v=*(const uint32_t *)s;if(f)*f=v&0xFFu;if(off)*off=(int64_t)((v>>8)&0x7FFFFu);
}
static int nvn_va_get_stream(const void *s){return (int)((*(const uint32_t *)s>>27)&0x1Fu);}
static void nvn_vs_set_defaults(void *s){((uint32_t *)s)[0]=0;((uint32_t *)s)[1]=0;}
static void nvn_vs_set_stride(void *s,int64_t st){((uint32_t *)s)[0]=(uint32_t)st;}
static void nvn_vs_set_divisor(void *s,int d){((uint32_t *)s)[1]=(uint32_t)d;}
static int64_t nvn_vs_get_stride(const void *s){return (int64_t)((const uint32_t *)s)[0];}
static int nvn_vs_get_divisor(const void *s){return (int)((const uint32_t *)s)[1];}

static void *nvn_lookup(const char *name);   /* fwd：nvn_get_proc_address 要调它 */
/* M3y P1 取证：本轮补进表里的 54 个纹理/采样器名字，首次被游戏请求时打一行
   `[x] proc <name> -> host`（证明走的是宿主实现而不是空桩；每个名字一次）。 */
static const char *const k_m3y_procs[]={
 "nvnTextureBuilderSetSwizzle","nvnTextureBuilderSetDepthStencilMode","nvnTextureBuilderSetGLTextureName",
 "nvnTextureBuilderSetPackagedTextureData","nvnTextureBuilderSetPackagedTextureLayout",
 "nvnTextureBuilderGetWidth","nvnTextureBuilderGetHeight","nvnTextureBuilderGetDepth","nvnTextureBuilderGetLevels",
 "nvnTextureBuilderGetFormat","nvnTextureBuilderGetFlags","nvnTextureBuilderGetTarget","nvnTextureBuilderGetSamples",
 "nvnTextureBuilderGetStride","nvnTextureBuilderGetSwizzle","nvnTextureBuilderGetMemoryPool",
 "nvnTextureBuilderGetMemoryOffset","nvnTextureBuilderGetPackagedTextureData","nvnTextureBuilderGetSparseTileLayout",
 "nvnTextureBuilderGetGLTextureName","nvnTextureBuilderGetZCullStorageSize","nvnTextureBuilderGetDepthStencilMode",
 "nvnTextureSetDebugLabel","nvnTextureGetDebugID","nvnTextureGetDepthStencilMode","nvnTextureGetSparseTileLayout",
 "nvnTextureGetZCullStorageSize","nvnTextureFlushTexels","nvnTextureInvalidateTexels",
 "nvnSamplerBuilderSetBorderColori","nvnSamplerBuilderSetBorderColorui","nvnSamplerBuilderGetMinMagFilter",
 "nvnSamplerBuilderGetWrapMode","nvnSamplerBuilderGetLodClamp","nvnSamplerBuilderGetLodBias",
 "nvnSamplerBuilderGetCompare","nvnSamplerBuilderGetBorderColor","nvnSamplerBuilderGetBorderColori",
 "nvnSamplerBuilderGetBorderColorui","nvnSamplerBuilderGetMaxAnisotropy","nvnSamplerBuilderGetReductionFilter",
 "nvnSamplerSetDebugLabel","nvnSamplerGetDebugID","nvnSamplerGetMinMagFilter","nvnSamplerGetWrapMode",
 "nvnSamplerGetLodClamp","nvnSamplerGetLodBias","nvnSamplerGetCompare","nvnSamplerGetBorderColor",
 "nvnSamplerGetBorderColori","nvnSamplerGetBorderColorui","nvnSamplerGetMaxAnisotropy",
 "nvnSamplerGetReductionFilter","nvnSamplerPoolRegisterSamplerBuilder",NULL};
static void m3y_proc_note(const char *n,void *fn){
 static unsigned char seen[64];static unsigned nseen;
 for(unsigned i=0;k_m3y_procs[i];i++)if(!strcmp(k_m3y_procs[i],n)){
  if(i<64&&!(seen[i])){seen[i]=1;nseen++;vk_boot_logf("[x] proc %s -> %s (%u/%u)",n,fn?"host":"stub",nseen,(unsigned)(sizeof k_m3y_procs/sizeof *k_m3y_procs-1));}
  return;}
}
static void *nvn_get_proc_address(void *device,const char *name){
 (void)device;const char *n=name?name:"";void *fn=nvn_lookup(n);
 /* M68B：客侧有没有来取 present 这枚地址（取过 ≠ 调过，见 M68B 判别表）。只多一次首字母判 + 一次 strcmp。 */
 if(n[0]=='n'&&!strcmp(n,"nvnQueuePresentTexture")){g_nvn_present_proc++;
   if(g_nvn_present_proc<=3)vk_boot_logf("[m68b-present] proc-request=%llu name=%s fn=%s",
                                        (unsigned long long)g_nvn_present_proc,n,fn?"host":"stub");}
 if(!strncmp(n,"nvnVertexAttribState",20)||!strncmp(n,"nvnVertexStreamState",20))
  vk_boot_logf("[v] proc %s -> %s",n,fn?"host":"stub");   /* M3s：确认状态 helper 走的是哪条解析通道 */
 m3y_proc_note(n,fn);   /* M3y P1：本轮补表的纹理/采样器名字（首请求一次） */
 fprintf(stderr,"[nvn] proc %s -> %s\n",n,fn?"host":"stub");
 if(!fn&&strstr(n,"Size"))fprintf(stderr,"[nvn] size-query handed out: %s\n",n);   /* round14-C：尺寸类名字（GetStorageSize/GetSize…）落返-0 桩的 grep 标记——C② 池 Initialize 被尺寸 0 短路的头号嫌疑 */
 return runtime_hle_wrap_named(fn?fn:nvn_named_stub(n),n);   /* round9：游戏填 461 槽后无空检查 blr，表外必须给可调桩、不得返 NULL；同 fn→同 thunk（hle_stack.c:82-94），指针比较一致，stub 也走宿主栈；round20-A 表外桩按名分槽（stub#i name 打点） */
}
/* ============== M14B（2026-11-08）：NVN 状态对象 setter 的真实现 ==============
   本轮 p1b 探针实测的两条确证：
    ① bind 时刻客体里的 NVNblendState 内容不是因子 —— 31744 次 bind 里 90% 读到全 0，
       余下读到的是 0x6e12dfcb48 / 0x7f45a8f2c4 这种**指针值**（distinct 只有 3 种）；
    ② 原因不在偏移：`nvnBlendStateSetDefaults/SetBlendFunc/SetBlendEquation`（以及 color 的
       SetBlendEnable/SetDefaults）在本移植的 entries[] 里**没有条目** ⇒ 落 nvn_named_stub 的
       返 -0 空桩（M13A 的 A-stub 报表把它们排在 called Top，正是「被调用但什么也没做」）。
       空桩 ⇒ 状态对象从未被写 ⇒ 读到的只能是 0 或游戏自己塞进去的指针。
   所以上一轮「7 个 int32 不是 NVN blend 因子」的根因是**内容没人写**，不是结构/偏移错。
   本节按 donor 的权威布局补齐：
     blend：third_party/d2r_gfx/gfx/nvn/nvn_obj.h:95
            `uint64_t target:3, src_rgb:7, dst_rgb:7, src_a:7, dst_a:7, eq_rgb:3, eq_a:3;`（8 字节整）
            donor hle_nvn.c:2046-2051 的 SetDefaults/Func/Equation/Target 语义逐条照抄；
            NVN 枚举 1 基（include/nvn.h:944-975）；缺省 = (ONE,ZERO,ONE,ZERO, ADD,ADD)。
     color：nvn_obj.h:93 `uint32_t blend_en:8, logic_op:4, alpha_func:4`（blend 的开关在这里）。
   写的是客体状态对象偏移 0（客体与宿主同址，本移植一贯口径）。
   **smo_m14b=0 时四个函数第一行就返回 ⇒ 与空桩等价（B0 逐字节同现状）**。 */
static void m14b_guest_store(void *st, uint64_t raw, unsigned n)
{
    if (!st || (uintptr_t)st < 0x10000ull || (uintptr_t)st >= 0x800000000000ull) return;
    memcpy(st, &raw, n);
}
static uint64_t m14b_pack_mk(int tgt, int sr, int dr, int sa, int da, int er, int ea)
{
    return ((uint64_t)tgt & 7ull) | (((uint64_t)sr & 0x7Full) << 3) | (((uint64_t)dr & 0x7Full) << 10) |
           (((uint64_t)sa & 0x7Full) << 17) | (((uint64_t)da & 0x7Full) << 24) |
           (((uint64_t)er & 7ull) << 31) | (((uint64_t)ea & 7ull) << 34);
}
/* NVN 缺省（donor nvnBlendStateSetDefaults）：target=0, ONE, ZERO, ONE, ZERO, ADD, ADD */
#define M14B_BLEND_DEF_PACK m14b_pack_mk(0, 2, 1, 2, 1, 1, 1)
static uint64_t m14b_pack_load(const void *st)
{
    uint64_t raw = M14B_BLEND_DEF_PACK;
    if (!st || (uintptr_t)st < 0x10000ull) return raw;
    memcpy(&raw, st, 8);
    if (raw >> 37) return M14B_BLEND_DEF_PACK;   /* 高位有值 ⇒ 不是我们写的（未初始化/指针）⇒ 用缺省 */
    return raw;
}
static void m14b_blend_set_defaults(void *s)
{ if (!nvn_vk_m14b_on() || !s) return; m14b_guest_store(s, M14B_BLEND_DEF_PACK, 8); }
static void m14b_blend_set_target(void *s, int t)
{ uint64_t r; if (!nvn_vk_m14b_on() || !s) return; r = m14b_pack_load(s);
  m14b_guest_store(s, (r & ~7ull) | ((uint64_t)t & 7ull), 8); }
static void m14b_blend_set_func(void *s, int sr, int dr, int sa, int da)
{ uint64_t r; if (!nvn_vk_m14b_on() || !s) return; r = m14b_pack_load(s);
  r = (r & ~((0x7Full << 3) | (0x7Full << 10) | (0x7Full << 17) | (0x7Full << 24)))
    | (((uint64_t)sr & 0x7Full) << 3) | (((uint64_t)dr & 0x7Full) << 10)
    | (((uint64_t)sa & 0x7Full) << 17) | (((uint64_t)da & 0x7Full) << 24);
  m14b_guest_store(s, r, 8); }
static void m14b_blend_set_equation(void *s, int er, int ea)
{ uint64_t r; if (!nvn_vk_m14b_on() || !s) return; r = m14b_pack_load(s);
  r = (r & ~((7ull << 31) | (7ull << 34)))
    | (((uint64_t)er & 7ull) << 31) | (((uint64_t)ea & 7ull) << 34);
  m14b_guest_store(s, r, 8); }
/* color：blend_en 是 8 位字段，其余（logic_op/alpha_func）原样保留 */
static void m14b_color_set_defaults(void *s)
{ uint32_t z = 0; if (!nvn_vk_m14b_on() || !s) return; m14b_guest_store(s, z, 4); }
static void m14b_color_set_blend_enable(void *s, int en)
{ uint32_t v = 0; if (!nvn_vk_m14b_on() || !s) return;
  memcpy(&v, s, 4); v = (v & ~0xFFu) | ((uint32_t)(en ? 1 : 0) & 0xFFu); m14b_guest_store(s, v, 4); }

/* ============== M62A（2026-11-11）：depth-stencil / polygon / channel-mask（+ logicOp）四族 setter ==============
   根因（M60A 物证，只引用不复核）：
     ① entries[] 里这四族 setter **0 条** ⇒ nvnGetProcAddress/nvn_lookup 未命中 ⇒ nvn_named_stub(:6379-6383)
        的返 -0 空桩 ⇒ **客体状态对象从未被写**（M13A A-stub 报表里它们正是「被调用但什么也没做」）；
     ② 1440 帧 106 次 BindDepthStencilState 里 80 次的 8 字节是**堆指针**（0x5f4086f2c0 / 0x6020eaaa58 …），
        按 ds_st 解出 dt=0 / st=1 / 模板比较函数 14、15（NVN 枚举是 1 基，nvn.h:996-1029 ⇒ 非法）；
     ③ 回放侧 gfx_vk_exec.c:2069 把 k.dt/k.dw/k.df/k.st 直接取自 S.ds（:2889 case 5 memcpy 进 ds_st）⇒
        模板测试被打开 + 比较函数非法 ⇒ 片元整片丢弃，只剩 clear 色（实测纯橙红 (224,96,40) 占 99.58%）。
   写法照 M14B（:6383-6439）：**写的是「指针指向的那块客体内存」**（客体与宿主同址，本移植一贯口径），
   按 nvn_obj.h 的权威位域 pack/unpack，绝不再把「指针自己的 8 字节」当结构体。
   位序必须逐位对齐 nvn_obj.h:82/83/88/89-92（回放侧 memcpy 的正是同一份）：
     ds_st     uint64: dt:1 dw:1 df:4 st:1 sf_f:4 sf_b:4 fail_f:4 zfail_f:4 zpass_f:4 fail_b:4 zfail_b:4 zpass_b:4（低 39 位）
     poly_st   uint32: cull:2 front:1 mode:2 offset_en:3                              （低 8 位）
     chmask_st uint32: mask（每 target 4 bit：r=1 g=2 b=4 a=8，置位=写）              （全 32 位）
     color_st  uint32: blend_en:8 logic_op:4 alpha_func:4                             （低 16 位）
   缺省值与 donor hle_nvn.c 的 *SetDefaults 逐项相同（nvn_vk2/rec/gfx_vk_exec.c:64-66 的初值也同款）。
   未初始化判别（M14B m14b_pack_load 同款思路，ds 用枚举合法域加强）：
     ds 除高位外再查 df/sf/op 是否落在 NVN 合法域 1..8 —— M60A 那些指针值（0x5f40…）的高 39 位看着像
       「合法 ds_st」（bit6=1），但 df=0 ⇒ 越界，只有按枚举域查才判得出来。
     poly 查 bit8..31、color 查 bit16..31；chmask 全 32 位都是 mask，没有余位可验，按 NVN 契约
       （SetDefaults 先行，M60A 的 stub 报表里 nvnChannelMaskStateSetDefaults 确有调用）直接读-改-写。
   **smo_m62a=0（缺省）时每个函数第一行就返回 ⇒ 与空桩等价（B0 逐字节等于 M61A 基线）**。 */
#define M62A_SW_A "/data/storage/el2/base/files/smo_m62a"                        /* app 沙箱视图（构造期只能在此种植） */
#define M62A_SW_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_m62a"         /* hdc 唯一可写视图（优先读） */
static int m62a_on(void)
{
 static int cached=-1; static long last;
 long now=(long)time(NULL);
 if(cached>=0&&now==last)return cached;
 last=now;cached=0;
 {const char *paths[2]={M62A_SW_B,M62A_SW_A};
  for(int i=0;i<2;i++){FILE *f=fopen(paths[i],"r");
   if(f){char b[16];size_t n=fread(b,1,sizeof b-1,f);fclose(f);if(n){b[n]=0;cached=atoi(b)>0;}break;}}}
 return cached;
}
/* 构造期种开关文件（与 m34a/m35a/m45a 同款：hdc shell 是 uid 2000，建不了 app 数据目录）。
   已存在则只把权限改回 0666、**不动内容** ⇒ 脚本能在起 app 前 `echo 1 >` 它。 */
__attribute__((constructor)) static void m62a_seed(void)
{
 const char *paths[2]={M62A_SW_A,M62A_SW_B};
 for(int i=0;i<2;i++){
  if(access(paths[i],F_OK)==0){chmod(paths[i],0666);return;}
  int fd=open(paths[i],O_CREAT|O_WRONLY|O_TRUNC,0666);
  if(fd>=0){ssize_t w=write(fd,"0\n",2);(void)w;close(fd);chmod(paths[i],0666);return;}}}
/* 客体状态对象写入：写指针指向的那块内存（不是指针自身的字节）。范围校验同 m14b_guest_store。 */
static void m62a_guest_store(void *st,uint64_t raw,unsigned n)
{
 if(!st||(uintptr_t)st<0x10000ull||(uintptr_t)st>=0x800000000000ull)return;
 memcpy(st,&raw,n);
}
/* 每族首次写入留一行取证（A/B 时直接对照 M60A 的指针值）。 */
static void m62a_probe(int fam,uint64_t raw)
{
 static unsigned char seen[4];
 if(fam<0||fam>3||seen[fam])return; seen[fam]=1;
 vk_boot_logf("[m62a] family %d (0=depthStencil 1=polygon 2=channelMask 3=color) first write raw=%#llx",fam,(unsigned long long)raw);
}
#define M62A_U64_PUT(r,sh,w,v) (((r)&~(((1ull<<(w))-1ull)<<(sh)))|(((uint64_t)(v)&((1ull<<(w))-1ull))<<(sh)))
#define M62A_U32_PUT(r,sh,w,v) (((r)&~(((1u<<(w))-1u)<<(sh)))|(((uint32_t)(v)&((1u<<(w))-1u))<<(sh)))
/* ---- depth-stencil（nvn_obj.h:89-92）---- */
static uint64_t m62a_ds_def(void)
{   /* dt=0 dw=1 df=LESS(2) st=0 sf_f=sf_b=ALWAYS(8) fail/zfail/zpass=KEEP(1)（donor hle_nvn.c:1180-1183） */
 uint64_t r=0;
 r=M62A_U64_PUT(r,0,1,0);  r=M62A_U64_PUT(r,1,1,1);  r=M62A_U64_PUT(r,2,4,2);  r=M62A_U64_PUT(r,6,1,0);
 r=M62A_U64_PUT(r,7,4,8);  r=M62A_U64_PUT(r,11,4,8);
 r=M62A_U64_PUT(r,15,4,1); r=M62A_U64_PUT(r,19,4,1); r=M62A_U64_PUT(r,23,4,1);
 r=M62A_U64_PUT(r,27,4,1); r=M62A_U64_PUT(r,31,4,1); r=M62A_U64_PUT(r,35,4,1);
 return r;
}
static int m62a_ds_ok(uint64_t r)
{   /* 高位必须为 0，且 df/sf_f/sf_b/六个 op 都落在 NVN 合法域 1..8（nvn.h:996-1029） */
 if(r>>39)return 0;
 {const unsigned sh[9]={2,7,11,15,19,23,27,31,35};
  for(int i=0;i<9;i++){unsigned v=(unsigned)((r>>sh[i])&0xfull); if(v<1||v>8)return 0;}}
 return 1;
}
static uint64_t m62a_ds_load(const void *s)
{
 uint64_t r;
 if(!s||(uintptr_t)s<0x10000ull||(uintptr_t)s>=0x800000000000ull)return m62a_ds_def();
 memcpy(&r,s,8);
 return m62a_ds_ok(r)?r:m62a_ds_def();
}
static void m62a_ds_set_defaults(void *s)
{ uint64_t r=m62a_ds_def(); if(!m62a_on()||!s)return; m62a_guest_store(s,r,8); m62a_probe(0,r); }
static void m62a_ds_set_dt(void *s,int e)
{ uint64_t r; if(!m62a_on()||!s)return; r=M62A_U64_PUT(m62a_ds_load(s),0,1,e?1:0); m62a_guest_store(s,r,8); }
static void m62a_ds_set_dw(void *s,int e)
{ uint64_t r; if(!m62a_on()||!s)return; r=M62A_U64_PUT(m62a_ds_load(s),1,1,e?1:0); m62a_guest_store(s,r,8); }
static void m62a_ds_set_df(void *s,int f)
{ uint64_t r; if(!m62a_on()||!s)return; r=M62A_U64_PUT(m62a_ds_load(s),2,4,(uint64_t)f); m62a_guest_store(s,r,8); }
static void m62a_ds_set_st(void *s,int e)
{ uint64_t r; if(!m62a_on()||!s)return; r=M62A_U64_PUT(m62a_ds_load(s),6,1,e?1:0); m62a_guest_store(s,r,8); }
static void m62a_ds_set_sfunc(void *s,int face,int fn)
{ uint64_t r; if(!m62a_on()||!s)return; r=m62a_ds_load(s);
  if(face&1)r=M62A_U64_PUT(r,7,4,(uint64_t)fn);
  if(face&2)r=M62A_U64_PUT(r,11,4,(uint64_t)fn);
  m62a_guest_store(s,r,8); }
static void m62a_ds_set_sop(void *s,int face,int fail,int zfail,int zpass)
{ uint64_t r; if(!m62a_on()||!s)return; r=m62a_ds_load(s);
  if(face&1){r=M62A_U64_PUT(r,15,4,(uint64_t)fail);r=M62A_U64_PUT(r,19,4,(uint64_t)zfail);r=M62A_U64_PUT(r,23,4,(uint64_t)zpass);}
  if(face&2){r=M62A_U64_PUT(r,27,4,(uint64_t)fail);r=M62A_U64_PUT(r,31,4,(uint64_t)zfail);r=M62A_U64_PUT(r,35,4,(uint64_t)zpass);}
  m62a_guest_store(s,r,8); }
/* ---- polygon（nvn_obj.h:88）---- */
static uint32_t m62a_poly_def(void)
{   /* cull=NONE(0) front=CCW(1) mode=FILL(2) offset_en=0（donor hle_nvn.c:1175） */
 uint32_t r=0;
 r=M62A_U32_PUT(r,0,2,0); r=M62A_U32_PUT(r,2,1,1); r=M62A_U32_PUT(r,3,2,2); r=M62A_U32_PUT(r,5,3,0);
 return r;
}
static uint32_t m62a_poly_load(const void *s)
{
 uint32_t r;
 if(!s||(uintptr_t)s<0x10000ull||(uintptr_t)s>=0x800000000000ull)return m62a_poly_def();
 memcpy(&r,s,4);
 return (r>>8)?m62a_poly_def():r;   /* bit8..31 有值 ⇒ 未写/指针 */
}
static void m62a_poly_set_defaults(void *s)
{ uint32_t r=m62a_poly_def(); if(!m62a_on()||!s)return; m62a_guest_store(s,r,4); m62a_probe(1,r); }
static void m62a_poly_set_cull(void *s,int f)
{ uint32_t r; if(!m62a_on()||!s)return; r=M62A_U32_PUT(m62a_poly_load(s),0,2,(uint32_t)f); m62a_guest_store(s,r,4); }
static void m62a_poly_set_front(void *s,int f)
{ uint32_t r; if(!m62a_on()||!s)return; r=M62A_U32_PUT(m62a_poly_load(s),2,1,(uint32_t)f); m62a_guest_store(s,r,4); }
static void m62a_poly_set_mode(void *s,int m)
{ uint32_t r; if(!m62a_on()||!s)return; r=M62A_U32_PUT(m62a_poly_load(s),3,2,(uint32_t)m); m62a_guest_store(s,r,4); }
static void m62a_poly_set_offset(void *s,int e)
{ uint32_t r; if(!m62a_on()||!s)return; r=M62A_U32_PUT(m62a_poly_load(s),5,3,(uint32_t)e); m62a_guest_store(s,r,4); }
/* ---- channel-mask（nvn_obj.h:83）---- */
static void m62a_chmask_set_defaults(void *s)
{ uint32_t r=0xffffffffu; if(!m62a_on()||!s)return; m62a_guest_store(s,r,4); m62a_probe(2,r); }
static void m62a_chmask_set(void *s,int i,uint32_t rr,uint32_t gg,uint32_t bb,uint32_t aa)
{ uint32_t v,m;
  if(!m62a_on()||!s||i<0||i>7)return;
  memcpy(&v,s,4);
  m=((rr?1u:0)|(gg?2u:0)|(bb?4u:0)|(aa?8u:0));
  v=(v&~(0xfu<<(4*i)))|(m<<(4*i));
  m62a_guest_store(s,v,4); }
/* ---- color logicOp（nvn_obj.h:82）----
   缺省＝donor nvnColorStateSetDefaults（hle_nvn.c:1155）：blend_en=0 logic_op=COPY(3) alpha_func=ALWAYS(8)。
   m14b_color_set_defaults 写的是 0（没填 logic_op/alpha_func），所以 v==0 也按未写处理 ⇒ 先落缺省。 */
static void m62a_color_set_logicop(void *s,int op)
{ uint32_t v;
  if(!m62a_on()||!s)return;
  memcpy(&v,s,4);
  if(!v||(v>>16))v=M62A_U32_PUT(M62A_U32_PUT(0u,8,4,3u),12,4,8u);
  v=M62A_U32_PUT(v,8,4,(uint32_t)op);
  m62a_guest_store(s,v,4); m62a_probe(3,v); }

static void *nvn_lookup(const char *name){
 /* M45A / S2b-2（执行路径切换）：smo_m45a=1 ⇒ 名字在 nx2 的 537 张表里就返回它的 ELF 跳板（裸指针，包装由两个
    调用点照旧做），于是 NVN 调用走 nx2 hle_nvn 录制 → mtl_exec → gfx_vk_exec 回放；返回 NULL 的名字继续走本表旧路。
    关态（缺省 0）m45a_thunk_for() 第一行就返回 NULL ⇒ 逐字节原行为。 */
 { extern void *m45a_thunk_for(const char *);
   void *mt = m45a_thunk_for(name);
   if (mt) return mt; }
 static const struct{const char *name;void *fn;}entries[]={   /* 按 [nvn] 日志逐轮补 */
  {"nvnDeviceGetProcAddress",nvn_get_proc_address},
  {"nvnDeviceInitialize",nvn_device_initialize},
  {"nvnDeviceBuilderSetDefaults",nvn_device_builder_set_defaults},
  {"nvnDeviceBuilderSetFlags",nvn_device_builder_set_flags},
  {"nvnDeviceGetInteger",nvn_device_get_integer},
  {"nvnDeviceFinalize",nvn_device_finalize},
  {"nvnMemoryPoolBuilderSetDefaults",nvn_pool_builder_set_defaults},
  {"nvnMemoryPoolBuilderSetDevice",nvn_pool_builder_set_device},
  {"nvnMemoryPoolBuilderSetStorage",nvn_pool_builder_set_storage},
  {"nvnMemoryPoolBuilderSetFlags",nvn_pool_builder_set_flags},
  {"nvnMemoryPoolInitialize",nvn_pool_initialize},
  {"nvnMemoryPoolMap",nvn_pool_map},
  {"nvnMemoryPoolMapVirtual",nvn_pool_map_virtual},
  {"nvnMemoryPoolGetBufferAddress",nvn_pool_get_buffer_address},
  {"nvnMemoryPoolGetSize",nvn_pool_get_size},
  {"nvnMemoryPoolGetFlags",nvn_pool_get_flags},
  {"nvnMemoryPoolFinalize",nvn_pool_finalize},
  {"nvnBufferBuilderSetStorage",nvn_buffer_builder_set_storage},
  {"nvnBufferInitialize",nvn_buffer_initialize},
  {"nvnBufferMap",nvn_buffer_map},
  {"nvnBufferGetAddress",nvn_buffer_get_address},
  {"nvnQueueBuilderSetDefaults",nvn_queue_builder_set_defaults},
  {"nvnQueueBuilderSetDevice",nvn_queue_builder_set_device},
  {"nvnQueueBuilderSetFlags",nvn_queue_builder_set_flags},
  {"nvnQueueBuilderSetCommandMemorySize",nvn_queue_builder_set_command_memory_size},
  {"nvnQueueBuilderSetComputeMemorySize",nvn_queue_builder_set_compute_memory_size},
  {"nvnQueueBuilderSetControlMemorySize",nvn_queue_builder_set_control_memory_size},
  {"nvnQueueBuilderSetCommandFlushThreshold",nvn_queue_builder_set_command_flush_threshold},
  {"nvnQueueBuilderSetQueueMemory",nvn_queue_builder_set_queue_memory},
  {"nvnQueueBuilderGetQueueMemorySize",nvn_queue_builder_get_queue_memory_size},
  {"nvnMemoryPoolBuilderGetSize",nvn_pool_builder_get_size},
  {"nvnMemoryPoolBuilderGetMemory",nvn_pool_builder_get_memory},
  {"nvnMemoryPoolBuilderGetFlags",nvn_pool_builder_get_flags},
  {"nvnBufferBuilderGetSize",nvn_buffer_builder_get_size},
  {"nvnBufferBuilderGetMemoryPool",nvn_buffer_builder_get_memory_pool},
  {"nvnBufferBuilderGetMemoryOffset",nvn_buffer_builder_get_memory_offset},
  {"nvnBufferGetSize",nvn_buffer_get_size},
  {"nvnBufferGetMemoryPool",nvn_buffer_get_memory_pool},
  {"nvnBufferGetMemoryOffset",nvn_buffer_get_memory_offset},
  {"nvnCommandBufferInitialize",nvn_rec_command_buffer_initialize},
  {"nvnCommandBufferAddCommandMemory",nvn_rec_command_buffer_add_command_memory},
  {"nvnCommandBufferAddControlMemory",nvn_rec_command_buffer_add_control_memory},
  {"nvnCommandBufferGetCommandMemorySize",nvn_rec_get_command_memory_size},
  {"nvnCommandBufferGetControlMemorySize",nvn_rec_get_control_memory_size},
  {"nvnTextureBuilderSetDefaults",nvn_texture_builder_set_defaults},
  {"nvnTextureBuilderSetDevice",nvn_texture_builder_set_device},
  {"nvnTextureBuilderSetFlags",nvn_texture_builder_set_flags},
  {"nvnTextureBuilderSetTarget",nvn_texture_builder_set_target},
  {"nvnTextureBuilderSetWidth",nvn_texture_builder_set_width},
  {"nvnTextureBuilderSetHeight",nvn_texture_builder_set_height},
  {"nvnTextureBuilderSetDepth",nvn_texture_builder_set_depth},
  {"nvnTextureBuilderSetSize1D",nvn_texture_builder_set_size1d},
  {"nvnTextureBuilderSetSize2D",nvn_texture_builder_set_size2d},
  {"nvnTextureBuilderSetSize3D",nvn_texture_builder_set_size3d},
  {"nvnTextureBuilderSetLevels",nvn_texture_builder_set_levels},
  {"nvnTextureBuilderSetFormat",nvn_texture_builder_set_format},
  {"nvnTextureBuilderSetSamples",nvn_texture_builder_set_samples},
  {"nvnTextureBuilderSetStride",nvn_texture_builder_set_stride},
  {"nvnTextureBuilderGetStorageSize",nvn_texture_builder_get_storage_size},
  {"nvnTextureBuilderGetStorageAlignment",nvn_texture_builder_get_storage_alignment},
  {"nvnTextureBuilderGetStorageClass",nvn_texture_builder_get_storage_class},
  {"nvnTextureBuilderSetStorage",nvn_texture_builder_set_storage},
  {"nvnTextureInitialize",nvn_texture_initialize},
  {"nvnTextureGetTextureAddress",nvn_texture_get_texture_address},
  {"nvnTextureGetWidth",nvn_texture_get_width},
  {"nvnTextureGetHeight",nvn_texture_get_height},
  {"nvnTextureGetDepth",nvn_texture_get_depth},
  {"nvnTextureGetLevels",nvn_texture_get_levels},
  {"nvnTextureGetFormat",nvn_texture_get_format},
  {"nvnTextureGetTarget",nvn_texture_get_target},
  {"nvnTextureGetSamples",nvn_texture_get_samples},
  {"nvnTextureGetFlags",nvn_texture_get_flags},
  {"nvnTextureGetSwizzle",nvn_texture_get_swizzle},
  {"nvnTextureGetMemoryPool",nvn_texture_get_memory_pool},
  {"nvnTextureGetMemoryOffset",nvn_texture_get_memory_offset},
  {"nvnTextureGetStorageSize",nvn_texture_get_storage_size},
  {"nvnTextureGetStorageClass",nvn_texture_get_storage_class},
  {"nvnTextureGetRawStorageClass",nvn_texture_get_raw_storage_class},
  {"nvnTextureGetStride",nvn_texture_get_stride},
  {"nvnTextureGetViewOffset",nvn_texture_get_view_offset},
  /* M3y P1：纹理/采样器 helper 补表（54 个 dynstr 请求名，此前落返-0 空桩）。见实现块注释。 */
  {"nvnTextureBuilderSetSwizzle",nvn_texture_builder_set_swizzle},
  {"nvnTextureBuilderSetDepthStencilMode",nvn_texture_builder_set_depth_stencil_mode},
  {"nvnTextureBuilderSetGLTextureName",nvn_texture_builder_set_gl_tex_name},
  {"nvnTextureBuilderSetPackagedTextureData",nvn_texture_builder_set_pkg_data},
  {"nvnTextureBuilderSetPackagedTextureLayout",nvn_texture_builder_set_pkg_layout},
  {"nvnTextureBuilderGetWidth",nvn_texture_builder_get_width},
  {"nvnTextureBuilderGetHeight",nvn_texture_builder_get_height},
  {"nvnTextureBuilderGetDepth",nvn_texture_builder_get_depth},
  {"nvnTextureBuilderGetLevels",nvn_texture_builder_get_levels},
  {"nvnTextureBuilderGetFormat",nvn_texture_builder_get_format},
  {"nvnTextureBuilderGetFlags",nvn_texture_builder_get_flags},
  {"nvnTextureBuilderGetTarget",nvn_texture_builder_get_target},
  {"nvnTextureBuilderGetSamples",nvn_texture_builder_get_samples},
  {"nvnTextureBuilderGetStride",nvn_texture_builder_get_stride},
  {"nvnTextureBuilderGetSwizzle",nvn_texture_builder_get_swizzle},
  {"nvnTextureBuilderGetMemoryPool",nvn_texture_builder_get_memory_pool},
  {"nvnTextureBuilderGetMemoryOffset",nvn_texture_builder_get_memory_offset},
  {"nvnTextureBuilderGetPackagedTextureData",nvn_texture_builder_get_pkg_data},
  {"nvnTextureBuilderGetSparseTileLayout",nvn_texture_builder_get_sparse_tile_layout},
  {"nvnTextureBuilderGetGLTextureName",nvn_texture_builder_get_gl_tex_name},
  {"nvnTextureBuilderGetZCullStorageSize",nvn_texture_builder_get_zcull_storage_size},
  {"nvnTextureBuilderGetDepthStencilMode",nvn_texture_builder_get_depth_stencil_mode},
  {"nvnTextureSetDebugLabel",nvn_texture_set_debug_label},
  {"nvnTextureGetDebugID",nvn_texture_get_debug_id},
  {"nvnTextureGetDepthStencilMode",nvn_texture_get_depth_stencil_mode},
  {"nvnTextureGetSparseTileLayout",nvn_texture_get_sparse_tile_layout},
  {"nvnTextureGetZCullStorageSize",nvn_texture_get_zcull_storage_size},
  {"nvnTextureFlushTexels",nvn_texture_flush_texels},
  {"nvnTextureInvalidateTexels",nvn_texture_invalidate_texels},
  {"nvnSamplerBuilderSetDefaults",nvn_sampler_builder_set_defaults},
  {"nvnSamplerBuilderSetDevice",nvn_sampler_builder_set_device},
  {"nvnSamplerBuilderSetMinMagFilter",nvn_sampler_builder_set_min_mag_filter},
  {"nvnSamplerBuilderSetWrapMode",nvn_sampler_builder_set_wrap_mode},
  {"nvnSamplerBuilderSetLodClamp",nvn_sampler_builder_set_lod_clamp},
  {"nvnSamplerBuilderSetLodBias",nvn_sampler_builder_set_lod_bias},
  {"nvnSamplerBuilderSetCompare",nvn_sampler_builder_set_compare},
  {"nvnSamplerBuilderSetBorderColor",nvn_sampler_builder_set_border_color},
  {"nvnSamplerBuilderSetMaxAnisotropy",nvn_sampler_builder_set_max_anisotropy},
  {"nvnSamplerBuilderSetLodSnap",nvn_sampler_builder_set_lod_snap},
  {"nvnSamplerBuilderSetReductionFilter",nvn_sampler_builder_set_reduction_filter},
  {"nvnSamplerInitialize",nvn_sampler_initialize},
  {"nvnSamplerFinalize",nvn_sampler_finalize},
  /* M3y P1：采样器查询/整型边界色/RegisterSamplerBuilder 补表。 */
  {"nvnSamplerBuilderSetBorderColori",nvn_sampler_builder_set_border_colori},
  {"nvnSamplerBuilderSetBorderColorui",nvn_sampler_builder_set_border_colorui},
  {"nvnSamplerBuilderGetMinMagFilter",nvn_sampler_builder_get_min_mag_filter},
  {"nvnSamplerBuilderGetWrapMode",nvn_sampler_builder_get_wrap_mode},
  {"nvnSamplerBuilderGetLodClamp",nvn_sampler_builder_get_lod_clamp},
  {"nvnSamplerBuilderGetLodBias",nvn_sampler_builder_get_lod_bias},
  {"nvnSamplerBuilderGetCompare",nvn_sampler_builder_get_compare},
  {"nvnSamplerBuilderGetBorderColor",nvn_sampler_builder_get_border_color},
  {"nvnSamplerBuilderGetBorderColori",nvn_sampler_builder_get_border_colori},
  {"nvnSamplerBuilderGetBorderColorui",nvn_sampler_builder_get_border_colorui},
  {"nvnSamplerBuilderGetMaxAnisotropy",nvn_sampler_builder_get_max_anisotropy},
  {"nvnSamplerBuilderGetReductionFilter",nvn_sampler_builder_get_reduction_filter},
  {"nvnSamplerSetDebugLabel",nvn_sampler_set_debug_label},
  {"nvnSamplerGetDebugID",nvn_sampler_get_debug_id},
  {"nvnSamplerGetMinMagFilter",nvn_sampler_get_min_mag_filter},
  {"nvnSamplerGetWrapMode",nvn_sampler_get_wrap_mode},
  {"nvnSamplerGetLodClamp",nvn_sampler_get_lod_clamp},
  {"nvnSamplerGetLodBias",nvn_sampler_get_lod_bias},
  {"nvnSamplerGetCompare",nvn_sampler_get_compare},
  {"nvnSamplerGetBorderColor",nvn_sampler_get_border_color},
  {"nvnSamplerGetBorderColori",nvn_sampler_get_border_colori},
  {"nvnSamplerGetBorderColorui",nvn_sampler_get_border_colorui},
  {"nvnSamplerGetMaxAnisotropy",nvn_sampler_get_max_anisotropy},
  {"nvnSamplerGetReductionFilter",nvn_sampler_get_reduction_filter},
  {"nvnSamplerPoolRegisterSamplerBuilder",nvn_sampler_pool_register_sampler_builder},
  {"nvnQueueInitialize",nvn_queue_initialize},
  {"nvnQueueFinalize",nvn_queue_finalize},
  {"nvnProgramInitialize",nvn_program_initialize},
  {"nvnProgramFinalize",nvn_program_finalize},
  {"nvnProgramSetDebugLabel",nvn_program_set_debug_label},
  {"nvnProgramSetShaders",nvn_program_set_shaders},
  {"nvnWindowBuilderSetDefaults",nvn_window_builder_set_defaults},
  {"nvnWindowBuilderSetDevice",nvn_window_builder_set_device},
  {"nvnWindowBuilderSetNativeWindow",nvn_window_builder_set_native_window},
  {"nvnWindowBuilderSetTextures",nvn_window_builder_set_textures},
  {"nvnWindowBuilderSetNumActiveTextures",nvn_window_builder_set_num_active_textures},
  {"nvnWindowBuilderGetNumActiveTextures",nvn_window_builder_get_num_active_textures},
  {"nvnWindowBuilderSetPresentInterval",nvn_window_builder_set_present_interval},
  {"nvnWindowInitialize",nvn_window_initialize},
  {"nvnWindowSetNumActiveTextures",nvn_window_set_num_active_textures},
  {"nvnWindowSetCrop",nvn_window_set_crop},   /* 终局 F-A：装库 kick/notify 链终端（crash_1031 终局三段） */
  {"nvnWindowGetCrop",nvn_window_get_crop},
  {"nvnDeviceGetTextureHandle",nvn_device_get_texture_handle},   /* 终局 F-D：句柄族（crash_1031 终局四段） */
  {"nvnDeviceGetTexelFetchHandle",nvn_device_get_texel_fetch_handle},
  {"nvnDeviceGetImageHandle",nvn_device_get_image_handle},
  {"nvnDeviceGetSeparateTextureHandle",nvn_device_get_separate_texture_handle},
  {"nvnDeviceGetSeparateSamplerHandle",nvn_device_get_separate_sampler_handle},
  {"nvnWindowSetPresentInterval",nvn_window_set_present_interval},   /* F-B 同族存取补齐（防下一个空桩） */
  {"nvnWindowGetPresentInterval",nvn_window_get_present_interval},
  {"nvnWindowGetNativeWindow",nvn_window_get_native_window},
  {"nvnWindowFinalize",nvn_window_finalize},
  {"nvnWindowGetNumActiveTextures",nvn_window_get_num_active_textures},
  {"nvnTexturePoolGetSize",nvn_texture_pool_get_size},
  {"nvnTexturePoolInitialize",nvn_texture_pool_initialize},
  {"nvnTexturePoolFinalize",nvn_texture_pool_finalize},
  {"nvnTexturePoolRegisterTexture",nvn_texture_pool_register_texture},
  {"nvnTexturePoolRegisterImage",nvn_texture_pool_register_image},
  {"nvnTexturePoolSetDebugLabel",nvn_texture_pool_set_debug_label},
  {"nvnTexturePoolGetMemoryPool",nvn_texture_pool_get_memory_pool},
  {"nvnTexturePoolGetMemoryOffset",nvn_texture_pool_get_memory_offset},
  {"nvnSamplerPoolGetSize",nvn_sampler_pool_get_size},
  {"nvnSamplerPoolInitialize",nvn_sampler_pool_initialize},
  {"nvnSamplerPoolFinalize",nvn_sampler_pool_finalize},
  {"nvnSamplerPoolRegisterSampler",nvn_sampler_pool_register_sampler},
  {"nvnSamplerPoolSetDebugLabel",nvn_sampler_pool_set_debug_label},
  {"nvnSamplerPoolGetMemoryPool",nvn_sampler_pool_get_memory_pool},
  {"nvnSamplerPoolGetMemoryOffset",nvn_sampler_pool_get_memory_offset},
  {"nvnSyncInitialize",nvn_sync_initialize},   /* 终局 F2：提交/栅栏族"提交即退休"（crash_1031 终局章） */
  {"nvnSyncFinalize",nvn_sync_finalize},
  {"nvnSyncWait",nvn_sync_wait},
  {"nvnQueueWaitSync",nvn_queue_wait_sync},
  {"nvnQueueFenceSync",nvn_queue_fence_sync},
  {"nvnCommandBufferFenceSync",nvn_command_buffer_fence_sync},
  {"nvnQueueFlush",nvn_queue_flush},
  {"nvnQueueFinish",nvn_queue_finish},
  {"nvnQueueSubmitCommands",nvn_queue_submit_commands},
  {"nvnCommandBufferEndRecording",nvn_cb_end_recording_wired},
  {"nvnQueueAcquireTexture",nvn_queue_acquire_texture},
  {"nvnWindowAcquireTexture",nvn_window_acquire_texture},
  {"nvnQueuePresentTexture",nvn_queue_present_texture},
 {"nvnBufferFlushMappedRange",nvn_buffer_flush_mapped_range},
 {"nvnMemoryPoolFlushMappedRange",nvn_memory_pool_flush_mapped_range},
 {"nvnQueueGetError",nvn_queue_get_error},   /* round48 submit-chain：提交后错误查询（donor :722-724） */
 {"nvnEventBuilderSetDefaults",nvn_event_builder_set_defaults},   /* round48：Event 族=提交完成 CPU 可见原语（donor :685-695） */
 {"nvnEventBuilderSetStorage",nvn_event_builder_set_storage},
 {"nvnEventInitialize",nvn_event_initialize},
 {"nvnEventFinalize",nvn_event_finalize},
 {"nvnEventGetValue",nvn_event_get_value},
 {"nvnEventSignal",nvn_event_signal},
 {"nvnBufferFinalize",nvn_buffer_finalize},   /* round48：批次组成件（donor :285） */
 {"nvnTextureFinalize",nvn_texture_finalize},   /* round48（donor :443；此前返-0 桩 ×7/轮） */
  /* 终局五段 P0/P1：命令内存记账（RSTB 断链主修）+ 录制族迁 entries + Compare/Origin/Texels/Timestamp */
  {"nvnCommandBufferBeginRecording",nvn_cb_begin_recording_wired},   /* M5p B：重录=命令内存重置，回执/映射在此弃（原挂 Finalize，实测把提交前的回执全删了） */
  {"nvnCommandBufferIsRecording",nvn_rec_is_recording},
  {"nvnCommandBufferFinalize",nvn_cb_finalize_wired},
  {"nvnCommandBufferGetCommandMemoryUsed",nvn_rec_get_command_memory_used},
  {"nvnCommandBufferGetCommandMemoryFree",nvn_rec_get_command_memory_free},
  {"nvnCommandBufferGetControlMemoryUsed",nvn_rec_get_control_memory_used},
  {"nvnCommandBufferGetControlMemoryFree",nvn_rec_get_control_memory_free},
  {"nvnCommandBufferGetMemoryCallback",nvn_command_buffer_get_memory_callback},
  {"nvnCommandBufferGetMemoryCallbackData",nvn_command_buffer_get_memory_callback_data},
  {"nvnCommandBufferSetMemoryCallback",nvn_command_buffer_set_memory_callback},
  {"nvnCommandBufferSetMemoryCallbackData",nvn_command_buffer_set_memory_callback_data},
  {"nvnCommandBufferSetCommandMemoryCallbackEnabled",nvn_command_buffer_set_command_memory_callback_enabled},
  {"nvnCommandBufferSetDebugLabel",nvn_command_buffer_set_debug_label},
  {"nvnCommandBufferGetCopyRowStride",nvn_command_buffer_get_copy_row_stride},
  {"nvnCommandBufferSetCopyRowStride",nvn_command_buffer_set_copy_row_stride},
  {"nvnCommandBufferGetCopyImageStride",nvn_command_buffer_get_copy_image_stride},
  {"nvnCommandBufferSetCopyImageStride",nvn_command_buffer_set_copy_image_stride},
  {"nvnQueueGetTotalCommandMemoryUsed",nvn_queue_get_total_command_memory_used},
  {"nvnQueueGetTotalComputeMemoryUsed",nvn_queue_get_total_compute_memory_used},
  {"nvnQueueGetTotalControlMemoryUsed",nvn_queue_get_total_control_memory_used},
  {"nvnQueueResetMemoryUsageCounts",nvn_queue_reset_memory_usage_counts},
  {"nvnCommandBufferDrawElements",nvn_rec_draw_elements},
  /* M4ad P2②：这两条此前指向 cb_account 记账桩（连命令流都不进 ⇒ 零证据）。
     实测它们合计 35k 次/boot，是已记录 draw 的 2.2 倍，几何最可能藏在这里。 */
  {"nvnCommandBufferDrawElementsBaseVertex",nvn_rec_draw_elements_base_vertex},
  {"nvnCommandBufferDrawElementsInstanced",nvn_rec_draw_elements_instanced},
  {"nvnCommandBufferDrawElementsIndirect",nvn_command_buffer_draw_elements_indirect},
  {"nvnCommandBufferDrawArrays",nvn_rec_draw_arrays},
  {"nvnCommandBufferDrawArraysInstanced",nvn_cb_rec_draw_arrays_instanced},   /* M5p C：原指 cb_account 记账桩（从未进命令流） */
  {"nvnCommandBufferDrawArraysIndirect",nvn_command_buffer_draw_arrays_indirect},
  {"nvnCommandBufferBindVertexAttribState",nvn_cb_rec_bind_vertex_attrib_state},
  {"nvnCommandBufferBindVertexStreamState",nvn_cb_rec_bind_vertex_stream_state},
  /* M3s（2026-11-03）：SDK 顶点状态 helper——缺了它们，游戏构状态表的 Set/Get 全是空桩，
     BIND_VERTEX_ATTRIB/STREAM 收到的永远是同一块零数组（证据见本轮 [v] attrib_words/vertex totals）。 */
  {"nvnVertexAttribStateSetDefaults",nvn_va_set_defaults},
  {"nvnVertexAttribStateSetFormat",nvn_va_set_format},
  {"nvnVertexAttribStateSetStreamIndex",nvn_va_set_stream},
  {"nvnVertexAttribStateGetFormat",nvn_va_get_format},
  {"nvnVertexAttribStateGetStreamIndex",nvn_va_get_stream},
  {"nvnVertexStreamStateSetDefaults",nvn_vs_set_defaults},
  {"nvnVertexStreamStateSetStride",nvn_vs_set_stride},
  {"nvnVertexStreamStateSetDivisor",nvn_vs_set_divisor},
  {"nvnVertexStreamStateGetStride",nvn_vs_get_stride},
  {"nvnVertexStreamStateGetDivisor",nvn_vs_get_divisor},
  {"nvnCommandBufferBindVertexBuffer",nvn_cb_rec_bind_vertex_buffer},
  {"nvnCommandBufferBindVertexBuffers",nvn_rec_bind_vertex_buffers},
  {"nvnCommandBufferBindUniformBuffer",nvn_cb_rec_bind_uniform_buffer},
  {"nvnCommandBufferBindUniformBuffers",nvn_rec_bind_uniform_buffers},
  {"nvnCommandBufferBindStorageBuffer",nvn_rec_bind_storage_buffer},     /* M5x：进命令流（gmem 地址） */
  {"nvnCommandBufferBindStorageBuffers",nvn_rec_bind_storage_buffers},   /* M5x */
  {"nvnCommandBufferBindTexture",nvn_cb_rec_bind_texture},
  {"nvnCommandBufferBindTextures",nvn_rec_bind_textures},
  {"nvnCommandBufferBindImage",nvn_command_buffer_bind_image},
  {"nvnCommandBufferBindImages",nvn_command_buffer_bind_images},
  {"nvnCommandBufferBindSeparateTexture",nvn_command_buffer_bind_separate_texture},
  {"nvnCommandBufferBindSeparateTextures",nvn_command_buffer_bind_separate_textures},
  {"nvnCommandBufferBindSeparateSampler",nvn_command_buffer_bind_separate_sampler},
  {"nvnCommandBufferBindSeparateSamplers",nvn_command_buffer_bind_separate_samplers},
  {"nvnCommandBufferUpdateUniformBuffer",nvn_command_buffer_update_uniform_buffer},
  {"nvnCommandBufferClearBuffer",nvn_command_buffer_clear_buffer},
  {"nvnCommandBufferBarrier",nvn_command_buffer_barrier},
  {"nvnCommandBufferSetViewports",nvn_rec_set_viewports},
  {"nvnCommandBufferBeginTransformFeedback",nvn_command_buffer_begin_transform_feedback},
  {"nvnCommandBufferEndTransformFeedback",nvn_command_buffer_end_transform_feedback},
  {"nvnCommandBufferPauseTransformFeedback",nvn_command_buffer_pause_transform_feedback},
  {"nvnCommandBufferResumeTransformFeedback",nvn_command_buffer_resume_transform_feedback},
  {"nvnCommandBufferBindBlendState",nvn_cb_rec_bind_blend_state},
  {"nvnCommandBufferBindChannelMaskState",nvn_command_buffer_bind_channel_mask_state},
  {"nvnCommandBufferBindColorState",nvn_cb_rec_bind_color_state},
  /* M14B：状态对象 setter 的真实现（此前落返-0 空桩 ⇒ 客体状态对象从未被填）。smo_m14b=0 时是空操作。 */
  {"nvnBlendStateSetDefaults",m14b_blend_set_defaults},
  {"nvnBlendStateSetBlendTarget",m14b_blend_set_target},
  {"nvnBlendStateSetBlendFunc",m14b_blend_set_func},
  {"nvnBlendStateSetBlendEquation",m14b_blend_set_equation},
  {"nvnColorStateSetDefaults",m14b_color_set_defaults},
  {"nvnColorStateSetBlendEnable",m14b_color_set_blend_enable},
  /* M62A（2026-11-11）：四族 setter 补齐（此前 0 条 ⇒ 返-0 空桩 ⇒ 客体状态对象从未被写，M60A 物证见 :6442 前注释）。
     15 条 = depth-stencil 7 + polygon 5 + channel-mask 2 + logicOp 1；smo_m62a=0 时逐条空操作。 */
  {"nvnDepthStencilStateSetDefaults",m62a_ds_set_defaults},
  {"nvnDepthStencilStateSetDepthTestEnable",m62a_ds_set_dt},
  {"nvnDepthStencilStateSetDepthWriteEnable",m62a_ds_set_dw},
  {"nvnDepthStencilStateSetDepthFunc",m62a_ds_set_df},
  {"nvnDepthStencilStateSetStencilTestEnable",m62a_ds_set_st},
  {"nvnDepthStencilStateSetStencilFunc",m62a_ds_set_sfunc},
  {"nvnDepthStencilStateSetStencilOp",m62a_ds_set_sop},
  {"nvnPolygonStateSetDefaults",m62a_poly_set_defaults},
  {"nvnPolygonStateSetCullFace",m62a_poly_set_cull},
  {"nvnPolygonStateSetFrontFace",m62a_poly_set_front},
  {"nvnPolygonStateSetPolygonMode",m62a_poly_set_mode},
  {"nvnPolygonStateSetPolygonOffsetEnables",m62a_poly_set_offset},
  {"nvnChannelMaskStateSetDefaults",m62a_chmask_set_defaults},
  {"nvnChannelMaskStateSetChannelMask",m62a_chmask_set},
  {"nvnColorStateSetLogicOp",m62a_color_set_logicop},
  {"nvnCommandBufferBindDepthStencilState",nvn_cb_rec_bind_depth_stencil_state},
  {"nvnCommandBufferBindMultisampleState",nvn_command_buffer_bind_multisample_state},
  {"nvnCommandBufferBindPolygonState",nvn_command_buffer_bind_polygon_state},
  {"nvnCommandBufferBindProgram",nvn_rec_bind_program},
  {"nvnCommandBufferBindCoverageModulationTable",nvn_command_buffer_bind_coverage_modulation_table},
  {"nvnCommandBufferBindTransformFeedbackBuffer",nvn_command_buffer_bind_transform_feedback_buffer},
  {"nvnCommandBufferBindTransformFeedbackBuffers",nvn_command_buffer_bind_transform_feedback_buffers},
  {"nvnCommandBufferCallCommands",nvn_command_buffer_call_commands},
  {"nvnCommandBufferCopyCommands",nvn_command_buffer_copy_commands},
  {"nvnCommandBufferClearColor",nvn_rec_clear_color},
  {"nvnCommandBufferClearColori",nvn_command_buffer_clear_colori},
  {"nvnCommandBufferClearColorui",nvn_command_buffer_clear_colorui},
  {"nvnCommandBufferClearDepthStencil",nvn_command_buffer_clear_depth_stencil},
  {"nvnCommandBufferClearTexture",nvn_command_buffer_clear_texture},
  {"nvnCommandBufferClearTexturei",nvn_command_buffer_clear_texturei},
  {"nvnCommandBufferClearTextureui",nvn_command_buffer_clear_textureui},
  {"nvnCommandBufferCopyBufferToBuffer",nvn_command_buffer_copy_buffer_to_buffer},
  {"nvnCommandBufferCopyBufferToTexture",nvn_command_buffer_copy_buffer_to_texture},
  {"nvnCommandBufferCopyTextureToBuffer",nvn_command_buffer_copy_texture_to_buffer},
  {"nvnCommandBufferCopyTextureToTexture",nvn_command_buffer_copy_texture_to_texture},
  {"nvnCommandBufferDiscardColor",nvn_command_buffer_discard_color},
  {"nvnCommandBufferDiscardDepthStencil",nvn_command_buffer_discard_depth_stencil},
  {"nvnCommandBufferDispatchCompute",nvn_command_buffer_dispatch_compute},
  {"nvnCommandBufferDispatchComputeIndirect",nvn_command_buffer_dispatch_compute_indirect},
  {"nvnCommandBufferDownsample",nvn_command_buffer_downsample},
  {"nvnCommandBufferDownsampleTextureView",nvn_command_buffer_downsample_texture_view},
  {"nvnCommandBufferTiledDownsample",nvn_command_buffer_tiled_downsample},
  {"nvnCommandBufferTiledDownsampleTextureView",nvn_command_buffer_tiled_downsample_texture_view},
  {"nvnCommandBufferDrawTransformFeedback",nvn_command_buffer_draw_transform_feedback},
  {"nvnCommandBufferDrawTexture",nvn_command_buffer_draw_texture},
  {"nvnCommandBufferMultiDrawArraysIndirectCount",nvn_command_buffer_multi_draw_arrays_indirect_count},
  {"nvnCommandBufferMultiDrawElementsIndirectCount",nvn_command_buffer_multi_draw_elements_indirect_count},
  {"nvnCommandBufferInsertCheckpointDynamic",nvn_command_buffer_insert_checkpoint_dynamic},
  {"nvnCommandBufferInsertCheckpointStatic",nvn_command_buffer_insert_checkpoint_static},
  {"nvnCommandBufferInsertDebugMarker",nvn_command_buffer_insert_debug_marker},
  {"nvnCommandBufferInsertDebugMarkerDynamic",nvn_command_buffer_insert_debug_marker_dynamic},
  {"nvnCommandBufferInsertDebugMarkerStatic",nvn_command_buffer_insert_debug_marker_static},
  {"nvnCommandBufferPushDebugGroup",nvn_command_buffer_push_debug_group},
  {"nvnCommandBufferPushDebugGroupDynamic",nvn_command_buffer_push_debug_group_dynamic},
  {"nvnCommandBufferPushDebugGroupStatic",nvn_command_buffer_push_debug_group_static},
  {"nvnCommandBufferPopDebugGroup",nvn_command_buffer_pop_debug_group},
  {"nvnCommandBufferPopDebugGroupId",nvn_command_buffer_pop_debug_group_id},
  {"nvnCommandBufferReportCounter",nvn_command_buffer_report_counter},
  {"nvnCommandBufferReportValue",nvn_command_buffer_report_value},
  {"nvnCommandBufferResetCounter",nvn_command_buffer_reset_counter},
  {"nvnCommandBufferResolveDepthBuffer",nvn_command_buffer_resolve_depth_buffer},
  {"nvnCommandBufferRestoreZCullData",nvn_command_buffer_restore_zcull_data},
  {"nvnCommandBufferSaveZCullData",nvn_command_buffer_save_zcull_data},
  {"nvnCommandBufferSetAlphaRef",nvn_command_buffer_set_alpha_ref},
  {"nvnCommandBufferSetBlendColor",nvn_command_buffer_set_blend_color},
  {"nvnCommandBufferSetColorReductionEnable",nvn_command_buffer_set_color_reduction_enable},
  {"nvnCommandBufferSetColorReductionThresholds",nvn_command_buffer_set_color_reduction_thresholds},
  {"nvnCommandBufferSetConservativeRasterDilate",nvn_command_buffer_set_conservative_raster_dilate},
  {"nvnCommandBufferSetConservativeRasterEnable",nvn_command_buffer_set_conservative_raster_enable},
  {"nvnCommandBufferSetDepthBounds",nvn_command_buffer_set_depth_bounds},
  {"nvnCommandBufferSetDepthClamp",nvn_command_buffer_set_depth_clamp},
  {"nvnCommandBufferSetDepthRange",nvn_command_buffer_set_depth_range},
  {"nvnCommandBufferSetDepthRanges",nvn_command_buffer_set_depth_ranges},
  {"nvnCommandBufferSetInnerTessellationLevels",nvn_command_buffer_set_inner_tessellation_levels},
  {"nvnCommandBufferSetLineWidth",nvn_command_buffer_set_line_width},
  {"nvnCommandBufferSetOuterTessellationLevels",nvn_command_buffer_set_outer_tessellation_levels},
  {"nvnCommandBufferSetPatchSize",nvn_command_buffer_set_patch_size},
  {"nvnCommandBufferSetPointSize",nvn_command_buffer_set_point_size},
  {"nvnCommandBufferSetPolygonOffsetClamp",nvn_command_buffer_set_polygon_offset_clamp},
  {"nvnCommandBufferSetPrimitiveRestart",nvn_command_buffer_set_primitive_restart},
  {"nvnCommandBufferSetProgramSubroutines",nvn_command_buffer_set_program_subroutines},
  {"nvnCommandBufferSetRasterizerDiscard",nvn_command_buffer_set_rasterizer_discard},
  {"nvnCommandBufferSetRenderEnable",nvn_command_buffer_set_render_enable},
  {"nvnCommandBufferSetRenderEnableConditional",nvn_command_buffer_set_render_enable_conditional},
  {"nvnCommandBufferSetRenderTargets",nvn_rec_set_render_targets},
  {"nvnCommandBufferSetSampleMask",nvn_command_buffer_set_sample_mask},
  {"nvnCommandBufferSetSamplerPool",nvn_command_buffer_set_sampler_pool},
  {"nvnCommandBufferSetScissor",nvn_command_buffer_set_scissor},
  {"nvnCommandBufferSetScissors",nvn_rec_set_scissors},
  {"nvnCommandBufferSetShaderScratchMemory",nvn_command_buffer_set_shader_scratch_memory},
  {"nvnCommandBufferSetStencilCullCriteria",nvn_command_buffer_set_stencil_cull_criteria},
  {"nvnCommandBufferSetStencilMask",nvn_command_buffer_set_stencil_mask},
  {"nvnCommandBufferSetStencilRef",nvn_command_buffer_set_stencil_ref},
  {"nvnCommandBufferSetStencilValueMask",nvn_command_buffer_set_stencil_value_mask},
  {"nvnCommandBufferSetSubpixelPrecisionBias",nvn_command_buffer_set_subpixel_precision_bias},
  {"nvnCommandBufferSetTexturePool",nvn_command_buffer_set_texture_pool},
  {"nvnCommandBufferSetTiledCacheAction",nvn_command_buffer_set_tiled_cache_action},
  {"nvnCommandBufferSetTiledCacheTileSize",nvn_command_buffer_set_tiled_cache_tile_size},
  {"nvnCommandBufferSetViewport",nvn_command_buffer_set_viewport},
  {"nvnCommandBufferSetViewportSwizzles",nvn_command_buffer_set_viewport_swizzles},
  {"nvnCommandBufferSignalEvent",nvn_command_buffer_signal_event},
  {"nvnCommandBufferWaitEvent",nvn_command_buffer_wait_event},
  {"nvnTextureCompare",nvn_texture_compare},
  {"nvnTextureViewCompare",nvn_texture_view_compare},
  {"nvnTextureViewSetDefaults",nvn_texture_view_set_defaults},   /* 终局 F-G：View 14 件成对（crash_1031 终局七段） */
  {"nvnTextureViewSetLevels",nvn_texture_view_set_levels},
  {"nvnTextureViewSetLayers",nvn_texture_view_set_layers},
  {"nvnTextureViewSetFormat",nvn_texture_view_set_format},
  {"nvnTextureViewSetSwizzle",nvn_texture_view_set_swizzle},
  {"nvnTextureViewSetDepthStencilMode",nvn_texture_view_set_depth_stencil_mode},
  {"nvnTextureViewSetTarget",nvn_texture_view_set_target},
  {"nvnTextureViewGetLevels",nvn_texture_view_get_levels},
  {"nvnTextureViewGetLayers",nvn_texture_view_get_layers},
  {"nvnTextureViewGetFormat",nvn_texture_view_get_format},
  {"nvnTextureViewGetSwizzle",nvn_texture_view_get_swizzle},
  {"nvnTextureViewGetDepthStencilMode",nvn_texture_view_get_depth_stencil_mode},
  {"nvnTextureViewGetTarget",nvn_texture_view_get_target},
  {"nvnSamplerCompare",nvn_sampler_compare},
  {"nvnDeviceSetWindowOriginMode",nvn_device_set_window_origin_mode},
  {"nvnDeviceGetWindowOriginMode",nvn_device_get_window_origin_mode},
  {"nvnDeviceSetDepthMode",nvn_device_set_depth_mode},
  {"nvnDeviceGetDepthMode",nvn_device_get_depth_mode},
  {"nvnDeviceGetTimestampInNanoseconds",nvn_device_get_timestamp_in_nanoseconds},
  {"nvnDeviceGetCurrentTimestampInNanoseconds",nvn_device_get_current_timestamp_in_nanoseconds},
  {"nvnTextureWriteTexels",nvn_texture_write_texels},
  {"nvnTextureWriteTexelsStrided",nvn_texture_write_texels_strided},
  {"nvnTextureReadTexels",nvn_texture_read_texels},
  {"nvnTextureReadTexelsStrided",nvn_texture_read_texels_strided},
 };
 for(size_t i=0;i<sizeof entries/sizeof entries[0];i++)if(entries[i].name&&!strcmp(name,entries[i].name))return entries[i].fn;
 return NULL;
}
static void *nvn_bootstrap(const char *name){
 const char *n=name?name:"";void *fn=nvn_lookup(n);
 fprintf(stderr,"[nvn] %s -> %s\n",n,fn?"host":"unimplemented stub");
 return runtime_hle_wrap_named(fn?fn:nvn_named_stub(n),n);   /* 命中走包装：同 fn 同指针，与 GetProcAddress 给出的一致；表外返-0 桩 round20-A 起按名分槽并走宿主栈（原裸桩在客体栈上跑） */
}
__attribute__((used,section("__DATA,totkgfx"))) static const GfxEntry init_entry={"_ZN2nv18InitializeGraphicsEPvm",graphics_initialize,"native Metal context"};
__attribute__((used,section("__DATA,totkgfx"))) static const GfxEntry nvn_entry={"nvnBootstrapLoader",nvn_bootstrap,"host nvn table"};
__attribute__((used,section("__DATA,totkgfx"))) static const GfxEntry alloc_entry={"_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_",graphics_allocator,"game allocator callbacks"};
__attribute__((used,section("__DATA,totkgfx"))) static const GfxEntry dev_alloc_entry={"_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_",graphics_dev_allocator,"game diagnostic allocator callbacks"};
/* round11：线程栈地址空间分配器宿主替换。sdk ThreadManager::CreateThread（sdk+0x329af0）循环调
   AddressSpaceAllocator::AllocateSpace(size=[info+0x58],align=0x1000)，cbz x0 → guest Break（本轮停点）。
   loader.c:229-236 kind 1026 重定位按名查本表换宿主实现；读 this+8/+0x10/+0x18（start/end/guard，单位页），
   首次适应自建静态区间表选址（分配含两侧 guard 一并记账；装不下 size_p+2*guard 返 0，同 SDK 失败情形①）。
   round12 实测定单位：region/guard=页，入参 size/align=字节（16KB 线程栈 / 0x1000 对齐；round11 版混用
   页字节导致每次预留 ~64MB 三次烧光 256MB 区、且返回页号被当字节地址）——入参先 (x+0xfff)>>12 归一到页
   再算，返回值照 SDK（sdk+0x32c194-1a0 lsl #12）换算回字节地址，日志两单位都打防再犯。
   副作用：SDK 自身区间树不再有表项、其 free 路径变空操作——boot 期可接受，后续再回收。
   （GetStackGuardEndAddress 不加透传版：override 换的是 GOT 槽、拿不到 SDK 原函数指针，瞎返值会污染
   分配器边界；其唯一目的"验证栈区上界"由 region=[start,end) 日志直接给出。round12 实测 end=0x6f01fbf 页
   超宿主真实栈上界 0x6f01c3b000 约 3.5MB：仅顶部分配会被 services.c:134 拒，自底首次适应碰不到，暂不钳。） */
typedef struct{uint64_t lo,hi;} as_range_h;
static as_range_h as_used_h[4096];static unsigned as_used_h_n;
static uint64_t os_as_allocate_space(void *self,uint64_t size,uint64_t align){
 uint64_t start=*(const uint64_t *)((const char *)self+8),end=*(const uint64_t *)((const char *)self+0x10),guard=*(const uint64_t *)((const char *)self+0x18),cand=start+guard,ret=0;
 uint64_t size_p=(size+0xfff)>>12,align_p=(align+0xfff)>>12;   /* 入参字节→页（round12 实测），region/guard 本就是页 */
 if(!align_p)align_p=1;
 if(size_p+2*guard<=end-start)   /* 同 SDK：边界装不下 size_p+2*guard（页）直接失败（guard=4 页） */
  for(unsigned step=0;step<=as_used_h_n+1;step++){
   cand=(cand+align_p-1)/align_p*align_p;
   if(cand+size_p+guard>end)break;
   const as_range_h *hit=0;
   for(unsigned i=0;i<as_used_h_n;i++)if(!(as_used_h[i].hi<=cand||as_used_h[i].lo>=cand+size_p)){hit=&as_used_h[i];break;}
   if(!hit){if(as_used_h_n>=4096)break;as_used_h[as_used_h_n++]=(as_range_h){cand-guard,cand+size_p+guard};ret=cand<<12;break;}   /* 预留含两侧 guard；返回字节地址（SDK lsl#12 原样） */
   cand=hit->hi;
  }
 fprintf(stderr,"[os] AllocateSpace region=[%llx,%llx) guard=%#llx size=%#llx align=%#llx -> %llx used=%u | pages size_p=%llu align_p=%llu ret=%#llx\n",(unsigned long long)start,(unsigned long long)end,(unsigned long long)guard,(unsigned long long)size,(unsigned long long)align,(unsigned long long)ret,as_used_h_n,(unsigned long long)size_p,(unsigned long long)align_p,(unsigned long long)ret);
 return ret;
}
__attribute__((used,section("__DATA,totkgfx"))) static const GfxEntry as_alloc_entry={"_ZN2nn2os6detail21AddressSpaceAllocator13AllocateSpaceEmm",os_as_allocate_space,"thread-stack address space allocator (round11 probe)"};

/* M5z build marker v2 (forces one real recompile; content-bearing, not a flag). */
