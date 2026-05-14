// SPDX-License-Identifier: GPL-2.0-or-later
//
// sink_debug.h — primitiver Sink der jeden RX-Pump-Aufruf hex-dumpt.
// Macht im Wesentlichen dasselbe was bisher in main.c v0.4 die
// `stick_data_cb` direkt tat — nur jetzt als bridge-Sink.

#ifndef RFNETHM_SINK_DEBUG_H
#define RFNETHM_SINK_DEBUG_H

#include "source.h"

sink_t *sink_debug_get(void);

#endif // RFNETHM_SINK_DEBUG_H
