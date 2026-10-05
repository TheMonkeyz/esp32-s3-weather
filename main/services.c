// The display's outside services (see services.h). Moved from svc.c, October 5 (v1.14.0): forge_net keeps the
// health records, probes and User-Agent; the display adds its services, their probe URLs and the reasons' texts.
#include "services.h"
#include <stdio.h>
#include "esp_log.h"
#include "config.h"
#include "i18n.h"

static void probe_forecast(char *url, size_t n)
{
    location_t loc;
    config_get_location(&loc);
    snprintf(url, n, "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m",
             loc.lat, loc.lon);
}

static void probe_air(char *url, size_t n)
{
    location_t loc;
    config_get_location(&loc);
    snprintf(url, n, "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.4f&longitude=%.4f&current=us_aqi",
             loc.lat, loc.lon);
}

static void probe_alerts(char *url, size_t n)
{
    snprintf(url, n, "https://api.weather.gc.ca/collections/weather-alerts/items?f=json&limit=1&skipGeometry=true");
}

static void probe_radar(char *url, size_t n)
{
    snprintf(url, n, "https://geo.weather.gc.ca/geomet?service=WMS&version=1.3.0&request=GetCapabilities&layer=RADAR_1KM_RRAI");
}

static void probe_tiles(char *url, size_t n) { snprintf(url, n, "https://tile.openstreetmap.org/0/0/0.png"); }

// A failure's reason in the display language (the status screen shows it; the log line carries it too, as before)
static const char *why_text(svc_why_t code, int http)
{
    switch (code) {
    case SVC_WHY_CONNECT:   return tr(T_ERR_CONNECT);
    case SVC_WHY_TIMEOUT:   return tr(T_ERR_TIMEOUT);
    case SVC_WHY_NO_REPLY:  return tr(T_ERR_NO_REPLY);
    case SVC_WHY_BAD_REPLY: return tr(T_ERR_BAD_REPLY);
    case SVC_WHY_EMPTY:     return tr(T_ERR_EMPTY);
    default:                return NULL;
    }
}

void services_init(void)
{
    int ids[5] = {
        svc_add("Open-Meteo", "Forecast API v1", probe_forecast),
        svc_add("Open-Meteo air", "Air quality API v1", probe_air),
        svc_add("EC alerts", "OGC API", probe_alerts),
        svc_add("EC GeoMet radar", "WMS 1.3.0", probe_radar),
        svc_add("OpenStreetMap", "tiles", probe_tiles),
    };
    for (int i = 0; i < 5; i++)
        if (ids[i] != i) ESP_LOGE("svc", "service %d got id %d: services_init() must run before net_init()", i, ids[i]);
    svc_set_why_text(why_text);
}

int services_row(int row)
{
    if (row < SVC_UPDATES) return row;
    return svc_find(row == SVC_UPDATES ? SVC_NAME_UPDATES : row == SVC_NTP ? SVC_NAME_NTP : "");
}
