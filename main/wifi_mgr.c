#include "wifi_mgr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

static const char *TAG = "wifi";

// Reconnect attempts inside a single connect call before reporting failure.
// Power-on timing (router still booting, RF calibration, DHCP not ready) can make
// the FIRST attempt drop with WIFI_EVENT_STA_DISCONNECTED — retrying recovers it
// without a manual reboot.
#define WIFI_MAX_RETRY      3
#define WIFI_RETRY_DELAY_MS 800

static EventGroupHandle_t s_events;
#define BIT_GOT_IP    BIT0
#define BIT_FAIL      BIT1

static bool s_connected = false;
static int  s_retry = 0;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry < WIFI_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "disconnected — retry %d/%d", s_retry, WIFI_MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));  // let the AP/router settle
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "disconnected — %d retries exhausted, giving up this attempt", WIFI_MAX_RETRY);
            xEventGroupSetBits(s_events, BIT_FAIL);
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_retry = 0;                  // fresh budget for the next disconnect
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

void wifi_mgr_init(void)
{
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL);
}

bool wifi_mgr_connect_sta(const char *ssid, const char *password, int timeout_ms)
{
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, password, sizeof(wc.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);          // disable power save (matches old firmware)

    s_retry = 0;                            // fresh retry budget for this attempt
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_FAIL);
    ESP_LOGI(TAG, "connecting to '%s'... (up to %d retries)", ssid, WIFI_MAX_RETRY);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_FAIL,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_GOT_IP) {
        ESP_LOGI(TAG, "connected, got IP");
        return true;
    }
    ESP_LOGE(TAG, "connect failed/timeout");
    return false;
}

bool wifi_mgr_is_connected(void) { return s_connected; }

void wifi_mgr_start_ap(const char *ssid, const char *password)
{
    if (!s_ap_netif)  s_ap_netif  = esp_netif_create_default_wifi_ap();
    // STA interface is required so /scan_wifi can list nearby site networks while the
    // hotspot stays up — an AP-only device cannot scan. Hence APSTA mode below.
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.ap.ssid, ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = strlen(ssid);
    strlcpy((char *)wc.ap.password, password, sizeof(wc.ap.password));
    wc.ap.max_connection = 4;
    wc.ap.authmode = (strlen(password) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    // Disable 802.11w (Protected Management Frames) on the config SoftAP. ESP-IDF 5.x defaults
    // the SoftAP to PMF-capable, which makes the AP run an "SA Query" against the phone and then
    // DISASSOCIATE it (reason 209) when the phone doesn't answer — exactly the join/leave churn
    // that left the operator unable to reach 192.168.4.1. Older firmware (older IDF) had PMF off,
    // which is why setup worked before. Plain WPA2-PSK with PMF off is the most phone-compatible.
    wc.ap.pmf_cfg.capable  = false;
    wc.ap.pmf_cfg.required = false;

    // APSTA — matches modbus_iot_gateway (AP for the phone + STA so /scan_wifi works).
    // Hotspot stability comes from the captive-portal handling (DNS + generate_204), which
    // stops Android from evicting this no-internet AP — not from the wifi mode.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);   // no power-save → steadier AP beacons/responses

    // Captive portal: hand out 192.168.4.1 as the DHCP DNS server so the phone's
    // connectivity-check lookups hit our DNS responder (dns_server.c). Without this the
    // phone keeps its old DNS, never queries us, and Android evicts the no-internet AP.
    {
        esp_netif_dns_info_t dns = { 0 };
        dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1);
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        uint8_t offer_dns = 2;   // DHCPS_OFFER_DNS
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
        esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
        esp_netif_dhcps_start(s_ap_netif);
    }
    ESP_LOGI(TAG, "SoftAP '%s' up at 192.168.4.1 (APSTA + captive portal)", ssid);
}

void wifi_mgr_stop(void)
{
    esp_wifi_stop();
    s_connected = false;
}

// Drop the config SoftAP but KEEP the station connection (telemetry must keep running).
// Reverts APSTA → STA-only. Safe to call when no AP is up (just ensures STA mode).
void wifi_mgr_stop_ap(void)
{
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "SoftAP stopped — back to station-only");
}

// Number of clients currently joined to the config SoftAP (0 when nobody is configuring).
int wifi_mgr_ap_client_count(void)
{
    wifi_sta_list_t list = { 0 };
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
    return list.num;
}

int wifi_mgr_scan_json(char *out, size_t out_len)
{
    uint16_t n = 0;
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) { snprintf(out, out_len, "[]"); return 0; }
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) { snprintf(out, out_len, "[]"); return 0; }
    esp_wifi_scan_get_ap_records(&n, recs);

    size_t off = 0;
    off += snprintf(out + off, out_len - off, "[");
    for (int i = 0; i < n && off < out_len - 64; i++) {
        off += snprintf(out + off, out_len - off, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                        i ? "," : "", (char *)recs[i].ssid, recs[i].rssi, recs[i].authmode);
    }
    snprintf(out + off, out_len - off, "]");
    free(recs);
    return n;
}
