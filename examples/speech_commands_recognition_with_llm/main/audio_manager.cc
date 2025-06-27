/**
 * @file audio_manager.cc
 * @brief 🎧 音频管理器实现文件
 * 
 * 这里实现了audio_manager.h中声明的所有功能。
 * 主要包括录音缓冲区管理、音频播放控制和流式播放。
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
    , is_streaming(false)
    , streaming_buffer(nullptr)
    , streaming_buffer_size(STREAMING_BUFFER_SIZE)
    , streaming_write_pos(0)
    , streaming_read_pos(0)
{
    // 🧮 计算所需缓冲区大小
    recording_buffer_size = sample_rate * recording_duration_sec;  // 录音缓冲区（样本数）
    response_buffer_size = sample_rate * response_duration_sec * sizeof(int16_t);  // 响应缓冲区（字节数）
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
    
    // 分配流式播放缓冲区
    streaming_buffer = (uint8_t*)malloc(streaming_buffer_size);
    if (streaming_buffer == nullptr) {
        ESP_LOGE(TAG, "流式播放缓冲区分配失败，需要 %zu 字节", streaming_buffer_size);
        free(recording_buffer);
        free(response_buffer);
        recording_buffer = nullptr;
        response_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 流式播放缓冲区分配成功，大小: %zu 字节", streaming_buffer_size);
    
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
    
    if (streaming_buffer != nullptr) {
        free(streaming_buffer);
        streaming_buffer = nullptr;
    }
}

// 🎙️ ========== 录音功能实现 ==========

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
    
    // 📏 检查缓冲区是否还有空间
    if (recording_length + samples > recording_buffer_size) {
        ESP_LOGW(TAG, "录音缓冲区已满（超过10秒上限）");
        return false;
    }
    
    // 💾 将新的音频数据追加到缓冲区末尾
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

// 🔊 ========== 音频播放功能实现 ==========

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
    
    // 🔁 添加重试机制，确保音频可靠播放
    int retry_count = 0;
    const int max_retries = 3;
    esp_err_t audio_ret = ESP_FAIL;
    
    while (retry_count < max_retries && audio_ret != ESP_OK) {
        audio_ret = bsp_play_audio((const uint8_t*)response_buffer, response_length * sizeof(int16_t));
        if (audio_ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ 响应音频播放成功");
            response_played = true;
            break;
        } else {
            ESP_LOGE(TAG, "❌ 音频播放失败 (第%d次尝试): %s",
                     retry_count + 1, esp_err_to_name(audio_ret));
            retry_count++;
            if (retry_count < max_retries) {
                vTaskDelay(pdMS_TO_TICKS(100)); // 等100ms再试
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

// 🌐 ========== 网络音频处理（旧版本，现已弃用） ==========

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

// 🌊 ========== 流式播放功能实现 ==========

void AudioManager::startStreamingPlayback() {
    ESP_LOGI(TAG, "开始流式音频播放");
    is_streaming = true;
    streaming_write_pos = 0;
    streaming_read_pos = 0;
    
    // 清空缓冲区
    if (streaming_buffer) {
        memset(streaming_buffer, 0, streaming_buffer_size);
    }
}

bool AudioManager::addStreamingAudioChunk(const uint8_t* data, size_t size) {
    if (!is_streaming || !streaming_buffer || !data) {
        return false;
    }
    
    // 📏 计算环形缓冲区的剩余空间
    size_t available_space;
    if (streaming_write_pos >= streaming_read_pos) {
        // 写指针在读指针后面
        available_space = streaming_buffer_size - (streaming_write_pos - streaming_read_pos) - 1;
    } else {
        // 写指针在读指针前面（已绕回）
        available_space = streaming_read_pos - streaming_write_pos - 1;
    }
    
    if (size > available_space) {
        ESP_LOGW(TAG, "流式缓冲区空间不足: 需要 %zu, 可用 %zu", size, available_space);
        return false;
    }
    
    // 📝 将数据写入环形缓冲区
    size_t bytes_to_end = streaming_buffer_size - streaming_write_pos;
    if (size <= bytes_to_end) {
        // 简单情况：数据不跨越缓冲区末尾
        memcpy(streaming_buffer + streaming_write_pos, data, size);
        streaming_write_pos += size;
    } else {
        // 复杂情况：数据跨越末尾，需要分两段写入
        memcpy(streaming_buffer + streaming_write_pos, data, bytes_to_end);
        memcpy(streaming_buffer, data + bytes_to_end, size - bytes_to_end);
        streaming_write_pos = size - bytes_to_end;
    }
    
    // 如果写位置到达缓冲区末尾，循环回到开头
    if (streaming_write_pos >= streaming_buffer_size) {
        streaming_write_pos = 0;
    }
    
    ESP_LOGD(TAG, "添加流式音频块: %zu 字节, 写位置: %zu, 读位置: %zu", 
             size, streaming_write_pos, streaming_read_pos);
    
    // 🔍 检查是否有足够的数据可以播放
    size_t available_data;
    if (streaming_write_pos >= streaming_read_pos) {
        // 简单情况：写指针在读指针后面
        available_data = streaming_write_pos - streaming_read_pos;
    } else {
        // 复杂情况：数据跨越了缓冲区末尾
        available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
    }
    
    // 🎵 如果积累了足够的数据（200ms），开始播放
    while (available_data >= STREAMING_CHUNK_SIZE) {
        uint8_t chunk[STREAMING_CHUNK_SIZE];
        
        // 📖 从环形缓冲区读取一块数据
        size_t bytes_to_end = streaming_buffer_size - streaming_read_pos;
        if (STREAMING_CHUNK_SIZE <= bytes_to_end) {
            memcpy(chunk, streaming_buffer + streaming_read_pos, STREAMING_CHUNK_SIZE);
            streaming_read_pos += STREAMING_CHUNK_SIZE;
        } else {
            memcpy(chunk, streaming_buffer + streaming_read_pos, bytes_to_end);
            memcpy(chunk + bytes_to_end, streaming_buffer, STREAMING_CHUNK_SIZE - bytes_to_end);
            streaming_read_pos = STREAMING_CHUNK_SIZE - bytes_to_end;
        }
        
        // 如果读位置到达缓冲区末尾，循环回到开头
        if (streaming_read_pos >= streaming_buffer_size) {
            streaming_read_pos = 0;
        }
        
        // 🎶 播放这一块音频（使用流式版本，不会中断播放）
        esp_err_t ret = bsp_play_audio_stream(chunk, STREAMING_CHUNK_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "流式音频播放失败: %s", esp_err_to_name(ret));
            break;
        }
        
        // 重新计算可用数据
        if (streaming_write_pos >= streaming_read_pos) {
            available_data = streaming_write_pos - streaming_read_pos;
        } else {
            available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
        }
    }
    
    return true;
}

void AudioManager::finishStreamingPlayback() {
    if (!is_streaming) {
        return;
    }
    
    ESP_LOGI(TAG, "结束流式音频播放");
    
    // 🎬 处理最后的尾巴数据（不足200ms的部分）
    size_t remaining_data;
    if (streaming_write_pos >= streaming_read_pos) {
        remaining_data = streaming_write_pos - streaming_read_pos;
    } else {
        remaining_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
    }
    
    if (remaining_data > 0) {
        // 分配临时缓冲区
        uint8_t* remaining_buffer = (uint8_t*)malloc(remaining_data);
        if (remaining_buffer) {
            // 读取所有剩余数据
            if (streaming_write_pos >= streaming_read_pos) {
                memcpy(remaining_buffer, streaming_buffer + streaming_read_pos, remaining_data);
            } else {
                size_t bytes_to_end = streaming_buffer_size - streaming_read_pos;
                memcpy(remaining_buffer, streaming_buffer + streaming_read_pos, bytes_to_end);
                memcpy(remaining_buffer + bytes_to_end, streaming_buffer, streaming_write_pos);
            }
            
            // 🎹 播放最后的尾巴数据（使用普通版本，会停止I2S）
            esp_err_t ret = bsp_play_audio(remaining_buffer, remaining_data);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "✅ 播放剩余音频: %zu 字节", remaining_data);
            } else {
                ESP_LOGE(TAG, "❌ 播放剩余音频失败: %s", esp_err_to_name(ret));
            }
            
            free(remaining_buffer);
        }
    }
    
    is_streaming = false;
    streaming_write_pos = 0;
    streaming_read_pos = 0;
}