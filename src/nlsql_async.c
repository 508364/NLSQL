/**
 * @file nlsql_async.c
 * @brief NLSQL 异步工作队列：pthread 线程池 + 任务链表
 *
 * 低占用设计：默认 1 个工作线程；任务回调在工作线程触发。
 */
#include "nlsql_internal.h"

static void* aq_worker(void* arg) {
    async_queue_t* q = (async_queue_t*)arg;
    pthread_mutex_lock(&q->mtx);
    for (;;) {
        while (q->running && !q->head)
            pthread_cond_wait(&q->cv, &q->mtx);
        if (!q->running) break;
        async_task_t* t = q->head;
        q->head = t->next;
        if (!q->head) q->tail = NULL;
        q->pending--;
        q->inflight++;
        pthread_mutex_unlock(&q->mtx);
        t->fn(q->owner, t->arg);
        free(t);
        pthread_mutex_lock(&q->mtx);
        q->inflight--;
        if (q->inflight == 0 && q->pending == 0)
            pthread_cond_broadcast(&q->done_cv);
    }
    pthread_mutex_unlock(&q->mtx);
    return NULL;
}

int aq_start(async_queue_t* q, nlsql_conn_t* owner, int nthreads) {
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 8) nthreads = 8;
    memset(q, 0, sizeof(*q));
    q->owner = owner;
    q->nthreads = nthreads;
    q->running = 1;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->cv, NULL);
    pthread_cond_init(&q->done_cv, NULL);
    q->threads = (pthread_t*)calloc((size_t)nthreads, sizeof(pthread_t));
    if (!q->threads) return -1;
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&q->threads[i], NULL, aq_worker, q) != 0) {
            q->running = 0;
            pthread_cond_broadcast(&q->cv);
            for (int j = 0; j < i; j++) pthread_join(q->threads[j], NULL);
            free(q->threads);
            q->threads = NULL;
            return -1;
        }
    }
    return 0;
}

int aq_push(async_queue_t* q, async_fn fn, void* arg, void (*drop)(void*)) {
    pthread_mutex_lock(&q->mtx);
    if (!q->running) { pthread_mutex_unlock(&q->mtx); return NLSQL_ERR_BUSY; }
    if (q->pending >= NLSQL_DEFAULT_QUEUE) {
        pthread_mutex_unlock(&q->mtx);
        return NLSQL_ERR_BUSY;
    }
    async_task_t* t = (async_task_t*)calloc(1, sizeof(async_task_t));
    if (!t) { pthread_mutex_unlock(&q->mtx); return NLSQL_ERR; }
    t->fn = fn; t->arg = arg; t->drop = drop; t->next = NULL;
    if (q->tail) q->tail->next = t; else q->head = t;
    q->tail = t;
    q->pending++;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mtx);
    return NLSQL_OK;
}

void aq_wait_idle(async_queue_t* q) {
    pthread_mutex_lock(&q->mtx);
    while (q->pending > 0 || q->inflight > 0)
        pthread_cond_wait(&q->done_cv, &q->mtx);
    pthread_mutex_unlock(&q->mtx);
}

int aq_pending(async_queue_t* q) {
    pthread_mutex_lock(&q->mtx);
    int n = q->pending;
    pthread_mutex_unlock(&q->mtx);
    return n;
}

void aq_drain(async_queue_t* q) {
    if (!q->threads) return;
    pthread_mutex_lock(&q->mtx);
    q->running = 0;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mtx);
    for (int i = 0; i < q->nthreads; i++)
        pthread_join(q->threads[i], NULL);
    free(q->threads);
    q->threads = NULL;
    /* 释放未执行任务：drop 释放 arg（否则任务载荷泄漏）；
       pending 清零 + broadcast，避免并发 aq_wait_idle 永久挂起 */
    pthread_mutex_lock(&q->mtx);
    async_task_t* t = q->head;
    while (t) {
        async_task_t* nx = t->next;
        if (t->drop) t->drop(t->arg);
        free(t);
        t = nx;
    }
    q->head = q->tail = NULL;
    q->pending = 0;
    pthread_cond_broadcast(&q->done_cv);
    pthread_mutex_unlock(&q->mtx);
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->cv);
    pthread_cond_destroy(&q->done_cv);
}
