#include "opus_decoder.h"
#include <esp_log.h>
#include <cstring>

static const char* TAG = "opus_decoder";

OpusDecoder::OpusDecoder() {}

OpusDecoder::~OpusDecoder() {
    deinit();
}

bool OpusDecoder::init(int sample_rate, int channels) {
    if (decoder_) {
        ESP_LOGW(TAG, "Decoder already initialized");
        return true;
    }

    sample_rate_ = sample_rate;
    channels_ = channels;

    // Configure Opus decoder with explicit type conversions
    uint32_t sr = (sample_rate == 16000) ? ESP_AUDIO_SAMPLE_RATE_16K :
                  (sample_rate == 24000) ? ESP_AUDIO_SAMPLE_RATE_24K :
                  ESP_AUDIO_SAMPLE_RATE_48K;

    uint8_t ch = (channels == 1) ? static_cast<uint8_t>(ESP_AUDIO_MONO) : static_cast<uint8_t>(ESP_AUDIO_DUAL);

    esp_opus_dec_cfg_t opus_cfg = {};
    opus_cfg.sample_rate = sr;
    opus_cfg.channel = ch;
    opus_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    opus_cfg.self_delimited = false;

    esp_audio_err_t ret = esp_opus_dec_open(&opus_cfg, sizeof(opus_cfg), &decoder_);
    if (ret != ESP_AUDIO_ERR_OK || !decoder_) {
        ESP_LOGE(TAG, "Failed to create Opus decoder: %d", ret);
        return false;
    }

    // Calculate frame size: 60ms @ sample_rate
    // 60ms = 0.06s, so samples = sample_rate * 0.06
    frame_size_ = (sample_rate * 60) / 1000 * channels;

    ESP_LOGI(TAG, "Opus decoder initialized: %dHz, %dch, frame_size=%d samples",
             sample_rate_, channels_, frame_size_);

    return true;
}

void OpusDecoder::deinit() {
    if (decoder_) {
        esp_opus_dec_close(decoder_);
        decoder_ = nullptr;
        ESP_LOGI(TAG, "Opus decoder closed");
    }
}

void OpusDecoder::reset() {
    if (decoder_) {
        // Close and reopen to reset internal state
        esp_opus_dec_close(decoder_);
        decoder_ = nullptr;

        esp_opus_dec_cfg_t opus_cfg = {};
        uint32_t sr = (sample_rate_ == 16000) ? ESP_AUDIO_SAMPLE_RATE_16K :
                      (sample_rate_ == 24000) ? ESP_AUDIO_SAMPLE_RATE_24K :
                      ESP_AUDIO_SAMPLE_RATE_48K;
        uint8_t ch = (channels_ == 1) ? static_cast<uint8_t>(ESP_AUDIO_MONO) : static_cast<uint8_t>(ESP_AUDIO_DUAL);
        opus_cfg.sample_rate = sr;
        opus_cfg.channel = ch;
        opus_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
        opus_cfg.self_delimited = false;

        esp_audio_err_t ret = esp_opus_dec_open(&opus_cfg, sizeof(opus_cfg), &decoder_);
        if (ret != ESP_AUDIO_ERR_OK || !decoder_) {
            ESP_LOGE(TAG, "Failed to reset Opus decoder: %d", ret);
        } else {
            ESP_LOGI(TAG, "Opus decoder reset successfully");
        }
    }
}

int OpusDecoder::decode(const uint8_t* opus_data, size_t opus_len,
                        int16_t* pcm_out, size_t pcm_max_samples) {
    if (!decoder_) {
        ESP_LOGE(TAG, "Decoder not initialized");
        return -1;
    }

    if (!opus_data || opus_len == 0 || !pcm_out || pcm_max_samples == 0) {
        ESP_LOGE(TAG, "Invalid parameters: opus_data=%p, opus_len=%zu, pcm_out=%p, pcm_max_samples=%zu",
                 opus_data, opus_len, pcm_out, pcm_max_samples);
        return -1;
    }

    ESP_LOGD(TAG, "Decoding Opus packet: len=%zu bytes, max_samples=%zu", opus_len, pcm_max_samples);

    esp_audio_dec_in_raw_t raw = {
        .buffer = const_cast<uint8_t*>(opus_data),
        .len = static_cast<uint32_t>(opus_len),
        .consumed = 0,
        .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
    };

    esp_audio_dec_out_frame_t out_frame = {};
    out_frame.buffer = reinterpret_cast<uint8_t*>(pcm_out);
    out_frame.len = static_cast<uint32_t>(pcm_max_samples * sizeof(int16_t));
    out_frame.decoded_size = 0;

    ESP_LOGD(TAG, "Input buffer: %p, len=%u, Output buffer: %p, len=%u",
             raw.buffer, raw.len, out_frame.buffer, out_frame.len);

    esp_audio_dec_info_t dec_info = {};

    ESP_LOGD(TAG, "Calling esp_opus_dec_decode...");
    esp_audio_err_t ret = esp_opus_dec_decode(decoder_, &raw, &out_frame, &dec_info);

    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Decode failed: error=%d, consumed=%u, decoded_size=%u",
                 ret, raw.consumed, out_frame.decoded_size);
        return -1;
    }

    int samples_decoded = out_frame.decoded_size / sizeof(int16_t);
    ESP_LOGD(TAG, "Decode success: consumed=%u bytes, decoded=%d samples (%u bytes)",
             raw.consumed, samples_decoded, out_frame.decoded_size);

    return samples_decoded;
}
