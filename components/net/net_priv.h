#pragma once
#include "net.h"

/* captive_dns.c */
esp_err_t captive_dns_start(uint32_t ip_addr);
void captive_dns_stop(void);

/* portal.c */
esp_err_t portal_start(void);           /* idempotent */
httpd_handle_t portal_handle(void);

/* wifi.c, used by the portal */
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
    bool band_5g;
} net_scan_entry_t;

int net_scan(net_scan_entry_t *out, int max);
bool net_hotspot_open(void);            /* the setup hotspot has no password (headless) */
esp_err_t net_try_credentials(const char *ssid, const char *pass);

/* ota.c */
void ota_init(void);                    /* call once at start-up */
void ota_confirm(const char *why);      /* the running firmware works: cancel the rollback */
esp_err_t ota_register(httpd_handle_t server);
void net_restart_soon(void);                    /* restart in a second, after the reply has gone out */
