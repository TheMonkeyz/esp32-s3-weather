#pragma once
#include <stdbool.h>

typedef enum { PRESENCE_ACTIVE = 0, PRESENCE_DIM = 1, PRESENCE_OFF = 2 } presence_state_t;

typedef struct {
    bool  enabled;
    float margin_db;      // noise must exceed baseline + margin to count as "loud"
    float wake_s;         // seconds of (mostly) continuous noise needed to wake from DIM/OFF
    float dim_s;          // quiet seconds before dimming
    float off_s;          // further quiet seconds before the screen turns off
    int   bright_pct;     // normal brightness
    int   dim_pct;        // dimmed brightness
    float baseline_db;    // background noise (dBFS), set by calibration
} presence_cfg_t;

typedef struct {
    float level_db, threshold_db, wake_progress, quiet_s, calib_left_s;
    presence_state_t state;
    bool  calibrating, mic_ok;
    int   brightness;
    bool  imu_ok;         // motion sensor found
    float motion_g;       // recent movement (change from the resting position, g; peak, decays in ~1 s)
    float motion_thr;     // movement that wakes it (g)
} presence_status_t;

void presence_start(void);                          // after touch_init (shares its I2C bus)
void presence_get_config(presence_cfg_t *out);
void presence_set_config(const presence_cfg_t *in); // saves to NVS (baseline is kept)
void presence_get_status(presence_status_t *st);
bool presence_calibrate(int seconds);               // measure background noise; keep quiet meanwhile
void presence_wake(void);
bool presence_touch(void);                          // returns true if the screen was off (swallow the touch)
bool presence_screen_off(void);
void presence_preview_brightness(int pct);         // while dragging a slider: applied at once, not saved
bool presence_motion_wake(void);                    // wake on pick-up / movement (saved)
void presence_set_motion(bool on, float threshold_g);   // threshold 0.02..0.5 g (saved)
