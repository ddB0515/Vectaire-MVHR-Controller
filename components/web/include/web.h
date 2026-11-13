#pragma once
/* Web page and JSON API on the device's HTTP server (normal operation): live state, changes, diagnostics. */

#ifdef __cplusplus
extern "C" {
#endif

/* Register the home page and /api handlers; does nothing if Wi-Fi isn't configured. */
void web_start(void);

#ifdef __cplusplus
}
#endif
