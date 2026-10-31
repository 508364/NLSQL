/**
 * @file nlsql_util.c
 * @brief NLSQL 基础层：缓冲、编码、错误、SHA1、安全工具
 */
#include "nlsql_internal.h"
#include "netleaf_nlsql_platform.h"
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

/* =========================================
 * sbuf 动态缓冲
 * ========================================= */

int sbuf_init(sbuf_t* b, uint32_t cap) {
    if (cap < 32) cap = 32;
    b->p = (char*)malloc(cap);
    if (!b->p) return -1;
    b->len = 0; b->cap = cap;
    b->p[0] = '\0';
    return 0;
}

static int sbuf_grow(sbuf_t* b, uint32_t need) {
    if (b->len + need + 1 <= b->cap) return 0;
    uint32_t nc = b->cap * 2;
    while (nc < b->len + need + 1) nc *= 2;
    char* np = (char*)realloc(b->p, nc);
    if (!np) return -1;
    b->p = np; b->cap = nc;
    return 0;
}

int sbuf_append(sbuf_t* b, const void* data, uint32_t n) {
    if (sbuf_grow(b, n) != 0) return -1;
    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

int sbuf_append_str(sbuf_t* b, const char* s) {
    return sbuf_append(b, s, (uint32_t)strlen(s));
}

int sbuf_append_u8(sbuf_t* b, uint8_t v) {
    return sbuf_append(b, &v, 1);
}

int sbuf_append_u32(sbuf_t* b, uint32_t v) {
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return sbuf_append(b, t, 4);
}

int sbuf_append_u64(sbuf_t* b, uint64_t v) {
    uint8_t t[8];
    for (int i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8 * i));
    return sbuf_append(b, t, 8);
}

int sbuf_printf(sbuf_t* b, const char* fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((uint32_t)n < sizeof(tmp)) return sbuf_append(b, tmp, (uint32_t)n);
    char* big = (char*)malloc((uint32_t)n + 1);
    if (!big) return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int rc = sbuf_append(b, big, (uint32_t)n);
    free(big);
    return rc;
}

void sbuf_free(sbuf_t* b) {
    free(b->p);
    b->p = NULL; b->len = b->cap = 0;
}

/* =========================================
 * lenenc 变长整数（MySQL 编码兼容）
 * ========================================= */

uint32_t nl_lenenc(uint64_t v, uint8_t out[9]) {
    if (v < 251) { out[0] = (uint8_t)v; return 1; }
    if (v < 65536) { out[0] = 0xFC; out[1] = (uint8_t)v; out[2] = (uint8_t)(v >> 8); return 3; }
    if (v < (1ull << 32)) {
        out[0] = 0xFD;
        for (int i = 0; i < 4; i++) out[1 + i] = (uint8_t)(v >> (8 * i));
        return 5;
    }
    out[0] = 0xFE;
    for (int i = 0; i < 8; i++) out[1 + i] = (uint8_t)(v >> (8 * i));
    return 9;
}

uint32_t nl_read_lenenc(const uint8_t* p, uint32_t avail, uint64_t* out) {
    if (avail < 1) return 0;
    uint8_t f = p[0];
    if (f < 0xFB) { *out = f; return 1; }
    if (f == 0xFC) { if (avail < 3) return 0; *out = p[1] | ((uint64_t)p[2] << 8); return 3; }
    if (f == 0xFD) {
        if (avail < 5) return 0;
        *out = 0;
        for (int i = 0; i < 4; i++) *out |= ((uint64_t)p[1 + i]) << (8 * i);
        return 5;
    }
    if (f == 0xFE) {
        if (avail < 9) return 0;
        *out = 0;
        for (int i = 0; i < 8; i++) *out |= ((uint64_t)p[1 + i]) << (8 * i);
        return 9;
    }
    return 0; /* 0xFB=NULL 由调用方单独处理 */
}

/* =========================================
 * 公平三模式读写锁（READ 共享 / EXCL 独占 / SNAP 挡写放读）
 * ========================================= */

void frw_init(frw_lock_t* l) {
    pthread_mutex_init(&l->mtx, NULL);
    pthread_cond_init(&l->cv, NULL);
    l->readers = 0;
    l->excl_holder = 0;
    l->snap_holder = 0;
    l->next_ticket = 0;
    l->writer_bypass = 0;
    l->qhead = l->qtail = NULL;
}

void frw_destroy(frw_lock_t* l) {
    pthread_mutex_destroy(&l->mtx);
    pthread_cond_destroy(&l->cv);
}

/* 自己前面是否有等待中的写者/压实 */
static int frw_writer_ahead(const frw_lock_t* l, const frw_waiter_t* self) {
    for (const frw_waiter_t* w = l->qhead; w && w != self; w = w->next)
        if (w->mode != FRW_READ) return 1;
    return 0;
}

/* 读者能否进入：无独占写者；前面若有等待中的写者/压实，可限流越过（最多 FRW_BYPASS_MAX 个），
   既让长压实时读不被整体堵死，又保证写者等待有界、不被新读者无限插队 */
static int frw_read_can_enter(const frw_lock_t* l, const frw_waiter_t* self, int* bypassed) {
    *bypassed = 0;
    if (l->excl_holder) return 0;
    if (!frw_writer_ahead(l, self)) return 1;
    /* 压实进行中：写者本就被 SNAP 挡住，此时放行读者不会延长写者等待 → 不限流 */
    if (l->snap_holder) return 1;
    if (l->writer_bypass >= FRW_BYPASS_MAX) return 0;
    *bypassed = 1;
    return 1;
}

/* 写者/压实能否进入：必须队首，且无冲突持有者。
   SNAP 与 READ 并发安全（压实只读内存 + 写快照/journal 文件，读者不碰文件），故不等读者 */
static int frw_write_can_enter(const frw_lock_t* l, const frw_waiter_t* self) {
    if (l->qhead != self) return 0;
    if (l->excl_holder || l->snap_holder) return 0;
    if (self->mode == FRW_EXCL && l->readers > 0) return 0;
    return 1;
}

static void frw_unlink(frw_lock_t* l, frw_waiter_t* w) {
    if (w->prev) w->prev->next = w->next; else l->qhead = w->next;
    if (w->next) w->next->prev = w->prev; else l->qtail = w->prev;
}

void frw_acquire(frw_lock_t* l, frw_mode_t m) {
    pthread_mutex_lock(&l->mtx);
    frw_waiter_t w;
    w.next = w.prev = NULL;
    w.ticket = l->next_ticket++;
    w.mode = m;
    /* 尾插：严格票序 */
    w.prev = l->qtail;
    if (l->qtail) l->qtail->next = &w; else l->qhead = &w;
    l->qtail = &w;
    for (;;) {
        int bypassed = 0;
        int ok = (m == FRW_READ) ? frw_read_can_enter(l, &w, &bypassed)
                                 : frw_write_can_enter(l, &w);
        if (ok) {
            if (bypassed) l->writer_bypass++;   /* 记账：该读者越过了等待中的写者 */
            break;
        }
        pthread_cond_wait(&l->cv, &l->mtx);
    }
    frw_unlink(l, &w);
    if (m == FRW_READ) {
        l->readers++;
    } else if (m == FRW_EXCL) {
        l->excl_holder = 1;
        l->writer_bypass = 0;   /* 写者终于拿到 → 越过额度清零 */
    } else {
        l->snap_holder = 1;
        l->writer_bypass = 0;
    }
    pthread_mutex_unlock(&l->mtx);
}

void frw_release(frw_lock_t* l, frw_mode_t m) {
    pthread_mutex_lock(&l->mtx);
    if (m == FRW_READ) l->readers--;
    else if (m == FRW_EXCL) l->excl_holder = 0;
    else { l->snap_holder = 0; l->writer_bypass = 0; }
    /* 广播让队列按票序推进：队首要写则放它，要读则放行其后所有可进入的读者 */
    pthread_cond_broadcast(&l->cv);
    pthread_mutex_unlock(&l->mtx);
}

/* =========================================
 * 错误处理（MySQL 风格）
 * ========================================= */

void nlsql_set_err(nlsql_conn_t* c, int code, const char* sqlstate, const char* fmt, ...) {
    if (!c) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* err_mtx：并发读者都会写错误槽，必须互斥否则文本撕裂 */
    pthread_mutex_lock(&c->err_mtx);
    memcpy(c->err_msg, buf, sizeof(c->err_msg));
    c->err_code = code;
    snprintf(c->err_sqlstate, sizeof(c->err_sqlstate), "%s", sqlstate ? sqlstate : "HY000");
    pthread_mutex_unlock(&c->err_mtx);
}

/* 锁外安全拷贝错误文本（wire 层解锁后再发 ERR 包时用） */
void nlsql_copy_err(nlsql_conn_t* c, int* code, char* msg, size_t msgsz,
                    char* state, size_t statesz) {
    if (!c) return;
    pthread_mutex_lock(&c->err_mtx);
    if (code) *code = c->err_code;
    if (msg && msgsz) snprintf(msg, msgsz, "%s", c->err_msg);
    if (state && statesz) snprintf(state, statesz, "%s", c->err_sqlstate);
    pthread_mutex_unlock(&c->err_mtx);
}

static char g_global_err[256] = "unknown error";

void nlsql_set_global_err(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_global_err, sizeof(g_global_err), fmt, ap);
    va_end(ap);
}

const char* nlsql_global_error(void) { return g_global_err; }

const char* nlsql_error(nlsql_conn_t* conn) {
    return conn ? conn->err_msg : nlsql_global_error();
}

int nlsql_errno(nlsql_conn_t* conn) { return conn ? conn->err_code : NLSQL_ERR; }

const char* nlsql_sqlstate(nlsql_conn_t* conn) {
    return conn ? conn->err_sqlstate : "HY000";
}

/* =========================================
 * 标识符 / 路径安全
 * ========================================= */

int nlsql_valid_ident(const char* s) {
    if (!s || !s[0]) return 0;
    size_t n = strlen(s);
    if (n > NLSQL_MAX_IDENT) return 0;
    if (s[0] >= '0' && s[0] <= '9') return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                 (ch >= '0' && ch <= '9') || ch == '_' || ch == '$';
        if (!ok) return 0;
    }
    return 1;
}

/**
 * 数据目录净化：拒绝路径穿越成分（"..")，禁止以点开头段；
 * 允许字母数字/下划线/连字符/点 与 '/' 分隔，规范化后写入 out。
 */
static int sanitize_dir(const char* in, char* out, uint32_t outsz) {
    if (!in || !in[0]) return -1;
    size_t n = strlen(in);
    if (n >= outsz) return -1;
    /* 拒绝 NUL 之外的控制字符与反斜杠（统一 '/'） */
    uint32_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch == '\\') { out[o++] = '/'; continue; }
        if (ch < 0x20 || ch == 0x7F) return -1;
        out[o++] = (char)ch;
    }
    out[o] = '\0';
    /* Windows 盘符前缀（如 "Z:/data"）：允许 ':' 只出现在 out[1]，段检查从盘符后开始 */
    size_t first = 0;
#ifdef _WIN32
    int drive = (o >= 2) &&
                ((out[0] >= 'A' && out[0] <= 'Z') || (out[0] >= 'a' && out[0] <= 'z')) &&
                out[1] == ':';
    if (drive) first = (o > 2 && out[2] == '/') ? 3 : 2;
#endif
    /* 逐段检查：不允许 ".."、空段末尾、以 '.' 开头段 */
    const char* seg = out + first;
    while (*seg) {
        const char* nx = strchr(seg, '/');
        size_t sl = nx ? (size_t)(nx - seg) : strlen(seg);
        if (sl == 0 && nx) { /* 双斜杠：容忍 */ }
        else {
            if (sl == 2 && seg[0] == '.' && seg[1] == '.') return -1;
            if (seg[0] == '.' && sl > 0) return -1;
            for (size_t i = 0; i < sl; i++) {
                char ch = seg[i];
                int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                         (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
                if (!ok) return -1;
            }
        }
        if (!nx) break;
        seg = nx + 1;
    }
    return 0;
}

/* =========================================
 * SHA1（纯 C 实现，mysql_native_password 用）
 * ========================================= */

static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(nlsha1_t* s, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

void nlsha1_init(nlsha1_t* s) {
    s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE;
    s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
    s->len = 0; s->blen = 0;
}

void nlsha1_update(nlsha1_t* s, const void* data, uint32_t n) {
    const uint8_t* p = (const uint8_t*)data;
    s->len += n;
    while (n > 0) {
        uint32_t take = 64 - s->blen;
        if (take > n) take = n;
        memcpy(s->buf + s->blen, p, take);
        s->blen += take; p += take; n -= take;
        if (s->blen == 64) { sha1_block(s, s->buf); s->blen = 0; }
    }
}

void nlsha1_final(nlsha1_t* s, uint8_t out[20]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    nlsha1_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->blen != 56) nlsha1_update(s, &z, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    /* 手工写入避免再次计入 len */
    memcpy(s->buf + 56, lenb, 8);
    sha1_block(s, s->buf);
    s->blen = 0;
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(s->h[i]);
    }
}

void nlsha1(const void* data, uint32_t n, uint8_t out[20]) {
    nlsha1_t s;
    nlsha1_init(&s);
    nlsha1_update(&s, data, n);
    nlsha1_final(&s, out);
}

/* =========================================
 * 字符串转义
 * ========================================= */

uint32_t nlsql_escape_string(char* dst, const char* src, uint32_t len) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < len; i++) {
        char ch = src[i];
        if (ch == '\'' || ch == '\\' || ch == '"' || ch == '`') dst[o++] = '\\';
        if (ch == '\0') { dst[o++] = '\\'; dst[o++] = '0'; }
        else dst[o++] = ch;
    }
    dst[o] = '\0';
    return o;
}

/* =========================================
 * 文件锁（数据目录防双开）
 * POSIX：flock(LOCK_EX|LOCK_NB)；Windows：CRT 共享模式 _SH_DENYRW。
 * 两种实现都在 fd 关闭时自动释放。
 * ========================================= */

int nlsql_lock_dir(const char* root, char* lock_path, uint32_t lpsz) {
    snprintf(lock_path, lpsz, "%s/.nlsql.lock", root);
    int fd = -1;
    int rc = nlsql_lock_exclusive(lock_path, &fd);
    if (rc == 1) return -2; /* 已被持有 */
    if (rc != 0) return -1;
    char pidbuf[32];
    int n = snprintf(pidbuf, sizeof(pidbuf), "%ld\n", (long)getpid());
    if (ftruncate(fd, 0) == 0) (void)!write(fd, pidbuf, (size_t)n);
    return fd;
}

void nlsql_unlock_dir(int fd, const char* lock_path) {
    nlsql_unlock_exclusive(fd);
    if (lock_path) unlink(lock_path);
}

/* 目录创建（递归）；Windows 跳过盘符段（不能 mkdir("Z:")） */
int nlsql_mkdir_p(const char* path) {
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t n = strlen(tmp);
    size_t start = 1;
#ifdef _WIN32
    if (n >= 2 && tmp[1] == ':' &&
        ((tmp[0] >= 'A' && tmp[0] <= 'Z') || (tmp[0] >= 'a' && tmp[0] <= 'z')))
        start = (n > 2 && tmp[2] == '/') ? 3 : 2;
#endif
    for (size_t i = start; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int nlsql_sanitize_dir(const char* in, char* out, uint32_t outsz) {
    return sanitize_dir(in, out, outsz);
}
