#include "dns_server.h"

#include <esp_log.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "dns_server";
static TaskHandle_t dns_task = nullptr;
static int dns_sock = -1;
static uint32_t dns_ip = 0;

static void dns_task_fn(void* arg) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    dns_sock = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (dns_sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(nullptr);
        return;
    }

    if (lwip_bind(dns_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind failed");
        lwip_close(dns_sock);
        dns_sock = -1;
        vTaskDelete(nullptr);
        return;
    }

    uint8_t buf[512];
    while (true) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        int len = lwip_recvfrom(dns_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &from_len);
        if (len < 12) continue;

        // DNS header
        uint16_t id = (buf[0] << 8) | buf[1];
        // Build response
        uint8_t resp[512];
        int rlen = 0;
        resp[rlen++] = (id >> 8) & 0xFF;
        resp[rlen++] = id & 0xFF;
        resp[rlen++] = 0x81; // standard query response, no error
        resp[rlen++] = 0x80;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01; // QDCOUNT
        resp[rlen++] = 0x00; resp[rlen++] = 0x01; // ANCOUNT
        resp[rlen++] = 0x00; resp[rlen++] = 0x00; // NSCOUNT
        resp[rlen++] = 0x00; resp[rlen++] = 0x00; // ARCOUNT

        // Copy question
        int qlen = len - 12;
        if (qlen < 4) continue;
        memcpy(resp + rlen, buf + 12, qlen);
        rlen += qlen;

        // Answer: pointer to name (0xC00C)
        resp[rlen++] = 0xC0; resp[rlen++] = 0x0C;
        resp[rlen++] = 0x00; resp[rlen++] = 0x01; // TYPE A
        resp[rlen++] = 0x00; resp[rlen++] = 0x01; // CLASS IN
        resp[rlen++] = 0x00; resp[rlen++] = 0x00; resp[rlen++] = 0x00; resp[rlen++] = 0x3C; // TTL 60
        resp[rlen++] = 0x00; resp[rlen++] = 0x04; // RDLENGTH

        uint32_t ip = dns_ip ? dns_ip : 0xC0A80401; // 192.168.4.1
        resp[rlen++] = (ip >> 24) & 0xFF;
        resp[rlen++] = (ip >> 16) & 0xFF;
        resp[rlen++] = (ip >> 8) & 0xFF;
        resp[rlen++] = ip & 0xFF;

        lwip_sendto(dns_sock, resp, rlen, 0, (sockaddr*)&from, from_len);
    }
}

void dns_server_start(uint32_t ap_ip_addr) {
    dns_ip = ap_ip_addr;
    if (!dns_task) {
        // DNS server栈大小：3KB（2KB会栈溢出）
        xTaskCreate(dns_task_fn, "dns_srv", 3072, nullptr, 5, &dns_task);
        ESP_LOGI(TAG, "DNS server started (stack: 3KB)");
    }
}

void dns_server_stop() {
    if (dns_task) {
        vTaskDelete(dns_task);
        dns_task = nullptr;
    }
    if (dns_sock >= 0) {
        lwip_close(dns_sock);
        dns_sock = -1;
    }
}
