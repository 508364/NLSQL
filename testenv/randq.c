/* 1C2G 随机查询 / 多库排列基准
 * 场景：4 个库 × 2 张表，每表 5 万行；随机 WHERE / 范围 / LIKE / 聚合 / 分组 / 排序，
 *       以及随机跨库切换、随机点查、随机读写混跑。
 * 约束：调用方用 taskset 绑 1 核、ulimit -v 2GB 运行（本程序内也用 workers=1 模拟单线程）。
 * 输出：每类操作 n / 总耗时 / 平均 / p50 / p95 / p99 / max / QPS，末尾给峰值 RSS。
 * 用法：./randq <dir> [rows_per_table] [seed]
 */
#define _POSIX_C_SOURCE 200809L
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define NDB     4
#define NTBL    2

static uint64_t g_seed;
static uint64_t rnd(void) {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return g_seed;
}
static long rnd_range(long lo, long hi) { return lo + (long)(rnd() % (uint64_t)(hi - lo + 1)); }

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

typedef struct {
    const char* name;
    long n, cap;
    double* lat_us;
} bench_t;

static void mem_line(const char* tag);

static int bench_init(bench_t* b, const char* name, long n) {
    memset(b, 0, sizeof(*b));
    b->name = name; b->cap = n;
    b->lat_us = (double*)malloc((size_t)n * sizeof(double));
    return b->lat_us ? 0 : -1;
}

static void bench_report(bench_t* b) {
    if (!b->n) { printf("RANDQ %s n=0\n", b->name); return; }
    qsort(b->lat_us, (size_t)b->n, sizeof(double), cmp_d);
    double total = 0;
    for (long i = 0; i < b->n; i++) total += b->lat_us[i];
    double p50 = b->lat_us[b->n * 50 / 100];
    double p95 = b->lat_us[(long)((double)b->n * 0.95) < b->n ? (long)((double)b->n * 0.95) : b->n - 1];
    double p99 = b->lat_us[(long)((double)b->n * 0.99) < b->n ? (long)((double)b->n * 0.99) : b->n - 1];
    printf("RANDQ %-14s n=%-6ld total_ms=%-9.1f avg_us=%-8.1f p50=%-7.1f p95=%-8.1f p99=%-8.1f max_us=%-9.1f qps=%.0f\n",
           b->name, b->n, total / 1000.0, total / b->n,
           p50, p95, p99, b->lat_us[b->n - 1], b->n * 1000000.0 / (total > 0 ? total : 1));
    free(b->lat_us); b->lat_us = NULL;
    mem_line(b->name);
}

/* 执行一条 SQL 并记录一次延迟（µs） */
static int q(nlsql_conn_t* c, bench_t* b, const char* sql) {
    nlsql_result_t* r = NULL;
    double t0 = now_ms();
    int rc = nlsql_query(c, sql, &r);
    double dt = (now_ms() - t0) * 1000.0;
    if (rc != NLSQL_OK) { fprintf(stderr, "ERR %s: %s\n", sql, nlsql_global_error()); return -1; }
    nlsql_free_result(r);
    if (b && b->n < b->cap) b->lat_us[b->n++] = dt;
    return 0;
}

static long read_mem_mb(const char* key) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256]; long v = -1;
    size_t kl = strlen(key);
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, key, kl) == 0) sscanf(line + kl, "%ld", &v);
    fclose(f);
    return v / 1024;
}

static void mem_line(const char* tag) {
    printf("RANDQ mem %-14s rss_mb=%ld peak_mb=%ld vsz_mb=%ld\n", tag,
           read_mem_mb("VmRSS:"), read_mem_mb("VmHWM:"), read_mem_mb("VmSize:"));
    fflush(stdout);
}

static long read_peak_rss_mb(void) {
    return read_mem_mb("VmHWM:");
}

static long long files_mb(const char* dir) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "du -sm '%s' 2>/dev/null | cut -f1", dir);
    FILE* p = popen(cmd, "r");
    if (!p) return -1;
    long long v = -1;
    if (fscanf(p, "%lld", &v) != 1) v = -1;
    pclose(p);
    return v;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "d_rq";
    long rows = argc > 2 ? atol(argv[2]) : 50000;
    g_seed = argc > 3 ? (uint64_t)atoll(argv[3]) : 88172645463325252ull;
    if (!g_seed) g_seed = 1;
    setvbuf(stdout, NULL, _IOLBF, 0);

    nlsql_open_opts_t o;
    memset(&o, 0, sizeof(o));
    o.data_dir = dir; o.workers = 1; o.journal_mb = 16;
    nlsql_conn_t* c = nlsql_open_ex(&o);
    if (!c) { fprintf(stderr, "open fail: %s\n", nlsql_global_error()); return 1; }

    printf("RANDQ cfg rows_per_table=%ld dbs=%d tables_per_db=%d workers=1 seed=%llu\n",
           rows, NDB, NTBL, (unsigned long long)g_seed);

    char sql[512];
    double t_setup = now_ms();
    for (int d = 0; d < NDB; d++) {
        snprintf(sql, sizeof(sql), "CREATE DATABASE db%d", d);
        if (q(c, NULL, sql) != 0) return 1;
    }
    /* 建库与灌数据 */
    long inserted = 0;
    for (int d = 0; d < NDB; d++) {
        snprintf(sql, sizeof(sql), "USE db%d", d);
        if (q(c, NULL, sql) != 0) return 1;
        for (int t = 0; t < NTBL; t++) {
            snprintf(sql, sizeof(sql),
                     "CREATE TABLE t%d (id INTEGER PRIMARY KEY, a INTEGER, b INTEGER, "
                     "c VARCHAR(32), d DOUBLE)", t);
            if (q(c, NULL, sql) != 0) return 1;
            for (long i = 1; i <= rows; i++) {
                snprintf(sql, sizeof(sql),
                         "INSERT INTO t%d (id,a,b,c,d) VALUES (%ld,%ld,%ld,'k%03ld-xxxxxxxx',%ld.%ld)",
                         t, i, (long)(rnd() % 100000), (long)(rnd() % 100000),
                         (long)(rnd() % 512), (long)(rnd() % 1000), (long)(rnd() % 10));
                if (q(c, NULL, sql) != 0) return 1;
                inserted++;
            }
        }
    }
    double t_load = now_ms();
    if (q(c, NULL, "USE db0") != 0) return 1;
    printf("RANDQ load rows_total=%ld load_ms=%.1f rows_per_s=%.0f\n",
           inserted, t_load - t_setup, inserted * 1000.0 / (t_load - t_setup));
    mem_line("after_load");

    bench_t b;
    const int NT = NTBL;

    /* 1) 主键点查（随机表、随机 id） */
    bench_init(&b, "pk_point", 50000);
    for (long i = 0; i < b.cap; i++) {
        int t = (int)rnd_range(0, NT - 1);
        snprintf(sql, sizeof(sql), "SELECT * FROM t%d WHERE id = %ld", t, rnd_range(1, rows));
        if (q(c, &b, sql) != 0) return 1;
    }
    bench_report(&b);

    /* 2) 非索引列等值查（必然全表扫描） */
    bench_init(&b, "scan_eq", 1000);
    for (long i = 0; i < b.cap; i++) {
        int t = (int)rnd_range(0, NT - 1);
        snprintf(sql, sizeof(sql), "SELECT id FROM t%d WHERE a = %ld", t, rnd_range(0, 99999));
        if (q(c, &b, sql) != 0) return 1;
    }
    bench_report(&b);

    /* 3) 主键范围 + 排序 + LIMIT */
    bench_init(&b, "range_order", 2000);
    for (long i = 0; i < b.cap; i++) {
        int t = (int)rnd_range(0, NT - 1);
        long lo = rnd_range(1, rows > 600 ? rows - 600 : 1);
        snprintf(sql, sizeof(sql),
                 "SELECT id,a FROM t%d WHERE id BETWEEN %ld AND %ld ORDER BY id LIMIT 20", t, lo, lo + 500);
        if (q(c, &b, sql) != 0) return 1;
    }
    bench_report(&b);

    /* 4) LIKE 前缀随机命中 */
    bench_init(&b, "like_prefix", 1000);
    for (long i = 0; i < b.cap; i++) {
        int t = (int)rnd_range(0, NT - 1);
        snprintf(sql, sizeof(sql), "SELECT id FROM t%d WHERE c LIKE 'k%03ld-%%' LIMIT 10", t, rnd_range(0, 511));
        if (q(c, &b, sql) != 0) return 1;
    }
    bench_report(&b);

    /* 5) 聚合（随机阈值；引擎为单聚合够用版，故 COUNT 与 SUM 分开测） */
    bench_init(&b, "agg_count", 500);
    for (long i = 0; i < b.cap; i++) {
        int t = (int)rnd_range(0, NT - 1);
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM t%d WHERE a > %ld", t, rnd_range(0, 99999));
        if (q(c, &b, sql) != 0) return 1;
    }
    bench_report(&b);
    bench_init(&b, "agg_sum", 500);
    for (long i = 0; i < b.cap; i++) {
        int t = (int)rnd_range(0, NT - 1);
        snprintf(sql, sizeof(sql), "SELECT SUM(b) FROM t%d WHERE a > %ld", t, rnd_range(0, 99999));
        if (q(c, &b, sql) != 0) return 1;
    }
    bench_report(&b);

    /* 6) GROUP BY + HAVING */
    bench_init(&b, "group_by", 200);
    for (long i = 0; i < b.cap; i++) {
        int t = (int)rnd_range(0, NT - 1);
        snprintf(sql, sizeof(sql), "SELECT a, COUNT(*) FROM t%d GROUP BY a HAVING COUNT(*) >= 1 LIMIT 50", t);
        if (q(c, &b, sql) != 0) return 1;
    }
    bench_report(&b);

    /* 7) 非主键列全量排序 + LIMIT */
    bench_init(&b, "order_by", 200);
    for (long i = 0; i < b.cap; i++) {
        int t = (int)rnd_range(0, NT - 1);
        const char* col = (rnd() & 1) ? "b" : "d";
        snprintf(sql, sizeof(sql), "SELECT id,%s FROM t%d ORDER BY %s DESC LIMIT 20", col, t, col);
        if (q(c, &b, sql) != 0) return 1;
    }
    bench_report(&b);

    /* 8) 多库随机切换 + 点查（库排列） */
    bench_init(&b, "db_switch", 5000);
    {
        int cur = 0;
        for (long i = 0; i < b.cap; i++) {
            int d = (int)rnd_range(0, NDB - 1), t = (int)rnd_range(0, NT - 1);
            double t0 = now_ms();
            if (d != cur) {
                snprintf(sql, sizeof(sql), "USE db%d", d);
                nlsql_result_t* r = NULL;
                if (nlsql_query(c, sql, &r) != NLSQL_OK) {
                    fprintf(stderr, "ERR %s: %s\n", sql, nlsql_global_error()); return 1;
                }
                nlsql_free_result(r);
                cur = d;
            }
            snprintf(sql, sizeof(sql), "SELECT id,a,c FROM t%d WHERE id = %ld", t, rnd_range(1, rows));
            nlsql_result_t* r = NULL;
            int rc = nlsql_query(c, sql, &r);
            if (rc != NLSQL_OK) {
                fprintf(stderr, "ERR rc=%d errno=%d sqlstate=%s [%s] msg=%s\n", rc, nlsql_errno(c),
                        nlsql_sqlstate(c) ? nlsql_sqlstate(c) : "-", sql, nlsql_error(c));
                return 1;
            }
            nlsql_free_result(r);
            double dt = (now_ms() - t0) * 1000.0;
            if (b.n < b.cap) b.lat_us[b.n++] = dt;
        }
    }
    bench_report(&b);

    /* 9) 随机读写混合：点查 + UPDATE + INSERT/DELETE 轮流 */
    bench_init(&b, "mixed_rw", 4000);
    {
        long next_id = 1000000;
        for (long i = 0; i < b.cap; i++) {
            int t = (int)rnd_range(0, NT - 1);
            long id = rnd_range(1, rows);
            double t0 = now_ms();
            if ((i & 3) == 0) {
                snprintf(sql, sizeof(sql), "UPDATE t%d SET a = %ld WHERE id = %ld", t, rnd_range(0, 99999), id);
                nlsql_result_t* r = NULL;
                if (nlsql_query(c, sql, &r) != NLSQL_OK) return 1;
                nlsql_free_result(r);
            } else if ((i & 3) == 1) {
                long nid = next_id++;
                snprintf(sql, sizeof(sql),
                         "INSERT INTO t%d (id,a,b,c,d) VALUES (%ld,%ld,%ld,'k%03ld-x',%ld.0)",
                         t, nid, (long)(rnd() % 100000), (long)(rnd() % 100000), (long)(rnd() % 512), (long)(rnd() % 1000));
                nlsql_result_t* r = NULL;
                if (nlsql_query(c, sql, &r) != NLSQL_OK) return 1;
                nlsql_free_result(r);
            } else {
                snprintf(sql, sizeof(sql), "SELECT id,a,b FROM t%d WHERE id = %ld", t, id);
                nlsql_result_t* r = NULL;
                if (nlsql_query(c, sql, &r) != NLSQL_OK) return 1;
                nlsql_free_result(r);
            }
            if (b.n < b.cap) b.lat_us[b.n++] = (now_ms() - t0) * 1000.0;
        }
    }
    bench_report(&b);

    /* 10) 全表 COUNT（4 库 8 表全扫） */
    bench_init(&b, "count_all", 64);
    for (long i = 0; i < b.cap; i++) {
        int d = (int)(i % NDB), t = (int)((i / NDB) % NT);
        snprintf(sql, sizeof(sql), "USE db%d", d);
        if (q(c, NULL, sql) != 0) return 1;
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM t%d", t);
        if (q(c, &b, sql) != 0) return 1;
    }
    bench_report(&b);

    /* 写满后 flush + 重开验证 */
    double tf = now_ms();
    if (nlsql_flush(c) != NLSQL_OK) return 1;
    printf("RANDQ flush_ms=%.1f\n", now_ms() - tf);
    long rss = read_peak_rss_mb();
    printf("RANDQ peak_rss_mb=%ld data_dir_mb=%lld\n", rss, files_mb(dir));
    nlsql_close(c);

    double tr = now_ms();
    c = nlsql_open_ex(&o);
    if (!c) { fprintf(stderr, "reopen fail\n"); return 1; }
    if (q(c, NULL, "USE db0") != 0) return 1;
    nlsql_result_t* r = NULL;
    if (nlsql_query(c, "SELECT COUNT(*) FROM t0", &r) != NLSQL_OK) return 1;
    long long cnt = r->rows[0].vals[0].i;
    nlsql_free_result(r);
    printf("RANDQ reopen_ms=%.0f count_db0_t0=%lld\n", now_ms() - tr, cnt);
    nlsql_close(c);
    printf("RANDQ OK\n");
    return 0;
}
