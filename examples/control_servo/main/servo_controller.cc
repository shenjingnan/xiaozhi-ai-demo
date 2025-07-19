/**
 * @file servo_controller.cc
 * @brief ESP32-S3 舵机控制器类实现
 *
 * 本文件实现了ServoController类的所有方法，包括：
 * 1. 舵机PWM初始化和配置
 * 2. 舵机角度控制（绝对角度设置）
 * 3. 舵机旋转控制（相对角度旋转）
 * 4. 舵机状态管理
 */

#include "servo_controller.h"

// 静态成员变量定义
const char* ServoController::TAG = "舵机控制器";

ServoController::ServoController() 
    : current_angle_(CENTER_ANGLE), initialized_(false) {
    // 构造函数：初始化成员变量
}

ServoController::~ServoController() {
    // 析构函数：清理资源（如果需要的话）
}

esp_err_t ServoController::init() {
    ESP_LOGI(TAG, "正在初始化舵机 (GPIO%d)...", SERVO_GPIO);

    // 配置LEDC定时器
    ledc_timer_config_t ledc_timer = {
        .speed_mode = SERVO_LEDC_MODE,
        .duty_resolution = SERVO_PWM_RESOLUTION,
        .timer_num = SERVO_LEDC_TIMER,
        .freq_hz = SERVO_PWM_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };
    
    esp_err_t ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC定时器配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 配置LEDC通道
    ledc_channel_config_t ledc_channel = {
        .gpio_num = SERVO_GPIO,
        .speed_mode = SERVO_LEDC_MODE,
        .channel = SERVO_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = SERVO_LEDC_TIMER,
        .duty = 0, // 初始占空比为0
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD, // 默认模式：无输出时不关闭电源域
        .flags = {0}
    };
    
    ret = ledc_channel_config(&ledc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC通道配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 硬件配置完成，标记为已初始化
    initialized_ = true;

    // 设置舵机到中位（90度）
    current_angle_ = CENTER_ANGLE;
    ret = setAngle(current_angle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "舵机初始角度设置失败");
        initialized_ = false; // 如果角度设置失败，重置初始化标志
        return ret;
    }

    ESP_LOGI(TAG, "✓ 舵机初始化成功");
    return ESP_OK;
}

esp_err_t ServoController::setAngle(int angle) {
    if (!initialized_) {
        ESP_LOGE(TAG, "舵机未初始化，请先调用init()");
        return ESP_ERR_INVALID_STATE;
    }

    // 限制角度范围
    angle = constrainAngle(angle);

    // 计算对应的脉宽（微秒）
    int pulse_width = SERVO_MIN_PULSE_WIDTH + 
                      (angle * (SERVO_MAX_PULSE_WIDTH - SERVO_MIN_PULSE_WIDTH)) / MAX_ANGLE;

    // 计算占空比
    uint32_t duty = calculateDuty(angle);

    // 设置PWM占空比
    esp_err_t ret = ledc_set_duty(SERVO_LEDC_MODE, SERVO_LEDC_CHANNEL, duty);
    if (ret == ESP_OK) {
        ledc_update_duty(SERVO_LEDC_MODE, SERVO_LEDC_CHANNEL);
        current_angle_ = angle;
        ESP_LOGI(TAG, "舵机转动到 %d 度 (脉宽: %d us, 占空比: %lu)", 
                 angle, pulse_width, duty);
    } else {
        ESP_LOGE(TAG, "舵机角度设置失败: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t ServoController::rotate(int angle) {
    if (!initialized_) {
        ESP_LOGE(TAG, "舵机未初始化，请先调用init()");
        return ESP_ERR_INVALID_STATE;
    }

    int target_angle = current_angle_ + angle;

    // 限制角度范围
    target_angle = constrainAngle(target_angle);

    if (angle > 0) {
        ESP_LOGI(TAG, "🔄 舵机顺时针旋转%d度: %d° → %d°", angle, current_angle_, target_angle);
    } else if (angle < 0) {
        ESP_LOGI(TAG, "🔄 舵机逆时针旋转%d度: %d° → %d°", -angle, current_angle_, target_angle);
    } else {
        ESP_LOGI(TAG, "🔄 舵机保持当前位置: %d°", current_angle_);
        return ESP_OK;
    }

    return setAngle(target_angle);
}

int ServoController::getCurrentAngle() const {
    return current_angle_;
}

esp_err_t ServoController::resetToCenter() {
    ESP_LOGI(TAG, "重置舵机到中心位置 (%d度)", CENTER_ANGLE);
    return setAngle(CENTER_ANGLE);
}

bool ServoController::isInitialized() const {
    return initialized_;
}

uint32_t ServoController::calculateDuty(int angle) const {
    // 计算对应的脉宽（微秒）
    int pulse_width = SERVO_MIN_PULSE_WIDTH + 
                      (angle * (SERVO_MAX_PULSE_WIDTH - SERVO_MIN_PULSE_WIDTH)) / MAX_ANGLE;

    // 计算占空比（13位分辨率下）
    // 占空比 = (脉宽 / 周期) * 最大占空比值
    // 周期 = 1/50Hz = 20ms = 20000us
    uint32_t duty = (pulse_width * ((1 << SERVO_PWM_RESOLUTION) - 1)) / 20000;
    
    return duty;
}

int ServoController::constrainAngle(int angle) const {
    if (angle < MIN_ANGLE) {
        return MIN_ANGLE;
    }
    if (angle > MAX_ANGLE) {
        return MAX_ANGLE;
    }
    return angle;
}
