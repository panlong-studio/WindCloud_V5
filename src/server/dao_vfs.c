#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mysql/mysql.h>
#include <pthread.h>
#include "dao_vfs.h"
#include "db_pool.h"
#include "log.h"
#include "sha256_utils.h"

//MySQL 连接参数（默认值）
static char g_db_host[64] = "127.0.0.1";
static char g_db_user[64] = "root";
static char g_db_password[128] = "123456";
static char g_db_name[64] = "windcloud";

/* 线程局部存储：每个工作线程独立的数据库连接（TLS 模式） */
static __thread MYSQL *g_thread_db_conn = NULL;

/* 连接池使用标记 */
static int g_use_pool = 0;

/* ============ 工具函数 ============ */

/**
 * 安全执行 SQL 查询
 */
static int dao_execute_query(MYSQL *conn, const char *query) {
    if (mysql_query(conn, query) != 0) {
        LOG_ERROR("SQL 查询失败: %s, 错误: %s", query, mysql_error(conn));
        return -1;
    }
    return 0;
}

/**
 * 生成密码哈希和盐值（使用 SHA256）
 */
static void generate_password_hash(const char *password, char *salt, char *hash) {
    /* 简单盐值生成（生产环境应使用更强的随机盐） */
    sprintf(salt, "%ld", (long)time(NULL) % 1000000);
    
    /* 拼接盐值和密码， 生成哈希 */
    char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", password, salt);
    sha256_hash(combined, hash);
}

/**
 * 验证密码（优先使用 SHA256，回退支持 MD5 并尝试迁移到 SHA256）
 */
static int verify_password(MYSQL *conn, int user_id, const char *password, const char *stored_hash, const char *stored_salt) {
    char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", password, stored_salt);

    /* 首先使用 SHA256 验证 */
    char sha_hash[65] = {0};
    if (sha256_hash(combined, sha_hash) == 0) {
        if (strcmp(sha_hash, stored_hash) == 0) {
            return 0;
        }
    }
    return -1;
}

/* ============ 连接管理 ============ */

void dao_set_connection_params(const char *host, const char *user, const char *password, const char *name) {
    if (host != NULL) {
        strncpy(g_db_host, host, sizeof(g_db_host) - 1);
    }
    if (user != NULL) {
        strncpy(g_db_user, user, sizeof(g_db_user) - 1);
    }
    if (password != NULL) {
        strncpy(g_db_password, password, sizeof(g_db_password) - 1);
    }
    if (name != NULL) {
        strncpy(g_db_name, name, sizeof(g_db_name) - 1);
    }
    LOG_INFO("数据库连接参数已设置: host=%s, user=%s, db=%s", g_db_host, g_db_user, g_db_name);
}

void* get_thread_db_conn(void) {
    return (void*)g_thread_db_conn;
}

int dao_init(void) {
    /* MySQL 全局初始化 */
    if (mysql_library_init(0, NULL, NULL) != 0) {
        LOG_ERROR("MySQL 库初始化失败");
        return -1;
    }
    LOG_INFO("MySQL 库初始化成功");
    return 0;
}

int dao_init_with_pool(int min_connections, int max_connections) {
    /* 先初始化 MySQL 库 */
    if (dao_init() != 0) {
        return -1;
    }

    /* 创建连接池配置 */
    db_pool_config_t config = {0};
    config.host = g_db_host;
    config.user = g_db_user;
    config.password = g_db_password;
    config.db_name = g_db_name;
    config.min_connections = min_connections > 0 ? min_connections : 5;
    config.max_connections = max_connections > 0 ? max_connections : 20;
    config.max_idle_time = 3600;    /* 1 小时空闲超时 */
    config.connection_timeout = 30;  /* 30 秒获取连接超时 */

    /* 初始化连接池 */
    if (db_pool_init(&config) != 0) {
        LOG_ERROR("数据库连接池初始化失败");
        return -1;
    }

    g_use_pool = 1;  /* 标记使用连接池模式 */
    LOG_INFO("数据库连接池初始化成功: min=%d, max=%d", config.min_connections, config.max_connections);
    return 0;
}

MYSQL* dao_get_connection(void) {
    /* 使用连接池模式 */
    if (g_use_pool) {
        MYSQL *conn = db_pool_get_connection();
        if (conn != NULL) {
            g_thread_db_conn = conn;  /* 也保存到 TLS 中，以保持兼容性 */
        }
        return conn;
    }

    /* 传统 TLS 模式 */
    MYSQL *conn = mysql_init(NULL);
    if (conn == NULL) {
        LOG_ERROR("MySQL 连接初始化失败");
        return NULL;
    }

    if (mysql_real_connect(conn, g_db_host, g_db_user, g_db_password, g_db_name, 0, NULL, 0) == NULL) {
        LOG_ERROR("连接数据库失败: %s", mysql_error(conn));
        mysql_close(conn);
        return NULL;
    }

    /* 设置字符集 */
    mysql_set_character_set(conn, "utf8mb4");

    g_thread_db_conn = conn;
    LOG_INFO("线程数据库连接建立成功");
    return conn;
}

void dao_close_connection(MYSQL *conn) {
    if (conn == NULL) {
        return;
    }

    /* 连接池模式：释放连接回池 */
    if (g_use_pool) {
        db_pool_release_connection(conn);
        g_thread_db_conn = NULL;
        return;
    }

    /* 传统 TLS 模式：直接关闭连接 */
    mysql_close(conn);
    g_thread_db_conn = NULL;
    LOG_INFO("线程数据库连接已关闭");
}

/* ============ 用户认证 ============ */

int dao_login_user(MYSQL *conn, const char *username, const char *password) {
    char query[512];
    MYSQL_RES *result;
    MYSQL_ROW row;
    int user_id = -1;

    if (conn == NULL || username == NULL || password == NULL) {
        return -1;
    }

    /* 查询用户 */
    snprintf(query, sizeof(query), 
        "SELECT id, password_hash, salt FROM users WHERE username='%s'", 
        username);

    if (dao_execute_query(conn, query) != 0) {
        return -1;
    }

    result = mysql_store_result(conn);
    if (result == NULL) {
        LOG_ERROR("获取查询结果失败: %s", mysql_error(conn));
        return -1;
    }

    row = mysql_fetch_row(result);
    if (row != NULL) {
        /* 用户存在，验证密码 */
        int id = atoi(row[0]);
        const char *stored_hash = row[1];
        const char *stored_salt = row[2];

        if (verify_password(conn, id, password, stored_hash, stored_salt) == 0) {
            user_id = id;
            LOG_INFO("用户登录成功: username=%s, user_id=%d", username, user_id);
        } else {
            LOG_WARN("用户登录密码错误: username=%s", username);
        }
    } else {
        LOG_WARN("用户不存在: username=%s", username);
    }

    mysql_free_result(result);
    return user_id;
}

int dao_register_user(MYSQL *conn, const char *username, const char *password) {
    char query[512];
    char salt[33] = {0};
    char password_hash[65] = {0};
    MYSQL_RES *result;

    if (conn == NULL || username == NULL || password == NULL) {
        return -1;
    }

    /* 检查用户是否已存在 */
    snprintf(query, sizeof(query), "SELECT id FROM users WHERE username='%s'", username);
    if (dao_execute_query(conn, query) != 0) {
        return -1;
    }

    result = mysql_store_result(conn);
    if (result == NULL) {
        LOG_ERROR("获取查询结果失败: %s", mysql_error(conn));
        return -1;
    }

    if (mysql_num_rows(result) > 0) {
        mysql_free_result(result);
        LOG_WARN("用户已存在: username=%s", username);
        return 0;
    }
    mysql_free_result(result);

    /* 生成盐值和哈希 */
    generate_password_hash(password, salt, password_hash);

    /* 插入新用户 */
    snprintf(query, sizeof(query),
        "INSERT INTO users (username, password_hash, salt) VALUES ('%s', '%s', '%s')",
        username, password_hash, salt);

    if (dao_execute_query(conn, query) != 0) {
        return -1;
    }

    int new_id = mysql_insert_id(conn);
    LOG_INFO("用户注册成功: username=%s, user_id=%d", username, new_id);

    /* 为新用户创建根目录 */
    if (dao_init_root_dir(conn, new_id) < 0) {
        LOG_WARN("无法为新用户创建根目录: user_id=%d", new_id);
    }

    return new_id;
}

/* ============ 目录和文件操作 ============ */

int dao_get_root_dir_id(MYSQL *conn, int user_id) {
    char query[256];
    MYSQL_RES *result;
    MYSQL_ROW row;
    int dir_id = -1;

    if (conn == NULL || user_id <= 0) {
        return -1;
    }

    snprintf(query, sizeof(query),
        "SELECT id FROM paths WHERE user_id=%d AND parent_id IS NULL AND type='d'",
        user_id);

    if (dao_execute_query(conn, query) != 0) {
        return -1;
    }

    result = mysql_store_result(conn);
    if (result == NULL) {
        return -1;
    }

    row = mysql_fetch_row(result);
    if (row != NULL) {
        dir_id = atoi(row[0]);
    }

    mysql_free_result(result);
    return dir_id;
}

//获取父目录id
int dao_get_parent_id(MYSQL *conn, int dir_id) {
    if (conn == NULL || dir_id <= 0) {
        return -1;
    }
    char query[256];
    snprintf(query, sizeof(query), "SELECT parent_id FROM paths WHERE id=%d AND tomb=0", dir_id);
    if (dao_execute_query(conn, query) != 0) {
        LOG_ERROR("查询父目录失败，dir_id=%d", dir_id);
        return -1;
    }
    MYSQL_RES *res = mysql_store_result(conn);
    if (res == NULL) {
        LOG_ERROR("获取结果集失败，dir_id=%d", dir_id);
        return -1;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    int parent_id = -1;
    if (row != NULL && row[0] != NULL) {
        parent_id = atoi(row[0]);
    }
    mysql_free_result(res);
    return parent_id;  // 根目录返回 -1
}

int dao_init_root_dir(MYSQL *conn, int user_id) {
    char query[512];
    MYSQL_RES *result;
    int root_id = -1;

    if (conn == NULL || user_id <= 0) {
        return -1;
    }

    /* 检查根目录是否已存在 */
    root_id = dao_get_root_dir_id(conn, user_id);
    if (root_id > 0) {
        return root_id;  /* 已存在 */
    }

    /* 创建根目录 */
    snprintf(query, sizeof(query),
        "INSERT INTO paths (user_id, parent_id, name, path, type) VALUES (%d, NULL, '/', '/', 'd')",
        user_id);

    if (dao_execute_query(conn, query) != 0) {
        return -1;
    }

    root_id = mysql_insert_id(conn);
    LOG_INFO("根目录创建成功: user_id=%d, root_id=%d", user_id, root_id);
    return root_id;
}

int dao_find_child_id(MYSQL *conn, int parent_id, const char *name, int is_dir) {
    char query[512];
    MYSQL_RES *result;
    MYSQL_ROW row;
    int child_id = -1;

    if (conn == NULL || parent_id <= 0 || name == NULL) {
        return -1;
    }

    if (is_dir == -1) {
        /* 不限制类型 */
        snprintf(query, sizeof(query),
            "SELECT id FROM paths WHERE parent_id=%d AND name='%s' AND tomb=0",
            parent_id, name);
    } else if (is_dir == 1) {
        /* 查找目录 */
        snprintf(query, sizeof(query),
            "SELECT id FROM paths WHERE parent_id=%d AND name='%s' AND type='d' AND tomb=0",
            parent_id, name);
    } else {
        /* 查找文件 */
        snprintf(query, sizeof(query),
            "SELECT id FROM paths WHERE parent_id=%d AND name='%s' AND type='f' AND tomb=0",
            parent_id, name);
    }

    if (dao_execute_query(conn, query) != 0) {
        return -1;
    }

    result = mysql_store_result(conn);
    if (result == NULL) {
        return -1;
    }

    row = mysql_fetch_row(result);
    if (row != NULL) {
        child_id = atoi(row[0]);
    }

    mysql_free_result(result);
    return child_id;
}

// int dao_list_dir(MYSQL *conn, int user_id, int dir_id, char *result, int result_len) {
//     char query[512];
//     MYSQL_RES *sql_result;
//     MYSQL_ROW row;
//     int used = 0;
//     if (conn == NULL || user_id <= 0 || dir_id <= 0 || result == NULL) {
//         return -1;
//     }
//     memset(result, 0, result_len);
//     snprintf(query, sizeof(query),
//         "SELECT name FROM paths WHERE user_id=%d AND parent_id=%d AND tomb=0 ORDER BY type DESC, name",
//         user_id, dir_id);
//     if (dao_execute_query(conn, query) != 0) {
//         return -1;
//     }
//     sql_result = mysql_store_result(conn);
//     if (sql_result == NULL) {
//         return -1;
//     }
//     while ((row = mysql_fetch_row(sql_result)) != NULL) {
//         int left = result_len - used;
//         int ret = snprintf(result + used, left, "%s ", row[0]);
//         if (ret < 0 || ret >= left) break;
//         used += ret;
//     }
//     mysql_free_result(sql_result);
//     if (used == 0) {
//         strcpy(result, "当前目录为空");
//     }
//     return 0;
// }

//新版dao_list_dir
int dao_list_dir(MYSQL *conn, int user_id, int dir_id, char *result, int result_len) {
    char query[512];
    MYSQL_RES *sql_result;
    MYSQL_ROW row;
    int used = 0;

    if (conn == NULL || user_id <= 0 || dir_id <= 0 || result == NULL) {
        return -1;
    }

    memset(result, 0, result_len);

    // 先添加 "." 和 ".."
    int ret = snprintf(result, result_len, ". ..");
    if (ret < 0 || ret >= result_len) return -1;
    used = ret;

    // 查询数据库中的子项
    snprintf(query, sizeof(query),
        "SELECT name FROM paths WHERE user_id=%d AND parent_id=%d AND tomb=0 ORDER BY type DESC, name",
        user_id, dir_id);

    if (dao_execute_query(conn, query) != 0) {
        return -1;
    }

    sql_result = mysql_store_result(conn);
    if (sql_result == NULL) {
        return -1;
    }

    // 如果有子项，先加一个空格，再逐个添加
    if (mysql_num_rows(sql_result) > 0) {
        // 加空格分隔符
        if (used + 1 < result_len) {
            result[used] = ' ';
            used++;
            result[used] = '\0';
        }
        while ((row = mysql_fetch_row(sql_result)) != NULL) {
            int left = result_len - used;
            ret = snprintf(result + used, left, "%s ", row[0]);
            if (ret < 0 || ret >= left) break;
            used += ret;
        }
        // 去掉末尾可能多余的空格
        if (used > 0 && result[used-1] == ' ') {
            result[used-1] = '\0';
        }
    }

    mysql_free_result(sql_result);
    return 0;
}

int dao_create_dir(MYSQL *conn, int user_id, int parent_id, const char *dir_name) {
    char query[512];
    char path[512];
    MYSQL_RES *result;
    int new_id = -1;

    if (conn == NULL || user_id <= 0 || parent_id <= 0 || dir_name == NULL) {
        return -1;
    }

    /* 检查目录是否已存在 */
    if (dao_find_child_id(conn, parent_id, dir_name, 1) > 0) {
        LOG_WARN("目录已存在: dir_name=%s", dir_name);
        return 0;
    }

    /* 构建虚拟路径 */
    snprintf(path, sizeof(path), "/%s", dir_name);

    /* 创建目录 */
    snprintf(query, sizeof(query),
        "INSERT INTO paths (user_id, parent_id, name, path, type) VALUES (%d, %d, '%s', '%s', 'd')",
        user_id, parent_id, dir_name, path);

    if (dao_execute_query(conn, query) != 0) {
        return -1;
    }

    new_id = mysql_insert_id(conn);
    LOG_INFO("目录创建成功: user_id=%d, parent_id=%d, dir_name=%s, new_id=%d",
        user_id, parent_id, dir_name, new_id);
    return new_id;
}

int dao_delete_entry(MYSQL *conn, int user_id, int path_id) {
    char query[512];

    if (conn == NULL || user_id <= 0 || path_id <= 0) {
        return -1;
    }

    /* 使用软删除（tomb 标记） */
    snprintf(query, sizeof(query),
        "UPDATE paths SET tomb=1 WHERE id=%d AND user_id=%d",
        path_id, user_id);

    if (dao_execute_query(conn, query) != 0) {
        return -1;
    }

    LOG_INFO("条目删除成功: user_id=%d, path_id=%d", user_id, path_id);
    return 0;
}

int dao_get_file_info(MYSQL *conn, int user_id, int path_id, char *sha256, unsigned long *file_size) {
    char query[512];
    MYSQL_RES *result;
    MYSQL_ROW row;

    if (conn == NULL || user_id <= 0 || path_id <= 0 || sha256 == NULL || file_size == NULL) {
        return -1;
    }

    snprintf(query, sizeof(query),
        "SELECT f.sha256sum, f.size FROM files f "
        "JOIN paths p ON f.id = p.file_id "
        "WHERE p.id=%d AND p.user_id=%d AND p.type='f'",
        path_id, user_id);

    if (dao_execute_query(conn, query) != 0) {
        return -1;
    }

    result = mysql_store_result(conn);
    if (result == NULL) {
        return -1;
    }

    row = mysql_fetch_row(result);
    if (row != NULL) {
        strcpy(sha256, row[0]);
        *file_size = strtoul(row[1], NULL, 10);
        mysql_free_result(result);
        return 0;
    }

    mysql_free_result(result);
    return -1;
}

/* ============ 文件存储相关 ============ */

int dao_find_file_by_hash(MYSQL *conn, const char *sha256) {
    char query[512];
    MYSQL_RES *result;
    MYSQL_ROW row;
    int file_id = -1;

    if (conn == NULL || sha256 == NULL) {
        return -1;
    }

    snprintf(query, sizeof(query),
        "SELECT id FROM files WHERE sha256sum='%s'",
        sha256);

    if (dao_execute_query(conn, query) != 0) {
        return -1;
    }

    result = mysql_store_result(conn);
    if (result == NULL) {
        return -1;
    }

    row = mysql_fetch_row(result);
    if (row != NULL) {
        file_id = atoi(row[0]);
    }

    mysql_free_result(result);
    return file_id;
}

int dao_insert_file(MYSQL *conn, const char *sha256, unsigned long file_size) {
    char query[512];
    int new_id = -1;

    if (conn == NULL || sha256 == NULL) {
        return -1;
    }

    snprintf(query, sizeof(query),
        "INSERT INTO files (sha256sum, size) VALUES ('%s', %lu)",
        sha256, file_size);

    if (dao_execute_query(conn, query) != 0) {
        return -1;
    }

    new_id = mysql_insert_id(conn);
    LOG_INFO("物理文件记录插入成功: sha256=%s, file_size=%lu, file_id=%d",
        sha256, file_size, new_id);
    return new_id;
}

int dao_insert_path(MYSQL *conn, int user_id, int parent_id, const char *filename, int file_id, const char *path) {
    char query[512];
    int new_id = -1;

    if (conn == NULL || user_id <= 0 || parent_id <= 0 || filename == NULL || file_id <= 0) {
        return -1;
    }

    snprintf(query, sizeof(query),
        "INSERT INTO paths (user_id, parent_id, name, path, type, file_id) VALUES (%d, %d, '%s', '%s', 'f', %d)",
        user_id, parent_id, filename, path, file_id);

    if (dao_execute_query(conn, query) != 0) {
        return -1;
    }

    new_id = mysql_insert_id(conn);
    LOG_INFO("虚拟路径记录插入成功: user_id=%d, parent_id=%d, filename=%s, file_id=%d, path_id=%d",
        user_id, parent_id, filename, file_id, new_id);
    return new_id;
}

/* ============ 块传输进度相关（path_to_file 表） ============ */

int dao_create_transfer_record(MYSQL *conn, int path_id, int file_id, int total_blocks, const char *status) {
    char query[512];

    if (conn == NULL || path_id <= 0 || file_id <= 0 || total_blocks <= 0 || status == NULL) {
        return -1;
    }

    snprintf(query, sizeof(query),
        "INSERT INTO path_to_file (path_id, file_id, sequence, total_blocks, transfer_status) "
        "VALUES (%d, %d, 0, %d, '%s') "
        "ON DUPLICATE KEY UPDATE sequence=0, total_blocks=%d, transfer_status='%s', updated_at=CURRENT_TIMESTAMP",
        path_id, file_id, total_blocks, status, total_blocks, status);

    if (dao_execute_query(conn, query) != 0) {
        LOG_ERROR("创建块传输记录失败: path_id=%d, file_id=%d", path_id, file_id);
        return -1;
    }

    LOG_INFO("块传输记录创建成功: path_id=%d, file_id=%d, total_blocks=%d, status=%s",
        path_id, file_id, total_blocks, status);
    return 0;
}

int dao_get_transfer_sequence(MYSQL *conn, int path_id, int *sequence) {
    char query[512];
    MYSQL_RES *result;
    MYSQL_ROW row;

    if (conn == NULL || path_id <= 0 || sequence == NULL) {
        return -1;
    }

    snprintf(query, sizeof(query),
        "SELECT sequence FROM path_to_file WHERE path_id=%d",
        path_id);

    if (dao_execute_query(conn, query) != 0) {
        return -1;
    }

    result = mysql_store_result(conn);
    if (result == NULL) {
        return -1;
    }

    row = mysql_fetch_row(result);
    if (row != NULL) {
        *sequence = atoi(row[0]);
        mysql_free_result(result);
        LOG_DEBUG("获取块传输进度: path_id=%d, sequence=%d", path_id, *sequence);
        return 0;
    }

    mysql_free_result(result);
    LOG_WARN("块传输记录不存在: path_id=%d", path_id);
    return -1;
}

int dao_update_transfer_sequence(MYSQL *conn, int path_id, int new_sequence) {
    char query[512];

    if (conn == NULL || path_id <= 0 || new_sequence < 0) {
        return -1;
    }

    snprintf(query, sizeof(query),
        "UPDATE path_to_file SET sequence=%d, updated_at=CURRENT_TIMESTAMP WHERE path_id=%d",
        new_sequence, path_id);

    if (dao_execute_query(conn, query) != 0) {
        LOG_ERROR("更新块传输进度失败: path_id=%d, new_sequence=%d", path_id, new_sequence);
        return -1;
    }

    LOG_DEBUG("块传输进度已更新: path_id=%d, new_sequence=%d", path_id, new_sequence);
    return 0;
}

int dao_complete_transfer(MYSQL *conn, int path_id) {
    char query[512];

    if (conn == NULL || path_id <= 0) {
        return -1;
    }

    snprintf(query, sizeof(query),
        "UPDATE path_to_file SET transfer_status='completed', updated_at=CURRENT_TIMESTAMP WHERE path_id=%d",
        path_id);

    if (dao_execute_query(conn, query) != 0) {
        LOG_ERROR("完成块传输失败: path_id=%d", path_id);
        return -1;
    }

    LOG_INFO("块传输已完成: path_id=%d", path_id);
    return 0;
}

int dao_cancel_transfer(MYSQL *conn, int path_id) {
    char query[512];

    if (conn == NULL || path_id <= 0) {
        return -1;
    }

    snprintf(query, sizeof(query),
        "UPDATE path_to_file SET transfer_status='cancelled', updated_at=CURRENT_TIMESTAMP WHERE path_id=%d",
        path_id);

    if (dao_execute_query(conn, query) != 0) {
        LOG_ERROR("取消块传输失败: path_id=%d", path_id);
        return -1;
    }

    LOG_INFO("块传输已取消: path_id=%d", path_id);
    return 0;
}
