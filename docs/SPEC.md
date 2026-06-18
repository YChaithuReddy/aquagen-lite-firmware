# AquaGen Lite — Flow Meter IoT Gateway
## Functional & Technical Specification (derived from `viethnam_final_v10` Arduino firmware)
### Purpose: rebuild this firmware from scratch in ESP-IDF

> This document is reverse-engineered from the existing Arduino firmware
> (`viethnam_final_v10`, ~1,653 lines). It captures **exactly what the device does today**
> so the ESP-IDF rewrite reaches functional parity, then improves on it.
> Version of source analyzed: header says `2025-04-21`, firmware version string `1.0.0`.

---

## 1. Product Overview

AquaGen Lite is an **ESP32-based water-metering IoT gateway**. It:

1. Reads cumulative flow (consumption) from up to **3 ultrasonic water meters** over **RS485 / Modbus RTU**.
2. Connects to **Azure IoT Hub** over **MQTT + TLS** using a per-device **SAS token**.
3. Publishes **consumption telemetry** as JSON at a configurable interval.
4. Provides a **WiFi Access-Point + web form** for field configuration (WiFi creds, Azure device identity, meter Modbus settings).
5. Persists all configuration in **NVS flash** (survives reboot / power loss).
6. Supports **OTA firmware update** triggered by an Azure cloud-to-device (C2D) message.
7. Shows status via **3 LEDs** (AP/error, WiFi, MQTT).

Deployed at schools, tanks, and borewells. Powered by 230 V AC. Configured on site via a phone connecting to the device's hotspot, then viewed on the `web.aquagen.co.in` dashboard.

---

## 2. Hardware Definition

### MCU
- ESP32 (single, dual-core). ~150 KB usable heap budget assumed for the rewrite.

### Pin Map (from firmware `#define`s — keep identical for hardware compatibility)
| Function | GPIO | Notes |
|---|---|---|
| RS485 / Modbus RX (Serial2 RX) | **16** (`RXD2`) | UART2 |
| RS485 / Modbus TX (Serial2 TX) | **17** (`TXD2`) | UART2 |
| Config-mode trigger (push switch) | **34** (`TRIGGER_PIN`) | `INPUT_PULLUP`; LOW at boot → AP/config mode |
| RED LED | **26** | AP mode / error |
| GREEN LED | **25** | WiFi connected |
| BLUE LED | **27** | MQTT connected |

> Note: the SOP describes a 4th red LED as a "power indicator" — that is a hardware power rail LED, not GPIO-driven.

### RS485 Serial Settings
- ⚠️ **CORRECTION (2026-06-13):** the actual meter runs at **2400 baud, EVEN parity → `8E1`**. The old Arduino firmware is hardcoded to **9600 8N1**, which is WRONG for this meter (wrong baud + parity = no Modbus response). **ESP-IDF default = 2400 / 8 data / Even / 1 stop**, and **baud + parity MUST be configurable** (per-meter, via NVS/web/Twin) so future meters don't need a reflash.
- No explicit DE/RE direction-control GPIO in this firmware (the `ModbusMaster` lib + transceiver auto-direction). **For ESP-IDF, confirm whether the RS485 transceiver needs an RTS/DE pin** — ESP-IDF's native `uart_set_mode(UART_MODE_RS485_HALF_DUPLEX)` typically wants one. (The advanced gateway uses RTS=18.)

### Meter Wiring (from SOP, RS485 mode)
- A = Yellow, B = Green, GND = Black, Power = Red (DC 7.5–24 V).
- A check valve must be installed before the meter; meter has its own 6-yr lithium battery.

---

## 3. Boot Sequence & State Machine

```
Power on
  │
  ├─ Serial @115200, init LEDs, init Serial2 (Modbus)
  ├─ Read TRIGGER_PIN (GPIO34)
  │
  ├─ setupConfiguration(forceAP): load all config from NVS ("flowmeter" namespace)
  │
  ├─ Decide mode:
  │    forceAP == true (GPIO34 LOW at boot)   ──┐
  │    OR ssid empty OR password empty        ──┤──► AP / CONFIG MODE
  │    else                                   ──► STATION MODE
  │
  ├─ AP / CONFIG MODE:
  │    RED LED on; start SoftAP "FluxgenConnect"/"VTNMCredentials"; start web server on :80
  │    loop(): handle web clients; auto-reboot after AP_SESSION_TIMEOUT
  │
  └─ STATION MODE:
       connectToWiFi() (up to 180 s)
         success → GREEN on → NTP time sync → init Azure client → init MQTT → init watchdog → loop()
         fail    → RED on → ESP.restart()
```

### loop() — Station Mode duties (every ~300 ms tick)
1. If WiFi dropped → cleanup MQTT, reconnect WiFi, re-init MQTT.
2. If SAS token expired → cleanup + re-init MQTT (regenerates token).
3. If `millis() > next_telemetry_send_time_ms`:
   - For each of the 3 configured slaves: read Modbus → if OK and MQTT up, publish telemetry.
   - Schedule next send at `now + telemetryFrequencyMillis`.
4. If OTA pending → run OTA.
5. Reset task watchdog.

### loop() — AP Mode duties
- Service web server clients.
- Auto-reboot after `AP_SESSION_TIMEOUT`.

> 🐞 **Bug to fix in rewrite:** `AP_SESSION_TIMEOUT = 30000000` ms = **8.3 hours**, but the comment says "30 minutes." Intended value is `1800000` (30 min). Decide the real requirement.

---

## 4. Configuration System

### 4.1 Storage
- NVS (ESP-IDF `nvs_flash`) namespace: **`flowmeter`**.
- All values stored as strings except telemetry frequency (`unsigned long`).

### 4.2 Configurable Fields (full list — keys are the NVS keys)
| NVS key | Meaning | Default |
|---|---|---|
| `ssid` | WiFi SSID | "" |
| `password` | WiFi password | "" |
| `deviceId` | Azure IoT Hub device ID | "" |
| `deviceKey` | Azure device primary key (SAS) | "" |
| `slaveId` / `slaveId2` / `slaveId3` | Modbus slave addresses for meters 1/2/3 | "1"/"2"/"3" |
| `unitId` / `unitId2` / `unitId3` | Logical unit IDs sent in telemetry | "Unit_0"/"Unit_1"/"Unit_2" |
| `serial` / `serial2` / `serial3` | Meter serial numbers | "SN1"/"SN2"/"SN3" |
| `telemetryFreq` | Telemetry interval (ms) | 60000 |

> The Azure IoT Hub **FQDN is hardcoded**: `fluxgen-testhub.azure-devices.net`. In the rewrite, make this configurable (NVS) or build-time, not hardcoded in two places (it appears both as a `#define` and is referenced separately).

### 4.3 AP Mode Web Server (port 80)
SoftAP SSID **`FluxgenConnect`**, password **`VTNMCredentials`** (⚠ identical on every device, public in the SOP — see §9 security).

Endpoints currently implemented:
- `GET /` → HTML config form (all fields above).
- `POST /api/htmlform` → save all fields from the form, persist to NVS.
- `GET /api/<field>` → returns the current value as JSON (one per field).
- `POST /api/<field>` → update a single field (JSON body), persist.
- `GET /api/telemetryFreq` / `POST /api/telemetryFreq` → telemetry interval.
- `GET /api/scan` → scan WiFi networks → JSON `{networks:[{ssid,rssi,encryption}]}`.
- `POST /api/saveAndReboot` → persist + `ESP.restart()`.
- `onNotFound` → 404 JSON.

> The form is bare HTML. The companion "FluxgenConnect App" (mobile) talks to these REST endpoints. In the ESP-IDF rewrite, keep the **same REST contract** so the existing mobile app keeps working (or version it deliberately).

---

## 5. Network & Time

### WiFi (Station)
- `WiFi.mode(WIFI_STA)`, connect with stored creds, 180 s timeout.
- On success: disable WiFi power save (`esp_wifi_set_ps(WIFI_PS_NONE)`).
- Only **2.4 GHz** supported (per SOP).

### NTP Time Sync
- Servers: `pool.ntp.org`, `time.nist.gov`.
- Timezone constants currently set to **PST (-8, +1 DST)** — but telemetry timestamps are emitted in **UTC** (`%Y-%m-%dT%H:%M:%SZ`). Recommend UTC everywhere in the rewrite.
- Sanity gate: waits until clock > 2017-11-13 (so SAS tokens have valid expiry). 20 retries × 1 s.

---

## 6. Azure IoT Hub Connection

### Identity & Auth
- Azure SDK for C (`az_iot_hub_client`).
- **SAS token auth**: HMAC-SHA256 over the resource URI + expiry, signed with the device key, base64-encoded. Helper class `AzIoTSasToken`.
- Token lifetime: **60 minutes** (`SAS_TOKEN_DURATION_IN_MINUTES`). Regenerated automatically when `IsExpired()`.

### MQTT
- ESP-IDF MQTT client (`esp-mqtt`), MQTTS, **port 8883**.
- Broker URI: `mqtts://fluxgen-testhub.azure-devices.net`.
- `client_id` and `username` derived from Azure SDK; `password` = SAS token.
- TLS root CA: **Baltimore CyberTrust Root** (embedded in `ca.h` as `ca_pem`). Note Azure has been migrating roots to DigiCert Global G2 — the rewrite should bundle the **current** Azure root(s) or use `esp_crt_bundle`.
- Keepalive 30 s; auto-reconnect on; clean session on.
- On **5+ disconnects** → `ESP.restart()`.

### Topics
- Telemetry publish topic: from `az_iot_hub_client_telemetry_get_publish_topic()` (i.e. `devices/{deviceId}/messages/events/`).
- C2D subscribe topic: `devices/{deviceId}/messages/devicebound/#`, QoS 1.

---

## 7. Modbus Reading (core measurement logic)

Function: `getPandBulkTotalFlow(slave_id, index)` — reads one meter ("Panda/PandBulk" ultrasonic meter).

Per read:
1. Flush Serial2 RX buffer.
2. `node.begin(slave_id, Serial2)`; 50 ms settle.
3. `readHoldingRegisters(0x0007, 2)` — **2 registers starting at 0x0007**.
4. On success, combine the two 16-bit registers into a 32-bit value using **CDAB (mid-little-endian) byte order**:
   - reg0 = CD, reg1 = AB → rawValue = A<<24 | B<<16 | C<<8 | D
   - reinterpret as signed int32, multiply by **0.01** → consumption value (double).
5. Retries: **2 attempts**, 100 ms apart. On failure → value = NaN, returns 0.

> 🐞 The docstring says "register 0x0008, 4 registers" but the code reads **0x0007, qty 2**. Trust the code. Document the real register map from the meter datasheet in the rewrite.
>
> 💡 **Enhancement opportunity:** the meter also exposes status flags (battery-low, empty-pipe, reverse-flow, over-range — per SOP §6 fault table). The current firmware ignores them. The ESP-IDF version should read and report these.

---

## 8. Telemetry Payload

Built per meter, published individually.

```json
{
  "consumption": 1234.56,
  "created_on": "2026-06-13T08:00:00Z",
  "type": "FLOW",
  "unit_id": "TFG23316F"
}
```

- `consumption`: parsed value (0 if NaN).
- `created_on`: UTC ISO-8601.
- `type`: constant `"FLOW"`.
- `unit_id`: the configured `unitIdN` for that meter.
- (Note: the meter **serial number** is passed around but **not** currently included in the payload — consider adding it so the backend can disambiguate.)

Published QoS 1, not retained.

> 💡 No offline buffering today: if WiFi/MQTT is down at send time, the reading is **discarded**. The advanced gateway solves this with SD-card store-and-forward; the rewrite should adopt the same.

---

## 9. OTA Update

- Triggered by a C2D JSON message: `{"type":"ota_update","version":"x.y.z","url":"https://..."}`.
- If `version != FIRMWARE_VERSION`, downloads from `url` via HTTP(S) and flashes using `Update` API; restarts on success.
- ⚠️ **No signature/hash verification, no version-downgrade guard, follows redirects from an arbitrary URL.** The rewrite must add signed/verified OTA (the advanced gateway has hardened OTA logic to copy).

---

## 10. Watchdog & Status LEDs

### Watchdog
- ESP32 Task WDT, `WATCHDOG_TIMEOUT = 100` (seconds — very loose). Enabled **only in station mode**. Reset each loop. Choose a sane timeout (e.g. 30 s) in the rewrite and feed from every long-running task.

### LED semantics (keep identical for field techs)
| State | RED (26) | GREEN (25) | BLUE (27) |
|---|---|---|---|
| AP / config mode | ON | off | off |
| WiFi connected | off | ON | — |
| MQTT connected | — | — | ON |
| Error / failed read | blink | — | — |
(Maps to SOP LED table: Red=config, Green=WiFi, Blue=internet/MQTT.)

---

## 11. Known Bugs & Risks (carry into the rewrite as "do not repeat")

1. **AP timeout 8.3 h vs documented 30 min** (value/comment mismatch).
2. **No offline buffering** → data loss during outages.
3. **Web config has no auth; `GET /api/deviceKey` returns the Azure key in plaintext**; AP password is shared across all devices and printed in the SOP.
4. **OTA unsigned / unverified** → remote-code-execution risk.
5. **Secrets printed to serial** at boot (SSID/password/device key).
6. **Hardcoded WiFi creds in `iot_configs.h`** (`Fluxgen bhive` / `AnandG@123`) — legacy file, remove.
7. **Hardcoded Hub FQDN** in two places.
8. **Stale comments** throughout (register address, byte order, timeouts).
9. **Meter health flags ignored.**
10. **No diagnostics telemetry** (RSSI, uptime, heap, firmware version, battery).
11. **Baltimore root CA may be deprecated** by Azure — verify current root.

---

## 12. Functional Requirements for the ESP-IDF Rewrite (parity checklist)

The ESP-IDF version MUST, at minimum, reproduce:

- [ ] ESP32, same GPIO map, RS485 @ 9600 8N1, 3-meter support.
- [ ] NVS config (namespace `flowmeter`) with all fields in §4.2.
- [ ] Boot state machine: GPIO34/empty-creds → AP+web config; else station.
- [ ] SoftAP + web server with the **same REST endpoint contract** (§4.3) so the existing mobile app works.
- [ ] WiFi station connect w/ retry + power-save off.
- [ ] SNTP time sync (UTC), pre-SAS sanity gate.
- [ ] Azure IoT Hub MQTTS:8883, SAS token (HMAC-SHA256, 60-min, auto-renew), correct root CA.
- [ ] Modbus read: register `0x0007` qty 2, CDAB→int32 ×0.01, retries.
- [ ] Telemetry JSON schema in §8, per-meter, QoS1, at `telemetryFreq`.
- [ ] C2D subscribe + OTA trigger.
- [ ] Task watchdog + LED status semantics.

## 13. Target Improvements (the "next level" — build these into the ESP-IDF version)

- [ ] **Store-and-forward**: AquaGen has **no SD card** → use a **dedicated flash partition (LittleFS) ring buffer** for offline cache + chronological replay. No lost readings. NOTE: flash capacity is far smaller than SD — size the partition for worst-case offline duration (e.g. at 1 reading/min × 3 meters, a ~512 KB–1 MB data partition holds days, not weeks). Overwrite-oldest policy when full; replay in chronological order on reconnect; mark-and-delete after successful publish. Mind flash wear (LittleFS wear-levels; keep write rate sane).
- [ ] **Meter health telemetry**: battery-low, empty-pipe, reverse-flow, over-range alarms.
- [ ] **Device diagnostics**: RSSI, uptime, free heap, firmware version, boot reason, per-meter last-read status.
- [ ] **Azure Device Twin**: remote config (telemetry interval, slave IDs), remote reboot, maintenance mode, OTA-by-twin.
- [ ] **Signed/verified OTA** with version-downgrade protection.
- [ ] **Security**: per-device AP password, auth on config endpoints, never expose device key over GET, stop printing secrets, current Azure root CA / `esp_crt_bundle`.
- [ ] **Configurable Hub FQDN** (no hardcoding).
- [ ] **Optional connectivity**: 4G/SIM fallback (A7670C) and/or remote access (WireGuard) — already proven in the sibling `modbus_iot_gateway` project; reuse those modules.
- [ ] **Cleaner web UI**: structured config app (WiFi scan w/ signal bars, validation, Modbus explorer/test-read).
- [ ] **Accurate RTC** (DS3231) so offline timestamps are correct without NTP.

---

## 14. Suggested ESP-IDF Project Structure

```
aquagen_lite_idf/
├── CMakeLists.txt
├── sdkconfig.defaults          # MQTT TLS, NVS, FATFS, task WDT
├── partitions.csv              # factory + ota_0 + ota_1 + nvs (size generously NOW — see gateway's frozen-partition lesson)
├── main/
│   ├── main.c                  # app entry, state machine, task orchestration
│   ├── app_config.c/.h         # NVS load/save (namespace "flowmeter")
│   ├── wifi_mgr.c/.h           # STA connect + SoftAP
│   ├── web_config.c/.h         # esp_http_server, REST endpoints (keep app contract)
│   ├── time_sync.c/.h          # SNTP
│   ├── azure_mqtt.c/.h         # esp-mqtt + SAS token (az SDK or hand-rolled HMAC)
│   ├── sas_token.c/.h          # HMAC-SHA256 SAS generation/renew
│   ├── modbus_meter.c/.h       # RS485 read + CDAB parse + retries + health flags
│   ├── telemetry.c/.h          # JSON build + publish + offline queue
│   ├── store_forward.c/.h      # SD / flash ring buffer + replay   (NEW)
│   ├── ota.c/.h                # signed OTA via C2D / twin
│   ├── leds.c/.h               # status LED state machine
│   └── diagnostics.c/.h        # RSSI/uptime/heap/version telemetry (NEW)
└── docs/  (this spec + meter register map + Azure setup)
```

> **Partition lesson from the sibling gateway:** once devices ship, the partition table is effectively frozen (OTA can't change it). Size NVS + OTA slots generously from day one.

---

---

# PART B — Features to PORT from `modbus_iot_gateway` v1.4.0

> The user requires these four capabilities in the new AquaGen ESP-IDF firmware.
> Specs below are extracted from the **proven** gateway implementation so we reuse
> the exact contract (twin property names, NVS keys, endpoints) rather than reinventing.
> Source: `/Users/admin/Python Code/modbus_iot_gateway/main/`.

## 15. Azure Device Twin (remote config / OTA / interval / maintenance / reboot)

Gateway handlers: `handle_device_twin_desired_properties()`, `report_device_twin_properties()` (in `main.c`).

### MQTT topics
- Subscribe desired: `$iothub/twin/PATCH/properties/desired/#`
- Publish reported: `$iothub/twin/PATCH/properties/reported/?$rid={request_id}`
- Twin GET response: `$iothub/twin/res/#`

### DESIRED properties the device reads (apply only if `$version` changed)
| Property | Type | Action |
|---|---|---|
| `telemetry_interval` | int (30–3600 s) | change send interval |
| `maintenance_mode` | bool | pause all telemetry when true |
| `reboot_device` | bool (one-shot) | report `reboot_status:"rebooting"`, wait 3 s, `esp_restart()` |
| `ota_enabled` | bool | allow/block OTA |
| `ota_url` | string (<256) | if `ota_enabled` & URL present → `ota_start_update(url,"remote")` after 40 KB heap check |
| `web_server_enabled` | bool | start/stop config web server remotely |
| `modbus_retry_count` | int (0–3) | Modbus retries |
| `modbus_retry_delay` | int (10–500 ms) | Modbus retry delay |
| `batch_telemetry` | bool | batch vs per-reading send |
| `sensors[]` | array (≤10) | full per-sensor remote config (slave_id, register, data_type, scale, byte_order, unit_id, etc.) |

### REPORTED properties the device writes back
`firmware_version`, `device_id`, `telemetry_interval`, `modbus_retry_count`, `modbus_retry_delay`, `batch_telemetry`, `sensor_count`, `network_mode` (WiFi/SIM), `web_server_enabled`, `maintenance_mode`, `ota_enabled`, `ota_url`, `ota_status` (idle/downloading/success/failed), `last_boot_time`, `uptime_sec`, `free_heap`, `sensors[]` summary.

> For AquaGen (3 fixed meters) we can keep the `sensors[]` model OR simplify to slave1/2/3 — decide at scaffold time. Recommend keeping the array model for forward-compat with the gateway.

## 16. OTA from GitHub (redirect handling + version reporting)

Gateway module: `ota_update.c/.h`. Entry: `ota_start_update(url, version)` → background `ota_download_task`.

Flow to reproduce:
1. URL source: Device Twin `ota_url`, or web `/api/ota/start` (URL) / `/api/ota/upload` (multipart).
2. **Stop MQTT before OTA** (`mqtt_stop_for_ota()`) to free ~30 KB heap / single TLS session. Require ≥40 KB free heap.
3. **Manual redirect loop** (GitHub release → CDN), follows 301/302/303/307/308, captures `Location` header in `http_event_handler`. Up to MAX_REDIRECTS.
4. **Cert handling:** GitHub URLs → skip cert bundle (CDN cert issues); Azure/other → `esp_crt_bundle_attach`.
5. HTTP buffers: RX 4096 / TX 1024 / download 4096.
6. Write: `esp_ota_get_next_update_partition` → `esp_ota_begin` → loop `esp_ota_write` → `esp_ota_end`.
7. **Version reporting** to twin via `ota_status_callback` → `ota_status/ota_message/ota_progress/ota_bytes_downloaded/ota_total_bytes`.
8. **Rollback safety:** NVS `boot_cnt` counter; firmware must call `ota_mark_valid()` (`esp_ota_mark_app_valid_cancel_rollback`) after a healthy boot, else auto-rollback after 3 bad boots.
   > ⚠️ No explicit downgrade guard today — ADD a version-compare check in the rewrite (don't flash same/older unless forced).

Key fns: `ota_start_update`, `ota_download_task`, `http_event_handler`, `ota_mark_valid`, `ota_is_rollback`, `ota_is_in_progress`, `ota_set_status_callback`.

## 17. WireGuard VPN client (remote web config + MQTT, P2P mesh, self-healing)

Gateway module: `wireguard_client.c/.h`. Pulls `trombik/esp_wireguard >=0.9.0`.

### Endpoints
- Register HTTPS: `https://iiot.aquagen.co.in/wg/api/agent/register`
- Register HTTP fallback: `http://20.197.19.49:3200/wg/api/agent/register`
- Peer coordination (over tunnel): `http://10.100.0.1:3200/wg/api/coord/peers`, `/wg/api/coord/endpoints`

### NVS (namespace `wg_id`)
`privkey`, `pubkey` (Curve25519, base64), `local_ip` (e.g. `10.100.0.42`), `psk`, `server_pub`.

### Boot ordering (respects "one TLS at a time")
Network up → time sync → `wireguard_setup()` (blocking, registers/loads identity) → `wireguard_start_keepalive_task()` → **then** start MQTT. WG HTTPS register finishes before Azure MQTT TLS opens.

### Self-healing
- No NVS identity → generate keys → register → save.
- Server rejects (4xx → `REG_REJECTED`) → wipe NVS → regen → poll every 5 s → `esp_restart()` once re-registered (or deferred if web server up).
- Tunnel down → keepalive 10 s tick reconnects (no restart). Server identity re-verify every 20 s.

### Status
- `/vpn-status` GET → JSON `{device, vpn_ip, tunnel, registered, server, pubkey, free_heap_kb}` (registered lazily by keepalive task).
- IP range `10.100.0.0/24`, server-assigned via registration response `allowed_ip`.
- Keepalive task: 5120-byte stack, priority 4.

Key fns: `wireguard_setup`, `wireguard_keepalive_task`, `register_with_panel`, `nvs_load_identity`, `nvs_save_identity`, `generate_wg_keys`, `wireguard_register_http_handlers`.

> Budget: ~50 KB flash, ~15–20 KB resident heap. Account for this in the (generous) partition plan.

## 18. New Configuration UI App (replaces the bare HTML form)

The user wants a **proper config UI app** (like the previously-built mobile app), not embedded HTML. Plan:

- **Keep the device as a clean REST API** over the SoftAP (and over WireGuard `10.100.0.X` for remote). Don't embed a giant HTML/JS blob in C (the gateway's 11,853-line `web_config.c` is its biggest maintenance liability — do NOT repeat that).
- Target the **gateway's proven endpoint contract** so one app can manage both products. Endpoints to support (subset relevant to AquaGen):

  Config saves: `/save_wifi_config`, `/save_azure_config`, `/save_system_config`, `/save_network_mode`, (`/save_sim_config`, `/save_sd_config`, `/save_rtc_config` if those peripherals are present).
  Sensors/meters: `/edit_sensor`, `/save_single_sensor`, `/delete_sensor`, `/test_sensor`, `/test_rs485`.
  Live/diagnostics: `/scan_wifi`, `/live_data`, `/api/system_status`, `/api/modbus_poll`, `/api/modbus/status`, `/api/azure/status`.
  Modbus tools: `/modbus_scan`, `/modbus_read_live`, `/write_single_register`, `/write_multiple_registers`.
  OTA: `/api/ota/status`, `/api/ota/start`, `/api/ota/upload`, `/api/ota/cancel`, `/api/ota/confirm`, `/api/ota/reboot`.
  SD/RTC: `/api/sd_status`, `/api/sd_clear`, `/api/sd_replay`, `/api/rtc_time`, `/api/rtc_sync`, `/api/rtc_set`.
  System: `/reboot`, `/watchdog_control`, `/api/system_status`. All POST return JSON; include CORS `OPTIONS`.
- **App stack options** (to decide): Flutter (matches user's existing apps) talking to the device REST API; provisioning via QR; live signal bars from `/scan_wifi`; a Modbus Explorer screen using `/api/modbus_poll`.
- **Decision needed:** locate the OLD config app repo so the new one reuses its auth/branding and we lock a single API contract across firmware + app.

## 19. Updated ESP-IDF module list (with Part B)

**Hardware decisions locked (2026-06-13): WiFi-only (NO A7670C/SIM), NO SD card, Flutter config app.**

Add to the §14 structure:
```
main/
  ├── device_twin.c/.h        # desired/reported handlers (§15)
  ├── ota.c/.h                # GitHub OTA + redirects + rollback (§16)
  ├── wireguard_client.c/.h   # VPN + self-heal (§17)  [reuse gateway file ~verbatim]
  ├── flash_buffer.c/.h       # store-and-forward via LittleFS ring buffer (NO SD — see §13)
  └── ds3231_rtc.c/.h         # OPTIONAL — only if AquaGen board has a DS3231; else rely on NTP
```
**Dropped (not on AquaGen hardware):** `a7670c_ppp.c` / `a7670c_http.c` (no SIM), `sd_card_logger.c` (no SD → replaced by `flash_buffer.c`).

`idf_component.yml` must pull `trombik/esp_wireguard >=0.9.0` + a LittleFS component (`joltwallet/littlefs`). `sdkconfig.defaults`: `CONFIG_WIREGUARD_ESP_NETIF=y`, `CONFIG_WIREGUARD_MAX_PEERS=4`, keep SoftAP support enabled.

### Partition plan — 4 MB flash (CONFIRMED), OTA-only, size NOW + freeze forever
```
nvs       : 0x9000,  36 KB    (config + wg_id + boot_cnt)
otadata   : 8 KB
phy_init  : 4 KB
ota_0     : 1.85 MB           (first flash lands here — NO factory partition on 4 MB)
ota_1     : 1.85 MB           (OTA target slot)
littlefs  : 192–256 KB        (offline telemetry ring buffer — small, since 4 MB + no SD)
```
**⚠️ 4 MB is tight — this is the one real risk.** Dual-slot OTA on 4 MB leaves ~1.85 MB per app slot. The full feature set (Azure IoT C SDK + mbedTLS/TLS + esp_wireguard + LittleFS + MQTT + HTTP REST server) must fit in that. It's **plausible only because the config UI is in the Flutter app, not embedded HTML in firmware** — the gateway's 11,853-line `web_config.c` blob is exactly what we're NOT carrying, which buys the headroom. Mitigations if the binary overflows 1.85 MB:
- Trim mbedTLS cipher suites to those Azure needs.
- Use `esp_crt_bundle` selectively / single root CA.
- Disable unused ESP-IDF components in `sdkconfig`.
- Last resort: make WireGuard a build-time opt-in, or move the offline buffer to NVS (tiny) and shrink littlefs.
**Action: measure app binary size after the core builds; decide WireGuard inclusion against remaining flash before committing the frozen partition table.**

### Config App (Flutter) — NEW build, optimized for non-technical field installers
Goal (user's words): IoT boards ship to many sites; **different, non-technical people** configure the flow meter via this app. So: **simple, fast, clear, foolproof.**

- **Greenfield Flutter app** (not a port of the old one — fresh, lean APK).
- **Guided wizard UX**, not a settings dump:
  1. "Connect to device" — detect/join the `FluxgenConnect` SoftAP (show a single Connect button + status).
  2. "Pick WiFi" — live scan list with signal bars (`/scan_wifi`), tap to select, enter password.
  3. "Meter details" — Device ID + the 3 meters' serial/slave IDs, pre-fillable / QR-scannable.
  4. "Test" — one-tap live read (`/test_sensor` / `/api/modbus_poll`) showing each meter's value so the installer SEES it working before leaving site.
  5. "Save & finish" — `/save_*` + reboot; big green "Configured ✓" confirmation.
- **Clear status at all times**: connection state, last reading, success/failure in plain language (no raw JSON, no jargon).
- Talks to the device REST API over SoftAP (provisioning) and over WireGuard `10.100.0.x` (remote support).
- Uses the gateway endpoint contract in §18 → one app can serve both products.
- Branding: FluxGen / AquaGen. (Old app repo optional — building fresh.)

### Visual design direction — modern, premium, water-themed
Not generic Material defaults ("AI slop") — a deliberate, clean system:
- **Material 3 (Material You)** base, Flutter `useMaterial3: true`, dynamic color seeded from an **aqua/teal brand color** (e.g. seed `#0EA5A4` teal → tonal palette). Cohesive light **and** dark theme.
- **Water/flow motif:** soft gradient headers (teal→cyan), subtle animated wave or flow accent on the home/test screen, droplet/flow iconography (rounded, friendly — not industrial).
- **Cards over forms:** each wizard step is a rounded `Card` (16–20px radius), generous whitespace, one clear primary action per screen (large filled button, full-width, easy to tap in the field).
- **Step progress indicator** at top (1→5 dots/bar) so installers always know where they are.
- **Micro-interactions:** smooth page transitions between steps, success check animation (Lottie or `AnimatedSwitcher`) on "Configured ✓", live pulsing dot for connection status, animated signal bars on WiFi scan.
- **Live test screen is the hero:** big numeric readout per meter (animated count-up), color-coded status chip (green ok / amber warn / red fail), unit label — makes "it's working" instantly obvious.
- **Typography:** one modern geometric/humanist sans (e.g. Inter or Manrope via `google_fonts`), strong size hierarchy, high contrast for outdoor/sunlight readability.
- **Accessibility/field-friendly:** large tap targets (≥48dp), high-contrast mode, works one-handed, minimal text input (prefer pickers/scan/toggles).
- Packages: `google_fonts`, `lottie` (or `rive`), `flutter_animate` for tasteful motion, `mobile_scanner` for QR.
- Reference the latest Material 3 guidance at build time; avoid overused purple-gradient-on-white clichés — lean into the teal/aqua identity.

> **Hard rule inherited from the gateway:** plan the **partition table generously on day one** (NVS + 2 OTA slots + factory) and then freeze it — OTA cannot change partitions on deployed devices.

---

---

# PART C — Production, Provisioning & Field Workflow (DECIDED)

> Decisions locked with the user on 2026-06-13 for the first batch of **150 boxes**
> (not yet shipped — will be flashed with this new firmware, then sent out).
> These are committed requirements, not open questions.

## 20. Meter & Modbus settings (corrected)
- **Serial: 2400 baud, 8 data bits, EVEN parity, 1 stop (`8E1`).** This is the real meter setting; the old firmware's 9600/8N1 was wrong.
- baud + parity + slave ID + register + scale are **per-meter configurable** (NVS / web / Twin) — no reflash to support a different meter later.
- Read logic (from Part A §7): holding register `0x0007`, qty 2, CDAB→int32, ×0.01. Alarm/health registers (battery, empty-pipe, reverse, over-range) **stubbed until the meter datasheet/register map arrives**.

## 21. Device identity provisioning (150 boxes, no manual typing)

**How it was done before:** manually — Device ID + key typed into the FluxgenConnect app per box. Slow, error-prone. **We are NOT doing this.**

Two automated paths (final pick to confirm at build start):

### Option A — Azure DPS (recommended for least office work)
- Flash **identical firmware** to all 150 (no per-box IDs).
- Each box **self-registers** to Azure IoT Hub on first connect via **DPS group enrollment**, using its chip-unique ID (eFuse MAC / serial) as the registration ID; Azure auto-creates the device identity.
- No CSV of 150 secret keys to manage; no per-box flashing differences.
- Firmware needs a **DPS registration module** (ID Scope + group key embedded; derive per-device key from group key + registration ID).
- Trade-off: a shared group key lives in firmware (acceptable now; move to per-device X.509 certs later for higher security).

### Option B — Bake-at-flash (use if IDs must match your own serial scheme)
- A batch tool holds a **CSV of 150 {serial, device_id, device_key, ap_password}**.
- For each box: flash firmware **+ a generated per-device NVS image** (identity) in one ~30 s step, auto-incrementing through the CSV; same tool prints that box's QR label.
- Fully automated assembly line, no typing; you control the Device IDs.

> Either way: **flashing is automated, minutes for the batch.** A **QR sticker** still goes on every box (encodes AP SSID + AP password [+ Device ID in Option B]) so the field app joins + identifies the box without typing.

## 22. Pre-ship workflow (per box, at the office, AFTER flashing, BEFORE shipping)
1. **Provision identity** — Option A: just flash (self-registers later); Option B: flash firmware + per-box NVS identity. Register/confirm in Azure.
2. **Label** — print + apply QR sticker; log `serial ↔ Device ID` in the master sheet.
3. **Bench QA test** (catch dead units before they travel):
   - Power on → verify boot LED pattern.
   - Box joins office test WiFi.
   - Confirm telemetry reaches Azure (proves identity + cloud path).
   - Connect a real meter **or a Modbus simulator @ 2400/8E1** → confirm a valid reading.
4. **Reset to ship state** — clear the office test WiFi **but KEEP the Azure identity**, so the box ships in **config/hotspot mode** (Red+Blue LED, `FluxgenConnect` AP up) ready for the field installer. → firmware needs a **`clear_network_keep_identity()`** function (distinct from full factory reset).
5. **Finalize** — mark QA ✅ in master sheet; pack with QR sticker visible.

## 23. Field install (per site, ~2 min, non-technical installer)
1. Mount meter, wire RS485 (2400/8E1), power 230 V AC.
2. App → scan box QR → auto-join hotspot (Option B also gets Device ID from QR).
3. Pick site WiFi + password; confirm 3 meter serials/slave IDs.
4. One-tap **TEST** → live readings shown (proof before leaving).
5. App captures **GPS + photo + site/school name** → pushes to backend.
6. Save → reboot → connects to Azure → live. ✅

## 24. Factory reset (recovery)
Long-press the config button (≈10 s) → **wipe ALL NVS (identity + network)** → fresh AP mode. Distinct from §22.4's network-only clear. Document in the SOP.

## 25. Build plan & order
**3 deliverables:**
1. **ESP-IDF firmware** — core (config/NVS, WiFi/SoftAP, Modbus @2400/8E1, Azure MQTT/SAS) → DPS provisioning → Device Twin (§15) → GitHub OTA (§16) → WireGuard (§17) → flash ring-buffer store-and-forward (§13) → `clear_network_keep_identity` + factory reset. Measure binary vs 1.85 MB slot before freezing partitions; WireGuard is the trim candidate if tight.
2. **Office batch tool** — bulk-create/enroll 150 Azure identities (DPS group enrollment or CSV) + generate 150 QR labels + master sheet. One-click.
3. **Flutter config app** — QR scan → 5-step wizard → live test → GPS/photo/site capture → modern Material 3 aqua UI (§18). Talks to device REST API over SoftAP + WireGuard.

**Outstanding inputs (don't block firmware core):** meter datasheet (alarm registers), `web.aquagen.co.in` backend repo access (site-data push + Twin portal), final pick DPS vs bake-at-flash.

---

*Generated from `viethnam_final_v10` (Part A) + `modbus_iot_gateway` v1.4.0 (Part B) source analysis + locked production decisions (Part C). Part B contracts are extracted from the working gateway so the AquaGen ESP-IDF build reuses proven names, topics, NVS keys, and endpoints — not reinvented ones.*
