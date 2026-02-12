#include "task_manager.h"
#include <esp_log.h>

static const char* TAG = "app_init";

// === 2任务架构 ===
// 任务句柄在create_all_tasks()函数中声明

// 旧任务句柄（已弃用）
#if 0
static TaskHandle_t h_audio_input = nullptr;
static TaskHandle_t h_audio_output = nullptr;
static TaskHandle_t h_afe_process = nullptr;
static TaskHandle_t h_wake_detect = nullptr;
static TaskHandle_t h_audio_mixer = nullptr;
static TaskHandle_t h_opus_decode = nullptr;
static TaskHandle_t h_opus_encode = nullptr;
static TaskHandle_t h_websocket = nullptr;
static TaskHandle_t h_state_machine = nullptr;
static TaskHandle_t h_ui_update = nullptr;
static TaskHandle_t h_led_control = nullptr;
static TaskHandle_t h_heartbeat = nullptr;
#endif

// 任务句柄
static TaskHandle_t h_audio_main = nullptr;
static TaskHandle_t h_main_ctrl = nullptr;

// 暂停/恢复音频任务（用于配网模式释放内存）
void suspend_audio_tasks() {
    ESP_LOGI(TAG, "⏸️ Suspending audio tasks to free memory for provisioning...");
    if (h_audio_main) {
        vTaskSuspend(h_audio_main);
        ESP_LOGI(TAG, "Audio task suspended");
    }
    // 延迟确保任务完全暂停
    vTaskDelay(pdMS_TO_TICKS(200));
}

bool create_all_tasks() {
    ESP_LOGI(TAG, "Creating 2-task architecture...");

    TaskManager& tm = TaskManager::instance();

    // 新架构：2个任务替代原来的13个任务
    // ⚠️ 紧急：减少栈大小以释放内部RAM（当前96.7%使用，WebSocket任务无法创建）
    std::vector<TaskManager::TaskDef> tasks = {
        // Core 1 - Audio Main Task（整合7个音频任务）
        {
            .name = "audio_main",
            .func = audio_main_task,
            .stack_size = 40960,  // 40KB (Opus encoder needs ~31KB stack)
            .priority = 20,       // 高优先级保证实时性
            .core_id = 1,
            .param = nullptr,
            .handle = &h_audio_main,
        },

        // Core 0 - Main Control Task（整合6个控制任务）
        {
            .name = "main_ctrl",
            .func = main_control_task,
            .stack_size = 8192,   // 8KB (从12KB减少，释放4KB RAM)
            .priority = 10,       // 中等优先级
            .core_id = 0,
            .param = nullptr,
            .handle = &h_main_ctrl,
        },
    };

    // 批量创建任务
    if (!tm.create_tasks(tasks)) {
        ESP_LOGE(TAG, "Failed to create tasks");
        return false;
    }

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "2-task architecture created successfully!");
    ESP_LOGI(TAG, "Total stack: 40KB (32+8, optimized for RAM)");
    ESP_LOGI(TAG, "Core 0: main_ctrl (8KB) - WebSocket, FSM, UI, LED, Heartbeat");
    ESP_LOGI(TAG, "Core 1: audio_main (32KB) - I2S, AFE, Wake, Opus, Mixer");
    ESP_LOGI(TAG, "AFE task (12KB) + LED task (2KB) created separately");
    ESP_LOGI(TAG, "Total app stack: 52KB (vs 82KB before optimization, -37%%)");
    ESP_LOGI(TAG, "==================================================");

    return true;
}

// 旧任务定义（已弃用，保留用于参考）
#if 0
// === 旧架构：13任务 ===
    {
        .name = "audio_input",
        .func = audio_input_task,
        .stack_size = 5120,
        .priority = 20,
        .core_id = 1,
    },
    {
        .name = "audio_output",
        .func = audio_output_task,
        .stack_size = 5120,
        .priority = 19,
        .core_id = 1,
    },
    {
        .name = "afe_process",
        .func = afe_process_task,
        .stack_size = 8192,
        .priority = 18,
        .core_id = 1,
    },
    {
        .name = "websocket",
        .func = websocket_task,
        .stack_size = 6144,
        .priority = 8,
        .core_id = 0,
    },
    {
        .name = "state_machine",
        .func = state_machine_task,
        .stack_size = 6144,
        .priority = 6,
        .core_id = 0,
    },
    {
        .name = "led_control",
        .func = led_control_task,
        .stack_size = 2560,
        .priority = 2,
        .core_id = 0,
    },
    {
        .name = "heartbeat",
        .func = heartbeat_task,
        .stack_size = 4096,
        .priority = 1,
        .core_id = 0,
    },
#endif
