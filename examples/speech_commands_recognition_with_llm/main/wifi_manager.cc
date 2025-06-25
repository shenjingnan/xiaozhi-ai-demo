#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include <cstring>

static const char *TAG = "WiFiManager";

// 静态成员初始化
EventGroupHandle_t WiFiManager::s_wifi_event_group = NULL;
int WiFiManager::s_retry_num = 0;
esp_ip4_addr_t WiFiManager::s_ip_addr = {0};

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
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < wifi_manager->max_retry_) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "重试连接WiFi... (%d/%d)", s_retry_num, wifi_manager->max_retry_);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "WiFi连接失败");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        s_ip_addr = event->ip_info.ip;
        ESP_LOGI(TAG, "获得IP地址:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t WiFiManager::connect() {
    if (initialized_) {
        ESP_LOGW(TAG, "WiFi已经初始化");
        return ESP_OK;
    }
    
    // 初始化事件组
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "创建事件组失败");
        return ESP_FAIL;
    }
    
    // 初始化TCP/IP栈
    ESP_ERROR_CHECK(esp_netif_init());
    
    // 创建默认事件循环（如果还没创建）
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "创建事件循环失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 创建默认WiFi STA接口
    esp_netif_create_default_wifi_sta();
    
    // 初始化WiFi配置
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // 注册事件处理器
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                       ESP_EVENT_ANY_ID,
                                                       &event_handler,
                                                       this,
                                                       &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                       IP_EVENT_STA_GOT_IP,
                                                       &event_handler,
                                                       this,
                                                       &instance_got_ip_));
    
    // 配置WiFi连接参数
    wifi_config_t wifi_config = {};
    std::strncpy((char*)wifi_config.sta.ssid, ssid_.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy((char*)wifi_config.sta.password, password_.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi初始化完成，正在连接到 %s", ssid_.c_str());
    
    // 等待连接结果
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE,
                                          pdFALSE,
                                          portMAX_DELAY);
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✅ WiFi连接成功: %s", ssid_.c_str());
        initialized_ = true;
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "❌ WiFi连接失败: %s", ssid_.c_str());
        // 清理资源
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        esp_wifi_stop();
        esp_wifi_deinit();
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "意外事件");
        return ESP_FAIL;
    }
}

void WiFiManager::disconnect() {
    if (!initialized_) {
        return;
    }
    
    ESP_LOGI(TAG, "断开WiFi连接...");
    
    // 注销事件处理器
    if (instance_any_id_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }
    
    // 停止WiFi
    esp_wifi_stop();
    esp_wifi_deinit();
    
    // 删除事件组
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    
    initialized_ = false;
    s_retry_num = 0;
    s_ip_addr.addr = 0;
    
    ESP_LOGI(TAG, "WiFi已断开");
}

bool WiFiManager::isConnected() const {
    if (!initialized_ || !s_wifi_event_group) {
        return false;
    }
    
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

std::string WiFiManager::getIpAddress() const {
    if (!isConnected()) {
        return "";
    }
    
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&s_ip_addr));
    return std::string(ip_str);
}

int8_t WiFiManager::getRssi() const {
    if (!isConnected()) {
        return 0;
    }
    
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}