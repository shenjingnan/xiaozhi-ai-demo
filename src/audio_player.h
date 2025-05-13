#pragma once

#include <vector>
#include <memory>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "opus/opus.h"  // 包含Opus头文件，使用正确的路径

// MAX98357A I2S配置
namespace AudioConfig {
    // I2S配置 - MAX98357A功放
    const gpio_num_t I2S_BCLK = GPIO_NUM_5;   // 位时钟
    const gpio_num_t I2S_LRCLK = GPIO_NUM_6;  // 左/右时钟
    const gpio_num_t I2S_DOUT = GPIO_NUM_7;   // 数据输出
    const i2s_port_t I2S_PORT = I2S_NUM_1;    // 注意：麦克风使用I2S_NUM_0

    // 音频配置
    const int SAMPLE_RATE = 16000;       // 16kHz
    const int SAMPLE_BITS = 16;          // 16位
    const int CHANNELS = 1;              // 单声道
    const int BUFFER_SIZE = 2048;        // 缓冲区大小
}

// 音频播放器类
class AudioPlayer {
private:
    i2s_chan_handle_t i2s_handle;
    TaskHandle_t play_task_handle;
    QueueHandle_t audio_queue;
    OpusDecoder* opus_decoder;
    bool is_playing;

    // 静态任务函数
    static void playTaskFunc(void* arg);
    
    // 播放任务的执行函数
    void playTask();
    
    // 初始化I2S
    esp_err_t initI2S();
    
    // 初始化Opus解码器
    bool initOpusDecoder();

public:
    AudioPlayer();
    ~AudioPlayer();

    // 初始化播放器
    bool init();
    
    // 提交Opus编码的音频数据到播放队列
    bool queueAudio(const uint8_t* data, size_t length);
    
    // 停止当前播放
    void stop();
    
    // 是否正在播放
    bool isPlaying() const { return is_playing; }
}; 