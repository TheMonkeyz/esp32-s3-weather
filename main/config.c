// Persistent settings (location, units) + local-time and unit-formatting helpers
#include <time.h>
#include "config.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "config";
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static location_t cur;
static bool loaded;
static volatile int utc_offset;
static volatile bool offset_known;

static void load(void)
{
    if (loaded) return;
    strcpy(cur.name, "Québec");
    cur.lat = 46.8139;
    cur.lon = -71.2080;
    nvs_handle_t h;
    if (nvs_open("loc", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(cur.name);
        int64_t la, lo;
        char name[48];
        if (nvs_get_str(h, "name", name, &n) == ESP_OK &&
            nvs_get_i64(h, "lat", &la) == ESP_OK && nvs_get_i64(h, "lon", &lo) == ESP_OK) {
            strlcpy(cur.name, name, sizeof(cur.name));
            cur.lat = la / 1e6;
            cur.lon = lo / 1e6;
        }
        nvs_close(h);
    }
    loaded = true;
    ESP_LOGI(TAG, "Location: %s (%.4f, %.4f)", cur.name, cur.lat, cur.lon);
}

void config_get_location(location_t *out)
{
    load();
    taskENTER_CRITICAL(&mux);
    *out = cur;
    taskEXIT_CRITICAL(&mux);
}

bool config_set_location(const location_t *loc)
{
    if (loc->lat < -85 || loc->lat > 85 || loc->lon < -180 || loc->lon > 180) return false;
    nvs_handle_t h;
    if (nvs_open("loc", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, "name", loc->name);
    nvs_set_i64(h, "lat", (int64_t)(loc->lat * 1e6));
    nvs_set_i64(h, "lon", (int64_t)(loc->lon * 1e6));
    nvs_commit(h);
    nvs_close(h);
    taskENTER_CRITICAL(&mux);
    cur = *loc;
    taskEXIT_CRITICAL(&mux);
    offset_known = false;   // new place may be in another time zone; weather fetch will update it
    ESP_LOGI(TAG, "Saved location: %s (%.4f, %.4f)", loc->name, loc->lat, loc->lon);
    return true;
}

void config_set_utc_offset(int seconds)
{
    utc_offset = seconds;
    offset_known = true;
}

bool config_local_time(long t, struct tm *out)
{
    if (t < 1600000000L) return false;          // clock not set by SNTP yet
    time_t tt = (time_t)t;
    if (offset_known) {
        tt += utc_offset;
        gmtime_r(&tt, out);
    } else {
        localtime_r(&tt, out);                   // fallback: TZ from net.c (Eastern)
    }
    return true;
}

/* ---------- units and formats ---------- */

static units_t units;
static bool units_loaded;

static void units_load(void)
{
    if (units_loaded) return;
    units_loaded = true;
    nvs_handle_t h;
    if (nvs_open("units", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "temp", &v) == ESP_OK) units.fahrenheit = v == 1;
    if (nvs_get_u8(h, "wind", &v) == ESP_OK && v <= WIND_MS) units.wind = v;
    if (nvs_get_u8(h, "h12", &v) == ESP_OK) units.h12 = v == 1;
    nvs_close(h);
    ESP_LOGI(TAG, "Units: %s, %s, %s", units.fahrenheit ? "F" : "C",
             units.wind == WIND_MPH ? "mph" : units.wind == WIND_MS ? "m/s" : "km/h", units.h12 ? "12 h" : "24 h");
}

void config_get_units(units_t *out)
{
    units_load();
    taskENTER_CRITICAL(&mux);
    *out = units;
    taskEXIT_CRITICAL(&mux);
}

bool config_set_units(const units_t *u)
{
    if (u->wind > WIND_MS) return false;
    nvs_handle_t h;
    if (nvs_open("units", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_u8(h, "temp", u->fahrenheit);
    nvs_set_u8(h, "wind", u->wind);
    nvs_set_u8(h, "h12", u->h12);
    nvs_commit(h);
    nvs_close(h);
    units_load();
    taskENTER_CRITICAL(&mux);
    units = *u;
    taskEXIT_CRITICAL(&mux);
    ESP_LOGI(TAG, "Saved units: %s, %s, %s", u->fahrenheit ? "F" : "C",
             u->wind == WIND_MPH ? "mph" : u->wind == WIND_MS ? "m/s" : "km/h", u->h12 ? "12 h" : "24 h");
    return true;
}

int config_temp(double c)
{
    units_t u;
    config_get_units(&u);
    double t = u.fahrenheit ? c * 9 / 5 + 32 : c;
    return (int)(t < 0 ? t - 0.5 : t + 0.5);
}

void config_fmt_wind(double kmh, char *out, int n)
{
    units_t u;
    config_get_units(&u);
    if (u.wind == WIND_MPH) snprintf(out, n, "%.0f mph", kmh / 1.609344);
    else if (u.wind == WIND_MS) snprintf(out, n, "%.0f m/s", kmh / 3.6);
    else snprintf(out, n, "%.0f km/h", kmh);
}

void config_fmt_time(int h, int m, char *out, int n)
{
    units_t u;
    config_get_units(&u);
    if (u.h12) snprintf(out, n, "%d:%02d %s", (h + 11) % 12 + 1, m, h < 12 ? "AM" : "PM");
    else snprintf(out, n, "%02d:%02d", h, m);
}

void config_fmt_hour(int h, char *out, int n)
{
    units_t u;
    config_get_units(&u);
    if (u.h12) snprintf(out, n, "%d %s", (h + 11) % 12 + 1, h < 12 ? "AM" : "PM");
    else snprintf(out, n, "%02d:00", h);
}

bool config_miles(void)
{
    units_t u;
    config_get_units(&u);
    return u.wind == WIND_MPH;
}

void config_fmt_hhmm(const char *hhmm, char *out, int n)
{
    int h, m;
    if (hhmm && sscanf(hhmm, "%d:%d", &h, &m) == 2) config_fmt_time(h, m, out, n);
    else snprintf(out, n, "%s", hhmm ? hhmm : "");
}
