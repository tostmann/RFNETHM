// SPDX-License-Identifier: GPL-2.0-or-later
//
// source_usb.c — HmIP-RFUSB-Source-Implementierung.
//
// Macht den ESP32-S3-USB-Host für den eq-3-HmIP-RFUSB-Stick auf, fährt
// die bmcond-konservative CP210x-Init-Sequenz, führt die Mode-aware
// Boot-Probe (IDENTIFY → ggf. CHANGE_APP → verify-IDENTIFY) und beginnt
// danach Bulk-IN-URB-Bytes via bridge_on_source_rx() in die Bridge zu
// fanout-en.
//
// Der gesamte Code in dieser Datei stammt aus main.c v0.4 — v0.5 ist
// nur Refactoring (Bewegung in eigenes Modul + source_t-Interface), kein
// Funktions-Change.  Hard-Guards (CP210x-OTP-Requests 0x40/0xFF NICHT
// senden) sind hier dupliziert, weil dieses Modul self-contained sein soll.
//
// ┌─────────────────────────── HARD GUARD ───────────────────────────┐
// │  NEVER send CP210x vendor requests 0x40 (VENDOR_SPECIFIC) or     │
// │  0xFF (VENDOR_SPECIFIC_2) — they write OTP and destroy the eq-3 │
// │  identity.  Our 6-step bmcond init is volatile-only.             │
// └───────────────────────────────────────────────────────────────────┘

#include "source_usb.h"
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

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

static const char *TAG = "src-usb";

#ifndef VBUS_GPIO
#define VBUS_GPIO          -1
#endif
#ifndef VBUS_ACTIVE_HIGH
#define VBUS_ACTIVE_HIGH    1
#endif

#define HMIP_RFUSB_VID  0x1B1F
#define HMIP_RFUSB_PID  0xC020

// CP210x vendor requests (volatile-only).
#define CP210X_REQTYPE_OUT      (USB_BM_REQUEST_TYPE_TYPE_VENDOR  | \
                                 USB_BM_REQUEST_TYPE_RECIP_INTERFACE | \
                                 USB_BM_REQUEST_TYPE_DIR_OUT)
#define CP210X_IFC_ENABLE       0x00
#define CP210X_SET_LINE_CTL     0x03
#define CP210X_SET_MHS          0x07
#define CP210X_PURGE            0x12
#define CP210X_SET_FLOW         0x13
#define CP210X_SET_BAUDRATE     0x1E

static const uint8_t s_set_flow_payload[16] = {
    0x01, 0x00, 0x00, 0x00,   0x40, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00,   0x40, 0x00, 0x00, 0x00,
};
static const uint8_t s_baud_115200_le[4] = { 0x00, 0xC2, 0x01, 0x00 };

// ───── Modul-State ──────────────────────────────────────────────────────

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
    source_t                  source;
    cdc_acm_dev_hdl_t         cdc;            // current handle, NULL if disconnected
    SemaphoreHandle_t         disconnect_sem;
    QueueHandle_t             boot_q;
    hmu_decoder_t             decoder;
    SemaphoreHandle_t         tx_mtx;         // serialize tx() from sinks vs. boot probe
    bool                      ready;          // source.ops->ready()
    bool                      stick_connected;
    bool                      boot_done;
    char                      app_tag[32];
    char                      describe_buf[80];
} S;

// ───── Helpers ──────────────────────────────────────────────────────────

static void vbus_init(void)
{
    if (VBUS_GPIO < 0) {
        ESP_LOGI(TAG, "VBUS: passive 5V tie (no GPIO switch)");
        return;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << VBUS_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = 0,
        .pull_down_en = 0,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(VBUS_GPIO, VBUS_ACTIVE_HIGH ? 1 : 0);
    ESP_LOGI(TAG, "VBUS: GPIO%d driven %s",
             (int)VBUS_GPIO, VBUS_ACTIVE_HIGH ? "HIGH" : "LOW");
}

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

// ───── HMU-Decoder-Callback (für Boot-Probe-Frame-Parsing) ──────────────

static void usb_frame_cb(void *ctx, uint8_t dst, uint8_t cnt,
                         const uint8_t *payload, size_t payload_len)
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
    ESP_LOGW(TAG, "FRAME OK  dst=0x%02X (%s)  cnt=0x%02X  payload[%u]:",
             dst, dst_label, cnt, (unsigned)payload_len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, payload, payload_len, ESP_LOG_WARN);

    if (dst == HMU_DST_DUAL_ERR) {
        post_boot_event(BOOT_TAG_DUAL_ERR, "(DUAL_ERR)");
        return;
    }
    if (dst != HMU_DST_COMMON || payload_len < 2) return;

    const uint8_t *tag_p = NULL;
    size_t taglen = 0;
    if (payload[0] == 0x05 && payload_len >= 3) {
        tag_p  = payload + 2; taglen = payload_len - 2;
    } else if (payload[0] == 0x00 && payload_len >= 2) {
        tag_p  = payload + 1; taglen = payload_len - 1;
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

    boot_class_t klass = classify_tag(tag);
    ESP_LOGW(TAG, "App-Tag = '%s' (%s)", tag,
             klass == BOOT_TAG_APP ? "App"
           : klass == BOOT_TAG_BL  ? "Bootloader"
           :                         "unknown");
    post_boot_event(klass, tag);
}

// ───── cdc_acm-Callbacks ────────────────────────────────────────────────

static bool cdc_data_cb(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    ESP_LOGD(TAG, "RX %u bytes (raw, on-wire):", (unsigned)len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, data, len, ESP_LOG_DEBUG);

    // Always feed the boot-probe decoder.
    hmu_decoder_feed(&S.decoder, data, len);

    // Once the boot probe has finished, fanout to bridge sinks.
    // (Vor boot_done bleiben die Bytes „nur" in der Decoder-Welt — die
    // Sinks sehen Stick-Verkehr erst wenn der App-Mode bestätigt ist.)
    if (S.boot_done && S.source.rx_sink) {
        S.source.rx_sink(S.source.rx_sink_ctx, data, len);
    }
    return true;
}

static void cdc_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "stick error %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "stick disconnected");
        if (S.disconnect_sem) xSemaphoreGive(S.disconnect_sem);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "stick serial-state 0x%04X", event->data.serial_state.val);
        break;
    case CDC_ACM_HOST_NETWORK_CONNECTION:
        break;
    default:
        break;
    }
}

// ───── CP210x bmcond-Init-Sequenz (volatile only) ───────────────────────

static esp_err_t cp210x_init(cdc_acm_dev_hdl_t cdc)
{
    struct ctrl_step {
        uint8_t        bRequest;
        uint16_t       wValue;
        uint16_t       wLen;
        const uint8_t *payload;
        const char    *label;
    };
    static const struct ctrl_step steps[] = {
        { CP210X_IFC_ENABLE,   0x0001, 0,  NULL,                "1. IFC_ENABLE on" },
        { CP210X_SET_MHS,      0x0303, 0,  NULL,                "2. SET_MHS DTR=1 RTS=1" },
        { CP210X_SET_LINE_CTL, 0x0800, 0,  NULL,                "3. SET_LINE_CTL 8N1" },
        { CP210X_SET_FLOW,     0x0000, 16, s_set_flow_payload,  "4. SET_FLOW (no HW flow)" },
        { CP210X_SET_BAUDRATE, 0x0000, 4,  s_baud_115200_le,    "5. SET_BAUDRATE 115200" },
        { CP210X_PURGE,        0x000F, 0,  NULL,                "6. PURGE TX+RX+queues" },
    };

    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        esp_err_t err = cdc_acm_host_send_custom_request(
            cdc, CP210X_REQTYPE_OUT,
            steps[i].bRequest, steps[i].wValue, 0,
            steps[i].wLen, (uint8_t *)steps[i].payload);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "  %s — failed: %s", steps[i].label, esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "  %s — OK", steps[i].label);
    }
    return ESP_OK;
}

// ───── Boot-Probe (IDENTIFY → CHANGE_APP if BL → verify IDENTIFY) ──────

static esp_err_t encode_and_send(uint8_t dst, uint8_t cnt,
                                 const uint8_t *payload, size_t payload_len,
                                 const char *label)
{
    uint8_t out[HMU_MAX_FRAME_ESC];
    int n = hmu_frame_encode(dst, cnt, payload, payload_len, out, sizeof(out));
    if (n < 0) {
        ESP_LOGE(TAG, "TX %s: encode failed", label);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "TX %s cnt=0x%02X (%d bytes):", label, cnt, n);
    ESP_LOG_BUFFER_HEXDUMP(TAG, out, (size_t)n, ESP_LOG_INFO);

    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    cdc_acm_dev_hdl_t cdc = S.cdc;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (cdc) {
        err = cdc_acm_host_data_tx_blocking(cdc, out, (size_t)n, pdMS_TO_TICKS(500));
    }
    xSemaphoreGive(S.tx_mtx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TX %s: tx_blocking failed: %s", label, esp_err_to_name(err));
    }
    return err;
}

static bool wait_boot_event(boot_event_t *out, TickType_t to)
{
    return xQueueReceive(S.boot_q, out, to) == pdPASS;
}

static void drain_boot_q(void)
{
    boot_event_t ev;
    while (xQueueReceive(S.boot_q, &ev, 0) == pdPASS) { }
}

static bool boot_to_app(uint8_t *cnt_io)
{
    const TickType_t T = pdMS_TO_TICKS(1500);

    drain_boot_q();

    if (encode_and_send(HMU_DST_COMMON, (*cnt_io)++,
                        HMU_PL_IDENTIFY, sizeof(HMU_PL_IDENTIFY),
                        "COMMON_IDENTIFY (probe 1)") != ESP_OK)
        return false;

    boot_event_t ev;
    if (!wait_boot_event(&ev, T)) {
        ESP_LOGE(TAG, "boot: stumm auf IDENTIFY (1)");
        return false;
    }
    boot_event_t extra;
    while (wait_boot_event(&extra, pdMS_TO_TICKS(200))) {
        if (ev.klass == BOOT_TAG_UNKNOWN || ev.klass == BOOT_TAG_NONE) ev = extra;
    }

    if (ev.klass == BOOT_TAG_APP || ev.klass == BOOT_TAG_DUAL_ERR) {
        ESP_LOGW(TAG, "boot: bereits App ('%s')", ev.tag);
    } else if (ev.klass == BOOT_TAG_BL) {
        ESP_LOGW(TAG, "boot: BL ('%s') → CHANGE_APP", ev.tag);
        if (encode_and_send(HMU_DST_COMMON, (*cnt_io)++,
                            HMU_PL_CHANGE_APP, sizeof(HMU_PL_CHANGE_APP),
                            "COMMON_CHANGE_APP") != ESP_OK)
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

    if (encode_and_send(HMU_DST_COMMON, (*cnt_io)++,
                        HMU_PL_IDENTIFY, sizeof(HMU_PL_IDENTIFY),
                        "COMMON_IDENTIFY (verify)") != ESP_OK)
        return false;
    if (!wait_boot_event(&ev, T)) {
        ESP_LOGE(TAG, "boot: stumm auf verify-IDENTIFY");
        return false;
    }
    if (ev.klass != BOOT_TAG_APP && ev.klass != BOOT_TAG_DUAL_ERR) {
        ESP_LOGE(TAG, "boot: verify gescheitert ('%s' klass=%d)",
                 ev.tag, ev.klass);
        return false;
    }
    ESP_LOGW(TAG, "boot: App-Mode verifiziert ('%s')", ev.tag);
    while (wait_boot_event(&extra, pdMS_TO_TICKS(200))) { }

    snprintf(S.app_tag, sizeof(S.app_tag), "%s", ev.tag);
    return true;
}

// ───── Stick-Task (open / init / probe / wait disconnect / repeat) ─────

static void stick_task(void *arg)
{
    (void)arg;
    while (1) {
        cdc_acm_dev_hdl_t cdc = NULL;
        const cdc_acm_host_device_config_t cfg = {
            .connection_timeout_ms = 5000,
            .out_buffer_size       = 256,
            .in_buffer_size        = 256,
            .event_cb              = cdc_event_cb,
            .data_cb               = cdc_data_cb,
            .user_arg              = NULL,
        };

        ESP_LOGI(TAG, "open %04X:%04X (vendor-specific) ...",
                 HMIP_RFUSB_VID, HMIP_RFUSB_PID);
        esp_err_t err = cdc_acm_host_open_vendor_specific(
            HMIP_RFUSB_VID, HMIP_RFUSB_PID, 0, &cfg, &cdc);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "open failed: %s — retry in 2s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        S.cdc             = cdc;
        S.stick_connected = true;
        S.boot_done       = false;
        S.app_tag[0]      = '\0';

        ESP_LOGI(TAG, "open ok — running CP210x init:");
        if (cp210x_init(cdc) != ESP_OK) {
            ESP_LOGE(TAG, "init failed; closing");
            S.cdc = NULL;
            cdc_acm_host_close(cdc);
            S.stick_connected = false;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        hmu_decoder_init(&S.decoder, usb_frame_cb, NULL);
        drain_boot_q();
        vTaskDelay(pdMS_TO_TICKS(300));   // settle after PURGE

        ESP_LOGI(TAG, "starting boot probe:");
        uint8_t cnt = 0x01;
        bool ok = boot_to_app(&cnt);
        if (ok) {
            S.boot_done = true;
            S.ready     = true;
            ESP_LOGW(TAG, "stick READY (%s) — cnt now 0x%02X", S.app_tag, cnt);
        } else {
            ESP_LOGE(TAG, "boot FAILED — open until disconnect for diagnosis");
        }

        // Block until the stick disconnects.  RX continues to flow via
        // cdc_data_cb → bridge fanout (boot_done==true).
        xSemaphoreTake(S.disconnect_sem, portMAX_DELAY);

        ESP_LOGI(TAG, "decoder stats — ok=%u crc_err=%u trunc=%u skip=%u",
                 (unsigned)S.decoder.frames_ok,
                 (unsigned)S.decoder.frames_crc_err,
                 (unsigned)S.decoder.frames_truncated,
                 (unsigned)S.decoder.bytes_skipped);
        S.ready           = false;
        S.boot_done       = false;
        S.stick_connected = false;
        S.cdc             = NULL;
        cdc_acm_host_close(cdc);
        ESP_LOGI(TAG, "closed; waiting for reconnect");
    }
}

// ───── source_t-Implementation-Hooks ────────────────────────────────────

static esp_err_t op_tx(source_t *src, const uint8_t *data, size_t len)
{
    (void)src;
    if (!data || !len) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    cdc_acm_dev_hdl_t cdc = S.cdc;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (cdc) {
        err = cdc_acm_host_data_tx_blocking(cdc, (uint8_t *)data, len,
                                            pdMS_TO_TICKS(500));
    }
    xSemaphoreGive(S.tx_mtx);
    return err;
}

static bool op_ready(source_t *src) { (void)src; return S.ready; }

static esp_err_t op_reset(source_t *src)
{
    (void)src;
    // VBUS-FET-Power-Cycle wäre hier möglich wenn VBUS_GPIO >= 0.
    // Auf YD-V1.4 (passive) ist es ein No-op.
    if (VBUS_GPIO >= 0) {
        gpio_set_level(VBUS_GPIO, VBUS_ACTIVE_HIGH ? 0 : 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(VBUS_GPIO, VBUS_ACTIVE_HIGH ? 1 : 0);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "reset: no VBUS-FET on this board — no-op");
    return ESP_ERR_NOT_SUPPORTED;
}

static const char *op_describe(source_t *src)
{
    (void)src;
    snprintf(S.describe_buf, sizeof(S.describe_buf),
             "USB %04X:%04X %s%s%s",
             HMIP_RFUSB_VID, HMIP_RFUSB_PID,
             S.stick_connected ? "connected" : "disconnected",
             S.boot_done       ? " App "      : "",
             S.app_tag[0]      ? S.app_tag    : "");
    return S.describe_buf;
}

static const struct source_ops s_usb_ops = {
    .tx       = op_tx,
    .ready    = op_ready,
    .reset    = op_reset,
    .describe = op_describe,
};

static void usb_lib_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) ESP_LOGI(TAG, "lib NO_CLIENTS");
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)   ESP_LOGI(TAG, "lib ALL_FREE");
    }
}

// ───── Public init ──────────────────────────────────────────────────────

source_t *source_usb_init(void)
{
    memset(&S, 0, sizeof(S));
    S.disconnect_sem  = xSemaphoreCreateBinary();
    S.boot_q          = xQueueCreate(8, sizeof(boot_event_t));
    S.tx_mtx          = xSemaphoreCreateMutex();
    S.source.ops      = &s_usb_ops;
    S.source.short_id = "usb";

    vbus_init();

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 5, NULL);

    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));
    xTaskCreate(stick_task, "stick", 6144, NULL, 4, NULL);

    return &S.source;
}

void source_usb_get_stats(source_usb_stats_t *out)
{
    if (!out) return;
    out->frames_ok        = S.decoder.frames_ok;
    out->frames_crc_err   = S.decoder.frames_crc_err;
    out->frames_truncated = S.decoder.frames_truncated;
    out->bytes_skipped    = S.decoder.bytes_skipped;
    out->stick_connected  = S.stick_connected;
    out->boot_done        = S.boot_done;
    snprintf(out->app_tag, sizeof(out->app_tag), "%s", S.app_tag);
}
