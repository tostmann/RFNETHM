// SPDX-License-Identifier: GPL-2.0-or-later
//
// net.h — WiFi-STA-Initialisierung für RFNETHM v0.6+.
//
// v0.10: Infrastruktur (NVS, netif, event-loop, esp_wifi_init+set_mode)
// wird IMMER initialisiert, damit Improv-Serial bei Bedarf einsteigen
// kann.  Nur das esp_wifi_set_config + start passiert, wenn Credentials
// vorhanden sind.  Improv-Backend ruft start() selber, wenn es einen
// tryConnect() macht.
//
// Credentials kommen primär aus NVS (`namespace="rfnethm", key="wifi_ssid"
// + "wifi_pass"`).  Build-time-Macros (`-DWIFI_SSID=...` / `-DWIFI_PASS=...`)
// überschreiben NVS, wenn gesetzt.  Sind beide leer → STA bleibt im
// Idle-Mode bis Improv-Serial provisioniert.

#ifndef RFNETHM_NET_H
#define RFNETHM_NET_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialisiert NVS, TCPIP-Stack, Event-Loop, esp_wifi_init+set_mode(STA),
// registriert die Event-Handler.  Wenn Credentials da sind: zusätzlich
// set_config + start (Auto-Connect läuft).  Wenn keine Credentials:
// alles ist bereit für Improv-Serial-Provisionierung.  Returns ESP_OK
// in beiden Fällen.
esp_err_t net_init(void);

// True wenn STA-Verbindung aktiv und IP gebunden.
bool net_is_connected(void);

// Aktuelle IPv4-Adresse als String "x.x.x.x" (oder "0.0.0.0" wenn nicht
// connected).  Buffer wird intern bereitgestellt und ist nur bis zum
// nächsten Aufruf gültig.
const char *net_ip_str(void);

// SSID-String den wir gerade nutzen (oder "(none)") — für Status-UI.
const char *net_ssid(void);

// Persistiert Credentials im NVS-Namespace `rfnethm` (Keys `wifi_ssid` /
// `wifi_pass`).  Wird vom Improv-Glue nach erfolgreicher Provisionierung
// aufgerufen, sodass der nächste Boot ohne Improv-Window auskommt.
// Buffers werden in lokale Strings kopiert.
esp_err_t net_persist_creds(const char *ssid, const char *psk);

// Wenn true, unterdrückt der STA_DISCONNECTED-Handler den
// `esp_wifi_connect()`-Auto-Reconnect.  Wird vom Improv-Glue benutzt,
// damit unser net.c während Improv's tryConnect() nicht reinpfuscht.
void net_set_external_control(bool on);

// ── Captive-AP-Fallback (v0.12) ─────────────────────────────────────────
// Wenn keine Creds da sind oder STA nach MAX_RETRY auf gibt, startet
// `net_start_softap()` einen offenen AP namens "RFNetHM XXXX" (XXXX =
// letzte 4 Hex-Stellen der Base-MAC) plus einen Mini-DNS-Responder, der
// jeden A-Query auf die AP-IP (192.168.4.1) zurückgibt — Captive-Portal-
// Standardpattern.  Idempotent.
esp_err_t net_start_softap(void);

// True wenn der SoftAP gerade aktiv ist.
bool net_is_ap_mode(void);

// SSID des SoftAP (Buffer intern, nur bis nächster Aufruf gültig) oder "".
const char *net_ap_ssid(void);

// mDNS/AP-Hostname `rfnethm-XXXX` — wird unabhängig vom Mode bestimmt.
const char *net_hostname(void);

// Async-Scan auslösen + auf Result blocken (max ~6 s).  Schreibt bis zu
// `cap` Einträge in `out`, gibt die effektive Anzahl in `*got` zurück.
// `out`/`got` dürfen NULL sein für reines Triggern (selten sinnvoll).
typedef struct {
    char     ssid[33];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  authmode;   // wifi_auth_mode_t value
} net_scan_entry_t;

esp_err_t net_scan(net_scan_entry_t *out, size_t cap, size_t *got);

// Löscht NVS-Creds (wifi_ssid/wifi_pass).  Beim nächsten Boot triggert
// das den Captive-AP-Fallback.
esp_err_t net_clear_creds(void);

// True wenn `wifi_ssid` im rfnethm-NVS-Namespace gesetzt UND nicht-leer
// ist.  Wird vom Improv-Glue benutzt, um die Window-Länge zu wählen
// (30 s wenn Creds bereits da, 120 s wenn fresh-Provisioning gebraucht
// wird).
bool net_has_creds(void);

#ifdef __cplusplus
}
#endif

#endif // RFNETHM_NET_H
