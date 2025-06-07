# For prerequisites running the following sample, visit https://help.aliyun.com/document_detail/611472.html
# 语音转文字示例代码

import os
import time
from dashscope.audio.asr import *

# 若没有将API Key配置到环境变量中，需将下面这行代码注释放开，并将apiKey替换为自己的API Key
# import dashscope
# dashscope.api_key = "apiKey"

from datetime import datetime

def get_timestamp():
    now = datetime.now()
    formatted_timestamp = now.strftime("[%Y-%m-%d %H:%M:%S.%f]")
    return formatted_timestamp

class Callback(RecognitionCallback):
    def on_complete(self) -> None:
        print(get_timestamp() + ' Recognition completed')  # recognition complete

    def on_error(self, result: RecognitionResult) -> None:
        print('Recognition task_id: ', result.request_id)
        print('Recognition error: ', result.message)
        exit(0)

    def on_event(self, result: RecognitionResult) -> None:
        sentence = result.get_sentence()
        if 'text' in sentence:
            print(get_timestamp() + ' RecognitionCallback text: ', sentence['text'])
            if RecognitionResult.is_sentence_end(sentence):
                print(get_timestamp() + 
                    'RecognitionCallback sentence end, request_id:%s, usage:%s'
                    % (result.get_request_id(), result.get_usage(sentence)))


callback = Callback()

recognition = Recognition(model='paraformer-realtime-v2',
                          format='wav',
                          sample_rate=16000,
                          # “language_hints”只支持paraformer-realtime-v2模型
                          language_hints=['zh', 'en'],
                          callback=callback)

recognition.start()

try:
    audio_data: bytes = None
    f = open("asr_example.wav", 'rb')
    if os.path.getsize("asr_example.wav"):
        while True:
            audio_data = f.read(3200)
            if not audio_data:
                break
            else:
                recognition.send_audio_frame(audio_data)
            time.sleep(0.1)
    else:
        raise Exception(
            'The supplied file was empty (zero bytes long)')
    f.close()
except Exception as e:
    raise e

recognition.stop()

print(
    '[Metric] requestId: {}, first package delay ms: {}, last package delay ms: {}'
    .format(
        recognition.get_last_request_id(),
        recognition.get_first_package_delay(),
        recognition.get_last_package_delay(),
    ))