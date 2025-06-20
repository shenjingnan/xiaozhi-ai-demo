#!/usr/bin/env python3
"""
测试修复后的客户端代码
直接调用start_conversation方法，不需要交互式输入
"""

import asyncio
import sys
sys.path.append('/Users/nemo/Documents/PlatformIO/Projects/xiaozhi-replica/examples/use_llm/server')

from client import PythonTestClient
import logging

# 配置日志
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

async def test_client():
    """测试客户端"""
    client = PythonTestClient()
    
    try:
        # 连接到服务器
        await client.connect()
        logger.info("✓ 连接成功")
        
        # 等待握手完成
        await asyncio.sleep(1)
        
        # 直接开始对话测试（5秒录音）
        logger.info("开始对话测试...")
        await client.start_conversation(5)
        
    except Exception as e:
        logger.error(f"测试失败: {e}")
        import traceback
        traceback.print_exc()
    finally:
        client.cleanup()
        await client.disconnect()

if __name__ == "__main__":
    asyncio.run(test_client())