#pragma once
#include <stdbool.h>
#include <time.h>

typedef struct {
    char name[48];
    double lat, lon;
} location_t;

// Places: up to MAX_PLACES; the "location" everything uses (forecast, alerts, radar...) is the one shown.
#define MAX_PLACES 4
void config_get_location(location_t *out);          // the place shown; defaults to Québec City
// An NVS write's result: false (and a log line naming `what`) if it failed. Savers chain these and report "not saved".
#include "esp_err.h"
bool nvs_check(esp_err_t err, const char *what);
bool config_set_location(const location_t *loc);    // edits the place shown; saves to NVS
int config_place_count(void);
int config_active_place(void);
bool config_get_place(int i, location_t *out);
bool config_set_place(int i, const location_t *loc);   // i == count adds one
bool config_delete_place(int i);                       // the last place can't be deleted
bool config_select_place(int i);                       // show place i (saved, kept across restarts)

// Local time helpers: UTC offset reported by the weather service for the configured location
void config_set_utc_offset(int seconds);
bool config_local_time(long t, struct tm *out);     // false if the clock isn't synced yet

// Display units and formats (settings page, NVS namespace "units"). Data is always fetched in metric and
// converted for display only.
typedef enum { WIND_KMH, WIND_MPH, WIND_MS } wind_unit_t;
typedef struct {
    bool fahrenheit;
    unsigned char wind;      // wind_unit_t
    bool h12;                // 12-hour clock
    unsigned char lang;      // lang_t (i18n.h): display and settings page language
} units_t;
void config_get_units(units_t *out);                       // defaults: °C, km/h, 24-hour
bool config_set_units(const units_t *u);                   // saves to NVS
int config_temp(double celsius);                           // rounded, in the chosen unit
void config_fmt_wind(double kmh, char *out, int n);        // "12 km/h", "7 mph", "3 m/s"
void config_fmt_time(int h, int m, char *out, int n);      // "14:45" or "2:45 PM"
void config_fmt_hour(int h, char *out, int n);             // "14:00" or "2 PM"
void config_fmt_hhmm(const char *hhmm, char *out, int n);  // "HH:MM" from the forecast, reformatted
bool config_miles(void);                                   // distances in miles (wind in mph), else km
