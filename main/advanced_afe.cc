#include "advanced_afe.h"
#include "task_manager.h"
#include <esp_log.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_afe_sr_models.h>
#include <esp_afe_config.h>  // For afe_config_init()
#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#include <esp_aec.h>           // For AEC_MODE_* constants
#include <model_path.h>

static const char* TAG = "advanced_afe";

// 使用全局AFE输出队列（定义在app_queues.cc）
extern QueueHandle_t g_afe_output_queue;

AdvancedAFE::AdvancedAFE() {
}

AdvancedAFE::~AdvancedAFE() {
    deinit();
}

bool AdvancedAFE::init(const Config& config) {
    config_ = config;

    ESP_LOGI(TAG, "Initializing AFE: rate=%d, channels=%d, frame=%d",
             config_.sample_rate, config_.channels, config_.frame_size);

    // 创建输入队列（输出使用全局g_afe_output_queue）
    input_queue_ = xQueueCreate(16, sizeof(int16_t*));
    if (!input_queue_) {
        ESP_LOGE(TAG, "Failed to create input queue");
        return false;
    }

    // 使用全局g_afe_output_queue作为输出队列
    output_queue_ = g_afe_output_queue;
    if (!output_queue_) {
        ESP_LOGE(TAG, "Global AFE output queue not initialized");
        vQueueDelete(input_queue_);
        input_queue_ = nullptr;
        return false;
    }

    // === 使用xiaozhi的方法：afe_config_init() ===
    // 1. 初始化模型列表（从分区加载WakeNet模型）
    srmodel_list_t* models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "Failed to init srmodel (WakeNet模型未找到)");
        // 如果WakeNet模型未加载，继续但禁用WakeNet
        models = nullptr;
    } else {
        ESP_LOGI(TAG, "SR models initialized: num=%d", models->num);
        if (models->num > 0 && models->model_name) {
            for (int i = 0; i < models->num && i < 3; i++) {
                if (models->model_name[i]) {
                    ESP_LOGI(TAG, "  Model %d: %s", i, models->model_name[i]);
                }
            }
        }
    }

    // 2. 使用afe_config_init创建默认配置
    // 根据AEC开关决定输入格式和总通道数
    total_channels_ = config_.channels + (config_.enable_aec ? 1 : 0);
    const char* input_format;
    if (config_.enable_aec) {
        input_format = (config_.channels == 2) ? "MMR" : "MR";
    } else {
        input_format = (config_.channels == 2) ? "MM" : "MMN";
    }

    ESP_LOGI(TAG, "Creating AFE config with afe_config_init()...");
    ESP_LOGI(TAG, "  - Input format: %s (mic=%d, total=%d)", input_format, config_.channels, total_channels_);
    // 使用AFE_TYPE_SR以保持WakeNet支持（AFE_TYPE_VC会自动禁用WakeNet）
    ESP_LOGI(TAG, "  - Type: AFE_TYPE_SR (Speech Recognition + AEC)");
    ESP_LOGI(TAG, "  - Mode: AFE_MODE_HIGH_PERF");
    ESP_LOGI(TAG, "  - AEC: %s", config_.enable_aec ? "ENABLED" : "disabled");

    afe_config_t* afe_config = afe_config_init(
        input_format,        // 输入格式："MMR" (双麦+参考) 或 "MM" (双麦)
        models,              // 模型列表（可能为nullptr）
        AFE_TYPE_SR,         // SR模式：支持WakeNet + AEC
        AFE_MODE_HIGH_PERF   // 高性能模式
    );

    if (!afe_config) {
        ESP_LOGE(TAG, "afe_config_init() failed");
        return false;
    }
    ESP_LOGI(TAG, "✓ afe_config_init() succeeded");

    // 3. 调整特定配置（根据用户Config覆盖默认值）
    // 注意：afe_config_init已经设置了大部分参数，我们只需要覆盖特定的

    // AEC配置
    if (config_.enable_aec) {
        afe_config->aec_init = true;
        afe_config->aec_mode = AEC_MODE_SR_LOW_COST;  // LOW_COST使用esp_aec3_728（ESP32-S3优化版），更稳定
        afe_config->aec_filter_length = 4;              // ESP32-S3推荐值
        ESP_LOGI(TAG, "AEC enabled: mode=SR_LOW_COST, filter_length=4");
    } else {
        afe_config->aec_init = false;
    }

    // AGC自动增益控制
    if (config_.enable_agc) {
        afe_config->agc_init = true;
        afe_config->agc_mode = AFE_AGC_MODE_WAKENET;  // WakeNet协同模式
        afe_config->agc_compression_gain_db = 18;     // 压缩增益18dB（默认9）
        afe_config->agc_target_level_dbfs = 3;        // 目标 -3 dBFS
        ESP_LOGI(TAG, "AGC enabled: mode=WAKENET, gain=%ddB, target=-%ddBFS",
                 afe_config->agc_compression_gain_db, afe_config->agc_target_level_dbfs);
    } else {
        afe_config->agc_init = false;
    }

    // NS噪声抑制
    afe_config->ns_init = config_.enable_ns;
    if (!config_.enable_ns) {
        ESP_LOGI(TAG, "NS disabled (avoid over-suppression of quiet mic signal)");
    }

    // VAD设置
    afe_config->vad_init = config_.enable_vad;
    if (config_.enable_vad) {
        // VAD_MODE_0: 质量优先模式（最宽容，适合快速说话，不易切断）
        // VAD_MODE_3: 激进模式（容易误切断快速说话）
        afe_config->vad_mode = VAD_MODE_0;  // 改用质量模式，避免切断快速说话
        ESP_LOGI(TAG, "VAD enabled: mode=VAD_MODE_0 (quality, lenient for fast speech)");
    }

    // WakeNet设置
    if (!config_.enable_wakenet || !models) {
        // 如果用户禁用或模型未加载，强制禁用WakeNet
        afe_config->wakenet_init = false;
        afe_config->wakenet_model_name = nullptr;
        afe_config->wakenet_model_name_2 = nullptr;
        ESP_LOGI(TAG, "WakeNet disabled (user config or model not found)");
    } else {
        // afe_config_init已经设置了WakeNet，保持启用
        ESP_LOGI(TAG, "WakeNet enabled by afe_config_init");
        if (afe_config->wakenet_model_name) {
            ESP_LOGI(TAG, "  Model: %s", afe_config->wakenet_model_name);
        }
    }

    // BSS波束形成：禁用（ESP32-S3算力不足以实时处理BSS+AEC+WakeNet）
    // BSS使chunk_size从512翻倍到1024，导致AFE内部ringbuffer持续溢出
    afe_config->se_init = false;
    ESP_LOGI(TAG, "BSS beamforming DISABLED (ESP32-S3 CPU insufficient for real-time BSS)");

    // 内存分配模式：优先使用PSRAM
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_config->afe_perferred_core = 1;  // Core 1执行AFE任务

    // 4. 打印最终配置（用于调试）
    ESP_LOGI(TAG, "Final AFE configuration:");
    ESP_LOGI(TAG, "  - WakeNet: %s", afe_config->wakenet_init ? "ON" : "OFF");
    ESP_LOGI(TAG, "  - VAD: %s", afe_config->vad_init ? "ON" : "OFF");
    ESP_LOGI(TAG, "  - NS: %s", afe_config->ns_init ? "ON" : "OFF");
    ESP_LOGI(TAG, "  - AEC: %s", afe_config->aec_init ? "ON" : "OFF");
    ESP_LOGI(TAG, "  - AGC: %s", afe_config->agc_init ? "ON" : "OFF");
    ESP_LOGI(TAG, "  - SE (Beamforming): %s", afe_config->se_init ? "ON" : "OFF");

    // 5. 创建AFE handle
    ESP_LOGI(TAG, "Creating AFE handle...");
    afe_handle_ = esp_afe_handle_from_config(afe_config);
    if (!afe_handle_) {
        ESP_LOGE(TAG, "Failed to get AFE handle");
        afe_config_free(afe_config);  // 使用ESP-SR提供的释放函数
        vQueueDelete(input_queue_);
        input_queue_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "✓ AFE handle created successfully");

    // 6. 创建AFE data（此处可能初始化WakeNet模型）
    if (afe_config->wakenet_init) {
        ESP_LOGI(TAG, "⚠️  Starting AFE data creation (WakeNet enabled, may take 5-10 seconds)...");
        ESP_LOGI(TAG, "    Free heap before: %lu bytes, PSRAM: %lu bytes",
                 esp_get_free_heap_size(),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    afe_data_ = afe_handle_->create_from_config(afe_config);

    if (afe_config->wakenet_init) {
        ESP_LOGI(TAG, "    Free heap after: %lu bytes, PSRAM: %lu bytes",
                 esp_get_free_heap_size(),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    // 7. 释放配置（afe_data_已经拷贝了配置）
    afe_config_free(afe_config);
    afe_config = nullptr;

    if (!afe_data_) {
        ESP_LOGE(TAG, "❌ Failed to create AFE data");
        ESP_LOGE(TAG, "Possible causes:");
        ESP_LOGE(TAG, "  1. WakeNet model file not found in partition");
        ESP_LOGE(TAG, "  2. Insufficient memory (heap or PSRAM)");
        ESP_LOGE(TAG, "  3. Model incompatible with ESP-SR version");
        vQueueDelete(input_queue_);
        input_queue_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "✅ AFE data created successfully!");

    // 打印AFE配置信息
    int afe_chunk_size = afe_handle_->get_feed_chunksize(afe_data_);
    int afe_sample_rate = afe_handle_->get_samp_rate(afe_data_);
    int afe_channels = afe_handle_->get_fetch_channel_num(afe_data_);

    ESP_LOGI(TAG, "AFE initialized: chunk=%d, rate=%d, channels=%d",
             afe_chunk_size, afe_sample_rate, afe_channels);

    // 打印AFE处理管道
    afe_handle_->print_pipeline(afe_data_);

    // 分配临时缓冲区（使用total_channels_包含参考通道）
    temp_buffer_size_ = afe_chunk_size * total_channels_;
    temp_buffer_ = (int16_t*)heap_caps_malloc(
        temp_buffer_size_ * sizeof(int16_t),
        MALLOC_CAP_SPIRAM);

    if (!temp_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate temp buffer");
        afe_handle_->destroy(afe_data_);
        afe_data_ = nullptr;
        vQueueDelete(input_queue_);
        vQueueDelete(output_queue_);
        input_queue_ = nullptr;
        output_queue_ = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "AFE ready");
    return true;
}

void AdvancedAFE::deinit() {
    stop();

    if (afe_data_) {
        afe_handle_->destroy(afe_data_);
        afe_data_ = nullptr;
    }

    if (temp_buffer_) {
        heap_caps_free(temp_buffer_);
        temp_buffer_ = nullptr;
    }

    if (input_queue_) {
        // 清空队列
        int16_t* buf;
        while (xQueueReceive(input_queue_, &buf, 0) == pdTRUE) {
            heap_caps_free(buf);
        }
        vQueueDelete(input_queue_);
        input_queue_ = nullptr;
    }

    // output_queue_是全局的，不删除，只清空我们的数据
    if (output_queue_) {
        audio_data_msg_t* msg;
        while (xQueueReceive(output_queue_, &msg, 0) == pdTRUE) {
            if (msg && msg->data) {
                heap_caps_free(msg->data);
            }
            heap_caps_free(msg);
        }
        output_queue_ = nullptr;  // 不删除，只是解除引用
    }

    ESP_LOGI(TAG, "AFE deinitialized");
}

bool AdvancedAFE::start() {
    if (task_handle_) {
        ESP_LOGW(TAG, "AFE task already running");
        return true;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        afe_task,
        "afe_task",  // 重命名以便栈监控识别
        12288,  // 12KB (减少以释放8KB RAM，WakeNet已通过afe_config_init修复)
        this,
        18,     // 优先级18 (与现有afe_process_task一致)
        &task_handle_,
        1       // Core 1
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AFE task");
        return false;
    }

    ESP_LOGI(TAG, "AFE task started");
    return true;
}

void AdvancedAFE::stop() {
    if (task_handle_) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
        ESP_LOGI(TAG, "AFE task stopped");
    }
}

void AdvancedAFE::enable_aec(bool enable) {
    if (!afe_handle_ || !afe_data_) return;
    if (enable) {
        afe_handle_->enable_aec(afe_data_);
        aec_counter_reset_ = true;  // 重置零输出计数器
        ESP_LOGI(TAG, "AEC dynamically ENABLED (TTS playback)");
    } else {
        afe_handle_->disable_aec(afe_data_);
        ESP_LOGI(TAG, "AEC dynamically DISABLED (idle/recording)");
    }
}

void AdvancedAFE::enable_wakenet(bool enable) {
    if (!afe_handle_ || !afe_data_) return;
    if (enable) {
        afe_handle_->enable_wakenet(afe_data_);
        ESP_LOGI(TAG, "WakeNet dynamically ENABLED (meeting ended)");
    } else {
        afe_handle_->disable_wakenet(afe_data_);
        ESP_LOGI(TAG, "WakeNet dynamically DISABLED (meeting mode, saving CPU)");
    }
}

void AdvancedAFE::feed(const int16_t* data, size_t samples) {
    if (!afe_data_ || !input_queue_) {
        return;
    }

    // 分配缓冲区并拷贝数据（使用内存池，避免每16ms malloc/free）
    size_t size = samples * total_channels_ * sizeof(int16_t);
    // 256 samples * 3 channels * 2 bytes = 1536B → fits POOL_L_2K
    int16_t* buf = (int16_t*)pool_alloc(POOL_L_2K);
    if (!buf) {
        ESP_LOGW(TAG, "Failed to allocate feed buffer from pool");
        return;
    }

    memcpy(buf, data, size);

    // 发送到输入队列（非阻塞）
    if (xQueueSend(input_queue_, &buf, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Input queue full, dropping frame");
        pool_free(POOL_L_2K, buf);
    }
}

int AdvancedAFE::fetch(int16_t* out, size_t max_samples) {
    if (!output_queue_) {
        ESP_LOGE(TAG, "fetch(): output_queue_ is nullptr!");
        return 0;
    }

    // 定期打印队列状态（每30000次，约每15分钟）
    static uint32_t fetch_call_count = 0;
    fetch_call_count++;
    if (fetch_call_count % 30000 == 0) {
        UBaseType_t queued = uxQueueMessagesWaiting(output_queue_);
        ESP_LOGD(TAG, "fetch() call #%lu: queue has %u messages", fetch_call_count, queued);
    }

    audio_data_msg_t* msg = nullptr;
    if (xQueueReceive(output_queue_, &msg, 0) == pdTRUE) {
        if (msg && msg->data) {
            // 拷贝数据
            size_t samples = (max_samples < msg->samples) ? max_samples : msg->samples;
            memcpy(out, msg->data, samples * sizeof(int16_t));

            // 释放消息
            free_audio_msg(msg);

            return samples;
        }
    }

    return 0;
}

void AdvancedAFE::afe_task(void* arg) {
    AdvancedAFE* self = (AdvancedAFE*)arg;
    self->process_loop();
}

void AdvancedAFE::process_loop() {
    ESP_LOGI(TAG, "AFE processing loop started on core %d", xPortGetCoreID());

    int afe_chunk_size = afe_handle_->get_feed_chunksize(afe_data_);
    size_t accumulated_samples = 0;
    uint32_t frame_count = 0;

    while (true) {
        int16_t* input_buf = nullptr;

        // 从输入队列获取数据
        if (xQueueReceive(input_queue_, &input_buf, portMAX_DELAY) == pdTRUE) {
            if (!input_buf) {
                continue;
            }

            frame_count++;

            // 累积样本到临时缓冲区（包含参考通道）
            size_t input_samples = config_.frame_size * total_channels_;
            if (accumulated_samples + input_samples <= temp_buffer_size_) {
                memcpy(temp_buffer_ + accumulated_samples, input_buf,
                       input_samples * sizeof(int16_t));
                accumulated_samples += input_samples;
            }

            pool_free(POOL_L_2K, input_buf);

            // 当累积的样本达到AFE chunk size时，进行处理
            size_t required_samples = afe_chunk_size * total_channels_;
            while (accumulated_samples >= required_samples) {
                // 喂入AFE
                afe_handle_->feed(afe_data_, temp_buffer_);

                // 稍等一下让AFE内部任务处理数据（避免ringbuffer满）
                vTaskDelay(pdMS_TO_TICKS(1));

                // 获取AFE输出
                afe_fetch_result_t* res = afe_handle_->fetch(afe_data_);
                if (res && res->data) {
                    // 计算样本数（单声道）
                    int samples = res->data_size / sizeof(int16_t);

                    // AEC零输出检测：如果连续100帧全零，自动禁用AEC（防止MR格式bug）
                    static uint32_t consecutive_zero_frames = 0;
                    if (aec_counter_reset_) {
                        consecutive_zero_frames = 0;
                        aec_counter_reset_ = false;
                    }
                    bool all_zero = true;
                    int16_t* pcm = (int16_t*)res->data;
                    for (int i = 0; i < samples && all_zero; i++) {
                        if (pcm[i] != 0) all_zero = false;
                    }
                    if (all_zero) {
                        if (++consecutive_zero_frames == 100) {
                            ESP_LOGE(TAG, "❌ AEC FAILURE: 100 consecutive zero-output frames! Disabling AEC as fallback");
                            afe_handle_->disable_aec(afe_data_);
                        }
                    } else {
                        consecutive_zero_frames = 0;
                    }

                    // 分配音频消息
                    audio_data_msg_t* msg = alloc_audio_msg(samples, 1);
                    if (msg) {
                        memcpy(msg->data, res->data, res->data_size);

                        // 发送到全局AFE输出队列
                        if (xQueueSend(output_queue_, &msg, 0) != pdTRUE) {
                            ESP_LOGW(TAG, "AFE output queue full, dropping frame");
                            free_audio_msg(msg);
                        } else {
                            // 每100帧打印一次发送成功
                            static uint32_t send_count = 0;
                            send_count++;
                            if (send_count % 1000 == 0) {
                                ESP_LOGI(TAG, "✅ AFE sent %lu msgs to queue (samples=%d)", send_count, samples);
                            }
                        }
                    } else {
                        // alloc失败
                        static uint32_t alloc_fail_count = 0;
                        alloc_fail_count++;
                        if (alloc_fail_count % 50 == 0) {
                            ESP_LOGW(TAG, "⚠️ AFE alloc failed %lu times", alloc_fail_count);
                        }
                    }

                    // 检测唤醒词（添加调试日志）
                    // WakeNet状态值：0=未检测，1=检测中，2=已检测(WAKENET_DETECTED)
                    static uint32_t wakenet_frame_count = 0;
                    wakenet_frame_count++;

                    // 每100帧打印一次WakeNet状态（即使未检测到）
                    if (wakenet_frame_count % 1000 == 0) {
                        ESP_LOGI(TAG, "🔍 WakeNet: state=%d, volume=%.2f, vad=%d (frame #%lu)",
                                 res->wakeup_state, res->data_volume, res->vad_state, wakenet_frame_count);
                    }

                    if (res->wakeup_state == WAKENET_DETECTED) {
                        const char* wake_word = "wake";
                        if (res->wake_word_index > 0 && config_.wake_words &&
                            res->wake_word_index <= config_.wake_word_count) {
                            wake_word = config_.wake_words[res->wake_word_index - 1];
                        }

                        wake_detected_ = true;
                        ESP_LOGI(TAG, "🎉🎉🎉 Wake word detected: %s (index=%d)",
                                 wake_word, res->wake_word_index);

                        if (wake_cb_) {
                            wake_cb_(wake_word);
                        }
                    } else if (res->wakeup_state > 0) {
                        // WakeNet正在处理但未检测到（状态=1表示检测中）
                        ESP_LOGI(TAG, "⚠️ WakeNet processing (state=%d, volume=%.2f)",
                                 res->wakeup_state, res->data_volume);
                    }

                    // 检测VAD
                    if (config_.enable_vad) {
                        bool new_vad_state = (res->vad_state == VAD_SPEECH);
                        if (new_vad_state != vad_active_) {
                            vad_active_ = new_vad_state;
                            if (vad_cb_) {
                                vad_cb_(vad_active_);
                            }
                        }
                    }

                    // 更新音频能量（用于UI动画）
                    audio_energy_ = (int)(res->data_volume * 10);
                }

                // 移动剩余数据到缓冲区开头
                if (accumulated_samples > required_samples) {
                    memmove(temp_buffer_,
                            temp_buffer_ + required_samples,
                            (accumulated_samples - required_samples) * sizeof(int16_t));
                }
                accumulated_samples -= required_samples;
            }

            // 每5秒打印一次统计
            if (frame_count % 1560 == 0) {
                ESP_LOGI(TAG, "AFE stats: processed=%lu, energy=%d, vad=%d",
                         frame_count, audio_energy_, vad_active_);
            }
        }
    }
}
