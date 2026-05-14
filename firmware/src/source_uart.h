// SPDX-License-Identifier: GPL-2.0-or-later
//
// source_uart.h — source_t-Implementierung für HM-MOD-RPI-PCB / RPI-RF-MOD
// am UART1 (eQ3-Pinheader-Adapter).

#ifndef RFNETHM_SOURCE_UART_H
#define RFNETHM_SOURCE_UART_H

#include "source.h"

// Initialisiert die UART-Source.  Konfiguriert UART1 (TX=GPIO17,
// RX=GPIO18 @ 115200 8N1), pulst RST (GPIO16 active-LOW), wartet
// Boot-Banner ab, schaltet ggf. BL → App via SYSTEM_START_APP, und
// pumpt danach Modul-RX-Bytes via source.rx_sink in die Bridge.
//
// Returns die source_t-Referenz für bridge_attach_source().
// NULL wenn Init nicht möglich (z.B. UART1 belegt).
source_t *source_uart_init(void);

// Optionaler Status-Snapshot.  Wenn das Modul nicht erkannt wird sind
// alle Felder nuller / app_tag leer.
typedef struct {
    uint32_t frames_ok;
    uint32_t frames_crc_err;
    uint32_t frames_truncated;
    uint32_t bytes_skipped;
    bool     module_present;     // RX-Edges seit Boot gesehen
    bool     boot_done;          // App-Mode verifiziert
    bool     flash_lock;         // Flash-Modus aktiv (Modul im BL gehalten)
    char     app_tag[32];
} source_uart_stats_t;

void source_uart_get_stats(source_uart_stats_t *out);

// HW-Reset für Flash-Workflow (siehe note_to_rfnethm_2026-05-08
// in CUL32-HM).  Pulse-RST mit gespeicherter Polarität, lese Banner-Tag.
//
// hold_in_bl=true:
//   - kein CHANGE_APP, Modul bleibt im BL
//   - S.flash_lock=true → Supervisor swapt nicht weg
//   - S.boot_done=false (informational), S.ready=true (TX-Pfad offen)
//   - RX-Fanout zur Bridge offen → bmcond kann via TCP/2329 mit BL reden
//
// hold_in_bl=false:
//   - Voller boot_probe (BL→App via tag-passenden CHANGE_APP-Pfad)
//   - S.flash_lock=false (cleared)
//   - S.boot_done=true wenn App-Mode verifiziert
//
// tag_out wird auf den letzten gesehenen Tag gesetzt (BL- oder App-Tag,
// je nach Modus + Erfolg).  ESP_OK wenn Banner+Tag erfolgreich gelesen.
esp_err_t source_uart_reset_for_flash(bool hold_in_bl,
                                      char *tag_out, size_t tag_cap);

// HW-Layer-Primitive: Pulse RST mit gespeicherter Polarität, Banner
// abwarten, Tag merken.  Setzt **kein** flash_lock — ist bewusst so weil
// piVCCU's hb_rf_eth_send_reset (cmd=4) genau ein RST-Pulse-Primitive
// ist und nicht impliziert dass der Caller flashen will.  Application-
// Layer-Flash-Lock geht über source_uart_reset_for_flash + HTTP-API.
//
// Wirkung auf interne State:
//   - S.ready bleibt true (UART-TX-Pfad bleibt offen)
//   - S.boot_done = false (Modul ist nach Pulse im BL)
//   - S.app_tag wird auf BL-Tag gesetzt (z.B. "Co_CPU_BL")
//   - S.flash_lock unverändert (separat via reset_for_flash)
//
// Verwendet von sink_hbrfeth.c im T_RESET (cmd=4)-Handler.
esp_err_t source_uart_pulse_rst_only(char *tag_out, size_t tag_cap);

// Returns true wenn aktuell im Flash-Mode (Modul im BL gehalten).
bool source_uart_is_flash_locked(void);

#endif // RFNETHM_SOURCE_UART_H
