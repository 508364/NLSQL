/* wcbench：极简 MySQL 协议客户端，测 wire 服务端读上限（剔除 Python 客户端开销）
 * 用法：wcbench <port> <threads> <queries_per_thread> <db>
 * 服务端需未配置凭据（无 token 握手放行）。*/
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
#include <stdint.h>

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
    buf[o++] = 33;                    /* utf8 */
    memset(buf + o, 0, 23); o += 23;
    memcpy(buf + o, "root", 5); o += 5;
    buf[o++] = 0x00;                  /* 空 auth（服务端无凭据放行） */
    memcpy(buf + o, "mysql_native_password", 22); o += 22;
    /* 握手响应也是 MySQL 包：必须带 4 字节头（len + seq=收到的 seq+1） */
    uint8_t hdr[4] = { (uint8_t)o, (uint8_t)(o >> 8), (uint8_t)(o >> 16), (uint8_t)(seq + 1) };
    if (wr_n(fd, hdr, 4) != 0) return -1;
    if (wr_n(fd, buf, o) != 0) return -1;
    if (rd_pkt(fd, &p, &len, &seq) != 0) return -1;
    int ok = (p[0] == 0x00);
    free(p);
    return ok ? 0 : -1;
}

/* 发一条查询并消费完整响应；返回 0=OK，<0=协议错误；*rows 输出结果集行数 */
static int do_query(int fd, const char* sql, int* rows) {
    char pbuf[256];
    size_t sl = strlen(sql);
    if (sl + 1 > sizeof pbuf) return -1;
    pbuf[0] = (char)0x03;                          /* COM_QUERY */
    memcpy(pbuf + 1, sql, sl);
    size_t pl = sl + 1;
    uint8_t hdr[4] = { (uint8_t)pl, (uint8_t)(pl >> 8), (uint8_t)(pl >> 16), 0 };
    if (wr_n(fd, hdr, 4) != 0) return -1;
    if (wr_n(fd, pbuf, pl) != 0) return -1;
    uint8_t* p = NULL; size_t len = 0; uint8_t seq = 0;
    if (rd_pkt(fd, &p, &len, &seq) != 0) { fprintf(stderr, "[wc] read resp fail\n"); return -1; }
    if (p[0] == 0xFF) {
        int en = len >= 3 ? (p[1] | (p[2] << 8)) : 0;
        fprintf(stderr, "[wc] ERR %d: %s\n", en, len > 9 ? (char*)p + 9 : "");
        free(p); return -1;
    }
    if (p[0] == 0x00) { free(p); if (rows) *rows = 0; return 0; } /* OK（无结果集） */
    /* 结果集：colcount → coldef×C → EOF → rows… → EOF */
    uint64_t cols = 0;
    for (size_t i = 0; i < len; i++) {             /* 简化 lenenc（本基准列数<128） */
        cols = p[i] & 0x7F;
        if (!(p[i] & 0x80)) break;
    }
    free(p);
    for (uint64_t c = 0; c < cols; c++) {
        if (rd_pkt(fd, &p, &len, &seq) != 0) return -1;
        free(p);
    }
    /* EOF（或列定义后状态包） */
    if (rd_pkt(fd, &p, &len, &seq) != 0) return -1;
    free(p);
    int n = 0;
    for (;;) {
        if (rd_pkt(fd, &p, &len, &seq) != 0) return -1;
        int is_eof = (p[0] == 0xFE && len < 9);
        free(p);
        if (is_eof) break;
        n++;
        if (n > 1000000) return -1;
    }
    if (rows) *rows = n;
    return 0;
}

static int g_port, g_qps_warm = 1;
static long g_nq;
static int g_threads;
static const char* g_db = "wdb";
static volatile int g_go = 0;
static long g_ok_cnt = 0, g_err_cnt = 0;
static long g_rows_chk = 0;

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void* worker(void* arg) {
    long idx = (long)arg;
    int fd = -1;
    for (int t = 0; t < 50 && fd < 0; t++) {
        struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)g_port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(fd, (struct sockaddr*)&sa, sizeof sa) != 0) { close(fd); fd = -1; sleep(1); continue; }
        int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (do_handshake(fd) != 0) { close(fd); fd = -1; sleep(1); continue; }
        char use[64];
        snprintf(use, sizeof use, "USE %s", g_db);
        if (do_query(fd, use, NULL) != 0) { close(fd); fd = -1; sleep(1); }
    }
    if (fd < 0) { __sync_fetch_and_sub(&g_err_cnt, 0); g_err_cnt += 1000000; return NULL; }
    while (!g_go) ;
    unsigned seed = (unsigned)idx * 7919u + 1u;
    long ok = 0, err = 0, rchk = 0;
    for (long i = 0; i < g_nq; i++) {
        int id = (int)(i * 137) % 200000;
        char sql[96];
        int n = snprintf(sql, sizeof sql, "SELECT v FROM t WHERE id=%d", id);
        (void)n;
        int rows = 0;
        if (do_query(fd, sql, &rows) == 0) { ok++; if (rows == 1) rchk++; }
        else err++;
    }
    close(fd);
    __sync_fetch_and_add(&g_ok_cnt, ok);
    __sync_fetch_and_add(&g_err_cnt, err);
    __sync_fetch_and_add(&g_rows_chk, rchk);
    return NULL;
}

int main(int argc, char** argv) {
    setbuf(stdout, NULL);
    if (argc < 4) { fprintf(stderr, "usage: wcbench port threads nq [db]\n"); return 2; }
    g_port = atoi(argv[1]);
    g_threads = atoi(argv[2]);
    g_nq = atol(argv[3]);
    const char* db = argc > 4 ? argv[4] : "wdb";
    g_db = db;

    /* 先在主线程验证可用 */
    {
        struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)g_port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(fd, (struct sockaddr*)&sa, sizeof sa) != 0) { perror("connect"); return 1; }
        int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); /* 否则 Nagle+延迟ACK 每条 40ms */
        if (do_handshake(fd) != 0) { fprintf(stderr, "handshake fail\n"); return 1; }
        char use[64]; snprintf(use, sizeof use, "USE %s", db);
        int rc = do_query(fd, use, NULL);
        if (rc != 0) { fprintf(stderr, "USE fail rc=%d\n", rc); return 1; }
        int rows = 0;
        double t0 = now_s();
        for (int i = 0; i < 1000; i++) {
            char sql[96];
            snprintf(sql, sizeof sql, "SELECT v FROM t WHERE id=%d", i * 137 % 200000);
            if (i % 50 == 0) fprintf(stderr, "[wc] probe i=%d\n", i);
            if (do_query(fd, sql, &rows) != 0 || rows != 1) {
                fprintf(stderr, "probe query fail rows=%d i=%d\n", rows, i); return 1;
            }
        }
        double dt = now_s() - t0;
        printf("SINGLE_CONN n=1000 total=%.4fs qps=%.0f lat_us=%.1f\n", dt, 1000 / dt, dt * 1e6 / 1000);
        close(fd);
    }

    pthread_t* th = (pthread_t*)calloc((size_t)g_threads, sizeof(pthread_t));
    for (long i = 0; i < g_threads; i++) pthread_create(&th[i], NULL, worker, (void*)i);
    sleep(1); /* 连接与 USE */
    double t0 = now_s();
    g_go = 1;
    for (int i = 0; i < g_threads; i++) pthread_join(th[i], NULL);
    double dt = now_s() - t0;
    long tot = g_ok_cnt;
    printf("MULTI threads=%d nq=%ld ok=%ld err=%ld rows1=%ld total=%.3fs qps=%.0f\n",
           g_threads, g_nq * g_threads, g_ok_cnt, g_err_cnt, g_rows_chk, dt,
           dt > 0 ? (double)tot / dt : 0);
    free(th);
    return g_err_cnt == 0 ? 0 : 1;
}
