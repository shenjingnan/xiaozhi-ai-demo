# 对话超时逻辑优化

## 问题描述

用户反馈在唤醒后说话时，即使还在说话，系统也会在5秒后自动退出，这影响了用户体验。

## 原始问题分析

1. **硬超时问题**：系统有5秒硬编码超时，无论用户是否还在说话都会强制退出
2. **VAD检测过于敏感**：1秒静音就认为语音结束，但用户可能只是在思考
3. **缺乏持续对话机制**：没有考虑用户想要连续对话的场景

## 优化方案

### 1. 动态超时机制

**原始逻辑**：
```cpp
static const TickType_t COMMAND_TIMEOUT_MS = 5000; // 5秒硬超时

// 检查手动超时
if ((current_time - command_timeout_start) > pdMS_TO_TICKS(COMMAND_TIMEOUT_MS))
{
    ESP_LOGW(TAG, "⏰ 命令词等待超时 (%lu秒)", (unsigned long)(COMMAND_TIMEOUT_MS / 1000));
    stop_audio_recording();
    execute_exit_logic();
}
```

**优化后逻辑**：
```cpp
static const TickType_t COMMAND_TIMEOUT_MS = 10000; // 增加到10秒
static TickType_t last_activity_time = 0; // 最后活动时间
static bool has_recent_speech = false; // 最近是否有语音

// 智能超时检查：只有在真正没有任何活动时才超时
TickType_t time_since_activity = current_time - last_activity_time;

// 如果有最近的语音活动，重置超时计时器
if (has_recent_speech && time_since_activity < pdMS_TO_TICKS(2000))
{
    command_timeout_start = current_time - pdMS_TO_TICKS(2000);
}
else if (time_since_start > pdMS_TO_TICKS(COMMAND_TIMEOUT_MS))
{
    // 只有在真正超时且没有最近活动时才退出
    if (!has_recent_speech || time_since_activity > pdMS_TO_TICKS(5000))
    {
        execute_exit_logic();
    }
}
```

### 2. VAD参数优化

**原始配置**：
```cpp
// VAD_MODE_2: 中等激进模式
// 500ms最小静音持续时间
vad_model_data = vadnet->create(vad_model_name, VAD_MODE_2, 1, 300, 500);

// 1秒静音就认为语音结束
if (silence_duration > pdMS_TO_TICKS(1000))
{
    stop_audio_recording();
    return ESP_ERR_TIMEOUT;
}
```

**优化后配置**：
```cpp
// VAD_MODE_1: 较为保守的模式，减少误判
// 800ms最小静音持续时间，给用户更多思考时间
vad_model_data = vadnet->create(vad_model_name, VAD_MODE_1, 1, 200, 800);

// 3秒静音才认为语音结束，给用户更多思考时间
if (silence_duration > pdMS_TO_TICKS(3000))
{
    stop_audio_recording();
    return ESP_ERR_TIMEOUT;
}
```

### 3. 活动状态跟踪

新增全局变量来跟踪用户活动：
```cpp
static TickType_t last_activity_time = 0; // 最后一次有任何活动的时间
static bool has_recent_speech = false; // 标记最近是否有语音活动
```

在检测到语音时更新状态：
```cpp
if (vad_state == VAD_SPEECH)
{
    speech_detected = true;
    has_recent_speech = true;
    last_speech_time = xTaskGetTickCount();
    last_activity_time = xTaskGetTickCount(); // 更新活动时间
}
```

## 优化效果

### 1. 更智能的超时机制
- **基础超时**：从5秒增加到10秒
- **动态重置**：检测到语音活动时自动重置计时器
- **智能判断**：区分"用户暂停思考"和"用户真正结束对话"

### 2. 更宽松的语音检测
- **VAD模式**：从激进模式改为保守模式，减少误判
- **静音阈值**：从1秒增加到3秒，给用户更多思考时间
- **最小静音时间**：从500ms增加到800ms

### 3. 更好的用户体验
- **持续对话**：用户可以在思考时暂停，不会被强制退出
- **自然交互**：支持更自然的对话节奏
- **容错性**：减少因网络延迟或思考时间导致的意外退出

## 测试建议

1. **长对话测试**：测试用户说话超过5秒的场景
2. **暂停思考测试**：测试用户在说话中间暂停思考的场景
3. **网络延迟测试**：测试在网络较慢时的表现
4. **噪音环境测试**：测试在有背景噪音时的稳定性

## 配置参数说明

| 参数 | 原始值 | 优化值 | 说明 |
|------|--------|--------|------|
| 基础超时 | 5秒 | 10秒 | 给用户更多时间 |
| VAD模式 | MODE_2(激进) | MODE_1(保守) | 减少误判 |
| 静音检测 | 1秒 | 3秒 | 更多思考时间 |
| 最小静音 | 500ms | 800ms | 更稳定的检测 |
| 活动重置 | 无 | 2秒内 | 智能重置计时器 |

这些优化确保了用户在正常对话中不会被意外打断，同时保持了系统的响应性和稳定性。
