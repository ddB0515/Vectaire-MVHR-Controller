#pragma once
/* Reply decoders (pure C, host-testable). Each returns the MVHR_G_* group it updated, or 0 if the reply was unusable. */
#include <stdint.h>

#include "mvhr.h"

#define MVHR_CMD_STATUS         0x00
#define MVHR_CMD_FIRE           0x01
#define MVHR_CMD_CLOCK_READ     0x02
#define MVHR_CMD_CLOCK_TIME     0x03
#define MVHR_CMD_CLOCK_DATE     0x04
#define MVHR_CMD_CLOCK_REGION   0x05
#define MVHR_CMD_COUNTERS       0x06
#define MVHR_CMD_FILTER_RESET   0x07
#define MVHR_CMD_BOOST_READ     0x08
#define MVHR_CMD_BOOST_WRITE    0x09
#define MVHR_CMD_OVERRUN_READ   0x0A
#define MVHR_CMD_OVERRUN_WRITE  0x0B
#define MVHR_CMD_HUMID_READ     0x0C
#define MVHR_CMD_HUMID_WRITE    0x0D
#define MVHR_CMD_FAN            0x0E
#define MVHR_CMD_CONFIG_READ    0x0F
#define MVHR_CMD_CONFIG_WRITE   0x10    /* the whole 49-byte installer config block */
#define MVHR_CMD_BYPASS         0x11
#define MVHR_CMD_NIGHT          0x12
#define MVHR_CMD_HOLIDAY        0x15
#define MVHR_CMD_VERSION        0x16

uint32_t mvhr_decode_reply(mvhr_state_t *st, uint8_t cmd, const uint8_t *data, uint8_t len);

/* Build the 9-byte clock block for writes from the current clock */
void mvhr_encode_clock(const mvhr_clock_t *c, uint8_t out[9]);
