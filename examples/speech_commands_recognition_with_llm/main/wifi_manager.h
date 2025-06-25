#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include <string>

/**
 * @brief WiFi管理器类
 * 
 * 封装ESP32的WiFi连接功能，提供简单易用的接口
 */
class WiFiManager {
public:
    /**
     * @brief 构造函数
     * @param ssid WiFi网络名称
     * @param password WiFi密码
     * @param max_retry 最大重试次数，默认5次
     */
    WiFiManager(const std::string& ssid, const std::string& password, int max_retry = 5);
    
    /**
     * @brief 析构函数
     */
    ~WiFiManager();
    
    /**
     * @brief 初始化并连接WiFi
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t connect();
    
    /**
     * @brief 断开WiFi连接
     */
    void disconnect();
    
    /**
     * @brief 获取连接状态
     * @return true表示已连接，false表示未连接
     */
    bool isConnected() const;
    
    /**
     * @brief 获取IP地址字符串
     * @return IP地址字符串，如果未连接返回空字符串
     */
    std::string getIpAddress() const;
    
    /**
     * @brief 获取WiFi信号强度
     * @return RSSI值（dBm）
     */
    int8_t getRssi() const;

private:
    // WiFi事件处理器
    static void event_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data);
    
    // 配置参数
    std::string ssid_;
    std::string password_;
    int max_retry_;
    
    // 状态变量
    static EventGroupHandle_t s_wifi_event_group;
    static int s_retry_num;
    static const int WIFI_CONNECTED_BIT = BIT0;
    static const int WIFI_FAIL_BIT = BIT1;
    
    // 是否已初始化
    bool initialized_;
    
    // 事件处理器实例
    esp_event_handler_instance_t instance_any_id_;
    esp_event_handler_instance_t instance_got_ip_;
    
    // IP地址
    static esp_ip4_addr_t s_ip_addr;
};

#endif // WIFI_MANAGER_H