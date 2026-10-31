/**
 * @file test_ext_base.c
 * @brief base 扩展测试：时间函数改写结果值 + COM_STMT_* 服务端预处理二进制结果
 *
 * 断言全部校验实际值（日期形态/时间范围/行值/错误码），而非仅返回码。
 * 扩展以 dlopen 方式加载，与宿主 NetLeaf 的插件加载路径一致。
 */
#include "netleaf_nlsql.h"
#include "netleaf_nlsql_ext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include "netleaf_nlsql_platform.h"
#include "nlsql_test_util.h"
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

extern void nlsha1(const void* data, uint32_t n, uint8_t out[20]);

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

static void* load_ext(const char* name, const char* init_sym, int (**init_fn)(void)) {
    void* h = test_ext_open(name);
    if (!h) return NULL;
    *init_fn = (int (*)(void))nlsql_dl_sym(h, init_sym);
    if (!*init_fn) { printf("  dlsym %s: %s\n", init_sym, nlsql_dl_error()); nlsql_dl_close(h); return NULL; }
    return h;
}

/* 发一条语句并读回响应包；用于 stmt_prepare 中跳过多余的结果包 */
static int skip_packet(int fd) {
    uint8_t* p = NULL; size_t len = 0;
    if (test_rd_packet(fd, &p, &len) != 0) return -1;
    free(p);
    return 0;
}

/* ---------------- 最小 MySQL 协议客户端 ---------------- */

/* 握手连接；成功返回 fd，失败 -1 */
static int do_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) { nlsql_sock_close(fd); return -1; }
    uint8_t* p = NULL; size_t len = 0;
    if (test_rd_packet(fd, &p, &len) != 0 || len < 40 || p[0] != 10) { free(p); nlsql_sock_close(fd); return -1; }
    uint8_t scram[20];
    size_t vo = 1;
    while (vo < len && p[vo]) vo++;
    vo++; vo += 4;
    memcpy(scram, p + vo, 8);
    vo += 8 + 1 + 2 + 1 + 2 + 2 + 1 + 10;
    memcpy(scram + 8, p + vo, 12);
    free(p);
    uint8_t s1[20], s2[20], token[20], cat[40], hsh[20];
    nlsha1("", 0, s1);
    nlsha1(s1, 20, s2);
    memcpy(cat, scram, 20);
    memcpy(cat + 20, s2, 20);
    nlsha1(cat, 40, hsh);
    for (int i = 0; i < 20; i++) token[i] = hsh[i] ^ s1[i];
    uint8_t resp[256];
    size_t o = 0;
    uint32_t caps = 0x00008000u | 0x00000200u | 0x00000008u | 0x00000001u;
    resp[o++] = caps & 0xFF; resp[o++] = (caps >> 8) & 0xFF;
    resp[o++] = (caps >> 16) & 0xFF; resp[o++] = (caps >> 24) & 0xFF;
    resp[o++] = 0; resp[o++] = 0; resp[o++] = 0x40; resp[o++] = 0;
    resp[o++] = 33;
    memset(resp + o, 0, 23); o += 23;
    memcpy(resp + o, "root\0", 5); o += 5;
    memcpy(resp + o, "w\0", 2); o += 2;
    resp[o++] = 20;
    memcpy(resp + o, token, 20); o += 20;
    if (test_wr_packet(fd, 1, resp, o) != 0) { nlsql_sock_close(fd); return -1; }
    if (test_rd_packet(fd, &p, &len) != 0 || len < 1 || p[0] != 0x00) { free(p); nlsql_sock_close(fd); return -1; }
    free(p);
    return fd;
}

/* 发送 COM_STMT_PREPARE，读回全部响应；*stmt_id 输出语句号，返回 0=成功 */
static int stmt_prepare(int fd, const char* sql, uint32_t* stmt_id) {
    size_t sl = strlen(sql);
    uint8_t* buf = (uint8_t*)malloc(sl + 1);
    if (!buf) return -1;
    buf[0] = 0x16;
    memcpy(buf + 1, sql, sl);
    int bad = test_wr_packet(fd, 2, buf, sl + 1);
    free(buf);
    if (bad != 0) return -1;
    uint8_t* p = NULL; size_t len = 0;
    if (test_rd_packet(fd, &p, &len) != 0) return -1;
    if (p[0] == 0xFF || len < 12) { free(p); return -1; }
    /* 布局：0x00 | stmt_id(4) | num_columns(2) | num_params(2) | filler | warnings(2) */
    *stmt_id = (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
    int nparams = p[7] | (p[8] << 8);
    free(p);
    for (int i = 0; i < nparams; i++) {
        if (skip_packet(fd) != 0) return -1;
    }
    if (nparams > 0) {
        if (skip_packet(fd) != 0) return -1;
    }
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int (*ext_init)(void) = NULL;
    void* ext = load_ext("nlsql_ext_base", "nlsql_ext_base_init", &ext_init);
    CHECK(ext != NULL && ext_init != NULL, "load nlsql_ext_base plugin");
    if (!ext || !ext_init) { printf("\n%d passed, %d failed\n", g_pass, g_fail); return g_fail ? 1 : 0; }

    char ddir[256];
    test_dir(ddir, sizeof(ddir), "nlsql_ext_base");
    nlsql_conn_t* c = nlsql_open(ddir);
    CHECK(c != NULL, "open connection");
    if (!c) { printf("\n%d passed, %d failed\n", g_pass, g_fail); return g_fail ? 1 : 0; }
    CHECK(nlsql_query(c, "CREATE DATABASE w", NULL) == NLSQL_OK, "create database w");
    CHECK(nlsql_query(c, "USE w", NULL) == NLSQL_OK, "use w");
    CHECK(nlsql_query(c, "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16))", NULL) == NLSQL_OK, "create table t");
    CHECK(nlsql_query(c, "INSERT INTO t VALUES (1, 'alice'), (2, 'bob')", NULL) == NLSQL_OK, "insert rows");

    int port = 36000 + (getpid() % 500);
    int h = nlsql_wire_start(c, port, 4);
    CHECK(h > 0, "wire_start");
    int fd = (h > 0) ? do_connect(port) : -1;
    CHECK(fd >= 0, "client handshake");
    if (h <= 0 || fd < 0) { if (fd >= 0) nlsql_sock_close(fd); if (h > 0) nlsql_wire_stop(h); nlsql_close(c); return 1; }

    /* 扩展未启用：COM_STMT_PREPARE 必须被拒（1235 = 不支持） */
    {
        const char* sql = "SELECT name FROM t WHERE id = ?";
        size_t sl = strlen(sql);
        uint8_t* buf = (uint8_t*)malloc(sl + 1);
        buf[0] = 0x16; memcpy(buf + 1, sql, sl);
        test_wr_packet(fd, 2, buf, sl + 1);
        free(buf);
        uint8_t* p = NULL; size_t len = 0;
        CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0xFF, "prepare rejected before extension");
        int code = (p && len >= 3) ? (p[1] | (p[2] << 8)) : 0;
        CHECK(code == NLSQL_ER_NOT_SUPPORTED, "reject errno 1235 before extension");
        free(p);
    }

    CHECK(ext_init() == 0, "nlsql_ext_base_init");
    CHECK(nlsql_ext_feature("server_prepare") == 1, "feature server_prepare enabled");

    /* 时间函数：值形态与范围 */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT NOW()", &r) == NLSQL_OK && r && r->row_count == 1, "select now()");
        if (r && r->row_count == 1) {
            const char* s = r->rows[0].vals[0].s;
            CHECK(s && strlen(s) == 19 && s[4] == '-' && s[7] == '-' && s[10] == ' ' &&
                  s[13] == ':' && s[16] == ':', "now() format YYYY-MM-DD HH:MM:SS");
            time_t now = time(NULL);
            struct tm tmv;
            localtime_r(&now, &tmv);
            char expect[32];
            strftime(expect, sizeof(expect), "%Y-%m-%d %H", &tmv);
            CHECK(s && strncmp(s, expect, 13) == 0, "now() matches current local hour");
        }
        nlsql_free_result(r); r = NULL;

        CHECK(nlsql_query(c, "SELECT CURDATE()", &r) == NLSQL_OK && r && r->row_count == 1, "select curdate()");
        if (r && r->row_count == 1) {
            time_t now = time(NULL);
            struct tm tmv;
            localtime_r(&now, &tmv);
            char expect[16];
            strftime(expect, sizeof(expect), "%Y-%m-%d", &tmv);
            CHECK(r->rows[0].vals[0].s && strcmp(r->rows[0].vals[0].s, expect) == 0,
                  "curdate() equals today's local date");
        }
        nlsql_free_result(r); r = NULL;

        CHECK(nlsql_query(c, "SELECT CURTIME()", &r) == NLSQL_OK && r && r->row_count == 1, "select curtime()");
        if (r && r->row_count == 1) {
            const char* s = r->rows[0].vals[0].s;
            CHECK(s && strlen(s) == 8 && s[2] == ':' && s[5] == ':', "curtime() format HH:MM:SS");
        }
        nlsql_free_result(r); r = NULL;

        CHECK(nlsql_query(c, "SELECT UNIX_TIMESTAMP()", &r) == NLSQL_OK && r && r->row_count == 1, "select unix_timestamp()");
        if (r && r->row_count == 1) {
            long long v = (long long)r->rows[0].vals[0].i;
            long long now = (long long)time(NULL);
            CHECK(v >= now - 5 && v <= now + 5, "unix_timestamp() within +-5s of system time");
        }
        nlsql_free_result(r); r = NULL;

        CHECK(nlsql_query(c, "SELECT FROM_UNIXTIME(0)", &r) == NLSQL_OK && r && r->row_count == 1, "select from_unixtime(0)");
        if (r && r->row_count == 1) {
            time_t z = 0;
            struct tm tmv;
            localtime_r(&z, &tmv);
            char expect[32];
            strftime(expect, sizeof(expect), "%Y-%m-%d %H:%M:%S", &tmv);
            CHECK(r->rows[0].vals[0].s && strcmp(r->rows[0].vals[0].s, expect) == 0,
                  "from_unixtime(0) equals local epoch");
        }
        nlsql_free_result(r); r = NULL;

        /* 字符串里的 NOW 不应被改写（改写器跳过引号内容） */
        CHECK(nlsql_query(c, "SELECT 'NOW()'", &r) == NLSQL_OK && r && r->row_count == 1, "select literal 'NOW()'");
        if (r && r->row_count == 1)
            CHECK(r->rows[0].vals[0].s && strcmp(r->rows[0].vals[0].s, "NOW()") == 0,
                  "quoted NOW() stays literal");
        nlsql_free_result(r);
    }

    /* 服务端预处理：prepare → execute(int64 参数) → 二进制结果集行值 */
    {
        uint32_t sid = 0;
        CHECK(stmt_prepare(fd, "SELECT name FROM t WHERE id = ?", &sid) == 0, "COM_STMT_PREPARE ok");
        uint8_t buf[64];
        size_t o = 0;
        buf[o++] = 0x17;                 /* COM_STMT_EXECUTE */
        buf[o++] = (uint8_t)(sid & 0xFF); buf[o++] = (uint8_t)((sid >> 8) & 0xFF);
        buf[o++] = (uint8_t)((sid >> 16) & 0xFF); buf[o++] = (uint8_t)((sid >> 24) & 0xFF);
        buf[o++] = 0;                    /* flags: CURSOR_TYPE_NO_CURSOR */
        buf[o++] = 1; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0; /* iteration_count=1 */
        buf[o++] = 0;                    /* null bitmap (1 param) */
        buf[o++] = 1;                    /* new_params_bound */
        buf[o++] = 0x08; buf[o++] = 0x00;/* LONGLONG */
        buf[o++] = 2; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;
        buf[o++] = 0; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;
        CHECK(test_wr_packet(fd, 3, buf, o) == 0, "send COM_STMT_EXECUTE");

        uint8_t* p = NULL; size_t len = 0;
        CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0x01, "binary resultset column count 1");
        free(p);
        /* 列定义：catalog 为 lenenc "def"，即 p[0]=3、p[1..3]="def" */
        CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0x03 && memcmp(p + 1, "def", 3) == 0,
              "binary column def");
        free(p);
        CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0xFE, "EOF after columns");
        free(p);
        /* 二进制行：0x00 + null bitmap(1B) + lenenc "bob" */
        CHECK(test_rd_packet(fd, &p, &len) == 0 && len >= 3 && p[0] == 0x00, "binary row header");
        CHECK(len >= 5 && p[1] == 0x00 && p[2] == 0x03 && memcmp(p + 3, "bob", 3) == 0,
              "binary row value 'bob' for id=2");
        free(p);
        CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0xFE, "EOF after rows");
        free(p);

        /* 再次执行但不重发类型（new_params_bound=0）：值仍应正确 */
        o = 0;
        buf[o++] = 0x17;
        buf[o++] = (uint8_t)(sid & 0xFF); buf[o++] = (uint8_t)((sid >> 8) & 0xFF);
        buf[o++] = (uint8_t)((sid >> 16) & 0xFF); buf[o++] = (uint8_t)((sid >> 24) & 0xFF);
        buf[o++] = 0;
        buf[o++] = 1; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;
        buf[o++] = 0;                    /* null bitmap */
        buf[o++] = 0;                    /* 沿用上次类型 */
        buf[o++] = 1; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;
        buf[o++] = 0; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;
        test_wr_packet(fd, 4, buf, o);
        CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0x01, "second execute: column count");
        free(p);
        test_rd_packet(fd, &p, &len); free(p);
        CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0xFE, "second execute: EOF cols");
        free(p);
        CHECK(test_rd_packet(fd, &p, &len) == 0 && len >= 8 && p[2] == 0x05 &&
              memcmp(p + 3, "alice", 5) == 0,
              "second execute reuses bound type, returns 'alice' for id=1");
        free(p);
        CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0xFE, "second execute: EOF rows");
        free(p);

        /* 未知语句号 → 1243 */
        o = 0;
        buf[o++] = 0x17;
        buf[o++] = 0xEE; buf[o++] = 0xEE; buf[o++] = 0x00; buf[o++] = 0x00;
        buf[o++] = 0; buf[o++] = 1; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;
        test_wr_packet(fd, 5, buf, o);
        CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0xFF, "unknown stmt -> ERR");
        CHECK(len >= 3 && (p[1] | (p[2] << 8)) == NLSQL_ER_UNKNOWN_STMT, "unknown stmt errno 1243");
        free(p);

        /* COM_STMT_CLOSE 无响应；随后执行同 id 应 1243 */
        o = 0;
        buf[o++] = 0x19;
        buf[o++] = (uint8_t)(sid & 0xFF); buf[o++] = (uint8_t)((sid >> 8) & 0xFF);
        buf[o++] = (uint8_t)((sid >> 16) & 0xFF); buf[o++] = (uint8_t)((sid >> 24) & 0xFF);
        test_wr_packet(fd, 6, buf, o);
        o = 0;
        buf[o++] = 0x17;
        buf[o++] = (uint8_t)(sid & 0xFF); buf[o++] = (uint8_t)((sid >> 8) & 0xFF);
        buf[o++] = (uint8_t)((sid >> 16) & 0xFF); buf[o++] = (uint8_t)((sid >> 24) & 0xFF);
        buf[o++] = 0; buf[o++] = 1; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;
        test_wr_packet(fd, 7, buf, o);
        CHECK(test_rd_packet(fd, &p, &len) == 0 && p[0] == 0xFF &&
              len >= 3 && (p[1] | (p[2] << 8)) == NLSQL_ER_UNKNOWN_STMT,
              "execute after CLOSE -> 1243");
        free(p);
    }

    { uint8_t q = 0x01; test_wr_packet(fd, 8, &q, 1); } /* COM_QUIT */
    nlsql_sock_close(fd);
    nlsql_wire_stop(h);
    nlsql_close(c);
    nlsql_dl_close(ext);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
