#pragma once
/* proc_pidpath/proc_pidinfo shim：OHOS 用 /proc/self/exe，进程枚举退化 */
#include <stdint.h>
#include <stddef.h>
#define PROC_PIDPATHINFO_MAXSIZE 1024
#define PROC_PIDTBSDINFO 3
#define SZOMB 5
struct proc_bsdinfo {
    uint32_t pbi_status;
    uint32_t pbi_flags;
    uint64_t pbi_start_tvsec;
    char pad[228];
};
int proc_pidpath(int pid, char *buf, size_t len);
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
