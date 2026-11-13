#pragma once
/* MVHR bus transport: UART, request/reply transactions with retries. Single-owner: call only from one task. */
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "vbus_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int uart_num;
    int tx_gpio;
    int rx_gpio;
    bool invert_tx;             /* true for an NPN open-collector driver */
    bool invert_rx;
    uint32_t reply_timeout_ms;  /* wait for the first reply byte after our frame has gone out */
    uint32_t gap_timeout_ms;    /* silence that ends a reply */
    uint8_t retries;            /* extra attempts after the first */
} vbus_config_t;

#define VBUS_CONFIG_DEFAULT() { \
    .uart_num = 1, .tx_gpio = 2, .rx_gpio = 3, .invert_tx = true, .invert_rx = false, \
    .reply_timeout_ms = 150, .gap_timeout_ms = 40, .retries = 4 }

typedef struct {
    uint32_t requests;
    uint32_t replies;
    uint32_t timeouts;          /* attempts without a valid reply */
    uint32_t failures;          /* requests that failed after all retries */
    uint32_t crc_errors;
    uint32_t framing_errors;
    uint32_t echo_frames;       /* our own frames heard back on the wire */
    uint32_t rx_bytes;          /* everything heard on the wire, valid or not */
    uint32_t idle_bytes;        /* heard while we weren't waiting for a reply (nobody else should talk) */
    uint32_t last_reply_ms;     /* time from our ETX to the reply's first byte */
} vbus_stats_t;

/* Optional tap for diagnostics: called for every frame sent (tx=true) or received. */
typedef void (*vbus_tap_cb_t)(bool tx, const uint8_t *wire, size_t len, void *arg);

/* Simulated unit: writes the reply data (VBUS_MAX_DATA bytes max) and returns its length, or -1 for no reply. */
typedef int (*vbus_responder_t)(uint8_t cmd, const uint8_t *data, uint8_t len, uint8_t *reply, void *arg);

esp_err_t vbus_init(const vbus_config_t *cfg);

/*
 * Instead of vbus_init(): answer requests with a simulated unit, keeping the bus timing. The UART and the
 * bus pins are never touched, so nothing can reach a real unit.
 */
esp_err_t vbus_init_simulated(const vbus_config_t *cfg, vbus_responder_t responder, void *arg);

/*
 * Send a request and wait for the matching reply (addr 0xD1, same cmd).
 * ESP_OK with *reply filled, ESP_ERR_TIMEOUT if all attempts failed.
 */
esp_err_t vbus_transact(uint8_t cmd, const uint8_t *data, uint8_t len, vbus_frame_t *reply);

void vbus_get_stats(vbus_stats_t *out);

/* Percentage of 20 samples (1 ms apart, weak pull-down) with the RX pin high: idle bus 100, open wire 0. -1 if simulated. */
int vbus_rx_high_pct(void);

/* Read the bus for ms while idle (between polls), so stray traffic shows up in the stats and the trace. */
void vbus_listen(uint32_t ms);

/* Bus trace (from boot until full) as text, and starting it again. */
esp_err_t vbus_send_trace(httpd_req_t *req);
void vbus_trace_rearm(void);

/* Last attempts as text, oldest first: "FAIL lvl1 stale0 tx 02 .. | rx 02@12 .." (byte@ms since the send started). */
size_t vbus_format_capture(char *out, size_t size);

/* Wiring test: pull the bus low for about 20 ms by driving TX directly; returns vbus_rx_high_pct() meanwhile (expect 0). */
int vbus_tx_test(void);
void vbus_set_tap(vbus_tap_cb_t cb, void *arg);

#ifdef __cplusplus
}
#endif
