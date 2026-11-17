/* Bind the donor graphics interface to this native runtime, without donor game hooks. */
#include "runtime.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../third_party/d2r_gfx/gfx/nvn/nvn_mtl.h"
#include "../third_party/d2r_gfx/gfx/nvn/nvn_tex.h"   /* nvn_texdesc/nvn_texlayout + nvn_tex_layout（host/nvn_tex.c，round15 拷自 donor） */
typedef struct{const char *name;void *fn;const char *why;} GfxEntry;
uint64_t g_t0_ns;

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
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <sys/ucontext.h>
static uintptr_t watch_lo,watch_hi,watch_addr,watch_end;static long watch_from=300;static int watch_hits,watch_init,watch_read;
static unsigned char watch_snap[32];static int watch_snap_valid;static _Atomic int watch_armed;   /* TOTK: content poll at every present + periodic re-arm (an unrelated write in the page unprotects it; without re-arm the block's own write in the same frame is missed) */
static void *watch_rearm_thread(void *arg){(void)arg;for(;;){usleep(100);if(watch_lo&&atomic_load(&watch_armed))mprotect((void *)watch_lo,watch_hi-watch_lo,watch_read?PROT_NONE:PROT_READ);}return NULL;}
int runtime_module_offset(uint64_t pc,const char **name,uint64_t *off);
static void watch_hex(char *b,size_t *n,const char *label,uint64_t v){while(*label)b[(*n)++]=*label++;b[(*n)++]='0';b[(*n)++]='x';for(int i=15;i>=0;i--)b[(*n)++]="0123456789abcdef"[(v>>(i*4))&15];}
static void watch_sym(char *b,size_t *n,const char *label,uint64_t pc){const char *nm;uint64_t off;while(*label)b[(*n)++]=*label++;if(runtime_module_offset(pc,&nm,&off)){while(*nm)b[(*n)++]=*nm++;watch_hex(b,n,"+",off);}else watch_hex(b,n,"host:",pc);}
/* TOTK: watch a guest range [addr, addr+len); TOTK_WATCH_READ=1 also catches reads (pages become inaccessible after each present).
   Set from the environment (TOTK_WATCH_ADDR) or at runtime by the graphics layer (DM_NVN_WATCH_SB: a storage buffer bound to a given shader). */
int totk_vpool_backing(uintptr_t va,uint64_t len,uintptr_t *cpu);
void runtime_watch_set(uintptr_t addr,size_t len){
 { uintptr_t cpu=0; if(totk_vpool_backing(addr,len?len:256,&cpu)&&cpu){char b[160];size_t n=0;watch_hex(b,&n,"[watch] virtual pool address ",addr);watch_hex(b,&n," -> CPU backing ",cpu);b[n++]=10;write(2,b,n);addr=cpu;} }
 watch_addr=addr;watch_end=addr+(len?len:256);watch_lo=addr&~(uintptr_t)0x3fff;watch_hi=(watch_end+0x3fff)&~(uintptr_t)0x3fff;watch_hits=0;
 char b[160];size_t n=0;watch_hex(b,&n,"[watch] range set addr=",addr);watch_hex(b,&n," end=",watch_end);watch_hex(b,&n," pages=",(watch_hi-watch_lo)>>14);b[n++]=10;write(2,b,n);
}
void runtime_watch_tick(uint64_t frame){
 if(!watch_init){watch_init=1;const char *e=getenv("TOTK_WATCH_ADDR");if(e){uintptr_t a=(uintptr_t)strtoull(e,NULL,16);const char *l=getenv("TOTK_WATCH_LEN");runtime_watch_set(a,l?(size_t)strtoull(l,NULL,0):256);}const char *f=getenv("TOTK_WATCH_FRAME");if(f)watch_from=atol(f);watch_read=getenv("TOTK_WATCH_READ")!=NULL;}
 if(watch_lo&&(long)frame>=watch_from){ unsigned char cur[32];memcpy(cur,(const void *)watch_addr,32);
  if(!watch_snap_valid||memcmp(cur,watch_snap,32)){char b[256];size_t n=0;watch_hex(b,&n,"[watch] content at present frame=",frame);for(int i=0;i<32;i+=4){uint32_t w;memcpy(&w,cur+i,4);b[n++]=' ';for(int k=7;k>=0;k--)b[n++]="0123456789abcdef"[(w>>(k*4))&15];}b[n++]=10;write(2,b,n);memcpy(watch_snap,cur,32);watch_snap_valid=1;} }
 if(!watch_lo||(long)frame<watch_from||watch_hits>=200)return;
 { static int started;if(!started){started=1;pthread_t t;pthread_create(&t,NULL,watch_rearm_thread,NULL);} atomic_store(&watch_armed,1); }
 if(mprotect((void *)watch_lo,watch_hi-watch_lo,watch_read?PROT_NONE:PROT_READ)!=0){char b[96];size_t n=0;watch_hex(b,&n,"[watch] mprotect failed page=",watch_lo);b[n++]=10;write(2,b,n);}
}
static int watch_fault(void *addr,int is_write,void *context){
 uintptr_t a=(uintptr_t)addr;if(!watch_lo||a<watch_lo||a>=watch_hi)return 0;
 if(a<watch_addr||a>=watch_end){mprotect((void *)watch_lo,watch_hi-watch_lo,PROT_READ|PROT_WRITE);return 1;}   /* other data in the pages: silently unprotect (re-armed at the next present) */
 ucontext_t *u=context;char b[1024];size_t n=0;
 { extern uint64_t g_frame_pub; extern _Atomic int dm_submit_busy; uint64_t th=0; pthread_threadid_np(NULL,&th);
   watch_hex(b,&n,"[watch] F",g_frame_pub);watch_hex(b,&n," t_us=",clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000);watch_hex(b,&n," th=",th);watch_hex(b,&n," submit_busy=",(uint64_t)atomic_load(&dm_submit_busy)); }
 watch_hex(b,&n,is_write?" write hit addr=":" read hit addr=",a);watch_hex(b,&n," off=",a-watch_addr);
#ifdef __MUSL__   /* OHOS: musl aarch64 mcontext 为值类型，regs[31]/sp/pc */
 watch_sym(b,&n," pc=",u->uc_mcontext.pc);watch_sym(b,&n," lr=",u->uc_mcontext.regs[30]);
 uint64_t fp=u->uc_mcontext.regs[29];
#else             /* macOS: Apple arm64 mcontext 指针风格 __ss */
 watch_sym(b,&n," pc=",u->uc_mcontext->__ss.__pc);watch_sym(b,&n," lr=",u->uc_mcontext->__ss.__lr);
 uint64_t fp=u->uc_mcontext->__ss.__fp;
#endif
for(int d=0;d<8&&fp&&!(fp&15);d++){uint64_t fr[2];mach_vm_size_t got=0;if(mach_vm_read_overwrite(mach_task_self(),fp,16,(uintptr_t)fr,&got)!=KERN_SUCCESS||got!=16)break;watch_sym(b,&n," <",fr[1]);fp=fr[0];}
 b[n++]='\n';write(2,b,n);watch_hits++;
 mprotect((void *)watch_lo,watch_hi-watch_lo,PROT_READ|PROT_WRITE);return 1;
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
/* donor hle_nvn.c g_devinfo 的 Odyssey 全表（SMO 1.3.0 实测值，Mac 上验证过）。
   历史：曾只答 0/1 两项（GLSLC 四元组 0x2E-0x31 被撤，当时语境下撤除是对的）；终局二段定谳
   全 0 把异步装载池短路成零容量（TexturePoolRegister×412 后绝对静默、enqueue 静默丢 work），
   且 H1 页隔离+F2 提交族已修、验尸链齐备，恢复全表。每个 what 首查打点，崩了即回滚单点。 */
static const int nvn_devinfo_odyssey[]={
 [0x00]=53,[0x01]=311,[0x02]=14,[0x03]=65536,[0x04]=256,[0x05]=8,[0x06]=16,[0x07]=4,[0x08]=16,[0x09]=32,
 [0x0A]=16,[0x0B]=4,[0x0C]=4,[0x0D]=4,[0x0E]=16,[0x0F]=32,[0x10]=32,[0x11]=256,[0x12]=256,
 [0x13]=4,[0x14]=8,[0x15]=512,[0x16]=256,[0x17]=2,[0x18]=256,[0x19]=4096,[0x1A]=131072,
 [0x1B]=16,[0x1C]=1536,[0x1D]=1024,[0x1E]=64,[0x1F]=1536,[0x20]=65535,[0x21]=65535,[0x22]=65535,
 [0x23]=8,[0x24]=1048576,[0x25]=4096,[0x26]=16,[0x27]=4096,[0x28]=1,[0x29]=1,[0x2A]=1,
 [0x2B]=262144,[0x2C]=15,[0x2D]=2048,[0x2E]=1,[0x2F]=1,[0x30]=14,[0x31]=5,[0x32]=1,[0x33]=8,
 [0x34]=8,[0x35]=4,[0x36]=32,[0x37]=128,[0x38]=32,[0x39]=128,[0x3A]=65536,[0x3B]=1,[0x3C]=4,
 [0x3D]=16384,[0x3E]=134217728,[0x3F]=2048,[0x40]=16384,[0x41]=16384,[0x42]=1,[0x43]=1,[0x44]=1,
 [0x45]=1,[0x46]=5,[0x47]=1,[0x48]=1,[0x49]=1,[0x4A]=1,[0x4B]=1,[0x4C]=1,[0x4D]=16,
 [0x4E]=1024,[0x4F]=32,[0x50]=4096,[0x51]=65536,[0x52]=65536,[0x53]=256,[0x54]=16384,
 [0x55]=262144,[0x56]=4096,[0x57]=1,[0x58]=4,[0x59]=2,[0x5A]=0,[0x5B]=4096,[0x5C]=16384,
 [0x5D]=4096,[0x5E]=128,[0x5F]=4096};
static void nvn_device_get_integer(const void *device,int what,int *out){(void)device;   /* main+0x9414c4 cmp #0x35、+0x9414d0 cmp #0x137（minor≥311） */
 unsigned w=(unsigned)what;
 static uint64_t seen[2];   /* 每个 what 首查打点（0x00-0x7F bitmap） */
 if(w<0x80){if(!((seen[w>>6]>>(w&63))&1)){seen[w>>6]|=1ULL<<(w&63);fprintf(stderr,"[nvn] GetInteger %#x -> %d\n",w,w<(unsigned)(sizeof nvn_devinfo_odyssey/sizeof*nvn_devinfo_odyssey)?nvn_devinfo_odyssey[w]:0);} }
 *out=w<(unsigned)(sizeof nvn_devinfo_odyssey/sizeof*nvn_devinfo_odyssey)?nvn_devinfo_odyssey[w]:0;
}
static void nvn_device_finalize(void *device){(void)device;}   /* 槽 e30；donor hle_nvn.c:146 清 magic，此处无对象账本，空操作 */
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
static void *nvn_pool_map(const void *p){const pool_h *o=p;fprintf(stderr,"[nvn] map pool=%p magic=%#x mem=%p size=%#llx\n",p,o->magic,o->mem,(unsigned long long)o->size);return o->mem;}   /* round14 插桩：magic≠0x504f4f4c 即 foreign 对象（从未过宿主 Initialize）；行为不变仍无条件返 mem——有意不学 donor hle_nvn.c:262-264 对 CPU_NO_ACCESS 返 NULL：round13 双证游戏拿 Map 结果当写基址（main+0x9a3f38/0x9a3f60），宿主无 vpool 层返 NULL 必崩 */
typedef struct{const void *physical_pool;int64_t physical_offset,virtual_offset;uint64_t size;int32_t storage_class;uint32_t padding;} nvn_map_req_h;   /* =totk_vpool.h:7-15 NVNmappingRequest，0x28 实测布局 */
static uint64_t nvn_pool_map_virtual(void *p,int n,const void *req){
 pool_h *v=p;const nvn_map_req_h *r=req;
 if(r)fprintf(stderr,"[nvn] map-virtual pool=%p n=%d phys=%p poff=%#llx voff=%#llx size=%#llx class=%d\n",p,n,r->physical_pool,(unsigned long long)r->physical_offset,(unsigned long long)r->virtual_offset,(unsigned long long)r->size,r->storage_class);   /* round14 插桩：dump 请求定位 mystery 池 */
 if(!v||v->magic!=0x504f4f4c||!(v->flags&0x200)||!v->mem||n<0||(n&&!r))return 0;   /* donor totk_vpool.c:107-108 同款校验（0x200=VIRTUAL；count=0 合法空操作） */
 if(!n)return 1;
 {const pool_h *pp=r->physical_pool;   /* donor-lite：不做 mach_vm_remap 页别名（totk_vpool.c:85-97），直接把虚拟池 mem 指到物理后备 p->mem+physical_offset，游戏随后 Map 虚拟池即拿 CPU 地址；boot 版不重映射/不登记 mtl，差异注此 */
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
static uint64_t nvn_command_buffer_initialize(void *c,void *d){cmd_h *o=c;memset(o,0,sizeof(cmd_h));o->magic=0x434d4442;o->dev=d;o->inline_cur=~(uint64_t)0;o->inline_end=0;return 1;}   /* 'CMDB'，donor :773-775 */
static void nvn_command_buffer_add_command_memory(void *c,const void *p,int64_t off,uint64_t n){cmd_h *o=c;const pool_h *pp=p;o->cmd_base=(uintptr_t)(pp?pp->mem:NULL)+(uintptr_t)off;o->game_size=n;o->cmd_used=0;o->hw_used=0;}   /* donor AddCommandMemory（accounting only） */
static void nvn_command_buffer_add_control_memory(void *c,void *m,uint64_t n){cmd_h *o=c;o->ctl=m;o->ctl_size=n;o->ctl_used=0;}   /* donor :840-844 */
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
static uint64_t nvn_texture_builder_get_storage_size(const void *b){nvn_texlayout l;nvn_tex_layout(&((const tex_builder_h *)b)->d,&l);if(!l.bpb)fprintf(stderr,"[nvn] TextureBuilderGetStorageSize unknown format %#x\n",((const tex_builder_h *)b)->d.format);return l.size;}   /* donor :382-391 走 nvn_tex_layout（简化掉 SIZELOG/预排布日志） */
static uint64_t nvn_texture_builder_get_storage_alignment(const void *b){   /* round22：donor :393-394 原式走 nvn_tex_layout（host/nvn_tex.c 与 donor 逐字节同）——块线性 512、pitch 32、LINEAR_RENDER_TARGET 0x40 128、BUFFER=bpb、未知格式 512；donor 有公式故照抄、不取 0x1000 假值：池管理器 0x97fb38-0x97fb54 align-up 链吃这颗实值，round21 stub#59 返 0 是 -1MB 放置错位头嫌 */
 nvn_texlayout l;nvn_tex_layout(&((const tex_builder_h *)b)->d,&l);
 {static unsigned n;if(n<8){n++;fprintf(stderr,"[nvn] TextureBuilderGetStorageAlignment tgt=%d fmt=%#x fl=%#x -> %#x (donor nvn_tex_layout: 512 blocklinear / 32|128 pitch / bpb buffer)\n",((const tex_builder_h *)b)->d.target,((const tex_builder_h *)b)->d.format,((const tex_builder_h *)b)->d.flags,l.align);}}
 return l.align;}
static int nvn_texture_builder_get_storage_class(const void *b){   /* round22：donor :372-375 totk_storage_class——NVNstorageClass 是后端不透明兼容标识（nvn.h:44 typedef int，头无枚举值、非 Tegra memory-kind），donor 约 BUFFER(0xA, nvn.h:787)→0、其余（池放置主体 2D）→1，照抄 */
 int c=((const tex_builder_h *)b)->d.target==0xA?0:1;
 {static unsigned n;if(n<8){n++;fprintf(stderr,"[nvn] TextureBuilderGetStorageClass tgt=%d -> %d (donor totk_storage_class: BUFFER=0, others=1)\n",((const tex_builder_h *)b)->d.target,c);}}
 return c;}
/* round19-A：纹理池存储链。此前 SetStorage/Initialize/GetTextureAddress 全是返-0 桩（round18.log:189/227/245），
   纹理 GPU VA=0，池管理器偏移 obj[8]−obj[0x20][0x10]（main+0x97ec90）= 0−1MB 对齐池基址 = -1MB，
   写循环 str w26,[x9,x8,lsl#2]（main+0x9a3efc）x9=池CPU基址+w21 越界 SIGSEGV。现按 donor hle_nvn.c:358/399-408
   存 pool/off 并以与 :145 nvn_buffer_get_address 相同的 CPU 回声约定取址（GPU 地址=池 mem+off，两差同源后
   偏移自然归位）。donor 无 GetTextureAddress（grep 空），自建。tex_h=donor tex_t 共享前缀去 serial
   （nvn_obj.h:35-39），只写游戏 NVNtexture 分配内（FITS(tex_t,NVNtexture)），本表内自洽读写。 */
typedef struct{uint32_t magic;nvn_texdesc d;uint8_t has_layout,layout[8];nvn_texlayout L;void *mem;void *pool;int64_t off;} tex_h;   /* donor tex_t 前缀 */
static void nvn_texture_builder_set_storage(void *b,void *pool,int64_t off){((tex_builder_h *)b)->pool=pool;((tex_builder_h *)b)->off=off;}   /* donor :358 */
static uint64_t nvn_texture_initialize(void *t,const void *b){
 const tex_builder_h *s=b;tex_h *o=t;memset(o,0,sizeof *o);o->magic=0x54455800;o->d=s->d;o->pool=s->pool;o->off=s->off;o->has_layout=s->has_layout;memcpy(o->layout,s->layout,8);   /* 'TEX\0'，donor :405 */
 {const pool_h *p=s->pool;o->mem=p&&p->mem?(char *)p->mem+(ptrdiff_t)o->off:0;}   /* donor :408：CPU 回声 */
 fprintf(stderr,"[nvn] Texture %p init pool=%p off=%#llx -> mem=%p\n",t,s->pool,(unsigned long long)s->off,o->mem);
 return 1;
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
static int nvn_texture_get_storage_class(const void *t){return ((const tex_h *)t)->d.target==0xA?0:1;}   /* round22：donor :450 对象级 totk_storage_class 同式（nvn_protos.h:185）——补齐同族只读查询，免再落返-0 桩 */
static int nvn_texture_get_raw_storage_class(const void *t){(void)t;return 1;}   /* round22：donor :462 NOPROTO（头无原型）——调用点 main+0x174899c 与 1 比较不等则置状态位 0x100，donor 恒返 1；stub 返 0 会误置位 */
static int64_t nvn_texture_get_stride(const void *t){const nvn_texdesc *d=&((const tex_h *)t)->d;return nvn_tex_is_linear(d)?(int64_t)nvn_tex_linear_stride(d):0;}   /* nvn_protos.h:197：pitch 存储才有行距（LINEAR 0x10/LINEAR_RENDER_TARGET 0x40，nvn_tex.h:27-33），块线性无单一 stride 返 0 */
static int64_t nvn_texture_get_view_offset(const void *t,const void *view){   /* nvn_protos.h:186：视图级偏移=级偏移+层×每层字节 */
 const nvn_texdesc *d=&((const tex_h *)t)->d;
 if(!view)return 0;
 { const unsigned char *w=(const unsigned char *)view;unsigned base_level=w[1],min_layer=(unsigned)w[4]|((unsigned)w[5]<<8);   /* donor view_t（nvn_obj.h:41-45）：bitfield@0、base_level@1、nlevels@2、target@3、min_layer@4(u16) */
   nvn_texlayout l;nvn_tex_layout(d,&l);
   return (int64_t)(nvn_tex_level_offset(d,base_level)+(uint64_t)min_layer*l.layer_size); }
}
/* round21-C：Sampler/Queue/Window 接库——round20 stub 名单证实游戏已在调这三个 Initialize（返 0=失败，
   Queue 对象是后续 submit 侧一切的前提），各 Builder setter 同步。布局照 donor nvn_obj.h:51-67，语义照
   donor hle_nvn.c:517-546（Sampler）/720-726（Queue）/2358-2381（Window），简化掉 mtl/life 层。
   枚举默认值（nvn.h:890/898/905/925）：min=NEAREST_MIPMAP_LINEAR 0x4、mag=LINEAR 0x1、wrap=REPEAT 0x1、cmp=LESS 0x2。 */
typedef struct{uint8_t minf,magf,ws,wt,wr,cmp_mode,cmp_func,reduction;float lod_min,lod_max,lod_bias,aniso,border[4],lod_snap;} samp_par_h;   /* =samp_par（nvn_obj.h:51，44B 断言 :54） */
typedef struct{void *dev;samp_par_h p;} samp_builder_h;   /* =samp_builder_t（:52） */
typedef struct{uint32_t magic;samp_par_h p;} samp_h;      /* =samp_t 前缀（:53） */
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
static uint64_t g_nvn_fence_gen,g_nvn_handle_gen,g_nvn_presents;
uint64_t dm_nvn_frame_count(void){return g_nvn_presents;}   /* freeze_watchdog.c 弱符号的强定义：游戏自认渲染帧数=出图计数（donor :2469/2550 同位递增）；本宿主无显示链，QueuePresentTexture 处计数 */
typedef struct{uint32_t magic,fenced;uint64_t fence;} sync_h;   /* =donor sync_t（hle_nvn.c:611 注释族），'SYNC'=0x434e5953 */
static uint64_t nvn_sync_initialize(void *s,void *d){(void)d;*(sync_h *)s=(sync_h){0x434e5953,0,0};{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] SyncInitialize %p (retire-immediate host)\n",s);}return 1;}   /* donor :609 盖 magic 返 1；此前空桩返 0=Initialize 失败 */
static void nvn_sync_finalize(void *s){((sync_h *)s)->magic=0;}   /* donor :610 */
static void nvn_queue_fence_sync(void *q,void *s,int cond,int flags){(void)q;sync_h *o=s;o->fenced=1;o->fence=++g_nvn_fence_gen;{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] QueueFenceSync sync=%p fence=%llu cond=%d flags=%d -> retired now\n",s,(unsigned long long)o->fence,cond,flags);}}   /* donor :656；无 GPU 即刻退休，等价真机"该提交点已过" */
static void nvn_command_buffer_fence_sync(void *c,void *s,int cond,int flags){(void)c;sync_h *o=s;o->fenced=1;o->fence=g_nvn_fence_gen+1;{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] CommandBufferFenceSync sync=%p fence=%llu cond=%d flags=%d -> next submit, retired now\n",s,(unsigned long long)o->fence,cond,flags);}}   /* donor :661：代表下一次提交（fence_now+1），宿主侧同样即刻成立 */
static int nvn_sync_wait(const void *s,uint64_t timeout){const sync_h *o=s;if(!o->fenced)return 2;{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] SyncWait sync=%p fence=%llu timeout=%llu -> ALREADY_SIGNALED (host retires instantly)\n",s,(unsigned long long)o->fence,(unsigned long long)timeout);}return 0;}   /* donor :670：未围栏按真机等到超时(2)，已围栏宿主恒 ALREADY_SIGNALED(0) */
static uint64_t nvn_queue_wait_sync(void *q,const void *s){(void)q;(void)s;{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] QueueWaitSync -> 1 (waited work already done)\n");}return 1;}   /* donor :654：等待的活已在提交序前跑完 */
static void nvn_queue_flush(void *q){(void)q;{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] QueueFlush %p -> immediate (nothing in flight)\n",q);}}   /* donor :728 空实现；提交即退休 */
static void nvn_queue_finish(void *q){(void)q;{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] QueueFinish %p -> immediate (nothing in flight)\n",q);}}   /* donor :729 排空回放；本宿主无在飞工作 */
static void nvn_queue_submit_commands(void *q,int n,const void *h){(void)q;(void)h;{static unsigned nlogged;if(nlogged++<1)fprintf(stderr,"[nvn] QueueSubmitCommands %p n=%d -> retired immediately (no replay layer)\n",q,n);}}   /* donor :2245 入回放队列；本宿主命令只记账不回放，退休无副作用 */
static uint64_t nvn_command_buffer_end_recording(void *c){(void)c;uint64_t h=++g_nvn_handle_gen;{static unsigned n;if(n++<1)fprintf(stderr,"[nvn] CommandBufferEndRecording -> handle %#llx (opaque token)\n",(unsigned long long)h);}return h;}   /* donor :1061 返句柄；此前空桩返 0。BOTW 注释：句柄是控制内存位置/不透明令牌——本表 SubmitCommands 不消费，仅需非零可辨 */
static void nvn_memory_pool_flush_mapped_range(const void *p,int64_t off,uint64_t n){(void)p;(void)off;(void)n;{static unsigned l;if(l++<1)fprintf(stderr,"[nvn] MemoryPoolFlushMappedRange off=%lld n=%llu -> CPU-coherent, no-op\n",(long long)off,(unsigned long long)n);}}   /* donor :258 空实现：CPU 侧写入本就已就绪 */
static void nvn_buffer_flush_mapped_range(const void *b,int64_t off,uint64_t size){const buf_h *v=b;
 if(off<0||(uint64_t)off>v->size||size>v->size-(uint64_t)off){fprintf(stderr,"[nvn][RED] BufferFlushMappedRange OOB off=%lld size=%llu buflen=%llu\n",(long long)off,(unsigned long long)size,(unsigned long long)v->size);return;}   /* donor :297 越界 die→本表风格红日志 */
 dm_guest_write_prepare((uint8_t *)nvn_buffer_map(b)+(uintptr_t)off,(size_t)size);   /* donor 同行：喂 DM 写观察点 */
 {static unsigned n;if(n++<1)fprintf(stderr,"[nvn] BufferFlushMappedRange off=%lld size=%llu -> CPU-coherent\n",(long long)off,(unsigned long long)size);}}
static int nvn_window_acquire_texture(void *w,void *s,int *idx){win_h *o=w;int i=o->active?o->next++%o->active:0;if(idx)*idx=i;
 if(s){sync_h *y=s;y->fenced=3;y->fence=0;}   /* donor :2505：acquire sync"显示不再读该张"=立即成立（fenced bit1=acquire 类标记） */
 {static unsigned n;if(n<4){n++;fprintf(stderr,"[nvn] WindowAcquireTexture -> idx=%d active=%d sync=%p\n",i,o->active,s);} }return 0;}   /* donor :2471：next%active 轮转、SUCCESS=0；无显示链不等待 */
static int nvn_queue_acquire_texture(void *q,void *w,int *idx){(void)q;int r=nvn_window_acquire_texture(w,0,idx);{static unsigned n;if(n<4){n++;fprintf(stderr,"[nvn] QueueAcquireTexture -> idx=%d ret=%d\n",idx?*idx:-1,r);} }return r;}   /* donor :2513（GAME_ODYSSEY 同款）转发窗口获取；此前空桩返 0 但不写 *idx——游戏读未初始化下标 */
static void nvn_queue_present_texture(void *q,void *w,int idx){(void)q;(void)w;g_nvn_presents++;{static unsigned n;if(n<4){n++;fprintf(stderr,"[nvn] QueuePresentTexture idx=%d present #%llu (no display; counted for watchdog)\n",idx,(unsigned long long)g_nvn_presents);} }}   /* donor :2909 出图计帧（g_frame++）；本宿主无显示链只记账 */
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
static void nvn_program_set_debug_label(void *p,const char *l){(void)p;(void)l;}   /* donor 记名空操作 */
static uint64_t nvn_program_set_shaders(void *p,int n,const void *sd){
 if(n<0||n>6){fprintf(stderr,"[nvn] ProgramSetShaders stages=%d【红】\n",n);return 0;}   /* donor :581 同款校验 */
 ((prog_h *)p)->nstage=n;   /* boot 期只记账：shader 本体由后续 Vulkan 后端接线（donor :582-586 拷 sd+control_intern，无 replay 层省略） */
 {static unsigned k;if(k<8){k++;const uint64_t *s=sd;fprintf(stderr,"[nvn] ProgramSetShaders %p n=%d data0=%#llx\n",p,n,n?(unsigned long long)s[0]:0ull);}}   /* 首个 data=GPU code 地址，定位用一次性日志 */
 return 1;
}
/* round16：描述符池（TexturePool/SamplerPool）全家——donor hle_nvn.c:301-324 简化（无 mtl/life 层）。
   GetSize(n) 是计数→字节的静态尺寸查询（管理器建池前问它，round15 mark 证实此类查询在池存在前发生）：
   boot 走宽裕超配 n*0x100+0x1000 取整页——真公式未证（donor/官方头均无），后续 Vulkan 接线时校准；
   超配只费内存，欠配才会溢出。同名符号只能有一个：对象级 GetSize 不再另设回声版。 */
typedef struct{const void *pool;int64_t off;int32_t n;} dpool_h;   /* =nvn_obj.h:27 dpool_t */
static uint64_t nvn_dpool_size_bytes(uint64_t n){return (n*0x100+0x1000+0xfff)&~0xfffULL;}
static uint64_t nvn_texture_pool_get_size(uint64_t n){fprintf(stderr,"[nvn] TexturePoolGetSize n=%llu -> %#llx (oversize)\n",(unsigned long long)n,(unsigned long long)nvn_dpool_size_bytes(n));return nvn_dpool_size_bytes(n);}
static uint64_t nvn_sampler_pool_get_size(uint64_t n){fprintf(stderr,"[nvn] SamplerPoolGetSize n=%llu -> %#llx (oversize)\n",(unsigned long long)n,(unsigned long long)nvn_dpool_size_bytes(n));return nvn_dpool_size_bytes(n);}
static uint64_t nvn_texture_pool_initialize(void *tp,const void *p,int64_t off,int n){dpool_h *o=tp;o->pool=p;o->off=off;o->n=n;fprintf(stderr,"[nvn] TexturePool %p pool=%p off=%#llx n=%d\n",tp,p,(unsigned long long)off,n);return 1;}   /* donor :301-303（无 mtl_set_tex_pool_n） */
static void nvn_texture_pool_finalize(void *tp){(void)tp;}   /* donor :304 */
static void nvn_texture_pool_register_texture(const void *tp,int id,const void *t,const void *v){(void)tp;(void)t;(void)v;fprintf(stderr,"[nvn] TexturePoolRegister id=%d\n",id);}   /* donor :306-313 记名空操作（无 mtl/life 层） */
static void nvn_texture_pool_register_image(const void *tp,int id,const void *t,const void *v){nvn_texture_pool_register_texture(tp,id,t,v);}   /* donor :317-319 转发 RegisterTexture */
static void *nvn_texture_pool_get_memory_pool(const void *tp){return ((dpool_h *)tp)->pool;}
static int64_t nvn_texture_pool_get_memory_offset(const void *tp){return ((const dpool_h *)tp)->off;}
static void nvn_texture_pool_set_debug_label(void *tp,const char *l){(void)tp;(void)l;}
static uint64_t nvn_sampler_pool_initialize(void *sp,const void *p,int64_t off,int n){dpool_h *o=sp;o->pool=p;o->off=off;o->n=n;fprintf(stderr,"[nvn] SamplerPool %p pool=%p off=%#llx n=%d\n",sp,p,(unsigned long long)off,n);return 1;}   /* donor :320-322 */
static void nvn_sampler_pool_finalize(void *sp){(void)sp;}   /* donor :323 */
static void nvn_sampler_pool_register_sampler(const void *sp,int id,const void *s){(void)sp;(void)s;fprintf(stderr,"[nvn] SamplerPoolRegister id=%d\n",id);}   /* donor :324 记名空操作 */
static void *nvn_sampler_pool_get_memory_pool(const void *sp){return ((dpool_h *)sp)->pool;}
static int64_t nvn_sampler_pool_get_memory_offset(const void *sp){return ((const dpool_h *)sp)->off;}
static void nvn_sampler_pool_set_debug_label(void *sp,const char *l){(void)sp;(void)l;}
static void *nvn_lookup(const char *name);   /* fwd：nvn_get_proc_address 要调它 */
static void *nvn_get_proc_address(void *device,const char *name){
 (void)device;const char *n=name?name:"";void *fn=nvn_lookup(n);
 fprintf(stderr,"[nvn] proc %s -> %s\n",n,fn?"host":"stub");
 if(!fn&&strstr(n,"Size"))fprintf(stderr,"[nvn] size-query handed out: %s\n",n);   /* round14-C：尺寸类名字（GetStorageSize/GetSize…）落返-0 桩的 grep 标记——C② 池 Initialize 被尺寸 0 短路的头号嫌疑 */
 return runtime_hle_wrap_named(fn?fn:nvn_named_stub(n),n);   /* round9：游戏填 461 槽后无空检查 blr，表外必须给可调桩、不得返 NULL；同 fn→同 thunk（hle_stack.c:82-94），指针比较一致，stub 也走宿主栈；round20-A 表外桩按名分槽（stub#i name 打点） */
}
static void *nvn_lookup(const char *name){
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
  {"nvnCommandBufferInitialize",nvn_command_buffer_initialize},
  {"nvnCommandBufferAddCommandMemory",nvn_command_buffer_add_command_memory},
  {"nvnCommandBufferAddControlMemory",nvn_command_buffer_add_control_memory},
  {"nvnCommandBufferGetCommandMemorySize",nvn_command_buffer_get_command_memory_size},
  {"nvnCommandBufferGetControlMemorySize",nvn_command_buffer_get_control_memory_size},
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
  {"nvnCommandBufferEndRecording",nvn_command_buffer_end_recording},
  {"nvnQueueAcquireTexture",nvn_queue_acquire_texture},
  {"nvnWindowAcquireTexture",nvn_window_acquire_texture},
  {"nvnQueuePresentTexture",nvn_queue_present_texture},
  {"nvnBufferFlushMappedRange",nvn_buffer_flush_mapped_range},
  {"nvnMemoryPoolFlushMappedRange",nvn_memory_pool_flush_mapped_range},
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
