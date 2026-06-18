#include "provisioning.h"
#include "iot_configs.h"
#include "app_config.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "provision";
static char s_reg_id[32] = "";

const char *provisioning_registration_id(void)
{
    if (s_reg_id[0] == '\0') {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_reg_id, sizeof(s_reg_id), "%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return s_reg_id;
}

// Derive the per-device key from the group enrollment key: base64(HMAC-SHA256(decode(groupKey), regId)).
static bool derive_device_key(const char *group_key_b64, const char *reg_id, char *out, size_t out_len)
{
    unsigned char gk[64]; size_t gk_len = 0;
    if (mbedtls_base64_decode(gk, sizeof(gk), &gk_len,
                              (const unsigned char *)group_key_b64, strlen(group_key_b64)) != 0)
        return false;
    unsigned char mac[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(info, gk, gk_len, (const unsigned char *)reg_id, strlen(reg_id), mac) != 0)
        return false;
    size_t n = 0;
    if (mbedtls_base64_encode((unsigned char *)out, out_len, &n, mac, sizeof(mac)) != 0)
        return false;
    out[n] = '\0';
    return true;
}

// --- DPS over MQTT (synchronous, event-group driven) ---
#define DPS_BIT_ASSIGNED   BIT0
#define DPS_BIT_FAIL       BIT1

static EventGroupHandle_t s_dps_ev;
static char s_assigned_hub[96];
static char s_assigned_dev[64];
static char s_op_id[64];

// url-encode (same set as sas_token).
static void urlenc(const char *src, char *dst, size_t dst_len)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 4 < dst_len; p++) {
        unsigned char c = *p;
        if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') dst[o++]=c;
        else { dst[o++]='%'; dst[o++]=hex[c>>4]; dst[o++]=hex[c&0xF]; }
    }
    dst[o]='\0';
}

// SAS token for DPS: resource = "{idScope}/registrations/{regId}".
static bool dps_sas(const char *id_scope, const char *reg_id, const char *device_key_b64,
                    char *out, size_t out_len)
{
    char resource[160], res_enc[256];
    snprintf(resource, sizeof(resource), "%s/registrations/%s", id_scope, reg_id);
    urlenc(resource, res_enc, sizeof(res_enc));
    int64_t exp = (int64_t)time(NULL) + 3600;
    char to_sign[320];
    int sl = snprintf(to_sign, sizeof(to_sign), "%s\n%lld", res_enc, (long long)exp);
    unsigned char key[64]; size_t kl=0;
    if (mbedtls_base64_decode(key, sizeof(key), &kl, (const unsigned char*)device_key_b64, strlen(device_key_b64))!=0) return false;
    unsigned char mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, kl,
                    (const unsigned char*)to_sign, sl, mac);
    char sig_b64[64]; size_t sn=0;
    mbedtls_base64_encode((unsigned char*)sig_b64, sizeof(sig_b64), &sn, mac, sizeof(mac));
    sig_b64[sn]='\0';
    char sig_enc[128]; urlenc(sig_b64, sig_enc, sizeof(sig_enc));
    snprintf(out, out_len, "SharedAccessSignature sr=%s&sig=%s&se=%lld&skn=registration",
             res_enc, sig_enc, (long long)exp);
    return true;
}

static void dps_parse_result(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;
    cJSON *status = cJSON_GetObjectItem(root, "status");
    cJSON *opid = cJSON_GetObjectItem(root, "operationId");
    if (opid && cJSON_IsString(opid)) strlcpy(s_op_id, opid->valuestring, sizeof(s_op_id));

    cJSON *reg = cJSON_GetObjectItem(root, "registrationState");
    if (reg) {
        cJSON *hub = cJSON_GetObjectItem(reg, "assignedHub");
        cJSON *dev = cJSON_GetObjectItem(reg, "deviceId");
        if (hub && cJSON_IsString(hub)) strlcpy(s_assigned_hub, hub->valuestring, sizeof(s_assigned_hub));
        if (dev && cJSON_IsString(dev)) strlcpy(s_assigned_dev, dev->valuestring, sizeof(s_assigned_dev));
    }
    if (status && cJSON_IsString(status)) {
        if (strcmp(status->valuestring, "assigned") == 0 && s_assigned_hub[0])
            xEventGroupSetBits(s_dps_ev, DPS_BIT_ASSIGNED);
        else if (strcmp(status->valuestring, "failed") == 0)
            xEventGroupSetBits(s_dps_ev, DPS_BIT_FAIL);
        // "assigning" → keep polling (handled by caller)
    }
    cJSON_Delete(root);
}

static esp_mqtt_client_handle_t s_dps_client;

static void dps_event(void *a, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    if (id == MQTT_EVENT_CONNECTED) {
        esp_mqtt_client_subscribe(s_dps_client, "$dps/registrations/res/#", 1);
        // Body MUST carry the real registrationId (was empty "" → DPS register would fail / mis-assign).
        char body[64];
        snprintf(body, sizeof(body), "{\"registrationId\":\"%s\"}", provisioning_registration_id());
        esp_mqtt_client_publish(s_dps_client,
            "$dps/registrations/PUT/iotdps-register/?$rid=1", body, 0, 1, 0);
    } else if (id == MQTT_EVENT_DATA) {
        dps_parse_result(e->data, e->data_len);
    }
}

static bool run_dps(void)
{
    if (DPS_ID_SCOPE[0] == '\0' || DPS_GROUP_SYMMETRIC_KEY[0] == '\0') {
        ESP_LOGE(TAG, "DPS not configured (ID Scope / group key empty)");
        return false;
    }
    const char *reg_id = provisioning_registration_id();
    char dev_key[64];
    if (!derive_device_key(DPS_GROUP_SYMMETRIC_KEY, reg_id, dev_key, sizeof(dev_key))) {
        ESP_LOGE(TAG, "device key derivation failed");
        return false;
    }
    char sas[320];
    if (!dps_sas(DPS_ID_SCOPE, reg_id, dev_key, sas, sizeof(sas))) return false;

    char uri[96]; snprintf(uri, sizeof(uri), "mqtts://%s:8883", DPS_GLOBAL_ENDPOINT);
    char user[160];
    snprintf(user, sizeof(user), "%s/registrations/%s/api-version=%s",
             DPS_ID_SCOPE, reg_id, DPS_API_VERSION);

    s_dps_ev = xEventGroupCreate();
    s_assigned_hub[0] = s_assigned_dev[0] = s_op_id[0] = '\0';

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = user,
        .credentials.client_id = reg_id,
        .credentials.authentication.password = sas,
    };
    s_dps_client = esp_mqtt_client_init(&mc);
    esp_mqtt_client_register_event(s_dps_client, ESP_EVENT_ANY_ID, dps_event, NULL);
    esp_mqtt_client_start(s_dps_client);

    ESP_LOGI(TAG, "DPS registering as '%s'...", reg_id);
    EventBits_t bits = xEventGroupWaitBits(s_dps_ev, DPS_BIT_ASSIGNED | DPS_BIT_FAIL,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(60000));
    esp_mqtt_client_stop(s_dps_client);
    esp_mqtt_client_destroy(s_dps_client);
    s_dps_client = NULL;

    if (!(bits & DPS_BIT_ASSIGNED)) {
        ESP_LOGE(TAG, "DPS registration failed/timeout");
        return false;
    }

    // Persist assigned identity.
    app_config_t *cfg = app_config_get();
    strlcpy(cfg->iothub_fqdn, s_assigned_hub, sizeof(cfg->iothub_fqdn));
    strlcpy(cfg->device_id,   s_assigned_dev, sizeof(cfg->device_id));
    strlcpy(cfg->device_key,  dev_key,        sizeof(cfg->device_key));
    app_config_save();
    ESP_LOGI(TAG, "DPS assigned: hub=%s device=%s", s_assigned_hub, s_assigned_dev);
    return true;
}

bool provisioning_ensure(provision_mode_t mode)
{
    app_config_t *cfg = app_config_get();
    bool baked = (cfg->device_id[0] != '\0' && cfg->device_key[0] != '\0');

    switch (mode) {
    case PROVISION_BAKED:
        if (!baked) ESP_LOGE(TAG, "baked identity missing");
        return baked;
    case PROVISION_DPS:
        return run_dps();
    case PROVISION_AUTO:
    default:
        if (baked) { ESP_LOGI(TAG, "using baked-at-flash identity"); return true; }
        return run_dps();
    }
}
