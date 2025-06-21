#!/usr/bin/env python3
"""
ESP32音频WebSocket服务器
通过WebSocket与ESP32双向通信
"""

import os
import sys
import json
import base64
import wave
import asyncio
import websockets
from datetime import datetime
from pydub import AudioSegment
import time
from send_custom_audio import get_custom_audio

# 音频参数配置
SAMPLE_RATE = 16000      # 采样率 16kHz
CHANNELS = 1             # 单声道
BIT_DEPTH = 16          # 16位
BYTES_PER_SAMPLE = 2    # 16位 = 2字节

# WebSocket服务器配置
WS_HOST = "0.0.0.0"  # 监听所有接口
WS_PORT = 8888       # WebSocket端口


class WebSocketAudioServer:
    """WebSocket音频服务器"""
    
    def __init__(self):
        self.output_dir = os.path.join(os.path.dirname(__file__), "user_records")
        # 确保输出目录存在
        os.makedirs(self.output_dir, exist_ok=True)
        
        # 加载light_on音频数据
        self.response_audio = get_custom_audio("light_on")
        print(f"✅ 已加载响应音频: {len(self.response_audio)} 字节 (约{len(self.response_audio)/2/SAMPLE_RATE:.1f}秒)")
        
    async def handle_client(self, websocket, path):
        """处理客户端连接"""
        client_ip = websocket.remote_address[0]
        print(f"\n🔗 新的客户端连接: {client_ip}")
        
        try:
            async for message in websocket:
                try:
                    # 检查消息类型
                    if isinstance(message, bytes):
                        # 二进制音频数据
                        print(f"🎤 [{client_ip}] 接收到二进制音频数据: {len(message)} 字节")
                        
                        # 保存音频
                        current_timestamp = datetime.now()
                        saved_file = await self.save_audio([message], current_timestamp)
                        if saved_file:
                            print(f"✅ [{client_ip}] 音频已保存: {saved_file}")
                        
                        # 等待一下再发送响应
                        print(f"⏳ [{client_ip}] 等待0.5秒后发送响应音频...")
                        await asyncio.sleep(0.5)
                        
                        # 发送响应音频
                        await self.send_response_audio(websocket, client_ip)
                        continue
                    
                    # 解析JSON消息
                    data = json.loads(message)
                    event = data.get('event')
                    
                    if event == 'wake_word_detected':
                        print(f"🎉 [{client_ip}] 检测到唤醒词！")
                        
                    elif event == 'audio_data':
                        # JSON格式的音频数据（保留兼容性）
                        audio_base64 = data.get('data', '')
                        audio_size = data.get('size', 0)
                        
                        print(f"🎤 [{client_ip}] 接收到JSON格式音频数据: {audio_size} 字节")
                        
                        try:
                            # Base64解码
                            audio_bytes = base64.b64decode(audio_base64)
                            
                            # 保存音频
                            current_timestamp = datetime.now()
                            saved_file = await self.save_audio([audio_bytes], current_timestamp)
                            if saved_file:
                                print(f"✅ [{client_ip}] 音频已保存: {saved_file}")
                            
                            # 等待一下再发送响应
                            print(f"⏳ [{client_ip}] 等待0.5秒后发送响应音频...")
                            await asyncio.sleep(0.5)
                            
                            # 发送响应音频
                            await self.send_response_audio(websocket, client_ip)
                            
                        except Exception as e:
                            print(f"❌ [{client_ip}] 音频处理失败: {e}")
                        
                except json.JSONDecodeError as e:
                    print(f"❌ [{client_ip}] JSON解析错误: {e}")
                except Exception as e:
                    print(f"❌ [{client_ip}] 处理消息错误: {e}")
                    
        except websockets.exceptions.ConnectionClosed:
            print(f"🔌 [{client_ip}] 客户端断开连接")
        except Exception as e:
            print(f"❌ [{client_ip}] 连接错误: {e}")
            
    async def send_response_audio(self, websocket, client_ip):
        """发送响应音频到ESP32"""
        print(f"📤 [{client_ip}] 发送响应音频...")
        
        try:
            # 直接发送二进制PCM数据
            print(f"   音频数据长度: {len(self.response_audio)} 字节")
            print(f"   音频时长: {len(self.response_audio) / 2 / SAMPLE_RATE:.2f} 秒")
            
            # 一次性发送所有二进制数据
            await websocket.send(self.response_audio)
            
            # 发送一个ping包作为音频结束标志
            await websocket.ping()
            
            print(f"✅ [{client_ip}] 响应音频发送完成")
            
        except Exception as e:
            print(f"❌ [{client_ip}] 发送音频失败: {e}")
    
    async def save_audio(self, audio_buffer, timestamp):
        """保存音频数据为MP3文件"""
        if not audio_buffer:
            print("⚠️  没有音频数据可保存")
            return None
            
        try:
            # 合并所有音频数据
            audio_data = b''.join(audio_buffer)
            
            # 生成文件名
            timestamp_str = timestamp.strftime("%Y%m%d_%H%M%S") if timestamp else datetime.now().strftime("%Y%m%d_%H%M%S")
            wav_filename = os.path.join(self.output_dir, f"recording_{timestamp_str}.wav")
            mp3_filename = os.path.join(self.output_dir, f"recording_{timestamp_str}.mp3")
            
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
            print(f"\n✅ 音频信息:")
            print(f"   时长: {duration:.2f} 秒")
            print(f"   大小: {len(audio_data) / 1024:.1f} KB")
            
            return mp3_filename
            
        except Exception as e:
            print(f"\n❌ 保存音频失败: {e}")
            return None
            
    async def start_server(self):
        """启动WebSocket服务器"""
        print("="*60)
        print("ESP32音频WebSocket服务器")
        print("="*60)
        print(f"监听地址: ws://{WS_HOST}:{WS_PORT}")
        print(f"响应音频: light_on.h (约2.87秒)")
        print("="*60)
        print("\n等待ESP32连接...\n")
        
        # 创建WebSocket服务器
        async with websockets.serve(self.handle_client, WS_HOST, WS_PORT):
            await asyncio.Future()  # 永远运行


def main():
    server = WebSocketAudioServer()
    
    try:
        # 运行服务器
        asyncio.run(server.start_server())
    except KeyboardInterrupt:
        print("\n\n⚠️  服务器已停止")


if __name__ == "__main__":
    main()