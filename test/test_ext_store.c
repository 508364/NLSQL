/**
 * @file test_ext_store.c
 * @brief store 扩展测试：事务快照回滚/提交的行数、二级/唯一索引查询结果与 SHOW INDEX
 */
#include "netleaf_nlsql.h"
#include "netleaf_nlsql_ext.h"
#include "nlsql_test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

int main(void) {
    void* ext = test_ext_open("nlsql_ext_store");
    CHECK(ext != NULL, "load nlsql_ext_store plugin");
    if (!ext) { printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
    int (*ext_init)(void) = (int (*)(void))nlsql_dl_sym(ext, "nlsql_ext_store_init");
    CHECK(ext_init != NULL, "dlsym nlsql_ext_store_init");

    char ddir[256];
    test_dir(ddir, sizeof(ddir), "nlsql_ext_store");
    nlsql_conn_t* c = nlsql_open(ddir);
    CHECK(c != NULL, "open connection");
    if (!c || !ext_init) { printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
    CHECK(ext_init() == 0, "nlsql_ext_store_init");
    CHECK(nlsql_ext_feature("index") == 1, "feature index enabled");

    CHECK(nlsql_query(c, "CREATE DATABASE s", NULL) == NLSQL_OK, "create database s");
    CHECK(nlsql_query(c, "USE s", NULL) == NLSQL_OK, "use s");
    CHECK(nlsql_query(c, "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(16), score DOUBLE)", NULL) == NLSQL_OK,
          "create table users");
    CHECK(nlsql_query(c, "INSERT INTO users VALUES (1, 'alice', 95.5), (2, 'bob', 82.0), (3, 'carol', 77.25)", NULL) == NLSQL_OK,
          "insert 3 rows");

    long long n = 0;
    CHECK(test_count_rows(c, "users", &n) && n == 3, "baseline count = 3");

    /* 回滚：事务内可见 4 行，回滚后恢复 3 行 */
    CHECK(nlsql_query(c, "BEGIN", NULL) == NLSQL_OK, "begin transaction");
    CHECK(nlsql_query(c, "INSERT INTO users VALUES (4, 'dave', 60.0)", NULL) == NLSQL_OK, "insert in txn");
    CHECK(test_count_rows(c, "users", &n) && n == 4, "txn sees 4 rows");
    CHECK(nlsql_query(c, "ROLLBACK", NULL) == NLSQL_OK, "rollback");
    CHECK(test_count_rows(c, "users", &n) && n == 3, "rollback restores 3 rows");
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT COUNT(*) FROM users WHERE name = 'dave'", &r) == NLSQL_OK &&
              r && r->rows[0].vals[0].i == 0, "rolled back row is gone");
        nlsql_free_result(r);
    }

    /* 提交：START TRANSACTION / COMMIT 持久化 4 行 */
    CHECK(nlsql_query(c, "START TRANSACTION", NULL) == NLSQL_OK, "start transaction");
    CHECK(nlsql_query(c, "INSERT INTO users VALUES (4, 'dave', 60.0)", NULL) == NLSQL_OK, "insert 4th in txn");
    CHECK(nlsql_query(c, "COMMIT", NULL) == NLSQL_OK, "commit");
    CHECK(test_count_rows(c, "users", &n) && n == 4, "commit keeps 4 rows");

    /* 二级索引：等值查询返回值正确 */
    CHECK(nlsql_query(c, "CREATE INDEX idx_score ON users (score)", NULL) == NLSQL_OK, "create index idx_score");
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT name FROM users WHERE score = 82", &r) == NLSQL_OK &&
              r && r->row_count == 1 && strcmp(r->rows[0].vals[0].s, "bob") == 0,
              "index equality lookup returns bob");
        nlsql_free_result(r);
    }
    /* 索引维护：新插入行也能被索引查到 */
    CHECK(nlsql_query(c, "INSERT INTO users VALUES (5, 'erin', 91.0)", NULL) == NLSQL_OK, "insert 5th");
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT name FROM users WHERE score = 91", &r) == NLSQL_OK &&
              r && r->row_count == 1 && strcmp(r->rows[0].vals[0].s, "erin") == 0,
              "index sees newly inserted row");
        nlsql_free_result(r);
    }

    /* 唯一索引：重复值必须返回 1062 */
    CHECK(nlsql_query(c, "CREATE UNIQUE INDEX uq_name ON users (name)", NULL) == NLSQL_OK, "create unique index uq_name");
    {
        int rc = nlsql_query(c, "INSERT INTO users VALUES (6, 'alice', 50.0)", NULL);
        CHECK(rc == NLSQL_ER_DUP_ENTRY, "duplicate unique index -> 1062");
        CHECK(nlsql_error(c) && strstr(nlsql_error(c), "Duplicate") != NULL, "dup entry message");
    }

    /* SHOW INDEX 反映索引清单（主键 + idx_score + uq_name） */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SHOW INDEX FROM users", &r) == NLSQL_OK && r && r->col_count == 13,
              "show index 13 columns");
        int has_primary = 0, has_score = 0, has_uq = 0;
        for (uint64_t i = 0; r && i < r->row_count; i++) {
            const char* key = r->rows[i].vals[2].s;
            if (key && strcmp(key, "PRIMARY") == 0) has_primary = 1;
            if (key && strcmp(key, "idx_score") == 0) { has_score = 1;
                CHECK(r->rows[i].vals[1].s && strcmp(r->rows[i].vals[1].s, "1") == 0,
                      "idx_score Non_unique = 1"); }
            if (key && strcmp(key, "uq_name") == 0) { has_uq = 1;
                CHECK(r->rows[i].vals[1].s && strcmp(r->rows[i].vals[1].s, "0") == 0,
                      "uq_name Non_unique = 0"); }
        }
        CHECK(has_primary && has_score && has_uq, "show index lists PRIMARY/idx_score/uq_name");
        nlsql_free_result(r);
    }

    /* DROP INDEX 后不再出现在 SHOW INDEX */
    CHECK(nlsql_query(c, "DROP INDEX idx_score ON users", NULL) == NLSQL_OK, "drop index idx_score");
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SHOW INDEX FROM users", &r) == NLSQL_OK && r, "show index after drop");
        int has_score = 0;
        for (uint64_t i = 0; r && i < r->row_count; i++) {
            const char* key = r->rows[i].vals[2].s;
            if (key && strcmp(key, "idx_score") == 0) has_score = 1;
        }
        CHECK(!has_score, "idx_score removed from SHOW INDEX");
        nlsql_free_result(r);
    }
    /* 删掉的索引不影响查询正确性（走普通扫描） */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT name FROM users WHERE score = 82", &r) == NLSQL_OK &&
              r && r->row_count == 1 && strcmp(r->rows[0].vals[0].s, "bob") == 0,
              "query still correct after drop index");
        nlsql_free_result(r);
    }

    nlsql_close(c);
    nlsql_dl_close(ext);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
