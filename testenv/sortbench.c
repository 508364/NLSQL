/* 排序（重排）基准：千级 / 万级 / 百万级行数下的 ORDER BY 全链路耗时
 * 场景：主键排序（已有序输入）、整数随机列、字符串列、浮点列、多列混合、
 *       分组（GROUP BY）、去重（DISTINCT）、无排序扫描（对照）。
 * 约束：外部 taskset -c 0 + ulimit -v 2000000（1 核 2 GB）。
 * 用法：./sortbench <dir> <rows> [seed] [reps]
 */
#define _POSIX_C_SOURCE 200809L
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long g_rows = 1000;
static uint64_t g_seed = 88172645463325252ull;

static uint64_t rnd(void) {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17;
    return g_seed;
}

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
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
    printf("SORT mem %-18s rss_mb=%ld peak_mb=%ld vsz_mb=%ld\n", tag,
           read_mem_mb("VmRSS:"), read_mem_mb("VmHWM:"), read_mem_mb("VmSize:"));
    fflush(stdout);
}

static int q(nlsql_conn_t* c, const char* sql) {
    nlsql_result_t* r = NULL;
    int rc = nlsql_query(c, sql, &r);
    if (rc != NLSQL_OK) {
        fprintf(stderr, "SORT ERR [%s] %s\n", sql, nlsql_error(c));
        return -1;
    }
    nlsql_free_result(r);
    return 0;
}

typedef struct {
    const char* name;
    const char* sql;
    long long   expect;     /* <0 表示不校验行数 */
    int         verify_col; /* >=0: 校验该结果列非降序；-1 不校验 */
} scen_t;

/* 运行一个场景 reps 次，输出 avg/min/max 毫秒 */
static int run_scen(nlsql_conn_t* c, const scen_t* s, int reps, int verify_col) {
    double mn = 1e18, mx = -1, sum = 0;
    long long got = -1;
    for (int i = 0; i < reps; i++) {
        nlsql_result_t* r = NULL;
        double t0 = now_ms();
        int rc = nlsql_query(c, s->sql, &r);
        double dt = now_ms() - t0;
        if (rc != NLSQL_OK) {
            fprintf(stderr, "SORT ERR [%s] %s\n", s->sql, nlsql_error(c));
            return -1;
        }
        got = (long long)r->row_count;
        if (verify_col >= 0 && r->row_count > 1) {
            /* 排序列非降序校验 */
            const nlsql_value_t* p = &r->rows[0].vals[verify_col];
            if (p->is_null) p = NULL;
            for (uint64_t k = 1; k < r->row_count; k++) {
                const nlsql_value_t* v = &r->rows[k].vals[verify_col];
                if (p && !v->is_null && v->type == NLSQL_TYPE_INT64 && p->type == NLSQL_TYPE_INT64 &&
                    v->i < p->i) {
                    fprintf(stderr, "SORT ORDER WRONG at %llu: %lld < %lld\n",
                            (unsigned long long)k, (long long)v->i, (long long)p->i);
                    nlsql_free_result(r);
                    return -1;
                }
                p = v->is_null ? NULL : v;
            }
        }
        nlsql_free_result(r);
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
        sum += dt;
    }
    if (s->expect >= 0 && got != s->expect) {
        fprintf(stderr, "SORT ROWS MISMATCH %s got=%lld expect=%lld\n", s->name, got, s->expect);
        return -1;
    }
    printf("SORT %-18s reps=%d avg_ms=%-9.2f min_ms=%-9.2f max_ms=%-9.2f rows=%-9lld peak_mb=%ld\n",
           s->name, reps, sum / reps, mn, mx, got, read_mem_mb("VmHWM:"));
    fflush(stdout);
    return 0;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "d_sort";
    if (argc > 2) g_rows = atol(argv[2]);
    if (argc > 3) g_seed = strtoull(argv[3], NULL, 10);
    int reps = argc > 4 ? atoi(argv[4]) : (g_rows <= 10000 ? 5 : 3);
    if (!g_seed) g_seed = 1;
    setvbuf(stdout, NULL, _IOLBF, 0);

    nlsql_open_opts_t o;
    memset(&o, 0, sizeof(o));
    o.data_dir = dir; o.workers = 1; o.journal_mb = 256; /* 大库导入期不反复压实 */
    nlsql_conn_t* c = nlsql_open_ex(&o);
    if (!c) { fprintf(stderr, "SORT open: %s\n", nlsql_global_error()); return 1; }

    char sql[512];
    if (q(c, "CREATE DATABASE sdb") != 0) return 1;
    if (q(c, "USE sdb") != 0) return 1;
    if (q(c, "CREATE TABLE t0 (id INTEGER PRIMARY KEY, a INTEGER, b INTEGER, "
                "c VARCHAR(32), d DOUBLE)") != 0) return 1;

    double t0 = now_ms();
    for (long i = 1; i <= g_rows; i++) {
        uint64_t ra = rnd(), rb = rnd(), rc = rnd(), rd = rnd();
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t0 (id,a,b,c,d) VALUES (%ld,%ld,%ld,'k%03ld-xxxxxxxx',%ld.%ld)",
                 i, (long)(ra % 100000), (long)(rb % 100000),
                 (long)(rc % 512), (long)(rd % 1000), (long)(rd % 10));
        if (q(c, sql) != 0) return 1;
    }
    double load_ms = now_ms() - t0;
    printf("SORT cfg rows=%ld reps=%d seed=%llu\n", g_rows, reps, (unsigned long long)g_seed);
    printf("SORT load rows=%ld load_ms=%.1f rows_per_s=%.0f\n", g_rows, load_ms, g_rows * 1000.0 / load_ms);
    mem_line("after_load");

    scen_t sc[] = {
        {"sort_pk_full",       "SELECT id,a FROM t0 ORDER BY id", g_rows, 0},
        {"sort_int_full",      "SELECT id,a FROM t0 ORDER BY a", g_rows, 1},
        {"sort_int_limit100",  "SELECT id,a FROM t0 ORDER BY a LIMIT 100", 100, 1},
        {"sort_str_full",      "SELECT id,c FROM t0 ORDER BY c", g_rows, -1},
        {"sort_double_full",   "SELECT id,d FROM t0 ORDER BY d", g_rows, -1},
        {"sort_multi_lim1k",   "SELECT id,a,c FROM t0 ORDER BY a DESC, c LIMIT 1000", 1000, -1},
        {"group_by_a_lim50",   "SELECT a, COUNT(*) FROM t0 GROUP BY a LIMIT 50", 50, -1},
        {"distinct_c",         "SELECT DISTINCT c FROM t0", -1, -1},
        {"scan_id_nosort",     "SELECT id FROM t0", g_rows, -1},
    };
    for (size_t i = 0; i < sizeof(sc) / sizeof(sc[0]); i++) {
        if (run_scen(c, &sc[i], reps, sc[i].verify_col) != 0) return 1;
        mem_line(sc[i].name);
    }

    double tf = now_ms();
    if (nlsql_flush(c) != NLSQL_OK) return 1;
    printf("SORT flush_ms=%.1f peak_rss_mb=%ld\n", now_ms() - tf, read_mem_mb("VmHWM:"));
    nlsql_close(c);

    double tr = now_ms();
    c = nlsql_open_ex(&o);
    if (!c) { fprintf(stderr, "SORT reopen fail\n"); return 1; }
    if (q(c, "USE sdb") != 0) return 1;
    nlsql_result_t* r = NULL;
    if (nlsql_query(c, "SELECT COUNT(*) FROM t0", &r) != NLSQL_OK) return 1;
    long long cnt = r->rows[0].vals[0].i;
    nlsql_free_result(r);
    printf("SORT reopen_ms=%.0f count=%lld %s\n", now_ms() - tr, cnt, cnt == g_rows ? "OK" : "MISMATCH");
    nlsql_close(c);
    printf("SORT DONE\n");
    return cnt == g_rows ? 0 : 1;
}
