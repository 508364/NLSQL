/**
 * @file nlsql_ext_store.c
 * @brief NLSQL 存储扩展：事务控制 + 二级/复合/唯一索引
 *
 * 事务语句（BEGIN / START TRANSACTION / COMMIT / ROLLBACK）由语句处理器接管，
 * 调用核心的事务快照接口；索引能力通过特性开关 "index" 打开，
 * 之后核心解析器即可执行 CREATE [UNIQUE] INDEX / DROP INDEX，
 * 并在等值查询时使用二级索引快速路径。
 *
 * 索引定义另写入每库 sidecar（nlsql_ext_meta.dat），切库后自动重建，
 * 因此重开数据目录不再丢失二级索引。
 */
#define _GNU_SOURCE
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define EXT_STORE_ID "nlsql_ext_store"

/* ---------------- 索引元数据持久化 ----------------
 * 核心的二级索引只存在于内存（表结构的一部分），重开数据目录即丢失。
 * 本扩展把索引定义写入每库 sidecar（kind="index"，target=表名，
 * def="<unique 0|1>|<col1,col2>"），并在切入某库后按记录重建。
 */

static _Thread_local int t_store_busy;

/* 跳过空白 */
static const char* skip_ws_p(const char* p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* 读取标识符（可反引号）；返回结束后位置，name 输出 */
static const char* read_ident(const char* p, char* out, size_t osz) {
    p = skip_ws_p(p);
    size_t n = 0;
    if (*p == '`') {
        p++;
        while (*p && *p != '`' && n + 1 < osz) out[n++] = *p++;
        if (*p == '`') p++;
    } else {
        while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '$') && n + 1 < osz)
            out[n++] = *p++;
    }
    out[n] = '\0';
    return p;
}

static int starts_ci(const char* p, const char* w) {
    return strncasecmp(p, w, strlen(w)) == 0 && !isalnum((unsigned char)p[strlen(w)]);
}

/* 解析 CREATE [UNIQUE] INDEX name ON table (cols)；成功返回 0 */
static int parse_create_index(const char* sql, char* name, size_t nsz,
                              char* table, size_t tsz, char* cols, size_t csz, int* unique) {
    const char* p = skip_ws_p(sql);
    if (!starts_ci(p, "CREATE")) return -1;
    p = skip_ws_p(p + 6);
    *unique = 0;
    if (starts_ci(p, "UNIQUE")) { *unique = 1; p = skip_ws_p(p + 6); }
    if (!starts_ci(p, "INDEX")) return -1;
    p = read_ident(p + 5, name, nsz);
    if (!name[0]) return -1;
    p = skip_ws_p(p);
    if (!starts_ci(p, "ON")) return -1;
    p = read_ident(p + 2, table, tsz);
    if (!table[0]) return -1;
    p = skip_ws_p(p);
    if (*p != '(') return -1;
    p++;
    size_t o = 0;
    cols[0] = '\0';
    for (;;) {
        char c[68];
        p = read_ident(p, c, sizeof(c));
        if (!c[0]) return -1;
        int w = snprintf(cols + o, csz - o, "%s%s", o ? "," : "", c);
        if (w < 0 || (size_t)w >= csz - o) return -1;
        o += (size_t)w;
        p = skip_ws_p(p);
        if (*p == ',') { p++; continue; }
        break;
    }
    return (*p == ')') ? 0 : -1;
}

/* 解析 DROP INDEX name ON table */
static int parse_drop_index(const char* sql, char* name, size_t nsz, char* table, size_t tsz) {
    const char* p = skip_ws_p(sql);
    if (!starts_ci(p, "DROP")) return -1;
    p = skip_ws_p(p + 4);
    if (!starts_ci(p, "INDEX")) return -1;
    p = read_ident(p + 5, name, nsz);
    if (!name[0]) return -1;
    p = skip_ws_p(p);
    if (!starts_ci(p, "ON")) return -1;
    p = read_ident(p + 2, table, tsz);
    return table[0] ? 0 : -1;
}

/* 已重建索引的 (连接, 库)：连接 ID 单调递增，重开连接后必然重新重建 */
static uint64_t g_loaded_conn_id;
static char g_loaded_db[65];

static int restore_cb(const char* kind, const char* name, const char* target,
                      const char* def, void* ud) {
    (void)name;
    nlsql_conn_t* conn = (nlsql_conn_t*)ud;
    if (strcmp(kind, "index") != 0 || !target[0] || !def[0]) return 0;
    char* d = strdup(def);
    if (!d) return 0;
    int unique = (d[0] == '1');
    char* cols_str = strchr(d, '|');
    if (!cols_str) { free(d); return 0; }
    *cols_str++ = '\0';
    const char* colv[8];
    int ncols = 0;
    char* save = NULL;
    for (char* tok = strtok_r(cols_str, ",", &save); tok && ncols < 8; tok = strtok_r(NULL, ",", &save))
        colv[ncols++] = tok;
    if (ncols > 0) nlsql_ext_create_index(conn, target, name, colv, ncols, unique);
    free(d);
    return 0;
}

/* 切入新库后按 sidecar 重建索引；同库重复调用为空操作 */
static void store_sync_indexes(nlsql_conn_t* conn) {
    if (t_store_busy) return;
    const char* db = nlsql_ext_conn_db(conn);
    if (!db[0]) return;
    uint64_t cid = nlsql_ext_conn_id(conn);
    if (cid == g_loaded_conn_id && strcmp(db, g_loaded_db) == 0) return;
    g_loaded_conn_id = cid;
    snprintf(g_loaded_db, sizeof(g_loaded_db), "%s", db);
    /* 重建期间禁止本处理器再拦截，避免递归 */
    t_store_busy = 1;
    nlsql_ext_meta_each(conn, restore_cb, conn);
    t_store_busy = 0;
}

/* 去空白后比较（不区分大小写） */
static int kw_is(const char* sql, const char* kw, size_t klen) {
    while (*sql == ' ' || *sql == '\t' || *sql == '\r' || *sql == '\n') sql++;
    size_t n = 0;
    while (sql[n] && !isspace((unsigned char)sql[n]) && sql[n] != ';') n++;
    if (n != klen) return 0;
    return strncasecmp(sql, kw, klen) == 0;
}

static int store_handler(nlsql_conn_t* conn, const char* sql, nlsql_result_t** out, void* ud) {
    (void)ud;
    if (out) *out = NULL;
    if (!conn || !sql) return NLSQL_EXT_UNHANDLED;

    /* 重建索引期间不拦截任何语句（直接交还核心），避免递归 */
    if (t_store_busy) return NLSQL_EXT_UNHANDLED;

    store_sync_indexes(conn);

    /* CREATE/DROP INDEX：交核心执行，成功后把定义写入每库 sidecar */
    {
        char iname[72], itbl[72], icols[512];
        const char* p = sql;
        while (*p && isspace((unsigned char)*p)) p++;
        int uniq = 0;
        if (starts_ci(p, "CREATE") &&
            parse_create_index(sql, iname, sizeof(iname), itbl, sizeof(itbl),
                               icols, sizeof(icols), &uniq) == 0) {
            t_store_busy = 1;
            nlsql_result_t* r = NULL;
            int rc = nlsql_query(conn, sql, &r);
            t_store_busy = 0;
            if (rc == NLSQL_OK) {
                char def[600];
                snprintf(def, sizeof(def), "%d|%s", uniq, icols);
                nlsql_ext_meta_put(conn, "index", iname, itbl, def);
            }
            if (out) *out = r; else nlsql_free_result(r);
            return rc;
        }
        if (starts_ci(p, "DROP") &&
            parse_drop_index(sql, iname, sizeof(iname), itbl, sizeof(itbl)) == 0) {
            t_store_busy = 1;
            nlsql_result_t* r = NULL;
            int rc = nlsql_query(conn, sql, &r);
            t_store_busy = 0;
            if (rc == NLSQL_OK) nlsql_ext_meta_del(conn, "index", iname);
            if (out) *out = r; else nlsql_free_result(r);
            return rc;
        }
    }

    if (kw_is(sql, "BEGIN", 5) || kw_is(sql, "START", 5)) {
        /* START TRANSACTION 与 BEGIN 等价 */
        if (kw_is(sql, "START", 5)) {
            const char* p = sql;
            while (*p && isspace((unsigned char)*p)) p++;
            p += 5;
            while (*p && isspace((unsigned char)*p)) p++;
            if (strncasecmp(p, "TRANSACTION", 11) != 0) return NLSQL_EXT_UNHANDLED;
        }
        return nlsql_ext_txn_begin(conn);
    }
    if (kw_is(sql, "COMMIT", 6)) return nlsql_ext_txn_commit(conn);
    if (kw_is(sql, "ROLLBACK", 8)) return nlsql_ext_txn_rollback(conn);
    return NLSQL_EXT_UNHANDLED;
}

static int g_inited;

static int store_init(void) {
    if (g_inited) return 0;
    if (nlsql_ext_register_stmt_handler(store_handler, NULL) != NLSQL_OK) return -1;
    nlsql_ext_set_feature("index", 1);
    g_inited = 1;
    return 0;
}

static void store_shutdown(void) {
    if (!g_inited) return;
    nlsql_ext_unregister_stmt_handler(store_handler);
    nlsql_ext_set_feature("index", 0);
    g_inited = 0;
}

int nlsql_ext_store_init(void) { return store_init(); }
void nlsql_ext_store_shutdown(void) { store_shutdown(); }

#if NLSQL_HAVE_NETLEAF_HEADER
static int ext_available(void) { return 1; }
static const char* ext_version(void) { return NLSQL_VERSION; }

NL_EXTENSION_DEFINE(nlsql_ext_store, "NLSQL Store", NLSQL_VERSION, "508364",
                    "事务控制与二级/复合/唯一索引",
                    "Windows,Linux,MacOS",
                    NL_CAP_THREAD_SAFE | NL_CAP_EXT_SYSTEM,
                    store_init, store_shutdown, ext_available, ext_version);

__attribute__((visibility("default")))
nl_plugin_info_t* plugin_get_info(void) {
    (void)NL_EXTENSION_GET_INFO(nlsql_ext_store);
    static nl_plugin_info_t info;
    memset(&info, 0, sizeof(info));
    info.name = "NLSQL Store";
    info.id = EXT_STORE_ID;
    info.version = NLSQL_VERSION;
    info.description = "Transactions + secondary/composite/unique indexes for NLSQL";
    info.author = "508364";
    info.license = "Apache-2.0";
    info.capabilities = NL_CAP_THREAD_SAFE;
    info.platform_windows = 1; info.platform_linux = 1; info.platform_macos = 1;
    info.min_nl_version = NL_MODULE_VERSION;
    info.init = store_init;
    info.shutdown = store_shutdown;
    info.is_available = ext_available;
    return &info;
}
#endif

__attribute__((visibility("default"))) int plugin_init(void) { return store_init(); }
__attribute__((visibility("default"))) void plugin_shutdown(void) { store_shutdown(); }
