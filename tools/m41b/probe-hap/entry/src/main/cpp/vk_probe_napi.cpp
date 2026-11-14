// m41b vkprobe napi 胶水：把 C 探针结果交给 ArkTS
#include <napi/native_api.h>
#include <string.h>

extern "C" const char *m41b_probe_run(void);

static napi_value RunProbe(napi_env env, napi_callback_info info) {
    const char *s = m41b_probe_run();
    napi_value ret = nullptr;
    napi_create_string_utf8(env, s ? s : "(null)", NAPI_AUTO_LENGTH, &ret);
    return ret;
}

static napi_value Version(napi_env env, napi_callback_info info) {
    napi_value ret = nullptr;
    napi_create_string_utf8(env, "m41b-vkprobe-1.0", NAPI_AUTO_LENGTH, &ret);
    return ret;
}

static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        { "runProbe", nullptr, RunProbe, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "version",  nullptr, Version,  nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "vkprobe",
    .nm_priv = nullptr,
    .reserved = { 0 },
};

extern "C" __attribute__((constructor)) void RegisterModule(void) {
    napi_module_register(&demoModule);
}
