#include "task_manager.h"
#include "led_controller.h"
#include "system_monitor.h"
#include "lvgl_ui.h"
#include "config.h"
#include "ota_update.h"
#include <esp_log.h>
#include <esp_websocket_client.h>
#include <cJSON.h>
#include <string.h>
#include <math.h>
#include <esp_mac.h>

static const char* TAG = "main_ctrl";

// FSM状态
typedef enum {
    FSM_STATE_IDLE,       // WiFi+WS已连接，等待唤醒词
    FSM_STATE_RECORDING,  // 唤醒后录音
    FSM_STATE_SPEAKING,   // TTS播放
    FSM_STATE_MUSIC,      // 音乐播放（无超时，支持暂停/恢复）
    FSM_STATE_ERROR,      // 网络错误
} fsm_state_t;

// 当前FSM状态（非static，WebSocket事件处理器需要访问作为状态守卫）
fsm_state_t g_current_fsm_state = FSM_STATE_IDLE;

// WebSocket客户端句柄和连接状态
// g_ws_client 非static：OTA模块需要在下载前关闭WS释放WiFi buffer
esp_websocket_client_handle_t g_ws_client = nullptr;
bool g_ws_connected = false;  // 非static，允许audio_main_task和OTA访问
static bool g_audio_start_sent = false;  // 追踪audio_start/listen(start)是否成功发送
static bool g_tts_end_received = false;  // 追踪tts_end是否已收到（等待队列排空）
static uint32_t g_speaking_start_time = 0;  // SPEAKING模式进入时间（用于超时检测）
static char g_session_id[16] = {0};  // 服务器分配的会话ID
static bool g_hello_acked = false;  // hello握手是否完成
static uint32_t g_drain_wait_count = 0;  // SPEAKING→IDLE队列排空计数
static int g_reconnect_attempts = 0;    // 指数退避重连计数（连接成功时重置）
static bool g_auto_listen_enabled = false;  // TTS结束后回到IDLE等待唤醒词（true会导致噪音循环）
static bool g_music_was_playing = false;    // 音乐因唤醒中断后标记，TTS结束后恢复音乐
static uint32_t g_thinking_start_time = 0;  // IDLE(Thinking)模式进入时间（用于超时重置UI）
static uint32_t g_recording_start_time = 0; // [S0-2] 录音FSM开始时间（15s超时保护）

// TTS binary packet counters (file-level for reset on tts_start)
static uint32_t g_tts_rx_count = 0;    // TTS packets received this session
static uint32_t g_tts_drop_count = 0;  // TTS packets dropped this session

// [S0-5] 从芯片MAC自动生成设备ID和Token
static char g_device_id[24] = {0};    // "hitony-AABBCCDDEEFF"
static char g_device_token[20] = {0}; // MAC反转hex作为简单token

static void init_device_identity() {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(g_device_id, sizeof(g_device_id), "hitony-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    // Token: 反转MAC字节序 + XOR混淆，简单但唯一
    snprintf(g_device_token, sizeof(g_device_token), "%02X%02X%02X%02X%02X%02X",
             mac[5] ^ 0xA5, mac[4] ^ 0x5A, mac[3] ^ 0xA5,
             mac[2] ^ 0x5A, mac[1] ^ 0xA5, mac[0] ^ 0x5A);
    ESP_LOGI(TAG, "Device ID: %s, Token: %s", g_device_id, g_device_token);
}

// Forward declaration
static void websocket_event_handler(void* handler_args, esp_event_base_t base,
                                     int32_t event_id, void* event_data);

/**
 * @brief 销毁并重建WebSocket客户端（用于auto-reconnect失败时的硬重连）
 */
static void ws_recreate_client() {
    ESP_LOGW(TAG, "Recreating WebSocket client...");

    if (g_ws_client) {
        esp_websocket_client_stop(g_ws_client);
        esp_websocket_client_destroy(g_ws_client);
        g_ws_client = nullptr;
    }
    g_ws_connected = false;
    g_hello_acked = false;
    g_session_id[0] = '\0';

    static char ws_headers[128];
    snprintf(ws_headers, sizeof(ws_headers),
             "x-device-id: %s\r\nx-device-token: %s\r\n",
             g_device_id, g_device_token);

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = HITONY_WS_URL;
    ws_cfg.headers = ws_headers;
    ws_cfg.task_stack = 4096;           // 4KB (瘦回调不做cJSON，足够)
    ws_cfg.buffer_size = 8192;          // 8KB接收缓冲（容纳多帧TTS数据）
    ws_cfg.disable_auto_reconnect = true; // 禁用库自动重连（FSM管理重连）
    ws_cfg.network_timeout_ms = 10000;
    ws_cfg.ping_interval_sec = 0;       // 禁用WS ping（改用TCP keepalive）
    ws_cfg.pingpong_timeout_sec = 0;    // 禁用WS pong超时
    ws_cfg.keep_alive_enable = true;    // TCP keepalive（内核处理，不受应用阻塞影响）
    ws_cfg.keep_alive_idle = 10;        // 10s空闲开始探测（服务器ASR+LLM需5-10s）
    ws_cfg.keep_alive_interval = 5;     // 每5s探测
    ws_cfg.keep_alive_count = 3;        // 3次失败=断连（总超时：10+5*3=25s）

    g_ws_client = esp_websocket_client_init(&ws_cfg);
    if (!g_ws_client) {
        ESP_LOGE(TAG, "Failed to recreate WebSocket client");
        return;
    }

    esp_websocket_register_events(g_ws_client, WEBSOCKET_EVENT_ANY,
                                  websocket_event_handler, nullptr);

    ESP_LOGI(TAG, "Reconnecting to %s", HITONY_WS_URL);
    esp_err_t ret = esp_websocket_client_start(g_ws_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %d", ret);
        esp_websocket_client_destroy(g_ws_client);
        g_ws_client = nullptr;
    } else {
        ESP_LOGI(TAG, "WebSocket client recreated, waiting for connection...");
    }
}

/**
 * @brief 清空播放队列（释放所有待播放的Opus包）
 */
static void flush_playback_queue() {
    opus_packet_msg_t* msg = nullptr;
    int flushed = 0;
    while (xQueueReceive(g_opus_playback_queue, &msg, 0) == pdTRUE) {
        free_opus_msg(msg);
        flushed++;
    }
    if (flushed > 0) {
        ESP_LOGI(TAG, "Flushed %d packets from playback queue", flushed);
    }
}

/**
 * @brief 发送JSON消息到WebSocket服务器
 */
static bool ws_send_json(const char* json_str) {
    if (!g_ws_client || !esp_websocket_client_is_connected(g_ws_client)) {
        ESP_LOGW(TAG, "WS not connected, drop message");
        return false;
    }

    int len = strlen(json_str);
    int ret = esp_websocket_client_send_text(g_ws_client, json_str, len, pdMS_TO_TICKS(200));
    if (ret > 0) {
        ESP_LOGI(TAG, "-> Server: %s", json_str);
        return true;
    }
    ESP_LOGW(TAG, "WS send fail, ret=%d", ret);
    return false;
}

/**
 * @brief 发送简单类型消息
 */
static bool ws_send_type(const char* type) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"type\":\"%s\"}", type);
    return ws_send_json(buf);
}

/**
 * @brief 发送hello握手消息
 */
static void ws_send_hello() {
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"hello\",\"device_id\":\"%s\",\"fw\":\"%s\",\"listen_mode\":\"auto\"}",
             g_device_id, HITONY_FW_VERSION);
    ws_send_json(buf);
    ESP_LOGI(TAG, "Hello sent (fw=%s), waiting for server response...", HITONY_FW_VERSION);
}

/**
 * @brief 发送listen协议消息（xiaozhi风格）
 * @param state "detect", "start", "stop"
 * @param mode 可选: "auto", "manual" (仅在state="start"时使用)
 * @param text 可选: 唤醒词文本 (仅在state="detect"时使用)
 */
static bool ws_send_listen(const char* state, const char* mode = nullptr, const char* text = nullptr) {
    char buf[192];
    if (mode && text) {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"%s\",\"text\":\"%s\"}",
                 state, mode, text);
    } else if (mode) {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"%s\"}",
                 state, mode);
    } else if (text) {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"listen\",\"state\":\"%s\",\"text\":\"%s\"}",
                 state, text);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"listen\",\"state\":\"%s\"}", state);
    }
    return ws_send_json(buf);
}

/**
 * @brief 发送abort消息（带原因）
 */
static bool ws_send_abort(const char* reason = nullptr) {
    char buf[128];
    if (reason) {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"abort\",\"reason\":\"%s\"}", reason);
    } else {
        snprintf(buf, sizeof(buf), "{\"type\":\"abort\"}");
    }
    return ws_send_json(buf);
}

/**
 * @brief WebSocket事件处理器（瘦身版 — 运行在WS内部任务中）
 *
 * 只做 memcpy + queue push（<5μs），不做任何解析、mutex、UI操作。
 * 所有重逻辑在 main_control_task 主循环的 Section 0 中处理。
 */
static void websocket_event_handler(void* handler_args, esp_event_base_t base,
                                     int32_t event_id, void* event_data) {
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED: {
            ws_raw_msg_t msg = {.data = nullptr, .len = 0, .msg_type = WS_MSG_CONNECTED};
            xQueueSend(g_ws_rx_queue, &msg, 0);
            break;
        }

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED: {
            // Both unexpected disconnect and graceful close (e.g. OTA) need reconnect
            ws_raw_msg_t msg = {.data = nullptr, .len = 0, .msg_type = WS_MSG_DISCONNECTED};
            xQueueSend(g_ws_rx_queue, &msg, 0);
            break;
        }

        case WEBSOCKET_EVENT_DATA: {
            uint8_t opcode = data->op_code;

            // Ping (0x09) and Pong (0x0A) are handled by ESP-IDF internally, ignore silently
            if (opcode == 0x09 || opcode == 0x0A) {
                break;
            }

            // --- Fragmented binary frame reassembly ---
            // ESP-IDF delivers chunks with payload_offset/payload_len when frame > buffer
            static uint8_t* s_reasm_buf = nullptr;
            static int s_reasm_offset = 0;
            static int s_reasm_total = 0;

            if (opcode == 0x02 && data->payload_len > data->data_len) {
                if (data->payload_offset == 0) {
                    // First chunk — allocate reassembly buffer (pool sized by total)
                    if (s_reasm_buf) {
                        pool_free_by_size(s_reasm_buf, s_reasm_total);
                        s_reasm_buf = nullptr;
                    }
                    if (data->payload_len <= 0 || data->payload_len > 4096) {
                        ESP_LOGW(TAG, "WS frag: payload too large (%d)", data->payload_len);
                        break;
                    }
                    pool_type_t rtype = (data->payload_len <= 256) ? POOL_S_256
                                      : (data->payload_len <= 2048) ? POOL_L_2K : POOL_L_4K;
                    s_reasm_buf = (uint8_t*)pool_alloc(rtype);
                    if (!s_reasm_buf) {
                        ESP_LOGW(TAG, "WS frag: pool alloc fail (%d)", data->payload_len);
                        break;
                    }
                    s_reasm_total = data->payload_len;
                    s_reasm_offset = 0;
                }
                if (s_reasm_buf && s_reasm_offset + data->data_len <= s_reasm_total) {
                    memcpy(s_reasm_buf + s_reasm_offset, data->data_ptr, data->data_len);
                    s_reasm_offset += data->data_len;
                }
                if (s_reasm_buf && s_reasm_offset >= s_reasm_total) {
                    // Complete — enqueue reassembled frame
                    ws_raw_msg_t msg = {
                        .data = s_reasm_buf,
                        .len = (uint16_t)s_reasm_total,
                        .msg_type = WS_MSG_BINARY,
                    };
                    if (xQueueSend(g_ws_rx_queue, &msg, 0) != pdTRUE) {
                        pool_free_by_size(s_reasm_buf, s_reasm_total);
                        ESP_LOGW(TAG, "WS frag: queue full after reassembly (%d B)", s_reasm_total);
                    }
                    s_reasm_buf = nullptr;
                    s_reasm_offset = 0;
                    s_reasm_total = 0;
                }
                break;
            }
            // Clear any stale reassembly state on non-fragment
            if (s_reasm_buf) {
                pool_free_by_size(s_reasm_buf, s_reasm_total);
                s_reasm_buf = nullptr;
                s_reasm_offset = 0;
                s_reasm_total = 0;
            }

            // --- Normal (non-fragmented) frame handling ---
            if (data->data_len <= 0 || data->data_len >= 4096) {
                ESP_LOGW(TAG, "WS data: invalid len=%d, op=0x%02X", data->data_len, opcode);
                break;
            }

            if (opcode != 0x01 && opcode != 0x02) {
                ESP_LOGW(TAG, "WS data: unexpected opcode=0x%02X, len=%d", opcode, data->data_len);
                break;
            }

            // 选择pool
            pool_type_t ptype;
            if (data->data_len <= 256) ptype = POOL_S_256;
            else if (data->data_len <= 2048) ptype = POOL_L_2K;
            else ptype = POOL_L_4K;

            uint8_t* buf = (uint8_t*)pool_alloc(ptype);
            if (!buf) {
                ESP_LOGW(TAG, "WS handler: pool alloc fail (%d bytes)", data->data_len);
                break;
            }
            memcpy(buf, data->data_ptr, data->data_len);

            ws_raw_msg_t msg = {
                .data = buf,
                .len = (uint16_t)data->data_len,
                .msg_type = (uint8_t)(opcode == 0x02 ? WS_MSG_BINARY : WS_MSG_TEXT),
            };

            if (xQueueSend(g_ws_rx_queue, &msg, 0) != pdTRUE) {
                pool_free(ptype, buf);
                ESP_LOGW(TAG, "WS RX queue full, dropped %s (%d B)",
                         opcode == 0x02 ? "bin" : "txt", data->data_len);
            }
            break;
        }

        case WEBSOCKET_EVENT_ERROR: {
            ESP_LOGE(TAG, "WS error (FSM=%d, heap=%lu)",
                     g_current_fsm_state, (unsigned long)esp_get_free_heap_size());
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// WS消息处理函数（运行在main_ctrl任务中，非WS任务）
// ============================================================================

/**
 * @brief 处理WS连接事件
 */
static void handle_ws_connected() {
    ESP_LOGI(TAG, "WebSocket connected to server");
    g_ws_connected = true;
    g_hello_acked = false;
    g_session_id[0] = '\0';

    ws_send_hello();

    fsm_event_msg_t evt = {.event = FSM_EVENT_WS_CONNECTED};
    xQueueSend(g_fsm_event_queue, &evt, 0);
}

/**
 * @brief 处理WS断开事件
 */
static void handle_ws_disconnected() {
    ESP_LOGW(TAG, "WebSocket disconnected! FSM=%d, TTS_rx=%lu, tts_end=%d",
             g_current_fsm_state, g_tts_rx_count, g_tts_end_received);
    ESP_LOGW(TAG, "  Memory: heap=%lu, internal=%lu, largest=%lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    g_ws_connected = false;
    g_hello_acked = false;
    g_session_id[0] = '\0';

    // 排空WS接收队列，释放pool buffers防止泄漏
    ws_raw_msg_t stale;
    int drained = 0;
    while (xQueueReceive(g_ws_rx_queue, &stale, 0) == pdTRUE) {
        if (stale.data) {
            pool_free_by_size(stale.data, stale.len);
        }
        drained++;
    }
    if (drained > 0) {
        ESP_LOGW(TAG, "Drained %d stale messages from WS RX queue", drained);
    }

    // CRITICAL FIX: 排空播放队列，防止内存泄漏
    flush_playback_queue();

    // 停止音乐动画并隐藏耳机图标
    lvgl_ui_set_music_energy(0.0f);

    // During OTA: WS was intentionally closed to free WiFi buffers.
    // Don't trigger reconnect — device will reboot after OTA completes.
    if (ota_is_running()) {
        ESP_LOGI(TAG, "WS closed during OTA — suppressing reconnect");
        lvgl_ui_set_status("OTA updating...");
        return;
    }

    lvgl_ui_set_status("Server lost");

    fsm_event_msg_t disc_evt = {.event = FSM_EVENT_WS_DISCONNECTED};
    xQueueSend(g_fsm_event_queue, &disc_evt, 0);
}

/**
 * @brief 处理WS二进制帧（批量TTS Opus包）
 *
 * 服务器发送批量格式: [2B BE length][opus data][2B BE length][opus data]...
 * 每批约10个包(~2KB)，减少TCP分段数量，解决手机热点限流问题。
 *
 * @return 始终返回false（调用者释放原始batch buffer，个别包已复制到独立pool buffer）
 */
static bool handle_ws_binary(uint8_t* data, uint16_t len) {
    // 状态守卫：SPEAKING和MUSIC都接受音频包
    if (g_current_fsm_state != FSM_STATE_SPEAKING && g_current_fsm_state != FSM_STATE_MUSIC) {
        g_tts_drop_count++;
        if (g_tts_drop_count <= 5 || g_tts_drop_count % 20 == 0) {
            ESP_LOGW(TAG, "TTS drop: state=%d, dropped=%lu", g_current_fsm_state, g_tts_drop_count);
        }
        return false;
    }

    // MUSIC状态不更新超时计时器（无5秒超时）
    if (g_current_fsm_state == FSM_STATE_SPEAKING) {
        g_speaking_start_time = xTaskGetTickCount();
    }

    // 解析批量格式: [2B BE length][opus data]...
    size_t offset = 0;
    int parsed = 0;

    while (offset + 2 <= (size_t)len) {
        uint16_t pkt_len = ((uint16_t)data[offset] << 8) | data[offset + 1];
        offset += 2;

        // 合法性检查
        if (pkt_len == 0 || offset + pkt_len > (size_t)len) {
            ESP_LOGW(TAG, "TTS batch: invalid pkt_len=%u at offset=%zu (total=%u)", pkt_len, offset - 2, len);
            break;
        }

        g_tts_rx_count++;

        // 使用 alloc_opus_msg 分配独立的 msg + data buffer 并复制数据
        opus_packet_msg_t* msg = alloc_opus_msg(pkt_len);
        if (!msg) {
            ESP_LOGW(TAG, "TTS batch: pool exhausted at packet %d (rx=%lu)", parsed, g_tts_rx_count);
            break;
        }
        memcpy(msg->data, &data[offset], pkt_len);

        // Wait up to 30ms (half an Opus frame) for a queue slot to open.
        // On failure, drop only THIS packet and continue parsing remaining packets
        // (instead of break which loses the entire batch tail).
        if (xQueueSend(g_opus_playback_queue, &msg, pdMS_TO_TICKS(30)) != pdTRUE) {
            free_opus_msg(msg);
            offset += pkt_len;
            continue;
        }

        parsed++;
        offset += pkt_len;
    }

    if (parsed > 0) {
        UBaseType_t pb_depth = uxQueueMessagesWaiting(g_opus_playback_queue);
        ESP_LOGI(TAG, "TTS batch: %d pkts parsed, total_rx=%lu, queue=%u/24",
                 parsed, g_tts_rx_count, (unsigned)pb_depth);
    }

    return false;  // 调用者释放原始batch buffer
}

/**
 * @brief 处理WS文本帧（JSON控制消息）
 */
static void handle_ws_text(const char* data, uint16_t len) {
    ESP_LOGI(TAG, "Server JSON: %.*s", len, data);

    cJSON* root = cJSON_ParseWithLength(data, len);
    if (!root) return;

    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "hello") == 0) {
        cJSON* sid = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(sid)) {
            strncpy(g_session_id, sid->valuestring, sizeof(g_session_id) - 1);
            g_session_id[sizeof(g_session_id) - 1] = '\0';
        }
        g_hello_acked = true;
        ESP_LOGI(TAG, "Hello handshake complete, session=%s", g_session_id);
        lvgl_ui_set_status("Connected");
        lvgl_ui_set_debug_info("Say 'Hi Tony'");

        cJSON* features = cJSON_GetObjectItem(root, "features");
        if (features) {
            cJSON* abort_feat = cJSON_GetObjectItem(features, "abort");
            if (cJSON_IsTrue(abort_feat)) {
                ESP_LOGI(TAG, "Server supports abort feature");
            }
        }

    } else if (strcmp(type->valuestring, "tts_start") == 0) {
        ESP_LOGI(TAG, "Server: TTS start");
        cJSON* text_item = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text_item)) {
            ESP_LOGI(TAG, "TTS text: %s", text_item->valuestring);
        }

        // tts_start 和后续 binary 包都经过同一 FIFO 队列(g_ws_rx_queue)，
        // 顺序天然保持。在此同步设置 SPEAKING 状态，后续 binary 包不会被丢弃。
        const char* state_names[] = {"IDLE", "RECORDING", "SPEAKING", "MUSIC", "ERROR"};
        fsm_state_t prev_state = g_current_fsm_state;
        g_current_fsm_state = FSM_STATE_SPEAKING;
        g_speaking_start_time = xTaskGetTickCount();
        g_thinking_start_time = 0;
        g_tts_end_received = false;
        g_drain_wait_count = 0;
        g_tts_rx_count = 0;
        g_tts_drop_count = 0;

        if (prev_state == FSM_STATE_RECORDING) {
            audio_cmd_t cmd_rec = AUDIO_CMD_STOP_RECORDING;
            xQueueSend(g_audio_cmd_queue, &cmd_rec, 0);
            g_audio_start_sent = false;
        }

        audio_cmd_t cmd_play = AUDIO_CMD_START_PLAYBACK;
        xQueueSend(g_audio_cmd_queue, &cmd_play, 0);

        LedController::instance().set_system_state(LedController::SystemState::SPEAKING);
        lvgl_ui_set_status("Speaking...");

        ESP_LOGI(TAG, "FSM: %s -> SPEAKING (tts_start)", state_names[prev_state]);
        ESP_LOGI(TAG, "  Memory: heap=%lu, internal=%lu, largest=%lu",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    } else if (strcmp(type->valuestring, "tts_end") == 0) {
        ESP_LOGI(TAG, "Server: TTS end (rx=%lu, drop=%lu)", g_tts_rx_count, g_tts_drop_count);
        cJSON* reason = cJSON_GetObjectItem(root, "reason");
        if (cJSON_IsString(reason) && strcmp(reason->valuestring, "abort") == 0) {
            ESP_LOGI(TAG, "TTS end (abort acknowledged)");
        }
        fsm_event_msg_t evt = {.event = FSM_EVENT_TTS_END};
        xQueueSend(g_fsm_event_queue, &evt, 0);

    } else if (strcmp(type->valuestring, "music_start") == 0) {
        ESP_LOGI(TAG, "Server: Music start");
        cJSON* title = cJSON_GetObjectItem(root, "title");
        const char* title_str = "";
        if (cJSON_IsString(title)) {
            title_str = title->valuestring;
            ESP_LOGI(TAG, "Music title: %s", title_str);
        }

        const char* state_names[] = {"IDLE", "RECORDING", "SPEAKING", "MUSIC", "ERROR"};
        fsm_state_t prev_state = g_current_fsm_state;
        g_current_fsm_state = FSM_STATE_MUSIC;
        g_tts_end_received = false;
        g_drain_wait_count = 0;
        g_tts_rx_count = 0;
        g_tts_drop_count = 0;
        g_music_was_playing = false;

        // Flush stale FSM events (e.g. TTS_END from hint TTS sent before music_start).
        // Without this, the hint's tts_end event would be processed after we enter MUSIC
        // state, causing g_tts_end_received=true and premature music stop.
        {
            fsm_event_msg_t stale_evt;
            int flushed = 0;
            while (xQueueReceive(g_fsm_event_queue, &stale_evt, 0) == pdTRUE) {
                flushed++;
            }
            if (flushed > 0) {
                ESP_LOGW(TAG, "music_start: flushed %d stale FSM events", flushed);
            }
        }

        if (prev_state == FSM_STATE_RECORDING) {
            audio_cmd_t cmd_rec = AUDIO_CMD_STOP_RECORDING;
            xQueueSend(g_audio_cmd_queue, &cmd_rec, 0);
            g_audio_start_sent = false;
        }

        audio_cmd_t cmd_play = AUDIO_CMD_START_PLAYBACK;
        xQueueSend(g_audio_cmd_queue, &cmd_play, 0);

        LedController::instance().set_system_state(LedController::SystemState::SPEAKING);
        lvgl_ui_set_state(UI_STATE_MUSIC);
        if (title_str[0]) {
            lvgl_ui_set_music_title(title_str);
        }

        ESP_LOGI(TAG, "FSM: %s -> MUSIC (music_start)", state_names[prev_state]);

    } else if (strcmp(type->valuestring, "music_end") == 0) {
        ESP_LOGI(TAG, "Server: Music end");
        lvgl_ui_hide_music_title();
        if (g_current_fsm_state == FSM_STATE_MUSIC) {
            fsm_event_msg_t evt = {.event = FSM_EVENT_TTS_END};
            xQueueSend(g_fsm_event_queue, &evt, 0);
        } else {
            ESP_LOGW(TAG, "music_end ignored (state=%d, not MUSIC)", g_current_fsm_state);
            g_music_was_playing = false;
        }

    } else if (strcmp(type->valuestring, "music_resume") == 0) {
        ESP_LOGI(TAG, "Server: Music resume");
        if (g_music_was_playing) {
            g_current_fsm_state = FSM_STATE_MUSIC;
            g_tts_end_received = false;
            g_drain_wait_count = 0;
            g_music_was_playing = false;

            // Flush stale FSM events (TTS_END from voice interaction reply)
            {
                fsm_event_msg_t stale_evt;
                while (xQueueReceive(g_fsm_event_queue, &stale_evt, 0) == pdTRUE) {}
            }

            audio_cmd_t cmd_play = AUDIO_CMD_START_PLAYBACK;
            xQueueSend(g_audio_cmd_queue, &cmd_play, 0);

            LedController::instance().set_system_state(LedController::SystemState::SPEAKING);
            lvgl_ui_set_state(UI_STATE_MUSIC);
            ESP_LOGI(TAG, "FSM: -> MUSIC (resume)");
        } else {
            ESP_LOGW(TAG, "music_resume ignored (no paused music)");
        }

    } else if (strcmp(type->valuestring, "asr_text") == 0) {
        cJSON* text_item = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text_item)) {
            ESP_LOGI(TAG, "ASR result: %s", text_item->valuestring);
        }

    } else if (strcmp(type->valuestring, "error") == 0) {
        cJSON* msg_item = cJSON_GetObjectItem(root, "message");
        if (cJSON_IsString(msg_item)) {
            ESP_LOGW(TAG, "Server error: %s", msg_item->valuestring);
        }
        if (g_thinking_start_time > 0) {
            g_thinking_start_time = 0;
            LedController::instance().set_system_state(LedController::SystemState::LISTENING);
            lvgl_ui_set_status("Server Error");
            lvgl_ui_set_debug_info("Say 'Hi Tony'");
            ESP_LOGW(TAG, "Server error during thinking, resetting to IDLE");
        }

    } else if (strcmp(type->valuestring, "expression") == 0) {
        cJSON* expr_item = cJSON_GetObjectItem(root, "expr");
        if (cJSON_IsString(expr_item)) {
            // Default 3 seconds, server can override with "duration_ms"
            uint32_t dur = 3000;
            cJSON* dur_item = cJSON_GetObjectItem(root, "duration_ms");
            if (cJSON_IsNumber(dur_item) && dur_item->valueint > 0) {
                dur = (uint32_t)dur_item->valueint;
            }
            ESP_LOGI(TAG, "Server expression: %s (%lums)", expr_item->valuestring, (unsigned long)dur);
            lvgl_ui_show_expression(expr_item->valuestring, dur);
        }

    } else if (strcmp(type->valuestring, "pong") == 0) {
        ESP_LOGD(TAG, "Server pong");

    } else if (strcmp(type->valuestring, "ota_notify") == 0) {
        cJSON* version = cJSON_GetObjectItem(root, "version");
        cJSON* url = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(version) && cJSON_IsString(url)) {
            ESP_LOGI(TAG, "OTA available: version=%s url=%s", version->valuestring, url->valuestring);
            if (strcmp(version->valuestring, HITONY_FW_VERSION) != 0) {
                if (!ota_is_running()) {
                    ota_start_update(url->valuestring);
                }
            } else {
                ESP_LOGI(TAG, "OTA: already on version %s, skipping", HITONY_FW_VERSION);
            }
        }
    }

    cJSON_Delete(root);
}

/**
 * @brief FSM事件处理函数
 */
static void fsm_handle_event(fsm_state_t* state, fsm_event_msg_t event) {
    fsm_state_t old_state = *state;
    LedController& led = LedController::instance();

    switch (*state) {
        case FSM_STATE_IDLE:
            if (event.event == FSM_EVENT_WAKE_DETECTED) {
                *state = FSM_STATE_RECORDING;
                g_thinking_start_time = 0;
                g_recording_start_time = xTaskGetTickCount();  // [S0-2] 录音开始计时

                ESP_LOGI(TAG, "Wake detected, entering RECORDING mode");

                // [S0-1] 即时视觉反馈：眼睛快速变大+状态灯变红
                lvgl_ui_set_state(UI_STATE_LISTENING);

                ringbuffer_reset(&g_pcm_ringbuffer);

                // Xiaozhi风格协议: listen(detect) + listen(start, auto)
                ws_send_listen("detect", nullptr, "Hi Tony");
                g_audio_start_sent = ws_send_listen("start", "auto");

                audio_cmd_t cmd = AUDIO_CMD_START_RECORDING;
                xQueueSend(g_audio_cmd_queue, &cmd, 0);

                led.set_system_state(LedController::SystemState::RECORDING);

            } else if (event.event == FSM_EVENT_TTS_START) {
                *state = FSM_STATE_SPEAKING;
                g_speaking_start_time = xTaskGetTickCount();
                g_thinking_start_time = 0;
                lvgl_ui_set_pupil_offset(0, 0);  // 停止思考动画

                ESP_LOGI(TAG, "TTS start (from IDLE), entering SPEAKING mode");
                audio_cmd_t cmd_play = AUDIO_CMD_START_PLAYBACK;
                xQueueSend(g_audio_cmd_queue, &cmd_play, 0);

                led.set_system_state(LedController::SystemState::SPEAKING);
                lvgl_ui_set_state(UI_STATE_SPEAKING);

            } else if (event.event == FSM_EVENT_WS_CONNECTED) {
                // WebSocket连接成功（在IDLE状态下）
                led.set_system_state(LedController::SystemState::LISTENING);
                lvgl_ui_set_state(UI_STATE_WS_CONNECTED);

            } else if (event.event == FSM_EVENT_WS_DISCONNECTED) {
                *state = FSM_STATE_ERROR;
                led.set_system_state(LedController::SystemState::NO_NETWORK);
                lvgl_ui_set_state(UI_STATE_ERROR);
            }
            break;

        case FSM_STATE_RECORDING:
            if (event.event == FSM_EVENT_RECORDING_END) {
                *state = FSM_STATE_IDLE;
                g_recording_start_time = 0;
                g_thinking_start_time = xTaskGetTickCount();  // 记录"Thinking"开始时间

                ESP_LOGI(TAG, "Recording end, entering IDLE(Thinking) mode");

                if (!g_audio_start_sent && g_ws_connected) {
                    ESP_LOGW(TAG, "listen(start) was not sent earlier, sending now...");
                    ws_send_listen("start", "auto");
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                ws_send_listen("stop");
                g_audio_start_sent = false;

                audio_cmd_t cmd = AUDIO_CMD_STOP_RECORDING;
                xQueueSend(g_audio_cmd_queue, &cmd, 0);

                led.set_system_state(LedController::SystemState::THINKING);
                lvgl_ui_set_state(UI_STATE_WS_CONNECTED);  // Thinking阶段保持WS_CONNECTED灯色

            } else if (event.event == FSM_EVENT_TTS_START) {
                *state = FSM_STATE_SPEAKING;
                g_speaking_start_time = xTaskGetTickCount();
                g_recording_start_time = 0;

                ESP_LOGI(TAG, "TTS start, entering SPEAKING mode");
                audio_cmd_t cmd_rec = AUDIO_CMD_STOP_RECORDING;
                xQueueSend(g_audio_cmd_queue, &cmd_rec, 0);

                audio_cmd_t cmd_play = AUDIO_CMD_START_PLAYBACK;
                xQueueSend(g_audio_cmd_queue, &cmd_play, 0);

                led.set_system_state(LedController::SystemState::SPEAKING);
                lvgl_ui_set_state(UI_STATE_SPEAKING);

            } else if (event.event == FSM_EVENT_WS_DISCONNECTED) {
                ESP_LOGW(TAG, "WebSocket disconnected during RECORDING, stopping");
                *state = FSM_STATE_ERROR;
                g_audio_start_sent = false;
                g_recording_start_time = 0;

                audio_cmd_t cmd = AUDIO_CMD_STOP_RECORDING;
                xQueueSend(g_audio_cmd_queue, &cmd, 0);

                led.set_system_state(LedController::SystemState::NO_NETWORK);
                lvgl_ui_set_state(UI_STATE_ERROR);
            }
            break;

        case FSM_STATE_SPEAKING:
            if (event.event == FSM_EVENT_TTS_END) {
                g_tts_end_received = true;
                ESP_LOGI(TAG, "TTS end received, waiting for playback queue to drain...");

            } else if (event.event == FSM_EVENT_WAKE_DETECTED) {
                // 用户在TTS播放期间再次唤醒 → 中断TTS，开始新录音
                ESP_LOGI(TAG, "Wake during SPEAKING -> aborting TTS, start new recording");

                // 发送abort到服务器（带原因）
                ws_send_abort("wake_word_detected");

                // 停止播放
                audio_cmd_t cmd_stop = AUDIO_CMD_STOP_PLAYBACK;
                xQueueSend(g_audio_cmd_queue, &cmd_stop, 0);
                flush_playback_queue();
                lvgl_ui_set_music_energy(0.0f);
                g_tts_end_received = false;
                g_speaking_start_time = 0;
                g_drain_wait_count = 0;

                // 直接进入RECORDING模式
                *state = FSM_STATE_RECORDING;
                g_recording_start_time = xTaskGetTickCount();
                ringbuffer_reset(&g_pcm_ringbuffer);
                ws_send_listen("detect", nullptr, "Hi Tony");
                g_audio_start_sent = ws_send_listen("start", "auto");

                audio_cmd_t cmd_rec = AUDIO_CMD_START_RECORDING;
                xQueueSend(g_audio_cmd_queue, &cmd_rec, 0);

                led.set_system_state(LedController::SystemState::RECORDING);
                lvgl_ui_set_state(UI_STATE_LISTENING);

            } else if (event.event == FSM_EVENT_WS_DISCONNECTED) {
                ESP_LOGW(TAG, "WebSocket disconnected during SPEAKING, stopping playback");
                *state = FSM_STATE_ERROR;
                g_tts_end_received = false;
                g_speaking_start_time = 0;

                audio_cmd_t cmd = AUDIO_CMD_STOP_PLAYBACK;
                xQueueSend(g_audio_cmd_queue, &cmd, 0);
                flush_playback_queue();

                led.set_system_state(LedController::SystemState::NO_NETWORK);
                lvgl_ui_set_state(UI_STATE_ERROR);
            }
            break;

        case FSM_STATE_MUSIC:
            if (event.event == FSM_EVENT_TTS_END) {
                g_tts_end_received = true;
                ESP_LOGI(TAG, "Music end received, waiting for playback queue to drain...");

            } else if (event.event == FSM_EVENT_WAKE_DETECTED) {
                // 用户在音乐播放期间唤醒 → 暂停音乐，开始录音
                ESP_LOGI(TAG, "Wake during MUSIC -> pausing music, start recording");

                ws_send_json("{\"type\":\"music_ctrl\",\"action\":\"pause\"}");

                audio_cmd_t cmd_stop = AUDIO_CMD_STOP_PLAYBACK;
                xQueueSend(g_audio_cmd_queue, &cmd_stop, 0);
                flush_playback_queue();
                g_tts_end_received = false;
                g_drain_wait_count = 0;
                g_music_was_playing = true;

                *state = FSM_STATE_RECORDING;
                g_recording_start_time = xTaskGetTickCount();
                ringbuffer_reset(&g_pcm_ringbuffer);
                ws_send_listen("detect", nullptr, "Hi Tony");
                g_audio_start_sent = ws_send_listen("start", "auto");

                audio_cmd_t cmd_rec = AUDIO_CMD_START_RECORDING;
                xQueueSend(g_audio_cmd_queue, &cmd_rec, 0);

                led.set_system_state(LedController::SystemState::RECORDING);
                lvgl_ui_set_state(UI_STATE_LISTENING);

            } else if (event.event == FSM_EVENT_WS_DISCONNECTED) {
                ESP_LOGW(TAG, "WebSocket disconnected during MUSIC, stopping playback");
                *state = FSM_STATE_ERROR;
                g_tts_end_received = false;
                g_music_was_playing = false;
                lvgl_ui_set_music_energy(0.0f);

                audio_cmd_t cmd = AUDIO_CMD_STOP_PLAYBACK;
                xQueueSend(g_audio_cmd_queue, &cmd, 0);
                flush_playback_queue();

                led.set_system_state(LedController::SystemState::NO_NETWORK);
                lvgl_ui_set_state(UI_STATE_ERROR);
            }
            break;

        case FSM_STATE_ERROR:
            if (event.event == FSM_EVENT_WS_CONNECTED) {
                *state = FSM_STATE_IDLE;
                g_reconnect_attempts = 0;
                ESP_LOGI(TAG, "WebSocket reconnected! Recovering to IDLE");
                led.set_system_state(LedController::SystemState::LISTENING);
                lvgl_ui_set_state(UI_STATE_WS_CONNECTED);
            }
            break;
    }

    if (old_state != *state) {
        const char* state_names[] = {"IDLE", "RECORDING", "SPEAKING", "MUSIC", "ERROR"};
        ESP_LOGI(TAG, "FSM: %s -> %s", state_names[old_state], state_names[*state]);
    }
}

/**
 * @brief Main Control Task - 整合所有控制任务
 */
void main_control_task(void* arg) {
    ESP_LOGI(TAG, "Main Control Task started on Core %d", xPortGetCoreID());

    // [S0-5] 从芯片MAC生成唯一设备标识
    init_device_identity();

    // === 1. 等待WiFi连接（带超时，每秒更新UI）===
    // 注意：LVGL更新由独立的lvgl_task处理（带互斥锁），这里不调用lv_timer_handler()
    ESP_LOGI(TAG, "Waiting for WiFi connection (timeout: 10s)...");
    lvgl_ui_set_state(UI_STATE_WIFI_CONNECTING);  // [S1-5] 启动阶段状态灯同步
    lvgl_ui_set_status("Connecting WiFi...");

    bool wifi_connected = false;
    for (int i = 0; i < 10; i++) {
        EventBits_t bits = xEventGroupWaitBits(g_app_event_group, EVENT_WIFI_CONNECTED,
                                               pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));

        if (bits & EVENT_WIFI_CONNECTED) {
            wifi_connected = true;
            break;
        }

        char buf[32];
        snprintf(buf, sizeof(buf), "Connecting WiFi... %ds", 10 - i - 1);
        lvgl_ui_set_status(buf);
    }

    if (wifi_connected) {
        ESP_LOGI(TAG, "WiFi connected");
        lvgl_ui_set_state(UI_STATE_WIFI_CONNECTED);
        lvgl_ui_set_status("WiFi connected!");

        // Binding info is only shown during provisioning mode (touch during boot)
        // Normal boot: skip straight to WebSocket connection
        ESP_LOGI(TAG, "Device: %s (token=%s)", g_device_id, g_device_token);
    } else {
        ESP_LOGW(TAG, "WiFi timeout, running in offline mode");
        lvgl_ui_set_state(UI_STATE_ERROR);
        lvgl_ui_set_status("Offline mode");
    }

    // === 2. 初始化WebSocket客户端 ===
    if (wifi_connected) {
        static char ws_headers[128];
        snprintf(ws_headers, sizeof(ws_headers),
                 "x-device-id: %s\r\nx-device-token: %s\r\n",
                 g_device_id, g_device_token);

        esp_websocket_client_config_t ws_cfg = {};
        ws_cfg.uri = HITONY_WS_URL;
        ws_cfg.headers = ws_headers;
        ws_cfg.task_stack = 4096;           // 4KB (瘦回调不做cJSON，足够)
        ws_cfg.buffer_size = 8192;          // 8KB接收缓冲（容纳多帧TTS数据）
        ws_cfg.disable_auto_reconnect = true; // 禁用库自动重连（FSM管理重连）
        ws_cfg.network_timeout_ms = 10000;
        ws_cfg.ping_interval_sec = 0;       // 禁用WS ping（改用TCP keepalive）
        ws_cfg.pingpong_timeout_sec = 0;    // 禁用WS pong超时
        ws_cfg.keep_alive_enable = true;    // TCP keepalive（内核处理）
        ws_cfg.keep_alive_idle = 10;        // 10s空闲开始探测（服务器ASR+LLM需5-10s）
        ws_cfg.keep_alive_interval = 5;
        ws_cfg.keep_alive_count = 3;        // 总超时：10+5*3=25s

        g_ws_client = esp_websocket_client_init(&ws_cfg);
        if (!g_ws_client) {
            ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        } else {
            esp_websocket_register_events(g_ws_client, WEBSOCKET_EVENT_ANY,
                                          websocket_event_handler, nullptr);

            ESP_LOGI(TAG, "Connecting to %s", HITONY_WS_URL);
            esp_err_t ret = esp_websocket_client_start(g_ws_client);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to start WebSocket client: %d", ret);
                esp_websocket_client_destroy(g_ws_client);
                g_ws_client = nullptr;
            }
        }
    } else {
        g_ws_client = nullptr;
    }

    // === 3. LED控制器 ===
    LedController& led = LedController::instance();

    ESP_LOGI(TAG, "All control components initialized");

    // 更新UI显示当前状态
    if (g_ws_client) {
        lvgl_ui_set_debug_info("Connecting to server...");
    } else {
        lvgl_ui_set_debug_info("Offline mode");
    }

    // === 本地状态变量 ===
    // 使用全局变量，WebSocket事件处理器可以检查状态（状态守卫）
    // g_current_fsm_state 已在文件顶部定义
    uint32_t heartbeat_counter = 0;
    uint32_t ws_tx_count = 0;
    // g_drain_wait_count 使用文件级静态变量

    ESP_LOGI(TAG, "Entering main control loop...");

    // === 主循环 ===
    while (1) {
        // === 0. 处理 WebSocket 接收队列（从瘦回调中转发的原始消息）===
        {
            ws_raw_msg_t raw_msg;
            int ws_processed = 0;
            while (ws_processed < 10 && xQueueReceive(g_ws_rx_queue, &raw_msg, 0) == pdTRUE) {
                ws_processed++;
                switch (raw_msg.msg_type) {
                    case WS_MSG_BINARY: {
                        bool transferred = handle_ws_binary(raw_msg.data, raw_msg.len);
                        if (!transferred) {
                            // 所有权未转移（被丢弃或队列满），释放buffer
                            pool_free_by_size(raw_msg.data, raw_msg.len);
                        }
                        break;
                    }
                    case WS_MSG_TEXT:
                        handle_ws_text((char*)raw_msg.data, raw_msg.len);
                        pool_free_by_size(raw_msg.data, raw_msg.len);
                        break;
                    case WS_MSG_CONNECTED:
                        handle_ws_connected();
                        break;
                    case WS_MSG_DISCONNECTED:
                        handle_ws_disconnected();
                        break;
                }
            }
        }

        // === 1. 处理FSM事件队列 ===
        fsm_event_msg_t event;
        if (xQueueReceive(g_fsm_event_queue, &event, 0) == pdTRUE) {
            fsm_handle_event(&g_current_fsm_state, event);
        }

        // === 2. 检查Audio Task事件 ===
        EventBits_t audio_bits = xEventGroupGetBits(g_audio_event_bits);

        if (audio_bits & AUDIO_EVENT_WAKE_DETECTED) {
            xEventGroupClearBits(g_audio_event_bits, AUDIO_EVENT_WAKE_DETECTED);
            // AEC未启用时，SPEAKING/MUSIC期间忽略语音唤醒词（扬声器回声会误触发WakeNet）
            // TODO: AEC启用后可移除此过滤，实现全双工TTS打断
            if (g_current_fsm_state != FSM_STATE_SPEAKING &&
                g_current_fsm_state != FSM_STATE_MUSIC) {
                fsm_event_msg_t wake_evt = {.event = FSM_EVENT_WAKE_DETECTED};
                xQueueSend(g_fsm_event_queue, &wake_evt, 0);
            } else {
                ESP_LOGW(TAG, "Voice wake ignored during %s (no AEC, likely speaker echo)",
                         g_current_fsm_state == FSM_STATE_MUSIC ? "MUSIC" : "SPEAKING");
            }
        }

        // [S0-6] 触摸唤醒：即时反馈 + 状态感知行为
        if (audio_bits & AUDIO_EVENT_TOUCH_WAKE) {
            xEventGroupClearBits(g_audio_event_bits, AUDIO_EVENT_TOUCH_WAKE);
            ESP_LOGI(TAG, "Touch wake in state %d", g_current_fsm_state);

            // 即时LED反馈（所有状态通用）
            LedController::instance().set_system_state(LedController::SystemState::WAKE_DETECTED);

            // 即时瞳孔反馈：快速收缩表示"收到"
            lvgl_ui_set_pupil_offset(0, 0);

            fsm_event_msg_t wake_evt = {.event = FSM_EVENT_WAKE_DETECTED};
            xQueueSend(g_fsm_event_queue, &wake_evt, 0);
        }

        if (audio_bits & AUDIO_EVENT_VAD_END) {
            xEventGroupClearBits(g_audio_event_bits, AUDIO_EVENT_VAD_END);

            if (g_current_fsm_state == FSM_STATE_RECORDING) {
                fsm_event_msg_t end_evt = {.event = FSM_EVENT_RECORDING_END};
                xQueueSend(g_fsm_event_queue, &end_evt, 0);
            }
        }

        // === 3. 状态相关操作 ===
        switch (g_current_fsm_state) {
            case FSM_STATE_RECORDING: {
                // [S0-2] 录音15s超时保护：防止VAD失败导致永久录音
                if (g_recording_start_time > 0 &&
                    (xTaskGetTickCount() - g_recording_start_time) > pdMS_TO_TICKS(15000)) {
                    ESP_LOGW(TAG, "RECORDING timeout (15s), forcing end");
                    g_recording_start_time = 0;
                    fsm_event_msg_t timeout_evt = {.event = FSM_EVENT_RECORDING_END};
                    xQueueSend(g_fsm_event_queue, &timeout_evt, 0);
                    break;
                }

                if (!g_audio_start_sent && g_ws_client && esp_websocket_client_is_connected(g_ws_client)) {
                    g_audio_start_sent = ws_send_type("audio_start");
                }

                opus_packet_msg_t* opus_msg = nullptr;
                int tx_this_loop = 0;
                while (tx_this_loop < 4 && xQueueReceive(g_opus_tx_queue, &opus_msg, 0) == pdTRUE) {
                    tx_this_loop++;
                    if (g_ws_client && esp_websocket_client_is_connected(g_ws_client)) {
                        int sent = esp_websocket_client_send_bin(g_ws_client,
                                    (const char*)opus_msg->data, opus_msg->len,
                                    pdMS_TO_TICKS(100));

                        if (sent > 0) {
                            ws_tx_count++;
                            if (ws_tx_count % 20 == 0) {
                                ESP_LOGI(TAG, "WS TX: %lu packets sent", ws_tx_count);
                            }
                        } else {
                            ESP_LOGW(TAG, "Failed to send WebSocket data");
                        }
                    } else {
                        ws_tx_count++;
                        if (ws_tx_count % 20 == 0) {
                            ESP_LOGI(TAG, "Offline: %lu Opus packets encoded (not sent)", ws_tx_count);
                        }
                    }

                    free_opus_msg(opus_msg);
                }
                break;
            }

            case FSM_STATE_SPEAKING: {
                // 每1秒打印一次SPEAKING诊断（调试TTS中断问题）
                {
                    static uint32_t last_speaking_mem_log = 0;
                    uint32_t now_tick = xTaskGetTickCount();
                    if (last_speaking_mem_log == 0 ||
                        (now_tick - last_speaking_mem_log) * portTICK_PERIOD_MS > 1000) {
                        uint32_t elapsed_ms = (now_tick - g_speaking_start_time) * portTICK_PERIOD_MS;
                        UBaseType_t ws_q = uxQueueMessagesWaiting(g_ws_rx_queue);
                        UBaseType_t pb_q = uxQueueMessagesWaiting(g_opus_playback_queue);
                        ESP_LOGI(TAG, "SPEAKING @%lums: rx=%lu drop=%lu ws_q=%u pb_q=%u tts_end=%d WS=%s",
                                 (unsigned long)elapsed_ms, g_tts_rx_count, g_tts_drop_count,
                                 (unsigned)ws_q, (unsigned)pb_q, g_tts_end_received,
                                 (g_ws_client && esp_websocket_client_is_connected(g_ws_client)) ? "Y" : "N");
                        last_speaking_mem_log = now_tick;
                    }
                }

                // 无包警告：2s和4s未收到新TTS包时提前告警（诊断5-packet问题）
                if (g_speaking_start_time > 0 && g_tts_rx_count > 0) {
                    uint32_t gap_ms = (xTaskGetTickCount() - g_speaking_start_time) * portTICK_PERIOD_MS;
                    static bool warned_2s = false, warned_4s = false;
                    if (gap_ms > 2000 && !warned_2s) {
                        warned_2s = true;
                        ESP_LOGW(TAG, "No TTS packet for 2s! rx=%lu, WS=%s",
                                 g_tts_rx_count,
                                 (g_ws_client && esp_websocket_client_is_connected(g_ws_client)) ? "connected" : "DISCONNECTED");
                    }
                    if (gap_ms > 4000 && !warned_4s) {
                        warned_4s = true;
                        ESP_LOGW(TAG, "No TTS packet for 4s! rx=%lu, WS=%s, heap=%lu",
                                 g_tts_rx_count,
                                 (g_ws_client && esp_websocket_client_is_connected(g_ws_client)) ? "connected" : "DISCONNECTED",
                                 (unsigned long)esp_get_free_heap_size());
                    }
                    // 收到新包时重置警告标记（g_speaking_start_time已在handle_ws_binary中重置）
                    if (gap_ms < 500) { warned_2s = false; warned_4s = false; }
                }

                // [S0-3] 8秒超时保护（从5s放宽至8s：避免首包慢时误触超时）
                if (g_speaking_start_time > 0 &&
                    (xTaskGetTickCount() - g_speaking_start_time) > pdMS_TO_TICKS(8000)) {
                    ESP_LOGW(TAG, "SPEAKING timeout (8s no packet, rx=%lu drop=%lu), sending abort and forcing IDLE",
                             g_tts_rx_count, g_tts_drop_count);
                    // 通知服务器设备超时，让服务器清理会话状态
                    ws_send_abort("speaking_timeout");

                    g_tts_end_received = false;
                    g_drain_wait_count = 0;
                    g_speaking_start_time = 0;

                    audio_cmd_t cmd = AUDIO_CMD_STOP_PLAYBACK;
                    xQueueSend(g_audio_cmd_queue, &cmd, 0);
                    flush_playback_queue();

                    g_current_fsm_state = FSM_STATE_IDLE;
                    led.set_system_state(LedController::SystemState::LISTENING);
                    lvgl_ui_set_state(UI_STATE_WS_CONNECTED);
                    break;
                }

                // 等待播放队列排空
                if (g_tts_end_received) {
                    UBaseType_t queue_count = uxQueueMessagesWaiting(g_opus_playback_queue);
                    if (queue_count == 0) {
                        g_drain_wait_count++;
                        if (g_drain_wait_count >= 10) {  // 100ms buffer
                            g_tts_end_received = false;
                            g_drain_wait_count = 0;
                            g_speaking_start_time = 0;

                            audio_cmd_t cmd = AUDIO_CMD_STOP_PLAYBACK;
                            xQueueSend(g_audio_cmd_queue, &cmd, 0);

                            // Auto-listen: TTS结束后自动进入聆听模式
                            if (g_auto_listen_enabled && g_ws_connected) {
                                ESP_LOGI(TAG, "Playback drained, auto-listen enabled -> entering RECORDING");
                                g_current_fsm_state = FSM_STATE_RECORDING;
                                g_recording_start_time = xTaskGetTickCount();
                                ringbuffer_reset(&g_pcm_ringbuffer);

                                g_audio_start_sent = ws_send_listen("start", "auto");

                                audio_cmd_t cmd_rec = AUDIO_CMD_START_RECORDING;
                                xQueueSend(g_audio_cmd_queue, &cmd_rec, 0);

                                led.set_system_state(LedController::SystemState::RECORDING);
                                lvgl_ui_set_state(UI_STATE_LISTENING);
                            } else if (g_music_was_playing && g_ws_connected) {
                                ESP_LOGI(TAG, "Playback drained, requesting music resume");
                                ws_send_json("{\"type\":\"music_ctrl\",\"action\":\"resume\"}");
                                g_current_fsm_state = FSM_STATE_IDLE;
                                led.set_system_state(LedController::SystemState::LISTENING);
                                lvgl_ui_set_state(UI_STATE_WS_CONNECTED);
                            } else {
                                ESP_LOGI(TAG, "Playback drained, entering IDLE");
                                g_current_fsm_state = FSM_STATE_IDLE;
                                g_music_was_playing = false;
                                led.set_system_state(LedController::SystemState::LISTENING);
                                lvgl_ui_set_state(UI_STATE_WS_CONNECTED);
                            }
                            ESP_LOGI(TAG, "Post-TTS transition (session=%s, WS=%s, auto_listen=%d)",
                                     g_session_id, g_ws_connected ? "connected" : "DISCONNECTED",
                                     g_auto_listen_enabled);
                        }
                    } else {
                        g_drain_wait_count = 0;
                    }
                }
                break;
            }

            case FSM_STATE_MUSIC: {
                // 音乐模式：无5秒超时（音乐流间隔不确定），只处理队列排空
                if (g_tts_end_received) {
                    UBaseType_t queue_count = uxQueueMessagesWaiting(g_opus_playback_queue);
                    if (queue_count == 0) {
                        g_drain_wait_count++;
                        if (g_drain_wait_count >= 10) {  // 100ms buffer
                            g_tts_end_received = false;
                            g_drain_wait_count = 0;

                            audio_cmd_t cmd = AUDIO_CMD_STOP_PLAYBACK;
                            xQueueSend(g_audio_cmd_queue, &cmd, 0);

                            ESP_LOGI(TAG, "Music playback drained, entering IDLE");
                            g_current_fsm_state = FSM_STATE_IDLE;
                            g_music_was_playing = false;
                            lvgl_ui_set_music_energy(0.0f);
                            led.set_system_state(LedController::SystemState::LISTENING);
                            lvgl_ui_set_state(UI_STATE_WS_CONNECTED);
                        }
                    } else {
                        g_drain_wait_count = 0;
                    }
                }
                break;
            }

            case FSM_STATE_ERROR: {
                // 指数退避重连: 3s → 6s → 12s → 24s → 24s (max)
                static uint32_t last_reconnect_tick = 0;
                uint32_t now = xTaskGetTickCount();

                int shift = g_reconnect_attempts < 4 ? g_reconnect_attempts : 3;
                uint32_t backoff_ms = 3000u << shift;
                if (backoff_ms > 24000) backoff_ms = 24000;

                uint32_t elapsed_ms = (last_reconnect_tick > 0)
                    ? (now - last_reconnect_tick) * portTICK_PERIOD_MS : backoff_ms;

                if (last_reconnect_tick == 0 || elapsed_ms > backoff_ms) {
                    ESP_LOGW(TAG, "Reconnect attempt #%d (backoff %lums)...",
                             g_reconnect_attempts + 1, (unsigned long)backoff_ms);
                    ws_recreate_client();
                    last_reconnect_tick = now;
                    g_reconnect_attempts++;
                } else {
                    // [S1-2] 每秒更新重连倒计时显示
                    static uint32_t last_countdown_s = 0;
                    uint32_t remaining_s = (backoff_ms - elapsed_ms) / 1000;
                    if (remaining_s != last_countdown_s) {
                        last_countdown_s = remaining_s;
                        char buf[32];
                        snprintf(buf, sizeof(buf), "Reconnect %lus...", (unsigned long)remaining_s);
                        lvgl_ui_set_status(buf);
                    }
                }
                break;
            }

            case FSM_STATE_IDLE: {
                // "Thinking" 超时：如果服务器10秒内没有响应tts_start，重置UI
                if (g_thinking_start_time > 0 &&
                    (xTaskGetTickCount() - g_thinking_start_time) > pdMS_TO_TICKS(10000)) {
                    ESP_LOGW(TAG, "Thinking timeout (10s), server did not respond with TTS");
                    g_thinking_start_time = 0;
                    lvgl_ui_set_pupil_offset(0, 0);  // 重置瞳孔位置
                    led.set_system_state(LedController::SystemState::LISTENING);
                    lvgl_ui_set_status("Connected");
                    lvgl_ui_set_debug_info("Say 'Hi Tony'");
                }

                // [S0-4] 思考动画：瞳孔左右缓慢摆动，表示正在等待服务器响应
                if (g_thinking_start_time > 0) {
                    uint32_t elapsed = (xTaskGetTickCount() - g_thinking_start_time) * portTICK_PERIOD_MS;
                    int x_offset = (int)(8.0f * sinf((float)elapsed / 1000.0f * 3.14159f));
                    lvgl_ui_set_pupil_offset(x_offset, 0);
                }

                // Safety net: detect WS disconnect in IDLE (e.g. close event missed)
                // Skip during OTA — WS was intentionally closed
                if (!g_ws_connected && g_hello_acked && !ota_is_running()) {
                    ESP_LOGW(TAG, "IDLE but WS disconnected — forcing ERROR state for reconnect");
                    g_hello_acked = false;
                    g_session_id[0] = '\0';
                    g_current_fsm_state = FSM_STATE_ERROR;
                    led.set_system_state(LedController::SystemState::NO_NETWORK);
                    lvgl_ui_set_state(UI_STATE_ERROR);
                    break;
                }

                // [S1-3] g_music_was_playing 10s超时清理：防止音乐恢复请求无响应导致标记永久卡住
                if (g_music_was_playing && g_thinking_start_time == 0) {
                    static uint32_t music_flag_set_time = 0;
                    if (music_flag_set_time == 0) {
                        music_flag_set_time = xTaskGetTickCount();
                    } else if ((xTaskGetTickCount() - music_flag_set_time) > pdMS_TO_TICKS(10000)) {
                        ESP_LOGW(TAG, "g_music_was_playing stuck for 10s, clearing");
                        g_music_was_playing = false;
                        music_flag_set_time = 0;
                    }
                }
                break;
            }

            default:
                break;
        }

        // === 4. UI更新（由lvgl_task独立处理，此处不再调用lv_timer_handler）===

        // === 5. 系统心跳 ===
        heartbeat_counter++;
        if (heartbeat_counter >= 100) {  // 1秒
            heartbeat_counter = 0;

            // 每5秒打印心跳
            static uint32_t heartbeat_5s = 0;
            heartbeat_5s++;
            if (heartbeat_5s >= 5) {
                heartbeat_5s = 0;
                const char* state_names[] = {"IDLE", "RECORDING", "SPEAKING", "MUSIC", "ERROR"};
                ESP_LOGI(TAG, "Heartbeat: State=%s WS=%s Hello=%s Session=%s",
                         state_names[g_current_fsm_state],
                         g_ws_connected ? "Y" : "N",
                         g_hello_acked ? "Y" : "N",
                         g_session_id[0] ? g_session_id : "none");
            }

            // 每10秒打印详细统计
            static uint32_t stats_counter = 0;
            stats_counter++;

            if (stats_counter >= 10) {
                stats_counter = 0;

                ESP_LOGI(TAG, "=== System Stats ===");
                ESP_LOGI(TAG, "FSM State: %d, WS TX: %lu packets", g_current_fsm_state, ws_tx_count);
                ESP_LOGI(TAG, "Free heap: %lu bytes, PSRAM: %lu bytes",
                         esp_get_free_heap_size(),
                         heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

                // Stack watermarks: uxTaskGetStackHighWaterMark returns minimum
                // remaining stack in StackType_t units (bytes on ESP32-S3)
                struct { const char* name; uint32_t stack_bytes; } task_info[] = {
                    {"audio_main", 32768},
                    {"main_ctrl",  8192},
                    {"afe_task",   12288},
                    {"led_ctrl",   2048},
                };
                ESP_LOGI(TAG, "=== Stack Watermarks (min bytes free) ===");
                for (auto& ti : task_info) {
                    TaskHandle_t h = xTaskGetHandle(ti.name);
                    if (h) {
                        UBaseType_t wm = uxTaskGetStackHighWaterMark(h);
                        ESP_LOGI(TAG, "%-10s: %5u free / %5lu alloc",
                                 ti.name, (unsigned)wm, ti.stack_bytes);
                    }
                }
            }

            if (stats_counter % 30 == 0) {
                SystemMonitor::instance().print_system_report();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (g_ws_client) {
        esp_websocket_client_stop(g_ws_client);
        esp_websocket_client_destroy(g_ws_client);
    }
    ESP_LOGI(TAG, "Main Control Task exiting");
    vTaskDelete(NULL);
}
