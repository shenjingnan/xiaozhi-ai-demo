/**
 * @file main.cc
 * @brief ESP32-S3 智能语音助手 - 支持大语言模型对话的语音助手主程序
 *
 * 🎯 功能特点：
 * 1. 语音唤醒 - 支持"你好小智"唤醒词，随时待命
 * 2. 连续对话 - 无需重复唤醒词，可以进行多轮对话
 * 3. AI对话 - 接入大语言模型，理解自然语言并生成智能回复
 * 4. 本地控制 - 内置"开灯"、"关灯"等命令词，快速响应
 * 5. 实时传输 - 一边说话一边传输，降低响应延迟
 *
 * 🔧 硬件配置（小智AI标准接线）：
 * ┌─────────────────────────────────────────────────┐
 * │ ESP32-S3 开发板 + 外围硬件                       │
 * ├─────────────────────────────────────────────────┤
 * │ • INMP441 数字麦克风（拾音）                     │
 * │   └─ VDD→3.3V, GND→GND, SD→GPIO6              │
 * │   └─ WS→GPIO4, SCK→GPIO5                      │
 * │ • MAX98357A 数字功放（播音）                     │  
 * │   └─ DIN→GPIO7, BCLK→GPIO15, LRC→GPIO16       │
 * │   └─ VIN→3.3V, GND→GND                        │
 * │ • LED 指示灯                                    │
 * │   └─ 正极→GPIO21, 负极→GND（需要限流电阻）      │
 * └─────────────────────────────────────────────────┘
 *
 * 📊 音频参数（语音识别标准配置）：
 * - 采样率：16kHz（人声频率范围）
 * - 声道数：1（单声道，节省内存）
 * - 位深度：16位（CD音质）
 *
 * 🤖 AI模型说明：
 * - 唤醒词检测：WakeNet（"你好小智"）- 本地运行，低功耗
 * - 命令词识别：MultiNet（中文命令）- 本地运行，快速响应
 * - 对话理解：通过WebSocket连接云端大语言模型
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
#include "esp_nsn_iface.h"          // 噪音抑制接口
#include "esp_nsn_models.h"         // 噪音抑制模型
#include "model_path.h"             // 模型路径定义
#include "bsp_board.h"              // 板级支持包，INMP441麦克风驱动
#include "esp_log.h"                // ESP日志系统
#include "mock_voices/hi.h"         // 欢迎音频数据文件
#include "mock_voices/ok.h"         // 确认音频数据文件
#include "mock_voices/bye.h"     // 再见音频数据文件
#include "mock_voices/custom.h"     // 自定义音频数据文件
#include "driver/gpio.h"            // GPIO驱动
#include "nvs_flash.h"              // NVS存储
}

#include "audio_manager.h"          // 音频管理器
#include "wifi_manager.h"           // WiFi管理器
#include "websocket_client.h"        // WebSocket客户端

static const char *TAG = "语音识别"; // 日志标签

// 🔌 硬件引脚定义
#define LED_GPIO GPIO_NUM_21 // LED指示灯连接到GPIO21（记得加限流电阻哦）

// 📡 网络配置（请根据您的实际情况修改）
#define WIFI_SSID "<你的WIFI名称>"                 // 您的WiFi名称
#define WIFI_PASS "<你的WIFI密码>"           // 您的WiFi密码

// 🌐 WebSocket服务器配置
#define WS_URI "ws://<你的电脑IP地址>:8888" // 请改为您的电脑IP地址:8888

// WiFi和WebSocket管理器
static WiFiManager* wifi_manager = nullptr;
static WebSocketClient* websocket_client = nullptr;

// 🎮 系统状态机（程序的不同工作阶段）
typedef enum
{
    STATE_WAITING_WAKEUP = 0,   // 休眠状态：等待用户说"你好小智"
    STATE_RECORDING = 1,        // 录音状态：正在录制用户说话
    STATE_WAITING_RESPONSE = 2, // 等待状态：等待服务器返回AI响应
} system_state_t;

// 🎤 本地命令词ID（快速响应，无需联网）
// 这些ID来自ESP-SR语音识别框架的预定义命令词表
#define COMMAND_TURN_OFF_LIGHT 308 // "帮我关灯" - 关闭LED
#define COMMAND_TURN_ON_LIGHT 309  // "帮我开灯" - 点亮LED
#define COMMAND_BYE_BYE 314        // "拜拜" - 退出对话
#define COMMAND_CUSTOM 315         // "现在安全屋情况如何" - 演示用

// 📝 命令词配置结构（告诉系统要识别哪些命令）
typedef struct
{
    int command_id;              // 命令的唯一标识符
    const char *pinyin;          // 命令的拼音（用于语音识别匹配）
    const char *description;     // 命令的中文描述（方便理解）
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

// NS（噪音抑制）相关变量  
static esp_nsn_iface_t *nsn_handle = NULL;
static esp_nsn_data_t *nsn_model_data = NULL;

// 音频参数
#define SAMPLE_RATE 16000 // 采样率 16kHz

// 音频管理器
static AudioManager* audio_manager = nullptr;

// VAD（语音活动检测）相关变量
static bool vad_speech_detected = false;
static int vad_silence_frames = 0;
static const int VAD_SILENCE_FRAMES_REQUIRED = 20; // VAD检测到静音的帧数阈值（约600ms）

// 💬 连续对话功能相关变量
// 连续对话模式：第一次对话后，不需要再说唤醒词就能继续对话
static bool is_continuous_conversation = false;  // 是否在连续对话中
static TickType_t recording_timeout_start = 0;  // 开始计时的时间点
#define RECORDING_TIMEOUT_MS 10000              // 等待说话超时（10秒没说话就退出）
static bool user_started_speaking = false;      // 用户是否已经开始说话

/**
 * @brief WebSocket事件处理函数
 * 
 * 当WebSocket连接发生各种事件时（连接成功、收到数据、断开等），
 * 这个函数会被自动调用来处理这些事件。
 * 
 * @param event 事件数据，包含事件类型和相关数据
 */
static void on_websocket_event(const WebSocketClient::EventData& event)
{
    switch (event.type)
    {
    case WebSocketClient::EventType::CONNECTED:
        ESP_LOGI(TAG, "🔗 WebSocket已连接");
        break;

    case WebSocketClient::EventType::DISCONNECTED:
        ESP_LOGI(TAG, "🔌 WebSocket已断开");
        break;

    case WebSocketClient::EventType::DATA_BINARY:
    {
        ESP_LOGI(TAG, "收到WebSocket二进制数据，长度: %zu 字节", event.data_len);

        // 使用AudioManager处理WebSocket音频数据
        if (audio_manager != nullptr && event.data_len > 0 && current_state == STATE_WAITING_RESPONSE) {
            // 如果还没开始流式播放，初始化
            if (!audio_manager->isStreamingActive()) {
                ESP_LOGI(TAG, "🎵 开始流式音频播放");
                audio_manager->startStreamingPlayback();
            }
            
            // 添加音频数据到流式播放队列
            bool added = audio_manager->addStreamingAudioChunk(event.data, event.data_len);
            
            if (added) {
                ESP_LOGD(TAG, "添加流式音频块: %zu 字节", event.data_len);
            } else {
                ESP_LOGW(TAG, "流式音频缓冲区满");
            }
        }
    }
    break;
    
    case WebSocketClient::EventType::PING:
        // 检测ping包作为流结束标志
        if (audio_manager != nullptr && audio_manager->isStreamingActive()) {
            ESP_LOGI(TAG, "收到ping包，结束流式播放");
            audio_manager->finishStreamingPlayback();
            // 标记响应已播放
            if (current_state == STATE_WAITING_RESPONSE) {
                audio_manager->setStreamingComplete();
            }
        }
        break;

    case WebSocketClient::EventType::DATA_TEXT:
        // JSON数据处理（用于其他事件）
        if (event.data && event.data_len > 0) {
            // 创建临时缓冲区
            char *json_str = (char *)malloc(event.data_len + 1);
            if (json_str) {
                memcpy(json_str, event.data, event.data_len);
                json_str[event.data_len] = '\0';
                ESP_LOGI(TAG, "收到JSON消息: %s", json_str);
                free(json_str);
            }
        }
        break;

    case WebSocketClient::EventType::ERROR:
        ESP_LOGI(TAG, "❌ WebSocket错误");
        break;
        
    default:
        break;
    }
}

/**
 * @brief 初始化LED指示灯
 *
 * 💡 这个函数会把GPIO21设置为输出模式，用来控制LED灯的亮灭。
 * 初始状态为关闭（低电平）。
 * 
 * 注意：LED需要串联一个限流电阻（如220Ω）以保护LED和GPIO。
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

// 实时流式传输标志
static bool is_realtime_streaming = false;

/**
 * @brief 配置本地命令词识别
 *
 * 🎆 这个函数会告诉语音识别系统要识别哪些中文命令。
 * 这些命令在本地运行，不需要联网，响应速度快。
 * 
 * 工作流程：
 * 1. 清空旧的命令词列表
 * 2. 添加我们定义的新命令词（如“帮我开灯”）
 * 3. 更新到识别模型中
 *
 * @param multinet 语音识别的接口对象
 * @param mn_model_data 识别模型的数据
 * @return ESP_OK=成功，ESP_FAIL=失败
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
 * @brief 根据命令ID获取中文说明
 *
 * 🔍 这是一个工具函数，用来查找命令ID对应的中文说明。
 * 比如：309 -> "帮我开灯"
 * 
 * @param command_id 命令的数字ID
 * @return 命令的中文说明文字
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
 * @brief 播放音频文件
 *
 * 🔊 这个函数会通过扬声器播放指定的音频数据。
 * 使用AudioManager管理音频播放，确保不会与其他音频冲突。
 * 
 * @param audio_data 要播放的音频数据（PCM格式）
 * @param data_len 音频数据的字节数
 * @param description 音频的描述（如"欢迎音频"）
 * @return ESP_OK=播放成功
 */
static esp_err_t play_audio_with_stop(const uint8_t *audio_data, size_t data_len, const char *description)
{
    if (audio_manager != nullptr) {
        return audio_manager->playAudio(audio_data, data_len, description);
    }
    return ESP_ERR_INVALID_STATE;
}

/**
 * @brief 退出对话模式
 *
 * 👋 当用户说“拜拜”或对话超时后，调用这个函数结束对话。
 * 
 * 执行步骤：
 * 1. 播放“再见”的音频
 * 2. 断开WebSocket连接
 * 3. 清理所有状态
 * 4. 回到等待唤醒词的初始状态
 */
static void execute_exit_logic(void)
{
    // 播放再见音频
    ESP_LOGI(TAG, "播放再见音频...");
    play_audio_with_stop(bye, bye_len, "再见音频");

    // 断开WebSocket连接
    if (websocket_client != nullptr) {
        websocket_client->disconnect();
    }

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
 * @brief 🉰️ 程序主入口（这里是一切的开始）
 *
 * ESP32启动后会自动调用这个函数。
 * 
 * 主要工作流程：
 * 1. 初始化各种硬件（LED、麦克风、扬声器）
 * 2. 连接WiFi和WebSocket服务器
 * 3. 加载语音识别模型
 * 4. 进入主循环，开始监听用户说话
 */
extern "C" void app_main(void)
{
    // ① 初始化NVS（非易失性存储）
    // NVS用于保存WiFi配置等信息，即使断电也不会丢失
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // 如果NVS区域满了或版本不匹配，就清空重来
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ② 初始化LED灯（用于状态指示）
    init_led();

    // ③ 连接WiFi网络
    ESP_LOGI(TAG, "正在连接WiFi...");
    wifi_manager = new WiFiManager(WIFI_SSID, WIFI_PASS);
    if (wifi_manager->connect() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi连接失败");
        ESP_LOGE(TAG, "请检查：1) WiFi名称和密码是否正确 2) 路由器是否开启");
        delete wifi_manager;
        return;
    }
    
    // ④ 连接WebSocket服务器（用于与电脑通信）
    ESP_LOGI(TAG, "正在连接WebSocket服务器...");
    websocket_client = new WebSocketClient(WS_URI, true, 5000);
    websocket_client->setEventCallback(on_websocket_event);  // 设置事件处理函数
    if (websocket_client->connect() != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket连接失败");
        ESP_LOGE(TAG, "请检查：1) 电脑上的server.py是否在运行 2) IP地址是否正确");
        delete websocket_client;
        delete wifi_manager;
        return;
    }

    // ⑤ 初始化麦克风（INMP441数字麦克风）
    ESP_LOGI(TAG, "正在初始化INMP441数字麦克风...");
    ESP_LOGI(TAG, "音频参数: 16kHz采样率, 单声道, 16位");

    ret = bsp_board_init(16000, 1, 16); // 16kHz, 单声道, 16位
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "INMP441麦克风初始化失败: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "请检查硬件连接: VDD->3.3V, GND->GND, SD->GPIO6, WS->GPIO4, SCK->GPIO5");
        return;
    }
    ESP_LOGI(TAG, "✓ INMP441麦克风初始化成功");

    // ⑥ 初始化扬声器（MAX98357A数字功放）
    ESP_LOGI(TAG, "正在初始化音频播放功能...");
    ESP_LOGI(TAG, "音频播放参数: 16kHz采样率, 单声道, 16位");

    ret = bsp_audio_init(16000, 1, 16); // 16kHz, 单声道, 16位
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "音频播放初始化失败: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "请检查MAX98357A硬件连接: DIN->GPIO7, BCLK->GPIO15, LRC->GPIO16");
        return;
    }
    ESP_LOGI(TAG, "✓ 音频播放初始化成功");

    // ⑦ 初始化VAD（语音活动检测）
    // VAD用于检测用户什么时候开始说话、什么时候停止
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

    // ⑧ 加载唤醒词检测模型（识别“你好小智”）
    ESP_LOGI(TAG, "正在加载唤醒词检测模型...");

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

    // ⑨ 加载命令词识别模型（识别“开灯”、“关灯”等）
    ESP_LOGI(TAG, "正在加载命令词识别模型...");

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

    // ⑩ 初始化噪音抑制（可选，提高噪音环境下的识别率）
    ESP_LOGI(TAG, "正在初始化噪音抑制模块...");
    
    // 获取噪音抑制模型
    char *nsn_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    if (nsn_model_name == NULL) {
        ESP_LOGW(TAG, "未找到噪音抑制模型，将不使用噪音抑制");
    } else {
        ESP_LOGI(TAG, "✓ 选择噪音抑制模型: %s", nsn_model_name);
        
        // 获取噪音抑制接口
        nsn_handle = (esp_nsn_iface_t *)esp_nsnet_handle_from_name(nsn_model_name);
        if (nsn_handle == NULL) {
            ESP_LOGW(TAG, "获取噪音抑制接口失败");
        } else {
            // 创建噪音抑制实例
            nsn_model_data = nsn_handle->create(nsn_model_name);
            if (nsn_model_data == NULL) {
                ESP_LOGW(TAG, "创建噪音抑制实例失败");
            } else {
                ESP_LOGI(TAG, "✓ 噪音抑制初始化成功");
                ESP_LOGI(TAG, "  - 噪音抑制模型: %s", nsn_model_name);
                ESP_LOGI(TAG, "  - 采样率: %d Hz", SAMPLE_RATE);
            }
        }
    }

    // ⑪ 准备音频缓冲区
    // 获取语音识别模型需要的数据块大小
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

    ESP_LOGI(TAG, "✓ 使用WebSocket进行通信");

    // 显示系统配置信息
    ESP_LOGI(TAG, "✓ 智能语音助手系统配置完成:");
    ESP_LOGI(TAG, "  - 唤醒词模型: %s", model_name);
    ESP_LOGI(TAG, "  - 命令词模型: %s", mn_name);
    ESP_LOGI(TAG, "  - 音频块大小: %d 字节", audio_chunksize);
    ESP_LOGI(TAG, "  - 噪音抑制: %s", (nsn_model_data != NULL) ? "已启用" : "未启用");
    ESP_LOGI(TAG, "  - 检测置信度: 90%%");
    ESP_LOGI(TAG, "正在启动智能语音助手...");
    ESP_LOGI(TAG, "请对着麦克风说出唤醒词 '你好小智'");

    // 🎮 主循环 - 开始实时语音识别
    ESP_LOGI(TAG, "系统启动完成，请对着麦克风说 '你好小智'...");

    while (1)
    {
        // 🎧 从麦克风读取一小段音频数据
        // 这里的false表示要处理后的数据（不要原始数据）
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

        // 如果启用了噪音抑制，先对音频数据进行噪音抑制处理
        int16_t *processed_audio = buffer;
        static int16_t *ns_out_buffer = NULL;  // 噪音抑制输出缓冲区
        if (nsn_handle != NULL && nsn_model_data != NULL) {
            // 如果输出缓冲区未分配，分配它
            if (ns_out_buffer == NULL) {
                int ns_chunksize = nsn_handle->get_samp_chunksize(nsn_model_data);
                ns_out_buffer = (int16_t *)malloc(ns_chunksize * sizeof(int16_t));
                if (ns_out_buffer == NULL) {
                    ESP_LOGW(TAG, "噪音抑制输出缓冲区分配失败");
                    nsn_handle = NULL;  // 禁用噪音抑制
                }
            }
            
            if (ns_out_buffer != NULL) {
                // 执行噪音抑制
                nsn_handle->process(nsn_model_data, buffer, ns_out_buffer);
                processed_audio = ns_out_buffer;  // 使用噪音抑制后的数据
            }
        }

        if (current_state == STATE_WAITING_WAKEUP)
        {
            // 🛌 休眠状态：监听唤醒词“你好小智”
            wakenet_state_t wn_state = wakenet->detect(model_data, processed_audio);

            if (wn_state == WAKENET_DETECTED)
            {
                ESP_LOGI(TAG, "🎉 检测到唤醒词 '你好小智'！");
                printf("=== 唤醒词检测成功！模型: %s ===\n", model_name);

                // 检查WebSocket连接状态
                if (websocket_client != nullptr && !websocket_client->isConnected())
                {
                    ESP_LOGI(TAG, "WebSocket未连接，正在重连...");
                    websocket_client->connect();
                    vTaskDelay(pdMS_TO_TICKS(500));  // 等待500ms
                }

                // 通知服务器：唤醒成功
                if (websocket_client != nullptr && websocket_client->isConnected())
                {
                    // 构造JSON消息
                    char wake_msg[256];
                    snprintf(wake_msg, sizeof(wake_msg),
                             "{\"event\":\"wake_word_detected\",\"model\":\"%s\",\"timestamp\":%lld}",
                             model_name,
                             (long long)esp_timer_get_time() / 1000);
                    websocket_client->sendText(wake_msg);
                }

                // 🎵 播放“叮咚”提示音，表示准备好了
                ESP_LOGI(TAG, "播放欢迎音频...");
                play_audio_with_stop(hi, hi_len, "欢迎音频");

                // 发送开始录音事件
                if (websocket_client != nullptr && websocket_client->isConnected())
                {
                    const char* start_msg = "{\"event\":\"recording_started\"}";
                    websocket_client->sendText(start_msg);
                    ESP_LOGI(TAG, "发送录音开始事件");
                }

                // 🎙️ 进入录音状态
                current_state = STATE_RECORDING;
                audio_manager->startRecording();
                
                // 初始化各种状态变量
                vad_speech_detected = false;        // 还没检测到说话
                vad_silence_frames = 0;             // 静音帧计数器清零
                is_continuous_conversation = false;  // 第一次对话，不是连续模式
                user_started_speaking = false;      // 用户还没开始说话
                recording_timeout_start = 0;        // 第一次不设超时
                is_realtime_streaming = false;      // 等用户说话后再开始传输
                
                // 重置各种检测器
                vad_reset_trigger(vad_inst);        // 重置VAD
                multinet->clean(mn_model_data);     // 清空命令词缓冲区
                
                ESP_LOGI(TAG, "开始录音，请说话...");
            }
        }
        else if (current_state == STATE_RECORDING)
        {
            // 🎙️ 录音状态：记录用户说的话
            if (audio_manager->isRecording() && !audio_manager->isRecordingBufferFull())
            {
                // 将音频数据存入录音缓冲区
                int samples = audio_chunksize / sizeof(int16_t);
                audio_manager->addRecordingData(processed_audio, samples);
                
                // 📤 实时传输音频到服务器（边说边传，降低延迟）
                if (is_realtime_streaming && websocket_client != nullptr && websocket_client->isConnected())
                {
                    // 立即发送当前这段音频
                    size_t bytes_to_send = samples * sizeof(int16_t);
                    websocket_client->sendBinary((const uint8_t*)processed_audio, bytes_to_send);
                    ESP_LOGD(TAG, "实时发送: %zu 字节", bytes_to_send);
                }
                
                // 🎯 连续对话模式下，同时检测本地命令词（如“开灯”、“拜拜”）
                if (is_continuous_conversation)
                {
                    esp_mn_state_t mn_state = multinet->detect(mn_model_data, processed_audio);
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
                                is_realtime_streaming = false;  // 等待用户开始说话才开启流式传输
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
                                is_realtime_streaming = false;  // 等待用户开始说话才开启流式传输
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
                                is_realtime_streaming = false;  // 等待用户开始说话才开启流式传输
                                vad_reset_trigger(vad_inst);
                                multinet->clean(mn_model_data);
                                ESP_LOGI(TAG, "命令执行完成，继续录音...");
                                continue;
                            }
                        }
                    }
                }
                
                // 👂 使用VAD检测用户是否在说话
                // VAD会分析音频，判断是语音还是静音
                vad_state_t vad_state = vad_process(vad_inst, processed_audio, SAMPLE_RATE, 30);
                
                // 如果VAD检测到有人说话
                if (vad_state == VAD_SPEECH) {
                    vad_speech_detected = true;
                    vad_silence_frames = 0;
                    user_started_speaking = true;  // 标记用户已经开始说话
                    recording_timeout_start = 0;  // 用户说话后取消超时
                    
                    // 🚀 用户开始说话了，启动实时传输
                    if (!is_realtime_streaming) {
                        is_realtime_streaming = true;
                        if (is_continuous_conversation) {
                            ESP_LOGI(TAG, "连续对话：检测到说话，开始实时传输...");
                        } else {
                            ESP_LOGI(TAG, "首次对话：检测到说话，开始实时传输...");
                        }
                    }
                    
                    // 显示录音进度（每100ms显示一次）
                    static TickType_t last_log_time = 0;
                    TickType_t current_time = xTaskGetTickCount();
                    if (current_time - last_log_time > pdMS_TO_TICKS(100)) {
                        ESP_LOGD(TAG, "正在录音... 当前长度: %.2f 秒", audio_manager->getRecordingDuration());
                        last_log_time = current_time;
                    }
                } else if (vad_state == VAD_SILENCE && vad_speech_detected) {
                    // 🤐 检测到静音（但之前已经有说话）
                    vad_silence_frames++;
                    
                    // 如果静音超过600ms，认为用户说完了
                    if (vad_silence_frames >= VAD_SILENCE_FRAMES_REQUIRED) {
                        ESP_LOGI(TAG, "VAD检测到用户说话结束，录音长度: %.2f 秒",
                                 audio_manager->getRecordingDuration());
                        audio_manager->stopRecording();
                        is_realtime_streaming = false;  // 停止实时流式传输

                        // 只有在用户确实说话了才发送数据
                        size_t rec_len = 0;
                        audio_manager->getRecordingBuffer(rec_len);
                        if (user_started_speaking && rec_len > SAMPLE_RATE / 4) // 至少0.25秒的音频
                        {
                            // 发送录音结束事件
                            if (websocket_client != nullptr && websocket_client->isConnected())
                            {
                                const char* end_msg = "{\"event\":\"recording_ended\"}";
                                websocket_client->sendText(end_msg);
                                ESP_LOGI(TAG, "发送录音结束事件");
                            }
                            
                            // 切换到等待响应状态
                            current_state = STATE_WAITING_RESPONSE;
                            audio_manager->resetResponsePlayedFlag(); // 重置播放标志
                            ESP_LOGI(TAG, "等待服务器响应音频...");
                        }
                        else
                        {
                            ESP_LOGI(TAG, "录音时间过短或用户未说话，重新开始录音");
                            // 发送录音取消事件
                            if (websocket_client != nullptr && websocket_client->isConnected())
                            {
                                const char* cancel_msg = "{\"event\":\"recording_cancelled\"}";
                                websocket_client->sendText(cancel_msg);
                            }
                            // 重新开始录音
                            audio_manager->clearRecordingBuffer();
                            audio_manager->startRecording();
                            vad_speech_detected = false;
                            vad_silence_frames = 0;
                            user_started_speaking = false;
                            is_realtime_streaming = !is_continuous_conversation;  // 只在非连续对话模式下开启流式传输
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
                // ⚠️ 录音时间太长，缓冲区满了（10秒上限）
                ESP_LOGW(TAG, "录音缓冲区已满，停止录音");
                audio_manager->stopRecording();
                is_realtime_streaming = false;  // 停止实时流式传输

                // 发送录音结束事件
                if (websocket_client != nullptr && websocket_client->isConnected())
                {
                    const char* end_msg = "{\"event\":\"recording_ended\"}";
                    websocket_client->sendText(end_msg);
                    ESP_LOGI(TAG, "发送录音结束事件（缓冲区满）");
                }

                // 切换到等待响应状态
                current_state = STATE_WAITING_RESPONSE;
                audio_manager->resetResponsePlayedFlag(); // 重置播放标志
                ESP_LOGI(TAG, "等待服务器响应音频...");
            }
            
            // ⏱️ 连续对话模式下，检查是否超时没说话
            if (is_continuous_conversation && recording_timeout_start > 0 && !user_started_speaking)
            {
                TickType_t current_time = xTaskGetTickCount();
                if ((current_time - recording_timeout_start) > pdMS_TO_TICKS(RECORDING_TIMEOUT_MS))
                {
                    ESP_LOGW(TAG, "⏰ 超过10秒没说话，退出对话");
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
            // ⏳ 等待状态：等待服务器的AI回复
            
            // 音频会通过WebSocket流式接收并播放
            // 这里只需要检查播放是否完成
            if (audio_manager->isResponsePlayed())
            {
                // 🔁 AI回复完毕，进入连续对话模式
                // 通知服务器准备接收下一轮对话
                if (websocket_client != nullptr && websocket_client->isConnected())
                {
                    const char* start_msg = "{\"event\":\"recording_started\"}";
                    websocket_client->sendText(start_msg);
                }
                
                current_state = STATE_RECORDING;
                audio_manager->clearRecordingBuffer();
                audio_manager->startRecording();
                vad_speech_detected = false;
                vad_silence_frames = 0;
                is_continuous_conversation = true;  // 标记为连续对话模式
                user_started_speaking = false;
                recording_timeout_start = xTaskGetTickCount();  // 开始超时计时
                is_realtime_streaming = false;  // 在连续对话模式下，等待用户开始说话才开启流式传输
                audio_manager->resetResponsePlayedFlag(); // 重置标志
                // 重置VAD触发器状态
                vad_reset_trigger(vad_inst);
                // 重置命令词识别缓冲区
                multinet->clean(mn_model_data);
                ESP_LOGI(TAG, "进入连续对话模式，请在%d秒内继续说话...", RECORDING_TIMEOUT_MS / 1000);
                ESP_LOGI(TAG, "💡 提示：1) 可以继续提问 2) 说“帮我开/关灯” 3) 说“拜拜”结束");
            }
        }

        // 短暂延时，避免CPU占用过高，同时保证实时性
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // 🧹 资源清理（正常情况下不会执行到这里）
    // 因为上面是无限循环，只有出错时才会走到这里
    ESP_LOGI(TAG, "正在清理系统资源...");

    // 销毁噪音抑制实例
    if (nsn_model_data != NULL && nsn_handle != NULL)
    {
        nsn_handle->destroy(nsn_model_data);
    }

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

    // 清理WebSocket客户端
    if (websocket_client != nullptr)
    {
        delete websocket_client;
        websocket_client = nullptr;
    }

    // 清理WiFi管理器
    if (wifi_manager != nullptr)
    {
        delete wifi_manager;
        wifi_manager = nullptr;
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
