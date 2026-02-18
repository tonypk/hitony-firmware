#include "opus_encoder.h"
#include <esp_log.h>
#include <string.h>

static const char* TAG = "opus_enc";

OpusEncoder::OpusEncoder() {
}

OpusEncoder::~OpusEncoder() {
    deinit();
}

bool OpusEncoder::init(int sample_rate, int channels, int bitrate) {
    if (encoder_) {
        ESP_LOGW(TAG, "Encoder already initialized");
        return true;
    }

    sample_rate_ = sample_rate;
    channels_ = channels;
    bitrate_ = bitrate;

    // 配置Opus编码器 (20ms帧, VoIP模式)
    esp_opus_enc_config_t cfg = {
        .sample_rate = sample_rate,
        .channel = channels,
        .bits_per_sample = 16,
        .bitrate = bitrate,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS,  // 20ms帧（减少~40ms编码延迟）
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity = 8,  // 高复杂度 (0-10范围)，优先质量（快速说话需要）
        .enable_fec = false,
        .enable_dtx = false,
        .enable_vbr = true,  // 启用VBR：快速说话时自动提升码率
    };

    esp_audio_err_t ret = esp_opus_enc_open(&cfg, sizeof(cfg), &encoder_);
    if (ret != ESP_AUDIO_ERR_OK || !encoder_) {
        ESP_LOGE(TAG, "Failed to open Opus encoder: %d", ret);
        return false;
    }

    // 获取帧大小
    int in_size = 0;
    int out_size = 0;
    ret = esp_opus_enc_get_frame_size(encoder_, &in_size, &out_size);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to get frame size: %d", ret);
        esp_opus_enc_close(encoder_);
        encoder_ = nullptr;
        return false;
    }

    // in_size是字节数，转换为样本数
    frame_size_ = in_size / (channels * sizeof(int16_t));

    ESP_LOGI(TAG, "Opus encoder initialized: %dHz, %dch, %dbps, frame=%zu samples (%d bytes)",
             sample_rate, channels, bitrate, frame_size_, in_size);

    return true;
}

void OpusEncoder::deinit() {
    if (encoder_) {
        esp_opus_enc_close(encoder_);
        encoder_ = nullptr;
    }
}

int OpusEncoder::encode(const int16_t* pcm_in, size_t pcm_samples,
                        uint8_t* opus_out, size_t opus_max_len) {
    if (!encoder_) {
        ESP_LOGE(TAG, "Encoder not initialized");
        return -1;
    }

    if (!pcm_in || !opus_out) {
        ESP_LOGE(TAG, "Invalid input/output buffer");
        return -1;
    }

    // 检查帧大小匹配
    if (pcm_samples != frame_size_) {
        ESP_LOGE(TAG, "Frame size mismatch: got %zu samples, expected %zu samples",
                 pcm_samples, frame_size_);
        return -1;
    }

    // 输入帧
    uint32_t input_bytes = pcm_samples * channels_ * sizeof(int16_t);
    esp_audio_enc_in_frame_t in_frame = {
        .buffer = (unsigned char*)pcm_in,
        .len = input_bytes,
    };

    // 输出帧
    esp_audio_enc_out_frame_t out_frame = {
        .buffer = opus_out,
        .len = static_cast<uint32_t>(opus_max_len),
        .encoded_bytes = 0,
        .pts = 0,
    };

    // 添加详细调试日志（首次调用）
    static bool first_encode = true;
    if (first_encode) {
        ESP_LOGI(TAG, "🔍 First encode: pcm_samples=%zu, frame_size=%zu, input_bytes=%lu, opus_max_len=%zu",
                 pcm_samples, frame_size_, input_bytes, opus_max_len);
        first_encode = false;
    }

    esp_audio_err_t ret = esp_opus_enc_process(encoder_, &in_frame, &out_frame);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "❌ esp_opus_enc_process failed: error=%d, in_len=%lu, out_max=%lu",
                 ret, input_bytes, opus_max_len);
        return -1;
    }

    return out_frame.encoded_bytes;
}
