#include "threadpool.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/*  工作线程主函数（前向声明）                                           */
/* ------------------------------------------------------------------ */

/* http_handler.h 中声明的处理函数 */
extern void handle_client(http_sock_t client_fd, struct sockaddr_in *addr);

static void *worker_thread(void *arg)
{
    threadpool_t *pool = (threadpool_t *)arg;

    while (1) {
        pthread_mutex_lock(&pool->mutex);

        /* 等待任务或关闭信号 */
        while (pool->count == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->not_empty, &pool->mutex);
        }

        if (pool->shutdown && pool->count == 0) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        /* 取出队头任务 */
        client_task_t task = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->queue_size;
        pool->count--;

        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->mutex);

        /* 处理请求 */
        handle_client(task.client_fd, &task.client_addr);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  公开接口                                                            */
/* ------------------------------------------------------------------ */

threadpool_t *threadpool_create(int thread_count, int queue_size)
{
    threadpool_t *pool = calloc(1, sizeof(threadpool_t));
    if (!pool) return NULL;

    pool->thread_count = thread_count;
    pool->queue_size   = queue_size;
    pool->queue        = calloc(queue_size, sizeof(client_task_t));
    pool->threads      = calloc(thread_count, sizeof(pthread_t));

    if (!pool->queue || !pool->threads) goto err;

    pthread_mutex_init(&pool->mutex,    NULL);
    pthread_cond_init(&pool->not_empty, NULL);
    pthread_cond_init(&pool->not_full,  NULL);

    for (int i = 0; i < thread_count; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0) {
            LOG_ERROR("threadpool: failed to create thread %d", i);
            goto err;
        }
    }

    LOG_INFO("threadpool: created %d worker threads, queue size %d",
             thread_count, queue_size);
    return pool;

err:
    free(pool->queue);
    free(pool->threads);
    free(pool);
    return NULL;
}

int threadpool_submit(threadpool_t *pool, client_task_t task)
{
    pthread_mutex_lock(&pool->mutex);

    /* 队列满时等待 */
    while (pool->count == pool->queue_size && !pool->shutdown) {
        pthread_cond_wait(&pool->not_full, &pool->mutex);
    }

    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }

    pool->queue[pool->tail] = task;
    pool->tail = (pool->tail + 1) % pool->queue_size;
    pool->count++;

    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);
    return 0;
}

void threadpool_destroy(threadpool_t *pool)
{
    if (!pool) return;

    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);

    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);

    free(pool->queue);
    free(pool->threads);
    free(pool);

    LOG_INFO("threadpool: destroyed");
}
