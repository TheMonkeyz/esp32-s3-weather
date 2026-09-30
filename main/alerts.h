#pragma once
#include <stdbool.h>
#include <time.h>

// Environment Canada weather alerts (watches, warnings, advisories, statements) for one point.
#define ALERTS_MAX 4

typedef struct {
    char id[80];          // feature id (for the region shape)
    char name[48];        // "Frost advisory"
    char colour;          // 'r' red, 'o' orange, 'y' yellow, 'g' grey (unknown / statement)
    time_t ends;          // event end (UTC epoch), 0 if unknown
    char area[72];        // "City of Québec"
    char text[900];       // description (English)
} alert_t;

typedef struct {
    alert_t a[ALERTS_MAX];
    int n;                // sorted most severe first
} alerts_t;

bool alerts_fetch(double lat, double lon, alerts_t *out);   // false = request failed (keep the old ones)

// Map of the alert's region: the cached radar basemap (cropped to w x h, centred on the location) with the
// region filled in the alert colour and a dot at the location. Returns a new PSRAM RGB565 buffer (w*h) the
// caller owns, or NULL. Downloads the region shape; can take a few seconds.
uint16_t *alerts_map(const alert_t *a, double lat, double lon, int w, int h);
