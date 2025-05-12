# INMP441麦克风与ESP32-S3音频采集上传项目 (C++版本)

本项目使用C++实现了INMP441麦克风采集音频，通过ESP32-S3-DevKitC-1开发板处理并上传到xyz.ai API的功能。

## 硬件连接

INMP441麦克风与ESP32-S3的连接方式如下：

| INMP441引脚 | ESP32-S3引脚 | 说明 |
|------------|-------------|------|
| VDD        | 3.3V        | 电源 |
| GND        | GND         | 地   |
| SD         | GPIO13      | 数据 |
| SCK        | GPIO14      | 位时钟 |
| WS         | GPIO15      | 左/右时钟 |
| L/R        | GND         | 左/右声道选择，接地为左声道 |

## 软件配置

在使用前，需要修改`main.cpp`文件中的以下配置：

```cpp
namespace Config {
    // WiFi配置
    const char* WIFI_SSID = "您的WiFi名称";
    const char* WIFI_PASS = "您的WiFi密码";
    
    // API配置
    const char* API_ENDPOINT = "https://xyz.ai/api/audio";
    const char* API_KEY = "您的API密钥";
    
    // 其他配置...
}
```

## 功能说明

本项目使用C++面向对象编程方式实现，主要包含以下类：

1. **AudioQueue**: 线程安全的音频数据队列，用于任务间通信
2. **I2SMicrophone**: 封装I2S接口与INMP441麦克风的通信
3. **OpusEncoder**: 封装Opus编码器的操作
4. **WiFiManager**: 管理WiFi连接
5. **HttpClient**: 处理HTTP请求
6. **AudioCaptureTask**: 音频采集任务
7. **AudioUploadTask**: 音频上传任务
8. **AudioUploadApp**: 主应用类，协调各组件工作

## C++版本的优势

相比C语言版本，C++版本有以下优势：

1. **更好的代码组织**: 使用类和对象封装功能，代码结构更清晰
2. **自动内存管理**: 使用`std::vector`等容器，避免手动内存管理
3. **更安全的类型系统**: 减少类型错误和内存泄漏
4. **更简洁的代码**: 减少重复代码，提高可读性
5. **更好的错误处理**: 使用异常和返回值组合处理错误

## 工作流程

1. 初始化系统组件（NVS、WiFi）
2. 创建并启动音频采集任务和音频上传任务
3. 音频采集任务从INMP441麦克风读取数据，转换为PCM格式，并发送到队列
4. 音频上传任务从队列接收PCM数据，使用Opus编码器编码，并上传到xyz.ai API

## 编译和烧录

使用PlatformIO编译和烧录：

```bash
# 编译
pio run

# 烧录
pio run -t upload

# 监视串口输出
pio run -t monitor
```

## 注意事项

1. 本项目使用C++17标准，确保您的PlatformIO配置正确
2. INMP441麦克风输出的是32位有符号整数，左对齐，需要右移16位转换为16位PCM数据
3. 默认采样率为16kHz，单声道
4. 每次上传约1秒的音频数据（50帧，每帧20ms）
5. 确保WiFi连接稳定，否则上传可能失败

## 调试

可以通过串口监视器查看程序运行状态和调试信息：

```bash
pio run -t monitor
```

## 扩展功能

可以根据需要扩展以下功能：

1. 添加按钮控制录音开始和结束
2. 添加LED指示录音和上传状态
3. 添加本地存储功能，在网络不可用时保存音频数据
4. 添加音频前处理功能，如降噪、回声消除等
5. 添加更完善的错误处理和重试机制
