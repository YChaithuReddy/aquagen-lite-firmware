#include "telemetry.h"
#include "azure_mqtt.h"
#include "flash_buffer.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "telemetry";

// Pace replayed records so a burst flush after a long outage doesn't trip Azure IoT Hub's
// per-device throttle (which silently drops messages on the free/B1 tiers).
#define REPLAY_DELAY_MS 300

static uint32_t s_sent = 0;       // published to Azure (live + replayed)
static uint32_t s_buffered = 0;   // written to the offline buffer (failed live send)
uint32_t telemetry_sent_count(void)     { return s_sent; }
uint32_t telemetry_buffered_count(void) { return s_buffered; }

void telemetry_init(void)
{
    flash_buffer_init();
}

static void iso8601_utc(char *out, size_t len)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

// Build payload matching the legacy schema: {consumption, created_on, type, unit_id, serial}.
static void build_json(const meter_cfg_t *m, const meter_reading_t *r, char *out, size_t len)
{
    char ts[32];
    iso8601_utc(ts, sizeof(ts));
    double v = (r && r->ok && !isnan(r->consumption)) ? r->consumption : 0.0;
    snprintf(out, len,
             "{\"consumption\":%.2f,\"created_on\":\"%s\",\"type\":\"FLOW\","
             "\"unit_id\":\"%s\",\"serial\":\"%s\"}",
             v, ts, m->unit_id, m->serial);
}

bool telemetry_send(const meter_cfg_t *meter, const meter_reading_t *reading)
{
    char json[256];
    build_json(meter, reading, json, sizeof(json));

    ESP_LOGI(TAG, "──> publishing to Azure: %s", json);
    if (azure_mqtt_is_connected() && azure_mqtt_publish_telemetry(json)) {
        s_sent++;
        ESP_LOGI(TAG, "    ✅ SENT to Azure IoT Hub (slave %u)", meter->slave_id);
        return true;
    }
    // offline (or publish failed) → store-and-forward
    flash_buffer_push(json);
    s_buffered++;
    ESP_LOGW(TAG, "    ⚠ MQTT not connected — buffered to flash (%u queued, will resend on reconnect)",
             (unsigned)flash_buffer_count());
    return false;
}

static bool replay_send(const char *json, void *ctx)
{
    (void)ctx;
    if (!azure_mqtt_is_connected()) return false;
    if (!azure_mqtt_publish_telemetry(json)) return false;
    s_sent++;
    vTaskDelay(pdMS_TO_TICKS(REPLAY_DELAY_MS));   // pace the flush to avoid Azure throttling
    return true;
}

int telemetry_flush_buffer(void)
{
    if (flash_buffer_is_empty() || !azure_mqtt_is_connected()) return 0;
    int n = (int)flash_buffer_replay(replay_send, NULL);
    if (n > 0) ESP_LOGI(TAG, "    ✅ resent %d buffered reading(s) to Azure", n);
    return n;
}
