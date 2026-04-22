#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <errno.h>
#include "client_socket.h"
#include "config.h"
#include "client_command_handle.h"   // 新增头文件
#include "log.h"

static void load_value_or_default(const char *key, char *value, size_t value_sz, const char *default_value) {
    char tmp[256] = {0};

    if (get_target((char *)key, tmp) == 0) {
        snprintf(value, value_sz, "%s", tmp);
        return;
    }

    snprintf(value, value_sz, "%s", default_value);
}

static void init_log_with_fallback(const char *level_str, const char *log_file) {
    const char *real_log_file = log_file;

    if (log_file != NULL && strncmp(log_file, "../", 3) == 0) {
        if (access("../log", F_OK) == 0) {
            real_log_file = log_file;
        } else if (access("./log", F_OK) == 0) {
            real_log_file = log_file + 3;
        }
    }

    if (init_log(level_str, real_log_file) == 0) {
        return;
    }

    init_log(level_str, NULL);
}

// 函数作用：客户端主函数。
// 参数 argc：命令行参数个数，这里没有实际使用。
// 参数 argv：命令行参数数组，这里没有实际使用。
// 返回值：程序正常结束时返回 0。
int main(int argc, char *argv[])
{
    // 这两行只是为了消除“未使用参数”警告。
    (void)argc;
    (void)argv;

    // 准备两个字符数组，分别保存服务器 IP 和端口。
    char ip[64] = {0};
    char port[64] = {0};
    char log_level[32] = {0};
    char log_file[256] = {0};

    // 先用默认日志路径初始化，确保配置加载阶段的日志也能落盘。
    init_log_with_fallback("INFO", "../log/client.log");

    // 从配置文件中读取 IP、端口和日志参数。
    load_value_or_default("ip", ip, sizeof(ip), "127.0.0.1");
    load_value_or_default("port", port, sizeof(port), "9090");
    load_value_or_default("log", log_level, sizeof(log_level), "INFO");
    load_value_or_default("client_log", log_file, sizeof(log_file), "../log/client.log");

    // 先初始化日志。
    // 否则后面如果 connect 失败，ERROR_CHECK 里打印日志时可能没有输出目标。
    init_log_with_fallback(log_level, log_file);
    signal(SIGPIPE, SIG_IGN);

    // sock_fd 就是客户端和服务端通信用的 socket。
    int sock_fd = 0;

    // 主动连接到服务端。
    init_socket(&sock_fd, ip, port);
    LOG_INFO("客户端已连接服务器，地址=%s，端口=%s", ip, port);

    // input 用来保存用户每次输入的一整行命令。
    char input[512];

    printf("欢迎使用 WindCloud 客户端！(输入 quit 或 exit 以退出)\n");
    printf("请完成登录/注册：\n");
    printf("登录格式：login    username:password\n");
    printf("注册格式：register username:password\n");

    // 客户端进入命令循环。
    while (1) {
        // 打印命令提示符。
        printf("> ");

        // 立刻把提示符刷到终端上，避免缓冲区里还没显示。
        fflush(stdout);

        // fgets 从标准输入读一整行。
        // 如果返回 NULL，通常表示输入结束，例如按下 Ctrl+D。
        if (fgets(input, sizeof(input), stdin) == NULL) {
            LOG_INFO("客户端输入结束");
            break;
        }

        // fgets 通常会把末尾的 '\n' 一起读进来。
        // 这里把它替换成 '\0'，让字符串更方便后续处理。
        input[strcspn(input, "\n")] = '\0';

        // 用户输入 quit 或 exit 时，客户端主动退出。
        if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
            printf("再见！\n");
            LOG_INFO("客户端请求退出");
            break;
        }

        // 如果用户只输入了一个空行，就继续下一轮循环。
        if (strlen(input) == 0) {
            continue;
        }

        // 真正的命令发送、上传下载、结果接收，都交给 process_command 去做。
        LOG_DEBUG("客户端开始处理命令，输入=%s", input);
        process_command(sock_fd, input);
    }

    // 退出前关闭 socket。
    close(sock_fd);
    LOG_INFO("客户端套接字已关闭");

    // 关闭日志系统。
    close_log();
    return 0;
}
