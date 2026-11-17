// VideoToolbox shim —— H.264 硬解；OHOS 侧周末换 AVCodec（软解先行），stub 返回失败
#pragma once
#include <stdint.h>

typedef struct OpaqueVTDecompressionSession *VTDecompressionSessionRef;
typedef struct {
    void *decompressionOutputRefCon;
    void (*decompressionOutputCallback)(void *refCon, void *session, void *status, void *infoFlags,
                                       void *imageBuffer, int64_t pts, int64_t duration);
} VTDecompressionOutputCallbackRecord;

typedef struct {
    uint32_t width, height;
} VTSessionFormat;

int VTDecompressionSessionCreate(void *allocator, void *formatDesc, void *videoDecoderSpecification,
                                 void *destinationImageBufferAttributes,
                                 const VTDecompressionOutputCallbackRecord *callback,
                                 VTDecompressionSessionRef *out);
int VTDecompressionSessionDecodeFrame(VTDecompressionSessionRef s, void *sampleBuffer, void *flags,
                                      void *infoFlagsOut);
void VTDecompressionSessionInvalidate(VTDecompressionSessionRef s);
int VTDecompressionSessionWaitForAsynchronousFrames(VTDecompressionSessionRef s);
