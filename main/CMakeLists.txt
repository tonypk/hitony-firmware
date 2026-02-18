idf_component_register(
    SRCS
        "main.cc"
        "audio_i2s.cc"
        "dns_server.cc"
        "lvgl_ui.cc"
        "opus_decoder.cc"
        "opus_encoder.cc"
        "advanced_afe.cc"
        # 新架构文件
        "app_queues.cc"
        "task_manager.cc"
        "app_init.cc"
        # 2任务架构
        "audio_main_task.cc"
        "main_control_task.cc"
        # 增强功能模块
        "system_monitor.cc"
        "led_controller.cc"
        "wifi_provisioning.cc"
        "ota_update.cc"
    INCLUDE_DIRS "."
    REQUIRES
        esp_websocket_client
        esp_wifi
        esp_http_client
        app_update
        esp_netif
        nvs_flash
        cjson
        esp-sr
        esp_codec_dev
        esp_driver_i2s
        esp_driver_i2c
        esp_driver_gpio
        esp_http_server
        esp_lcd
        esp_audio_codec
        driver
)
