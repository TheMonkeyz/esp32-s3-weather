#pragma once
#include <stdbool.h>
#include <time.h>

// Environment Canada weather alerts (watches, warnings, advisories, statements) for one point.
#define ALERTS_MAX 4
#define ALERT_LANGS 2         // Environment Canada sends English and French (index = lang_t: 0 en, 1 fr)

typedef struct {
    char id[80];          // feature id (for the region shape)
    char code[16];        // alert type ("FTA"): with the place, what "the same warning" means (re-issues get a new id)
    char feature[24];     // region ("fea1-1068"): with code, which region map is shown
    char name[ALERT_LANGS][48];    // "Frost advisory" / "Avis de gel"
    char colour;          // 'r' red, 'o' orange, 'y' yellow, 'g' grey (unknown / statement)
    time_t ends;          // event end (UTC epoch), 0 if unknown
    char area[ALERT_LANGS][72];    // "City of Québec" / "Ville de Québec"
    char text[ALERT_LANGS][900];   // description
} alert_t;

typedef struct {
    alert_t a[ALERTS_MAX];
    int n;                // sorted most severe first
} alerts_t;

bool alerts_fetch(double lat, double lon, alerts_t *out);   // false = request failed (keep the old ones)

// Which alerts sound, for the place shown. Environment Canada re-issues a warning every few hours under a new id: the
// same warning is the same alert code, and it sounds once, then again only if it gets worse (yellow -> orange -> red).
// The first call after a reset (start-up, place switch) only records what is already there.
#define ALERTS_SEEN_MAX 16
typedef struct {
    struct { char code[48]; char colour; } s[ALERTS_SEEN_MAX];
    int n;
    bool primed;
} alerts_seen_t;
char alerts_to_sound(alerts_seen_t *seen, const alerts_t *al);   // colour of the most severe to sound, or 0
// "Which region map": code + region, so a re-issue keeps the map (the id changes, the region doesn't)
void alerts_map_key(const alert_t *a, char *out, int size);

// Map of the alert's region: the cached radar basemap (cropped to w x h, centred on the location) with the
// region filled in the alert colour and a dot at the location. Returns a new PSRAM RGB565 buffer (w*h) the
// caller owns, or NULL. Downloads the region shape; can take a few seconds.
uint16_t *alerts_map(const alert_t *a, double lat, double lon, int w, int h);
