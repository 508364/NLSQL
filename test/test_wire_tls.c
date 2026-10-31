/**
 * @file test_wire_tls.c
 * @brief MySQL 传输协议 TLS 回环测试：SSLRequest → TLS1.2 → 认证 → 查询
 *
 * 依赖：OpenSSL 客户端库（构建期）+ NetLeaf TLS 扩展（运行期，NLSQL_TLS_LIB
 * 指定路径或按标准库名查找）。任一缺失时返回 77，由 CTest 记为 skipped。
 */
#include "netleaf_nlsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "netleaf_nlsql_platform.h"
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include <openssl/ssl.h>

extern void nlsha1(const void* data, uint32_t n, uint8_t out[20]);

#define SKIP 77

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

/* ---------- 明文侧 ---------- */
static int rd_n(int fd, void* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char*)buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int rd_packet(int fd, uint8_t** out, size_t* len) {
    *out = NULL; *len = 0;
    uint8_t hdr[4];
    if (rd_n(fd, hdr, 4) != 0) return -1;
    size_t n = (size_t)hdr[0] | ((size_t)hdr[1] << 8) | ((size_t)hdr[2] << 16);
    uint8_t* p = (uint8_t*)malloc(n + 1);
    if (!p) return -1;
    if (n && rd_n(fd, p, n) != 0) { free(p); return -1; }
    p[n] = '\0';
    *out = p; *len = n;
    return 0;
}

static int wr_packet(int fd, uint8_t seq, const uint8_t* payload, size_t len) {
    uint8_t hdr[4] = { (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), seq };
    if (write(fd, hdr, 4) != 4) return -1;
    if (len && write(fd, payload, (ssize_t)len) != (ssize_t)len) return -1;
    return 0;
}

/* ---------- TLS 侧 ---------- */
static int ssl_rd_n(SSL* s, void* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = SSL_read(s, (char*)buf + got, (int)(n - got));
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int ssl_rd_packet(SSL* s, uint8_t** out, size_t* len) {
    *out = NULL; *len = 0;
    uint8_t hdr[4];
    if (ssl_rd_n(s, hdr, 4) != 0) return -1;
    size_t n = (size_t)hdr[0] | ((size_t)hdr[1] << 8) | ((size_t)hdr[2] << 16);
    uint8_t* p = (uint8_t*)malloc(n + 1);
    if (!p) return -1;
    if (n && ssl_rd_n(s, p, n) != 0) { free(p); return -1; }
    p[n] = '\0';
    *out = p; *len = n;
    return 0;
}

static int ssl_wr_packet(SSL* s, uint8_t seq, const uint8_t* payload, size_t len) {
    uint8_t hdr[4] = { (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), seq };
    if (SSL_write(s, hdr, 4) != 4) return -1;
    if (len && SSL_write(s, payload, (int)len) != (int)len) return -1;
    return 0;
}

static int contains(const uint8_t* buf, size_t n, const char* needle) {
    size_t m = strlen(needle);
    if (m == 0 || n < m) return 0;
    for (size_t i = 0; i + m <= n; i++)
        if (memcmp(buf + i, needle, m) == 0) return 1;
    return 0;
}

#define CAPS_CLIENT (0x00008000u | 0x00000200u | 0x00000800u | 0x00000008u | 0x00000001u)

/* 读握手包并取 scramble；成功返回 0 */
static int read_scramble(int fd, uint8_t scram[20]) {
    uint8_t* p = NULL; size_t len = 0;
    if (rd_packet(fd, &p, &len) != 0 || len < 4 || p[0] != 10) return -1;
    size_t vo = 1;
    while (vo < len && p[vo]) vo++;
    vo++;          /* 版本串 NUL */
    vo += 4;       /* 连接 id */
    memcpy(scram, p + vo, 8);
    vo += 8 + 1 + 2 + 1 + 2 + 2 + 1 + 10;
    memcpy(scram + 8, p + vo, 12);
    free(p);
    return 0;
}

/* 明文发送 SSLRequest（seq=1，32 字节） */
static int send_ssl_request(int fd) {
    uint8_t req[32];
    memset(req, 0, sizeof(req));
    req[0] = CAPS_CLIENT & 0xFF; req[1] = (CAPS_CLIENT >> 8) & 0xFF;
    req[2] = (CAPS_CLIENT >> 16) & 0xFF; req[3] = (CAPS_CLIENT >> 24) & 0xFF;
    req[4] = 0; req[5] = 0; req[6] = 0x40; req[7] = 0; /* max packet 4MB */
    req[8] = 33;                                      /* charset */
    return wr_packet(fd, 1, req, sizeof(req));
}

/* TLS 已就绪后发送登录响应（seq=2）并读 OK */
static int login_over_tls(SSL* ssl, const char* db, const uint8_t scram[20]) {
    uint8_t s1[20], s2[20], token[20];
    nlsha1("", 0, s1);
    nlsha1(s1, 20, s2);
    uint8_t cat[40], hsh[20];
    memcpy(cat, scram, 20);
    memcpy(cat + 20, s2, 20);
    nlsha1(cat, 40, hsh);
    for (int i = 0; i < 20; i++) token[i] = hsh[i] ^ s1[i];

    uint8_t resp[256];
    size_t o = 0, dbl = strlen(db);
    resp[o++] = CAPS_CLIENT & 0xFF; resp[o++] = (CAPS_CLIENT >> 8) & 0xFF;
    resp[o++] = (CAPS_CLIENT >> 16) & 0xFF; resp[o++] = (CAPS_CLIENT >> 24) & 0xFF;
    resp[o++] = 0; resp[o++] = 0; resp[o++] = 0x40; resp[o++] = 0;
    resp[o++] = 33;
    memset(resp + o, 0, 23); o += 23;
    memcpy(resp + o, "root", 4); o += 5;
    memcpy(resp + o, db, dbl + 1); o += dbl + 1;
    resp[o++] = 20;
    memcpy(resp + o, token, 20); o += 20;
    if (ssl_wr_packet(ssl, 2, resp, o) != 0) return -1;

    uint8_t* p = NULL; size_t len = 0;
    if (ssl_rd_packet(ssl, &p, &len) != 0) return -1;
    int ok = (len > 0 && p[0] == 0x00);
    free(p);
    return ok ? 0 : -1;
}

static int tls_query_ok(SSL* ssl, uint8_t seq, const char* sql, const char* expect) {
    uint8_t* buf = (uint8_t*)malloc(strlen(sql) + 1);
    if (!buf) return -1;
    buf[0] = 0x03;
    memcpy(buf + 1, sql, strlen(sql));
    if (ssl_wr_packet(ssl, seq, buf, strlen(sql) + 1) != 0) { free(buf); return -1; }
    free(buf);

    uint8_t* p = NULL; size_t len = 0;
    if (ssl_rd_packet(ssl, &p, &len) != 0 || len == 0) return -1;
    if (p[0] == 0xFF || p[0] >= 0xFB) { free(p); return -1; } /* ERR 或非法列数 */
    uint64_t ncols = p[0];
    free(p);
    for (uint64_t i = 0; i < ncols; i++) {
        if (ssl_rd_packet(ssl, &p, &len) != 0) return -1;
        free(p);
    }
    if (ssl_rd_packet(ssl, &p, &len) != 0) return -1;
    int found = 0;
    if (p[0] == 0xFE) {          /* 列定义后的 EOF，再读行 */
        free(p);
        if (ssl_rd_packet(ssl, &p, &len) != 0) return -1;
    }
    if (contains(p, len, expect)) found = 1;
    free(p);
    if (ssl_rd_packet(ssl, &p, &len) != 0) return -1;  /* 行后 EOF */
    free(p);
    return found ? 0 : -1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 1) TLS 扩展可用性（缺失则跳过） */
    const char* env = getenv("NLSQL_TLS_LIB");
    void* probe = NULL;
    if (env && env[0]) probe = nlsql_dl_open(env, RTLD_NOW | RTLD_LOCAL);
    if (!probe) probe = nlsql_dl_open("libnetleaf_tls3.so", RTLD_NOW | RTLD_LOCAL);
    if (!probe) probe = nlsql_dl_open("libnetleaf_tls2.so", RTLD_NOW | RTLD_LOCAL);
    if (!probe) { printf("SKIP: NetLeaf TLS extension not found\n"); return SKIP; }
    nlsql_dl_close(probe);

    /* 2) 运行期生成自签证书 */
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/nlsql_tls_%d_%llu", (int)getpid(),
             (unsigned long long)nlsql_mono_ms());
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", dir);
    if (system(cmd) != 0) { printf("SKIP: mkdir failed\n"); return SKIP; }
    char crt[300], key[300];
    snprintf(crt, sizeof(crt), "%s/server.crt", dir);
    snprintf(key, sizeof(key), "%s/server.key", dir);
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -nodes -keyout %s -out %s "
             "-days 2 -subj /CN=localhost >/dev/null 2>&1", key, crt);
    if (system(cmd) != 0) { printf("SKIP: openssl not available\n"); return SKIP; }

    /* 3) 启动 TLS 服务端 */
    char data[300];
    snprintf(data, sizeof(data), "%s/data", dir);
    nlsql_conn_t* c = nlsql_open(data);
    if (!c) { printf("open failed: %s\n", nlsql_global_error()); return 1; }
    nlsql_query(c, "CREATE DATABASE w", NULL);
    nlsql_query(c, "USE w", NULL);
    nlsql_query(c, "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16))", NULL);
    nlsql_query(c, "INSERT INTO t VALUES (1, 'hello')", NULL);

    int port = 36000 + (getpid() % 1000);
    int h = nlsql_wire_start_tls(c, port, 4, crt, key);
    CHECK(h > 0, "wire_start_tls");
    if (h <= 0) {
        printf("  err: %s\n", nlsql_global_error());
        nlsql_close(c);
        return 1;
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    CHECK(ctx != NULL, "SSL_CTX_new");
    if (!ctx) { nlsql_wire_stop(h); nlsql_close(c); return 1; }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* 4) TLS1.2 全链路 */
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0, "connect");

        uint8_t scram[20];
        CHECK(read_scramble(fd, scram) == 0, "read handshake v10");
        CHECK(send_ssl_request(fd) == 0, "send SSLRequest plaintext");

        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        CHECK(SSL_connect(ssl) == 1, "TLS handshake after SSLRequest");
        CHECK(SSL_version(ssl) == TLS1_2_VERSION, "negotiated TLSv1.2");
        CHECK(login_over_tls(ssl, "w", scram) == 0, "login over TLS");
        CHECK(tls_query_ok(ssl, 3, "SELECT @@version", "8.0.36-NLSQL") == 0, "query @@version over TLS");
        CHECK(tls_query_ok(ssl, 4, "SHOW FULL COLUMNS FROM t", "utf8_general_ci") == 0, "SHOW FULL COLUMNS over TLS");

        uint8_t quit = 0x01;
        ssl_wr_packet(ssl, 5, &quit, 1);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(fd);
    }

    /* 5) 低于 TLS1.2 的客户端应被拒（服务端强制 1.2） */
    {
        SSL_CTX* old = SSL_CTX_new(TLS_client_method());
        if (old) {
            SSL_CTX_set_verify(old, SSL_VERIFY_NONE, NULL);
            SSL_CTX_set_min_proto_version(old, TLS1_1_VERSION);
            SSL_CTX_set_max_proto_version(old, TLS1_1_VERSION);
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0) {
                uint8_t scram[20];
                read_scramble(fd, scram);
                send_ssl_request(fd);
                SSL* ssl = SSL_new(old);
                SSL_set_fd(ssl, fd);
                CHECK(SSL_connect(ssl) != 1, "TLS1.1 rejected");
                SSL_free(ssl);
            }
            close(fd);
            SSL_CTX_free(old);
        }
    }

    SSL_CTX_free(ctx);
    nlsql_wire_stop(h);
    nlsql_close(c);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) printf("  cleanup failed: %s\n", cmd);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
