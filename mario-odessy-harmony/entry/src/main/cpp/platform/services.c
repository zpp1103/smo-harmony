/* Horizon services reached by the current TOTK startup. Unknown operations fail. */
#include "runtime.h"
#include <TargetConditionals.h>
#include "ipc.h"
#include "game_version.h"
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <stdio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <ctype.h>
#include <string.h>
#include <sys/mman.h>

#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>
/* Guest address layout. The SDK encodes IPC buffer addresses with 39 bits (HIPC descriptor bits 36..38),
   so all guest memory must lie below 2^39. Darwin arm64 keeps 0x10_0000_0000..0x70_0000_0000 reserved,
   leaving exactly 64 GiB at 0x70_0000_0000 for the alias region (the real 39-bit Horizon size, which the
   game's allocators depend on); the stack and heap regions are reserved separately below it. */
#define GUEST_ADDRESS_LIMIT (1ULL<<39)
#define ALIAS_SPACE_AT 0x7000000000ULL
#define ALIAS_SIZE_MAX (64ULL<<30)
/* TOTK_ALIAS_GIB: alias region size reported to the game (default 64, the real 39-bit Horizon size). An iOS app signed
   without extended virtual addressing (unavailable to personal teams) gets a much smaller map: the old 6 + 16 GiB
   reservation failed on an iPhone, while 4.75 + 16 GiB fit on an iPad (A16, iPadOS 26.7). So the largest size from 16
   down to 4 GiB that fits is chosen at startup; the game maps about 3.3 GiB of it. */
#if TARGET_OS_IPHONE
#define ALIAS_DEFAULT_GIB 16
#define ALIAS_MIN_GIB 4ULL
#else
#define ALIAS_DEFAULT_GIB 64
#endif
static uint64_t alias_len;
static uint64_t alias_requested(void){const char *e=getenv("TOTK_ALIAS_GIB");uint64_t g=e?strtoull(e,NULL,0):ALIAS_DEFAULT_GIB;if(g<1||g>64)runtime_fail("TOTK_ALIAS_GIB must be 1..64");return g<<30;}
static uint64_t alias_size(void){if(!alias_len)alias_len=alias_requested();return alias_len;}
#define ALIAS_SIZE alias_size()
#define LOW_SEARCH_FROM 0x300000000ULL
#if TARGET_OS_IPHONE
#define LOW_SIZE (HEAP_OFF+HEAP_CAP)   /* iOS: no slack after the heap cap; address space is scarce */
#else
#define LOW_SIZE (6ULL<<30)
#endif
#define STACK_OFF 0x10000000ULL
#define STACK_SIZE 0x10000000ULL
#define HEAP_OFF (STACK_OFF+STACK_SIZE+0x10000000ULL)
static uint8_t *alias_space,*low_space,*heap;
static size_t heap_size;
static unsigned char alias_pages[ALIAS_SIZE_MAX/4096];
/* 4 KiB guest pages that are unmapped but whose host 16 KiB page is still committed with old contents: only these are
   zeroed when mapped again. Every other free page is untouched anonymous memory (zero on first touch), so mapping does
   not make it resident; a fully free 16 KiB host page is handed back to the system (fresh anonymous mapping). On the
   iPhone, where the app may use about 4 GB in total, memory the game maps but never touches must stay unbacked. */
static unsigned char alias_stale[ALIAS_SIZE_MAX/4096];
static uint64_t alias_bytes;
static uint8_t user_exception_context[0x1000] __attribute__((aligned(0x1000)));
#define SYSTEM_RESOURCE_BUDGET (32ULL<<20)
#define HEAP_CAP (4ULL<<30)   /* 1.4.3 sets a 2.2 GiB heap during nninitStartup */
#define APP_POOL app_pool()
static uint64_t app_pool(void){static uint64_t v;if(!v){const char *e=getenv("TOTK_APP_POOL");uint64_t extra=(runtime_output_res_extra_bytes()+runtime_render_pool_extra()+0x1fffff)&~0x1fffffULL;
 v=e?strtoull(e,NULL,0):0xcd500000ULL+extra;if(!e&&extra)fprintf(stderr,"[totk] application pool %#llx: +%llu MiB for the bigger texture heaps (TOTK_OUTPUT_RES, TOTK_RENDER_RES)\n",(unsigned long long)v,(unsigned long long)(extra>>20));}return v;}
/* TOTK: application memory pool reported to the game (default: retail 3.2 GiB; TOTK_OUTPUT_RES adds what its HeapDef overlay gives the texture heaps) */
#define ALIAS_START ((uintptr_t)alias_space)
#define STACK_START ((uintptr_t)low_space+STACK_OFF)
#define ASLR_START ((uintptr_t)low_space)
#if TARGET_OS_IPHONE
#define ASLR_SIZE (ALIAS_START+ALIAS_SIZE-(uintptr_t)low_space)   /* iOS: everything the process can map ends far below 2^39 */
#else
#define ASLR_SIZE (GUEST_ADDRESS_LIMIT-(uintptr_t)low_space)
#endif
static uint8_t *reserve_fixed(uint64_t at,uint64_t size){
 mach_vm_address_t a=at;   /* FIXED without OVERWRITE: fails instead of replacing an existing mapping */
 return mach_vm_map(mach_task_self(),&a,size,0,VM_FLAGS_FIXED,MEMORY_OBJECT_NULL,0,FALSE,VM_PROT_NONE,VM_PROT_ALL,VM_INHERIT_DEFAULT)==KERN_SUCCESS?(uint8_t *)(uintptr_t)a:NULL;
}
#if TARGET_OS_IPHONE
#define GUEST_ALIGN (2ULL<<20)
/* Reserve size bytes in the lowest free gap of this process's map above 4 GiB (2 MiB aligned). The end of the map is
   not reported, so the last gap is only tried; FIXED fails there with KERN_NO_SPACE when it reaches past the end. */
static uint8_t *reserve_in_gap(uint64_t size,int log){
 uint64_t prev=1ULL<<32;
 for(;;){
  mach_vm_address_t r=prev;mach_vm_size_t rsize=0;vm_region_basic_info_data_64_t info;mach_msg_type_number_t n=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t obj=MACH_PORT_NULL;
  int more=mach_vm_region(mach_task_self(),&r,&rsize,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&n,&obj)==KERN_SUCCESS;
  uint64_t start=(prev+GUEST_ALIGN-1)&~(GUEST_ALIGN-1),end=more?r:GUEST_ADDRESS_LIMIT;
  if(end>start&&end-start>=(256ULL<<20)&&log)fprintf(stderr,"[totk-mem] free %#llx..%s%#llx\n",(unsigned long long)start,more?"":"(end of map, at most) ",(unsigned long long)end);
  if(end>start&&end-start>=size){uint8_t *p=reserve_fixed(start,size);if(p)return p;}
  if(!more)return NULL;
  prev=r+rsize;
 }
}
#endif
static void memory_init(void) {
 if(alias_space)return;
#if TARGET_OS_IPHONE
 /* iOS: stack+heap and the alias region go side by side in one reservation (ASLR_SIZE spans both), placed in the lowest
    free gap that holds them. The free space depends on the device, so the alias region shrinks from the requested size
    to ALIAS_MIN_GIB before giving up (logging the free gaps); TOTK_ALIAS_GIB pins one size. */
 uint64_t want=alias_requested(),least=getenv("TOTK_ALIAS_GIB")?want:(ALIAS_MIN_GIB<<30);
 for(uint64_t a=want;a>=least&&!low_space;a-=1ULL<<30)if((low_space=reserve_in_gap(LOW_SIZE+a,0)))alias_len=a;
 if(!low_space){reserve_in_gap(UINT64_MAX,1);runtime_fail("reserve guest stack, heap and alias regions");}
 alias_space=low_space+LOW_SIZE;
#else
 alias_space=reserve_fixed(ALIAS_SPACE_AT,ALIAS_SIZE);
 if(alias_space)for(uint64_t at=LOW_SEARCH_FROM;at+LOW_SIZE<=ALIAS_SPACE_AT&&!low_space;at+=1ULL<<30)low_space=reserve_fixed(at,LOW_SIZE);
 if(!low_space){
  /* macOS 27: the shared cache fills 0x1_8000_0000..0xF_C000_0000 and the system reserves ~24 GiB at a random place in
     0x70_0000_0000..2^39, so neither fixed region fits. As on iOS, put stack+heap and the alias region side by side in one
     free hole, shrinking the alias region (the game uses < 3 GiB of it) until it fits. TOTK_ALIAS_GIB pins one size. */
  if(alias_space)mach_vm_deallocate(mach_task_self(),(mach_vm_address_t)(uintptr_t)alias_space,ALIAS_SIZE);
  uint64_t want=alias_requested(),least=getenv("TOTK_ALIAS_GIB")?want:(8ULL<<30);
  for(uint64_t g=want;!low_space;g-=4ULL<<30){
   for(uint64_t at=LOW_SEARCH_FROM;at+LOW_SIZE+g<=GUEST_ADDRESS_LIMIT&&!low_space;at+=1ULL<<30)
    if((low_space=reserve_fixed(at,LOW_SIZE+g)))alias_len=g;
   if(g<least+(4ULL<<30))break;
  }
  if(!low_space)runtime_fail(getenv("TOTK_ALIAS_GIB")?"reserve guest stack, heap and the TOTK_ALIAS_GIB alias region below 2^39":"reserve guest stack, heap and alias regions below 2^39");
  alias_space=low_space+LOW_SIZE;
 }
#endif
 heap=low_space+HEAP_OFF;
 fprintf(stderr,"[totk] guest layout: alias %#llx+%#llx stack %#llx+%#llx heap %#llx (cap %#llx)\n",(unsigned long long)ALIAS_START,ALIAS_SIZE,(unsigned long long)STACK_START,STACK_SIZE,(unsigned long long)(uintptr_t)heap,HEAP_CAP);
}
/* Scoped stack moves: independent host backing, copy back on unmap.
 * Logical accounting is 4 KiB; Darwin protections are rounded to 16 KiB.
 * The leading/trailing partial source pages cannot be isolated on this host.
 */
static pthread_mutex_t memory_lock=PTHREAD_MUTEX_INITIALIZER;
typedef struct{uint64_t dst,src,size;} StackMap;static StackMap stack_maps[4096];static unsigned nstack_maps;
static int overlaps(uint64_t a,uint64_t n,uint64_t b,uint64_t m){return a<b+m&&b<a+n;}
/* crash_1031 毒化源判定：stack_move 历史环（容量 32，溢出覆盖最旧）。stack_maps[] 在 unmap 即删，
   而 H1 的毒化窗口恰在 unmap 拷回撞复用块之后，须留历史；写侧持 memory_lock，读侧（crash handler）
   只读+算术、无锁，async-signal-safe。全零槽位 lo=0 使 lo-0x4000 下溢成巨值，永不误命中。 */
typedef struct{uint64_t src_lo,src_hi,dst_lo,dst_hi,thread;} StackMoveRec;
#define STACK_MOVE_HIST 32
static StackMoveRec stack_move_hist[STACK_MOVE_HIST];static unsigned stack_move_hist_head;
int runtime_stack_move_hit(uint64_t addr,uint64_t *lo,uint64_t *hi){
 for(unsigned i=0;i<STACK_MOVE_HIST;i++){const StackMoveRec *r=&stack_move_hist[i];
  if(addr>=r->src_lo-0x4000&&addr<r->src_hi+0x4000){if(lo)*lo=r->src_lo;if(hi)*hi=r->src_hi;return 1;}
  if(addr>=r->dst_lo-0x4000&&addr<r->dst_hi+0x4000){if(lo)*lo=r->dst_lo;if(hi)*hi=r->dst_hi;return 1;}}
 return 0;
}
/* One-shot diagnostics for the silent stack_move rejects CreateAliasStackUnsafe retries 64 times over
   (guest SDK: MapAliasStack -> svc#4 -> stack_move error != 0xd401 -> silent 0 -> retry -> Break). */
static uint64_t stack_fail(int unmap,uint64_t dst,uint64_t src,uint64_t size,unsigned code,long bad_page){
 static unsigned logged;if(logged++<8)fprintf(stderr,"[totk-stack] %sMapMemory dst=%#llx src=%#llx size=%#llx ret=%#x STACK=[%#llx+%#llx) ALIAS=[%#llx+%#llx) HEAP=[%#llx+%#llx) badpage=%ld\n",
  unmap?"Un":"",(unsigned long long)dst,(unsigned long long)src,(unsigned long long)size,code,
  (unsigned long long)STACK_START,(unsigned long long)STACK_SIZE,(unsigned long long)ALIAS_START,(unsigned long long)ALIAS_SIZE,
  (unsigned long long)(uintptr_t)heap,(unsigned long long)heap_size,bad_page);
 return code;
}
/* crash_1031 H1 修复（报告 §5.2）：4K 页主机（OHOS 真机）对整个源范围做页级替换——map 搬运完成后
   mmap(PROT_NONE|MAP_FIXED) 封死 [src,src+size)（真机语义：源区消失；首尾不再留 RW 部分页，线程
   存活期间堆视角触碰即 SIGSEGV，crash handler 的毒化源判定会直接指认）；unmap 先 mmap(RW|MAP_FIXED)
   换上全新匿名页再 memcpy 拷回（拷回永远落在全新页上，关掉"拷回撞堆复用/残留旧页"窗口）。
   src/size 已由入口检查保证 4K 对齐，整段替换无部分页。对外语义（返回码/成败判据/拷回内容）不变，
   只收紧页保护时序。16K 页主机（Darwin）保持旧 mprotect 内层页路径：4K 粒度 MAP_FIXED 被内核按
   16K 取整会殃及同页相邻堆数据（原注释 "cannot be isolated" 的物理限制，以安全为先）。
   已知残余：若线程存活期间 SetHeapSize 的 mprotect 恰好覆盖源范围会把封死页翻回 RW（boot 期
   SetHeapSize 早于线程创建，实际不可达，记录在案）。 */
static int stack_src_page_replace(void){static int mode;if(!mode)mode=sysconf(_SC_PAGESIZE)==4096?1:-1;return mode>0;}
static uint64_t stack_move(int unmap,uint64_t dst,uint64_t src,uint64_t size){
 if(!size||((dst|src|size)&4095))return stack_fail(unmap,dst,src,size,0xcc01,-1);
 if(dst<STACK_START||size>STACK_SIZE||dst-STACK_START>STACK_SIZE-size)return stack_fail(unmap,dst,src,size,0xdc01,-1);
 int inHeap=src>=(uintptr_t)heap&&size<=heap_size&&src-(uintptr_t)heap<=heap_size-size;
 uint64_t aliasStart=ALIAS_START;
 int inAlias=src>=aliasStart&&size<=ALIAS_SIZE&&src-aliasStart<=ALIAS_SIZE-size;
 if(!inHeap&&!inAlias)return stack_fail(unmap,dst,src,size,0xdc01,-1);
 pthread_mutex_lock(&memory_lock);
 if(inAlias){unsigned first=(unsigned)((src-aliasStart)/4096);for(uint64_t i=0;i<size/4096;i++)if(!alias_pages[first+i]){pthread_mutex_unlock(&memory_lock);return stack_fail(unmap,dst,src,size,0xdc01,(long)(src+i*4096));}}

 unsigned found=nstack_maps;for(unsigned i=0;i<nstack_maps;i++)if(stack_maps[i].dst==dst&&stack_maps[i].src==src&&stack_maps[i].size==size)found=i;
 uint64_t lo=dst&~0x3fffULL,hi=(dst+size+0x3fff)&~0x3fffULL;
 uint64_t slo=(src+0x3fff)&~0x3fffULL,shi=(src+size)&~0x3fffULL;
 int replace=stack_src_page_replace();   /* H1 修复：4K 页走整段页替换，16K 页维持旧内层页路径 */
 if(unmap){
  if(found==nstack_maps){pthread_mutex_unlock(&memory_lock);return 0xd401;}
  if(replace){if(mmap((void *)(uintptr_t)src,size,PROT_READ|PROT_WRITE,MAP_FIXED|MAP_PRIVATE|MAP_ANON,-1,0)!=(void *)(uintptr_t)src)runtime_fail("remap moved stack source");}
  else if(shi>slo&&mprotect((void *)slo,shi-slo,PROT_READ|PROT_WRITE))runtime_fail("restore moved stack source");
  memcpy((void *)src,(const void *)dst,size);stack_maps[found]=stack_maps[--nstack_maps];
  for(uint64_t page=lo;page<hi;page+=0x4000){int used=0;for(unsigned i=0;i<nstack_maps;i++)if(overlaps(page,0x4000,stack_maps[i].dst,stack_maps[i].size))used=1;
   if(!used&&mprotect((void *)page,0x4000,PROT_NONE))runtime_fail("decommit moved stack");}
 }else{
  if(nstack_maps==4096){pthread_mutex_unlock(&memory_lock);return 0xd401;}
  for(unsigned i=0;i<nstack_maps;i++)if(overlaps(dst,size,stack_maps[i].dst,stack_maps[i].size)||overlaps(src,size,stack_maps[i].src,stack_maps[i].size)){pthread_mutex_unlock(&memory_lock);return stack_fail(unmap,dst,src,size,0xdc01,-1);}
  if(mprotect((void *)lo,hi-lo,PROT_READ|PROT_WRITE))runtime_fail("commit moved stack");memcpy((void *)dst,(const void *)src,size);
  if(replace){if(mmap((void *)(uintptr_t)src,size,PROT_NONE,MAP_FIXED|MAP_PRIVATE|MAP_ANON,-1,0)!=(void *)(uintptr_t)src)runtime_fail("seal moved stack source");}
  else if(shi>slo&&mprotect((void *)slo,shi-slo,PROT_NONE))runtime_fail("protect moved stack source");
  stack_maps[nstack_maps++]=(StackMap){dst,src,size};
  {unsigned long long tid;pthread_threadid_np(NULL,&tid);stack_move_hist[stack_move_hist_head]=(StackMoveRec){src,src+size,dst,dst+size,tid};stack_move_hist_head=(stack_move_hist_head+1)%STACK_MOVE_HIST;}
 }
 pthread_mutex_unlock(&memory_lock);return 0;
}
static uint64_t alias_map(int unmap,uint64_t addr,uint64_t size){
 uint64_t start=ALIAS_START;
 if(!size||((addr|size)&4095))return 0xcc01;
 if(addr<start||size>ALIAS_SIZE||addr-start>ALIAS_SIZE-size)return 0xdc01;
 pthread_mutex_lock(&memory_lock);unsigned first=(unsigned)((addr-start)/4096),count=(unsigned)(size/4096);
 /* Horizon MapPhysicalMemory maps only the pages of the range that are still unmapped and leaves mapped pages (and their
    contents) untouched; TOTK's physical-memory heaps grow by re-mapping ranges that overlap what they already own. */
 uint64_t fresh=0;if(!unmap)for(unsigned i=0;i<count;i++)if(!alias_pages[first+i])fresh+=4096;
 /* Physical-memory resource reservation failure is LimitReached (132), not
    InvalidCurrentMemory (106). The original SDK maps LimitReached to its
    recoverable out-of-memory result; 0xd401 makes AllocateMemoryPages abort. */
 if(!unmap&&fresh+alias_bytes+heap_size+runtime_image_bytes()>APP_POOL){
  pthread_mutex_unlock(&memory_lock);
  const char *negative=getenv("TOTK_PROBE_NEGATIVE");
  return negative&&atoi(negative)==14?0xd401:0x10801;
 }
 if(unmap){
  for(unsigned i=0;i<nstack_maps;i++)if(overlaps(addr,size,stack_maps[i].src,stack_maps[i].size)){pthread_mutex_unlock(&memory_lock);return 0xdc01;}
  uint64_t released=0;for(unsigned i=0;i<count;i++)if(alias_pages[first+i]){alias_pages[first+i]=0;alias_stale[first+i]=1;released+=4096;}alias_bytes-=released;
  uint64_t low=addr&~0x3fffULL,high=(addr+size+0x3fff)&~0x3fffULL;
  for(uint64_t page=low;page<high;page+=0x4000){unsigned p=(unsigned)((page-start)/4096);if(!alias_pages[p]&&!alias_pages[p+1]&&!alias_pages[p+2]&&!alias_pages[p+3]){
   if(mmap((void *)page,0x4000,PROT_NONE,MAP_FIXED|MAP_PRIVATE|MAP_ANON,-1,0)!=(void *)page)runtime_fail("decommit physical alias page");
   memset(alias_stale+p,0,4);}}
 }else if(fresh){
  uint64_t low=addr&~0x3fffULL,high=(addr+size+0x3fff)&~0x3fffULL;
  if(mprotect((void *)low,high-low,PROT_READ|PROT_WRITE))runtime_fail("commit physical alias pages");
  for(unsigned i=0;i<count;i++)if(!alias_pages[first+i]){   /* new pages only: zero-filled like fresh pool pages */
   if(alias_stale[first+i]){memset((void *)(addr+(uint64_t)i*4096),0,4096);alias_stale[first+i]=0;}
   alias_pages[first+i]=1;}
  alias_bytes+=fresh;
 }
 pthread_mutex_unlock(&memory_lock);return 0;
}
uint64_t runtime_heap_base(void){return (uintptr_t)heap;}
void runtime_heap_range(uint8_t **base,size_t *size){*base=heap;*size=heap_size;}   /* key 4: the camera projection search (odyssey_aspect.c) */
/* Guest memory the game has committed (heap size + mapped alias pages), for memory reports (iOS app monitor). */
void runtime_guest_committed(uint64_t *heap_bytes,uint64_t *alias_mapped){*heap_bytes=heap_size;*alias_mapped=alias_bytes;}
/* Physical memory behind the guest regions (stack+heap and alias): resident pages, of which dirty, and pages the system
   compressed. Committed sizes overstate what the game uses; these are what count against the iOS memory limit. */
static void region_pages(uint64_t a,uint64_t end,uint64_t *resident,uint64_t *dirty,uint64_t *compressed){
 while(a<end){vm_address_t va=(vm_address_t)a;vm_size_t vs=0;natural_t depth=0;vm_region_submap_info_data_64_t info;mach_msg_type_number_t cnt=VM_REGION_SUBMAP_INFO_COUNT_64;
  if(vm_region_recurse_64(mach_task_self(),&va,&vs,&depth,(vm_region_recurse_info_t)&info,&cnt)!=KERN_SUCCESS||va>=end)break;
  *resident+=(uint64_t)info.pages_resident*vm_page_size;*dirty+=(uint64_t)info.pages_dirtied*vm_page_size;*compressed+=(uint64_t)info.pages_swapped_out*vm_page_size;
  a=(uint64_t)va+vs;}
}
void runtime_guest_resident(uint64_t *resident,uint64_t *dirty,uint64_t *compressed){
 *resident=*dirty=*compressed=0;if(!low_space)return;
 region_pages((uintptr_t)low_space,(uintptr_t)low_space+LOW_SIZE,resident,dirty,compressed);
 region_pages(ALIAS_START,ALIAS_START+ALIAS_SIZE,resident,dirty,compressed);
}
static void host_diag_init(void) { fprintf(stderr,"[totk] native diagnostics initialized\n"); }
static void host_oe_init(void) { fprintf(stderr,"[totk] application environment: local macOS process\n"); }
static void host_aoc_init(void) { fprintf(stderr,"[totk] add-on content: no DLC\n"); }
static int host_aoc_count(void) { return 0; }
static int host_aoc_list(int *out,int offset,int count) {
 (void)out;if(offset<0||count<0)runtime_fail("invalid add-on content list range");return 0;
}
static void host_error_init(void) { fprintf(stderr,"[totk] error contexts: local diagnostics\n"); }
static void host_fs_init(void) { runtime_fs_init(); }
static void host_pctl_init(void) { fprintf(stderr,"[totk] local application parental controls initialized\n"); }
/* Odyssey's VR mode (Options) asks parental controls about stereo vision. The SDK calls go to the pctl service, which
   this host does not run, and Get / Set abort on a failed call (sdk 1.3.0: GetStereoVisionRestriction returns the
   service's bool, SetStereoVisionRestriction(bool) returns nothing, the other two return nn::Result). A Mac has no
   parental controls: not restricted, and changing the setting is always permitted. */
static bool stereo_restricted;
static bool host_pctl_stereo_get(void) { return stereo_restricted; }
static void host_pctl_stereo_set(bool on) { stereo_restricted = on; fprintf(stderr,"[totk] stereo vision restriction set to %d (local)\n",on); }
static uint32_t host_pctl_stereo_confirm(void) { return 0; }
/* Starting VR mode (smo-recomp audit A3, signatures from the sdk 1.3.0 disassembly): nn::pl::BeginVrMode returns an
   nn::Result, EndVrMode and nn::oe::Begin/EndBlockingHomeButton return nothing, and all four abort if their service call
   fails, which this host had no service for. A Mac has no system VR state and no HOME button to block. */
static uint32_t host_pl_begin_vr(void) { fprintf(stderr,"[totk] VR mode begins (local: the game draws its own stereo view)\n"); return 0; }
static void host_pl_end_vr(void) { fprintf(stderr,"[totk] VR mode ends\n"); }
static void host_oe_home_block(void) {}
static void host_abort_init(void) { fprintf(stderr,"[totk] native crash observer initialized\n"); }
/* crash_1031 追加轮 §E.3c：abort 链两条只打点不改行为的桩——跳板 main+0x97bfa8 先问
   IsDebuggerAttached（实测返 0 → tbz 走 AbortImpl("","",0)），此后每次 abort 自动留名；两桩均原样
   转调 sdk 真身（runtime_symbol 取定义地址，非本表 GOT 槽，无递归），游戏可见行为不变。
   AbortImpl 真身是变参函数，唯一已知调用点（跳板 0x97bfd0）恰只传 4 个定参，按 4 参转调在
   AAPCS64 下被调方可见状态与真调用一致（x4+ 本就携带残余值，变参区由被调方自行保存）。 */
static uint64_t host_diag_is_debugger_attached(void) {
 uint8_t *fn=runtime_symbol("_ZN2nn4diag18IsDebuggerAttachedEv");
 uint64_t r=fn?((uint64_t(*)(void))fn)():0;
 fprintf(stderr,"[totk] diag IsDebuggerAttached -> %llu (thread=%u)\n",(unsigned long long)r,runtime_thread_current_handle());
 return r;
}
/* e1e2 A4：host 只桩了 4 参 AbortImpl，12:00:30 崩溃的第一次 abort 走的是未覆盖重载（见
   notes/04-轮次/evidence_e1e2/abort_forwarding.md §4-E4），天然不进 host 日志。补 6 参（sdk+0x17EDA0）
   与 MetaLogContext 变参（sdk+0x17EEB0）两个 override，只打印不转发——转发会制造嵌套（重入保护）。
   字符串安全截断 ≤64 字节；Result 指针允许为空。注意 MetaLogContext 版签名里没有 module/file/func/line
   （(const MetaLogContext&,const nn::Result*,const char* fmt,...)），透传的是 context 地址+Result+格式串。 */
static const char *diag_str64(const char *s){static char buf[2][65];static int slot;char *b;
 if(!s)return "(null)";b=buf[slot&1];slot++;
 size_t i=0;while(i<64&&s[i])b[i]=s[i],i++;b[i]=0;return b;}
/* M80A：`smo_m80a` bit3(=8) = 6 参 abort 转发开关（缺省 0 = 只打印、不转发 = 旧行为）。
   为什么默认不转发：转发到 sdk 真身会让进程按 SDK 的口径**终止**，而 M79A 已证这条 abort 不是渲染
   必经点（A1 跑满 523 帧不崩）——长跑验证时把它换成「进程退出」是本末倒置；真正的 abort 原因
   上面那行 [totk] 已经打全（module/file/func/line + Result）。开关位只在文件存在时读一次。 */
static int m80a_fwd_abort(void) {
 static int cached=-1; const char *p[2]={"/data/storage/el2/base/files/smo_m80a",
  "/data/app/el2/100/base/com.vibecc.smo/files/smo_m80a"}; int v=0;
 if(cached>=0)return cached;
 cached=0;
 for(int i=0;i<2;i++){FILE *f=fopen(p[i],"r"); if(!f)continue; if(fscanf(f,"%d",&v)!=1)v=0; fclose(f);
  cached=((v>0)&&((v&8)!=0))?1:0; break;}
 return cached;
}
static void host_diag_abort2_6(const char *module,const char *file,const char *func,int line,const void *result,const char *fmt,...) {
 fprintf(stderr,"[totk] diag AbortImpl2 6arg/Result module=%s file=%s func=%s line=%d result=%s%#llx fmt=%s (thread=%u)\n",
  diag_str64(module),diag_str64(file),diag_str64(func),line,result?"":"null:",result?(unsigned long long)*(const uint32_t*)result:(unsigned long long)0,diag_str64(fmt),runtime_thread_current_handle());
 /* M80A：默认到达这里就结束（不转发 ⇒ 进程存活）；bit3=1 才按签名转发。转发时 fmt 换成空串：
    变参个数与被调方不一致会读栈垃圾，而 abort 的正文已经在上面的 [totk] 行里。Result 为空则不转发
    （否则又把 sdk 送回同一个空指针坑）。 */
 if(result && m80a_fwd_abort()){
  uint8_t *fn=runtime_symbol("_ZN2nn4diag6detail9AbortImplEPKcS3_S3_iPKNS_6ResultES3_z");
  if(fn){fprintf(stderr,"[totk] diag AbortImpl2 forwarding to sdk (fmt forced empty)\n");
   ((void(*)(const char *,const char *,const char *,int,const void *,const char *))fn)(module,file,func,line,result,"");}
  else fprintf(stderr,"[totk] diag AbortImpl2 forward target missing (kept alive)\n");
 }
}
static void host_diag_abort2_mlc(const void *ctx,const void *result,const char *fmt,...) {
 fprintf(stderr,"[totk] diag AbortImpl2 MetaLogContext ctx=%p result=%s%#llx fmt=%s (thread=%u)\n",
  ctx,result?"":"null:",result?(unsigned long long)*(const uint32_t*)result:(unsigned long long)0,diag_str64(fmt),runtime_thread_current_handle());
}
/* M80A（2026-11-12）：**崩溃根因修复**。M79A 在 sdk+0x17ed1c（= VAbortImpl+0xAC 的 `ldr w8,[x19]`）
   指令级定谳：x19 = x4 = 第 5 参 `const nn::Result*` = **空** ⇒ VAbortImpl 解引用空指针（SIGSEGV），
   把真正的 abort 信息整段盖掉。来源就是我们这枚桩「只带 4 参转调」：AAPCS64 下被调方看到的 x4 是
   宿主残值 0。sdk 自己的 4 参包装（sdk+0x17EE90）在转调前会 `str wzr,[sp,#8]` 造一份合法 Result ——
   这里做同一件事：**自建 nn::Result{0}（4 字节 POD，值 0 = 成功）显式放进第 5 参**。
   对目标是 4 参包装（会自己重填 x4）还是 5 参真身（读 x4）两种情形都安全。 */
static void host_diag_abort_impl(const char *module,const char *file,const char *func,int line) {
 fprintf(stderr,"[totk] diag AbortImpl module=%p file=%p func=%p line=%d (thread=%u); forwarding to sdk (host-built Result{0})\n",(void *)module,(void *)file,(void *)func,line,runtime_thread_current_handle());
 uint8_t *fn=runtime_symbol("_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i");
 if(!fn)runtime_fail("AbortImpl forward target missing");
 volatile uint32_t result0=0;   /* 栈上合法 nn::Result；调用期间一直活着 */
 ((void(*)(const char *,const char *,const char *,int,const void *))fn)(module,file,func,line,(const void *)&result0);
}
/* nn::vi 显示服务（SMO 导入 6 个，符号来自 sdk 反解）：宿主无 vi:m IPC，按 HLE 盖掉整条显示链。
   Display/Layer 用静态占位句柄；GetNativeWindow 返回非空占位（nvn_vk 未接线前无人消费）；
   vsync 事件在游戏线程上绑定（wrapper 保线程，auto-clear 如真实 vsync），若游戏首帧前阻塞等
   vsync，再补 16.6ms 宿主定时线程调 hle_signal_system_event（threads.c 该函数注释本就预想显示链）。 */
static uint64_t g_vi_display,g_vi_layer;
static uint32_t host_vi_initialize(void){return 0;}
static uint32_t host_vi_open_default_display(void **out){*out=&g_vi_display;return 0;}
static uint32_t host_vi_create_layer(void **layer,void *display){(void)display;*layer=&g_vi_layer;return 0;}
static uint32_t host_vi_set_layer_scaling_mode(void *layer,int mode){(void)layer;(void)mode;return 0;}
static uint32_t host_vi_get_native_window(void **nw,void *layer){(void)layer;*nw=(void*)0x1;return 0;}
/* round46 定谳（F-H se_bind 实证 service-bound）：装库完成等待 0x1003 与 vi vsync 绑定共享同一
   event_t 节点，宿主从不 signal vsync ⇒ 装库永等。真机 vsync 是 60Hz 硬件常驻信号——宿主起
   60Hz 定时线程喂 hle_signal_system_event（该入口注释原话就是为"显示链接的垂直同步回调"设计的
   宿主侧调用，含未绑定自动跳过护栏，游戏销毁事件后安全）。 */
static void *vsync_tick(void *p){dm_host_helper_qos(QOS_CLASS_DEFAULT);for(;;){usleep(16667);hle_signal_system_event(p);}return NULL;}   /* round20b：vsync 宿主辅助线程自设 default（donor dm_host_helper_qos），别继承客体高类 */
static uint32_t host_vi_get_display_vsync_event(void *ev,void *display){
 (void)display;hle_bind_system_event(ev,1);
 static int armed;if(!armed){armed=1;pthread_t th;if(pthread_create(&th,NULL,vsync_tick,ev)!=0)runtime_fail("vsync tick thread");pthread_detach(th);fprintf(stderr,"[totk] vsync 60Hz signal armed\n");}
 return 0;}
/* 终局 F-E（crash_1031 终局六段）：游戏首达 movie/logo 视频阶段，subsdk0 的 NvBlit/SfNvnUtil
 * 初始化调 nv::InitializeGraphics；sdk 内嵌 NV 栈真码三断言（空 buf / 重复初始化 / size<0x8000）
 * 在无 nvdrv 的本移植（movie_host.c:27 既有取舍）下必炸 svcBreak。宿主 override 顶掉真码：
 * 照抄参数校验语义（坏参数照 fail——那是游戏真错），成功路径记账返 0（NV 成功=0）。 */
static uint64_t nv_init_graphics(void *buf,uint64_t size){
 static void *gbuf;static uint64_t gsize;
 fprintf(stderr,"[totk] nv::InitializeGraphics buf=%p size=%#llx\n",buf,(unsigned long long)size);
 if(!buf||!size)runtime_fail("nv::InitializeGraphics null buffer");
 if(size<0x8000)runtime_fail("nv::InitializeGraphics buffer smaller than 0x8000");
 if(gbuf)fprintf(stderr,"[totk] nv::InitializeGraphics called again (%p,%#llx): host stub accepts\n",gbuf,(unsigned long long)gsize);
 gbuf=buf;gsize=size;return 0;}
static uint64_t nv_set_graphics_allocator(void *a,void *f,void *r,void *user){
 fprintf(stderr,"[totk] nv::SetGraphicsAllocator alloc=%p free=%p realloc=%p user=%p (recorded, unused)\n",a,f,r,user);return 0;}
static uint64_t nv_set_graphics_devtools_allocator(void *a,void *f,void *r,void *user){
 (void)a;(void)f;(void)r;(void)user;fprintf(stderr,"[totk] nv::SetGraphicsDevtoolsAllocator (recorded, unused)\n");return 0;}
static uint64_t nv_finalize_graphics(void){fprintf(stderr,"[totk] nv::FinalizeGraphics\n");return 0;}
static void observed_os_init(uint64_t h,uint64_t args) {
 fprintf(stderr,"[totk] ENTER nnosInitialize\n");((void(*)(uint64_t,uint64_t))runtime_symbol("nnosInitialize"))(h,args);fprintf(stderr,"[totk] RETURN nnosInitialize\n");
}
static void observed_sdk_init(void) { fprintf(stderr,"[totk] ENTER nninitInitializeSdkModule\n");((void(*)(void))runtime_symbol("nninitInitializeSdkModule"))();fprintf(stderr,"[totk] RETURN nninitInitializeSdkModule\n"); }
static void observed_startup(void) { fprintf(stderr,"[totk] ENTER nninitStartup\n");((void(*)(void))runtime_symbol("nninitStartup"))();fprintf(stderr,"[totk] RETURN nninitStartup\n"); }
static void observed_main(void) { fprintf(stderr,"[totk] ENTER nnMain\n");((void(*)(void))runtime_symbol("nnMain"))();fprintf(stderr,"[totk] RETURN nnMain\n"); }
static void cpu_boost(int mode) { if(mode<0||mode>2)runtime_fail("unknown CPU boost mode");fprintf(stderr,"[totk] CPU boost request %d: macOS manages CPU frequency\n",mode); }
/* TOTK_OPERATION_MODE=handheld reports the Switch handheld mode (oe OperationMode 0, PerformanceMode Normal 0) instead of
   docked (1, Boost 1): the game then uses its own handheld settings (1280x720 output, lower internal resolution), which
   Nintendo made for half the docked GPU clock. Read once; any other value stops the start. */
static int handheld_mode(void) {static int v=-1;if(v<0){const char *e=getenv("TOTK_OPERATION_MODE");int h;if(!e||!strcmp(e,"docked"))h=0;else if(!strcmp(e,"handheld"))h=1;else runtime_fail("TOTK_OPERATION_MODE must be docked or handheld");
 fprintf(stderr,"[totk] operation mode reported to the game: %s\n",h?"handheld (performance normal)":"docked (performance boost)");v=h;}return v;}
int runtime_handheld_mode(void){return handheld_mode();}   /* runtime/input.c: the pad is the handheld controller then */
static int operation_mode(void) {return handheld_mode()?0:1;}
static int performance_mode(void) {return handheld_mode()?0:1;}
static int focus_policy;
static void focus_mode(int mode) {if(mode<0||mode>3)runtime_fail("unknown focus handling mode");focus_policy=mode;runtime_startup_checkpoint((uintptr_t)__builtin_return_address(0));fprintf(stderr,"[totk] focus policy stored: %d (window lifecycle pending)\n",focus_policy);}
static int focus_state(void) {return runtime_front_focus();}
static unsigned resume_notification,operation_notification,performance_notification;
static void resume_enabled(unsigned value) {resume_notification=value&255;}
static void operation_enabled(unsigned value) {operation_notification=value&255;}
static void performance_enabled(unsigned value) {performance_notification=value&255;}
static uint32_t performance_configuration[2];
static void performance_config(int mode,int cfg) {if(mode<0||mode>1)runtime_fail("invalid performance mode");performance_configuration[mode]=(uint32_t)cfg;fprintf(stderr,"[totk] performance request mode=%d cfg=%x: native macOS scheduling\n",mode,(uint32_t)cfg);}
static uint32_t get_performance_config(int mode){
 if(mode<0||mode>1)runtime_fail("invalid performance mode");
 return performance_configuration[mode]?performance_configuration[mode]:(mode?0x20003u:0x10001u);
}
/* The game's language: TOTK_LANG as given (a Switch language code), otherwise from a locale: TOTK_LOCALE, which the
   launcher sets to the app's first preferred language (BCP 47, e.g. "pt-BR"; it follows a per-app language), or else
   the Mac's current locale. Brazil maps to "pt-BR" only in the 1.4.3 build, whose RomFS has the official USpt text. */
uint64_t runtime_desired_language(void) {
 const char *explicit=getenv("TOTK_LANG");char code[8]={0};
 if(explicit){if(strlen(explicit)>7)runtime_fail("language code too long");memcpy(code,explicit,strlen(explicit));}
 else {
  char language[16]={0},region[4]={0};const char *tag=getenv("TOTK_LOCALE");
  if(tag&&*tag){
   size_t n=strcspn(tag,"-_");if(n<sizeof(language))memcpy(language,tag,n);
   for(const char *p=tag+n;*p;){p+=strspn(p,"-_");size_t k=strcspn(p,"-_");
    if(k==2&&isalpha((unsigned char)p[0])&&isalpha((unsigned char)p[1])){region[0]=(char)toupper((unsigned char)p[0]);region[1]=(char)toupper((unsigned char)p[1]);}
    p+=k;}
  } else {
   CFLocaleRef locale=CFLocaleCopyCurrent();
   CFStringRef value=CFLocaleGetValue(locale,kCFLocaleLanguageCode);
   if(value)CFStringGetCString(value,language,sizeof(language),kCFStringEncodingUTF8);
   value=CFLocaleGetValue(locale,kCFLocaleCountryCode);
   if(value)CFStringGetCString(value,region,sizeof(region),kCFStringEncodingUTF8);CFRelease(locale);
  }
#ifdef TOTK_GAME_V143
  const char *portuguese=!strcmp(region,"BR")?"pt-BR":"pt";
#else
  const char *portuguese="pt";
#endif
  const char *selected=!strcmp(language,"ja")?"ja":!strcmp(language,"de")?"de":!strcmp(language,"fr")?"fr":!strcmp(language,"es")?"es":!strcmp(language,"it")?"it":!strcmp(language,"nl")?"nl":!strcmp(language,"ru")?"ru":!strcmp(language,"ko")?"ko":!strcmp(language,"zh")?"zh-Hans":!strcmp(language,"pt")?portuguese:"en-US";
  memcpy(code,selected,strlen(selected));
 }
 uint64_t result;memcpy(&result,code,8);return result;
}
static uint32_t observed_region(uint64_t *out,uint64_t size){
 uint8_t *fn=runtime_symbol("_ZN2nn2os21AllocateAddressRegionEPmm");uint32_t r=((uint32_t(*)(uint64_t *,uint64_t))fn)(out,size);
 fprintf(stderr,"[totk] AllocateAddressRegion size=%llx result=%x address=%llx\n",(unsigned long long)size,r,(unsigned long long)(r?0:*out));
 if(r&&getenv("TOTK_TRACE_VAMM")){
  uint8_t *sdk=fn-0x12ca60;uint64_t root;memcpy(&root,sdk+0xd16bb8,8);uint64_t *v=(void *)(uintptr_t)(root+0x418);
  for(unsigned i=0;i<10;i++)fprintf(stderr,"[totk] VAMM +%x=%llx\n",i*8,(unsigned long long)v[i]);
  if(v[8]){uint64_t *a=(void *)(uintptr_t)v[8];for(unsigned i=0;i<12;i++)fprintf(stderr,"[totk] allocator +%x=%llx\n",i*8,(unsigned long long)a[i]);if(a[2]){uint64_t *node=(void *)(uintptr_t)a[2];for(unsigned i=0;i<12;i++)fprintf(stderr,"[totk] node +%x=%llx\n",i*8,(unsigned long long)node[i]);}}
 }
 return r;
}
static uint32_t observed_pages(uint64_t addr,uint64_t size){uint32_t r=((uint32_t(*)(uint64_t,uint64_t))runtime_symbol("_ZN2nn2os19AllocateMemoryPagesEmm"))(addr,size);fprintf(stderr,"[totk] AllocateMemoryPages address=%llx size=%llx result=%x\n",(unsigned long long)addr,(unsigned long long)size,r);return r;}
void *runtime_observer(const char *name) {
 static const struct {const char *name;void *fn;} entries[]={
 {"_ZN2nn2os21AllocateAddressRegionEPmm",observed_region},{"_ZN2nn2os19AllocateMemoryPagesEmm",observed_pages},{"nnosInitialize",observed_os_init},{"nninitInitializeSdkModule",observed_sdk_init},{"nninitStartup",observed_startup},{"nnMain",observed_main}};
 for(size_t i=0;i<sizeof(entries)/sizeof(entries[0]);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
void *runtime_override(const char *name) {
 static const struct {const char *name;void *fn;} entries[]={
  {"_ZN2nn2oe18GetDesiredLanguageEv",runtime_desired_language},
  {"_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi",performance_config},
  {"_ZN2nn2oe27GetPerformanceConfigurationENS0_15PerformanceModeE",get_performance_config},
  {"_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE",cpu_boost},{"_ZN2nn2oe16GetOperationModeEv",operation_mode},{"_ZN2nn2oe18GetPerformanceModeEv",performance_mode},
  {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE",focus_mode},{"_ZN2nn2oe20GetCurrentFocusStateEv",focus_state},
  {"_ZN2nn2oe28SetResumeNotificationEnabledEb",resume_enabled},{"_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb",operation_enabled},{"_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb",performance_enabled},
  {"nndiagStartup",host_diag_init},{"_ZN2nn2oe10InitializeEv",host_oe_init},{"_ZN2nn3aoc10InitializeEv",host_aoc_init},
  {"_ZN2nn3aoc17CountAddOnContentEv",host_aoc_count},{"_ZN2nn3aoc16ListAddOnContentEPiii",host_aoc_list},
  {"_ZN2nn3err26InitializeContextWriterApiEv",host_error_init},{"_ZN2nn2fs24InitializeForApplicationEv",host_fs_init},
  {"_ZN2nn4pctl10InitializeEv",host_pctl_init},{"_ZN2nn4pctl26GetStereoVisionRestrictionEv",host_pctl_stereo_get},
  {"_ZN2nn4pctl26SetStereoVisionRestrictionEb",host_pctl_stereo_set},
  {"_ZN2nn4pctl42ConfirmStereoVisionRestrictionConfigurableEv",host_pctl_stereo_confirm},
  {"_ZN2nn4pctl53RequestStereoVisionRestrictionConfigurationPermissionEv",host_pctl_stereo_confirm},
  {"_ZN2nn2pl11BeginVrModeEv",host_pl_begin_vr},{"_ZN2nn2pl9EndVrModeEv",host_pl_end_vr},
  {"_ZN2nn2oe23BeginBlockingHomeButtonEv",host_oe_home_block},{"_ZN2nn2oe21EndBlockingHomeButtonEv",host_oe_home_block},{"_ZN2nn4diag34InitializeApplicationAbortObserverEv",host_abort_init},{"_ZN2nn4diag18IsDebuggerAttachedEv",host_diag_is_debugger_attached},{"_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i",host_diag_abort_impl},{"_ZN2nn4diag6detail9AbortImplEPKcS3_S3_iPKNS_6ResultES3_z",host_diag_abort2_6},{"_ZN2nn4diag6detail9AbortImplERKNS0_14MetaLogContextEPKNS_6ResultEPKcz",host_diag_abort2_mlc},
  {"_ZN2nn2vi10InitializeEv",host_vi_initialize},
  {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",host_vi_open_default_display},
  {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",host_vi_create_layer},
  {"_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE",host_vi_set_layer_scaling_mode},
  {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",host_vi_get_native_window},
  {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",host_vi_get_display_vsync_event},
  {"_ZN2nv18InitializeGraphicsEPvm",nv_init_graphics},   /* 终局 F-E：NV 图形层断言族（crash_1031 终局六段） */
  {"_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_",nv_set_graphics_allocator},
  {"_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_",nv_set_graphics_devtools_allocator},
  {"_ZN2nv16FinalizeGraphicsEv",nv_finalize_graphics}};
 for(size_t i=0;i<sizeof(entries)/sizeof(entries[0]);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;
 void *by3=runtime_by3_override(name);if(by3)return by3;
 void *input=runtime_input_override(name);if(input)return input;void *font=runtime_font_override(name);if(font)return font;void *codec=runtime_codec_override(name);if(codec)return codec;void *movie=runtime_movie_override(name);if(movie)return movie;void *clock=runtime_time_override(name);if(clock)return clock;void *applet=runtime_applet_override(name);if(applet)return applet;void *swkbd=runtime_swkbd_override(name);if(swkbd)return swkbd;void *account=runtime_account_override(name);if(account)return account;void *fs=runtime_fs_override(name);if(fs)return fs;void *front=runtime_front_override(name);return front?front:runtime_gfx_override(name);
}
void runtime_core_checkpoint(void);
/* svcBreak 取证（crash_1031 追加轮 §E.3a）：reason 无放行位即 fatal，runtime_fail 前把 x29-0x60..x29+0x180
   （60 个 u64，new 失败的 info{heap,?,size,align} 就在堆核心帧的这段栈上，0x951f34-44 stp/str 落栈）一次性
   单行转储——write(2) 直写不走 stdio 缓冲，防 abort 路径丢日志；离线从栈窗里捡失败堆指针喂
   TOTK_CRASH_HEAPWALK 走空闲链：链上出现 ASCII size=毒化定案，链空且 heap_size 顶格=真不够大定案。 */
static void svc_break_hex(char *b,size_t *n,uint64_t v){b[(*n)++]=' ';b[(*n)++]='0';b[(*n)++]='x';for(int i=15;i>=0;i--)b[(*n)++]="0123456789abcdef"[(v>>(i*4))&15];}
static void svc_break_window(uint64_t x29){
 uint64_t w[60];memcpy(w,(const void *)(uintptr_t)(x29-0x60),sizeof w);
 char b[64+60*19];size_t n=0;const char *lab="[totk] Break x29-0x60..+0x180:";
 while(*lab)b[n++]=*lab++;
 for(unsigned i=0;i<60;i++)svc_break_hex(b,&n,w[i]);
 b[n++]='\n';write(2,b,n);
}
/* round9 观测（偶发 SIGABRT 定位）：装机崩溃 backtrace 落宿主 libgame.so 的 runtime_svc+5104——
   arm64 obj 符号化定谳为 svc 0x26 guest Break 致命路的 runtime_fail("original SDK Break")
   （services.c:529）。abort 时不知在处理哪个 SVC：入口把 {num,x0..x3,fp,lr,module,offset} 存进
   全局槽（纯 store、无锁无 I/O，num 最后写=提交位），正常返回清零；runtime_fail 与 SIGABRT 的
   abort_report（loader.c）各打一行 write(2) 落笔（垂死路径缓冲 stdio 会丢，svc_break_window 同款）。 */
static struct { const char *module; uint64_t offset, x[4], fp, lr; unsigned num; } g_svc_ctx;
void runtime_svc_context_line(const char *tag) {
 char b[512]; size_t n = 0;
 while (*tag && n < sizeof b - 160) b[n++] = *tag++;
 unsigned num = g_svc_ctx.num;   /* 快照：可能与并发写竞争，取证可接受 */
 if (!num) { const char *s = "svc=none (between SVCs)\n"; while (*s) b[n++] = *s++; write(2, b, n); return; }
 const char *s = "svc=0x"; while (*s) b[n++] = *s++;
 for (int i = 28; i >= 0; i -= 4) b[n++] = "0123456789abcdef"[(num >> i) & 15];
 if (g_svc_ctx.module) { s = " at "; while (*s) b[n++] = *s++;
  for (const char *m = g_svc_ctx.module; *m && n < sizeof b - 160; ) b[n++] = *m++;
  s = "+0x"; while (*s) b[n++] = *s++;
  for (int i = 60; i >= 0; i -= 4) b[n++] = "0123456789abcdef"[(unsigned)((g_svc_ctx.offset >> i) & 15)]; }
 static const char *const names[4] = {" x0=", " x1=", " x2=", " x3="};
 for (int k = 0; k < 4; k++) { s = names[k]; while (*s) b[n++] = *s++;
  for (int i = 60; i >= 0; i -= 4) b[n++] = "0123456789abcdef"[(unsigned)((g_svc_ctx.x[k] >> i) & 15)]; }
 s = " fp="; while (*s) b[n++] = *s++;
 for (int i = 60; i >= 0; i -= 4) b[n++] = "0123456789abcdef"[(unsigned)((g_svc_ctx.fp >> i) & 15)];
 s = " lr="; while (*s) b[n++] = *s++;
 for (int i = 60; i >= 0; i -= 4) b[n++] = "0123456789abcdef"[(unsigned)((g_svc_ctx.lr >> i) & 15)];
 b[n++] = '\n'; write(2, b, n);
}
static uint64_t runtime_svc_dispatch(unsigned num,uint64_t *r,const char *module,uint64_t offset);
/* M6o: the pc of the SVC instruction itself, for the per-thread svc8 ring (threads.c). `offset` is the
   gate's ORIGINAL site inside the module (sdk.gates.tsv column 4, e.g. 0x37ec34 for svc 0x34), while the
   LR at the gate is only the patch-area stub (sdk+0xd3f3f4) and carries no call-site information.
   Per-thread because SVCs run on the guest thread; saved/restored so a nested dispatch cannot leak. */
__thread uint64_t runtime_svc_pc_tls;
__thread const char *runtime_svc_mod_tls;
uint64_t runtime_svc(unsigned num,uint64_t *r,const char *module,uint64_t offset) {
 /* Black-screen case: the game never presents a frame, so the frame-based freeze watchdog never arms and a signal cannot
    be delivered (hdc shell is not allowed to signal the app). Dump once from here instead: this runs on the guest thread
    itself, which is the one context where runtime_dump_threads() is proven safe (the abort path already calls it). */
 { static int dumped; if (!dumped) { static long t0; struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
     if (!t0) t0 = ts.tv_sec;
     if (ts.tv_sec - t0 >= 20 && getenv("SMO_DUMP")) { dumped = 1; { __attribute__((weak)) void runtime_dump_threads(void); if (runtime_dump_threads) runtime_dump_threads(); } } } }
 g_svc_ctx.module = module; g_svc_ctx.offset = offset; g_svc_ctx.fp = r[29]; g_svc_ctx.lr = r[30];
 for (int i = 0; i < 4; i++) g_svc_ctx.x[i] = r[i];
 g_svc_ctx.num = num;   /* 提交位最后写：读到 num>0 时其余字段已就位（abort 竞态下至多看到半旧值，取证可接受） */
 uint64_t prev_pc = runtime_svc_pc_tls; const char *prev_mod = runtime_svc_mod_tls;
 runtime_svc_pc_tls = offset; runtime_svc_mod_tls = module;
 /* M6r：失败账本要的是**调用时**的入参（dispatch 会用返回下标覆盖 r[1..3]，例如 GetInfo 的 r[1]、
   WaitSynchronization 的 r[1]=index），所以先抄 4 个字。纯寄存器搬运，无 I/O 无时钟。 */
 uint64_t fa_a[4] = { r[0], r[1], r[2], r[3] };
 uint64_t res = runtime_svc_dispatch(num,r,module,offset);
 /* M6r：只有非零返回才进账本（零 = 成功，热路径只多一次寄存器判断）。两个例外不记：
   0x0b SleepThread 的 r[0] 原样保留请求的纳秒数（不是返回码），0x10 GetCurrentProcessorNumber
   的 r[0] 是当前核号（0..2）——两者都是「值」不是「结果」，记进去只会造噪声。 */
 if (r[0] && num != 0x0b && num != 0x10) {
  extern void runtime_fa_svc(uint32_t num, const uint64_t *a, uint32_t ret, uint64_t lr, uint32_t thr);
  runtime_fa_svc((uint32_t)num, fa_a, (uint32_t)r[0], r[30], runtime_thread_current_handle());
 }
 runtime_svc_pc_tls = prev_pc; runtime_svc_mod_tls = prev_mod;
 g_svc_ctx.num = 0;   /* 已返回：此后 abort 落在 SVC 间隙 */
 return res;
}
static uint64_t runtime_svc_dispatch(unsigned num,uint64_t *r,const char *module,uint64_t offset) {
 runtime_core_checkpoint();
 // Successful SVCs occur several times per frame. Keep the detailed trace available
 // for diagnostics without formatting and writing every call during normal play.
 static _Atomic int trace=-1;int enabled=atomic_load_explicit(&trace,memory_order_relaxed);
 if(enabled<0){enabled=getenv("TOTK_TRACE_SVC")!=NULL;atomic_store_explicit(&trace,enabled,memory_order_relaxed);}
 if(enabled)fprintf(stderr,"[totk] SVC 0x%x at %s+0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n",num,module,(unsigned long long)offset,(unsigned long long)r[0],(unsigned long long)r[1],(unsigned long long)r[2],(unsigned long long)r[3]);
 if(svc_thread((int)num,r))return r[0];
 if(runtime_ipc_svc(num,r))return r[0];
 memory_init();
 switch(num) {
 case 0x02:r[0]=runtime_module_permission(r[0],r[1],(unsigned)r[2]);return r[0];
 case 0x03:r[0]=runtime_module_lock_permission(r[0],r[1],(unsigned)r[2],(unsigned)r[3]);return r[0];
 case 0x29: {
  uint64_t value=0;unsigned id=(unsigned)r[1];
  switch(id) {
  case 0:value=7;break;case 1:value=0x0ffffffff0000000ULL;break;
  case 2:value=ALIAS_START;break;case 3:value=ALIAS_SIZE;break;
  case 4:value=(uintptr_t)heap;break;case 5:value=HEAP_CAP;break;
  case 6:case 21:value=APP_POOL;break;case 7:case 22:value=runtime_image_bytes()+heap_size+alias_bytes;
   { static unsigned polls; if(getenv("TOTK_MEM_LOG")&&(polls++%600)==0) fprintf(stderr,"[totk-mem] GetInfo(%u) total=%#llx used=%#llx image=%#llx heap=%#llx alias=%#llx\n",id,(unsigned long long)APP_POOL,(unsigned long long)value,(unsigned long long)runtime_image_bytes(),(unsigned long long)heap_size,(unsigned long long)alias_bytes); }
   break;
  case 8:value=0;break;case 10:value=0;break;
  case 11:arc4random_buf(&value,8);break;
  case 12:value=ASLR_START;break;case 13:value=ASLR_SIZE;break;
  case 14:value=STACK_START;break;case 15:value=STACK_SIZE;break;
  case 16:value=SYSTEM_RESOURCE_BUDGET;break;case 17:value=sizeof(stack_maps)+sizeof(alias_pages);break;case 18:value=GAME_TITLE_ID;break;
  case 20:value=(uintptr_t)user_exception_context;break;   /* UserExceptionContextAddress: SDK stores e.g. the dying-message region at +0x1c0 */case 23:value=1;break;
  /* AliasRegionExtraSize (18.0.0+): our alias reservation contains no
   * kernel-owned extension outside the range reported by ids 2 and 3. */
  case 28:value=0;break;
  default:fprintf(stderr,"unknown GetInfo id %u\n",id);runtime_fail("GetInfo id not implemented");
  }
  r[0]=0;r[1]=value;return 0;
 }
 case 0x2c:case 0x2d:{uint64_t a=r[0],n=r[1];r[0]=alias_map(num==0x2d,a,n);
  if(getenv("TOTK_MEM_LOG"))fprintf(stderr,"[totk-mem] %sMapPhysicalMemory %#llx+%#llx -> %#llx alias=%#llx remaining=%#llx\n",num==0x2d?"Un":"",(unsigned long long)a,(unsigned long long)n,(unsigned long long)r[0],(unsigned long long)alias_bytes,(unsigned long long)(APP_POOL-runtime_image_bytes()-heap_size-alias_bytes));
  if(r[0])fprintf(stderr,"[totk] %sMapPhysicalMemory %#llx+%#llx failed %#llx (alias %#llx used, heap %#llx)\n",num==0x2d?"Un":"",(unsigned long long)a,(unsigned long long)n,(unsigned long long)r[0],(unsigned long long)alias_bytes,(unsigned long long)heap_size);return r[0];}
 case 0x04:case 0x05:{
  /* round23：一次性返回值日志（前 4 次）定论 CreateAliasStackUnsafe 64 连败时 svc#4 的真实返回：
     0=MapAliasStack 成功→败因在 CheckGuardSpace（QueryMemory 误报）；非 0=stack_move 拒绝（配 stack_fail 行）。 */
  uint64_t md=r[0],ms=r[1],mn=r[2];r[0]=stack_move(num==5,md,ms,mn);
  {static unsigned logged;if(logged++<4)fprintf(stderr,"[totk-stack] svc %#x %sMapMemory dst=%#llx src=%#llx size=%#llx ret=%#llx heap=%#llx+%#llx maps=%u\n",
   num,num==5?"Un":"",(unsigned long long)md,(unsigned long long)ms,(unsigned long long)mn,(unsigned long long)r[0],
   (unsigned long long)(uintptr_t)heap,(unsigned long long)heap_size,nstack_maps);}
  return r[0];}
 case 0x06: {
  uint64_t addr=r[2],base=addr&~0xfffULL,size=0x1000;unsigned state=0,perm=0,attr=0;
  uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(pthread_self());size_t stack=pthread_get_stacksize_np(pthread_self());
  /* round23 边界护栏：宿主栈范围必须"非空、像线程栈、top>stack"才可信——防 shim 层再退回垃圾值时
     top-stack 下溢把整段客体地址误报成宿主栈（state 0xb，round17/22 的 CheckGuardSpace 64 连败）。 */
  if(stack&&stack<=(1u<<30)&&top>stack&&addr>=top-stack&&addr<top){base=top-stack;size=stack;state=0xb;perm=3;}
  else if(addr>=(uintptr_t)heap&&addr<(uintptr_t)heap+heap_size){base=(uintptr_t)heap;size=heap_size;state=5;perm=3;}
  else if(addr>=(uintptr_t)low_space&&addr<(uintptr_t)low_space+LOW_SIZE){base=(uintptr_t)low_space;size=LOW_SIZE;if(addr<(uintptr_t)heap){size=(uintptr_t)heap-base;}else{base=(uintptr_t)heap+heap_size;size=(uintptr_t)low_space+LOW_SIZE-base;}}
  else if(addr>=(uintptr_t)low_space+LOW_SIZE&&addr<ALIAS_START){base=(uintptr_t)low_space+LOW_SIZE;size=ALIAS_START-base;state=0x10;perm=0;}   /* host-owned: Inaccessible */
  else if(!addr){base=0;size=ASLR_START;state=0;perm=0;}
  else if(runtime_module_query(addr,&base,&size,&state,&perm,&attr)){}
  else {state=3;perm=5;}
  pthread_mutex_lock(&memory_lock);
  if(addr>=ALIAS_START&&addr<ALIAS_START+ALIAS_SIZE){
   unsigned p=(unsigned)((addr-ALIAS_START)/4096),low=p,high=p+1;unsigned allocated=alias_pages[p];
   while(low>0&&alias_pages[low-1]==allocated)low--;while(high<sizeof(alias_pages)&&alias_pages[high]==allocated)high++;
   base=ALIAS_START+(uint64_t)low*4096;size=(uint64_t)(high-low)*4096;state=allocated?5:0;perm=allocated?3:0;
  }
  for(unsigned i=0;i<nstack_maps;i++){
   StackMap *m=&stack_maps[i];uint64_t end=m->dst+m->size,send=m->src+m->size;
   if(addr>=m->dst&&addr<end){base=m->dst;size=m->size;state=0xb;perm=3;break;}
   if(addr>=m->src&&addr<send){base=m->src;size=m->size;state=5;perm=0;attr=4;break;}
   if(state==0){uint64_t oldEnd=base+size;if(end<=addr&&end>base)base=end;if(m->dst>addr&&m->dst<oldEnd)oldEnd=m->dst;size=oldEnd-base;}
   else if(state==5){uint64_t oldEnd=base+size;if(send<=addr&&send>base)base=send;if(m->src>addr&&m->src<oldEnd)oldEnd=m->src;size=oldEnd-base;}
  }pthread_mutex_unlock(&memory_lock);
  struct {uint64_t base,size;uint32_t state,attr,perm,ipc,device,pad;} info={base,size,state,attr,perm,0,0,0};
  if(r[0])memcpy((void *)(uintptr_t)r[0],&info,sizeof(info));r[0]=0;r[1]=0;return 0;
 }
 case 0x01:{size_t size=r[1];
  /* crash_1031 追加轮取证：SetHeapSize（svc#1）无条件打点（不再依赖 TOTK_MEM_LOG）——候选②
     "堆真不够大"的决定性证据：进入打请求尺寸与三条判据现值（cap/2MB 对齐/pool 余量），返回路径
     打返回码；被拒行可直接对账是哪条判的 0xd401。 */
  fprintf(stderr,"[totk-mem] SetHeapSize %#zx: pool=%#llx heap_cap=%#llx image=%#llx alias=%#llx cur=%#zx\n",size,(unsigned long long)APP_POOL,(unsigned long long)HEAP_CAP,(unsigned long long)runtime_image_bytes(),(unsigned long long)alias_bytes,heap_size);
  if(getenv("TOTK_MEM_LOG"))fprintf(stderr,"[totk-mem] SetHeapSize %#zx (image=%#llx alias=%#llx pool=%#llx)\n",size,(unsigned long long)runtime_image_bytes(),(unsigned long long)alias_bytes,(unsigned long long)APP_POOL);
  if(size>HEAP_CAP||size%0x200000||size+runtime_image_bytes()>APP_POOL){r[0]=0xd401;fprintf(stderr,"[totk-mem] SetHeapSize %#zx -> REJECTED 0xd401\n",size);return r[0];}
  if(size>heap_size&&mprotect(heap+heap_size,size-heap_size,PROT_READ|PROT_WRITE))runtime_fail("commit heap");
  if(size<heap_size&&mprotect(heap+size,heap_size-size,PROT_NONE))runtime_fail("decommit heap");heap_size=size;r[0]=0;r[1]=(uintptr_t)heap;fprintf(stderr,"[totk-mem] SetHeapSize -> 0 base=%#llx\n",(unsigned long long)(uintptr_t)heap);return 0;}
 case 0x0c: r[0]=0;r[1]=44;return 0;
 case 0x0e: r[0]=0;r[1]=0;r[2]=15;return 0;
 case 0x25: {uint64_t id;pthread_threadid_np(NULL,&id);r[0]=0;r[1]=id;return 0;}
 case 0x10:r[0]=0;return 0;
 case 0x26: {uint32_t result=0;if(r[1]&&r[2]>=4)memcpy(&result,(void *)(uintptr_t)r[1],4);fprintf(stderr,"[totk] guest Break reason=%llx result=%x\n",(unsigned long long)r[0],result);
  {char rb[19*19+1];size_t rn=0;for(unsigned i=0;i<=18;i++){if(i)rb[rn++]=' ';for(int k=60;k>=0;k-=4)rb[rn++]="0123456789abcdef"[(r[i]>>k)&15];}rb[rn]=0;fprintf(stderr,"[totk] Break regs x0..x18: %s\n",rb);}   /* round11：每轮诊断提速；单行防 hilog 吞行 */
  runtime_trace_guest(r[29],r[30]);if(r[0]&0x80000000ULL)return 0;svc_break_window(r[29]);{__attribute__((weak)) void runtime_dump_threads(void);if(runtime_dump_threads)runtime_dump_threads();}/* 诊断模式：把 sdk Break 从致命改为「记录后继续」。理由：模拟器上 surface 的创建晚于这次 abort，应用被杀 = 永远没有窗口可画，G1 无从谈起。这里只跳过 exit，不改 sdk/镜像；要恢复致命语义删掉本段即可。 */fprintf(stderr,"[totk] Break NON-FATAL (diagnostic build): guest continues\n");r[0]=0;return 0;}
 case 0x27: {size_t n=r[1];if(n>65536)runtime_fail("debug string too long");fwrite((void *)(uintptr_t)r[0],1,n,stderr);r[0]=0;return 0;}
 case 0x0b:{int64_t ns=(int64_t)r[0];if(ns>0){struct timespec t={ns/1000000000,ns%1000000000};nanosleep(&t,NULL);}r[0]=0;return 0;}
 default:fprintf(stderr,"[totk] unimplemented SVC 0x%x at %s+%#llx\n",num,module,(unsigned long long)offset);if(num==0x1f)fprintf(stderr,"[totk] named port %.12s\n",(const char *)(uintptr_t)r[1]);runtime_trace_guest(r[29],r[30]);runtime_fail("system service not implemented");return 0;
 }
}
