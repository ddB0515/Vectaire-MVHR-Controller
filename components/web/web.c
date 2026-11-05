/*
 * Home page (www/home.html), diagnostics page (www/diag.html) and JSON API. Reading and the bus wiring test are
 * open on the local network; every change needs the device password in the X-OTA-Password header (the same
 * password as OTA, shown on the device).
 */
#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "autoclock.h"
#include "cJSON.h"
#include "timezones.h"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha.h"
#include "mvhr.h"
#include "net.h"
#include "settings.h"
#include "vbus.h"

static const char *TAG = "web";

extern const char home_html_start[] asm("_binary_home_html_start");
extern const char home_html_end[] asm("_binary_home_html_end");
extern const char diag_html_start[] asm("_binary_diag_html_start");
extern const char diag_html_end[] asm("_binary_diag_html_end");

static esp_err_t unauthorized(httpd_req_t *req)
{
    vTaskDelay(pdMS_TO_TICKS(1000));    /* slows down guessing */
    httpd_resp_set_status(req, "401 Unauthorized");
    return httpd_resp_sendstr(req, "wrong device password");
}

static esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = net_send_chunked(req, txt, strlen(txt));
    cJSON_free(txt);
    return err;
}

/* ---------------------------------------------------------------- page */

static esp_err_t home_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* EMBED_TXTFILES appends a NUL terminator; don't send it */
    return net_send_chunked(req, home_html_start, home_html_end - home_html_start - 1);
}

static esp_err_t diag_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return net_send_chunked(req, diag_html_start, diag_html_end - diag_html_start - 1);
}

/* ---------------------------------------------------------------- GET /api/state */

static esp_err_t state_get(httpd_req_t *req)
{
    static const char *bypass[] = {"heat", "bypass", "mode3"};
    static const char *ha_states[] = {"off", "connecting", "connected"};
    mvhr_state_t st;
    mvhr_get_state(&st);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "online", st.online);
    cJSON_AddBoolToObject(o, "simulated", mvhr_is_simulated());
    cJSON_AddNumberToObject(o, "out_offset", st.outdoor_offset_x10 / 10.0);     /* outdoor temperature offset, C */
    if (st.valid & MVHR_G_STATUS) {
        cJSON_AddNumberToObject(o, "t_in", st.temp_extract_x10 / 10.0);
        cJSON_AddNumberToObject(o, "t_out", st.temp_supply_x10 / 10.0);            /* with the offset */
        cJSON_AddNumberToObject(o, "t_out_raw", st.temp_supply_raw_x10 / 10.0);    /* as the unit reports it */
        if (st.humidity_x10 <= MVHR_RH_VALID_MAX_X10) {
            cJSON_AddNumberToObject(o, "rh", st.humidity_x10 / 10.0);
        }
        cJSON_AddNumberToObject(o, "fan_a", st.fan_a_pct);
        cJSON_AddNumberToObject(o, "fan_b", st.fan_b_pct);
        cJSON_AddNumberToObject(o, "fan", st.fan_speed);
        cJSON_AddStringToObject(o, "bypass", st.bypass_override == MVHR_BYPASS_AUTO ? "auto"
                                             : st.bypass_override <= 2 ? bypass[st.bypass_override] : "auto");
        cJSON_AddNumberToObject(o, "bypass_state", st.bypass_auto_state);
        cJSON_AddBoolToObject(o, "holiday", st.holiday);
        cJSON_AddBoolToObject(o, "frost", st.frost);
        const char *fn = mvhr_function_text(&st);
        cJSON_AddStringToObject(o, "function", fn ? fn : "Normal");
        cJSON_AddStringToObject(o, "boost", mvhr_boost_text(&st));
        cJSON_AddStringToObject(o, "airflow", mvhr_airflow_text(&st));
        cJSON_AddNumberToObject(o, "alarms", st.alarms);
        cJSON_AddStringToObject(o, "alarm", mvhr_alarm_text(st.alarms));
        cJSON_AddNumberToObject(o, "fire_reply", st.fire_reply);
    }
    if (st.valid & MVHR_G_SETTINGS) {
        cJSON_AddNumberToObject(o, "rh_set", st.humidity_setpoint);
        cJSON_AddNumberToObject(o, "boost_delay", st.boost_delay_s);
        cJSON_AddNumberToObject(o, "overrun_min", st.overrun_s / 60);
    }
    if (st.valid & MVHR_G_VERSION) {
        char fw[8];
        snprintf(fw, sizeof(fw), "%u.%u", st.fw_major, st.fw_minor);
        cJSON_AddStringToObject(o, "unit_fw", fw);
        if (st.night_known) {
            cJSON *n = cJSON_AddObjectToObject(o, "night");
            cJSON_AddBoolToObject(n, "on", st.night_enabled);
            cJSON_AddNumberToObject(n, "start", st.night_start_min);
            cJSON_AddNumberToObject(n, "end", st.night_end_min);
        }
        if (st.cfg_len >= 49 && st.cfg[0] == 0x31) {
            /* installer fan settings, read only: PWM 0..4095 -> % with one decimal (see ui_settings.c) */
#define CFG16(at) ((unsigned)((st.cfg[at] << 8) | st.cfg[(at) + 1]))
#define PWM_PCT(at) ((double)((CFG16(at) * 1000U + 2048U) / 4096U) / 10.0)
            cJSON *f = cJSON_AddObjectToObject(o, "fans");
            cJSON *in = cJSON_AddArrayToObject(f, "in"), *out = cJSON_AddArrayToObject(f, "out");
            for (int i = 0; i < 3; i++) {
                cJSON_AddItemToArray(in, cJSON_CreateNumber(PWM_PCT(8 + 2 * i)));
                cJSON_AddItemToArray(out, cJSON_CreateNumber(PWM_PCT(14 + 2 * i)));
            }
            cJSON_AddNumberToObject(f, "stall", PWM_PCT(20));
            cJSON_AddNumberToObject(f, "holiday", CFG16(47) / 10.0);
#undef PWM_PCT
#undef CFG16
        }
    }
    if (st.valid & MVHR_G_COUNTERS) {
        cJSON_AddNumberToObject(o, "filter", st.filter_pct);
        cJSON_AddNumberToObject(o, "hours", st.hours);
        cJSON_AddNumberToObject(o, "outages", st.power_outages);
    }
    if (st.valid & MVHR_G_CLOCK) {
        cJSON *c = cJSON_AddObjectToObject(o, "clock");
        cJSON_AddNumberToObject(c, "h", st.clock.hour);
        cJSON_AddNumberToObject(c, "mi", st.clock.minute);
        cJSON_AddNumberToObject(c, "d", st.clock.day);
        cJSON_AddNumberToObject(c, "mo", st.clock.month);
        cJSON_AddNumberToObject(c, "y", 2000 + st.clock.year);
        /* the rest of the unit's clock block, raw (for the Diagnostics page): 12/24 h, date format, time zone
           list index, UTC offset byte (low 7 bits offset, top bit possibly the DST flag) */
        cJSON_AddNumberToObject(c, "f24", st.clock.format_24h);
        cJSON_AddNumberToObject(c, "datefmt", st.clock.date_format);
        cJSON_AddNumberToObject(c, "region", st.clock.region);
        cJSON_AddNumberToObject(c, "offset", st.clock.utc_offset);
    }
    {
        cJSON *nt = cJSON_AddObjectToObject(o, "ntp");      /* network time: on/off, standard time offset in hours, rule */
        cJSON_AddBoolToObject(nt, "on", settings_get()->ntp_on);
        cJSON_AddNumberToObject(nt, "tz", settings_get()->tz_std_x10 / 10.0);
        cJSON_AddNumberToObject(nt, "rule", settings_get()->dst_rule);      /* 0..3 = a summer time rule, 4 = none */
        cJSON_AddNumberToObject(nt, "zone", tz_find(settings_get()->tz_std_x10, settings_get()->dst_rule));   /* -1 = not in the list */
        char text[128];
        autoclock_ntp_describe(text, sizeof(text));
        cJSON_AddStringToObject(nt, "text", text);
    }

    if (httpd_req_get_url_query_len(req) > 0) {
        char q[32] = "";
        httpd_req_get_url_query_str(req, q, sizeof(q));
        if (strstr(q, "zones")) {       /* the time zone list, asked for once when the page loads: [name, offset x10, rule] */
            cJSON *zl = cJSON_AddArrayToObject(o, "zones");
            for (int i = 0; i < tz_count(); i++) {
                cJSON *z = cJSON_CreateArray();
                cJSON_AddItemToArray(z, cJSON_CreateString(tz_get(i)->name));
                cJSON_AddItemToArray(z, cJSON_CreateNumber(tz_get(i)->tz_std_x10));
                cJSON_AddItemToArray(z, cJSON_CreateNumber(tz_get(i)->rule));
                cJSON_AddItemToArray(zl, z);
            }
        }
    }

    net_status_t n;
    net_get_status(&n);
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *d = cJSON_AddObjectToObject(o, "device");
    cJSON_AddStringToObject(d, "name", settings_get()->device_name);
    cJSON_AddStringToObject(d, "id", settings_device_id());
    cJSON_AddStringToObject(d, "fw", app->version);
#ifdef VECTAIRE_HEADLESS
    cJSON_AddStringToObject(d, "hw", CONFIG_IDF_TARGET " (headless)");
#else
    cJSON_AddStringToObject(d, "hw", CONFIG_IDF_TARGET " (display)");
#endif
    cJSON_AddStringToObject(d, "ssid", settings_get()->wifi_ssid);     /* the saved network, also during a trial */
    cJSON_AddBoolToObject(d, "wifi_trial", n.wifi_trial);
    cJSON_AddStringToObject(d, "wifi_change", n.wifi_change);
    cJSON_AddStringToObject(d, "ip", n.ip);
    cJSON_AddNumberToObject(d, "rssi", n.rssi);
    cJSON_AddStringToObject(d, "ha", ha_states[ha_state()]);
    cJSON_AddNumberToObject(d, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(d, "heap_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(d, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    return send_json(req, o);
}

/* ---------------------------------------------------------------- POST /api/set */

static bool get_int(const cJSON *root, const char *key, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    *out = v->valueint;
    return true;
}

/* Body: one or more of {"fan":2, "bypass":"auto|heat|bypass", "holiday":true, "rh_set":60, "boost_delay":60,
   "overrun_min":20, "night":{"on":true,"start":1320,"end":360}, "reset_filter":true,
   "clock":{"h":12,"mi":30,"d":29,"mo":10,"y":2026}, "out_offset":-0.8, "ntp":{"on":true,"zone":12}, "ntp_check":true, "fans":{"in":[30,50,80],"out":[30,50,80]}} */
static esp_err_t set_post(httpd_req_t *req)
{
    if (!net_password_ok(req)) {
        return unauthorized(req);
    }
    if (req->content_len <= 0 || req->content_len > 512) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    char buf[513];
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read");
        }
        got += r;
    }
    buf[got] = 0;
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    }

    mvhr_state_t st;
    mvhr_get_state(&st);
    esp_err_t err = ESP_OK;
    int n = 0, v;
    const cJSON *j;
    if (get_int(root, "fan", &v)) {
        err |= mvhr_set_fan_speed((uint8_t)v);
        n++;
    }
    if (cJSON_IsString(j = cJSON_GetObjectItemCaseSensitive(root, "bypass"))) {
        err |= mvhr_set_bypass(!strcmp(j->valuestring, "heat") ? 0 : !strcmp(j->valuestring, "bypass") ? 1
                               : MVHR_BYPASS_AUTO);
        n++;
    }
    if (cJSON_IsBool(j = cJSON_GetObjectItemCaseSensitive(root, "holiday"))) {
        err |= mvhr_set_holiday(cJSON_IsTrue(j));
        n++;
    }
    if (get_int(root, "rh_set", &v)) {
        err |= mvhr_set_humidity_setpoint((uint8_t)v);
        n++;
    }
    if (get_int(root, "boost_delay", &v)) {
        err |= mvhr_set_boost_delay((uint16_t)v);
        n++;
    }
    if (get_int(root, "overrun_min", &v)) {
        err |= mvhr_set_overrun((uint16_t)(v * 60));
        n++;
    }
    if (cJSON_IsObject(j = cJSON_GetObjectItemCaseSensitive(root, "night"))) {
        int start = st.night_start_min, end = st.night_end_min;
        get_int(j, "start", &start);
        get_int(j, "end", &end);
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(j, "on");
        err |= st.night_known ? mvhr_set_night_inhibition(cJSON_IsBool(on) ? cJSON_IsTrue(on) : st.night_enabled,
                                                          (uint16_t)start, (uint16_t)end)
                              : ESP_ERR_INVALID_STATE;
        n++;
    }
    if (cJSON_IsNumber(j = cJSON_GetObjectItemCaseSensitive(root, "out_offset"))) {
        /* outdoor temperature offset in degrees, one decimal: saved and applied at once */
        double x10 = j->valuedouble * 10.0;
        int r = (int)(x10 < 0 ? x10 - 0.5 : x10 + 0.5);
        if (r < -MVHR_OUTDOOR_OFFSET_MAX_X10 || r > MVHR_OUTDOOR_OFFSET_MAX_X10) {
            err |= ESP_ERR_INVALID_ARG;
        } else {
            settings_t s = *settings_get();
            s.outdoor_offset_x10 = (int16_t)r;
            err |= settings_save(&s);
            err |= mvhr_set_outdoor_offset((int16_t)r);
        }
        n++;
    }
    if (cJSON_IsObject(j = cJSON_GetObjectItemCaseSensitive(root, "ntp"))) {
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(j, "on"), *tz = cJSON_GetObjectItemCaseSensitive(j, "tz");
        int tz_x10 = settings_get()->tz_std_x10;
        if (cJSON_IsNumber(tz)) {
            double x = tz->valuedouble * 10.0;
            tz_x10 = (int)(x < 0 ? x - 0.5 : x + 0.5);
        }
        int rule = settings_get()->dst_rule, zone;
        if (get_int(j, "zone", &zone)) {            /* a zone from the list sets both the offset and the rule */
            const tz_zone_t *z = tz_get(zone);
            if (z) {
                tz_x10 = z->tz_std_x10;
                rule = z->rule;
            } else {
                rule = -1;
            }
        } else {
            get_int(j, "rule", &rule);
        }
        err |= (rule < 0 || rule > 4) ? ESP_ERR_INVALID_ARG
                                      : autoclock_ntp_configure(cJSON_IsBool(on) ? cJSON_IsTrue(on) : settings_get()->ntp_on,
                                                                (int16_t)tz_x10, (uint8_t)rule);
        n++;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ntp_check"))) {
        autoclock_ntp_check_now();
        n++;
    }
    if (cJSON_IsObject(j = cJSON_GetObjectItemCaseSensitive(root, "fans"))) {
        /* {"in":[30,50,80],"out":[30,50,80]}: percent per speed, null or missing = leave as is */
        uint16_t duty[6] = {0};
        uint8_t mask = 0;
        bool bad = false;
        static const char *const side[2] = {"in", "out"};
        for (int s = 0; s < 2; s++) {
            const cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, side[s]);
            for (int i = 0; i < 3 && cJSON_IsArray(arr); i++) {
                const cJSON *e = cJSON_GetArrayItem(arr, i);
                if (!cJSON_IsNumber(e)) {
                    continue;
                }
                double x10 = e->valuedouble * 10.0 + 0.5;
                if (x10 < MVHR_FAN_DUTY_MIN_X10 || x10 > MVHR_FAN_DUTY_MAX_X10 + 0.99) {
                    bad = true;
                    continue;
                }
                duty[3 * s + i] = (uint16_t)x10;
                mask |= 1u << (3 * s + i);
            }
        }
        err |= (bad || !mask || !st.cfg_len) ? ESP_ERR_INVALID_ARG : mvhr_set_fan_duties(duty, mask);
        n++;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "reset_filter"))) {
        err |= mvhr_reset_filter();
        n++;
    }
    if (cJSON_IsObject(j = cJSON_GetObjectItemCaseSensitive(root, "clock"))) {
        int h, mi, d, mo, y;
        if (get_int(j, "h", &h) && get_int(j, "mi", &mi) && get_int(j, "d", &d) && get_int(j, "mo", &mo) &&
            get_int(j, "y", &y)) {
            err |= mvhr_set_clock((uint8_t)h, (uint8_t)mi, (uint8_t)d, (uint8_t)mo, (uint16_t)y);
        } else {
            err |= ESP_ERR_INVALID_ARG;
        }
        n++;
    }
    cJSON_Delete(root);
    if (!n || err != ESP_OK) {
        ESP_LOGW(TAG, "change refused (%d items)", n);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, n ? "value out of range or unit not ready"
                                                                  : "nothing to change");
    }
    ESP_LOGI(TAG, "%d change(s) queued", n);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* GET /api/auth: lets the page check the password before enabling the controls */
static esp_err_t auth_get(httpd_req_t *req)
{
    if (!net_password_ok(req)) {
        return unauthorized(req);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ---------------------------------------------------------------- diagnostics */

/* GET /api/bus: bus counters and the unit's main values, for checking the wiring without USB */
static esp_err_t bus_get(httpd_req_t *req)
{
    vbus_stats_t b;
    mvhr_state_t m;
    vbus_get_stats(&b);
    mvhr_get_state(&m);
    static char json[4200];     /* httpd serves one request at a time */
    snprintf(json, sizeof(json),
             "{\"simulated\":%s,\"online\":%s,\"requests\":%lu,\"replies\":%lu,\"timeouts\":%lu,"
             "\"failures\":%lu,\"rx_bytes\":%lu,\"echo_frames\":%lu,\"crc_errors\":%lu,\"framing_errors\":%lu,"
             "\"uptime_s\":%lu,\"idle_bytes\":%lu,\"last_reply_ms\":%lu,\"rx_high_pct\":%d,\"extract_x10\":%d,\"supply_x10\":%d,\"fan\":%u,\"fw\":\"%u.%u\"}",
             mvhr_is_simulated() ? "true" : "false", m.online ? "true" : "false", (unsigned long)b.requests,
             (unsigned long)b.replies, (unsigned long)b.timeouts, (unsigned long)b.failures,
             (unsigned long)b.rx_bytes, (unsigned long)b.echo_frames, (unsigned long)b.crc_errors,
             (unsigned long)b.framing_errors, (unsigned long)(esp_timer_get_time() / 1000000),
             (unsigned long)b.idle_bytes, (unsigned long)b.last_reply_ms, vbus_rx_high_pct(), m.temp_extract_x10,
             m.temp_supply_x10, m.fan_speed, m.fw_major, m.fw_minor);
    static char cap[3600];
    vbus_format_capture(cap, sizeof(cap));
    size_t n = strlen(json);
    snprintf(json + n - 1, sizeof(json) - n + 1, ",\"attempts\":\"%s\"}", cap);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return net_send_chunked(req, json, strlen(json));
}

/* POST /api/bus/txtest: pull the bus low briefly and report whether RX sees it */
static esp_err_t txtest_post(httpd_req_t *req)
{
    int idle = vbus_rx_high_pct();
    int active = vbus_tx_test();
    char json[64];
    snprintf(json, sizeof(json), "{\"rx_high_pct_idle\":%d,\"rx_high_pct_tx_on\":%d}", idle, active);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t trace_get(httpd_req_t *req)
{
    return vbus_send_trace(req);
}

static esp_err_t trace_post(httpd_req_t *req)
{
    if (!net_password_ok(req)) {
        return unauthorized(req);
    }
    vbus_trace_rearm();
    return httpd_resp_sendstr(req, "trace re-armed");
}

/* GET /api/crash: why we last restarted, plus the stored core dump summary (pc and code addresses: the stack's on
   RISC-V, the backtrace on Xtensa; for addr2line against the .elf of the same build) */
static esp_err_t crash_get(httpd_req_t *req)
{
    static const char *reasons[] = {"unknown", "power-on", "external", "software", "panic", "int-wdt", "task-wdt",
                                    "wdt", "deep-sleep", "brownout", "sdio", "usb", "jtag", "efuse", "pwr-glitch",
                                    "cpu-lockup"};
    esp_reset_reason_t r = esp_reset_reason();
    static char out[1024];
    static const char *ha_states[] = {"off", "connecting", "connected"};
    size_t n = snprintf(out, sizeof(out),
                        "{\"reset_reason\":\"%s\",\"uptime_s\":%lu,\"heap_free\":%u,\"heap_min\":%u,"
                        "\"heap_largest\":%u,\"psram_free\":%u,\"ha\":\"%s\"",
                        (size_t)r < sizeof(reasons) / sizeof(reasons[0]) ? reasons[r] : "?",
                        (unsigned long)(esp_timer_get_time() / 1000000),
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), ha_states[ha_state()]);
    esp_core_dump_summary_t *sum = malloc(sizeof(*sum));
    if (sum && esp_core_dump_image_check() == ESP_OK && esp_core_dump_get_summary(sum) == ESP_OK) {
#if CONFIG_IDF_TARGET_ARCH_RISCV
        /* no backtrace on RISC-V: list the stack words that point at code instead */
        n += snprintf(out + n, sizeof(out) - n,
                      ",\"task\":\"%s\",\"pc\":\"0x%08lx\",\"ra\":\"0x%08lx\",\"cause\":%lu,\"addr\":\"0x%08lx\","
                      "\"elf_sha\":\"%.16s\",\"stack_code\":\"",
                      sum->exc_task, (unsigned long)sum->exc_pc, (unsigned long)sum->ex_info.ra,
                      (unsigned long)sum->ex_info.mcause, (unsigned long)sum->ex_info.mtval,
                      (const char *)sum->app_elf_sha256);
        const uint32_t *w = (const uint32_t *)sum->exc_bt_info.stackdump;
        for (uint32_t i = 0; i < sum->exc_bt_info.dump_size / 4 && n < sizeof(out) - 16; i++) {
            if (esp_ptr_executable((const void *)w[i])) {
                n += snprintf(out + n, sizeof(out) - n, "%08lx ", (unsigned long)w[i]);
            }
        }
#else
        /* Xtensa keeps a real backtrace */
        n += snprintf(out + n, sizeof(out) - n,
                      ",\"task\":\"%s\",\"pc\":\"0x%08lx\",\"cause\":%lu,\"addr\":\"0x%08lx\","
                      "\"elf_sha\":\"%.16s\",\"stack_code\":\"",
                      sum->exc_task, (unsigned long)sum->exc_pc, (unsigned long)sum->ex_info.exc_cause,
                      (unsigned long)sum->ex_info.exc_vaddr, (const char *)sum->app_elf_sha256);
        for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 16 && n < sizeof(out) - 16; i++) {
            n += snprintf(out + n, sizeof(out) - n, "%08lx ", (unsigned long)sum->exc_bt_info.bt[i]);
        }
#endif
        n += snprintf(out + n, sizeof(out) - n, "\"");
    }
    free(sum);
    snprintf(out + n, sizeof(out) - n, "}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static void register_handlers(httpd_handle_t srv)
{
    const httpd_uri_t uris[] = {
        {.uri = "/diag", .method = HTTP_GET, .handler = diag_get},
        {.uri = "/api/state", .method = HTTP_GET, .handler = state_get},
        {.uri = "/api/set", .method = HTTP_POST, .handler = set_post},
        {.uri = "/api/auth", .method = HTTP_GET, .handler = auth_get},
        {.uri = "/api/bus", .method = HTTP_GET, .handler = bus_get},
        {.uri = "/api/bus/txtest", .method = HTTP_POST, .handler = txtest_post},
        {.uri = "/api/bus/trace", .method = HTTP_GET, .handler = trace_get},
        {.uri = "/api/bus/trace", .method = HTTP_POST, .handler = trace_post},
        {.uri = "/api/crash", .method = HTTP_GET, .handler = crash_get},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(srv, &uris[i]) != ESP_OK) {
            ESP_LOGE(TAG, "can't register %s", uris[i].uri);
        }
    }
}

void web_start(void)
{
    net_set_home_handler(home_get);
    net_on_server_start(register_handlers);
}
