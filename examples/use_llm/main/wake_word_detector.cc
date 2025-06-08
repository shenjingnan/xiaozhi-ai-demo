/**
 * @file wake_word_detector.cc
 * @brief 唤醒词检测器类实现
 */

#include "wake_word_detector.h"

extern "C" {
#include "esp_log.h"
#include "esp_process_sdkconfig.h"
}

static const char *TAG = "WakeWordDetector";

WakeWordDetector::WakeWordDetector() 
    : wakenet(nullptr), model_data(nullptr), model_name(nullptr), chunk_size(0) {
}

WakeWordDetector::~WakeWordDetector() {
    if (model_data && wakenet) {
        wakenet->destroy(model_data);
    }
}

esp_err_t WakeWordDetector::init(srmodel_list_t *models) {
    ESP_LOGI(TAG, "正在初始化唤醒词检测模型...");

    // 自动选择sdkconfig中配置的唤醒词模型
    model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    if (model_name == NULL) {
        ESP_LOGE(TAG, "未找到任何唤醒词模型！");
        ESP_LOGE(TAG, "请确保已正确配置并烧录唤醒词模型文件");
        ESP_LOGE(TAG, "可通过 'idf.py menuconfig' 配置唤醒词模型");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "✓ 选择唤醒词模型: %s", model_name);

    // 获取唤醒词检测接口
    wakenet = (esp_wn_iface_t *)esp_wn_handle_from_name(model_name);
    if (wakenet == NULL) {
        ESP_LOGE(TAG, "获取唤醒词接口失败，模型: %s", model_name);
        return ESP_FAIL;
    }

    // 创建唤醒词模型数据实例
    // DET_MODE_90: 检测模式，90%置信度阈值，平衡准确率和误触发率
    model_data = wakenet->create(model_name, DET_MODE_90);
    if (model_data == NULL) {
        ESP_LOGE(TAG, "创建唤醒词模型数据失败");
        return ESP_FAIL;
    }

    // 获取模型要求的音频数据块大小
    chunk_size = wakenet->get_samp_chunksize(model_data) * sizeof(int16_t);
    
    ESP_LOGI(TAG, "✓ 唤醒词检测器初始化成功");
    ESP_LOGI(TAG, "  - 模型: %s", model_name);
    ESP_LOGI(TAG, "  - 音频块大小: %d 字节", chunk_size);
    ESP_LOGI(TAG, "  - 检测置信度: 90%%");

    return ESP_OK;
}

wakenet_state_t WakeWordDetector::detect(int16_t *buffer) {
    if (!wakenet || !model_data) {
        return WAKENET_NO_DETECT;
    }
    
    return wakenet->detect(model_data, buffer);
}

int WakeWordDetector::get_chunk_size() {
    return chunk_size;
}
