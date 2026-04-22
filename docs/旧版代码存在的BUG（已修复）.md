# 现存 BUG（第一阶段静态代码审计）

本文档只审计当前项目中“第一期 / 第二期”已经涉及到的核心代码，不讨论第三期到第五期功能。

---

## BUG 1：`puts` 协议前后不一致，客户端会永久阻塞

**位置：**

- `src/client/client_command_handle.c:118`
- `src/client/client_command_handle.c:119`
- `src/server/handle.c:251`
- `src/server/handle.c:253`
- `src/server/handle.c:297`
- `src/server/handle.c:301`

**现象：**

- 客户端执行 `puts 文件名` 后，经常卡死不返回。
- 服务器端即使已经完成上传，客户端仍然在等待数据。
- 当服务器文件已完整存在时，这个问题 100% 会出现。

**分析：**

- 客户端在发送完文件后，固定还要再读一个“响应长度 + 响应字符串”：
  - 先 `recv(sock_fd, &resp_len, sizeof(int), MSG_WAITALL);`
  - 再按长度读取响应正文。
- 但是服务端 `handle_puts()` 在两条关键路径上都没有发送这段响应：
  - `remaining <= 0` 时直接 `close(file_fd); return;`
  - 上传成功时只 `printf("上传完成！\n");`，然后直接关闭文件并返回
- 于是客户端一直阻塞在 `recv()` 上，造成协议级死锁。

**修复对比：**

【原始代码段】

```c
if (remaining <= 0) {
    printf("文件已存在且完整，无需续传。\n");
    close(file_fd);
    return;
}

...

if (received_count < remaining) {
    ftruncate(file_fd, local_size + received_count);
} else {
    printf("上传完成！\n");
}

close(file_fd);
```

```c
int resp_len = 0;
recv(sock_fd, &resp_len, sizeof(int), MSG_WAITALL);
if (resp_len > 0 && resp_len < sizeof(response)) {
    recv(sock_fd, response, resp_len, MSG_WAITALL);
    response[resp_len] = '\0';
    printf("%s\n", response);
}
```

【修复后代码段】

```c
if (remaining <= 0) {
    send_msg(listen_fd, "服务器端文件已完整存在，无需续传。\n");
    close(file_fd);
    return;
}

...

if (received_count < remaining) {
    ftruncate(file_fd, local_size + received_count);
    send_msg(listen_fd, "传输中断，已保存当前进度。\n");
} else {
    send_msg(listen_fd, "上传完成！\n");
}

close(file_fd);
```

---

## BUG 2：`gets` 文件不存在时，服务端发的是字符串，客户端却按 `off_t` 读取，协议立刻错位

**位置：**

- `src/server/handle.c:175`
- `src/server/handle.c:177`
- `src/client/client_command_handle.c:31`
- `src/client/client_command_handle.c:32`

**现象：**

- 下载不存在的文件时，客户端可能显示乱码大小、假装下载成功，或者后续所有命令都异常。
- 更严重时，后面的通信内容会全部“串协议”，因为双方已经不在同一个读写节奏上了。

**分析：**

- 服务端在文件不存在时调用 `send_msg()`，发送的是：
  - `int len`
  - `len` 字节字符串
- 但客户端此时并不按这个格式接收，而是直接读取一个 `off_t file_size`。
- 这样客户端拿到的根本不是文件大小，而是“字符串长度 + 字符串前几个字节”的拼接数据。
- 从这一刻开始，TCP 字节流已经被错误解释，后续协议全部错位。

**修复对比：**

【原始代码段】

```c
int file_fd = open(real_path, O_RDONLY);
if (file_fd == -1) {
    send_msg(listen_fd, "Error: File not found on server.");
    return;
}
```

```c
off_t file_size = 0;
recv(sock_fd, &file_size, sizeof(off_t), MSG_WAITALL);
```

【修复后代码段】

```c
off_t file_total_size = -1;

int file_fd = open(real_path, O_RDONLY);
if (file_fd == -1) {
    send_full(listen_fd, &file_total_size, sizeof(off_t));
    return;
}

fstat(file_fd, &st);
file_total_size = st.st_size;
send_full(listen_fd, &file_total_size, sizeof(off_t));
```

```c
off_t file_size = 0;
if (recv_full(sock_fd, &file_size, sizeof(off_t)) <= 0) {
    printf("接收文件大小失败\n");
    return;
}

if (file_size < 0) {
    printf("服务器文件不存在\n");
    return;
}
```

---

## BUG 3：客户端先发命令、后做参数校验，`puts`/`gets` 会把服务端拖进死等

**位置：**

- `src/client/client_command_handle.c:21`
- `src/client/client_command_handle.c:22`
- `src/client/client_command_handle.c:23`
- `src/client/client_command_handle.c:25`
- `src/client/client_command_handle.c:78`
- `src/server/handle.c:64`
- `src/server/handle.c:70`
- `src/server/handle.c:167`
- `src/server/handle.c:220`

**现象：**

- 用户输入 `puts` 但没带文件名时，客户端打印“用法提示”，看起来像结束了；
- 但服务端已经收到了 `puts` 命令，并进入 `handle_puts()`，随后阻塞在接收文件大小的 `recv()` 上；
- 同一个连接后续再发送别的命令时，协议会完全混乱。

**分析：**

- 客户端 `process_command()` 一上来就先发：
  - 命令长度
  - 命令字符串
- 之后才去判断当前是不是 `gets` / `puts`，以及参数够不够。
- 这意味着“无效命令格式”已经发给服务端了。
- `puts` 最危险，因为服务端在 `handle_puts()` 里一定会继续等 `file_len`，而客户端已经提前 `return` 了。

**修复对比：**

【原始代码段】

```c
int len = strlen(input);
send(sock_fd, &len, sizeof(int), 0);
send(sock_fd, input, len, 0);

if (strcmp(cmd, "gets") == 0) {
    if (strlen(arg) == 0) {
        printf("用法: gets <文件名>\n");
        return;
    }
}
```

【修复后代码段】

```c
if (strcmp(cmd, "gets") == 0 && strlen(arg) == 0) {
    printf("用法: gets <文件名>\n");
    return;
}

if (strcmp(cmd, "puts") == 0 && strlen(arg) == 0) {
    printf("用法: puts <文件名>\n");
    return;
}

len = strlen(input);
send_full(sock_fd, &len, sizeof(int));
send_full(sock_fd, input, len);
```

---

## BUG 4：命令长度没有做边界检查，`recv()` 可能把栈上缓冲区写爆

**位置：**

- `src/server/handle.c:38`
- `src/server/handle.c:40`
- `src/server/handle.c:45`
- `src/server/handle.c:47`

**现象：**

- 恶意客户端只要伪造一个很大的 `cmd_len`，服务端就可能发生栈内存越界写。
- 轻则当前线程崩溃，重则直接把整个服务端打挂。

**分析：**

- `cmd_buf` 只有 `512` 字节。
- 但服务端收到 `cmd_len` 后，没有判断：
  - 是否小于等于 0
  - 是否大于 `sizeof(cmd_buf) - 1`
- 直接执行：
  - `recv(listen_fd, cmd_buf, cmd_len, MSG_WAITALL);`
- 这会把网络数据直接写进固定大小的栈数组里，属于典型缓冲区溢出。

**修复对比：**

【原始代码段】

```c
int cmd_len = 0;
ssize_t ret = recv(listen_fd, &cmd_len, sizeof(int), MSG_WAITALL);
...
char cmd_buf[512] = {0};
ret = recv(listen_fd, cmd_buf, cmd_len, MSG_WAITALL);
```

【修复后代码段】

```c
int cmd_len = 0;
ssize_t ret = recv_full(listen_fd, &cmd_len, sizeof(int));
if (ret <= 0) {
    break;
}

char cmd_buf[512] = {0};
if (cmd_len <= 0 || cmd_len >= (int)sizeof(cmd_buf)) {
    send_msg(listen_fd, "命令长度非法\n");
    break;
}

ret = recv_full(listen_fd, cmd_buf, cmd_len);
if (ret <= 0) {
    break;
}
cmd_buf[cmd_len] = '\0';
```

---

## BUG 5：`sscanf("%s %s")` 没有限宽，长命令会继续冲破本地数组

**位置：**

- `src/server/handle.c:53`
- `src/server/handle.c:54`
- `src/server/handle.c:55`
- `src/client/client_command_handle.c:14`
- `src/client/client_command_handle.c:19`

**现象：**

- 输入特别长的命令字或特别长的文件名时，客户端或服务端都可能栈溢出。
- 这种问题即使没有恶意攻击，单纯输入超长文件名也可能触发。

**分析：**

- `%s` 会一直读到空白符为止，不知道目标数组到底有多大。
- 当前代码里：
  - 服务端 `cmd[64]`、`arg[256]`
  - 客户端 `cmd[100]`、`arg[200]`
- 但 `sscanf()` 没有限制最大写入长度，所以超过数组上限就会覆盖邻接内存。

**修复对比：**

【原始代码段】

```c
char cmd[64] = {0};
char arg[256] = {0};
sscanf(cmd_buf, "%s %s", cmd, arg);
```

```c
char cmd[100], arg[200];
sscanf(input, "%s %s", cmd, arg);
```

【修复后代码段】

```c
char cmd[64] = {0};
char arg[256] = {0};
sscanf(cmd_buf, "%63s %255s", cmd, arg);
```

```c
char cmd[100] = {0};
char arg[200] = {0};
sscanf(input, "%99s %199s", cmd, arg);
```

---

## BUG 6：大量 `send()` / `recv()` / `write()` 只调用一次，没有处理半包，协议和文件内容都可能损坏

**位置：**

- `src/client/client_command_handle.c:22`
- `src/client/client_command_handle.c:23`
- `src/client/client_command_handle.c:94`
- `src/client/client_command_handle.c:111`
- `src/client/client_command_handle.c:119`
- `src/server/handle.c:18`
- `src/server/handle.c:21`
- `src/server/handle.c:187`
- `src/server/handle.c:247`
- `src/server/handle.c:281`

**现象：**

- 命令长度只发出一部分时，服务端会把后续正文当成新的长度字段。
- 文件上传时如果 `send()` 只发出一部分，客户端却把 `remaining` 减掉整块长度，最终文件缺数据。
- 下载时如果本地 `write()` 失败或只写一部分，也会形成静默损坏。

**分析：**

- TCP 是字节流，不保证“一次 `send()` 对应一次完整 `recv()`”。
- 即使是阻塞套接字，也不能假设一次 `send()` 就把全部数据推完。
- 当前代码只对部分 `recv()` 使用了 `MSG_WAITALL`，但对 `send()` 基本都只调用一次。
- 这不是“偶现小问题”，而是协议代码里必须显式处理的基础规则。

**修复对比：**

【原始代码段】

```c
send(sock_fd, &len, sizeof(int), 0);
send(sock_fd, input, len, 0);
```

```c
send(sock_fd, buf, n, 0);
remaining -= n;
```

【修复后代码段】

```c
int send_full(int fd, const void *buf, int len) {
    int total = 0;
    const char *p = (const char *)buf;
    while (total < len) {
        int ret = send(fd, p + total, len - total, 0);
        if (ret <= 0) {
            return -1;
        }
        total += ret;
    }
    return 0;
}
```

```c
if (send_full(sock_fd, &len, sizeof(int)) == -1) {
    return;
}
if (send_full(sock_fd, input, len) == -1) {
    return;
}
```

---

## BUG 7：路径拼接没有做合法性校验，客户端可以跳出 `../test` 根目录

**位置：**

- `src/server/handle.c:26`
- `src/server/handle.c:28`
- `src/server/handle.c:30`
- `src/server/handle.c:93`
- `src/server/handle.c:171`
- `src/server/handle.c:223`

**现象：**

- 恶意客户端可以构造 `../` 之类的参数，访问、下载、删除、创建 `test` 目录外的文件。
- 这会把“虚拟根目录”直接绕过去，变成任意文件读写删除漏洞。

**分析：**

- `get_real_path()` 只是机械拼接字符串，没有检查：
  - 参数里是否包含 `..`
  - 是否包含多余斜杠
  - 拼接结果是否仍在 `SERVER_BASE_DIR` 内
- 例如 `gets ../../etc/passwd`，最终就可能拼成 `../test/../../etc/passwd`。
- 操作系统在路径解析时会把 `..` 还原成真实父目录，于是目录隔离失效。

**修复对比：**

【原始代码段】

```c
void get_real_path(char *res, const char *path, const char *arg) {
    if (strcmp(path, "/") == 0) {
        sprintf(res, "%s/%s", SERVER_BASE_DIR, arg);
    } else {
        sprintf(res, "%s%s/%s", SERVER_BASE_DIR, path, arg);
    }
}
```

【修复后代码段】

```c
int get_real_path(char *res, int size, const char *path, const char *arg) {
    if (strstr(arg, "..") != NULL) {
        return -1;
    }

    if (strcmp(path, "/") == 0) {
        snprintf(res, size, "%s/%s", SERVER_BASE_DIR, arg);
    } else {
        snprintf(res, size, "%s%s/%s", SERVER_BASE_DIR, path, arg);
    }
    return 0;
}
```

---

## BUG 8：`ls` 没有 `closedir()`，并且结果字符串反复 `strcat()`，会同时造成句柄泄漏和缓冲区溢出

**位置：**

- `src/server/handle.c:117`
- `src/server/handle.c:120`
- `src/server/handle.c:126`
- `src/server/handle.c:131`
- `src/server/handle.c:132`
- `src/server/handle.c:134`

**现象：**

- 多次执行 `ls` 后，服务端目录流句柄会持续泄漏。
- 某个目录中文件较多、文件名较长时，`result[4096]` 会被 `strcat()` 写爆。
- 轻则返回乱码，重则当前线程崩溃。

**分析：**

- `handle_ls()` 打开目录后遍历完成直接 `send_msg()`，但没有 `closedir(dir)`。
- `strcat()` 每次都假定目的缓冲区还有空间，这里也没有剩余长度检查。
- 这是两个独立 bug：
  - 资源泄漏
  - 栈缓冲区溢出

**修复对比：**

【原始代码段】

```c
char result[4096] = {0};
while ((file = readdir(dir)) != NULL) {
    if (strcmp(file->d_name, ".") == 0 || strcmp(file->d_name, "..") == 0) {
        continue;
    }
    strcat(result, file->d_name);
    strcat(result, " ");
}
send_msg(listen_fd, result);
```

【修复后代码段】

```c
char result[4096] = {0};
int used = 0;

while ((file = readdir(dir)) != NULL) {
    if (strcmp(file->d_name, ".") == 0 || strcmp(file->d_name, "..") == 0) {
        continue;
    }

    int left = sizeof(result) - used;
    int ret = snprintf(result + used, left, "%s ", file->d_name);
    if (ret < 0 || ret >= left) {
        break;
    }
    used += ret;
}

closedir(dir);
send_msg(listen_fd, result);
```

---

## BUG 9：线程池退出逻辑只能唤醒“空闲线程”，正在 `recv()` 的工作线程无法退出，`SIGINT` 后服务端会卡死

**位置：**

- `src/server/server.c:80`
- `src/server/server.c:81`
- `src/server/server.c:82`
- `src/server/server.c:86`
- `src/server/worker.c:16`
- `src/server/worker.c:17`
- `src/server/worker.c:32`
- `src/server/handle.c:37`
- `src/server/handle.c:40`

**现象：**

- 服务端收到 `Ctrl+C` 后，不一定能退出。
- 只要某个工作线程正阻塞在 `handle_request()` 里的 `recv()`，主线程 `pthread_join()` 就会一直等下去。

**分析：**

- 当前线程池模型是：
  - 主线程 `accept()` 一个连接
  - 把 `conn_fd` 交给某个工作线程
  - 工作线程在 `handle_request()` 里循环处理这个连接直到客户端断开
- 这意味着线程实际上是“长期占有连接”的，而不是“短任务线程池”。
- 退出时虽然设置了 `pool.exitFlag = 1`，并 `broadcast` 条件变量，但这只能唤醒那些正睡在 `pthread_cond_wait()` 的空闲线程。
- 已经在 `recv()` 里的线程根本看不到 `exitFlag`，所以 `pthread_join()` 可能永久等待。

**修复对比：**

【原始代码段】

```c
pthread_mutex_lock(&pool.lock);
pool.exitFlag = 1;
pthread_cond_broadcast(&pool.cond);
pthread_mutex_unlock(&pool.lock);

for (int i = 0; i < pool.num; i++) {
    pthread_join(pool.thread_id_arr[i], NULL);
}
```

【修复后代码段】

```c
pthread_mutex_lock(&pool.lock);
pool.exitFlag = 1;
pthread_cond_broadcast(&pool.cond);
pthread_mutex_unlock(&pool.lock);

close(listen_fd);

/* 第二阶段建议：
 * 1. 把每个活跃 client_fd 记录下来；
 * 2. 退出时主动 shutdown(client_fd, SHUT_RDWR)；
 * 3. 让阻塞中的 recv 返回，线程才能走到 exitFlag 判断处退出。
 */

for (int i = 0; i < pool.num; i++) {
    pthread_join(pool.thread_id_arr[i], NULL);
}
```

---

## BUG 10：`pthread_create()` 的错误检查写错了，创建线程失败时不会被发现

**位置：**

- `src/server/thread_pool.c:19`
- `src/server/thread_pool.c:20`
- `include/error_check.h:18`
- `include/error_check.h:19`
- `include/error_check.h:20`
- `include/error_check.h:21`

**现象：**

- 当系统线程资源不够、`pthread_create()` 失败时，代码仍然会继续往下跑。
- 之后线程池数量和实际线程数量不一致，退出时 `pthread_join()` 还可能对无效线程 ID 操作。

**分析：**

- `pthread_create()` 成功返回 `0`，失败返回“正的错误码”。
- 但当前代码却用：
  - `ERROR_CHECK(ret, -1, "pthread_create");`
- 这只会检查 `ret == -1`。
- 所以真实失败值如 `11`、`22` 完全不会被拦住。

**修复对比：**

【原始代码段】

```c
int ret = pthread_create(&pool->thread_id_arr[idx], NULL, thread_func, (void*) pool);
ERROR_CHECK(ret, -1, "pthread_create");
```

【修复后代码段】

```c
int ret = pthread_create(&pool->thread_id_arr[idx], NULL, thread_func, (void*)pool);
THREAD_ERROR_CHECK(ret, "pthread_create");
```

---

## BUG 11：日志系统没有初始化就被 `ERROR_CHECK` 使用，出错时可能不是打印日志，而是直接崩溃

**位置：**

- `include/error_check.h:19`
- `include/error_check.h:21`
- `src/common/log.c:4`
- `src/common/log.c:43`
- `src/common/log.c:80`
- `src/common/log.c:82`

**现象：**

- 例如 `connect()`、`bind()`、`socket()` 失败时，本来应该打印错误日志并退出。
- 但当前项目从头到尾没有调用 `init_log()`。
- 于是第一次真正触发 `LOG_ERROR()` 时，`g_log_fp` 仍然是 `NULL`，`fprintf(g_log_fp, ...)` 行为未定义，可能直接崩溃。

**分析：**

- `ERROR_CHECK` 宏内部调用 `LOG_ERROR`。
- `LOG_ERROR` 最终走到 `log_write()`。
- `log_write()` 里直接：
  - `fprintf(g_log_fp, ...)`
- 可 `g_log_fp` 只有在 `init_log()` 里才会被赋值为文件或 `stdout`。
- 当前项目没有任何地方初始化日志系统，所以错误路径本身不可靠。

**修复对比：**

【原始代码段】

```c
static FILE* g_log_fp = NULL;

void log_write(...) {
    ...
    fprintf(g_log_fp, "[%s] [%s] ...", ...);
}
```

【修复后代码段】

```c
static FILE* g_log_fp = NULL;

int init_log(const char* level_str, const char* log_file) {
    ...
    if (log_file) {
        g_log_fp = fopen(log_file, "a");
    } else {
        g_log_fp = stdout;
    }
    return 0;
}

void log_write(...) {
    if (g_log_fp == NULL) {
        g_log_fp = stdout;
    }
    ...
    fprintf(g_log_fp, "[%s] [%s] ...", ...);
}
```

---

## BUG 12：路径与结果字符串大量使用 `sprintf` / `strcat`，深目录或长文件名下有明显溢出风险

**位置：**

- `src/server/handle.c:28`
- `src/server/handle.c:30`
- `src/server/handle.c:103`
- `src/server/handle.c:105`
- `src/server/handle.c:106`
- `src/server/handle.c:116`
- `src/server/handle.c:144`
- `src/server/handle.c:146`

**现象：**

- 如果目录层级变深、文件名很长，`current_path[512]`、`real_path[1024]` 都可能越界。
- 这类 bug 一旦发生，往往表现为：
  - 路径乱码
  - 随机崩溃
  - 后续命令行为异常

**分析：**

- `sprintf()` 不会检查目标数组剩余空间。
- `strcat()` 也假设目标缓冲区足够大。
- 当前代码在多个地方把用户输入的 `arg` 直接拼接进固定长度数组里，没有任何上界保护。

**修复对比：**

【原始代码段】

```c
sprintf(real_path, "%s%s/%s", SERVER_BASE_DIR, current_path, arg);
...
strcat(current_path, "/");
strcat(current_path, arg);
```

【修复后代码段】

```c
snprintf(real_path, sizeof(real_path), "%s%s/%s", SERVER_BASE_DIR, current_path, arg);
```

```c
if (snprintf(current_path, 512, "%s/%s", old_path, arg) >= 512) {
    send_msg(listen_fd, "路径过长\n");
    return;
}
```

---

## 审计结论

当前代码最需要优先修复的是下面 4 类问题：

1. `puts` / `gets` 协议不对称，导致客户端阻塞、协议错位。
2. 命令长度和字符串解析没有做边界保护，存在明显栈溢出风险。
3. TCP 收发没有统一封装，半包问题会直接破坏协议。
4. 线程池退出机制和路径拼接缺乏保护，分别会导致服务端无法优雅退出，以及目录越权访问。

这些问题都属于第一期、第二期核心功能范围内的现存缺陷，第二阶段修复时应优先处理。
