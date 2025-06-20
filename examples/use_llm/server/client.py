#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Python测试客户端
模拟ESP32设备与WebSocket服务器的交互，用于测试语音助手功能
"""

import asyncio
import websockets
import json
import base64
import pyaudio
import threading
import time
import queue
import logging
from typing import Optional

# 配置日志
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# 音频参数配置
SAMPLE_RATE = 16000  # ESP32使用16kHz采样率
CHUNK_SIZE = 3200    # 音频块大小，与model_demo保持一致
FORMAT = pyaudio.paInt16
CHANNELS = 1  # 单声道

# 播放音频参数
PLAYBACK_RATE = 24000  # LLM返回的音频是24kHz
PLAYBACK_CHUNK = 1024

class PythonTestClient:
    """Python测试客户端"""
    
    def __init__(self, server_url: str = "ws://127.0.0.1:8888"):
        self.server_url = server_url
        self.websocket: Optional[websockets.WebSocketClientProtocol] = None
        self.is_connected = False
        
        # 音频设备
        self.p = pyaudio.PyAudio()
        self.recording = False
        self.playing = False
        
        # 音频队列
        self.audio_queue = queue.Queue()
        self.playback_queue = queue.Queue()
        
        # 状态标记
        self.audio_transfer_complete = False
        self.response_complete = False
        
        # 线程
        self.recording_thread: Optional[threading.Thread] = None
        self.playback_thread: Optional[threading.Thread] = None
        
    async def connect(self):
        """连接到WebSocket服务器"""
        try:
            logger.info(f"连接到服务器: {self.server_url}")
            self.websocket = await websockets.connect(self.server_url)
            self.is_connected = True
            
            # 发送握手消息
            await self.websocket.send(json.dumps({
                "type": "hello",
                "client_type": "python"
            }))
            
            logger.info("已发送握手消息，等待服务器响应...")
            
        except Exception as e:
            logger.error(f"连接失败: {e}")
            raise
    
    async def disconnect(self):
        """断开连接"""
        self.is_connected = False
        if self.websocket:
            await self.websocket.close()
    
    def start_playback_thread(self):
        """启动音频播放线程"""
        if self.playback_thread and self.playback_thread.is_alive():
            return
            
        self.playing = True
        self.playback_thread = threading.Thread(target=self._playback_worker, daemon=True)
        self.playback_thread.start()
        logger.info("音频播放线程已启动")
    
    def _playback_worker(self):
        """音频播放工作线程"""
        try:
            logger.info("正在初始化播放设备...")
            stream = self.p.open(
                format=FORMAT,
                channels=CHANNELS,
                rate=PLAYBACK_RATE,  # 24kHz播放
                output=True,
                frames_per_buffer=PLAYBACK_CHUNK
            )
            logger.info("播放设备初始化成功")
            
            try:
                while self.playing:
                    try:
                        audio_data = self.playback_queue.get(timeout=0.1)
                        if audio_data is None:  # 停止信号
                            break
                        stream.write(audio_data)
                        self.playback_queue.task_done()
                    except queue.Empty:
                        continue
                    except Exception as e:
                        logger.error(f"播放错误: {e}")
                        continue
            finally:
                stream.stop_stream()
                stream.close()
                logger.info("播放设备已关闭")
                
        except Exception as e:
            logger.error(f"播放设备初始化失败: {e}")
            logger.error("请检查音频输出设备状态")
            self.playing = False
    
    def start_recording(self):
        """开始录音"""
        if self.recording:
            logger.warning("已经在录音中")
            return
        
        self.recording = True
        self.recording_thread = threading.Thread(target=self._recording_worker, daemon=True)
        self.recording_thread.start()
        logger.info("开始录音...")
    
    def stop_recording(self):
        """停止录音"""
        if not self.recording:
            return
        
        self.recording = False
        if self.recording_thread:
            self.recording_thread.join(timeout=1)
        logger.info("停止录音")
    
    def _recording_worker(self):
        """录音工作线程"""
        try:
            logger.info("正在初始化录音设备...")
            stream = self.p.open(
                format=FORMAT,
                channels=CHANNELS,
                rate=SAMPLE_RATE,
                input=True,
                frames_per_buffer=CHUNK_SIZE
            )
            logger.info("录音设备初始化成功")
            
            try:
                while self.recording:
                    try:
                        audio_data = stream.read(CHUNK_SIZE, exception_on_overflow=False)
                        self.audio_queue.put(audio_data)
                    except Exception as e:
                        logger.error(f"录音错误: {e}")
                        break
            finally:
                stream.stop_stream()
                stream.close()
                logger.info("录音设备已关闭")
                
        except Exception as e:
            logger.error(f"录音设备初始化失败: {e}")
            logger.error("请检查麦克风权限和设备状态")
            self.recording = False
    
    async def send_audio_chunks(self):
        """发送音频数据块到服务器"""
        while self.recording and self.is_connected:
            try:
                # 使用非阻塞方式获取音频数据
                try:
                    audio_data = self.audio_queue.get_nowait()
                except queue.Empty:
                    # 如果队列为空，异步等待一小段时间再重试
                    await asyncio.sleep(0.01)
                    continue
                
                # 编码音频数据
                encoded_audio = base64.b64encode(audio_data).decode('utf-8')
                
                # 发送音频块
                await self.websocket.send(json.dumps({
                    "type": "audio_chunk",
                    "data": encoded_audio,
                    "format": "pcm16",
                    "sample_rate": SAMPLE_RATE
                }))
                
                self.audio_queue.task_done()
                
            except Exception as e:
                logger.error(f"发送音频数据失败: {e}")
                break
    
    async def handle_server_message(self, message: str):
        """处理服务器消息"""
        try:
            data = json.loads(message)
            msg_type = data.get("type")
            
            if msg_type == "hello_ack":
                logger.info("服务器握手确认，状态: " + data.get("status", "unknown"))
                
            elif msg_type == "audio_chunk":
                # 接收音频数据块
                audio_data = base64.b64decode(data.get("data", ""))
                self.playback_queue.put(audio_data)
                logger.debug(f"收到音频块: {len(audio_data)} 字节")
                
            elif msg_type == "audio_done":
                # 音频传输完成标记
                logger.info("🎧 AI音频传输完成")
                self.audio_transfer_complete = True
                
            elif msg_type == "response_done":
                # 整个响应完成
                logger.info("✅ AI响应完全结束")
                self.response_complete = True
                
            elif msg_type == "text":
                logger.info(f"AI文本回复: {data.get('data', '')}")
                
            elif msg_type == "input_transcript":
                logger.info(f"语音识别结果: {data.get('data', '')}")
                
            elif msg_type == "error":
                logger.error(f"服务器错误: {data.get('message', '')}")
                
            elif msg_type == "pong":
                logger.debug("收到心跳回复")
                
        except json.JSONDecodeError:
            logger.error(f"无效的JSON消息: {message}")
        except Exception as e:
            logger.error(f"处理消息错误: {e}")
    
    async def start_conversation(self, record_duration: int = 5):
        """开始对话"""
        if not self.is_connected:
            logger.error("未连接到服务器")
            return
        
        logger.info("初始化对话...")
        
        # 发送开始监听信号
        await self.websocket.send(json.dumps({
            "type": "start_listening"
        }))
        logger.info("✓ 已发送开始监听信号")
        
        # 启动音频播放线程（延迟启动以避免卡住）
        try:
            self.start_playback_thread()
            logger.info("✓ 播放线程已启动")
        except Exception as e:
            logger.warning(f"播放线程启动失败: {e}，将跳过音频播放")
        
        # 开始录音和发送音频
        self.start_recording()
        logger.info("✓ 录音已启动")
        
        # 创建音频发送任务
        audio_task = asyncio.create_task(self.send_audio_chunks())
        logger.info("✓ 音频发送任务已创建")
        
        try:
            logger.info(f"🎤 对话已开始，请说话... (将录制 {record_duration} 秒)")
            
            # 等待录音设备初始化完成
            await asyncio.sleep(0.5)
            
            # 检查录音是否成功开始
            if not self.recording:
                logger.error("录音设备初始化失败，对话已取消")
                return
            
            # 显示倒计时
            for i in range(record_duration, 0, -1):
                if not self.recording:  # 检查录音状态
                    logger.warning("录音已意外停止")
                    break
                logger.info(f"⏱️  录音中... 剩余 {i} 秒")
                await asyncio.sleep(1)
            
            logger.info("⏹️  录音时间结束")
            
        except KeyboardInterrupt:
            logger.info("用户取消对话")
        finally:
            # 停止录音
            self.stop_recording()
            
            # 取消音频发送任务
            try:
                audio_task.cancel()
                await audio_task
            except asyncio.CancelledError:
                pass
            
            # 发送停止监听信号
            await self.websocket.send(json.dumps({
                "type": "stop_listening"
            }))
            
            logger.info("🤖 录音已停止，正在请求AI回复...")
            
            # 等待AI回复（设置超时）
            await self.wait_for_ai_response(timeout=30)
    
    async def wait_for_ai_response(self, timeout: int = 30):
        """等待AI回复"""
        try:
            start_time = asyncio.get_event_loop().time()
            audio_received = False
            total_audio_bytes = 0
            
            # 重置状态标记
            self.audio_transfer_complete = False
            self.response_complete = False
            
            logger.info("🤖 等待AI回复...")
            
            while (asyncio.get_event_loop().time() - start_time) < timeout:
                try:
                    # 等待消息，设置短超时以便检查总体超时
                    message = await asyncio.wait_for(self.websocket.recv(), timeout=1.0)
                    await self.handle_server_message(message)
                    
                    # 检查消息类型
                    data = json.loads(message)
                    msg_type = data.get("type")
                    
                    if msg_type == "audio_chunk":
                        if not audio_received:
                            audio_received = True
                            logger.info("🎧 开始接收AI音频数据...")
                        audio_data = base64.b64decode(data.get("data", ""))
                        total_audio_bytes += len(audio_data)
                        
                    elif msg_type == "audio_done":
                        logger.info(f"🎧 音频传输完成，总共 {total_audio_bytes} 字节")
                        
                        # 等待音频播放完成
                        await self.wait_for_playback_finish()
                        break
                        
                    elif msg_type == "input_transcript":
                        logger.info(f"📝 语音识别: {data.get('data', '')}")
                    elif msg_type == "text":
                        logger.info(f"💬 AI文本回复: {data.get('data', '')}")
                        
                except asyncio.TimeoutError:
                    # 1秒内没有消息，继续等待
                    continue
                except json.JSONDecodeError:
                    # 非JSON消息，继续等待
                    continue
                    
            if not audio_received:
                logger.warning("超时：未收到AI音频回复")
            else:
                logger.info("✓ AI回复处理完成")
                
        except Exception as e:
            logger.error(f"等待AI回复时出错: {e}")
    
    async def wait_for_playback_finish(self):
        """等待音频播放完成"""
        logger.info("🔊 等待音频播放完成...")
        
        # 等待音频队列清空且留出缓冲时间
        max_wait_time = 10  # 最多等待10秒
        wait_start = asyncio.get_event_loop().time()
        
        while (asyncio.get_event_loop().time() - wait_start) < max_wait_time:
            # 检查音频队列是否为空
            if self.playback_queue.empty():
                # 队列为空，等待一点时间让最后的音频播放完成
                await asyncio.sleep(1)
                if self.playback_queue.empty():
                    logger.info("✓ 音频播放完成")
                    break
            else:
                # 队列不为空，继续等待
                await asyncio.sleep(0.5)
        
        if (asyncio.get_event_loop().time() - wait_start) >= max_wait_time:
            logger.warning("音频播放等待超时")
    
    async def main_loop(self):
        """主循环"""
        try:
            # 启动消息处理
            async for message in self.websocket:
                await self.handle_server_message(message)
                
        except websockets.exceptions.ConnectionClosed:
            logger.info("与服务器连接已断开")
        except Exception as e:
            logger.error(f"消息处理错误: {e}")
    
    def cleanup(self):
        """清理资源"""
        self.recording = False
        self.playing = False
        
        # 停止播放线程
        if self.playback_thread and self.playback_thread.is_alive():
            self.playback_queue.put(None)  # 停止信号
            self.playback_thread.join(timeout=1)
        
        # 清空队列
        while not self.audio_queue.empty():
            try:
                self.audio_queue.get_nowait()
            except queue.Empty:
                break
        while not self.playback_queue.empty():
            try:
                self.playback_queue.get_nowait()
            except queue.Empty:
                break
        
        # 关闭音频设备
        if hasattr(self, 'p'):
            self.p.terminate()

async def interactive_mode(client: PythonTestClient):
    """交互模式"""
    logger.info("=== Python语音助手测试客户端 ===")
    logger.info("输入命令:")
    logger.info("  start      - 开始对话 (默认5秒录音)")
    logger.info("  start 10   - 开始对话 (录音10秒)")
    logger.info("  quick      - 快速测试 (3秒录音)")
    logger.info("  quit       - 退出")
    
    while True:
        try:
            command = await asyncio.to_thread(input, "\n> ")
            command = command.strip().lower()
            
            if command == "quit":
                break
            elif command == "start":
                await client.start_conversation(5)  # 默认5秒
            elif command == "quick":
                await client.start_conversation(3)  # 快速3秒测试
            elif command.startswith("start "):
                # 解析自定义时长
                try:
                    duration = int(command.split()[1])
                    if 1 <= duration <= 30:
                        await client.start_conversation(duration)
                    else:
                        logger.info("录音时长必须在1-30秒之间")
                except (ValueError, IndexError):
                    logger.info("无效的时长格式，请输入: start 5")
            else:
                logger.info("未知命令，请查看上方帮助信息")
                
        except KeyboardInterrupt:
            break
    
    logger.info("退出交互模式")

async def main():
    """主函数"""
    client = PythonTestClient()
    
    try:
        # 连接到服务器
        await client.connect()
        
        # 等待握手完成
        await asyncio.sleep(1)
        
        # 启动交互模式
        await interactive_mode(client)
        
    except Exception as e:
        logger.error(f"程序错误: {e}")
    finally:
        client.cleanup()
        await client.disconnect()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("程序已退出")