#include "log_capture.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#define LOG_RING_SIZE 6144   // last ~6 KB of log (RAM ring); plenty for a recent-history view

static char s_ring[LOG_RING_SIZE];
static size_t s_head = 0;     // next write position
static size_t s_len  = 0;     // valid bytes (caps at LOG_RING_SIZE)
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig = NULL;

static void ring_write(const char *data, int n)
{
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++) {
        s_ring[s_head] = data[i];
        s_head = (s_head + 1) % LOG_RING_SIZE;
        if (s_len < LOG_RING_SIZE) s_len++;
    }
    portEXIT_CRITICAL(&s_mux);
}

// esp_log hook: format into a local buffer, copy to the ring, then pass through to the real UART
// vprintf so serial-monitor output is unchanged.
static int log_vprintf(const char *fmt, va_list args)
{
    char buf[256];
    va_list cp;
    va_copy(cp, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, cp);
    va_end(cp);
    if (n > 0) ring_write(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
    return s_orig ? s_orig(fmt, args) : n;
}

void log_capture_init(void)
{
    s_orig = esp_log_set_vprintf(log_vprintf);
}

size_t log_capture_dump(char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;
    portENTER_CRITICAL(&s_mux);
    size_t n = (s_len < out_len - 1) ? s_len : out_len - 1;
    size_t start = (s_head + LOG_RING_SIZE - s_len) % LOG_RING_SIZE;  // oldest byte
    for (size_t i = 0; i < n; i++) out[i] = s_ring[(start + i) % LOG_RING_SIZE];
    portEXIT_CRITICAL(&s_mux);
    out[n] = '\0';
    return n;
}
