#include "websocket_client.h"
#include "esp_log.h"
#include <cstring>

static const char *TAG = "WebSocketClient";

WebSocketClient::WebSocketClient(const std::string& uri, bool auto_reconnect, 
                               int reconnect_interval_ms)
    : uri_(uri), auto_reconnect_(auto_reconnect), 
      reconnect_interval_ms_(reconnect_interval_ms),
      client_(nullptr), connected_(false), reconnect_task_handle_(nullptr) {
}

WebSocketClient::~WebSocketClient() {
    disconnect();
}

void WebSocketClient::setEventCallback(EventCallback callback) {
    event_callback_ = callback;
}

void WebSocketClient::websocket_event_handler(void* handler_args, esp_event_base_t base, 
                                             int32_t event_id, void* event_data) {
    WebSocketClient* ws_client = static_cast<WebSocketClient*>(handler_args);
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;
    
    EventData event;
    event.data = nullptr;
    event.data_len = 0;
    event.op_code = 0;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "🔗 WebSocket已连接");
            ws_client->connected_ = true;
            event.type = EventType::CONNECTED;
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "🔌 WebSocket已断开");
            ws_client->connected_ = false;
            event.type = EventType::DISCONNECTED;
            break;
            
        case WEBSOCKET_EVENT_DATA:
            ESP_LOGD(TAG, "收到WebSocket数据，长度: %d 字节, op_code: 0x%02x", 
                    data->data_len, data->op_code);
            event.data = (const uint8_t*)data->data_ptr;
            event.data_len = data->data_len;
            event.op_code = data->op_code;
            
            // 根据操作码确定事件类型
            if (data->op_code == 0x01) { // 文本数据
                event.type = EventType::DATA_TEXT;
            } else if (data->op_code == 0x02) { // 二进制数据
                event.type = EventType::DATA_BINARY;
            } else if (data->op_code == 0x09) { // Ping
                event.type = EventType::PING;
            } else if (data->op_code == 0x0A) { // Pong
                event.type = EventType::PONG;
            } else {
                event.type = EventType::DATA_BINARY; // 默认作为二进制处理
            }
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGI(TAG, "❌ WebSocket错误");
            ws_client->connected_ = false;
            event.type = EventType::ERROR;
            break;
            
        default:
            return;
    }
    
    // 调用用户回调
    if (ws_client->event_callback_) {
        ws_client->event_callback_(event);
    }
}

void WebSocketClient::reconnect_task(void* arg) {
    WebSocketClient* ws_client = static_cast<WebSocketClient*>(arg);
    
    while (1) {
        if (!ws_client->connected_ && ws_client->client_ != nullptr && ws_client->auto_reconnect_) {
            ESP_LOGI(TAG, "尝试重新连接WebSocket...");
            esp_websocket_client_stop(ws_client->client_);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_websocket_client_start(ws_client->client_);
        }
        vTaskDelay(pdMS_TO_TICKS(ws_client->reconnect_interval_ms_));
    }
}

esp_err_t WebSocketClient::connect() {
    if (client_ != nullptr) {
        ESP_LOGW(TAG, "WebSocket客户端已存在");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "正在连接WebSocket服务器: %s", uri_.c_str());
    
    // 配置WebSocket客户端
    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = uri_.c_str();
    ws_cfg.buffer_size = BUFFER_SIZE;
    ws_cfg.task_stack = TASK_STACK_SIZE;
    ws_cfg.reconnect_timeout_ms = 10000;  // 10秒重连超时
    ws_cfg.network_timeout_ms = 10000;     // 10秒网络超时
    
    // 初始化WebSocket客户端
    client_ = esp_websocket_client_init(&ws_cfg);
    if (client_ == nullptr) {
        ESP_LOGE(TAG, "WebSocket客户端初始化失败");
        return ESP_FAIL;
    }
    
    // 注册事件处理器
    esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, websocket_event_handler, this);
    
    // 启动WebSocket客户端
    esp_err_t ret = esp_websocket_client_start(client_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket客户端启动失败: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
        return ret;
    }
    
    // 创建重连任务（如果启用了自动重连）
    if (auto_reconnect_ && reconnect_task_handle_ == nullptr) {
        xTaskCreate(reconnect_task, "ws_reconnect", RECONNECT_TASK_STACK_SIZE, 
                   this, 5, &reconnect_task_handle_);
        ESP_LOGI(TAG, "WebSocket重连任务已创建");
    }
    
    return ESP_OK;
}

void WebSocketClient::disconnect() {
    // 停止重连任务
    if (reconnect_task_handle_ != nullptr) {
        vTaskDelete(reconnect_task_handle_);
        reconnect_task_handle_ = nullptr;
        ESP_LOGI(TAG, "WebSocket重连任务已停止");
    }
    
    // 停止并销毁WebSocket客户端
    if (client_ != nullptr) {
        ESP_LOGI(TAG, "正在断开WebSocket连接...");
        esp_websocket_client_stop(client_);
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
        connected_ = false;
        ESP_LOGI(TAG, "✅ WebSocket已断开");
    }
}

int WebSocketClient::sendText(const std::string& text, int timeout_ms) {
    if (client_ == nullptr || !connected_) {
        ESP_LOGW(TAG, "WebSocket未连接，无法发送文本");
        return -1;
    }
    
    int len = esp_websocket_client_send_text(client_, text.c_str(), text.length(), 
                                            timeout_ms / portTICK_PERIOD_MS);
    if (len < 0) {
        ESP_LOGE(TAG, "发送文本失败");
    } else {
        ESP_LOGD(TAG, "发送文本成功: %d 字节", len);
    }
    
    return len;
}

int WebSocketClient::sendBinary(const uint8_t* data, size_t len, int timeout_ms) {
    if (client_ == nullptr || !connected_) {
        ESP_LOGW(TAG, "WebSocket未连接，无法发送二进制数据");
        return -1;
    }
    
    int sent = esp_websocket_client_send_bin(client_, (const char*)data, len, 
                                            timeout_ms / portTICK_PERIOD_MS);
    if (sent < 0) {
        ESP_LOGE(TAG, "发送二进制数据失败");
    } else {
        ESP_LOGD(TAG, "发送二进制数据成功: %d 字节", sent);
    }
    
    return sent;
}

esp_err_t WebSocketClient::sendPing() {
    if (client_ == nullptr || !connected_) {
        ESP_LOGW(TAG, "WebSocket未连接，无法发送ping");
        return ESP_ERR_INVALID_STATE;
    }
    
    // ESP-IDF的WebSocket客户端会自动处理ping/pong
    // 这里可以手动发送ping包
    return ESP_OK;
}