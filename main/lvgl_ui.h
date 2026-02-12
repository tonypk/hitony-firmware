#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    UI_STATE_BOOT = 0,
    UI_STATE_PROVISIONING,
    UI_STATE_WIFI_CONNECTING,
    UI_STATE_WIFI_CONNECTED,
    UI_STATE_WS_CONNECTED,
    UI_STATE_LISTENING,
    UI_STATE_SPEAKING,
    UI_STATE_MUSIC,
    UI_STATE_ERROR,
} ui_state_t;

typedef enum {
    UI_EXPR_NONE = 0,
    UI_EXPR_HEART,
    UI_EXPR_THUMBS_UP,
    UI_EXPR_GLASSES,
    UI_EXPR_PRAY,
} ui_expression_t;

void lvgl_ui_init();
void lvgl_ui_init_touch(void* i2c_bus_handle);
void lvgl_ui_set_status(const char* text);
void lvgl_ui_set_state(ui_state_t state);
void lvgl_ui_set_expression(ui_expression_t expr);
void lvgl_ui_set_debug_info(const char* info);  // Update debug info display
void lvgl_ui_update_recording_stats(uint32_t opus_count, bool is_recording);  // Update recording stats
void lvgl_ui_set_pupil_offset(int x_offset, int y_offset);  // Manually move pupils (-10 to 10 range)
void lvgl_ui_set_music_energy(float energy);  // Update music rhythm animation (0.0-1.0, called during playback)
void lvgl_ui_set_music_title(const char* title);  // Show song title during music playback
void lvgl_ui_hide_music_title();  // Hide song title
void lcd_only_test();

typedef void (*ui_touch_cb_t)(bool pressed);
void lvgl_ui_set_touch_cb(ui_touch_cb_t cb);

// Boot-time provisioning trigger
bool lvgl_ui_wait_for_touch(uint32_t timeout_ms);  // Wait for touch, returns true if touched
void lvgl_ui_clear_touch_flag();  // Clear touch detection flag

// Device binding info overlay (shown after WiFi connect)
void lvgl_ui_show_binding_info(const char* device_id, const char* token, const char* admin_url);
void lvgl_ui_hide_binding_info();
