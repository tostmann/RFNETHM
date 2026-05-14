// SPDX-License-Identifier: GPL-2.0-or-later
//
// sink_tcp.h — Raw-Bytestream-TCP-Listener-Sink.
//
// Wire-Vertrag (kein Encapsulation):
//   - Server bindet auf TCP-Port (Default 2329).
//   - Bytes vom Server an Client = 1:1 die HmIP-RFUSB-Bulk-IN-Bytes.
//   - Bytes vom Client an Server = 1:1 zu Bulk-OUT (Stick).
//   - Frame-Strukturierung passiert in HMUARTLGW-0xfd-Layer transparent
//     durch TCP — Client muss sein eigenes Streaming-Decoder mitbringen.
//
// Der Sink unterstützt mehrere parallele Clients (Default 4); RX vom
// USB-Stick wird an alle aktiven Clients gefanned.  TX von einem
// beliebigen Client wird via bridge_tx_to_source() geleitet.
//
// CULFW32-Schwester-Implementation als Referenz:
// /Public/CLAUDE/CULFW32/firmware/components/transport_rawuart_tcp/

#ifndef RFNETHM_SINK_TCP_H
#define RFNETHM_SINK_TCP_H

#include "source.h"

#define SINK_TCP_DEFAULT_PORT  2329
#define SINK_TCP_MAX_CLIENTS   4

// Erstellt + startet den TCP-Listener-Sink.  port=0 → Default 2329.
// Returns sink_t* das per bridge_attach_sink() registriert werden kann.
sink_t *sink_tcp_init(uint16_t port);

typedef struct {
    uint16_t port;
    int      active_clients;
    uint32_t total_accepts;
    uint32_t total_disconnects;
    uint32_t rx_bytes_from_clients;
    uint32_t tx_bytes_to_clients;
    uint32_t tx_dropped_eagain;
} sink_tcp_stats_t;

void sink_tcp_get_stats(sink_tcp_stats_t *out);

#endif // RFNETHM_SINK_TCP_H
