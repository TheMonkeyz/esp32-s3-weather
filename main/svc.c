// External service health (see svc.h)
#include "svc.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "http_once.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "config.h"
#include "net.h"
#include "ota.h"
#include "i18n.h"

static const char *TAG = "svc";
#define PROBE_AFTER_US (5 * 60 * 1000000LL)    // the status page re-checks services idle for 5 min

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static svc_info_t svc[SVC_COUNT] = {
    [SVC_FORECAST] = { "Open-Meteo", "Forecast API v1" },
    [SVC_AIR]      = { "Open-Meteo air", "Air quality API v1" },
    [SVC_ALERTS]   = { "EC alerts", "OGC API" },
    [SVC_RADAR]    = { "EC GeoMet radar", "WMS 1.3.0" },
    [SVC_TILES]    = { "OpenStreetMap", "tiles" },
    [SVC_UPDATES]  = { "GitHub Pages", "updates" },
    [SVC_NTP]      = { "pool.ntp.org", "SNTP" },
};
static volatile bool probe_running;

static void record(svc_id_t id, bool ok, const char *why, int64_t t0)
{
    int64_t now = esp_timer_get_time();
    bool changed;
    int fails;
    taskENTER_CRITICAL(&mux);
    svc_info_t *s = &svc[id];
    changed = s->last_try && s->ok != ok;
    s->last_try = now;
    s->ms = t0 ? (int)((now - t0) / 1000) : 0;
    s->ok = ok;
    s->probing = false;
    if (ok) { s->last_ok = now; s->fails = 0; }
    else { s->fails++; strlcpy(s->why, why, sizeof(s->why)); }
    fails = s->fails;
    taskEXIT_CRITICAL(&mux);
    if (changed || fails == 1) {               // log transitions only, not every retry
        if (ok) ESP_LOGI(TAG, "%s: OK again", svc[id].name);
        else ESP_LOGW(TAG, "%s: %s", svc[id].name, why);
    }
}

void svc_http(svc_id_t id, esp_err_t err, int status, int64_t t0)
{
    if (err == ESP_OK && status == 200) { record(id, true, "", t0); return; }
    char why[40];
    if (err == ESP_OK) snprintf(why, sizeof(why), "HTTP %d", status);
    else if (err == ESP_ERR_HTTP_CONNECT) snprintf(why, sizeof(why), "%s", tr(T_ERR_CONNECT));
    else if (err == ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_TIMEOUT) snprintf(why, sizeof(why), "%s", tr(T_ERR_TIMEOUT));
    else if (err == ESP_ERR_HTTP_FETCH_HEADER) snprintf(why, sizeof(why), "%s", tr(T_ERR_NO_REPLY));
    else snprintf(why, sizeof(why), "%s", esp_err_to_name(err));
    record(id, false, why, t0);
}

void svc_ok(svc_id_t id, int64_t t0) { record(id, true, "", t0); }
void svc_fail(svc_id_t id, const char *why, int64_t t0) { record(id, false, why, t0); }

void svc_get(svc_id_t id, svc_info_t *out)
{
    taskENTER_CRITICAL(&mux);
    *out = svc[id];
    taskEXIT_CRITICAL(&mux);
}

/* ---------- probe: small requests to the services nothing has used lately ---------- */

static void probe_url(svc_id_t id, char *url, size_t n)
{
    location_t loc;
    config_get_location(&loc);
    switch (id) {
    case SVC_FORECAST:
        snprintf(url, n, "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m",
                 loc.lat, loc.lon); break;
    case SVC_AIR:
        snprintf(url, n, "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.4f&longitude=%.4f&current=us_aqi",
                 loc.lat, loc.lon); break;
    case SVC_ALERTS:
        snprintf(url, n, "https://api.weather.gc.ca/collections/weather-alerts/items?f=json&limit=1&skipGeometry=true"); break;
    case SVC_RADAR:
        snprintf(url, n, "https://geo.weather.gc.ca/geomet?service=WMS&version=1.3.0&request=GetCapabilities&layer=RADAR_1KM_RRAI"); break;
    case SVC_TILES:
        snprintf(url, n, "https://tile.openstreetmap.org/0/0/0.png"); break;
    case SVC_UPDATES:
        snprintf(url, n, OTA_SITE "channels.json"); break;
    default:
        url[0] = 0;
    }
}

static void probe_task(void *arg)
{
    char url[200];
    for (int i = 0; i < SVC_COUNT; i++) {
        bool due;
        taskENTER_CRITICAL(&mux);
        due = svc[i].probing;
        taskEXIT_CRITICAL(&mux);
        if (!due) continue;
        probe_url(i, url, sizeof(url));
        esp_http_client_config_t cfg = {
            .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000,
            .user_agent = "QuebecWeatherDisplay/1.0 (ESP32 hobby device; personal use)",   // OSM tile policy
        };
        int64_t t0 = esp_timer_get_time();
        int status;
        esp_err_t err = http_once(&cfg, &status);         // body read and dropped
        svc_http(i, err, status, t0);
        ESP_LOGI(TAG, "probe %s: %s, HTTP %d, %d ms", svc[i].name, esp_err_to_name(err), status,
                 (int)((esp_timer_get_time() - t0) / 1000));
    }
    probe_running = false;
    vTaskDelete(NULL);
}

void svc_probe_stale(void)
{
    if (probe_running || !net_is_connected()) return;
    int64_t now = esp_timer_get_time();
    int n = 0;
    taskENTER_CRITICAL(&mux);
    for (int i = 0; i < SVC_COUNT; i++) {
        if (i == SVC_NTP) continue;                          // SNTP syncs on its own schedule
        svc[i].probing = !svc[i].last_try || now - svc[i].last_try > PROBE_AFTER_US;
        n += svc[i].probing;
    }
    taskEXIT_CRITICAL(&mux);
    if (!n) return;
    probe_running = true;
    if (xTaskCreate(probe_task, "svc_probe", 8192, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "probe task not started");
        taskENTER_CRITICAL(&mux);
        for (int i = 0; i < SVC_COUNT; i++) svc[i].probing = false;
        taskEXIT_CRITICAL(&mux);
        probe_running = false;
    }
}
