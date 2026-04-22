#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <errno.h>
#include "queue.h"
#include "thread_pool.h"
#include "worker.h"
#include "epoll.h"
#include "server_socket.h"
#include "config.h"
#include "error_check.h"
#include "log.h"
#include "dao_vfs.h"


int pipe_fd[2];

//父进程的SIGINT信号处理函数func
void func(int num){
    (void)num;
    write(pipe_fd[1],"1",1);  //随便发一个字节，通知子进程退出
}

// 加载config.ini里的配置项，如果配置文件里没有，就用默认值
static void load_value_or_default(const char *key, char *value, size_t value_sz, const char *default_value) {
    char tmp[256] = {0};
    if (get_target((char *)key, tmp) == 0) {    //根据Key找到tmp
        snprintf(value, value_sz, "%s", tmp);   //将tmp的内容复制到value中，value_sz是value的大小，防止溢出
        return;
    }
    snprintf(value, value_sz, "%s", default_value);  //如果配置文件里没有这个 key，就用默认值
}

//在调用init_log前增加一个判断log路径存在的壳子
static void init_log_with_fallback(const char *level_str, const char *log_file) {
    const char *real_log_file = log_file;

    if (log_file != NULL && strncmp(log_file, "../", 3) == 0) {
        if (access("../log", F_OK) == 0) {      //access判断文件目录是否存在
            real_log_file = log_file;
        } else if (access("./log", F_OK) == 0) {  //如果 ../log 不存在但 ./log 存在，则使用 ./log 目录
            real_log_file = log_file + 3;         //+3跳过 "../" 前缀
        }
    }

    if (init_log(level_str, real_log_file) == 0) {  //用正常路径初始化日志成功，直接返回
        return;
    }

    init_log(level_str, NULL);    //init_log("info", NULL); → 输出到控制台（标准输出 stdout）
}


int main(){
    // 忽略 SIGPIPE,send/recv时对方断连就会发送SIGPIPE信号，默认是直接让进程退出
    signal(SIGPIPE, SIG_IGN);

    char ip[64] = {0}; 
    char port[64] = {0};
    char log_level[32] = {0};
    char log_file[256] = {0};

    init_log_with_fallback("INFO", "../log/server.log");

    // 加载配置项，如果配置文件里没有，就用默认值
    load_value_or_default("ip", ip, sizeof(ip), "127.0.0.1");
    load_value_or_default("port", port, sizeof(port), "9091");
    load_value_or_default("log", log_level, sizeof(log_level), "INFO");
    load_value_or_default("server_log", log_file, sizeof(log_file), "../log/server.log");

    // 优先初始化日志，否则 socket/bind/accept 等调用一旦失败，ERROR_CHECK 无法安全打印日志
    init_log_with_fallback(log_level, log_file);
    LOG_INFO("服务端配置加载完成，地址=%s，端口=%s", ip, port);

    if (pipe(pipe_fd) != 0) {
        LOG_ERROR("创建管道失败: %s", strerror(errno));
        close_log();
        return 1;
    }
    
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("创建子进程失败: %s", strerror(errno));
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        close_log();
        return 1;
    }

    if(pid != 0){
        signal(SIGINT, func);
        LOG_INFO("服务端父进程等待子进程退出，子进程pid=%d", (int)pid);
        wait(NULL);
        LOG_INFO("服务端父进程退出");
        exit(0);
    }

    if (setpgid(0, 0) != 0) {
        LOG_WARN("设置进程组失败 errno=%d", errno);
    }

    int listen_fd = 0;
    init_socket(&listen_fd, ip, port);


    // 初始化数据库连接参数
    char db_host[64] = {0};
    char db_user[64] = {0};
    char db_password[128] = {0};
    char db_name[64] = {0};
    load_value_or_default("db_host", db_host, sizeof(db_host), "127.0.0.1");
    load_value_or_default("db_user", db_user, sizeof(db_user), "root");
    load_value_or_default("db_password", db_password, sizeof(db_password), "root");
    load_value_or_default("db_name", db_name, sizeof(db_name), "windcloud");
    
    //设置数据库连接参数到全局变量，供 dao_vfs.c 使用
    dao_set_connection_params(db_host, db_user, db_password, db_name);
    

    // 最小连接数 = 工作线程数 + 2（冗余）
    // 最小连接数是最大线程数的一半
    int min_pool_size = 5 + 2;
    int max_pool_size = min_pool_size/ 2;
    //初始化数据库连接池
    if (dao_init_with_pool(min_pool_size, max_pool_size) != 0) {
        LOG_ERROR("数据库连接池初始化失败");
        close_log();
        return 1;
    }

    thread_pool_t pool;
    init_thread_pool(&pool, 5);

    int epfd = epoll_create(1);
    ERROR_CHECK(epfd, -1, "epoll_create");
    add_epoll_fd(epfd, listen_fd);
    add_epoll_fd(epfd, pipe_fd[0]);

    LOG_INFO("服务端启动成功，地址=%s，端口=%s", ip, port);

    while(1){
        struct epoll_event lst[10];
        int nready = epoll_wait(epfd, lst, 10, -1);  //lst用来存放就绪事件，10是lst的大小，-1表示一直等下去直到有事件发生
        if (nready == -1) {
            if (errno == EINTR) {
                continue;
            }
            ERROR_CHECK(nready, -1, "epoll_wait");
        }
    
        for(int idx = 0; idx < nready; idx++){
            // 取出当前就绪的 fd
            int fd = lst[idx].data.fd;

            //判断是监听 socket 还是管道 fd
            if(fd == pipe_fd[0]){
                char buf[10];
                read(fd, buf, sizeof(buf));
                LOG_INFO("服务端收到退出信号");

                pthread_mutex_lock(&pool.lock);
                pool.exitFlag = 1;

                // 先把队列里还没处理的连接全部清掉,否则线程被唤醒后，可能还会继续拿旧任务
                while(pool.queue.size > 0){
                    int client_fd = deQueue(&pool.queue);
                    if(client_fd != -1){
                        shutdown(client_fd, SHUT_RDWR);  //使用 shutdown(fd, SHUT_WR) 会立即发送 FIN，告诉对端“本端不会再发送数据”，
                                                         //但对端仍可发送数据过来;这样对端可以继续发送完剩余数据，本端也能接收直到对端关闭
                                                         //shutdown(fd, SHUT_RDWR) 会强制让阻塞的 recv 立即返回 0（对端关闭）或 -1（错误），从而线程可以检查退出标志并安全退出                      close(client_fd);
                    }
                }

                // 再把每个工作线程当前正在处理的连接主动 shutdown
                for(int i = 0; i < pool.num; i++){
                    if(pool.busy_fds[i] != -1){
                        shutdown(pool.busy_fds[i], SHUT_RDWR);// 强制让工作线程的 recv 返回
                    }
                }

                pthread_cond_broadcast(&pool.cond);
                pthread_mutex_unlock(&pool.lock);

                for(int i = 0; i < pool.num; i++){
                    pthread_join(pool.thread_id_arr[i], NULL);
                }

                // 最后关闭日志
                close_log();

                pthread_exit((void*)NULL);
            }

            if(fd == listen_fd){
                int conn_fd = accept(listen_fd, NULL, NULL);
                if (conn_fd == -1) {
                    LOG_WARN("接收客户端连接失败，错误码=%d", errno);
                    continue;
                }
                LOG_INFO("接收到客户端连接，客户端fd=%d", conn_fd);

                pthread_mutex_lock(&pool.lock);
                enQueue(&pool.queue, conn_fd);
                pthread_cond_signal(&pool.cond);// 唤醒一个工作线程来处理这个新连接
                pthread_mutex_unlock(&pool.lock);
            }
        }
    }

    // 理论上正常不会走到这里，写上只是让资源回收更完整
    close_log();
    return 0;
}
