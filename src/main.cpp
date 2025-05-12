#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <queue>
#include <cmath>
#include <sstream>
#include <iomanip>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "opus/opus.h"
#include "mbedtls/base64.h"

static const char *TAG = "AUDIO_UPLOAD";

// 配置参数
namespace Config {
    // WiFi配置
    const char* WIFI_SSID = "您的WiFi名称";
    const char* WIFI_PASS = "您的WiFi密码";
    const int MAX_RETRY = 5;

    // API配置
    const char* API_ENDPOINT = "https://xyz.ai/api/audio";
    const char* API_KEY = "您的API密钥";

    // I2S配置 - INMP441麦克风
    const gpio_num_t I2S_WS_IO = GPIO_NUM_15;  // 左/右时钟
    const gpio_num_t I2S_SCK_IO = GPIO_NUM_14; // 位时钟
    const gpio_num_t I2S_SD_IO = GPIO_NUM_13;  // 数据
    const i2s_port_t I2S_PORT = I2S_NUM_0;

    // 音频配置
    const int SAMPLE_RATE = 16000;        // 16kHz
    const int SAMPLE_BITS = 32;           // INMP441输出32位
    const int CHANNELS = 1;               // 单声道
    const int DMA_BUF_COUNT = 8;
    const int DMA_BUF_LEN = 1024;

    // Opus编码配置
    const int FRAME_SIZE = 320;           // 20ms @ 16kHz
    const int MAX_PACKET_SIZE = 1500;     // 最大Opus包大小
    const int FRAMES_TO_SEND = 50;        // 约1秒的音频
}

// 音频数据队列
class AudioQueue {
private:
    std::queue<std::vector<int16_t>> queue;
    std::mutex mutex;
    SemaphoreHandle_t semaphore;

public:
    AudioQueue() {
        semaphore = xSemaphoreCreateBinary();
        xSemaphoreGive(semaphore);
    }

    ~AudioQueue() {
        vSemaphoreDelete(semaphore);
    }

    bool push(const std::vector<int16_t>& data) {
        if (xSemaphoreTake(semaphore, pdMS_TO_TICKS(10)) != pdTRUE) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (queue.size() >= 5) {
                ESP_LOGW(TAG, "队列已满，丢弃数据");
                xSemaphoreGive(semaphore);
                return false;
            }
            queue.push(data);
        }

        xSemaphoreGive(semaphore);
        return true;
    }

    bool pop(std::vector<int16_t>& data) {
        if (xSemaphoreTake(semaphore, pdMS_TO_TICKS(10)) != pdTRUE) {
            return false;
        }

        bool result = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!queue.empty()) {
                data = std::move(queue.front());
                queue.pop();
                result = true;
            }
        }

        xSemaphoreGive(semaphore);
        return result;
    }

    bool isEmpty() {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.empty();
    }
};

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
        const size_t buffer_size = Config::FRAME_SIZE * (Config::SAMPLE_BITS / 8);
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
            int32_t sample = *reinterpret_cast<int32_t*>(&i2s_buffer[i * (Config::SAMPLE_BITS / 8)]) >> 16;
            pcm_data[i] = static_cast<int16_t>(sample);
        }

        return ESP_OK;
    }
};

// Opus编码器类
class OpusEncoder {
private:
    ::OpusEncoder* encoder;
    std::vector<uint8_t> packet_buffer;

public:
    OpusEncoder() : encoder(nullptr) {
        packet_buffer.resize(Config::MAX_PACKET_SIZE);
    }

    ~OpusEncoder() {
        if (encoder) {
            opus_encoder_destroy(encoder);
        }
    }

    bool init() {
        int error;
        encoder = opus_encoder_create(Config::SAMPLE_RATE, Config::CHANNELS, OPUS_APPLICATION_VOIP, &error);
        if (error != OPUS_OK || encoder == nullptr) {
            ESP_LOGE(TAG, "创建Opus编码器失败: %d", error);
            return false;
        }

        // 设置编码器参数
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(16000));  // 16 kbps
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));   // 复杂度 (0-10)

        return true;
    }

    int encode(const std::vector<int16_t>& pcm_data, std::vector<uint8_t>& opus_data) {
        if (!encoder) {
            ESP_LOGE(TAG, "Opus编码器未初始化");
            return -1;
        }

        int nbytes = opus_encode(encoder, pcm_data.data(), Config::FRAME_SIZE,
                                packet_buffer.data(), Config::MAX_PACKET_SIZE);
        if (nbytes < 0) {
            ESP_LOGE(TAG, "Opus编码失败: %d", nbytes);
            return nbytes;
        }

        // 复制编码后的数据
        opus_data.resize(nbytes);
        std::copy(packet_buffer.begin(), packet_buffer.begin() + nbytes, opus_data.begin());

        return nbytes;
    }
};

// WiFi连接类
class WiFiManager {
private:
    bool connected;
    int retry_num;
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    static void eventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
        WiFiManager* self = static_cast<WiFiManager*>(arg);
        self->handleEvent(event_base, event_id, event_data);
    }

    void handleEvent(esp_event_base_t event_base, int32_t event_id, void* event_data) {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (retry_num < Config::MAX_RETRY) {
                esp_wifi_connect();
                retry_num++;
                ESP_LOGI(TAG, "重试连接到AP");
            } else {
                ESP_LOGI(TAG, "连接到AP失败");
                connected = false;
            }
        } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "获取IP地址:" IPSTR, IP2STR(&event->ip_info.ip));
            retry_num = 0;
            connected = true;
        }
    }

public:
    WiFiManager() : connected(false), retry_num(0), instance_any_id(nullptr), instance_got_ip(nullptr) {}

    ~WiFiManager() {
        // 注销事件处理程序
        if (instance_any_id) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
        }
        if (instance_got_ip) {
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
        }
    }

    bool init() {
        ESP_LOGI(TAG, "初始化WiFi");

        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                          ESP_EVENT_ANY_ID,
                                                          &WiFiManager::eventHandler,
                                                          this,
                                                          &instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                          IP_EVENT_STA_GOT_IP,
                                                          &WiFiManager::eventHandler,
                                                          this,
                                                          &instance_got_ip));

        wifi_config_t wifi_config = {};
        strncpy((char*)wifi_config.sta.ssid, Config::WIFI_SSID, sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, Config::WIFI_PASS, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "WiFi初始化完成");
        return true;
    }

    bool isConnected() const {
        return connected;
    }

    void waitForConnection() {
        while (!connected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
};

// 工具函数 - Base64编码
std::string base64Encode(const std::vector<uint8_t>& data) {
    size_t output_len = 0;
    // 计算Base64编码后的长度
    mbedtls_base64_encode(nullptr, 0, &output_len, data.data(), data.size());

    // 分配足够的内存
    std::vector<unsigned char> base64_buffer(output_len + 1); // +1 为了结尾的 null 字符

    // 执行编码
    mbedtls_base64_encode(base64_buffer.data(), base64_buffer.size(), &output_len,
                          data.data(), data.size());

    // 确保字符串正确终止
    base64_buffer[output_len] = 0;

    // 转换为std::string并返回
    return std::string(reinterpret_cast<char*>(base64_buffer.data()));
}

// HTTP客户端类
class HttpClient {
private:
    static esp_err_t httpEventHandler(esp_http_client_event_t *evt) {
        switch(evt->event_id) {
            case HTTP_EVENT_ERROR:
                ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
                break;
            case HTTP_EVENT_ON_CONNECTED:
                ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
                break;
            case HTTP_EVENT_HEADER_SENT:
                ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
                break;
            case HTTP_EVENT_ON_HEADER:
                ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
                break;
            case HTTP_EVENT_ON_DATA:
                ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
                if (evt->data_len) {
                    printf("%.*s", evt->data_len, (char*)evt->data);
                }
                break;
            case HTTP_EVENT_ON_FINISH:
                ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
                break;
            case HTTP_EVENT_DISCONNECTED:
                ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
                break;
        }
        return ESP_OK;
    }

public:
    bool postAudio(const std::vector<uint8_t>& opus_data) {
        ESP_LOGI(TAG, "发送音频数据，大小: %d 字节", opus_data.size());

        // 配置HTTP客户端
        esp_http_client_config_t config = {};
        config.url = Config::API_ENDPOINT;
        config.event_handler = httpEventHandler;
        config.method = HTTP_METHOD_POST;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "初始化HTTP客户端失败");
            return false;
        }

        // 将音频数据编码为Base64
        std::string base64_audio = base64Encode(opus_data);

        // 构建JSON数据
        std::stringstream json_ss;
        json_ss << "{\"audio_data\":\"" << base64_audio << "\",";
        json_ss << "\"format\":\"opus\",";
        json_ss << "\"sample_rate\":" << Config::SAMPLE_RATE << ",";
        json_ss << "\"channels\":" << Config::CHANNELS << "}";

        std::string json_data = json_ss.str();
        ESP_LOGI(TAG, "JSON数据大小: %d 字节", json_data.length());

        // 设置HTTP头
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Authorization", std::string("Bearer ").append(Config::API_KEY).c_str());

        // 设置POST数据
        esp_http_client_set_post_field(client, json_data.c_str(), json_data.length());

        // 执行HTTP请求
        esp_err_t err = esp_http_client_perform(client);
        bool success = false;

        if (err == ESP_OK) {
            int status_code = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "HTTP POST状态码 = %d", status_code);
            success = (status_code >= 200 && status_code < 300);
        } else {
            ESP_LOGE(TAG, "HTTP POST请求失败: %s", esp_err_to_name(err));
        }

        // 清理HTTP客户端
        esp_http_client_cleanup(client);
        return success;
    }
};

// 音频采集任务
class AudioCaptureTask {
private:
    I2SMicrophone microphone;
    AudioQueue& audio_queue;
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

            // 将PCM数据发送到队列
            if (!audio_queue.push(pcm_data)) {
                ESP_LOGW(TAG, "无法将PCM数据发送到队列");
            }
        }
    }

public:
    AudioCaptureTask(AudioQueue& queue) : audio_queue(queue), task_handle(nullptr) {}

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

// 音频上传任务
class AudioUploadTask {
private:
    AudioQueue& audio_queue;
    WiFiManager& wifi_manager;
    OpusEncoder opus_encoder;
    HttpClient http_client;
    TaskHandle_t task_handle;

    static void taskFunc(void* arg) {
        AudioUploadTask* self = static_cast<AudioUploadTask*>(arg);
        self->run();
    }

    void run() {
        ESP_LOGI(TAG, "启动音频上传任务");

        // 等待WiFi连接
        wifi_manager.waitForConnection();

        // 初始化Opus编码器
        if (!opus_encoder.init()) {
            ESP_LOGE(TAG, "初始化Opus编码器失败");
            vTaskDelete(nullptr);
            return;
        }

        std::vector<int16_t> pcm_data;
        std::vector<uint8_t> opus_data;
        std::vector<uint8_t> opus_buffer;
        opus_buffer.reserve(10 * 1024);  // 10KB缓冲区

        int frames_collected = 0;

        while (true) {
            // 等待PCM数据
            if (audio_queue.pop(pcm_data)) {
                // 编码PCM数据
                int nbytes = opus_encoder.encode(pcm_data, opus_data);
                if (nbytes < 0) {
                    continue;
                }

                // 将编码后的数据添加到缓冲区
                opus_buffer.insert(opus_buffer.end(), opus_data.begin(), opus_data.end());
                frames_collected++;

                // 当收集到足够的帧时，发送数据
                if (frames_collected >= Config::FRAMES_TO_SEND) {
                    // 发送数据
                    http_client.postAudio(opus_buffer);

                    // 重置缓冲区
                    opus_buffer.clear();
                    frames_collected = 0;
                }
            } else {
                // 如果队列为空，等待一段时间
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

public:
    AudioUploadTask(AudioQueue& queue, WiFiManager& wifi)
        : audio_queue(queue), wifi_manager(wifi), task_handle(nullptr) {}

    bool start() {
        // 创建任务
        BaseType_t ret = xTaskCreate(
            taskFunc,
            "audio_upload",
            8192,
            this,
            4,
            &task_handle
        );

        return (ret == pdPASS);
    }
};

// 主应用类
class AudioUploadApp {
private:
    AudioQueue audio_queue;
    WiFiManager wifi_manager;
    AudioCaptureTask capture_task;
    AudioUploadTask upload_task;

public:
    AudioUploadApp() : capture_task(audio_queue), upload_task(audio_queue, wifi_manager) {}

    void start() {
        ESP_LOGI(TAG, "启动音频采集和上传应用");

        // 初始化NVS
        esp_err_t ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            ret = nvs_flash_init();
        }
        ESP_ERROR_CHECK(ret);

        // 初始化WiFi
        if (!wifi_manager.init()) {
            ESP_LOGE(TAG, "初始化WiFi失败");
            return;
        }

        // 启动音频采集任务
        if (!capture_task.start()) {
            ESP_LOGE(TAG, "启动音频采集任务失败");
            return;
        }

        // 启动音频上传任务
        if (!upload_task.start()) {
            ESP_LOGE(TAG, "启动音频上传任务失败");
            return;
        }
    }
};

extern "C" void app_main(void) {
    AudioUploadApp app;
    app.start();
}
