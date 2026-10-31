/**
 * @file netleaf_nlsql.h
 * @brief NLSQL - NetLeaf 嵌入式 MySQL 兼容数据库扩展（公共 API）
 * @version 1.2.0
 *
 * NLSQL 是 NetLeaf 的 NL 扩展：基于 MySQL 语义裁剪的**完全本地、完全离线**
 * 嵌入式 SQL 引擎。
 *
 * 设计目标：
 *  - MySQL 兼容：SQL 方言子集 + MySQL 风格错误码/sqlstate + 可选 MySQL 传输协议
 *  - 完全离线：核心为进程内库，无网络、无服务端、无外部依赖
 *  - 异步架构：内置工作线程池，回调式异步查询
 *  - 低占用/小体积/快：纯 C11，无第三方库，WAL 追加写入
 *  - 默认不开放服务端；如需被官方 mysql 客户端直接访问，
 *    可显式调用 nlsql_wire_start()（仅绑定 127.0.0.1）
 *
 * @note 线程模型：每个 nlsql_conn_t 内部是公平三模式读写锁（读语句共享 READ、
 *       写语句独占 EXCL、压实挡写放读 SNAP）；同一连接可多线程并发只读，写操作串行化。
 *       同一数据目录同一时刻仅允许一个写连接（文件锁保护），只读连接可多开。
 */

#ifndef NETLEAF_NLSQL_H
#define NETLEAF_NLSQL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================
 * 版本
 * ========================================= */

#define NLSQL_VERSION            "1.2.0"
#define NLSQL_SERVER_VERSION     "8.0.36-NLSQL"   /* 对 mysql 客户端上报的版本串 */
#define NLSQL_PROTOCOL_VERSION   10                /* MySQL 客户端/服务端协议版本 */

/* =========================================
 * 返回码（MySQL 风格错误码子集 + NLSQL 私有段）
 * ========================================= */

#define NLSQL_OK                 0
#define NLSQL_ERR               (-1)     /* 通用失败，细节见 nlsql_error() */

/* MySQL 兼容错误码（与 ER_* 对应，仅取常用子集） */
#define NLSQL_ER_DB_CREATE_EXISTS     1007
#define NLSQL_ER_DB_DROP_EXISTS       1008
#define NLSQL_ER_DBACCESS_DENIED      1044
#define NLSQL_ER_ACCESS_DENIED        1045
#define NLSQL_ER_NO_SUCH_TABLE        1146
#define NLSQL_ER_NO_SUCH_DB           1049
#define NLSQL_ER_SYNTAX               1064   /* You have an error in your SQL syntax ... */
#define NLSQL_ER_TABLE_EXISTS         1050
#define NLSQL_ER_DUP_ENTRY            1062
#define NLSQL_ER_UNKNOWN_COLUMN       1054
#define NLSQL_ER_BAD_FIELD_ERROR      1054
#define NLSQL_ER_WRONG_VALUE_COUNT    1136
#define NLSQL_ER_BAD_NULL_ERROR       1048
#define NLSQL_ER_UNKNOWN_STMT         1243
#define NLSQL_ER_NOT_SUPPORTED        1235   /* this version of NLSQL doesn't yet support ... */

/* NLSQL 私有错误段（-30000 起，不与 MySQL 冲突） */
#define NLSQL_ERR_LOCKED         (-30001)  /* 数据目录被其他进程持有 */
#define NLSQL_ERR_IO             (-30002)  /* 文件读写失败 */
#define NLSQL_ERR_PATH           (-30003)  /* 非法路径/标识符（含路径穿越） */
#define NLSQL_ERR_BUSY           (-30004)  /* 异步队列满 / 正在关闭 */
#define NLSQL_ERR_PROTOCOL       (-30005)  /* MySQL 协议层错误 */

/* =========================================
 * 数据类型（对齐 MySQL 协议类型编号的常用子集）
 * ========================================= */

typedef enum nlsql_type {
    NLSQL_TYPE_NULL     = 0,
    NLSQL_TYPE_INT64    = 3,   /* MySQL MYSQL_TYPE_LONG 族 → 64 位整数 */
    NLSQL_TYPE_DOUBLE   = 5,
    NLSQL_TYPE_VARCHAR  = 15,
    NLSQL_TYPE_TEXT     = 252,
    NLSQL_TYPE_DATETIME = 12   /* 以 'YYYY-MM-DD HH:MM:SS' 文本存储 */
} nlsql_type_t;

/* =========================================
 * 值 / 结果集
 * ========================================= */

typedef struct nlsql_value {
    nlsql_type_t type;
    int          is_null;
    union {                        /* i/d 同为定宽数值、由 type 决定互斥：
                                      40B → 32B；s/len 保持独立字段（盲 free 安全） */
        int64_t i;                 /* INT64 */
        double  d;                 /* DOUBLE */
    };
    char*        s;                /* VARCHAR/TEXT/DATETIME（调用方只读，随结果集释放） */
    uint32_t     len;              /* s 的字节长度 */
} nlsql_value_t;

typedef struct nlsql_column {
    char         name[65];   /* MySQL 标识符上限 64 + NUL */
    nlsql_type_t type;
} nlsql_column_t;

typedef struct nlsql_row {
    nlsql_value_t* vals;     /* 与列数对齐 */
} nlsql_row_t;

typedef struct nlsql_result {
    nlsql_column_t* cols;
    int             col_count;
    nlsql_row_t*    rows;
    uint64_t        row_count;
    uint64_t         row_cap;         /* rows 预分配容量（内部使用，倍增增长） */
    void*           _arena;          /* 行值 arena 块链（内部使用，整块释放） */
    uint64_t        affected;     /* INSERT/UPDATE/DELETE 影响行数 */
    int64_t         insert_id;    /* AUTO_INCREMENT 生成的最后一个 ID */
} nlsql_result_t;

/* =========================================
 * 连接（嵌入式会话）
 * ========================================= */

typedef struct nlsql_conn nlsql_conn_t;

typedef struct nlsql_open_opts {
    const char* data_dir;      /* 数据目录（相对/绝对路径均可，内部做净化） */
    const char* user;          /* 本地访问口令门控，可 NULL */
    const char* password;      /* 可 NULL */
    const char* auth_file;     /* 凭据文件路径，可 NULL；格式见 nlsql_load_auth_file() */
    int         require_auth;  /* 1 = 强制认证：未配置凭据时也拒绝查询/连接 */
    int         readonly;      /* 1 = 只读模式（拒绝写语句与 DDL） */
    int         workers;       /* 异步工作线程数，0 = 默认（1，低占用） */
    int         journal_mb;    /* WAL 压实阈值（MB），0 = 默认 64 */
    int         auto_flush_ms; /* 自动落盘间隔（ms），0 = 默认 5000；<0 = 关闭 */
} nlsql_open_opts_t;

/**
 * 打开（或创建）一个嵌入式数据库会话。
 * @return 连接句柄；失败返回 NULL（errno 无效时用 nlsql_global_error() 取原因）
 */
nlsql_conn_t* nlsql_open(const char* data_dir);
nlsql_conn_t* nlsql_open_ex(const nlsql_open_opts_t* opts);

/** 关闭会话：等待异步队列清空、等待进行中的后台压实结束、fsync WAL、释放。
 *  不做全量压实（WAL 已持久化，未压实部分下次打开按 LSN 重放）；需要全量快照请先调 nlsql_flush()。 */
void nlsql_close(nlsql_conn_t* conn);

/** 最近一次错误的 MySQL 风格描述（缓冲区随连接生存期）。 */
const char* nlsql_error(nlsql_conn_t* conn);
/** 最近一次错误码。 */
int nlsql_errno(nlsql_conn_t* conn);
/** 最近一次错误的 SQLSTATE（5 字符）。 */
const char* nlsql_sqlstate(nlsql_conn_t* conn);
/** 打开失败等无连接场景下的全局错误描述。 */
const char* nlsql_global_error(void);

/**
 * 设置会话凭据（门控用途）。
 * 设置后，后续 nlsql_query 前必须已通过 nlsql_authenticate()。
 */
int nlsql_set_credentials(nlsql_conn_t* conn, const char* user, const char* password);
/** 按已设置凭据校验（user/password 不匹配返回 NLSQL_ER_ACCESS_DENIED）。 */
int nlsql_authenticate(nlsql_conn_t* conn, const char* user, const char* password);

/**
 * 从文件加载凭据（覆盖已设置的凭据）。便于把口令从代码/命令行参数移出。
 * 文件为纯文本，每行一条，支持 `key=value` 与 `user:password` 单行形式：
 *
 *     # 注释行
 *     user=app
 *     password=secret123
 *
 * 未知键忽略；缺少 `user` 返回错误。空密码（`password=`）合法。
 * @return NLSQL_OK / NLSQL_ERR_IO（文件不可读）/ NLSQL_ERR（内容缺 user）
 */
int nlsql_load_auth_file(nlsql_conn_t* conn, const char* path);

/** 强制将 WAL 压实为快照并落盘（同步等待后台压实完成；close 不再自动做）。 */
int nlsql_flush(nlsql_conn_t* conn);

/* =========================================
 * 同步查询
 * ========================================= */

/**
 * 执行一条 SQL（MySQL 方言子集）。
 * @param out 成功且产生结果集/影响行时输出，调用方用 nlsql_free_result 释放；
 *            可传 NULL（不关心结果）。SELECT 1 这类常量查询也会产出结果集。
 * @return NLSQL_OK 或错误码
 */
int nlsql_query(nlsql_conn_t* conn, const char* sql, nlsql_result_t** out);

/** 释放结果集。 */
void nlsql_free_result(nlsql_result_t* res);

/* =========================================
 * 预编译语句（参数绑定，防注入）
 * ========================================= */

typedef struct nlsql_stmt nlsql_stmt_t;

/** 预编译：解析并校验 '?' 占位符。 */
int nlsql_prepare(nlsql_conn_t* conn, const char* sql, nlsql_stmt_t** out);
/** 绑定参数：idx 从 0 开始；字符串参数会被复制。 */
int nlsql_stmt_bind_null(nlsql_stmt_t* stmt, int idx);
int nlsql_stmt_bind_int(nlsql_stmt_t* stmt, int idx, int64_t v);
int nlsql_stmt_bind_double(nlsql_stmt_t* stmt, int idx, double v);
int nlsql_stmt_bind_text(nlsql_stmt_t* stmt, int idx, const char* s, uint32_t len);
/** 执行（可重复执行；参数保持上次绑定值）。 */
int nlsql_stmt_execute(nlsql_stmt_t* stmt, nlsql_result_t** out);
/** 参数个数。 */
int nlsql_stmt_param_count(nlsql_stmt_t* stmt);
void nlsql_stmt_free(nlsql_stmt_t* stmt);

/* =========================================
 * 异步架构（内置工作线程池）
 * ========================================= */

typedef void (*nlsql_result_cb)(nlsql_conn_t* conn,
                                int err,               /* NLSQL_OK 或错误码 */
                                nlsql_result_t* res,   /* 成功时非 NULL，回调后由库释放 */
                                void* userdata);

/**
 * 异步执行 SQL：任务入队后立即返回，工作线程执行完毕后回调。
 * @note 回调在库的工作线程上触发，请勿在回调内再同步调用 nlsql_query 等阻塞接口
 *       （会死锁，工作线程数为 1 时尤其如此）。
 * @return NLSQL_OK（已入队）/ NLSQL_ERR_BUSY（队列满或正在关闭）
 */
int nlsql_query_async(nlsql_conn_t* conn, const char* sql,
                      nlsql_result_cb cb, void* userdata);

/** 异步预编译执行（复用同一 stmt，串行派发）。 */
int nlsql_stmt_execute_async(nlsql_stmt_t* stmt, nlsql_result_cb cb, void* userdata);

/**
 * 写通道提交（fire-and-forget）：语句立即入队返回，调用者零阻塞，
 * 由专职工作线程串行批量消化（FIFO，与同步语句同序）。
 * 结果与错误丢弃——需要结果的语句请用 nlsql_query / nlsql_query_async。
 * @return NLSQL_OK / NLSQL_ERR_BUSY（队列满 256，背压，可稍后重试）
 */
int nlsql_submit(nlsql_conn_t* conn, const char* sql);

/** 等待该连接所有异步任务完成（可选，close 时自动执行）。 */
void nlsql_async_wait(nlsql_conn_t* conn);

/* =========================================
 * MySQL 传输协议适配层（默认关闭）
 * ========================================= */

/**
 * 开启 MySQL 协议监听：官方 mysql 客户端 / 各语言 libmysql 驱动可直接连接。
 *
 * 安全约束：
 *  - 仅绑定 127.0.0.1（环回），绝不对外网暴露
 *  - 需先 nlsql_set_credentials()，客户端凭据不匹配即拒连
 *    （未设置时默认 root/空密码，仅供本地调试）
 *  - mysql_native_password 质询认证（SHA1），口令不落明文日志
 *
 * @return 监听句柄（>0），失败返回负数
 */
int nlsql_wire_start(nlsql_conn_t* conn, int port, int max_clients);

/**
 * 开启 MySQL 协议监听并启用 TLS（客户端以 CLIENT_SSL 协商后全程加密）。
 *
 * TLS 由 NetLeaf TLS 扩展（netleaf_tls2 / netleaf_tls3）承载，运行时按 dlopen 加载
 * （核心库仍零第三方依赖）：
 *  - 查找顺序：环境变量 NLSQL_TLS_LIB 指定路径 → libnetleaf_tls2.so(.1) → DLL
 *              → libnetleaf_tls3.so(.2) → DLL
 *  - 协议版本强制 TLS 1.2（min = max = TLS1.2），协商结果非 TLSv1.2 即断开
 *  - 扩展缺失或证书加载失败时直接返回错误，绝不静默退化为明文
 *
 * @param cert_file 服务端证书（PEM）
 * @param key_file  服务端私钥（PEM）
 * @return 监听句柄（>0），失败返回负数；错误文本可用 nlsql_global_error() 读取
 */
int nlsql_wire_start_tls(nlsql_conn_t* conn, int port, int max_clients,
                         const char* cert_file, const char* key_file);

void nlsql_wire_stop(int handle);

/* =========================================
 * 宿主直连 API（netleaf 通过 dlsym/get_function 获取符号即可操作数据库）
 * ========================================= */

/**
 * 打开（或创建）宿主默认会话并持有，进程内单例。
 * @param data_dir 数据目录
 * @param user/password 可 NULL（不设门控）
 * @return NLSQL_OK / NLSQL_ERR_LOCKED / NLSQL_ERR_IO ...
 */
int nlsql_host_open(const char* data_dir, const char* user, const char* password);
/** 关闭宿主默认会话（未打开时为空操作）。 */
void nlsql_host_close(void);
/** 取宿主默认会话（未打开返回 NULL）。 */
nlsql_conn_t* nlsql_host_conn(void);

/** 在默认会话上执行任意 SQL（建库/建表/增删改查均可）。 */
int nlsql_host_exec(const char* sql, nlsql_result_t** out);
/** 创建数据库（存在则返回 NLSQL_ER_DB_EXISTS）。 */
int nlsql_host_create_db(const char* name);
/** 删除数据库。 */
int nlsql_host_drop_db(const char* name);
/** 切换当前数据库。 */
int nlsql_host_use(const char* name);
/** 落盘（WAL 压实）。 */
int nlsql_host_flush(void);

/* =========================================
 * 工具
 * ========================================= */

/** 库版本串。 */
const char* nlsql_version(void);
/** 校验标识符（[A-Za-z0-9_$]，首字符非数字，<=64 字节）：合法返回 1。 */
int nlsql_valid_ident(const char* s);
/** MySQL 风格字符串转义（单引号/反斜杠/NUL）：dst 需 >= 2*len+1。 */
uint32_t nlsql_escape_string(char* dst, const char* src, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* NETLEAF_NLSQL_H */
