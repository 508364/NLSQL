/* 校验：不强制 flush 直接 close → 重开按 LSN 重放，数据必须完整 */
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int run(nlsql_conn_t* c, const char* sql) {
    nlsql_result_t* r = NULL;
    int rc = nlsql_query(c, sql, &r);
    if (rc != NLSQL_OK) { fprintf(stderr, "ERR %s: %s\n", sql, nlsql_global_error()); return rc; }
    nlsql_free_result(r);
    return 0;
}

static long long scalar(nlsql_conn_t* c, const char* sql) {
    nlsql_result_t* r = NULL;
    if (nlsql_query(c, sql, &r) != NLSQL_OK) { fprintf(stderr, "ERR %s\n", sql); return -1; }
    long long v = -1;
    if (r->row_count && r->col_count) v = r->rows[0].vals[0].i;
    nlsql_free_result(r);
    return v;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "d_reopen";
    int vlen = argc > 2 ? atoi(argv[2]) : 64;   /* 行宽（VARCHAR 长度） */
    int nrows = argc > 3 ? atoi(argv[3]) : 300; /* 总行数 */
    int split = nrows * 2 / 3;                  /* 前 2/3 进快照，后 1/3 留在 WAL */
    char* payload = (char*)malloc((size_t)vlen + 1);
    size_t sqlcap = (size_t)vlen + 256;
    char* sql = (char*)malloc(sqlcap);
    if (!payload || !sql) return 1;
    memset(payload, 'x', (size_t)vlen);
    payload[vlen] = '\0';
    char ddlsql[128];
    snprintf(ddlsql, sizeof(ddlsql), "CREATE TABLE t (id INTEGER PRIMARY KEY, v VARCHAR(%d))", vlen);

    nlsql_conn_t* c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "open1 fail: %s\n", nlsql_global_error()); return 1; }
    run(c, "CREATE DATABASE d");
    run(c, "USE d");
    run(c, ddlsql);
    for (int i = 1; i <= split; i++) {
        snprintf(sql, sqlcap, "INSERT INTO t (id, v) VALUES (%d, '%s')", i, payload);
        run(c, sql);
    }
    nlsql_flush(c);                       /* 让前 2/3 行进快照，其余留在 WAL */
    for (int i = split + 1; i <= nrows; i++) {
        snprintf(sql, sqlcap, "INSERT INTO t (id, v) VALUES (%d, '%s')", i, payload);
        run(c, sql);
    }
    run(c, "UPDATE t SET v='changed' WHERE id = 5");
    run(c, "DELETE FROM t WHERE id = 7");
    nlsql_close(c);                       /* 不 flush，直接关 */

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "open2 fail\n"); return 1; }
    run(c, "USE d");                      /* USE 触发该库 journal 全量重放 */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double reopen_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("REOPEN open+replay_ms=%.1f wal_mb=%.1f\n", reopen_ms, (double)vlen * (nrows - split) / (1024.0 * 1024.0));
    long long n = scalar(c, "SELECT COUNT(*) FROM t");
    long long ch = scalar(c, "SELECT COUNT(*) FROM t WHERE id = 5");
    printf("REOPEN count=%lld expect=%d changed_ok=%lld\n", n, nrows - 1, ch);
    /* 校验 UPDATE 生效：点查 id=5 的 v 长度 */
    {
        nlsql_result_t* r = NULL;
        if (nlsql_query(c, "SELECT v FROM t WHERE id = 5", &r) != NLSQL_OK) { fprintf(stderr, "q fail\n"); return 1; }
        printf("REOPEN v5=%s\n", (r->row_count && r->rows[0].vals[0].s) ? r->rows[0].vals[0].s : "(null)");
        nlsql_free_result(r);
    }
    long long gone = scalar(c, "SELECT COUNT(*) FROM t WHERE id = 7");
    printf("REOPEN id7_removed=%lld\n", (long long)(gone == 0 ? 1 : 0));
    nlsql_close(c);
    free(payload);
    free(sql);
    int ok = (n == nrows - 1) && (ch == 1) && (gone == 0);
    printf("REOPEN %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
