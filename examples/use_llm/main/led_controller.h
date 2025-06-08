/**
 * @file led_controller.h
 * @brief LED控制器类
 */

#pragma once

extern "C" {
#include "driver/gpio.h"
}

class LEDController {
public:
    LEDController();
    ~LEDController();
    
    /**
     * @brief 初始化LED控制器
     * @param gpio_pin LED连接的GPIO引脚
     * @return esp_err_t 
     */
    esp_err_t init(gpio_num_t gpio_pin);
    
    /**
     * @brief 点亮LED
     */
    void turn_on();
    
    /**
     * @brief 熄灭LED
     */
    void turn_off();
    
    /**
     * @brief 获取LED状态
     * @return true LED点亮
     * @return false LED熄灭
     */
    bool is_on() const { return led_state; }

private:
    gpio_num_t led_gpio;
    bool led_state;
};
