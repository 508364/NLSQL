/* 校验增量帧快照：多次 flush 之间只追加新行 → 文件只增长新增行；重开解析正确；
   改行后退化为整表重写；尾部残缺帧 / 垃圾尾 → 加载时回滚截断且数据不丢 */
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int run(nlsql_conn_t* c, const char* sql) {
    nlsql_result_t* r = NULL;
    int rc = nlsql_query(c, sql, &r);
    if (rc != NLSQL_OK) { fprintf(stderr, "ERR %s: %s\n", sql, nlsql_global_error()); return rc; }
    nlsql_free_result(r);
    return 0;
}

static long long scalar(nlsql_conn_t* c, const char* sql) {
    nlsql_result_t* r = NULL;
    if (nlsql_query(c, sql, &r) != NLSQL_OK) { fprintf(stderr, "ERR %s\n", sql); return -1; }
    long long v = -1;
    if (r->row_count && r->col_count) v = r->rows[0].vals[0].i;
    nlsql_free_result(r);
    return v;
}

static long long fsize(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 ? (long long)st.st_size : -1;
}

static int ins(nlsql_conn_t* c, int lo, int hi) {
    char sql[160];
    for (int i = lo; i <= hi; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO t (id, v) VALUES (%d, 'row-%d-xxxxxxxxxxxxxxxx')", i, i);
        if (run(c, sql) != 0) return -1;
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "d_incr";
    char nlt[512];
    snprintf(nlt, sizeof(nlt), "%s/d/t.nlt", dir);
    snprintf(nlt, sizeof(nlt), "%s/d/t.nlt", dir);
    int ok = 1;

    nlsql_conn_t* c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "open1 fail: %s\n", nlsql_global_error()); return 1; }
    run(c, "CREATE DATABASE d");
    run(c, "USE d");
    run(c, "CREATE TABLE t (id INTEGER PRIMARY KEY, v VARCHAR(64))");

    ins(c, 1, 200);
    nlsql_flush(c);
    long long s1 = fsize(nlt);

    ins(c, 201, 300);
    nlsql_flush(c);
    long long s2 = fsize(nlt);

    ins(c, 301, 400);
    nlsql_flush(c);
    long long s3 = fsize(nlt);
    nlsql_close(c);

    printf("INCR file base=%lld after_frame1=%lld after_frame2=%lld\n", s1, s2, s3);
    /* 200 行基础区 ≈ 200*~35B ≈ 7KB；每个后续帧只应增长约 100 行（≈3.5KB），
       远小于整表重写（≈14KB / 21KB） */
    printf("INCR frame1_growth=%lld frame2_growth=%lld (full rewrite would write %lld / %lld)\n",
           s2 - s1, s3 - s2, s2, s3);
    /* 增量帧只写新增 100 行 → 增长远小于当前文件（整表重写会写全部行） */
    ok = ok && (s2 - s1) < s2 / 2 && (s3 - s2) < s3 / 2;

    /* 重开：走帧解析路径 */
    c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "open2 fail\n"); return 1; }
    run(c, "USE d");
    long long n = scalar(c, "SELECT COUNT(*) FROM t");
    long long n1 = scalar(c, "SELECT COUNT(*) FROM t WHERE id = 1");
    long long n250 = scalar(c, "SELECT COUNT(*) FROM t WHERE id = 250");
    long long n400 = scalar(c, "SELECT COUNT(*) FROM t WHERE id = 400");
    printf("INCR reopen count=%lld spot(1,250,400)=%lld,%lld,%lld\n", n, n1, n250, n400);
    ok = ok && n == 400 && n1 == 1 && n250 == 1 && n400 == 1;

    /* 改行 → 下次压实必须整表重写（帧被合并掉） */
    run(c, "UPDATE t SET v='changed' WHERE id = 150");
    nlsql_flush(c);
    long long s4 = fsize(nlt);
    printf("INCR after_update file=%lld (<= full size, frames merged)\n", s4);
    ok = ok && s4 > 0 && s4 <= s3 + 64;
    nlsql_close(c);

    c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "open3 fail\n"); return 1; }
    run(c, "USE d");
    n = scalar(c, "SELECT COUNT(*) FROM t");
    long long ch = scalar(c, "SELECT COUNT(*) FROM t WHERE id = 150");
    {
        nlsql_result_t* r = NULL;
        if (nlsql_query(c, "SELECT v FROM t WHERE id = 150", &r) != NLSQL_OK) return 1;
        const char* v = (r->row_count && r->rows[0].vals[0].s) ? r->rows[0].vals[0].s : "";
        printf("INCR after_update count=%lld v150=%s\n", n, v);
        ok = ok && n == 400 && ch == 1 && strcmp(v, "changed") == 0;
        nlsql_free_result(r);
    }
    nlsql_close(c);

    /* 尾部垃圾（< 帧头）→ 加载时截断，数据不丢 */
    FILE* f = fopen(nlt, "ab");
    if (f) { fwrite("GARBAGEGARBAGEGARB", 1, 18, f); fclose(f); }
    c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "open4 fail\n"); return 1; }
    run(c, "USE d");
    n = scalar(c, "SELECT COUNT(*) FROM t");
    long long s5 = fsize(nlt);
    printf("INCR garbage_tail count=%lld file_truncated=%lld\n", n, s5);
    ok = ok && n == 400 && s5 == s4;
    nlsql_close(c);

    /* 残缺帧（帧头完整、行数据截断）→ 同样回滚截断 */
    f = fopen(nlt, "ab");
    if (f) {
        unsigned char h[24];
        memset(h, 0, sizeof(h));
        h[8] = 1;               /* nrows = 1 */
        h[16] = 2;              /* next_auto = 2 */
        fwrite(h, 1, 24, f);
        fputc(1, f);            /* 第 0 列：int64 标签，但缺 8 字节 */
        fclose(f);
    }
    c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "open5 fail\n"); return 1; }
    run(c, "USE d");
    n = scalar(c, "SELECT COUNT(*) FROM t");
    long long s6 = fsize(nlt);
    printf("INCR partial_frame count=%lld file_truncated=%lld\n", n, s6);
    ok = ok && n == 400 && s6 == s5;
    nlsql_close(c);

    /* 截断后再追加：文件尾必须干净，重开仍正确 */
    c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "open6 fail\n"); return 1; }
    run(c, "USE d");
    ins(c, 401, 500);
    nlsql_flush(c);
    nlsql_close(c);
    c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "open7 fail\n"); return 1; }
    run(c, "USE d");
    n = scalar(c, "SELECT COUNT(*) FROM t");
    long long n500 = scalar(c, "SELECT COUNT(*) FROM t WHERE id = 500");
    printf("INCR append_after_truncate count=%lld id500=%lld\n", n, n500);
    ok = ok && n == 500 && n500 == 1;
    nlsql_close(c);

    /* 崩在追加帧中途：尾部残缺帧 + WAL 里还有未压实的插入 → 截断残缺帧后按 LSN 重放补齐 */
    c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "open8 fail\n"); return 1; }
    run(c, "USE d");
    ins(c, 501, 510);                 /* 只进 WAL，不 flush */
    nlsql_close(c);
    f = fopen(nlt, "ab");
    if (f) {
        unsigned char h[24];
        memset(h, 0, sizeof(h));
        h[1] = 1; h[8] = 3;           /* lsn=256, nrows=3，行数据只写了一半 */
        fwrite(h, 1, 24, f);
        fwrite("xx", 1, 2, f);
        fclose(f);
    }
    c = nlsql_open(dir);
    if (!c) { fprintf(stderr, "open9 fail\n"); return 1; }
    run(c, "USE d");
    n = scalar(c, "SELECT COUNT(*) FROM t");
    long long n510 = scalar(c, "SELECT COUNT(*) FROM t WHERE id = 510");
    printf("INCR crash_partial_frame count=%lld id510=%lld\n", n, n510);
    ok = ok && n == 510 && n510 == 1;
    nlsql_close(c);

    printf("INCR %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
