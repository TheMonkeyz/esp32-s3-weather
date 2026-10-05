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
#include "utf8.h"
#include "esp_attr.h"

static const char *TAG = "config";
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
// Places: the first one lives in NVS namespace "loc" (name, lat, lon in micro-degrees, as before several places
// existed); the others in "places" as typed keys "pNn" (name), "pNa" / "pNo" (lat / lon in micro-degrees), N = 1..3,
// with the count "n" and the place shown "act". Until v1.12.0 they were raw location_t blobs "p1".."p3", read only if
// the size matched: the first field added to location_t would have dropped every extra place. Those blobs are read
// once and rewritten as typed keys.

static location_t places[MAX_PLACES];
static int nplaces = 1, active;
static bool loaded, place_saved;             // place_saved: NVS has a place (someone chose one)
static volatile int utc_offset;
static volatile bool offset_known;

static bool save(void);

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
            place_saved = true;
        }
        nvs_close(h);
    }
    bool migrate = false;
    if (nvs_open("places", NVS_READONLY, &h) == ESP_OK) {
        uint8_t n = 1, act = 0;
        nvs_get_u8(h, "n", &n);
        nvs_get_u8(h, "act", &act);
        for (int i = 1; i < n && i < MAX_PLACES; i++) {
            char k[8];
            size_t len = sizeof(places[i].name);
            int64_t la, lo;
            snprintf(k, sizeof(k), "p%dn", i);
            bool typed = nvs_get_str(h, k, places[i].name, &len) == ESP_OK;
            snprintf(k, sizeof(k), "p%da", i);
            typed = typed && nvs_get_i64(h, k, &la) == ESP_OK;
            snprintf(k, sizeof(k), "p%do", i);
            typed = typed && nvs_get_i64(h, k, &lo) == ESP_OK;
            if (typed) { places[i].lat = la / 1e6; places[i].lon = lo / 1e6; }
            else {                                       // the old blob (location_t as it was until v1.12.0)
                struct { char name[48]; double lat, lon; } old;
                snprintf(k, sizeof(k), "p%d", i);
                len = sizeof(old);
                if (nvs_get_blob(h, k, &old, &len) != ESP_OK || len != sizeof(old)) break;
                utf8_copy(places[i].name, old.name, sizeof(places[i].name));
                places[i].lat = old.lat;
                places[i].lon = old.lon;
                migrate = true;
            }
            nplaces = i + 1;
        }
        active = act < nplaces ? act : 0;
        nvs_close(h);
    }
    loaded = true;
    if (migrate) ESP_LOGI(TAG, "Places: moved to typed keys: %s", save() ? "saved" : "NOT saved");
    for (int i = 0; i < nplaces; i++)
        ESP_LOGI(TAG, "Place %d%s: %s (%.4f, %.4f)", i + 1, i == active ? " (shown)" : "", places[i].name,
                 places[i].lat, places[i].lon);
}

// All places + the one shown. Every write is checked: a failure is logged with its key and the caller answers "not
// saved" (the settings page shows it) instead of "Saved". The count is written last, after the places it counts.
static bool save(void)
{
    nvs_handle_t h;
    bool ok = nvs_check(nvs_open("loc", NVS_READWRITE, &h), "open loc");
    if (ok) {
        ok = nvs_check(nvs_set_str(h, "name", places[0].name), "loc/name") &&
             nvs_check(nvs_set_i64(h, "lat", (int64_t)(places[0].lat * 1e6)), "loc/lat") &&
             nvs_check(nvs_set_i64(h, "lon", (int64_t)(places[0].lon * 1e6)), "loc/lon") &&
             nvs_check(nvs_commit(h), "loc commit");
        nvs_close(h);
    }
    if (!ok || !nvs_check(nvs_open("places", NVS_READWRITE, &h), "open places")) return false;
    for (int i = 1; i < MAX_PLACES && ok; i++) {
        char kn[8], ka[8], ko[8], kb[4] = { 'p', '0' + i, 0 };
        snprintf(kn, sizeof(kn), "p%dn", i);
        snprintf(ka, sizeof(ka), "p%da", i);
        snprintf(ko, sizeof(ko), "p%do", i);
        nvs_erase_key(h, kb);                            // the old blob, if any
        if (i < nplaces) {
            ok = nvs_check(nvs_set_str(h, kn, places[i].name), kn) &&
                 nvs_check(nvs_set_i64(h, ka, (int64_t)(places[i].lat * 1e6)), ka) &&
                 nvs_check(nvs_set_i64(h, ko, (int64_t)(places[i].lon * 1e6)), ko);
        } else {
            nvs_erase_key(h, kn);
            nvs_erase_key(h, ka);
            nvs_erase_key(h, ko);
        }
    }
    ok = ok && nvs_check(nvs_set_u8(h, "n", nplaces), "places/n") && nvs_check(nvs_set_u8(h, "act", active), "places/act") &&
         nvs_check(nvs_commit(h), "places commit");
    nvs_close(h);
    return ok;
}

void config_get_location(location_t *out)
{
    load();
    taskENTER_CRITICAL(&mux);
    *out = places[active];
    taskEXIT_CRITICAL(&mux);
}

bool config_set_location(const location_t *loc) { return config_set_place(config_active_place(), loc); }

static RTC_NOINIT_ATTR uint32_t hint_test;           // test console: the hint on the next boot (one boot)
#define HINT_TEST 0x4817A11Eu

bool config_place_is_default(void) { load(); return (!place_saved && nplaces == 1) || hint_test == HINT_TEST; }

bool config_hint_wanted(void)
{
    if (hint_test == HINT_TEST) return true;
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "hint", &v); nvs_close(h); }
    return !v;
}

void config_hint_done(void)
{
    if (hint_test == HINT_TEST) { hint_test = 0; return; }   // (a test: the real flag is left alone)
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_check(nvs_set_u8(h, "hint", 1), "ui/hint");
    nvs_commit(h);
    nvs_close(h);
}

void config_hint_next_boot(void) { hint_test = HINT_TEST; }

static bool gest_test;                               // this boot is a "hint next-boot" test: show it, keep the flag

bool config_gesture_hint_wanted(void)
{
    if (hint_test == HINT_TEST) { gest_test = true; return true; }
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "gest", &v); nvs_close(h); }
    return !v;
}

void config_gesture_hint_done(void)
{
    if (gest_test) return;
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_check(nvs_set_u8(h, "gest", 1), "ui/gest");
    nvs_commit(h);
    nvs_close(h);
}

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
    location_t l = *loc;
    utf8_cut(l.name, sizeof(l.name));                    // (a byte-wise cut split a character)
    taskENTER_CRITICAL(&mux);
    places[i] = l;
    if (i == nplaces) nplaces++;
    taskEXIT_CRITICAL(&mux);
    if (i == active) offset_known = false;   // may be another time zone; the next forecast sets it
    if (i == 0) place_saved = true;
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
    if (!nvs_check(nvs_open("units", NVS_READWRITE, &h), "open units")) return false;
    bool ok = nvs_check(nvs_set_u8(h, "temp", u->fahrenheit), "units/temp") &&
              nvs_check(nvs_set_u8(h, "wind", u->wind), "units/wind") &&
              nvs_check(nvs_set_u8(h, "h12", u->h12), "units/h12") &&
              nvs_check(nvs_set_u8(h, "lang", u->lang), "units/lang") && nvs_check(nvs_commit(h), "units commit");
    nvs_close(h);
    if (!ok) return false;
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

// 12-hour clock: "2:45 PM" in English (and Inuktitut, which has no convention of its own here); in French, Québec
// style (OQLF): "2 h 45 p.m.", an hour alone "3 p.m." (the owner's choice, October 2026)
void config_fmt_time(int h, int m, char *out, int n)
{
    units_t u;
    config_get_units(&u);
    if (u.h12 && u.lang == LANG_FR) snprintf(out, n, "%d h %02d %s", (h + 11) % 12 + 1, m, h < 12 ? "a.m." : "p.m.");
    else if (u.h12) snprintf(out, n, "%d:%02d %s", (h + 11) % 12 + 1, m, h < 12 ? "AM" : "PM");
    else snprintf(out, n, "%02d:%02d", h, m);
}

void config_fmt_hour(int h, char *out, int n)
{
    units_t u;
    config_get_units(&u);
    if (u.h12 && u.lang == LANG_FR) snprintf(out, n, "%d %s", (h + 11) % 12 + 1, h < 12 ? "a.m." : "p.m.");
    else if (u.h12) snprintf(out, n, "%d %s", (h + 11) % 12 + 1, h < 12 ? "AM" : "PM");
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
