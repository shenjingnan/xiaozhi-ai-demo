#!/usr/bin/env python3
"""
音频录制工具
用于手动控制音频录制，支持开始/停止录音并保存为MP3格式
Python 3.9 兼容
"""

import os
import sys
import time
import threading
import queue
from datetime import datetime
import pyaudio
import wave
from pydub import AudioSegment

# 音频参数配置
RATE = 16000          # 采样率 16kHz
CHANNELS = 1          # 单声道
FORMAT = pyaudio.paInt16  # 16位
CHUNK = 1024          # 每个缓冲区的帧数


class AudioRecorder:
    """音频录制器类"""
    
    def __init__(self):
        self.is_recording = False
        self.audio_queue = queue.Queue()
        self.record_thread = None
        self.p = pyaudio.PyAudio()
        self.stream = None
        self.start_time = None
        
        # 确保输出目录存在
        self.output_dir = os.path.join(os.path.dirname(__file__), "user_audios")
        os.makedirs(self.output_dir, exist_ok=True)
        
    def _record_audio(self):
        """录音线程函数"""
        try:
            # 打开音频流
            self.stream = self.p.open(
                format=FORMAT,
                channels=CHANNELS,
                rate=RATE,
                input=True,
                frames_per_buffer=CHUNK
            )
            
            print("🎤 录音已开始...")
            
            while self.is_recording:
                try:
                    # 读取音频数据
                    data = self.stream.read(CHUNK, exception_on_overflow=False)
                    self.audio_queue.put(data)
                except Exception as e:
                    print(f"录音过程中出错: {e}")
                    break
                    
        finally:
            # 清理资源
            if self.stream:
                self.stream.stop_stream()
                self.stream.close()
                
    def start_recording(self):
        """开始录音"""
        if self.is_recording:
            print("⚠️  已经在录音中...")
            return
            
        self.is_recording = True
        self.start_time = datetime.now()
        self.audio_queue = queue.Queue()  # 清空队列
        
        # 启动录音线程
        self.record_thread = threading.Thread(target=self._record_audio)
        self.record_thread.start()
        
    def stop_recording(self):
        """停止录音并保存文件"""
        if not self.is_recording:
            print("⚠️  当前没有在录音...")
            return
            
        self.is_recording = False
        
        # 等待录音线程结束
        if self.record_thread:
            self.record_thread.join(timeout=2)
            
        print("🛑 录音已停止")
        
        # 收集所有音频数据
        audio_frames = []
        while not self.audio_queue.empty():
            audio_frames.append(self.audio_queue.get())
            
        if not audio_frames:
            print("⚠️  没有录制到音频数据")
            return
            
        # 生成文件名
        timestamp = self.start_time.strftime("%Y%m%d_%H%M%S")
        wav_filename = os.path.join(self.output_dir, f"recording_{timestamp}.wav")
        mp3_filename = os.path.join(self.output_dir, f"recording_{timestamp}.mp3")
        
        # 保存为WAV文件
        with wave.open(wav_filename, 'wb') as wf:
            wf.setnchannels(CHANNELS)
            wf.setsampwidth(self.p.get_sample_size(FORMAT))
            wf.setframerate(RATE)
            wf.writeframes(b''.join(audio_frames))
            
        print(f"✅ WAV文件已保存: {wav_filename}")
        
        # 转换为MP3
        try:
            audio = AudioSegment.from_wav(wav_filename)
            audio.export(mp3_filename, format="mp3", bitrate="128k")
            print(f"✅ MP3文件已保存: {mp3_filename}")
            
            # 删除临时WAV文件
            os.remove(wav_filename)
            
            # 计算录音时长
            duration = (datetime.now() - self.start_time).total_seconds()
            print(f"📊 录音时长: {duration:.2f} 秒")
            
        except Exception as e:
            print(f"❌ MP3转换失败: {e}")
            print(f"   WAV文件保留在: {wav_filename}")
            
    def cleanup(self):
        """清理资源"""
        self.is_recording = False
        if self.stream:
            self.stream.close()
        self.p.terminate()


def main():
    """主函数"""
    print("=" * 50)
    print("音频录制工具")
    print("=" * 50)
    print("命令说明:")
    print("  start - 开始录音")
    print("  stop  - 停止录音并保存")
    print("  quit  - 退出程序")
    print("=" * 50)
    print()
    
    recorder = AudioRecorder()
    
    try:
        while True:
            command = input("请输入命令 (start/stop/quit): ").strip().lower()
            
            if command == "start":
                recorder.start_recording()
            elif command == "stop":
                recorder.stop_recording()
            elif command == "quit" or command == "exit":
                print("👋 正在退出...")
                if recorder.is_recording:
                    recorder.stop_recording()
                break
            else:
                print("❓ 未知命令，请输入 start、stop 或 quit")
                
    except KeyboardInterrupt:
        print("\n\n⚠️  检测到中断信号")
        if recorder.is_recording:
            print("正在停止录音...")
            recorder.stop_recording()
    finally:
        recorder.cleanup()
        print("程序已退出")


if __name__ == "__main__":
    # 检查依赖
    try:
        import pyaudio
        import pydub
    except ImportError as e:
        print(f"❌ 缺少必要的依赖: {e}")
        print("请安装: pip install pyaudio pydub")
        sys.exit(1)
        
    main()