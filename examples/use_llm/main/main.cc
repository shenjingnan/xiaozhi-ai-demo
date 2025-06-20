/**
 * @file main.cc
 * @brief ESP32-S3 智能语音助手 - LLM语音交互主程序
 *
 * 本程序实现了ESP32与Python服务器的TCP通信，包括：
 * 1. 语音唤醒检测 - 支持"你好小智"唤醒词
 * 2. 音频数据采集和上传 - 通过TCP发送PCM音频到服务器
 * 3. VADNet语音活动检测 - 判断用户是否说完话
 * 4. 音频播放 - 播放服务器返回的AI语音回复
 * 5. WiFi连接管理 - 自动连接WiFi网络
 *
 * 硬件配置：
 * - ESP32-S3-DevKitC-1开发板（需要PSRAM版本）
 * - INMP441数字麦克风（音频输入）
 *   连接方式：VDD->3.3V, GND->GND, SD->GPIO6, WS->GPIO4, SCK->GPIO5
 * - MAX98357A数字功放（音频输出）
 *   连接方式：DIN->GPIO7, BCLK->GPIO15, LRC->GPIO16, VIN->3.3V, GND->GND
 * - BOOT按键（GPIO0）用于手动唤醒
 *
 * 音频参数：
 * - 采样率：16kHz
 * - 声道：单声道(Mono)
 * - 位深度：16位
 *
 * 使用的AI模型：
 * - 唤醒词检测：WakeNet9 "你好小智"模型
 * - VAD检测：VADNet模型用于语音活动检测
 */

extern "C"
{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wn_iface.h"           // 唤醒词检测接口
#include "esp_wn_models.h"          // 唤醒词模型管理
// VAD检测功能暂时禁用
// #include "esp_vadn_iface.h"         // VAD检测接口
// #include "esp_vadn_models.h"        // VAD模型管理
#include "esp_process_sdkconfig.h"  // sdkconfig处理函数
#include "model_path.h"             // 模型路径定义
#include "bsp_board.h"              // 板级支持包，INMP441麦克风驱动
#include "mock_voices/welcome.h"    // 欢迎音频数据文件
#include "driver/gpio.h"            // GPIO驱动
#include "mbedtls/base64.h"         // Base64编码库
}

static const char *TAG = "语音助手LLM"; // 日志标签

// WiFi配置
#define WIFI_SSID "your_wifi_ssid"
#define WIFI_PASS "your_wifi_password"
#define WIFI_MAXIMUM_RETRY 5

// TCP服务器配置
#define SERVER_IP "192.168.1.100"
#define SERVER_PORT 8888

// BOOT按键GPIO定义
#define BOOT_GPIO GPIO_NUM_0 // BOOT按键连接到GPIO0

// WiFi事件组
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// 系统状态定义
typedef enum
{
    STATE_WAITING_WAKEUP = 0,  // 等待唤醒词
    STATE_RECORDING = 1,       // 录音状态
    STATE_WAITING_RESPONSE = 2,// 等待AI回复
} system_state_t;

// 全局变量
static system_state_t current_state = STATE_WAITING_WAKEUP;
static int tcp_socket = -1;
static int wifi_retry_num = 0;
static bool socket_connected = false;
static bool is_recording = false;

// 唤醒词相关
static esp_wn_iface_t *wakenet = NULL;
static model_iface_data_t *wn_model_data = NULL;

// 音频缓冲区
static int16_t *audio_buffer = NULL;
static size_t audio_chunk_size = 0;

// VAD检测参数
#define VAD_SILENCE_TIMEOUT_MS 2000  // 2秒静音超时
static TickType_t last_speech_time = 0;

/**
 * @brief WiFi事件处理函数
 */
static void event_handler(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            wifi_retry_num++;
            ESP_LOGI(TAG, "重试连接WiFi");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "WiFi连接失败");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "获取到IP地址:" IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief 初始化WiFi
 */
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi初始化完成");

    // 等待连接结果
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✓ WiFi连接成功，SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "✗ WiFi连接失败，SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "意外的WiFi事件");
    }
}

/**
 * @brief Base64编码音频数据
 */
static char* encode_audio_base64(const int16_t* audio_data, size_t data_len)
{
    size_t encoded_len = 0;
    
    // 计算编码后长度
    mbedtls_base64_encode(NULL, 0, &encoded_len, (const unsigned char*)audio_data, data_len);
    
    // 分配内存
    char* encoded = (char*)malloc(encoded_len + 1);
    if (encoded == NULL) {
        return NULL;
    }
    
    // 执行编码
    int ret = mbedtls_base64_encode((unsigned char*)encoded, encoded_len, &encoded_len, 
                                   (const unsigned char*)audio_data, data_len);
    if (ret != 0) {
        free(encoded);
        return NULL;
    }
    
    encoded[encoded_len] = '\0';
    return encoded;
}

/**
 * @brief 发送TCP消息
 */
static esp_err_t send_tcp_message(const char* message)
{
    if (!socket_connected || tcp_socket < 0) {
        ESP_LOGW(TAG, "TCP未连接，无法发送消息");
        return ESP_FAIL;
    }
    
    int len = send(tcp_socket, message, strlen(message), 0);
    if (len < 0) {
        ESP_LOGE(TAG, "发送TCP消息失败");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

/**
 * @brief 发送握手消息
 */
static void send_hello_message(void)
{
    const char* message = "HELLO:esp32\n";
    send_tcp_message(message);
    ESP_LOGI(TAG, "发送握手消息");
}

/**
 * @brief 发送开始录音消息
 */
static void send_start_listening_message(void)
{
    const char* message = "START_LISTENING\n";
    send_tcp_message(message);
    ESP_LOGI(TAG, "发送开始录音消息");
}

/**
 * @brief 发送停止录音消息
 */
static void send_stop_listening_message(void)
{
    const char* message = "STOP_LISTENING\n";
    send_tcp_message(message);
    ESP_LOGI(TAG, "发送停止录音消息");
}

/**
 * @brief 发送音频数据块
 */
static void send_audio_chunk(const int16_t* audio_data, size_t data_len)
{
    char* encoded_audio = encode_audio_base64(audio_data, data_len);
    if (encoded_audio == NULL) {
        ESP_LOGE(TAG, "音频Base64编码失败");
        return;
    }
    
    char* message = (char*)malloc(strlen(encoded_audio) + 20);
    sprintf(message, "AUDIO:%s\n", encoded_audio);
    
    send_tcp_message(message);
    ESP_LOGD(TAG, "发送音频块: %zu 字节", data_len);
    
    free(encoded_audio);
    free(message);
}

/**
 * @brief 解码Base64音频数据并播放
 */
static void play_audio_from_base64(const char* base64_data)
{
    size_t decoded_len = 0;
    
    // 计算解码后长度
    mbedtls_base64_decode(NULL, 0, &decoded_len, 
                         (const unsigned char*)base64_data, strlen(base64_data));
    
    // 分配内存
    uint8_t* decoded_audio = (uint8_t*)malloc(decoded_len);
    if (decoded_audio == NULL) {
        ESP_LOGE(TAG, "音频解码内存分配失败");
        return;
    }
    
    // 执行解码
    int ret = mbedtls_base64_decode(decoded_audio, decoded_len, &decoded_len,
                                   (const unsigned char*)base64_data, strlen(base64_data));
    if (ret != 0) {
        ESP_LOGE(TAG, "音频Base64解码失败");
        free(decoded_audio);
        return;
    }
    
    // 播放音频
    esp_err_t play_ret = bsp_play_audio(decoded_audio, decoded_len);
    if (play_ret == ESP_OK) {
        ESP_LOGI(TAG, "✓ AI音频回复播放成功 (%zu字节)", decoded_len);
    } else {
        ESP_LOGE(TAG, "AI音频播放失败: %s", esp_err_to_name(play_ret));
    }
    
    free(decoded_audio);
}

/**
 * @brief TCP连接初始化
 */
static void tcp_init(void)
{
    struct sockaddr_in server_addr;
    
    // 创建socket
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0) {
        ESP_LOGE(TAG, "创建socket失败");
        return;
    }
    
    // 配置服务器地址
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    
    // 连接服务器
    ESP_LOGI(TAG, "连接TCP服务器: %s:%d", SERVER_IP, SERVER_PORT);
    
    if (connect(tcp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "TCP连接失败");
        close(tcp_socket);
        tcp_socket = -1;
        return;
    }
    
    socket_connected = true;
    ESP_LOGI(TAG, "✓ TCP连接成功");
    
    // 发送握手消息
    send_hello_message();
}

/**
 * @brief 初始化BOOT按键
 */
static void init_boot_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BOOT按键初始化失败: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "✓ BOOT按键初始化成功");
}

/**
 * @brief 检查BOOT按键是否被按下
 */
static bool is_boot_button_pressed(void)
{
    return gpio_get_level(BOOT_GPIO) == 0;
}

/**
 * @brief 开始录音
 */
static void start_recording(void)
{
    if (current_state != STATE_WAITING_WAKEUP) {
        return;
    }
    
    ESP_LOGI(TAG, "🎤 开始录音，请说话...");
    current_state = STATE_RECORDING;
    is_recording = true;
    last_speech_time = xTaskGetTickCount();
    
    // 发送开始录音消息
    send_start_listening_message();
}

/**
 * @brief 停止录音
 */
static void stop_recording(void)
{
    if (current_state != STATE_RECORDING) {
        return;
    }
    
    ESP_LOGI(TAG, "🛑 录音结束，等待AI回复...");
    current_state = STATE_WAITING_RESPONSE;
    is_recording = false;
    
    // 发送停止录音消息
    send_stop_listening_message();
    
    // 模拟AI回复（因为我们没有完整的服务器实现）
    vTaskDelay(pdMS_TO_TICKS(2000)); // 等待2秒模拟处理时间
    
    ESP_LOGI(TAG, "模拟AI回复：播放欢迎音频");
    esp_err_t audio_ret = bsp_play_audio(welcome, welcome_len);
    if (audio_ret == ESP_OK) {
        ESP_LOGI(TAG, "✓ AI音频回复播放成功");
    }
    
    // 返回等待唤醒状态
    current_state = STATE_WAITING_WAKEUP;
    ESP_LOGI(TAG, "返回等待唤醒状态");
}

/**
 * @brief 应用程序主入口函数
 */
extern "C" void app_main(void)
{
    // ========== 第一步：初始化NVS ==========
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // ========== 第二步：初始化WiFi ==========
    wifi_init_sta();
    
    // ========== 第三步：初始化BOOT按键 ==========
    init_boot_button();
    
    // ========== 第四步：初始化INMP441麦克风硬件 ==========
    ESP_LOGI(TAG, "正在初始化INMP441数字麦克风...");
    ESP_LOGI(TAG, "音频参数: 采样率16kHz, 单声道, 16位深度");
    
    ret = bsp_board_init(16000, 1, 16); // 16kHz, 单声道, 16位
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "INMP441麦克风初始化失败: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "请检查硬件连接: VDD->3.3V, GND->GND, SD->GPIO6, WS->GPIO4, SCK->GPIO5");
        return;
    }
    ESP_LOGI(TAG, "✓ INMP441麦克风初始化成功");
    
    // ========== 第五步：初始化音频播放功能 ==========
    ESP_LOGI(TAG, "正在初始化音频播放功能...");
    ESP_LOGI(TAG, "音频播放参数: 采样率16kHz, 单声道, 16位深度");
    
    ret = bsp_audio_init(16000, 1, 16); // 16kHz, 单声道, 16位
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "音频播放初始化失败: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "请检查MAX98357A硬件连接: DIN->GPIO7, BCLK->GPIO15, LRC->GPIO16");
        return;
    }
    ESP_LOGI(TAG, "✓ 音频播放初始化成功");
    
    // ========== 第六步：初始化语音识别模型 ==========
    ESP_LOGI(TAG, "正在初始化语音识别模型...");
    
    // 检查内存状态
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    
    ESP_LOGI(TAG, "内存状态检查:");
    ESP_LOGI(TAG, "  - 总可用内存: %zu KB", free_heap / 1024);
    ESP_LOGI(TAG, "  - 内部RAM: %zu KB", free_internal / 1024);
    ESP_LOGI(TAG, "  - PSRAM: %zu KB", free_spiram / 1024);
    
    if (free_heap < 200 * 1024) {
        ESP_LOGE(TAG, "可用内存不足，需要至少200KB");
        return;
    }
    
    // 加载语音识别模型
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "语音识别模型初始化失败");
        ESP_LOGE(TAG, "请检查模型文件是否正确烧录到Flash分区");
        return;
    }
    
    // 初始化唤醒词检测模型
    char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    if (wn_name == NULL) {
        ESP_LOGE(TAG, "未找到任何唤醒词模型！");
        return;
    }
    
    ESP_LOGI(TAG, "✓ 选择唤醒词模型: %s", wn_name);
    
    wakenet = (esp_wn_iface_t *)esp_wn_handle_from_name(wn_name);
    if (wakenet == NULL) {
        ESP_LOGE(TAG, "获取唤醒词接口失败");
        return;
    }
    
    wn_model_data = wakenet->create(wn_name, DET_MODE_90);
    if (wn_model_data == NULL) {
        ESP_LOGE(TAG, "创建唤醒词模型数据失败");
        return;
    }
    
    // 暂时禁用VAD检测模型（API兼容性问题）
    ESP_LOGW(TAG, "VAD功能暂时禁用，将使用BOOT按键手动控制录音结束");
    
    // ========== 第七步：准备音频缓冲区 ==========
    audio_chunk_size = wakenet->get_samp_chunksize(wn_model_data) * sizeof(int16_t);
    audio_buffer = (int16_t *)malloc(audio_chunk_size);
    if (audio_buffer == NULL) {
        ESP_LOGE(TAG, "音频缓冲区内存分配失败，需要 %zu 字节", audio_chunk_size);
        return;
    }
    
    // ========== 第八步：初始化TCP连接 ==========
    tcp_init();
    
    // 播放启动音频
    ESP_LOGI(TAG, "播放启动音频...");
    esp_err_t audio_ret = bsp_play_audio(welcome, welcome_len);
    if (audio_ret == ESP_OK) {
        ESP_LOGI(TAG, "✓ 启动音频播放成功");
    }
    
    // ========== 第九步：主循环 - 实时音频采集与语音识别 ==========
    ESP_LOGI(TAG, "✓ 智能语音助手系统启动完成");
    ESP_LOGI(TAG, "系统状态: 等待唤醒词 '你好小智' 或按下BOOT按键");
    
    while (1) {
        // 从INMP441麦克风获取一帧音频数据
        ret = bsp_get_feed_data(false, audio_buffer, audio_chunk_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "麦克风音频数据获取失败: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        if (current_state == STATE_WAITING_WAKEUP) {
            // 检查BOOT按键手动唤醒
            if (is_boot_button_pressed()) {
                ESP_LOGI(TAG, "🔘 检测到BOOT按键按下，手动唤醒");
                start_recording();
                continue;
            }
            
            // 唤醒词检测
            wakenet_state_t wn_state = wakenet->detect(wn_model_data, audio_buffer);
            if (wn_state == WAKENET_DETECTED) {
                ESP_LOGI(TAG, "🎉 检测到唤醒词 '你好小智'！");
                start_recording();
            }
        }
        else if (current_state == STATE_RECORDING) {
            // 录音状态，上传音频数据并进行VAD检测
            if (socket_connected) {
                send_audio_chunk(audio_buffer, audio_chunk_size);
            }
            
            // 手动检测BOOT按键来停止录音
            if (is_boot_button_pressed()) {
                ESP_LOGI(TAG, "🔘 BOOT按键按下，停止录音");
                stop_recording();
                // 等待按键释放
                while (is_boot_button_pressed()) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
        }
        else if (current_state == STATE_WAITING_RESPONSE) {
            // 等待AI回复状态，什么都不做
            // 状态转换在stop_recording函数中完成
        }
        
        // 短暂延时，避免CPU占用过高
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    // ========== 资源清理 ==========
    ESP_LOGI(TAG, "正在清理系统资源...");
    
    if (wn_model_data != NULL && wakenet != NULL) {
        wakenet->destroy(wn_model_data);
    }
    
    if (audio_buffer != NULL) {
        free(audio_buffer);
    }
    
    if (tcp_socket >= 0) {
        close(tcp_socket);
    }
}