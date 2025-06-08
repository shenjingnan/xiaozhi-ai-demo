/**
 * @file audio_recorder.cc
 * @brief 音频录制器类实现
 */

#include "audio_recorder.h"
#include "system_config.h"

extern "C" {
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
}

static const char *TAG = "AudioRecorder";

AudioRecorder::AudioRecorder() 
    : vadnet(nullptr), vad_model_data(nullptr), vad_chunk_size(0),
      audio_buffer(nullptr), audio_buffer_pos(0), recording(false),
      last_speech_time(0), recording_start_time(0), speech_detected(false) {
}

AudioRecorder::~AudioRecorder() {
    // VAD功能暂时禁用，无需清理VAD资源
    if (audio_buffer) {
        free(audio_buffer);
    }
}

esp_err_t AudioRecorder::init(void *models) {
    ESP_LOGI(TAG, "正在初始化音频录制器...");
    
    // 分配音频缓冲区
    audio_buffer = (int16_t *)malloc(MAX_AUDIO_BUFFER_SIZE);
    if (audio_buffer == NULL) {
        ESP_LOGE(TAG, "音频缓冲区内存分配失败");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "音频录制缓冲区初始化成功，大小: %d 字节", MAX_AUDIO_BUFFER_SIZE);
    
    // 初始化VAD模型
    esp_err_t vad_ret = init_vad_model(models);
    if (vad_ret != ESP_OK) {
        ESP_LOGW(TAG, "VAD模型初始化失败，将使用传统静音检测方法");
    } else {
        ESP_LOGI(TAG, "✓ VAD语音活动检测模型初始化成功");
    }
    
    return ESP_OK;
}

esp_err_t AudioRecorder::init_vad_model(void *models) {
    ESP_LOGI(TAG, "正在初始化VAD语音活动检测模型...");

    // 暂时禁用VAD功能，使用传统静音检测
    ESP_LOGW(TAG, "VAD功能暂时禁用，使用传统静音检测方法");
    vadnet = NULL;
    vad_model_data = NULL;

    return ESP_OK;
}

void AudioRecorder::start_recording() {
    audio_buffer_pos = 0;
    recording = true;
    speech_detected = false;
    recording_start_time = xTaskGetTickCount();
    last_speech_time = xTaskGetTickCount();
    ESP_LOGI(TAG, "开始录制音频...");
}

void AudioRecorder::stop_recording() {
    recording = false;
    speech_detected = false;
    ESP_LOGI(TAG, "停止录制音频，录制了 %zu 字节", audio_buffer_pos * sizeof(int16_t));
}

esp_err_t AudioRecorder::add_audio_data(int16_t *buffer, int samples) {
    if (!recording) {
        return ESP_OK;
    }

    // 检查缓冲区是否有足够空间
    bool buffer_full = false;
    if (audio_buffer_pos + samples > MAX_AUDIO_BUFFER_SIZE / sizeof(int16_t)) {
        ESP_LOGW(TAG, "音频缓冲区已满，用户说话时间较长，不再添加新数据但继续VAD检测");
        buffer_full = true;
    } else {
        // 复制音频数据到缓冲区
        memcpy(&audio_buffer[audio_buffer_pos], buffer, samples * sizeof(int16_t));
        audio_buffer_pos += samples;
    }

    // 使用传统静音检测方法
    if (is_audio_silent(buffer, samples)) {
        TickType_t current_time = xTaskGetTickCount();
        // 100ms静音检测 - 更快发送音频到服务端
        if ((current_time - last_speech_time) > pdMS_TO_TICKS(100)) {
            ESP_LOGI(TAG, "检测到静音超过 100 毫秒，停止录制");
            stop_recording();
            return ESP_ERR_TIMEOUT;
        }
    } else {
        if (!speech_detected) {
            ESP_LOGI(TAG, "首次检测到语音活动");
        }
        speech_detected = true;
        last_speech_time = xTaskGetTickCount();
    }

    // 如果缓冲区满了，但用户还在说话，立即返回缓冲区满的状态
    // 这样主程序可以立即处理当前已录制的音频，而不是等待VAD检测到静音
    if (buffer_full) {
        ESP_LOGI(TAG, "缓冲区已满，立即处理当前录制的音频");
        stop_recording();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool AudioRecorder::check_timeout() {
    if (!recording) {
        return false;
    }
    
    TickType_t current_time = xTaskGetTickCount();
    TickType_t time_since_start = current_time - recording_start_time;
    
    // 如果录制开始后3秒内没有检测到任何语音，则超时
    if (!speech_detected && time_since_start > pdMS_TO_TICKS(CONVERSATION_TIMEOUT_MS)) {
        ESP_LOGI(TAG, "录制超时，3秒内未检测到语音");
        return true;
    }
    
    return false;
}

bool AudioRecorder::is_audio_silent(int16_t *buffer, int samples) {
    for (int i = 0; i < samples; i++) {
        if (abs(buffer[i]) > SILENCE_THRESHOLD) {
            return false;
        }
    }
    return true;
}
