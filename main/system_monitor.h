#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdint>

/**
 * @brief 系统监控器 - 实时性能和健康状态监控
 *
 * 功能：
 * - CPU使用率监控（双核）
 * - 内存使用监控（内部RAM + PSRAM）
 * - 任务栈水位监控
 * - 队列使用率监控
 * - 音频流水线延迟监控
 * - 网络健康状态
 */
class SystemMonitor {
public:
    // 系统健康状态
    enum class HealthStatus {
        EXCELLENT = 0,  // 所有指标正常
        GOOD = 1,       // 轻微警告
        WARNING = 2,    // 需要注意
        CRITICAL = 3    // 严重问题
    };

    // CPU统计
    struct CpuStats {
        float core0_usage;      // Core 0 使用率 (0-100%)
        float core1_usage;      // Core 1 使用率 (0-100%)
        uint32_t idle_time_core0;
        uint32_t idle_time_core1;
    };

    // 内存统计
    struct MemoryStats {
        uint32_t internal_free;     // 内部RAM空闲 (bytes)
        uint32_t internal_total;    // 内部RAM总量
        uint32_t psram_free;        // PSRAM空闲
        uint32_t psram_total;       // PSRAM总量
        uint32_t largest_free_block;
        float internal_usage;       // 内部RAM使用率 (0-100%)
        float psram_usage;          // PSRAM使用率 (0-100%)
    };

    // 队列统计
    struct QueueStats {
        const char* name;
        uint32_t size;              // 队列容量
        uint32_t used;              // 当前使用
        uint32_t peak;              // 历史峰值
        float usage;                // 使用率 (0-100%)
    };

    // 音频延迟统计
    struct AudioLatencyStats {
        uint32_t capture_to_afe_ms;     // 采集到AFE延迟
        uint32_t afe_to_encode_ms;      // AFE到编码延迟
        uint32_t encode_to_ws_ms;       // 编码到WebSocket延迟
        uint32_t ws_to_decode_ms;       // WebSocket到解码延迟
        uint32_t decode_to_play_ms;     // 解码到播放延迟
        uint32_t total_latency_ms;      // 总延迟
    };

    // 网络统计
    struct NetworkStats {
        bool wifi_connected;
        bool ws_connected;
        int8_t wifi_rssi;           // WiFi信号强度
        uint32_t ws_tx_packets;
        uint32_t ws_rx_packets;
        uint32_t ws_tx_bytes;
        uint32_t ws_rx_bytes;
        uint32_t ws_errors;
    };

    static SystemMonitor& instance() {
        static SystemMonitor inst;
        return inst;
    }

    /**
     * @brief 初始化监控系统
     */
    bool init();

    /**
     * @brief 启动监控任务
     */
    bool start();

    /**
     * @brief 停止监控任务
     */
    void stop();

    /**
     * @brief 获取CPU统计
     */
    CpuStats get_cpu_stats();

    /**
     * @brief 获取内存统计
     */
    MemoryStats get_memory_stats();

    /**
     * @brief 获取所有队列统计
     */
    void get_queue_stats(QueueStats* stats, size_t max_count, size_t* actual_count);

    /**
     * @brief 获取音频延迟统计
     */
    AudioLatencyStats get_audio_latency();

    /**
     * @brief 获取网络统计
     */
    NetworkStats get_network_stats();

    /**
     * @brief 获取整体健康状态
     */
    HealthStatus get_health_status();

    /**
     * @brief 打印完整的系统报告
     */
    void print_system_report();

    /**
     * @brief 记录队列使用峰值
     */
    void record_queue_usage(const char* name, uint32_t current, uint32_t max);

    /**
     * @brief 记录音频时间戳（用于延迟计算）
     */
    void record_audio_timestamp(const char* stage);

private:
    SystemMonitor() = default;

    TaskHandle_t monitor_task_handle_ = nullptr;
    bool running_ = false;

    // 内部统计
    CpuStats cpu_stats_ = {};
    MemoryStats memory_stats_ = {};
    AudioLatencyStats audio_latency_ = {};
    NetworkStats network_stats_ = {};

    // 队列峰值记录
    struct QueuePeakRecord {
        char name[32];
        uint32_t peak_usage;
        uint32_t capacity;
    };
    QueuePeakRecord queue_peaks_[16] = {};
    size_t queue_peak_count_ = 0;

    static void monitor_task(void* arg);
    void update_stats();
};
