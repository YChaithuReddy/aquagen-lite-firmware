/**
 * AquaGen Lite — compile-time constants & hardware definitions.
 * Runtime-tunable values live in NVS via app_config.* (not here).
 */
#ifndef IOT_CONFIGS_H
#define IOT_CONFIGS_H

// ---- Firmware version (reported to Device Twin; gates OTA) ----
#define FW_VERSION_MAJOR 2
#define FW_VERSION_MINOR 0
#define FW_VERSION_PATCH 0
#define FW_VERSION_STRING "2.3.6"

// ---- Hardware pin map (keep identical to AquaGen board) ----
#define PIN_RS485_RX     16   // UART2 RX  (Modbus)
#define PIN_RS485_TX     17   // UART2 TX  (Modbus)
#define PIN_RS485_RTS    18   // DE/RE direction pin = GPIO18 (from modbus_iot_gateway, which reads
                              // this meter fine). The board's RS485 chip needs DE driven; with -1
                              // (auto) it never transmitted → 0-byte reads. IDF RS485 half-duplex
                              // mode auto-toggles this pin around each TX.
#define PIN_TRIGGER      34   // config button, ACTIVE-HIGH like the gateway: idle LOW (pull-down), press = to 3.3V
#define PIN_TRIGGER_ALT  0    // config button alt = GPIO0/BOOT, ACTIVE-LOW: idle HIGH (internal pull-up), press = to GND
#define PIN_LED_RED      26   // AP/config mode / error
#define PIN_LED_GREEN    25   // WiFi connected
#define PIN_LED_BLUE     27   // MQTT connected

#define MODBUS_UART_NUM      2

// ---- Modbus meter defaults (meter is 2400 baud / EVEN parity — confirmed by user) ----
#define DEFAULT_MODBUS_BAUD     2400
#define DEFAULT_MODBUS_PARITY   "even"   // "none" | "even" | "odd"
#define MODBUS_DATA_BITS        8
#define MODBUS_STOP_BITS        1
#define MODBUS_FLOW_REG         0x0007   // holding register start — the consumption total (matches the
                                         // working Vietnam firmware). Reg 8 read [0,3092] which isn't the
                                         // total; the earlier 0-byte issue was RS485 wiring, not the register.
#define MODBUS_FLOW_QTY         2        // registers
#define MODBUS_SCALE            0.01     // raw int32 * scale = consumption
#define MODBUS_READ_RETRIES     2
#define MODBUS_RETRY_DELAY_MS   100

#define MAX_METERS              3

// ---- Azure IoT Hub ----
#define DEFAULT_IOTHUB_FQDN     "fluxgen-testhub.azure-devices.net"  // overridable via NVS
#define MQTT_PORT               8883
#define SAS_TOKEN_DURATION_MIN  60

// ---- WireGuard remote-access VPN ----
// RE-ENABLED (2026-06-14): the earlier crashes (LoadProhibited in esp_wireguard_connect +
// main-task stack overflow) were caused by the main task stack being only 3584 bytes — WG
// setup (HTTPS register + crypto + netif) needs more. Bumped CONFIG_ESP_MAIN_TASK_STACK_SIZE
// to 8192 (matches the proven modbus_iot_gateway). esp_wireguard 0.9.0 is identical in both
// projects and the wg_start code is byte-for-byte the same, so with the bigger stack it works.
#define ENABLE_WIREGUARD 1

// ---- Azure DPS (Device Provisioning Service) — Option A (zero-touch) ----
// Fill these from the Azure portal DPS instance before flashing the DPS build.
#define DPS_GLOBAL_ENDPOINT     "global.azure-devices-provisioning.net"
#define DPS_ID_SCOPE            ""   // e.g. "0ne00ABCDEF" — REQUIRED for DPS path
#define DPS_GROUP_SYMMETRIC_KEY ""   // base64 group enrollment key — REQUIRED for DPS path
#define DPS_API_VERSION         "2019-03-31"

// ---- Telemetry ----
#define DEFAULT_TELEMETRY_INTERVAL_S  300   // 5 minutes (matches the old Vietnam firmware's 300000 ms)
#define TELEMETRY_INTERVAL_MIN_S      30
#define TELEMETRY_INTERVAL_MAX_S      3600

// ---- Reliability (ported from modbus_iot_gateway, tuned for the unattended fleet) ----
#define NTP_RESYNC_INTERVAL_S      (24 * 60 * 60)   // re-sync the clock daily; drift breaks SAS tokens
#define MQTT_RECOVERY_TIMEOUT_S    (30 * 60)        // MQTT down this long → self-reboot to recover
// Modbus self-heal: a wedged RS485/UART won't recover via per-read retries.
#define MODBUS_FAIL_REINIT         3   // consecutive all-failed read cycles → re-init the RS485 driver
#define MODBUS_REINIT_MAX_REBOOT   4   // driver re-inits without any good read → self-reboot (last resort)

// ---- SoftAP (config mode) ----
// NOTE: these defaults only apply to a board flashed WITHOUT a baked NVS identity.
// Provisioned boards override them from NVS (ap_ssid / ap_pw). Set to Gravity_water_01
// for quick bench testing (2026-06-13) so a plain flash comes up matching the 01 QR.
#define AP_SSID_DEFAULT         "Gravity_water_01"
#define AP_PASSWORD_FALLBACK    "Config123"
#define AP_SESSION_TIMEOUT_MS   (30 * 60 * 1000)   // 30 minutes (the old firmware's 8.3h was a bug)

// Runtime config mode (opened by the trigger button while telemetry runs): auto-close the SoftAP +
// web server after this long with NO client connected, so a walked-away operator doesn't leave the
// hotspot + web server up forever (heap + open-AP risk). Resets whenever a phone is connected.
#define CONFIG_MODE_IDLE_TIMEOUT_MS   (10 * 60 * 1000)   // 10 minutes idle → back to clean station

// Low-heap safety net. If free heap stays below CRITICAL for HEAP_LOW_STREAK consecutive loop checks
// (~every 300 ms), self-heal: first close the config web server (the biggest non-essential consumer);
// if it is still critically low, reboot — the same self-recovery philosophy as MQTT auto-recovery.
#define HEAP_CRITICAL_BYTES     (15 * 1024)
#define HEAP_LOW_STREAK         20

// ---- NTP ----
#define NTP_SERVER_1            "pool.ntp.org"
#define NTP_SERVER_2            "time.nist.gov"

// ---- Factory reset ----
#define FACTORY_RESET_HOLD_MS   10000   // long-press trigger pin this long → wipe all NVS

#endif // IOT_CONFIGS_H
