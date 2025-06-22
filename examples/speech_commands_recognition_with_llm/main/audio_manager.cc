/**
 * @file audio_manager.cc
 * @brief 音频管理器类实现
 */

extern "C" {
#include <string.h>
#include "esp_log.h"
#include "bsp_board.h"
}

#include "audio_manager.h"

const char* AudioManager::TAG = "AudioManager";

AudioManager::AudioManager(uint32_t sample_rate, uint32_t recording_duration_sec, uint32_t response_duration_sec)
    : sample_rate(sample_rate)
    , recording_duration_sec(recording_duration_sec)
    , response_duration_sec(response_duration_sec)
    , recording_buffer(nullptr)
    , recording_buffer_size(0)
    , recording_length(0)
    , is_recording(false)
    , response_buffer(nullptr)
    , response_buffer_size(0)
    , response_length(0)
    , response_played(false)
    , ws_audio_buffer(nullptr)
    , ws_audio_buffer_size(0)
    , ws_audio_buffer_len(0)
    , receiving_audio(false)
    , last_audio_time(0)
{
    // 计算缓冲区大小
    recording_buffer_size = sample_rate * recording_duration_sec;  // 样本数
    response_buffer_size = sample_rate * response_duration_sec * sizeof(int16_t);  // 字节数
}

AudioManager::~AudioManager() {
    deinit();
}

esp_err_t AudioManager::init() {
    ESP_LOGI(TAG, "初始化音频管理器...");
    
    // 分配录音缓冲区
    recording_buffer = (int16_t*)malloc(recording_buffer_size * sizeof(int16_t));
    if (recording_buffer == nullptr) {
        ESP_LOGE(TAG, "录音缓冲区分配失败，需要 %zu 字节", 
                 recording_buffer_size * sizeof(int16_t));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 录音缓冲区分配成功，大小: %zu 字节 (%lu 秒)", 
             recording_buffer_size * sizeof(int16_t), (unsigned long)recording_duration_sec);
    
    // 分配响应缓冲区
    response_buffer = (int16_t*)calloc(response_buffer_size / sizeof(int16_t), sizeof(int16_t));
    if (response_buffer == nullptr) {
        ESP_LOGE(TAG, "响应缓冲区分配失败，需要 %zu 字节", response_buffer_size);
        free(recording_buffer);
        recording_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 响应缓冲区分配成功，大小: %zu 字节 (%lu 秒)", 
             response_buffer_size, (unsigned long)response_duration_sec);
    
    return ESP_OK;
}

void AudioManager::deinit() {
    if (recording_buffer != nullptr) {
        free(recording_buffer);
        recording_buffer = nullptr;
    }
    
    if (response_buffer != nullptr) {
        free(response_buffer);
        response_buffer = nullptr;
    }
    
    if (ws_audio_buffer != nullptr) {
        free(ws_audio_buffer);
        ws_audio_buffer = nullptr;
    }
}

// ========== 录音相关实现 ==========

void AudioManager::startRecording() {
    is_recording = true;
    recording_length = 0;
    ESP_LOGI(TAG, "开始录音...");
}

void AudioManager::stopRecording() {
    is_recording = false;
    ESP_LOGI(TAG, "停止录音，当前长度: %zu 样本 (%.2f 秒)", 
             recording_length, getRecordingDuration());
}

bool AudioManager::addRecordingData(const int16_t* data, size_t samples) {
    if (!is_recording || recording_buffer == nullptr) {
        return false;
    }
    
    // 检查缓冲区是否有足够空间
    if (recording_length + samples > recording_buffer_size) {
        ESP_LOGW(TAG, "录音缓冲区已满");
        return false;
    }
    
    // 复制数据到缓冲区
    memcpy(&recording_buffer[recording_length], data, samples * sizeof(int16_t));
    recording_length += samples;
    
    return true;
}

const int16_t* AudioManager::getRecordingBuffer(size_t& length) const {
    length = recording_length;
    return recording_buffer;
}

void AudioManager::clearRecordingBuffer() {
    recording_length = 0;
}

float AudioManager::getRecordingDuration() const {
    return (float)recording_length / sample_rate;
}

bool AudioManager::isRecordingBufferFull() const {
    return recording_length >= recording_buffer_size;
}

// ========== 响应音频相关实现 ==========

void AudioManager::startReceivingResponse() {
    response_length = 0;
    response_played = false;
}

bool AudioManager::addResponseData(const uint8_t* data, size_t size) {
    size_t samples = size / sizeof(int16_t);
    
    if (samples * sizeof(int16_t) > response_buffer_size) {
        ESP_LOGW(TAG, "响应数据过大，超过缓冲区限制");
        return false;
    }
    
    memcpy(response_buffer, data, size);
    response_length = samples;
    
    ESP_LOGI(TAG, "📦 接收到完整音频数据: %zu 字节, %zu 样本", size, samples);
    return true;
}

esp_err_t AudioManager::finishResponseAndPlay() {
    if (response_length == 0) {
        ESP_LOGW(TAG, "没有响应音频数据可播放");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "📢 播放响应音频: %zu 样本 (%.2f 秒)",
             response_length, (float)response_length / sample_rate);
    
    // 添加重试机制确保音频播放完整
    int retry_count = 0;
    const int max_retries = 3;
    esp_err_t audio_ret = ESP_FAIL;
    
    while (retry_count < max_retries && audio_ret != ESP_OK) {
        audio_ret = bsp_play_audio((const uint8_t*)response_buffer, response_length * sizeof(int16_t));
        if (audio_ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ 响应音频播放完成");
            response_played = true;
            break;
        } else {
            ESP_LOGE(TAG, "❌ 音频数据写入失败 (尝试 %d/%d): %s",
                     retry_count + 1, max_retries, esp_err_to_name(audio_ret));
            retry_count++;
            if (retry_count < max_retries) {
                vTaskDelay(pdMS_TO_TICKS(100)); // 等待100ms后重试
            }
        }
    }
    
    return audio_ret;
}

esp_err_t AudioManager::playAudio(const uint8_t* audio_data, size_t data_len, const char* description) {
    ESP_LOGI(TAG, "播放%s...", description);
    esp_err_t ret = bsp_play_audio(audio_data, data_len);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✓ %s播放成功", description);
    } else {
        ESP_LOGE(TAG, "%s播放失败: %s", description, esp_err_to_name(ret));
    }
    return ret;
}

// ========== WebSocket音频处理实现 ==========

bool AudioManager::processWebSocketData(uint8_t op_code, const uint8_t* data, size_t data_len, bool is_waiting_response) {
    // 检查是否是完整的数据包
    if (op_code == 0x08 && data_len == 2) {
        // WebSocket关闭帧
        ESP_LOGI(TAG, "收到WebSocket关闭帧");
        return false;
    }
    
    // 二进制数据处理 (op_code == 0x02 表示二进制帧)
    if (op_code == 0x02 && data_len > 0) {
        // 如果这是第一个二进制数据包
        if (!receiving_audio) {
            ESP_LOGI(TAG, "开始接收二进制音频数据");
            receiving_audio = true;
            
            // 分配缓冲区
            if (ws_audio_buffer) {
                free(ws_audio_buffer);
            }
            ws_audio_buffer_size = MAX_WS_AUDIO_SIZE;
            ws_audio_buffer = (uint8_t*)calloc(ws_audio_buffer_size, 1);
            if (!ws_audio_buffer) {
                ESP_LOGE(TAG, "无法分配音频缓冲区");
                receiving_audio = false;
                return false;
            }
            ws_audio_buffer_len = 0;
        }
        
        // 累积音频数据
        if (ws_audio_buffer && (ws_audio_buffer_len + data_len) <= ws_audio_buffer_size) {
            memcpy(ws_audio_buffer + ws_audio_buffer_len, data, data_len);
            ws_audio_buffer_len += data_len;
            last_audio_time = xTaskGetTickCount();
            
            // 每累积10KB显示一次进度
            if (ws_audio_buffer_len % 10240 < data_len) {
                ESP_LOGI(TAG, "累积音频数据: %zu KB", ws_audio_buffer_len / 1024);
            }
        }
        return false;  // 还在接收中
    }
    // 检测音频传输结束（收到ping包）
    else if (op_code == 0x09) { // ping帧
        ESP_LOGI(TAG, "收到ping包，检查是否有待播放的音频");
        
        if (receiving_audio && ws_audio_buffer && ws_audio_buffer_len > 0) {
            ESP_LOGI(TAG, "音频数据接收完成，总大小: %zu 字节 (%.2f 秒)",
                     ws_audio_buffer_len, (float)ws_audio_buffer_len / 2 / sample_rate);
            receiving_audio = false;
            
            // 播放累积的音频数据
            if (is_waiting_response) {
                addResponseData(ws_audio_buffer, ws_audio_buffer_len);
                finishResponseAndPlay();
            }
            
            // 清理缓冲区
            free(ws_audio_buffer);
            ws_audio_buffer = nullptr;
            ws_audio_buffer_size = 0;
            ws_audio_buffer_len = 0;
            
            return true;  // 音频处理完成
        }
    }
    // 超时检测（如果500ms没有新数据，认为传输结束）
    else if (receiving_audio && last_audio_time > 0 &&
             (xTaskGetTickCount() - last_audio_time) > pdMS_TO_TICKS(500)) {
        ESP_LOGI(TAG, "音频数据接收超时，准备播放");
        receiving_audio = false;
        
        // 播放累积的音频数据
        if (ws_audio_buffer && ws_audio_buffer_len > 0 && is_waiting_response) {
            ESP_LOGI(TAG, "音频数据接收完成（超时），总大小: %zu 字节 (%.2f 秒)",
                     ws_audio_buffer_len, (float)ws_audio_buffer_len / 2 / sample_rate);
            addResponseData(ws_audio_buffer, ws_audio_buffer_len);
            finishResponseAndPlay();
        }
        
        // 清理缓冲区
        if (ws_audio_buffer) {
            free(ws_audio_buffer);
            ws_audio_buffer = nullptr;
            ws_audio_buffer_size = 0;
            ws_audio_buffer_len = 0;
        }
        last_audio_time = 0;
        
        return true;  // 音频处理完成
    }
    
    return false;  // 还在处理中
}