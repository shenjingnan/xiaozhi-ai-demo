/**
 * ESP32-S3-DevKitC-1 with INMP441 microphone board support
 * ESP32-S3-DevKitC-1 开发板配合 INMP441 麦克风的硬件抽象层实现
 *
 * @copyright Copyright 2021 Espressif Systems (Shanghai) Co. Ltd.
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *               http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include <string.h>
#include "bsp_board.h"
#include "driver/i2s_std.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// INMP441 I2S 引脚配置
// INMP441 是一个数字 MEMS 麦克风，通过 I2S 接口与 ESP32-S3 通信
#define I2S_WS_PIN GPIO_NUM_4  // 字选择信号 (Word Select/LR Clock) - 控制左右声道
#define I2S_SCK_PIN GPIO_NUM_5 // 串行时钟信号 (Serial Clock/Bit Clock) - 数据传输时钟
#define I2S_SD_PIN GPIO_NUM_6  // 串行数据信号 (Serial Data) - 音频数据输出

// MAX98357A I2S 输出引脚配置
// MAX98357A 是一个数字音频功放，通过 I2S 接口接收音频数据
#define I2S_OUT_BCLK_PIN GPIO_NUM_15 // 位时钟信号 (Bit Clock)
#define I2S_OUT_LRC_PIN GPIO_NUM_16  // 左右声道时钟信号 (LR Clock)
#define I2S_OUT_DIN_PIN GPIO_NUM_7   // 数据输入信号 (Data Input)
#define I2S_OUT_SD_PIN GPIO_NUM_8    // Shutdown引脚 (可选，用于关闭功放)

// I2S 配置参数
#define I2S_PORT_RX I2S_NUM_0 // 使用 I2S 端口 0 用于录音
#define I2S_PORT_TX I2S_NUM_1 // 使用 I2S 端口 1 用于播放
#define SAMPLE_RATE 16000     // 采样率 16kHz，适合语音识别
#define BITS_PER_SAMPLE 16    // 每个采样点 16 位
#define CHANNELS 1            // 单声道配置

static const char *TAG = "bsp_board";

// I2S 接收通道句柄，用于管理音频数据接收
static i2s_chan_handle_t rx_handle = nullptr;
// I2S 发送通道句柄，用于管理音频数据播放
static i2s_chan_handle_t tx_handle = nullptr;
// I2S 发送通道状态标志
static bool tx_channel_enabled = false;

/**
 * @brief 初始化 I2S 接口用于 INMP441 麦克风
 *
 * INMP441 是一个数字 MEMS 麦克风，需要特定的 I2S 配置：
 * - 使用标准 I2S 协议 (Philips 格式)
 * - 单声道模式，只使用左声道
 * - 16 位数据宽度
 *
 * @param sample_rate 采样率 (Hz)
 * @param channel_format 声道数 (1=单声道, 2=立体声)
 * @param bits_per_chan 每个采样点的位数 (16 或 32)
 * @return esp_err_t 初始化结果
 */
static esp_err_t bsp_i2s_init(uint32_t sample_rate, int channel_format, int bits_per_chan)
{
    esp_err_t ret = ESP_OK;

    // 创建 I2S 通道配置
    // 设置为主模式，ESP32-S3 作为时钟源
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT_RX, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&chan_cfg, nullptr, &rx_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "创建 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 🎯 确定数据位宽度
    i2s_data_bit_width_t bit_width = (bits_per_chan == 32) ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;

    // 配置 I2S 标准模式，专门针对 INMP441 优化
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, I2S_SLOT_MODE_MONO), // 插槽配置
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // INMP441 不需要主时钟
            .bclk = I2S_SCK_PIN,     // 位时钟引脚
            .ws = I2S_WS_PIN,        // 字选择引脚
            .dout = I2S_GPIO_UNUSED, // 不需要数据输出（仅录音）
            .din = I2S_SD_PIN,       // 数据输入引脚
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    // INMP441 特定配置调整
    // INMP441 输出左对齐数据，我们只使用左声道
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    // 初始化 I2S 标准模式
    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 I2S 标准模式失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 启用 I2S 通道开始接收数据
    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "启用 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 清理初始噪音：读取并丢弃前几帧数据
    const size_t discard_samples = 8192; // 丢弃前8KB数据
    uint8_t *discard_buffer = (uint8_t *)malloc(discard_samples);
    if (discard_buffer) {
        size_t bytes_read;
        for (int i = 0; i < 3; i++) { // 读取3次
            i2s_channel_read(rx_handle, discard_buffer, discard_samples, &bytes_read, pdMS_TO_TICKS(100));
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        free(discard_buffer);
        ESP_LOGD(TAG, "已清理I2S输入缓冲区初始数据");
    }

    ESP_LOGI(TAG, "I2S 初始化成功");
    return ESP_OK;
}

/**
 * @brief 🚀 初始化开发板硬件
 *
 * 这是整个音频系统的“启动按钮”，它会：
 * - 初始化INMP441麦克风
 * - 设置好所有GPIO引脚
 * - 准备好录音功能
 *
 * @param sample_rate 采样率（Hz），推荐16000
 * @param channel_format 声道格式，1=单声道
 * @param bits_per_chan 每个采样点的位数，推荐16
 * @return esp_err_t 初始化结果
 */
esp_err_t bsp_board_init(uint32_t sample_rate, int channel_format, int bits_per_chan)
{
    ESP_LOGI(TAG, "🚀 正在初始化ESP32-S3-DevKitC-1 + INMP441麦克风");
    ESP_LOGI(TAG, "🎵 音频参数: 采样率=%ldHz, 声道数=%d, 位深=%d位",
             sample_rate, channel_format, bits_per_chan);

    return bsp_i2s_init(sample_rate, channel_format, bits_per_chan);
}

/**
 * @brief 🎤 从麦克风获取音频数据
 *
 * 这个函数就像“录音师”，它会：
 * 
 * 🎯 工作流程：
 * 1. 从I2S接口读取原始数据
 * 2. 对INMP441的输出进行格式转换
 * 3. 可选择性应用增益调整
 * 4. 确保数据适合语音识别
 *
 * @param is_get_raw_channel 是否获取原始数据（true=不处理）
 * @param buffer 存储音频数据的缓冲区
 * @param buffer_len 缓冲区长度（字节）
 * @return esp_err_t 读取结果
 */
esp_err_t bsp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_read = 0;

    // 🎤 从I2S通道读取音频数据
    ret = i2s_channel_read(rx_handle, buffer, buffer_len, &bytes_read, portMAX_DELAY);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ 读取I2S数据失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 🔍 检查读取的数据长度是否符合预期
    if (bytes_read != buffer_len)
    {
        ESP_LOGW(TAG, "⚠️ 预期读取%d字节，实际读取%d字节", buffer_len, bytes_read);
    }

    // 🎯 INMP441特定的数据处理
    // INMP441输出24位数据在 32位帧中，左对齐
    // 我们需要提取最高有效的16位用于语音识别
    if (!is_get_raw_channel)
    {
        int samples = buffer_len / sizeof(int16_t);

        // 🎶 对INMP441的数据进行处理
        // 麦克风输出左对齐数据，进行信号电平调整
        for (int i = 0; i < samples; i++)
        {
            // 当前使用原始信号电平（无增益）
            // 测试表明原始电平已足够满足唤醒词检测需求
            int32_t sample = static_cast<int32_t>(buffer[i]);

            // 🔊 可选：应用2倍增益以提升信号强度（当前已禁用）
            // 如果发现声音太小，可以取消下面这行的注释
            // sample = sample * 2;

            // 📦 限制在16位有符号整数范围内
            if (sample > 32767)
            {
                sample = 32767;
            }
            if (sample < -32768)
            {
                sample = -32768;
            }

            buffer[i] = static_cast<int16_t>(sample);
        }
    }

    return ESP_OK;
}

/**
 * @brief 🎵 获取音频输入通道数
 *
 * 返回当前麦克风的声道数。
 * 我们使用单声道，节省资源且足够语音识别使用。
 *
 * @return int 通道数（1=单声道）
 */
int bsp_get_feed_channel(void)
{
    return CHANNELS;
}

/**
 * @brief 🔊 初始化I2S输出接口用于MAX98357A功放
 *
 * 这个函数专门为MAX98357A功放配置I2S通信：
 * 
 * 🔧 I2S配置特点：
 * - 使用Philips标准协议
 * - 支持单声道/立体声
 * - 16位数据宽度
 * - 3W输出功率
 *
 * @param sample_rate 采样率（Hz）
 * @param channel_format 声道数（1=单声道，2=立体声）
 * @param bits_per_chan 每个采样点的位数（16或32）
 * @return esp_err_t 初始化结果
 */
esp_err_t bsp_audio_init(uint32_t sample_rate, int channel_format, int bits_per_chan)
{
    esp_err_t ret = ESP_OK;

    // 🔌 初始化MAX98357A的SD引脚（控制功放开关）
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2S_OUT_SD_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(I2S_OUT_SD_PIN, 1); // 高电平启用功放
    ESP_LOGI(TAG, "✅ MAX98357A SD引脚已初始化（GPIO%d）", I2S_OUT_SD_PIN);

    // 🔧 创建I2S发送通道配置
    // ESP32作为主机（Master），提供时钟信号给功放
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT_TX, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&chan_cfg, &tx_handle, nullptr);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ 创建I2S发送通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 🎯 确定数据位宽度
    i2s_data_bit_width_t bit_width = (bits_per_chan == 32) ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;

    // 🎶 配置I2S标准模式（专门为MAX98357A优化）
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, (channel_format == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // MCLK：MAX98357A不需要主时钟
            .bclk = I2S_OUT_BCLK_PIN,  // BCLK：位时钟→ GPIO15
            .ws = I2S_OUT_LRC_PIN,     // LRC：左右声道时钟→ GPIO16
            .dout = I2S_OUT_DIN_PIN,   // DIN：数据输出→ GPIO7
            .din = I2S_GPIO_UNUSED,    // DIN：不需要（只播放不录音）
            .invert_flags = {
                .mclk_inv = false,     // 不反转主时钟
                .bclk_inv = false,     // 不反转位时钟
                .ws_inv = false,       // 不反转字选择
            },
        },
    };

    // 🚀 初始化I2S标准模式
    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ 初始化I2S发送标准模式失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // ▶️ 启用I2S发送通道开始播放数据
    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ 启用I2S发送通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 🟢 设置通道状态标志
    tx_channel_enabled = true;

    ESP_LOGI(TAG, "✅ I2S音频播放初始化成功");
    return ESP_OK;
}

/**
 * @brief 通过 I2S 播放音频数据
 *
 * 这个函数将音频数据发送到 MAX98357A 功放进行播放：
 * 1. 将音频数据写入 I2S 发送通道
 * 2. 确保数据完全发送
 *
 * @param audio_data 指向音频数据的指针
 * @param data_len 音频数据长度（字节）
 * @return esp_err_t 播放结果
 */
esp_err_t bsp_play_audio(const uint8_t *audio_data, size_t data_len)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_written = 0;
    size_t total_written = 0;

    if (tx_handle == nullptr)
    {
        ESP_LOGE(TAG, "❌ I2S发送通道未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    if (audio_data == nullptr || data_len == 0)
    {
        ESP_LOGE(TAG, "❌ 无效的音频数据");
        return ESP_ERR_INVALID_ARG;
    }

    // 确保 I2S 发送通道已启用（如果之前被停止了）
    if (!tx_channel_enabled)
    {
        // 先启用功放
        gpio_set_level(I2S_OUT_SD_PIN, 1); // 高电平启用功放
        vTaskDelay(pdMS_TO_TICKS(10)); // 等待功放启动
        ESP_LOGD(TAG, "✅ MAX98357A功放已启用");
        
        ret = i2s_channel_enable(tx_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "❌ 启用I2S发送通道失败: %s", esp_err_to_name(ret));
            return ret;
        }
        tx_channel_enabled = true;
        ESP_LOGD(TAG, "✅ I2S发送通道已重新启用");
        
        // 发送一小段静音数据来初始化通道
        const size_t init_silence_size = 256; // 减小到256字节，避免大量内存分配
        static uint8_t init_silence[256] = {0}; // 使用静态数组，避免动态分配
        size_t silence_written = 0;
        i2s_channel_write(tx_handle, init_silence, init_silence_size, &silence_written, pdMS_TO_TICKS(10));
    }

    // 循环写入音频数据，确保所有数据都被发送
    while (total_written < data_len)
    {
        size_t bytes_to_write = data_len - total_written;
        
        // 将音频数据写入 I2S 发送通道
        ret = i2s_channel_write(tx_handle, audio_data + total_written, bytes_to_write, &bytes_written, portMAX_DELAY);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "❌ 写入I2S音频数据失败: %s", esp_err_to_name(ret));
            break;
        }

        total_written += bytes_written;

        // 显示播放进度（每10KB显示一次）
        if ((total_written % 10240) < bytes_written)
        {
            ESP_LOGD(TAG, "音频播放进度: %zu/%zu 字节 (%.1f%%)", 
                     total_written, data_len, (float)total_written * 100.0f / data_len);
        }
    }

    if (total_written != data_len)
    {
        ESP_LOGW(TAG, "音频数据写入不完整: 预期 %zu 字节，实际写入 %zu 字节", data_len, total_written);
        return ESP_FAIL;
    }

    // 播放完成后停止I2S输出以防止噪音
    esp_err_t stop_ret = bsp_audio_stop();
    if (stop_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "停止音频输出时出现警告: %s", esp_err_to_name(stop_ret));
    }

    ESP_LOGI(TAG, "音频播放完成，播放了 %zu 字节", total_written);
    return ESP_OK;
}

/**
 * @brief 通过 I2S 播放音频数据（流式版本，不停止I2S）
 *
 * 这个函数与 bsp_play_audio 类似，但不会在播放完成后停止I2S，
 * 适用于连续播放多个音频块的流式场景。
 *
 * @param audio_data 指向音频数据的指针
 * @param data_len 音频数据长度（字节）
 * @return esp_err_t 播放结果
 */
esp_err_t bsp_play_audio_stream(const uint8_t *audio_data, size_t data_len)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_written = 0;
    size_t total_written = 0;

    if (tx_handle == nullptr)
    {
        ESP_LOGE(TAG, "❌ I2S发送通道未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    if (audio_data == nullptr || data_len == 0)
    {
        ESP_LOGE(TAG, "❌ 无效的音频数据");
        return ESP_ERR_INVALID_ARG;
    }

    // 确保 I2S 发送通道已启用（如果之前被停止了）
    if (!tx_channel_enabled)
    {
        // 先启用功放
        gpio_set_level(I2S_OUT_SD_PIN, 1); // 高电平启用功放
        vTaskDelay(pdMS_TO_TICKS(10)); // 等待功放启动
        ESP_LOGD(TAG, "✅ MAX98357A功放已启用");
        
        ret = i2s_channel_enable(tx_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "❌ 启用I2S发送通道失败: %s", esp_err_to_name(ret));
            return ret;
        }
        tx_channel_enabled = true;
        ESP_LOGD(TAG, "✅ I2S发送通道已重新启用");
        
        // 发送一小段静音数据来初始化通道
        const size_t init_silence_size = 256; // 减小到256字节，避免大量内存分配
        static uint8_t init_silence[256] = {0}; // 使用静态数组，避免动态分配
        size_t silence_written = 0;
        i2s_channel_write(tx_handle, init_silence, init_silence_size, &silence_written, pdMS_TO_TICKS(10));
    }

    // 循环写入音频数据，确保所有数据都被发送
    while (total_written < data_len)
    {
        size_t bytes_to_write = data_len - total_written;
        
        // 将音频数据写入 I2S 发送通道
        ret = i2s_channel_write(tx_handle, audio_data + total_written, bytes_to_write, &bytes_written, portMAX_DELAY);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "❌ 写入I2S音频数据失败: %s", esp_err_to_name(ret));
            break;
        }

        total_written += bytes_written;

        // 显示播放进度（每10KB显示一次）
        if ((total_written % 10240) < bytes_written)
        {
            ESP_LOGD(TAG, "音频播放进度: %zu/%zu 字节 (%.1f%%)", 
                     total_written, data_len, (float)total_written * 100.0f / data_len);
        }
    }

    if (total_written != data_len)
    {
        ESP_LOGW(TAG, "音频数据写入不完整: 预期 %zu 字节，实际写入 %zu 字节", data_len, total_written);
        return ESP_FAIL;
    }

    // 注意：这里不调用 bsp_audio_stop()，保持I2S继续运行
    ESP_LOGD(TAG, "流式音频块播放完成，播放了 %zu 字节", total_written);
    return ESP_OK;
}

/**
 * @brief 停止 I2S 音频输出以防止噪音
 *
 * 这个函数会暂时禁用 I2S 发送通道，停止向 MAX98357A 发送数据，
 * 从而消除播放完成后的噪音。当需要再次播放音频时，
 * 可以重新启用通道。
 *
 * @return esp_err_t 停止结果
 */
esp_err_t bsp_audio_stop(void)
{
    esp_err_t ret = ESP_OK;

    if (tx_handle == nullptr)
    {
        ESP_LOGW(TAG, "⚠️ I2S发送通道未初始化，无需停止");
        return ESP_OK;
    }

    // 🟢 只有在通道启用时才禁用它
    if (tx_channel_enabled)
    {
        // 🔇 发送一些静音数据来清空缓冲区
        const size_t silence_size = 4096; // 4KB的静音数据
        uint8_t *silence_buffer = (uint8_t *)calloc(silence_size, 1);
        if (silence_buffer) {
            size_t bytes_written = 0;
            i2s_channel_write(tx_handle, silence_buffer, silence_size, &bytes_written, pdMS_TO_TICKS(100));
            free(silence_buffer);
            ESP_LOGD(TAG, "✅ 已发送静音数据清空缓冲区");
        }
        
        // ⏱️ 等待一小段时间让静音数据播放完
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // 🔌 先通过SD引脚关闭功放，防止噪音
        gpio_set_level(I2S_OUT_SD_PIN, 0); // 低电平关闭功放
        ESP_LOGD(TAG, "✅ MAX98357A功放已关闭");
        vTaskDelay(pdMS_TO_TICKS(10)); // 等待功放完全关闭
        
        // 🛑️ 禁用I2S发送通道
        ret = i2s_channel_disable(tx_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "❌ 禁用I2S发送通道失败: %s", esp_err_to_name(ret));
            return ret;
        }
        tx_channel_enabled = false;
        ESP_LOGI(TAG, "✅ I2S音频输出已停止");
    }
    else
    {
        ESP_LOGD(TAG, "ℹ️ I2S发送通道已经是禁用状态");
    }

    return ESP_OK;
}
