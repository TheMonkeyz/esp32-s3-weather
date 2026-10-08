#pragma once
#include <stdbool.h>

// Speaker (ES8311 + amplifier, I2S shared with the microphones): chimes for new weather alerts.
// The sounds are synthesised beeps whose urgency follows the alert level, no audio files. Settings in NVS namespace "sound".

typedef struct {
    int level;            // 0 off, 1 red only, 2 orange and red, 3 all alerts (yellow and statements too)
    int volume;           // 0..100
    int quiet_from, quiet_to;   // quiet hours in minutes after midnight (local time of the place shown);
                                // equal = no quiet hours. Red alerts still sound during quiet hours.
} sound_cfg_t;

void sound_start(void);                    // after audio_init() (audio.c: the shared I2S bus)
void sound_get_config(sound_cfg_t *out);
bool sound_set_config(const sound_cfg_t *in);   // saved; false if NVS refused (still applied)
// A new alert of this colour ('r' red, 'o' orange, 'y' yellow, 'g' statement/grey) appeared: chime if the level
// setting includes it and it isn't quiet hours (red always sounds).
void sound_alert(char colour);
void sound_test(int level);                // play the sound for level 1 yellow / 2 orange / 3 red now, whatever
                                           // the settings
bool sound_ok(void);                       // speaker found

// Does an alert of severity sev (3 red, 2 orange, 1 yellow, 0 statement) sound, for the level setting and the quiet
// hours, at minute `now` of the day (-1: the clock isn't set)? Red always does; equal from/to = no quiet hours; quiet
// hours may cross midnight. (tests/host/test_version.c)
static inline bool sound_wanted(int level, int sev, int quiet_from, int quiet_to, int now)
{
    int min = level == 1 ? 3 : level == 2 ? 2 : 0;
    if (level <= 0 || sev < min) return false;
    if (sev >= 3 || quiet_from == quiet_to || now < 0) return true;
    bool quiet = quiet_from < quiet_to ? now >= quiet_from && now < quiet_to
                                       : now >= quiet_from || now < quiet_to;   // across midnight
    return !quiet;
}
