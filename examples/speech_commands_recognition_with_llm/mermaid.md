# ESP32与WebSocket服务器交互时序图

## 核心交互流程

```mermaid
sequenceDiagram
    participant ESP32 as ESP32<br/>(main.cc)
    participant WS as WebSocket服务器<br/>(websocket_audio_server.py)
    participant Model as 大模型<br/>(OmniRealtimeClient)
    
    Note over ESP32: 初始状态：等待唤醒词
    
    %% 唤醒和连接阶段
    ESP32->>ESP32: 检测到唤醒词 "你好小智"
    ESP32->>WS: 建立WebSocket连接
    WS-->>ESP32: 连接确认
    ESP32->>WS: 发送唤醒事件<br/>{"event": "wake_word_detected"}
    ESP32->>ESP32: 播放欢迎音频
    
    Note over ESP32: 切换到录音状态
    
    %% 录音阶段
    ESP32->>ESP32: 开始录音（VAD检测）
    loop 录音过程
        ESP32->>ESP32: 采集音频数据<br/>(16kHz, 16bit, 单声道)
        ESP32->>ESP32: VAD检测语音活动
        alt 检测到语音
            ESP32->>ESP32: 继续录音
        else 检测到静音
            ESP32->>ESP32: 静音帧计数++
        end
    end
    
    Note over ESP32: VAD检测到持续静音<br/>录音结束
    
    %% 发送音频阶段
    ESP32->>WS: 发送二进制PCM音频数据<br/>(WebSocket Binary Frame)
    WS->>WS: 保存用户音频为MP3
    
    Note over ESP32: 切换到等待响应状态
    
    %% 大模型处理阶段
    alt 使用AI大模型
        WS->>Model: 连接大模型服务
        WS->>Model: 分块发送用户音频<br/>(Base64编码)
        WS->>Model: 触发响应生成<br/>create_response()
        Model-->>WS: 流式返回音频数据<br/>(24kHz)
        WS->>WS: 累积响应音频
        WS->>WS: 重采样 24kHz→16kHz
    else 使用默认音频
        WS->>WS: 加载默认音频<br/>(light_on.h)
    end
    
    %% 响应阶段
    WS->>WS: 保存响应音频为MP3
    WS->>ESP32: 发送二进制响应音频<br/>(16kHz PCM)
    WS->>ESP32: 发送ping包<br/>(音频结束标志)
    
    ESP32->>ESP32: 接收并累积音频数据
    ESP32->>ESP32: 检测到ping包
    ESP32->>ESP32: 播放响应音频
    
    Note over ESP32: 进入连续对话模式
    
    %% 连续对话循环
    ESP32->>ESP32: 重新开始录音<br/>(10秒超时)
    
    alt 用户继续说话
        ESP32->>ESP32: 重复录音流程
    else 检测到命令词
        alt "帮我开灯"
            ESP32->>ESP32: 执行开灯<br/>GPIO21=HIGH
            ESP32->>ESP32: 播放确认音频
        else "帮我关灯" 
            ESP32->>ESP32: 执行关灯<br/>GPIO21=LOW
            ESP32->>ESP32: 播放确认音频
        else "拜拜"
            ESP32->>ESP32: 播放再见音频
            ESP32->>WS: 断开WebSocket连接
            Note over ESP32: 返回等待唤醒状态
        end
    else 录音超时(10秒)
        ESP32->>ESP32: 播放再见音频
        ESP32->>WS: 断开WebSocket连接
        Note over ESP32: 返回等待唤醒状态
    end
```

## 关键数据流

### 1. 音频数据格式
- **ESP32发送**: 16kHz, 16bit, 单声道 PCM
- **服务器接收**: 二进制WebSocket帧
- **大模型输入**: Base64编码的音频块
- **大模型输出**: 24kHz PCM
- **服务器响应**: 重采样到16kHz后发送

### 2. 状态转换
```mermaid
stateDiagram-v2
    [*] --> 等待唤醒: 初始化完成
    等待唤醒 --> 录音中: 检测到唤醒词
    录音中 --> 等待响应: VAD检测到静音
    等待响应 --> 录音中: 播放响应完成<br/>(连续对话)
    录音中 --> 等待唤醒: 检测到"拜拜"
    录音中 --> 等待唤醒: 录音超时
```

### 3. WebSocket消息类型

#### JSON消息
```json
{
    "event": "wake_word_detected",
    "model": "模型名称",
    "timestamp": 时间戳
}
```

#### 二进制消息
- 音频数据：直接发送PCM二进制数据
- 结束标志：使用WebSocket ping帧

### 4. VAD参数
- 检测模式：中等灵敏度
- 帧长度：30ms
- 最小语音时长：200ms
- 最小静音时长：1000ms
- 静音帧阈值：20帧（约600ms）

### 5. 超时设置
- 命令词等待：5秒
- 连续对话录音：10秒
- WebSocket响应：15秒

## 错误处理流程

```mermaid
sequenceDiagram
    participant ESP32
    participant WS
    participant Model
    
    alt WiFi连接失败
        ESP32->>ESP32: 重试连接<br/>(最多5次)
        ESP32->>ESP32: 连接失败<br/>保持离线模式
    else WebSocket连接失败
        ESP32->>WS: 连接请求
        WS--xESP32: 连接失败
        ESP32->>ESP32: 返回等待唤醒状态
    else 大模型调用失败
        WS->>Model: 发送音频
        Model--xWS: 处理失败
        WS->>WS: 使用默认音频
        WS->>ESP32: 发送默认响应
    end
```