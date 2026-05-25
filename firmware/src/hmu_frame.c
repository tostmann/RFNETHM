// SPDX-License-Identifier: GPL-2.0-or-later
/* HMUARTLGW Frame Codec — Implementation.
 *
 * Portiert verbatim aus /Public/CLAUDE/CUL32-HM/bmcond/src/frame.c
 * (GPL-2.0-or-later) mit unverändertem Algorithmus.  Die einzige
 * Anpassung ist HMU_MAX_PAYLOAD = 512 (statt 1024 bei bmcond) — siehe
 * hmu_frame.h.
 */

#include "hmu_frame.h"
#include <string.h>

const uint8_t HMU_PL_IDENTIFY[1]   = { 0x01 };
const uint8_t HMU_PL_CHANGE_APP[1] = { 0x03 };

uint16_t hmu_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = HMU_CRC_INIT;
    for (size_t i = 0; i < len; ++i) {
        crc ^= ((uint16_t)data[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ HMU_CRC_POLY);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

static size_t emit_escaped(uint8_t *out, uint8_t b)
{
    if (b == HMU_MAGIC || b == HMU_ESCAPE) {
        out[0] = HMU_ESCAPE;
        out[1] = b ^ HMU_ESCAPE_MASK;
        return 2;
    }
    out[0] = b;
    return 1;
}

int hmu_frame_encode(uint8_t dst, uint8_t cnt,
                     const uint8_t *payload, size_t payload_len,
                     uint8_t *out, size_t out_cap)
{
    if (payload_len > HMU_MAX_PAYLOAD) return -1;

    size_t length = 2 + payload_len;
    if (length > 0xffff) return -1;

    uint8_t raw[3 + 2 + HMU_MAX_PAYLOAD];
    raw[0] = HMU_MAGIC;
    raw[1] = (uint8_t)(length >> 8);
    raw[2] = (uint8_t)(length & 0xff);
    raw[3] = dst;
    raw[4] = cnt;
    if (payload_len > 0) memcpy(&raw[5], payload, payload_len);

    size_t raw_len = 3 + length;
    uint16_t crc = hmu_crc16(raw, raw_len);

    if (out_cap < 1 + 2 * (2 + length + 2)) return -1;

    size_t pos = 0;
    out[pos++] = HMU_MAGIC;
    pos += emit_escaped(&out[pos], raw[1]);
    pos += emit_escaped(&out[pos], raw[2]);
    for (size_t i = 0; i < length; ++i) {
        pos += emit_escaped(&out[pos], raw[3 + i]);
    }
    pos += emit_escaped(&out[pos], (uint8_t)(crc >> 8));
    pos += emit_escaped(&out[pos], (uint8_t)(crc & 0xff));

    return (int)pos;
}

/* ───────────────────────── Streaming Decoder ───────────────────────── */

void hmu_decoder_init(hmu_decoder_t *d, hmu_frame_cb cb, void *ctx)
{
    memset(d, 0, sizeof(*d));
    d->state = HMU_DEC_HUNT_MAGIC;
    d->cb = cb;
    d->ctx = ctx;
}

static void dec_resync(hmu_decoder_t *d)
{
    d->state = HMU_DEC_HUNT_MAGIC;
    d->esc_pending = false;
    d->length = 0;
    d->body_idx = 0;
}

static uint8_t apply_escape(hmu_decoder_t *d, uint8_t b, bool *consumed)
{
    *consumed = false;
    if (d->esc_pending) {
        d->esc_pending = false;
        return b ^ HMU_ESCAPE_MASK;
    }
    if (b == HMU_ESCAPE) {
        d->esc_pending = true;
        *consumed = true;
        return 0;
    }
    return b;
}

void hmu_decoder_feed(hmu_decoder_t *d, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        uint8_t raw = data[i];

        if (raw == HMU_MAGIC) {
            if (d->state != HMU_DEC_HUNT_MAGIC) {
                d->frames_truncated++;
                d->bytes_skipped += d->body_idx;
            }
            dec_resync(d);
            d->state = HMU_DEC_LEN_HI;
            continue;
        }

        if (d->state == HMU_DEC_HUNT_MAGIC) {
            d->bytes_skipped++;
            continue;
        }

        bool consumed_as_escape;
        uint8_t b = apply_escape(d, raw, &consumed_as_escape);
        if (consumed_as_escape) continue;

        switch (d->state) {
        case HMU_DEC_LEN_HI:
            d->length = ((uint16_t)b) << 8;
            d->state = HMU_DEC_LEN_LO;
            break;
        case HMU_DEC_LEN_LO:
            d->length |= b;
            d->body_idx = 0;
            if (d->length < 2 || d->length > sizeof(d->body)) {
                d->frames_truncated++;
                dec_resync(d);
            } else {
                d->state = HMU_DEC_BODY;
            }
            break;
        case HMU_DEC_BODY:
            d->body[d->body_idx++] = b;
            if (d->body_idx >= d->length) {
                d->state = HMU_DEC_CRC_HI;
            }
            break;
        case HMU_DEC_CRC_HI:
            d->crc_hi = b;
            d->state = HMU_DEC_CRC_LO;
            break;
        case HMU_DEC_CRC_LO: {
            uint16_t got = ((uint16_t)d->crc_hi << 8) | b;
            uint8_t buf[3 + sizeof(d->body)];
            buf[0] = HMU_MAGIC;
            buf[1] = (uint8_t)(d->length >> 8);
            buf[2] = (uint8_t)(d->length & 0xff);
            memcpy(&buf[3], d->body, d->length);
            uint16_t want = hmu_crc16(buf, 3 + d->length);
            if (got == want) {
                d->frames_ok++;
                if (d->cb && d->length >= 2) {
                    d->cb(d->ctx, d->body[0], d->body[1],
                          &d->body[2], d->length - 2);
                }
            } else {
                d->frames_crc_err++;
            }
            dec_resync(d);
            break;
        }
        default:
            dec_resync(d);
        }
    }
}
