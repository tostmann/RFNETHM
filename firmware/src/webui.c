// SPDX-License-Identifier: GPL-2.0-or-later

#include "webui.h"
#include "version.h"
#include "bridge.h"
#include "net.h"
#include "source_usb.h"
#include "source_uart.h"
#include "sink_tcp.h"
#include "sink_hbrfeth.h"
#include "sink_hmuartlgw_legacy.h"
#include "log_buffer.h"
#include "ota_check.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_core_dump.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "webui";
static httpd_handle_t s_http;

// Cached coredump-present flag.  esp_core_dump_image_check() macht eine
// synchrone SPI-Flash-Lesung, die bei /api/status-Hochfrequenz spürbare
// Latenz + ESP-IDF-internen Log-Lärm produziert (E "Incorrect size: 1"
// pro Aufruf bei leerer/korrupter Partition).  Da Coredumps nur durch
// einen ESP-Crash neu entstehen (= Reboot, → Re-Init), reicht ein
// einziger Check pro Boot.  h_coredump_delete invalidiert den Cache nach
// erfolgreichem Erase, damit /api/status den neuen State sofort sieht.
static bool s_coredump_present = false;

// ───── Maintenance-Auth (optional Token) ────────────────────────────────
//
// Wenn ein Token in NVS gesetzt ist, verlangen alle destructive Endpoints
// einen `X-Auth-Token:`-Header mit demselben Wert.  Wenn KEIN Token in NVS
// gesetzt ist (Default-Fresh-Boot), bleiben die Endpoints offen — kompat
// zum bisherigen Verhalten, kein Lockout-Risiko.
//
// Token-Setzen geht über POST /api/auth/token mit Body
// {"token":"<neuer-string>"} — ist Auth bereits aktiv, muss der bestehende
// Token im X-Auth-Token-Header mit.  Clear: DELETE /api/auth/token (auch
// auth-geschützt wenn aktiv).
#define AUTH_NVS_NS    "rfnh_auth"
#define AUTH_NVS_TOKEN "token"
#define AUTH_TOKEN_MAX 64

static void auth_get_token(char *out, size_t cap)
{
    if (cap == 0) return;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(AUTH_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = cap;
    nvs_get_str(h, AUTH_NVS_TOKEN, out, &sz);
    nvs_close(h);
}

// Liefert ESP_OK wenn (a) kein Auth aktiv oder (b) Header matchen.
// Bei Mismatch: 401-Response gesendet, ESP_FAIL zurück.  Handler MUSS
// in dem Fall sofort returnen.
static esp_err_t require_auth_or_401(httpd_req_t *req)
{
    char saved[AUTH_TOKEN_MAX];
    auth_get_token(saved, sizeof(saved));
    if (saved[0] == '\0') return ESP_OK;   // auth nicht aktiv

    char header[AUTH_TOKEN_MAX] = {0};
    esp_err_t e = httpd_req_get_hdr_value_str(req, "X-Auth-Token",
                                              header, sizeof(header));
    if (e != ESP_OK || header[0] == '\0') {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"missing X-Auth-Token\"}", 32);
        return ESP_FAIL;
    }
    if (strcmp(header, saved) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"invalid token\"}", 25);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ───── Embedded static assets (siehe src/CMakeLists.txt EMBED_FILES) ────
//
// IDF konvertiert jedes EMBED_FILES-Element in zwei extern-Symbols
// (`_binary_<name>_start` / `_binary_<name>_end`), wobei `<name>` der
// Dateiname mit `.` → `_` ist.

extern const uint8_t index_html_gz_start[]   asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]     asm("_binary_index_html_gz_end");
extern const uint8_t app_css_gz_start[]      asm("_binary_app_css_gz_start");
extern const uint8_t app_css_gz_end[]        asm("_binary_app_css_gz_end");
extern const uint8_t app_js_gz_start[]       asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[]         asm("_binary_app_js_gz_end");
extern const uint8_t busmatic_png_start[]        asm("_binary_busmatic_png_start");
extern const uint8_t busmatic_png_end[]          asm("_binary_busmatic_png_end");
extern const uint8_t busmatic_white_png_start[]  asm("_binary_busmatic_white_png_start");
extern const uint8_t busmatic_white_png_end[]    asm("_binary_busmatic_white_png_end");

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_EXT:      return "EXT";
    case ESP_RST_SW:       return "SW";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "UNKNOWN";
    }
}

// ───── Static asset handlers (gzip pre-compressed) ──────────────────────

static esp_err_t serve_gzip(httpd_req_t *req, const char *mime,
                            const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, mime);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=300");
    return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t h_index(httpd_req_t *req)
{
    return serve_gzip(req, "text/html; charset=utf-8",
                      index_html_gz_start, index_html_gz_end);
}

static esp_err_t h_css(httpd_req_t *req)
{
    return serve_gzip(req, "text/css", app_css_gz_start, app_css_gz_end);
}

static esp_err_t h_js(httpd_req_t *req)
{
    return serve_gzip(req, "application/javascript",
                      app_js_gz_start, app_js_gz_end);
}

static esp_err_t h_logo(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    return httpd_resp_send(req, (const char *)busmatic_png_start,
                           busmatic_png_end - busmatic_png_start);
}

static esp_err_t h_logo_white(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    return httpd_resp_send(req, (const char *)busmatic_white_png_start,
                           busmatic_white_png_end - busmatic_white_png_start);
}

// Schreibt einen JSON-string-Body (ohne umgebende Anführungszeichen) escaped
// in `dst`.  `"` und `\` werden mit Backslash escaped, Steuerzeichen als
// \uXXXX.  Pflicht für Strings aus externen Quellen (SSID, HM-Modul-Tag),
// damit eine SSID mit `"` im Namen nicht das ganze JSON kaputt macht.
static void json_escape(const char *src, char *dst, size_t dst_cap)
{
    if (dst_cap == 0) return;
    if (!src) src = "";
    size_t i = 0;
    while (*src && i + 7 < dst_cap) {
        unsigned char c = (unsigned char)*src++;
        if (c == '"' || c == '\\') {
            dst[i++] = '\\';
            dst[i++] = c;
        } else if (c < 0x20) {
            int w = snprintf(dst + i, dst_cap - i, "\\u%04x", c);
            if (w < 0) break;
            i += (size_t)w;
        } else {
            dst[i++] = c;
        }
    }
    dst[i] = '\0';
}

// ───── /api/status ──────────────────────────────────────────────────────

static esp_err_t h_status(httpd_req_t *req)
{
    bridge_stats_t       bs;  bridge_get_stats(&bs);
    bridge_tx_info_t     ti;  bridge_get_tx_info(&ti);
    source_usb_stats_t   us;  source_usb_get_stats(&us);
    source_uart_stats_t  uts; source_uart_get_stats(&uts);
    sink_tcp_stats_t     ts;  sink_tcp_get_stats(&ts);
    sink_hbrfeth_stats_t hs;  sink_hbrfeth_get_stats(&hs);
    sink_hmuartlgw_legacy_stats_t ls; sink_hmuartlgw_legacy_get_stats(&ls);
    source_t *cur_src = bridge_get_source();
    const char *active_src = (cur_src && cur_src->short_id) ? cur_src->short_id : "none";

    // tx.rej — Per-Sink-Reject-Counter als kompaktes Objekt
    char rej[256] = {0};
    {
        int rp = 0;
        rp += snprintf(rej+rp, sizeof(rej)-rp, "{");
        bool first = true;
        for (size_t i = 0; i < BRIDGE_MAX_SINKS; i++) {
            if (!ti.slot_used[i] || !ti.ids[i][0]) continue;
            rp += snprintf(rej+rp, sizeof(rej)-rp, "%s\"%s\":%u",
                           first ? "" : ",", ti.ids[i],
                           (unsigned)ti.tx_rejected_lock[i]);
            first = false;
        }
        rp += snprintf(rej+rp, sizeof(rej)-rp, "}");
    }
    int64_t since_us  = ti.last_tx_us > 0 ? (esp_timer_get_time() - ti.last_tx_us) : -1;
    int64_t rx_age_us = bs.rx_last_us  > 0 ? (esp_timer_get_time() - bs.rx_last_us) : -1;

    // System-Health: min_free_heap, largest contiguous block, num tasks,
    // engste Stack-HWM (in words; <256 = WARN).
    size_t      free_now    = esp_get_free_heap_size();
    size_t      min_free    = esp_get_minimum_free_heap_size();
    size_t      largest     = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    UBaseType_t ntasks      = uxTaskGetNumberOfTasks();
    UBaseType_t tightest    = 0xFFFFFFFFu;
    char        tightest_name[configMAX_TASK_NAME_LEN + 1] = "?";
    {
        TaskStatus_t *snap = (TaskStatus_t *)pvPortMalloc(sizeof(TaskStatus_t) * ntasks);
        if (snap) {
            UBaseType_t n = uxTaskGetSystemState(snap, ntasks, NULL);
            for (UBaseType_t i = 0; i < n; i++) {
                if (snap[i].usStackHighWaterMark < tightest) {
                    tightest = snap[i].usStackHighWaterMark;
                    strncpy(tightest_name, snap[i].pcTaskName ? snap[i].pcTaskName : "?",
                            sizeof(tightest_name) - 1);
                    tightest_name[sizeof(tightest_name) - 1] = '\0';
                }
            }
            vPortFree(snap);
        } else {
            ESP_LOGW(TAG, "h_status: pvPortMalloc(%u tasks) failed — stack HWM skipped",
                     (unsigned)ntasks);
        }
        if (tightest == 0xFFFFFFFFu) tightest = 0;
    }
    bool coredump_present = s_coredump_present;

    // Externe Strings escapen — SSID kann beliebige UTF-8/Sonderzeichen
    // enthalten, app_tag kommt vom HM-Modul-Banner (validiert, aber
    // defensiv).  Buffer pro Quelle 200 B = 32 raw × 6 (worst-case \uXXXX) + NUL.
    char ssid_esc[200], usb_tag_esc[200], uart_tag_esc[200];
    json_escape(net_ssid(),  ssid_esc,     sizeof(ssid_esc));
    json_escape(us.app_tag,  usb_tag_esc,  sizeof(usb_tag_esc));
    json_escape(uts.app_tag, uart_tag_esc, sizeof(uart_tag_esc));

    char buf[3000];
    int n = snprintf(buf, sizeof(buf),
        "{"
          "\"fw\":{\"version\":\"%s\",\"built\":\"%s\"},"
          "\"net\":{\"up\":%s,\"ssid\":\"%s\",\"ip\":\"%s\","
                  "\"ap\":%s,\"host\":\"%s\"},"
          "\"src\":{\"active\":\"%s\"},"
          "\"usb\":{\"connected\":%s,\"boot\":%s,\"tag\":\"%s\","
                  "\"frames_ok\":%u,\"crc_err\":%u,\"trunc\":%u,\"skip\":%u},"
          "\"uart\":{\"present\":%s,\"boot\":%s,\"flash_lock\":%s,\"tag\":\"%s\","
                   "\"frames_ok\":%u,\"crc_err\":%u,\"trunc\":%u,\"skip\":%u},"
          "\"br\":{\"sinks\":%u,\"rx_bytes\":%u,\"rx_pumps\":%u,"
                  "\"tx_bytes\":%u,\"tx_drop\":%u,\"tx_drop_lock\":%u,"
                  "\"rx_age_ms\":%lld},"
          "\"tx\":{\"mode\":\"%s\",\"owner\":\"%s\",\"since_ms\":%lld,"
                  "\"rej_total\":%u,\"rej\":%s},"
          "\"tcp\":{\"port\":%u,\"clients\":%d,\"accepts\":%u,"
                  "\"rx\":%u,\"tx\":%u},"
          "\"hb\":{\"port\":%u,\"clients\":%d,\"connects\":%u,\"disconnects\":%u,"
                  "\"ka_timeouts\":%u,\"bad_crc\":%u,\"rx\":%u,\"tx\":%u},"
          "\"hmu\":{\"port\":%u,\"clients\":%d,\"connects\":%u,\"disconnects\":%u,"
                   "\"tx\":%u,\"ack\":%u,\"orph\":%u,\"foreign\":%u,\"rx\":%u,"
                   "\"hello\":%u,\"aes\":%u,\"reject\":%u},"
          "\"sys\":{\"uptime_s\":%u,\"free_heap\":%u,\"min_free_heap\":%u,"
                   "\"largest_block\":%u,\"tasks\":%u,"
                   "\"stack_min_name\":\"%s\",\"stack_min_words\":%u,"
                   "\"coredump\":%s,\"reset_reason\":\"%s\"}"
        "}",
        FW_VERSION_STRING, FW_BUILD_DATE,
        net_is_connected() ? "true" : "false", ssid_esc, net_ip_str(),
        net_is_ap_mode() ? "true" : "false", net_hostname(),
        active_src,
        us.stick_connected ? "true" : "false",
        us.boot_done       ? "true" : "false",
        usb_tag_esc,
        (unsigned)us.frames_ok, (unsigned)us.frames_crc_err,
        (unsigned)us.frames_truncated, (unsigned)us.bytes_skipped,
        uts.module_present ? "true" : "false",
        uts.boot_done      ? "true" : "false",
        uts.flash_lock     ? "true" : "false",
        uart_tag_esc,
        (unsigned)uts.frames_ok, (unsigned)uts.frames_crc_err,
        (unsigned)uts.frames_truncated, (unsigned)uts.bytes_skipped,
        (unsigned)bs.sink_count,
        (unsigned)bs.rx_bytes_total, (unsigned)bs.rx_pumps_total,
        (unsigned)bs.tx_bytes_total, (unsigned)bs.tx_dropped_not_ready,
        (unsigned)bs.tx_dropped_locked,
        (long long)(rx_age_us < 0 ? -1 : rx_age_us / 1000),
        ti.mode == BRIDGE_TX_PINNED ? "pinned" : "auto",
        ti.owner_id,
        (long long)(since_us < 0 ? -1 : since_us / 1000),
        (unsigned)ti.tx_rejected_lock_total, rej,
        ts.port, ts.active_clients,
        (unsigned)ts.total_accepts,
        (unsigned)ts.rx_bytes_from_clients, (unsigned)ts.tx_bytes_to_clients,
        hs.port, hs.active_clients,
        (unsigned)hs.total_connects, (unsigned)hs.total_disconnects,
        (unsigned)hs.keepalive_timeouts, (unsigned)hs.bad_crc,
        (unsigned)hs.rx_frames_from_clients, (unsigned)hs.tx_frames_to_clients,
        ls.port, ls.active_clients,
        (unsigned)ls.total_accepts, (unsigned)ls.total_disconnects,
        (unsigned)ls.app_send_translated, (unsigned)ls.llmac_acks_routed,
        (unsigned)ls.llmac_acks_orphaned, (unsigned)ls.llmac_acks_foreign,
        (unsigned)ls.llmac_recv_broadcast,
        (unsigned)ls.hello_pushes, (unsigned)ls.aes_keys_persisted,
        (unsigned)ls.app_send_rejected,
        (unsigned)(esp_log_timestamp() / 1000),
        (unsigned)free_now, (unsigned)min_free, (unsigned)largest,
        (unsigned)ntasks, tightest_name, (unsigned)tightest,
        coredump_present ? "true" : "false",
        reset_reason_str(esp_reset_reason()));

    if (n < 0 || n >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "buf overflow");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

// Minimal JSON-string-extract (no nested escapes). Returns 0 on success.
static int json_field(const char *in, const char *key, char *out, size_t out_cap)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(in, needle);
    if (!p) return -1;
    p = strchr(p + strlen(needle), ':');
    if (!p) return -1;
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (*p != '"') return -1;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_cap) {
        if (*p == '\\' && p[1]) p++;   // strip backslash escapes minimally
        out[n++] = *p++;
    }
    out[n] = '\0';
    return (*p == '"') ? 0 : -1;
}

// Bool-Field-Lookup: matched sowohl JSON-Boolean (true/false) als auch
// String-"true"/"1".  Returns false wenn key fehlt oder Wert nicht
// truthy ist.
static bool json_field_truthy(const char *in, const char *key)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(in, needle);
    if (!p) return false;
    p = strchr(p + strlen(needle), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    return strncmp(p, "true", 4) == 0 || *p == '1';
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    if (require_auth_or_401(req) != ESP_OK) return ESP_FAIL;
    char body[256];
    int  total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        total += r;
    }
    body[total] = '\0';

    char ssid[33] = {0}, pass[65] = {0};
    if (json_field(body, "ssid", ssid, sizeof(ssid)) != 0 || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_FAIL;
    }
    json_field(body, "pass", pass, sizeof(pass));   // empty pass = open net

    // Persistenz UND in-memory-Buffer (s_ssid_buf/s_pass_buf in net.c) in
    // einem Call — sonst zeigt /api/status oder net_ssid() bis zum nächsten
    // Reboot noch die alte SSID.  Returnwert ist hart geprüft, sonst
    // bekommt der User ein {"saved":true} obwohl der nvs_commit gefailt
    // ist.
    esp_err_t err = net_persist_creds(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "h_wifi: net_persist_creds failed: %s",
                 esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "persist failed");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "saved WiFi creds via WebUI (ssid='%s')", ssid);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"saved\":true}", 14);
}

static const char *authmode_str(uint8_t a)
{
    switch (a) {
    case 0:  return "open";
    case 1:  return "wep";
    case 2:  return "wpa";
    case 3:  return "wpa2";
    case 4:  return "wpa-wpa2";
    case 5:  return "wpa2-ent";
    case 6:  return "wpa3";
    case 7:  return "wpa2-wpa3";
    default: return "unknown";
    }
}

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    static net_scan_entry_t entries[24];
    size_t got = 0;
    esp_err_t err = net_scan(entries, sizeof(entries)/sizeof(entries[0]), &got);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return ESP_FAIL;
    }
    char buf[1800];
    int p = 0;
    p += snprintf(buf+p, sizeof(buf)-p, "[");
    for (size_t i = 0; i < got && p < (int)sizeof(buf)-128; i++) {
        // Escape doublequotes/backslashes minimally — wifi-SSIDs sind selten exotisch
        char ssid_esc[66] = {0};
        size_t e = 0;
        for (size_t k = 0; entries[i].ssid[k] && e < sizeof(ssid_esc)-2; k++) {
            char c = entries[i].ssid[k];
            if (c == '"' || c == '\\') ssid_esc[e++] = '\\';
            ssid_esc[e++] = c;
        }
        p += snprintf(buf+p, sizeof(buf)-p,
                      "%s{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%u,\"auth\":\"%s\"}",
                      i == 0 ? "" : ",",
                      ssid_esc, (int)entries[i].rssi,
                      (unsigned)entries[i].channel,
                      authmode_str(entries[i].authmode));
    }
    p += snprintf(buf+p, sizeof(buf)-p, "]");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, p);
}

static void delayed_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// ───── /api/auth/* — Maintenance-Token verwalten ────────────────────────

static esp_err_t h_auth_status(httpd_req_t *req)
{
    char tok[AUTH_TOKEN_MAX];
    auth_get_token(tok, sizeof(tok));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req,
        tok[0] ? "{\"enabled\":true}" : "{\"enabled\":false}",
        tok[0] ? 16 : 17);
}

static esp_err_t h_auth_token_post(httpd_req_t *req)
{
    if (require_auth_or_401(req) != ESP_OK) return ESP_FAIL;

    char body[128] = {0};
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        total += r;
    }
    body[total] = '\0';

    char newtok[AUTH_TOKEN_MAX] = {0};
    if (json_field(body, "token", newtok, sizeof(newtok)) != 0 || newtok[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing token");
        return ESP_FAIL;
    }
    nvs_handle_t h;
    if (nvs_open(AUTH_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs_open");
        return ESP_FAIL;
    }
    esp_err_t err = nvs_set_str(h, AUTH_NVS_TOKEN, newtok);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "auth: token set (len=%u)", (unsigned)strlen(newtok));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"enabled\":true,\"saved\":true}", 28);
}

static esp_err_t h_auth_token_delete(httpd_req_t *req)
{
    if (require_auth_or_401(req) != ESP_OK) return ESP_FAIL;
    nvs_handle_t h;
    if (nvs_open(AUTH_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs_open");
        return ESP_FAIL;
    }
    nvs_erase_key(h, AUTH_NVS_TOKEN);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "auth: token cleared");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"enabled\":false,\"cleared\":true}", 32);
}

static esp_err_t h_wifi_reset(httpd_req_t *req)
{
    if (require_auth_or_401(req) != ESP_OK) return ESP_FAIL;
    esp_err_t err = net_clear_creds();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs erase failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"reset\":true,\"reboot\":true}", 28);
    xTaskCreate(delayed_reboot_task, "reboot-cred", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// POST /api/source/uart/reset    body: {"hold_in_bl":true|false}
//
// Triggert HW-Reset auf der UART-Source (für bmcond/transport_rfnethm
// Flash-Workflow, siehe note_to_rfnethm_2026-05-08_bl_entry_for_flash.md
// in CUL32-HM/docs).
//
// hold_in_bl=true:  Modul bleibt im Bootloader; flash_lock-Flag setzt
//                   die Bridge fest auf UART (Supervisor swapt nicht).
// hold_in_bl=false: voller Boot-Probe-Cycle bis App-Mode verifiziert.
//
// Response: {"status":"ok|fail","tag":"<bl-tag|app-tag>"}
static esp_err_t h_source_uart_reset(httpd_req_t *req)
{
    if (require_auth_or_401(req) != ESP_OK) return ESP_FAIL;
    char body[128] = {0};
    int  total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        total += r;
    }
    body[total] = '\0';

    bool hold_in_bl = json_field_truthy(body, "hold_in_bl");

    char tag[32] = {0};
    esp_err_t err = source_uart_reset_for_flash(hold_in_bl, tag, sizeof(tag));

    char resp[128];
    int rn = snprintf(resp, sizeof(resp),
                      "{\"status\":\"%s\",\"tag\":\"%s\",\"hold_in_bl\":%s}",
                      err == ESP_OK ? "ok" : "fail",
                      tag,
                      hold_in_bl ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, resp, rn);
}

// POST /api/source/usb/reset — heute ein 501.  Der HmIP-RFUSB-Stick
// hat keinen am ESP32-Side schaltbaren RST-Pin (= USB-Bus-Reset reicht
// nicht für BL-Re-Entry, siehe pcb_basics_must_have-Memory).  Eine
// Hardware-Variante mit GPIO-VBUS-Switch könnte das später anbieten.
static esp_err_t h_source_usb_reset(httpd_req_t *req)
{
    httpd_resp_set_status(req, "501 Not Implemented");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req,
        "{\"status\":\"unsupported\",\"reason\":\"USB-Stick hat keinen GPIO-Reset am ESP32-Side\"}",
        87);
}

// POST /api/bridge/master  body: {"sink":"hbrfeth"|"rawuart"|"hmu"|"auto"}
static esp_err_t h_bridge_master(httpd_req_t *req)
{
    char body[128] = {0};
    int  total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        total += r;
    }
    body[total] = '\0';

    char sink_id[BRIDGE_SINK_ID_MAXLEN] = {0};
    if (json_field(body, "sink", sink_id, sizeof(sink_id)) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing sink");
        return ESP_FAIL;
    }
    esp_err_t err = bridge_set_tx_master(sink_id);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown sink id");
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set failed");
        return ESP_FAIL;
    }
    bridge_tx_info_t ti; bridge_get_tx_info(&ti);
    char resp[96];
    int rn = snprintf(resp, sizeof(resp),
                      "{\"ok\":true,\"mode\":\"%s\",\"owner\":\"%s\"}",
                      ti.mode == BRIDGE_TX_PINNED ? "pinned" : "auto",
                      ti.owner_id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, rn);
}

static esp_err_t h_log(httpd_req_t *req)
{
    uint32_t since = 0;
    char qs[64];
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(qs, "since", val, sizeof(val)) == ESP_OK) {
            since = (uint32_t)strtoul(val, NULL, 10);
        }
    }
    static char body[8192];
    static char lines[7000];
    uint32_t head = 0, oldest = 0;
    size_t n = log_buffer_get_since(since, lines, sizeof(lines), &head, &oldest);
    int p = snprintf(body, sizeof(body),
                     "{\"head\":%u,\"oldest\":%u,\"count\":%u,\"lines\":[%s]}",
                     (unsigned)head, (unsigned)oldest, (unsigned)n, lines);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, p);
}

// Captive-portal probe URLs → 302 zur WebUI-Root.  Nicht auf "/" relativ
// (manche Probes folgen dem nicht), sondern auf eine absolute http://-URL
// mit der aktuellen Host-IP.  Das ist genau das, was Android/iOS/Windows
// als „Captive-Portal-erkannt" interpretiert.
static esp_err_t h_captive_redirect(httpd_req_t *req)
{
    char loc[64];
    // Im AP-Mode immer auf 192.168.4.1, sonst auf die STA-IP.
    snprintf(loc, sizeof(loc), "http://%s/", net_ip_str());
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

// Wildcard-404-Handler: im AP-Mode redirecten wir alles auf "/", außerhalb
// des AP-Mode liefern wir ein normales 404 zurück.
static esp_err_t h_not_found(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (net_is_ap_mode()) {
        return h_captive_redirect(req);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return ESP_FAIL;
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    if (require_auth_or_401(req) != ESP_OK) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"rebooting\":true}", 18);
    xTaskCreate(delayed_reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ───── OTA (v0.9) — multipart/form-data not parsed, just raw octet-stream
// (POST body = firmware.bin).  WebUI does:
//   fetch('/api/ota', {method:'POST', body: file})
// We stream into the next OTA-partition and on success ask for reboot.

static esp_err_t h_ota(httpd_req_t *req)
{
    if (require_auth_or_401(req) != ESP_OK) return ESP_FAIL;

    const esp_partition_t *upd = esp_ota_get_next_update_partition(NULL);
    if (!upd) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }

    // Content-Length-Sanity: mind. 256 KB (RFNetHM ist >1 MB; ein
    // sinnvoll-knapper Lower-Bound), max. partition-size − 4 KB Headroom
    // (esp_ota_write würde sonst irgendwann mit ESP_ERR_OTA_VALIDATE_FAILED
    // brechen, hier sauberer abfangen).
    const int min_len = 256 * 1024;
    const int max_len = (int)upd->size - 0x1000;
    if (req->content_len < min_len || req->content_len > max_len) {
        ESP_LOGW(TAG, "OTA rejected: content_len=%d out of range [%d, %d]",
                 req->content_len, min_len, max_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "content_len out of range");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "OTA: writing to '%s' (size %u, content_len %d)",
             upd->label, (unsigned)upd->size, req->content_len);

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(upd, OTA_SIZE_UNKNOWN, &h);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin");
        return ESP_FAIL;
    }

    // Image-Header-Akkumulation: erste 288 Bytes =
    // esp_image_header(24) + segment_header(8) + esp_app_desc(256).
    // Bei magic-Mismatch (kein ESP-Image) oder project_name-Mismatch
    // (z.B. CULFW32-Image versehentlich hochgeladen) → abort.
    uint8_t hdr_buf[288];
    size_t  hdr_filled    = 0;
    bool    hdr_validated = false;
    char    new_version[33]      = {0};
    char    new_project[33]      = {0};

    char buf[1024];
    int  remaining = req->content_len;
    int  written   = 0;
    while (remaining > 0) {
        int chunk = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int r = httpd_req_recv(req, buf, chunk);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            esp_ota_abort(h);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv error");
            return ESP_FAIL;
        }

        // Header sammeln bis vollständig.
        if (!hdr_validated) {
            size_t take = sizeof(hdr_buf) - hdr_filled;
            if (take > (size_t)r) take = (size_t)r;
            memcpy(hdr_buf + hdr_filled, buf, take);
            hdr_filled += take;
            if (hdr_filled == sizeof(hdr_buf)) {
                const esp_app_desc_t *new_d =
                    (const esp_app_desc_t *)&hdr_buf[32];
                if (new_d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
                    esp_ota_abort(h);
                    ESP_LOGE(TAG, "OTA: bad magic 0x%08x — not an ESP image",
                             (unsigned)new_d->magic_word);
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                        "not an ESP image (magic mismatch)");
                    return ESP_FAIL;
                }
                snprintf(new_version, sizeof(new_version), "%.*s",
                         (int)sizeof(new_d->version), new_d->version);
                snprintf(new_project, sizeof(new_project), "%.*s",
                         (int)sizeof(new_d->project_name), new_d->project_name);

                const esp_app_desc_t *cur_d = esp_app_get_description();
                if (strncmp(new_d->project_name, cur_d->project_name,
                            sizeof(new_d->project_name)) != 0) {
                    esp_ota_abort(h);
                    ESP_LOGE(TAG, "OTA: project_name mismatch: "
                                  "image='%s' running='%.32s'",
                             new_project, cur_d->project_name);
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                        "project_name mismatch");
                    return ESP_FAIL;
                }
                ESP_LOGW(TAG, "OTA: image='%s' v%s (running v%.32s)",
                         new_project, new_version, cur_d->version);
                hdr_validated = true;
            }
        }

        if (esp_ota_write(h, buf, r) != ESP_OK) {
            esp_ota_abort(h);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write");
            return ESP_FAIL;
        }
        remaining -= r;
        written   += r;
    }
    if (!hdr_validated) {
        // Upload zu klein, esp_app_desc nie gelesen — defensiv.
        esp_ota_abort(h);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image too small");
        return ESP_FAIL;
    }
    if (esp_ota_end(h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_end");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(upd) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "OTA done — %d bytes — rebooting to v%s in 500 ms",
             written, new_version);

    httpd_resp_set_type(req, "application/json");
    char j[200];
    const esp_app_desc_t *cur_d = esp_app_get_description();
    int n = snprintf(j, sizeof(j),
                     "{\"ota\":\"ok\",\"bytes\":%d,"
                     "\"from\":\"%.32s\",\"to\":\"%s\"}",
                     written, cur_d->version, new_version);
    httpd_resp_send(req, j, n);
    xTaskCreate(delayed_reboot_task, "reboot-ota", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ───── /api/tasks — Stack-HWM-Übersicht für Dauerlauf-Diagnose ──────────
//
// Liefert ein JSON-Array mit allen FreeRTOS-Tasks und ihren Stack-HWM.
// uxTaskGetSystemState() benötigt CONFIG_FREERTOS_USE_TRACE_FACILITY=y.
static esp_err_t h_tasks(httpd_req_t *req)
{
    UBaseType_t ntasks = uxTaskGetNumberOfTasks();
    TaskStatus_t *snap = (TaskStatus_t *)pvPortMalloc(sizeof(TaskStatus_t) * ntasks);
    if (!snap) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    UBaseType_t n = uxTaskGetSystemState(snap, ntasks, NULL);

    static char body[3072];
    int p = 0;
    p += snprintf(body+p, sizeof(body)-p, "{\"count\":%u,\"tasks\":[", (unsigned)n);
    for (UBaseType_t i = 0; i < n && p < (int)sizeof(body) - 80; i++) {
        const char *state =
            (snap[i].eCurrentState == eRunning)   ? "run" :
            (snap[i].eCurrentState == eReady)     ? "rdy" :
            (snap[i].eCurrentState == eBlocked)   ? "blk" :
            (snap[i].eCurrentState == eSuspended) ? "sus" :
            (snap[i].eCurrentState == eDeleted)   ? "del" : "?";
        p += snprintf(body+p, sizeof(body)-p,
                      "%s{\"name\":\"%s\",\"prio\":%u,\"hwm\":%u,\"state\":\"%s\",\"core\":%d}",
                      i ? "," : "",
                      snap[i].pcTaskName ? snap[i].pcTaskName : "?",
                      (unsigned)snap[i].uxCurrentPriority,
                      (unsigned)snap[i].usStackHighWaterMark,
                      state,
#if CONFIG_FREERTOS_UNICORE
                      0
#else
                      (int)snap[i].xCoreID
#endif
                      );
    }
    p += snprintf(body+p, sizeof(body)-p, "]}");
    vPortFree(snap);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, p);
}

// ───── /api/coredump — vorhandenes Crash-Image streamen oder löschen ────
//
// GET liefert das ELF aus der coredump-Partition als
// application/octet-stream (Content-Disposition setzt einen Filename mit
// FW-Version + Zeitstempel-Hinweis, damit ein curl-Download direkt
// brauchbar ist).  DELETE löscht das Image (= erase Partition Header)
// damit der nächste Crash sauber überschrieben werden kann.
static esp_err_t h_coredump_get(httpd_req_t *req)
{
    size_t addr = 0, size = 0;
    if (!s_coredump_present) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no coredump");
        return ESP_FAIL;
    }
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image_get");
        return ESP_FAIL;
    }

    char disp[96];
    snprintf(disp, sizeof(disp),
             "attachment; filename=\"coredump-v%s.elf\"", FW_VERSION_STRING);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    // Streame in 1024-Byte-Chunks aus dem Flash.
    uint8_t buf[1024];
    size_t  off = 0;
    while (off < size) {
        size_t chunk = (size - off > sizeof(buf)) ? sizeof(buf) : (size - off);
        if (esp_flash_read(NULL, buf, addr + off, chunk) != ESP_OK) {
            ESP_LOGE(TAG, "coredump: flash_read @0x%" PRIx32 " failed",
                     (uint32_t)(addr + off));
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, chunk) != ESP_OK) {
            return ESP_FAIL;
        }
        off += chunk;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_coredump_delete(httpd_req_t *req)
{
    esp_err_t e = esp_core_dump_image_erase();
    if (e != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "erase failed: %d", (int)e);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
        return ESP_FAIL;
    }
    s_coredump_present = false;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"erased\":true}", 15);
}

// ───── /api/update/check — Online-Update-Verfügbarkeits-Check ──────────
//
// GET ohne Query: liefert den letzten gecachten Status (oder "idle" falls
// nie gechecked).  GET mit ?refresh=1 oder POST: triggert einen frischen
// HTTPS-Pull von manifest.json gegen den Public-Webflasher-Server.
// Antwort-Format siehe ota_check_status_json().

static esp_err_t h_update_check(httpd_req_t *req)
{
    bool refresh = false;
    if (req->method == HTTP_POST) {
        refresh = true;
    } else {
        char qbuf[32];
        if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
            char v[8];
            if (httpd_query_key_value(qbuf, "refresh", v, sizeof(v)) == ESP_OK
                && v[0] && v[0] != '0') {
                refresh = true;
            }
        }
    }

    if (refresh) ota_check_refresh();   // synchron, kann ~1-3s blockieren

    char body[320];
    int n = ota_check_status_json(body, sizeof(body));
    if (n < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status buffer too small");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, n);
}

// ───── init ─────────────────────────────────────────────────────────────

esp_err_t webui_init(uint16_t port)
{
    if (s_http) return ESP_OK;

    // Coredump-Check einmalig beim Init seeden — alle anschließenden
    // h_status / h_coredump_get nutzen die Cache-Variable.
    s_coredump_present = (esp_core_dump_image_check() == ESP_OK);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port    = port ? port : 80;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 26;          // 4 static + 11 api + 5 captive + headroom
    // Default 7 ist zu viel für LWIP_MAX_SOCKETS=16 wenn 3 Listener-Sinks
    // ihrerseits 4 Clients halten dürfen. 4 reicht für realistische
    // Web-UI-Last (1-3 Browser-Tabs) und lässt Reserve für die Sinks.
    // Stress-Test 2026-05-25 hat den vorigen Default-7 unter Connect-Storm
    // als HTTP-Server-Killer identifiziert.
    cfg.max_open_sockets = 4;
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;
    // IDF-Default-Stack 4096 reicht für h_ota nicht: 1024 B Recv-Buffer auf
    // dem Stack + esp_ota_write (flash-erase + verify) + lwIP-Pfad.
    // 2026-05-07 verifiziert: 4096 → PANIC mid-OTA, 8192 stabil.
    cfg.stack_size     = 8192;

    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }

    httpd_uri_t u;
    u = (httpd_uri_t){ .uri = "/",                         .method = HTTP_GET,  .handler = h_index  }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/app.css",                  .method = HTTP_GET,  .handler = h_css    }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/app.js",                   .method = HTTP_GET,  .handler = h_js     }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/assets/busmatic.png",      .method = HTTP_GET,  .handler = h_logo   }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/assets/busmatic_white.png",.method = HTTP_GET,  .handler = h_logo_white }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/status",               .method = HTTP_GET,  .handler = h_status }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/wifi",                 .method = HTTP_POST, .handler = h_wifi       }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/wifi/scan",            .method = HTTP_GET,  .handler = h_wifi_scan  }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/wifi/reset",           .method = HTTP_POST, .handler = h_wifi_reset }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/log",                  .method = HTTP_GET,  .handler = h_log        }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/bridge/master",        .method = HTTP_POST, .handler = h_bridge_master }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/source/uart/reset",    .method = HTTP_POST, .handler = h_source_uart_reset }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/source/usb/reset",     .method = HTTP_POST, .handler = h_source_usb_reset  }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/reboot",               .method = HTTP_POST, .handler = h_reboot     }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/ota",                  .method = HTTP_POST, .handler = h_ota        }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/tasks",                .method = HTTP_GET,    .handler = h_tasks            }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/coredump",             .method = HTTP_GET,    .handler = h_coredump_get     }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/coredump",             .method = HTTP_DELETE, .handler = h_coredump_delete  }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/update/check",         .method = HTTP_GET,    .handler = h_update_check     }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/update/check",         .method = HTTP_POST,   .handler = h_update_check     }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/auth/status",          .method = HTTP_GET,    .handler = h_auth_status      }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/auth/token",           .method = HTTP_POST,   .handler = h_auth_token_post  }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/api/auth/token",           .method = HTTP_DELETE, .handler = h_auth_token_delete }; httpd_register_uri_handler(s_http, &u);

    // Captive-Portal-Probes — Android, Apple, Windows.  Antwort = 302 → "/".
    u = (httpd_uri_t){ .uri = "/generate_204",      .method = HTTP_GET, .handler = h_captive_redirect }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/gen_204",           .method = HTTP_GET, .handler = h_captive_redirect }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = h_captive_redirect }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/canonical.html",    .method = HTTP_GET, .handler = h_captive_redirect }; httpd_register_uri_handler(s_http, &u);
    u = (httpd_uri_t){ .uri = "/ncsi.txt",          .method = HTTP_GET, .handler = h_captive_redirect }; httpd_register_uri_handler(s_http, &u);
    httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, h_not_found);

    ESP_LOGI(TAG, "WebUI on http://%s:%u/", net_ip_str(), cfg.server_port);
    return ESP_OK;
}
