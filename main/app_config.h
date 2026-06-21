/**
 * AquaGen Lite — runtime configuration (persisted in NVS namespace "aquagen").
 * Identity (device_id/device_key/ap_password) is written at provisioning time;
 * network + meter settings are written by the field installer via the config app.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "iot_configs.h"

typedef struct {
    char     unit_id[24];      // logical meter id sent in telemetry (e.g. "TFG23316F")
    char     serial[24];       // physical meter serial number
    uint8_t  slave_id;         // Modbus slave address
    uint32_t baud;             // per-meter baud (default 2400)
    char     parity[6];        // "none" | "even" | "odd"
    bool     enabled;
} meter_cfg_t;

typedef struct {
    // --- Identity (provisioned at factory; installer never edits) ---
    char device_id[64];
    char device_key[128];      // Azure SAS primary key (blank if using DPS)
    char iothub_fqdn[96];      // defaults to DEFAULT_IOTHUB_FQDN
    char ap_ssid[33];          // per-device SoftAP name (e.g. "Gravity_water_01"); fallback AP_SSID_DEFAULT
    char ap_password[32];      // SoftAP password (shared "Config123" in this batch)

    // --- Network (set by installer) ---
    char wifi_ssid[33];
    char wifi_password[64];

    // --- Install / site record (set by installer at finish; reported to Azure for the master sheet) ---
    char site_name[64];        // site / school name
    char site_gps[40];         // "lat,lon" captured at install

    // --- Meters ---
    meter_cfg_t meters[MAX_METERS];

    // --- Telemetry / twin-tunable ---
    uint32_t telemetry_interval_s;
    uint8_t  modbus_retry_count;
    uint16_t modbus_retry_delay_ms;
    bool     maintenance_mode;

    // --- OTA ---
    bool ota_enabled;
} app_config_t;

// Load config from NVS into the global config (applies defaults for missing keys). Call once at boot.
void app_config_load(void);

// Persist the current global config to NVS.
void app_config_save(void);

// Accessor for the live config.
app_config_t *app_config_get(void);

// True if WiFi credentials are present (decides station vs AP mode at boot).
bool app_config_has_wifi(void);

// True if a device identity has been provisioned (device_id present, or DPS in use).
bool app_config_has_identity(void);

// Clear ONLY network creds (wifi ssid/password), KEEP identity. Used by the pre-ship "reset to ship" step.
void app_config_clear_network_keep_identity(void);

// Full factory reset: erase the entire "aquagen" NVS namespace (identity + network + meters).
void app_config_factory_reset(void);

#endif // APP_CONFIG_H
