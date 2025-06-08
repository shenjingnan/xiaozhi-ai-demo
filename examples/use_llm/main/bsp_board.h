/**
 * Local BSP board header for ESP32-S3-DevKitC-1 with INMP441 microphone
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

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the board with specified audio parameters
 *
 * @param sample_rate Sample rate in Hz (e.g., 16000)
 * @param channel_format Number of channels (1 for mono, 2 for stereo)
 * @param bits_per_chan Bits per sample (16 or 32)
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t bsp_board_init(uint32_t sample_rate, int channel_format, int bits_per_chan);

/**
 * @brief Get audio data from microphone
 *
 * @param is_get_raw_channel Whether to get raw channel data without processing
 * @param buffer Buffer to store audio data
 * @param buffer_len Length of buffer in bytes
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t bsp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len);

/**
 * @brief Get the number of feed channels
 *
 * @return Number of channels
 */
int bsp_get_feed_channel(void);

/**
 * @brief Initialize I2S output for audio playback
 *
 * @param sample_rate Sample rate in Hz (e.g., 16000)
 * @param channel_format Number of channels (1 for mono, 2 for stereo)
 * @param bits_per_chan Bits per sample (16 or 32)
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t bsp_audio_init(uint32_t sample_rate, int channel_format, int bits_per_chan);

/**
 * @brief Play audio data through I2S output
 *
 * @param audio_data Pointer to audio data buffer
 * @param data_len Length of audio data in bytes
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t bsp_play_audio(const uint8_t *audio_data, size_t data_len);

/**
 * @brief Stop I2S audio output to prevent noise
 *
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t bsp_audio_stop(void);

#ifdef __cplusplus
}
#endif
