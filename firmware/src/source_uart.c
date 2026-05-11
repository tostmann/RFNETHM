// SPDX-License-Identifier: GPL-2.0-or-later
//
// source_uart.c — HM-MOD-RPI-PCB / RPI-RF-MOD source-Implementation.
//
// Spiegelbild zu source_usb.c, aber für die UART-1-verkabelten Module
// am eQ3-Pinheader-Adapter.  Pin-Belegung (Stand 2026-05-07, siehe
// docs/breadboard_wiring.md):
//
//   GPIO17 → UART1 TX → eQ3-Pin 8  → Modul-RX (über R1 1K am HM-MOD-RPI-PCB)
//   GPIO18 → UART1 RX ← eQ3-Pin 10 ← Modul-TX (über R2 1K am HM-MOD-RPI-PCB)
//   GPIO16 → eQ3-Pin 12 → HM-MOD-RPI-PCB RST (TRX1.4 C2CK/RST, active LOW)
//   GPIO7  → eQ3-Pin 35 → RPI-RF-MOD     RST (alternative)
//
// Boot-Probe (verbatim vom hm_probe.c-Erfolgs-Run):
//   1. RST GPIO16 active-LOW pulse → Release mit ESP-internem Pull-Up
//   2. Boot-Banner abwarten → Tag (BL oder App)
//   3. Falls _BL: dst=OS cmd=0x03 SYSTEM_START_APP → wait Push
//   4. dst=COMMON cmd=0x01 IDENTIFY verify
//
// HM-MOD-RPI-PCB hat KEINEN externen Pull-Up auf RST; high-Z-Release
// floatet die Linie wieder runter → Modul hängt im Reset.  Daher
// MUSS die Release-Phase entweder Output-HIGH halten oder INPUT mit
// internem Pull-Up.

#include "source_uart.h"
#include "bridge.h"
#include "hmu_frame.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/uart.h"

static const char *TAG = "src-uart";

#define PIN_TX           17
#define PIN_RX           18
#define PIN_RST_HMMOD    16   // eQ3-Pin 12 (HM-MOD-RPI-PCB)
#define PIN_RST_RPIRF    7    // eQ3-Pin 35 (RPI-RF-MOD alt)

#define UART_NUM         UART_NUM_1
#define BAUD             115200
#define UART_RX_BUF      2048

typedef enum {
    BOOT_TAG_NONE = 0,
    BOOT_TAG_APP,
    BOOT_TAG_BL,
    BOOT_TAG_DUAL_ERR,
    BOOT_TAG_UNKNOWN,
} boot_class_t;

typedef struct {
    boot_class_t klass;
    char         tag[32];
} boot_event_t;

static struct {
    source_t          source;
    QueueHandle_t     boot_q;
    SemaphoreHandle_t tx_mtx;
    hmu_decoder_t     decoder;
    bool              ready;
    bool              module_present;
    bool              boot_done;
    bool              flash_lock;        // Modul im BL gehalten für Flash
    int64_t           flash_lock_us;     // Lock-Zeitstempel für Auto-Release
    char              app_tag[32];
    char              describe_buf[80];
} S;

// Flash-Lock auto-release nach 5 Min Inaktivität — schützt vor
// vergessenen Flash-Modi.
#define FLASH_LOCK_IDLE_TIMEOUT_US  (5 * 60 * 1000000ULL)

#include "esp_timer.h"

// ───── GPIO / Reset ─────────────────────────────────────────────────────

static void cfg_gpio_in_pull(int pin, int up, int down)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = up   ? 1 : 0,
        .pull_down_en = down ? 1 : 0,
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

// Reset-Sequenz mit Polaritäts-Wahl.  Release endet als Input mit dem
// passenden internen Pull (45 kΩ) damit die Linie auf dem Inactive-
// Pegel bleibt — HM-MOD und RPI-RF-MOD haben keinen externen Pull-Up
// auf RST.
//   active_high=0 (active-LOW):  output(0) 50ms → output(1) 50ms → input+pull-up   50ms
//   active_high=1 (active-HIGH): output(1) 50ms → output(0) 50ms → input+pull-down 50ms
static void reset_pulse_pol(int pin, int active_high)
{
    int assert_lvl  = active_high ? 1 : 0;
    int release_lvl = active_high ? 0 : 1;
    cfg_gpio_out(pin, assert_lvl);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(pin, release_lvl);
    vTaskDelay(pdMS_TO_TICKS(50));
    cfg_gpio_in_pull(pin, /*up=*/!active_high, /*down=*/active_high);
    vTaskDelay(pdMS_TO_TICKS(50));
}

// Hält pin im asserted-reset Zustand (Modul soll stumm bleiben).
static void hold_in_reset(int pin, int active_high)
{
    cfg_gpio_out(pin, active_high ? 1 : 0);
}

// ───── Tag-Klassifikation ──────────────────────────────────────────────

static boot_class_t classify_tag(const char *tag)
{
    size_t n = strlen(tag);
    if (n >= 4 && strcmp(tag + n - 4, "_App") == 0) return BOOT_TAG_APP;
    if (n >= 3 && (strcmp(tag + n - 3, "_Bl") == 0
                || strcmp(tag + n - 3, "_BL") == 0)) return BOOT_TAG_BL;
    return BOOT_TAG_UNKNOWN;
}

static void post_boot_event(boot_class_t klass, const char *tag)
{
    boot_event_t ev = { .klass = klass };
    snprintf(ev.tag, sizeof(ev.tag), "%s", tag ? tag : "");
    if (S.boot_q) xQueueSend(S.boot_q, &ev, 0);
}

static bool wait_boot_event(boot_event_t *out, TickType_t to)
{
    return S.boot_q && xQueueReceive(S.boot_q, out, to) == pdPASS;
}

static void drain_boot_q(void)
{
    boot_event_t ev;
    while (S.boot_q && xQueueReceive(S.boot_q, &ev, 0) == pdPASS) { }
}

// ───── HMU-Decoder-Callback ────────────────────────────────────────────

static void uart_frame_cb(void *ctx, uint8_t dst, uint8_t cnt,
                          const uint8_t *payload, size_t plen)
{
    (void)ctx;
    const char *dst_label =
        dst == HMU_DST_OS       ? "OS/HMSYSTEM"
      : dst == HMU_DST_APP      ? "APP/TRX"
      : dst == HMU_DST_HMIP     ? "HMIP"
      : dst == HMU_DST_LLMAC    ? "LLMAC"
      : dst == HMU_DST_COMMON   ? "COMMON"
      : dst == HMU_DST_DUAL_ERR ? "DUAL_ERR"
      :                           "?";
    ESP_LOGI(TAG, "FRAME OK  dst=0x%02X (%s)  cnt=0x%02X  payload[%u]",
             dst, dst_label, cnt, (unsigned)plen);

    if (dst == HMU_DST_DUAL_ERR) {
        post_boot_event(BOOT_TAG_DUAL_ERR, "(DUAL_ERR)");
        return;
    }

    // Tag-Frames: Boot-Banner kommt als dst=OS (legacy) ODER
    // dst=COMMON Reply auf IDENTIFY (DualCoPro / sub=0x05 oder 0x00).
    const uint8_t *tag_p = NULL;
    size_t taglen = 0;

    if (dst == HMU_DST_OS && plen >= 2 && payload[0] == 0x00) {
        tag_p  = payload + 1;
        taglen = plen - 1;
    } else if (dst == HMU_DST_COMMON && plen >= 3 && payload[0] == 0x05) {
        // IDENTIFY-Reply: [05, 01, <ASCII-Tag>]
        tag_p  = payload + 2;
        taglen = plen - 2;
    } else if (dst == HMU_DST_COMMON && plen >= 2 && payload[0] == 0x00) {
        // IDENTIFY-Reply Variante: [00, <ASCII-Tag>]
        tag_p  = payload + 1;
        taglen = plen - 1;
    } else {
        return;
    }

    char tag[32];
    if (taglen >= sizeof(tag)) taglen = sizeof(tag) - 1;
    memcpy(tag, tag_p, taglen);
    tag[taglen] = '\0';
    while (taglen > 0 && (tag[taglen - 1] == '\0' || tag[taglen - 1] == ' '
                       || tag[taglen - 1] == '\r' || tag[taglen - 1] == '\n')) {
        tag[--taglen] = '\0';
    }
    if (taglen == 0) return;

    boot_class_t klass = classify_tag(tag);
    ESP_LOGW(TAG, "App-Tag = '%s' (%s)", tag,
             klass == BOOT_TAG_APP ? "App"
           : klass == BOOT_TAG_BL  ? "Bootloader"
           :                         "unknown");
    post_boot_event(klass, tag);
}

// ───── TX-Helper für Boot-Probe + Source-tx ────────────────────────────

static esp_err_t encode_and_send(uint8_t dst, uint8_t cnt,
                                 const uint8_t *payload, size_t payload_len,
                                 const char *label)
{
    static uint8_t out[HMU_MAX_FRAME_ESC];
    int n = hmu_frame_encode(dst, cnt, payload, payload_len, out, sizeof(out));
    if (n < 0) {
        ESP_LOGE(TAG, "TX %s: encode failed", label);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "TX %s cnt=0x%02X (%d bytes)", label, cnt, n);
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    int wrote = uart_write_bytes(UART_NUM, out, (size_t)n);
    xSemaphoreGive(S.tx_mtx);
    return wrote == n ? ESP_OK : ESP_FAIL;
}

// Modul-Familie aus dem BL-Tag ableiten:
//   "HMIP_TRX_Bl"     → DualCoPro (HmIP-RFUSB / RPI-RF-MOD), CHANGE_APP via dst=COMMON
//   "Co_CPU_BL"       → Legacy SiLabs (HM-MOD-RPI-PCB),       CHANGE_APP via dst=OS
typedef enum {
    FAMILY_UNKNOWN = 0,
    FAMILY_DUALCOPRO,   // HMIP_TRX_Bl / DualCoPro_App
    FAMILY_LEGACY,      // Co_CPU_BL   / Co_CPU_App
} module_family_t;

static module_family_t family_from_tag(const char *tag)
{
    if (!tag || !*tag) return FAMILY_UNKNOWN;
    if (strncmp(tag, "HMIP_TRX", 8) == 0)   return FAMILY_DUALCOPRO;
    if (strncmp(tag, "DualCoPro", 9) == 0)  return FAMILY_DUALCOPRO;
    if (strncmp(tag, "Co_CPU", 6) == 0)     return FAMILY_LEGACY;
    return FAMILY_UNKNOWN;
}

// Polaritäten sind hardware-konstant (Schaltungs-Fakt, kein Discovery-Bedarf):
//   HM-MOD-RPI-PCB: active-LOW  (via piVCCU-Code, verifiziert 2026-05-07)
//   RPI-RF-MOD:     active-HIGH (empirisch verifiziert 2026-05-08, e2e-Flash 2026-05-11)
#define POL_HM_MOD     0
#define POL_RPI_RF     1

// Versucht eine Reset-Pulse-Polarität auf einem Pin und prüft ob danach
// ein Banner kommt.  Vorhandene Boot-Queue wird vorher gedrained.
static bool try_reset_get_banner(int pin, int active_high, int wait_ms,
                                 boot_event_t *ev_out)
{
    drain_boot_q();
    reset_pulse_pol(pin, active_high);
    return wait_boot_event(ev_out, pdMS_TO_TICKS(wait_ms));
}

// Reset+Banner-Discovery — pulst nacheinander beide RST-Pins mit ihrer
// hardware-konstanten Polarität.  Das jeweils nicht-getestete Modul
// bleibt im assert-reset, damit es nicht in den Banner-Burst hineinredet.
// Returns true wenn ein Banner kam; ev_out enthält dann Tag+Klasse.
static bool reset_and_get_banner(boot_event_t *ev_out)
{
    drain_boot_q();
    *ev_out = (boot_event_t){ .klass = BOOT_TAG_NONE };

    // Phase 1: HM-MOD testen, RPI-RF-MOD im assert-reset
    hold_in_reset(PIN_RST_RPIRF, POL_RPI_RF);
    ESP_LOGI(TAG, "Phase 1: HM-MOD-RST GPIO%d active-LOW (RPI-RF-MOD held in reset)",
             PIN_RST_HMMOD);
    if (try_reset_get_banner(PIN_RST_HMMOD, POL_HM_MOD, 600, ev_out)) {
        ESP_LOGW(TAG, "Banner via HM-MOD-RST — Modul ist HM-MOD-Familie");
        return true;
    }

    // Phase 2: RPI-RF-MOD testen, HM-MOD im assert-reset
    hold_in_reset(PIN_RST_HMMOD, POL_HM_MOD);
    ESP_LOGI(TAG, "Phase 2: RPI-RF-MOD-RST GPIO%d active-HIGH (HM-MOD held in reset)",
             PIN_RST_RPIRF);
    if (try_reset_get_banner(PIN_RST_RPIRF, POL_RPI_RF, 1500, ev_out)) {
        ESP_LOGW(TAG, "Banner via RPI-RF-MOD-RST — Modul ist RPI-RF-MOD-Familie");
        return true;
    }
    return false;
}

// Boot-Probe — full-cycle: RST+Banner-Discovery, dann CHANGE_APP wenn
// nötig, finaler App-Mode verifiziert.
static bool boot_probe(uint8_t *cnt_io)
{
    static const uint8_t IDENTIFY_PL[1]  = { 0x01 };
    static const uint8_t CHANGE_APP_PL[1] = { 0x03 };
    // 3500ms: RPI-RF-MOD/DualCoPro braucht nach CHANGE_APP empirisch
    // ~2200ms bis der App-Push kommt (Live-Probe 2026-05-11).  HM-MOD ist
    // schneller, aber early-return on success kostet hier nichts.
    const TickType_t T = pdMS_TO_TICKS(3500);

    boot_event_t ev = { .klass = BOOT_TAG_NONE };
    bool got_banner = reset_and_get_banner(&ev);

    // Kein Banner → expliziter IDENTIFY-Versuch (Modul evtl. schon in App)
    if (!got_banner) {
        ESP_LOGI(TAG, "kein Banner — IDENTIFY explicit (Modul evtl. schon im App-Mode)");
        if (encode_and_send(HMU_DST_COMMON, (*cnt_io)++,
                            IDENTIFY_PL, sizeof(IDENTIFY_PL),
                            "COMMON_IDENTIFY (probe)") != ESP_OK)
            return false;
        if (!wait_boot_event(&ev, T)) {
            ESP_LOGE(TAG, "boot: Modul stumm — kein RST-Pin trifft, oder Hardware nicht ready");
            return false;
        }
    }

    // Drain Folge-Frames die im selben Banner-Burst liegen
    boot_event_t extra;
    while (wait_boot_event(&extra, pdMS_TO_TICKS(200))) {
        if (ev.klass == BOOT_TAG_UNKNOWN || ev.klass == BOOT_TAG_NONE) ev = extra;
    }

    if (ev.klass == BOOT_TAG_APP) {
        ESP_LOGW(TAG, "boot: bereits App ('%s')", ev.tag);
    } else if (ev.klass == BOOT_TAG_BL) {
        module_family_t fam = family_from_tag(ev.tag);
        const char *label;
        uint8_t change_app_dst;
        switch (fam) {
        case FAMILY_DUALCOPRO:
            change_app_dst = HMU_DST_COMMON;
            label = "COMMON_CHANGE_APP (DualCoPro)";
            break;
        case FAMILY_LEGACY:
            change_app_dst = HMU_DST_OS;
            label = "SYSTEM_START_APP (Legacy)";
            break;
        default:
            ESP_LOGE(TAG, "boot: BL-Tag '%s' unbekannte Familie — kein CHANGE_APP-Pfad",
                     ev.tag);
            return false;
        }
        ESP_LOGW(TAG, "boot: BL ('%s', %s) → %s", ev.tag,
                 fam == FAMILY_DUALCOPRO ? "DualCoPro" : "Legacy", label);

        if (encode_and_send(change_app_dst, (*cnt_io)++,
                            CHANGE_APP_PL, sizeof(CHANGE_APP_PL),
                            label) != ESP_OK)
            return false;
        if (!wait_boot_event(&ev, T)) {
            ESP_LOGE(TAG, "boot: keine Push nach CHANGE_APP");
            return false;
        }
        if (ev.klass != BOOT_TAG_APP) {
            ESP_LOGE(TAG, "boot: erwarte App-Tag, got '%s' (klass=%d)",
                     ev.tag, ev.klass);
            return false;
        }
        ESP_LOGW(TAG, "boot: Push bestätigt App: '%s'", ev.tag);
        while (wait_boot_event(&extra, pdMS_TO_TICKS(200))) { }
    } else {
        ESP_LOGE(TAG, "boot: unklassifiziert '%s'", ev.tag);
        return false;
    }

    // Verify auf COMMON-Pfad — non-fatal
    char saved_tag[32];
    snprintf(saved_tag, sizeof(saved_tag), "%s", ev.tag);
    if (encode_and_send(HMU_DST_COMMON, (*cnt_io)++,
                        IDENTIFY_PL, sizeof(IDENTIFY_PL),
                        "COMMON_IDENTIFY (verify)") == ESP_OK) {
        if (wait_boot_event(&ev, T) && ev.klass == BOOT_TAG_APP) {
            ESP_LOGW(TAG, "boot: Verify-Reply '%s' bestätigt App-Mode", ev.tag);
        } else {
            ESP_LOGW(TAG, "boot: Verify ohne Tag — Push war eindeutig");
        }
        while (wait_boot_event(&extra, pdMS_TO_TICKS(200))) { }
    }

    snprintf(S.app_tag, sizeof(S.app_tag), "%s", saved_tag);
    return true;
}

// ───── RX-Pump-Task ────────────────────────────────────────────────────

static void uart_rx_task(void *arg)
{
    (void)arg;
    static uint8_t buf[256];
    while (1) {
        int rd = uart_read_bytes(UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(100));

        // Flash-Lock-Idle-Timeout prüfen (auch wenn 0 bytes ankamen)
        if (S.flash_lock && S.flash_lock_us > 0) {
            int64_t now = esp_timer_get_time();
            if (now - S.flash_lock_us > (int64_t)FLASH_LOCK_IDLE_TIMEOUT_US) {
                ESP_LOGW(TAG, "Flash-Lock auto-release nach 5 Min idle");
                S.flash_lock = false;
                S.flash_lock_us = 0;
            } else if (rd > 0) {
                S.flash_lock_us = now;  // Aktivität → Idle-Timer reset
            }
        }

        if (rd <= 0) continue;
        S.module_present = true;
        // Decoder füttern (Boot-Probe-Phase + reguläre Frames)
        hmu_decoder_feed(&S.decoder, buf, rd);
        // Fanout: immer wenn Bridge eine Source attached hat (rx_sink
        // ist gesetzt von bridge_attach_source).  Boot-Probe-Bytes UND
        // BL-Bytes UND App-Bytes laufen alle zu den Sinks — entspricht
        // CULFW32-Verhalten im hbrfeth_listener und passt zu piVCCU-
        // Semantik wo der Userspace selbst entscheidet was gerade kommt.
        // Sinks die das interpretieren (HMUARTLGW-Legacy-Emulation)
        // haben eigene State-Machine die mit allen Phasen klarkommt.
        if (S.source.rx_sink) {
            S.source.rx_sink(S.source.rx_sink_ctx, buf, rd);
        }
    }
}

// ───── source_t-Implementation-Hooks ────────────────────────────────────

static esp_err_t op_tx(source_t *src, const uint8_t *data, size_t len)
{
    (void)src;
    if (!data || !len) return ESP_ERR_INVALID_ARG;
    if (!S.ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    int wrote = uart_write_bytes(UART_NUM, data, len);
    xSemaphoreGive(S.tx_mtx);
    return wrote == (int)len ? ESP_OK : ESP_FAIL;
}

static bool op_ready(source_t *src) { (void)src; return S.ready; }

static esp_err_t op_reset(source_t *src)
{
    (void)src;
    S.ready     = false;
    S.boot_done = false;
    // boot_probe() macht eigene Reset-Sequenz mit Polaritäts-Discovery
    uint8_t cnt = 0x01;
    bool ok = boot_probe(&cnt);
    if (ok) {
        S.ready     = true;
        S.boot_done = true;
        return ESP_OK;
    }
    return ESP_FAIL;
}

static const char *op_describe(source_t *src)
{
    (void)src;
    snprintf(S.describe_buf, sizeof(S.describe_buf),
             "UART HM-MOD %s%s%s",
             S.module_present ? "present" : "absent",
             S.boot_done ? " App "  : "",
             S.app_tag[0] ? S.app_tag : "");
    return S.describe_buf;
}

static const struct source_ops s_uart_ops = {
    .tx       = op_tx,
    .ready    = op_ready,
    .reset    = op_reset,
    .describe = op_describe,
};

// ───── Init-Task (Boot-Probe + danach RX-Pump) ─────────────────────────

static void init_task(void *arg)
{
    (void)arg;
    // 300 ms Settle nach UART-Setup
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "starting boot probe (HM-MOD-RPI-PCB hypothesis):");
    uint8_t cnt = 0x01;
    if (boot_probe(&cnt)) {
        S.boot_done = true;
        S.ready     = true;
        ESP_LOGW(TAG, "UART-Source READY ('%s')", S.app_tag);
    } else {
        ESP_LOGE(TAG, "Boot-Probe fehlgeschlagen — Source bleibt nicht-ready");
    }

    // Pump-Task läuft schon (uart_rx_task wurde in source_uart_init gestartet),
    // boot_done==true gibt jetzt den Fanout frei.
    vTaskDelete(NULL);
}

// ───── Public init ─────────────────────────────────────────────────────

source_t *source_uart_init(void)
{
    memset(&S, 0, sizeof(S));
    S.boot_q          = xQueueCreate(8, sizeof(boot_event_t));
    S.tx_mtx          = xSemaphoreCreateMutex();
    S.source.ops      = &s_uart_ops;
    S.source.short_id = "uart";

    hmu_decoder_init(&S.decoder, uart_frame_cb, NULL);

    // UART1 einrichten
    uart_config_t uc = {
        .baud_rate  = BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(UART_NUM, UART_RX_BUF, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config(UART_NUM, &uc) != ESP_OK ||
        uart_set_pin(UART_NUM, PIN_TX, PIN_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "UART1 install failed");
        return NULL;
    }
    uart_flush_input(UART_NUM);

    xTaskCreate(uart_rx_task, "src-uart-rx", 4096, NULL, 5, NULL);
    xTaskCreate(init_task,    "src-uart-init", 4096, NULL, 4, NULL);

    return &S.source;
}

void source_uart_get_stats(source_uart_stats_t *out)
{
    if (!out) return;
    out->frames_ok        = S.decoder.frames_ok;
    out->frames_crc_err   = S.decoder.frames_crc_err;
    out->frames_truncated = S.decoder.frames_truncated;
    out->bytes_skipped    = S.decoder.bytes_skipped;
    out->module_present   = S.module_present;
    out->boot_done        = S.boot_done;
    out->flash_lock       = S.flash_lock;
    snprintf(out->app_tag, sizeof(out->app_tag), "%s", S.app_tag);
}

bool source_uart_is_flash_locked(void)
{
    return S.flash_lock;
}

esp_err_t source_uart_pulse_rst_only(char *tag_out, size_t tag_cap)
{
    if (tag_out && tag_cap) tag_out[0] = '\0';

    ESP_LOGW(TAG, "==== pulse_rst_only — HW-Layer-Primitive (kein flash_lock) ====");

    // Modul-State markieren: nach Pulse ist's im BL
    S.boot_done = false;
    S.app_tag[0] = '\0';
    // S.ready BLEIBT true — UART-TX-Pfad bleibt offen, bmcond/multimacd
    // kann sofort weiterarbeiten ohne erst auf Probe-Cycle zu warten.

    boot_event_t ev = { .klass = BOOT_TAG_NONE };
    bool got = reset_and_get_banner(&ev);

    boot_event_t extra;
    while (wait_boot_event(&extra, pdMS_TO_TICKS(200))) {
        if (ev.klass == BOOT_TAG_UNKNOWN || ev.klass == BOOT_TAG_NONE) ev = extra;
    }

    if (!got) {
        ESP_LOGE(TAG, "pulse_rst_only: kein Banner-Tag empfangen");
        return ESP_FAIL;
    }

    snprintf(S.app_tag, sizeof(S.app_tag), "%s", ev.tag);
    if (tag_out && tag_cap) {
        snprintf(tag_out, tag_cap, "%s", ev.tag);
    }
    ESP_LOGW(TAG, "pulse_rst_only done — Tag '%s' (%s)", ev.tag,
             ev.klass == BOOT_TAG_BL ? "BL" :
             ev.klass == BOOT_TAG_APP ? "App" : "?");
    return ESP_OK;
}

esp_err_t source_uart_reset_for_flash(bool hold_in_bl,
                                      char *tag_out, size_t tag_cap)
{
    if (tag_out && tag_cap) tag_out[0] = '\0';

    if (hold_in_bl) {
        ESP_LOGW(TAG, "==== reset_for_flash(hold_in_bl=true) — Modul wird im BL gehalten ====");
        S.boot_done  = false;
        S.app_tag[0] = '\0';

        // Reset + Banner-Discovery (gleicher Pfad wie boot_probe)
        boot_event_t ev = { .klass = BOOT_TAG_NONE };
        bool got = reset_and_get_banner(&ev);

        // Drain weitere Banner-Frames im selben Burst
        boot_event_t extra;
        while (wait_boot_event(&extra, pdMS_TO_TICKS(200))) {
            if (ev.klass == BOOT_TAG_UNKNOWN || ev.klass == BOOT_TAG_NONE) ev = extra;
        }

        if (!got) {
            ESP_LOGE(TAG, "reset_for_flash: kein Banner-Tag empfangen");
            S.flash_lock = false;
            return ESP_FAIL;
        }

        // Tag merken (egal ob BL oder App), für /api/status sichtbar
        snprintf(S.app_tag, sizeof(S.app_tag), "%s", ev.tag);
        if (tag_out && tag_cap) {
            snprintf(tag_out, tag_cap, "%s", ev.tag);
        }

        // Flash-Lock setzen — RX-Fanout offen, Supervisor swapt nicht weg
        S.flash_lock    = true;
        S.flash_lock_us = esp_timer_get_time();
        S.ready         = true;   // TX-Pfad zur Bridge offen
        ESP_LOGW(TAG, "==== Flash-Lock aktiv — Tag '%s' (%s) ====", ev.tag,
                 ev.klass == BOOT_TAG_BL ? "BL" :
                 ev.klass == BOOT_TAG_APP ? "App" : "?");
        return ESP_OK;
    }

    // hold_in_bl=false → vollständigen boot_probe + Lock release
    ESP_LOGW(TAG, "==== reset_for_flash(hold_in_bl=false) — voller App-Cycle ====");
    S.flash_lock = false;
    S.flash_lock_us = 0;
    S.ready      = false;
    S.boot_done  = false;
    uint8_t cnt = 0x01;
    bool ok = boot_probe(&cnt);
    if (ok) {
        S.ready     = true;
        S.boot_done = true;
        if (tag_out && tag_cap) snprintf(tag_out, tag_cap, "%s", S.app_tag);
        return ESP_OK;
    }
    return ESP_FAIL;
}
