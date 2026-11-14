// m41b_vkprobe — 真机 Vulkan 格式能力探针（M41B）
// 单文件 C，静态链 -lvulkan（必要时 dlopen 回退），输出到 stdout + 指定文件。
// 采集：instance/device 扩展全列表、VkPhysicalDeviceProperties、指定格式 optimal/linear 位。 
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

#include <vulkan/vulkan.h>

static FILE *g_out = NULL;

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_out) {
        va_start(ap, fmt);
        vfprintf(g_out, fmt, ap);
        va_end(ap);
    }
    fflush(stdout);
    if (g_out) fflush(g_out);
}

static const char *vkres(VkResult r) {
    switch (r) {
#define C(x) case x: return #x;
        C(VK_SUCCESS) C(VK_ERROR_OUT_OF_HOST_MEMORY) C(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        C(VK_ERROR_INITIALIZATION_FAILED) C(VK_ERROR_LAYER_NOT_PRESENT)
        C(VK_ERROR_EXTENSION_NOT_PRESENT) C(VK_ERROR_INCOMPATIBLE_DRIVER)
        C(VK_ERROR_OUT_OF_DATE_KHR) C(VK_ERROR_SURFACE_LOST_KHR)
#undef C
        default: return "VK_RESULT_UNKNOWN";
    }
}

static const char *devType(VkPhysicalDeviceType t) {
    switch (t) {
#define C(x) case x: return #x;
        C(VK_PHYSICAL_DEVICE_TYPE_OTHER) C(VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        C(VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) C(VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU)
        C(VK_PHYSICAL_DEVICE_TYPE_CPU)
#undef C
        default: return "UNKNOWN";
    }
}

// ---- 扩展记录（用于按名字查询） ----
#define MAXEXT 1024
static char g_instExt[MAXEXT][256];
static int  g_instExtN = 0;
static char g_devExt[MAXEXT][256];
static int  g_devExtN = 0;

static int has_ext(char tab[MAXEXT][256], int n, const char *name) {
    for (int i = 0; i < n; i++) if (strcmp(tab[i], name) == 0) return 1;
    return 0;
}

static void dump_ext_list(const char *title, const char *const *names, uint32_t n,
                          char tab[MAXEXT][256], int *cnt) {
    out("\n########## %s (%u) ##########\n", title, n);
    for (uint32_t i = 0; i < n; i++) {
        unsigned spec = 0;
        // 只对 device ext 读 specVersion；instance ext 用另一支
        out("%s\n", names[i]);
        if (*cnt < MAXEXT) {
            snprintf(tab[*cnt], 256, "%s", names[i]);
            (*cnt)++;
        }
        (void)spec;
    }
    out("\n--- %s marker: VK_OHOS_* ---\n", title);
    int any = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (strncmp(names[i], "VK_OHOS_", 8) == 0) { out("  OHOS: %s\n", names[i]); any = 1; }
    }
    if (!any) out("  OHOS: (none)\n");
    out("--- %s marker: VK_KHR/EXT/VALVE noteworthy ---\n", title);
    for (uint32_t i = 0; i < n; i++) {
        const char *e = names[i];
        if (strstr(e, "swapchain") || strstr(e, "external_memory") || strstr(e, "external_semaphore")
            || strstr(e, "native_buffer") || strstr(e, "external_format_resolve")
            || strstr(e, "image_drm_format") || strstr(e, "hardware_buffer")
            || strstr(e, "android") || strstr(e, "display") || strstr(e, "present")
            || strstr(e, "texture_compression") || strstr(e, "maintenance") || strstr(e, "16bit_storage")
            || strstr(e, "sampler_filter") || strstr(e, "get_memory_requirements2") || strstr(e, "bind_memory2"))
            out("  KEY: %s\n", e);
    }
}

// ---- 格式表 ----
typedef struct { VkFormat f; const char *name; } FmtEnt;

static const FmtEnt g_fmts[] = {
    { VK_FORMAT_BC1_RGB_UNORM_BLOCK,        "BC1_RGB_UNORM" },
    { VK_FORMAT_BC1_RGBA_UNORM_BLOCK,       "BC1_RGBA_UNORM" },
    { VK_FORMAT_BC1_RGB_SRGB_BLOCK,         "BC1_RGB_SRGB" },
    { VK_FORMAT_BC1_RGBA_SRGB_BLOCK,        "BC1_RGBA_SRGB" },
    { VK_FORMAT_BC2_UNORM_BLOCK,            "BC2_UNORM" },
    { VK_FORMAT_BC2_SRGB_BLOCK,             "BC2_SRGB" },
    { VK_FORMAT_BC3_UNORM_BLOCK,            "BC3_UNORM" },
    { VK_FORMAT_BC3_SRGB_BLOCK,             "BC3_SRGB" },
    { VK_FORMAT_BC4_UNORM_BLOCK,            "BC4_UNORM" },
    { VK_FORMAT_BC5_UNORM_BLOCK,            "BC5_UNORM" },
    { VK_FORMAT_BC6H_UFLOAT_BLOCK,          "BC6H_UFLOAT" },
    { VK_FORMAT_BC7_UNORM_BLOCK,            "BC7_UNORM" },
    { VK_FORMAT_BC7_SRGB_BLOCK,             "BC7_SRGB" },
    { VK_FORMAT_ASTC_4x4_UNORM_BLOCK,       "ASTC_4x4_UNORM" },
    { VK_FORMAT_ASTC_4x4_SRGB_BLOCK,        "ASTC_4x4_SRGB" },
    { VK_FORMAT_ASTC_6x6_UNORM_BLOCK,       "ASTC_6x6_UNORM" },
    { VK_FORMAT_ASTC_8x8_UNORM_BLOCK,       "ASTC_8x8_UNORM" },
    { VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,    "ETC2_R8G8B8_UNORM" },
    { VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,  "ETC2_R8G8B8A8_UNORM" },
    { VK_FORMAT_EAC_R11_UNORM_BLOCK,        "EAC_R11_UNORM" },
    { VK_FORMAT_R8G8B8A8_UNORM,             "R8G8B8A8_UNORM" },
    { VK_FORMAT_R8G8B8A8_SRGB,              "R8G8B8A8_SRGB" },
    { VK_FORMAT_B8G8R8A8_UNORM,             "B8G8R8A8_UNORM" },
    // ---- 追加扫描（非派活单必需，供纹理管线选型） ----
    { VK_FORMAT_ASTC_5x5_UNORM_BLOCK,       "ASTC_5x5_UNORM" },
    { VK_FORMAT_ASTC_5x5_SRGB_BLOCK,        "ASTC_5x5_SRGB" },
    { VK_FORMAT_ASTC_6x6_SRGB_BLOCK,        "ASTC_6x6_SRGB" },
    { VK_FORMAT_ASTC_8x5_UNORM_BLOCK,       "ASTC_8x5_UNORM" },
    { VK_FORMAT_ASTC_8x6_UNORM_BLOCK,       "ASTC_8x6_UNORM" },
    { VK_FORMAT_ASTC_8x8_SRGB_BLOCK,        "ASTC_8x8_SRGB" },
    { VK_FORMAT_ASTC_10x5_UNORM_BLOCK,      "ASTC_10x5_UNORM" },
    { VK_FORMAT_ASTC_10x6_UNORM_BLOCK,      "ASTC_10x6_UNORM" },
    { VK_FORMAT_ASTC_10x8_UNORM_BLOCK,      "ASTC_10x8_UNORM" },
    { VK_FORMAT_ASTC_10x10_UNORM_BLOCK,     "ASTC_10x10_UNORM" },
    { VK_FORMAT_ASTC_12x10_UNORM_BLOCK,     "ASTC_12x10_UNORM" },
    { VK_FORMAT_ASTC_12x12_UNORM_BLOCK,     "ASTC_12x12_UNORM" },
    { VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,     "ETC2_R8G8B8_SRGB" },
    { VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,   "ETC2_R8G8B8A8_SRGB" },
    { VK_FORMAT_EAC_R11G11_UNORM_BLOCK,     "EAC_R11G11_UNORM" },
    { VK_FORMAT_EAC_R11_SNORM_BLOCK,       "EAC_R11_SIGNED" },
    { VK_FORMAT_B10G11R11_UFLOAT_PACK32,    "B10G11R11_UFLOAT" },
    { VK_FORMAT_R16G16B16A16_SFLOAT,        "R16G16B16A16_SFLOAT" },
    { VK_FORMAT_R8G8_UNORM,                 "R8G8_UNORM" },
    { VK_FORMAT_R8_UNORM,                   "R8_UNORM" },
    { VK_FORMAT_D32_SFLOAT,                 "D32_SFLOAT" },
    { VK_FORMAT_D24_UNORM_S8_UINT,          "D24_UNORM_S8" },
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32,   "A2B10G10R10_UNORM" },
};

static const uint32_t bitList[] = {
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_CUBIC_BIT_EXT,
    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT,
    VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
    VK_FORMAT_FEATURE_BLIT_SRC_BIT,
    VK_FORMAT_FEATURE_BLIT_DST_BIT,
    VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
};
static const char *bitName[] = {
    "SAMPLED_IMAGE", "SAMP_IMAGE_FILTER_LINEAR", "SAMP_IMAGE_FILTER_CUBIC",
    "TRANSFER_SRC", "TRANSFER_DST", "COLOR_ATTACHMENT", "BLIT_SRC", "BLIT_DST", "STORAGE_IMAGE",
};

int main(int argc, char **argv) {
    const char *outpath = (argc > 1) ? argv[1] : NULL;
    if (outpath) g_out = fopen(outpath, "a");

    out("\n================ m41b vkprobe start ================\n");
    time_t now = time(NULL);
    out("time: %s", ctime(&now));
#ifdef __aarch64__
    out("arch: aarch64\n");
#elif defined(__x86_64__)
    out("arch: x86_64\n");
#endif
    out("VK_HEADER_VERSION: %u\n", (unsigned)VK_HEADER_VERSION);

    PFN_vkGetInstanceProcAddr gp = NULL;

    // 1) 直接链 libvulkan 符号（静态链时可用）；失败再 dlopen
    VkResult r = VK_SUCCESS;
    uint32_t apiVer = 0;
    r = vkEnumerateInstanceVersion(&apiVer);
    out("vkEnumerateInstanceVersion -> %d (%s), loader apiVersion=0x%08x (%u.%u.%u)\n",
        (int)r, vkres(r), apiVer,
        VK_VERSION_MAJOR(apiVer), VK_VERSION_MINOR(apiVer), VK_VERSION_PATCH(apiVer));

    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "m41b_vkprobe";
    app.applicationVersion = 1;
    app.pEngineName = "m41b";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_0;

    // ---- instance extensions ----
    uint32_t icount = 0;
    r = vkEnumerateInstanceExtensionProperties(NULL, &icount, NULL);
    out("\nvkEnumerateInstanceExtensionProperties(count) -> %d (%s), n=%u\n", (int)r, vkres(r), icount);
    VkExtensionProperties *iep = calloc(icount ? icount : 1, sizeof(*iep));
    if (icount) {
        r = vkEnumerateInstanceExtensionProperties(NULL, &icount, iep);
        out("  fetch -> %d (%s)\n", (int)r, vkres(r));
    }
    const char *inames[MAXEXT];
    for (uint32_t i = 0; i < icount && i < MAXEXT; i++) inames[i] = iep[i].extensionName;
    dump_ext_list("INSTANCE EXTENSIONS", inames, icount < MAXEXT ? icount : MAXEXT,
                  g_instExt, &g_instExtN);

    // ---- instance (尽量不加扩展，先看裸 device） ----
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    r = vkCreateInstance(&ici, NULL, &inst);
    out("\nvkCreateInstance(no ext) -> %d (%s), instance=%p\n", (int)r, vkres(r), (void*)inst);
    if (r != VK_SUCCESS) {
        out("!! 无法创建 VkInstance，格式表无法采集。\n");
        goto done;
    }

    // ---- physical devices ----
    uint32_t ndev = 0;
    r = vkEnumeratePhysicalDevices(inst, &ndev, NULL);
    out("vkEnumeratePhysicalDevices -> %d (%s), n=%u\n", (int)r, vkres(r), ndev);
    if (r != VK_SUCCESS || ndev == 0) {
        out("!! 无物理设备\n");
        goto destroy;
    }
    if (ndev > 8) ndev = 8;
    VkPhysicalDevice pds[8];
    r = vkEnumeratePhysicalDevices(inst, &ndev, pds);
    out("  fetch -> %d (%s)\n", (int)r, vkres(r));

    for (uint32_t di = 0; di < ndev; di++) {
        VkPhysicalDevice pd = pds[di];
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        out("\n########## PHYSICAL DEVICE #%u ##########\n", di);
        out("deviceName     : %s\n", p.deviceName);
        out("apiVersion     : 0x%08x (%u.%u.%u)\n", p.apiVersion,
            VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
        out("driverVersion  : 0x%08x (%u.%u.%u.%u packed)\n", p.driverVersion,
            (p.driverVersion >> 22) & 0x3ff, (p.driverVersion >> 12) & 0x3ff,
            (p.driverVersion >> 6) & 0x3f, p.driverVersion & 0x3f);
        // driverID 在 OHOS 头里不在 VkPhysicalDeviceProperties，走 pNext 链
        {
            VkPhysicalDeviceDriverProperties dp = {0};
            dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            VkPhysicalDeviceProperties2 p2 = {0};
            p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            p2.pNext = &dp;
            if (VK_VERSION_MINOR(p.apiVersion) >= 1 && vkGetPhysicalDeviceProperties2) {
                vkGetPhysicalDeviceProperties2(pd, &p2);
                out("driverID(pNext): %u (%s)\n", dp.driverID, dp.driverName);
                out("driverInfo(pNext): %s\n", dp.driverInfo);
                out("conformanceVersion: %u.%u.%u.%u\n", dp.conformanceVersion.major,
                    dp.conformanceVersion.minor, dp.conformanceVersion.subminor, dp.conformanceVersion.patch);
            } else {
                out("driverID(pNext): N/A (no props2 path)\n");
            }
        }
        out("vendorID       : 0x%04x\n", p.vendorID);
        out("deviceID       : 0x%04x\n", p.deviceID);
        out("deviceType     : %d (%s)\n", (int)p.deviceType, devType(p.deviceType));
        out("maxImageDim2D  : %u\n", p.limits.maxImageDimension2D);
        out("maxSamplerAniso: %.2f\n", (double)p.limits.maxSamplerAnisotropy);
        out("sparseBinding  : %d\n", (int)p.sparseProperties.residencyStandard2DBlockShape);

        // ---- device extensions ----
        uint32_t dcount = 0;
        r = vkEnumerateDeviceExtensionProperties(pd, NULL, &dcount, NULL);
        out("\nvkEnumerateDeviceExtensionProperties(count) -> %d (%s), n=%u\n", (int)r, vkres(r), dcount);
        VkExtensionProperties *dep = calloc(dcount ? dcount : 1, sizeof(*dep));
        if (dcount) vkEnumerateDeviceExtensionProperties(pd, NULL, &dcount, dep);
        const char *dnames[MAXEXT];
        uint32_t dn = dcount < MAXEXT ? dcount : MAXEXT;
        for (uint32_t i = 0; i < dn; i++) dnames[i] = dep[i].extensionName;
        dump_ext_list("DEVICE EXTENSIONS", dnames, dn, g_devExt, &g_devExtN);

        // ---- queue families (参考) ----
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
        VkQueueFamilyProperties qf[16];
        if (qn > 16) qn = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
        out("\nqueueFamilies: %u\n", qn);
        for (uint32_t q = 0; q < qn; q++)
            out("  [%u] flags=0x%x count=%u\n", q, qf[q].queueFlags, qf[q].queueCount);

        // ---- FORMAT TABLE ----
        out("\n########## FORMAT TABLE (device #%u) ##########\n", di);
        out("%-20s | %-10s | %-18s | %-18s | %s\n",
            "FORMAT", "LINEAR", "OPT_DECODED", "LIN_DECODED", "OPT_HEX");
        for (size_t i = 0; i < sizeof(g_fmts)/sizeof(g_fmts[0]); i++) {
            VkFormatProperties fp;
            memset(&fp, 0, sizeof(fp));
            vkGetPhysicalDeviceFormatProperties(pd, g_fmts[i].f, &fp);

            char optDec[512] = ""; char linDec[512] = "";
            for (size_t b = 0; b < sizeof(bitList)/sizeof(bitList[0]); b++) {
                if (fp.optimalTilingFeatures & bitList[b]) {
                    if (*optDec) strncat(optDec, "+", sizeof(optDec)-strlen(optDec)-1);
                    strncat(optDec, bitName[b], sizeof(optDec)-strlen(optDec)-1);
                }
                if (fp.linearTilingFeatures & bitList[b]) {
                    if (*linDec) strncat(linDec, "+", sizeof(linDec)-strlen(linDec)-1);
                    strncat(linDec, bitName[b], sizeof(linDec)-strlen(linDec)-1);
                }
            }
            if (!*optDec) strcpy(optDec, "(none)");
            if (!*linDec) strcpy(linDec, "(none)");
            out("%-20s | opt=0x%08x lin=0x%08x | %s | %s | buf=0x%08x\n",
                g_fmts[i].name, fp.optimalTilingFeatures, fp.linearTilingFeatures,
                optDec, linDec, fp.bufferFeatures);
        }

        // ---- 一句话判定 ----
        out("\n########## VERDICT (device #%u) ##########\n", di);
        const VkFormat bcFormats[] = {
            VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
            VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK,
            VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,
            VK_FORMAT_R8G8B8A8_UNORM,
        };
        const char *bcNames[] = { "BC1_RGB", "BC1_RGBA", "BC3", "BC7", "ASTC_4x4", "ETC2_RGB", "RGBA8" };
        for (size_t i = 0; i < sizeof(bcFormats)/sizeof(bcFormats[0]); i++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(pd, bcFormats[i], &fp);
            int samp = !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
            int lin  = !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
            int dst  = !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
            out("%-10s sampled=%s(filter_linear=%s) transfer_dst=%s -> %s\n",
                bcNames[i], samp ? "YES" : "NO", lin ? "YES" : "NO", dst ? "YES" : "NO",
                (samp && lin) ? "DIRECT-SAMPLE OK" : (samp ? "sample(newer filter?)" : "NOT SAMPLEABLE"));
        }
        out("\n");
    }

destroy:
    vkDestroyInstance(inst, NULL);
done:
    out("================ m41b vkprobe end ================\n");
    if (g_out) fclose(g_out);
    return 0;
}
