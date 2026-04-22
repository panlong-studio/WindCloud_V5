#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "client_socket.h"
#include "error_check.h"
#include "log.h"

// 函数作用：创建客户端 socket，并主动连接到服务端。
// 参数 listen_fd：输出参数，用来保存创建好的 socket fd。
// 参数 ip：服务端 IP 地址字符串。
// 参数 port：服务端端口字符串。
// 返回值：无。
void init_socket(int* listen_fd,char* ip,char* port){
    // 创建 TCP socket。
    *listen_fd=socket(AF_INET, SOCK_STREAM, 0);
    ERROR_CHECK(*listen_fd, -1, "创建客户端套接字");
    LOG_INFO("客户端套接字创建成功，描述符=%d", *listen_fd);

    // sockaddr_in 用来保存 IPv4 地址信息。
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    // AF_INET 表示 IPv4。
    addr.sin_family = AF_INET;

    // inet_addr 把字符串 IP 转成网络字节序整数。
    addr.sin_addr.s_addr = inet_addr(ip);

    // htons 把主机字节序端口转成网络字节序端口。
    addr.sin_port = htons(atoi(port));

    // connect 会发起 TCP 三次握手，主动连接服务端。
    int ret = connect(*listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    ERROR_CHECK(ret, -1, "连接服务端");
    LOG_INFO("客户端连接成功，地址=%s，端口=%s", ip, port);
}

