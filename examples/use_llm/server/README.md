# 千问大模型集成服务器

这是一个基于千问大模型的语音处理服务器，提供完整的语音对话功能。

## 功能特性

1. **语音识别 (STT)**: 将PCM格式音频转换为文字
2. **智能推理**: 使用千问大模型生成智能回复（限制20字内）
3. **语音合成 (TTS)**: 将文字转换为语音
4. **HTTP API**: 提供简单的REST API接口

## 安装依赖

```bash
pip install -r requirements.txt
```

## 配置

确保 `.env` 文件中包含你的千问API密钥：

```
DASHSCOPE_API_KEY=your-api-key-here
```

## 启动服务器

```bash
python server.py
```

服务器将在 `http://localhost:8080` 启动。

## API 使用

### 处理音频 (主要功能)

**端点**: `POST /process_audio`

**请求**: 
- 方法: POST
- 内容类型: multipart/form-data
- 参数: `audio` (PCM格式音频文件)

**响应**: 
- 成功: 返回MP3格式的音频文件
- 失败: 返回JSON错误信息

### 健康检查

**端点**: `GET /health`

**响应**: 
```json
{
  "status": "ok",
  "message": "服务运行正常"
}
```

## 测试

使用提供的测试客户端：

```bash
python test_client.py
```

## 工作流程

1. 客户端发送PCM格式音频数据
2. 服务器将PCM转换为WAV格式
3. 调用千问STT进行语音识别
4. 调用千问文本推理模型生成回复
5. 调用千问TTS将回复转换为语音
6. 返回MP3格式音频给客户端

## 注意事项

- 音频格式要求: PCM, 16kHz, 单声道, 16位
- 回复文本限制在20个字以内
- 支持中英文语音识别
- 默认使用女声音色 (longxiaochun_v2)

## 文件说明

- `server.py`: 主服务器文件
- `test_client.py`: 测试客户端
- `requirements.txt`: Python依赖包
- `.env`: API密钥配置文件
- `README.md`: 使用说明文档
