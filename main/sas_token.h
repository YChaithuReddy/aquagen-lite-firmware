/** AquaGen Lite — Azure IoT Hub SAS token (HMAC-SHA256 of resource+expiry, signed with device key). */
#ifndef SAS_TOKEN_H
#define SAS_TOKEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Generate a SAS token for the given hub/device using the base64 device key.
 * Writes "SharedAccessSignature sr=...&sig=...&se=..." into out.
 * Returns true on success. `expiry_unix` receives the token expiry (for renewal checks).
 */
bool sas_token_generate(const char *iothub_fqdn,
                        const char *device_id,
                        const char *device_key_b64,
                        int duration_minutes,
                        char *out, size_t out_len,
                        int64_t *expiry_unix);

#endif // SAS_TOKEN_H
