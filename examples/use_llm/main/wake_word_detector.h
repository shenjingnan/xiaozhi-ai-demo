/**
 * @file wake_word_detector.h
 * @brief 唤醒词检测器类
 */

#pragma once

extern "C" {
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
}

class WakeWordDetector {
public:
    WakeWordDetector();
    ~WakeWordDetector();
    
    /**
     * @brief 初始化唤醒词检测器
     * @param models 语音识别模型列表
     * @return esp_err_t 
     */
    esp_err_t init(srmodel_list_t *models);
    
    /**
     * @brief 检测唤醒词
     * @param buffer 音频数据缓冲区
     * @return wakenet_state_t 检测结果
     */
    wakenet_state_t detect(int16_t *buffer);
    
    /**
     * @brief 获取音频块大小
     * @return int 音频块大小（字节）
     */
    int get_chunk_size();
    
    /**
     * @brief 获取模型名称
     * @return const char* 模型名称
     */
    const char* get_model_name() const { return model_name; }

private:
    esp_wn_iface_t *wakenet;
    model_iface_data_t *model_data;
    char *model_name;
    int chunk_size;
};
