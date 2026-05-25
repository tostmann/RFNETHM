// SPDX-License-Identifier: GPL-2.0-or-later

#include "sink_hmuartlgw_legacy.h"
#include "hmu_frame.h"
#include "version.h"
#include "bridge.h"
#include "source_usb.h"
#include "source_uart.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "nvs.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "sink-hmu-l";

#define NVS_NS_AES  "rfnh_aes"
#define NVS_KEY_CUR  "cur"
#define NVS_KEY_PRV  "prv"
#define NVS_KEY_TMP  "tmp"

// Forward decls (Phase D — werden weiter unten definiert).
static bool is_bypass_mode(void);
static void persist_aes_key(const char *key, const uint8_t *blob, size_t len);

// HMUARTLGW-Konstanten — konsolidiert aus CULFW32 frontend_hmuartlgw +
// FHEM 00_HMUARTLGW.pm.

// dst-Layer (siehe hmu_frame.h)
#define DST_OS         HMU_DST_OS
#define DST_APP        HMU_DST_APP
#define DST_LLMAC      HMU_DST_LLMAC

// LLMAC (DualCoPro) commands — Subset für Phase B (TX/ACK).
#define LLMAC_ACK      0x01
#define LLMAC_RECV     0x05
#define LLMAC_SEND     0x06

// OS-Layer commands
#define OS_GET_APP            0x00
#define OS_GET_FIRMWARE       0x02
#define OS_CHANGE_APP         0x03
#define OS_ACK                0x04
#define OS_UPDATE_FIRMWARE    0x05
#define OS_NORMAL_MODE        0x06
#define OS_UPDATE_MODE        0x07
#define OS_GET_CREDITS        0x08
#define OS_ENABLE_CREDITS     0x09
#define OS_ENABLE_CSMACA      0x0a
#define OS_GET_SERIAL         0x0b
#define OS_SET_TIME           0x0e

// APP-Layer commands
#define APP_SET_HMID          0x00
#define APP_GET_HMID          0x01
#define APP_SEND              0x02
#define APP_SET_CURRENT_KEY   0x03
#define APP_ACK               0x04
#define APP_RECV              0x05
#define APP_ADD_PEER          0x06
#define APP_REMOVE_PEER       0x07
#define APP_GET_PEERS         0x08
#define APP_PEER_ADD_AES      0x09
#define APP_PEER_REMOVE_AES   0x0a
#define APP_SET_TEMP_KEY      0x0b
#define APP_SET_PREVIOUS_KEY  0x0f
#define APP_DEFAULT_HMID      0x10
#define APP_TRX_GET_MCU_TYPE  0x12

// ACK-Status
#define ACK             0x04
#define ACK_OK          0x01
#define ACK_INFO        0x02
#define ACK_MULTIPART   0x07
#define ACK_EUNKNOWN    0x84
#define ACK_ENOCREDITS  0x05
#define ACK_ECSMACA     0x06

typedef enum {
    HOST_AWAITING_PROBE,    // Hello (Co_CPU_BL) gepushed, warten auf erste OS-Probe
    HOST_FHEM,              // OS_GET_APP zuerst → FHEM
    HOST_HOMEGEAR,          // OS_CHANGE_APP zuerst → Homegear/rfd
    HOST_ACTIVE,            // Init durch
} host_mode_t;

typedef struct {
    int                 sock;
    uint32_t            ip;
    uint16_t            port;
    bool                active;
    TaskHandle_t        task;
    host_mode_t         host;
    hmu_decoder_t       dec;
    esp_timer_handle_t  hello_timer;
    bool                hello_repeating;
} hmu_client_t;

// Pending-ACK-Tracking (Phase B): wenn FHEM ein APP_SEND schickt, packagen
// wir es zu LLMAC_SEND um, schicken's an den Stick, und merken uns
// (cnt → client) damit der zurückkommende LLMAC_ACK an den richtigen
// FHEM-Client geroutet werden kann.  Default 8 Slots; Multi-Client +
// Rapid-Fire-Coexistence wird in Phase D verfeinert (mit cnt-Räumen
// pro Client).
typedef struct {
    bool          active;
    uint8_t       cnt;
    hmu_client_t *client;
    int64_t       sent_us;
} pending_ack_t;
#define PENDING_ACK_SLOTS  8
#define PENDING_ACK_TIMEOUT_US  (300 * 1000)   // 300 ms — FHEM-Default ~95

static struct {
    sink_t              self;
    uint16_t            port;
    int                 listen_sock;
    SemaphoreHandle_t   mtx;
    hmu_client_t        clients[SINK_HMUARTLGW_LEGACY_MAX_CLIENTS];
    sink_hmuartlgw_legacy_stats_t stats;

    uint8_t             hmid[3];          // 3-Byte HMID, default aus MAC abgeleitet
    char                serial[11];       // 10-Byte ASCII serial + NUL
    // Firmware-Triplet (1.4.1 → FHEM zeigt nicht "outdated").
    uint8_t             fw_major;
    uint8_t             fw_minor;
    uint8_t             fw_patch;

    // Source-Side Frame-Decoder — parst die Bytes die vom HmIP-RFUSB-Stick
    // via Bridge zu uns kommen (Bulk-IN-Frames in DualCoPro-Format).
    hmu_decoder_t       src_dec;
    pending_ack_t       pending[PENDING_ACK_SLOTS];

    // Phase C: cnt-Counter für unsolicited RX-Frames (RFNETHM → FHEM).
    // Spiegelt das Verhalten der echten HM-MOD-RPI-PCB, die bei jedem
    // neuen RX einen lokalen Counter hochzählt (verifiziert 2026-04-30
    // gegen echte Hardware am Pi5-Header).
    uint8_t             rx_cnt;
} S;

// ─── Frame-Send (mit Escape + CRC) ─────────────────────────────────────

static void send_frame(hmu_client_t *c, uint8_t dst, uint8_t cnt,
                       const uint8_t *payload, size_t plen)
{
    // Stack-Allokation: Race-frei zwischen client_task, hello_trampoline,
    // route_llmac_ack_to_fhem (verschiedene Tasks).  HMU_MAX_FRAME_ESC ist
    // ~1 KB, client_task hat 6 KB Stack — ok.
    uint8_t buf[HMU_MAX_FRAME_ESC];
    int n = hmu_frame_encode(dst, cnt, payload, plen, buf, sizeof(buf));
    if (n < 0) {
        ESP_LOGW(TAG, "frame_encode failed (plen=%u)", (unsigned)plen);
        return;
    }
    if (c->sock < 0) return;
    int sent = send(c->sock, buf, n, 0);
    if (sent < 0) {
        ESP_LOGW(TAG, "send failed errno=%d", errno);
    } else {
        S.stats.frames_tx_to_fhem++;
    }
}

// ─── Hello-Push: "Co_CPU_BL" 200 ms nach Connect, dann alle 3 s ─────────

static void send_hello(hmu_client_t *c)
{
    // dst=0x00, payload=[ACK=0x04, ACK_INFO=0x02, "Co_CPU_BL"]
    static const uint8_t hello[] = {
        ACK, ACK_INFO,
        'C','o','_','C','P','U','_','B','L'
    };
    send_frame(c, DST_OS, 0, hello, sizeof(hello));
    S.stats.hello_pushes++;
    ESP_LOGI(TAG, "→ Co_CPU_BL push to %d.%d.%d.%d:%u",
             (int)((c->ip      ) & 0xff), (int)((c->ip >>  8) & 0xff),
             (int)((c->ip >> 16) & 0xff), (int)((c->ip >> 24) & 0xff),
             (unsigned)c->port);
}

static void hello_trampoline(void *arg)
{
    hmu_client_t *c = (hmu_client_t *)arg;
    if (!c->active) return;
    // Mode-Check: nach erster Probe stoppen.
    if (c->host != HOST_AWAITING_PROBE) {
        c->hello_repeating = false;
        return;
    }
    send_hello(c);
    if (c->hello_repeating) {
        // Re-arm alle 3 s.
        esp_timer_start_once(c->hello_timer, 3 * 1000 * 1000);
    }
}

static void schedule_hello(hmu_client_t *c)
{
    if (is_bypass_mode()) {
        // Co_CPU_App-Source pusht ihren eigenen Hello-Frame; wir wären
        // doppelt.  Skip in Phase-D-Bypass.
        c->host = HOST_ACTIVE;
        return;
    }
    esp_timer_create_args_t args = {
        .callback = hello_trampoline,
        .arg      = c,
        .name     = "hmu-hello",
    };
    if (esp_timer_create(&args, &c->hello_timer) != ESP_OK) {
        ESP_LOGE(TAG, "timer_create failed");
        return;
    }
    c->hello_repeating = true;
    // 200 ms damit FHEM seinen Init-Setup (verbose-Logger, State-Init)
    // komplettieren kann (CULFW32-Erfahrungswert 2026-04-29).
    esp_timer_start_once(c->hello_timer, 200 * 1000);
}

static void cancel_hello(hmu_client_t *c)
{
    c->hello_repeating = false;
    if (c->hello_timer) {
        esp_timer_stop(c->hello_timer);
        esp_timer_delete(c->hello_timer);
        c->hello_timer = NULL;
    }
}

// ─── OS-Layer Antworten ────────────────────────────────────────────────

static void respond_os(hmu_client_t *c, uint8_t cnt, uint8_t cmd,
                       const uint8_t *payload, size_t plen)
{
    uint8_t buf[24];
    size_t n = 0;

    switch (cmd) {
    case OS_GET_APP:
        // [0x00] + ASCII "Co_CPU_App" — kein ACK-Wrapper.
        buf[n++] = 0x00;
        memcpy(buf + n, "Co_CPU_App", 10);
        n += 10;
        break;
    case OS_GET_FIRMWARE:
        // [0x04, 0x02, 0x01, 0x00, 0x03, FW_M, FW_m, FW_p] — BL 1.0.3 + FW M.m.p
        buf[n++] = ACK;
        buf[n++] = ACK_INFO;
        buf[n++] = 0x01; buf[n++] = 0x00; buf[n++] = 0x03;   // BL 1.0.3
        buf[n++] = S.fw_major;
        buf[n++] = S.fw_minor;
        buf[n++] = S.fw_patch;
        break;
    case OS_GET_SERIAL:
        // [0x04, 0x02, 10-byte ASCII serial]
        buf[n++] = ACK;
        buf[n++] = ACK_INFO;
        for (int i = 0; i < 10; i++) {
            buf[n++] = (uint8_t)(S.serial[i] ? S.serial[i] : ' ');
        }
        break;
    case OS_GET_CREDITS:
        // [0x04, 0x02, load%]   load=0 (idle)
        buf[n++] = ACK;
        buf[n++] = ACK_INFO;
        buf[n++] = 0x00;
        break;
    case OS_SET_TIME:
    case OS_NORMAL_MODE:
    case OS_CHANGE_APP:
    case OS_ENABLE_CSMACA:
    case OS_ENABLE_CREDITS:
        buf[n++] = ACK;
        buf[n++] = ACK_OK;
        break;
    case OS_UPDATE_FIRMWARE:
    case OS_UPDATE_MODE:
        // FW-Update können wir nicht — explizit NACK damit FHEM nicht den
        // Update-Strom anfängt.
        buf[n++] = ACK;
        buf[n++] = ACK_EUNKNOWN;
        break;
    default:
        buf[n++] = ACK;
        buf[n++] = ACK_OK;
        break;
    }
    send_frame(c, DST_OS, cnt, buf, n);
    (void)payload; (void)plen;
}

// ─── APP-Layer Antworten ───────────────────────────────────────────────

static void respond_app(hmu_client_t *c, uint8_t cnt, uint8_t cmd,
                        const uint8_t *payload, size_t plen)
{
    uint8_t buf[16];
    size_t n = 0;

    switch (cmd) {
    case APP_GET_HMID:
    case APP_DEFAULT_HMID:
        // ACK_WITH_MULTIPART_DATA: [0x04, 0x07, part_no, total_parts, hmid[0..2]]
        // pivccu detect_radio_module liest data_len==6, data[3..5]=HMID.
        buf[n++] = ACK;
        buf[n++] = ACK_MULTIPART;
        buf[n++] = 0x01;   // part_no
        buf[n++] = 0x01;   // total_parts
        buf[n++] = S.hmid[0];
        buf[n++] = S.hmid[1];
        buf[n++] = S.hmid[2];
        break;
    case APP_SET_HMID:
        // payload[1..3] = neue HMID
        if (plen >= 4) {
            S.hmid[0] = payload[1];
            S.hmid[1] = payload[2];
            S.hmid[2] = payload[3];
            ESP_LOGI(TAG, "HMID set to %02X%02X%02X",
                     S.hmid[0], S.hmid[1], S.hmid[2]);
        }
        buf[n++] = ACK;
        buf[n++] = ACK_OK;
        break;
    case APP_SET_CURRENT_KEY:
    case APP_SET_PREVIOUS_KEY:
    case APP_SET_TEMP_KEY: {
        // Phase D: persistiere in NVS (storage-only).  Payload =
        // [cmd][idx][16-byte key] = 18 Bytes.  Ohne idx (alte FW) wären
        // es 17 — wir akzeptieren beides.
        if (plen >= 17) {
            const char *nk = (cmd == APP_SET_CURRENT_KEY)  ? NVS_KEY_CUR
                           : (cmd == APP_SET_PREVIOUS_KEY) ? NVS_KEY_PRV
                                                            : NVS_KEY_TMP;
            persist_aes_key(nk, payload + 1, plen - 1);
        }
        buf[n++] = ACK;
        buf[n++] = ACK_OK;
        break;
    }
    case APP_SEND:
        // Phase B: APP_SEND von FHEM (Legacy-Format) → LLMAC_SEND zur
        // USB-Source (DualCoPro-Format) übersetzen.  Hier nur dispatchen,
        // den eigentlichen Re-Pack macht handle_app_send_legacy().
        // Wir antworten an FHEM noch NICHT — der Legacy-APP_ACK kommt
        // erst aus on_source_frame() wenn der Stick mit LLMAC_ACK
        // bestätigt hat.  Wenn Stick nicht ready oder kein Slot frei:
        // sofort ACK_EUNKNOWN.
        return; /* Antwort kommt async — siehe handle_app_send_legacy() */
    case APP_TRX_GET_MCU_TYPE:
        // Wir sind Legacy CoPro, nicht TRX-Class — explizit NACK damit
        // rfd-detector schneller in den richtigen Detection-Pfad geht.
        buf[n++] = ACK;
        buf[n++] = ACK_EUNKNOWN;
        break;
    default:
        buf[n++] = ACK;
        buf[n++] = ACK_OK;
        break;
    }
    send_frame(c, DST_APP, cnt, buf, n);
    (void)payload;
}

// ─── Phase D: Bypass-Mode-Detection ────────────────────────────────────
//
// Wenn die aktive Source `Co_CPU_App` reportet (= echtes HM-MOD-RPI-PCB,
// z.B. später per UART-Pinheader an einem Hardware-Spin), spricht sie
// nativ das Legacy-HMUARTLGW-Protokoll, das FHEM erwartet.  In diesem
// Fall MUSS der Sink reine Passthrough-Funktion machen — kein
// Hello-Push, keine OS-Stubs, keine APP_SEND-Translation, kein
// LLMAC_RECV-Repack.  Bytes vom Source 1:1 zu allen Clients, Bytes
// von Client 1:1 zur Source.
//
// Aktuelles Hardware-Setup (HmIP-RFUSB nur via USB-Host) liefert
// `DualCoPro_App` — Bypass ist somit unerreichbar in dieser
// Konfiguration.  Code ist defensiv für die Pin-Header-Variante.
// Aktive Source (USB oder UART) abfragen — Bridge entscheidet, welche.
// Liefert connected/boot_done/tag der aktiven Source; bei detached oder
// unbekannter short_id alles auf "nicht ready".
typedef struct {
    bool connected;
    bool boot_done;
    char tag[32];
} active_source_state_t;

static void get_active_source_state(active_source_state_t *out)
{
    out->connected = false;
    out->boot_done = false;
    out->tag[0]    = '\0';

    source_t *src = bridge_get_source();
    if (!src || !src->short_id) return;

    if (strcmp(src->short_id, "usb") == 0) {
        source_usb_stats_t us;
        source_usb_get_stats(&us);
        out->connected = us.stick_connected;
        out->boot_done = us.boot_done;
        snprintf(out->tag, sizeof(out->tag), "%s", us.app_tag);
    } else if (strcmp(src->short_id, "uart") == 0) {
        source_uart_stats_t us;
        source_uart_get_stats(&us);
        out->connected = us.module_present;
        out->boot_done = us.boot_done;
        snprintf(out->tag, sizeof(out->tag), "%s", us.app_tag);
    }
}

static bool is_bypass_mode(void)
{
    active_source_state_t st;
    get_active_source_state(&st);
    return st.connected && st.boot_done &&
           strcmp(st.tag, "Co_CPU_App") == 0;
}

// ─── Phase D: AES-Key NVS-Persistenz (storage-only) ───────────────────
//
// FHEM lädt 17-Byte-Keys per APP_SET_CURRENT/PREVIOUS/TEMP_KEY hoch.
// Phase A hat sie blind ge-ACKed und vergessen.  Phase D persistiert
// sie zumindest in NVS-Namespace `rfnh_aes`, sodass sie über Reboot
// hinweg rekonstruierbar wären — die echte AES-Auth-Verarbeitung
// (Phase E) kann dann darauf aufbauen ohne erneuten FHEM-Upload.
//
// **Wichtig:** wir machen aktuell keine AES-Auth.  AES-getaggte
// BidCoS-Frames erreichen die FHEM-Seite mit `status=0x01` (= als
// wären sie unverschlüsselt).  Per-Peer-AES-Toleranz von FHEM trägt
// das in der Übergangszeit (siehe Memory hmuartlgw_protocol_spec.md).
static void persist_aes_key(const char *key, const uint8_t *blob, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_AES, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, blob, len);
    nvs_commit(h);
    nvs_close(h);
    S.stats.aes_keys_persisted++;
    ESP_LOGI(TAG, "AES key %s persisted (%u bytes)", key, (unsigned)len);
}

// ─── Pending-ACK (Phase B): cnt → client mapping ──────────────────────
//
// Aufrufer dieser Helper MÜSSEN S.mtx halten — pending[] wird sowohl
// vom client_task (pending_alloc nach Translation) als auch vom
// source-RX-Task (pending_find_and_take in route_llmac_ack_to_fhem)
// mutiert.

static void pending_gc_locked(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < PENDING_ACK_SLOTS; i++) {
        if (S.pending[i].active &&
            (now - S.pending[i].sent_us) > PENDING_ACK_TIMEOUT_US) {
            // Timeout — FHEM hat unsere Frist überschritten, der Client
            // wartet ohnehin nicht mehr; nur freigeben.
            S.pending[i].active = false;
        }
    }
}

static int pending_alloc_locked(uint8_t cnt, hmu_client_t *c)
{
    pending_gc_locked();
    for (int i = 0; i < PENDING_ACK_SLOTS; i++) {
        if (!S.pending[i].active) {
            S.pending[i].active  = true;
            S.pending[i].cnt     = cnt;
            S.pending[i].client  = c;
            S.pending[i].sent_us = esp_timer_get_time();
            return i;
        }
    }
    return -1;
}

static pending_ack_t *pending_find_and_take_locked(uint8_t cnt)
{
    for (int i = 0; i < PENDING_ACK_SLOTS; i++) {
        if (S.pending[i].active && S.pending[i].cnt == cnt) {
            S.pending[i].active = false;
            return &S.pending[i];
        }
    }
    return NULL;
}

// ─── Phase B: APP_SEND (Legacy) → LLMAC_SEND (DualCoPro) ──────────────

// Legacy APP_SEND payload (FW > 0x010006):
//   [0]=0x02 [1..2]=00 00 [3]=mode-byte [4]=msg_nr
//   [5]=flags [6]=mtype [7..9]=src(3) [10..12]=dst(3) [13..]=body
// → mindestens 13 Bytes.
//
// DualCoPro LLMAC_SEND payload:
//   [0]=0x06 [1..3]=opts(80 00 80) [4]=msg_nr (= o4)
//   [5]=flags [6]=mtype [7..9]=src(3) [10..12]=dst(3) [13..]=body
// → mindestens 14 Bytes (inkl. cmd byte, also 13 ohne cmd byte).
static bool handle_app_send_legacy(hmu_client_t *c, uint8_t cnt,
                                    const uint8_t *legacy_payload, size_t lp_len)
{
    if (lp_len < 13) {
        ESP_LOGW(TAG, "APP_SEND too short (%u)", (unsigned)lp_len);
        return false;
    }

    // Source ready prüfen — sonst würde das Frame im void verschwinden.
    // Aktive Source kann USB oder UART sein (siehe is_bypass_mode/
    // get_active_source_state).
    active_source_state_t st;
    get_active_source_state(&st);
    if (!st.connected || !st.boot_done) {
        ESP_LOGW(TAG, "APP_SEND but source not ready (conn=%d boot=%d)",
                 st.connected, st.boot_done);
        return false;
    }

    // Wenn die Source bereits Co_CPU_App spricht (z.B. HM-MOD-RPI-PCB am
    // UART), spricht sie nativ Legacy → keine Translation, der Pfad
    // gehört in is_bypass_mode/s_on_source_rx-Bypass.  Hier landet ein
    // APP_SEND nur wenn die Source DualCoPro_App (oder kompatibel) ist.
    if (strcmp(st.tag, "Co_CPU_App") == 0) {
        ESP_LOGW(TAG, "Co_CPU_App source — APP_SEND should be handled by bypass path");
        return false;
    }

    // DualCoPro LLMAC_SEND-Payload bauen (cmd + opts + msg_nr + body).
    uint8_t dc_payload[256];
    size_t  dc_len = 0;
    dc_payload[dc_len++] = LLMAC_SEND;        // 0x06
    dc_payload[dc_len++] = 0x80;              // opts[0]
    dc_payload[dc_len++] = 0x00;              // opts[1]
    dc_payload[dc_len++] = 0x80;              // opts[2]
    dc_payload[dc_len++] = legacy_payload[4]; // opts[3] = msg_nr (CNT-Echo)

    // Body ab Legacy-Position 5 (flags+mtype+src+dst+body) durchreichen.
    size_t body_len = lp_len - 5;
    if (body_len + dc_len > sizeof(dc_payload)) {
        ESP_LOGW(TAG, "frame too large (%u body bytes)", (unsigned)body_len);
        return false;
    }
    memcpy(dc_payload + dc_len, legacy_payload + 5, body_len);
    dc_len += body_len;

    // Wire-Frame mit dst=LLMAC=0x03, cnt = original-FHEM-cnt (Stick
    // spiegelt das in seinem ACK).  Stack-Allokation (siehe send_frame).
    uint8_t wire[HMU_MAX_FRAME_ESC];
    int n = hmu_frame_encode(DST_LLMAC, cnt, dc_payload, dc_len,
                              wire, sizeof(wire));
    if (n < 0) {
        ESP_LOGW(TAG, "hmu_frame_encode failed (dc_len=%u)", (unsigned)dc_len);
        return false;
    }

    // Pending-Slot reservieren BEVOR der Frame in der Bridge verschwindet,
    // sonst race wenn der Stick blitz-schnell antwortet.  Allokation
    // unter S.mtx, danach release — bridge_tx_to_source darf NICHT mit
    // S.mtx gehalten laufen (Lock-Reihenfolge S.mtx → bridge-mtx ist
    // sonst andersrum bei route_llmac_ack_to_fhem aus dem RX-Pfad).
    xSemaphoreTake(S.mtx, portMAX_DELAY);
    int slot = pending_alloc_locked(cnt, c);
    xSemaphoreGive(S.mtx);
    if (slot < 0) {
        ESP_LOGW(TAG, "pending-ACK pool full — dropping APP_SEND");
        return false;
    }

    esp_err_t err = bridge_tx_to_source(&S.self, wire, (size_t)n);
    if (err != ESP_OK) {
        xSemaphoreTake(S.mtx, portMAX_DELAY);
        S.pending[slot].active = false;
        xSemaphoreGive(S.mtx);
        if (err == BRIDGE_ERR_TX_LOCKED) {
            ESP_LOGW(TAG, "APP_SEND rejected: TX-lock owned by another sink");
        } else {
            ESP_LOGW(TAG, "bridge_tx_to_source failed: %s", esp_err_to_name(err));
        }
        return false;
    }

    S.stats.app_send_translated++;
    ESP_LOGI(TAG, "APP_SEND→LLMAC_SEND cnt=0x%02x msg_nr=0x%02x body_len=%u",
             cnt, legacy_payload[4], (unsigned)body_len);
    return true;
}

// Wird im on_source_frame() gerufen wenn LLMAC_ACK reinkommt.
// Übersetzt zurück zu Legacy APP_ACK + sendet an pending-Client.
static void route_llmac_ack_to_fhem(uint8_t cnt,
                                     const uint8_t *llmac_payload, size_t lp_len)
{
    pending_ack_t *p;
    xSemaphoreTake(S.mtx, portMAX_DELAY);
    p = pending_find_and_take_locked(cnt);
    if (!p) {
        // Fan-out: jeder LLMAC_ACK landet bei allen Sinks.  Wenn der ACK
        // einem TX gehört, das ein anderer Sink (z.B. HB-RF-ETH) gemacht
        // hat, ist „kein pending" der Normalfall — kein Fehler.  Trennen
        // anhand des aktuellen TX-Master-Owners: bin ich's nicht, ist's
        // foreign; bin ich's, war's ein echter Late-ACK nach Pending-GC.
        bridge_tx_info_t ti;
        bridge_get_tx_info(&ti);
        bool we_are_master = (strcmp(ti.owner_id, "hmu") == 0);
        if (we_are_master) {
            S.stats.llmac_acks_orphaned++;
        } else {
            S.stats.llmac_acks_foreign++;
        }
        xSemaphoreGive(S.mtx);
        return;
    }
    hmu_client_t *c = p->client;
    if (!c || !c->active) {
        xSemaphoreGive(S.mtx);
        return;
    }

    // LLMAC_ACK status byte: payload[1] (cmd ist payload[0]=0x01).
    // 0x01 = OK, sonst NACK-ähnlich.  Map auf Legacy ACK_OK / ACK_EUNKNOWN.
    uint8_t status = (lp_len >= 2 && llmac_payload[1] == 0x01)
                     ? ACK_OK : ACK_EUNKNOWN;
    uint8_t resp[2] = { ACK, status };
    send_frame(c, DST_APP, cnt, resp, 2);
    S.stats.llmac_acks_routed++;
    xSemaphoreGive(S.mtx);
}

// ─── Phase C: LLMAC_RECV (DualCoPro) → APP_RECV (Legacy) ──────────────
//
// DualCoPro LLMAC_RECV (vom Stick) — verifiziert byte-genau 2026-04-30
// gegen DualCoPro 2.8.6 (Memory hmuartlgw_dualcopro_real_reference):
//   payload[0]   = 0x05 (LLMAC_RECV cmd)
//   payload[1..2]= 16-bit ms-Timestamp (rolling)
//   payload[3]   = info-Byte (0x00 unsolicited)
//   payload[4]   = RSSI raw
//   payload[5..] = AskSin ab CNT (kein LEN voran)
//
// Legacy APP_RECV (Richtung FHEM):
//   payload[0]   = 0x05 (APP_RECV cmd)
//   payload[1]   = status (0x01 normal, 0x02 AES-OK, 0x03 AES-KO)
//   payload[2]   = info
//   payload[3]   = RSSI
//   payload[4..] = AskSin ab CNT
//
// Translation: ts wegwerfen, status=0x01 voranstellen, Rest 1:1.
//
// AES: Phase D wird das echt verarbeiten.  Bis dahin reporten wir
// status=0x01 (= unverschlüsselt-empfangen). Wenn der Stick die AES-
// Auth selbst macht und nur das Klartext-Frame liefert, ist 0x01
// inhaltlich auch korrekt.
static void route_llmac_recv_to_fhem(const uint8_t *payload, size_t plen)
{
    if (plen < 5) {
        ESP_LOGW(TAG, "LLMAC_RECV too short (%u)", (unsigned)plen);
        return;
    }
    uint8_t info     = payload[3];
    uint8_t rssi     = payload[4];
    const uint8_t *ask  = payload + 5;
    size_t  ask_len  = plen - 5;

    // Legacy-Payload bauen.
    uint8_t legacy[256];
    if (4 + ask_len > sizeof(legacy)) {
        ESP_LOGW(TAG, "LLMAC_RECV too long (%u askSin bytes)", (unsigned)ask_len);
        return;
    }
    legacy[0] = APP_RECV;
    legacy[1] = 0x01;          // status normal
    legacy[2] = info;
    legacy[3] = rssi;
    memcpy(legacy + 4, ask, ask_len);
    size_t legacy_len = 4 + ask_len;

    xSemaphoreTake(S.mtx, portMAX_DELAY);
    uint8_t cnt = S.rx_cnt++;
    int delivered = 0;
    for (int i = 0; i < SINK_HMUARTLGW_LEGACY_MAX_CLIENTS; i++) {
        hmu_client_t *c = &S.clients[i];
        if (!c->active || c->host == HOST_AWAITING_PROBE) continue;
        send_frame(c, DST_APP, cnt, legacy, legacy_len);
        delivered++;
    }
    if (delivered > 0) {
        S.stats.llmac_recv_broadcast++;
        ESP_LOGI(TAG, "LLMAC_RECV→APP_RECV cnt=0x%02x rssi=-0x%02x ask_len=%u → %d FHEM-client(s)",
                 cnt, (unsigned)(0x100 - rssi), (unsigned)ask_len, delivered);
    }
    xSemaphoreGive(S.mtx);
}

// ─── Source-Side Frame-Decoder-Callback ───────────────────────────────

static void on_source_frame(void *ctx, uint8_t dst, uint8_t cnt,
                             const uint8_t *payload, size_t plen)
{
    (void)ctx;
    if (plen < 1) return;

    uint8_t cmd = payload[0];

    if (dst == DST_LLMAC && cmd == LLMAC_ACK) {
        // Phase B — TX-Bestätigung vom Stick.
        route_llmac_ack_to_fhem(cnt, payload, plen);
        return;
    }
    if (dst == DST_LLMAC && cmd == LLMAC_RECV) {
        // Phase C — unsolicited BidCoS-Frame vom Wire.
        route_llmac_recv_to_fhem(payload, plen);
        return;
    }
    // HMIP-Frames (dst=0x02) werden im Legacy-Mode ignoriert — FHEM
    // HMUARTLGW.pm hat keine HmIP-Layer, BidCoS-only.
    // COMMON-Frames (dst=0xfe) sind boot-time-Identify, nicht relevant
    // nach Init.
}

// ─── Frame-Callback (vom Client-seitigen Decoder) ──────────────────────

static void on_frame(void *ctx, uint8_t dst, uint8_t cnt,
                     const uint8_t *payload, size_t plen)
{
    hmu_client_t *c = (hmu_client_t *)ctx;
    S.stats.frames_rx_from_fhem++;

    if (plen == 0) {
        ESP_LOGW(TAG, "empty payload — drop");
        return;
    }
    uint8_t cmd = payload[0];

    // Host-Mode-Detection beim ersten Frame (siehe CULFW32 dispatch).
    if (c->host == HOST_AWAITING_PROBE) {
        if (dst == DST_OS && cmd == OS_GET_APP) {
            c->host = HOST_FHEM;
            cancel_hello(c);
            ESP_LOGI(TAG, "host detected: FHEM (OS_GET_APP first)");
        } else if (dst == DST_OS && cmd == OS_CHANGE_APP) {
            c->host = HOST_HOMEGEAR;
            cancel_hello(c);
            ESP_LOGI(TAG, "host detected: Homegear/rfd (OS_CHANGE_APP first)");
        }
    }

    if (dst == DST_OS) {
        respond_os(c, cnt, cmd, payload, plen);
    } else if (dst == DST_APP) {
        // APP_SEND ist async: Re-Pack zu LLMAC_SEND, ACK kommt erst
        // wenn LLMAC_ACK vom Stick zurückkommt (s. on_source_frame).
        if (cmd == APP_SEND) {
            if (!handle_app_send_legacy(c, cnt, payload, plen)) {
                // Translation oder Source-Submit fehlgeschlagen → sofort
                // ACK_EUNKNOWN, sonst hängt FHEM im 95-ms-Wait.
                uint8_t resp[2] = { ACK, ACK_EUNKNOWN };
                send_frame(c, DST_APP, cnt, resp, 2);
                S.stats.app_send_rejected++;
            }
        } else {
            respond_app(c, cnt, cmd, payload, plen);
        }
    } else {
        ESP_LOGW(TAG, "frame on unsupported dst=0x%02X cmd=0x%02X plen=%u — ACK_EUNKNOWN",
                 dst, cmd, (unsigned)plen);
        uint8_t nack[2] = { ACK, ACK_EUNKNOWN };
        send_frame(c, dst, cnt, nack, 2);
    }
}

// ─── Connection-Slots / Tasks ──────────────────────────────────────────

static hmu_client_t *find_free_slot(void)
{
    for (int i = 0; i < SINK_HMUARTLGW_LEGACY_MAX_CLIENTS; i++) {
        if (!S.clients[i].active) return &S.clients[i];
    }
    return NULL;
}

static void close_client(hmu_client_t *c)
{
    if (!c->active) return;
    cancel_hello(c);
    if (c->sock >= 0) {
        shutdown(c->sock, SHUT_RDWR);
        close(c->sock);
        c->sock = -1;
    }
    c->active = false;
    S.stats.total_disconnects++;
    S.stats.active_clients--;
}

static void client_task(void *arg)
{
    hmu_client_t *c = (hmu_client_t *)arg;
    ESP_LOGI(TAG, "client task starting fd=%d from %d.%d.%d.%d:%u",
             c->sock,
             (int)((c->ip      ) & 0xff), (int)((c->ip >>  8) & 0xff),
             (int)((c->ip >> 16) & 0xff), (int)((c->ip >> 24) & 0xff),
             (unsigned)c->port);

    hmu_decoder_init(&c->dec, on_frame, c);
    schedule_hello(c);

    uint8_t buf[512];
    while (c->active) {
        int n = recv(c->sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            ESP_LOGI(TAG, "client closed (n=%d errno=%d)", n, errno);
            break;
        }
        if (is_bypass_mode()) {
            // Pure-Passthrough: Client-Bytes 1:1 zur Source.
            esp_err_t terr = bridge_tx_to_source(&S.self, buf, (size_t)n);
            if (terr == ESP_OK) {
                S.stats.bypass_tx_bytes += (uint32_t)n;
            }
            // BRIDGE_ERR_TX_LOCKED → silent drop, gezählt in bridge stats
        } else {
            hmu_decoder_feed(&c->dec, buf, (size_t)n);
        }
    }

    xSemaphoreTake(S.mtx, portMAX_DELAY);
    close_client(c);
    xSemaphoreGive(S.mtx);
    c->task = NULL;
    vTaskDelete(NULL);
}

static void accept_task(void *arg)
{
    (void)arg;

    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int fd = accept(S.listen_sock, (struct sockaddr *)&src, &slen);
        if (fd < 0) {
            ESP_LOGW(TAG, "accept failed errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // SO_SNDTIMEO 100 ms — schützt hello_trampoline (esp_timer-Task)
        // und alle send_frame-Aufrufer aus Source-RX-Tasks vor einem
        // langsamen FHEM-Client: blocking send() blockt max 100 ms, kein
        // unbegrenzter Stall mehr.  100 ms ist großzügig für LAN; bei
        // produktivem Stau wäre eher ein Client-Problem als ein Netz-
        // Problem.
        struct timeval snd_to = { .tv_sec = 0, .tv_usec = 100000 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));

        xSemaphoreTake(S.mtx, portMAX_DELAY);
        hmu_client_t *c = find_free_slot();
        if (!c) {
            xSemaphoreGive(S.mtx);
            ESP_LOGW(TAG, "max clients (%d) reached — reject",
                     SINK_HMUARTLGW_LEGACY_MAX_CLIENTS);
            close(fd);
            continue;
        }
        memset(c, 0, sizeof(*c));
        c->sock   = fd;
        c->ip     = src.sin_addr.s_addr;
        c->port   = ntohs(src.sin_port);
        c->active = true;
        c->host   = HOST_AWAITING_PROBE;
        S.stats.total_accepts++;
        S.stats.active_clients++;
        xSemaphoreGive(S.mtx);

        BaseType_t r = xTaskCreate(client_task, "hmu-cli", 6144,
                                    c, 5, &c->task);
        if (r != pdPASS) {
            ESP_LOGE(TAG, "client_task spawn failed");
            close_client(c);
        }
    }
}

// ─── sink_t-Interface (Phase A: kein bridge-rx-passthrough) ────────────

// Phase B+C: feed source-side decoder; pickt LLMAC_ACK + LLMAC_RECV
//            zur Übersetzung.  Phase D: bei Bypass-Mode (Co_CPU_App-
//            Source) reine Byte-Fanout an alle TCP-Clients.
static void s_on_source_rx(sink_t *self, const uint8_t *data, size_t len)
{
    (void)self;

    if (is_bypass_mode()) {
        // Co_CPU_App-Source spricht bereits Legacy — direktes
        // Byte-Fanout an alle aktiven FHEM-Clients.  Snapshot der Sockets
        // unter S.mtx, dann MSG_DONTWAIT-send ohne Mutex — sonst kann ein
        // langsamer Client die ganze Source-RX-Pipeline einfrieren
        // (Source-Driver-Buffer läuft voll, Frames vom HM-Modul gehen
        // verloren).
        int socks[SINK_HMUARTLGW_LEGACY_MAX_CLIENTS];
        int nsocks = 0;
        xSemaphoreTake(S.mtx, portMAX_DELAY);
        for (int i = 0; i < SINK_HMUARTLGW_LEGACY_MAX_CLIENTS; i++) {
            hmu_client_t *c = &S.clients[i];
            if (c->active && c->sock >= 0) {
                socks[nsocks++] = c->sock;
            }
        }
        xSemaphoreGive(S.mtx);

        for (int i = 0; i < nsocks; i++) {
            ssize_t w = send(socks[i], data, len, MSG_DONTWAIT);
            if (w < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    S.stats.bypass_tx_dropped++;
                } else {
                    ESP_LOGW(TAG, "bypass send sock=%d failed errno=%d — closing",
                             socks[i], errno);
                    shutdown(socks[i], SHUT_RDWR);
                }
            } else {
                S.stats.bypass_rx_bytes += (uint32_t)w;
            }
        }
        return;
    }

    // Emulation-Mode (Phase A-C): durch Decoder ziehen, gefilterte
    // LLMAC_ACK/LLMAC_RECV-Frames werden in on_source_frame umgepackt.
    hmu_decoder_feed(&S.src_dec, data, len);
}

static esp_err_t s_start(sink_t *self) { (void)self; return ESP_OK; }
static esp_err_t s_stop(sink_t *self)  { (void)self; return ESP_OK; }
static const char *s_describe(sink_t *self) { (void)self; return "hmuartlgw-legacy"; }

static const struct sink_ops s_ops = {
    .on_source_rx = s_on_source_rx,
    .start        = s_start,
    .stop         = s_stop,
    .describe     = s_describe,
};

// ─── init ──────────────────────────────────────────────────────────────

static void init_identity_from_mac(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    // HMID: letzte 3 Bytes der MAC (eindeutig pro Gerät, nicht 0xCAFE42-Stub).
    S.hmid[0] = mac[3];
    S.hmid[1] = mac[4];
    S.hmid[2] = mac[5];
    // Serial: 10-stellige ASCII, ableitbar aus MAC (RFNETHM<MAC4-5>).
    snprintf(S.serial, sizeof(S.serial), "RFNH%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    // FW-Version 1.4.1 — kein "outdated" in FHEM, sieht legitim aus.
    S.fw_major = 0x01;
    S.fw_minor = 0x04;
    S.fw_patch = 0x01;
    ESP_LOGI(TAG, "identity: HMID=%02X%02X%02X serial='%s' fw=%u.%u.%u",
             S.hmid[0], S.hmid[1], S.hmid[2], S.serial,
             S.fw_major, S.fw_minor, S.fw_patch);
}

sink_t *sink_hmuartlgw_legacy_init(uint16_t port)
{
    if (S.mtx) return &S.self;   // idempotent

    S.port = port ? port : SINK_HMUARTLGW_LEGACY_DEFAULT_PORT;
    S.mtx  = xSemaphoreCreateMutex();
    init_identity_from_mac();
    hmu_decoder_init(&S.src_dec, on_source_frame, NULL);

    S.listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (S.listen_sock < 0) {
        ESP_LOGE(TAG, "socket() failed errno=%d", errno);
        return NULL;
    }
    int yes = 1;
    setsockopt(S.listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in baddr = { 0 };
    baddr.sin_family      = AF_INET;
    baddr.sin_addr.s_addr = htonl(INADDR_ANY);
    baddr.sin_port        = htons(S.port);
    if (bind(S.listen_sock, (struct sockaddr *)&baddr, sizeof(baddr)) < 0) {
        ESP_LOGE(TAG, "bind(:%u) failed errno=%d", (unsigned)S.port, errno);
        close(S.listen_sock);
        S.listen_sock = -1;
        return NULL;
    }
    if (listen(S.listen_sock, 4) < 0) {
        ESP_LOGE(TAG, "listen failed errno=%d", errno);
        close(S.listen_sock);
        S.listen_sock = -1;
        return NULL;
    }
    ESP_LOGI(TAG, "Legacy HMUARTLGW emulation listening on TCP:%u "
                  "(FHEM/Homegear-compatible)", (unsigned)S.port);

    S.self.ops  = &s_ops;
    S.self.user = NULL;

    xTaskCreate(accept_task, "hmu-acc", 4096, NULL, 5, NULL);
    return &S.self;
}

void sink_hmuartlgw_legacy_get_stats(sink_hmuartlgw_legacy_stats_t *out)
{
    if (!out) return;
    xSemaphoreTake(S.mtx, portMAX_DELAY);
    *out = S.stats;
    out->port = S.port;
    xSemaphoreGive(S.mtx);
}
