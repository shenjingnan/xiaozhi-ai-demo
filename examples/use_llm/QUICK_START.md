# 快速开始指南

## 问题解决：NVS初始化失败

如果您遇到了 `ESP_ERR_NOT_FOUND (0x105)` 错误，这是因为NVS分区的问题。

### 什么是NVS？

**NVS (Non-Volatile Storage)** 是ESP32的非易失性存储系统，用于存储：
- WiFi配置信息
- 系统参数
- 用户设置数据

### 解决方案

我已经修复了分区表配置，现在包含了正确的NVS分区。请按以下步骤操作：

#### 1. 重新编译固件
```bash
cd examples/use_llm
source ~/esp/esp-idf/export.sh
idf.py build
```

#### 2. 完全擦除Flash（推荐）
```bash
idf.py erase-flash
```

#### 3. 重新烧录固件
```bash
idf.py flash
```

#### 4. 监控串口输出
```bash
idf.py monitor
```

## 配置步骤

### 1. 修改WiFi配置

编辑 `main/main.cc` 文件，找到以下行并修改：

```cpp
// WiFi配置
#define WIFI_SSID "your_wifi_ssid"        // 改为您的WiFi名称
#define WIFI_PASS "your_wifi_password"    // 改为您的WiFi密码

// 服务器配置
#define SERVER_URL "http://192.168.1.100:8080/process_audio"  // 改为服务端IP
```

### 2. 启动Python服务端

```bash
cd server
pip install -r requirements.txt

# 配置千问API密钥
echo "DASHSCOPE_API_KEY=your_api_key_here" > .env

# 启动服务端
python server.py
```

### 3. 重新编译和烧录

```bash
idf.py build
idf.py flash
idf.py monitor
```

## 预期的启动日志

正常启动时，您应该看到类似以下的日志：

```
I (888) 语音识别: 正在初始化NVS...
I (895) 语音识别: ✓ NVS初始化成功
I (900) 语音识别: 正在初始化WiFi连接...
I (1200) 语音识别: ✓ WiFi连接成功
I (1205) 语音识别: ✓ 音频录制缓冲区初始化成功
I (1210) 语音识别: ✓ 外接LED初始化成功，初始状态：关闭
I (1220) 语音识别: ✓ INMP441麦克风初始化成功
I (1230) 语音识别: ✓ 音频播放初始化成功
...
I (2000) 语音识别: 系统启动完成，等待唤醒词 '你好小智'...
```

## 使用方法

1. 对着麦克风说："你好小智"
2. 听到欢迎音频后，说出您的指令
3. 支持的本地命令：
   - "帮我开灯"
   - "帮我关灯" 
   - "拜拜"
4. 其他语音会发送到云端AI处理

## 常见问题

### Q: 仍然出现NVS错误怎么办？
A: 执行 `idf.py erase-flash` 完全擦除Flash，然后重新烧录

### Q: WiFi连接失败？
A: 检查WiFi SSID和密码是否正确，确保设备在WiFi覆盖范围内

### Q: 无法连接服务端？
A: 检查服务端IP地址配置，确保服务端已启动并且防火墙允许8080端口

### Q: 语音识别不准确？
A: 确保环境安静，麦克风连接正确，说话清晰

更多详细信息请参考 `USAGE_GUIDE.md` 文件。
