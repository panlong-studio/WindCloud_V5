#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "file_cmds.h"
#include "dao_vfs.h"
#include "session.h"
#include "log.h"

void handle_cd(int client_fd, int user_id, int *current_dir_id, char *current_path, char *arg) {
    if (arg == NULL || arg[0] == '\0') {
        LOG_WARN("切换目录缺少参数，客户端fd=%d", client_fd);
        send_msg(client_fd, "输入错误");
        return;
    }

    MYSQL *conn = (MYSQL*)get_thread_db_conn();
    if (conn == NULL) {
        LOG_ERROR("无法获取数据库连接，客户端fd=%d", client_fd);
        send_msg(client_fd, "服务器内部错误");
        return;
    }

    // 处理 cd .
    if (strcmp(arg, ".") == 0) {
        send_msg(client_fd, "已在当前目录");
        return;
    }

    // 处理 cd ..
    if (strcmp(arg, "..") == 0) {
        int parent_id = dao_get_parent_id(conn, *current_dir_id);
        if (parent_id <= 0) {
            send_msg(client_fd, "已在根目录");
            return;
        }
        *current_dir_id = parent_id;
        // 更新虚拟路径：去掉最后一段
        char *last_slash = strrchr(current_path, '/');
        if (last_slash != NULL && last_slash != current_path) {
            *last_slash = '\0';
        } else {
            strcpy(current_path, "/");
        }
        LOG_INFO("客户端切换到上级目录，客户端fd=%d，新路径=%s", client_fd, current_path);
        send_msg(client_fd, "已返回上级目录");
        return;
    }

    if (strcmp(arg, "/") == 0) {
        /* 返回根目录 */
        int root_id = dao_get_root_dir_id(conn, user_id);
        if (root_id <= 0) {
            send_msg(client_fd, "获取根目录失败");
            return;
        }
        *current_dir_id = root_id;
        strcpy(current_path, "/");
        LOG_INFO("客户端切换回根目录，客户端fd=%d", client_fd);
        send_msg(client_fd, "已返回根目录");
        return;
    }

    /* 查找子目录 */
    int child_id = dao_find_child_id(conn, *current_dir_id, arg, 1);
    if (child_id <= 0) {
        LOG_WARN("切换目录失败，客户端fd=%d，子目录不存在，参数=%s", client_fd, arg);
        send_msg(client_fd, "目录不存在！");
        return;
    }

    *current_dir_id = child_id;
    
    /* 更新虚拟路径 */
    if (strlen(current_path) + strlen(arg) + 2 > 512) {
        LOG_WARN("切换目录路径过长，客户端fd=%d", client_fd);
        send_msg(client_fd, "路径过长");
        return;
    }
    
    if (strcmp(current_path, "/") != 0) {
        strcat(current_path, "/");
    }
    strcat(current_path, arg);

    LOG_INFO("切换目录成功，客户端fd=%d，当前路径=%s，dir_id=%d", client_fd, current_path, *current_dir_id);
    send_msg(client_fd, "进入目录成功");
}

void handle_ls(int client_fd, int user_id, int current_dir_id) {
    MYSQL *conn = (MYSQL*)get_thread_db_conn();
    if (conn == NULL) {
        LOG_ERROR("无法获取数据库连接，客户端fd=%d", client_fd);
        send_msg(client_fd, "服务器内部错误");
        return;
    }

    char result[4096] = {0};
    if (dao_list_dir(conn, user_id, current_dir_id, result, sizeof(result)) != 0) {
        LOG_WARN("列目录失败，客户端fd=%d，dir_id=%d", client_fd, current_dir_id);
        send_msg(client_fd, "目录列出失败");
        return;
    }

    LOG_INFO("列目录成功，客户端fd=%d，路径=%s", client_fd, result);
    send_msg(client_fd, result);
}

void handle_pwd(int client_fd, char *current_path) {
    LOG_INFO("处理显示当前路径命令，客户端fd=%d，当前路径=%s", client_fd, current_path);
    send_msg(client_fd, current_path);
}

void handle_rm(int client_fd, int user_id, int current_dir_id, char *arg) {
    if (arg == NULL || arg[0] == '\0') {
        LOG_WARN("删除命令缺少参数，客户端fd=%d", client_fd);
        send_msg(client_fd, "输入错误");
        return;
    }

    MYSQL *conn = (MYSQL*)get_thread_db_conn();
    if (conn == NULL) {
        LOG_ERROR("无法获取数据库连接，客户端fd=%d", client_fd);
        send_msg(client_fd, "服务器内部错误");
        return;
    }

    /* 查找要删除的条目 */
    int path_id = dao_find_child_id(conn, current_dir_id, arg, -1);
    if (path_id <= 0) {
        LOG_WARN("删除失败，客户端fd=%d，条目不存在，参数=%s", client_fd, arg);
        send_msg(client_fd, "条目不存在");
        return;
    }

    if (dao_delete_entry(conn, user_id, path_id) != 0) {
        LOG_WARN("删除失败，客户端fd=%d，path_id=%d", client_fd, path_id);
        send_msg(client_fd, "删除失败");
        return;
    }

    LOG_INFO("删除成功，客户端fd=%d，path_id=%d", client_fd, path_id);
    send_msg(client_fd, "删除成功");
}

void handle_mkdir(int client_fd, int user_id, int current_dir_id, char *arg) {
    if (arg == NULL || arg[0] == '\0') {
        LOG_WARN("创建目录命令缺少参数，客户端fd=%d", client_fd);
        send_msg(client_fd, "输入错误");
        return;
    }

    MYSQL *conn = (MYSQL*)get_thread_db_conn();
    if (conn == NULL) {
        LOG_ERROR("无法获取数据库连接，客户端fd=%d", client_fd);
        send_msg(client_fd, "服务器内部错误");
        return;
    }

    int new_dir_id = dao_create_dir(conn, user_id, current_dir_id, arg);
    if (new_dir_id < 0) {
        LOG_WARN("创建目录失败（数据库错误），客户端fd=%d，参数=%s", client_fd, arg);
        send_msg(client_fd, "文件夹创建失败");
        return;
    }
    
    if (new_dir_id == 0) {
        LOG_WARN("创建目录失败（目录已存在），客户端fd=%d，参数=%s", client_fd, arg);
        send_msg(client_fd, "目录已存在");
        return;
    }

    LOG_INFO("创建目录成功，客户端fd=%d，参数=%s，new_dir_id=%d", client_fd, arg, new_dir_id);
    send_msg(client_fd, "创建文件夹成功");
}