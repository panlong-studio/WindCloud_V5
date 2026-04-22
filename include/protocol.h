#ifndef _PROTOCOL_H_
#define _PROTOCOL_H_

#include <sys/types.h>

// 普通命令参数统一使用固定长度数组。
#define CMD_DATA_LEN 256

// 文件名也统一使用固定长度数组
#define FILE_NAME_LEN 256

// 块传输相关定义
#define BLOCK_SIZE 10485760    // 10MB 块大小
#define MAX_BLOCK_DATA 10485760 // 块数据最大大小

// 命令类型枚举。
// 客户端发命令时写入这个编号。
// 服务端收命令时读取这个编号。
// 这样双方不需要每次都重复传完整字符串去判断命令。
typedef enum {
    CMD_TYPE_INVALID = 0, // 非法命令，或者暂时无法识别的命令
    CMD_TYPE_PWD,         // 查看当前虚拟路径
    CMD_TYPE_CD,          // 切换目录
    CMD_TYPE_LS,          // 查看目录内容
    CMD_TYPE_GETS,        // 下载文件
    CMD_TYPE_PUTS,        // 上传文件
    CMD_TYPE_RM,          // 删除文件
    CMD_TYPE_MKDIR,       // 创建目录
    CMD_TYPE_REPLY,        // 服务端返回的普通文本响应
    CMD_TYPE_LOGIN,        // 登录命令
    CMD_TYPE_REGISTER,    // 注册命令
    CMD_TYPE_LOGOUT,      // 退出登录命令
} cmd_type_t;

// 普通命令结构体。
// 这个结构体负责传输：
// 1. 普通命令的参数
// 2. 服务端返回的文本消息
typedef struct {
    int cmd_type;              // 命令类型，对应上面的 cmd_type_t
    int data_len;              // data 里真正有效的字符串长度
    char data[CMD_DATA_LEN];   // 命令参数，或者普通文本响应
} command_packet_t;

// 文件传输结构体。
// 上传和下载都需要知道文件名、总大小、断点位置。
// 所以把这些字段统一放进一个结构体里，双方协议就统一了。
typedef struct {
    int cmd_type;                  // 当前是 puts 还是 gets
    int data_len;                  // file_name 中有效的字符串长度
    off_t file_size;               // 文件总大小
    off_t offset;                  // 断点续传位置
    char file_name[FILE_NAME_LEN]; // 文件名
    char hash[65];                 // 文件内容的 SHA256 哈希值，64 字节 + 1 字节 '\0'
} file_packet_t;

// 块传输结构体
// 用于发送单个块数据
typedef struct {
    int cmd_type;                 // CMD_TYPE_GETS 或 CMD_TYPE_PUTS
    int block_index;               // 块序号（从 0 开始）
    int block_size;                // 当前块的实际大小
    off_t block_offset;            // 块在文件中的偏移
    char hash[65];                 // 文件内容的 SHA256 哈希值
    char block_data[MAX_BLOCK_DATA]; // 块数据
} block_packet_t;

// 函数作用：把命令字符串转换成命令枚举值。
// 参数 cmd_str：例如 "pwd"、"ls"、"puts"。
// 返回值：成功时返回对应的 cmd_type_t，失败返回 CMD_TYPE_INVALID。
cmd_type_t get_cmd_type(const char *cmd_str);

// 函数作用：保证把 len 个字节完整发送出去。
// 参数 fd：socket 文件描述符。
// 参数 buf：要发送的数据起始地址。
// 参数 len：本次总共要发送多少字节。
// 返回值：成功返回 0，失败返回 -1。
int send_full(int fd, const void *buf, int len);

// 函数作用：保证从 fd 中完整接收 len 个字节。
// 参数 fd：socket 文件描述符。
// 参数 buf：接收缓冲区起始地址。
// 参数 len：本次必须收满的字节数。
// 返回值：
// 1. 成功时返回实际接收到的字节数，也就是 len。
// 2. 失败或对端断开时返回 <= 0。
int recv_full(int fd, void *buf, int len);

// 函数作用：初始化普通命令结构体。
// 参数 packet：要被填写的结构体地址。
// 参数 type：命令类型。
// 参数 data：参数字符串或者响应字符串，可以传 NULL。
// 返回值：无。
void init_command_packet(command_packet_t *packet, cmd_type_t type, const char *data);

// 函数作用：初始化文件传输结构体。
// 参数 packet：要被填写的结构体地址。
// 参数 type：命令类型，一般是 CMD_TYPE_PUTS 或 CMD_TYPE_GETS。
// 参数 file_name：文件名，可以传 NULL。
// 参数 file_size：文件总大小。
// 参数 offset：断点续传位置。
// 返回值：无。
void init_file_packet(
    file_packet_t *packet, 
    cmd_type_t type, 
    const char *file_name, 
    off_t file_size, 
    off_t offset
);

// 函数作用：发送一个完整的普通命令结构体。
// 参数 fd：socket 文件描述符。
// 参数 packet：要发送的结构体地址。
// 返回值：成功返回 0，失败返回 -1。
int send_command_packet(int fd, const command_packet_t *packet);

// 函数作用：接收一个完整的普通命令结构体。
// 参数 fd：socket 文件描述符。
// 参数 packet：用于保存结果的结构体地址。
// 返回值：成功时返回接收字节数，失败返回 <= 0 或 -1。
int recv_command_packet(int fd, command_packet_t *packet);

// 函数作用：发送一个完整的文件传输结构体。
// 参数 fd：socket 文件描述符。
// 参数 packet：要发送的结构体地址。
// 返回值：成功返回 0，失败返回 -1。
int send_file_packet(int fd, const file_packet_t *packet);

// 函数作用：接收一个完整的文件传输结构体。
// 参数 fd：socket 文件描述符。
// 参数 packet：用于保存结果的结构体地址。
// 返回值：成功时返回接收字节数，失败返回 <= 0 或 -1。
int recv_file_packet(int fd, file_packet_t *packet);

#endif
