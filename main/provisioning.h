/**
 * AquaGen Lite — device identity provisioning. TWO paths (build both, test, pick one):
 *
 *  A) DPS (Azure Device Provisioning Service): flash identical firmware to all 150; each box
 *     self-registers on first connect using its eFuse-MAC registration id + a group symmetric
 *     key, and DPS hands back the assigned IoT Hub + device id (cached in NVS).
 *
 *  B) Bake-at-flash: a per-device NVS image (device_id + device_key + ap_password) is flashed
 *     alongside firmware at the factory. Nothing to do at runtime — app_config already loaded it.
 *
 * Selection is runtime: if a device_key is already present (bake-at-flash), we skip DPS;
 * otherwise, if DPS is enabled, we run DPS. Controlled by PROVISION_MODE below.
 */
#ifndef PROVISIONING_H
#define PROVISIONING_H

#include <stdbool.h>

typedef enum {
    PROVISION_AUTO = 0,   // bake-at-flash if device_key present, else DPS
    PROVISION_BAKED,      // force bake-at-flash only (no DPS)
    PROVISION_DPS,        // force DPS
} provision_mode_t;

// Ensure the device has a usable identity. Must run AFTER WiFi + time sync, BEFORE azure_mqtt_start().
// Returns true if an identity is ready (device_id + key resolvable). Persists DPS results to NVS.
bool provisioning_ensure(provision_mode_t mode);

// Convenience: the registration id used for DPS (eFuse MAC as hex). Also used for QR / labels.
const char *provisioning_registration_id(void);

#endif // PROVISIONING_H
