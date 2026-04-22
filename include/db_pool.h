#ifndef _DB_POOL_H_
#define _DB_POOL_H_

#include <mysql/mysql.h>
#include <pthread.h>

/**
 * 数据库连接池
 * 支持连接的创建、获取、释放、回收、销毁
 * 线程安全的连接池实现
 */

/**
 * 连接池配置参数
 */
typedef struct {
    const char *host;
    const char *user;
    const char *password;
    const char *db_name;
    int min_connections;      /* 最小连接数 */
    int max_connections;      /* 最大连接数 */
    int max_idle_time;        /* 连接最大空闲时间（秒），0 表示无限制 */
    int connection_timeout;   /* 获取连接超时时间（秒） */
} db_pool_config_t;

/**
 * 单个连接的状态封装
 */
typedef struct {
    MYSQL *conn;              /* MySQL 连接指针 */
    time_t last_used_time;    /* 最后使用时间 */
    int in_use;               /* 是否正在使用（1 正在使用，0 空闲） */
} db_connection_t;

/**
 * 连接池结构
 */
typedef struct {
    db_connection_t **connections;  /* 连接数组 */
    int pool_size;                   /* 当前连接池大小 */
    int max_size;                    /* 最大连接数 */
    int min_size;                    /* 最小连接数 */
    int in_use_count;                /* 正在使用的连接数 */
    
    db_pool_config_t config;         /* 连接池配置 */
    pthread_mutex_t lock;            /* 线程互斥锁 */
    pthread_cond_t cond;             /* 条件变量 */
} db_pool_t;

/**
 * 初始化连接池
 * 参数 config：连接池配置
 * 返回：0 成功，-1 失败
 */
int db_pool_init(db_pool_config_t *config);

/**
 * 获取数据库连接
 * 若连接池中有可用连接，直接返回
 * 否则等待其他连接释放或创建新连接
 * 返回：MYSQL* 指针，NULL 表示失败
 */
MYSQL* db_pool_get_connection(void);

/**
 * 释放数据库连接回连接池
 * 参数 conn：要释放的连接
 * 返回：0 成功，-1 失败
 */
int db_pool_release_connection(MYSQL *conn);

/**
 * 获取连接池状态
 * 参数 pool_size：当前连接池大小（输出）
 * 参数 in_use_count：正在使用的连接数（输出）
 * 返回：void
 */
void db_pool_get_stats(int *pool_size, int *in_use_count);

/**
 * 销毁连接池，关闭所有连接
 * 返回：void
 */
void db_pool_destroy(void);

#endif
