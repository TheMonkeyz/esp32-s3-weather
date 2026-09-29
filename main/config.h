#pragma once
#include <stdbool.h>
#include <time.h>

typedef struct {
    char name[48];
    double lat, lon;
} location_t;

void config_get_location(location_t *out);          // defaults to Québec City
bool config_set_location(const location_t *loc);    // saves to NVS

// Local time helpers: UTC offset reported by the weather service for the configured location
void config_set_utc_offset(int seconds);
bool config_local_time(long t, struct tm *out);     // false if the clock isn't synced yet
