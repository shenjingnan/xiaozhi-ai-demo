#pragma once

#include <string>
#include <functional>
#include "esp_websocket_client.h"
#include "esp_event.h"

// WebSocket事件回调类型
using WebSocketMessageCallback = std::function<void(const uint8_t* data, size_t len)>;
using WebSocketEventCallback = std::function<void(esp_websocket_event_id_t event)>;

// WebSocket客户端类
class WebSocketClient {
private:
    esp_websocket_client_handle_t client;
    WebSocketMessageCallback message_callback;
    WebSocketEventCallback event_callback;
    bool is_connected;

    // 静态事件处理函数
    static void eventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
    
    // 处理WebSocket事件
    void handleEvent(esp_websocket_event_data_t* event);

public:
    WebSocketClient();
    ~WebSocketClient();

    // 初始化WebSocket客户端
    bool init(const std::string& url);
    
    // 连接服务器
    bool connect();
    
    // 断开连接
    void disconnect();
    
    // 发送文本消息
    bool sendText(const std::string& message);
    
    // 发送二进制数据
    bool sendBinary(const uint8_t* data, size_t len);
    
    // 是否已连接
    bool isConnected() const { return is_connected; }
    
    // 设置消息回调
    void setMessageCallback(WebSocketMessageCallback callback) {
        message_callback = std::move(callback);
    }
    
    // 设置事件回调
    void setEventCallback(WebSocketEventCallback callback) {
        event_callback = std::move(callback);
    }
}; 