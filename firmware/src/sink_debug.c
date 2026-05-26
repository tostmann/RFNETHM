// SPDX-License-Identifier: GPL-2.0-or-later

#include "sink_debug.h"
#include "esp_log.h"

static const char *TAG = "sink-dbg";

static void on_rx(sink_t *s, const uint8_t *data, size_t len)
{
    (void)s;
    ESP_LOGD(TAG, "fanout %u bytes:", (unsigned)len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, data, len, ESP_LOG_DEBUG);
}

static const char *describe(sink_t *s) { (void)s; return "debug-hexdump"; }

static const struct sink_ops s_ops = {
    .on_source_rx = on_rx,
    .start        = NULL,
    .stop         = NULL,
    .describe     = describe,
};

static sink_t s_sink = { .ops = &s_ops };

sink_t *sink_debug_get(void) { return &s_sink; }
