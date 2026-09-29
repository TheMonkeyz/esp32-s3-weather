#pragma once
#include <stdbool.h>


typedef enum { WX_CLEAR, WX_PARTLY, WX_CLOUDY, WX_FOG, WX_RAIN, WX_SNOW, WX_STORM } wx_kind_t;

typedef struct {
    double tmax, tmin;
    int code;
    char date[12];   // YYYY-MM-DD
} wx_day_t;

typedef struct {
    double temp, feels, wind;
    int humidity, code, is_day;
    wx_day_t day[4];
    int ndays;
} weather_t;

bool weather_fetch(weather_t *w);
const char *weather_text(int code);
wx_kind_t weather_kind(int code);
