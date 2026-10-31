/**
 * @file test_recover.c
 * @brief NLSQL 存储可靠性测试：崩溃恢复 / 强杀 / journal 截断容错
 *
 * 场景：
 *   1) 子进程写入后 _exit 崩溃（未 flush 的 64KB 用户态缓冲丢失，已落盘数据必须完好）
 *   2) 写入过程中强杀（POSIX SIGKILL / Windows TerminateProcess）→ 重放恢复，引擎可打开、内部一致、可继续写
 *   3) journal 尾部截断半条记录 → 容错重放（完整前缀生效，不崩溃）
 *   4) 崩溃-恢复循环 → 行数稳定不腐化
 *   5) 压实中途崩溃（LSN 对齐）→ 新快照表不重复、旧快照表差量补放、两边一致
 *
 * 子进程：POSIX 用 fork()，Windows 用 CreateProcess 重跑自身子命令（见 main 分发）。
 */
#include "netleaf_nlsql.h"
#include "netleaf_nlsql_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <signal.h>
#endif
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

/* 子进程模式 */
enum { CHILD_EXIT = 0, CHILD_STREAM = 1, CHILD_TRUNC = 2 };

static void q_ok(nlsql_conn_t* c, const char* sql) {
    int rc = nlsql_query(c, sql, NULL);
    if (rc != NLSQL_OK)
        printf("  [%s] err=%d msg=%s\n", sql, rc, nlsql_error(c));
}

static int64_t count_of(nlsql_conn_t* c, const char* table) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    nlsql_result_t* r = NULL;
    int64_t n = -1;
    if (nlsql_query(c, sql, &r) == NLSQL_OK && r && r->row_count == 1)
        n = r->rows[0].vals[0].i;
    else
        printf("  count failed: %s\n", nlsql_error(c));
    nlsql_free_result(r);
    return n;
}

/* 全表扫描行数与 COUNT(*) 一致性 + 主键点查一致性抽样 */
static void check_consistent(nlsql_conn_t* c, const char* table) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    nlsql_result_t* r = NULL;
    CHECK(nlsql_query(c, sql, &r) == NLSQL_OK && r && r->row_count == 1, "count query ok");
    int64_t cnt = (r && r->row_count == 1) ? r->rows[0].vals[0].i : -1;
    nlsql_free_result(r); r = NULL;

    snprintf(sql, sizeof(sql), "SELECT id FROM %s ORDER BY id", table);
    CHECK(nlsql_query(c, sql, &r) == NLSQL_OK && r, "scan ids ok");
    if (r) {
        CHECK((int64_t)r->row_count == cnt, "full scan == count(*)");
        /* 抽样主键点查：前 8 个 id 必须各自命中 1 行（索引与数据一致） */
        int samples = r->row_count < 8 ? (int)r->row_count : 8;
        for (int i = 0; i < samples; i++) {
            int64_t id = r->rows[i].vals[0].i;
            char s2[256];
            snprintf(s2, sizeof(s2), "SELECT COUNT(*) FROM %s WHERE id = %lld",
                     table, (long long)id);
            nlsql_result_t* r2 = NULL;
            if (nlsql_query(c, s2, &r2) == NLSQL_OK && r2 && r2->row_count == 1)
                CHECK(r2->rows[0].vals[0].i == 1, "pk point lookup hits 1");
            else
                CHECK(0, "pk point lookup query");
            nlsql_free_result(r2);
        }
        nlsql_free_result(r); r = NULL;
    }
}

static void temp_dir(char* out, size_t cap, const char* tag) {
#ifdef _WIN32
    const char* base = getenv("TEMP");
    if (!base || !base[0]) base = ".";
#else
    const char* base = "/tmp";
#endif
    snprintf(out, cap, "%s/nlsql_recov_%s_%d_%llu", base, tag, (int)getpid(),
             (unsigned long long)nlsql_mono_ms());
}

static int file_exists(const char* p) {
    struct stat sb;
    return stat(p, &sb) == 0;
}

static void rm_dir(const char* dir) {
    char cmd[600];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "cmd /c rmdir /s /q \"%s\" 2>nul", dir);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
#endif
    if (system(cmd) != 0) printf("  warn: cleanup %s failed\n", dir);
}

/* 子进程实际逻辑：返回退出码；调用方用 _exit 模拟崩溃（不 close / 不 flush） */
static int child_run(int mode, const char* dir, const char* marker) {
    nlsql_conn_t* sc = nlsql_open(dir);
    if (!sc) return 1;
    if (mode == CHILD_EXIT) {
        nlsql_query(sc, "USE r1", NULL);
        for (int i = 0; i < 100; i++)
            nlsql_query(sc, "INSERT INTO t (v) VALUES ('lost')", NULL);
        return 0;
    }
    if (mode == CHILD_TRUNC) {
        nlsql_query(sc, "CREATE DATABASE r3", NULL);
        nlsql_query(sc, "USE r3", NULL);
        nlsql_query(sc, "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(24))", NULL);
        for (int i = 0; i < 3000; i++) {
            char sql[128];
            snprintf(sql, sizeof(sql),
                     "INSERT INTO t (v) VALUES ('value-padding-xxxxxxxxxx-%d')", i);
            nlsql_query(sc, sql, NULL);
        }
        return 0;
    }
    /* CHILD_STREAM：种子 + 标记文件，然后无限写入等父进程强杀 */
    nlsql_query(sc, "CREATE DATABASE r2", NULL);
    nlsql_query(sc, "USE r2", NULL);
    nlsql_query(sc, "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(24))", NULL);
    nlsql_query(sc, "INSERT INTO t (v) VALUES ('seed')", NULL);
    nlsql_flush(sc);
    int fd = open(marker, O_CREAT | O_WRONLY, 0600);
    if (fd >= 0) close(fd);
    for (;;) {
        if (nlsql_query(sc, "INSERT INTO t (v) VALUES ('stream-data')", NULL) != NLSQL_OK)
            return 2;
    }
}

/* ---------------- 子进程启动/等待/强杀 ---------------- */

#ifdef _WIN32
#define CHILD_KILL_CODE 0xDEADu
typedef HANDLE child_t;

static child_t spawn_child(int mode, const char* dir, const char* marker) {
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) return NULL;
    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "\"%s\" child %d \"%s\" \"%s\"",
             exe, mode, dir, marker ? marker : "");
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static int child_wait(child_t h, int* killed) {
    DWORD code = 0;
    if (!h) { *killed = 0; return -1; }
    WaitForSingleObject(h, INFINITE);
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    *killed = (code == CHILD_KILL_CODE);
    return *killed ? 0 : (int)code;
}

static void child_kill(child_t h) {
    if (h) TerminateProcess(h, CHILD_KILL_CODE);
}
#else
typedef pid_t child_t;

static child_t spawn_child(int mode, const char* dir, const char* marker) {
    pid_t pid = fork();
    if (pid == 0) _exit(child_run(mode, dir, marker));
    return pid;
}

static int child_wait(child_t pid, int* killed) {
    int st = 0;
    waitpid(pid, &st, 0);
    *killed = WIFSIGNALED(st) ? 1 : 0;
    return *killed ? 0 : WEXITSTATUS(st);
}

static void child_kill(child_t pid) {
    kill(pid, SIGKILL);
}
#endif

/* 场景 1+4：子进程 _exit 崩溃，缓冲丢失但已 flush 数据完好，循环多次不腐化 */
static void test_crash_exit(void) {
    char dir[300];
    temp_dir(dir, sizeof(dir), "exit");
    rm_dir(dir);

    nlsql_conn_t* c = nlsql_open(dir);
    CHECK(c != NULL, "open exit-recovery dir");
    if (!c) return;
    q_ok(c, "CREATE DATABASE r1");
    q_ok(c, "USE r1");
    q_ok(c, "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(24))");
    for (int i = 0; i < 500; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO t (v) VALUES ('persist-%d')", i);
        q_ok(c, sql);
    }
    nlsql_flush(c);   /* 落盘 + 压实快照 */
    nlsql_close(c);

    /* 循环 3 次：子进程 open 后各插 100 行（<64KB 缓冲，未落盘）然后 _exit 崩溃 */
    for (int round = 0; round < 3; round++) {
        child_t ch = spawn_child(CHILD_EXIT, dir, NULL);
        int killed = 0;
        int rc = child_wait(ch, &killed);
        CHECK(!killed && rc == 0, "crash child ran");

        c = nlsql_open(dir);
        CHECK(c != NULL, "reopen after _exit crash");
        if (!c) break;
        q_ok(c, "USE r1");
        CHECK(count_of(c, "t") == 500, "flushed 500 rows intact, unflushed lost");
        nlsql_close(c);
    }
    rm_dir(dir);
}

/* 场景 2：写入中强杀 → 重放恢复 + 内部一致 + 可继续写 */
static void test_sigkill(void) {
    char dir[300], marker[320];
    temp_dir(dir, sizeof(dir), "kill");
    snprintf(marker, sizeof(marker), "%s.marker", dir);
    rm_dir(dir);
    unlink(marker);

    child_t ch = spawn_child(CHILD_STREAM, dir, marker);
    int waited = 0;
    while (!file_exists(marker) && waited < 400) { nlsql_sleep_ms(5); waited++; }
    CHECK(file_exists(marker), "child ready marker");
    nlsql_sleep_ms(30);
    child_kill(ch);
    int killed = 0;
    child_wait(ch, &killed);
    CHECK(killed, "child killed");
    unlink(marker);

    nlsql_conn_t* c = nlsql_open(dir);
    CHECK(c != NULL, "open after kill");
    if (c) {
        q_ok(c, "USE r2");
        CHECK(count_of(c, "t") >= 1, "seed row recovered");
        check_consistent(c, "t");
        int64_t before = count_of(c, "t");

        /* 崩溃后继续写 → flush → 重开，语义必须一致 */
        for (int i = 0; i < 5; i++)
            q_ok(c, "INSERT INTO t (v) VALUES ('post-crash')");
        CHECK(count_of(c, "t") == before + 5, "writes after recovery");
        nlsql_flush(c);
        nlsql_close(c);

        c = nlsql_open(dir);
        CHECK(c != NULL, "reopen after post-crash flush");
        if (c) {
            q_ok(c, "USE r2");
            CHECK(count_of(c, "t") == before + 5, "post-crash rows persisted");
            check_consistent(c, "t");
            nlsql_close(c);
        }
    }
    rm_dir(dir);
}

/* 场景 3：journal 尾部截断半条记录 → 容错重放，完整前缀生效 */
static void test_truncated_journal(void) {
    char dir[300];
    temp_dir(dir, sizeof(dir), "trunc");
    rm_dir(dir);

    /* 子进程写入量 > 64KB 缓冲（触发自动落盘）后 _exit，保证 journal 有已落盘内容 */
    child_t ch = spawn_child(CHILD_TRUNC, dir, NULL);
    int killed = 0;
    int rc = child_wait(ch, &killed);
    CHECK(!killed && rc == 0, "trunc child wrote");

    /* 截掉 journal 最后 7 字节，制造半条记录 */
    char jpath[400];
    snprintf(jpath, sizeof(jpath), "%s/r3/journal.log", dir);
    struct stat sb;
    CHECK(stat(jpath, &sb) == 0 && sb.st_size > 64, "journal has flushed content");
    if (stat(jpath, &sb) == 0 && sb.st_size > 64) {
        CHECK(truncate(jpath, sb.st_size - 7) == 0, "truncate journal tail");
    }

    nlsql_conn_t* c = nlsql_open(dir);
    CHECK(c != NULL, "open with truncated journal");
    if (c) {
        q_ok(c, "USE r3");
        int64_t n = count_of(c, "t");
        CHECK(n >= 500 && n <= 3000, "complete prefix replayed (500..3000)");
        check_consistent(c, "t");
        /* 截断修复后可继续写并持久化 */
        q_ok(c, "INSERT INTO t (v) VALUES ('after-trunc')");
        CHECK(count_of(c, "t") == n + 1, "write after truncation recovery");
        nlsql_flush(c);
        nlsql_close(c);

        c = nlsql_open(dir);
        CHECK(c != NULL, "reopen after trunc recovery");
        if (c) {
            q_ok(c, "USE r3");
            CHECK(count_of(c, "t") == n + 1, "trunc recovery persisted");
            nlsql_close(c);
        }
    }
    rm_dir(dir);
}

static int copy_file(const char* src, const char* dst) {
    FILE* f = fopen(src, "rb");
    if (!f) return -1;
    FILE* g = fopen(dst, "wb");
    if (!g) { fclose(f); return -1; }
    char buf[4096];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        if (fwrite(buf, 1, n, g) != n) { rc = -1; break; }
    fclose(f);
    fclose(g);
    return rc;
}

/* 场景 5：压实中途崩溃的确定性复现（LSN 对齐）
   构造磁盘态 = ta 快照新(F2) + tb 快照旧(F1) + journal 含 (F1..F2] 完整记录，
   即 eng_flush 写完第 1 张表快照后被 kill。恢复后两表都必须精确等于 F2 状态：
   ta 跳过已含记录（不重复），tb 恰好补放差量（不丢不重）。 */
static void test_compact_mid_crash(void) {
    char dir[300];
    temp_dir(dir, sizeof(dir), "mid");
    rm_dir(dir);

    nlsql_conn_t* c = nlsql_open(dir);
    CHECK(c != NULL, "open mid-crash dir");
    if (!c) return;
    q_ok(c, "CREATE DATABASE r4");
    q_ok(c, "USE r4");
    q_ok(c, "CREATE TABLE ta (id INT PRIMARY KEY, v VARCHAR(200))");
    q_ok(c, "CREATE TABLE tb (id INT PRIMARY KEY, v VARCHAR(200))");
    for (int i = 0; i < 100; i++) {
        char sql[320];
        snprintf(sql, sizeof(sql), "INSERT INTO ta VALUES (%d, 'a-%d')", i, i);
        q_ok(c, sql);
        snprintf(sql, sizeof(sql), "INSERT INTO tb VALUES (%d, 'b-%d')", i, i);
        q_ok(c, sql);
    }
    nlsql_flush(c); /* flush1：两表快照 snap=F1，journal 清空 */

    char bak_tb[320], bak_jr[320], p_tb[320], p_jr[320];
    snprintf(bak_tb, sizeof(bak_tb), "%s/tb.f1.bak", dir);
    snprintf(bak_jr, sizeof(bak_jr), "%s/journal.f2.bak", dir);
    snprintf(p_tb, sizeof(p_tb), "%s/r4/tb.nlt", dir);
    snprintf(p_jr, sizeof(p_jr), "%s/r4/journal.log", dir);
    CHECK(copy_file(p_tb, bak_tb) == 0, "backup tb snapshot (F1)");

    for (int i = 100; i < 150; i++) { /* 第二批：落在 journal (F1..F2] */
        char sql[320];
        snprintf(sql, sizeof(sql), "INSERT INTO ta VALUES (%d, 'a-%d')", i, i);
        q_ok(c, sql);
        snprintf(sql, sizeof(sql), "INSERT INTO tb VALUES (%d, 'b-%d')", i, i);
        q_ok(c, sql);
    }
    /* 填充 >64KB：语句尾记录平时留在 jbuf（不落盘），用长行把第二批顶进磁盘文件，
       保证备份的 journal 完整包含 (F1..F2] 的 tb 记录（尾部残余填充行缺失无影响：
       ta 快照 F2 已含全部，tb 不涉及填充） */
    char pad[201];
    memset(pad, 'x', 200); pad[200] = '\0';
    for (int i = 0; i < 400; i++) {
        char sql[320];
        snprintf(sql, sizeof(sql), "INSERT INTO ta VALUES (%d, '%s')", 10000 + i, pad);
        q_ok(c, sql);
    }
    CHECK(copy_file(p_jr, bak_jr) == 0, "backup journal with batch-2 records");
    struct stat jsz; stat(p_jr, &jsz);
    printf("  [mid-crash] journal backup size=%lld\n", (long long)jsz.st_size);
    CHECK(jsz.st_size > 60 * 1024, "batch-2 pushed into journal file (>60KB)");
    nlsql_flush(c); /* flush2：本应两表 F2 + journal 清空；我们随后把磁盘回退到"压实一半" */
    nlsql_close(c);

    /* 回退成 eng_flush 中途崩溃态：ta=F2（已保存）、tb=F1（未保存）、journal=完整 (F1..F2] */
    CHECK(copy_file(bak_tb, p_tb) == 0, "roll back tb snapshot to F1");
    CHECK(copy_file(bak_jr, p_jr) == 0, "roll back journal (uncleaned)");

    c = nlsql_open(dir);
    CHECK(c != NULL, "reopen mid-crash state");
    if (c) {
        q_ok(c, "USE r4");
        int64_t nta = count_of(c, "ta"), ntb = count_of(c, "tb");
        printf("  [mid-crash] ta=%lld tb=%lld\n", (long long)nta, (long long)ntb);
        CHECK(nta == 550, "ta: new snapshot skips replay, no duplicates (550)");
        CHECK(ntb == 150, "tb: old snapshot replays gap exactly once (150)");
        check_consistent(c, "ta");
        check_consistent(c, "tb");
        /* 恢复后可继续写且 LSN 单调（不与旧记录冲突） */
        q_ok(c, "INSERT INTO tb VALUES (150, 'after')");
        CHECK(count_of(c, "tb") == 151, "write after mid-crash recovery");
        nlsql_flush(c);
        nlsql_close(c);

        c = nlsql_open(dir);
        CHECK(c != NULL, "reopen after post-recovery flush");
        if (c) {
            q_ok(c, "USE r4");
            CHECK(count_of(c, "ta") == 550 && count_of(c, "tb") == 151,
                  "post-recovery state persisted");
            nlsql_close(c);
        }
    }
    rm_dir(dir);
}

int main(int argc, char** argv) {
    /* Windows 子进程入口：<exe> child <mode> <dir> <marker> */
    if (argc >= 4 && strcmp(argv[1], "child") == 0) {
        _exit(child_run(atoi(argv[2]), argv[3], argc > 4 ? argv[4] : NULL));
    }
    printf("NLSQL crash-recovery / reliability tests\n");
    test_crash_exit();
    test_sigkill();
    test_truncated_journal();
    test_compact_mid_crash();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
