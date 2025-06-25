#ifndef WEBSOCKET_CLIENT_H
#define WEBSOCKET_CLIENT_H

#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string>
#include <functional>

/**
 * @brief WebSocket客户端类
 * 
 * 封装ESP32的WebSocket连接功能，提供事件回调和自动重连功能
 */
class WebSocketClient {
public:
    /**
     * @brief WebSocket事件类型
     */
    enum class EventType {
        CONNECTED,
        DISCONNECTED,
        DATA_TEXT,
        DATA_BINARY,
        PING,
        PONG,
        ERROR
    };
    
    /**
     * @brief WebSocket事件数据
     */
    struct EventData {
        EventType type;
        const uint8_t* data;
        size_t data_len;
        int op_code;
    };
    
    /**
     * @brief 事件回调函数类型
     */
    using EventCallback = std::function<void(const EventData&)>;
    
    /**
     * @brief 构造函数
     * @param uri WebSocket服务器URI
     * @param auto_reconnect 是否自动重连，默认true
     * @param reconnect_interval_ms 重连间隔（毫秒），默认5000ms
     */
    WebSocketClient(const std::string& uri, bool auto_reconnect = true, 
                   int reconnect_interval_ms = 5000);
    
    /**
     * @brief 析构函数
     */
    ~WebSocketClient();
    
    /**
     * @brief 设置事件回调函数
     * @param callback 回调函数
     */
    void setEventCallback(EventCallback callback);
    
    /**
     * @brief 连接到WebSocket服务器
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t connect();
    
    /**
     * @brief 断开WebSocket连接
     */
    void disconnect();
    
    /**
     * @brief 发送文本数据
     * @param text 要发送的文本
     * @param timeout_ms 超时时间（毫秒）
     * @return 实际发送的字节数，-1表示失败
     */
    int sendText(const std::string& text, int timeout_ms = portMAX_DELAY);
    
    /**
     * @brief 发送二进制数据
     * @param data 要发送的数据
     * @param len 数据长度
     * @param timeout_ms 超时时间（毫秒）
     * @return 实际发送的字节数，-1表示失败
     */
    int sendBinary(const uint8_t* data, size_t len, int timeout_ms = portMAX_DELAY);
    
    /**
     * @brief 发送ping包
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t sendPing();
    
    /**
     * @brief 获取连接状态
     * @return true表示已连接，false表示未连接
     */
    bool isConnected() const { return connected_; }
    
    /**
     * @brief 设置是否自动重连
     * @param enable true启用自动重连，false禁用
     */
    void setAutoReconnect(bool enable) { auto_reconnect_ = enable; }
    
    /**
     * @brief 设置重连间隔
     * @param interval_ms 重连间隔（毫秒）
     */
    void setReconnectInterval(int interval_ms) { reconnect_interval_ms_ = interval_ms; }

private:
    // WebSocket事件处理器
    static void websocket_event_handler(void* handler_args, esp_event_base_t base, 
                                      int32_t event_id, void* event_data);
    
    // 重连任务
    static void reconnect_task(void* arg);
    
    // 配置参数
    std::string uri_;
    bool auto_reconnect_;
    int reconnect_interval_ms_;
    
    // WebSocket客户端句柄
    esp_websocket_client_handle_t client_;
    
    // 状态变量
    bool connected_;
    
    // 重连任务句柄
    TaskHandle_t reconnect_task_handle_;
    
    // 事件回调
    EventCallback event_callback_;
    
    // 缓冲区大小和任务栈大小
    static constexpr int BUFFER_SIZE = 8192;
    static constexpr int TASK_STACK_SIZE = 8192;
    static constexpr int RECONNECT_TASK_STACK_SIZE = 4096;
};

#endif // WEBSOCKET_CLIENT_H