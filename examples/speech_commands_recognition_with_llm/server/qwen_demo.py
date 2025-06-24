# -- coding: utf-8 --
"""
按键触发录音，自动检测结束的版本
按一下 '1' 开始录音，LLM自动检测说话结束并回复
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
should_start_recording = False  # 新增：标记是否应该开始录音
current_status = "👂"  # 当前状态显示
is_processing = False  # 标记是否正在处理响应

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
    """按键按下事件 - 按一下 '1' 开始录音"""
    global should_start_recording, is_recording, is_processing
    try:
        if hasattr(key, 'char') and key.char == '1':
            with recording_lock:
                # 只有在不录音且不处理的状态下才能开始新录音
                if not is_recording and not is_processing:
                    should_start_recording = True
        # ESC键退出
        elif key == keyboard.Key.esc:
            return False
    except AttributeError:
        pass

def update_status_line(message):
    """更新状态行（覆盖同一行）"""
    print(f"\r{message}                    ", end="", flush=True)

async def handle_recording_trigger(client: OmniRealtimeClient):
    """处理录音触发的异步任务"""
    global is_recording, should_start_recording, current_status, is_processing
    
    while True:
        with recording_lock:
            if should_start_recording and not is_recording and not is_processing:
                should_start_recording = False
                is_recording = True
                current_status = "🎙️"
                update_status_line(f"{current_status} 正在录音，请说话...")
                
                # 清空音频缓冲区
                eventd = {
                    "event_id": "event_" + str(int(time.time() * 1000)),
                    "type": "input_audio_buffer.clear"
                }
                await client.send_event(eventd)
        
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
        print("- 按 '1' 键开始录音 🎙️")
        print("- 说话结束后自动识别并回复")
        print("- 回复完成后可再次按 '1' 开始新对话")
        print("- 按 'ESC' 键退出程序\n")
        update_status_line(f"{current_status} 就绪，按 '1' 开始对话")
        
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
                        print(f"\n录音错误: {e}")
                        update_status_line(f"{current_status} 就绪，按 '1' 开始对话")
            
            await asyncio.sleep(0.01)  # 10ms 间隔，更低延迟
    finally:
        stream.stop_stream()
        stream.close()
        p_local.terminate()

async def main():
    global is_recording, is_processing, current_status
    
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
        suppress=True  # 阻止按键传递到终端
    )
    listener.start()
    
    # 定义回调函数
    def on_text_output(text):
        """处理文本输出"""
        print(f"\n💬 AI: {text}", end="", flush=True)
    
    def on_input_done(text):
        """处理输入转录完成 - 表示VAD检测到说话结束"""
        global is_recording, is_processing, current_status
        if text.strip():  # 如果有有效输入
            print(f"\n🎤 识别: {text}")
            with recording_lock:
                is_recording = False
                is_processing = True
                current_status = "⏳"
            update_status_line(f"{current_status} 思考中...")
    
    def on_output_done(text):
        """处理输出完成"""
        global is_processing, current_status
        with recording_lock:
            is_processing = False
            current_status = "👂"
        print("\n✅ 回复完成")
        update_status_line(f"{current_status} 就绪，按 '1' 开始新对话")
    
    # 添加额外的事件处理器
    extra_handlers = {
        "input_audio_buffer.speech_started": lambda data: print("\n🔊 检测到说话..."),
        "input_audio_buffer.speech_stopped": lambda data: update_status_line("🤔 分析中..."),
        "response.done": lambda data: on_output_done("")
    }
    
    # 配置实时客户端
    realtime_client = OmniRealtimeClient(
        base_url="wss://dashscope.aliyuncs.com/api-ws/v1/realtime",
        api_key=api_key,
        model="qwen-omni-turbo-realtime-2025-05-08",
        voice="Chelsie",
        on_text_delta=on_text_output,
        on_audio_delta=handle_audio_data,
        on_input_transcript=on_input_done,
        on_output_transcript=lambda text: print(f"\n[转录] {text}") if text else None,
        extra_event_handlers=extra_handlers,
        turn_detection_mode=TurnDetectionMode.SERVER_VAD  # 使用服务器VAD自动检测
    )
    
    try:
        # 连接到服务器
        print("正在连接到Qwen实时服务...")
        await realtime_client.connect()
        print("✅ 连接成功！")
        
        # 配置VAD参数
        vad_config = {
            "event_id": "event_" + str(int(time.time() * 1000)),
            "type": "session.update",
            "session": {
                "turn_detection": {
                    "type": "server_vad",
                    "threshold": 0.5,  # VAD阈值
                    "prefix_padding_ms": 300,  # 前缀填充
                    "silence_duration_ms": 700  # 静音持续时间，更短的静音检测
                }
            }
        }
        await realtime_client.send_event(vad_config)
        
        # 创建并发任务
        tasks = [
            asyncio.create_task(realtime_client.handle_messages()),
            asyncio.create_task(microphone_streaming(realtime_client)),
            asyncio.create_task(handle_recording_trigger(realtime_client))
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