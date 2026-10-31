/**
 * @file nlsql_ext_sql.c
 * @brief NLSQL SQL 内核扩展：JOIN / 子查询 / 视图 / 触发器
 *
 * 核心解析器只覆盖单表子集；本扩展以语句处理器形式接管复杂查询，
 * 通过公共 API 读取基表数据后自行完成连接、投影与条件过滤。
 * 未命中的语句返回 NLSQL_EXT_UNHANDLED，交还核心执行，行为不变。
 *
 * 支持子集（测试覆盖）：
 *   SELECT 列表 FROM t [AS a] [INNER|LEFT] JOIN t2 [AS b] ON ... [WHERE ...]
 *   WHERE 中：比较、AND/OR/NOT、IN (列表|子查询)、EXISTS (子查询)、IS [NOT] NULL
 *   标量子查询：expr = (SELECT ...)
 *   ORDER BY expr [ASC|DESC], ... / LIMIT n / DISTINCT
 *   CREATE VIEW name AS SELECT ... / DROP VIEW name
 *   CREATE TRIGGER name AFTER INSERT|UPDATE|DELETE ON t FOR EACH ROW <stmt;>
 *   DROP TRIGGER name
 *
 * 相关子查询：子查询文本中出现「alias.col」点号限定，或位于 SELECT 列表中的
 * 标量子查询（此时外层 FROM 尚未解析、别名未知），按可能相关处理 —— 保存
 * 文本，在外层逐行求值时把外层列替换为字面量后重新执行；其余子查询仍一次性
 * 求值，保持原有性能。
 *
 * 触发器 NEW/OLD：按本语句展开为字面量后执行触发器体 —— INSERT 取 VALUES 字面量，
 * UPDATE/DELETE 先预取匹配行得到 OLD；UPDATE 的 NEW 取 SET 字面量，非字面量赋值
 * （如 SET a = b + 1）回退 OLD 值。
 *
 * 视图与触发器定义随库持久化到 <data_dir>/<db>/nlsql_ext_meta.dat（核心 meta API），
 * 切库/重开连接时按「连接 ID + 库名」懒加载重建。
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
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define EXT_SQL_ID  "nlsql_ext_sql"
#define MAX_SRC     8
#define MAX_SEL     32
#define MAX_ORDER   8
#define MAX_LIST    64
#define MAX_VIEWS   32
#define MAX_TRIGS   32
#define NAME_SZ     65

/* =========================================
 * 词法
 * ========================================= */

typedef enum {
    TT_EOF = 0, TT_IDENT, TT_NUM, TT_STR, TT_SYM, TT_LP, TT_RP, TT_COMMA, TT_DOT, TT_STAR
} tt_t;

typedef struct {
    const char* start;   /* 词起始位置（指向原 SQL） */
    int         len;
    int         type;
    long long   i;
    double      d;
    char*       sval;    /* TT_STR / TT_IDENT 的大写无关副本（小写） */
    int         slen;
} tok_t;

typedef struct {
    const char* sql;
    int         pos;
    int         len;
} scanner_t;

static void tok_free(tok_t* t) {
    free(t->sval);
    t->sval = NULL;
}

static int ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_' || c == '`';
}
static int ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '$' || c == '`';
}

static void lex_next(scanner_t* sc, tok_t* t) {
    memset(t, 0, sizeof(*t));
    while (sc->pos < sc->len && isspace((unsigned char)sc->sql[sc->pos])) sc->pos++;
    if (sc->pos >= sc->len) { t->type = TT_EOF; return; }
    char c = sc->sql[sc->pos];
    t->start = sc->sql + sc->pos;
    if (ident_start(c)) {
        int st = sc->pos;
        while (sc->pos < sc->len && ident_char(sc->sql[sc->pos])) sc->pos++;
        int n = sc->pos - st;
        t->type = TT_IDENT;
        t->len = n;
        t->sval = (char*)malloc((size_t)n + 1);
        if (t->sval) {
            for (int i = 0; i < n; i++) {
                char ch = sc->sql[st + i];
                t->sval[i] = (char)tolower((unsigned char)ch);
            }
            t->sval[n] = '\0';
        }
        t->slen = n;
        return;
    }
    if (isdigit((unsigned char)c) || (c == '.' && sc->pos + 1 < sc->len &&
        isdigit((unsigned char)sc->sql[sc->pos + 1]))) {
        int st = sc->pos;
        int isd = 0;
        while (sc->pos < sc->len && isdigit((unsigned char)sc->sql[sc->pos])) sc->pos++;
        if (sc->pos < sc->len && sc->sql[sc->pos] == '.') { isd = 1; sc->pos++;
            while (sc->pos < sc->len && isdigit((unsigned char)sc->sql[sc->pos])) sc->pos++; }
        int n = sc->pos - st;
        char num[64];
        if (n >= (int)sizeof(num)) n = (int)sizeof(num) - 1;
        memcpy(num, sc->sql + st, (size_t)n); num[n] = '\0';
        t->type = TT_NUM;
        t->len = sc->pos - st;
        if (isd) { t->d = atof(num); } else { t->i = strtoll(num, NULL, 10); }
        return;
    }
    if (c == '\'' || c == '"') {
        char q = c;
        sc->pos++;
        int st = sc->pos;
        char* buf = (char*)malloc((size_t)(sc->len - st) + 1);
        int o = 0;
        while (sc->pos < sc->len) {
            char ch = sc->sql[sc->pos];
            if (ch == '\\' && sc->pos + 1 < sc->len) { buf[o++] = sc->sql[sc->pos + 1]; sc->pos += 2; continue; }
            if (ch == q) {
                if (sc->pos + 1 < sc->len && sc->sql[sc->pos + 1] == q) { buf[o++] = q; sc->pos += 2; continue; }
                sc->pos++;
                break;
            }
            buf[o++] = ch;
            sc->pos++;
        }
        buf[o] = '\0';
        t->type = TT_STR;
        t->sval = buf;
        t->slen = o;
        t->len = sc->pos - st;
        return;
    }
    switch (c) {
        case '(': t->type = TT_LP; t->len = 1; sc->pos++; return;
        case ')': t->type = TT_RP; t->len = 1; sc->pos++; return;
        case ',': t->type = TT_COMMA; t->len = 1; sc->pos++; return;
        case '.': t->type = TT_DOT; t->len = 1; sc->pos++; return;
        case '*': t->type = TT_STAR; t->len = 1; sc->pos++; return;
        case ';': t->type = TT_EOF; sc->pos = sc->len; return;
        default: break;
    }
    /* 运算符：<= >= <> != = < > + - / */
    t->type = TT_SYM;
    if ((c == '<' || c == '>' || c == '!') && sc->pos + 1 < sc->len) {
        char n = sc->sql[sc->pos + 1];
        if (n == '=' || (c == '<' && n == '>') || (c == '!' && n == '=')) {
            t->len = 2; sc->pos += 2; return;
        }
    }
    t->len = 1;
    sc->pos++;
}

/* =========================================
 * 值表达式 / 条件
 * ========================================= */

typedef enum { EX_COL = 0, EX_INT, EX_DBL, EX_STR, EX_NULL, EX_SUBQ } exkind_t;

typedef struct {
    exkind_t kind;
    char     tbl[NAME_SZ];
    char     col[NAME_SZ];
    long long i;
    double    d;
    char*     s;
    int       slen;
    int       is_dbl;
    char*     sub_sql;   /* 可能相关子查询：保存原始文本，求值期按行替换外层列 */
} expr_t;

typedef enum { C_AND = 0, C_OR, C_NOT, C_CMP, C_IN, C_ISNULL, C_EXISTS } ckind_t;

typedef struct cond {
    ckind_t kind;
    struct cond *a, *b;
    int      op;         /* 0 '=' 1 '!=' 2 '<' 3 '<=' 4 '>' 5 '>=' */
    expr_t   l, r;
    expr_t*  list;
    int      nlist;
    int      neg;        /* NOT IN / IS NOT NULL */
    int      exists;     /* 预计算 EXISTS 结果 */
    char*    sub_sql;    /* 可能相关子查询：保存原始文本，求值期按行替换外层列 */
} cond_t;

static void expr_free(expr_t* e) { if (e) { free(e->s); free(e->sub_sql); } }
static void cond_free(cond_t* c) {
    if (!c) return;
    cond_free(c->a); cond_free(c->b);
    expr_free(&c->l); expr_free(&c->r);
    for (int i = 0; i < c->nlist; i++) expr_free(&c->list[i]);
    free(c->list);
    free(c->sub_sql);
    free(c);
}

/* =========================================
 * 查询结构
 * ========================================= */

typedef struct {
    int    is_star;
    char   star_tbl[NAME_SZ];   /* "" = 全部源 */
    expr_t e;
    char   alias[NAME_SZ];
} sel_t;

typedef struct {
    char           alias[NAME_SZ];
    nlsql_result_t* res;    /* 已装载的数据 */
    int            join;    /* 0 首表 1 内连接 2 左连接 */
    cond_t*        on;
    char           inner_sql[4096]; /* 派生表/视图内联 */
    int            has_inner;
} src_t;

typedef struct {
    int distinct;
    sel_t sel[MAX_SEL];
    int   nsel;
    src_t src[MAX_SRC];
    int   nsrc;
    cond_t* where;
    struct { expr_t e; int desc; } order[MAX_ORDER];
    int   norder;
    long long limit;
    int   has_limit;
    nlsql_conn_t* conn;      /* exec_query 赋值：求值期执行相关子查询需要 */
} query_t;

static void query_free(query_t* q) {
    for (int i = 0; i < q->nsel; i++) expr_free(&q->sel[i].e);
    for (int i = 0; i < q->nsrc; i++) {
        cond_free(q->src[i].on);
        if (q->src[i].res) nlsql_free_result(q->src[i].res);
    }
    cond_free(q->where);
    for (int i = 0; i < q->norder; i++) expr_free(&q->order[i].e);
}

/* =========================================
 * 解析器
 * ========================================= */

typedef struct {
    scanner_t sc;
    tok_t     cur, nx;
    nlsql_conn_t* conn;
    int       err;
    int       in_select;   /* 正在解析 SELECT 列表项：其中标量子查询一律按可能相关处理 */
} parser_t;

static void advance(parser_t* p) {
    tok_free(&p->cur);
    p->cur = p->nx;
    memset(&p->nx, 0, sizeof(p->nx));
    lex_next(&p->sc, &p->nx);
}

static void parser_init(parser_t* p, nlsql_conn_t* conn, const char* sql) {
    memset(p, 0, sizeof(*p));
    p->conn = conn;
    p->sc.sql = sql;
    p->sc.len = (int)strlen(sql);
    lex_next(&p->sc, &p->cur);
    lex_next(&p->sc, &p->nx);
}
static void parser_free(parser_t* p) {
    tok_free(&p->cur); tok_free(&p->nx);
}

static int is_kw(const tok_t* t, const char* w) {
    return t->type == TT_IDENT && t->sval && strcasecmp(t->sval, w) == 0;
}
static int is_sym(const tok_t* t, const char* s) {
    return t->type == TT_SYM && (int)strlen(s) == t->len && strncmp(t->start, s, (size_t)t->len) == 0;
}

/* 预看 '.' 之后的词类型（p->sc 已停在 nx 之后；nx 为 '.'） */
static tt_t lex_peek_after_dot(parser_t* p) {
    scanner_t sc = p->sc;
    tok_t t;
    memset(&t, 0, sizeof(t));
    lex_next(&sc, &t);
    tt_t ty = (tt_t)t.type;
    tok_free(&t);
    return ty;
}

/* 捕获以 SELECT 开头的子查询文本：调用时 cur 恰为 '('，nx 为 SELECT */
static char* capture_subquery(parser_t* p) {
    if (p->cur.type != TT_LP) return NULL;
    const char* qstart = p->nx.start;
    advance(p); /* cur = SELECT */
    int depth = 1;
    for (;;) {
        if (p->cur.type == TT_EOF) return NULL;
        if (p->cur.type == TT_LP) depth++;
        else if (p->cur.type == TT_RP) {
            depth--;
            if (depth == 0) {
                const char* qend = p->cur.start;
                size_t n = (size_t)(qend - qstart);
                char* out = (char*)malloc(n + 1);
                if (!out) return NULL;
                memcpy(out, qstart, n);
                out[n] = '\0';
                advance(p); /* 跳过 ')' */
                return out;
            }
        }
        advance(p);
    }
}

/* 执行一个子查询并返回结果（不相关，一次性求值） */
static nlsql_result_t* run_subquery(nlsql_conn_t* conn, const char* sql) {
    nlsql_result_t* r = NULL;
    int rc = nlsql_query(conn, sql, &r);
    if (rc != NLSQL_OK) { nlsql_free_result(r); return NULL; }
    return r;
}

/* 相关子查询启发式：子查询文本中出现「标识符.标识符」点号限定即视为可能相关。
 * 该判定偏保守（可能把引用子查询自身表的 a.id 也当成外层引用），这样只是多按行
 * 求值一次，结果仍正确；不相关的子查询不受影响，保持一次性求值的性能。 */
static int sub_maybe_correlated(const char* sql) {
    int inq = 0; char q = 0;
    for (const char* p = sql; *p; p++) {
        if (inq) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == q) inq = 0;
            continue;
        }
        if (*p == '\'' || *p == '"' || *p == '`') { inq = 1; q = *p; continue; }
        if (ident_start(*p)) {
            const char* e = p;
            while (*e && ident_char(*e)) e++;
            if (*e == '.' && ident_start(e[1])) {
                const char* e2 = e + 1;
                while (*e2 && ident_char(*e2)) e2++;
                if (e2 > e + 1) return 1;
            }
            p = e - 1;
        }
    }
    return 0;
}

static int parse_expr(parser_t* p, expr_t* e) {
    memset(e, 0, sizeof(*e));
    if (p->cur.type == TT_LP && (is_kw(&p->nx, "select"))) {
        char* sub = capture_subquery(p);
        if (!sub) return -1;
        /* SELECT 列表中的标量子查询：外层 FROM 尚未解析、别名未知，一律视为
         * 可能相关；其它位置按点号限定启发式判断。可能相关则保存文本，求值期
         * 逐行替换外层列后执行，不做解析期一次性求值。 */
        if (p->in_select || sub_maybe_correlated(sub)) {
            e->kind = EX_SUBQ;
            e->sub_sql = sub;
            return 0;
        }
        nlsql_result_t* r = run_subquery(p->conn, sub);
        free(sub);
        if (!r) return -1;
        e->kind = EX_SUBQ;
        if (r->row_count > 0 && r->col_count > 0) {
            nlsql_value_t* v = &r->rows[0].vals[0];
            if (!v->is_null) {
                if (v->type == NLSQL_TYPE_INT64) { e->i = v->i; }
                else if (v->type == NLSQL_TYPE_DOUBLE) { e->d = v->d; e->is_dbl = 1; }
                else { e->s = (char*)malloc((size_t)v->len + 1); if (e->s) { memcpy(e->s, v->s, v->len); e->s[v->len] = '\0'; } e->slen = (int)v->len; }
            }
        }
        nlsql_free_result(r);
        return 0;
    }
    if (p->cur.type == TT_SYM && (is_sym(&p->cur, "-"))) {
        advance(p);
        int isd = 0;
        if (p->cur.type == TT_NUM) {
            for (int i = 0; i < p->cur.len; i++) if (p->cur.start[i] == '.') isd = 1;
            if (isd) { e->kind = EX_DBL; e->d = -atof(p->cur.start); }
            else { e->kind = EX_INT; e->i = -p->cur.i; }
            advance(p);
            return 0;
        }
        return -1;
    }
    if (p->cur.type == TT_NUM) {
        /* 判断整数/浮点：源文本含 '.' */
        int isd = 0;
        for (int i = 0; i < p->cur.len; i++) if (p->cur.start[i] == '.') isd = 1;
        if (isd) { e->kind = EX_DBL; e->d = atof(p->cur.start); }
        else { e->kind = EX_INT; e->i = p->cur.i; }
        advance(p);
        return 0;
    }
    if (p->cur.type == TT_STR) {
        e->kind = EX_STR;
        e->s = p->cur.sval ? strdup(p->cur.sval) : NULL;
        e->slen = e->s ? (int)strlen(e->s) : 0;
        advance(p);
        return 0;
    }
    if (is_kw(&p->cur, "null")) {
        e->kind = EX_NULL;
        advance(p);
        return 0;
    }
    if (p->cur.type == TT_IDENT) {
        char tbl[NAME_SZ] = "", col[NAME_SZ] = "";
        snprintf(col, sizeof(col), "%s", p->cur.sval ? p->cur.sval : "");
        advance(p);
        if (p->cur.type == TT_DOT) {
            advance(p);
            if (p->cur.type != TT_IDENT) return -1;
            snprintf(tbl, sizeof(tbl), "%s", col);
            snprintf(col, sizeof(col), "%s", p->cur.sval ? p->cur.sval : "");
            advance(p);
        }
        e->kind = EX_COL;
        snprintf(e->tbl, sizeof(e->tbl), "%s", tbl);
        snprintf(e->col, sizeof(e->col), "%s", col);
        return 0;
    }
    return -1;
}

static int parse_cond(parser_t* p, cond_t** out);

static int parse_predicate(parser_t* p, cond_t** out) {
    *out = NULL;
    if (is_kw(&p->cur, "not")) {
        advance(p);
        cond_t* sub = NULL;
        if (parse_predicate(p, &sub) != 0) return -1;
        cond_t* c = (cond_t*)calloc(1, sizeof(*c));
        if (!c) { cond_free(sub); return -1; }
        c->kind = C_NOT; c->a = sub;
        *out = c;
        return 0;
    }
    if (p->cur.type == TT_LP && !is_kw(&p->nx, "select")) {
        /* 括号分组条件 */
        advance(p);
        if (parse_cond(p, out) != 0) return -1;
        if (p->cur.type != TT_RP) return -1;
        advance(p);
        return 0;
    }
    if (is_kw(&p->cur, "exists")) {
        advance(p);
        if (p->cur.type != TT_LP) return -1;
        char* sub = capture_subquery(p);
        if (!sub) return -1;
        cond_t* c = (cond_t*)calloc(1, sizeof(*c));
        if (!c) { free(sub); return -1; }
        c->kind = C_EXISTS;
        if (sub_maybe_correlated(sub)) {
            /* 可能引用外层行：保存文本，求值期逐行执行 */
            c->sub_sql = sub;
        } else {
            nlsql_result_t* r = run_subquery(p->conn, sub);
            free(sub);
            c->exists = (r && r->row_count > 0) ? 1 : 0;
            nlsql_free_result(r);
        }
        *out = c;
        return 0;
    }

    cond_t* c = (cond_t*)calloc(1, sizeof(*c));
    if (!c) return -1;
    if (parse_expr(p, &c->l) != 0) { cond_free(c); return -1; }

    if (is_kw(&p->cur, "is")) {
        advance(p);
        int neg = 0;
        if (is_kw(&p->cur, "not")) { neg = 1; advance(p); }
        if (!is_kw(&p->cur, "null")) { cond_free(c); return -1; }
        advance(p);
        c->kind = C_ISNULL; c->neg = neg;
        *out = c;
        return 0;
    }
    if (is_kw(&p->cur, "not") || is_kw(&p->cur, "in")) {
        int neg = 0;
        if (is_kw(&p->cur, "not")) { neg = 1; advance(p); }
        if (!is_kw(&p->cur, "in")) { cond_free(c); return -1; }
        advance(p);
        if (p->cur.type != TT_LP) { cond_free(c); return -1; }
        c->kind = C_IN; c->neg = neg;
        if (is_kw(&p->nx, "select")) {
            char* sub = capture_subquery(p);
            if (!sub) { cond_free(c); return -1; }
            if (sub_maybe_correlated(sub)) {
                /* 可能相关：保存文本，求值期逐行替换外层列后执行 */
                c->sub_sql = sub;
            } else {
                nlsql_result_t* r = run_subquery(p->conn, sub);
                free(sub);
                if (!r) { cond_free(c); return -1; }
                int cap = (int)r->row_count;
                if (cap > 0) {
                    c->list = (expr_t*)calloc((size_t)cap, sizeof(expr_t));
                    if (!c->list) { nlsql_free_result(r); cond_free(c); return -1; }
                    for (int i = 0; i < cap; i++) {
                        expr_t* e = &c->list[c->nlist++];
                        nlsql_value_t* v = &r->rows[i].vals[0];
                        if (v->is_null) { e->kind = EX_NULL; }
                        else if (v->type == NLSQL_TYPE_INT64) { e->kind = EX_INT; e->i = v->i; }
                        else if (v->type == NLSQL_TYPE_DOUBLE) { e->kind = EX_DBL; e->d = v->d; }
                        else { e->kind = EX_STR; e->s = (char*)malloc((size_t)v->len + 1); if (e->s) { memcpy(e->s, v->s, v->len); e->s[v->len] = '\0'; } e->slen = (int)v->len; }
                    }
                }
                nlsql_free_result(r);
            }
        } else {
            /* IN (值列表) */
            advance(p); /* 跳过 '(' */
            for (;;) {
                if (c->nlist >= MAX_LIST) { cond_free(c); return -1; }
                c->list = (expr_t*)realloc(c->list, sizeof(expr_t) * (size_t)(c->nlist + 1));
                if (!c->list) { cond_free(c); return -1; }
                expr_t* e = &c->list[c->nlist];
                if (parse_expr(p, e) != 0) { cond_free(c); return -1; }
                c->nlist++;
                if (p->cur.type == TT_COMMA) { advance(p); continue; }
                break;
            }
            if (p->cur.type != TT_RP) { cond_free(c); return -1; }
            advance(p);
        }
        *out = c;
        return 0;
    }

    /* 比较运算符 */
    int op = -1;
    if (p->cur.type == TT_SYM) {
        if (is_sym(&p->cur, "=")) op = 0;
        else if (is_sym(&p->cur, "!=") || is_sym(&p->cur, "<>")) op = 1;
        else if (is_sym(&p->cur, "<")) op = 2;
        else if (is_sym(&p->cur, "<=")) op = 3;
        else if (is_sym(&p->cur, ">")) op = 4;
        else if (is_sym(&p->cur, ">=")) op = 5;
    }
    if (op < 0) { cond_free(c); return -1; }
    advance(p);
    if (parse_expr(p, &c->r) != 0) { cond_free(c); return -1; }
    c->kind = C_CMP; c->op = op;
    *out = c;
    return 0;
}

static int parse_and(parser_t* p, cond_t** out) {
    cond_t* l = NULL;
    if (parse_predicate(p, &l) != 0) return -1;
    while (is_kw(&p->cur, "and")) {
        advance(p);
        cond_t* r = NULL;
        if (parse_predicate(p, &r) != 0) { cond_free(l); return -1; }
        cond_t* c = (cond_t*)calloc(1, sizeof(*c));
        if (!c) { cond_free(l); cond_free(r); return -1; }
        c->kind = C_AND; c->a = l; c->b = r; l = c;
    }
    *out = l;
    return 0;
}

static int parse_cond(parser_t* p, cond_t** out) {
    cond_t* l = NULL;
    if (parse_and(p, &l) != 0) return -1;
    while (is_kw(&p->cur, "or")) {
        advance(p);
        cond_t* r = NULL;
        if (parse_and(p, &r) != 0) { cond_free(l); return -1; }
        cond_t* c = (cond_t*)calloc(1, sizeof(*c));
        if (!c) { cond_free(l); cond_free(r); return -1; }
        c->kind = C_OR; c->a = l; c->b = r; l = c;
    }
    *out = l;
    return 0;
}

/* 解析一个 FROM 源 */
static int parse_source(parser_t* p, src_t* s) {
    memset(s, 0, sizeof(*s));
    if (p->cur.type == TT_LP && is_kw(&p->nx, "select")) {
        char* sub = capture_subquery(p);
        if (!sub) return -1;
        snprintf(s->inner_sql, sizeof(s->inner_sql), "%s", sub);
        free(sub);
        s->has_inner = 1;
    } else if (p->cur.type == TT_IDENT) {
        snprintf(s->alias, sizeof(s->alias), "%s", p->cur.sval ? p->cur.sval : "");
        advance(p);
    } else {
        return -1;
    }
    if (is_kw(&p->cur, "as")) { advance(p); }
    if (p->cur.type == TT_IDENT && !is_kw(&p->cur, "where") && !is_kw(&p->cur, "join") &&
        !is_kw(&p->cur, "inner") && !is_kw(&p->cur, "left") && !is_kw(&p->cur, "right") &&
        !is_kw(&p->cur, "on") && !is_kw(&p->cur, "order") && !is_kw(&p->cur, "limit") &&
        !is_kw(&p->cur, "group")) {
        snprintf(s->alias, sizeof(s->alias), "%s", p->cur.sval ? p->cur.sval : "");
        advance(p);
    }
    return 0;
}

static int parse_select(parser_t* p, query_t* q) {
    memset(q, 0, sizeof(*q));
    if (!is_kw(&p->cur, "select")) return -1;
    advance(p);
    if (is_kw(&p->cur, "distinct")) { q->distinct = 1; advance(p); }
    for (;;) {
        if (q->nsel >= MAX_SEL) return -1;
        sel_t* s = &q->sel[q->nsel];
        memset(s, 0, sizeof(*s));
        if (p->cur.type == TT_STAR) {
            s->is_star = 1;
            advance(p);
        } else if (p->cur.type == TT_IDENT && p->nx.type == TT_DOT &&
                   (lex_peek_after_dot(p) == TT_STAR)) {
            /* alias.* */
            snprintf(s->star_tbl, sizeof(s->star_tbl), "%s", p->cur.sval ? p->cur.sval : "");
            advance(p); /* alias */
            advance(p); /* . */
            advance(p); /* * */
            s->is_star = 1;
        } else {
            p->in_select = 1;   /* SELECT 列表中的标量子查询一律按可能相关处理 */
            int erc = parse_expr(p, &s->e);
            p->in_select = 0;
            if (erc != 0) return -1;
            if (is_kw(&p->cur, "as")) advance(p);
            if (p->cur.type == TT_IDENT && !is_kw(&p->cur, "from") && !is_kw(&p->cur, "where") &&
                !is_kw(&p->cur, "order") && !is_kw(&p->cur, "limit") && !is_kw(&p->cur, "group")) {
                snprintf(s->alias, sizeof(s->alias), "%s", p->cur.sval ? p->cur.sval : "");
                advance(p);
            }
        }
        q->nsel++;
        if (p->cur.type == TT_COMMA) { advance(p); continue; }
        break;
    }
    if (!is_kw(&p->cur, "from")) return -1;
    advance(p);

    if (parse_source(p, &q->src[0]) != 0) return -1;
    q->nsrc = 1;
    for (;;) {
        int join = 0;
        if (is_kw(&p->cur, "inner")) { advance(p); if (!is_kw(&p->cur, "join")) return -1; join = 1; }
        else if (is_kw(&p->cur, "left")) { advance(p); if (is_kw(&p->cur, "outer")) advance(p); if (!is_kw(&p->cur, "join")) return -1; join = 2; }
        else if (is_kw(&p->cur, "right")) { advance(p); if (is_kw(&p->cur, "outer")) advance(p); if (!is_kw(&p->cur, "join")) return -1; join = 1; }
        else if (is_kw(&p->cur, "join")) { join = 1; }
        else break;
        if (join && !is_kw(&p->cur, "join")) { /* 已消费 join 关键字 */ }
        advance(p); /* 跳过 JOIN */
        if (q->nsrc >= MAX_SRC) return -1;
        if (parse_source(p, &q->src[q->nsrc]) != 0) return -1;
        q->src[q->nsrc].join = join;
        if (is_kw(&p->cur, "on")) {
            advance(p);
            if (parse_cond(p, &q->src[q->nsrc].on) != 0) return -1;
        } else if (join == 2) {
            return -1; /* LEFT JOIN 需要 ON（简化） */
        }
        q->nsrc++;
    }

    if (is_kw(&p->cur, "where")) {
        advance(p);
        if (parse_cond(p, &q->where) != 0) return -1;
    }
    if (is_kw(&p->cur, "order")) {
        advance(p);
        if (!is_kw(&p->cur, "by")) return -1;
        advance(p);
        for (;;) {
            if (q->norder >= MAX_ORDER) return -1;
            if (parse_expr(p, &q->order[q->norder].e) != 0) return -1;
            if (is_kw(&p->cur, "desc")) { q->order[q->norder].desc = 1; advance(p); }
            else if (is_kw(&p->cur, "asc")) { advance(p); }
            q->norder++;
            if (p->cur.type == TT_COMMA) { advance(p); continue; }
            break;
        }
    }
    if (is_kw(&p->cur, "limit")) {
        advance(p);
        if (p->cur.type != TT_NUM) return -1;
        q->limit = p->cur.i;
        q->has_limit = 1;
        advance(p);
    }
    return 0;
}

/* =========================================
 * 求值与执行
 * ========================================= */

typedef struct {
    int          is_null;
    int          is_int;
    long long    i;
    int          is_dbl;
    double       d;
    const char*  s;
    int          slen;
} pv_t;

static pv_t pv_from_value(const nlsql_value_t* v) {
    pv_t r; memset(&r, 0, sizeof(r));
    if (!v || v->is_null || v->type == NLSQL_TYPE_NULL) { r.is_null = 1; return r; }
    if (v->type == NLSQL_TYPE_INT64) { r.is_int = 1; r.i = v->i; return r; }
    if (v->type == NLSQL_TYPE_DOUBLE) { r.is_dbl = 1; r.d = v->d; return r; }
    r.s = v->s ? v->s : "";
    r.slen = (int)v->len;
    return r;
}

static int pv_num(const pv_t* v, double* out) {
    if (v->is_int) { *out = (double)v->i; return 1; }
    if (v->is_dbl) { *out = v->d; return 1; }
    if (v->s) {
        char* end = NULL;
        double d = strtod(v->s, &end);
        if (end != v->s && *end == '\0') { *out = d; return 1; }
    }
    return 0;
}

static int pv_cmp(const pv_t* a, const pv_t* b) {
    if (a->is_null || b->is_null) return 2; /* 不可比较 */
    if (a->is_int || a->is_dbl || b->is_int || b->is_dbl) {
        double x, y;
        if (pv_num(a, &x) && pv_num(b, &y)) return x < y ? -1 : (x > y ? 1 : 0);
    }
    char ta[32], tb[32];
    int la = a->slen, lb = b->slen;
    const char* sa = a->s;
    const char* sb = b->s;
    if (!sa && a->is_int) { snprintf(ta, sizeof(ta), "%lld", a->i); sa = ta; la = (int)strlen(ta); }
    if (!sb && b->is_int) { snprintf(tb, sizeof(tb), "%lld", b->i); sb = tb; lb = (int)strlen(tb); }
    if (!sa) sa = "";
    if (!sb) sb = "";
    int m = la < lb ? la : lb;
    int c = memcmp(sa, sb, (size_t)m);
    if (c != 0) return c < 0 ? -1 : 1;
    return la < lb ? -1 : (la > lb ? 1 : 0);
}

/* 列查找：返回源下标与行内列下标 */
static int find_col(query_t* q, const int* cur, const char* tbl, const char* col, int* src_i, int* col_i) {
    for (int i = 0; i < q->nsrc; i++) {
        if (tbl[0] && strcasecmp(q->src[i].alias, tbl) != 0) continue;
        nlsql_result_t* r = q->src[i].res;
        if (!r) continue;
        for (int j = 0; j < r->col_count; j++)
            if (strcasecmp(r->cols[j].name, col) == 0) {
                if (cur[i] < 0) { *src_i = i; *col_i = j; return 0; }
                *src_i = i; *col_i = j;
                return 0;
            }
    }
    return -1;
}

/* ---------- 相关子查询：外层行值替换 ---------- */

/* 把值渲染为 SQL 字面量（字符串做单引号转义；NULL 输出 NULL） */
static int append_literal(char* out, size_t cap, size_t* len, const pv_t* v) {
    char tmp[64];
    const char* src;
    size_t n;
    if (v->is_null) { src = "NULL"; n = 4; }
    else if (v->is_int) { snprintf(tmp, sizeof(tmp), "%lld", v->i); src = tmp; n = strlen(tmp); }
    else if (v->is_dbl) { snprintf(tmp, sizeof(tmp), "%.17g", v->d); src = tmp; n = strlen(tmp); }
    else {
        /* 字符串：'...'，转义反斜杠与单引号 */
        size_t need = (size_t)(v->slen) * 2 + 3;
        char* lit = (char*)malloc(need);
        if (!lit) return -1;
        size_t k = 0;
        lit[k++] = '\'';
        const char* s = v->s ? v->s : "";
        for (int i = 0; i < v->slen; i++) {
            if (s[i] == '\'' || s[i] == '\\') lit[k++] = '\\';
            lit[k++] = s[i];
        }
        lit[k++] = '\'';
        int ok = 0;
        if (*len + k < cap) { memcpy(out + *len, lit, k); *len += k; ok = 1; }
        free(lit);
        return ok ? 0 : -1;
    }
    if (*len + n >= cap) return -1;
    memcpy(out + *len, src, n);
    *len += n;
    return 0;
}

/* 将子查询文本中「外层表别名.列」替换为当前外层行的字面量值。
 * 只替换能匹配外层源别名的限定列；未限定列与子查询自身表列保持原样。 */
static char* substitute_outer(query_t* q, const int* cur, const char* sql) {
    size_t cap = strlen(sql) * 4 + 256, len = 0;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    int inq = 0; char qc = 0;
    for (const char* p = sql; *p; ) {
        char ch = *p;
        if (inq) {
            if (ch == '\\' && p[1] && len + 2 < cap) { out[len++] = ch; out[len++] = p[1]; p += 2; continue; }
            if (len + 1 < cap) out[len++] = ch;
            if (ch == qc) inq = 0;
            p++;
            continue;
        }
        if (ch == '\'' || ch == '"' || ch == '`') { inq = 1; qc = ch; if (len + 1 < cap) out[len++] = ch; p++; continue; }
        if (ident_start(ch)) {
            const char* e1 = p;
            while (*e1 && ident_char(*e1)) e1++;
            if (*e1 == '.' && ident_start(e1[1])) {
                const char* e2 = e1 + 1;
                while (*e2 && ident_char(*e2)) e2++;
                char tbl[NAME_SZ], col[NAME_SZ];
                size_t t1 = (size_t)(e1 - p), t2 = (size_t)(e2 - (e1 + 1));
                int is_outer = 0, si = -1, ci = -1;
                if (t1 < sizeof(tbl) && t2 < sizeof(col)) {
                    memcpy(tbl, p, t1); tbl[t1] = '\0';
                    memcpy(col, e1 + 1, t2); col[t2] = '\0';
                    for (int i = 0; i < q->nsrc; i++)
                        if (strcasecmp(q->src[i].alias, tbl) == 0) { is_outer = 1; break; }
                }
                if (is_outer && find_col(q, cur, tbl, col, &si, &ci) == 0 &&
                    cur[si] >= 0 && q->src[si].res) {
                    pv_t v = pv_from_value(&q->src[si].res->rows[cur[si]].vals[ci]);
                    if (append_literal(out, cap, &len, &v) != 0) { free(out); return NULL; }
                    p = e2;
                    continue;
                }
            }
            if (len + (size_t)(e1 - p) < cap) { memcpy(out + len, p, (size_t)(e1 - p)); len += (size_t)(e1 - p); }
            else { free(out); return NULL; }
            p = e1;
            continue;
        }
        if (len + 1 < cap) out[len++] = ch;
        else { free(out); return NULL; }
        p++;
    }
    out[len] = '\0';
    return out;
}

/* 按当前外层行求值一个「可能相关」子查询；返回结果集（调用方释放） */
static nlsql_result_t* run_subquery_corr(query_t* q, const int* cur, const char* sub_sql) {
    char* sql = substitute_outer(q, cur, sub_sql);
    if (!sql) return NULL;
    nlsql_result_t* r = run_subquery(q->conn, sql);
    free(sql);
    return r;
}

static pv_t eval_expr(query_t* q, const int* cur, const expr_t* e) {
    static _Thread_local char t_sub_str[512];
    pv_t r; memset(&r, 0, sizeof(r));
    switch (e->kind) {
        case EX_INT: r.is_int = 1; r.i = e->i; break;
        case EX_DBL: r.is_dbl = 1; r.d = e->d; break;
        case EX_STR: r.s = e->s; r.slen = e->slen; break;
        case EX_NULL: r.is_null = 1; break;
        case EX_SUBQ:
            if (e->sub_sql) {
                /* 可能相关：按当前外层行替换后求值，取首行首列 */
                nlsql_result_t* rs = run_subquery_corr(q, cur, e->sub_sql);
                if (!rs) { r.is_null = 1; break; }
                if (rs->row_count > 0 && rs->col_count > 0) {
                    r = pv_from_value(&rs->rows[0].vals[0]);
                    if (r.s) {
                        /* 结果集即将释放：字符串拷到线程局部缓冲，保证返回指针有效 */
                        size_t n = (size_t)r.slen;
                        if (n > sizeof(t_sub_str) - 1) n = sizeof(t_sub_str) - 1;
                        memcpy(t_sub_str, r.s, n);
                        t_sub_str[n] = '\0';
                        r.s = t_sub_str;
                        r.slen = (int)n;
                    }
                } else r.is_null = 1;
                nlsql_free_result(rs);
                break;
            }
            if (e->s) { r.s = e->s; r.slen = e->slen; }
            else if (e->is_dbl) { r.is_dbl = 1; r.d = e->d; }
            else { r.is_int = 1; r.i = e->i; }
            break;
        case EX_COL: {
            int si, ci;
            if (find_col(q, cur, e->tbl, e->col, &si, &ci) != 0) { r.is_null = 1; break; }
            if (cur[si] < 0) { r.is_null = 1; break; }
            nlsql_value_t* v = &q->src[si].res->rows[cur[si]].vals[ci];
            return pv_from_value(v);
        }
    }
    return r;
}

static int eval_cond(query_t* q, const int* cur, cond_t* c) {
    if (!c) return 1;
    switch (c->kind) {
        case C_AND: return eval_cond(q, cur, c->a) && eval_cond(q, cur, c->b);
        case C_OR:  return eval_cond(q, cur, c->a) || eval_cond(q, cur, c->b);
        case C_NOT: return !eval_cond(q, cur, c->a);
        case C_EXISTS:
            if (c->sub_sql) {
                /* 可能相关：按当前外层行替换后执行，行数 > 0 即为真 */
                nlsql_result_t* rs = run_subquery_corr(q, cur, c->sub_sql);
                int hit = (rs && rs->row_count > 0) ? 1 : 0;
                nlsql_free_result(rs);
                return hit;
            }
            return c->exists;
        case C_ISNULL: {
            pv_t v = eval_expr(q, cur, &c->l);
            return c->neg ? !v.is_null : v.is_null;
        }
        case C_IN: {
            pv_t v = eval_expr(q, cur, &c->l);
            if (v.is_null) return 0;
            if (c->sub_sql) {
                /* 可能相关：按当前外层行替换后执行，逐结果行比较 */
                nlsql_result_t* rs = run_subquery_corr(q, cur, c->sub_sql);
                int hit = 0;
                if (rs)
                    for (uint64_t i = 0; i < rs->row_count && rs->col_count > 0 && !hit; i++) {
                        pv_t x = pv_from_value(&rs->rows[i].vals[0]);
                        if (!x.is_null && pv_cmp(&v, &x) == 0) hit = 1;
                    }
                nlsql_free_result(rs);
                return c->neg ? !hit : hit;
            }
            int hit = 0;
            for (int i = 0; i < c->nlist; i++) {
                pv_t x = eval_expr(q, cur, &c->list[i]);
                if (!x.is_null && pv_cmp(&v, &x) == 0) { hit = 1; break; }
            }
            return c->neg ? !hit : hit;
        }
        case C_CMP: {
            pv_t a = eval_expr(q, cur, &c->l);
            pv_t b = eval_expr(q, cur, &c->r);
            int cm = pv_cmp(&a, &b);
            if (cm == 2) return 0;
            switch (c->op) {
                case 0: return cm == 0;
                case 1: return cm != 0;
                case 2: return cm < 0;
                case 3: return cm <= 0;
                case 4: return cm > 0;
                case 5: return cm >= 0;
            }
            return 0;
        }
    }
    return 0;
}

/* 组合收集 */
typedef struct { int idx[MAX_SRC]; } combo_t;

typedef struct {
    query_t* q;
    combo_t* v;
    size_t   n, cap;
} combo_vec_t;

static int combo_push(combo_vec_t* cv, const int* cur) {
    if (cv->n >= cv->cap) {
        size_t nc = cv->cap ? cv->cap * 2 : 64;
        combo_t* nv = (combo_t*)realloc(cv->v, nc * sizeof(combo_t));
        if (!nv) return -1;
        cv->v = nv; cv->cap = nc;
    }
    memcpy(cv->v[cv->n].idx, cur, sizeof(int) * (size_t)cv->q->nsrc);
    cv->n++;
    return 0;
}

static void join_walk(query_t* q, int level, int* cur, combo_vec_t* cv) {
    if (level == q->nsrc) {
        if (eval_cond(q, cur, q->where)) combo_push(cv, cur);
        return;
    }
    src_t* s = &q->src[level];
    if (!s->res) return;
    int matched = 0;
    for (uint64_t r = 0; r < s->res->row_count; r++) {
        cur[level] = (int)r;
        if (s->join == 0 || (s->on && eval_cond(q, cur, s->on))) {
            matched = 1;
            join_walk(q, level + 1, cur, cv);
        }
    }
    if (!matched && s->join == 2) {
        cur[level] = -1;
        join_walk(q, level + 1, cur, cv);
    }
}

/* 排序上下文（线程局部） */
static _Thread_local query_t* t_sort_q;

static int sort_cmp(const void* pa, const void* pb) {
    const combo_t* a = (const combo_t*)pa;
    const combo_t* b = (const combo_t*)pb;
    for (int i = 0; i < t_sort_q->norder; i++) {
        pv_t x = eval_expr(t_sort_q, a->idx, &t_sort_q->order[i].e);
        pv_t y = eval_expr(t_sort_q, b->idx, &t_sort_q->order[i].e);
        int c = pv_cmp(&x, &y);
        if (c == 2) continue;
        if (c != 0) return t_sort_q->order[i].desc ? -c : c;
    }
    return 0;
}

/* =========================================
 * 结果集构造（与核心 arena 布局兼容，交给 nlsql_free_result 释放）
 * ========================================= */

typedef struct ext_arena_blk {
    struct ext_arena_blk* next;
    uint32_t used, cap;
    char data[1];
} ext_arena_blk_t;

static nlsql_result_t* make_result(int ncols, uint64_t nrows) {
    nlsql_result_t* r = (nlsql_result_t*)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->col_count = ncols;
    r->cols = (nlsql_column_t*)calloc((size_t)ncols, sizeof(nlsql_column_t));
    if (!r->cols) { free(r); return NULL; }
    if (nrows == 0) return r;
    r->rows = (nlsql_row_t*)calloc((size_t)nrows, sizeof(nlsql_row_t));
    if (!r->rows) { free(r->cols); free(r); return NULL; }
    size_t need = (size_t)nrows * (size_t)ncols * sizeof(nlsql_value_t);
    ext_arena_blk_t* blk = (ext_arena_blk_t*)malloc(sizeof(ext_arena_blk_t) + need + 8);
    if (!blk) { free(r->rows); free(r->cols); free(r); return NULL; }
    blk->next = NULL; blk->used = (uint32_t)need; blk->cap = (uint32_t)(need + 8);
    r->_arena = blk;
    char* base = blk->data;
    for (uint64_t i = 0; i < nrows; i++)
        r->rows[i].vals = (nlsql_value_t*)(base + i * (size_t)ncols * sizeof(nlsql_value_t));
    r->row_count = nrows;
    return r;
}

static void set_val(nlsql_value_t* dst, const pv_t* v) {
    memset(dst, 0, sizeof(*dst));
    if (v->is_null) { dst->is_null = 1; dst->type = NLSQL_TYPE_NULL; return; }
    if (v->is_int) { dst->type = NLSQL_TYPE_INT64; dst->i = v->i; }
    else if (v->is_dbl) { dst->type = NLSQL_TYPE_DOUBLE; dst->d = v->d; }
    else {
        dst->type = NLSQL_TYPE_VARCHAR;
        dst->len = (uint32_t)v->slen;
        dst->s = (char*)malloc((size_t)v->slen + 1);
        if (dst->s) { memcpy(dst->s, v->s ? v->s : "", (size_t)v->slen); dst->s[v->slen] = '\0'; }
        else { dst->is_null = 1; dst->type = NLSQL_TYPE_NULL; }
    }
}

/* 输出列描述 */
typedef struct { int is_star; int src_idx; int col_idx; expr_t e; char name[NAME_SZ]; } outcol_t;

/* =========================================
 * 视图 / 触发器注册表
 * ========================================= */

typedef struct { char name[NAME_SZ]; char sql[4096]; int used; } view_t;
typedef struct { char name[NAME_SZ]; char table[NAME_SZ]; int event; /*1 insert 2 update 3 delete*/ char body[4096]; int used; } trig_t;

static pthread_mutex_t g_reg_mtx = PTHREAD_MUTEX_INITIALIZER;
static view_t g_views[MAX_VIEWS];
static trig_t g_trigs[MAX_TRIGS];

static const char* view_lookup(const char* name) {
    const char* r = NULL;
    pthread_mutex_lock(&g_reg_mtx);
    for (int i = 0; i < MAX_VIEWS; i++)
        if (g_views[i].used && strcasecmp(g_views[i].name, name) == 0) { r = g_views[i].sql; break; }
    pthread_mutex_unlock(&g_reg_mtx);
    return r;
}

static int view_add(const char* name, const char* sql) {
    pthread_mutex_lock(&g_reg_mtx);
    int slot = -1, found = -1;
    for (int i = 0; i < MAX_VIEWS; i++) {
        if (g_views[i].used && strcasecmp(g_views[i].name, name) == 0) { found = i; break; }
        if (!g_views[i].used && slot < 0) slot = i;
    }
    int idx = found >= 0 ? found : slot;
    if (idx < 0) { pthread_mutex_unlock(&g_reg_mtx); return -1; }
    snprintf(g_views[idx].name, sizeof(g_views[idx].name), "%s", name);
    snprintf(g_views[idx].sql, sizeof(g_views[idx].sql), "%s", sql);
    g_views[idx].used = 1;
    pthread_mutex_unlock(&g_reg_mtx);
    return 0;
}

static int view_drop(const char* name) {
    int ok = -1;
    pthread_mutex_lock(&g_reg_mtx);
    for (int i = 0; i < MAX_VIEWS; i++)
        if (g_views[i].used && strcasecmp(g_views[i].name, name) == 0) { g_views[i].used = 0; ok = 0; break; }
    pthread_mutex_unlock(&g_reg_mtx);
    return ok;
}

static int trig_add(const trig_t* t) {
    pthread_mutex_lock(&g_reg_mtx);
    int slot = -1, found = -1;
    for (int i = 0; i < MAX_TRIGS; i++) {
        if (g_trigs[i].used && strcasecmp(g_trigs[i].name, t->name) == 0) { found = i; break; }
        if (!g_trigs[i].used && slot < 0) slot = i;
    }
    int idx = found >= 0 ? found : slot;
    if (idx < 0) { pthread_mutex_unlock(&g_reg_mtx); return -1; }
    g_trigs[idx] = *t;
    g_trigs[idx].used = 1;
    pthread_mutex_unlock(&g_reg_mtx);
    return 0;
}

static int trig_drop(const char* name) {
    int ok = -1;
    pthread_mutex_lock(&g_reg_mtx);
    for (int i = 0; i < MAX_TRIGS; i++)
        if (g_trigs[i].used && strcasecmp(g_trigs[i].name, name) == 0) { g_trigs[i].used = 0; ok = 0; break; }
    pthread_mutex_unlock(&g_reg_mtx);
    return ok;
}

/* 收集指定表+事件的触发器体（拷贝，避免持锁执行） */
static int trig_collect(const char* table, int event, char bodies[][4096], int max) {
    int n = 0;
    pthread_mutex_lock(&g_reg_mtx);
    for (int i = 0; i < MAX_TRIGS && n < max; i++)
        if (g_trigs[i].used && strcasecmp(g_trigs[i].table, table) == 0 && g_trigs[i].event == event) {
            snprintf(bodies[n], 4096, "%s", g_trigs[i].body);
            n++;
        }
    pthread_mutex_unlock(&g_reg_mtx);
    return n;
}

/* =========================================
 * 元数据持久化（每库 sidecar，按连接+库懒加载重建）
 * ========================================= */

static _Thread_local int t_sql_busy;
static uint64_t g_loaded_conn_id;
static char     g_loaded_db[65];

static int sql_restore_cb(const char* kind, const char* name, const char* target,
                          const char* def, void* ud) {
    (void)ud;
    if (strcmp(kind, "view") == 0) {
        view_add(name, def);
    } else if (strcmp(kind, "trigger") == 0) {
        trig_t t;
        memset(&t, 0, sizeof(t));
        snprintf(t.name, sizeof(t.name), "%s", name);
        snprintf(t.table, sizeof(t.table), "%s", target);
        int ev = 0;
        const char* bar = strchr(def, '|');
        if (bar) {
            ev = atoi(def);
            snprintf(t.body, sizeof(t.body), "%s", bar + 1);
        }
        if (ev >= 1 && ev <= 3) {
            t.event = ev;
            trig_add(&t);
        }
    }
    return 0;
}

/* 切库/重开连接后从 sidecar 重建本库的视图与触发器（key 为连接 ID + 库名） */
static void sql_sync_meta(nlsql_conn_t* conn) {
    if (t_sql_busy) return;
    const char* db = nlsql_ext_conn_db(conn);
    if (!db[0]) return;
    uint64_t cid = nlsql_ext_conn_id(conn);
    if (cid == g_loaded_conn_id && strcmp(db, g_loaded_db) == 0) return;
    g_loaded_conn_id = cid;
    snprintf(g_loaded_db, sizeof(g_loaded_db), "%s", db);
    t_sql_busy = 1;
    nlsql_ext_meta_each(conn, sql_restore_cb, conn);
    t_sql_busy = 0;
}

/* =========================================
 * 装载源
 * ========================================= */

static int load_source(nlsql_conn_t* conn, src_t* s) {
    char sql[4096];
    if (s->has_inner) {
        snprintf(sql, sizeof(sql), "%s", s->inner_sql);
    } else {
        const char* vsql = view_lookup(s->alias);
        if (vsql) snprintf(sql, sizeof(sql), "%s", vsql);
        else snprintf(sql, sizeof(sql), "SELECT * FROM %s", s->alias);
    }
    nlsql_result_t* r = NULL;
    int rc = nlsql_query(conn, sql, &r);
    if (rc != NLSQL_OK) { nlsql_free_result(r); return -1; }
    s->res = r;
    return 0;
}

/* =========================================
 * 执行 SELECT
 * ========================================= */

static int exec_query(nlsql_conn_t* conn, query_t* q, nlsql_result_t** out) {
    *out = NULL;
    q->conn = conn; /* 相关子查询求值期需要连接 */
    for (int i = 0; i < q->nsrc; i++)
        if (load_source(conn, &q->src[i]) != 0) return NLSQL_ERR;

    combo_vec_t cv; memset(&cv, 0, sizeof(cv)); cv.q = q;
    int cur[MAX_SRC]; memset(cur, 0, sizeof(cur));
    join_walk(q, 0, cur, &cv);

    if (q->norder > 0 && cv.n > 1) {
        t_sort_q = q;
        qsort(cv.v, cv.n, sizeof(combo_t), sort_cmp);
        t_sort_q = NULL;
    }

    /* 展开输出列 */
    outcol_t oc[MAX_SRC * 32];
    int noc = 0;
    for (int i = 0; i < q->nsel; i++) {
        sel_t* s = &q->sel[i];
        if (s->is_star) {
            for (int k = 0; k < q->nsrc; k++) {
                if (s->star_tbl[0] && strcasecmp(q->src[k].alias, s->star_tbl) != 0) continue;
                nlsql_result_t* r = q->src[k].res;
                if (!r) continue;
                for (int j = 0; j < r->col_count && noc < (int)(sizeof(oc)/sizeof(oc[0])); j++) {
                    oc[noc].is_star = 0; oc[noc].src_idx = k; oc[noc].col_idx = j;
                    memset(&oc[noc].e, 0, sizeof(expr_t));
                    oc[noc].e.kind = EX_COL;
                    snprintf(oc[noc].e.tbl, NAME_SZ, "%s", q->src[k].alias);
                    snprintf(oc[noc].e.col, NAME_SZ, "%s", r->cols[j].name);
                    snprintf(oc[noc].name, NAME_SZ, "%s", r->cols[j].name);
                    noc++;
                }
            }
        } else {
            if (noc >= (int)(sizeof(oc)/sizeof(oc[0]))) break;
            oc[noc].is_star = 0; oc[noc].src_idx = -1; oc[noc].col_idx = -1;
            oc[noc].e = s->e; /* 浅拷贝（字符串指针共享，查询释放时统一释放原 e） */
            if (s->alias[0]) snprintf(oc[noc].name, NAME_SZ, "%s", s->alias);
            else if (s->e.kind == EX_COL) snprintf(oc[noc].name, NAME_SZ, "%s", s->e.col);
            else snprintf(oc[noc].name, NAME_SZ, "expr");
            noc++;
        }
    }
    if (noc == 0) return NLSQL_ERR;

    uint64_t total = cv.n;
    if (q->has_limit && (uint64_t)q->limit < total) total = (uint64_t)q->limit;
    nlsql_result_t* r = make_result(noc, total);
    if (!r) { free(cv.v); return NLSQL_ERR; }
    for (int j = 0; j < noc; j++) snprintf(r->cols[j].name, sizeof(r->cols[j].name), "%s", oc[j].name);

    uint64_t out_row = 0;
    for (size_t i = 0; i < cv.n && out_row < total; i++) {
        for (int j = 0; j < noc; j++) {
            pv_t v = eval_expr(q, cv.v[i].idx, &oc[j].e);
            set_val(&r->rows[out_row].vals[j], &v);
            if (v.is_int) r->cols[j].type = NLSQL_TYPE_INT64;
            else if (v.is_dbl) r->cols[j].type = NLSQL_TYPE_DOUBLE;
            else if (!v.is_null) r->cols[j].type = NLSQL_TYPE_VARCHAR;
        }
        out_row++;
    }
    r->row_count = out_row;
    free(cv.v);
    *out = r;
    return NLSQL_OK;
}

/* =========================================
 * 特性检测与语句分发
 * ========================================= */

/* 大小写无关的整词查找（跳过字符串内部） */
static int find_word(const char* sql, const char* w) {
    size_t wl = strlen(w);
    int inq = 0; char q = 0;
    for (const char* p = sql; *p; p++) {
        if (inq) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == q) inq = 0;
            continue;
        }
        if (*p == '\'' || *p == '"' || *p == '`') { inq = 1; q = *p; continue; }
        if (strncasecmp(p, w, wl) == 0) {
            char before = (p == sql) ? ' ' : p[-1];
            char after = p[wl];
            if (!ident_char(before) && !ident_char(after)) return 1;
        }
    }
    return 0;
}

static int find_subquery_pattern(const char* sql) {
    int inq = 0; char q = 0;
    for (const char* p = sql; *p; p++) {
        if (inq) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == q) inq = 0;
            continue;
        }
        if (*p == '\'' || *p == '"' || *p == '`') { inq = 1; q = *p; continue; }
        if (*p == '(') {
            const char* t = p + 1;
            while (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n') t++;
            if (strncasecmp(t, "select", 6) == 0 &&
                !ident_char(t[6])) return 1;
        }
    }
    return 0;
}

static void trim_copy(char* dst, size_t dsz, const char* src) {
    while (*src && isspace((unsigned char)*src)) src++;
    size_t n = strlen(src);
    while (n > 0 && isspace((unsigned char)src[n - 1])) n--;
    if (n >= dsz) n = dsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 从 FROM 后读取首个源名（跳过字符串），返回 1 成功 */
static int extract_first_from(const char* sql, char* out, size_t osz) {
    int inq = 0; char q = 0;
    for (const char* p = sql; *p; p++) {
        if (inq) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == q) inq = 0;
            continue;
        }
        if (*p == '\'' || *p == '"' || *p == '`') { inq = 1; q = *p; continue; }
        if (strncasecmp(p, "from", 4) == 0 && !ident_char(p[4]) && (p == sql || !ident_char(p[-1]))) {
            const char* t = p + 4;
            while (*t && isspace((unsigned char)*t)) t++;
            if (*t == '(') return 0; /* 派生表 */
            const char* st = t;
            if (*t == '`') { t++; st = t; while (*t && *t != '`') t++; }
            else { while (*t && ident_char(*t)) t++; }
            size_t n = (size_t)(t - st);
            if (n == 0 || n >= osz) return 0;
            memcpy(out, st, n); out[n] = '\0';
            return 1;
        }
    }
    return 0;
}

static int is_stmt_select(const char* sql) {
    while (*sql && isspace((unsigned char)*sql)) sql++;
    return strncasecmp(sql, "select", 6) == 0 && !ident_char(sql[6]);
}

static int is_stmt_dml(const char* sql, int* event) {
    while (*sql && isspace((unsigned char)*sql)) sql++;
    if (strncasecmp(sql, "insert", 6) == 0 && !ident_char(sql[6])) { *event = 1; return 1; }
    if (strncasecmp(sql, "update", 6) == 0 && !ident_char(sql[6])) { *event = 2; return 1; }
    if (strncasecmp(sql, "delete", 6) == 0 && !ident_char(sql[6])) { *event = 3; return 1; }
    return 0;
}

/* 提取 DML 目标表名 */
static int extract_dml_table(const char* sql, char* out, size_t osz) {
    const char* p = sql;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncasecmp(p, "insert", 6) == 0) {
        p += 6;
        while (*p && isspace((unsigned char)*p)) p++;
        if (strncasecmp(p, "into", 4) == 0) { p += 4; while (*p && isspace((unsigned char)*p)) p++; }
    } else if (strncasecmp(p, "update", 6) == 0) {
        p += 6;
        while (*p && isspace((unsigned char)*p)) p++;
    } else if (strncasecmp(p, "delete", 6) == 0) {
        p += 6;
        while (*p && isspace((unsigned char)*p)) p++;
        if (strncasecmp(p, "from", 4) == 0) { p += 4; while (*p && isspace((unsigned char)*p)) p++; }
    } else return 0;
    const char* st = p;
    while (*p && ident_char(*p)) p++;
    size_t n = (size_t)(p - st);
    if (n == 0 || n >= osz) return 0;
    memcpy(out, st, n); out[n] = '\0';
    return 1;
}

/* 执行触发器体：按分号拆分（忽略引号内分号） */
static void run_trigger_bodies(nlsql_conn_t* conn, char bodies[][4096], int n) {
    for (int i = 0; i < n; i++) {
        const char* p = bodies[i];
        while (*p) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            int inq = 0; char q = 0;
            const char* st = p;
            while (*p) {
                if (inq) {
                    if (*p == '\\' && p[1]) { p += 2; continue; }
                    if (*p == q) inq = 0;
                    p++;
                    continue;
                }
                if (*p == '\'' || *p == '"') { inq = 1; q = *p; p++; continue; }
                if (*p == ';') break;
                p++;
            }
            size_t n = (size_t)(p - st);
            if (n > 0) {
                char* one = (char*)malloc(n + 1);
                if (one) {
                    memcpy(one, st, n); one[n] = '\0';
                    nlsql_result_t* rr = NULL;
                    nlsql_query(conn, one, &rr);
                    nlsql_free_result(rr);
                    free(one);
                }
            }
            if (*p == ';') p++;
        }
    }
}

/* ---------------- DDL 处理器 ---------------- */

static int handle_create_view(nlsql_conn_t* conn, const char* sql, nlsql_result_t** out) {
    const char* p = sql;
    p += 6; /* CREATE */
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncasecmp(p, "view", 4) != 0) return NLSQL_EXT_UNHANDLED;
    p += 4;
    while (*p && isspace((unsigned char)*p)) p++;
    const char* st = p;
    while (*p && ident_char(*p)) p++;
    if (p == st) return NLSQL_EXT_UNHANDLED;
    char name[NAME_SZ];
    size_t n = (size_t)(p - st);
    if (n >= sizeof(name)) return NLSQL_EXT_UNHANDLED;
    memcpy(name, st, n); name[n] = '\0';
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncasecmp(p, "as", 2) != 0) return NLSQL_EXT_UNHANDLED;
    p += 2;
    char body[4096];
    trim_copy(body, sizeof(body), p);
    /* 去掉结尾分号 */
    size_t bl = strlen(body);
    while (bl > 0 && body[bl - 1] == ';') body[--bl] = '\0';
    if (view_add(name, body) != 0) return NLSQL_ERR;
    nlsql_ext_meta_put(conn, "view", name, "", body);
    if (out) *out = NULL;
    return NLSQL_OK;
}

static int handle_drop_view(nlsql_conn_t* conn, const char* sql, nlsql_result_t** out) {
    const char* p = sql;
    p += 4; /* DROP */
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncasecmp(p, "view", 4) != 0) return NLSQL_EXT_UNHANDLED;
    p += 4;
    while (*p && isspace((unsigned char)*p)) p++;
    const char* st = p;
    while (*p && ident_char(*p)) p++;
    if (p == st) return NLSQL_EXT_UNHANDLED;
    char name[NAME_SZ];
    size_t n = (size_t)(p - st);
    if (n >= sizeof(name)) return NLSQL_EXT_UNHANDLED;
    memcpy(name, st, n); name[n] = '\0';
    if (view_drop(name) != 0) return NLSQL_ERR;
    nlsql_ext_meta_del(conn, "view", name);
    if (out) *out = NULL;
    return NLSQL_OK;
}

static int handle_create_trigger(nlsql_conn_t* conn, const char* sql, nlsql_result_t** out) {
    const char* p = sql;
    p += 6;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncasecmp(p, "trigger", 7) != 0) return NLSQL_EXT_UNHANDLED;
    p += 7;
    while (*p && isspace((unsigned char)*p)) p++;
    const char* st = p;
    while (*p && ident_char(*p)) p++;
    if (p == st) return NLSQL_EXT_UNHANDLED;
    trig_t t; memset(&t, 0, sizeof(t));
    size_t n = (size_t)(p - st);
    if (n >= sizeof(t.name)) return NLSQL_EXT_UNHANDLED;
    memcpy(t.name, st, n); t.name[n] = '\0';

    const char* on = strcasestr(p, " on ");
    if (!on) return NLSQL_EXT_UNHANDLED;
    /* event 位于 trigger 名与 ON 之间 */
    char mid[128];
    size_t mn = (size_t)(on - p);
    if (mn >= sizeof(mid)) return NLSQL_EXT_UNHANDLED;
    memcpy(mid, p, mn); mid[mn] = '\0';
    if (strcasestr(mid, "after") == NULL && strcasestr(mid, "before") == NULL) return NLSQL_EXT_UNHANDLED;
    if (strcasestr(mid, "insert")) t.event = 1;
    else if (strcasestr(mid, "update")) t.event = 2;
    else if (strcasestr(mid, "delete")) t.event = 3;
    else return NLSQL_EXT_UNHANDLED;

    p = on + 4;
    while (*p && isspace((unsigned char)*p)) p++;
    st = p;
    while (*p && ident_char(*p)) p++;
    n = (size_t)(p - st);
    if (n == 0 || n >= sizeof(t.table)) return NLSQL_EXT_UNHANDLED;
    memcpy(t.table, st, n); t.table[n] = '\0';

    /* 跳过 FOR EACH ROW */
    const char* forp = strcasestr(p, "for each row");
    if (forp) p = forp + strlen("for each row");
    trim_copy(t.body, sizeof(t.body), p);
    size_t bl = strlen(t.body);
    while (bl > 0 && t.body[bl - 1] == ';') t.body[--bl] = '\0';
    if (trig_add(&t) != 0) return NLSQL_ERR;
    {
        char def[4200];
        snprintf(def, sizeof(def), "%d|%s", t.event, t.body);
        nlsql_ext_meta_put(conn, "trigger", t.name, t.table, def);
    }
    if (out) *out = NULL;
    return NLSQL_OK;
}

static int handle_drop_trigger(nlsql_conn_t* conn, const char* sql, nlsql_result_t** out) {
    const char* p = sql;
    p += 4;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncasecmp(p, "trigger", 7) != 0) return NLSQL_EXT_UNHANDLED;
    p += 7;
    while (*p && isspace((unsigned char)*p)) p++;
    const char* st = p;
    while (*p && ident_char(*p)) p++;
    if (p == st) return NLSQL_EXT_UNHANDLED;
    char name[NAME_SZ];
    size_t n = (size_t)(p - st);
    if (n >= sizeof(name)) return NLSQL_EXT_UNHANDLED;
    memcpy(name, st, n); name[n] = '\0';
    if (trig_drop(name) != 0) return NLSQL_ERR;
    nlsql_ext_meta_del(conn, "trigger", name);
    if (out) *out = NULL;
    return NLSQL_OK;
}

/* ---------------- 触发器 NEW/OLD 展开 ---------------- */

#define TRG_COL_MAX 64

typedef struct {
    char name[NAME_SZ];
    char newv[256];
    char oldv[256];
    int  has_new, has_old;
} trg_map_t;

/* 值 → SQL 字面量 */
static void value_lit(const nlsql_value_t* v, char* out, size_t sz) {
    if (!v || v->is_null || v->type == NLSQL_TYPE_NULL) { snprintf(out, sz, "NULL"); return; }
    if (v->type == NLSQL_TYPE_INT64) { snprintf(out, sz, "%lld", (long long)v->i); return; }
    if (v->type == NLSQL_TYPE_DOUBLE) { snprintf(out, sz, "%.17g", v->d); return; }
    size_t k = 0;
    out[k++] = '\'';
    for (uint32_t i = 0; i < v->len && k + 3 < sz; i++) {
        if (v->s[i] == '\'' || v->s[i] == '\\') out[k++] = '\\';
        out[k++] = v->s[i];
    }
    out[k++] = '\'';
    out[k] = '\0';
}

/* 读取一个 SQL 字面量（字符串/数字/NULL，原样保留文本） */
static int read_sql_literal(const char** pp, char* out, size_t sz) {
    const char* p = *pp;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '\'') {
        const char* st = p;
        p++;
        while (*p) {
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == '\'') { p++; break; }
            p++;
        }
        size_t n = (size_t)(p - st);
        if (n >= sz) return 0;
        memcpy(out, st, n); out[n] = '\0';
        *pp = p;
        return 1;
    }
    if (*p == '-' || isdigit((unsigned char)*p)) {
        const char* st = p;
        if (*p == '-') p++;
        while (*p && (isdigit((unsigned char)*p) || *p == '.' || *p == 'e' || *p == 'E' ||
                      *p == '+' || *p == '-')) p++;
        size_t n = (size_t)(p - st);
        if (n == 0 || n >= sz) return 0;
        memcpy(out, st, n); out[n] = '\0';
        *pp = p;
        return 1;
    }
    if (strncasecmp(p, "null", 4) == 0 && !ident_char(p[4])) {
        snprintf(out, sz, "NULL");
        *pp = p + 4;
        return 1;
    }
    return 0;
}

static int trg_col_idx(const trg_map_t* m, int n, const char* name) {
    for (int i = 0; i < n; i++)
        if (strcasecmp(m[i].name, name) == 0) return i;
    return -1;
}

/* 读取表列清单（SHOW COLUMNS），返回列数 */
static int trg_load_cols(nlsql_conn_t* conn, const char* tbl, trg_map_t* m, int max) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SHOW COLUMNS FROM %s", tbl);
    nlsql_result_t* r = NULL;
    if (nlsql_query(conn, sql, &r) != NLSQL_OK || !r) { nlsql_free_result(r); return -1; }
    int n = 0;
    for (uint64_t i = 0; i < r->row_count && n < max && r->col_count > 0; i++) {
        memset(&m[n], 0, sizeof(m[n]));
        snprintf(m[n].name, NAME_SZ, "%s", r->rows[i].vals[0].s ? r->rows[i].vals[0].s : "");
        n++;
    }
    nlsql_free_result(r);
    return n;
}

/* 记录 WHERE 位置（跳过字符串；返回 NULL 表示无 WHERE） */
static const char* find_where(const char* sql) {
    int inq = 0; char q = 0;
    for (const char* p = sql; *p; p++) {
        if (inq) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == q) inq = 0;
            continue;
        }
        if (*p == '\'' || *p == '"' || *p == '`') { inq = 1; q = *p; continue; }
        if (strncasecmp(p, "where", 5) == 0) {
            char before = (p == sql) ? ' ' : p[-1];
            if (!ident_char(before) && !ident_char(p[5])) return p + 5;
        }
    }
    return NULL;
}

/* INSERT：从 VALUES 字面量填充 NEW 列 */
static void trg_fill_insert(const char* sql, trg_map_t* m, int n) {
    const char* p = strcasestr(sql, "into");
    if (p) p += 4;
    else { p = sql; while (*p && !isspace((unsigned char)*p)) p++; }
    while (*p && isspace((unsigned char)*p)) p++;
    /* 跳过表名 */
    while (*p && ident_char(*p)) p++;
    while (*p && isspace((unsigned char)*p)) p++;
    int map[TRG_COL_MAX];
    int nc = 0;
    if (*p == '(') {
        p++;
        while (*p && nc < TRG_COL_MAX) {
            while (*p && isspace((unsigned char)*p)) p++;
            char nm[NAME_SZ];
            size_t k = 0;
            while (*p && ident_char(*p) && k < NAME_SZ - 1) nm[k++] = *p++;
            nm[k] = '\0';
            if (k == 0) break;
            map[nc++] = trg_col_idx(m, n, nm);
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == ',') { p++; continue; }
            break;
        }
        if (*p == ')') p++;
    }
    const char* v = strcasestr(p, "values");
    if (!v) return;
    p = v + 6;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(') return;
    p++;
    int k = 0;
    while (*p && k < TRG_COL_MAX) {
        char lit[256];
        if (!read_sql_literal(&p, lit, sizeof(lit))) break;
        int idx = (nc > 0) ? map[k] : k;
        if (idx >= 0 && idx < n) {
            snprintf(m[idx].newv, sizeof(m[idx].newv), "%s", lit);
            m[idx].has_new = 1;
        }
        k++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ',') { p++; continue; }
        break;
    }
}

/* UPDATE：从 SET 字面量填充 NEW 列（非字面量赋值留空，展开时回退 OLD） */
static void trg_fill_update_new(const char* sql, trg_map_t* m, int n) {
    const char* p = strcasestr(sql, "set");
    if (!p) return;
    p += 3;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        char nm[NAME_SZ];
        size_t k = 0;
        while (*p && ident_char(*p) && k < NAME_SZ - 1) nm[k++] = *p++;
        nm[k] = '\0';
        if (k == 0) break;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '=') break;
        p++;
        char lit[256];
        if (!read_sql_literal(&p, lit, sizeof(lit))) break;
        int idx = trg_col_idx(m, n, nm);
        if (idx >= 0) {
            snprintf(m[idx].newv, sizeof(m[idx].newv), "%s", lit);
            m[idx].has_new = 1;
        }
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ',') { p++; continue; }
        break;
    }
}

/* UPDATE/DELETE：预取匹配行为 OLD 列赋值 */
static void trg_fill_old(nlsql_conn_t* conn, const char* sql, const char* tbl,
                         trg_map_t* m, int n) {
    char qsql[4096];
    const char* w = find_where(sql);
    if (w) {
        snprintf(qsql, sizeof(qsql), "SELECT * FROM %s WHERE %s", tbl, w);
        size_t ql = strlen(qsql);
        while (ql > 0 && (qsql[ql - 1] == ';' || isspace((unsigned char)qsql[ql - 1]))) qsql[--ql] = '\0';
    } else {
        snprintf(qsql, sizeof(qsql), "SELECT * FROM %s LIMIT 1", tbl);
    }
    nlsql_result_t* r = NULL;
    if (nlsql_query(conn, qsql, &r) != NLSQL_OK || !r || r->row_count == 0) {
        nlsql_free_result(r);
        return;
    }
    for (int j = 0; j < r->col_count && j < n; j++) {
        int idx = trg_col_idx(m, n, r->cols[j].name);
        if (idx >= 0) {
            value_lit(&r->rows[0].vals[j], m[idx].oldv, sizeof(m[idx].oldv));
            m[idx].has_old = 1;
        }
    }
    nlsql_free_result(r);
}

/* 把触发器体内的 NEW.col / OLD.col 替换为字面量 */
static void trig_expand(const char* body, const trg_map_t* m, int n, char* out, size_t osz) {
    size_t k = 0;
    int inq = 0; char q = 0;
    for (const char* p = body; *p && k + 1 < osz; ) {
        char ch = *p;
        if (inq) {
            if (ch == '\\' && p[1] && k + 2 < osz) { out[k++] = ch; out[k++] = p[1]; p += 2; continue; }
            out[k++] = ch;
            if (ch == q) inq = 0;
            p++;
            continue;
        }
        if (ch == '\'' || ch == '"') { inq = 1; q = ch; out[k++] = ch; p++; continue; }
        int is_new = (strncasecmp(p, "new", 3) == 0 && !ident_char(p[3]));
        int is_old = (strncasecmp(p, "old", 3) == 0 && !ident_char(p[3]));
        if (is_new || is_old) {
            const char* d = p + 3;
            while (*d && isspace((unsigned char)*d)) d++;
            if (*d == '.') {
                d++;
                while (*d && isspace((unsigned char)*d)) d++;
                char nm[NAME_SZ];
                size_t j = 0;
                while (*d && ident_char(*d) && j < NAME_SZ - 1) nm[j++] = *d++;
                nm[j] = '\0';
                int idx = trg_col_idx(m, n, nm);
                const char* val = "NULL";
                if (idx >= 0) {
                    if (is_new && m[idx].has_new) val = m[idx].newv;
                    else if (!is_new && m[idx].has_old) val = m[idx].oldv;
                    else if (is_new && m[idx].has_old) val = m[idx].oldv; /* 非字面量赋值回退 OLD */
                }
                size_t vl = strlen(val);
                if (k + vl < osz) { memcpy(out + k, val, vl); k += vl; }
                p = d;
                continue;
            }
        }
        out[k++] = ch;
        p++;
    }
    out[k] = '\0';
}

/* 为一批触发器体预计算 NEW/OLD 并按行展开 */
static void trig_prepare(nlsql_conn_t* conn, const char* dml, const char* tbl, int ev,
                         char bodies[][4096], int nb, char outs[][4096]) {
    trg_map_t m[TRG_COL_MAX];
    int n = trg_load_cols(conn, tbl, m, TRG_COL_MAX);
    if (n <= 0) {
        for (int i = 0; i < nb; i++) snprintf(outs[i], 4096, "%s", bodies[i]);
        return;
    }
    if (ev == 1) trg_fill_insert(dml, m, n);
    else {
        trg_fill_old(conn, dml, tbl, m, n);
        if (ev == 2) trg_fill_update_new(dml, m, n);
    }
    for (int i = 0; i < nb; i++) trig_expand(bodies[i], m, n, outs[i], 4096);
}

/* ---------------- 语句处理器 ---------------- */

static _Thread_local int t_in_trigger;

static int sql_handler(nlsql_conn_t* conn, const char* sql, nlsql_result_t** out, void* ud) {
    (void)ud;
    if (out) *out = NULL;
    if (!conn || !sql) return NLSQL_EXT_UNHANDLED;
    while (*sql && isspace((unsigned char)*sql)) sql++;
    sql_sync_meta(conn); /* 切库/重开后重建视图与触发器 */

    if (strncasecmp(sql, "create", 6) == 0 && !ident_char(sql[6])) {
        const char* p = sql + 6;
        while (*p && isspace((unsigned char)*p)) p++;
        if (strncasecmp(p, "view", 4) == 0) return handle_create_view(conn, sql, out);
        if (strncasecmp(p, "trigger", 7) == 0) return handle_create_trigger(conn, sql, out);
        return NLSQL_EXT_UNHANDLED;
    }
    if (strncasecmp(sql, "drop", 4) == 0 && !ident_char(sql[4])) {
        const char* p = sql + 4;
        while (*p && isspace((unsigned char)*p)) p++;
        if (strncasecmp(p, "view", 4) == 0) return handle_drop_view(conn, sql, out);
        if (strncasecmp(p, "trigger", 7) == 0) return handle_drop_trigger(conn, sql, out);
        return NLSQL_EXT_UNHANDLED;
    }

    /* DML + 触发器（体内 NEW/OLD 先按本语句展开为字面量） */
    int ev = 0;
    if (!t_in_trigger && is_stmt_dml(sql, &ev)) {
        char tbl[NAME_SZ];
        if (extract_dml_table(sql, tbl, sizeof(tbl))) {
            char bodies[8][4096];
            int nb = trig_collect(tbl, ev, bodies, 8);
            if (nb > 0) {
                char outs[8][4096];
                trig_prepare(conn, sql, tbl, ev, bodies, nb, outs);
                t_in_trigger = 1;
                nlsql_result_t* r = NULL;
                int rc = nlsql_query(conn, sql, &r);
                if (rc == NLSQL_OK) run_trigger_bodies(conn, outs, nb);
                t_in_trigger = 0;
                if (out) *out = r; else nlsql_free_result(r);
                return rc;
            }
        }
    }

    if (!is_stmt_select(sql)) return NLSQL_EXT_UNHANDLED;

    /* 是否需要本扩展：JOIN / 子查询 / 视图源 */
    char from_name[NAME_SZ];
    int need = 0;
    if (find_word(sql, "join")) need = 1;
    else if (find_subquery_pattern(sql)) need = 1;
    else if (extract_first_from(sql, from_name, sizeof(from_name)) && view_lookup(from_name)) need = 1;
    if (!need) return NLSQL_EXT_UNHANDLED;

    parser_t p;
    parser_init(&p, conn, sql);
    query_t q;
    int rc = parse_select(&p, &q);
    int bad = (rc != 0) || (p.cur.type != TT_EOF);
    parser_free(&p);
    if (bad) { query_free(&q); return NLSQL_EXT_UNHANDLED; }

    nlsql_result_t* res = NULL;
    int erc = exec_query(conn, &q, &res);
    query_free(&q);
    if (erc != NLSQL_OK) { nlsql_free_result(res); return NLSQL_ERR; }
    *out = res;
    return NLSQL_OK;
}

/* ---------------- 生命周期 ---------------- */

static int g_inited;

static int sql_init(void) {
    if (g_inited) return 0;
    if (nlsql_ext_register_stmt_handler(sql_handler, NULL) != NLSQL_OK) return -1;
    nlsql_ext_set_feature("sql_kernel", 1);
    g_inited = 1;
    return 0;
}

static void sql_shutdown(void) {
    if (!g_inited) return;
    nlsql_ext_unregister_stmt_handler(sql_handler);
    nlsql_ext_set_feature("sql_kernel", 0);
    g_inited = 0;
}

int nlsql_ext_sql_init(void) { return sql_init(); }
void nlsql_ext_sql_shutdown(void) { sql_shutdown(); }

#if NLSQL_HAVE_NETLEAF_HEADER
static int ext_available(void) { return 1; }
static const char* ext_version(void) { return NLSQL_VERSION; }

NL_EXTENSION_DEFINE(nlsql_ext_sql, "NLSQL SQL", NLSQL_VERSION, "508364",
                    "JOIN、子查询、视图与触发器",
                    "Windows,Linux,MacOS",
                    NL_CAP_THREAD_SAFE | NL_CAP_EXT_SYSTEM,
                    sql_init, sql_shutdown, ext_available, ext_version);

__attribute__((visibility("default")))
nl_plugin_info_t* plugin_get_info(void) {
    (void)NL_EXTENSION_GET_INFO(nlsql_ext_sql);
    static nl_plugin_info_t info;
    memset(&info, 0, sizeof(info));
    info.name = "NLSQL SQL";
    info.id = EXT_SQL_ID;
    info.version = NLSQL_VERSION;
    info.description = "JOIN/subquery/view/trigger for NLSQL";
    info.author = "508364";
    info.license = "Apache-2.0";
    info.capabilities = NL_CAP_THREAD_SAFE;
    info.platform_windows = 1; info.platform_linux = 1; info.platform_macos = 1;
    info.min_nl_version = NL_MODULE_VERSION;
    info.init = sql_init;
    info.shutdown = sql_shutdown;
    info.is_available = ext_available;
    return &info;
}
#endif

__attribute__((visibility("default"))) int plugin_init(void) { return sql_init(); }
__attribute__((visibility("default"))) void plugin_shutdown(void) { sql_shutdown(); }
