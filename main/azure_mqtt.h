/** AquaGen Lite — Azure IoT Hub MQTT client (MQTTS:8883, SAS auth, TLS via cert bundle). */
#ifndef AZURE_MQTT_H
#define AZURE_MQTT_H

#include <stdbool.h>
#include <stdint.h>

// Start the MQTT client using the live app_config identity. Returns true if start was initiated.
bool azure_mqtt_start(void);
void azure_mqtt_stop(void);

bool azure_mqtt_is_connected(void);
uint32_t azure_mqtt_reconnect_count(void);   // MQTT reconnects since boot (flaky-link diagnostic)

// Publish a telemetry JSON payload to devices/{id}/messages/events/. Returns true if enqueued.
bool azure_mqtt_publish_telemetry(const char *json);

// Publish to an arbitrary topic (used by Device Twin reported-properties). Returns true if enqueued.
bool azure_mqtt_publish_raw(const char *topic, const char *payload);

// True if the SAS token is near/after expiry and the client should be restarted.
bool azure_mqtt_sas_expiring(void);

// Optional callback for cloud-to-device messages (used later by Device Twin / OTA).
typedef void (*azure_c2d_cb_t)(const char *topic, const char *data, int len);
void azure_mqtt_set_c2d_cb(azure_c2d_cb_t cb);

#endif // AZURE_MQTT_H
