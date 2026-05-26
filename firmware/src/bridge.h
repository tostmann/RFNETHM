// SPDX-License-Identifier: GPL-2.0-or-later
//
// bridge.h — verbindet 1 source_t mit N sink_t.
//
// Die Bridge ist die einzige Komponente die weiß welche Sinks an welche
// Source hängen.  Source und Sinks sind voneinander entkoppelt (siehe
// source.h für die Interface-Spec).
//
// Fanout-Modell:
//   - Source liefert RX-Bytes → bridge_on_source_rx() → ruft jeden
//     Sink synchron auf seinem on_source_rx-Hook auf.
//   - Sink will TX zur Source → bridge_tx_to_source() → leitet weiter
//     wenn source_ready() UND der Sink den TX-Lock besitzt.
//
// TX-Lock (v0.14, "Single-Writer-Soft-Lock"):
//   - Ein 8-bit msg_nr-Counter, ein RF-Channel, eine AES-Challenge-State-
//     Machine — der HmIP-RFUSB-Stick verträgt nur **einen** TX-Master
//     gleichzeitig.  Die Bridge serialisiert das softwareseitig.
//   - Mode AUTO (Default): erster Sender ergreift den Lock; nach 5 s
//     Stille ist der Lock frei und der nächste Sender bekommt ihn.
//   - Mode PINNED: ein Sink ist explizit als TX-Master festgepinnt; alle
//     anderen werden mit ESP_ERR_NOT_PERMITTED abgelehnt.
//   - WebUI hat Radio-Buttons pro Kachel zum Umschalten.
//
// Die Bridge hat keinen eigenen Task; sie ist eine reine Vermittlungs-
// schicht.  Quellen und Sinks bringen ihre eigenen Tasks mit (z.B.
// USB-Host-Task, TCP-Accept-Task).

#ifndef RFNETHM_BRIDGE_H
#define RFNETHM_BRIDGE_H

#include "source.h"

#define BRIDGE_MAX_SINKS         8
#define BRIDGE_SINK_ID_MAXLEN    16
#define BRIDGE_TX_AUTO_IDLE_US   5000000     // 5 s Idle bis Lock-Release im AUTO

// "TX rejected, lock owned by anderer Sink" — semantisch identisch zu
// ESP_ERR_NOT_ALLOWED (IDF 5.5.x).  Kein selbsterfundener Sentinel im
// 0xE0xx-Range mehr — der konnte mit zukünftigen IDF-Werten kollidieren.
#define BRIDGE_ERR_TX_LOCKED     ESP_ERR_NOT_ALLOWED

// Initialisiere die globale Bridge.  Muss einmal beim Start aufgerufen
// werden bevor irgendeine source/sink-API genutzt wird.
void bridge_init(void);

// Source-Anbindung — kann nach dem Erstellen der Source gerufen werden,
// bevor source_t.user-Init abgeschlossen ist (die Source-Init-Routine
// ruft selbst bridge_on_source_rx() wenn Bytes ankommen, also muss die
// Bridge die Source-Referenz schon kennen).
//
// Wenn vorher schon eine Source attached war → wird detached (RX-Sink
// auf NULL gesetzt, fanout endet).  Dadurch ist Hot-Swap zur Laufzeit
// möglich (z.B. USB-Stick ein-/abstecken bei aktiver UART-Modul-Source).
void bridge_attach_source(source_t *src);

// Source vom Bridge lösen.  RX-Bytes der Source landen danach nicht
// mehr im Fanout (Sinks bleiben weiter listening, aber leer).  No-op
// wenn die Source aktuell nicht attached ist.
void bridge_detach_source(source_t *src);

source_t *bridge_get_source(void);

// Sink-Anbindung.  short_id ist eine kurze stabile Kennung (zB "hbrfeth",
// "rawuart", "hmu"), die in der WebUI als TX-Master-Radio-Wert benutzt
// wird.  Darf NULL sein für Sinks die keinen TX-Pfad haben (z.B. debug)
// — die kann man im UI nicht als Master wählen.
//
// Returns ESP_OK oder ESP_ERR_NO_MEM wenn schon BRIDGE_MAX_SINKS belegt.
esp_err_t bridge_attach_sink(sink_t *sink, const char *short_id);

// Hook der von der Source aus dem RX-Pfad gerufen wird.  Verteilt an
// alle gerade registrierten Sinks.  Synchron — Sinks dürfen nicht
// blockieren.
void bridge_on_source_rx(void *ctx, const uint8_t *data, size_t len);

// Sink → Source TX-Pfad.  `who` ist der sink_t* des Aufrufers (eigene
// `&S.self`-Referenz aus dem Sink-Modul); NULL bedeutet "interner /
// system call" und umgeht den TX-Lock.
//
// Returns:
//   ESP_OK                 — TX hat den Source-Layer erreicht
//   ESP_ERR_INVALID_STATE  — Source nicht ready
//   BRIDGE_ERR_TX_LOCKED   — TX-Lock von einem anderen Sink belegt
//   weitere                — propagiert von source->ops->tx()
esp_err_t bridge_tx_to_source(sink_t *who, const uint8_t *data, size_t len);

// ───── TX-Lock-Steuerung ────────────────────────────────────────────────

typedef enum {
    BRIDGE_TX_AUTO    = 0,    // first-sender-wins, idle-release
    BRIDGE_TX_PINNED  = 1,    // pinned to a specific sink
} bridge_tx_mode_t;

// Setzt den TX-Lock-Mode.
//   id == NULL || id == "" || id == "auto"  →  AUTO-Mode (Lock free)
//   id == "<short_id of sink>"               →  PIN auf diesen Sink
// Returns ESP_ERR_NOT_FOUND wenn der short_id keinem registrierten Sink
// entspricht.
esp_err_t bridge_set_tx_master(const char *id_or_auto);

typedef struct {
    bridge_tx_mode_t mode;
    int              owner_idx;                                // -1 wenn niemand
    char             owner_id[BRIDGE_SINK_ID_MAXLEN];          // "" wenn niemand
    int64_t          last_tx_us;                               // 0 wenn nie
    char             ids[BRIDGE_MAX_SINKS][BRIDGE_SINK_ID_MAXLEN];
    bool             slot_used[BRIDGE_MAX_SINKS];
    uint32_t         tx_rejected_lock[BRIDGE_MAX_SINKS];
    uint32_t         tx_rejected_lock_total;
} bridge_tx_info_t;

void bridge_get_tx_info(bridge_tx_info_t *out);

// Statistik / Status.
typedef struct {
    uint32_t  rx_bytes_total;
    uint32_t  rx_pumps_total;     // Anzahl on-source-rx-Aufrufe (≈ URBs)
    uint32_t  tx_bytes_total;
    uint32_t  tx_dropped_not_ready;
    uint32_t  tx_dropped_locked;     // NEU v0.14: TX vom Nicht-Master abgelehnt
    size_t    sink_count;
    int64_t   rx_last_us;            // esp_timer_get_time() des letzten on_source_rx; 0 wenn nie
} bridge_stats_t;

void bridge_get_stats(bridge_stats_t *out);

#endif // RFNETHM_BRIDGE_H
