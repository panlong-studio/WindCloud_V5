#ifndef _DAO_VFS_H_
#define _DAO_VFS_H_

#include <mysql/mysql.h>

/**
 * 数据库虚拟文件系统 DAO 层函数声明
 * 底层使用 MySQL C API 进行数据库操作
 * 所有函数均支持线程安全（通过 get_thread_db_conn 获取线程专属连接）
 */

/* ============ 连接管理 ============ */

/**
 * 设置数据库连接参数
 * 应在任何其他 DAO 函数之前调用
 * 参数 host：数据库主机
 * 参数 user：数据库用户
 * 参数 password：数据库密码
 * 参数 name：数据库名称
 * 返回：void
 */
void dao_set_connection_params(const char *host, const char *user, const char *password, const char *name);

/**
 * 初始化数据库连接池
 * 使用连接池管理数据库连接，支持连接复用和并发控制
 * 参数 min_connections：最小连接数（建议 5-10）
 * 参数 max_connections：最大连接数（建议 20-50）
 * 返回：0 成功，-1 失败
 */
int dao_init_with_pool(int min_connections, int max_connections);

/**
 * 获取当前线程的数据库连接（兼容模式，自动从连接池获取）
 * 返回：MYSQL* 指针，或 NULL（未初始化）
 * 注意：使用连接池时无需手动调用 dao_close_connection，连接自动归还池
 */
void* get_thread_db_conn(void);

/**
 * 初始化数据库连接池
 * 仅在服务启动时调用一次
 * 返回：0 成功，-1 失败
 */
int dao_init(void);

/**
 * 线程初始化数据库连接（应在工作线程开始处调用）
 * 返回：MYSQL* 指针，或 NULL 失败
 */
MYSQL* dao_get_connection(void);

/**
 * 线程关闭数据库连接（应在工作线程退出前调用）
 * 参数 conn：MYSQL* 指针
 * 返回：void
 */
void dao_close_connection(MYSQL *conn);

/* ============ 用户认证相关 ============ */

/**
 * 用户登录
 * 参数 conn：MYSQL* 指针
 * 参数 username：用户名
 * 参数 password：明文密码
 * 返回：>0 用户 ID，0 登录失败，-1 数据库错误
 */
int dao_login_user(MYSQL *conn, const char *username, const char *password);

/**
 * 用户注册
 * 参数 conn：MYSQL* 指针
 * 参数 username：用户名
 * 参数 password：明文密码
 * 返回：>0 新用户 ID，0 用户已存在，-1 数据库错误
 */
int dao_register_user(MYSQL *conn, const char *username, const char *password);

/* ============ 目录和文件操作 ============ */

/**
 * 获取用户的根目录 ID
 * 参数 conn：MYSQL* 指针
 * 参数 user_id：用户 ID
 * 返回：>0 根目录 ID，-1 失败或不存在
 */
int dao_get_root_dir_id(MYSQL *conn, int user_id);

// 获取目录的父目录 ID，若已是根目录则返回 -1
int dao_get_parent_id(MYSQL *conn, int dir_id);

/**
 * 创建或获取根目录（用户初次登录时调用）
 * 参数 conn：MYSQL* 指针
 * 参数 user_id：用户 ID
 * 返回：>0 根目录 ID，-1 失败
 */
int dao_init_root_dir(MYSQL *conn, int user_id);

/**
 * 查找指定目录下的子项（文件或目录）
 * 参数 conn：MYSQL* 指针
 * 参数 parent_id：父目录 ID
 * 参数 name：子项名称
 * 参数 is_dir：是否查找目录（1 为目录，0 为文件，-1 不限）
 * 返回：>0 子项 ID，-1 不存在或查询失败
 */
int dao_find_child_id(MYSQL *conn, int parent_id, const char *name, int is_dir);

/**
 * 列出目录内容
 * 参数 conn：MYSQL* 指针
 * 参数 user_id：用户 ID
 * 参数 dir_id：目录 ID
 * 参数 result：输出缓冲区（调用者预分配，建议 4096 字节）
 * 参数 result_len：输出缓冲区大小
 * 返回：0 成功，-1 失败；结果写入 result
 */
int dao_list_dir(MYSQL *conn, int user_id, int dir_id, char *result, int result_len);

/**
 * 创建目录
 * 参数 conn：MYSQL* 指针
 * 参数 user_id：用户 ID
 * 参数 parent_id：父目录 ID
 * 参数 dir_name：目录名称
 * 返回：>0 新目录 ID，0 目录已存在，-1 创建失败
 */
int dao_create_dir(MYSQL *conn, int user_id, int parent_id, const char *dir_name);

/**
 * 删除文件或目录项
 * 参数 conn：MYSQL* 指针
 * 参数 user_id：用户 ID
 * 参数 path_id：路径 ID
 * 返回：0 成功，-1 失败
 */
int dao_delete_entry(MYSQL *conn, int user_id, int path_id);

/**
 * 获取文件信息（哈希、大小等）
 * 参数 conn：MYSQL* 指针
 * 参数 user_id：用户 ID
 * 参数 path_id：路径 ID
 * 参数 sha256：输出缓冲区，存放 SHA256 哈希（65 字节，包括 \0）
 * 参数 file_size：输出文件大小
 * 返回：0 成功，-1 失败
 */
int dao_get_file_info(MYSQL *conn, int user_id, int path_id, char *sha256, unsigned long *file_size);

/* ============ 文件存储相关 ============ */

/**
 * 根据哈希查找物理文件
 * 参数 conn：MYSQL* 指针
 * 参数 sha256：SHA256 哈希值（64 字节）
 * 返回：>0 文件 ID，-1 不存在或查询失败
 */
int dao_find_file_by_hash(MYSQL *conn, const char *sha256);

/**
 * 插入新物理文件记录
 * 参数 conn：MYSQL* 指针
 * 参数 sha256：SHA256 哈希值
 * 参数 file_size：文件大小
 * 返回：>0 新文件 ID，-1 失败
 */
int dao_insert_file(MYSQL *conn, const char *sha256, unsigned long file_size);

/**
 * 为文件创建虚拟路径记录（用于秒传）
 * 参数 conn：MYSQL* 指针
 * 参数 user_id：用户 ID
 * 参数 parent_id：父目录 ID
 * 参数 filename：文件名
 * 参数 file_id：物理文件 ID
 * 参数 path：虚拟路径（冗余字段）
 * 返回：>0 新路径 ID，-1 失败
 */
int dao_insert_path(MYSQL *conn, int user_id, int parent_id, const char *filename, int file_id, const char *path);

/* ============ 块传输进度相关（path_to_file 表） ============ */

/**
 * 创建块传输进度记录
 * 参数 conn：MYSQL* 指针
 * 参数 path_id：虚拟路径 ID
 * 参数 file_id：物理文件 ID
 * 参数 total_blocks：文件总块数
 * 参数 status：传输状态（'uploading'/'downloading'）
 * 返回：>0 成功，-1 失败
 */
int dao_create_transfer_record(MYSQL *conn, int path_id, int file_id, int total_blocks, const char *status);

/**
 * 获取块传输进度
 * 参数 conn：MYSQL* 指针
 * 参数 path_id：虚拟路径 ID
 * 参数 sequence：输出，返回已完成的块序号
 * 返回：0 成功，-1 失败或记录不存在
 */
int dao_get_transfer_sequence(MYSQL *conn, int path_id, int *sequence);

/**
 * 更新块传输进度
 * 参数 conn：MYSQL* 指针
 * 参数 path_id：虚拟路径 ID
 * 参数 new_sequence：新的块序号
 * 返回：0 成功，-1 失败
 */
int dao_update_transfer_sequence(MYSQL *conn, int path_id, int new_sequence);

/**
 * 完成块传输 (将状态改为 'completed')
 * 参数 conn：MYSQL* 指针
 * 参数 path_id：虚拟路径 ID
 * 返回：0 成功，-1 失败
 */
int dao_complete_transfer(MYSQL *conn, int path_id);

/**
 * 取消块传输
 * 参数 conn：MYSQL* 指针
 * 参数 path_id：虚拟路径 ID
 * 返回：0 成功，-1 失败
 */
int dao_cancel_transfer(MYSQL *conn, int path_id);

#endif
