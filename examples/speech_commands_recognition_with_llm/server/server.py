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

# 尝试导入服务器版本的客户端，如果没有则使用原版
try:
    from examples.speech_commands_recognition_with_llm.server.omni_realtime_client import (
        OmniRealtimeClient,
        TurnDetectionMode,
    )

    OMNI_CLIENT_AVAILABLE = True
    print("✅ 使用服务器版本的大模型客户端（优化日志输出）")
except ImportError:
    # 添加model_demo目录到sys.path
    sys.path.append(os.path.join(os.path.dirname(__file__), "../model_demo"))
    try:
        from omni_realtime_client import OmniRealtimeClient, TurnDetectionMode

        OMNI_CLIENT_AVAILABLE = True
        print("⚠️  使用原版大模型客户端")
    except ImportError:
        print("⚠️  警告: 无法导入omni_realtime_client，将使用默认音频响应")
        OMNI_CLIENT_AVAILABLE = False

# 音频参数配置
SAMPLE_RATE = 16000  # ESP32使用的采样率 16kHz
MODEL_SAMPLE_RATE = 24000  # 大模型输出的采样率 24kHz
CHANNELS = 1  # 单声道
BIT_DEPTH = 16  # 16位
BYTES_PER_SAMPLE = 2  # 16位 = 2字节

# WebSocket服务器配置
WS_HOST = "0.0.0.0"  # 监听所有接口
WS_PORT = 8888  # WebSocket端口


class WebSocketAudioServer:
    """WebSocket音频服务器"""

    def __init__(self):
        self.output_dir = os.path.join(os.path.dirname(__file__), "user_records")
        self.response_dir = os.path.join(os.path.dirname(__file__), "response_records")
        # 确保输出目录存在
        os.makedirs(self.output_dir, exist_ok=True)
        os.makedirs(self.response_dir, exist_ok=True)

        # 初始化Omni Realtime客户端
        self.api_key = os.environ.get("DASHSCOPE_API_KEY")
        if not self.api_key or not OMNI_CLIENT_AVAILABLE:
            if not self.api_key:
                print(
                    "⚠️  警告: 未设置DASHSCOPE_API_KEY环境变量，将使用默认light_on音频"
                )
            self.use_model = False
        else:
            self.use_model = True
            print("✅ 已配置大模型API，将使用AI生成响应音频")


    async def handle_client(self, websocket, path):
        """处理客户端连接"""
        client_ip = websocket.remote_address[0]
        print(f"\n🔗 新的客户端连接: {client_ip}")
        
        # 客户端状态
        client_state = {
            'is_recording': False,
            'realtime_client': None,
            'message_task': None,
            'audio_buffer': bytearray(),
            'audio_tracker': {'total_sent': 0, 'last_time': time.time()}
        }

        try:
            async for message in websocket:
                try:
                    # 检查消息类型
                    if isinstance(message, bytes):
                        # 二进制音频数据 - 实时转发到LLM
                        if client_state['is_recording'] and client_state['realtime_client']:
                            # 添加到缓冲区（用于保存）
                            client_state['audio_buffer'].extend(message)
                            
                            # 实时转发到LLM
                            encoded_data = base64.b64encode(message).decode("utf-8")
                            event = {
                                "event_id": "event_" + str(int(time.time() * 1000)),
                                "type": "input_audio_buffer.append",
                                "audio": encoded_data,
                            }
                            await client_state['realtime_client'].send_event(event)
                            print(f"   实时转发音频块: {len(message)} 字节")
                        continue

                    # 解析JSON消息
                    data = json.loads(message)
                    event = data.get("event")

                    if event == "wake_word_detected":
                        print(f"🎉 [{client_ip}] 检测到唤醒词！")
                        
                    elif event == "recording_started":
                        print(f"🎤 [{client_ip}] 开始录音...")
                        client_state['is_recording'] = True
                        client_state['audio_buffer'] = bytearray()
                        client_state['audio_tracker'] = {'total_sent': 0, 'last_time': time.time()}
                        
                        # 初始化LLM连接
                        if self.use_model:
                            try:
                                # 创建大模型客户端
                                client_state['realtime_client'] = OmniRealtimeClient(
                                    base_url="wss://dashscope.aliyuncs.com/api-ws/v1/realtime",
                                    api_key=self.api_key,
                                    model="qwen-omni-turbo-realtime-2025-05-08",
                                    voice="Chelsie",
                                    on_audio_delta=lambda audio: asyncio.create_task(
                                        self.on_audio_delta_handler(websocket, client_ip, audio, client_state['audio_tracker'])
                                    ),
                                    turn_detection_mode=TurnDetectionMode.MANUAL,
                                )
                                
                                # 连接到大模型
                                await client_state['realtime_client'].connect()
                                
                                # 启动消息处理
                                client_state['message_task'] = asyncio.create_task(
                                    client_state['realtime_client'].handle_messages()
                                )
                                
                                print(f"✅ [{client_ip}] LLM连接成功，准备接收实时音频")
                                
                            except Exception as e:
                                print(f"❌ [{client_ip}] 初始化大模型失败: {e}")
                                client_state['realtime_client'] = None
                    
                    elif event == "recording_ended":
                        print(f"✅ [{client_ip}] 录音结束")
                        client_state['is_recording'] = False
                        
                        # 保存音频
                        if len(client_state['audio_buffer']) > 0:
                            print(f"📊 [{client_ip}] 音频总大小: {len(client_state['audio_buffer'])} 字节 ({len(client_state['audio_buffer'])/2/SAMPLE_RATE:.2f}秒)")
                            
                            # 保存音频
                            current_timestamp = datetime.now()
                            saved_file = await self.save_audio([bytes(client_state['audio_buffer'])], current_timestamp)
                            if saved_file:
                                print(f"✅ [{client_ip}] 音频已保存: {saved_file}")
                        
                        # 触发LLM响应生成
                        if self.use_model and client_state['realtime_client']:
                            try:
                                # 手动触发响应生成
                                await client_state['realtime_client'].create_response()
                                
                                # 等待响应完成（最多30秒）
                                print(f"🤖 [{client_ip}] 等待模型生成响应...")
                                max_wait_time = 30
                                start_time = time.time()
                                
                                while time.time() - start_time < max_wait_time:
                                    await asyncio.sleep(0.1)
                                    
                                    # 如果超过2秒没有新的音频数据发送，认为响应结束
                                    if client_state['audio_tracker']['total_sent'] > 0 and \
                                       time.time() - client_state['audio_tracker']['last_time'] > 2.0:
                                        print(f"✅ [{client_ip}] 响应音频发送完成，总计: {client_state['audio_tracker']['total_sent']} 字节")
                                        break
                                
                                # 如果没有收到任何音频响应，只打印警告
                                if client_state['audio_tracker']['total_sent'] == 0:
                                    print(f"⚠️ [{client_ip}] 未收到大模型响应")
                                
                                # 发送ping作为音频结束标志
                                await websocket.ping()
                                
                            except Exception as e:
                                print(f"❌ [{client_ip}] 模型处理失败: {e}")
                        else:
                            # 不使用模型时只打印警告
                            print(f"⚠️ [{client_ip}] 未启用AI模型，无法生成响应")
                    
                    elif event == "recording_cancelled":
                        print(f"⚠️ [{client_ip}] 录音取消")
                        client_state['is_recording'] = False
                        client_state['audio_buffer'] = bytearray()

                except json.JSONDecodeError as e:
                    print(f"❌ [{client_ip}] JSON解析错误: {e}")
                except Exception as e:
                    print(f"❌ [{client_ip}] 处理消息错误: {e}")

        except websockets.exceptions.ConnectionClosed:
            print(f"🔌 [{client_ip}] 客户端断开连接")
        except Exception as e:
            print(f"❌ [{client_ip}] 连接错误: {e}")
        finally:
            # 清理资源
            if client_state['realtime_client']:
                try:
                    if client_state['message_task']:
                        client_state['message_task'].cancel()
                    await client_state['realtime_client'].close()
                except:
                    pass

    # 删除不再需要的process_streaming_audio_with_first_message方法
    
    async def on_audio_delta_handler(self, websocket, client_ip, audio_data, audio_tracker):
        """处理模型返回的音频片段"""
        try:
            # 直接重采样并发送音频数据
            resampled = self.resample_audio(
                audio_data, MODEL_SAMPLE_RATE, SAMPLE_RATE
            )
            
            # 立即发送到ESP32
            await websocket.send(resampled)
            print(f"   → 流式发送音频块: {len(resampled)} 字节")
            
            # 更新音频跟踪信息
            audio_tracker['total_sent'] += len(resampled)
            audio_tracker['last_time'] = time.time()
            
        except Exception as e:
            print(f"❌ [{client_ip}] 发送音频块失败: {e}")
    
    async def process_streaming_audio(self, websocket, client_ip):
        """处理流式音频数据"""
        print(f"🎤 [{client_ip}] 开始接收流式音频数据...")
        
        # 音频数据缓冲区
        audio_buffer = bytearray()
        
        # 等待音频数据
        while True:
            try:
                # 接收数据（设置超时）
                message = await asyncio.wait_for(websocket.recv(), timeout=5.0)
                
                if isinstance(message, bytes):
                    # 二进制音频数据
                    audio_buffer.extend(message)
                    print(f"   收到音频块: {len(message)} 字节, 总计: {len(audio_buffer)} 字节")
                    
                    # 检查是否已经收到足够的数据（例如超过1秒）
                    if len(audio_buffer) >= SAMPLE_RATE * 2:  # 1秒的音频数据
                        # 可以开始处理了
                        break
                else:
                    # 非二进制消息，可能是控制消息
                    break
                    
            except asyncio.TimeoutError:
                # 超时，认为音频接收完成
                print(f"⏰ [{client_ip}] 音频接收超时，准备处理")
                break
            except Exception as e:
                print(f"❌ [{client_ip}] 接收音频数据失败: {e}")
                break
        
        if len(audio_buffer) > 0:
            print(f"✅ [{client_ip}] 音频接收完成，总大小: {len(audio_buffer)} 字节 ({len(audio_buffer)/2/SAMPLE_RATE:.2f}秒)")
            
            # 保存音频
            current_timestamp = datetime.now()
            saved_file = await self.save_audio([bytes(audio_buffer)], current_timestamp)
            if saved_file:
                print(f"✅ [{client_ip}] 音频已保存: {saved_file}")
            
            # 等待一下再发送响应
            print(f"⏳ [{client_ip}] 等待0.5秒后发送响应音频...")
            await asyncio.sleep(0.5)
            
            # 发送响应音频
            if self.use_model:
                await self.send_model_response_audio(websocket, client_ip, bytes(audio_buffer))
            else:
                print(f"⚠️ [{client_ip}] 未启用AI模型，无法生成响应")
        else:
            print(f"⚠️ [{client_ip}] 没有接收到音频数据")
    
    async def send_model_response_audio(self, websocket, client_ip, user_audio_data):
        """使用大模型生成并发送响应音频（流式）"""
        print(f"🤖 [{client_ip}] 使用大模型生成响应音频（流式）...")

        try:
            total_audio_sent = 0
            stream_complete = False
            last_sent_time = time.time()
            
            # 定义流式音频处理函数
            async def on_audio_delta(audio_data):
                """直接处理并发送音频片段到ESP32"""
                nonlocal total_audio_sent, last_sent_time
                try:
                    # 直接重采样并发送音频数据
                    # audio_data 是 24kHz 的音频数据，需要转换为 16kHz
                    resampled = self.resample_audio(
                        audio_data, MODEL_SAMPLE_RATE, SAMPLE_RATE
                    )
                    
                    # 立即发送到ESP32
                    await websocket.send(resampled)
                    total_audio_sent += len(resampled)
                    last_sent_time = time.time()  # 更新最后发送时间
                    print(f"   → 流式发送音频块: {len(resampled)} 字节")
                    
                except Exception as e:
                    print(f"❌ [{client_ip}] 发送音频块失败: {e}")

            # 创建大模型客户端（参考qwen_demo.py的实现）
            realtime_client = OmniRealtimeClient(
                base_url="wss://dashscope.aliyuncs.com/api-ws/v1/realtime",
                api_key=self.api_key,
                model="qwen-omni-turbo-realtime-2025-05-08",
                voice="Chelsie",
                on_audio_delta=lambda audio: asyncio.create_task(on_audio_delta(audio)),
                turn_detection_mode=TurnDetectionMode.MANUAL,  # 使用手动模式，因为我们需要控制何时生成响应
            )

            # 连接到大模型
            await realtime_client.connect()

            # 启动消息处理
            message_task = asyncio.create_task(realtime_client.handle_messages())

            # 发送用户音频数据
            print(f"   发送用户音频到大模型: {len(user_audio_data)} 字节")

            # 将音频数据分块发送（参考main.py的实现）
            chunk_size = 3200  # 每个块200ms的音频数据（16kHz * 2字节 * 0.2秒）
            for i in range(0, len(user_audio_data), chunk_size):
                chunk = user_audio_data[i : i + chunk_size]
                # Base64编码音频数据
                encoded_data = base64.b64encode(chunk).decode("utf-8")

                # 构建事件（参考main.py的格式）
                event = {
                    "event_id": "event_" + str(int(time.time() * 1000)),
                    "type": "input_audio_buffer.append",
                    "audio": encoded_data,
                }
                await realtime_client.send_event(event)

                # 小延迟避免过快发送
                await asyncio.sleep(0.01)

            # 手动触发响应生成
            await realtime_client.create_response()

            # 等待响应生成和发送完成
            max_wait_time = 30  # 最多等待30秒
            start_time = time.time()
            
            while time.time() - start_time < max_wait_time:
                await asyncio.sleep(0.1)
                
                # 如果超过1秒没有新的音频数据发送，认为流结束
                if total_audio_sent > 0 and time.time() - last_sent_time > 1.0:
                    stream_complete = True
                    break
            
            # 发送ping作为音频结束标志
            await websocket.ping()
            
            # 取消消息任务并关闭连接
            try:
                message_task.cancel()
                await message_task
            except asyncio.CancelledError:
                pass
            await realtime_client.close()
            
            if total_audio_sent > 0:
                print(f"✅ [{client_ip}] 流式音频发送完成，总计: {total_audio_sent} 字节 ({total_audio_sent/2/SAMPLE_RATE:.2f}秒)")
            else:
                print(f"⚠️  [{client_ip}] 未收到大模型响应")

        except Exception as e:
            print(f"❌ [{client_ip}] 大模型处理失败: {e}")
            import traceback

            traceback.print_exc()


    async def save_audio(self, audio_buffer, timestamp):
        """保存音频数据为MP3文件"""
        if not audio_buffer:
            print("⚠️  没有音频数据可保存")
            return None

        try:
            # 合并所有音频数据
            audio_data = b"".join(audio_buffer)

            # 生成文件名
            timestamp_str = (
                timestamp.strftime("%Y%m%d_%H%M%S")
                if timestamp
                else datetime.now().strftime("%Y%m%d_%H%M%S")
            )
            wav_filename = os.path.join(
                self.output_dir, f"recording_{timestamp_str}.wav"
            )
            mp3_filename = os.path.join(
                self.output_dir, f"recording_{timestamp_str}.mp3"
            )

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

            # 显示音频信息
            duration = len(audio_data) / BYTES_PER_SAMPLE / SAMPLE_RATE
            print(f"\n✅ 音频信息:")
            print(f"   时长: {duration:.2f} 秒")
            print(f"   大小: {len(audio_data) / 1024:.1f} KB")

            return mp3_filename

        except Exception as e:
            print(f"\n❌ 保存音频失败: {e}")
            return None

    async def save_response_audio(
        self, audio_data, timestamp, sample_rate=MODEL_SAMPLE_RATE
    ):
        """保存响应音频数据为MP3文件"""
        if not audio_data:
            print("⚠️  没有响应音频数据可保存")
            return None

        try:
            # 生成文件名
            timestamp_str = (
                timestamp.strftime("%Y%m%d_%H%M%S")
                if timestamp
                else datetime.now().strftime("%Y%m%d_%H%M%S")
            )
            wav_filename = os.path.join(
                self.response_dir, f"response_{timestamp_str}.wav"
            )
            mp3_filename = os.path.join(
                self.response_dir, f"response_{timestamp_str}.mp3"
            )

            # 保存为WAV文件（使用正确的采样率）
            with wave.open(wav_filename, "wb") as wav_file:
                wav_file.setnchannels(CHANNELS)
                wav_file.setsampwidth(BIT_DEPTH // 8)
                wav_file.setframerate(sample_rate)  # 使用传入的采样率
                wav_file.writeframes(audio_data)

            # 转换为MP3
            audio = AudioSegment.from_wav(wav_filename)
            audio.export(mp3_filename, format="mp3", bitrate="128k")

            # 删除临时WAV文件
            os.remove(wav_filename)

            # 显示音频信息
            duration = len(audio_data) / BYTES_PER_SAMPLE / sample_rate
            print(f"\n✅ 响应音频信息:")
            print(f"   时长: {duration:.2f} 秒")
            print(f"   大小: {len(audio_data) / 1024:.1f} KB")
            print(f"   采样率: {sample_rate} Hz")

            return mp3_filename

        except Exception as e:
            print(f"\n❌ 保存响应音频失败: {e}")
            return None

    def resample_audio(self, audio_data, from_rate, to_rate):
        """重采样音频数据"""
        if from_rate == to_rate:
            return audio_data

        try:
            import numpy as np
            from scipy import signal

            # 将字节数据转换为numpy数组
            audio_array = np.frombuffer(audio_data, dtype=np.int16)

            # 计算重采样后的长度
            num_samples = int(len(audio_array) * to_rate / from_rate)

            # 使用scipy进行重采样
            resampled = signal.resample(audio_array, num_samples)

            # 转换回int16
            resampled = np.clip(resampled, -32768, 32767).astype(np.int16)

            # 转换回字节数据
            return resampled.tobytes()

        except ImportError:
            # 如果没有安装scipy，使用简单的线性插值
            print("⚠️  未安装scipy，使用简单重采样方法")

            # 将字节数据转换为16位整数数组
            audio_array = []
            for i in range(0, len(audio_data), 2):
                if i + 1 < len(audio_data):
                    sample = int.from_bytes(
                        audio_data[i : i + 2], byteorder="little", signed=True
                    )
                    audio_array.append(sample)

            # 简单的重采样
            ratio = to_rate / from_rate
            resampled = []
            for i in range(int(len(audio_array) * ratio)):
                src_idx = i / ratio
                src_idx_int = int(src_idx)
                src_idx_frac = src_idx - src_idx_int

                if src_idx_int + 1 < len(audio_array):
                    # 线性插值
                    sample = int(
                        audio_array[src_idx_int] * (1 - src_idx_frac)
                        + audio_array[src_idx_int + 1] * src_idx_frac
                    )
                else:
                    sample = audio_array[min(src_idx_int, len(audio_array) - 1)]

                resampled.append(sample)

            # 转换回字节数据
            result = bytearray()
            for sample in resampled:
                result.extend(sample.to_bytes(2, byteorder="little", signed=True))

            return bytes(result)

    async def start_server(self):
        """启动WebSocket服务器"""
        print("=" * 60)
        print("ESP32音频WebSocket服务器")
        print("=" * 60)
        print(f"监听地址: ws://{WS_HOST}:{WS_PORT}")
        if self.use_model:
            print(f"响应模式: AI大模型生成响应")
            print(f"模型: qwen-omni-turbo-realtime")
        else:
            print(f"响应模式: 未启用（需要设置 DASHSCOPE_API_KEY）")
            print(f"提示: 设置环境变量 DASHSCOPE_API_KEY 以启用AI响应")
        print("=" * 60)
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
