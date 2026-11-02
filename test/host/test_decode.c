/* Host test: decode real captured frames (fredkelly/vectaire-esphome docs/protocol.md) with the firmware code. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mvhr_decode.h"
#include "vbus_codec.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* Feed a de-escaped body (len..crc) through encode-style escaping, then the decoder. */
static int decode_body(const uint8_t *body, size_t n, vbus_frame_t *f)
{
    uint8_t wire[VBUS_MAX_WIRE];
    size_t w = 0;
    wire[w++] = VBUS_STX;
    for (size_t i = 0; i < n; i++) {
        if (body[i] == 2 || body[i] == 3 || body[i] == 0x7F) {
            wire[w++] = 0x7F;
            wire[w++] = body[i] ^ 0x7F;
        } else {
            wire[w++] = body[i];
        }
    }
    wire[w++] = VBUS_ETX;
    vbus_decoder_t d;
    vbus_decoder_reset(&d);
    int r = VBUS_DEC_NONE;
    for (size_t i = 0; i < w; i++) {
        r = vbus_decoder_feed(&d, wire[i], f);
    }
    return r;
}

int main(void)
{
    vbus_frame_t f;
    mvhr_state_t st = {0};

    /* Real length-25 status frame: 21.3 C, 20.3 C, RH 33.0 % (bytes 7-8), fan 1 at 60 % / 42 %, CRC 11 C5 */
    static const uint8_t status[] = {0x19, 0xD1, 0x00, 0x00, 0x00, 0x00, 0xD5, 0x00, 0xCB, 0x00, 0x01, 0x4A, 0x00,
                                     0x3C, 0x2A, 0x00, 0x00, 0xFF, 0x11, 0x00, 0x02, 0x01, 0x00, 0x11, 0xC5};
    CHECK(decode_body(status, sizeof(status), &f) == VBUS_DEC_FRAME);
    CHECK(f.addr == 0xD1 && f.cmd == 0x00 && f.len == 20);
    CHECK(mvhr_decode_reply(&st, f.cmd, f.data, f.len) == MVHR_G_STATUS);
    CHECK(st.temp_extract_x10 == 213 && st.temp_supply_x10 == 203);
    CHECK(st.humidity_x10 == 330 && st.fan_a_pct == 60 && st.fan_b_pct == 42);
    CHECK(st.fan_speed == 1 && st.alarms == 0 && st.holiday == false);
    CHECK(st.bypass_override == MVHR_BYPASS_AUTO && st.bypass_locked);

    CHECK(!strcmp(mvhr_boost_text(&st), "Off") && !strcmp(mvhr_airflow_text(&st), "Heat recovery"));
    st.status = MVHR_FUNC_SWITCH_DELAY;
    CHECK(!strcmp(mvhr_boost_text(&st), "Delay"));
    st.status = MVHR_FUNC_BOOST | MVHR_IND_HUMIDITY;
    CHECK(!strcmp(mvhr_boost_text(&st), "On"));
    st.status = MVHR_FUNC_OVERRUN;
    CHECK(!strcmp(mvhr_boost_text(&st), "Overrun"));
    st.status = MVHR_FUNC_NIGHT_INHIBIT;
    CHECK(!strcmp(mvhr_boost_text(&st), "Off") && !strcmp(mvhr_function_text(&st), "Night mode"));
    st.bypass_auto_state = 2;
    CHECK(!strcmp(mvhr_airflow_text(&st), "Intake motor off"));
    st.bypass_override = 1;
    CHECK(!strcmp(mvhr_airflow_text(&st), "Bypass"));
    st.status = 0;
    st.bypass_override = MVHR_BYPASS_AUTO;
    st.bypass_auto_state = 0;

    /* Real length-7 version frame 07 D1 16 42 05 5F 6A */
    static const uint8_t ver[] = {0x07, 0xD1, 0x16, 0x42, 0x05, 0x5F, 0x6A};
    CHECK(decode_body(ver, sizeof(ver), &f) == VBUS_DEC_FRAME && f.cmd == 0x16 && f.len == 2);

    /* Real fan ack 05 D1 0E EC 50 and holiday ack 05 D1 15 BE FE */
    static const uint8_t ack1[] = {0x05, 0xD1, 0x0E, 0xEC, 0x50};
    static const uint8_t ack2[] = {0x05, 0xD1, 0x15, 0xBE, 0xFE};
    CHECK(decode_body(ack1, sizeof(ack1), &f) == VBUS_DEC_FRAME && f.cmd == 0x0E && f.len == 0);
    CHECK(decode_body(ack2, sizeof(ack2), &f) == VBUS_DEC_FRAME && f.cmd == 0x15 && f.len == 0);

    /* Real panel write frames: our encoder must produce them byte for byte (de-escaped body shown in docs) */
    static const struct { uint8_t cmd, val; uint8_t crc_lo, crc_hi; } writes[] = {
        {0x0E, 0x00, 0x5B, 0xA2}, {0x0E, 0x01, 0xD2, 0xB3}, {0x0E, 0x02, 0x49, 0x81},
        {0x15, 0x00, 0x62, 0xD3}, {0x15, 0x01, 0xEB, 0xC2},
    };
    for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); i++) {
        uint8_t out[VBUS_MAX_WIRE];
        size_t n = vbus_encode(0xD0, writes[i].cmd, &writes[i].val, 1, out, sizeof(out));
        /* last two body bytes before ETX are the CRC (none of these need escaping) */
        CHECK(n >= 8 && out[n - 3] == writes[i].crc_lo && out[n - 2] == writes[i].crc_hi);
    }

    /* Real length-14 time frame: 0E D1 02 HH MM DD MM YY 01 00 78 94 — check field mapping */
    static const uint8_t clk[] = {0x16, 0x0C, 0x0B, 0x06, 0x1A, 0x01, 0x00, 0x78, 0x94};   /* 22:12 11/06/26 */
    CHECK(mvhr_decode_reply(&st, 0x02, clk, sizeof(clk)) == MVHR_G_CLOCK);
    CHECK(st.clock.hour == 22 && st.clock.minute == 12 && st.clock.day == 11 && st.clock.month == 6);
    CHECK(st.clock.year == 26 && st.clock.format_24h == 1 && st.clock.region == 0x78 && st.clock.utc_offset == 0x94);

    printf("%s (%d failures)\n", fails ? "FAILED" : "all decode tests passed", fails);
    return fails != 0;
}
