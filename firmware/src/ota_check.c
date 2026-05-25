// SPDX-License-Identifier: GPL-2.0-or-later
//
// ota_check — Implementation.  Siehe ota_check.h für die API-Semantik.

#include "ota_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "version.h"

static const char *TAG = "ota_check";

#define MAX_RESP_BYTES   2048   // manifest.json ist ~500 B — 2K reicht mit headroom
#define VER_STR_MAX      24

typedef struct {
    ota_check_state_t state;
    char              current_ver[VER_STR_MAX];
    char              latest_ver[VER_STR_MAX];
    char              error[96];
    int64_t           last_check_us;   // 0 = nie
} ota_state_t;

static ota_state_t      s_state;
static SemaphoreHandle_t s_mtx = NULL;

// Lazy-init der Mutex (kein eigener init() — wird beim ersten
// Aufruf erzeugt, das ist racy aber funktioniert weil Handler beim
// httpd-Startup-init alle vom selben Task aufgerufen werden bevor
// die WebUI hochgeht).
static void mtx_ensure(void)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        // Default-state — current_ver wird beim ersten refresh() gesetzt.
        s_state.state = OTA_CHECK_IDLE;
        snprintf(s_state.current_ver, sizeof(s_state.current_ver),
                 "%s", FW_VERSION_STRING);
        s_state.latest_ver[0] = '\0';
        s_state.error[0]      = '\0';
        s_state.last_check_us = 0;
    }
}

// ───── Versions-Vergleich ──────────────────────────────────────────────

// Parsing-Helper: "0.14.138" → 3 ints, true bei Erfolg.
static bool parse_ver(const char *s, int *maj, int *min, int *bld)
{
    if (!s) return false;
    return sscanf(s, "%d.%d.%d", maj, min, bld) == 3;
}

// returnt:  >0 falls a>b,  0 falls a==b,  <0 falls a<b.  Bei Parse-Fehler 0.
static int cmp_ver(const char *a, const char *b)
{
    int am, an, ab, bm, bn, bb;
    if (!parse_ver(a, &am, &an, &ab)) return 0;
    if (!parse_ver(b, &bm, &bn, &bb)) return 0;
    if (am != bm) return (am > bm) ? 1 : -1;
    if (an != bn) return (an > bn) ? 1 : -1;
    if (ab != bb) return (ab > bb) ? 1 : -1;
    return 0;
}

// ───── HTTPS-Pull-Helper ───────────────────────────────────────────────

typedef struct {
    char    buf[MAX_RESP_BYTES + 1];
    size_t  used;
    bool    overflow;
} resp_accum_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    resp_accum_t *a = (resp_accum_t *)evt->user_data;
    if (!a) return ESP_OK;
    if (a->overflow) return ESP_OK;
    if (a->used + evt->data_len > MAX_RESP_BYTES) {
        a->overflow = true;
        return ESP_OK;
    }
    memcpy(a->buf + a->used, evt->data, evt->data_len);
    a->used += evt->data_len;
    a->buf[a->used] = '\0';
    return ESP_OK;
}

// ───── Public API ──────────────────────────────────────────────────────

esp_err_t ota_check_refresh(void)
{
    mtx_ensure();

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.state    = OTA_CHECK_CHECKING;
    s_state.error[0] = '\0';
    xSemaphoreGive(s_mtx);

    resp_accum_t accum = {0};

    esp_http_client_config_t cfg = {
        .url             = OTA_CHECK_MANIFEST_URL,
        .timeout_ms      = 6000,
        .event_handler   = http_event_cb,
        .user_data       = &accum,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_state.state = OTA_CHECK_ERROR;
        snprintf(s_state.error, sizeof(s_state.error), "client_init failed");
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }

    esp_err_t e = esp_http_client_perform(cli);
    int code   = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (e != ESP_OK) {
        ESP_LOGW(TAG, "fetch failed: %s", esp_err_to_name(e));
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_state.state = OTA_CHECK_ERROR;
        snprintf(s_state.error, sizeof(s_state.error), "fetch: %s", esp_err_to_name(e));
        s_state.last_check_us = esp_timer_get_time();
        xSemaphoreGive(s_mtx);
        return e;
    }
    if (code != 200) {
        ESP_LOGW(TAG, "manifest HTTP %d", code);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_state.state = OTA_CHECK_ERROR;
        snprintf(s_state.error, sizeof(s_state.error), "HTTP %d", code);
        s_state.last_check_us = esp_timer_get_time();
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }
    if (accum.overflow || accum.used == 0) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_state.state = OTA_CHECK_ERROR;
        snprintf(s_state.error, sizeof(s_state.error),
                 accum.overflow ? "response too large" : "empty response");
        s_state.last_check_us = esp_timer_get_time();
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(accum.buf);
    if (!root) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_state.state = OTA_CHECK_ERROR;
        snprintf(s_state.error, sizeof(s_state.error), "JSON parse failed");
        s_state.last_check_us = esp_timer_get_time();
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }
    cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsString(ver) || !ver->valuestring) {
        cJSON_Delete(root);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_state.state = OTA_CHECK_ERROR;
        snprintf(s_state.error, sizeof(s_state.error), "no version field");
        s_state.last_check_us = esp_timer_get_time();
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }

    char latest[VER_STR_MAX];
    snprintf(latest, sizeof(latest), "%s", ver->valuestring);
    cJSON_Delete(root);

    int rel = cmp_ver(latest, FW_VERSION_STRING);
    ota_check_state_t new_state = (rel > 0) ? OTA_CHECK_AVAILABLE
                                            : OTA_CHECK_UP_TO_DATE;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.state = new_state;
    snprintf(s_state.latest_ver, sizeof(s_state.latest_ver), "%s", latest);
    snprintf(s_state.current_ver, sizeof(s_state.current_ver), "%s", FW_VERSION_STRING);
    s_state.error[0]      = '\0';
    s_state.last_check_us = esp_timer_get_time();
    xSemaphoreGive(s_mtx);

    ESP_LOGI(TAG, "manifest version=%s (current=%s) → %s",
             latest, FW_VERSION_STRING,
             (new_state == OTA_CHECK_AVAILABLE) ? "UPDATE_AVAILABLE" : "up-to-date");
    return ESP_OK;
}

static const char *state_name(ota_check_state_t s)
{
    switch (s) {
        case OTA_CHECK_IDLE:        return "idle";
        case OTA_CHECK_CHECKING:    return "checking";
        case OTA_CHECK_UP_TO_DATE:  return "up_to_date";
        case OTA_CHECK_AVAILABLE:   return "available";
        case OTA_CHECK_ERROR:       return "error";
    }
    return "?";
}

int ota_check_status_json(char *buf, size_t cap)
{
    mtx_ensure();

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ota_state_t snap = s_state;
    xSemaphoreGive(s_mtx);

    int age_s = -1;
    if (snap.last_check_us > 0) {
        int64_t now_us = esp_timer_get_time();
        age_s = (int)((now_us - snap.last_check_us) / 1000000);
        if (age_s < 0) age_s = 0;
    }

    int n = snprintf(buf, cap,
        "{\"state\":\"%s\","
        "\"current\":\"%s\","
        "\"available\":\"%s\","
        "\"update_available\":%s,"
        "\"checked_age_s\":%d,"
        "\"error\":\"%s\"}",
        state_name(snap.state),
        snap.current_ver,
        snap.latest_ver,
        (snap.state == OTA_CHECK_AVAILABLE) ? "true" : "false",
        age_s,
        snap.error);

    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}
