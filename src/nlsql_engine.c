/**
 * @file nlsql_engine.c
 * @brief NLSQL 执行引擎：语句分发、表达式求值、结果集构造
 *
 * 写路径约定：先 journal 后改内存；journal 超限自动压实（eng_flush）。
 * 并发：由外层公平读写锁 frw 串行化（读共享 / 写独占 / 压实挡写放读），本文件内部不再加锁。
 */
#include "nlsql_internal.h"
#include "netleaf_nlsql_ext.h"
#include <math.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

/* =========================================
 * 结果集构造辅助
 * ========================================= */

static nlsql_result_t* res_new(int ncols) {
    nlsql_result_t* r = (nlsql_result_t*)calloc(1, sizeof(nlsql_result_t));
    if (!r) return NULL;
    if (ncols > 0) {
        r->cols = (nlsql_column_t*)calloc((size_t)ncols, sizeof(nlsql_column_t));
        if (!r->cols) { free(r); return NULL; }
    }
    r->col_count = ncols;
    return r;
}

/* 行值 arena：每块 [块头 | nlsql_value_t...]，bump 分配。
   结果集每行 vals 不再逐行 calloc（5 万行 = 5 万次小分配与碎片），整块链统一释放 */
typedef struct val_arena_blk {
    struct val_arena_blk* next;
    uint32_t used, cap; /* 字节 */
    char data[1];
} val_arena_blk_t;

static nlsql_value_t* arena_alloc(nlsql_result_t* r, int ncols) {
    size_t need = (size_t)ncols * sizeof(nlsql_value_t);
    val_arena_blk_t* b = (val_arena_blk_t*)r->_arena;
    if (!b || b->used + need > b->cap) {
        size_t blk = need > 4096 ? need + 64 : 4096;
        b = (val_arena_blk_t*)malloc(sizeof(val_arena_blk_t) + blk);
        if (!b) return NULL;
        b->cap = (uint32_t)blk; b->used = 0;
        b->next = (val_arena_blk_t*)r->_arena;
        r->_arena = b;
    }
    nlsql_value_t* v = (nlsql_value_t*)(b->data + b->used);
    b->used += (uint32_t)need;
    memset(v, 0, need);
    return v;
}

void nlsql_val_arena_free(nlsql_result_t* r) {
    val_arena_blk_t* b = (val_arena_blk_t*)r->_arena;
    while (b) { val_arena_blk_t* n = b->next; free(b); b = n; }
    r->_arena = NULL;
}

static int res_add_row(nlsql_result_t* r, int ncols) {
    /* rows 按倍增预分配：避免逐行 realloc 的 O(N²) 拷贝与碎片 */
    if (r->row_count >= r->row_cap) {
        uint64_t nc = r->row_cap ? r->row_cap * 2 : 16;
        nlsql_row_t* nr = (nlsql_row_t*)realloc(r->rows, (size_t)nc * sizeof(nlsql_row_t));
        if (!nr) return -1;
        r->rows = nr; r->row_cap = nc;
    }
    nlsql_value_t* vals = arena_alloc(r, ncols);
    if (!vals) return -1;
    r->rows[r->row_count].vals = vals;
    r->row_count++;
    return 0;
}

static void val_set_str(nlsql_value_t* v, const char* s, uint32_t len, nlsql_type_t t) {
    v->type = t;
    v->len = len;
    v->s = (char*)malloc((size_t)len + 1);
    if (v->s) { memcpy(v->s, s, len); v->s[len] = '\0'; }
    else v->is_null = 1;
}

/* 放弃未提交行：释放值字符串；vals 归还空闲链复用（行槽在表数组内，不单独 free 行本身） */
static void row_discard(table_t* t, row_t* r) {
    if (!r) return;
    for (int i = 0; i < t->col_count; i++) free(r->vals[i].s);
    nlsql_tbl_val_recycle(t, r->vals);
    r->vals = NULL;
}

static void val_cast(nlsql_value_t* v, nlsql_type_t nt, uint32_t vlen);

/* 聚合结果槽：组计算完成后写入，HAVING/输出表达式求值时读取（查询级单聚合够用版） */
static _Thread_local nlsql_value_t g_agg_slot;
static _Thread_local int g_agg_valid;
static _Thread_local int g_agg_forbidden; /* 1=当前位置禁止聚合（WHERE 等） */

/* =========================================
 * 表达式求值
 * ========================================= */

/* 取列值：col 可能是 "table.col" 或 "col" */
static const nlsql_value_t* row_col_value(table_t* t, row_t* row, const char* col) {
    const char* dot = strchr(col, '.');
    const char* name = dot ? dot + 1 : col;
    for (int i = 0; i < t->col_count; i++)
        if (strcmp(t->cols[i].name, name) == 0)
            return &row->vals[i];
    return NULL;
}

static int value_to_double(const nlsql_value_t* v, double* out) {
    if (v->is_null) { *out = 0; return 0; }
    switch (v->type) {
        case NLSQL_TYPE_INT64: *out = (double)v->i; return 0;
        case NLSQL_TYPE_DOUBLE: *out = v->d; return 0;
        default: *out = atof(v->s ? v->s : "0"); return 0;
    }
}

static int value_to_int(const nlsql_value_t* v, int64_t* out) {
    if (v->is_null) { *out = 0; return 0; }
    switch (v->type) {
        case NLSQL_TYPE_INT64: *out = v->i; return 0;
        case NLSQL_TYPE_DOUBLE: *out = (int64_t)v->d; return 0;
        default: *out = strtoll(v->s ? v->s : "0", NULL, 10); return 0;
    }
}

/* 求值一个表达式，输出填充 v（字符串结果 malloc，调用方 free） */
static int eval_expr(expr_t* e, table_t* t, row_t* row,
                     nlsql_value_t* params, int nparams,
                     nlsql_value_t* out, char* errmsg, uint32_t errsz) {
    memset(out, 0, sizeof(*out));
    switch (e->kind) {
        case EX_NULL:
            out->type = NLSQL_TYPE_NULL; out->is_null = 1; return 0;
        case EX_INT:
            out->type = NLSQL_TYPE_INT64; out->i = e->i; return 0;
        case EX_DOUBLE:
            out->type = NLSQL_TYPE_DOUBLE; out->d = e->d; return 0;
        case EX_STR:
            val_set_str(out, e->s, e->slen, NLSQL_TYPE_VARCHAR);
            if (!out->s && e->slen) { snprintf(errmsg, errsz, "out of memory"); return -1; }
            return 0;
        case EX_PARAM:
            if (e->param_idx >= nparams) { snprintf(errmsg, errsz, "Incorrect arguments to EXECUTE"); return -1; }
            {
                nlsql_value_t* p = &params[e->param_idx];
                *out = *p;
                if (!p->is_null && (p->type == NLSQL_TYPE_VARCHAR || p->type == NLSQL_TYPE_TEXT || p->type == NLSQL_TYPE_DATETIME) && p->s) {
                    out->s = (char*)malloc((size_t)p->len + 1);
                    if (!out->s) { snprintf(errmsg, errsz, "out of memory"); return -1; }
                    memcpy(out->s, p->s, p->len); out->s[p->len] = '\0';
                }
            }
            return 0;
        case EX_COL: {
            if (!t || !row) { snprintf(errmsg, errsz, "Unknown column '%s'", e->col); return -1; }
            /* 热路径：列索引缓存 + 表结构世代校验，避免每行全表列名扫描与名字比对 */
            const nlsql_value_t* v = NULL;
            if (e->col_idx >= 0 && e->col_idx < t->col_count &&
                e->col_epoch == t->epoch) { /* 结构世代相同=名字必一致，免每行 strcmp */
                v = &row->vals[e->col_idx];
            } else {
                v = row_col_value(t, row, e->col);
                if (v) { e->col_idx = (int)(v - row->vals); e->col_epoch = t->epoch; }
            }
            if (!v) { snprintf(errmsg, errsz, "Unknown column '%s' in 'where clause'", e->col); return -1; }
            *out = *v;
            if (!v->is_null && v->s && (v->type == NLSQL_TYPE_VARCHAR || v->type == NLSQL_TYPE_TEXT || v->type == NLSQL_TYPE_DATETIME)) {
                out->s = (char*)malloc((size_t)v->len + 1);
                if (!out->s) { snprintf(errmsg, errsz, "out of memory"); return -1; }
                memcpy(out->s, v->s, v->len); out->s[v->len] = '\0';
            }
            return 0;
        }
        case EX_NEG: {
            nlsql_value_t a;
            if (eval_expr(e->a, t, row, params, nparams, out, errmsg, errsz) != 0) return -1;
            if (out->is_null) return 0;
            if (out->type == NLSQL_TYPE_INT64) out->i = -out->i;
            else if (out->type == NLSQL_TYPE_DOUBLE) out->d = -out->d;
            else { double d; value_to_double(out, &d); free(out->s); out->s = NULL; out->type = NLSQL_TYPE_DOUBLE; out->d = -d; }
            (void)a;
            return 0;
        }
        case EX_ADD: case EX_SUB: case EX_MUL: case EX_DIV: case EX_MOD: {
            nlsql_value_t a, b;
            if (eval_expr(e->a, t, row, params, nparams, &a, errmsg, errsz) != 0) return -1;
            if (eval_expr(e->b, t, row, params, nparams, &b, errmsg, errsz) != 0) { free(a.s); return -1; }
            int both_int = (a.type == NLSQL_TYPE_INT64 && !a.is_null) && (b.type == NLSQL_TYPE_INT64 && !b.is_null);
            if (both_int) {
                out->type = NLSQL_TYPE_INT64;
                switch (e->kind) {
                    case EX_ADD: out->i = a.i + b.i; break;
                    case EX_SUB: out->i = a.i - b.i; break;
                    case EX_MUL: out->i = a.i * b.i; break;
                    case EX_DIV:
                        if (b.i == 0) { out->is_null = 1; out->type = NLSQL_TYPE_NULL; }
                        else out->i = a.i / b.i;
                        break;
                    default: /* EX_MOD */
                        if (b.i == 0) { out->is_null = 1; out->type = NLSQL_TYPE_NULL; }
                        else out->i = a.i % b.i;
                        break;
                }
            } else {
                double x, y;
                value_to_double(&a, &x); value_to_double(&b, &y);
                out->type = NLSQL_TYPE_DOUBLE;
                switch (e->kind) {
                    case EX_ADD: out->d = x + y; break;
                    case EX_SUB: out->d = x - y; break;
                    case EX_MUL: out->d = x * y; break;
                    case EX_DIV: out->d = (y == 0) ? 0 : x / y; if (y == 0) { out->is_null = 1; out->type = NLSQL_TYPE_NULL; } break;
                    default: out->d = (y == 0) ? 0 : fmod(x, y); if (y == 0) { out->is_null = 1; out->type = NLSQL_TYPE_NULL; } break;
                }
            }
            free(a.s); free(b.s);
            return 0;
        }
        case EX_AGG: {
            if (!g_agg_valid || g_agg_forbidden) {
                snprintf(errmsg, errsz, "Aggregate functions are not allowed here");
                return -1;
            }
            *out = g_agg_slot;
            out->s = NULL;
            if (!g_agg_slot.is_null && g_agg_slot.s) {
                out->s = (char*)malloc((size_t)g_agg_slot.len + 1);
                if (!out->s) { snprintf(errmsg, errsz, "out of memory"); return -1; }
                memcpy(out->s, g_agg_slot.s, g_agg_slot.len);
                out->s[g_agg_slot.len] = '\0';
            }
            return 0;
        }
        default:
            snprintf(errmsg, errsz, "unsupported expression");
            return -1;
    }
}

/* =========================================
 * LIKE 匹配（% 任意串，_ 单字符）
 * ========================================= */

static int like_match(const char* s, const char* pat) {
    const char* sp = s;
    const char* pp = pat;
    const char* star_s = NULL, * star_p = NULL;
    while (*sp) {
        if (*pp == '%') { star_p = pp++; star_s = sp; }
        else if (*pp == '_' || *pp == *sp) { pp++; sp++; }
        else if (star_p) { pp = star_p + 1; sp = ++star_s; }
        else return 0;
    }
    while (*pp == '%') pp++;
    return *pp == '\0';
}

/* 值比较：返回 -1/0/1；NULL 参与比较不匹配（SQL 三值逻辑简化） */
static int cmp_values(const nlsql_value_t* a, const nlsql_value_t* b, int* rel) {
    if (a->is_null || b->is_null) return -1; /* NULL 比较不成立 */
    /* 整数快速路径（主键冲突/扫描热路径，避免 double 转换） */
    if (a->type == NLSQL_TYPE_INT64 && b->type == NLSQL_TYPE_INT64) {
        *rel = (a->i < b->i) ? -1 : (a->i > b->i) ? 1 : 0;
        return 0;
    }
    int a_num = (a->type == NLSQL_TYPE_INT64 || a->type == NLSQL_TYPE_DOUBLE);
    int b_num = (b->type == NLSQL_TYPE_INT64 || b->type == NLSQL_TYPE_DOUBLE);
    if (a_num && b_num) {
        double x, y;
        value_to_double(a, &x); value_to_double(b, &y);
        *rel = (x < y) ? -1 : (x > y) ? 1 : 0;
        return 0;
    }
    if (!a_num && !b_num) {
        int c = strcmp(a->s ? a->s : "", b->s ? b->s : "");
        *rel = (c < 0) ? -1 : (c > 0) ? 1 : 0;
        return 0;
    }
    /* 数字 vs 字符串：按 MySQL 宽松语义转数字比较 */
    {
        double x, y;
        value_to_double(a, &x); value_to_double(b, &y);
        *rel = (x < y) ? -1 : (x > y) ? 1 : 0;
        return 0;
    }
}

/* 三值逻辑：0=假 1=真 2=unknown(NULL 参与比较)。-1=求值错误。
 * 仅在 NOT/AND/OR 内部传播 unknown；顶层入口 eval_cond 把 unknown 归一为不匹配。 */
static int eval_cond_impl(cond_t* cd, table_t* t, row_t* row,
                          nlsql_value_t* params, int nparams,
                          char* errmsg, uint32_t errsz) {
    if (!cd) return 1; /* 无 WHERE 全匹配 */
    /* IS [NOT] NULL 叶子（is_not 与 op 同节点，先于 is_not 分支处理） */
    if (cd->l && !cd->a && cd->op == OP_IS_NULL) {
        nlsql_value_t l;
        if (eval_expr(cd->l, t, row, params, nparams, &l, errmsg, errsz) != 0) return -1;
        int res = l.is_null ? 1 : 0;
        free(l.s);
        return cd->is_not ? !res : res;
    }
    if (cd->is_not) {
        int r = eval_cond_impl(cd->a, t, row, params, nparams, errmsg, errsz);
        if (r < 0) return r;
        return (r == 2) ? 2 : !r;   /* NOT unknown = unknown */
    }
    if (cd->a && cd->b) { /* AND / OR 内部节点（三值传播） */
        int la = eval_cond_impl(cd->a, t, row, params, nparams, errmsg, errsz);
        if (la < 0) return la;
        int lb = eval_cond_impl(cd->b, t, row, params, nparams, errmsg, errsz);
        if (lb < 0) return lb;
        if (cd->is_and) {
            if (la == 0 || lb == 0) return 0;
            return (la == 1 && lb == 1) ? 1 : 2;
        }
        if (la == 1 || lb == 1) return 1;
        return (la == 0 && lb == 0) ? 0 : 2;
    }
    /* 叶子比较 */
    nlsql_value_t l, r;
    if (eval_expr(cd->l, t, row, params, nparams, &l, errmsg, errsz) != 0) return -1;
    if (eval_expr(cd->r, t, row, params, nparams, &r, errmsg, errsz) != 0) { free(l.s); return -1; }
    int result = 0;
    if (cd->op == OP_LIKE || cd->op == OP_NOT_LIKE) {
        if (l.is_null || r.is_null) result = 2;             /* NULL LIKE x = unknown */
        else {
            int m = (l.s && r.s) ? like_match(l.s, r.s) : 0;
            result = (cd->op == OP_LIKE) ? m : !m;
        }
    } else if (cd->op == OP_EQ && (l.is_null || r.is_null)) {
        result = (l.is_null && r.is_null); /* NULL = NULL 兼容简化 */
    } else if (l.is_null || r.is_null) {
        result = 2;                                          /* NULL 参与比较 = unknown */
    } else {
        int rel;
        if (cmp_values(&l, &r, &rel) != 0) result = 0;
        else switch (cd->op) {
            case OP_EQ: result = rel == 0; break;
            case OP_NE: result = rel != 0; break;
            case OP_LT: result = rel < 0;  break;
            case OP_LE: result = rel <= 0; break;
            case OP_GT: result = rel > 0;  break;
            default:    result = rel >= 0; break;
        }
    }
    free(l.s); free(r.s);
    return result;
}

/* 顶层入口：unknown → 不匹配（与 MySQL WHERE 过滤 NULL 行一致） */
static int eval_cond(cond_t* cd, table_t* t, row_t* row,
                     nlsql_value_t* params, int nparams,
                     char* errmsg, uint32_t errsz) {
    int r = eval_cond_impl(cd, t, row, params, nparams, errmsg, errsz);
    return (r == 2) ? 0 : r;
}

/* =========================================
 * SELECT
 * ========================================= */

static int col_index(table_t* t, const char* name) {
    const char* dot = strchr(name, '.');
    const char* nm = dot ? dot + 1 : name;
    for (int i = 0; i < t->col_count; i++)
        if (strcmp(t->cols[i].name, nm) == 0) return i;
    return -1;
}

/* 排序键：预解析排序列索引（避免每次比较都 strcmp 列表名） */
typedef struct {
    int            n;
    int*           idx;   /* <0 表示列不存在，比较时跳过 */
    unsigned char* desc;
} sort_key_t;

static int sort_key_init(sort_key_t* k, table_t* t, order_by_t* order, int oc) {
    memset(k, 0, sizeof(*k));
    if (oc <= 0) return 0;
    k->idx = (int*)malloc((size_t)oc * sizeof(int));
    k->desc = (unsigned char*)malloc((size_t)oc);
    if (!k->idx || !k->desc) { free(k->idx); free(k->desc); memset(k, 0, sizeof(*k)); return -1; }
    for (int i = 0; i < oc; i++) {
        k->idx[i] = col_index(t, order[i].col);
        k->desc[i] = order[i].desc ? 1 : 0;
    }
    k->n = oc;
    return 0;
}

static void sort_key_free(sort_key_t* k) {
    free(k->idx); free(k->desc);
    memset(k, 0, sizeof(*k));
}

/* 行比较（按排序键列表） */
static int row_cmp_key(const void* x, const void* y, void* ctxv) {
    const sort_key_t* k = (const sort_key_t*)ctxv;
    const row_t* a = (const row_t*)x; const row_t* b = (const row_t*)y;
    for (int i = 0; i < k->n; i++) {
        int idx = k->idx[i];
        if (idx < 0) continue;
        int rel;
        nlsql_value_t va = a->vals[idx], vb = b->vals[idx];
        if (va.is_null && vb.is_null) rel = 0;
        else if (va.is_null) rel = -1;
        else if (vb.is_null) rel = 1;
        else if (cmp_values(&va, &vb, &rel) != 0) rel = 0;
        if (rel != 0) return k->desc[i] ? -rel : rel;
    }
    return 0;
}

/* 行指针数组是否已按排序键非降序（如 ORDER BY 主键且插入顺序有序 → 可跳过排序与拷贝） */
static int rows_sorted_key(sort_key_t* k, row_t** rows, uint64_t n) {
    for (uint64_t i = 1; i < n; i++)
        if (row_cmp_key(rows[i - 1], rows[i], k) > 0) return 0;
    return 1;
}

/* 归并/插入排序（稳定，可移植） */
static void rows_sort(sort_key_t* k, row_t* rows, uint64_t n) {
    /* 小规模 n：直接插入排序；大规模：归并排序 */
    if (n <= 64) {
        for (uint64_t i = 1; i < n; i++) {
            row_t key = rows[i];
            int64_t j = (int64_t)i - 1;
            while (j >= 0) {
                if (row_cmp_key(&rows[j], &key, k) <= 0) break;
                rows[j + 1] = rows[j];
                j--;
            }
            rows[j + 1] = key;
        }
        return;
    }
    /* 归并 */
    row_t* tmp = (row_t*)malloc((size_t)n * sizeof(row_t));
    if (!tmp) return;
    for (uint64_t width = 1; width < n; width *= 2) {
        for (uint64_t i = 0; i < n; i += 2 * width) {
            uint64_t lo = i;
            uint64_t mid = i + width < n ? i + width : n;
            uint64_t hi = i + 2 * width < n ? i + 2 * width : n;
            uint64_t a = lo, b = mid, kk = lo;
            while (a < mid && b < hi) {
                if (row_cmp_key(&rows[a], &rows[b], k) <= 0) tmp[kk++] = rows[a++];
                else tmp[kk++] = rows[b++];
            }
            while (a < mid) tmp[kk++] = rows[a++];
            while (b < hi) tmp[kk++] = rows[b++];
        }
        memcpy(rows + 0, tmp, (size_t)n * sizeof(row_t));
    }
    free(tmp);
}

/* 堆选前 k 小（按排序键）：O(n log k)，用于 ORDER BY + LIMIT 的 top-N 剪枝。
   out 需能容纳 k 个 row_t；返回实际写入个数并就地排好序。 */
static void heap_sift_up(sort_key_t* k, row_t* h, uint64_t i) {
    while (i > 0) {
        uint64_t p = (i - 1) / 2;
        if (row_cmp_key(&h[p], &h[i], k) >= 0) break;
        row_t t = h[p]; h[p] = h[i]; h[i] = t;
        i = p;
    }
}

static void heap_sift_down(sort_key_t* k, row_t* h, uint64_t n, uint64_t i) {
    for (;;) {
        uint64_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && row_cmp_key(&h[l], &h[m], k) > 0) m = l;
        if (r < n && row_cmp_key(&h[r], &h[m], k) > 0) m = r;
        if (m == i) break;
        row_t t = h[m]; h[m] = h[i]; h[i] = t;
        i = m;
    }
}

static uint64_t rows_topn(sort_key_t* k, row_t** src, uint64_t n, uint64_t want, row_t* out) {
    uint64_t cnt = 0;
    if (want == 0) return 0;
    for (uint64_t i = 0; i < n; i++) {
        if (cnt < want) {
            out[cnt] = *src[i];
            heap_sift_up(k, out, cnt);
            cnt++;
        } else if (row_cmp_key(src[i], &out[0], k) < 0) {
            out[0] = *src[i];      /* 比堆顶（当前第 want 小）更小 → 替换并下沉 */
            heap_sift_down(k, out, cnt, 0);
        }
    }
    rows_sort(k, out, cnt);
    return cnt;
}

/* 聚合默认列名：COUNT(*) / SUM(v) */
static void agg_display_name(expr_t* ag, char* buf, uint32_t sz) {
    const char* fn = (ag->i == AGG_COUNT) ? "COUNT" : (ag->i == AGG_SUM) ? "SUM" :
                     (ag->i == AGG_AVG) ? "AVG" : (ag->i == AGG_MIN) ? "MIN" : "MAX";
    if (!ag->a) { snprintf(buf, sz, "%s(*)", fn); return; }
    if (ag->a->kind == EX_COL && ag->a->col) snprintf(buf, sz, "%s(%s)", fn, ag->a->col);
    else snprintf(buf, sz, "%s(...)", fn);
}

/* 计算一个组的聚合值（rows 为组内行指针；n=0 → 空组：COUNT=0 其余 NULL） */
static int agg_compute(expr_t* ag, table_t* t, row_t** rows, uint64_t n,
                       nlsql_value_t* params, int nparams,
                       nlsql_value_t* out, char* errmsg, uint32_t errsz) {
    memset(out, 0, sizeof(*out));
    agg_kind_t ak = (agg_kind_t)ag->i;
    if (ak == AGG_COUNT && !ag->a) {           /* COUNT(*) */
        out->type = NLSQL_TYPE_INT64; out->i = (int64_t)n;
        return 0;
    }
    if (ak == AGG_COUNT) {
        int64_t cnt = 0;
        for (uint64_t k = 0; k < n; k++) {
            nlsql_value_t v;
            memset(&v, 0, sizeof(v));
            if (eval_expr(ag->a, t, rows[k], params, nparams, &v, errmsg, errsz) != 0) { free(v.s); return -1; }
            if (!v.is_null) cnt++;
            free(v.s);
        }
        out->type = NLSQL_TYPE_INT64; out->i = cnt;
        return 0;
    }
    if (ak == AGG_SUM || ak == AGG_AVG) {
        int all_int = 1, seen = 0;
        int64_t isum = 0;
        double dsum = 0;
        for (uint64_t k = 0; k < n; k++) {
            nlsql_value_t v;
            memset(&v, 0, sizeof(v));
            if (eval_expr(ag->a, t, rows[k], params, nparams, &v, errmsg, errsz) != 0) { free(v.s); return -1; }
            if (!v.is_null) {
                seen++;
                if (v.type == NLSQL_TYPE_INT64) { isum += v.i; dsum += (double)v.i; }
                else { double d; value_to_double(&v, &d); all_int = 0; dsum += d; }
            }
            free(v.s);
        }
        if (!seen) { out->type = NLSQL_TYPE_NULL; out->is_null = 1; return 0; }
        if (ak == AGG_AVG) { out->type = NLSQL_TYPE_DOUBLE; out->d = dsum / (double)seen; }
        else if (all_int) { out->type = NLSQL_TYPE_INT64; out->i = isum; }
        else { out->type = NLSQL_TYPE_DOUBLE; out->d = dsum; }
        return 0;
    }
    /* MIN / MAX：比较取优 */
    nlsql_value_t best;
    memset(&best, 0, sizeof(best));
    int seen = 0;
    for (uint64_t k = 0; k < n; k++) {
        nlsql_value_t v;
        memset(&v, 0, sizeof(v));
        if (eval_expr(ag->a, t, rows[k], params, nparams, &v, errmsg, errsz) != 0) {
            free(v.s); free(best.s); return -1;
        }
        if (!v.is_null) {
            if (!seen) { best = v; v.s = NULL; seen = 1; }
            else {
                int rel = 0;
                if (cmp_values(&v, &best, &rel) == 0) {
                    int better = (ak == AGG_MIN) ? (rel < 0) : (rel > 0);
                    if (better) { free(best.s); best = v; v.s = NULL; }
                }
            }
        }
        free(v.s);
    }
    if (!seen) { free(best.s); out->type = NLSQL_TYPE_NULL; out->is_null = 1; return 0; }
    *out = best;
    return 0;
}

/* 两行值是否完全相等（NULL==NULL） */
static int row_vals_equal(const nlsql_value_t* a, const nlsql_value_t* b, int ncols) {
    for (int i = 0; i < ncols; i++) {
        if (a[i].is_null && b[i].is_null) continue;
        if (a[i].is_null || b[i].is_null) return 0;
        int rel = 0;
        if (cmp_values(&a[i], &b[i], &rel) != 0 || rel != 0) return 0;
    }
    return 1;
}

/* DISTINCT 去重哈希表：开放寻址，槽存 row 索引 +1（0 = 空）。
   早期实现逐行线性扫描已保留结果，复杂度 O(n × 唯一值数)；哈希后接近 O(n)。 */
typedef struct {
    uint64_t* slots;
    uint64_t  mask;
    uint64_t  used;
} dist_t;

static uint64_t val_hash(const nlsql_value_t* v) {
    uint64_t h = 1469598103934665603ull; /* FNV-1a offset */
#define H_MIX(x) do { h ^= (uint64_t)(x); h *= 1099511628211ull; } while (0)
    H_MIX(v->type);
    H_MIX(v->is_null ? 1u : 0u);
    if (!v->is_null) {
        if (v->type == NLSQL_TYPE_INT64) {
            H_MIX(v->i);
        } else if (v->type == NLSQL_TYPE_DOUBLE) {
            double d = (v->d == 0.0) ? 0.0 : v->d; /* -0.0 与 0.0 视为同值 */
            uint64_t bits; memcpy(&bits, &d, sizeof(bits));
            H_MIX(bits);
        } else {
            uint32_t n = v->s ? v->len : 0;
            for (uint32_t i = 0; i < n; i++) H_MIX((unsigned char)v->s[i]);
            H_MIX((uint64_t)n);
        }
    }
#undef H_MIX
    return h;
}

static uint64_t row_hash(const nlsql_value_t* vals, int ncols) {
    uint64_t h = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < ncols; i++) {
        h ^= val_hash(&vals[i]) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h *= 0xff51afd7ed558ccdull;
    }
    return h;
}

static void dist_free(dist_t* d) { free(d->slots); d->slots = NULL; d->mask = 0; d->used = 0; }

static int dist_init(dist_t* d, uint64_t hint) {
    uint64_t cap = 16;
    while (cap < hint * 2 + 1 && cap < (1ull << 30)) cap <<= 1;
    d->slots = (uint64_t*)calloc(cap, sizeof(uint64_t));
    if (!d->slots) return -1;
    d->mask = cap - 1; d->used = 0;
    return 0;
}

static int dist_grow(dist_t* d, nlsql_result_t* res, int ncols) {
    uint64_t oldcap = d->mask + 1, newcap = oldcap * 2;
    uint64_t* ns = (uint64_t*)calloc(newcap, sizeof(uint64_t));
    if (!ns) return -1;
    uint64_t nmask = newcap - 1;
    for (uint64_t i = 0; i < oldcap; i++) {
        uint64_t v = d->slots[i];
        if (!v) continue;
        uint64_t ridx = v - 1;
        uint64_t h = (ridx < res->row_count) ? row_hash(res->rows[ridx].vals, ncols) : 0;
        uint64_t pos = h & nmask;
        while (ns[pos]) pos = (pos + 1) & nmask;
        ns[pos] = v;
    }
    free(d->slots);
    d->slots = ns; d->mask = nmask;
    return 0;
}

/* 返回 1=重复（未插入）；0=新值（已插入 rowidx）；-1=内存不足 */
static int dist_check_add(dist_t* d, nlsql_result_t* res, const nlsql_value_t* vals,
                          int ncols, uint64_t rowidx) {
    uint64_t h = row_hash(vals, ncols);
    uint64_t pos = h & d->mask;
    while (d->slots[pos]) {
        uint64_t ridx = d->slots[pos] - 1;
        if (ridx < res->row_count && row_vals_equal(res->rows[ridx].vals, vals, ncols)) return 1;
        pos = (pos + 1) & d->mask;
    }
    d->slots[pos] = rowidx + 1;
    d->used++;
    if (d->used * 10 > (d->mask + 1) * 7) return dist_grow(d, res, ncols); /* 装载 >0.7：扩容 */
    return 0;
}

/* 窗口裁剪（DISTINCT 先全量再去窗口）：裁掉行只释放值字符串，vals 由 arena 统一管理 */
static void res_trim(nlsql_result_t* res, uint64_t start, uint64_t cnt) {
    uint64_t m = res->row_count;
    if (start > m) start = m;
    if (cnt > m - start) cnt = m - start;
    for (uint64_t i = 0; i < start; i++)
        if (res->rows[i].vals)
            for (int k = 0; k < res->col_count; k++) free(res->rows[i].vals[k].s);
    for (uint64_t i = start + cnt; i < m; i++)
        if (res->rows[i].vals)
            for (int k = 0; k < res->col_count; k++) free(res->rows[i].vals[k].s);
    if (start) memmove(res->rows, res->rows + start, (size_t)cnt * sizeof(nlsql_row_t));
    res->row_count = cnt;
}

/* 表达式是否引用列（快速路径的值侧必须为纯常量/参数） */
static int expr_has_col(expr_t* e) {
    if (!e) return 0;
    if (e->kind == EX_COL) return 1;
    return expr_has_col(e->a) || expr_has_col(e->b);
}

/* 在表达式树中查找首个 EX_AGG 节点 */
static expr_t* expr_find_agg(expr_t* e) {
    if (!e) return NULL;
    if (e->kind == EX_AGG) return e;
    expr_t* f = expr_find_agg(e->a);
    return f ? f : expr_find_agg(e->b);
}

/* 在条件树中查找首个 EX_AGG 节点（HAVING 单独携带聚合时用） */
static expr_t* cond_find_agg(cond_t* c) {
    if (!c) return NULL;
    if (c->l) {
        expr_t* f = expr_find_agg(c->l);
        if (f) return f;
        return c->r ? expr_find_agg(c->r) : NULL;
    }
    expr_t* f = cond_find_agg(c->a);
    return f ? f : cond_find_agg(c->b);
}

/* 在 WHERE 的 AND 链中寻找 pk = <纯值> 叶子（点查 O(1) 快速路径入口） */
static int find_pk_eq(cond_t* c, table_t* t, expr_t** val_side) {
    if (!c) return 0;
    if (c->l && c->r) { /* 叶子 */
        if (c->op != OP_EQ || c->is_not) return 0;
        int lcol = (c->l->kind == EX_COL), rcol = (c->r->kind == EX_COL);
        if (lcol == rcol) return 0;
        expr_t* col = lcol ? c->l : c->r;
        expr_t* val = lcol ? c->r : c->l;
        if (expr_has_col(val)) return 0;
        const char* nm = col->col ? strchr(col->col, '.') : NULL;
        nm = nm ? nm + 1 : col->col;
        if (t->pk_col >= 0 && strcmp(t->cols[t->pk_col].name, nm) == 0) {
            *val_side = val;
            return 1;
        }
        return 0;
    }
    if (c->is_and) return find_pk_eq(c->a, t, val_side) || find_pk_eq(c->b, t, val_side);
    return 0;
}

/* 在 WHERE 的 AND 链中寻找「二级索引单列 = 纯值」叶子（索引等值加速入口） */
static int find_sec_eq(cond_t* c, table_t* t, tbl_index_t** ix_out, expr_t** val_side) {
    if (!c) return 0;
    if (c->l && c->r) {
        if (c->op != OP_EQ || c->is_not) return 0;
        int lcol = (c->l->kind == EX_COL), rcol = (c->r->kind == EX_COL);
        if (lcol == rcol) return 0;
        expr_t* col = lcol ? c->l : c->r;
        expr_t* val = lcol ? c->r : c->l;
        if (expr_has_col(val)) return 0;
        const char* nm = col->col ? strchr(col->col, '.') : NULL;
        nm = nm ? nm + 1 : col->col;
        int ci = col_index(t, nm);
        if (ci < 0) return 0;
        for (tbl_index_t* ix = t->indexes; ix; ix = ix->next)
            if (ix->ncols == 1 && ix->cols[0] == ci) { *ix_out = ix; *val_side = val; return 1; }
        return 0;
    }
    if (c->is_and) return find_sec_eq(c->a, t, ix_out, val_side) || find_sec_eq(c->b, t, ix_out, val_side);
    return 0;
}

static int exec_select(nlsql_conn_t* c, engine_t* e, select_stmt_t* sel,
                       nlsql_value_t* params, int nparams, nlsql_result_t** out) {
    table_t* t = nlsql_tbl_find(e, sel->table);
    if (!t) {
        nlsql_set_err(c, NLSQL_ER_NO_SUCH_TABLE, "42S02",
                      "Table '%s.%s' doesn't exist", e->cur_db->name, sel->table);
        return NLSQL_ER_NO_SUCH_TABLE;
    }
    g_agg_valid = 0;
    memset(&g_agg_slot, 0, sizeof(g_agg_slot));
    /* 收集匹配行索引（WHERE 中禁止聚合）
     * 主键点查快速路径：AND 链中含 pk = 纯值 → 哈希 O(1) 定位，
     * 命中行仍用完整 WHERE 校验（其余条件兜底）；不适用则回退全表扫描 */
    uint64_t cap = 64, n = 0;
    row_t** idx = (row_t**)malloc(cap * sizeof(row_t*));
    if (!idx) return NLSQL_ERR;
    g_agg_forbidden = 1;
    int scan_done = 0;
    expr_t* pk_val = NULL;
    if (sel->where && nlsql_pk_ensure(t) == NLSQL_OK && find_pk_eq(sel->where, t, &pk_val)) {
        nlsql_value_t kv;
        char emsg[128] = {0};
        memset(&kv, 0, sizeof(kv));
        if (eval_expr(pk_val, t, NULL, params, nparams, &kv, emsg, sizeof(emsg)) == 0) {
            uint64_t hit = 0;
            if (!kv.is_null && nlsql_pk_lookup(t, &kv, &hit) && hit < t->row_count) {
                int m = eval_cond(sel->where, t, &t->rows[hit], params, nparams, emsg, sizeof(emsg));
                if (m < 0) {
                    free(kv.s); free(idx);
                    g_agg_forbidden = 0;
                    if (strstr(emsg, "Aggregate"))
                        nlsql_set_err(c, NLSQL_ER_NOT_SUPPORTED, "42000", "%s", emsg);
                    else
                        nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22", "%s", emsg);
                    return strstr(emsg, "Aggregate") ? NLSQL_ER_NOT_SUPPORTED : NLSQL_ER_UNKNOWN_COLUMN;
                }
                if (m) idx[n++] = &t->rows[hit];
            }
            free(kv.s);
            scan_done = 1; /* 未命中也确定无匹配行，无需扫描 */
        }
        /* 值侧求值失败（如参数缺失）→ 回退全表，由循环统一报错 */
    }
    /* 二级索引等值快速路径：命中行仍用完整 WHERE 校验 */
    if (!scan_done && sel->where && t->indexes) {
        tbl_index_t* six = NULL;
        expr_t* sval = NULL;
        if (find_sec_eq(sel->where, t, &six, &sval)) {
            nlsql_value_t kv;
            char emsg[128] = {0};
            memset(&kv, 0, sizeof(kv));
            if (eval_expr(sval, t, NULL, params, nparams, &kv, emsg, sizeof(emsg)) == 0) {
                if (!kv.is_null) {
                    uint64_t* hits = NULL, cnt = 0;
                    if (nlsql_idx_lookup(t, six, &kv, &hits, &cnt) == NLSQL_OK) {
                        for (uint64_t k = 0; k < cnt; k++) {
                            if (hits[k] >= t->row_count) continue;
                            int m = eval_cond(sel->where, t, &t->rows[hits[k]], params, nparams,
                                              emsg, sizeof(emsg));
                            if (m < 0) {
                                free(hits); free(kv.s); free(idx);
                                g_agg_forbidden = 0;
                                nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22", "%s", emsg);
                                return NLSQL_ER_UNKNOWN_COLUMN;
                            }
                            if (m) {
                                if (n == cap) {
                                    cap *= 2;
                                    row_t** ni = (row_t**)realloc(idx, (size_t)cap * sizeof(row_t*));
                                    if (!ni) { free(hits); free(kv.s); free(idx); g_agg_forbidden = 0; return NLSQL_ERR; }
                                    idx = ni;
                                }
                                idx[n++] = &t->rows[hits[k]];
                            }
                        }
                        free(hits);
                        scan_done = 1;
                    }
                }
                free(kv.s);
            }
        }
    }
    if (!scan_done && !sel->where) {
        /* 无 WHERE 快速收集（全表匹配，免逐行条件求值调用） */
        for (uint64_t r = 0; r < t->row_count; r++) {
            if (n == cap) {
                cap *= 2;
                row_t** ni = (row_t**)realloc(idx, (size_t)cap * sizeof(row_t*));
                if (!ni) { free(idx); g_agg_forbidden = 0; return NLSQL_ERR; }
                idx = ni;
            }
            idx[n++] = &t->rows[r];
        }
    } else if (!scan_done) for (uint64_t r = 0; r < t->row_count; r++) {
        char emsg[128];
        int m = eval_cond(sel->where, t, &t->rows[r], params, nparams, emsg, sizeof(emsg));
        if (m < 0) {
            free(idx);
            g_agg_forbidden = 0;
            if (strstr(emsg, "Aggregate"))
                nlsql_set_err(c, NLSQL_ER_NOT_SUPPORTED, "42000", "%s", emsg);
            else
                nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22", "%s", emsg);
            return strstr(emsg, "Aggregate") ? NLSQL_ER_NOT_SUPPORTED : NLSQL_ER_UNKNOWN_COLUMN;
        }
        if (m) {
            if (n == cap) {
                cap *= 2;
                row_t** ni = (row_t**)realloc(idx, (size_t)cap * sizeof(row_t*));
                if (!ni) { free(idx); g_agg_forbidden = 0; return NLSQL_ERR; }
                idx = ni;
            }
            idx[n++] = &t->rows[r];
        }
    }
    g_agg_forbidden = 0;

    /* 输出列（普通列 → cidx） */
    int* cidx = NULL;
    int plain_ncols = 0;
    int star = !sel->cols || sel->col_count == 0 || sel->cols[0][0] == '\0';
    if (!star) {
        plain_ncols = sel->col_count;
        cidx = (int*)malloc((size_t)plain_ncols * sizeof(int));
        if (!cidx) { free(idx); return NLSQL_ERR; }
        for (int i = 0; i < plain_ncols; i++) {
            cidx[i] = col_index(t, sel->cols[i]);
            if (cidx[i] < 0) {
                free(cidx); free(idx);
                nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22",
                              "Unknown column '%s' in 'field list'", sel->cols[i]);
                return NLSQL_ER_UNKNOWN_COLUMN;
            }
        }
    } else {
        /* 星号：仅“聚合且无 GROUP BY”输出 0 个普通列，否则全部列 */
        plain_ncols = (sel->agg && !sel->has_group) ? 0 : t->col_count;
        cidx = (int*)malloc((size_t)(plain_ncols ? plain_ncols : 1) * sizeof(int));
        if (!cidx) { free(idx); return NLSQL_ERR; }
        for (int i = 0; i < plain_ncols; i++) cidx[i] = i;
    }

    /* LIMIT 窗口 */
    uint64_t start = sel->has_offset ? (uint64_t)sel->offset : 0;
    uint64_t lim_cnt = UINT64_MAX;
    if (sel->has_limit) lim_cnt = (uint64_t)sel->limit;

    /* ============ 聚合 / GROUP BY / HAVING 路径 ============ */
    if (sel->agg || sel->has_group || sel->having) {
        int gidx = -1;
        if (sel->has_group) {
            gidx = col_index(t, sel->group);
            if (gidx < 0) {
                free(cidx); free(idx);
                nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22",
                              "Unknown column '%s' in 'group statement'", sel->group);
                return NLSQL_ER_UNKNOWN_COLUMN;
            }
        }
        /* 按分组列排序 → 连续段分组；无 GROUP BY → 全部行为一组 */
        row_t* rows_copy = NULL;
        row_t** mem = NULL;      /* 组成员指针数组（排序后顺序） */
        uint64_t* gstart = NULL;
        uint64_t gcnt = 0;
        mem = (row_t**)malloc((n ? n : 1) * sizeof(row_t*));
        if (!mem) { free(cidx); free(idx); return NLSQL_ERR; }
        if (sel->has_group && n > 0) {
            order_by_t go;
            memset(&go, 0, sizeof(go));
            snprintf(go.col, sizeof(go.col), "%s", sel->group);
            sort_key_t gkey;
            if (sort_key_init(&gkey, t, &go, 1) != 0) { free(mem); free(cidx); free(idx); return NLSQL_ERR; }
            if (rows_sorted_key(&gkey, idx, n)) {
                /* 输入已按分组列有序（常见：GROUP BY 主键/自增列）：直接复用扫描顺序，免拷贝免排序 */
                for (uint64_t i = 0; i < n; i++) mem[i] = idx[i];
            } else {
                rows_copy = (row_t*)malloc(n * sizeof(row_t));
                if (!rows_copy) { sort_key_free(&gkey); free(mem); free(cidx); free(idx); return NLSQL_ERR; }
                for (uint64_t i = 0; i < n; i++) rows_copy[i] = *idx[i];
                rows_sort(&gkey, rows_copy, n);
                for (uint64_t i = 0; i < n; i++) mem[i] = &rows_copy[i];
            }
            sort_key_free(&gkey);
            gstart = (uint64_t*)malloc((n + 1) * sizeof(uint64_t));
            if (!gstart) { free(rows_copy); free(mem); free(cidx); free(idx); return NLSQL_ERR; }
            uint64_t s = 0;
            for (uint64_t i = 1; i <= n; i++) {
                int same = 0;
                if (i < n) {
                    const nlsql_value_t* a = &mem[s]->vals[gidx];
                    const nlsql_value_t* b = &mem[i]->vals[gidx];
                    if (a->is_null && b->is_null) same = 1;
                    else if (!a->is_null && !b->is_null) { int rel = 0; same = cmp_values(a, b, &rel) == 0 && rel == 0; }
                }
                if (!i) continue;
                if (i < n && same) continue;
                gstart[gcnt++] = s;
                s = i;
            }
            gstart[gcnt] = n; /* 哨兵 */
        } else if (sel->has_group) {
            /* GROUP BY 无匹配行 → 0 组（与 MySQL 一致，不输出行） */
            gstart = (uint64_t*)malloc(2 * sizeof(uint64_t));
            if (!gstart) { free(mem); free(cidx); free(idx); return NLSQL_ERR; }
            gstart[0] = 0; gstart[1] = 0;
        } else {
            for (uint64_t i = 0; i < n; i++) mem[i] = idx[i];
            if (n == 0) mem[0] = NULL; /* 空表无 GROUP BY：组代表行置 NULL，后续 reps/​HAVING 防护 */
            gcnt = 1;
            gstart = (uint64_t*)malloc(2 * sizeof(uint64_t));
            if (!gstart) { free(mem); free(cidx); free(idx); return NLSQL_ERR; }
            gstart[0] = 0; gstart[1] = n;
        }

        /* 组行指针 + 组聚合 + HAVING 过滤 */
        /* 聚合规格：SELECT 聚合优先；仅 HAVING 带聚合（如 SELECT g ... HAVING COUNT(*)>2）
           时从 having 树提取（够用版：单聚合，不额外输出列） */
        expr_t* agg_spec = sel->agg;
        if (!agg_spec && sel->having) agg_spec = cond_find_agg(sel->having);
        row_t** reps = (row_t**)malloc((gcnt ? gcnt : 1) * sizeof(row_t*));
        nlsql_value_t* aggvals = (nlsql_value_t*)malloc((gcnt ? gcnt : 1) * sizeof(nlsql_value_t));
        if (!reps || !aggvals) { free(reps); free(aggvals); free(gstart); free(mem); free(rows_copy); free(cidx); free(idx); return NLSQL_ERR; }
        uint64_t kept = 0;
        for (uint64_t g = 0; g < gcnt; g++) {
            uint64_t gs = gstart[g], ge = gstart[g + 1];
            row_t** members = mem + gs;
            nlsql_value_t av;
            memset(&av, 0, sizeof(av));
            if (agg_spec) {
                if (agg_compute(agg_spec, t, members, ge - gs, params, nparams, &av,
                                (char[128]){0}, 128) != 0) {
                    for (uint64_t k = 0; k < kept; k++) free(aggvals[k].s);
                    free(reps); free(aggvals); free(gstart); free(mem); free(rows_copy); free(cidx); free(idx);
                    nlsql_set_err(c, NLSQL_ER_NOT_SUPPORTED, "42000", "aggregate computation failed");
                    return NLSQL_ER_NOT_SUPPORTED;
                }
            }
            /* HAVING（组代表行求值；聚合经槽读取，槽只读不夺权） */
            int pass = 1;
            if (sel->having) {
                row_t* rep = mem[gs];
                if (!rep) {
                    pass = 0; /* 空组（无匹配行）：无代表行可求值，直接不通过 */
                } else {
                g_agg_slot = av;
                g_agg_valid = 1;
                char emsg[128] = {0};
                int m = eval_cond(sel->having, t, rep, params, nparams, emsg, sizeof(emsg));
                g_agg_valid = 0;
                if (m < 0) {
                    free(av.s);
                    for (uint64_t k = 0; k < kept; k++) free(aggvals[k].s);
                    free(reps); free(aggvals); free(gstart); free(mem); free(rows_copy); free(cidx); free(idx);
                    nlsql_set_err(c, NLSQL_ER_NOT_SUPPORTED, "42000", "%s", emsg);
                    return NLSQL_ER_NOT_SUPPORTED;
                }
                pass = m;
                }
            }
            if (pass) {
                reps[kept] = mem[gs];
                aggvals[kept] = av;
                kept++;
            } else {
                free(av.s);
            }
        }
        free(gstart);

        /* ORDER BY：对组代表行排序（并行搬动聚合值；预解析排序键，避免每次比较 strcmp 列名） */
        if (sel->order_count > 0 && kept > 1 && reps[0]) {
            sort_key_t okey;
            if (sort_key_init(&okey, t, sel->order, sel->order_count) != 0) {
                for (uint64_t k = 0; k < kept; k++) free(aggvals[k].s);
                free(reps); free(aggvals); free(mem); free(rows_copy); free(cidx); free(idx);
                return NLSQL_ERR;
            }
            for (uint64_t i = 1; i < kept; i++) {
                row_t* krep = reps[i];
                nlsql_value_t kav = aggvals[i];
                int64_t j = (int64_t)i - 1;
                while (j >= 0) {
                    if (row_cmp_key(reps[j], krep, &okey) <= 0) break;
                    reps[j + 1] = reps[j];
                    aggvals[j + 1] = aggvals[j];
                    j--;
                }
                reps[j + 1] = krep;
                aggvals[j + 1] = kav;
            }
            sort_key_free(&okey);
        }

        /* 裁窗口 */
        if (start > kept) start = kept;
        uint64_t cnt = (lim_cnt < kept - start) ? lim_cnt : kept - start;

        int ncols = plain_ncols + (sel->agg ? 1 : 0);
        nlsql_result_t* res = res_new(ncols);
        if (!res) {
            for (uint64_t k = 0; k < kept; k++) free(aggvals[k].s);
            free(reps); free(aggvals); free(mem); free(rows_copy); free(cidx); free(idx);
            return NLSQL_ERR;
        }
        dist_t dist;
        memset(&dist, 0, sizeof(dist));
        if (sel->distinct && dist_init(&dist, cnt < 4096 ? cnt : 4096) != 0) goto agg_oom;
        for (int i = 0; i < plain_ncols; i++) {
            const char* nm = t->cols[cidx[i]].name;
            if (sel->col_alias && sel->col_alias[i][0]) nm = sel->col_alias[i];
            snprintf(res->cols[i].name, sizeof(res->cols[i].name), "%s", nm);
            res->cols[i].type = t->cols[cidx[i]].type;
        }
        if (sel->agg) {
            char dn[65];
            const char* nm = sel->agg_alias[0] ? sel->agg_alias : NULL;
            if (!nm) { agg_display_name(sel->agg, dn, sizeof(dn)); nm = dn; }
            snprintf(res->cols[plain_ncols].name, sizeof(res->cols[plain_ncols].name), "%s", nm);
            res->cols[plain_ncols].type =
                (sel->agg->i == AGG_COUNT) ? NLSQL_TYPE_INT64 : NLSQL_TYPE_NULL;
        }
        for (uint64_t r = start; r < start + cnt; r++) {
            if (res_add_row(res, ncols) != 0) goto agg_oom;
            nlsql_value_t* dst = res->rows[res->row_count - 1].vals;
            for (int i = 0; i < plain_ncols; i++) {
                dst[i].is_null = 1; dst[i].type = NLSQL_TYPE_NULL; dst[i].s = NULL;
                if (reps[r]) {
                    const nlsql_value_t* v = &reps[r]->vals[cidx[i]];
                    dst[i] = *v;
                    dst[i].s = NULL;
                    if (!v->is_null && v->s &&
                        (v->type == NLSQL_TYPE_VARCHAR || v->type == NLSQL_TYPE_TEXT || v->type == NLSQL_TYPE_DATETIME)) {
                        dst[i].s = (char*)malloc((size_t)v->len + 1);
                        if (dst[i].s) { memcpy(dst[i].s, v->s, v->len); dst[i].s[v->len] = '\0'; dst[i].len = v->len; }
                        else { dst[i].is_null = 1; dst[i].type = NLSQL_TYPE_NULL; }
                    }
                }
            }
            if (sel->agg) {
                dst[plain_ncols] = aggvals[r];
                aggvals[r].s = NULL;
                if (sel->agg->i == AGG_COUNT || !dst[plain_ncols].is_null)
                    res->cols[plain_ncols].type = dst[plain_ncols].type;
            }
            if (sel->distinct) {
                int dup = dist_check_add(&dist, res, dst, ncols, res->row_count - 1);
                if (dup < 0) goto agg_oom;
                if (dup) {
                    if (sel->agg) free(dst[plain_ncols].s); /* 仅存在聚合列时该 slot 才被赋值（否则越界读垃圾指针） */
                    for (int i = 0; i < plain_ncols; i++) free(dst[i].s);
                    res->row_count--; /* vals 归 arena，无需逐行释放 */
                }
            }
        }
        dist_free(&dist);
        for (uint64_t k = 0; k < kept; k++) free(aggvals[k].s);
        free(reps); free(aggvals); free(mem); free(rows_copy); free(cidx); free(idx);
        *out = res;
        return NLSQL_OK;
    agg_oom:
        dist_free(&dist);
        for (uint64_t k = 0; k < kept; k++) free(aggvals[k].s);
        free(reps); free(aggvals); free(mem); free(rows_copy); free(cidx); free(idx);
        nlsql_free_result(res);
        return NLSQL_ERR;
    }

    /* ============ 普通路径 ============ */
    int ncols = plain_ncols;
    /* ORDER BY：① 输入已有序 → 免拷贝免排序；② 有 LIMIT（非 DISTINCT）→ 堆选前 N；③ 否则全量排序 */
    row_t* rows_copy = NULL;
    if (sel->order_count > 0 && n > 0) {
        sort_key_t okey;
        if (sort_key_init(&okey, t, sel->order, sel->order_count) != 0) {
            free(cidx); free(idx); return NLSQL_ERR;
        }
        uint64_t want = n;
        if (!sel->distinct && lim_cnt != UINT64_MAX) {
            want = (start > UINT64_MAX - lim_cnt) ? n : start + lim_cnt;
            if (want > n) want = n;
        }
        if (rows_sorted_key(&okey, idx, n)) {
            /* 已按排序键有序（如 ORDER BY 主键且插入顺序一致）：沿用扫描顺序 */
        } else if (want < n) {
            /* top-N 剪枝：只保留最小 want 行，排序规模从 n log n 降到 n log want */
            if (want == 0) {
                n = 0;
            } else {
                rows_copy = (row_t*)malloc((size_t)want * sizeof(row_t));
                if (!rows_copy) { sort_key_free(&okey); free(cidx); free(idx); return NLSQL_ERR; }
                n = rows_topn(&okey, idx, n, want, rows_copy);
                for (uint64_t i = 0; i < n; i++) idx[i] = &rows_copy[i];
            }
        } else {
            rows_copy = (row_t*)malloc((size_t)n * sizeof(row_t));
            if (!rows_copy) { sort_key_free(&okey); free(cidx); free(idx); return NLSQL_ERR; }
            for (uint64_t i = 0; i < n; i++) rows_copy[i] = *idx[i];
            rows_sort(&okey, rows_copy, n);
            for (uint64_t i = 0; i < n; i++) idx[i] = &rows_copy[i];
        }
        sort_key_free(&okey);
    }

    nlsql_result_t* res = res_new(ncols);
    if (!res) { free(cidx); free(idx); free(rows_copy); return NLSQL_ERR; }
    for (int i = 0; i < ncols; i++) {
        const char* nm = t->cols[cidx[i]].name;
        if (sel->col_alias && sel->col_alias[i][0]) nm = sel->col_alias[i];
        snprintf(res->cols[i].name, sizeof(res->cols[i].name), "%s", nm);
        res->cols[i].type = t->cols[cidx[i]].type;
    }
    dist_t dist;
    memset(&dist, 0, sizeof(dist));
    if (sel->distinct) {
        uint64_t hint = n < 65536 ? n : 65536;
        if (dist_init(&dist, hint) != 0) {
            nlsql_free_result(res); free(cidx); free(idx); free(rows_copy);
            return NLSQL_ERR;
        }
    }
    uint64_t lo = sel->distinct ? 0 : (start > n ? n : start);
    uint64_t hi = n;
    for (uint64_t r = lo; r < hi; r++) {
        uint64_t added = res->row_count;
        if (!sel->distinct) {
            if (added >= (r - lo) && lim_cnt != UINT64_MAX && added >= lim_cnt && r >= start + lim_cnt) break;
        }
        if (res_add_row(res, ncols) != 0) {
            dist_free(&dist);
            nlsql_free_result(res); free(cidx); free(idx); free(rows_copy);
            return NLSQL_ERR;
        }
        row_t* src = idx[r];
        nlsql_value_t* dst = res->rows[res->row_count - 1].vals;
        for (int i = 0; i < ncols; i++) {
            const nlsql_value_t* v = &src->vals[cidx[i]];
            dst[i] = *v;
            dst[i].s = NULL;
            if (!v->is_null && v->s &&
                (v->type == NLSQL_TYPE_VARCHAR || v->type == NLSQL_TYPE_TEXT || v->type == NLSQL_TYPE_DATETIME)) {
                dst[i].s = (char*)malloc((size_t)v->len + 1);
                if (dst[i].s) { memcpy(dst[i].s, v->s, v->len); dst[i].s[v->len] = '\0'; dst[i].len = v->len; }
                else { dst[i].is_null = 1; dst[i].type = NLSQL_TYPE_NULL; }
            }
        }
        if (sel->distinct) {
            int dup = dist_check_add(&dist, res, dst, ncols, res->row_count - 1);
            if (dup < 0) {
                dist_free(&dist);
                nlsql_free_result(res); free(cidx); free(idx); free(rows_copy);
                return NLSQL_ERR;
            }
            if (dup) {
                for (int i = 0; i < ncols; i++) free(dst[i].s);
                res->row_count--; /* vals 归 arena，无需逐行释放 */
            } else if (lim_cnt != UINT64_MAX && res->row_count >= start + lim_cnt) {
                break; /* 去重行已攒够窗口（DISTINCT 先于 LIMIT），无需继续扫描 */
            }
        }
    }
    /* DISTINCT 先全量再去窗口；普通路径已在循环内限窗 */
    if (sel->distinct) {
        res_trim(res, start, lim_cnt == UINT64_MAX ? res->row_count : lim_cnt);
        dist_free(&dist);
    }
    free(cidx); free(idx); free(rows_copy);
    *out = res;
    return NLSQL_OK;
}

/* =========================================
 * INSERT
 * ========================================= */

static int journal_autoinc(engine_t* e, table_t* t) {
    sbuf_t b;
    if (sbuf_init(&b, 64) != 0) return NLSQL_ERR_IO;
    nlsql_jr_put_str(&b, t->name);
    sbuf_append_u64(&b, (uint64_t)t->next_auto);
    int rc = nlsql_jr_append(e, JR_AUTOINC, b.p, b.len);
    sbuf_free(&b);
    return rc;
}

static int exec_insert(nlsql_conn_t* c, engine_t* e, insert_stmt_t* ins,
                       nlsql_value_t* params, int nparams, nlsql_result_t** out) {
    table_t* t = nlsql_tbl_find(e, ins->table);
    if (!t) {
        nlsql_set_err(c, NLSQL_ER_NO_SUCH_TABLE, "42S02",
                      "Table '%s.%s' doesn't exist", e->cur_db->name, ins->table);
        return NLSQL_ER_NO_SUCH_TABLE;
    }
    int64_t last_id = 0;
    uint64_t affected = 0;

    /* 主键索引：单列主键走 O(1) 哈希查重，多列/无索引回退线性 */
    int pkc = -1, npk = 0;
    for (int i = 0; i < t->col_count; i++) if (t->cols[i].pk) { npk++; pkc = i; }
    int pk_idx_ok = (npk == 1) && (nlsql_pk_ensure(t) == NLSQL_OK);

    row_t* row = NULL;   /* 当前未提交行（循环外可见，错误出口统一丢弃） */
    int irc = 0;
    /* 循环外常量：列映射 / AUTO_INCREMENT 列（行间不变） */
    int map[NLSQL_MAX_COLUMNS];
    if (ins->col_count > NLSQL_MAX_COLUMNS) { /* 防御：列清单超上限会越界写 map[] */
        nlsql_set_err(c, NLSQL_ER_NOT_SUPPORTED, "42000", "Too many columns");
        return NLSQL_ER_NOT_SUPPORTED;
    }
    if (ins->col_count) {
        for (int i = 0; i < ins->col_count; i++) {
            map[i] = col_index(t, ins->cols[i]);
            if (map[i] < 0) {
                nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22",
                              "Unknown column '%s' in 'field list'", ins->cols[i]);
                return NLSQL_ER_UNKNOWN_COLUMN;
            }
        }
    }
    int auto_col = -1;
    for (int i = 0; i < t->col_count; i++) if (t->cols[i].auto_inc) { auto_col = i; break; }
    int auto_given = 0;
    if (ins->col_count)
        for (int i = 0; i < ins->col_count; i++)
            if (col_index(t, ins->cols[i]) == auto_col) { auto_given = 1; break; }
    for (int r = 0; r < ins->row_count; r++) {
        int nvals = ins->row_col_counts[r];
        /* 无列清单时目标列数 = 表列数（原先取 nvals 导致值数多于列数时堆越界读写） */
        int target = ins->col_count ? ins->col_count : t->col_count;
        if (nvals != target) {
            nlsql_set_err(c, NLSQL_ER_WRONG_VALUE_COUNT, "21S01",
                          "Column count doesn't match value count at row %d", r + 1);
            return NLSQL_ER_WRONG_VALUE_COUNT;
        }
        if (!ins->col_count) for (int i = 0; i < target; i++) map[i] = i;
        /* 构造新行（默认全 NULL） */
        row = NULL;
        if (nlsql_row_alloc(t, &row) != 0) return NLSQL_ERR;
        for (int i = 0; i < t->col_count; i++) { row->vals[i].is_null = 1; row->vals[i].type = NLSQL_TYPE_NULL; }

        /* AUTO_INCREMENT 主键：省略或 NULL 时自动赋值 */
        int auto_filled = 0;
        if (auto_col >= 0) {
            if (auto_given) {
                /* 显式值：求值后更新 next_auto */
                for (int i = 0; i < target; i++) {
                    if (map[i] != auto_col) continue;
                    nlsql_value_t v;
                    char em[128];
                    if (eval_expr(ins->rows[r][i], t, NULL, params, nparams, &v, em, sizeof(em)) != 0) {
                        nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22", "%s", em);
                        free(v.s); irc = NLSQL_ER_UNKNOWN_COLUMN; goto ins_err;
                    }
                    if (!v.is_null && v.type == NLSQL_TYPE_INT64 && v.i >= t->next_auto) {
                        t->next_auto = v.i + 1;
                        last_id = v.i;
                        if (journal_autoinc(e, t) != NLSQL_OK) { nlsql_set_err(c, NLSQL_ERR_IO, "HY000", "journal write failed"); free(v.s); irc = NLSQL_ERR_IO; goto ins_err; }
                    }
                    free(v.s);
                    break;
                }
            } else {
                if (t->next_auto < 1) t->next_auto = 1;
                row->vals[auto_col].type = NLSQL_TYPE_INT64;
                row->vals[auto_col].i = t->next_auto;
                row->vals[auto_col].is_null = 0;
                last_id = t->next_auto;
                t->next_auto++;
                auto_filled = 1;
                if (journal_autoinc(e, t) != NLSQL_OK) { nlsql_set_err(c, NLSQL_ERR_IO, "HY000", "journal write failed"); irc = NLSQL_ERR_IO; goto ins_err; }
            }
        }

        /* 求值其余列 */
        for (int i = 0; i < target; i++) {
            if (auto_filled && map[i] == auto_col) continue;
            nlsql_value_t v;
            char em[128];
            if (eval_expr(ins->rows[r][i], t, NULL, params, nparams, &v, em, sizeof(em)) != 0) {
                nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22", "%s", em);
                free(v.s); irc = NLSQL_ER_UNKNOWN_COLUMN; goto ins_err;
            }
            /* 类型归一 */
            tbl_col_t* tc = &t->cols[map[i]];
            if (!v.is_null) {
                if (tc->type == NLSQL_TYPE_INT64 && v.type != NLSQL_TYPE_INT64) {
                    int64_t iv; value_to_int(&v, &iv); free(v.s); v.s = NULL; v.type = NLSQL_TYPE_INT64; v.i = iv;
                } else if (tc->type == NLSQL_TYPE_DOUBLE && v.type != NLSQL_TYPE_DOUBLE) {
                    double dv; value_to_double(&v, &dv); free(v.s); v.s = NULL; v.type = NLSQL_TYPE_DOUBLE; v.d = dv;
                } else if ((tc->type == NLSQL_TYPE_VARCHAR || tc->type == NLSQL_TYPE_TEXT || tc->type == NLSQL_TYPE_DATETIME)
                           && v.type != NLSQL_TYPE_VARCHAR && v.type != NLSQL_TYPE_TEXT && v.type != NLSQL_TYPE_DATETIME) {
                    char buf[64];
                    const char* txt = NULL;
                    uint32_t len;
                    if (v.type == NLSQL_TYPE_INT64) { len = (uint32_t)snprintf(buf, sizeof(buf), "%lld", (long long)v.i); txt = buf; }
                    else { len = (uint32_t)snprintf(buf, sizeof(buf), "%g", v.d); txt = buf; }
                    free(v.s);
                    v.s = (char*)malloc((size_t)len + 1);
                    memcpy(v.s, txt, len); v.s[len] = '\0';
                    v.type = NLSQL_TYPE_VARCHAR; v.len = len;
                }
                if (tc->type == NLSQL_TYPE_VARCHAR && tc->vlen && v.len > tc->vlen) {
                    /* MySQL 非严格模式：截断 */
                    v.s[tc->vlen] = '\0'; v.len = tc->vlen;
                }
            }
            /* NOT NULL 检查 */
            if (tc->not_null && v.is_null && tc->type != NLSQL_TYPE_INT64) {
                free(v.s);
                nlsql_set_err(c, NLSQL_ER_BAD_NULL_ERROR, "23000",
                              "Column '%s' cannot be null", tc->name);
                irc = NLSQL_ER_BAD_NULL_ERROR; goto ins_err;
            }
            if (tc->not_null && v.is_null && tc->type == NLSQL_TYPE_INT64) {
                /* 整数 NOT NULL：MySQL 实际也报错 */
                free(v.s);
                nlsql_set_err(c, NLSQL_ER_BAD_NULL_ERROR, "23000",
                              "Column '%s' cannot be null", tc->name);
                irc = NLSQL_ER_BAD_NULL_ERROR; goto ins_err;
            }
            /* 主键冲突检查（索引可用 → O(1) 哈希探测） */
            if (tc->pk && !v.is_null) {
                uint64_t hit = 0;
                int dup = 0;
                if (pk_idx_ok) {
                    dup = nlsql_pk_lookup(t, &v, &hit);
                } else {
                    for (uint64_t x = 0; x < t->row_count; x++) {
                        nlsql_value_t* ev = &t->rows[x].vals[map[i]];
                        int rel;
                        if (!ev->is_null && cmp_values(&v, ev, &rel) == 0 && rel == 0) { dup = 1; break; }
                    }
                }
                if (dup) {
                    char disp[64];
                    if (v.type == NLSQL_TYPE_INT64) snprintf(disp, sizeof(disp), "%lld", (long long)v.i);
                    else snprintf(disp, sizeof(disp), "%s", v.s ? v.s : "");
                    free(v.s);
                    nlsql_set_err(c, NLSQL_ER_DUP_ENTRY, "23000",
                                  "Duplicate entry '%s' for key 'PRIMARY'", disp);
                    irc = NLSQL_ER_DUP_ENTRY; goto ins_err;
                }
            }
            row->vals[map[i]] = v;
        }

        /* 未映射列填 DEFAULT（省略列清单或列清单不全时） */
        for (int i = 0; i < t->col_count; i++) {
            tbl_col_t* tc = &t->cols[i];
            if (!tc->has_def || row->vals[i].type != NLSQL_TYPE_NULL || row->vals[i].s) continue;
            int given = 0;
            for (int k = 0; k < target; k++) if (map[k] == i) { given = 1; break; }
            if (given) continue;
            row->vals[i] = tc->def;
            if (tc->def.s) {
                char* dup = (char*)malloc((size_t)tc->def.len + 1);
                if (dup) { memcpy(dup, tc->def.s, (size_t)tc->def.len + 1); row->vals[i].s = dup; }
                else { row->vals[i].is_null = 1; row->vals[i].type = NLSQL_TYPE_NULL; }
            }
        }

        /* 唯一二级索引冲突检查（索引由扩展创建；查找不含本行） */
        for (tbl_index_t* ix = t->indexes; ix; ix = ix->next) {
            if (!ix->unique) continue;
            nlsql_value_t keys[8];
            int has_null = 0;
            for (int k = 0; k < ix->ncols; k++) {
                keys[k] = row->vals[ix->cols[k]];
                if (keys[k].is_null) { has_null = 1; break; }
            }
            if (has_null) continue;
            uint64_t* hits = NULL; uint64_t cnt = 0;
            int lrc = nlsql_idx_lookup(t, ix, keys, &hits, &cnt);
            free(hits);
            if (lrc != NLSQL_OK) { irc = lrc; goto ins_err; }
            if (cnt > 0) {
                char disp[96];
                if (keys[0].type == NLSQL_TYPE_INT64) snprintf(disp, sizeof(disp), "%lld", (long long)keys[0].i);
                else snprintf(disp, sizeof(disp), "%s", keys[0].s ? keys[0].s : "");
                nlsql_set_err(c, NLSQL_ER_DUP_ENTRY, "23000",
                              "Duplicate entry '%s' for key '%s'", disp, ix->name);
                irc = NLSQL_ER_DUP_ENTRY; goto ins_err;
            }
        }

        /* journal 整行 */
        sbuf_t b;
        if (sbuf_init(&b, 128) != 0) { irc = NLSQL_ERR; goto ins_err; }
        nlsql_jr_put_str(&b, t->name);
        for (int i = 0; i < t->col_count; i++) nlsql_sbuf_enc_value(&b, &row->vals[i]);
        int jrc = nlsql_jr_append(e, JR_INSERT, b.p, b.len);
        sbuf_free(&b);
        if (jrc != NLSQL_OK) {
            nlsql_set_err(c, NLSQL_ERR_IO, "HY000", "journal write failed");
            irc = NLSQL_ERR_IO; goto ins_err;
        }
        /* 主键入索引（行号 = commit 前 row_count） */
        if (pk_idx_ok && pkc >= 0 && !row->vals[pkc].is_null)
            nlsql_pk_add(t, t->row_count, &row->vals[pkc]);
        nlsql_tbl_commit_row(t);
        nlsql_tbl_mark_dirty(t);
        affected++;

        /* journal 超限不在语句内压实（会打断大批量写入）：语句结束由
           调用层在 SNAP 锁下统一压实，读不受影响、写不再被 3-7 秒停顿切碎 */
    }

ins_err:
    if (irc != 0) { row_discard(t, row); return irc; } /* 丢弃未提交行 */

    nlsql_result_t* res = res_new(0);
    if (!res) return NLSQL_ERR;
    res->affected = affected;
    res->insert_id = last_id;
    e->last_insert_id = last_id;  /* LAST_INSERT_ID() 会话级“最近自增值” */
    *out = res;
    return NLSQL_OK;
}

/* =========================================
 * DDL / USE / SHOW / 常量 SELECT
 * ========================================= */

static int journal_create_tbl(engine_t* e, table_t* t) {
    sbuf_t b;
    if (sbuf_init(&b, 256) != 0) return NLSQL_ERR_IO;
    nlsql_jr_put_str(&b, t->name);
    uint8_t nc[2] = { (uint8_t)t->col_count, (uint8_t)(t->col_count >> 8) };
    sbuf_append(&b, nc, 2);
    for (int i = 0; i < t->col_count; i++) {
        nlsql_jr_put_str(&b, t->cols[i].name);
        sbuf_append_u8(&b, (uint8_t)t->cols[i].type);
        sbuf_append_u32(&b, t->cols[i].vlen);
        uint8_t flags = (uint8_t)((t->cols[i].pk ? 1 : 0) | (t->cols[i].auto_inc ? 2 : 0) | (t->cols[i].not_null ? 4 : 0)
                                  | (t->cols[i].has_def ? 8 : 0));
        sbuf_append_u8(&b, flags);
        if (t->cols[i].has_def) nlsql_sbuf_enc_value(&b, &t->cols[i].def);
    }
    int rc = nlsql_jr_append(e, JR_CREATE_TBL, b.p, b.len);
    sbuf_free(&b);
    return rc == NLSQL_OK ? NLSQL_OK : NLSQL_ERR_IO;
}

static int journal_tbl_name(engine_t* e, uint8_t type, const char* name) {
    sbuf_t b;
    if (sbuf_init(&b, 64) != 0) return NLSQL_ERR_IO;
    nlsql_jr_put_str(&b, name);
    int rc = nlsql_jr_append(e, type, b.p, b.len);
    sbuf_free(&b);
    return rc == NLSQL_OK ? NLSQL_OK : NLSQL_ERR_IO;
}

/* 表加入内存链（头插） */
static void tbl_link(database_t* db, table_t* t) {
    t->next = db->tables;
    db->tables = t;
}

static int exec_create_table(nlsql_conn_t* c, engine_t* e, create_table_stmt_t* ct, nlsql_result_t** out) {
    if (nlsql_tbl_find(e, ct->table)) {
        if (ct->if_not_exists) { if (out) *out = NULL; return NLSQL_OK; }  /* 已存在：IF NOT EXISTS 视为成功 */
        nlsql_set_err(c, NLSQL_ER_TABLE_EXISTS, "42S01",
                      "Table '%s' already exists", ct->table);
        return NLSQL_ER_TABLE_EXISTS;
    }
    table_t* t = (table_t*)calloc(1, sizeof(table_t));
    if (!t) return NLSQL_ERR;
    snprintf(t->name, sizeof(t->name), "%s", ct->table);
    t->col_count = ct->col_count;
    t->cols = (tbl_col_t*)calloc((size_t)ct->col_count, sizeof(tbl_col_t));
    if (!t->cols) { free(t); return NLSQL_ERR; }
    for (int i = 0; i < ct->col_count; i++) {
        snprintf(t->cols[i].name, sizeof(t->cols[i].name), "%s", ct->cols[i].name);
        t->cols[i].type = ct->cols[i].type;
        t->cols[i].vlen = ct->cols[i].vlen;
        t->cols[i].pk = ct->cols[i].pk;
        t->cols[i].auto_inc = ct->cols[i].auto_inc;
        t->cols[i].not_null = ct->cols[i].not_null;
        if (ct->cols[i].has_def) {
            t->cols[i].has_def = 1;
            t->cols[i].def = ct->cols[i].def;
            if (t->cols[i].def.s) {
                char* dup = (char*)malloc((size_t)t->cols[i].def.len + 1);
                if (!dup) { for (int k = 0; k < i; k++) free(t->cols[k].def.s); free(t->cols); free(t); return NLSQL_ERR; }
                memcpy(dup, t->cols[i].def.s, t->cols[i].def.len + 1);
                t->cols[i].def.s = dup;
            }
            val_cast(&t->cols[i].def, t->cols[i].type, t->cols[i].vlen);
        }
    }
    t->next_auto = 1;
    if (journal_create_tbl(e, t) != NLSQL_OK) {
        for (int i = 0; i < t->col_count; i++) free(t->cols[i].def.s);
        free(t->cols); free(t);
        nlsql_set_err(c, NLSQL_ERR_IO, "HY000", "journal write failed");
        return NLSQL_ERR_IO;
    }
    tbl_link(e->cur_db, t);
    nlsql_tbl_mark_dirty(t);
    if (out) { *out = NULL; }
    return NLSQL_OK;
}

static int exec_drop_table(nlsql_conn_t* c, engine_t* e, const char* name, int if_exists,
                           nlsql_result_t** out) {
    table_t* t = nlsql_tbl_find(e, name);
    if (!t) {
        if (if_exists) { if (out) *out = NULL; return NLSQL_OK; }  /* DROP TABLE IF EXISTS：不存在即成功 */
        nlsql_set_err(c, NLSQL_ER_NO_SUCH_TABLE, "42S02",
                      "Table '%s.%s' doesn't exist", e->cur_db->name, name);
        return NLSQL_ER_NO_SUCH_TABLE;
    }
    if (journal_tbl_name(e, JR_DROP_TBL, name) != NLSQL_OK) {
        nlsql_set_err(c, NLSQL_ERR_IO, "HY000", "journal write failed");
        return NLSQL_ERR_IO;
    }
    table_t** pp = &e->cur_db->tables;
    while (*pp) {
        if (*pp == t) { *pp = t->next; break; }
        pp = &(*pp)->next;
    }
    for (uint64_t r = 0; r < t->row_count; r++) {
        for (int i = 0; i < t->col_count; i++) free(t->rows[r].vals[i].s);
        /* vals 归 arena */
    }
    nlsql_tbl_val_arena_free(t);
    free(t->rows);
    for (int i = 0; i < t->col_count; i++) free(t->cols[i].def.s);
    free(t->cols); free(t);
    /* 删除快照文件 */
    char path[1150];
    snprintf(path, sizeof(path), "%s/%s/%s.nlt", e->root, e->cur_db->name, name);
    unlink(path);
    if (out) *out = NULL;
    return NLSQL_OK;
}

static int exec_truncate(nlsql_conn_t* c, engine_t* e, const char* name, nlsql_result_t** out) {
    table_t* t = nlsql_tbl_find(e, name);
    if (!t) {
        nlsql_set_err(c, NLSQL_ER_NO_SUCH_TABLE, "42S02",
                      "Table '%s.%s' doesn't exist", e->cur_db->name, name);
        return NLSQL_ER_NO_SUCH_TABLE;
    }
    if (journal_tbl_name(e, JR_TRUNCATE, name) != NLSQL_OK) {
        nlsql_set_err(c, NLSQL_ERR_IO, "HY000", "journal write failed");
        return NLSQL_ERR_IO;
    }
    for (uint64_t r = 0; r < t->row_count; r++) {
        for (int i = 0; i < t->col_count; i++) free(t->rows[r].vals[i].s);
        /* vals 归 arena，TRUNCATE 后整块回收 */
    }
    nlsql_tbl_val_arena_free(t);
    t->row_count = 0;
    t->next_auto = 1;
    nlsql_pk_invalidate(t);
    nlsql_tbl_mark_mutated(t); /* 清空无法用增量帧表达 */
    if (out) *out = NULL;
    return NLSQL_OK;
}

/* =========================================
 * ALTER TABLE（DDL 罕见：改内存后立即压实持久化，
 * 不走增量 journal——崩溃时内存与磁盘同时回退到 ALTER 前，保持一致）
 * ========================================= */

/* 值按新列类型归一（复用 INSERT 的转换语义） */
static void val_cast(nlsql_value_t* v, nlsql_type_t nt, uint32_t vlen) {
    if (v->is_null) return;
    int is_str = (nt == NLSQL_TYPE_VARCHAR || nt == NLSQL_TYPE_TEXT || nt == NLSQL_TYPE_DATETIME);
    if (nt == NLSQL_TYPE_INT64 && v->type != NLSQL_TYPE_INT64) {
        int64_t iv = (v->type == NLSQL_TYPE_DOUBLE) ? (int64_t)v->d
                                                    : (v->s ? strtoll(v->s, NULL, 10) : 0);
        free(v->s); v->s = NULL; v->len = 0;
        v->type = NLSQL_TYPE_INT64; v->i = iv;
    } else if (nt == NLSQL_TYPE_DOUBLE && v->type != NLSQL_TYPE_DOUBLE) {
        double dv = (v->type == NLSQL_TYPE_INT64) ? (double)v->i
                                                  : (v->s ? atof(v->s) : 0.0);
        free(v->s); v->s = NULL; v->len = 0;
        v->type = NLSQL_TYPE_DOUBLE; v->d = dv;
    } else if (is_str && v->type != NLSQL_TYPE_VARCHAR && v->type != NLSQL_TYPE_TEXT
               && v->type != NLSQL_TYPE_DATETIME) {
        char buf[64]; uint32_t len;
        if (v->type == NLSQL_TYPE_INT64) len = (uint32_t)snprintf(buf, sizeof(buf), "%lld", (long long)v->i);
        else len = (uint32_t)snprintf(buf, sizeof(buf), "%g", v->d);
        free(v->s);
        v->s = (char*)malloc((size_t)len + 1);
        if (v->s) { memcpy(v->s, buf, len); v->s[len] = '\0'; }
        else { v->is_null = 1; v->type = NLSQL_TYPE_NULL; return; }
        v->type = NLSQL_TYPE_VARCHAR; v->len = len;
    }
    if (is_str && v->s && vlen && v->len > vlen) { v->s[vlen] = '\0'; v->len = vlen; }
}

static int exec_alter(nlsql_conn_t* c, engine_t* e, alter_stmt_t* al, nlsql_result_t** out) {
    table_t* t = nlsql_tbl_find(e, al->table);
    if (!t) {
        nlsql_set_err(c, NLSQL_ER_NO_SUCH_TABLE, "42S02",
                      "Table '%s.%s' doesn't exist", e->cur_db->name, al->table);
        return NLSQL_ER_NO_SUCH_TABLE;
    }
    char oldpath[1170]; /* 覆盖 root(1023)+"/"+db(63)+"/"+tbl(63)+".nlt" 最坏情况，防截断 */
    snprintf(oldpath, sizeof(oldpath), "%s/%s/%s.nlt", e->root, e->cur_db->name, t->name);

    switch (al->op) {
    case ALT_ADD_COL: {
        if (col_index(t, al->col) >= 0) {
            nlsql_set_err(c, 1060, "42S21", "Duplicate column name '%s'", al->col);
            return 1060;
        }
        if (t->col_count >= NLSQL_MAX_COLUMNS) {
            nlsql_set_err(c, NLSQL_ER_NOT_SUPPORTED, "42000", "Too many columns");
            return NLSQL_ER_NOT_SUPPORTED;
        }
        if (al->def.not_null && t->row_count > 0 && !al->def.auto_inc && !al->def.has_def) {
            nlsql_set_err(c, NLSQL_ER_BAD_NULL_ERROR, "23000",
                          "Column '%s' cannot be null", al->def.name);
            return NLSQL_ER_BAD_NULL_ERROR;
        }
        if (al->def.pk) {                 /* 单列主键模型：新 pk 接管旧 pk */
            for (int i = 0; i < t->col_count; i++) t->cols[i].pk = 0;
        }
        tbl_col_t* nc = (tbl_col_t*)realloc(t->cols, (size_t)(t->col_count + 1) * sizeof(tbl_col_t));
        if (!nc) { nlsql_set_err(c, NLSQL_ERR, "HY000", "out of memory"); return NLSQL_ERR; }
        t->cols = nc;
        t->vals_free = NULL; /* 列数变化，旧空闲块尺寸不符，弃链（由 arena 统一回收） */
        for (uint64_t r = 0; r < t->row_count; r++) {
            nlsql_value_t* nv = nlsql_tbl_val_alloc(t, (int)t->col_count + 1);
            if (!nv) { nlsql_set_err(c, NLSQL_ERR, "HY000", "out of memory"); return NLSQL_ERR; }
            memcpy(nv, t->rows[r].vals, (size_t)t->col_count * sizeof(nlsql_value_t));
            t->rows[r].vals = nv;
            memset(&nv[t->col_count], 0, sizeof(nlsql_value_t));  /* 旧行新列默认 NULL */
            if (al->def.has_def) {         /* 有 DEFAULT → 旧行填默认值 */
                nv[t->col_count] = al->def.def;
                if (al->def.def.s) {
                    char* dup = (char*)malloc((size_t)al->def.def.len + 1);
                    if (dup) { memcpy(dup, al->def.def.s, (size_t)al->def.def.len + 1); nv[t->col_count].s = dup; }
                    else { nv[t->col_count].is_null = 1; nv[t->col_count].type = NLSQL_TYPE_NULL; }
                }
            }
        }
        tbl_col_t* col = &t->cols[t->col_count];
        memset(col, 0, sizeof(*col));
        memcpy(col->name, al->def.name, sizeof(col->name));
        col->type = al->def.type; col->vlen = al->def.vlen;
        col->pk = al->def.pk; col->auto_inc = al->def.auto_inc; col->not_null = al->def.not_null;
        if (al->def.has_def) {
            col->has_def = 1;
            col->def = al->def.def;
            if (col->def.s) {
                char* dup = (char*)malloc((size_t)col->def.len + 1);
                if (dup) { memcpy(dup, col->def.s, (size_t)col->def.len + 1); col->def.s = dup; }
                else { col->has_def = 0; memset(&col->def, 0, sizeof(col->def)); }
            }
        }
        t->col_count++;
        nlsql_pk_invalidate(t);
        break;
    }
    case ALT_DROP_COL: {
        int idx = col_index(t, al->col);
        if (idx < 0) {
            nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22",
                          "Unknown column '%s' in '%s'", al->col, al->table);
            return NLSQL_ER_UNKNOWN_COLUMN;
        }
        if (t->col_count <= 1) {
            nlsql_set_err(c, NLSQL_ER_NOT_SUPPORTED, "42000", "Cannot drop the only column");
            return NLSQL_ER_NOT_SUPPORTED;
        }
        for (uint64_t r = 0; r < t->row_count; r++) {
            free(t->rows[r].vals[idx].s);
            memmove(&t->rows[r].vals[idx], &t->rows[r].vals[idx + 1],
                    (size_t)(t->col_count - idx - 1) * sizeof(nlsql_value_t));
        }
        memmove(&t->cols[idx], &t->cols[idx + 1],
                (size_t)(t->col_count - idx - 1) * sizeof(tbl_col_t));
        t->col_count--;
        t->vals_free = NULL; /* 列数变化，旧空闲块尺寸不符，弃链（由 arena 统一回收） */
        free(t->cols[t->col_count].def.s);   /* 被删列（memmove 后槽位残留） */
        memset(&t->cols[t->col_count], 0, sizeof(tbl_col_t));
        nlsql_pk_invalidate(t);   /* 列位移 / pk 列移除，索引重建于下次 ensure */
        break;
    }
    case ALT_MODIFY_COL: {
        int idx = col_index(t, al->col);
        if (idx < 0) {
            nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22",
                          "Unknown column '%s' in '%s'", al->col, al->table);
            return NLSQL_ER_UNKNOWN_COLUMN;
        }
        tbl_col_t* tc = &t->cols[idx];
        if (al->def.auto_inc && al->def.type != NLSQL_TYPE_INT64) {
            nlsql_set_err(c, NLSQL_ER_NOT_SUPPORTED, "42000",
                          "AUTO_INCREMENT column must be INTEGER");
            return NLSQL_ER_NOT_SUPPORTED;
        }
        tc->type = al->def.type; tc->vlen = al->def.vlen;
        tc->pk = al->def.pk; tc->auto_inc = al->def.auto_inc; tc->not_null = al->def.not_null;
        free(tc->def.s);
        memset(&tc->def, 0, sizeof(tc->def));
        tc->has_def = 0;
        if (al->def.has_def) {
            tc->has_def = 1;
            tc->def = al->def.def;
            if (tc->def.s) {
                char* dup = (char*)malloc((size_t)tc->def.len + 1);
                if (dup) { memcpy(dup, tc->def.s, (size_t)tc->def.len + 1); tc->def.s = dup; }
                else { tc->has_def = 0; memset(&tc->def, 0, sizeof(tc->def)); }
            }
        }
        if (tc->pk) {
            for (int i = 0; i < t->col_count; i++)
                if (i != idx) t->cols[i].pk = 0;   /* 单列主键模型 */
        }
        for (uint64_t r = 0; r < t->row_count; r++) {
            nlsql_value_t* v = &t->rows[r].vals[idx];
            if (v->is_null && tc->has_def) {       /* NULL 行补默认值 */
                *v = tc->def;
                if (tc->def.s) {
                    char* dup = (char*)malloc((size_t)tc->def.len + 1);
                    if (dup) { memcpy(dup, tc->def.s, (size_t)tc->def.len + 1); v->s = dup; }
                    else { v->is_null = 1; v->type = NLSQL_TYPE_NULL; }
                }
                continue;
            }
            if (tc->not_null && v->is_null) {
                nlsql_set_err(c, NLSQL_ER_BAD_NULL_ERROR, "23000",
                              "Column '%s' cannot be null", tc->name);
                return NLSQL_ER_BAD_NULL_ERROR;
            }
            val_cast(v, tc->type, tc->vlen);
        }
        nlsql_pk_invalidate(t);
        break;
    }
    case ALT_RENAME_COL: {
        int idx = col_index(t, al->col);
        if (idx < 0) {
            nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22",
                          "Unknown column '%s' in '%s'", al->col, al->table);
            return NLSQL_ER_UNKNOWN_COLUMN;
        }
        if (col_index(t, al->newname) >= 0) {
            nlsql_set_err(c, 1060, "42S21", "Duplicate column name '%s'", al->newname);
            return 1060;
        }
        snprintf(t->cols[idx].name, sizeof(t->cols[idx].name), "%s", al->newname);
        break;
    }
    case ALT_RENAME_TABLE: {
        if (!nlsql_valid_ident(al->newname)) {
            nlsql_set_err(c, NLSQL_ERR_PATH, "HY000", "invalid table name");
            return NLSQL_ERR_PATH;
        }
        if (nlsql_tbl_find(e, al->newname)) {
            nlsql_set_err(c, NLSQL_ER_TABLE_EXISTS, "42S01",
                          "Table '%s' already exists", al->newname);
            return NLSQL_ER_TABLE_EXISTS;
        }
        snprintf(t->name, sizeof(t->name), "%s", al->newname);
        break;
    }
    }

    t->epoch++;   /* 结构世代 +1：失效所有列索引缓存的免名校验 */
    nlsql_tbl_mark_dirty(t);
    int rc = eng_flush(e);   /* DDL 立即压实：快照 + journal truncate 原子持久化 */
    if (rc != NLSQL_OK) {
        nlsql_set_err(c, NLSQL_ERR_IO, "HY000", "flush failed");
        return NLSQL_ERR_IO;
    }
    if (al->op == ALT_RENAME_TABLE) unlink(oldpath);  /* 新快照落盘后删除旧快照 */
    if (out) *out = NULL;
    return NLSQL_OK;
}

/* =========================================
 * 元数据（系统变量 / 元数据函数 / SHOW 族）
 * 面向 GUI 客户端（SQLyog 等）连接后必发的探测语句
 * ========================================= */

/* 系统变量静态表：SHOW VARIABLES 与 SELECT @@var 共用同一份真值 */
typedef struct { const char* name; const char* val; } sysvar_t;

static const sysvar_t g_sysvars[] = {
    { "autocommit",                  "1" },
    { "character_set_client",        "utf8" },
    { "character_set_connection",    "utf8" },
    { "character_set_database",      "utf8" },
    { "character_set_filesystem",    "binary" },
    { "character_set_results",       "utf8" },
    { "character_set_server",        "utf8" },
    { "character_set_system",        "utf8" },
    { "collation_connection",        "utf8_general_ci" },
    { "collation_database",          "utf8_general_ci" },
    { "collation_server",            "utf8_general_ci" },
    { "default_storage_engine",      "NLSQL" },
    { "have_ssl",                    "YES" },
    { "hostname",                    "localhost" },
    { "init_connect",                "" },
    { "interactive_timeout",         "28800" },
    { "license",                     "GPL" },
    { "lower_case_table_names",      "0" },
    { "max_allowed_packet",          "16777216" },
    { "max_connections",             "64" },
    { "net_read_timeout",            "30" },
    { "net_write_timeout",           "60" },
    { "performance_schema",          "0" },
    { "protocol_version",            "10" },
    { "pseudo_thread_id",            "1" },
    { "sql_mode",                    "" },
    { "storage_engine",              "NLSQL" },
    { "system_time_zone",            "UTC" },
    { "time_zone",                   "SYSTEM" },
    { "transaction_isolation",       "REPEATABLE-READ" },
    { "tx_isolation",                "REPEATABLE-READ" },
    { "version",                     NLSQL_SERVER_VERSION },
    { "version_comment",             "NLSQL embedded engine" },
    { "wait_timeout",                "28800" },
};
#define SYSVAR_N (int)(sizeof(g_sysvars) / sizeof(g_sysvars[0]))

/* 按名查系统变量；支持 session./global. 前缀。找到返回 1 并填 *out（指向静态串） */
static int sysvar_lookup(const char* name, const char** out) {
    if (!name) return 0;
    if (strncmp(name, "session.", 8) == 0) name += 8;
    else if (strncmp(name, "global.", 7) == 0) name += 7;
    if (strncmp(name, "@@", 2) == 0) name += 2;
    for (int i = 0; i < SYSVAR_N; i++) {
        if (strcmp(g_sysvars[i].name, name) == 0) { *out = g_sysvars[i].val; return 1; }
    }
    return 0;
}

/* 元数据函数求值 */
static void meta_func_eval(nlsql_conn_t* c, engine_t* e, int fid, nlsql_value_t* v) {
    char buf[160];
    switch (fid) {
        case MF_DATABASE:
            if (e->cur_db) val_set_str(v, e->cur_db->name,
                                       (uint32_t)strlen(e->cur_db->name), NLSQL_TYPE_VARCHAR);
            else { v->type = NLSQL_TYPE_NULL; v->is_null = 1; }
            break;
        case MF_VERSION:
            val_set_str(v, NLSQL_SERVER_VERSION, (uint32_t)strlen(NLSQL_SERVER_VERSION),
                        NLSQL_TYPE_VARCHAR);
            break;
        case MF_USER:
            snprintf(buf, sizeof(buf), "%s@localhost",
                     c->have_creds && c->creds_user[0] ? c->creds_user : "root");
            val_set_str(v, buf, (uint32_t)strlen(buf), NLSQL_TYPE_VARCHAR);
            break;
        case MF_CONNECTION_ID:
            v->type = NLSQL_TYPE_INT64; v->i = 1; break;
        case MF_LAST_INSERT_ID:
            v->type = NLSQL_TYPE_INT64; v->i = e->last_insert_id; break;
        case MF_ROW_COUNT:
            v->type = NLSQL_TYPE_INT64; v->i = 0; break;
        default:
            v->type = NLSQL_TYPE_NULL; v->is_null = 1; break;
    }
}

/* 已加载库查找（SHOW ... FROM db 用；不触发换库以免读锁下改引擎状态） */
static database_t* find_loaded_db(engine_t* e, const char* name) {
    for (database_t* d = e->dbs; d; d = d->next)
        if (strcmp(d->name, name) == 0) return d;
    return NULL;
}

/* 列类型名（SHOW 族共用） */
static void col_type_text(const tbl_col_t* col, char* out, size_t sz) {
    switch (col->type) {
        case NLSQL_TYPE_INT64: snprintf(out, sz, "bigint"); break;
        case NLSQL_TYPE_DOUBLE: snprintf(out, sz, "double"); break;
        case NLSQL_TYPE_VARCHAR: snprintf(out, sz, "varchar(%u)", col->vlen); break;
        case NLSQL_TYPE_DATETIME: snprintf(out, sz, "datetime"); break;
        default: snprintf(out, sz, "text"); break;
    }
}

static int show_variables(nlsql_conn_t* c, parsed_stmt_t* st, nlsql_result_t** out) {
    (void)c;
    nlsql_result_t* res = res_new(2);
    if (!res) return NLSQL_ERR;
    snprintf(res->cols[0].name, sizeof(res->cols[0].name), "Variable_name");
    snprintf(res->cols[1].name, sizeof(res->cols[1].name), "Value");
    res->cols[0].type = res->cols[1].type = NLSQL_TYPE_VARCHAR;
    for (int i = 0; i < SYSVAR_N; i++) {
        if (st->has_like && !like_match(g_sysvars[i].name, st->like_pat)) continue;
        if (res_add_row(res, 2) != 0) { nlsql_free_result(res); return NLSQL_ERR; }
        nlsql_value_t* v = res->rows[res->row_count - 1].vals;
        val_set_str(&v[0], g_sysvars[i].name, (uint32_t)strlen(g_sysvars[i].name), NLSQL_TYPE_VARCHAR);
        val_set_str(&v[1], g_sysvars[i].val, (uint32_t)strlen(g_sysvars[i].val), NLSQL_TYPE_VARCHAR);
    }
    *out = res;
    return NLSQL_OK;
}

static int show_status(parsed_stmt_t* st, nlsql_result_t** out) {
    static const sysvar_t rows[] = {
        { "Aborted_clients", "0" },
        { "Bytes_received", "0" },
        { "Bytes_sent", "0" },
        { "Connections", "1" },
        { "Open_tables", "0" },
        { "Queries", "0" },
        { "Threads_connected", "1" },
        { "Threads_running", "1" },
        { "Uptime", "0" },
    };
    nlsql_result_t* res = res_new(2);
    if (!res) return NLSQL_ERR;
    snprintf(res->cols[0].name, sizeof(res->cols[0].name), "Variable_name");
    snprintf(res->cols[1].name, sizeof(res->cols[1].name), "Value");
    res->cols[0].type = res->cols[1].type = NLSQL_TYPE_VARCHAR;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (st->has_like && !like_match(rows[i].name, st->like_pat)) continue;
        if (res_add_row(res, 2) != 0) { nlsql_free_result(res); return NLSQL_ERR; }
        nlsql_value_t* v = res->rows[res->row_count - 1].vals;
        val_set_str(&v[0], rows[i].name, (uint32_t)strlen(rows[i].name), NLSQL_TYPE_VARCHAR);
        val_set_str(&v[1], rows[i].val, (uint32_t)strlen(rows[i].val), NLSQL_TYPE_VARCHAR);
    }
    *out = res;
    return NLSQL_OK;
}

static int show_engines(nlsql_result_t** out) {
    static const char* hdr[6] = { "Engine", "Support", "Comment", "Transactions", "XA", "Savepoints" };
    static const char* val[6] = { "NLSQL", "DEFAULT", "NLSQL embedded engine", "NO", "NO", "NO" };
    nlsql_result_t* res = res_new(6);
    if (!res) return NLSQL_ERR;
    for (int i = 0; i < 6; i++) {
        snprintf(res->cols[i].name, sizeof(res->cols[i].name), "%s", hdr[i]);
        res->cols[i].type = NLSQL_TYPE_VARCHAR;
    }
    if (res_add_row(res, 6) != 0) { nlsql_free_result(res); return NLSQL_ERR; }
    for (int i = 0; i < 6; i++)
        val_set_str(&res->rows[0].vals[i], val[i], (uint32_t)strlen(val[i]), NLSQL_TYPE_VARCHAR);
    *out = res;
    return NLSQL_OK;
}

static int show_charset(nlsql_result_t** out) {
    static const char* hdr[4] = { "Charset", "Description", "Default collation", "Maxlen" };
    static const char* val[4] = { "utf8", "UTF-8 Unicode", "utf8_general_ci", "3" };
    nlsql_result_t* res = res_new(4);
    if (!res) return NLSQL_ERR;
    for (int i = 0; i < 4; i++) {
        snprintf(res->cols[i].name, sizeof(res->cols[i].name), "%s", hdr[i]);
        res->cols[i].type = NLSQL_TYPE_VARCHAR;
    }
    if (res_add_row(res, 4) != 0) { nlsql_free_result(res); return NLSQL_ERR; }
    for (int i = 0; i < 4; i++)
        val_set_str(&res->rows[0].vals[i], val[i], (uint32_t)strlen(val[i]), NLSQL_TYPE_VARCHAR);
    *out = res;
    return NLSQL_OK;
}

static int show_collation(nlsql_result_t** out) {
    static const char* hdr[6] = { "Collation", "Charset", "Id", "Default", "Compiled", "Sortlen" };
    static const char* val[6] = { "utf8_general_ci", "utf8", "33", "Yes", "Yes", "1" };
    nlsql_result_t* res = res_new(6);
    if (!res) return NLSQL_ERR;
    for (int i = 0; i < 6; i++) {
        snprintf(res->cols[i].name, sizeof(res->cols[i].name), "%s", hdr[i]);
        res->cols[i].type = NLSQL_TYPE_VARCHAR;
    }
    if (res_add_row(res, 6) != 0) { nlsql_free_result(res); return NLSQL_ERR; }
    for (int i = 0; i < 6; i++)
        val_set_str(&res->rows[0].vals[i], val[i], (uint32_t)strlen(val[i]), NLSQL_TYPE_VARCHAR);
    *out = res;
    return NLSQL_OK;
}

static int show_table_status(nlsql_conn_t* c, engine_t* e, parsed_stmt_t* st, nlsql_result_t** out) {
    database_t* db = e->cur_db;
    if (st->ident3[0]) {
        db = find_loaded_db(e, st->ident3);
        if (!db) { /* 未加载（懒加载）时按当前库回答，避免读锁下换库 */
            nlsql_set_err(c, NLSQL_ER_NO_SUCH_DB, "42000", "Unknown database '%s'", st->ident3);
            return NLSQL_ER_NO_SUCH_DB;
        }
    }
    static const char* hdr[18] = {
        "Name", "Engine", "Version", "Row_format", "Rows", "Avg_row_length",
        "Data_length", "Max_data_length", "Index_length", "Data_free",
        "Auto_increment", "Create_time", "Update_time", "Check_time",
        "Collation", "Checksum", "Create_options", "Comment"
    };
    nlsql_result_t* res = res_new(18);
    if (!res) return NLSQL_ERR;
    for (int i = 0; i < 18; i++) {
        snprintf(res->cols[i].name, sizeof(res->cols[i].name), "%s", hdr[i]);
        res->cols[i].type = NLSQL_TYPE_VARCHAR;
    }
    for (table_t* t = db ? db->tables : NULL; t; t = t->next) {
        if (st->has_like && !like_match(t->name, st->like_pat)) continue;
        if (res_add_row(res, 18) != 0) { nlsql_free_result(res); return NLSQL_ERR; }
        nlsql_value_t* v = res->rows[res->row_count - 1].vals;
        char nums[8][32];
        val_set_str(&v[0], t->name, (uint32_t)strlen(t->name), NLSQL_TYPE_VARCHAR);
        val_set_str(&v[1], "NLSQL", 5, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[2], "10", 2, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[3], "Dynamic", 7, NLSQL_TYPE_VARCHAR);
        snprintf(nums[0], sizeof(nums[0]), "%llu", (unsigned long long)t->row_count);
        val_set_str(&v[4], nums[0], (uint32_t)strlen(nums[0]), NLSQL_TYPE_VARCHAR);
        val_set_str(&v[5], "0", 1, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[6], "0", 1, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[7], "0", 1, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[8], "0", 1, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[9], "0", 1, NLSQL_TYPE_VARCHAR);
        snprintf(nums[1], sizeof(nums[1]), "%lld", (long long)t->next_auto);
        val_set_str(&v[10], nums[1], (uint32_t)strlen(nums[1]), NLSQL_TYPE_VARCHAR);
        for (int i = 11; i <= 13; i++) val_set_str(&v[i], "", 0, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[14], "utf8_general_ci", 15, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[15], "", 0, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[16], "", 0, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[17], "", 0, NLSQL_TYPE_VARCHAR);
    }
    *out = res;
    return NLSQL_OK;
}

static int show_full_tables(engine_t* e, parsed_stmt_t* st, nlsql_result_t** out) {
    database_t* db = e->cur_db;
    if (st->ident3[0]) {
        db = find_loaded_db(e, st->ident3);
        if (!db) db = e->cur_db;
    }
    nlsql_result_t* res = res_new(2);
    if (!res) return NLSQL_ERR;
    snprintf(res->cols[0].name, sizeof(res->cols[0].name), "Tables_in_%.53s",
             db ? db->name : "");
    snprintf(res->cols[1].name, sizeof(res->cols[1].name), "Table_type");
    res->cols[0].type = res->cols[1].type = NLSQL_TYPE_VARCHAR;
    for (table_t* t = db ? db->tables : NULL; t; t = t->next) {
        if (st->has_like && !like_match(t->name, st->like_pat)) continue;
        if (res_add_row(res, 2) != 0) { nlsql_free_result(res); return NLSQL_ERR; }
        nlsql_value_t* v = res->rows[res->row_count - 1].vals;
        val_set_str(&v[0], t->name, (uint32_t)strlen(t->name), NLSQL_TYPE_VARCHAR);
        val_set_str(&v[1], "BASE TABLE", 10, NLSQL_TYPE_VARCHAR);
    }
    *out = res;
    return NLSQL_OK;
}

static int show_full_columns(nlsql_conn_t* c, engine_t* e, parsed_stmt_t* st, nlsql_result_t** out) {
    database_t* db = e->cur_db;
    if (st->ident3[0]) {
        database_t* d = find_loaded_db(e, st->ident3);
        if (d) db = d;
    }
    table_t* t = NULL;
    for (table_t* it = db ? db->tables : NULL; it; it = it->next)
        if (strcmp(it->name, st->ident2) == 0) { t = it; break; }
    if (!t) {
        nlsql_set_err(c, NLSQL_ER_NO_SUCH_TABLE, "42S02",
                      "Table '%s.%s' doesn't exist", db ? db->name : "", st->ident2);
        return NLSQL_ER_NO_SUCH_TABLE;
    }
    static const char* hdr[9] = {
        "Field", "Type", "Collation", "Null", "Key", "Default", "Extra", "Privileges", "Comment"
    };
    nlsql_result_t* res = res_new(9);
    if (!res) return NLSQL_ERR;
    for (int i = 0; i < 9; i++) {
        snprintf(res->cols[i].name, sizeof(res->cols[i].name), "%s", hdr[i]);
        res->cols[i].type = NLSQL_TYPE_VARCHAR;
    }
    for (int i = 0; i < t->col_count; i++) {
        if (st->has_like && !like_match(t->cols[i].name, st->like_pat)) continue;
        if (res_add_row(res, 9) != 0) { nlsql_free_result(res); return NLSQL_ERR; }
        nlsql_value_t* v = res->rows[res->row_count - 1].vals;
        char tbuf[48], dbuf[64];
        col_type_text(&t->cols[i], tbuf, sizeof(tbuf));
        val_set_str(&v[0], t->cols[i].name, (uint32_t)strlen(t->cols[i].name), NLSQL_TYPE_VARCHAR);
        val_set_str(&v[1], tbuf, (uint32_t)strlen(tbuf), NLSQL_TYPE_VARCHAR);
        val_set_str(&v[2], "utf8_general_ci", 15, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[3], t->cols[i].not_null ? "NO" : "YES", t->cols[i].not_null ? 2 : 3, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[4], t->cols[i].pk ? "PRI" : "", t->cols[i].pk ? 3 : 0, NLSQL_TYPE_VARCHAR);
        if (t->cols[i].has_def && !t->cols[i].def.is_null) {
            nlsql_value_t* d = &t->cols[i].def;
            if (d->type == NLSQL_TYPE_INT64) snprintf(dbuf, sizeof(dbuf), "%lld", (long long)d->i);
            else if (d->type == NLSQL_TYPE_DOUBLE) snprintf(dbuf, sizeof(dbuf), "%.10g", d->d);
            else snprintf(dbuf, sizeof(dbuf), "%s", d->s ? d->s : "");
            val_set_str(&v[5], dbuf, (uint32_t)strlen(dbuf), NLSQL_TYPE_VARCHAR);
        } else {
            v[5].type = NLSQL_TYPE_NULL; v[5].is_null = 1;
        }
        val_set_str(&v[6], t->cols[i].auto_inc ? "auto_increment" : "",
                    t->cols[i].auto_inc ? 14 : 0, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[7], "select,insert,update,references", 31, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[8], "", 0, NLSQL_TYPE_VARCHAR);
    }
    *out = res;
    return NLSQL_OK;
}

/* CREATE [UNIQUE] INDEX（扩展特性 "index" 打开后可用） */
static int exec_create_index(nlsql_conn_t* c, engine_t* e, parsed_stmt_t* st, nlsql_result_t** out) {
    if (!nlsql_ext_feature("index")) {
        nlsql_set_err(c, NLSQL_ER_NOT_SUPPORTED, "42000",
                      "CREATE INDEX requires the nlsql_ext_store extension");
        return NLSQL_ER_NOT_SUPPORTED;
    }
    table_t* t = nlsql_tbl_find(e, st->ident2);
    if (!t) {
        nlsql_set_err(c, NLSQL_ER_NO_SUCH_TABLE, "42S02",
                      "Table '%s.%s' doesn't exist", e->cur_db ? e->cur_db->name : "", st->ident2);
        return NLSQL_ER_NO_SUCH_TABLE;
    }
    int cols[8];
    for (int i = 0; i < st->idx_ncol; i++) {
        cols[i] = col_index(t, st->idx_cols[i]);
        if (cols[i] < 0) {
            nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22",
                          "Key column '%s' doesn't exist in table", st->idx_cols[i]);
            return NLSQL_ER_UNKNOWN_COLUMN;
        }
    }
    char err[160] = "";
    int rc = nlsql_idx_create(t, st->ident, cols, st->idx_ncol, st->idx_unique, err, sizeof(err));
    if (rc != NLSQL_OK) {
        int code = (rc == NLSQL_ER_DUP_ENTRY) ? NLSQL_ER_DUP_ENTRY : NLSQL_ER_NOT_SUPPORTED;
        nlsql_set_err(c, code, "42000", "%s", err[0] ? err : "cannot create index");
        return code;
    }
    nlsql_result_t* res = res_new(0);
    if (!res) return NLSQL_ERR;
    *out = res;
    return NLSQL_OK;
}

static int exec_drop_index(nlsql_conn_t* c, engine_t* e, parsed_stmt_t* st, nlsql_result_t** out) {
    if (!nlsql_ext_feature("index")) {
        nlsql_set_err(c, NLSQL_ER_NOT_SUPPORTED, "42000",
                      "DROP INDEX requires the nlsql_ext_store extension");
        return NLSQL_ER_NOT_SUPPORTED;
    }
    table_t* t = nlsql_tbl_find(e, st->ident2);
    if (!t) {
        nlsql_set_err(c, NLSQL_ER_NO_SUCH_TABLE, "42S02",
                      "Table '%s.%s' doesn't exist", e->cur_db ? e->cur_db->name : "", st->ident2);
        return NLSQL_ER_NO_SUCH_TABLE;
    }
    if (nlsql_idx_drop(t, st->ident) != NLSQL_OK) {
        nlsql_set_err(c, 1091, "42000", "Can't DROP '%s'; check that it exists", st->ident);
        return 1091;
    }
    nlsql_result_t* res = res_new(0);
    if (!res) return NLSQL_ERR;
    *out = res;
    return NLSQL_OK;
}

static int show_index(nlsql_conn_t* c, engine_t* e, parsed_stmt_t* st, nlsql_result_t** out) {
    database_t* db = e->cur_db;
    if (st->ident3[0]) {
        database_t* d = find_loaded_db(e, st->ident3);
        if (d) db = d;
    }
    table_t* t = NULL;
    for (table_t* it = db ? db->tables : NULL; it; it = it->next)
        if (strcmp(it->name, st->ident2) == 0) { t = it; break; }
    if (!t) {
        nlsql_set_err(c, NLSQL_ER_NO_SUCH_TABLE, "42S02",
                      "Table '%s.%s' doesn't exist", db ? db->name : "", st->ident2);
        return NLSQL_ER_NO_SUCH_TABLE;
    }
    static const char* hdr[13] = {
        "Table", "Non_unique", "Key_name", "Seq_in_index", "Column_name", "Collation",
        "Cardinality", "Sub_part", "Packed", "Null", "Index_type", "Comment", "Index_comment"
    };
    nlsql_result_t* res = res_new(13);
    if (!res) return NLSQL_ERR;
    for (int i = 0; i < 13; i++) {
        snprintf(res->cols[i].name, sizeof(res->cols[i].name), "%s", hdr[i]);
        res->cols[i].type = NLSQL_TYPE_VARCHAR;
    }
    for (int i = 0; i < t->col_count; i++) {
        if (!t->cols[i].pk) continue;
        if (res_add_row(res, 13) != 0) { nlsql_free_result(res); return NLSQL_ERR; }
        nlsql_value_t* v = res->rows[res->row_count - 1].vals;
        char card[32];
        snprintf(card, sizeof(card), "%llu", (unsigned long long)t->row_count);
        val_set_str(&v[0], t->name, (uint32_t)strlen(t->name), NLSQL_TYPE_VARCHAR);
        val_set_str(&v[1], "0", 1, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[2], "PRIMARY", 7, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[3], "1", 1, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[4], t->cols[i].name, (uint32_t)strlen(t->cols[i].name), NLSQL_TYPE_VARCHAR);
        val_set_str(&v[5], "A", 1, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[6], card, (uint32_t)strlen(card), NLSQL_TYPE_VARCHAR);
        v[7].type = NLSQL_TYPE_NULL; v[7].is_null = 1;
        v[8].type = NLSQL_TYPE_NULL; v[8].is_null = 1;
        val_set_str(&v[9], "", 0, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[10], "BTREE", 5, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[11], "", 0, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[12], "", 0, NLSQL_TYPE_VARCHAR);
    }
    /* 二级索引（普通/唯一/复合） */
    for (tbl_index_t* ix = t->indexes; ix; ix = ix->next) {
        char card[32], nonuniq[8], seq[16];
        snprintf(card, sizeof(card), "%llu", (unsigned long long)ix->n);
        snprintf(nonuniq, sizeof(nonuniq), "%d", ix->unique ? 0 : 1);
        for (int k = 0; k < ix->ncols; k++) {
            if (res_add_row(res, 13) != 0) { nlsql_free_result(res); return NLSQL_ERR; }
            nlsql_value_t* v = res->rows[res->row_count - 1].vals;
            snprintf(seq, sizeof(seq), "%d", k + 1);
            const char* cn = (ix->cols[k] >= 0 && ix->cols[k] < t->col_count) ? t->cols[ix->cols[k]].name : "";
            val_set_str(&v[0], t->name, (uint32_t)strlen(t->name), NLSQL_TYPE_VARCHAR);
            val_set_str(&v[1], nonuniq, (uint32_t)strlen(nonuniq), NLSQL_TYPE_VARCHAR);
            val_set_str(&v[2], ix->name, (uint32_t)strlen(ix->name), NLSQL_TYPE_VARCHAR);
            val_set_str(&v[3], seq, (uint32_t)strlen(seq), NLSQL_TYPE_VARCHAR);
            val_set_str(&v[4], cn, (uint32_t)strlen(cn), NLSQL_TYPE_VARCHAR);
            val_set_str(&v[5], "A", 1, NLSQL_TYPE_VARCHAR);
            val_set_str(&v[6], card, (uint32_t)strlen(card), NLSQL_TYPE_VARCHAR);
            v[7].type = NLSQL_TYPE_NULL; v[7].is_null = 1;
            v[8].type = NLSQL_TYPE_NULL; v[8].is_null = 1;
            val_set_str(&v[9], "", 0, NLSQL_TYPE_VARCHAR);
            val_set_str(&v[10], "BTREE", 5, NLSQL_TYPE_VARCHAR);
            val_set_str(&v[11], "", 0, NLSQL_TYPE_VARCHAR);
            val_set_str(&v[12], "", 0, NLSQL_TYPE_VARCHAR);
        }
    }
    *out = res;
    return NLSQL_OK;
}

static int show_create_table(nlsql_conn_t* c, engine_t* e, const char* name,
                             nlsql_result_t** out) {
    table_t* t = nlsql_tbl_find(e, name);
    if (!t) {
        nlsql_set_err(c, NLSQL_ER_NO_SUCH_TABLE, "42S02",
                      "Table '%s.%s' doesn't exist", e->cur_db ? e->cur_db->name : "", name);
        return NLSQL_ER_NO_SUCH_TABLE;
    }
    sbuf_t b;
    if (sbuf_init(&b, 512) != 0) return NLSQL_ERR;
    sbuf_printf(&b, "CREATE TABLE `%s` (\n", t->name);
    int pk_col = -1;
    for (int i = 0; i < t->col_count; i++) {
        char ty[48];
        col_type_text(&t->cols[i], ty, sizeof(ty));
        sbuf_printf(&b, "  `%s` %s", t->cols[i].name, ty);
        sbuf_append_str(&b, (t->cols[i].not_null || t->cols[i].pk) ? " NOT NULL" : " DEFAULT NULL");
        if (t->cols[i].auto_inc) sbuf_append_str(&b, " AUTO_INCREMENT");
        if (t->cols[i].pk && pk_col < 0) pk_col = i;
        sbuf_append_str(&b, ",\n");
    }
    if (pk_col >= 0) sbuf_printf(&b, "  PRIMARY KEY (`%s`)\n", t->cols[pk_col].name);
    sbuf_printf(&b, ") ENGINE=NLSQL DEFAULT CHARSET=utf8");

    nlsql_result_t* res = res_new(2);
    if (!res) { sbuf_free(&b); return NLSQL_ERR; }
    snprintf(res->cols[0].name, sizeof(res->cols[0].name), "Table");
    snprintf(res->cols[1].name, sizeof(res->cols[1].name), "Create Table");
    res->cols[0].type = res->cols[1].type = NLSQL_TYPE_VARCHAR;
    if (res_add_row(res, 2) != 0) { sbuf_free(&b); nlsql_free_result(res); return NLSQL_ERR; }
    val_set_str(&res->rows[0].vals[0], t->name, (uint32_t)strlen(t->name), NLSQL_TYPE_VARCHAR);
    val_set_str(&res->rows[0].vals[1], b.p, b.len, NLSQL_TYPE_VARCHAR);
    sbuf_free(&b);
    *out = res;
    return NLSQL_OK;
}

static int show_create_db(nlsql_conn_t* c, engine_t* e, const char* name,
                          nlsql_result_t** out) {
    const char* dbn = (name && name[0]) ? name : (e->cur_db ? e->cur_db->name : "");
    if (!dbn[0]) {
        nlsql_set_err(c, 1046, "3D000", "No database selected");
        return 1046;
    }
    if (!find_loaded_db(e, dbn)) {
        nlsql_set_err(c, 1049, "42000", "Unknown database '%s'", dbn);
        return 1049;
    }
    char ddl[128];
    snprintf(ddl, sizeof(ddl), "CREATE DATABASE `%s` /*!40100 DEFAULT CHARACTER SET utf8 */", dbn);
    nlsql_result_t* res = res_new(2);
    if (!res) return NLSQL_ERR;
    snprintf(res->cols[0].name, sizeof(res->cols[0].name), "Database");
    snprintf(res->cols[1].name, sizeof(res->cols[1].name), "Create Database");
    res->cols[0].type = res->cols[1].type = NLSQL_TYPE_VARCHAR;
    if (res_add_row(res, 2) != 0) { nlsql_free_result(res); return NLSQL_ERR; }
    val_set_str(&res->rows[0].vals[0], dbn, (uint32_t)strlen(dbn), NLSQL_TYPE_VARCHAR);
    val_set_str(&res->rows[0].vals[1], ddl, (uint32_t)strlen(ddl), NLSQL_TYPE_VARCHAR);
    *out = res;
    return NLSQL_OK;
}

static int show_warnings(nlsql_result_t** out) {
    static const char* hdr[3] = { "Level", "Code", "Message" };
    nlsql_result_t* res = res_new(3);
    if (!res) return NLSQL_ERR;
    for (int i = 0; i < 3; i++) {
        snprintf(res->cols[i].name, sizeof(res->cols[i].name), "%s", hdr[i]);
        res->cols[i].type = NLSQL_TYPE_VARCHAR;
    }
    *out = res;  /* 无告警：空结果集 */
    return NLSQL_OK;
}

static int exec_show_db(nlsql_conn_t* c, engine_t* e, nlsql_result_t** out) {
    (void)c;
    nlsql_result_t* res = res_new(1);
    if (!res) return NLSQL_ERR;
    snprintf(res->cols[0].name, sizeof(res->cols[0].name), "Database");
    res->cols[0].type = NLSQL_TYPE_VARCHAR;
    /* 扫描数据目录下的子目录（懒加载下已加载链表不全） */
    DIR* d = opendir(e->root);
    if (d) {
        struct dirent* de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char path[1360]; /* 覆盖 root(1023)+"/"+d_name(255) 最坏情况，防截断 */
            snprintf(path, sizeof(path), "%s/%s", e->root, de->d_name);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            if (strlen(de->d_name) > NLSQL_MAX_IDENT) continue;
            if (res_add_row(res, 1) != 0) { closedir(d); nlsql_free_result(res); return NLSQL_ERR; }
            val_set_str(&res->rows[res->row_count - 1].vals[0], de->d_name,
                        (uint32_t)strlen(de->d_name), NLSQL_TYPE_VARCHAR);
        }
        closedir(d);
    }
    *out = res;
    return NLSQL_OK;
}

static int exec_show_tables(nlsql_conn_t* c, engine_t* e, parsed_stmt_t* st, nlsql_result_t** out) {
    (void)c;
    database_t* db = e->cur_db;
    if (st && st->ident3[0]) {
        database_t* d = find_loaded_db(e, st->ident3);
        if (d) db = d;
    }
    nlsql_result_t* res = res_new(1);
    if (!res) return NLSQL_ERR;
    snprintf(res->cols[0].name, sizeof(res->cols[0].name), "Tables_in_%.53s", db ? db->name : "");
    res->cols[0].type = NLSQL_TYPE_VARCHAR;
    for (table_t* t = db ? db->tables : NULL; t; t = t->next) {
        if (st && st->has_like && !like_match(t->name, st->like_pat)) continue;
        if (res_add_row(res, 1) != 0) { nlsql_free_result(res); return NLSQL_ERR; }
        val_set_str(&res->rows[res->row_count - 1].vals[0], t->name, (uint32_t)strlen(t->name), NLSQL_TYPE_VARCHAR);
    }
    *out = res;
    return NLSQL_OK;
}

static int exec_show_columns(nlsql_conn_t* c, engine_t* e, const char* name, nlsql_result_t** out) {
    table_t* t = nlsql_tbl_find(e, name);
    if (!t) {
        nlsql_set_err(c, NLSQL_ER_NO_SUCH_TABLE, "42S02",
                      "Table '%s.%s' doesn't exist", e->cur_db->name, name);
        return NLSQL_ER_NO_SUCH_TABLE;
    }
    nlsql_result_t* res = res_new(4);
    if (!res) return NLSQL_ERR;
    snprintf(res->cols[0].name, sizeof(res->cols[0].name), "Field");
    snprintf(res->cols[1].name, sizeof(res->cols[1].name), "Type");
    snprintf(res->cols[2].name, sizeof(res->cols[2].name), "Null");
    snprintf(res->cols[3].name, sizeof(res->cols[3].name), "Key");
    for (int i = 0; i < 4; i++) res->cols[i].type = NLSQL_TYPE_VARCHAR;
    for (int i = 0; i < t->col_count; i++) {
        if (res_add_row(res, 4) != 0) { nlsql_free_result(res); return NLSQL_ERR; }
        nlsql_value_t* v = res->rows[res->row_count - 1].vals;
        const char* tn = "text";
        char tbuf[32];
        switch (t->cols[i].type) {
            case NLSQL_TYPE_INT64: tn = "bigint"; break;
            case NLSQL_TYPE_DOUBLE: tn = "double"; break;
            case NLSQL_TYPE_VARCHAR:
                snprintf(tbuf, sizeof(tbuf), "varchar(%u)", t->cols[i].vlen); tn = tbuf; break;
            case NLSQL_TYPE_DATETIME: tn = "datetime"; break;
            default: break;
        }
        val_set_str(&v[0], t->cols[i].name, (uint32_t)strlen(t->cols[i].name), NLSQL_TYPE_VARCHAR);
        val_set_str(&v[1], tn, (uint32_t)strlen(tn), NLSQL_TYPE_VARCHAR);
        val_set_str(&v[2], t->cols[i].not_null ? "NO" : "YES", 3, NLSQL_TYPE_VARCHAR);
        val_set_str(&v[3], t->cols[i].pk ? "PRI" : "", t->cols[i].pk ? 3 : 0, NLSQL_TYPE_VARCHAR);
    }
    *out = res;
    return NLSQL_OK;
}

static int exec_select_const(nlsql_conn_t* c, engine_t* e, parsed_stmt_t* st,
                             nlsql_value_t* params, int nparams, nlsql_result_t** out) {
    int n = st->const_count;
    nlsql_result_t* res = res_new(n);
    if (!res) return NLSQL_ERR;
    if (res_add_row(res, n) != 0) { nlsql_free_result(res); return NLSQL_ERR; }
    nlsql_value_t* v = res->rows[0].vals;
    for (int i = 0; i < n; i++) {
        expr_t* ex = st->consts[i];
        char dname[80];
        dname[0] = '\0';
        if (ex && ex->kind == EX_SYSVAR) {
            const char* val = NULL;
            if (sysvar_lookup(ex->s, &val))
                val_set_str(&v[i], val, (uint32_t)strlen(val), NLSQL_TYPE_VARCHAR);
            else { v[i].type = NLSQL_TYPE_NULL; v[i].is_null = 1; }
            snprintf(dname, sizeof(dname), "@@%s", ex->s ? ex->s : "");
        } else if (ex && ex->kind == EX_FUNC) {
            meta_func_eval(c, e, (int)ex->i, &v[i]);
            switch ((int)ex->i) {
                case MF_DATABASE:        snprintf(dname, sizeof(dname), "DATABASE()"); break;
                case MF_VERSION:         snprintf(dname, sizeof(dname), "VERSION()"); break;
                case MF_USER:            snprintf(dname, sizeof(dname), "USER()"); break;
                case MF_CONNECTION_ID:   snprintf(dname, sizeof(dname), "CONNECTION_ID()"); break;
                case MF_LAST_INSERT_ID:  snprintf(dname, sizeof(dname), "LAST_INSERT_ID()"); break;
                default:                 snprintf(dname, sizeof(dname), "ROW_COUNT()"); break;
            }
        } else {
            char em[128];
            if (eval_expr(ex, NULL, NULL, params, nparams, &v[i], em, sizeof(em)) != 0) {
                nlsql_set_err(c, NLSQL_ER_SYNTAX, "42000", "%s", em);
                nlsql_free_result(res);
                return NLSQL_ER_SYNTAX;
            }
            snprintf(dname, sizeof(dname), "%d", i + 1);
        }
        const char* alias = st->const_alias ? st->const_alias[i] : "";
        snprintf(res->cols[i].name, sizeof(res->cols[i].name), "%.64s", alias[0] ? alias : dname);
        res->cols[i].type = v[i].type == NLSQL_TYPE_NULL ? NLSQL_TYPE_VARCHAR : v[i].type;
    }
    *out = res;
    return NLSQL_OK;
}

/* =========================================
 * 分发
 * ========================================= */

static int exec_update(nlsql_conn_t* c, engine_t* e, update_stmt_t* upd,
                       nlsql_value_t* params, int nparams, nlsql_result_t** out);
static int exec_delete(nlsql_conn_t* c, engine_t* e, delete_stmt_t* del,
                       nlsql_value_t* params, int nparams, nlsql_result_t** out);
/* 语句执行统一出口。
 * journal 采用用户态写缓冲（64KB）：不在每语句尾落盘（每次 write ≈ 29µs，
 * 批量插入时占 99% 耗时），只在缓冲满 / 压实 / 切库 / 关闭 / 显式 flush 时落盘。
 * 进程崩溃最多回退 ≤64KB 未落盘操作，与快照状态保持一致（内存与缓冲同失）；
 * 需要持久化的调用方用 nlsql_host_flush()/eng_flush 显式落盘。 */
int eng_execute(nlsql_conn_t* c, engine_t* e, parsed_stmt_t* st,
                nlsql_value_t* params, int nparams, nlsql_result_t** out) {
    if (out) *out = NULL;

    /* 只读连接拒绝写语句（MySQL --read-only 同款：errno 1290 / SQLSTATE HY000） */
    if (e->readonly && !st->readonly_stmt) {
        nlsql_set_err(c, 1290, "HY000",
                      "The server is running in read-only mode so it cannot execute this statement");
        return 1290;
    }

    /* 需要选中库的语句 */
    int need_db = (st->kind != ST_CREATE_DB && st->kind != ST_DROP_DB &&
                   st->kind != ST_USE && st->kind != ST_SHOW_DB &&
                   st->kind != ST_SELECT_CONST && st->kind != ST_SET &&
                   st->kind != ST_SHOW_VARIABLES && st->kind != ST_SHOW_STATUS &&
                   st->kind != ST_SHOW_ENGINES && st->kind != ST_SHOW_CHARSET &&
                   st->kind != ST_SHOW_COLLATION && st->kind != ST_SHOW_WARNINGS &&
                   st->kind != ST_SHOW_CREATE_DB);
    if (need_db && !e->cur_db) {
        nlsql_set_err(c, 1046, "3D000", "No database selected");
        return 1046;
    }

    switch (st->kind) {
        case ST_SELECT:        return exec_select(c, e, &st->u.sel, params, nparams, out);
        case ST_SELECT_CONST:  return exec_select_const(c, e, st, params, nparams, out);
        case ST_INSERT:        return exec_insert(c, e, &st->u.ins, params, nparams, out);
        case ST_UPDATE:        return exec_update(c, e, &st->u.upd, params, nparams, out);
        case ST_DELETE:        return exec_delete(c, e, &st->u.del, params, nparams, out);
        case ST_CREATE_TABLE:  return exec_create_table(c, e, &st->u.ct, out);
        case ST_DROP_TABLE:    return exec_drop_table(c, e, st->ident, st->if_exists, out);
        case ST_TRUNCATE:      return exec_truncate(c, e, st->ident, out);
        case ST_ALTER_TABLE:   return exec_alter(c, e, &st->u.alt, out);
        case ST_SHOW_DB:       return exec_show_db(c, e, out);
        case ST_SHOW_TABLES:   return exec_show_tables(c, e, st, out);
        case ST_SHOW_COLUMNS:  return exec_show_columns(c, e, st->ident2, out);
        case ST_SHOW_FULL_TABLES:  return show_full_tables(e, st, out);
        case ST_SHOW_FULL_COLUMNS: return show_full_columns(c, e, st, out);
        case ST_SHOW_TABLE_STATUS: return show_table_status(c, e, st, out);
        case ST_SHOW_VARIABLES:    return show_variables(c, st, out);
        case ST_SHOW_STATUS:       return show_status(st, out);
        case ST_SHOW_ENGINES:      return show_engines(out);
        case ST_SHOW_CHARSET:      return show_charset(out);
        case ST_SHOW_COLLATION:    return show_collation(out);
        case ST_SHOW_INDEX:        return show_index(c, e, st, out);
        case ST_CREATE_INDEX:      return exec_create_index(c, e, st, out);
        case ST_DROP_INDEX:        return exec_drop_index(c, e, st, out);
        case ST_SHOW_CREATE_TABLE: return show_create_table(c, e, st->ident, out);
        case ST_SHOW_CREATE_DB:    return show_create_db(c, e, st->ident, out);
        case ST_SHOW_WARNINGS:     return show_warnings(out);
        case ST_SET:           return NLSQL_OK; /* SET 配置语句：no-op，返回 OK */
        case ST_CREATE_DB: {
            int rc = nlsql_db_create(e, st->ident);
            if (rc != NLSQL_OK) {
                if (rc == NLSQL_ERR_PATH) nlsql_set_err(c, NLSQL_ERR_PATH, "42000", "Invalid database name");
                else nlsql_set_err(c, NLSQL_ERR_IO, "HY000", "Cannot create database '%s'", st->ident);
                return rc;
            }
            return NLSQL_OK;
        }
        case ST_DROP_DB: {
            int rc = nlsql_db_drop(e, st->ident);
            if (rc == NLSQL_ER_NO_SUCH_DB && st->if_exists) return NLSQL_OK;  /* DROP DATABASE IF EXISTS */
            if (rc != NLSQL_OK) {
                if (rc == NLSQL_ER_NO_SUCH_DB) nlsql_set_err(c, NLSQL_ER_NO_SUCH_DB, "42000",
                              "Can't drop database '%s'; database doesn't exist", st->ident);
                else nlsql_set_err(c, rc, "HY000", "Error dropping database '%s'", st->ident);
                return rc;
            }
            return NLSQL_OK;
        }
        case ST_USE: {
            int rc = nlsql_db_load(e, st->ident);
            if (rc != NLSQL_OK) {
                nlsql_set_err(c, NLSQL_ER_NO_SUCH_DB, "42000",
                              "Unknown database '%s'", st->ident);
                return NLSQL_ER_NO_SUCH_DB;
            }
            return NLSQL_OK;
        }
        default:
            nlsql_set_err(c, NLSQL_ER_NOT_SUPPORTED, "42000", "Unsupported statement");
            return NLSQL_ER_NOT_SUPPORTED;
    }
}

static int journal_row(engine_t* e, uint8_t type, table_t* t, uint64_t idx, row_t* row) {
    sbuf_t b;
    if (sbuf_init(&b, 128) != 0) return NLSQL_ERR;
    nlsql_jr_put_str(&b, t->name);
    sbuf_append_u64(&b, idx);
    if (row) for (int i = 0; i < t->col_count; i++) nlsql_sbuf_enc_value(&b, &row->vals[i]);
    int rc = nlsql_jr_append(e, type, b.p, b.len);
    sbuf_free(&b);
    return rc == NLSQL_OK ? NLSQL_OK : NLSQL_ERR_IO;
}

static int exec_update(nlsql_conn_t* c, engine_t* e, update_stmt_t* upd,
                       nlsql_value_t* params, int nparams, nlsql_result_t** out) {
    table_t* t = nlsql_tbl_find(e, upd->table);
    /* 错误出口统一释放本行 newvals 副本（成功行已在 memcpy 后由下一行的重建覆盖，不会误释放） */
#define UPD_ROWCLEAN() do { for (int k = 0; k < t->col_count; k++) free(newvals[k].s); } while (0)
    if (upd->set_count > NLSQL_MAX_COLUMNS) { /* 防御：SET 项超上限会越界写 setidx[] */
        nlsql_set_err(c, NLSQL_ER_NOT_SUPPORTED, "42000", "Too many SET items");
        return NLSQL_ER_NOT_SUPPORTED;
    }
    if (!t) {
        nlsql_set_err(c, NLSQL_ER_NO_SUCH_TABLE, "42S02",
                      "Table '%s.%s' doesn't exist", e->cur_db->name, upd->table);
        return NLSQL_ER_NO_SUCH_TABLE;
    }
    int setidx[NLSQL_MAX_COLUMNS];
    for (int i = 0; i < upd->set_count; i++) {
        setidx[i] = col_index(t, upd->cols[i]);
        if (setidx[i] < 0) {
            nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22",
                          "Unknown column '%s' in 'field list'", upd->cols[i]);
            return NLSQL_ER_UNKNOWN_COLUMN;
        }
    }
    uint64_t affected = 0;
    /* 主键索引：单列主键 O(1) 查重 */
    int pkc = -1, npk = 0;
    for (int i = 0; i < t->col_count; i++) if (t->cols[i].pk) { npk++; pkc = i; }
    int pk_idx_ok = (npk == 1) && (nlsql_pk_ensure(t) == NLSQL_OK);
    int pk_in_set = 0;
    for (int i = 0; i < upd->set_count; i++) if (setidx[i] == pkc) pk_in_set = 1;
    for (uint64_t r = 0; r < t->row_count; r++) {
        char em[128];
        int m = eval_cond(upd->where, t, &t->rows[r], params, nparams, em, sizeof(em));
        if (m < 0) { nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22", "%s", em); return NLSQL_ER_UNKNOWN_COLUMN; }
        if (!m) continue;
        /* 计算新行（副本） */
        nlsql_value_t newvals[NLSQL_MAX_COLUMNS];
        memcpy(newvals, t->rows[r].vals, (size_t)t->col_count * sizeof(nlsql_value_t));
        for (int i = 0; i < t->col_count; i++) {
            /* 字符串值需要复制，避免与旧行共享 */
            if (newvals[i].s) {
                newvals[i].s = strdup(newvals[i].s);
            }
        }
        for (int i = 0; i < upd->set_count; i++) {
            nlsql_value_t v;
            if (eval_expr(upd->vals[i], t, &t->rows[r], params, nparams, &v, em, sizeof(em)) != 0) {
                nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22", "%s", em);
                UPD_ROWCLEAN(); return NLSQL_ER_UNKNOWN_COLUMN;
            }
            tbl_col_t* tc = &t->cols[setidx[i]];
            if (!v.is_null && tc->type == NLSQL_TYPE_INT64 && v.type != NLSQL_TYPE_INT64) {
                int64_t iv; value_to_int(&v, &iv); free(v.s); v.s = NULL; v.type = NLSQL_TYPE_INT64; v.i = iv;
            } else if (!v.is_null && tc->type == NLSQL_TYPE_DOUBLE && v.type != NLSQL_TYPE_DOUBLE) {
                double dv; value_to_double(&v, &dv); free(v.s); v.s = NULL; v.type = NLSQL_TYPE_DOUBLE; v.d = dv;
            } else if (!v.is_null && (tc->type == NLSQL_TYPE_VARCHAR || tc->type == NLSQL_TYPE_TEXT || tc->type == NLSQL_TYPE_DATETIME)
                       && v.type != NLSQL_TYPE_VARCHAR && v.type != NLSQL_TYPE_TEXT && v.type != NLSQL_TYPE_DATETIME) {
                /* 与 INSERT 一致的字符串归一（保证列类型一致与主键索引哈希一致） */
                char buf[64];
                const char* txt = NULL;
                uint32_t len;
                if (v.type == NLSQL_TYPE_INT64) { len = (uint32_t)snprintf(buf, sizeof(buf), "%lld", (long long)v.i); txt = buf; }
                else { len = (uint32_t)snprintf(buf, sizeof(buf), "%g", v.d); txt = buf; }
                free(v.s);
                v.s = (char*)malloc((size_t)len + 1);
                if (v.s) { memcpy(v.s, txt, len); v.s[len] = '\0'; }
                v.type = NLSQL_TYPE_VARCHAR; v.len = len;
            }
            if (tc->not_null && v.is_null) {
                free(v.s);
                nlsql_set_err(c, NLSQL_ER_BAD_NULL_ERROR, "23000", "Column '%s' cannot be null", tc->name);
                UPD_ROWCLEAN(); return NLSQL_ER_BAD_NULL_ERROR;
            }
            if (tc->pk && !v.is_null) {
                int dup = 0;
                if (pk_idx_ok) {
                    uint64_t hit = 0;
                    if (nlsql_pk_lookup(t, &v, &hit) && hit != r) dup = 1;
                } else {
                    for (uint64_t x = 0; x < t->row_count; x++) {
                        if (x == r) continue;
                        nlsql_value_t* ev = &t->rows[x].vals[setidx[i]];
                        int rel;
                        if (!ev->is_null && cmp_values(&v, ev, &rel) == 0 && rel == 0) { dup = 1; break; }
                    }
                }
                if (dup) {
                    free(v.s);
                    nlsql_set_err(c, NLSQL_ER_DUP_ENTRY, "23000", "Duplicate entry for key 'PRIMARY'");
                    UPD_ROWCLEAN(); return NLSQL_ER_DUP_ENTRY;
                }
            }
            free(newvals[setidx[i]].s);
            newvals[setidx[i]] = v;
        }
        row_t tmp; tmp.vals = newvals;
        if (journal_row(e, JR_UPDATE, t, r, &tmp) != NLSQL_OK) {
            nlsql_set_err(c, NLSQL_ERR_IO, "HY000", "journal write failed");
            UPD_ROWCLEAN(); return NLSQL_ERR_IO;
        }
        /* 替换内存行：释放旧值（主键索引先移除旧键，s 尚未释放） */
        if (pk_idx_ok && pk_in_set && pkc >= 0) {
            nlsql_value_t oldpk = t->rows[r].vals[pkc];
            nlsql_pk_remove(t, r, &oldpk);
        }
        for (int i = 0; i < t->col_count; i++) free(t->rows[r].vals[i].s);
        memcpy(t->rows[r].vals, newvals, (size_t)t->col_count * sizeof(nlsql_value_t));
        if (pk_idx_ok && pk_in_set && pkc >= 0)
            nlsql_pk_add(t, r, &t->rows[r].vals[pkc]); /* NULL 时内部跳过 */
        nlsql_tbl_mark_mutated(t); /* 既有行被改：增量帧不可用，下次压实整表重写 */
        affected++;
    }
    nlsql_result_t* res = res_new(0);
    if (!res) return NLSQL_ERR;
    res->affected = affected;
    *out = res;
    return NLSQL_OK;
#undef UPD_ROWCLEAN
}

static int exec_delete(nlsql_conn_t* c, engine_t* e, delete_stmt_t* del,
                       nlsql_value_t* params, int nparams, nlsql_result_t** out) {
    table_t* t = nlsql_tbl_find(e, del->table);
    if (!t) {
        nlsql_set_err(c, NLSQL_ER_NO_SUCH_TABLE, "42S02",
                      "Table '%s.%s' doesn't exist", e->cur_db->name, del->table);
        return NLSQL_ER_NO_SUCH_TABLE;
    }
    /* 倒序删除避免索引漂移 */
    uint64_t affected = 0;
    for (int64_t r = (int64_t)t->row_count - 1; r >= 0; r--) {
        char em[128];
        int m = eval_cond(del->where, t, &t->rows[(uint64_t)r], params, nparams, em, sizeof(em));
        if (m < 0) { nlsql_set_err(c, NLSQL_ER_UNKNOWN_COLUMN, "42S22", "%s", em); return NLSQL_ER_UNKNOWN_COLUMN; }
        if (!m) continue;
        if (journal_row(e, JR_DELETE, t, (uint64_t)r, NULL) != NLSQL_OK) {
            nlsql_set_err(c, NLSQL_ERR_IO, "HY000", "journal write failed");
            return NLSQL_ERR_IO;
        }
        for (int i = 0; i < t->col_count; i++) free(t->rows[(uint64_t)r].vals[i].s);
        nlsql_tbl_val_recycle(t, t->rows[(uint64_t)r].vals); /* vals 复用，删除/插入循环稳态零增长 */
        memmove(&t->rows[r], &t->rows[(uint64_t)r + 1],
                (size_t)(t->row_count - (uint64_t)r - 1) * sizeof(row_t));
        t->row_count--;
        nlsql_pk_invalidate(t); /* 行号移位，索引下次需要时重建 */
        nlsql_tbl_mark_mutated(t); /* 删行导致后续行前移：增量帧不可用 */
        affected++;
    }
    nlsql_result_t* res = res_new(0);
    if (!res) return NLSQL_ERR;
    res->affected = affected;
    *out = res;
    return NLSQL_OK;
}
