/**
 * @file led_controller.cc
 * @brief LED控制器类实现
 */

#include "led_controller.h"

extern "C" {
#include "esp_log.h"
}

static const char *TAG = "LEDController";

LEDController::LEDController() : led_gpio(GPIO_NUM_NC), led_state(false) {
}

LEDController::~LEDController() {
}

esp_err_t LEDController::init(gpio_num_t gpio_pin) {
    led_gpio = gpio_pin;
    
    ESP_LOGI(TAG, "正在初始化LED (GPIO%d)...", led_gpio);

    // 配置GPIO为输出模式
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << led_gpio),    // 设置GPIO
        .mode = GPIO_MODE_OUTPUT,              // 输出模式
        .pull_up_en = GPIO_PULLUP_DISABLE,     // 禁用上拉
        .pull_down_en = GPIO_PULLDOWN_DISABLE, // 禁用下拉
        .intr_type = GPIO_INTR_DISABLE         // 禁用中断
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED GPIO初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 初始状态设置为关闭（低电平）
    turn_off();
    ESP_LOGI(TAG, "✓ LED初始化成功，初始状态：关闭");
    
    return ESP_OK;
}

void LEDController::turn_on() {
    if (led_gpio != GPIO_NUM_NC) {
        gpio_set_level(led_gpio, 1);
        led_state = true;
        ESP_LOGI(TAG, "LED点亮");
    }
}

void LEDController::turn_off() {
    if (led_gpio != GPIO_NUM_NC) {
        gpio_set_level(led_gpio, 0);
        led_state = false;
        ESP_LOGI(TAG, "LED熄灭");
    }
}
