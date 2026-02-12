#pragma once

#include <vector>
#include <cstdint>
#include <esp_audio_enc.h>
#include <esp_opus_enc.h>

class OpusEncoder {
public:
    OpusEncoder();
    ~OpusEncoder();

    bool init(int sample_rate = 16000, int channels = 1, int bitrate = 24000);
    void deinit();

    // Encode PCM16 to Opus packet
    // Returns number of bytes encoded (or -1 on error)
    int encode(const int16_t* pcm_in, size_t pcm_samples, uint8_t* opus_out, size_t opus_max_len);

    // Get expected PCM frame size for current configuration
    size_t frame_size() const { return frame_size_; }

private:
    void* encoder_ = nullptr;
    int sample_rate_ = 16000;
    int channels_ = 1;
    int bitrate_ = 24000;
    size_t frame_size_ = 0;  // PCM samples per frame
};
