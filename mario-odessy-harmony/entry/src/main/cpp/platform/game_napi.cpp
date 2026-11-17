// game_napi.cpp —— ArkTS <-> 游戏宿主 NAPI 桥
// 今晚接通：start(dir) 在独立线程跑 loader 的完整 boot 序列（TOTK_HEADLESS=1，CPU 侧）；
// XComponent surface 句柄经 OH_NativeXComponent 回调收下（swapchain 周末接 nvn_vk）。
// 接线模式抄自 jhaizhou-ops/d2r-mobile dm/harmony/native/napi_ohos.c。
#include "napi/native_api.h"
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>
#include <unistd.h>

#define LOG_DOMAIN 0x0000
#define LOG_TAG "GameHost"   /* 中性名：平台 NAPI 桥全游戏共用（原 SmoGame 为 SMO 遗留） */
#define HI(fmt, ...) ((void)OH_LOG_INFO(LOG_APP, fmt, ##__VA_ARGS__))
#define HE(fmt, ...) ((void)OH_LOG_ERROR(LOG_APP, fmt, ##__VA_ARGS__))

extern "C" int runtime_main(const char *rewritten_dir, const char *mode);   /* platform/loader.c（原 smo_runtime_main，两处同改防链接错） */
extern "C" void runtime_dump_threads(void);   /* platform/threads.c：guest 线程/锁持有者转储（挂起取证） */
extern "C" void runtime_nvn_stub_report(void);   /* platform/gfx_bridge.c：表外桩调用计数报告（终局 B 计划） */
extern "C" __attribute__((weak)) void runtime_nvn_stub_report(void) {}   // x86_64 壳 weak 兜底（PITFALLS G4）
// x86_64 壳 target 不编 platform 宿主源，与 runtime_main 同款 weak 兜底（真机 arm64 链接真身）
extern "C" __attribute__((weak)) void runtime_dump_threads(void) {}
// Vulkan 窗口接线（round23批1落点1）：surface 到手/销毁转发 nvn_vk 后端，窗口到达触发懒起层。
// arm64 链 nvn_vk.c 真身；x86_64 壳 target 不编 nvn_vk.c，弱符号空实现兜底（runtime_main 同款模式）
extern "C" __attribute__((weak)) void nvn_vk_set_native_window(void *w, uint32_t width, uint32_t height) {
    (void)w; (void)width; (void)height;
}
// 游戏专属 env 配置钩子：donor 对拍地址（TOTK_DUMP_AT 0x131f318 族）等 per-game 配置
// 由 games/<tid>/game_env.c 提供强符号；平台默认空实现，换游戏零改本文件。
extern "C" __attribute__((weak)) void game_configure_env(const char *dir) { (void)dir; }
#ifndef HOST_ARCH
// x86_64 模拟器不编宿主（游戏镜像是 ARM64）：弱符号兜底，arm64 上由 loader.c 的强符号覆盖
extern "C" __attribute__((weak)) int runtime_main(const char *rewritten_dir, const char *mode) {
    (void)rewritten_dir; (void)mode;
    HE("emulator shell only: host runtime not built for x86_64");
    return 2;
}
#endif

static std::atomic<bool> g_running{false};
static std::atomic<int> g_rc{-1};

/* ---- stderr -> hilog：loader 全部诊断（[smo]/[totk]/[odyssey]/[ohos-shim]/FAIL）走 fprintf(stderr)，
   OHOS 上 fd2 无人消费（boot_round3：exit(1) 的 FAIL 文本丢失，0 条原生行）。pipe+转发线程，
   Start() 装一次。stdout 不动（probe 的 JSON 输出本工程用不到）。 ---- */
static void redirect_stderr_to_hilog() {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) return;
    int fds[2];
    if (pipe(fds) != 0) { HE("stderr bridge: pipe failed"); return; }
    // round43 渐冻机制（crash_1031）：转发线程的 HI() 在 hilog 背压下若阻塞 → 管道(64K)满 →
    // 全进程 fprintf(stderr) 阻塞在 write() → 游戏线程渐冻（音频心跳停=读端死铁证）。
    // 写端设非阻塞：满则 glibc stdio 丢弃该次输出（丢日志换线程活性），转发线程死不再拖死全场。
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    dup2(fds[1], 2);
    close(fds[1]);
    std::thread([rfd = fds[0]] {
        char buf[1024];
        size_t len = 0;
        for (;;) {
            ssize_t n = read(rfd, buf + len, sizeof(buf) - len);
            if (n <= 0) break;
            len += (size_t)n;
            size_t start = 0;
            for (size_t i = 0; i < len; i++)
                if (buf[i] == '\n') {
                    buf[i] = '\0';
                    if (i > start) HI("%{public}s", buf + start);
                    start = i + 1;
                }
            len -= start;
            if (start) memmove(buf, buf + start, len);
            if (len == sizeof(buf)) len = 0;   // 无换行的超长行：丢弃
        }
    }).detach();
}

/* ---- XComponent surface（d2r-mobile 模式）：窗口句柄先存，供后续 Vulkan swapchain ---- */
static std::atomic<void *> g_window{nullptr};
static std::atomic<int> g_surf_w{0}, g_surf_h{0};

static void surface_set(OH_NativeXComponent *c, void *window) {
    if (window) {
        uint64_t w = 0, h = 0;
        OH_NativeXComponent_GetXComponentSize(c, window, &w, &h);
        g_window.store(window);
        g_surf_w.store((int)w);
        g_surf_h.store((int)h);
        HI("surface ready %{public}llux%{public}llu", (unsigned long long)w, (unsigned long long)h);
        nvn_vk_set_native_window(window, (uint32_t)w, (uint32_t)h);   /* 批1落点1：转发后端（到达即触发懒起层，nvn_vk.c set_native_window） */
    } else {
        g_window.store(nullptr);
        HI("surface destroyed");
        nvn_vk_set_native_window(nullptr, 0, 0);   /* 批1落点1：销毁传 NULL，后端拆交换链（nvn_vk.h:30 注释同款） */
    }
}
static void on_created(OH_NativeXComponent *c, void *window) { surface_set(c, window); }
static void on_changed(OH_NativeXComponent *c, void *window) { surface_set(c, window); }
static void on_destroyed(OH_NativeXComponent *c, void *window) { (void)c; surface_set(c, NULL); }
static void on_touch(OH_NativeXComponent *c, void *window) { (void)c; (void)window; }
static OH_NativeXComponent_Callback g_xcb = {on_created, on_changed, on_destroyed, on_touch};

/* ---- NAPI ---- */
static napi_value Start(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    static char dir[512] = "";
    if (argc >= 1) {
        size_t n = 0;
        napi_get_value_string_utf8(env, argv[0], dir, sizeof(dir), &n);
    }
    if (g_running.load()) { napi_value out; napi_create_int32(env, 0, &out); return out; }
    redirect_stderr_to_hilog();
    HI("start dir=%{public}s", dir);
    setenv("TOTK_HEADLESS", "1", 1);   // 无窗口 boot：不进 frontend，直接 CPU 侧启动序列
    // crash_1031 取证：内存 SVC 打点（GetInfo 节流行/MapPhysicalMemory/SetHeapSize）默认开，
    // 仅日志不改行为；overwrite=0——用户显式设置的环境变量优先（可设 0 关闭）
    setenv("TOTK_MEM_LOG", "1", 0);
    // RomFS：宿主 romfs.c 的 runtime_fs_init 读 TOTK_ROMFS（缺省相对路径在 app 沙箱必落空）；
    // 资源由页面「导入游戏资源」放入 <dir>/romfs（runtime-index.bin + romfs.bin.NNN 分卷）
    {
        static char romfs_dir[600];
        snprintf(romfs_dir, sizeof(romfs_dir), "%s/romfs", dir);
        setenv("TOTK_ROMFS", romfs_dir, 1);
    }
    setenv("TOTK_ROMFS_PATCHES", "0", 1);  // 无补丁 overlay（显式关掉，避免缺省相对路径查找）
    setenv("TOTK_ROMFS_MODS", "0", 1);
    game_configure_env(dir);   // 游戏专属 env（SMO 版=games/0100000000010000/game_env.c 的 TOTK_DUMP_AT donor 对拍窗）
    // 账户/存档档案目录：account.c/fs_host.c 读 TOTK_PROFILE_DIR（缺省相对 "user-data"），
    // appspawn 的 CWD 只读，mkdir 必失败（round23 "FAIL: create local account directory"）；锚到 <dir>/user-data
    {
        static char profile_dir[600];
        snprintf(profile_dir, sizeof(profile_dir), "%s/user-data", dir);
        setenv("TOTK_PROFILE_DIR", profile_dir, 1);
    }
    g_running.store(true);
    g_rc.store(-1);
    std::thread([] {
        int rc = runtime_main(dir, "boot");
        g_rc.store(rc);
        HI("boot thread returned rc=%{public}d", rc);
        g_running.store(false);
    }).detach();
    // 挂起取证（round29）：headless boot 挂起时 hdc shell kill -USR1 被 yama 拒（Operation not
    // permitted）、freeze_watchdog 又要求"已呈现首帧"。宿主内自驱动：TOTK_HANG_DUMP_SECS（默认 60，0 关）
    // 秒后首次转储 guest 线程/锁持有者，此后每该间隔一次——挂起自动出现场，正常运行只是多几段日志。
    {
        const char *hs = getenv("TOTK_HANG_DUMP_SECS");
        int hang_s = hs ? atoi(hs) : 60;
        if (hang_s > 0) {
            std::thread([hang_s] {
                for (;;) {
                    sleep(hang_s);
                    if (!g_running.load()) return;
                    HI("hang watchdog: dumping threads");
                    runtime_dump_threads();
                    runtime_nvn_stub_report();   /* 终局 B 计划：高频表外桩名单 */
                }
            }).detach();
        }
    }
    napi_value out; napi_create_int32(env, 0, &out); return out;
}

static napi_value Stop(napi_env env, napi_callback_info) {
    // TODO: 宿主优雅退出（svc ExitProcess 路径），此处先只报告
    napi_value undef; napi_get_undefined(env, &undef); return undef;
}

static napi_value Status(napi_env env, napi_callback_info) {
    // 0=idle 1=running 2=returned；供 ArkTS 轮询状态文案
    int s = g_running.load() ? 1 : (g_rc.load() >= 0 ? 2 : 0);
    napi_value out; napi_create_int32(env, s, &out); return out;
}

static napi_value SurfaceProbe(napi_env env, napi_callback_info) {
    napi_value out; napi_create_int32(env, g_window.load() ? 1 : 0, &out); return out;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"start", nullptr, Start, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stop", nullptr, Stop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"status", nullptr, Status, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"surfaceProbe", nullptr, SurfaceProbe, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    // XComponent 以本库为 libraryname 加载时，exports 上挂着原生组件句柄（d2r-mobile 模式）
    napi_value xobj;
    OH_NativeXComponent *xc = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xobj) == napi_ok &&
        napi_unwrap(env, xobj, (void **)&xc) == napi_ok && xc) {
        OH_NativeXComponent_RegisterCallback(xc, &g_xcb);
        HI("xcomponent callback registered");
    }
    return exports;
}
EXTERN_C_END

static napi_module gameModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "game",
    .nm_priv = nullptr,
};

extern "C" __attribute__((constructor)) void RegisterGameModule(void) {
    napi_module_register(&gameModule);
}
