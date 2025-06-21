#!/bin/bash
# ESP32音频接收脚本 (macOS版本)

# 默认串口参数
PORT=${1:-/dev/cu.usbserial-0001}  # 第一个参数为串口
BAUDRATE=115200

echo "=================================="
echo "ESP32音频接收器 (macOS)"
echo "=================================="
echo "串口: $PORT"
echo "波特率: $BAUDRATE"
echo ""

# 检查串口是否存在
if [ ! -e "$PORT" ]; then
    echo "错误：串口 $PORT 不存在"
    echo ""
    echo "可用的串口设备："
    ls /dev/cu.* | grep -E "(usbserial|usbmodem|SLAB)" || echo "未找到USB串口设备"
    echo ""
    echo "使用方法: $0 <串口设备>"
    echo "例如: $0 /dev/cu.usbserial-0001"
    exit 1
fi

# 检查Python脚本是否存在
SCRIPT_DIR="$(dirname "$0")"
RECEIVER_SCRIPT="$SCRIPT_DIR/audio_receiver_simple.py"

if [ ! -f "$RECEIVER_SCRIPT" ]; then
    echo "错误：找不到接收脚本 $RECEIVER_SCRIPT"
    exit 1
fi

# 检查依赖
if ! python3 -c "import pydub" 2>/dev/null; then
    echo "错误：缺少Python依赖 pydub"
    echo "请运行: pip3 install pydub"
    exit 1
fi

echo "开始接收音频数据..."
echo "按 Ctrl+C 退出"
echo "=================================="

# macOS使用screen命令读取串口
# 注意：运行后需要按Ctrl+A然后K来退出screen
screen -L -Logfile - "$PORT" "$BAUDRATE" | python3 "$RECEIVER_SCRIPT"