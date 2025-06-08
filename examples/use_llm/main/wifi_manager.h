/**
 * @file wifi_manager.h
 * @brief WiFi连接管理类
 */

#pragma once

extern "C" {
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/event_groups.h"
}

class WiFiManager {
public:
    WiFiManager();
    ~WiFiManager();
    
    /**
     * @brief 初始化WiFi连接
     * @return esp_err_t 
     */
    esp_err_t init();
    
    /**
     * @brief 检查WiFi连接状态
     * @return true 已连接
     * @return false 未连接
     */
    bool is_connected();

private:
    EventGroupHandle_t wifi_event_group;
    int retry_num;
    
    /**
     * @brief WiFi事件处理函数
     */
    static void event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data);
    
    /**
     * @brief 扫描可用的WiFi网络
     */
    void scan_networks();
};
