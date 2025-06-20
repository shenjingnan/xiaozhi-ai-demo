#!/usr/bin/env python3
"""
音频设备测试脚本
用于诊断pyaudio相关问题
"""

import pyaudio
import logging

# 配置日志
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

def test_audio_devices():
    """测试音频设备"""
    try:
        logger.info("初始化PyAudio...")
        p = pyaudio.PyAudio()
        
        logger.info("检查音频设备数量...")
        device_count = p.get_device_count()
        logger.info(f"发现 {device_count} 个音频设备")
        
        # 列出所有设备
        logger.info("\n=== 音频设备列表 ===")
        for i in range(device_count):
            info = p.get_device_info_by_index(i)
            logger.info(f"设备 {i}: {info['name']}")
            logger.info(f"  输入通道: {info['maxInputChannels']}")
            logger.info(f"  输出通道: {info['maxOutputChannels']}")
            logger.info(f"  默认采样率: {info['defaultSampleRate']}")
            logger.info("")
        
        # 测试录音设备
        logger.info("=== 测试录音设备 ===")
        try:
            stream = p.open(
                format=pyaudio.paInt16,
                channels=1,
                rate=16000,
                input=True,
                frames_per_buffer=1024
            )
            logger.info("✓ 录音设备初始化成功")
            
            # 简单录音测试
            logger.info("录音测试中... (2秒)")
            for i in range(2):
                data = stream.read(1024, exception_on_overflow=False)
                logger.info(f"录音数据: {len(data)} 字节")
            
            stream.stop_stream()
            stream.close()
            logger.info("✓ 录音测试完成")
            
        except Exception as e:
            logger.error(f"✗ 录音设备测试失败: {e}")
        
        # 测试播放设备
        logger.info("\n=== 测试播放设备 ===")
        try:
            stream = p.open(
                format=pyaudio.paInt16,
                channels=1,
                rate=24000,
                output=True,
                frames_per_buffer=1024
            )
            logger.info("✓ 播放设备初始化成功")
            
            # 生成简单测试音频
            import numpy as np
            duration = 1  # 1秒
            sample_rate = 24000
            frequency = 440  # A4音符
            
            t = np.linspace(0, duration, int(sample_rate * duration), False)
            wave = np.sin(frequency * 2 * np.pi * t)
            audio_data = (wave * 32767).astype(np.int16).tobytes()
            
            logger.info("播放测试音频...")
            stream.write(audio_data)
            
            stream.stop_stream()
            stream.close()
            logger.info("✓ 播放测试完成")
            
        except Exception as e:
            logger.error(f"✗ 播放设备测试失败: {e}")
        
        p.terminate()
        logger.info("✓ 音频设备测试完成")
        
    except Exception as e:
        logger.error(f"✗ PyAudio初始化失败: {e}")
        logger.error("可能需要安装: brew install portaudio")

def check_permissions():
    """检查macOS音频权限"""
    import os
    logger.info("\n=== 权限检查 ===")
    logger.info("在macOS上，可能需要在以下位置授权麦克风权限:")
    logger.info("系统偏好设置 -> 安全性与隐私 -> 隐私 -> 麦克风")
    logger.info("确保Python或终端应用已被授权使用麦克风")

if __name__ == "__main__":
    check_permissions()
    test_audio_devices()