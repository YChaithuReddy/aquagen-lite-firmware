# AquaGen Lite — ESP-IDF Firmware

ESP32 water-meter IoT gateway: reads ultrasonic flow meters over RS485/Modbus RTU and
reports consumption to Azure IoT Hub over MQTT. WiFi-only, 4 MB flash. Rewrite of the
legacy Arduino `viethnam_final_v10` firmware. Full spec: [`docs/SPEC.md`](docs/SPEC.md).

## Build
```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Status — incremental build

**Done (core, flashable):**
- Project scaffold: CMake, `partitions_4mb.csv` (OTA-only, frozen), `sdkconfig.defaults`
- `app_config` — NVS config (identity / network / per-meter), clear-network-keep-identity, factory reset
- `leds` — RED/GREEN/BLUE status state machine
- `wifi_mgr` — STA connect + SoftAP config mode + scan
- `modbus_meter` — RTU master @ **2400/8E1** (configurable), holding reg 0x0007, CDAB→int32 ×0.01
- `main` — boot state machine (factory-reset button, AP vs station), periodic meter read loop, watchdog

**Next (layering in):**
- `sas_token` + `azure_mqtt` — Azure IoT Hub MQTTS:8883, SAS auth
- `telemetry` + `flash_buffer` — JSON telemetry + LittleFS offline store-and-forward
- `provisioning` — **both** Azure DPS and bake-at-flash paths (test, then pick)
- `device_twin` — remote config / OTA trigger / interval / maintenance / reboot
- `ota` — GitHub OTA with redirect handling + version reporting + rollback
- `wireguard_client` — VPN for remote config/MQTT (binary-size permitting on 4 MB)
- `web_config` — REST API for the Flutter config app (no embedded HTML)

## Hardware
ESP32, RS485 on UART2 (RX=16, TX=17), trigger button GPIO34, LEDs R=26/G=25/B=27.
Meter: 2400 baud, even parity, 8E1.

## Companion
Flutter config app: `../aquagen_config_app` (QR-scan provisioning wizard).
