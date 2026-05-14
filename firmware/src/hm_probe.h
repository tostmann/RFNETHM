// SPDX-License-Identifier: GPL-2.0-or-later
//
// HM-Modul Boot-Identify-Probe — sendet COMMON_IDENTIFY auf UART1
// (GPIO17 TX / GPIO18 RX) und logged das App-/BL-Tag des angeschlossenen
// HM-MOD-RPI-PCB / RPI-RF-MOD / sonstigen HMUARTLGW-Moduls.
//
// One-shot: einmal beim Boot aufgerufen, danach ist UART1 wieder frei.

#ifndef RFNETHM_HM_PROBE_H
#define RFNETHM_HM_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

void hm_probe_run(void);

#ifdef __cplusplus
}
#endif

#endif
