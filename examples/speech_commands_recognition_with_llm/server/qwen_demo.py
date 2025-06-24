# -- coding: utf-8 --
"""
使用 pynput 库的版本，跨平台支持更好
安装：pip install pynput
"""
import os, time, base64, asyncio
from omni_realtime_client import OmniRealtimeClient, TurnDetectionMode
import pyaudio
import queue
import threading
from pynput import keyboard
import sys

# 创建一个全局音频队列和播放线程
audio_queue = queue.Queue()
audio_player = None

# 初始化PyAudio
p = pyaudio.PyAudio()
RATE = 16000  # 采样率 16kHz
CHUNK = 3200  # 每个音频块的大小
FORMAT = pyaudio.paInt16  # 16位PCM格式
CHANNELS = 1  # 单声道

# 录音控制标志
is_recording = False
recording_lock = threading.Lock()
key_pressed = False
current_status = "👂"  # 当前状态显示

def audio_player_thread():
    """后台线程用于播放音频数据"""
    stream = p.open(format=FORMAT,
                    channels=CHANNELS,
                    rate=24000,
                    output=True,
                    frames_per_buffer=CHUNK)
    
    try:
        while True:
            try:
                # 从队列获取音频数据
                audio_data = audio_queue.get(block=True, timeout=0.5)
                if audio_data is None:  # 结束信号
                    break
                # 播放音频数据
                stream.write(audio_data)
                audio_queue.task_done()
            except queue.Empty:
                # 如果队列为空，继续等待
                continue
    finally:
        # 清理
        stream.stop_stream()
        stream.close()

def start_audio_player():
    """启动音频播放线程"""
    global audio_player
    if audio_player is None or not audio_player.is_alive():
        audio_player = threading.Thread(target=audio_player_thread, daemon=True)
        audio_player.start()

def handle_audio_data(audio_data):
    """处理接收到的音频数据"""
    # 将音频数据放入队列
    audio_queue.put(audio_data)

def on_press(key):
    """按键按下事件"""
    global key_pressed
    try:
        if hasattr(key, 'char') and key.char == '1':
            key_pressed = True
            # 不返回False，让监听器继续运行
    except AttributeError:
        pass

def on_release(key):
    """按键释放事件"""
    global key_pressed
    try:
        if hasattr(key, 'char') and key.char == '1':
            key_pressed = False
        # ESC键退出
        if key == keyboard.Key.esc:
            return False
    except AttributeError:
        pass

async def handle_key_events(client: OmniRealtimeClient):
    """处理按键事件的异步任务"""
    global is_recording, key_pressed, current_status
    
    while True:
        with recording_lock:
            if key_pressed and not is_recording:
                is_recording = True
                current_status = "🎙️"
                print(f"\r{current_status} 正在录音...", end="", flush=True)
                # 清空音频缓冲区
                eventd = {
                    "event_id": "event_" + str(int(time.time() * 1000)),
                    "type": "input_audio_buffer.clear"
                }
                await client.send_event(eventd)
            elif not key_pressed and is_recording:
                is_recording = False
                current_status = "👂"
                print(f"\r{current_status} 等待回复...", end="", flush=True)
                # 发送结束录音事件，触发LLM响应
                eventd = {
                    "event_id": "event_" + str(int(time.time() * 1000)),
                    "type": "input_audio_buffer.commit"
                }
                await client.send_event(eventd)
                
                # 创建响应
                response_event = {
                    "event_id": "event_" + str(int(time.time() * 1000)),
                    "type": "response.create",
                    "response": {
                        "modalities": ["text", "audio"],
                        "instructions": "请回答用户的问题"
                    }
                }
                await client.send_event(response_event)
        
        await asyncio.sleep(0.05)  # 50ms 检查一次

async def microphone_streaming(client: OmniRealtimeClient):
    """麦克风录音流式传输"""
    CHUNK = 3200
    FORMAT = pyaudio.paInt16
    CHANNELS = 1
    RATE = 16000
    
    p_local = pyaudio.PyAudio()
    stream = p_local.open(format=FORMAT,
                    channels=CHANNELS,
                    rate=RATE,
                    input=True,
                    frames_per_buffer=CHUNK)
    
    try:
        print("\n📌 使用说明：")
        print("- 按住 '1' 键开始录音 🎙️")
        print("- 释放 '1' 键停止录音并获取AI回复 👂")
        print("- 按 'ESC' 键或 'Ctrl+C' 退出程序\n")
        print(f"\r{current_status} 就绪", end="", flush=True)
        
        while True:
            with recording_lock:
                if is_recording:
                    try:
                        # 读取音频数据
                        audio_data = stream.read(CHUNK, exception_on_overflow=False)
                        encoded_data = base64.b64encode(audio_data).decode('utf-8')
                        
                        # 发送音频数据到服务器
                        eventd = {
                            "event_id": "event_" + str(int(time.time() * 1000)),
                            "type": "input_audio_buffer.append",
                            "audio": encoded_data
                        }
                        await client.send_event(eventd)
                    except Exception as e:
                        print(f"录音错误: {e}")
            
            await asyncio.sleep(0.05)  # 50ms 间隔
    finally:
        stream.stop_stream()
        stream.close()
        p_local.terminate()

async def main():
    # 检查环境变量
    api_key = os.environ.get("DASHSCOPE_API_KEY")
    if not api_key:
        print("❌ 错误：请设置环境变量 DASHSCOPE_API_KEY")
        print("export DASHSCOPE_API_KEY='your-api-key'")
        return
    
    # 启动音频播放线程
    start_audio_player()
    
    # 启动键盘监听器，使用suppress=True来阻止按键传递到终端
    listener = keyboard.Listener(
        on_press=on_press,
        on_release=on_release,
        suppress=True  # 阻止按键传递到终端
    )
    listener.start()
    
    # 配置实时客户端
    realtime_client = OmniRealtimeClient(
        base_url="wss://dashscope.aliyuncs.com/api-ws/v1/realtime",
        api_key=api_key,
        model="qwen-omni-turbo-realtime-2025-05-08",
        voice="Chelsie",
        on_text_delta=lambda text: print(f"\nAI: {text}", end="", flush=True),
        on_audio_delta=handle_audio_data,
        on_input_transcript=lambda text: print(f"\n🎤 输入: {text}"),
        on_output_transcript=lambda text: (print(f"\n✅ 完成"), print(f"\r{current_status} 就绪", end="", flush=True)),  # 输出完整转录
        turn_detection_mode=TurnDetectionMode.MANUAL  # 手动控制模式，使用按键控制
    )
    
    try:
        # 连接到服务器
        print("正在连接到Qwen实时服务...")
        await realtime_client.connect()
        print("✅ 连接成功！")
        
        # 创建并发任务
        tasks = [
            asyncio.create_task(realtime_client.handle_messages()),
            asyncio.create_task(microphone_streaming(realtime_client)),
            asyncio.create_task(handle_key_events(realtime_client))
        ]
        
        # 等待任务完成（直到用户中断）
        await asyncio.gather(*tasks)
        
    except KeyboardInterrupt:
        print("\n\n👋 正在退出程序...")
    except Exception as e:
        print(f"\n❌ 错误: {e}")
    finally:
        # 停止键盘监听
        listener.stop()
        # 清理资源
        audio_queue.put(None)  # 发送结束信号给音频播放线程
        if audio_player:
            audio_player.join(timeout=1)
        await realtime_client.close()
        p.terminate()
        print("✅ 程序已退出")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n程序已退出")