# 新功能集成指南

## 🎯 快速开始

### 1. SystemMonitor 集成

在 `main/main.cc` 的 `app_main()` 中添加：

```cpp
#include "system_monitor.h"

// 在Phase 6: 启动监控任务之后
ESP_LOGI(TAG, "[Phase 7] Starting system monitor...");
if (!SystemMonitor::instance().init()) {
    ESP_LOGE(TAG, "Failed to init system monitor!");
}
if (!SystemMonitor::instance().start()) {
    ESP_LOGE(TAG, "Failed to start system monitor!");
}
```

在 `heartbeat_task()` 中添加系统报告：

```cpp
// 在control_tasks.cc的heartbeat_task中
if (count % 20 == 0) {  // 每20秒打印一次详细报告
    SystemMonitor::instance().print_system_report();
}
```

### 2. LedController 集成

在 `main/main.cc` 初始化：

```cpp
#include "led_controller.h"
#include "config.h"

// Phase 7: 初始化LED控制器
ESP_LOGI(TAG, "[Phase 7] Initializing LED controller...");
if (!LedController::instance().init(HITONY_LED_G)) {
    ESP_LOGE(TAG, "Failed to init LED controller!");
}
if (!LedController::instance().start()) {
    ESP_LOGE(TAG, "Failed to start LED controller!");
}

// 设置启动状态
LedController::instance().set_system_state(LedController::SystemState::BOOTING);
```

在 `state_machine_task()` 中更新LED状态：

```cpp
// control_tasks.cc
#include "led_controller.h"

void state_machine_task(void* arg) {
    // ...
    
    switch (event.type) {
        case STATE_EVENT_WIFI_CONNECTED:
            LedController::instance().set_system_state(
                LedController::SystemState::IDLE);
            break;
            
        case STATE_EVENT_WAKE_DETECTED:
            LedController::instance().set_system_state(
                LedController::SystemState::WAKE_DETECTED);
            break;
            
        case STATE_EVENT_VAD_START:
            LedController::instance().set_system_state(
                LedController::SystemState::RECORDING);
            break;
            
        case STATE_EVENT_TTS_START:
            LedController::instance().set_system_state(
                LedController::SystemState::SPEAKING);
            break;
    }
}
```

### 3. 队列监控集成

在各个任务中添加队列使用监控：

```cpp
// audio_tasks.cc
#include "system_monitor.h"

void audio_input_task(void* arg) {
    while (1) {
        // ... 发送队列
        if (xQueueSend(g_audio_input_queue, &msg, 0) == pdTRUE) {
            // 记录队列使用情况
            UBaseType_t used = uxQueueMessagesWaiting(g_audio_input_queue);
            SystemMonitor::instance().record_queue_usage(
                "audio_input", used, 4);
        }
    }
}
```

## 📊 使用示例

### 查看系统状态

```cpp
// 获取内存统计
auto mem = SystemMonitor::instance().get_memory_stats();
ESP_LOGI(TAG, "Internal RAM: %.1f%% used", mem.internal_usage);
ESP_LOGI(TAG, "PSRAM: %.1f%% used", mem.psram_usage);

// 获取健康状态
auto health = SystemMonitor::instance().get_health_status();
if (health == SystemMonitor::HealthStatus::CRITICAL) {
    ESP_LOGE(TAG, "System health critical!");
}
```

### LED动画控制

```cpp
// 设置自定义动画
LedController::instance().set_animation(
    LedController::AnimationMode::BREATHING,  // 呼吸灯
    128,    // 亮度 0-255
    1.5f    // 速度倍率
);

// 临时闪烁3次
LedController::instance().blink_once(3, 200);
```

## 🔧 编译和测试

```bash
# 1. 清理并重新编译
idf.py fullclean
idf.py build

# 2. 烧录
idf.py flash

# 3. 查看日志
idf.py monitor
```

## ✅ 验证清单

- [ ] 编译无错误
- [ ] 所有任务成功创建
- [ ] SystemMonitor 每5秒更新统计
- [ ] LED 根据状态变化
- [ ] 系统报告每20秒打印
- [ ] 队列使用率被正确记录

## 🐛 常见问题

### Q: 编译错误 "driver component not found"
A: 确保CMakeLists.txt中添加了 `driver` 到REQUIRES列表

### Q: LED不亮或闪烁异常
A: 检查GPIO引脚配置，确认HITONY_LED_G定义正确

### Q: 系统监控数据全为0
A: 确保调用了 `init()` 和 `start()`

## 📈 下一步

1. 实现 VAD 端点检测
2. 添加 Watchdog 定时器
3. 优化 WebSocket 重连
4. 完善 UI 状态显示
