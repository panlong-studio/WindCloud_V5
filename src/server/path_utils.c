#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "path_utils.h"

static const char *get_server_base_dir(void) {
    if (access(SERVER_BASE_DIR, F_OK) == 0) {
        return SERVER_BASE_DIR;
    }
    if (access("./test", F_OK) == 0) {
        return "./test";
    }
    return SERVER_BASE_DIR;
}

int check_arg_path(const char *arg) {
    if (arg == NULL || arg[0] == '\0') {
        return -1;
    }
    // 安全防护：禁止跳出根目录
    if (strstr(arg, "..") != NULL) {
        return -1;
    }
    // 安全防护：禁止客户端直接传递绝对路径，防止绕过沙箱
    if (arg[0] == '/') {
        return -1;
    }
    return 0;
}

int get_current_real_path(char *res, int size, const char *current_path) {
    const char *base_dir = get_server_base_dir();
    int ret = snprintf(res, size, "%s%s", base_dir, current_path);
    if (ret < 0 || ret >= size) {
        return -1;
    }
    return 0;
}

int get_real_path(char *res, int size, const char *path, const char *arg) {
    if (check_arg_path(arg) == -1) {
        return -1;
    }

    int ret = 0;
    const char *base_dir = get_server_base_dir();

    if (strcmp(path, "/") == 0) {
        ret = snprintf(res, size, "%s/%s", base_dir, arg);
    } else {
        ret = snprintf(res, size, "%s%s/%s", base_dir, path, arg);
    }

    if (ret < 0 || ret >= size) {
        return -1;
    }
    return 0;
}

int update_current_path(char *current_path, int size, const char *arg) {
    char new_path[512] = {0};
    int ret = 0;

    if (strcmp(current_path, "/") == 0) {
        ret = snprintf(new_path, sizeof(new_path), "/%s", arg);
    } else {
        ret = snprintf(new_path, sizeof(new_path), "%s/%s", current_path, arg);
    }

    if (ret < 0 || ret >= size) {
        return -1;
    }

    strcpy(current_path, new_path);
    return 0;
}