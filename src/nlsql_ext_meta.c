/**
 * @file nlsql_ext_meta.c
 * @brief 扩展元数据 sidecar 持久化
 *
 * 文件：<data_dir>/<db>/nlsql_ext_meta.dat
 * 格式：每行一条记录，字段以 TAB 分隔，字段内的 TAB/换行/反斜杠做转义：
 *   kind \t name \t target \t def \n
 *
 * 写路径：读全文件 → 内存中增/删 → 写临时文件 → fsync → rename（原子替换）。
 * 记录量级极小（数十条），全量重写足够简单且崩溃安全。
 */
#include "nlsql_internal.h"
#include "netleaf_nlsql_ext.h"
#include "netleaf_nlsql_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define META_FILE "nlsql_ext_meta.dat"
#define META_LINE_MAX (64u * 1024u)   /* 单条记录上限（含定义 SQL） */

typedef struct meta_rec {
    char* kind;
    char* name;
    char* target;
    char* def;
} meta_rec_t;

typedef struct meta_set {
    meta_rec_t* v;
    int         n, cap;
} meta_set_t;

static void rec_free(meta_rec_t* r) {
    free(r->kind); free(r->name); free(r->target); free(r->def);
    memset(r, 0, sizeof(*r));
}

static void set_free(meta_set_t* s) {
    for (int i = 0; i < s->n; i++) rec_free(&s->v[i]);
    free(s->v);
    s->v = NULL; s->n = s->cap = 0;
}

/* 计算 sidecar 路径；未 USE 库时返回 NLSQL_ERR */
int nlsql_meta_path(nlsql_conn_t* c, char* out, size_t sz) {
    if (!c || !c->eng.cur_db || !c->eng.cur_db->name[0]) return NLSQL_ERR;
    if (!nlsql_valid_ident(c->eng.cur_db->name)) return NLSQL_ERR;
    int n = snprintf(out, sz, "%s/%s/%s", c->eng.root, c->eng.cur_db->name, META_FILE);
    if (n <= 0 || (size_t)n >= sz) return NLSQL_ERR;
    return NLSQL_OK;
}

/* ---------------- 转义 ---------------- */

static char* esc_dup(const char* s) {
    if (!s) s = "";
    size_t n = strlen(s);
    char* o = (char*)malloc(2 * n + 1);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == '\\' || ch == '\t' || ch == '\n' || ch == '\r') {
            o[k++] = '\\';
            o[k++] = ch == '\t' ? 't' : ch == '\n' ? 'n' : ch == '\r' ? 'r' : '\\';
        } else {
            o[k++] = ch;
        }
    }
    o[k] = '\0';
    return o;
}

/* 解码一段（in 以 '\\0' 结束；返回 malloc 串） */
static char* unesc_dup(const char* s, size_t n) {
    char* o = (char*)malloc(n + 1);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
            o[k++] = s[i] == 't' ? '\t' : s[i] == 'n' ? '\n' : s[i] == 'r' ? '\r' : s[i];
        } else {
            o[k++] = s[i];
        }
    }
    o[k] = '\0';
    return o;
}

/* ---------------- 读 ---------------- */

/* 解析一行（就地拆分，返回字段数，最多 4） */
static int split_line(char* line, char* f[4]) {
    int n = 0;
    char* p = line;
    f[n++] = p;
    while (*p) {
        if (*p == '\t' && n < 4) { *p = '\0'; f[n++] = p + 1; }
        p++;
    }
    return n;
}

static int set_load(meta_set_t* s, const char* path) {
    memset(s, 0, sizeof(*s));
    FILE* f = fopen(path, "rb");
    if (!f) return (errno == ENOENT) ? 0 : -1;
    char* line = (char*)malloc(META_LINE_MAX);
    if (!line) { fclose(f); return -1; }
    int rc = 0;
    while (fgets(line, (int)META_LINE_MAX, f)) {
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln - 1] == '\n' || line[ln - 1] == '\r')) line[--ln] = '\0';
        if (ln == 0) continue;
        char* fld[4] = { NULL, NULL, NULL, NULL };
        int nf = split_line(line, fld);
        meta_rec_t r;
        memset(&r, 0, sizeof(r));
        r.kind   = unesc_dup(fld[0], strlen(fld[0]));
        r.name   = nf > 1 ? unesc_dup(fld[1], strlen(fld[1])) : strdup("");
        r.target = nf > 2 ? unesc_dup(fld[2], strlen(fld[2])) : strdup("");
        r.def    = nf > 3 ? unesc_dup(fld[3], strlen(fld[3])) : strdup("");
        if (!r.kind || !r.name || !r.target || !r.def) { rec_free(&r); rc = -1; break; }
        if (s->n >= s->cap) {
            int nc = s->cap ? s->cap * 2 : 16;
            meta_rec_t* nv = (meta_rec_t*)realloc(s->v, (size_t)nc * sizeof(*nv));
            if (!nv) { rec_free(&r); rc = -1; break; }
            s->v = nv; s->cap = nc;
        }
        s->v[s->n++] = r;
    }
    free(line);
    fclose(f);
    return rc;
}

/* ---------------- 写 ---------------- */

static int write_all(int fd, const char* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static int set_store(const meta_set_t* s, const char* path) {
    char tmp[1200];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return -1;
    int fd = nlsql_file_open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) return -1;
    int rc = 0;
    for (int i = 0; i < s->n && rc == 0; i++) {
        char* e[4];
        e[0] = esc_dup(s->v[i].kind);
        e[1] = esc_dup(s->v[i].name);
        e[2] = esc_dup(s->v[i].target);
        e[3] = esc_dup(s->v[i].def);
        if (!e[0] || !e[1] || !e[2] || !e[3]) {
            rc = -1;
        } else {
            if (write_all(fd, e[0], strlen(e[0])) != 0 ||
                write_all(fd, "\t", 1) != 0 ||
                write_all(fd, e[1], strlen(e[1])) != 0 ||
                write_all(fd, "\t", 1) != 0 ||
                write_all(fd, e[2], strlen(e[2])) != 0 ||
                write_all(fd, "\t", 1) != 0 ||
                write_all(fd, e[3], strlen(e[3])) != 0 ||
                write_all(fd, "\n", 1) != 0) rc = -1;
        }
        for (int k = 0; k < 4; k++) free(e[k]);
    }
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    if (rc == 0 && nlsql_rename_replace(tmp, path) != 0) rc = -1;
    if (rc != 0) unlink(tmp);
    return rc;
}

/* ---------------- 对外接口 ---------------- */

int nlsql_ext_meta_put(nlsql_conn_t* conn, const char* kind, const char* name,
                       const char* target, const char* def) {
    if (!conn || !kind || !kind[0] || !name || !name[0]) return NLSQL_ERR;
    char path[1100];
    if (nlsql_meta_path(conn, path, sizeof(path)) != NLSQL_OK) return NLSQL_ERR;

    meta_set_t s;
    if (set_load(&s, path) != 0) { set_free(&s); return NLSQL_ERR; }

    int idx = -1;
    for (int i = 0; i < s.n; i++)
        if (strcmp(s.v[i].kind, kind) == 0 && strcmp(s.v[i].name, name) == 0) { idx = i; break; }

    meta_rec_t r;
    memset(&r, 0, sizeof(r));
    r.kind = strdup(kind);
    r.name = strdup(name);
    r.target = strdup(target ? target : "");
    r.def = strdup(def ? def : "");
    if (!r.kind || !r.name || !r.target || !r.def) { rec_free(&r); set_free(&s); return NLSQL_ERR; }

    if (idx >= 0) {
        rec_free(&s.v[idx]);
        s.v[idx] = r;
    } else {
        if (s.n >= s.cap) {
            int nc = s.cap ? s.cap * 2 : 16;
            meta_rec_t* nv = (meta_rec_t*)realloc(s.v, (size_t)nc * sizeof(*nv));
            if (!nv) { rec_free(&r); set_free(&s); return NLSQL_ERR; }
            s.v = nv; s.cap = nc;
        }
        s.v[s.n++] = r;
    }
    int rc = set_store(&s, path);
    set_free(&s);
    return rc == 0 ? NLSQL_OK : NLSQL_ERR;
}

int nlsql_ext_meta_del(nlsql_conn_t* conn, const char* kind, const char* name) {
    if (!conn || !kind || !name) return NLSQL_ERR;
    char path[1100];
    if (nlsql_meta_path(conn, path, sizeof(path)) != NLSQL_OK) return NLSQL_ERR;

    meta_set_t s;
    if (set_load(&s, path) != 0) { set_free(&s); return NLSQL_ERR; }

    int found = 0;
    for (int i = 0; i < s.n; i++) {
        if (strcmp(s.v[i].kind, kind) == 0 && strcmp(s.v[i].name, name) == 0) {
            rec_free(&s.v[i]);
            memmove(&s.v[i], &s.v[i + 1], (size_t)(s.n - i - 1) * sizeof(meta_rec_t));
            s.n--; found = 1;
            break;
        }
    }
    int rc = 0;
    if (found) rc = set_store(&s, path);
    set_free(&s);
    return rc == 0 ? NLSQL_OK : NLSQL_ERR;
}

int nlsql_ext_meta_each(nlsql_conn_t* conn, nlsql_ext_meta_cb cb, void* ud) {
    if (!conn || !cb) return NLSQL_ERR;
    char path[1100];
    if (nlsql_meta_path(conn, path, sizeof(path)) != NLSQL_OK) return NLSQL_ERR;

    meta_set_t s;
    if (set_load(&s, path) != 0) { set_free(&s); return NLSQL_ERR; }
    int rc = 0;
    for (int i = 0; i < s.n; i++) {
        rc = cb(s.v[i].kind, s.v[i].name, s.v[i].target, s.v[i].def, ud);
        if (rc != 0) break;
    }
    set_free(&s);
    return rc;
}
