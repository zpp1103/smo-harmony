// M42A / S1: the OHOS Vulkan instance bootstrap for the vendored nx2android backend.
//
// Why this file exists (all of it measured earlier in this project, not guessed):
//   1. On this emulator the system loader has no ICD: a plain vkCreateInstance answers -9
//      (VK_ERROR_INCOMPATIBLE_DRIVER). The APK ships SwiftShader (entry/libs/arm64-v8a/libvk_swiftshader.so
//      + vk_swiftshader_icd.json), but VK_DRIVER_FILES / VK_ICD_FILENAMES are ignored inside an OHOS app
//      process (system loader), so the only working route is VK_LUNARG_direct_driver_loading, which the
//      bundled loader does implement. Evidence: logs/m40az_b/vk_boot.log:19-25 (three env attempts -9, then
//      "[prox] vkCreateInstance rv=0").
//   2. The OHOS implicit device layer (VK_LAYER_OHOS_surface, /system/lib64/libvulkan_swapchain.so) requires
//      the driver to report VK_OHOS_native_buffer when it enumerates device extensions, and returns -3 from
//      vkCreateDevice without it; SwiftShader reports no OHOS extension. So the vkGetInstanceProcAddr we hand
//      the loader is a proxy that appends VK_OHOS_native_buffer to the enumeration and strips it again before
//      the real driver's vkCreateDevice. Evidence: nvn_vk.c:1504-1590 and logs/m40az_b/vk_boot.log:138-151.
//   3. The emulator's loader answers -9 unless VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR is set where
//      VK_KHR_portability_enumeration exists, and it fails outright if an extension it lacks is requested —
//      so the extension list is built from what the loader really has, never hardcoded.
//
// This is the same domain knowledge that lives in platform/nvn_vk/nvn_vk.c:1504-1640, re-expressed here so the
// v2 backend is self-contained and nvn_vk.c stays untouched (M42A constraint: one line of nvn_vk.c changes).
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "dm.h"
#include "gfx_vk.h"

#define PROX_EXT "VK_OHOS_native_buffer"

// platform/vk_boot.c: prepares the bundled ICD (writes its manifest, sets the JIT dir) and gives its path.
extern const char *vk_boot_icd_path(void);
extern int vk_boot_init(void);

static PFN_vkGetInstanceProcAddr g_real_gipa;        // the driver's own entry (nil for the plain loader route)
static VkInstance g_real_inst;
static PFN_vkEnumerateDeviceExtensionProperties g_real_enum_dext;
static PFN_vkCreateDevice g_real_create_device;

static VkResult prox_create_instance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *al, VkInstance *out) {
    if (!g_real_create_device && !g_real_gipa) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateInstance mk = (PFN_vkCreateInstance)g_real_gipa(NULL, "vkCreateInstance");
    if (!mk) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult rv = mk(ci, al, out);
    VKLOG("prox: driver vkCreateInstance -> %d", (int)rv);
    if (rv == VK_SUCCESS) {
        g_real_inst = *out;
        g_real_enum_dext = (PFN_vkEnumerateDeviceExtensionProperties)g_real_gipa(g_real_inst, "vkEnumerateDeviceExtensionProperties");
        g_real_create_device = (PFN_vkCreateDevice)g_real_gipa(g_real_inst, "vkCreateDevice");
        VKLOG("prox: driver table enumDeviceExt=%p createDevice=%p", (void *)g_real_enum_dext, (void *)g_real_create_device);
    }
    return rv;
}

// The layer enumerates the driver's device extensions and looks for PROX_EXT; the driver has none, so we add
// one. The query is two calls (count first, then the array), and the loader's terminator also appends the
// implicit layers' own extensions into the same array — hence the spare slots (+5), otherwise the count is
// reset to 0 with VK_INCOMPLETE (M3d finding, nvn_vk.c:1551-1557).
static VkResult prox_enum_dext(VkPhysicalDevice pd, const char *layer, uint32_t *cnt, VkExtensionProperties *props) {
    if (!g_real_enum_dext) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer) return g_real_enum_dext(pd, layer, cnt, props);
    if (!props) {
        uint32_t n = 0;
        VkResult rv = g_real_enum_dext(pd, NULL, &n, NULL);
        if (rv == VK_SUCCESS) *cnt = n + 5;
        return rv;
    }
    uint32_t cap = *cnt, n = cap;
    VkResult rv = g_real_enum_dext(pd, NULL, &n, props);
    if (rv != VK_SUCCESS) return rv;
    int have = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(props[i].extensionName, PROX_EXT)) have = 1;
    int appended = 0;
    if (!have && n < cap) {
        memset(&props[n], 0, sizeof props[n]);
        snprintf(props[n].extensionName, sizeof props[n].extensionName, "%s", PROX_EXT);
        props[n].specVersion = 1;
        n++; appended = 1;
    }
    *cnt = n;
    return (n < cap || appended) ? VK_SUCCESS : VK_INCOMPLETE;
}

static VkResult prox_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *al, VkDevice *out) {
    if (!g_real_create_device) return VK_ERROR_INITIALIZATION_FAILED;
    VkDeviceCreateInfo ci2 = *ci;
    const char *keep[16];
    uint32_t nk = 0, stripped = 0;
    if (ci->enabledExtensionCount && ci->ppEnabledExtensionNames && ci->enabledExtensionCount <= 16) {
        for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
            if (!strcmp(ci->ppEnabledExtensionNames[i], PROX_EXT)) { stripped++; continue; }
            keep[nk++] = ci->ppEnabledExtensionNames[i];
        }
        if (stripped) { ci2.enabledExtensionCount = nk; ci2.ppEnabledExtensionNames = keep; }
    }
    VkResult rv = g_real_create_device(pd, &ci2, al, out);
    VKLOG("prox: driver vkCreateDevice -> %d (ext %u -> %u, stripped %u)", (int)rv, ci->enabledExtensionCount, ci2.enabledExtensionCount, stripped);
    return rv;
}

static PFN_vkVoidFunction prox_gipa(VkInstance inst, const char *name) {
    (void)inst;
    if (!name || !g_real_gipa) return NULL;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)prox_create_instance;
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) return (PFN_vkVoidFunction)prox_enum_dext;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)prox_create_device;
    return g_real_gipa(inst, name);
}

static int has_iext(PFN_vkEnumerateInstanceExtensionProperties ep, const char *want) {
    uint32_t n = 0;
    if (ep(NULL, &n, NULL) || !n) return 0;
    VkExtensionProperties xp[64];
    if (n > 64) n = 64;
    if (ep(NULL, &n, xp)) return 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(xp[i].extensionName, want)) return 1;
    return 0;
}

VkResult nvn_vk2_instance_create(VkInstance *out) {
    vk_boot_init();   // bundled ICD manifest + SwiftShader JIT dir (platform/vk_boot.c); idempotent
    void *lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { VKLOG("[RED] libvulkan.so: %s", dlerror()); return VK_ERROR_INITIALIZATION_FAILED; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) { VKLOG("[RED] no vkGetInstanceProcAddr in libvulkan.so"); return VK_ERROR_INITIALIZATION_FAILED; }
    PFN_vkEnumerateInstanceExtensionProperties ep = (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");

    const char *iext[8];
    uint32_t niext = 0;
    int portability = 0;
    if (ep) {
        if (has_iext(ep, "VK_KHR_surface")) iext[niext++] = "VK_KHR_surface";
        if (has_iext(ep, "VK_KHR_portability_enumeration")) { iext[niext++] = "VK_KHR_portability_enumeration"; portability = 1; }
        if (has_iext(ep, "VK_EXT_debug_utils")) iext[niext++] = "VK_EXT_debug_utils";
        VKLOG("loader instance extensions: %u usable of the wanted set (portability %d)", niext, portability);
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "switch2hos", 1, "nvn_vk2", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL,
                                portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0u,
                                &app, 0, NULL, niext, iext};

    PFN_vkCreateInstance mk = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!mk) { VKLOG("[RED] no vkCreateInstance through the loader"); return VK_ERROR_INITIALIZATION_FAILED; }
    VkResult rv = mk(&ici, NULL, out);
    VKLOG("vkCreateInstance through the loader -> %d", (int)rv);
    if (rv == VK_SUCCESS) return rv;

    // No ICD behind the system loader (the emulator's case): hand the bundled SwiftShader driver straight to
    // the loader, behind the proxy above.
    const char *so = vk_boot_icd_path();
    if (!so) { VKLOG("[RED] no bundled ICD path (vk_boot_init failed): cannot use direct driver loading"); return rv; }
    void *icd = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!icd) { VKLOG("[RED] dlopen %s: %s", so, dlerror()); return rv; }
    g_real_gipa = (PFN_vkGetInstanceProcAddr)dlsym(icd, "vkGetInstanceProcAddr");
    if (!g_real_gipa) { VKLOG("[RED] no vkGetInstanceProcAddr in %s", so); return rv; }
    if (!gipa(NULL, "vkCreateInstance")) return rv;

    VkDirectDriverLoadingInfoLUNARG info = {VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_INFO_LUNARG, NULL, 0,
                                            (PFN_vkGetInstanceProcAddrLUNARG)prox_gipa};
    VkDirectDriverLoadingListLUNARG list = {VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG, NULL,
                                            VK_DIRECT_DRIVER_LOADING_MODE_INCLUSIVE_LUNARG, 1, &info};
    if (!has_iext(ep, VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME)) {
        VKLOG("[RED] the loader has no %s: cannot use the bundled driver", VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME);
        return rv;
    }
    iext[niext++] = VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME;
    ici.enabledExtensionCount = niext;
    ici.ppEnabledExtensionNames = iext;
    ici.pNext = &list;
    rv = mk(&ici, NULL, out);
    VKLOG("vkCreateInstance through direct driver loading (%s) -> %d", so, (int)rv);
    return rv;
}
