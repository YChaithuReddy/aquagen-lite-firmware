#include "app_config.h"
#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "app_config";
static const char *NS = "aquagen";

static app_config_t s_cfg;

app_config_t *app_config_get(void) { return &s_cfg; }

static void apply_defaults(void)
{
    if (s_cfg.iothub_fqdn[0] == '\0')
        strlcpy(s_cfg.iothub_fqdn, DEFAULT_IOTHUB_FQDN, sizeof(s_cfg.iothub_fqdn));
    if (s_cfg.ap_ssid[0] == '\0')
        strlcpy(s_cfg.ap_ssid, AP_SSID_DEFAULT, sizeof(s_cfg.ap_ssid));
    if (s_cfg.ap_password[0] == '\0')
        strlcpy(s_cfg.ap_password, AP_PASSWORD_FALLBACK, sizeof(s_cfg.ap_password));
    if (s_cfg.telemetry_interval_s == 0)
        s_cfg.telemetry_interval_s = DEFAULT_TELEMETRY_INTERVAL_S;
    if (s_cfg.modbus_retry_count == 0)
        s_cfg.modbus_retry_count = MODBUS_READ_RETRIES;
    if (s_cfg.modbus_retry_delay_ms == 0)
        s_cfg.modbus_retry_delay_ms = MODBUS_RETRY_DELAY_MS;

    for (int i = 0; i < MAX_METERS; i++) {
        meter_cfg_t *m = &s_cfg.meters[i];
        if (m->slave_id == 0) m->slave_id = i + 1;
        if (m->baud == 0)     m->baud = DEFAULT_MODBUS_BAUD;
        if (m->parity[0] == '\0') strlcpy(m->parity, DEFAULT_MODBUS_PARITY, sizeof(m->parity));
        if (m->unit_id[0] == '\0') snprintf(m->unit_id, sizeof(m->unit_id), "Unit_%d", i);
    }
}

// Helper: read a string key into dst (leaves dst untouched if key missing).
static void get_str(nvs_handle_t h, const char *key, char *dst, size_t len)
{
    size_t l = len;
    if (nvs_get_str(h, key, dst, &l) != ESP_OK) dst[0] = '\0';
}

void app_config_load(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "device_id",   s_cfg.device_id,    sizeof(s_cfg.device_id));
        get_str(h, "device_key",  s_cfg.device_key,   sizeof(s_cfg.device_key));
        get_str(h, "iothub",      s_cfg.iothub_fqdn,  sizeof(s_cfg.iothub_fqdn));
        get_str(h, "ap_ssid",     s_cfg.ap_ssid,      sizeof(s_cfg.ap_ssid));
        get_str(h, "ap_pw",       s_cfg.ap_password,  sizeof(s_cfg.ap_password));
        get_str(h, "wifi_ssid",   s_cfg.wifi_ssid,    sizeof(s_cfg.wifi_ssid));
        get_str(h, "wifi_pw",     s_cfg.wifi_password,sizeof(s_cfg.wifi_password));
        get_str(h, "site_name",   s_cfg.site_name,    sizeof(s_cfg.site_name));
        get_str(h, "site_gps",    s_cfg.site_gps,     sizeof(s_cfg.site_gps));

        nvs_get_u32(h, "tel_int",  &s_cfg.telemetry_interval_s);
        nvs_get_u8 (h, "mb_retry", &s_cfg.modbus_retry_count);
        nvs_get_u16(h, "mb_delay", &s_cfg.modbus_retry_delay_ms);
        uint8_t b = 0;
        if (nvs_get_u8(h, "maint", &b) == ESP_OK) s_cfg.maintenance_mode = b;
        b = 1;
        if (nvs_get_u8(h, "ota_en", &b) == ESP_OK) s_cfg.ota_enabled = b; else s_cfg.ota_enabled = true;

        for (int i = 0; i < MAX_METERS; i++) {
            char key[16];
            meter_cfg_t *m = &s_cfg.meters[i];
            snprintf(key, sizeof(key), "m%d_unit", i);   get_str(h, key, m->unit_id, sizeof(m->unit_id));
            snprintf(key, sizeof(key), "m%d_ser", i);    get_str(h, key, m->serial,  sizeof(m->serial));
            snprintf(key, sizeof(key), "m%d_par", i);    get_str(h, key, m->parity,  sizeof(m->parity));
            snprintf(key, sizeof(key), "m%d_sid", i);    nvs_get_u8 (h, key, &m->slave_id);
            snprintf(key, sizeof(key), "m%d_baud", i);   nvs_get_u32(h, key, &m->baud);
            // One meter per box: meter 0 enabled by default, meters 1-2 off (override via app).
            snprintf(key, sizeof(key), "m%d_en", i);     { uint8_t e = (i == 0) ? 1 : 0; if (nvs_get_u8(h, key, &e) == ESP_OK) m->enabled = e; else m->enabled = (i == 0); }
        }
        nvs_close(h);
        ESP_LOGI(TAG, "config loaded (device_id='%s', wifi='%s')", s_cfg.device_id, s_cfg.wifi_ssid);
    } else {
        ESP_LOGW(TAG, "no config in NVS — using defaults");
    }
    apply_defaults();
}

void app_config_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) { ESP_LOGE(TAG, "nvs_open failed"); return; }

    nvs_set_str(h, "device_id",  s_cfg.device_id);
    nvs_set_str(h, "device_key", s_cfg.device_key);
    nvs_set_str(h, "iothub",     s_cfg.iothub_fqdn);
    nvs_set_str(h, "ap_ssid",    s_cfg.ap_ssid);
    nvs_set_str(h, "ap_pw",      s_cfg.ap_password);
    nvs_set_str(h, "wifi_ssid",  s_cfg.wifi_ssid);
    nvs_set_str(h, "wifi_pw",    s_cfg.wifi_password);
    nvs_set_str(h, "site_name",  s_cfg.site_name);
    nvs_set_str(h, "site_gps",   s_cfg.site_gps);
    nvs_set_u32(h, "tel_int",    s_cfg.telemetry_interval_s);
    nvs_set_u8 (h, "mb_retry",   s_cfg.modbus_retry_count);
    nvs_set_u16(h, "mb_delay",   s_cfg.modbus_retry_delay_ms);
    nvs_set_u8 (h, "maint",      s_cfg.maintenance_mode ? 1 : 0);
    nvs_set_u8 (h, "ota_en",     s_cfg.ota_enabled ? 1 : 0);

    for (int i = 0; i < MAX_METERS; i++) {
        char key[16];
        meter_cfg_t *m = &s_cfg.meters[i];
        snprintf(key, sizeof(key), "m%d_unit", i);  nvs_set_str(h, key, m->unit_id);
        snprintf(key, sizeof(key), "m%d_ser", i);   nvs_set_str(h, key, m->serial);
        snprintf(key, sizeof(key), "m%d_par", i);   nvs_set_str(h, key, m->parity);
        snprintf(key, sizeof(key), "m%d_sid", i);   nvs_set_u8 (h, key, m->slave_id);
        snprintf(key, sizeof(key), "m%d_baud", i);  nvs_set_u32(h, key, m->baud);
        snprintf(key, sizeof(key), "m%d_en", i);    nvs_set_u8 (h, key, m->enabled ? 1 : 0);
    }

    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "config saved (%s)", esp_err_to_name(err));
}

bool app_config_has_wifi(void)
{
    return s_cfg.wifi_ssid[0] != '\0';
}

bool app_config_has_identity(void)
{
    // device_id present (bake-at-flash / manual), OR DPS will assign at runtime.
    return s_cfg.device_id[0] != '\0';
}

void app_config_clear_network_keep_identity(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "wifi_ssid");
    nvs_erase_key(h, "wifi_pw");
    nvs_commit(h);
    nvs_close(h);
    s_cfg.wifi_ssid[0] = '\0';
    s_cfg.wifi_password[0] = '\0';
    ESP_LOGI(TAG, "network creds cleared; identity kept (ship-ready)");
}

void app_config_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "FACTORY RESET — all config erased");
}
