// SPDX-License-Identifier: GPL-2.0-or-later
//
// log_buffer.h — RAM-Ringbuffer für ESP_LOG / printf-Output.  Wird via
// `esp_log_set_vprintf()` als Tee-Hook installiert (Original-Output am
// UART bleibt erhalten, parallel landet jede Zeile im Ringbuffer).
//
// `/api/log?since=<seq>` im WebUI fragt die neuen Zeilen seit der
// letzten Antwort ab; JS-Tab im Frontend pollt im Sekundentakt.

#ifndef RFNETHM_LOG_BUFFER_H
#define RFNETHM_LOG_BUFFER_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Installiert den vprintf-Hook und initialisiert den Ringbuffer.
// Idempotent — sicher mehrfach aufzurufen.
esp_err_t log_buffer_init(void);

// Schreibt JSON-Array-Content (Zeilen-Strings, kommasepariert, ohne
// `[` / `]`) für alle Zeilen mit Sequenznummer > `since` in `out`.
// `*head_seq`   = nächste freie Sequenznummer (== Zähler aller bisher
//                 erfassten Zeilen).
// `*oldest_seq` = älteste noch im Ring verfügbare Sequenznummer.
// Returnt die Anzahl der ausgegebenen Zeilen.
size_t log_buffer_get_since(uint32_t since,
                             char *out, size_t out_cap,
                             uint32_t *head_seq, uint32_t *oldest_seq);

#ifdef __cplusplus
}
#endif

#endif // RFNETHM_LOG_BUFFER_H
