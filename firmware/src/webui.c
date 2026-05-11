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

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "webui";
static httpd_handle_t s_http;

#define NVS_NS    "rfnethm"
#define NVS_SSID  "wifi_ssid"
#define NVS_PASS  "wifi_pass"

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
extern const uint8_t busmatic_png_start[]    asm("_binary_busmatic_png_start");
extern const uint8_t busmatic_png_end[]      asm("_binary_busmatic_png_end");

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
    int64_t since_us = ti.last_tx_us > 0 ? (esp_timer_get_time() - ti.last_tx_us) : -1;

    char buf[2600];
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
                  "\"tx_bytes\":%u,\"tx_drop\":%u,\"tx_drop_lock\":%u},"
          "\"tx\":{\"mode\":\"%s\",\"owner\":\"%s\",\"since_ms\":%lld,"
                  "\"rej_total\":%u,\"rej\":%s},"
          "\"tcp\":{\"port\":%u,\"clients\":%d,\"accepts\":%u,"
                  "\"rx\":%u,\"tx\":%u},"
          "\"hb\":{\"port\":%u,\"clients\":%d,\"connects\":%u,\"disconnects\":%u,"
                  "\"ka_timeouts\":%u,\"bad_crc\":%u,\"rx\":%u,\"tx\":%u},"
          "\"hmu\":{\"port\":%u,\"clients\":%d,\"connects\":%u,\"disconnects\":%u,"
                   "\"tx\":%u,\"ack\":%u,\"orph\":%u,\"rx\":%u,"
                   "\"hello\":%u,\"aes\":%u,\"reject\":%u},"
          "\"sys\":{\"uptime_s\":%u,\"free_heap\":%u,\"reset_reason\":\"%s\"}"
        "}",
        FW_VERSION_STRING, FW_BUILD_DATE,
        net_is_connected() ? "true" : "false", net_ssid(), net_ip_str(),
        net_is_ap_mode() ? "true" : "false", net_hostname(),
        active_src,
        us.stick_connected ? "true" : "false",
        us.boot_done       ? "true" : "false",
        us.app_tag,
        (unsigned)us.frames_ok, (unsigned)us.frames_crc_err,
        (unsigned)us.frames_truncated, (unsigned)us.bytes_skipped,
        uts.module_present ? "true" : "false",
        uts.boot_done      ? "true" : "false",
        uts.flash_lock     ? "true" : "false",
        uts.app_tag,
        (unsigned)uts.frames_ok, (unsigned)uts.frames_crc_err,
        (unsigned)uts.frames_truncated, (unsigned)uts.bytes_skipped,
        (unsigned)bs.sink_count,
        (unsigned)bs.rx_bytes_total, (unsigned)bs.rx_pumps_total,
        (unsigned)bs.tx_bytes_total, (unsigned)bs.tx_dropped_not_ready,
        (unsigned)bs.tx_dropped_locked,
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
        (unsigned)ls.llmac_acks_orphaned, (unsigned)ls.llmac_recv_broadcast,
        (unsigned)ls.hello_pushes, (unsigned)ls.aes_keys_persisted,
        (unsigned)ls.app_send_rejected,
        (unsigned)(esp_log_timestamp() / 1000),
        (unsigned)esp_get_free_heap_size(),
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

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs_open");
        return ESP_FAIL;
    }
    nvs_set_str(h, NVS_SSID, ssid);
    nvs_set_str(h, NVS_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
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

static esp_err_t h_wifi_reset(httpd_req_t *req)
{
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
    const esp_partition_t *upd = esp_ota_get_next_update_partition(NULL);
    if (!upd) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "OTA: writing to '%s' (size %u)", upd->label, (unsigned)upd->size);

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(upd, OTA_SIZE_UNKNOWN, &h);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin");
        return ESP_FAIL;
    }

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
        if (esp_ota_write(h, buf, r) != ESP_OK) {
            esp_ota_abort(h);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write");
            return ESP_FAIL;
        }
        remaining -= r;
        written   += r;
    }
    if (esp_ota_end(h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_end");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(upd) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "OTA done — %d bytes — rebooting in 500 ms", written);

    httpd_resp_set_type(req, "application/json");
    char j[80];
    int n = snprintf(j, sizeof(j), "{\"ota\":\"ok\",\"bytes\":%d}", written);
    httpd_resp_send(req, j, n);
    xTaskCreate(delayed_reboot_task, "reboot-ota", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ───── init ─────────────────────────────────────────────────────────────

esp_err_t webui_init(uint16_t port)
{
    if (s_http) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port    = port ? port : 80;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 22;          // 4 static + 8 api + 5 captive + headroom
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
