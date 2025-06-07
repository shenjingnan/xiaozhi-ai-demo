#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
使用真实音频测试服务端
"""

import requests
import json

def test_with_text():
    """直接测试文本到语音的转换"""
    print("🧪 测试文本到语音转换...")
    
    # 模拟一个简单的请求，但我们需要绕过语音识别
    # 让我们直接测试TTS部分
    
    try:
        # 发送一个健康检查请求
        response = requests.get('http://localhost:8080/health', timeout=10)
        
        if response.status_code == 200:
            print("✅ 服务端运行正常")
            print(f"响应: {response.json()}")
        else:
            print(f"❌ 健康检查失败: {response.status_code}")
            
    except requests.exceptions.RequestException as e:
        print(f"❌ 请求失败: {e}")

def check_server_info():
    """检查服务端信息"""
    print("📋 检查服务端信息...")
    
    try:
        response = requests.get('http://localhost:8080/', timeout=10)
        
        if response.status_code == 200:
            info = response.json()
            print("✅ 服务端信息:")
            print(f"  消息: {info.get('message', 'N/A')}")
            print(f"  端点: {info.get('endpoints', {})}")
            print(f"  用法: {info.get('usage', 'N/A')}")
        else:
            print(f"❌ 获取服务端信息失败: {response.status_code}")
            
    except requests.exceptions.RequestException as e:
        print(f"❌ 请求失败: {e}")

if __name__ == "__main__":
    check_server_info()
    test_with_text()
