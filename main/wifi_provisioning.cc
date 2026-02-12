/**
 * @file wifi_provisioning.cc
 * @brief WiFi配网模块实现
 */

#include "wifi_provisioning.h"
#include "dns_server.h"  // 使用xiaozhi已有的DNS服务器
#include <string.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_http_server.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <lwip/ip_addr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <cJSON.h>

static const char* TAG = "wifi_prov";

// NVS配置键
#define NVS_NAMESPACE "wifi_config"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"
#define NVS_KEY_CONFIGURED "configured"

// AP配置
#define DEFAULT_AP_SSID_PREFIX "HiTony-"
#define DEFAULT_AP_PASSWORD ""  // 开放模式，无密码
#define AP_CHANNEL 1
#define AP_MAX_CONNECTIONS 4

// 全局状态
static wifi_prov_state_t g_prov_state = PROV_STATE_IDLE;
static httpd_handle_t g_http_server = nullptr;
static wifi_prov_event_cb_t g_event_callback = nullptr;
static void* g_callback_user_data = nullptr;
static wifi_ap_record_t* g_scan_results = nullptr;
static uint16_t g_scan_count = 0;
static EventGroupHandle_t g_scan_event_group = nullptr;
#define SCAN_DONE_BIT BIT0

// 状态更新
static void update_state(wifi_prov_state_t new_state) {
    g_prov_state = new_state;
    if (g_event_callback) {
        g_event_callback(new_state, g_callback_user_data);
    }
}

// WiFi事件处理器
static void wifi_prov_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP started");
                update_state(PROV_STATE_AP_STARTED);
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*)event_data;
                ESP_LOGI(TAG, "Station connected: " MACSTR, MAC2STR(event->mac));
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*)event_data;
                ESP_LOGI(TAG, "Station disconnected: " MACSTR, MAC2STR(event->mac));
                break;
            }

            case WIFI_EVENT_SCAN_DONE:
                ESP_LOGI(TAG, "WiFi scan done");
                if (g_scan_event_group) {
                    xEventGroupSetBits(g_scan_event_group, SCAN_DONE_BIT);
                }
                break;

            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started, connecting...");
                update_state(PROV_STATE_CONNECTING);
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "STA disconnected");
                update_state(PROV_STATE_FAILED);
                break;

            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "✓ Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        update_state(PROV_STATE_CONNECTED);
    }
}

// ============================================================================
// NVS配置操作
// ============================================================================

bool wifi_provisioning_is_configured(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }

    uint8_t configured = 0;
    err = nvs_get_u8(nvs_handle, NVS_KEY_CONFIGURED, &configured);
    nvs_close(nvs_handle);

    return (err == ESP_OK && configured == 1);
}

static esp_err_t save_wifi_config(const char* ssid, const char* password) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    // 保存SSID和密码
    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, NVS_KEY_CONFIGURED, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi config saved: %s", ssid);
    } else {
        ESP_LOGE(TAG, "Failed to save WiFi config: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t wifi_provisioning_clear_config(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_erase_all(nvs_handle);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "WiFi config cleared");
    return ESP_OK;
}

// ============================================================================
// HTTP Server处理器
// ============================================================================

// Ultra-Lightweight WiFi Setup Page - Mobile Optimized (No complex JS)
static const char* HTML_PAGE = R"html(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>WiFi Setup</title>
<style>
*{box-sizing:border-box}
body{margin:0;padding:15px;font-family:Arial,sans-serif;background:#f0f0f0;font-size:16px}
.box{background:#fff;padding:20px;border-radius:8px;max-width:500px;margin:0 auto}
h1{margin:0 0 5px;font-size:22px;color:#333}
p{margin:0 0 20px;color:#666;font-size:14px}
label{display:block;margin:15px 0 5px;font-weight:bold;color:#333}
select,input,button{width:100%;padding:12px;font-size:16px;border:2px solid #ddd;border-radius:4px;margin:0}
select:focus,input:focus{border-color:#007bff;outline:none}
button{background:#007bff;color:#fff;border:none;font-weight:bold;margin-top:20px;cursor:pointer}
button:active{background:#0056b3}
.hint{font-size:13px;color:#888;margin-top:3px}
.msg{margin-top:15px;padding:12px;border-radius:4px;font-size:14px;display:none}
.ok{background:#d4edda;color:#155724}
.err{background:#f8d7da;color:#721c24}
.refresh{background:#6c757d;margin-top:10px;padding:10px;font-size:14px}
.refresh:active{background:#5a6268}
</style>
</head>
<body>
<div class="box">
<h1>WiFi Setup</h1>
<p>Connect HiTony to your WiFi network</p>

<label>1. Select Network (or enter manually below):</label>
<select id="net">
<option value="">-- Scanning networks... --</option>
</select>
<button class="refresh" onclick="doScan()">Refresh List</button>

<label>2. Or Enter Network Name Manually:</label>
<input type="text" id="ssid" placeholder="WiFi name (SSID)">
<div class="hint">Leave blank to use selected network above</div>

<label>3. Password (optional for open networks):</label>
<input type="password" id="pass" placeholder="WiFi password">

<button onclick="doConnect()">Connect</button>

<div id="msg" class="msg"></div>
</div>

<script>
function msg(t,ok){var m=document.getElementById('msg');m.innerText=t;m.className='msg '+(ok?'ok':'err');m.style.display='block'}

function doScan(){
msg('Scanning...',1);
var x=new XMLHttpRequest();
x.onload=function(){
if(x.status==200){
try{
var d=JSON.parse(x.responseText);
var s=document.getElementById('net');
s.innerHTML='';
if(!d.networks||d.networks.length==0){
s.innerHTML='<option value="">-- No networks found --</option>';
msg('No networks found. Enter SSID manually.',0);
return;
}
var o=document.createElement('option');
o.value='';
o.innerText='-- Select a network --';
s.appendChild(o);
for(var i=0;i<d.networks.length;i++){
var w=d.networks[i];
var opt=document.createElement('option');
opt.value=w.ssid;
opt.innerText=w.ssid+' ('+(w.authmode>0?'Locked':'Open')+', '+w.rssi+'dBm)';
s.appendChild(opt);
}
document.getElementById('msg').style.display='none';
}catch(e){msg('Scan error: '+e.message,0)}
}else{msg('Scan failed',0)}
};
x.onerror=function(){msg('Network error',0)};
x.open('GET','/scan',true);
x.send();
}

function doConnect(){
var sel=document.getElementById('net').value;
var man=document.getElementById('ssid').value;
var pwd=document.getElementById('pass').value;
var ssid=man||sel;

if(!ssid){
msg('Please select or enter a network',0);
return;
}

msg('Connecting to '+ssid+'...',1);

var x=new XMLHttpRequest();
x.onload=function(){
if(x.status==200){
try{
var d=JSON.parse(x.responseText);
if(d.success){
msg('Success! Restarting in 3 seconds...',1);
setTimeout(function(){msg('Restarting...',1)},2500);
}else{
msg('Failed: '+(d.message||'Unknown error'),0);
}
}catch(e){msg('Error: '+e.message,0)}
}else{msg('Connection failed',0)}
};
x.onerror=function(){msg('Network error',0)};
x.open('POST','/connect',true);
x.setRequestHeader('Content-Type','application/json');
x.send(JSON.stringify({ssid:ssid,password:pwd}));
}

window.onload=function(){setTimeout(doScan,500)};
</script>
</body>
</html>
)html";

// Captive Portal检测处理器 - 直接返回配网页面（比302重定向更可靠）
// iOS: 响应非"Success"内容 → CNA自动弹出显示此页面
// Android: 响应非204 → 显示"登录到WiFi网络"通知
static esp_err_t http_captive_handler(httpd_req_t* req) {
    ESP_LOGI(TAG, "Captive portal: %s", req->uri);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, HTML_PAGE, strlen(HTML_PAGE));
    return ESP_OK;
}

// GET / - 返回配网页面
static esp_err_t http_get_root_handler(httpd_req_t* req) {
    // 禁用缓存，确保每次都重新加载
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, HTML_PAGE, strlen(HTML_PAGE));
    return ESP_OK;
}

// GET /scan - 扫描WiFi网络（优化内存使用）
static esp_err_t http_get_scan_handler(httpd_req_t* req) {
    ESP_LOGI(TAG, "WiFi scan...");

    // 临时切换到APSTA模式以支持扫描（纯AP模式无法扫描）
    wifi_mode_t current_mode;
    esp_wifi_get_mode(&current_mode);
    if (current_mode == WIFI_MODE_AP) {
        ESP_LOGI(TAG, "Switching to APSTA mode for scanning...");
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        vTaskDelay(pdMS_TO_TICKS(100));  // 等待模式切换完成
    }

    // 启动扫描
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;  // 不显示隐藏网络，节省内存
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        // 切回AP模式
        if (current_mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 限制最多10个网络，节省内存
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 10) count = 10;

    if (g_scan_results) {
        free(g_scan_results);
        g_scan_results = nullptr;
    }

    wifi_ap_record_t* results = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * count);
    if (!results) {
        ESP_LOGE(TAG, "Scan malloc failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_wifi_scan_get_ap_records(&count, results);
    ESP_LOGI(TAG, "Scan: %d networks", count);

    // 手动构建JSON，避免cJSON大内存分配
    char json[1024];
    int len = snprintf(json, sizeof(json), "{\"networks\":[");

    for (int i = 0; i < count && len < 900; i++) {
        if (i > 0) len += snprintf(json + len, sizeof(json) - len, ",");
        len += snprintf(json + len, sizeof(json) - len,
            "{\"ssid\":\"%s\",\"rssi\":%d,\"authmode\":%d}",
            results[i].ssid, results[i].rssi, results[i].authmode);
    }

    snprintf(json + len, sizeof(json) - len, "]}");

    free(results);

    // 切回AP模式节省内存
    if (current_mode == WIFI_MODE_AP) {
        ESP_LOGI(TAG, "Switching back to AP mode");
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

// POST /connect - 连接WiFi（优化内存，避免cJSON）
static esp_err_t http_post_connect_handler(httpd_req_t* req) {
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    content[ret] = '\0';

    // 简单JSON解析，避免cJSON内存分配
    char ssid[33] = {0};
    char password[64] = {0};

    // 查找 "ssid":"..."
    char* ssid_start = strstr(content, "\"ssid\":\"");
    if (ssid_start) {
        ssid_start += 8;  // 跳过 "ssid":"
        char* ssid_end = strchr(ssid_start, '"');
        if (ssid_end) {
            int len = ssid_end - ssid_start;
            if (len > 0 && len < 33) {
                strncpy(ssid, ssid_start, len);
            }
        }
    }

    // 查找 "password":"..."
    char* pwd_start = strstr(content, "\"password\":\"");
    if (pwd_start) {
        pwd_start += 12;  // 跳过 "password":"
        char* pwd_end = strchr(pwd_start, '"');
        if (pwd_end) {
            int len = pwd_end - pwd_start;
            if (len > 0 && len < 64) {
                strncpy(password, pwd_start, len);
            }
        }
    }

    if (strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Invalid SSID");
        const char* err_json = "{\"success\":false,\"message\":\"Invalid SSID\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, err_json, HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WiFi config: SSID=%s", ssid);

    // 保存配置到NVS
    esp_err_t err = save_wifi_config(ssid, password);

    // 手动构建JSON响应
    const char* json_resp = (err == ESP_OK)
        ? "{\"success\":true,\"message\":\"Saved\"}"
        : "{\"success\":false,\"message\":\"Save failed\"}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_resp, HTTPD_RESP_USE_STRLEN);

    // 保存成功，3秒后重启（给前端足够时间显示消息）
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✅ WiFi config saved successfully! Restarting in 3s...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    return ESP_OK;
}

// HTTP Server启动
static esp_err_t start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;       // 减小到6KB（原8KB）
    config.max_uri_handlers = 16;   // 10 captive + /scan + /connect + / + /* = 14
    config.lru_purge_enable = true;
    config.max_open_sockets = 2;    // 需要>=2：captive portal检测可能并发请求
    config.backlog_conn = 2;        // 匹配max_open_sockets
    config.ctrl_port = 32768;       // 使用自定义控制端口减少冲突

    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "💾 Before httpd_start: Internal=%u", free_before);

    ESP_LOGI(TAG, "Starting HTTP Server...");

    if (httpd_start(&g_http_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        size_t free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGE(TAG, "💾 httpd_start FAILED: Internal=%u (used %d bytes)",
                 free_after, free_before - free_after);
        return ESP_FAIL;
    }

    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "💾 After httpd_start: Internal=%u (used %d bytes)",
             free_after, free_before - free_after);

    // 注册处理器

    // Captive Portal检测端点（iOS + Android + Windows，参考xiaozhi esp-wifi-connect）
    const char* captive_uris[] = {
        "/generate_204",                 // Android主要端点
        "/gen_204",                      // Android简写
        "/hotspot-detect.html",          // iOS主要端点
        "/library/test/success.html",    // iOS备用端点
        "/ncsi.txt",                     // Windows
        "/mobile/status.php",            // Android部分版本
        "/check_network_status.txt",     // Android (xiaozhi)
        "/connectivity-check.html",      // Android (xiaozhi)
        "/fwlink/",                      // Microsoft
        "/success.txt",                  // Firefox
    };

    for (const char* uri_path : captive_uris) {
        httpd_uri_t uri = {
            .uri = uri_path,
            .method = HTTP_GET,
            .handler = http_captive_handler,
            .user_ctx = nullptr
        };
        httpd_register_uri_handler(g_http_server, &uri);
    }

    // 配网功能API
    httpd_uri_t uri_scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = http_get_scan_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(g_http_server, &uri_scan);

    httpd_uri_t uri_connect = {
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = http_post_connect_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(g_http_server, &uri_connect);

    // 配网页面
    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = http_get_root_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(g_http_server, &uri_root);

    // Captive Portal - 通配符处理器，捕获所有其他请求并重定向到根页面
    httpd_uri_t uri_wildcard = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = http_get_root_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(g_http_server, &uri_wildcard);

    ESP_LOGI(TAG, "✓ HTTP Server started on http://192.168.4.1");
    return ESP_OK;
}

// ============================================================================
// 公共API
// ============================================================================

esp_err_t wifi_provisioning_init(void) {
    ESP_LOGI(TAG, "Initializing WiFi provisioning...");

    // 创建事件组
    g_scan_event_group = xEventGroupCreate();

    // 注册WiFi事件处理器
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_prov_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_prov_event_handler, nullptr));

    return ESP_OK;
}

esp_err_t wifi_provisioning_start(const char* ap_ssid, const char* ap_password) {
    char ssid[33];

    ESP_LOGI(TAG, "🔧 Starting WiFi provisioning (lightweight mode)...");

    // === 内存监控：启动前 ===
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "💾 Heap BEFORE prov: Internal=%u, PSRAM=%u", free_internal, free_psram);

    // 如果没有提供SSID，生成默认SSID（使用MAC地址后4位）
    if (!ap_ssid) {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_AP, mac);
        snprintf(ssid, sizeof(ssid), "%s%02X%02X", DEFAULT_AP_SSID_PREFIX, mac[4], mac[5]);
        ap_ssid = ssid;
    }

    const char* password = ap_password ? ap_password : DEFAULT_AP_PASSWORD;

    ESP_LOGI(TAG, "Starting AP mode: SSID=%s", ap_ssid);

    // 配置AP
    wifi_config_t wifi_config = {};
    strlcpy((char*)wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(ap_ssid);
    strlcpy((char*)wifi_config.ap.password, password, sizeof(wifi_config.ap.password));
    wifi_config.ap.channel = AP_CHANNEL;
    wifi_config.ap.max_connection = 1;  // 减少到1个连接（只需1台手机）
    wifi_config.ap.authmode = (strlen(password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    // === Checkpoint 1: WiFi模式切换 ===
    ESP_LOGI(TAG, "[Step 1] Setting WiFi mode to AP...");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "💾 After set_mode: Internal=%u", free_internal);

    // === Checkpoint 2: WiFi配置 ===
    ESP_LOGI(TAG, "[Step 2] Configuring WiFi AP...");
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "💾 After config: Internal=%u", free_internal);

    // === Checkpoint 3: WiFi启动 ===
    ESP_LOGI(TAG, "[Step 3] Starting WiFi AP...");
    ESP_ERROR_CHECK(esp_wifi_start());
    free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "💾 After wifi_start: Internal=%u", free_internal);

    // === Checkpoint 3.5: 配置AP网络参数（参考xiaozhi esp-wifi-connect组件） ===
    // Captive Portal必需：DHCP必须通告DNS服务器，否则手机无法检测到captive portal
    ESP_LOGI(TAG, "[Step 3.5] Configuring AP network for captive portal...");
    {
        esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap_netif) {
            // 停止DHCP服务器以修改配置
            esp_netif_dhcps_stop(ap_netif);

            // 1. 显式设置AP的IP/GW/Netmask（参考xiaozhi: wifi_configuration_ap.cc）
            esp_netif_ip_info_t ip_info = {};
            IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
            IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
            IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
            ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));

            // 2. 设置DNS服务器为AP自身IP
            esp_netif_dns_info_t dns = {};
            IP4_ADDR(&dns.ip.u_addr.ip4, 192, 168, 4, 1);
            dns.ip.type = IPADDR_TYPE_V4;
            ESP_ERROR_CHECK(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns));

            // 3. 启用DHCP DNS选项（确保DHCP响应中包含DNS服务器地址）
            uint8_t dns_offer = 1;
            ESP_ERROR_CHECK(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                            ESP_NETIF_DOMAIN_NAME_SERVER, &dns_offer, sizeof(dns_offer)));

            // 重启DHCP服务器（包含IP+DNS配置）
            ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));
            ESP_LOGI(TAG, "AP network configured: IP=192.168.4.1, DNS=192.168.4.1, DHCP DNS offer=ON");
        } else {
            ESP_LOGW(TAG, "Failed to get AP netif handle");
        }
    }

    // === Checkpoint 4: HTTP Server启动 ===
    ESP_LOGI(TAG, "[Step 4] Starting HTTP Server...");
    start_http_server();
    free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "💾 After HTTP server: Internal=%u", free_internal);

    // === Checkpoint 5: DNS Server启动 ===
    ESP_LOGI(TAG, "[Step 5] Starting DNS Server...");
    dns_server_start(0xC0A80401);  // 192.168.4.1
    free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "💾 After DNS server: Internal=%u", free_internal);
    ESP_LOGI(TAG, "✓ DNS Server started for Captive Portal");

    ESP_LOGI(TAG, "✓ AP started! Connect to: %s", ap_ssid);
    if (strlen(password) > 0) {
        ESP_LOGI(TAG, "✓ Password: %s", password);
    } else {
        ESP_LOGI(TAG, "✓ Open network (no password)");
    }
    ESP_LOGI(TAG, "✓ Captive Portal: Connect and browser will auto-redirect");

    // === 最终内存统计 ===
    free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "💾 Heap AFTER prov: Internal=%u, PSRAM=%u", free_internal, free_psram);

    return ESP_OK;
}

esp_err_t wifi_provisioning_stop(void) {
    ESP_LOGI(TAG, "Stopping provisioning...");

    // 停止DNS服务器（使用xiaozhi的dns_server模块）
    dns_server_stop();

    // 停止HTTP Server
    if (g_http_server) {
        httpd_stop(g_http_server);
        g_http_server = nullptr;
    }

    // 清理扫描结果
    if (g_scan_results) {
        free(g_scan_results);
        g_scan_results = nullptr;
        g_scan_count = 0;
    }

    update_state(PROV_STATE_IDLE);
    return ESP_OK;
}

wifi_prov_state_t wifi_provisioning_get_state(void) {
    return g_prov_state;
}

void wifi_provisioning_register_callback(wifi_prov_event_cb_t cb, void* user_data) {
    g_event_callback = cb;
    g_callback_user_data = user_data;
}
