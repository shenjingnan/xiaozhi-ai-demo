#!/usr/bin/env python3
"""
简单的WebSocket测试客户端
"""

import asyncio
import websockets
import logging

logging.basicConfig(level=logging.DEBUG)
logger = logging.getLogger(__name__)

async def main():
    """主函数"""
    try:
        logger.info("连接到测试服务器: ws://127.0.0.1:8889")
        
        async with websockets.connect("ws://127.0.0.1:8889") as websocket:
            logger.info("连接成功!")
            
            # 发送测试消息
            await websocket.send("Hello, Server!")
            
            # 接收回复
            response = await websocket.recv()
            logger.info(f"收到回复: {response}")
            
    except Exception as e:
        logger.error(f"连接失败: {e}")

if __name__ == "__main__":
    asyncio.run(main())