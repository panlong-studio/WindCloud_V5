#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <mysql/mysql.h>
#include <pthread.h>
#include "db_pool.h"
#include "log.h"

/* 全局连接池实例 */
static db_pool_t *g_pool = NULL;
static pthread_mutex_t g_pool_init_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * 创建单个数据库连接
 */
static MYSQL* create_db_connection(db_pool_config_t *config) {
    MYSQL *conn = mysql_init(NULL);
    if (conn == NULL) {
        LOG_ERROR("MySQL 连接初始化失败");
        return NULL;
    }

    /* 连接到数据库 */
    if (mysql_real_connect(conn, config->host, config->user, config->password, 
                          config->db_name, 0, NULL, 0) == NULL) {
        LOG_ERROR("连接数据库失败: %s", mysql_error(conn));
        mysql_close(conn);
        return NULL;
    }

    /* 设置字符集 */
    mysql_set_character_set(conn, "utf8mb4");

    /* 设置连接超时 */
    unsigned int timeout = config->connection_timeout > 0 ? config->connection_timeout : 30;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    LOG_DEBUG("数据库连接创建成功");
    return conn;
}

/**
 * 验证连接是否有效
 */
static int is_connection_valid(MYSQL *conn) {
    if (conn == NULL) {
        return 0;
    }

    /* 使用 ping 命令检查连接 */
    if (mysql_ping(conn) != 0) {
        LOG_WARN("连接已失效: %s", mysql_error(conn));
        return 0;
    }

    return 1;
}

/**
 * 关闭数据库连接
 */
static void close_db_connection(MYSQL *conn) {
    if (conn != NULL) {
        mysql_close(conn);
        LOG_DEBUG("数据库连接已关闭");
    }
}

/**
 * 初始化连接池
 */
int db_pool_init(db_pool_config_t *config) {
    if (config == NULL) {
        LOG_ERROR("连接池配置为空");
        return -1;
    }

    pthread_mutex_lock(&g_pool_init_lock);

    /* 如果已经初始化过，直接返回 */
    if (g_pool != NULL) {
        LOG_WARN("连接池已初始化");
        pthread_mutex_unlock(&g_pool_init_lock);
        return 0;
    }

    /* 分配连接池结构 */
    g_pool = (db_pool_t *)malloc(sizeof(db_pool_t));
    if (g_pool == NULL) {
        LOG_ERROR("连接池内存分配失败");
        pthread_mutex_unlock(&g_pool_init_lock);
        return -1;
    }

    memset(g_pool, 0, sizeof(db_pool_t));

    /* 保存配置 */
    memcpy(&g_pool->config, config, sizeof(db_pool_config_t));
    g_pool->max_size = config->max_connections;
    g_pool->min_size = config->min_connections;
    g_pool->pool_size = 0;
    g_pool->in_use_count = 0;

    /* 初始化锁和条件变量 */
    pthread_mutex_init(&g_pool->lock, NULL);
    pthread_cond_init(&g_pool->cond, NULL);

    /* 分配连接数组 */
    g_pool->connections = (db_connection_t **)malloc(sizeof(db_connection_t *) * g_pool->max_size);
    if (g_pool->connections == NULL) {
        LOG_ERROR("连接数组内存分配失败");
        free(g_pool);
        g_pool = NULL;
        pthread_mutex_unlock(&g_pool_init_lock);
        return -1;
    }

    memset(g_pool->connections, 0, sizeof(db_connection_t *) * g_pool->max_size);

    /* 创建最小连接数 */
    for (int i = 0; i < g_pool->min_size; i++) {
        MYSQL *conn = create_db_connection(config);
        if (conn == NULL) {
            LOG_WARN("创建最小连接失败，已创建 %d 个连接", i);
            break;
        }

        db_connection_t *db_conn = (db_connection_t *)malloc(sizeof(db_connection_t));
        if (db_conn == NULL) {
            LOG_ERROR("连接封装内存分配失败");
            close_db_connection(conn);
            break;
        }

        db_conn->conn = conn;
        db_conn->last_used_time = time(NULL);
        db_conn->in_use = 0;

        g_pool->connections[g_pool->pool_size] = db_conn;
        g_pool->pool_size++;
    }

    LOG_INFO("连接池初始化成功: min=%d, max=%d, current=%d", 
        g_pool->min_size, g_pool->max_size, g_pool->pool_size);

    pthread_mutex_unlock(&g_pool_init_lock);
    return 0;
}

/**
 * 获取数据库连接
 */
MYSQL* db_pool_get_connection(void) {
    if (g_pool == NULL) {
        LOG_ERROR("连接池未初始化");
        return NULL;
    }

    pthread_mutex_lock(&g_pool->lock);

    /* 循环查找可用连接 */
    while (1) {
        /* 首先遍历现有连接，找到空闲连接 */
        for (int i = 0; i < g_pool->pool_size; i++) {
            db_connection_t *db_conn = g_pool->connections[i];
            if (db_conn != NULL && !db_conn->in_use) {
                /* 验证连接是否有效 */
                if (is_connection_valid(db_conn->conn)) {
                    db_conn->in_use = 1;
                    db_conn->last_used_time = time(NULL);
                    g_pool->in_use_count++;
                    
                    LOG_DEBUG("获取连接成功 (复用), 连接标号=%d, 使用中=%d, 总数=%d",
                        i, g_pool->in_use_count, g_pool->pool_size);
                    
                    pthread_mutex_unlock(&g_pool->lock);
                    return db_conn->conn;
                } else {
                    /* 连接无效，关闭并移除 */
                    LOG_WARN("移除失效连接，连接标号=%d", i);
                    close_db_connection(db_conn->conn);
                    free(db_conn);
                    g_pool->connections[i] = NULL;
                }
            }
        }

        /* 如果没有可用连接，检查是否可以创建新连接 */
        if (g_pool->pool_size < g_pool->max_size) {
            MYSQL *new_conn = create_db_connection(&g_pool->config);
            if (new_conn != NULL) {
                db_connection_t *db_conn = (db_connection_t *)malloc(sizeof(db_connection_t));
                if (db_conn != NULL) {
                    db_conn->conn = new_conn;
                    db_conn->last_used_time = time(NULL);
                    db_conn->in_use = 1;

                    g_pool->connections[g_pool->pool_size] = db_conn;
                    g_pool->pool_size++;
                    g_pool->in_use_count++;

                    LOG_DEBUG("获取连接成功 (新建), 连接标号=%d, 使用中=%d, 总数=%d",
                        g_pool->pool_size - 1, g_pool->in_use_count, g_pool->pool_size);

                    pthread_mutex_unlock(&g_pool->lock);
                    return new_conn;
                } else {
                    LOG_ERROR("连接封装内存分配失败");
                    close_db_connection(new_conn);
                }
            }
        }

        /* 连接池已满且无可用连接，等待其他线程释放连接 */
        LOG_DEBUG("连接池满，等待连接释放... 使用中=%d, 总数=%d", g_pool->in_use_count, g_pool->pool_size);
        
        int timeout_sec = g_pool->config.connection_timeout > 0 ? g_pool->config.connection_timeout : 30;
        struct timespec abs_timeout;
        clock_gettime(CLOCK_REALTIME, &abs_timeout);
        abs_timeout.tv_sec += timeout_sec;

        int ret = pthread_cond_timedwait(&g_pool->cond, &g_pool->lock, &abs_timeout);
        if (ret == ETIMEDOUT) {
            LOG_ERROR("获取连接超时");
            pthread_mutex_unlock(&g_pool->lock);
            return NULL;
        }
    }

    pthread_mutex_unlock(&g_pool->lock);
    return NULL;
}

/**
 * 释放数据库连接回连接池
 */
int db_pool_release_connection(MYSQL *conn) {
    if (g_pool == NULL || conn == NULL) {
        LOG_ERROR("无效参数: pool=%p, conn=%p", g_pool, conn);
        return -1;
    }

    pthread_mutex_lock(&g_pool->lock);

    /* 在连接池中找到对应的连接 */
    for (int i = 0; i < g_pool->pool_size; i++) {
        db_connection_t *db_conn = g_pool->connections[i];
        if (db_conn != NULL && db_conn->conn == conn) {
            db_conn->in_use = 0;
            db_conn->last_used_time = time(NULL);
            g_pool->in_use_count--;

            LOG_DEBUG("连接已释放，连接标号=%d, 使用中=%d, 总数=%d",
                i, g_pool->in_use_count, g_pool->pool_size);

            /* 通知等待的线程有连接可用 */
            pthread_cond_signal(&g_pool->cond);

            pthread_mutex_unlock(&g_pool->lock);
            return 0;
        }
    }

    LOG_WARN("释放未知连接");
    pthread_mutex_unlock(&g_pool->lock);
    return -1;
}

/**
 * 获取连接池状态
 */
void db_pool_get_stats(int *pool_size, int *in_use_count) {
    if (g_pool == NULL) {
        if (pool_size != NULL) *pool_size = 0;
        if (in_use_count != NULL) *in_use_count = 0;
        return;
    }

    pthread_mutex_lock(&g_pool->lock);

    if (pool_size != NULL) *pool_size = g_pool->pool_size;
    if (in_use_count != NULL) *in_use_count = g_pool->in_use_count;

    pthread_mutex_unlock(&g_pool->lock);
}

/**
 * 销毁连接池
 */
void db_pool_destroy(void) {
    if (g_pool == NULL) {
        return;
    }

    pthread_mutex_lock(&g_pool->lock);

    /* 关闭所有连接 */
    for (int i = 0; i < g_pool->pool_size; i++) {
        if (g_pool->connections[i] != NULL) {
            close_db_connection(g_pool->connections[i]->conn);
            free(g_pool->connections[i]);
            g_pool->connections[i] = NULL;
        }
    }

    /* 释放连接数组 */
    if (g_pool->connections != NULL) {
        free(g_pool->connections);
        g_pool->connections = NULL;
    }

    LOG_INFO("连接池已销毁");

    pthread_mutex_unlock(&g_pool->lock);

    /* 销毁锁和条件变量 */
    pthread_cond_destroy(&g_pool->cond);
    pthread_mutex_destroy(&g_pool->lock);

    free(g_pool);
    g_pool = NULL;
}
