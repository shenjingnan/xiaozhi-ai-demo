#!/bin/bash
# ESP32音频接收脚本
# 直接从串口读取数据，不使用idf.py monitor

# 默认串口参数
PORT=${1:-/dev/ttyUSB0}  # 第一个参数为串口，默认/dev/ttyUSB0
BAUDRATE=115200

echo "=================================="
echo "ESP32音频接收器"
echo "=================================="
echo "串口: $PORT"
echo "波特率: $BAUDRATE"
echo ""

# 检查串口是否存在
if [ ! -e "$PORT" ]; then
    echo "错误：串口 $PORT 不存在"
    echo ""
    echo "可用的串口设备："
    ls /dev/tty* | grep -E "(USB|ACM)" || echo "未找到USB串口设备"
    exit 1
fi

# 检查Python脚本是否存在
SCRIPT_DIR="$(dirname "$0")"
RECEIVER_SCRIPT="$SCRIPT_DIR/audio_receiver_simple.py"

if [ ! -f "$RECEIVER_SCRIPT" ]; then
    echo "错误：找不到接收脚本 $RECEIVER_SCRIPT"
    exit 1
fi

echo "开始接收音频数据..."
echo "按 Ctrl+C 退出"
echo "=================================="

# 使用cat或stty读取串口数据并传递给Python脚本
# 先配置串口参数
stty -F "$PORT" "$BAUDRATE" cs8 -cstopb -parenb 2>/dev/null

# 读取串口数据并传递给Python脚本
cat "$PORT" | python3 "$RECEIVER_SCRIPT"