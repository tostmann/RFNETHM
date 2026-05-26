// SPDX-License-Identifier: GPL-2.0-or-later

#include "mdns_glue.h"
#include "net.h"
#include "version.h"
#include "source_usb.h"
#include "source_uart.h"
#include "bridge.h"

#include "mdns.h"
#include "esp_log.h"
#include "esp_netif.h"
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
//   model=RFNetHM      — Carrier-Board (immer dasselbe, das ist *unsere* HW)
//   radio=             — Radio-Modul aus dem App-Tag abgeleitet (s.u.)
//   fw / fwver=        — Firmware-Version
//   tag=               — Source-App-Tag (eq-3-Modul-Identifikation, roh)
//   boot=yes|no        — Source-im-App-Mode
//   active=usb|uart    — welche Source aktuell die Bridge bekommt
//
// `radio=` ist ehrliche Aussage ohne Anmaßung: Co_CPU* identifiziert das
// HM-MOD-RPI-PCB eindeutig, DualCoPro* könnte RPI-RF-MOD oder HmIP-RFUSB
// sein — wir advertisen dann "DualCoPro" als Familienname, nicht ein
// konkretes Modul.  Disambiguierung via SGTIN-Probe wäre Variante 3.
#define HBRFETH_PORT   3008
#define RAWUART_PORT   2329
#define HMUARTLGW_PORT 2330
#define HTTP_PORT      80

// Zusätzlicher kurzer Hostname-Alias: `rfnethm.local` neben dem
// eindeutigen `rfnethm-XXXX.local`.  Realistisch wird nur ein Stick
// pro Netz betrieben; bei mehreren bleibt die eindeutige Form der
// stabile Pfad und `rfnethm.local` ist je nach Race nicht-eindeutig
// (siehe README).
#define SHORT_ALIAS_HOST "rfnethm"

static bool         s_initialized;
static char         s_last_tag[32];
static bool         s_last_boot;
static char         s_last_active[8];
static char         s_last_radio[24];
static bool         s_alias_added;
static uint32_t     s_alias_ip;

// App-Tag → friendly Radio-Module-Name fürs `radio=`-TXT-Item.
// Mapping deckt die in firmware/src/source_uart.c:253-260 verwendete
// Familien-Klassifikation ab und folgt der Tag-Whitelist aus
// hmip-copro-update.jar (vgl. CUL32-HM/bmcond/src/copro_query.c:404-409).
static const char *radio_from_tag(const char *tag)
{
    if (!tag || !*tag || strcmp(tag, "(none)") == 0)
        return "unknown";
    // Co_CPU_BL / Co_CPU_App → einzige bekannte Hardware: HM-MOD-RPI-PCB
    if (strncmp(tag, "Co_CPU", 6) == 0)        return "HM-MOD-RPI-PCB";
    // DualCoPro_App / DualCoPro_Bl → RPI-RF-MOD oder HmIP-RFUSB,
    // aus dem Tag allein nicht unterscheidbar.
    if (strncmp(tag, "DualCoPro", 9) == 0)     return "DualCoPro";
    // HMIP_TRX_Bl / HMIP_TRX_App / HMIP_TRX_App_prod → HmIP-RFUSB-Familie
    if (strncmp(tag, "HMIP_TRX", 8) == 0)      return "HMIP_TRX";
    // HMIPW_* (Wired-Varianten — unwahrscheinlich am RFNETHM, aber whitelisted)
    if (strncmp(tag, "HMIPW_", 6) == 0)        return "HMIPW";
    return "unknown";
}

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

// Helper: service-add + instance-name in einem Schritt; Fehler werden
// einmal pro Fail geloggt statt silent verschluckt.
static void add_service(const char *type, const char *proto, uint16_t port,
                        const mdns_txt_item_t *txt, size_t ntxt,
                        const char *instance)
{
    esp_err_t err = mdns_service_add(NULL, type, proto, port, txt, ntxt);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_service_add(%s%s:%u) failed: %s",
                 type, proto, (unsigned)port, esp_err_to_name(err));
        return;
    }
    err = mdns_service_instance_name_set(type, proto, instance);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_service_instance_name_set(%s%s) failed: %s",
                 type, proto, esp_err_to_name(err));
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
    const char *radio  = radio_from_tag(tag);
    strlcpy(s_last_tag,    tag,    sizeof(s_last_tag));
    strlcpy(s_last_active, active, sizeof(s_last_active));
    strlcpy(s_last_radio,  radio,  sizeof(s_last_radio));
    s_last_boot = boot;

    // TXT-Set fürs UDP-Service-Bundle (für _raw-uart._udp Discovery
    // muss bmcond `wire=hb-rf-eth` + `caps=` finden, sonst Default-
    // Capabilities).  Andere Services teilen den Set außer wire/caps.
    mdns_txt_item_t txt_udp[] = {
        {"model", "RFNetHM"},
        {"radio", radio},
        {"fw",    fw},
        {"tag",   tag},
        {"boot",  boot_s},
        {"active",active},
        {"wire",  "hb-rf-eth"},
        {"caps",  "bidcos,hmip"},
    };
    mdns_txt_item_t txt_tcp[] = {
        {"model", "RFNetHM"},
        {"radio", radio},
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
    add_service("_raw-uart", "_udp", HBRFETH_PORT, txt_udp, ntxt_udp,
                "RFNetHM HB-RF-ETH Bridge");

    // _rawuart._tcp — Raw-HMUARTLGW-Stream (TCP/2329).  Eigener
    // RFNETHM-Service, kein bmcond-Discovery-Pendant.
    add_service("_rawuart", "_tcp", RAWUART_PORT, txt_tcp, ntxt_tcp,
                "RFNetHM Raw HMUARTLGW");

    // _hmuartlgw._tcp — HM-MOD-RPI-PCB-emulierender Endpoint für
    // FHEM/Homegear.  FHEM-Define: `HMUARTLGW uart://<host>:2330`.
    add_service("_hmuartlgw", "_tcp", HMUARTLGW_PORT, txt_tcp, ntxt_tcp,
                "RFNetHM HM-MOD-UART (FHEM/Homegear)");

    // _http._tcp — WebUI.
    add_service("_http", "_tcp", HTTP_PORT, txt_tcp, ntxt_tcp,
                "RFNetHM WebUI");

    ESP_LOGI(TAG, "advertised _raw-uart._udp:%d (wire=hb-rf-eth caps=bidcos,hmip), "
                  "_rawuart._tcp:%d, _hmuartlgw._tcp:%d, _http._tcp:%d "
                  "(host %s.local, active=%s, tag=%s, radio=%s, boot=%s)",
             HBRFETH_PORT, RAWUART_PORT, HMUARTLGW_PORT, HTTP_PORT,
             net_hostname(), active, tag, radio, boot_s);
}

// STA-IPv4 holen.  Liefert 0 wenn netif (noch) keine Adresse hat.
static uint32_t sta_ipv4(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return 0;
    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK) return 0;
    return info.ip.addr;
}

// Delegate-Hostname `rfnethm.local` registrieren bzw. Adresse aktualisieren.
// Wir machen das ohne Conflict-Probing — bei mehreren Sticks am gleichen Netz
// antworten halt beide, der Browser landet auf einem davon.  Eindeutiger Pfad
// bleibt `rfnethm-XXXX.local`.
static void update_short_alias(void)
{
    uint32_t ip = sta_ipv4();
    if (!ip) return;
    if (s_alias_added && ip == s_alias_ip) return;

    mdns_ip_addr_t addr = {0};
    addr.addr.type      = ESP_IPADDR_TYPE_V4;
    addr.addr.u_addr.ip4.addr = ip;
    addr.next = NULL;

    esp_err_t err;
    if (!s_alias_added) {
        err = mdns_delegate_hostname_add(SHORT_ALIAS_HOST, &addr);
        if (err == ESP_OK) {
            s_alias_added = true;
            s_alias_ip    = ip;
            ESP_LOGI(TAG, "alias host %s.local → " IPSTR " published",
                     SHORT_ALIAS_HOST, IP2STR(&addr.addr.u_addr.ip4));
        } else {
            ESP_LOGW(TAG, "mdns_delegate_hostname_add(%s) failed: %s",
                     SHORT_ALIAS_HOST, esp_err_to_name(err));
        }
    } else {
        err = mdns_delegate_hostname_set_address(SHORT_ALIAS_HOST, &addr);
        if (err == ESP_OK) {
            s_alias_ip = ip;
            ESP_LOGI(TAG, "alias host %s.local → " IPSTR " updated",
                     SHORT_ALIAS_HOST, IP2STR(&addr.addr.u_addr.ip4));
        } else {
            ESP_LOGW(TAG, "mdns_delegate_hostname_set_address(%s) failed: %s",
                     SHORT_ALIAS_HOST, esp_err_to_name(err));
        }
    }
}

static void refresh_txt_if_changed(void)
{
    char active[8], tag[32];
    bool boot;
    current_source_state(active, sizeof(active), tag, sizeof(tag), &boot);
    const char *radio = radio_from_tag(tag);
    if (strcmp(tag,    s_last_tag)    == 0 &&
        strcmp(active, s_last_active) == 0 &&
        strcmp(radio,  s_last_radio)  == 0 &&
        boot == s_last_boot) return;

    strlcpy(s_last_tag,    tag,    sizeof(s_last_tag));
    strlcpy(s_last_active, active, sizeof(s_last_active));
    strlcpy(s_last_radio,  radio,  sizeof(s_last_radio));
    s_last_boot = boot;
    const char *boot_s = boot ? "yes" : "no";

    const char *types[][2] = {
        {"_raw-uart",  "_udp"},
        {"_rawuart",   "_tcp"},
        {"_hmuartlgw", "_tcp"},
        {"_http",      "_tcp"},
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); i++) {
        if (mdns_service_txt_item_set(types[i][0], types[i][1], "tag",    tag)    != ESP_OK) fails++;
        if (mdns_service_txt_item_set(types[i][0], types[i][1], "boot",   boot_s) != ESP_OK) fails++;
        if (mdns_service_txt_item_set(types[i][0], types[i][1], "active", active) != ESP_OK) fails++;
        if (mdns_service_txt_item_set(types[i][0], types[i][1], "radio",  radio)  != ESP_OK) fails++;
    }
    if (fails) {
        ESP_LOGW(TAG, "TXT refresh: %d txt_item_set call(s) failed", fails);
    }
    ESP_LOGI(TAG, "TXT refresh: active=%s tag=%s radio=%s boot=%s",
             active, tag, radio, boot_s);
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

    err = mdns_hostname_set(net_hostname());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_hostname_set('%s') failed: %s",
                 net_hostname(), esp_err_to_name(err));
    }
    err = mdns_instance_name_set("RFNetHM HmIP-Bridge");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_instance_name_set failed: %s",
                 esp_err_to_name(err));
    }

    register_services();
    update_short_alias();

    // Periodisch checken ob sich der USB-Tag oder Boot-State geändert
    // hat.  Geht aus von ~1-3 BL→App-Wechseln pro Stick-Lebenszeit;
    // 5 s Polling-Intervall genügt locker.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        refresh_txt_if_changed();
        update_short_alias();
    }
}

esp_err_t mdns_glue_init(void)
{
    if (s_initialized) return ESP_OK;
    BaseType_t r = xTaskCreate(mdns_task, "mdns", 4096, NULL, 4, NULL);
    return (r == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
