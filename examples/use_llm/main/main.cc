/**
 * @file main.cc
 * @brief ESP32-S3 智能语音助手 - 语音命令识别主程序
 *
 * 本程序实现了完整的智能语音助手功能，包括：
 * 1. 语音唤醒检测 - 支持"你好小智"等多种唤醒词
 * 2. 命令词识别 - 支持"帮我开灯"、"帮我关灯"、"拜拜"等语音指令
 * 3. 音频反馈播放 - 通过MAX98357A功放播放确认音频
 * 4. LED灯控制 - 根据语音指令控制外接LED灯
 *
 * 硬件配置：
 * - ESP32-S3-DevKitC-1开发板（需要PSRAM版本）
 * - INMP441数字麦克风（音频输入）
 *   连接方式：VDD->3.3V, GND->GND, SD->GPIO6, WS->GPIO4, SCK->GPIO5
 * - MAX98357A数字功放（音频输出）
 *   连接方式：DIN->GPIO7, BCLK->GPIO15, LRC->GPIO16, VIN->3.3V, GND->GND
 * - 外接LED灯（GPIO21控制）
 *
 * 音频参数：
 * - 采样率：16kHz
 * - 声道：单声道(Mono)
 * - 位深度：16位
 *
 * 使用的AI模型：
 * - 唤醒词检测：WakeNet9 "你好小智"模型
 * - 命令词识别：MultiNet7中文命令词识别模型
 */

extern "C"
{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wn_iface.h"           // 唤醒词检测接口
#include "esp_wn_models.h"          // 唤醒词模型管理
#include "esp_mn_iface.h"           // 命令词识别接口
#include "esp_mn_models.h"          // 命令词模型管理
#include "esp_mn_speech_commands.h" // 命令词配置
#include "esp_process_sdkconfig.h"  // sdkconfig处理函数
#include "model_path.h"             // 模型路径定义
#include "bsp_board.h"              // 板级支持包，INMP441麦克风驱动
#include "esp_log.h"                // ESP日志系统
#include "mock_voices/welcome.h"    // 欢迎音频数据文件
#include "mock_voices/light_on.h"   // 开灯音频数据文件
#include "mock_voices/light_off.h"  // 关灯音频数据文件
#include "mock_voices/byebye.h"     // 再见音频数据文件
#include "mock_voices/custom.h"     // 自定义音频数据文件
#include "driver/gpio.h"            // GPIO驱动
#include "esp_wifi.h"               // WiFi驱动
#include "esp_event.h"              // 事件循环
#include "esp_netif.h"              // 网络接口
#include "nvs_flash.h"              // NVS存储
#include "esp_http_client.h"        // HTTP客户端
}

static const char *TAG = "语音识别"; // 日志标签

// WiFi配置
#define WIFI_SSID "1804"
#define WIFI_PASS "Sjn123123@"
#define WIFI_MAXIMUM_RETRY 5

// 服务器配置
#define SERVER_URL "http://192.168.1.152:8080/process_audio"

// WiFi事件组
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// 外接LED GPIO定义
#define LED_GPIO GPIO_NUM_21 // 外接LED灯珠连接到GPIO21

// 音频录制相关定义
#define MAX_AUDIO_BUFFER_SIZE (16000 * 2 * 10) // 10秒的音频缓冲区 (16kHz, 16bit)
#define SILENCE_THRESHOLD 500                  // 静音阈值
#define SILENCE_DURATION_MS 1000               // 静音持续时间（毫秒）

// 系统状态定义
typedef enum
{
    STATE_WAITING_WAKEUP = 0,  // 等待唤醒词
    STATE_WAITING_COMMAND = 1, // 等待命令词
    STATE_RECORDING_AUDIO = 2, // 录制音频中
} system_state_t;

// 命令词ID定义（对应commands_cn.txt中的ID）
#define COMMAND_TURN_OFF_LIGHT 308 // "帮我关灯"
#define COMMAND_TURN_ON_LIGHT 309  // "帮我开灯"
#define COMMAND_BYE_BYE 314        // "拜拜"
#define COMMAND_CUSTOM 315         // "自定义命令词"

// 命令词配置结构体
typedef struct
{
    int command_id;
    const char *pinyin;
    const char *description;
} command_config_t;

// 自定义命令词列表
static const command_config_t custom_commands[] = {
    {COMMAND_TURN_ON_LIGHT, "bang wo kai deng", "帮我开灯"},
    {COMMAND_TURN_OFF_LIGHT, "bang wo guan deng", "帮我关灯"},
    {COMMAND_BYE_BYE, "bai bai", "拜拜"},
    {COMMAND_CUSTOM, "xian zai an quan wu qing kuang ru he", "现在安全屋情况如何"},
};

#define CUSTOM_COMMANDS_COUNT (sizeof(custom_commands) / sizeof(custom_commands[0]))

// 全局变量
static system_state_t current_state = STATE_WAITING_WAKEUP;
static esp_mn_iface_t *multinet = NULL;
static model_iface_data_t *mn_model_data = NULL;
static TickType_t command_timeout_start = 0;
static const TickType_t COMMAND_TIMEOUT_MS = 5000; // 5秒超时

// 音频录制相关全局变量
static int16_t *audio_buffer = NULL;
static size_t audio_buffer_pos = 0;
static TickType_t last_audio_time = 0;
static bool is_recording = false;

// WiFi重连计数器
static int s_retry_num = 0;

// 函数声明
static esp_err_t test_network_connectivity(void);
static void wifi_scan_networks(void);

/**
 * @brief WiFi事件处理函数
 */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *disconnected = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGI(TAG, "WiFi断开连接，原因: %d", disconnected->reason);

        if (s_retry_num < WIFI_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "重试连接WiFi (第%d次)", s_retry_num);
        }
        else
        {
            ESP_LOGE(TAG, "WiFi连接失败，已达到最大重试次数");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "获得IP地址:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief 扫描可用的WiFi网络
 */
static void wifi_scan_networks(void)
{
    ESP_LOGI(TAG, "开始扫描WiFi网络...");

    wifi_scan_config_t scan_config = {};
    scan_config.ssid = NULL;
    scan_config.bssid = NULL;
    scan_config.channel = 0;
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 120;
    scan_config.scan_time.active.max = 150;

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi扫描启动失败: %s", esp_err_to_name(ret));
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "发现 %d 个WiFi网络", ap_count);

    if (ap_count > 0) {
        wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (ap_list != NULL) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_list);

            ESP_LOGI(TAG, "可用WiFi网络列表:");
            for (int i = 0; i < ap_count; i++) {
                ESP_LOGI(TAG, "  %d: %s (信号强度: %d dBm, 加密: %d)",
                         i+1, ap_list[i].ssid, ap_list[i].rssi, ap_list[i].authmode);

                // 检查目标网络是否在列表中
                if (strcmp((char*)ap_list[i].ssid, WIFI_SSID) == 0) {
                    ESP_LOGI(TAG, "  ✓ 找到目标网络 '%s'，信号强度: %d dBm",
                             WIFI_SSID, ap_list[i].rssi);
                }
            }
            free(ap_list);
        }
    }
}

/**
 * @brief 初始化WiFi连接
 */
static esp_err_t wifi_init_sta(void)
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
    strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi初始化完成");

    // 扫描可用网络
    wifi_scan_networks();

    /* 等待连接建立或失败 */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "WiFi连接成功，SSID:%s", WIFI_SSID);

        // 等待一下让网络稳定
        vTaskDelay(pdMS_TO_TICKS(2000));

        // 测试网络连通性
        esp_err_t test_ret = test_network_connectivity();
        if (test_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "网络连通性测试失败，但WiFi已连接");
        }

        return ESP_OK;
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "WiFi连接失败，SSID:%s", WIFI_SSID);
        return ESP_FAIL;
    }
    else
    {
        ESP_LOGE(TAG, "WiFi连接异常事件");
        return ESP_FAIL;
    }
}

/**
 * @brief 初始化外接LED GPIO
 *
 * 配置GPIO21为输出模式，用于控制外接LED灯珠
 */
static void init_led(void)
{
    ESP_LOGI(TAG, "正在初始化外接LED (GPIO21)...");

    // 配置GPIO21为输出模式
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),    // 设置GPIO21
        .mode = GPIO_MODE_OUTPUT,              // 输出模式
        .pull_up_en = GPIO_PULLUP_DISABLE,     // 禁用上拉
        .pull_down_en = GPIO_PULLDOWN_DISABLE, // 禁用下拉
        .intr_type = GPIO_INTR_DISABLE         // 禁用中断
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "外接LED GPIO初始化失败: %s", esp_err_to_name(ret));
        return;
    }

    // 初始状态设置为关闭（低电平）
    gpio_set_level(LED_GPIO, 0);
    ESP_LOGI(TAG, "✓ 外接LED初始化成功，初始状态：关闭");
}

static void led_turn_on(void)
{
    gpio_set_level(LED_GPIO, 1);
    ESP_LOGI(TAG, "外接LED点亮");
}

static void led_turn_off(void)
{
    gpio_set_level(LED_GPIO, 0);
    ESP_LOGI(TAG, "外接LED熄灭");
}

/**
 * @brief 初始化音频录制缓冲区
 */
static esp_err_t init_audio_buffer(void)
{
    audio_buffer = (int16_t *)malloc(MAX_AUDIO_BUFFER_SIZE);
    if (audio_buffer == NULL)
    {
        ESP_LOGE(TAG, "音频缓冲区内存分配失败");
        return ESP_ERR_NO_MEM;
    }
    audio_buffer_pos = 0;
    is_recording = false;
    ESP_LOGI(TAG, "音频录制缓冲区初始化成功，大小: %d 字节", MAX_AUDIO_BUFFER_SIZE);
    return ESP_OK;
}

/**
 * @brief 开始录制音频
 */
static void start_audio_recording(void)
{
    audio_buffer_pos = 0;
    is_recording = true;
    last_audio_time = xTaskGetTickCount();
    ESP_LOGI(TAG, "开始录制音频...");
}

/**
 * @brief 停止录制音频
 */
static void stop_audio_recording(void)
{
    is_recording = false;
    ESP_LOGI(TAG, "停止录制音频，录制了 %zu 字节", audio_buffer_pos * sizeof(int16_t));
}

/**
 * @brief 检测音频是否为静音
 */
static bool is_audio_silent(int16_t *buffer, int samples)
{
    for (int i = 0; i < samples; i++)
    {
        if (abs(buffer[i]) > SILENCE_THRESHOLD)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 添加音频数据到录制缓冲区
 */
static esp_err_t add_audio_to_buffer(int16_t *buffer, int samples)
{
    if (!is_recording)
    {
        return ESP_OK;
    }

    // 检查缓冲区是否有足够空间
    if (audio_buffer_pos + samples > MAX_AUDIO_BUFFER_SIZE / sizeof(int16_t))
    {
        ESP_LOGW(TAG, "音频缓冲区已满，停止录制");
        stop_audio_recording();
        return ESP_ERR_NO_MEM;
    }

    // 复制音频数据到缓冲区
    memcpy(&audio_buffer[audio_buffer_pos], buffer, samples * sizeof(int16_t));
    audio_buffer_pos += samples;

    // 检测静音
    if (is_audio_silent(buffer, samples))
    {
        TickType_t current_time = xTaskGetTickCount();
        if ((current_time - last_audio_time) > pdMS_TO_TICKS(SILENCE_DURATION_MS))
        {
            ESP_LOGI(TAG, "检测到静音超过 %d 毫秒，停止录制", SILENCE_DURATION_MS);
            stop_audio_recording();
            return ESP_ERR_TIMEOUT; // 使用超时错误码表示静音检测
        }
    }
    else
    {
        last_audio_time = xTaskGetTickCount();
    }

    return ESP_OK;
}

/**
 * @brief 测试网络连通性
 */
static esp_err_t test_network_connectivity(void)
{
    ESP_LOGI(TAG, "开始网络连通性测试...");

    // 简单的HTTP GET请求测试服务器连通性
    esp_http_client_config_t config = {};
    config.url = "http://192.168.1.152:8080/health";
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 10000; // 10秒超时

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "网络测试：HTTP客户端初始化失败");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "网络测试：无法连接到服务器: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "网络测试：服务器响应状态码: %d", status_code);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status_code == 200)
    {
        ESP_LOGI(TAG, "✓ 网络连通性测试成功");
        return ESP_OK;
    }
    else
    {
        ESP_LOGE(TAG, "✗ 网络连通性测试失败，状态码: %d", status_code);
        return ESP_FAIL;
    }
}

/**
 * @brief HTTP事件处理函数
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer;
    static int output_len;

    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (!esp_http_client_is_chunked_response(evt->client))
        {
            if (evt->user_data)
            {
                memcpy(evt->user_data + output_len, evt->data, evt->data_len);
                output_len += evt->data_len;
            }
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        output_len = 0;
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}

/**
 * @brief 发送音频数据到服务端并获取回复音频
 */
static esp_err_t send_audio_to_server(int16_t *audio_data, size_t audio_len, uint8_t **response_audio, size_t *response_len)
{
    esp_err_t ret = ESP_OK;

    // 首先检查WiFi连接状态
    wifi_ap_record_t ap_info;
    esp_err_t wifi_ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (wifi_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi未连接，无法发送请求: %s", esp_err_to_name(wifi_ret));
        return ESP_ERR_WIFI_NOT_CONNECT;
    }

    // 获取IP地址信息
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
    {
        ESP_LOGI(TAG, "当前IP地址: " IPSTR, IP2STR(&ip_info.ip));
        ESP_LOGI(TAG, "网关地址: " IPSTR, IP2STR(&ip_info.gw));
        ESP_LOGI(TAG, "子网掩码: " IPSTR, IP2STR(&ip_info.netmask));
    }

    ESP_LOGI(TAG, "尝试连接服务器: %s", SERVER_URL);

    // 分配响应缓冲区
    uint8_t *response_buffer = (uint8_t *)malloc(1024 * 1024); // 1MB缓冲区
    if (response_buffer == NULL)
    {
        ESP_LOGE(TAG, "响应缓冲区内存分配失败");
        return ESP_ERR_NO_MEM;
    }

    // 配置HTTP客户端
    esp_http_client_config_t config = {};
    config.url = SERVER_URL;
    config.method = HTTP_METHOD_POST;
    config.event_handler = http_event_handler;
    config.user_data = response_buffer;
    config.timeout_ms = 30000; // 30秒超时

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "HTTP客户端初始化失败");
        free(response_buffer);
        return ESP_FAIL;
    }

    // 设置请求头
    esp_http_client_set_header(client, "Content-Type", "multipart/form-data; boundary=----WebKitFormBoundary7MA4YWxkTrZu0gW");

    // 构建multipart/form-data请求体
    const char *boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    char header_part[512];
    char footer_part[128];
    int wlen;
    int content_length;
    int status_code;
    int data_read;

    snprintf(header_part, sizeof(header_part),
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"audio\"; filename=\"audio.pcm\"\r\n"
             "Content-Type: application/octet-stream\r\n\r\n",
             boundary);

    snprintf(footer_part, sizeof(footer_part), "\r\n--%s--\r\n", boundary);

    size_t total_len = strlen(header_part) + (audio_len * sizeof(int16_t)) + strlen(footer_part);

    ESP_LOGI(TAG, "发送音频数据到服务端，大小: %zu 字节", audio_len * sizeof(int16_t));

    // 设置请求体长度
    esp_http_client_set_post_field(client, NULL, total_len);

    // 开始请求
    ret = esp_http_client_open(client, total_len);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP客户端打开失败: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    // 发送header部分
    wlen = esp_http_client_write(client, header_part, strlen(header_part));
    if (wlen < 0)
    {
        ESP_LOGE(TAG, "发送HTTP头部失败");
        ret = ESP_FAIL;
        goto cleanup;
    }

    // 发送音频数据
    wlen = esp_http_client_write(client, (char *)audio_data, audio_len * sizeof(int16_t));
    if (wlen < 0)
    {
        ESP_LOGE(TAG, "发送音频数据失败");
        ret = ESP_FAIL;
        goto cleanup;
    }

    // 发送footer部分
    wlen = esp_http_client_write(client, footer_part, strlen(footer_part));
    if (wlen < 0)
    {
        ESP_LOGE(TAG, "发送HTTP尾部失败");
        ret = ESP_FAIL;
        goto cleanup;
    }

    // 获取响应
    content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0)
    {
        ESP_LOGE(TAG, "获取HTTP响应头失败");
        ret = ESP_FAIL;
        goto cleanup;
    }

    status_code = esp_http_client_get_status_code(client);
    if (status_code != 200)
    {
        ESP_LOGE(TAG, "服务端返回错误状态码: %d", status_code);
        ret = ESP_FAIL;
        goto cleanup;
    }

    // 读取响应数据
    data_read = esp_http_client_read_response(client, (char *)response_buffer, 1024 * 1024);
    if (data_read < 0)
    {
        ESP_LOGE(TAG, "读取HTTP响应失败");
        ret = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG, "从服务端接收到音频响应，大小: %d 字节", data_read);

    *response_audio = response_buffer;
    *response_len = data_read;

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK && response_buffer)
    {
        free(response_buffer);
    }

    return ret;
}

/**
 * @brief 配置自定义命令词
 *
 * 该函数会清除现有命令词，然后添加自定义命令词列表中的所有命令
 *
 * @param multinet 命令词识别接口指针
 * @param mn_model_data 命令词模型数据指针
 * @return esp_err_t
 *         - ESP_OK: 配置成功
 *         - ESP_FAIL: 配置失败
 */
static esp_err_t configure_custom_commands(esp_mn_iface_t *multinet, model_iface_data_t *mn_model_data)
{
    ESP_LOGI(TAG, "开始配置自定义命令词...");

    // 首先尝试从sdkconfig加载默认命令词配置
    esp_mn_commands_update_from_sdkconfig(multinet, mn_model_data);

    // 清除现有命令词，重新开始
    esp_mn_commands_clear();

    // 分配命令词管理结构
    esp_err_t ret = esp_mn_commands_alloc(multinet, mn_model_data);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "命令词管理结构分配失败: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    // 添加自定义命令词
    int success_count = 0;
    int fail_count = 0;

    for (int i = 0; i < CUSTOM_COMMANDS_COUNT; i++)
    {
        const command_config_t *cmd = &custom_commands[i];

        ESP_LOGI(TAG, "添加命令词 [%d]: %s (%s)",
                 cmd->command_id, cmd->description, cmd->pinyin);

        // 添加命令词
        esp_err_t ret_cmd = esp_mn_commands_add(cmd->command_id, cmd->pinyin);
        if (ret_cmd == ESP_OK)
        {
            success_count++;
            ESP_LOGI(TAG, "✓ 命令词 [%d] 添加成功", cmd->command_id);
        }
        else
        {
            fail_count++;
            ESP_LOGE(TAG, "✗ 命令词 [%d] 添加失败: %s",
                     cmd->command_id, esp_err_to_name(ret_cmd));
        }
    }

    // 更新命令词到模型
    ESP_LOGI(TAG, "更新命令词到模型...");
    esp_mn_error_t *error_phrases = esp_mn_commands_update();
    if (error_phrases != NULL && error_phrases->num > 0)
    {
        ESP_LOGW(TAG, "有 %d 个命令词更新失败:", error_phrases->num);
        for (int i = 0; i < error_phrases->num; i++)
        {
            ESP_LOGW(TAG, "  失败命令 %d: %s",
                     error_phrases->phrases[i]->command_id,
                     error_phrases->phrases[i]->string);
        }
    }

    // 打印配置结果
    ESP_LOGI(TAG, "命令词配置完成: 成功 %d 个, 失败 %d 个", success_count, fail_count);

    // 打印激活的命令词
    ESP_LOGI(TAG, "当前激活的命令词列表:");
    multinet->print_active_speech_commands(mn_model_data);

    // 打印支持的命令列表
    ESP_LOGI(TAG, "支持的语音命令:");
    for (int i = 0; i < CUSTOM_COMMANDS_COUNT; i++)
    {
        const command_config_t *cmd = &custom_commands[i];
        ESP_LOGI(TAG, "  ID=%d: '%s'", cmd->command_id, cmd->description);
    }

    return (fail_count == 0) ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 获取命令词的中文描述
 *
 * @param command_id 命令ID
 * @return const char* 命令的中文描述，如果未找到返回"未知命令"
 */
static const char *get_command_description(int command_id)
{
    for (int i = 0; i < CUSTOM_COMMANDS_COUNT; i++)
    {
        if (custom_commands[i].command_id == command_id)
        {
            return custom_commands[i].description;
        }
    }
    return "未知命令";
}

/**
 * @brief 执行退出逻辑
 *
 * 播放再见音频并返回等待唤醒状态
 */
static void execute_exit_logic(void)
{
    // 播放再见音频
    ESP_LOGI(TAG, "播放再见音频...");
    esp_err_t audio_ret = bsp_play_audio(byebye, byebye_len);
    if (audio_ret == ESP_OK)
    {
        ESP_LOGI(TAG, "✓ 再见音频播放成功");
    }
    else
    {
        ESP_LOGE(TAG, "再见音频播放失败: %s", esp_err_to_name(audio_ret));
    }

    current_state = STATE_WAITING_WAKEUP;
    ESP_LOGI(TAG, "返回等待唤醒状态，请说出唤醒词 '你好小智'");
}

/**
 * @brief 应用程序主入口函数
 *
 * 初始化INMP441麦克风硬件，加载唤醒词检测模型，
 * 然后进入主循环进行实时音频采集和唤醒词检测。
 */
extern "C" void app_main(void)
{
    // ========== 第一步：初始化NVS ==========
    ESP_LOGI(TAG, "正在初始化NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS分区需要擦除，正在擦除...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret == ESP_ERR_NOT_FOUND)
    {
        ESP_LOGE(TAG, "NVS分区未找到，请检查分区表配置");
        ESP_LOGE(TAG, "确保分区表中包含nvs分区");
        return;
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "✓ NVS初始化成功");

    // ========== 第二步：初始化WiFi ==========
    ESP_LOGI(TAG, "正在初始化WiFi连接...");
    ret = wifi_init_sta();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi初始化失败，将继续运行但无法使用网络功能");
    }
    else
    {
        ESP_LOGI(TAG, "✓ WiFi连接成功");
    }

    // ========== 第三步：初始化音频录制缓冲区 ==========
    ESP_LOGI(TAG, "正在初始化音频录制缓冲区...");
    ret = init_audio_buffer();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "音频录制缓冲区初始化失败: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "✓ 音频录制缓冲区初始化成功");

    // ========== 第四步：初始化外接LED ==========
    init_led();

    // ========== 第二步：初始化INMP441麦克风硬件 ==========
    ESP_LOGI(TAG, "正在初始化INMP441数字麦克风...");
    ESP_LOGI(TAG, "音频参数: 采样率16kHz, 单声道, 16位深度");

    ret = bsp_board_init(16000, 1, 16); // 16kHz, 单声道, 16位
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "INMP441麦克风初始化失败: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "请检查硬件连接: VDD->3.3V, GND->GND, SD->GPIO6, WS->GPIO4, SCK->GPIO5");
        return;
    }
    ESP_LOGI(TAG, "✓ INMP441麦克风初始化成功");

    // ========== 第三步：初始化音频播放功能 ==========
    ESP_LOGI(TAG, "正在初始化音频播放功能...");
    ESP_LOGI(TAG, "音频播放参数: 采样率16kHz, 单声道, 16位深度");

    ret = bsp_audio_init(16000, 1, 16); // 16kHz, 单声道, 16位
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "音频播放初始化失败: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "请检查MAX98357A硬件连接: DIN->GPIO7, BCLK->GPIO15, LRC->GPIO16");
        return;
    }
    ESP_LOGI(TAG, "✓ 音频播放初始化成功");

    // ========== 第四步：初始化语音识别模型 ==========
    ESP_LOGI(TAG, "正在初始化唤醒词检测模型...");

    // 检查内存状态
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "内存状态检查:");
    ESP_LOGI(TAG, "  - 总可用内存: %zu KB", free_heap / 1024);
    ESP_LOGI(TAG, "  - 内部RAM: %zu KB", free_internal / 1024);
    ESP_LOGI(TAG, "  - PSRAM: %zu KB", free_spiram / 1024);

    if (free_heap < 100 * 1024)
    {
        ESP_LOGE(TAG, "可用内存不足，需要至少100KB");
        return;
    }

    // 从模型目录加载所有可用的语音识别模型
    ESP_LOGI(TAG, "开始加载模型文件...");

    // 临时添加错误处理和重试机制
    srmodel_list_t *models = NULL;
    int retry_count = 0;
    const int max_retries = 3;

    while (models == NULL && retry_count < max_retries)
    {
        ESP_LOGI(TAG, "尝试加载模型 (第%d次)...", retry_count + 1);

        // 在每次重试前等待一下
        if (retry_count > 0)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        models = esp_srmodel_init("model");

        if (models == NULL)
        {
            ESP_LOGW(TAG, "模型加载失败，准备重试...");
            retry_count++;
        }
    }
    if (models == NULL)
    {
        ESP_LOGE(TAG, "语音识别模型初始化失败");
        ESP_LOGE(TAG, "请检查模型文件是否正确烧录到Flash分区");
        return;
    }

    // 自动选择sdkconfig中配置的唤醒词模型（如果配置了多个模型则选择第一个）
    char *model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    if (model_name == NULL)
    {
        ESP_LOGE(TAG, "未找到任何唤醒词模型！");
        ESP_LOGE(TAG, "请确保已正确配置并烧录唤醒词模型文件");
        ESP_LOGE(TAG, "可通过 'idf.py menuconfig' 配置唤醒词模型");
        return;
    }

    ESP_LOGI(TAG, "✓ 选择唤醒词模型: %s", model_name);

    // 获取唤醒词检测接口
    esp_wn_iface_t *wakenet = (esp_wn_iface_t *)esp_wn_handle_from_name(model_name);
    if (wakenet == NULL)
    {
        ESP_LOGE(TAG, "获取唤醒词接口失败，模型: %s", model_name);
        return;
    }

    // 创建唤醒词模型数据实例
    // DET_MODE_90: 检测模式，90%置信度阈值，平衡准确率和误触发率
    model_iface_data_t *model_data = wakenet->create(model_name, DET_MODE_90);
    if (model_data == NULL)
    {
        ESP_LOGE(TAG, "创建唤醒词模型数据失败");
        return;
    }

    // ========== 第五步：初始化命令词识别模型 ==========
    ESP_LOGI(TAG, "正在初始化命令词识别模型...");

    // 获取中文命令词识别模型（MultiNet7）
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (mn_name == NULL)
    {
        ESP_LOGE(TAG, "未找到中文命令词识别模型！");
        ESP_LOGE(TAG, "请确保已正确配置并烧录MultiNet7中文模型");
        return;
    }

    ESP_LOGI(TAG, "✓ 选择命令词模型: %s", mn_name);

    // 获取命令词识别接口
    multinet = esp_mn_handle_from_name(mn_name);
    if (multinet == NULL)
    {
        ESP_LOGE(TAG, "获取命令词识别接口失败，模型: %s", mn_name);
        return;
    }

    // 创建命令词模型数据实例
    mn_model_data = multinet->create(mn_name, 6000);
    if (mn_model_data == NULL)
    {
        ESP_LOGE(TAG, "创建命令词模型数据失败");
        return;
    }

    // 配置自定义命令词
    ESP_LOGI(TAG, "正在配置命令词...");
    esp_err_t cmd_config_ret = configure_custom_commands(multinet, mn_model_data);
    if (cmd_config_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "命令词配置失败");
        return;
    }
    ESP_LOGI(TAG, "✓ 命令词配置完成");

    // ========== 第六步：准备音频缓冲区 ==========
    // 获取模型要求的音频数据块大小（样本数 × 每样本字节数）
    int audio_chunksize = wakenet->get_samp_chunksize(model_data) * sizeof(int16_t);

    // 分配音频数据缓冲区内存
    int16_t *buffer = (int16_t *)malloc(audio_chunksize);
    if (buffer == NULL)
    {
        ESP_LOGE(TAG, "音频缓冲区内存分配失败，需要 %d 字节", audio_chunksize);
        ESP_LOGE(TAG, "请检查系统可用内存");
        return;
    }

    // 显示系统配置信息
    ESP_LOGI(TAG, "✓ 智能语音助手系统配置完成:");
    ESP_LOGI(TAG, "  - 唤醒词模型: %s", model_name);
    ESP_LOGI(TAG, "  - 命令词模型: %s", mn_name);
    ESP_LOGI(TAG, "  - 音频块大小: %d 字节", audio_chunksize);
    ESP_LOGI(TAG, "  - 检测置信度: 90%%");
    ESP_LOGI(TAG, "正在启动智能语音助手...");
    ESP_LOGI(TAG, "请对着麦克风说出唤醒词 '你好小智'");

    // ========== 第七步：主循环 - 实时音频采集与语音识别 ==========
    ESP_LOGI(TAG, "系统启动完成，等待唤醒词 '你好小智'...");

    while (1)
    {
        // 从INMP441麦克风获取一帧音频数据
        // false参数表示获取处理后的音频数据（非原始通道数据）
        ret = bsp_get_feed_data(false, buffer, audio_chunksize);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "麦克风音频数据获取失败: %s", esp_err_to_name(ret));
            ESP_LOGE(TAG, "请检查INMP441硬件连接");
            vTaskDelay(pdMS_TO_TICKS(10)); // 等待10ms后重试
            continue;
        }

        if (current_state == STATE_WAITING_WAKEUP)
        {
            // 第一阶段：唤醒词检测
            wakenet_state_t wn_state = wakenet->detect(model_data, buffer);

            if (wn_state == WAKENET_DETECTED)
            {
                ESP_LOGI(TAG, "🎉 检测到唤醒词 '你好小智'！");
                printf("=== 唤醒词检测成功！模型: %s ===\n", model_name);

                // 播放欢迎音频
                ESP_LOGI(TAG, "播放欢迎音频...");
                esp_err_t audio_ret = bsp_play_audio(welcome, welcome_len);
                if (audio_ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "音频播放失败: %s", esp_err_to_name(audio_ret));
                }
                else
                {
                    ESP_LOGI(TAG, "✓ 欢迎音频播放成功");
                }

                // 切换到命令词识别状态
                current_state = STATE_WAITING_COMMAND;
                command_timeout_start = xTaskGetTickCount();
                multinet->clean(mn_model_data); // 清理命令词识别缓冲区

                // 开始录制音频，准备检测命令词或发送到服务端
                start_audio_recording();

                ESP_LOGI(TAG, "进入命令词识别模式，请说出指令...");
                ESP_LOGI(TAG, "支持的指令: '帮我开灯'、'帮我关灯' 或 '拜拜'");
            }
        }
        else if (current_state == STATE_WAITING_COMMAND)
        {
            // 第二阶段：命令词识别和音频录制
            esp_mn_state_t mn_state = multinet->detect(mn_model_data, buffer);

            // 同时将音频数据添加到录制缓冲区
            esp_err_t record_ret = add_audio_to_buffer(buffer, audio_chunksize / sizeof(int16_t));
            if (record_ret == ESP_ERR_TIMEOUT)
            {
                // 检测到静音，停止录制，准备发送到服务端
                ESP_LOGI(TAG, "检测到静音，准备发送音频到服务端处理...");
                current_state = STATE_RECORDING_AUDIO;

                // 发送音频到服务端
                uint8_t *response_audio = NULL;
                size_t response_len = 0;
                esp_err_t send_ret = send_audio_to_server(audio_buffer, audio_buffer_pos, &response_audio, &response_len);

                if (send_ret == ESP_OK && response_audio != NULL)
                {
                    ESP_LOGI(TAG, "成功从服务端获取音频回复，开始播放...");

                    // 播放服务端返回的音频
                    esp_err_t play_ret = bsp_play_audio(response_audio, response_len);
                    if (play_ret == ESP_OK)
                    {
                        ESP_LOGI(TAG, "✓ 服务端音频回复播放成功");
                    }
                    else
                    {
                        ESP_LOGE(TAG, "服务端音频回复播放失败: %s", esp_err_to_name(play_ret));
                    }

                    // 释放响应音频内存
                    free(response_audio);
                }
                else
                {
                    ESP_LOGE(TAG, "发送音频到服务端失败: %s", esp_err_to_name(send_ret));

                    // 播放错误提示音频（可以使用自定义音频）
                    esp_err_t audio_ret = bsp_play_audio(custom, custom_len);
                    if (audio_ret == ESP_OK)
                    {
                        ESP_LOGI(TAG, "✓ 错误提示音频播放成功");
                    }
                }

                // 返回等待唤醒状态
                current_state = STATE_WAITING_WAKEUP;
                ESP_LOGI(TAG, "返回等待唤醒状态，请说出唤醒词 '你好小智'");
                continue;
            }

            if (mn_state == ESP_MN_STATE_DETECTED)
            {
                // 检测到命令词，停止录制
                stop_audio_recording();

                // 获取识别结果
                esp_mn_results_t *mn_result = multinet->get_results(mn_model_data);
                if (mn_result->num > 0)
                {
                    int command_id = mn_result->command_id[0];
                    float prob = mn_result->prob[0];

                    const char *cmd_desc = get_command_description(command_id);
                    ESP_LOGI(TAG, "🎯 检测到命令词: ID=%d, 置信度=%.2f, 内容=%s, 命令='%s'",
                             command_id, prob, mn_result->string, cmd_desc);

                    // 处理具体命令
                    if (command_id == COMMAND_TURN_ON_LIGHT)
                    {
                        ESP_LOGI(TAG, "💡 执行开灯命令");
                        led_turn_on();

                        // 播放开灯确认音频
                        esp_err_t audio_ret = bsp_play_audio(light_on, light_on_len);
                        if (audio_ret == ESP_OK)
                        {
                            ESP_LOGI(TAG, "✓ 开灯确认音频播放成功");
                        }
                    }
                    else if (command_id == COMMAND_TURN_OFF_LIGHT)
                    {
                        ESP_LOGI(TAG, "💡 执行关灯命令");
                        led_turn_off();

                        // 播放关灯确认音频
                        esp_err_t audio_ret = bsp_play_audio(light_off, light_off_len);
                        if (audio_ret == ESP_OK)
                        {
                            ESP_LOGI(TAG, "✓ 关灯确认音频播放成功");
                        }
                    }
                    else if (command_id == COMMAND_CUSTOM)
                    {
                        ESP_LOGI(TAG, "💡 执行自定义命令词");

                        // 播放自定义确认音频
                        esp_err_t audio_ret = bsp_play_audio(custom, custom_len);
                        if (audio_ret == ESP_OK)
                        {
                            ESP_LOGI(TAG, "✓ 自定义确认音频播放成功");
                        }
                    }
                    else if (command_id == COMMAND_BYE_BYE)
                    {
                        ESP_LOGI(TAG, "👋 检测到拜拜命令，立即退出");
                        execute_exit_logic();
                        continue; // 跳过后续的超时重置逻辑，直接进入下一次循环
                    }
                    else
                    {
                        ESP_LOGW(TAG, "⚠️  未知命令ID: %d", command_id);
                    }
                }

                // 命令处理完成，重新开始5秒倒计时，继续等待下一个命令
                command_timeout_start = xTaskGetTickCount();
                multinet->clean(mn_model_data); // 清理命令词识别缓冲区
                ESP_LOGI(TAG, "命令执行完成，重新开始5秒倒计时");
                ESP_LOGI(TAG, "可以继续说出指令: '帮我开灯'、'帮我关灯' 或 '拜拜'");
            }
            else if (mn_state == ESP_MN_STATE_TIMEOUT)
            {
                ESP_LOGW(TAG, "⏰ 命令词识别超时");
                stop_audio_recording();
                execute_exit_logic();
            }
            else
            {
                // 检查手动超时
                TickType_t current_time = xTaskGetTickCount();
                if ((current_time - command_timeout_start) > pdMS_TO_TICKS(COMMAND_TIMEOUT_MS))
                {
                    ESP_LOGW(TAG, "⏰ 命令词等待超时 (%lu秒)", (unsigned long)(COMMAND_TIMEOUT_MS / 1000));
                    stop_audio_recording();
                    execute_exit_logic();
                }
            }
        }

        // 短暂延时，避免CPU占用过高，同时保证实时性
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // ========== 资源清理 ==========
    // 注意：由于主循环是无限循环，以下代码正常情况下不会执行
    // 仅在程序异常退出时进行资源清理
    ESP_LOGI(TAG, "正在清理系统资源...");

    // 销毁唤醒词模型数据
    if (model_data != NULL)
    {
        wakenet->destroy(model_data);
    }

    // 释放音频缓冲区内存
    if (buffer != NULL)
    {
        free(buffer);
    }

    // 释放录制音频缓冲区内存
    if (audio_buffer != NULL)
    {
        free(audio_buffer);
    }

    // 删除当前任务
    vTaskDelete(NULL);
}
