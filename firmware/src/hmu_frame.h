// SPDX-License-Identifier: GPL-2.0-or-later
/* HMUARTLGW Frame Codec — DualCoPro-kompatibel.
 *
 * Frame-Layout (auf-dem-Wire, vor Escape):
 *   [0xfd] [len_hi] [len_lo] [dst] [cnt] [payload...] [crc_hi] [crc_lo]
 *
 *   length      = (dst + cnt + payload) bytes, ohne CRC
 *   CRC16       = poly 0x8005, init 0xd77f, big-endian,
 *                 computed über [0xfd, len_hi, len_lo, dst, cnt, payload]
 *   Escape      = 0xfc und 0xfd im Body durch [0xfc, b XOR 0x80] ersetzt
 *                 (Magic-0xfd am Frame-Anfang ist NICHT escaped)
 *
 * dst-Werte (DualCoPro):
 *   0x00 OS / HMSYSTEM (Legacy-Pfad)
 *   0x01 APP / TRX
 *   0x02 HMIP   (DualCoPro)
 *   0x03 LLMAC  (DualCoPro BidCoS-MAC)
 *   0xfe COMMON (DualCoPro identify)
 *   0xff DUAL_ERR
 *
 * Portiert verbatim (mit gleichem Layout) aus
 * /Public/CLAUDE/CUL32-HM/bmcond/src/frame.{c,h} (GPL-2.0-or-later).
 */

#ifndef RFNETHM_HMU_FRAME_H
#define RFNETHM_HMU_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define HMU_MAGIC          0xfd
#define HMU_ESCAPE         0xfc
#define HMU_ESCAPE_MASK    0x80

#define HMU_DST_OS         0x00
#define HMU_DST_APP        0x01
#define HMU_DST_HMIP       0x02
#define HMU_DST_LLMAC      0x03
#define HMU_DST_COMMON     0xfe
#define HMU_DST_DUAL_ERR   0xff

#define HMU_CRC_POLY       0x8005
#define HMU_CRC_INIT       0xd77f

/* Boot-Probe-Payloads für source_usb / source_uart.  IDENTIFY auf
 * dst=COMMON erfragt den App-Tag; CHANGE_APP auf dst=OS triggert den
 * BL→App-Switch (Legacy-Pfad für HM-MOD-RPI-PCB / Co_CPU_BL). */
extern const uint8_t HMU_PL_IDENTIFY[1];    /* { 0x01 } */
extern const uint8_t HMU_PL_CHANGE_APP[1];  /* { 0x03 } */

/* For RFNETHM v0.3 we keep the buffer modest — DualCoPro practical max
 * is ~300 bytes per the CUL32-HM live observations.  bmcond uses 1024. */
#define HMU_MAX_PAYLOAD    512
#define HMU_MAX_FRAME      (1 + 2 + 2 + HMU_MAX_PAYLOAD + 2)
#define HMU_MAX_FRAME_ESC  (1 + 2 * (2 + 2 + HMU_MAX_PAYLOAD + 2))

uint16_t hmu_crc16(const uint8_t *data, size_t len);

int hmu_frame_encode(uint8_t dst, uint8_t cnt,
                     const uint8_t *payload, size_t payload_len,
                     uint8_t *out, size_t out_cap);

typedef void (*hmu_frame_cb)(void *ctx,
                             uint8_t dst, uint8_t cnt,
                             const uint8_t *payload, size_t payload_len);

typedef enum {
    HMU_DEC_HUNT_MAGIC,
    HMU_DEC_LEN_HI,
    HMU_DEC_LEN_LO,
    HMU_DEC_BODY,
    HMU_DEC_CRC_HI,
    HMU_DEC_CRC_LO,
} hmu_dec_state_t;

typedef struct {
    hmu_dec_state_t state;
    bool            esc_pending;
    uint16_t        length;
    uint16_t        body_idx;
    uint8_t         body[2 + HMU_MAX_PAYLOAD];
    uint8_t         crc_hi;
    hmu_frame_cb    cb;
    void           *ctx;
    uint32_t        frames_ok;
    uint32_t        frames_crc_err;
    uint32_t        frames_truncated;
    uint32_t        bytes_skipped;
} hmu_decoder_t;

void hmu_decoder_init(hmu_decoder_t *d, hmu_frame_cb cb, void *ctx);
void hmu_decoder_feed(hmu_decoder_t *d, const uint8_t *data, size_t len);

#endif
