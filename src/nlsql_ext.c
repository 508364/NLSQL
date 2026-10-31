/**
 * @file nlsql_ext.c
 * @brief 扩展钩子注册表与派发（语句处理器 / SQL 预改写 / 特性开关 / 事务 / 索引）
 *
 * 设计：扩展（NetLeaf .so）调用注册接口；核心执行 SQL 时先跑预改写，
 * 再依次询问语句处理器；全部返回 NLSQL_EXT_UNHANDLED 才走内置解析器。
 * 未加载扩展时注册表为空，行为与旧版一致。
 */
#include "nlsql_internal.h"
#include "netleaf_nlsql_ext.h"
#include <stdatomic.h>

#define EXT_MAX_HANDLERS  16
#define EXT_MAX_REWRITERS 16
#define EXT_MAX_FEATURES  32
#define EXT_MAX_DEPTH     8   /* 处理器递归深度上限（处理器内可再调用 nlsql_query） */

typedef struct { int used; nlsql_ext_stmt_fn fn; void* ud; } ext_handler_t;
typedef struct { int used; nlsql_ext_rewrite_fn fn; void* ud; } ext_rewriter_t;
typedef struct { char name[48]; int on; } ext_feature_t;

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static ext_handler_t   g_handlers[EXT_MAX_HANDLERS];
static ext_rewriter_t  g_rewriters[EXT_MAX_REWRITERS];
static ext_feature_t   g_features[EXT_MAX_FEATURES];
static int             g_nfeat;

static __thread int g_depth;

/* ---------- 注册 ---------- */

static int add_slot(void* arr, int stride, int cap, void* item) {
    char* base = (char*)arr;
    for (int i = 0; i < cap; i++) {
        int used = *(int*)(base + (size_t)i * stride);
        if (!used) {
            memcpy(base + (size_t)i * stride, item, (size_t)stride);
            *(int*)(base + (size_t)i * stride) = 1;
            return 0;
        }
    }
    return -1;
}

int nlsql_ext_register_stmt_handler(nlsql_ext_stmt_fn fn, void* ud) {
    if (!fn) return NLSQL_ERR;
    ext_handler_t h = { 1, fn, ud };
    pthread_mutex_lock(&g_mtx);
    int rc = add_slot(g_handlers, sizeof(ext_handler_t), EXT_MAX_HANDLERS, &h);
    pthread_mutex_unlock(&g_mtx);
    return rc == 0 ? NLSQL_OK : NLSQL_ERR;
}

int nlsql_ext_unregister_stmt_handler(nlsql_ext_stmt_fn fn) {
    int rc = NLSQL_ERR;
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < EXT_MAX_HANDLERS; i++)
        if (g_handlers[i].used && g_handlers[i].fn == fn) { g_handlers[i].used = 0; rc = NLSQL_OK; }
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

int nlsql_ext_register_rewriter(nlsql_ext_rewrite_fn fn, void* ud) {
    if (!fn) return NLSQL_ERR;
    ext_rewriter_t rw = { 1, fn, ud };
    pthread_mutex_lock(&g_mtx);
    int rc = add_slot(g_rewriters, sizeof(ext_rewriter_t), EXT_MAX_REWRITERS, &rw);
    pthread_mutex_unlock(&g_mtx);
    return rc == 0 ? NLSQL_OK : NLSQL_ERR;
}

int nlsql_ext_unregister_rewriter(nlsql_ext_rewrite_fn fn) {
    int rc = NLSQL_ERR;
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < EXT_MAX_REWRITERS; i++)
        if (g_rewriters[i].used && g_rewriters[i].fn == fn) { g_rewriters[i].used = 0; rc = NLSQL_OK; }
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

int nlsql_ext_set_feature(const char* name, int enabled) {
    if (!name || !name[0] || strlen(name) >= sizeof(g_features[0].name)) return NLSQL_ERR;
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_nfeat; i++)
        if (strcmp(g_features[i].name, name) == 0) {
            g_features[i].on = enabled ? 1 : 0;
            pthread_mutex_unlock(&g_mtx);
            return NLSQL_OK;
        }
    if (g_nfeat >= EXT_MAX_FEATURES) { pthread_mutex_unlock(&g_mtx); return NLSQL_ERR; }
    snprintf(g_features[g_nfeat].name, sizeof(g_features[g_nfeat].name), "%s", name);
    g_features[g_nfeat].on = enabled ? 1 : 0;
    g_nfeat++;
    pthread_mutex_unlock(&g_mtx);
    return NLSQL_OK;
}

int nlsql_ext_feature(const char* name) {
    if (!name) return 0;
    int on = 0;
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_nfeat; i++)
        if (strcmp(g_features[i].name, name) == 0) { on = g_features[i].on; break; }
    pthread_mutex_unlock(&g_mtx);
    return on;
}

int nlsql_ext_feature_enabled(const char* name) { return nlsql_ext_feature(name); }

int nlsql_ext_dispatch_depth(void) { return g_depth; }

void nlsql_ext_set_error(nlsql_conn_t* conn, int code, const char* sqlstate, const char* msg) {
    if (!conn) return;
    nlsql_set_err(conn, code, sqlstate ? sqlstate : "HY000", "%s", msg ? msg : "");
}

/* ---------- 预改写 ---------- */

char* nlsql_ext_rewrite(const char* sql) {
    if (!sql) return NULL;
    ext_rewriter_t snap[EXT_MAX_REWRITERS];
    int n = 0;
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < EXT_MAX_REWRITERS; i++)
        if (g_rewriters[i].used) snap[n++] = g_rewriters[i];
    pthread_mutex_unlock(&g_mtx);
    if (n == 0) return NULL;
    char* cur = NULL;
    const char* in = sql;
    for (int i = 0; i < n; i++) {
        char* next = snap[i].fn(in, snap[i].ud);
        if (next) {
            free(cur);
            cur = next;
            in = cur;
        }
    }
    return cur;
}

/* ---------- 处理器派发 ---------- */

int nlsql_ext_dispatch(nlsql_conn_t* c, const char* sql, nlsql_result_t** out) {
    if (!sql) return NLSQL_EXT_UNHANDLED;
    if (g_depth >= EXT_MAX_DEPTH) return NLSQL_EXT_UNHANDLED;
    ext_handler_t snap[EXT_MAX_HANDLERS];
    int n = 0;
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < EXT_MAX_HANDLERS; i++)
        if (g_handlers[i].used) snap[n++] = g_handlers[i];
    pthread_mutex_unlock(&g_mtx);
    for (int i = 0; i < n; i++) {
        g_depth++;
        int rc = snap[i].fn(c, sql, out, snap[i].ud);
        g_depth--;
        if (rc != NLSQL_EXT_UNHANDLED) return rc;
    }
    return NLSQL_EXT_UNHANDLED;
}

/* ---------- 事务 ---------- */

int nlsql_ext_txn_begin(nlsql_conn_t* conn) {
    if (!conn) return NLSQL_ERR;
    frw_acquire(&conn->frw, FRW_EXCL);
    int rc = nlsql_txn_begin(&conn->eng);
    frw_release(&conn->frw, FRW_EXCL);
    return rc;
}

int nlsql_ext_txn_commit(nlsql_conn_t* conn) {
    if (!conn) return NLSQL_ERR;
    frw_acquire(&conn->frw, FRW_EXCL);
    int rc = nlsql_txn_commit(&conn->eng);
    frw_release(&conn->frw, FRW_EXCL);
    return rc;
}

int nlsql_ext_txn_rollback(nlsql_conn_t* conn) {
    if (!conn) return NLSQL_ERR;
    frw_acquire(&conn->frw, FRW_EXCL);
    int rc = nlsql_txn_rollback(&conn->eng);
    frw_release(&conn->frw, FRW_EXCL);
    return rc;
}

int nlsql_ext_txn_active(nlsql_conn_t* conn) {
    return (conn && conn->eng.txn_active) ? 1 : 0;
}

/* ---------- 索引 DDL ---------- */

static int build_index_sql(char* buf, size_t sz, const char* table, const char* name,
                           const char* const* cols, int ncols, int unique) {
    if (!table || !name || !cols || ncols <= 0 || ncols > 8) return -1;
    size_t off = (size_t)snprintf(buf, sz, "CREATE %sINDEX %s ON %s (",
                                  unique ? "UNIQUE " : "", name, table);
    if (off >= sz) return -1;
    for (int i = 0; i < ncols; i++) {
        int w = snprintf(buf + off, sz - off, "%s%s", i ? "," : "", cols[i]);
        if (w < 0 || (size_t)w >= sz - off) return -1;
        off += (size_t)w;
    }
    if (off + 2 >= sz) return -1;
    buf[off++] = ')';
    buf[off] = '\0';
    return 0;
}

int nlsql_ext_create_index(nlsql_conn_t* conn, const char* table, const char* index_name,
                           const char* const* cols, int ncols, int unique) {
    if (!conn) return NLSQL_ERR;
    char sql[1024];
    if (build_index_sql(sql, sizeof(sql), table, index_name, cols, ncols, unique) != 0)
        return NLSQL_ERR;
    nlsql_result_t* r = NULL;
    int rc = nlsql_query(conn, sql, &r);
    nlsql_free_result(r);
    return rc;
}

int nlsql_ext_drop_index(nlsql_conn_t* conn, const char* table, const char* index_name) {
    if (!conn || !table || !index_name) return NLSQL_ERR;
    char sql[256];
    snprintf(sql, sizeof(sql), "DROP INDEX %s ON %s", index_name, table);
    nlsql_result_t* r = NULL;
    int rc = nlsql_query(conn, sql, &r);
    nlsql_free_result(r);
    return rc;
}

/* =========================================
 * 会话信息与连接注册表
 * ========================================= */

static pthread_mutex_t g_reg_mtx = PTHREAD_MUTEX_INITIALIZER;
static nlsql_conn_t*   g_reg[NLSQL_EXT_PROC_MAX];
static atomic_uint_fast64_t g_next_id = 1;

void nlsql_reg_add(nlsql_conn_t* c) {
    if (!c) return;
    c->id = (uint64_t)atomic_fetch_add(&g_next_id, 1);
    pthread_mutex_lock(&g_reg_mtx);
    for (int i = 0; i < NLSQL_EXT_PROC_MAX; i++)
        if (!g_reg[i]) { g_reg[i] = c; break; }
    pthread_mutex_unlock(&g_reg_mtx);
}

void nlsql_reg_del(nlsql_conn_t* c) {
    pthread_mutex_lock(&g_reg_mtx);
    for (int i = 0; i < NLSQL_EXT_PROC_MAX; i++)
        if (g_reg[i] == c) g_reg[i] = NULL;
    pthread_mutex_unlock(&g_reg_mtx);
}

const char* nlsql_ext_conn_db(nlsql_conn_t* conn) {
    if (!conn || !conn->eng.cur_db) return "";
    return conn->eng.cur_db->name;
}

const char* nlsql_ext_conn_data_dir(nlsql_conn_t* conn) {
    return conn ? conn->eng.root : "";
}

const char* nlsql_ext_conn_user(nlsql_conn_t* conn) {
    if (!conn) return "";
    if (conn->have_creds && conn->creds_user[0]) return conn->creds_user;
    return "root";
}

uint64_t nlsql_ext_conn_id(nlsql_conn_t* conn) { return conn ? conn->id : 0; }

int nlsql_ext_proc_list(nlsql_ext_proc_t* out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    pthread_mutex_lock(&g_reg_mtx);
    for (int i = 0; i < NLSQL_EXT_PROC_MAX && n < cap; i++) {
        nlsql_conn_t* c = g_reg[i];
        if (!c) continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id = c->id;
        snprintf(out[n].user, sizeof(out[n].user), "%s", nlsql_ext_conn_user(c));
        snprintf(out[n].db, sizeof(out[n].db), "%s", nlsql_ext_conn_db(c));
        out[n].readonly = c->readonly;
        n++;
    }
    pthread_mutex_unlock(&g_reg_mtx);
    return n;
}
