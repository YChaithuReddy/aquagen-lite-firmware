// wireguard_client.h - WireGuard VPN auto-registration client
//
// Provides remote access to the gateway's web config panel and MQTT path
// through a WireGuard tunnel managed by the FluxGen IoT panel server.
//
// Boot flow (called from app_main after WiFi/PPP up + SNTP synced):
//   1. nvs_load_identity() - check if we already have a registered identity
//   2. If not registered: generate Curve25519 keypair + POST to register API
//   3. If registered: verify with server (server may say "deleted" -> wipe + re-register)
//   4. Start WireGuard tunnel with assigned IP
//   5. Spawn keepalive task (10s tunnel check / 20s server verify)
//
// MUST be called BEFORE Azure MQTT connects (Issue #6 - one TLS session at a time).

#ifndef WIREGUARD_CLIENT_H
#define WIREGUARD_CLIENT_H

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Full setup: NVS load -> generate/register -> tunnel up. Blocking.
// Returns ESP_OK if tunnel is up, ESP_FAIL if registration permanently failed.
// Safe to call multiple times - subsequent calls are no-ops if tunnel up.
esp_err_t wireguard_setup(void);

// Spawn the background keepalive task. Call AFTER wireguard_setup() succeeds.
// Task: every 10s reconnect tunnel if down, every 20s verify with server.
// On peer-deletion: wipes NVS, regenerates keys, polls every 5s until unblocked,
// then esp_restart() to bring tunnel up cleanly.
void wireguard_start_keepalive_task(void);

// Register a /vpn-status JSON handler on the existing httpd server.
// Call AFTER web_config_start_server_only() succeeds.
esp_err_t wireguard_register_http_handlers(httpd_handle_t server);

// Status accessors (safe to call from any task; thread-safe via cached state)
bool        wireguard_tunnel_is_up(void);
const char* wireguard_get_local_ip(void);     // assigned VPN IP, e.g. "10.100.0.42"
const char* wireguard_get_pubkey(void);       // base64 public key
const char* wireguard_get_device_name(void);  // "ESP32-XXXXXXXXXXXX"
bool        wireguard_is_registered(void);

#ifdef __cplusplus
}
#endif

#endif // WIREGUARD_CLIENT_H
