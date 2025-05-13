#include "websocket_client.h"
#include "esp_log.h"

static const char* TAG = "WS_CLIENT";

// 构造函数
WebSocketClient::WebSocketClient()
    : client(nullptr), is_connected(false) {
}

// 析构函数
WebSocketClient::~WebSocketClient() {
    disconnect();
}

// 初始化WebSocket客户端
bool WebSocketClient::init(const std::string& url) {
    ESP_LOGI(TAG, "初始化WebSocket客户端, URL: %s", url.c_str());
    
    // 创建WebSocket客户端配置
    esp_websocket_client_config_t config = {};
    config.uri = url.c_str();
    
    // 创建WebSocket客户端实例
    client = esp_websocket_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "初始化WebSocket客户端失败");
        return false;
    }
    
    // 注册事件处理函数
    ESP_ERROR_CHECK(esp_websocket_register_events(
        client,
        WEBSOCKET_EVENT_ANY,
        eventHandler,
        this));
    
    ESP_LOGI(TAG, "WebSocket客户端初始化完成");
    return true;
}

// 连接服务器
bool WebSocketClient::connect() {
    if (!client) {
        ESP_LOGE(TAG, "WebSocket客户端未初始化");
        return false;
    }
    
    ESP_LOGI(TAG, "连接WebSocket服务器");
    esp_err_t err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动WebSocket客户端失败: %s", esp_err_to_name(err));
        return false;
    }
    
    return true;
}

// 断开连接
void WebSocketClient::disconnect() {
    if (client) {
        ESP_LOGI(TAG, "断开WebSocket连接");
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        client = nullptr;
    }
    is_connected = false;
}

// 发送文本消息
bool WebSocketClient::sendText(const std::string& message) {
    if (!client || !is_connected) {
        ESP_LOGE(TAG, "WebSocket未连接");
        return false;
    }
    
    esp_err_t err = esp_websocket_client_send_text(
        client,
        message.c_str(),
        message.length(),
        portMAX_DELAY);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "发送WebSocket文本消息失败: %s", esp_err_to_name(err));
        return false;
    }
    
    return true;
}

// 发送二进制数据
bool WebSocketClient::sendBinary(const uint8_t* data, size_t len) {
    if (!client || !is_connected) {
        ESP_LOGE(TAG, "WebSocket未连接");
        return false;
    }
    
    esp_err_t err = esp_websocket_client_send_bin(
        client,
        reinterpret_cast<const char*>(data),
        len,
        portMAX_DELAY);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "发送WebSocket二进制消息失败: %s", esp_err_to_name(err));
        return false;
    }
    
    return true;
}

// 静态事件处理函数
void WebSocketClient::eventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    WebSocketClient* self = static_cast<WebSocketClient*>(handler_args);
    esp_websocket_event_data_t* event = static_cast<esp_websocket_event_data_t*>(event_data);
    self->handleEvent(event);
}

// 处理WebSocket事件
void WebSocketClient::handleEvent(esp_websocket_event_data_t* event) {
    switch (event->op_code) {
        case WS_TRANSPORT_OPCODES_TEXT:
            ESP_LOGI(TAG, "收到文本消息, 长度: %d", event->data_len);
            break;
            
        case WS_TRANSPORT_OPCODES_BINARY:
            ESP_LOGI(TAG, "收到二进制消息, 长度: %d", event->data_len);
            if (message_callback) {
                message_callback(
                    reinterpret_cast<const uint8_t*>(event->data_ptr),
                    event->data_len);
            }
            break;
            
        case WS_TRANSPORT_OPCODES_CLOSE:
            ESP_LOGI(TAG, "收到关闭消息");
            is_connected = false;
            break;
            
        default:
            ESP_LOGI(TAG, "收到其他类型消息, opcode: %d", event->op_code);
            break;
    }
    
    // 处理连接状态事件
    if (event->event_id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "WebSocket已连接");
        is_connected = true;
        if (event_callback) {
            event_callback(WEBSOCKET_EVENT_CONNECTED);
        }
    } else if (event->event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "WebSocket已断开连接");
        is_connected = false;
        if (event_callback) {
            event_callback(WEBSOCKET_EVENT_DISCONNECTED);
        }
    } else if (event->event_id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGE(TAG, "WebSocket错误");
        if (event_callback) {
            event_callback(WEBSOCKET_EVENT_ERROR);
        }
    }
} 