#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <functional>
#include <map>
#include "task_manager.h"

/**
 * @brief 应用状态机
 *
 * 管理应用的状态转换，处理各种事件
 */
class StateMachine {
public:
    // 应用状态
    typedef enum {
        STATE_BOOT,              // 启动中
        STATE_PROVISIONING,      // 配网中
        STATE_WIFI_CONNECTING,   // WiFi连接中
        STATE_WIFI_CONNECTED,    // WiFi已连接
        STATE_WS_CONNECTING,     // WebSocket连接中
        STATE_WS_CONNECTED,      // WebSocket已连接（空闲）
        STATE_LISTENING,         // 录音中
        STATE_PROCESSING,        // 云端处理中
        STATE_SPEAKING,          // TTS播放中
        STATE_ERROR,             // 错误状态
    } State;

    // 状态转换回调
    using StateCallback = std::function<void(State from, State to)>;

    static StateMachine& instance() {
        static StateMachine inst;
        return inst;
    }

    /**
     * @brief 初始化状态机
     */
    bool init();

    /**
     * @brief 获取当前状态
     */
    State get_state() const { return current_state_; }

    /**
     * @brief 获取状态名称
     */
    const char* get_state_name(State state) const;

    /**
     * @brief 转换到新状态
     * @return true 转换成功，false 非法转换
     */
    bool transition_to(State new_state);

    /**
     * @brief 处理事件
     */
    void handle_event(const state_event_msg_t& event);

    /**
     * @brief 设置状态转换回调
     */
    void on_state_changed(StateCallback cb) { state_cb_ = cb; }

    /**
     * @brief 状态机主循环（运行在state_machine_task中）
     */
    void run();

    /**
     * @brief 检查状态转换是否合法
     */
    bool is_valid_transition(State from, State to) const;

private:
    StateMachine() = default;

    // 状态处理函数
    void on_enter_boot();
    void on_enter_provisioning();
    void on_enter_wifi_connecting();
    void on_enter_wifi_connected();
    void on_enter_ws_connecting();
    void on_enter_ws_connected();
    void on_enter_listening();
    void on_enter_processing();
    void on_enter_speaking();
    void on_enter_error();

    void on_exit_boot();
    void on_exit_provisioning();
    void on_exit_wifi_connecting();
    void on_exit_wifi_connected();
    void on_exit_ws_connecting();
    void on_exit_ws_connected();
    void on_exit_listening();
    void on_exit_processing();
    void on_exit_speaking();
    void on_exit_error();

    // 事件处理
    void handle_wifi_connected();
    void handle_wifi_disconnected();
    void handle_ws_connected();
    void handle_ws_disconnected();
    void handle_wake_detected(const char* wake_word);
    void handle_vad_start();
    void handle_vad_end();
    void handle_tts_start();
    void handle_tts_end();
    void handle_touch_pressed();
    void handle_error(const char* message);

    State current_state_ = STATE_BOOT;
    State previous_state_ = STATE_BOOT;

    StateCallback state_cb_;

    // 状态转换表（定义合法的转换）
    std::map<State, std::vector<State>> transition_table_;

    // 统计
    uint32_t state_enter_time_ = 0;
    uint32_t total_wake_count_ = 0;
    uint32_t total_interaction_count_ = 0;
};

// ============================================================================
// 状态机任务实现
// ============================================================================

inline void state_machine_task(void* arg) {
    StateMachine::instance().run();
}
