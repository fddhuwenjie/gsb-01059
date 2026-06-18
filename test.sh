#!/bin/bash

# TCP客户端项目测试脚本

set -e

echo "=========================================="
echo "TCP客户端项目测试脚本"
echo "=========================================="
echo ""

# 颜色定义
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 清理旧容器
echo "步骤1: 清理旧容器..."
docker compose down > /dev/null 2>&1 || true
echo "✓ 清理完成"
echo ""

# 构建并启动服务
echo "步骤2: 构建并启动服务..."
docker compose up --build -d
echo "✓ 服务启动完成"
echo ""

# 等待服务启动
echo "步骤3: 等待服务启动（3秒）..."
sleep 3
echo "✓ 等待完成"
echo ""

# 检查服务状态
echo "步骤4: 检查服务状态..."
if docker compose ps | grep -q "Up"; then
    echo "✓ 服务运行正常"
else
    echo -e "${RED}✗ 服务启动失败${NC}"
    docker compose ps
    exit 1
fi
echo ""

# 显示服务器日志
echo "=========================================="
echo "测试服务器日志:"
echo "=========================================="
docker compose logs test-server
echo ""

# 显示客户端测试日志
echo "=========================================="
echo "客户端测试日志:"
echo "=========================================="
docker compose logs backend
echo ""

# 检查测试结果
echo "=========================================="
echo "测试结果分析:"
echo "=========================================="

TEST_LOG=$(docker compose logs backend 2>&1)

if echo "$TEST_LOG" | grep -q "✓✓✓ All tests passed"; then
    echo -e "${GREEN}✓✓✓ 测试通过！所有功能正常！${NC}"
    echo ""
    echo "测试详情:"
    echo "$TEST_LOG" | grep -E "(✓|✗|Test|Error)" | head -20
    exit 0
elif echo "$TEST_LOG" | grep -q "✗\|Error\|Failed"; then
    echo -e "${RED}✗ 测试失败！请检查错误信息${NC}"
    echo ""
    echo "错误详情:"
    echo "$TEST_LOG" | grep -E "(✗|Error|Failed)" | head -10
    exit 1
else
    echo -e "${YELLOW}⚠ 无法确定测试结果，请手动检查日志${NC}"
    echo ""
    echo "完整日志:"
    echo "$TEST_LOG"
    exit 2
fi
