// SPDX-License-Identifier: GPL-2.0-or-later
//
// improv_glue.h — Improv-Serial WiFi provisioning auf UART0 (RFNETHM).
//
// Lauscht 120 s nach Boot auf das `IMPROV`-Magic; bei Treffer
// antwortet die Lib mit Device-Info / Scan-Result / WIFI_SETTINGS.  Bei
// erfolgreicher Provisionierung werden Credentials in den
// `rfnethm`-NVS-Namespace persistiert (siehe net_persist_creds), und
// der Stick connectet ohne Reboot weiter.
//
// Voraussetzung: net_init() wurde vorher aufgerufen, damit
// esp_netif/event-loop/esp_wifi_init bereits stehen.  Lib's IDF-Backend
// erkennt das (`ESP_ERR_INVALID_STATE`-tolerant) und nutzt die laufende
// Infrastruktur.

#ifndef RFNETHM_IMPROV_GLUE_H
#define RFNETHM_IMPROV_GLUE_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Installiert UART0-Driver, instanziiert ImprovWiFi, spawnt RX-Task.
// Idempotent — Mehrfach-Aufrufe sind no-op.
esp_err_t improv_init(void);

// True, solange das Improv-Window noch offen ist.
bool      improv_is_armed(void);

// Verbleibende ms im Window — 0 nachdem es geschlossen ist.
uint32_t  improv_window_remaining_ms(void);

#ifdef __cplusplus
}
#endif

#endif // RFNETHM_IMPROV_GLUE_H
