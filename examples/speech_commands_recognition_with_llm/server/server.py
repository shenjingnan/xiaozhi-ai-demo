#!/usr/bin/env python3
"""
🎯 ESP32智能语音助手WebSocket服务器

📖 使用指南
================
这是一个让ESP32具备AI对话能力的服务器程序！就像给你的硬件装上了"大脑"。

🎮 快速开始（3步搞定）：
   第1步：安装依赖
   pip install websockets pydub asyncio numpy scipy

   第2步：设置API密钥（二选一）
   方法A（推荐）：export DASHSCOPE_API_KEY='你的密钥'
   方法B：在代码第117行直接填写 self.api_key = "你的密钥"

   第3步：运行服务器
   python server.py

📝 主要功能：
   1. 接收ESP32发送的实时音频流
   2. 将音频发送给AI大模型进行语音识别和理解
   3. 接收大模型的语音响应并转发给ESP32播放
   4. 支持实时双向音频流传输

🌟 工作流程（像打电话一样简单）：
   1. ESP32听到"你好小智" → 开始录音
   2. 录音通过WiFi发送到本服务器
   3. 服务器转发给阿里云AI → AI理解并生成回复
   4. AI的语音回复发回ESP32 → 扬声器播放
   5. 完成一次智能对话！

🔧 环境要求：
   - Python 3.8或更高版本
   - ESP32和电脑在同一WiFi下
   - 阿里云DashScope账号（免费额度够用）

💡 常见问题：
   Q: ESP32连不上服务器？
   A: 检查防火墙是否允许8888端口，确认IP地址正确

   Q: AI不回复？
   A: 检查API密钥是否正确设置，网络是否正常

   Q: 音频有杂音？
   A: 检查麦克风接线，远离干扰源

🎯 获取API密钥：
   1. 访问 https://dashscope.console.aliyun.com/
   2. 注册/登录阿里云账号
   3. 创建API-KEY（有免费额度）

📚 更多帮助：
   - WebSocket是什么？一种可以双向实时通信的网络协议
   - 异步编程是什么？让程序可以同时处理多个任务
   - PCM是什么？一种未压缩的原始音频格式
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
import socket

# 尝试导入服务器版本的客户端，如果没有则使用原版
from omni_realtime_client import (
    OmniRealtimeClient,
    TurnDetectionMode,
)

OMNI_CLIENT_AVAILABLE = True

# 🎵 音频参数配置
SAMPLE_RATE = 16000  # ESP32使用的采样率 16kHz
MODEL_SAMPLE_RATE = 24000  # 大模型输出的采样率 24kHz
CHANNELS = 1  # 单声道（节省带宽，语音识别不需要立体声）
BIT_DEPTH = 16  # 16位深度（CD音质标准）
BYTES_PER_SAMPLE = 2  # 16位 = 2字节

# 🌐 WebSocket服务器配置
WS_HOST = "0.0.0.0"  # 监听所有网络接口（允许局域网访问）
WS_PORT = 8888  # WebSocket端口（确保防火墙允许此端口）

# 💡 新手提示：
# - 采样率越高，音质越好，但数据量也越大
# - 16kHz对语音识别来说已经足够
# - 24kHz是大模型生成的高质量音频


class WebSocketAudioServer:
    """
    🎙️ WebSocket音频服务器

    这是整个系统的核心类，负责：
    1. 管理WebSocket连接
    2. 处理音频数据流
    3. 与AI大模型通信
    4. 音频格式转换和重采样

    💡 设计理念：
    - 使用异步编程支持多客户端并发连接
    - 流式处理音频，减少延迟
    - 自动保存音频记录便于调试
    """

    def __init__(self):
        """
        🏗️ 初始化服务器

        主要工作：
        1. 创建音频存储目录
        2. 加载API密钥
        3. 初始化大模型客户端
        """
        # 📁 设置音频保存目录
        self.output_dir = os.path.join(
            os.path.dirname(__file__), "user_records"
        )  # 用户录音
        self.response_dir = os.path.join(
            os.path.dirname(__file__), "response_records"
        )  # AI响应

        # 确保目录存在（如果不存在会自动创建）
        os.makedirs(self.output_dir, exist_ok=True)
        os.makedirs(self.response_dir, exist_ok=True)

        # 🔑 配置API密钥
        # 初始化Omni Realtime客户端
        # 优先从环境变量获取 API 密钥（推荐方式）
        self.api_key = os.environ.get("DASHSCOPE_API_KEY")

        # ⚠️ 注意事项：
        # 如果环境变量没有设置，可以在这里硬编码（不推荐在生产环境使用）
        # 警告：请勿将 API 密钥提交到版本控制系统（如Git）
        # self.api_key = "your-api-key-here"  # 请替换为您的实际 API 密钥

        # 💡 新手提示：
        # 在终端设置环境变量：export DASHSCOPE_API_KEY='sk-xxxxx'
        # Windows用户使用：set DASHSCOPE_API_KEY=sk-xxxxx

        if not self.api_key or not OMNI_CLIENT_AVAILABLE:
            if not self.api_key:
                print(
                    "⚠️  警告: 未设置DASHSCOPE_API_KEY环境变量\n"
                    "   请通过以下方式之一配置API密钥：\n"
                    "   1. 设置环境变量: export DASHSCOPE_API_KEY='your-api-key'\n"
                    "   2. 在代码中硬编码（仅限开发环境）"
                )
            self.use_model = False
        else:
            self.use_model = True
            print("✅ 已配置大模型API，将使用AI生成响应音频")

    async def handle_client(self, websocket, path):
        """
        🤝 处理客户端连接

        这是WebSocket连接的主处理函数，每个客户端连接都会调用此函数。

        参数：
            websocket: WebSocket连接对象
            path: 连接路径（通常为'/'）

        功能流程：
            1. 接收客户端消息（JSON控制消息或二进制音频数据）
            2. 根据消息类型执行相应操作
            3. 管理与大模型的连接和通信
            4. 处理异常和清理资源

        💡 异步编程说明：
        - async/await 允许在等待I/O操作时处理其他连接
        - 不会阻塞其他客户端的请求
        """
        client_ip = websocket.remote_address[0]
        print(f"\n🔗 新的客户端连接: {client_ip}")

        # 📊 客户端状态管理
        # 每个连接维护独立的状态，支持多客户端并发
        client_state = {
            "is_recording": False,  # 是否正在录音
            "realtime_client": None,  # 大模型客户端实例
            "message_task": None,  # 消息处理任务
            "audio_buffer": bytearray(),  # 音频缓冲区（用于保存录音）
            "audio_tracker": {  # 音频发送跟踪器
                "total_sent": 0,  # 已发送的总字节数
                "last_time": time.time(),  # 最后发送时间
            },
        }

        try:
            async for message in websocket:
                try:
                    # 🔍 检查消息类型
                    if isinstance(message, bytes):
                        # 🎵 二进制音频数据 - 实时转发到LLM
                        # 💡 WebSocket可以传输文本（JSON）和二进制数据
                        if (
                            client_state["is_recording"]
                            and client_state["realtime_client"]
                        ):
                            # 保存到缓冲区（用于本地录音文件）
                            client_state["audio_buffer"].extend(message)

                            # 🚀 实时转发到LLM
                            # Base64编码是因为WebSocket文本消息需要ASCII字符
                            encoded_data = base64.b64encode(message).decode("utf-8")

                            # 构建大模型API事件格式
                            event = {
                                "event_id": "event_"
                                + str(int(time.time() * 1000)),  # 唯一事件ID
                                "type": "input_audio_buffer.append",  # 追加音频数据
                                "audio": encoded_data,  # Base64编码的音频
                            }

                            # 异步发送，不阻塞后续音频接收
                            await client_state["realtime_client"].send_event(event)
                            print(f"   📤 实时转发音频块: {len(message)} 字节")
                        continue

                    # 解析JSON消息
                    data = json.loads(message)
                    event = data.get("event")

                    if event == "wake_word_detected":
                        # 🎯 唤醒词检测事件
                        print(f"🎉 [{client_ip}] 检测到唤醒词！")
                        # 💡 此时ESP32已经被唤醒，准备接收用户指令

                    elif event == "recording_started":
                        # 🎙️ 开始录音事件
                        print(f"🎤 [{client_ip}] 开始录音...")
                        client_state["is_recording"] = True
                        client_state["audio_buffer"] = bytearray()
                        client_state["audio_tracker"] = {
                            "total_sent": 0,
                            "last_time": time.time(),
                        }

                        # 🤖 初始化LLM连接
                        # 💡 每次录音开始时创建新的大模型连接，确保状态独立
                        if self.use_model:
                            try:
                                # 创建大模型客户端实例
                                # 📌 关键参数说明：
                                # - base_url: 阿里云大模型的WebSocket端点
                                # - model: 使用的模型版本
                                # - voice: 语音合成的音色
                                # - on_audio_delta: 音频流回调函数
                                # - turn_detection_mode: 手动模式，由我们控制何时生成响应
                                client_state["realtime_client"] = OmniRealtimeClient(
                                    base_url="wss://dashscope.aliyuncs.com/api-ws/v1/realtime",
                                    api_key=self.api_key,
                                    model="qwen-omni-turbo-realtime-2025-05-08",
                                    voice="Chelsie",
                                    # 🎵 音频流回调函数
                                    # 当大模型生成音频片段时，立即转发给ESP32
                                    # 使用lambda创建异步任务，实现流式传输
                                    on_audio_delta=lambda audio: asyncio.create_task(
                                        self.on_audio_delta_handler(
                                            websocket,
                                            client_ip,
                                            audio,
                                            client_state["audio_tracker"],
                                        )
                                    ),
                                    turn_detection_mode=TurnDetectionMode.MANUAL,
                                )

                                # 连接到大模型
                                await client_state["realtime_client"].connect()

                                # 启动消息处理
                                client_state["message_task"] = asyncio.create_task(
                                    client_state["realtime_client"].handle_messages()
                                )

                                print(f"✅ [{client_ip}] LLM连接成功，准备接收实时音频")

                            except Exception as e:
                                print(f"❌ [{client_ip}] 初始化大模型失败: {e}")
                                client_state["realtime_client"] = None

                    elif event == "recording_ended":
                        # 🏁 录音结束事件
                        print(f"✅ [{client_ip}] 录音结束")
                        client_state["is_recording"] = False

                        # 💡 录音结束后的处理流程：
                        # 1. 保存用户录音到本地
                        # 2. 触发大模型生成响应
                        # 3. 流式发送响应音频给ESP32

                        # 保存音频
                        if len(client_state["audio_buffer"]) > 0:
                            print(
                                f"📊 [{client_ip}] 音频总大小: {len(client_state['audio_buffer'])} 字节 ({len(client_state['audio_buffer'])/2/SAMPLE_RATE:.2f}秒)"
                            )

                            # 保存音频
                            current_timestamp = datetime.now()
                            saved_file = await self.save_audio(
                                [bytes(client_state["audio_buffer"])], current_timestamp
                            )
                            if saved_file:
                                print(f"✅ [{client_ip}] 音频已保存: {saved_file}")

                        # 🤖 触发LLM响应生成
                        if self.use_model and client_state["realtime_client"]:
                            try:
                                # 📌 手动触发响应生成
                                # 因为我们使用MANUAL模式，需要明确告诉大模型开始生成响应
                                await client_state["realtime_client"].create_response()

                                # ⏳ 等待响应完成（最多30秒）
                                print(f"🤖 [{client_ip}] 等待模型生成响应...")
                                max_wait_time = 30  # 超时保护，避免无限等待
                                start_time = time.time()

                                # 💡 等待策略说明：
                                # - 每100ms检查一次状态
                                # - 如果2秒内没有新音频，认为响应结束
                                # - 最多等待30秒避免超时

                                while time.time() - start_time < max_wait_time:
                                    await asyncio.sleep(0.1)

                                    # 如果超过2秒没有新的音频数据发送，认为响应结束
                                    if (
                                        client_state["audio_tracker"]["total_sent"] > 0
                                        and time.time()
                                        - client_state["audio_tracker"]["last_time"]
                                        > 2.0
                                    ):
                                        print(
                                            f"✅ [{client_ip}] 响应音频发送完成，总计: {client_state['audio_tracker']['total_sent']} 字节"
                                        )
                                        break

                                # 如果没有收到任何音频响应，只打印警告
                                if client_state["audio_tracker"]["total_sent"] == 0:
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
                        client_state["is_recording"] = False
                        client_state["audio_buffer"] = bytearray()

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
            if client_state["realtime_client"]:
                try:
                    if client_state["message_task"]:
                        client_state["message_task"].cancel()
                    await client_state["realtime_client"].close()
                except:
                    pass

    async def on_audio_delta_handler(
        self, websocket, client_ip, audio_data, audio_tracker
    ):
        """
        🎵 处理模型返回的音频片段

        这是流式音频处理的核心函数，实现低延迟的语音响应。

        参数：
            websocket: WebSocket连接对象
            client_ip: 客户端IP地址
            audio_data: 音频数据（24kHz采样率）
            audio_tracker: 音频发送跟踪器

        主要工作：
            1. 音频重采样（24kHz → 16kHz）
            2. 立即发送给ESP32
            3. 更新发送统计

        💡 流式处理的优势：
        - 减少首字节延迟
        - 用户听到第一个字就知道系统在响应
        - 提升交互体验
        """
        try:
            # 🔄 音频重采样
            # 大模型输出24kHz，ESP32需要16kHz
            # 必须转换采样率，否则播放速度会不正确
            resampled = self.resample_audio(audio_data, MODEL_SAMPLE_RATE, SAMPLE_RATE)

            # 立即发送到ESP32
            await websocket.send(resampled)
            print(f"   → 流式发送音频块: {len(resampled)} 字节")

            # 更新音频跟踪信息
            audio_tracker["total_sent"] += len(resampled)
            audio_tracker["last_time"] = time.time()

        except Exception as e:
            print(f"❌ [{client_ip}] 发送音频块失败: {e}")

    async def save_audio(self, audio_buffer, timestamp):
        """
        💾 保存音频数据为MP3文件

        参数：
            audio_buffer: 音频数据列表
            timestamp: 时间戳

        返回：
            str: 保存的文件路径，失败返回None

        功能：
            1. 合并音频数据
            2. 保存为WAV格式
            3. 转换为MP3格式（节省空间）
            4. 删除临时WAV文件

        💡 为什么保存音频：
        - 调试和分析
        - 训练自定义模型
        - 用户隐私合规记录
        """
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
        """
        🔄 重采样音频数据

        参数：
            audio_data: 原始音频数据（字节）
            from_rate: 原始采样率（Hz）
            to_rate: 目标采样率（Hz）

        返回：
            bytes: 重采样后的音频数据

        算法说明：
            1. 优先使用scipy的高质量重采样
            2. 如果scipy不可用，使用简单的线性插值

        💡 采样率转换原理：
        - 采样率决定每秒采集多少个音频样本
        - 24kHz转16kHz需要减少样本数
        - 使用插值算法保持音频质量

        ⚠️ 注意事项：
        - 重采样可能引入少量失真
        - 对语音识别影响较小
        """
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

    def get_local_ips(self):
        """
        🌐 获取本机所有可用的IP地址

        返回：
            list: IP地址列表

        功能：
            1. 获取所有网络接口的IP
            2. 过滤掉回环地址
            3. 支持多网卡环境

        💡 使用场景：
        - ESP32需要知道服务器的局域网IP
        - 在路由器后面时，使用内网IP
        - 支持有线和无线网络
        """
        ips = []
        try:
            # 获取主机名
            hostname = socket.gethostname()

            # 获取所有网络接口的IP地址
            for info in socket.getaddrinfo(hostname, None):
                # 只获取IPv4地址
                if info[0] == socket.AF_INET:
                    ip = info[4][0]
                    if ip not in ips and not ip.startswith("127."):
                        ips.append(ip)

            # 如果上面的方法没有获取到IP，尝试另一种方法
            if not ips:
                # 创建一个UDP socket来获取本机IP
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                try:
                    # 连接到一个外部地址（不会真正发送数据）
                    s.connect(("8.8.8.8", 80))
                    ip = s.getsockname()[0]
                    if ip not in ips and not ip.startswith("127."):
                        ips.append(ip)
                except:
                    pass
                finally:
                    s.close()

            # 始终添加localhost
            ips.append("127.0.0.1")

        except Exception as e:
            print(f"⚠️  获取本机IP地址失败: {e}")
            ips = ["127.0.0.1"]

        return ips

    async def start_server(self):
        """
        🚀 启动WebSocket服务器

        主要工作：
            1. 显示服务器信息
            2. 列出所有可用的连接地址
            3. 创建WebSocket服务器实例
            4. 永久运行直到手动停止

        💡 新手提示：
        - 确保防火墙允许8888端口
        - ESP32和电脑需要在同一网络
        - 使用Ctrl+C停止服务器
        """
        print("=" * 60)
        print("ESP32音频WebSocket服务器")
        print("=" * 60)

        # 获取所有可用的IP地址
        local_ips = self.get_local_ips()
        print("可用的连接地址:")
        for ip in local_ips:
            print(f"  - ws://{ip}:{WS_PORT}")

        if self.use_model:
            print(f"\n响应模式: AI大模型生成响应")
            print(f"模型: qwen-omni-turbo-realtime")
        else:
            print(f"\n响应模式: 未启用（需要设置 DASHSCOPE_API_KEY）")
            print(f"提示: 设置环境变量 DASHSCOPE_API_KEY 以启用AI响应")
        print("=" * 60)
        print("\n等待ESP32连接...\n")

        # 创建WebSocket服务器
        async with websockets.serve(self.handle_client, WS_HOST, WS_PORT):
            await asyncio.Future()  # 永远运行


def main():
    """
    🎯 程序入口点

    功能：
        1. 创建服务器实例
        2. 运行异步事件循环
        3. 处理键盘中断

    💡 Python异步编程说明：
    - asyncio.run() 创建并运行事件循环
    - 事件循环管理所有异步任务
    - 支持高并发连接处理
    """
    server = WebSocketAudioServer()

    try:
        # 🏃 运行服务器
        asyncio.run(server.start_server())
    except KeyboardInterrupt:
        # 👋 优雅退出
        print("\n\n⚠️  服务器已停止")


if __name__ == "__main__":
    main()

# 🎉 恭喜你看完了整个代码！
#
# 📚 学到了什么？
# 1. WebSocket服务器的基本实现
# 2. 异步编程的实际应用
# 3. 音频数据的处理和转换
# 4. 与AI API的实时通信
#
# 🚀 下一步可以尝试：
# 1. 修改语音风格（voice参数）
# 2. 添加更多功能（如多语言支持）
# 3. 优化音频质量（调整采样率）
# 4. 增加错误重试机制
#
# 💪 加油！你已经掌握了AI语音助手的核心技术！
