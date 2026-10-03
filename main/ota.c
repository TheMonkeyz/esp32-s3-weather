// Over-the-air updates (see ota.h).
// Safety: the bootloader has rollback enabled. A freshly installed image starts "pending verify"; if it doesn't
// run for 60 s (crash, boot loop), the bootloader goes back to the previous one. The download is checked by
// esp_https_ota (image header, SHA-256) and we also require the same project name before switching.
#include "ota.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "http_once.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "nvs.h"
#include "cJSON.h"
#include "net.h"
#include "svc.h"
#include "i18n.h"
#include "esp_timer.h"

static const char *TAG = "ota";
#define CHECK_EVERY_S (6 * 3600)
#define VALID_AFTER_S 60

static ota_status_t st;
static ota_listener_t listener;
static SemaphoreHandle_t mux;
static TaskHandle_t task;
static char app_url[256];
#define NOTES_MAX 3072
static char *notes;                 // PSRAM, NOTES_MAX, under mux
static volatile bool want_check, want_install;

static void publish(void)
{
    ota_status_t copy;
    xSemaphoreTake(mux, portMAX_DELAY);
    copy = st;
    xSemaphoreGive(mux);
    if (listener) listener(&copy);
}

static void set_state(ota_state_t s, const char *err)
{
    xSemaphoreTake(mux, portMAX_DELAY);
    st.state = s;
    if (err) strlcpy(st.error, err, sizeof(st.error));
    xSemaphoreGive(mux);
    publish();
}

/* ---------- versions: vMAJOR.MINOR.PATCH[-rc.N | -N-gHASH (git describe) | -anything] ---------- */

typedef struct { int v[3]; int kind; int n; } ver_t;   // kind: 0 pre-release, 1 release, 2 dev build after it

static bool parse_ver(const char *s, ver_t *o)
{
    memset(o, 0, sizeof(*o));
    if (*s == 'v' || *s == 'V') s++;
    char *end;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s)) return false;
        o->v[i] = strtol(s, &end, 10);
        s = end;
        if (i < 2) { if (*s != '.') return false; s++; }
    }
    if (!*s) { o->kind = 1; return true; }
    if (*s != '-') return false;
    s++;
    if (!strncmp(s, "rc.", 3)) { o->kind = 0; o->n = atoi(s + 3); return true; }
    if (isdigit((unsigned char)*s) && strstr(s, "-g")) { o->kind = 2; o->n = atoi(s); return true; }
    o->kind = 0;                                   // other suffixes (test builds) count as pre-releases
    o->n = -1;
    return true;
}

static int cmp_ver(const ver_t *a, const ver_t *b)
{
    for (int i = 0; i < 3; i++) if (a->v[i] != b->v[i]) return a->v[i] - b->v[i];
    if (a->kind != b->kind) return a->kind - b->kind;
    return a->n - b->n;
}

/* ---------- HTTP GET into a buffer ---------- */

typedef struct { char *buf; int len, cap; } rx_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    rx_t *rx = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && rx->len + e->data_len < rx->cap) {
        memcpy(rx->buf + rx->len, e->data, e->data_len);
        rx->len += e->data_len;
        rx->buf[rx->len] = 0;
    }
    return ESP_OK;
}

static cJSON *get_json(const char *url, int cap)
{
    rx_t rx = { .cap = cap };
    rx.buf = heap_caps_calloc(1, rx.cap, MALLOC_CAP_SPIRAM);
    if (!rx.buf) return NULL;
    esp_http_client_config_t cfg = {
        .url = url, .event_handler = http_evt, .user_data = &rx,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000,
    };
    int64_t t0 = esp_timer_get_time();
    int status;
    esp_err_t err = http_once(&cfg, &status);
    cJSON *j = err == ESP_OK && status == 200 ? cJSON_Parse(rx.buf) : NULL;
    if (err == ESP_OK && status == 200 && !j) svc_fail(SVC_UPDATES, tr(T_ERR_BAD_REPLY), t0);
    else svc_http(SVC_UPDATES, err, status, t0);
    if (!j) ESP_LOGW(TAG, "GET %s: %s, status %d", url, esp_err_to_name(err), status);
    free(rx.buf);
    return j;
}

/* ---------- release notes ---------- */


// Keeps the releases newer than the running version, up to the offered one. When a release is offered,
// release-candidate sections are skipped: the release's own section lists everything.
static void fetch_notes(const ver_t *cur, bool cur_ok, const ver_t *lat)
{
    char *buf = heap_caps_calloc(1, NOTES_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    int n = 0, kept = 0;
    cJSON *j = get_json(OTA_SITE "notes.json", 24576);
    cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItem(j, "releases")) {
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(r, "version"));
        const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(r, "date"));
        ver_t rv;
        if (!v || !parse_ver(v, &rv)) continue;
        if (cmp_ver(&rv, lat) > 0 || (cur_ok && cmp_ver(&rv, cur) <= 0)) continue;
        if (lat->kind == 1 && rv.kind == 0) continue;
        char date[32] = "";
        int y, mo, dd;
        if (d && sscanf(d, "%d-%d-%d", &y, &mo, &dd) == 3 && mo >= 1 && mo <= 12)
            tr_date_ymd(y, mo, dd, date, sizeof(date));                // "September 30, 2026" / "30 septembre 2026"
        int need = snprintf(NULL, 0, "%s%s|%s\n", kept ? "\n" : "", v, date);
        if (n + need >= NOTES_MAX - 32) break;
        n += snprintf(buf + n, NOTES_MAX - n, "%s%s|%s\n", kept ? "\n" : "", v, date);
        kept++;
        cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItem(r, "notes")) {
            const char *t = cJSON_GetStringValue(it);
            if (!t) continue;
            if (n + (int)strlen(t) + 2 >= NOTES_MAX - 32) { n += snprintf(buf + n, NOTES_MAX - n, "...\n"); goto full; }
            n += snprintf(buf + n, NOTES_MAX - n, "%s\n", t);
        }
    }
full:
    cJSON_Delete(j);
    if (!j) ESP_LOGW(TAG, "No release notes");
    else ESP_LOGI(TAG, "Release notes: %d release(s), %d bytes", kept, n);
    xSemaphoreTake(mux, portMAX_DELAY);
    if (!notes || strcmp(notes, buf)) {
        free(notes);
        notes = buf;
        buf = NULL;
        st.notes_id++;
    }
    xSemaphoreGive(mux);
    free(buf);
}

void ota_get_notes(char *out, size_t size)
{
    if (!size) return;
    xSemaphoreTake(mux, portMAX_DELAY);
    strlcpy(out, notes ? notes : "", size);
    xSemaphoreGive(mux);
}

/* ---------- check ---------- */

static void check(void)
{
    set_state(OTA_CHECKING, "");
    cJSON *ch = get_json(OTA_SITE "channels.json", 8192);
    if (!ch) { set_state(OTA_FAILED, tr(T_OTA_NO_SITE)); return; }
    char chan[8];
    xSemaphoreTake(mux, portMAX_DELAY);
    strlcpy(chan, st.channel, sizeof(chan));
    xSemaphoreGive(mux);
    cJSON *c = cJSON_GetObjectItem(ch, chan);
    if (!cJSON_IsObject(c)) c = cJSON_GetObjectItem(ch, "stable");      // no beta right now: stable
    const char *ver = cJSON_GetStringValue(cJSON_GetObjectItem(c, "version"));
    const char *man = cJSON_GetStringValue(cJSON_GetObjectItem(c, "manifest"));
    if (!ver || !man) { cJSON_Delete(ch); set_state(OTA_FAILED, "Unexpected channels.json"); return; }
    xSemaphoreTake(mux, portMAX_DELAY);
    strlcpy(st.latest, ver, sizeof(st.latest));
    xSemaphoreGive(mux);

    ver_t cur, lat;
    bool cur_ok = parse_ver(st.current, &cur), lat_ok = parse_ver(ver, &lat);
    bool newer = lat_ok && (!cur_ok || cmp_ver(&lat, &cur) > 0);        // unparsable local build: offer it
    ESP_LOGI(TAG, "%s channel offers %s, running %s: %s", chan, ver, st.current, newer ? "update available" : "up to date");
    if (!newer) { cJSON_Delete(ch); set_state(OTA_UP_TO_DATE, ""); return; }

    // The app image is the manifest part at 0x10000, relative to the manifest
    char url[256];
    snprintf(url, sizeof(url), OTA_SITE "%s", man);
    cJSON_Delete(ch);
    cJSON *m = get_json(url, 8192);
    cJSON *parts = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(m, "builds"), 0), "parts");
    const char *path = NULL;
    cJSON *p;
    cJSON_ArrayForEach(p, parts) {
        cJSON *off = cJSON_GetObjectItem(p, "offset");
        if (cJSON_IsNumber(off) && off->valueint == 0x10000) path = cJSON_GetStringValue(cJSON_GetObjectItem(p, "path"));
    }
    if (!path) { cJSON_Delete(m); set_state(OTA_FAILED, tr(T_OTA_NO_IMAGE)); return; }
    char *slash = strrchr(url, '/');
    if (slash) slash[1] = 0;
    xSemaphoreTake(mux, portMAX_DELAY);
    snprintf(app_url, sizeof(app_url), "%s%s", url, path);
    xSemaphoreGive(mux);
    cJSON_Delete(m);
    fetch_notes(&cur, cur_ok, &lat);                                   // optional: an update installs without them
    set_state(OTA_AVAILABLE, "");
}

/* ---------- install ---------- */

static void install(void)
{
    char url[256];
    xSemaphoreTake(mux, portMAX_DELAY);
    strlcpy(url, app_url, sizeof(url));
    st.progress = 0;
    xSemaphoreGive(mux);
    ESP_LOGI(TAG, "Installing %s", url);
    set_state(OTA_DOWNLOADING, "");
    esp_http_client_config_t http = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 20000,
        .keep_alive_enable = true, .buffer_size = 4096,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) { set_state(OTA_FAILED, tr(T_OTA_NO_START)); return; }

    esp_app_desc_t desc;
    if (esp_https_ota_get_img_desc(h, &desc) != ESP_OK ||
        strcmp(desc.project_name, esp_app_get_description()->project_name)) {
        ESP_LOGE(TAG, "Not this project's firmware (%s)", desc.project_name);
        esp_https_ota_abort(h);
        set_state(OTA_FAILED, tr(T_OTA_WRONG));
        return;
    }
    int total = esp_https_ota_get_image_size(h), last = -1;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int pct = total > 0 ? (int)(100LL * esp_https_ota_get_image_len_read(h) / total) : 0;
        if (pct != last) {
            last = pct;
            xSemaphoreTake(mux, portMAX_DELAY);
            st.progress = pct;
            xSemaphoreGive(mux);
            publish();
        }
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        ESP_LOGE(TAG, "Download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        set_state(OTA_FAILED, tr(T_OTA_INTERRUPTED));
        return;
    }
    err = esp_https_ota_finish(h);                  // verifies the image and selects it for the next boot
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image rejected: %s", esp_err_to_name(err));
        set_state(OTA_FAILED, tr(T_OTA_INVALID));
        return;
    }
    ESP_LOGI(TAG, "Update installed, restarting");
    xSemaphoreTake(mux, portMAX_DELAY);
    st.progress = 100;
    xSemaphoreGive(mux);
    set_state(OTA_DONE, "");
    vTaskDelay(pdMS_TO_TICKS(2500));
    esp_restart();
}

/* ---------- task ---------- */

static void ota_task(void *arg)
{
    TickType_t started = xTaskGetTickCount();
    bool validated = false;
    TickType_t next_check = started + pdMS_TO_TICKS(60 * 1000);          // first check a minute after boot
    while (1) {
        if (!validated && xTaskGetTickCount() - started >= pdMS_TO_TICKS(VALID_AFTER_S * 1000)) {
            validated = true;
            esp_ota_img_states_t s;
            if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &s) == ESP_OK && s == ESP_OTA_IMG_PENDING_VERIFY) {
                esp_ota_mark_app_valid_cancel_rollback();
                ESP_LOGI(TAG, "New firmware ran %d s: marked valid (no rollback)", VALID_AFTER_S);
            }
        }
        if (want_install) {
            want_install = false;
            if (st.state == OTA_AVAILABLE && net_is_connected()) install();
        }
        if (want_check || (int32_t)(xTaskGetTickCount() - next_check) >= 0) {
            want_check = false;
            next_check = xTaskGetTickCount() + pdMS_TO_TICKS(CHECK_EVERY_S * 1000LL);
            if (net_is_connected()) check();
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    }
}

void ota_start(ota_listener_t l)
{
    listener = l;
    mux = xSemaphoreCreateMutex();
    strlcpy(st.current, esp_app_get_description()->version, sizeof(st.current));
    strcpy(st.channel, "stable");
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(st.channel);
        nvs_get_str(h, "channel", st.channel, &n);
        nvs_close(h);
    }
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running %s from %s, channel %s", st.current, run ? run->label : "?", st.channel);
    xTaskCreatePinnedToCore(ota_task, "ota", 6144, NULL, 2, &task, 0);
}

void ota_check_now(void)
{
    want_check = true;
    if (task) xTaskNotifyGive(task);
}

bool ota_install(void)
{
    if (st.state != OTA_AVAILABLE) return false;
    want_install = true;
    if (task) xTaskNotifyGive(task);
    return true;
}

void ota_set_channel(const char *channel)
{
    if (strcmp(channel, "stable") && strcmp(channel, "beta")) return;
    xSemaphoreTake(mux, portMAX_DELAY);
    strlcpy(st.channel, channel, sizeof(st.channel));
    xSemaphoreGive(mux);
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) { nvs_set_str(h, "channel", channel); nvs_commit(h); nvs_close(h); }
    ota_check_now();
}

void ota_get_status(ota_status_t *out)
{
    if (!mux) { memset(out, 0, sizeof(*out)); return; }   // before ota_start()
    xSemaphoreTake(mux, portMAX_DELAY);
    *out = st;
    xSemaphoreGive(mux);
}
