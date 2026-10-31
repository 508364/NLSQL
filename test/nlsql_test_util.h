/**
 * @file nlsql_test_util.h
 * @brief 测试公用小工具：独立数据目录 / 扩展加载 / MySQL 协议包收发 / COUNT(*)
 *
 * 抽出来避免各用例重复实现同一套平台适配代码：
 *   - 目录名带单调毫秒（Wine/Windows 下 getpid() 可能恒定，只靠 pid 会命中残留目录）
 *   - 套接字收发统一走 netleaf_nlsql_platform.h（Windows 下 SOCKET 不是 CRT fd）
 */
#ifndef NLSQL_TEST_UTIL_H
#define NLSQL_TEST_UTIL_H

#include "netleaf_nlsql.h"
#include "netleaf_nlsql_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* 本用例专属数据目录：/tmp/<tag>_<pid>_<mono_ms> */
static inline void test_dir(char* out, size_t cap, const char* tag) {
    snprintf(out, cap, "/tmp/%s_%d_%llu", tag, (int)getpid(),
             (unsigned long long)nlsql_mono_ms());
}

/* 按 NLSQL_EXT_DIR（缺省 "."）打开扩展插件；失败打印 dlerror 并返回 NULL */
static inline void* test_ext_open(const char* name) {
    const char* dir = getenv("NLSQL_EXT_DIR");
    if (!dir || !dir[0]) dir = ".";
    char path[520];
    nlsql_plugin_path(path, sizeof(path), dir, name);
    void* h = nlsql_dl_open(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) printf("  dlerror(%s): %s\n", name, nlsql_dl_error());
    return h;
}

/* 解析并调用扩展初始化符号；成功返回 0 */
static inline int test_ext_init(void* h, const char* sym) {
    int (*init)(void) = (int (*)(void))nlsql_dl_sym(h, sym);
    if (!init) {
        printf("  dlsym %s: %s\n", sym, nlsql_dl_error());
        return -1;
    }
    return init();
}

/* SELECT COUNT(*) FROM <table>；成功返回 1 并写入 *out */
static inline int test_count_rows(nlsql_conn_t* c, const char* table, long long* out) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    nlsql_result_t* r = NULL;
    int ok = (nlsql_query(c, sql, &r) == NLSQL_OK && r && r->row_count == 1);
    if (ok) *out = (long long)r->rows[0].vals[0].i;
    nlsql_free_result(r);
    return ok;
}

/* ---------- MySQL 协议包（原生套接字，走平台封装） ---------- */

static inline int test_rd_all(int fd, void* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = nlsql_sock_recv(fd, (char*)buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static inline int test_wr_all(int fd, const void* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = nlsql_sock_send(fd, (const char*)buf + sent, n - sent);
        if (w <= 0) return -1;
        sent += (size_t)w;
    }
    return 0;
}

static inline int test_rd_packet(int fd, uint8_t** out, size_t* len) {
    *out = NULL; *len = 0;
    uint8_t hdr[4];
    if (test_rd_all(fd, hdr, 4) != 0) return -1;
    size_t n = (size_t)hdr[0] | ((size_t)hdr[1] << 8) | ((size_t)hdr[2] << 16);
    uint8_t* p = (uint8_t*)malloc(n + 1);
    if (!p) return -1;
    if (n && test_rd_all(fd, p, n) != 0) { free(p); return -1; }
    p[n] = '\0';
    *out = p; *len = n;
    return 0;
}

static inline int test_wr_packet(int fd, uint8_t seq, const uint8_t* payload, size_t len) {
    uint8_t hdr[4] = { (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), seq };
    if (test_wr_all(fd, hdr, 4) != 0) return -1;
    if (len && test_wr_all(fd, payload, len) != 0) return -1;
    return 0;
}

#endif /* NLSQL_TEST_UTIL_H */
