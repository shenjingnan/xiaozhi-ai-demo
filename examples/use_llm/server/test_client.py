#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
测试客户端 - 用于测试千问大模型集成服务器
"""

import requests
import wave
import os

def wav_to_pcm(wav_file_path):
    """将WAV文件转换为PCM数据"""
    with wave.open(wav_file_path, 'rb') as wav_file:
        # 读取音频数据
        pcm_data = wav_file.readframes(wav_file.getnframes())
        return pcm_data

def test_api():
    """测试API"""
    # 服务器地址
    server_url = "http://localhost:8080"
    
    # 检查服务器健康状态
    try:
        response = requests.get(f"{server_url}/health")
        print(f"健康检查: {response.json()}")
    except Exception as e:
        print(f"无法连接到服务器: {e}")
        return
    
    # 使用现有的示例音频文件进行测试
    audio_file_path = "asr_example.wav"
    
    if not os.path.exists(audio_file_path):
        print(f"找不到测试音频文件: {audio_file_path}")
        return
    
    # 将WAV转换为PCM
    pcm_data = wav_to_pcm(audio_file_path)
    print(f"PCM数据大小: {len(pcm_data)} 字节")
    
    # 发送请求到API
    try:
        files = {'audio': ('test.pcm', pcm_data, 'application/octet-stream')}
        
        print("发送音频数据到服务器...")
        response = requests.post(f"{server_url}/process_audio", files=files)
        
        if response.status_code == 200:
            # 保存返回的音频文件
            with open('response_audio.mp3', 'wb') as f:
                f.write(response.content)
            print("成功！返回的音频已保存为 response_audio.mp3")
        else:
            print(f"请求失败: {response.status_code}")
            print(f"错误信息: {response.text}")
            
    except Exception as e:
        print(f"请求失败: {e}")

if __name__ == '__main__':
    test_api()
