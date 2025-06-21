#!/usr/bin/env python3
"""
ESP32音频接收器 - 显示所有日志版本
同时显示ESP32的调试日志和处理音频数据
"""

import os
import sys
import json
import base64
import wave
import serial
from datetime import datetime
from pydub import AudioSegment
import argparse
import time
from send_custom_audio import get_custom_audio

# 音频参数配置
SAMPLE_RATE = 16000      # 采样率 16kHz
CHANNELS = 1             # 单声道
BIT_DEPTH = 16          # 16位
BYTES_PER_SAMPLE = 2    # 16位 = 2字节


class AudioReceiverWithLogs:
    """带日志显示的音频接收器"""
    
    def __init__(self, port, baudrate=115200):
        self.port = port
        self.baudrate = baudrate
        self.serial = None
        self.is_recording = False
        self.audio_buffer = []
        self.expected_sequence = 0
        self.output_dir = os.path.join(os.path.dirname(__file__), "user_records")
        self.current_timestamp = None
        
        # 加载light_on音频数据
        self.response_audio = get_custom_audio("light_on")
        print(f"已加载响应音频: {len(self.response_audio)} 字节 (约{len(self.response_audio)/2/SAMPLE_RATE:.1f}秒)")
        
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
        if line.startswith("{"):
            try:
                data = json.loads(line)
                event = data.get('event')
                
                if event == 'wake_word_detected':
                    print(f"\n🎉 [JSON] 检测到唤醒词！")
                    
                elif event == 'recording_started':
                    print("\n🎤 [JSON] 开始接收音频...")
                    self.is_recording = True
                    self.audio_buffer = []
                    self.expected_sequence = 0
                    self.current_timestamp = datetime.now()
                    
                elif event == 'audio_data' and self.is_recording:
                    # 接收音频数据
                    sequence = data.get('sequence', 0)
                    audio_base64 = data.get('data', '')
                    
                    try:
                        # Base64解码
                        audio_bytes = base64.b64decode(audio_base64)
                        self.audio_buffer.append(audio_bytes)
                        self.expected_sequence = sequence + 1
                        
                        # 显示进度
                        total_bytes = len(b''.join(self.audio_buffer))
                        duration = total_bytes / BYTES_PER_SAMPLE / SAMPLE_RATE
                        print(f"\r📊 [JSON] 已接收: 包#{sequence + 1}, 累计{duration:.2f}秒", end='')
                        
                    except Exception as e:
                        print(f"\n❌ [JSON] 音频解码失败: {e}")
                        
                elif event == 'recording_stopped':
                    print(f"\n🛑 [JSON] 录音结束")
                    self.is_recording = False
                    self.save_audio()
                    
                    # 发送响应音频
                    print("\n⏳ 等待0.5秒后发送响应音频...")
                    time.sleep(0.5)
                    self.send_response_audio()
                    
                else:
                    # 其他JSON事件
                    print(f"📩 [JSON] {event}: {line[:100]}...")
                    
            except json.JSONDecodeError:
                # 非JSON格式，显示为日志
                self.show_log(line)
        else:
            # 普通日志
            self.show_log(line)
            
    def show_log(self, line):
        """显示ESP32日志"""
        # 根据日志内容添加不同的标记
        if "ESP_LOGI" in line or "I (" in line:
            print(f"ℹ️  [LOG] {line}")
        elif "ESP_LOGW" in line or "W (" in line:
            print(f"⚠️  [LOG] {line}")
        elif "ESP_LOGE" in line or "E (" in line:
            print(f"❌ [LOG] {line}")
        elif "响应音频" in line:
            print(f"🔊 [LOG] {line}")
        elif "录音" in line:
            print(f"🎤 [LOG] {line}")
        else:
            print(f"📝 [LOG] {line}")
                
    def send_response_audio(self):
        """发送响应音频到ESP32"""
        print("\n📤 开始发送响应音频...")
        
        # 发送开始事件
        start_msg = json.dumps({
            "event": "response_started",
            "timestamp": int(time.time() * 1000)
        })
        self.serial.write((start_msg + '\n').encode())
        self.serial.flush()
        
        # 分块发送音频数据
        chunk_size = 4096  # 每个数据包4KB
        sequence = 0
        total_sent = 0
        
        while total_sent < len(self.response_audio):
            # 计算本次发送的数据量
            chunk_end = min(total_sent + chunk_size, len(self.response_audio))
            chunk_data = self.response_audio[total_sent:chunk_end]
            
            # Base64编码
            encoded_data = base64.b64encode(chunk_data).decode('ascii')
            
            # 构建数据包
            data_msg = json.dumps({
                "event": "response_audio",
                "sequence": sequence,
                "size": len(chunk_data),
                "data": encoded_data
            })
            
            # 发送数据
            self.serial.write((data_msg + '\n').encode())
            self.serial.flush()
            
            total_sent += len(chunk_data)
            sequence += 1
            
            # 显示进度
            progress = total_sent / len(self.response_audio) * 100
            print(f"\r📤 发送进度: {progress:.1f}% ({total_sent}/{len(self.response_audio)} 字节)", end='')
            
            # 短暂延时，避免发送过快
            time.sleep(0.01)
        
        # 发送结束事件
        stop_msg = json.dumps({
            "event": "response_stopped",
            "timestamp": int(time.time() * 1000)
        })
        self.serial.write((stop_msg + '\n').encode())
        self.serial.flush()
        
        print(f"\n✅ 响应音频发送完成: {sequence} 个数据包")
        print(f"   音频时长: {len(self.response_audio) / 2 / SAMPLE_RATE:.2f} 秒")
    
    def save_audio(self):
        """保存音频数据为MP3文件"""
        if not self.audio_buffer:
            print("⚠️  没有音频数据可保存")
            return
            
        try:
            # 合并所有音频数据
            audio_data = b''.join(self.audio_buffer)
            
            # 生成文件名
            timestamp = self.current_timestamp.strftime("%Y%m%d_%H%M%S")
            wav_filename = os.path.join(self.output_dir, f"recording_{timestamp}.wav")
            mp3_filename = os.path.join(self.output_dir, f"recording_{timestamp}.mp3")
            
            # 保存为WAV文件
            with wave.open(wav_filename, 'wb') as wav_file:
                wav_file.setnchannels(CHANNELS)
                wav_file.setsampwidth(BIT_DEPTH // 8)
                wav_file.setframerate(SAMPLE_RATE)
                wav_file.writeframes(audio_data)
                
            # 转换为MP3
            audio = AudioSegment.from_wav(wav_filename)
            audio.export(mp3_filename, format="mp3", bitrate="128k")
            
            # 删除临时WAV文件
            os.remove(wav_filename)
            
            # 显示音频信息
            duration = len(audio_data) / BYTES_PER_SAMPLE / SAMPLE_RATE
            print(f"\n✅ 音频已保存: {mp3_filename}")
            print(f"   时长: {duration:.2f} 秒")
            
        except Exception as e:
            print(f"\n❌ 保存音频失败: {e}")
            
    def run(self):
        """主运行循环"""
        if not self.connect():
            return
            
        print("\n🎧 开始监听ESP32...")
        print("=" * 60)
        
        try:
            while True:
                if self.serial.in_waiting > 0:
                    try:
                        line = self.serial.readline().decode('utf-8', errors='replace')
                        self.process_line(line)
                    except Exception as e:
                        print(f"\n❌ 读取数据出错: {e}")
                        
        except KeyboardInterrupt:
            print("\n\n⚠️  程序已停止")
        finally:
            if self.serial and self.serial.is_open:
                self.serial.close()
                print("✅ 串口已关闭")


def main():
    parser = argparse.ArgumentParser(description='ESP32音频接收器（带日志显示）')
    parser.add_argument('-p', '--port', help='串口设备路径')
    parser.add_argument('-b', '--baudrate', type=int, default=115200, help='波特率')
    
    args = parser.parse_args()
    
    # 自动检测串口
    if not args.port:
        import serial.tools.list_ports
        ports = list(serial.tools.list_ports.comports())
        
        if not ports:
            print("❌ 未找到可用的串口设备")
            return
            
        print("可用的串口设备:")
        for i, port in enumerate(ports):
            print(f"{i+1}. {port.device} - {port.description}")
            
        if len(ports) == 1:
            args.port = ports[0].device
            print(f"\n自动选择串口: {args.port}")
        else:
            try:
                choice = int(input("\n选择串口编号: ")) - 1
                if 0 <= choice < len(ports):
                    args.port = ports[choice].device
                else:
                    print("❌ 无效的选择")
                    return
            except (ValueError, KeyboardInterrupt):
                print("\n❌ 操作已取消")
                return
    
    print("\n" + "="*60)
    print("ESP32音频接收器（带日志显示）")
    print("="*60)
    print(f"串口: {args.port}")
    print(f"波特率: {args.baudrate}")
    print(f"响应音频: light_on.h (约2.87秒)")
    print("="*60)
    
    receiver = AudioReceiverWithLogs(args.port, args.baudrate)
    receiver.run()


if __name__ == "__main__":
    main()