/* wire 探针：起 wire_srv + 预填 20 万行，供 Python 客户端计时/压测，40s 窗口 */
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    setbuf(stdout, NULL); setbuf(stderr, NULL);
    system("rm -rf /workspace/nlsql/testenv/data_wp && mkdir -p /workspace/nlsql/testenv/data_wp");
    nlsql_conn_t* c = nlsql_open("/workspace/nlsql/testenv/data_wp");
    if (!c) { printf("open fail: %s\n", nlsql_global_error()); return 1; }
    nlsql_result_t* r = NULL;
    int rc = nlsql_query(c, "CREATE DATABASE wdb", &r);
    if (r) { nlsql_free_result(r); r = NULL; }
    rc = nlsql_query(c, "USE wdb", &r);
    printf("USE rc=%d err=%s\n", rc, nlsql_error(c));
    if (r) { nlsql_free_result(r); r = NULL; }
    rc = nlsql_query(c, "CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(30))", &r);
    if (r) { nlsql_free_result(r); r = NULL; }
    if (rc) { printf("CREATE rc=%d err=%s\n", rc, nlsql_error(c)); return 1; }
    for (int b = 0; b < 400; b++) {
        char sql[16384] = "INSERT INTO t VALUES ";
        int off = (int)strlen(sql);
        long actual = 0;
        for (int i = 0; i < 500 && off < 15000; i++) {
            off += snprintf(sql + off, sizeof(sql) - (size_t)off, "%s(%d,'v%d-%d')",
                            i ? "," : "", b * 500 + i, b, i);
            actual++;
        }
        rc = nlsql_query(c, sql, &r);
        if (r) { nlsql_free_result(r); r = NULL; }
        if (rc) { printf("INS b=%d rc=%d err=%s\n", b, rc, nlsql_error(c)); return 1; }
    }
    nlsql_result_t* cr = NULL;
    nlsql_query(c, "SELECT COUNT(*) FROM t", &cr);
    printf("PREFILL=%lld\n", cr && cr->row_count ? (long long)cr->rows[0].vals[0].i : -1LL);
    if (cr) nlsql_free_result(cr);
    int h = nlsql_wire_start(c, 13317, 8);
    printf("wire=%d err=%s global=%s\n", h, nlsql_error(c), nlsql_global_error());
    if (h < 0) return 2;
    fflush(stdout);
    sleep(90); /* 基准窗口 */
    return 0;
}
