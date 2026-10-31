/**
 * @file test_ext_info.c
 * @brief information_schema 虚拟库扩展测试：SCHEMATA/TABLES/COLUMNS/VIEWS/TRIGGERS/STATISTICS
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

/* 结果集中查找字符串列（col 列）是否包含某值 */
static int has_str(nlsql_result_t* r, int col, const char* val) {
    if (!r || col < 0 || col >= r->col_count) return 0;
    for (uint64_t i = 0; i < r->row_count; i++)
        if (!r->rows[i].vals[col].is_null && r->rows[i].vals[col].s &&
            strcmp(r->rows[i].vals[col].s, val) == 0) return 1;
    return 0;
}

static void* load_ext(const char* name, const char* init_sym) {
    void* h = test_ext_open(name);
    if (!h) return NULL;
    if (test_ext_init(h, init_sym) != 0) { nlsql_dl_close(h); return NULL; }
    return h;
}

int main(void) {
    void* info = load_ext("nlsql_ext_info", "nlsql_ext_info_init");
    CHECK(info != NULL, "load nlsql_ext_info plugin + init");
    if (!info) { printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
    /* 视图/触发器/索引由其他扩展提供；缺失时相应断言自然不满足 */
    void* sqlx = load_ext("nlsql_ext_sql", "nlsql_ext_sql_init");
    void* store = load_ext("nlsql_ext_store", "nlsql_ext_store_init");
    CHECK(sqlx != NULL, "load nlsql_ext_sql plugin + init");
    CHECK(store != NULL, "load nlsql_ext_store plugin + init");
    CHECK(nlsql_ext_feature("information_schema") == 1, "feature information_schema enabled");

    char ddir[256];
    test_dir(ddir, sizeof(ddir), "nlsql_ext_info");
    nlsql_conn_t* c = nlsql_open(ddir);
    CHECK(c != NULL, "open connection");
    if (!c) { printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }

    CHECK(nlsql_query(c, "CREATE DATABASE isdb", NULL) == NLSQL_OK, "create database isdb");
    CHECK(nlsql_query(c, "USE isdb", NULL) == NLSQL_OK, "use isdb");
    CHECK(nlsql_query(c, "CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR(16))", NULL) == NLSQL_OK, "create t1");
    CHECK(nlsql_query(c, "CREATE TABLE t2 (id INT PRIMARY KEY)", NULL) == NLSQL_OK, "create t2");
    CHECK(nlsql_query(c, "INSERT INTO t1 VALUES (1, 'x'), (2, 'y')", NULL) == NLSQL_OK, "insert t1");
    CHECK(nlsql_query(c, "CREATE VIEW v_meta AS SELECT id, name FROM t1", NULL) == NLSQL_OK, "create view v_meta");
    CHECK(nlsql_query(c, "CREATE TRIGGER trg_meta AFTER INSERT ON t1 FOR EACH ROW INSERT INTO t2 (id) VALUES (NEW.id)",
                      NULL) == NLSQL_OK, "create trigger trg_meta");
    CHECK(nlsql_query(c, "CREATE INDEX idx_name ON t1 (name)", NULL) == NLSQL_OK, "create index idx_name");

    /* USE information_schema：返回 OK 且不改变真实当前库 */
    CHECK(nlsql_query(c, "USE information_schema", NULL) == NLSQL_OK, "use information_schema");
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SHOW TABLES", &r) == NLSQL_OK && r && r->row_count == 2,
              "real current db unchanged (SHOW TABLES = t1,t2)");
        nlsql_free_result(r);
    }

    /* SCHEMATA */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT SCHEMA_NAME FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = 'isdb'", &r) == NLSQL_OK,
              "SCHEMATA executes");
        CHECK(r && r->row_count == 1 && has_str(r, 0, "isdb"), "SCHEMATA has isdb");
        nlsql_free_result(r);
    }

    /* TABLES：全量 + 过滤 + 排序 + LIMIT */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT * FROM information_schema.TABLES", &r) == NLSQL_OK, "TABLES executes");
        CHECK(r && r->col_count == 6 && strcmp(r->cols[2].name, "TABLE_NAME") == 0, "TABLES 6 cols, TABLE_NAME");
        CHECK(has_str(r, 2, "t1") && has_str(r, 1, "isdb"), "TABLES contains isdb.t1");
        nlsql_free_result(r); r = NULL;

        CHECK(nlsql_query(c, "SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = 'isdb' ORDER BY TABLE_NAME",
                          &r) == NLSQL_OK, "TABLES filter+order executes");
        CHECK(r && r->row_count == 2 && has_str(r, 0, "t1") && has_str(r, 0, "t2"),
              "TABLES isdb -> t1,t2");
        nlsql_free_result(r); r = NULL;

        CHECK(nlsql_query(c, "SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = 'isdb' ORDER BY TABLE_NAME LIMIT 1",
                          &r) == NLSQL_OK && r && r->row_count == 1 &&
              strcmp(r->rows[0].vals[0].s, "t1") == 0, "TABLES ORDER BY + LIMIT 1 -> t1");
        nlsql_free_result(r);
    }

    /* COLUMNS */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT COLUMN_NAME, DATA_TYPE FROM information_schema.COLUMNS WHERE TABLE_NAME = 't1' ORDER BY ORDINAL_POSITION",
                          &r) == NLSQL_OK, "COLUMNS executes");
        CHECK(r && r->row_count == 2 && strcmp(r->cols[0].name, "COLUMN_NAME") == 0, "COLUMNS 2 rows");
        if (r && r->row_count == 2)
            CHECK(strcmp(r->rows[0].vals[0].s, "id") == 0 && strcmp(r->rows[1].vals[0].s, "name") == 0,
                  "COLUMNS t1 -> id,name");
        nlsql_free_result(r);
    }

    /* VIEWS / TRIGGERS：来自 sidecar 元数据 */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT TABLE_NAME FROM information_schema.VIEWS WHERE TABLE_SCHEMA = 'isdb'", &r) == NLSQL_OK,
              "VIEWS executes");
        CHECK(r && has_str(r, 0, "v_meta"), "VIEWS contains v_meta");
        nlsql_free_result(r); r = NULL;

        CHECK(nlsql_query(c, "SELECT TRIGGER_NAME, EVENT_OBJECT_TABLE FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA = 'isdb'",
                          &r) == NLSQL_OK, "TRIGGERS executes");
        CHECK(r && r->row_count == 1 && has_str(r, 0, "trg_meta") && has_str(r, 1, "t1"),
              "TRIGGERS contains trg_meta on t1");
        nlsql_free_result(r);
    }

    /* STATISTICS：来自 SHOW INDEX */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT INDEX_NAME, COLUMN_NAME FROM information_schema.STATISTICS WHERE TABLE_NAME = 't1'",
                          &r) == NLSQL_OK, "STATISTICS executes");
        CHECK(r && r->row_count >= 1 && has_str(r, 0, "idx_name"), "STATISTICS contains idx_name");
        nlsql_free_result(r);
    }

    /* 视图模式下可不带库前缀 */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT TABLE_NAME FROM TABLES WHERE TABLE_SCHEMA = 'isdb'", &r) == NLSQL_OK &&
              r && r->row_count == 2, "unqualified TABLES under information_schema");
        nlsql_free_result(r);
    }

    /* 不存在的虚拟表 -> 1146 */
    CHECK(nlsql_query(c, "SELECT * FROM information_schema.NO_SUCH_TABLE", NULL) == NLSQL_ER_NO_SUCH_TABLE,
          "unknown information_schema table -> 1146");

    /* USE 真实库后退出视图模式：无前缀 TABLES 不再由本扩展接管 */
    CHECK(nlsql_query(c, "USE isdb", NULL) == NLSQL_OK, "use isdb again");
    CHECK(nlsql_query(c, "SELECT * FROM TABLES", NULL) != NLSQL_OK,
          "after USE isdb unqualified TABLES no longer served by extension");
    /* 带前缀仍可用 */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = 'isdb'", &r) == NLSQL_OK &&
              r && r->row_count == 2, "prefixed TABLES works after leaving view mode");
        nlsql_free_result(r);
    }

    nlsql_close(c);
    if (store) nlsql_dl_close(store);
    if (sqlx) nlsql_dl_close(sqlx);
    nlsql_dl_close(info);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
