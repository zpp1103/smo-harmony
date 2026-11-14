// M42A / S1 selftest: a fixed PSO drawing one full-screen triangle through the vendored nx2android backend
// (gfx_vk_core.c's device + memory pools), rendered offscreen, read back and written as a PPM.
//
// What this proves, and what it deliberately does not touch (docs/m41-fork-plan.md §7.1 切片 1):
//   * the nx2 `mtl_*` backend links and builds a Vulkan device on OHOS, in branch B (DM_VK_NO_ZEROCOPY=1, no
//     surface, no swapchain);
//   * a pipeline compiles and a draw lands in an offscreen render target, and the pixels read back correct;
//   * the submit→fence round trip stays under one 60 fps frame (16.7 ms).
//   It does NOT touch the game, the DMSV shader bundle, the recording layer or the present path.
//
// The SPIR-V is the one already in the repository (platform/nvn_vk/tri_spv.h, M3l), never recompiled here:
//   tri.vert = 3 vertices from gl_VertexIndex, red triangle covering ~79% of the target with the top-right
//              corner left to the clear colour; tri.frag = fixed vec4(1,0,0,1).
// So the expected readback is: triangle area = (255,0,0); outside = the clear colour (16,16,48).
//
// Switches (this project reads its switches from files in the app sandbox, not from the environment, because
// an OHOS app process cannot be given env vars by `aa start` — the file names mirror the nx2 env var names so
// the port keeps the same vocabulary; env still wins when present, e.g. under a future launcher):
//   DM_VK_SHOT=<n>            write the PPM of frame n (1-based). 0/unset = no shot.
//   DM_VK2_SELFTEST=1         run this selftest at all (default 0: nothing happens, the game is untouched)
//   DM_VK_SHOT_DIR=<dir>      where the PPM goes (default /data/storage/el2/base/files)
#define VK_NO_PROTOTYPES
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "dm.h"
#include "gfx_vk.h"

#include "../platform/nvn_vk/tri_spv.h"   // ★ the repository's own triangle SPIR-V, reused as-is (M42A constraint 6)

extern int vk_boot_log_open(void);
extern int vk_boot_jit_env_setup(void);

#define SW_DIR_A "/data/storage/el2/base/files/"
#define SW_DIR_B "/data/app/el2/100/base/com.vibecc.smo/files/"

#define TRI_W 1600
#define TRI_H 900
#define TRI_FRAMES 5
// VkClearColorValue.float32 is NORMALISED (0..1): handing it 8-bit values (16, 16, 48) is undefined and
// SwiftShader clamped the whole thing to white — that is why the background read 255,255,255 while the drawn
// triangle was exact (logs/m42a/l, /m, /n). The expected 8-bit colour is still (16, 16, 48).
#define CLEAR_R (16.0f / 255.0f)
#define CLEAR_G (16.0f / 255.0f)
#define CLEAR_B (48.0f / 255.0f)

// ---- switches: env first, then the sandbox file of the same name ----
static int sw_get_int(const char *name, int dflt) {
    const char *e = getenv(name);
    if (e && *e) return atoi(e);
    char p[256];
    snprintf(p, sizeof p, "%s%s", SW_DIR_B, name);
    FILE *f = fopen(p, "r");
    if (!f) { snprintf(p, sizeof p, "%s%s", SW_DIR_A, name); f = fopen(p, "r"); }
    if (!f) return dflt;
    int v = dflt;
    if (fscanf(f, "%d", &v) != 1) v = dflt;
    fclose(f);
    return v;
}

// Seeds the switch files so `hdc shell "echo 1 > ..."` works on the first run (same convention as smo_m40az).
__attribute__((constructor)) static void vk2_seed_switches(void) {
    static const char *const names[] = {"DM_VK_SHOT", "DM_VK2_SELFTEST", "DM_VK2_MODE_CLEAR", "DM_VK2_ALIAS"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        static const char *const dirs[] = {SW_DIR_A, SW_DIR_B};
        for (unsigned d = 0; d < 2; d++) {
            char p[256];
            snprintf(p, sizeof p, "%s%s", dirs[d], names[i]);
            int fd = open(p, O_CREAT | O_WRONLY, 0666);
            if (fd >= 0) { close(fd); chmod(p, 0666); }
        }
    }
}

static void tri_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tri_log(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    LOG("%s", b);
}

#define STEP(name) tri_log("[vk2] step: %s", name)

static int write_ppm(const char *path, const uint8_t *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { tri_log("[RED] [vk2] cannot write %s", path); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    // B8G8R8A8 memory -> P6 wants RGB
    uint8_t *row = malloc((size_t)w * 3);
    if (!row) { fclose(f); return -1; }
    for (int y = 0; y < h; y++) {
        const uint8_t *s = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) { row[x * 3 + 0] = s[x * 4 + 2]; row[x * 3 + 1] = s[x * 4 + 1]; row[x * 3 + 2] = s[x * 4 + 0]; }
        fwrite(row, 1, (size_t)w * 3, f);
    }
    free(row);
    fclose(f);
    return 0;
}

static VkShaderModule mk_module(VkDevice dev, const unsigned char *bytes, unsigned nbytes) {
    // tri_spv.h stores bytes; SPIR-V words must be 4-byte aligned, so copy through malloc'd storage.
    uint32_t nw = nbytes / 4;
    uint32_t *w = malloc((size_t)nw * 4);
    if (!w) return VK_NULL_HANDLE;
    memcpy(w, bytes, nbytes);
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)nw * 4, w};
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(dev, &ci, NULL, &m);
    free(w);
    if (r) { tri_log("[RED] [vk2] vkCreateShaderModule -> %d", (int)r); return VK_NULL_HANDLE; }
    return m;
}

// Copies the shot from the el2 storage dir into the app's own files dir a little later, when that dir exists and
// is writable (see run_selftest). pthread, so it never blocks the constructor.
static void *shot_copy_later(void *arg) {
    char *src = (char *)arg;
    sleep(15);
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    char dst[512];
    snprintf(dst, sizeof dst, "%s%s", SW_DIR_B, base);
    FILE *in = fopen(src, "rb");
    if (!in) { tri_log("[RED] [vk2] copy-back: cannot reopen %s", src); return NULL; }
    FILE *out = fopen(dst, "wb");
    if (!out) { tri_log("[RED] [vk2] copy-back: cannot create %s", dst); fclose(in); return NULL; }
    char buf[65536];
    size_t n, total = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) { if (fwrite(buf, 1, n, out) != n) { total = 0; break; } total += n; }
    fclose(in);
    fclose(out);
    tri_log("[vk2] copy-back: %s -> %s (%zu bytes)", src, dst, total);
    return NULL;
}

static int run_selftest(void) {
    int shot_frame = sw_get_int("DM_VK_SHOT", 0);
    int mode_draw = sw_get_int("DM_VK2_MODE_CLEAR", 0) ? 0 : 1;
    int variant = sw_get_int("DM_VK2_V", 0);
    tri_log("[vk2] mode: %s, variant=%d", mode_draw ? "draw (triangle)" : "clear only (no draw)", variant);
    const char *dir = getenv("DM_VK_SHOT_DIR");
    // The el2 storage dir is the only one writable this early (the selftest runs from a load-time constructor,
    // before the app framework prepares /data/app/el2/100/base/<bundle>/files — run k's write there failed).
    // But `hdc file recv` as the shell user cannot read el2 (run j: permission denied), so after writing here a
    // helper thread re-copies the file into the app's own files dir once the framework is up.
    if (!dir || !*dir) dir = SW_DIR_A;
    tri_log("[vk2] selftest start: W=%d H=%d frames=%d shot=%d dir=%s", TRI_W, TRI_H, TRI_FRAMES, shot_frame, dir);

    // Branch B is the default delivery (plan §8 R5): no zero-copy, no surface, no swapchain.
    setenv("DM_VK_NO_ZEROCOPY", "1", 1);
    setenv("DM_VK_SHOT", "0", 1);   // the vendored core must not try its own (Android) shot path
    vk_boot_log_open();
    // Our own JIT rule for the bundled SwiftShader (platform/vk_boot.c:196): Reactor cannot allocate
    // executable pages on OHOS, so it compiles into files under SMO_JIT_DIR. This is a *process-wide* setting
    // that must be in place before the ICD is dlopen'd: when the selftest ran without it, instance/device/
    // pipeline creation all succeeded and the first vkCmdDraw crashed inside libvk_swiftshader.so on a NULL
    // routine (logs/m42a/c..f), while a submit with no draw passed (logs/m42a/h) — i.e. clear-then-crash.
    if (vk_boot_jit_env_setup()) tri_log("[RED] [vk2] SMO_JIT_DIR setup failed: the shader JIT may crash");
    vk_init();                     // gfx_vk_core.c: instance (OHOS bootstrap), device, features, completion thread
    tri_log("[vk2] device up: %s, Vulkan %u.%u, device 0x%p", V.props.deviceName,
            VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion), (void *)V.dev);
    tri_log("[vk2] boot zero_copy=0 branch=B (DM_VK_NO_ZEROCOPY=1: pools keep a host-visible shadow copy)");

    // A memory pool through the backend's own path: proves which pool mode this run really took.
    void *pool_mem = NULL;
    size_t pool_len = 1u << 20;
    if (posix_memalign(&pool_mem, 4096, pool_len) == 0) {
        memset(pool_mem, 0, pool_len);
        mtl_pool_add(pool_mem, pool_len);   // gfx_vk_core.c:630 -> zc = -4 (DM_VK_NO_ZEROCOPY) -> copied pool
        tri_log("[vk2] mtl_pool_add(%zu) done: zero_copy=0 expected above", pool_len);
    } else tri_log("[RED] [vk2] posix_memalign for the probe pool failed");

    VkDevice dev = V.dev;
    VkResult r;
    int rc = -1;
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory img_mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkCommandPool cp = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkBuffer rb = VK_NULL_HANDLE;
    VkDeviceMemory rb_mem = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkDeviceSize rb_size = (VkDeviceSize)TRI_W * TRI_H * 4;
    void *mapped = NULL;

    // ---- render target ----
    STEP("vkCreateImage");
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D,
                             VK_FORMAT_B8G8R8A8_UNORM, {(uint32_t)TRI_W, (uint32_t)TRI_H, 1}, 1, 1,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                             VK_IMAGE_LAYOUT_UNDEFINED};
    r = vkCreateImage(dev, &ici, NULL, &img);
    if (r) { tri_log("[RED] [vk2] vkCreateImage -> %d", (int)r); goto done; }
    STEP("vkGetImageMemoryRequirements");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img, &req);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                                vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    tri_log("[vk2] image memory: req.size=%llu bits=%#x -> type %u (count %u)", (unsigned long long)req.size, req.memoryTypeBits, mai.memoryTypeIndex, V.mem.memoryTypeCount);
    STEP("vkAllocateMemory(image)");
    r = vkAllocateMemory(dev, &mai, NULL, &img_mem);
    if (r) { tri_log("[RED] [vk2] image memory -> %d", (int)r); goto done; }
    STEP("vkBindImageMemory");
    vkBindImageMemory(dev, img, img_mem, 0);
    STEP("vkCreateImageView");
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D,
                                 VK_FORMAT_B8G8R8A8_UNORM, {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                 VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    r = vkCreateImageView(dev, &vci, NULL, &view);
    if (r) { tri_log("[RED] [vk2] vkCreateImageView -> %d", (int)r); goto done; }

    // ---- render pass / framebuffer ----
    STEP("vkCreateRenderPass");
    // LOAD, not CLEAR: SwiftShader's render-pass clear delivered white (255,255,255) for a (16,16,48) clear
    // value — twice, with and without a following barrier (logs/m42a/l and /m) — while the drawn pixels were
    // exact. The frame therefore clears the image explicitly with vkCmdClearColorImage first (see the loop).
    VkAttachmentDescription att = {0, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD,
                                   VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
    VkRenderPassCreateInfo rci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sd, 0, NULL};
    r = vkCreateRenderPass(dev, &rci, NULL, &rp);
    if (r) { tri_log("[RED] [vk2] vkCreateRenderPass -> %d", (int)r); goto done; }
    STEP("vkCreateFramebuffer");
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, TRI_W, TRI_H, 1};
    r = vkCreateFramebuffer(dev, &fci, NULL, &fb);
    if (r) { tri_log("[RED] [vk2] vkCreateFramebuffer -> %d", (int)r); goto done; }

    // ---- the fixed pipeline ----
    STEP("vkCreatePipelineLayout");
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL};
    r = vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    if (r) { tri_log("[RED] [vk2] vkCreatePipelineLayout -> %d", (int)r); goto done; }
    STEP("shader modules");
    vs = mk_module(dev, tri_vert_spv, tri_vert_spv_len);
    fs = mk_module(dev, tri_frag_spv, tri_frag_spv_len);
    if (!vs || !fs) goto done;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkViewport vp = {0, 0, TRI_W, TRI_H, 0, 1};
    VkRect2D sc = {{0, 0}, {TRI_W, TRI_H}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0,
                                                 VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE,
                                                 VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0,
                                               VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0, NULL, VK_FALSE, VK_FALSE};
    VkPipelineColorBlendAttachmentState cba = {VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
                                               VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xf};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0,
                                               VK_FALSE, VK_LOGIC_OP_COPY, 1, &cba, {0, 0, 0, 0}};
    VkGraphicsPipelineCreateInfo gpci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vi, &ia, NULL, &vps,
                                         &rs, &ms, NULL, &cbs, NULL, pl, rp, 0, VK_NULL_HANDLE, -1};
    STEP("vkCreateGraphicsPipelines");
    r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
    if (r) { tri_log("[RED] [vk2] vkCreateGraphicsPipelines -> %d", (int)r); goto done; }

    // ---- readback buffer (host visible) ----
    STEP("readback buffer");
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, rb_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    r = vkCreateBuffer(dev, &bci, NULL, &rb);
    if (r) { tri_log("[RED] [vk2] vkCreateBuffer -> %d", (int)r); goto done; }
    VkMemoryRequirements breq;
    vkGetBufferMemoryRequirements(dev, rb, &breq);
    VkMemoryAllocateInfo bmai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, breq.size,
                                 vk_mem_type(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
    r = vkAllocateMemory(dev, &bmai, NULL, &rb_mem);
    if (r) { tri_log("[RED] [vk2] buffer memory -> %d", (int)r); goto done; }
    vkBindBufferMemory(dev, rb, rb_mem, 0);
    r = vkMapMemory(dev, rb_mem, 0, rb_size, 0, &mapped);
    if (r) { tri_log("[RED] [vk2] vkMapMemory -> %d", (int)r); goto done; }

    // ---- command buffer + fence ----
    STEP("command pool/fence");
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, V.qf};
    r = vkCreateCommandPool(dev, &cpi, NULL, &cp);
    if (r) { tri_log("[RED] [vk2] vkCreateCommandPool -> %d", (int)r); goto done; }
    VkCommandBufferAllocateInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    r = vkAllocateCommandBuffers(dev, &cbi, &cb);
    if (r) { tri_log("[RED] [vk2] vkAllocateCommandBuffers -> %d", (int)r); goto done; }
    VkFenceCreateInfo fci2 = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    r = vkCreateFence(dev, &fci2, NULL, &fence);
    if (r) { tri_log("[RED] [vk2] vkCreateFence -> %d", (int)r); goto done; }

    // ---- draw N frames; each submit->fence is timed (g_ns_submit) ----
    uint64_t g_ns_submit = 0;
    int frames_done = 0;
    for (int f = 1; f <= TRI_FRAMES; f++) {
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        if ((r = vkBeginCommandBuffer(cb, &bi))) { tri_log("[RED] [vk2] vkBeginCommandBuffer -> %d", (int)r); goto done; }
        VkClearValue cv = {{{CLEAR_R, CLEAR_G, CLEAR_B, 1.0f}}};
        // 1) the image becomes a transfer destination, 2) explicit clear, 3) it becomes a colour attachment
        VkImageMemoryBarrier pre = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                    f == 1 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &pre);
        vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv.color, 1, &pre.subresourceRange);
        VkImageMemoryBarrier toca = pre;
        toca.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toca.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toca.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toca.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &toca);
        VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {(uint32_t)TRI_W, (uint32_t)TRI_H}}, 1, &cv};
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        // DM_VK2_MODE=clear: a bisection mode that keeps every resource (pipeline, framebuffer, render pass) but
        // draws nothing, so a crash can be attributed to the draw or to the submit path itself.
        if (mode_draw && variant != 1) {              // variant 1: bind only, no draw
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdDraw(cb, variant == 2 ? 0 : 3, 1, 0, 0);   // variant 2: zero vertices (pipeline setup, no raster)
        }
        vkCmdEndRenderPass(cb);
        // A memory dependency between the colour writes of the pass and the copy that reads them. The render
        // pass's final layout transition orders the *layout*, not the writes against a later transfer read, and
        // without it SwiftShader's copy read the untouched background as 0xFFFFFFFF (white) instead of the
        // clear colour — the triangle was right and the clear area was not (logs/m42a/l).
        VkImageMemoryBarrier imb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &imb);
        VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)TRI_W, (uint32_t)TRI_H, 1}};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &bic);
        if ((r = vkEndCommandBuffer(cb))) { tri_log("[RED] [vk2] vkEndCommandBuffer -> %d", (int)r); goto done; }

        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        STEP("vkQueueSubmit");
        uint64_t t0 = now_ns();
        if ((r = vkQueueSubmit(V.q, 1, &si, fence))) { tri_log("[RED] [vk2] vkQueueSubmit -> %d", (int)r); goto done; }
        tri_log("[vk2] frame %d: submitted, waiting on the fence", f);
        r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);   // 5 s: a deadlocked chain must fail loudly, not hang the app
        if (r) { tri_log("[RED] [vk2] vkWaitForFences -> %d (5 s timeout: the submit/wait chain is stuck)", (int)r); goto done; }
        uint64_t dt = now_ns() - t0;
        vkResetFences(dev, 1, &fence);
        g_ns_submit = dt;
        frames_done = f;
        tri_log("[vk2] frame %d: g_ns_submit=%llu ns (%.2f ms)",
                f, (unsigned long long)g_ns_submit, (double)g_ns_submit / 1e6);
        if (shot_frame == f) {
            char path[512];
            snprintf(path, sizeof path, "%snvn_vk2_tri_%d.ppm", dir, f);
            if (write_ppm(path, (const uint8_t *)mapped, TRI_W, TRI_H) == 0) {
                tri_log("[vk2] shot: %s (%dx%d, %llu bytes)", path, TRI_W, TRI_H, (unsigned long long)rb_size);
                char *arg = strdup(path);
                pthread_t t;
                if (arg && pthread_create(&t, NULL, shot_copy_later, arg) == 0) pthread_detach(t);
            }
            else tri_log("[RED] [vk2] shot write failed: %s", path);
        }
    }
    tri_log("[vk2] selftest done: frames=%d last g_ns_submit=%llu ns (%.2f ms), budget 16.7 ms -> %s",
            frames_done, (unsigned long long)g_ns_submit, (double)g_ns_submit / 1e6, g_ns_submit < 16700000ull ? "OK" : "OVER");
    rc = 0;

done:
    if (mapped) {
        PFN_vkUnmapMemory unmap = (PFN_vkUnmapMemory)vkGetDeviceProcAddr(dev, "vkUnmapMemory");
        if (unmap) unmap(dev, rb_mem);
    }
    if (fence) vkDestroyFence(dev, fence, NULL);
    if (rb) vkDestroyBuffer(dev, rb, NULL);
    if (rb_mem) vkFreeMemory(dev, rb_mem, NULL);
    if (cp) {
        PFN_vkDestroyCommandPool dp = (PFN_vkDestroyCommandPool)vkGetDeviceProcAddr(dev, "vkDestroyCommandPool");
        if (dp) dp(dev, cp, NULL);
    }
    if (pipe) vkDestroyPipeline(dev, pipe, NULL);
    if (pl) {
        PFN_vkDestroyPipelineLayout dpl = (PFN_vkDestroyPipelineLayout)vkGetDeviceProcAddr(dev, "vkDestroyPipelineLayout");
        if (dpl) dpl(dev, pl, NULL);
    }
    if (vs) vkDestroyShaderModule(dev, vs, NULL);
    if (fs) vkDestroyShaderModule(dev, fs, NULL);
    if (fb) vkDestroyFramebuffer(dev, fb, NULL);
    if (rp) vkDestroyRenderPass(dev, rp, NULL);
    if (view) vkDestroyImageView(dev, view, NULL);
    if (img) vkDestroyImage(dev, img, NULL);
    if (img_mem) vkFreeMemory(dev, img_mem, NULL);
    if (pool_mem) { mtl_pool_del(pool_mem); free(pool_mem); }
    if (rc) tri_log("[RED] [vk2] selftest FAILED (rc=%d)", rc);
    return rc;
}

// M42A: the selftest runs at library load time, i.e. BEFORE this app's own nvn_vk backend makes its Vulkan
// instance/device on the same bundled SwiftShader ICD. Two concurrent Vulkan users in one process crashed
// inside libvk_swiftshader.so (sig11 addr=0 pc=+0x790aa0, right after our vkQueueSubmit — logs/m42a/c and /d),
// and this slice may not touch the game's bring-up to avoid it. Loading-time execution keeps the two strictly
// sequential and changes nothing for a run with the switch off (DM_VK2_SELFTEST=0, the default).
__attribute__((constructor)) static void vk2_selftest_maybe(void) {
    if (!sw_get_int("DM_VK2_SELFTEST", 0)) { tri_log("[vk2] selftest off (DM_VK2_SELFTEST=0)"); return; }
    run_selftest();
}

// ============================ M58A / judgment A ============================
// Question: in THIS driver (bundled SwiftShader), do two images bound to ONE VkDeviceMemory at different
// offsets really share bytes? M57A saw a game slab where A(+0x200200) and B(+0x228600) had the same
// VkDeviceMemory yet B read back all zeros, so this is tested in isolation: two 1024x1024 D32_SFLOAT images
// with VK_IMAGE_CREATE_ALIAS_BIT, bound together at offset 0 and +0x100000.
// Every number below is read back from real image bytes through transfer copies (never through a render pass),
// so a private fast-clear/LRZ state cannot fake the answer. Steps:
//   1. clear A=0.25 and B=0.75, read both
//        A=0.25, B=0.75  -> bytes NOT shared (each image owns its storage; offsets are honoured, quietly)
//        A=B=0.75        -> shared AND the clear lands in memory at the same place (offset 0 collision) 
//        A=B=0.25        -> shared storage but B's clear went somewhere else
//   2. transfer-copy 0.5 (pixel 0 = 0.875) into A, read both; 3. transfer-copy 0.625 (pixel 0 = 0.9375) into B, read both
// A transfer copy writes bytes, so a copy that shows up in the other image proves byte sharing even if clears
// are private state.
// Switch: DM_VK2_ALIAS=1 (default 0: nothing runs; the selftest and the game stay untouched).
#define AL_W 1024
#define AL_H 1024
#define AL_OFF_B 0x100000ull
#define AL_PIX ((size_t)AL_W * AL_H)

static void al_stat(const char *tag, const float *p) {
    float mn = 1e30f, mx = -1e30f;
    double sum = 0.0;
    size_t c25 = 0, c50 = 0, c62 = 0, c75 = 0, c87 = 0, c93 = 0, cz = 0;
    for (size_t i = 0; i < AL_PIX; i++) {
        float v = p[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
        if (v == 0.0f) cz++;
        else if (v > 0.24f && v < 0.26f) c25++;
        else if (v > 0.49f && v < 0.51f) c50++;
        else if (v > 0.61f && v < 0.64f) c62++;
        else if (v > 0.74f && v < 0.76f) c75++;
        else if (v > 0.86f && v < 0.89f) c87++;
        else if (v > 0.92f && v < 0.95f) c93++;
    }
    tri_log("[m58a-al] %s: min=%.6f max=%.6f mean=%.6f zero=%zu n.25=%zu n.5=%zu n.625=%zu n.75=%zu n.875=%zu n.9375=%zu | p0=%.6f p1=%.6f p2=%.6f p3=%.6f prow1=%.6f",
            tag, mn, mx, sum / (double)AL_PIX, cz, c25, c50, c62, c75, c87, c93,
            p[0], p[1], p[2], p[3], p[AL_W]);
}

static void al_bar(VkCommandBuffer cb, VkImage img, VkImageLayout oldL, VkImageLayout newL,
                   VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, src, dst, oldL, newL,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                              {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

static VkImage al_mkimg(VkDevice dev, const char *name) {
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_ALIAS_BIT,
                             VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, {(uint32_t)AL_W, (uint32_t)AL_H, 1}, 1, 1,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img = VK_NULL_HANDLE;
    VkResult r = vkCreateImage(dev, &ici, NULL, &img);
    tri_log("[m58a-al] vkCreateImage(%s, ALIAS_BIT|DEPTH_ATTACH|XFER_SRC|XFER_DST) -> %d img=%p", name, (int)r, (void *)(uintptr_t)img);
    return r ? VK_NULL_HANDLE : img;
}

static int run_alias_test(void) {
    tri_log("[m58a-al] === judgment A: two 1024x1024 D32 images on ONE VkDeviceMemory, offsets 0 and %#llx",
            (unsigned long long)AL_OFF_B);
    setenv("DM_VK_NO_ZEROCOPY", "1", 1);
    setenv("DM_VK_SHOT", "0", 1);
    vk_boot_log_open();
    if (vk_boot_jit_env_setup()) tri_log("[RED] [m58a-al] SMO_JIT_DIR setup failed");
    vk_init();
    VkDevice dev = V.dev;
    tri_log("[m58a-al] device up: %s", V.props.deviceName);

    VkImage ia = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBuffer rba = VK_NULL_HANDLE, rbb = VK_NULL_HANDLE, fill = VK_NULL_HANDLE;
    VkDeviceMemory rba_m = VK_NULL_HANDLE, rbb_m = VK_NULL_HANDLE, fill_m = VK_NULL_HANDLE;
    VkCommandPool cp = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void *pa = NULL, *pb = NULL, *pf = NULL;
    int rc = -1;

    ia = al_mkimg(dev, "A");
    ib = al_mkimg(dev, "B");
    if (!ia || !ib) goto done;

    VkMemoryRequirements ra, rb;
    vkGetImageMemoryRequirements(dev, ia, &ra);
    vkGetImageMemoryRequirements(dev, ib, &rb);
    tri_log("[m58a-al] reqA: size=%llu align=%llu bits=%#x | reqB: size=%llu align=%llu bits=%#x",
            (unsigned long long)ra.size, (unsigned long long)ra.alignment, ra.memoryTypeBits,
            (unsigned long long)rb.size, (unsigned long long)rb.alignment, rb.memoryTypeBits);

    VkDeviceSize need = rb.size + (VkDeviceSize)AL_OFF_B;
    if (ra.size > need) need = ra.size;
    VkDeviceSize align = ra.alignment > rb.alignment ? ra.alignment : rb.alignment;
    if (align < 1) align = 1;
    need = (need + align - 1) / align * align;
    uint32_t bits = ra.memoryTypeBits & rb.memoryTypeBits;
    uint32_t ti = vk_mem_type(bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, need, ti};
    VkResult r = vkAllocateMemory(dev, &mai, NULL, &mem);
    tri_log("[m58a-al] vkAllocateMemory(ONE block, size=%llu type=%u from bits=%#x) -> %d mem=%p",
            (unsigned long long)need, ti, bits, (int)r, (void *)(uintptr_t)mem);
    if (r) goto done;
    VkResult ba = vkBindImageMemory(dev, ia, mem, 0);
    VkResult bb = vkBindImageMemory(dev, ib, mem, (VkDeviceSize)AL_OFF_B);
    tri_log("[m58a-al] vkBindImageMemory(A @0) -> %d ; vkBindImageMemory(B @%#llx same mem) -> %d",
            (int)ba, (unsigned long long)AL_OFF_B, (int)bb);
    if (ba || bb) goto done;

    // readback + fill buffers (host visible)
    VkDeviceSize img_bytes = (VkDeviceSize)AL_W * AL_H * 4;
    VkDeviceMemory *rms[2] = {&rba_m, &rbb_m};
    VkBuffer *rbs[2] = {&rba, &rbb};
    for (int i = 0; i < 2; i++) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, img_bytes,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        if (vkCreateBuffer(dev, &bci, NULL, rbs[i])) goto done;
        VkMemoryRequirements br;
        vkGetBufferMemoryRequirements(dev, *rbs[i], &br);
        VkMemoryAllocateInfo bmi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, br.size,
                                    vk_mem_type(br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
        if (vkAllocateMemory(dev, &bmi, NULL, rms[i])) goto done;
        if (vkBindBufferMemory(dev, *rbs[i], *rms[i], 0)) goto done;
    }
    {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, img_bytes,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        if (vkCreateBuffer(dev, &bci, NULL, &fill)) goto done;
        VkMemoryRequirements br;
        vkGetBufferMemoryRequirements(dev, fill, &br);
        VkMemoryAllocateInfo bmi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, br.size,
                                    vk_mem_type(br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
        if (vkAllocateMemory(dev, &bmi, NULL, &fill_m)) goto done;
        if (vkBindBufferMemory(dev, fill, fill_m, 0)) goto done;
    }
    if (vkMapMemory(dev, rba_m, 0, img_bytes, 0, &pa)) goto done;
    if (vkMapMemory(dev, rbb_m, 0, img_bytes, 0, &pb)) goto done;
    if (vkMapMemory(dev, fill_m, 0, img_bytes, 0, &pf)) goto done;
    memset(pa, 0xff, (size_t)img_bytes);   // 0xff = NaN pattern: an untouched readback is recognisable
    memset(pb, 0xff, (size_t)img_bytes);

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, V.qf};
    if (vkCreateCommandPool(dev, &cpi, NULL, &cp)) goto done;
    VkCommandBufferAllocateInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vkAllocateCommandBuffers(dev, &cbi, &cb)) goto done;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    if (vkCreateFence(dev, &fci, NULL, &fence)) goto done;

    VkBufferImageCopy bicA = {0, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)AL_W, (uint32_t)AL_H, 1}};
    VkImageLayout la = VK_IMAGE_LAYOUT_UNDEFINED, lb = VK_IMAGE_LAYOUT_UNDEFINED;

    // --- step 0: read both before anything is written (are they zero filled? same buffer?) ---
    for (int step = 0; step <= 3; step++) {
        if (vkResetCommandBuffer(cb, 0)) goto done;
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        if (vkBeginCommandBuffer(cb, &bi)) goto done;
        if (step == 0) {
            al_bar(cb, ia, la, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT);
            al_bar(cb, ib, lb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT);
            la = lb = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        } else if (step == 1) {
            // clear A=0.25 and B=0.75 in one submit
            al_bar(cb, ia, la, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            al_bar(cb, ib, lb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            la = lb = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            VkClearDepthStencilValue ca = {0.25f, 0}, cbv = {0.75f, 0};
            VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCmdClearDepthStencilImage(cb, ia, la, &ca, 1, &rng);
            vkCmdClearDepthStencilImage(cb, ib, lb, &cbv, 1, &rng);
            al_bar(cb, ia, la, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            al_bar(cb, ib, lb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            la = lb = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        } else {
            // step 2: transfer-copy 0.5 (pixel0 0.875) into A ; step 3: 0.625 (pixel0 0.9375) into B
            VkImage dst = (step == 2) ? ia : ib;
            float *fp = (float *)pf;
            for (size_t i = 0; i < AL_PIX; i++) fp[i] = (step == 2) ? 0.5f : 0.625f;
            fp[0] = (step == 2) ? 0.875f : 0.9375f;
            al_bar(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy bic = bicA;
            vkCmdCopyBufferToImage(cb, fill, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
            al_bar(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        }
        vkCmdCopyImageToBuffer(cb, ia, la, rba, 1, &bicA);
        vkCmdCopyImageToBuffer(cb, ib, lb, rbb, 1, &bicA);
        if (vkEndCommandBuffer(cb)) goto done;
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        if (vkQueueSubmit(V.q, 1, &si, fence)) { tri_log("[RED] [m58a-al] vkQueueSubmit(step %d) failed", step); goto done; }
        r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
        if (r) { tri_log("[RED] [m58a-al] vkWaitForFences(step %d) -> %d", step, (int)r); goto done; }
        vkResetFences(dev, 1, &fence);
        char t[96];
        snprintf(t, sizeof t, "step%d-A", step);
        al_stat(t, (const float *)pa);
        snprintf(t, sizeof t, "step%d-B", step);
        al_stat(t, (const float *)pb);
    }
    tri_log("[m58a-al] interpretation: step1 A=0.25/B=0.75 -> NOT shared; A=B (both 0.75 or both 0.25) -> shared; "
            "step2/3 a transfer copy into one image showing up in the other -> byte sharing");
    rc = 0;

done:
    {
        PFN_vkUnmapMemory unmap = (PFN_vkUnmapMemory)vkGetDeviceProcAddr(dev, "vkUnmapMemory");
        if (unmap) {
            if (pa) unmap(dev, rba_m);
            if (pb) unmap(dev, rbb_m);
            if (pf) unmap(dev, fill_m);
        }
    }
    if (fence) vkDestroyFence(dev, fence, NULL);
    if (cp) {
        PFN_vkDestroyCommandPool dp = (PFN_vkDestroyCommandPool)vkGetDeviceProcAddr(dev, "vkDestroyCommandPool");
        if (dp) dp(dev, cp, NULL);
    }
    if (rba) vkDestroyBuffer(dev, rba, NULL);
    if (rbb) vkDestroyBuffer(dev, rbb, NULL);
    if (fill) vkDestroyBuffer(dev, fill, NULL);
    if (rba_m) vkFreeMemory(dev, rba_m, NULL);
    if (rbb_m) vkFreeMemory(dev, rbb_m, NULL);
    if (fill_m) vkFreeMemory(dev, fill_m, NULL);
    if (ia) vkDestroyImage(dev, ia, NULL);
    if (ib) vkDestroyImage(dev, ib, NULL);
    if (mem) vkFreeMemory(dev, mem, NULL);
    tri_log("[m58a-al] alias test done rc=%d", rc);
    return rc;
}

__attribute__((constructor)) static void vk2_alias_maybe(void) {
    if (!sw_get_int("DM_VK2_ALIAS", 0)) return;   // default 0:完全静默
    if (sw_get_int("DM_VK2_SELFTEST", 0)) { tri_log("[m58a-al] DM_VK2_SELFTEST=1 also set -> alias test skipped (one device per run)"); return; }
    run_alias_test();
}
