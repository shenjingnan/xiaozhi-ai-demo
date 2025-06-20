#!/usr/bin/env python3
"""
简单的WebSocket测试服务器
"""

import asyncio
import websockets
import logging

logging.basicConfig(level=logging.DEBUG)
logger = logging.getLogger(__name__)

async def handle_client(websocket, path):
    """处理客户端连接"""
    logger.info(f"新客户端连接: {websocket.remote_address}")
    
    try:
        async for message in websocket:
            logger.info(f"收到消息: {message}")
            await websocket.send(f"Echo: {message}")
    except websockets.exceptions.ConnectionClosed:
        logger.info("客户端已断开")
    except Exception as e:
        logger.error(f"错误: {e}")

async def main():
    """主函数"""
    logger.info("启动测试WebSocket服务器: ws://0.0.0.0:8889")
    
    async with websockets.serve(handle_client, "0.0.0.0", 8889):
        logger.info("服务器启动成功")
        await asyncio.Future()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("服务器已停止")