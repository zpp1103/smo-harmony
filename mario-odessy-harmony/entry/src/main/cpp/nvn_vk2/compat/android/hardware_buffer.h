// M42A / S1: branch-B shim for <android/hardware_buffer.h>.
//
// The nx2android backend reaches the guest's own memory through an AHardwareBuffer imported as a
// VkBuffer (gfx_vk_core.c pool_zero_copy). OHOS has no AHardwareBuffer, and this slice runs the
// backend in nx2's first-class non-zero-copy mode (docs/m41-fork-plan.md §4.2 分支 B):
// DM_VK_NO_ZEROCOPY=1 makes pool_add_one (gfx_vk_core.c:626) skip pool_zero_copy entirely and take
// the host-visible shadow copy + vk_pools_sync path.
//
// So these declarations exist only so that file still compiles, and the four functions always fail
// (ohos_dm_stub.c). Nothing is silently allowed to succeed: if some future change reaches this code
// without DM_VK_NO_ZEROCOPY, the pool degrades to the copied path with a "[RED] ... could not be
// zero-copy" log line rather than a wrong picture.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct AHardwareBuffer AHardwareBuffer;

typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;

#define AHARDWAREBUFFER_FORMAT_BLOB             0x21u
#define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN    0x0000000000000003ull
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN   0x0000000000000030ull
#define AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER   0x0000000000010000ull

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out);
void AHardwareBuffer_release(AHardwareBuffer *buf);
int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *buf, int socket_fd);

// The Android-only Vulkan counterparts the backend names in its zero-copy path.
typedef struct VkImportAndroidHardwareBufferInfoANDROID {
    VkStructureType sType;
    const void *pNext;
    AHardwareBuffer *buffer;
} VkImportAndroidHardwareBufferInfoANDROID;
#define VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID ((VkStructureType)1000180001)

typedef struct VkAndroidHardwareBufferPropertiesANDROID {
    VkStructureType sType;
    void *pNext;
    VkDeviceSize allocationSize;
    uint32_t memoryTypeBits;
} VkAndroidHardwareBufferPropertiesANDROID;
#define VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID ((VkStructureType)1000180002)

typedef VkResult (VKAPI_PTR *PFN_vkGetAndroidHardwareBufferPropertiesANDROID)(VkDevice device, const AHardwareBuffer *buffer, VkAndroidHardwareBufferPropertiesANDROID *pProperties);
