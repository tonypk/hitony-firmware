#include "lvgl_ui.h"
#include "config.h"
#include "task_manager.h"
#include "audio_i2s.h"

#include <esp_log.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_st77916.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_cst816s.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

#include <lvgl.h>
#include <stdlib.h>

static const char* TAG = "lvgl_ui";

static esp_lcd_panel_io_handle_t panel_io = nullptr;
static esp_lcd_panel_handle_t panel = nullptr;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t* buf1 = nullptr;
static lv_color_t* buf2 = nullptr;
static lv_obj_t* status_label = nullptr;
static lv_obj_t* debug_label = nullptr;  // Debug info label (kept for ESP_LOG, hidden from UI)
static lv_obj_t* eye_left = nullptr;
static lv_obj_t* eye_right = nullptr;
// Pupils removed — eyes are solid color shapes
static lv_obj_t* zzz_label = nullptr;

static lv_obj_t* expr_container = nullptr;
static lv_obj_t* expr_heart = nullptr;
static lv_obj_t* expr_thumb = nullptr;
static lv_obj_t* expr_glasses = nullptr;
static lv_obj_t* expr_pray = nullptr;
static lv_obj_t* touch_layer = nullptr;

static ui_state_t current_state = UI_STATE_BOOT;
static ui_expression_t current_expr = UI_EXPR_NONE;
static lv_timer_t* gaze_timer = nullptr;
static lv_timer_t* blink_timer = nullptr;
static lv_timer_t* state_timer = nullptr;

// === Nomi-style eye configuration ===
// Eyes are rounded rectangles, expressions change shape/position/radius
#define NOMI_EYE_COLOR_HEX  0x4FC3F7   // Soft blue
#define NOMI_CENTER_Y       (-20)       // Eyes vertical center offset

struct NomiEyeParams {
    int16_t x_off;    // X offset from eye center position
    int16_t y_off;    // Y offset from eye center position
    int16_t width;
    int16_t height;
    int16_t radius;
};

struct NomiExpression {
    NomiEyeParams left;
    NomiEyeParams right;
};

enum NomiExprId {
    EXPR_NORMAL = 0,
    EXPR_BLINK,
    EXPR_HAPPY,
    EXPR_WIDE,
    EXPR_SLEEP,
    EXPR_LOOK_LEFT,
    EXPR_LOOK_RIGHT,
    EXPR_LOOK_UP,
    EXPR_ERROR,
    EXPR_COUNT,
};

// Expression library (tuned for 360x360 round display)
//                                  left {x, y, w, h, r}           right {x, y, w, h, r}
static const NomiExpression NOMI_EXPRESSIONS[EXPR_COUNT] = {
    [EXPR_NORMAL]     = {{ -55,   0,  72,  52,  20 }, {  55,   0,  72,  52,  20 }},
    [EXPR_BLINK]      = {{ -55,   0,  72,   6,   3 }, {  55,   0,  72,   6,   3 }},
    [EXPR_HAPPY]      = {{ -55,   5,  72,  20,  10 }, {  55,   5,  72,  20,  10 }},
    [EXPR_WIDE]       = {{ -55,   0,  64,  68,  26 }, {  55,   0,  64,  68,  26 }},
    [EXPR_SLEEP]      = {{ -55,   5,  72,   6,   3 }, {  55,   5,  72,   6,   3 }},
    [EXPR_LOOK_LEFT]  = {{ -68,   0,  72,  52,  20 }, {  42,   0,  72,  52,  20 }},
    [EXPR_LOOK_RIGHT] = {{ -42,   0,  72,  52,  20 }, {  68,   0,  72,  52,  20 }},
    [EXPR_LOOK_UP]    = {{ -55, -10,  72,  52,  20 }, {  55, -10,  72,  52,  20 }},
    [EXPR_ERROR]      = {{ -55,   0,  52,  52,  26 }, {  55,   0,  52,  52,  26 }},
};

// Current animated state (tracks where each eye is right now)
static NomiEyeParams cur_left  = { -55, 0, 72, 52, 20 };
static NomiEyeParams cur_right = {  55, 0, 72, 52, 20 };
static NomiExprId current_base_expr = EXPR_NORMAL;

static esp_lcd_touch_handle_t touch_handle = nullptr;
static i2c_master_bus_handle_t touch_i2c_bus = nullptr;
static ui_touch_cb_t touch_cb = nullptr;
static lv_indev_t* touch_indev = nullptr;

// 触摸唤醒统计
static uint32_t touch_count = 0;
static uint32_t wake_trigger_count = 0;

// Boot-time touch detection for provisioning trigger
static bool g_boot_touch_detected = false;

// --- Gesture detection ---
static bool gesture_tracking = false;
static bool gesture_consumed = false;  // Long press already handled
static uint16_t gesture_start_x = 0;
static uint16_t gesture_start_y = 0;
static uint16_t gesture_last_x = 0;
static uint16_t gesture_last_y = 0;
static uint32_t gesture_start_tick = 0;
#define GESTURE_MIN_DISTANCE  50   // Min pixels for swipe
#define GESTURE_LONG_PRESS_MS 800  // Long press threshold

enum GestureType {
    GESTURE_NONE = 0,
    GESTURE_TAP,
    GESTURE_SWIPE_UP,
    GESTURE_SWIPE_DOWN,
    GESTURE_LONG_PRESS,
};

// --- Volume UI ---
static lv_obj_t* vol_bar = nullptr;
static lv_obj_t* vol_label = nullptr;
static lv_timer_t* vol_hide_timer = nullptr;

// LVGL互斥锁 - 防止多任务同时访问LVGL（LVGL不是线程安全的）
static SemaphoreHandle_t s_lvgl_mutex = nullptr;

static bool lvgl_lock(uint32_t timeout_ms = 100) {
    if (!s_lvgl_mutex) return true;  // 未初始化时直接放行
    return xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void lvgl_unlock() {
    if (s_lvgl_mutex) xSemaphoreGive(s_lvgl_mutex);
}

static const char* state_text(ui_state_t state) {
    switch (state) {
        case UI_STATE_BOOT: return "BOOT: Initializing...";
        case UI_STATE_PROVISIONING: return "SETUP: WiFi Config";
        case UI_STATE_WIFI_CONNECTING: return "WiFi: Connecting...";
        case UI_STATE_WIFI_CONNECTED: return "WiFi: Connected";
        case UI_STATE_WS_CONNECTED: return "READY: Touch to talk";
        case UI_STATE_LISTENING: return "LISTENING...";
        case UI_STATE_SPEAKING: return "SPEAKING...";
        case UI_STATE_MUSIC: return "MUSIC";
        case UI_STATE_ERROR: return "ERROR: Check network";
        default: return "UNKNOWN";
    }
}

// --- Nomi eye animation helpers ---
// Apply NomiEyeParams to an lv_obj (position relative to screen center)
static void apply_eye_params(lv_obj_t* eye, const NomiEyeParams* p) {
    if (!eye) return;
    lv_obj_set_size(eye, p->width, p->height);
    lv_obj_set_style_radius(eye, p->radius, 0);
    lv_obj_align(eye, LV_ALIGN_CENTER, p->x_off, NOMI_CENTER_Y + p->y_off);
}

// Animation callback setters (LVGL anim calls these with interpolated values)
static void anim_set_left_w(void*, int32_t v)  { cur_left.width = v;  if (eye_left) lv_obj_set_width(eye_left, v); }
static void anim_set_left_h(void*, int32_t v)  { cur_left.height = v; if (eye_left) lv_obj_set_height(eye_left, v); }
static void anim_set_left_r(void*, int32_t v)  { cur_left.radius = v; if (eye_left) lv_obj_set_style_radius(eye_left, v, 0); }
static void anim_set_left_x(void*, int32_t v)  { cur_left.x_off = v;  if (eye_left) lv_obj_align(eye_left, LV_ALIGN_CENTER, v, NOMI_CENTER_Y + cur_left.y_off); }
static void anim_set_left_y(void*, int32_t v)  { cur_left.y_off = v;  if (eye_left) lv_obj_align(eye_left, LV_ALIGN_CENTER, cur_left.x_off, NOMI_CENTER_Y + v); }

static void anim_set_right_w(void*, int32_t v) { cur_right.width = v;  if (eye_right) lv_obj_set_width(eye_right, v); }
static void anim_set_right_h(void*, int32_t v) { cur_right.height = v; if (eye_right) lv_obj_set_height(eye_right, v); }
static void anim_set_right_r(void*, int32_t v) { cur_right.radius = v; if (eye_right) lv_obj_set_style_radius(eye_right, v, 0); }
static void anim_set_right_x(void*, int32_t v) { cur_right.x_off = v;  if (eye_right) lv_obj_align(eye_right, LV_ALIGN_CENTER, v, NOMI_CENTER_Y + cur_right.y_off); }
static void anim_set_right_y(void*, int32_t v) { cur_right.y_off = v;  if (eye_right) lv_obj_align(eye_right, LV_ALIGN_CENTER, cur_right.x_off, NOMI_CENTER_Y + v); }

static void start_anim(int32_t from, int32_t to, uint32_t dur, lv_anim_exec_xcb_t cb) {
    if (from == to) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, nullptr);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, dur);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// Animate both eyes to a target expression
static void animate_to_expression(NomiExprId expr_id, uint32_t duration_ms = 200) {
    if (expr_id >= EXPR_COUNT) return;
    const NomiExpression* expr = &NOMI_EXPRESSIONS[expr_id];

    if (duration_ms == 0) {
        // Instant (used during init)
        cur_left = expr->left;
        cur_right = expr->right;
        apply_eye_params(eye_left, &cur_left);
        apply_eye_params(eye_right, &cur_right);
        return;
    }

    // Left eye
    start_anim(cur_left.width,  expr->left.width,  duration_ms, anim_set_left_w);
    start_anim(cur_left.height, expr->left.height, duration_ms, anim_set_left_h);
    start_anim(cur_left.radius, expr->left.radius, duration_ms, anim_set_left_r);
    start_anim(cur_left.x_off,  expr->left.x_off,  duration_ms, anim_set_left_x);
    start_anim(cur_left.y_off,  expr->left.y_off,  duration_ms, anim_set_left_y);

    // Right eye
    start_anim(cur_right.width,  expr->right.width,  duration_ms, anim_set_right_w);
    start_anim(cur_right.height, expr->right.height, duration_ms, anim_set_right_h);
    start_anim(cur_right.radius, expr->right.radius, duration_ms, anim_set_right_r);
    start_anim(cur_right.x_off,  expr->right.x_off,  duration_ms, anim_set_right_x);
    start_anim(cur_right.y_off,  expr->right.y_off,  duration_ms, anim_set_right_y);
}

static void set_expression_visible(ui_expression_t expr) {
    if (!expr_container) return;
    lv_obj_add_flag(expr_heart, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(expr_thumb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(expr_glasses, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(expr_pray, LV_OBJ_FLAG_HIDDEN);
    if (expr == UI_EXPR_HEART) {
        lv_obj_clear_flag(expr_heart, LV_OBJ_FLAG_HIDDEN);
    } else if (expr == UI_EXPR_THUMBS_UP) {
        lv_obj_clear_flag(expr_thumb, LV_OBJ_FLAG_HIDDEN);
    } else if (expr == UI_EXPR_GLASSES) {
        lv_obj_clear_flag(expr_glasses, LV_OBJ_FLAG_HIDDEN);
    } else if (expr == UI_EXPR_PRAY) {
        lv_obj_clear_flag(expr_pray, LV_OBJ_FLAG_HIDDEN);
    }
}


static void apply_state(void* arg) {
    (void)arg;

    if (state_timer) {
        lv_timer_del(state_timer);
        state_timer = nullptr;
    }

    // --- Status text: visible during boot only, hidden after connected ---
    if (status_label) {
        bool is_boot_phase = (current_state == UI_STATE_BOOT ||
                              current_state == UI_STATE_PROVISIONING ||
                              current_state == UI_STATE_WIFI_CONNECTING ||
                              current_state == UI_STATE_WIFI_CONNECTED);
        if (is_boot_phase) {
            lv_label_set_text(status_label, state_text(current_state));
            lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 35);
            lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // --- Nomi eye state mapping ---
    if (eye_left && eye_right) {
        NomiExprId expr;
        switch (current_state) {
            case UI_STATE_BOOT:
            case UI_STATE_PROVISIONING:
            case UI_STATE_WIFI_CONNECTING:
                expr = EXPR_SLEEP;
                break;
            case UI_STATE_LISTENING:
                expr = EXPR_WIDE;
                break;
            case UI_STATE_SPEAKING:
            case UI_STATE_MUSIC:
                expr = EXPR_HAPPY;
                break;
            case UI_STATE_ERROR:
                expr = EXPR_ERROR;
                break;
            default:  // WIFI_CONNECTED, WS_CONNECTED
                expr = EXPR_NORMAL;
                break;
        }
        animate_to_expression(expr, 200);
        current_base_expr = expr;
    }

    if (zzz_label) {
        bool sleeping = (current_state == UI_STATE_BOOT);
        if (sleeping) {
            lv_obj_clear_flag(zzz_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(zzz_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Show/hide music title based on MUSIC state
    if (music_title_label) {
        if (current_state == UI_STATE_MUSIC && music_title_buf[0]) {
            lv_obj_clear_flag(music_title_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(music_title_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void blink_cb(lv_timer_t* t) {
    if (!eye_left || !eye_right || !t) return;
    static bool eyes_closed = false;

    if (!eyes_closed) {
        animate_to_expression(EXPR_BLINK, 80);
        eyes_closed = true;
        lv_timer_set_period(t, 120);
    } else {
        animate_to_expression(current_base_expr, 120);
        eyes_closed = false;
        lv_timer_set_period(t, 3000 + (rand() % 4000));
    }
}

static void gaze_cb(lv_timer_t* t) {
    if (!eye_left || !eye_right || !t) return;

    // Weighted random gaze: 50% center, 50% look around
    static const NomiExprId gaze_options[] = {
        EXPR_NORMAL, EXPR_NORMAL, EXPR_NORMAL,
        EXPR_LOOK_LEFT, EXPR_LOOK_RIGHT, EXPR_LOOK_UP,
    };
    NomiExprId expr = gaze_options[rand() % 6];
    animate_to_expression(expr, 300);
    current_base_expr = expr;

    lv_timer_set_period(t, 2000 + (rand() % 3000));
}

// --- Volume UI helpers ---
static void vol_hide_cb(lv_timer_t* t) {
    (void)t;
    if (vol_bar) lv_obj_add_flag(vol_bar, LV_OBJ_FLAG_HIDDEN);
    if (vol_label) lv_obj_add_flag(vol_label, LV_OBJ_FLAG_HIDDEN);
    if (vol_hide_timer) {
        lv_timer_del(vol_hide_timer);
        vol_hide_timer = nullptr;
    }
}

static void show_volume_ui(int vol, bool muted) {
    if (!vol_bar || !vol_label) return;

    if (muted) {
        lv_label_set_text(vol_label, LV_SYMBOL_MUTE);
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%s %d%%", LV_SYMBOL_VOLUME_MAX, vol);
        lv_label_set_text(vol_label, buf);
    }
    lv_bar_set_value(vol_bar, muted ? 0 : vol, LV_ANIM_ON);
    lv_obj_clear_flag(vol_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(vol_label, LV_OBJ_FLAG_HIDDEN);

    // Auto-hide after 1.5s
    if (vol_hide_timer) lv_timer_del(vol_hide_timer);
    vol_hide_timer = lv_timer_create(vol_hide_cb, 1500, nullptr);
    lv_timer_set_repeat_count(vol_hide_timer, 1);
}

static void handle_gesture(GestureType gesture) {
    auto& audio = AudioI2S::instance();

    switch (gesture) {
        case GESTURE_SWIPE_UP: {
            int vol = audio.get_volume() + 10;
            if (vol > 100) vol = 100;
            audio.set_volume(vol);
            if (audio.is_muted()) audio.set_mute(false);
            show_volume_ui(vol, false);
            break;
        }
        case GESTURE_SWIPE_DOWN: {
            int vol = audio.get_volume() - 10;
            if (vol < 0) vol = 0;
            audio.set_volume(vol);
            show_volume_ui(vol, audio.is_muted());
            break;
        }
        case GESTURE_LONG_PRESS: {
            bool mute = !audio.is_muted();
            audio.set_mute(mute);
            show_volume_ui(audio.get_volume(), mute);
            break;
        }
        default:
            break;
    }
}

static void touch_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
    (void)drv;
    if (!touch_handle) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    esp_lcd_touch_read_data(touch_handle);
    uint16_t x[1] = {0};
    uint16_t y[1] = {0};
    uint8_t count = 0;
    bool touched = esp_lcd_touch_get_coordinates(touch_handle, x, y, nullptr, &count, 1);
    if (touched && count > 0) {
        touch_count++;
        g_boot_touch_detected = true;
        data->state = LV_INDEV_STATE_PR;
        data->point.x = x[0];
        data->point.y = y[0];

        if (!gesture_tracking) {
            // Start tracking new gesture
            gesture_tracking = true;
            gesture_consumed = false;
            gesture_start_x = x[0];
            gesture_start_y = y[0];
            gesture_last_x = x[0];
            gesture_last_y = y[0];
            gesture_start_tick = xTaskGetTickCount();
        } else {
            // Update last known position
            gesture_last_x = x[0];
            gesture_last_y = y[0];

            if (!gesture_consumed) {
                uint32_t held_ms = (xTaskGetTickCount() - gesture_start_tick) * portTICK_PERIOD_MS;
                int dx = (int)gesture_last_x - (int)gesture_start_x;
                int dy = (int)gesture_last_y - (int)gesture_start_y;

                // Check swipe during drag (responsive, don't wait for release)
                if (abs(dy) > GESTURE_MIN_DISTANCE && abs(dy) > abs(dx)) {
                    if (dy < 0) {
                        ESP_LOGI(TAG, "Gesture: SWIPE_UP (dy=%d)", dy);
                        handle_gesture(GESTURE_SWIPE_UP);
                    } else {
                        ESP_LOGI(TAG, "Gesture: SWIPE_DOWN (dy=%d)", dy);
                        handle_gesture(GESTURE_SWIPE_DOWN);
                    }
                    gesture_consumed = true;
                }
                // Check long press (no significant movement)
                else if (held_ms >= GESTURE_LONG_PRESS_MS &&
                         (dx * dx + dy * dy) < (GESTURE_MIN_DISTANCE * GESTURE_MIN_DISTANCE)) {
                    ESP_LOGI(TAG, "Gesture: LONG_PRESS (%lums)", held_ms);
                    handle_gesture(GESTURE_LONG_PRESS);
                    gesture_consumed = true;
                }
            }
        }
    } else {
        data->state = LV_INDEV_STATE_REL;

        if (gesture_tracking) {
            gesture_tracking = false;

            if (!gesture_consumed) {
                // Short tap without swipe → wake trigger with debounce
                static uint32_t last_wake_time = 0;
                uint32_t now = xTaskGetTickCount();
                if (now - last_wake_time > pdMS_TO_TICKS(3000)) {
                    wake_trigger_count++;
                    ESP_LOGI(TAG, "Touch wake #%lu", wake_trigger_count);
                    xEventGroupSetBits(g_audio_event_bits, AUDIO_EVENT_TOUCH_WAKE);
                    last_wake_time = now;
                }
            }
        }
    }
}

static void touch_event_cb(lv_event_t* e) {
    if (!touch_cb) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        ESP_LOGI(TAG, "touch: pressed");
        touch_cb(true);
    } else if (code == LV_EVENT_RELEASED) {
        ESP_LOGI(TAG, "touch: released");
        touch_cb(false);
    }
}

// HiTony vendor init sequence (from official HiTony config)
static const st77916_lcd_init_cmd_t vendor_specific_init_yysj[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){}, 0, 0},
    {0x11, (uint8_t []){}, 0, 0},
    {0x00, (uint8_t []){}, 0, 120},
};
static void lv_tick_cb(void* arg) {
    (void)arg;
    lv_tick_inc(2);
}

static void lvgl_task(void* arg) {
    (void)arg;
    while (true) {
        if (lvgl_lock(50)) {
            lv_timer_handler();
            lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map) {
    int32_t x1 = area->x1;
    int32_t y1 = area->y1;
    int32_t x2 = area->x2 + 1;
    int32_t y2 = area->y2 + 1;
    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, color_map);
    lv_disp_flush_ready(drv);
}

static void set_backlight_level(int level) {
    if (HITONY_QSPI_BL != GPIO_NUM_NC) {
        ESP_ERROR_CHECK(gpio_set_level(HITONY_QSPI_BL, level));
    }
    if (HITONY_QSPI_BL_ALT != GPIO_NUM_NC) {
        ESP_ERROR_CHECK(gpio_set_level(HITONY_QSPI_BL_ALT, level));
    }
}

static void init_display() {
    // Power control (HiTony board sets GPIO9 low)
    if (HITONY_POWER_CTRL != GPIO_NUM_NC) {
        gpio_config_t pwr_cfg = {};
        pwr_cfg.mode = GPIO_MODE_OUTPUT;
        pwr_cfg.pin_bit_mask = (1ULL << HITONY_POWER_CTRL);
        ESP_ERROR_CHECK(gpio_config(&pwr_cfg));
        ESP_ERROR_CHECK(gpio_set_level(HITONY_POWER_CTRL, 0));
        ESP_LOGI(TAG, "LCD power ctrl LOW on GPIO%d", (int)HITONY_POWER_CTRL);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    spi_bus_config_t bus_cfg = {};
#if HITONY_LCD_USE_QSPI
    bus_cfg = HITONY_ST77916_PANEL_BUS_QSPI_CONFIG(
        HITONY_QSPI_PCLK,
        HITONY_QSPI_D0,
        HITONY_QSPI_D1,
        HITONY_QSPI_D2,
        HITONY_QSPI_D3,
        HITONY_DISPLAY_WIDTH * 80 * sizeof(uint16_t));
#else
    bus_cfg.sclk_io_num = HITONY_QSPI_PCLK;
    bus_cfg.mosi_io_num = HITONY_LCD_SPI_MOSI;
    bus_cfg.miso_io_num = -1;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.flags = 0;
#endif
    bus_cfg.max_transfer_sz = HITONY_DISPLAY_WIDTH * 80 * sizeof(uint16_t);
    bus_cfg.intr_flags = 0;
    ESP_ERROR_CHECK(spi_bus_initialize(HITONY_QSPI_LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

#if HITONY_LCD_USE_QSPI
    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(HITONY_QSPI_CS, NULL, NULL);
#else
    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_SPI_CONFIG(HITONY_QSPI_CS, HITONY_QSPI_DC, NULL, NULL);
#endif
    io_cfg.pclk_hz = HITONY_LCD_USE_QSPI ? 10000000 : 100000;
    io_cfg.spi_mode = 0;
    ESP_LOGI(TAG, "LCD SPI: mode=%d pclk=%dHz CS=%d",
             io_cfg.spi_mode, (int)io_cfg.pclk_hz, (int)HITONY_QSPI_CS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)HITONY_QSPI_LCD_HOST, &io_cfg, &panel_io));

    st77916_vendor_config_t vendor_config = {};
    if (HITONY_LCD_USE_CUSTOM_INIT) {
        vendor_config.init_cmds = vendor_specific_init_yysj;
        vendor_config.init_cmds_size = sizeof(vendor_specific_init_yysj) / sizeof(st77916_lcd_init_cmd_t);
    } else {
        vendor_config.init_cmds = nullptr;
        vendor_config.init_cmds_size = 0;
    }
    vendor_config.flags.use_qspi_interface = HITONY_LCD_USE_QSPI ? 1 : 0;

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = HITONY_QSPI_RST;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = HITONY_LCD_BITS_PER_PIXEL;
    panel_cfg.flags.reset_active_high = HITONY_LCD_RESET_ACTIVE_HIGH;
    panel_cfg.vendor_config = &vendor_config;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_cfg, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_swap_xy(panel, HITONY_DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel, HITONY_DISPLAY_MIRROR_X, HITONY_DISPLAY_MIRROR_Y);

    // Backlight on (assume active-low; keep steady)
    uint64_t bl_mask = 0;
    int bl = (int)HITONY_QSPI_BL;
    int bl_alt = (int)HITONY_QSPI_BL_ALT;
    if (bl >= 0) {
        bl_mask |= (1ULL << (uint32_t)bl);
    }
    if (bl_alt >= 0) {
        bl_mask |= (1ULL << (uint32_t)bl_alt);
    }
    if (bl_mask) {
        gpio_config_t bl_cfg = {};
        bl_cfg.mode = GPIO_MODE_OUTPUT;
        bl_cfg.pin_bit_mask = bl_mask;
        esp_err_t bl_err = gpio_config(&bl_cfg);
        if (bl_err != ESP_OK) {
            ESP_LOGW(TAG, "Backlight gpio_config failed (mask=0x%llx, err=0x%x)", (unsigned long long)bl_mask, bl_err);
        } else {
            int on_level = HITONY_BL_ACTIVE_LOW ? 0 : 1;
            ESP_LOGI(TAG, "Backlight pins: BL=%d BL_ALT=%d active_low=%d", bl, bl_alt, HITONY_BL_ACTIVE_LOW);
            // Backlight stays OFF here — turned on after first LVGL frame to avoid garbled screen
            set_backlight_level(!on_level);  // OFF
            ESP_LOGI(TAG, "Backlight OFF (deferred until first frame)");
        }
    }

#if HITONY_LCD_ONLY_TEST
    // Raw panel color test (bypass LVGL) to verify bus/panel works
    {
        uint16_t* line = (uint16_t*)heap_caps_malloc(HITONY_DISPLAY_WIDTH * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (line) {
            const uint16_t colors[4] = {0xFFFF, 0xF800, 0x07E0, 0x001F}; // white/red/green/blue
            for (int c = 0; c < 4; ++c) {
                for (int i = 0; i < HITONY_DISPLAY_WIDTH; ++i) line[i] = colors[c];
                for (int y = 0; y < HITONY_DISPLAY_HEIGHT; ++y) {
                    esp_lcd_panel_draw_bitmap(panel, 0, y, HITONY_DISPLAY_WIDTH, y + 1, line);
                }
                ESP_LOGI(TAG, "Panel color test %d/4", c + 1);
                vTaskDelay(pdMS_TO_TICKS(600));
            }
            heap_caps_free(line);
        }
    }
#endif
}

void lcd_only_test() {
    init_display();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void lvgl_ui_init() {
    // 创建LVGL互斥锁（必须在所有LVGL操作之前）
    s_lvgl_mutex = xSemaphoreCreateMutex();
    assert(s_lvgl_mutex);

    lv_init();
    init_display();

    size_t buf_pixels = HITONY_DISPLAY_WIDTH * 40;
    buf1 = (lv_color_t*)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_DMA);
    buf2 = (lv_color_t*)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_pixels);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = HITONY_DISPLAY_WIDTH;
    disp_drv.ver_res = HITONY_DISPLAY_HEIGHT;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static esp_timer_handle_t tick_timer;
    esp_timer_create_args_t tick_args = {};
    tick_args.callback = &lv_tick_cb;
    tick_args.arg = nullptr;
    tick_args.dispatch_method = ESP_TIMER_TASK;
    tick_args.name = "lv_tick";
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2000)); // 2ms

    // 注意：lvgl_task在所有UI元素创建完成后再启动（见函数末尾）
    // 避免初始化期间多任务同时访问LVGL

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    zzz_label = nullptr;
    status_label = nullptr;

    // Nomi-style eyes: simple LVGL objects with rounded corners
    eye_left = lv_obj_create(lv_scr_act());
    lv_obj_set_style_bg_color(eye_left, lv_color_hex(NOMI_EYE_COLOR_HEX), 0);
    lv_obj_set_style_bg_opa(eye_left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(eye_left, 0, 0);
    lv_obj_clear_flag(eye_left, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    eye_right = lv_obj_create(lv_scr_act());
    lv_obj_set_style_bg_color(eye_right, lv_color_hex(NOMI_EYE_COLOR_HEX), 0);
    lv_obj_set_style_bg_opa(eye_right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(eye_right, 0, 0);
    lv_obj_clear_flag(eye_right, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // Initial state: sleep expression (no animation)
    animate_to_expression(EXPR_SLEEP, 0);
    ESP_LOGI(TAG, "Nomi eyes created (color=#%06X)", NOMI_EYE_COLOR_HEX);


    // Boot-phase status text (shown only during startup, hidden after WS connected)
    status_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_18, 0);
    lv_obj_set_width(status_label, 280);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(status_label, state_text(current_state));
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 35);


    // Fullscreen touch layer
    touch_layer = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(touch_layer);
    lv_obj_set_style_bg_opa(touch_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_size(touch_layer, HITONY_DISPLAY_WIDTH, HITONY_DISPLAY_HEIGHT);
    lv_obj_align(touch_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(touch_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(touch_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(touch_layer, touch_event_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(touch_layer, touch_event_cb, LV_EVENT_RELEASED, nullptr);

    // --- Volume indicator (bottom center, hidden by default) ---
    vol_bar = lv_bar_create(lv_scr_act());
    lv_obj_set_size(vol_bar, 180, 8);
    lv_obj_align(vol_bar, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_bar_set_range(vol_bar, 0, 100);
    lv_bar_set_value(vol_bar, 80, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(vol_bar, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(vol_bar, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_bg_color(vol_bar, lv_color_hex(0x4FC3F7), LV_PART_INDICATOR);
    lv_obj_set_style_radius(vol_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(vol_bar, 4, LV_PART_INDICATOR);
    lv_obj_clear_flag(vol_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(vol_bar, LV_OBJ_FLAG_HIDDEN);

    vol_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(vol_label, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(vol_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(vol_label, "");
    lv_obj_align(vol_label, LV_ALIGN_BOTTOM_MID, 0, -62);
    lv_obj_clear_flag(vol_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(vol_label, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "LVGL initialized");
    current_state = UI_STATE_BOOT;
    apply_state(nullptr);

    // Force one refresh（此时lvgl_task还未启动，安全调用）
    lv_timer_handler();

    // Now turn on backlight — first frame is in LCD VRAM, no garbled screen
    {
        int on_level = HITONY_BL_ACTIVE_LOW ? 0 : 1;
        set_backlight_level(on_level);
        ESP_LOGI(TAG, "Backlight ON (after first frame)");
    }

    // Start animated pupil movement and blinking
    gaze_timer = lv_timer_create(gaze_cb, 2000 + (rand() % 2000), nullptr);
    lv_timer_set_repeat_count(gaze_timer, -1);  // Repeat indefinitely

    blink_timer = lv_timer_create(blink_cb, 3000 + (rand() % 4000), nullptr);
    lv_timer_set_repeat_count(blink_timer, -1);  // Repeat indefinitely

    // 所有UI元素创建完成，现在启动LVGL更新任务
    // 从此刻起，只有lvgl_task负责调用lv_timer_handler()
    xTaskCreate(lvgl_task, "lvgl", 3072, nullptr, 5, nullptr);
    ESP_LOGI(TAG, "LVGL task started (sole handler for lv_timer_handler)");
}

void lvgl_ui_init_touch(void* i2c_bus_handle) {
    if (touch_handle || !i2c_bus_handle) return;
    touch_i2c_bus = (i2c_master_bus_handle_t)i2c_bus_handle;

    esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();

    ESP_LOGI(TAG, "Touch I2C: port=%d SDA=%d SCL=%d INT=%d",
        (int)HITONY_I2C_PORT, (int)HITONY_I2C_SDA, (int)HITONY_I2C_SCL, (int)HITONY_TP_INT);

    // Optional reset pulse for touch controller
    if (HITONY_TP_RST != GPIO_NUM_NC) {
        gpio_config_t rst_cfg = {};
        rst_cfg.mode = GPIO_MODE_OUTPUT;
        rst_cfg.pin_bit_mask = (1ULL << (uint32_t)HITONY_TP_RST);
        ESP_ERROR_CHECK(gpio_config(&rst_cfg));
        gpio_set_level(HITONY_TP_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(HITONY_TP_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // HiTony uses CST816S at 0x15 on the shared audio I2C bus
    const uint8_t use_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;
    if (i2c_master_probe(touch_i2c_bus, use_addr, pdMS_TO_TICKS(100)) != ESP_OK) {
        ESP_LOGW(TAG, "Touch I2C probe failed (no device at 0x%02x)", use_addr);
        return;
    }

    esp_err_t io_err = esp_lcd_new_panel_io_i2c(touch_i2c_bus, &tp_io_config, &tp_io_handle);
    if (io_err != ESP_OK) {
        ESP_LOGW(TAG, "Touch I2C io init failed (err=0x%x)", io_err);
        return;
    }

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max = HITONY_DISPLAY_WIDTH;
    tp_cfg.y_max = HITONY_DISPLAY_HEIGHT;
    tp_cfg.rst_gpio_num = HITONY_TP_RST;
    tp_cfg.int_gpio_num = HITONY_TP_INT;
    tp_cfg.levels.reset = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy = HITONY_DISPLAY_SWAP_XY;
    tp_cfg.flags.mirror_x = HITONY_DISPLAY_MIRROR_X;
    tp_cfg.flags.mirror_y = HITONY_DISPLAY_MIRROR_Y;

    esp_err_t tp_err = esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &touch_handle);
    if (tp_err != ESP_OK) {
        ESP_LOGW(TAG, "Touch init failed (addr=0x%02x err=0x%x)", use_addr, tp_err);
        return;
    }

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    touch_indev = lv_indev_drv_register(&indev_drv);
    (void)touch_indev;
}

void lvgl_ui_set_status(const char* text) {
    if (status_label && lvgl_lock(50)) {
        if (!lv_obj_has_flag(status_label, LV_OBJ_FLAG_HIDDEN)) {
            lv_label_set_text(status_label, text);
            lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 35);
        }
        lvgl_unlock();
    }
}

void lvgl_ui_set_state(ui_state_t state) {
    current_state = state;
    if (lvgl_lock(50)) {
        lv_async_call(apply_state, nullptr);
        lvgl_unlock();
    }
}

void lvgl_ui_set_expression(ui_expression_t expr) {
    current_expr = expr;
    if (lvgl_lock(50)) {
        if (expr_container) {
            set_expression_visible(expr);
        }
        lvgl_unlock();
    }
}

void lvgl_ui_set_touch_cb(ui_touch_cb_t cb) {
    touch_cb = cb;
}

void lvgl_ui_set_debug_info(const char* info) {
    // Debug info is now log-only, not displayed on screen.
    ESP_LOGD(TAG, "Debug: %s", info);
}

void lvgl_ui_update_recording_stats(uint32_t opus_count, bool is_recording) {
    // Stats are log-only now (no on-screen text)
    ESP_LOGD(TAG, "Recording stats: opus=%lu recording=%d touch=%lu",
             opus_count, is_recording, touch_count);
}

void lvgl_ui_set_pupil_offset(int x_offset, int y_offset) {
    // Pupils removed — no-op
    (void)x_offset;
    (void)y_offset;
}

// Boot-time touch detection for provisioning trigger
bool lvgl_ui_wait_for_touch(uint32_t timeout_ms) {
    g_boot_touch_detected = false;  // Reset flag

    ESP_LOGI(TAG, "Waiting for touch (timeout: %lu ms)...", timeout_ms);

    uint32_t start_time = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start_time) < timeout_ticks) {
        if (g_boot_touch_detected) {
            ESP_LOGI(TAG, "Touch detected! Entering provisioning mode");
            return true;
        }
        // 注意：不在这里调用lv_timer_handler()
        // lvgl_task已经在独立任务中运行，负责所有LVGL更新和触摸事件处理
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGI(TAG, "Timeout - no touch detected");
    return false;
}

void lvgl_ui_clear_touch_flag() {
    g_boot_touch_detected = false;
}

// === Music Title Display ===
static lv_obj_t* music_title_label = nullptr;
static char music_title_buf[128] = {0};

void lvgl_ui_set_music_title(const char* title) {
    if (!title || !title[0]) return;
    if (!lvgl_lock(100)) return;

    snprintf(music_title_buf, sizeof(music_title_buf), "%s", title);

    if (!music_title_label) {
        music_title_label = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_color(music_title_label, lv_color_hex(0xE0E0E0), 0);
        lv_obj_set_style_text_font(music_title_label, &lv_font_simsun_16_cjk, 0);
        lv_obj_set_width(music_title_label, 240);
        lv_obj_set_style_text_align(music_title_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(music_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_clear_flag(music_title_label, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_label_set_text(music_title_label, music_title_buf);
    lv_obj_align(music_title_label, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_clear_flag(music_title_label, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "Music title display: %s", music_title_buf);
    lvgl_unlock();
}

void lvgl_ui_hide_music_title() {
    if (!lvgl_lock(10)) return;
    music_title_buf[0] = '\0';
    if (music_title_label) {
        lv_obj_add_flag(music_title_label, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_unlock();
}

// === Music Rhythm Animation ===
static lv_obj_t* headphone_icon = nullptr;  // 耳机图标
static float last_music_energy = 0.0f;      // 上次音乐能量
static uint32_t last_beat_time = 0;         // 上次节拍时间
static bool music_animation_active = false; // 音乐动画激活状态

// 创建耳机图标（使用Unicode音符符号）
static void create_headphone_icon() {
    if (headphone_icon) return;

    // 使用彩色圆形替代符号（更明显）
    headphone_icon = lv_obj_create(lv_scr_act());
    lv_obj_set_size(headphone_icon, 20, 20);  // 20x20像素的圆
    lv_obj_set_style_radius(headphone_icon, 10, 0);  // 圆形
    lv_obj_set_style_bg_color(headphone_icon, lv_color_hex(0x00FFFF), 0);  // 青色
    lv_obj_set_style_bg_opa(headphone_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(headphone_icon, 0, 0);  // 无边框
    lv_obj_align(headphone_icon, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_add_flag(headphone_icon, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏
}

void lvgl_ui_set_music_energy(float energy) {
    if (!lvgl_lock(10)) return;

    // 仅在MUSIC状态时处理音乐动画（避免TTS也触发）
    if (current_state != UI_STATE_MUSIC) {
        // 非MUSIC状态时确保图标隐藏
        if (headphone_icon) {
            lv_obj_add_flag(headphone_icon, LV_OBJ_FLAG_HIDDEN);
        }
        music_animation_active = false;
        lvgl_unlock();
        return;
    }

    // 首次调用时创建耳机图标
    if (!headphone_icon) {
        create_headphone_icon();
    }

    // 显示耳机图标
    if (headphone_icon && !music_animation_active) {
        lv_obj_clear_flag(headphone_icon, LV_OBJ_FLAG_HIDDEN);
        music_animation_active = true;
    }

    // 如果能量为0，表示音乐停止，隐藏耳机图标
    if (energy <= 0.01f) {
        if (headphone_icon) {
            lv_obj_add_flag(headphone_icon, LV_OBJ_FLAG_HIDDEN);
        }
        music_animation_active = false;
        last_music_energy = 0.0f;
        lvgl_unlock();
        return;
    }

    uint32_t now = xTaskGetTickCount();

    // 节拍检测：能量突然增大（超过阈值）= 节拍
    const float BEAT_THRESHOLD = 0.35f;  // 节拍检测阈值
    const uint32_t MIN_BEAT_INTERVAL = pdMS_TO_TICKS(200);  // 最小节拍间隔200ms（防止抖动）

    bool is_beat = (energy > BEAT_THRESHOLD &&
                    energy > last_music_energy * 1.5f &&  // 能量增加50%以上
                    (now - last_beat_time) > MIN_BEAT_INTERVAL);

    if (is_beat) {
        last_beat_time = now;

        // 根据能量强度选择动画
        if (energy > 0.7f) {
            // 强节拍：表情变化
            animate_to_expression(EXPR_HAPPY, 150);
        } else if (energy > 0.5f) {
            // 中等节拍：轻微表情变化
            animate_to_expression(EXPR_HAPPY, 100);
        } else {
            // 弱节拍：瞳孔轻微缩放（通过快速动画实现）
            animate_to_expression(EXPR_NORMAL, 80);
        }

        // 圆形图标颜色闪烁动画
        if (headphone_icon) {
            static bool color_toggle = false;
            color_toggle = !color_toggle;
            lv_color_t color = color_toggle ? lv_color_hex(0x00FFFF) : lv_color_hex(0xFF00FF);  // 青色/粉色交替
            lv_obj_set_style_bg_color(headphone_icon, color, 0);
        }
    }

    last_music_energy = energy;
    lvgl_unlock();
}

// === Device Binding Info (scrolling text at top, for provisioning mode) ===
static lv_obj_t* binding_label = nullptr;

void lvgl_ui_show_binding_info(const char* device_id, const char* token, const char* admin_url) {
    if (!lvgl_lock(100)) return;

    if (binding_label) {
        lv_obj_del(binding_label);
        binding_label = nullptr;
    }

    // Single scrolling label at top
    binding_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(binding_label, lv_color_hex(0x4FC3F7), 0);
    lv_obj_set_style_text_font(binding_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(binding_label, 260);  // Constrain width for circular display
    lv_label_set_long_mode(binding_label, LV_LABEL_LONG_SCROLL_CIRCULAR);

    static char scroll_buf[256];
    snprintf(scroll_buf, sizeof(scroll_buf),
             "ID: %s   Token: %s   Bind: %s",
             device_id, token, admin_url);
    lv_label_set_text(binding_label, scroll_buf);
    lv_obj_align(binding_label, LV_ALIGN_TOP_MID, 0, 30);

    ESP_LOGI(TAG, "Binding info (scroll): %s", scroll_buf);
    lvgl_unlock();
}

void lvgl_ui_hide_binding_info() {
    if (!lvgl_lock(100)) return;
    if (binding_label) {
        lv_obj_del(binding_label);
        binding_label = nullptr;
    }
    lvgl_unlock();
}
