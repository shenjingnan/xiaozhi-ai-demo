/**
 * @file wifi_manager.cc
 * @brief 📶 WiFi管理器实现文件 - 让ESP32轻松连上互联网
 * 
 * 这个文件实现了WiFi连接的全部逻辑，包括：
 * - 🔍 扫描和连接WiFi网络
 * - 🔄 连接失败后自动重试
 * - 🏠 获取DHCP分配的IP地址
 * - 📊 监控信号强度
 * 
 * 开发提示：请确保路由器开启了2.4GHz频段，ESP32不支持5GHz！
 */

#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include <cstring>

static const char *TAG = "WiFiManager";

// 🎯 静态成员初始化（这些变量在所有WiFiManager实例之间共享）
EventGroupHandle_t WiFiManager::s_wifi_event_group = NULL;  // 事件组句柄
int WiFiManager::s_retry_num = 0;                          // 当前重试次数
esp_ip4_addr_t WiFiManager::s_ip_addr = {0};               // IP地址结构体

WiFiManager::WiFiManager(const std::string& ssid, const std::string& password, int max_retry)
    : ssid_(ssid), password_(password), max_retry_(max_retry), initialized_(false),
      instance_any_id_(nullptr), instance_got_ip_(nullptr) {
}

WiFiManager::~WiFiManager() {
    if (initialized_) {
        disconnect();
    }
}

void WiFiManager::event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    WiFiManager* wifi_manager = static_cast<WiFiManager*>(arg);
    
    // 🟢 WiFi驱动启动完成，开始连接
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } 
    // 🔴 WiFi连接断开（可能是密码错误、信号太弱等）
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < wifi_manager->max_retry_) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "🔄 重试连接WiFi... (%d/%d)", s_retry_num, wifi_manager->max_retry_);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "❌ WiFi连接失败");
    } 
    // 🎉 成功获得IP地址，可以上网了！
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        s_ip_addr = event->ip_info.ip;
        ESP_LOGI(TAG, "🏠 获得IP地址:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;  // 重置重试计数器
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);  // 设置连接成功标志
    }
}

esp_err_t WiFiManager::connect() {
    if (initialized_) {
        ESP_LOGW(TAG, "⚠️ WiFi已经初始化");
        return ESP_OK;
    }
    
    // 🎯 创建事件组（用于等待WiFi连接结果）
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "❌ 创建事件组失败");
        return ESP_FAIL;
    }
    
    // 🌐 初始化TCP/IP协议栈（让ESP32能够使用网络）
    ESP_ERROR_CHECK(esp_netif_init());
    
    // 🔁 创建事件循环（用于处理各种系统事件）
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "❌ 创建事件循环失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 📡 创建默认WiFi STA接口（STA=Station，即WiFi客户端模式）
    esp_netif_create_default_wifi_sta();
    
    // 🔧 初始化WiFi驱动（使用默认配置）
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // 🔔 注册事件处理函数
    // 当WiFi发生任何事件时，都会通知我们
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                       ESP_EVENT_ANY_ID,    // 监听所有WiFi事件
                                                       &event_handler,
                                                       this,
                                                       &instance_any_id_));
    // 当获得IP地址时，也会通知我们
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                       IP_EVENT_STA_GOT_IP, // 监听获得IP事件
                                                       &event_handler,
                                                       this,
                                                       &instance_got_ip_));
    
    // 🔐 配置WiFi连接参数
    wifi_config_t wifi_config = {};
    // 复制WiFi名称（最多32个字符）
    std::strncpy((char*)wifi_config.sta.ssid, ssid_.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    // 复制WiFi密码（最多64个字符）
    std::strncpy((char*)wifi_config.sta.password, password_.c_str(), sizeof(wifi_config.sta.password) - 1);
    // 设置加密方式（至少WPA2，更安全）
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    // 支持WPA3加密（更高级的安全性）
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    
    // 🚀 设置WiFi工作模式并启动
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));      // 设为客户端模式
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));  // 应用配置
    ESP_ERROR_CHECK(esp_wifi_start());                      // 启动WiFi
    
    ESP_LOGI(TAG, "📶 WiFi初始化完成，正在连接到 %s", ssid_.c_str());
    
    // ⏳ 等待连接结果（会阻塞在这里直到连接成功或失败）
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,  // 等待这两个事件之一
                                          pdFALSE,                             // 不清除事件位
                                          pdFALSE,                             // 不需要两个事件都发生
                                          portMAX_DELAY);                      // 永久等待
    
    // 🎯 检查连接结果
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✅ WiFi连接成功: %s", ssid_.c_str());
        initialized_ = true;
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "❌ WiFi连接失败: %s", ssid_.c_str());
        ESP_LOGI(TAG, "💡 提示：请检查WiFi名称和密码是否正确！");
        
        // 🧹 清理资源（释放内存，恢复状态）
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        esp_wifi_stop();
        esp_wifi_deinit();
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "⚠️ 意外事件");
        return ESP_FAIL;
    }
}

void WiFiManager::disconnect() {
    if (!initialized_) {
        return;
    }
    
    ESP_LOGI(TAG, "🔌 断开WiFi连接...");
    
    // 🔔 注销事件处理器（不再监听WiFi事件）
    if (instance_any_id_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }
    
    // 🛑️ 停止WiFi驱动
    esp_wifi_stop();
    esp_wifi_deinit();
    
    // 🧹 删除事件组（释放内存）
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    
    // 🔄 重置所有状态变量
    initialized_ = false;
    s_retry_num = 0;
    s_ip_addr.addr = 0;
    
    ESP_LOGI(TAG, "✅ WiFi已完全断开");
}

bool WiFiManager::isConnected() const {
    if (!initialized_ || !s_wifi_event_group) {
        return false;
    }
    
    // 🔍 检查事件组中的连接标志位
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

std::string WiFiManager::getIpAddress() const {
    if (!isConnected()) {
        return "";
    }
    
    // 🏠 将IP地址结构体转换为可读字符串
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&s_ip_addr));
    return std::string(ip_str);
}

int8_t WiFiManager::getRssi() const {
    if (!isConnected()) {
        return 0;
    }
    
    // 📊 获取当前连接的AP（接入点）信息
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;  // 返回信号强度（负数，越接近0信号越好）
    }
    return 0;
}