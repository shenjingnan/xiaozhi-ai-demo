# coding=utf-8
# 文本转音频示例代码

import dashscope
from dashscope.audio.tts_v2 import *

# 若没有将API Key配置到环境变量中，需将your-api-key替换为自己的API Key
# dashscope.api_key = "your-api-key"

# 模型
model = "cosyvoice-v2"
# 音色
voice = "longxiaochun_v2"

# 实例化SpeechSynthesizer，并在构造方法中传入模型（model）、音色（voice）等请求参数
synthesizer = SpeechSynthesizer(model=model, voice=voice)
# 发送待合成文本，获取二进制音频
audio = synthesizer.call("今天天气怎么样？")
print('[Metric] requestId为：{}，首包延迟为：{}毫秒'.format(
    synthesizer.get_last_request_id(),
    synthesizer.get_first_package_delay()))

# 将音频保存至本地
with open('output.mp3', 'wb') as f:
    f.write(audio)