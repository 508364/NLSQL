/* 压力测试：1 写（nlsql_submit 100 万行）+ 3 只读（PyMySQL wire）并发
 * 流程：init+预填50万+wire_start → 打印 READY → 写线程开始 submit → sleep 12s
 * → join 写线程 → async_wait → 打印写统计，退出。Python 窗口 ~10s。 */
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

static nlsql_conn_t* g_conn;
static const long TOTAL = 1000000;     /* submit 目标行数 */
static long g_ok = 0, g_busy = 0;
static volatile long g_async_errs = 0;
static volatile int g_write_done = 0;
static double g_write_sec = 0;

static void write_cb(nlsql_conn_t* c, int rc, nlsql_result_t* res, void* ud) {
    (void)res;
    if (rc != 0) {
        long base = (long)(intptr_t)ud;
        if (g_async_errs < 10)
            fprintf(stderr, "[ASYNC_ERR] rc=%d base=%ld err=%s state=%s\n",
                    rc, base, nlsql_error(c), nlsql_sqlstate(c));
        g_async_errs++;
    }
}

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int insert_batch(nlsql_conn_t* c, long base, int nrows) {
    static char sql[16384];
    int off = snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ");
    for (int i = 0; i < nrows && off < 15000; i++)
        off += snprintf(sql + off, sizeof(sql) - (size_t)off, "%s(%ld,'val-%ld',%ld.%d)",
                        i ? "," : "", base + i, base + i, (base + i) / 100, i % 100);
    nlsql_result_t* r = NULL;
    int rc = nlsql_query(c, sql, &r);
    if (r) nlsql_free_result(r);
    return rc;
}

static void* writer(void* arg) {
    (void)arg;
    char sql[32768];
    long sent = 0;
    double t0 = now_s();
    while (sent < TOTAL) {
        int off = snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ");
        long n = TOTAL - sent;
        if (n > 500) n = 500;
        long base = sent + 500000;   /* 偏移避开预填 0..499999 */
        long actual = 0;
        for (long i = 0; i < n && off < 30000; i++) {
            off += snprintf(sql + off, sizeof(sql) - (size_t)off, "%s(%ld,'val-%ld',%ld.%d)",
                            i ? "," : "", base + i, base + i, (base + i) / 100, (int)(i % 100));
            actual++;
        }
        int rc = nlsql_query_async(g_conn, sql, write_cb, (void*)(intptr_t)sent);
        if (rc == 0) { sent += actual; g_ok += actual; }
        else if (rc == NLSQL_ERR_BUSY) { g_busy++; }     /* 队列满，立即重试 */
        else { fprintf(stderr, "submit fatal rc=%d err=%s\n", rc, nlsql_error(g_conn)); break; }
    }
    g_write_sec = now_s() - t0;
    nlsql_async_wait(g_conn);
    g_write_done = 1;
    return NULL;
}

int main(void) {
    setbuf(stdout, NULL); setbuf(stderr, NULL);
    system("rm -rf /workspace/nlsql/testenv/data_st && mkdir -p /workspace/nlsql/testenv/data_st");
    g_conn = nlsql_open("/workspace/nlsql/testenv/data_st");
    if (!g_conn) { printf("open fail: %s\n", nlsql_global_error()); return 1; }
    nlsql_result_t* r = NULL;
    nlsql_query(g_conn, "CREATE DATABASE sdb", &r); if (r) { nlsql_free_result(r); r = NULL; }
    int rc = nlsql_query(g_conn, "USE sdb", &r); if (r) { nlsql_free_result(r); r = NULL; }
    rc = nlsql_query(g_conn, "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(30), s DOUBLE)", &r);
    if (r) { nlsql_free_result(r); r = NULL; }
    if (rc) { printf("create rc=%d %s\n", rc, nlsql_error(g_conn)); return 1; }
    /* 预填 50 万行 */
    for (long b = 0; b < 1000; b++) {
        if (insert_batch(g_conn, b * 500, 500) != 0) {
            printf("prefill b=%ld rc=%d err=%s\n", b, rc, nlsql_error(g_conn)); return 1;
        }
    }
    /* 分段打点：预填落地数 */
    nlsql_result_t* pc = NULL;
    nlsql_query(g_conn, "SELECT COUNT(*) FROM t", &pc);
    printf("PREFILL_COUNT=%lld\n", pc && pc->row_count ? (long long)pc->rows[0].vals[0].i : -1);
    if (pc) nlsql_free_result(pc);
    int h = nlsql_wire_start(g_conn, 13319, 8);
    printf("wire=%d\n", h); fflush(stdout);
    if (h < 0) return 2;
    pthread_t th;
    pthread_create(&th, NULL, writer, NULL);
    printf("READY\n"); fflush(stdout);
    pthread_join(th, NULL);
    fprintf(stderr, "[P3] writer joined\n");
    /* flush 前打点：行是否已在内存 */
    nlsql_result_t* ac = NULL;
    nlsql_query(g_conn, "SELECT COUNT(*) FROM t", &ac);
    fprintf(stderr, "[P3.5] count_before_flush=%lld\n",
            ac && ac->row_count ? (long long)ac->rows[0].vals[0].i : -1LL);
    if (ac) nlsql_free_result(ac);
    nlsql_flush(g_conn);
    fprintf(stderr, "[P4] flushed\n");
    /* 写统计 */
    nlsql_result_t* cr = NULL;
    rc = nlsql_query(g_conn, "SELECT COUNT(*) FROM t", &cr);
    long total = (rc == 0 && cr && cr->row_count) ? (long)cr->rows[0].vals[0].i : -1;
    if (cr) nlsql_free_result(cr);
    fprintf(stderr, "[P5] count1=%ld\n", total);
    sleep(1);
    rc = nlsql_query(g_conn, "SELECT COUNT(*) FROM t", &cr);
    long total2 = (rc == 0 && cr && cr->row_count) ? (long)cr->rows[0].vals[0].i : -1;
    if (cr) nlsql_free_result(cr);
    fprintf(stderr, "[P6] count2=%ld (after 1s)\n", total2);
    printf("WRITE_OK=%ld BUSY=%ld ASYNC_ERRS=%ld sec=%.2f cps=%.0f TOTAL_ROWS=%ld\n",
           g_ok, g_busy, g_async_errs, g_write_sec, g_ok / (g_write_sec > 0 ? g_write_sec : 1), total);
    fflush(stdout);
    /* 保活窗口：wire 全程在线，读者 10s 窗口不被 close 提前断开，稳态 QPS 口径纯净 */
    fprintf(stderr, "[P6.5] hold wire alive 12s for readers\n");
    sleep(12);
#ifdef EXPLICIT_STOP
    nlsql_wire_stop(h);
    fprintf(stderr, "[P7] wire stopped explicitly\n");
#else
    fprintf(stderr, "[P7] going to close with wire active (auto-stop path)\n");
#endif
    nlsql_close(g_conn);
    fprintf(stderr, "[P8] closed\n");
    return total2 == 1500000 ? 0 : 3;
}
