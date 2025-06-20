#!/usr/bin/env python3
"""
简化版WebSocket服务器（不包含LLM连接）
用于测试基本WebSocket连接功能
"""

import asyncio
import websockets
import json
import logging
import time
from typing import Dict

# 配置日志
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# WebSocket服务器配置
WS_HOST = "0.0.0.0"
WS_PORT = 8888

class SimpleServer:
    """简化版WebSocket服务器"""
    
    def __init__(self):
        self.clients: Dict[str, websockets.WebSocketServerProtocol] = {}
        self.client_counter = 0
        
    def generate_client_id(self) -> str:
        """生成唯一的客户端ID"""
        self.client_counter += 1
        return f"client_{self.client_counter}_{int(time.time())}"
    
    async def handle_client(self, websocket, path):
        """处理客户端连接"""
        client_id = self.generate_client_id()
        logger.info(f"[{client_id}] 新客户端连接: {websocket.remote_address}")
        
        self.clients[client_id] = websocket
        
        try:
            async for message in websocket:
                await self.handle_message(client_id, websocket, message)
        except websockets.exceptions.ConnectionClosed:
            logger.info(f"[{client_id}] 连接已关闭")
        except Exception as e:
            logger.error(f"[{client_id}] 连接错误: {e}")
        finally:
            if client_id in self.clients:
                del self.clients[client_id]
            logger.info(f"[{client_id}] 客户端已清理")
    
    async def handle_message(self, client_id: str, websocket, message: str):
        """处理客户端消息"""
        try:
            logger.info(f"[{client_id}] 收到消息: {message[:100]}...")
            
            data = json.loads(message)
            msg_type = data.get("type")
            
            if msg_type == "hello":
                # 客户端握手
                client_type = data.get("client_type", "unknown")
                logger.info(f"[{client_id}] 客户端类型: {client_type}")
                
                # 发送握手确认
                await websocket.send(json.dumps({
                    "type": "hello_ack",
                    "status": "ready",
                    "client_id": client_id
                }))
                
            elif msg_type == "start_listening":
                logger.info(f"[{client_id}] 开始监听")
                await websocket.send(json.dumps({
                    "type": "status",
                    "message": "listening_started"
                }))
                
            elif msg_type == "stop_listening":
                logger.info(f"[{client_id}] 停止监听")
                await websocket.send(json.dumps({
                    "type": "status", 
                    "message": "listening_stopped"
                }))
                
                # 模拟AI回复
                await asyncio.sleep(1)
                await websocket.send(json.dumps({
                    "type": "text",
                    "data": "这是模拟的AI回复"
                }))
                
            elif msg_type == "audio_chunk":
                # 简单记录音频数据
                audio_data = data.get("data", "")
                logger.debug(f"[{client_id}] 收到音频块: {len(audio_data)} 字符")
                
            elif msg_type == "ping":
                await websocket.send(json.dumps({"type": "pong"}))
                
            else:
                logger.warning(f"[{client_id}] 未知消息类型: {msg_type}")
                
        except json.JSONDecodeError:
            logger.error(f"[{client_id}] 无效的JSON消息")
        except Exception as e:
            logger.error(f"[{client_id}] 处理消息错误: {e}")
    
    async def start(self):
        """启动WebSocket服务器"""
        logger.info(f"启动简化版WebSocket服务器: ws://{WS_HOST}:{WS_PORT}")
        
        # 启动服务器
        async with websockets.serve(self.handle_client, WS_HOST, WS_PORT):
            logger.info("服务器启动成功，等待客户端连接...")
            await asyncio.Future()  # 永久运行

async def main():
    """主函数"""
    server = SimpleServer()
    await server.start()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("服务器已停止")