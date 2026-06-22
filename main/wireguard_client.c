// wireguard_client.c - WireGuard VPN auto-registration for FluxGen Modbus IoT Gateway
// Adapted from /Users/admin/Downloads/esp32-wireguard reference (advanced firmware).

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "esp_wireguard.h"

// Internal headers from trombik/esp_wireguard for keygen + P2P
// (managed_components/trombik__esp_wireguard/src/ added to INCLUDE_DIRS in CMakeLists.txt)
#include "wireguard.h"
#include "wireguardif.h"
#include "crypto.h"

#include "esp_netif.h"
#include "lwip/ip_addr.h"
#include "lwip/ip6_addr.h"
#include "lwip/sockets.h"

#include "wireguard_client.h"
#include "web_config.h"  // for web_config_get_server() lazy handler registration
#include "app_config.h"  // AquaGen config (device_id used as VPN hostname)

static const char *TAG = "WG_CLIENT";

// ── Configuration (matches FluxGen panel server) ─────────────────────
#include "iot_secrets.h"          // WG_AUTH_KEY (gitignored — see iot_secrets.h.template)
#define WG_SERVER_IP      "20.197.19.49"
#define WG_SERVER_PORT    51821
#define AUTH_KEY          WG_AUTH_KEY
#define REGISTER_URL      "https://iiot.aquagen.co.in/wg/api/agent/register"
#define REGISTER_URL_ALT  "http://20.197.19.49:3200/wg/api/agent/register"
#define NVS_NAMESPACE     "wg_id"

// Minimum free heap before attempting an HTTP registration call.
// Web server + MQTT TLS + WireGuard tunnel together need ~100KB.
// Below this threshold a new TLS socket will fail anyway, so skip early.
#define WG_MIN_HEAP_FOR_REGISTER  40000

// P2P peer discovery (polls relay server via WG tunnel for direct device peers).
// DISABLED for AquaGen: admin reaches devices via the VPN server relay by their 10.100.0.x
// IP, so direct peer-to-peer isn't needed. The coord server (10.100.0.1:3200) isn't deployed
// for this hub → it just logged "Connection reset by peer" every 30s. Set 1 to re-enable.
#define ENABLE_WG_P2P           0
// HTTP re-register cadence in keepalive ticks (10s each). The tunnel's own keepalive runs at
// the UDP level (persistent_keepalive=25), so this server check-in can be infrequent. 30 ticks
// = 5 min (was every 2 ticks = 20s, too chatty for a 50-box fleet hitting one server).
#define WG_REREGISTER_TICKS     30
#define P2P_MAX_DIRECT_PEERS    3
#define COORD_PEERS_URL         "http://10.100.0.1:3200/wg/api/coord/peers"
#define COORD_ENDPOINTS_URL     "http://10.100.0.1:3200/wg/api/coord/endpoints"
#define P2P_POLL_MS             30000

// ── Identity (loaded from NVS or assigned by server) ─────────────────
static char s_device_name[32]   = {0};
static char s_wg_privkey[48]    = {0};
static char s_wg_pubkey[48]     = {0};
static char s_wg_local_ip[20]   = {0};
static char s_wg_psk[48]        = {0};
static char s_wg_server_pub[48] = {0};
static bool s_registered        = false;
static bool s_setup_done        = false;
static bool s_restart_pending   = false;  // deferred esp_restart() until web server goes down

// P2P direct peer tracking
static uint8_t s_direct_peer_idx[P2P_MAX_DIRECT_PEERS];
static char    s_direct_peer_ip[P2P_MAX_DIRECT_PEERS][20];
static int     s_direct_peer_count = 0;

// Separate HTTP response buffer for p2p_discovery_task — avoids races with keepalive's s_http_buf
static char s_p2p_buf[2048];
static int  s_p2p_buf_len = 0;

static wireguard_config_t s_wg_config = ESP_WIREGUARD_CONFIG_DEFAULT();
static wireguard_ctx_t    s_wg_ctx    = {0};

// ── HTTP response capture buffer (registration JSON parsing) ─────────
static char s_http_buf[1024];
static int  s_http_buf_len = 0;

// Registration result — distinguishes server rejection from network failure.
// This is critical: a socket error must NOT trigger the NVS-wipe + restart path.
typedef enum {
    REG_OK       = 0,  // server responded 200/201 with valid credentials
    REG_REJECTED = 1,  // server responded 4xx — explicit peer deletion/block
    REG_NET_ERR  = 2,  // socket/TLS failed — network issue, keep existing identity
} reg_result_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        int space = (int)sizeof(s_http_buf) - s_http_buf_len - 1;
        if (space <= 0) return ESP_OK;
        int copy = evt->data_len < space ? evt->data_len : space;
        memcpy(s_http_buf + s_http_buf_len, evt->data, copy);
        s_http_buf_len += copy;
        s_http_buf[s_http_buf_len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t p2p_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        int space = (int)sizeof(s_p2p_buf) - s_p2p_buf_len - 1;
        if (space <= 0) return ESP_OK;
        int copy = evt->data_len < space ? evt->data_len : space;
        memcpy(s_p2p_buf + s_p2p_buf_len, evt->data, copy);
        s_p2p_buf_len += copy;
        s_p2p_buf[s_p2p_buf_len] = '\0';
    }
    return ESP_OK;
}

// ── Curve25519 key generation ────────────────────────────────────────
static void generate_wg_keys(void)
{
    uint8_t priv[32], pub[32];

    // Random private key + Curve25519 clamp (RFC 7748)
    esp_fill_random(priv, 32);
    priv[0]  &= 248;
    priv[31] &= 127;
    priv[31] |= 64;

    // Derive public key from base point {9, 0, ...}
    static const uint8_t basepoint[32] = {9};
    wireguard_x25519(pub, priv, basepoint);

    // Base64 encode both
    size_t outlen = sizeof(s_wg_privkey);
    wireguard_base64_encode(priv, 32, s_wg_privkey, &outlen);
    outlen = sizeof(s_wg_pubkey);
    wireguard_base64_encode(pub, 32, s_wg_pubkey, &outlen);

    ESP_LOGI(TAG, "Generated WG keys: pub=%s", s_wg_pubkey);
}

// ── NVS persistence ──────────────────────────────────────────────────
static bool nvs_load_identity(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len;
    len = sizeof(s_wg_privkey);
    if (nvs_get_str(h, "privkey", s_wg_privkey, &len) != ESP_OK) { nvs_close(h); return false; }
    len = sizeof(s_wg_pubkey);
    if (nvs_get_str(h, "pubkey", s_wg_pubkey, &len) != ESP_OK) { nvs_close(h); return false; }
    len = sizeof(s_wg_local_ip);
    if (nvs_get_str(h, "local_ip", s_wg_local_ip, &len) != ESP_OK) { nvs_close(h); return false; }
    len = sizeof(s_wg_psk);
    nvs_get_str(h, "psk", s_wg_psk, &len);  // optional
    len = sizeof(s_wg_server_pub);
    if (nvs_get_str(h, "server_pub", s_wg_server_pub, &len) != ESP_OK) { nvs_close(h); return false; }

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded NVS identity: ip=%s pub=%.20s...", s_wg_local_ip, s_wg_pubkey);
    return true;
}

static void nvs_save_identity(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open for save failed");
        return;
    }
    nvs_set_str(h, "privkey",    s_wg_privkey);
    nvs_set_str(h, "pubkey",     s_wg_pubkey);
    nvs_set_str(h, "local_ip",   s_wg_local_ip);
    nvs_set_str(h, "psk",        s_wg_psk);
    nvs_set_str(h, "server_pub", s_wg_server_pub);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved identity to NVS");
}

static void nvs_clear_identity(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "NVS identity wiped");
    }
    s_wg_local_ip[0]   = '\0';
    s_wg_psk[0]        = '\0';
    s_wg_server_pub[0] = '\0';
}

// ── HTTP register helper ─────────────────────────────────────────────
static reg_result_t try_register_url(const char *url, const char *body, bool use_tls)
{
    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = HTTP_METHOD_POST,
        .timeout_ms    = 10000,
        .event_handler = http_event_handler,
        // Modest buffers to limit heap pressure during boot (Issue #6)
        .buffer_size      = 1024,
        .buffer_size_tx   = 1024,
    };
    if (use_tls) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    s_http_buf_len = 0;
    s_http_buf[0]  = '\0';

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http_client_init failed for %s", url);
        return REG_NET_ERR;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        // Socket/TLS failure — NOT a server rejection, keep existing NVS identity
        ESP_LOGW(TAG, "POST %s failed: %s", url, esp_err_to_name(err));
        return REG_NET_ERR;
    }

    ESP_LOGI(TAG, "POST %s -> HTTP %d (%.100s)", url, status, s_http_buf);

    if (status == 200 || status == 201) return REG_OK;
    if (status >= 400 && status < 500) return REG_REJECTED;  // explicit server rejection
    return REG_NET_ERR;  // 5xx or unexpected
}

// Extract a JSON string field value into dst.  No external JSON dep — keeps RAM low.
// Looks for "key":"value" (whitespace-tolerant on the key side only).
static void json_extract_str(const char *src, const char *key, char *dst, size_t dst_size)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(src, needle);
    if (!p) return;
    p += strlen(needle);
    const char *e = strchr(p, '"');
    if (!e) return;
    size_t n = (size_t)(e - p);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

static reg_result_t register_with_panel(void)
{
    char body[512];
    snprintf(body, sizeof(body),
        "{\"public_key\":\"%s\","
        "\"hostname\":\"%s\","
        "\"auth_key\":\"%s\"}",
        s_wg_pubkey, s_device_name, AUTH_KEY);

    // Try HTTPS (Caddy) first
    reg_result_t r = try_register_url(REGISTER_URL, body, true);

    // On explicit server rejection from HTTPS, no need to try HTTP fallback
    if (r == REG_REJECTED) {
        ESP_LOGE(TAG, "HTTPS: peer explicitly rejected (4xx)");
        return REG_REJECTED;
    }

    // On HTTPS network error, try plain HTTP fallback
    if (r != REG_OK) {
        r = try_register_url(REGISTER_URL_ALT, body, false);
        if (r == REG_REJECTED) {
            ESP_LOGE(TAG, "HTTP fallback: peer explicitly rejected (4xx)");
            return REG_REJECTED;
        }
        if (r != REG_OK) {
            ESP_LOGE(TAG, "Registration failed on both URLs (network error)");
            return REG_NET_ERR;
        }
    }

    json_extract_str(s_http_buf, "allowed_ip",    s_wg_local_ip,   sizeof(s_wg_local_ip));
    json_extract_str(s_http_buf, "server_pubkey", s_wg_server_pub, sizeof(s_wg_server_pub));
    json_extract_str(s_http_buf, "preshared_key", s_wg_psk,        sizeof(s_wg_psk));

    if (s_wg_local_ip[0] && s_wg_server_pub[0]) {
        ESP_LOGI(TAG, "Registered: ip=%s server_pub=%.20s...", s_wg_local_ip, s_wg_server_pub);
        return REG_OK;
    }

    ESP_LOGE(TAG, "Could not parse registration response");
    return REG_NET_ERR;
}

// ── WireGuard tunnel start ───────────────────────────────────────────
static esp_err_t wg_start(void)
{
    s_wg_config.private_key          = s_wg_privkey;
    s_wg_config.listen_port          = 0;
    s_wg_config.public_key           = s_wg_server_pub;
    s_wg_config.preshared_key        = (s_wg_psk[0] != '\0') ? s_wg_psk : NULL;
    s_wg_config.allowed_ip           = s_wg_local_ip;
    s_wg_config.allowed_ip_mask      = "255.255.255.0";
    s_wg_config.endpoint             = WG_SERVER_IP;
    s_wg_config.port                 = WG_SERVER_PORT;
    s_wg_config.persistent_keepalive = 25;

    esp_err_t err = esp_wireguard_init(&s_wg_config, &s_wg_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wireguard_init: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wireguard_connect(&s_wg_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wireguard_connect: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Waiting for handshake (60s max)...");
    for (int i = 0; i < 60; i++) {
        if (esp_wireguardif_peer_is_up(&s_wg_ctx) == ESP_OK) {
            ESP_LOGI(TAG, "✓ Tunnel UP — VPN IP %s", s_wg_local_ip);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "Tunnel handshake timeout — will retry in keepalive task");
    return ESP_ERR_TIMEOUT;
}

// ── Public API ───────────────────────────────────────────────────────
esp_err_t wireguard_setup(void)
{
    if (s_setup_done && s_registered) {
        return ESP_OK;
    }

    // Use the user-configured Azure device ID as the VPN hostname so the
    // device appears with a meaningful name on the VPN server panel instead
    // of the raw MAC address. Falls back to MAC if not yet configured.
    if (s_device_name[0] == '\0') {
        const app_config_t *cfg = app_config_get();
        if (cfg && cfg->device_id[0] != '\0') {
            strncpy(s_device_name, cfg->device_id, sizeof(s_device_name) - 1);
            s_device_name[sizeof(s_device_name) - 1] = '\0';
        } else {
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            snprintf(s_device_name, sizeof(s_device_name),
                     "ESP32-%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
        ESP_LOGI(TAG, "Device name: %s", s_device_name);
    }

    // Step 1: Try to load identity from NVS
    s_registered = nvs_load_identity();

    if (!s_registered) {
        ESP_LOGI(TAG, "No NVS identity — generating new keys + registering");
        generate_wg_keys();

        // Up to 10 attempts over ~50s
        for (int attempt = 0; attempt < 10 && !s_registered; attempt++) {
            reg_result_t r = register_with_panel();
            if (r == REG_OK) {
                nvs_save_identity();
                s_registered = true;
                break;
            }
            ESP_LOGW(TAG, "Registration attempt %d failed (%s); retry in 5s",
                     attempt + 1, r == REG_REJECTED ? "rejected" : "net error");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    } else {
        // Verify cached identity is still valid on the server
        ESP_LOGI(TAG, "Verifying NVS identity with server...");
        reg_result_t verify_r = register_with_panel();

        if (verify_r == REG_REJECTED) {
            // Server explicitly rejected our stored pubkey — must regenerate
            ESP_LOGW(TAG, "Server rejected stored identity — wiping NVS, regenerating");
            nvs_clear_identity();
            s_registered = false;

            generate_wg_keys();
            for (int attempt = 0; attempt < 10 && !s_registered; attempt++) {
                reg_result_t r = register_with_panel();
                if (r == REG_OK) {
                    nvs_save_identity();
                    s_registered = true;
                    break;
                }
                ESP_LOGW(TAG, "Re-registration attempt %d failed; retry in 5s", attempt + 1);
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        } else if (verify_r == REG_NET_ERR) {
            // Network unreachable at boot — keep NVS identity, proceed with tunnel start
            ESP_LOGW(TAG, "Server unreachable at boot — keeping NVS identity, will verify in keepalive");
            s_registered = true;
        }
        // REG_OK: server confirmed identity (may have refreshed PSK), proceed
    }

    if (!s_registered) {
        ESP_LOGE(TAG, "Could not register after 10 attempts — keepalive task will keep trying");
        s_setup_done = true;
        return ESP_FAIL;
    }

    // Step 2: Bring up tunnel
    // Give the server 3s to apply the new PSK to its WireGuard interface
    // before sending the Initiation packet — avoids PSK-mismatch on first handshake.
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_err_t err = wg_start();
    s_setup_done = true;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Tunnel didn't come up immediately; keepalive will retry");
        // Don't fail hard — keepalive task will reconnect
    }
    return ESP_OK;
}

// Lazy-registers /vpn-status whenever the gateway's web server is up.
// Handles GPIO-triggered server start/stop transparently.
static void ensure_status_handler_registered(void)
{
    static httpd_handle_t s_last_server = NULL;
    httpd_handle_t cur = web_config_get_server();
    if (cur == NULL) {
        s_last_server = NULL;  // server is down; will re-register when it comes back
        return;
    }
    if (cur == s_last_server) return;  // already registered on this server instance
    if (wireguard_register_http_handlers(cur) == ESP_OK) {
        s_last_server = cur;
    }
}

// ── P2P peer discovery ───────────────────────────────────────────────

// Returns the device's local LAN IP as a dotted-decimal string.
// Tries WiFi STA first, then PPP (SIM module). Returns false if neither
// interface has an address assigned yet.
static bool get_lan_ip(char *out, size_t out_size)
{
    esp_netif_ip_info_t ip_info;
    esp_netif_t *iface;

    iface = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (iface && esp_netif_get_ip_info(iface, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        uint32_t a = ip_info.ip.addr;
        snprintf(out, out_size, "%u.%u.%u.%u",
            (unsigned int)((a >> 0) & 0xFF), (unsigned int)((a >> 8) & 0xFF),
            (unsigned int)((a >> 16) & 0xFF), (unsigned int)((a >> 24) & 0xFF));
        return true;
    }
    iface = esp_netif_get_handle_from_ifkey("PPP_DEF");
    if (iface && esp_netif_get_ip_info(iface, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        uint32_t a = ip_info.ip.addr;
        snprintf(out, out_size, "%u.%u.%u.%u",
            (unsigned int)((a >> 0) & 0xFF), (unsigned int)((a >> 8) & 0xFF),
            (unsigned int)((a >> 16) & 0xFF), (unsigned int)((a >> 24) & 0xFF));
        return true;
    }
    return false;
}

__attribute__((unused)) static void p2p_discovery_task(void *arg)
{
    for (int i = 0; i < P2P_MAX_DIRECT_PEERS; i++) {
        s_direct_peer_idx[i] = WIREGUARDIF_INVALID_INDEX;
        s_direct_peer_ip[i][0] = '\0';
    }

    // Let the tunnel stabilise before starting peer discovery
    vTaskDelay(pdMS_TO_TICKS(30000));

    while (1) {
        if (!s_registered || !wireguard_tunnel_is_up()) {
            vTaskDelay(pdMS_TO_TICKS(P2P_POLL_MS));
            continue;
        }

        // ── Step 1: fetch peer list from coord server ─────────────────
        char body[192];
        snprintf(body, sizeof(body),
            "{\"public_key\":\"%s\",\"preshared_key\":\"%s\"}",
            s_wg_pubkey, s_wg_psk);

        s_p2p_buf_len = 0;
        s_p2p_buf[0] = '\0';

        {
            esp_http_client_config_t cfg = {
                .url           = COORD_PEERS_URL,
                .method        = HTTP_METHOD_POST,
                .timeout_ms    = 8000,
                .event_handler = p2p_http_event_handler,
                .buffer_size      = 512,
                .buffer_size_tx   = 512,
            };
            esp_http_client_handle_t c = esp_http_client_init(&cfg);
            if (c) {
                esp_http_client_set_header(c, "Content-Type", "application/json");
                esp_http_client_set_post_field(c, body, strlen(body));
                esp_err_t err = esp_http_client_perform(c);
                int status   = esp_http_client_get_status_code(c);
                esp_http_client_cleanup(c);

                if (err == ESP_OK && (status == 200 || status == 201) && s_p2p_buf_len > 10) {
                    ESP_LOGD(TAG, "[P2P] peers resp: %.120s", s_p2p_buf);

                    // ── Parse peer array ──────────────────────────────
                    char *pos = s_p2p_buf;
                    while (pos && *pos) {
                        char *pk_start = strstr(pos, "\"public_key\":\"");
                        if (!pk_start) break;
                        pk_start += 14;
                        char *pk_end = strchr(pk_start, '"');
                        if (!pk_end) break;

                        char peer_pk[48] = {0};
                        int pk_len = (int)(pk_end - pk_start);
                        if (pk_len > 0 && pk_len < (int)sizeof(peer_pk))
                            strncpy(peer_pk, pk_start, pk_len);

                        // Skip self and relay server
                        if (strcmp(peer_pk, s_wg_pubkey) == 0 ||
                            strcmp(peer_pk, s_wg_server_pub) == 0) {
                            pos = pk_end + 1;
                            continue;
                        }

                        // Extract allowed_ip (VPN IP of this peer, e.g. 10.100.0.X)
                        char peer_ip[20] = {0};
                        {
                            char *p = strstr(pk_start, "\"allowed_ip\":\"");
                            if (p && p < pk_start + 500) {
                                p += 14;
                                char *e = strchr(p, '"');
                                if (e && (e - p) > 0 && (e - p) < (int)sizeof(peer_ip))
                                    strncpy(peer_ip, p, e - p);
                            }
                        }

                        // Check online flag
                        char *online_ptr = strstr(pk_start, "\"online\":true");
                        bool is_online = (online_ptr && online_ptr < pk_start + 500);

                        // Build endpoint string — prefer LAN IP (zero-latency same-subnet path)
                        char peer_endpoint[64] = {0};
                        {
                            char *p = strstr(pk_start, "\"lan_ip\":\"");
                            if (p && p < pk_start + 500) {
                                p += 10;
                                char *e = strchr(p, '"');
                                if (e && (e - p) > 0 && (e - p) < (int)sizeof(peer_endpoint))
                                    strncpy(peer_endpoint, p, e - p);
                            }
                        }
                        if (peer_endpoint[0] == '\0') {
                            char *p = strstr(pk_start, "\"endpoint\":\"");
                            if (p && p < pk_start + 500) {
                                p += 12;
                                char *e = strchr(p, '"');
                                if (e && (e - p) > 0 && (e - p) < (int)sizeof(peer_endpoint))
                                    strncpy(peer_endpoint, p, e - p);
                            }
                        }

                        // Extract preshared_key (base64) for this peer
                        char peer_psk_b64[48] = {0};
                        {
                            char *p = strstr(pk_start, "\"preshared_key\":\"");
                            if (p && p < pk_start + 500) {
                                p += 17;
                                char *e = strchr(p, '"');
                                if (e && (e - p) > 0 && (e - p) < (int)sizeof(peer_psk_b64))
                                    strncpy(peer_psk_b64, p, e - p);
                            }
                        }

                        // Split endpoint into IP and port
                        char ep_ip[20] = {0};
                        uint16_t ep_port = WG_SERVER_PORT;
                        if (peer_endpoint[0]) {
                            char *colon = strrchr(peer_endpoint, ':');
                            if (colon && colon > peer_endpoint) {
                                int ip_len = (int)(colon - peer_endpoint);
                                if (ip_len > 0 && ip_len < (int)sizeof(ep_ip))
                                    strncpy(ep_ip, peer_endpoint, ip_len);
                                ep_port = (uint16_t)atoi(colon + 1);
                            } else {
                                strncpy(ep_ip, peer_endpoint, sizeof(ep_ip) - 1);
                            }
                        }

                        if (!is_online || peer_ip[0] == '\0' || ep_ip[0] == '\0' || peer_pk[0] == '\0') {
                            pos = pk_end + 1;
                            continue;
                        }

                        // Check if already tracked
                        int slot = -1;
                        for (int i = 0; i < s_direct_peer_count; i++) {
                            if (strcmp(s_direct_peer_ip[i], peer_ip) == 0) { slot = i; break; }
                        }

                        if (slot < 0 && s_direct_peer_count < P2P_MAX_DIRECT_PEERS) {
                            // New peer — add to WireGuard interface
                            slot = s_direct_peer_count;

                            // Decode PSK (binary needed by wireguardif_add_peer)
                            uint8_t psk_bytes[32] = {0};
                            const uint8_t *psk_ptr = NULL;
                            if (peer_psk_b64[0]) {
                                size_t psk_len = 32;
                                if (wireguard_base64_decode(peer_psk_b64, psk_bytes, &psk_len) && psk_len == 32)
                                    psk_ptr = psk_bytes;
                            }

                            struct wireguardif_peer peer_cfg;
                            wireguardif_peer_init(&peer_cfg);
                            peer_cfg.public_key    = peer_pk;   // base64; wireguardif decodes internally
                            peer_cfg.preshared_key = psk_ptr;   // decoded binary bytes, or NULL
                            peer_cfg.keep_alive    = 25;

                            ip_addr_t allowed_ip, allowed_mask, ep_addr;
                            memset(&allowed_ip,   0, sizeof(allowed_ip));
                            memset(&allowed_mask, 0, sizeof(allowed_mask));
                            memset(&ep_addr,      0, sizeof(ep_addr));
                            ipaddr_aton(peer_ip,          &allowed_ip);
                            ipaddr_aton("255.255.255.255", &allowed_mask);
                            ipaddr_aton(ep_ip,             &ep_addr);

                            peer_cfg.allowed_ip   = allowed_ip;
                            peer_cfg.allowed_mask = allowed_mask;
                            peer_cfg.endpoint_ip  = ep_addr;
                            peer_cfg.endport_port = ep_port;

                            uint8_t idx = WIREGUARDIF_INVALID_INDEX;
                            err_t add_err = wireguardif_add_peer(s_wg_ctx.netif, &peer_cfg, &idx);
                            if (add_err == ERR_OK && idx != WIREGUARDIF_INVALID_INDEX) {
                                s_direct_peer_idx[slot] = idx;
                                strncpy(s_direct_peer_ip[slot], peer_ip, sizeof(s_direct_peer_ip[slot]) - 1);
                                s_direct_peer_count++;
                                wireguardif_connect(s_wg_ctx.netif, idx);
                                ESP_LOGI(TAG, "[P2P] Added peer %s → %s:%u", peer_ip, ep_ip, ep_port);
                            } else {
                                ESP_LOGW(TAG, "[P2P] wireguardif_add_peer(%s) err=%d", peer_ip, (int)add_err);
                            }

                        } else if (slot >= 0 && s_direct_peer_idx[slot] != WIREGUARDIF_INVALID_INDEX) {
                            // Existing peer — update endpoint in case it roamed
                            ip_addr_t new_ep;
                            memset(&new_ep, 0, sizeof(new_ep));
                            ipaddr_aton(ep_ip, &new_ep);
                            wireguardif_update_endpoint(s_wg_ctx.netif, s_direct_peer_idx[slot], &new_ep, ep_port);
                        }

                        pos = pk_end + 1;
                    }
                } else {
                    ESP_LOGD(TAG, "[P2P] coord/peers: %s status=%d", esp_err_to_name(err), status);
                }
            }
        }

        // ── Step 2: report own LAN IP + IPv6 to coord/endpoints ──────
        {
            char lan_ip[20]   = {0};
            char ipv6_str[48] = {0};

            get_lan_ip(lan_ip, sizeof(lan_ip));

            // Try to get global IPv6 address (available when CONFIG_LWIP_IPV6=y)
            esp_netif_t *sta6 = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (sta6) {
                esp_ip6_addr_t ip6 = {0};
                if (esp_netif_get_ip6_global(sta6, &ip6) == ESP_OK) {
                    // ntohl each word: lwIP stores IPv6 in network byte order as uint32_t
                    uint32_t w0 = lwip_ntohl(ip6.addr[0]);
                    uint32_t w1 = lwip_ntohl(ip6.addr[1]);
                    uint32_t w2 = lwip_ntohl(ip6.addr[2]);
                    uint32_t w3 = lwip_ntohl(ip6.addr[3]);
                    snprintf(ipv6_str, sizeof(ipv6_str),
                        "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
                        (unsigned int)((w0 >> 16) & 0xffff), (unsigned int)(w0 & 0xffff),
                        (unsigned int)((w1 >> 16) & 0xffff), (unsigned int)(w1 & 0xffff),
                        (unsigned int)((w2 >> 16) & 0xffff), (unsigned int)(w2 & 0xffff),
                        (unsigned int)((w3 >> 16) & 0xffff), (unsigned int)(w3 & 0xffff));
                }
            }

            if (lan_ip[0] || ipv6_str[0]) {
                char report[384];
                snprintf(report, sizeof(report),
                    "{\"public_key\":\"%s\",\"preshared_key\":\"%s\","
                    "\"lan_ip\":\"%s\",\"ipv6\":\"%s\",\"wg_port\":0}",
                    s_wg_pubkey, s_wg_psk, lan_ip, ipv6_str);

                esp_http_client_config_t rcfg = {
                    .url        = COORD_ENDPOINTS_URL,
                    .method     = HTTP_METHOD_POST,
                    .timeout_ms = 5000,
                    .buffer_size      = 256,
                    .buffer_size_tx   = 256,
                };
                esp_http_client_handle_t rc = esp_http_client_init(&rcfg);
                if (rc) {
                    esp_http_client_set_header(rc, "Content-Type", "application/json");
                    esp_http_client_set_post_field(rc, report, strlen(report));
                    esp_http_client_perform(rc);
                    esp_http_client_cleanup(rc);
                    ESP_LOGD(TAG, "[P2P] Reported lan=%s ipv6=%s", lan_ip, ipv6_str);
                }
            }
        }

        // ── Step 3: log direct peer health ────────────────────────────
        for (int i = 0; i < s_direct_peer_count; i++) {
            if (s_direct_peer_idx[i] != WIREGUARDIF_INVALID_INDEX) {
                ip_addr_t cur_ip;
                uint16_t  cur_port;
                err_t up = wireguardif_peer_is_up(s_wg_ctx.netif, s_direct_peer_idx[i], &cur_ip, &cur_port);
                ESP_LOGI(TAG, "[P2P] Peer %s: %s", s_direct_peer_ip[i], up == ERR_OK ? "UP" : "DOWN");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(P2P_POLL_MS));
    }
}

// ── Browser-to-wake task ─────────────────────────────────────────────
// When the web server is down, holds port 80 with a minimal TCP socket.
// First browser request gets a "starting..." page with a 2-second
// auto-refresh; the full web server is started behind the scenes and
// is ready by the time the browser follows the refresh.
// Auto-stops the web server after 5 minutes to reclaim ~64 KB heap.
#define WAKE_WEB_SERVER_TIMEOUT_MS (30 * 60 * 1000)

static const char WAKE_HTML[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n\r\n"
    "<!DOCTYPE html><html><head>"
    "<meta http-equiv='refresh' content='2;url=/'>"
    "<title>FluxGen Gateway</title></head><body>"
    "<p><b>FluxGen Gateway</b> &mdash; web interface is starting, "
    "please wait...</p>"
    "</body></html>";

// Set by the GPIO34 config button (main.c). When true, this task keeps the config web server
// up (and yields the wake-listener socket); when cleared, it stops the server. Doing it here —
// the single owner of port 80 — avoids a bind race and avoids any runtime WiFi reconfiguration.
extern volatile bool g_web_open_req;

static void tcp_wake_task(void *arg)
{
    for (;;) {
        // Config button / Device Twin requested the web UI → start it and keep it up, REGARDLESS of
        // WireGuard state (the button MUST work even if the tunnel is down / not yet registered —
        // that was bug B1). Don't hold the wake-listener socket while config is open. Reachable on
        // the box's LAN IP and, if up, its VPN IP.
        if (g_web_open_req) {
            if (web_config_get_server() == NULL) {
                ESP_LOGI(TAG, "[WAKE] config requested — starting web server");
                web_config_start();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        // Request cleared but a server is still up → stop it, reclaim heap.
        if (web_config_get_server() != NULL) {
            ESP_LOGI(TAG, "[WAKE] config closed — stopping web server");
            web_config_stop();
        }

        // Browser-wake-over-VPN listener — only relevant once registered + tunnel up.
        if (!s_registered || s_wg_ctx.netif == NULL || !wireguard_tunnel_is_up()) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        int srv = socket(AF_INET, SOCK_STREAM, 0);
        if (srv < 0) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        int opt = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr = {
            .sin_family      = AF_INET,
            .sin_addr.s_addr = htonl(INADDR_ANY),
            .sin_port        = htons(80),
        };

        if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(srv, 1) != 0) {
            ESP_LOGW(TAG, "[WAKE] bind/listen port 80 failed (%d) — retry 15s", errno);
            close(srv);
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }

        ESP_LOGI(TAG, "[WAKE] Ready — open http://%s/ in browser to start web UI",
                 s_wg_local_ip);

        // Accept loop — exit when web server comes up or tunnel drops
        while (wireguard_tunnel_is_up() && web_config_get_server() == NULL) {
            struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
            setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            int cli = accept(srv, NULL, NULL);
            if (cli < 0) continue; // 5s timeout — loop and re-check conditions

            // Drain the browser's HTTP request (we don't need its content)
            char buf[256];
            recv(cli, buf, sizeof(buf) - 1, 0);

            // Reply with auto-refresh page; browser waits 2s then reloads /
            send(cli, WAKE_HTML, sizeof(WAKE_HTML) - 1, 0);
            close(cli);

            // Release port 80 before httpd grabs it
            close(srv);
            srv = -1;

            ESP_LOGI(TAG, "[WAKE] Browser hit — starting web server");
            web_config_start();

            // Auto-stop after 5 min to reclaim heap
            vTaskDelay(pdMS_TO_TICKS(WAKE_WEB_SERVER_TIMEOUT_MS));
            if (web_config_get_server() != NULL) {
                ESP_LOGI(TAG, "[WAKE] 5-min timeout — stopping web server");
                web_config_stop();
            }
            break; // back to outer loop — reopen wake listener
        }

        if (srv >= 0) close(srv);
    }
}

// ── Keepalive task ───────────────────────────────────────────────────
static void wireguard_keepalive_task(void *arg)
{
    int tick = 0;

    // If setup never registered us, keep polling every 5s until admin unblocks
    while (!s_registered) {
        ESP_LOGW(TAG, "[KEEPALIVE] Not registered — retry in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (register_with_panel() == REG_OK) {
            nvs_save_identity();
            s_registered = true;
            ESP_LOGI(TAG, "[KEEPALIVE] Registered after retry — starting tunnel");
            wg_start();
        } else {
            // Cycle keys after each batch — admin may have blocked the pubkey
            generate_wg_keys();
        }
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        tick++;

        // Lazy-register /vpn-status if web server has come up since last tick
        ensure_status_handler_registered();

        // 10s tunnel check
        bool up = (esp_wireguardif_peer_is_up(&s_wg_ctx) == ESP_OK);
        if (!up) {
            ESP_LOGW(TAG, "[KEEPALIVE] Tunnel down — reconnecting");
            esp_wireguard_connect(&s_wg_ctx);
        }

        // Apply deferred restart once web server has gone idle
        if (s_restart_pending && web_config_get_server() == NULL) {
            ESP_LOGI(TAG, "[KEEPALIVE] Applying deferred restart — web server idle");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }

        // 20s server-verify.
        // Skip when web server is running — the extra HTTP connection would consume
        // the last ~15KB of heap (web server uses ~64KB leaving very little margin).
        // Also skip when heap is already below threshold regardless of web server state.
        if (tick % WG_REREGISTER_TICKS == 0) {
            bool web_up   = (web_config_get_server() != NULL);
            uint32_t heap = (uint32_t)esp_get_free_heap_size();

            if (web_up) {
                ESP_LOGD(TAG, "[KEEPALIVE] Skip server-verify — web server active");
            } else if (heap < WG_MIN_HEAP_FOR_REGISTER) {
                ESP_LOGW(TAG, "[KEEPALIVE] Skip server-verify — heap low (%"PRIu32" B)", heap);
            } else {
                reg_result_t r = register_with_panel();

                if (r == REG_REJECTED) {
                    // Server explicitly deleted our peer — must regenerate identity
                    ESP_LOGW(TAG, "[KEEPALIVE] Server says peer deleted — wipe + re-register");
                    esp_wireguard_disconnect(&s_wg_ctx);
                    nvs_clear_identity();
                    s_registered = false;

                    generate_wg_keys();
                    while (register_with_panel() != REG_OK) {
                        ESP_LOGW(TAG, "[KEEPALIVE] Waiting for admin unblock... 5s");
                        vTaskDelay(pdMS_TO_TICKS(5000));
                        generate_wg_keys();
                    }
                    nvs_save_identity();
                    s_registered = true;

                    // Restart needed to bring tunnel up with new credentials.
                    // Defer if web server came up during the re-registration window.
                    if (web_config_get_server() != NULL) {
                        ESP_LOGW(TAG, "[KEEPALIVE] Re-registered — restart pending (web server up)");
                        s_restart_pending = true;
                    } else {
                        ESP_LOGI(TAG, "[KEEPALIVE] Re-registered — restarting to bring tunnel up cleanly");
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        esp_restart();
                    }
                } else if (r == REG_NET_ERR) {
                    // Transient network issue — do nothing, tunnel keepalive will reconnect
                    ESP_LOGD(TAG, "[KEEPALIVE] Server unreachable — will retry next verify tick");
                }
                // REG_OK: identity confirmed, nothing to do
            }
        }

        if (up && (tick % 6 == 0)) {
            ESP_LOGI(TAG, "[KEEPALIVE] Tunnel OK — %s | heap %"PRIu32" KB",
                     s_wg_local_ip,
                     (uint32_t)(esp_get_free_heap_size() / 1024));
        }
    }
}

void wireguard_start_keepalive_task(void)
{
    static bool spawned = false;
    if (spawned) return;
    spawned = true;
    xTaskCreate(wireguard_keepalive_task, "wg_keepalive", 5120, NULL, 4, NULL);
#if ENABLE_WG_P2P
    xTaskCreate(p2p_discovery_task,       "wg_p2p",       6144, NULL, 3, NULL);
#endif
    xTaskCreate(tcp_wake_task,            "wg_wake",      3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "WG keepalive + browser-wake tasks spawned%s",
             ENABLE_WG_P2P ? " + P2P discovery" : "");
}

// ── /vpn-status HTTP handler ─────────────────────────────────────────
static esp_err_t handler_vpn_status(httpd_req_t *req)
{
    bool tunnel_up = (esp_wireguardif_peer_is_up(&s_wg_ctx) == ESP_OK);
    char buf[384];
    snprintf(buf, sizeof(buf),
        "{\"device\":\"%s\","
        "\"vpn_ip\":\"%s\","
        "\"tunnel\":%s,"
        "\"registered\":%s,"
        "\"server\":\"%s:%d\","
        "\"pubkey\":\"%s\","
        "\"free_heap_kb\":%"PRIu32"}",
        s_device_name,
        s_wg_local_ip[0] ? s_wg_local_ip : "",
        tunnel_up ? "true" : "false",
        s_registered ? "true" : "false",
        WG_SERVER_IP, WG_SERVER_PORT,
        s_wg_pubkey,
        (uint32_t)(esp_get_free_heap_size() / 1024));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

esp_err_t wireguard_register_http_handlers(httpd_handle_t server)
{
    if (!server) {
        ESP_LOGW(TAG, "register_http_handlers: server is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    httpd_uri_t uri = {
        .uri      = "/vpn-status",
        .method   = HTTP_GET,
        .handler  = handler_vpn_status,
        .user_ctx = NULL,
    };
    esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Registered /vpn-status handler");
    } else if (err == ESP_ERR_HTTPD_HANDLER_EXISTS) {
        // Already registered on this server (e.g. handle reused after a stop/start). That's fine —
        // treat as success so ensure_status_handler_registered() stops re-trying every 10s.
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "Failed to register /vpn-status: %s", esp_err_to_name(err));
    }
    return err;
}

// ── Status accessors ─────────────────────────────────────────────────
bool wireguard_tunnel_is_up(void)
{
    return s_registered && (esp_wireguardif_peer_is_up(&s_wg_ctx) == ESP_OK);
}

const char* wireguard_get_local_ip(void)    { return s_wg_local_ip; }
const char* wireguard_get_pubkey(void)      { return s_wg_pubkey; }
const char* wireguard_get_device_name(void) { return s_device_name; }
bool        wireguard_is_registered(void)   { return s_registered; }
