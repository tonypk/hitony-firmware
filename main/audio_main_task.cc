#include "task_manager.h"
#include "audio_i2s.h"
#include "advanced_afe.h"
#include "opus_encoder.h"
#include "opus_decoder.h"
#include "lvgl_ui.h"
#include "config.h"
#include <esp_log.h>
#include <string.h>
#include <cmath>

static const char* TAG = "audio_main";

// 音频模式
typedef enum {
    AUDIO_MODE_IDLE,       // 等待唤醒词
    AUDIO_MODE_RECORDING,  // 录音并编码
    AUDIO_MODE_THINKING,   // 等待服务器响应（禁止VAD触发，继续AFE处理）
    AUDIO_MODE_PLAYING,    // 播放TTS
} audio_mode_t;

// Opus编码统计（用于屏幕显示）
static uint32_t g_opus_encode_count = 0;

/**
 * @brief Audio Main Task - 整合所有音频处理
 *
 * 整合功能：
 * - I2S音频输入/输出
 * - AFE处理（降噪、波束形成、VAD）
 * - 唤醒词检测
 * - Opus编码/解码
 * - 音频混音
 */
void audio_main_task(void* arg) {
    ESP_LOGI(TAG, "Audio Main Task started on Core %d", xPortGetCoreID());

    // === 初始化所有音频组件 ===

    // 1. 音频I2S接口（已在main.cc Phase 1.5初始化）
    AudioI2S& audio_i2s = AudioI2S::instance();
    // NOTE: AudioI2S和触摸传感器已在main.cc中提前初始化
    // 这里直接使用即可

    // 2. AFE音频前端处理器（使用xiaozhi的afe_config_init()方法）
    //   ✅ 修复成功：使用afe_config_init()替代手动配置
    AdvancedAFE afe;
    AdvancedAFE::Config afe_cfg = {
        .sample_rate = HITONY_SAMPLE_RATE,
        .channels = 2,           // 双麦克风
        .frame_size = 256,       // 每通道256 samples（匹配feed调用的256）
        .enable_aec = false,     // AEC禁用："MMR"格式导致AFE零输出（ESP-SR bug/限制），改用"MM"格式+WakeNet自身抗噪能力
        .enable_ns = true,       // 启用降噪
        .enable_agc = false,     // 禁用AGC（避免与WakeNet冲突）
        .enable_vad = true,      // 启用VAD
        .enable_wakenet = true,  // ✅ 启用WakeNet（使用xiaozhi方法）
        .agc_level = 3,
        .ns_level = 2,
        .wake_threshold = 0,
    };

    if (!afe.init(afe_cfg)) {
        ESP_LOGE(TAG, "Failed to initialize AFE");
        vTaskDelete(NULL);
        return;
    }

    // 设置唤醒词回调（xiaozhi方式）
    afe.on_wake_detected([](const char* wake_word) {
        ESP_LOGI(TAG, "🎤🎤🎤 Wake word detected: %s", wake_word);
        xEventGroupSetBits(g_audio_event_bits, AUDIO_EVENT_WAKE_DETECTED);
    });

    // 启动AFE处理任务
    if (!afe.start()) {
        ESP_LOGE(TAG, "Failed to start AFE task");
        afe.deinit();
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "AFE processing task started with WakeNet (AEC: %s)", afe_cfg.enable_aec ? "ON" : "OFF");

    // 3. Opus编码器/解码器
    OpusEncoder opus_enc;
    // 激进优化：32kbps → 48kbps，最大化快速说话识别准确度
    // 48kbps接近Opus在16kHz单声道的最优码率（理论上限约64kbps）
    if (!opus_enc.init(16000, 1, 48000)) {  // 16kHz, mono, 48kbps
        ESP_LOGE(TAG, "Failed to initialize Opus encoder");
        vTaskDelete(NULL);
        return;
    }

    OpusDecoder opus_dec;
    if (!opus_dec.init(16000, 1)) {  // 16kHz, mono
        ESP_LOGE(TAG, "Failed to initialize Opus decoder");
        opus_enc.deinit();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "All audio components initialized successfully");

    // === 本地状态变量 ===
    audio_mode_t mode = AUDIO_MODE_IDLE;
    alignas(16) int16_t i2s_buffer[512];           // I2S读取缓冲区（512 samples = ~32ms @ 16kHz）
    alignas(16) int16_t afe_accumulator[960];      // AFE输出累积器（Opus需要960 samples）
    size_t afe_accum_count = 0;

    uint32_t frame_count = 0;
    uint32_t i2s_read_count = 0;  // I2S成功读取次数
    uint32_t i2s_samples_total = 0;  // 总采样数
    uint32_t last_stats_time = 0;
    uint32_t silence_start_time = 0;  // 静音计时器（跨代码块共享）
    uint32_t recording_start_time = 0;  // 录音开始时间
    uint32_t vad_trigger_count = 0;    // VAD连续触发次数
    uint32_t last_vad_trigger_time = 0; // 上次VAD触发时间（冷却计时）
    uint32_t thinking_start_time = 0;   // THINKING模式开始时间
    uint32_t tts_play_count = 0;        // TTS播放帧计数（每次PLAYING重置）
    uint32_t tts_underrun_count = 0;    // TTS underrun计数（每次PLAYING重置）
    const uint32_t MAX_RECORDING_MS = 10000;  // 最大录音时间10秒
    const uint32_t THINKING_TIMEOUT_MS = 15000; // THINKING超时15秒

    ESP_LOGI(TAG, "Entering main audio processing loop...");

    // === 主循环 ===
    while (1) {
        frame_count++;

        // 每10000帧打印一次当前模式
        if (frame_count % 10000 == 0) {
            ESP_LOGI(TAG, "Main loop: frame=%lu, mode=%d (0=IDLE,1=REC,2=THINK,3=PLAY)",
                     frame_count, mode);
        }

        // === 播放模式：解码+播放，同时保持WakeNet活跃 ===
        // 全双工设计：播放1帧TTS(~60ms DMA阻塞)后，I2S RX DMA已收集输入数据，
        // read_frame近乎即时返回。继续feed AFE保持WakeNet检测，实现TTS打断。
        // Opus编码在section 4中已有mode==RECORDING守卫，PLAYING时不会编码。
        if (mode == AUDIO_MODE_PLAYING) {
            // 检查控制命令
            audio_cmd_t play_cmd;
            if (xQueueReceive(g_audio_cmd_queue, &play_cmd, 0) == pdTRUE) {
                if (play_cmd == AUDIO_CMD_STOP_PLAYBACK) {
                    ESP_LOGI(TAG, "Stop playback mode, resetting for next wake");
                    mode = AUDIO_MODE_IDLE;
                    last_vad_trigger_time = xTaskGetTickCount();
                    vad_trigger_count = 0;
                    opus_dec.reset();
                    ringbuffer_reset(&g_pcm_ringbuffer);  // 清除echo污染数据
                    ringbuffer_reset(&g_ref_ringbuffer);   // 清除残留参考数据
                    if (afe_cfg.enable_aec) afe.enable_aec(false);  // 停止播放→禁用AEC
                    afe_accum_count = 0;  // 重置AFE累积器
                    // 打印内存状态（检测泄漏）
                    ESP_LOGI(TAG, "Post-playback: heap=%lu, PSRAM=%lu",
                             esp_get_free_heap_size(),
                             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                    pool_print_stats();
                } else if (play_cmd == AUDIO_CMD_START_RECORDING) {
                    ESP_LOGI(TAG, "Start recording (interrupting playback)");
                    mode = AUDIO_MODE_RECORDING;
                    afe_accum_count = 0;
                    silence_start_time = 0;
                    recording_start_time = xTaskGetTickCount();
                    opus_dec.reset();
                    ringbuffer_reset(&g_pcm_ringbuffer);
                    ringbuffer_reset(&g_ref_ringbuffer);  // 清除残留参考数据
                    if (afe_cfg.enable_aec) afe.enable_aec(false);  // 录音时禁用AEC
                }
            }

            // 解码+播放1帧（~60ms DMA阻塞，期间RX DMA自动收集麦克风数据）
            opus_packet_msg_t* rx_msg = nullptr;
            if (mode == AUDIO_MODE_PLAYING &&
                xQueueReceive(g_opus_playback_queue, &rx_msg, pdMS_TO_TICKS(20)) == pdTRUE) {
                tts_play_count++;
                tts_underrun_count = 0;  // 收到数据，重置连续underrun

                if (rx_msg->len >= 3) {
                    int16_t pcm_decoded[960];
                    int decoded_samples = opus_dec.decode(rx_msg->data, rx_msg->len,
                                                          pcm_decoded, 960);
                    if (decoded_samples > 0) {
                        if (tts_play_count <= 3 || tts_play_count % 50 == 0) {
                            UBaseType_t q_depth = uxQueueMessagesWaiting(g_opus_playback_queue);
                            ESP_LOGI(TAG, "TTS play #%lu: %d samples, queue=%u/24",
                                     tts_play_count, decoded_samples, (unsigned)q_depth);
                        }

                        // 音乐节奏动画：计算音频能量并更新UI
                        // 降低更新频率以减少CPU负载：每3帧（180ms）更新一次，而非每60ms
                        static uint32_t energy_update_counter = 0;
                        energy_update_counter++;

                        if (energy_update_counter % 3 == 0) {
                            // 计算RMS能量
                            int64_t sum_squares = 0;
                            for (int i = 0; i < decoded_samples; i++) {
                                int32_t sample = pcm_decoded[i];
                                sum_squares += (int64_t)sample * sample;
                            }
                            float rms = sqrtf((float)sum_squares / decoded_samples);
                            float energy = rms / 32768.0f;  // 归一化到0.0-1.0

                            // UI会根据当前状态决定是否显示耳机图标
                            lvgl_ui_set_music_energy(energy);
                        }

                        audio_i2s.play_frame((uint8_t*)pcm_decoded, decoded_samples * sizeof(int16_t));
                        // 喂入参考 RingBuffer 供 AEC 回声消除使用
                        ringbuffer_write(&g_ref_ringbuffer, pcm_decoded, decoded_samples);
                    }
                }
                free_opus_msg(rx_msg);
            } else if (mode == AUDIO_MODE_PLAYING) {
                tts_underrun_count++;
                // Log sparingly: first 3 + every 200
                if (tts_underrun_count <= 3 || tts_underrun_count % 200 == 0) {
                    ESP_LOGW(TAG, "TTS underrun #%lu (played %lu)", tts_underrun_count, tts_play_count);
                }
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            // 不再continue — 落入下面的I2S读取和AFE处理，保持WakeNet活跃
        }

        // === 1. I2S 音频输入（IDLE, RECORDING, PLAYING模式）===
        int n = audio_i2s.read_frame((uint8_t*)i2s_buffer, sizeof(i2s_buffer));

        // 首次I2S读取时打印调试信息
        static bool first_i2s_read = true;
        if (first_i2s_read && n > 0) {
            ESP_LOGI(TAG, "🔊 首次I2S读取成功: n=%d bytes", n);
            first_i2s_read = false;
        }

        if (n > 0) {
            int samples = n / sizeof(int16_t);
            i2s_read_count++;
            i2s_samples_total += samples;

            // === 计算原始I2S音频的RMS音量（诊断麦克风）===
            static uint32_t volume_check_count = 0;
            static uint32_t last_volume_print = 0;
            volume_check_count++;

            // 每30秒打印一次原始音量（减少日志噪声）
            // PLAYING模式下跳过音量日志（会拾取TTS回声，不具参考价值）
            if (mode != AUDIO_MODE_PLAYING && volume_check_count - last_volume_print >= 930) {
                // 计算 RMS 音量
                int64_t sum_squares = 0;
                for (int i = 0; i < samples; i++) {
                    int32_t sample = i2s_buffer[i];
                    sum_squares += (int64_t)sample * sample;
                }
                float rms = sqrtf((float)sum_squares / samples);
                float volume_percent = (rms / 32768.0f) * 100.0f;  // 归一化到 0-100%

                ESP_LOGI(TAG, "🎤 原始I2S音量: RMS=%.1f (%.2f%%), samples=%d",
                         rms, volume_percent, samples);
                last_volume_print = volume_check_count;
            }

            // 写入RingBuffer（零拷贝）- 原始双声道数据
            size_t written = ringbuffer_write(&g_pcm_ringbuffer, i2s_buffer, samples);

            // 前3次写入打印详细信息
            static int write_count = 0;
            if (write_count < 3) {
                size_t rb_available = ringbuffer_data_available(&g_pcm_ringbuffer);
                ESP_LOGI(TAG, "RingBuffer write #%d: samples=%d, written=%zu, avail=%zu",
                         write_count, samples, written, rb_available);
                write_count++;
            }

            if (written < samples) {
                ESP_LOGW(TAG, "RingBuffer full, dropped %zu samples", samples - written);
            }
        } else if (n < 0) {
            static int error_count = 0;
            if (error_count < 5) {
                ESP_LOGW(TAG, "⚠️ I2S读取失败: n=%d", n);
                error_count++;
            }
        }

        // === 2. 检查Main Task的控制命令（非阻塞）===
        audio_cmd_t cmd;
        if (xQueueReceive(g_audio_cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd) {
                case AUDIO_CMD_START_RECORDING:
                    ESP_LOGI(TAG, "Start recording mode");
                    mode = AUDIO_MODE_RECORDING;
                    afe_accum_count = 0;
                    silence_start_time = 0;
                    recording_start_time = xTaskGetTickCount();
                    ringbuffer_reset(&g_pcm_ringbuffer);
                    break;

                case AUDIO_CMD_STOP_RECORDING:
                    ESP_LOGI(TAG, "Stop recording, entering THINKING mode (waiting for server)");
                    mode = AUDIO_MODE_THINKING;
                    thinking_start_time = xTaskGetTickCount();
                    afe_accum_count = 0;
                    vad_trigger_count = 0;

                    // 更新屏幕显示
                    lvgl_ui_update_recording_stats(g_opus_encode_count, false);
                    lvgl_ui_set_status("Thinking...");
                    break;

                case AUDIO_CMD_START_PLAYBACK:
                    ESP_LOGI(TAG, "Start playback mode");
                    mode = AUDIO_MODE_PLAYING;
                    tts_play_count = 0;
                    tts_underrun_count = 0;
                    if (afe_cfg.enable_aec) afe.enable_aec(true);  // 播放时启用AEC
                    break;

                case AUDIO_CMD_STOP_PLAYBACK:
                    ESP_LOGI(TAG, "Stop playback mode");
                    mode = AUDIO_MODE_IDLE;
                    // 设置冷却时间防止立即触发VAD录音
                    last_vad_trigger_time = xTaskGetTickCount();
                    vad_trigger_count = 0;
                    ringbuffer_reset(&g_ref_ringbuffer);  // 清除残留参考数据
                    if (afe_cfg.enable_aec) afe.enable_aec(false);  // 停止播放→禁用AEC
                    break;
            }
        }

        // === 3. AFE处理（如果RingBuffer有足够数据）===
        size_t available = ringbuffer_data_available(&g_pcm_ringbuffer);

        // 前2次检查打印RingBuffer状态
        static int check_count = 0;
        if (check_count < 2) {
            ESP_LOGI(TAG, "RingBuffer check #%d: available=%zu (need>=512)", check_count, available);
            check_count++;
        }

        if (available >= 512) {  // AFE处理块大小（512个int16 = 256 samples × 2 mic channels）
            // 首次进入AFE处理时打印
            static bool first_afe_process = true;
            if (first_afe_process) {
                ESP_LOGI(TAG, "✅ 首次进入AFE处理块！available=%zu, aec=%d", available, afe_cfg.enable_aec);
                first_afe_process = false;
            }

            if (afe_cfg.enable_aec) {
                // AEC模式：3通道交织 [M0, M1, Ref]
                int16_t mic_input[512];    // 2ch mic 数据
                ringbuffer_read(&g_pcm_ringbuffer, mic_input, 512);

                // 获取参考音频（播放中=实际PCM，空闲=零）
                int16_t ref_input[256];
                size_t ref_avail = ringbuffer_data_available(&g_ref_ringbuffer);
                if (ref_avail >= 256) {
                    ringbuffer_read(&g_ref_ringbuffer, ref_input, 256);
                } else {
                    memset(ref_input, 0, sizeof(ref_input));
                }

                // 交织为 [M0, M1, Ref, M0, M1, Ref, ...] (3ch)
                int16_t afe_input[768];
                for (int i = 0; i < 256; i++) {
                    afe_input[i * 3]     = mic_input[i * 2];      // Mic 0
                    afe_input[i * 3 + 1] = mic_input[i * 2 + 1];  // Mic 1
                    afe_input[i * 3 + 2] = ref_input[i];           // Reference
                }

                afe.feed(afe_input, 256);
            } else {
                // 无AEC模式：2通道直接喂入
                int16_t afe_input[512];
                ringbuffer_read(&g_pcm_ringbuffer, afe_input, 512);
                afe.feed(afe_input, 256);
            }
        }

        // === 4. 从AFE获取输出（持续消费直到队列为空，避免溢出）===
        // AFE有内部任务异步处理，我们需要持续fetch消费输出队列
        // ⚠️ 重要：必须在每次循环中清空所有待处理数据，否则会导致 "Ringbuffer of AFE(FEED) is full"
        static uint32_t afe_fetch_count = 0;
        // vad_trigger_count and last_vad_trigger_time moved to function scope

        int total_fetched = 0;
        int fetch_iterations = 0;
        const int MAX_FETCH_ITERATIONS = 10;  // 防止无限循环

        while (fetch_iterations < MAX_FETCH_ITERATIONS) {
            int16_t afe_output[256];
            int afe_samples = afe.fetch(afe_output, 256);

            if (afe_samples <= 0) {
                break;  // 队列已空，退出循环
            }

            fetch_iterations++;
            total_fetched += afe_samples;
            afe_fetch_count++;

            // 定期打印AFE fetch结果
            if (afe_fetch_count % 2000 == 0) {
                ESP_LOGI(TAG, "AFE fetch #%lu: samples=%d, iterations=%d, total=%d",
                         afe_fetch_count, afe_samples, fetch_iterations, total_fetched);
            }

        // === 4.1. 录音超时检查 ===
            if (mode == AUDIO_MODE_RECORDING && recording_start_time > 0) {
                uint32_t now = xTaskGetTickCount();
                if (now - recording_start_time > pdMS_TO_TICKS(MAX_RECORDING_MS)) {
                    ESP_LOGI(TAG, "Max recording time reached (%lums), entering THINKING", MAX_RECORDING_MS);
                    mode = AUDIO_MODE_THINKING;
                    thinking_start_time = xTaskGetTickCount();
                    afe_accum_count = 0;
                    recording_start_time = 0;
                    silence_start_time = 0;
                    vad_trigger_count = 0;

                    lvgl_ui_update_recording_stats(g_opus_encode_count, false);
                    lvgl_ui_set_status("Recording done");
                    xEventGroupSetBits(g_audio_event_bits, AUDIO_EVENT_VAD_END);
                }
            }

        // === 4.2. VAD静音检测（仅RECORDING模式）===
            // WakeNet负责唤醒词检测（通过AFE回调），VAD仅用于录音结束判断
                if (!afe.is_voice_active()) {
                    // RECORDING模式下：静音2秒后自动停止录音
                    if (mode == AUDIO_MODE_RECORDING) {
                        uint32_t now = xTaskGetTickCount();

                        if (silence_start_time == 0) {
                            silence_start_time = now;
                        } else if (now - silence_start_time > pdMS_TO_TICKS(2000)) {
                            // 短录音优化：<500ms录音可能是auto-listen后无人说话，直接回IDLE
                            uint32_t recording_duration_ms = (now - recording_start_time) * portTICK_PERIOD_MS;
                            if (recording_duration_ms < 500) {
                                ESP_LOGI(TAG, "Short recording (%lums < 500ms), skipping server, back to IDLE",
                                         recording_duration_ms);
                                mode = AUDIO_MODE_IDLE;
                                last_vad_trigger_time = now;
                                vad_trigger_count = 0;
                                afe_accum_count = 0;
                                silence_start_time = 0;
                                lvgl_ui_set_status("Say 'Hi Tony'");
                                // 通知main task但用专门的bit表示"短录音取消"
                                xEventGroupSetBits(g_audio_event_bits, AUDIO_EVENT_VAD_END);
                            } else {
                                ESP_LOGI(TAG, "静音2秒，entering THINKING mode (recorded %lums)", recording_duration_ms);
                                mode = AUDIO_MODE_THINKING;
                                thinking_start_time = xTaskGetTickCount();
                                afe_accum_count = 0;
                                silence_start_time = 0;

                                // 更新屏幕显示为待机状态
                                lvgl_ui_update_recording_stats(g_opus_encode_count, false);
                                lvgl_ui_set_status("Recording done");

                                // 通知Main Control Task
                                xEventGroupSetBits(g_audio_event_bits, AUDIO_EVENT_VAD_END);
                            }
                        }
                    }
                } else if (mode == AUDIO_MODE_RECORDING) {
                    // VAD活动时重置静音计时器
                    silence_start_time = 0;
                }

                // === 4.2. Opus编码（仅在RECORDING模式）===
                // 注：WakeNet检测由AFE内部任务完成，通过回调触发录音
                if (mode == AUDIO_MODE_RECORDING) {
                    // 首次进入RECORDING模式时打印
                    static bool first_recording = true;
                    if (first_recording) {
                        ESP_LOGI(TAG, "📼 进入RECORDING模式！开始累积AFE样本进行Opus编码");
                        first_recording = false;
                    }

                    // 累积AFE输出到960 samples（Opus帧大小）
                    size_t to_copy = (afe_samples < (960 - afe_accum_count)) ?
                                     afe_samples : (960 - afe_accum_count);

                    memcpy(&afe_accumulator[afe_accum_count], afe_output, to_copy * sizeof(int16_t));
                    afe_accum_count += to_copy;

                    // 定期打印累积进度（每次累积时）
                    static uint32_t accum_log_count = 0;
                    accum_log_count++;
                    if (accum_log_count % 10 == 0) {
                        ESP_LOGD(TAG, "📊 AFE累积进度: %zu/960 samples (%d%%)",
                                 afe_accum_count, (afe_accum_count * 100) / 960);
                    }

                    // 当累积满960 samples时，进行Opus编码
                    if (afe_accum_count >= 960) {
                        g_opus_encode_count++;

                        alignas(16) uint8_t opus_packet[512];  // 增大buffer：60ms帧需要>280字节
                        int opus_len = opus_enc.encode(afe_accumulator, 960,
                                                       opus_packet, sizeof(opus_packet));

                        if (opus_len > 0) {
                            // Log first 3 + every 50th encode
                            if (g_opus_encode_count <= 3 || g_opus_encode_count % 50 == 0) {
                                ESP_LOGI(TAG, "Opus #%lu: %d bytes", g_opus_encode_count, opus_len);
                            }

                            // 更新屏幕显示（每5个包更新一次，避免过于频繁）
                            if (g_opus_encode_count % 5 == 0) {
                                lvgl_ui_update_recording_stats(g_opus_encode_count, true);
                            }

                            // 分配Opus消息并发送到Main Task
                            opus_packet_msg_t* msg = alloc_opus_msg(opus_len);
                            if (msg) {
                                memcpy(msg->data, opus_packet, opus_len);

                                if (xQueueSend(g_opus_tx_queue, &msg, 0) != pdTRUE) {
                                    ESP_LOGW(TAG, "Opus TX queue full, dropping packet");
                                    free_opus_msg(msg);
                                } else {
                                    xEventGroupSetBits(g_audio_event_bits, AUDIO_EVENT_ENCODE_READY);
                                }
                            } else {
                                ESP_LOGW(TAG, "alloc_opus_msg failed!");
                            }
                        } else {
                            ESP_LOGW(TAG, "❌ Opus编码失败: %d", opus_len);
                        }

                        // 重置累积器
                        afe_accum_count = 0;
                    }
                }
        }  // end while (fetch_iterations < MAX_FETCH_ITERATIONS)

        // === 5. (播放逻辑已移至循环顶部的PLAYING模式专用段) ===

        // === 6. THINKING模式超时 ===
        if (mode == AUDIO_MODE_THINKING) {
            if (xTaskGetTickCount() - thinking_start_time > pdMS_TO_TICKS(THINKING_TIMEOUT_MS)) {
                ESP_LOGW(TAG, "THINKING timeout (%lums), returning to IDLE", THINKING_TIMEOUT_MS);
                mode = AUDIO_MODE_IDLE;
                last_vad_trigger_time = xTaskGetTickCount();
                vad_trigger_count = 0;
                lvgl_ui_set_status("Idle...");
            }
        }

        // === 7. 定期统计（每10秒）===
        uint32_t now = xTaskGetTickCount();
        if (now - last_stats_time > pdMS_TO_TICKS(10000)) {
            last_stats_time = now;

            ESP_LOGI(TAG, "=== Audio Task Stats ===");
            ESP_LOGI(TAG, "Mode: %s, Frames: %lu",
                     (mode == AUDIO_MODE_IDLE) ? "IDLE" :
                     (mode == AUDIO_MODE_RECORDING) ? "RECORDING" :
                     (mode == AUDIO_MODE_THINKING) ? "THINKING" : "PLAYING",
                     frame_count);
            ESP_LOGI(TAG, "I2S reads: %lu (total samples: %lu)",
                     i2s_read_count, i2s_samples_total);
            ESP_LOGI(TAG, "RingBuffer available: %zu samples",
                     ringbuffer_data_available(&g_pcm_ringbuffer));
            ESP_LOGI(TAG, "AFE energy: %d, VAD: %d",
                     afe.get_audio_energy(), afe.is_voice_active());

            // 打印内存池统计
            pool_print_stats();
        }

        // Yield to other tasks. During PLAYING mode, the 60ms DMA write
        // already yields CPU; skip the extra delay to reduce playback jitter.
        if (mode != AUDIO_MODE_PLAYING) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    // 清理（通常不会到达这里）
    opus_enc.deinit();
    opus_dec.deinit();
    ESP_LOGI(TAG, "Audio Main Task exiting");
    vTaskDelete(NULL);
}
