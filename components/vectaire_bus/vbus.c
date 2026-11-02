#include "vbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "vbus";

#define BAUD            1200
#define BYTE_TIME_US    (10 * 1000000 / BAUD)   /* 8N1 */

static vbus_config_t s_cfg;
static vbus_stats_t s_stats;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static vbus_tap_cb_t s_tap;
static void *s_tap_arg;
static vbus_responder_t s_responder;
static void *s_responder_arg;

/* raw capture of an attempt for diagnostics: what we sent, every byte heard with ms since the send started */
#define CAP_RX  96
typedef struct {
    uint8_t tx[40];
    uint8_t tx_len;
    uint8_t rx[CAP_RX];
    uint16_t rx_ms[CAP_RX];
    uint8_t rx_len;
    uint8_t ok;
    uint8_t level_before;       /* RX pin just before sending (1 = idle bus) */
    uint16_t stale;             /* bytes that arrived since the previous attempt and were discarded */
} capture_t;
#define CAP_HISTORY 6
static capture_t s_cap, s_cap_hist[CAP_HISTORY];

/* Bus trace from boot (or re-arm) until full, so start-up problems stay visible. Shorter without PSRAM. */
#define TRACE_LEN           4096
#define TRACE_LEN_INTERNAL  1024
typedef struct {
    uint32_t t_ms;
    char kind;          /* T sent, R heard during an attempt, I heard while idle, S discarded, K ok, X timeout */
    uint8_t b;
} trace_t;
static trace_t *s_trace;
static size_t s_trace_n, s_trace_len;

static void trace(char kind, const uint8_t *p, size_t n)
{
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&s_stats_lock);
    for (size_t i = 0; s_trace && i < n && s_trace_n < s_trace_len; i++) {
        s_trace[s_trace_n++] = (trace_t){.t_ms = t, .kind = kind, .b = p ? p[i] : 0};
    }
    portEXIT_CRITICAL(&s_stats_lock);
}
static uint8_t s_cap_next;

static void capture_done(bool ok)
{
    trace(ok ? 'K' : 'X', NULL, 1);
    s_cap.ok = ok;
    portENTER_CRITICAL(&s_stats_lock);
    s_cap_hist[s_cap_next] = s_cap;
    s_cap_next = (s_cap_next + 1) % CAP_HISTORY;
    portEXIT_CRITICAL(&s_stats_lock);
}

#define SIM_TURNAROUND_MS   20      /* simulated unit's delay before it answers */

static void tap(bool tx, const uint8_t *wire, size_t len)
{
    if (s_tap) {
        s_tap(tx, wire, len, s_tap_arg);
    }
    if (esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
        ESP_LOG_BUFFER_HEXDUMP(tx ? "vbus tx" : "vbus rx", wire, len, ESP_LOG_DEBUG);
    }
}

#define STAT_ADD(field, n) do { portENTER_CRITICAL(&s_stats_lock); s_stats.field += (n); portEXIT_CRITICAL(&s_stats_lock); } while (0)
#define STAT_INC(field) do { portENTER_CRITICAL(&s_stats_lock); s_stats.field++; portEXIT_CRITICAL(&s_stats_lock); } while (0)

esp_err_t vbus_init(const vbus_config_t *cfg)
{
    s_cfg = *cfg;
    const uart_config_t uc = {
        .baud_rate = BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    s_trace_len = TRACE_LEN;
    s_trace = heap_caps_calloc(TRACE_LEN, sizeof(trace_t), MALLOC_CAP_SPIRAM);
    if (!s_trace) {
        s_trace_len = TRACE_LEN_INTERNAL;
        s_trace = calloc(TRACE_LEN_INTERNAL, sizeof(trace_t));
    }
    ESP_RETURN_ON_ERROR(uart_driver_install(cfg->uart_num, 512, 0, 0, NULL, 0), TAG, "driver");
    ESP_RETURN_ON_ERROR(uart_param_config(cfg->uart_num, &uc), TAG, "params");
    ESP_RETURN_ON_ERROR(uart_set_pin(cfg->uart_num, cfg->tx_gpio, cfg->rx_gpio, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE), TAG, "pins");
    uint32_t inv = (cfg->invert_tx ? UART_SIGNAL_TXD_INV : 0) | (cfg->invert_rx ? UART_SIGNAL_RXD_INV : 0);
    ESP_RETURN_ON_ERROR(uart_set_line_inverse(cfg->uart_num, inv), TAG, "inverse");
    /* hand every byte to the driver at once: by default it waits for ~120 bytes or an idle timeout that
       doesn't fire reliably at 1200 baud, and replies then arrive seconds late */
    ESP_RETURN_ON_ERROR(uart_set_rx_full_threshold(cfg->uart_num, 1), TAG, "rx threshold");
    ESP_RETURN_ON_ERROR(uart_set_rx_timeout(cfg->uart_num, 2), TAG, "rx timeout");
    ESP_LOGI(TAG, "UART%d 1200 8N1, tx=%d%s rx=%d%s", cfg->uart_num, cfg->tx_gpio, cfg->invert_tx ? " (inv)" : "",
             cfg->rx_gpio, cfg->invert_rx ? " (inv)" : "");
    return ESP_OK;
}

static int rx_sample(void)
{
    /* sample against a weak pull-down: the divider (about 3.6k) still reads high, an open wire reads low */
    gpio_pullup_dis(s_cfg.rx_gpio);
    gpio_pulldown_en(s_cfg.rx_gpio);
    esp_rom_delay_us(100);
    int high = 0;
    for (int i = 0; i < 20; i++) {
        high += gpio_get_level(s_cfg.rx_gpio);
        esp_rom_delay_us(1000);
    }
    gpio_pulldown_dis(s_cfg.rx_gpio);
    gpio_pullup_en(s_cfg.rx_gpio);
    return high * 5;
}

size_t vbus_format_capture(char *out, size_t size)
{
    capture_t hist[CAP_HISTORY];
    uint8_t next;
    portENTER_CRITICAL(&s_stats_lock);
    memcpy(hist, s_cap_hist, sizeof(hist));
    next = s_cap_next;
    portEXIT_CRITICAL(&s_stats_lock);
    size_t n = 0;
    out[0] = 0;
    for (int k = 0; k < CAP_HISTORY && n < size; k++) {
        const capture_t *c = &hist[(next + k) % CAP_HISTORY];
        if (!c->tx_len) {
            continue;
        }
        n += snprintf(out + n, size - n, "%s%s lvl%u stale%u tx", n ? " || " : "", c->ok ? "OK" : "FAIL",
                      c->level_before, c->stale);
        for (int i = 0; i < c->tx_len && n < size; i++) {
            n += snprintf(out + n, size - n, " %02x", c->tx[i]);
        }
        if (n < size) {
            n += snprintf(out + n, size - n, " | rx");
        }
        for (int i = 0; i < c->rx_len && n < size; i++) {
            n += snprintf(out + n, size - n, " %02x@%u", c->rx[i], c->rx_ms[i]);
        }
    }
    return n < size ? n : size - 1;
}

void vbus_listen(uint32_t ms)
{
    if (s_responder) {
        vTaskDelay(pdMS_TO_TICKS(ms));
        return;
    }
    uint8_t buf[32];
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    for (int64_t now; (now = esp_timer_get_time()) < end;) {
        int n = uart_read_bytes(s_cfg.uart_num, buf, sizeof(buf), pdMS_TO_TICKS((end - now) / 1000 + 1));
        if (n > 0) {
            STAT_ADD(rx_bytes, n);
            STAT_ADD(idle_bytes, n);
            trace('I', buf, n);
        }
    }
}

void vbus_trace_rearm(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    s_trace_n = 0;
    portEXIT_CRITICAL(&s_stats_lock);
}

esp_err_t vbus_send_trace(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char line[400];
    size_t n = 0, count;
    portENTER_CRITICAL(&s_stats_lock);
    count = s_trace_n;
    portEXIT_CRITICAL(&s_stats_lock);
    /* one line per burst: same kind, bytes less than 15 ms apart (a byte takes 8.3 ms at 1200 baud) */
    for (size_t i = 0; i < count; i++) {
        const trace_t *e = &s_trace[i];
        bool cont = i > 0 && e->kind == s_trace[i - 1].kind && e->t_ms - s_trace[i - 1].t_ms < 15 &&
                    e->kind != 'K' && e->kind != 'X' && n < sizeof(line) - 8;
        if (!cont) {
            if (n) {
                line[n++] = '\n';
                httpd_resp_send_chunk(req, line, n);
            }
            n = snprintf(line, sizeof(line), "%6lu.%03lu %c", (unsigned long)(e->t_ms / 1000),
                         (unsigned long)(e->t_ms % 1000), e->kind);
            if (e->kind == 'K' || e->kind == 'X') {
                n += snprintf(line + n, sizeof(line) - n, e->kind == 'K' ? " reply ok" : " timeout");
                continue;
            }
        }
        n += snprintf(line + n, sizeof(line) - n, " %02x", e->b);
    }
    if (n) {
        line[n++] = '\n';
        httpd_resp_send_chunk(req, line, n);
    }
    snprintf(line, sizeof(line), "-- %u of %u entries%s\n", (unsigned)count, (unsigned)s_trace_len,
             count >= s_trace_len ? " (full; POST /api/bus/trace to re-arm)" : "");
    httpd_resp_send_chunk(req, line, strlen(line));
    return httpd_resp_send_chunk(req, NULL, 0);
}

int vbus_rx_high_pct(void)
{
    return s_responder ? -1 : rx_sample();
}

int vbus_tx_test(void)
{
    if (s_responder) {
        return -1;
    }
    /* drive the NPN directly (pin high = transistor on = bus low), then give the pin back to the UART */
    gpio_reset_pin(s_cfg.tx_gpio);
    gpio_set_direction(s_cfg.tx_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(s_cfg.tx_gpio, 1);
    esp_rom_delay_us(500);
    int pct = rx_sample();
    gpio_set_level(s_cfg.tx_gpio, 0);
    uart_set_pin(s_cfg.uart_num, s_cfg.tx_gpio, s_cfg.rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGW(TAG, "TX test: RX high %d%% while the bus should be pulled low", pct);
    return pct;
}

esp_err_t vbus_init_simulated(const vbus_config_t *cfg, vbus_responder_t responder, void *arg)
{
    s_cfg = *cfg;
    s_responder_arg = arg;
    s_responder = responder;
    ESP_LOGW(TAG, "SIMULATED unit, the bus UART is not used");
    return ESP_OK;
}

void vbus_set_tap(vbus_tap_cb_t cb, void *arg)
{
    s_tap_arg = arg;
    s_tap = cb;
}

void vbus_get_stats(vbus_stats_t *out)
{
    portENTER_CRITICAL(&s_stats_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
}

/* One attempt: send, then collect bytes until a matching reply or timeout. */
static esp_err_t attempt(const uint8_t *wire, size_t wire_len, uint8_t cmd, vbus_frame_t *reply)
{
    int64_t start_us = esp_timer_get_time();
    s_cap.tx_len = wire_len < sizeof(s_cap.tx) ? wire_len : sizeof(s_cap.tx);
    memcpy(s_cap.tx, wire, s_cap.tx_len);
    s_cap.rx_len = 0;
    size_t stale = 0;
    uart_get_buffered_data_len(s_cfg.uart_num, &stale);
    s_cap.stale = stale;
    s_cap.level_before = gpio_get_level(s_cfg.rx_gpio);
    uint8_t junk[32];
    int nj;
    while ((nj = uart_read_bytes(s_cfg.uart_num, junk, sizeof(junk), 0)) > 0) {
        trace('S', junk, nj);
    }
    uart_write_bytes(s_cfg.uart_num, wire, wire_len);
    trace('T', wire, wire_len);
    tap(true, wire, wire_len);
    /* frame duration at 1200 baud, plus margin; the echo arrives during this time */
    uart_wait_tx_done(s_cfg.uart_num, pdMS_TO_TICKS(wire_len * BYTE_TIME_US / 1000 + 100));
    int64_t sent_us = esp_timer_get_time();

    vbus_decoder_t dec;
    vbus_decoder_reset(&dec);
    vbus_frame_t f;
    uint8_t rx_wire[VBUS_MAX_WIRE];
    size_t rx_len = 0;
    int64_t frame_start_us = 0;
    int64_t deadline = sent_us + (int64_t)s_cfg.reply_timeout_ms * 1000;

    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now >= deadline) {
            if (rx_len) {
                tap(false, rx_wire, rx_len);
            }
            capture_done(false);
            return ESP_ERR_TIMEOUT;
        }
        uint8_t b;
        int n = uart_read_bytes(s_cfg.uart_num, &b, 1, pdMS_TO_TICKS((deadline - now) / 1000 + 1));
        if (n <= 0) {
            continue;
        }
        STAT_INC(rx_bytes);
        trace('R', &b, 1);
        now = esp_timer_get_time();
        if (s_cap.rx_len < CAP_RX) {
            s_cap.rx[s_cap.rx_len] = b;
            s_cap.rx_ms[s_cap.rx_len++] = (uint16_t)((now - start_us) / 1000);
        }
        if (b == VBUS_STX) {
            frame_start_us = now;
        }
        /* keep waiting while bytes keep coming */
        int64_t gap_deadline = now + (int64_t)s_cfg.gap_timeout_ms * 1000;
        if (gap_deadline > deadline) {
            deadline = gap_deadline;
        }
        if (rx_len < sizeof(rx_wire)) {
            rx_wire[rx_len++] = b;
        }
        switch (vbus_decoder_feed(&dec, b, &f)) {
        case VBUS_DEC_FRAME:
            if (f.addr == VBUS_ADDR_REQUEST) {
                /* our own echo (or a stale request): the unit's reply only starts now */
                STAT_INC(echo_frames);
                rx_len = 0;
                sent_us = now;
                deadline = now + (int64_t)s_cfg.reply_timeout_ms * 1000;
                break;
            }
            if (f.addr == VBUS_ADDR_REPLY && f.cmd == cmd) {
                portENTER_CRITICAL(&s_stats_lock);
                s_stats.last_reply_ms = frame_start_us > sent_us ? (uint32_t)((frame_start_us - sent_us) / 1000) : 0;
                portEXIT_CRITICAL(&s_stats_lock);
                tap(false, rx_wire, rx_len);
                capture_done(true);
                *reply = f;
                return ESP_OK;
            }
            ESP_LOGW(TAG, "unexpected frame addr %02x cmd %02x (waiting for %02x)", f.addr, f.cmd, cmd);
            break;
        case VBUS_DEC_BAD_CRC:
            STAT_INC(crc_errors);
            break;
        case VBUS_DEC_BAD_LEN:
        case VBUS_DEC_OVERFLOW:
            STAT_INC(framing_errors);
            break;
        default:
            break;
        }
    }
}

/* Feed wire bytes through a fresh decoder; true if they hold one valid frame. */
static bool decode_wire(const uint8_t *wire, size_t len, vbus_frame_t *f)
{
    vbus_decoder_t dec;
    vbus_decoder_reset(&dec);
    for (size_t i = 0; i < len; i++) {
        if (vbus_decoder_feed(&dec, wire[i], f) == VBUS_DEC_FRAME) {
            return true;
        }
    }
    return false;
}

/* One attempt against the simulated unit: same frames, same timing, no UART. */
static esp_err_t sim_attempt(const uint8_t *wire, size_t wire_len, uint8_t cmd, vbus_frame_t *reply)
{
    tap(true, wire, wire_len);
    vbus_frame_t req;
    uint8_t data[VBUS_MAX_DATA];
    int n = decode_wire(wire, wire_len, &req) ? s_responder(req.cmd, req.data, req.len, data, s_responder_arg) : -1;
    uint8_t rx_wire[VBUS_MAX_WIRE];
    size_t rx_len = n >= 0 ? vbus_encode(VBUS_ADDR_REPLY, req.cmd, data, (uint8_t)n, rx_wire, sizeof(rx_wire)) : 0;
    if (!rx_len) {
        vTaskDelay(pdMS_TO_TICKS(wire_len * BYTE_TIME_US / 1000 + s_cfg.reply_timeout_ms));
        return ESP_ERR_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS((wire_len + rx_len) * BYTE_TIME_US / 1000 + SIM_TURNAROUND_MS));
    tap(false, rx_wire, rx_len);
    if (!decode_wire(rx_wire, rx_len, reply) || reply->cmd != cmd) {
        STAT_INC(framing_errors);
        return ESP_ERR_INVALID_RESPONSE;
    }
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.last_reply_ms = SIM_TURNAROUND_MS;
    portEXIT_CRITICAL(&s_stats_lock);
    return ESP_OK;
}

esp_err_t vbus_transact(uint8_t cmd, const uint8_t *data, uint8_t len, vbus_frame_t *reply)
{
    uint8_t wire[VBUS_MAX_WIRE];
    size_t wire_len = vbus_encode(VBUS_ADDR_REQUEST, cmd, data, len, wire, sizeof(wire));
    ESP_RETURN_ON_FALSE(wire_len, ESP_ERR_INVALID_SIZE, TAG, "frame too long");
    STAT_INC(requests);
    for (int i = 0; i <= s_cfg.retries; i++) {
        esp_err_t err = s_responder ? sim_attempt(wire, wire_len, cmd, reply) : attempt(wire, wire_len, cmd, reply);
        if (err == ESP_OK) {
            STAT_INC(replies);
            return ESP_OK;
        }
        STAT_INC(timeouts);
    }
    STAT_INC(failures);
    return ESP_ERR_TIMEOUT;
}
