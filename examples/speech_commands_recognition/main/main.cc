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
#include "esp_wn_iface.h"  // 唤醒词检测接口
#include "esp_wn_models.h" // 唤醒词模型管理
#include "model_path.h"    // 模型路径定义
#include "bsp_board.h"     // 板级支持包，INMP441麦克风驱动
#include "esp_log.h"       // ESP日志系统
#include "mock_voices/welcome.h"       // 欢迎音频数据文件
}

static const char *TAG = "唤醒词检测"; // 日志标签



/**
 * @brief 应用程序主入口函数
 *
 * 初始化INMP441麦克风硬件，加载唤醒词检测模型，
 * 然后进入主循环进行实时音频采集和唤醒词检测。
 */
extern "C" void app_main(void)
{
    // ========== 第一步：初始化INMP441麦克风硬件 ==========
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

    // ========== 第三步：初始化唤醒词检测模型 ==========
    ESP_LOGI(TAG, "正在初始化唤醒词检测模型...");

    // 从模型目录加载所有可用的语音识别模型
    srmodel_list_t *models = esp_srmodel_init("model");
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

    // ========== 第五步：主循环 - 实时音频采集与唤醒词检测 ==========
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

        // 将音频数据送入唤醒词检测算法
        // 返回检测状态：WAKENET_NO_DETECT(未检测到) 或 WAKENET_DETECTED(检测到)
        wakenet_state_t state = wakenet->detect(model_data, buffer);

        // 检查是否检测到唤醒词
        if (state == WAKENET_DETECTED)
        {
            ESP_LOGI(TAG, "🎉 检测到唤醒词！");

            // 输出检测结果到串口
            printf("=== 唤醒词检测成功！模型: %s ===\n", model_name);
            printf("=== Wake word detected! Model: %s ===\n", model_name);

            // 播放音频提示音
            ESP_LOGI(TAG, "播放音频提示音");
            esp_err_t audio_ret = bsp_play_audio(welcome, welcome_len);
            if (audio_ret != ESP_OK) {
                ESP_LOGE(TAG, "音频播放失败: %s", esp_err_to_name(audio_ret));
            } else {
                ESP_LOGI(TAG, "✓ 音频播放成功");
            }

            // 这里可以添加唤醒后的处理逻辑
            // 例如：启动语音识别、发送网络请求等
        }

        // 短暂延时，避免CPU占用过高，同时保证实时性
        // 1ms延时确保检测的实时性
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
