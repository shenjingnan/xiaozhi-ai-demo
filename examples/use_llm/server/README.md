# 语音助手WebSocket服务器

这是一个WebSocket服务器，用于连接ESP32设备和Qwen Omni Turbo Realtime LLM模型，实现实时语音对话功能。

## 功能特性

- **WebSocket服务器**: 接收ESP32和Python客户端的连接
- **音频转发**: 将客户端音频数据转发给LLM模型
- **实时对话**: 支持语音输入和音频输出
- **多客户端支持**: 可同时处理多个客户端连接
- **手动VAD模式**: 由客户端控制录音开始和结束

## 安装依赖

```bash
pip install -r requirements.txt
```

## 环境配置

设置API密钥环境变量:

```bash
export DASHSCOPE_API_KEY=your_api_key_here
```

## 启动服务器

```bash
python server.py
```

服务器将在 `ws://0.0.0.0:8888` 启动。

## 测试客户端

运行Python测试客户端:

```bash
python client.py
```

### 客户端使用方法

1. 启动客户端后输入 `start` 开始对话
2. 对着麦克风说话
3. 按回车键停止录音并获取AI回复
4. 输入 `quit` 退出程序

## 通信协议

### 客户端到服务器的消息格式

#### 握手消息
```json
{
    "type": "hello",
    "client_type": "esp32" | "python"
}
```

#### 开始录音
```json
{
    "type": "start_listening"
}
```

#### 音频数据块
```json
{
    "type": "audio_chunk",
    "data": "base64_encoded_pcm_audio",
    "format": "pcm16",
    "sample_rate": 16000
}
```

#### 停止录音
```json
{
    "type": "stop_listening"
}
```

#### 心跳包
```json
{
    "type": "ping"
}
```

### 服务器到客户端的消息格式

#### 握手确认
```json
{
    "type": "hello_ack",
    "status": "ready"
}
```

#### 音频回复
```json
{
    "type": "audio",
    "data": "base64_encoded_pcm_audio",
    "format": "pcm16",
    "sample_rate": 24000
}
```

#### 文本回复
```json
{
    "type": "text",
    "data": "AI回复文本"
}
```

#### 语音识别结果
```json
{
    "type": "input_transcript",
    "data": "用户说的话"
}
```

#### 错误消息
```json
{
    "type": "error",
    "message": "错误描述"
}
```

#### 心跳回复
```json
{
    "type": "pong"
}
```

## 音频格式

- **输入音频**: 16kHz, 16位, 单声道PCM
- **输出音频**: 24kHz, 16位, 单声道PCM
- **编码格式**: Base64编码的原始PCM数据

## 注意事项

1. 确保已设置 `DASHSCOPE_API_KEY` 环境变量
2. 客户端音频格式必须为16kHz PCM格式
3. 服务器使用手动VAD模式，需要客户端主动控制录音开始和结束
4. 每个客户端会独立创建LLM连接，支持并发对话