// SPDX-License-Identifier: GPL-2.0-or-later
//
// bridge.c — Source/Sink-Vermittlung mit kleinem Spinlock-Schutz.
//
// v0.14: TX-Lock-Statemachine ergänzt (siehe bridge.h Kommentar
// "Single-Writer-Soft-Lock").

#include "bridge.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "bridge";

static struct {
    source_t          *source;
    sink_t            *sinks[BRIDGE_MAX_SINKS];
    char               sink_ids[BRIDGE_MAX_SINKS][BRIDGE_SINK_ID_MAXLEN];
    size_t             sink_count;
    SemaphoreHandle_t  mtx;          // Schutz für sinks[]-Liste + tx-Lock
    bridge_stats_t     stats;

    // TX-Lock-Statemachine
    bridge_tx_mode_t   tx_mode;       // AUTO oder PINNED
    int                tx_owner_idx;  // -1 wenn niemand (AUTO idle)
    int64_t            tx_last_us;    // letzter erfolgreicher TX
    uint32_t           tx_rej_lock[BRIDGE_MAX_SINKS];
    uint32_t           tx_rej_lock_total;
} s_br;

static int find_sink_idx_locked(sink_t *who)
{
    for (size_t i = 0; i < s_br.sink_count; i++) {
        if (s_br.sinks[i] == who) return (int)i;
    }
    return -1;
}

static int find_sink_id_locked(const char *id)
{
    if (!id) return -1;
    for (size_t i = 0; i < s_br.sink_count; i++) {
        if (s_br.sink_ids[i][0] && strcmp(s_br.sink_ids[i], id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

void bridge_init(void)
{
    memset(&s_br, 0, sizeof(s_br));
    s_br.mtx          = xSemaphoreCreateMutex();
    s_br.tx_mode      = BRIDGE_TX_AUTO;
    s_br.tx_owner_idx = -1;
}

void bridge_attach_source(source_t *src)
{
    if (!src) return;
    // attach/detach unter s_br.mtx — sonst sehen parallele
    // bridge_tx_to_source / bridge_get_source partielle Updates
    // (z.B. s_br.source schon gesetzt, rx_sink noch alt).
    xSemaphoreTake(s_br.mtx, portMAX_DELAY);
    if (s_br.source && s_br.source != src) {
        ESP_LOGW(TAG, "source swap: %s →", source_describe(s_br.source));
        s_br.source->rx_sink     = NULL;
        s_br.source->rx_sink_ctx = NULL;
    }
    s_br.source = src;
    src->rx_sink     = bridge_on_source_rx;
    src->rx_sink_ctx = NULL;
    xSemaphoreGive(s_br.mtx);
    ESP_LOGI(TAG, "source attached: %s", source_describe(src));
}

void bridge_detach_source(source_t *src)
{
    if (!src) return;
    xSemaphoreTake(s_br.mtx, portMAX_DELAY);
    if (s_br.source == src) {
        ESP_LOGW(TAG, "source detached: %s", source_describe(src));
        src->rx_sink     = NULL;
        src->rx_sink_ctx = NULL;
        s_br.source      = NULL;
    }
    xSemaphoreGive(s_br.mtx);
}

source_t *bridge_get_source(void)
{
    xSemaphoreTake(s_br.mtx, portMAX_DELAY);
    source_t *s = s_br.source;
    xSemaphoreGive(s_br.mtx);
    return s;
}

esp_err_t bridge_attach_sink(sink_t *sink, const char *short_id)
{
    if (!sink) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_br.mtx, portMAX_DELAY);
    if (s_br.sink_count >= BRIDGE_MAX_SINKS) {
        xSemaphoreGive(s_br.mtx);
        return ESP_ERR_NO_MEM;
    }
    size_t idx = s_br.sink_count++;
    s_br.sinks[idx] = sink;
    if (short_id) {
        strncpy(s_br.sink_ids[idx], short_id, BRIDGE_SINK_ID_MAXLEN - 1);
        s_br.sink_ids[idx][BRIDGE_SINK_ID_MAXLEN - 1] = '\0';
    } else {
        s_br.sink_ids[idx][0] = '\0';
    }
    s_br.stats.sink_count = s_br.sink_count;
    xSemaphoreGive(s_br.mtx);
    ESP_LOGI(TAG, "sink attached: %s id='%s' (now %u sinks)",
             sink_describe(sink), s_br.sink_ids[idx], (unsigned)s_br.sink_count);
    return ESP_OK;
}

void bridge_on_source_rx(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (!data || !len) return;

    // Snapshot der Sink-Liste unter Mutex; danach Aufruf ohne Lock,
    // damit Sinks (die ggf. in andere Tasks/Sockets schreiben) nicht
    // andere Bridge-Calls blockieren.
    sink_t *snapshot[BRIDGE_MAX_SINKS];
    size_t  n;
    xSemaphoreTake(s_br.mtx, portMAX_DELAY);
    n = s_br.sink_count;
    memcpy(snapshot, s_br.sinks, n * sizeof(sink_t *));
    s_br.stats.rx_pumps_total++;
    s_br.stats.rx_bytes_total += len;
    s_br.stats.rx_last_us = esp_timer_get_time();
    xSemaphoreGive(s_br.mtx);

    for (size_t i = 0; i < n; i++) {
        sink_on_source_rx(snapshot[i], data, len);
    }
}

esp_err_t bridge_tx_to_source(sink_t *who, const uint8_t *data, size_t len)
{
    // Source-Pointer atomar snapshotten — sonst kann der Supervisor
    // mid-call das s_br.source-Feld nullen (bridge_detach_source) und
    // wir dereferenzieren NULL in source_ready/source_tx.
    xSemaphoreTake(s_br.mtx, portMAX_DELAY);
    source_t *src = s_br.source;
    if (!src || !source_ready(src)) {
        s_br.stats.tx_dropped_not_ready++;
        xSemaphoreGive(s_br.mtx);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_br.mtx);

    // Lock-Check (nur wenn `who` gesetzt — NULL = system, bypass).
    if (who) {
        xSemaphoreTake(s_br.mtx, portMAX_DELAY);
        int my_idx = find_sink_idx_locked(who);
        int64_t now = esp_timer_get_time();

        bool allowed = false;
        if (s_br.tx_mode == BRIDGE_TX_PINNED) {
            allowed = (my_idx >= 0 && my_idx == s_br.tx_owner_idx);
        } else {
            // AUTO: Lock frei wenn nie benutzt, eigener owner, oder Idle-Timeout abgelaufen
            bool idle = (s_br.tx_owner_idx < 0) ||
                        ((now - s_br.tx_last_us) > BRIDGE_TX_AUTO_IDLE_US);
            allowed = (my_idx >= 0 && (idle || my_idx == s_br.tx_owner_idx));
            if (allowed && my_idx != s_br.tx_owner_idx) {
                ESP_LOGI(TAG, "tx-master AUTO claim: '%s' (was '%s')",
                         (my_idx >= 0 && s_br.sink_ids[my_idx][0])
                            ? s_br.sink_ids[my_idx] : "?",
                         (s_br.tx_owner_idx >= 0 && s_br.sink_ids[s_br.tx_owner_idx][0])
                            ? s_br.sink_ids[s_br.tx_owner_idx] : "-");
                s_br.tx_owner_idx = my_idx;
            }
        }

        if (!allowed) {
            if (my_idx >= 0) s_br.tx_rej_lock[my_idx]++;
            s_br.tx_rej_lock_total++;
            s_br.stats.tx_dropped_locked++;
            xSemaphoreGive(s_br.mtx);
            // Throttled log — eine Zeile alle ~64 rejects pro Sink
            if (my_idx >= 0 && (s_br.tx_rej_lock[my_idx] & 63u) == 1u) {
                ESP_LOGW(TAG, "TX rejected (lock): '%s' wants TX, master is '%s' (#%u)",
                         s_br.sink_ids[my_idx][0] ? s_br.sink_ids[my_idx] : "?",
                         (s_br.tx_owner_idx >= 0 && s_br.sink_ids[s_br.tx_owner_idx][0])
                            ? s_br.sink_ids[s_br.tx_owner_idx] : "-",
                         (unsigned)s_br.tx_rej_lock[my_idx]);
            }
            return BRIDGE_ERR_TX_LOCKED;
        }

        // Allowed — Owner ist der aktuelle Sender (im AUTO oben gesetzt,
        // im PINNED == my_idx); Timestamp-Update geschieht erst NACH
        // dem TX, damit ein langsamer source_tx() den Lock nicht
        // versehentlich freigibt.
        xSemaphoreGive(s_br.mtx);
    }

    esp_err_t err = source_tx(src, data, len);
    if (err == ESP_OK) {
        xSemaphoreTake(s_br.mtx, portMAX_DELAY);
        s_br.stats.tx_bytes_total += len;
        s_br.tx_last_us = esp_timer_get_time();
        xSemaphoreGive(s_br.mtx);
    }
    return err;
}

esp_err_t bridge_set_tx_master(const char *id_or_auto)
{
    xSemaphoreTake(s_br.mtx, portMAX_DELAY);
    if (!id_or_auto || !id_or_auto[0] || strcmp(id_or_auto, "auto") == 0) {
        s_br.tx_mode      = BRIDGE_TX_AUTO;
        s_br.tx_owner_idx = -1;
        xSemaphoreGive(s_br.mtx);
        ESP_LOGI(TAG, "tx-master set to AUTO");
        return ESP_OK;
    }
    int idx = find_sink_id_locked(id_or_auto);
    if (idx < 0) {
        xSemaphoreGive(s_br.mtx);
        ESP_LOGW(TAG, "tx-master pin: id '%s' unknown", id_or_auto);
        return ESP_ERR_NOT_FOUND;
    }
    s_br.tx_mode      = BRIDGE_TX_PINNED;
    s_br.tx_owner_idx = idx;
    xSemaphoreGive(s_br.mtx);
    ESP_LOGI(TAG, "tx-master PIN to '%s'", id_or_auto);
    return ESP_OK;
}

void bridge_get_tx_info(bridge_tx_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_br.mtx, portMAX_DELAY);
    out->mode        = s_br.tx_mode;
    out->owner_idx   = s_br.tx_owner_idx;
    out->last_tx_us  = s_br.tx_last_us;
    if (s_br.tx_owner_idx >= 0 && s_br.tx_owner_idx < (int)s_br.sink_count
        && s_br.sink_ids[s_br.tx_owner_idx][0]) {
        strncpy(out->owner_id, s_br.sink_ids[s_br.tx_owner_idx],
                BRIDGE_SINK_ID_MAXLEN - 1);
    }
    for (size_t i = 0; i < s_br.sink_count; i++) {
        out->slot_used[i] = true;
        strncpy(out->ids[i], s_br.sink_ids[i], BRIDGE_SINK_ID_MAXLEN - 1);
        out->tx_rejected_lock[i] = s_br.tx_rej_lock[i];
    }
    out->tx_rejected_lock_total = s_br.tx_rej_lock_total;
    xSemaphoreGive(s_br.mtx);
}

void bridge_get_stats(bridge_stats_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_br.mtx, portMAX_DELAY);
    *out = s_br.stats;
    xSemaphoreGive(s_br.mtx);
}
