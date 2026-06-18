/** AquaGen Lite — Modbus RTU master over UART2 (RS485). Reads flow consumption. */
#ifndef MODBUS_METER_H
#define MODBUS_METER_H

#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

// Initialize UART2 for RS485 at the given baud/parity. Call before reads; re-call to change line settings.
void modbus_meter_init(uint32_t baud, const char *parity);

typedef struct {
    bool   ok;            // true if read succeeded
    double consumption;   // parsed value (raw int32 * MODBUS_SCALE)
    // Alarm flags — STUBBED until meter datasheet/register map arrives.
    bool   alarm_battery_low;
    bool   alarm_empty_pipe;
    bool   alarm_reverse_flow;
    bool   alarm_over_range;
} meter_reading_t;

// Read one meter (holding reg 0x0007, qty 2, CDAB->int32). Retries per config. Honours the meter's baud/parity.
meter_reading_t modbus_meter_read(const meter_cfg_t *meter, uint8_t retry_count, uint16_t retry_delay_ms);

// Running Modbus health counters (ported from modbus_iot_gateway) — the key field diagnostic:
// distinguishes "meter unplugged / wrong address" (timeouts) from "noise on the bus" (crc_errors)
// from "wrong register/func" (last_exception). Reported to the Device Twin + /api/system_status.
typedef struct {
    uint32_t total;          // logical read calls
    uint32_t ok;             // successful reads
    uint32_t failed;         // reads that failed after all retries
    uint32_t timeouts;       // attempts with no / short reply
    uint32_t crc_errors;     // attempts with a CRC mismatch
    uint8_t  last_exception; // last Modbus exception code seen (0 = none)
} modbus_stats_t;

const modbus_stats_t *modbus_meter_stats(void);

#endif // MODBUS_METER_H
