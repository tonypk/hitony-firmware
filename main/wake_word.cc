#include "wake_word.h"

#include <esp_log.h>
#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#include <model_path.h>

static const char* TAG = "wake_word";

bool WakeWord::init() {
    model_list_ = esp_srmodel_init("model");
    if (!model_list_) {
        ESP_LOGE(TAG, "Failed to init srmodel");
        return false;
    }

    if (model_list_->num <= 0) {
        ESP_LOGE(TAG, "No wake models found");
        return false;
    }

    char* model_name = esp_srmodel_filter(model_list_, ESP_WN_PREFIX, NULL);
    if (!model_name) {
        if (model_list_->model_name && model_list_->model_name[0]) {
            model_name = model_list_->model_name[0];
        }
    }
    if (!model_name) {
        ESP_LOGE(TAG, "No valid wake model name");
        return false;
    }
    ESP_LOGI(TAG, "Using wake model: %s", model_name);
    wakenet_iface_ = esp_wn_handle_from_name(model_name);
    if (!wakenet_iface_) {
        ESP_LOGE(TAG, "Failed to get wakenet iface");
        return false;
    }

    wakenet_data_ = wakenet_iface_->create(model_name, DET_MODE_95);
    if (!wakenet_data_) {
        ESP_LOGE(TAG, "Failed to create wakenet data");
        return false;
    }

    sample_rate_ = wakenet_iface_->get_samp_rate(wakenet_data_);
    chunk_size_ = wakenet_iface_->get_samp_chunksize(wakenet_data_);
    ESP_LOGI(TAG, "Wake word ready: rate=%d, chunk=%d", sample_rate_, chunk_size_);

    return true;
}

void WakeWord::on_detected(std::function<void(const std::string&)> cb) {
    cb_ = cb;
}

void WakeWord::feed(const int16_t* samples, int sample_count) {
    if (!wakenet_data_) return;
    // Process in chunks
    int offset = 0;
    while (offset + chunk_size_ <= sample_count) {
        int res = wakenet_iface_->detect(wakenet_data_, const_cast<int16_t*>(samples + offset));
        if (res > 0) {
            const char* name = wakenet_iface_->get_word_name(wakenet_data_, res);
            ESP_LOGI(TAG, "Wake word detected: %s", name);
            if (cb_) cb_(name ? name : "wake");
        }
        offset += chunk_size_;
    }
}
