#pragma once

#include <cstddef>
#include <cstdint>

class AudioI2S {
public:
    static AudioI2S& instance() {
        static AudioI2S inst;
        return inst;
    }

    bool init();  // 完整初始化：I2C + I2S + Codec
    bool init_i2c_only();  // 轻量级初始化：只创建I2C总线（用于配网模式）
    int read_frame(uint8_t* buf, size_t len);
    int play_frame(const uint8_t* buf, size_t len);
    void play_test_tone();
    void* i2c_bus() const { return i2c_bus_; }

private:
    AudioI2S() = default;
    void* tx_chan_ = nullptr; // i2s_chan_handle_t
    void* rx_chan_ = nullptr; // i2s_chan_handle_t
    void* i2c_bus_ = nullptr; // i2c_master_bus_handle_t
    void* output_dev_ = nullptr; // esp_codec_dev_handle_t
    void* input_dev_ = nullptr;  // esp_codec_dev_handle_t
};
