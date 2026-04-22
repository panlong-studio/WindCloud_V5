#include <stdio.h>
#include <string.h>
#include "session.h"
#include "protocol.h"
#include "log.h"
#include "file_cmds.h"
#include "file_transfer.h"
#include "auth.h"
#include "dao_vfs.h"

void send_msg(int client_fd, const char *msg) {
    command_packet_t reply_packet;
    init_command_packet(&reply_packet, CMD_TYPE_REPLY, msg);

    if (send_command_packet(client_fd, &reply_packet) == -1) {
        LOG_WARN("发送响应失败，客户端fd=%d，消息=%s", client_fd, msg);
        return;
    }
    LOG_DEBUG("响应发送成功，客户端fd=%d，消息=%s", client_fd, msg);
}

static cmd_type_t get_packet_cmd_type(const command_packet_t *cmd_packet) {
    if (cmd_packet == NULL) {
        return CMD_TYPE_INVALID;
    }
    return (cmd_type_t)cmd_packet->cmd_type;
}

void handle_request(int client_fd) {
    char current_path[512] = "/";
    int current_user_id = -1;
    int current_dir_id = -1;

    while (1) {
        command_packet_t cmd_packet;

        if (recv_command_packet(client_fd, &cmd_packet) <= 0) {
            LOG_INFO("客户端连接断开，客户端fd=%d，当前路径=%s", client_fd, current_path);
            break;
        }

        cmd_type_t cmd_type = get_packet_cmd_type(&cmd_packet);
        LOG_DEBUG("收到客户端命令，客户端fd=%d，命令类型=%d，数据=%s", client_fd, cmd_type, cmd_packet.data);

        if (current_user_id == -1 && cmd_type != CMD_TYPE_LOGIN && cmd_type != CMD_TYPE_REGISTER && cmd_type != CMD_TYPE_LOGOUT) {
            LOG_WARN("未登录用户尝试执行命令，客户端fd=%d，命令类型=%d", client_fd, cmd_type);
            send_msg(client_fd, "请先登录!");
            continue;
        }

        switch (cmd_type) {
            case CMD_TYPE_LOGIN:
                handle_login(client_fd, cmd_packet.data, &current_user_id);
                /* 登录成功后初始化当前目录 ID */
                if (current_user_id > 0) {
                    if (current_dir_id == -1) {
                        MYSQL *conn = (MYSQL*)get_thread_db_conn();
                        if (conn != NULL) {
                            current_dir_id = dao_get_root_dir_id(conn, current_user_id);
                            if (current_dir_id <= 0) {
                                /* 创建根目录 */
                                current_dir_id = dao_init_root_dir(conn, current_user_id);
                            }
                        }
                    }
                }
                break;

            case CMD_TYPE_REGISTER:
                handle_register(client_fd, cmd_packet.data, &current_user_id);
                /* 注册成功后初始化当前目录 ID */
                if (current_user_id > 0) {
                    MYSQL *conn = (MYSQL*)get_thread_db_conn();
                    if (conn != NULL) {
                        current_dir_id = dao_get_root_dir_id(conn, current_user_id);
                        if (current_dir_id <= 0) {
                            current_dir_id = dao_init_root_dir(conn, current_user_id);
                        }
                    }
                }
                break;

            case CMD_TYPE_LOGOUT:
                current_user_id = -1;
                current_dir_id = -1;
                strcpy(current_path, "/");
                send_msg(client_fd, "已退出登录");
                break;

            case CMD_TYPE_PWD:
                handle_pwd(client_fd, current_path);
                break;

            case CMD_TYPE_CD:
                handle_cd(client_fd, current_user_id, &current_dir_id, current_path, cmd_packet.data);
                break;

            case CMD_TYPE_LS:
                handle_ls(client_fd, current_user_id, current_dir_id);
                break;

            case CMD_TYPE_GETS:
                handle_gets(client_fd, current_user_id, current_dir_id, current_path, cmd_packet.data);
                break;

            case CMD_TYPE_PUTS:
                handle_puts(client_fd, current_user_id, current_dir_id, current_path, cmd_packet.data);
                break;

            case CMD_TYPE_RM:
                handle_rm(client_fd, current_user_id, current_dir_id, cmd_packet.data);
                break;

            case CMD_TYPE_MKDIR:
                handle_mkdir(client_fd, current_user_id, current_dir_id, cmd_packet.data);
                break;

            default:
                LOG_WARN("命令类型无效，客户端fd=%d，命令类型=%d", client_fd, cmd_type);
                send_msg(client_fd, "指令错误!");
                break;
        }
    }
}