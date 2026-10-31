/* 增量帧快照基准：同一张大表上对比「整表重写」与「只追加新行」的 flush 耗时与 IO 量。
   整表重写通过一次 UPDATE（既有行被改 → 无法增量）触发；增量通过插入少量新行触发 */
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int run(nlsql_conn_t* c, const char* sql) {
    nlsql_result_t* r = NULL;
    int rc = nlsql_query(c, sql, &r);
    if (rc != NLSQL_OK) { fprintf(stderr, "ERR %s: %s\n", sql, nlsql_global_error()); return rc; }
    nlsql_free_result(r);
    return 0;
}

static long long fsize(const char* p) {
    struct stat st; return stat(p, &st) == 0 ? (long long)st.st_size : -1;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "d_incrbench";
    int nrows = argc > 2 ? atoi(argv[2]) : 2048;
    int vlen  = argc > 3 ? atoi(argv[3]) : 65536;
    int extra = argc > 4 ? atoi(argv[4]) : 8;

    char* payload = (char*)malloc((size_t)vlen + 1);
    size_t sqlcap = (size_t)vlen + 256;
    char* sql = (char*)malloc(sqlcap);
    char ddlsql[128];
    char nlt[512];
    if (!payload || !sql) return 1;
    memset(payload, 'x', (size_t)vlen); payload[vlen] = '\0';
    snprintf(ddlsql, sizeof(ddlsql), "CREATE TABLE t (id INTEGER PRIMARY KEY, v VARCHAR(%d))", vlen);
    snprintf(nlt, sizeof(nlt), "%s/d/t.nlt", dir);

    nlsql_conn_t* c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "open fail: %s\n", nlsql_global_error()); return 1; }
    run(c, "CREATE DATABASE d");
    run(c, "USE d");
    run(c, ddlsql);
    double t0 = now_ms();
    for (int i = 1; i <= nrows; i++) {
        snprintf(sql, sqlcap, "INSERT INTO t (id, v) VALUES (%d, '%s')", i, payload);
        if (run(c, sql) != 0) return 1;
    }
    double t_ins = now_ms() - t0;
    double f0 = now_ms(); nlsql_flush(c); double t_base = now_ms() - f0;
    long long s0 = fsize(nlt);
    printf("INCRBENCH rows=%d vlen=%d insert_ms=%.0f base_flush_ms=%.0f file_mb=%.1f\n",
           nrows, vlen, t_ins, t_base, (double)s0 / (1024 * 1024));

    /* 全量重写基线：改一行 → 只能整表重写 */
    run(c, "UPDATE t SET v='changed' WHERE id = 1");
    f0 = now_ms(); nlsql_flush(c); double t_full = now_ms() - f0;
    long long s1 = fsize(nlt);
    printf("INCRBENCH full_rewrite flush_ms=%.1f write_mb=%.1f\n", t_full, (double)s1 / (1024 * 1024));

    /* 增量：只插 extra 行 → 只追加这些行 */
    for (int k = 1; k <= extra; k++) {
        snprintf(sql, sqlcap, "INSERT INTO t (id, v) VALUES (%d, '%s')", nrows + k, payload);
        if (run(c, sql) != 0) return 1;
    }
    f0 = now_ms(); nlsql_flush(c); double t_app = now_ms() - f0;
    long long s2 = fsize(nlt);
    printf("INCRBENCH append_only flush_ms=%.1f write_mb=%.1f speedup=%.1fx\n",
           t_app, (double)(s2 - s1) / (1024 * 1024), t_full / (t_app > 0.01 ? t_app : 0.01));

    /* 多轮「插几行 + flush」：验证帧累积下每轮仍是只追加 */
    double acc = 0; long long s_prev = s2;
    int rounds = 4;
    for (int r = 1; r <= rounds; r++) {
        for (int k = 1; k <= extra; k++) {
            int id = nrows + extra * r + k;
            snprintf(sql, sqlcap, "INSERT INTO t (id, v) VALUES (%d, '%s')", id, payload);
            if (run(c, sql) != 0) return 1;
        }
        f0 = now_ms(); nlsql_flush(c); double fr = now_ms() - f0; acc += fr;
        long long s3 = fsize(nlt);
        printf("INCRBENCH round%d flush_ms=%.1f growth_mb=%.1f\n", r, fr,
               (double)(s3 - s_prev) / (1024 * 1024));
        s_prev = s3;
    }
    printf("INCRBENCH rounds_avg_flush_ms=%.1f (vs full %.1f)\n", acc / rounds, t_full);
    nlsql_close(c);

    /* 重开校验行数与最后一帧的行 */
    t0 = now_ms();
    c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "reopen fail\n"); return 1; }
    run(c, "USE d");
    double t_open = now_ms() - t0;
    nlsql_result_t* r = NULL;
    if (nlsql_query(c, "SELECT COUNT(*) FROM t", &r) != NLSQL_OK) return 1;
    long long n = r->rows[0].vals[0].i;
    nlsql_free_result(r);
    long long expect = nrows + extra * 5;
    printf("INCRBENCH reopen_ms=%.0f count=%lld expect=%lld\n", t_open, n, expect);
    nlsql_close(c);
    printf("INCRBENCH %s\n", n == expect ? "OK" : "FAIL");
    free(payload); free(sql);
    return n == expect ? 0 : 1;
}
