#!/bin/bash
# 直接从串口读取并传递给Python脚本

# 检测可用的串口
echo "检测可用串口..."
PORTS=$(ls /dev/cu.* | grep -E "(usbserial|usbmodem|SLAB)" 2>/dev/null)

if [ -z "$PORTS" ]; then
    echo "错误：未找到USB串口设备"
    exit 1
fi

# 如果只有一个串口，直接使用
PORT_COUNT=$(echo "$PORTS" | wc -l)
if [ "$PORT_COUNT" -eq 1 ]; then
    PORT=$PORTS
    echo "自动选择串口: $PORT"
else
    # 多个串口，让用户选择
    echo "找到多个串口："
    echo "$PORTS" | nl
    read -p "请选择串口编号: " choice
    PORT=$(echo "$PORTS" | sed -n "${choice}p")
fi

echo "使用串口: $PORT"
echo "波特率: 115200"
echo ""

# 配置串口
stty -f "$PORT" 115200 cs8 -cstopb -parenb 2>/dev/null

# 启动接收器
echo "启动音频接收器..."
echo "按 Ctrl+C 退出"
echo "=================================="

# 直接使用cat读取串口
cat "$PORT" | python3 "$(dirname "$0")/audio_receiver_simple.py"