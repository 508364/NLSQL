/**
 * @file nlsql_ext_wan.c
 * @brief NLSQL 外网网关扩展：经 NetLeaf 主库网络 API 暴露 HTTP 查询接口
 *
 * 约束：不在本扩展内硬编码任何网络地址/端口/密钥。监听端口由调用方传入；
 * 所有套接字与 HTTP 服务能力均通过宿主 NetLeaf 已加载的 nl_server_* API
 * 动态获取（dlsym），本扩展自身不做任何 socket 编程。
 *
 * 用法：
 *   nlsql_ext_wan_start(conn, port);   // 启动网关
 *   nlsql_ext_wan_stop();              // 停止
 *
 * 请求：POST /sql，请求体即 SQL 文本；返回纯文本：
 *   首行 "OK" 或 "ERR <code>"，随后每行制表符分隔的列/值。
 */
#include "netleaf_nlsql.h"
#include "netleaf_nlsql_ext.h"
#include "netleaf_nlsql_platform.h"
#if __has_include("netleaf_module.h")
#include "netleaf_module.h"
#define NLSQL_HAVE_NETLEAF_HEADER 1
#else
#define NLSQL_HAVE_NETLEAF_HEADER 0
#endif

#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define EXT_WAN_ID "nlsql_ext_wan"

typedef struct nl_server nl_server_t;

typedef void (*wan_http_handler)(const char* path, int method, const char* body,
                                 size_t body_size, char** response,
                                 size_t* response_len, void* userdata);

typedef struct {
    nl_server_t* (*create)(int protocol, int port);
    void         (*set_handler)(nl_server_t*, wan_http_handler, void*);
    int          (*start)(nl_server_t*);
    void         (*stop)(nl_server_t*);
    void         (*destroy)(nl_server_t*);
} wan_api_t;

static wan_api_t g_api;
static void* g_lib;
static nl_server_t* g_srv;
static nlsql_conn_t* g_conn;

#define NL_PROTO_HTTP_MIRROR 1 /* NetLeaf nl_protocol_t: TCP=0, HTTP=1 */

static int wan_bind_api(void) {
    if (g_api.create) return 0;
    void* h = nlsql_dl_open(NULL, RTLD_LAZY);
    if (h) {
        g_api.create      = (__typeof__(g_api.create))nlsql_dl_sym(h, "nl_server_create");
        g_api.set_handler = (__typeof__(g_api.set_handler))nlsql_dl_sym(h, "nl_server_set_handler");
        g_api.start       = (__typeof__(g_api.start))nlsql_dl_sym(h, "nl_server_start");
        g_api.stop        = (__typeof__(g_api.stop))nlsql_dl_sym(h, "nl_server_stop");
        g_api.destroy     = (__typeof__(g_api.destroy))nlsql_dl_sym(h, "nl_server_destroy");
    }
    if (!g_api.create) {
        /* 宿主未预加载 NetLeaf：尝试按默认库名装载（不写死绝对路径） */
        void* lib = nlsql_dl_open(NLSQL_NETLEAF_LIB, RTLD_LAZY | RTLD_GLOBAL);
        if (!lib) return -1;
        g_lib = lib;
        g_api.create      = (__typeof__(g_api.create))nlsql_dl_sym(lib, "nl_server_create");
        g_api.set_handler = (__typeof__(g_api.set_handler))nlsql_dl_sym(lib, "nl_server_set_handler");
        g_api.start       = (__typeof__(g_api.start))nlsql_dl_sym(lib, "nl_server_start");
        g_api.stop        = (__typeof__(g_api.stop))nlsql_dl_sym(lib, "nl_server_stop");
        g_api.destroy     = (__typeof__(g_api.destroy))nlsql_dl_sym(lib, "nl_server_destroy");
    }
    return g_api.create ? 0 : -1;
}

/* ---------------- 结果编码 ---------------- */

typedef struct {
    char*  p;
    size_t len, cap;
} obuf_t;

static void ob_put(obuf_t* b, const char* s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 512;
        while (nc < b->len + n + 1) nc *= 2;
        char* np = (char*)realloc(b->p, nc);
        if (!np) return;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void ob_str(obuf_t* b, const char* s) { ob_put(b, s, strlen(s)); }

static void ob_value(obuf_t* b, const nlsql_value_t* v) {
    char tmp[64];
    if (v->is_null || v->type == NLSQL_TYPE_NULL) { ob_str(b, "NULL"); return; }
    switch (v->type) {
        case NLSQL_TYPE_INT64:
            snprintf(tmp, sizeof(tmp), "%lld", (long long)v->i);
            ob_str(b, tmp);
            break;
        case NLSQL_TYPE_DOUBLE:
            snprintf(tmp, sizeof(tmp), "%.17g", v->d);
            ob_str(b, tmp);
            break;
        default:
            ob_put(b, v->s ? v->s : "", v->len);
            break;
    }
}

static void wan_handler(const char* path, int method, const char* body, size_t body_size,
                        char** response, size_t* response_len, void* ud) {
    (void)path; (void)method; (void)ud;
    obuf_t b = { 0 };
    nlsql_conn_t* c = g_conn;
    if (!c) {
        ob_str(&b, "ERR no connection");
    } else if (!body || body_size == 0) {
        ob_str(&b, "ERR empty SQL body");
    } else {
        char* sql = (char*)malloc(body_size + 1);
        if (!sql) {
            ob_str(&b, "ERR out of memory");
        } else {
            memcpy(sql, body, body_size);
            sql[body_size] = '\0';
            nlsql_result_t* res = NULL;
            int rc = nlsql_query(c, sql, &res);
            free(sql);
            if (rc != NLSQL_OK) {
                char emsg[256];
                const char* e = nlsql_error(c);
                snprintf(emsg, sizeof(emsg), "%s", e ? e : "query failed");
                char tmp[320];
                snprintf(tmp, sizeof(tmp), "ERR %d %s\n", rc, emsg);
                ob_str(&b, tmp);
            } else {
                ob_str(&b, "OK\n");
                if (res && res->col_count > 0) {
                    for (int i = 0; i < res->col_count; i++) {
                        if (i) ob_str(&b, "\t");
                        ob_str(&b, res->cols[i].name);
                    }
                    ob_str(&b, "\n");
                    for (uint64_t r = 0; r < res->row_count; r++) {
                        for (int i = 0; i < res->col_count; i++) {
                            if (i) ob_str(&b, "\t");
                            ob_value(&b, &res->rows[r].vals[i]);
                        }
                        ob_str(&b, "\n");
                    }
                }
                nlsql_free_result(res);
            }
        }
    }
    if (!b.p) { b.p = (char*)malloc(1); if (b.p) b.p[0] = '\0'; }
    /* NetLeaf 的 HTTP 服务端把 handler 返回值原样写回 socket，不补状态行/头，
       因此这里必须自行拼装完整的 HTTP/1.1 响应，浏览器/curl 才能正常解析。 */
    {
        char hdr[192];
        int hn = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/plain; charset=utf-8\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n\r\n",
                          b.len);
        char* out = (char*)malloc((size_t)hn + b.len + 1);
        if (!out) {
            free(b.p);
            *response = NULL;
            *response_len = 0;
            return;
        }
        memcpy(out, hdr, (size_t)hn);
        memcpy(out + hn, b.p, b.len);
        out[hn + (int)b.len] = '\0';
        free(b.p);
        *response = out;           /* 所有权交给 NetLeaf，由宿主 free() */
        *response_len = (size_t)hn + b.len;
    }
}

/* ---------------- 对外接口 ---------------- */

int nlsql_ext_wan_start(nlsql_conn_t* conn, int port) {
    if (!conn || port <= 0 || port > 65535) return NLSQL_ERR;
    if (g_srv) return NLSQL_ERR;
    if (wan_bind_api() != 0) return NLSQL_ERR;
    g_conn = conn;
    nl_server_t* srv = g_api.create(NL_PROTO_HTTP_MIRROR, port);
    if (!srv) return NLSQL_ERR;
    g_api.set_handler(srv, wan_handler, NULL);
    if (g_api.start(srv) != 0) {
        g_api.destroy(srv);
        g_conn = NULL;
        return NLSQL_ERR;
    }
    g_srv = srv;
    return NLSQL_OK;
}

void nlsql_ext_wan_stop(void) {
    if (!g_srv) return;
    g_api.stop(g_srv);
    g_api.destroy(g_srv);
    g_srv = NULL;
    g_conn = NULL;
}

int nlsql_ext_wan_running(void) { return g_srv ? 1 : 0; }

static int g_inited;

static int wan_init(void) {
    if (g_inited) return 0;
    g_inited = 1;
    return 0;
}

static void wan_shutdown(void) {
    if (!g_inited) return;
    nlsql_ext_wan_stop();
    g_inited = 0;
}

#if NLSQL_HAVE_NETLEAF_HEADER
static int ext_available(void) { return 1; }
static const char* ext_version(void) { return NLSQL_VERSION; }

NL_EXTENSION_DEFINE(nlsql_ext_wan, "NLSQL WAN", NLSQL_VERSION, "508364",
                    "经 NetLeaf 网络 API 提供外网 HTTP 查询网关",
                    "Windows,Linux,MacOS",
                    NL_CAP_THREAD_SAFE | NL_CAP_CLIENT | NL_CAP_EXT_SYSTEM,
                    wan_init, wan_shutdown, ext_available, ext_version);

__attribute__((visibility("default")))
nl_plugin_info_t* plugin_get_info(void) {
    (void)NL_EXTENSION_GET_INFO(nlsql_ext_wan);
    static nl_plugin_info_t info;
    memset(&info, 0, sizeof(info));
    info.name = "NLSQL WAN";
    info.id = EXT_WAN_ID;
    info.version = NLSQL_VERSION;
    info.description = "HTTP gateway for NLSQL via NetLeaf network API";
    info.author = "508364";
    info.license = "Apache-2.0";
    info.capabilities = NL_CAP_THREAD_SAFE | NL_CAP_CLIENT;
    info.platform_windows = 1; info.platform_linux = 1; info.platform_macos = 1;
    info.min_nl_version = NL_MODULE_VERSION;
    info.init = wan_init;
    info.shutdown = wan_shutdown;
    info.is_available = ext_available;
    return &info;
}
#endif

__attribute__((visibility("default"))) int plugin_init(void) { return wan_init(); }
__attribute__((visibility("default"))) void plugin_shutdown(void) { wan_shutdown(); }
