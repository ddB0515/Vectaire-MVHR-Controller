/* Minimal captive-portal DNS: answers every A query with the hotspot's own address. */
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "net_priv.h"

static const char *TAG = "dns";

static TaskHandle_t s_task;
static int s_sock = -1;
static uint32_t s_ip;           /* network byte order */

static void dns_task(void *arg)
{
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            break;                      /* socket closed by captive_dns_stop() */
        }
        if (n < 12 || (buf[2] & 0x80)) {
            continue;                   /* too short, or not a query */
        }
        /* find the end of the first question: QNAME labels, then QTYPE + QCLASS */
        int p = 12;
        while (p < n && buf[p] != 0) {
            p += buf[p] + 1;
        }
        p += 5;
        if (p > n || p + 16 > (int)sizeof(buf)) {
            continue;
        }
        uint16_t qtype = (uint16_t)((buf[p - 4] << 8) | buf[p - 3]);
        buf[2] = 0x84;                  /* response, authoritative */
        buf[3] = 0x00;                  /* no error */
        buf[6] = 0;                     /* ANCOUNT */
        buf[7] = qtype == 1 ? 1 : 0;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        if (qtype == 1) {
            const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4};
            memcpy(&buf[p], answer, sizeof(answer));
            memcpy(&buf[p + sizeof(answer)], &s_ip, 4);
            p += sizeof(answer) + 4;
        }
        sendto(s_sock, buf, p, 0, (struct sockaddr *)&from, from_len);
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t captive_dns_start(uint32_t ip_addr)
{
    if (s_task) {
        return ESP_OK;
    }
    s_ip = ip_addr;
    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0) {
        return ESP_FAIL;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }
    xTaskCreate(dns_task, "dns", 3072, NULL, 3, &s_task);
    ESP_LOGI(TAG, "captive DNS started");
    return ESP_OK;
}

void captive_dns_stop(void)
{
    if (s_sock >= 0) {
        shutdown(s_sock, SHUT_RDWR);
        close(s_sock);
        s_sock = -1;
        ESP_LOGI(TAG, "captive DNS stopped");
    }
}
