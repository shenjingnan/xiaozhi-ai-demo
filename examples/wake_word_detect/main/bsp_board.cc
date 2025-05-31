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

// INMP441 I2S 引脚配置
// INMP441 是一个数字 MEMS 麦克风，通过 I2S 接口与 ESP32-S3 通信
#define I2S_WS_PIN GPIO_NUM_4  // 字选择信号 (Word Select/LR Clock) - 控制左右声道
#define I2S_SCK_PIN GPIO_NUM_5 // 串行时钟信号 (Serial Clock/Bit Clock) - 数据传输时钟
#define I2S_SD_PIN GPIO_NUM_6  // 串行数据信号 (Serial Data) - 音频数据输出

// I2S 配置参数
#define I2S_PORT I2S_NUM_0     // 使用 I2S 端口 0
#define SAMPLE_RATE 16000      // 采样率 16kHz，适合语音识别
#define BITS_PER_SAMPLE 16     // 每个采样点 16 位
#define CHANNELS 1             // 单声道配置

static const char *TAG = "bsp_board";

// I2S 接收通道句柄，用于管理音频数据接收
static i2s_chan_handle_t rx_handle = nullptr;

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
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&chan_cfg, nullptr, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 确定正确的数据位宽度枚举值
    i2s_data_bit_width_t bit_width = (bits_per_chan == 32) ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;

    // 配置 I2S 标准模式，专门针对 INMP441 优化
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,  // 不使用外部时钟
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, I2S_SLOT_MODE_MONO),  // 插槽配置
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,    // INMP441 不需要主时钟
            .bclk = I2S_SCK_PIN,        // 位时钟引脚
            .ws = I2S_WS_PIN,           // 字选择引脚
            .dout = I2S_GPIO_UNUSED,    // 不需要数据输出（仅录音）
            .din = I2S_SD_PIN,          // 数据输入引脚
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
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化 I2S 标准模式失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 启用 I2S 通道开始接收数据
    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启用 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2S 初始化成功");
    return ESP_OK;
}



/**
 * @brief 初始化开发板硬件
 *
 * 这是硬件抽象层的主要初始化函数，设置 INMP441 麦克风
 *
 * @param sample_rate 采样率 (Hz)，推荐 16000
 * @param channel_format 声道格式，1=单声道
 * @param bits_per_chan 每个采样点的位数，推荐 16
 * @return esp_err_t 初始化结果
 */
esp_err_t bsp_board_init(uint32_t sample_rate, int channel_format, int bits_per_chan)
{
    ESP_LOGI(TAG, "正在初始化 ESP32-S3-DevKitC-1 配合 INMP441 麦克风");
    ESP_LOGI(TAG, "音频参数: 采样率=%ld Hz, 声道数=%d, 位深=%d",
             sample_rate, channel_format, bits_per_chan);

    return bsp_i2s_init(sample_rate, channel_format, bits_per_chan);
}

/**
 * @brief 从麦克风获取音频数据
 *
 * 这个函数从 INMP441 麦克风读取音频数据，并进行必要的信号处理：
 * 1. 从 I2S 接口读取原始数据
 * 2. 对 INMP441 的输出进行格式转换和增益调整
 * 3. 确保数据适合后续的语音识别处理
 *
 * @param is_get_raw_channel 是否获取原始通道数据（不进行处理）
 * @param buffer 存储音频数据的缓冲区
 * @param buffer_len 缓冲区长度（字节）
 * @return esp_err_t 读取结果
 */
esp_err_t bsp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_read = 0;

    // 从 I2S 通道读取音频数据
    ret = i2s_channel_read(rx_handle, buffer, buffer_len, &bytes_read, portMAX_DELAY);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "读取 I2S 数据失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 检查读取的数据长度是否符合预期
    if (bytes_read != buffer_len) {
        ESP_LOGW(TAG, "预期读取 %d 字节，实际读取 %d 字节", buffer_len, bytes_read);
    }

    // INMP441 特定的数据处理
    // INMP441 输出 24 位数据在 32 位帧中，左对齐
    // 我们需要提取最高有效的 16 位用于 16 位音频处理
    if (!is_get_raw_channel) {
        int samples = buffer_len / sizeof(int16_t);

        // 对 INMP441 的数据进行处理
        // 麦克风输出左对齐数据，进行信号电平调整
        for (int i = 0; i < samples; i++) {
            // 当前使用原始信号电平（无增益）
            // 测试表明原始电平已足够满足唤醒词检测需求
            int32_t sample = static_cast<int32_t>(buffer[i]);

            // 可选：应用 2 倍增益以提升信号强度（当前已禁用）
            // 如果发现信号电平不足，可以取消下面这行的注释
            // sample = sample * 2;

            // 限制在 16 位有符号整数范围内
            if (sample > 32767) {
                sample = 32767;
            }
            if (sample < -32768) {
                sample = -32768;
            }

            buffer[i] = static_cast<int16_t>(sample);
        }
    }

    return ESP_OK;
}

/**
 * @brief 获取音频输入通道数
 *
 * @return int 通道数（1=单声道）
 */
int bsp_get_feed_channel(void)
{
    return CHANNELS;
}
