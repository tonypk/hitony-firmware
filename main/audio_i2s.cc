#include "audio_i2s.h"
#include "config.h"

#include <esp_log.h>
#include <esp_err.h>
#include <driver/i2s_std.h>
#include <driver/i2s_tdm.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>

static const char* TAG = "audio_i2s";

// 轻量级初始化：只创建I2C总线（用于配网模式触摸检测）
bool AudioI2S::init_i2c_only() {
    if (i2c_bus_) {
        ESP_LOGI(TAG, "I2C bus already initialized");
        return true;
    }

    // I2C master for touch sensor (and later for codec)
    i2c_master_bus_config_t i2c_bus_cfg = {};
    i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_bus_cfg.i2c_port = HITONY_I2C_PORT;
    i2c_bus_cfg.sda_io_num = HITONY_I2C_SDA;
    i2c_bus_cfg.scl_io_num = HITONY_I2C_SCL;
    i2c_bus_cfg.glitch_ignore_cnt = 7;
    i2c_bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t ret = i2c_new_master_bus(&i2c_bus_cfg, (i2c_master_bus_handle_t*)&i2c_bus_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "I2C bus initialized (lightweight mode, no I2S/DMA)");
    return true;
}

bool AudioI2S::init() {
    // 先初始化I2C（如果还没初始化）
    if (!i2c_bus_) {
        if (!init_i2c_only()) {
            ESP_LOGE(TAG, "Failed to initialize I2C bus");
            return false;
        }
    } else {
        ESP_LOGI(TAG, "I2C bus already initialized, skipping I2C init");
    }

    // Power up codec and PA
    gpio_config_t io_conf = {};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << HITONY_CODEC_PWR) | (1ULL << HITONY_CODEC_PA);
#if HITONY_CODEC_PA2 != HITONY_I2S_DIN
    io_conf.pin_bit_mask |= (1ULL << HITONY_CODEC_PA2);
#endif
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(HITONY_CODEC_PWR, 1));
    ESP_ERROR_CHECK(gpio_set_level(HITONY_CODEC_PA, 1));
#if HITONY_CODEC_PA2 != HITONY_I2S_DIN
    ESP_ERROR_CHECK(gpio_set_level(HITONY_CODEC_PA2, 1));
#endif

    // I2C已在init_i2c_only()中创建，这里不再重复创建

    // Create I2S channels (TX std + RX tdm for ES7210)
    i2s_chan_config_t chan_cfg = {};
    chan_cfg.id = I2S_NUM_0;
    chan_cfg.role = I2S_ROLE_MASTER;
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.auto_clear_before_cb = false;
    chan_cfg.intr_priority = 0;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, (i2s_chan_handle_t*)&tx_chan_, (i2s_chan_handle_t*)&rx_chan_));

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg.sample_rate_hz = HITONY_SAMPLE_RATE;
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std_cfg.clk_cfg.ext_clk_freq_hz = 0;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    std_cfg.slot_cfg.ws_pol = false;
    std_cfg.slot_cfg.bit_shift = true;
    std_cfg.slot_cfg.left_align = true;
    std_cfg.slot_cfg.big_endian = false;
    std_cfg.slot_cfg.bit_order_lsb = false;
    std_cfg.gpio_cfg.mclk = HITONY_I2S_MCLK;
    std_cfg.gpio_cfg.bclk = HITONY_I2S_BCLK;
    std_cfg.gpio_cfg.ws = HITONY_I2S_WS;
    std_cfg.gpio_cfg.dout = HITONY_I2S_DOUT;
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    i2s_tdm_config_t tdm_cfg = {};
    tdm_cfg.clk_cfg.sample_rate_hz = HITONY_SAMPLE_RATE;
    tdm_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    tdm_cfg.clk_cfg.ext_clk_freq_hz = 0;
    tdm_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tdm_cfg.clk_cfg.bclk_div = 8;
    tdm_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    tdm_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    tdm_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    tdm_cfg.slot_cfg.slot_mask = (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3);
    tdm_cfg.slot_cfg.ws_width = I2S_TDM_AUTO_WS_WIDTH;
    tdm_cfg.slot_cfg.ws_pol = false;
    tdm_cfg.slot_cfg.bit_shift = true;
    tdm_cfg.slot_cfg.left_align = false;
    tdm_cfg.slot_cfg.big_endian = false;
    tdm_cfg.slot_cfg.bit_order_lsb = false;
    tdm_cfg.slot_cfg.skip_mask = false;
    tdm_cfg.slot_cfg.total_slot = I2S_TDM_AUTO_SLOT_NUM;
    tdm_cfg.gpio_cfg.mclk = HITONY_I2S_MCLK;
    tdm_cfg.gpio_cfg.bclk = HITONY_I2S_BCLK;
    tdm_cfg.gpio_cfg.ws = HITONY_I2S_WS;
    tdm_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    tdm_cfg.gpio_cfg.din = HITONY_I2S_DIN;
    tdm_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    tdm_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    tdm_cfg.gpio_cfg.invert_flags.ws_inv = false;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode((i2s_chan_handle_t)tx_chan_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode((i2s_chan_handle_t)rx_chan_, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable((i2s_chan_handle_t)tx_chan_));
    ESP_ERROR_CHECK(i2s_channel_enable((i2s_chan_handle_t)rx_chan_));

    // Create codec data/control interfaces
    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = I2S_NUM_0;
    i2s_cfg.rx_handle = (i2s_chan_handle_t)rx_chan_;
    i2s_cfg.tx_handle = (i2s_chan_handle_t)tx_chan_;
    const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)HITONY_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = (i2c_master_bus_handle_t)i2c_bus_,
    };
    const audio_codec_ctrl_if_t* out_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl;
    es8311_cfg.gpio_if = gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = HITONY_CODEC_PA;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    const audio_codec_if_t* out_codec = es8311_codec_new(&es8311_cfg);

    esp_codec_dev_cfg_t out_cfg = {};
    out_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    out_cfg.codec_if = out_codec;
    out_cfg.data_if = data_if;
    output_dev_ = esp_codec_dev_new(&out_cfg);
    if (!output_dev_) {
        ESP_LOGE(TAG, "Failed to create output codec device");
        return false;
    }

    // Input codec (ES7210)
    i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t* in_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2;
    const audio_codec_if_t* in_codec = es7210_codec_new(&es7210_cfg);

    esp_codec_dev_cfg_t in_cfg = {};
    in_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    in_cfg.codec_if = in_codec;
    in_cfg.data_if = data_if;
    input_dev_ = esp_codec_dev_new(&in_cfg);
    if (!input_dev_) {
        ESP_LOGE(TAG, "Failed to create input codec device");
        return false;
    }

    // Open devices
    esp_codec_dev_sample_info_t in_fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),  // MIC1 + MIC2
        .sample_rate = (uint32_t)HITONY_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open((esp_codec_dev_handle_t)input_dev_, &in_fs));

    // 设置麦克风输入增益（使用最大值37.5dB补偿16kHz音量不足）
    // 这是WakeNet正常工作的关键！增益不足会导致唤醒词识别失败
    // 为双麦克风（MIC1+MIC2）设置增益
    // ES7210支持0-37.5dB增益范围，使用最大值以获得最佳效果
    esp_err_t gain_err = esp_codec_dev_set_in_channel_gain(
        (esp_codec_dev_handle_t)input_dev_,
        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),  // MIC1 + MIC2
        37.5  // 最大增益37.5dB（相比30dB提升2.4倍音量）
    );
    if (gain_err == ESP_OK) {
        ESP_LOGI(TAG, "✅ Microphone gain set to 37.5dB @ 16kHz (MIC1 + MIC2)");
    } else {
        ESP_LOGW(TAG, "Failed to set microphone gain: %s", esp_err_to_name(gain_err));
    }

    esp_codec_dev_sample_info_t out_fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = (uint32_t)HITONY_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open((esp_codec_dev_handle_t)output_dev_, &out_fs));
    // Ensure output is unmuted and loud enough
    esp_codec_dev_set_out_mute((esp_codec_dev_handle_t)output_dev_, false);
    esp_codec_dev_set_out_vol((esp_codec_dev_handle_t)output_dev_, 100);

    ESP_LOGI(TAG, "Codec initialized (ES7210 + ES8311)");
    return true;
}

int AudioI2S::read_frame(uint8_t* buf, size_t len) {
    if (!input_dev_) return -1;
    esp_err_t err = esp_codec_dev_read((esp_codec_dev_handle_t)input_dev_, buf, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read audio frame: %d", err);
        return -1;
    }
    return (int)len;
}

int AudioI2S::play_frame(const uint8_t* buf, size_t len) {
    if (!output_dev_) return -1;
    esp_err_t err = esp_codec_dev_write((esp_codec_dev_handle_t)output_dev_, (void*)buf, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write audio frame: %d", err);
        return -1;
    }
    return (int)len;
}
