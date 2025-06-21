#!/usr/bin/env python3
"""
从custom.h文件中提取音频数据并转换为Python可用的格式
"""

import os
import re
import struct

def parse_audio_h(audio_name="light_on"):
    """解析音频.h文件，提取音频数据
    
    Args:
        audio_name: 音频文件名（不含.h后缀），可选: custom, light_on, light_off, welcome, byebye
    """
    file_path = os.path.join(os.path.dirname(__file__), 
                            f"../main/mock_voices/{audio_name}.h")
    
    with open(file_path, 'r') as f:
        content = f.read()
    
    # 提取数组长度
    len_match = re.search(rf'{audio_name}_len\s*=\s*(\d+)', content)
    if not len_match:
        raise ValueError(f"未找到{audio_name}_len定义")
    
    audio_len = int(len_match.group(1))
    print(f"{audio_name}音频长度: {audio_len} 字节")
    
    # 提取十六进制数据
    hex_pattern = r'0x([0-9a-fA-F]{2})'
    hex_values = re.findall(hex_pattern, content)
    
    # 转换为字节数组
    audio_bytes = bytearray()
    for hex_val in hex_values:
        audio_bytes.append(int(hex_val, 16))
    
    # 验证长度
    if len(audio_bytes) != audio_len:
        print(f"警告：提取的数据长度 {len(audio_bytes)} 与声明的长度 {audio_len} 不匹配")
    
    # 将字节数组转换为16位PCM样本
    samples = []
    for i in range(0, len(audio_bytes), 2):
        if i + 1 < len(audio_bytes):
            # 小端序：低字节在前，高字节在后
            sample = struct.unpack('<h', audio_bytes[i:i+2])[0]
            samples.append(sample)
    
    print(f"提取了 {len(samples)} 个PCM样本")
    
    # 返回原始字节数据（用于发送）
    return bytes(audio_bytes[:audio_len])

def get_custom_audio(audio_name="light_on"):
    """获取音频数据
    
    Args:
        audio_name: 音频文件名，默认为light_on
    """
    try:
        return parse_audio_h(audio_name)
    except Exception as e:
        print(f"解析{audio_name}.h失败: {e}")
        # 返回空音频作为备用
        return b'\x00' * 16000  # 1秒静音

if __name__ == "__main__":
    # 测试解析
    audio_name = "light_on"  # 可以改为: custom, light_off, welcome, byebye
    audio_data = get_custom_audio(audio_name)
    print(f"成功提取音频数据: {len(audio_data)} 字节")
    print(f"预计时长: {len(audio_data) / 2 / 16000:.2f} 秒")