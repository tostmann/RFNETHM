// SPDX-License-Identifier: GPL-2.0-or-later
//
// sink_hmuartlgw_legacy.h — TCP-Listener mit lokal emuliertem
// HM-MOD-RPI-PCB ("Legacy") HMUARTLGW-Frontend.
//
// Phase A (2026-05-07):
//   - Akzeptiert FHEM-Client-Connects (`define <name> HMUARTLGW
//     uart://<ip>:2330`).
//   - Pushed `Co_CPU_BL` 200 ms nach Connect, repeat alle 3 s bis erste
//     OS-Probe; danach `Co_CPU_App` als Antwort auf OS_GET_APP.
//   - Beantwortet die komplette OS- und APP-Init-Sequenz lokal mit
//     byte-genauen Stubs (siehe CULFW32 frontend_hmuartlgw / Memory
//     `hmuartlgw_protocol_spec.md`).
//   - APP_SEND wird **derzeit** mit ACK_EUNKNOWN abgelehnt — Phase B
//     macht die Legacy-→DualCoPro-Übersetzung Richtung USB-Stick.
//   - Kein APP_RECV vom Stick noch — Phase C subscribed an die Bridge.
//
// Output: FHEM kommt zu `cond=ok`, `D-firmware=1.4.1`,
// `D-serialNr=RFNETHM01`, `Initialized=1`.  Frame-TX/RX folgt.
//
// Architekturvertrag mit dem Rest des RFNETHM-Stacks:
//   - Eigenes sink_t-Interface, aber **subscribet die Bridge erst in
//     Phase C** (kein on_rx vom Stick im Phase-A-Stub).
//   - Sendet niemals direkt an die USB-Source in Phase A.
//
// Schwester-Implementation (Referenz für byte-genaue Antworten):
// /Public/CLAUDE/CULFW32/firmware/components/frontend_hmuartlgw/

#ifndef RFNETHM_SINK_HMUARTLGW_LEGACY_H
#define RFNETHM_SINK_HMUARTLGW_LEGACY_H

#include "source.h"

#define SINK_HMUARTLGW_LEGACY_DEFAULT_PORT  2330
#define SINK_HMUARTLGW_LEGACY_MAX_CLIENTS   4

// Erstellt + startet den TCP-Listener.  port=0 → Default 2330.
sink_t *sink_hmuartlgw_legacy_init(uint16_t port);

typedef struct {
    uint16_t port;
    int      active_clients;
    uint32_t total_accepts;
    uint32_t total_disconnects;
    uint32_t frames_rx_from_fhem;
    uint32_t frames_tx_to_fhem;
    uint32_t frames_crc_err;
    uint32_t hello_pushes;
    uint32_t app_send_rejected;   // Phase A — wenn Source nicht ready
    uint32_t app_send_translated; // Phase B — Legacy → DualCoPro
    uint32_t llmac_acks_routed;   // Phase B — DualCoPro-ACK → FHEM-ACK
    uint32_t llmac_acks_orphaned; // Phase B — kein matching pending, aber wir waren tx-master
    uint32_t llmac_acks_foreign;  // Phase B — ACK gehört einem anderen Sink (HB-RF-ETH etc.)
    uint32_t llmac_recv_broadcast;// Phase C — LLMAC_RECV → APP_RECV
    uint32_t bypass_rx_bytes;     // Phase D — passthrough source→FHEM
    uint32_t bypass_tx_bytes;     // Phase D — passthrough FHEM→source
    uint32_t aes_keys_persisted;  // Phase D — NVS-stored keys (storage-only)
} sink_hmuartlgw_legacy_stats_t;

void sink_hmuartlgw_legacy_get_stats(sink_hmuartlgw_legacy_stats_t *out);

#endif // RFNETHM_SINK_HMUARTLGW_LEGACY_H
