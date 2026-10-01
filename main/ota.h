#pragma once
#include <stdbool.h>
#include <stddef.h>

// Firmware updates over Wi-Fi from the web-flasher site (GitHub Pages): channels.json says which version each
// channel (stable / beta) offers, the channel's manifest.json gives the app image. Checks shortly after boot and
// every 6 hours; installing is always the user's choice (screen or settings page).

typedef enum {
    OTA_IDLE,          // not checked yet
    OTA_CHECKING,
    OTA_UP_TO_DATE,
    OTA_AVAILABLE,     // latest > current
    OTA_DOWNLOADING,   // progress 0..100
    OTA_DONE,          // restarting
    OTA_FAILED,        // error has the reason
} ota_state_t;

typedef struct {
    ota_state_t state;
    char current[32];   // this firmware's version
    char latest[32];    // version offered by the channel ("" if unknown)
    char channel[8];    // "stable" / "beta"
    char error[64];
    int progress;       // percent while downloading
    int notes_id;       // changes whenever the release notes change (see ota_get_notes)
} ota_status_t;

typedef void (*ota_listener_t)(const ota_status_t *st);   // called from the OTA task on every change

void ota_start(ota_listener_t listener);   // after Wi-Fi is up
void ota_check_now(void);
bool ota_install(void);                    // false if nothing to install or already busy
void ota_set_channel(const char *channel); // saved; triggers a check
void ota_get_status(ota_status_t *out);
// What's new between the running version and the offered one, from the site's notes.json (CHANGELOG.md):
// sections separated by a blank line, each a "vX.Y.Z|Month D, YYYY" header line then one line per change.
// Empty if nothing is offered or the notes couldn't be read.
void ota_get_notes(char *out, size_t size);
