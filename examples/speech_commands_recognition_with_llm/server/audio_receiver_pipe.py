#!/usr/bin/env python3
"""
ESP32音频接收器 - 管道版本
通过标准输入/输出与ESP32通信
使用方式: idf.py monitor | python audio_receiver_pipe.py
"""

import os
import sys
import json
import base64
import wave
from datetime import datetime
from pydub import AudioSegment
import time
from send_custom_audio import get_custom_audio

# 音频参数配置
SAMPLE_RATE = 16000      # 采样率 16kHz
CHANNELS = 1             # 单声道
BIT_DEPTH = 16          # 16位
BYTES_PER_SAMPLE = 2    # 16位 = 2字节


class PipeAudioReceiver:
    """通过管道通信的音频接收器"""
    
    def __init__(self):
        self.is_recording = False
        self.audio_buffer = []
        self.expected_sequence = 0
        self.output_dir = os.path.join(os.path.dirname(__file__), "user_records")
        self.current_timestamp = None
        
        # 加载light_on音频数据
        self.response_audio = get_custom_audio("light_on")
        sys.stderr.write(f"已加载响应音频: {len(self.response_audio)} 字节 (约{len(self.response_audio)/2/SAMPLE_RATE:.1f}秒)\n")
        
        # 确保输出目录存在
        os.makedirs(self.output_dir, exist_ok=True)
        
    def process_line(self, line):
        """处理接收到的一行数据"""
        line = line.strip()
        if not line:
            return
            
        # 直接输出原始行（传递给终端）
        print(line)
        sys.stdout.flush()
        
        # 尝试解析JSON格式的消息
        if line.startswith("{"):
            try:
                data = json.loads(line)
                event = data.get('event')
                
                if event == 'wake_word_detected':
                    sys.stderr.write("\n🎉 检测到唤醒词！\n")
                    
                elif event == 'recording_started':
                    sys.stderr.write("🎤 开始接收音频...\n")
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
                        sys.stderr.write(f"\r📊 已接收: 包#{sequence + 1}, 累计{duration:.2f}秒")
                        
                    except Exception as e:
                        sys.stderr.write(f"\n❌ 音频解码失败: {e}\n")
                        
                elif event == 'recording_stopped':
                    sys.stderr.write(f"\n🛑 录音结束\n")
                    self.is_recording = False
                    self.save_audio()
                    
                    # 发送响应音频
                    sys.stderr.write("⏳ 等待0.5秒后发送响应音频...\n")
                    time.sleep(0.5)
                    self.send_response_audio()
                    
            except json.JSONDecodeError:
                # 非JSON格式，忽略
                pass
                
    def send_response_audio(self):
        """发送响应音频到ESP32"""
        sys.stderr.write("📤 开始发送响应音频...\n")
        
        # 发送开始事件
        start_msg = json.dumps({
            "event": "response_started",
            "timestamp": int(time.time() * 1000)
        })
        print(start_msg)
        sys.stdout.flush()
        
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
            
            # 发送数据（通过标准输出）
            print(data_msg)
            sys.stdout.flush()
            
            total_sent += len(chunk_data)
            sequence += 1
            
            # 显示进度
            progress = total_sent / len(self.response_audio) * 100
            sys.stderr.write(f"\r📤 发送进度: {progress:.1f}% ({total_sent}/{len(self.response_audio)} 字节)")
            
            # 短暂延时，避免发送过快
            time.sleep(0.01)
        
        # 发送结束事件
        stop_msg = json.dumps({
            "event": "response_stopped",
            "timestamp": int(time.time() * 1000)
        })
        print(stop_msg)
        sys.stdout.flush()
        
        sys.stderr.write(f"\n✅ 响应音频发送完成: {sequence} 个数据包\n")
        sys.stderr.write(f"   音频时长: {len(self.response_audio) / 2 / SAMPLE_RATE:.2f} 秒\n")
    
    def save_audio(self):
        """保存音频数据为MP3文件"""
        if not self.audio_buffer:
            sys.stderr.write("⚠️  没有音频数据可保存\n")
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
            sys.stderr.write(f"✅ 音频已保存: {mp3_filename}\n")
            sys.stderr.write(f"   时长: {duration:.2f} 秒\n")
            
        except Exception as e:
            sys.stderr.write(f"❌ 保存音频失败: {e}\n")
            
    def run(self):
        """从标准输入读取数据"""
        sys.stderr.write("="*60 + "\n")
        sys.stderr.write("ESP32音频接收器（管道版本）\n")
        sys.stderr.write("="*60 + "\n")
        sys.stderr.write("响应音频: light_on.h (约2.87秒)\n")
        sys.stderr.write("注意：所有状态信息输出到stderr，不干扰数据流\n")
        sys.stderr.write("="*60 + "\n\n")
        
        try:
            # 从标准输入读取
            for line in sys.stdin:
                self.process_line(line)
                
        except KeyboardInterrupt:
            sys.stderr.write("\n\n⚠️  程序已停止\n")


def main():
    receiver = PipeAudioReceiver()
    receiver.run()


if __name__ == "__main__":
    main()