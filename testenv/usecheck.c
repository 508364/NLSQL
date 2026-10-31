/* 最小复现：多库交替 USE + 点查，观察虚拟内存/常驻内存是否随切库暴涨 */
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <string.h>

static long memv(const char* key) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char l[256]; long v = -1; size_t kl = strlen(key);
    while (fgets(l, sizeof(l), f)) if (!strncmp(l, key, kl)) sscanf(l + kl, "%ld", &v);
    fclose(f);
    return v / 1024;
}

static int q(nlsql_conn_t* c, const char* s) {
    nlsql_result_t* r = NULL;
    int rc = nlsql_query(c, s, &r);
    if (rc != NLSQL_OK) { printf("ERR rc=%d [%s] %s\n", rc, s, nlsql_global_error()); return -1; }
    nlsql_free_result(r);
    return 0;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "d_use";
    int rounds = argc > 2 ? atoi(argv[2]) : 40;
    int nrows  = argc > 3 ? atoi(argv[3]) : 200;
    nlsql_open_opts_t o;
    memset(&o, 0, sizeof(o));
    o.data_dir = dir; o.workers = 1; o.journal_mb = 16;
    nlsql_conn_t* c = nlsql_open_ex(&o);
    if (!c) { printf("open fail %s\n", nlsql_global_error()); return 1; }
    char sql[256];
    for (int d = 0; d < 4; d++) {
        snprintf(sql, sizeof(sql), "CREATE DATABASE db%d", d); if (q(c, sql)) return 1;
        snprintf(sql, sizeof(sql), "USE db%d", d); if (q(c, sql)) return 1;
        if (q(c, "CREATE TABLE t (id INTEGER PRIMARY KEY, a INTEGER, c VARCHAR(32))")) return 1;
        for (int i = 1; i <= nrows; i++) {
            snprintf(sql, sizeof(sql), "INSERT INTO t (id,a,c) VALUES (%d,%d,'k%03d-x')", i, i * 7, i % 100);
            if (q(c, sql)) return 1;
        }
    }
    printf("SETUP rss=%ldMB vsz=%ldMB\n", memv("VmRSS:"), memv("VmSize:"));
    int cur = 3;
    for (int i = 0; i < rounds; i++) {
        int d = i % 4;
        if (d != cur) {
            snprintf(sql, sizeof(sql), "USE db%d", d);
            if (q(c, sql)) return 1;
            cur = d;
        }
        snprintf(sql, sizeof(sql), "SELECT id,a,c FROM t WHERE id = %d", (i * 13) % nrows + 1);
        if (q(c, sql)) return 1;
        if (i % 5 == 4)
            printf("R%d after USE db%d rss=%ldMB vsz=%ldMB\n", i + 1, d, memv("VmRSS:"), memv("VmSize:"));
    }
    printf("DONE\n");
    nlsql_close(c);
    return 0;
}
