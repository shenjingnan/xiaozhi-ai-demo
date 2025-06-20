#!/bin/bash
# 启动服务器脚本 - 使用Python 3.13

echo "正在启动WebSocket服务器 (Python 3.13)..."

# 检查API密钥
if [ -z "$DASHSCOPE_API_KEY" ]; then
    echo "错误: 请设置DASHSCOPE_API_KEY环境变量"
    echo "运行: export DASHSCOPE_API_KEY=your_api_key"
    exit 1
fi

# 使用Python 3.13启动服务器
/Users/nemo/.pyenv/versions/3.13.2/bin/python server.py