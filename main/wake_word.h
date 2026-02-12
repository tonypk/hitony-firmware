#pragma once

#include <functional>
#include <string>

#include <esp_wn_iface.h>
#include <model_path.h>

class WakeWord {
public:
    bool init();
    void on_detected(std::function<void(const std::string&)> cb);
    void feed(const int16_t* samples, int sample_count);

    int sample_rate() const { return sample_rate_; }
    int chunk_size() const { return chunk_size_; }

private:
    std::function<void(const std::string&)> cb_;
    srmodel_list_t* model_list_ = nullptr;
    model_iface_data_t* wakenet_data_ = nullptr;
    const esp_wn_iface_t* wakenet_iface_ = nullptr;
    int sample_rate_ = 16000;
    int chunk_size_ = 320;
};
