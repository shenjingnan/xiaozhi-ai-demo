#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32-LLM WebSocket服务器
用于接收ESP32和Python客户端的音频数据，通过Qwen Omni Turbo Realtime模型处理并返回音频回复
"""

import asyncio
import websockets
import json
import base64
import os
import time
import logging
from typing import Dict, Optional, Set
from datetime import datetime
from omni_realtime_client import OmniRealtimeClient, TurnDetectionMode

# 配置日志
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# WebSocket服务器配置
WS_HOST = "0.0.0.0"  # 监听所有网络接口
WS_PORT = 8888

# 客户端连接管理
class ClientConnection:
    """客户端连接信息"""
    def __init__(self, websocket, client_id: str, client_type: str = "unknown"):
        self.websocket = websocket
        self.client_id = client_id
        self.client_type = client_type  # "esp32" 或 "python"
        self.connected_at = datetime.now()
        self.llm_client: Optional[OmniRealtimeClient] = None
        self.is_listening = False
        self.audio_buffer = []
        
    async def cleanup(self):
        """清理连接资源"""
        if self.llm_client:
            await self.llm_client.close()

class AudioRelayServer:
    """音频中继服务器，连接ESP32/Python客户端与LLM"""
    
    def __init__(self):
        self.clients: Dict[str, ClientConnection] = {}
        self.client_counter = 0
        
    def generate_client_id(self) -> str:
        """生成唯一的客户端ID"""
        self.client_counter += 1
        return f"client_{self.client_counter}_{int(time.time())}"
    
    async def handle_llm_text(self, client: ClientConnection, text: str):
        """处理LLM返回的文本"""
        logger.info(f"[{client.client_id}] LLM文本回复: {text}")
        # 发送文本给客户端（可选）
        await client.websocket.send(json.dumps({
            "type": "text",
            "data": text
        }))
    
    async def handle_llm_audio(self, client: ClientConnection, audio_data: bytes):
        """处理LLM返回的音频数据"""
        logger.debug(f"[{client.client_id}] 收到LLM音频数据: {len(audio_data)} 字节")
        # 将音频数据转发给客户端
        await client.websocket.send(json.dumps({
            "type": "audio_chunk",
            "data": base64.b64encode(audio_data).decode('utf-8'),
            "format": "pcm16",
            "sample_rate": 24000
        }))
    
    async def handle_llm_input_transcript(self, client: ClientConnection, transcript: str):
        """处理输入语音转文本结果"""
        logger.info(f"[{client.client_id}] 用户说: {transcript}")
        # 通知客户端识别结果
        await client.websocket.send(json.dumps({
            "type": "input_transcript",
            "data": transcript
        }))
    
    async def handle_llm_output_transcript(self, client: ClientConnection, transcript: str):
        """处理输出语音转文本结果"""
        logger.info(f"[{client.client_id}] AI回复: {transcript}")
    
    async def handle_audio_done(self, client: ClientConnection, event: dict):
        """处理音频传输完成事件"""
        logger.info(f"[{client.client_id}] AI音频传输完成")
        await client.websocket.send(json.dumps({
            "type": "audio_done"
        }))
    
    async def handle_response_done(self, client: ClientConnection, event: dict):
        """处理整个响应完成事件"""
        logger.info(f"[{client.client_id}] AI完整响应结束")
        await client.websocket.send(json.dumps({
            "type": "response_done"
        }))
    
    async def setup_llm_client(self, client: ClientConnection) -> bool:
        """为客户端设置LLM连接"""
        try:
            # 创建LLM客户端
            client.llm_client = OmniRealtimeClient(
                base_url="wss://dashscope.aliyuncs.com/api-ws/v1/realtime",
                api_key=os.environ.get("DASHSCOPE_API_KEY"),
                model="qwen-omni-turbo-realtime-2025-05-08",
                voice="Chelsie",
                turn_detection_mode=TurnDetectionMode.MANUAL,  # 使用手动模式
                on_text_delta=lambda text: asyncio.create_task(
                    self.handle_llm_text(client, text)
                ),
                on_audio_delta=lambda audio: asyncio.create_task(
                    self.handle_llm_audio(client, audio)
                ),
                on_input_transcript=lambda transcript: asyncio.create_task(
                    self.handle_llm_input_transcript(client, transcript)
                ),
                on_output_transcript=lambda transcript: asyncio.create_task(
                    self.handle_llm_output_transcript(client, transcript)
                ),
                extra_event_handlers={
                    "response.audio.done": lambda event: asyncio.create_task(
                        self.handle_audio_done(client, event)
                    ),
                    "response.done": lambda event: asyncio.create_task(
                        self.handle_response_done(client, event)
                    )
                }
            )
            
            # 连接到LLM服务
            await client.llm_client.connect()
            
            # 启动消息处理
            asyncio.create_task(client.llm_client.handle_messages())
            
            logger.info(f"[{client.client_id}] LLM客户端连接成功")
            return True
            
        except Exception as e:
            logger.error(f"[{client.client_id}] LLM客户端连接失败: {e}")
            return False
    
    async def handle_client_message(self, client: ClientConnection, message: str):
        """处理客户端消息"""
        try:
            data = json.loads(message)
            msg_type = data.get("type")
            
            if msg_type == "hello":
                # 客户端握手
                client.client_type = data.get("client_type", "unknown")
                logger.info(f"[{client.client_id}] 客户端类型: {client.client_type}")
                
                # 设置LLM连接
                if await self.setup_llm_client(client):
                    await client.websocket.send(json.dumps({
                        "type": "hello_ack",
                        "status": "ready"
                    }))
                else:
                    await client.websocket.send(json.dumps({
                        "type": "error",
                        "message": "Failed to connect to LLM"
                    }))
                    
            elif msg_type == "start_listening":
                # 开始录音
                client.is_listening = True
                client.audio_buffer = []
                logger.info(f"[{client.client_id}] 开始监听音频")
                
            elif msg_type == "audio_chunk":
                # 接收音频块
                if client.is_listening and client.llm_client:
                    audio_data = base64.b64decode(data.get("data", ""))
                    client.audio_buffer.append(audio_data)
                    
                    # 转发音频到LLM
                    await client.llm_client.stream_audio(audio_data)
                    logger.debug(f"[{client.client_id}] 转发音频块: {len(audio_data)} 字节")
                    
            elif msg_type == "stop_listening":
                # 停止录音，请求LLM回复
                client.is_listening = False
                logger.info(f"[{client.client_id}] 停止监听，请求LLM回复")
                
                if client.llm_client:
                    # 触发LLM生成回复
                    await client.llm_client.create_response()
                    
            elif msg_type == "ping":
                # 心跳包
                await client.websocket.send(json.dumps({"type": "pong"}))
                
        except json.JSONDecodeError:
            logger.error(f"[{client.client_id}] 无效的JSON消息: {message}")
        except Exception as e:
            logger.error(f"[{client.client_id}] 处理消息错误: {e}")
    
    async def handle_client(self, websocket, path):
        """处理客户端连接"""
        client_id = self.generate_client_id()
        client = ClientConnection(websocket, client_id)
        self.clients[client_id] = client
        
        logger.info(f"[{client_id}] 新客户端连接: {websocket.remote_address}")
        
        try:
            async for message in websocket:
                await self.handle_client_message(client, message)
                
        except websockets.exceptions.ConnectionClosed:
            logger.info(f"[{client_id}] 客户端断开连接")
        except Exception as e:
            logger.error(f"[{client_id}] 连接错误: {e}")
        finally:
            # 清理客户端
            await client.cleanup()
            del self.clients[client_id]
            logger.info(f"[{client_id}] 客户端资源已清理")
    
    async def start(self):
        """启动WebSocket服务器"""
        logger.info(f"启动WebSocket服务器: ws://{WS_HOST}:{WS_PORT}")
        
        # 检查API密钥
        if not os.environ.get("DASHSCOPE_API_KEY"):
            logger.error("错误: 未设置DASHSCOPE_API_KEY环境变量")
            logger.error("请运行: export DASHSCOPE_API_KEY=your_api_key")
            return
        
        # 启动服务器
        async with websockets.serve(self.handle_client, WS_HOST, WS_PORT):
            logger.info("服务器启动成功，等待客户端连接...")
            await asyncio.Future()  # 永久运行

async def main():
    """主函数"""
    server = AudioRelayServer()
    await server.start()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("服务器已停止")