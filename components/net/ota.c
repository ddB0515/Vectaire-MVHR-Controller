/*
 * Firmware update over Wi-Fi: POST the app image (build/vectaire_c5.bin) to /api/ota with the OTA password in
 * the X-OTA-Password header (tools/ota.sh does this). A new image stays on trial until it reconnects to Wi-Fi;
 * if it crashes or doesn't reconnect within VERIFY_TIMEOUT_MS, the bootloader goes back to the previous one.
 */
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_priv.h"
#include "settings.h"

static const char *TAG = "ota";

#define VERIFY_TIMEOUT_MS   (5 * 60 * 1000)
#define CHUNK               4096
#define BAD_PASSWORD_DELAY  1000    /* slows down guessing */

#define CRASH_MAGIC         0x5642ca5e
#define CRASH_LIMIT         3           /* crash restarts in a row before going back to the other firmware */
#define STABLE_AFTER_MS     (2 * 60 * 1000)

/* survive a software reset, cleared at power-on */
static RTC_NOINIT_ATTR uint32_t s_crash_magic, s_crash_count;

static bool s_pending_verify;
static bool s_busy;
static esp_timer_handle_t s_verify_timer, s_restart_timer;

static void verify_timeout_cb(void *arg)
{
    ESP_LOGE(TAG, "new firmware didn't reconnect to Wi-Fi in time, rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

static void restart_cb(void *arg)
{
    esp_restart();
}

void net_restart_soon(void)
{
    esp_timer_start_once(s_restart_timer, 1000 * 1000);     /* let the reply go out first */
}

static void stable_cb(void *arg)
{
    s_crash_count = 0;
}

/* Boot-loop guard: a confirmed image can still crash later (and rollback no longer applies), so after
   CRASH_LIMIT crash restarts in a row, boot the other slot if it holds a firmware. */
static void crash_guard(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    bool crash = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    if (s_crash_magic != CRASH_MAGIC || !crash) {
        s_crash_magic = CRASH_MAGIC;
        s_crash_count = 0;
    }
    if (crash && ++s_crash_count >= CRASH_LIMIT) {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        esp_app_desc_t desc;
        if (other && esp_ota_get_partition_description(other, &desc) == ESP_OK) {
            s_crash_count = 0;
            ESP_LOGE(TAG, "%lu crash restarts in a row, going back to firmware %s in %s",
                     (unsigned long)CRASH_LIMIT, desc.version, other->label);
            if (esp_ota_set_boot_partition(other) == ESP_OK) {
                esp_restart();
            }
        }
    }
    if (crash) {
        ESP_LOGW(TAG, "restarted after a crash (%lu in a row)", (unsigned long)s_crash_count);
    }
    const esp_timer_create_args_t st = {.callback = stable_cb, .name = "ota_stable"};
    esp_timer_handle_t t;
    if (esp_timer_create(&st, &t) == ESP_OK) {
        esp_timer_start_once(t, STABLE_AFTER_MS * 1000ULL);
    }
}

void ota_init(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "firmware %s, built %s %s, running from %s", app->version, app->date, app->time, running->label);

    const esp_timer_create_args_t rt = {.callback = restart_cb, .name = "ota_restart"};
    esp_timer_create(&rt, &s_restart_timer);
    crash_guard();

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
        const esp_timer_create_args_t vt = {.callback = verify_timeout_cb, .name = "ota_verify"};
        esp_timer_create(&vt, &s_verify_timer);
        esp_timer_start_once(s_verify_timer, VERIFY_TIMEOUT_MS * 1000ULL);
        ESP_LOGW(TAG, "new firmware on trial until it reconnects to Wi-Fi");
    }
}

void ota_confirm(const char *why)
{
    if (!s_pending_verify) {
        return;
    }
    s_pending_verify = false;
    esp_timer_stop(s_verify_timer);
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "new firmware confirmed (%s)", why);
}

bool net_password_ok(httpd_req_t *req)
{
    char given[sizeof(settings_get()->ota_password) + 1];
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Password", given, sizeof(given)) != ESP_OK) {
        return false;
    }
    const char *want = settings_get()->ota_password;
    size_t n = strlen(want);
    if (strlen(given) != n) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= given[i] ^ want[i];
    }
    return diff == 0;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    ESP_LOGW(TAG, "update refused: %s", msg);
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, msg);
}

/* Receive exactly len bytes (less only at the end of the body). Returns the count, or -1 on error. */
static int recv_full(httpd_req_t *req, char *buf, int len)
{
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        got += r;
    }
    return got;
}

/* The image must be firmware for this project, not something else that happens to be an ESP32-C5 app. */
static const char *check_image(const char *buf, int len)
{
    const size_t off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (len < (int)(off + sizeof(esp_app_desc_t))) {
        return "image too short";
    }
    const esp_image_header_t *hdr = (const esp_image_header_t *)buf;
    if (hdr->magic != ESP_IMAGE_HEADER_MAGIC) {
        return "not an ESP app image";
    }
    esp_app_desc_t desc;
    memcpy(&desc, buf + off, sizeof(desc));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name)) != 0) {
        return "not Vectaire firmware";
    }
    ESP_LOGI(TAG, "receiving firmware %s, built %s %s", desc.version, desc.date, desc.time);
    return NULL;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    net_status_t st;
    net_get_status(&st);
    if (st.state != NET_CONNECTED) {
        return fail(req, "403 Forbidden", "updates only over the home network");
    }
    if (!net_password_ok(req)) {
        vTaskDelay(pdMS_TO_TICKS(BAD_PASSWORD_DELAY));
        return fail(req, "401 Unauthorized", "wrong OTA password");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len <= 0 || req->content_len > part->size) {
        return fail(req, "400 Bad Request", "bad image size");
    }
    if (s_busy) {
        return fail(req, "409 Conflict", "update already running");
    }
    s_busy = true;

    char *buf = malloc(CHUNK);
    esp_ota_handle_t ota = 0;
    const char *err = NULL;
    int total = 0;
    int64_t t0 = esp_timer_get_time();
    if (!buf) {
        err = "out of memory";
    }
    while (!err && total < req->content_len) {
        int want = req->content_len - total < CHUNK ? req->content_len - total : CHUNK;
        int n = recv_full(req, buf, want);
        if (n <= 0) {
            err = "upload interrupted";
            break;
        }
        if (total == 0) {
            err = check_image(buf, n);
            if (!err && esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
                err = "can't start update";
            }
            if (err) {
                break;
            }
        }
        if (esp_ota_write(ota, buf, n) != ESP_OK) {
            err = "flash write failed";
            break;
        }
        total += n;
    }
    free(buf);

    if (!err) {
        esp_err_t e = esp_ota_end(ota);
        ota = 0;
        if (e != ESP_OK) {
            err = e == ESP_ERR_OTA_VALIDATE_FAILED ? "image is corrupt" : "can't finish update";
        } else if (esp_ota_set_boot_partition(part) != ESP_OK) {
            err = "can't select new firmware";
        }
    }
    if (ota) {
        esp_ota_abort(ota);
    }
    s_busy = false;
    if (err) {
        return fail(req, "400 Bad Request", err);
    }

    ESP_LOGW(TAG, "update written to %s (%d bytes, %lld ms), restarting", part->label, total,
             (esp_timer_get_time() - t0) / 1000);
    httpd_resp_sendstr(req, "ok, restarting");
    esp_timer_start_once(s_restart_timer, 1000 * 1000);     /* let the reply go out first */
    return ESP_OK;
}

/* POST /api/restart, same password as updates */
static esp_err_t restart_post(httpd_req_t *req)
{
    if (!net_password_ok(req)) {
        vTaskDelay(pdMS_TO_TICKS(BAD_PASSWORD_DELAY));
        return fail(req, "401 Unauthorized", "wrong OTA password");
    }
    ESP_LOGW(TAG, "restart requested over HTTP");
    httpd_resp_sendstr(req, "ok, restarting");
    esp_timer_start_once(s_restart_timer, 1000 * 1000);
    return ESP_OK;
}

esp_err_t ota_register(httpd_handle_t server)
{
    const httpd_uri_t uri = {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post};
    const httpd_uri_t rst = {.uri = "/api/restart", .method = HTTP_POST, .handler = restart_post};
    httpd_register_uri_handler(server, &rst);
    return httpd_register_uri_handler(server, &uri);
}
