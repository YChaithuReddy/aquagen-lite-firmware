/**
 * AquaGen Lite — config REST API (esp_http_server) for the Flutter app.
 * No embedded HTML blob (unlike the gateway's 11,853-line web_config.c) — pure JSON REST.
 * Served on the SoftAP (192.168.4.1) during config mode, and reachable over WireGuard.
 */
#ifndef WEB_CONFIG_H
#define WEB_CONFIG_H

#include "esp_http_server.h"

void web_config_start(void);
void web_config_stop(void);

// Returns the running server handle (or NULL) — used by wireguard_client to lazily register /vpn-status.
httpd_handle_t web_config_get_server(void);

#endif // WEB_CONFIG_H
