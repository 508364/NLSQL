/* bigrow：大行（64KB~2MB 级 TEXT）顺序读写基准——暴露大行读写的吞吐、内存、停顿与写放大瓶颈
 * 用法：bigrow <datadir> <rowsize_kb> <nrows> [journal_mb]   （journal_mb 默认 64）
 * 输出均以 "BIGROW " 前缀的 key=value 行，便于机器解析。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netleaf_nlsql.h>

static char* g_sql;      /* >= 4MB + 4KB */
static char* g_payload;
static size_t g_plen;

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static long vm_kb(void) {
    FILE* f = fopen("/proc/self/status", "r"); char l[256]; long v = 0;
    while (f && fgets(l, sizeof l, f))
        if (!strncmp(l, "VmRSS:", 6)) { v = strtol(l + 6, NULL, 10); break; }
    if (f) fclose(f); return v;
}
/* /proc/self/io：wchar=write 系统调用累计字节（可算写放大），write_bytes=实际落盘 */
static long io_long(const char* key) {
    FILE* f = fopen("/proc/self/io", "r"); char l[256]; long v = -1; size_t k = strlen(key);
    while (f && fgets(l, sizeof l, f))
        if (!strncmp(l, key, k)) { v = strtol(l + k + 1, NULL, 10); break; }
    if (f) fclose(f); return v;
}
static long fsize(const char* p) {
    FILE* f = fopen(p, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n;
}
static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

int main(int argc, char** argv) {
    setbuf(stdout, NULL);
    if (argc < 4) { fprintf(stderr, "usage: bigrow <datadir> <rowsize_kb> <nrows> [journal_mb]\n"); return 2; }
    const char* dir = argv[1];
    long rowsize_kb = atol(argv[2]);
    long nrows = atol(argv[3]);
    int jmb = argc > 4 ? atoi(argv[4]) : 64;
    g_plen = (size_t)rowsize_kb * 1024;
    if (rowsize_kb <= 0 || nrows <= 0) { fprintf(stderr, "bad args\n"); return 2; }

    /* 载荷：仅 [a-z0-9]（免转义、长度不膨胀），首部嵌 id 便于辨识 */
    g_payload = malloc(g_plen);
    if (!g_payload) { fprintf(stderr, "payload malloc fail\n"); return 1; }
    for (size_t i = 0; i < g_plen; i++) g_payload[i] = (char)('a' + (i * 31 % 26));
    g_sql = malloc(4u * 1024 * 1024 + 8192);
    if (!g_sql) { fprintf(stderr, "sql malloc fail\n"); return 1; }

    printf("BIGROW CFG rowsize_kb=%ld nrows=%ld journal_mb=%d rss0=%ldMB\n",
           rowsize_kb, nrows, jmb, vm_kb() / 1024);
    long w0 = io_long("wchar");

    nlsql_open_opts_t o; memset(&o, 0, sizeof o);
    o.data_dir = dir; o.journal_mb = jmb;
    double t = now_ms();
    nlsql_conn_t* c = nlsql_open_ex(&o);
    if (!c) { fprintf(stderr, "open fail: %s\n", nlsql_global_error()); return 1; }
    printf("BIGROW OPEN ms=%.1f rss=%ldMB\n", now_ms() - t, vm_kb() / 1024);

    nlsql_result_t* r = NULL;
    if (nlsql_query(c, "CREATE DATABASE bdb", NULL) != NLSQL_OK &&
        nlsql_errno(c) != 1007) { fprintf(stderr, "create db: %s\n", nlsql_error(c)); return 1; }
    nlsql_query(c, "USE bdb", NULL);
    if (nlsql_query(c, "CREATE TABLE t (id INT PRIMARY KEY, v TEXT, s DOUBLE)", NULL) != NLSQL_OK) {
        fprintf(stderr, "create table: %s\n", nlsql_error(c)); return 1;
    }

    /* ---- INSERT ---- */
    double* lat = malloc((size_t)nrows * sizeof(double));
    double t_ins = now_ms();
    long rss_max = vm_kb();
    for (long id = 0; id < nrows; id++) {
        int h = snprintf(g_sql, 64, "INSERT INTO t VALUES (%ld, '", id);
        memcpy(g_sql + h, g_payload, g_plen);
        int off = h + (int)g_plen;
        off += snprintf(g_sql + off, 64, "', %.3f)", (double)id * 0.5);
        double t0 = now_ms();
        if (nlsql_query(c, g_sql, NULL) != NLSQL_OK) {
            fprintf(stderr, "insert id=%ld fail: %s\n", id, nlsql_error(c)); return 1;
        }
        lat[id] = now_ms() - t0;
        if (id % 100 == 0) { long m = vm_kb(); if (m > rss_max) rss_max = m; }
        (void)off;
    }
    double d_ins = now_ms() - t_ins;
    qsort(lat, (size_t)nrows, sizeof(double), cmp_d);
    printf("BIGROW INSERT total=%.3fs rows_per_s=%.0f MB_per_s=%.1f p50=%.2fms p99=%.2fms max=%.2fms rss_max=%ldMB\n",
           d_ins / 1000.0, nrows / (d_ins / 1000.0),
           (double)nrows * g_plen / 1048576.0 / (d_ins / 1000.0),
           lat[nrows / 2], lat[(long)((double)nrows * 0.99) < nrows ? (long)((double)nrows * 0.99) : nrows - 1],
           lat[nrows - 1], rss_max / 1024);
    free(lat);

    /* ---- 显式 flush（全表压实尖峰） ---- */
    t = now_ms();
    if (nlsql_flush(c) != NLSQL_OK) fprintf(stderr, "flush: %s\n", nlsql_error(c));
    printf("BIGROW FLUSH ms=%.1f rss=%ldMB\n", now_ms() - t, vm_kb() / 1024);

    /* ---- COUNT ---- */
    t = now_ms();
    if (nlsql_query(c, "SELECT COUNT(*) FROM t", &r) != NLSQL_OK) { fprintf(stderr, "count: %s\n", nlsql_error(c)); return 1; }
    long cnt = (long)r->rows[0].vals[0].i; nlsql_free_result(r);
    printf("BIGROW COUNT ms=%.1f rows=%ld\n", now_ms() - t, cnt);

    /* ---- 全量读 SELECT v ---- */
    t = now_ms(); long rss_a = vm_kb();
    if (nlsql_query(c, "SELECT v FROM t", &r) != NLSQL_OK) { fprintf(stderr, "read: %s\n", nlsql_error(c)); return 1; }
    double d_rd = now_ms() - t;
    long rss_b = vm_kb();
    int oklen = r->row_count == (uint64_t)nrows &&
                r->rows[r->row_count - 1].vals[0].len == g_plen;
    printf("BIGROW READ_FULL ms=%.1f rows=%lu MB_per_s=%.1f rss_before=%ldMB rss_peak=%ldMB delta=%ldMB verify=%s\n",
           d_rd, (unsigned long)r->row_count,
           (double)nrows * g_plen / 1048576.0 / (d_rd / 1000.0),
           rss_a / 1024, rss_b / 1024, (rss_b - rss_a) / 1024, oklen ? "OK" : "FAIL");
    nlsql_free_result(r);
    printf("BIGROW READ_FULL rss_after_free=%ldMB\n", vm_kb() / 1024);

    /* ---- 轻列读（id,s）对照 ---- */
    t = now_ms();
    if (nlsql_query(c, "SELECT id, s FROM t", &r) != NLSQL_OK) { fprintf(stderr, "read light: %s\n", nlsql_error(c)); return 1; }
    double d_lt = now_ms() - t;
    printf("BIGROW READ_LIGHT ms=%.1f rows=%lu rows_per_s=%.0f\n",
           d_lt, (unsigned long)r->row_count, r->row_count / (d_lt / 1000.0));
    nlsql_free_result(r);

    /* ---- 主键点查（每次返回 rowsize_kb 的值） ---- */
    long npq = nrows < 100 ? nrows : 100;
    t = now_ms(); long pq_ok = 0;
    for (long i = 0; i < npq; i++) {
        long id = i * nrows / npq;
        snprintf(g_sql, 96, "SELECT v FROM t WHERE id=%ld", id);
        if (nlsql_query(c, g_sql, &r) == NLSQL_OK && r->row_count == 1) {
            if (r->rows[0].vals[0].len == g_plen) pq_ok++;
            nlsql_free_result(r);
        }
    }
    double d_pq = now_ms() - t;
    printf("BIGROW POINT n=%ld total=%.1f avg_us=%.0f MB_per_s=%.1f ok=%ld\n",
           npq, d_pq, d_pq * 1000.0 / npq,
           (double)npq * g_plen / 1048576.0 / (d_pq / 1000.0), pq_ok);

    /* ---- UPDATE 一行 2MB ---- */
    {
        int h = snprintf(g_sql, 64, "UPDATE t SET v='");
        memcpy(g_sql + h, g_payload, g_plen);
        int off = h + (int)g_plen;
        off += snprintf(g_sql + off, 64, "' WHERE id=0");
        t = now_ms();
        if (nlsql_query(c, g_sql, NULL) != NLSQL_OK) fprintf(stderr, "update: %s\n", nlsql_error(c));
        printf("BIGROW UPDATE_ONE ms=%.1f rss=%ldMB\n", now_ms() - t, vm_kb() / 1024);
        (void)off;
    }

    /* ---- 关闭（最终压实） ---- */
    t = now_ms();
    nlsql_close(c);
    double d_cl = now_ms() - t;
    printf("BIGROW CLOSE ms=%.1f rss_after=%ldMB\n", d_cl, vm_kb() / 1024);

    char p1[512], p2[512];
    snprintf(p1, sizeof p1, "%s/bdb/t.nlt", dir);
    snprintf(p2, sizeof p2, "%s/bdb/journal.log", dir);
    long logical = (nrows + 1) * (long)g_plen;   /* +1 = UPDATE 重写一次 */
    long w1 = io_long("wchar");
    long wb = io_long("write_bytes");
    printf("BIGROW FILES snapshot=%ldKB journal=%ldKB\n", fsize(p1) / 1024, fsize(p2) / 1024);
    printf("BIGROW IO wchar_delta=%ldMB logical=%ldMB write_amplification=%.2fx write_bytes=%ldMB\n",
           (w1 - w0) / 1048576, logical / 1048576,
           w0 >= 0 && w1 >= 0 ? (double)(w1 - w0) / (double)logical : -1.0,
           wb / 1048576);
    return 0;
}
