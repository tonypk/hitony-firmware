#include "task_manager.h"
#include <esp_log.h>
#include <esp_system.h>
#include <string.h>

static const char* TAG = "task_mgr";

bool TaskManager::init() {
    ESP_LOGI(TAG, "Task Manager initialized");
    return true;
}

bool TaskManager::create_task(const TaskDef& def) {
    BaseType_t ret;

    if (def.core_id == -1) {
        // 任意核心
        ret = xTaskCreate(
            def.func,
            def.name,
            def.stack_size,
            def.param,
            def.priority,
            def.handle
        );
    } else {
        // 绑定到指定核心
        ret = xTaskCreatePinnedToCore(
            def.func,
            def.name,
            def.stack_size,
            def.param,
            def.priority,
            def.handle,
            def.core_id
        );
    }

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task: %s", def.name);
        return false;
    }

    if (def.handle && *def.handle) {
        tasks_.push_back(*def.handle);
    }

    ESP_LOGI(TAG, "Created task: %s (stack=%u, prio=%u, core=%d)",
             def.name, def.stack_size, def.priority, def.core_id);

    return true;
}

bool TaskManager::create_tasks(const std::vector<TaskDef>& defs) {
    for (const auto& def : defs) {
        if (!create_task(def)) {
            return false;
        }
    }
    return true;
}

std::vector<TaskManager::TaskStats> TaskManager::get_task_stats() {
    std::vector<TaskStats> stats;
    // TODO: uxTaskGetSystemState需要在sdkconfig中启用configUSE_TRACE_FACILITY
    // 暂时返回空列表
    ESP_LOGW(TAG, "Task stats not available (configUSE_TRACE_FACILITY disabled)");
    return stats;
}

void TaskManager::print_task_stats() {
    auto stats = get_task_stats();

    ESP_LOGI(TAG, "Task Statistics:");
    ESP_LOGI(TAG, "%-16s %5s %8s %8s %6s", "Name", "Prio", "Stack", "Free", "CPU%");
    ESP_LOGI(TAG, "========================================================");

    for (const auto& stat : stats) {
        ESP_LOGI(TAG, "%-16s %5u %8u %8u %6.2f",
                 stat.name,
                 stat.priority,
                 stat.stack_size,
                 stat.stack_free,
                 stat.cpu_usage);
    }
}

void TaskManager::monitor_stack_usage() {
    auto stats = get_task_stats();

    for (const auto& stat : stats) {
        // 警告：栈使用超过80%
        if (stat.stack_free < stat.stack_size * 0.2f) {
            ESP_LOGW(TAG, "Task %s: stack usage high! Free=%u/%u",
                     stat.name, stat.stack_free, stat.stack_size);
        }
    }
}

void TaskManager::get_cpu_usage(float& core0_usage, float& core1_usage) {
    // TODO: 实现每个核心的CPU使用率计算
    core0_usage = 0.0f;
    core1_usage = 0.0f;
}
