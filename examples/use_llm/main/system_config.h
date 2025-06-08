/**
 * @file system_config.h
 * @brief 系统配置和常量定义
 */

#pragma once

// WiFi配置
#define WIFI_SSID "1804"
#define WIFI_PASS "Sjn123123@"
#define WIFI_MAXIMUM_RETRY 5

// 服务器配置
#define SERVER_URL "http://192.168.1.152:8080/process_audio"

// 音频录制相关定义
#define MAX_AUDIO_BUFFER_SIZE (16000 * 2 * 10) // 10秒的音频缓冲区 (16kHz, 16bit)
#define SILENCE_THRESHOLD 500                  // 静音阈值
#define SILENCE_DURATION_MS 2000               // 静音持续时间（毫秒）

// 超时配置
#define CONVERSATION_TIMEOUT_MS 10000          // 对话超时时间（10秒，给VAD足够时间检测）

// 系统状态定义
typedef enum
{
    STATE_WAITING_WAKEUP = 0,  // 等待唤醒词
    STATE_RECORDING_AUDIO = 1, // 录制音频中
} system_state_t;
