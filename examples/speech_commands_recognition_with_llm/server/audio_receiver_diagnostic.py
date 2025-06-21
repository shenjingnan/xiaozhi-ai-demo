#!/usr/bin/env python3
"""
ESP32音频接收诊断工具
用于诊断音频接收和保存的问题
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


class DiagnosticAudioReceiver:
    """诊断版音频接收器"""
    
    def __init__(self, port, baudrate=115200):
        self.port = port
        self.baudrate = baudrate
        self.serial = None
        self.is_recording = False
        self.audio_buffer = []
        self.packet_info = []  # 记录每个数据包的信息
        self.expected_sequence = 0
        self.output_dir = os.path.join(os.path.dirname(__file__), "user_records")
        self.current_timestamp = None
        
        # 统计信息
        self.total_bytes_received = 0
        self.total_samples_received = 0
        self.packet_count = 0
        
        # 加载light_on音频数据（更短的音频）
        self.custom_audio = get_custom_audio("light_on")
        print(f"已加载light_on音频: {len(self.custom_audio)} 字节 (约{len(self.custom_audio)/2/SAMPLE_RATE:.1f}秒)")
        
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
                print(f"\n🎉 检测到唤醒词！模型: {data.get('model')}")
                
            elif event == 'recording_started':
                print("\n🎤 开始录音...")
                print(f"   时间戳: {data.get('timestamp', 'N/A')}")
                self.is_recording = True
                self.audio_buffer = []
                self.packet_info = []
                self.expected_sequence = 0
                self.current_timestamp = datetime.now()
                self.total_bytes_received = 0
                self.total_samples_received = 0
                self.packet_count = 0
                
            elif event == 'audio_data' and self.is_recording:
                # 接收音频数据
                sequence = data.get('sequence', 0)
                audio_base64 = data.get('data', '')
                reported_size = data.get('size', 0)
                
                # 检查序列号
                if sequence != self.expected_sequence:
                    print(f"\n⚠️  序列号不连续: 期望 {self.expected_sequence}, 收到 {sequence}")
                    
                try:
                    # Base64解码
                    audio_bytes = base64.b64decode(audio_base64)
                    actual_size = len(audio_bytes)
                    
                    # 验证大小
                    if reported_size != actual_size:
                        print(f"\n⚠️  数据大小不匹配: 报告 {reported_size}, 实际 {actual_size}")
                    
                    # 记录数据包信息
                    self.packet_info.append({
                        'sequence': sequence,
                        'size': actual_size,
                        'samples': actual_size // BYTES_PER_SAMPLE
                    })
                    
                    self.audio_buffer.append(audio_bytes)
                    self.expected_sequence = sequence + 1
                    self.packet_count += 1
                    self.total_bytes_received += actual_size
                    self.total_samples_received += actual_size // BYTES_PER_SAMPLE
                    
                    # 显示进度
                    duration = self.total_samples_received / SAMPLE_RATE
                    print(f"\r📊 包 #{sequence}: {actual_size} 字节 ({actual_size//BYTES_PER_SAMPLE} 样本) | 总计: {self.total_bytes_received} 字节, {duration:.2f}s", end='')
                    
                except Exception as e:
                    print(f"\n❌ 音频数据解码失败: {e}")
                    
            elif event == 'recording_stopped':
                print(f"\n\n🛑 录音结束")
                print(f"   时间戳: {data.get('timestamp', 'N/A')}")
                self.is_recording = False
                self.show_diagnostics()
                self.save_audio()
                
                # 发送响应音频
                print("\n⏳ 等待1秒后发送响应音频...")
                time.sleep(1)
                self.send_response_audio()
                
        except json.JSONDecodeError:
            # 非JSON格式的消息
            if "录音长度:" in line:
                print(f"\n📝 ESP32报告: {line}")
            elif "录音数据发送完成" in line:
                print(f"📝 ESP32报告: {line}")
                
    def show_diagnostics(self):
        """显示诊断信息"""
        print("\n" + "="*60)
        print("📊 音频接收诊断信息")
        print("="*60)
        print(f"数据包总数: {self.packet_count}")
        print(f"总字节数: {self.total_bytes_received}")
        print(f"总样本数: {self.total_samples_received}")
        print(f"计算时长: {self.total_samples_received / SAMPLE_RATE:.3f} 秒")
        print(f"平均包大小: {self.total_bytes_received / self.packet_count:.1f} 字节" if self.packet_count > 0 else "")
        
        # 显示前5个和后5个数据包信息
        if self.packet_info:
            print("\n前5个数据包:")
            for i, info in enumerate(self.packet_info[:5]):
                print(f"  包 #{info['sequence']}: {info['size']} 字节 ({info['samples']} 样本)")
                
            if len(self.packet_info) > 10:
                print("  ...")
                
            if len(self.packet_info) > 5:
                print("\n后5个数据包:")
                for info in self.packet_info[-5:]:
                    print(f"  包 #{info['sequence']}: {info['size']} 字节 ({info['samples']} 样本)")
        
        print("="*60)
                
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
        
        while total_sent < len(self.custom_audio):
            # 计算本次发送的数据量
            chunk_end = min(total_sent + chunk_size, len(self.custom_audio))
            chunk_data = self.custom_audio[total_sent:chunk_end]
            
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
            progress = total_sent / len(self.custom_audio) * 100
            print(f"\r📤 发送进度: {progress:.1f}% ({total_sent}/{len(self.custom_audio)} 字节)", end='')
            
            # 短暂延时，避免发送过快
            time.sleep(0.01)
        
        # 发送结束事件
        stop_msg = json.dumps({
            "event": "response_stopped",
            "timestamp": int(time.time() * 1000)
        })
        self.serial.write((stop_msg + '\n').encode())
        self.serial.flush()
        
        print(f"\n✅ 响应音频发送完成: {sequence} 个数据包, {len(self.custom_audio)} 字节")
        print(f"   预计播放时长: {len(self.custom_audio) / 2 / SAMPLE_RATE:.2f} 秒")
    
    def save_audio(self):
        """保存音频数据为MP3文件"""
        if not self.audio_buffer:
            print("⚠️  没有音频数据可保存")
            return
            
        try:
            # 合并所有音频数据
            audio_data = b''.join(self.audio_buffer)
            
            # 验证数据完整性
            actual_total_bytes = len(audio_data)
            if actual_total_bytes != self.total_bytes_received:
                print(f"\n⚠️  数据完整性问题: 合并后 {actual_total_bytes} 字节, 统计 {self.total_bytes_received} 字节")
            
            print(f"\n💾 准备保存音频: {actual_total_bytes} 字节")
            
            # 生成文件名
            timestamp = self.current_timestamp.strftime("%Y%m%d_%H%M%S")
            wav_filename = os.path.join(self.output_dir, f"diagnostic_{timestamp}.wav")
            mp3_filename = os.path.join(self.output_dir, f"diagnostic_{timestamp}.mp3")
            
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
            
            # 保留WAV文件用于诊断
            # os.remove(wav_filename)
            
            # 显示最终音频信息
            total_samples = actual_total_bytes // BYTES_PER_SAMPLE
            duration = total_samples / SAMPLE_RATE
            print(f"\n📊 最终音频信息:")
            print(f"   - 时长: {duration:.3f} 秒")
            print(f"   - 采样数: {total_samples}")
            print(f"   - WAV大小: {os.path.getsize(wav_filename) / 1024:.1f} KB")
            print(f"   - MP3大小: {os.path.getsize(mp3_filename) / 1024:.1f} KB")
            print(f"   - 采样率: {SAMPLE_RATE} Hz")
            print(f"   - 声道: {CHANNELS} (单声道)")
            print(f"   - 位深度: {BIT_DEPTH} 位")
            
        except Exception as e:
            print(f"\n❌ 保存音频失败: {e}")
            import traceback
            traceback.print_exc()
            
    def run(self):
        """主运行循环"""
        if not self.connect():
            return
            
        print("\n🎧 诊断模式：监听ESP32音频数据...")
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
    parser = argparse.ArgumentParser(description='ESP32音频接收诊断工具')
    parser.add_argument('-p', '--port', help='串口设备路径')
    parser.add_argument('-b', '--baudrate', type=int, default=115200, help='波特率 (默认: 115200)')
    
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
    print("ESP32音频接收诊断工具")
    print("="*60)
    print(f"串口: {args.port}")
    print(f"波特率: {args.baudrate}")
    print(f"输出目录: user_records/")
    print("="*60)
    
    receiver = DiagnosticAudioReceiver(args.port, args.baudrate)
    receiver.run()


if __name__ == "__main__":
    main()