/**
 * @file nlsql_wire.c
 * @brief NLSQL MySQL 传输协议适配层（可选，默认关闭）
 *
 * 仅绑定 127.0.0.1，供官方 mysql 客户端 / libmysql 驱动直连。
 * 认证：mysql_native_password 质询（SHA1），凭据来自 conn->creds_*。
 * 命令：COM_QUERY / COM_PING / COM_INIT_DB / COM_QUIT。
 * TLS：可选 SSL，经 NetLeaf TLS2 扩展（mbedTLS 2.28）承载，强制 TLS 1.2；
 *      运行时 dlopen，核心库保持零第三方依赖。
 */
#include "nlsql_internal.h"
#include "netleaf_nlsql_platform.h"
#include <stdatomic.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#endif
#include <time.h>

/* 协议常量 */
#define CLIENT_LONG_PASSWORD      0x00000001u
#define CLIENT_FOUND_ROWS         0x00000002u
#define CLIENT_CONNECT_WITH_DB    0x00000008u
#define CLIENT_PROTOCOL_41        0x00000200u
#define CLIENT_SSL                0x00000800u
#define CLIENT_TRANSACTIONS       0x00002000u
#define CLIENT_SECURE_CONNECTION  0x00008000u
#define CLIENT_PLUGIN_AUTH        0x00080000u

#define NL_CAP_BASE (CLIENT_LONG_PASSWORD | CLIENT_FOUND_ROWS | CLIENT_CONNECT_WITH_DB | \
                     CLIENT_PROTOCOL_41 | CLIENT_TRANSACTIONS | CLIENT_SECURE_CONNECTION | \
                     CLIENT_PLUGIN_AUTH)

#define COM_QUIT     0x01
#define COM_INIT_DB  0x02
#define COM_QUERY    0x03
#define COM_PING     0x0E
#define COM_STMT_PREPARE 0x16
#define COM_STMT_EXECUTE 0x17
#define COM_STMT_CLOSE   0x19
#define COM_STMT_RESET   0x1A

#define WS_MAX_STMT 32

#define MAX_TRACKED_CLIENTS 64

/* =========================================
 * TLS：NetLeaf TLS2 运行时桥接
 *
 * 核心库不链接 netleaf（保持零第三方依赖）：按需 dlopen libnetleaf_tls2，
 * 通过 dlsym 取符号。配置结构按 netleaf_tls2.h 的 nl_tls2_config_t 布局镜像，
 * 版本固定 TLS1.2（min=max）。
 * ========================================= */

#define NL_TLS2_PROTO_TLS1_2 2

typedef struct nl_tls2_cfg_mirror {
    const char* ca_file;
    const char* ca_path;
    const char* cert_file;
    const char* key_file;
    const char* key_pass;
    const char* crl_file;
    int         verify_peer;
    int         client_auth;
    int         is_server;
    int         use_dtls;
    int         handshake_timeout_ms;
    int         min_proto;
    int         max_proto;
    const unsigned char* psk;
    size_t      psk_len;
    const char* psk_identity;
    const int*  ciphersuites;
    const char* const* alpn;
    int         enable_session_tickets;
    int         session_cache_size;
} nl_tls2_cfg_t;

typedef struct tls_api {
    void* lib;
    void* (*create)(void);
    void  (*destroy)(void*);
    int   (*configure)(void*, const nl_tls2_cfg_t*);
    int   (*handshake)(void*, int);
    int   (*send)(void*, const void*, size_t);
    int   (*recv)(void*, void*, size_t);
    int   (*close)(void*);
    const char* (*get_proto)(const void*);
    const char* (*strerror_)(int);
    int         proto12; /* 该扩展的 "TLS1.2" 枚举值（tls2=2，tls3=0） */
} tls_api_t;

/* 每连接 I/O 上下文：明文走 fd，TLS 走 netleaf 上下文 */
typedef struct nlio {
    int        fd;
    tls_api_t* tls;   /* NULL = 明文 */
    void*      ctx;
} nlio_t;

static pthread_mutex_t g_tls_load_mtx = PTHREAD_MUTEX_INITIALIZER;
static tls_api_t       g_tls_api;
static int             g_tls_state; /* 0=未尝试 1=可用 -1=不可用 */

/* 从已打开的库按符号前缀（nl_tls2_ / nl_tls3_）绑定 API */
static int tls_bind(void* lib, const char* pfx, int proto12, tls_api_t* a) {
    char n[64];
    memset(a, 0, sizeof(*a));
    a->lib = lib;
#define TLS_SYM(field, suffix)                                   \
    do {                                                         \
        snprintf(n, sizeof(n), "%s%s", pfx, suffix);              \
        *(void**)(&a->field) = nlsql_dl_sym(lib, n);               \
    } while (0)
    TLS_SYM(create, "_create");
    TLS_SYM(destroy, "_destroy");
    TLS_SYM(configure, "_configure");
    TLS_SYM(handshake, "_handshake");
    TLS_SYM(send, "_send");
    TLS_SYM(recv, "_recv");
    TLS_SYM(close, "_close");
    TLS_SYM(get_proto, "_get_proto_version");
    TLS_SYM(strerror_, "_strerror");
#undef TLS_SYM
    a->proto12 = proto12;
    return (a->create && a->destroy && a->configure && a->handshake &&
            a->send && a->recv && a->close) ? 0 : -1;
}

/* 加载 NetLeaf TLS 扩展（优先 tls2，其次 tls3）；失败后不重复尝试，
   避免每次连接都刷 dlopen。 */
static tls_api_t* tls_load(void) {
    pthread_mutex_lock(&g_tls_load_mtx);
    if (g_tls_state == 0) {
        g_tls_state = -1;
        static const struct { const char* pfx; int proto12; } kinds[] = {
            { "nl_tls2", NL_TLS2_PROTO_TLS1_2 }, /* tls2 枚举：TLS1.2 = 2 */
            { "nl_tls3", 0 },                    /* tls3 枚举：TLS1.2 = 0 */
        };
        /* 环境变量指定路径优先（部署时可放任意位置），否则按标准库名查找 */
        const char* env = getenv("NLSQL_TLS_LIB");
        if (env && env[0]) {
            void* lib = nlsql_dl_open(env, RTLD_NOW | RTLD_LOCAL);
            if (lib) {
                for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
                    if (tls_bind(lib, kinds[i].pfx, kinds[i].proto12, &g_tls_api) == 0) {
                        g_tls_state = 1;
                        break;
                    }
                }
                if (g_tls_state != 1) { nlsql_dl_close(lib); memset(&g_tls_api, 0, sizeof(g_tls_api)); }
            }
        }
        static const char* const names[] = {
            "libnetleaf_tls2.so", "libnetleaf_tls2.so.1", "netleaf_tls2.dll",
            "libnetleaf_tls3.so", "libnetleaf_tls3.so.2", "netleaf_tls3.dll",
        };
        for (size_t i = 0; g_tls_state != 1 && i < sizeof(names) / sizeof(names[0]); i++) {
            void* lib = nlsql_dl_open(names[i], RTLD_NOW | RTLD_LOCAL);
            if (!lib) continue;
            const char* pfx = strstr(names[i], "tls3") ? "nl_tls3" : "nl_tls2";
            int pr = strstr(names[i], "tls3") ? 0 : NL_TLS2_PROTO_TLS1_2;
            if (tls_bind(lib, pfx, pr, &g_tls_api) == 0) g_tls_state = 1;
            else { nlsql_dl_close(lib); memset(&g_tls_api, 0, sizeof(g_tls_api)); }
        }
    }
    pthread_mutex_unlock(&g_tls_load_mtx);
    return g_tls_state == 1 ? &g_tls_api : NULL;
}

struct wire_srv {
    nlsql_conn_t* conn;
    int     listen_fd;
    atomic_int running;      /* accept 线程读 / wire_stop 写，原子消除 data race */
    pthread_t accept_th;
    pthread_mutex_t cmtx;
    pthread_cond_t  cnd;      /* 客户端退场广播：stop 等 nclients 归零（严禁超时后 free） */
    int       client_fds[MAX_TRACKED_CLIENTS]; /* 槽位先于线程创建登记；CLI 退场先摘槽再 close，防 fd 复用误伤 */
    int       nclients;
    int       max_clients;
    char      cert_file[1024]; /* 空串 = 不启用 TLS */
    char      key_file[1024];
    tls_api_t* tls_api;        /* 非 NULL 且 cert/key 非空时在能力位广播 CLIENT_SSL */
};

typedef struct {
    wire_srv_t* srv;
    int fd;
} cli_arg_t;

/* =========================================
 * 分组：包 I/O
 * ========================================= */

static int rd_n(nlio_t* io, void* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r;
        if (io->tls) {
            int t = io->tls->recv(io->ctx, (char*)buf + got, n - got);
            if (t <= 0) return -1;
            r = (ssize_t)t;
        } else {
            r = nlsql_sock_recv(io->fd, (char*)buf + got, n - got);
            if (r < 0) {
                if (nlsql_sock_interrupted()) continue;
                return -1;
            }
            if (r == 0) return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static int wr_all(nlio_t* io, const void* buf, size_t n) {
    size_t put = 0;
    while (put < n) {
        ssize_t w;
        if (io->tls) {
            int t = io->tls->send(io->ctx, (const char*)buf + put, n - put);
            if (t <= 0) return -1;
            w = (ssize_t)t;
        } else {
            /* 平台层封装：POSIX 带 MSG_NOSIGNAL，Windows 走 winsock send */
            w = nlsql_sock_send(io->fd, (const char*)buf + put, n - put);
            if (w < 0) {
                if (nlsql_sock_interrupted()) continue;
                return -1;
            }
            if (w == 0) return -1;
        }
        put += (size_t)w;
    }
    return 0;
}

/* 写多个 iovec：明文用 sendmsg 一次发出（Windows 退回逐段 send）；TLS 逐段加密发送 */
static int wr_iov(nlio_t* io, struct iovec* iv, int n) {
#ifdef _WIN32
    for (int i = 0; i < n; i++)
        if (wr_all(io, iv[i].iov_base, iv[i].iov_len) != 0) return -1;
    return 0;
#else
    if (io->tls) {
        for (int i = 0; i < n; i++)
            if (wr_all(io, iv[i].iov_base, iv[i].iov_len) != 0) return -1;
        return 0;
    }
    while (n > 0) {
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = iv;
        mh.msg_iovlen = (size_t)n;
        ssize_t w = sendmsg(io->fd, &mh, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        size_t done = (size_t)w;
        int i = 0;
        while (i < n && done >= iv[i].iov_len) { done -= iv[i].iov_len; i++; }
        if (i == n) return 0;
        iv[i].iov_base = (char*)iv[i].iov_base + done;
        iv[i].iov_len -= done;
        iv += i; n -= i;
    }
    return 0;
#endif
}

/* 读一个 MySQL 包：返回 payload（malloc），*seq 输出序列号 */
static int rd_packet(nlio_t* io, uint8_t** out, size_t* outlen, uint8_t* seq) {
    uint8_t hdr[4];
    if (rd_n(io, hdr, 4) != 0) return -1;
    size_t len = (size_t)hdr[0] | ((size_t)hdr[1] << 8) | ((size_t)hdr[2] << 16);
    if (seq) *seq = hdr[3];
    if (len > NLSQL_MAX_PACKET) return -1;
    uint8_t* p = (uint8_t*)malloc(len + 1);
    if (!p) return -1;
    if (len && rd_n(io, p, len) != 0) { free(p); return -1; }
    p[len] = '\0';
    *out = p; *outlen = len;
    return 0;
}

/* 写一个包（payload 自动分片为 <=16MB-1）；hdr+payload 合并为一次 sendmsg */
static int wr_packet(nlio_t* io, uint8_t seq, const uint8_t* payload, size_t len) {
    while (len >= 0xFFFFFF) {
        uint8_t hdr[4] = { 0xFF, 0xFF, 0xFF, seq };
        struct iovec iv[2] = { { hdr, 4 }, { (void*)payload, 0xFFFFFF } };
        if (wr_iov(io, iv, 2) != 0) return -1;
        payload += 0xFFFFFF; len -= 0xFFFFFF; seq++;
    }
    uint8_t hdr[4] = { (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), seq };
    struct iovec iv[2] = { { hdr, 4 }, { (void*)payload, len } };
    return wr_iov(io, iv, len ? 2 : 1);
}

/* 常数时间字节比较：认证 token/口令散列对比不随匹配进度泄露时序（防逐字节侧信道） */
static int ct_eq(const void* a, const void* b, size_t n) {
    const volatile uint8_t* x = (const volatile uint8_t*)a;
    const volatile uint8_t* y = (const volatile uint8_t*)b;
    volatile uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}

/* 读完整命令包：大语句按 MySQL 协议分片为多个包（非末片长度恒为 0xFFFFFF），
   拼接后一次交付；否则第二片会被误当新命令读取 → 协议失步 */
static int rd_cmd_packet(nlio_t* io, uint8_t** out, size_t* outlen, uint8_t* seq) {
    uint8_t* p = NULL;
    size_t len = 0;
    if (rd_packet(io, &p, &len, seq) != 0) return -1;
    while (len == 0xFFFFFF) {
        uint8_t* p2 = NULL;
        size_t l2 = 0;
        if (rd_packet(io, &p2, &l2, seq) != 0) { free(p); return -1; }
        if (len + l2 > NLSQL_MAX_PACKET) { free(p); free(p2); return -1; }
        uint8_t* np = (uint8_t*)realloc(p, len + l2 + 1);
        if (!np) { free(p); free(p2); return -1; }
        memcpy(np + len, p2, l2);
        free(p2);
        p = np;
        len += l2;
    }
    p[len] = '\0';
    *out = p;
    *outlen = len;
    return 0;
}

/* =========================================
    分组：报文构造
 * ========================================= */

static void put_u8(sbuf_t* b, uint8_t v) { sbuf_append_u8(b, v); }
static void put_u16(sbuf_t* b, uint16_t v) {
    uint8_t t[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    sbuf_append(b, t, 2);
}
static void put_u32(sbuf_t* b, uint32_t v) { sbuf_append_u32(b, v); }
static void put_lenenc(sbuf_t* b, uint64_t v) {
    uint8_t le[9];
    uint32_t n = nl_lenenc(v, le);
    sbuf_append(b, le, n);
}
static void put_str(sbuf_t* b, const char* s) {
    size_t n = strlen(s);
    put_lenenc(b, n);
    sbuf_append(b, s, (uint32_t)n);
}

/* ---- 报文流：多包连续拼接成一块，一次 send（减 syscall 与 malloc） ---- */
typedef struct pstream {
    uint8_t*  p;
    size_t    cap, len;
    uint8_t   seq;
    nlio_t*   io;
    int       err;
} pstream_t;

static void ps_init(pstream_t* ps, nlio_t* io, uint8_t seq, size_t cap) {
    ps->p = (uint8_t*)malloc(cap);
    ps->cap = ps->p ? cap : 0;
    ps->len = 0; ps->seq = seq; ps->io = io;
    ps->err = ps->p ? 0 : 1;
}

static int ps_flush(pstream_t* ps) {
    if (ps->err) return -1;
    if (ps->len) {
        if (wr_all(ps->io, ps->p, ps->len) != 0) { ps->err = 1; return -1; }
        ps->len = 0;
    }
    return 0;
}

/* 追加一个完整包 [hdr 4B][payload] */
static int ps_add(pstream_t* ps, const uint8_t* payload, size_t len) {
    if (ps->err) return -1;
    if (len >= 0xFFFFFF) { /* 超大包：冲刷后按分片直发 */
        if (ps_flush(ps) != 0) return -1;
        if (wr_packet(ps->io, ps->seq, payload, len) != 0) { ps->err = 1; return -1; }
        ps->seq = (uint8_t)(ps->seq + (uint8_t)(len / 0xFFFFFF + 1));
        return 0;
    }
    if (ps->len + 4 + len > ps->cap) {
        if (ps_flush(ps) != 0) return -1;
        if (4 + len > ps->cap) { /* 单包大于缓冲容量：直发 */
            if (wr_packet(ps->io, ps->seq, payload, len) != 0) { ps->err = 1; return -1; }
            ps->seq++;
            return 0;
        }
    }
    ps->p[ps->len]     = (uint8_t)len;
    ps->p[ps->len + 1] = (uint8_t)(len >> 8);
    ps->p[ps->len + 2] = (uint8_t)(len >> 16);
    ps->p[ps->len + 3] = ps->seq++;
    memcpy(ps->p + ps->len + 4, payload, len);
    ps->len += 4 + len;
    return 0;
}

static void ps_free(pstream_t* ps) { free(ps->p); ps->p = NULL; }

/* ---- OK/EOF/ERR：栈构造，零 malloc ---- */
static void send_ok(wire_srv_t* srv, nlio_t* io, uint8_t* seq, uint64_t affected, int64_t insert_id) {
    uint8_t pkt[32], le[9];
    size_t n = 0;
    uint32_t k;
    pkt[n++] = 0x00;
    k = nl_lenenc(affected, le); memcpy(pkt + n, le, k); n += k;
    k = nl_lenenc((uint64_t)insert_id, le); memcpy(pkt + n, le, k); n += k;
    pkt[n++] = 0x02; pkt[n++] = 0x00; /* SERVER_STATUS_AUTOCOMMIT */
    pkt[n++] = 0x00; pkt[n++] = 0x00; /* warnings */
    wr_packet(io, (*seq)++, pkt, n);
    (void)srv;
}

static void send_err(wire_srv_t* srv, nlio_t* io, uint8_t* seq, int code, const char* state, const char* msg) {
    uint8_t pkt[512];
    uint16_t c = (uint16_t)(code > 0 ? code : 1064);
    pkt[0] = 0xFF;
    pkt[1] = (uint8_t)c; pkt[2] = (uint8_t)(c >> 8);
    pkt[3] = '#';
    memcpy(pkt + 4, (state && state[0]) ? state : "HY000", 5);
    size_t n = 9;
    if (msg) {
        size_t ml = strlen(msg);
        if (ml > sizeof(pkt) - n) ml = sizeof(pkt) - n;
        memcpy(pkt + n, msg, ml);
        n += ml;
    }
    wr_packet(io, (*seq)++, pkt, n);
    (void)srv;
}

/* 值 → 文本 */
static void value_text(nlsql_value_t* v, sbuf_t* b) {
    char tmp[64];
    switch (v->type) {
        case NLSQL_TYPE_INT64:
            snprintf(tmp, sizeof(tmp), "%lld", (long long)v->i);
            sbuf_append_str(b, tmp);
            break;
        case NLSQL_TYPE_DOUBLE:
            snprintf(tmp, sizeof(tmp), "%.10g", v->d);
            sbuf_append_str(b, tmp);
            break;
        default:
            if (v->s) sbuf_append(b, v->s, v->len);
            break;
    }
}

/* 全部结果包收集进 pstream，最后一次 send（1 行结果 = 5 包 1 次 syscall） */
static void send_resultset(wire_srv_t* srv, nlio_t* io, uint8_t* seq, nlsql_result_t* res) {
    pstream_t ps;
    sbuf_t b, val;
    ps_init(&ps, io, *seq, 65536);
    if (sbuf_init(&b, 512) != 0) { ps_free(&ps); return; }
    if (sbuf_init(&val, 64) != 0) { sbuf_free(&b); ps_free(&ps); return; }

    /* 列数包 */
    b.len = 0;
    put_lenenc(&b, (uint64_t)res->col_count);
    ps_add(&ps, (const uint8_t*)b.p, b.len);

    /* 列定义（复用同一 sbuf） */
    for (int i = 0; i < res->col_count; i++) {
        b.len = 0;
        put_str(&b, "def");
        put_str(&b, "");            /* schema */
        put_str(&b, "");            /* table */
        put_str(&b, "");            /* org_table */
        put_str(&b, res->cols[i].name);
        put_str(&b, res->cols[i].name); /* org_name */
        put_lenenc(&b, 0x0C);       /* fixed-length fields length */
        put_u16(&b, 33);            /* utf8_general_ci */
        put_u32(&b, 1024);          /* column length */
        put_u8(&b, (uint8_t)res->cols[i].type);
        put_u16(&b, 0);             /* flags */
        put_u8(&b, 31);             /* decimals */
        put_u16(&b, 0);             /* filler */
        ps_add(&ps, (const uint8_t*)b.p, b.len);
    }
    /* EOF（列定义结束） */
    {
        uint8_t eof[5] = { 0xFE, 0x00, 0x00, 0x02, 0x00 };
        ps_add(&ps, eof, 5);
    }
    /* 行 */
    for (uint64_t r = 0; r < res->row_count; r++) {
        b.len = 0;
        for (int i = 0; i < res->col_count; i++) {
            nlsql_value_t* v = &res->rows[r].vals[i];
            if (v->is_null || v->type == NLSQL_TYPE_NULL) {
                put_u8(&b, 0xFB);
            } else {
                val.len = 0;            /* 复用，不 free/不重分配 */
                value_text(v, &val);
                put_lenenc(&b, val.len);
                sbuf_append(&b, val.p, val.len);
            }
        }
        ps_add(&ps, (const uint8_t*)b.p, b.len);
    }
    /* EOF（行结束） */
    {
        uint8_t eof[5] = { 0xFE, 0x00, 0x00, 0x02, 0x00 };
        ps_add(&ps, eof, 5);
    }
    ps_flush(&ps);
    *seq = ps.seq;
    ps_free(&ps);
    sbuf_free(&b);
    sbuf_free(&val);
    (void)srv;
}

/* =========================================
 * 握手与认证
 * ========================================= */

/* 在已建立 TCP 连接上升级 TLS（客户端已发 SSLRequest 之后调用）。
   失败即断开：不回退明文，避免降级攻击。 */
static int tls_start(wire_srv_t* srv, nlio_t* io) {
    tls_api_t* a = srv->tls_api;
    if (!a || !srv->cert_file[0] || !srv->key_file[0]) return -1;
    void* ctx = a->create();
    if (!ctx) return -1;
    nl_tls2_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cert_file = srv->cert_file;
    cfg.key_file  = srv->key_file;
    cfg.is_server = 1;
    cfg.client_auth = 0;
    cfg.handshake_timeout_ms = 10000;
    cfg.min_proto = a->proto12;
    cfg.max_proto = a->proto12; /* 强制 TLS 1.2（上下限同时锁定，tls2/tls3 枚举值不同） */
    int rc = a->configure(ctx, &cfg);
    if (rc == 0) rc = a->handshake(ctx, io->fd);
    if (rc != 0) {
        a->destroy(ctx);
        return -1;
    }
    /* 协商结果必须是 TLSv1.2；否则断开（防客户端与实现对版本上限的分歧） */
    if (a->get_proto) {
        const char* pv = a->get_proto(ctx);
        if (!pv || strncmp(pv, "TLSv1.2", 7) != 0) {
            if (a->close) a->close(ctx);
            a->destroy(ctx);
            return -1;
        }
    }
    io->tls = a;
    io->ctx = ctx;
    return 0;
}

/* 库名只允许标识符字符，杜绝把客户端库名拼进 USE 造成注入 */
static int db_name_ok(const char* s) {
    size_t n = 0;
    if (!s || !s[0]) return 0;
    for (const char* p = s; *p; p++, n++) {
        char ch = *p;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '$')) return 0;
    }
    return n <= NLSQL_MAX_IDENT;
}

static int do_handshake(wire_srv_t* srv, nlio_t* io, uint8_t* seq) {
    nlsql_conn_t* c = srv->conn;
    uint8_t scram[20]; /* 每次握手局部质询：多连接并发握手不共享（TSAN 竞争 + 认证串扰） */
    /* 配置了证书且 TLS 扩展可用时才通告 CLIENT_SSL */
    uint32_t cap = NL_CAP_BASE;
    if (srv->tls_api && srv->cert_file[0] && srv->key_file[0]) cap |= CLIENT_SSL;
    /* 生成 20 字节质询随机数（平台 CSPRNG，失败则时间+地址混合） */
    int ok = nlsql_random_bytes(scram, 20);
    if (!ok) {
        uint64_t t = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 16);
        for (int i = 0; i < 20; i++) {
            t = t * 6364136223846793005ull + 1442695040888963407ull;
            scram[i] = (uint8_t)(t >> 33);
        }
    }
    /* scramble 不能含 0x00（协议要求） */
    for (int i = 0; i < 20; i++) if (scram[i] == 0) scram[i] = 0x41 + (uint8_t)i;

    sbuf_t b;
    if (sbuf_init(&b, 128) != 0) return -1;
    put_u8(&b, 10); /* protocol version */
    sbuf_append_str(&b, NLSQL_SERVER_VERSION); put_u8(&b, 0);
    put_u32(&b, 1); /* connection id */
    sbuf_append(&b, scram, 8);
    put_u8(&b, 0);  /* filler */
    put_u16(&b, (uint16_t)(cap & 0xFFFF));
    put_u8(&b, 33); /* charset utf8 */
    put_u16(&b, 0x0002);
    put_u16(&b, (uint16_t)(cap >> 16));
    put_u8(&b, 21); /* auth data len */
    sbuf_append(&b, "\0\0\0\0\0\0\0\0\0\0", 10);
    sbuf_append(&b, scram + 8, 12);
    put_u8(&b, 0);
    sbuf_append_str(&b, "mysql_native_password"); put_u8(&b, 0);
    int rc = wr_packet(io, (*seq)++, (const uint8_t*)b.p, b.len);
    sbuf_free(&b);
    if (rc != 0) return -1;

    /* 读握手响应（或 SSLRequest） */
    uint8_t* p = NULL; size_t len = 0;
    if (rd_packet(io, &p, &len, seq) != 0 || len < 32) {
        if (p) { memset(p, 0, len); free(p); }
        return -1;
    }
    uint32_t caps = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);

    /* SSLRequest：客户端先发 32 字节能力包，随后立即开始 TLS 握手；
       升级完成后在加密信道上重新读取真正的登录包（seq 继续递增）。
       未启用 TLS 时收到该包一律拒绝，绝不静默降级。 */
    if (caps & CLIENT_SSL) {
        memset(p, 0, len); free(p); p = NULL;
        if (!srv->tls_api) return -1;
        if (tls_start(srv, io) != 0) return -1;
        if (rd_packet(io, &p, &len, seq) != 0 || len < 32) {
            if (p) { memset(p, 0, len); free(p); }
            return -1;
        }
        caps = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    (*seq)++; /* 响应包（OK/ERR）序列号 = 客户端握手响应 seq + 1 */
    size_t off = 32;
    char user[128] = "";
    size_t ul = 0;
    while (off < len && p[off] && ul < sizeof(user) - 1) user[ul++] = (char)p[off++];
    off++;
    char db[65] = "";
    if ((caps & CLIENT_CONNECT_WITH_DB) && off < len) {
        /* 握手携带的库名：认证通过后自动 USE（GUI 客户端往往只在此处指定库） */
        size_t dl = 0;
        while (off < len && p[off] && dl < sizeof(db) - 1) db[dl++] = (char)p[off++];
        db[dl] = '\0';
        while (off < len && p[off]) off++;
        off++;
    }
    uint8_t token[20] = { 0 }; /* 初始化：无 token 分支下后续 XOR 不读未初始化栈 */
    int have_token = 0;
    if (caps & CLIENT_SECURE_CONNECTION) {
        if (off < len) {
            uint8_t tl = p[off++];
            if (tl == 20 && off + 20 <= len) { memcpy(token, p + off, 20); have_token = 1; }
        }
    }
    memset(p, 0, len); /* 握手响应含明文 token，释放前清零缩小泄露窗口 */
    free(p);

    /* 校验 */
    frw_acquire(&c->frw, FRW_EXCL);
    int accepted;
    if (c->require_auth && !c->have_creds) {
        accepted = 0; /* 强制认证但服务端无凭据：一律拒绝，避免空密码直连 */
    } else if (!c->have_creds) {
        if (!have_token) {
            accepted = 1; /* 空密码客户端（如 PyMySQL）不发 token，未配置凭据即放行 */
        } else {
        /* 未配置凭据：仅放行空密码。stored=SHA1(SHA1(""))，token=SHA1("")^SHA1(scram||stored) */
        uint8_t sp[20], stored[20];
        nlsha1("", 0, sp);          /* SHA1("") */
        nlsha1(sp, 20, stored);     /* SHA1(SHA1("")) */
        nlsha1_t s2;
        nlsha1_init(&s2);
        nlsha1_update(&s2, scram, 20);
        nlsha1_update(&s2, stored, 20);
        uint8_t h[20];
        nlsha1_final(&s2, h);
        uint8_t expect[20];
        for (int i = 0; i < 20; i++) expect[i] = sp[i] ^ h[i];
        accepted = ct_eq(token, expect, 20);
        }
        if (accepted) c->authenticated = 1;
    } else {
        /* stored = SHA1(SHA1(pass))；recovered = token XOR SHA1(scram||stored)；SHA1(recovered)==stored */
        uint8_t h[20];
        nlsha1_t s;
        nlsha1_init(&s);
        nlsha1_update(&s, scram, 20);
        nlsha1_update(&s, c->creds_sha, 20);
        nlsha1_final(&s, h);
        uint8_t rec[20];
        for (int i = 0; i < 20; i++) rec[i] = token[i] ^ h[i];
        uint8_t rehash[20];
        nlsha1(rec, 20, rehash);
        accepted = have_token &&
                   strcmp(user, c->creds_user) == 0 &&
                   ct_eq(rehash, c->creds_sha, 20);
        memset(rec, 0, sizeof(rec));
        memset(rehash, 0, sizeof(rehash));
        if (accepted) c->authenticated = 1;
    }
    frw_release(&c->frw, FRW_EXCL);

    /* 认证材料出栈前清零（token/scram 均为口令验证因子） */
    memset(token, 0, sizeof(token));
    memset(scram, 0, sizeof(scram));
    if (!accepted) {
        send_err(srv, io, seq, NLSQL_ER_ACCESS_DENIED, "28000", "Access denied");
        return -1;
    }
    send_ok(srv, io, seq, 0, 0);

    /* 客户端在握手里指定的库：认证成功后再切换（失败不拒连，保持旧行为兼容） */
    if (db[0] && db_name_ok(db)) {
        char sql[96];
        nlsql_result_t* r = NULL;
        snprintf(sql, sizeof(sql), "USE %s", db);
        nlsql_query(c, sql, &r);
        if (r) nlsql_free_result(r);
    }
    return 0;
}

/* =========================================
 * 服务端预处理（COM_STMT_*）：受特性开关 "server_prepare" 控制
 * ========================================= */

typedef struct {
    uint32_t id;
    char*    sql;
    int      nparams;
    uint16_t types[NLSQL_MAX_PARAMS > 0 ? 32 : 32]; /* 上次绑定的类型，供重复执行复用 */
    int      used;
} wstmt_t;

static uint64_t rd_le(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void put_u64le(sbuf_t* b, uint64_t v) {
    uint8_t t[8];
    for (int i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8 * i));
    sbuf_append(b, t, 8);
}

static void send_stmt_prepare_ok(wire_srv_t* srv, nlio_t* io, uint8_t* seq,
                                 uint32_t stmt_id, int nparams) {
    (void)srv;
    pstream_t ps;
    sbuf_t b;
    ps_init(&ps, io, *seq, 4096);
    if (sbuf_init(&b, 256) != 0) { ps_free(&ps); return; }
    b.len = 0;
    put_u8(&b, 0x00);
    put_u32(&b, stmt_id);
    put_u16(&b, 0);                 /* num_columns（结果列在执行时下发） */
    put_u16(&b, (uint16_t)nparams);
    put_u8(&b, 0);
    put_u16(&b, 0);                 /* warning_count */
    ps_add(&ps, (const uint8_t*)b.p, b.len);
    for (int i = 0; i < nparams; i++) {
        b.len = 0;
        put_str(&b, "def");
        put_str(&b, "");
        put_str(&b, "");
        put_str(&b, "");
        put_str(&b, "?");
        put_str(&b, "?");
        put_lenenc(&b, 0x0C);
        put_u16(&b, 33);
        put_u32(&b, 0);
        put_u8(&b, 253);            /* VAR_STRING */
        put_u16(&b, 0);
        put_u8(&b, 0);
        put_u16(&b, 0);
        ps_add(&ps, (const uint8_t*)b.p, b.len);
    }
    if (nparams > 0) {
        uint8_t eof[5] = { 0xFE, 0x00, 0x00, 0x02, 0x00 };
        ps_add(&ps, eof, 5);
    }
    ps_flush(&ps);
    *seq = ps.seq;
    ps_free(&ps);
    sbuf_free(&b);
}

static uint8_t mysql_type_of(nlsql_type_t t) {
    switch (t) {
        case NLSQL_TYPE_INT64:    return 8;   /* LONGLONG */
        case NLSQL_TYPE_DOUBLE:   return 5;
        case NLSQL_TYPE_TEXT:     return 252;
        case NLSQL_TYPE_DATETIME: return 12;
        case NLSQL_TYPE_NULL:     return 6;
        default:                  return 253; /* VAR_STRING */
    }
}

static void enc_datetime(sbuf_t* b, const char* s) {
    int Y = 0, M = 0, D = 0, h = 0, mi = 0, se = 0;
    sscanf(s ? s : "", "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &mi, &se);
    put_u8(b, 7);
    put_u16(b, (uint16_t)Y);
    put_u8(b, (uint8_t)M);
    put_u8(b, (uint8_t)D);
    put_u8(b, (uint8_t)h);
    put_u8(b, (uint8_t)mi);
    put_u8(b, (uint8_t)se);
}

static void send_binary_resultset(wire_srv_t* srv, nlio_t* io, uint8_t* seq, nlsql_result_t* res) {
    (void)srv;
    pstream_t ps;
    sbuf_t b, val;
    ps_init(&ps, io, *seq, 65536);
    if (sbuf_init(&b, 1024) != 0) { ps_free(&ps); return; }
    if (sbuf_init(&val, 64) != 0) { sbuf_free(&b); ps_free(&ps); return; }

    b.len = 0;
    put_lenenc(&b, (uint64_t)res->col_count);
    ps_add(&ps, (const uint8_t*)b.p, b.len);

    for (int i = 0; i < res->col_count; i++) {
        b.len = 0;
        put_str(&b, "def");
        put_str(&b, "");
        put_str(&b, "");
        put_str(&b, "");
        put_str(&b, res->cols[i].name);
        put_str(&b, res->cols[i].name);
        put_lenenc(&b, 0x0C);
        put_u16(&b, 33);
        put_u32(&b, 1024);
        put_u8(&b, mysql_type_of(res->cols[i].type));
        put_u16(&b, 0);
        put_u8(&b, 31);
        put_u16(&b, 0);
        ps_add(&ps, (const uint8_t*)b.p, b.len);
    }
    uint8_t eof[5] = { 0xFE, 0x00, 0x00, 0x02, 0x00 };
    ps_add(&ps, eof, 5);

    int ncols = res->col_count;
    size_t nbm = (size_t)((ncols + 7 + 2) / 8);
    for (uint64_t r = 0; r < res->row_count; r++) {
        b.len = 0;
        put_u8(&b, 0x00);                       /* binary row header */
        size_t bm_off = b.len;
        for (size_t i = 0; i < nbm; i++) put_u8(&b, 0);
        for (int i = 0; i < ncols; i++) {
            nlsql_value_t* v = &res->rows[r].vals[i];
            if (v->is_null || v->type == NLSQL_TYPE_NULL) {
                size_t bit = (size_t)i + 2;
                b.p[bm_off + bit / 8] |= (char)(1u << (bit % 8));
                continue;
            }
            switch (mysql_type_of(res->cols[i].type)) {
                case 8: put_u64le(&b, (uint64_t)v->i); break;
                case 5: {
                    double d = v->d;
                    uint64_t bits;
                    memcpy(&bits, &d, 8);
                    put_u64le(&b, bits);
                    break;
                }
                case 12: enc_datetime(&b, v->s); break;
                default:
                    val.len = 0;
                    value_text(v, &val);
                    put_lenenc(&b, val.len);
                    sbuf_append(&b, val.p, val.len);
                    break;
            }
        }
        ps_add(&ps, (const uint8_t*)b.p, b.len);
    }
    ps_add(&ps, eof, 5);
    ps_flush(&ps);
    *seq = ps.seq;
    ps_free(&ps);
    sbuf_free(&b);
    sbuf_free(&val);
}

static wstmt_t* stmt_find(wstmt_t* arr, uint32_t id) {
    for (int i = 0; i < WS_MAX_STMT; i++)
        if (arr[i].used && arr[i].id == id) return &arr[i];
    return NULL;
}

static wstmt_t* stmt_alloc(wstmt_t* arr, uint32_t* next_id) {
    for (int i = 0; i < WS_MAX_STMT; i++)
        if (!arr[i].used) {
            memset(&arr[i], 0, sizeof(arr[i]));
            arr[i].used = 1;
            arr[i].id = (*next_id)++;
            return &arr[i];
        }
    return NULL;
}

static void stmt_clear(wstmt_t* s) {
    free(s->sql);
    memset(s, 0, sizeof(*s));
}

static void handle_stmt_prepare(wire_srv_t* srv, nlio_t* io, uint8_t* seq,
                                const char* sql, wstmt_t* arr, uint32_t* next_id) {
    nlsql_conn_t* c = srv->conn;
    nlsql_stmt_t* st = NULL;
    int rc = nlsql_prepare(c, sql, &st);
    if (rc != NLSQL_OK) {
        char emsg[512] = "";
        char estate[8] = "HY000";
        nlsql_copy_err(c, NULL, emsg, sizeof(emsg), estate, sizeof(estate));
        send_err(srv, io, seq, rc, estate, emsg);
        return;
    }
    int np = nlsql_stmt_param_count(st);
    nlsql_stmt_free(st);
    wstmt_t* w = stmt_alloc(arr, next_id);
    if (!w) { send_err(srv, io, seq, NLSQL_ER_NOT_SUPPORTED, "42000", "Too many prepared statements"); return; }
    w->sql = strdup(sql);
    w->nparams = np;
    send_stmt_prepare_ok(srv, io, seq, w->id, np);
}

/* 解码参数并执行；返回 0 成功 */
static void handle_stmt_execute(wire_srv_t* srv, nlio_t* io, uint8_t* seq,
                                const uint8_t* p, size_t len, wstmt_t* arr) {
    nlsql_conn_t* c = srv->conn;
    if (len < 10) { send_err(srv, io, seq, NLSQL_ER_UNKNOWN_STMT, "HY000", "Malformed COM_STMT_EXECUTE"); return; }
    uint32_t id = (uint32_t)rd_le(p + 1, 4);
    wstmt_t* w = stmt_find(arr, id);
    if (!w) { send_err(srv, io, seq, NLSQL_ER_UNKNOWN_STMT, "HY000", "Unknown prepared statement handler"); return; }
    int np = w->nparams;
    nlsql_value_t params[32];
    char* strs[32] = { 0 };
    memset(params, 0, sizeof(params));

    size_t off = 10;
    const uint8_t* nullmap = NULL;
    if (np > 0) {
        size_t nbm = (size_t)((np + 7) / 8);
        if (off + nbm > len) { send_err(srv, io, seq, NLSQL_ER_UNKNOWN_STMT, "HY000", "Bad param bitmap"); return; }
        nullmap = p + off;
        off += nbm;
    }
    int new_bound = (off < len) ? p[off++] : 0;
    const uint8_t* types = NULL;
    if (new_bound) {
        if (np > 0) {
            if (off + (size_t)np * 2 > len) { send_err(srv, io, seq, NLSQL_ER_UNKNOWN_STMT, "HY000", "Bad param types"); return; }
            types = p + off;
            for (int i = 0; i < np; i++) w->types[i] = (uint16_t)rd_le(types + i * 2, 2);
            off += (size_t)np * 2;
        }
    }
    /* new_bound == 0 时沿用上次绑定的类型（w->types 保持不变） */

    for (int i = 0; i < np; i++) {
        int is_null = nullmap && (nullmap[i / 8] & (1u << (i % 8)));
        params[i].type = NLSQL_TYPE_NULL;
        params[i].is_null = 1;
        if (is_null) continue;
        params[i].is_null = 0;   /* 非 NULL 参数：显式清掉占位标记 */
        uint16_t ty = w->types[i];
        switch (ty) {
            case 0x01: { if (off + 1 > len) goto bad; params[i].type = NLSQL_TYPE_INT64; params[i].i = (int8_t)p[off]; off += 1; break; }
            case 0x02: { if (off + 2 > len) goto bad; params[i].type = NLSQL_TYPE_INT64; params[i].i = (int16_t)rd_le(p + off, 2); off += 2; break; }
            case 0x03: { if (off + 4 > len) goto bad; params[i].type = NLSQL_TYPE_INT64; params[i].i = (int32_t)rd_le(p + off, 4); off += 4; break; }
            case 0x08: { if (off + 8 > len) goto bad; params[i].type = NLSQL_TYPE_INT64; params[i].i = (int64_t)rd_le(p + off, 8); off += 8; break; }
            case 0x04: { if (off + 4 > len) goto bad; uint32_t bits = (uint32_t)rd_le(p + off, 4); float f; memcpy(&f, &bits, 4); params[i].type = NLSQL_TYPE_DOUBLE; params[i].d = (double)f; off += 4; break; }
            case 0x05: { if (off + 8 > len) goto bad; uint64_t bits = rd_le(p + off, 8); double d; memcpy(&d, &bits, 8); params[i].type = NLSQL_TYPE_DOUBLE; params[i].d = d; off += 8; break; }
            case 0x06: params[i].type = NLSQL_TYPE_NULL; params[i].is_null = 1; break;
            case 0x0C: case 0x07: case 0x0A: case 0x0B: {
                if (off >= len) goto bad;
                uint8_t dl = p[off++];
                if (dl == 0) { params[i].type = NLSQL_TYPE_NULL; params[i].is_null = 1; break; }
                if (off + dl > len) goto bad;
                int Y = 0, M = 0, D = 0, h = 0, mi = 0, se = 0;
                if (dl >= 4) { Y = (int)rd_le(p + off, 2); M = p[off + 2]; D = p[off + 3]; }
                if (dl >= 7) { h = p[off + 4]; mi = p[off + 5]; se = p[off + 6]; }
                off += dl;
                char* buf = (char*)malloc(32);
                if (!buf) goto bad;
                snprintf(buf, 32, "%04d-%02d-%02d %02d:%02d:%02d", Y, M, D, h, mi, se);
                strs[i] = buf;
                params[i].type = NLSQL_TYPE_DATETIME;
                params[i].s = buf;
                params[i].len = (uint32_t)strlen(buf);
                break;
            }
            default: {
                if (off >= len) goto bad;
                uint64_t n = 0;
                size_t used = 0;
                uint8_t b0 = p[off];
                if (b0 < 0xFB) { n = b0; used = 1; }
                else if (b0 == 0xFC && off + 3 <= len) { n = rd_le(p + off + 1, 2); used = 3; }
                else if (b0 == 0xFD && off + 4 <= len) { n = rd_le(p + off + 1, 3); used = 4; }
                else if (b0 == 0xFE && off + 9 <= len) { n = rd_le(p + off + 1, 8); used = 9; }
                else goto bad;
                off += used;
                if (n > len - off) goto bad;
                char* buf = (char*)malloc((size_t)n + 1);
                if (!buf) goto bad;
                memcpy(buf, p + off, (size_t)n);
                buf[n] = '\0';
                off += (size_t)n;
                strs[i] = buf;
                params[i].type = NLSQL_TYPE_VARCHAR;
                params[i].s = buf;
                params[i].len = (uint32_t)n;
                break;
            }
        }
    }

    {
        nlsql_result_t* res = NULL;
        int rc = nlsql_exec_sql(c, w->sql, params, np, &res);
        if (rc == NLSQL_OK) nlsql_maybe_flush(c);
        if (rc != NLSQL_OK) {
            char emsg[512] = "";
            char estate[8] = "HY000";
            nlsql_copy_err(c, NULL, emsg, sizeof(emsg), estate, sizeof(estate));
            send_err(srv, io, seq, rc, estate, emsg);
        } else if (res && res->col_count > 0) {
            send_binary_resultset(srv, io, seq, res);
            nlsql_free_result(res);
        } else {
            uint64_t aff = res ? res->affected : 0;
            int64_t iid = res ? res->insert_id : 0;
            nlsql_free_result(res);
            send_ok(srv, io, seq, aff, iid);
        }
    }
    for (int i = 0; i < np; i++) free(strs[i]);
    return;
bad:
    for (int i = 0; i < np; i++) free(strs[i]);
    send_err(srv, io, seq, NLSQL_ER_UNKNOWN_STMT, "HY000", "Malformed prepared statement parameters");
}

/* =========================================
 * 命令循环
 * ========================================= */

static void handle_query(wire_srv_t* srv, nlio_t* io, uint8_t* seq, const char* sql) {
    nlsql_conn_t* c = srv->conn;
    nlsql_result_t* res = NULL;
    int rc;
    char emsg[512] = "";   /* 与 conn->err_msg 同尺寸，免截断告警 */
    char estate[8] = "HY000";
    /* 与嵌入式 API 共用执行入口：扩展预改写/处理器对 wire 同样生效 */
    rc = nlsql_exec_sql(c, sql, NULL, 0, &res);
    if (rc == NLSQL_OK) nlsql_maybe_flush(c);  /* 压实走 SNAP 锁，不阻塞并发读 */
    /* 锁外拷出错误文本（错误槽自带互斥，避免读者并发覆盖造成撕裂） */
    if (rc != NLSQL_OK) nlsql_copy_err(c, NULL, emsg, sizeof(emsg), estate, sizeof(estate));
    if (rc != NLSQL_OK) {
        send_err(srv, io, seq, rc, estate, emsg);
    } else if (res && res->col_count > 0) {
        send_resultset(srv, io, seq, res);
        nlsql_free_result(res);
    } else {
        uint64_t aff = res ? res->affected : 0;
        int64_t iid = res ? res->insert_id : 0;
        nlsql_free_result(res);
        send_ok(srv, io, seq, aff, iid);
    }
}

static void* cli_thread(void* arg) {
    cli_arg_t* a = (cli_arg_t*)arg;
    wire_srv_t* srv = a->srv;
    int fd = a->fd;
    free(a);
    uint8_t seq = 0;
    nlio_t io = { fd, NULL, NULL };
    /* 每连接独立的服务端预处理语句表（连接断开时统一释放） */
    wstmt_t stmts[WS_MAX_STMT];
    uint32_t next_id = 1;
    memset(stmts, 0, sizeof(stmts));
    if (do_handshake(srv, &io, &seq) == 0) {
        for (;;) {
            uint8_t* p = NULL; size_t len = 0;
            if (rd_cmd_packet(&io, &p, &len, &seq) != 0 || len < 1) { free(p); break; }
            seq++; /* 响应序列号 = 客户端命令 seq + 1 */
            uint8_t cmd = p[0];
            if (cmd == COM_QUIT) { free(p); break; }
            if (cmd == COM_PING) { free(p); send_ok(srv, &io, &seq, 0, 0); continue; }
            if (cmd == COM_INIT_DB) {
                p[len] = '\0';
                char sql[128];
                snprintf(sql, sizeof(sql), "USE %s", (char*)(p + 1));
                free(p);
                handle_query(srv, &io, &seq, sql);
                continue;
            }
            if (cmd == COM_QUERY) {
                p[len] = '\0';
                handle_query(srv, &io, &seq, (char*)(p + 1));
                free(p);
                continue;
            }
            /* 服务端预处理：由 base 扩展开启 "server_prepare" 特性后可用 */
            if (cmd == COM_STMT_PREPARE || cmd == COM_STMT_EXECUTE ||
                cmd == COM_STMT_CLOSE || cmd == COM_STMT_RESET) {
                if (!nlsql_ext_feature("server_prepare")) {
                    free(p);
                    send_err(srv, &io, &seq, NLSQL_ER_NOT_SUPPORTED, "42000",
                             "Server-side prepared statements are disabled");
                    continue;
                }
                if (cmd == COM_STMT_PREPARE) {
                    p[len] = '\0';
                    handle_stmt_prepare(srv, &io, &seq, (char*)(p + 1), stmts, &next_id);
                } else if (cmd == COM_STMT_EXECUTE) {
                    handle_stmt_execute(srv, &io, &seq, p, len, stmts);
                } else if (cmd == COM_STMT_CLOSE) {
                    /* 协议规定无响应包 */
                    if (len >= 5) {
                        wstmt_t* w = stmt_find(stmts, (uint32_t)rd_le(p + 1, 4));
                        if (w) stmt_clear(w);
                    }
                } else { /* COM_STMT_RESET */
                    if (len >= 5) {
                        wstmt_t* w = stmt_find(stmts, (uint32_t)rd_le(p + 1, 4));
                        if (!w) {
                            free(p);
                            send_err(srv, &io, &seq, NLSQL_ER_UNKNOWN_STMT, "HY000",
                                     "Unknown prepared statement handler");
                            continue;
                        }
                        for (int i = 0; i < 32; i++) w->types[i] = 0; /* 清空绑定类型 */
                    }
                    send_ok(srv, &io, &seq, 0, 0);
                }
                free(p);
                continue;
            }
            free(p);
            send_err(srv, &io, &seq, NLSQL_ER_NOT_SUPPORTED, "42000",
                     "Command not supported by NLSQL");
        }
    }
    for (int i = 0; i < WS_MAX_STMT; i++)
        if (stmts[i].used) stmt_clear(&stmts[i]);
    /* TLS 会话收尾：发 close_notify 并释放 netleaf 上下文（fd 由本线程 close） */
    if (io.ctx) {
        if (io.tls->close) io.tls->close(io.ctx);
        io.tls->destroy(io.ctx);
        io.ctx = NULL;
    }
    /* 先摘槽再 close：槽位登记期间 fd 必持有，stop 才能安全 shutdown（无 fd 复用误伤） */
    pthread_mutex_lock(&srv->cmtx);
    for (int i = 0; i < srv->nclients; i++) {
        if (srv->client_fds[i] == fd) {
            srv->client_fds[i] = srv->client_fds[srv->nclients - 1];
            srv->nclients--;
            break;
        }
    }
    pthread_cond_broadcast(&srv->cnd); /* 通知 wire_stop：已退场 */
    pthread_mutex_unlock(&srv->cmtx);
    shutdown(fd, SHUT_RDWR);
    nlsql_sock_close(fd);
    return NULL;
}

/* 摘除 fd 槽位（槽登记期 fd 由登记方持有，按 fd 匹配安全） */
static void slot_remove(wire_srv_t* srv, int fd) {
    pthread_mutex_lock(&srv->cmtx);
    for (int i = 0; i < srv->nclients; i++) {
        if (srv->client_fds[i] == fd) {
            srv->client_fds[i] = srv->client_fds[srv->nclients - 1];
            srv->nclients--;
            break;
        }
    }
    pthread_mutex_unlock(&srv->cmtx);
}

static void* accept_thread(void* arg) {
    wire_srv_t* srv = (wire_srv_t*)arg;
    while (srv->running) {
        struct pollfd pf = { .fd = srv->listen_fd, .events = POLLIN };
        int pr = poll(&pf, 1, 200);
        if (pr <= 0) continue;
        int fd = accept(srv->listen_fd, NULL, NULL);
        if (fd < 0) continue;
        /* 先登记槽位再创建线程：消除“线程先退场、槽后登记”导致计数永不归零的死锁窗口 */
        pthread_mutex_lock(&srv->cmtx);
        int full = srv->nclients >= srv->max_clients;
        if (!full) srv->client_fds[srv->nclients++] = fd;
        pthread_mutex_unlock(&srv->cmtx);
        if (full) {
            uint8_t seq2 = 0;
            nlio_t tio = { fd, NULL, NULL };
            send_err(srv, &tio, &seq2, 1040, "08004", "Too many connections");
            nlsql_sock_close(fd);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        cli_arg_t* a = (cli_arg_t*)malloc(sizeof(cli_arg_t));
        if (!a) { slot_remove(srv, fd); nlsql_sock_close(fd); continue; }
        a->srv = srv; a->fd = fd;
        pthread_t th;
        if (pthread_create(&th, NULL, cli_thread, a) != 0) {
            free(a); slot_remove(srv, fd); nlsql_sock_close(fd); continue;
        }
        /* 线程即刻 detach：wire_stop 以 nclients==0 的条件变量同步生命周期，
           无需 join（TSAN thread-leak 报警随之消除），退场资源由系统自动回收 */
        pthread_detach(th);
    }
    return NULL;
}

/* =========================================
 * 启停
 * ========================================= */

wire_srv_t* wire_start_tls(nlsql_conn_t* conn, int port, int max_clients,
                           const char* cert_file, const char* key_file,
                           char* err, uint32_t errsz) {
    if (port <= 0 || port > 65535) { snprintf(err, errsz, "invalid port"); return NULL; }
    if (max_clients <= 0) max_clients = 16;
    if (max_clients > MAX_TRACKED_CLIENTS) max_clients = MAX_TRACKED_CLIENTS;
    wire_srv_t* w = (wire_srv_t*)calloc(1, sizeof(wire_srv_t));
    if (!w) { snprintf(err, errsz, "out of memory"); return NULL; }
    if (cert_file && key_file && cert_file[0] && key_file[0]) {
        if (strlen(cert_file) >= sizeof(w->cert_file) ||
            strlen(key_file) >= sizeof(w->key_file)) {
            snprintf(err, errsz, "cert/key path too long");
            free(w); return NULL;
        }
        snprintf(w->cert_file, sizeof(w->cert_file), "%s", cert_file);
        snprintf(w->key_file, sizeof(w->key_file), "%s", key_file);
        w->tls_api = tls_load();
        if (!w->tls_api) {
            /* 显式要求 TLS 而扩展不可用：直接失败，不静默退化为明文 */
            snprintf(err, errsz, "TLS requested but NetLeaf TLS2 extension not found "
                     "(set NLSQL_TLS_LIB or install libnetleaf_tls2)");
            free(w); return NULL;
        }
    }
    w->conn = conn;
    w->max_clients = max_clients;
    w->running = 1;
    pthread_mutex_init(&w->cmtx, NULL);
    pthread_cond_init(&w->cnd, NULL);

    if (nlsql_sock_global_init() != 0) { snprintf(err, errsz, "socket init failed"); free(w); return NULL; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(err, errsz, "socket failed"); free(w); return NULL; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 仅环回，绝不对外 */
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        snprintf(err, errsz, "bind 127.0.0.1:%d failed", port);
        nlsql_sock_close(fd); free(w); return NULL;
    }
    if (listen(fd, 16) != 0) {
        snprintf(err, errsz, "listen failed");
        nlsql_sock_close(fd); free(w); return NULL;
    }
    w->listen_fd = fd;
    if (pthread_create(&w->accept_th, NULL, accept_thread, w) != 0) {
        snprintf(err, errsz, "cannot start accept thread");
        nlsql_sock_close(fd); free(w); return NULL;
    }
    return w;
}

/* 明文服务端：不启用 TLS，会话与 TLS 共用同一命令循环 */
wire_srv_t* wire_start(nlsql_conn_t* conn, int port, int max_clients, char* err, uint32_t errsz) {
    return wire_start_tls(conn, port, max_clients, NULL, NULL, err, errsz);
}

void wire_stop(wire_srv_t* w) {
    if (!w) return;
    w->running = 0;
    pthread_join(w->accept_th, NULL);
    nlsql_sock_close(w->listen_fd);
    /* shutdown 所有活跃连接：令阻塞 read 立即返回，cli_thread 走退出清理；
       退场时 swap-pop + broadcast，此处等计数归零（所有 cli 已越过 srv 使用点）
       —— 不允许超时后 free：那是 use-after-free / 堆破坏 */
    pthread_mutex_lock(&w->cmtx);
    for (int i = 0; i < w->nclients; i++) shutdown(w->client_fds[i], SHUT_RDWR);
    while (w->nclients > 0) pthread_cond_wait(&w->cnd, &w->cmtx);
    pthread_mutex_unlock(&w->cmtx);
    pthread_mutex_destroy(&w->cmtx);
    pthread_cond_destroy(&w->cnd);
    free(w);
}
