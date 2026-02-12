#include "system_monitor.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char* TAG = "sys_monitor";

bool SystemMonitor::init() {
    ESP_LOGI(TAG, "Initializing system monitor");

    // 初始化统计数据
    memset(&cpu_stats_, 0, sizeof(cpu_stats_));
    memset(&memory_stats_, 0, sizeof(memory_stats_));
    memset(&audio_latency_, 0, sizeof(audio_latency_));
    memset(&network_stats_, 0, sizeof(network_stats_));

    return true;
}

bool SystemMonitor::start() {
    if (running_) {
        ESP_LOGW(TAG, "Monitor already running");
        return true;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        monitor_task,
        "sys_monitor",
        2048,  // 减小到2KB节省内存
        this,
        5,  // 中等优先级
        &monitor_task_handle_,
        0   // Core 0
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create monitor task");
        return false;
    }

    running_ = true;
    ESP_LOGI(TAG, "System monitor started");
    return true;
}

void SystemMonitor::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    if (monitor_task_handle_) {
        vTaskDelete(monitor_task_handle_);
        monitor_task_handle_ = nullptr;
    }
}

void SystemMonitor::monitor_task(void* arg) {
    SystemMonitor* monitor = static_cast<SystemMonitor*>(arg);

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t interval = pdMS_TO_TICKS(5000);  // 每5秒更新一次

    while (monitor->running_) {
        monitor->update_stats();
        vTaskDelayUntil(&last_wake, interval);
    }

    vTaskDelete(nullptr);
}

void SystemMonitor::update_stats() {
    // 更新内存统计
    memory_stats_.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    memory_stats_.internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    memory_stats_.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    memory_stats_.psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    memory_stats_.largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    if (memory_stats_.internal_total > 0) {
        memory_stats_.internal_usage =
            (1.0f - (float)memory_stats_.internal_free / memory_stats_.internal_total) * 100.0f;
    }

    if (memory_stats_.psram_total > 0) {
        memory_stats_.psram_usage =
            (1.0f - (float)memory_stats_.psram_free / memory_stats_.psram_total) * 100.0f;
    }

    // CPU使用率需要基于任务运行时间计算
    // 这里简化处理，可以后续使用FreeRTOS的vTaskGetRunTimeStats优化
    cpu_stats_.core0_usage = 0.0f;  // TODO: 实现真实的CPU使用率
    cpu_stats_.core1_usage = 0.0f;
}

SystemMonitor::CpuStats SystemMonitor::get_cpu_stats() {
    return cpu_stats_;
}

SystemMonitor::MemoryStats SystemMonitor::get_memory_stats() {
    // 实时更新
    MemoryStats stats;
    stats.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    stats.internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    stats.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    stats.psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    stats.largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    if (stats.internal_total > 0) {
        stats.internal_usage =
            (1.0f - (float)stats.internal_free / stats.internal_total) * 100.0f;
    }

    if (stats.psram_total > 0) {
        stats.psram_usage =
            (1.0f - (float)stats.psram_free / stats.psram_total) * 100.0f;
    }

    return stats;
}

void SystemMonitor::get_queue_stats(QueueStats* stats, size_t max_count, size_t* actual_count) {
    size_t count = 0;

    for (size_t i = 0; i < queue_peak_count_ && count < max_count; i++) {
        stats[count].name = queue_peaks_[i].name;
        stats[count].size = queue_peaks_[i].capacity;
        stats[count].used = 0;  // 需要外部更新
        stats[count].peak = queue_peaks_[i].peak_usage;
        stats[count].usage = queue_peaks_[i].capacity > 0 ?
            (float)queue_peaks_[i].peak_usage / queue_peaks_[i].capacity * 100.0f : 0.0f;
        count++;
    }

    *actual_count = count;
}

SystemMonitor::AudioLatencyStats SystemMonitor::get_audio_latency() {
    return audio_latency_;
}

SystemMonitor::NetworkStats SystemMonitor::get_network_stats() {
    return network_stats_;
}

SystemMonitor::HealthStatus SystemMonitor::get_health_status() {
    // 综合评估系统健康状态
    HealthStatus status = HealthStatus::EXCELLENT;

    MemoryStats mem = get_memory_stats();

    // 检查内存
    if (mem.internal_usage > 90.0f) {
        status = HealthStatus::CRITICAL;
    } else if (mem.internal_usage > 80.0f) {
        status = HealthStatus::WARNING;
    } else if (mem.internal_usage > 70.0f) {
        status = HealthStatus::GOOD;
    }

    // 检查最大可分配块
    if (mem.largest_free_block < 8192) {
        if (status < HealthStatus::WARNING) {
            status = HealthStatus::WARNING;
        }
    }

    // 检查网络
    if (!network_stats_.wifi_connected) {
        if (status < HealthStatus::WARNING) {
            status = HealthStatus::WARNING;
        }
    }

    return status;
}

void SystemMonitor::print_system_report() {
    ESP_LOGI(TAG, "=== SYSTEM HEALTH REPORT ===");

    // 内存报告
    MemoryStats mem = get_memory_stats();
    ESP_LOGI(TAG, "Memory:");
    ESP_LOGI(TAG, "  Internal: %lu / %lu bytes (%.1f%% used), largest block: %lu",
             mem.internal_total - mem.internal_free, mem.internal_total,
             mem.internal_usage, mem.largest_free_block);
    ESP_LOGI(TAG, "  PSRAM:    %lu / %lu bytes (%.1f%% used)",
             mem.psram_total - mem.psram_free, mem.psram_total,
             mem.psram_usage);

    // CPU报告
    CpuStats cpu = get_cpu_stats();
    ESP_LOGI(TAG, "CPU:");
    ESP_LOGI(TAG, "  Core 0: %.1f%%", cpu.core0_usage);
    ESP_LOGI(TAG, "  Core 1: %.1f%%", cpu.core1_usage);

    // 队列报告
    QueueStats queues[16];
    size_t queue_count;
    get_queue_stats(queues, 16, &queue_count);

    if (queue_count > 0) {
        ESP_LOGI(TAG, "Queues:");
        for (size_t i = 0; i < queue_count; i++) {
            ESP_LOGI(TAG, "  %s: peak %lu/%lu (%.1f%%)",
                     queues[i].name, queues[i].peak, queues[i].size,
                     queues[i].usage);
        }
    }

    // 健康状态
    HealthStatus health = get_health_status();
    const char* health_str[] = {"EXCELLENT", "GOOD", "WARNING", "CRITICAL"};
    ESP_LOGI(TAG, "Overall Health: %s", health_str[(int)health]);

    ESP_LOGI(TAG, "=========================");
}

void SystemMonitor::record_queue_usage(const char* name, uint32_t current, uint32_t max) {
    // 查找或创建记录
    for (size_t i = 0; i < queue_peak_count_; i++) {
        if (strcmp(queue_peaks_[i].name, name) == 0) {
            if (current > queue_peaks_[i].peak_usage) {
                queue_peaks_[i].peak_usage = current;
            }
            queue_peaks_[i].capacity = max;
            return;
        }
    }

    // 添加新记录
    if (queue_peak_count_ < 16) {
        strncpy(queue_peaks_[queue_peak_count_].name, name, 31);
        queue_peaks_[queue_peak_count_].name[31] = '\0';
        queue_peaks_[queue_peak_count_].peak_usage = current;
        queue_peaks_[queue_peak_count_].capacity = max;
        queue_peak_count_++;
    }
}

void SystemMonitor::record_audio_timestamp(const char* stage) {
    // TODO: 实现音频时间戳记录用于延迟计算
    // 可以使用环形缓冲区记录每个阶段的时间戳
}
