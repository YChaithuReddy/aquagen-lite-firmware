#include "sas_token.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "esp_log.h"

static const char *TAG = "sas";

// Percent-encode the characters Azure's SAS scheme requires (RFC 3986 unreserved are left as-is).
static void url_encode(const char *src, char *dst, size_t dst_len)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 4 < dst_len; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0xF];
        }
    }
    dst[o] = '\0';
}

bool sas_token_generate(const char *iothub_fqdn, const char *device_id,
                        const char *device_key_b64, int duration_minutes,
                        char *out, size_t out_len, int64_t *expiry_unix)
{
    if (!device_key_b64 || device_key_b64[0] == '\0') {
        ESP_LOGE(TAG, "no device key");
        return false;
    }

    // resource URI = "{fqdn}/devices/{deviceId}", then URL-encoded.
    char resource[160];
    snprintf(resource, sizeof(resource), "%s/devices/%s", iothub_fqdn, device_id);
    char resource_enc[256];
    url_encode(resource, resource_enc, sizeof(resource_enc));

    int64_t expiry = (int64_t)time(NULL) + (int64_t)duration_minutes * 60;
    if (expiry_unix) *expiry_unix = expiry;

    // string to sign = "{resource_enc}\n{expiry}"
    char to_sign[320];
    int sign_len = snprintf(to_sign, sizeof(to_sign), "%s\n%lld", resource_enc, (long long)expiry);

    // decode the base64 device key
    unsigned char key[64];
    size_t key_len = 0;
    if (mbedtls_base64_decode(key, sizeof(key), &key_len,
                              (const unsigned char *)device_key_b64, strlen(device_key_b64)) != 0) {
        ESP_LOGE(TAG, "device key base64 decode failed");
        return false;
    }

    // HMAC-SHA256
    unsigned char hmac[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(info, key, key_len, (const unsigned char *)to_sign, sign_len, hmac) != 0) {
        ESP_LOGE(TAG, "hmac failed");
        return false;
    }

    // base64(signature) then url-encode it
    char sig_b64[64];
    size_t sig_len = 0;
    if (mbedtls_base64_encode((unsigned char *)sig_b64, sizeof(sig_b64), &sig_len, hmac, sizeof(hmac)) != 0) {
        ESP_LOGE(TAG, "sig base64 encode failed");
        return false;
    }
    sig_b64[sig_len] = '\0';
    char sig_enc[128];
    url_encode(sig_b64, sig_enc, sizeof(sig_enc));

    int n = snprintf(out, out_len,
                     "SharedAccessSignature sr=%s&sig=%s&se=%lld",
                     resource_enc, sig_enc, (long long)expiry);
    if (n <= 0 || (size_t)n >= out_len) {
        ESP_LOGE(TAG, "token buffer too small");
        return false;
    }
    return true;
}
