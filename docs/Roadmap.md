# 项目执行路线图

## Phase 1: 项目初始化
- [x] 创建项目目录结构
- [x] 初始化Git仓库
- [x] 创建基础配置文件

## Phase 2: Backend实现
- [x] 创建CMakeLists.txt和项目结构
- [x] 实现协议消息编解码器
- [x] 实现TcpClient类核心功能
  - [x] 连接管理
  - [x] 消息发送
  - [x] 消息接收和解析
  - [x] 回调机制
- [x] 实现测试客户端程序

## Phase 3: 测试服务器实现
- [x] 创建测试服务器项目结构
- [x] 实现协议解析
- [x] 实现内存数据存储
- [x] 实现读写操作处理

## Phase 4: Docker化
- [x] 创建backend Dockerfile（跨平台）
- [x] 创建test-server Dockerfile（跨平台）
- [x] 创建docker-compose.yml
- [x] 测试Docker构建和运行

## Phase 5: 文档和验证
- [x] 创建README.md
- [x] 创建.gitignore
- [x] 编写测试用例
- [x] 验证完整流程
