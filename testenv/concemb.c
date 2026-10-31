/* 嵌入式同连接并发只读上限测试
 * 一个 nlsql_conn_t 上开 N 个线程同时点查，逐步加线程看总 QPS / 延迟 / 错误 / 线程栈占用。
 * 用法：./concemb <dir> [rows] [seconds_per_level]
 */
#define _POSIX_C_SOURCE 200809L
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

static long g_rows = 50000;

static double now_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

static long stat_val(const char* key) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256]; long v = -1;
    size_t kl = strlen(key);
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, key, kl) == 0) sscanf(line + kl, "%ld", &v);
    fclose(f);
    return v;
}

static int q(nlsql_conn_t* c, const char* sql) {
    nlsql_result_t* r = NULL;
    if (nlsql_query(c, sql, &r) != NLSQL_OK) return -1;
    nlsql_free_result(r);
    return 0;
}

typedef struct {
    nlsql_conn_t* c;
    int      dur_ms;
    uint64_t seed;
    long     n;
    double   sum_us;
    int      err;
} arg_t;

static void* work(void* p) {
    arg_t* a = (arg_t*)p;
    char sql[192];
    uint64_t s = a->seed ? a->seed : 1;
    double deadline = now_us() + (double)a->dur_ms * 1000.0;
    long n = 0; double sum = 0;
    while (now_us() < deadline) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        long id = 1 + (long)(s % (uint64_t)g_rows);
        snprintf(sql, sizeof(sql), "SELECT id,a,c FROM t0 WHERE id = %ld", id);
        double t0 = now_us();
        nlsql_result_t* r = NULL;
        if (nlsql_query(a->c, sql, &r) != NLSQL_OK) { a->err = 1; break; }
        nlsql_free_result(r);
        sum += now_us() - t0;
        n++;
    }
    a->n = n; a->sum_us = sum;
    return NULL;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "d_cemb";
    if (argc > 2) g_rows = atol(argv[2]);
    int dur_ms = argc > 3 ? atoi(argv[3]) * 1000 : 2000;
    setvbuf(stdout, NULL, _IOLBF, 0);

    nlsql_conn_t* c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "CEMB open: %s\n", nlsql_global_error()); return 1; }

    char sql[512];
    if (q(c, "CREATE DATABASE emb") != 0) return 1;
    if (q(c, "USE emb") != 0) return 1;
    if (q(c, "CREATE TABLE t0 (id INTEGER PRIMARY KEY, a INTEGER, b INTEGER, c VARCHAR(32), d DOUBLE)") != 0) return 1;
    uint64_t s = 88172645463325252ull;
    for (long i = 1; i <= g_rows; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        long a1 = (long)(s % 100000);
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        long b1 = (long)(s % 100000);
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t0 (id,a,b,c,d) VALUES (%ld,%ld,%ld,'k%03ld-x',1.5)",
                 i, a1, b1, i % 512);
        if (q(c, sql) != 0) return 1;
    }
    if (nlsql_flush(c) != NLSQL_OK) return 1;
    printf("CEMB cfg rows=%ld dur_per_level=%ds\n", g_rows, dur_ms / 1000);
    printf("CEMB mem after_load threads=%ld vmsize_mb=%ld rss_mb=%ld\n",
           stat_val("Threads:"), stat_val("VmSize:") / 1024, stat_val("VmRSS:") / 1024);

    int levels[] = {1, 2, 4, 8, 16, 32, 64};
    for (size_t li = 0; li < sizeof(levels) / sizeof(levels[0]); li++) {
        int nt = levels[li];
        arg_t* a = (arg_t*)calloc((size_t)nt, sizeof(arg_t));
        pthread_t* th = (pthread_t*)calloc((size_t)nt, sizeof(pthread_t));
        if (!a || !th) return 1;
        double w0 = now_us();
        for (int i = 0; i < nt; i++) {
            a[i].c = c; a[i].dur_ms = dur_ms; a[i].seed = s + (uint64_t)i * 7919u + 1;
            pthread_create(&th[i], NULL, work, &a[i]);
        }
        for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
        double wall = now_us() - w0;
        long long n = 0; double sum = 0; int err = 0;
        for (int i = 0; i < nt; i++) { n += a[i].n; sum += a[i].sum_us; err |= a[i].err; }
        printf("CEMB thr=%-3d ok=%d n=%-9lld wall_ms=%-7.1f qps=%-8.0f avg_us=%-7.2f per_thr_qps=%-8.0f threads=%ld vmsize_mb=%ld rss_mb=%ld\n",
               nt, err ? -1 : nt, n, wall / 1000.0, (double)n * 1e6 / wall,
               n ? sum / (double)n : 0, (double)n / nt * 1e6 / wall,
               stat_val("Threads:"), stat_val("VmSize:") / 1024, stat_val("VmRSS:") / 1024);
        free(a); free(th);
    }
    nlsql_close(c);
    printf("CEMB DONE\n");
    return 0;
}
