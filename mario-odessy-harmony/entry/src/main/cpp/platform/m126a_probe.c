/* m126a_probe.c — M126A（2026-11-14）RX 权限判决探针 + seal() 明文取证 + 直接 exec mmap 变体的日志端。
 *
 * 本单要一刀切开的分歧（M125A 已钉死：同一 md5 的 #5 也崩、重启+清 swap 后 #7 仍崩、证书到 2049）：
 *   整个进程拿不到可执行页（设备/签名策略）
 *   vs 只有「我们 patch 出来的匿名页」拿不到（背衬/映射方式/时机）。
 * 所以这里跑一个 5 格矩阵（每格一行，都在 shader/loader 初始化之前）：
 *   anon        : mmap(anon RW, 2 页) → 写一字节 → mprotect(RX)      = seal() 对 patch/text 页做的事
 *   anon-create : mmap(anon R|X) 创建即带 X
 *   file        : mmap(libgame.so 只读页) → mprotect(RX)
 *   file-rw     : mmap(libgame.so, R|W MAP_PRIVATE) → mprotect(RX)   = 最贴近 seal 的 W→X
 *   file-create : mmap(libgame.so, R|X MAP_PRIVATE) 创建即带 X       = smo_exec_mmap=1 用的写法
 * verdict 一行把五格答案并列，报告里直接引用。
 *
 * 为什么这一行一定能在崩溃前打出来：本函数的第一条语句就是 vk_boot_log_open()（vk_boot.c，幂等），
 * 之后每行都是 fprintf(stderr) + vk_boot_logf（后者每行 fflush），与崩溃点（首条 guest 指令，
 * 4 s 后）之间有数秒余量；调用点是 vk_boot.c 的 constructor(101) 第一行（早于 Vulkan/ICD、
 * 早于 nvn_vk.c 的 vk_boot_log_open@1894、早于 loader 的 map_module/seal）。
 *
 * 两个 knob（app 自己先建文件，hdc 只能覆盖已有文件）：
 *   smo_rxprobe   缺省 1：开启即打印；写 0 ⇒ 除 seed 行外一条探针行都不打（判据 5 关态口径）。
 *   smo_exec_mmap 缺省 0：=1 ⇒ map_module 用「文件背衬 + 创建即 R|W|X」，失败回落 build #7 旧路径。
 */
#include "m126a_probe.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern int vk_boot_log_open(void);       /* vk_boot.c：幂等；打开 /data/storage/el2/base/files/vk_boot.log */
extern void vk_boot_logf(const char *fmt, ...);

#define M126A_KNOB_RX_A "/data/storage/el2/base/files/smo_rxprobe"
#define M126A_KNOB_RX_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_rxprobe"
#define M126A_KNOB_EX_A "/data/storage/el2/base/files/smo_exec_mmap"
#define M126A_KNOB_EX_B "/data/app/el2/100/base/com.vibecc.smo/files/smo_exec_mmap"

static int g_m126a_rxprobe = -2;    /* -2 = 未读；0/1 = 已读并缓存 */
static int g_m126a_execmmap = -2;

static size_t m126a_page(void)
{
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (size_t)ps : 4096u;
}

/* stderr（hilog 转发）+ vk_boot.log 双写；第一条语句先保证日志文件已开。
   format(printf) 属性 ⇒ 每个 % 与实参的匹配由 clang 在编译期核对（本单的探针行正是靠它防 M99A 那类事故）。 */
__attribute__((format(printf, 1, 2)))
static void m126a_emit(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", buf);
    fflush(stderr);
    (void)vk_boot_log_open();
    vk_boot_logf("%s", buf);
}

/* /proc/self/maps 里包含 addr 的那一行的 perms（找不到 = maps=none）。 */
static void m126a_perms_for(uint64_t addr, char *out, size_t n)
{
    FILE *f = fopen("/proc/self/maps", "r");
    snprintf(out, n, "maps=none");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long long s = 0, e = 0;
        char perms[8] = { 0 };
        if (sscanf(line, "%llx-%llx %7s", &s, &e, perms) != 3) continue;
        if (addr >= (uint64_t)s && addr < (uint64_t)e) { snprintf(out, n, "perms=%s", perms); break; }
    }
    fclose(f);
}

int m126a_rxprobe_on(void);
int m126a_exec_mmap_on(void);

static int m126a_knob(const char *path_a, const char *path_b, int dflt, const char *name)
{
    const char *p[2] = { path_a, path_b };
    int v = dflt, have = 0;
    for (int i = 0; i < 2 && !have; i++) {
        FILE *f = fopen(p[i], "r");
        if (!f) continue;
        char b[16] = { 0 };
        size_t n = fread(b, 1, sizeof b - 1, f);
        fclose(f);
        have = 1;
        v = (n > 0 && b[0] == '1') ? 1 : 0;
    }
    if (!have) { FILE *f = fopen(path_a, "w"); if (f) { fputs(dflt ? "1\n" : "0\n", f); fclose(f); } }   /* hdc 只能覆盖已有文件 ⇒ app 先建 */
    m126a_emit("[m126a] %s=%d (%s) file=[%s]", name, v, v == 1 ? "ON" : "OFF", path_a);
    return v;
}

int m126a_rxprobe_on(void)
{
    if (g_m126a_rxprobe == -2) g_m126a_rxprobe = m126a_knob(M126A_KNOB_RX_A, M126A_KNOB_RX_B, 1, "smo_rxprobe");
    return g_m126a_rxprobe == 1;
}

int m126a_exec_mmap_on(void)
{
    if (g_m126a_execmmap == -2) g_m126a_execmmap = m126a_knob(M126A_KNOB_EX_A, M126A_KNOB_EX_B, 0, "smo_exec_mmap");
    return g_m126a_execmmap == 1;
}

/* 文件背衬的一格：prot 是 mmap 时的权限；do_mprotect=1 时再看 mprotect(RX) 能不能成功。
   返回 1 = 这一格最终拿到可执行页。 */
static int m126a_file_try(const char *tag, const char *path, int fd, size_t len, unsigned prot, int do_mprotect)
{
    char perms[64] = "maps=none";
    errno = 0;
    void *p = mmap(NULL, len, (int)prot, MAP_PRIVATE, fd, 0);
    int e = errno;
    if (p == MAP_FAILED) {
        m126a_emit("[m126a] rxprobe %s ok=0 path=%s len=0x%zx prot=0x%x errno=%d(%s) stage=mmap",
                   tag, path, len, prot, e, strerror(e));
        return 0;
    }
    int rc = 0;
    if (do_mprotect) {
        errno = 0;
        rc = mprotect(p, len, PROT_READ | PROT_EXEC);
        e = errno;
    }
    int ok = do_mprotect ? (rc == 0) : 1;
    m126a_perms_for((uint64_t)(uintptr_t)p, perms, sizeof perms);
    m126a_emit("[m126a] rxprobe %s ok=%d path=%s addr=0x%llx len=0x%zx prot=0x%x errno=%d(%s) rc=%d %s",
               tag, ok, path, (unsigned long long)(uintptr_t)p, len,
               do_mprotect ? (unsigned)(PROT_READ | PROT_EXEC) : prot, e, strerror(e), rc, perms);
    munmap(p, len);
    return ok;
}

void m126a_rxprobe_early(void)
{
    static int done;
    if (done) return;
    done = 1;
    (void)vk_boot_log_open();                 /* 最早打开取证文件：探针绝不因「stderr 被丢/日志未开」而丢 */
    if (!m126a_rxprobe_on()) return;
    const size_t len = 2 * m126a_page();      /* 2 页 */
    int anon_ok = 0, anon_create_ok = 0, file_ok = 0, file_rw_ok = 0, file_create_ok = 0;

    /* ① 匿名 RW → mprotect RX：与 seal() 对 patch/text 页做的事完全同型（页先被写过，再 W→X） */
    {
        errno = 0;
        void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        int e = errno;
        if (p == MAP_FAILED) {
            m126a_emit("[m126a] rxprobe anon ok=0 addr=0x0 len=0x%zx errno=%d(%s) stage=mmap", len, e, strerror(e));
        } else {
            *(volatile unsigned char *)p = 0x5a;   /* 先把页碰实（seal 的页在 RW 阶段也都被写过） */
            char perms[64];
            errno = 0;
            int rc = mprotect(p, len, PROT_READ | PROT_EXEC);
            e = errno;
            m126a_perms_for((uint64_t)(uintptr_t)p, perms, sizeof perms);
            anon_ok = (rc == 0);
            m126a_emit("[m126a] rxprobe anon ok=%d addr=0x%llx len=0x%zx errno=%d(%s) prot=0x5 rc=%d %s",
                       anon_ok, (unsigned long long)(uintptr_t)p, len, e, strerror(e), rc, perms);
            munmap(p, len);
        }
    }
    /* ② 匿名「创建即带 X」 */
    {
        errno = 0;
        void *p = mmap(NULL, len, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
        int e = errno;
        if (p != MAP_FAILED) { anon_create_ok = 1; munmap(p, len); }
        m126a_emit("[m126a] rxprobe anon-create ok=%d len=0x%zx errno=%d(%s)", anon_create_ok, len, e, strerror(e));
    }
    /* ③④⑤ 文件背衬：拿本模块 libgame.so 自己的路径（dladdr，不硬编码安装路径） */
    {
        Dl_info di;
        const char *path = NULL;
        if (dladdr((void *)(uintptr_t)&m126a_rxprobe_early, &di) && di.dli_fname && di.dli_fname[0]) path = di.dli_fname;
        errno = 0;
        int fd = path ? open(path, O_RDONLY) : -1;
        int oe = errno;
        if (fd < 0) {
            m126a_emit("[m126a] rxprobe file ok=0 path=%s errno=%d(%s) stage=open", path ? path : "(null)", oe, strerror(oe));
        } else {
            m126a_emit("[m126a] rxprobe file path=%s (本模块 libgame.so 自身)", path);
            file_ok = m126a_file_try("file", path, fd, len, PROT_READ, 1);                             /* 只读页 → RX */
            file_rw_ok = m126a_file_try("file-rw", path, fd, len, PROT_READ | PROT_WRITE, 1);          /* 可写文件页 → RX */
            file_create_ok = m126a_file_try("file-create", path, fd, len, PROT_READ | PROT_EXEC, 0);   /* 创建即 RX */
            close(fd);
        }
    }
    m126a_emit("[m126a] rxprobe verdict anon=%d anon_create=%d file=%d file_rw=%d file_create=%d knob=1 pid=%d",
               anon_ok, anon_create_ok, file_ok, file_rw_ok, file_create_ok, (int)getpid());
    m126a_emit("[m126a] rxprobe reading anon=1&&file_rw=1 => 进程能拿可执行页（问题在背衬/时机）；"
               "anon=0&&file_create=0 => 设备/签名策略对本进程整体禁执行");
}

void m126a_seal_args(const char *what, uint64_t addr, uint64_t len, unsigned prot, int rc, int err, uint64_t mmap_base)
{
    if (!m126a_rxprobe_on()) return;
    m126a_emit("[m126a] seal what=%s addr=0x%llx len=0x%llx prot=0x%x mmap=0x%llx rc=%d errno=%d(%s)%s",
               what, (unsigned long long)addr, (unsigned long long)len, prot,
               (unsigned long long)mmap_base, rc, err, strerror(err), rc == 0 ? "" : " -> maps dump");
    if (rc != 0) m126a_maps_dump(what, addr, len);
}

void m126a_maps_dump(const char *tag, uint64_t addr, uint64_t len)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { m126a_emit("[m126a] maps tag=%s open FAILED errno=%d(%s)", tag, errno, strerror(errno)); return; }
    char line[512];
    uint64_t end = addr + (len ? len : 1);
    int hit = 0, printed = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long s = 0, e = 0;
        if (sscanf(line, "%llx-%llx", &s, &e) != 2) continue;
        if ((uint64_t)s >= end || (uint64_t)e <= addr) continue;
        hit++;
        if (printed < 8) {
            size_t n = strlen(line);
            while (n && line[n - 1] == '\n') line[--n] = 0;
            m126a_emit("[m126a] maps#%d %s", printed, line);
            printed++;
        }
    }
    fclose(f);
    m126a_emit("[m126a] maps tag=%s addr=0x%llx len=0x%llx hit=%d printed=%d%s",
               tag, (unsigned long long)addr, (unsigned long long)len, hit, printed,
               hit > printed ? " (only first 8)" : "");
}

void m126a_execmap_summary(int fallback, int demote_fail)
{
    if (!m126a_rxprobe_on()) return;
    m126a_emit("[m126a] execmap summary fallback=%d demote_fail=%d (fallback>0 => 文件背衬创建即 X 被拒，已回落 build #7 路径)",
               fallback, demote_fail);
}

void m126a_execmap_note(const char *mod, const char *path, uint64_t base, uint64_t len,
                        unsigned prot, int ok, int err, int fallback, uint64_t reserve)
{
    if (!m126a_rxprobe_on()) return;
    m126a_emit("[m126a] execmap mod=%s ok=%d mmap=0x%llx reserve=0x%llx len=0x%llx prot=0x%x fallback=%d errno=%d(%s) path=%s",
               mod, ok, (unsigned long long)base, (unsigned long long)reserve,
               (unsigned long long)len, prot, fallback, err, strerror(err), path);
}
