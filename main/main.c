/**
 * AquaGen Lite — ESP32 Modbus → Azure IoT flow-meter gateway (ESP-IDF rewrite).
 *
 * CORE MILESTONE (this build): boot state machine, config/NVS, LEDs, WiFi (STA + SoftAP
 * config mode), Modbus meter reads @2400/8E1, factory-reset button. Azure MQTT/telemetry,
 * provisioning (DPS + bake-at-flash), Device Twin, OTA, WireGuard, and the web config API
 * are layered in next — search for "TODO(next)".
 *
 * FW 2.0.0 — see docs/SPEC.md.
 */
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include "esp_sntp.h"

#include "iot_configs.h"
#include "app_config.h"
#include "leds.h"
#include "wifi_mgr.h"
#include "dns_server.h"
#include "modbus_meter.h"
#include "azure_mqtt.h"
#include "telemetry.h"
#include "flash_buffer.h"
#include "provisioning.h"
#include "device_twin.h"
#include "ota.h"
#include "web_config.h"
#include "log_capture.h"
#include "wireguard_client.h"

static const char *TAG = "main";
static bool s_ap_mode = false;
static bool s_config_active = false;            // config web server up while in station mode
static volatile bool s_config_btn = false;      // set by the trigger ISR, handled in the loop

// Runtime config-web-UI request, toggled by the GPIO34 button. The WireGuard "wake" task owns
// port 80, so it (not main) starts/stops the web server based on this flag — avoids a port-80
// race AND avoids reconfiguring WiFi at runtime (bringing up a SoftAP while WiFi+VPN+Azure run
// is what reset the board). Config is reached over the box's LAN/VPN IP, exactly like the gateway.
volatile bool g_web_open_req = false;

static void run_station_mode(void);
static void run_ap_mode(void);

// Config button — wired EXACTLY like the modbus_iot_gateway (same board):
//   GPIO34 = ACTIVE-HIGH: idles LOW (pull-down), pressed = connect to 3.3V (rising edge).
//   GPIO0  = ACTIVE-LOW : idles HIGH (pull-up, BOOT button), pressed = to GND (falling edge).
// (AquaGen previously treated GPIO34 active-low, so on a gateway-wired board the idle-LOW looked
//  like a stuck press → constant config/loop. This matches the gateway, fixing that.)
// A runtime press raises a flag; the station loop toggles the config web server WITHOUT rebooting.
static void IRAM_ATTR trigger_isr(void *arg) { s_config_btn = true; }

// ---- Restart-count diagnostics (ported from gateway) ----
// Counts unhealthy self-recovery reboots so a flaky box is visible remotely (Device Twin).
static uint32_t s_restart_count = 0;

static void load_restart_count(void)
{
    nvs_handle_t h;
    if (nvs_open("aquagen", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "restart_cnt", &s_restart_count);
        nvs_close(h);
    }
}

static void bump_restart_count(void)
{
    s_restart_count++;
    nvs_handle_t h;
    if (nvs_open("aquagen", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "restart_cnt", s_restart_count);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Exposed to device_twin.c for the reported-properties block.
uint32_t app_restart_count(void) { return s_restart_count; }

// ---- Boot-loop protection / SAFE MODE (critical for unattended remote boxes) ----
// A config/data-driven crash that fires every boot would loop forever and brick an un-visitable box.
// We persist a "consecutive unhealthy boots" counter: bumped at every boot, reset only after the box
// has run healthy (MQTT up) for a sustained time. If it exceeds the threshold the box enters SAFE
// MODE — it SKIPS the risky parts (boot meter self-test + Modbus reads, the most likely crash source)
// but KEEPS WiFi + WireGuard + Azure + OTA + the web server up, so it stays fully remotely fixable.
#define BOOT_FAIL_SAFE_THRESHOLD  5
#define BOOT_HEALTHY_AFTER_MS     (3 * 60 * 1000)   // uptime w/ MQTT before clearing the counter
static bool s_safe_mode = false;
bool app_safe_mode(void) { return s_safe_mode; }      // reported to the Device Twin

static uint32_t boot_fail_get(void)
{
    nvs_handle_t h; uint32_t v = 0;
    if (nvs_open("aquagen", NVS_READONLY, &h) == ESP_OK) { nvs_get_u32(h, "boot_fail", &v); nvs_close(h); }
    return v;
}
static void boot_fail_set(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open("aquagen", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u32(h, "boot_fail", v); nvs_commit(h); nvs_close(h); }
}

// ── Software RTC (no battery-backed RTC chip on this HW) ──────────────────────────────────────
// Persist the wall clock to NVS so a power-loss-while-offline reboot doesn't reset time to 1970.
// On boot we restore it (near-correct), NTP later corrects it exactly, and any offline records
// stamped before that correction are retimed by the constant offset (clock_check_sync).
#define TIME_VALID_FLOOR  1735689600LL   // 2025-01-01 UTC; epoch below this = clock not real yet
static uint32_t s_boot_id    = 0;
static int64_t  s_prov_base  = 0;        // restored epoch anchored to uptime 0 (0 = none restored)
static bool     s_clock_auth = false;    // is the wall clock NTP-authoritative this boot?

static uint32_t boot_id_next(void)
{
    nvs_handle_t h; uint32_t v = 0;
    if (nvs_open("aquagen", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "boot_seq", &v); v++;
        nvs_set_u32(h, "boot_seq", v); nvs_commit(h); nvs_close(h);
    }
    return v;
}

static void clock_save(void)
{
    if ((int64_t)time(NULL) < TIME_VALID_FLOOR) return;   // never persist a bogus clock
    nvs_handle_t h;
    if (nvs_open("aquagen", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u64(h, "rtc_epoch", (uint64_t)time(NULL)); nvs_commit(h); nvs_close(h);
    }
}

static void clock_restore(void)
{
    nvs_handle_t h; uint64_t e = 0;
    if (nvs_open("aquagen", NVS_READONLY, &h) == ESP_OK) { nvs_get_u64(h, "rtc_epoch", &e); nvs_close(h); }
    if ((int64_t)e > TIME_VALID_FLOOR) {
        struct timeval tv = { .tv_sec = (time_t)e, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        s_prov_base = (int64_t)e - esp_timer_get_time() / 1000000;   // epoch at uptime 0
        ESP_LOGW(TAG, "software RTC restored: %lld (provisional until NTP)", (long long)e);
    }
}

// Detect the provisional→authoritative transition. On it, retime this boot's buffered records by
// the constant offset (a software-RTC clock runs at the right rate, just shifted by the downtime).
static void clock_check_sync(void)
{
    if (s_clock_auth) return;
    if ((int64_t)time(NULL) <= TIME_VALID_FLOOR) return;   // still no real time
    int64_t up = esp_timer_get_time() / 1000000, real = (int64_t)time(NULL);
    if (s_prov_base > 0) {
        int64_t delta = real - (s_prov_base + up);   // same offset for every provisional record
        if (delta != 0) flash_buffer_retime(s_boot_id, delta);
    }
    s_clock_auth = true;
    telemetry_set_clock(true, s_boot_id);
    clock_save();
    ESP_LOGW(TAG, "clock authoritative (%lld) — offline records retimed", (long long)real);
}

// ---- OTA request queue ----
// The Device Twin handler runs in the MQTT task, and the /ota web handler in the httpd task.
// Neither may call azure_mqtt_stop() (esp_mqtt_client_stop) — stopping the client from the MQTT
// task asserts/crashes, and that crash, with the trigger still latched in the twin, caused a
// reboot loop. So those callers just QUEUE the url here; the station loop performs the OTA from
// the MAIN task, where stopping MQTT is safe.
static char s_pending_ota_url[256];
static volatile bool s_ota_pending = false;

void ota_request(const char *url)
{
    if (!url || !url[0]) return;
    strlcpy(s_pending_ota_url, url, sizeof(s_pending_ota_url));
    s_ota_pending = true;
}

static void setup_trigger_interrupt(void)
{
    // GPIO34 = ACTIVE-HIGH, exactly like the gateway: idle LOW (pull-down), press = rising edge.
    // (GPIO34 is input-only; the internal pull-down is a no-op but the board supplies it, same as
    // the gateway. The matching active-high read in config_button_pressed() is what fixes the bug.)
    gpio_config_t io34 = {
        .intr_type    = GPIO_INTR_POSEDGE,      // press = rising edge (connect to 3.3V)
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_TRIGGER),
        .pull_up_en   = 0,
        .pull_down_en = 1,
    };
    gpio_config(&io34);

    // GPIO0/BOOT = ACTIVE-LOW, like the gateway: internal pull-up (idle HIGH), press = falling edge.
    gpio_config_t io0 = {
        .intr_type    = GPIO_INTR_NEGEDGE,
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_TRIGGER_ALT),
        .pull_up_en   = 1,
        .pull_down_en = 0,
    };
    gpio_config(&io0);

    static bool isr_installed = false;
    if (!isr_installed) { gpio_install_isr_service(0); isr_installed = true; }
    gpio_isr_handler_add(PIN_TRIGGER, trigger_isr, NULL);
    gpio_isr_handler_add(PIN_TRIGGER_ALT, trigger_isr, NULL);
    ESP_LOGI(TAG, "config button armed: GPIO%d active-HIGH (to 3.3V) + GPIO%d BOOT active-LOW — gateway-style",
             PIN_TRIGGER, PIN_TRIGGER_ALT);
}

static void sync_time(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER_1);
    esp_sntp_setservername(1, NTP_SERVER_2);
    esp_sntp_init();
    // wait up to ~20 s for a valid clock (SAS tokens need real time)
    for (int i = 0; i < 40 && time(NULL) < 1700000000; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "time synced: %lld", (long long)time(NULL));
}

__attribute__((unused)) static void check_factory_reset(void)
{
    // Hold the trigger pin LOW for FACTORY_RESET_HOLD_MS at boot → wipe everything.
    // GPIO34 is input-only with NO internal pull (the board supplies an external pull-up),
    // so we must NOT call gpio_set_pull_mode here (it logs a GPIO-number error and does nothing).
    gpio_set_direction(PIN_TRIGGER, GPIO_MODE_INPUT);
    if (gpio_get_level(PIN_TRIGGER) != 0) return;          // not held → normal boot

    ESP_LOGW(TAG, "trigger held at boot — timing for factory reset...");
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(PIN_TRIGGER) == 0) {
        if (esp_timer_get_time() - start > (int64_t)FACTORY_RESET_HOLD_MS * 1000) {
            led_blink(LED_RED, 80, 10);
            app_config_factory_reset();
            ESP_LOGW(TAG, "factory reset done — rebooting");
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // released before threshold → treat as a normal config-mode request (short press)
}

static void run_station_mode(void)
{
    app_config_t *cfg = app_config_get();
    leds_state_wifi_ok();

    s_boot_id = boot_id_next();
    clock_restore();             // seed the clock from NVS so we're never at 1970 if NTP can't run
    sync_time();
    telemetry_init();            // mounts the LittleFS offline buffer
    telemetry_set_clock(s_clock_auth, s_boot_id);   // provisional until NTP confirmed below
    clock_check_sync();          // if NTP synced at boot → mark authoritative + retime any backlog

    // WireGuard (optional remote access) — disabled by default; crashes on esp_wireguard 0.9.0 +
    // IDF 5.5. Telemetry/config don't need it. See ENABLE_WIREGUARD in iot_configs.h.
#if ENABLE_WIREGUARD
    wireguard_setup();
    wireguard_start_keepalive_task();
#endif

    // Resolve device identity: baked-at-flash if present, else Azure DPS self-register.
    if (!provisioning_ensure(PROVISION_AUTO)) {
        ESP_LOGE(TAG, "no device identity — cannot reach Azure; falling back to config mode");
        run_ap_mode();           // never returns
    }
    azure_mqtt_start();          // SAS token + MQTTS:8883
    device_twin_init();          // register twin handler on the C2D callback

    // Wait up to ~10 s for the first MQTT connection so the FIRST reading publishes live
    // instead of going through the offline buffer. The buffer then only kicks in for a real
    // drop (WiFi/internet loss), which is what it's for.
    for (int i = 0; i < 50 && !azure_mqtt_is_connected(); i++) vTaskDelay(pdMS_TO_TICKS(200));
    if (azure_mqtt_is_connected())
        ESP_LOGI(TAG, "MQTT connected — first reading will publish live");
    else
        ESP_LOGW(TAG, "MQTT not up yet — first reading will buffer until it connects");

    ESP_LOGI(TAG, "STATION MODE — telemetry every %lu s", (unsigned long)cfg->telemetry_interval_s);

    esp_task_wdt_add(NULL);
    int64_t next_send = 0;
    bool was_mqtt = false;
    int64_t boot_ms   = esp_timer_get_time() / 1000;
    int64_t last_mqtt_ok = boot_ms;   // for connectivity auto-recovery
    int64_t last_ntp     = boot_ms;   // for daily clock re-sync
    int64_t last_clk_save = boot_ms;  // for periodic software-RTC persistence
    int     heap_low_streak   = 0;    // consecutive low-heap checks (low-memory safety net)
    int     mb_consec_fail    = 0;    // consecutive all-failed Modbus read cycles (self-heal)
    int     mb_reinit_count   = 0;    // RS485 driver re-inits since the last good read
    int64_t config_open_ms    = 0;    // when the config web UI was opened (for idle auto-close)
    bool    prev_web_req      = false;
    int64_t last_btn_ms       = 0;    // last config-button toggle (edge debounce)
    bool    boot_marked_healthy = false;  // boot-loop counter cleared once sustained-healthy

    while (1) {
        int64_t now = esp_timer_get_time() / 1000;

        // Queued OTA (from the Device Twin or /ota web handler, which run in other tasks; SAFE MODE
        // still allows OTA — it's how a remote box gets repaired). ota_start() runs the download in
        // its OWN background task: it reboots the chip on success, or just ends on failure. We keep
        // MQTT UP throughout (≈150 KB free, OTA needs 40 KB) so a FAILED OTA never takes the box
        // offline — telemetry keeps flowing. No esp_mqtt_client_stop() here (that must not run from
        // the MQTT/httpd task anyway — hence the queue).
        if (s_ota_pending) {
            s_ota_pending = false;
            ESP_LOGW(TAG, "starting OTA (background task): %s", s_pending_ota_url);
            device_twin_report();           // report current ota_status first
            ota_start(s_pending_ota_url);
        }

        // Config button — EXACTLY the modbus_iot_gateway model: PURE EDGE-TRIGGERED. The ISR sets
        // s_config_btn on a press edge (GPIO34 rising / GPIO0 falling); we toggle ONCE per edge and
        // never look at the pin LEVEL. A pin that is permanently tied (e.g. GPIO34 held at 3.3V)
        // produces NO edges → NO toggling → no loop. (The old level-polling toggled forever while a
        // pin was held — that was the loop.) A short time-gate ignores contact bounce.
        if (s_config_btn) {
            s_config_btn = false;
            if (now - last_btn_ms >= 400) {       // debounce bounce; real presses are seconds apart
                last_btn_ms = now;
                s_config_active = !s_config_active;
                g_web_open_req = s_config_active;  // wake task starts/stops the web server (no reboot)
                if (s_config_active) {
                    ESP_LOGW(TAG, "config button — opening config web UI (reach it on the box's LAN/VPN IP)");
                    leds_state_ap_mode();
                    led_set(LED_BLUE, true);
                } else {
                    ESP_LOGW(TAG, "config button — closing config web UI");
                    led_set(LED_BLUE, false);
                    leds_state_wifi_ok();
                }
            }
        }

        // Low-heap safety net — proactively self-heal before a hard out-of-memory crash.
        if (esp_get_free_heap_size() < HEAP_CRITICAL_BYTES) {
            if (++heap_low_streak >= HEAP_LOW_STREAK) {
                if (s_config_active) {
                    ESP_LOGE(TAG, "heap critical (%lu) — closing config web server to recover",
                             (unsigned long)esp_get_free_heap_size());
                    g_web_open_req = false;   // wake task stops the server
                    led_set(LED_BLUE, false);
                    leds_state_wifi_ok();
                    s_config_active = false;
                } else {
                    ESP_LOGE(TAG, "heap critical (%lu) — self-recovery reboot (#%lu)",
                             (unsigned long)esp_get_free_heap_size(),
                             (unsigned long)(s_restart_count + 1));
                    bump_restart_count();
                    esp_restart();
                }
                heap_low_streak = 0;
            }
        } else {
            heap_low_streak = 0;
        }

        // Config web UI idle auto-close: whether opened by the button or the Device Twin, close it
        // after CONFIG_MODE_IDLE_TIMEOUT_MS so a forgotten-open session doesn't hold ~64 KB forever.
        if (g_web_open_req && !prev_web_req) config_open_ms = now;   // just opened
        prev_web_req = g_web_open_req;
        if (g_web_open_req && now - config_open_ms >= CONFIG_MODE_IDLE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "config web UI open %d min — auto-closing", CONFIG_MODE_IDLE_TIMEOUT_MS / 60000);
            g_web_open_req = false;
            s_config_active = false;
            led_set(LED_BLUE, false);
            leds_state_wifi_ok();
        }

        if (!wifi_mgr_is_connected()) {
            ESP_LOGW(TAG, "WiFi dropped — reconnecting");
            leds_state_mqtt_ok(false);
            leds_state_error();
            esp_task_wdt_reset();
            // Cap one attempt under the 30 s task-WDT; the loop retries on the next pass.
            wifi_mgr_connect_sta(cfg->wifi_ssid, cfg->wifi_password, 25000);
            esp_task_wdt_reset();
            if (wifi_mgr_is_connected()) { leds_state_wifi_ok(); azure_mqtt_start(); }
        }

        // SAS token renewal
        if (azure_mqtt_sas_expiring()) {
            ESP_LOGI(TAG, "SAS expiring — restarting MQTT");
            azure_mqtt_start();
        }

        bool mqtt = azure_mqtt_is_connected();
        leds_state_mqtt_ok(mqtt);
        if (mqtt && !was_mqtt) {
            ota_mark_valid_if_pending();  // healthy boot reached → cancel OTA rollback
            device_twin_request();        // fetch desired props
            device_twin_report();         // report current state
        }
        was_mqtt = mqtt;
        if (mqtt) {
            last_mqtt_ok = now;
            // Drain buffered readings whenever online — OLDEST-FIRST (chronological), exactly like
            // the gateway. Runs every loop (cheap no-op if empty) so records buffered while online,
            // or a replay that stalled on a transient Azure error, get sent promptly — not only on
            // the next reconnect. flash_buffer replays top-of-file (oldest) → newest; each record
            // also carries its real created_on timestamp.
            telemetry_flush_buffer();
        }

        // Boot-loop protection: once we've been healthy (MQTT up) for a sustained window, clear the
        // consecutive-unhealthy-boot counter — this boot is proven good, so a future crash starts
        // counting fresh and a healthy box never trips safe mode.
        if (!boot_marked_healthy && mqtt && (now - boot_ms) >= BOOT_HEALTHY_AFTER_MS) {
            boot_fail_set(0);
            boot_marked_healthy = true;
            ESP_LOGI(TAG, "boot confirmed healthy — boot-loop counter cleared");
        }

        // Daily NTP re-sync — clock drift would eventually invalidate SAS tokens.
        if (now - last_ntp >= (int64_t)NTP_RESYNC_INTERVAL_S * 1000) {
            ESP_LOGI(TAG, "periodic NTP re-sync");
            esp_sntp_restart();
            last_ntp = now;
        }

        // Software RTC: if NTP only synced now (booted offline), mark authoritative + retime the
        // backlog; and persist the clock every ~5 min so a power-loss reboot restores recent time.
        clock_check_sync();
        if (s_clock_auth && now - last_clk_save >= 300000) { clock_save(); last_clk_save = now; }

        // Connectivity auto-recovery — MQTT stuck-down this long → reboot to self-heal.
        // (Keyed on MQTT, NOT on meter reads: a reboot fixes a wedged WiFi/MQTT stack but
        // can't fix an unplugged meter, so a faulty meter must never cause a reboot loop.)
        if (now - last_mqtt_ok >= (int64_t)MQTT_RECOVERY_TIMEOUT_S * 1000) {
            ESP_LOGE(TAG, "MQTT down >%d s — self-recovery reboot (#%lu)",
                     MQTT_RECOVERY_TIMEOUT_S, (unsigned long)(s_restart_count + 1));
            bump_restart_count();
            esp_restart();
        }

        // Skip the Modbus path entirely in safe mode (likely repeating-crash source) — the box
        // stays online + remotely fixable; resume normal reads after a remote repair + reboot.
        if (!s_safe_mode && !cfg->maintenance_mode && now >= next_send) {
            bool any_attempt = false, any_ok = false;
            for (int i = 0; i < MAX_METERS; i++) {
                meter_cfg_t *m = &cfg->meters[i];
                if (!m->enabled) continue;
                any_attempt = true;
                meter_reading_t r = modbus_meter_read(m, cfg->modbus_retry_count, cfg->modbus_retry_delay_ms);
                if (r.ok) {
                    any_ok = true;
                    telemetry_send(m, &r);   // publishes live, or buffers to flash if offline
                } else {
                    led_blink(LED_RED, 50, 2);
                }
            }
            // Modbus self-heal: per-read retries can't fix a wedged RS485/UART. After N consecutive
            // all-failed cycles, re-init the driver; if it stays dead across several re-inits, reboot.
            if (any_attempt && !any_ok) {
                if (++mb_consec_fail % MODBUS_FAIL_REINIT == 0) {
                    if (++mb_reinit_count >= MODBUS_REINIT_MAX_REBOOT) {
                        ESP_LOGE(TAG, "Modbus dead after %d re-inits — self-recovery reboot (#%lu)",
                                 mb_reinit_count, (unsigned long)(s_restart_count + 1));
                        bump_restart_count();
                        esp_restart();
                    }
                    ESP_LOGW(TAG, "Modbus: %d consecutive failed reads — re-initializing RS485 driver",
                             mb_consec_fail);
                    modbus_meter_reinit();
                }
            } else if (any_ok) {
                mb_consec_fail = 0;
                mb_reinit_count = 0;
            }
            next_send = now + (int64_t)cfg->telemetry_interval_s * 1000;
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

static void run_ap_mode(void)
{
    app_config_t *cfg = app_config_get();
    s_ap_mode = true;
    leds_state_ap_mode();
    led_set(LED_BLUE, true);  // Red+Blue = config mode (per SOP LED table)

    wifi_mgr_start_ap(cfg->ap_ssid, cfg->ap_password);   // per-box SSID (e.g. Gravity_water_01)
    web_config_start();          // REST API for the Flutter app (§18 endpoint contract)
    dns_server_start();          // captive portal: keep the phone from evicting this no-internet AP
    ESP_LOGI(TAG, "CONFIG MODE — join '%s', open the app to configure", cfg->ap_ssid);

    int64_t start = esp_timer_get_time();
    while (1) {
        if (esp_timer_get_time() - start > (int64_t)AP_SESSION_TIMEOUT_MS * 1000) {
            ESP_LOGI(TAG, "AP timeout — restarting to retry station mode");
            esp_restart();
        }

        // NOTE: the config button is intentionally NOT read here. GPIO34 is input-only with no
        // internal pull-up; if the board lacks an external pull-up it floats LOW, which previously
        // looked like a held button and rebooted in a loop. In config mode the operator finishes
        // via the app ("Save & reboot to normal mode"); the physical button is only used at runtime
        // (station mode) to toggle the config web server.
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    log_capture_init();   // mirror all ESP_LOG into a RAM ring → pullable via GET /logs over VPN

    // NVS init (required by config + wifi).
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    load_restart_count();

    // Boot-loop protection: bump the consecutive-unhealthy-boot counter now; it's cleared later
    // once we've run healthy for a few minutes (run_station_mode). If it's too high, a crash is
    // looping every boot → enter SAFE MODE (skip the risky meter path, keep remote access alive).
    uint32_t boot_fail = boot_fail_get() + 1;
    boot_fail_set(boot_fail);
    s_safe_mode = (boot_fail > BOOT_FAIL_SAFE_THRESHOLD);

    ESP_LOGI(TAG, "===== AquaGen Lite %s booting (recovery reboots: %lu, boot_fail: %lu%s) =====",
             FW_VERSION_STRING, (unsigned long)s_restart_count, (unsigned long)boot_fail,
             s_safe_mode ? " — SAFE MODE: meter disabled, remote access only" : "");

    leds_init();
    led_blink(LED_BLUE, 50, 2);

    // NOTE: boot-time factory reset (hold trigger 10 s) REMOVED — it wiped the baked Azure
    // identity + WiFi, and GPIO34 (input-only, external pull-up) could trigger it accidentally
    // on a marginal connection → unacceptable for unattended boxes. The trigger now ONLY opens
    // config mode (boot short-press below, or the runtime ISR). A real reset = re-flash identity.
    app_config_load();

    // Factory-line METER SELF-TEST: read the meter once at boot and print PASS/FAIL on serial,
    // so right after flashing you instantly see if RS485 is wired correctly — no WiFi/app needed.
    // SKIPPED in safe mode: a bad meter config is the most likely repeating-crash source, so we
    // don't touch the Modbus path until the box has been remotely repaired.
    if (!s_safe_mode) {
        app_config_t *mc = app_config_get();
        if (mc->meters[0].enabled) {
            meter_reading_t r = modbus_meter_read(&mc->meters[0], 2, 100);
            if (r.ok)
                ESP_LOGI(TAG, "==== METER SELF-TEST: slave %u -> %.2f  ✅ PASS ====",
                         mc->meters[0].slave_id, r.consumption);
            else
                ESP_LOGE(TAG, "==== METER SELF-TEST: slave %u  ❌ NO RESPONSE — check RS485 A/B, GND, meter power/address ====",
                         mc->meters[0].slave_id);
        }
    }

    wifi_mgr_init();
    setup_trigger_interrupt();        // runtime press → config mode (gateway behaviour)

    app_config_t *cfg = app_config_get();

    // Config mode ONLY when there's no WiFi to run with (fresh box → operator onboards via SoftAP).
    // We deliberately do NOT read GPIO34 at boot: it's input-only with no internal pull-up, so a
    // floating pin would spuriously force config mode. A configured box ALWAYS boots station and
    // keeps retrying (matches modbus_iot_gateway); config is entered at runtime via the button.
    if (!app_config_has_wifi()) {
        run_ap_mode();               // never returns
    }

    // Best-effort initial connect; whether or not it succeeds we proceed to station mode, whose
    // loop owns (re)connection + self-recovery reboot. A connect failure here is NOT config mode.
    if (!wifi_mgr_connect_sta(cfg->wifi_ssid, cfg->wifi_password, 60000))
        ESP_LOGW(TAG, "initial WiFi connect failed — entering station mode anyway; loop will retry");
    run_station_mode();              // never returns
}
