# 双向音频通信状态说明

## 当前问题

ESP32与Python脚本之间的双向音频通信存在以下问题：

### 1. 串口通信限制
- 使用 `idf.py monitor` 时，数据流向是单向的：ESP32 → Python
- Python脚本无法通过标准管道将数据发送回ESP32
- ESP32的 `getchar()` 只能读取来自调试器的输入，不能读取Python的输出

### 2. 当前状态
- ✅ ESP32 → Python：录音数据传输正常
- ❌ Python → ESP32：响应音频无法传输

## 临时解决方案

目前在 `main.cc` 中直接播放预设的 `light_on` 音频作为响应：

```cpp
// 直接播放预设的响应音频（暂时绕过Python响应）
ESP_LOGI(TAG, "播放预设响应音频...");
esp_err_t audio_ret = bsp_play_audio(light_on, light_on_len);
```

## 完整解决方案（未来实现）

要实现真正的双向通信，需要以下几种方案之一：

### 方案1：使用ESP32的UART接口
- 配置专门的UART用于与Python通信
- Python通过pyserial直接读写UART
- 需要修改硬件连接

### 方案2：使用网络通信
- ESP32作为HTTP客户端或WebSocket客户端
- Python运行HTTP/WebSocket服务器
- 通过WiFi传输音频数据

### 方案3：使用USB CDC
- 配置ESP32的USB为CDC设备
- 实现双向串口通信
- 需要ESP32-S3的USB OTG功能

### 方案4：使用共享存储
- 通过SD卡或其他存储介质
- ESP32和Python分别读写文件
- 延迟较高但实现简单

## 当前功能

虽然双向通信尚未完全实现，但系统仍可以：
1. 检测唤醒词"你好小智"
2. 录制用户语音
3. 将录音发送到Python脚本保存
4. 播放预设的响应音频
5. 继续等待语音命令

## 测试方法

```bash
# 编译并烧录
cd examples/speech_commands_recognition_with_llm
idf.py build flash

# 运行（录音会被保存，但响应音频是预设的）
idf.py monitor | python3 server/audio_receiver_pipe.py
```