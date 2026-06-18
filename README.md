# TCP客户端项目

## 项目介绍

基于 libuv 实现的 TCP 客户端库，封装为 TcpClient 类，支持自定义二进制协议的数据读写操作。项目包含完整的客户端实现和测试服务器，可通过 Docker 一键运行验证。

## 服务说明

| 服务 | 说明 | 端口 |
|------|------|------|
| test-server | TCP测试服务器，处理客户端读写请求 | 8888 (容器内) / 8889 (外部) |
| backend | TCP客户端测试程序，自动执行读写测试 | - |

## 如何运行

### 一键测试（推荐）

```bash
./test.sh
```

### 手动运行

```bash
# 启动服务
docker compose up --build -d

# 等待启动
sleep 3

# 查看测试结果
docker compose logs backend

# 停止服务
docker compose down
```

### 测试成功标志

```
✓ Connected successfully!
✓ Write successful!
✓ Read successful!
✓✓✓ All tests passed! Data matches!
```

## 测试账号

本项目为 TCP 协议测试项目，不涉及用户账号系统。测试通过自动化流程验证：
1. 客户端连接服务器
2. 写入数据 "Hello, World!"
3. 读取数据并验证一致性

## 题目内容
使用libuv实现以下协议的客户端部分，封装成一个名为TcpClient的类：// 读数据请求 func = 0 | 0xABCD(2B) | length(2B) | func(2B) | dataSize(2B) |

// 读数据响应 func = 2 | 0xABCD(2B) | length(2B) | func(2B) | dataSize(2B) | data(nB) |

// 写数据请求 func = 1 | 0xABCD(2B) | length(2B) | func(2B) | dataSize(2B) | data(nB) |

// 写数据响应 func = 3 | 0xABCD(2B) | length(2B) | func(2B) | dataSize(2B) |


使用 libuv 实现 TCP 客户端，封装成 TcpClient 类，支持以下协议：

### 协议格式

**读请求 (func=0) / 写响应 (func=3)**
```
| func(2B) | 0xABCD(2B) | length(2B) | func(2B) | dataSize(2B) |
```

**读响应 (func=2) / 写请求 (func=1)**
```
| func(2B) | 0xABCD(2B) | length(2B) | func(2B) | dataSize(2B) | data(nB) |
```

### 实现要求

- TcpClient 类：支持 Connect、ReadData、WriteData、Disconnect 操作
- 协议编解码：小端序，魔数 0xABCD 验证
- 异步回调机制
- Docker 跨平台支持（ARM64/AMD64）
