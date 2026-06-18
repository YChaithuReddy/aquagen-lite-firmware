#include "device_twin.h"
#include "azure_mqtt.h"
#include "app_config.h"
#include "iot_configs.h"
#include "ota.h"
#include "modbus_meter.h"
#include "telemetry.h"
#include "flash_buffer.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "cJSON.h"
#include "nvs.h"

static const char *TAG = "twin";
static int s_rid = 1;

// Defined in main.c — number of unhealthy self-recovery reboots (fleet diagnostics).
extern uint32_t app_restart_count(void);
// Defined in main.c — queues an OTA to run from the main task (can't stop MQTT from here).
extern void ota_request(const char *url);
// Defined in main.c — runtime config-web-UI request (lets the twin open config remotely).
extern volatile bool g_web_open_req;

// Last desired-twin $version we applied, persisted so a stale desired (e.g. reboot_device=true or
// an old ota_url left in the twin) is NOT re-applied on every reconnect/boot → no action loops.
static uint32_t twin_version_get(void)
{
    nvs_handle_t h; uint32_t v = 0;
    if (nvs_open("aquagen", NVS_READONLY, &h) == ESP_OK) { nvs_get_u32(h, "twin_ver", &v); nvs_close(h); }
    return v;
}
static void twin_version_set(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open("aquagen", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u32(h, "twin_ver", v); nvs_commit(h); nvs_close(h); }
}

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_SW:       return "sw-reboot";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "int-wdt";
        case ESP_RST_TASK_WDT: return "task-wdt";
        case ESP_RST_WDT:      return "wdt";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP:return "deepsleep";
        case ESP_RST_EXT:      return "ext-reset";
        default:               return "other";
    }
}

// Persisted across the OTA reboot so the SAME twin ota_url can't re-flash on every boot.
static bool ota_url_is_new(const char *url)
{
    nvs_handle_t h; char last[256] = {0}; size_t len = sizeof(last);
    if (nvs_open("aquagen", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, "last_ota", last, &len);
        nvs_close(h);
    }
    return strncmp(last, url, sizeof(last)) != 0;
}

static void ota_url_remember(const char *url)
{
    nvs_handle_t h;
    if (nvs_open("aquagen", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "last_ota", url);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Apply the "desired" object (whether from a PATCH or the full-twin GET response).
static void apply_desired(cJSON *desired)
{
    if (!desired) return;
    app_config_t *cfg = app_config_get();
    bool dirty = false;

    // Idempotency: skip if we've already applied this (or a newer) desired version. Persisted in
    // NVS so a stale desired property can't re-fire its action on every reconnect or after a reboot.
    cJSON *ver = cJSON_GetObjectItem(desired, "$version");
    uint32_t version = (ver && cJSON_IsNumber(ver)) ? (uint32_t)ver->valuedouble : 0;
    if (version != 0 && version <= twin_version_get()) {
        ESP_LOGI(TAG, "desired $version %lu already applied — skipping", (unsigned long)version);
        return;
    }
    if (version != 0) twin_version_set(version);   // record BEFORE any reboot-triggering action

    cJSON *it;
    if ((it = cJSON_GetObjectItem(desired, "web_server_enabled")) && cJSON_IsBool(it)) {
        g_web_open_req = cJSON_IsTrue(it);   // open/close the config web UI remotely
        ESP_LOGI(TAG, "web_server_enabled -> %d", (int)g_web_open_req);
    }
    if ((it = cJSON_GetObjectItem(desired, "telemetry_interval")) && cJSON_IsNumber(it)) {
        uint32_t v = (uint32_t)it->valuedouble;
        if (v >= TELEMETRY_INTERVAL_MIN_S && v <= TELEMETRY_INTERVAL_MAX_S && v != cfg->telemetry_interval_s) {
            cfg->telemetry_interval_s = v; dirty = true;
            ESP_LOGI(TAG, "telemetry_interval -> %lu s", (unsigned long)v);
        }
    }
    if ((it = cJSON_GetObjectItem(desired, "maintenance_mode")) && cJSON_IsBool(it)) {
        cfg->maintenance_mode = cJSON_IsTrue(it); dirty = true;
        ESP_LOGI(TAG, "maintenance_mode -> %d", cfg->maintenance_mode);
    }
    if ((it = cJSON_GetObjectItem(desired, "modbus_retry_count")) && cJSON_IsNumber(it)) {
        int v = it->valueint; if (v >= 0 && v <= 3) { cfg->modbus_retry_count = v; dirty = true; }
    }
    if ((it = cJSON_GetObjectItem(desired, "modbus_retry_delay")) && cJSON_IsNumber(it)) {
        int v = it->valueint; if (v >= 10 && v <= 500) { cfg->modbus_retry_delay_ms = v; dirty = true; }
    }
    if ((it = cJSON_GetObjectItem(desired, "ota_enabled")) && cJSON_IsBool(it)) {
        cfg->ota_enabled = cJSON_IsTrue(it); dirty = true;
    }

    // OTA trigger — QUEUE it for the main task (must not stop MQTT from this callback's task).
    // Guard on a persisted last-applied url so the same ota_url left in the twin can't re-fire
    // every boot (which, combined with the old in-task stop crash, caused a reboot loop).
    cJSON *ota_url = cJSON_GetObjectItem(desired, "ota_url");
    if (cfg->ota_enabled && ota_url && cJSON_IsString(ota_url) && ota_url->valuestring[0]) {
        if (ota_url_is_new(ota_url->valuestring)) {
            ESP_LOGI(TAG, "OTA requested: %s", ota_url->valuestring);
            ota_url_remember(ota_url->valuestring);   // persists across the OTA reboot
            ota_request(ota_url->valuestring);        // main loop performs it
        } else {
            ESP_LOGI(TAG, "OTA url unchanged — already applied, skipping");
        }
    }

    if (dirty) app_config_save();
    device_twin_report();   // echo new state back

    // reboot is a one-shot action — do it last, after reporting.
    if ((it = cJSON_GetObjectItem(desired, "reboot_device")) && cJSON_IsTrue(it)) {
        ESP_LOGW(TAG, "remote reboot requested");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
}

// azure_mqtt C2D callback — routes twin traffic.
static void on_message(const char *topic, const char *data, int len)
{
    if (strstr(topic, "twin/PATCH/properties/desired")) {
        cJSON *root = cJSON_ParseWithLength(data, len);
        if (root) { apply_desired(root); cJSON_Delete(root); }
    } else if (strstr(topic, "twin/res/")) {
        // full-twin GET response: { "desired": {...}, "reported": {...} }
        cJSON *root = cJSON_ParseWithLength(data, len);
        if (root) {
            apply_desired(cJSON_GetObjectItem(root, "desired"));
            cJSON_Delete(root);
        }
    }
    // devicebound/ C2D messages could be handled here too if needed.
}

void device_twin_init(void)
{
    azure_mqtt_set_c2d_cb(on_message);
}

void device_twin_request(void)
{
    char topic[64];
    snprintf(topic, sizeof(topic), "$iothub/twin/GET/?$rid=%d", s_rid++);
    azure_mqtt_publish_raw(topic, "");
}

void device_twin_report(void)
{
    app_config_t *cfg = app_config_get();
    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;

    char *json = NULL;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "firmware_version", FW_VERSION_STRING);
    cJSON_AddStringToObject(r, "device_id", cfg->device_id);
    cJSON_AddNumberToObject(r, "telemetry_interval", cfg->telemetry_interval_s);
    cJSON_AddNumberToObject(r, "modbus_retry_count", cfg->modbus_retry_count);
    cJSON_AddNumberToObject(r, "modbus_retry_delay", cfg->modbus_retry_delay_ms);
    cJSON_AddBoolToObject(r, "maintenance_mode", cfg->maintenance_mode);
    cJSON_AddBoolToObject(r, "ota_enabled", cfg->ota_enabled);
    cJSON_AddStringToObject(r, "network_mode", "WiFi");
    cJSON_AddNumberToObject(r, "rssi", rssi);
    cJSON_AddNumberToObject(r, "uptime_sec", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(r, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(r, "min_free_heap", (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(r, "restart_count", (double)app_restart_count());
    cJSON_AddStringToObject(r, "reset_reason", reset_reason_str());
    cJSON_AddNumberToObject(r, "mqtt_reconnects", (double)azure_mqtt_reconnect_count());
    cJSON_AddNumberToObject(r, "telemetry_sent", (double)telemetry_sent_count());
    cJSON_AddNumberToObject(r, "telemetry_buffered", (double)telemetry_buffered_count());
    cJSON_AddNumberToObject(r, "buffer_queued", (double)flash_buffer_count());
    cJSON_AddStringToObject(r, "ota_status", ota_status_str());

    // Modbus health — the key field diagnostic (unplugged-meter vs bus-noise vs wrong-register).
    const modbus_stats_t *ms = modbus_meter_stats();
    cJSON_AddNumberToObject(r, "modbus_total", (double)ms->total);
    cJSON_AddNumberToObject(r, "modbus_ok", (double)ms->ok);
    cJSON_AddNumberToObject(r, "modbus_failed", (double)ms->failed);
    cJSON_AddNumberToObject(r, "modbus_timeouts", (double)ms->timeouts);
    cJSON_AddNumberToObject(r, "modbus_crc_errors", (double)ms->crc_errors);
    cJSON_AddNumberToObject(r, "modbus_last_exception", (double)ms->last_exception);

    json = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (json) {
        char topic[80];
        snprintf(topic, sizeof(topic), "$iothub/twin/PATCH/properties/reported/?$rid=%d", s_rid++);
        azure_mqtt_publish_raw(topic, json);
        cJSON_free(json);
    }
}
