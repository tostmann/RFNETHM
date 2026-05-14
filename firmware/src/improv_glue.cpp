// SPDX-License-Identifier: GPL-2.0-or-later
//
// Improv-Serial-WiFi-Provisioning für RFNETHM, UART0-basiert (CH343P
// Bridge auf YD-ESP32-S3 V1.4).  Lib-Quelle:
// https://github.com/tostmann/improv-wifi-busware (Pin in
// idf_component.yml).
//
// Architektur (analog CULFW32, vereinfacht):
//   - UART0-Driver wird hier installiert (RX-Buffer 512 B).
//   - improv_task liest Bytes vom UART, füttert die Lib via feedBytes().
//   - write_fn schreibt Antwort-Bytes per uart_write_bytes auf UART0.
//   - Tick wird im Task-Loop einmal pro 100 ms gerufen.
//   - onConnected → net_persist_creds() (Atomar, kein Reboot — die
//     Lib's tryConnect hat den STA bereits hochgezogen, persisting nur
//     für den nächsten Boot).
//   - net_set_external_control(true) ist während des Windows aktiv,
//     damit unser eigener STA_DISCONNECTED-Handler nicht in die
//     re-provision-Sequenz reinpfuscht.
//
// Stick-Console: das v0.9-Banner und alle ESP_LOG_* gehen weiterhin
// via printf auf UART0 raus.  Die Lib parst nur eingehende Bytes mit
// dem `IMPROV`-Magic; unsere Logs enthalten das Magic nicht, also keine
// Konflikte.

#include "improv_glue.h"
#include "net.h"

#include "improv_wifi/improv_wifi.h"
#include "improv_wifi/idf_backend.h"
#include "improv_wifi/types.h"

#include "version.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>
#include <cstring>

static const char *TAG = "improv";

namespace ipw = improv_wifi_busware;

namespace {

constexpr uart_port_t UART_NUM         = UART_NUM_0;
constexpr int         UART_RX_BUF      = 512;
constexpr int         UART_TX_BUF      = 0;     // 0 = blocking writes (sync)
// Idle-basiertes Timeout: das Window bleibt offen solange host→stick
// Bytes innerhalb der letzten IDLE_MS-Spanne gekommen sind.  Erst
// `IDLE_MS` ohne jeglichen UART-Byte → Window geht zu.  Damit kann der
// User entspannt reconfiguraten (ESP Web Tools öffnen, scannen, tippen,
// senden) ohne dass ihm mitten drin der Boden weggezogen wird.
//
// Initial-Idle (= "ab Boot ohne Aktivität"):
//   - Keine Creds:    120 s — Erstprovisioning, langsamer User
//   - Creds da:        30 s — schnelle Reconfig, sonst zu
//
// Lib-internes windowMs ist auf ≫ Idle gesetzt; wir steuern das Window
// effektiv selbst über die Idle-Bookkeeping unten.
constexpr uint32_t    IDLE_MS_FRESH      = 120 * 1000;
constexpr uint32_t    IDLE_MS_HASCREDS   =  30 * 1000;
constexpr uint32_t    LIB_WINDOW_MS_CAP  =  60 * 60 * 1000;  // 1 h hard-cap

ipw::EspIdfWiFiBackend *s_backend          = nullptr;
ipw::ImprovWiFi        *s_inst             = nullptr;
bool                    s_inited           = false;
int64_t                 s_last_activity_us = 0;
uint32_t                s_idle_ms          = IDLE_MS_FRESH;
bool                    s_armed            = false;

void write_fn(const uint8_t *data, size_t len, void * /*user*/)
{
    if (!data || len == 0) return;
    uart_write_bytes(UART_NUM, reinterpret_cast<const char *>(data), len);
}

void on_error_cb(ipw::Error e, void * /*user*/)
{
    ESP_LOGW(TAG, "improv error 0x%02x", static_cast<unsigned>(e));
}

void on_connected_cb(const char *ssid, const char *psk, void * /*user*/)
{
    ESP_LOGI(TAG, "provisioned to '%s' — persisting in rfnethm NVS",
             ssid ? ssid : "?");
    net_persist_creds(ssid ? ssid : "", psk ? psk : "");
    // Kein Reboot — Lib hat den STA bereits hochgezogen, der nächste
    // Boot nimmt die persistierten Creds und beschneidet das Improv-
    // Window auf WINDOW_MS_HASCREDS (30 s) — kurz, aber nicht aus,
    // damit Re-Provisioning via ESP Web Tools jederzeit möglich bleibt
    // (WLAN-Wechsel ohne Captive-AP-Ritual).
}

void improv_task(void * /*arg*/)
{
    uint8_t buf[64];
    while (true) {
        const int64_t  now_us = esp_timer_get_time();
        const uint32_t now_ms = static_cast<uint32_t>(now_us / 1000);
        s_inst->tick(now_ms);

        // Eigene Idle-Logik: Window läuft solange Aktivität innerhalb der
        // letzten s_idle_ms — und solange die Lib selbst nicht den 1 h-
        // Hard-Cap erreicht (Sicherheits-Cap, z.B. gegen Dauerprasseln).
        const int64_t idle_us = now_us - s_last_activity_us;
        const bool    expired = idle_us > static_cast<int64_t>(s_idle_ms) * 1000;
        if (!s_inst->isArmed() || expired) {
            ESP_LOGI(TAG, "window closed (idle %us, lib_armed=%d)",
                     static_cast<unsigned>(idle_us / 1000000),
                     static_cast<int>(s_inst->isArmed()));
            s_armed = false;
            net_set_external_control(false);
            // Driver eingebaut lassen — späterer Code könnte UART
            // weiter brauchen (Console-Echo etc.).  Nur die Lib hält
            // sich raus.
            break;
        }

        const int got = uart_read_bytes(UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (got > 0) {
            // Jedes empfangene Byte zählt als Aktivität — auch wenn die
            // Lib das nicht als Improv-Magic erkennt (z.B. der User tippt
            // versehentlich was im Terminal).  Konservativ: das Idle-
            // Fenster lieber zu lang als zu kurz schließen.
            s_last_activity_us = now_us;
            s_inst->feedBytes(buf, static_cast<size_t>(got));
        }
    }
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" esp_err_t improv_init(void)
{
    if (s_inited) return ESP_OK;

    // UART0-Driver installieren — printf nutzt UART0 weiter via VFS,
    // aber wir lesen RX direkt vom Driver.
    uart_config_t cfg = {};
    cfg.baud_rate           = 115200;
    cfg.data_bits           = UART_DATA_8_BITS;
    cfg.parity              = UART_PARITY_DISABLE;
    cfg.stop_bits           = UART_STOP_BITS_1;
    cfg.flow_ctrl           = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk          = UART_SCLK_DEFAULT;

    esp_err_t err = uart_param_config(UART_NUM, &cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_driver_install(UART_NUM, UART_RX_BUF, UART_TX_BUF, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    // Lib-Backend + Instanz statisch — Lebensdauer = Programm.
    static ipw::EspIdfWiFiBackend s_be;
    s_backend = &s_be;

    s_idle_ms = net_has_creds() ? IDLE_MS_HASCREDS : IDLE_MS_FRESH;
    s_last_activity_us = esp_timer_get_time();
    s_armed = true;

    ipw::Config c;
    c.backend                  = s_backend;
    c.write                    = &write_fn;
    c.userCtx                  = nullptr;
    c.windowMs                 = LIB_WINDOW_MS_CAP;
    c.device.chipFamily        = ipw::ChipFamily::Esp32S3;
    c.device.firmwareName      = "RFNETHM";
    c.device.firmwareVersion   = FW_VERSION_STRING;
    c.device.deviceName        = "RFNETHM";
    c.device.deviceUrl         = nullptr;   // Lib füllt http://<ip>/
    c.onError                  = &on_error_cb;
    c.onConnected              = &on_connected_cb;

    static ipw::ImprovWiFi s_improv_inst{c};
    s_inst = &s_improv_inst;

    // Solange das Window armed ist, nehmen wir net.c den Auto-Reconnect
    // weg — sonst kollidieren wir mit Lib's tryConnect/disconnect-
    // Sequenz.
    net_set_external_control(true);

    BaseType_t r = xTaskCreate(&improv_task, "improv", 4096, nullptr, 4, nullptr);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_FAIL;
    }

    s_inited = true;
    ESP_LOGI(TAG, "improv-serial armed: idle-timeout=%us (creds %spresent), "
                  "extends on UART activity",
             static_cast<unsigned>(s_idle_ms / 1000),
             net_has_creds() ? "" : "not ");
    return ESP_OK;
}

extern "C" bool improv_is_armed(void)
{
    return s_armed && s_inst && s_inst->isArmed();
}

extern "C" uint32_t improv_window_remaining_ms(void)
{
    // Verbleibende Idle-Zeit, NICHT das absolute Lib-Window — passt zum
    // refactor in v0.14.55: Window läuft nach IDLE_MS Stille zu, nicht
    // nach absolutem Timer ab Boot.
    if (!s_armed) return 0;
    const int64_t idle_us = esp_timer_get_time() - s_last_activity_us;
    const int64_t left_us = static_cast<int64_t>(s_idle_ms) * 1000 - idle_us;
    return left_us > 0 ? static_cast<uint32_t>(left_us / 1000) : 0;
}
