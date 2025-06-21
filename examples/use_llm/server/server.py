#!/usr/bin/env python3
"""
ESP32-S3 串口通信服务端
用于接收开发板发送的消息，特别是唤醒词检测成功的通知
Python 3.9 兼容
"""

import serial
import serial.tools.list_ports
import json
import time
from datetime import datetime
import sys


def find_esp32_port():
    """自动查找ESP32-S3的串口"""
    ports = serial.tools.list_ports.comports()
    
    for port in ports:
        # 打印所有可用端口信息
        print(f"发现端口: {port.device} - {port.description}")
        
        # ESP32-S3通常包含这些关键词
        if any(keyword in port.description.lower() for keyword in ['esp32', 'silicon labs', 'cp210x', 'uart']):
            print(f"✓ 找到ESP32设备: {port.device}")
            return port.device
    
    # 如果没有自动找到，让用户选择
    if ports:
        print("\n未自动识别到ESP32设备，请手动选择：")
        for i, port in enumerate(ports):
            print(f"{i}: {port.device} - {port.description}")
        
        try:
            choice = int(input("请输入端口号 (0-{}): ".format(len(ports)-1)))
            if 0 <= choice < len(ports):
                return ports[choice].device
        except:
            pass
    
    return None


def main():
    """主函数"""
    print("ESP32-S3 串口消息接收服务")
    print("=" * 50)
    
    # 查找串口
    port = find_esp32_port()
    if not port:
        print("错误：未找到ESP32设备！")
        print("请确保：")
        print("1. ESP32-S3已通过USB连接到电脑")
        print("2. 已安装USB驱动程序")
        sys.exit(1)
    
    # 串口配置
    baudrate = 115200  # ESP-IDF默认波特率
    
    try:
        # 打开串口
        ser = serial.Serial(
            port=port,
            baudrate=baudrate,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.1,  # 100ms超时
            xonxoff=False,
            rtscts=False,
            dsrdtr=False
        )
        
        print(f"\n✓ 成功连接到 {port}，波特率: {baudrate}")
        print("等待接收消息...\n")
        
        # 清空缓冲区
        ser.reset_input_buffer()
        
        # 消息缓冲区
        message_buffer = ""
        
        while True:
            try:
                # 读取可用数据
                if ser.in_waiting > 0:
                    # 读取数据
                    data = ser.read(ser.in_waiting)
                    
                    try:
                        # 尝试解码为UTF-8
                        text = data.decode('utf-8', errors='ignore')
                        message_buffer += text
                        
                        # 查找完整的行
                        while '\n' in message_buffer:
                            line, message_buffer = message_buffer.split('\n', 1)
                            line = line.strip()
                            
                            if line:
                                # 添加时间戳
                                timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                                
                                # 检查是否是唤醒词检测消息
                                if "唤醒词检测成功" in line:
                                    print(f"\n[{timestamp}] 🎉 {line}")
                                    print(">>> 系统已被唤醒，等待后续指令...\n")
                                    
                                    # 这里可以添加额外的处理逻辑
                                    # 例如：播放提示音、启动语音识别等
                                
                                # 检查是否是开始录音消息
                                elif "开始录音" in line:
                                    print(f"\n[{timestamp}] 🎙️ {line}")
                                    print(">>> 正在监听用户语音输入...\n")
                                
                                # 检查是否是结束录音消息
                                elif "结束录音" in line:
                                    print(f"\n[{timestamp}] 🛑 {line}")
                                    print(">>> 用户语音输入完成\n")
                                    
                                # 检查是否是JSON格式的消息
                                elif line.startswith('{') and line.endswith('}'):
                                    try:
                                        msg = json.loads(line)
                                        print(f"[{timestamp}] JSON消息: {json.dumps(msg, ensure_ascii=False, indent=2)}")
                                    except json.JSONDecodeError:
                                        print(f"[{timestamp}] {line}")
                                else:
                                    # 普通日志消息
                                    print(f"[{timestamp}] {line}")
                    
                    except UnicodeDecodeError:
                        # 如果不是文本数据，显示为十六进制
                        hex_data = ' '.join(f'{b:02x}' for b in data)
                        print(f"[二进制数据] {hex_data}")
                
                # 短暂休眠避免CPU占用过高
                time.sleep(0.01)
                
            except KeyboardInterrupt:
                print("\n\n正在关闭串口连接...")
                break
            except Exception as e:
                print(f"读取错误: {e}")
                time.sleep(1)
        
    except serial.SerialException as e:
        print(f"串口错误: {e}")
        print("请检查：")
        print("1. 串口是否被其他程序占用（如Arduino IDE的串口监视器）")
        print("2. ESP32是否正确连接")
    except Exception as e:
        print(f"未知错误: {e}")
    finally:
        if 'ser' in locals() and ser.is_open:
            ser.close()
            print("串口已关闭")


if __name__ == "__main__":
    main()