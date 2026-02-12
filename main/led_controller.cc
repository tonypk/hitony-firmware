#include "led_controller.h"
#include <esp_log.h>
#include <driver/ledc.h>
#include <math.h>

static const char* TAG = "led_ctrl";

// PWM配置
#define LED_PWM_TIMER       LEDC_TIMER_0
#define LED_PWM_MODE        LEDC_LOW_SPEED_MODE
#define LED_PWM_CHANNEL     LEDC_CHANNEL_0
#define LED_PWM_FREQ_HZ     5000
#define LED_PWM_RESOLUTION  LEDC_TIMER_8_BIT

bool LedController::init(gpio_num_t led_pin) {
    led_pin_ = led_pin;

    // 配置LEDC定时器
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LED_PWM_MODE,
        .duty_resolution = LED_PWM_RESOLUTION,
        .timer_num = LED_PWM_TIMER,
        .freq_hz = LED_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };

    esp_err_t ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC timer: %d", ret);
        return false;
    }

    // 配置LEDC通道
    ledc_channel_config_t ledc_channel = {
        .gpio_num = led_pin_,
        .speed_mode = LED_PWM_MODE,
        .channel = LED_PWM_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LED_PWM_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags = {},
    };

    ret = ledc_channel_config(&ledc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC channel: %d", ret);
        return false;
    }

    ESP_LOGI(TAG, "LED controller initialized on GPIO %d", led_pin_);
    return true;
}

bool LedController::start() {
    if (running_) {
        ESP_LOGW(TAG, "LED controller already running");
        return true;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        led_task,
        "led_ctrl",
        2048,
        this,
        2,  // 低优先级
        &led_task_handle_,
        0   // Core 0
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LED task");
        return false;
    }

    running_ = true;
    ESP_LOGI(TAG, "LED controller started");
    return true;
}

void LedController::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    if (led_task_handle_) {
        vTaskDelete(led_task_handle_);
        led_task_handle_ = nullptr;
    }

    // 关闭LED
    ledc_set_duty(LED_PWM_MODE, LED_PWM_CHANNEL, 0);
    ledc_update_duty(LED_PWM_MODE, LED_PWM_CHANNEL);
}

void LedController::led_task(void* arg) {
    LedController* controller = static_cast<LedController*>(arg);

    const TickType_t update_interval = pdMS_TO_TICKS(20);  // 50Hz更新率

    while (controller->running_) {
        controller->update_led();
        vTaskDelay(update_interval);
    }

    vTaskDelete(nullptr);
}

void LedController::update_led() {
    // 处理临时闪烁
    if (blink_count_ > 0) {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        uint32_t phase = now % (blink_duration_ * 2);

        if (phase < blink_duration_) {
            apply_brightness(target_brightness_);
        } else {
            apply_brightness(0);
        }

        if (phase < 50) {  // 每个周期开始时减少计数
            blink_count_--;
        }
        return;
    }

    // 正常动画
    uint8_t brightness = calculate_brightness();
    apply_brightness(brightness);

    animation_phase_ += (uint32_t)(20 * animation_speed_);  // 每20ms增加
}

uint8_t LedController::calculate_brightness() {
    switch (current_mode_) {
        case AnimationMode::OFF:
            return 0;

        case AnimationMode::SOLID:
            return target_brightness_;

        case AnimationMode::BREATHING: {
            // 正弦呼吸效果，周期约3秒
            float phase = (animation_phase_ % 3000) / 3000.0f * 2.0f * M_PI;
            float intensity = (sinf(phase) + 1.0f) / 2.0f;
            return (uint8_t)(target_brightness_ * intensity);
        }

        case AnimationMode::SLOW_BLINK: {
            // 1秒周期
            uint32_t phase = animation_phase_ % 1000;
            return phase < 500 ? target_brightness_ : 0;
        }

        case AnimationMode::FAST_BLINK: {
            // 200ms周期
            uint32_t phase = animation_phase_ % 200;
            return phase < 100 ? target_brightness_ : 0;
        }

        case AnimationMode::PULSE: {
            // 快速脉冲，周期800ms
            uint32_t phase = animation_phase_ % 800;
            if (phase < 200) {
                float intensity = (float)phase / 200.0f;
                return (uint8_t)(target_brightness_ * intensity);
            } else if (phase < 400) {
                float intensity = (float)(400 - phase) / 200.0f;
                return (uint8_t)(target_brightness_ * intensity);
            } else {
                return 0;
            }
        }

        case AnimationMode::FADE_IN: {
            if (current_brightness_ < target_brightness_) {
                current_brightness_++;
            }
            return current_brightness_;
        }

        case AnimationMode::FADE_OUT: {
            if (current_brightness_ > 0) {
                current_brightness_--;
            }
            return current_brightness_;
        }

        case AnimationMode::HEARTBEAT: {
            // 心跳效果：快速双跳
            uint32_t phase = animation_phase_ % 1200;
            if (phase < 100) {
                return target_brightness_;
            } else if (phase < 200) {
                return 0;
            } else if (phase < 300) {
                return target_brightness_;
            } else {
                return 0;
            }
        }

        default:
            return 0;
    }
}

void LedController::apply_brightness(uint8_t brightness) {
    ledc_set_duty(LED_PWM_MODE, LED_PWM_CHANNEL, brightness);
    ledc_update_duty(LED_PWM_MODE, LED_PWM_CHANNEL);
}

void LedController::set_animation(AnimationMode mode, uint8_t brightness, float speed) {
    current_mode_ = mode;
    target_brightness_ = brightness;
    animation_speed_ = speed;
    animation_phase_ = 0;

    ESP_LOGI(TAG, "Animation set: mode=%d, brightness=%d, speed=%.1f",
             (int)mode, brightness, speed);
}

void LedController::set_system_state(SystemState state) {
    switch (state) {
        case SystemState::BOOTING:
            set_animation(AnimationMode::FAST_BLINK, 128, 1.0f);
            break;

        case SystemState::IDLE:
            set_animation(AnimationMode::BREATHING, 64, 0.8f);
            break;

        case SystemState::LISTENING:
            set_animation(AnimationMode::SOLID, 32, 1.0f);
            break;

        case SystemState::WAKE_DETECTED:
            set_animation(AnimationMode::PULSE, 255, 2.0f);
            break;

        case SystemState::RECORDING:
            set_animation(AnimationMode::SOLID, 255, 1.0f);
            break;

        case SystemState::THINKING:
            set_animation(AnimationMode::BREATHING, 180, 1.5f);
            break;

        case SystemState::SPEAKING:
            set_animation(AnimationMode::PULSE, 200, 1.2f);
            break;

        case SystemState::ERROR:
            set_animation(AnimationMode::FAST_BLINK, 255, 2.0f);
            break;

        case SystemState::NO_WIFI:
            set_animation(AnimationMode::SLOW_BLINK, 128, 1.0f);
            break;

        case SystemState::NO_NETWORK:
            set_animation(AnimationMode::HEARTBEAT, 150, 1.0f);
            break;
    }

    ESP_LOGI(TAG, "System state: %d", (int)state);
}

void LedController::set_brightness(uint8_t brightness) {
    target_brightness_ = brightness;
}

void LedController::set_led(bool on) {
    apply_brightness(on ? target_brightness_ : 0);
}

void LedController::blink_once(uint8_t count, uint32_t duration_ms) {
    blink_count_ = count;
    blink_duration_ = duration_ms;
}
