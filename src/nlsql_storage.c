/**
 * @file nlsql_storage.c
 * @brief NLSQL 存储层：表快照 + 每库 WAL 追加日志 + 压实
 *
 * 布局：
 *   <root>/<db>/<table>.nlt   表快照（原子写：tmp + rename）
 *   <root>/<db>/journal.log   WAL（先追加后改内存，崩溃可重放）
 *
 * 压实：journal 超过阈值（默认 64MB，可经 journal_mb 配置）或显式 flush 时，重写全部快照并清空 journal。
 */
#include "nlsql_internal.h"
#include "netleaf_nlsql_platform.h"
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pthread.h>

/* 主键索引互斥：懒重建（重建时释放并换新槽数组）与并发读者的探测必须串行，
   否则共享读锁下 SELECT 与重建并行 → 读到已 free 的槽数组（UAF）。
   每次只包一个短操作，EXCL 写路径本就无并发，不构成瓶颈。 */
static pthread_mutex_t g_pk_mtx = PTHREAD_MUTEX_INITIALIZER;

/* 格式 v2：快照头部含 snap_lsn、journal 记录含 LSN（13 字节头）——
   快照与 WAL 按 (表, LSN) 对齐，压实中途崩溃也不重复/不丢失。未发布格式无兼容负担 */
#define NLT_MAGIC "NLT2"
#define JRL_MAGIC "NLJL2\n"
#define JR_HDR_LEN 13   /* type(1) + plen(4) + lsn(8) */

/* =========================================
 * 值编码（journal / 快照共用）
 * 0=null 1=int64 2=double 3=str(lenenc+bytes)
 * ========================================= */

static int enc_value(sbuf_t* b, const nlsql_value_t* v) {
    if (!v || v->is_null || v->type == NLSQL_TYPE_NULL)
        return sbuf_append_u8(b, 0);
    switch (v->type) {
        case NLSQL_TYPE_INT64:
            if (sbuf_append_u8(b, 1) != 0) return -1;
            return sbuf_append_u64(b, (uint64_t)v->i);
        case NLSQL_TYPE_DOUBLE: {
            uint64_t bits;
            memcpy(&bits, &v->d, 8);
            if (sbuf_append_u8(b, 2) != 0) return -1;
            return sbuf_append_u64(b, bits);
        }
        default:
            if (sbuf_append_u8(b, 3) != 0) return -1;
            {
                uint8_t le[9];
                uint32_t n = nl_lenenc(v->len, le);
                if (sbuf_append(b, le, n) != 0) return -1;
                if (v->len) return sbuf_append(b, v->s, v->len);
            }
            return 0;
    }
}

static const uint8_t* dec_value(const uint8_t* p, uint32_t avail, nlsql_value_t* v) {
    if (avail < 1) return NULL;
    uint8_t tag = *p++; avail--;
    memset(v, 0, sizeof(*v));
    if (tag == 0) { v->is_null = 1; v->type = NLSQL_TYPE_NULL; return p; }
    if (tag == 1) {
        if (avail < 8) return NULL;
        uint64_t bits = 0;
        for (int i = 0; i < 8; i++) bits |= ((uint64_t)p[i]) << (8 * i);
        v->type = NLSQL_TYPE_INT64; v->i = (int64_t)bits;
        return p + 8;
    }
    if (tag == 2) {
        if (avail < 8) return NULL;
        uint64_t bits = 0;
        for (int i = 0; i < 8; i++) bits |= ((uint64_t)p[i]) << (8 * i);
        memcpy(&v->d, &bits, 8);
        v->type = NLSQL_TYPE_DOUBLE;
        return p + 8;
    }
    if (tag == 3) {
        uint64_t n;
        uint32_t used = nl_read_lenenc(p, avail, &n);
        if (!used || n > avail - used) return NULL;
        v->type = NLSQL_TYPE_VARCHAR;
        v->len = (uint32_t)n;
        if (n) {
            v->s = (char*)malloc((size_t)n + 1);
            if (!v->s) return NULL;
            memcpy(v->s, p + used, n);
            v->s[n] = '\0';
        } else {
            v->s = (char*)calloc(1, 1);
            if (!v->s) return NULL;
        }
        return p + used + n;
    }
    return NULL;
}

/* =========================================
 * journal 追加
 * ========================================= */

struct journal {
    char path[1100];
    int  fd;
    uint64_t size;
};

static int jr_open(journal_t** out, const char* dbdir, int readonly) {
    journal_t* j = (journal_t*)calloc(1, sizeof(journal_t));
    if (!j) return -1;
    snprintf(j->path, sizeof(j->path), "%s/journal.log", dbdir);
    /* 只读连接：O_RDONLY|O_CREAT（文件不存在时创建空文件但不写内容），
       禁止对写者的 WAL 做任何 magic 写入 / 截断 */
    j->fd = nlsql_file_open(j->path, readonly ? (O_RDONLY | O_CREAT) : (O_CREAT | O_RDWR | O_APPEND), 0600);
    if (j->fd < 0) { free(j); return -1; }
    struct stat st;
    if (fstat(j->fd, &st) == 0) j->size = (uint64_t)st.st_size;
    /* 写文件头（新建时） */
    if (j->size == 0 && !readonly) {
        ssize_t w = write(j->fd, JRL_MAGIC, sizeof(JRL_MAGIC) - 1);
        if (w <= 0) { close(j->fd); free(j); return -1; } /* magic 写失败即报错：否则 WAL 永不被重放 */
        j->size += (uint64_t)w;
    }
    *out = j;
    return 0;
}

static void jr_close(journal_t* j) {
    if (j) {
        if (j->fd >= 0) close(j->fd);
        free(j);
    }
}

/* journal 落盘：把用户态缓冲一次 write 写入 fd */
static int jr_out(engine_t* e) {
    if (!e->jlen) return 0;
    if (!e->jr) { e->jlen = 0; return 0; }
    uint32_t off = 0;
    while (off < e->jlen) {
        ssize_t w = write(e->jr->fd, e->jbuf + off, e->jlen - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            /* 部分写后失败：已写字节从缓冲前移，否则下次从头重写造成 journal 前缀重复错位 */
            if (off > 0) {
                e->jlen -= off;
                memmove(e->jbuf, e->jbuf + off, e->jlen);
                e->jr->size += off;
            }
            return -1;
        }
        if (w == 0) {
            if (off > 0) {
                e->jlen -= off;
                memmove(e->jbuf, e->jbuf + off, e->jlen);
                e->jr->size += off;
            }
            return -1;
        }
        off += (uint32_t)w;
    }
    e->jr->size += e->jlen;
    e->jlen = 0;
    return 0;
}

int nlsql_jr_flush(engine_t* e) { return jr_out(e) == 0 ? NLSQL_OK : NLSQL_ERR_IO; }

/* 自动落盘边界：先写缓冲再 fsync，确保已提交记录扛得住掉电 */
int nlsql_jr_sync(engine_t* e) {
    if (jr_out(e) != 0) return NLSQL_ERR_IO;
    if (!e->jr || e->jr->fd < 0) return NLSQL_OK;
    return fsync(e->jr->fd) == 0 ? NLSQL_OK : NLSQL_ERR_IO;
}

#define NLSQL_JBUF_CAP (64u * 1024u)

/* 直接写一段到 journal fd（处理短写） */
static int jr_raw_write(int fd, const void* data, uint32_t n) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) return -1;
        off += (uint32_t)w;
    }
    return 0;
}

/* 追加一条 journal 记录到写缓冲（满 64KB 自动落盘；无每行 malloc/write）。
   单条记录 > 64KB（大 TEXT 行）时：先落盘缓冲，再绕过缓冲直接写该条；
   中途崩溃产生的半条由重放按长度校验截断到完整前缀 */
static int jr_append(engine_t* e, uint8_t type, const void* payload, uint32_t plen) {
    if (!e->jr) return -1;
    uint64_t rec_lsn = ++e->lsn;
    uint32_t need = JR_HDR_LEN + plen;
    if (need > NLSQL_JBUF_CAP) {
        if (jr_out(e) != 0) return -1;
        uint8_t hdr[JR_HDR_LEN];
        hdr[0] = type;
        hdr[1] = (uint8_t)plen; hdr[2] = (uint8_t)(plen >> 8);
        hdr[3] = (uint8_t)(plen >> 16); hdr[4] = (uint8_t)(plen >> 24);
        for (int i = 0; i < 8; i++) hdr[5 + i] = (uint8_t)(rec_lsn >> (8 * i));
        if (jr_raw_write(e->jr->fd, hdr, JR_HDR_LEN) != 0) return -1;
        if (plen && jr_raw_write(e->jr->fd, payload, plen) != 0) return -1;
        e->jr->size += need;
        return 0;
    }
    if (!e->jbuf) {
        e->jbuf = (uint8_t*)malloc(NLSQL_JBUF_CAP);
        if (!e->jbuf) return -1;
        e->jcap = NLSQL_JBUF_CAP; e->jlen = 0;
    }
    if (e->jlen + need > e->jcap && jr_out(e) != 0) return -1;
    uint8_t* p = e->jbuf + e->jlen;
    p[0] = type;
    p[1] = (uint8_t)plen; p[2] = (uint8_t)(plen >> 8);
    p[3] = (uint8_t)(plen >> 16); p[4] = (uint8_t)(plen >> 24);
    for (int i = 0; i < 8; i++) p[5 + i] = (uint8_t)(rec_lsn >> (8 * i));
    if (plen) memcpy(p + JR_HDR_LEN, payload, plen);
    e->jlen += need;
    return 0;
}

static void jr_put_str(sbuf_t* b, const char* s) {
    uint8_t le[9];
    uint32_t n = nl_lenenc(strlen(s), le);
    sbuf_append(b, le, n);
    sbuf_append_str(b, s);
}

/* =========================================
 * 表快照：并行流式编码流水线
 *
 * 快照占压实总时间的绝大部分（GB 级库要重编码全部行）。原实现单线程
 * 「编码 1MB → 写盘」严格串行，CPU 编码与磁盘写入无法重叠。
 * 现改为：W 个编码线程按固定行块并行编码，结果投入有界重排窗口，
 * 由写线程按块序落盘 —— 编码（多核）与写盘、fsync 重叠推进；
 * 峰值内存恒定在 (W + SLOTS) × 块大小（约 10MB），与表大小无关。
 * ========================================= */

#define SNAP_BLK_BYTES  (1024u * 1024u)  /* 单块目标字节 */
#define SNAP_SLOTS      8                /* 有序重排窗口槽位数 */
#define SNAP_MAX_ENC    4                /* 并行编码线程上限（低内存 + 通用指令） */

/* 增量帧：基础行区之后可追加若干「只含新增行」的帧，压实不再重写旧行。
   帧 = lsn(8) + nrows(8) + next_auto(8) + 行编码（与基础行同格式）。
   帧数超上限则整表重写一次合并掉，防止长期只追加导致加载解析过长。 */
#define SNAP_FRAME_HDR  24
#define SNAP_MAX_FRAMES 64

typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    uint8_t*        buf[SNAP_SLOTS];
    uint32_t        len[SNAP_SLOTS];
    uint8_t         ready[SNAP_SLOTS];
    uint64_t        write_idx;   /* 下一个待落盘块（也是窗口左沿） */
    uint64_t        next_idx;    /* 下一个待编码块 */
    uint64_t        nblocks;
    uint64_t        rows_per_blk;
    uint64_t        row_lo;      /* 本段起始行（增量帧从 snap_rows 开始编码） */
    uint64_t        row_count;   /* 本段结束行（开区间上界） */
    table_t*        t;
    int             stop;
    int             err;
} snap_pipe_t;

static void snap_pipe_fail(snap_pipe_t* pp) {
    pthread_mutex_lock(&pp->mtx);
    pp->err = 1;
    pp->stop = 1;
    pthread_cond_broadcast(&pp->cv);
    pthread_mutex_unlock(&pp->mtx);
}

static void* snap_encoder(void* arg) {
    snap_pipe_t* pp = (snap_pipe_t*)arg;
    sbuf_t b;
    if (sbuf_init(&b, SNAP_BLK_BYTES + 4096) != 0) { snap_pipe_fail(pp); return NULL; }
    for (;;) {
        pthread_mutex_lock(&pp->mtx);
        while (!pp->stop && pp->next_idx >= pp->write_idx + SNAP_SLOTS)
            pthread_cond_wait(&pp->cv, &pp->mtx);
        if (pp->stop) { pthread_mutex_unlock(&pp->mtx); break; }
        uint64_t k = pp->next_idx++;
        pthread_mutex_unlock(&pp->mtx);

        uint64_t lo = pp->row_lo + k * pp->rows_per_blk;
        uint64_t hi = lo + pp->rows_per_blk;
        if (hi > pp->row_count) hi = pp->row_count;
        b.len = 0;
        int ok = 1;
        for (uint64_t r = lo; r < hi && ok; r++)
            for (int i = 0; i < pp->t->col_count; i++)
                if (enc_value(&b, &pp->t->rows[r].vals[i]) != 0) { ok = 0; break; }

        pthread_mutex_lock(&pp->mtx);
        if (pp->stop || !ok) {
            pp->err = 1;
            pp->stop = 1;
            pthread_cond_broadcast(&pp->cv);
            pthread_mutex_unlock(&pp->mtx);
            break;
        }
        uint32_t slot = (uint32_t)(k % SNAP_SLOTS);
        pp->buf[slot] = (uint8_t*)b.p;   /* 缓冲所有权交给写线程 */
        pp->len[slot] = b.len;
        pp->ready[slot] = 1;
        pthread_cond_broadcast(&pp->cv);
        pthread_mutex_unlock(&pp->mtx);

        if (sbuf_init(&b, SNAP_BLK_BYTES + 4096) != 0) { snap_pipe_fail(pp); break; }
    }
    sbuf_free(&b);
    return NULL;
}

/* 估算每块行数：大 TEXT 行进小分块（避免一块含多行 2MB），窄行进大分块 */
static uint64_t snap_rows_per_blk(const table_t* t) {
    uint64_t est = 0;
    for (int i = 0; i < t->col_count; i++) {
        if (t->cols[i].type == NLSQL_TYPE_VARCHAR) est += t->cols[i].vlen + 5;
        else est += 16;
    }
    if (est == 0) est = 16;
    uint64_t per = SNAP_BLK_BYTES / est;
    if (per < 1) per = 1;
    if (per > 65536) per = 65536;
    return per;
}

static int tbl_save_snapshot(engine_t* e, table_t* t);
static int tbl_append_snapshot(engine_t* e, table_t* t, const char* path);

/* 把行区间 [lo_row, hi_row) 编码并流式写入 f（并行编码 + 有序重排窗口）。
   返回 1 成功 / 0 失败。 */
static int enc_rows_stream(table_t* t, uint64_t lo_row, uint64_t hi_row, FILE* f) {
    if (lo_row >= hi_row) return 1;
    int ok = 1;
    uint64_t per = snap_rows_per_blk(t);
    uint64_t nblocks = (hi_row - lo_row + per - 1) / per;
    /* 编码线程用满可用核：压实线程整体降 nice（见 flush_thread_main），不会抢读者的 CPU */
    long ncpu = nlsql_cpu_count();
    int nenc = (int)(ncpu > 0 ? ncpu : 1);
    if (nenc > SNAP_MAX_ENC) nenc = SNAP_MAX_ENC;
    if (nblocks == 0) nenc = 0;
    else if ((uint64_t)nenc > nblocks) nenc = (int)nblocks;

    if (nenc <= 1) {
        /* 小表 / 单核：直接串行编码落盘（不起线程） */
        for (uint64_t k = 0; k < nblocks && ok; k++) {
            uint64_t lo = lo_row + k * per, hi = lo + per;
            if (hi > hi_row) hi = hi_row;
            sbuf_t s;
            if (sbuf_init(&s, SNAP_BLK_BYTES + 4096) != 0) { ok = 0; break; }
            for (uint64_t r = lo; r < hi && ok; r++)
                for (int i = 0; i < t->col_count; i++)
                    if (enc_value(&s, &t->rows[r].vals[i]) != 0) { ok = 0; break; }
            if (ok && s.len && fwrite(s.p, 1, s.len, f) != s.len) ok = 0;
            sbuf_free(&s);
        }
        return ok;
    }

    snap_pipe_t pp;
    memset(&pp, 0, sizeof(pp));
    pthread_mutex_init(&pp.mtx, NULL);
    pthread_cond_init(&pp.cv, NULL);
    pp.t = t;
    pp.rows_per_blk = per;
    pp.row_lo = lo_row;
    pp.row_count = hi_row;
    pp.nblocks = nblocks;
    pthread_t th[SNAP_MAX_ENC];
    int started = 0;
    for (int i = 0; i < nenc; i++) {
        if (pthread_create(&th[i], NULL, snap_encoder, &pp) != 0) break;
        started++;
    }
    if (started == 0) {
        /* 线程创建失败 → 退回串行 */
        pthread_mutex_destroy(&pp.mtx);
        pthread_cond_destroy(&pp.cv);
        for (uint64_t k = 0; k < nblocks && ok; k++) {
            uint64_t lo = lo_row + k * per, hi = lo + per;
            if (hi > hi_row) hi = hi_row;
            sbuf_t s;
            if (sbuf_init(&s, SNAP_BLK_BYTES + 4096) != 0) { ok = 0; break; }
            for (uint64_t r = lo; r < hi && ok; r++)
                for (int i = 0; i < t->col_count; i++)
                    if (enc_value(&s, &t->rows[r].vals[i]) != 0) { ok = 0; break; }
            if (ok && s.len && fwrite(s.p, 1, s.len, f) != s.len) ok = 0;
            sbuf_free(&s);
        }
        return ok;
    }
    /* 写线程：严格按块序消费（保证行序不变） */
    for (uint64_t k = 0; k < nblocks && ok; k++) {
        uint32_t slot = (uint32_t)(k % SNAP_SLOTS);
        pthread_mutex_lock(&pp.mtx);
        while (!pp.ready[slot] && !pp.stop)
            pthread_cond_wait(&pp.cv, &pp.mtx);
        if (pp.stop) { pthread_mutex_unlock(&pp.mtx); ok = 0; break; }
        uint8_t* p = pp.buf[slot];
        uint32_t n = pp.len[slot];
        pp.buf[slot] = NULL;
        pp.ready[slot] = 0;
        pp.write_idx = k + 1;
        pthread_cond_broadcast(&pp.cv);
        pthread_mutex_unlock(&pp.mtx);
        if (n && fwrite(p, 1, n, f) != n) ok = 0;
        free(p);
    }
    /* 收尾：通知停止并回收所有线程与残留缓冲 */
    pthread_mutex_lock(&pp.mtx);
    pp.stop = 1;
    pthread_cond_broadcast(&pp.cv);
    pthread_mutex_unlock(&pp.mtx);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < SNAP_SLOTS; i++) { free(pp.buf[i]); pp.buf[i] = NULL; }
    pthread_mutex_destroy(&pp.mtx);
    pthread_cond_destroy(&pp.cv);
    return ok;
}

/* 能否只追加新行：快照文件存在、结构世代未变、已有行从未被改/删、纯追加、帧数未超上限 */
static int snap_can_append(engine_t* e, table_t* t, const char* path) {
    if (t->snap_rows == 0 || t->snap_mutated) return 0;
    if (t->snap_epoch != t->epoch) return 0;
    if (t->row_count < t->snap_rows) return 0;
    if (t->snap_frames >= SNAP_MAX_FRAMES) return 0;
    struct stat st;
    if (stat(path, &st) != 0 || (uint64_t)st.st_size == 0) return 0;
    (void)e;
    return 1;
}

static int tbl_append_snapshot(engine_t* e, table_t* t, const char* path) {
    uint64_t nrows = t->row_count - t->snap_rows;
    if (nrows == 0) { t->dirty = 0; return 0; } /* 无新增行：无需写盘 */
    FILE* f = fopen(path, "ab");
    if (!f) return -1;
    uint8_t hdr[SNAP_FRAME_HDR];
    uint64_t lsn = e->lsn, na = (uint64_t)t->next_auto;
    for (int i = 0; i < 8; i++) {
        hdr[i] = (uint8_t)(lsn >> (8 * i));
        hdr[8 + i] = (uint8_t)(nrows >> (8 * i));
        hdr[16 + i] = (uint8_t)(na >> (8 * i));
    }
    int ok = (fwrite(hdr, 1, SNAP_FRAME_HDR, f) == SNAP_FRAME_HDR);
    if (ok) ok = enc_rows_stream(t, t->snap_rows, t->row_count, f);
    if (ok) ok = (fflush(f) == 0);
    if (ok) ok = (fsync(fileno(f)) == 0);
    fclose(f);
    /* 中途失败：帧可能只写了一半，交由加载端按「尾部残缺帧」回滚截断，
       内存状态保持不变（下次压实整表重写） */
    if (!ok) return -1;
    t->snap_lsn = lsn;
    t->snap_rows = t->row_count;
    t->snap_frames++;
    t->snap_mutated = 0;
    t->dirty = 0;
    return 0;
}

static int tbl_save_snapshot(engine_t* e, table_t* t) {
    /* 上限覆盖最坏情况 root(1023)+"/"+db(63)+"/"+tbl(63)+".nlt"，防 snprintf 截断写错文件 */
    char path[1170], tmp[1180];
    snprintf(path, sizeof(path), "%s/%s/%s.nlt", e->root, e->cur_db->name, t->name);
    /* 只追加新行：把 [snap_rows, row_count) 作为增量帧写入，旧行一个字节都不重写 */
    if (snap_can_append(e, t, path) && tbl_append_snapshot(e, t, path) == 0) return 0;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    sbuf_t b;
    if (sbuf_init(&b, 4096) != 0) { fclose(f); return -1; }
    sbuf_append(&b, NLT_MAGIC, 4);
    sbuf_append_u64(&b, e->lsn); /* snap_lsn：本表已包含到的 journal 序号（压实期间锁内无新写入，即当前尾） */
    sbuf_append_u32(&b, (uint32_t)t->col_count);
    for (int i = 0; i < t->col_count; i++) {
        tbl_col_t* c = &t->cols[i];
        jr_put_str(&b, c->name);
        sbuf_append_u8(&b, (uint8_t)c->type);
        sbuf_append_u32(&b, c->vlen);
        uint8_t flags = (uint8_t)((c->pk ? 1 : 0) | (c->auto_inc ? 2 : 0) | (c->not_null ? 4 : 0)
                                  | (c->has_def ? 8 : 0));
        sbuf_append_u8(&b, flags);
        if (c->has_def) enc_value(&b, &c->def);
    }
    sbuf_append_u64(&b, t->row_count);
    sbuf_append_u64(&b, (uint64_t)t->next_auto);
    if (b.len && fwrite(b.p, 1, b.len, f) != b.len) { sbuf_free(&b); fclose(f); unlink(tmp); return -1; }
    sbuf_free(&b);

    int ok = enc_rows_stream(t, 0, t->row_count, f);
    if (ok) ok = (fflush(f) == 0);
    if (ok) ok = (fsync(fileno(f)) == 0);
    fclose(f);
    if (!ok) { unlink(tmp); return -1; }
    if (nlsql_rename_replace(tmp, path) != 0) { unlink(tmp); return -1; }
    t->snap_rows = t->row_count;
    t->snap_epoch = t->epoch;
    t->snap_frames = 0;
    t->snap_mutated = 0;
    t->dirty = 0;
    return 0;
}

static void free_row_values(row_t* r, int ncols) {
    if (!r->vals) return;
    for (int i = 0; i < ncols; i++) free(r->vals[i].s);
    /* vals 数组由表级 arena 统一管理，不逐行释放 */
    r->vals = NULL;
}

static void table_free(table_t* t) {
    if (t->rows)
        for (uint64_t r = 0; r < t->row_count; r++) {
            nlsql_value_t* vals = t->rows[r].vals; /* free_row_values 会置 NULL，先留引用 */
            free_row_values(&t->rows[r], t->col_count);
            if (t->heap_rows) free(vals); /* 快照表 vals 逐行 malloc */
        }
    free(t->rows);
    nlsql_tbl_val_arena_free(t); /* 行值块整块回收 */
    for (int i = 0; i < t->col_count; i++) free(t->cols[i].def.s);
    free(t->cols);
    nlsql_idx_free_all(t);
    nlsql_pk_free(t);
    free(t);
}

static int tbl_load_snapshot(engine_t* e, const char* db, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 8 || (uint64_t)sz > 4ull * 1024ull * 1024ull * 1024ull) { fclose(f); return -1; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return -1; }
    if (memcmp(buf, NLT_MAGIC, 4) != 0) { free(buf); return -1; }

    table_t* t = (table_t*)calloc(1, sizeof(table_t));
    if (!t) { free(buf); return -1; }
    const uint8_t* p = buf + 4;
    uint32_t avail = (uint32_t)sz - 4;
    if (avail < 12) { free(buf); free(t); return -1; }
    memcpy(&t->snap_lsn, p, 8); p += 8; avail -= 8;
    if (t->snap_lsn > e->lsn) e->lsn = t->snap_lsn; /* 恢复 LSN 计数器（快照可能比 journal 新） */
    uint32_t ncols;
    memcpy(&ncols, p, 4); p += 4; avail -= 4;
    if (ncols == 0 || ncols > NLSQL_MAX_COLUMNS) { free(buf); free(t); return -1; }
    t->col_count = (int)ncols;
    t->cols = (tbl_col_t*)calloc(ncols, sizeof(tbl_col_t));
    if (!t->cols) { free(buf); free(t); return -1; }
    for (uint32_t i = 0; i < ncols; i++) {
        uint64_t n;
        uint32_t u = nl_read_lenenc(p, avail, &n);
        if (!u || n > avail - u || n > NLSQL_MAX_IDENT) goto bad;
        memcpy(t->cols[i].name, p + u, n);
        p += u + n; avail -= u + (uint32_t)n;
        if (avail < 6) goto bad;
        t->cols[i].type = (nlsql_type_t)*p++;
        memcpy(&t->cols[i].vlen, p, 4); p += 4; avail -= 4;
        uint8_t flags = *p++; avail--;
        t->cols[i].pk = flags & 1;
        t->cols[i].auto_inc = flags & 2;
        t->cols[i].not_null = flags & 4;
        if (flags & 8) {
            t->cols[i].has_def = 1;
            const uint8_t* np = dec_value(p, avail, &t->cols[i].def);
            if (!np) goto bad;
            avail -= (uint32_t)(np - p);
            p = np;
        }
    }
    if (avail < 16) goto bad;
    memcpy(&t->row_count, p, 8); p += 8;
    memcpy(&t->next_auto, p, 8); p += 8; avail -= 16;
    t->rows = (row_t*)calloc(t->row_count ? (size_t)t->row_count : 1, sizeof(row_t));
    if (!t->rows) goto bad;
    t->row_cap = t->row_count ? t->row_count : 1; /* 快照预分配即容量，否则后续插入扩容计算出错 */
    for (uint64_t r = 0; r < t->row_count; r++) {
        t->rows[r].vals = nlsql_tbl_val_alloc(t, t->col_count);
        if (!t->rows[r].vals) goto bad;
        for (int i = 0; i < t->col_count; i++) {
            const uint8_t* np = dec_value(p, avail, &t->rows[r].vals[i]);
            if (!np) goto bad;
            avail -= (uint32_t)(np - p);
            p = np;
        }
    }

    /* ---- 增量帧：基础行区之后可跟若干「只含新增行」的帧 ---- */
    uint64_t snap_total = t->row_count;              /* 已入库行数（含基础区与已解析帧） */
    uint32_t frames = 0;
    uint64_t good_off = (uint64_t)(p - buf);         /* 最后一个完整段结束偏移 */
    while (avail >= SNAP_FRAME_HDR) {
        uint64_t flsn = 0, nrows = 0, na = 0;
        for (int i = 0; i < 8; i++) {
            flsn |= ((uint64_t)p[i]) << (8 * i);
            nrows |= ((uint64_t)p[8 + i]) << (8 * i);
            na |= ((uint64_t)p[16 + i]) << (8 * i);
        }
        /* 每行至少 col_count 个字节（每值一个标签）：nrows 超过剩余字节即损坏帧，
           先拦住，避免按损坏行数做巨量扩容 */
        if (t->col_count <= 0 || nrows > (avail - SNAP_FRAME_HDR) / (uint64_t)t->col_count) break;
        const uint8_t* q = p + SNAP_FRAME_HDR;
        uint32_t qav = avail - SNAP_FRAME_HDR;
        uint64_t before = t->row_count;
        int bad = 0;
        for (uint64_t r = 0; r < nrows && !bad; r++) {
            if (t->row_count >= t->row_cap) {
                uint64_t nc = t->row_cap ? t->row_cap * 2 : 64;
                row_t* nr = (row_t*)realloc(t->rows, (size_t)nc * sizeof(row_t));
                if (!nr) { bad = 2; break; }
                memset(nr + t->row_cap, 0, (size_t)(nc - t->row_cap) * sizeof(row_t));
                t->rows = nr; t->row_cap = nc;
            }
            row_t* rr = &t->rows[t->row_count];
            rr->vals = nlsql_tbl_val_alloc(t, t->col_count);
            if (!rr->vals) { bad = 2; break; }
            t->row_count++;
            for (int i = 0; i < t->col_count; i++) {
                const uint8_t* nq = dec_value(q, qav, &rr->vals[i]);
                if (!nq) { bad = 1; break; }
                qav -= (uint32_t)(nq - q);
                q = nq;
            }
        }
        if (bad) {
            if (bad == 2) goto bad;
            /* 残缺帧：撤掉本帧已加的行，恢复到帧前状态 */
            for (uint64_t r = before; r < t->row_count; r++)
                free_row_values(&t->rows[r], t->col_count);
            t->row_count = before;
            break;
        }
        p = q; avail = qav;
        good_off = (uint64_t)(p - buf);
        frames++;
        snap_total = t->row_count;
        if (flsn > t->snap_lsn) t->snap_lsn = flsn;
        if (flsn > e->lsn) e->lsn = flsn;
        t->next_auto = (int64_t)na;
    }
    /* 尾部有残缺帧 / 半截帧头 → 截断到最后一个完整段，
       否则下次追加会接在垃圾后面，形成永久的解析断点（只读连接截断失败无妨：它不追加） */
    if (avail > 0 && truncate(path, (off_t)good_off) != 0) {
        /* 只读连接或权限不足：截断失败不影响本次内存加载（只读连接不会追加） */
    }
    t->snap_rows = snap_total;
    t->snap_epoch = t->epoch;
    t->snap_frames = frames;
    t->snap_mutated = 0;
    t->dirty = 0;
    (void)db;
    /* 表名取文件名 */
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char* dot = strrchr(base, '.');
    size_t nlen = dot ? (size_t)(dot - base) : strlen(base);
    if (nlen > NLSQL_MAX_IDENT) nlen = NLSQL_MAX_IDENT;
    memcpy(t->name, base, nlen);
    t->name[nlen] = '\0';

    /* 挂链 */
    t->next = e->cur_db->tables;
    e->cur_db->tables = t;
    free(buf);
    return 0;
bad:
    table_free(t);
    free(buf);
    return -1;
}

/* =========================================
 * journal 重放
 * ========================================= */

static table_t* db_find_table(database_t* db, const char* name) {
    for (table_t* t = db->tables; t; t = t->next)
        if (strcmp(t->name, name) == 0) return t;
    return NULL;
}

static int replay_one(engine_t* e, const uint8_t* p, uint32_t avail) {
    if (avail < JR_HDR_LEN) return -1;
    uint8_t type = p[0];
    uint32_t plen = (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
    uint64_t rec_lsn = 0;
    for (int i = 0; i < 8; i++) rec_lsn |= ((uint64_t)p[5 + i]) << (8 * i);
    p += JR_HDR_LEN; avail -= JR_HDR_LEN;
    if (plen > avail) return -1;
    const uint8_t* end = p + plen;
    database_t* db = e->cur_db;
    if (!db) return -1;
    if (rec_lsn > e->lsn) e->lsn = rec_lsn;

    /* 快照已包含此记录（压实中途崩溃：新快照表 + 未清 journal）→ 跳过，防重复执行 */
    #define SNAP_HAS(name) do { \
        table_t* _ex = db_find_table(db, name); \
        if (_ex && rec_lsn <= _ex->snap_lsn) goto snap_done; \
    } while (0)

    switch (type) {
        case JR_CREATE_TBL: {
            uint64_t n; uint32_t u = nl_read_lenenc(p, avail, &n);
            if (!u || n > NLSQL_MAX_IDENT || n > avail - u) return -1;
            char name[NLSQL_MAX_IDENT + 1];
            memcpy(name, p + u, n); name[n] = '\0';
            SNAP_HAS(name); /* 同名表已在新快照中 → 本 CREATE 已体现 */
            p += u + n; avail -= u + (uint32_t)n;
            if (avail < 2) return -1;
            uint16_t ncols;
            memcpy(&ncols, p, 2); p += 2; avail -= 2;
            if (!ncols || ncols > NLSQL_MAX_COLUMNS) return -1;
            table_t* t = (table_t*)calloc(1, sizeof(table_t));
            if (!t) return -1;
            snprintf(t->name, sizeof(t->name), "%s", name);
            t->col_count = ncols;
            t->cols = (tbl_col_t*)calloc(ncols, sizeof(tbl_col_t));
            if (!t->cols) { free(t); return -1; }
            for (int i = 0; i < ncols; i++) {
                uint32_t u2 = nl_read_lenenc(p, avail, &n);
                if (!u2 || n > NLSQL_MAX_IDENT || n > avail - u2) { table_free(t); return -1; }
                memcpy(t->cols[i].name, p + u2, n);
                p += u2 + n; avail -= u2 + (uint32_t)n;
                if (avail < 6) { table_free(t); return -1; }
                t->cols[i].type = (nlsql_type_t)*p++;
                memcpy(&t->cols[i].vlen, p, 4); p += 4; avail -= 4;
                uint8_t flags = *p++; avail--;
                t->cols[i].pk = flags & 1;
                t->cols[i].auto_inc = flags & 2;
                t->cols[i].not_null = flags & 4;
                if (flags & 8) {
                    const uint8_t* np = dec_value(p, avail, &t->cols[i].def);
                    if (!np) { table_free(t); return -1; }
                    t->cols[i].has_def = 1;
                    avail -= (uint32_t)(np - p);
                    p = np;
                }
            }
            t->next = db->tables;
            db->tables = t;
            break;
        }
        case JR_DROP_TBL: {
            uint64_t n; uint32_t u = nl_read_lenenc(p, avail, &n);
            if (!u || n > NLSQL_MAX_IDENT || n > avail - u) return -1;
            char name[NLSQL_MAX_IDENT + 1];
            memcpy(name, p + u, n); name[n] = '\0';
            SNAP_HAS(name);
            table_t** pp = &db->tables;
            while (*pp) {
                if (strcmp((*pp)->name, name) == 0) {
                    table_t* dead = *pp;
                    *pp = dead->next;
                    table_free(dead);
                    break;
                }
                pp = &(*pp)->next;
            }
            break;
        }
        case JR_INSERT: {
            uint64_t n; uint32_t u = nl_read_lenenc(p, avail, &n);
            if (!u || n > NLSQL_MAX_IDENT || n > avail - u) return -1;
            char name[NLSQL_MAX_IDENT + 1];
            memcpy(name, p + u, n); name[n] = '\0';
            SNAP_HAS(name);
            p += u + n; avail -= u + (uint32_t)n;
            table_t* t = db_find_table(db, name);
            if (!t) return -1;
            while (t->row_count >= t->row_cap) { /* while：row_cap 异常低估时也能扩到足够 */
                uint64_t nc = t->row_cap ? t->row_cap * 2 : 64;
                row_t* nr = (row_t*)realloc(t->rows, (size_t)nc * sizeof(row_t));
                if (!nr) return -1;
                t->rows = nr; t->row_cap = nc;
            }
            row_t* r = &t->rows[t->row_count];
            r->vals = nlsql_tbl_val_alloc(t, t->col_count);
            if (!r->vals) return -1;
            for (int i = 0; i < t->col_count; i++) {
                const uint8_t* np = dec_value(p, (uint32_t)(end - p), &r->vals[i]);
                if (!np) { free_row_values(r, t->col_count); return -1; }
                p = np;
            }
            t->row_count++;
            break;
        }
        case JR_UPDATE: {
            uint64_t n; uint32_t u = nl_read_lenenc(p, avail, &n);
            if (!u || n > NLSQL_MAX_IDENT || n > avail - u) return -1;
            char name[NLSQL_MAX_IDENT + 1];
            memcpy(name, p + u, n); name[n] = '\0';
            SNAP_HAS(name);
            p += u + n; avail -= u + (uint32_t)n;
            table_t* t = db_find_table(db, name);
            if (!t) return -1;
            if ((uint32_t)(end - p) < 8) return -1;
            uint64_t idx = 0;
            for (int i = 0; i < 8; i++) idx |= ((uint64_t)p[i]) << (8 * i);
            p += 8;
            if (idx >= t->row_count) return -1;
            row_t* r = &t->rows[idx];
            nlsql_value_t* ov = r->vals;
            free_row_values(r, t->col_count);
            nlsql_tbl_val_recycle(t, ov);          /* 旧 vals 回收复用 */
            r->vals = nlsql_tbl_val_alloc(t, t->col_count);
            if (!r->vals) return -1;
            for (int i = 0; i < t->col_count; i++) {
                const uint8_t* np = dec_value(p, (uint32_t)(end - p), &r->vals[i]);
                if (!np) { free_row_values(r, t->col_count); return -1; }
                p = np;
            }
            nlsql_tbl_mark_mutated(t); /* 重放了改行 → 增量帧不可用 */
            break;
        }
        case JR_DELETE: {
            uint64_t n; uint32_t u = nl_read_lenenc(p, avail, &n);
            if (!u || n > NLSQL_MAX_IDENT || n > avail - u) return -1;
            char name[NLSQL_MAX_IDENT + 1];
            memcpy(name, p + u, n); name[n] = '\0';
            SNAP_HAS(name);
            p += u + n; avail -= u + (uint32_t)n;
            table_t* t = db_find_table(db, name);
            if (!t) return -1;
            if ((uint32_t)(end - p) < 8) return -1;
            uint64_t idx = 0;
            for (int i = 0; i < 8; i++) idx |= ((uint64_t)p[i]) << (8 * i);
            if (idx >= t->row_count) return -1;
            {
                nlsql_value_t* dv = t->rows[idx].vals;
                free_row_values(&t->rows[idx], t->col_count);
                nlsql_tbl_val_recycle(t, dv); /* 字符串释放后，行值归还空闲链 */
            }
            memmove(&t->rows[idx], &t->rows[idx + 1],
                    (size_t)(t->row_count - idx - 1) * sizeof(row_t));
            t->row_count--;
            nlsql_tbl_mark_mutated(t); /* 重放了删行 → 增量帧不可用 */
            break;
        }
        case JR_TRUNCATE: {
            uint64_t n; uint32_t u = nl_read_lenenc(p, avail, &n);
            if (!u || n > NLSQL_MAX_IDENT || n > avail - u) return -1;
            char name[NLSQL_MAX_IDENT + 1];
            memcpy(name, p + u, n); name[n] = '\0';
            SNAP_HAS(name);
            table_t* t = db_find_table(db, name);
            if (!t) return -1;
            for (uint64_t r = 0; r < t->row_count; r++)
                free_row_values(&t->rows[r], t->col_count);
            t->row_count = 0;
            t->next_auto = 1;
            nlsql_tbl_mark_mutated(t); /* 重放了截断 → 增量帧不可用 */
            break;
        }
        case JR_AUTOINC: {
            uint64_t n; uint32_t u = nl_read_lenenc(p, avail, &n);
            if (!u || n > NLSQL_MAX_IDENT || n > avail - u) return -1;
            char name[NLSQL_MAX_IDENT + 1];
            memcpy(name, p + u, n); name[n] = '\0';
            SNAP_HAS(name);
            p += u + n; avail -= u + (uint32_t)n;
            table_t* t = db_find_table(db, name);
            if (!t || (uint32_t)(end - p) < 8) return -1;
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (8 * i);
            t->next_auto = (int64_t)v;
            break;
        }
        default:
            return -1; /* 未知类型：视为日志损坏 */
    }
snap_done: /* 快照已包含 → 直接通过（NOSONAR：控制流跳转） */
    return 0;
}

static int db_replay_journal(engine_t* e, const char* dbdir) {
    FILE* f = fopen(e->jr ? e->jr->path : "", "rb");
    /* journal 已由 jr_open 打开为 fd，这里换一种读取方式 */
    if (f) fclose(f);
    journal_t* j = e->jr;
    if (!j || j->fd < 0) return -1;
    off_t sz = lseek(j->fd, 0, SEEK_END);
    if (sz < 0) return -1;
    if (lseek(j->fd, 0, SEEK_SET) < 0) return -1;
    if (sz == 0) return 0;
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) return -1;
    size_t rd = 0;
    while (rd < (size_t)sz) {
        ssize_t w = read(j->fd, buf + rd, (size_t)sz - rd);
        if (w <= 0) break;
        rd += (size_t)w;
    }
    size_t off = sizeof(JRL_MAGIC) - 1;
    if (rd < off || memcmp(buf, JRL_MAGIC, off) != 0) {
        free(buf);
        /* 坏头（非空）：截断重建，否则其后所有追加记录永不被重放（WAL 静默全丢）。
           只读连接不动写者的文件。 */
        if (!e->readonly && (size_t)sz > 0) {
            if (ftruncate(j->fd, 0) == 0 && lseek(j->fd, 0, SEEK_SET) == 0) {
                ssize_t w = write(j->fd, JRL_MAGIC, sizeof(JRL_MAGIC) - 1);
                j->size = (w > 0) ? (uint64_t)w : 0;
            }
        }
        return 0; /* 空日志/坏头：当作空处理 */
    }
    int failed = 0;
    while (off + JR_HDR_LEN <= rd) {
        uint32_t plen = (uint32_t)buf[off + 1] | ((uint32_t)buf[off + 2] << 8) |
                        ((uint32_t)buf[off + 3] << 16) | ((uint32_t)buf[off + 4] << 24);
        if (off + JR_HDR_LEN + plen > rd) break; /* 半条日志：截断重放 */
        if (replay_one(e, buf + off, JR_HDR_LEN + plen) != 0) { failed = 1; break; } /* 坏条目：停在此 */
        off += JR_HDR_LEN + plen;
    }
    free(buf);
    /* 截掉未重放部分，保证后续追加从干净处开始（只读连接禁截断） */
    if (off < (size_t)sz && !e->readonly) {
        if (ftruncate(j->fd, (off_t)off) == 0)
            j->size = off;
    }
    /* 重放中途失败：内存已被污染（如 UPDATE 半途行值置空）——
       丢弃表链，从干净的快照重新加载，保证后续 SELECT 不解引用坏行 */
    if (failed && e->cur_db) {
        table_t* t = e->cur_db->tables;
        while (t) {
            table_t* nx = t->next;
            table_free(t);
            t = nx;
        }
        e->cur_db->tables = NULL;
        DIR* d = opendir(dbdir);
        if (d) {
            struct dirent* de;
            while ((de = readdir(d)) != NULL) {
                size_t n = strlen(de->d_name);
                if (n > 4 && strcmp(de->d_name + n - 4, ".nlt") == 0 && n <= NLSQL_MAX_IDENT + 4) {
                    char path[1360]; /* 覆盖 dbdir(1099)+"/"+d_name(255) 最坏情况 */
                    snprintf(path, sizeof(path), "%s/%s", dbdir, de->d_name);
                    tbl_load_snapshot(e, e->cur_db->name, path);
                }
            }
            closedir(d);
        }
    }
    return 0;
}

/* =========================================
 * 数据库目录加载
 * ========================================= */

static int load_db_tables(engine_t* e, const char* dbname) {
    char dbdir[1100];
    snprintf(dbdir, sizeof(dbdir), "%s/%s", e->root, dbname);
    database_t* db = (database_t*)calloc(1, sizeof(database_t));
    if (!db) return -1;
    snprintf(db->name, sizeof(db->name), "%s", dbname);
    db->next = e->dbs;
    e->dbs = db;
    e->cur_db = db;   /* 正在加载的库同时也是后续重放的目标库 */

    /* 1) 快照 */
    DIR* d = opendir(dbdir);
    if (d) {
        struct dirent* de;
        while ((de = readdir(d)) != NULL) {
            size_t n = strlen(de->d_name);
            if (n > 4 && strcmp(de->d_name + n - 4, ".nlt") == 0 && n <= NLSQL_MAX_IDENT + 4) {
                char path[1360]; /* 覆盖 dbdir(1099)+"/"+d_name(255) 最坏情况 */
                snprintf(path, sizeof(path), "%s/%s", dbdir, de->d_name);
                tbl_load_snapshot(e, dbname, path);
            }
        }
        closedir(d);
    }
    /* 2) WAL 重放（切库前先落盘并关闭旧库写缓冲） */
    if (nlsql_jr_flush(e) != NLSQL_OK) return -1;
    if (e->jr) { jr_close(e->jr); e->jr = NULL; }
    if (jr_open(&e->jr, dbdir, e->readonly) != 0) return -1;
    db_replay_journal(e, dbdir);
    return 0;
}

/* =========================================
 * eng_open / eng_close / eng_flush
 * ========================================= */

int eng_open(engine_t* e, const char* data_dir, uint32_t journal_mb, int readonly) {
    memset(e, 0, sizeof(*e));
    e->lock_fd = -1; /* 默认未持锁：只读路径（不加锁）close 时不得 close(0) 关掉 stdin */
    char clean[512];
    if (nlsql_sanitize_dir(data_dir, clean, sizeof(clean)) != 0)
        return NLSQL_ERR_PATH;
    if (nlsql_mkdir_p(clean) != 0)
        return NLSQL_ERR_IO;
    /* 转绝对路径（长度护栏：cwd+clean 超 root 容量直接拒绝，
       否则 snprintf 截断后所有读写落错目录）。Windows 盘符路径本身即绝对。 */
    if (!nlsql_path_is_abs(clean)) {
        char cwd[600];
        if (!getcwd(cwd, sizeof(cwd))) return NLSQL_ERR_IO;
        nlsql_path_slashify(cwd);
        size_t cl = strlen(cwd), dl = strlen(clean);
        if (cl + 1 + dl >= sizeof(e->root)) return NLSQL_ERR_PATH;
        memcpy(e->root, cwd, cl);
        e->root[cl] = '/';
        memcpy(e->root + cl + 1, clean, dl + 1);
    } else {
        snprintf(e->root, sizeof(e->root), "%s", clean);
    }
    e->readonly = readonly;
    e->journal_limit = (journal_mb ? journal_mb : 64) * 1024ull * 1024ull;

    if (!readonly) {
        int lr = nlsql_lock_dir(e->root, e->lock_path, sizeof(e->lock_path));
        if (lr == -2) return NLSQL_ERR_LOCKED;
        if (lr < 0) return NLSQL_ERR_IO;
        e->lock_fd = lr;
    }

    /* 扫描子目录 = 数据库列表，但不预加载全部（按 USE 懒加载） */
    return NLSQL_OK;
}

static void db_free_all(database_t* db) {
    table_t* t = db->tables;
    while (t) {
        table_t* nx = t->next;
        table_free(t);
        t = nx;
    }
    free(db);
}

/* 统计当前库快照文件总字节：journal 阈值据此自适应放大，
   1GB 级库不再被固定 64MB 阈值逼出频繁全量重写（写放大从 ~10x 降到 ~5x） */
static uint64_t calc_snap_bytes(engine_t* e) {
    if (!e->cur_db) return e->snap_bytes;
    char dbdir[1100];
    snprintf(dbdir, sizeof(dbdir), "%s/%s", e->root, e->cur_db->name);
    DIR* d = opendir(dbdir);
    if (!d) return e->snap_bytes;
    uint64_t sum = 0;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name);
        if (n < 4 || strcmp(de->d_name + n - 4, ".nlt") != 0) continue;
        char p[1400];
        snprintf(p, sizeof(p), "%s/%s", dbdir, de->d_name);
        struct stat st;
        if (stat(p, &st) == 0) sum += (uint64_t)st.st_size;
    }
    closedir(d);
    e->snap_bytes = sum;
    return sum;
}

int eng_flush(engine_t* e) {
    if (!e->cur_db) return NLSQL_OK;
    if (e->readonly) return NLSQL_OK;
    int rc = NLSQL_OK;
    if (nlsql_jr_flush(e) != NLSQL_OK) rc = NLSQL_ERR_IO; /* 先落盘写缓冲（WAL 语句尾） */
    for (table_t* t = e->cur_db->tables; t; t = t->next) {
        if (t->dirty) {
            if (tbl_save_snapshot(e, t) != 0) rc = NLSQL_ERR_IO;
        }
    }
    /* 仅当快照全部落盘成功才清空 journal——快照写失败（ENOSPC 等）时保留 WAL 供恢复重放 */
    if (rc == NLSQL_OK && e->jr && e->jr->fd >= 0) {
        if (ftruncate(e->jr->fd, 0) != 0) rc = NLSQL_ERR_IO;
        lseek(e->jr->fd, 0, SEEK_SET);
        ssize_t w = write(e->jr->fd, JRL_MAGIC, sizeof(JRL_MAGIC) - 1);
        if (w > 0) e->jr->size = (uint64_t)w;
        calc_snap_bytes(e); /* 刷新快照总字节 → 下次 journal 阈值自适应 */
    }
    return rc;
}

void eng_close(engine_t* e) {
    /* 关闭不做全量压实：只把 WAL 写缓冲 write() 进文件（不 fsync），
       数据完整性由「快照 + 尾部 WAL 重放」保证（进程崩溃安全；掉电可能丢未 fsync 尾部）。
       压实是性能优化（控制 WAL 体积），不是持久化前提 —— 一次 UPDATE 触发整表 512MB 重写
       纯属冗余 IO，改为留给下次打开时按 LSN 精确重放/后台压实。显式 nlsql_flush() 仍可强制全量压实。 */
    nlsql_jr_flush(e); /* 只读路径兜底（正常为空） */
    if (e->jr) { jr_close(e->jr); e->jr = NULL; }
    free(e->jbuf); e->jbuf = NULL; e->jlen = 0; e->jcap = 0;
    /* 释放所有已加载数据库（从链表头释放，cur_db 可能只是链中一个节点） */
    database_t* db = e->dbs;
    while (db) {
        database_t* nx = db->next;
        db_free_all(db);
        db = nx;
    }
    e->dbs = NULL;
    e->cur_db = NULL;
    if (e->lock_fd >= 0) {
        nlsql_unlock_dir(e->lock_fd, e->lock_path);
        e->lock_fd = -1;
    }
}

/* =========================================
 * 存储辅助（供引擎调用）
 * ========================================= */

int nlsql_db_load(engine_t* e, const char* dbname) {
    char dbdir[1100];
    snprintf(dbdir, sizeof(dbdir), "%s/%s", e->root, dbname);
    /* 已加载？只切 cur_db 并换该库的 journal —— 绝不能再从磁盘重载一次：
       重载会产生同名库的第二份对象（旧对象从链上失联泄漏），且旧表指针/索引全部失效 */
    for (database_t* d = e->dbs; d; d = d->next)
        if (strcmp(d->name, dbname) == 0) {
            if (e->cur_db == d) return NLSQL_OK; /* 已在当前库：无 journal 需要切换 */
            if (nlsql_jr_flush(e) != NLSQL_OK) return NLSQL_ERR_IO;
            if (e->jr) { jr_close(e->jr); e->jr = NULL; }
            if (jr_open(&e->jr, dbdir, e->readonly) != 0) return NLSQL_ERR_IO;
            e->cur_db = d;
            return NLSQL_OK;
        }
    struct stat st;
    if (stat(dbdir, &st) != 0 || !S_ISDIR(st.st_mode))
        return NLSQL_ER_NO_SUCH_DB;
    database_t* before = e->dbs;
    if (load_db_tables(e, dbname) != 0) {
        /* 半加载回滚：新库已挂链则摘除并释放，cur_db 退回原链头 */
        if (e->dbs != before) {
            database_t* nd = e->dbs;
            e->dbs = nd->next;
            if (e->cur_db == nd) e->cur_db = e->dbs;
            db_free_all(nd);
        }
        return NLSQL_ERR_IO;
    }
    return NLSQL_OK;
}

int nlsql_db_create(engine_t* e, const char* dbname) {
    if (!nlsql_valid_ident(dbname)) return NLSQL_ERR_PATH;
    char dbdir[1100];
    snprintf(dbdir, sizeof(dbdir), "%s/%s", e->root, dbname);
    if (mkdir(dbdir, 0755) != 0 && errno != EEXIST) return NLSQL_ERR_IO;
    return NLSQL_OK;
}

int nlsql_db_drop(engine_t* e, const char* dbname) {
    if (!nlsql_valid_ident(dbname)) return NLSQL_ERR_PATH;
    /* 先确认库存在，再动 journal：库不存在时提前返回，不能关闭当前库 journal，
       否则后续写语句全部报 "journal write failed"（DROP DATABASE IF EXISTS 会走到这里）。 */
    char dbdir[1100];
    snprintf(dbdir, sizeof(dbdir), "%s/%s", e->root, dbname);
    DIR* d = opendir(dbdir);
    if (!d) return NLSQL_ER_NO_SUCH_DB;
    nlsql_jr_flush(e); /* 删除前落盘缓冲 */
    /* 库目录即将删除：先关该库 journal，避免后续语句写入已 unlink 的幽灵 fd */
    if (e->jr) { jr_close(e->jr); e->jr = NULL; }
    struct dirent* de;
    int rc = NLSQL_OK;
    char paths[256][1360]; /* d_name 可达 255：dbdir(1099)+"/"+255 防截断漏删 */
    int np = 0;
    while ((de = readdir(d)) != NULL && np < 256) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        snprintf(paths[np++], sizeof(paths[0]), "%s/%s", dbdir, de->d_name);
    }
    closedir(d);
    for (int i = 0; i < np; i++)
        if (unlink(paths[i]) != 0) rc = NLSQL_ERR_IO;
    if (rmdir(dbdir) != 0) rc = NLSQL_ERR_IO;
    /* 从内存链摘除 */
    database_t** pp = &e->dbs;
    while (*pp) {
        if (strcmp((*pp)->name, dbname) == 0) {
            database_t* dead = *pp;
            *pp = dead->next;
            if (e->cur_db == dead) e->cur_db = e->dbs;
            db_free_all(dead);
            break;
        }
        pp = &(*pp)->next;
    }
    /* 摘除后链头可能成为新的当前库：为它重开 journal（否则其写入全部失败） */
    if (e->cur_db && !e->readonly) {
        char nd[1100];
        snprintf(nd, sizeof(nd), "%s/%s", e->root, e->cur_db->name);
        jr_open(&e->jr, nd, 0);
    }
    return rc;
}

int nlsql_jr_append(engine_t* e, uint8_t type, const void* payload, uint32_t plen) {
    return jr_append(e, type, payload, plen) == 0 ? NLSQL_OK : NLSQL_ERR_IO;
}

void nlsql_jr_put_str(sbuf_t* b, const char* s) { jr_put_str(b, s); }
int nlsql_sbuf_enc_value(sbuf_t* b, const nlsql_value_t* v) { return enc_value(b, v); }

uint64_t nlsql_journal_size(engine_t* e) { return (e->jr ? e->jr->size : 0) + e->jlen; }
uint64_t nlsql_journal_limit(engine_t* e) {
    /* 自适应阈值：压实触发点取「journal 未压实量 > 已有快照体积」（limit=(S+J)/2），
       即 LSM 式倍增——总写入 N 字节只需 O(log N) 次全量重写，写放大从 ~10x 降到 ~2-3x；
       下限仍是用户配置值，小库不会退化成频繁压实 */
    uint64_t adaptive = (e->snap_bytes + nlsql_journal_size(e)) / 2;
    return adaptive > e->journal_limit ? adaptive : e->journal_limit;
}

table_t* nlsql_tbl_find(engine_t* e, const char* name) {
    return e->cur_db ? db_find_table(e->cur_db, name) : NULL;
}

/* 行值 arena：每块 [块头 | nlsql_value_t...]，bump 分配。
   表内每行 vals 不再逐行 calloc（大表 = 数十万次小分配与碎片），块链随表释放/TRUNCATE 整体回收 */
typedef struct tbl_val_blk {
    struct tbl_val_blk* next;
    uint32_t used, cap; /* 字节 */
    char data[1];
} tbl_val_blk_t;

nlsql_value_t* nlsql_tbl_val_alloc(table_t* t, int nvals) {
    if (nvals <= 0) return NULL;
    size_t need = (size_t)nvals * sizeof(nlsql_value_t);
    /* 优先复用空闲链（DELETE/失败插入回收的行值；col_count 未变则尺寸恒等，
       ALTER 改列数时清链以维护该不变式） */
    if (t->vals_free) {
        nlsql_value_t* v = t->vals_free;
        t->vals_free = *(nlsql_value_t**)v; /* next 指针存于块首 8 字节 */
        memset(v, 0, need);
        return v;
    }
    tbl_val_blk_t* b = (tbl_val_blk_t*)t->val_arena;
    if (!b || b->used + need > b->cap) {
        /* 批量块：≥64 行的量，摊薄块头与系统调用 */
        size_t batch = need * 64;
        size_t blk = batch < 4096 ? 4096 : (batch > 0x10000 ? need + 4096 : batch);
        b = (tbl_val_blk_t*)malloc(sizeof(tbl_val_blk_t) + blk);
        if (!b) return NULL;
        b->cap = (uint32_t)blk; b->used = 0;
        b->next = (tbl_val_blk_t*)t->val_arena;
        t->val_arena = b;
    }
    nlsql_value_t* v = (nlsql_value_t*)(b->data + b->used);
    b->used += (uint32_t)need;
    memset(v, 0, need);
    return v;
}

void nlsql_tbl_val_arena_free(table_t* t) {
    tbl_val_blk_t* b = (tbl_val_blk_t*)t->val_arena;
    while (b) { tbl_val_blk_t* n = b->next; free(b); b = n; }
    t->val_arena = NULL;
    t->vals_free = NULL; /* 空闲链的块也来自 arena */
}

void nlsql_tbl_val_recycle(table_t* t, nlsql_value_t* v) {
    if (!v) return;
    *(nlsql_value_t**)v = t->vals_free; /* next 覆写块首（该 vals 不再被引用） */
    t->vals_free = v;
}

int nlsql_row_alloc(table_t* t, row_t** out) {
    while (t->row_count >= t->row_cap) { /* while：row_cap 异常低估时也能扩到足够 */
        uint64_t nc = t->row_cap ? t->row_cap * 2 : 64;
        row_t* nr = (row_t*)realloc(t->rows, (size_t)nc * sizeof(row_t));
        if (!nr) return -1;
        t->rows = nr; t->row_cap = nc;
    }
    row_t* r = &t->rows[t->row_count];
    memset(r, 0, sizeof(*r));
    r->vals = nlsql_tbl_val_alloc(t, t->col_count);
    if (!r->vals) return -1;
    *out = r;
    return 0;
}

void nlsql_tbl_mark_dirty(table_t* t) { t->dirty = 1; nlsql_idx_mark_dirty(t); }
/* 既有行被改/删 → 增量帧无法表达，下次压实必须整表重写 */
void nlsql_tbl_mark_mutated(table_t* t) { t->snap_mutated = 1; t->dirty = 1; nlsql_idx_mark_dirty(t); }
void nlsql_tbl_commit_row(table_t* t) { t->row_count++; }
void nlsql_tbl_set_auto(table_t* t, int64_t v) { t->next_auto = v; }
int64_t nlsql_tbl_get_auto(table_t* t) { return t->next_auto; }

/* =========================================
 * 主键哈希索引（开放寻址 + 墓碑；仅通用 CPU 指令）
 * pk_slots: 0=空 1=墓碑 否则 row_idx+2（避开 0/1 特殊值）
 * ========================================= */

#define NLSQL_PK_TOMB 1ULL

static uint64_t pk_mix(uint64_t x) { /* splitmix64 */
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static uint64_t pk_hash(const nlsql_value_t* v) {
    if (v->type == NLSQL_TYPE_INT64) return pk_mix((uint64_t)v->i);
    if (v->type == NLSQL_TYPE_DOUBLE) {
        double d = v->d;
        if (d == 0) d = 0; /* -0 与 +0 等值 → 同哈希 */
        uint64_t bits;
        memcpy(&bits, &d, sizeof(bits));
        return pk_mix(bits);
    }
    uint64_t h = 14695981039346656037ULL; /* FNV-1a 64 */
    const uint8_t* p = (const uint8_t*)(v->s ? v->s : "");
    for (uint32_t i = 0; i < v->len; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return pk_mix(h);
}

static int pk_num_d(const nlsql_value_t* v, double* out) {
    if (v->type == NLSQL_TYPE_INT64) { *out = (double)v->i; return 1; }
    if (v->type == NLSQL_TYPE_DOUBLE) { *out = v->d; return 1; }
    return 0;
}

/* 等值判定（与 engine cmp_values 语义一致：数值宽松/字符串精确） */
static int pk_eq(const nlsql_value_t* a, const nlsql_value_t* b) {
    if (a->type == NLSQL_TYPE_INT64 && b->type == NLSQL_TYPE_INT64) return a->i == b->i;
    double x, y;
    int an = pk_num_d(a, &x), bn = pk_num_d(b, &y);
    if (an && bn) return x == y;
    return strcmp(a->s ? a->s : "", b->s ? b->s : "") == 0;
}

/* 线性探测：找到返回槽位；未找到返回首个可用插入槽（优先首个墓碑） */
static uint64_t pk_probe(table_t* t, const nlsql_value_t* v, int* found) {
    uint64_t mask = t->pk_cap - 1;
    uint64_t i = pk_hash(v) & mask;
    uint64_t tomb = UINT64_MAX;
    for (uint64_t n = 0; n <= mask; n++, i = (i + 1) & mask) {
        uint64_t s = t->pk_slots[i];
        if (s == 0) { *found = 0; return (tomb != UINT64_MAX) ? tomb : i; }
        if (s == NLSQL_PK_TOMB) { if (tomb == UINT64_MAX) tomb = i; continue; }
        if (pk_eq(&t->rows[s - 2].vals[t->pk_col], v)) { *found = 1; return i; }
    }
    *found = 0;
    return (tomb != UINT64_MAX) ? tomb : 0; /* 不应到达（容量保证） */
}

static int pk_rebuild(table_t* t, uint64_t want) {
    uint64_t cap = 16;
    while (cap < want * 2) cap <<= 1;
    uint64_t* ns = (uint64_t*)calloc((size_t)cap, sizeof(uint64_t));
    if (!ns) return -1;
    free(t->pk_slots);
    t->pk_slots = ns; t->pk_cap = cap; t->pk_used = 0; t->pk_dirty = 0;
    for (uint64_t r = 0; r < t->row_count; r++) {
        const nlsql_value_t* v = &t->rows[r].vals[t->pk_col];
        if (v->is_null) continue;
        uint64_t mask = cap - 1, i = pk_hash(v) & mask;
        while (t->pk_slots[i] && t->pk_slots[i] != NLSQL_PK_TOMB) {
            if (t->pk_slots[i] - 2 == r) break; /* 同行重复（不应） */
            i = (i + 1) & mask;
        }
        if (!t->pk_slots[i]) t->pk_used++;
        t->pk_slots[i] = r + 2;
    }
    return 0;
}

/* 无锁内部版本：调用方已持 g_pk_mtx */
static void pk_free_raw(table_t* t) {
    free(t->pk_slots);
    t->pk_slots = NULL; t->pk_cap = 0; t->pk_used = 0; t->pk_col = -1; t->pk_dirty = 1;
}
static void pk_invalidate_raw(table_t* t) {
    free(t->pk_slots);
    t->pk_slots = NULL; t->pk_cap = 0; t->pk_used = 0; t->pk_dirty = 1;
}

int nlsql_pk_ensure(table_t* t) {
    int pkc = -1, npk = 0;
    pthread_mutex_lock(&g_pk_mtx);
    for (int i = 0; i < t->col_count; i++)
        if (t->cols[i].pk) { npk++; pkc = i; }
    if (npk != 1) {
        if (t->pk_slots) pk_free_raw(t);
        t->pk_col = -1;
        pthread_mutex_unlock(&g_pk_mtx);
        return NLSQL_ERR; /* 多列/无主键 → 调用方走线性 */
    }
    t->pk_col = pkc;
    int rc = NLSQL_OK;
    if (!t->pk_slots || t->pk_dirty)
        rc = pk_rebuild(t, t->row_count) == 0 ? NLSQL_OK : NLSQL_ERR;
    pthread_mutex_unlock(&g_pk_mtx);
    return rc;
}

int nlsql_pk_lookup(table_t* t, const nlsql_value_t* v, uint64_t* row_out) {
    int found = 0;
    pthread_mutex_lock(&g_pk_mtx);
    if (t->pk_slots && !t->pk_dirty && !v->is_null) {
        uint64_t i = pk_probe(t, v, &found);
        if (found && row_out) *row_out = t->pk_slots[i] - 2;
    }
    pthread_mutex_unlock(&g_pk_mtx);
    return found;
}

void nlsql_pk_add(table_t* t, uint64_t row_idx, const nlsql_value_t* v) {
    pthread_mutex_lock(&g_pk_mtx);
    if (!t->pk_slots || t->pk_dirty || v->is_null || t->pk_col < 0) {
        pthread_mutex_unlock(&g_pk_mtx);
        return;
    }
    if (t->pk_used * 10 >= t->pk_cap * 7) { /* 负载 >70% 扩容重建 */
        if (pk_rebuild(t, t->row_count) != 0) {
            pk_invalidate_raw(t);
            pthread_mutex_unlock(&g_pk_mtx);
            return;
        }
    }
    int found = 0;
    uint64_t i = pk_probe(t, v, &found);
    if (!found) {
        if (t->pk_slots[i] == 0) t->pk_used++;
        t->pk_slots[i] = row_idx + 2;
    }
    pthread_mutex_unlock(&g_pk_mtx);
}

void nlsql_pk_remove(table_t* t, uint64_t row_idx, const nlsql_value_t* v) {
    pthread_mutex_lock(&g_pk_mtx);
    if (t->pk_slots && !t->pk_dirty && !v->is_null && t->pk_col >= 0) {
        int found = 0;
        uint64_t i = pk_probe(t, v, &found);
        if (found && t->pk_slots[i] - 2 == row_idx) t->pk_slots[i] = NLSQL_PK_TOMB;
    }
    pthread_mutex_unlock(&g_pk_mtx);
}

void nlsql_pk_invalidate(table_t* t) {
    pthread_mutex_lock(&g_pk_mtx);
    pk_invalidate_raw(t);
    pthread_mutex_unlock(&g_pk_mtx);
}

void nlsql_pk_free(table_t* t) {
    pthread_mutex_lock(&g_pk_mtx);
    pk_free_raw(t);
    pthread_mutex_unlock(&g_pk_mtx);
}

/* =========================================
 * 二级索引（普通/唯一/复合）：排序行号数组 + 二分查找
 * 写路径只置 dirty，查找/唯一性检查前从 rows 惰性重建。
 * ========================================= */

static pthread_mutex_t g_idx_mtx = PTHREAD_MUTEX_INITIALIZER;

static int idx_val_cmp(const nlsql_value_t* a, const nlsql_value_t* b) {
    if (a->is_null || b->is_null) {
        if (a->is_null && b->is_null) return 0;
        return a->is_null ? -1 : 1;
    }
    int anum = (a->type == NLSQL_TYPE_INT64 || a->type == NLSQL_TYPE_DOUBLE);
    int bnum = (b->type == NLSQL_TYPE_INT64 || b->type == NLSQL_TYPE_DOUBLE);
    if (anum && bnum) {
        double x = (a->type == NLSQL_TYPE_INT64) ? (double)a->i : a->d;
        double y = (b->type == NLSQL_TYPE_INT64) ? (double)b->i : b->d;
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    const char* x = a->s ? a->s : "";
    const char* y = b->s ? b->s : "";
    uint32_t xl = a->s ? a->len : 0, yl = b->s ? b->len : 0;
    uint32_t m = xl < yl ? xl : yl;
    int c = m ? memcmp(x, y, m) : 0;
    if (c) return c;
    return xl < yl ? -1 : (xl > yl ? 1 : 0);
}

static _Thread_local table_t*    g_build_t;
static _Thread_local tbl_index_t* g_build_ix;

static int idx_row_cmp(const void* pa, const void* pb) {
    uint64_t ia = *(const uint64_t*)pa, ib = *(const uint64_t*)pb;
    table_t* t = g_build_t;
    tbl_index_t* ix = g_build_ix;
    for (int k = 0; k < ix->ncols; k++) {
        int c = idx_val_cmp(&t->rows[ia].vals[ix->cols[k]], &t->rows[ib].vals[ix->cols[k]]);
        if (c) return c;
    }
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

static int idx_key_cmp(const nlsql_value_t* keys, table_t* t, tbl_index_t* ix, uint64_t row) {
    for (int k = 0; k < ix->ncols; k++) {
        int c = idx_val_cmp(&keys[k], &t->rows[row].vals[ix->cols[k]]);
        if (c) return c;
    }
    return 0;
}

/* 重建排序数组；有 NULL 键的行不入索引（等值查找不会命中 NULL）。 */
static int idx_rebuild(table_t* t, tbl_index_t* ix) {
    uint64_t cap = t->row_count ? t->row_count : 1;
    uint64_t* ord = (uint64_t*)malloc((size_t)cap * sizeof(uint64_t));
    if (!ord) return -1;
    uint64_t n = 0;
    for (uint64_t r = 0; r < t->row_count; r++) {
        int nullkey = 0;
        for (int k = 0; k < ix->ncols; k++)
            if (t->rows[r].vals[ix->cols[k]].is_null) { nullkey = 1; break; }
        if (!nullkey) ord[n++] = r;
    }
    g_build_t = t; g_build_ix = ix;
    if (n > 1) qsort(ord, (size_t)n, sizeof(uint64_t), idx_row_cmp);
    g_build_t = NULL; g_build_ix = NULL;
    free(ix->order);
    ix->order = ord;
    ix->n = n;
    ix->dirty = 0;
    return 0;
}

int nlsql_idx_lookup(table_t* t, tbl_index_t* ix, const nlsql_value_t* keys,
                     uint64_t** rows_out, uint64_t* cnt_out) {
    *rows_out = NULL; *cnt_out = 0;
    if (!t || !ix) return NLSQL_ERR;
    pthread_mutex_lock(&g_idx_mtx);
    if (ix->dirty || !ix->order) {
        if (idx_rebuild(t, ix) != 0) { pthread_mutex_unlock(&g_idx_mtx); return NLSQL_ERR; }
    }
    uint64_t lo = 0, hi = ix->n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (idx_key_cmp(keys, t, ix, ix->order[mid]) > 0) lo = mid + 1;
        else hi = mid;
    }
    uint64_t e = lo;
    while (e < ix->n && idx_key_cmp(keys, t, ix, ix->order[e]) == 0) e++;
    uint64_t cnt = e - lo;
    if (cnt == 0) { pthread_mutex_unlock(&g_idx_mtx); return NLSQL_OK; }
    uint64_t* out = (uint64_t*)malloc((size_t)cnt * sizeof(uint64_t));
    if (!out) { pthread_mutex_unlock(&g_idx_mtx); return NLSQL_ERR; }
    memcpy(out, ix->order + lo, (size_t)cnt * sizeof(uint64_t));
    pthread_mutex_unlock(&g_idx_mtx);
    *rows_out = out; *cnt_out = cnt;
    return NLSQL_OK;
}

tbl_index_t* nlsql_idx_find(table_t* t, const char* name) {
    if (!t || !name) return NULL;
    for (tbl_index_t* ix = t->indexes; ix; ix = ix->next)
        if (strcmp(ix->name, name) == 0) return ix;
    return NULL;
}

int nlsql_idx_create(table_t* t, const char* name, const int* cols, int ncols, int unique,
                     char* err, uint32_t errsz) {
    if (!t || !name || !nlsql_valid_ident(name) || ncols <= 0 || ncols > 8) {
        if (err) snprintf(err, errsz, "invalid index definition");
        return NLSQL_ER_SYNTAX;
    }
    if (nlsql_idx_find(t, name)) {
        if (err) snprintf(err, errsz, "Duplicate key name '%s'", name);
        return NLSQL_ER_DUP_ENTRY;
    }
    tbl_index_t* ix = (tbl_index_t*)calloc(1, sizeof(tbl_index_t));
    if (!ix) return NLSQL_ERR;
    snprintf(ix->name, sizeof(ix->name), "%s", name);
    ix->ncols = ncols;
    ix->unique = unique ? 1 : 0;
    for (int i = 0; i < ncols; i++) ix->cols[i] = cols[i];
    ix->dirty = 1;
    ix->next = t->indexes;
    t->indexes = ix;
    if (unique) {
        /* 建唯一索引时立即重建并检查既有数据有无重复 */
        pthread_mutex_lock(&g_idx_mtx);
        int rc = idx_rebuild(t, ix);
        pthread_mutex_unlock(&g_idx_mtx);
        if (rc != 0) { nlsql_idx_drop(t, name); return rc; }
        for (uint64_t i = 1; i < ix->n; i++) {
            /* 两行键完全相等（不含行号）→ 重复 */
            int same = 1;
            for (int k = 0; k < ix->ncols; k++)
                if (idx_val_cmp(&t->rows[ix->order[i]].vals[ix->cols[k]],
                                &t->rows[ix->order[i - 1]].vals[ix->cols[k]]) != 0) { same = 0; break; }
            if (same) {
                if (err) snprintf(err, errsz, "Duplicate entry for key '%s'", name);
                nlsql_idx_drop(t, name);
                return NLSQL_ER_DUP_ENTRY;
            }
        }
    }
    return NLSQL_OK;
}

int nlsql_idx_drop(table_t* t, const char* name) {
    tbl_index_t** pp = &t->indexes;
    while (*pp) {
        tbl_index_t* ix = *pp;
        if (strcmp(ix->name, name) == 0) {
            *pp = ix->next;
            free(ix->order);
            free(ix);
            return NLSQL_OK;
        }
        pp = &ix->next;
    }
    return NLSQL_ER_UNKNOWN_STMT;
}

void nlsql_idx_mark_dirty(table_t* t) {
    for (tbl_index_t* ix = t->indexes; ix; ix = ix->next) ix->dirty = 1;
}

void nlsql_idx_free_all(table_t* t) {
    tbl_index_t* ix = t->indexes;
    while (ix) {
        tbl_index_t* n = ix->next;
        free(ix->order);
        free(ix);
        ix = n;
    }
    t->indexes = NULL;
}

/* =========================================
 * 事务快照（当前库，内存深拷贝）
 * ========================================= */

typedef struct txn_db_snap {
    char      dbname[NLSQL_MAX_IDENT + 1];
    table_t*  tables;
    int64_t   last_insert_id;
} txn_db_snap_t;

static void txn_free_tables(table_t* t) {
    while (t) { table_t* n = t->next; table_free(t); t = n; }
}

static int txn_copy_value(nlsql_value_t* dst, const nlsql_value_t* src) {
    *dst = *src;
    dst->s = NULL;
    if ((src->type == NLSQL_TYPE_VARCHAR || src->type == NLSQL_TYPE_TEXT ||
         src->type == NLSQL_TYPE_DATETIME) && src->s) {
        dst->s = (char*)malloc((size_t)src->len + 1);
        if (!dst->s) return -1;
        memcpy(dst->s, src->s, (size_t)src->len + 1);
        dst->len = src->len;
    }
    return 0;
}

static table_t* txn_copy_table(table_t* src) {
    table_t* t = (table_t*)calloc(1, sizeof(table_t));
    if (!t) return NULL;
    snprintf(t->name, sizeof(t->name), "%s", src->name);
    t->col_count = src->col_count;
    t->cols = (tbl_col_t*)calloc((size_t)(src->col_count ? src->col_count : 1), sizeof(tbl_col_t));
    if (!t->cols) { free(t); return NULL; }
    for (int i = 0; i < src->col_count; i++) {
        t->cols[i] = src->cols[i];
        t->cols[i].def.s = NULL;
        if (src->cols[i].has_def && src->cols[i].def.s) {
            t->cols[i].def.s = (char*)malloc((size_t)src->cols[i].def.len + 1);
            if (!t->cols[i].def.s) { table_free(t); return NULL; }
            memcpy(t->cols[i].def.s, src->cols[i].def.s, (size_t)src->cols[i].def.len + 1);
        }
    }
    t->row_count = t->row_cap = src->row_count;
    t->next_auto = src->next_auto;
    t->epoch = src->epoch;
    t->heap_rows = 1;
    t->rows = (row_t*)calloc((size_t)(src->row_count ? src->row_count : 1), sizeof(row_t));
    if (!t->rows) { table_free(t); return NULL; }
    for (uint64_t r = 0; r < src->row_count; r++) {
        t->rows[r].vals = (nlsql_value_t*)calloc((size_t)src->col_count, sizeof(nlsql_value_t));
        if (!t->rows[r].vals) { table_free(t); return NULL; }
        for (int i = 0; i < src->col_count; i++)
            if (txn_copy_value(&t->rows[r].vals[i], &src->rows[r].vals[i]) != 0) { table_free(t); return NULL; }
    }
    /* 索引定义随快照保留（数据重建留待下次查找） */
    tbl_index_t** pp = &t->indexes;
    for (tbl_index_t* ix = src->indexes; ix; ix = ix->next) {
        tbl_index_t* n = (tbl_index_t*)calloc(1, sizeof(tbl_index_t));
        if (!n) { table_free(t); return NULL; }
        *n = *ix;
        n->order = NULL; n->n = 0; n->dirty = 1; n->next = NULL;
        *pp = n; pp = &n->next;
    }
    return t;
}

/* 回滚后快照表直接成为活动表：把逐行 malloc 的行值迁入表级 arena，统一所有权。
   否则后续 INSERT/UPDATE 追加的行来自 arena，与堆行混用，table_free 无法按同一方式释放。 */
static void txn_adopt_tables(table_t* t) {
    for (; t; t = t->next) {
        if (!t->heap_rows) continue;
        uint64_t n = t->row_count;
        nlsql_value_t** tmp = n ? (nlsql_value_t**)malloc((size_t)n * sizeof(*tmp)) : NULL;
        if (n && !tmp) return; /* OOM：保持堆语义，table_free 仍可正确释放 */
        int ok = 1;
        for (uint64_t r = 0; r < n; r++) {
            tmp[r] = nlsql_tbl_val_alloc(t, t->col_count);
            if (!tmp[r]) { ok = 0; break; }
        }
        if (ok) {
            for (uint64_t r = 0; r < n; r++) {
                memcpy(tmp[r], t->rows[r].vals, (size_t)t->col_count * sizeof(nlsql_value_t));
                free(t->rows[r].vals); /* 字符串指针已转移，仅释放数组本身 */
                t->rows[r].vals = tmp[r];
            }
            t->heap_rows = 0;
        }
        free(tmp); /* 失败时该表保持 heap_rows=1 且行值不变，由 table_free 按堆释放 */
    }
}

/* 恢复的表强制整表重写快照（旧快照文件含已回滚的行） */
static void txn_mark_rewrite(table_t* t) {
    for (; t; t = t->next) {
        t->dirty = 1;
        t->snap_rows = 0;
        t->snap_epoch = 0;
        t->snap_frames = 0;
        t->snap_mutated = 1;
        t->pk_dirty = 1;
        nlsql_idx_mark_dirty(t);
    }
}

int nlsql_txn_begin(engine_t* e) {
    if (!e || e->readonly) return NLSQL_ERR;
    if (e->txn_active) return NLSQL_OK; /* 已开启：保持既有事务 */
    if (!e->cur_db) { nlsql_set_global_err("no database selected"); return NLSQL_ER_NO_SUCH_DB; }
    txn_db_snap_t* s = (txn_db_snap_t*)calloc(1, sizeof(txn_db_snap_t));
    if (!s) return NLSQL_ERR;
    snprintf(s->dbname, sizeof(s->dbname), "%s", e->cur_db->name);
    s->last_insert_id = e->last_insert_id;
    table_t** pp = &s->tables;
    for (table_t* t = e->cur_db->tables; t; t = t->next) {
        table_t* c = txn_copy_table(t);
        if (!c) { txn_free_tables(s->tables); free(s); return NLSQL_ERR; }
        *pp = c; pp = &c->next;
    }
    e->txn_snap = s;
    e->txn_active = 1;
    return NLSQL_OK;
}

int nlsql_txn_commit(engine_t* e) {
    if (!e) return NLSQL_ERR;
    txn_db_snap_t* s = (txn_db_snap_t*)e->txn_snap;
    if (s) txn_free_tables(s->tables);
    free(s);
    e->txn_snap = NULL;
    e->txn_active = 0;
    return NLSQL_OK;
}

int nlsql_txn_rollback(engine_t* e) {
    if (!e) return NLSQL_ERR;
    txn_db_snap_t* s = (txn_db_snap_t*)e->txn_snap;
    if (!s) { e->txn_active = 0; return NLSQL_OK; }
    database_t* db = e->cur_db;
    if (!db || strcmp(db->name, s->dbname) != 0) {
        nlsql_set_global_err("rollback requires the transaction database to be selected");
        txn_free_tables(s->tables);
        free(s);
        e->txn_snap = NULL;
        e->txn_active = 0;
        return NLSQL_ERR;
    }
    txn_free_tables(db->tables);
    db->tables = s->tables;
    e->last_insert_id = s->last_insert_id;
    free(s);
    e->txn_snap = NULL;
    e->txn_active = 0;
    txn_adopt_tables(db->tables); /* 快照表转为 arena 行值所有权，后续追加行释放方式一致 */
    txn_mark_rewrite(db->tables);
    /* 把恢复后的内存状态写回快照并清空 WAL（已回滚的记录不得在重开时被重放） */
    int rc = eng_flush(e);
    return rc;
}
