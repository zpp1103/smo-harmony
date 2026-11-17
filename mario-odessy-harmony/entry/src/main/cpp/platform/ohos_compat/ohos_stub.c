// ohos_stub.c —— shim 契约的实现：SHA-256 真实现；AudioUnit=虚拟默认输出设备（round7 实装）；
//                 AudioQueue/VT/CF 失败返回（周末接 OHOS 原生）
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

// ---------- AudioUnit / AudioComponent：虚拟默认输出设备（round7 实装，真模拟语义） ----------
/* 语义：AudioComponentFindNext 对 kAudioUnitType_Output 返回哨兵组件（=设备表枚举到默认输出设备）；
   AudioComponentInstanceNew 分配实例；AudioUnitSetProperty 记录 StreamFormat（audio_mixer.c:148 的
   48kHz float 立体声）与 SetRenderCallback（:149 存 {pull,mixer}）；AudioOutputUnitStart 起 host 线程
   每 512/rate 秒（48k≈10.67ms）以 512 帧立体声调存的回调——这就是虚拟声卡时钟：callback frames
   心跳、prefill/underrun/主环形缓冲全部真活（pull 在 !playing/prefill/underrun 时本就 memset 静音帧，
   audio_mixer.c:117/:123/:135，静音走既有路径；audio_mixer.c 设备路径零改动，unit_ok :155 自然置位）。
   出口两条：有音频 HAL 的设备把帧送 OH_AudioRenderer（NDK libohaudio）——API20 SDK 无 push 式
   OH_AudioRenderer_Write、只有回调式写端，故 ticker 产帧入二级小环（单生产单消费原子索引，与主环
   同法）、HAL 写回调消费，饥饿喂零（声卡侧静音，不动 mixer 侧计量）；模拟器/无 HAL 丢弃（null sink）。
   设备时钟独立于游戏线程、宁可静音不欠采样，与 Ryujinx SoundIoDevice / SDL audio device 同构。 */
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <ohaudio/native_audiostreambuilder.h>
int pthread_setname_np(pthread_t, const char *);   /* musl _GNU_SOURCE 接口，显式原型防隐式 int（同下方 pthread_getattr_np 注） */

struct OpaqueAudioComponent { unsigned present; };   /* 哨兵组件：默认输出设备存在 */
static struct OpaqueAudioComponent au_default_output = {1};
typedef OSStatus (*AURenderCb)(void *, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32, AudioBufferList *);
#define AU_PERIOD_FRAMES 512u
#define AU_SINK_FRAMES 4096u
struct OpaqueAudioComponentInstance {
    AudioStreamBasicDescription fmt; AURenderCallbackStruct cb;   /* shim 布局 {input=回调, proc=ctx}（audio_mixer.c:149 {pull,m}） */
    pthread_t thread; _Atomic int running; int started;
    OH_AudioStreamBuilder *builder; OH_AudioRenderer *renderer;
    _Atomic uint64_t srd, swr; float sink_ring[AU_SINK_FRAMES * 2];
};
AudioComponent AudioComponentFindNext(AudioComponent inRef, const AudioComponentDescription *desc) {
    (void)inRef;
    if (desc && desc->componentType == kAudioUnitType_Output) return &au_default_output;
    return NULL;
}
OSStatus AudioComponentInstanceNew(AudioComponent comp, AudioComponentInstance *out) {
    if (!comp || !out) return kAudioComponentErr_InstanceInvalidated;
    *out = calloc(1, sizeof(struct OpaqueAudioComponentInstance));
    if (!*out) return kAudioComponentErr_InstanceInvalidated;
    return noErr;
}
OSStatus AudioComponentInstanceDispose(AudioComponentInstance inst) {
    if (!inst) return 0;
    AudioOutputUnitStop(inst);
    free(inst);
    return 0;
}
OSStatus AudioUnitInitialize(AudioComponentInstance inst) { return inst ? noErr : kAudioUnitErr_InvalidProperty; }
OSStatus AudioUnitUninitialize(AudioComponentInstance inst) { (void)inst; return 0; }
OSStatus AudioUnitSetProperty(AudioComponentInstance inst, unsigned int propID, unsigned int scope,
                              unsigned int element, const void *data, unsigned int dataSize) {
    (void)scope; (void)element;
    struct OpaqueAudioComponentInstance *u = (struct OpaqueAudioComponentInstance *)inst;
    if (!u || !data) return kAudioUnitErr_InvalidProperty;
    if (propID == kAudioUnitProperty_StreamFormat && dataSize >= sizeof(AudioStreamBasicDescription)) {
        u->fmt = *(const AudioStreamBasicDescription *)data; return noErr;
    }
    if (propID == kAudioUnitProperty_SetRenderCallback && dataSize >= sizeof(AURenderCallbackStruct)) {
        u->cb = *(const AURenderCallbackStruct *)data; return noErr;
    }
    return noErr;   /* 其余属性 benign 接受（真 CoreAudio 常见成功面），不阻塞 setup 链 */
}
OSStatus AudioUnitGetProperty(AudioComponentInstance inst, unsigned int propID, unsigned int scope,
                              unsigned int element, void *data, unsigned int *dataSize) {
    (void)inst; (void)propID; (void)scope; (void)element; (void)data;
    if (dataSize) *dataSize = 0;
    return kAudioUnitErr_InvalidProperty;
}
/* ticker → 二级环 → HAL 写回调（null sink 即直接丢弃，mixer 侧计量不受影响） */
static void au_sink_push(struct OpaqueAudioComponentInstance *u, const float *lr, UInt32 frames) {
    if (!u->renderer) return;   /* 模拟器/无 HAL：null sink */
    uint64_t wr = atomic_load_explicit(&u->swr, memory_order_relaxed);
    uint64_t rd = atomic_load_explicit(&u->srd, memory_order_acquire);
    if (wr + frames - rd > AU_SINK_FRAMES) { rd = wr + frames - AU_SINK_FRAMES; atomic_store_explicit(&u->srd, rd, memory_order_release); }   /* 溢出丢最旧 */
    for (UInt32 i = 0; i < frames; i++) { uint64_t k = (wr + i) % AU_SINK_FRAMES; u->sink_ring[2 * k] = lr[2 * i]; u->sink_ring[2 * k + 1] = lr[2 * i + 1]; }
    atomic_store_explicit(&u->swr, wr + frames, memory_order_release);
}
static OH_AudioData_Callback_Result au_hal_write(OH_AudioRenderer *renderer, void *userData,
                                                 void *audioData, int32_t audioDataSize) {
    (void)renderer;
    struct OpaqueAudioComponentInstance *u = userData;
    float *out = audioData;
    if (!u || audioDataSize <= 0 || audioDataSize % (int32_t)(2 * sizeof(float))) {
        memset(audioData, 0, audioDataSize > 0 ? (size_t)audioDataSize : 0);
        return AUDIO_DATA_CALLBACK_RESULT_INVALID;
    }
    int32_t frames = audioDataSize / (int32_t)(2 * sizeof(float));
    uint64_t rd = atomic_load_explicit(&u->srd, memory_order_acquire);
    uint64_t wr = atomic_load_explicit(&u->swr, memory_order_acquire);
    uint64_t have = wr - rd, n = (uint64_t)frames < have ? (uint64_t)frames : have;
    for (uint64_t i = 0; i < n; i++) { uint64_t k = (rd + i) % AU_SINK_FRAMES; out[2 * i] = u->sink_ring[2 * k]; out[2 * i + 1] = u->sink_ring[2 * k + 1]; }
    if (n < (uint64_t)frames) memset(out + 2 * n, 0, (size_t)(frames - n) * 2 * sizeof(float));   /* 二级环饥饿：声卡侧静音 */
    atomic_store_explicit(&u->srd, rd + n, memory_order_release);
    return AUDIO_DATA_CALLBACK_RESULT_VALID;
}
static void au_sink_open(struct OpaqueAudioComponentInstance *u) {
    OH_AudioStreamBuilder *b = NULL;
    if (OH_AudioStreamBuilder_Create(&b, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS || !b) goto none;
    int32_t rate = u->fmt.mSampleRate > 0 ? (int32_t)(u->fmt.mSampleRate + 0.5) : 48000;
    OH_AudioStreamBuilder_SetSamplingRate(b, rate);
    OH_AudioStreamBuilder_SetChannelCount(b, 2);   /* 主环/pull 固定立体声（audio_mixer.c:148） */
    OH_AudioStreamBuilder_SetSampleFormat(b, AUDIOSTREAM_SAMPLE_F32LE);
    OH_AudioStreamBuilder_SetLatencyMode(b, AUDIOSTREAM_LATENCY_MODE_FAST);
    OH_AudioStreamBuilder_SetRendererInfo(b, AUDIOSTREAM_USAGE_GAME);
    if (OH_AudioStreamBuilder_SetRendererWriteDataCallback(b, au_hal_write, u) != AUDIOSTREAM_SUCCESS) { OH_AudioStreamBuilder_Destroy(b); goto none; }
    OH_AudioRenderer *r = NULL;
    if (OH_AudioStreamBuilder_GenerateRenderer(b, &r) == AUDIOSTREAM_SUCCESS && r &&
        OH_AudioRenderer_Start(r) == AUDIOSTREAM_SUCCESS) {
        u->builder = b; u->renderer = r;
        fprintf(stderr, "[ohos-audio] OHAudio renderer %d Hz stereo (game/fast)\n", rate);
        return;
    }
    if (r) OH_AudioRenderer_Release(r);
    OH_AudioStreamBuilder_Destroy(b);
none:
    fprintf(stderr, "[ohos-audio] no OHAudio output: null sink (emulator / no audio HAL)\n");
}
static void *au_ticker(void *arg) {
    struct OpaqueAudioComponentInstance *u = arg;
    pthread_setname_np(pthread_self(), "au-ticker");
    double rate = u->fmt.mSampleRate > 0 ? u->fmt.mSampleRate : 48000.0;
    uint64_t irate = rate >= 1 ? (uint64_t)(rate + 0.5) : 48000;
    float *scratch = malloc(AU_PERIOD_FRAMES * 2 * sizeof(float));
    if (!scratch) return NULL;
    AudioBufferList io = {1, {{2, AU_PERIOD_FRAMES * 2 * (UInt32)sizeof(float), scratch}, {0, 0, NULL}}};
    AURenderCb fn = (AURenderCb)u->cb.input;
    struct timespec base; clock_gettime(CLOCK_MONOTONIC, &base);
    uint64_t consumed = 0;   /* 分子=已回调帧数×1e9，有理数时钟：48000 帧精确 1 秒，无长期漂移 */
    while (atomic_load_explicit(&u->running, memory_order_acquire)) {
        consumed += (uint64_t)AU_PERIOD_FRAMES * 1000000000ull;
        uint64_t at = consumed / irate;
        struct timespec dl = { base.tv_sec + (time_t)(at / 1000000000ull), base.tv_nsec + (long)(at % 1000000000ull) };
        if (dl.tv_nsec >= 1000000000L) { dl.tv_nsec -= 1000000000L; dl.tv_sec++; }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > dl.tv_sec || (now.tv_sec == dl.tv_sec && now.tv_nsec > dl.tv_nsec)) {
            base = now; consumed = 0; continue;   /* 宿主卡顿落后：跳过本周期并重同步，不追赶洪泛 */
        }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &dl, NULL) == EINTR) {}
        if (!atomic_load_explicit(&u->running, memory_order_acquire)) break;
        AudioTimeStamp ts = {0}; AudioUnitRenderActionFlags flags = 0;
        if (fn) fn(u->cb.proc, &flags, &ts, 0, AU_PERIOD_FRAMES, &io);   /* =audio_mixer.c:109 的 pull */
        au_sink_push(u, scratch, AU_PERIOD_FRAMES);
    }
    free(scratch);
    return NULL;
}
OSStatus AudioOutputUnitStart(AudioComponentInstance inst) {
    struct OpaqueAudioComponentInstance *u = (struct OpaqueAudioComponentInstance *)inst;
    if (!u) return kAudioUnitErr_InvalidProperty;
    if (u->started) return noErr;
    au_sink_open(u);
    atomic_store_explicit(&u->running, 1, memory_order_release);
    if (pthread_create(&u->thread, NULL, au_ticker, u)) {
        atomic_store_explicit(&u->running, 0, memory_order_release);
        return kAudioUnitErr_InvalidProperty;
    }
    u->started = 1;
    return noErr;
}
OSStatus AudioOutputUnitStop(AudioComponentInstance inst) {
    struct OpaqueAudioComponentInstance *u = (struct OpaqueAudioComponentInstance *)inst;
    if (!u) return kAudioUnitErr_InvalidProperty;
    if (u->started) {
        atomic_store_explicit(&u->running, 0, memory_order_release);
        pthread_join(u->thread, NULL);   /* 周期 ~10.67ms，join 快 */
        u->started = 0;
    }
    if (u->renderer) { OH_AudioRenderer_Stop(u->renderer); OH_AudioRenderer_Release(u->renderer); u->renderer = NULL; }
    if (u->builder) { OH_AudioStreamBuilder_Destroy(u->builder); u->builder = NULL; }
    return 0;
}

// ---------- 二次收口：前端交互层与 NVN 纹理工具的桩（周末实装） ----------
CFLocaleRef CFLocaleCopyCurrent(void) { return NULL; }
CFStringRef CFLocaleGetValue(CFLocaleRef locale, CFStringRef key) { (void)locale; (void)key; return NULL; }
/* AudioOutputUnitStart/Stop 已实装（上方虚拟默认输出设备段），此处旧失败桩删除（round7） */
int proc_listallpids(void *buffer, int buffersize) { (void)buffer; (void)buffersize; return 0; }
char _mh_execute_header;
void dm_gfx_anomdump(void) {}
void dm_gfx_gpucapture_next(void) {}
void dm_gfx_passdump_next(void) {}
void dm_gfx_querylog_next(void) {}
void dm_gfx_texdump_next(void) {}
/* nvn_deswizzle 假桩已摘（round23批2）：真身=third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c（arm64 编入），
   本处残留的同名错签名桩会造成重复符号（m4_port_plan.md:45 早已警告） */
void runtime_gamepad_clear(void) {}
void runtime_gamepad_type_cycle(void) {}
void runtime_mouse_toggle(void) {}
void runtime_rumble_set(double low, double high) { (void)low; (void)high; }
void runtime_shortcut_help_toggle(void) {}
void taa_toggle(void) {}

// round 1 stubs
// CC_SHA256 一次性接口不能再空桩：platform/loader.c:187 与 platform/shared_font.c:28 用它做模块/字体完整性校验，
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
// 见 platform/entry.S（OHOS/ELF 移植，来源 by3 runtime/gate.S + thread_enter.S）；此处原空桩删除。
// mach_continuous_time 同步改为真实现：nn::time 稳态时钟（platform/time_host.c:35）用它计秒，
// 空桩返回 0 会让游戏时间恒停。与 Darwin 语义对齐：连续时间，纳秒基。
uint64_t mach_continuous_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- pthread QoS 真实现（ohos_compat.h 原空桩的函数体，round20b）----
 * OHOS QoS 只能设/查「当前线程」（sysroot qos/qos.h：OH_QoS_SetThreadQoS /
 * OH_QoS_ResetThreadQoS / OH_QoS_GetThreadQoS，libqos.so，API 12+），与 Apple
 * pthread_set/get_qos_class_*_np 的自身线程语义刚好对齐（get 的调用点全部传
 * pthread_self()，其它 tid 一律报 UNSPECIFIED）。Apple QOS_CLASS_* → QoS_Level：
 *   USER_INTERACTIVE(0)→QOS_USER_INTERACTIVE(5)   USER_INITIATED(1)→QOS_USER_INITIATED(3)
 *   DEFAULT(2)→QOS_DEFAULT(2)                     UTILITY(3)→QOS_UTILITY(1)
 *   UNSPECIFIED(-1)→OH_QoS_ResetThreadQoS（撤掉已设级，回系统缺省）
 * 直接链 -lqos 亦可，取 dlopen：老系统无 libqos.so 时不挂构建期链接，运行期才退化——
 * 首次调用初始化失败只打一行日志，之后 set 返 -1、get 报 QOS_CLASS_UNSPECIFIED。 */
#include <pthread.h>
#include <dlfcn.h>
#include <qos/qos.h>

typedef int (*qos_set_fn)(QoS_Level);
typedef int (*qos_reset_fn)(void);
typedef int (*qos_get_fn)(QoS_Level *);
static qos_set_fn qos_set;
static qos_reset_fn qos_reset;
static qos_get_fn qos_get;
static void qos_lib_init(void) {
    void *h = dlopen("libqos.so", RTLD_NOW | RTLD_LOCAL);
    if (h) {
        qos_set = (qos_set_fn)dlsym(h, "OH_QoS_SetThreadQoS");
        qos_reset = (qos_reset_fn)dlsym(h, "OH_QoS_ResetThreadQoS");
        qos_get = (qos_get_fn)dlsym(h, "OH_QoS_GetThreadQoS");
    }
    if (!h || !qos_set || !qos_reset || !qos_get)
        fprintf(stderr, "[qos] libqos.so unavailable (dlopen/dlsym failed): thread QoS degrades to no-op\n");
}
int pthread_set_qos_class_self_np(int cls, int rel) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, qos_lib_init);
    (void)rel;
    if (!qos_set) return -1;
    switch (cls) {
    case QOS_CLASS_USER_INTERACTIVE: return qos_set(QOS_USER_INTERACTIVE);
    case QOS_CLASS_USER_INITIATED:   return qos_set(QOS_USER_INITIATED);
    case QOS_CLASS_DEFAULT:          return qos_set(QOS_DEFAULT);
    case QOS_CLASS_UTILITY:          return qos_set(QOS_UTILITY);
    case QOS_CLASS_UNSPECIFIED:      return qos_reset();
    default: return -1;
    }
}
int pthread_get_qos_class_np(void *tid, qos_class_t *cls, int *rel) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, qos_lib_init);
    if (rel) *rel = 0;
    if (cls) *cls = QOS_CLASS_UNSPECIFIED;
    if (!qos_get || (tid && tid != (void *)pthread_self())) return 0;
    QoS_Level l;
    if (qos_get(&l) != 0) return 0;   /* 未设级（GetThreadQoS 返 -1）：按 UNSPECIFIED 报 */
    if (cls) switch (l) {
    case QOS_USER_INTERACTIVE: *cls = QOS_CLASS_USER_INTERACTIVE; break;
    case QOS_USER_INITIATED:   *cls = QOS_CLASS_USER_INITIATED; break;
    case QOS_DEFAULT:          *cls = QOS_CLASS_DEFAULT; break;
    case QOS_UTILITY:          *cls = QOS_CLASS_UTILITY; break;
    case QOS_BACKGROUND:       *cls = QOS_CLASS_UTILITY; break;   /* Apple 侧无 BACKGROUND 宏，就近代入 */
    default:                   *cls = QOS_CLASS_USER_INTERACTIVE; break;   /* QOS_DEADLINE_REQUEST：高于 interactive 的请求级 */
    }
    return 0;
}
