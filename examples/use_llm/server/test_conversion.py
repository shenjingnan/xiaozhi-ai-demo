#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
测试MP3到PCM转换功能
"""

import subprocess
import sys

def check_ffmpeg():
    """检查ffmpeg是否可用"""
    try:
        result = subprocess.run(['ffmpeg', '-version'], 
                              stdout=subprocess.DEVNULL, 
                              stderr=subprocess.DEVNULL)
        return result.returncode == 0
    except FileNotFoundError:
        return False

def main():
    print("检查ffmpeg是否可用...")
    
    if check_ffmpeg():
        print("✅ ffmpeg 可用")
    else:
        print("❌ ffmpeg 不可用")
        print("请安装ffmpeg:")
        print("  macOS: brew install ffmpeg")
        print("  Ubuntu: sudo apt install ffmpeg")
        print("  Windows: 下载并安装ffmpeg")
        sys.exit(1)
    
    print("✅ 服务端依赖检查通过")

if __name__ == "__main__":
    main()
