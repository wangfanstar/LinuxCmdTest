#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include "platform.h"
#ifndef _WIN32
#include <netinet/in.h>
#endif

/* ------------------------------------------------------------------ */
/*  任务结构：封装一个客户端连接                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    http_sock_t         client_fd;
    struct sockaddr_in  client_addr;
} client_task_t;

/* ------------------------------------------------------------------ */
/*  线程池结构                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    /* 工作线程数组 */
    pthread_t          *threads;
    int                 thread_count;

    /* 任务循环队列 */
    client_task_t      *queue;
    int                 queue_size;   /* 队列容量 */
    int                 head;         /* 出队位置 */
    int                 tail;         /* 入队位置 */
    int                 count;        /* 当前任务数 */

    /* 同步原语 */
    pthread_mutex_t     mutex;
    pthread_cond_t      not_empty;    /* 有任务可取 */
    pthread_cond_t      not_full;     /* 有空位可入队 */

    /* 关闭标志 */
    int                 shutdown;
} threadpool_t;

/* 创建线程池；thread_count 个工作线程，queue_size 任务队列长度 */
threadpool_t *threadpool_create(int thread_count, int queue_size);

/* 向线程池提交一个任务；队列满时阻塞等待空位 */
int threadpool_submit(threadpool_t *pool, client_task_t task);

/* 优雅关闭线程池（等待所有线程退出后释放资源） */
void threadpool_destroy(threadpool_t *pool);

#endif /* THREADPOOL_H */
