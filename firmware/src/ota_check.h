// SPDX-License-Identifier: GPL-2.0-or-later
//
// ota_check — Online-Update-Verfügbarkeits-Check für RFNETHM.
//
// Fetcht periodisch (oder on-demand via GET /api/update/check) die
// manifest.json vom Public-Webflasher-Server (install.busware.de/rfnethm)
// und vergleicht die dort hinterlegte Release-Version mit der lokal
// laufenden FW_VERSION_STRING.  Status ist thread-safe abrufbar als
// JSON-Snapshot.
//
// Bewusst KEIN auto-install — der eigentliche Flash bleibt im
// existierenden POST /api/ota (User lädt firmware_rfnethm_esp32s3.bin
// vom Webflasher und postet es).  Ein "Update verfügbar"-Badge in der
// WebUI reicht; auto-install wird in einer späteren Iteration
// betrachtet.

#ifndef OTA_CHECK_H
#define OTA_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_CHECK_IDLE      = 0,  // nie gechecked
    OTA_CHECK_CHECKING  = 1,  // fetch läuft
    OTA_CHECK_UP_TO_DATE = 2, // available <= current
    OTA_CHECK_AVAILABLE = 3,  // available > current
    OTA_CHECK_ERROR     = 4,  // fetch oder parse fehlgeschlagen
} ota_check_state_t;

// One-shot online-Refresh.  Macht synchron einen HTTPS-Pull der
// manifest.json, parsed das `version`-Feld, vergleicht.  Setzt den
// internen state-Snapshot und returnt ESP_OK bei Erfolg.
//
// Blockiert für ~1-3 s typisch.  Web-Handler ruft das synchron auf,
// das ist ok bei recv_wait_timeout=30.
esp_err_t ota_check_refresh(void);

// Schreibt einen JSON-Snapshot in den Caller-Buffer:
//   {"state":"available","current":"0.14.138","available":"0.14.140",
//    "checked_age_s":12,"error":""}
// Returnt die Anzahl geschriebener Bytes (excl. NUL), oder -1 wenn
// der Buffer zu klein war.
int ota_check_status_json(char *buf, size_t cap);

// Manifest-URL für den Pull.  Default: https://install.busware.de/rfnethm/manifest.json
// Kann später zur Laufzeit überschreibbar gemacht werden (z.B. NVS-Setting).
#define OTA_CHECK_MANIFEST_URL "https://install.busware.de/rfnethm/manifest.json"

#ifdef __cplusplus
}
#endif

#endif // OTA_CHECK_H
