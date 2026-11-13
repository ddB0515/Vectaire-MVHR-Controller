#include "vbus_codec.h"

#include <string.h>

uint16_t vbus_crc16_x25(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
        }
    }
    return (uint16_t)~crc;
}

static bool put_escaped(uint8_t v, uint8_t *out, size_t out_size, size_t *pos)
{
    if (v == VBUS_STX || v == VBUS_ETX || v == VBUS_ESC) {
        if (*pos + 2 > out_size) {
            return false;
        }
        out[(*pos)++] = VBUS_ESC;
        out[(*pos)++] = v ^ VBUS_ESC;
    } else {
        if (*pos + 1 > out_size) {
            return false;
        }
        out[(*pos)++] = v;
    }
    return true;
}

size_t vbus_encode(uint8_t addr, uint8_t cmd, const uint8_t *data, uint8_t len, uint8_t *out, size_t out_size)
{
    if (len > VBUS_MAX_DATA) {
        return 0;
    }
    uint8_t body[VBUS_MAX_BODY];
    body[0] = (uint8_t)(len + 5);
    body[1] = addr;
    body[2] = cmd;
    if (len) {
        memcpy(&body[3], data, len);
    }
    uint16_t crc = vbus_crc16_x25(body, (size_t)len + 3);
    body[len + 3] = crc & 0xFF;
    body[len + 4] = crc >> 8;

    size_t pos = 0;
    if (out_size < 2) {
        return 0;
    }
    out[pos++] = VBUS_STX;
    for (size_t i = 0; i < (size_t)len + 5; i++) {
        if (!put_escaped(body[i], out, out_size, &pos)) {
            return 0;
        }
    }
    if (pos + 1 > out_size) {
        return 0;
    }
    out[pos++] = VBUS_ETX;
    return pos;
}

void vbus_decoder_reset(vbus_decoder_t *d)
{
    d->n = 0;
    d->in_frame = false;
    d->esc = false;
}

vbus_dec_result_t vbus_decoder_feed(vbus_decoder_t *d, uint8_t byte, vbus_frame_t *frame)
{
    if (byte == VBUS_STX) {             /* STX always restarts a frame */
        d->n = 0;
        d->in_frame = true;
        d->esc = false;
        return VBUS_DEC_NONE;
    }
    if (!d->in_frame) {
        return VBUS_DEC_NONE;
    }
    if (byte == VBUS_ESC) {
        d->esc = true;
        return VBUS_DEC_NONE;
    }
    if (byte == VBUS_ETX) {
        d->in_frame = false;
        /* residue of CRC-16/X-25 over body including its CRC: ~0xF0B8 after the final inversion */
        if (d->n < 5 || d->body[0] != d->n) {
            return VBUS_DEC_BAD_LEN;
        }
        if (vbus_crc16_x25(d->body, d->n) != (uint16_t)~0xF0B8) {
            return VBUS_DEC_BAD_CRC;
        }
        frame->addr = d->body[1];
        frame->cmd = d->body[2];
        frame->len = (uint8_t)(d->n - 5);
        memcpy(frame->data, &d->body[3], frame->len);
        return VBUS_DEC_FRAME;
    }
    if (d->esc) {
        byte ^= VBUS_ESC;
        d->esc = false;
    }
    if (d->n >= VBUS_MAX_BODY) {
        d->in_frame = false;
        return VBUS_DEC_OVERFLOW;
    }
    d->body[d->n++] = byte;
    return VBUS_DEC_NONE;
}
