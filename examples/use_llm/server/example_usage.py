#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
千问大模型集成服务器使用示例
演示如何发送PCM音频数据并接收处理后的音频回复
"""

import requests
import wave
import os

def send_audio_to_server(audio_file_path, server_url="http://localhost:8080"):
    """
    发送音频文件到服务器并获取回复
    
    Args:
        audio_file_path: 音频文件路径（WAV格式）
        server_url: 服务器地址
    
    Returns:
        bool: 是否成功
    """
    
    # 检查文件是否存在
    if not os.path.exists(audio_file_path):
        print(f"错误: 找不到音频文件 {audio_file_path}")
        return False
    
    # 读取WAV文件并转换为PCM
    try:
        with wave.open(audio_file_path, 'rb') as wav_file:
            pcm_data = wav_file.readframes(wav_file.getnframes())
            print(f"读取音频文件: {audio_file_path}")
            print(f"PCM数据大小: {len(pcm_data)} 字节")
            print(f"采样率: {wav_file.getframerate()} Hz")
            print(f"声道数: {wav_file.getnchannels()}")
            print(f"采样位数: {wav_file.getsampwidth() * 8} bit")
    except Exception as e:
        print(f"错误: 无法读取音频文件 - {e}")
        return False
    
    # 发送到服务器
    try:
        print(f"\n发送音频数据到服务器: {server_url}")
        
        files = {'audio': ('audio.pcm', pcm_data, 'application/octet-stream')}
        response = requests.post(f"{server_url}/process_audio", files=files, timeout=60)
        
        if response.status_code == 200:
            # 保存返回的音频
            output_file = "server_response.mp3"
            with open(output_file, 'wb') as f:
                f.write(response.content)
            
            print(f"✅ 成功! 服务器回复已保存为: {output_file}")
            print(f"回复音频大小: {len(response.content)} 字节")
            return True
        else:
            print(f"❌ 请求失败: HTTP {response.status_code}")
            print(f"错误信息: {response.text}")
            return False
            
    except requests.exceptions.Timeout:
        print("❌ 请求超时，请检查网络连接或服务器状态")
        return False
    except Exception as e:
        print(f"❌ 请求失败: {e}")
        return False

def main():
    """主函数"""
    print("=== 千问大模型集成服务器使用示例 ===\n")
    
    # 检查服务器状态
    server_url = "http://localhost:8080"
    try:
        response = requests.get(f"{server_url}/health", timeout=5)
        if response.status_code == 200:
            print(f"✅ 服务器运行正常: {server_url}")
        else:
            print(f"❌ 服务器状态异常: HTTP {response.status_code}")
            return
    except Exception as e:
        print(f"❌ 无法连接到服务器: {e}")
        print("请确保服务器已启动 (python3 server.py)")
        return
    
    # 使用示例音频文件
    audio_file = "asr_example.wav"
    
    if os.path.exists(audio_file):
        print(f"\n使用示例音频文件: {audio_file}")
        success = send_audio_to_server(audio_file, server_url)
        
        if success:
            print("\n🎉 测试完成！你可以播放 server_response.mp3 听取服务器的回复")
        else:
            print("\n❌ 测试失败")
    else:
        print(f"\n❌ 找不到示例音频文件: {audio_file}")
        print("请确保在正确的目录中运行此脚本")

if __name__ == '__main__':
    main()
