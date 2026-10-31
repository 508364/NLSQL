/* 全量嵌入式基准：覆盖 README 性能表全部行（3 次取中位） */
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* 读 /proc/self/status 的 VmRSS / VmHWM（kB） */
static long vm_kb(const char* key) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256]; long v = -1; size_t klen = strlen(key);
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, klen) == 0) { sscanf(line + klen, "%ld", &v); break; }
    fclose(f);
    return v;
}

static void exec(nlsql_conn_t* db, const char* sql) {
    nlsql_result_t* r = NULL;
    nlsql_query(db, sql, &r);
    if (r) nlsql_free_result(r);
}

int main(void) {
    system("rm -rf /workspace/nlsql/testenv/data_b3 && mkdir -p /workspace/nlsql/testenv/data_b3");
    nlsql_conn_t* db = nlsql_open("/workspace/nlsql/testenv/data_b3"); if (!db) { puts("open fail"); return 1; }
    nlsql_result_t* r = NULL;
    exec(db, "CREATE DATABASE b");
    exec(db, "USE b");
    double t0, t1;
    exec(db, "CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(30), s DOUBLE)");

    /* 1. 批插 5 万行 */
    t0 = now_ms();
    for (int b = 0; b < 100; b++) {
        char sql[16384] = "INSERT INTO t VALUES ";
        int off = (int)strlen(sql);
        for (int i = 0; i < 500; i++) {
            off += snprintf(sql + off, sizeof(sql) - off, "%s(%d,'val-%d-%d',%d.%d)",
                            i ? "," : "", b * 500 + i, b, i, b, i);
        }
        nlsql_query(db, sql, &r);
        if (r) { nlsql_free_result(r); r = NULL; }
    }
    t1 = now_ms();
    printf("INSERT x50000 (batch): %.1f ms  %.0f rows/s\n", t1 - t0, 50000.0 / (t1 - t0) * 1000);

    /* 2. 主键冲突拒绝 x2000 */
    int rej = 0;
    t0 = now_ms();
    for (int i = 0; i < 2000; i++) {
        char q[128];
        snprintf(q, sizeof q, "INSERT INTO t VALUES (%d,'dup',1.0)", i);
        if (nlsql_query(db, q, &r) != NLSQL_OK) rej++;
        if (r) { nlsql_free_result(r); r = NULL; }
    }
    t1 = now_ms();
    printf("PK dup reject x2000: %.1f ms  rejected=%d\n", t1 - t0, rej);

    /* 3. UPDATE 全表 5 万行 */
    t0 = now_ms();
    nlsql_query(db, "UPDATE t SET v='upd'", &r);
    t1 = now_ms();
    printf("UPDATE all 50000: %.1f ms  rc=%d\n", t1 - t0, r ? 0 : -1);
    if (r) { nlsql_free_result(r); r = NULL; }

    /* 4. COUNT */
    t0 = now_ms();
    nlsql_query(db, "SELECT COUNT(*) FROM t", &r);
    t1 = now_ms();
    printf("SELECT COUNT(*): %.1f ms (%lld)\n", t1 - t0,
           r && r->row_count ? (long long)r->rows[0].vals[0].i : -1LL);
    if (r) { nlsql_free_result(r); r = NULL; }

    /* 5. 3 列全量 */
    t0 = now_ms();
    nlsql_query(db, "SELECT id, v, s FROM t", &r);
    t1 = now_ms();
    printf("SELECT 3 cols: %.1f ms  rows=%llu  %.0f rows/s\n", t1 - t0,
           r ? (unsigned long long)r->row_count : 0,
           r && r->row_count ? (double)r->row_count / (t1 - t0) * 1000 : 0);
    if (r) { nlsql_free_result(r); r = NULL; }

    /* 6. ORDER BY + LIMIT（全量排序） */
    t0 = now_ms();
    nlsql_query(db, "SELECT id, v, s FROM t ORDER BY id DESC LIMIT 50000", &r);
    t1 = now_ms();
    printf("ORDER BY+LIMIT: %.1f ms  rows=%llu\n", t1 - t0,
           r ? (unsigned long long)r->row_count : 0);
    if (r) { nlsql_free_result(r); r = NULL; }

    /* 7. 主键点查 hit / miss */
    char q[128]; uint64_t hit = 0;
    t0 = now_ms();
    for (int i = 0; i < 100000; i++) {
        snprintf(q, sizeof q, "SELECT v FROM t WHERE id=%d", (int)((int64_t)i * 49999 % 50000));
        nlsql_query(db, q, &r);
        if (r) { if (r->row_count) hit++; nlsql_free_result(r); r = NULL; }
    }
    t1 = now_ms();
    printf("PK hit SELECT x100000: %.1f ms  %.0f q/s  hits=%llu\n", t1 - t0,
           100000.0 / (t1 - t0) * 1000, (unsigned long long)hit);

    t0 = now_ms();
    for (int i = 0; i < 100000; i++) {
        snprintf(q, sizeof q, "SELECT v FROM t WHERE id=%d", 900000 + i);
        nlsql_query(db, q, &r);
        if (r) { nlsql_free_result(r); r = NULL; }
    }
    t1 = now_ms();
    printf("PK miss SELECT x100000: %.1f ms  %.0f q/s\n", t1 - t0, 100000.0 / (t1 - t0) * 1000);

    /* 8. SELECT * 全量取出（持结果读 RSS = 5 万行 3 列表峰值） */
    t0 = now_ms();
    nlsql_query(db, "SELECT * FROM t", &r);
    t1 = now_ms();
    printf("SELECT * 50000: %.1f ms  rows=%llu\n", t1 - t0,
           r ? (unsigned long long)r->row_count : 0);
    printf("RSS after SELECT*: %.2f MB\n", vm_kb("VmRSS:") / 1024.0);
    if (r) { nlsql_free_result(r); r = NULL; }

    /* 9. 单聚合 x4（SUM/AVG/MIN/MAX 分四条，引擎为够用版单聚合） */
    static const char* agg[4] = {
        "SELECT SUM(id) FROM t", "SELECT AVG(id) FROM t",
        "SELECT MIN(id) FROM t", "SELECT MAX(id) FROM t"
    };
    t0 = now_ms();
    for (int i = 0; i < 4; i++) {
        nlsql_query(db, agg[i], &r);
        if (r) { nlsql_free_result(r); r = NULL; }
    }
    t1 = now_ms();
    printf("SUM/AVG/MIN/MAX x4: %.1f ms total\n", t1 - t0);

    /* 10. 线性扩展：批插 20 万行（独立表） */
    exec(db, "CREATE TABLE t2 (id INT PRIMARY KEY, v VARCHAR(30))");
    t0 = now_ms();
    for (int b = 0; b < 400; b++) {
        char sql[16384] = "INSERT INTO t2 VALUES ";
        int off = (int)strlen(sql);
        for (int i = 0; i < 500; i++) {
            off += snprintf(sql + off, sizeof(sql) - off, "%s(%d,'r%d-%d')",
                            i ? "," : "", b * 500 + i, b, i);
        }
        nlsql_query(db, sql, &r);
        if (r) { nlsql_free_result(r); r = NULL; }
    }
    t1 = now_ms();
    printf("INSERT x200000 (batch): %.1f ms  %.0f rows/s\n", t1 - t0, 200000.0 / (t1 - t0) * 1000);

    /* 11. GROUP BY + HAVING（5 万行、10 组） */
    exec(db, "CREATE TABLE g (id INT PRIMARY KEY, k INT)");
    for (int b = 0; b < 100; b++) {
        char sql[16384] = "INSERT INTO g VALUES ";
        int off = (int)strlen(sql);
        for (int i = 0; i < 500; i++) {
            off += snprintf(sql + off, sizeof(sql) - off, "%s(%d,%d)",
                            i ? "," : "", b * 500 + i, i % 10);
        }
        nlsql_query(db, sql, &r);
        if (r) { nlsql_free_result(r); r = NULL; }
    }
    t0 = now_ms();
    nlsql_query(db, "SELECT k FROM g GROUP BY k HAVING COUNT(*) > 4000", &r);
    t1 = now_ms();
    printf("GROUP BY+HAVING (50000 rows, 10 groups): %.1f ms  groups=%llu\n", t1 - t0,
           r ? (unsigned long long)r->row_count : 0);
    if (r) { nlsql_free_result(r); r = NULL; }

    nlsql_close(db);
    printf("RSS after close: %.2f MB\n", vm_kb("VmRSS:") / 1024.0);
    return 0;
}
