#pragma once
#include <stdbool.h>
#include "config.h"


typedef enum { WX_CLEAR, WX_PARTLY, WX_CLOUDY, WX_FOG, WX_RAIN, WX_SNOW, WX_STORM } wx_kind_t;

typedef struct {
    double tmax, tmin;
    int code, pop;       // pop = max precipitation probability %
    char date[12];   // YYYY-MM-DD
    char sunrise[6], sunset[6];   // HH:MM local
    float uv_max;
} wx_day_t;

#define WX_DAYS  7           // forecast days (the weather screen shows the first 3, the hourly view all)
#define WX_HOURS (WX_DAYS * 24)   // hourly forecast, starts at 00:00 today

typedef struct {
    float temp, wind;                  // temp NAN: no value (the reply had null)
    unsigned char code, pop, is_day;   // pop = precipitation probability %, WX_POP_NONE: no value
} wx_hour_t;
#define WX_POP_NONE 255

typedef struct {
    double temp, feels, wind;
    int humidity, code, is_day;
    float uv;                // UV index now
    wx_day_t day[WX_DAYS];
    int ndays;
    wx_hour_t hour[WX_HOURS];
    int nhours;
    // Next 2 hours from the 15-minute forecast ("nowcast")
    enum { NC_NONE, NC_STARTS, NC_STOPS } nc_kind;   // precipitation starting / stopping within 2 h
    char nc_time[6];                                  // HH:MM, local
    bool nc_snow;
    int utc_offset;          // seconds, for this place (config_set_utc_offset when it's the one shown)
} weather_t;

// Air quality (Open-Meteo air-quality API, CAMS). Pollen only exists for Europe: -1 elsewhere.
typedef struct {
    int us_aqi;              // -1 unknown
    float pm25;
    float pollen[4];         // grains/m³: alder, birch, grass, ragweed (-1 = no data)
} air_t;

bool weather_fetch(const location_t *loc, weather_t *w);   // forecast for one place
int weather_last_status(void);                             // HTTP status of the last forecast request (0: none)
// Drop the days before `today` ("YYYY-MM-DD", the place's local date) and their hours, so day[0] and hour[0..23] are
// today: between local midnight and the next fetch (the whole outage when offline) the forecast still starts
// yesterday, and the hourly view's "Now", its graph and the "Today" column showed yesterday. Returns the days dropped
// (0 if today isn't in the forecast).
int weather_from_today(weather_t *w, const char *today);
bool air_fetch(air_t *a);
const char *weather_text(int code);
wx_kind_t weather_kind(int code);
