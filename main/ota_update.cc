/**
 * @file ota_update.cc
 * @brief OTA firmware update — download via HTTP, flash to inactive partition, reboot.
 */

#include "ota_update.h"
#include "config.h"
#include "lvgl_ui.h"

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_http_client.h>
#include <esp_websocket_client.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

// External WebSocket client from main_control_task — stop before OTA download
// to free WiFi buffers (ESP32 minimal config can't handle concurrent connections)
extern esp_websocket_client_handle_t g_ws_client;
extern bool g_ws_connected;

static const char* TAG = "ota";

static bool s_ota_running = false;
static char s_ota_url[256] = {0};

// Download buffer — allocated in PSRAM
#define OTA_BUF_SIZE 8192


static void ota_task(void* arg) {
    ESP_LOGI(TAG, "OTA update starting: %s", s_ota_url);
    lvgl_ui_set_status("Updating...");

    // Stop WebSocket to free WiFi buffers for HTTP download
    // ESP32 with minimal WiFi config can't handle concurrent TCP connections reliably
    if (g_ws_client) {
        ESP_LOGI(TAG, "Stopping WebSocket for OTA download...");
        esp_websocket_client_close(g_ws_client, pdMS_TO_TICKS(2000));
        g_ws_connected = false;
        vTaskDelay(pdMS_TO_TICKS(500));  // Let WiFi stack settle
        ESP_LOGI(TAG, "WebSocket stopped, proceeding with download");
    }

    esp_err_t err;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);

    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition available");
        lvgl_ui_set_status("No partition");
        goto fail;
    }

    ESP_LOGI(TAG, "Writing to partition: %s (offset 0x%lx, size 0x%lx)",
             update_partition->label,
             (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    // Configure HTTP client
    {
        esp_http_client_config_t http_config = {};
        http_config.url = s_ota_url;
        http_config.timeout_ms = 60000;  // 60s timeout for large firmware files
        http_config.buffer_size = OTA_BUF_SIZE;
        http_config.buffer_size_tx = 1024;
        http_config.keep_alive_enable = true;

        esp_http_client_handle_t client = esp_http_client_init(&http_config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            lvgl_ui_set_status("HTTP error");
            goto fail;
        }

        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            lvgl_ui_set_status("Connect failed");
            esp_http_client_cleanup(client);
            goto fail;
        }

        int content_length = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP status=%d, content_length=%d", status_code, content_length);

        if (status_code != 200) {
            ESP_LOGE(TAG, "HTTP error: status %d", status_code);
            lvgl_ui_set_status("Server error");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            goto fail;
        }

        // Begin OTA write
        err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
            lvgl_ui_set_status("Flash error");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            goto fail;
        }

        // Download and write in chunks
        char* buf = (char*)heap_caps_malloc(OTA_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!buf) {
            buf = (char*)malloc(OTA_BUF_SIZE);
        }
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate download buffer");
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            goto fail;
        }

        int total_read = 0;
        int last_progress = -1;
        int read_retries = 0;
        while (true) {
            int read_len = esp_http_client_read(client, buf, OTA_BUF_SIZE);
            if (read_len < 0) {
                read_retries++;
                ESP_LOGW(TAG, "HTTP read error (retry %d/3, downloaded %d bytes so far)", read_retries, total_read);
                if (read_retries >= 3) {
                    ESP_LOGE(TAG, "HTTP read failed after 3 retries, aborting OTA");
                    err = ESP_FAIL;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));  // Wait 1s before retry
                continue;
            }
            read_retries = 0;  // Reset retry counter on success

            if (read_len == 0) {
                // Connection closed or transfer complete
                if (esp_http_client_is_complete_data_received(client)) {
                    ESP_LOGI(TAG, "Download complete: %d bytes", total_read);
                } else {
                    ESP_LOGW(TAG, "Connection closed prematurely at %d bytes", total_read);
                    err = ESP_FAIL;
                }
                break;
            }

            err = esp_ota_write(ota_handle, buf, read_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                break;
            }

            total_read += read_len;

            // Update progress on LCD (every 5%)
            if (content_length > 0) {
                int progress = (total_read * 100) / content_length;
                if (progress != last_progress && progress % 5 == 0) {
                    last_progress = progress;
                    char status[32];
                    snprintf(status, sizeof(status), "%d%%", progress);
                    lvgl_ui_set_status(status);
                    ESP_LOGI(TAG, "OTA progress: %d%% (%d/%d)", progress, total_read, content_length);
                }
            }

            // Yield to other tasks periodically
            if (total_read % (OTA_BUF_SIZE * 8) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            lvgl_ui_set_status("Write error");
            goto fail;
        }

        // Finalize OTA
        err = esp_ota_end(ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
                ESP_LOGE(TAG, "Image validation failed — corrupt download?");
            }
            lvgl_ui_set_status("Verify failed");
            goto fail;
        }

        // Set boot partition
        err = esp_ota_set_boot_partition(update_partition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
            lvgl_ui_set_status("Boot failed");
            goto fail;
        }

        ESP_LOGI(TAG, "OTA update successful! Firmware size: %d bytes. Rebooting in 2s...", total_read);
        lvgl_ui_set_status("Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

fail:
    s_ota_running = false;
    vTaskDelay(pdMS_TO_TICKS(5000));
    lvgl_ui_set_status("Ready");
    vTaskDelete(NULL);
}


bool ota_start_update(const char* url) {
    if (s_ota_running) {
        ESP_LOGW(TAG, "OTA already in progress");
        return false;
    }

    if (!url || strlen(url) >= sizeof(s_ota_url)) {
        ESP_LOGE(TAG, "Invalid OTA URL");
        return false;
    }

    strncpy(s_ota_url, url, sizeof(s_ota_url) - 1);
    s_ota_url[sizeof(s_ota_url) - 1] = '\0';
    s_ota_running = true;

    // Create OTA task with 8KB stack (HTTP client needs stack space)
    BaseType_t ret = xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        s_ota_running = false;
        return false;
    }

    return true;
}


bool ota_is_running(void) {
    return s_ota_running;
}
