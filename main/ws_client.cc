#include "ws_client.h"

#include <esp_log.h>

static const char* TAG = "ws_client";

void WsClient::ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    auto self = static_cast<WsClient*>(handler_args);
    auto *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            self->connected_ = true;
            if (self->on_connected_) self->on_connected_();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WebSocket disconnected");
            self->connected_ = false;
            if (self->on_disconnected_) self->on_disconnected_();
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "WebSocket error");
            self->connected_ = false;
            if (self->on_disconnected_) self->on_disconnected_();
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == WS_TRANSPORT_OPCODES_TEXT && self->on_text_) {
                self->on_text_(std::string((const char*)data->data_ptr, data->data_len));
            } else if (data->op_code == WS_TRANSPORT_OPCODES_BINARY && self->on_binary_) {
                self->on_binary_((const uint8_t*)data->data_ptr, data->data_len);
            }
            break;
        default:
            break;
    }
}

bool WsClient::start(const std::string& url, const std::string& device_id, const std::string& token) {
    // Store headers persistently to avoid use-after-free
    // MUST set before init
    headers_ = "x-device-id: " + device_id + "\r\n" +
               "x-device-token: " + token + "\r\n";

    esp_websocket_client_config_t cfg = {};
    cfg.uri = url.c_str();
    cfg.headers = headers_.c_str();  // Set headers in config
    // Avoid disconnect if no data within default 10s
    cfg.network_timeout_ms = 60000;
    cfg.reconnect_timeout_ms = 10000;

    // Enable TCP keepalive (reference: xiaozhi-esp32)
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 5;        // Start keepalive after 5s idle
    cfg.keep_alive_interval = 5;    // Send keepalive every 5s
    cfg.keep_alive_count = 3;       // Retry 3 times before timeout

    // Disable WebSocket ping/pong - let server handle keepalive via TCP
    cfg.ping_interval_sec = 0;
    cfg.pingpong_timeout_sec = 0;

    // CRITICAL: Increase stack size for Opus decoding in callbacks
    // Default 4KB is insufficient - Opus decoder needs ~8-12KB
    cfg.task_stack = 16384;  // 16KB stack for WebSocket task
    cfg.task_prio = 5;       // Priority 5 (default)

    client_ = esp_websocket_client_init(&cfg);
    if (!client_) {
        ESP_LOGE(TAG, "Failed to init websocket client");
        return false;
    }

    esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, ws_event_handler, this);

    if (esp_websocket_client_start(client_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start websocket client");
        return false;
    }

    return true;
}

void WsClient::stop() {
    if (client_) {
        esp_websocket_client_stop(client_);
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
    }
}

bool WsClient::send_text(const std::string& text) {
    if (!client_) return false;
    if (!esp_websocket_client_is_connected(client_)) {
        return false;
    }
    int ret = esp_websocket_client_send_text(client_, text.c_str(), text.size(), 1000 / portTICK_PERIOD_MS);
    return ret >= 0;
}

bool WsClient::send_binary(const uint8_t* data, size_t len) {
    if (!client_) return false;
    if (!esp_websocket_client_is_connected(client_)) {
        return false;
    }
    int ret = esp_websocket_client_send_bin(client_, (const char*)data, len, 1000 / portTICK_PERIOD_MS);
    return ret >= 0;
}

void WsClient::on_text(std::function<void(const std::string&)> cb) {
    on_text_ = cb;
}

void WsClient::on_binary(std::function<void(const uint8_t*, size_t)> cb) {
    on_binary_ = cb;
}

void WsClient::on_connected(std::function<void()> cb) {
    on_connected_ = cb;
}

void WsClient::on_disconnected(std::function<void()> cb) {
    on_disconnected_ = cb;
}
