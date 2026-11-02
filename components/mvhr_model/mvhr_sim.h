#pragma once
/*
 * Built-in simulated unit (pure C, host-testable), answering like the real unit's firmware 1.3.
 * Reports firmware 1.3 with the 49-byte config block. Values drift slowly so the UI and HA have something to show.
 */
#include <stdint.h>

/* Reset the simulated unit to its power-on state. */
void mvhr_sim_init(void);

/*
 * Answer one request. Writes the reply data to out (VBUS_MAX_DATA bytes) and returns its length,
 * or -1 for no reply (unknown command, short data, and never for 0x10 / 0x13 / 0xB0-0xB2).
 */
int mvhr_sim_reply(uint8_t cmd, const uint8_t *data, uint8_t len, uint8_t *out, uint32_t now_ms);
