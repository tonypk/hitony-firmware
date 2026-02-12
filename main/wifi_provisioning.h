/**
 * @file wifi_provisioning.h
 * @brief WiFi配网模块 - 支持AP模式热点配网
 *
 * 功能：
 * 1. 启动AP模式，设备作为热点
 * 2. 提供HTTP Server，扫描周边WiFi
 * 3. 接收用户选择的SSID和密码
 * 4. 保存配置到NVS并连接
 */

#pragma once

#include <esp_err.h>
#include <esp_wifi.h>
#include <esp_event.h>

#ifdef __cplusplus
extern "C" {
#endif

// 配网状态
typedef enum {
    PROV_STATE_IDLE = 0,           // 空闲
    PROV_STATE_AP_STARTED,         // AP热点已启动
    PROV_STATE_SCANNING,           // 正在扫描WiFi
    PROV_STATE_CONFIGURING,        // 正在配置
    PROV_STATE_CONNECTING,         // 正在连接
    PROV_STATE_CONNECTED,          // 已连接
    PROV_STATE_FAILED              // 连接失败
} wifi_prov_state_t;

// 配网事件回调
typedef void (*wifi_prov_event_cb_t)(wifi_prov_state_t state, void* user_data);

/**
 * @brief 初始化WiFi配网系统
 * @return ESP_OK on success
 */
esp_err_t wifi_provisioning_init(void);

/**
 * @brief 启动AP模式配网
 * @param ap_ssid AP热点名称（如果为NULL，使用默认名称 "EchoEar-XXXX"）
 * @param ap_password AP密码（如果为NULL，使用默认密码或开放模式）
 * @return ESP_OK on success
 */
esp_err_t wifi_provisioning_start(const char* ap_ssid, const char* ap_password);

/**
 * @brief 停止配网模式，切换到STA模式
 * @return ESP_OK on success
 */
esp_err_t wifi_provisioning_stop(void);

/**
 * @brief 检查是否已有WiFi配置
 * @return true if WiFi credentials are saved
 */
bool wifi_provisioning_is_configured(void);

/**
 * @brief 清除保存的WiFi配置
 * @return ESP_OK on success
 */
esp_err_t wifi_provisioning_clear_config(void);

/**
 * @brief 获取当前配网状态
 * @return Current provisioning state
 */
wifi_prov_state_t wifi_provisioning_get_state(void);

/**
 * @brief 注册配网事件回调
 * @param cb Callback function
 * @param user_data User data to pass to callback
 */
void wifi_provisioning_register_callback(wifi_prov_event_cb_t cb, void* user_data);

#ifdef __cplusplus
}
#endif
