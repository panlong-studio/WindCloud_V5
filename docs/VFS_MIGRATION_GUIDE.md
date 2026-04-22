# 虚拟文件系统改造部署指南

## 概述
本文档说明如何将 WindCloud 项目从本地文件系统改造为数据库虚拟文件系统，并支持用户隔离、极速秒传和断点续传。

## 前置条件

### 系统环境
- Linux（Ubuntu 18.04+ 或其他发行版）
- GCC 编译器
- GNU Make
- MySQL Server 5.7+ 或 MariaDB 10.3+

### 安装依赖库
```bash
# Ubuntu/Debian
sudo apt update
sudo apt install libmysqlclient-dev libmysqlclient21 mysql-server

# 若使用 MariaDB
sudo apt install libmariadb-dev libmariadb3

# 其他必要库
sudo apt install libpthread-stubs0-dev
```

## 步骤1：数据库初始化

### 1.1 启动 MySQL 服务
```bash
sudo systemctl start mysql
# 或
sudo service mysql start
```

### 1.2 创建数据库和表
```bash
# 使用 root 用户登录 MySQL
mysql -u root -p

# 在 MySQL 命令行中执行
source /path/to/docs/database_init.sql;

# 查看创建结果
USE windcloud;
SHOW TABLES;
```

### 1.3 创建应用用户（可选，生产环境推荐）
```sql
-- 创建专用用户
CREATE USER 'windcloud'@'localhost' IDENTIFIED BY 'windcloud_password';
GRANT ALL PRIVILEGES ON windcloud.* TO 'windcloud'@'localhost';
FLUSH PRIVILEGES;
```

## 步骤2：配置应用

### 2.1 修改 config/config.ini
编辑 `config/config.ini` 文件，确保数据库配置正确：
```ini
# 如使用 root 用户
db_host=127.0.0.1
db_user=root
db_password=root        # 根据实际情况修改密码
db_name=windcloud

# 如使用专用用户
db_host=127.0.0.1
db_user=windcloud
db_password=windcloud_password
db_name=windcloud
```

### 2.2 其他配置项
```ini
ip=127.0.0.1            # 服务器绑定 IP
port=9090               # 服务器端口
log=DEBUG               # 日志级别
server_log=../log/server.log
client_log=../log/client.log
```

## 步骤3：编译项目

### 3.1 创建存储目录
```bash
cd /path/to/WindCloud_V3

# 创建物理文件存储目录
mkdir -p ./storage
mkdir -p ./log

# 设置权限
chmod 755 ./storage ./log
```

### 3.2 编译
```bash
# 清理旧编译文件（如有）
make clean

# 编译项目
make

# 查看编译结果
ls -la bin/
```

### 3.3 编译故障排查

**错误：找不到 mysql.h**
```bash
# 检查 MySQL 开发库是否安装
dpkg -l | grep mysql

# 若未安装，使用命令安装
sudo apt install libmysqlclient-dev
```

**错误：undefined reference to `mysql_*`**
```bash
# Makefile 中应包含 -lmysqlclient 链接标志
# 检查 Makefile 中的 LIBS 定义
grep "LIBS" Makefile
```

## 步骤4：运行服务

### 4.1 启动服务端
```bash
# 确保在项目根目录
cd /path/to/WindCloud_V3

# 运行服务器
./bin/server_app

# 期望输出
# 工作线程初始化成功，共 4 个线程
# 监听端口 9090
```

### 4.2 启动客户端（另一个终端）
```bash
cd /path/to/WindCloud_V3

# 运行客户端
./bin/client_app

# 输入服务器 IP 和端口
服务器IP: 127.0.0.1
服务器Port: 9090
```

### 4.3 基本操作

**注册用户**
```
> register
用户名: testuser
密码: password123
```

**登录**
```
> login
用户名: testuser
密码: password123
```

**查看当前路径**
```
> pwd
/
```

**列出目录内容**
```
> ls
（初始为空）
```

**创建目录**
```
> mkdir documents
```

**切换目录**
```
> cd documents
```

**上传文件**
```
> puts /local/path/to/file.txt

# 若文件哈希已存在（秒传）
秒传成功！

# 若需要上传
上传完成！
```

**下载文件**
```
> gets file.txt
```

## 文件结构说明

```
WindCloud_V3/
├── bin/                    # 可执行文件
│   ├── server_app         # 服务端
│   └── client_app         # 客户端
├── build/                 # 编译中间文件
├── config/
│   └── config.ini         # 配置文件
├── docs/
│   └── database_init.sql  # 数据库初始化脚本
├── include/               # 头文件
│   ├── dao_vfs.h         # 数据库虚拟文件系统（新增）
│   ├── auth.h            # 用户认证（新增）
│   ├── protocol.h        # 协议（已修改）
│   └── ... 其他文件
├── log/                   # 日志文件
├── src/
│   ├── client/           # 客户端源代码
│   ├── server/
│   │   ├── dao_vfs.c     # 数据库操作实现（新增）
│   │   ├── auth.c        # 用户认证实现（新增）
│   │   ├── file_cmds.c   # 文件操作（已修改）
│   │   ├── file_transfer.c # 文件传输（已修改）
│   │   ├── session.c     # 会话管理（已修改）
│   │   └── ... 其他文件
│   └── common/           # 公共代码
├── storage/              # 物理文件存储目录（自动创建）
├── Makefile             # 编译配置
└── ...
```

## 数据库设计

### users 表
- `id`: 用户 ID（主键）
- `username`: 用户名（唯一）
- `password_hash`: 密码哈希
- `salt`: 盐值
- `created_at`: 创建时间

### files 表
- `id`: 文件 ID（主键）
- `sha256sum`: 文件 SHA256 哈希（唯一，用于秒传）
- `size`: 文件大小
- `created_at`: 创建时间

### paths 表
- `id`: 路径 ID（主键）
- `user_id`: 用户 ID（外键）
- `parent_id`: 父目录 ID（自引用）
- `name`: 文件/目录名
- `path`: 虚拟路径（冗余字段，便于查询）
- `type`: 类型（'d' 目录，'f' 文件）
- `file_id`: 物理文件 ID（仅文件类型非空）
- `tomb`: 软删除标记（0 正常，1 删除）
- `create_time`: 创建时间

## 关键特性

### 1. 用户隔离
每个用户拥有独立的文件树，通过 `user_id` 隔离。

### 2. 极速秒传
- 客户端计算文件 SHA256 哈希
- 服务端检查 `files` 表中是否存在相同哈希
- 若存在，直接创建 `paths` 记录，无需上传文件内容
- 适用于大文件和重复文件

### 3. 断点续传
- 客户端记录上传进度（字节偏移）
- 服务端返回已接收的字节数
- 断网后可从上次位置继续上传

### 4. 虚拟文件系统
- 支持无限深度的目录树结构
- 支持软删除（回收站功能，可扩展）
- 支持文件重命名（修改 `name`）

## 安全建议

1. **数据库认证**
   - 不要在生产环境使用 root 用户
   - 为专用用户设置强密码
   - 定期备份数据库

2. **密码存储**
   - 已使用 MD5 + 盐值存储（可升级为 bcrypt）
   - 盐值存储于 `salt` 字段

3. **文件存储**
   - 物理文件存储在 `./storage/` 目录
   - 可配置权限限制访问
   - 建议定期备份

4. **网络安全**
   - 建议在生产环境使用 SSL/TLS 加密传输
   - 实现速率限制防止滥用

## 故障排查

### 问题1：数据库连接失败
**症状**：`线程无法连接数据库`

**解决**：
```bash
# 检查 MySQL 是否运行
sudo systemctl status mysql

# 验证配置文件
cat config/config.ini | grep db_

# 测试连接
mysql -h 127.0.0.1 -u root -p -e "USE windcloud; SHOW TABLES;"
```

### 问题2：编译失败
**症状**：`undefined reference to mysql_*`

**解决**：
```bash
# 检查链接库
ldconfig -p | grep mysql

# 重新安装开发库
sudo apt install --reinstall libmysqlclient-dev

# 清理并重新编译
make clean && make
```

### 问题3：文件上传失败
**症状**：`服务端创建文件失败`

**解决**：
```bash
# 检查存储目录权限
ls -ld ./storage

# 修改权限
chmod 777 ./storage

# 检查磁盘空间
df -h .
```

### 问题4：登录失败
**症状**：`用户名或密码错误`

**解决**：
```bash
# 检查用户是否存在
mysql -u root -p windcloud -e "SELECT * FROM users;"

# 查看日志
tail -f log/server.log
```

## 升级建议

1. **密码加密**：升级为 bcrypt 或 Argon2
2. **文件压缩**：上传前压缩，下载时解压
3. **权限管理**：添加文件分享、协作编辑功能
4. **缓存优化**：使用 Redis 缓存热点数据
5. **监控告警**：集成 Prometheus + Grafana

## 联系支持
如有问题，请查阅项目文档或联系开发团队。

---
最后更新：2026年4月
