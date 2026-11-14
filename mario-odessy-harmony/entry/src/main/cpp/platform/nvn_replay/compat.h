/* 兼容垫片：让 infra 的 NVN 源文件在我们树里编译。
   infra 的 HLE_FUNC 会把函数登记进 .hle$m 链接段（他们自己的派发机制）；
   我们只取函数体，注册走我们自己的 gfx_bridge 符号表。 */
#ifndef NVN_PORT_COMPAT_H
#define NVN_PORT_COMPAT_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct Cpu {                 /* 只声明用到的两个成员，布局与我们 hle_stack 不共享 */
    uint64_t x[32];
    uint64_t v[32];
} Cpu;
#define HLE_CAT_(a,b) a##b
#define HLE_CAT(a,b) HLE_CAT_(a,b)
#define HLE_FUNC(sym, fn) void fn(Cpu *c)   /* 非 static：胶水层要能直接调用 infra 的实现 */
/* 日志：接到我们的 stderr（宿主侧 hilog 已由上层接管），语义与 infra 的 rt.h 一致 */
#define LOG(...)   fprintf(stderr, __VA_ARGS__)
#define FATAL(...) (fprintf(stderr, "FATAL: " __VA_ARGS__), abort())
#define ENV(name)  ({ static const char *env_v_; static int env_i_; if (!env_i_) { env_v_ = getenv(name); env_i_ = 1; } env_v_; })
#define GUEST_LO 0x7000000000ull
#define GUEST_HI 0x7320000000ull
#endif
