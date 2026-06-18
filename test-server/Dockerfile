# 多阶段构建，支持ARM64和AMD64
FROM --platform=$BUILDPLATFORM ubuntu:22.04 AS builder

# 安装构建工具和依赖
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libuv1-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# 复制源代码
COPY . .

# 构建项目
RUN mkdir -p build && \
    cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    cmake --build . || (echo "Build failed!" && exit 1)

# 运行时镜像
FROM ubuntu:22.04

# 安装运行时依赖
RUN apt-get update && apt-get install -y \
    libuv1 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 从构建阶段复制二进制文件
COPY --from=builder /build/build/test_server .

# 暴露端口
EXPOSE 8888

# 设置启动命令
CMD ["./test_server"]
