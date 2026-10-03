#pragma once
#include <stdbool.h>
#include <stddef.h>

#define OTA_SITE "https://themonkeyz.github.io/esp32-s3-weather/"   // web-flasher site (GitHub Pages)

// Firmware updates over Wi-Fi from the web-flasher site (GitHub Pages): channels.json says which version each
// channel (stable / beta) offers, the channel's manifest.json gives the app image. Checks a minute after boot (or as
// soon as Wi-Fi is up, if it wasn't then), whenever Wi-Fi comes back after an outage, and every 6 hours; installing
// is always the user's choice (screen or settings page). A new image is confirmed once it has run 60 s connected to
// Wi-Fi (10 min without): a restart before that goes back to the previous one, which then says so (rolled_back).

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
    char rolled_back[32];   // a version that was installed and undone by the bootloader (shown once), else ""
} ota_status_t;

typedef void (*ota_listener_t)(const ota_status_t *st);   // called from the OTA task on every change

void ota_start(ota_listener_t listener);   // at start-up (before Wi-Fi)
void ota_check_now(void);
bool ota_install(void);                    // false if nothing to install or already busy
void ota_set_channel(const char *channel); // saved; triggers a check
void ota_get_status(ota_status_t *out);
bool ota_pending_verify(void);             // running a new image not confirmed yet (a restart now rolls it back)
// Restart, but not while a new image is pending verify: then wait (up to 10 min) until it is confirmed. A restart
// from Settings or after saving Wi-Fi in the first minute after an update undid the update.
void ota_restart_when_safe(void);
// What's new between the running version and the offered one, from the site's notes.json (CHANGELOG.md):
// sections separated by a blank line, each a "vX.Y.Z|Month D, YYYY" header line then one line per change.
// Empty if nothing is offered or the notes couldn't be read.
void ota_get_notes(char *out, size_t size);
