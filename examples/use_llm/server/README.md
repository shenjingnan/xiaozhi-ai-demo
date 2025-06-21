# ESP32-S3 串口通信服务端

用于接收ESP32-S3开发板发送的串口消息，特别是唤醒词检测成功的通知。

## 安装依赖

```bash
pip install -r requirements.txt
```

## 使用方法

1. 先编译并烧录ESP32-S3程序：
```bash
cd ..
idf.py build flash
```

2. 运行Python服务端：
```bash
python server.py
```

3. 脚本会自动检测ESP32-S3的串口，也可以手动选择。

## 消息格式

当ESP32-S3检测到唤醒词时，会发送以下消息：

1. 文本消息：`唤醒词检测成功`
2. JSON消息：包含事件类型、模型名称和时间戳
   ```json
   {
     "event": "wake_word_detected",
     "model": "模型名称",
     "timestamp": 1234567890
   }
   ```

## 注意事项

- 确保ESP32-S3通过USB连接到电脑
- 关闭其他占用串口的程序（如Arduino IDE的串口监视器）
- 默认波特率为115200