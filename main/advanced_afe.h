#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>
#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#include <functional>

/**
 * @brief 先进的AFE音频前端处理器
 *
 * 功能：
 * - 回声消除 (AEC)
 * - 噪声抑制 (NS)
 * - 自动增益控制 (AGC)
 * - 波束形成 (Beamforming) - 双麦克风
 * - 语音活动检测 (VAD)
 * - 唤醒词检测
 */
class AdvancedAFE {
public:
    // 回调函数类型
    using WakeCallback = std::function<void(const char* wake_word)>;
    using VadCallback = std::function<void(bool voice_active)>;

    // AFE配置
    struct Config {
        int sample_rate = 16000;     // 采样率 (Hz)
        int channels = 2;            // 麦克风数量
        int frame_size = 512;        // 帧大小 (samples)

        // AFE功能开关
        bool enable_aec = false;     // 回声消除（需要参考音频）
        bool enable_ns = true;       // 噪声抑制
        bool enable_agc = true;      // 自动增益
        bool enable_vad = true;      // VAD检测
        bool enable_wakenet = true;  // 唤醒词检测

        // AFE参数
        int agc_level = 3;           // AGC等级 (0-31)
        int ns_level = 2;            // 降噪等级 (0-3)

        // 唤醒词参数
        int wake_threshold = 0;      // 唤醒阈值（0=auto）
        const char** wake_words = nullptr;  // 唤醒词列表
        int wake_word_count = 0;
    };

    AdvancedAFE();
    ~AdvancedAFE();

    /**
     * @brief 初始化AFE
     * @param config AFE配置
     * @return true 成功，false 失败
     */
    bool init(const Config& config);

    /**
     * @brief 反初始化AFE
     */
    void deinit();

    /**
     * @brief 设置唤醒词回调
     */
    void on_wake_detected(WakeCallback cb) { wake_cb_ = cb; }

    /**
     * @brief 设置VAD回调
     */
    void on_vad_changed(VadCallback cb) { vad_cb_ = cb; }

    /**
     * @brief 启动AFE处理任务
     */
    bool start();

    /**
     * @brief 停止AFE处理任务
     */
    void stop();

    /**
     * @brief 喂入原始音频数据（双麦克风交织）
     * @param data 音频数据 [L0, R0, L1, R1, ...]
     * @param samples 样本数（单通道）
     */
    void feed(const int16_t* data, size_t samples);

    /**
     * @brief 获取AFE处理后的音频数据
     * @param out 输出缓冲区
     * @param max_samples 最大样本数
     * @return 实际获取的样本数
     */
    int fetch(int16_t* out, size_t max_samples);

    /**
     * @brief 获取唤醒词检测状态
     */
    bool is_wake_detected() const { return wake_detected_; }

    /**
     * @brief 重置唤醒词状态
     */
    void reset_wake() { wake_detected_ = false; }

    /**
     * @brief 获取VAD状态
     */
    bool is_voice_active() const { return vad_active_; }

    /**
     * @brief 获取音频能量（用于UI动画）
     */
    int get_audio_energy() const { return audio_energy_; }

    /**
     * @brief 动态启用/禁用AEC（运行时切换）
     * 播放TTS时启用AEC消除扬声器回声，空闲时禁用以保证WakeNet正常检测
     */
    void enable_aec(bool enable);

private:
    // AFE处理任务
    static void afe_task(void* arg);
    void process_loop();

    // 唤醒词检测
    void detect_wake_word(const int16_t* data, int samples);

    // VAD检测
    void detect_vad(int vad_state);

    Config config_;

    // AFE接口和数据
    const esp_afe_sr_iface_t* afe_handle_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;

    // 唤醒词接口
    const esp_wn_iface_t* wakenet_ = nullptr;
    model_iface_data_t* wake_model_ = nullptr;

    // 队列
    QueueHandle_t input_queue_ = nullptr;   // 输入队列
    QueueHandle_t output_queue_ = nullptr;  // 输出队列

    // 任务句柄
    TaskHandle_t task_handle_ = nullptr;

    // 回调
    WakeCallback wake_cb_;
    VadCallback vad_cb_;

    // 状态
    volatile bool wake_detected_ = false;
    volatile bool vad_active_ = false;
    volatile int audio_energy_ = 0;

    // 通道
    int total_channels_ = 0;  // 总通道数 (mic + ref)

    // 缓冲区
    int16_t* temp_buffer_ = nullptr;
    size_t temp_buffer_size_ = 0;
};
