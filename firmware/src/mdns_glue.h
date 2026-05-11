// SPDX-License-Identifier: GPL-2.0-or-later
//
// mdns_glue.h — meldet drei Services per mDNS / DNS-SD an, sobald die
// STA-Verbindung steht.  bmcond / multimacd / Avahi-Browser können den
// Stick dann ohne hartkodierte IP finden.
//
//   _hbrfeth._udp  Port 3008  (HB-RF-ETH-UDP-Bridge, A.R.s Wire)
//   _rawuart._tcp  Port 2329  (Raw-HMUARTLGW-Stream)
//   _http._tcp     Port 80    (WebUI)
//
// TXT-Records:  model=RFNetHM, tag=<App-Tag live>, boot=<yes|no>,
//               fw=v<MAJOR.MINOR.BUILD>
//
// Hostname: rfnethm-<last4 base-MAC>  (deckungsgleich mit STA-Hostname
// aus net.c).  Auflösbar als rfnethm-XXXX.local.

#ifndef RFNETHM_MDNS_GLUE_H
#define RFNETHM_MDNS_GLUE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Idempotent.  Spawned: ein Task der auf STA-got-IP wartet und dann die
// Services anmeldet.  Falls noch keine Verbindung steht, bleibt der
// Responder still.  Periodischer Refresh der TXT-Records (5 s) fängt
// USB-Tag-Wechsel (BL → App) auf.
esp_err_t mdns_glue_init(void);

#ifdef __cplusplus
}
#endif

#endif // RFNETHM_MDNS_GLUE_H
