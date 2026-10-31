/**
 * @file test_ext_sql.c
 * @brief SQL 内核扩展测试：JOIN/LEFT JOIN/子查询/视图/触发器的结果值与错误码
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

static int msg_count(nlsql_conn_t* c, const char* msg, long long* out) {
    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM logs WHERE msg = '%s'", msg);
    nlsql_result_t* r = NULL;
    int ok = (nlsql_query(c, sql, &r) == NLSQL_OK && r && r->row_count == 1);
    if (ok) *out = (long long)r->rows[0].vals[0].i;
    nlsql_free_result(r);
    return ok;
}

int main(void) {
    void* ext = test_ext_open("nlsql_ext_sql");
    CHECK(ext != NULL, "load nlsql_ext_sql plugin");
    if (!ext) { printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
    int (*ext_init)(void) = (int (*)(void))nlsql_dl_sym(ext, "nlsql_ext_sql_init");
    CHECK(ext_init != NULL, "dlsym nlsql_ext_sql_init");

    char ddir[256];
    test_dir(ddir, sizeof(ddir), "nlsql_ext_sql");
    nlsql_conn_t* c = nlsql_open(ddir);
    CHECK(c != NULL, "open connection");
    if (!c || !ext_init) { printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
    CHECK(ext_init() == 0, "nlsql_ext_sql_init");
    CHECK(nlsql_ext_feature("sql_kernel") == 1, "feature sql_kernel enabled");

    CHECK(nlsql_query(c, "CREATE DATABASE k", NULL) == NLSQL_OK, "create database k");
    CHECK(nlsql_query(c, "USE k", NULL) == NLSQL_OK, "use k");
    CHECK(nlsql_query(c, "CREATE TABLE a (id INT PRIMARY KEY, name VARCHAR(16))", NULL) == NLSQL_OK, "create a");
    CHECK(nlsql_query(c, "CREATE TABLE b (id INT PRIMARY KEY, aname VARCHAR(16))", NULL) == NLSQL_OK, "create b");
    CHECK(nlsql_query(c, "CREATE TABLE logs (id INT PRIMARY KEY AUTO_INCREMENT, msg VARCHAR(32))", NULL) == NLSQL_OK,
          "create logs");
    CHECK(nlsql_query(c, "INSERT INTO a VALUES (1, 'a1'), (2, 'a2'), (3, 'a3')", NULL) == NLSQL_OK, "insert a");
    CHECK(nlsql_query(c, "INSERT INTO b VALUES (1, 'b1'), (3, 'b3')", NULL) == NLSQL_OK, "insert b");

    /* INNER JOIN：2 行，值为 a1/b1、a3/b3 */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT a.name, b.aname FROM a JOIN b ON a.id = b.id ORDER BY a.id", &r) == NLSQL_OK,
              "inner join executes");
        CHECK(r && r->row_count == 2 && r->col_count == 2, "inner join 2 rows x 2 cols");
        if (r && r->row_count == 2) {
            CHECK(strcmp(r->cols[0].name, "name") == 0 && strcmp(r->cols[1].name, "aname") == 0,
                  "inner join column names");
            CHECK(strcmp(r->rows[0].vals[0].s, "a1") == 0 && strcmp(r->rows[0].vals[1].s, "b1") == 0,
                  "inner join row1 = a1/b1");
            CHECK(strcmp(r->rows[1].vals[0].s, "a3") == 0 && strcmp(r->rows[1].vals[1].s, "b3") == 0,
                  "inner join row2 = a3/b3");
        }
        nlsql_free_result(r);
    }

    /* LEFT JOIN：3 行，第三行右侧为 NULL */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT a.name, b.aname FROM a LEFT JOIN b ON a.id = b.id ORDER BY a.id", &r) == NLSQL_OK,
              "left join executes");
        CHECK(r && r->row_count == 3, "left join 3 rows");
        if (r && r->row_count == 3) {
            CHECK(strcmp(r->rows[0].vals[0].s, "a1") == 0 && strcmp(r->rows[0].vals[1].s, "b1") == 0,
                  "left join row1 matched");
            CHECK(strcmp(r->rows[1].vals[0].s, "a2") == 0 && r->rows[1].vals[1].is_null,
                  "left join row2 unmatched -> NULL");
            CHECK(strcmp(r->rows[2].vals[0].s, "a3") == 0 && strcmp(r->rows[2].vals[1].s, "b3") == 0,
                  "left join row3 matched");
        }
        nlsql_free_result(r);
    }

    /* 子查询 IN：id 属于 b 的 a 行（a1、a3） */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT name FROM a WHERE id IN (SELECT id FROM b) ORDER BY id", &r) == NLSQL_OK,
              "subquery IN executes");
        CHECK(r && r->row_count == 2, "IN subquery 2 rows");
        if (r && r->row_count == 2) {
            CHECK(strcmp(r->rows[0].vals[0].s, "a1") == 0, "IN row1 = a1");
            CHECK(strcmp(r->rows[1].vals[0].s, "a3") == 0, "IN row2 = a3");
        }
        nlsql_free_result(r);
    }

    /* EXISTS：有匹配时全表返回，无匹配时 0 行 */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT name FROM a WHERE EXISTS (SELECT * FROM b WHERE b.id = 3)", &r) == NLSQL_OK,
              "EXISTS executes");
        CHECK(r && r->row_count == 3, "EXISTS true -> all 3 rows");
        nlsql_free_result(r); r = NULL;
        CHECK(nlsql_query(c, "SELECT name FROM a WHERE EXISTS (SELECT * FROM b WHERE b.id = 99)", &r) == NLSQL_OK &&
              r && r->row_count == 0, "EXISTS false -> 0 rows");
        nlsql_free_result(r);
    }

    /* 视图：注册后可查询，值正确；DROP 后回到核心报 1146 */
    CHECK(nlsql_query(c, "CREATE VIEW v AS SELECT id, name FROM a", NULL) == NLSQL_OK, "create view v");
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT * FROM v", &r) == NLSQL_OK, "select * from view");
        CHECK(r && r->row_count == 3 && r->col_count == 2, "view returns 3 rows x 2 cols");
        if (r && r->row_count == 3) {
            CHECK(r->rows[0].vals[0].i == 1 && strcmp(r->rows[0].vals[1].s, "a1") == 0,
                  "view row1 = 1/a1");
            CHECK(r->rows[2].vals[0].i == 3 && strcmp(r->rows[2].vals[1].s, "a3") == 0,
                  "view row3 = 3/a3");
        }
        nlsql_free_result(r); r = NULL;
        CHECK(nlsql_query(c, "SELECT name FROM v WHERE id = 2", &r) == NLSQL_OK &&
              r && r->row_count == 1 && strcmp(r->rows[0].vals[0].s, "a2") == 0,
              "view with WHERE returns a2");
        nlsql_free_result(r);
    }

    /* 触发器：AFTER INSERT / AFTER DELETE 写入日志表 */
    CHECK(nlsql_query(c, "CREATE TRIGGER trg_ins AFTER INSERT ON a FOR EACH ROW INSERT INTO logs (msg) VALUES ('inserted')",
                      NULL) == NLSQL_OK, "create insert trigger");
    CHECK(nlsql_query(c, "INSERT INTO a VALUES (4, 'a4')", NULL) == NLSQL_OK, "insert row with trigger");
    long long n = 0;
    CHECK(test_count_rows(c, "logs", &n) && n == 1, "insert trigger wrote 1 log row");
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT msg FROM logs", &r) == NLSQL_OK && r && r->row_count == 1 &&
              strcmp(r->rows[0].vals[0].s, "inserted") == 0, "log msg = 'inserted'");
        nlsql_free_result(r);
    }

    /* 触发器内不含触发器体执行时，DML 本身影响行数仍正确 */
    CHECK(nlsql_query(c, "CREATE TRIGGER trg_del AFTER DELETE ON a FOR EACH ROW INSERT INTO logs (msg) VALUES ('deleted')",
                      NULL) == NLSQL_OK, "create delete trigger");
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "DELETE FROM a WHERE id = 4", &r) == NLSQL_OK, "delete row with trigger");
        CHECK(r && r->affected == 1, "delete affected = 1");
        nlsql_free_result(r);
    }
    CHECK(test_count_rows(c, "logs", &n) && n == 2, "delete trigger wrote second log row");

    CHECK(nlsql_query(c, "DROP TRIGGER trg_ins", NULL) == NLSQL_OK, "drop insert trigger");
    CHECK(nlsql_query(c, "INSERT INTO a VALUES (5, 'a5')", NULL) == NLSQL_OK, "insert after trigger drop");
    CHECK(test_count_rows(c, "logs", &n) && n == 2, "dropped trigger no longer fires");

    CHECK(nlsql_query(c, "DROP VIEW v", NULL) == NLSQL_OK, "drop view v");
    {
        nlsql_result_t* r = NULL;
        int rc = nlsql_query(c, "SELECT * FROM v", &r);
        CHECK(rc == NLSQL_ER_NO_SUCH_TABLE, "view dropped -> 1146");
        nlsql_free_result(r);
    }

    /* 普通 SELECT 仍由核心处理（扩展不接管），验证无回归 */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT COUNT(*) FROM a", &r) == NLSQL_OK && r && r->rows[0].vals[0].i == 4,
              "core SELECT count(a) = 4");
        nlsql_free_result(r);
    }

    /* ---- 相关子查询：按外层行求值 ---- */
    {
        nlsql_result_t* r = NULL;
        CHECK(nlsql_query(c, "SELECT name FROM a WHERE EXISTS (SELECT * FROM b WHERE b.id = a.id) ORDER BY id", &r) == NLSQL_OK,
              "correlated EXISTS executes");
        CHECK(r && r->row_count == 2, "correlated EXISTS 2 rows");
        if (r && r->row_count == 2)
            CHECK(strcmp(r->rows[0].vals[0].s, "a1") == 0 && strcmp(r->rows[1].vals[0].s, "a3") == 0,
                  "correlated EXISTS rows a1/a3");
        nlsql_free_result(r); r = NULL;

        CHECK(nlsql_query(c, "SELECT id, (SELECT aname FROM b WHERE b.id = a.id) AS bn FROM a WHERE id <= 2 ORDER BY id", &r) == NLSQL_OK,
              "correlated scalar executes");
        CHECK(r && r->row_count == 2 && r->col_count == 2, "correlated scalar 2 rows x 2 cols");
        if (r && r->row_count == 2) {
            CHECK(strcmp(r->rows[0].vals[1].s, "b1") == 0, "correlated scalar row1 = b1");
            CHECK(r->rows[1].vals[1].is_null, "correlated scalar row2 = NULL");
        }
        nlsql_free_result(r); r = NULL;

        CHECK(nlsql_query(c, "SELECT name FROM a WHERE id IN (SELECT id FROM b WHERE b.id = a.id) ORDER BY id", &r) == NLSQL_OK &&
              r && r->row_count == 2, "correlated IN 2 rows");
        nlsql_free_result(r);
    }

    /* ---- 触发器 NEW / OLD ---- */
    CHECK(nlsql_query(c, "CREATE TABLE src (id INT PRIMARY KEY, v VARCHAR(16))", NULL) == NLSQL_OK, "create src");
    CHECK(nlsql_query(c, "CREATE TRIGGER trg_new AFTER INSERT ON src FOR EACH ROW INSERT INTO logs (msg) VALUES (NEW.v)",
                      NULL) == NLSQL_OK, "create NEW trigger");
    CHECK(nlsql_query(c, "INSERT INTO src VALUES (1, 'n1')", NULL) == NLSQL_OK, "insert src row");
    {
        long long m = 0;
        CHECK(msg_count(c, "n1", &m) && m == 1, "NEW.v expanded to n1");
    }
    CHECK(nlsql_query(c, "CREATE TRIGGER trg_old_upd AFTER UPDATE ON src FOR EACH ROW INSERT INTO logs (msg) VALUES (OLD.v)",
                      NULL) == NLSQL_OK, "create OLD update trigger");
    CHECK(nlsql_query(c, "UPDATE src SET v = 'n2' WHERE id = 1", NULL) == NLSQL_OK, "update src row");
    {
        long long m = 0;
        CHECK(msg_count(c, "n1", &m) && m == 2, "OLD.v on update = previous n1");
    }
    CHECK(nlsql_query(c, "CREATE TRIGGER trg_old_del AFTER DELETE ON src FOR EACH ROW INSERT INTO logs (msg) VALUES (OLD.v)",
                      NULL) == NLSQL_OK, "create OLD delete trigger");
    CHECK(nlsql_query(c, "DELETE FROM src WHERE id = 1", NULL) == NLSQL_OK, "delete src row");
    {
        long long m = 0;
        CHECK(msg_count(c, "n2", &m) && m == 1, "OLD.v on delete = n2");
    }

    /* ---- sidecar 持久化：卸载扩展后重开，注册表从零重建 ---- */
    CHECK(nlsql_query(c, "CREATE VIEW v2 AS SELECT id, name FROM a", NULL) == NLSQL_OK, "create view v2");
    CHECK(nlsql_query(c, "CREATE TRIGGER trg_p AFTER INSERT ON a FOR EACH ROW INSERT INTO logs (msg) VALUES ('keep')",
                      NULL) == NLSQL_OK, "create persist trigger");
    CHECK(nlsql_flush(c) == NLSQL_OK, "flush before restart");
    nlsql_close(c);
    {
        void (*ext_shutdown)(void) = (void (*)(void))nlsql_dl_sym(ext, "nlsql_ext_sql_shutdown");
        if (ext_shutdown) ext_shutdown();
    }
    nlsql_dl_close(ext);
    ext = NULL;

    ext = test_ext_open("nlsql_ext_sql");
    CHECK(ext != NULL, "reload nlsql_ext_sql plugin (fresh registry)");
    if (ext) {
        int (*init2)(void) = (int (*)(void))nlsql_dl_sym(ext, "nlsql_ext_sql_init");
        nlsql_conn_t* c2 = nlsql_open(ddir);
        CHECK(c2 != NULL && init2 && init2() == 0, "reopen connection + re-init");
        if (c2 && init2) {
            CHECK(nlsql_query(c2, "USE k", NULL) == NLSQL_OK, "reuse db k");
            nlsql_result_t* r = NULL;
            CHECK(nlsql_query(c2, "SELECT * FROM v2", &r) == NLSQL_OK && r && r->row_count == 4,
                  "restored view v2 has 4 rows");
            nlsql_free_result(r); r = NULL;
            CHECK(nlsql_query(c2, "INSERT INTO a VALUES (6, 'a6')", NULL) == NLSQL_OK,
                  "insert with restored trigger");
            {
                long long m = 0;
                CHECK(msg_count(c2, "keep", &m) && m == 1, "restored trigger wrote 'keep'");
            }
            char meta[512];
            snprintf(meta, sizeof(meta), "%s/k/nlsql_ext_meta.dat", ddir);
            FILE* f = fopen(meta, "rb");
            CHECK(f != NULL, "sidecar file exists");
            char buf[4096] = "";
            if (f) {
                size_t got = fread(buf, 1, sizeof(buf) - 1, f);
                buf[got] = '\0';
                fclose(f);
            }
            CHECK(strstr(buf, "view\tv2") != NULL, "sidecar contains view v2");
            CHECK(strstr(buf, "trigger\ttrg_p") != NULL, "sidecar contains trigger trg_p");
            nlsql_close(c2);
        }
        nlsql_dl_close(ext);
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
