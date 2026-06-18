/** AquaGen Lite — OTA update from a URL (GitHub releases), with redirect handling + rollback. (§16) */
#ifndef OTA_H
#define OTA_H

#include <stdbool.h>

// Kick off an OTA download+flash from `url` in a background task. No-op if one is already running,
// OTA disabled, or the heap is too low. Reboots on success.
void ota_start(const char *url);

bool ota_in_progress(void);

// "idle" | "downloading" | "success" | "failed" — reported to the Device Twin.
const char *ota_status_str(void);

// Call once early at boot: if running a freshly-OTA'd image, mark it valid after health checks
// (cancels the rollback). Call after WiFi + Azure are confirmed up.
void ota_mark_valid_if_pending(void);

#endif // OTA_H
