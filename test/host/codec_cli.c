/* Host-side driver for vbus_codec, used by test_codec.py.
 *   codec_cli enc <addr> <cmd> [data hex...]   -> prints wire bytes as hex
 *   codec_cli dec <wire hex...>                -> prints one line per frame / error
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vbus_codec.h"

int main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "enc") == 0) {
        uint8_t data[VBUS_MAX_DATA];
        int n = 0;
        for (int i = 4; i < argc && n < VBUS_MAX_DATA; i++) {
            data[n++] = (uint8_t)strtoul(argv[i], NULL, 16);
        }
        uint8_t out[VBUS_MAX_WIRE];
        size_t len = vbus_encode((uint8_t)strtoul(argv[2], NULL, 16), (uint8_t)strtoul(argv[3], NULL, 16), data,
                                 (uint8_t)n, out, sizeof(out));
        for (size_t i = 0; i < len; i++) {
            printf("%02x%s", out[i], i + 1 < len ? " " : "\n");
        }
        return len ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "dec") == 0) {
        vbus_decoder_t d;
        vbus_frame_t f;
        vbus_decoder_reset(&d);
        for (int i = 2; i < argc; i++) {
            vbus_dec_result_t r = vbus_decoder_feed(&d, (uint8_t)strtoul(argv[i], NULL, 16), &f);
            if (r == VBUS_DEC_FRAME) {
                printf("FRAME %02x %02x", f.addr, f.cmd);
                for (int k = 0; k < f.len; k++) {
                    printf(" %02x", f.data[k]);
                }
                printf("\n");
            } else if (r != VBUS_DEC_NONE) {
                printf("ERROR %d\n", (int)r);
            }
        }
        return 0;
    }
    fprintf(stderr, "usage: codec_cli enc <addr> <cmd> [data...] | dec <bytes...>\n");
    return 2;
}
