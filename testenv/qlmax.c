/* 1C2G 查库极限速度基准
 * 目标：在单核 + 2GB 约束下，压出嵌入式 API 查询路径的最快速度与持续稳定性。
 * 场景：
 *   1) 主键点查紧循环（可变 id，含解析/执行/取行/结果释放全链路）
 *   2) 三列投影点查 / 缺失主键点查
 *   3) 全表 COUNT(*) 重复压测（行吞吐）
 *   4) 全表全量 SELECT（行吞吐 + 结果集内存峰值）
 *   5) 20 秒持续点查稳定性（5 秒窗口 QPS + RSS）
 *   6) 同一连接 1/2/4 线程并发只读（1 核下的扩展性/争用）
 * 输出：每阶段 n / 总耗时 / 平均 / p50 / p95 / p99 / max / QPS，末尾峰值 RSS 与 CPU 核数占用。
 * 用法：./qlmax <dir> [rows] [seed]       （外部用 taskset -c 0 与 ulimit -v 2000000）
 */
#define _POSIX_C_SOURCE 200809L
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define RING 65536

static long g_rows = 50000;
static uint64_t g_seed = 88172645463325252ull;

static uint64_t rnd_next(uint64_t* s) {
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x;
    return x;
}
static long rnd_range(uint64_t* s, long lo, long hi) {
    return lo + (long)(rnd_next(s) % (uint64_t)(hi - lo + 1));
}

static double now_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

/* 进程累计 CPU 时间（秒），用于算实际占用的核数 */
static double cpu_sec(void) {
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return -1;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (!n) return -1;
    buf[n] = 0;
    /* 第 14/15 字段：utime stime（comm 里可能有空格，跳过括号） */
    char* p = strrchr(buf, ')');
    if (!p) return -1;
    p += 2; /* 跳过 ") "，此时 p 指向第 3 字段 state */
    int field = 3;
    char* save = NULL;
    double v = 0;
    for (char* tok = strtok_r(p, " ", &save); tok; tok = strtok_r(NULL, " ", &save), field++) {
        if (field == 14 || field == 15) v += atof(tok);
        if (field > 15) break;
    }
    long hz = sysconf(_SC_CLK_TCK);
    return hz > 0 ? v / (double)hz : v;
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
    printf("QLMAX mem %-16s rss_mb=%ld peak_mb=%ld vsz_mb=%ld\n", tag,
           read_mem_mb("VmRSS:"), read_mem_mb("VmHWM:"), read_mem_mb("VmSize:"));
    fflush(stdout);
}

/* ---------- 延迟统计（环形样本 + 总量） ---------- */
typedef struct {
    const char* name;
    long long   n;
    double      sum_us, max_us, t0, t1;
    double*     ring;      /* 最近 RING 个样本 */
    unsigned    pos, cnt;
} st_t;

static int st_init(st_t* s, const char* name) {
    memset(s, 0, sizeof(*s));
    s->name = name;
    s->ring = (double*)malloc(sizeof(double) * RING);
    return s->ring ? 0 : -1;
}

static inline void st_rec(st_t* s, double us) {
    s->n++;
    s->sum_us += us;
    if (us > s->max_us) s->max_us = us;
    s->ring[s->pos] = us;
    s->pos = (s->pos + 1) % RING;
    if (s->cnt < RING) s->cnt++;
}

static void st_begin(st_t* s) { s->n = 0; s->sum_us = 0; s->max_us = 0; s->pos = s->cnt = 0; s->t0 = now_us(); }
static double st_end(st_t* s) { s->t1 = now_us(); return s->t1 - s->t0; }

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* wall_us：本阶段墙钟微秒（含所有开销），QPS 按它算 */
static void st_report(st_t* s, double wall_us) {
    double avg = s->n ? s->sum_us / (double)s->n : 0;
    double p50 = 0, p95 = 0, p99 = 0;
    if (s->cnt) {
        double* tmp = (double*)malloc(sizeof(double) * s->cnt);
        if (tmp) {
            memcpy(tmp, s->ring, sizeof(double) * s->cnt);
            qsort(tmp, s->cnt, sizeof(double), cmp_d);
            p50 = tmp[(size_t)(s->cnt * 0.50)];
            p95 = tmp[(size_t)(s->cnt * 0.95) < s->cnt ? (size_t)(s->cnt * 0.95) : s->cnt - 1];
            p99 = tmp[(size_t)(s->cnt * 0.99) < s->cnt ? (size_t)(s->cnt * 0.99) : s->cnt - 1];
            free(tmp);
        }
    }
    printf("QLMAX %-18s n=%-9lld wall_ms=%-9.1f avg_us=%-7.2f p50=%-7.2f p95=%-8.2f p99=%-8.2f max_us=%-9.2f qps=%.0f\n",
           s->name, s->n, wall_us / 1000.0, avg, p50, p95, p99, s->max_us,
           wall_us > 0 ? (double)s->n * 1e6 / wall_us : 0);
    fflush(stdout);
    free(s->ring); s->ring = NULL;
}

/* ---------- 查询封装 ---------- */
static int do_query(nlsql_conn_t* c, const char* sql) {
    nlsql_result_t* r = NULL;
    int rc = nlsql_query(c, sql, &r);
    if (rc != NLSQL_OK) {
        fprintf(stderr, "QLMAX ERR rc=%d errno=%d [%s] %s\n", rc, nlsql_errno(c), sql, nlsql_error(c));
        return -1;
    }
    nlsql_free_result(r);
    return 0;
}

/* 单次计时点查：返回 0 成功 */
static inline int timed_pk(nlsql_conn_t* c, const char* sql) {
    nlsql_result_t* r = NULL;
    int rc = nlsql_query(c, sql, &r);
    if (rc != NLSQL_OK) return -1;
    nlsql_free_result(r);
    return 0;
}

typedef struct {
    nlsql_conn_t* c;
    long          rows;
    long          dur_ms;
    uint64_t      seed;
    int           proj;     /* 0 = SELECT *；1 = SELECT id,a,c；2 = 缺失主键 */
    st_t          st;
    int           err;
} thr_arg_t;

static void* pk_worker(void* p) {
    thr_arg_t* a = (thr_arg_t*)p;
    char sql[256];
    uint64_t s = a->seed ? a->seed : 1;
    double deadline = now_us() + (double)a->dur_ms * 1000.0;
    st_begin(&a->st);
    while (now_us() < deadline) {
        long id;
        if (a->proj == 2) id = g_rows + 1 + rnd_range(&s, 0, a->rows);
        else              id = rnd_range(&s, 1, a->rows);
        if (a->proj == 0)      snprintf(sql, sizeof(sql), "SELECT * FROM t0 WHERE id = %ld", id);
        else if (a->proj == 1) snprintf(sql, sizeof(sql), "SELECT id,a,c FROM t0 WHERE id = %ld", id);
        else                   snprintf(sql, sizeof(sql), "SELECT id FROM t0 WHERE id = %ld", id);
        double t0 = now_us();
        int rc = timed_pk(a->c, sql);
        double dt = now_us() - t0;
        if (rc != 0) { a->err = 1; break; }
        st_rec(&a->st, dt);
    }
    a->st.t1 = now_us();
    return NULL;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "d_qlmax";
    if (argc > 2) g_rows = atol(argv[2]);
    if (argc > 3) g_seed = strtoull(argv[3], NULL, 10);
    if (!g_seed) g_seed = 1;
    if (g_rows < 1000) g_rows = 1000;
    setvbuf(stdout, NULL, _IOLBF, 0);

    nlsql_open_opts_t o;
    memset(&o, 0, sizeof(o));
    o.data_dir = dir; o.workers = 1; o.journal_mb = 16;
    nlsql_conn_t* c = nlsql_open_ex(&o);
    if (!c) { fprintf(stderr, "QLMAX open fail: %s\n", nlsql_global_error()); return 1; }

    printf("QLMAX cfg rows=%ld workers=1 seed=%llu\n", g_rows, (unsigned long long)g_seed);

    char sql[512];
    double t_setup = now_us();
    if (do_query(c, "CREATE DATABASE lim") != 0) return 1;
    if (do_query(c, "USE lim") != 0) return 1;
    if (do_query(c, "CREATE TABLE t0 (id INTEGER PRIMARY KEY, a INTEGER, b INTEGER, "
                     "c VARCHAR(32), d DOUBLE)") != 0) return 1;

    uint64_t s = g_seed;
    for (long i = 1; i <= g_rows; i++) {
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t0 (id,a,b,c,d) VALUES (%ld,%ld,%ld,'k%03ld-xxxxxxxx',%ld.%ld)",
                 i, (long)(rnd_next(&s) % 100000), (long)(rnd_next(&s) % 100000),
                 (long)(rnd_next(&s) % 512), (long)(rnd_next(&s) % 1000), (long)(rnd_next(&s) % 10));
        if (do_query(c, sql) != 0) return 1;
    }
    double t_after_load = now_us();
    printf("QLMAX load rows=%ld load_ms=%.1f rows_per_s=%.0f\n",
           g_rows, (t_after_load - t_setup) / 1000.0, g_rows * 1e6 / (t_after_load - t_setup));
    mem_line("after_load");

    /* 预热：2 万次点查（页表/分支预测/堆稳态） */
    {
        st_t w; if (st_init(&w, "warmup") != 0) return 1;
        thr_arg_t a; memset(&a, 0, sizeof(a));
        a.c = c; a.rows = g_rows; a.dur_ms = 0; a.seed = g_seed ^ 0x9e37u; a.proj = 0;
        /* 用定次方式预热 */
        char q[256];
        st_begin(&w);
        for (int i = 0; i < 20000; i++) {
            long id = rnd_range(&a.seed, 1, g_rows);
            snprintf(q, sizeof(q), "SELECT * FROM t0 WHERE id = %ld", id);
            double t0 = now_us();
            if (timed_pk(c, q) != 0) return 1;
            st_rec(&w, now_us() - t0);
        }
        st_end(&w);
        st_report(&w, w.t1 - w.t0);
    }

    st_t st;
    thr_arg_t arg;

    /* 1) 主键点查 SELECT *：5 秒紧循环 */
    if (st_init(&st, "pk_point_star") != 0) return 1;
    memset(&arg, 0, sizeof(arg));
    arg.c = c; arg.rows = g_rows; arg.dur_ms = 5000; arg.seed = g_seed + 1; arg.proj = 0; arg.st = st;
    {
        double c0 = cpu_sec(), w0 = now_us();
        pk_worker(&arg);
        double wall = now_us() - w0;
        double c1 = cpu_sec();
        st_report(&arg.st, wall);
        printf("QLMAX cpu pk_point_star cpu_s=%.2f wall_s=%.2f cores=%.2f\n",
               c1 - c0, wall / 1e6, (c1 - c0) / (wall / 1e6));
    }
    if (arg.err) return 1;

    /* 2) 三列投影点查：3 秒 */
    if (st_init(&st, "pk_point_3col") != 0) return 1;
    memset(&arg, 0, sizeof(arg));
    arg.c = c; arg.rows = g_rows; arg.dur_ms = 3000; arg.seed = g_seed + 2; arg.proj = 1; arg.st = st;
    { double w0 = now_us(); pk_worker(&arg); st_report(&arg.st, now_us() - w0); }
    if (arg.err) return 1;

    /* 3) 缺失主键点查：3 秒（快速判空路径） */
    if (st_init(&st, "pk_miss") != 0) return 1;
    memset(&arg, 0, sizeof(arg));
    arg.c = c; arg.rows = g_rows; arg.dur_ms = 3000; arg.seed = g_seed + 3; arg.proj = 2; arg.st = st;
    { double w0 = now_us(); pk_worker(&arg); st_report(&arg.st, now_us() - w0); }
    if (arg.err) return 1;
    mem_line("after_pk");

    /* 4) 全表 COUNT(*) ×200：行吞吐 */
    {
        int reps = 200;
        if (st_init(&st, "count_star") != 0) return 1;
        st_begin(&st);
        for (int i = 0; i < reps; i++) {
            nlsql_result_t* r = NULL;
            double t0 = now_us();
            if (nlsql_query(c, "SELECT COUNT(*) FROM t0", &r) != NLSQL_OK) return 1;
            double dt = now_us() - t0;
            if (!r || r->row_count != 1 || r->rows[0].vals[0].i != g_rows) {
                fprintf(stderr, "QLMAX count mismatch\n"); return 1;
            }
            nlsql_free_result(r);
            st_rec(&st, dt);
        }
        double wall = st_end(&st);
        st_report(&st, wall);
        printf("QLMAX count_star rows_scan_per_s=%.0f rows_total=%lld\n",
               (double)g_rows * reps * 1e6 / wall, (long long)g_rows * reps);
    }

    /* 5) 全表全量 SELECT（3 列）×20：行吞吐 + 结果集内存 */
    {
        int reps = 20;
        if (st_init(&st, "scan_full_3col") != 0) return 1;
        st_begin(&st);
        for (int i = 0; i < reps; i++) {
            nlsql_result_t* r = NULL;
            double t0 = now_us();
            if (nlsql_query(c, "SELECT id,a,b FROM t0", &r) != NLSQL_OK) return 1;
            double dt = now_us() - t0;
            if (!r || (long)r->row_count != g_rows) {
                fprintf(stderr, "QLMAX scan mismatch %lld\n", r ? (long long)r->row_count : -1); return 1;
            }
            nlsql_free_result(r);
            st_rec(&st, dt);
        }
        double wall = st_end(&st);
        st_report(&st, wall);
        printf("QLMAX scan_full_3col rows_out_per_s=%.0f\n", (double)g_rows * reps * 1e6 / wall);
        mem_line("after_scan_full");
    }

    /* 6) 20 秒持续点查：每 5 秒窗口 QPS + RSS（看长稳/泄漏） */
    {
        printf("QLMAX stability begin dur_s=20 window_s=5\n");
        uint64_t ss = g_seed + 7;
        char q[256];
        double all_t0 = now_us();
        long long total = 0;
        for (int w = 0; w < 4; w++) {
            double w0 = now_us(), wd = w0 + 5e6;
            long long cnt = 0;
            while (now_us() < wd) {
                long id = rnd_range(&ss, 1, g_rows);
                snprintf(q, sizeof(q), "SELECT id,a FROM t0 WHERE id = %ld", id);
                if (timed_pk(c, q) != 0) return 1;
                cnt++;
            }
            double dt = now_us() - w0;
            long rss = read_mem_mb("VmRSS:");
            printf("QLMAX stab win=%d qps=%.0f rss_mb=%ld\n", w, (double)cnt * 1e6 / dt, rss);
            total += cnt;
        }
        double dt = now_us() - all_t0;
        printf("QLMAX stab total_qps=%.0f total_n=%lld\n", (double)total * 1e6 / dt, total);
        mem_line("after_stability");
    }

    /* 7) 并发只读：同一连接 1/2/4 线程（1 核下看锁争用与扩展性） */
    for (int nt = 1; nt <= 4; nt *= 2) {
        thr_arg_t* args = (thr_arg_t*)calloc((size_t)nt, sizeof(thr_arg_t));
        pthread_t* th = (pthread_t*)calloc((size_t)nt, sizeof(pthread_t));
        if (!args || !th) return 1;
        char label[64];
        snprintf(label, sizeof(label), "conc_%dthr_total", nt);
        st_t tot; if (st_init(&tot, label) != 0) return 1;
        double w0 = now_us();
        for (int i = 0; i < nt; i++) {
            args[i].c = c; args[i].rows = g_rows; args[i].dur_ms = 3000;
            args[i].seed = g_seed + 100 + (uint64_t)i; args[i].proj = 1;
            if (st_init(&args[i].st, "thr") != 0) return 1;
            pthread_create(&th[i], NULL, pk_worker, &args[i]);
        }
        for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
        double wall = now_us() - w0;
        long long n = 0; double sum = 0, mx = 0;
        for (int i = 0; i < nt; i++) {
            if (args[i].err) { fprintf(stderr, "QLMAX thread err\n"); return 1; }
            n += args[i].st.n; sum += args[i].st.sum_us;
            if (args[i].st.max_us > mx) mx = args[i].st.max_us;
        }
        tot.n = n; tot.sum_us = sum; tot.max_us = mx;
        /* 混合各线程环形样本（抽样步长 7），得到总延迟分布 */
        for (int i = 0; i < nt && tot.cnt < RING; i++) {
            unsigned step = 7;
            for (unsigned k = 0; k < args[i].st.cnt && tot.cnt < RING; k += step)
                tot.ring[tot.cnt++] = args[i].st.ring[k];
            free(args[i].st.ring); args[i].st.ring = NULL;
        }
        tot.pos = tot.cnt % RING;
        st_report(&tot, wall);
        free(args); free(th);
        mem_line("after_conc");
    }

    /* 收尾：flush + 峰值 RSS + 重开校验 */
    double tf = now_us();
    if (nlsql_flush(c) != NLSQL_OK) return 1;
    printf("QLMAX flush_ms=%.1f peak_rss_mb=%ld\n", (now_us() - tf) / 1000.0, read_mem_mb("VmHWM:"));
    nlsql_close(c);

    double tr = now_us();
    c = nlsql_open_ex(&o);
    if (!c) { fprintf(stderr, "QLMAX reopen fail\n"); return 1; }
    if (do_query(c, "USE lim") != 0) return 1;
    nlsql_result_t* r = NULL;
    if (nlsql_query(c, "SELECT COUNT(*) FROM t0", &r) != NLSQL_OK) return 1;
    long long cnt = r->rows[0].vals[0].i;
    nlsql_free_result(r);
    printf("QLMAX reopen_ms=%.0f count=%lld %s\n", (now_us() - tr) / 1000.0, cnt,
           cnt == g_rows ? "OK" : "MISMATCH");
    nlsql_close(c);
    printf("QLMAX DONE\n");
    return cnt == g_rows ? 0 : 1;
}
