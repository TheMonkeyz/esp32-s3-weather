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

void sound_start(void);                    // after presence_start() (it opens the shared I2S bus)
void sound_get_config(sound_cfg_t *out);
bool sound_set_config(const sound_cfg_t *in);   // saved; false if NVS refused (still applied)
// A new alert of this colour ('r' red, 'o' orange, 'y' yellow, 'g' statement/grey) appeared: chime if the level
// setting includes it and it isn't quiet hours (red always sounds).
void sound_alert(char colour);
void sound_test(int level);                // play the sound for level 1 yellow / 2 orange / 3 red now, whatever
                                           // the settings
bool sound_ok(void);                       // speaker found
