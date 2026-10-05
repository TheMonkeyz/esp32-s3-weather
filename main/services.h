#pragma once
#include "svc.h"

// The outside services the display depends on, for the status screen (two swipes right). forge_net's svc.h keeps
// their health; services_init() adds the display's five before net_init() adds NTP and ota_start() the update site,
// so their ids are these. The status screen lists them in rows: the five, then the update site, then NTP.
enum {
    SVC_FORECAST,   // Open-Meteo forecast
    SVC_AIR,        // Open-Meteo air quality
    SVC_ALERTS,     // Environment Canada alerts (api.weather.gc.ca)
    SVC_RADAR,      // Environment Canada GeoMet radar (WMS)
    SVC_TILES,      // OpenStreetMap tiles
    SVC_UPDATES,    // (a row number: the web-flasher site on GitHub Pages, forge_ota's)
    SVC_NTP,        // (a row number: pool.ntp.org, forge_net's)
    SVC_COUNT       // rows
};

void services_init(void);           // before net_init(); also sets the reasons' texts (display language)
int services_row(int row);          // the svc id shown in that row, -1 if that service doesn't exist (yet)
