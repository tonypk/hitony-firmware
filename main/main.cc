/**
 * @file main_new.cc
 * @brief HiTony 智能音箱主程序 - 新架构
 *
 * 这是重构后的main.cc，采用双核任务分离架构
 * 使用时请将此文件重命名为main.cc替换旧版本
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <nvs_flash.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <driver/gpio.h>
#include <string.h>
#include <lwip/ip_addr.h>

#include "config.h"
#include "audio_i2s.h"
#include "lvgl_ui.h"
#include "task_manager.h"
#include "system_monitor.h"
#include "led_controller.h"
#include "wifi_provisioning.h"

static const char* TAG = "main";

// ============================================================================
// 基础初始化
// ============================================================================

static void init_nvs() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");
}

static void init_gpio() {
    // 初始化LED引脚
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << HITONY_LED_G);  // 只初始化绿色LED
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    // 关闭LED
    gpio_set_level(HITONY_LED_G, 0);

    ESP_LOGI(TAG, "GPIO initialized");
}

// WiFi事件处理器
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // 获取详细的断开原因
        wifi_event_sta_disconnected_t* disconn_evt = (wifi_event_sta_disconnected_t*)event_data;
        ESP_LOGW(TAG, "WiFi disconnected! Reason: %d (%s)",
                 disconn_evt->reason,
                 disconn_evt->reason == WIFI_REASON_AUTH_EXPIRE ? "Auth expired" :
                 disconn_evt->reason == WIFI_REASON_AUTH_FAIL ? "Auth failed" :
                 disconn_evt->reason == WIFI_REASON_ASSOC_EXPIRE ? "Assoc expired" :
                 disconn_evt->reason == WIFI_REASON_ASSOC_FAIL ? "Assoc failed" :
                 disconn_evt->reason == WIFI_REASON_HANDSHAKE_TIMEOUT ? "Handshake timeout" :
                 disconn_evt->reason == WIFI_REASON_NO_AP_FOUND ? "AP not found" :
                 disconn_evt->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ? "4-way handshake timeout" :
                 "Unknown");
        ESP_LOGW(TAG, "Reconnecting...");

        // 清除WiFi连接事件位
        xEventGroupClearBits(g_app_event_group, EVENT_WIFI_CONNECTED);
        xEventGroupSetBits(g_app_event_group, EVENT_WIFI_DISCONNECTED);

        vTaskDelay(pdMS_TO_TICKS(1000));  // 延迟1秒再重连，避免过快重连
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "✓ WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));

        // 设置WiFi连接事件位（供main_control_task使用）
        xEventGroupClearBits(g_app_event_group, EVENT_WIFI_DISCONNECTED);
        xEventGroupSetBits(g_app_event_group, EVENT_WIFI_CONNECTED);
    }
}

static void init_wifi_with_flag(bool force_provisioning) {
#if HITONY_USE_HARDCODED_WIFI
    // ========================================================================
    // 硬编码WiFi模式（快速测试，节省RAM - 不启动AP/HTTP/DNS）
    // ========================================================================
    ESP_LOGI(TAG, "=== WiFi Init (Hardcoded Mode - RAM Optimized) ===");

    ESP_LOGI(TAG, "[1/6] Initializing netif...");
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_LOGI(TAG, "[2/6] Creating event loop...");
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "[3/6] Creating WiFi STA interface...");
    esp_netif_create_default_wifi_sta();

    // 最小WiFi配置（节省RAM）
    ESP_LOGI(TAG, "[4/6] Initializing WiFi driver (minimal buffers)...");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 2;
    cfg.dynamic_rx_buf_num = 6;
    cfg.tx_buf_type = 0;
    cfg.static_tx_buf_num = 3;
    cfg.cache_tx_buf_num = 6;
    cfg.rx_mgmt_buf_num = 3;
    cfg.ampdu_rx_enable = 0;
    cfg.ampdu_tx_enable = 0;

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册事件处理器
    ESP_LOGI(TAG, "[5/6] Registering event handlers...");
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, nullptr));

    // 配置WiFi连接
    ESP_LOGI(TAG, "[6/6] Connecting to: %s", HITONY_WIFI_SSID);
    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, HITONY_WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, HITONY_WIFI_PASSWORD);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "✓ WiFi starting (hardcoded credentials)");

#else
    // ========================================================================
    // 配网模式（需要更多RAM - AP/HTTP/DNS服务器）
    // ========================================================================
    ESP_LOGI(TAG, "=== WiFi Initialization with Provisioning ===");

    ESP_LOGI(TAG, "Initializing netif...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 2;
    cfg.dynamic_rx_buf_num = 6;
    cfg.tx_buf_type = 0;
    cfg.static_tx_buf_num = 3;
    cfg.cache_tx_buf_num = 6;
    cfg.rx_mgmt_buf_num = 3;
    cfg.ampdu_rx_enable = 0;
    cfg.ampdu_tx_enable = 0;

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(wifi_provisioning_init());

    if (force_provisioning) {
        // 用户触摸触发，强制进入配网模式
        ESP_LOGI(TAG, "🔧 Force provisioning mode (user requested)");

        // === 关键调试点：配网启动前内存检查 ===
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "💾 Memory BEFORE provisioning:");
        ESP_LOGI(TAG, "   Internal RAM: %u bytes free (largest block: %u)",
                 free_internal, largest_block);
        ESP_LOGI(TAG, "   PSRAM: %u bytes free", free_psram);

        wifi_provisioning_start(nullptr, nullptr);
        ESP_LOGI(TAG, "AP provisioning mode started (touch triggered)");
    } else if (wifi_provisioning_is_configured()) {
        // 使用保存的配置
        char ssid[33] = {0};
        char password[65] = {0};
        nvs_handle_t nvs_handle;

        if (nvs_open("wifi_config", NVS_READONLY, &nvs_handle) == ESP_OK) {
            size_t ssid_len = sizeof(ssid);
            size_t pass_len = sizeof(password);
            nvs_get_str(nvs_handle, "ssid", ssid, &ssid_len);
            nvs_get_str(nvs_handle, "password", password, &pass_len);
            nvs_close(nvs_handle);

            wifi_config_t wifi_config = {};
            strcpy((char*)wifi_config.sta.ssid, ssid);
            strcpy((char*)wifi_config.sta.password, password);
            wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
            wifi_config.sta.pmf_cfg.capable = true;
            wifi_config.sta.pmf_cfg.required = false;

            ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       &wifi_event_handler, nullptr));
            ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       &wifi_event_handler, nullptr));

            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_start());

            ESP_LOGI(TAG, "✓ Connecting to: %s", ssid);
        }
    } else {
        wifi_provisioning_start(nullptr, nullptr);
        ESP_LOGI(TAG, "AP provisioning mode started");
    }
#endif
}

// ============================================================================
// 主函数
// ============================================================================

extern "C" void app_main() {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  HiTony Smart Speaker - NEW ARCH    ║");
    ESP_LOGI(TAG, "║  ESP32-S3 Dual Core Architecture     ║");
    ESP_LOGI(TAG, "║  Version: 2.0.0                      ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════╝");
    ESP_LOGI(TAG, "");

    // ========================================================================
    // Phase 1: 基础初始化
    // ========================================================================
    ESP_LOGI(TAG, "[Phase 1] Basic Initialization...");

    init_nvs();
    init_gpio();

    // 打印系统信息
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip: %s, Cores: %d, Revision: %d",
             CONFIG_IDF_TARGET,
             chip_info.cores,
             chip_info.revision);

    ESP_LOGI(TAG, "Free heap: %lu bytes, PSRAM: %lu bytes",
             esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // ========================================================================
    // Phase 1.5: 初始化LVGL UI和触摸（必须在检测触摸之前）
    // ========================================================================
    ESP_LOGI(TAG, "[Phase 1.5] Initializing LVGL UI...");
    lvgl_ui_init();
    ESP_LOGI(TAG, "LVGL UI initialized");

    // 初始化I2C总线（触摸传感器需要，轻量级初始化不含I2S）
    ESP_LOGI(TAG, "Initializing I2C bus for touch sensor...");
    AudioI2S& audio_i2s = AudioI2S::instance();
    audio_i2s.init_i2c_only();  // 只初始化I2C，不初始化I2S（节省16KB DMA内存）
    ESP_LOGI(TAG, "I2C bus initialized (lightweight mode)");

    // 初始化触摸传感器（配网检测必需）
    ESP_LOGI(TAG, "Initializing touch sensor...");
    lvgl_ui_init_touch(audio_i2s.i2c_bus());
    ESP_LOGI(TAG, "Touch sensor initialized");

    // ========================================================================
    // Phase 2: 检测配网模式（优先检测，决定后续初始化）
    // ========================================================================
    ESP_LOGI(TAG, "[Phase 2] Checking for provisioning trigger...");
    lvgl_ui_set_status("Touch screen to configure WiFi (5s)");
    lvgl_ui_set_debug_info("Touch to enter WiFi setup...");

    bool force_provisioning = lvgl_ui_wait_for_touch(5000);  // Wait 5 seconds

    if (force_provisioning) {
        ESP_LOGI(TAG, "✅ User requested WiFi configuration - PROVISIONING MODE");
        lvgl_ui_set_status("Entering WiFi setup mode...");
    } else {
        ESP_LOGI(TAG, "No touch detected, proceeding with normal startup");
        lvgl_ui_set_status("Starting...");
        lvgl_ui_set_debug_info("Initializing...");
    }

    // ========================================================================
    // Phase 3: 创建全局队列（配网模式跳过）
    // ========================================================================
    if (!force_provisioning) {
        ESP_LOGI(TAG, "[Phase 3] Creating global queues...");

        if (!init_global_queues()) {
            ESP_LOGE(TAG, "Failed to initialize queues!");
            return;
        }
    } else {
        ESP_LOGI(TAG, "[Phase 3] Skipping queues (provisioning mode)");
    }

    // ========================================================================
    // Phase 3.5: 完整I2S初始化（仅正常模式需要，配网模式已在Phase 1.5初始化I2C）
    // ========================================================================
    if (!force_provisioning) {
        ESP_LOGI(TAG, "[Phase 3.5] Initializing full I2S (normal mode)...");

        // 注意：I2C总线已在Phase 1.5初始化，这里只需初始化I2S部分
        // 但由于AudioI2S::init()会检查i2c_bus_是否已存在，所以直接调用init()即可
        if (!audio_i2s.init()) {
            ESP_LOGE(TAG, "Failed to initialize I2S!");
            return;
        }
        ESP_LOGI(TAG, "✓ Full I2S initialized (I2C + I2S + Codec)");
    } else {
        ESP_LOGI(TAG, "[Phase 3.5] Skipping I2S init (provisioning mode, I2C-only)");
    }

    // ========================================================================
    // Phase 4: 创建任务（配网模式跳过以节省内存）
    // ========================================================================
    if (!force_provisioning) {
        ESP_LOGI(TAG, "[Phase 4] NORMAL MODE - creating all tasks...");

        // 初始化任务管理器
        TaskManager::instance().init();

        // 创建所有应用任务
        if (!create_all_tasks()) {
            ESP_LOGE(TAG, "Failed to create tasks!");
            return;
        }

        // NOTE: AFE由audio_main_task内部初始化，不需要全局初始化
        ESP_LOGI(TAG, "All application tasks created");
    } else {
        ESP_LOGI(TAG, "[Phase 4] PROVISIONING MODE - skipping all tasks");
        ESP_LOGI(TAG, "⚡ Memory savings: ~70KB (49KB audio task + queues + buffers)");
    }

    // ========================================================================
    // Phase 5: 初始化WiFi（参考xiaozhi，任务创建之后）
    // ========================================================================
    ESP_LOGI(TAG, "[Phase 5] Initializing WiFi...");
    init_wifi_with_flag(force_provisioning);
    ESP_LOGI(TAG, "WiFi initialized");

    // ========================================================================
    // Phase 6: 启动系统监控和LED控制
    // ========================================================================
    ESP_LOGI(TAG, "[Phase 6] Starting system enhancements...");

    // 初始化并启动LED控制器
    if (!LedController::instance().init(HITONY_LED_G)) {
        ESP_LOGW(TAG, "Failed to initialize LED controller");
    } else {
        if (!LedController::instance().start()) {
            ESP_LOGW(TAG, "Failed to start LED controller");
        } else {
            // 设置启动状态
            LedController::instance().set_system_state(LedController::SystemState::BOOTING);
            ESP_LOGI(TAG, "LED controller started");
        }
    }

    // 初始化并启动系统监控
    if (!SystemMonitor::instance().init()) {
        ESP_LOGW(TAG, "Failed to initialize system monitor");
    } else {
        if (!SystemMonitor::instance().start()) {
            ESP_LOGW(TAG, "Failed to start system monitor");
        } else {
            ESP_LOGI(TAG, "System monitor started");
        }
    }

    // ========================================================================
    // Phase 7: 启动完成
    // ========================================================================
    ESP_LOGI(TAG, "[Phase 7] Startup complete!");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "System Status:");
    ESP_LOGI(TAG, "- Audio I2S: OK");
    ESP_LOGI(TAG, "- LVGL UI: OK");
    ESP_LOGI(TAG, "- LED Controller: OK");
    ESP_LOGI(TAG, "- System Monitor: OK");
    ESP_LOGI(TAG, "- Task Count: %d", uxTaskGetNumberOfTasks());
    ESP_LOGI(TAG, "- Free Heap: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "- Free PSRAM: %lu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "HiTony is ready! 🎤");
    ESP_LOGI(TAG, "");

    // 主任务完成，删除自己
    // 后续所有工作由各个子任务处理
    vTaskDelete(NULL);
}
