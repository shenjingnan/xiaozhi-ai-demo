#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
千问大模型集成服务器
提供HTTP API接口，实现：PCM音频 -> STT -> 文本推理 -> TTS -> 音频返回
"""

import os
import io
import wave
import time
import requests
import logging
import subprocess
import tempfile
from flask import Flask, request, jsonify, send_file
from dotenv import load_dotenv
import dashscope
from dashscope.audio.asr import Recognition, RecognitionCallback, RecognitionResult
from dashscope.audio.tts_v2 import SpeechSynthesizer

# 加载环境变量
load_dotenv()

# 配置日志
logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    handlers=[
        logging.StreamHandler(),
        logging.FileHandler("debug.log", encoding="utf-8"),
    ],
)
logger = logging.getLogger(__name__)

app = Flask(__name__)

# 配置千问API
dashscope.api_key = os.getenv("DASHSCOPE_API_KEY")

# 千问API配置
QWEN_API_URL = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
QWEN_API_KEY = os.getenv("DASHSCOPE_API_KEY")


class STTCallback(RecognitionCallback):
    """语音识别回调类"""

    def __init__(self):
        self.result_text = ""
        self.is_complete = False
        self.error_message = None

    def on_complete(self) -> None:
        print("[STT] 语音识别完成")
        self.is_complete = True

    def on_error(self, result: RecognitionResult) -> None:
        print(f"[STT] 识别错误: {result.message}")
        self.error_message = result.message
        self.is_complete = True

    def on_event(self, result: RecognitionResult) -> None:
        sentence = result.get_sentence()
        if "text" in sentence:
            self.result_text = sentence["text"]
            print(f"[STT] 识别文本: {self.result_text}")


def pcm_to_wav(pcm_data, sample_rate=16000, channels=1, sample_width=2):
    """将PCM数据转换为WAV格式"""
    wav_buffer = io.BytesIO()
    with wave.open(wav_buffer, "wb") as wav_file:
        wav_file.setnchannels(channels)
        wav_file.setsampwidth(sample_width)
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(pcm_data)
    wav_buffer.seek(0)
    return wav_buffer.getvalue()


def convert_mp3_to_pcm(mp3_data):
    """将MP3音频数据转换为16kHz单声道16位PCM格式"""
    try:
        # 创建临时文件
        with tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as mp3_temp:
            mp3_temp.write(mp3_data)
            mp3_temp_path = mp3_temp.name

        with tempfile.NamedTemporaryFile(suffix=".pcm", delete=False) as pcm_temp:
            pcm_temp_path = pcm_temp.name

        try:
            # 使用ffmpeg转换MP3到PCM
            cmd = [
                "ffmpeg",
                "-i",
                mp3_temp_path,  # 输入MP3文件
                "-ar",
                "16000",  # 采样率 16kHz
                "-ac",
                "1",  # 单声道
                "-f",
                "s16le",  # 16位小端格式
                "-y",  # 覆盖输出文件
                pcm_temp_path,  # 输出PCM文件
            ]

            result = subprocess.run(
                cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True
            )

            if result.returncode != 0:
                raise Exception(f"ffmpeg转换失败: {result.stderr}")

            # 读取转换后的PCM数据
            with open(pcm_temp_path, "rb") as f:
                pcm_data = f.read()

            print(f"[转换] MP3({len(mp3_data)}字节) -> PCM({len(pcm_data)}字节)")
            return pcm_data

        finally:
            # 清理临时文件
            try:
                os.unlink(mp3_temp_path)
                os.unlink(pcm_temp_path)
            except:
                pass

    except Exception as e:
        print(f"[转换] MP3转PCM失败: {str(e)}")
        raise e


def speech_to_text(audio_data):
    """语音转文字"""
    try:
        # 创建回调对象
        callback = STTCallback()

        # 创建识别对象
        recognition = Recognition(
            model="paraformer-realtime-v2",
            format="wav",
            sample_rate=16000,
            language_hints=["zh", "en"],
            callback=callback,
        )

        # 开始识别
        recognition.start()

        # 发送音频数据
        chunk_size = 3200
        for i in range(0, len(audio_data), chunk_size):
            chunk = audio_data[i : i + chunk_size]
            recognition.send_audio_frame(chunk)
            time.sleep(0.01)  # 稍微延迟以模拟实时流

        # 停止识别
        recognition.stop()

        # 等待识别完成
        timeout = 10  # 10秒超时
        start_time = time.time()
        while not callback.is_complete and (time.time() - start_time) < timeout:
            time.sleep(0.1)

        if callback.error_message:
            raise Exception(f"STT错误: {callback.error_message}")

        return callback.result_text

    except Exception as e:
        print(f"[STT] 语音识别失败: {str(e)}")
        raise e


def text_inference(text):
    """文本推理，生成回复（限制20字内）"""
    try:
        headers = {
            "Authorization": f"Bearer {QWEN_API_KEY}",
            "Content-Type": "application/json",
        }

        data = {
            "model": "qwen-plus",
            "messages": [
                {
                    "role": "system",
                    "content": "你是一个智能助手。请用简洁的中文回答用户问题，回答内容控制在20个字以内。",
                },
                {"role": "user", "content": text},
            ],
        }

        response = requests.post(QWEN_API_URL, headers=headers, json=data)
        response.raise_for_status()

        result = response.json()
        response_text = result["choices"][0]["message"]["content"]

        # # 确保回复不超过20个字
        # if len(response_text) > 20:
        #     response_text = response_text[:20]

        print(f"[推理] 输入: {text}")
        print(f"[推理] 输出: {response_text}")

        return response_text

    except Exception as e:
        print(f"[推理] 文本推理失败: {str(e)}")
        raise e


def text_to_speech(text):
    """文字转语音，返回PCM格式音频"""
    try:
        # 创建语音合成器
        synthesizer = SpeechSynthesizer(model="cosyvoice-v2", voice="longxiaochun_v2")

        # 合成语音（返回MP3格式）
        mp3_audio_data = synthesizer.call(text)

        print(f"[TTS] 语音合成完成，开始转换为PCM格式")

        # 将MP3转换为PCM格式
        pcm_audio_data = convert_mp3_to_pcm(mp3_audio_data)

        print(f"[TTS] PCM转换完成，大小: {len(pcm_audio_data)} 字节")

        return pcm_audio_data

    except Exception as e:
        print(f"[TTS] 语音合成失败: {str(e)}")
        raise e


@app.route("/process_audio", methods=["POST"])
def process_audio():
    """处理音频的主要API端点"""
    try:
        # 检查是否有音频数据
        if "audio" not in request.files:
            return jsonify({"error": "没有找到音频文件"}), 400

        audio_file = request.files["audio"]
        if audio_file.filename == "":
            return jsonify({"error": "没有选择音频文件"}), 400

        # 读取PCM音频数据
        pcm_data = audio_file.read()
        print(f"[API] 接收到音频数据，大小: {len(pcm_data)} 字节")

        # 将PCM转换为WAV格式
        wav_data = pcm_to_wav(pcm_data)

        # 步骤1: 语音转文字
        print("[API] 开始语音识别...")
        text = speech_to_text(wav_data)
        if not text:
            return jsonify({"error": "语音识别失败，没有识别到文字"}), 400

        # 步骤2: 文本推理
        print("[API] 开始文本推理...")
        response_text = text_inference(text)

        # 步骤3: 文字转语音
        print("[API] 开始语音合成...")
        audio_response = text_to_speech(response_text)

        # 返回PCM音频数据
        audio_buffer = io.BytesIO(audio_response)
        audio_buffer.seek(0)

        return send_file(
            audio_buffer,
            mimetype="application/octet-stream",
            as_attachment=True,
            download_name="response.pcm",
        )

    except Exception as e:
        print(f"[API] 处理失败: {str(e)}")
        return jsonify({"error": f"处理失败: {str(e)}"}), 500


@app.route("/health", methods=["GET"])
def health_check():
    """健康检查端点"""
    return jsonify({"status": "ok", "message": "服务运行正常"})


@app.route("/", methods=["GET"])
def index():
    """根路径，显示API使用说明"""
    return jsonify(
        {
            "message": "千问大模型集成服务器",
            "endpoints": {
                "/process_audio": "POST - 处理音频数据（PCM格式）",
                "/health": "GET - 健康检查",
            },
            "usage": "发送PCM格式音频到 /process_audio 端点，将返回处理后的音频回复",
        }
    )


if __name__ == "__main__":
    print("启动千问大模型集成服务器...")
    print(f"API Key: {os.getenv('DASHSCOPE_API_KEY')[:10]}...")
    app.run(host="0.0.0.0", port=8080, debug=True)
