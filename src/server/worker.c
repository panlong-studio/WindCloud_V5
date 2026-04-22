#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include "worker.h"
#include "thread_pool.h"
#include "queue.h"
#include "session.h"
#include "log.h"
#include "dao_vfs.h"


void* thread_func(void *arg) {
    worker_arg_t *worker_arg = (worker_arg_t *)arg;   // arg保存了线程池指针和线程编号

    thread_pool_t *pool = worker_arg->pool;

    // 取出当前线程自己的编号
    int worker_index = worker_arg->index;

    // 获取数据库连接
    MYSQL *conn = dao_get_connection();
    if (conn == NULL) {
        LOG_ERROR("线程无法获取数据库连接，线程=%lu", (unsigned long)pthread_self());
        return NULL;
    }
    LOG_INFO("工作线程数据库连接已获取，线程=%lu", (unsigned long)pthread_self());

    while (1) {
        // 原子性访问互斥资源
        pthread_mutex_lock(&pool->lock);
        while (pool->queue.size==0 && !pool->exitFlag) {
            pthread_cond_wait(&pool->cond, &pool->lock);
        }

        if (pool->exitFlag) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }
        int client_fd = deQueue(&pool->queue);

        // 记录正在处理的客户端 fd，方便后续 shutdown（断开TCP）
        pool->busy_fds[worker_index] = client_fd;
        pthread_mutex_unlock(&pool->lock);
        LOG_INFO("工作线程开始处理客户端，线程=%lu，客户端fd=%d", (unsigned long)pthread_self(), client_fd);


        // 真正处理这个客户端的所有命令
        handle_request(client_fd);  

        // 处理完，关闭fd，回收内核资源
        shutdown(client_fd, SHUT_WR);  //shutdown只关闭本端
        close(client_fd);             //close直接关闭收发两端

        // 准备修改 busy_fds，所以重新加锁。
        pthread_mutex_lock(&pool->lock);

        // 当前线程已经空闲了，把记录改回 -1
        pool->busy_fds[worker_index] = -1;
        pthread_mutex_unlock(&pool->lock);
        LOG_INFO("工作线程处理完成，线程=%lu，客户端fd=%d", (unsigned long)pthread_self(), client_fd);
    }

    // 线程退出时关闭线程局部数据库连接
    dao_close_connection(conn);
    LOG_INFO("工作线程退出，线程=%lu", (unsigned long)pthread_self());
    return NULL;
}
