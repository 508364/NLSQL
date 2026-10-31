/**
 * @file nlsql_ext_info.c
 * @brief NLSQL information_schema 虚拟库扩展
 *
 * 核心没有系统表，SHOW 族之外也无从查询元数据。本扩展以语句处理器接管
 *
 *   SELECT <列清单|*> FROM information_schema.<表> [WHERE ...] [ORDER BY ...] [LIMIT n]
 *   USE information_schema            （返回 OK，不改变真实当前库）
 *
 * 之后同一连接上还支持无前缀写法 `SELECT ... FROM <表>`（"视图模式"标记）。
 *
 * 虚拟表：SCHEMATA / TABLES / COLUMNS / VIEWS / TRIGGERS / STATISTICS，
 * 列名与顺序尽量对齐 MySQL 8.0（只保留常用列）。
 *
 * 数据来源：
 *   - SCHEMATA ：SHOW DATABASES
 *   - TABLES   ：逐库 USE <db> + SHOW TABLES
 *   - COLUMNS  ：逐表 SHOW FULL COLUMNS（不可用时回退 SHOW COLUMNS）
 *   - VIEWS / TRIGGERS：nlsql_ext_meta_each（仅读当前库 sidecar，不执行 SQL）
 *   - STATISTICS：逐表 SHOW INDEX
 * 遍历结束后按需 USE <原库> 还原（原库为空则不还原）。
 *
 * 查询求值由本扩展自行完成：先按固定 schema 构造完整行集，再做
 * WHERE（AND 连接的等值/比较/IS [NOT] NULL）、ORDER BY（单/多列）、
 * LIMIT（支持 `LIMIT n`、`LIMIT off, n`、`LIMIT n OFFSET off`）、投影。
 * 结果集用与核心兼容的 arena 布局构造，可被 nlsql_free_result 直接释放。
 *
 * 只读，不做任何权限判断；未命中本扩展的语句一律返回 NLSQL_EXT_UNHANDLED。
 */
#define _GNU_SOURCE
#include "netleaf_nlsql.h"
#include "netleaf_nlsql_ext.h"
#include "netleaf_nlsql_platform.h"
#if __has_include("netleaf_module.h")
#include "netleaf_module.h"
#define NLSQL_HAVE_NETLEAF_HEADER 1
#else
#define NLSQL_HAVE_NETLEAF_HEADER 0
#endif

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define EXT_INFO_ID "nlsql_ext_info"

#define INF_NAME_MAX  65    /* 标识符上限 64 + NUL */
#define INF_VAL_MAX   128   /* 字符串字面量缓冲 */
#define INF_MAX_COLS  16    /* 单表最大列数 */
#define INF_PROJ_MAX  32    /* SELECT 列清单上限 */
#define INF_COND_MAX  16    /* WHERE 条件上限 */
#define INF_ORDER_MAX 4     /* ORDER BY 项上限 */
#define INF_MAX_DBS   64    /* SHOW DATABASES 上限 */
#define INF_FLAG_MAX  32    /* 连接 → 视图模式 小表容量 */

/* =========================================
 * 虚拟表定义
 * ========================================= */

static const char* const INF_SCHEMATA_COLS[] = {
    "CATALOG_NAME", "SCHEMA_NAME", "DEFAULT_CHARACTER_SET_NAME",
    "DEFAULT_COLLATION_NAME", "SQL_PATH"
};
static const char* const INF_TABLES_COLS[] = {
    "TABLE_CATALOG", "TABLE_SCHEMA", "TABLE_NAME", "TABLE_TYPE", "ENGINE", "ROW_FORMAT"
};
static const char* const INF_COLUMNS_COLS[] = {
    "TABLE_CATALOG", "TABLE_SCHEMA", "TABLE_NAME", "COLUMN_NAME", "ORDINAL_POSITION",
    "COLUMN_DEFAULT", "IS_NULLABLE", "DATA_TYPE", "CHARACTER_MAXIMUM_LENGTH",
    "COLUMN_TYPE", "COLUMN_KEY", "EXTRA"
};
static const char* const INF_VIEWS_COLS[] = {
    "TABLE_CATALOG", "TABLE_SCHEMA", "TABLE_NAME", "VIEW_DEFINITION", "CHECK_OPTION",
    "IS_UPDATABLE", "DEFINER", "SECURITY_TYPE", "CHARACTER_SET_CLIENT",
    "COLLATION_CONNECTION"
};
static const char* const INF_TRIGGERS_COLS[] = {
    "TRIGGER_CATALOG", "TRIGGER_SCHEMA", "TRIGGER_NAME", "EVENT_MANIPULATION",
    "EVENT_OBJECT_TABLE", "ACTION_STATEMENT", "ACTION_TIMING"
};
static const char* const INF_STATISTICS_COLS[] = {
    "TABLE_CATALOG", "TABLE_SCHEMA", "TABLE_NAME", "NON_UNIQUE", "INDEX_SCHEMA",
    "INDEX_NAME", "SEQ_IN_INDEX", "COLUMN_NAME", "COLLATION", "CARDINALITY",
    "INDEX_TYPE"
};

typedef enum {
    INF_SCHEMATA = 0, INF_TABLES, INF_COLUMNS, INF_VIEWS, INF_TRIGGERS, INF_STATISTICS
} inf_kind_t;

typedef struct {
    const char*        name;
    const char* const* cols;
    int                ncols;
    int                kind;
} inf_table_t;

static const inf_table_t g_inf_tables[] = {
    { "SCHEMATA",   INF_SCHEMATA_COLS,   5,  INF_SCHEMATA },
    { "TABLES",     INF_TABLES_COLS,     6,  INF_TABLES },
    { "COLUMNS",    INF_COLUMNS_COLS,    12, INF_COLUMNS },
    { "VIEWS",      INF_VIEWS_COLS,      10, INF_VIEWS },
    { "TRIGGERS",   INF_TRIGGERS_COLS,   7,  INF_TRIGGERS },
    { "STATISTICS", INF_STATISTICS_COLS, 11, INF_STATISTICS },
};
#define INF_TABLE_COUNT ((int)(sizeof(g_inf_tables) / sizeof(g_inf_tables[0])))

static const inf_table_t* inf_table_find(const char* name) {
    for (int i = 0; i < INF_TABLE_COUNT; i++)
        if (strcasecmp(g_inf_tables[i].name, name) == 0) return &g_inf_tables[i];
    return NULL;
}

static int inf_col_index(const inf_table_t* tb, const char* name) {
    for (int i = 0; i < tb->ncols; i++)
        if (strcasecmp(tb->cols[i], name) == 0) return i;
    return -1;
}

/* =========================================
 * 词法辅助
 * ========================================= */

static int inf_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '$' || c == '`';
}

/* p 是否以整词 w 开头（大小写无关） */
static int starts_word(const char* p, const char* w) {
    size_t n = strlen(w);
    if (strncasecmp(p, w, n) != 0) return 0;
    return !inf_ident_char(p[n]);
}

/* 整词查找（跳过引号内文本） */
static int find_word_ci(const char* sql, const char* w) {
    size_t wl = strlen(w);
    int inq = 0;
    char qc = 0;
    for (const char* p = sql; *p; p++) {
        if (inq) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == qc) inq = 0;
            continue;
        }
        if (*p == '\'' || *p == '"' || *p == '`') { inq = 1; qc = *p; continue; }
        if (strncasecmp(p, w, wl) == 0 &&
            (p == sql || !inf_ident_char(p[-1])) && !inf_ident_char(p[wl])) return 1;
    }
    return 0;
}

/* 解析 USE 后的库名；成功返回 1 */
static int parse_use_name(const char* p, char* out, size_t osz) {
    p += 3; /* "USE" */
    while (*p && isspace((unsigned char)*p)) p++;
    size_t n = 0;
    if (*p == '`') {
        p++;
        while (*p && *p != '`' && n + 1 < osz) out[n++] = *p++;
        if (*p == '`') p++;
    } else {
        while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '$') && n + 1 < osz)
            out[n++] = *p++;
    }
    out[n] = '\0';
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == ';') p++;
    while (*p && isspace((unsigned char)*p)) p++;
    return *p ? 0 : (n > 0);
}

/* =========================================
 * 行集（固定 schema 的完整行集，先构造后过滤）
 * ========================================= */

typedef struct {
    char* s;
    int   is_null;
} icell_t;

typedef struct {
    const char* const* names;
    int                ncols;
    icell_t*           cells;
    uint64_t           nrows, cap;
} iset_t;

static void iset_init(iset_t* s, const char* const* names, int ncols) {
    memset(s, 0, sizeof(*s));
    s->names = names;
    s->ncols = ncols;
}

static int iset_push(iset_t* s, const char* const* vals) {
    if (s->nrows >= s->cap) {
        uint64_t nc = s->cap ? s->cap * 2 : 64;
        icell_t* nv = (icell_t*)realloc(s->cells, (size_t)nc * (size_t)s->ncols * sizeof(icell_t));
        if (!nv) return -1;
        s->cells = nv;
        s->cap = nc;
    }
    icell_t* row = &s->cells[s->nrows * (uint64_t)s->ncols];
    for (int i = 0; i < s->ncols; i++) {
        row[i].s = NULL;
        row[i].is_null = 0;
        if (!vals[i]) { row[i].is_null = 1; continue; }
        row[i].s = strdup(vals[i]);
        if (!row[i].s) row[i].is_null = 1; /* 内存不足时降级为 NULL，不中断查询 */
    }
    s->nrows++;
    return 0;
}

static void iset_free(iset_t* s) {
    for (uint64_t r = 0; r < s->nrows; r++) {
        icell_t* row = &s->cells[r * (uint64_t)s->ncols];
        for (int i = 0; i < s->ncols; i++) free(row[i].s);
    }
    free(s->cells);
    memset(s, 0, sizeof(*s));
}

/* 读取结果集某格为文本；is_null 输出是否 SQL NULL */
static void read_str(const nlsql_result_t* r, uint64_t row, int col,
                     char* out, size_t osz, int* is_null) {
    out[0] = '\0';
    if (is_null) *is_null = 1;
    if (!r || col < 0 || col >= r->col_count || row >= r->row_count) return;
    const nlsql_value_t* v = &r->rows[row].vals[col];
    if (!v || v->is_null || v->type == NLSQL_TYPE_NULL) return;
    if (is_null) *is_null = 0;
    if (v->type == NLSQL_TYPE_INT64) snprintf(out, osz, "%lld", (long long)v->i);
    else if (v->type == NLSQL_TYPE_DOUBLE) snprintf(out, osz, "%.15g", v->d);
    else snprintf(out, osz, "%s", v->s ? v->s : "");
}

/* =========================================
 * SELECT 子集解析
 * ========================================= */

typedef enum {
    IT_EOF = 0, IT_IDENT, IT_STR, IT_NUM, IT_SYM,
    IT_LP, IT_RP, IT_COMMA, IT_DOT, IT_STAR, IT_SEMI
} itok_type_t;

typedef struct {
    int   type;
    char* text;   /* 标识符（小写）/字符串（已解转义）/数字原文/运算符原文 */
    int   len;
} itok_t;

typedef struct {
    const char* sql;
    int         pos;
    int         len;
} iscanner_t;

static void ilex(iscanner_t* sc, itok_t* t) {
    memset(t, 0, sizeof(*t));
    while (sc->pos < sc->len && isspace((unsigned char)sc->sql[sc->pos])) sc->pos++;
    if (sc->pos >= sc->len) { t->type = IT_EOF; return; }
    char c = sc->sql[sc->pos];
    if (c == '`') { /* 反引号标识符 */
        int st = ++sc->pos;
        while (sc->pos < sc->len && sc->sql[sc->pos] != '`') sc->pos++;
        int n = sc->pos - st;
        if (sc->pos < sc->len) sc->pos++;
        t->type = IT_IDENT;
        t->len = n;
        t->text = (char*)malloc((size_t)n + 1);
        if (t->text) {
            for (int i = 0; i < n; i++)
                t->text[i] = (char)tolower((unsigned char)sc->sql[st + i]);
            t->text[n] = '\0';
        }
        return;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        int st = sc->pos;
        while (sc->pos < sc->len &&
               (isalnum((unsigned char)sc->sql[sc->pos]) || sc->sql[sc->pos] == '_' ||
                sc->sql[sc->pos] == '$')) sc->pos++;
        int n = sc->pos - st;
        t->type = IT_IDENT;
        t->len = n;
        t->text = (char*)malloc((size_t)n + 1);
        if (t->text) {
            for (int i = 0; i < n; i++)
                t->text[i] = (char)tolower((unsigned char)sc->sql[st + i]);
            t->text[n] = '\0';
        }
        return;
    }
    if (isdigit((unsigned char)c)) {
        int st = sc->pos;
        while (sc->pos < sc->len && isdigit((unsigned char)sc->sql[sc->pos])) sc->pos++;
        if (sc->pos < sc->len && sc->sql[sc->pos] == '.') {
            sc->pos++;
            while (sc->pos < sc->len && isdigit((unsigned char)sc->sql[sc->pos])) sc->pos++;
        }
        int n = sc->pos - st;
        t->type = IT_NUM;
        t->len = n;
        t->text = (char*)malloc((size_t)n + 1);
        if (t->text) {
            memcpy(t->text, sc->sql + st, (size_t)n);
            t->text[n] = '\0';
        }
        return;
    }
    if (c == '\'' || c == '"') {
        char qc = c;
        int st = ++sc->pos;
        char* buf = (char*)malloc((size_t)(sc->len - st) + 1);
        if (!buf) { t->type = IT_EOF; return; }
        int o = 0;
        while (sc->pos < sc->len) {
            char ch = sc->sql[sc->pos];
            if (ch == '\\' && sc->pos + 1 < sc->len) {
                buf[o++] = sc->sql[sc->pos + 1];
                sc->pos += 2;
                continue;
            }
            if (ch == qc) {
                if (sc->pos + 1 < sc->len && sc->sql[sc->pos + 1] == qc) {
                    buf[o++] = qc;
                    sc->pos += 2;
                    continue;
                }
                sc->pos++;
                break;
            }
            buf[o++] = ch;
            sc->pos++;
        }
        buf[o] = '\0';
        t->type = IT_STR;
        t->text = buf;
        t->len = o;
        return;
    }
    switch (c) {
        case '(': t->type = IT_LP; sc->pos++; return;
        case ')': t->type = IT_RP; sc->pos++; return;
        case ',': t->type = IT_COMMA; sc->pos++; return;
        case '.': t->type = IT_DOT; sc->pos++; return;
        case '*': t->type = IT_STAR; sc->pos++; return;
        case ';': t->type = IT_SEMI; sc->pos++; return;
        default: break;
    }
    t->type = IT_SYM;
    if ((c == '<' || c == '>' || c == '!') && sc->pos + 1 < sc->len) {
        char nc = sc->sql[sc->pos + 1];
        if (nc == '=' || (c == '<' && nc == '>')) {
            t->text = (char*)malloc(3);
            if (t->text) {
                t->text[0] = c;
                t->text[1] = nc;
                t->text[2] = '\0';
            }
            t->len = 2;
            sc->pos += 2;
            return;
        }
    }
    t->text = (char*)malloc(2);
    if (t->text) {
        t->text[0] = c;
        t->text[1] = '\0';
    }
    t->len = 1;
    sc->pos++;
}

typedef struct {
    iscanner_t sc;
    itok_t     cur;
} iparser_t;

static void ip_next(iparser_t* p) {
    free(p->cur.text);
    p->cur.text = NULL;
    ilex(&p->sc, &p->cur);
}

static void ip_init(iparser_t* p, const char* sql) {
    memset(p, 0, sizeof(*p));
    p->sc.sql = sql;
    p->sc.len = (int)strlen(sql);
    ilex(&p->sc, &p->cur);
}

static void ip_free(iparser_t* p) {
    free(p->cur.text);
    p->cur.text = NULL;
}

static int ip_kw(const iparser_t* p, const char* w) {
    return p->cur.type == IT_IDENT && p->cur.text && strcasecmp(p->cur.text, w) == 0;
}

static int ip_sym(const iparser_t* p, const char* s) {
    return p->cur.type == IT_SYM && p->cur.text && strcmp(p->cur.text, s) == 0;
}

/* FROM/WHERE/... 等终止关键字（用于判断别名是否出现） */
static int ip_stop_kw(const iparser_t* p) {
    static const char* const kw[] = { "from", "where", "order", "limit", "group", "having", "and" };
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++)
        if (ip_kw(p, kw[i])) return 1;
    return 0;
}

typedef struct {
    char name[INF_NAME_MAX];
    char alias[INF_NAME_MAX];
} iproj_t;

typedef struct {
    char   col[INF_NAME_MAX];
    int    op;        /* 0 = 1 != 2 < 3 <= 4 > 5 >= 6 IS NULL 7 IS NOT NULL */
    int    rhs_kind;  /* 0 字符串 1 数字 2 NULL */
    char   rhs_str[INF_VAL_MAX];
} icond_t;

typedef struct {
    int     star;
    iproj_t proj[INF_PROJ_MAX];
    int     nproj;
    char    schema[INF_NAME_MAX];
    char    table[INF_NAME_MAX];
    icond_t cond[INF_COND_MAX];
    int     ncond;
    char    order_col[INF_ORDER_MAX][INF_NAME_MAX];
    int     order_desc[INF_ORDER_MAX];
    int     norder;
    long long limit;
    int     has_limit;
    long long offset;
    int     has_offset;
} iquery_t;

/* 列引用：可带限定前缀（db.tbl.col），取最后一段。返回 -1 失败，-2 命中 `x.*` */
static int ip_colref(iparser_t* p, char* out, size_t osz) {
    if (p->cur.type != IT_IDENT || !p->cur.text) return -1;
    snprintf(out, osz, "%s", p->cur.text);
    ip_next(p);
    while (p->cur.type == IT_DOT) {
        ip_next(p);
        if (p->cur.type == IT_STAR) { ip_next(p); return -2; }
        if (p->cur.type != IT_IDENT || !p->cur.text) return -1;
        snprintf(out, osz, "%s", p->cur.text);
        ip_next(p);
    }
    return 0;
}

static int ip_literal(iparser_t* p, icond_t* c) {
    if (p->cur.type == IT_STR) {
        c->rhs_kind = 0;
        snprintf(c->rhs_str, sizeof(c->rhs_str), "%s", p->cur.text ? p->cur.text : "");
        ip_next(p);
        return 0;
    }
    if (p->cur.type == IT_NUM) {
        c->rhs_kind = 1;
        snprintf(c->rhs_str, sizeof(c->rhs_str), "%s", p->cur.text ? p->cur.text : "");
        ip_next(p);
        return 0;
    }
    if (ip_kw(p, "null")) {
        c->rhs_kind = 2;
        ip_next(p);
        return 0;
    }
    return -1;
}

static int ip_predicate(iparser_t* p, icond_t* c) {
    memset(c, 0, sizeof(*c));
    if (ip_colref(p, c->col, sizeof(c->col)) != 0) return -1;
    if (ip_kw(p, "is")) {
        ip_next(p);
        int neg = 0;
        if (ip_kw(p, "not")) { neg = 1; ip_next(p); }
        if (!ip_kw(p, "null")) return -1;
        ip_next(p);
        c->op = neg ? 7 : 6;
        c->rhs_kind = 2;
        return 0;
    }
    int op = -1;
    if (ip_sym(p, "=")) op = 0;
    else if (ip_sym(p, "!=") || ip_sym(p, "<>")) op = 1;
    else if (ip_sym(p, "<")) op = 2;
    else if (ip_sym(p, "<=")) op = 3;
    else if (ip_sym(p, ">")) op = 4;
    else if (ip_sym(p, ">=")) op = 5;
    if (op < 0) return -1;
    ip_next(p);
    if (ip_literal(p, c) != 0) return -1;
    c->op = op;
    return 0;
}

static int ip_where(iparser_t* p, iquery_t* q) {
    for (;;) {
        if (q->ncond >= INF_COND_MAX) return -1;
        if (ip_predicate(p, &q->cond[q->ncond]) != 0) return -1;
        q->ncond++;
        if (ip_kw(p, "and")) { ip_next(p); continue; }
        break;
    }
    return 0;
}

static int ip_projection(iparser_t* p, iquery_t* q) {
    if (p->cur.type == IT_STAR) { q->star = 1; ip_next(p); return 0; }
    for (;;) {
        if (q->star) break;
        if (q->nproj >= INF_PROJ_MAX) return -1;
        iproj_t* pr = &q->proj[q->nproj];
        memset(pr, 0, sizeof(*pr));
        char nm[INF_NAME_MAX];
        int rc = ip_colref(p, nm, sizeof(nm));
        if (rc == -1) return -1;
        if (rc == -2) {
            q->star = 1;
        } else {
            snprintf(pr->name, sizeof(pr->name), "%s", nm);
            if (ip_kw(p, "as")) {
                ip_next(p);
                if (p->cur.type != IT_IDENT || !p->cur.text) return -1;
                snprintf(pr->alias, sizeof(pr->alias), "%s", p->cur.text);
                ip_next(p);
            } else if (p->cur.type == IT_IDENT && !ip_stop_kw(p)) {
                snprintf(pr->alias, sizeof(pr->alias), "%s", p->cur.text);
                ip_next(p);
            }
            q->nproj++;
        }
        if (p->cur.type == IT_COMMA) { ip_next(p); continue; }
        break;
    }
    return 0;
}

static int ip_source(iparser_t* p, iquery_t* q) {
    if (p->cur.type != IT_IDENT || !p->cur.text) return -1;
    char first[INF_NAME_MAX];
    snprintf(first, sizeof(first), "%s", p->cur.text);
    ip_next(p);
    if (p->cur.type == IT_DOT) {
        ip_next(p);
        if (p->cur.type != IT_IDENT || !p->cur.text) return -1;
        snprintf(q->schema, sizeof(q->schema), "%s", first);
        snprintf(q->table, sizeof(q->table), "%s", p->cur.text);
        ip_next(p);
    } else {
        snprintf(q->table, sizeof(q->table), "%s", first);
    }
    if (ip_kw(p, "as")) {
        ip_next(p);
        if (p->cur.type != IT_IDENT) return -1;
        ip_next(p);
    } else if (p->cur.type == IT_IDENT && !ip_stop_kw(p)) {
        ip_next(p);
    }
    return 0;
}

static int ip_select(iparser_t* p, iquery_t* q) {
    memset(q, 0, sizeof(*q));
    if (!ip_kw(p, "select")) return -1;
    ip_next(p);
    if (ip_kw(p, "distinct")) ip_next(p);
    if (ip_projection(p, q) != 0) return -1;
    if (!ip_kw(p, "from")) return -1;
    ip_next(p);
    if (ip_source(p, q) != 0) return -1;
    if (ip_kw(p, "where")) {
        ip_next(p);
        if (ip_where(p, q) != 0) return -1;
    }
    if (ip_kw(p, "order")) {
        ip_next(p);
        if (!ip_kw(p, "by")) return -1;
        ip_next(p);
        for (;;) {
            if (q->norder >= INF_ORDER_MAX) return -1;
            char nm[INF_NAME_MAX];
            if (ip_colref(p, nm, sizeof(nm)) != 0) return -1;
            snprintf(q->order_col[q->norder], INF_NAME_MAX, "%s", nm);
            q->order_desc[q->norder] = 0;
            if (ip_kw(p, "desc")) { q->order_desc[q->norder] = 1; ip_next(p); }
            else if (ip_kw(p, "asc")) ip_next(p);
            q->norder++;
            if (p->cur.type == IT_COMMA) { ip_next(p); continue; }
            break;
        }
    }
    if (ip_kw(p, "limit")) {
        ip_next(p);
        if (p->cur.type != IT_NUM) return -1;
        long long v = strtoll(p->cur.text ? p->cur.text : "0", NULL, 10);
        ip_next(p);
        if (p->cur.type == IT_COMMA) {
            ip_next(p);
            if (p->cur.type != IT_NUM) return -1;
            q->offset = v;
            q->has_offset = 1;
            q->limit = strtoll(p->cur.text ? p->cur.text : "0", NULL, 10);
            q->has_limit = 1;
            ip_next(p);
        } else if (ip_kw(p, "offset")) {
            ip_next(p);
            if (p->cur.type != IT_NUM) return -1;
            q->offset = strtoll(p->cur.text ? p->cur.text : "0", NULL, 10);
            q->has_offset = 1;
            q->limit = v;
            q->has_limit = 1;
            ip_next(p);
        } else {
            q->limit = v;
            q->has_limit = 1;
        }
    }
    if (p->cur.type == IT_SEMI) ip_next(p);
    return p->cur.type == IT_EOF ? 0 : -1;
}

/* =========================================
 * 结果集构造（与核心 arena 布局兼容，交给 nlsql_free_result 释放）
 * ========================================= */

typedef struct inf_arena_blk {
    struct inf_arena_blk* next;
    uint32_t              used, cap;
    char                  data[1];
} inf_arena_blk_t;

static nlsql_result_t* inf_make_result(int ncols, uint64_t nrows) {
    nlsql_result_t* r = (nlsql_result_t*)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->col_count = ncols;
    r->cols = (nlsql_column_t*)calloc((size_t)ncols, sizeof(nlsql_column_t));
    if (!r->cols) { free(r); return NULL; }
    if (nrows == 0) return r;
    r->rows = (nlsql_row_t*)calloc((size_t)nrows, sizeof(nlsql_row_t));
    if (!r->rows) { free(r->cols); free(r); return NULL; }
    size_t need = (size_t)nrows * (size_t)ncols * sizeof(nlsql_value_t);
    inf_arena_blk_t* blk = (inf_arena_blk_t*)malloc(sizeof(inf_arena_blk_t) + need + 8);
    if (!blk) { free(r->rows); free(r->cols); free(r); return NULL; }
    blk->next = NULL;
    blk->used = (uint32_t)need;
    blk->cap = (uint32_t)(need + 8);
    r->_arena = blk;
    char* base = blk->data;
    for (uint64_t i = 0; i < nrows; i++)
        r->rows[i].vals = (nlsql_value_t*)(base + i * (size_t)ncols * sizeof(nlsql_value_t));
    r->row_count = nrows;
    return r;
}

/* =========================================
 * 数据采集
 * ========================================= */

static void cur_db_copy(nlsql_conn_t* conn, char* out, size_t osz) {
    const char* db = nlsql_ext_conn_db(conn);
    snprintf(out, osz, "%s", db ? db : "");
}

/* 还原当前库（原库为空则不还原） */
static void restore_db(nlsql_conn_t* conn, const char* db) {
    if (!db || !db[0]) return;
    char sql[128];
    snprintf(sql, sizeof(sql), "USE %s", db);
    nlsql_query(conn, sql, NULL);
}

static int list_dbs(nlsql_conn_t* conn, char dbs[][INF_NAME_MAX], int max) {
    nlsql_result_t* r = NULL;
    if (nlsql_query(conn, "SHOW DATABASES", &r) != NLSQL_OK) {
        nlsql_free_result(r);
        return -1;
    }
    int n = 0;
    for (uint64_t i = 0; i < r->row_count && n < max; i++) {
        char buf[INF_NAME_MAX];
        int is_null = 0;
        read_str(r, i, 0, buf, sizeof(buf), &is_null);
        if (is_null || !buf[0]) continue;
        snprintf(dbs[n], INF_NAME_MAX, "%s", buf);
        n++;
    }
    nlsql_free_result(r);
    return n;
}

static int build_schemata(nlsql_conn_t* conn, iset_t* set) {
    nlsql_result_t* r = NULL;
    if (nlsql_query(conn, "SHOW DATABASES", &r) != NLSQL_OK) {
        nlsql_free_result(r);
        return -1;
    }
    for (uint64_t i = 0; i < r->row_count; i++) {
        char db[INF_NAME_MAX];
        int is_null = 0;
        read_str(r, i, 0, db, sizeof(db), &is_null);
        if (is_null || !db[0]) continue;
        const char* vals[5] = { "def", db, "utf8mb4", "utf8mb4_general_ci", NULL };
        iset_push(set, vals);
    }
    nlsql_free_result(r);
    return 0;
}

/* 拆 Type 文本：varchar(16) → base=varchar, len=16 */
static void type_split(const char* type, char* base, size_t bsz, char* len, size_t lsz) {
    base[0] = '\0';
    len[0] = '\0';
    const char* lp = strchr(type, '(');
    size_t n = lp ? (size_t)(lp - type) : strlen(type);
    if (n >= bsz) n = bsz - 1;
    memcpy(base, type, n);
    base[n] = '\0';
    if (lp) {
        lp++;
        size_t m = 0;
        while (*lp && *lp != ')' && m + 1 < lsz) len[m++] = *lp++;
        len[m] = '\0';
    }
}

static void push_columns(nlsql_conn_t* conn, iset_t* set, const char* db, const char* table) {
    char sql[224];
    nlsql_result_t* r = NULL;
    snprintf(sql, sizeof(sql), "SHOW FULL COLUMNS FROM %s", table);
    if (nlsql_query(conn, sql, &r) != NLSQL_OK) {
        nlsql_free_result(r);
        r = NULL;
        snprintf(sql, sizeof(sql), "SHOW COLUMNS FROM %s", table);
        if (nlsql_query(conn, sql, &r) != NLSQL_OK) {
            nlsql_free_result(r);
            return;
        }
    }
    /* SHOW FULL COLUMNS: Field/Type/Collation/Null/Key/Default/Extra/...
       SHOW COLUMNS     : Field/Type/Null/Key */
    int full = (r->col_count >= 7);
    int c_field = 0, c_type = 1;
    int c_null = full ? 3 : 2;
    int c_key = full ? 4 : 3;
    int c_def = full ? 5 : -1;
    int c_extra = full ? 6 : -1;
    for (uint64_t i = 0; i < r->row_count; i++) {
        char field[INF_NAME_MAX], type[64], nullable[8], key[8];
        char def[INF_VAL_MAX], extra[32];
        int dnull = 1, enull = 1;
        read_str(r, i, c_field, field, sizeof(field), NULL);
        read_str(r, i, c_type, type, sizeof(type), NULL);
        read_str(r, i, c_null, nullable, sizeof(nullable), NULL);
        read_str(r, i, c_key, key, sizeof(key), NULL);
        read_str(r, i, c_def, def, sizeof(def), &dnull);
        read_str(r, i, c_extra, extra, sizeof(extra), &enull);
        if (!field[0]) continue;

        char pos[24];
        snprintf(pos, sizeof(pos), "%llu", (unsigned long long)(i + 1));

        char base[48], len[24];
        type_split(type, base, sizeof(base), len, sizeof(len));

        /* CHARACTER_MAXIMUM_LENGTH 仅对字符类型有意义 */
        const char* charlen = NULL;
        if (len[0] && (strcmp(base, "varchar") == 0 || strcmp(base, "char") == 0))
            charlen = len;

        const char* vals[12] = {
            "def", db, table, field, pos,
            dnull ? NULL : def, nullable[0] ? nullable : "", base[0] ? base : type,
            charlen, type, key, enull ? "" : extra
        };
        iset_push(set, vals);
    }
    nlsql_free_result(r);
}

static void push_statistics(nlsql_conn_t* conn, iset_t* set, const char* db, const char* table) {
    char sql[200];
    nlsql_result_t* r = NULL;
    snprintf(sql, sizeof(sql), "SHOW INDEX FROM %s", table);
    if (nlsql_query(conn, sql, &r) != NLSQL_OK) {
        nlsql_free_result(r);
        return;
    }
    if (r->col_count < 7) { nlsql_free_result(r); return; }
    int c_type = r->col_count > 10 ? 10 : -1;
    for (uint64_t i = 0; i < r->row_count; i++) {
        char nonuniq[8] = "", iname[INF_NAME_MAX] = "", seq[24] = "";
        char colname[INF_NAME_MAX] = "", coll[8] = "", card[24] = "", itype[32] = "";
        read_str(r, i, 1, nonuniq, sizeof(nonuniq), NULL);
        read_str(r, i, 2, iname, sizeof(iname), NULL);
        read_str(r, i, 3, seq, sizeof(seq), NULL);
        read_str(r, i, 4, colname, sizeof(colname), NULL);
        read_str(r, i, 5, coll, sizeof(coll), NULL);
        read_str(r, i, 6, card, sizeof(card), NULL);
        read_str(r, i, c_type, itype, sizeof(itype), NULL);
        if (!iname[0]) continue;
        const char* vals[11] = {
            "def", db, table, nonuniq[0] ? nonuniq : "0", db, iname, seq, colname,
            coll[0] ? coll : "A", card, itype[0] ? itype : "BTREE"
        };
        iset_push(set, vals);
    }
    nlsql_free_result(r);
}

typedef struct {
    iset_t*     set;
    const char* db;
    int         kind;      /* INF_VIEWS / INF_TRIGGERS */
    char        definer[96];
} meta_ctx_t;

static int meta_collect_cb(const char* kind, const char* name, const char* target,
                           const char* def, void* ud) {
    meta_ctx_t* ctx = (meta_ctx_t*)ud;
    if (ctx->kind == INF_VIEWS) {
        if (strcmp(kind, "view") != 0) return 0;
        const char* vals[10] = {
            "def", ctx->db, name, def, "NONE", "YES", ctx->definer, "DEFINER",
            "utf8", "utf8_general_ci"
        };
        iset_push(ctx->set, vals);
        return 0;
    }
    if (strcmp(kind, "trigger") != 0) return 0;
    /* def 形如 "<事件号>\t<语句>"；事件号 1/2/3 对应 INSERT/UPDATE/DELETE */
    char event[16];
    const char* stmt = def ? def : "";
    event[0] = '\0';
    const char* tab = def ? strchr(def, '\t') : NULL;
    if (tab) {
        int n = (int)(tab - def);
        if (n > 0 && n < (int)sizeof(event)) {
            int digits = 1;
            for (int i = 0; i < n; i++)
                if (!isdigit((unsigned char)def[i])) { digits = 0; break; }
            memcpy(event, def, (size_t)n);
            event[n] = '\0';
            if (digits) {
                int e = atoi(event);
                snprintf(event, sizeof(event), "%s",
                         e == 1 ? "INSERT" : e == 2 ? "UPDATE" : e == 3 ? "DELETE" : "");
            } else {
                event[0] = '\0';
            }
        }
        stmt = tab + 1;
    }
    const char* vals[7] = { "def", ctx->db, name, event, target, stmt, "AFTER" };
    iset_push(ctx->set, vals);
    return 0;
}

static int build_meta_objects(nlsql_conn_t* conn, iset_t* set, int kind) {
    char orig[INF_NAME_MAX];
    char dbs[INF_MAX_DBS][INF_NAME_MAX];
    cur_db_copy(conn, orig, sizeof(orig));
    int ndb = list_dbs(conn, dbs, INF_MAX_DBS);
    if (ndb <= 0) return 0;

    meta_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.set = set;
    ctx.kind = kind;
    snprintf(ctx.definer, sizeof(ctx.definer), "%s@localhost", nlsql_ext_conn_user(conn));

    for (int d = 0; d < ndb; d++) {
        char sql[4200];
        snprintf(sql, sizeof(sql), "USE %s", dbs[d]);
        if (nlsql_query(conn, sql, NULL) != NLSQL_OK) continue;
        ctx.db = dbs[d];
        nlsql_ext_meta_each(conn, meta_collect_cb, &ctx);
    }
    restore_db(conn, orig);
    return 0;
}

static int build_schema_objects(nlsql_conn_t* conn, iset_t* set, int kind) {
    char orig[INF_NAME_MAX];
    char dbs[INF_MAX_DBS][INF_NAME_MAX];
    cur_db_copy(conn, orig, sizeof(orig));
    int ndb = list_dbs(conn, dbs, INF_MAX_DBS);
    if (ndb <= 0) return 0;

    for (int d = 0; d < ndb; d++) {
        char sql[4200];
        snprintf(sql, sizeof(sql), "USE %s", dbs[d]);
        if (nlsql_query(conn, sql, NULL) != NLSQL_OK) continue;

        nlsql_result_t* tr = NULL;
        if (nlsql_query(conn, "SHOW TABLES", &tr) != NLSQL_OK) {
            nlsql_free_result(tr);
            continue;
        }
        for (uint64_t i = 0; i < tr->row_count; i++) {
            char tname[INF_NAME_MAX];
            int is_null = 0;
            read_str(tr, i, 0, tname, sizeof(tname), &is_null);
            if (is_null || !tname[0]) continue;
            if (kind == INF_TABLES) {
                const char* vals[6] = { "def", dbs[d], tname, "BASE TABLE", "NLSQL", "Dynamic" };
                iset_push(set, vals);
            } else if (kind == INF_COLUMNS) {
                push_columns(conn, set, dbs[d], tname);
            } else if (kind == INF_STATISTICS) {
                push_statistics(conn, set, dbs[d], tname);
            }
        }
        nlsql_free_result(tr);
    }
    restore_db(conn, orig);
    return 0;
}

static int build_dataset(nlsql_conn_t* conn, const inf_table_t* tb, iset_t* set) {
    iset_init(set, tb->cols, tb->ncols);
    switch (tb->kind) {
        case INF_SCHEMATA: return build_schemata(conn, set);
        case INF_TABLES:
        case INF_COLUMNS:
        case INF_STATISTICS: return build_schema_objects(conn, set, tb->kind);
        case INF_VIEWS:
        case INF_TRIGGERS: return build_meta_objects(conn, set, tb->kind);
    }
    return 0;
}

/* =========================================
 * 求值：过滤 / 排序 / 投影
 * ========================================= */

static int cell_num(const char* s, double* out) {
    if (!s || !s[0]) return 0;
    char* end = NULL;
    double v = strtod(s, &end);
    if (!end || *end != '\0') return 0;
    *out = v;
    return 1;
}

static int icell_cmp(const icell_t* a, const icell_t* b) {
    if (a->is_null && b->is_null) return 0;
    if (a->is_null) return -1;
    if (b->is_null) return 1;
    double x, y;
    if (cell_num(a->s, &x) && cell_num(b->s, &y))
        return x < y ? -1 : (x > y ? 1 : 0);
    int c = strcmp(a->s ? a->s : "", b->s ? b->s : "");
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

static int icond_cmp(const icell_t* c, const icond_t* k) {
    double x, y;
    if (k->rhs_kind == 1 && cell_num(c->s, &x) && cell_num(k->rhs_str, &y))
        return x < y ? -1 : (x > y ? 1 : 0);
    int r = strcmp(c->s ? c->s : "", k->rhs_str);
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

static int row_matches(const iset_t* set, uint64_t row, const iquery_t* q, const int* cidx) {
    for (int i = 0; i < q->ncond; i++) {
        const icell_t* c = &set->cells[row * (uint64_t)set->ncols + (uint64_t)cidx[i]];
        const icond_t* k = &q->cond[i];
        if (k->op == 6) {
            if (!c->is_null) return 0;
            continue;
        }
        if (k->op == 7) {
            if (c->is_null) return 0;
            continue;
        }
        if (c->is_null) return 0;
        int cm = icond_cmp(c, k);
        switch (k->op) {
            case 0: if (cm != 0) return 0; break;
            case 1: if (cm == 0) return 0; break;
            case 2: if (cm >= 0) return 0; break;
            case 3: if (cm > 0) return 0; break;
            case 4: if (cm <= 0) return 0; break;
            case 5: if (cm < 0) return 0; break;
            default: return 0;
        }
    }
    return 1;
}

static _Thread_local const iset_t*  t_ord_set;
static _Thread_local const iquery_t* t_ord_q;
static _Thread_local int t_ord_idx[INF_ORDER_MAX];

static int order_cmp(const void* pa, const void* pb) {
    uint64_t ra = *(const uint64_t*)pa;
    uint64_t rb = *(const uint64_t*)pb;
    const iset_t* s = t_ord_set;
    const iquery_t* q = t_ord_q;
    for (int i = 0; i < q->norder; i++) {
        int ci = t_ord_idx[i];
        if (ci < 0) continue;
        const icell_t* a = &s->cells[ra * (uint64_t)s->ncols + (uint64_t)ci];
        const icell_t* b = &s->cells[rb * (uint64_t)s->ncols + (uint64_t)ci];
        int c = icell_cmp(a, b);
        if (c != 0) return q->order_desc[i] ? -c : c;
    }
    return 0;
}

/* 整数列（输出为 INT64，其余为 VARCHAR） */
static int is_num_column(const char* name) {
    static const char* const num[] = {
        "ORDINAL_POSITION", "SEQ_IN_INDEX", "NON_UNIQUE", "CARDINALITY", "ACTION_ORDER"
    };
    for (size_t i = 0; i < sizeof(num) / sizeof(num[0]); i++)
        if (strcasecmp(name, num[i]) == 0) return 1;
    return 0;
}

static void fill_cell(nlsql_value_t* v, const icell_t* c, int numeric) {
    memset(v, 0, sizeof(*v));
    if (c->is_null) {
        v->type = NLSQL_TYPE_NULL;
        v->is_null = 1;
        return;
    }
    if (numeric) {
        char* end = NULL;
        long long iv = strtoll(c->s, &end, 10);
        if (end && *end == '\0') {
            v->type = NLSQL_TYPE_INT64;
            v->i = iv;
            return;
        }
    }
    v->type = NLSQL_TYPE_VARCHAR;
    v->len = (uint32_t)strlen(c->s);
    v->s = strdup(c->s);
    if (!v->s) {
        v->type = NLSQL_TYPE_NULL;
        v->is_null = 1;
        v->len = 0;
    }
}

static int info_exec(nlsql_conn_t* conn, const inf_table_t* tb, iquery_t* q,
                     nlsql_result_t** out) {
    *out = NULL;
    iset_t set;
    memset(&set, 0, sizeof(set));
    if (build_dataset(conn, tb, &set) != 0) {
        iset_free(&set);
        nlsql_ext_set_error(conn, NLSQL_ERR, "HY000", "failed to collect information_schema data");
        return NLSQL_ERR;
    }

    int cidx[INF_COND_MAX];
    for (int i = 0; i < q->ncond; i++) {
        cidx[i] = inf_col_index(tb, q->cond[i].col);
        if (cidx[i] < 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Unknown column '%s' in 'where clause'", q->cond[i].col);
            nlsql_ext_set_error(conn, NLSQL_ER_UNKNOWN_COLUMN, "42S22", msg);
            iset_free(&set);
            return NLSQL_ER_UNKNOWN_COLUMN;
        }
    }

    int oidx[INF_ORDER_MAX];
    for (int i = 0; i < q->norder; i++) {
        oidx[i] = inf_col_index(tb, q->order_col[i]);
        if (oidx[i] < 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Unknown column '%s' in 'order clause'", q->order_col[i]);
            nlsql_ext_set_error(conn, NLSQL_ER_UNKNOWN_COLUMN, "42S22", msg);
            iset_free(&set);
            return NLSQL_ER_UNKNOWN_COLUMN;
        }
    }

    /* 投影列解析 */
    int pidx[INF_MAX_COLS];
    char pname[INF_MAX_COLS][INF_NAME_MAX];
    int np = 0;
    if (q->star) {
        for (int j = 0; j < tb->ncols; j++) {
            pidx[np] = j;
            snprintf(pname[np], INF_NAME_MAX, "%s", tb->cols[j]);
            np++;
        }
    } else {
        for (int i = 0; i < q->nproj; i++) {
            int idx = inf_col_index(tb, q->proj[i].name);
            if (idx < 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Unknown column '%s' in 'field list'", q->proj[i].name);
                nlsql_ext_set_error(conn, NLSQL_ER_UNKNOWN_COLUMN, "42S22", msg);
                iset_free(&set);
                return NLSQL_ER_UNKNOWN_COLUMN;
            }
            if (np >= INF_MAX_COLS) break;
            pidx[np] = idx;
            snprintf(pname[np], INF_NAME_MAX, "%s",
                     q->proj[i].alias[0] ? q->proj[i].alias : tb->cols[idx]);
            np++;
        }
    }
    if (np == 0) {
        iset_free(&set);
        nlsql_ext_set_error(conn, NLSQL_ERR, "HY000", "empty select list");
        return NLSQL_ERR;
    }

    uint64_t* keep = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)(set.nrows ? set.nrows : 1));
    if (!keep) {
        iset_free(&set);
        return NLSQL_ERR;
    }
    uint64_t nkeep = 0;
    for (uint64_t r = 0; r < set.nrows; r++)
        if (row_matches(&set, r, q, cidx)) keep[nkeep++] = r;

    if (q->norder > 0 && nkeep > 1) {
        for (int i = 0; i < q->norder; i++) t_ord_idx[i] = oidx[i];
        t_ord_set = &set;
        t_ord_q = q;
        qsort(keep, (size_t)nkeep, sizeof(uint64_t), order_cmp);
        t_ord_set = NULL;
        t_ord_q = NULL;
    }

    uint64_t start = (q->has_offset && q->offset > 0) ? (uint64_t)q->offset : 0;
    if (start > nkeep) start = nkeep;
    uint64_t total = nkeep - start;
    if (q->has_limit) {
        uint64_t lim = q->limit > 0 ? (uint64_t)q->limit : 0;
        if (lim < total) total = lim;
    }

    nlsql_result_t* res = inf_make_result(np, total);
    if (!res) {
        free(keep);
        iset_free(&set);
        return NLSQL_ERR;
    }
    for (int j = 0; j < np; j++) {
        /* 列名限长拷贝，避免在长 WHERE 名上截断告警 */
        size_t cn = strlen(pname[j]);
        if (cn >= sizeof(res->cols[j].name)) cn = sizeof(res->cols[j].name) - 1;
        memcpy(res->cols[j].name, pname[j], cn);
        res->cols[j].name[cn] = '\0';
        res->cols[j].type = NLSQL_TYPE_VARCHAR;
    }
    for (uint64_t k = 0; k < total; k++) {
        uint64_t row = keep[start + k];
        for (int j = 0; j < np; j++) {
            const icell_t* c = &set.cells[row * (uint64_t)set.ncols + (uint64_t)pidx[j]];
            fill_cell(&res->rows[k].vals[j], c, is_num_column(tb->cols[pidx[j]]));
            if (res->rows[k].vals[j].type == NLSQL_TYPE_INT64)
                res->cols[j].type = NLSQL_TYPE_INT64;
        }
    }
    free(keep);
    iset_free(&set);
    *out = res;
    return NLSQL_OK;
}

/* =========================================
 * 视图模式标记（连接 → 是否处于 USE information_schema 之后）
 * ========================================= */

typedef struct {
    uint64_t id;
    int      on;
} flag_slot_t;

static pthread_mutex_t g_flag_mtx = PTHREAD_MUTEX_INITIALIZER;
static flag_slot_t     g_flags[INF_FLAG_MAX];
static int             g_flag_rr;

static void flag_set(nlsql_conn_t* conn, int on) {
    uint64_t id = nlsql_ext_conn_id(conn);
    if (!id) return;
    pthread_mutex_lock(&g_flag_mtx);
    int slot = -1;
    for (int i = 0; i < INF_FLAG_MAX; i++)
        if (g_flags[i].id == id) { slot = i; break; }
    if (slot < 0) {
        slot = g_flag_rr;
        g_flag_rr = (g_flag_rr + 1) % INF_FLAG_MAX;
        g_flags[slot].id = id;
    }
    g_flags[slot].on = on ? 1 : 0;
    pthread_mutex_unlock(&g_flag_mtx);
}

static int flag_get(nlsql_conn_t* conn) {
    uint64_t id = nlsql_ext_conn_id(conn);
    if (!id) return 0;
    int on = 0;
    pthread_mutex_lock(&g_flag_mtx);
    for (int i = 0; i < INF_FLAG_MAX; i++)
        if (g_flags[i].id == id) { on = g_flags[i].on; break; }
    pthread_mutex_unlock(&g_flag_mtx);
    return on;
}

/* =========================================
 * 语句处理器
 * ========================================= */

static _Thread_local int t_info_busy;   /* 防止子语句再次进入本处理器 */

static int info_handler(nlsql_conn_t* conn, const char* sql, nlsql_result_t** out, void* ud) {
    (void)ud;
    if (out) *out = NULL;
    if (!conn || !sql) return NLSQL_EXT_UNHANDLED;
    if (t_info_busy) return NLSQL_EXT_UNHANDLED;

    const char* p = sql;
    while (*p && isspace((unsigned char)*p)) p++;

    /* USE：接管 information_schema（虚拟库，不改变真实当前库），其余库交还核心 */
    if (starts_word(p, "use")) {
        char db[INF_NAME_MAX];
        if (parse_use_name(p, db, sizeof(db))) {
            if (strcasecmp(db, "information_schema") == 0) {
                flag_set(conn, 1);
                return NLSQL_OK;
            }
            flag_set(conn, 0);
        }
        return NLSQL_EXT_UNHANDLED;
    }

    if (!starts_word(p, "select")) return NLSQL_EXT_UNHANDLED;

    /* 快速预判：非视图模式且文本里没有 information_schema 时不必解析 */
    if (!flag_get(conn) && !find_word_ci(sql, "information_schema"))
        return NLSQL_EXT_UNHANDLED;

    iparser_t ps;
    iquery_t q;
    ip_init(&ps, sql);
    int ok = ip_select(&ps, &q);
    ip_free(&ps);
    if (ok != 0) return NLSQL_EXT_UNHANDLED;

    const inf_table_t* tb = NULL;
    if (q.schema[0]) {
        if (strcasecmp(q.schema, "information_schema") != 0) return NLSQL_EXT_UNHANDLED;
        tb = inf_table_find(q.table);
        if (!tb) {
            char msg[160];
            snprintf(msg, sizeof(msg), "Table 'information_schema.%s' doesn't exist", q.table);
            nlsql_ext_set_error(conn, NLSQL_ER_NO_SUCH_TABLE, "42S02", msg);
            return NLSQL_ER_NO_SUCH_TABLE;
        }
    } else {
        tb = inf_table_find(q.table);
        /* 无前缀且视图模式关闭时不接管：可能是真实的同名表 */
        if (!tb || !flag_get(conn)) return NLSQL_EXT_UNHANDLED;
    }

    t_info_busy = 1;
    nlsql_result_t* res = NULL;
    int rc = info_exec(conn, tb, &q, &res);
    t_info_busy = 0;

    if (rc != NLSQL_OK) {
        nlsql_free_result(res);
        return rc;
    }
    if (out) *out = res;
    else nlsql_free_result(res);
    return NLSQL_OK;
}

/* =========================================
 * 生命周期
 * ========================================= */

static int g_inited;

static int info_init(void) {
    if (g_inited) return 0;
    if (nlsql_ext_register_stmt_handler(info_handler, NULL) != NLSQL_OK) return -1;
    nlsql_ext_set_feature("information_schema", 1);
    g_inited = 1;
    return 0;
}

static void info_shutdown(void) {
    if (!g_inited) return;
    nlsql_ext_unregister_stmt_handler(info_handler);
    nlsql_ext_set_feature("information_schema", 0);
    g_inited = 0;
}

/* 供测试/嵌入式调用者直接启停 */
int nlsql_ext_info_init(void) { return info_init(); }
void nlsql_ext_info_shutdown(void) { info_shutdown(); }

/* ---------------- NetLeaf 插件入口 ---------------- */

#if NLSQL_HAVE_NETLEAF_HEADER
static int ext_available(void) { return 1; }
static const char* ext_version(void) { return NLSQL_VERSION; }

NL_EXTENSION_DEFINE(nlsql_ext_info, "NLSQL Info", NLSQL_VERSION, "508364",
                    "information_schema 虚拟库",
                    "Windows,Linux,MacOS",
                    NL_CAP_THREAD_SAFE | NL_CAP_EXT_SYSTEM,
                    info_init, info_shutdown, ext_available, ext_version);

__attribute__((visibility("default")))
nl_plugin_info_t* plugin_get_info(void) {
    (void)NL_EXTENSION_GET_INFO(nlsql_ext_info);
    static nl_plugin_info_t info;
    memset(&info, 0, sizeof(info));
    info.name = "NLSQL Info";
    info.id = EXT_INFO_ID;
    info.version = NLSQL_VERSION;
    info.description = "information_schema virtual database for NLSQL";
    info.author = "508364";
    info.license = "Apache-2.0";
    info.capabilities = NL_CAP_THREAD_SAFE;
    info.platform_windows = 1; info.platform_linux = 1; info.platform_macos = 1;
    info.min_nl_version = NL_MODULE_VERSION;
    info.init = info_init;
    info.shutdown = info_shutdown;
    info.is_available = ext_available;
    return &info;
}
#endif

__attribute__((visibility("default"))) int plugin_init(void) { return info_init(); }
__attribute__((visibility("default"))) void plugin_shutdown(void) { info_shutdown(); }
