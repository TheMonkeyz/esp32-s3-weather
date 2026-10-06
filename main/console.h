#pragma once

#include <stdbool.h>

// The display's test console commands (screen, page, tap, press, swipe, drag, wake, presence, fps, pictest, alert,
// dirty, memlow, hint, bench, memspeed), the "where" breadcrumbs of the moves and the display, and the periodic
// "diag: display" line. After testcon_start() (its memspeed replaces the built-in one).
void console_init(void);
// "alert at LAT LON | off": main.c looks the first place's alerts up at that point instead (RAM only, for tests)
void console_on_alert_at(void (*fn)(bool on, double lat, double lon));
