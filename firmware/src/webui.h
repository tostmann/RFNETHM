// SPDX-License-Identifier: GPL-2.0-or-later
//
// webui.h — minimaler HTTP-Status-Server für RFNETHM v0.8.
//
// Endpoints:
//   GET /              → embedded single-page HTML+JS Dashboard
//   GET /api/status    → JSON: bridge, source-USB, sink-TCP, sink-HBRFETH,
//                              netif, fw-version, uptime
//   POST /api/wifi     → JSON {"ssid":"…","pass":"…"} → NVS save,
//                        Antwort {"saved":true} — restart muss der User
//                        manuell anstoßen
//   POST /api/reboot   → 200 + restart in 500ms
//
// v0.9 erweitert um:
//   POST /api/ota      → multipart/octet-stream — schreibt firmware.bin
//                        in passive OTA-Partition + esp_ota_set_boot

#ifndef RFNETHM_WEBUI_H
#define RFNETHM_WEBUI_H

#include "esp_err.h"

esp_err_t webui_init(uint16_t port);

#endif // RFNETHM_WEBUI_H
