#pragma once

#include <string>
#include <functional>
#include <vector>
#include <atomic>
#include <esp_websocket_client.h>

class WsClient {
public:
    bool start(const std::string& url, const std::string& device_id, const std::string& token);
    void stop();
    bool send_text(const std::string& text);
    bool send_binary(const uint8_t* data, size_t len);
    bool connected() const { return connected_; }

    void on_text(std::function<void(const std::string&)> cb);
    void on_binary(std::function<void(const uint8_t*, size_t)> cb);
    void on_connected(std::function<void()> cb);
    void on_disconnected(std::function<void()> cb);

private:
    static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
    esp_websocket_client_handle_t client_ = nullptr;
    std::function<void(const std::string&)> on_text_;
    std::function<void(const uint8_t*, size_t)> on_binary_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;
    std::atomic<bool> connected_{false};
    std::string headers_;  // Persistent storage for WebSocket headers
};
