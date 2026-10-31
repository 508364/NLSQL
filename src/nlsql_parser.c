/**
 * @file nlsql_parser.c
 * @brief NLSQL 词法分析 + 递归下降解析器（MySQL 方言子集）
 *
 * 支持语句：
 *   SELECT [cols|*|COUNT(*)] FROM t [WHERE ..] [ORDER BY .. [ASC|DESC],..]
 *          [LIMIT n [OFFSET m] | LIMIT m, n]
 *   SELECT const[, const...]                （无 FROM，兼容 SELECT 1 / SELECT @@version）
 *   INSERT INTO t [(cols)] VALUES (...), (...)
 *   UPDATE t SET c=e, ... [WHERE ..]
 *   DELETE FROM t [WHERE ..]
 *   CREATE DATABASE / DROP DATABASE [IF [NOT] EXISTS]
 *   CREATE TABLE t (col type [opts], ...) / DROP TABLE / TRUNCATE [TABLE]
 *   USE db / SHOW DATABASES | TABLES | COLUMNS FROM t
 *
 * 安全：表达式嵌套深度限制（防栈溢出）、标识符白名单、占位符 '?' 计数。
 */
#include "nlsql_internal.h"

/* =========================================
 * 词法
 * ========================================= */

typedef enum tok {
    T_EOF = 0, T_IDENT, T_NUM, T_DBL, T_STR, T_PARAM,
    T_LP, T_RP, T_COMMA, T_DOT, T_SEMI, T_STAR,
    T_PLUS, T_MINUS, T_SLASH, T_PERCENT,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_ATVAR,   /* @@system_var（ident 已小写，点号保留） */
    /* 关键字（kw 字段标记具体词） */
    T_KW
} tok_t;

typedef enum kw {
    KW_NONE = 0, KW_SELECT, KW_FROM, KW_WHERE, KW_INSERT, KW_INTO, KW_VALUES,
    KW_UPDATE, KW_SET, KW_DELETE, KW_CREATE, KW_DROP, KW_DATABASE,
    KW_DATABASES, KW_TABLE, KW_TABLES, KW_USE, KW_SHOW, KW_COLUMNS,
    KW_ORDER, KW_BY, KW_LIMIT, KW_OFFSET, KW_AND, KW_OR, KW_NOT, KW_NULL,
    KW_LIKE, KW_INT, KW_INTEGER, KW_BIGINT, KW_DOUBLE, KW_FLOAT, KW_VARCHAR,
    KW_TEXT, KW_DATETIME, KW_PRIMARY, KW_KEY, KW_AUTO_INCREMENT, KW_IF,
    KW_EXISTS, KW_TRUNCATE, KW_COUNT, KW_ASC, KW_DESC,
    KW_AS, KW_IN, KW_IS, KW_TO, KW_SUM, KW_AVG, KW_MIN, KW_MAX,
    KW_ALTER, KW_GROUP, KW_HAVING, KW_COLUMN, KW_MODIFY, KW_RENAME,
    KW_BETWEEN, KW_DISTINCT, KW_ADD, KW_DEFAULT, KW_NOT_EXISTS_HINT,
    /* SHOW 族（客户端元数据浏览） */
    KW_STATUS, KW_VARIABLES, KW_ENGINES, KW_CHARACTER, KW_COLLATION,
    KW_INDEX, KW_KEYS, KW_FULL, KW_GLOBAL, KW_SESSION, KW_WARNINGS, KW_ERRORS, KW_GRANTS,
    KW_UNIQUE
} kw_t;

typedef struct lexer {
    const char* s;
    uint32_t    pos, len;
    tok_t       tok;
    kw_t        kw;
    char        ident[NLSQL_MAX_IDENT + 1];
    int64_t     inum;
    double      dnum;
    char*       str;         /* malloc，T_STR 时有效 */
    uint32_t    slen;
    int         depth_err;   /* 解析深度超限 */
} lexer_t;

static void lex_free(lexer_t* lx) {
    free(lx->str);
    lx->str = NULL;
}

static int is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}
static int is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

/* 关键字表（小写，按长度排序 + 长度分桶，通用 CPU 指令，无 SIMD） */
static const struct { const char* w; kw_t k; } g_kws[] = {
    /* len 2 */ {"as",KW_AS},{"by",KW_BY},{"if",KW_IF},{"in",KW_IN},{"is",KW_IS},{"or",KW_OR},{"to",KW_TO},
    /* len 3 */ {"add",KW_ADD},{"and",KW_AND},{"asc",KW_ASC},{"avg",KW_AVG},{"int",KW_INT},{"key",KW_KEY},{"max",KW_MAX},{"min",KW_MIN},{"not",KW_NOT},{"set",KW_SET},{"sum",KW_SUM},{"use",KW_USE},
    /* len 4 */ {"desc",KW_DESC},{"drop",KW_DROP},{"from",KW_FROM},{"full",KW_FULL},{"into",KW_INTO},{"keys",KW_KEYS},{"like",KW_LIKE},{"null",KW_NULL},{"show",KW_SHOW},{"text",KW_TEXT},
    /* len 5 */ {"alter",KW_ALTER},{"count",KW_COUNT},{"float",KW_FLOAT},{"group",KW_GROUP},{"index",KW_INDEX},{"limit",KW_LIMIT},{"order",KW_ORDER},{"table",KW_TABLE},{"where",KW_WHERE},
    /* len 6 */ {"bigint",KW_BIGINT},{"column",KW_COLUMN},{"create",KW_CREATE},{"delete",KW_DELETE},{"double",KW_DOUBLE},{"errors",KW_ERRORS},{"exists",KW_EXISTS},{"global",KW_GLOBAL},{"grants",KW_GRANTS},{"having",KW_HAVING},{"insert",KW_INSERT},{"modify",KW_MODIFY},{"offset",KW_OFFSET},{"rename",KW_RENAME},{"select",KW_SELECT},{"status",KW_STATUS},{"tables",KW_TABLES},{"unique",KW_UNIQUE},{"update",KW_UPDATE},{"values",KW_VALUES},
    /* len 7 */ {"between",KW_BETWEEN},{"columns",KW_COLUMNS},{"default",KW_DEFAULT},{"engines",KW_ENGINES},{"integer",KW_INTEGER},{"primary",KW_PRIMARY},{"session",KW_SESSION},{"varchar",KW_VARCHAR},
    /* len 8 */ {"database",KW_DATABASE},{"datetime",KW_DATETIME},{"distinct",KW_DISTINCT},{"truncate",KW_TRUNCATE},{"warnings",KW_WARNINGS},
    /* len 9 */ {"character",KW_CHARACTER},{"collation",KW_COLLATION},{"databases",KW_DATABASES},{"variables",KW_VARIABLES},
    /* len 14 */ {"auto_increment",KW_AUTO_INCREMENT},
};
/* 每个长度的 [start,count] 分桶（索引 = 关键字长度），共 75 项 */
static const struct { uint8_t start, cnt; } g_kw_range[15] = {
    {0,0},{0,0},{0,7},{7,12},{19,10},{29,9},{38,20},{58,8},{66,5},{71,4},{0,0},{0,0},{0,0},{0,0},{75,1}
};

static kw_t kw_lookup(const char* s, uint32_t n) {
    if (n < 2 || n > 14) return KW_NONE;
    uint8_t st = g_kw_range[n].start, cnt = g_kw_range[n].cnt;
    for (uint8_t i = 0; i < cnt; i++)
        if (memcmp(s, g_kws[st + i].w, n) == 0) return g_kws[st + i].k;
    return KW_NONE;
}

/* 元数据函数名（ident 已小写）→ 函数编号；非元数据函数返回 0 */
static int meta_func_id(const char* s) {
    if (strcmp(s, "database") == 0 || strcmp(s, "schema") == 0) return MF_DATABASE;
    if (strcmp(s, "version") == 0) return MF_VERSION;
    if (strcmp(s, "user") == 0 || strcmp(s, "current_user") == 0 ||
        strcmp(s, "session_user") == 0 || strcmp(s, "system_user") == 0) return MF_USER;
    if (strcmp(s, "connection_id") == 0) return MF_CONNECTION_ID;
    if (strcmp(s, "last_insert_id") == 0) return MF_LAST_INSERT_ID;
    if (strcmp(s, "row_count") == 0) return MF_ROW_COUNT;
    return 0;
}

/* 只读预看下一个有效字符（跳过空白与注释，不改动词法状态） */
static char lex_peek_char(lexer_t* lx) {
    const char* s = lx->s;
    uint32_t p = lx->pos;
    for (;;) {
        while (p < lx->len && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
        if (p + 1 < lx->len && s[p] == '-' && s[p + 1] == '-') {
            while (p < lx->len && s[p] != '\n') p++;
            continue;
        }
        if (p < lx->len && s[p] == '#') {
            while (p < lx->len && s[p] != '\n') p++;
            continue;
        }
        if (p + 1 < lx->len && s[p] == '/' && s[p + 1] == '*') {
            p += 2;
            while (p + 1 < lx->len && !(s[p] == '*' && s[p + 1] == '/')) p++;
            p = (p + 1 < lx->len) ? p + 2 : lx->len;
            continue;
        }
        break;
    }
    return p < lx->len ? s[p] : '\0';
}

static void lex_next(lexer_t* lx) {
    const char* s = lx->s;
    for (;;) {
        while (lx->pos < lx->len) {
            char c = s[lx->pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') lx->pos++;
            else break;
        }
        if (lx->pos + 1 < lx->len && s[lx->pos] == '-' && s[lx->pos + 1] == '-') {
            while (lx->pos < lx->len && s[lx->pos] != '\n') lx->pos++;
            continue;
        }
        if (lx->pos < lx->len && s[lx->pos] == '#') {
            while (lx->pos < lx->len && s[lx->pos] != '\n') lx->pos++;
            continue;
        }
        /* C 风格注释整段跳过；版本注释（感叹号前缀的块注释）同样按注释处理，兼容客户端驱动 */
        if (lx->pos + 1 < lx->len && s[lx->pos] == '/' && s[lx->pos + 1] == '*') {
            uint32_t p = lx->pos + 2;
            while (p + 1 < lx->len && !(s[p] == '*' && s[p + 1] == '/')) p++;
            lx->pos = (p + 1 < lx->len) ? p + 2 : lx->len;
            continue;
        }
        break;
    }
    if (lx->pos >= lx->len) { lx->tok = T_EOF; return; }
    char c = s[lx->pos];
    if (is_ident_start(c)) {
        uint32_t st = lx->pos;
        while (lx->pos < lx->len && (is_ident_char(s[lx->pos]) || s[lx->pos] == '`')) {
            if (s[lx->pos] == '`') { lx->pos++; continue; } /* 跳过反引号 */
            lx->pos++;
        }
        uint32_t n = lx->pos - st;
        if (n > NLSQL_MAX_IDENT) { lx->tok = T_EOF; lx->depth_err = 2; return; }
        char low[NLSQL_MAX_IDENT + 1];
        for (uint32_t i = 0; i < n; i++)
            low[i] = (s[st + i] >= 'A' && s[st + i] <= 'Z') ? (char)(s[st + i] + 32) : s[st + i];
        low[n] = '\0';
        memcpy(lx->ident, low, n + 1);
        lx->kw = kw_lookup(low, n);
        lx->tok = T_KW;
        return;
    }
    if (c == '@') {
        /* @var / @@var：系统变量与用户变量统一按名字收录（用户变量仅返回 NULL） */
        lx->pos++;
        if (lx->pos < lx->len && s[lx->pos] == '@') lx->pos++;
        uint32_t ns = lx->pos;
        while (lx->pos < lx->len &&
               (is_ident_char(s[lx->pos]) || s[lx->pos] == '.')) lx->pos++;
        uint32_t n = lx->pos - ns;
        if (n == 0 || n > NLSQL_MAX_IDENT) { lx->tok = T_EOF; lx->depth_err = 5; return; }
        for (uint32_t i = 0; i < n; i++)
            lx->ident[i] = (s[ns + i] >= 'A' && s[ns + i] <= 'Z') ? (char)(s[ns + i] + 32) : s[ns + i];
        lx->ident[n] = '\0';
        lx->kw = KW_NONE;
        lx->tok = T_ATVAR;
        return;
    }
    if (c >= '0' && c <= '9') {
        uint32_t st = lx->pos;
        while (lx->pos < lx->len && s[lx->pos] >= '0' && s[lx->pos] <= '9') lx->pos++;
        int isdbl = 0;
        if (lx->pos < lx->len && s[lx->pos] == '.' && lx->pos + 1 < lx->len &&
            s[lx->pos + 1] >= '0' && s[lx->pos + 1] <= '9') {
            isdbl = 1;
            lx->pos++;
            while (lx->pos < lx->len && s[lx->pos] >= '0' && s[lx->pos] <= '9') lx->pos++;
        }
        /* 科学计数法指数（如 77.25e0，MySQL 协议客户端常用转义形式） */
        if (lx->pos < lx->len && (s[lx->pos] == 'e' || s[lx->pos] == 'E')) {
            uint32_t ep = lx->pos + 1;
            if (ep < lx->len && (s[ep] == '+' || s[ep] == '-')) ep++;
            if (ep < lx->len && s[ep] >= '0' && s[ep] <= '9') {
                isdbl = 1;
                lx->pos = ep;
                while (lx->pos < lx->len && s[lx->pos] >= '0' && s[lx->pos] <= '9') lx->pos++;
            }
        }
        uint32_t n = lx->pos - st;
        char num[64];
        if (n >= sizeof(num)) { lx->tok = T_EOF; lx->depth_err = 3; return; }
        memcpy(num, s + st, n); num[n] = '\0';
        if (isdbl) { lx->tok = T_DBL; lx->dnum = atof(num); }
        else { lx->tok = T_NUM; lx->inum = strtoll(num, NULL, 10); }
        return;
    }
    if (c == '\'') {
        /* 预扫描定位闭合引号：无转义（绝大多数情况，大 TEXT 字面量即此类）时
           一次 allot + 一次 memcpy 整段吞入；原先逐字符 sbuf_append_u8（2MB 字面量
           = 千万次跨编译单元调用 + 十余次倍增 realloc）是解析阶段的最大热点 */
        uint32_t p = lx->pos + 1, esc = 0;
        while (p < lx->len) {
            char ch = s[p];
            if (ch == '\'') {
                if (p + 1 < lx->len && s[p + 1] == '\'') { esc = 1; p += 2; continue; }
                break; /* 闭合引号 */
            }
            if (ch == '\\' && p + 1 < lx->len) { esc = 1; p += 2; continue; }
            p++;
        }
        if (p >= lx->len) { lx->tok = T_EOF; lx->depth_err = 4; return; }
        uint32_t lit = lx->pos + 1;
        uint32_t litlen = p - lit;
        sbuf_t b;
        if (sbuf_init(&b, litlen + 1) != 0) { lx->tok = T_EOF; return; }
        if (!esc) {
            /* 快路径：整段一次拷贝 */
            if (litlen && sbuf_append(&b, s + lit, litlen) != 0) { sbuf_free(&b); lx->tok = T_EOF; return; }
            lx->pos = p + 1;
            free(lx->str);
            lx->str = b.p;
            lx->slen = b.len;
            lx->tok = T_STR;
            return;
        }
        /* 含转义：容量已按整段预留，逐字符解码不再触发 realloc */
        lx->pos = lit;
        while (lx->pos < lx->len) {
            char ch = s[lx->pos];
            if (ch == '\'') {
                /* '' → 字面单引号转义对 */
                if (lx->pos + 1 < lx->len && s[lx->pos + 1] == '\'') {
                    lx->pos += 2;
                    if (sbuf_append_u8(&b, '\'') != 0) { sbuf_free(&b); lx->tok = T_EOF; return; }
                    continue;
                }
                break; /* 单引号 = 闭合 */
            }
            if (ch == '\\' && lx->pos + 1 < lx->len) {
                char n2 = s[lx->pos + 1];
                switch (n2) {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    case 'r': ch = '\r'; break;
                    case '0': ch = '\0'; break;
                    default:  ch = n2;   break;
                }
                lx->pos += 2;
            } else {
                lx->pos++;
            }
            if (sbuf_append_u8(&b, (uint8_t)ch) != 0) { sbuf_free(&b); lx->tok = T_EOF; return; }
        }
        if (lx->pos >= lx->len) { sbuf_free(&b); lx->tok = T_EOF; lx->depth_err = 4; return; }
        lx->pos++; /* 闭合引号 */
        free(lx->str);
        lx->str = b.p;
        lx->slen = b.len;
        lx->tok = T_STR;
        return;
    }
    lx->pos++;
    switch (c) {
        case '(': lx->tok = T_LP; return;
        case ')': lx->tok = T_RP; return;
        case ',': lx->tok = T_COMMA; return;
        case '.': lx->tok = T_DOT; return;
        case ';': lx->tok = T_SEMI; return;
        case '*': lx->tok = T_STAR; return;
        case '+': lx->tok = T_PLUS; return;
        case '-': lx->tok = T_MINUS; return;
        case '/': lx->tok = T_SLASH; return;
        case '%': lx->tok = T_PERCENT; return;
        case '?': lx->tok = T_PARAM; return;
        case '=': lx->tok = T_EQ; return;
        case '<':
            if (lx->pos < lx->len && s[lx->pos] == '=') { lx->pos++; lx->tok = T_LE; }
            else if (lx->pos < lx->len && s[lx->pos] == '>') { lx->pos++; lx->tok = T_NE; }
            else lx->tok = T_LT;
            return;
        case '>':
            if (lx->pos < lx->len && s[lx->pos] == '=') { lx->pos++; lx->tok = T_GE; }
            else lx->tok = T_GT;
            return;
        case '!':
            if (lx->pos < lx->len && s[lx->pos] == '=') { lx->pos++; lx->tok = T_NE; }
            else lx->tok = T_EOF;
            return;
        default: lx->tok = T_EOF; lx->depth_err = 5; return;
    }
}

/* =========================================
 * AST 构造 / 释放
 * ========================================= */

static expr_t* expr_new(expr_kind_t k) {
    expr_t* e = (expr_t*)calloc(1, sizeof(expr_t));
    if (e) { e->kind = k; e->col_idx = -1; e->col_epoch = 0; }
    return e;
}

static cond_t* cond_new(void) {
    return (cond_t*)calloc(1, sizeof(cond_t));
}

static void expr_free(expr_t* e) {
    if (!e) return;
    expr_free(e->a);
    expr_free(e->b);
    free(e->s);
    free(e->col);
    free(e);
}

/* 深拷贝（IN/BETWEEN 改写时复制左操作数，字符串独立分配） */
static expr_t* expr_clone(const expr_t* e) {
    if (!e) return NULL;
    expr_t* n = expr_new(e->kind);
    if (!n) return NULL;
    n->i = e->i; n->d = e->d; n->param_idx = e->param_idx;
    if (e->s) {
        n->s = (char*)malloc((size_t)e->slen + 1);
        if (!n->s) { free(n); return NULL; }
        memcpy(n->s, e->s, e->slen); n->s[e->slen] = '\0'; n->slen = e->slen;
    }
    if (e->col) {
        n->col = (char*)malloc(strlen(e->col) + 1);
        if (!n->col) { free(n); return NULL; }
        strcpy(n->col, e->col);
    }
    n->a = expr_clone(e->a);
    if (e->a && !n->a) { expr_free(n); return NULL; }
    n->b = expr_clone(e->b);
    if (e->b && !n->b) { expr_free(n); return NULL; }
    return n;
}

static void cond_free(cond_t* c) {
    if (!c) return;
    cond_free(c->a); cond_free(c->b);
    expr_free(c->l); expr_free(c->r);
    free(c);
}

void nlsql_parsed_free(parsed_stmt_t* p) {
    if (!p) return;
    switch (p->kind) {
        case ST_SELECT:
            free(p->u.sel.cols);
            free(p->u.sel.col_alias);
            expr_free(p->u.sel.agg);
            cond_free(p->u.sel.having);
            cond_free(p->u.sel.where);
            free(p->u.sel.order);
            break;
        case ST_SELECT_CONST:
            for (int i = 0; i < p->const_count; i++) expr_free(p->consts[i]);
            free(p->consts);
            free(p->const_alias);
            /* 解析中途失败（kind 未提升到 ST_SELECT）时 sel 侧已分配字段的兜底释放；
               成功的纯常量 SELECT 中这些字段均为 NULL（结构体 calloc），free 安全 */
            free(p->u.sel.cols);
            free(p->u.sel.col_alias);
            expr_free(p->u.sel.agg);
            cond_free(p->u.sel.having);
            cond_free(p->u.sel.where);
            free(p->u.sel.order);
            break;
        case ST_INSERT:
            for (int r = 0; r < p->u.ins.row_count; r++) {
                if (p->u.ins.rows && p->u.ins.rows[r]) {
                    for (int i = 0; i < p->u.ins.row_col_counts[r]; i++)
                        expr_free(p->u.ins.rows[r][i]);
                    free(p->u.ins.rows[r]);
                }
            }
            free(p->u.ins.rows);
            free(p->u.ins.row_col_counts);
            free(p->u.ins.cols);
            break;
        case ST_UPDATE:
            free(p->u.upd.cols);
            for (int i = 0; i < p->u.upd.set_count; i++) expr_free(p->u.upd.vals[i]);
            free(p->u.upd.vals);
            cond_free(p->u.upd.where);
            break;
        case ST_DELETE:
            cond_free(p->u.del.where);
            break;
        case ST_CREATE_TABLE:
            for (int i = 0; i < p->u.ct.col_count; i++)
                free(p->u.ct.cols[i].def.s);
            free(p->u.ct.cols);
            break;
        case ST_ALTER_TABLE:
            free(p->u.alt.def.def.s);
            break;
        default: break;
    }
    free(p);
}

/* =========================================
 * 解析器
 * ========================================= */

typedef struct parser {
    lexer_t lx;
    char*   err; uint32_t errsz;
    int     depth;
    parsed_stmt_t* out;
} parser_t;

static void perr(parser_t* P, const char* msg) {
    if (P->err && P->errsz)
        snprintf(P->err, P->errsz,
                 "You have an error in your SQL syntax near '%s': %s",
                 P->lx.pos <= P->lx.len ? P->lx.s + (P->lx.pos > 0 ? P->lx.pos - 1 : 0) : "",
                 msg);
}

static int expect_tok(parser_t* P, tok_t t) {
    if (P->lx.tok != t) { perr(P, "unexpected token"); return -1; }
    lex_next(&P->lx);
    return 0;
}

/* 关键字判定 */
static int kw_is(parser_t* P, kw_t k) { return P->lx.tok == T_KW && P->lx.kw == k; }
static int eat_kw(parser_t* P, kw_t k) {
    if (kw_is(P, k)) { lex_next(&P->lx); return 1; }
    return 0;
}

/* 普通标识符或非关键字的词（表名/列名允许复用部分词） */
static int parse_name(parser_t* P, char out[NLSQL_MAX_IDENT + 1]) {
    /* 词法把所有词归为 T_KW；kw==KW_NONE 即非保留字，可作标识符 */
    if (P->lx.tok == T_KW && P->lx.kw == KW_NONE) {
        snprintf(out, NLSQL_MAX_IDENT + 1, "%s", P->lx.ident);
    }
    else { perr(P, "identifier expected"); return -1; }
    lex_next(&P->lx);
    if (!nlsql_valid_ident(out)) { perr(P, "invalid identifier"); return -1; }
    return 0;
}

static expr_t* parse_expr(parser_t* P);

static expr_t* parse_primary(parser_t* P) {
    if (++P->depth > NLSQL_MAX_DEPTH) { P->lx.depth_err = 6; perr(P, "expression too deep"); return NULL; }
    expr_t* e = NULL;
    switch (P->lx.tok) {
        case T_NUM:
            e = expr_new(EX_INT); if (e) e->i = P->lx.inum;
            lex_next(&P->lx);
            break;
        case T_DBL:
            e = expr_new(EX_DOUBLE); if (e) e->d = P->lx.dnum;
            lex_next(&P->lx);
            break;
        case T_STR:
            e = expr_new(EX_STR);
            if (e) { e->s = (char*)malloc(P->lx.slen + 1);
                     if (e->s) { memcpy(e->s, P->lx.str, P->lx.slen); e->s[P->lx.slen] = '\0'; }
                     e->slen = P->lx.slen; }
            lex_next(&P->lx);
            break;
        case T_PARAM: {
            e = expr_new(EX_PARAM);
            if (e) e->param_idx = P->out->param_count++;
            lex_next(&P->lx);
            break;
        }
        case T_LP: {
            lex_next(&P->lx);
            e = parse_expr(P);
            if (!e || expect_tok(P, T_RP) != 0) { expr_free(e); e = NULL; }
            break;
        }
        case T_ATVAR:
            /* @@系统变量 / @用户变量（引擎对未知变量返回 NULL） */
            e = expr_new(EX_SYSVAR);
            if (e) e->s = strdup(P->lx.ident);
            lex_next(&P->lx);
            break;
        case T_MINUS:
            lex_next(&P->lx);
            e = parse_primary(P);
            if (e) { expr_t* n = expr_new(EX_NEG); if (!n) { expr_free(e); return NULL; } n->a = e; e = n; }
            break;
        case T_KW:
            if (P->lx.kw == KW_NULL) { e = expr_new(EX_NULL); lex_next(&P->lx); }
            else if (P->lx.kw == KW_COUNT || P->lx.kw == KW_SUM || P->lx.kw == KW_AVG ||
                     P->lx.kw == KW_MIN || P->lx.kw == KW_MAX) {
                /* 聚合函数：COUNT(*) | COUNT/SUM/AVG/MIN/MAX (expr) */
                agg_kind_t ak = (P->lx.kw == KW_COUNT) ? AGG_COUNT :
                                (P->lx.kw == KW_SUM)   ? AGG_SUM   :
                                (P->lx.kw == KW_AVG)   ? AGG_AVG   :
                                (P->lx.kw == KW_MIN)   ? AGG_MIN   : AGG_MAX;
                lex_next(&P->lx);
                if (expect_tok(P, T_LP) != 0) return NULL;
                expr_t* arg = NULL;
                if (P->lx.tok == T_STAR) {
                    if (ak != AGG_COUNT) { perr(P, "'*' only allowed in COUNT(*)"); return NULL; }
                    lex_next(&P->lx);
                } else {
                    arg = parse_expr(P);
                    if (!arg) return NULL;
                }
                if (expect_tok(P, T_RP) != 0) { expr_free(arg); return NULL; }
                e = expr_new(EX_AGG);
                if (!e) { expr_free(arg); return NULL; }
                e->i = (int64_t)ak;
                e->a = arg;
            }
            else {
                /* 元数据函数（DATABASE()/VERSION()/USER()/...）：名称后紧跟 '(' 才算函数 */
                int fid = meta_func_id(P->lx.ident);
                if (fid > 0 && lex_peek_char(&P->lx) == '(') {
                    lex_next(&P->lx);
                    if (expect_tok(P, T_LP) != 0) return NULL;
                    if (expect_tok(P, T_RP) != 0) return NULL;
                    e = expr_new(EX_FUNC);
                    if (e) e->i = fid;
                    break;
                }
                /* 标识符（含非关键字）→ 列引用；支持限定列名 tbl.col（引擎按末段列名匹配） */
                {
                    char qcol[2 * NLSQL_MAX_IDENT + 2];
                    snprintf(qcol, sizeof(qcol), "%s", P->lx.ident);
                    lex_next(&P->lx);
                    if (P->lx.tok == T_DOT) {
                        lex_next(&P->lx);
                        if (P->lx.tok != T_KW || P->lx.kw != KW_NONE) {
                            perr(P, "column name expected after '.'");
                            return NULL;
                        }
                        size_t qn = strlen(qcol);
                        snprintf(qcol + qn, sizeof(qcol) - qn, ".%s", P->lx.ident);
                        lex_next(&P->lx);
                    }
                    e = expr_new(EX_COL);
                    if (e) e->col = strdup(qcol);
                }
            }
            break;
        default:
            break;
    }
    if (!e && !P->lx.depth_err) perr(P, "expression expected");
    P->depth--;
    return e;
}

static expr_t* parse_term(parser_t* P) {
    expr_t* l = parse_primary(P);
    if (!l) return NULL;
    int chain = 0;
    while (P->lx.tok == T_STAR || P->lx.tok == T_SLASH || P->lx.tok == T_PERCENT) {
        if (++chain > NLSQL_MAX_DEPTH) { /* 左结合链不走 depth 检查，需单独限长防求值递归栈溢出 */
            P->lx.depth_err = 6; perr(P, "expression too deep"); expr_free(l); return NULL;
        }
        expr_kind_t k = P->lx.tok == T_STAR ? EX_MUL : P->lx.tok == T_SLASH ? EX_DIV : EX_MOD;
        lex_next(&P->lx);
        expr_t* r = parse_primary(P);
        if (!r) { expr_free(l); return NULL; }
        expr_t* n = expr_new(k);
        if (!n) { expr_free(l); expr_free(r); return NULL; }
        n->a = l; n->b = r;
        l = n;
    }
    return l;
}

static expr_t* parse_expr(parser_t* P) {
    if (++P->depth > NLSQL_MAX_DEPTH) { P->lx.depth_err = 6; perr(P, "expression too deep"); return NULL; }
    expr_t* l = parse_term(P);
    if (!l) { P->depth--; return NULL; }
    int chain = 0;
    while (P->lx.tok == T_PLUS || P->lx.tok == T_MINUS) {
        if (++chain > NLSQL_MAX_DEPTH) { /* 左结合链：链长受 NLSQL_MAX_DEPTH 限制 */
            P->lx.depth_err = 6; perr(P, "expression too deep"); expr_free(l); P->depth--; return NULL;
        }
        expr_kind_t k = P->lx.tok == T_PLUS ? EX_ADD : EX_SUB;
        lex_next(&P->lx);
        expr_t* r = parse_term(P);
        if (!r) { expr_free(l); P->depth--; return NULL; }
        expr_t* n = expr_new(k);
        if (!n) { expr_free(l); expr_free(r); P->depth--; return NULL; }
        n->a = l; n->b = r;
        l = n;
    }
    P->depth--;
    return l;
}

static cond_t* parse_or(parser_t* P);
static cond_t* parse_and(parser_t* P) {
    if (++P->depth > NLSQL_MAX_DEPTH) { perr(P, "condition too deep"); return NULL; }
    cond_t* l = NULL;
    if (eat_kw(P, KW_NOT)) {
        cond_t* inner = parse_and(P);
        if (!inner) { P->depth--; return NULL; }
        l = cond_new();
        if (!l) { cond_free(inner); P->depth--; return NULL; }
        l->is_not = 1; l->a = inner;
    } else if (P->lx.tok == T_LP) {
        lex_next(&P->lx);
        l = parse_or(P);
        if (l) {
            if (expect_tok(P, T_RP) != 0) { cond_free(l); l = NULL; }
        }
    } else {
        expr_t* le = parse_expr(P);
        if (!le) { P->depth--; return NULL; }
        /* ---- 特殊谓词：IS [NOT] NULL / [NOT] IN (...) / [NOT] BETWEEN a AND b ---- */
        int neg = 0;
        if (P->lx.tok == T_KW && P->lx.kw == KW_NOT) {
            neg = 1;
            lex_next(&P->lx);
            if (P->lx.tok != T_KW) { expr_free(le); perr(P, "LIKE/IN/BETWEEN expected after NOT"); P->depth--; return NULL; }
        }
        if (neg && P->lx.tok == T_KW && P->lx.kw == KW_LIKE) {   /* a NOT LIKE b */
            lex_next(&P->lx);
            expr_t* re = parse_expr(P);
            if (!re) { expr_free(le); P->depth--; return NULL; }
            l = cond_new();
            if (!l) { expr_free(le); expr_free(re); P->depth--; return NULL; }
            l->l = le; l->r = re; l->op = OP_NOT_LIKE;
            goto leaf_done;
        }
        if (P->lx.tok == T_KW && P->lx.kw == KW_IS) {
            lex_next(&P->lx);
            int n2 = eat_kw(P, KW_NOT);
            if (!kw_is(P, KW_NULL)) { expr_free(le); perr(P, "NULL expected after IS"); P->depth--; return NULL; }
            lex_next(&P->lx);
            l = cond_new();
            if (!l) { expr_free(le); P->depth--; return NULL; }
            l->l = le; l->op = OP_IS_NULL; l->is_not = n2;
            goto leaf_done;
        }
        if (P->lx.tok == T_KW && P->lx.kw == KW_IN) {
            lex_next(&P->lx);
            if (expect_tok(P, T_LP) != 0) { expr_free(le); P->depth--; return NULL; }
            cond_t* acc = NULL;
            for (int nitem = 0; ; nitem++) {
                if (nitem >= 256) { expr_free(le); cond_free(acc); perr(P, "IN list too long"); P->depth--; return NULL; }
                expr_t* re = parse_expr(P);
                if (!re) { expr_free(le); cond_free(acc); P->depth--; return NULL; }
                cond_t* lc = cond_new();
                if (!lc) { expr_free(le); expr_free(re); cond_free(acc); P->depth--; return NULL; }
                lc->l = expr_clone(le); lc->r = re; lc->op = OP_EQ;
                if (acc) {
                    cond_t* n = cond_new();
                    if (!n) { expr_free(le); cond_free(acc); cond_free(lc); P->depth--; return NULL; }
                    n->is_and = 0; n->a = acc; n->b = lc;  /* OR 链 */
                    acc = n;
                } else acc = lc;
                if (P->lx.tok == T_COMMA) { lex_next(&P->lx); continue; }
                break;
            }
            if (expect_tok(P, T_RP) != 0) { expr_free(le); cond_free(acc); P->depth--; return NULL; }
            expr_free(le);
            if (neg) {
                cond_t* w = cond_new();
                if (!w) { cond_free(acc); P->depth--; return NULL; }
                w->is_not = 1; w->a = acc;
                acc = w;
            }
            l = acc;
            goto leaf_done;
        }
        if (P->lx.tok == T_KW && P->lx.kw == KW_BETWEEN) {
            lex_next(&P->lx);
            expr_t* lo = parse_expr(P);
            if (!lo) { expr_free(le); P->depth--; return NULL; }
            if (!eat_kw(P, KW_AND)) { expr_free(le); expr_free(lo); perr(P, "AND expected in BETWEEN"); P->depth--; return NULL; }
            expr_t* hi = parse_expr(P);
            if (!hi) { expr_free(le); expr_free(lo); P->depth--; return NULL; }
            cond_t* c1 = cond_new(), * c2 = cond_new(), * acc = cond_new();
            if (!c1 || !c2 || !acc) { expr_free(le); expr_free(lo); expr_free(hi); free(c1); free(c2); free(acc); P->depth--; return NULL; }
            c1->l = expr_clone(le); c1->r = lo; c1->op = OP_GE;   /* x >= lo */
            c2->l = le;             c2->r = hi; c2->op = OP_LE;   /* x <= hi（le 最后一份所有权） */
            acc->is_and = 1; acc->a = c1; acc->b = c2;
            if (neg) {
                cond_t* w = cond_new();
                if (!w) { cond_free(acc); P->depth--; return NULL; }
                w->is_not = 1; w->a = acc;
                acc = w;
            }
            l = acc;
            goto leaf_done;
        }
        /* ---- 普通比较 ---- */
        if (neg) { expr_free(le); perr(P, "LIKE/IN/BETWEEN expected after NOT"); P->depth--; return NULL; }
        cmp_op_t op;
        switch (P->lx.tok) {
            case T_EQ: op = OP_EQ; break;
            case T_NE: op = OP_NE; break;
            case T_LT: op = OP_LT; break;
            case T_LE: op = OP_LE; break;
            case T_GT: op = OP_GT; break;
            case T_GE: op = OP_GE; break;
            case T_KW:
                if (P->lx.kw == KW_LIKE) { op = OP_LIKE; lex_next(&P->lx); goto got_op; }
                if (P->lx.kw == KW_NOT) {
                    lex_next(&P->lx);
                    if (kw_is(P, KW_LIKE)) { op = OP_NOT_LIKE; lex_next(&P->lx); goto got_op; }
                }
                /* fallthrough */
            default:
                expr_free(le); perr(P, "comparison operator expected"); P->depth--; return NULL;
        }
        lex_next(&P->lx);
got_op:;
        expr_t* re = parse_expr(P);
        if (!re) { expr_free(le); P->depth--; return NULL; }
        l = cond_new();
        if (!l) { expr_free(le); expr_free(re); P->depth--; return NULL; }
        l->l = le; l->r = re; l->op = op;
    }
leaf_done:
    if (!l) { P->depth--; return NULL; }
    int achain = 0;
    while (kw_is(P, KW_AND)) {
        if (++achain > NLSQL_MAX_DEPTH) { /* AND 左结合链限长，防 eval_cond 递归栈溢出 */
            P->lx.depth_err = 7; perr(P, "condition too deep"); cond_free(l); P->depth--; return NULL;
        }
        lex_next(&P->lx);
        cond_t* r = parse_and(P);
        if (!r) { cond_free(l); P->depth--; return NULL; }
        cond_t* n = cond_new();
        if (!n) { cond_free(l); cond_free(r); P->depth--; return NULL; }
        n->is_and = 1; n->a = l; n->b = r;
        l = n;
    }
    P->depth--;
    return l;
}

static cond_t* parse_or(parser_t* P) {
    if (++P->depth > NLSQL_MAX_DEPTH) { perr(P, "condition too deep"); return NULL; }
    cond_t* l = parse_and(P);
    if (!l) { P->depth--; return NULL; }
    int ochain = 0;
    while (kw_is(P, KW_OR)) {
        if (++ochain > NLSQL_MAX_DEPTH) {
            P->lx.depth_err = 7; perr(P, "condition too deep"); cond_free(l); P->depth--; return NULL;
        }
        lex_next(&P->lx);
        cond_t* r = parse_and(P);
        if (!r) { cond_free(l); P->depth--; return NULL; }
        cond_t* n = cond_new();
        if (!n) { cond_free(l); cond_free(r); P->depth--; return NULL; }
        n->is_and = 0; n->a = l; n->b = r;
        l = n;
    }
    P->depth--;
    return l;
}

static cond_t* parse_where(parser_t* P) {
    if (!eat_kw(P, KW_WHERE)) return NULL;
    cond_t* c = parse_or(P);
    if (!c && !P->lx.depth_err) P->lx.depth_err = 7; /* WHERE 存在但条件解析失败 */
    return c;
}

/* =========================================
 * 语句级解析
 * ========================================= */

static int parse_select(parser_t* P) {
    parsed_stmt_t* st = P->out;
    st->kind = ST_SELECT_CONST;
    lex_next(&P->lx);

    /* SELECT DISTINCT ... */
    if (kw_is(P, KW_DISTINCT)) { st->u.sel.distinct = 1; lex_next(&P->lx); }

    /* 列清单（支持：* | 列 [AS 别名] | 聚合函数 [AS 别名]，单聚合够用版） */
    {
        int cap = 8, n = 0;
        char (*cols)[NLSQL_MAX_IDENT + 1] = (char (*)[NLSQL_MAX_IDENT + 1])malloc(cap * (NLSQL_MAX_IDENT + 1));
        if (!cols) return -1;
        char (*alias)[NLSQL_MAX_IDENT + 1] = (char (*)[NLSQL_MAX_IDENT + 1])calloc(cap, sizeof(*alias));
        if (!alias) { free(cols); return -1; }
        int is_star = 0;
        int const_like = 0;
        if (P->lx.tok == T_NUM || P->lx.tok == T_DBL || P->lx.tok == T_STR ||
            P->lx.tok == T_PARAM || P->lx.tok == T_MINUS || P->lx.tok == T_ATVAR ||
            (P->lx.tok == T_KW && P->lx.kw == KW_NULL) ||
            (P->lx.tok == T_KW && meta_func_id(P->lx.ident) > 0 &&
             lex_peek_char(&P->lx) == '(')) const_like = 1;

        if (const_like) {
            free(cols); free(alias);
            int ccap = 8;
            st->consts = (expr_t**)malloc(ccap * sizeof(expr_t*));
            st->const_alias = (char (*)[NLSQL_MAX_IDENT + 1])calloc(ccap, NLSQL_MAX_IDENT + 1);
            if (!st->consts || !st->const_alias) return -1;
            for (;;) {
                expr_t* e = parse_expr(P);
                if (!e) return -1;
                if (st->const_count == ccap) {
                    ccap *= 2;
                    expr_t** nc = (expr_t**)realloc(st->consts, ccap * sizeof(expr_t*));
                    if (!nc) { expr_free(e); return -1; }
                    st->consts = nc;
                    char (*na)[NLSQL_MAX_IDENT + 1] =
                        (char (*)[NLSQL_MAX_IDENT + 1])realloc(st->const_alias,
                                                               (size_t)ccap * (NLSQL_MAX_IDENT + 1));
                    if (!na) { expr_free(e); return -1; }
                    st->const_alias = na;
                    memset(st->const_alias + st->const_count, 0,
                           (size_t)(ccap - st->const_count) * (NLSQL_MAX_IDENT + 1));
                }
                if (eat_kw(P, KW_AS)) {
                    if (parse_name(P, st->const_alias[st->const_count]) != 0) { expr_free(e); return -1; }
                }
                st->consts[st->const_count++] = e;
                if (P->lx.tok == T_COMMA) { lex_next(&P->lx); continue; }
                break;
            }
            return 0;
        }

        for (;;) {
            /* 聚合项 */
            if (P->lx.tok == T_KW && (P->lx.kw == KW_COUNT || P->lx.kw == KW_SUM ||
                                      P->lx.kw == KW_AVG || P->lx.kw == KW_MIN || P->lx.kw == KW_MAX)) {
                if (st->u.sel.agg) { free(cols); free(alias); perr(P, "multiple aggregate functions not supported"); return -1; }
                if (is_star) { free(cols); free(alias); perr(P, "SELECT *, agg not supported"); return -1; }
                expr_t* ag = parse_primary(P);
                if (!ag) { free(cols); free(alias); return -1; }
                st->u.sel.agg = ag;
                if (eat_kw(P, KW_AS)) {
                    if (parse_name(P, st->u.sel.agg_alias) != 0) { free(cols); free(alias); return -1; }
                }
            } else if (P->lx.tok == T_STAR) {
                is_star = 1;
                lex_next(&P->lx);
            } else {
                if (P->lx.tok != T_IDENT && P->lx.tok != T_KW) {
                    if (n == 0 && !st->u.sel.agg) {
                        free(cols); free(alias);
                        perr(P, "column list expected");
                        return -1;
                    }
                    break;
                }
                if (st->u.sel.agg) { free(cols); free(alias); perr(P, "aggregate must be the last select item"); return -1; }
                if (n == cap) {
                    cap *= 2;
                    char (*nc)[NLSQL_MAX_IDENT + 1] = (char (*)[NLSQL_MAX_IDENT + 1])realloc(cols, cap * (NLSQL_MAX_IDENT + 1));
                    char (*na)[NLSQL_MAX_IDENT + 1] = (char (*)[NLSQL_MAX_IDENT + 1])realloc(alias, cap * (NLSQL_MAX_IDENT + 1));
                    if (!nc || !na) { free(nc ? nc : cols); free(na ? na : alias); return -1; }
                    cols = nc; alias = na;
                    memset(alias + n, 0, (size_t)(cap - n) * sizeof(*alias));
                }
                if (parse_name(P, cols[n]) != 0) { free(cols); free(alias); return -1; }
                if (eat_kw(P, KW_AS)) {
                    if (parse_name(P, alias[n]) != 0) { free(cols); free(alias); return -1; }
                }
                n++;
            }
            if (P->lx.tok == T_COMMA) {
                lex_next(&P->lx);
                if (P->lx.tok == T_NUM || P->lx.tok == T_DBL || P->lx.tok == T_STR ||
                    P->lx.tok == T_PARAM) {
                    free(cols); free(alias); perr(P, "mixing columns and constants not supported"); return -1;
                }
                continue;
            }
            break;
        }
        if (is_star && (n > 0 || st->u.sel.agg)) {
            free(cols); free(alias);
            perr(P, "'*' cannot be combined with other select items");
            return -1;
        }
        if (is_star) { free(cols); free(alias); st->u.sel.cols = NULL; }
        else if (n == 0 && st->u.sel.agg) { free(cols); free(alias); st->u.sel.cols = NULL; }
        else { st->u.sel.cols = cols; st->u.sel.col_count = n; st->u.sel.col_alias = alias; }
        st->kind = ST_SELECT;
        if (!eat_kw(P, KW_FROM)) { perr(P, "FROM expected"); return -1; }
        if (parse_name(P, st->u.sel.table) != 0) return -1;
    }

    st->u.sel.where = parse_where(P);
    if (P->lx.depth_err) return -1;

    /* GROUP BY 单列 + HAVING（够用版） */
    if (eat_kw(P, KW_GROUP)) {
        if (!eat_kw(P, KW_BY)) { perr(P, "BY expected"); return -1; }
        if (parse_name(P, st->u.sel.group) != 0) return -1;
        st->u.sel.has_group = 1;
        if (P->lx.tok == T_COMMA) { perr(P, "only single-column GROUP BY is supported"); return -1; }
    }
    if (eat_kw(P, KW_HAVING)) {
        st->u.sel.having = parse_or(P);
        if (!st->u.sel.having) { perr(P, "HAVING condition expected"); return -1; }
    }

    /* ORDER BY col [ASC|DESC], ... */
    if (eat_kw(P, KW_ORDER)) {
        if (!eat_kw(P, KW_BY)) { perr(P, "BY expected"); return -1; }
        int cap = 4;
        st->u.sel.order = (order_by_t*)malloc(cap * sizeof(order_by_t));
        if (!st->u.sel.order) return -1;
        for (;;) {
            if (st->u.sel.order_count == cap) {
                cap *= 2;
                order_by_t* no = (order_by_t*)realloc(st->u.sel.order, cap * sizeof(order_by_t));
                if (!no) return -1;
                st->u.sel.order = no;
            }
            order_by_t* ob = &st->u.sel.order[st->u.sel.order_count];
            memset(ob, 0, sizeof(*ob));
            if (parse_name(P, ob->col) != 0) return -1;
            if (eat_kw(P, KW_DESC)) ob->desc = 1;
            else eat_kw(P, KW_ASC);
            st->u.sel.order_count++;
            if (P->lx.tok == T_COMMA) { lex_next(&P->lx); continue; }
            break;
        }
    }

    /* LIMIT n [OFFSET m] | LIMIT m, n */
    if (eat_kw(P, KW_LIMIT)) {
        if (P->lx.tok != T_NUM) { perr(P, "number expected after LIMIT"); return -1; }
        int64_t a = P->lx.inum;
        lex_next(&P->lx);
        if (P->lx.tok == T_COMMA) {
            lex_next(&P->lx);
            if (P->lx.tok != T_NUM) { perr(P, "number expected"); return -1; }
            st->u.sel.offset = a;
            st->u.sel.has_offset = 1;
            st->u.sel.limit = P->lx.inum;
            st->u.sel.has_limit = 1;
            lex_next(&P->lx);
        } else {
            st->u.sel.limit = a;
            st->u.sel.has_limit = 1;
            if (eat_kw(P, KW_OFFSET)) {
                if (P->lx.tok != T_NUM) { perr(P, "number expected after OFFSET"); return -1; }
                st->u.sel.offset = P->lx.inum;
                st->u.sel.has_offset = 1;
                lex_next(&P->lx);
            }
        }
    }
    return 0;
}

static expr_t** parse_tuple(parser_t* P, int* count_out) {
    if (expect_tok(P, T_LP) != 0) return NULL;
    int cap = 8, n = 0;
    expr_t** vals = (expr_t**)malloc(cap * sizeof(expr_t*));
    if (!vals) return NULL;
    for (;;) {
        expr_t* e = parse_expr(P);
        if (!e) { for (int i = 0; i < n; i++) expr_free(vals[i]); free(vals); return NULL; }
        if (n == cap) {
            cap *= 2;
            expr_t** nv = (expr_t**)realloc(vals, cap * sizeof(expr_t*));
            if (!nv) { expr_free(e); for (int i = 0; i < n; i++) expr_free(vals[i]); free(vals); return NULL; }
            vals = nv;
        }
        vals[n++] = e;
        if (P->lx.tok == T_COMMA) { lex_next(&P->lx); continue; }
        break;
    }
    if (expect_tok(P, T_RP) != 0) {
        for (int i = 0; i < n; i++) expr_free(vals[i]);
        free(vals);
        return NULL;
    }
    *count_out = n;
    return vals;
}

static int parse_insert(parser_t* P) {
    parsed_stmt_t* st = P->out;
    st->kind = ST_INSERT;
    lex_next(&P->lx);
    if (!eat_kw(P, KW_INTO)) { perr(P, "INTO expected"); return -1; }
    if (parse_name(P, st->u.ins.table) != 0) return -1;

    /* 可选列清单（在 VALUES 之前且不以 VALUES 开头） */
    if (P->lx.tok == T_LP && !(P->lx.tok == T_KW && P->lx.kw == KW_VALUES)) {
        int cap = 8, n = 0;
        st->u.ins.cols = (char (*)[NLSQL_MAX_IDENT + 1])malloc(cap * (NLSQL_MAX_IDENT + 1));
        if (!st->u.ins.cols) return -1;
        lex_next(&P->lx);
        for (;;) {
            if (n >= NLSQL_MAX_COLUMNS) { perr(P, "too many columns"); return -1; }
            if (n == cap) {
                cap *= 2;
                char (*nc)[NLSQL_MAX_IDENT + 1] = (char (*)[NLSQL_MAX_IDENT + 1])realloc(st->u.ins.cols, cap * (NLSQL_MAX_IDENT + 1));
                if (!nc) return -1;
                st->u.ins.cols = nc;
            }
            if (parse_name(P, st->u.ins.cols[n]) != 0) return -1;
            n++;
            if (P->lx.tok == T_COMMA) { lex_next(&P->lx); continue; }
            break;
        }
        if (expect_tok(P, T_RP) != 0) return -1;
        st->u.ins.col_count = n;
    }

    if (!eat_kw(P, KW_VALUES)) { perr(P, "VALUES expected"); return -1; }
    int rcap = 4;
    st->u.ins.rows = (expr_t***)malloc(rcap * sizeof(expr_t**));
    st->u.ins.row_col_counts = (int*)malloc(rcap * sizeof(int));
    if (!st->u.ins.rows || !st->u.ins.row_col_counts) return -1;
    for (;;) {
        int n = 0;
        expr_t** vals = parse_tuple(P, &n);
        if (!vals) return -1;
        if (st->u.ins.row_count == rcap) {
            rcap *= 2;
            expr_t*** nr = (expr_t***)realloc(st->u.ins.rows, rcap * sizeof(expr_t**));
            int* nc2 = (int*)realloc(st->u.ins.row_col_counts, rcap * sizeof(int));
            /* 先把 realloc 成功的指针挂回 st：原块已被 realloc 移动/释放，
               再 free(nr) 会让 st 里残留的旧指针悬垂 → parsed_free 时 double-free */
            if (nr) st->u.ins.rows = nr;
            if (nc2) st->u.ins.row_col_counts = nc2;
            if (!nr || !nc2) return -1;
        }
        st->u.ins.rows[st->u.ins.row_count] = vals;
        st->u.ins.row_col_counts[st->u.ins.row_count] = n;
        st->u.ins.row_count++;
        if (P->lx.tok == T_COMMA) { lex_next(&P->lx); continue; }
        break;
    }
    return 0;
}

static int parse_update(parser_t* P) {
    parsed_stmt_t* st = P->out;
    st->kind = ST_UPDATE;
    lex_next(&P->lx);
    if (parse_name(P, st->u.upd.table) != 0) return -1;
    if (!eat_kw(P, KW_SET)) { perr(P, "SET expected"); return -1; }
    int cap = 8;
    st->u.upd.cols = (char (*)[NLSQL_MAX_IDENT + 1])malloc(cap * (NLSQL_MAX_IDENT + 1));
    st->u.upd.vals = (expr_t**)malloc(cap * sizeof(expr_t*));
    if (!st->u.upd.cols || !st->u.upd.vals) return -1;
    for (;;) {
        if (st->u.upd.set_count >= NLSQL_MAX_COLUMNS) { perr(P, "too many SET items"); return -1; }
        if (st->u.upd.set_count == cap) {
            cap *= 2;
            char (*nc)[NLSQL_MAX_IDENT + 1] = (char (*)[NLSQL_MAX_IDENT + 1])realloc(st->u.upd.cols, cap * (NLSQL_MAX_IDENT + 1));
            expr_t** nv = (expr_t**)realloc(st->u.upd.vals, cap * sizeof(expr_t*));
            /* 同 INSERT：realloc 成功的指针先挂回 st，防旧指针悬垂 double-free */
            if (nc) st->u.upd.cols = nc;
            if (nv) st->u.upd.vals = nv;
            if (!nc || !nv) return -1;
        }
        if (parse_name(P, st->u.upd.cols[st->u.upd.set_count]) != 0) return -1;
        if (expect_tok(P, T_EQ) != 0) return -1;
        expr_t* e = parse_expr(P);
        if (!e) return -1;
        st->u.upd.vals[st->u.upd.set_count] = e;
        st->u.upd.set_count++;
        if (P->lx.tok == T_COMMA) { lex_next(&P->lx); continue; }
        break;
    }
    st->u.upd.where = parse_where(P);
    return P->lx.depth_err ? -1 : 0;
}

static int parse_delete(parser_t* P) {
    parsed_stmt_t* st = P->out;
    st->kind = ST_DELETE;
    lex_next(&P->lx);
    if (!eat_kw(P, KW_FROM)) { perr(P, "FROM expected"); return -1; }
    if (parse_name(P, st->u.del.table) != 0) return -1;
    st->u.del.where = parse_where(P);
    return P->lx.depth_err ? -1 : 0;
}

static int parse_type(parser_t* P, nlsql_type_t* t, uint32_t* vlen) {
    *vlen = 0;
    if (kw_is(P, KW_INT) || kw_is(P, KW_INTEGER) || kw_is(P, KW_BIGINT)) { *t = NLSQL_TYPE_INT64; lex_next(&P->lx); return 0; }
    if (kw_is(P, KW_DOUBLE) || kw_is(P, KW_FLOAT)) { *t = NLSQL_TYPE_DOUBLE; lex_next(&P->lx); return 0; }
    if (kw_is(P, KW_VARCHAR)) {
        lex_next(&P->lx);
        if (expect_tok(P, T_LP) != 0) return -1;
        if (P->lx.tok != T_NUM || P->lx.inum < 1 || P->lx.inum > 65535) { perr(P, "bad VARCHAR length"); return -1; }
        *vlen = (uint32_t)P->lx.inum;
        lex_next(&P->lx);
        if (expect_tok(P, T_RP) != 0) return -1;
        *t = NLSQL_TYPE_VARCHAR;
        return 0;
    }
    if (kw_is(P, KW_TEXT)) { *t = NLSQL_TYPE_TEXT; lex_next(&P->lx); return 0; }
    if (kw_is(P, KW_DATETIME)) { *t = NLSQL_TYPE_DATETIME; lex_next(&P->lx); return 0; }
    perr(P, "type expected");
    return -1;
}

/* 列约束尾部：PRIMARY KEY / AUTO_INCREMENT / NOT NULL（CREATE 与 ALTER 共用） */
static int parse_col_constraints(parser_t* P, column_def_t* cd) {
    for (;;) {
        if (kw_is(P, KW_PRIMARY)) {
            lex_next(&P->lx);
            if (!eat_kw(P, KW_KEY)) { perr(P, "KEY expected"); return -1; }
            cd->pk = 1; cd->not_null = 1;
        } else if (kw_is(P, KW_AUTO_INCREMENT)) {
            lex_next(&P->lx);
            cd->auto_inc = 1;
        } else if (kw_is(P, KW_NOT)) {
            lex_next(&P->lx);
            if (!eat_kw(P, KW_NULL)) { perr(P, "NULL expected"); return -1; }
            cd->not_null = 1;
        } else if (kw_is(P, KW_DEFAULT)) {
            lex_next(&P->lx);
            int neg = 0;
            if (P->lx.tok == T_MINUS) { neg = 1; lex_next(&P->lx); }
            nlsql_value_t* d = &cd->def;
            memset(d, 0, sizeof(*d));
            if (P->lx.tok == T_NUM) {
                d->type = NLSQL_TYPE_INT64;
                d->i = neg ? -P->lx.inum : P->lx.inum;
            } else if (P->lx.tok == T_DBL) {
                d->type = NLSQL_TYPE_DOUBLE;
                d->d = neg ? -P->lx.dnum : P->lx.dnum;
            } else if (P->lx.tok == T_STR) {
                d->type = NLSQL_TYPE_VARCHAR;
                d->len = P->lx.slen;
                d->s = (char*)malloc((size_t)d->len + 1);
                if (!d->s) { perr(P, "out of memory"); return -1; }
                memcpy(d->s, P->lx.str, d->len);
                d->s[d->len] = '\0';
            } else if (P->lx.tok == T_KW && P->lx.kw == KW_NULL) {
                d->type = NLSQL_TYPE_NULL; d->is_null = 1;
            } else { perr(P, "DEFAULT value expected"); return -1; }
            lex_next(&P->lx);
            cd->has_def = 1;
        } else break;
    }
    return 0;
}

static int parse_create_table(parser_t* P) {
    parsed_stmt_t* st = P->out;
    st->kind = ST_CREATE_TABLE;
    lex_next(&P->lx);
    eat_kw(P, KW_TABLE);
    if (eat_kw(P, KW_IF)) {
        if (eat_kw(P, KW_NOT)) { if (eat_kw(P, KW_EXISTS)) st->u.ct.if_not_exists = 1; }
        else eat_kw(P, KW_EXISTS);
    }
    if (parse_name(P, st->u.ct.table) != 0) return -1;
    if (expect_tok(P, T_LP) != 0) return -1;
    int cap = 8;
    st->u.ct.cols = (column_def_t*)malloc(cap * sizeof(column_def_t));
    if (!st->u.ct.cols) return -1;
    for (;;) {
        if (st->u.ct.col_count == cap) {
            cap *= 2;
            column_def_t* nc = (column_def_t*)realloc(st->u.ct.cols, cap * sizeof(column_def_t));
            if (!nc) return -1;
            st->u.ct.cols = nc;
        }
        column_def_t* cd = &st->u.ct.cols[st->u.ct.col_count];
        memset(cd, 0, sizeof(*cd));
        if (parse_name(P, cd->name) != 0) return -1;
        if (parse_type(P, &cd->type, &cd->vlen) != 0) return -1;
        if (parse_col_constraints(P, cd) != 0) return -1;
        st->u.ct.col_count++;
        if (st->u.ct.col_count > NLSQL_MAX_COLUMNS) { perr(P, "too many columns"); return -1; }
        if (P->lx.tok == T_COMMA) { lex_next(&P->lx); continue; }
        break;
    }
    if (expect_tok(P, T_RP) != 0) return -1;
    return 0;
}

/* ALTER TABLE t ADD/DROP/MODIFY COLUMN ... | RENAME COLUMN a TO b | RENAME TO t2 */
static int parse_alter_table(parser_t* P) {
    parsed_stmt_t* st = P->out;
    st->kind = ST_ALTER_TABLE;
    lex_next(&P->lx);                 /* ALTER */
    eat_kw(P, KW_TABLE);
    if (parse_name(P, st->u.alt.table) != 0) return -1;
    if (P->lx.tok != T_KW) { perr(P, "ADD/DROP/MODIFY/RENAME expected"); return -1; }
    switch (P->lx.kw) {
        case KW_ADD:
            lex_next(&P->lx);
            eat_kw(P, KW_COLUMN);     /* COLUMN 可省略 */
            st->u.alt.op = ALT_ADD_COL;
            if (parse_name(P, st->u.alt.col) != 0) return -1;
            st->u.alt.def = (column_def_t){ 0 };
            memcpy(st->u.alt.def.name, st->u.alt.col, sizeof(st->u.alt.def.name));
            if (parse_type(P, &st->u.alt.def.type, &st->u.alt.def.vlen) != 0) return -1;
            return parse_col_constraints(P, &st->u.alt.def);
        case KW_DROP:
            lex_next(&P->lx);
            eat_kw(P, KW_COLUMN);
            st->u.alt.op = ALT_DROP_COL;
            return parse_name(P, st->u.alt.col);
        case KW_MODIFY:
            lex_next(&P->lx);
            eat_kw(P, KW_COLUMN);
            st->u.alt.op = ALT_MODIFY_COL;
            if (parse_name(P, st->u.alt.col) != 0) return -1;
            st->u.alt.def = (column_def_t){ 0 };
            memcpy(st->u.alt.def.name, st->u.alt.col, sizeof(st->u.alt.def.name));
            if (parse_type(P, &st->u.alt.def.type, &st->u.alt.def.vlen) != 0) return -1;
            return parse_col_constraints(P, &st->u.alt.def);
        case KW_RENAME: {
            lex_next(&P->lx);
            if (eat_kw(P, KW_COLUMN)) {          /* RENAME COLUMN a TO b */
                st->u.alt.op = ALT_RENAME_COL;
                if (parse_name(P, st->u.alt.col) != 0) return -1;
                if (!eat_kw(P, KW_TO)) { perr(P, "TO expected"); return -1; }
                return parse_name(P, st->u.alt.newname);
            }
            if (eat_kw(P, KW_TABLE)) {           /* RENAME TABLE a TO b */
                if (!eat_kw(P, KW_TO)) { perr(P, "TO expected"); return -1; }
            } else if (!eat_kw(P, KW_TO)) {      /* RENAME TO b */
                perr(P, "COLUMN/TO expected after RENAME"); return -1;
            }
            st->u.alt.op = ALT_RENAME_TABLE;
            return parse_name(P, st->u.alt.newname);
        }
        default:
            perr(P, "ADD/DROP/MODIFY/RENAME expected");
            return -1;
    }
}

/* CREATE [UNIQUE] INDEX name ON table (col[, col...]) */
static int parse_create_index(parser_t* P, parsed_stmt_t* st) {
    st->kind = ST_CREATE_INDEX;
    st->idx_unique = 0;
    if (kw_is(P, KW_UNIQUE)) { st->idx_unique = 1; lex_next(&P->lx); }
    if (!kw_is(P, KW_INDEX)) { perr(P, "INDEX expected"); return -1; }
    lex_next(&P->lx);
    if (parse_name(P, st->ident) != 0) return -1;              /* 索引名 */
    if (!(P->lx.tok == T_KW && P->lx.kw == KW_NONE && strcasecmp(P->lx.ident, "on") == 0)) {
        perr(P, "ON expected"); return -1;
    }
    lex_next(&P->lx);
    if (parse_name(P, st->ident2) != 0) return -1;             /* 表名 */
    if (expect_tok(P, T_LP) != 0) return -1;
    st->idx_ncol = 0;
    for (;;) {
        if (st->idx_ncol >= 8) { perr(P, "too many index columns"); return -1; }
        if (parse_name(P, st->idx_cols[st->idx_ncol]) != 0) return -1;
        st->idx_ncol++;
        if (P->lx.tok == T_COMMA) { lex_next(&P->lx); continue; }
        break;
    }
    return expect_tok(P, T_RP);
}

/* DROP INDEX name ON table */
static int parse_drop_index(parser_t* P, parsed_stmt_t* st) {
    st->kind = ST_DROP_INDEX;
    lex_next(&P->lx);                                          /* INDEX */
    if (parse_name(P, st->ident) != 0) return -1;
    if (!(P->lx.tok == T_KW && P->lx.kw == KW_NONE && strcasecmp(P->lx.ident, "on") == 0)) {
        perr(P, "ON expected"); return -1;
    }
    lex_next(&P->lx);
    return parse_name(P, st->ident2);
}

int nlsql_parse(const char* sql, parsed_stmt_t** out, char* errbuf, uint32_t errsz) {
    *out = NULL;
    if (!sql) { if (errbuf) snprintf(errbuf, errsz, "Query was empty"); return NLSQL_ER_SYNTAX; }
    uint32_t len = (uint32_t)strlen(sql);
    if (len == 0) { if (errbuf) snprintf(errbuf, errsz, "Query was empty"); return NLSQL_ER_SYNTAX; }
    if (len > NLSQL_MAX_SQL_LEN) { if (errbuf) snprintf(errbuf, errsz, "Query too large"); return NLSQL_ERR_PROTOCOL; }

    parser_t P;
    memset(&P, 0, sizeof(P));
    P.err = errbuf; P.errsz = errsz;
    P.lx.s = sql; P.lx.len = len; P.lx.pos = 0;

    parsed_stmt_t* st = (parsed_stmt_t*)calloc(1, sizeof(parsed_stmt_t));
    if (!st) return NLSQL_ERR;
    P.out = st;

    lex_next(&P.lx);
    if (P.lx.tok == T_SEMI) lex_next(&P.lx);
    if (P.lx.tok == T_EOF) {
        /* 整条语句都是注释（典型：驱动下发的版本注释语句）→ 按 no-op 成功处理，
           否则客户端初始化序列里的注释语句会拿到 1064 而中断连接 */
        lex_free(&P.lx);
        st->kind = ST_SET;
        *out = st;
        return NLSQL_OK;
    }

    int rc = 0;
    switch (P.lx.tok == T_KW ? P.lx.kw : KW_NONE) {
        case KW_SELECT:  rc = parse_select(&P); break;
        case KW_INSERT:  rc = parse_insert(&P); break;
        case KW_UPDATE:  rc = parse_update(&P); break;
        case KW_DELETE:  rc = parse_delete(&P); break;
        case KW_CREATE:
            lex_next(&P.lx);
            if (kw_is(&P, KW_DATABASE)) {
                st->kind = ST_CREATE_DB;
                lex_next(&P.lx);
                eat_kw(&P, KW_IF);
                if (eat_kw(&P, KW_NOT)) eat_kw(&P, KW_EXISTS);
                else eat_kw(&P, KW_EXISTS);
                rc = parse_name(&P, st->ident) == 0 ? 0 : -1;
            } else if (kw_is(&P, KW_TABLE)) {
                rc = parse_create_table(&P);
            } else if (kw_is(&P, KW_INDEX) || kw_is(&P, KW_UNIQUE)) {
                rc = parse_create_index(&P, st);
            } else { perr(&P, "DATABASE, TABLE or INDEX expected"); rc = -1; }
            break;
        case KW_DROP:
            lex_next(&P.lx);
            if (kw_is(&P, KW_DATABASE)) {
                st->kind = ST_DROP_DB;
                lex_next(&P.lx);
                if (eat_kw(&P, KW_IF) && eat_kw(&P, KW_EXISTS)) st->if_exists = 1;
                rc = parse_name(&P, st->ident) == 0 ? 0 : -1;
            } else if (kw_is(&P, KW_TABLE)) {
                st->kind = ST_DROP_TABLE;
                lex_next(&P.lx);
                if (eat_kw(&P, KW_IF) && eat_kw(&P, KW_EXISTS)) st->if_exists = 1;
                rc = parse_name(&P, st->ident) == 0 ? 0 : -1;
            } else if (kw_is(&P, KW_INDEX)) {
                rc = parse_drop_index(&P, st);
            } else { perr(&P, "DATABASE, TABLE or INDEX expected"); rc = -1; }
            break;
        case KW_USE:
            st->kind = ST_USE;
            lex_next(&P.lx);
            rc = parse_name(&P, st->ident) == 0 ? 0 : -1;
            break;
        case KW_SET:
            /* SET 语句（如 SET NAMES utf8mb4 / SET autocommit=1）：不透明跳过，执行时 no-op 返回 OK。
               兼容客户端连接后立即下发的 SET 配置语句，本嵌入式引擎无会话变量需生效。 */
            st->kind = ST_SET;
            P.lx.pos = P.lx.len; P.lx.tok = T_EOF;
            break;
        case KW_ALTER: rc = parse_alter_table(&P); break;
        case KW_SHOW: {
            lex_next(&P.lx);
            int is_full = 0;
            if (kw_is(&P, KW_FULL)) { is_full = 1; lex_next(&P.lx); }
            if (kw_is(&P, KW_GLOBAL)) { st->show_global = 1; lex_next(&P.lx); }
            else if (kw_is(&P, KW_SESSION)) lex_next(&P.lx);
            if (kw_is(&P, KW_DATABASES)) { st->kind = ST_SHOW_DB; lex_next(&P.lx); }
            else if (kw_is(&P, KW_TABLES)) {
                st->kind = is_full ? ST_SHOW_FULL_TABLES : ST_SHOW_TABLES;
                lex_next(&P.lx);
                if (eat_kw(&P, KW_FROM) || kw_is(&P, KW_IN)) {
                    if (kw_is(&P, KW_IN)) lex_next(&P.lx);
                    rc = parse_name(&P, st->ident3) == 0 ? 0 : -1;
                    if (rc != 0) break;
                }
                if (kw_is(&P, KW_LIKE)) {
                    lex_next(&P.lx);
                    if (P.lx.tok != T_STR) { perr(&P, "LIKE pattern expected"); rc = -1; break; }
                    snprintf(st->like_pat, sizeof(st->like_pat), "%s", P.lx.str ? P.lx.str : "");
                    st->has_like = 1;
                    lex_next(&P.lx);
                }
            } else if (kw_is(&P, KW_COLUMNS)) {
                lex_next(&P.lx);
                if (!eat_kw(&P, KW_FROM)) { perr(&P, "FROM expected"); rc = -1; break; }
                st->kind = is_full ? ST_SHOW_FULL_COLUMNS : ST_SHOW_COLUMNS;
                if (parse_name(&P, st->ident2) != 0) { rc = -1; break; }
                if (eat_kw(&P, KW_FROM)) {
                    if (parse_name(&P, st->ident3) != 0) { rc = -1; break; }
                }
                if (kw_is(&P, KW_LIKE)) {
                    lex_next(&P.lx);
                    if (P.lx.tok != T_STR) { perr(&P, "LIKE pattern expected"); rc = -1; break; }
                    snprintf(st->like_pat, sizeof(st->like_pat), "%s", P.lx.str ? P.lx.str : "");
                    st->has_like = 1;
                    lex_next(&P.lx);
                }
            } else if (kw_is(&P, KW_VARIABLES)) {
                st->kind = ST_SHOW_VARIABLES;
                lex_next(&P.lx);
                if (kw_is(&P, KW_LIKE)) {
                    lex_next(&P.lx);
                    if (P.lx.tok != T_STR) { perr(&P, "LIKE pattern expected"); rc = -1; break; }
                    snprintf(st->like_pat, sizeof(st->like_pat), "%s", P.lx.str ? P.lx.str : "");
                    st->has_like = 1;
                    lex_next(&P.lx);
                }
            } else if (kw_is(&P, KW_STATUS)) {
                st->kind = ST_SHOW_STATUS;
                lex_next(&P.lx);
                if (kw_is(&P, KW_LIKE)) {
                    lex_next(&P.lx);
                    if (P.lx.tok != T_STR) { perr(&P, "LIKE pattern expected"); rc = -1; break; }
                    snprintf(st->like_pat, sizeof(st->like_pat), "%s", P.lx.str ? P.lx.str : "");
                    st->has_like = 1;
                    lex_next(&P.lx);
                }
            } else if (kw_is(&P, KW_ENGINES)) {
                st->kind = ST_SHOW_ENGINES; lex_next(&P.lx);
            } else if (kw_is(&P, KW_CHARACTER)) {
                st->kind = ST_SHOW_CHARSET; lex_next(&P.lx); eat_kw(&P, KW_SET);
                if (kw_is(&P, KW_LIKE)) {
                    lex_next(&P.lx);
                    if (P.lx.tok != T_STR) { perr(&P, "LIKE pattern expected"); rc = -1; break; }
                    snprintf(st->like_pat, sizeof(st->like_pat), "%s", P.lx.str ? P.lx.str : "");
                    st->has_like = 1;
                    lex_next(&P.lx);
                }
            } else if (kw_is(&P, KW_COLLATION)) {
                st->kind = ST_SHOW_COLLATION; lex_next(&P.lx);
            } else if (kw_is(&P, KW_INDEX) || kw_is(&P, KW_KEYS)) {
                lex_next(&P.lx);
                if (!eat_kw(&P, KW_FROM)) eat_kw(&P, KW_IN);
                st->kind = ST_SHOW_INDEX;
                if (parse_name(&P, st->ident2) != 0) { rc = -1; break; }
                if (eat_kw(&P, KW_FROM)) {
                    if (parse_name(&P, st->ident3) != 0) { rc = -1; break; }
                }
            } else if (kw_is(&P, KW_WARNINGS) || kw_is(&P, KW_ERRORS)) {
                st->kind = ST_SHOW_WARNINGS; lex_next(&P.lx);
            } else if (kw_is(&P, KW_CREATE)) {
                lex_next(&P.lx);
                if (kw_is(&P, KW_DATABASE)) {
                    lex_next(&P.lx);
                    st->kind = ST_SHOW_CREATE_DB;
                    if (P.lx.tok == T_KW || P.lx.tok == T_IDENT)
                        rc = parse_name(&P, st->ident) == 0 ? 0 : -1;
                } else if (eat_kw(&P, KW_TABLE)) {
                    st->kind = ST_SHOW_CREATE_TABLE;
                    rc = parse_name(&P, st->ident) == 0 ? 0 : -1;
                } else {
                    perr(&P, "TABLE or DATABASE expected");
                    rc = -1;
                }
            } else if (kw_is(&P, KW_TABLE)) {
                /* SHOW TABLE STATUS [FROM db] [LIKE 'pat'] */
                lex_next(&P.lx);
                if (!kw_is(&P, KW_STATUS)) { perr(&P, "STATUS expected"); rc = -1; break; }
                lex_next(&P.lx);
                st->kind = ST_SHOW_TABLE_STATUS;
                if (eat_kw(&P, KW_FROM)) {
                    if (parse_name(&P, st->ident3) != 0) { rc = -1; break; }
                }
                if (kw_is(&P, KW_LIKE)) {
                    lex_next(&P.lx);
                    if (P.lx.tok != T_STR) { perr(&P, "LIKE pattern expected"); rc = -1; break; }
                    snprintf(st->like_pat, sizeof(st->like_pat), "%s", P.lx.str ? P.lx.str : "");
                    st->has_like = 1;
                    lex_next(&P.lx);
                } else if (kw_is(&P, KW_WHERE)) {
                    /* 客户端可能带 WHERE：本实现忽略条件（返回全部表状态） */
                    P.lx.pos = P.lx.len; P.lx.tok = T_EOF;
                }
            } else { perr(&P, "unsupported SHOW statement"); rc = -1; }
            break;
        }
        case KW_TRUNCATE:
            st->kind = ST_TRUNCATE;
            lex_next(&P.lx);
            eat_kw(&P, KW_TABLE);
            rc = parse_name(&P, st->ident) == 0 ? 0 : -1;
            break;
        default:
            perr(&P, "unknown statement");
            rc = -1;
            break;
    }

    if (rc == 0) {
        /* 语句尾部：可选分号，之后必须 EOF */
        if (P.lx.tok == T_SEMI) lex_next(&P.lx);
        if (P.lx.tok != T_EOF) {
            perr(&P, "trailing characters after statement");
            rc = -1;
        }
    }
    if (rc != 0 || P.lx.depth_err) {
        lex_free(&P.lx);
        nlsql_parsed_free(st);
        if (errbuf && !errbuf[0]) snprintf(errbuf, errsz, "You have an error in your SQL syntax");
        return NLSQL_ER_SYNTAX;
    }
    /* 只读标记 */
    switch (st->kind) {
        case ST_SELECT: case ST_SELECT_CONST: case ST_SHOW_DB:
        case ST_SHOW_TABLES: case ST_SHOW_COLUMNS: case ST_USE: case ST_SET:
        case ST_SHOW_VARIABLES: case ST_SHOW_STATUS: case ST_SHOW_ENGINES:
        case ST_SHOW_CHARSET: case ST_SHOW_COLLATION: case ST_SHOW_TABLE_STATUS:
        case ST_SHOW_FULL_TABLES: case ST_SHOW_FULL_COLUMNS: case ST_SHOW_INDEX:
        case ST_SHOW_CREATE_TABLE: case ST_SHOW_WARNINGS: case ST_SHOW_CREATE_DB:
            st->readonly_stmt = 1; break;
        default: break;
    }
    lex_free(&P.lx);
    *out = st;
    return NLSQL_OK;
}
