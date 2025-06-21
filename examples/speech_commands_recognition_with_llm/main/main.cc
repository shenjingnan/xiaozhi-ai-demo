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
#include "freertos/stream_buffer.h"  // 流缓冲区
#include "freertos/event_groups.h"  // 事件组
#include "mbedtls/base64.h" // Base64编码库
#include "esp_timer.h"      // ESP定时器，用于获取时间戳
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
#include "driver/uart.h"            // UART驱动
#include "esp_wifi.h"               // WiFi驱动
#include "esp_event.h"              // 事件循环
#include "nvs_flash.h"              // NVS存储
#include "esp_netif.h"              // 网络接口
#include "esp_websocket_client.h"   // WebSocket客户端
}

static const char *TAG = "语音识别"; // 日志标签

// 外接LED GPIO定义
#define LED_GPIO GPIO_NUM_21 // 外接LED灯珠连接到GPIO21

// WiFi配置
#define WIFI_SSID "1804"
#define WIFI_PASS "Sjn123123@"
#define WIFI_MAXIMUM_RETRY 5

// WebSocket配置
#define WS_URI "ws://192.168.1.163:8888"  // 需要替换为实际的服务器IP

// WiFi事件组
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// WebSocket客户端句柄
static esp_websocket_client_handle_t ws_client = NULL;

// 系统状态定义
typedef enum
{
    STATE_WAITING_WAKEUP = 0,  // 等待唤醒词
    STATE_RECORDING = 1,       // 录音中
    STATE_WAITING_RESPONSE = 2, // 等待Python响应
    STATE_WAITING_COMMAND = 3, // 等待命令词
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

// 音频参数
#define SAMPLE_RATE 16000  // 采样率 16kHz

// 录音相关变量
#define RECORDING_BUFFER_SIZE (SAMPLE_RATE * 10 * 2) // 10秒的音频数据 (16kHz * 10s * 2字节)
static int16_t *recording_buffer = NULL;
static size_t recording_length = 0;
static bool is_recording = false;
static int silence_frames = 0;
static const int SILENCE_THRESHOLD = 200; // 静音阈值
static const int SILENCE_FRAMES_REQUIRED = 30; // 需要连续30帧静音才认为说话结束

// 接收音频相关变量
#define RESPONSE_BUFFER_SIZE (1024 * 1024) // 1MB的音频数据 (可容纳约32秒的16kHz音频)
static int16_t *response_buffer = NULL;
static size_t response_length = 0;
static bool response_played = false;  // 标记响应音频是否已播放

// WiFi重试计数
static int s_retry_num = 0;

/**
 * @brief 处理接收到的完整音频响应数据
 *
 * @param audio_data 二进制PCM音频数据
 * @param data_size 数据大小（字节）
 */
static void process_response_audio(const uint8_t *audio_data, size_t data_size)
{
    // 将数据复制到响应缓冲区
    response_length = data_size / sizeof(int16_t);
    if (response_length <= RESPONSE_BUFFER_SIZE / sizeof(int16_t))
    {
        memcpy(response_buffer, audio_data, data_size);
        ESP_LOGI(TAG, "📦 接收到完整音频数据: %zu 字节, %zu 样本", data_size, response_length);
        
        // 立即播放音频
        ESP_LOGI(TAG, "📢 播放响应音频: %zu 样本 (%.2f 秒)", 
                 response_length, (float)response_length / SAMPLE_RATE);
        
        // 添加重试机制确保音频播放完整
        int retry_count = 0;
        const int max_retries = 3;
        esp_err_t audio_ret = ESP_FAIL;
        
        while (retry_count < max_retries && audio_ret != ESP_OK) {
            audio_ret = bsp_play_audio((const uint8_t *)response_buffer, response_length * sizeof(int16_t));
            if (audio_ret == ESP_OK) {
                ESP_LOGI(TAG, "✅ 音频数据已写入I2S");
                
                // 计算音频播放时间并等待
                uint32_t play_time_ms = (response_length * sizeof(int16_t) / 2) * 1000 / SAMPLE_RATE;
                uint32_t buffer_time_ms = 50; // 额外的缓冲时间
                ESP_LOGI(TAG, "等待音频播放完成: %.2f 秒", (play_time_ms + buffer_time_ms) / 1000.0f);
                vTaskDelay(pdMS_TO_TICKS(play_time_ms + buffer_time_ms));
                
                // 播放完成后立即停止I2S，防止噪音
                esp_err_t stop_ret = bsp_audio_stop();
                if (stop_ret == ESP_OK) {
                    ESP_LOGI(TAG, "✅ 音频播放完成并已停止I2S");
                } else {
                    ESP_LOGW(TAG, "⚠️ 停止I2S时出现警告: %s", esp_err_to_name(stop_ret));
                }
                
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
    }
    else
    {
        ESP_LOGW(TAG, "响应音频数据过大 (%zu 样本)，超过缓冲区限制 (%d 样本)", 
                 response_length, RESPONSE_BUFFER_SIZE / sizeof(int16_t));
    }
}

/**
 * @brief WiFi事件处理器
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "重试连接WiFi... (%d/%d)", s_retry_num, WIFI_MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"WiFi连接失败");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "获得IP地址:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief WebSocket事件处理器
 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "🔗 WebSocket已连接");
        break;
        
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "🔌 WebSocket已断开");
        break;
        
    case WEBSOCKET_EVENT_DATA:
        {
            // 静态缓冲区用于累积二进制音频数据
            static uint8_t *audio_buffer = NULL;
            static size_t audio_buffer_size = 0;
            static size_t audio_buffer_len = 0;
            static const size_t MAX_AUDIO_SIZE = 1024 * 1024; // 最大1MB的音频数据
            static bool receiving_audio = false;
            static TickType_t last_audio_time = 0;
            
            ESP_LOGI(TAG, "收到WebSocket数据，长度: %d 字节, op_code: 0x%02x", data->data_len, data->op_code);
            
            // 检查是否是完整的数据包
            if (data->op_code == 0x08 && data->data_len == 2) {
                // WebSocket关闭帧
                ESP_LOGI(TAG, "收到WebSocket关闭帧");
                break;
            }
            
            // 二进制数据处理 (op_code == 0x02 表示二进制帧)
            if (data->op_code == 0x02 && data->data_len > 0) {
                // 如果这是第一个二进制数据包
                if (!receiving_audio) {
                    ESP_LOGI(TAG, "开始接收二进制音频数据");
                    receiving_audio = true;
                    
                    // 分配缓冲区
                    if (audio_buffer) {
                        free(audio_buffer);
                    }
                    audio_buffer_size = MAX_AUDIO_SIZE;
                    audio_buffer = (uint8_t *)calloc(audio_buffer_size, 1);
                    if (!audio_buffer) {
                        ESP_LOGE(TAG, "无法分配音频缓冲区");
                        receiving_audio = false;
                        break;
                    }
                    audio_buffer_len = 0;
                }
                
                // 累积音频数据
                if (audio_buffer && (audio_buffer_len + data->data_len) <= audio_buffer_size) {
                    memcpy(audio_buffer + audio_buffer_len, data->data_ptr, data->data_len);
                    audio_buffer_len += data->data_len;
                    last_audio_time = xTaskGetTickCount();
                    
                    // 每累积10KB显示一次进度
                    if (audio_buffer_len % 10240 < data->data_len) {
                        ESP_LOGI(TAG, "累积音频数据: %zu KB", audio_buffer_len / 1024);
                    }
                }
            }
            // 检测音频传输结束（收到ping包）
            else if (data->op_code == 0x09) {  // ping帧
                ESP_LOGI(TAG, "收到ping包，检查是否有待播放的音频");
                
                if (receiving_audio && audio_buffer && audio_buffer_len > 0) {
                    ESP_LOGI(TAG, "音频数据接收完成，总大小: %zu 字节 (%.2f 秒)", 
                             audio_buffer_len, (float)audio_buffer_len / 2 / SAMPLE_RATE);
                    receiving_audio = false;
                    
                    // 播放累积的音频数据
                    if (current_state == STATE_WAITING_RESPONSE) {
                        process_response_audio(audio_buffer, audio_buffer_len);
                    }
                    
                    // 清理缓冲区
                    free(audio_buffer);
                    audio_buffer = NULL;
                    audio_buffer_size = 0;
                    audio_buffer_len = 0;
                }
            }
            // 超时检测（如果500ms没有新数据，认为传输结束）
            else if (receiving_audio && last_audio_time > 0 && 
                     (xTaskGetTickCount() - last_audio_time) > pdMS_TO_TICKS(500)) {
                ESP_LOGI(TAG, "音频数据接收超时，准备播放");
                receiving_audio = false;
                
                // 播放累积的音频数据
                if (audio_buffer && audio_buffer_len > 0 && current_state == STATE_WAITING_RESPONSE) {
                    ESP_LOGI(TAG, "音频数据接收完成（超时），总大小: %zu 字节 (%.2f 秒)", 
                             audio_buffer_len, (float)audio_buffer_len / 2 / SAMPLE_RATE);
                    process_response_audio(audio_buffer, audio_buffer_len);
                }
                
                // 清理缓冲区
                if (audio_buffer) {
                    free(audio_buffer);
                    audio_buffer = NULL;
                    audio_buffer_size = 0;
                    audio_buffer_len = 0;
                }
                last_audio_time = 0;
            }
            // JSON数据处理（用于其他事件）
            else if (data->data_ptr && data->data_len > 0 && data->data_ptr[0] == '{') {
                // 创建临时缓冲区
                char *json_str = (char *)malloc(data->data_len + 1);
                if (json_str) {
                    memcpy(json_str, data->data_ptr, data->data_len);
                    json_str[data->data_len] = '\0';
                    ESP_LOGI(TAG, "收到JSON消息: %s", json_str);
                    free(json_str);
                }
            }
        }
        break;
        
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(TAG, "❌ WebSocket错误");
        break;
    }
}

/**
 * @brief 初始化WiFi连接
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
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    
    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi初始化完成");
    
    // 等待连接或失败
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE,
                                          pdFALSE,
                                          portMAX_DELAY);
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✅ WiFi连接成功: %s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "❌ WiFi连接失败: %s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "意外事件");
    }
}

/**
 * @brief 连接WebSocket服务器
 */
static void websocket_connect(void)
{
    if (ws_client != NULL)
    {
        ESP_LOGW(TAG, "WebSocket客户端已存在");
        return;
    }
    
    ESP_LOGI(TAG, "正在连接WebSocket服务器: %s", WS_URI);
    
    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = WS_URI;
    ws_cfg.buffer_size = 8192;
    
    ws_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    esp_websocket_client_start(ws_client);
    
    // 等待连接
    vTaskDelay(pdMS_TO_TICKS(500));
    
    if (esp_websocket_client_is_connected(ws_client))
    {
        ESP_LOGI(TAG, "✅ WebSocket连接成功");
    }
    else
    {
        ESP_LOGW(TAG, "⚠️ WebSocket连接可能未就绪");
    }
}

/**
 * @brief 断开WebSocket连接
 */
static void websocket_disconnect(void)
{
    if (ws_client != NULL)
    {
        ESP_LOGI(TAG, "正在断开WebSocket连接...");
        esp_websocket_client_stop(ws_client);
        esp_websocket_client_destroy(ws_client);
        ws_client = NULL;
        ESP_LOGI(TAG, "✅ WebSocket已断开");
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
 * @brief 发送录音缓冲区的所有音频数据
 *
 * 一次性发送完整的录音数据（二进制格式）
 */
static void send_recorded_audio(void)
{
    if (recording_buffer == NULL || recording_length == 0)
    {
        ESP_LOGW(TAG, "没有录音数据可发送");
        return;
    }
    
    if (ws_client == NULL || !esp_websocket_client_is_connected(ws_client))
    {
        ESP_LOGW(TAG, "WebSocket未连接，无法发送录音数据");
        return;
    }
    
    size_t data_size = recording_length * sizeof(int16_t);
    ESP_LOGI(TAG, "开始发送录音数据，总大小: %zu 样本 (%.2f 秒), %zu 字节", 
             recording_length, (float)recording_length / SAMPLE_RATE, data_size);
    
    // 直接发送二进制PCM数据
    esp_websocket_client_send_bin(ws_client, (const char *)recording_buffer, data_size, portMAX_DELAY);
    ESP_LOGI(TAG, "✅ 录音数据发送完成（二进制格式）");
}


// 注释掉不再需要的串口输入任务
#if 0
/**
 * @brief 串口输入处理任务
 *
 * 接收并处理来自Python脚本的JSON消息
 */
static void uart_input_task(void *pvParameters)
{
    char line_buffer[2048];
    int line_pos = 0;
    
    ESP_LOGI(TAG, "串口输入任务已启动");
    
    while (1)
    {
        int ch = getchar();
        if (ch != EOF)
        {
            if (ch == '\n')
            {
                line_buffer[line_pos] = '\0';
                
                // 尝试解析JSON
                if (line_buffer[0] == '{')
                {
                    // 简单的JSON解析，查找event字段
                    char *event_start = strstr(line_buffer, "\"event\":\"");
                    if (event_start)
                    {
                        event_start += 9; // 跳过 "event":"
                        char *event_end = strchr(event_start, '"');
                        if (event_end)
                        {
                            *event_end = '\0';
                            
                            if (strcmp(event_start, "response_started") == 0)
                            {
                                ESP_LOGI(TAG, "🎵 开始接收响应音频");
                                is_receiving_response = true;
                                response_length = 0;
                                expected_response_sequence = 0;
                            }
                            else if (strcmp(event_start, "response_audio") == 0)
                            {
                                // 提取sequence和data
                                char *seq_start = strstr(line_buffer, "\"sequence\":");
                                char *data_start = strstr(line_buffer, "\"data\":\"");
                                
                                if (seq_start && data_start)
                                {
                                    uint32_t sequence = 0;
                                    sscanf(seq_start + 11, "%lu", (unsigned long*)&sequence);
                                    
                                    data_start += 8; // 跳过 "data":"
                                    char *data_end = strchr(data_start, '"');
                                    if (data_end)
                                    {
                                        *data_end = '\0';
                                        process_response_audio(data_start, sequence);
                                    }
                                }
                            }
                            else if (strcmp(event_start, "response_stopped") == 0)
                            {
                                ESP_LOGI(TAG, "响应音频接收完成，准备播放");
                                is_receiving_response = false;
                                
                                // 播放接收到的音频
                                if (response_length > 0)
                                {
                                    size_t audio_bytes = response_length * sizeof(int16_t);
                                    ESP_LOGI(TAG, "播放响应音频: %zu 字节 (%.2f 秒)", 
                                             audio_bytes, (float)response_length / SAMPLE_RATE);
                                    
                                    esp_err_t audio_ret = bsp_play_audio((const unsigned char *)response_buffer, audio_bytes);
                                    if (audio_ret == ESP_OK)
                                    {
                                        ESP_LOGI(TAG, "✓ 响应音频播放完成");
                                    }
                                    else
                                    {
                                        ESP_LOGE(TAG, "响应音频播放失败: %s", esp_err_to_name(audio_ret));
                                    }
                                    
                                    // 播放完成后，切换到命令词识别状态
                                    if (current_state == STATE_WAITING_RESPONSE)
                                    {
                                        current_state = STATE_WAITING_COMMAND;
                                        command_timeout_start = xTaskGetTickCount();
                                        multinet->clean(mn_model_data); // 清理命令词识别缓冲区
                                        ESP_LOGI(TAG, "进入命令词识别模式，请说出指令...");
                                        ESP_LOGI(TAG, "支持的指令: '帮我开灯'、'帮我关灯' 或 '拜拜'");
                                    }
                                }
                            }
                        }
                    }
                }
                
                line_pos = 0;
            }
            else if (line_pos < sizeof(line_buffer) - 1)
            {
                line_buffer[line_pos++] = ch;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
#endif

/**
 * @brief 检测音频是否为静音
 *
 * @param buffer 音频数据缓冲区
 * @param samples 样本数
 * @return true 如果是静音
 * @return false 如果不是静音
 */
static bool is_silence(int16_t *buffer, int samples)
{
    int64_t sum = 0;
    for (int i = 0; i < samples; i++)
    {
        sum += abs(buffer[i]);
    }
    int avg = sum / samples;
    return avg < SILENCE_THRESHOLD;
}

/**
 * @brief 播放音频并自动停止I2S
 * 
 * @param audio_data 音频数据
 * @param data_len 数据长度
 * @param description 音频描述（用于日志）
 * @return esp_err_t 播放结果
 */
static esp_err_t play_audio_with_stop(const uint8_t *audio_data, size_t data_len, const char *description)
{
    esp_err_t ret = bsp_play_audio(audio_data, data_len);
    if (ret == ESP_OK) {
        // 计算播放时间
        uint32_t play_time_ms = (data_len / 2) * 1000 / SAMPLE_RATE;
        uint32_t buffer_time_ms = 50;
        ESP_LOGI(TAG, "等待%s播放完成: %.2f 秒", description, (play_time_ms + buffer_time_ms) / 1000.0f);
        vTaskDelay(pdMS_TO_TICKS(play_time_ms + buffer_time_ms));
        
        // 停止I2S
        esp_err_t stop_ret = bsp_audio_stop();
        if (stop_ret == ESP_OK) {
            ESP_LOGI(TAG, "✓ %s播放成功", description);
        } else {
            ESP_LOGW(TAG, "停止I2S时出现警告: %s", esp_err_to_name(stop_ret));
        }
    } else {
        ESP_LOGE(TAG, "%s播放失败: %s", description, esp_err_to_name(ret));
    }
    return ret;
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
    play_audio_with_stop(byebye, byebye_len, "再见音频");
    
    // 断开WebSocket连接
    websocket_disconnect();

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
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // ========== 第二步：初始化外接LED ==========
    init_led();
    
    // ========== 第三步：初始化WiFi ==========
    ESP_LOGI(TAG, "正在连接WiFi...");
    wifi_init_sta();

    // ========== 第四步：初始化INMP441麦克风硬件 ==========
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

    // 分配录音缓冲区
    recording_buffer = (int16_t *)malloc(RECORDING_BUFFER_SIZE);
    if (recording_buffer == NULL)
    {
        ESP_LOGE(TAG, "录音缓冲区内存分配失败，需要 %d 字节", RECORDING_BUFFER_SIZE);
        free(buffer);
        return;
    }
    ESP_LOGI(TAG, "✓ 录音缓冲区分配成功，大小: %d 字节", RECORDING_BUFFER_SIZE);
    
    // 分配响应音频缓冲区（使用calloc确保初始化为0）
    response_buffer = (int16_t *)calloc(RESPONSE_BUFFER_SIZE / sizeof(int16_t), sizeof(int16_t));
    if (response_buffer == NULL)
    {
        ESP_LOGE(TAG, "响应缓冲区内存分配失败，需要 %d 字节", RESPONSE_BUFFER_SIZE);
        free(buffer);
        free(recording_buffer);
        return;
    }
    ESP_LOGI(TAG, "✓ 响应缓冲区分配成功，大小: %d 字节（已初始化为0）", RESPONSE_BUFFER_SIZE);
    
    // 创建串口输入处理任务
    // 不再需要串口输入任务，改用WebSocket
    // xTaskCreate(uart_input_task, "uart_input", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "✓ 使用WebSocket进行通信");

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
        esp_err_t ret = bsp_get_feed_data(false, buffer, audio_chunksize);
        if (ret != ESP_OK)
        {
            // 仅在调试模式下输出错误日志
            #ifdef DEBUG_MODE
            ESP_LOGE(TAG, "麦克风音频数据获取失败: %s", esp_err_to_name(ret));
            ESP_LOGE(TAG, "请检查INMP441硬件连接");
            #endif
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
                
                // 连接WebSocket
                websocket_connect();
                
                // 通过WebSocket发送唤醒词检测事件
                if (ws_client != NULL && esp_websocket_client_is_connected(ws_client))
                {
                    char wake_msg[256];
                    snprintf(wake_msg, sizeof(wake_msg),
                            "{\"event\":\"wake_word_detected\",\"model\":\"%s\",\"timestamp\":%lld}", 
                            model_name, 
                            (long long)esp_timer_get_time() / 1000);
                    esp_websocket_client_send_text(ws_client, wake_msg, strlen(wake_msg), portMAX_DELAY);
                }

                // 播放欢迎音频
                ESP_LOGI(TAG, "播放欢迎音频...");
                play_audio_with_stop(welcome, welcome_len, "欢迎音频");

                // 确保I2S输出完全停止，避免录音时产生噪音
                vTaskDelay(pdMS_TO_TICKS(100)); // 额外等待100ms
                esp_err_t stop_ret = bsp_audio_stop();
                if (stop_ret != ESP_OK) {
                    ESP_LOGW(TAG, "录音前停止I2S失败: %s", esp_err_to_name(stop_ret));
                }

                // 切换到录音状态
                current_state = STATE_RECORDING;
                is_recording = true;
                recording_length = 0;
                silence_frames = 0;
                ESP_LOGI(TAG, "开始录音，请说话...");
            }
        }
        else if (current_state == STATE_RECORDING)
        {
            // 录音阶段：录制用户说话内容
            if (is_recording && recording_length < RECORDING_BUFFER_SIZE / sizeof(int16_t))
            {
                // 将音频数据存入录音缓冲区
                int samples = audio_chunksize / sizeof(int16_t);
                memcpy(&recording_buffer[recording_length], buffer, audio_chunksize);
                recording_length += samples;

                // 检测静音
                if (is_silence(buffer, samples))
                {
                    silence_frames++;
                    if (silence_frames >= SILENCE_FRAMES_REQUIRED)
                    {
                        // 检测到持续静音，认为用户说完了
                        ESP_LOGI(TAG, "检测到用户说话结束，录音长度: %zu 样本 (%.2f 秒)", 
                                 recording_length, (float)recording_length / SAMPLE_RATE);
                        is_recording = false;

                        // 发送录音数据到Python脚本
                        ESP_LOGI(TAG, "正在发送录音数据到电脑...");
                        send_recorded_audio();
                        
                        // 切换到等待响应状态
                        current_state = STATE_WAITING_RESPONSE;
                        response_played = false;  // 重置播放标志
                        ESP_LOGI(TAG, "等待服务器响应音频...");
                    }
                }
                else
                {
                    // 检测到声音，重置静音计数
                    silence_frames = 0;
                }
            }
            else if (recording_length >= RECORDING_BUFFER_SIZE / sizeof(int16_t))
            {
                // 录音缓冲区满了，强制停止录音
                ESP_LOGW(TAG, "录音缓冲区已满，停止录音");
                is_recording = false;
                
                // 发送录音数据到Python脚本
                ESP_LOGI(TAG, "正在发送录音数据到电脑...");
                send_recorded_audio();
                
                // 切换到等待响应状态
                current_state = STATE_WAITING_RESPONSE;
                response_played = false;  // 重置播放标志
                ESP_LOGI(TAG, "等待服务器响应音频...");
            }
        }
        else if (current_state == STATE_WAITING_RESPONSE)
        {
            // 等待响应状态：等待WebSocket响应并播放
            // 响应音频的播放在WebSocket事件处理器中完成
            
            // 检查是否已经播放完成
            if (response_played)
            {
                // 响应已播放完成，切换到命令词识别状态
                current_state = STATE_WAITING_COMMAND;
                command_timeout_start = xTaskGetTickCount();
                multinet->clean(mn_model_data);
                response_played = false;  // 重置标志
                ESP_LOGI(TAG, "进入命令词识别模式，请说出指令...");
                ESP_LOGI(TAG, "支持的指令: '帮我开灯'、'帮我关灯' 或 '拜拜'");
            }
        }
        else if (current_state == STATE_WAITING_COMMAND)
        {
            // 第二阶段：命令词识别
            esp_mn_state_t mn_state = multinet->detect(mn_model_data, buffer);

            if (mn_state == ESP_MN_STATE_DETECTED)
            {
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
                        play_audio_with_stop(light_on, light_on_len, "开灯确认音频");
                    }
                    else if (command_id == COMMAND_TURN_OFF_LIGHT)
                    {
                        ESP_LOGI(TAG, "💡 执行关灯命令");
                        led_turn_off();

                        // 播放关灯确认音频
                        play_audio_with_stop(light_off, light_off_len, "关灯确认音频");
                    }
                    else if (command_id == COMMAND_CUSTOM)
                    {
                        ESP_LOGI(TAG, "💡 执行自定义命令词");

                        // 播放自定义确认音频
                        play_audio_with_stop(custom, custom_len, "自定义确认音频");
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
                execute_exit_logic();
            }
            else
            {
                // 检查手动超时
                TickType_t current_time = xTaskGetTickCount();
                if ((current_time - command_timeout_start) > pdMS_TO_TICKS(COMMAND_TIMEOUT_MS))
                {
                    ESP_LOGW(TAG, "⏰ 命令词等待超时 (%lu秒)", (unsigned long)(COMMAND_TIMEOUT_MS / 1000));
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

    // 释放录音缓冲区内存
    if (recording_buffer != NULL)
    {
        free(recording_buffer);
    }
    
    // 释放响应缓冲区内存
    if (response_buffer != NULL)
    {
        free(response_buffer);
    }

    // 删除当前任务
    vTaskDelete(NULL);
}
