// SPDX-License-Identifier: GPL-2.0-or-later

#include "mdns_glue.h"
#include "net.h"
#include "version.h"
#include "source_usb.h"
#include "source_uart.h"
#include "bridge.h"

#include "mdns.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "mdns";

// Service-Namen entsprechen dem CULFW32/bmcond-Discovery-Schema
// (siehe CUL32-HM/bmcond/docs/sources_schema.md):
//   _raw-uart._udp    — HB-RF-ETH-UDP-Bridge (Standard, bidcos default)
//   _hmuartlgw._tcp   — Legacy-HMUARTLGW-Stream (FHEM/Homegear)
//   _http._tcp        — WebUI
//
// TXT-Records werden von bmcond ausgewertet:
//   wire=hb-rf-eth     — Wire-Protokoll-Identifier (Pflicht für UDP/3008)
//   caps=bidcos,hmip   — explizite Capability-Override
//   model=             — Hardware-Modell
//   fw / fwver=        — Firmware-Version
//   tag=               — Source-App-Tag (eq-3-Modul-Identifikation)
//   boot=yes|no        — Source-im-App-Mode
//   active=usb|uart    — welche Source aktuell die Bridge bekommt
#define HBRFETH_PORT   3008
#define RAWUART_PORT   2329
#define HMUARTLGW_PORT 2330
#define HTTP_PORT      80

static bool         s_initialized;
static char         s_last_tag[32];
static bool         s_last_boot;
static char         s_last_active[8];

// Aktive Source und ihren Tag bestimmen.  Caller stellt Buffer.
// active und tag sind (cap)-große char-Arrays; boot ist ein bool*.
static void current_source_state(char *active, size_t active_cap,
                                 char *tag,    size_t tag_cap,
                                 bool *boot_out)
{
    source_t *cur = bridge_get_source();
    const char *active_s = (cur && cur->short_id) ? cur->short_id : "none";
    snprintf(active, active_cap, "%s", active_s);

    source_usb_stats_t  us;  source_usb_get_stats(&us);
    source_uart_stats_t uts; source_uart_get_stats(&uts);

    if (cur && cur->short_id && strcmp(cur->short_id, "usb") == 0) {
        snprintf(tag, tag_cap, "%s", us.app_tag[0]  ? us.app_tag  : "(none)");
        *boot_out = us.boot_done;
    } else if (cur && cur->short_id && strcmp(cur->short_id, "uart") == 0) {
        snprintf(tag, tag_cap, "%s", uts.app_tag[0] ? uts.app_tag : "(none)");
        *boot_out = uts.boot_done;
    } else {
        snprintf(tag, tag_cap, "%s", "(none)");
        *boot_out = false;
    }
}

static void register_services(void)
{
    char fw[24];
    snprintf(fw, sizeof(fw), "v%s", FW_VERSION_STRING);

    char active[8], tag[32];
    bool boot;
    current_source_state(active, sizeof(active), tag, sizeof(tag), &boot);
    const char *boot_s = boot ? "yes" : "no";
    strlcpy(s_last_tag,    tag,    sizeof(s_last_tag));
    strlcpy(s_last_active, active, sizeof(s_last_active));
    s_last_boot = boot;

    // TXT-Set fürs UDP-Service-Bundle (für _raw-uart._udp Discovery
    // muss bmcond `wire=hb-rf-eth` + `caps=` finden, sonst Default-
    // Capabilities).  Andere Services teilen den Set außer wire/caps.
    mdns_txt_item_t txt_udp[] = {
        {"model", "RFNetHM"},
        {"fw",    fw},
        {"tag",   tag},
        {"boot",  boot_s},
        {"active",active},
        {"wire",  "hb-rf-eth"},
        {"caps",  "bidcos,hmip"},
    };
    mdns_txt_item_t txt_tcp[] = {
        {"model", "RFNetHM"},
        {"fw",    fw},
        {"tag",   tag},
        {"boot",  boot_s},
        {"active",active},
    };
    const size_t ntxt_udp = sizeof(txt_udp) / sizeof(txt_udp[0]);
    const size_t ntxt_tcp = sizeof(txt_tcp) / sizeof(txt_tcp[0]);

    // _raw-uart._udp — HB-RF-ETH-UDP-Bridge.  Service-Name vom
    // CULFW32/bmcond-Schema: bmcond's discover-Code nimmt diesen
    // Service-Namen als bidcos-Default-Source und liest TXT-Records
    // wire=hb-rf-eth + caps= als Override.
    mdns_service_add(NULL, "_raw-uart", "_udp", HBRFETH_PORT, txt_udp, ntxt_udp);
    mdns_service_instance_name_set("_raw-uart", "_udp",
                                    "RFNetHM HB-RF-ETH Bridge");

    // _rawuart._tcp — Raw-HMUARTLGW-Stream (TCP/2329).  Eigener
    // RFNETHM-Service, kein bmcond-Discovery-Pendant.
    mdns_service_add(NULL, "_rawuart", "_tcp", RAWUART_PORT, txt_tcp, ntxt_tcp);
    mdns_service_instance_name_set("_rawuart", "_tcp",
                                    "RFNetHM Raw HMUARTLGW");

    // _hmuartlgw._tcp — HM-MOD-RPI-PCB-emulierender Endpoint für
    // FHEM/Homegear.  FHEM-Define: `HMUARTLGW uart://<host>:2330`.
    mdns_service_add(NULL, "_hmuartlgw", "_tcp", HMUARTLGW_PORT, txt_tcp, ntxt_tcp);
    mdns_service_instance_name_set("_hmuartlgw", "_tcp",
                                    "RFNetHM HM-MOD-UART (FHEM/Homegear)");

    // _http._tcp — WebUI.
    mdns_service_add(NULL, "_http", "_tcp", HTTP_PORT, txt_tcp, ntxt_tcp);
    mdns_service_instance_name_set("_http", "_tcp",
                                    "RFNetHM WebUI");

    ESP_LOGI(TAG, "advertised _raw-uart._udp:%d (wire=hb-rf-eth caps=bidcos,hmip), "
                  "_rawuart._tcp:%d, _hmuartlgw._tcp:%d, _http._tcp:%d "
                  "(host %s.local, active=%s, tag=%s, boot=%s)",
             HBRFETH_PORT, RAWUART_PORT, HMUARTLGW_PORT, HTTP_PORT,
             net_hostname(), active, tag, boot_s);
}

static void refresh_txt_if_changed(void)
{
    char active[8], tag[32];
    bool boot;
    current_source_state(active, sizeof(active), tag, sizeof(tag), &boot);
    if (strcmp(tag,    s_last_tag)    == 0 &&
        strcmp(active, s_last_active) == 0 &&
        boot == s_last_boot) return;

    strlcpy(s_last_tag,    tag,    sizeof(s_last_tag));
    strlcpy(s_last_active, active, sizeof(s_last_active));
    s_last_boot = boot;
    const char *boot_s = boot ? "yes" : "no";

    const char *types[][2] = {
        {"_raw-uart",  "_udp"},
        {"_rawuart",   "_tcp"},
        {"_hmuartlgw", "_tcp"},
        {"_http",      "_tcp"},
    };
    for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); i++) {
        mdns_service_txt_item_set(types[i][0], types[i][1], "tag",    tag);
        mdns_service_txt_item_set(types[i][0], types[i][1], "boot",   boot_s);
        mdns_service_txt_item_set(types[i][0], types[i][1], "active", active);
    }
    ESP_LOGI(TAG, "TXT refresh: active=%s tag=%s boot=%s", active, tag, boot_s);
}

static void mdns_task(void *arg)
{
    (void)arg;

    // Warte bis STA tatsächlich connected ist, sonst registriert mDNS
    // sich auf ein Interface ohne IP und die Replies erreichen niemanden.
    while (!net_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    s_initialized = true;

    mdns_hostname_set(net_hostname());
    mdns_instance_name_set("RFNetHM HmIP-Bridge");

    register_services();

    // Periodisch checken ob sich der USB-Tag oder Boot-State geändert
    // hat.  Geht aus von ~1-3 BL→App-Wechseln pro Stick-Lebenszeit;
    // 5 s Polling-Intervall genügt locker.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        refresh_txt_if_changed();
    }
}

esp_err_t mdns_glue_init(void)
{
    if (s_initialized) return ESP_OK;
    BaseType_t r = xTaskCreate(mdns_task, "mdns", 4096, NULL, 4, NULL);
    return (r == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
