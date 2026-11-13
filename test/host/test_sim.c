/* Host test: the built-in simulated unit, read back through the real codec and decoders. */
#include <stdio.h>
#include <string.h>

#include "mvhr_decode.h"
#include "mvhr_sim.h"
#include "vbus_codec.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static mvhr_state_t st;

/* Request -> wire -> simulated unit -> reply wire -> decoder -> state. Returns the reply length or -1. */
static int xfer(uint8_t cmd, const uint8_t *data, uint8_t len, uint32_t now_ms)
{
    uint8_t out[VBUS_MAX_DATA], wire[VBUS_MAX_WIRE];
    int n = mvhr_sim_reply(cmd, data, len, out, now_ms);
    if (n < 0) {
        return -1;
    }
    size_t w = vbus_encode(VBUS_ADDR_REPLY, cmd, out, (uint8_t)n, wire, sizeof(wire));
    vbus_decoder_t d;
    vbus_decoder_reset(&d);
    vbus_frame_t f;
    int r = VBUS_DEC_NONE;
    for (size_t i = 0; i < w; i++) {
        r = vbus_decoder_feed(&d, wire[i], &f);
    }
    CHECK(r == VBUS_DEC_FRAME && f.cmd == cmd && f.len == n);
    st.valid |= mvhr_decode_reply(&st, f.cmd, f.data, f.len);
    return n;
}

static void u16(uint8_t cmd, uint16_t v, uint32_t now_ms)
{
    const uint8_t d[2] = {v >> 8, v & 0xFF};
    CHECK(xfer(cmd, d, 2, now_ms) == 0);
}

int main(void)
{
    mvhr_sim_init();

    /* everything the model polls decodes */
    CHECK(xfer(MVHR_CMD_STATUS, NULL, 0, 0) == 17);
    CHECK(xfer(MVHR_CMD_CONFIG_READ, NULL, 0, 0) == 49);
    CHECK(xfer(MVHR_CMD_CLOCK_READ, NULL, 0, 0) == 9);
    CHECK(xfer(MVHR_CMD_COUNTERS, NULL, 0, 0) == 9);
    CHECK(xfer(MVHR_CMD_BOOST_READ, NULL, 0, 0) == 2);
    CHECK(xfer(MVHR_CMD_OVERRUN_READ, NULL, 0, 0) == 2);
    CHECK(xfer(MVHR_CMD_HUMID_READ, NULL, 0, 0) == 2);
    CHECK(st.fw_major == 1 && st.fw_minor == 3 && st.cfg_len == 49);
    CHECK(st.night_known && st.night_enabled && st.night_start_min == 22 * 60 && st.night_end_min == 6 * 60);
    CHECK(st.fan_speed == 2 && st.bypass_override == MVHR_BYPASS_AUTO && !st.holiday && st.alarms == 0);
    CHECK(st.temp_extract_x10 >= 202 && st.temp_extract_x10 <= 218);
    CHECK(st.temp_supply_x10 >= 45 && st.temp_supply_x10 <= 125);
    CHECK(st.humidity_x10 >= 330 && st.humidity_x10 <= 770 && st.humidity_x10 % 10 == 0);
    CHECK((st.fan_a_pct == 70 || st.fan_a_pct == 100) && st.fan_b_pct == st.fan_a_pct - 10);  /* speed 2 or boost */
    CHECK(st.clock.hour == 12 && st.clock.day == 28 && st.clock.month == 10 && st.clock.year == 26);
    CHECK(st.filter_pct == 63 && st.boost_delay_s == 60 && st.overrun_s == 900 && st.humidity_setpoint == 65);

    /* values drift over time */
    int16_t t0 = st.temp_extract_x10;
    xfer(MVHR_CMD_STATUS, NULL, 0, 150 * 1000);
    CHECK(st.temp_extract_x10 != t0);

    /* writes are applied */
    const uint8_t fan3 = 2, bypass1 = 1, on = 1;
    CHECK(xfer(MVHR_CMD_FAN, &fan3, 1, 0) == 0);
    CHECK(xfer(MVHR_CMD_BYPASS, &bypass1, 1, 0) == 0);
    CHECK(xfer(MVHR_CMD_HOLIDAY, &on, 1, 0) == 0);
    xfer(MVHR_CMD_STATUS, NULL, 0, 0);
    CHECK(st.fan_speed == 3 && st.bypass_override == 1 && st.bypass_auto_state == 1 && st.holiday);

    u16(MVHR_CMD_BOOST_WRITE, 120, 0);
    u16(MVHR_CMD_OVERRUN_WRITE, 1800, 0);
    u16(MVHR_CMD_HUMID_WRITE, 99, 0);
    xfer(MVHR_CMD_BOOST_READ, NULL, 0, 0);
    xfer(MVHR_CMD_OVERRUN_READ, NULL, 0, 0);
    xfer(MVHR_CMD_HUMID_READ, NULL, 0, 0);
    CHECK(st.boost_delay_s == 120 && st.overrun_s == 1800 && st.humidity_setpoint == 99);

    const uint8_t night_off[4] = {0x01, 0x2C, 0x01, 0xE0};     /* disabled, 05:00 .. 08:00 */
    CHECK(xfer(MVHR_CMD_NIGHT, night_off, 4, 0) == 0);
    xfer(MVHR_CMD_CONFIG_READ, NULL, 0, 0);
    CHECK(!st.night_enabled && st.night_start_min == 300 && st.night_end_min == 480);

    /* filter saturation grows, reset clears it */
    xfer(MVHR_CMD_COUNTERS, NULL, 0, 60 * 60 * 1000);
    CHECK(st.filter_pct == 69 && st.hours == 12346);
    CHECK(xfer(MVHR_CMD_FILTER_RESET, NULL, 0, 60 * 60 * 1000) == 0);
    xfer(MVHR_CMD_COUNTERS, NULL, 0, 60 * 60 * 1000);
    CHECK(st.filter_pct == 0);

    /* clock: set 23:59 28/02/2028 (leap year), then it ticks over into 29/02 and on into March */
    mvhr_clock_t c = st.clock;
    c.hour = 23, c.minute = 59, c.day = 28, c.month = 2, c.year = 28;
    uint8_t blk[9];
    mvhr_encode_clock(&c, blk);
    CHECK(xfer(MVHR_CMD_CLOCK_TIME, blk, 9, 1000) == 1);
    xfer(MVHR_CMD_CLOCK_READ, NULL, 0, 1000 + 60 * 1000);
    CHECK(st.clock.hour == 0 && st.clock.minute == 0 && st.clock.day == 29 && st.clock.month == 2);
    xfer(MVHR_CMD_CLOCK_READ, NULL, 0, 1000 + (24 * 60 + 1) * 60 * 1000);
    CHECK(st.clock.day == 1 && st.clock.month == 3 && st.clock.year == 28);

    /* night inhibition shows in the status byte while the clock is inside the window */
    const uint8_t night_on[4] = {0x80 | (23 * 60) >> 8, (23 * 60) & 0xFF, 0x00, 0x00};    /* 23:00 .. 00:00 */
    CHECK(xfer(MVHR_CMD_NIGHT, night_on, 4, 0) == 0);
    xfer(MVHR_CMD_STATUS, NULL, 0, 1000);
    CHECK((st.status & 0x0F) == MVHR_FUNC_NIGHT_INHIBIT);

    /* installer config write: the whole 49-byte block, here with intake speed 2 changed to 55.0 % */
    uint8_t cfg[49];
    CHECK(xfer(MVHR_CMD_CONFIG_READ, NULL, 0, 0) == 49);   /* read fresh, as the firmware does before a write */
    memcpy(cfg, st.cfg, sizeof(cfg));
    cfg[10] = (uint8_t)((550 * 4096 / 1000) >> 8);
    cfg[11] = (uint8_t)((550 * 4096 / 1000) & 0xFF);
    CHECK(xfer(MVHR_CMD_CONFIG_WRITE, cfg, 4, 0) == -1);            /* a short block is refused */
    CHECK(xfer(MVHR_CMD_CONFIG_WRITE, cfg, sizeof(cfg), 0) == 0);
    CHECK(xfer(MVHR_CMD_CONFIG_READ, NULL, 0, 0) == 49);
    CHECK(((st.cfg[10] << 8) | st.cfg[11]) == 550 * 4096 / 1000);
    CHECK(((st.cfg[8] << 8) | st.cfg[9]) == 30 * 4096 / 100);       /* the others are untouched */
    CHECK(st.night_enabled && st.night_start_min == 23 * 60);

    /* never answered */
    uint8_t blob[67] = {0x43};
    CHECK(xfer(0x13, NULL, 0, 0) == -1);
    for (int cmd = 0xB0; cmd <= 0xB2; cmd++) {
        CHECK(xfer((uint8_t)cmd, blob, 4, 0) == -1);
    }
    CHECK(xfer(MVHR_CMD_FAN, NULL, 0, 0) == -1);

    printf(fails ? "%d FAILED\n" : "all sim tests passed\n", fails);
    return fails != 0;
}
