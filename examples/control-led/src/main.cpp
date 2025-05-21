#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

// 定义LED引脚
#define LED_PIN GPIO_NUM_21
// 定义按钮引脚
#define BUTTON_PIN GPIO_NUM_41

class LightController {
private:
    gpio_num_t pin;
    bool state;
    const char* TAG;

public:
    LightController(gpio_num_t pin) : pin(pin), state(false), TAG("LightController") {
        // 配置GPIO
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        turnOff(); // 初始状态为关闭
        ESP_LOGI(TAG, "LightController initialized on pin %d", pin);
    }

    void turnOn() {
        gpio_set_level(pin, 1); // 高电平点亮LED
        state = true;
        ESP_LOGI(TAG, "LED turned ON");
    }

    void turnOff() {
        gpio_set_level(pin, 0); // 低电平熄灭LED
        state = false;
        ESP_LOGI(TAG, "LED turned OFF");
    }

    void toggle() {
        if (state) {
            turnOff();
        } else {
            turnOn();
        }
    }

    void blink(int delayMs) {
        toggle();
        vTaskDelay(delayMs / portTICK_PERIOD_MS);
    }
};

class ButtonController {
private:
    gpio_num_t pin;
    bool lastState;
    const char* TAG;

public:
    ButtonController(gpio_num_t pin) : pin(pin), lastState(false), TAG("ButtonController") {
        // 配置GPIO
        gpio_reset_pin(pin);
        // 设置为输入模式，启用内部下拉电阻
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
        ESP_LOGI(TAG, "ButtonController initialized on pin %d", pin);
    }

    // 读取按钮状态
    bool isPressed() {
        // 读取GPIO电平
        bool currentState = gpio_get_level(pin) == 1;

        // 如果状态发生变化，记录日志
        if (currentState != lastState) {
            ESP_LOGI(TAG, "Button state changed to: %s", currentState ? "PRESSED" : "RELEASED");
            lastState = currentState;
        }

        return currentState;
    }
};

extern "C" void app_main() {
    // 创建LED控制器实例
    LightController ledController(LED_PIN);
    // 创建按钮控制器实例
    ButtonController buttonController(BUTTON_PIN);

    printf("按钮控制LED程序开始运行，LED连接在GPIO %d，按钮连接在GPIO %d\n", LED_PIN, BUTTON_PIN);

    // 无限循环，检测按钮状态并控制LED
    while (1) {
        // 检查按钮状态
        if (buttonController.isPressed()) {
            // 按钮被按下，点亮LED
            ledController.turnOn();
        } else {
            // 按钮释放，熄灭LED
            ledController.turnOff();
        }

        // 短暂延时，避免CPU占用过高
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
