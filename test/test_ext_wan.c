/**
 * @file test_ext_wan.c
 * @brief WAN 扩展测试：经 NetLeaf HTTP 服务端查询并校验响应正文
 *
 * 需要宿主 NetLeaf 动态库（NLSQL_NETLEAF_LIB）；缺失时返回 77（CTest 记为 skipped）。
 * 扩展本身不做 socket 编程，端口由测试传入，验证“经主库发到外网”的路径。
 */
#include "netleaf_nlsql.h"
#include "nlsql_test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

/* 向网关发一条 SQL，收完整响应（读到 EOF）；返回 0 成功 */
static int http_query(int port, const char* sql, char* out, size_t osz) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) { nlsql_sock_close(fd); return -1; }
    char req[4096];
    int n = snprintf(req, sizeof(req),
                     "POST /sql HTTP/1.1\r\n"
                     "Host: 127.0.0.1\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s",
                     strlen(sql), sql);
    if (test_wr_all(fd, req, (size_t)n) != 0) { nlsql_sock_close(fd); return -1; }
    shutdown(fd, SHUT_WR);
    size_t got = 0;
    for (;;) {
        if (got + 1 >= osz) break;
        ssize_t r = nlsql_sock_recv(fd, out + got, osz - got - 1);
        if (r <= 0) break;
        got += (size_t)r;
    }
    out[got] = '\0';
    nlsql_sock_close(fd);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    const char* nl_lib = getenv("NLSQL_NETLEAF_LIB");
    if (!nl_lib || !nl_lib[0]) {
        printf("SKIP: NLSQL_NETLEAF_LIB not set\n");
        return 77;
    }
    /* 宿主库以 GLOBAL 装载：扩展通过 nlsql_dl_sym(NULL) 取 nl_server_* 符号 */
    void* host = nlsql_dl_open(nl_lib, RTLD_NOW | RTLD_GLOBAL);
    if (!host) {
        printf("SKIP: cannot load NetLeaf library: %s\n", nlsql_dl_error());
        return 77;
    }

    void* ext = test_ext_open("nlsql_ext_wan");
    CHECK(ext != NULL, "load nlsql_ext_wan plugin");
    if (!ext) { printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
    int  (*wan_start)(nlsql_conn_t*, int) = (int (*)(nlsql_conn_t*, int))nlsql_dl_sym(ext, "nlsql_ext_wan_start");
    void (*wan_stop)(void) = (void (*)(void))nlsql_dl_sym(ext, "nlsql_ext_wan_stop");
    int  (*wan_running)(void) = (int (*)(void))nlsql_dl_sym(ext, "nlsql_ext_wan_running");
    CHECK(wan_start && wan_stop && wan_running, "resolve wan API symbols");

    char ddir[256];
    test_dir(ddir, sizeof(ddir), "nlsql_ext_wan");
    nlsql_conn_t* c = nlsql_open(ddir);
    CHECK(c != NULL, "open connection");
    if (!c || !wan_start) { printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
    CHECK(nlsql_query(c, "CREATE DATABASE w", NULL) == NLSQL_OK, "create database w");
    CHECK(nlsql_query(c, "USE w", NULL) == NLSQL_OK, "use w");
    CHECK(nlsql_query(c, "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16))", NULL) == NLSQL_OK, "create table t");
    CHECK(nlsql_query(c, "INSERT INTO t VALUES (1, 'alice'), (2, 'bob')", NULL) == NLSQL_OK, "insert rows");

    int port = 37000 + (getpid() % 500);
    int srt = wan_start(c, port);
    CHECK(srt == NLSQL_OK, "wan gateway started");
    if (srt != NLSQL_OK) {
        printf("  start failed (host API unavailable?)\n");
        nlsql_close(c);
        nlsql_dl_close(ext); nlsql_dl_close(host);
        printf("\n%d passed, %d failed\n", g_pass, g_fail);
        return 77;
    }
    CHECK(wan_running() == 1, "wan_running() = 1");

    char resp[4096];
    CHECK(http_query(port, "SELECT name FROM t WHERE id = 2", resp, sizeof(resp)) == 0, "http query ok");
    CHECK(strncmp(resp, "HTTP/1.1 200", 12) == 0, "response has HTTP status line");
    CHECK(strstr(resp, "Content-Length:") != NULL, "response has Content-Length");
    CHECK(strstr(resp, "\r\n\r\nOK\n") != NULL, "body starts with OK");
    CHECK(strstr(resp, "name\nbob\n") != NULL, "body contains column + value 'bob'");

    CHECK(http_query(port, "SELECT * FROM no_such_table", resp, sizeof(resp)) == 0, "http error query ok");
    CHECK(strncmp(resp, "HTTP/1.1 200", 12) == 0, "error response still HTTP 200");
    CHECK(strstr(resp, "ERR 1146") != NULL, "body reports MySQL errno 1146");
    CHECK(strstr(resp, "no_such_table") != NULL, "error body mentions table name");

    wan_stop();
    CHECK(wan_running() == 0, "wan_running() = 0 after stop");

    nlsql_close(c);
    nlsql_dl_close(ext);
    nlsql_dl_close(host);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
