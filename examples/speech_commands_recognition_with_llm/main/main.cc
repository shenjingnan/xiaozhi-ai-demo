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
#include "freertos/stream_buffer.h" // 流缓冲区
#include "freertos/event_groups.h"  // 事件组
#include "mbedtls/base64.h"         // Base64编码库
#include "esp_timer.h"              // ESP定时器，用于获取时间戳
#include "esp_wn_iface.h"           // 唤醒词检测接口
#include "esp_wn_models.h"          // 唤醒词模型管理
#include "esp_mn_iface.h"           // 命令词识别接口
#include "esp_mn_models.h"          // 命令词模型管理
#include "esp_mn_speech_commands.h" // 命令词配置
#include "esp_process_sdkconfig.h"  // sdkconfig处理函数
#include "esp_vad.h"                // VAD接口
#include "model_path.h"             // 模型路径定义
#include "bsp_board.h"              // 板级支持包，INMP441麦克风驱动
#include "esp_log.h"                // ESP日志系统
#include "mock_voices/hi.h"         // 欢迎音频数据文件
#include "mock_voices/ok.h"         // 确认音频数据文件
#include "mock_voices/bye.h"     // 再见音频数据文件
#include "mock_voices/custom.h"     // 自定义音频数据文件
#include "driver/gpio.h"            // GPIO驱动
#include "driver/uart.h"            // UART驱动
#include "esp_wifi.h"               // WiFi驱动
#include "esp_event.h"              // 事件循环
#include "nvs_flash.h"              // NVS存储
#include "esp_netif.h"              // 网络接口
#include "esp_websocket_client.h"   // WebSocket客户端
}

#include "audio_manager.h"          // 音频管理器

static const char *TAG = "语音识别"; // 日志标签

// 外接LED GPIO定义
#define LED_GPIO GPIO_NUM_21 // 外接LED灯珠连接到GPIO21

// WiFi配置
#define WIFI_SSID "1804"
#define WIFI_PASS "Sjn123123@"
#define WIFI_MAXIMUM_RETRY 5

// WebSocket配置
#define WS_URI "ws://192.168.1.163:8888" // 需要替换为实际的服务器IP

// WiFi事件组
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// WebSocket客户端句柄
static esp_websocket_client_handle_t ws_client = NULL;

// 系统状态定义
typedef enum
{
    STATE_WAITING_WAKEUP = 0,   // 等待唤醒词
    STATE_RECORDING = 1,        // 录音中
    STATE_WAITING_RESPONSE = 2, // 等待Python响应
    STATE_WAITING_COMMAND = 3,  // 等待命令词
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

// VAD（语音活动检测）相关变量
static vad_handle_t vad_inst = NULL;

// 音频参数
#define SAMPLE_RATE 16000 // 采样率 16kHz

// 音频管理器
static AudioManager* audio_manager = nullptr;

// VAD（语音活动检测）相关变量
static bool vad_speech_detected = false;
static int vad_silence_frames = 0;
static const int VAD_SILENCE_FRAMES_REQUIRED = 20; // VAD检测到静音的帧数阈值（约600ms）

// 连续对话相关变量
static bool is_continuous_conversation = false;  // 是否处于连续对话模式
static TickType_t recording_timeout_start = 0;  // 录音超时计时开始时间
#define RECORDING_TIMEOUT_MS 10000  // 录音超时时间（10秒）
static bool user_started_speaking = false;  // 标记用户是否已经开始说话

// WiFi重试计数
static int s_retry_num = 0;


/**
 * @brief WiFi事件处理器
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < WIFI_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "重试连接WiFi... (%d/%d)", s_retry_num, WIFI_MAXIMUM_RETRY);
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "WiFi连接失败");
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
 * @brief WebSocket事件处理器
 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "🔗 WebSocket已连接");
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "🔌 WebSocket已断开");
        break;

    case WEBSOCKET_EVENT_DATA:
    {
        ESP_LOGI(TAG, "收到WebSocket数据，长度: %d 字节, op_code: 0x%02x", data->data_len, data->op_code);

        // 使用AudioManager处理WebSocket音频数据
        if (audio_manager != nullptr) {
            bool audio_complete = audio_manager->processWebSocketData(
                data->op_code, 
                (const uint8_t*)data->data_ptr, 
                data->data_len,
                current_state == STATE_WAITING_RESPONSE
            );
            
            // 如果音频处理完成，更新响应播放标志
            if (audio_complete && current_state == STATE_WAITING_RESPONSE) {
                // 音频已在processWebSocketData中播放
            }
        }
        
        // JSON数据处理（用于其他事件）
        if (data->data_ptr && data->data_len > 0 && data->data_ptr[0] == '{') {
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

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "✅ WiFi连接成功: %s", WIFI_SSID);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "❌ WiFi连接失败: %s", WIFI_SSID);
    }
    else
    {
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
    if (audio_manager == nullptr) {
        ESP_LOGW(TAG, "音频管理器未初始化");
        return;
    }
    
    size_t recording_length = 0;
    const int16_t* recording_buffer = audio_manager->getRecordingBuffer(recording_length);
    
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


/**
 * @brief 播放音频的包装函数
 *
 * @param audio_data 音频数据
 * @param data_len 数据长度
 * @param description 音频描述（用于日志）
 * @return esp_err_t 播放结果
 */
static esp_err_t play_audio_with_stop(const uint8_t *audio_data, size_t data_len, const char *description)
{
    if (audio_manager != nullptr) {
        return audio_manager->playAudio(audio_data, data_len, description);
    }
    return ESP_ERR_INVALID_STATE;
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
    play_audio_with_stop(bye, bye_len, "再见音频");

    // 断开WebSocket连接
    websocket_disconnect();

    // 重置所有状态
    current_state = STATE_WAITING_WAKEUP;
    if (audio_manager != nullptr) {
        audio_manager->stopRecording();
        audio_manager->clearRecordingBuffer();
    }
    is_continuous_conversation = false;
    user_started_speaking = false;
    recording_timeout_start = 0;
    vad_speech_detected = false;
    vad_silence_frames = 0;
    
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
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
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

    // ========== 第四步：初始化VAD（语音活动检测）==========
    ESP_LOGI(TAG, "正在初始化语音活动检测（VAD）...");
    
    // 创建VAD实例，使用更精确的参数控制
    // VAD_MODE_1: 中等灵敏度
    // 16000Hz采样率，30ms帧长度，最小语音时长200ms，最小静音时长1000ms
    vad_inst = vad_create_with_param(VAD_MODE_1, SAMPLE_RATE, 30, 200, 1000);
    if (vad_inst == NULL) {
        ESP_LOGE(TAG, "创建VAD实例失败");
        return;
    }
    
    ESP_LOGI(TAG, "✓ VAD初始化成功");
    ESP_LOGI(TAG, "  - VAD模式: 1 (中等灵敏度)");
    ESP_LOGI(TAG, "  - 采样率: %d Hz", SAMPLE_RATE);
    ESP_LOGI(TAG, "  - 帧长度: 30 ms");
    ESP_LOGI(TAG, "  - 最小语音时长: 200 ms");
    ESP_LOGI(TAG, "  - 最小静音时长: 1000 ms");

    // ========== 第五步：初始化语音识别模型 ==========
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

    // 初始化音频管理器
    audio_manager = new AudioManager(SAMPLE_RATE, 10, 32);  // 16kHz, 10秒录音, 32秒响应
    ret = audio_manager->init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "音频管理器初始化失败: %s", esp_err_to_name(ret));
        free(buffer);
        delete audio_manager;
        audio_manager = nullptr;
        return;
    }
    ESP_LOGI(TAG, "✓ 音频管理器初始化成功");

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
                play_audio_with_stop(hi, hi_len, "欢迎音频");

                // 切换到录音状态
                current_state = STATE_RECORDING;
                audio_manager->startRecording();
                vad_speech_detected = false;
                vad_silence_frames = 0;
                is_continuous_conversation = false;  // 第一次录音，不是连续对话
                user_started_speaking = false;
                recording_timeout_start = 0;  // 第一次录音不需要超时
                // 重置VAD触发器状态
                vad_reset_trigger(vad_inst);
                // 重置命令词识别缓冲区
                multinet->clean(mn_model_data);
                ESP_LOGI(TAG, "开始录音，请说话...");
            }
        }
        else if (current_state == STATE_RECORDING)
        {
            // 录音阶段：录制用户说话内容
            if (audio_manager->isRecording() && !audio_manager->isRecordingBufferFull())
            {
                // 将音频数据存入录音缓冲区
                int samples = audio_chunksize / sizeof(int16_t);
                audio_manager->addRecordingData(buffer, samples);
                
                // 如果是连续对话模式，同时进行命令词检测
                if (is_continuous_conversation)
                {
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
                            
                            ESP_LOGI(TAG, "🎯 在录音中检测到命令词: ID=%d, 置信度=%.2f, 内容=%s, 命令='%s'",
                                     command_id, prob, mn_result->string, cmd_desc);
                            
                            // 停止录音
                            audio_manager->stopRecording();
                            
                            // 直接处理命令，不发送到服务器
                            if (command_id == COMMAND_TURN_ON_LIGHT)
                            {
                                ESP_LOGI(TAG, "💡 执行开灯命令");
                                led_turn_on();
                                play_audio_with_stop(ok, ok_len, "开灯确认音频");
                                // 继续保持连续对话模式
                                audio_manager->clearRecordingBuffer();
                                audio_manager->startRecording();
                                vad_speech_detected = false;
                                vad_silence_frames = 0;
                                user_started_speaking = false;
                                recording_timeout_start = xTaskGetTickCount();
                                vad_reset_trigger(vad_inst);
                                multinet->clean(mn_model_data);
                                ESP_LOGI(TAG, "命令执行完成，继续录音...");
                                continue;
                            }
                            else if (command_id == COMMAND_TURN_OFF_LIGHT)
                            {
                                ESP_LOGI(TAG, "💡 执行关灯命令");
                                led_turn_off();
                                play_audio_with_stop(ok, ok_len, "关灯确认音频");
                                // 继续保持连续对话模式
                                audio_manager->clearRecordingBuffer();
                                audio_manager->startRecording();
                                vad_speech_detected = false;
                                vad_silence_frames = 0;
                                user_started_speaking = false;
                                recording_timeout_start = xTaskGetTickCount();
                                vad_reset_trigger(vad_inst);
                                multinet->clean(mn_model_data);
                                ESP_LOGI(TAG, "命令执行完成，继续录音...");
                                continue;
                            }
                            else if (command_id == COMMAND_BYE_BYE)
                            {
                                ESP_LOGI(TAG, "👋 检测到拜拜命令，退出对话");
                                execute_exit_logic();
                                continue;
                            }
                            else if (command_id == COMMAND_CUSTOM)
                            {
                                ESP_LOGI(TAG, "💡 执行自定义命令词");
                                play_audio_with_stop(custom, custom_len, "自定义确认音频");
                                // 继续保持连续对话模式
                                audio_manager->clearRecordingBuffer();
                                audio_manager->startRecording();
                                vad_speech_detected = false;
                                vad_silence_frames = 0;
                                user_started_speaking = false;
                                recording_timeout_start = xTaskGetTickCount();
                                vad_reset_trigger(vad_inst);
                                multinet->clean(mn_model_data);
                                ESP_LOGI(TAG, "命令执行完成，继续录音...");
                                continue;
                            }
                        }
                    }
                }
                
                // 使用VAD检测语音活动（使用30ms帧长度）
                vad_state_t vad_state = vad_process(vad_inst, buffer, SAMPLE_RATE, 30);
                
                // 根据VAD状态处理
                if (vad_state == VAD_SPEECH) {
                    vad_speech_detected = true;
                    vad_silence_frames = 0;
                    user_started_speaking = true;  // 标记用户已经开始说话
                    recording_timeout_start = 0;  // 用户说话后取消超时
                    // 显示录音进度（每100ms显示一次）
                    static TickType_t last_log_time = 0;
                    TickType_t current_time = xTaskGetTickCount();
                    if (current_time - last_log_time > pdMS_TO_TICKS(100)) {
                        ESP_LOGD(TAG, "正在录音... 当前长度: %.2f 秒", audio_manager->getRecordingDuration());
                        last_log_time = current_time;
                    }
                } else if (vad_state == VAD_SILENCE && vad_speech_detected) {
                    // 检测到静音，但必须先检测到过语音
                    vad_silence_frames++;
                    if (vad_silence_frames >= VAD_SILENCE_FRAMES_REQUIRED) {
                        // VAD检测到持续静音，认为用户说完了
                        ESP_LOGI(TAG, "VAD检测到用户说话结束，录音长度: %.2f 秒",
                                 audio_manager->getRecordingDuration());
                        audio_manager->stopRecording();

                        // 只有在用户确实说话了才发送数据
                        size_t rec_len = 0;
                        audio_manager->getRecordingBuffer(rec_len);
                        if (user_started_speaking && rec_len > SAMPLE_RATE / 4) // 至少0.25秒的音频
                        {
                            // 发送录音数据到Python脚本
                            ESP_LOGI(TAG, "正在发送录音数据到电脑...");
                            send_recorded_audio();

                            // 切换到等待响应状态
                            current_state = STATE_WAITING_RESPONSE;
                            audio_manager->resetResponsePlayedFlag(); // 重置播放标志
                            ESP_LOGI(TAG, "等待服务器响应音频...");
                        }
                        else
                        {
                            ESP_LOGI(TAG, "录音时间过短或用户未说话，重新开始录音");
                            // 重新开始录音
                            audio_manager->clearRecordingBuffer();
                            audio_manager->startRecording();
                            vad_speech_detected = false;
                            vad_silence_frames = 0;
                            user_started_speaking = false;
                            if (is_continuous_conversation)
                            {
                                recording_timeout_start = xTaskGetTickCount();
                            }
                            vad_reset_trigger(vad_inst);
                            multinet->clean(mn_model_data);
                        }
                    }
                }
            }
            else if (audio_manager->isRecordingBufferFull())
            {
                // 录音缓冲区满了，强制停止录音
                ESP_LOGW(TAG, "录音缓冲区已满，停止录音");
                audio_manager->stopRecording();

                // 发送录音数据到Python脚本
                ESP_LOGI(TAG, "正在发送录音数据到电脑...");
                send_recorded_audio();

                // 切换到等待响应状态
                current_state = STATE_WAITING_RESPONSE;
                audio_manager->resetResponsePlayedFlag(); // 重置播放标志
                ESP_LOGI(TAG, "等待服务器响应音频...");
            }
            
            // 检查连续对话模式下的超时
            if (is_continuous_conversation && recording_timeout_start > 0 && !user_started_speaking)
            {
                TickType_t current_time = xTaskGetTickCount();
                if ((current_time - recording_timeout_start) > pdMS_TO_TICKS(RECORDING_TIMEOUT_MS))
                {
                    ESP_LOGW(TAG, "⏰ 连续对话录音超时，用户未说话");
                    audio_manager->stopRecording();
                    execute_exit_logic();
                }
                // 每秒提示一次剩余时间
                static TickType_t last_timeout_log = 0;
                if (current_time - last_timeout_log > pdMS_TO_TICKS(1000))
                {
                    int remaining_seconds = (RECORDING_TIMEOUT_MS - (current_time - recording_timeout_start) * portTICK_PERIOD_MS) / 1000;
                    if (remaining_seconds > 0)
                    {
                        ESP_LOGI(TAG, "等待用户说话... 剩余 %d 秒", remaining_seconds);
                    }
                    last_timeout_log = current_time;
                }
            }
        }
        else if (current_state == STATE_WAITING_RESPONSE)
        {
            // 等待响应状态：等待WebSocket响应并播放
            // 响应音频的播放在WebSocket事件处理器中完成

            // 检查是否已经播放完成
            if (audio_manager->isResponsePlayed())
            {
                // 响应已播放完成，重新进入录音状态（连续对话）
                current_state = STATE_RECORDING;
                audio_manager->clearRecordingBuffer();
                audio_manager->startRecording();
                vad_speech_detected = false;
                vad_silence_frames = 0;
                is_continuous_conversation = true;  // 标记为连续对话模式
                user_started_speaking = false;
                recording_timeout_start = xTaskGetTickCount();  // 开始超时计时
                audio_manager->resetResponsePlayedFlag(); // 重置标志
                // 重置VAD触发器状态
                vad_reset_trigger(vad_inst);
                // 重置命令词识别缓冲区
                multinet->clean(mn_model_data);
                ESP_LOGI(TAG, "进入连续对话模式，请继续说话（%d秒内）...", RECORDING_TIMEOUT_MS / 1000);
                ESP_LOGI(TAG, "您可以：1) 继续对话 2) 说出命令词 3) 说'拜拜'退出");
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
                        play_audio_with_stop(ok, ok_len, "开灯确认音频");
                    }
                    else if (command_id == COMMAND_TURN_OFF_LIGHT)
                    {
                        ESP_LOGI(TAG, "💡 执行关灯命令");
                        led_turn_off();

                        // 播放关灯确认音频
                        play_audio_with_stop(ok, ok_len, "关灯确认音频");
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

    // 销毁VAD实例
    if (vad_inst != NULL)
    {
        vad_destroy(vad_inst);
    }

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

    // 释放音频管理器
    if (audio_manager != nullptr)
    {
        delete audio_manager;
        audio_manager = nullptr;
    }

    // 删除当前任务
    vTaskDelete(NULL);
}
