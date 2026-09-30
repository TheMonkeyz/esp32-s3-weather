#pragma once
#include <stdbool.h>


typedef enum { WX_CLEAR, WX_PARTLY, WX_CLOUDY, WX_FOG, WX_RAIN, WX_SNOW, WX_STORM } wx_kind_t;

typedef struct {
    double tmax, tmin;
    int code, pop;       // pop = max precipitation probability %
    char date[12];   // YYYY-MM-DD
} wx_day_t;

#define WX_DAYS  7           // forecast days (the weather screen shows the first 3, the hourly view all)
#define WX_HOURS (WX_DAYS * 24)   // hourly forecast, starts at 00:00 today

typedef struct {
    float temp, wind;
    unsigned char code, pop, is_day;   // pop = precipitation probability %
} wx_hour_t;

typedef struct {
    double temp, feels, wind;
    int humidity, code, is_day;
    wx_day_t day[WX_DAYS];
    int ndays;
    wx_hour_t hour[WX_HOURS];
    int nhours;
    // Next 2 hours from the 15-minute forecast ("nowcast")
    enum { NC_NONE, NC_STARTS, NC_STOPS } nc_kind;   // precipitation starting / stopping within 2 h
    char nc_time[6];                                  // HH:MM, local
    bool nc_snow;
} weather_t;

bool weather_fetch(weather_t *w);
const char *weather_text(int code);
wx_kind_t weather_kind(int code);
