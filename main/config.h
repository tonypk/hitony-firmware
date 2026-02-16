#pragma once

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <driver/i2c.h>

// WiFi配置模式选择
// 0 = 使用配网模式（AP热点配网）
// 1 = 使用硬编码WiFi（快速测试）
#define HITONY_USE_HARDCODED_WIFI 0

// 硬编码WiFi配置（仅当HITONY_USE_HARDCODED_WIFI=1时使用）
#define HITONY_WIFI_SSID "Tonyphone"
#define HITONY_WIFI_PASSWORD "12345678"

// WebSocket endpoint
#define HITONY_WS_URL "ws://136.111.249.161:9001/ws"  // Using IP to avoid DNS issues

// Device identity / token
// IMPORTANT: Replace HITONY_DEVICE_TOKEN with your actual device token
// Register via: curl -X POST http://SERVER:9001/register -H 'Content-Type: application/json'
//   -d '{"device_id":"hitony-001","token":"your_token"}'
#define HITONY_DEVICE_ID "hitony-001"
#define HITONY_DEVICE_TOKEN "devtoken"  // TODO: Replace with your actual token

// Audio parameters (恢复16kHz，使用更高增益补偿音量)
#define HITONY_SAMPLE_RATE 16000  // 恢复16kHz
#define HITONY_CHANNELS 1
#define HITONY_BITS 16

// Audio frame size (20ms @ 16kHz mono = 320 samples = 640 bytes)
#define HITONY_FRAME_SAMPLES 320
#define HITONY_FRAME_BYTES (HITONY_FRAME_SAMPLES * 2)

// HiTony v1.2 pin mapping (from HiTony_SCH_V1_2 / xiaozhi board config)
#define HITONY_I2S_MCLK  GPIO_NUM_42
#define HITONY_I2S_BCLK  GPIO_NUM_40
#define HITONY_I2S_WS    GPIO_NUM_39
#define HITONY_I2S_DOUT  GPIO_NUM_41  // ES8311 DAC
#define HITONY_I2S_DIN   GPIO_NUM_3   // ES7210 ADC (MIC1), HiTony v1.2

#define HITONY_CODEC_PWR GPIO_NUM_48
#define HITONY_CODEC_PA  GPIO_NUM_15  // HiTony v1.2
#define HITONY_CODEC_PA2 GPIO_NUM_NC

#define HITONY_I2C_SDA   GPIO_NUM_2
#define HITONY_I2C_SCL   GPIO_NUM_1
#define HITONY_I2C_PORT  I2C_NUM_0
// Touch (CST816S on HiTony; I2C on same bus)
#define HITONY_TP_INT    GPIO_NUM_10
#define HITONY_TP_RST    GPIO_NUM_NC

// Display (HiTony QSPI ST77916, 360x360)
#define HITONY_DISPLAY_WIDTH 360
#define HITONY_DISPLAY_HEIGHT 360

#define HITONY_POWER_CTRL GPIO_NUM_9

#define HITONY_QSPI_LCD_HOST SPI2_HOST
#define HITONY_QSPI_PCLK     GPIO_NUM_18
#define HITONY_QSPI_CS       GPIO_NUM_14
#define HITONY_QSPI_DC       GPIO_NUM_45
#define HITONY_QSPI_D0       GPIO_NUM_46
#define HITONY_QSPI_D1       GPIO_NUM_13
#define HITONY_QSPI_D2       GPIO_NUM_11
#define HITONY_QSPI_D3       GPIO_NUM_12
#define HITONY_QSPI_RST      GPIO_NUM_47   // HiTony v1.2
#define HITONY_QSPI_RST_ALT  GPIO_NUM_NC
#define HITONY_QSPI_BL       GPIO_NUM_44
#define HITONY_QSPI_BL_ALT   GPIO_NUM_NC
#define HITONY_BL_ACTIVE_LOW 0

#define HITONY_LCD_BITS_PER_PIXEL 16
#define HITONY_LCD_RESET_ACTIVE_HIGH 1

// Use HiTony QSPI LCD path (same as xiaozhi-esp32 HiTony board)
#define HITONY_LCD_USE_QSPI  1
// SPI test MOSI pin (use D0 for 1-line SPI)
#define HITONY_LCD_SPI_MOSI  HITONY_QSPI_D0

#define HITONY_DISPLAY_MIRROR_X false
#define HITONY_DISPLAY_MIRROR_Y false
#define HITONY_DISPLAY_SWAP_XY  false

// Use vendor-specific LCD init sequence (set to 0 to use driver default)
#define HITONY_LCD_USE_CUSTOM_INIT 1
// Run LCD-only bring-up test (no LVGL, no Wi-Fi, no audio)
#define HITONY_LCD_ONLY_TEST 0

// QSPI bus config helper (from xiaozhi-esp32 HiTony board config)
#define HITONY_ST77916_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz) \
    {                                                                             \
        .data0_io_num = d0,                                                       \
        .data1_io_num = d1,                                                       \
        .sclk_io_num = sclk,                                                      \
        .data2_io_num = d2,                                                       \
        .data3_io_num = d3,                                                       \
        .max_transfer_sz = max_trans_sz,                                          \
    }

// Hardware LED indicator (HiTony v1.2)
#define HITONY_LED_G GPIO_NUM_43  // Green LED for status indication

// Firmware version (semantic versioning)
#define HITONY_FW_VERSION "2.2.3"

// OTA update server URL (HTTP, not HTTPS — ESP32-S3 with limited RAM)
#define HITONY_OTA_URL "http://136.111.249.161/api/ota/firmware"

// Feature toggles
#define HITONY_ENABLE_WAKE_WORD 0

// ESP-SR wake word
typedef enum {
    WAKE_WORD_MODE_AFE = 0,
    WAKE_WORD_MODE_ESP = 1,
} wake_word_mode_t;

#define HITONY_WAKE_WORD_MODE WAKE_WORD_MODE_AFE
