// ohos_stub.c —— shim 契约的实现：SHA-256 真实现；AudioQueue/VT/CF 失败返回（周末接 OHOS 原生）
#include <AudioToolbox/AudioToolbox.h>
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <VideoToolbox/VideoToolbox.h>
#include <execinfo.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

// ---------- SHA-256（公有域实现，loader 哈希校验用） ----------
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

#define ROR32(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

int CC_SHA256_Init(CC_SHA256_CTX *c) {
    c->datalen = 0; c->bitlen = 0;
    c->state[0]=0x6a09e667; c->state[1]=0xbb67ae85; c->state[2]=0x3c6ef372; c->state[3]=0xa54ff53a;
    c->state[4]=0x510e527f; c->state[5]=0x9b05688c; c->state[6]=0x1f83d9ab; c->state[7]=0x5be0cd19;
    return 1;
}

static void sha256_transform(CC_SHA256_CTX *c, const uint8_t *p) {
    uint32_t m[64], a,b,cc,d,e,f,g,h,t1,t2;
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)p[i*4]<<24 | (uint32_t)p[i*4+1]<<16 | (uint32_t)p[i*4+2]<<8 | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(m[i-15],7) ^ ROR32(m[i-15],18) ^ (m[i-15] >> 3);
        uint32_t s1 = ROR32(m[i-2],17) ^ ROR32(m[i-2],19) ^ (m[i-2] >> 10);
        m[i] = m[i-16] + s0 + m[i-7] + s1;
    }
    a=c->state[0]; b=c->state[1]; cc=c->state[2]; d=c->state[3];
    e=c->state[4]; f=c->state[5]; g=c->state[6]; h=c->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e,6) ^ ROR32(e,11) ^ ROR32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + K256[i] + m[i];
        uint32_t S0 = ROR32(a,2) ^ ROR32(a,13) ^ ROR32(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->state[0]+=a; c->state[1]+=b; c->state[2]+=cc; c->state[3]+=d;
    c->state[4]+=e; c->state[5]+=f; c->state[6]+=g; c->state[7]+=h;
}

int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len) {
        size_t take = 64 - c->datalen < len ? 64 - c->datalen : len;
        memcpy(c->data + c->datalen, p, take);
        c->datalen += (uint32_t)take; p += take; len -= take;
        if (c->datalen == 64) { sha256_transform(c, c->data); c->bitlen += 512; c->datalen = 0; }
    }
    return 1;
}

int CC_SHA256_Final(uint8_t *md, CC_SHA256_CTX *c) {
    size_t i = c->datalen;
    c->bitlen += (uint64_t)c->datalen * 8;
    c->data[i++] = 0x80;
    if (i > 56) { while (i < 64) c->data[i++] = 0; sha256_transform(c, c->data); i = 0; }
    while (i < 56) c->data[i++] = 0;
    for (int j = 7; j >= 0; j--) c->data[i++] = (uint8_t)(c->bitlen >> (j * 8));
    sha256_transform(c, c->data);
    for (int j = 0; j < 8; j++) {
        md[j*4]   = (uint8_t)(c->state[j] >> 24);
        md[j*4+1] = (uint8_t)(c->state[j] >> 16);
        md[j*4+2] = (uint8_t)(c->state[j] >> 8);
        md[j*4+3] = (uint8_t)c->state[j];
    }
    return 1;
}

// ---------- AudioQueue：失败返回，audio_out/audio_mixer 走退化分支 ----------
OSStatus AudioQueueNewOutput(const AudioStreamBasicDescription *fmt, AudioQueueOutputCallback cb,
                             void *userData, void *runLoop, void *runLoopMode, unsigned int flags,
                             AudioQueueRef *out) {
    (void)fmt; (void)cb; (void)userData; (void)runLoop; (void)runLoopMode; (void)flags;
    if (out) *out = NULL;
    fprintf(stderr, "[ohos-shim] AudioQueueNewOutput -> not implemented (weekend: oh_audio)\n");
    return kAudioQueueErr_CannotStart;
}
OSStatus AudioQueueDispose(AudioQueueRef aq, int immediately) { (void)aq; (void)immediately; return 0; }
OSStatus AudioQueueAllocateBuffer(AudioQueueRef aq, unsigned int bytes, AudioQueueBufferRef *out) {
    (void)aq; (void)bytes; if (out) *out = NULL; return -1;
}
OSStatus AudioQueueFreeBuffer(AudioQueueRef aq, AudioQueueBufferRef buf) { (void)aq; (void)buf; return 0; }
OSStatus AudioQueueEnqueueBuffer(AudioQueueRef aq, AudioQueueBufferRef buf, unsigned long numParam,
                                 const AudioQueueParam *params) {
    (void)aq; (void)buf; (void)numParam; (void)params; return -1;
}
OSStatus AudioQueueStart(AudioQueueRef aq, void *startTime) { (void)aq; (void)startTime; return -1; }
OSStatus AudioQueuePause(AudioQueueRef aq) { (void)aq; return -1; }
OSStatus AudioQueueReset(AudioQueueRef aq) { (void)aq; return -1; }
OSStatus AudioQueueSetParameter(AudioQueueRef aq, AudioQueueParam param, double value) {
    (void)aq; (void)param; (void)value; return -1;
}

// ---------- CoreFoundation ----------
CFStringRef CFStringCreateWithCString(void *alloc, const char *cStr, CFStringEncoding enc) {
    (void)alloc; (void)enc;
    return (CFStringRef)strdup(cStr ? cStr : "");
}
int CFStringGetCString(CFStringRef s, char *buffer, CFIndex bufferSize, CFStringEncoding enc) {
    (void)enc;
    if (!s || !buffer || bufferSize <= 0) return 0;
    strncpy(buffer, (const char *)s, (size_t)bufferSize - 1);
    buffer[bufferSize - 1] = 0;
    return 1;
}
CFIndex CFStringGetLength(CFStringRef s) { return s ? (CFIndex)strlen((const char *)s) : 0; }
void CFRelease(void *cf) { free(cf); }

// ---------- VideoToolbox：失败返回，movie_host 退化（周末 AVCodec 软解） ----------
OSStatus VTDecompressionSessionCreate(void *allocator, void *formatDesc, void *videoDecoderSpecification,
                                      void *destinationImageBufferAttributes,
                                      const VTDecompressionOutputCallbackRecord *callback,
                                      VTDecompressionSessionRef *out) {
    (void)allocator; (void)formatDesc; (void)videoDecoderSpecification;
    (void)destinationImageBufferAttributes; (void)callback;
    if (out) *out = NULL;
    fprintf(stderr, "[ohos-shim] VTDecompressionSessionCreate -> not implemented (weekend: AVCodec)\n");
    return -1;
}
OSStatus VTDecompressionSessionDecodeFrame(VTDecompressionSessionRef s, void *sampleBuffer, void *flags,
                                           void *infoFlagsOut) {
    (void)s; (void)sampleBuffer; (void)flags; (void)infoFlagsOut; return -1;
}
void VTDecompressionSessionInvalidate(VTDecompressionSessionRef s) { (void)s; }
OSStatus VTDecompressionSessionWaitForAsynchronousFrames(VTDecompressionSessionRef s) { (void)s; return -1; }

// ---------- execinfo ----------
int backtrace(void **array, int size) { (void)array; (void)size; return 0; }
char **backtrace_symbols(void *const *array, int size) { (void)array; (void)size; return NULL; }
void backtrace_symbols_fd(void *const *array, int size, int fd) { (void)array; (void)size; (void)fd; }

// ---------- posix_spawn：watchdog 降级 no-op ----------
int posix_spawn_file_actions_init(void *fa) { (void)fa; return 0; }
int posix_spawn_file_actions_addclose(void *fa, int fd) { (void)fa; (void)fd; return 0; }
int posix_spawn_file_actions_destroy(void *fa) { (void)fa; return 0; }
int posix_spawnattr_init(void *attr) { (void)attr; return 0; }
int posix_spawnattr_setflags(void *attr, short flags) { (void)attr; (void)flags; return 0; }
int posix_spawn(void *pid, const char *path, const void *fa, const void *attr,
                char *const argv[], char *const envp[]) {
    (void)pid; (void)path; (void)fa; (void)attr; (void)argv; (void)envp;
    fprintf(stderr, "[ohos-shim] posix_spawn -> not implemented (watchdog disabled)\n");
    return -1;
}

// ---------- libproc：进程枚举退化（游戏槽位功能周末重写） ----------
int proc_pidinfo(int pid, int flavor, unsigned long long arg, void *buffer, int buffersize) {
    (void)pid; (void)flavor; (void)arg; (void)buffer; (void)buffersize; return 0;
}

// ---------- AudioUnit / AudioComponent：失败返回，audio_mixer 退化（周末 oh_audio 重写） ----------
AudioComponent AudioComponentFindNext(AudioComponent inRef, const AudioComponentDescription *desc) {
    (void)inRef; (void)desc;
    fprintf(stderr, "[ohos-shim] AudioComponentFindNext -> not implemented (weekend: oh_audio)\n");
    return NULL;
}
OSStatus AudioComponentInstanceNew(AudioComponent comp, AudioComponentInstance *out) {
    (void)comp; if (out) *out = NULL; return kAudioComponentErr_InstanceInvalidated;
}
OSStatus AudioComponentInstanceDispose(AudioComponentInstance inst) { (void)inst; return 0; }
OSStatus AudioUnitInitialize(AudioComponentInstance inst) { (void)inst; return kAudioUnitErr_InvalidProperty; }
OSStatus AudioUnitUninitialize(AudioComponentInstance inst) { (void)inst; return 0; }
OSStatus AudioUnitSetProperty(AudioComponentInstance inst, unsigned int propID, unsigned int scope,
                              unsigned int element, const void *data, unsigned int dataSize) {
    (void)inst; (void)propID; (void)scope; (void)element; (void)data; (void)dataSize;
    return kAudioUnitErr_InvalidProperty;
}
OSStatus AudioUnitGetProperty(AudioComponentInstance inst, unsigned int propID, unsigned int scope,
                              unsigned int element, void *data, unsigned int *dataSize) {
    (void)inst; (void)propID; (void)scope; (void)element; (void)data;
    if (dataSize) *dataSize = 0;
    return kAudioUnitErr_InvalidProperty;
}

// ---------- 二次收口：前端交互层与 NVN 纹理工具的桩（周末实装） ----------
CFLocaleRef CFLocaleCopyCurrent(void) { return NULL; }
CFStringRef CFLocaleGetValue(CFLocaleRef locale, CFStringRef key) { (void)locale; (void)key; return NULL; }
OSStatus AudioOutputUnitStart(AudioComponentInstance inst) { (void)inst; return kAudioUnitErr_InvalidProperty; }
OSStatus AudioOutputUnitStop(AudioComponentInstance inst) { (void)inst; return 0; }
int proc_listallpids(void *buffer, int buffersize) { (void)buffer; (void)buffersize; return 0; }
char _mh_execute_header;
void dm_gfx_anomdump(void) {}
void dm_gfx_gpucapture_next(void) {}
void dm_gfx_passdump_next(void) {}
void dm_gfx_querylog_next(void) {}
void dm_gfx_texdump_next(void) {}
void *nvn_deswizzle(void) { return NULL; }
void runtime_gamepad_clear(void) {}
void runtime_gamepad_type_cycle(void) {}
void runtime_mouse_toggle(void) {}
void runtime_rumble_set(double low, double high) { (void)low; (void)high; }
void runtime_shortcut_help_toggle(void) {}
void taa_toggle(void) {}

// round 1 stubs
// CC_SHA256 一次性接口不能再空桩：host/loader.c:187 与 host/shared_font.c:28 用它做模块/字体完整性校验，
// 空桩导致 digest 永远是栈上垃圾 → 必然 "image SHA256 differs from verified rewrite" → exit(1)。
// 用本文件已有的真实现（Init/Update/Final）拼装，语义对齐 CommonCrypto。
unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md) {
    CC_SHA256_CTX c;
    if (!CC_SHA256_Init(&c) || !CC_SHA256_Update(&c, data, len) || !CC_SHA256_Final(md, &c)) return NULL;
    return md;
}
void dm_gfx_shot_at(void) {}
void dm_gfx_window_shot(void) {}
/* round23：宿主线程栈范围改真实现。原空桩无返回值，services.c:398（svc#6 QueryMemory）在无原型下
   按隐式 int 取返回值——编译为取 x0 低 32 位再符号扩展：宿主线程描述符地址低 32 位带符号位时
   （ASLR 抽签，round17/22 两次命中），top 变成 0xffffffff_xxxxxxxx，svc#6 把所有客体地址报成
   宿主栈 state 0xb，SDK CheckGuardSpace 要求 guard 页 state==0 → CreateAliasStackUnsafe 64 连败
   空消息 Break。musl 的 pthread_getattr_np 取真实 [base,size)；失败返 0，调用侧分支自然不命中。
   原型在 ohos_compat.h（强制注入，调用点可见）。 */
int pthread_getattr_np(pthread_t, pthread_attr_t *);   /* musl _GNU_SOURCE 接口；显式原型防未定义 _GNU_SOURCE 时又落隐式 int */
uintptr_t pthread_get_stackaddr_np(pthread_t t) {
    pthread_attr_t a;
    void *base = NULL; size_t size = 0;
    if (pthread_getattr_np(t, &a) == 0) {
        if (pthread_attr_getstack(&a, &base, &size) != 0) base = NULL;
        pthread_attr_destroy(&a);
    }
    return (uintptr_t)base;
}
size_t pthread_get_stacksize_np(pthread_t t) {
    pthread_attr_t a;
    void *base = NULL; size_t size = 0;
    if (pthread_getattr_np(t, &a) == 0) {
        if (pthread_attr_getstack(&a, &base, &size) != 0) size = 0;
        pthread_attr_destroy(&a);
    }
    return size;
}
void pthread_mach_thread_np(void) {}
void runtime_boot_progress(void) {}
void runtime_boot_status(void) {}
void *runtime_codec_override(const char *name) { (void)name; return NULL; }
void runtime_front_focus(void) {}
void *runtime_front_override(const char *name) { (void)name; return NULL; }
void runtime_front_request_close(void) {}
void runtime_front_run(void) {}
void runtime_front_text_input(void) {}
void runtime_front_text_test(void) {}
void runtime_gamepad_read(void) {}
/* probe 入口（argv probe-* 模式专用，boot 不经此；OHOS 无真身，保链接） */
void runtime_gpu_path_probe(void) {}
void runtime_vpool_probe(void) {}

// round 2 stubs 中的 dm_thread_enter / runtime_thread_leave / runtime_gate 已改为汇编真实现，
// 见 host/entry.S（OHOS/ELF 移植，来源 by3 runtime/gate.S + thread_enter.S）；此处原空桩删除。
// mach_continuous_time 同步改为真实现：nn::time 稳态时钟（host/time_host.c:35）用它计秒，
// 空桩返回 0 会让游戏时间恒停。与 Darwin 语义对齐：连续时间，纳秒基。
uint64_t mach_continuous_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
