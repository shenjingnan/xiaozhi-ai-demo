#!/usr/bin/env python3
"""
调试版ESP32音频接收器
显示所有接收到的数据，帮助调试串口通信
"""

import os
import sys
import json
import base64
from datetime import datetime
from pydub import AudioSegment

# 音频参数配置
SAMPLE_RATE = 16000
CHANNELS = 1
BIT_DEPTH = 16
BYTES_PER_SAMPLE = 2


class DebugAudioReceiver:
    def __init__(self):
        self.line_count = 0
        self.json_count = 0
        self.audio_packets = 0

    def process_line(self, line):
        """处理接收到的一行数据"""
        self.line_count += 1
        line = line.strip()
        
        if not line:
            return
            
        # 显示前50个字符
        display_line = line[:50] + "..." if len(line) > 50 else line
        print(f"[{self.line_count:04d}] {display_line}")
        
        # 尝试解析JSON
        if line.startswith("{"):
            try:
                data = json.loads(line)
                self.json_count += 1
                event = data.get("event", "unknown")
                
                print(f"  └─ JSON事件: {event}")
                
                if event == "audio_data":
                    self.audio_packets += 1
                    seq = data.get("sequence", -1)
                    data_len = len(data.get("data", ""))
                    print(f"     └─ 音频包 #{seq}, Base64长度: {data_len}")
                    
            except json.JSONDecodeError as e:
                print(f"  └─ JSON解析失败: {e}")

    def run(self):
        """从标准输入读取数据"""
        print("=" * 60)
        print("ESP32音频接收器（调试版）")
        print("=" * 60)
        print("显示所有接收到的数据，帮助调试串口通信")
        print("统计信息将在程序结束时显示")
        print("=" * 60)
        print()

        try:
            # 从标准输入读取
            for line in sys.stdin:
                self.process_line(line)

        except KeyboardInterrupt:
            print("\n\n" + "=" * 60)
            print("统计信息：")
            print(f"- 总行数: {self.line_count}")
            print(f"- JSON消息数: {self.json_count}")
            print(f"- 音频数据包数: {self.audio_packets}")
            print("=" * 60)


def main():
    receiver = DebugAudioReceiver()
    receiver.run()


if __name__ == "__main__":
    main()