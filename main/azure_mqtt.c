#include "azure_mqtt.h"
#include "iot_configs.h"
#include "app_config.h"
#include "sas_token.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "azmqtt";

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;
static uint32_t s_connect_count = 0;   // CONNECTED events; reconnects = this - 1
static int64_t s_sas_expiry = 0;
static azure_c2d_cb_t s_c2d_cb = NULL;

static char s_uri[160];
static char s_username[192];
static char s_password[320];   // SAS token
static char s_client_id[80];
static char s_tele_topic[128];

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_connect_count++;
        ESP_LOGI(TAG, "MQTT connected");
        // Subscribe to C2D + device-twin topics (used by Device Twin / OTA later).
        esp_mqtt_client_subscribe(s_client, "$iothub/twin/PATCH/properties/desired/#", 1);
        esp_mqtt_client_subscribe(s_client, "$iothub/twin/res/#", 1);
        {
            char c2d[96];
            snprintf(c2d, sizeof(c2d), "devices/%s/messages/devicebound/#", app_config_get()->device_id);
            esp_mqtt_client_subscribe(s_client, c2d, 1);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_DATA:
        if (s_c2d_cb && e->topic && e->data) {
            char topic[160];
            int tl = e->topic_len < (int)sizeof(topic) - 1 ? e->topic_len : (int)sizeof(topic) - 1;
            memcpy(topic, e->topic, tl); topic[tl] = '\0';
            s_c2d_cb(topic, e->data, e->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;
    default:
        break;
    }
}

bool azure_mqtt_start(void)
{
    azure_mqtt_stop();
    app_config_t *cfg = app_config_get();
    if (cfg->device_id[0] == '\0') { ESP_LOGE(TAG, "no device_id"); return false; }

    if (!sas_token_generate(cfg->iothub_fqdn, cfg->device_id, cfg->device_key,
                            SAS_TOKEN_DURATION_MIN, s_password, sizeof(s_password), &s_sas_expiry)) {
        ESP_LOGE(TAG, "SAS generation failed");
        return false;
    }

    snprintf(s_uri, sizeof(s_uri), "mqtts://%s:%d", cfg->iothub_fqdn, MQTT_PORT);
    snprintf(s_username, sizeof(s_username), "%s/%s/?api-version=2021-04-12",
             cfg->iothub_fqdn, cfg->device_id);
    strlcpy(s_client_id, cfg->device_id, sizeof(s_client_id));
    snprintf(s_tele_topic, sizeof(s_tele_topic), "devices/%s/messages/events/", cfg->device_id);

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = s_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username = s_username,
        .credentials.client_id = s_client_id,
        .credentials.authentication.password = s_password,
        .session.keepalive = 30,
        .network.disable_auto_reconnect = false,
    };
    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) { ESP_LOGE(TAG, "client init failed"); return false; }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        ESP_LOGE(TAG, "client start failed");
        azure_mqtt_stop();
        return false;
    }
    ESP_LOGI(TAG, "MQTT starting → %s", s_uri);
    return true;
}

void azure_mqtt_stop(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
}

bool azure_mqtt_is_connected(void) { return s_connected; }
uint32_t azure_mqtt_reconnect_count(void) { return s_connect_count > 0 ? s_connect_count - 1 : 0; }

bool azure_mqtt_publish_telemetry(const char *json)
{
    if (!s_client || !s_connected) return false;
    int id = esp_mqtt_client_publish(s_client, s_tele_topic, json, 0, 1, 0);
    return id >= 0;
}

bool azure_mqtt_publish_raw(const char *topic, const char *payload)
{
    if (!s_client || !s_connected) return false;
    return esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0) >= 0;
}

bool azure_mqtt_sas_expiring(void)
{
    // renew 5 min before expiry
    return (int64_t)time(NULL) > (s_sas_expiry - 300);
}

void azure_mqtt_set_c2d_cb(azure_c2d_cb_t cb) { s_c2d_cb = cb; }
