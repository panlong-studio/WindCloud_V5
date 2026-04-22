#ifndef _PATH_UTILS_H_
#define _PATH_UTILS_H_

#define SERVER_BASE_DIR "../test"
#define MAX_PATH_LEN 1024

// 函数作用：检查用户传来的路径参数是否基本合法
int check_arg_path(const char *arg);

// 函数作用：把当前虚拟路径转换成服务器真实路径
int get_current_real_path(char *res, int size, const char *current_path);

// 函数作用：把“当前虚拟路径 + 参数”拼成最终真实路径
int get_real_path(char *res, int size, const char *path, const char *arg);

// 函数作用：更新客户端看到的虚拟路径
int update_current_path(char *current_path, int size, const char *arg);

#endif