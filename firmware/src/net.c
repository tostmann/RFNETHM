// SPDX-License-Identifier: GPL-2.0-or-later

#include "net.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "net";

#define NET_NVS_NS    "rfnethm"
#define NET_NVS_SSID  "wifi_ssid"
#define NET_NVS_PASS  "wifi_pass"

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

#define BIT_GOT_IP    BIT0
#define BIT_FAIL      BIT1
#define BIT_SCAN_DONE BIT2

static EventGroupHandle_t s_eg;
static esp_netif_t       *s_sta_netif;
static esp_netif_t       *s_ap_netif;
static char               s_ssid_buf[33];   // max 32 + NUL
static char               s_pass_buf[65];   // WPA2-PSK max 64 + NUL
static char               s_ip_buf[16];
static char               s_host_buf[24];   // "rfnethm-XXXX"
static char               s_ap_ssid[33];    // "RFNetHM XXXX"
static bool               s_connected;
static bool               s_ap_active;
static int                s_retry_count;
static const int          MAX_RETRY = 8;
static bool               s_external_control;   // v0.10: Improv-managed?

static void captive_dns_task(void *arg);
static void deferred_ap_fallback_task(void *arg);
static TaskHandle_t       s_dns_task;

// ─── Helpers ──────────────────────────────────────────────────────────

static void compute_names(void)
{
    if (s_host_buf[0]) return;   // already done
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_host_buf, sizeof(s_host_buf), "rfnethm-%02X%02X",
             mac[4], mac[5]);
    snprintf(s_ap_ssid,  sizeof(s_ap_ssid),  "RFNetHM %02X%02X",
             mac[4], mac[5]);
}

static void event_cb(void* arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Improv-Backend macht selbst esp_wifi_start; in dem Fall hat es
        // noch kein wifi_config gesetzt.  Wir connecten nur, wenn wir
        // selbst Credentials haben (s_ssid_buf nicht leer) UND nicht
        // unter Improv-Kontrolle stehen.
        if (!s_external_control && s_ssid_buf[0] != '\0') {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_external_control) {
            // Improv hat das Sagen — wir mischen uns nicht ein.
            return;
        }
        if (s_retry_count < MAX_RETRY && s_ssid_buf[0] != '\0') {
            s_retry_count++;
            ESP_LOGW(TAG, "STA disconnected — retry %d/%d", s_retry_count, MAX_RETRY);
            esp_wifi_connect();
        } else if (s_ssid_buf[0] != '\0') {
            xEventGroupSetBits(s_eg, BIT_FAIL);
            ESP_LOGE(TAG, "STA gave up after %d retries — falling back to SoftAP", MAX_RETRY);
            net_start_softap();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        s_connected = true;
        s_retry_count = 0;
        snprintf(s_ip_buf, sizeof(s_ip_buf), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP %s", s_ip_buf);
        if (s_eg) xEventGroupSetBits(s_eg, BIT_GOT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (s_eg) xEventGroupSetBits(s_eg, BIT_SCAN_DONE);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *ev = data;
        ESP_LOGI(TAG, "AP client joined: " MACSTR, MAC2STR(ev->mac));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *ev = data;
        ESP_LOGI(TAG, "AP client left:   " MACSTR, MAC2STR(ev->mac));
    }
}

static esp_err_t load_creds(void)
{
    // Build-time macros take precedence.
    if (sizeof(WIFI_SSID) > 1) {
        snprintf(s_ssid_buf, sizeof(s_ssid_buf), "%s", WIFI_SSID);
        snprintf(s_pass_buf, sizeof(s_pass_buf), "%s", WIFI_PASS);
        ESP_LOGI(TAG, "creds from build-time macro (SSID='%s')", s_ssid_buf);
        return ESP_OK;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t sz_s = sizeof(s_ssid_buf);
    size_t sz_p = sizeof(s_pass_buf);
    err = nvs_get_str(h, NET_NVS_SSID, s_ssid_buf, &sz_s);
    if (err == ESP_OK) err = nvs_get_str(h, NET_NVS_PASS, s_pass_buf, &sz_p);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "creds from NVS (SSID='%s')", s_ssid_buf);
    return err;
}

esp_err_t net_init(void)
{
    snprintf(s_ip_buf,   sizeof(s_ip_buf),   "0.0.0.0");
    s_ssid_buf[0] = '\0';
    s_pass_buf[0] = '\0';

    // NVS — used by WiFi-driver and by us for credential storage.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else if (err != ESP_OK) {
        ESP_ERROR_CHECK(err);
    }

    // Try to load creds (build-time or NVS).  If nothing → s_ssid_buf empty,
    // STA bleibt im Idle bis Improv etwas zustande bringt.
    bool have_creds = (load_creds() == ESP_OK && s_ssid_buf[0] != '\0');

    s_eg = xEventGroupCreate();

    // Infrastruktur IMMER hochfahren — Improv-Backend ist sonst gezwungen,
    // sich selbst um esp_netif/event-loop zu kümmern und das ergibt
    // schwer-debugbare Race-Conditions wenn unsere Handler später noch
    // dazukommen.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &event_cb, NULL));

    compute_names();
    esp_netif_set_hostname(s_sta_netif, s_host_buf);

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));   // no power-save → low-latency

    if (have_creds) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        wifi_config_t wc = { 0 };
        size_t sl = strlen(s_ssid_buf);
        if (sl > sizeof(wc.sta.ssid)) sl = sizeof(wc.sta.ssid);
        memcpy(wc.sta.ssid, s_ssid_buf, sl);
        size_t pl = strlen(s_pass_buf);
        if (pl > sizeof(wc.sta.password)) pl = sizeof(wc.sta.password);
        memcpy(wc.sta.password, s_pass_buf, pl);
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   // open OR WPA2 — kein Block
        wc.sta.pmf_cfg.capable    = true;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "STA starting, SSID='%s' (host='%s')", s_ssid_buf, s_host_buf);
    } else {
        // Kein NVS-Cred: STA-Stack hochfahren (Improv kann ihn nutzen),
        // aber kein wifi_config setzen.  Parallel läuft ein Deferred-Task
        // der nach Ablauf des Improv-Windows den Captive-AP startet, falls
        // immer noch keine Verbindung steht.
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGW(TAG, "no WiFi creds — STA idle, awaiting Improv (deferred AP fallback in 130 s)");
        xTaskCreate(deferred_ap_fallback_task, "ap_fallback", 3072, NULL, 4, NULL);
    }
    return ESP_OK;
}

static void deferred_ap_fallback_task(void *arg)
{
    // Improv-Window ist 120 s — nach 130 s gucken, ob die Lib oder die
    // WebUI in der Zwischenzeit Creds gesetzt + connected hat.  Wenn
    // nicht: AP-Fallback hochziehen, damit der User über die Captive-
    // SSID an die WebUI rankommt.
    vTaskDelay(pdMS_TO_TICKS(130000));
    if (!s_connected && !s_ap_active) {
        ESP_LOGW(TAG, "deferred AP fallback fires — Improv produced no connection");
        net_start_softap();
    }
    vTaskDelete(NULL);
}

bool net_is_connected(void)        { return s_connected; }
const char *net_ip_str(void)       {
    if (s_ap_active && !s_connected) return "192.168.4.1";
    return s_ip_buf;
}
const char *net_ssid(void)         {
    if (s_ap_active && !s_connected) return s_ap_ssid;
    return s_ssid_buf[0] ? s_ssid_buf : "(none)";
}
const char *net_hostname(void)     { compute_names(); return s_host_buf; }
bool net_is_ap_mode(void)          { return s_ap_active; }
const char *net_ap_ssid(void)      { compute_names(); return s_ap_ssid; }

esp_err_t net_persist_creds(const char *ssid, const char *psk)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NET_NVS_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, NET_NVS_PASS, psk ? psk : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        // In-memory-Kopie aktualisieren, damit auch ohne Reboot net_ssid()
        // den richtigen Wert zurückgibt.
        snprintf(s_ssid_buf, sizeof(s_ssid_buf), "%s", ssid);
        snprintf(s_pass_buf, sizeof(s_pass_buf), "%s", psk ? psk : "");
        ESP_LOGI(TAG, "creds persisted: SSID='%s'", s_ssid_buf);
    } else {
        ESP_LOGE(TAG, "persist failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t net_clear_creds(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, NET_NVS_SSID);
    nvs_erase_key(h, NET_NVS_PASS);
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        s_ssid_buf[0] = '\0';
        s_pass_buf[0] = '\0';
        ESP_LOGW(TAG, "creds cleared — next boot enters AP fallback");
    }
    return err;
}

void net_set_external_control(bool on)
{
    s_external_control = on;
    ESP_LOGI(TAG, "external control %s", on ? "ON (Improv)" : "OFF");
}

bool net_has_creds(void)
{
    // Build-time-Macros überschreiben NVS (siehe oben — WIFI_SSID hat
    // Fallback "" wenn nicht gesetzt; sizeof > 1 = real-string).
    if (sizeof(WIFI_SSID) > 1 && WIFI_SSID[0] != '\0') return true;
    nvs_handle_t h;
    if (nvs_open(NET_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char buf[33] = {0};
    size_t sz = sizeof(buf);
    esp_err_t err = nvs_get_str(h, NET_NVS_SSID, buf, &sz);
    nvs_close(h);
    return (err == ESP_OK && buf[0] != '\0');
}

// ─── SoftAP fallback ──────────────────────────────────────────────────

esp_err_t net_start_softap(void)
{
    if (s_ap_active) return ESP_OK;

    compute_names();
    ESP_LOGW(TAG, "starting captive AP '%s' (192.168.4.1)", s_ap_ssid);

    // STA bleibt mit drin — APSTA erlaubt scan() während AP läuft, was wir
    // für /api/wifi/scan brauchen.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t apc = { 0 };
    size_t sl = strlen(s_ap_ssid);
    if (sl > sizeof(apc.ap.ssid)) sl = sizeof(apc.ap.ssid);
    memcpy(apc.ap.ssid, s_ap_ssid, sl);
    apc.ap.ssid_len      = sl;
    apc.ap.channel       = 6;
    apc.ap.authmode      = WIFI_AUTH_OPEN;     // offener AP — Captive-Portal-Pattern
    apc.ap.max_connection = 4;
    apc.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apc));
    // esp_wifi_start() ist idempotent.  Falls STA schon lief, hängt der AP
    // einfach dran an.
    esp_wifi_start();

    s_ap_active = true;

    if (!s_dns_task) {
        xTaskCreate(captive_dns_task, "cap_dns", 3072, NULL, 4, &s_dns_task);
    }
    return ESP_OK;
}

// ─── Async-Scan via esp_wifi_scan_start ───────────────────────────────

esp_err_t net_scan(net_scan_entry_t *out, size_t cap, size_t *got)
{
    if (got) *got = 0;
    xEventGroupClearBits(s_eg, BIT_SCAN_DONE);

    wifi_scan_config_t sc = { 0 };
    sc.show_hidden = false;
    sc.scan_type   = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 100;
    sc.scan_time.active.max = 250;

    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start failed: %s", esp_err_to_name(err));
        return err;
    }

    // Wait up to 8 s for SCAN_DONE.
    EventBits_t b = xEventGroupWaitBits(s_eg, BIT_SCAN_DONE,
                                        pdTRUE, pdFALSE,
                                        pdMS_TO_TICKS(8000));
    if (!(b & BIT_SCAN_DONE)) {
        ESP_LOGW(TAG, "scan timed out");
        esp_wifi_scan_stop();
        return ESP_ERR_TIMEOUT;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0 || cap == 0 || !out) {
        if (got) *got = 0;
        return ESP_OK;
    }
    uint16_t fetch = (n > cap) ? cap : n;
    wifi_ap_record_t *recs = calloc(fetch, sizeof(*recs));
    if (!recs) return ESP_ERR_NO_MEM;
    err = esp_wifi_scan_get_ap_records(&fetch, recs);
    if (err == ESP_OK) {
        for (size_t i = 0; i < fetch; i++) {
            strlcpy(out[i].ssid, (char *)recs[i].ssid, sizeof(out[i].ssid));
            out[i].rssi     = recs[i].rssi;
            out[i].channel  = recs[i].primary;
            out[i].authmode = (uint8_t)recs[i].authmode;
        }
        if (got) *got = fetch;
    }
    free(recs);
    return err;
}

// ─── Tiny captive-portal DNS responder ────────────────────────────────
//
// Bindet UDP/53 auf 0.0.0.0, beantwortet jeden eingehenden Query (egal
// welche Domain) mit einer A-Antwort auf 192.168.4.1.  Reicht aus, damit
// Captive-Portal-Detector wie `connectivitycheck.gstatic.com`,
// `captive.apple.com`, `www.msftconnecttest.com` etc. den Klienten in
// unsere WebUI weiterleiten.  Strikt minimal — nur Header durchreichen,
// Question-Section spiegeln, eine A-Answer mit TTL=60 anhängen.

static void captive_dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "captive_dns: socket() failed");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in baddr = { 0 };
    baddr.sin_family = AF_INET;
    baddr.sin_port   = htons(53);
    baddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&baddr, sizeof(baddr)) < 0) {
        ESP_LOGE(TAG, "captive_dns: bind() failed");
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "captive_dns: listening on UDP/53");

    uint8_t buf[256];
    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (n < 12) continue;   // DNS-Header is 12 B

        // Set QR=1 (response), AA=1, RA=1.
        buf[2] = 0x84;
        buf[3] = 0x80;
        // ANCOUNT = 1
        buf[6] = 0; buf[7] = 1;
        // NSCOUNT/ARCOUNT zero
        buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 0;

        // Find end of QNAME (zero byte) starting at offset 12,
        // then skip QTYPE (2) + QCLASS (2).
        int p = 12;
        while (p < n && buf[p] != 0) {
            if (buf[p] >= 0xc0) { p += 2; goto qclass; }   // pointer
            p += 1 + buf[p];
        }
        if (p >= n) continue;
        p++;   // skip the terminating 0
qclass:
        if (p + 4 > (int)sizeof(buf)) continue;
        p += 4;   // QTYPE+QCLASS

        // Append answer:
        //   NAME  = c00c (pointer to QNAME at offset 12)
        //   TYPE  = 0001 (A)
        //   CLASS = 0001 (IN)
        //   TTL   = 0000003c (60 s)
        //   RDLEN = 0004
        //   RDATA = 192.168.4.1
        if (p + 16 > (int)sizeof(buf)) continue;
        buf[p++] = 0xc0; buf[p++] = 0x0c;
        buf[p++] = 0x00; buf[p++] = 0x01;
        buf[p++] = 0x00; buf[p++] = 0x01;
        buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x3c;
        buf[p++] = 0x00; buf[p++] = 0x04;
        buf[p++] = 192; buf[p++] = 168; buf[p++] = 4; buf[p++] = 1;

        sendto(sock, buf, p, 0, (struct sockaddr *)&src, slen);
    }
}
