/* bigstress：大行（默认 2MB TEXT）并发读写压力——嵌入式同步写 + MySQL 协议(loopback)读
 *
 * 阶段：
 *   1) 预填充 nprefill 行并 flush（读者可见基线）
 *   2) 启动 wire，读者空闲基线（写空转 3s）→ 基线 QPS/延迟
 *   3) 压力窗口 secs 秒：主线程同步 INSERT（行数 < cap 时）/ UPDATE（到 cap 后），读者持续点查
 *   4) 收尾：wire 全表大读（端到端 MB/s）、flush 停顿、文件大小、写放大
 *
 * 用法：bigstress <datadir> <port> <rowsize_kb> <nprefill> <cap_rows> <secs> <nreaders> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <time.h>
#include <netleaf_nlsql.h>

static int g_port, g_secs, g_nread, g_rowkb;
static long g_plen, g_prefill, g_cap;
static volatile int g_phase = 0;          /* 0=准备 1=基线 2=压力 3=停止 */
static nlsql_conn_t* g_conn;

/* 读者统计（按 phase 分桶：1=基线 2=压力） */
static long g_ok[4], g_err[4], g_rows1[4], g_bytes[4];
static long g_lat_usum[4];                /* 累计微秒（atomic 加） */
static double g_lat_max[4];
static long g_gt10ms[4], g_gt100ms[4];

/* 写者统计 */
static long g_nins, g_nupd, g_wbytes;
static double g_wlat_us, g_wlat_max;
static long g_w_gt10ms, g_w_gt100ms;
static volatile long g_next_id;

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

/* ---------- 极简 MySQL 协议客户端（读侧） ---------- */
static int rd_n(int fd, void* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char*)buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}
static int wr_n(int fd, const void* buf, size_t n) {
    size_t put = 0;
    while (put < n) {
        ssize_t w = send(fd, (const char*)buf + put, n - put, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        put += (size_t)w;
    }
    return 0;
}
static int rd_pkt(int fd, uint8_t** out, size_t* outlen, uint8_t* seq) {
    uint8_t hdr[4];
    if (rd_n(fd, hdr, 4) != 0) return -1;
    size_t len = (size_t)hdr[0] | ((size_t)hdr[1] << 8) | ((size_t)hdr[2] << 16);
    if (seq) *seq = hdr[3];
    uint8_t* p = (uint8_t*)malloc(len + 1);
    if (!p) return -1;
    if (len && rd_n(fd, p, len) != 0) { free(p); return -1; }
    p[len] = 0;
    *out = p; *outlen = len;
    return 0;
}
static int do_handshake(int fd) {
    uint8_t* p = NULL; size_t len = 0; uint8_t seq = 0;
    if (rd_pkt(fd, &p, &len, &seq) != 0) return -1;
    if (p[0] != 10) { free(p); return -1; }
    free(p);
    uint8_t buf[160]; size_t o = 0;
    uint32_t caps = 0x1 | 0x200 | 0x2000 | 0x8000 | 0x80000;
    uint32_t maxpkt = 0x1000000;
    memcpy(buf + o, &caps, 4); o += 4;
    memcpy(buf + o, &maxpkt, 4); o += 4;
    buf[o++] = 33;
    memset(buf + o, 0, 23); o += 23;
    memcpy(buf + o, "root", 5); o += 5;
    buf[o++] = 0x00;
    memcpy(buf + o, "mysql_native_password", 22); o += 22;
    uint8_t hdr[4] = { (uint8_t)o, (uint8_t)(o >> 8), (uint8_t)(o >> 16), (uint8_t)(seq + 1) };
    if (wr_n(fd, hdr, 4) != 0) return -1;
    if (wr_n(fd, buf, o) != 0) return -1;
    if (rd_pkt(fd, &p, &len, &seq) != 0) return -1;
    int ok = (p[0] == 0x00);
    free(p);
    return ok ? 0 : -1;
}
/* 发查询并消费响应；*rows=结果集行数，*bytes=行包累计载荷字节 */
static int do_query(int fd, const char* sql, int* rows, long* bytes) {
    char pbuf[256];
    size_t sl = strlen(sql);
    if (sl + 1 > sizeof pbuf) return -1;
    pbuf[0] = 0x03;
    memcpy(pbuf + 1, sql, sl);
    size_t pl = sl + 1;
    uint8_t hdr[4] = { (uint8_t)pl, (uint8_t)(pl >> 8), (uint8_t)(pl >> 16), 0 };
    if (wr_n(fd, hdr, 4) != 0) return -1;
    if (wr_n(fd, pbuf, pl) != 0) return -1;
    uint8_t* p = NULL; size_t len = 0; uint8_t seq = 0;
    if (rd_pkt(fd, &p, &len, &seq) != 0) return -1;
    if (p[0] == 0xFF) { free(p); return -1; }
    if (p[0] == 0x00) { free(p); if (rows) *rows = 0; return 0; }
    uint64_t cols = 0;
    for (size_t i = 0; i < len; i++) {
        cols = p[i] & 0x7F;
        if (!(p[i] & 0x80)) break;
    }
    free(p);
    long bytes_sum = 0;
    for (uint64_t c = 0; c < cols; c++) {
        if (rd_pkt(fd, &p, &len, &seq) != 0) return -1;
        free(p);
    }
    if (rd_pkt(fd, &p, &len, &seq) != 0) return -1;
    free(p);
    int n = 0;
    for (;;) {
        if (rd_pkt(fd, &p, &len, &seq) != 0) return -1;
        int is_eof = (p[0] == 0xFE && len < 9);
        bytes_sum += (long)len;
        free(p);
        if (is_eof) break;
        n++;
        if (n > 10000000) return -1;
    }
    if (rows) *rows = n;
    if (bytes) *bytes += bytes_sum;
    return 0;
}

/* ---------- 读线程 ---------- */
static void* reader(void* arg) {
    long idx = (long)arg;
    unsigned seed = (unsigned)idx * 7919u + 1u;
    int fd = -1;
    for (int t = 0; t < 60 && fd < 0; t++) {
        struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)g_port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(fd, (struct sockaddr*)&sa, sizeof sa) != 0) { close(fd); fd = -1; sleep(1); continue; }
        int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (do_handshake(fd) != 0) { close(fd); fd = -1; sleep(1); continue; }
        if (do_query(fd, "USE sdb", NULL, NULL) != 0) { close(fd); fd = -1; sleep(1); }
    }
    if (fd < 0) { g_err[1] += 1000000; return NULL; }

    while (g_phase == 0) { struct timespec z = {0, 10 * 1000000L}; nanosleep(&z, NULL); }
    long pool = g_prefill > 0 ? g_prefill : 1;
    while (g_phase < 3) {
        int ph = g_phase;
        if (ph == 0 || ph == 3) break;
        long id = (long)rand_r(&seed) % pool;
        char sql[96];
        snprintf(sql, sizeof sql, "SELECT v FROM t WHERE id=%ld", id);
        int rows = 0; long bytes = 0;
        double t0 = now_ms();
        int rc = do_query(fd, sql, &rows, &bytes);
        double dt = now_ms() - t0;
        if (rc == 0) {
            __sync_fetch_and_add(&g_ok[ph], 1);
            if (rows == 1) __sync_fetch_and_add(&g_rows1[ph], 1);
            __sync_fetch_and_add(&g_bytes[ph], bytes);
            __sync_fetch_and_add(&g_lat_usum[ph], (long)(dt * 1000.0));
            /* 最大延迟（近似：无锁窗口允许竞态） */
            if (dt > g_lat_max[ph]) g_lat_max[ph] = dt;
            if (dt > 10) __sync_fetch_and_add(&g_gt10ms[ph], 1);
            if (dt > 100) __sync_fetch_and_add(&g_gt100ms[ph], 1);
        } else {
            __sync_fetch_and_add(&g_err[ph], 1);
        }
    }
    close(fd);
    return NULL;
}

int main(int argc, char** argv) {
    setbuf(stdout, NULL);
    if (argc < 8) {
        fprintf(stderr, "usage: bigstress <datadir> <port> <rowsize_kb> <nprefill> <cap_rows> <secs> <nreaders>\n");
        return 2;
    }
    const char* dir = argv[1];
    g_port = atoi(argv[2]);
    g_rowkb = atoi(argv[3]);
    g_prefill = atol(argv[4]);
    g_cap = atol(argv[5]);
    g_secs = atoi(argv[6]);
    g_nread = atoi(argv[7]);
    g_plen = (long)g_rowkb * 1024;
    if (g_cap < g_prefill) g_cap = g_prefill;

    char* payload = malloc((size_t)g_plen);
    char* sqlbuf = malloc(4u * 1024 * 1024 + 8192);
    if (!payload || !sqlbuf) { fprintf(stderr, "malloc fail\n"); return 1; }
    for (long i = 0; i < g_plen; i++) payload[i] = (char)('a' + (i * 31 % 26));

    printf("BIGSTRESS CFG rowsize_kb=%d prefill=%ld cap=%ld secs=%d readers=%d rss0=%ldMB\n",
           g_rowkb, g_prefill, g_cap, g_secs, g_nread, vm_kb() / 1024);
    long w0 = io_long("wchar");

    nlsql_open_opts_t o; memset(&o, 0, sizeof o);
    o.data_dir = dir;
    g_conn = nlsql_open_ex(&o);
    if (!g_conn) { fprintf(stderr, "open fail: %s\n", nlsql_global_error()); return 1; }
    nlsql_query(g_conn, "CREATE DATABASE sdb", NULL);
    nlsql_query(g_conn, "USE sdb", NULL);
    if (nlsql_query(g_conn, "CREATE TABLE t (id INT PRIMARY KEY, v TEXT, s DOUBLE)", NULL) != NLSQL_OK) {
        fprintf(stderr, "create: %s\n", nlsql_error(g_conn)); return 1;
    }

    /* 预填充 */
    double t = now_ms();
    for (long id = 0; id < g_prefill; id++) {
        int h = snprintf(sqlbuf, 64, "INSERT INTO t VALUES (%ld, '", id);
        memcpy(sqlbuf + h, payload, (size_t)g_plen);
        int off = h + (int)g_plen;
        off += snprintf(sqlbuf + off, 64, "', %.3f)", (double)id);
        if (nlsql_query(g_conn, sqlbuf, NULL) != NLSQL_OK) {
            fprintf(stderr, "prefill id=%ld: %s\n", id, nlsql_error(g_conn)); return 1;
        }
        (void)off;
    }
    if (nlsql_flush(g_conn) != NLSQL_OK) fprintf(stderr, "prefill flush: %s\n", nlsql_error(g_conn));
    g_next_id = g_prefill;   /* 预填充占了 0..prefill-1，写者从其后开始 */
    printf("BIGSTRESS PREFILL rows=%ld MB=%.0f total=%.2fs MB_per_s=%.1f rss=%ldMB\n",
           g_prefill, (double)g_prefill * g_plen / 1048576, (now_ms() - t) / 1000.0,
           (double)g_prefill * g_plen / 1048576 / ((now_ms() - t) / 1000.0), vm_kb() / 1024);

    /* wire + 读者 */
    int wh = nlsql_wire_start(g_conn, g_port, 16);
    if (wh < 0) { fprintf(stderr, "wire start fail\n"); return 1; }
    printf("BIGSTRESS WIRE port=%d handle=%d\n", g_port, wh);

    pthread_t* th = calloc((size_t)g_nread, sizeof(pthread_t));
    for (long i = 0; i < g_nread; i++) pthread_create(&th[i], NULL, reader, (void*)i);
    sleep(2); /* 连接建立 */

    /* ---- 阶段 1：读者基线（写空转 3s）---- */
    g_phase = 1;
    double tb0 = now_ms();
    sleep(3);
    double dtb = now_ms() - tb0;
    long base_ok = g_ok[1], base_bytes = g_bytes[1], base_err = g_err[1];
    printf("BIGSTRESS BASELINE readers=%d ok=%ld err=%ld qps=%.0f MB_per_s=%.1f avg_us=%.0f max_ms=%.1f gt10ms=%ld gt100ms=%ld\n",
           g_nread, base_ok, base_err, base_ok / (dtb / 1000.0),
           (double)base_bytes / 1048576.0 / (dtb / 1000.0),
           base_ok ? (double)g_lat_usum[1] / base_ok : -1, g_lat_max[1],
           g_gt10ms[1], g_gt100ms[1]);

    /* ---- 阶段 2：读写压力 ---- */
    g_phase = 2;
    double tw0 = now_ms();
    double deadline = tw0 + g_secs * 1000.0;
    unsigned seed = 42u;
    long rss_max = vm_kb();
    int rss_stop = 0;
    while (now_ms() < deadline) {
        if (g_next_id < g_cap) {          /* 行数未到上限：INSERT 增长 */
            long id = g_next_id++;
            int h = snprintf(sqlbuf, 64, "INSERT INTO t VALUES (%ld, '", id);
            memcpy(sqlbuf + h, payload, (size_t)g_plen);
            int off = h + (int)g_plen;
            off += snprintf(sqlbuf + off, 64, "', %.3f)", (double)id);
            double t0 = now_ms();
            if (nlsql_query(g_conn, sqlbuf, NULL) != NLSQL_OK) {
                fprintf(stderr, "ins id=%ld: %s\n", id, nlsql_error(g_conn));
            } else {
                double dt = now_ms() - t0;
                g_nins++; g_wbytes += g_plen;
                g_wlat_us += dt * 1000.0;
                if (dt > g_wlat_max) g_wlat_max = dt;
                if (dt > 10) g_w_gt10ms++;
                if (dt > 100) g_w_gt100ms++;
            }
            (void)off;
        } else {                          /* 到上限：UPDATE 持续重写 2MB */
            long id = (long)rand_r(&seed) % g_cap;
            int h = snprintf(sqlbuf, 64, "UPDATE t SET v='");
            memcpy(sqlbuf + h, payload, (size_t)g_plen);
            int off = h + (int)g_plen;
            off += snprintf(sqlbuf + off, 64, "' WHERE id=%ld", id);
            double t0 = now_ms();
            if (nlsql_query(g_conn, sqlbuf, NULL) != NLSQL_OK) {
                fprintf(stderr, "upd id=%ld: %s\n", id, nlsql_error(g_conn));
            } else {
                double dt = now_ms() - t0;
                g_nupd++; g_wbytes += g_plen;
                g_wlat_us += dt * 1000.0;
                if (dt > g_wlat_max) g_wlat_max = dt;
                if (dt > 10) g_w_gt10ms++;
                if (dt > 100) g_w_gt100ms++;
            }
            (void)off;
        }
        if ((g_nins + g_nupd) % 64 == 0) {
            long m = vm_kb(); if (m > rss_max) rss_max = m;
            if (m > 3200 * 1024) { rss_stop = 1; break; }   /* cgroup 4GiB 保险 */
        }
    }
    double dtw = now_ms() - tw0;
    g_phase = 3;
    for (long i = 0; i < g_nread; i++) pthread_join(th[i], NULL);

    long w_ok = g_ok[2], w_err = g_err[2], w_bytes = g_bytes[2];
    long w_rows1 = g_rows1[2];
    long nwr = g_nins + g_nupd;
    printf("BIGSTRESS WRITE window=%.1fs ins=%ld upd=%ld rows=%ld MB=%.0f MB_per_s=%.1f rows_per_s=%.0f avg_us=%.0f max_ms=%.1f gt10ms=%ld gt100ms=%ld rss_max=%ldMB%s\n",
           dtw / 1000.0, g_nins, g_nupd, nwr, (double)g_wbytes / 1048576.0,
           (double)g_wbytes / 1048576.0 / (dtw / 1000.0), nwr / (dtw / 1000.0),
           nwr ? g_wlat_us / nwr : -1, g_wlat_max, g_w_gt10ms, g_w_gt100ms,
           rss_max / 1024, rss_stop ? " (RSS_LIMIT_STOP)" : "");
    printf("BIGSTRESS READ_STRESS readers=%d ok=%ld err=%ld rows1=%ld qps=%.0f MB_per_s=%.1f avg_us=%.0f max_ms=%.1f gt10ms=%ld gt100ms=%ld\n",
           g_nread, w_ok, w_err, w_rows1, w_ok / (dtw / 1000.0),
           (double)w_bytes / 1048576.0 / (dtw / 1000.0),
           w_ok ? (double)g_lat_usum[2] / w_ok : -1, g_lat_max[2], g_gt10ms[2], g_gt100ms[2]);

    /* ---- wire 全表大读（端到端）---- */
    {
        struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)g_port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0 && connect(fd, (struct sockaddr*)&sa, sizeof sa) == 0) {
            int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            if (do_handshake(fd) == 0) {
                do_query(fd, "USE sdb", NULL, NULL);
                int rows = 0; long bytes = 0;
                double t0 = now_ms();
                int rc = do_query(fd, "SELECT v FROM t", &rows, &bytes);
                double dt = now_ms() - t0;
                printf("BIGSTRESS WIRE_FULLREAD rc=%d rows=%d MB=%.0f ms=%.1f MB_per_s=%.1f rss=%ldMB\n",
                       rc, rows, (double)bytes / 1048576.0, dt,
                       dt > 0 ? (double)bytes / 1048576.0 / (dt / 1000.0) : -1, vm_kb() / 1024);
            }
            close(fd);
        }
    }

    /* ---- 收尾 ---- */
    t = now_ms();
    nlsql_wire_stop(wh);                  /* 先停 wire（其线程引用 conn），再关连接 */
    if (nlsql_flush(g_conn) != NLSQL_OK) fprintf(stderr, "final flush: %s\n", nlsql_error(g_conn));
    double dflush = now_ms() - t;
    t = now_ms();
    nlsql_close(g_conn);
    double dclose = now_ms() - t;

    char p1[512], p2[512];
    snprintf(p1, sizeof p1, "%s/sdb/t.nlt", dir);
    snprintf(p2, sizeof p2, "%s/sdb/journal.log", dir);
    long logical = (g_prefill + g_nins + g_nupd) * g_plen;
    long w1 = io_long("wchar");
    printf("BIGSTRESS FINAL flush_ms=%.1f close_ms=%.1f rss_after=%ldMB snapshot=%ldMB journal=%ldKB\n",
           dflush, dclose, vm_kb() / 1024, fsize(p1) / 1048576, fsize(p2) / 1024);
    printf("BIGSTRESS IO wchar_delta=%ldMB logical=%ldMB write_amplification=%.2fx\n",
           (w1 - w0) / 1048576, logical / 1048576,
           w0 >= 0 && w1 >= 0 ? (double)(w1 - w0) / (double)logical : -1.0);
    free(th); free(payload); free(sqlbuf);
    return (w_err == 0 && base_err == 0) ? 0 : 1;
}
