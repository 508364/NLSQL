/* 并发查询上限压测的服务端
 * 起一个 wire 服务器（127.0.0.1），预填单表，供多客户端同时连接做点查；
 * 周期性打印自身线程数与 RSS，用于观察 1C2G 下的并发连接开销。
 * 用法：./conc_srv <dir> <port> [rows] [max_clients] [seconds]
 */
#define _POSIX_C_SOURCE 200809L
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
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

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "d_conc";
    int port = argc > 2 ? atoi(argv[2]) : 13351;
    long rows = argc > 3 ? atol(argv[3]) : 50000;
    int maxc = argc > 4 ? atoi(argv[4]) : 64;
    int secs = argc > 5 ? atoi(argv[5]) : 60;
    setvbuf(stdout, NULL, _IOLBF, 0);

    nlsql_conn_t* c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "CONC open fail: %s\n", nlsql_global_error()); return 1; }

    nlsql_result_t* r = NULL;
    char sql[16384];
    if (nlsql_query(c, "CREATE DATABASE cdb", &r) != NLSQL_OK) {
        fprintf(stderr, "CONC create db: %s\n", nlsql_error(c)); return 1;
    }
    if (r) { nlsql_free_result(r); r = NULL; }
    if (nlsql_query(c, "USE cdb", &r) != NLSQL_OK) { fprintf(stderr, "CONC use: %s\n", nlsql_error(c)); return 1; }
    if (r) { nlsql_free_result(r); r = NULL; }
    if (nlsql_query(c, "CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(32))", &r) != NLSQL_OK) {
        fprintf(stderr, "CONC create tbl: %s\n", nlsql_error(c)); return 1;
    }
    if (r) { nlsql_free_result(r); r = NULL; }

    long done = 0;
    while (done < rows) {
        long n = rows - done; if (n > 500) n = 500;
        int off = snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ");
        for (long i = 0; i < n; i++)
            off += snprintf(sql + off, sizeof(sql) - (size_t)off, "%s(%ld,'v%ld-%ld')",
                            i ? "," : "", done + i + 1, done + i, i);
        if (nlsql_query(c, sql, &r) != NLSQL_OK) { fprintf(stderr, "CONC insert: %s\n", nlsql_error(c)); return 1; }
        if (r) { nlsql_free_result(r); r = NULL; }
        done += n;
    }
    if (nlsql_flush(c) != NLSQL_OK) return 1;

    int h = nlsql_wire_start(c, port, maxc);
    if (h <= 0) { fprintf(stderr, "CONC wire start: %s\n", nlsql_global_error()); return 2; }
    printf("CONC READY port=%d rows=%ld max_clients=%d secs=%d\n", port, rows, maxc, secs);

    double t0 = now_s();
    for (;;) {
        sleep(1);
        double el = now_s() - t0;
        printf("CONC stat t=%.0fs threads=%ld rss_mb=%ld\n", el,
               stat_val("Threads:"), stat_val("VmRSS:") / 1024);
        if (el >= secs) break;
    }
    nlsql_wire_stop(h);
    nlsql_close(c);
    printf("CONC DONE\n");
    return 0;
}
