#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include <string>

/**
 * @brief 📶 WiFi管理器类 - 处理无线网络连接
 * 
 * 这个类就像您的网络管家，负责：
 * - 🔗 连接到指定的WiFi网络
 * - 🔄 连接失败时自动重试
 * - 📊 监控网络状态和信号强度
 * - 🏠 获取分配的IP地址
 * 
 * 使用起来非常简单，只需要提供WiFi名称和密码即可！
 */
class WiFiManager {
public:
    /**
     * @brief 创建WiFi管理器
     * 
     * @param ssid WiFi网络名称（就是您路由器的名字）
     * @param password WiFi密码（连接路由器的密码）
     * @param max_retry 最大重试次数（默认5次，避免一直尝试）
     */
    WiFiManager(const std::string& ssid, const std::string& password, int max_retry = 5);
    
    /**
     * @brief 析构函数
     * 
     * 自动清理资源，断开WiFi连接。
     */
    ~WiFiManager();
    
    /**
     * @brief 🚀 初始化并连接WiFi
     * 
     * 调用这个函数开始连接WiFi，函数会：
     * 1. 初始化WiFi驱动
     * 2. 尝试连接到指定网络
     * 3. 等待获取IP地址
     * 
     * @return ESP_OK=连接成功，ESP_FAIL=连接失败
     */
    esp_err_t connect();
    
    /**
     * @brief 🔌 断开WiFi连接
     * 
     * 主动断开网络连接并释放相关资源。
     */
    void disconnect();
    
    /**
     * @brief 🟢 查询连接状态
     * 
     * 检查当前是否已连接到WiFi网络。
     * 
     * @return true=已连接上网，false=未连接
     */
    bool isConnected() const;
    
    /**
     * @brief 🏠 获取IP地址
     * 
     * 获取路由器分配给ESP32的IP地址。
     * 
     * @return IP地址字符串（如"192.168.1.100"），未连接时返回空字符串
     */
    std::string getIpAddress() const;
    
    /**
     * @brief 📊 获取WiFi信号强度
     * 
     * 信号强度参考：
     * - -30 ~ -50 dBm：信号极好 📶📶📶📶
     * - -50 ~ -70 dBm：信号良好 📶📶📶
     * - -70 ~ -85 dBm：信号一般 📶📶
     * - < -85 dBm：信号较差 📶
     * 
     * @return RSSI值（单位：dBm，负数，越接近0信号越好）
     */
    int8_t getRssi() const;

private:
    // 🔔 WiFi事件处理函数
    // 当WiFi发生事件时（如连接、断开、获得IP等），系统会调用这个函数
    static void event_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data);
    
    // 🔐 配置参数
    std::string ssid_;              // WiFi网络名称
    std::string password_;          // WiFi密码
    int max_retry_;                 // 最大重试次数
    
    // 📊 状态管理
    static EventGroupHandle_t s_wifi_event_group;  // 事件组句柄（用于线程同步）
    static int s_retry_num;                        // 当前重试次数
    static const int WIFI_CONNECTED_BIT = BIT0;    // 连接成功标志位
    static const int WIFI_FAIL_BIT = BIT1;         // 连接失败标志位
    
    // 🟢 状态变量
    bool initialized_;              // 是否已初始化
    
    // 🎟️ 事件处理器句柄
    esp_event_handler_instance_t instance_any_id_;  // 处理所有WiFi事件
    esp_event_handler_instance_t instance_got_ip_;  // 处理获得IP事件
    
    // 🏠 IP地址存储
    static esp_ip4_addr_t s_ip_addr;              // 当前分配的IP地址
};

#endif // WIFI_MANAGER_H