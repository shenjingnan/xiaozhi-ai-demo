#include "audio_player.h"
#include "esp_log.h"

static const char* TAG = "AUDIO_PLAYER";

// 构造函数
AudioPlayer::AudioPlayer()
    : i2s_handle(nullptr), play_task_handle(nullptr), audio_queue(nullptr),
      opus_decoder(nullptr), is_playing(false) {
}

// 析构函数
AudioPlayer::~AudioPlayer() {
    // 停止播放任务
    if (play_task_handle) {
        vTaskDelete(play_task_handle);
        play_task_handle = nullptr;
    }
    
    // 删除音频队列
    if (audio_queue) {
        vQueueDelete(audio_queue);
        audio_queue = nullptr;
    }
    
    // 销毁Opus解码器
    if (opus_decoder) {
        opus_decoder_destroy(opus_decoder);
        opus_decoder = nullptr;
    }
    
    // 关闭I2S接口
    if (i2s_handle) {
        i2s_channel_disable(i2s_handle);
        i2s_del_channel(i2s_handle);
        i2s_handle = nullptr;
    }
}

// 初始化I2S
esp_err_t AudioPlayer::initI2S() {
    ESP_LOGI(TAG, "初始化I2S功放接口");

    // 创建I2S通道
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AudioConfig::I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, &i2s_handle, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建I2S通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 配置I2S接口 - MAX98357A以标准I2S格式工作
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AudioConfig::SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AudioConfig::I2S_BCLK,
            .ws = AudioConfig::I2S_LRCLK,
            .dout = AudioConfig::I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(i2s_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化I2S标准模式失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 启用I2S通道
    ret = i2s_channel_enable(i2s_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启用I2S通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2S功放接口初始化完成");
    return ESP_OK;
}

// 初始化Opus解码器
bool AudioPlayer::initOpusDecoder() {
    int error;
    opus_decoder = opus_decoder_create(AudioConfig::SAMPLE_RATE, AudioConfig::CHANNELS, &error);
    
    if (error != OPUS_OK || opus_decoder == nullptr) {
        ESP_LOGE(TAG, "创建Opus解码器失败: %d", error);
        return false;
    }
    
    ESP_LOGI(TAG, "Opus解码器初始化完成");
    return true;
}

// 初始化播放器
bool AudioPlayer::init() {
    // 创建音频队列
    audio_queue = xQueueCreate(10, sizeof(std::vector<uint8_t>*));
    if (!audio_queue) {
        ESP_LOGE(TAG, "创建音频队列失败");
        return false;
    }
    
    // 初始化I2S接口
    if (initI2S() != ESP_OK) {
        ESP_LOGE(TAG, "初始化I2S接口失败");
        return false;
    }
    
    // 初始化Opus解码器
    if (!initOpusDecoder()) {
        ESP_LOGE(TAG, "初始化Opus解码器失败");
        return false;
    }
    
    // 创建播放任务
    BaseType_t ret = xTaskCreate(
        playTaskFunc,
        "audio_play",
        4096,
        this,
        5,
        &play_task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建播放任务失败");
        return false;
    }
    
    ESP_LOGI(TAG, "音频播放器初始化完成");
    return true;
}

// 静态任务函数
void AudioPlayer::playTaskFunc(void* arg) {
    AudioPlayer* self = static_cast<AudioPlayer*>(arg);
    self->playTask();
}

// 播放任务的执行函数
void AudioPlayer::playTask() {
    std::vector<uint8_t>* opus_data = nullptr;
    std::vector<int16_t> pcm_data(AudioConfig::BUFFER_SIZE);
    
    while (true) {
        // 等待音频数据
        if (xQueueReceive(audio_queue, &opus_data, portMAX_DELAY) == pdTRUE) {
            is_playing = true;
            ESP_LOGI(TAG, "开始解码和播放音频, 数据大小: %d", opus_data->size());
            
            // 解码Opus数据
            int frame_size = opus_decode(
                opus_decoder,
                opus_data->data(),
                opus_data->size(),
                pcm_data.data(),
                AudioConfig::BUFFER_SIZE,
                0
            );
            
            // 释放opus数据内存
            delete opus_data;
            
            if (frame_size < 0) {
                ESP_LOGE(TAG, "Opus解码错误: %d", frame_size);
                is_playing = false;
                continue;
            }
            
            // 调整PCM数据大小为实际解码的样本数
            pcm_data.resize(frame_size);
            
            // 输出到I2S接口
            size_t bytes_written = 0;
            size_t bytes_to_write = frame_size * sizeof(int16_t);
            
            esp_err_t ret = i2s_channel_write(
                i2s_handle,
                pcm_data.data(),
                bytes_to_write,
                &bytes_written,
                portMAX_DELAY
            );
            
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "I2S写入失败: %s", esp_err_to_name(ret));
            }
            
            ESP_LOGI(TAG, "音频播放完成, 写入 %d 字节", bytes_written);
            is_playing = false;
        }
    }
}

// 提交Opus编码的音频数据到播放队列
bool AudioPlayer::queueAudio(const uint8_t* data, size_t length) {
    if (!audio_queue) {
        ESP_LOGE(TAG, "音频队列未初始化");
        return false;
    }
    
    // 创建音频数据副本
    std::vector<uint8_t>* opus_data = new std::vector<uint8_t>(data, data + length);
    
    // 将数据指针放入队列
    if (xQueueSend(audio_queue, &opus_data, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "音频队列已满，无法添加新数据");
        delete opus_data;
        return false;
    }
    
    return true;
}

// 停止当前播放
void AudioPlayer::stop() {
    // 清空队列中的所有数据
    std::vector<uint8_t>* opus_data = nullptr;
    while (xQueueReceive(audio_queue, &opus_data, 0) == pdTRUE) {
        delete opus_data;
    }
    
    // 重置Opus解码器状态
    if (opus_decoder) {
        opus_decoder_ctl(opus_decoder, OPUS_RESET_STATE);
    }
    
    is_playing = false;
} 