/**
 * @file audio_manager.h
 * @brief 音频管理器类 - 负责音频录制和播放功能
 * 
 * 此类封装了ESP32-S3的音频处理功能，包括：
 * - 音频录制管理（缓冲区管理、录音状态控制）
 * - 响应音频缓存和播放
 * - WebSocket音频数据接收和处理
 */

#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

class AudioManager {
public:
    /**
     * @brief 构造函数
     * 
     * @param sample_rate 采样率（Hz）
     * @param recording_duration_sec 录音缓冲区时长（秒）
     * @param response_duration_sec 响应缓冲区时长（秒）
     */
    AudioManager(uint32_t sample_rate = 16000, 
                 uint32_t recording_duration_sec = 10,
                 uint32_t response_duration_sec = 32);
    
    /**
     * @brief 析构函数，释放所有分配的内存
     */
    ~AudioManager();

    /**
     * @brief 初始化音频管理器，分配缓冲区
     * 
     * @return esp_err_t 初始化结果
     */
    esp_err_t init();

    /**
     * @brief 反初始化，释放资源
     */
    void deinit();

    // ========== 录音相关接口 ==========
    
    /**
     * @brief 开始录音
     */
    void startRecording();

    /**
     * @brief 停止录音
     */
    void stopRecording();

    /**
     * @brief 检查是否正在录音
     * 
     * @return true 正在录音，false 未在录音
     */
    bool isRecording() const { return is_recording; }

    /**
     * @brief 向录音缓冲区添加音频数据
     * 
     * @param data 音频数据
     * @param samples 样本数（不是字节数）
     * @return true 成功添加，false 缓冲区已满
     */
    bool addRecordingData(const int16_t* data, size_t samples);

    /**
     * @brief 获取录音缓冲区数据
     * 
     * @param[out] length 返回样本数
     * @return 录音数据指针
     */
    const int16_t* getRecordingBuffer(size_t& length) const;

    /**
     * @brief 清空录音缓冲区
     */
    void clearRecordingBuffer();

    /**
     * @brief 获取当前录音时长（秒）
     * 
     * @return 录音时长
     */
    float getRecordingDuration() const;

    /**
     * @brief 检查录音缓冲区是否已满
     * 
     * @return true 已满，false 未满
     */
    bool isRecordingBufferFull() const;

    // ========== 响应音频相关接口 ==========

    /**
     * @brief 开始接收响应音频数据（用于WebSocket）
     */
    void startReceivingResponse();

    /**
     * @brief 添加响应音频数据块
     * 
     * @param data 音频数据
     * @param size 数据大小（字节）
     * @return true 成功，false 失败（缓冲区溢出等）
     */
    bool addResponseData(const uint8_t* data, size_t size);

    /**
     * @brief 完成响应音频接收并播放
     * 
     * @return esp_err_t 播放结果
     */
    esp_err_t finishResponseAndPlay();
    
    // ========== 流式音频播放接口 ==========
    
    /**
     * @brief 开始流式音频播放
     */
    void startStreamingPlayback();
    
    /**
     * @brief 添加音频数据块到流式播放队列
     * 
     * @param data 音频数据
     * @param size 数据大小（字节）
     * @return true 成功，false 失败
     */
    bool addStreamingAudioChunk(const uint8_t* data, size_t size);
    
    /**
     * @brief 结束流式音频播放
     */
    void finishStreamingPlayback();
    
    /**
     * @brief 检查流式播放是否正在进行
     * 
     * @return true 正在播放，false 未在播放
     */
    bool isStreamingActive() const { return is_streaming; }
    
    /**
     * @brief 标记流式播放已完成
     */
    void setStreamingComplete() { response_played = true; }

    /**
     * @brief 播放指定的音频数据
     * 
     * @param audio_data 音频数据
     * @param data_len 数据长度（字节）
     * @param description 音频描述（用于日志）
     * @return esp_err_t 播放结果
     */
    esp_err_t playAudio(const uint8_t* audio_data, size_t data_len, const char* description);

    /**
     * @brief 检查响应音频是否已播放
     * 
     * @return true 已播放，false 未播放
     */
    bool isResponsePlayed() const { return response_played; }

    /**
     * @brief 重置响应播放标志
     */
    void resetResponsePlayedFlag() { response_played = false; }

    // ========== WebSocket音频处理 ==========

    /**
     * @brief 处理WebSocket接收到的音频数据
     * 
     * @param op_code WebSocket操作码
     * @param data 数据指针
     * @param data_len 数据长度
     * @param is_waiting_response 是否正在等待响应
     * @return true 音频处理完成，false 还在接收中
     */
    bool processWebSocketData(uint8_t op_code, const uint8_t* data, size_t data_len, bool is_waiting_response);

    // ========== 工具函数 ==========

    /**
     * @brief 获取采样率
     * 
     * @return 采样率（Hz）
     */
    uint32_t getSampleRate() const { return sample_rate; }

    /**
     * @brief 获取录音缓冲区大小（样本数）
     * 
     * @return 缓冲区大小
     */
    size_t getRecordingBufferSize() const { return recording_buffer_size; }

    /**
     * @brief 获取响应缓冲区大小（字节）
     * 
     * @return 缓冲区大小
     */
    size_t getResponseBufferSize() const { return response_buffer_size; }

private:
    // 音频参数
    uint32_t sample_rate;
    uint32_t recording_duration_sec;
    uint32_t response_duration_sec;

    // 录音相关
    int16_t* recording_buffer;
    size_t recording_buffer_size;  // 样本数
    size_t recording_length;       // 当前录音长度（样本数）
    bool is_recording;

    // 响应音频相关
    int16_t* response_buffer;
    size_t response_buffer_size;   // 字节数
    size_t response_length;        // 当前响应长度（样本数）
    bool response_played;

    // WebSocket音频接收相关
    uint8_t* ws_audio_buffer;
    size_t ws_audio_buffer_size;
    size_t ws_audio_buffer_len;
    bool receiving_audio;
    TickType_t last_audio_time;
    static const size_t MAX_WS_AUDIO_SIZE = 1024 * 1024; // 1MB
    
    // 流式播放相关
    bool is_streaming;
    uint8_t* streaming_buffer;
    size_t streaming_buffer_size;
    size_t streaming_write_pos;
    size_t streaming_read_pos;
    static const size_t STREAMING_BUFFER_SIZE = 32768; // 32KB环形缓冲区
    static const size_t STREAMING_CHUNK_SIZE = 3200;   // 每次播放的块大小

    // 日志标签
    static const char* TAG;
};

#endif // AUDIO_MANAGER_H