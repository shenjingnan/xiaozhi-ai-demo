#!/usr/bin/env python3
"""
ESP32音频接收和保存工具
接收来自ESP32的Base64编码音频数据并保存为MP3格式
"""

import os
import sys
import json
import base64
import wave
import serial
import threading
import queue
from datetime import datetime
from pydub import AudioSegment
import argparse

# 音频参数配置
SAMPLE_RATE = 16000      # 采样率 16kHz
CHANNELS = 1             # 单声道
BIT_DEPTH = 16          # 16位
BYTES_PER_SAMPLE = 2    # 16位 = 2字节


class AudioReceiver:
    """ESP32音频接收器类"""
    
    def __init__(self, port, baudrate=115200):
        self.port = port
        self.baudrate = baudrate
        self.serial = None
        self.is_running = False
        self.is_recording = False
        self.audio_buffer = []
        self.sequence_counter = 0
        self.expected_sequence = 0
        self.output_dir = os.path.join(os.path.dirname(__file__), "user_records")
        self.current_timestamp = None
        
        # 确保输出目录存在
        os.makedirs(self.output_dir, exist_ok=True)
        
    def connect(self):
        """连接到串口"""
        try:
            self.serial = serial.Serial(self.port, self.baudrate, timeout=1)
            print(f"✅ 已连接到串口 {self.port} @ {self.baudrate} bps")
            return True
        except Exception as e:
            print(f"❌ 串口连接失败: {e}")
            return False
            
    def process_line(self, line):
        """处理接收到的一行数据"""
        line = line.strip()
        if not line:
            return
            
        # 尝试解析JSON格式的消息
        try:
            data = json.loads(line)
            event = data.get('event')
            
            if event == 'wake_word_detected':
                print(f"🎉 检测到唤醒词！模型: {data.get('model')}")
                
            elif event == 'recording_started':
                print("🎤 开始录音...")
                self.is_recording = True
                self.audio_buffer = []
                self.expected_sequence = 0
                self.current_timestamp = datetime.now()
                
            elif event == 'audio_data' and self.is_recording:
                # 接收音频数据
                sequence = data.get('sequence', 0)
                audio_base64 = data.get('data', '')
                
                # 检查序列号
                if sequence != self.expected_sequence:
                    print(f"⚠️  序列号不连续: 期望 {self.expected_sequence}, 收到 {sequence}")
                    
                try:
                    # Base64解码
                    audio_bytes = base64.b64decode(audio_base64)
                    self.audio_buffer.append(audio_bytes)
                    self.expected_sequence = sequence + 1
                    
                    # 显示进度
                    total_samples = len(b''.join(self.audio_buffer)) // BYTES_PER_SAMPLE
                    duration = total_samples / SAMPLE_RATE
                    print(f"\r📊 已接收: {sequence + 1} 包, 时长: {duration:.2f}s", end='')
                    
                except Exception as e:
                    print(f"\n❌ 音频数据解码失败: {e}")
                    
            elif event == 'recording_stopped':
                print(f"\n🛑 录音结束")
                self.is_recording = False
                self.save_audio()
                
        except json.JSONDecodeError:
            # 非JSON格式的消息，直接显示
            if "唤醒词检测成功" in line:
                print(f"💬 {line}")
            elif "开始录音" in line:
                print(f"💬 {line}")
            elif "结束录音" in line:
                print(f"💬 {line}")
            else:
                # 其他日志消息
                print(f"📝 {line}")
                
    def save_audio(self):
        """保存音频数据为MP3文件"""
        if not self.audio_buffer:
            print("⚠️  没有音频数据可保存")
            return
            
        try:
            # 合并所有音频数据
            audio_data = b''.join(self.audio_buffer)
            print(f"\n💾 准备保存音频: {len(audio_data)} 字节")
            
            # 生成文件名
            timestamp = self.current_timestamp.strftime("%Y%m%d_%H%M%S")
            wav_filename = os.path.join(self.output_dir, f"esp32_recording_{timestamp}.wav")
            mp3_filename = os.path.join(self.output_dir, f"esp32_recording_{timestamp}.mp3")
            
            # 保存为WAV文件
            with wave.open(wav_filename, 'wb') as wav_file:
                wav_file.setnchannels(CHANNELS)
                wav_file.setsampwidth(BIT_DEPTH // 8)
                wav_file.setframerate(SAMPLE_RATE)
                wav_file.writeframes(audio_data)
                
            print(f"✅ WAV文件已保存: {wav_filename}")
            
            # 转换为MP3
            audio = AudioSegment.from_wav(wav_filename)
            audio.export(mp3_filename, format="mp3", bitrate="128k")
            print(f"✅ MP3文件已保存: {mp3_filename}")
            
            # 删除临时WAV文件
            os.remove(wav_filename)
            
            # 显示音频信息
            total_samples = len(audio_data) // BYTES_PER_SAMPLE
            duration = total_samples / SAMPLE_RATE
            print(f"📊 音频信息:")
            print(f"   - 时长: {duration:.2f} 秒")
            print(f"   - 采样数: {total_samples}")
            print(f"   - 文件大小: {os.path.getsize(mp3_filename) / 1024:.1f} KB")
            
        except Exception as e:
            print(f"❌ 保存音频失败: {e}")
            
    def run(self):
        """主运行循环"""
        if not self.connect():
            return
            
        print("🎧 开始监听ESP32音频数据...")
        print("=" * 50)
        
        self.is_running = True
        
        try:
            while self.is_running:
                if self.serial.in_waiting > 0:
                    try:
                        line = self.serial.readline().decode('utf-8', errors='replace')
                        self.process_line(line)
                    except Exception as e:
                        print(f"\n❌ 读取数据出错: {e}")
                        
        except KeyboardInterrupt:
            print("\n\n⚠️  检测到中断信号")
        finally:
            self.cleanup()
            
    def cleanup(self):
        """清理资源"""
        self.is_running = False
        if self.serial and self.serial.is_open:
            self.serial.close()
            print("✅ 串口已关闭")


def list_serial_ports():
    """列出可用的串口"""
    import serial.tools.list_ports
    ports = serial.tools.list_ports.comports()
    
    print("可用的串口设备:")
    print("-" * 40)
    for i, port in enumerate(ports):
        print(f"{i+1}. {port.device}")
        print(f"   描述: {port.description}")
        print(f"   硬件ID: {port.hwid}")
        print()
        
    return [port.device for port in ports]


def main():
    parser = argparse.ArgumentParser(description='ESP32音频接收和保存工具')
    parser.add_argument('-p', '--port', help='串口设备路径')
    parser.add_argument('-b', '--baudrate', type=int, default=115200, help='波特率 (默认: 115200)')
    parser.add_argument('-l', '--list', action='store_true', help='列出可用串口')
    
    args = parser.parse_args()
    
    if args.list:
        list_serial_ports()
        return
        
    # 如果没有指定串口，尝试自动检测
    if not args.port:
        ports = list_serial_ports()
        if not ports:
            print("❌ 未找到可用的串口设备")
            return
            
        if len(ports) == 1:
            args.port = ports[0]
            print(f"自动选择串口: {args.port}")
        else:
            print("请选择串口设备编号:")
            try:
                choice = int(input("输入编号: ")) - 1
                if 0 <= choice < len(ports):
                    args.port = ports[choice]
                else:
                    print("❌ 无效的选择")
                    return
            except (ValueError, KeyboardInterrupt):
                print("\n❌ 操作已取消")
                return
                
    print("=" * 50)
    print("ESP32音频接收器")
    print("=" * 50)
    print(f"串口: {args.port}")
    print(f"波特率: {args.baudrate}")
    print(f"输出目录: user_records/")
    print("=" * 50)
    print()
    
    receiver = AudioReceiver(args.port, args.baudrate)
    receiver.run()


if __name__ == "__main__":
    # 检查依赖
    try:
        import serial
        import pydub
    except ImportError as e:
        print(f"❌ 缺少必要的依赖: {e}")
        print("请安装: pip install pyserial pydub")
        sys.exit(1)
        
    main()