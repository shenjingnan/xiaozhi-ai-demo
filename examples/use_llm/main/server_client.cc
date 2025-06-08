/**
 * @file server_client.cc
 * @brief 服务端通信客户端类实现
 */

#include "server_client.h"
#include "system_config.h"

extern "C" {
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <string.h>
#include <stdlib.h>
}

static const char *TAG = "ServerClient";

ServerClient::ServerClient() {
}

ServerClient::~ServerClient() {
}

esp_err_t ServerClient::check_wifi_connection() {
    // 首先检查WiFi连接状态
    wifi_ap_record_t ap_info;
    esp_err_t wifi_ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (wifi_ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi未连接，无法发送请求: %s", esp_err_to_name(wifi_ret));
        return ESP_ERR_WIFI_NOT_CONNECT;
    }

    // 获取IP地址信息
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "当前IP地址: " IPSTR, IP2STR(&ip_info.ip));
        ESP_LOGI(TAG, "网关地址: " IPSTR, IP2STR(&ip_info.gw));
        ESP_LOGI(TAG, "子网掩码: " IPSTR, IP2STR(&ip_info.netmask));
    }

    return ESP_OK;
}

esp_err_t ServerClient::send_audio(int16_t *audio_data, size_t audio_len, 
                                  uint8_t **response_audio, size_t *response_len) {
    esp_err_t ret = check_wifi_connection();
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "尝试连接服务器: %s", SERVER_URL);

    // 分配响应缓冲区
    uint8_t *response_buffer = (uint8_t *)malloc(1024 * 1024); // 1MB缓冲区
    if (response_buffer == NULL) {
        ESP_LOGE(TAG, "响应缓冲区内存分配失败");
        return ESP_ERR_NO_MEM;
    }

    // 配置HTTP客户端
    esp_http_client_config_t config = {};
    config.url = SERVER_URL;
    config.method = HTTP_METHOD_POST;
    config.event_handler = http_event_handler;
    config.user_data = response_buffer;
    config.timeout_ms = 30000; // 30秒超时

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP客户端初始化失败");
        free(response_buffer);
        return ESP_FAIL;
    }

    // 设置请求头
    esp_http_client_set_header(client, "Content-Type", "multipart/form-data; boundary=----WebKitFormBoundary7MA4YWxkTrZu0gW");

    // 构建multipart/form-data请求体
    const char *boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    char header_part[512];
    char footer_part[128];
    int wlen;
    int content_length;
    int status_code;
    int data_read;

    snprintf(header_part, sizeof(header_part),
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"audio\"; filename=\"audio.pcm\"\r\n"
             "Content-Type: application/octet-stream\r\n\r\n",
             boundary);

    snprintf(footer_part, sizeof(footer_part), "\r\n--%s--\r\n", boundary);

    size_t total_len = strlen(header_part) + (audio_len * sizeof(int16_t)) + strlen(footer_part);

    ESP_LOGI(TAG, "发送音频数据到服务端，大小: %zu 字节", audio_len * sizeof(int16_t));

    // 设置请求体长度
    esp_http_client_set_post_field(client, NULL, total_len);

    // 开始请求
    ret = esp_http_client_open(client, total_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP客户端打开失败: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    // 发送header部分
    wlen = esp_http_client_write(client, header_part, strlen(header_part));
    if (wlen < 0) {
        ESP_LOGE(TAG, "发送HTTP头部失败");
        ret = ESP_FAIL;
        goto cleanup;
    }

    // 发送音频数据
    wlen = esp_http_client_write(client, (char *)audio_data, audio_len * sizeof(int16_t));
    if (wlen < 0) {
        ESP_LOGE(TAG, "发送音频数据失败");
        ret = ESP_FAIL;
        goto cleanup;
    }

    // 发送footer部分
    wlen = esp_http_client_write(client, footer_part, strlen(footer_part));
    if (wlen < 0) {
        ESP_LOGE(TAG, "发送HTTP尾部失败");
        ret = ESP_FAIL;
        goto cleanup;
    }

    // 获取响应
    content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "获取HTTP响应头失败");
        ret = ESP_FAIL;
        goto cleanup;
    }

    status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "服务端返回错误状态码: %d", status_code);
        ret = ESP_FAIL;
        goto cleanup;
    }

    // 读取响应数据
    data_read = esp_http_client_read_response(client, (char *)response_buffer, 1024 * 1024);
    if (data_read < 0) {
        ESP_LOGE(TAG, "读取HTTP响应失败");
        ret = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG, "从服务端接收到音频响应，大小: %d 字节", data_read);

    *response_audio = response_buffer;
    *response_len = data_read;

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK && response_buffer) {
        free(response_buffer);
    }

    return ret;
}

esp_err_t ServerClient::http_event_handler(esp_http_client_event_t *evt) {
    static int output_len = 0;

    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (!esp_http_client_is_chunked_response(evt->client)) {
            if (evt->user_data) {
                memcpy((char*)evt->user_data + output_len, evt->data, evt->data_len);
                output_len += evt->data_len;
            }
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        output_len = 0;
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}
