/**
 * AquaGen Lite — OTA update. PORTED from the proven modbus_iot_gateway ota_update.c
 * (debugged through GitHub cert/heap/redirect/long-URL issues — see gateway CLAUDE.md Issue #6).
 * Adapted: WiFi-only (SIM/PPP path removed); MQTT is stopped by the caller (device_twin) before ota_start().
 *
 * The hard-won bits kept verbatim in spirit:
 *  - MANUAL redirect handling (disable_auto_redirect=true) capturing the Location header in the
 *    event handler and recreating the client per redirect. esp_https_ota's auto-redirect does NOT
 *    work with GitHub's release CDN (long signed URLs, cert mismatch).
 *  - GitHub host detection → skip cert bundle + skip CN check.
 *  - 4096/1024 buffers, 2048-byte redirect buffer, User-Agent + Accept headers (GitHub requires UA).
 */
#include "ota.h"
#include "iot_configs.h"
#include "app_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "ota";

#define OTA_MIN_FREE_HEAP   (40 * 1024)
#define MAX_REDIRECTS       10
#define DL_BUF_SIZE         4096

typedef enum { OTA_IDLE, OTA_DOWNLOADING, OTA_SUCCESS, OTA_FAILED } ota_state_t;
static volatile ota_state_t s_state = OTA_IDLE;
static volatile bool s_running = false;
static char s_url[256];

// GitHub CDN URLs are long (signed tokens) → need a big buffer.
static char s_redirect[2048];

const char *ota_status_str(void)
{
    switch (s_state) {
    case OTA_DOWNLOADING: return "downloading";
    case OTA_SUCCESS:     return "success";
    case OTA_FAILED:      return "failed";
    default:              return "idle";
    }
}

bool ota_in_progress(void) { return s_running; }

static bool is_github(const char *url)
{
    return strstr(url, "github.com") || strstr(url, "githubusercontent.com") ||
           strstr(url, "github-releases") || strstr(url, "objects.githubusercontent.com");
}

// Capture the Location header during redirects.
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER &&
        (strcasecmp(evt->header_key, "Location") == 0)) {
        strlcpy(s_redirect, evt->header_value, sizeof(s_redirect));
        ESP_LOGI(TAG, "captured Location (%d bytes)", (int)strlen(s_redirect));
    }
    return ESP_OK;
}

static void ota_task(void *arg)
{
    ESP_LOGI(TAG, "OTA start: %s", s_url);
    s_state = OTA_DOWNLOADING;
    s_running = true;

    esp_http_client_handle_t client = NULL;
    uint8_t *buf = malloc(DL_BUF_SIZE);
    char *current_url = strdup(s_url);
    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;
    int status_code = 0, content_length = 0;
    esp_err_t err;

    if (!buf || !current_url) { ESP_LOGE(TAG, "alloc failed"); goto fail; }

    int redirects = 0;
    while (redirects < MAX_REDIRECTS) {
        s_redirect[0] = '\0';
        bool gh = is_github(current_url);
        ESP_LOGI(TAG, "connecting: %s%s", current_url, gh ? " (github)" : "");

        // ALWAYS validate via the CA bundle. GitHub + cloudflare + Azure all use standard CAs that
        // are in esp_crt_bundle. (The old code set crt_bundle=NULL to "skip cert" for GitHub, but
        // esp-tls then has NO verification method and fails: "No server verification option set".)
        esp_http_client_config_t hc = {
            .url = current_url,
            .timeout_ms = 30000,
            .keep_alive_enable = false,
            .buffer_size = 4096,
            .buffer_size_tx = 1024,
            .skip_cert_common_name_check = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true,            // MANUAL redirect — the key fix
            .event_handler = http_event_handler,
        };
        client = esp_http_client_init(&hc);
        if (!client) { ESP_LOGE(TAG, "client init failed"); goto fail; }
        esp_http_client_set_header(client, "User-Agent", "AquaGen-OTA/1.0");
        esp_http_client_set_header(client, "Accept", "*/*");

        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) { ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err)); goto fail; }

        content_length = esp_http_client_fetch_headers(client);
        status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP %d, len %d", status_code, content_length);

        if (status_code == 301 || status_code == 302 || status_code == 303 ||
            status_code == 307 || status_code == 308) {
            redirects++;
            if (s_redirect[0] == '\0') { ESP_LOGE(TAG, "redirect w/o Location"); goto fail; }
            char *next = strdup(s_redirect);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = NULL;
            free(current_url);
            current_url = next;
            if (!current_url) goto fail;
            continue;
        }
        break;  // not a redirect
    }

    free(current_url);
    current_url = NULL;

    if (redirects >= MAX_REDIRECTS) { ESP_LOGE(TAG, "too many redirects"); goto fail; }
    if (status_code != 200) { ESP_LOGE(TAG, "HTTP error %d", status_code); goto fail; }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { ESP_LOGE(TAG, "no OTA partition"); goto fail; }
    ESP_LOGI(TAG, "writing to %s @ 0x%lx", part->label, (unsigned long)part->address);

    err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) { ESP_LOGE(TAG, "ota_begin: %s", esp_err_to_name(err)); goto fail; }
    ota_started = true;

    int total = 0, last_pct = -10, n;
    while ((n = esp_http_client_read(client, (char *)buf, DL_BUF_SIZE)) > 0) {
        err = esp_ota_write(ota_handle, buf, n);
        if (err != ESP_OK) { ESP_LOGE(TAG, "ota_write: %s", esp_err_to_name(err)); goto fail; }
        total += n;
        if (content_length > 0) {
            int pct = (total * 100) / content_length;
            if (pct >= last_pct + 10) { ESP_LOGI(TAG, "%d%% (%d/%d)", pct, total, content_length); last_pct = pct; }
        }
    }
    if (n < 0) { ESP_LOGE(TAG, "read error"); goto fail; }
    ESP_LOGI(TAG, "downloaded %d bytes", total);

    err = esp_ota_end(ota_handle);
    ota_handle = 0; ota_started = false;
    if (err != ESP_OK) { ESP_LOGE(TAG, "ota_end: %s", esp_err_to_name(err)); goto fail; }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) { ESP_LOGE(TAG, "set_boot: %s", esp_err_to_name(err)); goto fail; }

    s_state = OTA_SUCCESS;
    ESP_LOGI(TAG, "✅ OTA success — rebooting in 3s");
    if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
    free(buf);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return;

fail:
    s_state = OTA_FAILED;
    if (ota_started && ota_handle) esp_ota_abort(ota_handle);
    if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
    if (buf) free(buf);
    if (current_url) free(current_url);
    s_running = false;
    ESP_LOGE(TAG, "OTA failed");
    vTaskDelete(NULL);
}

void ota_start(const char *url)
{
    app_config_t *cfg = app_config_get();
    if (!cfg->ota_enabled) { ESP_LOGW(TAG, "OTA disabled"); return; }
    if (s_running) { ESP_LOGW(TAG, "OTA already running"); return; }
    if (!url || url[0] == '\0') return;
    if (esp_get_free_heap_size() < OTA_MIN_FREE_HEAP) {
        ESP_LOGE(TAG, "heap too low (%lu) — caller must stop MQTT first",
                 (unsigned long)esp_get_free_heap_size());
        return;
    }
    strlcpy(s_url, url, sizeof(s_url));
    xTaskCreate(ota_task, "ota", 8192, NULL, 5, NULL);
}

void ota_mark_valid_if_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA image marked valid (rollback cancelled)");
    }
}
