#pragma once

#include <cstdint>
#include <functional>

/**
 * @brief 系统诊断工具
 *
 * 提供各种诊断和测试功能：
 * - 音频流水线测试
 * - 网络连接测试
 * - 内存泄漏检测
 * - 性能基准测试
 */
class Diagnostics {
public:
    // 测试结果
    struct TestResult {
        bool passed;
        const char* name;
        const char* details;
        uint32_t duration_ms;
    };

    // 音频测试结果
    struct AudioTestResult {
        bool i2s_working;
        bool afe_working;
        bool encoder_working;
        bool decoder_working;
        uint32_t input_frames;
        uint32_t output_frames;
        uint32_t dropped_frames;
        float audio_quality;
    };

    // 网络测试结果
    struct NetworkTestResult {
        bool wifi_working;
        bool ws_working;
        int8_t wifi_rssi;
        uint32_t ping_ms;
        uint32_t throughput_kbps;
    };

    static Diagnostics& instance() {
        static Diagnostics inst;
        return inst;
    }

    /**
     * @brief 运行完整的系统诊断
     *
     * @return 所有测试是否通过
     */
    bool run_full_diagnostics();

    /**
     * @brief 测试I2S音频输入输出
     */
    AudioTestResult test_audio_pipeline();

    /**
     * @brief 测试网络连接
     */
    NetworkTestResult test_network();

    /**
     * @brief 测试内存分配和释放
     *
     * @param iterations 测试迭代次数
     * @return 是否检测到内存泄漏
     */
    bool test_memory_leaks(uint32_t iterations = 1000);

    /**
     * @brief 音频延迟基准测试
     *
     * @return 平均延迟(ms)
     */
    uint32_t benchmark_audio_latency();

    /**
     * @brief Opus编解码性能测试
     *
     * @return 每秒可处理的帧数
     */
    uint32_t benchmark_opus_codec();

    /**
     * @brief WebSocket吞吐量测试
     *
     * @return 吞吐量(kbps)
     */
    uint32_t benchmark_websocket_throughput();

    /**
     * @brief 生成测试音频信号
     *
     * @param frequency 频率(Hz)
     * @param duration_ms 持续时间(ms)
     */
    void generate_test_tone(uint32_t frequency, uint32_t duration_ms);

    /**
     * @brief 打印诊断报告
     */
    void print_diagnostics_report();

private:
    Diagnostics() = default;

    TestResult results_[32];
    uint32_t result_count_ = 0;

    void add_result(const TestResult& result);
};

/**
 * @brief 性能分析器 - 测量代码段执行时间
 */
class PerformanceProfiler {
public:
    PerformanceProfiler(const char* name);
    ~PerformanceProfiler();

    static void print_all_profiles();

private:
    const char* name_;
    uint32_t start_time_;

    struct Profile {
        char name[64];
        uint32_t total_time_ms;
        uint32_t call_count;
        uint32_t min_time_ms;
        uint32_t max_time_ms;
    };

    static Profile profiles_[32];
    static uint32_t profile_count_;

    static void update_profile(const char* name, uint32_t time_ms);
};

// 便捷宏：自动测量函数执行时间
#define PROFILE_FUNCTION() PerformanceProfiler __profiler__(__FUNCTION__)
#define PROFILE_SCOPE(name) PerformanceProfiler __profiler__##__LINE__(name)
