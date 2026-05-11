// SPDX-License-Identifier: GPL-2.0-or-later
//
// HB-RF-ETH-UDP-Listener — C-Port aus CULFW32
// (firmware/components/transport_hbrfeth_udp/src/hbrfeth_listener.cpp).
// Inhaltlich identisch, nur ohne C++-shared_ptr/std::array.

#include "sink_hbrfeth.h"
#include "bridge.h"
#include "hmu_frame.h"
#include "net.h"
#include "source_uart.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "sink-hb";

#define T_CONNECT     0
#define T_DISCONNECT  1
#define T_KEEPALIVE   2
#define T_LED         3
#define T_RESET       4
#define T_STARTCONN   5
#define T_STOPCONN    6
#define T_FRAME       7

#define KA_TIMEOUT_US      5000000ULL
#define KA_OUT_PERIOD_US   1000000ULL

typedef struct {
    bool      connected;
    bool      started;
    uint32_t  ip;          // network-byte-order
    uint16_t  port;        // host-byte-order
    uint8_t   endpoint_id; // V2 reconnect tracking
    uint8_t   tx_counter;  // our outgoing counter
    int64_t   last_rx_us;
} hb_client_t;

static struct {
    sink_t                self;
    uint16_t              port;
    int                   sock;
    SemaphoreHandle_t     mtx;
    hb_client_t           clients[SINK_HBRFETH_MAX_CLIENTS];
    sink_hbrfeth_stats_t  stats;
    esp_timer_handle_t    ka_timer;
    bool                  started_flag;
} S;

// ───── Helpers ──────────────────────────────────────────────────────────

static void client_disconnect(hb_client_t *c);

static hb_client_t *find(uint32_t ip, uint16_t port)
{
    for (int i = 0; i < SINK_HBRFETH_MAX_CLIENTS; i++) {
        if (S.clients[i].connected && S.clients[i].ip == ip && S.clients[i].port == port) {
            return &S.clients[i];
        }
    }
    return NULL;
}

static hb_client_t *find_or_create(uint32_t ip, uint16_t port)
{
    hb_client_t *c = find(ip, port);
    if (c) return c;

    // Same-IP-Eviction: A.R.s hb_rf_eth.ko macht beim try_connect jedesmal
    // einen frischen sock_create_kern, also kommt jeder Retry mit neuem
    // ephemeren Source-Port.  Ohne diese Eviction füllen 4 Retry-Bursts
    // sofort alle SLOTS, neue CONNECTs werden abgelehnt, der Kernel
    // hängt im "Timeout occured while connecting"-Loop.  Beobachtet
    // 2026-05-07 nach OTA-Reboot.
    for (int i = 0; i < SINK_HBRFETH_MAX_CLIENTS; i++) {
        if (S.clients[i].connected && S.clients[i].ip == ip) {
            client_disconnect(&S.clients[i]);
        }
    }

    for (int i = 0; i < SINK_HBRFETH_MAX_CLIENTS; i++) {
        if (!S.clients[i].connected) {
            memset(&S.clients[i], 0, sizeof(S.clients[i]));
            S.clients[i].ip            = ip;
            S.clients[i].port          = port;
            S.clients[i].endpoint_id   = 1;
            S.clients[i].last_rx_us    = esp_timer_get_time();
            return &S.clients[i];
        }
    }
    return NULL;   // all slots taken
}

static void client_disconnect(hb_client_t *c)
{
    if (!c->connected) return;
    memset(c, 0, sizeof(*c));
    S.stats.active_clients--;
    S.stats.total_disconnects++;
}

// Build [type, cnt, payload, crc-be] and sendto().  Caller holds mtx.
static void send_typed(uint32_t ip, uint16_t port, uint8_t type,
                       const uint8_t *payload, size_t plen,
                       const uint8_t *cnt_override)
{
    uint8_t buf[1500];
    if (plen + 4 > sizeof(buf)) return;
    buf[0] = type;
    if (cnt_override) {
        buf[1] = *cnt_override;
    } else {
        hb_client_t *c = find(ip, port);
        buf[1] = c ? c->tx_counter++ : 0;
    }
    if (plen) memcpy(buf + 2, payload, plen);
    uint16_t crc = hmu_crc16(buf, 2 + plen);
    buf[2 + plen]     = (uint8_t)((crc >> 8) & 0xff);
    buf[2 + plen + 1] = (uint8_t)( crc       & 0xff);

    struct sockaddr_in dst = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = ip,
        .sin_port        = htons(port),
    };
    sendto(S.sock, buf, plen + 4, 0, (struct sockaddr *)&dst, sizeof(dst));
}

// ───── Packet handling ──────────────────────────────────────────────────

static void handle_packet(const uint8_t *data, size_t len, uint32_t ip, uint16_t port)
{
    if (len < 4) return;
    uint16_t crc_calc = hmu_crc16(data, len - 2);
    uint16_t crc_recv = ((uint16_t)data[len - 2] << 8) | data[len - 1];
    if (crc_calc != crc_recv) {
        S.stats.bad_crc++;
        ESP_LOGW(TAG, "bad CRC from %d.%d.%d.%d:%u (calc %04x recv %04x)",
                 (int)((ip      ) & 0xff), (int)((ip >>  8) & 0xff),
                 (int)((ip >> 16) & 0xff), (int)((ip >> 24) & 0xff),
                 port, crc_calc, crc_recv);
        return;
    }

    uint8_t       type    = data[0];
    uint8_t       cnt     = data[1];   // mutable for counter_override
    const uint8_t *payload = data + 2;
    size_t        plen    = len - 4;

    xSemaphoreTake(S.mtx, portMAX_DELAY);

    hb_client_t *c = (type == T_CONNECT) ? find_or_create(ip, port)
                                         : find(ip, port);
    if (!c) {
        // Non-CONNECT from unknown client → ignore (matches HB-RF-ETH source).
        if (type == T_CONNECT) {
            // find_or_create returned NULL → all slots full
            ESP_LOGW(TAG, "all client slots taken — reject CONNECT");
        }
        xSemaphoreGive(S.mtx);
        return;
    }
    c->last_rx_us = esp_timer_get_time();

    switch (type) {
    case T_CONNECT:
        if (!c->connected) {
            c->connected = true;
            S.stats.active_clients++;
            S.stats.total_connects++;
        }
        c->started = false;
        if (plen == 1 && payload[0] == 1) {
            // V1: reply [status=1, mirror_cnt]
            c->endpoint_id += 2;
            uint8_t resp[2] = { 1, cnt };
            send_typed(ip, port, T_CONNECT, resp, 2, NULL);
            ESP_LOGI(TAG, "V1 CONNECT from %d.%d.%d.%d:%u (ep_id=%u)",
                     (int)((ip      )&0xff), (int)((ip >>  8)&0xff),
                     (int)((ip >> 16)&0xff), (int)((ip >> 24)&0xff),
                     port, c->endpoint_id);
        } else if (plen == 2 && payload[0] == 2) {
            // V2: reply [status=2, mirror_cnt, server_ep_id]
            //
            // Semantik des V2-CONNECT (aus hb_rf_eth.ko-Verhalten):
            //   client_ep == 0  → erste Verbindung, Server wählt ep_id.
            //   client_ep != 0  → Kernel meldet sich mit dem in der
            //                     vorherigen Session zugewiesenen ep_id
            //                     zurück (z.B. nach unserem Reboot).
            //                     Wir adoptieren seinen Wert; Session
            //                     fühlt sich für den Kernel kontinuierlich
            //                     an.  Frühere Logik hat den Mismatch
            //                     verworfen → Endlos-Reconnect-Loop weil
            //                     find_or_create() den Slot frisch mit
            //                     ep_id=1 startet, der Kernel aber seinen
            //                     gespeicherten Wert (z.B. 3) immer wieder
            //                     schickt.  Verifiziert 2026-05-07 mit
            //                     v0.12 nach OTA-Reboot: ~1.6 Hz cycle.
            uint8_t client_ep = payload[1];
            if (client_ep == 0) {
                c->endpoint_id += 2;
                c->started = false;
            } else {
                c->endpoint_id = client_ep;   // adopt + resume
            }
            uint8_t resp[3] = { 2, cnt, c->endpoint_id };
            send_typed(ip, port, T_CONNECT, resp, 3, NULL);
            ESP_LOGI(TAG, "V2 CONNECT from %d.%d.%d.%d:%u (ep_id=%u%s)",
                     (int)((ip      )&0xff), (int)((ip >>  8)&0xff),
                     (int)((ip >> 16)&0xff), (int)((ip >> 24)&0xff),
                     port, c->endpoint_id,
                     client_ep == 0 ? "" : " resume");
        }
        break;
    case T_DISCONNECT:
        ESP_LOGI(TAG, "DISCONNECT from client");
        client_disconnect(c);
        break;
    case T_KEEPALIVE:
        // last_rx_us already updated above
        break;
    case T_LED:
        // LED-Cmd: heute noop — kein adressierbares LED-Element
        // im Bridge-Mode.  CULFW32 macht's auch nicht.
        break;
    case T_RESET:
        // cmd=4 = piVCCU-Standard-Primitive (siehe
        // refs/piVCCU/hb_rf_eth.c::hb_rf_eth_send_reset).  Reiner
        // Hardware-Pulse, KEINE Application-Layer-Semantik.
        //
        // Wichtig: cmd=4 wird in piVCCU bei JEDEM connect() automatisch
        // geschickt — also auch bei normalen Container-Starts ohne
        // Flash-Intent.  Wir dürfen daher hier NICHT flash_lock setzen
        // (würde jeden normalen Container-Start 5 Min im flash-Modus
        // sticken; user-Argument 2026-05-08).
        //
        // Application-Layer-Flash-Lock geht über den expliziten
        // POST /api/source/uart/reset {"hold_in_bl":true}-Endpoint;
        // bmcond's transport_rfnethm (Iter 6) acquired den dort
        // explizit, dann erst cmd=4 als idempotent-RST.
        //
        // Auf USB-Source ist cmd=4 ein Noop (HmIP-RFUSB-Stick hat keinen
        // am ESP32 schaltbaren RST-Pin, siehe pcb_basics_must_have).
        {
            char tag[32] = {0};
            esp_err_t r = source_uart_pulse_rst_only(tag, sizeof(tag));
            if (r == ESP_OK) {
                ESP_LOGW(TAG, "T_RESET (cmd=4): RST-Pulse done, Banner-Tag '%s'", tag);
            } else {
                ESP_LOGW(TAG, "T_RESET (cmd=4): pulse_rst_only → %s "
                              "(kein UART-Modul gesteckt? USB-only-Setup?)",
                         esp_err_to_name(r));
            }
        }
        break;
    case T_STARTCONN:
        c->started = true;
        ESP_LOGI(TAG, "StartConn — frames will now flow");
        break;
    case T_STOPCONN:
        c->started = false;
        ESP_LOGI(TAG, "StopConn — frames suspended");
        break;
    case T_FRAME: {
        // Forward raw 0xfd-bytes to the source (USB stick).
        // Release mutex first — bridge_tx_to_source may block in
        // cdc_acm_host_data_tx_blocking().
        S.stats.rx_frames_from_clients++;
        xSemaphoreGive(S.mtx);
        bridge_tx_to_source(&S.self, payload, plen);
        return;  // mtx already released
    }
    default:
        S.stats.unknown_type++;
        ESP_LOGW(TAG, "unknown type=%u", type);
        break;
    }

    xSemaphoreGive(S.mtx);
}

// ───── Tasks ────────────────────────────────────────────────────────────

static void rx_task(void *arg)
{
    (void)arg;

    while (!net_is_connected()) vTaskDelay(pdMS_TO_TICKS(500));

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        ESP_LOGE(TAG, "socket failed errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(S.port),
    };
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(:%u) errno=%d", S.port, errno);
        close(s);
        vTaskDelete(NULL);
        return;
    }
    S.sock = s;
    S.started_flag = true;
    ESP_LOGI(TAG, "HB-RF-ETH UDP-listener bound on :%u", S.port);

    uint8_t buf[1500];
    struct sockaddr_in src;
    socklen_t srclen;
    while (1) {
        srclen = sizeof(src);
        int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&src, &srclen);
        if (n < 0) {
            if (errno == EINTR) continue;
            ESP_LOGW(TAG, "recvfrom errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        handle_packet(buf, (size_t)n, src.sin_addr.s_addr, ntohs(src.sin_port));
    }
}

static void keepalive_tick(void *arg)
{
    (void)arg;
    if (!S.started_flag) return;
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(S.mtx, portMAX_DELAY);
    for (int i = 0; i < SINK_HBRFETH_MAX_CLIENTS; i++) {
        hb_client_t *c = &S.clients[i];
        if (!c->connected) continue;
        if ((uint64_t)(now - c->last_rx_us) > KA_TIMEOUT_US) {
            ESP_LOGW(TAG, "keepalive timeout for %d.%d.%d.%d:%u",
                     (int)((c->ip      )&0xff), (int)((c->ip >>  8)&0xff),
                     (int)((c->ip >> 16)&0xff), (int)((c->ip >> 24)&0xff),
                     c->port);
            S.stats.keepalive_timeouts++;
            client_disconnect(c);
            continue;
        }
        send_typed(c->ip, c->port, T_KEEPALIVE, NULL, 0, NULL);
    }
    xSemaphoreGive(S.mtx);
}

// ───── sink_t-Hooks ─────────────────────────────────────────────────────

// USB-Source hat RX-Bytes geliefert → für jeden „started" Client als
// Type-7-Frame raussenden.  Bytes sind die rohen 0xfd-HMUARTLGW-Frames
// die das hb_rf_eth-Kernel-Modul direkt erwartet.
static void on_rx(sink_t *self, const uint8_t *data, size_t len)
{
    (void)self;
    if (!data || !len) return;

    xSemaphoreTake(S.mtx, portMAX_DELAY);
    for (int i = 0; i < SINK_HBRFETH_MAX_CLIENTS; i++) {
        hb_client_t *c = &S.clients[i];
        if (!c->connected) continue;
        if (!c->started) {
            S.stats.tx_dropped_not_started++;
            continue;
        }
        send_typed(c->ip, c->port, T_FRAME, data, len, NULL);
        S.stats.tx_frames_to_clients++;
    }
    xSemaphoreGive(S.mtx);
}

static const char *describe(sink_t *s)
{
    (void)s;
    static char buf[48];
    snprintf(buf, sizeof(buf), "HB-RF-ETH UDP :%u (%d/%d clients)",
             S.port, S.stats.active_clients, SINK_HBRFETH_MAX_CLIENTS);
    return buf;
}

static esp_err_t op_start(sink_t *s)
{
    (void)s;
    // Stack 6 KB — rx_task hat einen 1500-Byte-Buffer auf dem Stack plus
    // den lwIP-Pfad durch recvfrom() (eigene Stack-Tiefe).  4 KB reicht
    // nicht (Stack-Overflow bei eingehendem CONNECT, gemessen 2026-05-07).
    xTaskCreate(rx_task, "hb_rx", 6144, NULL, 4, NULL);

    esp_timer_create_args_t a = {
        .callback        = keepalive_tick,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "hb-ka",
    };
    if (esp_timer_create(&a, &S.ka_timer) == ESP_OK) {
        esp_timer_start_periodic(S.ka_timer, KA_OUT_PERIOD_US);
    }
    return ESP_OK;
}

static const struct sink_ops s_ops = {
    .on_source_rx = on_rx,
    .start        = op_start,
    .stop         = NULL,
    .describe     = describe,
};

sink_t *sink_hbrfeth_init(uint16_t port)
{
    memset(&S, 0, sizeof(S));
    S.port = port ? port : SINK_HBRFETH_DEFAULT_PORT;
    S.sock = -1;
    S.mtx  = xSemaphoreCreateMutex();
    S.self.ops = &s_ops;
    return &S.self;
}

void sink_hbrfeth_get_stats(sink_hbrfeth_stats_t *out)
{
    if (!out) return;
    xSemaphoreTake(S.mtx, portMAX_DELAY);
    *out = S.stats;
    out->port = S.port;
    xSemaphoreGive(S.mtx);
}
