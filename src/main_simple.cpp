#include <string>
#include <vector>
#include <cmath>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

static const char *TAG = "AUDIO_UPLOAD";

// 配置参数
namespace Config {
    // I2S配置 - INMP441麦克风
    const gpio_num_t I2S_WS_IO = GPIO_NUM_15;  // 左/右时钟
    const gpio_num_t I2S_SCK_IO = GPIO_NUM_14; // 位时钟
    const gpio_num_t I2S_SD_IO = GPIO_NUM_13;  // 数据
    const i2s_port_t I2S_PORT = I2S_NUM_0;

    // 音频配置
    const int SAMPLE_RATE = 16000;        // 16kHz
    const int CHANNELS = 1;               // 单声道
    const int FRAME_SIZE = 320;           // 20ms @ 16kHz
}

// I2S麦克风类
class I2SMicrophone {
private:
    i2s_chan_handle_t i2s_handle;

public:
    I2SMicrophone() : i2s_handle(nullptr) {}

    ~I2SMicrophone() {
        if (i2s_handle) {
            i2s_channel_disable(i2s_handle);
            i2s_del_channel(i2s_handle);
        }
    }

    esp_err_t init() {
        ESP_LOGI(TAG, "初始化I2S");

        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(Config::I2S_PORT, I2S_ROLE_MASTER);
        esp_err_t ret = i2s_new_channel(&chan_cfg, nullptr, &i2s_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "创建I2S通道失败: %s", esp_err_to_name(ret));
            return ret;
        }

        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(Config::SAMPLE_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = Config::I2S_SCK_IO,
                .ws = Config::I2S_WS_IO,
                .dout = I2S_GPIO_UNUSED,
                .din = Config::I2S_SD_IO,
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

        ret = i2s_channel_enable(i2s_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "启用I2S通道失败: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "I2S初始化完成");
        return ESP_OK;
    }

    esp_err_t readFrame(std::vector<int16_t>& pcm_data) {
        const size_t buffer_size = Config::FRAME_SIZE * sizeof(int32_t); // 32位数据
        std::vector<uint8_t> i2s_buffer(buffer_size);
        size_t bytes_read = 0;

        esp_err_t ret = i2s_channel_read(i2s_handle, i2s_buffer.data(), buffer_size, &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S读取失败: %d", ret);
            return ret;
        }

        // 将I2S数据转换为PCM数据
        pcm_data.resize(Config::FRAME_SIZE);
        for (int i = 0; i < Config::FRAME_SIZE; i++) {
            // INMP441输出的是32位有符号整数，左对齐，需要右移16位
            int32_t sample = *reinterpret_cast<int32_t*>(&i2s_buffer[i * sizeof(int32_t)]) >> 16;
            pcm_data[i] = static_cast<int16_t>(sample);
        }

        return ESP_OK;
    }
};

// 音频采集任务
class AudioCaptureTask {
private:
    I2SMicrophone microphone;
    TaskHandle_t task_handle;

    static void taskFunc(void* arg) {
        AudioCaptureTask* self = static_cast<AudioCaptureTask*>(arg);
        self->run();
    }

    void run() {
        ESP_LOGI(TAG, "启动音频采集任务");

        std::vector<int16_t> pcm_data;

        while (true) {
            // 从I2S读取数据
            if (microphone.readFrame(pcm_data) != ESP_OK) {
                continue;
            }

            // 打印音频数据的一些信息
            if (pcm_data.size() > 0) {
                ESP_LOGI(TAG, "采集到音频数据: %d 采样点, 第一个采样值: %d", 
                         pcm_data.size(), pcm_data[0]);
            }

            // 延迟一段时间
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

public:
    AudioCaptureTask() : task_handle(nullptr) {}

    bool start() {
        // 初始化I2S麦克风
        if (microphone.init() != ESP_OK) {
            return false;
        }

        // 创建任务
        BaseType_t ret = xTaskCreate(
            taskFunc,
            "audio_capture",
            4096,
            this,
            5,
            &task_handle
        );

        return (ret == pdPASS);
    }
};

// 主应用类
class AudioApp {
private:
    AudioCaptureTask capture_task;

public:
    void start() {
        ESP_LOGI(TAG, "启动音频采集应用");

        // 初始化NVS
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);

        // 启动音频采集任务
        if (!capture_task.start()) {
            ESP_LOGE(TAG, "启动音频采集任务失败");
            return;
        }

        ESP_LOGI(TAG, "应用启动成功");
    }
};

extern "C" void app_main(void) {
    AudioApp app;
    app.start();
} 