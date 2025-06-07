#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
测试音频格式转换
"""

import requests
import io
import wave
import tempfile
import os

def create_test_pcm():
    """创建一个测试用的PCM音频文件"""
    # 创建一个简单的16kHz单声道16位PCM音频（1秒的静音）
    sample_rate = 16000
    duration = 1  # 1秒
    samples = sample_rate * duration
    
    # 生成静音数据（16位，小端）
    pcm_data = b'\x00\x00' * samples
    
    return pcm_data

def pcm_to_wav(pcm_data, sample_rate=16000, channels=1, sample_width=2):
    """将PCM数据转换为WAV格式"""
    wav_buffer = io.BytesIO()
    with wave.open(wav_buffer, 'wb') as wav_file:
        wav_file.setnchannels(channels)
        wav_file.setsampwidth(sample_width)
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(pcm_data)
    wav_buffer.seek(0)
    return wav_buffer.getvalue()

def test_server():
    """测试服务端音频处理"""
    print("🧪 开始测试服务端音频格式...")
    
    # 创建测试PCM数据
    test_pcm = create_test_pcm()
    print(f"📝 创建测试PCM数据: {len(test_pcm)} 字节")
    
    # 转换为WAV格式（服务端需要WAV格式输入）
    test_wav = pcm_to_wav(test_pcm)
    print(f"📝 转换为WAV格式: {len(test_wav)} 字节")
    
    # 发送到服务端
    try:
        print("📤 发送音频到服务端...")
        
        files = {'audio': ('test.wav', test_wav, 'audio/wav')}
        response = requests.post('http://localhost:8080/process_audio', files=files, timeout=30)
        
        if response.status_code == 200:
            response_data = response.content
            print(f"✅ 服务端响应成功: {len(response_data)} 字节")
            print(f"📋 响应Content-Type: {response.headers.get('Content-Type', 'unknown')}")
            
            # 检查响应数据是否是PCM格式
            if len(response_data) > 0:
                # 简单检查：PCM数据应该是偶数字节（16位采样）
                if len(response_data) % 2 == 0:
                    print("✅ 响应数据格式正确（字节数为偶数，符合16位PCM）")
                else:
                    print("⚠️  响应数据格式可能有问题（字节数为奇数）")
                
                # 保存响应数据用于检查
                with open('test_response.pcm', 'wb') as f:
                    f.write(response_data)
                print("💾 响应数据已保存为 test_response.pcm")
            else:
                print("❌ 响应数据为空")
        else:
            print(f"❌ 服务端响应错误: {response.status_code}")
            print(f"错误信息: {response.text}")
            
    except requests.exceptions.RequestException as e:
        print(f"❌ 请求失败: {e}")
    except Exception as e:
        print(f"❌ 测试失败: {e}")

if __name__ == "__main__":
    test_server()
