// ohos_compat.h —— 强制注入（-include）：把宿主源码里的 Apple/BSD 专有调用映射到 OHOS musl
// 映射原则：mach_vm_read_overwrite 等对自进程的内存读 == memcpy；
//          mach 绝对时间 == clock_gettime(CLOCK_MONOTONIC_RAW) 换算；
//          行为待周末真机逐个验证，此处保证编译与语义近似。
#pragma once

#if !defined(__ASSEMBLER__)  // 汇编源(hle_stack.S)也被 -include 强灌本头：整个头对汇编隐藏
// M43A：给 nvn_vk2/rec/compat（nx2android 的 Darwin 垫层）一个可检测的标记——
// 两套垫层同时在场时，以本头（项目既有、全 target 强制注入）为准，避免重复定义。
#define DM_OHOS_COMPAT_H 1
// M43A / S2a：录制层（nvn_vk2/rec/**）自带 nx2android 的 Darwin 垫层（rec/compat/include），
// 两套垫层逐项重名。那批 TU 由 CMake 传 -DM43A_REC_TU=1：本头整体让位，只留下面这个标记。
#if defined(M43A_REC_TU)
#else
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>

// macOS stat timespec 字段 → musl 命名
#define st_mtimespec st_mtim
#define st_ctimespec st_ctim
#define st_atimespec st_atim
#define st_birthtimespec st_mtim

// macOS 时钟/类型别名
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC_RAW
typedef unsigned int vm_size_t;
#ifndef F_FULLFSYNC
#define F_FULLFSYNC 51 /* shim: OHOS fcntl 无此 cmd（返回 EINVAL），周末改 fdatasync */
#define F_GETPATH   50 /* shim: macOS fcntl 取文件路径，OHOS 退化（返回 EINVAL） */
#endif

// pthread_threadid_np → gettid
#include <sys/syscall.h>
static inline int compat_threadid(unsigned long long *out) { *out = (unsigned long long)syscall(SYS_gettid); return 0; }
#define pthread_threadid_np(t, out) compat_threadid(out)

// round23：宿主线程栈范围（platform/services.c svc#6 QueryMemory 首分支用；实现见 ohos_stub.c）。
// 原型必须先于调用点可见：此前无声明，services.c 按隐式 int 取返回值（编译为取 x0 低 32 位再符号
// 扩展），宿主线程描述符地址低 32 位带符号位时（ASLR 抽签，round17/22 两次命中）top 变
// 0xffffffff_xxxxxxxx，svc#6 把所有客体地址误报成宿主栈 state 0xb，SDK CheckGuardSpace 要
// guard 页 state==0 → CreateAliasStackUnsafe 64 连败空消息 Break。
#include <pthread.h>
uintptr_t pthread_get_stackaddr_np(pthread_t t);
size_t pthread_get_stacksize_np(pthread_t t);

// Mach-O getsectiondata（GPU 覆盖表 __DATA,totkgfx 等）：OHOS ELF 下读 libgame.so 自身同名段。
// dladdr 本函数拿 .so 路径与装载基址 → open 读 ELF64 头/节头表/.shstrtab 按名定位
// （clang 在 ELF 上把 section("seg,sect") 原样作为单段名，故按 "seg,sect" 拼串匹配）→
// alloc 段直接拷运行时镜像（dli_fbase+sh_addr，RELATIVE 重定位已由动态链接器完成）→
// malloc 缓存（boot 早期单线程调用，无锁），size 写回字节数。
#include <stdlib.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <elf.h>
extern char _mh_execute_header;
static inline const void *getsectiondata(const void *hdr, const char *seg, const char *sect, unsigned long *size) {
    (void)hdr;
    if (size) *size = 0;
    if (!seg || !sect) return NULL;
    static struct { char key[96]; void *data; unsigned long size; } cache[8];   /* 每翻译单元独立缓存，boot 期单线程 */
    char key[96];
    int kn = snprintf(key, sizeof key, "%s,%s", seg, sect);
    if (kn <= 0 || (size_t)kn >= sizeof key) return NULL;
    for (int i = 0; i < 8; i++)
        if (cache[i].data && strcmp(cache[i].key, key) == 0) { if (size) *size = cache[i].size; return cache[i].data; }
    Dl_info di;
    if (!dladdr((void *)&getsectiondata, &di) || !di.dli_fname || !di.dli_fbase) return NULL;   /* 本函数即随 libgame.so 装载 */
    int fd = open(di.dli_fname, O_RDONLY);
    if (fd < 0) return NULL;
    void *result = NULL;
    Elf64_Ehdr eh;
    Elf64_Shdr sh[96], *hit = NULL;   /* 本头被 .cpp 以 C++ 编译：goto 不得跨过带初始化的声明，故上提到所有 goto 之前 */
    if (read(fd, &eh, sizeof eh) != (ssize_t)sizeof eh || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0
        || eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_shentsize != sizeof(Elf64_Shdr)
        || eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_shnum > 96 || eh.e_phnum > 32)
        goto out;
    if (lseek(fd, (off_t)eh.e_shoff, SEEK_SET) < 0 || read(fd, sh, sizeof(Elf64_Shdr) * eh.e_shnum) != (ssize_t)(sizeof(Elf64_Shdr) * eh.e_shnum))
        goto out;
    if (!eh.e_shstrndx || eh.e_shstrndx >= eh.e_shnum) goto out;
    {   /* 节名表：按 "seg,sect" 全名找段 */
        size_t strsz = sh[eh.e_shstrndx].sh_size;
        char *strs = (char *)malloc(strsz);
        if (!strs) goto out;
        if (lseek(fd, (off_t)sh[eh.e_shstrndx].sh_offset, SEEK_SET) < 0 || read(fd, strs, strsz) != (ssize_t)strsz) { free(strs); goto out; }
        for (unsigned i = 0; i < eh.e_shnum && !hit; i++) {
            if (sh[i].sh_name >= strsz) continue;
            if (strcmp(sh[i].sh_name + strs, key) == 0) hit = &sh[i];
        }
        free(strs);
    }
    if (!hit || !hit->sh_size) goto out;
    {   /* alloc 段（sh_addr≠0，落在 PT_LOAD 内）：动态链接器装载时已把该段全部 R_AARCH64_RELATIVE
           重定位完，直接拷运行时镜像（dli_fbase+sh_addr）。段内指针在文件里是零、真值在 .rela.dyn
           addend 里——从文件读再手动补基址只能拿到全零（round5 对拍实证）。非 alloc 段才回退文件读。 */
        unsigned char *data = (unsigned char *)malloc(hit->sh_size);
        if (!data) goto out;
        if (hit->sh_addr)
            memcpy(data, (void *)((uintptr_t)di.dli_fbase + hit->sh_addr), hit->sh_size);
        else if (lseek(fd, (off_t)hit->sh_offset, SEEK_SET) < 0 || read(fd, data, hit->sh_size) != (ssize_t)hit->sh_size) { free(data); goto out; }
        for (int i = 0; i < 8; i++) if (!cache[i].data) { strcpy(cache[i].key, key); cache[i].data = data; cache[i].size = (unsigned long)hit->sh_size; break; }
        result = data;
        if (size) *size = (unsigned long)hit->sh_size;
    }
out:
    close(fd);
    return result;
}

// ---- mach 自进程内存操作：全部退化为直接内存访问 ----
typedef int kern_return_t;
typedef unsigned long long mach_vm_address_t;
typedef unsigned long long mach_vm_size_t;
typedef unsigned int mach_port_t;
typedef unsigned int mach_msg_type_number_t;
typedef unsigned int vm_region_flavor_t;
typedef unsigned int vm_prot_t;
typedef unsigned int *vm_region_info_t;
typedef unsigned int natural_t;
#define KERN_SUCCESS 0
#define KERN_INVALID_ADDRESS 1
#define KERN_NO_SPACE 3
#define VM_REGION_SUBMAP_INFO_COUNT_64 19
#define vm_page_size 4096 /* 诊断近似；OHOS 真实页大小周末校正 */
typedef struct {
    vm_prot_t protection;
    vm_prot_t max_protection;
    unsigned int inheritance, reserved;
    unsigned long long offset;
    unsigned int pages_resident, pages_shared_now_private, pages_shared_private, pages_dirtied;
    unsigned int depth, copy_strategy, pages_swapped_out;
} vm_region_submap_info_data_64_t;
typedef vm_region_submap_info_data_64_t *vm_region_recurse_info_t;
// 子图递归查询（内存诊断）：OHOS 无等价 → 失败，region_pages 空转、报告全 0
static inline kern_return_t vm_region_recurse_64(mach_vm_address_t task, mach_vm_address_t *addr,
                                                 mach_vm_size_t *size, natural_t *depth,
                                                 vm_region_recurse_info_t info,
                                                 mach_msg_type_number_t *count) {
    (void)task; (void)addr; (void)size; (void)depth; (void)info; (void)count;
    return KERN_INVALID_ADDRESS;
}
#define KERN_SUCCESS 0
#define KERN_INVALID_ADDRESS 1
#define VM_REGION_BASIC_INFO_64 9
#define VM_REGION_BASIC_INFO_COUNT_64 16
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_PROT_ALL 7
#define VM_FLAGS_FIXED 0x0000
#define VM_FLAGS_ANYWHERE 0x0001
#define VM_INHERIT_DEFAULT 1
#define VM_INHERIT_COPY 2
#define MEMORY_OBJECT_NULL ((mach_port_t)0)
#define MACH_PORT_NULL ((mach_port_t)0)
#define FALSE 0
#define TRUE 1
#define KERN_NO_SPACE 3

typedef struct {
    vm_prot_t protection;
    vm_prot_t max_protection;
    unsigned int inheritance, shared, reserved;
    mach_port_t behavior;
    unsigned short user_wired_count;
} vm_region_basic_info_data_64_t;

// region 查询：真实实现，解析 /proc/self/maps（宿主内存布局探测依赖它）。
// 2026-10-30 真机 check_permissions 在此 FAIL（模拟器正常）：三种死法各一次性报声，定位用。
static inline kern_return_t mach_vm_region(mach_vm_address_t task, mach_vm_address_t *addr,
                                           mach_vm_size_t *size, vm_region_flavor_t flavor,
                                           vm_region_info_t info, mach_msg_type_number_t *count,
                                           mach_port_t *object) {
    (void)task; (void)flavor; (void)count; (void)object;
    if (object) *object = MACH_PORT_NULL;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) {
        static int warned;
        if (!warned) { warned = 1; fprintf(stderr, "[totk] note: /proc/self/maps unavailable (app 域被拒?)\n"); }
        return KERN_INVALID_ADDRESS;
    }
    char line[512];
    kern_return_t kr = KERN_INVALID_ADDRESS;
    while (fgets(line, sizeof line, f)) {
        unsigned long long lo, hi;
        char perms[8];
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
        if ((mach_vm_address_t)hi <= *addr) continue;
        if ((mach_vm_address_t)lo > *addr) {   // 空隙：返回 KERN_INVALID_ADDRESS（调用方据此刻画空隙）
            static int gap_warned;
            if (!gap_warned) { gap_warned = 1; fprintf(stderr, "[totk] note: maps gap: query %#llx falls before next region %#llx\n",
                                                       (unsigned long long)*addr, lo); }
            break;
        }
        vm_region_basic_info_data_64_t *bi = (vm_region_basic_info_data_64_t *)(void *)info;
        bi->protection = (perms[0] == 'r' ? VM_PROT_READ : 0) | (perms[1] == 'w' ? VM_PROT_WRITE : 0) |
                         (perms[2] == 'x' ? VM_PROT_EXECUTE : 0);
        bi->max_protection = VM_PROT_ALL;
        if (lo != (unsigned long long)*addr) {
            static int mis_warned;
            if (!mis_warned) { mis_warned = 1; fprintf(stderr, "[totk] note: region start %#llx != requested %#llx (contains)\n",
                                                       lo, (unsigned long long)*addr); }
        }
        *addr = lo; *size = hi - lo;
        kr = KERN_SUCCESS;
        break;
    }
    fclose(f);
    return kr;
}

// mach_vm_map：PROT_NONE 预留走 mmap；FIXED 语义用 MAP_FIXED_NOREPLACE（已占用则失败，与原语义一致）
#include <sys/mman.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
static inline kern_return_t mach_vm_map(mach_vm_address_t task, mach_vm_address_t *addr,
                                        mach_vm_size_t size, mach_vm_address_t mask, int flags,
                                        mach_port_t obj, unsigned long long offset, int copy,
                                        vm_prot_t cur, vm_prot_t max, unsigned int inh) {
    (void)task; (void)mask; (void)obj; (void)offset; (void)copy; (void)max; (void)inh;
    void *hint = (flags & VM_FLAGS_FIXED) ? (void *)(uintptr_t)*addr : NULL;
    int mflags = MAP_ANON | MAP_PRIVATE | ((flags & VM_FLAGS_FIXED) ? MAP_FIXED_NOREPLACE : 0);
    void *p = mmap(hint, size, PROT_NONE, mflags, -1, 0);
    if (p == MAP_FAILED) return KERN_INVALID_ADDRESS;
    *addr = (mach_vm_address_t)(uintptr_t)p;
    (void)cur;
    return KERN_SUCCESS;
}
static inline kern_return_t mach_vm_deallocate(mach_vm_address_t task, mach_vm_address_t addr, mach_vm_size_t size) {
    (void)task;
    return munmap((void *)(uintptr_t)addr, size) == 0 ? KERN_SUCCESS : KERN_INVALID_ADDRESS;
}
static inline kern_return_t mach_port_deallocate(mach_vm_address_t task, mach_port_t p) { (void)task; (void)p; return KERN_SUCCESS; }


static inline kern_return_t mach_vm_read_overwrite(mach_vm_address_t task, mach_vm_address_t addr,
                                                   mach_vm_size_t size, mach_vm_address_t buf,
                                                   mach_vm_size_t *outsize) {
    (void)task;
    __builtin_memcpy((void *)(uintptr_t)buf, (const void *)(uintptr_t)addr, size);
    if (outsize) *outsize = size;
    return KERN_SUCCESS;
}
#define mach_task_self() 0
#define vm_read_overwrite mach_vm_read_overwrite
typedef mach_vm_address_t vm_address_t;
typedef unsigned int *vm_region_info_t;

// ---- mach 绝对时间（纳秒基）----
#include <time.h>
static inline unsigned long long mach_absolute_time(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
static inline double mach_timebase_info_factor(void) { return 1.0; }
typedef struct { unsigned int numer, denom; } mach_timebase_info_data_t;
static inline int mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = 1; t->denom = 1; return 0; }
/* nn::time 稳态时钟用（唯一调用点 platform/time_host.c，定义在 platform/ohos_compat/ohos_stub.c）：
   必须 64 位原型——此前全树无原型走隐式 int，arm64 上 64 位纳秒返回值被截断，
   设备运行超约 2.147s 即溢出为垃圾（审校修复）。类型用 uint64_t 与 ohos_stub.c 的定义
   完全一致（LP64 musl 上 uint64_t=unsigned long，写 unsigned long long 会与其冲突）。 */
uint64_t mach_continuous_time(void);

// clock_gettime_nsec_np：宿主取单调纳秒（诊断打点用）——须在 mach_absolute_time 之后
static inline unsigned long long compat_clock_ns(int clk) { (void)clk; return mach_absolute_time(); }
#define clock_gettime_nsec_np(clk) compat_clock_ns(clk)

// ---- BSD/Apple 杂项 ----
#define MAP_JIT 0
#include <sys/mman.h>
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
/* ---- pthread QoS：原空桩（set 恒 0 / get 恒 DEFAULT）改真实现，定义在 ohos_stub.c ----
   dlopen libqos.so 解析 OH_QoS_SetThreadQoS / OH_QoS_ResetThreadQoS / OH_QoS_GetThreadQoS
   （sysroot qos/qos.h，KernelEnhanceKit API 12+），Apple QOS_CLASS_* 四级映射 OHOS QoS_Level；
   libqos 缺失时打一行日志后安全退化（set 返 -1，get 报 UNSPECIFIED）。声明取代 static inline：
   dlopen 缓存与退化日志全进程一份，不能每翻译单元各持一份。
   调用点：platform/threads.c 客体线程 QoS（donor dm/host/hle_thread.c:287-292 同构）、
   audio_renderer / nvn_mtl 宿主线程。 */
typedef int qos_class_t;
int pthread_set_qos_class_self_np(int cls, int rel);
int pthread_get_qos_class_np(void *tid, qos_class_t *cls, int *rel);
#define QOS_CLASS_UNSPECIFIED -1
#define QOS_CLASS_USER_INTERACTIVE 0
#define QOS_CLASS_USER_INITIATED 1
#define QOS_CLASS_DEFAULT 2
#define QOS_CLASS_UTILITY 3

static inline int proc_pidpath(int pid, char *buf, size_t len) {
    (void)pid;
    ssize_t n = readlink("/proc/self/exe", buf, len - 1);
    if (n <= 0) return 0;
    buf[n] = 0; return (int)n;
}

// 指令缓存刷新（自修改代码/重定位补丁后）
static inline void sys_icache_invalidate(void *start, size_t len) {
#ifndef __aarch64__
    (void)start; (void)len;
#else
    // aarch64：用户态 IC IVAU + DSB/ISB（OHOS musl 无 __clear_cache 时用内联汇编）
    __builtin___clear_cache((char *)start, (char *)start + len);
#endif
}


#endif  // M43A_REC_TU
#endif  // !__ASSEMBLER__