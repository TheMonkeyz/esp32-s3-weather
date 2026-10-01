// Persistent settings (places, units) + local-time and unit-formatting helpers
#include <time.h>
#include "config.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_log.h"
#include "i18n.h"

static const char *TAG = "config";
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
// Places: the first one lives in NVS namespace "loc" (name, lat, lon in micro-degrees, as before several places
// existed); the others in "places" as blobs "p1".."p3", with the count "n" and the place shown "act".
static location_t places[MAX_PLACES];
static int nplaces = 1, active;
static bool loaded;
static volatile int utc_offset;
static volatile bool offset_known;

static void load(void)
{
    if (loaded) return;
    strcpy(places[0].name, "Québec");
    places[0].lat = 46.8139;
    places[0].lon = -71.2080;
    nvs_handle_t h;
    if (nvs_open("loc", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(places[0].name);
        int64_t la, lo;
        char name[48];
        if (nvs_get_str(h, "name", name, &n) == ESP_OK &&
            nvs_get_i64(h, "lat", &la) == ESP_OK && nvs_get_i64(h, "lon", &lo) == ESP_OK) {
            strlcpy(places[0].name, name, sizeof(places[0].name));
            places[0].lat = la / 1e6;
            places[0].lon = lo / 1e6;
        }
        nvs_close(h);
    }
    if (nvs_open("places", NVS_READONLY, &h) == ESP_OK) {
        uint8_t n = 1, act = 0;
        nvs_get_u8(h, "n", &n);
        nvs_get_u8(h, "act", &act);
        for (int i = 1; i < n && i < MAX_PLACES; i++) {
            char key[4] = { 'p', '0' + i, 0 };
            size_t len = sizeof(location_t);
            if (nvs_get_blob(h, key, &places[i], &len) != ESP_OK || len != sizeof(location_t)) break;
            nplaces = i + 1;
        }
        active = act < nplaces ? act : 0;
        nvs_close(h);
    }
    loaded = true;
    for (int i = 0; i < nplaces; i++)
        ESP_LOGI(TAG, "Place %d%s: %s (%.4f, %.4f)", i + 1, i == active ? " (shown)" : "", places[i].name,
                 places[i].lat, places[i].lon);
}

static bool save(void)                 // all places + the one shown
{
    nvs_handle_t h;
    if (nvs_open("loc", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, "name", places[0].name);
    nvs_set_i64(h, "lat", (int64_t)(places[0].lat * 1e6));
    nvs_set_i64(h, "lon", (int64_t)(places[0].lon * 1e6));
    nvs_commit(h);
    nvs_close(h);
    if (nvs_open("places", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_u8(h, "n", nplaces);
    nvs_set_u8(h, "act", active);
    for (int i = 1; i < MAX_PLACES; i++) {
        char key[4] = { 'p', '0' + i, 0 };
        if (i < nplaces) nvs_set_blob(h, key, &places[i], sizeof(location_t));
        else nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
    return true;
}

void config_get_location(location_t *out)
{
    load();
    taskENTER_CRITICAL(&mux);
    *out = places[active];
    taskEXIT_CRITICAL(&mux);
}

bool config_set_location(const location_t *loc) { return config_set_place(config_active_place(), loc); }

int config_place_count(void) { load(); return nplaces; }
int config_active_place(void) { load(); return active; }

bool config_get_place(int i, location_t *out)
{
    load();
    if (i < 0 || i >= nplaces) return false;
    taskENTER_CRITICAL(&mux);
    *out = places[i];
    taskEXIT_CRITICAL(&mux);
    return true;
}

bool config_set_place(int i, const location_t *loc)
{
    load();
    if (loc->lat < -85 || loc->lat > 85 || loc->lon < -180 || loc->lon > 180) return false;
    if (i < 0 || i > nplaces || i >= MAX_PLACES) return false;
    taskENTER_CRITICAL(&mux);
    places[i] = *loc;
    if (i == nplaces) nplaces++;
    taskEXIT_CRITICAL(&mux);
    if (i == active) offset_known = false;   // may be another time zone; the next forecast sets it
    ESP_LOGI(TAG, "Saved place %d: %s (%.4f, %.4f)", i + 1, loc->name, loc->lat, loc->lon);
    return save();
}

bool config_delete_place(int i)
{
    load();
    if (i < 0 || i >= nplaces || nplaces == 1) return false;
    taskENTER_CRITICAL(&mux);
    for (int k = i; k + 1 < nplaces; k++) places[k] = places[k + 1];
    nplaces--;
    if (active > i || active >= nplaces) active = active > 0 ? active - 1 : 0;
    taskEXIT_CRITICAL(&mux);
    ESP_LOGI(TAG, "Deleted place %d; %d left, showing %d", i + 1, nplaces, active + 1);
    return save();
}

bool config_select_place(int i)
{
    load();
    if (i < 0 || i >= nplaces) return false;
    if (i == active) return true;
    active = i;
    offset_known = false;
    ESP_LOGI(TAG, "Showing place %d: %s", i + 1, places[i].name);
    return save();
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
    if (nvs_open("units", NVS_READONLY, &h) != ESP_OK) { i18n_set(LANG_EN); return; }
    uint8_t v;
    if (nvs_get_u8(h, "temp", &v) == ESP_OK) units.fahrenheit = v == 1;
    if (nvs_get_u8(h, "wind", &v) == ESP_OK && v <= WIND_MS) units.wind = v;
    if (nvs_get_u8(h, "h12", &v) == ESP_OK) units.h12 = v == 1;
    if (nvs_get_u8(h, "lang", &v) == ESP_OK && v < LANG_COUNT) units.lang = v;
    nvs_close(h);
    i18n_set(units.lang);
    ESP_LOGI(TAG, "Units: %s, %s, %s, %s", units.fahrenheit ? "F" : "C",
             units.wind == WIND_MPH ? "mph" : units.wind == WIND_MS ? "m/s" : "km/h", units.h12 ? "12 h" : "24 h",
             i18n_code(units.lang));
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
    if (u->wind > WIND_MS || u->lang >= LANG_COUNT) return false;
    nvs_handle_t h;
    if (nvs_open("units", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_u8(h, "temp", u->fahrenheit);
    nvs_set_u8(h, "wind", u->wind);
    nvs_set_u8(h, "h12", u->h12);
    nvs_set_u8(h, "lang", u->lang);
    nvs_set_u8(h, "lang", u->lang);
    nvs_set_u8(h, "lang", u->lang);
    nvs_set_u8(h, "lang", u->lang);
    nvs_commit(h);
    nvs_close(h);
    units_load();
    taskENTER_CRITICAL(&mux);
    units = *u;
    taskEXIT_CRITICAL(&mux);
    i18n_set(u->lang);
    ESP_LOGI(TAG, "Saved units: %s, %s, %s, %s", u->fahrenheit ? "F" : "C",
             u->wind == WIND_MPH ? "mph" : u->wind == WIND_MS ? "m/s" : "km/h", u->h12 ? "12 h" : "24 h",
             i18n_code(u->lang));
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
