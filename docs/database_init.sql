-- WindCloud 虚拟文件系统数据库初始化脚本
-- 创建数据库
CREATE DATABASE IF NOT EXISTS windcloud CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

USE windcloud;

-- 用户表
CREATE TABLE IF NOT EXISTS users (
    id INT PRIMARY KEY AUTO_INCREMENT,
    username VARCHAR(64) UNIQUE NOT NULL,
    password_hash VARCHAR(64) NOT NULL,
    salt VARCHAR(32) NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 物理文件表（存新式文件内容）
CREATE TABLE IF NOT EXISTS files (
    id INT PRIMARY KEY AUTO_INCREMENT,
    sha256sum CHAR(64) UNIQUE NOT NULL,
    size BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_sha256 (sha256sum)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 虚拟路径表（支持目录树结构）
CREATE TABLE IF NOT EXISTS paths (
    id INT PRIMARY KEY AUTO_INCREMENT,
    user_id INT NOT NULL,
    parent_id INT,
    name VARCHAR(255) NOT NULL,
    path VARCHAR(512) NOT NULL,
    type CHAR(1) DEFAULT 'f',     -- 'd' 为目录，'f' 为文件
    file_id INT,                  -- 关联 files.id，仅文件非空
    tomb TINYINT DEFAULT 0,       -- 0 正常，1 软删除（回收站）
    create_time DATETIME DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_user_parent_name (user_id, parent_id, name),
    INDEX idx_user_parent (user_id, parent_id),
    INDEX idx_user_tomb (user_id, tomb),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (parent_id) REFERENCES paths(id) ON DELETE CASCADE,
    FOREIGN KEY (file_id) REFERENCES files(id) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 块传输进度表（支持断点续传）
-- 用于记录文件传输进度，便于中断后继续传输
CREATE TABLE IF NOT EXISTS path_to_file (
    id INT PRIMARY KEY AUTO_INCREMENT,
    path_id INT NOT NULL,         -- 虚拟路径ID，关联 paths 表
    file_id INT NOT NULL,         -- 物理文件ID，关联 files 表
    sequence INT DEFAULT 0,       -- 已传输完成的块序号（下一个要传的块）
    total_blocks INT NOT NULL,    -- 文件总块数
    transfer_status ENUM('uploading', 'downloading', 'completed', 'cancelled') DEFAULT 'uploading',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_path_file (path_id, file_id),
    INDEX idx_path_id (path_id),
    INDEX idx_file_id (file_id),
    INDEX idx_status (transfer_status),
    FOREIGN KEY (path_id) REFERENCES paths(id) ON DELETE CASCADE,
    FOREIGN KEY (file_id) REFERENCES files(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- 初始化每个用户的根目录（在登录/注册时调用存储过程）
-- 这个示例已在 DAO 函数中处理，这里无需再做

COMMIT;
