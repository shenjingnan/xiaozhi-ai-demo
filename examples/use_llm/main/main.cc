/**
 * @file main.cc
 * @brief ESP32-S3 智能语音助手 - 主程序
 *
 * 本程序实现了智能语音助手功能，包括：
 * 1. 语音唤醒检测 - 支持"你好小智"唤醒词
 * 2. VAD语音活动检测 - 使用VADNet模型智能检测用户说话结束
 * 3. 音频反馈播放 - 通过MAX98357A功放播放回复音频
 * 4. 云端AI对话 - 语音发送到云端进行AI处理
 *
 * 硬件配置：
 * - ESP32-S3-DevKitC-1开发板（需要PSRAM版本）
 * - INMP441数字麦克风（音频输入）
 *   连接方式：VDD->3.3V, GND->GND, SD->GPIO6, WS->GPIO4, SCK->GPIO5
 * - MAX98357A数字功放（音频输出）
 *   连接方式：DIN->GPIO7, BCLK->GPIO15, LRC->GPIO16, VIN->3.3V, GND->GND
 *
 * 音频参数：
 * - 采样率：16kHz
 * - 声道：单声道(Mono)
 * - 位深度：16位
 *
 * 使用的AI模型：
 * - 唤醒词检测：WakeNet9 "你好小智"模型
 * - 语音活动检测：VADNet1语音活动检测模型
 */

// 包含新的类头文件
#include "system_config.h"
#include "wifi_manager.h"
#include "wake_word_detector.h"
#include "audio_recorder.h"
#include "server_client.h"
#include "led_controller.h"

extern "C"
{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_process_sdkconfig.h"  // sdkconfig处理函数
#include "model_path.h"             // 模型路径定义
#include "bsp_board.h"              // 板级支持包，INMP441麦克风驱动
#include "esp_log.h"                // ESP日志系统
#include "mock_voices/welcome.h"    // 欢迎音频数据文件
#include "mock_voices/byebye.h"     // 再见音频数据文件
#include "nvs_flash.h"              // NVS存储
}

static const char *TAG = "语音识别"; // 日志标签

// 外接LED GPIO定义
#define LED_GPIO GPIO_NUM_21 // 外接LED灯珠连接到GPIO21

// 全局变量
static system_state_t current_state = STATE_WAITING_WAKEUP;
static TickType_t conversation_start_time = 0;

// 全局对象实例
static WiFiManager* wifi_manager = nullptr;
static WakeWordDetector* wake_detector = nullptr;
static AudioRecorder* audio_recorder = nullptr;
static ServerClient* server_client = nullptr;















/**
 * @brief 执行退出逻辑
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
        return;
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "✓ NVS初始化成功");

    // ========== 第二步：初始化WiFi ==========
    ESP_LOGI(TAG, "正在初始化WiFi连接...");
    wifi_manager = new WiFiManager();
    ret = wifi_manager->init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi初始化失败，将继续运行但无法使用网络功能");
    }
    else
    {
        ESP_LOGI(TAG, "✓ WiFi连接成功");
    }

    // ========== 第三步：初始化INMP441麦克风硬件 ==========
    ESP_LOGI(TAG, "正在初始化INMP441数字麦克风...");
    ret = bsp_board_init(16000, 1, 16); // 16kHz, 单声道, 16位
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "INMP441麦克风初始化失败: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "✓ INMP441麦克风初始化成功");

    // ========== 第四步：初始化音频播放功能 ==========
    ESP_LOGI(TAG, "正在初始化音频播放功能...");
    ret = bsp_audio_init(16000, 1, 16); // 16kHz, 单声道, 16位
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "音频播放初始化失败: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "✓ 音频播放初始化成功");

    // ========== 第五步：初始化语音识别模型 ==========
    ESP_LOGI(TAG, "正在加载语音识别模型...");

    // 从模型目录加载所有可用的语音识别模型
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL)
    {
        ESP_LOGE(TAG, "语音识别模型初始化失败");
        return;
    }

    // ========== 第六步：初始化唤醒词检测器 ==========
    ESP_LOGI(TAG, "正在初始化唤醒词检测器...");
    wake_detector = new WakeWordDetector();
    ret = wake_detector->init(models);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "唤醒词检测器初始化失败");
        return;
    }
    ESP_LOGI(TAG, "✓ 唤醒词检测器初始化成功");

    // ========== 第七步：初始化音频录制器 ==========
    ESP_LOGI(TAG, "正在初始化音频录制器...");
    audio_recorder = new AudioRecorder();
    ret = audio_recorder->init(models);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "音频录制器初始化失败");
        return;
    }
    ESP_LOGI(TAG, "✓ 音频录制器初始化成功");

    // ========== 第八步：初始化服务端客户端 ==========
    ESP_LOGI(TAG, "正在初始化服务端客户端...");
    server_client = new ServerClient();
    ESP_LOGI(TAG, "✓ 服务端客户端初始化成功");

    // ========== 第九步：准备音频缓冲区 ==========
    int audio_chunksize = wake_detector->get_chunk_size();
    int16_t *buffer = (int16_t *)malloc(audio_chunksize);
    if (buffer == NULL)
    {
        ESP_LOGE(TAG, "音频缓冲区内存分配失败");
        return;
    }

    // 显示系统配置信息
    ESP_LOGI(TAG, "✓ 智能语音助手系统配置完成:");
    ESP_LOGI(TAG, "  - 唤醒词模型: %s", wake_detector->get_model_name());
    ESP_LOGI(TAG, "  - 音频块大小: %d 字节", audio_chunksize);
    ESP_LOGI(TAG, "  - 会话超时: %d秒", CONVERSATION_TIMEOUT_MS / 1000);
    ESP_LOGI(TAG, "正在启动智能语音助手...");
    ESP_LOGI(TAG, "请对着麦克风说出唤醒词 '你好小智'");

    // ========== 第十步：主循环 ==========
    ESP_LOGI(TAG, "系统启动完成，等待唤醒词 '你好小智'...");

    while (1)
    {
        // 从INMP441麦克风获取一帧音频数据
        ret = bsp_get_feed_data(false, buffer, audio_chunksize);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "麦克风音频数据获取失败: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (current_state == STATE_WAITING_WAKEUP)
        {
            // 唤醒词检测
            wakenet_state_t wn_state = wake_detector->detect(buffer);

            if (wn_state == WAKENET_DETECTED)
            {
                ESP_LOGI(TAG, "🎉 检测到唤醒词 '你好小智'！");

                // 播放欢迎音频
                esp_err_t audio_ret = bsp_play_audio(welcome, welcome_len);
                if (audio_ret == ESP_OK)
                {
                    ESP_LOGI(TAG, "✓ 欢迎音频播放成功");
                }

                // 切换到录音状态
                current_state = STATE_RECORDING_AUDIO;
                conversation_start_time = xTaskGetTickCount();
                audio_recorder->start_recording();

                ESP_LOGI(TAG, "进入对话模式，请说话...");
            }
        }
        else if (current_state == STATE_RECORDING_AUDIO)
        {
            // 音频录制和VAD检测
            esp_err_t record_ret = audio_recorder->add_audio_data(buffer, audio_chunksize / sizeof(int16_t));

            // 添加调试日志
            static int main_debug_counter = 0;
            if (main_debug_counter % 200 == 0) { // 每200次打印一次
                ESP_LOGI(TAG, "主循环: record_ret=%d, 录制状态=%s, 音频长度=%zu",
                         record_ret, audio_recorder->is_recording() ? "录制中" : "已停止",
                         audio_recorder->get_audio_length());
            }
            main_debug_counter++;

            // 检查超时（防止用户一直不说话）
            if (audio_recorder->check_timeout())
            {
                ESP_LOGI(TAG, "录制超时，3秒内未检测到语音，退出对话模式");
                execute_exit_logic();
                continue;
            }

            if (record_ret == ESP_ERR_TIMEOUT || record_ret == ESP_ERR_NO_MEM)
            {
                // VAD检测到语音结束 或 音频缓冲区已满
                if (record_ret == ESP_ERR_TIMEOUT)
                {
                    ESP_LOGI(TAG, "检测到语音结束，发送到服务端处理...");
                }
                else
                {
                    ESP_LOGW(TAG, "音频缓冲区已满，用户说话时间较长，发送到服务端处理...");
                }

                // 发送音频到服务端
                uint8_t *response_audio = NULL;
                size_t response_len = 0;
                esp_err_t send_ret = server_client->send_audio(
                    audio_recorder->get_audio_data(),
                    audio_recorder->get_audio_length(),
                    &response_audio,
                    &response_len
                );

                if (send_ret == ESP_OK && response_audio != NULL)
                {
                    ESP_LOGI(TAG, "成功从服务端获取音频回复，开始播放...");

                    // 播放服务端返回的音频
                    esp_err_t play_ret = bsp_play_audio(response_audio, response_len);
                    if (play_ret == ESP_OK)
                    {
                        ESP_LOGI(TAG, "✓ 服务端音频回复播放成功");
                    }

                    // 释放响应音频内存
                    free(response_audio);

                    // 重新开始录音，等待下一句话
                    conversation_start_time = xTaskGetTickCount();
                    audio_recorder->start_recording();
                    ESP_LOGI(TAG, "继续等待下一句话...");
                }
                else
                {
                    ESP_LOGE(TAG, "发送音频到服务端失败，退出对话模式");
                    execute_exit_logic();
                }
            }
            else
            {
                // 继续录制，等待VAD检测语音结束
                // 不设置主循环超时，完全依赖VAD的语音活动检测
            }
        }

        // 短暂延时
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // ========== 资源清理 ==========
    // 注意：由于主循环是无限循环，以下代码正常情况下不会执行
    ESP_LOGI(TAG, "正在清理系统资源...");

    // 清理全局对象
    if (wake_detector) delete wake_detector;
    if (audio_recorder) delete audio_recorder;
    if (server_client) delete server_client;
    if (wifi_manager) delete wifi_manager;

    // 释放音频缓冲区内存
    if (buffer != NULL)
    {
        free(buffer);
    }

    // 删除当前任务
    vTaskDelete(NULL);
}
