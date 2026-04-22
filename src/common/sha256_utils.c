#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "sha256_utils.h"
#include "log.h"

int get_file_sha256(const char *file_path, char *sha256_out) {
    
    char command[512]={0};

    snprintf(command,sizeof(command),"sha256sum %s",file_path);
    
    FILE *pipe = popen(command, "r");
    if(pipe == NULL) {
        LOG_ERROR("执行 sha256sum 命令获取文件 %s 的 SHA256 值失败", file_path);
        return -1; 
    }

    char buf[1024]={0};

    if(fgets(buf, sizeof(buf), pipe) != NULL) {
       strncpy(sha256_out, buf, 64);
       sha256_out[64] = '\0';
    }else{
        LOG_ERROR("读取 sha256sum 命令输出 文件 %s 的 SHA256 值失败", file_path);
        pclose(pipe);
        return -1; 
    }

    pclose(pipe);
    return 0; 
}

int sha256_hash(const char *input, char *sha256_out) {
    char tmp_path[] = "/tmp/sha256tmpXXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd == -1) {
        LOG_ERROR("创建临时文件失败，用于计算 SHA256");
        return -1;
    }

    size_t len = strlen(input);
    if (write(fd, input, len) != (ssize_t)len) {
        LOG_ERROR("写入临时文件失败，用于计算 SHA256");
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);

    char command[512] = {0};
    snprintf(command, sizeof(command), "sha256sum %s", tmp_path);

    FILE *pipe = popen(command, "r");
    if (pipe == NULL) {
        LOG_ERROR("执行 sha256sum 命令计算输入字符串 SHA256 失败");
        unlink(tmp_path);
        return -1;
    }

    char buf[1024] = {0};
    if (fgets(buf, sizeof(buf), pipe) != NULL) {
        strncpy(sha256_out, buf, 64);
        sha256_out[64] = '\0';
    } else {
        LOG_ERROR("读取 sha256sum 命令输出失败");
        pclose(pipe);
        unlink(tmp_path);
        return -1;
    }

    pclose(pipe);
    unlink(tmp_path);
    return 0;
}
