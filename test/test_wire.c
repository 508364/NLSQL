/**
 * @file test_wire.c
 * @brief MySQL 传输协议回环测试：原生 socket 走握手/认证/查询/结果集
 */
#include "netleaf_nlsql.h"
#include "netleaf_nlsql_platform.h"
#include "nlsql_test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include <signal.h>

/* 复用库内 SHA1（mysql_native_password 客户端侧计算） */
extern void nlsha1(const void* data, uint32_t n, uint8_t out[20]);

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

static size_t read_lenenc(const uint8_t* p, uint64_t* v) {
    if (p[0] < 0xFB) { *v = p[0]; return 1; }
    if (p[0] == 0xFC) { *v = p[1] | ((uint64_t)p[2] << 8); return 3; }
    if (p[0] == 0xFD) { *v = p[1] | ((uint64_t)p[2] << 8) | ((uint64_t)p[3] << 16) | ((uint64_t)p[4] << 24); return 5; }
    *v = 0; return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    char dir[256];
    test_dir(dir, sizeof(dir), "nlsql_wire");
    nlsql_conn_t* c = nlsql_open(dir);
    if (!c) { printf("open failed: %s\n", nlsql_global_error()); return 1; }
    nlsql_query(c, "CREATE DATABASE w", NULL);
    nlsql_query(c, "USE w", NULL);
    nlsql_query(c, "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16))", NULL);
    nlsql_query(c, "INSERT INTO t VALUES (1, 'hello'), (2, NULL)", NULL);

    int port = 34000 + (getpid() % 1000);
    int h = nlsql_wire_start(c, port, 4);
    CHECK(h > 0, "wire_start");
    if (h <= 0) { printf("  wire err: %s\n", nlsql_global_error()); nlsql_close(c); return 1; }

    /* 客户端连接 */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0, "connect 127.0.0.1");

    /* 1) 读握手包 */
    uint8_t* p = NULL; size_t len = 0;
    CHECK(test_rd_packet(fd, &p, &len) == 0 && len > 4 && p[0] == 10, "handshake v10");
    if (!p || len <= 4 || p[0] != 10) { /* 握手失败：不要继续解引用 NULL 包 */
        free(p);
        nlsql_sock_close(fd);
        nlsql_wire_stop(h);
        nlsql_close(c);
        printf("\n%d passed, %d failed\n", g_pass, g_fail);
        return 1;
    }
    uint8_t scram[20];
    {
        /* 动态定位：1(ver) + strlen(ver)+1 + 4(connid) → scram[8] */
        size_t vo = 1;
        while (vo < len && p[vo]) vo++;
        vo++; /* NUL */
        vo += 4; /* conn id */
        memcpy(scram, p + vo, 8);
        vo += 8 + 1 + 2 + 1 + 2 + 2 + 1 + 10; /* filler/caps/charset/status/caps/authlen/reserved */
        memcpy(scram + 8, p + vo, 12);
    }
    free(p);

    /* 2) 握手响应：root/空密码，token = SHA1(scram) ^ SHA1(SHA1("")) */
    uint8_t s1[20], s2[20], token[20];
    nlsha1("", 0, s1);          /* SHA1("") */
    nlsha1(s1, 20, s2);         /* SHA1(SHA1("")) */
    {
        /* token = SHA1(pass) XOR SHA1(scram || SHA1(SHA1(pass)))；pass="" → SHA1(pass)=s1, stored=s2 */
        uint8_t cat[40];
        memcpy(cat, scram, 20);
        memcpy(cat + 20, s2, 20);
        uint8_t hsh[20];
        nlsha1(cat, 40, hsh);
        for (int i = 0; i < 20; i++) token[i] = hsh[i] ^ s1[i];
    }
    {
        uint8_t resp[256];
        size_t o = 0;
        uint32_t caps = 0x00008208u | 0x00000200u; /* PROTOCOL_41|SECURE? see below */
        caps = 0x00008000u | 0x00000200u | 0x00000008u | 0x00000001u; /* SECURE|PROTO41|CONNECT_DB|LONG_PASSWORD */
        resp[o++] = caps & 0xFF; resp[o++] = (caps >> 8) & 0xFF; resp[o++] = (caps >> 16) & 0xFF; resp[o++] = (caps >> 24) & 0xFF;
        resp[o++] = 0; resp[o++] = 0; resp[o++] = 0x40; resp[o++] = 0; /* maxpacket 4MB */
        resp[o++] = 33; /* charset */
        memset(resp + o, 0, 23); o += 23;
        memcpy(resp + o, "root\0", 5); o += 5;
        memcpy(resp + o, "w\0", 2); o += 2; /* database */
        resp[o++] = 20;
        memcpy(resp + o, token, 20); o += 20;
        CHECK(test_wr_packet(fd, 1, resp, o) == 0, "send handshake response");
    }

    /* 3) 认证结果应为 OK(0x00) */
    CHECK(test_rd_packet(fd, &p, &len) == 0 && len > 0 && p[0] == 0x00, "auth OK");
    free(p);

    /* 4) COM_PING */
    {
        uint8_t cmd = 0x0E;
        CHECK(test_wr_packet(fd, 2, &cmd, 1) == 0, "send ping");
        CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0x00, "ping OK");
        free(p);
    }

    /* 5) COM_QUERY SELECT */
    {
        const char* sql = "SELECT id, name FROM t ORDER BY id";
        uint8_t* buf = (uint8_t*)malloc(strlen(sql) + 1);
        buf[0] = 0x03;
        memcpy(buf + 1, sql, strlen(sql));
        CHECK(test_wr_packet(fd, 3, buf, strlen(sql) + 1) == 0, "send query");
        free(buf);

        /* 列数 */
        int rc_cc = test_rd_packet(fd, &p, &len);
        CHECK(rc_cc == 0 && p != NULL && p[0] == 0x02, "column count 2");
        if (rc_cc != 0 || !p) { nlsql_sock_close(fd); nlsql_wire_stop(h); nlsql_close(c); return 1; }
        uint64_t ncols = p[0];
        free(p);
        /* 列定义 xN */
        for (uint64_t i = 0; i < ncols; i++) {
            CHECK(test_rd_packet(fd, &p, &len) == 0, "column def");
            free(p);
        }
        /* EOF */
        CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0xFE, "EOF after columns");
        free(p);
        /* 行 1: id=1 name=hello */
        CHECK(test_rd_packet(fd, &p, &len) == 0, "row1");
        int ok1 = 0;
        if (len > 0) {
            uint64_t l1, l2;
            size_t u1 = read_lenenc(p, &l1);
            if (u1 && l1 == 1 && p[u1] == '1') {
                size_t u2 = read_lenenc(p + u1 + l1, &l2);
                if (u2 && l2 == 5 && memcmp(p + u1 + l1 + u2, "hello", 5) == 0) ok1 = 1;
            }
        }
        CHECK(ok1, "row1 values");
        free(p);
        /* 行 2: id=2 name=NULL */
        CHECK(test_rd_packet(fd, &p, &len) == 0, "row2");
        int ok2 = 0;
        if (len >= 3 && p[0] == 1 && p[1] == '2' && p[2] == 0xFB) ok2 = 1;
        CHECK(ok2, "row2 NULL via 0xFB");
        free(p);
        /* EOF */
        CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0xFE, "EOF after rows");
        free(p);
    }

    /* 6) 错误语句矩阵 → ERR 包带 errno + MySQL 标准 '#sqlstate'（客户端可读性） */
    {
        struct { const char* sql; int code; const char* state; } cases[] = {
            { "SELECT * FROM nope",      1146, "#42S02" }, /* unknown table */
            { "USE no_such_db",          1049, "#42000" }, /* unknown database */
            { "SELEC 1",                 1064, "#42000" }, /* syntax */
        };
        for (int ci = 0; ci < 3; ci++) {
            const char* sql = cases[ci].sql;
            uint8_t* buf = (uint8_t*)malloc(strlen(sql) + 1);
            buf[0] = 0x03;
            memcpy(buf + 1, sql, strlen(sql));
            test_wr_packet(fd, (uint8_t)(4 + ci), buf, strlen(sql) + 1);
            free(buf);
            CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0xFF, "ERR packet");
            int errcode = 0;
            if (len >= 3) errcode = p[1] | (p[2] << 8);
            CHECK(errcode == cases[ci].code, "errno over wire");
            /* MySQL ERR 文本区首字节 '#'+5 字节 sqlstate */
            CHECK(len >= 9 && memcmp(p + 3, cases[ci].state, 6) == 0, "sqlstate over wire");
            free(p);
        }
        /* 1062 重复主键：走插入成功序列 + 冲突（seq 7、8） */
        {
            const char* sql = "INSERT INTO t VALUES (9999, 'dup')";
            uint8_t* buf = (uint8_t*)malloc(strlen(sql) + 1);
            buf[0] = 0x03; memcpy(buf + 1, sql, strlen(sql));
            test_wr_packet(fd, 7, buf, strlen(sql) + 1);
            free(buf);
            CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0x00, "insert t2 OK");
            free(p);
            buf = (uint8_t*)malloc(strlen(sql) + 1);
            buf[0] = 0x03; memcpy(buf + 1, sql, strlen(sql));
            test_wr_packet(fd, 8, buf, strlen(sql) + 1);
            free(buf);
            CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0xFF, "dup ERR packet");
            int errcode = 0;
            if (len >= 3) errcode = p[1] | (p[2] << 8);
            CHECK(errcode == 1062, "errno 1062 dup over wire");
            CHECK(len >= 9 && memcmp(p + 3, "#23000", 6) == 0, "sqlstate 23000 dup");
            free(p);
        }
    }

    /* 7) COM_QUIT */
    {
        uint8_t cmd = 0x01;
        test_wr_packet(fd, 9, &cmd, 1);
    }
    nlsql_sock_close(fd);
    nlsql_wire_stop(h);

    /* 错误密码应被拒 */
    nlsql_set_credentials(c, "app", "secret123");
    int h2 = nlsql_wire_start(c, port + 1, 2);
    CHECK(h2 > 0, "wire_start with creds");
    if (h2 > 0) {
        int fd2 = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa2 = sa;
        sa2.sin_port = htons((uint16_t)(port + 1));
        if (connect(fd2, (struct sockaddr*)&sa2, sizeof(sa2)) == 0) {
            uint8_t* q = NULL; size_t ql = 0;
            test_rd_packet(fd2, &q, &ql);
            /* 直接发一个错误 token 的响应 */
            uint8_t resp[128];
            size_t o = 0;
            uint32_t caps = 0x00008000u | 0x00000200u | 0x00000001u;
            resp[o++] = caps & 0xFF; resp[o++] = 0; resp[o++] = 0; resp[o++] = 0;
            resp[o++] = 0; resp[o++] = 0; resp[o++] = 0x40; resp[o++] = 0;
            resp[o++] = 33;
            memset(resp + o, 0, 23); o += 23;
            memcpy(resp + o, "app\0", 4); o += 4;
            resp[o++] = 20;
            memset(resp + o, 0xAB, 20); o += 20;
            test_wr_packet(fd2, 1, resp, o);
            uint8_t* r2 = NULL; size_t l2 = 0;
            CHECK(test_rd_packet(fd2, &r2, &l2) == 0 && r2[0] == 0xFF, "wrong password denied");
            free(r2);
            free(q);
        }
        nlsql_sock_close(fd2);
        nlsql_wire_stop(h2);
    }

    nlsql_close(c);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
