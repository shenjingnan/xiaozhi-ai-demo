# 复刻小智

小智 AI 源码地址: https://github.com/78/xiaozhi-esp32

## AI 提示词

### 通过小智AI源码获取交互时序图

本项目是小智AI的源码，我希望你能分析代码，告诉我核心的交互逻辑
并通过使用mermaid生成交互时序图
时序图应该包含的对象如下：
用户、麦克风、功放、ESP32、服务端、LLM

## 简化版交互时序图

```mermaid
sequenceDiagram
    participant 用户
    participant 麦克风
    participant 功放
    participant ESP32
    participant 服务端
    participant LLM

    rect rgb(255, 240, 245)
    note right of 用户: 用户输入阶段
    ESP32->>ESP32: 监听用户语音
    用户->>麦克风: 说出"今天天气怎么样？"
    麦克风->>ESP32: 音频数据
    ESP32->>ESP32: Opus编码
    ESP32->>服务端: 发送编码后的音频数据
    end

    rect rgb(255, 250, 240)
    note right of 用户: AI回答阶段
    服务端->>LLM: 发送用户输入的音频
    LLM->>服务端: 返回LLM回答音频
    服务端->>ESP32: 发送合成的音频数据(Opus编码)
    ESP32->>功放: 发送解码后的音频数据
    功放->>用户: 播放声音"今天天气晴朗..."
    end
```

## 详细的小智 AI 交互时序图

```mermaid
sequenceDiagram
    participant 用户
    participant 麦克风
    participant 功放
    participant ESP32
    participant 服务端
    participant LLM

    rect rgb(230, 230, 250)
        Note over ESP32: 设备启动初始化
        ESP32->>ESP32: 初始化硬件
        ESP32->>ESP32: 连接网络
        ESP32->>ESP32: 注册物联网设备
    end

    rect rgb(240, 248, 255)
        Note over 用户,ESP32: 唤醒阶段
        ESP32->>ESP32: 进入唤醒词监听模式
        用户->>麦克风: 说出唤醒词"你好小智"
        麦克风->>ESP32: 采集音频
        ESP32->>ESP32: 本地唤醒词检测
        alt 检测到唤醒词
            ESP32->>ESP32: 触发唤醒状态
            ESP32->>功放: 播放唤醒提示音
            ESP32->>服务端: 建立WebSocket连接
            ESP32->>服务端: 发送hello消息
            服务端->>ESP32: 返回hello确认
        else 未检测到唤醒词
            ESP32->>ESP32: 继续监听唤醒词
        end
    end

    rect rgb(255, 240, 245)
        Note over 用户,LLM: 对话阶段
        alt 按键唤醒
            用户->>ESP32: 按下BOOT按钮
            ESP32->>ESP32: 触发唤醒状态
            ESP32->>服务端: 建立WebSocket连接
        end

        ESP32->>服务端: 发送listen开始指令
        用户->>麦克风: 说话(例如"今天天气怎么样？")
        麦克风->>ESP32: 采集音频
        ESP32->>ESP32: 音频编码(Opus)
        ESP32->>服务端: 发送编码后的音频数据流
        服务端->>服务端: 语音识别(STT)
        服务端->>ESP32: 发送识别文本
        服务端->>LLM: 发送用户文本
        LLM->>服务端: 返回AI回复
        服务端->>服务端: 语音合成(TTS)
        服务端->>ESP32: 发送TTS开始指令
        服务端->>ESP32: 发送音频数据流
        ESP32->>功放: 播放音频("今天天气晴朗...")
        服务端->>ESP32: 发送TTS结束指令
    end

    rect rgb(255, 250, 240)
        Note over 用户,ESP32: 物联网控制
        用户->>麦克风: 发出控制指令(如"打开客厅灯")
        麦克风->>ESP32: 采集音频
        ESP32->>服务端: 发送音频数据
        服务端->>服务端: 语音识别
        服务端->>LLM: 发送控制意图
        LLM->>服务端: 解析控制命令
        服务端->>ESP32: 发送IoT控制指令
        ESP32->>ESP32: ThingManager执行命令
        ESP32->>服务端: 返回执行结果
        服务端->>LLM: 更新设备状态
        LLM->>服务端: 生成反馈信息
        服务端->>ESP32: 发送TTS音频
        ESP32->>功放: 播放反馈音频
    end

    rect rgb(240, 255, 240)
        Note over ESP32,用户: 会话结束
        ESP32->>ESP32: 等待超时或用户指令
        alt 超时无交互
            ESP32->>ESP32: 回到唤醒词监听模式
            ESP32->>服务端: 关闭WebSocket连接
        else 用户主动结束
            用户->>麦克风: 说"再见"或类似结束语
            ESP32->>服务端: 关闭WebSocket连接
            ESP32->>ESP32: 回到唤醒词监听模式
        end
    end
```
