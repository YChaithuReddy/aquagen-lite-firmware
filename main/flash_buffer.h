/**
 * AquaGen Lite — offline telemetry store-and-forward over LittleFS (no SD card on this HW).
 * Newline-delimited JSON records in a single log on the "storage" partition. Overwrite-oldest
 * when the cap is hit. Replayed in chronological order on reconnect.
 */
#ifndef FLASH_BUFFER_H
#define FLASH_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

void flash_buffer_init(void);                 // mount LittleFS; call once at boot

// Append one telemetry record (a complete JSON string). Drops oldest if over the cap.
bool flash_buffer_push(const char *json);

bool flash_buffer_is_empty(void);
size_t flash_buffer_count(void);

/**
 * Replay buffered records oldest-first. For each record, `send(json, ctx)` is called;
 * it must return true if the record was published successfully. Stops at the first failure
 * (keeps the rest for the next attempt). Returns the number successfully sent.
 */
typedef bool (*flash_buffer_send_fn)(const char *json, void *ctx);
size_t flash_buffer_replay(flash_buffer_send_fn send, void *ctx);

#endif // FLASH_BUFFER_H
