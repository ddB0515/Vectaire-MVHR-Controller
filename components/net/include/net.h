#pragma once
/* Wi-Fi station, first-start provisioning (hotspot + captive setup page) and mDNS. */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_OFF = 0,            /* display-only mode, Wi-Fi never started */
    NET_PROVISIONING,       /* hotspot up, waiting for the setup page */
    NET_TESTING,            /* trying the credentials entered on the setup page */
    NET_CONNECTING,         /* normal operation, (re)connecting to the saved network */
    NET_CONNECTED,
} net_state_t;

typedef struct {
    net_state_t state;
    char ap_ssid[33];       /* provisioning hotspot */
    char ap_pass[17];
    char sta_ssid[33];
    char ip[16];
    int8_t rssi;
    char last_error[48];    /* why the last attempt failed, empty if none */
    bool phone_joined;      /* a client is connected to the hotspot */
    bool wifi_trial;        /* net_change_wifi() is trying the new network */
    char wifi_change[96];   /* outcome of the last net_change_wifi(), empty if none */
} net_status_t;

typedef void (*net_listener_t)(const net_status_t *st, void *arg);

/* Start according to the saved settings: connect if configured, otherwise do nothing. */
esp_err_t net_start(void);

/* Bring up the setup hotspot and portal (also used to re-configure later). */
esp_err_t net_start_provisioning(void);

/* Headless: the same hotspot without a password (nothing could show one), and the setup page shows the device
   password. It closes once nobody has been joined for close_after_s; a configured controller then restarts into
   normal operation. Calling it again while it is up only restarts the countdown. */
esp_err_t net_start_open_provisioning(uint32_t close_after_s);

/* Normal operation: switch to another network. The new one is tried first: saved once it gives us an address,
   otherwise (TRIAL_MS) the old network comes back. Starts a second later, so the HTTP reply still goes out. */
esp_err_t net_change_wifi(const char *ssid, const char *pass);

void net_get_status(net_status_t *out);
void net_set_listener(net_listener_t cb, void *arg);

/* Shared HTTP server (setup page, later diagnostics); NULL until Wi-Fi is up. */
httpd_handle_t net_http_server(void);

/* cb registers more handlers: called now if the server already runs, otherwise once it starts (first setup starts
   it only with the hotspot). */
void net_on_server_start(void (*cb)(httpd_handle_t server));

/* The page served at "/" outside provisioning (the web component sets it). */
void net_set_home_handler(esp_err_t (*handler)(httpd_req_t *req));

/* true if the X-OTA-Password header matches the device password */
bool net_password_ok(httpd_req_t *req);

/* Send a reply in small chunks: still works when memory is too fragmented for one large network buffer. */
esp_err_t net_send_chunked(httpd_req_t *req, const char *data, size_t len);

#ifdef __cplusplus
}
#endif
