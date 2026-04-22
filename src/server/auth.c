#include <stdio.h>
#include <string.h>
#include "auth.h"
#include "dao_vfs.h"
#include "session.h"
#include "log.h"

/**
 * 解析登录/注册数据
 * 格式：username:password
 */
static int parse_auth_data(const char *data, char *username, char *password) {
    if (data == NULL || username == NULL || password == NULL) {
        return -1;
    }

    const char *sep = strchr(data, ':');
    if (sep == NULL) {
        return -1;
    }

    int user_len = sep - data;
    if (user_len <= 0 || user_len >= 64) {
        return -1;
    }

    strncpy(username, data, user_len);
    username[user_len] = '\0';

    const char *pwd_part = sep + 1;
    int pwd_len = strlen(pwd_part);
    if (pwd_len <= 0 || pwd_len >= 128) {
        return -1;
    }

    strcpy(password, pwd_part);
    return 0;
}

void handle_login(int client_fd, const char *data, int *user_id) {
    char username[64] = {0};
    char password[128] = {0};

    if (client_fd < 0 || data == NULL || user_id == NULL) {
        send_msg(client_fd, "登录参数错误");
        return;
    }

    /* 获取当前线程的数据库连接 */
    MYSQL *conn = (MYSQL*)get_thread_db_conn();
    if (conn == NULL) {
        LOG_ERROR("无法获取数据库连接，客户端fd=%d", client_fd);
        send_msg(client_fd, "服务器内部错误");
        return;
    }

    /* 解析用户名和密码 */
    if (parse_auth_data(data, username, password) != 0) {
        LOG_WARN("解析登录数据失败，客户端fd=%d，数据=%s", client_fd, data);
        send_msg(client_fd, "用户名或密码格式错误");
        return;
    }

    /* 调用数据库操作进行登录 */
    int login_user_id = dao_login_user(conn, username, password);
    
    if (login_user_id > 0) {
        *user_id = login_user_id;
        LOG_INFO("用户登录成功，客户端fd=%d，username=%s，user_id=%d", client_fd, username, login_user_id);
        send_msg(client_fd, "登录成功");
    } else if (login_user_id == 0) {
        LOG_WARN("登录失败（用户不存在或密码错误），客户端fd=%d，username=%s", client_fd, username);
        send_msg(client_fd, "用户名或密码错误");
    } else {
        LOG_ERROR("登录失败（数据库错误），客户端fd=%d，username=%s", client_fd, username);
        send_msg(client_fd, "服务器内部错误");
    }

    return;
}

void handle_register(int client_fd, const char *data, int *user_id) {
    char username[64] = {0};
    char password[128] = {0};

    if (client_fd < 0 || data == NULL || user_id == NULL) {
        send_msg(client_fd, "注册参数错误");
        return;
    }

    /* 获取当前线程的数据库连接 */
    MYSQL *conn = (MYSQL*)get_thread_db_conn();
    if (conn == NULL) {
        LOG_ERROR("无法获取数据库连接，客户端fd=%d", client_fd);
        send_msg(client_fd, "服务器内部错误");
        return;
    }

    /* 解析用户名和密码 */
    if (parse_auth_data(data, username, password) != 0) {
        LOG_WARN("解析注册数据失败，客户端fd=%d，数据=%s", client_fd, data);
        send_msg(client_fd, "用户名或密码格式错误");
        return;
    }

    /* 调用数据库操作进行注册 */
    int register_user_id = dao_register_user(conn, username, password);

    if (register_user_id > 0) {
        *user_id = register_user_id;
        LOG_INFO("用户注册成功，客户端fd=%d，username=%s，user_id=%d", client_fd, username, register_user_id);
        send_msg(client_fd, "注册成功");
    } else if (register_user_id == 0) {
        LOG_WARN("注册失败（用户已存在），客户端fd=%d，username=%s", client_fd, username);
        send_msg(client_fd, "用户已存在");
    } else {
        LOG_ERROR("注册失败（数据库错误），客户端fd=%d，username=%s", client_fd, username);
        send_msg(client_fd, "服务器内部错误");
    }
}
