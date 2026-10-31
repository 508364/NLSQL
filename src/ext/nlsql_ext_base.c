/**
 * @file nlsql_ext_base.c
 * @brief NLSQL 基础扩展：时间函数预改写 + 服务器预处理特性
 *
 * NetLeaf 以插件形式加载本 .so；init 时把预改写器注册进 NLSQL 核心，
 * 并把 SQL 中的 NOW()/CURDATE()/CURTIME()/UNIX_TIMESTAMP()/FROM_UNIXTIME()
 * 替换为字面量，从而无需改动核心解析器即可使用时间函数。
 *
 * 同时对宿主 NetLeaf 暴露扩展元数据；宿主符号通过 dlsym 动态获取，
 * 未加载 NetLeaf 时本扩展仍可独立用于嵌入式 API（不硬依赖）。
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

#include <ctype.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EXT_BASE_ID "nlsql_ext_base"

/* ---------------- 动态缓冲 ---------------- */

typedef struct {
    char*  p;
    size_t len, cap;
    int    oom;
} dbuf_t;

static int dbuf_reserve(dbuf_t* b, size_t add) {
    if (b->oom) return -1;
    if (b->len + add + 1 <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + add + 1) nc *= 2;
    char* np = (char*)realloc(b->p, nc);
    if (!np) { b->oom = 1; return -1; }
    b->p = np;
    b->cap = nc;
    return 0;
}

static void dbuf_putn(dbuf_t* b, const char* s, size_t n) {
    if (dbuf_reserve(b, n) != 0) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void dbuf_puts(dbuf_t* b, const char* s) { dbuf_putn(b, s, strlen(s)); }
static void dbuf_putc(dbuf_t* b, char c) { dbuf_putn(b, &c, 1); }

/* ---------------- 时间字面量 ---------------- */

static void fmt_now(char* out, size_t sz) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, sz, "%Y-%m-%d %H:%M:%S", &tmv);
}

static void fmt_date(char* out, size_t sz) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, sz, "%Y-%m-%d", &tmv);
}

static void fmt_time(char* out, size_t sz) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, sz, "%H:%M:%S", &tmv);
}

/* 从前缀（已小写）判断函数种类；返回消耗的标识符长度，0=不匹配 */
static int time_func_kind(const char* s, int* kind) {
    struct { const char* name; int k; } names[] = {
        { "current_timestamp", 1 }, { "now", 1 },
        { "curdate", 2 }, { "current_date", 2 },
        { "curtime", 3 }, { "current_time", 3 },
        { "unix_timestamp", 4 },
        { "from_unixtime", 5 },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t n = strlen(names[i].name);
        if (strncasecmp(s, names[i].name, n) == 0 &&
            !(isalnum((unsigned char)s[n]) || s[n] == '_')) {
            *kind = names[i].k;
            return (int)n;
        }
    }
    return 0;
}

static int skip_ws(const char* s, int i) {
    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n') i++;
    return i;
}

/* 把 FROM_UNIXTIME(整数) 转为本地时间字面量；成功返回 1 */
static int from_unixtime_literal(const char* arg, char* out, size_t sz) {
    char* end = NULL;
    long long v = strtoll(arg, &end, 10);
    if (end == arg) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end != '\0') return 0;
    time_t t = (time_t)v;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, sz, "%Y-%m-%d %H:%M:%S", &tmv);
    return 1;
}

/* ---------------- 预改写 ---------------- */

static char* base_rewrite(const char* sql, void* ud) {
    (void)ud;
    if (!sql) return NULL;
    dbuf_t b = { 0 };
    int changed = 0;
    int i = 0;
    while (sql[i]) {
        char c = sql[i];
        /* 字符串 / 反引号标识符原样跳过 */
        if (c == '\'' || c == '"' || c == '`') {
            char q = c;
            dbuf_putc(&b, c);
            i++;
            while (sql[i]) {
                dbuf_putc(&b, sql[i]);
                if (sql[i] == '\\' && sql[i + 1]) { i++; dbuf_putc(&b, sql[i]); i++; continue; }
                if (sql[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        /* 注释原样跳过 */
        if (c == '-' && sql[i + 1] == '-') {
            while (sql[i] && sql[i] != '\n') dbuf_putc(&b, sql[i++]);
            continue;
        }
        if (c == '#') {
            while (sql[i] && sql[i] != '\n') dbuf_putc(&b, sql[i++]);
            continue;
        }
        if (c == '/' && sql[i + 1] == '*') {
            dbuf_puts(&b, "/*");
            i += 2;
            while (sql[i] && !(sql[i] == '*' && sql[i + 1] == '/')) dbuf_putc(&b, sql[i++]);
            if (sql[i]) { dbuf_puts(&b, "*/"); i += 2; }
            continue;
        }
        /* 标识符：尝试识别时间函数 */
        if (isalpha((unsigned char)c) || c == '_') {
            int kind = 0;
            int n = time_func_kind(sql + i, &kind);
            if (!n) {
                while (isalnum((unsigned char)sql[i]) || sql[i] == '_') dbuf_putc(&b, sql[i++]);
                continue;
            }
            int j = skip_ws(sql, i + n);
            char lit[64];
            if (kind == 1 || kind == 2 || kind == 3) {
                /* NOW()/CURDATE()/CURTIME()，也允许无括号形式（如 CURRENT_TIMESTAMP） */
                int has_paren = (sql[j] == '(');
                if (has_paren) {
                    int k = skip_ws(sql, j + 1);
                    if (sql[k] != ')') { /* 带参数形态不认识，原样输出 */ goto passthrough; }
                }
                if (kind == 1) fmt_now(lit, sizeof(lit));
                else if (kind == 2) fmt_date(lit, sizeof(lit));
                else fmt_time(lit, sizeof(lit));
                dbuf_putc(&b, '\'');
                dbuf_puts(&b, lit);
                dbuf_putc(&b, '\'');
                changed = 1;
                i = has_paren ? skip_ws(sql, j + 1) + 1 : i + n;
                continue;
            }
            if (kind == 4) {
                /* UNIX_TIMESTAMP()：无参返回当前秒数；带参留给核心表达式处理 */
                if (sql[j] == '(') {
                    int k = skip_ws(sql, j + 1);
                    if (sql[k] == ')') {
                        snprintf(lit, sizeof(lit), "%lld", (long long)time(NULL));
                        dbuf_puts(&b, lit);
                        changed = 1;
                        i = k + 1;
                        continue;
                    }
                }
            }
            if (kind == 5 && sql[j] == '(') {
                int k = skip_ws(sql, j + 1);
                int argstart = k;
                while (sql[k] && sql[k] != ')') k++;
                if (sql[k] == ')') {
                    char arg[64];
                    int alen = k - argstart;
                    if (alen > 0 && (size_t)alen < sizeof(arg)) {
                        memcpy(arg, sql + argstart, (size_t)alen);
                        arg[alen] = '\0';
                        char dt[64];
                        if (from_unixtime_literal(arg, dt, sizeof(dt))) {
                            dbuf_putc(&b, '\'');
                            dbuf_puts(&b, dt);
                            dbuf_putc(&b, '\'');
                            changed = 1;
                            i = k + 1;
                            continue;
                        }
                    }
                }
            }
passthrough:
            /* 不认识/参数形态不符：原样输出整个标识符 */
            dbuf_putn(&b, sql + i, (size_t)n);
            i += n;
            continue;
        }
        dbuf_putc(&b, c);
        i++;
    }
    if (b.oom) { free(b.p); return NULL; }
    if (!changed) { free(b.p); return NULL; }
    return b.p;
}

/* ---------------- 生命周期 ---------------- */

static int g_inited;

static int base_init(void) {
    if (g_inited) return 0;
    if (nlsql_ext_register_rewriter(base_rewrite, NULL) != NLSQL_OK) return -1;
    nlsql_ext_set_feature("server_prepare", 1);
    g_inited = 1;
    return 0;
}

static void base_shutdown(void) {
    if (!g_inited) return;
    nlsql_ext_unregister_rewriter(base_rewrite);
    nlsql_ext_set_feature("server_prepare", 0);
    g_inited = 0;
}

/* 供测试/嵌入式调用者直接启停 */
int nlsql_ext_base_init(void) { return base_init(); }
void nlsql_ext_base_shutdown(void) { base_shutdown(); }

/* ---------------- NetLeaf 插件入口 ---------------- */

#if NLSQL_HAVE_NETLEAF_HEADER
static int ext_available(void) { return 1; }
static const char* ext_version(void) { return NLSQL_VERSION; }

NL_EXTENSION_DEFINE(nlsql_ext_base, "NLSQL Base", NLSQL_VERSION, "508364",
                    "时间函数预改写与服务端预处理特性",
                    "Windows,Linux,MacOS",
                    NL_CAP_THREAD_SAFE | NL_CAP_EXT_SYSTEM,
                    base_init, base_shutdown, ext_available, ext_version);

__attribute__((visibility("default")))
nl_plugin_info_t* plugin_get_info(void) {
    (void)NL_EXTENSION_GET_INFO(nlsql_ext_base);
    static nl_plugin_info_t info;
    memset(&info, 0, sizeof(info));
    info.name = "NLSQL Base";
    info.id = EXT_BASE_ID;
    info.version = NLSQL_VERSION;
    info.description = "Time functions + server-side prepare for NLSQL";
    info.author = "508364";
    info.license = "Apache-2.0";
    info.capabilities = NL_CAP_THREAD_SAFE;
    info.platform_windows = 1; info.platform_linux = 1; info.platform_macos = 1;
    info.min_nl_version = NL_MODULE_VERSION;
    info.init = base_init;
    info.shutdown = base_shutdown;
    info.is_available = ext_available;
    return &info;
}
#endif

__attribute__((visibility("default"))) int plugin_init(void) { return base_init(); }
__attribute__((visibility("default"))) void plugin_shutdown(void) { base_shutdown(); }
