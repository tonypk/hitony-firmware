#pragma once

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdint>

/**
 * @brief LED控制器 - 提供丰富的LED动画和状态指示
 *
 * 支持多种动画模式：
 * - 呼吸灯效果
 * - 闪烁
 * - 渐变
 * - 脉冲
 */
class LedController {
public:
    // LED动画模式
    enum class AnimationMode {
        OFF,                // 关闭
        SOLID,              // 常亮
        BREATHING,          // 呼吸灯
        SLOW_BLINK,         // 慢速闪烁
        FAST_BLINK,         // 快速闪烁
        PULSE,              // 脉冲
        FADE_IN,            // 淡入
        FADE_OUT,           // 淡出
        HEARTBEAT,          // 心跳
    };

    // 系统状态对应的LED模式
    enum class SystemState {
        BOOTING,            // 启动中 - 快速闪烁
        IDLE,               // 空闲 - 慢速呼吸
        LISTENING,          // 监听中 - 常亮低亮度
        WAKE_DETECTED,      // 检测到唤醒词 - 快速脉冲
        RECORDING,          // 录音中 - 常亮
        THINKING,           // 处理中 - 呼吸灯
        SPEAKING,           // 播放中 - 脉冲
        ERROR,              // 错误 - 快速闪烁
        NO_WIFI,            // 无WiFi - 慢速闪烁
        NO_NETWORK,         // 无网络 - 双闪
    };

    static LedController& instance() {
        static LedController inst;
        return inst;
    }

    /**
     * @brief 初始化LED控制器
     *
     * @param led_pin LED GPIO引脚
     */
    bool init(gpio_num_t led_pin);

    /**
     * @brief 启动LED控制任务
     */
    bool start();

    /**
     * @brief 停止LED控制任务
     */
    void stop();

    /**
     * @brief 设置LED动画模式
     *
     * @param mode 动画模式
     * @param brightness 亮度 (0-255)
     * @param speed 速度倍率 (0.1 - 10.0)
     */
    void set_animation(AnimationMode mode, uint8_t brightness = 255, float speed = 1.0f);

    /**
     * @brief 根据系统状态自动设置LED
     *
     * @param state 系统状态
     */
    void set_system_state(SystemState state);

    /**
     * @brief 设置LED亮度
     *
     * @param brightness 0-255
     */
    void set_brightness(uint8_t brightness);

    /**
     * @brief 直接设置LED状态（覆盖动画）
     *
     * @param on true=亮, false=灭
     */
    void set_led(bool on);

    /**
     * @brief 临时闪烁N次（不影响当前动画）
     *
     * @param count 闪烁次数
     * @param duration_ms 每次闪烁持续时间
     */
    void blink_once(uint8_t count = 1, uint32_t duration_ms = 100);

private:
    LedController() = default;

    gpio_num_t led_pin_ = GPIO_NUM_NC;
    TaskHandle_t led_task_handle_ = nullptr;
    bool running_ = false;

    // 当前动画状态
    AnimationMode current_mode_ = AnimationMode::OFF;
    uint8_t target_brightness_ = 255;
    uint8_t current_brightness_ = 0;
    float animation_speed_ = 1.0f;

    // 动画参数
    uint32_t animation_phase_ = 0;
    uint32_t last_update_time_ = 0;

    // 临时闪烁
    uint8_t blink_count_ = 0;
    uint32_t blink_duration_ = 0;

    static void led_task(void* arg);
    void update_led();
    uint8_t calculate_brightness();
    void apply_brightness(uint8_t brightness);
};
