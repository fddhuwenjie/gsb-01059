# 项目需求文档

## 项目概述
实现基于libuv的TCP客户端库，支持自定义二进制协议的数据读写操作。

## 功能需求

### 1. TCP客户端类（TcpClient）
- 使用libuv实现异步TCP客户端
- 封装连接、断开、读写操作
- 支持自定义二进制协议

### 2. 协议格式

#### 读数据请求（func = 0）
```
| func(2B) | 0xABCD(2B) | length(2B) | func(2B) | dataSize(2B) |
```
- func: 0x0000 (2字节)
- 魔数: 0xABCD (2字节)
- length: 整个消息长度（2字节，小端序）
- func: 功能码（2字节，重复）
- dataSize: 数据大小（2字节，读请求中为0）

#### 读数据响应（func = 2）
```
| func(2B) | 0xABCD(2B) | length(2B) | func(2B) | dataSize(2B) | data(nB) |
```
- func: 0x0002 (2字节)
- 魔数: 0xABCD (2字节)
- length: 整个消息长度（2字节）
- func: 功能码（2字节）
- dataSize: 数据大小（2字节）
- data: 实际数据（n字节）

#### 写数据请求（func = 1）
```
| func(2B) | 0xABCD(2B) | length(2B) | func(2B) | dataSize(2B) | data(nB) |
```
- func: 0x0001 (2字节)
- 魔数: 0xABCD (2字节)
- length: 整个消息长度（2字节）
- func: 功能码（2字节）
- dataSize: 数据大小（2字节）
- data: 要写入的数据（n字节）

#### 写数据响应（func = 3）
```
| func(2B) | 0xABCD(2B) | length(2B) | func(2B) | dataSize(2B) |
```
- func: 0x0003 (2字节)
- 魔数: 0xABCD (2字节)
- length: 整个消息长度（2字节）
- func: 功能码（2字节）
- dataSize: 数据大小（2字节，写响应中为写入的数据大小）

### 3. TcpClient类接口
- `Connect(host, port)`: 连接到服务器
- `ReadData(callback)`: 发送读数据请求，异步接收响应
- `WriteData(data, callback)`: 发送写数据请求，异步接收响应
- `Disconnect()`: 断开连接
- `IsConnected()`: 检查连接状态

### 4. 验证方式
- 创建一个简单的测试服务器，实现相同的协议
- 客户端连接后可以执行读写操作
- 服务器端维护一个简单的内存数据存储
- 通过测试程序验证读写功能

## 技术需求

### 技术栈
- C++17或更高版本
- libuv (异步I/O库)
- CMake (构建系统)

### Docker要求
- 支持ARM64和AMD64架构
- 使用多阶段构建
- 基础镜像选择跨平台版本

## 项目结构
```
.
├── backend/          # C++ TCP客户端库
│   ├── src/
│   ├── include/
│   ├── CMakeLists.txt
│   └── Dockerfile
├── test-server/      # 测试服务器（用于验证客户端）
│   ├── src/
│   ├── CMakeLists.txt
│   └── Dockerfile
├── docker-compose.yml
├── .gitignore
└── README.md
```

## 非功能需求
- 代码需要良好的错误处理
- 支持异步操作，不阻塞事件循环
- 内存管理安全，避免泄漏
- 跨平台兼容性（Linux, macOS, Windows）
