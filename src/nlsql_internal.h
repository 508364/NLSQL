/**
 * @file nlsql_internal.h
 * @brief NLSQL 内部数据结构（仅扩展内部使用）
 */
#ifndef NLSQL_INTERNAL_H
#define NLSQL_INTERNAL_H

#include "netleaf_nlsql.h"
#include "netleaf_nlsql_ext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 通用上限（防资源耗尽/栈溢出） ---------- */
#define NLSQL_MAX_IDENT       64
#define NLSQL_MAX_COLUMNS     256
#define NLSQL_MAX_SQL_LEN     (4u * 1024u * 1024u)   /* 单条 SQL 上限 4MB */
#define NLSQL_MAX_DEPTH       64      /* 表达式/条件嵌套上限 */
#define NLSQL_MAX_PARAMS      128
#define NLSQL_MAX_PACKET      (16u * 1024u * 1024u)  /* 协议单包上限 16MB */
#define NLSQL_DEFAULT_QUEUE   256     /* 异步队列深度 */

/* ---------- 动态字符串缓冲 ---------- */
typedef struct sbuf {
    char*    p;
    uint32_t len;
    uint32_t cap;
} sbuf_t;
int  sbuf_init(sbuf_t* b, uint32_t cap);
int  sbuf_append(sbuf_t* b, const void* data, uint32_t n);
int  sbuf_append_str(sbuf_t* b, const char* s);
int  sbuf_append_u8(sbuf_t* b, uint8_t v);
int  sbuf_append_u32(sbuf_t* b, uint32_t v);
int  sbuf_append_u64(sbuf_t* b, uint64_t v);
int  sbuf_printf(sbuf_t* b, const char* fmt, ...);
void sbuf_free(sbuf_t* b);

/* ---------- 值编码（存储/网络共用） ---------- */
/* 变长整数编码（MySQL lenenc 同款） */
uint32_t nl_lenenc(uint64_t v, uint8_t out[9]);
uint32_t nl_read_lenenc(const uint8_t* p, uint32_t avail, uint64_t* out);

/* ---------- 错误处理 ---------- */
void nlsql_set_err(nlsql_conn_t* c, int code, const char* sqlstate, const char* fmt, ...);
void nlsql_copy_err(nlsql_conn_t* c, int* code, char* msg, size_t msgsz,
                    char* state, size_t statesz);
void nlsql_set_global_err(const char* fmt, ...);

/* ---------- 事务语句类型 ---------- */
typedef enum stmt_kind {
    ST_NONE = 0, ST_SELECT, ST_INSERT, ST_UPDATE, ST_DELETE,
    ST_CREATE_DB, ST_DROP_DB, ST_USE, ST_CREATE_TABLE, ST_DROP_TABLE,
    ST_TRUNCATE, ST_SHOW_DB, ST_SHOW_TABLES, ST_SHOW_COLUMNS, ST_SELECT_CONST,
    ST_ALTER_TABLE, ST_SET,
    /* 客户端元数据浏览（SHOW 族，只读） */
    ST_SHOW_VARIABLES, ST_SHOW_STATUS, ST_SHOW_ENGINES, ST_SHOW_CHARSET,
    ST_SHOW_COLLATION, ST_SHOW_TABLE_STATUS, ST_SHOW_FULL_TABLES,
    ST_SHOW_FULL_COLUMNS, ST_SHOW_INDEX, ST_SHOW_CREATE_TABLE, ST_SHOW_WARNINGS,
    ST_SHOW_CREATE_DB,
    /* 扩展能力（需扩展打开相应特性） */
    ST_CREATE_INDEX, ST_DROP_INDEX
} stmt_kind_t;

typedef enum expr_kind {
    EX_NULL = 0, EX_INT, EX_DOUBLE, EX_STR, EX_PARAM, EX_COL, EX_NEG,
    EX_ADD, EX_SUB, EX_MUL, EX_DIV, EX_MOD, EX_AGG,
    EX_SYSVAR,  /* @@系统变量：s = 变量名 */
    EX_FUNC     /* 元数据函数：i = 函数编号（DATABASE/VERSION/USER/...） */
} expr_kind_t;

/* 元数据函数编号（EX_FUNC.i） */
enum {
    MF_DATABASE = 1, MF_VERSION, MF_USER, MF_CONNECTION_ID,
    MF_LAST_INSERT_ID, MF_ROW_COUNT
};

/* 聚合函数（EX_AGG 节点 i 存此值） */
typedef enum agg_kind {
    AGG_COUNT = 1, AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX
} agg_kind_t;

typedef enum cmp_op { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
                      OP_LIKE, OP_NOT_LIKE, OP_IS_NULL } cmp_op_t;

typedef struct expr expr_t;
struct expr {
    expr_kind_t kind;
    int64_t     i;
    double      d;
    char*       s;         /* EX_STR */
    uint32_t    slen;
    char*       col;       /* EX_COL（可能 table.col，含点） */
    int         col_idx;   /* EX_COL 列索引缓存，-1 = 未解析（扫描热路径避免重复按名查找） */
    uint32_t    col_epoch; /* 缓存对应的表结构世代（epoch 相同即免名字校验，行循环零 strcmp） */
    int         param_idx; /* EX_PARAM */
    struct expr* a, * b;   /* 二元/一元操作数 */
};

typedef struct cond cond_t;
struct cond {
    int         is_and;    /* 1=AND 0=OR；叶子时忽略 */
    int         is_not;
    cond_t*     a, * b;    /* 内部节点 */
    expr_t*     l, * r;    /* 叶子：比较 */
    cmp_op_t    op;
};

typedef struct order_by {
    char col[NLSQL_MAX_IDENT + 1];
    int  desc;
} order_by_t;

typedef struct select_stmt {
    char      table[NLSQL_MAX_IDENT + 1];
    char      (*cols)[NLSQL_MAX_IDENT + 1];  /* 请求列；NULL/cols[0][0]==0 表示 * */
    int       col_count;
    char      (*col_alias)[NLSQL_MAX_IDENT + 1]; /* 与 cols 平行的 AS 别名（可空串） */
    expr_t*   agg;                           /* 聚合输出项（EX_AGG）；NULL=无（够用版仅单聚合） */
    char      agg_alias[NLSQL_MAX_IDENT + 1];
    int       distinct;                      /* SELECT DISTINCT */
    char      group[NLSQL_MAX_IDENT + 1];    /* GROUP BY 单列 */
    int       has_group;
    cond_t*   having;                        /* HAVING（组级过滤） */
    cond_t*   where;
    order_by_t* order; int order_count;
    int64_t   limit, offset; int has_limit, has_offset;
} select_stmt_t;

typedef struct insert_stmt {
    char table[NLSQL_MAX_IDENT + 1];
    char (*cols)[NLSQL_MAX_IDENT + 1]; int col_count; /* 可为 0（省略列清单） */
    expr_t*** rows;                  /* 多行 VALUES */
    int*      row_col_counts;
    int       row_count;
} insert_stmt_t;

typedef struct update_stmt {
    char table[NLSQL_MAX_IDENT + 1];
    char (*cols)[NLSQL_MAX_IDENT + 1];
    expr_t** vals;
    int set_count;
    cond_t* where;
} update_stmt_t;

typedef struct delete_stmt {
    char table[NLSQL_MAX_IDENT + 1];
    cond_t* where;
} delete_stmt_t;

typedef struct column_def {
    char         name[NLSQL_MAX_IDENT + 1];
    nlsql_type_t type;
    uint32_t     vlen;     /* VARCHAR(n) */
    int          pk;       /* PRIMARY KEY */
    int          auto_inc; /* AUTO_INCREMENT（仅 INTEGER 主键） */
    int          not_null;
    nlsql_value_t def;     /* DEFAULT 字面量（has_def=0 时无效） */
    int          has_def;
} column_def_t;

typedef struct create_table_stmt {
    char        table[NLSQL_MAX_IDENT + 1];
    column_def_t* cols; int col_count;
    int         if_not_exists;   /* CREATE TABLE IF NOT EXISTS：已存在时返回 OK */
} create_table_stmt_t;

typedef enum alter_op { ALT_ADD_COL = 0, ALT_DROP_COL, ALT_MODIFY_COL,
                        ALT_RENAME_COL, ALT_RENAME_TABLE } alter_op_t;

typedef struct alter_stmt {
    char         table[NLSQL_MAX_IDENT + 1];
    alter_op_t   op;
    char         col[NLSQL_MAX_IDENT + 1];     /* ADD/DROP/MODIFY/RENAME 的源列 */
    char         newname[NLSQL_MAX_IDENT + 1]; /* RENAME 目标（列名/表名） */
    column_def_t def;                          /* ADD/MODIFY 的列定义 */
} alter_stmt_t;

typedef struct parsed_stmt {
    stmt_kind_t kind;
    char        ident[NLSQL_MAX_IDENT + 1];  /* db / table 名 */
    char        ident2[NLSQL_MAX_IDENT + 1]; /* SHOW COLUMNS/INDEX FROM <ident2> */
    char        ident3[NLSQL_MAX_IDENT + 1]; /* SHOW TABLES/STATUS FROM <ident3>（库名） */
    char        like_pat[NLSQL_MAX_IDENT + 1]; /* SHOW ... LIKE '<pat>' */
    int         has_like;
    int         show_global;                 /* SHOW GLOBAL ... */
    union {
        select_stmt_t       sel;
        insert_stmt_t       ins;
        update_stmt_t       upd;
        delete_stmt_t       del;
        create_table_stmt_t ct;
        alter_stmt_t        alt;
    } u;
    /* 常量 SELECT（无 FROM）：expr 列表 */
    expr_t**    consts; int const_count;
    char      (*const_alias)[NLSQL_MAX_IDENT + 1]; /* 与 consts 平行的 AS 别名 */
    int         param_count;   /* 占位符总数 */
    int         readonly_stmt; /* 1 = 只读语句 */
    int         if_exists;     /* DROP ... IF EXISTS：对象不存在时返回 OK */
    /* CREATE/DROP INDEX */
    char        idx_cols[8][NLSQL_MAX_IDENT + 1];
    int         idx_ncol;
    int         idx_unique;
} parsed_stmt_t;

/* ---------- 解析器 ---------- */
int nlsql_parse(const char* sql, parsed_stmt_t** out, char* errbuf, uint32_t errsz);
void nlsql_parsed_free(parsed_stmt_t* p);

/* ---------- 存储层 ---------- */
typedef struct tbl_col {
    char         name[NLSQL_MAX_IDENT + 1];
    nlsql_type_t type;
    uint32_t     vlen;
    int          pk, auto_inc, not_null;
    nlsql_value_t def;     /* DEFAULT（has_def=0 时无效，s 需释放） */
    int          has_def;
} tbl_col_t;

typedef struct row {
    nlsql_value_t* vals;
} row_t;

/* 二级索引（多种索引）：普通/唯一/复合共用同一结构。
 * 条目按 (列值...) 排序存储在 order[] 中，查找用二分，支持重复键与范围。
 * dirty=1 时下次查找/维护前从 rows 重建（写路径只置脏，不逐行维护）。 */
typedef struct tbl_index {
    char        name[NLSQL_MAX_IDENT + 1];
    int         ncols;
    int         cols[8];
    int         unique;
    uint64_t*   order;      /* 行号数组，按索引键升序 */
    uint64_t    n;
    int         dirty;
    struct tbl_index* next;
} tbl_index_t;

typedef struct table {
    char        name[NLSQL_MAX_IDENT + 1];
    tbl_col_t*  cols; int col_count;
    row_t*      rows; uint64_t row_count, row_cap;
    void*       val_arena;  /* 行值 arena 块链（bump 分配，表释放/TRUNCATE 时整块回收） */
    nlsql_value_t* vals_free; /* 已删/未提交行的 vals 空闲链（复用，维持分配稳态） */
    int64_t     next_auto;
    uint32_t    epoch;      /* 表结构世代：ALTER（列增删改名）时 +1，用于列索引缓存失效 */
    /* 增量快照（append-only 帧）：快照文件已含的行数 / 结构世代 / 已追加帧数 /
       是否发生过行级修改（UPDATE/DELETE/TRUNCATE）——满足条件时压实只追加新行，不重写旧行 */
    uint64_t    snap_rows;
    uint32_t    snap_epoch;
    uint32_t    snap_frames;
    int         snap_mutated;
    int         dirty;      /* 有未快照修改 */
    uint64_t    snap_lsn;   /* 最近一次快照所含的最后 journal LSN（重放时跳过 ≤ 此值的记录） */
    /* 主键哈希索引（仅单列主键表启用；通用 CPU 指令开放寻址）
     * pk_slot: 0=空 1=墓碑 否则 = row_idx+2（避开 0/1 特殊值） */
    uint64_t*   pk_slots;
    uint64_t    pk_cap;     /* 2 的幂 */
    uint64_t    pk_used;    /* 含墓碑的占用数 */
    int         pk_col;     /* 索引的主键列，-1=未建/不支持（多列主键走线性） */
    int         pk_dirty;   /* 结构变化后待重建 */
    tbl_index_t* indexes;   /* 二级索引链表（扩展特性启用后创建） */
    int         heap_rows;  /* 1 = 行 vals 数组逐行 malloc（事务快照表），释放方式不同 */
    struct table* next;
} table_t;

typedef struct database {
    char name[NLSQL_MAX_IDENT + 1];
    table_t* tables;   /* 单链表（表数通常少） */
    struct database* next;
} database_t;

typedef struct journal journal_t;

/* 执行引擎上下文（挂在连接上） */
typedef struct engine {
    char       root[1024];   /* 数据目录绝对路径 */
    database_t* dbs;         /* 已加载库链表头（只增不挪，用于查找/释放） */
    database_t* cur_db;      /* USE 选中的库（可为链表任意节点） */
    journal_t* jr;
    uint32_t   journal_limit;
    int        readonly;
    /* journal 用户态写缓冲：合并 write 系统调用（每语句/每64KB/收尾落盘一次） */
    uint8_t*   jbuf;
    uint32_t   jlen;
    uint32_t   jcap;
    /* 递增日志序号：每条 journal 记录分配一个；快照记录其含有的最大 LSN。
       重放按 (表, LSN) 对齐快照与 WAL，压实中途崩溃也不会重复/丢失 */
    uint64_t   lsn;
    /* 当前库快照文件总字节（每次压实后刷新）：journal 阈值据此自适应放大，
       大库（GB 级）不再被固定 64MB 阈值逼出频繁全量重写 */
    uint64_t   snap_bytes;
    /* 文件锁 */
    char lock_path[1100];
    int   lock_fd;
    int64_t last_insert_id; /* LAST_INSERT_ID() 最近一次 AUTO_INCREMENT 值（0=无） */
    /* 事务快照（BEGIN 时创建，COMMIT 释放，ROLLBACK 恢复） */
    void*   txn_snap;
    int     txn_active;
} engine_t;

int  eng_open(engine_t* e, const char* data_dir, uint32_t journal_mb, int readonly);
void eng_close(engine_t* e);
int  eng_execute(nlsql_conn_t* c, engine_t* e, parsed_stmt_t* st,
                 nlsql_value_t* params, int nparams, nlsql_result_t** out);
int  eng_flush(engine_t* e);

/* 结果集行值 arena（nlsql_engine.c 定义，nlsql.c 释放时调用） */
void nlsql_val_arena_free(nlsql_result_t* res);

/* 存储层辅助（供引擎调用，见 nlsql_storage.c） */
int      nlsql_db_load(engine_t* e, const char* dbname);   /* NLSQL_OK / NLSQL_ER_NO_SUCH_DB */
int      nlsql_db_create(engine_t* e, const char* dbname);
int      nlsql_db_drop(engine_t* e, const char* dbname);
int      nlsql_jr_append(engine_t* e, uint8_t type, const void* payload, uint32_t plen);
int      nlsql_jr_flush(engine_t* e);   /* 把 journal 写缓冲落盘（切库/压实/语句尾/关闭前必须调用） */
int      nlsql_jr_sync(engine_t* e);    /* 写缓冲落盘后再 fsync（自动落盘的持久化边界） */
void     nlsql_jr_put_str(sbuf_t* b, const char* s);
int      nlsql_sbuf_enc_value(sbuf_t* b, const nlsql_value_t* v);
uint64_t nlsql_journal_size(engine_t* e);
uint64_t nlsql_journal_limit(engine_t* e);
table_t* nlsql_tbl_find(engine_t* e, const char* name);
int      nlsql_row_alloc(table_t* t, row_t** out);
nlsql_value_t* nlsql_tbl_val_alloc(table_t* t, int nvals); /* 行值 arena 分配（行 vals 数组） */
void     nlsql_tbl_val_arena_free(table_t* t);             /* 行值 arena 整块释放 */
void     nlsql_tbl_val_recycle(table_t* t, nlsql_value_t* v); /* 归还 vals 到空闲链复用 */
void     nlsql_tbl_mark_dirty(table_t* t);
void     nlsql_tbl_mark_mutated(table_t* t); /* 既有行被改/删 → 下次压实必须整表重写 */
void     nlsql_tbl_commit_row(table_t* t);
void     nlsql_tbl_set_auto(table_t* t, int64_t v);
int64_t  nlsql_tbl_get_auto(table_t* t);
/* 主键哈希索引（单列主键 O(1) 查重；通用 CPU 指令，开放寻址+墓碑） */
int      nlsql_pk_ensure(table_t* t);   /* NLSQL_OK=索引可用；其他=不可用（调用方回退线性） */
int      nlsql_pk_lookup(table_t* t, const nlsql_value_t* v, uint64_t* row_out); /* 1=命中 */
void     nlsql_pk_add(table_t* t, uint64_t row_idx, const nlsql_value_t* v);
void     nlsql_pk_remove(table_t* t, uint64_t row_idx, const nlsql_value_t* v);
void     nlsql_pk_invalidate(table_t* t); /* 行结构变化（DELETE 移位/TRUNCATE）后标脏 */
void     nlsql_pk_free(table_t* t);
/* 二级索引（见 nlsql_storage.c） */
tbl_index_t* nlsql_idx_find(table_t* t, const char* name);
int      nlsql_idx_create(table_t* t, const char* name, const int* cols, int ncols, int unique, char* err, uint32_t errsz);
int      nlsql_idx_drop(table_t* t, const char* name);
void     nlsql_idx_mark_dirty(table_t* t);
void     nlsql_idx_free_all(table_t* t);
/* 等值查找：返回 malloc 的行号数组（调用方 free），*cnt 输出条数 */
int      nlsql_idx_lookup(table_t* t, tbl_index_t* ix, const nlsql_value_t* keys,
                          uint64_t** rows_out, uint64_t* cnt_out);
/* 事务快照（见 nlsql_storage.c） */
int      nlsql_txn_begin(engine_t* e);
int      nlsql_txn_commit(engine_t* e);
int      nlsql_txn_rollback(engine_t* e);
int      nlsql_sanitize_dir(const char* in, char* out, uint32_t outsz);
int      nlsql_mkdir_p(const char* path);
int      nlsql_lock_dir(const char* root, char* lock_path, uint32_t lpsz); /* -2=被占用 */
void     nlsql_unlock_dir(int fd, const char* lock_path);

/* WAL 条目类型 */
enum { JR_CREATE_DB = 1, JR_DROP_DB, JR_CREATE_TBL, JR_DROP_TBL,
       JR_INSERT, JR_UPDATE, JR_DELETE, JR_TRUNCATE, JR_AUTOINC };

/* ---------- SHA1（mysql_native_password） ---------- */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t blen; } nlsha1_t;
void nlsha1_init(nlsha1_t* s);
void nlsha1_update(nlsha1_t* s, const void* data, uint32_t n);
void nlsha1_final(nlsha1_t* s, uint8_t out[20]);
void nlsha1(const void* data, uint32_t n, uint8_t out[20]);

/* ---------- MySQL 协议 ---------- */
typedef struct wire_srv wire_srv_t;
wire_srv_t* wire_start(nlsql_conn_t* conn, int port, int max_clients, char* err, uint32_t errsz);
/* cert_file/key_file 非空即启用 TLS（NetLeaf TLS2 扩展，强制 TLS 1.2） */
wire_srv_t* wire_start_tls(nlsql_conn_t* conn, int port, int max_clients,
                           const char* cert_file, const char* key_file,
                           char* err, uint32_t errsz);
void wire_stop(wire_srv_t* w);

/* ---------- 内部：连接结构 ---------- */
#include <pthread.h>

typedef struct async_task async_task_t;
typedef struct async_queue async_queue_t;

/**
 * 公平三模式读写锁：
 *   READ  —— 只读语句（SELECT/SHOW/SET），可并发共享
 *   EXCL  —— 写语句/DDL/USE/凭据变更，独占
 *   SNAP  —— 压实（写快照文件 + truncate journal）：挡写、放读
 * 队列按到达顺序取票。写者/压实只在自己是队首且无冲突持有者时获得；
 * 读者只要前面没有等待中的写者即可进入 → 写与读双向有界等待，不会互相饿死。
 */
typedef enum frw_mode { FRW_READ = 0, FRW_EXCL = 1, FRW_SNAP = 2 } frw_mode_t;

typedef struct frw_waiter {
    struct frw_waiter* next;
    struct frw_waiter* prev;
    uint64_t           ticket;
    frw_mode_t         mode;
} frw_waiter_t;

typedef struct frw_lock {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    uint64_t        readers;    /* 活跃 READ 持有者数 */
    int             excl_holder;/* 1=EXCL 持有中（挡所有读者与新写者） */
    int             snap_holder;/* 1=SNAP 持有中（挡写者，放读者） */
    uint64_t        next_ticket;
    uint32_t        writer_bypass; /* 写者等待期间已放行越过的读者数（限流，防写者被饿死） */
    frw_waiter_t*   qhead;
    frw_waiter_t*   qtail;
} frw_lock_t;

#define FRW_BYPASS_MAX 16  /* 写者等待时最多再放行多少读者越过它 */

void frw_init(frw_lock_t* l);
void frw_destroy(frw_lock_t* l);
void frw_acquire(frw_lock_t* l, frw_mode_t m);
void frw_release(frw_lock_t* l, frw_mode_t m);

/* 核心执行入口（扩展钩子 → 内置解析/执行），供 wire 复用 */
int nlsql_exec_sql(nlsql_conn_t* c, const char* sql,
                   nlsql_value_t* params, int nparams, nlsql_result_t** out);

/* 扩展钩子内部实现（nlsql_ext.c）：预改写与语句处理器派发 */
char* nlsql_ext_rewrite(const char* sql);
int   nlsql_ext_dispatch(nlsql_conn_t* c, const char* sql, nlsql_result_t** out);

/* 该语句能否在共享读锁下执行（USE 改当前库状态，必须独占） */
static inline int nlsql_stmt_shared_read(const parsed_stmt_t* st) {
    return st->readonly_stmt && st->kind != ST_USE;
}

/* 执行后若 journal 超阈值则自行取 SNAP 锁压实（调用方不得持锁） */
void nlsql_maybe_flush(nlsql_conn_t* c);
/* 空闲预压实：journal 超过 limit/2 才做，同样自行取 SNAP 锁 */
void nlsql_idle_flush(nlsql_conn_t* c);

struct nlsql_conn {
    frw_lock_t      frw;       /* 引擎数据（eng）的公平读写锁 */
    pthread_mutex_t err_mtx;   /* 错误槽互斥：并发读者写 err_msg 不撕裂 */
    engine_t        eng;
    async_queue_t*  aq;
    /* 异步压实线程：写路径只投递请求立即返回，压实在后台以 SNAP 锁推进，
       提交线程不再被 GB 级快照重编码阻塞；close/flush 会同步等待其完成 */
    pthread_t       flush_th;
    pthread_mutex_t flush_mtx;
    pthread_cond_t  flush_cv;
    int             flush_req;      /* 有待处理的压实请求 */
    int             flush_force;    /* 1=强制全量压实（显式 flush） */
    int             flush_running;  /* 后台线程正在压实 */
    int             flush_quit;
    int             flush_rc;       /* 最近一次压实返回码 */
    int             flush_th_started;

    /* 自动落盘：间隔内无新写入则 fsync WAL；连续一个间隔仍空闲则把脏表压实为快照 */
    int             auto_flush_ms;  /* 0 = 关闭（只读连接或显式禁用） */
    uint64_t        af_last_size;   /* 上次自动落盘时观察到的 journal 字节数 */
    uint64_t        jr_synced;      /* 最近一次 fsync 覆盖到的 journal 字节数 */

    /* 错误状态 */
    int  err_code;
    char err_msg[512];
    char err_sqlstate[6];

    /* 凭据门控 */
    int  have_creds;
    char creds_user[65];
    uint8_t creds_sha[20];   /* SHA1(SHA1(pass)) 存储，不落明文 */

    int  authenticated;
    int  readonly;

    uint64_t id;                /* 进程内唯一连接 ID（进程列表/SHOW PROCESSLIST 用） */

    parsed_stmt_t* last_parsed; /* 供 stmt 复用 */
};

/* 预编译语句（公共 nlsql_stmt_t 的内部定义） */
typedef struct nlsql_stmt {
    nlsql_conn_t*  conn;
    char*          sql;            /* 复制的 SQL 文本 */
    int            param_count;
    nlsql_value_t  params[NLSQL_MAX_PARAMS];
    uint8_t        bound[NLSQL_MAX_PARAMS];
} nlsql_stmt_t;

/* 异步队列 */
typedef void (*async_fn)(nlsql_conn_t* c, void* arg);
struct async_task {
    async_task_t* next;
    async_fn      fn;
    void*         arg;
    void        (*drop)(void* arg); /* 队列被 drain 未执行时释放 arg（fn 不会被调用） */
};
struct async_queue {
    pthread_mutex_t mtx;
    pthread_cond_t  cv, done_cv;
    async_task_t*   head, * tail;
    pthread_t*      threads;
    int             nthreads, running;
    nlsql_conn_t*   owner;
    int             inflight;
    int             pending;
};

int aq_start(async_queue_t* q, nlsql_conn_t* owner, int nthreads);
int aq_push(async_queue_t* q, async_fn fn, void* arg, void (*drop)(void*));
void aq_drain(async_queue_t* q);   /* 停止并回收线程 */
void aq_wait_idle(async_queue_t* q);
int  aq_pending(async_queue_t* q);   /* 排队未执行任务数（锁内读） */

/* ---------- 连接注册表（进程列表类信息用，见 nlsql_ext.c） ---------- */
void nlsql_reg_add(nlsql_conn_t* c);
void nlsql_reg_del(nlsql_conn_t* c);

/* ---------- 扩展元数据 sidecar 路径（见 nlsql_ext_meta.c） ---------- */
int  nlsql_meta_path(nlsql_conn_t* c, char* out, size_t sz);

#endif /* NLSQL_INTERNAL_H */
