/**
 * @file test_nlsql.c
 * @brief NLSQL 功能测试：DDL/DML/查询/预编译/持久化/异步/只读/锁
 */
#include "netleaf_nlsql.h"
#include "netleaf_nlsql_platform.h"
#include "netleaf_nlsql_ext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

static void q_ok(nlsql_conn_t* c, const char* sql) {
    int rc = nlsql_query(c, sql, NULL);
    if (rc != NLSQL_OK)
        printf("  [%s] err=%d msg=%s\n", sql, rc, nlsql_error(c));
}

static char g_dir[256];

static void make_dir(const char* tag) {
    /* 附加单调毫秒后缀：Wine/Windows 下 pid 可能复用，避免命中上次残留目录 */
    snprintf(g_dir, sizeof(g_dir), "/tmp/nlsql_test_%s_%d_%llu", tag, (int)getpid(),
             (unsigned long long)nlsql_mono_ms());
}

/* ---------- 基础 CRUD ---------- */
static void test_basic(void) {
    make_dir("basic");
    nlsql_conn_t* c = nlsql_open(g_dir);
    CHECK(c != NULL, "open");
    if (!c) { printf("  global: %s\n", nlsql_global_error()); return; }

    q_ok(c, "CREATE DATABASE testdb");
    q_ok(c, "USE testdb");
    q_ok(c, "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(32), score DOUBLE)");

    q_ok(c, "INSERT INTO users (name, score) VALUES ('alice', 95.5)");
    q_ok(c, "INSERT INTO users (name, score) VALUES ('bob', 82.0), ('carol', 77.25)");

    nlsql_result_t* r = NULL;
    CHECK(nlsql_query(c, "SELECT * FROM users", &r) == NLSQL_OK && r && r->row_count == 3, "select all 3 rows");
    if (r && r->row_count == 3) {
        CHECK(strcmp(r->cols[1].name, "name") == 0, "col name");
        CHECK(r->rows[0].vals[0].i == 1, "autoinc starts 1");
    }
    nlsql_free_result(r); r = NULL;

    CHECK(nlsql_query(c, "SELECT COUNT(*) FROM users", &r) == NLSQL_OK && r && r->row_count == 1 &&
          r->rows[0].vals[0].i == 3, "count(*) = 3");
    nlsql_free_result(r); r = NULL;

    CHECK(nlsql_query(c, "SELECT name FROM users WHERE score > 80 ORDER BY score DESC", &r) == NLSQL_OK &&
          r && r->row_count == 2 && strcmp(r->rows[0].vals[0].s, "alice") == 0, "where + order desc");
    nlsql_free_result(r); r = NULL;

    CHECK(nlsql_query(c, "SELECT * FROM users ORDER BY id LIMIT 1 OFFSET 1", &r) == NLSQL_OK &&
          r && r->row_count == 1 && strcmp(r->rows[0].vals[1].s, "bob") == 0, "limit offset");
    nlsql_free_result(r); r = NULL;

    CHECK(nlsql_query(c, "SELECT name FROM users WHERE name LIKE 'a%'", &r) == NLSQL_OK &&
          r && r->row_count == 1, "like a%");
    nlsql_free_result(r); r = NULL;

    /* UPDATE / DELETE */
    CHECK(nlsql_query(c, "UPDATE users SET score = 100 WHERE name = 'bob'", NULL) == NLSQL_OK, "update");
    CHECK(nlsql_query(c, "SELECT score FROM users WHERE name = 'bob'", &r) == NLSQL_OK &&
          r && r->rows[0].vals[0].d == 100.0, "updated value");
    nlsql_free_result(r); r = NULL;

    CHECK(nlsql_query(c, "DELETE FROM users WHERE name = 'carol'", NULL) == NLSQL_OK, "delete");
    CHECK(nlsql_query(c, "SELECT COUNT(*) FROM users", &r) == NLSQL_OK && r->rows[0].vals[0].i == 2, "after delete count=2");
    nlsql_free_result(r); r = NULL;

    /* 常量 SELECT */
    CHECK(nlsql_query(c, "SELECT 1", &r) == NLSQL_OK && r && r->row_count == 1 &&
          r->rows[0].vals[0].i == 1, "select 1");
    nlsql_free_result(r); r = NULL;

    /* SHOW */
    CHECK(nlsql_query(c, "SHOW TABLES", &r) == NLSQL_OK && r && r->row_count == 1, "show tables");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SHOW DATABASES", &r) == NLSQL_OK && r && r->row_count >= 1, "show databases");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SHOW COLUMNS FROM users", &r) == NLSQL_OK && r && r->row_count == 3, "show columns");
    nlsql_free_result(r); r = NULL;

    /* 错误路径 */
    CHECK(nlsql_query(c, "SELECT * FROM nosuch", NULL) == NLSQL_ER_NO_SUCH_TABLE, "no such table 1146");
    CHECK(nlsql_errno(c) == NLSQL_ER_NO_SUCH_TABLE, "errno 1146");
    CHECK(nlsql_query(c, "INSERT INTO users (nosuchcol) VALUES (1)", NULL) == NLSQL_ER_UNKNOWN_COLUMN, "unknown col 1054");
    CHECK(nlsql_query(c, "INSERT INTO users (name, score) VALUES ('x')", NULL) == NLSQL_ER_WRONG_VALUE_COUNT, "value count 1136");
    CHECK(nlsql_query(c, "INSERT INTO users (id, name) VALUES (1, 'dup')", NULL) == NLSQL_ER_DUP_ENTRY, "dup entry 1062");
    CHECK(nlsql_query(c, "THIS IS NOT SQL", NULL) == NLSQL_ER_SYNTAX, "syntax 1064");

    /* TRUNCATE / DROP */
    q_ok(c, "TRUNCATE TABLE users");
    CHECK(nlsql_query(c, "SELECT COUNT(*) FROM users", &r) == NLSQL_OK && r->rows[0].vals[0].i == 0, "truncate empties");
    nlsql_free_result(r); r = NULL;
    q_ok(c, "DROP TABLE users");
    CHECK(nlsql_query(c, "SELECT * FROM users", NULL) == NLSQL_ER_NO_SUCH_TABLE, "drop table");
    q_ok(c, "DROP DATABASE testdb");

    nlsql_close(c);
}

/* ---------- 预编译防注入 ---------- */
static void test_stmt(void) {
    make_dir("stmt");
    nlsql_conn_t* c = nlsql_open(g_dir);
    CHECK(c != NULL, "open stmt");
    if (!c) return;
    q_ok(c, "CREATE DATABASE d");
    q_ok(c, "USE d");
    q_ok(c, "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(64))");
    q_ok(c, "INSERT INTO t VALUES (1, 'ok')");

    nlsql_stmt_t* s = NULL;
    CHECK(nlsql_prepare(c, "SELECT * FROM t WHERE name = ?", &s) == NLSQL_OK && s, "prepare");
    CHECK(nlsql_stmt_param_count(s) == 1, "param count");
    CHECK(nlsql_stmt_bind_text(s, 0, "' OR '1'='1", 12) == NLSQL_OK, "bind injection text");
    nlsql_result_t* r = NULL;
    CHECK(nlsql_stmt_execute(s, &r) == NLSQL_OK && r && r->row_count == 0, "injection returns 0 rows");
    nlsql_free_result(r); r = NULL;

    CHECK(nlsql_stmt_bind_text(s, 0, "ok", 2) == NLSQL_OK, "bind ok");
    CHECK(nlsql_stmt_execute(s, &r) == NLSQL_OK && r && r->row_count == 1, "param 'ok' matches");
    nlsql_free_result(r); r = NULL;
    nlsql_stmt_free(s);

    /* 参数化 INSERT */
    s = NULL;
    CHECK(nlsql_prepare(c, "INSERT INTO t VALUES (?, ?)", &s) == NLSQL_OK, "prepare insert");
    nlsql_stmt_bind_int(s, 0, 2);
    nlsql_stmt_bind_text(s, 1, "param", 5);
    CHECK(nlsql_stmt_execute(s, NULL) == NLSQL_OK, "exec insert");
    nlsql_stmt_free(s);
    CHECK(nlsql_query(c, "SELECT COUNT(*) FROM t", &r) == NLSQL_OK && r->rows[0].vals[0].i == 2, "2 rows after param insert");
    nlsql_free_result(r); r = NULL;

    nlsql_close(c);
}

/* ---------- 持久化 ---------- */
static void test_persist(void) {
    make_dir("persist");
    nlsql_conn_t* c = nlsql_open(g_dir);
    CHECK(c != NULL, "open persist");
    if (!c) return;
    q_ok(c, "CREATE DATABASE p");
    q_ok(c, "USE p");
    q_ok(c, "CREATE TABLE kv (k VARCHAR(16) PRIMARY KEY, v TEXT)");
    q_ok(c, "INSERT INTO kv VALUES ('a', 'hello'), ('b', 'world')");
    nlsql_flush(c);
    nlsql_close(c);

    c = nlsql_open(g_dir);
    CHECK(c != NULL, "reopen");
    if (!c) return;
    q_ok(c, "USE p");
    nlsql_result_t* r = NULL;
    CHECK(nlsql_query(c, "SELECT v FROM kv WHERE k = 'a'", &r) == NLSQL_OK && r && r->row_count == 1 &&
          strcmp(r->rows[0].vals[0].s, "hello") == 0, "persisted value");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT COUNT(*) FROM kv", &r) == NLSQL_OK && r->rows[0].vals[0].i == 2, "persisted count");
    nlsql_free_result(r); r = NULL;
    nlsql_close(c);
}

/* ---------- 异步 ---------- */
static volatile int g_async_done = 0;
static volatile int g_async_err = -1;
static volatile int g_async_rows = -1;

static void async_cb(nlsql_conn_t* c, int err, nlsql_result_t* res, void* ud) {
    (void)c; (void)ud;
    g_async_err = err;
    g_async_rows = res ? (int)res->row_count : -1;
    g_async_done = 1;
}

static void test_async(void) {
    make_dir("async");
    nlsql_conn_t* c = nlsql_open(g_dir);
    CHECK(c != NULL, "open async");
    if (!c) return;
    q_ok(c, "CREATE DATABASE a");
    q_ok(c, "USE a");
    q_ok(c, "CREATE TABLE t (id INT PRIMARY KEY)");
    q_ok(c, "INSERT INTO t VALUES (1), (2), (3)");

    g_async_done = 0; g_async_err = -1; g_async_rows = -1;
    CHECK(nlsql_query_async(c, "SELECT * FROM t", async_cb, NULL) == NLSQL_OK, "enqueue async");
    nlsql_async_wait(c);
    CHECK(g_async_done == 1, "async callback fired");
    CHECK(g_async_err == NLSQL_OK, "async no error");
    CHECK(g_async_rows == 3, "async 3 rows");

    nlsql_close(c);
}

/* ---------- 只读 / 锁 ---------- */
static void test_readonly_lock(void) {
    make_dir("ro");
    nlsql_conn_t* w = nlsql_open(g_dir);
    CHECK(w != NULL, "open writer");
    if (!w) return;
    q_ok(w, "CREATE DATABASE r");
    q_ok(w, "USE r");
    q_ok(w, "CREATE TABLE t (id INT PRIMARY KEY)");
    q_ok(w, "INSERT INTO t VALUES (1)");
    /* journal 走 64KB 写缓冲（性能），跨连接可见性以 flush/压实/关闭为界 */
    CHECK(nlsql_flush(w) == NLSQL_OK, "flush before reader");

    /* 第二个可写连接应被文件锁拒绝 */
    nlsql_conn_t* w2 = nlsql_open(g_dir);
    CHECK(w2 == NULL, "second writer locked");

    /* 只读连接可共享目录（不加锁），但拒绝写 */
    nlsql_open_opts_t o;
    memset(&o, 0, sizeof(o));
    o.data_dir = g_dir;
    o.readonly = 1;
    nlsql_conn_t* ro = nlsql_open_ex(&o);
    CHECK(ro != NULL, "open readonly");
    if (ro) {
        q_ok(ro, "USE r");
        nlsql_result_t* res = NULL;
        CHECK(nlsql_query(ro, "SELECT * FROM t", &res) == NLSQL_OK && res && res->row_count == 1, "readonly select");
        nlsql_free_result(res);
        /* 写语句全类拒绝：MySQL --read-only 同款 errno 1290（ER_OPTION_PREVENTS_STATEMENT） */
        CHECK(nlsql_query(ro, "INSERT INTO t VALUES (2)", NULL) == 1290, "readonly rejects INSERT 1290");
        CHECK(nlsql_query(ro, "UPDATE t SET id = 9", NULL) == 1290, "readonly rejects UPDATE 1290");
        CHECK(nlsql_query(ro, "DELETE FROM t", NULL) == 1290, "readonly rejects DELETE 1290");
        CHECK(nlsql_query(ro, "CREATE TABLE x (a INT)", NULL) == 1290, "readonly rejects CREATE 1290");
        CHECK(nlsql_query(ro, "DROP TABLE t", NULL) == 1290, "readonly rejects DROP 1290");
        CHECK(nlsql_query(ro, "TRUNCATE TABLE t", NULL) == 1290, "readonly rejects TRUNCATE 1290");
        CHECK(nlsql_query(ro, "ALTER TABLE t ADD COLUMN z INT", NULL) == 1290, "readonly rejects ALTER 1290");
        /* 读语句与无害会话语句放行 */
        CHECK(nlsql_query(ro, "SHOW TABLES", NULL) == NLSQL_OK, "readonly allows SHOW");
        CHECK(nlsql_query(ro, "SET NAMES utf8mb4", NULL) == NLSQL_OK, "readonly allows SET");
        CHECK(strcmp(nlsql_sqlstate(ro), "HY000") == 0, "readonly err sqlstate HY000");
        nlsql_close(ro);
    }
    nlsql_close(w);
}

/* ---------- 未选库错误 ---------- */
static void test_no_db(void) {
    make_dir("nodb");
    nlsql_conn_t* c = nlsql_open(g_dir);
    CHECK(c != NULL, "open nodb");
    if (!c) return;
    CHECK(nlsql_query(c, "CREATE TABLE t (id INT PRIMARY KEY)", NULL) == 1046, "no database selected 1046");
    CHECK(strcmp(nlsql_sqlstate(c), "3D000") == 0, "sqlstate 3D000");
    CHECK(nlsql_query(c, "SHOW DATABASES", NULL) == NLSQL_OK, "show databases without use");
    nlsql_close(c);
}

/* ---------- 新谓词：IS NULL / IN / BETWEEN（含 NOT 变体） ---------- */
static void test_predicates(void) {
    make_dir("pred");
    nlsql_conn_t* c = nlsql_open(g_dir);
    CHECK(c != NULL, "open pred");
    if (!c) return;
    q_ok(c, "CREATE DATABASE p");
    q_ok(c, "USE p");
    q_ok(c, "CREATE TABLE t (id INT PRIMARY KEY, v INT, s VARCHAR(16))");
    q_ok(c, "INSERT INTO t VALUES (1, 10, 'a')");
    q_ok(c, "INSERT INTO t VALUES (2, 20, 'b')");
    q_ok(c, "INSERT INTO t VALUES (3, NULL, NULL)");
    q_ok(c, "INSERT INTO t VALUES (4, 40, 'd')");
    q_ok(c, "INSERT INTO t VALUES (5, 50, 'e')");
    q_ok(c, "INSERT INTO t VALUES (6, NULL, 'f')");

    nlsql_result_t* r = NULL;
    CHECK(nlsql_query(c, "SELECT id FROM t WHERE v IS NULL", &r) == NLSQL_OK && r->row_count == 2, "IS NULL -> 2");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT id FROM t WHERE v IS NOT NULL", &r) == NLSQL_OK && r->row_count == 4, "IS NOT NULL -> 4");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT id FROM t WHERE id IN (1, 3, 5)", &r) == NLSQL_OK && r->row_count == 3, "IN -> 3");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT id FROM t WHERE id NOT IN (1, 3, 5)", &r) == NLSQL_OK && r->row_count == 3, "NOT IN -> 3");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT id FROM t WHERE v BETWEEN 20 AND 45", &r) == NLSQL_OK && r->row_count == 2, "BETWEEN -> 2");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT id FROM t WHERE v NOT BETWEEN 20 AND 45", &r) == NLSQL_OK && r->row_count == 2, "NOT BETWEEN -> 2");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT id FROM t WHERE id > 2 AND id IN (3, 5) AND v IS NOT NULL", &r) == NLSQL_OK && r->row_count == 1, "AND 组合 -> 1");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT id FROM t WHERE s LIKE 'a' OR s = 'd'", &r) == NLSQL_OK && r->row_count == 2, "OR + LIKE -> 2");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT id FROM t WHERE nosuch IS NULL", &r) != NLSQL_OK, "unknown column error");
    nlsql_free_result(r);
    nlsql_close(c);
}

static void test_alter(void) {
    make_dir("alter");
    nlsql_conn_t* c = nlsql_open(g_dir);
    CHECK(c != NULL, "open alter");
    if (!c) return;
    q_ok(c, "CREATE DATABASE a");
    q_ok(c, "USE a");
    q_ok(c, "CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    q_ok(c, "INSERT INTO t VALUES (1, 10)");
    q_ok(c, "INSERT INTO t VALUES (2, 20)");

    nlsql_result_t* r = NULL;
    /* ADD COLUMN + DEFAULT：旧行填默认值 */
    q_ok(c, "ALTER TABLE t ADD COLUMN s VARCHAR(10) NOT NULL DEFAULT 'x'");
    (void)r;
    CHECK(nlsql_query(c, "SELECT s FROM t WHERE id = 1", &r) == NLSQL_OK && r->row_count == 1, "ADD col select");
    if (r && r->row_count) CHECK(!r->rows[0].vals[0].is_null &&
          strcmp(r->rows[0].vals[0].s, "x") == 0, "ADD col old row -> default 'x'");
    nlsql_free_result(r); r = NULL;
    /* 新行可写新列 */
    q_ok(c, "INSERT INTO t VALUES (3, 30, 'n')");
    CHECK(nlsql_query(c, "SELECT s FROM t WHERE id = 3", &r) == NLSQL_OK &&
          r->row_count == 1 && !r->rows[0].vals[0].is_null &&
          strcmp(r->rows[0].vals[0].s, "n") == 0, "ADD col new row");
    nlsql_free_result(r); r = NULL;
    /* 重名列报错 */
    CHECK(nlsql_query(c, "ALTER TABLE t ADD COLUMN v INT", &r) != NLSQL_OK, "ADD dup col error");
    nlsql_free_result(r); r = NULL;

    /* CREATE TABLE DEFAULT：省略列填默认值 */
    q_ok(c, "CREATE TABLE d (id INT PRIMARY KEY, n INT DEFAULT 7, s VARCHAR(8) DEFAULT 'hi')");
    q_ok(c, "INSERT INTO d (id) VALUES (1)");
    CHECK(nlsql_query(c, "SELECT n, s FROM d WHERE id = 1", &r) == NLSQL_OK && r->row_count == 1 &&
          r->rows[0].vals[0].i == 7 &&
          r->rows[0].vals[1].s && strcmp(r->rows[0].vals[1].s, "hi") == 0, "CREATE DEFAULT fill");
    nlsql_free_result(r); r = NULL;
    q_ok(c, "INSERT INTO d VALUES (2, 99, 'zz')");
    CHECK(nlsql_query(c, "SELECT n FROM d WHERE id = 2", &r) == NLSQL_OK &&
          r->row_count == 1 && r->rows[0].vals[0].i == 99, "explicit value overrides default");
    nlsql_free_result(r); r = NULL;

    /* MODIFY：类型转换 */
    q_ok(c, "ALTER TABLE t MODIFY v DOUBLE");
    CHECK(nlsql_query(c, "SELECT v FROM t WHERE id = 2", &r) == NLSQL_OK && r->row_count == 1 &&
          r->rows[0].vals[0].type == NLSQL_TYPE_DOUBLE && r->rows[0].vals[0].d == 20.0, "MODIFY cast");
    nlsql_free_result(r); r = NULL;

    /* RENAME COLUMN */
    q_ok(c, "ALTER TABLE t RENAME COLUMN s TO txt");
    CHECK(nlsql_query(c, "SELECT txt FROM t WHERE id = 1", &r) == NLSQL_OK && r->row_count == 1, "RENAME COLUMN");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT s FROM t WHERE id = 1", &r) != NLSQL_OK, "old col name error");
    nlsql_free_result(r); r = NULL;

    /* DROP COLUMN */
    q_ok(c, "ALTER TABLE t DROP COLUMN txt");
    CHECK(nlsql_query(c, "SELECT txt FROM t", &r) != NLSQL_OK, "DROP col error");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT id, v FROM t", &r) == NLSQL_OK && r->row_count == 3, "DROP col rows kept");
    nlsql_free_result(r); r = NULL;

    /* RENAME TABLE：旧快照清除 */
    q_ok(c, "ALTER TABLE t RENAME TO t2");
    CHECK(nlsql_query(c, "SELECT id FROM t2", &r) == NLSQL_OK && r->row_count == 3, "RENAME TABLE");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT id FROM t", &r) != NLSQL_OK, "old table gone");
    nlsql_free_result(r); r = NULL;

    /* 持久化：重开可见 */
    nlsql_close(c);
    c = nlsql_open(g_dir);
    CHECK(c != NULL, "reopen alter");
    if (c) {
        q_ok(c, "USE a");
        CHECK(nlsql_query(c, "SELECT id FROM t2", &r) == NLSQL_OK && r->row_count == 3, "alter persist");
        nlsql_free_result(r); r = NULL;
        nlsql_close(c);
    }
}

/* ---------- 聚合 / GROUP BY / HAVING / DISTINCT / 别名 / SET / 科学计数法 ---------- */
static void test_agg_more(void) {
    make_dir("aggmore");
    nlsql_conn_t* c = nlsql_open(g_dir);
    CHECK(c != NULL, "open aggmore");
    if (!c) return;
    q_ok(c, "CREATE DATABASE g");
    q_ok(c, "USE g");
    q_ok(c, "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, g VARCHAR(8), v DOUBLE)");
    /* 科学计数法字面量（MySQL 客户端转义浮点的常见形式 77.25e0） */
    q_ok(c, "INSERT INTO t (g, v) VALUES ('a', 10.5e0), ('a', 20.5e0), ('b', 30.5e0), ('b', 40.5e0), ('b', 50.5e0)");

    nlsql_result_t* r = NULL;
    CHECK(nlsql_query(c, "SELECT COUNT(*) FROM t", &r) == NLSQL_OK && r && r->rows[0].vals[0].i == 5, "count 5");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT SUM(v) FROM t", &r) == NLSQL_OK && r &&
          r->rows[0].vals[0].d > 152.0 && r->rows[0].vals[0].d < 153.0, "sum(v) ~152.5");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT AVG(v) FROM t", &r) == NLSQL_OK && r &&
          r->rows[0].vals[0].d > 30.4 && r->rows[0].vals[0].d < 30.6, "avg(v) ~30.5");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT MIN(v) FROM t", &r) == NLSQL_OK && r &&
          r->rows[0].vals[0].d > 10.4 && r->rows[0].vals[0].d < 10.6, "min(v) ~10.5");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT MAX(v) FROM t", &r) == NLSQL_OK && r &&
          r->rows[0].vals[0].d > 50.4 && r->rows[0].vals[0].d < 50.6, "max(v) ~50.5");
    nlsql_free_result(r); r = NULL;

    /* GROUP BY + HAVING：a 组 2 行、b 组 3 行，HAVING 过滤后仅 b */
    CHECK(nlsql_query(c, "SELECT g, COUNT(*) FROM t GROUP BY g", &r) == NLSQL_OK && r &&
          r->row_count == 2, "group by 2 groups");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT g FROM t GROUP BY g HAVING COUNT(*) > 2", &r) == NLSQL_OK && r &&
          r->row_count == 1 && strcmp(r->rows[0].vals[0].s, "b") == 0, "having cnt>2 -> b");
    nlsql_free_result(r); r = NULL;

    /* DISTINCT 单列 */
    CHECK(nlsql_query(c, "SELECT DISTINCT g FROM t", &r) == NLSQL_OK && r && r->row_count == 2, "distinct g 2");
    nlsql_free_result(r); r = NULL;

    /* AS 别名（列别名） */
    CHECK(nlsql_query(c, "SELECT COUNT(*) AS total FROM t", &r) == NLSQL_OK && r &&
          r->row_count == 1 && strcmp(r->cols[0].name, "total") == 0 &&
          r->rows[0].vals[0].i == 5, "alias total");
    nlsql_free_result(r); r = NULL;

    /* 聚合限制：一查询仅一个聚合（够用版，明确报错） */
    CHECK(nlsql_query(c, "SELECT COUNT(*), SUM(v) FROM t", NULL) != NLSQL_OK, "multi-agg error");
    /* WHERE 中禁止聚合 */
    CHECK(nlsql_query(c, "SELECT id FROM t WHERE COUNT(*) > 1", NULL) != NLSQL_OK, "agg in where error");

    /* SET 配置语句：no-op 返回 OK（客户端连接后常发） */
    CHECK(nlsql_query(c, "SET NAMES utf8mb4", NULL) == NLSQL_OK, "set names ok");
    CHECK(nlsql_query(c, "SET autocommit = 1", NULL) == NLSQL_OK, "set autocommit ok");

    nlsql_close(c);
}

/* ---------- 写通道 submit：零阻塞 FIFO + 背压 + 空闲预压实 ---------- */
static void test_submit(void) {
    make_dir("submit");
    nlsql_conn_t* c = nlsql_open(g_dir);
    CHECK(c != NULL, "open submit");
    if (!c) return;
    q_ok(c, "CREATE DATABASE sb");
    q_ok(c, "USE sb");
    q_ok(c, "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(32))");

    /* 批量 fire-and-forget：入队即返（BUSY 背压时稍候重试），FIFO 由队列保证 */
    char sql[128];
    int enqueue_fail = 0;
    for (int i = 0; i < 2000; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO t (v) VALUES ('sub-%d')", i);
        int tries = 0;
        while (nlsql_submit(c, sql) == NLSQL_ERR_BUSY && tries++ < 5000) usleep(200);
        if (tries >= 5000) enqueue_fail++;
    }
    CHECK(enqueue_fail == 0, "submit enqueue (backpressure retries exhausted)");
    nlsql_async_wait(c);

    nlsql_result_t* r = NULL;
    CHECK(nlsql_query(c, "SELECT COUNT(*) FROM t", &r) == NLSQL_OK && r &&
          r->row_count == 1 && r->rows[0].vals[0].i == 2000, "submit 2000 rows landed");
    nlsql_free_result(r); r = NULL;

    /* FIFO 顺序：submit 序号与自增值一致（最后一条是 id=2000 sub-1999） */
    CHECK(nlsql_query(c, "SELECT v FROM t WHERE id = 2000", &r) == NLSQL_OK && r &&
          r->row_count == 1 && strcmp(r->rows[0].vals[0].s, "sub-1999") == 0, "submit fifo order");
    nlsql_free_result(r); r = NULL;

    /* 提交与同步查询的可见性：wait 后同步可见（写通道与同步共锁串行）
       'sub-19%' 匹配 sub-19 + sub-190..199 + sub-1900..1999 = 1+10+100 = 111 */
    CHECK(nlsql_query(c, "SELECT COUNT(*) FROM t WHERE v LIKE 'sub-19%'", &r) == NLSQL_OK && r &&
          r->rows[0].vals[0].i == 111, "last batch visible");
    nlsql_free_result(r); r = NULL;

    nlsql_close(c);
}

/* ---------- 错误码 / SQLSTATE 可读性（MySQL 客户端解析依赖） ---------- */
static void test_errcodes(void) {
    make_dir("errc");
    nlsql_conn_t* c = nlsql_open(g_dir);
    CHECK(c != NULL, "open errc");
    if (!c) return;
    q_ok(c, "CREATE DATABASE e");
    q_ok(c, "USE e");
    q_ok(c, "CREATE TABLE t (id INT PRIMARY KEY, v INT NOT NULL)");
    q_ok(c, "INSERT INTO t VALUES (1, 10)");

    /* 1049 unknown database（USE 不存在的库） */
    CHECK(nlsql_query(c, "USE no_such_db", NULL) == NLSQL_ER_NO_SUCH_DB, "err 1049 unknown db");
    CHECK(strcmp(nlsql_sqlstate(c), "42000") == 0, "state 42000 for 1049");
    q_ok(c, "USE e"); /* 上一步失败不改当前库，显式复位防后续连锁 */

    /* 1146 unknown table（42S02） */
    CHECK(nlsql_query(c, "SELECT * FROM no_such_table", NULL) == NLSQL_ER_NO_SUCH_TABLE, "err 1146 unknown table");
    CHECK(strcmp(nlsql_sqlstate(c), "42S02") == 0, "state 42S02 for 1146");

    /* 1062 duplicate key（23000） */
    CHECK(nlsql_query(c, "INSERT INTO t VALUES (1, 20)", NULL) == NLSQL_ER_DUP_ENTRY, "err 1062 dup key");
    CHECK(strcmp(nlsql_sqlstate(c), "23000") == 0, "state 23000 for 1062");

    /* 1048 NOT NULL 违反（23000） */
    CHECK(nlsql_query(c, "INSERT INTO t VALUES (7, NULL)", NULL) == NLSQL_ER_BAD_NULL_ERROR, "err 1048 not null");
    CHECK(strcmp(nlsql_sqlstate(c), "23000") == 0, "state 23000 for 1048");

    /* 1054 unknown column（42S22） */
    CHECK(nlsql_query(c, "SELECT zzz FROM t", NULL) == NLSQL_ER_UNKNOWN_COLUMN, "err 1054 unknown column");
    CHECK(strcmp(nlsql_sqlstate(c), "42S22") == 0, "state 42S22 for 1054");

    /* 1064 syntax（42000） */
    CHECK(nlsql_query(c, "SELEC 1", NULL) == NLSQL_ER_SYNTAX, "err 1064 syntax");
    CHECK(strcmp(nlsql_sqlstate(c), "42000") == 0, "state 42000 for 1064");

    /* 1105/1235 不支持语法：报错可读而非崩溃 */
    int rc = nlsql_query(c, "SELECT * FROM t WHERE 1=1 AND id IN (1)", NULL);
    if (rc != NLSQL_OK) {
        CHECK(nlsql_error(c) && nlsql_error(c)[0], "unsupported stmt message readable");
    } else {
        CHECK(1, "IN list supported");
    }
    nlsql_close(c);
}

/* ---------- 客户端元数据（@@变量 / 元数据函数 / SHOW 族） ---------- */
static void test_metadata(void) {
    make_dir("meta");
    nlsql_conn_t* c = nlsql_open(g_dir);
    CHECK(c != NULL, "open meta");
    if (!c) return;
    nlsql_result_t* r = NULL;

    CHECK(nlsql_query(c, "SELECT @@version", &r) == NLSQL_OK && r && r->row_count == 1 &&
          r->rows[0].vals[0].s && strncmp(r->rows[0].vals[0].s, "8.", 2) == 0, "@@version");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT VERSION()", &r) == NLSQL_OK && r &&
          r->rows[0].vals[0].s && strcmp(r->rows[0].vals[0].s, NLSQL_SERVER_VERSION) == 0, "VERSION()");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SELECT CONNECTION_ID()", &r) == NLSQL_OK && r &&
          r->rows[0].vals[0].type == NLSQL_TYPE_INT64, "CONNECTION_ID()");
    nlsql_free_result(r); r = NULL;

    q_ok(c, "CREATE DATABASE mdb");
    q_ok(c, "USE mdb");
    CHECK(nlsql_query(c, "SELECT DATABASE()", &r) == NLSQL_OK && r &&
          strcmp(r->rows[0].vals[0].s, "mdb") == 0, "DATABASE()");
    nlsql_free_result(r); r = NULL;

    q_ok(c, "CREATE TABLE m (id INT PRIMARY KEY, v VARCHAR(10), note TEXT)");
    q_ok(c, "INSERT INTO m VALUES (7, 'x', 'n')");
    /* 显式主键插入不产生自增 id，LAST_INSERT_ID() 需用 AUTO_INCREMENT 表验证 */
    q_ok(c, "CREATE TABLE ai (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(10))");
    q_ok(c, "INSERT INTO ai (v) VALUES ('a'), ('b')");
    CHECK(nlsql_query(c, "SELECT LAST_INSERT_ID()", &r) == NLSQL_OK && r &&
          r->rows[0].vals[0].i == 2, "LAST_INSERT_ID()");
    nlsql_free_result(r); r = NULL;

    CHECK(nlsql_query(c, "SHOW VARIABLES LIKE 'version%'", &r) == NLSQL_OK && r &&
          r->row_count == 2 && strcmp(r->cols[0].name, "Variable_name") == 0, "SHOW VARIABLES LIKE");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SHOW FULL COLUMNS FROM m", &r) == NLSQL_OK && r && r->row_count == 3 &&
          strcmp(r->cols[0].name, "Field") == 0 && r->col_count == 9, "SHOW FULL COLUMNS");
    if (r && r->row_count == 3) {
        CHECK(r->rows[0].vals[0].s && strcmp(r->rows[0].vals[0].s, "id") == 0, "full columns field");
        CHECK(r->rows[0].vals[2].s && strcmp(r->rows[0].vals[2].s, "utf8_general_ci") == 0 &&
              r->rows[0].vals[2].len == 15, "collation without NUL");
    }
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SHOW INDEX FROM m", &r) == NLSQL_OK && r && r->row_count == 1 &&
          strcmp(r->cols[0].name, "Table") == 0, "SHOW INDEX");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SHOW KEYS FROM m", &r) == NLSQL_OK && r && r->row_count == 1,
          "SHOW KEYS alias");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SHOW CREATE DATABASE mdb", &r) == NLSQL_OK && r && r->row_count == 1 &&
          strstr(r->rows[0].vals[1].s, "CREATE DATABASE") != NULL, "SHOW CREATE DATABASE");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SHOW SESSION VARIABLES LIKE 'sql_mode'", &r) == NLSQL_OK &&
          r && r->row_count == 1, "SHOW SESSION VARIABLES LIKE");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SHOW TABLE STATUS", &r) == NLSQL_OK && r && r->row_count >= 1, "SHOW TABLE STATUS");
    if (r) {
        int found = 0;
        for (uint32_t i = 0; i < r->row_count; i++)
            if (r->rows[i].vals[0].s && strcmp(r->rows[i].vals[0].s, "m") == 0) found = 1;
        CHECK(found, "SHOW TABLE STATUS lists m");
    }
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SHOW CREATE TABLE m", &r) == NLSQL_OK && r && r->row_count == 1 &&
          strstr(r->rows[0].vals[1].s, "CREATE TABLE") != NULL, "SHOW CREATE TABLE");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SHOW ENGINES", &r) == NLSQL_OK && r && r->row_count >= 1, "SHOW ENGINES");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SHOW CHARACTER SET LIKE 'utf8'", &r) == NLSQL_OK && r && r->row_count == 1, "SHOW CHARACTER SET");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SHOW COLLATION", &r) == NLSQL_OK && r && r->row_count >= 1, "SHOW COLLATION");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SHOW WARNINGS", &r) == NLSQL_OK && r && r->col_count == 3, "SHOW WARNINGS");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SHOW STATUS LIKE 'Threads%'", &r) == NLSQL_OK && r && r->row_count >= 1, "SHOW STATUS");
    nlsql_free_result(r); r = NULL;

    /* 客户端驱动常见语句必须可用（此前 1064） */
    CHECK(nlsql_query(c, "SELECT @@character_set_client", &r) == NLSQL_OK && r &&
          r->rows[0].vals[0].s != NULL, "@@character_set_client");
    nlsql_free_result(r); r = NULL;
    CHECK(nlsql_query(c, "SET NAMES utf8", NULL) == NLSQL_OK, "SET NAMES");
    CHECK(nlsql_query(c, "/*!40101 SET NAMES utf8 */", NULL) == NLSQL_OK, "version comment stripped");

    q_ok(c, "DROP DATABASE mdb");
    nlsql_close(c);
}

/* ---------- IF [NOT] EXISTS 语义（以及失败 DROP DATABASE 后 journal 仍可用） ---------- */
static void test_if_exists(void) {
    make_dir("ifexists");
    nlsql_conn_t* c = nlsql_open(g_dir);
    CHECK(c != NULL, "ifexists open");
    if (!c) { printf("  global: %s\n", nlsql_global_error()); return; }
    q_ok(c, "CREATE DATABASE ifdb");
    q_ok(c, "USE ifdb");
    q_ok(c, "CREATE TABLE t (id INT PRIMARY KEY, v INT)");

    CHECK(nlsql_query(c, "DROP TABLE missing", NULL) == NLSQL_ER_NO_SUCH_TABLE,
          "drop missing table -> 1146");
    CHECK(nlsql_query(c, "DROP TABLE IF EXISTS missing", NULL) == NLSQL_OK,
          "drop table if exists missing -> ok");
    CHECK(nlsql_query(c, "DROP TABLE IF EXISTS t", NULL) == NLSQL_OK, "drop table if exists t -> ok");
    CHECK(nlsql_query(c, "DROP TABLE t", NULL) == NLSQL_ER_NO_SUCH_TABLE,
          "drop dropped table -> 1146");

    CHECK(nlsql_query(c, "CREATE TABLE IF NOT EXISTS t2 (id INT PRIMARY KEY, v INT)", NULL) == NLSQL_OK,
          "create table if not exists -> ok");
    CHECK(nlsql_query(c, "CREATE TABLE IF NOT EXISTS t2 (id INT PRIMARY KEY, v INT)", NULL) == NLSQL_OK,
          "create table if not exists again -> ok");
    CHECK(nlsql_query(c, "CREATE TABLE t2 (id INT PRIMARY KEY)", NULL) == NLSQL_ER_TABLE_EXISTS,
          "create existing table -> 1050");

    /* 关键回归：DROP DATABASE IF EXISTS 不存在的库曾提前关掉当前库 journal，
       导致后续写入全部 "journal write failed"。这里紧跟一次写入验证 journal 仍可用。 */
    CHECK(nlsql_query(c, "DROP DATABASE IF EXISTS missing_db", NULL) == NLSQL_OK,
          "drop database if exists missing -> ok");
    CHECK(nlsql_query(c, "INSERT INTO t2 VALUES (1, 10)", NULL) == NLSQL_OK,
          "insert after drop missing db -> ok");
    nlsql_result_t* r = NULL;
    CHECK(nlsql_query(c, "SELECT v FROM t2 WHERE id = 1", &r) == NLSQL_OK && r &&
          r->row_count == 1 && r->rows[0].vals[0].i == 10, "row written after drop missing db");
    nlsql_free_result(r);
    CHECK(nlsql_query(c, "DROP DATABASE missing_db", NULL) == NLSQL_ER_NO_SUCH_DB,
          "drop missing db -> 1049");

    nlsql_close(c);
}

/* ---------- 扩展元数据 sidecar + 连接注册表 ---------- */
static char g_meta_def[128];
static int  g_meta_hits;

static int meta_probe_cb(const char* kind, const char* name, const char* target,
                         const char* def, void* ud) {
    (void)ud;
    if (strcmp(kind, "view") == 0 && strcmp(name, "v_demo") == 0) {
        snprintf(g_meta_def, sizeof(g_meta_def), "%s", def);
        if (target) { /* target 应存在（可为空串） */ }
        g_meta_hits++;
    }
    return 0;
}

/* ---------- 自动落盘 ---------- */
static void test_auto_flush(void) {
    make_dir("autoflush");
    nlsql_open_opts_t o;
    memset(&o, 0, sizeof(o));
    o.data_dir = g_dir;
    o.auto_flush_ms = 200;
    nlsql_conn_t* c = nlsql_open_ex(&o);
    CHECK(c != NULL, "open auto flush");
    if (!c) return;
    q_ok(c, "CREATE DATABASE af");
    q_ok(c, "USE af");
    q_ok(c, "CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(80))");
    /* 写入 > 64 KiB 的 journal：空闲节拍到达时才值得压实成快照 */
    nlsql_result_t* r = NULL;
    int ins_ok = 1;
    for (int b = 0; b < 12; b++) {
        char sql[16384] = "INSERT INTO t VALUES ";
        int off = (int)strlen(sql);
        for (int i = 0; i < 100; i++)
            off += snprintf(sql + off, sizeof(sql) - off, "%s(%d,'%064d')",
                            i ? "," : "", b * 100 + i, i);
        if (nlsql_query(c, sql, &r) != NLSQL_OK) ins_ok = 0;
        nlsql_free_result(r); r = NULL;
    }
    CHECK(ins_ok, "auto flush bulk insert");
    /* 两个节拍：第一个 fsync WAL，第二个（写入已停歇）压实为快照 */
    usleep(900 * 1000);
    char snap[320];
    snprintf(snap, sizeof(snap), "%s/af/t.nlt", g_dir);
    FILE* f = fopen(snap, "rb");
    CHECK(f != NULL, "auto flush wrote snapshot without nlsql_flush");
    if (f) fclose(f);
    CHECK(nlsql_query(c, "SELECT COUNT(*) FROM t", &r) == NLSQL_OK && r && r->row_count == 1 &&
          r->rows[0].vals[0].i == 1200, "rows intact after auto flush");
    nlsql_free_result(r); r = NULL;
    nlsql_close(c);

    c = nlsql_open(g_dir);
    CHECK(c != NULL, "reopen after auto flush");
    if (!c) return;
    q_ok(c, "USE af");
    CHECK(nlsql_query(c, "SELECT COUNT(*) FROM t", &r) == NLSQL_OK && r && r->rows[0].vals[0].i == 1200,
          "auto flushed rows persisted");
    nlsql_free_result(r); r = NULL;
    nlsql_close(c);
}

/* ---------- 凭据控制：参数 / 文件 / 强制认证 ---------- */
static void test_auth(void) {
    char conf[256];
    snprintf(conf, sizeof(conf), "/tmp/nlsql_authconf_%d.conf", (int)getpid());
    FILE* f = fopen(conf, "w");
    CHECK(f != NULL, "write auth file");
    if (!f) return;
    fputs("# credentials for test\nuser=app\npassword=secret123\n", f);
    fclose(f);

    nlsql_open_opts_t o;
    make_dir("auth");
    memset(&o, 0, sizeof(o));
    o.data_dir = g_dir;
    o.auth_file = conf;
    o.require_auth = 1;
    nlsql_conn_t* c = nlsql_open_ex(&o);
    CHECK(c != NULL, "open with auth file");
    if (c) {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT 1", &r) == NLSQL_ER_ACCESS_DENIED, "auth file gate before login");
        nlsql_free_result(r); r = NULL;
        CHECK(nlsql_authenticate(c, "app", "wrong") == NLSQL_ER_ACCESS_DENIED, "auth file wrong password");
        CHECK(nlsql_authenticate(c, "other", "secret123") == NLSQL_ER_ACCESS_DENIED, "auth file wrong user");
        CHECK(nlsql_authenticate(c, "app", "secret123") == NLSQL_OK, "auth file correct");
        CHECK(nlsql_query(c, "SELECT 1", &r) == NLSQL_OK && r && r->row_count == 1, "query after auth file login");
        nlsql_free_result(r);
        nlsql_close(c);
    }

    /* 参数方式：require_auth 但未配置凭据 → 仍拒绝（不静默放行） */
    make_dir("auth_force");
    memset(&o, 0, sizeof(o));
    o.data_dir = g_dir;
    o.require_auth = 1;
    c = nlsql_open_ex(&o);
    CHECK(c != NULL, "open require_auth without creds");
    if (c) {
        CHECK(nlsql_query(c, "SELECT 1", NULL) == NLSQL_ER_ACCESS_DENIED, "require_auth denies without creds");
        CHECK(nlsql_authenticate(c, "root", "") == NLSQL_ER_ACCESS_DENIED, "require_auth auth denied");
        nlsql_close(c);
    }

    /* 参数方式：直接传 user/password，配置即生效 */
    make_dir("auth_opts");
    memset(&o, 0, sizeof(o));
    o.data_dir = g_dir;
    o.user = "svc";
    o.password = "pw";
    c = nlsql_open_ex(&o);
    CHECK(c != NULL, "open with user/password opts");
    if (c) {
        CHECK(nlsql_query(c, "SELECT 1", NULL) == NLSQL_ER_ACCESS_DENIED, "opts creds gate queries");
        CHECK(nlsql_authenticate(c, "svc", "pw") == NLSQL_OK, "opts creds authenticate");
        CHECK(nlsql_query(c, "SELECT 1", NULL) == NLSQL_OK, "query after opts auth");
        nlsql_close(c);
    }

    /* 指定了凭据文件但不可读 → 打开失败，避免静默退化为无认证 */
    make_dir("auth_missing");
    memset(&o, 0, sizeof(o));
    o.data_dir = g_dir;
    o.auth_file = "/nonexistent/nlsql_auth.conf";
    CHECK(nlsql_open_ex(&o) == NULL, "missing auth file fails open");

    remove(conf);
}

static void test_ext_meta(void) {
    make_dir("extmeta");
    nlsql_conn_t* c = nlsql_open(g_dir);
    CHECK(c != NULL, "extmeta open");
    if (!c) return;

    /* 未选库时不能写元数据（无库目录） */
    CHECK(nlsql_ext_meta_put(c, "view", "v_demo", "", "SELECT 1") == NLSQL_ERR,
          "meta put without db -> err");

    q_ok(c, "CREATE DATABASE md");
    q_ok(c, "USE md");
    CHECK(nlsql_ext_meta_put(c, "view", "v_demo", "", "SELECT 1") == NLSQL_OK, "meta put");
    g_meta_hits = 0; g_meta_def[0] = '\0';
    CHECK(nlsql_ext_meta_each(c, meta_probe_cb, NULL) == 0 && g_meta_hits == 1 &&
          strcmp(g_meta_def, "SELECT 1") == 0, "meta each roundtrip");
    CHECK(nlsql_ext_meta_put(c, "view", "v_demo", "", "SELECT 2") == NLSQL_OK, "meta overwrite");
    g_meta_hits = 0; g_meta_def[0] = '\0';
    CHECK(nlsql_ext_meta_each(c, meta_probe_cb, NULL) == 0 && g_meta_hits == 1 &&
          strcmp(g_meta_def, "SELECT 2") == 0, "meta overwrite visible");

    /* 含 tab / 换行的定义必须原样往返（转义） */
    CHECK(nlsql_ext_meta_put(c, "trigger", "t_demo", "t1", "1\tINSERT INTO log VALUES (NEW.id)") == NLSQL_OK,
          "meta put with tab");
    g_meta_hits = 0; g_meta_def[0] = '\0';
    CHECK(nlsql_ext_meta_each(c, meta_probe_cb, NULL) == 0 && g_meta_hits == 1, "meta trigger kept");

    /* 连接注册表：当前连接可见，ID 单调非零，用户默认 root */
    nlsql_ext_proc_t procs[NLSQL_EXT_PROC_MAX];
    int np = nlsql_ext_proc_list(procs, NLSQL_EXT_PROC_MAX);
    int self_seen = 0;
    for (int i = 0; i < np; i++)
        if (procs[i].id == nlsql_ext_conn_id(c) && strcmp(procs[i].db, "md") == 0 &&
            strcmp(procs[i].user, "root") == 0) self_seen = 1;
    CHECK(np >= 1 && self_seen, "proc list contains self with db/user");

    nlsql_flush(c);
    nlsql_close(c);

    /* 重开同一目录：sidecar 仍在 */
    c = nlsql_open(g_dir);
    CHECK(c != NULL, "extmeta reopen");
    if (!c) return;
    q_ok(c, "USE md");
    g_meta_hits = 0; g_meta_def[0] = '\0';
    CHECK(nlsql_ext_meta_each(c, meta_probe_cb, NULL) == 0 && g_meta_hits == 1 &&
          strcmp(g_meta_def, "SELECT 2") == 0, "meta persisted after reopen");

    CHECK(nlsql_ext_meta_del(c, "view", "v_demo") == NLSQL_OK, "meta del");
    g_meta_hits = 0;
    CHECK(nlsql_ext_meta_each(c, meta_probe_cb, NULL) == 0 && g_meta_hits == 0, "meta del visible");
    CHECK(nlsql_ext_meta_del(c, "view", "v_demo") == NLSQL_OK, "meta del missing -> ok");

    nlsql_close(c);
}

int main(void) {
    printf("NLSQL %s functional tests\n", nlsql_version());
    test_basic();
    test_stmt();
    test_persist();
    test_async();
    test_readonly_lock();
    test_no_db();
    test_predicates();
    test_alter();
    test_agg_more();
    test_submit();
    test_errcodes();
    test_metadata();
    test_if_exists();
    test_auth();
    test_ext_meta();
    test_auto_flush();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
