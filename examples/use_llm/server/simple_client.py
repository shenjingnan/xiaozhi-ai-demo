#!/usr/bin/env python3
"""
简化版客户端测试
专门用于诊断倒计时卡住问题
"""

import asyncio
import websockets
import json
import base64
import pyaudio
import threading
import queue
import logging

# 配置日志
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# 音频参数
SAMPLE_RATE = 16000
CHUNK_SIZE = 3200
FORMAT = pyaudio.paInt16
CHANNELS = 1

class SimpleClient:
    def __init__(self):
        self.websocket = None
        self.p = pyaudio.PyAudio()
        self.recording = False
        self.audio_queue = queue.Queue()
        
    async def connect(self):
        """连接到服务器"""
        logger.info("连接到服务器...")
        self.websocket = await websockets.connect("ws://127.0.0.1:8888")
        
        # 发送握手
        await self.websocket.send(json.dumps({
            "type": "hello",
            "client_type": "python"
        }))
        
        response = await self.websocket.recv()
        logger.info(f"握手成功: {response}")
        
    def start_recording(self):
        """开始录音"""
        logger.info("启动录音线程...")
        self.recording = True
        self.recording_thread = threading.Thread(target=self._record_worker, daemon=True)
        self.recording_thread.start()
        
    def stop_recording(self):
        """停止录音"""
        logger.info("停止录音...")
        self.recording = False
        
    def _record_worker(self):
        """录音工作线程"""
        try:
            logger.info("录音线程开始")
            stream = self.p.open(
                format=FORMAT,
                channels=CHANNELS,
                rate=SAMPLE_RATE,
                input=True,
                frames_per_buffer=CHUNK_SIZE
            )
            logger.info("录音设备已打开")
            
            while self.recording:
                try:
                    audio_data = stream.read(CHUNK_SIZE, exception_on_overflow=False)
                    self.audio_queue.put(audio_data)
                except Exception as e:
                    logger.error(f"录音错误: {e}")
                    break
                    
            stream.stop_stream()
            stream.close()
            logger.info("录音线程结束")
            
        except Exception as e:
            logger.error(f"录音线程错误: {e}")
            self.recording = False
    
    async def send_audio_chunks(self):
        """发送音频块"""
        logger.info("开始发送音频块...")
        chunk_count = 0
        
        while self.recording:
            try:
                audio_data = self.audio_queue.get(timeout=0.1)
                
                # 编码并发送
                encoded = base64.b64encode(audio_data).decode('utf-8')
                await self.websocket.send(json.dumps({
                    "type": "audio_chunk",
                    "data": encoded,
                    "format": "pcm16",
                    "sample_rate": SAMPLE_RATE
                }))
                
                chunk_count += 1
                if chunk_count % 10 == 0:  # 每10个块打印一次日志
                    logger.info(f"已发送 {chunk_count} 个音频块")
                
                self.audio_queue.task_done()
                
            except queue.Empty:
                await asyncio.sleep(0.01)
            except Exception as e:
                logger.error(f"发送音频块错误: {e}")
                break
        
        logger.info(f"音频发送完成，总共发送 {chunk_count} 个块")
        
    async def test_conversation(self):
        """测试对话"""
        logger.info("=== 开始简化对话测试 ===")
        
        # 发送开始监听
        await self.websocket.send(json.dumps({"type": "start_listening"}))
        logger.info("✓ 发送开始监听信号")
        
        # 开始录音
        self.start_recording()
        logger.info("✓ 录音已开始")
        
        # 创建音频发送任务
        audio_task = asyncio.create_task(self.send_audio_chunks())
        logger.info("✓ 音频发送任务已创建")
        
        # 倒计时测试
        duration = 5
        logger.info(f"开始 {duration} 秒倒计时测试...")
        
        for i in range(duration, 0, -1):
            logger.info(f"倒计时: {i} 秒")
            await asyncio.sleep(1)
            
            # 检查录音状态
            if not self.recording:
                logger.warning("录音意外停止!")
                break
        
        logger.info("倒计时完成")
        
        # 停止录音
        self.stop_recording()
        
        # 等待音频任务完成
        try:
            audio_task.cancel()
            await audio_task
        except asyncio.CancelledError:
            pass
            
        # 发送停止监听
        await self.websocket.send(json.dumps({"type": "stop_listening"}))
        logger.info("✓ 发送停止监听信号")
        
        logger.info("=== 测试完成 ===")
        
    def cleanup(self):
        """清理资源"""
        self.recording = False
        if hasattr(self, 'p'):
            self.p.terminate()

async def main():
    client = SimpleClient()
    
    try:
        await client.connect()
        await client.test_conversation()
    except Exception as e:
        logger.error(f"测试失败: {e}")
        import traceback
        traceback.print_exc()
    finally:
        client.cleanup()
        if client.websocket:
            await client.websocket.close()

if __name__ == "__main__":
    asyncio.run(main())