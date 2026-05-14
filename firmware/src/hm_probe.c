// SPDX-License-Identifier: GPL-2.0-or-later
//
// HM-Modul Boot-Identify-Probe für UART1 (HM-MOD-RPI-PCB Bringup).
//
// Pin-Belegung (siehe docs/breadboard_wiring.md, Stand 2026-05-07):
//   GPIO17 = HM_RX     (UART1 TX, eQ3-Pin 8 via R1=1K)
//   GPIO18 = HM_TX     (UART1 RX, eQ3-Pin 10 via R2=1K)
//   GPIO16 = HM_RST    (eQ3-Pin 12, TRX1.4 C2CK/RST, active LOW)
//
// Ablauf (verbatim portiert aus source_usb.c boot_to_app):
//   1. Reset-Pulse auf GPIO16 (active LOW), Release mit ESP-internem Pull-Up
//      — HM-MOD-RPI-PCB hat KEINEN internen Pull-Up auf der RST-Linie.
//   2. Boot-Banner abwarten + decodieren → Tag (BL oder App).
//   3. Falls _BL Suffix: COMMON_CHANGE_APP, neuen Banner-Push abwarten,
//      Verify-IDENTIFY senden.
//   4. Final-Tag in s_tag merken.

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "hmu_frame.h"
#include "hm_probe.h"

static const char *TAG = "hm_probe";

#define PIN_TX         17
#define PIN_RX         18
#define PIN_RST        16

#define UART_NUM       UART_NUM_1
#define BAUD           115200
#define RX_BUF_SIZE    1024

typedef enum {
    TAG_KIND_NONE = 0,
    TAG_KIND_BL,
    TAG_KIND_APP,
} tag_kind_t;

static volatile bool       s_got_tag;
static char                s_tag[32];
static tag_kind_t          s_tag_kind;
static uint8_t             s_cnt = 1;

static void hex_str(const uint8_t *p, size_t n, char *out, size_t cap)
{
    size_t lim = (n > 32) ? 32 : n;
    out[0] = '\0';
    for (size_t i = 0; i < lim; i++) {
        snprintf(out + 3 * i, cap - 3 * i, "%02X ", p[i]);
    }
}

static tag_kind_t classify_tag(const char *tag)
{
    size_t n = strlen(tag);
    if (n >= 4 && strcmp(tag + n - 4, "_App") == 0) return TAG_KIND_APP;
    if (n >= 3 && (strcmp(tag + n - 3, "_Bl") == 0
                || strcmp(tag + n - 3, "_BL") == 0)) return TAG_KIND_BL;
    return TAG_KIND_NONE;
}

// HMUARTLGW frame-callback.  Tag-Frames können auf zwei Pfaden ankommen:
//   dst=OS    cnt=0 sub=0       payload=<ASCII-Tag>           (Boot-Banner-Push)
//   dst=COMMON cnt=N sub=0x01    payload=[01, 00, <ASCII-Tag>] (IDENTIFY-Reply)
// Wir picken den ASCII-Teil aus beiden Frames raus.
static void on_frame(void *ctx, uint8_t dst, uint8_t cnt,
                     const uint8_t *payload, size_t plen)
{
    char hex[3 * 32 + 1];
    hex_str(payload, plen, hex, sizeof(hex));
    ESP_LOGI(TAG, "RX frame: dst=0x%02X cnt=0x%02X len=%u | %s%s",
             dst, cnt, (unsigned)plen, hex, plen > 32 ? "..." : "");

    const uint8_t *tagp = NULL;
    size_t taglen = 0;

    if (dst == HMU_DST_OS && plen >= 2 && payload[0] == 0x00) {
        // Boot-Banner-Push: payload[0]=sub-cmd 0x00, danach ASCII-Tag
        tagp = payload + 1;
        taglen = plen - 1;
    } else if (dst == HMU_DST_COMMON && plen >= 2) {
        // IDENTIFY-Reply: typisch [01, 00, <tag>...] oder [01, <tag>...]
        if (plen >= 3 && payload[1] < 0x20) {
            tagp = payload + 2;
            taglen = plen - 2;
        } else {
            tagp = payload + 1;
            taglen = plen - 1;
        }
    } else {
        return;  // Frame, aber kein Tag
    }

    char tmp[32] = {0};
    size_t n = (taglen < sizeof(tmp) - 1) ? taglen : sizeof(tmp) - 1;
    memcpy(tmp, tagp, n);
    tmp[n] = '\0';
    while (n > 0 && (tmp[n - 1] == '\0' || tmp[n - 1] == ' '
                  || tmp[n - 1] == '\r' || tmp[n - 1] == '\n')) {
        tmp[--n] = '\0';
    }
    if (n == 0) return;  // leerer Tag → Ignor

    snprintf(s_tag, sizeof(s_tag), "%s", tmp);
    s_tag_kind = classify_tag(s_tag);
    s_got_tag = true;
    ESP_LOGW(TAG, ">>> Modul-Tag: '%s' (%s)", s_tag,
             s_tag_kind == TAG_KIND_APP ? "App" :
             s_tag_kind == TAG_KIND_BL  ? "BL"  : "?");
}

// ───── GPIO-Helper ──────────────────────────────────────────────────────

static void cfg_gpio_in_pullup(int pin)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = 1,
        .pull_down_en = 0,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static void cfg_gpio_out(int pin, int level)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = 0,
        .pull_down_en = 0,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(pin, level);
}

// Reset-Sequenz: output(0) 50ms → output(1) 50ms → INPUT MIT INTERNEM PULLUP.
// Wichtig: am HM-MOD-RPI-PCB ist die RST-Linie ohne externen Pull-Up — wir
// MÜSSEN den ESP-internen 45kΩ-Pullup aktivieren, sonst floatet die Linie
// wieder runter und das Modul hängt im Reset.
static void reset_pulse(int pin)
{
    cfg_gpio_out(pin, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(pin, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    cfg_gpio_in_pullup(pin);   // ← FIX: Pull-Up enabled, nicht high-Z
    vTaskDelay(pdMS_TO_TICKS(50));
}

// ───── UART-Helper ──────────────────────────────────────────────────────

static esp_err_t tx_frame(uint8_t dst, uint8_t cnt,
                          const uint8_t *payload, size_t plen,
                          const char *what)
{
    static uint8_t s_frame[HMU_MAX_FRAME_ESC];
    int n = hmu_frame_encode(dst, cnt, payload, plen, s_frame, sizeof(s_frame));
    if (n <= 0) {
        ESP_LOGE(TAG, "%s: encode failed (%d)", what, n);
        return ESP_FAIL;
    }
    char hex[3 * 32 + 1];
    hex_str(s_frame, n, hex, sizeof(hex));
    ESP_LOGW(TAG, "TX %s cnt=0x%02X (%d bytes): %s", what, cnt, n, hex);
    int wrote = uart_write_bytes(UART_NUM, s_frame, n);
    return wrote == n ? ESP_OK : ESP_FAIL;
}

// Lese UART eine bestimmte Zeit, feede alles in den Decoder.  Returns true
// wenn s_got_tag während der Lese-Phase gesetzt wurde.
static bool read_into_decoder(hmu_decoder_t *dec, int timeout_ms)
{
    static uint8_t buf[256];
    int rounds = (timeout_ms + 49) / 50;
    for (int i = 0; i < rounds && !s_got_tag; i++) {
        int rd = uart_read_bytes(UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (rd > 0) {
            char rxh[3 * 32 + 1];
            hex_str(buf, rd, rxh, sizeof(rxh));
            ESP_LOGI(TAG, "rx %d: %s%s", rd, rxh, rd > 32 ? "..." : "");
            hmu_decoder_feed(dec, buf, rd);   // ← FIX: jetzt wirklich feeden
        }
    }
    return s_got_tag;
}

// ───── Probe-Hauptlauf ─────────────────────────────────────────────────

void hm_probe_run(void)
{
    static const uint8_t IDENTIFY_PL[1]   = { 0x01 };
    static const uint8_t CHANGE_APP_PL[1] = { 0x03 };

    s_got_tag  = false;
    s_tag[0]   = '\0';
    s_tag_kind = TAG_KIND_NONE;

    ESP_LOGW(TAG, "==== HM-Modul-Probe (UART1 TX=GPIO%d RX=GPIO%d RST=GPIO%d @ %d) ====",
             PIN_TX, PIN_RX, PIN_RST, BAUD);

    // UART1 einrichten
    uart_config_t uc = {
        .baud_rate  = BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(UART_NUM, RX_BUF_SIZE, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config(UART_NUM, &uc) != ESP_OK ||
        uart_set_pin(UART_NUM, PIN_TX, PIN_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "UART1-init failed");
        return;
    }
    uart_flush_input(UART_NUM);

    static hmu_decoder_t dec;
    hmu_decoder_init(&dec, on_frame, NULL);

    // Reset-Pulse — Modul kommt frisch hoch und schickt Boot-Banner-Push
    reset_pulse(PIN_RST);
    ESP_LOGI(TAG, "reset done — boot-banner abwarten (1500 ms)");
    read_into_decoder(&dec, 1500);

    if (!s_got_tag) {
        ESP_LOGW(TAG, "kein Banner-Push — IDENTIFY explizit anstoßen");
        if (tx_frame(HMU_DST_COMMON, s_cnt++, IDENTIFY_PL, sizeof(IDENTIFY_PL),
                     "IDENTIFY (probe 1)") != ESP_OK) goto done;
        read_into_decoder(&dec, 1500);
    }

    if (!s_got_tag) {
        ESP_LOGE(TAG, "Modul antwortet nicht auf IDENTIFY");
        goto done;
    }

    // Falls Bootloader → CHANGE_APP.  Auf Legacy-HM-MOD-RPI-PCB lebt der
    // START_APP-Befehl in der OS/SYSTEM-Zone (dst=0x00, cmd=0x03), nicht
    // in COMMON.  Boot-Banner-Tag kam mit dst=0x00 — selbe Zone.
    // (HmIP-RFUSB DualCoPro nutzt dafür dst=COMMON+cmd=0x03; Path-Split
    // anhand Modul-Family bei späterem Multi-Modul-Support.)
    if (s_tag_kind == TAG_KIND_BL) {
        ESP_LOGW(TAG, "Modul in BL ('%s') → SYSTEM:START_APP", s_tag);
        s_got_tag = false;
        s_tag[0]  = '\0';
        if (tx_frame(HMU_DST_OS, s_cnt++, CHANGE_APP_PL, sizeof(CHANGE_APP_PL),
                     "SYSTEM_START_APP") != ESP_OK) goto done;
        read_into_decoder(&dec, 1500);
        if (!s_got_tag) {
            ESP_LOGE(TAG, "kein Push nach SYSTEM_START_APP");
            goto done;
        }
        if (s_tag_kind != TAG_KIND_APP) {
            ESP_LOGE(TAG, "nach START_APP unerwarteter Tag '%s' (kind=%d)",
                     s_tag, s_tag_kind);
            goto done;
        }
        ESP_LOGW(TAG, "Push-Tag bestätigt App: '%s'", s_tag);

        // Verify-IDENTIFY auf COMMON (cmd=0x01).  Auf manchen Modul-FW
        // antwortet das nicht, deshalb non-fatal — Banner-Push hat den
        // App-Tag bereits eindeutig geliefert.
        char saved_tag[sizeof(s_tag)];
        snprintf(saved_tag, sizeof(saved_tag), "%s", s_tag);
        tag_kind_t saved_kind = s_tag_kind;
        s_got_tag = false;
        s_tag[0]  = '\0';
        if (tx_frame(HMU_DST_COMMON, s_cnt++, IDENTIFY_PL, sizeof(IDENTIFY_PL),
                     "COMMON_IDENTIFY (verify)") != ESP_OK) goto done;
        read_into_decoder(&dec, 1500);
        if (!s_got_tag || s_tag_kind != TAG_KIND_APP) {
            ESP_LOGW(TAG, "Verify-IDENTIFY ohne sauberen Tag — App-Mode-Push war eindeutig");
            // Tag aus Push wiederherstellen
            snprintf(s_tag, sizeof(s_tag), "%s", saved_tag);
            s_tag_kind = saved_kind;
            s_got_tag  = true;
        }
    } else if (s_tag_kind == TAG_KIND_APP) {
        ESP_LOGW(TAG, "Modul bereits im App-Mode ('%s')", s_tag);
    } else {
        ESP_LOGE(TAG, "Tag '%s' unklassifiziert", s_tag);
        goto done;
    }

    ESP_LOGW(TAG, "==== ERGEBNIS: App-Mode-Tag = '%s' ====", s_tag);
    ESP_LOGW(TAG, "decoder-stats: ok=%u crc=%u trunc=%u skip=%u",
             (unsigned)dec.frames_ok, (unsigned)dec.frames_crc_err,
             (unsigned)dec.frames_truncated, (unsigned)dec.bytes_skipped);

done:
    uart_driver_delete(UART_NUM);
}
