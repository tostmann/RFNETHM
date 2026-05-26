// SPDX-License-Identifier: GPL-2.0-or-later

#include "log_buffer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

#define LB_MAX_LINES  128
#define LB_LINE_CAP   200

typedef struct {
    uint32_t ts_ms;
    char     msg[LB_LINE_CAP];
} lb_line_t;

static lb_line_t          s_lines[LB_MAX_LINES];
static uint32_t           s_head_seq;          // next seq to write
static uint32_t           s_dropped_pending;   // unsynced count of contention-drops
static SemaphoreHandle_t  s_mtx;
static vprintf_like_t     s_orig_vprintf;

// ESP_LOG fügt ANSI-Color-Sequenzen ein (z.B. `\x1b[0;32m`).  Beim
// Speichern strippen wir die für eine plain-text-Browser-Konsole.
static void strip_ansi_inplace(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == 0x1b && r[1] == '[') {
            r += 2;
            while (*r && *r != 'm') r++;
            if (*r) r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static int log_hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);

    // 1) Original-vprintf: schreibt am UART aus.  Wenn s_orig_vprintf
    //    NULL ist (sollte nicht), fallback auf vprintf.
    int rc = s_orig_vprintf ? s_orig_vprintf(fmt, ap) : vprintf(fmt, ap);

    // 2) Format in lokalen Buffer rendern für den Ring.
    char tmp[LB_LINE_CAP];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap2);
    va_end(ap2);
    if (n <= 0) return rc;
    if (n >= (int)sizeof(tmp)) n = sizeof(tmp) - 1;
    tmp[n] = '\0';

    strip_ansi_inplace(tmp);

    // Trailing \r/\n strippen (eine Zeile pro Eintrag).
    size_t l = strlen(tmp);
    while (l > 0 && (tmp[l-1] == '\n' || tmp[l-1] == '\r')) tmp[--l] = '\0';
    if (l == 0) return rc;

    // ESP_LOG schickt einen Eintrag pro Aufruf — keine Aufsplittung
    // nötig.  printf() könnte mehrere \n enthalten; wir speichern dann
    // den ersten Teil und ignorieren den Rest (selten relevant; main()
    // banner ist mehrzeilig, akzeptable Einschränkung).
    char *nl = strchr(tmp, '\n');
    if (nl) *nl = '\0';

    if (s_mtx && xSemaphoreTake(s_mtx, 0) == pdTRUE) {
        // Falls vorher Drops aufgelaufen sind: einen Marker-Eintrag
        // einschieben, damit der Log-Reader weiß, dass Zeilen fehlen.
        if (s_dropped_pending) {
            uint32_t dropped = s_dropped_pending;
            s_dropped_pending = 0;
            size_t idx = s_head_seq % LB_MAX_LINES;
            s_lines[idx].ts_ms = esp_log_timestamp();
            snprintf(s_lines[idx].msg, sizeof(s_lines[idx].msg),
                     "<%u log lines dropped (mutex contention)>",
                     (unsigned)dropped);
            s_head_seq++;
        }
        size_t idx = s_head_seq % LB_MAX_LINES;
        s_lines[idx].ts_ms = esp_log_timestamp();
        strlcpy(s_lines[idx].msg, tmp, sizeof(s_lines[idx].msg));
        s_head_seq++;
        xSemaphoreGive(s_mtx);
    } else if (s_mtx) {
        // Reader hält den Mutex; statt blocken (innerhalb eines ESP_LOG-
        // Callbacks wäre das gefährlich) nur den Counter inkrementieren.
        // Nicht atomar — bei seltener Contention akzeptabel.
        s_dropped_pending++;
    }
    return rc;
}

esp_err_t log_buffer_init(void)
{
    if (s_mtx) return ESP_OK;
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;
    s_orig_vprintf = esp_log_set_vprintf(log_hook);
    return ESP_OK;
}

size_t log_buffer_get_since(uint32_t since,
                             char *out, size_t out_cap,
                             uint32_t *head_seq, uint32_t *oldest_seq)
{
    if (head_seq)   *head_seq   = 0;
    if (oldest_seq) *oldest_seq = 0;
    if (!s_mtx || out_cap == 0) return 0;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint32_t h = s_head_seq;
    uint32_t o = (h > LB_MAX_LINES) ? (h - LB_MAX_LINES) : 0;
    if (head_seq)   *head_seq   = h;
    if (oldest_seq) *oldest_seq = o;

    if (since < o) since = o;

    size_t pos   = 0;
    size_t count = 0;
    bool   first = true;
    for (uint32_t s = since; s < h; s++) {
        size_t idx = s % LB_MAX_LINES;
        // grobe Größenabschätzung: 30 (Header+seq+ts) + msg-Länge + Escapes
        size_t need = 32 + strlen(s_lines[idx].msg) * 2 + 4;
        if (pos + need >= out_cap) break;

        if (!first) out[pos++] = ',';
        // {"seq":N,"ts":T,"msg":"..."}
        int w = snprintf(out + pos, out_cap - pos,
                         "{\"seq\":%u,\"ts\":%u,\"msg\":\"",
                         (unsigned)s, (unsigned)s_lines[idx].ts_ms);
        if (w < 0) break;
        pos += w;

        for (const char *m = s_lines[idx].msg; *m && pos + 7 < out_cap; m++) {
            unsigned char c = (unsigned char)*m;
            if (c == '"' || c == '\\') {
                out[pos++] = '\\';
                out[pos++] = c;
            } else if (c < 0x20) {
                int w2 = snprintf(out + pos, out_cap - pos, "\\u%04x", c);
                if (w2 < 0) break;
                pos += w2;
            } else {
                out[pos++] = c;
            }
        }
        if (pos + 2 >= out_cap) break;
        out[pos++] = '"';
        out[pos++] = '}';
        count++;
        first = false;
    }
    if (pos < out_cap) out[pos] = '\0';
    xSemaphoreGive(s_mtx);
    return count;
}
