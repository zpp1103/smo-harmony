// game_napi.cpp —— ArkTS <-> 游戏宿主 NAPI 桥（骨架）
// 周末接通：host_main() 调用移植后的宿主入口（MacNativeGames games/odyssey/runtime），
// 在独立线程跑游戏循环；XComponent 的 surface 句柄经 nvn_vk 初始化 swapchain。
#include "napi/native_api.h"
#include <thread>
#include <atomic>

static std::atomic<bool> g_running{false};
static std::thread g_game_thread;

// 周日接通后替换为宿主真实入口（返回 0 表示正常退出）
static int host_main_stub(const char *romfs_path) {
    // TODO: host boot sequence:
    //   1) mmap 四个 .patched.bin（main/sdk/subsdk0/rtld，Switch 地址布局）
    //   2) rtld 自举 -> main 入口
    //   3) Horizon HLE svc 表就位；NVN 调用转发 nvn_vk
    (void)romfs_path;
    return 0;
}

static napi_value Start(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    char romfs[512] = "";
    if (argc >= 1) {
        size_t n = 0;
        napi_get_value_string_utf8(env, argv[0], romfs, sizeof(romfs), &n);
    }
    if (g_running.load()) {
        napi_value out; napi_create_int32(env, 0, &out); return out;
    }
    g_running.store(true);
    g_game_thread = std::thread([romfs] {
        host_main_stub(romfs);
        g_running.store(false);
    });
    g_game_thread.detach();
    napi_value out; napi_create_int32(env, 0, &out); return out;
}

static napi_value Stop(napi_env env, napi_callback_info) {
    // TODO: 宿主优雅退出（svc ExitProcess 路径），此处先置标志
    napi_value undef; napi_get_undefined(env, &undef); return undef;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"start", nullptr, Start, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stop", nullptr, Stop, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
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
