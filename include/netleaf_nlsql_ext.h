/**
 * @file netleaf_nlsql_ext.h
 * @brief NLSQL 扩展（NetLeaf 插件）可用的核心钩子
 *
 * 扩展以 NetLeaf NL 扩展（共享库）形式加载，链接 libnetleaf_nlsql.so，
 * 通过本头文件向核心注册：
 *   - SQL 语句处理器：在核心解析之前接管高级语句（JOIN/子查询/视图/触发器/事务）
 *   - SQL 预改写器：把 NOW()/CURDATE() 等函数展开为字面量
 *   - 特性开关：打开由扩展激活的核心能力（如服务端预处理、二级索引）
 *   - 事务快照：BEGIN/COMMIT/ROLLBACK
 *   - 二级索引：CREATE/DROP INDEX 的执行入口
 *
 * 未加载任何扩展时，核心行为与 1.2.0 完全一致。
 */
#ifndef NETLEAF_NLSQL_EXT_H
#define NETLEAF_NLSQL_EXT_H

#include "netleaf_nlsql.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 语句处理器返回值：本条 SQL 不由我处理（交还核心/下一个处理器）。 */
#define NLSQL_EXT_UNHANDLED 1

/** 由扩展填写连接错误槽（错误码 + SQLSTATE + 描述），处理器返回错误码前调用。 */
void nlsql_ext_set_error(nlsql_conn_t* conn, int code, const char* sqlstate, const char* msg);

/**
 * 语句处理器：返回 NLSQL_OK 表示已处理并产出结果；返回 NLSQL_EXT_UNHANDLED
 * 表示不接管，由核心继续解析执行。
 * @note 处理器内可以再次调用 nlsql_query() 执行子语句；核心有递归深度保护。
 */
typedef int (*nlsql_ext_stmt_fn)(nlsql_conn_t* conn, const char* sql,
                                 nlsql_result_t** out, void* ud);

/** SQL 预改写器：返回 malloc 的新 SQL（调用方负责 free），NULL 表示不改写。 */
typedef char* (*nlsql_ext_rewrite_fn)(const char* sql, void* ud);

int nlsql_ext_register_stmt_handler(nlsql_ext_stmt_fn fn, void* ud);
int nlsql_ext_unregister_stmt_handler(nlsql_ext_stmt_fn fn);
int nlsql_ext_register_rewriter(nlsql_ext_rewrite_fn fn, void* ud);
int nlsql_ext_unregister_rewriter(nlsql_ext_rewrite_fn fn);

/** 打开/关闭具名特性；未打开时对应能力返回 1235（与旧版一致）。 */
int nlsql_ext_set_feature(const char* name, int enabled);
int nlsql_ext_feature(const char* name);
int nlsql_ext_feature_enabled(const char* name);

/** 当前语句处理器递归深度（扩展可据此判断是否处于子语句执行中）。 */
int nlsql_ext_dispatch_depth(void);

/** 事务：BEGIN 时对当前库做内存快照；ROLLBACK 恢复内存并重写快照/清空 WAL。 */
int nlsql_ext_txn_begin(nlsql_conn_t* conn);
int nlsql_ext_txn_commit(nlsql_conn_t* conn);
int nlsql_ext_txn_rollback(nlsql_conn_t* conn);
int nlsql_ext_txn_active(nlsql_conn_t* conn);

/** 二级索引 DDL（等价于 SQL: CREATE [UNIQUE] INDEX / DROP INDEX）。 */
int nlsql_ext_create_index(nlsql_conn_t* conn, const char* table, const char* index_name,
                           const char* const* cols, int ncols, int unique);
int nlsql_ext_drop_index(nlsql_conn_t* conn, const char* table, const char* index_name);

/* =========================================
 * 会话信息（只读，供 information_schema / 进程列表类扩展使用）
 * ========================================= */

/** 当前库名（未 USE 时为空串）；返回内部指针，随连接生命周期有效。 */
const char* nlsql_ext_conn_db(nlsql_conn_t* conn);
/** 数据目录（打开时传入的路径）。 */
const char* nlsql_ext_conn_data_dir(nlsql_conn_t* conn);
/** 会话用户名（未设置凭据时为 "root"）。 */
const char* nlsql_ext_conn_user(nlsql_conn_t* conn);
/** 连接 ID（进程内唯一，单调递增）。 */
uint64_t nlsql_ext_conn_id(nlsql_conn_t* conn);

#define NLSQL_EXT_PROC_MAX 64

/** 进程列表项（对应 SHOW PROCESSLIST 的常用列）。 */
typedef struct nlsql_ext_proc {
    uint64_t id;
    char     user[65];
    char     db[65];
    int      readonly;
} nlsql_ext_proc_t;

/** 当前进程内活跃连接快照；返回写入的条数（<= cap）。 */
int nlsql_ext_proc_list(nlsql_ext_proc_t* out, int cap);

/* =========================================
 * 每库扩展元数据（sidecar 持久化）
 *
 * 扩展的 DDL 元数据（视图 / 触发器 / 二级索引 / 存储过程 / 授权）不属于核心
 * 表结构，核心不会持久化。本接口把定义写入 <data_dir>/<db>/nlsql_ext_meta.dat，
 * 随库持久化，扩展在重新打开数据目录后按记录重建。
 *
 * 记录为四元组 (kind, name, target, def)，同一库内 (kind, name) 唯一，
 * 重复 put 即覆盖。kind / name / target / def 均为调用方字符串，内部复制。
 * ========================================= */

/** 遍历回调：返回非 0 表示提前结束（该值即 each 的返回值）。 */
typedef int (*nlsql_ext_meta_cb)(const char* kind, const char* name,
                                 const char* target, const char* def, void* ud);

/** 写入/覆盖一条元数据（需已 USE 某库；否则返回 NLSQL_ERR）。 */
int nlsql_ext_meta_put(nlsql_conn_t* conn, const char* kind, const char* name,
                       const char* target, const char* def);
/** 删除一条元数据（不存在也算成功）。 */
int nlsql_ext_meta_del(nlsql_conn_t* conn, const char* kind, const char* name);
/** 遍历当前库全部元数据；cb 返回非 0 时提前结束并返回该值。 */
int nlsql_ext_meta_each(nlsql_conn_t* conn, nlsql_ext_meta_cb cb, void* ud);

#ifdef __cplusplus
}
#endif

#endif /* NETLEAF_NLSQL_EXT_H */
