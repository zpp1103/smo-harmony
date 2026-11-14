// AudioToolbox shim —— 宿主 audio_out/audio_mixer 用到的 AudioQueue + AudioUnit 面。
// OHOS 实装（oh_audio）周末替换 ohos_stub.c 里的失败返回。
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef double Float64;
typedef float Float32;
typedef unsigned int UInt32;
typedef int OSStatus;

typedef struct OpaqueAudioQueue *AudioQueueRef;
typedef struct OpaqueAudioQueueBuffer *AudioQueueBufferRef;
typedef unsigned int AudioQueueParam;
typedef void (*AudioQueueOutputCallback)(void *userData, AudioQueueRef aq, AudioQueueBufferRef buf);

typedef struct {
    double mSampleRate;
    unsigned int mFormatID;
    unsigned int mFormatFlags;
    unsigned int mBytesPerPacket;
    unsigned int mFramesPerPacket;
    unsigned int mBytesPerFrame;
    unsigned int mChannelsPerFrame;
    unsigned int mBitsPerChannel;
    unsigned int mReserved;
} AudioStreamBasicDescription;

typedef struct {
    Float64 mSampleRate;
    unsigned long long mHostTime;
    double mRateScalar;
    double mWordClockTime;
} AudioTimeStamp;

struct OpaqueAudioQueueBuffer {
    unsigned int mAudioDataByteSize;
    void *mAudioData;
    void *userData;
};

enum {
    kAudioQueueParam_Volume = 1,
    kAudioQueueParam_VolumeRampTime = 2,
    kAudioQueueParam_PlayRate = 3,
    kLinearPCMFormatFlagIsSignedInteger = 4,
    kLinearPCMFormatFlagIsFloat = 1,
    kLinearPCMFormatFlagIsPacked = 8,
    kAudioFormatFlagIsSignedInteger = 4,
    kAudioFormatFlagIsFloat = 1,
    kAudioFormatFlagIsPacked = 8,
    kAudioFormatLinearPCM = 0x6C70636D,
    kAudioFormatMPEG4AAC = 0x6D703461,
    kAudioFormatOpus = 0x4F707573,
};
enum { noErr = 0 };
#define kAudioQueueErr_CannotStart (-1)
#define kCFRunLoopCommonModes ((void *)2)

int AudioQueueNewOutput(const AudioStreamBasicDescription *fmt, AudioQueueOutputCallback cb,
                        void *userData, void *runLoop, void *runLoopMode, unsigned int flags,
                        AudioQueueRef *out);
OSStatus AudioQueueDispose(AudioQueueRef aq, int immediately);
OSStatus AudioQueueAllocateBuffer(AudioQueueRef aq, unsigned int bytes, AudioQueueBufferRef *out);
OSStatus AudioQueueFreeBuffer(AudioQueueRef aq, AudioQueueBufferRef buf);
OSStatus AudioQueueEnqueueBuffer(AudioQueueRef aq, AudioQueueBufferRef buf, unsigned long numParam,
                                 const AudioQueueParam *params);
OSStatus AudioQueueStart(AudioQueueRef aq, void *startTime);
OSStatus AudioQueuePause(AudioQueueRef aq);
OSStatus AudioQueueReset(AudioQueueRef aq);
OSStatus AudioQueueSetParameter(AudioQueueRef aq, AudioQueueParam param, double value);

// ---- AudioUnit / AudioComponent（audio_mixer 渲染链）：round7 实装为虚拟默认输出设备（ohos_stub.c） ----
typedef struct OpaqueAudioComponent *AudioComponent;
typedef struct OpaqueAudioComponentInstance *AudioComponentInstance;
typedef unsigned int AudioUnitRenderActionFlags;
typedef unsigned int AudioUnitScope;
typedef struct {
    unsigned int mNumberBuffers;
    struct { unsigned int mNumberChannels; unsigned int mDataByteSize; void *mData; } mBuffers[2];
} AudioBufferList;
typedef struct { void *input; void *proc; } AURenderCallbackStruct;
typedef struct {
    unsigned int componentType;
    unsigned int componentSubType;
    unsigned int componentManufacturer;
    unsigned int componentFlags;
    unsigned int componentFlagsMask;
} AudioComponentDescription;

enum {
    kAudioUnitType_Output = 0x61756F73,
    kAudioUnitSubType_RemoteIO = 0x72696F63,
    kAudioUnitSubType_DefaultOutput = 0x6465666F,
    kAudioUnitManufacturer_Apple = 0x6170706C,
    kAudioUnitScope_Input = 1,
    kAudioUnitScope_Output = 0,
    kAudioUnitProperty_SetRenderCallback = 23,
    kAudioUnitProperty_StreamFormat = 8,
    kAudioUnitErr_InvalidProperty = -10879,
    kAudioComponentErr_InstanceInvalidated = -66749,
};

AudioComponent AudioComponentFindNext(AudioComponent inRef, const AudioComponentDescription *desc);
OSStatus AudioComponentInstanceNew(AudioComponent comp, AudioComponentInstance *out);
OSStatus AudioComponentInstanceDispose(AudioComponentInstance inst);
OSStatus AudioUnitInitialize(AudioComponentInstance inst);
OSStatus AudioUnitUninitialize(AudioComponentInstance inst);
OSStatus AudioUnitSetProperty(AudioComponentInstance inst, unsigned int propID, AudioUnitScope scope,
                              unsigned int element, const void *data, unsigned int dataSize);
OSStatus AudioUnitGetProperty(AudioComponentInstance inst, unsigned int propID, AudioUnitScope scope,
                              unsigned int element, void *data, unsigned int *dataSize);
/* round7 前无原型（调用点隐式声明）；实装在 ohos_stub.c 虚拟输出设备段 */
OSStatus AudioOutputUnitStart(AudioComponentInstance inst);
OSStatus AudioOutputUnitStop(AudioComponentInstance inst);
