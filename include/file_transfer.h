#ifndef _FILE_TRANSFER_H_
#define _FILE_TRANSFER_H_

// 函数作用：处理 gets 命令，也就是下载文件。
// 参数 client_fd：客户端连接 fd。
// 参数 user_id：用户 ID。
// 参数 current_dir_id：当前目录 ID。
// 参数 current_path：当前虚拟路径。
// 参数 arg：要下载的文件名。
// 返回值：无。
void handle_gets(int client_fd, int user_id, int current_dir_id, char *current_path, char *arg);

// 函数作用：处理 puts 命令，也就是上传文件。
// 参数 client_fd：客户端连接 fd。
// 参数 user_id：用户 ID。
// 参数 current_dir_id：当前目录 ID。
// 参数 current_path：当前虚拟路径。
// 参数 arg：要上传后的目标文件名。
// 返回值：无。
void handle_puts(int client_fd, int user_id, int current_dir_id, char *current_path, char *arg);


#endif
