/**
 * @file wifi_manager.cc
 * @brief WiFi连接管理类实现
 */

#include "wifi_manager.h"
#include "system_config.h"

extern "C" {
#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>
}

static const char *TAG = "WiFiManager";

// WiFi事件组位定义
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// 静态实例指针，用于事件处理
static WiFiManager* instance = nullptr;

WiFiManager::WiFiManager() : wifi_event_group(nullptr), retry_num(0) {
    instance = this;
}

WiFiManager::~WiFiManager() {
    if (wifi_event_group) {
        vEventGroupDelete(wifi_event_group);
    }
    instance = nullptr;
}

esp_err_t WiFiManager::init() {
    wifi_event_group = xEventGroupCreate();
    if (!wifi_event_group) {
        ESP_LOGE(TAG, "创建WiFi事件组失败");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        this,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        this,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char *)wifi_config.sta.password, WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi初始化完成");

    // 扫描可用网络
    scan_networks();

    /* 等待连接建立或失败 */
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi连接成功，SSID:%s", WIFI_SSID);
        vTaskDelay(pdMS_TO_TICKS(1000)); // 等待网络稳定
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "WiFi连接失败，SSID:%s", WIFI_SSID);
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "WiFi连接异常事件");
        return ESP_FAIL;
    }
}

bool WiFiManager::is_connected() {
    wifi_ap_record_t ap_info;
    return esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
}

void WiFiManager::event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    WiFiManager* manager = static_cast<WiFiManager*>(arg);
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconnected = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGI(TAG, "WiFi断开连接，原因: %d", disconnected->reason);

        if (manager->retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            manager->retry_num++;
            ESP_LOGI(TAG, "重试连接WiFi (第%d次)", manager->retry_num);
        } else {
            ESP_LOGE(TAG, "WiFi连接失败，已达到最大重试次数");
            xEventGroupSetBits(manager->wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "获得IP地址:" IPSTR, IP2STR(&event->ip_info.ip));
        manager->retry_num = 0;
        xEventGroupSetBits(manager->wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void WiFiManager::scan_networks() {
    ESP_LOGI(TAG, "开始扫描WiFi网络...");

    wifi_scan_config_t scan_config = {};
    scan_config.ssid = NULL;
    scan_config.bssid = NULL;
    scan_config.channel = 0;
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 120;
    scan_config.scan_time.active.max = 150;

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi扫描启动失败: %s", esp_err_to_name(ret));
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "发现 %d 个WiFi网络", ap_count);

    if (ap_count > 0) {
        wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (ap_list != NULL) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_list);

            ESP_LOGI(TAG, "可用WiFi网络列表:");
            for (int i = 0; i < ap_count; i++) {
                ESP_LOGI(TAG, "  %d: %s (信号强度: %d dBm, 加密: %d)",
                         i+1, ap_list[i].ssid, ap_list[i].rssi, ap_list[i].authmode);

                // 检查目标网络是否在列表中
                if (strcmp((char*)ap_list[i].ssid, WIFI_SSID) == 0) {
                    ESP_LOGI(TAG, "  ✓ 找到目标网络 '%s'，信号强度: %d dBm",
                             WIFI_SSID, ap_list[i].rssi);
                }
            }
            free(ap_list);
        }
    }
}
