/**
 * @file server_client.h
 * @brief 服务端通信客户端类
 */

#pragma once

extern "C" {
#include "esp_http_client.h"
}

class ServerClient {
public:
    ServerClient();
    ~ServerClient();
    
    /**
     * @brief 发送音频数据到服务端并获取回复音频
     * @param audio_data 音频数据
     * @param audio_len 音频数据长度（样本数）
     * @param response_audio 返回的音频数据指针
     * @param response_len 返回的音频数据长度
     * @return esp_err_t 
     */
    esp_err_t send_audio(int16_t *audio_data, size_t audio_len, 
                        uint8_t **response_audio, size_t *response_len);

private:
    /**
     * @brief HTTP事件处理函数
     */
    static esp_err_t http_event_handler(esp_http_client_event_t *evt);
    
    /**
     * @brief 检查WiFi连接状态
     * @return esp_err_t 
     */
    esp_err_t check_wifi_connection();
};
