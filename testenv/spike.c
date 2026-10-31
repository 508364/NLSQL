/* 压实尖峰：100 万 / 300 万行表 nlsql_flush()（压实+落盘）耗时 */
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int insert_rows(nlsql_conn_t* db, long from, long to) {
    nlsql_result_t* r = NULL;
    long i = from;
    long next_report = ((from + 499999) / 500000) * 500000;
    while (i < to) {
        char sql[16384] = "INSERT INTO t VALUES ";
        int off = (int)strlen(sql);
        for (long k = 0; i < to && off < 15000; i++, k++) {
            off += snprintf(sql + off, sizeof(sql) - (size_t)off, "%s(%ld,'val-%ld',%ld.%d)",
                            k ? "," : "", i, i, i / 100, (int)(i % 100));
        }
        if (nlsql_query(db, sql, &r) != NLSQL_OK) {
            printf("insert fail @%ld: %s\n", i, nlsql_error(db));
            if (r) nlsql_free_result(r);
            return -1;
        }
        if (r) { nlsql_free_result(r); r = NULL; }
        if (i >= next_report) { printf("rows=%ld\n", i); next_report += 500000; }
    }
    return 0;
}

int main(void) {
    setbuf(stdout, NULL);
    system("rm -rf /workspace/nlsql/testenv/data_spike && mkdir -p /workspace/nlsql/testenv/data_spike");
    nlsql_conn_t* db = nlsql_open("/workspace/nlsql/testenv/data_spike");
    if (!db) { printf("open fail: %s\n", nlsql_global_error()); return 1; }
    nlsql_result_t* r = NULL;
    nlsql_query(db, "CREATE DATABASE b", &r); if (r) { nlsql_free_result(r); r = NULL; }
    nlsql_query(db, "USE b", &r); if (r) { nlsql_free_result(r); r = NULL; }
    nlsql_query(db, "CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(30), s DOUBLE)", &r);
    if (r) { nlsql_free_result(r); r = NULL; }

    double t0, t1;
    if (insert_rows(db, 0, 1000000)) return 1;
    t0 = now_ms();
    int rc = nlsql_flush(db);
    t1 = now_ms();
    printf("FLUSH@1,000,000 rows: %.1f ms rc=%d\n", t1 - t0, rc);

    if (insert_rows(db, 1000000, 3000000)) return 1;
    t0 = now_ms();
    rc = nlsql_flush(db);
    t1 = now_ms();
    printf("FLUSH@3,000,000 rows: %.1f ms rc=%d\n", t1 - t0, rc);

    nlsql_close(db);
    return 0;
}
