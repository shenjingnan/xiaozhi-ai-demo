#!/usr/bin/env python3
"""
WebSocket客户端测试工具
用于测试WebSocket服务器是否正常工作
"""

import asyncio
import websockets
import json
import base64
import numpy as np

# 音频参数
SAMPLE_RATE = 16000
DURATION = 2  # 秒

async def test_client():
    uri = "ws://localhost:8888"
    print(f"正在连接到 {uri}...")
    
    try:
        async with websockets.connect(uri) as websocket:
            print("✅ 连接成功!")
            
            # 1. 发送唤醒词检测事件
            wake_msg = json.dumps({
                "event": "wake_word_detected",
                "model": "TestClient",
                "timestamp": 123456
            })
            await websocket.send(wake_msg)
            print("📤 发送: 唤醒词检测事件")
            
            # 2. 发送开始录音事件
            start_msg = json.dumps({
                "event": "recording_started",
                "timestamp": 123457
            })
            await websocket.send(start_msg)
            print("📤 发送: 开始录音事件")
            
            # 3. 生成并发送测试音频数据
            # 生成一个1kHz的正弦波
            t = np.linspace(0, DURATION, SAMPLE_RATE * DURATION)
            audio_data = (np.sin(2 * np.pi * 1000 * t) * 32767).astype(np.int16)
            
            # 分块发送
            chunk_size = 4096
            sequence = 0
            
            for i in range(0, len(audio_data), chunk_size // 2):
                chunk = audio_data[i:i + chunk_size // 2]
                chunk_bytes = chunk.tobytes()
                encoded_data = base64.b64encode(chunk_bytes).decode('ascii')
                
                audio_msg = json.dumps({
                    "event": "audio_data",
                    "sequence": sequence,
                    "size": len(chunk_bytes),
                    "data": encoded_data
                })
                
                await websocket.send(audio_msg)
                sequence += 1
                
                # 短暂延时
                await asyncio.sleep(0.01)
            
            print(f"📤 发送: {sequence} 个音频数据包")
            
            # 4. 发送结束录音事件
            stop_msg = json.dumps({
                "event": "recording_stopped",
                "timestamp": 123458
            })
            await websocket.send(stop_msg)
            print("📤 发送: 结束录音事件")
            
            # 5. 接收响应
            print("\n等待服务器响应...")
            response_started = False
            response_packets = 0
            
            while True:
                try:
                    message = await asyncio.wait_for(websocket.recv(), timeout=5.0)
                    data = json.loads(message)
                    event = data.get('event')
                    
                    if event == 'response_started':
                        print("📥 收到: 响应开始")
                        response_started = True
                    elif event == 'response_audio':
                        response_packets += 1
                        print(f"\r📥 收到: 响应音频包 #{response_packets}", end='')
                    elif event == 'response_stopped':
                        print(f"\n📥 收到: 响应结束 (共{response_packets}个数据包)")
                        break
                        
                except asyncio.TimeoutError:
                    print("\n⏱️ 超时：未收到更多响应")
                    break
            
            print("\n✅ 测试完成!")
            
    except Exception as e:
        print(f"❌ 错误: {e}")

if __name__ == "__main__":
    print("="*60)
    print("WebSocket客户端测试工具")
    print("="*60)
    print("测试内容：")
    print("1. 连接到WebSocket服务器")
    print("2. 发送模拟的录音数据")
    print("3. 接收服务器响应音频")
    print("="*60)
    
    asyncio.run(test_client())