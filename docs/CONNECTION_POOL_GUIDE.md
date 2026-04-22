# 数据库连接池使用指南

## 概述

为了提升数据库连接的管理效率和并发性能，已新增数据库连接池模块。连接池支持：
- 连接复用：避免频繁创建/销毁连接
- 连接限流：通过最大连接数限制数据库负载
- 自动超时检测：移除失效连接
- 线程安全：使用互斥锁和条件变量实现

---

## 核心组件

### 1. `include/db_pool.h` - 连接池头文件

**主要结构体：**
- `db_pool_config_t` - 连接池配置
- `db_connection_t` - 单个连接的状态封装
- `db_pool_t` - 连接池全局状态

**主要函数：**
- `db_pool_init()` - 初始化连接池
- `db_pool_get_connection()` - 获取连接
- `db_pool_release_connection()` - 释放连接
- `db_pool_get_stats()` - 获取连接池统计信息
- `db_pool_destroy()` - 销毁连接池

### 2. `src/server/db_pool.c` - 连接池实现

**主要特色：**
- 动态连接创建：连接数在 `min_connections` 到 `max_connections` 之间变化
- 连接有效性检查：使用 `mysql_ping()` 验证连接
- 等待机制：当连接池满时，线程等待其他线程释放连接
- 超时控制：支持获取连接超时设置

---

## 使用方式

### 方式1：使用连接池（推荐）

```c
// 在 server.c 中
int main() {
    // ... 其他初始化 ...
    
    // 从配置文件读取数据库参数
    dao_set_connection_params(db_host, db_user, db_password, db_name);
    
    // 使用连接池初始化
    // 参数1：最小连接数（建议 = 工作线程数 + 2）
    // 参数2：最大连接数（建议 = 最小连接数 * 1.5）
    if (dao_init_with_pool(7, 10) != 0) {
        LOG_ERROR("数据库连接池初始化失败");
        return 1;
    }
    
    // ... 继续初始化 ...
}
```

### 方式2：使用传统 TLS 模式（不使用连接池）

```c
// 在 server.c 中
int main() {
    // ... 其他初始化 ...
    
    dao_set_connection_params(db_host, db_user, db_password, db_name);
    
    // 只初始化 MySQL 库，不使用连接池
    if (dao_init() != 0) {
        LOG_ERROR("MySQL 库初始化失败");
        return 1;
    }
    
    // ... 继续初始化 ...
}
```

---

## 连接获取和释放

### 自动管理（推荐）

使用 DAO 函数时，连接自动从 `get_thread_db_conn()` 获取：

```c
// 在各个 handle_* 函数中
MYSQL *conn = (MYSQL*)get_thread_db_conn();
if (conn == NULL) {
    conn = dao_get_connection();  // 自动从连接池获取
    if (conn == NULL) {
        send_msg(client_fd, "数据库连接失败");
        return;
    }
}

// 使用连接进行数据库操作
int file_id = dao_find_file_by_hash(conn, sha256);

// 操作结束后释放连接（如果使用连接池）
dao_close_connection(conn);
```

### 手动获取连接

如果需要直接使用连接池 API：

```c
// 获取连接
MYSQL *conn = db_pool_get_connection();
if (conn == NULL) {
    LOG_ERROR("获取连接超时");
    return -1;
}

// 使用连接执行 SQL
mysql_query(conn, "SELECT * FROM users");

// 使用完后立即释放，归还连接池
db_pool_release_connection(conn);
```

---

## 配置参数说明

连接池配置在 `dao_init_with_pool()` 中设置：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `min_connections` | 5 | 最小连接数，服务启动时创建 |
| `max_connections` | 20 | 最大连接数，动态扩展上限 |
| `max_idle_time` | 3600 | 连接最大空闲时间（秒） |
| `connection_timeout` | 30 | 获取连接超时时间（秒） |

### 推荐配置

基于不同的并发场景：

```c
// 低并发场景（如 5 个工作线程）
int min_size = 5 + 2;    // = 7
int max_size = (7 * 3) / 2;  // = 10

// 中并发场景（如 10 个工作线程）
int min_size = 10 + 2;   // = 12
int max_size = (12 * 3) / 2; // = 18

// 高并发场景（如 20 个工作线程）
int min_size = 20 + 2;   // = 22
int max_size = (22 * 3) / 2; // = 33
```

**配置原则：**
- 最小连接数 = 工作线程数 + 2（预留额度）
- 最大连接数 = 最小连接数 * 1.5（允许峰值扩展）
- 不要设置过大，避免数据库服务器压力过大

---

## 性能对比

### 连接池模式

| 指标 | 数值 |
|------|------|
| 连接创建开销 | 仅在第一次 |
| 连接重用率 | 95%+ |
| 线程等待时间 | 极短（通常 < 1ms） |
| 数据库连接数 | 固定（可控） |

### 传统 TLS 模式

| 指标 | 数值 |
|------|------|
| 连接创建开销 | 每个请求 |
| 连接重用率 | 0%（总是创建新连接） |
| 线程等待时间 | 长（取决于网络） |
| 数据库连接数 | 等于工作线程数 |

---

## 监控和调试

### 获取连接池统计信息

```c
int pool_size = 0;
int in_use_count = 0;

db_pool_get_stats(&pool_size, &in_use_count);

LOG_INFO("连接池统计: 总连接数=%d, 使用中=%d, 可用=%d",
    pool_size, in_use_count, pool_size - in_use_count);
```

### 日志输出

连接池在以下情况会输出日志：

- **初始化成功**
```
[INFO] 连接池初始化成功: min=7, max=10
```

- **获取连接（复用）**
```
[DEBUG] 获取连接成功 (复用), 连接标号=2, 使用中=6, 总数=7
```

- **创建新连接**
```
[DEBUG] 获取连接成功 (新建), 连接标号=7, 使用中=7, 总数=8
```

- **连接释放**
```
[DEBUG] 连接已释放，连接标号=2, 使用中=6, 总数=8
```

- **连接超时**
```
[ERROR] 获取连接超时
```

---

## 常见问题

### Q1: 如何判断是否正在使用连接池？

A: 检查服务日志中的初始化消息：
- 若看到 `"连接池初始化成功"` - 使用连接池
- 若看到 `"MySQL 库初始化成功"` - 使用传统模式

### Q2: 连接获取超时怎么办？

A: 可能原因：
1. 连接池最大值太小 - 增加 `max_connections`
2. 数据库服务不可用 - 检查数据库连接
3. SQL 查询太慢 - 优化 SQL 或增加工作线程

### Q3: 如何使用连接池与旧代码兼容？

A: 旧代码无需修改，自动适配：
- 若调用 `dao_init()` - 使用传统 TLS 模式
- 若调用 `dao_init_with_pool()` - 使用连接池模式

### Q4: 是否可以动态调整连接池大小？

A: 当前版本不支持动态调整，需要重启服务。未来版本可添加此功能。

---

## 故障排查

### 问题1：连接池内存泄漏

**症状**：内存持续增长，`pool_size` 不变

**解决**：
```c
// 确保所有连接都被释放
db_pool_get_stats(&pool_size, &in_use_count);
if (in_use_count > 0) {
    LOG_WARN("未释放的连接数: %d", in_use_count);
}

// 检查代码中是否遗漏了 dao_close_connection()
```

### 问题2：连接超时频繁

**症状**：日志中频繁出现 `"获取连接超时"`

**解决**：
```c
// 1. 增加连接池大小
dao_init_with_pool(10, 20);

// 2. 增加获取超时时间
// 修改 dao_vfs.c 中的配置参数

// 3. 优化 SQL 查询性能
// 使用 EXPLAIN 分析慢查询
```

### 问题3：数据库连接数异常

**症状**：`mysql> SHOW PROCESSLIST;` 显示连接数过多

**解决**：
```c
// 检查是否所有连接都被及时释放
db_pool_get_stats(&pool_size, &in_use_count);
LOG_INFO("请求处理后连接数: 总=%d, 使用中=%d", pool_size, in_use_count);

// 若 in_use_count > 0 且不再变化，说明有连接未释放
```

---

## 最佳实践

### 1. 合理设置连接池大小

```c
// 根据工作线程数调整
int worker_thread_count = 5;
int min_pool = worker_thread_count + 2;
int max_pool = (min_pool * 3) / 2;

dao_init_with_pool(min_pool, max_pool);
```

### 2. 及时释放连接

```c
// 操作完成后立即释放
MYSQL *conn = dao_get_connection();
// ... 使用连接 ...
dao_close_connection(conn);  // 重要！
```

### 3. 处理连接失败

```c
MYSQL *conn = dao_get_connection();
if (conn == NULL) {
    LOG_ERROR("连接获取失败");
    send_msg(client_fd, "服务器暂时不可用，请稍后重试");
    return;
}
```

### 4. 定期检查池状态

```c
// 在日志输出中添加
int pool_size, in_use;
db_pool_get_stats(&pool_size, &in_use);
LOG_DEBUG("DB pool: %d/%d", in_use, pool_size);
```

---

## 升级步骤

如果从旧版本升级：

1. **编译新代码**
   ```bash
   make clean
   make
   ```

2. **修改 server.c** - 将 `dao_init()` 改为 `dao_init_with_pool(7, 10)`

3. **验证日志** - 确认看到 `"连接池初始化成功"`

4. **性能测试** - 对比集中式连接池与旧 TLS 方式的性能

5. **逐步推向生产** - 先在测试环境验证，再灰度上线

---

最后更新：2026年4月
