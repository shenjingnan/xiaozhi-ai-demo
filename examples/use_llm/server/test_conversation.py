#!/usr/bin/env python3
"""
简化的对话测试脚本
用于测试录音停止和AI回复功能
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
PLAYBACK_RATE = 24000

class SimpleConversationTest:
    def __init__(self):
        self.websocket = None
        self.p = pyaudio.PyAudio()
        self.recording = False
        self.audio_queue = queue.Queue()
        self.playback_queue = queue.Queue()
        
    async def connect(self):
        """连接到服务器"""
        self.websocket = await websockets.connect("ws://127.0.0.1:8888")
        
        # 发送握手
        await self.websocket.send(json.dumps({
            "type": "hello",
            "client_type": "python"
        }))
        
        # 等待握手确认
        response = await self.websocket.recv()
        logger.info(f"握手确认: {response}")
        
    def start_recording(self):
        """开始录音"""
        self.recording = True
        self.recording_thread = threading.Thread(target=self._record_worker, daemon=True)
        self.recording_thread.start()
        logger.info("开始录音...")
        
    def stop_recording(self):
        """停止录音"""
        self.recording = False
        logger.info("停止录音")
        
    def _record_worker(self):
        """录音工作线程"""
        stream = self.p.open(
            format=FORMAT,
            channels=CHANNELS,
            rate=SAMPLE_RATE,
            input=True,
            frames_per_buffer=CHUNK_SIZE
        )
        
        try:
            while self.recording:
                audio_data = stream.read(CHUNK_SIZE)
                self.audio_queue.put(audio_data)
        finally:
            stream.stop_stream()
            stream.close()
            
    async def send_audio_chunks(self):
        """发送音频块"""
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
                
                self.audio_queue.task_done()
                
            except queue.Empty:
                await asyncio.sleep(0.01)
                
    def start_playback(self):
        """开始播放"""
        self.playing = True
        self.playback_thread = threading.Thread(target=self._playback_worker, daemon=True)
        self.playback_thread.start()
        
    def _playback_worker(self):
        """播放工作线程"""
        stream = self.p.open(
            format=FORMAT,
            channels=CHANNELS,
            rate=PLAYBACK_RATE,
            output=True,
            frames_per_buffer=1024
        )
        
        try:
            while getattr(self, 'playing', True):
                try:
                    audio_data = self.playback_queue.get(timeout=0.1)
                    if audio_data is None:
                        break
                    stream.write(audio_data)
                    self.playback_queue.task_done()
                except queue.Empty:
                    continue
        finally:
            stream.stop_stream()
            stream.close()
            
    async def test_conversation(self):
        """测试完整对话流程"""
        logger.info("=== 开始对话测试 ===")
        
        # 启动播放
        self.start_playback()
        
        # 发送开始监听
        await self.websocket.send(json.dumps({"type": "start_listening"}))
        logger.info("已发送开始监听信号")
        
        # 开始录音
        self.start_recording()
        
        # 创建音频发送任务
        audio_task = asyncio.create_task(self.send_audio_chunks())
        
        try:
            # 自动录音5秒
            record_duration = 5
            logger.info(f"请说话... (将录制 {record_duration} 秒)")
            
            # 显示倒计时
            for i in range(record_duration, 0, -1):
                logger.info(f"录音中... 剩余 {i} 秒")
                await asyncio.sleep(1)
            
            logger.info("录音时间结束")
            
        except KeyboardInterrupt:
            logger.info("用户取消")
        finally:
            # 停止录音
            self.stop_recording()
            
            # 等待音频发送任务完成
            try:
                audio_task.cancel()
                await audio_task
            except asyncio.CancelledError:
                pass
                
            # 发送停止监听
            await self.websocket.send(json.dumps({"type": "stop_listening"}))
            logger.info("已发送停止监听信号，等待AI回复...")
            
            # 等待AI回复
            await self.wait_for_response()
            
    async def wait_for_response(self):
        """等待AI回复"""
        timeout = 30
        start_time = asyncio.get_event_loop().time()
        
        while (asyncio.get_event_loop().time() - start_time) < timeout:
            try:
                message = await asyncio.wait_for(self.websocket.recv(), timeout=1.0)
                data = json.loads(message)
                msg_type = data.get("type")
                
                logger.info(f"收到消息: {msg_type}")
                
                if msg_type == "input_transcript":
                    logger.info(f"识别结果: {data.get('data')}")
                elif msg_type == "text":
                    logger.info(f"AI文本回复: {data.get('data')}")
                elif msg_type == "audio":
                    audio_data = base64.b64decode(data.get("data", ""))
                    logger.info(f"收到音频回复: {len(audio_data)} 字节")
                    self.playback_queue.put(audio_data)
                    
                    # 等待播放完成
                    await asyncio.sleep(3)
                    logger.info("✓ 对话测试完成")
                    return
                    
            except asyncio.TimeoutError:
                continue
            except json.JSONDecodeError:
                continue
                
        logger.warning("等待AI回复超时")
        
    def cleanup(self):
        """清理资源"""
        self.recording = False
        self.playing = False
        if hasattr(self, 'p'):
            self.p.terminate()

async def main():
    test = SimpleConversationTest()
    
    try:
        await test.connect()
        await test.test_conversation()
    except Exception as e:
        logger.error(f"测试失败: {e}")
    finally:
        test.cleanup()
        if test.websocket:
            await test.websocket.close()

if __name__ == "__main__":
    asyncio.run(main())