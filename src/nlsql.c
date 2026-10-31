/**
 * @file nlsql.c
 * @brief NLSQL 公共 API：连接、同步/异步查询、预编译语句、凭据门控
 */
#include "nlsql_internal.h"
#include <errno.h>
#include <time.h>
#if defined(__linux__)
#include <sys/resource.h> /* setpriority：后台压实线程降 nice */
#endif
#if defined(__GLIBC__)
#include <malloc.h> /* malloc_trim */
#endif

/* 前置声明（连接生命周期章节即会用到） */
static void  stop_wires_for_conn(nlsql_conn_t* conn);
static int   flush_core(nlsql_conn_t* c, int force);
static int   auto_land(nlsql_conn_t* c);
static void* flush_thread_main(void* arg);

/* =========================================
 * 连接生命周期
 * ========================================= */

nlsql_conn_t* nlsql_open_ex(const nlsql_open_opts_t* opts) {
    if (!opts || !opts->data_dir || !opts->data_dir[0]) {
        nlsql_set_global_err("data_dir required");
        return NULL;
    }
    nlsql_conn_t* c = (nlsql_conn_t*)calloc(1, sizeof(nlsql_conn_t));
    if (!c) { nlsql_set_global_err("out of memory"); return NULL; }
    pthread_mutex_init(&c->err_mtx, NULL);
    frw_init(&c->frw);
    pthread_mutex_init(&c->flush_mtx, NULL);
#ifndef _WIN32
    /* 条件变量用单调时钟：自动落盘的定时等待不受系统时间调整影响。
       winpthreads 只有相对超时接口（pthread_cond_timedwait_relative_np），
       且 pthread_condattr_setclock 会被忽略，故 Windows 分支走相对等待。 */
    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&c->flush_cv, &cattr);
    pthread_condattr_destroy(&cattr);
#else
    pthread_cond_init(&c->flush_cv, NULL);
#endif
    c->readonly = opts->readonly ? 1 : 0;
    int af = opts->auto_flush_ms;
    if (af == 0) af = 5000;              /* 0 = 默认 5 秒 */
    c->auto_flush_ms = (af > 0 && !c->readonly) ? af : 0;
    int rc = eng_open(&c->eng, opts->data_dir, (uint32_t)opts->journal_mb, c->readonly);
    if (rc != NLSQL_OK) {
        nlsql_set_global_err("cannot open data dir: %s (%d)", opts->data_dir, rc);
        frw_destroy(&c->frw);
        pthread_mutex_destroy(&c->err_mtx);
        pthread_mutex_destroy(&c->flush_mtx);
        pthread_cond_destroy(&c->flush_cv);
        free(c);
        return NULL;
    }
    /* 只读连接不起压实线程；线程创建失败则回退为同步压实（功能不受影响） */
    if (!c->readonly && pthread_create(&c->flush_th, NULL, flush_thread_main, c) == 0)
        c->flush_th_started = 1;
    c->aq = (async_queue_t*)calloc(1, sizeof(async_queue_t));
    if (!c->aq || aq_start(c->aq, c, opts->workers) != 0) {
        nlsql_set_global_err("cannot start async workers");
        if (c->flush_th_started) {
            pthread_mutex_lock(&c->flush_mtx);
            c->flush_quit = 1;
            pthread_cond_signal(&c->flush_cv);
            pthread_mutex_unlock(&c->flush_mtx);
            pthread_join(c->flush_th, NULL);
        }
        eng_close(&c->eng);
        free(c->aq);
        frw_destroy(&c->frw);
        pthread_mutex_destroy(&c->err_mtx);
        pthread_mutex_destroy(&c->flush_mtx);
        pthread_cond_destroy(&c->flush_cv);
        free(c);
        return NULL;
    }
    if (opts->user && opts->user[0]) {
        nlsql_set_credentials(c, opts->user, opts->password);
        c->authenticated = 0; /* 需显式 authenticate() 或 wire 层校验 */
    }
    nlsql_reg_add(c); /* 进程列表可见 */
    return c;
}

nlsql_conn_t* nlsql_open(const char* data_dir) {
    nlsql_open_opts_t o;
    memset(&o, 0, sizeof(o));
    o.data_dir = data_dir;
    return nlsql_open_ex(&o);
}

void nlsql_close(nlsql_conn_t* conn) {
    if (!conn) return;
    nlsql_reg_del(conn);       /* 首步摘除：进程列表不再引用本连接 */
    stop_wires_for_conn(conn); /* 再等所有 wire 客户端线程退场（它们要读 conn/lock） */
    if (conn->flush_th_started) {
        pthread_mutex_lock(&conn->flush_mtx);
        conn->flush_quit = 1;
        pthread_cond_signal(&conn->flush_cv);
        pthread_mutex_unlock(&conn->flush_mtx);
        pthread_join(conn->flush_th, NULL);
    }
    aq_wait_idle(conn->aq);
    aq_drain(conn->aq);
    frw_acquire(&conn->frw, FRW_EXCL);
    eng_close(&conn->eng);
    frw_release(&conn->frw, FRW_EXCL);
    free(conn->aq);
    frw_destroy(&conn->frw);
    pthread_mutex_destroy(&conn->err_mtx);
    pthread_mutex_destroy(&conn->flush_mtx);
    pthread_cond_destroy(&conn->flush_cv);
    free(conn);
#if defined(__GLIBC__)
    malloc_trim(0); /* close 会先等 wire 线程全部退场，此处归还峰值内存安全 */
#endif
}

/* =========================================
 * 凭据门控
 * ========================================= */

int nlsql_set_credentials(nlsql_conn_t* conn, const char* user, const char* password) {
    if (!conn || !user || !user[0] || strlen(user) > NLSQL_MAX_IDENT) return NLSQL_ERR;
    frw_acquire(&conn->frw, FRW_EXCL);
    snprintf(conn->creds_user, sizeof(conn->creds_user), "%s", user);
    /* SHA1(SHA1(password))：与 mysql_native_password 存储格式一致，不落明文 */
    uint8_t pass_buf[256];
    const char* pw = password ? password : "";
    size_t plen = strlen(pw);
    if (plen > sizeof(pass_buf)) plen = sizeof(pass_buf);
    memcpy(pass_buf, pw, plen);
    uint8_t h1[20], h2[20];
    nlsha1(pass_buf, (uint32_t)plen, h1);
    nlsha1(h1, 20, h2);
    memcpy(conn->creds_sha, h2, 20);
    conn->have_creds = 1;
    conn->authenticated = 0;
    /* 口令缓冲区清零，降低泄露窗口 */
    memset(pass_buf, 0, sizeof(pass_buf));
    frw_release(&conn->frw, FRW_EXCL);
    return NLSQL_OK;
}

int nlsql_authenticate(nlsql_conn_t* conn, const char* user, const char* password) {
    if (!conn) return NLSQL_ERR;
    frw_acquire(&conn->frw, FRW_EXCL);
    if (!conn->have_creds) {
        conn->authenticated = 1; /* 未设置凭据 = 本地直连放行 */
        frw_release(&conn->frw, FRW_EXCL);
        return NLSQL_OK;
    }
    uint8_t pass_buf[256];
    const char* pw = password ? password : "";
    size_t plen = strlen(pw);
    if (plen > sizeof(pass_buf)) plen = sizeof(pass_buf);
    memcpy(pass_buf, pw, plen);
    uint8_t h1[20], h2[20];
    nlsha1(pass_buf, (uint32_t)plen, h1);
    nlsha1(h1, 20, h2);
    memset(pass_buf, 0, sizeof(pass_buf));
    int ok = (user && strcmp(user, conn->creds_user) == 0 && memcmp(h2, conn->creds_sha, 20) == 0);
    conn->authenticated = ok;
    if (!ok)
        nlsql_set_err(conn, NLSQL_ER_ACCESS_DENIED, "28000",
                      "Access denied for user '%s'", user ? user : "(null)");
    frw_release(&conn->frw, FRW_EXCL);
    return ok ? NLSQL_OK : NLSQL_ER_ACCESS_DENIED;
}

int nlsql_flush(nlsql_conn_t* conn) {
    if (!conn) return NLSQL_ERR;
    if (!conn->flush_th_started) return flush_core(conn, 1);
    /* 投递强制压实并等它完成（后台线程合并已有请求，不会重复做无用功） */
    pthread_mutex_lock(&conn->flush_mtx);
    conn->flush_force = 1;
    conn->flush_req = 1;
    conn->flush_rc = NLSQL_OK;
    pthread_cond_signal(&conn->flush_cv);
    while (conn->flush_req || conn->flush_running)
        pthread_cond_wait(&conn->flush_cv, &conn->flush_mtx);
    int rc = conn->flush_rc;
    pthread_mutex_unlock(&conn->flush_mtx);
#if defined(__GLIBC__)
    malloc_trim(0); /* 压实/落盘后归还临时峰值内存 */
#endif
    return rc;
}

/* =========================================
 * 同步查询
 * ========================================= */

static int auth_gate(nlsql_conn_t* c) {
    if (c->have_creds && !c->authenticated) {
        nlsql_set_err(c, NLSQL_ER_ACCESS_DENIED, "28000",
                      "Not authenticated for this session");
        return NLSQL_ER_ACCESS_DENIED;
    }
    return NLSQL_OK;
}

/* journal 超阈值时取 SNAP 压实（挡写放读）。调用方不得持锁。
   非 force 且未超阈值时只做 fsync（便宜），不触发整表重编码。 */
static int flush_core(nlsql_conn_t* c, int force) {
    int did_flush = 0;
    int rc = NLSQL_OK;
    frw_acquire(&c->frw, FRW_SNAP);
    if (!c->eng.readonly) {
        uint64_t sz = nlsql_journal_size(&c->eng);
        int do_flush = force || sz > nlsql_journal_limit(&c->eng);
        if (do_flush) {
            rc = eng_flush(&c->eng);
            if (rc == NLSQL_OK) {
                c->jr_synced = nlsql_journal_size(&c->eng);
                c->af_last_size = c->jr_synced;
                did_flush = 1;
            }
        } else if (sz > 0 && sz != c->jr_synced) {
            rc = nlsql_jr_sync(&c->eng);
            if (rc == NLSQL_OK) c->jr_synced = sz;
        }
    }
    frw_release(&c->frw, FRW_SNAP);
    if (rc != NLSQL_OK) nlsql_set_err(c, rc, "HY000", "flush failed");
#if defined(__GLIBC__)
    if (did_flush && rc == NLSQL_OK) malloc_trim(0);
#else
    (void)did_flush;
#endif
    return rc;
}

/* 自动落盘一个节拍：间隔内有新数据 → fsync WAL；连续一个间隔仍无新数据
   （即写入已停歇）且 journal 超过阈值 → 把脏表压实成快照。
   小量写入只 fsync、不压实，避免随机改写的表被定时重编码整表。 */
#define AF_COMPACT_MIN (64ull * 1024ull)
static int auto_land(nlsql_conn_t* c) {
    if (c->eng.readonly) return NLSQL_OK;
    int rc = NLSQL_OK;
    frw_acquire(&c->frw, FRW_SNAP);
    uint64_t sz = nlsql_journal_size(&c->eng);
    if (sz == 0) {
        c->af_last_size = 0;
    } else if (sz != c->af_last_size || sz != c->jr_synced) {
        rc = nlsql_jr_sync(&c->eng);          /* 有新写入：只保证掉电持久性 */
        if (rc == NLSQL_OK) c->jr_synced = sz;
        c->af_last_size = sz;
    } else if (sz >= AF_COMPACT_MIN) {
        rc = eng_flush(&c->eng);              /* 空闲且 WAL 已积累：压实快照 */
        if (rc == NLSQL_OK) {
            c->jr_synced = nlsql_journal_size(&c->eng);
            c->af_last_size = c->jr_synced;
        }
    }
    frw_release(&c->frw, FRW_SNAP);
    if (rc != NLSQL_OK) nlsql_set_err(c, rc, "HY000", "auto flush failed");
    return rc;
}

/* 后台压实线程：把重编码从提交路径挪走，多个压实请求自动合并为一次推进 */
static void* flush_thread_main(void* arg) {
    nlsql_conn_t* c = (nlsql_conn_t*)arg;
    /* 后台压实整体降优先级（Linux 中 nice 按线程生效，编码线程继承）：
       压实时不抢读者/写者的 CPU，让前台读写优先拿到核心 */
#if defined(__linux__)
    setpriority(PRIO_PROCESS, 0, 10);
#endif
    pthread_mutex_lock(&c->flush_mtx);
    for (;;) {
        if (c->auto_flush_ms > 0) {
            /* 定时等待：超时即做一个自动落盘节拍 */
#ifdef _WIN32
            /* winpthreads 的 pthread_cond_timedwait 按 CLOCK_REALTIME 解释绝对时间，
               这里用相对超时接口，避免时钟基准不一致导致立即超时/空转 */
            struct timespec ts;
            ts.tv_sec = c->auto_flush_ms / 1000;
            ts.tv_nsec = (long)(c->auto_flush_ms % 1000) * 1000000L;
            while (!c->flush_req && !c->flush_quit) {
                if (pthread_cond_timedwait_relative_np(&c->flush_cv, &c->flush_mtx, &ts) == ETIMEDOUT) break;
            }
#else
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += c->auto_flush_ms / 1000;
            ts.tv_nsec += (long)(c->auto_flush_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            while (!c->flush_req && !c->flush_quit) {
                if (pthread_cond_timedwait(&c->flush_cv, &c->flush_mtx, &ts) == ETIMEDOUT) break;
            }
#endif
        } else {
            while (!c->flush_req && !c->flush_quit)
                pthread_cond_wait(&c->flush_cv, &c->flush_mtx);
        }
        if (c->flush_quit && !c->flush_req) break;
        if (!c->flush_req) {
            pthread_mutex_unlock(&c->flush_mtx);
            c->flush_rc = auto_land(c);
            pthread_mutex_lock(&c->flush_mtx);
            continue;
        }
        int force = c->flush_force;
        c->flush_req = 0;
        c->flush_force = 0;
        c->flush_running = 1;
        pthread_mutex_unlock(&c->flush_mtx);

        c->flush_rc = flush_core(c, force);

        pthread_mutex_lock(&c->flush_mtx);
        c->flush_running = 0;
        pthread_cond_broadcast(&c->flush_cv);
    }
    pthread_mutex_unlock(&c->flush_mtx);
    return NULL;
}

/* 投递压实请求（不阻塞）；无后台线程时同步压实 */
void nlsql_maybe_flush(nlsql_conn_t* c) {
    if (!c->flush_th_started) { flush_core(c, 0); return; }
    pthread_mutex_lock(&c->flush_mtx);
    if (!c->flush_req) {
        c->flush_req = 1;
        pthread_cond_signal(&c->flush_cv);
    }
    pthread_mutex_unlock(&c->flush_mtx);
}

/* 空闲预压实：与 maybe_flush 同义（阈值判断交给后台线程，锁内复核） */
void nlsql_idle_flush(nlsql_conn_t* c) {
    if (c->eng.readonly) return;
    nlsql_maybe_flush(c);
}

/* 已解析语句 → 按只读性取 READ/EXCL 锁执行 */
static int exec_parsed(nlsql_conn_t* c, parsed_stmt_t* st,
                       nlsql_value_t* params, int nparams, nlsql_result_t** out) {
    frw_mode_t m = nlsql_stmt_shared_read(st) ? FRW_READ : FRW_EXCL;
    frw_acquire(&c->frw, m);
    int rc = auth_gate(c);
    if (rc == NLSQL_OK)
        rc = eng_execute(c, &c->eng, st, params, nparams, out);
    /* 阈值判断放在锁内：与 eng 状态同步，避免解锁后读到撕裂值。
       只在写锁（EXCL）下读取 journal 计量：READ 可与后台压实（SNAP）并发，
       而压实线程会更新 jr->size/jlen，读锁下读取会构成数据竞争；读语句本就不产生新写入，
       无需触发压实。 */
    int need_flush = 0;
    if (rc == NLSQL_OK && !c->eng.readonly && m == FRW_EXCL)
        need_flush = (nlsql_journal_size(&c->eng) > nlsql_journal_limit(&c->eng));
    frw_release(&c->frw, m);
    if (need_flush) nlsql_maybe_flush(c);
    return rc;
}

/* 核心执行入口：扩展预改写 → 扩展语句处理器 → 内置解析/执行。
   wire 命令循环与同步/异步/预编译接口共用同一入口，扩展能力对全部调用生效。 */
int nlsql_exec_sql(nlsql_conn_t* c, const char* sql,
                   nlsql_value_t* params, int nparams, nlsql_result_t** out) {
    /* 先置空：认证/解析提前 return 时调用方不会拿到未初始化的陈旧指针 */
    if (out) *out = NULL;
    if (!c || !sql) return NLSQL_ERR;
    int arc = auth_gate(c);
    if (arc != NLSQL_OK) return arc;

    char* rewritten = nlsql_ext_rewrite(sql);
    const char* use = rewritten ? rewritten : sql;

    nlsql_result_t* dummy = NULL;
    nlsql_result_t** hout = out ? out : &dummy;
    int hrc = nlsql_ext_dispatch(c, use, hout);
    if (hrc != NLSQL_EXT_UNHANDLED) {
        if (!out) nlsql_free_result(dummy);
        free(rewritten);
        return hrc;
    }

    /* 解析在锁外：AST 纯栈式无全局状态，锁内只包引擎执行，压缩临界区 */
    parsed_stmt_t* st = NULL;
    char errbuf[256];
    errbuf[0] = '\0';
    int rc = nlsql_parse(use, &st, errbuf, sizeof(errbuf));
    free(rewritten);
    if (rc != NLSQL_OK) {
        nlsql_set_err(c, rc, "42000", "%s", errbuf[0] ? errbuf : "syntax error");
        return rc;
    }
    if (!out) out = &dummy;
    rc = exec_parsed(c, st, params, nparams, out);
    if (out == &dummy) nlsql_free_result(dummy);
    nlsql_parsed_free(st);
    return rc;
}

static int run_sql(nlsql_conn_t* c, const char* sql,
                   nlsql_value_t* params, int nparams, nlsql_result_t** out) {
    return nlsql_exec_sql(c, sql, params, nparams, out);
}

int nlsql_query(nlsql_conn_t* conn, const char* sql, nlsql_result_t** out) {
    if (!conn || !sql) return NLSQL_ERR;
    return run_sql(conn, sql, NULL, 0, out);
}

void nlsql_free_result(nlsql_result_t* res) {
    if (!res) return;
    for (uint64_t r = 0; r < res->row_count; r++) {
        if (!res->rows[r].vals) continue;
        for (int i = 0; i < res->col_count; i++) free(res->rows[r].vals[i].s);
    }
    nlsql_val_arena_free(res); /* 行值 arena 整块释放（含已裁剪行） */
    free(res->rows);
    free(res->cols);
    free(res);
}

/* =========================================
 * 预编译语句（参数绑定，防注入）
 * ========================================= */

int nlsql_prepare(nlsql_conn_t* conn, const char* sql, nlsql_stmt_t** out) {
    if (!conn || !sql || !out) return NLSQL_ERR;
    *out = NULL;
    parsed_stmt_t* st = NULL;
    char errbuf[256];
    errbuf[0] = '\0';
    int rc = nlsql_parse(sql, &st, errbuf, sizeof(errbuf));
    if (rc != NLSQL_OK) {
        nlsql_set_err(conn, rc, "42000", "%s", errbuf[0] ? errbuf : "syntax error");
        return rc;
    }
    nlsql_stmt_t* s = (nlsql_stmt_t*)calloc(1, sizeof(nlsql_stmt_t));
    if (!s) { nlsql_parsed_free(st); return NLSQL_ERR; }
    nlsql_parsed_free(st);
    s->conn = conn;
    s->sql = strdup(sql);
    s->param_count = 0;
    /* 统计参数个数 */
    {
        parsed_stmt_t* t2 = NULL;
        if (nlsql_parse(sql, &t2, errbuf, sizeof(errbuf)) == NLSQL_OK) {
            s->param_count = t2->param_count;
            nlsql_parsed_free(t2);
        }
    }
    if (!s->sql) { free(s); return NLSQL_ERR; }
    *out = s;
    return NLSQL_OK;
}

static int stmt_bind(nlsql_stmt_t* s, int idx, nlsql_value_t v) {
    if (!s || idx < 0 || idx >= NLSQL_MAX_PARAMS) return NLSQL_ERR;
    free(s->params[idx].s);
    s->params[idx] = v;
    s->bound[idx] = 1;
    if (idx + 1 > s->param_count) s->param_count = idx + 1;
    return NLSQL_OK;
}

int nlsql_stmt_bind_null(nlsql_stmt_t* stmt, int idx) {
    nlsql_value_t v; memset(&v, 0, sizeof(v));
    v.type = NLSQL_TYPE_NULL; v.is_null = 1;
    return stmt_bind(stmt, idx, v);
}
int nlsql_stmt_bind_int(nlsql_stmt_t* stmt, int idx, int64_t val) {
    nlsql_value_t v; memset(&v, 0, sizeof(v));
    v.type = NLSQL_TYPE_INT64; v.i = val;
    return stmt_bind(stmt, idx, v);
}
int nlsql_stmt_bind_double(nlsql_stmt_t* stmt, int idx, double val) {
    nlsql_value_t v; memset(&v, 0, sizeof(v));
    v.type = NLSQL_TYPE_DOUBLE; v.d = val;
    return stmt_bind(stmt, idx, v);
}
int nlsql_stmt_bind_text(nlsql_stmt_t* stmt, int idx, const char* s, uint32_t len) {
    if (!s) return nlsql_stmt_bind_null(stmt, idx);
    nlsql_value_t v; memset(&v, 0, sizeof(v));
    v.type = NLSQL_TYPE_VARCHAR; v.len = len;
    v.s = (char*)malloc((size_t)len + 1);
    if (!v.s) return NLSQL_ERR;
    memcpy(v.s, s, len); v.s[len] = '\0';
    return stmt_bind(stmt, idx, v);
}

int nlsql_stmt_param_count(nlsql_stmt_t* stmt) { return stmt ? stmt->param_count : 0; }

int nlsql_stmt_execute(nlsql_stmt_t* stmt, nlsql_result_t** out) {
    if (!stmt) return NLSQL_ERR;
    for (int i = 0; i < stmt->param_count; i++) {
        if (!stmt->bound[i]) {
            nlsql_set_err(stmt->conn, NLSQL_ER_UNKNOWN_STMT, "HY000",
                          "Incorrect arguments to EXECUTE (param %d unbound)", i);
            return NLSQL_ER_UNKNOWN_STMT;
        }
    }
    return run_sql(stmt->conn, stmt->sql, stmt->params, stmt->param_count, out);
}

void nlsql_stmt_free(nlsql_stmt_t* stmt) {
    if (!stmt) return;
    for (int i = 0; i < NLSQL_MAX_PARAMS; i++) free(stmt->params[i].s);
    free(stmt->sql);
    free(stmt);
}

/* =========================================
 * 异步查询
 * ========================================= */

typedef struct {
    char* sql;
    nlsql_result_cb cb;
    void* userdata;
    nlsql_value_t* params; /* stmt 参数快照（字符串深拷贝）：提交后 stmt 可随时 bind/free，
                              与 worker 线程解耦，消除异步 stmt 生命期竞争 */
    int nparams;
} async_job_t;

static void async_job_free(void* arg) {
    async_job_t* j = (async_job_t*)arg;
    if (!j) return;
    if (j->params) {
        for (int i = 0; i < j->nparams; i++) free(j->params[i].s);
        free(j->params);
    }
    free(j->sql);
    free(j);
}

static void async_run_query(nlsql_conn_t* c, void* arg) {
    async_job_t* j = (async_job_t*)arg;
    nlsql_result_t* res = NULL;
    int rc = run_sql(c, j->sql, j->params, j->nparams, &res);
    /* 流式空闲预压实：队列已空才做，避免与后续提交撞车（压实在 SNAP 锁下挡写放读） */
    if (aq_pending(c->aq) == 0) nlsql_idle_flush(c);
    if (j->cb) j->cb(c, rc, res, j->userdata);
    nlsql_free_result(res);
    async_job_free(j);
}

int nlsql_query_async(nlsql_conn_t* conn, const char* sql,
                      nlsql_result_cb cb, void* userdata) {
    if (!conn || !sql) return NLSQL_ERR;
    async_job_t* j = (async_job_t*)calloc(1, sizeof(async_job_t));
    if (!j) return NLSQL_ERR;
    j->sql = strdup(sql);
    j->cb = cb; j->userdata = userdata;
    if (!j->sql) { async_job_free(j); return NLSQL_ERR; }
    int rc = aq_push(conn->aq, async_run_query, j, async_job_free);
    if (rc != NLSQL_OK) async_job_free(j);
    return rc;
}

int nlsql_stmt_execute_async(nlsql_stmt_t* stmt, nlsql_result_cb cb, void* userdata) {
    if (!stmt || !stmt->sql) return NLSQL_ERR;
    async_job_t* j = (async_job_t*)calloc(1, sizeof(async_job_t));
    if (!j) return NLSQL_ERR;
    j->cb = cb; j->userdata = userdata;
    j->sql = strdup(stmt->sql);
    if (!j->sql) { async_job_free(j); return NLSQL_ERR; }
    /* 参数快照：在提交线程深拷贝，worker 只读快照，stmt 随后被释放/重绑都安全 */
    if (stmt->param_count > 0) {
        j->params = (nlsql_value_t*)calloc((size_t)stmt->param_count, sizeof(nlsql_value_t));
        if (!j->params) { async_job_free(j); return NLSQL_ERR; }
        for (int i = 0; i < stmt->param_count; i++) {
            const nlsql_value_t* s = &stmt->params[i];
            j->params[i] = *s;
            j->params[i].s = NULL;
            if (s->s) {
                j->params[i].s = (char*)malloc((size_t)s->len + 1);
                if (!j->params[i].s) { j->nparams = i; async_job_free(j); return NLSQL_ERR; }
                memcpy(j->params[i].s, s->s, s->len);
                j->params[i].s[s->len] = '\0';
            }
            j->nparams = i + 1;
        }
    }
    int rc = aq_push(stmt->conn->aq, async_run_query, j, async_job_free);
    if (rc != NLSQL_OK) async_job_free(j);
    return rc;
}

int nlsql_submit(nlsql_conn_t* conn, const char* sql) {
    return nlsql_query_async(conn, sql, NULL, NULL); /* 结果/错误丢弃（cb=NULL） */
}

void nlsql_async_wait(nlsql_conn_t* conn) {
    if (!conn || !conn->aq) return;
    aq_wait_idle(conn->aq);
    /* 空闲同步点：journal 过半阈值则顺手压实（此时无人排队） */
    nlsql_idle_flush(conn);
}

/* =========================================
 * MySQL 协议适配层（可选服务端）
 * ========================================= */

/* 句柄表：避免 64 位指针截断为 int（记录归属 conn，供 close 自动停线） */
#define NLSQL_WIRE_MAX 16
typedef struct { wire_srv_t* w; nlsql_conn_t* conn; } wire_slot_t;
static pthread_mutex_t g_wire_mtx = PTHREAD_MUTEX_INITIALIZER;
static wire_slot_t g_wire_tab[NLSQL_WIRE_MAX];

/* 把已启动的 wire 服务登记进句柄表；失败则停掉 */
static int wire_register(nlsql_conn_t* conn, wire_srv_t* w) {
    if (!w) return NLSQL_ERR_PROTOCOL;
    pthread_mutex_lock(&g_wire_mtx);
    for (int i = 0; i < NLSQL_WIRE_MAX; i++) {
        if (!g_wire_tab[i].w) {
            g_wire_tab[i].w = w;
            g_wire_tab[i].conn = conn;
            pthread_mutex_unlock(&g_wire_mtx);
            return i + 1;
        }
    }
    pthread_mutex_unlock(&g_wire_mtx);
    wire_stop(w);
    nlsql_set_global_err("too many wire servers");
    return NLSQL_ERR_PROTOCOL;
}

int nlsql_wire_start(nlsql_conn_t* conn, int port, int max_clients) {
    return nlsql_wire_start_tls(conn, port, max_clients, NULL, NULL);
}

int nlsql_wire_start_tls(nlsql_conn_t* conn, int port, int max_clients,
                         const char* cert_file, const char* key_file) {
    if (!conn) return NLSQL_ERR;
    char err[160] = "start failed";
    wire_srv_t* w = wire_start_tls(conn, port, max_clients, cert_file, key_file,
                                   err, sizeof(err));
    if (!w) { nlsql_set_global_err("%s", err); return NLSQL_ERR_PROTOCOL; }
    return wire_register(conn, w);
}

void nlsql_wire_stop(int handle) {
    if (handle <= 0 || handle > NLSQL_WIRE_MAX) return;
    pthread_mutex_lock(&g_wire_mtx);
    wire_srv_t* w = g_wire_tab[handle - 1].w;
    g_wire_tab[handle - 1].w = NULL;
    g_wire_tab[handle - 1].conn = NULL;
    pthread_mutex_unlock(&g_wire_mtx);
    if (w) wire_stop(w);
}

/* close 前置：停掉本连接的全部 wire 并等其 cli 线程退场。
   否则 cli_thread 会在 conn 的锁被 destroy/释放后继续操作 → use-after-free / 堆破坏。 */
static void stop_wires_for_conn(nlsql_conn_t* conn) {
    for (;;) {
        wire_srv_t* found = NULL;
        pthread_mutex_lock(&g_wire_mtx);
        for (int i = 0; i < NLSQL_WIRE_MAX; i++) {
            if (g_wire_tab[i].w && g_wire_tab[i].conn == conn) {
                found = g_wire_tab[i].w;
                g_wire_tab[i].w = NULL;
                g_wire_tab[i].conn = NULL;
                break;
            }
        }
        pthread_mutex_unlock(&g_wire_mtx);
        if (!found) return;
        wire_stop(found); /* 内部 shutdown 全部连接并等 nclients 归零，阻塞安全（close 尚未持锁） */
    }
}

/* =========================================
 * 宿主直连 API（进程内单例默认会话）
 * ========================================= */

static pthread_mutex_t g_host_mtx = PTHREAD_MUTEX_INITIALIZER;
static nlsql_conn_t* g_host_conn = NULL;

int nlsql_host_open(const char* data_dir, const char* user, const char* password) {
    if (!data_dir || !data_dir[0]) {
        nlsql_set_global_err("data_dir required");
        return NLSQL_ERR_PATH;
    }
    pthread_mutex_lock(&g_host_mtx);
    if (g_host_conn) {
        pthread_mutex_unlock(&g_host_mtx);
        return NLSQL_OK; /* 已打开：幂等 */
    }
    nlsql_open_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.data_dir = data_dir;
    nlsql_conn_t* c = nlsql_open_ex(&opts);
    if (!c) {
        pthread_mutex_unlock(&g_host_mtx);
        return NLSQL_ERR;
    }
    if (user && user[0]) {
        if (nlsql_set_credentials(c, user, password ? password : "") != NLSQL_OK) {
            nlsql_close(c);
            pthread_mutex_unlock(&g_host_mtx);
            return NLSQL_ERR;
        }
    }
    g_host_conn = c;
    pthread_mutex_unlock(&g_host_mtx);
    return NLSQL_OK;
}

void nlsql_host_close(void) {
    pthread_mutex_lock(&g_host_mtx);
    nlsql_conn_t* c = g_host_conn;
    g_host_conn = NULL;
    pthread_mutex_unlock(&g_host_mtx);
    if (c) nlsql_close(c);
}

nlsql_conn_t* nlsql_host_conn(void) {
    pthread_mutex_lock(&g_host_mtx);
    nlsql_conn_t* c = g_host_conn;
    pthread_mutex_unlock(&g_host_mtx);
    return c;
}

int nlsql_host_exec(const char* sql, nlsql_result_t** out) {
    if (!sql) return NLSQL_ERR;
    nlsql_conn_t* c = nlsql_host_conn();
    if (!c) {
        nlsql_set_global_err("host session not open");
        return NLSQL_ERR;
    }
    return nlsql_query(c, sql, out);
}

int nlsql_host_create_db(const char* name) {
    if (!nlsql_valid_ident(name)) {
        nlsql_set_global_err("invalid database name");
        return NLSQL_ERR_PATH;
    }
    char sql[24 + NLSQL_MAX_IDENT + 1];
    snprintf(sql, sizeof(sql), "CREATE DATABASE %s", name);
    return nlsql_host_exec(sql, NULL);
}

int nlsql_host_drop_db(const char* name) {
    if (!nlsql_valid_ident(name)) {
        nlsql_set_global_err("invalid database name");
        return NLSQL_ERR_PATH;
    }
    char sql[4 + NLSQL_MAX_IDENT + 1];
    snprintf(sql, sizeof(sql), "DROP DATABASE %s", name);
    return nlsql_host_exec(sql, NULL);
}

int nlsql_host_use(const char* name) {
    if (!nlsql_valid_ident(name)) {
        nlsql_set_global_err("invalid database name");
        return NLSQL_ERR_PATH;
    }
    char sql[4 + NLSQL_MAX_IDENT + 1];
    snprintf(sql, sizeof(sql), "USE %s", name);
    return nlsql_host_exec(sql, NULL);
}

int nlsql_host_flush(void) {
    nlsql_conn_t* c = nlsql_host_conn();
    if (!c) return NLSQL_ERR;
    return nlsql_flush(c);
}

/* =========================================
 * 工具
 * ========================================= */

const char* nlsql_version(void) { return NLSQL_VERSION; }
