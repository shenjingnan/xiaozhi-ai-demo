/**
 * @file servo_controller.h
 * @brief ESP32-S3 舵机控制器类定义
 *
 * 本文件定义了ServoController类，用于封装舵机控制的所有功能，包括：
 * 1. 舵机PWM初始化和配置
 * 2. 舵机角度控制（绝对角度设置）
 * 3. 舵机旋转控制（相对角度旋转）
 * 4. 舵机状态管理
 *
 * 硬件要求：
 * - ESP32-S3开发板
 * - 舵机连接到GPIO18
 *   连接方式：红线->5V/3.3V, 棕线->GND, 橙线->GPIO18
 *
 * PWM参数：
 * - 频率：50Hz
 * - 脉宽范围：0.5ms-2.5ms对应0-180度
 * - 分辨率：13位（8192级别）
 */

#pragma once

extern "C" {
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_err.h"
}

/**
 * @brief 舵机控制器类
 * 
 * 封装了舵机控制的所有功能，提供简洁易用的API接口
 */
class ServoController {
public:
    // 舵机控制GPIO和PWM配置常量
    static constexpr gpio_num_t SERVO_GPIO = GPIO_NUM_18;                 // 舵机PWM信号连接到GPIO18
    static constexpr ledc_timer_t SERVO_LEDC_TIMER = LEDC_TIMER_0;        // 使用LEDC定时器0
    static constexpr ledc_channel_t SERVO_LEDC_CHANNEL = LEDC_CHANNEL_0;  // 使用LEDC通道0
    static constexpr ledc_mode_t SERVO_LEDC_MODE = LEDC_LOW_SPEED_MODE;   // 低速模式
    static constexpr uint32_t SERVO_PWM_FREQ = 50;                       // 舵机PWM频率50Hz
    static constexpr ledc_timer_bit_t SERVO_PWM_RESOLUTION = LEDC_TIMER_13_BIT; // 13位分辨率（8192级别）

    // 舵机PWM脉宽定义（微秒）
    static constexpr int SERVO_MIN_PULSE_WIDTH = 500;     // 0度对应的脉宽（0.5ms）
    static constexpr int SERVO_MAX_PULSE_WIDTH = 2500;    // 180度对应的脉宽（2.5ms）
    static constexpr int SERVO_CENTER_PULSE_WIDTH = 1250; // 45度对应的脉宽（1.25ms）

    // 角度范围常量
    static constexpr int MIN_ANGLE = 0;     // 最小角度
    static constexpr int MAX_ANGLE = 180;   // 最大角度
    static constexpr int CENTER_ANGLE = 90; // 中心角度

public:
    /**
     * @brief 构造函数
     */
    ServoController();

    /**
     * @brief 析构函数
     */
    ~ServoController();

    /**
     * @brief 初始化舵机PWM控制
     * 
     * 配置GPIO18为PWM输出模式，用于控制舵机
     * PWM频率：50Hz，脉宽范围：0.5ms-2.5ms对应0-180度
     * 
     * @return esp_err_t 
     *         - ESP_OK: 初始化成功
     *         - ESP_FAIL: 初始化失败
     */
    esp_err_t init();

    /**
     * @brief 设置舵机角度（绝对角度）
     * 
     * @param angle 目标角度（0-180度）
     * @return esp_err_t 
     *         - ESP_OK: 设置成功
     *         - ESP_ERR_INVALID_ARG: 角度参数无效
     *         - ESP_FAIL: 设置失败
     */
    esp_err_t setAngle(int angle);

    /**
     * @brief 舵机旋转指定角度（相对角度）
     * 
     * @param angle 旋转角度，正数为顺时针，负数为逆时针
     * @return esp_err_t 
     *         - ESP_OK: 旋转成功
     *         - ESP_ERR_INVALID_ARG: 角度参数无效
     *         - ESP_FAIL: 旋转失败
     */
    esp_err_t rotate(int angle);

    /**
     * @brief 获取当前舵机角度
     * 
     * @return int 当前角度（0-180度）
     */
    int getCurrentAngle() const;

    /**
     * @brief 重置舵机到中心位置（90度）
     * 
     * @return esp_err_t 
     *         - ESP_OK: 重置成功
     *         - ESP_FAIL: 重置失败
     */
    esp_err_t resetToCenter();

    /**
     * @brief 检查舵机是否已初始化
     * 
     * @return true 已初始化
     * @return false 未初始化
     */
    bool isInitialized() const;

private:
    /**
     * @brief 计算角度对应的PWM占空比
     * 
     * @param angle 角度（0-180度）
     * @return uint32_t PWM占空比值
     */
    uint32_t calculateDuty(int angle) const;

    /**
     * @brief 限制角度范围
     * 
     * @param angle 输入角度
     * @return int 限制后的角度（0-180度）
     */
    int constrainAngle(int angle) const;

private:
    int current_angle_;      // 当前舵机角度
    bool initialized_;       // 初始化状态标志
    static const char* TAG;  // 日志标签
};
