#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Health of the external services the display depends on, for the status screen (two swipes right).
// Every fetch reports its outcome here; times are esp_timer microseconds (0 = never).

typedef enum {
    SVC_FORECAST,   // Open-Meteo forecast
    SVC_AIR,        // Open-Meteo air quality
    SVC_ALERTS,     // Environment Canada alerts (api.weather.gc.ca)
    SVC_RADAR,      // Environment Canada GeoMet radar (WMS)
    SVC_TILES,      // OpenStreetMap tiles
    SVC_UPDATES,    // web-flasher site on GitHub Pages
    SVC_NTP,        // pool.ntp.org
    SVC_COUNT
} svc_id_t;

typedef struct {
    const char *name;       // "Open-Meteo"
    const char *api;        // "Forecast API v1", "WMS 1.3.0"...
    int64_t last_try, last_ok;
    bool ok;                // outcome of the last try
    bool probing;           // a check is queued or running (svc_probe_stale)
    int fails;              // failures in a row
    int ms;                 // duration of the last try
    char why[40];           // reason of the last failure
} svc_info_t;

// t0 = esp_timer_get_time() taken before the request. Success = ESP_OK and HTTP 200.
void svc_http(svc_id_t id, esp_err_t err, int status, int64_t t0);
void svc_ok(svc_id_t id, int64_t t0);
void svc_fail(svc_id_t id, const char *why, int64_t t0);
void svc_get(svc_id_t id, svc_info_t *out);
// The User-Agent of the firmware's own requests (OSM's tile policy asks apps to identify themselves): the app name,
// the running version and the repository, e.g. "esp32-s3-weather/1.12.0 (open-source weather display; +https://...)".
const char *svc_user_agent(void);
// Status page opened: in a short-lived task, send a small request to each service not contacted for 5 min.
void svc_probe_stale(void);
