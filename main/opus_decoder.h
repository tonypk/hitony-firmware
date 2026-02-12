#pragma once

#include <vector>
#include <cstdint>
#include <esp_audio_dec.h>
#include <esp_opus_dec.h>

class OpusDecoder {
public:
    OpusDecoder();
    ~OpusDecoder();

    bool init(int sample_rate = 16000, int channels = 1);
    void deinit();

    // Decode Opus packet to PCM16
    // Returns number of PCM samples decoded (or -1 on error)
    int decode(const uint8_t* opus_data, size_t opus_len, int16_t* pcm_out, size_t pcm_max_samples);

    // Reset decoder state (call between sessions to clear residual state)
    void reset();

    // Get expected PCM frame size for current configuration
    size_t frame_size() const { return frame_size_; }

private:
    void* decoder_ = nullptr;
    int sample_rate_ = 16000;
    int channels_ = 1;
    size_t frame_size_ = 0;  // PCM samples per frame
};
