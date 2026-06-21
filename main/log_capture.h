/**
 * AquaGen Lite — in-RAM log ring buffer. Mirrors all ESP_LOG output into a circular buffer so the
 * recent serial log can be pulled remotely (GET /logs over the WireGuard VPN) — no USB needed.
 */
#ifndef LOG_CAPTURE_H
#define LOG_CAPTURE_H

#include <stddef.h>

// Install the esp_log hook (also keeps normal UART output). Call once, early in app_main.
void log_capture_init(void);

// Copy the buffered log (oldest→newest) into out (NUL-terminated). Returns bytes written.
size_t log_capture_dump(char *out, size_t out_len);

#endif // LOG_CAPTURE_H
