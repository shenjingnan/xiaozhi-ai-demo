/**
 * @file main.cc
 * @brief ESP32-S3 INMP441麦克风唤醒词检测主程序
 *
 * 本程序实现了基于ESP32-S3开发板和INMP441数字麦克风的唤醒词检测功能。
 * 支持通过idf.py menuconfig配置的各种唤醒词模型。
 * 程序会自动读取sdkconfig中配置的唤醒词模型，无需手动修改代码。
 *
 * 硬件配置：
 * - ESP32-S3-DevKitC-1开发板
 * - INMP441数字麦克风
 * - 连接方式：VDD->3.3V, GND->GND, SD->GPIO6, WS->GPIO4, SCK->GPIO5
 *
 * 音频参数：
 * - 采样率：16kHz
 * - 声道：单声道(Mono)
 * - 位深度：16位
 */

extern "C"
{
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wn_iface.h"        // 唤醒词检测接口
#include "esp_wn_models.h"       // 唤醒词模型管理
#include "esp_mn_iface.h"        // 命令词识别接口
#include "esp_mn_models.h"       // 命令词模型管理
#include "esp_mn_speech_commands.h" // 命令词配置
#include "model_path.h"          // 模型路径定义
#include "bsp_board.h"           // 板级支持包，INMP441麦克风驱动
#include "esp_log.h"             // ESP日志系统
#include "mock_voices/welcome.h" // 欢迎音频数据文件
#include "mock_voices/light_on.h"  // 开灯音频数据文件
#include "mock_voices/light_off.h" // 关灯音频数据文件
#include "mock_voices/byebye.h"    // 再见音频数据文件
#include "driver/gpio.h"         // GPIO驱动
}

static const char *TAG = "语音识别"; // 日志标签

// 外接LED GPIO定义
#define LED_GPIO GPIO_NUM_21 // 外接LED灯珠连接到GPIO21

// 系统状态定义
typedef enum {
    STATE_WAITING_WAKEUP = 0,    // 等待唤醒词
    STATE_WAITING_COMMAND = 1,   // 等待命令词
} system_state_t;

// 命令词ID定义（对应commands_cn.txt中的ID）
#define COMMAND_TURN_OFF_LIGHT 308  // "帮我关灯"
#define COMMAND_TURN_ON_LIGHT  309  // "帮我开灯"

// 全局变量
static system_state_t current_state = STATE_WAITING_WAKEUP;
static esp_mn_iface_t *multinet = NULL;
static model_iface_data_t *mn_model_data = NULL;
static TickType_t command_timeout_start = 0;
static const TickType_t COMMAND_TIMEOUT_MS = 5000; // 5秒超时

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
 * @brief 应用程序主入口函数
 *
 * 初始化INMP441麦克风硬件，加载唤醒词检测模型，
 * 然后进入主循环进行实时音频采集和唤醒词检测。
 */
extern "C" void app_main(void)
{
    // ========== 第一步：初始化外接LED ==========
    init_led();

    // ========== 第二步：初始化INMP441麦克风硬件 ==========
    ESP_LOGI(TAG, "正在初始化INMP441数字麦克风...");
    ESP_LOGI(TAG, "音频参数: 采样率16kHz, 单声道, 16位深度");

    esp_err_t ret = bsp_board_init(16000, 1, 16); // 16kHz, 单声道, 16位
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "INMP441麦克风初始化失败: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "请检查硬件连接: VDD->3.3V, GND->GND, SD->GPIO6, WS->GPIO4, SCK->GPIO5");
        return;
    }
    ESP_LOGI(TAG, "✓ INMP441麦克风初始化成功");

    // ========== 第二步：初始化音频播放功能 ==========
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

    // ========== 第三步：初始化语音识别模型 ==========
    ESP_LOGI(TAG, "正在初始化唤醒词检测模型...");

    // 检查内存状态
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "内存状态检查:");
    ESP_LOGI(TAG, "  - 总可用内存: %zu KB", free_heap / 1024);
    ESP_LOGI(TAG, "  - 内部RAM: %zu KB", free_internal / 1024);
    ESP_LOGI(TAG, "  - PSRAM: %zu KB", free_spiram / 1024);

    if (free_heap < 100 * 1024) {
        ESP_LOGE(TAG, "可用内存不足，需要至少100KB");
        return;
    }

    // 从模型目录加载所有可用的语音识别模型
    ESP_LOGI(TAG, "开始加载模型文件...");

    // 临时添加错误处理和重试机制
    srmodel_list_t *models = NULL;
    int retry_count = 0;
    const int max_retries = 3;

    while (models == NULL && retry_count < max_retries) {
        ESP_LOGI(TAG, "尝试加载模型 (第%d次)...", retry_count + 1);

        // 在每次重试前等待一下
        if (retry_count > 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        models = esp_srmodel_init("model");

        if (models == NULL) {
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

    // ========== 第四步：初始化命令词识别模型 ==========
    ESP_LOGI(TAG, "正在初始化命令词识别模型...");

    // 获取中文命令词识别模型
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (mn_name == NULL)
    {
        ESP_LOGE(TAG, "未找到中文命令词识别模型！");
        ESP_LOGE(TAG, "请确保已正确配置并烧录mn6_cn模型");
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

    // 尝试从sdkconfig加载命令词配置
    ESP_LOGI(TAG, "正在配置命令词...");

    // 尝试使用sdkconfig配置（如果函数存在）
    // 注意：这个函数在某些ESP-IDF版本中可能不存在
    // esp_mn_commands_update_from_sdkconfig(multinet, mn_model_data);

    // 打印激活的命令词（这些命令词应该已经在sdkconfig中配置）
    ESP_LOGI(TAG, "激活的命令词列表:");
    multinet->print_active_speech_commands(mn_model_data);

    ESP_LOGI(TAG, "✓ 命令词配置完成");
    ESP_LOGI(TAG, "支持的命令: ID=%d('帮我开灯'), ID=%d('帮我关灯')",
             COMMAND_TURN_ON_LIGHT, COMMAND_TURN_OFF_LIGHT);

    // ========== 第四步：准备音频缓冲区 ==========
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
    ESP_LOGI(TAG, "✓ 系统配置完成:");
    ESP_LOGI(TAG, "  - 唤醒词模型: %s", model_name);
    ESP_LOGI(TAG, "  - 音频块大小: %d 字节", audio_chunksize);
    ESP_LOGI(TAG, "  - 检测置信度: 90%%");
    ESP_LOGI(TAG, "正在启动麦克风唤醒词检测...");
    ESP_LOGI(TAG, "请对着麦克风说出配置的唤醒词");

    // ========== 第五步：主循环 - 实时音频采集与语音识别 ==========
    ESP_LOGI(TAG, "系统启动完成，等待唤醒词 '你好小智'...");

    while (1)
    {
        // 从INMP441麦克风获取一帧音频数据
        // false参数表示获取处理后的音频数据（非原始通道数据）
        esp_err_t ret = bsp_get_feed_data(false, buffer, audio_chunksize);
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
                ESP_LOGI(TAG, "进入命令词识别模式，请说出指令...");
                ESP_LOGI(TAG, "支持的指令: '帮我开灯' 或 '帮我关灯'");
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

                    ESP_LOGI(TAG, "🎯 检测到命令词: ID=%d, 置信度=%.2f, 内容=%s",
                             command_id, prob, mn_result->string);

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
                    else
                    {
                        ESP_LOGW(TAG, "⚠️  未知命令ID: %d", command_id);
                    }
                }

                // 命令处理完成，重新开始5秒倒计时，继续等待下一个命令
                command_timeout_start = xTaskGetTickCount();
                multinet->clean(mn_model_data); // 清理命令词识别缓冲区
                ESP_LOGI(TAG, "命令执行完成，重新开始5秒倒计时");
                ESP_LOGI(TAG, "可以继续说出指令: '帮我开灯' 或 '帮我关灯'");
            }
            else if (mn_state == ESP_MN_STATE_TIMEOUT)
            {
                ESP_LOGW(TAG, "⏰ 命令词识别超时");

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
            else
            {
                // 检查手动超时
                TickType_t current_time = xTaskGetTickCount();
                if ((current_time - command_timeout_start) > pdMS_TO_TICKS(COMMAND_TIMEOUT_MS))
                {
                    ESP_LOGW(TAG, "⏰ 命令词等待超时 (%lu秒)", (unsigned long)(COMMAND_TIMEOUT_MS / 1000));

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

    // 删除当前任务
    vTaskDelete(NULL);
}
