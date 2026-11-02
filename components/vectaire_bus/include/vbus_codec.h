#pragma once
/*
 * Vectaire MVHR bus framing (pure C, no ESP-IDF dependencies so it can be unit-tested on the host).
 *
 *   0x02 | len addr cmd data... crc_lo crc_hi | 0x03
 *          '---------- escaped body ---------'
 * len   = number of body bytes (len .. crc_hi)
 * addr  = 0xD0 request (controller -> unit), 0xD1 reply (unit -> controller)
 * crc   = CRC-16/X-25 over len..data, sent low byte first
 * 0x02 / 0x03 / 0x7F inside the body are sent as 0x7F, value ^ 0x7F.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VBUS_STX            0x02
#define VBUS_ETX            0x03
#define VBUS_ESC            0x7F
#define VBUS_ADDR_REQUEST   0xD0
#define VBUS_ADDR_REPLY     0xD1
#define VBUS_MAX_DATA       120
#define VBUS_MAX_BODY       (VBUS_MAX_DATA + 5)
/* worst case: every body byte escaped, plus STX/ETX */
#define VBUS_MAX_WIRE       (2 * VBUS_MAX_BODY + 2)

typedef struct {
    uint8_t addr;
    uint8_t cmd;
    uint8_t len;                    /* number of data bytes */
    uint8_t data[VBUS_MAX_DATA];
} vbus_frame_t;

uint16_t vbus_crc16_x25(const uint8_t *data, size_t len);

/* Encode a frame for the wire. Returns the number of bytes written, 0 if it doesn't fit. */
size_t vbus_encode(uint8_t addr, uint8_t cmd, const uint8_t *data, uint8_t len, uint8_t *out, size_t out_size);

typedef enum {
    VBUS_DEC_NONE = 0,      /* no complete frame yet */
    VBUS_DEC_FRAME,         /* valid frame in *frame */
    VBUS_DEC_BAD_CRC,
    VBUS_DEC_BAD_LEN,
    VBUS_DEC_OVERFLOW,
} vbus_dec_result_t;

typedef struct {
    uint8_t body[VBUS_MAX_BODY];
    uint8_t n;
    bool in_frame;
    bool esc;
} vbus_decoder_t;

void vbus_decoder_reset(vbus_decoder_t *d);

/* Feed one received byte. On VBUS_DEC_FRAME, *frame holds the decoded frame. */
vbus_dec_result_t vbus_decoder_feed(vbus_decoder_t *d, uint8_t byte, vbus_frame_t *frame);

#ifdef __cplusplus
}
#endif
