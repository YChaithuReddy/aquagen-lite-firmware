/** AquaGen Lite — WiFi: station connect + SoftAP config mode. */
#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include <stdbool.h>
#include <stddef.h>

void wifi_mgr_init(void);                          // init netif/event loop/wifi driver (once)
bool wifi_mgr_connect_sta(const char *ssid,
                          const char *password,
                          int timeout_ms);          // blocking; true if got IP
bool wifi_mgr_is_connected(void);
void wifi_mgr_start_ap(const char *ssid, const char *password);  // SoftAP for config mode
void wifi_mgr_stop_ap(void);                       // drop SoftAP, keep station (telemetry) up
int  wifi_mgr_ap_client_count(void);               // clients joined to the config SoftAP
void wifi_mgr_stop(void);

// Scan visible networks; fills the provided JSON string buffer with [{ssid,rssi,auth}]. Returns count.
int  wifi_mgr_scan_json(char *out, size_t out_len);

#endif // WIFI_MGR_H
