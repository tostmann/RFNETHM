// SPDX-License-Identifier: GPL-2.0-or-later
//
// sink_hbrfeth.h — HB-RF-ETH-UDP-Tunnel-Sink (Drop-in für piVCCU /
// RaspberryMatic / debmatic via Alex Reinerts `hb_rf_eth.ko`).
//
// Wire-Protokoll (verifiziert gegen alexreinert/HB-RF-ETH
// `rawuartudplistener.cpp`, byte-genau aus CULFW32-Implementierung
// `transport_hbrfeth_udp/src/hbrfeth_listener.cpp`):
//
//   UDP-Port 3008 (Default).
//   Frame: [Type:1] [Counter:1] [Payload:n-4] [CRC16-BE:2]
//   CRC16: poly 0x8005, init 0xd77f (= HMUARTLGW-CRC).
//
//   Type-Codes:
//     0  Connect    V1: payload=[ver=1]            → reply [1, mirror_cnt]
//                   V2: payload=[ver=2, ep_id]     → reply [2, mirror_cnt, server_ep_id]
//     1  Disconnect (host → modul)
//     2  KeepAlive  (1 Hz host → modul, 5 s timeout; modul auch out 1 Hz)
//     3  LED        (1 byte rgb-bits — wir ignorieren)
//     4  Reset      (we ignore)
//     5  StartConn  (Type-7-Frames werden erst nach diesem Type weitergeleitet)
//     6  StopConn   (gegenteil)
//     7  Frame      (raw HMUARTLGW 0xfd-binary frame)
//
// Jede (IP,Port)-Combo bekommt einen ClientState-Slot (max 4 parallel).
// Der Linux-Treiber `hb_rf_eth` macht typischerweise ein 1:1-Setup mit
// einer Instanz pro Stick — Multi-Client ist eher Diagnose-Use-Case.

#ifndef RFNETHM_SINK_HBRFETH_H
#define RFNETHM_SINK_HBRFETH_H

#include "source.h"

#define SINK_HBRFETH_DEFAULT_PORT  3008
#define SINK_HBRFETH_MAX_CLIENTS   4

// Erstellt + startet den HB-RF-ETH-UDP-Listener.  port=0 → Default 3008.
sink_t *sink_hbrfeth_init(uint16_t port);

typedef struct {
    uint16_t port;
    int      active_clients;
    uint32_t total_connects;
    uint32_t total_disconnects;
    uint32_t bad_crc;
    uint32_t unknown_type;
    uint32_t keepalive_timeouts;
    uint32_t rx_frames_from_clients;     // Type-7 Richtung Source
    uint32_t tx_frames_to_clients;        // Type-7 Richtung Client
    uint32_t tx_dropped_not_started;      // Sink hat Bytes aber kein StartConn
    uint32_t tx_dropped_eagain;           // sendto EAGAIN (lwIP TX-Queue voll)
} sink_hbrfeth_stats_t;

void sink_hbrfeth_get_stats(sink_hbrfeth_stats_t *out);

#endif // RFNETHM_SINK_HBRFETH_H
