/** AquaGen Lite — Azure Device Twin: apply desired props, report state. (Spec §15) */
#ifndef DEVICE_TWIN_H
#define DEVICE_TWIN_H

// Register the twin handler with azure_mqtt's C2D callback. Call after azure_mqtt_start().
void device_twin_init(void);

// Request the full twin (desired props) — call once after MQTT connects.
void device_twin_request(void);

// Publish current reported properties to the hub.
void device_twin_report(void);

#endif // DEVICE_TWIN_H
