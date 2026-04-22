#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <errno.h>
#include "file_transfer.h"
#include "dao_vfs.h"
#include "session.h"
#include "protocol.h"
#include "log.h"

#define BUFFER_SIZE 4096
#define STORAGE_DIR "./storage"

/**
 * 生成物理文件路径
 * 格式：./storage/<file_name>
 */
static void gen_physical_path(char *path, int path_len, const char *file_name) {
    snprintf(path, path_len, "%s/%s", STORAGE_DIR, file_name);
}


/**
 * 确保存储目录存在
 */
static int ensure_storage_dir(void) {
    struct stat st;
    if (stat(STORAGE_DIR, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        LOG_ERROR("存储路径 %s 存在但不是目录", STORAGE_DIR);
        return -1;
    }
    if (mkdir(STORAGE_DIR, 0755) == 0) {
        LOG_INFO("创建存储目录 %s 成功", STORAGE_DIR);
        return 0;
    }
    LOG_ERROR("创建存储目录 %s 失败: %s", STORAGE_DIR, strerror(errno));
    return -1;
}

/**
 * 计算文件的总块数
 */
static int calculate_total_blocks(unsigned long file_size) {
    return (int)((file_size + BLOCK_SIZE - 1) / BLOCK_SIZE);
}

/**
 * 发送块数据（下载）
 */
int send_block(int client_fd, int file_fd, int block_index, 
                      off_t block_offset, int block_size, const char *hash) {
    block_packet_t block_pkt;
    memset(&block_pkt, 0, sizeof(block_pkt));
    
    block_pkt.cmd_type = CMD_TYPE_GETS;
    block_pkt.block_index = block_index;
    block_pkt.block_size = block_size;
    block_pkt.block_offset = block_offset;
    strcpy(block_pkt.hash, hash);

    /* 读取块数据 */
    if (lseek(file_fd, block_offset, SEEK_SET) == -1) {
        LOG_ERROR("定位文件失败: 偏移=%lld, 错误=%d", (long long)block_offset, errno);
        return -1;
    }

    ssize_t ret = read(file_fd, block_pkt.block_data, block_size);
    if (ret != block_size) {
        LOG_ERROR("读取块数据失败: 块索引=%d, 期望=%d, 实际=%ld",
                  block_index, block_size, ret);
        return -1;
    }

    /* 计算包大小并发送 */
    int packet_size = sizeof(block_packet_t);
    if (send_full(client_fd, &block_pkt, packet_size) != 0) {
        LOG_ERROR("发送块数据失败: 块索引=%d", block_index);
        return -1;
    }

    LOG_DEBUG("块数据发送成功: 块索引=%d, 偏移=%lld, 大小=%d",
              block_index, (long long)block_offset, block_size);
    return 0;
}




// 接收块数据（上传）
// static int recv_block(int client_fd, block_packet_t *block_pkt) {
//     /* 接收整个块包 */
//     int packet_size = sizeof(block_packet_t);
//     if (recv_full(client_fd, block_pkt, packet_size) <= 0) {
//         LOG_ERROR("接收块数据失败");
//         return -1;
//     }

//     if (block_pkt->block_size <= 0 || block_pkt->block_size > MAX_BLOCK_DATA) {
//         LOG_ERROR("块大小无效: %d", block_pkt->block_size);
//         return -1;
//     }

//     LOG_DEBUG("块数据接收成功: 块索引=%d, 偏移=%lld, 大小=%d",
//               block_pkt->block_index, (long long)block_pkt->block_offset, block_pkt->block_size);
//     return 0;
// }




void handle_gets(int client_fd, int user_id, int current_dir_id, char *current_path, char *arg) {
    (void)current_path;
    if (arg == NULL || arg[0] == '\0') {
        LOG_WARN("下载命令缺少参数，客户端fd=%d", client_fd);
        file_packet_t error_packet;
        init_file_packet(&error_packet, CMD_TYPE_GETS, arg, -1, 0);
        send_file_packet(client_fd, &error_packet);
        return;
    }

    MYSQL *conn = (MYSQL*)get_thread_db_conn();
    if (conn == NULL) {
        LOG_ERROR("无法获取数据库连接，客户端fd=%d", client_fd);
        file_packet_t error_packet;
        init_file_packet(&error_packet, CMD_TYPE_GETS, arg, -1, 0);
        send_file_packet(client_fd, &error_packet);
        return;
    }

    /* 查找要下载的文件 */
    int file_path_id = dao_find_child_id(conn, current_dir_id, arg, 0);
    if (file_path_id <= 0) {
        LOG_WARN("下载文件不存在，客户端fd=%d，文件名=%s", client_fd, arg);
        file_packet_t error_packet;
        init_file_packet(&error_packet, CMD_TYPE_GETS, arg, -1, 0);
        send_file_packet(client_fd, &error_packet);
        return;
    }

    /* 获取文件信息 */
    char sha256[65] = {0};
    unsigned long file_size = 0;
    if (dao_get_file_info(conn, user_id, file_path_id, sha256, &file_size) != 0) {
        LOG_WARN("获取文件信息失败，客户端fd=%d，文件名=%s", client_fd, arg);
        file_packet_t error_packet;
        init_file_packet(&error_packet, CMD_TYPE_GETS, arg, -1, 0);
        send_file_packet(client_fd, &error_packet);
        return;
    }

    /* 构建物理文件路径 */
    char physical_path[512] = {0};
    gen_physical_path(physical_path, sizeof(physical_path), sha256);

    /* 打开物理文件 */
    int file_fd = open(physical_path, O_RDONLY);
    if (file_fd == -1) {
        LOG_WARN("打开下载文件失败，客户端fd=%d，物理路径=%s，错误码=%d", client_fd, physical_path, errno);
        file_packet_t error_packet;
        init_file_packet(&error_packet, CMD_TYPE_GETS, arg, -1, 0);
        send_file_packet(client_fd, &error_packet);
        return;
    }

    /* 计算总块数 */
    int total_blocks = calculate_total_blocks(file_size);

    /* 创建/获取块传输记录 */
    if (dao_create_transfer_record(conn, file_path_id, 0, total_blocks, "downloading") != 0) {
        LOG_WARN("创建下载进度记录失败，客户端fd=%d", client_fd);
    }

    /* 获取上次的传输进度 */
    int start_sequence = 0;
    if (dao_get_transfer_sequence(conn, file_path_id, &start_sequence) != 0) {
        start_sequence = 0;  /* 如果没有记录，从 0 开始 */
    }

    /* 发送文件信息（包含总块数） */
    file_packet_t server_file_packet;
    init_file_packet(&server_file_packet, CMD_TYPE_GETS, arg, file_size, start_sequence);
    strcpy(server_file_packet.hash, sha256);
    server_file_packet.offset = total_blocks;  /* 复用 offset 字段传递总块数 */

    if (send_file_packet(client_fd, &server_file_packet) == -1) {
        LOG_WARN("发送下载文件信息失败，客户端fd=%d，文件名=%s", client_fd, arg);
        close(file_fd);
        return;
    }

    LOG_INFO("开始下载（按块），客户端fd=%d，文件名=%s，总块数=%d，续传块=%d",
             client_fd, arg, total_blocks, start_sequence);

    /* 按块发送文件 */
    for (int block_idx = start_sequence; block_idx < total_blocks; block_idx++) {
        off_t block_offset = (off_t)block_idx * BLOCK_SIZE;
        int block_size = BLOCK_SIZE;

        /* 最后一块可能不足 BLOCK_SIZE */
        if (block_idx == total_blocks - 1) {
            block_size = (int)(file_size - block_offset);
        }

        /* 发送块 */
        if (send_block(client_fd, file_fd, block_idx, block_offset, block_size, sha256) != 0) {
            LOG_WARN("下载块发送失败，客户端fd=%d，块索引=%d", client_fd, block_idx);
            break;
        }

        /* 更新传输进度 */
        if (dao_update_transfer_sequence(conn, file_path_id, block_idx + 1) != 0) {
            LOG_WARN("更新下载进度失败，客户端fd=%d，块索引=%d", client_fd, block_idx);
        }
    }

    close(file_fd);

    /* 检查是否完成 */
    int final_sequence = 0;
    if (dao_get_transfer_sequence(conn, file_path_id, &final_sequence) == 0 &&
        final_sequence == total_blocks) {
        dao_complete_transfer(conn, file_path_id);
        LOG_INFO("下载完成，客户端fd=%d，文件名=%s，大小=%lu", client_fd, arg, file_size);
    }
}

void handle_puts(int client_fd, int user_id, int current_dir_id, char *current_path, char *arg) {
    (void)current_path;
    if (arg == NULL || arg[0] == '\0') {
        LOG_WARN("上传命令缺少参数，客户端fd=%d", client_fd);
        send_msg(client_fd, "文件名错误");
        return;
    }

    /* 确保存储目录存在 */
    if (ensure_storage_dir() != 0) {
        send_msg(client_fd, "服务器存储目录初始化失败");
        return;
    }

    MYSQL *conn = (MYSQL*)get_thread_db_conn();
    if (conn == NULL) {
        LOG_ERROR("无法获取数据库连接，客户端fd=%d", client_fd);
        send_msg(client_fd, "服务器内部错误");
        return;
    }

    /* 接收文件信息 */
    file_packet_t client_file_packet;
    if (recv_file_packet(client_fd, &client_file_packet) <= 0) {
        LOG_WARN("接收上传文件信息失败，客户端fd=%d", client_fd);
        send_msg(client_fd, "接收文件信息失败");
        return;
    }

    const char *sha256 = client_file_packet.hash;
    unsigned long file_size = client_file_packet.file_size;

    if (file_size <= 0 || strlen(sha256) != 64) {
        LOG_WARN("上传文件信息无效，客户端fd=%d，文件大小=%lu，哈希长度=%zu", 
            client_fd, file_size, strlen(sha256));
        send_msg(client_fd, "文件信息无效");
        return;
    }

    LOG_DEBUG("上传文件信息，客户端fd=%d，文件名=%s，大小=%lu，哈希=%s", 
        client_fd, arg, file_size, sha256);

    /* 生成物理文件路径 */
    char physical_path[512] = {0};
    gen_physical_path(physical_path, sizeof(physical_path), sha256);

    /* 检查文件是否已存在（秒传锚点） */
    int existing_file_id = dao_find_file_by_hash(conn, sha256);
    
    if (existing_file_id > 0) {
        /* 物理文件已存在，直接创建虚拟路径记录（秒传成功） */
        LOG_INFO("秒传触发，客户端fd=%d，文件名=%s，哈希=%s，文件ID=%d", 
            client_fd, arg, sha256, existing_file_id);

        char virtual_path[512] = {0};
        snprintf(virtual_path, sizeof(virtual_path), "/%s", arg);

        int new_path_id = dao_insert_path(conn, user_id, current_dir_id, arg, existing_file_id, virtual_path);
        if (new_path_id > 0) {
            send_msg(client_fd, "秒传成功！");
            LOG_INFO("虚拟路径记录创建成功，客户端fd=%d，path_id=%d", client_fd, new_path_id);
            return;
        } else {
            send_msg(client_fd, "虚拟路径记录创建失败");
            return;
        }
    }

    /* 物理文件不存在，需要接收数据 */
    int file_fd = open(physical_path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (file_fd == -1) {
        LOG_ERROR("创建上传文件失败，客户端fd=%d，路径=%s，错误码=%d", client_fd, physical_path, errno);
        send_msg(client_fd, "服务端创建文件失败");
        return;
    }

    /* 发送断点信息（首次上传，从 0 开始） */
    file_packet_t server_file_packet;
    init_file_packet(&server_file_packet, CMD_TYPE_PUTS, arg, file_size, 0);
    
    if (send_file_packet(client_fd, &server_file_packet) == -1) {
        LOG_WARN("发送上传断点信息失败，客户端fd=%d", client_fd);
        close(file_fd);
        return;
    }

    /* 扩展文件大小 */
    if (ftruncate(file_fd, file_size) == -1) {
        LOG_ERROR("扩展文件失败，客户端fd=%d，错误码=%d", client_fd, errno);
        send_msg(client_fd, "服务端扩展文件失败");
        close(file_fd);
        return;
    }

    /* 使用 mmap 接收数据 */
    char *map_ptr = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, file_fd, 0);
    if (map_ptr == MAP_FAILED) {
        LOG_ERROR("内存映射失败，客户端fd=%d，错误码=%d", client_fd, errno);
        send_msg(client_fd, "服务端内存映射失败");
        close(file_fd);
        return;
    }

    off_t received_count = 0;
    while (received_count < (off_t)file_size) {
        int once = BUFFER_SIZE;
        if ((off_t)file_size - received_count < BUFFER_SIZE) {
            once = (int)(file_size - received_count);
        }

        ssize_t ret = recv(client_fd, map_ptr + received_count, once, 0);
        if (ret <= 0) break;
        received_count += ret;
    }

    munmap(map_ptr, file_size);

    if (received_count < (off_t)file_size) {
        LOG_WARN("上传中断，客户端fd=%d，文件名=%s，已接收=%lld，总大小=%lu", 
            client_fd, arg, (long long)received_count, file_size);
        send_msg(client_fd, "传输中断，已保存当前进度。");
        close(file_fd);
        return;
    }

    close(file_fd);

    /* 插入物理文件记录 */
    int new_file_id = dao_insert_file(conn, sha256, file_size);
    if (new_file_id <= 0) {
        LOG_ERROR("插入物理文件记录失败，客户端fd=%d，哈希=%s", client_fd, sha256);
        send_msg(client_fd, "数据库操作失败");
        return;
    }

    /* 插入虚拟路径记录 */
    char virtual_path[512] = {0};
    snprintf(virtual_path, sizeof(virtual_path), "/%s", arg);

    int new_path_id = dao_insert_path(conn, user_id, current_dir_id, arg, new_file_id, virtual_path);
    if (new_path_id <= 0) {
        LOG_ERROR("插入虚拟路径记录失败，客户端fd=%d，文件名=%s", client_fd, arg);
        send_msg(client_fd, "数据库操作失败");
        return;
    }

    LOG_INFO("上传完成，客户端fd=%d，文件名=%s，大小=%lu，哈希=%s，path_id=%d", 
        client_fd, arg, file_size, sha256, new_path_id);
    send_msg(client_fd, "上传完成！");
}