// SPDX-License-Identifier: GPL-2.0-or-later
//
// RFNETHM v0.10 — wie v0.9 plus Improv-Serial-WiFi-Provisioning.
//
// Neu in v0.10: Credentials werden nicht mehr über build_flags
// hardcoded.  Beim ersten Boot ohne NVS-Eintrag öffnet sich für 120 s
// ein Improv-Serial-Window auf UART0 (CH343P-Bridge); per ESP Web Tools
// oder `improv_client.py` werden SSID + PSK übertragen, die Lib
// connectet, und onConnected persistiert die Werte in den NVS-
// Namespace `rfnethm`.  Beim nächsten Boot ist netinit dann ein
// no-Improv-no-build-flag-Quickstart.
//
// Sinks (TCP/UDP/WebUI) starten unabhängig vom Improv-Status — sie
// binden sobald net_is_connected().  Reihenfolge bleibt:
//   bridge → source_usb → bridge_attach_sink (debug)
//   net_init (Infra immer; STA nur wenn Creds)
//   improv_init (Window 120 s, idempotent)
//   sink_tcp + sink_hbrfeth + webui (ESP_OK gehört zur Konvention)

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_core_dump.h"

#include "version.h"
#include "bridge.h"
#include "source_usb.h"
#include "source_uart.h"
#include "sink_debug.h"
#include "net.h"
#include "improv_glue.h"
#include "sink_tcp.h"
#include "sink_hbrfeth.h"
#include "sink_hmuartlgw_legacy.h"
#include "log_buffer.h"
#include "webui.h"
#include "mdns_glue.h"

static const char *TAG = "rfnethm";

// Source-Referenzen für den Supervisor-Task (gesetzt in app_main).
static source_t *s_src_uart;
static source_t *s_src_usb;

// Supervisor: alle 500 ms Source-Status prüfen, ggf. Bridge umhängen.
//
//   Priorität (gewünschte Politik 2026-05-07):
//     1. USB-Stick ready → ist aktive Source (Hot-Swap, falls gerade UART)
//     2. USB nicht ready (oder gerade gezogen) + UART ready → UART aktiv
//     3. weder noch → Bridge ohne Source (alle Sinks open, keine Bytes)
//
// Ausnahme: wenn UART im Flash-Lock-Modus (Modul im BL gehalten für
// FW-Flash via bmcond /api/source/uart/reset {hold_in_bl:true}), bleibt
// die Bridge fest auf UART — kein Auto-Swap zu USB selbst wenn da
// ein Stick auftaucht.  Sonst würde bmcond's Flash-Sequenz mid-stream
// die Source unterm Hintern weggeswapt.
//
// Idle-Schreibe: nur swap loggen wenn sich die Lage tatsächlich ändert.
static void source_supervisor_task(void *arg)
{
    (void)arg;

    // Supervisor ist die Keystone-Task: wenn sie hängt, friert das
    // Source-Hot-Swap-Verhalten ein.  Subscribe an TWDT (5 s timeout aus
    // sdkconfig); reset in jeder Schleifeniteration.  Loop-Periode 500 ms,
    // also reichlich Puffer.
    esp_err_t wr = esp_task_wdt_add(NULL);
    if (wr != ESP_OK && wr != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "supervisor: esp_task_wdt_add failed (%d)", wr);
    }

    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(500));

        // Tatsächlichen Bridge-Zustand abfragen — sonst kann eine lokale
        // Tracking-Variable mit der Realität divergieren (z.B. wenn ein
        // attach mal fehlschlägt oder eine andere Stelle die Bridge
        // umschaltet).
        source_t *cur = bridge_get_source();

        // Flash-Lock-Schutz: wenn UART mid-flash, einfach nichts machen.
        if (source_uart_is_flash_locked()) {
            if (cur != s_src_uart) {
                ESP_LOGW(TAG, "supervisor: UART flash-lock active — pinning bridge to UART");
                bridge_attach_source(s_src_uart);
            }
            continue;
        }

        bool usb_ready  = source_ready(s_src_usb);
        bool uart_ready = source_ready(s_src_uart);
        source_t *want  = usb_ready  ? s_src_usb
                       : uart_ready ? s_src_uart
                       :              NULL;
        if (want == cur) continue;
        if (want) {
            ESP_LOGW(TAG, "supervisor: %s → %s",
                     cur ? source_describe(cur) : "(none)",
                     source_describe(want));
            bridge_attach_source(want);
        } else if (cur) {
            ESP_LOGW(TAG, "supervisor: detach (%s lost, no fallback)",
                     source_describe(cur));
            bridge_detach_source(cur);
        }
    }
}

// ───── Health-Logging ────────────────────────────────────────────────────
//
// Hilfsfunktion für den 60-s-Stats-Loop in app_main.  Liefert eine
// kompakte Zeile mit min_free_heap, largest_free_block, anzahl Tasks,
// minimale Stack-HWM aller Tasks plus den Namen der "engsten" Task.
// FreeRTOS muss mit USE_TRACE_FACILITY=y gebaut sein (siehe
// sdkconfig.defaults).
//
// Heap-Low-Schwelle: 16 KB.  Bei Unterschreiten ein WARN-Log.
#define HEAP_LOW_WARN_BYTES   (16 * 1024)
#define STACK_LOW_WARN_WORDS  (256)   // 256 words ≈ 1 KB freier Stack

static void log_health_line(void)
{
    size_t free_now = esp_get_free_heap_size();
    size_t min_free = esp_get_minimum_free_heap_size();
    size_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    UBaseType_t ntasks = uxTaskGetNumberOfTasks();
    TaskStatus_t *snap = (TaskStatus_t *)pvPortMalloc(sizeof(TaskStatus_t) * ntasks);
    UBaseType_t tightest_hwm = UINT32_MAX;
    const char *tightest_name = "?";
    if (snap) {
        UBaseType_t n = uxTaskGetSystemState(snap, ntasks, NULL);
        for (UBaseType_t i = 0; i < n; i++) {
            UBaseType_t hwm = snap[i].usStackHighWaterMark;
            if (hwm < tightest_hwm) {
                tightest_hwm = hwm;
                tightest_name = snap[i].pcTaskName ? snap[i].pcTaskName : "?";
            }
        }
        vPortFree(snap);
    } else {
        ESP_LOGW(TAG, "health: pvPortMalloc(%u tasks) failed — stack HWM skipped",
                 (unsigned)ntasks);
    }

    ESP_LOGI(TAG, "health: heap free=%u min=%u largest=%u | tasks=%u tightest='%s' hwm=%u words",
             (unsigned)free_now, (unsigned)min_free, (unsigned)largest,
             (unsigned)ntasks, tightest_name, (unsigned)tightest_hwm);

    if (min_free < HEAP_LOW_WARN_BYTES) {
        ESP_LOGW(TAG, "health: min_free_heap %u < %u — heap pressure detected",
                 (unsigned)min_free, (unsigned)HEAP_LOW_WARN_BYTES);
    }
    if (tightest_hwm < STACK_LOW_WARN_WORDS) {
        ESP_LOGW(TAG, "health: task '%s' stack HWM %u words < %u — stack growth seen",
                 tightest_name, (unsigned)tightest_hwm, (unsigned)STACK_LOW_WARN_WORDS);
    }

    // Coredump-Check entfernt aus der Health-Loop:
    // esp_core_dump_image_check() macht eine synchrone SPI-Flash-Lesung
    // und produzierte 1 ESP-IDF-internen "Incorrect size: 1"-E-Log pro
    // 60s.  Coredump-Existenz wird beim Boot-Banner einmalig geloggt
    // (siehe app_main) und ist über /api/status.sys.coredump abrufbar
    // (Cache in webui.c).
}

void app_main(void)
{
    // Log-Tee VOR allem anderen — sonst geht der Banner an UART, aber
    // nicht in den Ring, und /api/log liefert ein leeres Ergebnis.
    log_buffer_init();

    printf("\n");
    printf("=================================================\n");
    printf("  RFNetHM   v%s\n", FW_VERSION_STRING);
    printf("  built     %s\n", FW_BUILD_DATE);
    printf("=================================================\n");

    bridge_init();

    // ───── Dual-Source-Init (v0.15) ─────
    //
    // Beide Sources werden initialisiert.  Policy:
    //   • UART-Modul wird bei Boot geprobt — bei Erfolg sofort als
    //     Source attached (Always-On-Fallback).
    //   • USB-Stick wird parallel hochgefahren (USB-Host-Stack ist
    //     ein eigener Task).  Sobald der HmIP-RFUSB-Stick im App-Mode
    //     ist, übernimmt er die Bridge per Hot-Swap (USB hat Priorität).
    //   • Wird der Stick im Betrieb gezogen → Supervisor-Task swapt
    //     zurück auf UART (sofern dort noch ein Modul ready ist).
    //
    // bridge_attach_source() ist swap-fähig (siehe bridge.c).
    s_src_uart = source_uart_init();
    s_src_usb  = source_usb_init();
    if (source_ready(s_src_uart)) {
        bridge_attach_source(s_src_uart);   // typisch noch nicht ready hier
    }

    bridge_attach_sink(sink_debug_get(), "debug");

    // Net-Infra immer, STA nur mit Creds.  Auch ohne Creds: ESP_OK.
    net_init();

    // Improv-Serial-Window — armed 120 s, lauscht auf UART0/CH343P.
    improv_init();

    // Sinks starten — binden erst wenn der STA-Stack eine IP hat,
    // funktionieren also auch wenn Improv die WiFi gerade erst hochzieht.
    // short_id-Strings sind Pflicht-Identifier für TX-Lock + WebUI-Radios.
    // Stabil halten — die Web-Frontend-Strings hängen daran.
    sink_t *tcp = sink_tcp_init(0);
    bridge_attach_sink(tcp, "rawuart");
    sink_start(tcp);

    sink_t *hb = sink_hbrfeth_init(0);
    bridge_attach_sink(hb, "hbrfeth");
    sink_start(hb);

    // Phase A v0.13: HMUARTLGW Legacy-Mode-Emulation auf TCP:2330 für
    // FHEM/Homegear.  Lokale OS+APP-Init-Stubs; kein Frame-TX/RX yet.
    sink_t *hmu_legacy = sink_hmuartlgw_legacy_init(0);
    bridge_attach_sink(hmu_legacy, "hmu");
    sink_start(hmu_legacy);

    webui_init(0);

    // mDNS / DNS-SD: bmcond & co. finden uns als rfnethm-XXXX.local
    // ohne hartkodierte IP.  Idempotent, läuft im eigenen Task der auf
    // STA-got-IP wartet — funktioniert also auch wenn Improv erst noch
    // provisionieren muss.
    mdns_glue_init();

    // Source-Supervisor — pollt Source-Ready-Status, swapt Bridge wenn nötig.
    xTaskCreate(source_supervisor_task, "src-sup", 3072, NULL, 4, NULL);

    // Beim Boot: zeige Reset-Reason explizit + ggf. vorhandenen Coredump
    // an, damit ein crash-after-reboot in der Log-Geschichte unübersehbar
    // ist.  Reset-Reason war bislang nur im /api/status.sys.reset_reason
    // sichtbar.
    {
        esp_reset_reason_t rr = esp_reset_reason();
        ESP_LOGW(TAG, "boot: reset_reason=%d (1=POWERON 3=SW 4=PANIC 5=INT_WDT 6=TASK_WDT 8=BROWNOUT)",
                 (int)rr);
        if (esp_core_dump_image_check() == ESP_OK) {
            size_t addr = 0, size = 0;
            esp_core_dump_image_get(&addr, &size);
            ESP_LOGW(TAG, "boot: coredump valid in flash @0x%x size=%u — fetch via /api/coredump",
                     (unsigned)addr, (unsigned)size);
        }
    }

    ESP_LOGI(TAG, "ready — RF-Source-Auswahl: USB-Priorität, UART-Fallback");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        log_health_line();
        bridge_stats_t bs;        bridge_get_stats(&bs);
        source_uart_stats_t uts;  source_uart_get_stats(&uts);
        source_usb_stats_t  uss;  source_usb_get_stats(&uss);
        sink_tcp_stats_t ts;      sink_tcp_get_stats(&ts);
        sink_hbrfeth_stats_t hs;  sink_hbrfeth_get_stats(&hs);
        source_t *cur = bridge_get_source();
        const char *active = (cur == s_src_usb) ? "usb"
                            : (cur == s_src_uart) ? "uart"
                            :                       "(none)";
        ESP_LOGI(TAG, "stats: active=%s bridge rx=%u/%u tx=%u drop=%u sinks=%u | "
                      "uart present=%d boot=%d tag='%s' frames=%u | "
                      "usb conn=%d boot=%d tag='%s' frames=%u | "
                      "tcp :%u cli=%d rx=%u tx=%u | "
                      "hbrf :%u cli=%d conn=%u rx=%u tx=%u badcrc=%u | "
                      "net %s ssid=%s | improv %s",
                 active,
                 (unsigned)bs.rx_bytes_total, (unsigned)bs.rx_pumps_total,
                 (unsigned)bs.tx_bytes_total, (unsigned)bs.tx_dropped_not_ready,
                 (unsigned)bs.sink_count,
                 (int)uts.module_present, (int)uts.boot_done, uts.app_tag,
                 (unsigned)uts.frames_ok,
                 (int)uss.stick_connected, (int)uss.boot_done, uss.app_tag,
                 (unsigned)uss.frames_ok,
                 ts.port, ts.active_clients,
                 (unsigned)ts.rx_bytes_from_clients,
                 (unsigned)ts.tx_bytes_to_clients,
                 hs.port, hs.active_clients,
                 (unsigned)hs.total_connects,
                 (unsigned)hs.rx_frames_from_clients,
                 (unsigned)hs.tx_frames_to_clients,
                 (unsigned)hs.bad_crc,
                 net_is_connected() ? net_ip_str() : "disconnected",
                 net_ssid(),
                 improv_is_armed() ? "armed" : "idle");
    }
}
