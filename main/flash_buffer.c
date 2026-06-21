#include "flash_buffer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "flashbuf";
static const char *MOUNT = "/lfs";
static const char *LOG_PATH = "/lfs/tele.ndjson";

// Keep the log under this size; when exceeded we drop the oldest records.
#define BUFFER_MAX_BYTES   (200 * 1024)   // fits the 256 KB littlefs partition with headroom
#define TRIM_TARGET_BYTES  (120 * 1024)   // after trim, keep ~this much (newest)

static bool s_mounted = false;

void flash_buffer_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = MOUNT,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        s_mounted = false;
        return;
    }
    s_mounted = true;
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    ESP_LOGI(TAG, "littlefs mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
}

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

// Drop oldest records: keep only the last TRIM_TARGET_BYTES (aligned to a line boundary).
static void trim_oldest(void)
{
    long sz = file_size(LOG_PATH);
    if (sz <= TRIM_TARGET_BYTES) return;

    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) return;
    fseek(f, sz - TRIM_TARGET_BYTES, SEEK_SET);

    // advance to the next newline so we don't keep a partial first record
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') { }

    char *buf = malloc(TRIM_TARGET_BYTES);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, TRIM_TARGET_BYTES, f);
    fclose(f);

    FILE *w = fopen(LOG_PATH, "wb");
    if (w) { fwrite(buf, 1, n, w); fclose(w); }
    free(buf);
    ESP_LOGW(TAG, "buffer trimmed (was %ld bytes)", sz);
}

bool flash_buffer_push(const char *json)
{
    if (!s_mounted || !json) return false;
    if (file_size(LOG_PATH) > BUFFER_MAX_BYTES) trim_oldest();

    FILE *f = fopen(LOG_PATH, "ab");
    if (!f) { ESP_LOGE(TAG, "open for append failed"); return false; }
    fputs(json, f);
    fputc('\n', f);
    fclose(f);
    return true;
}

bool flash_buffer_push_provisional(const char *json, unsigned boot_id, long long created_epoch)
{
    if (!s_mounted || !json) return false;
    if (file_size(LOG_PATH) > BUFFER_MAX_BYTES) trim_oldest();
    FILE *f = fopen(LOG_PATH, "ab");
    if (!f) { ESP_LOGE(TAG, "open for append failed"); return false; }
    // record + TAB-separated sidecar (boot_id, provisional epoch). The sidecar is stripped before
    // publishing and consumed by flash_buffer_retime(); Azure never sees it.
    fprintf(f, "%s\t%u\t%lld\n", json, boot_id, created_epoch);
    fclose(f);
    return true;
}

void flash_buffer_retime(unsigned boot_id, long long delta_sec)
{
    if (!s_mounted) return;
    FILE *in = fopen(LOG_PATH, "rb");
    if (!in) return;
    const char *TMP = "/lfs/tele.tmp";
    FILE *out = fopen(TMP, "wb");
    if (!out) { fclose(in); return; }

    char line[512];
    unsigned fixed = 0;
    while (fgets(line, sizeof(line), in)) {
        size_t len = strlen(line);
        if (len && line[len - 1] == '\n') { line[len - 1] = '\0'; }
        char *tab = strchr(line, '\t');     // sidecar present → provisional record
        if (tab) {
            *tab = '\0';                     // line now = pure json
            unsigned b = 0; long long ep = 0;
            if (sscanf(tab + 1, "%u\t%lld", &b, &ep) == 2 && b == boot_id) {
                time_t t = (time_t)(ep + delta_sec);
                struct tm tm; gmtime_r(&t, &tm);
                char iso[24];
                strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);  // fixed 20 chars
                char *p = strstr(line, "\"created_on\":\"");
                if (p && strlen(iso) == 20) { memcpy(p + 14, iso, 20); }  // in-place, same width
                fixed++;
            }
            // (records from other boots: sidecar dropped here, json kept with its existing time)
        }
        fputs(line, out);
        fputc('\n', out);
    }
    fclose(in);
    fclose(out);
    remove(LOG_PATH);
    rename(TMP, LOG_PATH);
    if (fixed) ESP_LOGW(TAG, "retimed %u provisional record(s) by %+lld s", fixed, delta_sec);
}

bool flash_buffer_is_empty(void)
{
    return file_size(LOG_PATH) == 0;
}

size_t flash_buffer_count(void)
{
    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) return 0;
    size_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') n++;
    fclose(f);
    return n;
}

size_t flash_buffer_replay(flash_buffer_send_fn send, void *ctx)
{
    if (!s_mounted) return 0;
    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) return 0;

    char line[512];
    size_t sent = 0;
    long resume_at = 0;          // byte offset of the first NOT-yet-sent record
    bool stalled = false;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') { resume_at = ftell(f); continue; }
        char *tab = strchr(line, '\t');   // strip provisional sidecar — publish pure json only
        if (tab) *tab = '\0';

        if (send(line, ctx)) {
            sent++;
            resume_at = ftell(f);
        } else {
            stalled = true;       // stop at first failure; keep this + the rest
            break;
        }
    }

    if (!stalled) {
        // all sent → clear the log
        fclose(f);
        remove(LOG_PATH);
    } else {
        // rewrite the file with only the un-sent tail
        fseek(f, resume_at, SEEK_SET);
        char *rest = malloc(BUFFER_MAX_BYTES);
        if (rest) {
            size_t n = fread(rest, 1, BUFFER_MAX_BYTES, f);
            fclose(f);
            FILE *w = fopen(LOG_PATH, "wb");
            if (w) { fwrite(rest, 1, n, w); fclose(w); }
            free(rest);
        } else {
            fclose(f);
        }
    }
    if (sent) ESP_LOGI(TAG, "replayed %u buffered records", (unsigned)sent);
    return sent;
}
