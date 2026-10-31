/* 内存容量探顶：写行直到 RSS 触及 ~1.9G（ulimit -v 2G 保险丝），
   每 100 万行 flush 计时（大表压实尖峰），打印分段写速 */
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RSS_STOP_KB (1945L * 1024)   /* 1.945 GB，给 page cache 留余量 */
#define MAX_ROWS    30000000L

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static long rss_kb(void) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256]; long v = -1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "VmRSS:", 6) == 0) { sscanf(line + 6, "%ld", &v); break; }
    fclose(f);
    return v;
}

int main(void) {
    setbuf(stdout, NULL);
    system("rm -rf /workspace/nlsql/testenv/data_cap && mkdir -p /workspace/nlsql/testenv/data_cap");
    nlsql_conn_t* db = nlsql_open("/workspace/nlsql/testenv/data_cap");
    if (!db) { printf("open fail: %s\n", nlsql_global_error()); return 1; }
    nlsql_result_t* r = NULL;
    nlsql_query(db, "CREATE DATABASE b", &r); if (r) { nlsql_free_result(r); r = NULL; }
    nlsql_query(db, "USE b", &r); if (r) { nlsql_free_result(r); r = NULL; }
    nlsql_query(db, "CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(64), s DOUBLE)", &r);
    if (r) { nlsql_free_result(r); r = NULL; }

    long i = 0, since_mark = 0, next_mark = 500000, next_flush = 1000000;
    long peak_flush_ms = 0;
    double seg_t0 = now_ms(), t_start = now_ms(), peak_flush = 0;
    int rc_all = 0;

    while (i < MAX_ROWS) {
        long rss = rss_kb();
        if (rss >= RSS_STOP_KB) { printf("STOP: rss=%ld KB >= %ld KB\n", rss, RSS_STOP_KB); break; }

        char sql[16384] = "INSERT INTO t VALUES ";
        int off = (int)strlen(sql);
        long start = i;
        for (long k = 0; i < MAX_ROWS && off < 15000; i++, k++) {
            off += snprintf(sql + off, sizeof(sql) - (size_t)off, "%s(%ld,'val-%ld',%ld.%d)",
                            k ? "," : "", i, i, i / 100, (int)(i % 100));
        }
        if (nlsql_query(db, sql, &r) != NLSQL_OK) {
            printf("INSERT_FAIL @%ld: %s (rss=%ld KB)\n", i, nlsql_error(db), rss_kb());
            if (r) nlsql_free_result(r);
            rc_all = 1;
            break;
        }
        if (r) { nlsql_free_result(r); r = NULL; }
        since_mark += i - start;

        if (i >= next_mark) {
            next_mark += 500000;
            printf("rows=%ld rss=%.0f MB\n", i, rss_kb() / 1024.0);
        }
        if (i >= next_flush) {
            next_flush += 1000000;
            double seg = now_ms() - seg_t0;
            printf("  [seg] last 1M rows in %.1f s = %.0f rows/s\n", seg / 1000.0, 1000000.0 / seg);
            double f0 = now_ms();
            int frc = nlsql_flush(db);
            double f1 = now_ms();
            printf("FLUSH@%ld rows: %.0f ms rc=%d rss=%.0f MB\n", i, f1 - f0, frc, rss_kb() / 1024.0);
            if (f1 - f0 > peak_flush) { peak_flush = f1 - f0; peak_flush_ms = (long)(f1 - f0); }
            seg_t0 = now_ms();
        }
    }

    printf("MAX_ROWS=%ld peak_flush_ms=%ld total_sec=%.0f rss=%.1f MB\n", i, peak_flush_ms,
           (now_ms() - t_start) / 1000.0, rss_kb() / 1024.0);
    /* 点查抽样：大表下哈希索引仍然 O(1)？ */
    double t0 = now_ms(); uint64_t hit = 0;
    for (int k = 0; k < 100000; k++) {
        char q[64];
        snprintf(q, sizeof q, "SELECT v FROM t WHERE id=%ld", (long)(k * 31 % (i > 0 ? i : 1)));
        if (nlsql_query(db, q, &r) == NLSQL_OK && r && r->row_count) hit++;
        if (r) { nlsql_free_result(r); r = NULL; }
    }
    double t1 = now_ms();
    printf("PK hit x100000 on %ld rows: %.1f ms %.0f q/s hits=%llu\n", i, t1 - t0,
           100000.0 / (t1 - t0) * 1000, (unsigned long long)hit);

    nlsql_close(db);
    printf("rss_after_close=%.1f MB\n", rss_kb() / 1024.0);
    return rc_all;
}
