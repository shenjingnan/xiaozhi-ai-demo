#!/bin/bash

# 千问大模型集成服务器启动脚本

echo "=== 千问大模型集成服务器 ==="
echo "正在检查Python环境..."

# 检查Python版本
python3 --version

echo "正在检查依赖..."
# 安装依赖
pip3 install -r requirements.txt

echo "正在启动服务器..."
# 启动服务器
python3 server.py
