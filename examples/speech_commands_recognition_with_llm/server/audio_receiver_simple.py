#!/usr/bin/env python3
"""
简化版ESP32音频接收器
只处理JSON格式的音频数据，忽略其他日志
"""

import os
import sys
import json
import base64
import wave
import threading
from datetime import datetime
from pydub import AudioSegment
import time

# 音频参数配置
SAMPLE_RATE = 16000
CHANNELS = 1
BIT_DEPTH = 16
BYTES_PER_SAMPLE = 2


class SimpleAudioReceiver:
    def __init__(self):
        self.is_recording = False
        self.audio_buffer = []
        self.expected_sequence = 0
        self.output_dir = os.path.join(os.path.dirname(__file__), "user_records")
        self.current_timestamp = None

        # 确保输出目录存在
        os.makedirs(self.output_dir, exist_ok=True)

    def process_line(self, line):
        """处理接收到的一行数据"""
        line = line.strip()
        if not line:
            return

        # 只处理JSON格式的数据
        if not line.startswith("{"):
            return

        try:
            data = json.loads(line)
            event = data.get("event")

            if event == "wake_word_detected":
                print(f"\n✅ 检测到唤醒词！")

            elif event == "recording_started":
                print("🎤 开始接收音频数据...")
                self.is_recording = True
                self.audio_buffer = []
                self.expected_sequence = 0
                self.current_timestamp = datetime.now()

            elif event == "audio_data" and self.is_recording:
                sequence = data.get("sequence", 0)
                audio_base64 = data.get("data", "")

                # 检查序列号
                if sequence != self.expected_sequence:
                    print(
                        f"⚠️  数据包乱序: 期望 {self.expected_sequence}, 收到 {sequence}"
                    )

                try:
                    # Base64解码
                    audio_bytes = base64.b64decode(audio_base64)
                    self.audio_buffer.append(audio_bytes)
                    self.expected_sequence = sequence + 1

                    # 显示进度（不换行）
                    print(f"\r📦 已接收数据包: {sequence + 1}", end="", flush=True)

                except Exception as e:
                    print(f"\n❌ 音频解码失败: {e}")

            elif event == "recording_stopped":
                print(f"\n✅ 音频接收完成")
                self.is_recording = False
                self.save_audio()

        except json.JSONDecodeError:
            # 忽略非JSON数据
            pass

    def save_audio(self):
        """保存音频数据为MP3文件"""
        if not self.audio_buffer:
            print("⚠️  没有音频数据可保存")
            return

        try:
            # 合并所有音频数据
            audio_data = b"".join(self.audio_buffer)

            # 生成文件名
            timestamp = self.current_timestamp.strftime("%Y%m%d_%H%M%S")
            wav_filename = os.path.join(self.output_dir, f"recording_{timestamp}.wav")
            mp3_filename = os.path.join(self.output_dir, f"recording_{timestamp}.mp3")

            # 保存为WAV文件
            with wave.open(wav_filename, "wb") as wav_file:
                wav_file.setnchannels(CHANNELS)
                wav_file.setsampwidth(BIT_DEPTH // 8)
                wav_file.setframerate(SAMPLE_RATE)
                wav_file.writeframes(audio_data)

            # 转换为MP3
            audio = AudioSegment.from_wav(wav_filename)
            audio.export(mp3_filename, format="mp3", bitrate="128k")

            # 删除临时WAV文件
            os.remove(wav_filename)

            # 显示结果
            duration = len(audio_data) / (SAMPLE_RATE * BYTES_PER_SAMPLE)
            file_size = os.path.getsize(mp3_filename) / 1024

            print(f"\n✅ 音频已保存")
            print(f"📁 文件位置: {mp3_filename}")
            print(f"⏱️  录音时长: {duration:.1f} 秒")
            print(f"💾 文件大小: {file_size:.1f} KB")
            print("-" * 50)

        except Exception as e:
            print(f"\n❌ 保存音频失败: {e}")

    def run(self):
        """从标准输入读取数据"""
        print("=" * 50)
        print("ESP32音频接收器（简化版）")
        print("=" * 50)
        print("请运行: idf.py flash monitor | python audio_receiver_simple.py")
        print("或者直接从串口读取数据并通过管道传入")
        print("=" * 50)
        print("\n等待音频数据...\n")

        try:
            # 从标准输入读取
            for line in sys.stdin:
                self.process_line(line)

        except KeyboardInterrupt:
            print("\n\n✅ 程序已退出")


def main():
    # 检查依赖
    try:
        import pydub
    except ImportError:
        print("❌ 缺少必要的依赖: pydub")
        print("请安装: pip install pydub")
        sys.exit(1)

    receiver = SimpleAudioReceiver()
    receiver.run()


if __name__ == "__main__":
    main()
