/**
 * @file audio_recorder.h
 * @brief 音频录制器类
 */

#pragma once

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
}

class AudioRecorder {
public:
    AudioRecorder();
    ~AudioRecorder();
    
    /**
     * @brief 初始化音频录制器
     * @param models 语音识别模型列表
     * @return esp_err_t
     */
    esp_err_t init(void *models);
    
    /**
     * @brief 开始录制音频
     */
    void start_recording();
    
    /**
     * @brief 停止录制音频
     */
    void stop_recording();
    
    /**
     * @brief 添加音频数据到缓冲区并进行VAD检测
     * @param buffer 音频数据
     * @param samples 样本数
     * @return esp_err_t ESP_ERR_TIMEOUT表示检测到语音结束
     */
    esp_err_t add_audio_data(int16_t *buffer, int samples);
    
    /**
     * @brief 获取录制的音频数据
     * @return int16_t* 音频数据指针
     */
    int16_t* get_audio_data() { return audio_buffer; }
    
    /**
     * @brief 获取录制的音频数据长度
     * @return size_t 音频数据长度（样本数）
     */
    size_t get_audio_length() { return audio_buffer_pos; }
    
    /**
     * @brief 检查是否正在录制
     * @return true 正在录制
     * @return false 未录制
     */
    bool is_recording() { return recording; }
    
    /**
     * @brief 检查超时（3秒无语音活动）
     * @return true 超时
     * @return false 未超时
     */
    bool check_timeout();

private:
    // VAD相关（暂时禁用）
    void *vadnet;
    void *vad_model_data;
    int vad_chunk_size;
    
    // 音频缓冲区
    int16_t *audio_buffer;
    size_t audio_buffer_pos;
    bool recording;
    
    // 时间跟踪
    TickType_t last_speech_time;
    TickType_t recording_start_time;
    bool speech_detected;
    
    /**
     * @brief 初始化VAD模型
     */
    esp_err_t init_vad_model(void *models);
    
    /**
     * @brief 传统静音检测方法
     */
    bool is_audio_silent(int16_t *buffer, int samples);
};
