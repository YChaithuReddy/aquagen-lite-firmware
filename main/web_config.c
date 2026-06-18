#include "web_config.h"
#include "app_config.h"
#include "wifi_mgr.h"
#include "modbus_meter.h"
#include "iot_configs.h"
#include "ota.h"
#include "azure_mqtt.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "webcfg";
static httpd_handle_t s_server = NULL;

// Defined in main.c — queues an OTA to run from the main task (can't stop MQTT from the httpd task).
extern void ota_request(const char *url);

httpd_handle_t web_config_get_server(void) { return s_server; }

// --- helpers ---
static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_ok(httpd_req_t *req) { return send_json(req, "{\"status\":\"ok\"}"); }
static esp_err_t send_err(httpd_req_t *req, const char *msg)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"status\":\"error\",\"message\":\"%s\"}", msg);
    httpd_resp_set_status(req, "400 Bad Request");
    return send_json(req, buf);
}

// Read the full request body into a heap buffer (caller frees). Returns NULL on error.
static char *read_body(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 4096) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    buf[len] = '\0';
    return buf;
}

// GET / — basic identity/status landing (so a browser hitting the AP sees something).
static esp_err_t h_root(httpd_req_t *req)
{
    app_config_t *cfg = app_config_get();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"product\":\"AquaGen Lite\",\"fw\":\"%s\",\"device_id\":\"%s\",\"configured\":%s}",
             FW_VERSION_STRING, cfg->device_id, app_config_has_wifi() ? "true" : "false");
    return send_json(req, buf);
}

// GET /api/system_status — live status for the app.
static esp_err_t h_status(httpd_req_t *req)
{
    app_config_t *cfg = app_config_get();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "product", "AquaGen Lite");
    cJSON_AddStringToObject(r, "fw_version", FW_VERSION_STRING);
    cJSON_AddStringToObject(r, "device_id", cfg->device_id);
    cJSON_AddStringToObject(r, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddBoolToObject(r, "wifi_configured", app_config_has_wifi());
    cJSON_AddBoolToObject(r, "wifi_connected", wifi_mgr_is_connected());
    cJSON_AddNumberToObject(r, "telemetry_interval", cfg->telemetry_interval_s);
    cJSON_AddNumberToObject(r, "free_heap", (double)esp_get_free_heap_size());
    // Modbus health — lets the app show the operator why a meter isn't reading.
    const modbus_stats_t *ms = modbus_meter_stats();
    cJSON *mb = cJSON_AddObjectToObject(r, "modbus");
    cJSON_AddNumberToObject(mb, "total", (double)ms->total);
    cJSON_AddNumberToObject(mb, "ok", (double)ms->ok);
    cJSON_AddNumberToObject(mb, "failed", (double)ms->failed);
    cJSON_AddNumberToObject(mb, "timeouts", (double)ms->timeouts);
    cJSON_AddNumberToObject(mb, "crc_errors", (double)ms->crc_errors);
    cJSON_AddNumberToObject(mb, "last_exception", (double)ms->last_exception);
    cJSON *meters = cJSON_AddArrayToObject(r, "meters");
    for (int i = 0; i < MAX_METERS; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "slave_id", cfg->meters[i].slave_id);
        cJSON_AddStringToObject(m, "unit_id", cfg->meters[i].unit_id);
        cJSON_AddStringToObject(m, "serial", cfg->meters[i].serial);
        cJSON_AddNumberToObject(m, "baud", cfg->meters[i].baud);
        cJSON_AddStringToObject(m, "parity", cfg->meters[i].parity);
        cJSON_AddBoolToObject(m, "enabled", cfg->meters[i].enabled);
        cJSON_AddItemToArray(meters, m);
    }
    char *json = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    esp_err_t e = send_json(req, json);
    cJSON_free(json);
    return e;
}

// GET /scan_wifi — list nearby networks.
static esp_err_t h_scan(httpd_req_t *req)
{
    char buf[2048];
    wifi_mgr_scan_json(buf, sizeof(buf));
    return send_json(req, buf);
}

// POST /save_wifi  { "ssid": "...", "password": "..." }
static esp_err_t h_save_wifi(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return send_err(req, "no body");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return send_err(req, "bad json");
    cJSON *ssid = cJSON_GetObjectItem(j, "ssid");
    cJSON *pw = cJSON_GetObjectItem(j, "password");
    if (!cJSON_IsString(ssid)) { cJSON_Delete(j); return send_err(req, "ssid required"); }
    app_config_t *cfg = app_config_get();
    strlcpy(cfg->wifi_ssid, ssid->valuestring, sizeof(cfg->wifi_ssid));
    strlcpy(cfg->wifi_password, cJSON_IsString(pw) ? pw->valuestring : "", sizeof(cfg->wifi_password));
    cJSON_Delete(j);
    app_config_save();
    return send_ok(req);
}

// POST /save_azure  { "device_id": "...", "device_key": "...", "iothub": "..." }  (manual / bake fallback)
static esp_err_t h_save_azure(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return send_err(req, "no body");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return send_err(req, "bad json");
    app_config_t *cfg = app_config_get();
    cJSON *it;
    // Only overwrite when a NON-EMPTY value is provided — never let an empty/missing field
    // wipe a baked-at-flash identity (an app sending "device_id":"" used to clear it).
    if ((it = cJSON_GetObjectItem(j, "device_id")) && cJSON_IsString(it) && it->valuestring[0])
        strlcpy(cfg->device_id, it->valuestring, sizeof(cfg->device_id));
    if ((it = cJSON_GetObjectItem(j, "device_key")) && cJSON_IsString(it) && it->valuestring[0])
        strlcpy(cfg->device_key, it->valuestring, sizeof(cfg->device_key));
    if ((it = cJSON_GetObjectItem(j, "iothub")) && cJSON_IsString(it) && it->valuestring[0])
        strlcpy(cfg->iothub_fqdn, it->valuestring, sizeof(cfg->iothub_fqdn));
    cJSON_Delete(j);
    app_config_save();
    return send_ok(req);
}

// POST /save_meters  { "meters": [ {slave_id,unit_id,serial,baud,parity,enabled}, ... ] }
static esp_err_t h_save_meters(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return send_err(req, "no body");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return send_err(req, "bad json");
    cJSON *arr = cJSON_GetObjectItem(j, "meters");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(j); return send_err(req, "meters array required"); }
    app_config_t *cfg = app_config_get();
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n && i < MAX_METERS; i++) {
        cJSON *m = cJSON_GetArrayItem(arr, i);
        meter_cfg_t *dst = &cfg->meters[i];
        cJSON *it;
        if ((it = cJSON_GetObjectItem(m, "slave_id")) && cJSON_IsNumber(it)) dst->slave_id = it->valueint;
        if ((it = cJSON_GetObjectItem(m, "baud")) && cJSON_IsNumber(it)) dst->baud = (uint32_t)it->valuedouble;
        if ((it = cJSON_GetObjectItem(m, "unit_id")) && cJSON_IsString(it)) strlcpy(dst->unit_id, it->valuestring, sizeof(dst->unit_id));
        if ((it = cJSON_GetObjectItem(m, "serial")) && cJSON_IsString(it)) strlcpy(dst->serial, it->valuestring, sizeof(dst->serial));
        if ((it = cJSON_GetObjectItem(m, "parity")) && cJSON_IsString(it)) strlcpy(dst->parity, it->valuestring, sizeof(dst->parity));
        if ((it = cJSON_GetObjectItem(m, "enabled")) && cJSON_IsBool(it)) dst->enabled = cJSON_IsTrue(it);
    }
    cJSON_Delete(j);
    app_config_save();
    return send_ok(req);
}

// POST /test_meter  { "index": 0 }  → live Modbus read so the installer SEES it working.
static esp_err_t h_test_meter(httpd_req_t *req)
{
    char *body = read_body(req);
    int idx = 0;
    if (body) {
        cJSON *j = cJSON_Parse(body); free(body);
        if (j) { cJSON *it = cJSON_GetObjectItem(j, "index"); if (cJSON_IsNumber(it)) idx = it->valueint; cJSON_Delete(j); }
    }
    if (idx < 0 || idx >= MAX_METERS) return send_err(req, "bad index");
    app_config_t *cfg = app_config_get();
    meter_reading_t r = modbus_meter_read(&cfg->meters[idx], cfg->modbus_retry_count, cfg->modbus_retry_delay_ms);
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"%s\",\"index\":%d,\"unit_id\":\"%s\",\"consumption\":%.2f}",
             r.ok ? "ok" : "error", idx, cfg->meters[idx].unit_id, r.ok ? r.consumption : 0.0);
    return send_json(req, buf);
}

// POST /save_settings  { "telemetry_interval": 300, "maintenance_mode": false }
static esp_err_t h_save_settings(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return send_err(req, "no body");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return send_err(req, "bad json");
    app_config_t *cfg = app_config_get();
    cJSON *it;
    if ((it = cJSON_GetObjectItem(j, "telemetry_interval")) && cJSON_IsNumber(it)) {
        uint32_t v = (uint32_t)it->valuedouble;
        if (v >= TELEMETRY_INTERVAL_MIN_S && v <= TELEMETRY_INTERVAL_MAX_S) cfg->telemetry_interval_s = v;
    }
    if ((it = cJSON_GetObjectItem(j, "maintenance_mode")) && cJSON_IsBool(it))
        cfg->maintenance_mode = cJSON_IsTrue(it);
    cJSON_Delete(j);
    app_config_save();
    return send_ok(req);
}

// POST /ota  { "url": "https://.../aquagen_lite.bin" }  — remote firmware update over the VPN.
// Downloads, flashes the inactive OTA slot, and reboots (rolls back automatically if the new
// image doesn't reach a healthy state). Same teardown as the device-twin OTA path.
static esp_err_t h_ota(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return send_err(req, "no body");
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) return send_err(req, "bad json");
    cJSON *u = cJSON_GetObjectItem(j, "url");
    if (!u || !cJSON_IsString(u) || !u->valuestring[0]) { cJSON_Delete(j); return send_err(req, "no url"); }
    char url[512];
    strlcpy(url, u->valuestring, sizeof(url));
    cJSON_Delete(j);
    send_ok(req);                       // reply before the OTA runs
    ESP_LOGW(TAG, "OTA requested via web: %s", url);
    ota_request(url);                   // queue it — the main task stops MQTT + flashes
    return ESP_OK;
}

// POST /reboot
static esp_err_t h_reboot(httpd_req_t *req)
{
    send_ok(req);
    ESP_LOGW(TAG, "reboot requested via web");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// OPTIONS catch-all for CORS preflight from the app.
static esp_err_t h_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_sendstr(req, "");
}

static void reg(const char *uri, httpd_method_t method, esp_err_t (*fn)(httpd_req_t *))
{
    httpd_uri_t u = { .uri = uri, .method = method, .handler = fn };
    httpd_register_uri_handler(s_server, &u);
}

void web_config_start(void)
{
    if (s_server) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_server, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }

    reg("/", HTTP_GET, h_root);
    reg("/api/system_status", HTTP_GET, h_status);
    reg("/scan_wifi", HTTP_GET, h_scan);
    reg("/save_wifi", HTTP_POST, h_save_wifi);
    reg("/save_azure", HTTP_POST, h_save_azure);
    reg("/save_meters", HTTP_POST, h_save_meters);
    reg("/save_settings", HTTP_POST, h_save_settings);
    reg("/test_meter", HTTP_POST, h_test_meter);
    reg("/ota", HTTP_POST, h_ota);
    reg("/reboot", HTTP_POST, h_reboot);
    reg("/*", HTTP_OPTIONS, h_options);   // CORS preflight
    ESP_LOGI(TAG, "REST API started on :80");
}

void web_config_stop(void)
{
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
}
