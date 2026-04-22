#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include "client_command_handle.h"
#include "protocol.h"
#include "sha256_utils.h"
#include "log.h"

#define BUFFER_SIZE 4096

// 函数作用：把数据完整写入本地普通文件。
// 参数 fd：本地文件描述符。
// 参数 buf：要写入的数据起始地址。
// 参数 len：本次需要写入的总字节数。
// 返回值：成功返回 0，失败返回 -1。
static int write_file_full(int fd, const char *buf, int len) {
    int total = 0;  // total 记录已经成功写入了多少字节

    // 只要还有字节没写完，就继续写。
    while (total < len) {
        // buf + total 表示“从还没写入的位置继续写”。
        int ret = write(fd, buf + total, len - total);

        // ret <= 0 说明 write 失败。
        if (ret <= 0) {
            return -1;
        }

        // 把本次写成功的字节数累计起来。
        total += ret;
    }

    return 0;
}

// 函数作用：接收服务端返回的普通文本响应。
// 参数 sock_fd：客户端和服务端通信的 socket。
// 返回值：无。
static void recv_server_reply(int sock_fd) {
    // reply_packet 用来保存服务端发回来的文本结构体。
    command_packet_t reply_packet;

    // 按固定结构体大小完整接收。
    if (recv_command_packet(sock_fd, &reply_packet) <= 0) {
        printf("接收服务端响应失败\n");
        LOG_WARN("接收服务端响应失败，套接字=%d", sock_fd);
        return;
    }

    // 把服务端返回的文本直接打印出来。
    printf("%s\n", reply_packet.data);
    LOG_DEBUG("收到服务端响应，套接字=%d，消息=%s", sock_fd, reply_packet.data);
}

// 函数作用：处理用户输入的一整条命令。
// 参数 sock_fd：客户端 socket。
// 参数 input：用户输入的一整行命令，例如 "puts a.txt"。
// 返回值：无。
void process_command(int sock_fd, const char *input) {
    // cmd 保存命令字，例如 puts、gets、ls。
    char cmd[100] = {0};

    // arg 保存参数，例如文件名或目录名。
    char arg[200] = {0};

    // 按 命令字 + 参数 的格式拆分input字符串
    sscanf(input, "%99s %199s", cmd, arg);

    // gets 没带文件名时，客户端直接提示，不发给服务端。
    if (strcmp(cmd, "gets") == 0 && strlen(arg) == 0) {
        printf("用法: gets <文件名>\n");
        return;
    }

    // puts 没带文件名时，客户端直接提示，不发给服务端。
    if (strcmp(cmd, "puts") == 0 && strlen(arg) == 0) {
        printf("用法: puts <文件名>\n");
        return;
    }

    // cd、rm、mkdir 也都需要参数。
    if ((strcmp(cmd, "cd") == 0 || strcmp(cmd, "rm") == 0 || strcmp(cmd, "mkdir") == 0) && strlen(arg) == 0) {
        printf("该命令需要参数\n");
        return;
    }

    // 把字符串命令转换成枚举命令。
    // 后面客户端和服务端通信，都靠这个编号判断命令类型。
    cmd_type_t cmd_type = get_cmd_type(cmd);
    if (cmd_type == CMD_TYPE_INVALID) {
        printf("无效命令\n");
        LOG_WARN("无效命令，输入=%s", input);
        return;
    }

    // ------------------------------
    // 第一类：处理 gets 下载命令
    // ------------------------------
    if (cmd_type == CMD_TYPE_GETS) {
        LOG_INFO("客户端请求下载文件，文件=%s", arg);
        // 先准备一个普通命令结构体，告诉服务端“我要下载哪个文件”。
        command_packet_t cmd_packet;

        // 这里把命令类型写成 CMD_TYPE_GETS，把文件名写进 data。
        init_command_packet(&cmd_packet, cmd_type, arg);

        // 先把下载命令发给服务端。
        if (send_command_packet(sock_fd, &cmd_packet) == -1) {
            printf("发送命令失败\n");
            LOG_ERROR("发送下载命令失败，文件=%s", arg);
            return;
        }

        // server_file_packet 用来接收服务端返回的文件信息。
        file_packet_t server_file_packet;

        // 服务端会先告诉客户端：
        // 1. 文件是否存在
        // 2. 文件总大小是多少
        // 3. 当前下载的是哪个文件
        if (recv_file_packet(sock_fd, &server_file_packet) <= 0) {
            printf("接收文件信息失败\n");
            LOG_WARN("接收下载文件信息失败，文件=%s", arg);
            return;
        }

        // file_size < 0 说明服务端没有这个文件。
        if (server_file_packet.file_size < 0) {
            printf("服务器文件不存在\n");
            LOG_WARN("服务端文件不存在，文件=%s", arg);
            return;
        }

        // st 用来接收本地文件状态信息。
        struct stat st;

        // local_size 表示本地当前已经有多少字节。
        off_t local_size = 0;

        // request_offset 表示客户端这次打算从哪里开始继续下载。
        off_t request_offset = 0;

        // local_file_exists 用来标记“本地有没有这个文件”。
        int local_file_exists = 0;

        // 如果本地已经有同名文件，就把它当前大小读出来。
        if (stat(arg, &st) == 0) {
            local_size = st.st_size;
            local_file_exists = 1;
        }

        // 这里分 3 种情况：
        // 1. 本地文件更小：说明可以续传，从 local_size 继续。
        // 2. 本地文件和服务端一样大：说明已经下完整了。
        // 3. 本地文件比服务端还大：说明本地旧文件不可信，从 0 重下。
        if (local_size < server_file_packet.file_size) {
            request_offset = local_size;
        } else if (local_file_exists && local_size == server_file_packet.file_size) {
            request_offset = server_file_packet.file_size;
        } else {
            request_offset = 0;
        }
        LOG_DEBUG("下载断点信息，文件=%s，本地大小=%lld，服务端大小=%lld，偏移=%lld",
                  arg,
                  (long long)local_size,
                  (long long)server_file_packet.file_size,
                  (long long)request_offset);

        // client_file_packet 用来把“客户端本地已有多少字节”发回服务端。
        file_packet_t client_file_packet;

        // 这里 file_size 仍然写服务端总大小，offset 则写客户端想从哪里继续下载。
        init_file_packet(&client_file_packet,
                         CMD_TYPE_GETS,
                         arg,
                         server_file_packet.file_size,
                         request_offset);

        // 把续传位置发给服务端。
        if (send_file_packet(sock_fd, &client_file_packet) == -1) {
            printf("发送续传位置失败\n");
            LOG_WARN("发送下载断点位置失败，文件=%s", arg);
            return;
        }

        // 如果本地文件原本就完整，那么告诉服务端 offset = file_size 后，
        // 这里直接结束当前命令即可。
        if (local_file_exists && request_offset == server_file_packet.file_size) {
            printf("文件已存在且完整，无需下载。\n");
            LOG_INFO("下载已跳过，本地文件完整，文件=%s，大小=%lld", arg, (long long)server_file_packet.file_size);
            return;
        }

        // 下面开始真正准备接收文件内容。
        // O_WRONLY | O_CREAT 表示：
        // 1. 只写打开
        // 2. 文件不存在就创建
        char path[512];
        snprintf(path, sizeof(path), "./downloads/%s", arg);
        int fd = open(path, O_WRONLY | O_CREAT, 0666);
        if (fd == -1) {
            perror("创建文件失败");
            LOG_ERROR("创建本地文件失败，文件=%s，错误码=%d", arg, errno);
            return;
        }

        // 如果 request_offset == 0，说明这次不是续传，而是重新下载。
        // 所以需要先把旧文件截断清空。
        if (request_offset == 0) {
            if (ftruncate(fd, 0) == -1) {
                perror("清空旧文件失败");
                LOG_ERROR("截断本地文件失败，文件=%s，错误码=%d", arg, errno);
                close(fd);
                return;
            }
        }

        // 把文件读写位置移动到断点处。
        // 这样后面收到的新数据就会直接写到断点之后。
        if (lseek(fd, request_offset, SEEK_SET) == -1) {
            perror("移动文件指针失败");
            LOG_ERROR("定位本地文件失败，文件=%s，偏移=%lld，错误码=%d",
                      arg,
                      (long long)request_offset,
                      errno);
            close(fd);
            return;
        }

        // buf 是每一轮接收文件数据的小缓冲区。
        char buf[BUFFER_SIZE];

        // remaining 表示还剩多少字节没下载。
        off_t remaining = server_file_packet.file_size - request_offset;

        // 只要还有剩余字节，就一轮轮接收。
        while (remaining > 0) {
            // once 表示这一轮最多接收多少字节。
            int once = BUFFER_SIZE;

            // 最后一轮可能不足 4096，所以要取剩余值。
            if (remaining < BUFFER_SIZE) {
                once = (int)remaining;
            }

            // recv_full 的意思是：这一轮要求必须收满 once 个字节。
            if (recv_full(sock_fd, buf, once) <= 0) {
                printf("下载中断，已经保留当前进度。\n");
                LOG_WARN("下载中断，文件=%s，已保存=%lld，总大小=%lld",
                         arg,
                         (long long)(server_file_packet.file_size - remaining),
                         (long long)server_file_packet.file_size);
                close(fd);
                return;
            }

            // 把本轮收到的数据完整写进本地文件。
            if (write_file_full(fd, buf, once) == -1) {
                perror("写入本地文件失败");
                LOG_ERROR("写入本地文件失败，文件=%s，错误码=%d", arg, errno);
                close(fd);
                return;
            }

            // 每写完一轮，就把 remaining 减掉本轮大小。
            remaining -= once;
        }

        // 文件收完后关闭本地文件。
        close(fd);

        // 打印下载成功信息。
        printf("下载成功: %s (%ld 字节)\n", arg, (long)server_file_packet.file_size);
        LOG_INFO("下载成功，文件=%s，大小=%lld", arg, (long long)server_file_packet.file_size);
        return;
    }

    // ------------------------------
    // 第二类：处理 puts 上传命令
    // ------------------------------
    if (cmd_type == CMD_TYPE_PUTS) {
        LOG_INFO("客户端请求上传文件，文件=%s", arg);
        // 上传前先打开本地文件。
        // 如果本地文件都打不开，后面根本没必要再和服务端继续握手。
        int fd = open(arg, O_RDONLY);
        if (fd == -1) {
            perror("打开文件失败");
            LOG_WARN("打开本地上传文件失败，文件=%s，错误码=%d", arg, errno);
            return;
        }

        // st 用来读取本地文件信息。
        struct stat st;
        if (fstat(fd, &st) == -1) {
            perror("获取文件大小失败");
            LOG_ERROR("读取本地上传文件信息失败，文件=%s，错误码=%d", arg, errno);
            close(fd);
            return;
        }

        // 先发一个普通命令结构体，告诉服务端“我接下来要执行 puts”。
        command_packet_t cmd_packet;
        init_command_packet(&cmd_packet, cmd_type, arg);
        if (send_command_packet(sock_fd, &cmd_packet) == -1) {
            printf("发送命令失败\n");
            LOG_ERROR("发送上传命令失败，文件=%s", arg);
            close(fd);
            return;
        }

        // 再发一个文件传输结构体，告诉服务端文件总大小、文件名和 SHA256 哈希。
        file_packet_t client_file_packet;
        init_file_packet(&client_file_packet, CMD_TYPE_PUTS, arg, st.st_size, 0);

        char sha256[65] = {0};
        if (get_file_sha256(arg, sha256) != 0) {
            printf("计算文件 SHA256 失败\n");
            LOG_ERROR("计算上传文件 SHA256 失败，文件=%s", arg);
            close(fd);
            return;
        }
        strncpy(client_file_packet.hash, sha256, sizeof(client_file_packet.hash) - 1);
        client_file_packet.hash[sizeof(client_file_packet.hash) - 1] = '\0';

        if (send_file_packet(sock_fd, &client_file_packet) == -1) {
            printf("发送文件信息失败\n");
            LOG_WARN("发送上传文件信息失败，文件=%s", arg);
            close(fd);
            return;
        }

        // 下面等服务端返回“它那边已经收到多少字节了”。
        file_packet_t server_file_packet;
        if (recv_file_packet(sock_fd, &server_file_packet) <= 0) {
            printf("接收服务端断点信息失败\n");
            LOG_WARN("接收上传断点位置失败，文件=%s", arg);
            close(fd);
            return;
        }

        // 如果服务端返回的 offset 比本地文件还大，说明这个值不可信，直接从 0 开始发。
        if (server_file_packet.offset > st.st_size) {
            server_file_packet.offset = 0;
        }
        LOG_DEBUG("上传断点信息，文件=%s，本地大小=%lld，偏移=%lld",
                  arg,
                  (long long)st.st_size,
                  (long long)server_file_packet.offset);

        // 把本地文件读位置移动到服务端要求的断点。
        if (lseek(fd, server_file_packet.offset, SEEK_SET) == -1) {
            perror("移动文件指针失败");
            LOG_ERROR("定位本地上传文件失败，文件=%s，偏移=%lld，错误码=%d",
                      arg,
                      (long long)server_file_packet.offset,
                      errno);
            close(fd);
            return;
        }

        // buf 用来临时存放每一轮 read 出来的数据。
        char buf[BUFFER_SIZE];

        // remaining 表示还剩多少字节没有上传。
        off_t remaining = st.st_size - server_file_packet.offset;

        // 只要还有剩余字节，就继续发。
        while (remaining > 0) {
            // once 表示这一轮最多读多少字节。
            int once = BUFFER_SIZE;
            if (remaining < BUFFER_SIZE) {
                once = (int)remaining;
            }

            // 先从本地文件读一块数据到 buf。
            int nread = read(fd, buf, once);

            // nread <= 0 表示读文件失败，或者读到了文件结尾。
            if (nread <= 0) {
                break;
            }

            // 再把这块数据完整发给服务端。
            // 这里必须用 send_full，不能只调一次 send。
            if (send_full(sock_fd, buf, nread) == -1) {
                printf("上传中断，已经发送的内容由服务端自己保留。\n");
                LOG_WARN("上传中断，文件=%s，已发送=%lld，总大小=%lld",
                         arg,
                         (long long)(st.st_size - remaining),
                         (long long)st.st_size);
                close(fd);
                return;
            }

            // remaining 减去本轮已经成功发出的字节数。
            remaining -= nread;
        }

        // 上传结束后先关本地文件。
        close(fd);

        // 再接收服务端的最终提示，例如“上传完成”。
        recv_server_reply(sock_fd);
        LOG_INFO("上传成功，文件=%s", arg);
        return;
    }

    // ------------------------------
    // 第三类：处理普通命令
    // ------------------------------

    // 普通命令只需要发一个 command_packet_t 就够了。
    command_packet_t cmd_packet;
    init_command_packet(&cmd_packet, cmd_type, arg);

    if (send_command_packet(sock_fd, &cmd_packet) == -1) {
        printf("发送命令失败\n");
        LOG_WARN("发送命令失败，输入=%s", input);
        return;
    }

    // 普通命令通常都会收到一条文本响应。
    recv_server_reply(sock_fd);
}
