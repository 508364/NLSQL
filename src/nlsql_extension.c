/**
 * @file nlsql_extension.c
 * @brief NLSQL 作为 NetLeaf NL 扩展的注册入口（插件共享库入口）
 *
 * NetLeaf 插件系统通过 dlsym("plugin_get_info") 获取描述符；
 * 扩展元数据通过 NL_EXTENSION_DEFINE 注册到 NL 扩展系统。
 */
#include "netleaf_module.h"
#include "netleaf_nlsql.h"
#include <string.h>

static int g_nlsql_ext_initialized = 0;

static int nlsql_ext_init(void) {
    if (g_nlsql_ext_initialized) return 0;
    /* 注册到 NL 扩展系统（宿主 netleaf 提供该符号） */
    nl_extension_register(NL_EXTENSION_GET_INFO(nlsql));
    g_nlsql_ext_initialized = 1;
    return 0;
}

static void nlsql_ext_shutdown(void) {
    if (!g_nlsql_ext_initialized) return;
    nl_extension_unregister("nlsql");
    g_nlsql_ext_initialized = 0;
}

static int nlsql_ext_available(void) { return 1; }

static const char* nlsql_ext_version(void) { return NLSQL_VERSION; }

/* 扩展元数据定义（描述 <=50 汉字） */
NL_EXTENSION_DEFINE(nlsql, "NLSQL", NLSQL_VERSION, "508364",
                    "基于MySQL语义的完全离线嵌入式SQL引擎，兼容方言子集与协议，异步低占用",
                    "Windows,Linux,MacOS",
                    NL_CAP_THREAD_SAFE | NL_CAP_ASYNC | NL_CAP_CLIENT,
                    nlsql_ext_init, nlsql_ext_shutdown,
                    nlsql_ext_available, nlsql_ext_version)

/* 插件描述符（宿主 dlsym 入口） */
#ifdef _WIN32
#define NLSQL_PLUGIN_API __declspec(dllexport)
#else
#define NLSQL_PLUGIN_API __attribute__((visibility("default")))
#endif

NLSQL_PLUGIN_API nl_plugin_info_t* plugin_get_info(void) {
    static nl_plugin_info_t info;
    memset(&info, 0, sizeof(info));
    info.name = "NLSQL";
    info.id = "nlsql_v1";
    info.version = NLSQL_VERSION;
    info.description = "Offline embedded MySQL-compatible SQL engine";
    info.author = "508364";
    info.license = "Apache-2.0";
    info.capabilities = NL_CAP_THREAD_SAFE | NL_CAP_ASYNC | NL_CAP_CLIENT;
    info.platform_windows = 1;
    info.platform_linux = 1;
    info.platform_macos = 1;
    info.min_nl_version = NL_MODULE_VERSION;
    info.init = nlsql_ext_init;
    info.shutdown = nlsql_ext_shutdown;
    info.is_available = nlsql_ext_available;
    info.register_module = NULL;
    return &info;
}

NLSQL_PLUGIN_API int plugin_init(void) { return nlsql_ext_init(); }
NLSQL_PLUGIN_API void plugin_shutdown(void) { nlsql_ext_shutdown(); }
