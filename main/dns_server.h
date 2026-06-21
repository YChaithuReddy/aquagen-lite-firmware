#pragma once
// Captive-portal DNS: while the config SoftAP is up, resolve EVERY hostname to the
// gateway IP (192.168.4.1). This — together with answering Android's /generate_204
// connectivity check — makes the phone treat the no-internet hotspot as valid, so it
// does NOT deauth/evict it. Fixes "device not reachable after connecting WiFi".
void dns_server_start(void);
void dns_server_stop(void);
