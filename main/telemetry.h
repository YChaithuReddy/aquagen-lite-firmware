/** AquaGen Lite — telemetry: build JSON, publish to Azure or buffer offline, replay on reconnect. */
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdbool.h>
#include "modbus_meter.h"
#include "app_config.h"

void telemetry_init(void);   // mounts flash buffer

// Tell telemetry whether the clock is NTP-authoritative yet, and this boot's id. While NOT
// authoritative, buffered records are tagged provisional so they can be retimed once NTP arrives.
void telemetry_set_clock(bool authoritative, unsigned boot_id);

// Build the telemetry JSON for a reading + publish; if offline, queue to flash. Returns true if published live.
bool telemetry_send(const meter_cfg_t *meter, const meter_reading_t *reading);

// Try to flush any buffered records to Azure (call after MQTT (re)connects). Returns count sent.
int  telemetry_flush_buffer(void);

// Lifetime counters for remote diagnostics (reported to the Device Twin).
uint32_t telemetry_sent_count(void);       // readings published to Azure (live + replayed)
uint32_t telemetry_buffered_count(void);   // readings written to the offline buffer (failed live)

#endif // TELEMETRY_H
