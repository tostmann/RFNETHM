// SPDX-License-Identifier: GPL-2.0-or-later
//
// source_usb.h — source_t-Implementierung für HmIP-RFUSB am USB-Host-Port.

#ifndef RFNETHM_SOURCE_USB_H
#define RFNETHM_SOURCE_USB_H

#include "source.h"

// Initialisiert die globale USB-Source.  Installiert intern den ESP-IDF
// USB-Host-Stack + cdc_acm_host und startet einen Task der auf Stick-
// Connect/Disconnect reagiert, CP210x-Init durchführt, Mode-aware Boot-
// Probe ausführt und danach Bytes über source.rx_sink in die Bridge
// fanned.  Returns die source_t-Referenz die per bridge_attach_source()
// registriert werden soll.
source_t *source_usb_init(void);

// Optionaler Hook: gibt an wieviele Frames der HMU-Decoder bisher
// gesehen hat (debug-only).  Bei keiner aktiven Verbindung == 0.
typedef struct {
    uint32_t frames_ok;
    uint32_t frames_crc_err;
    uint32_t frames_truncated;
    uint32_t bytes_skipped;
    bool     stick_connected;
    bool     boot_done;
    char     app_tag[32];
} source_usb_stats_t;

void source_usb_get_stats(source_usb_stats_t *out);

#endif // RFNETHM_SOURCE_USB_H
