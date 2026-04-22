#ifndef _FILE_CMDS_H_
#define _FILE_CMDS_H_

void handle_cd(int client_fd, int user_id, int *current_dir_id, char *current_path, char *arg);
void handle_ls(int client_fd, int user_id, int current_dir_id);
void handle_pwd(int client_fd, char *current_path);
void handle_rm(int client_fd, int user_id, int current_dir_id, char *arg);
void handle_mkdir(int client_fd, int user_id, int current_dir_id, char *arg);

#endif