#pragma once

// The display's test console commands (screen, page, tap, press, swipe, drag, wake, presence, fps, pictest, alert,
// dirty, hint, bench, memspeed), the "where" breadcrumbs of the moves and the display, and the periodic
// "diag: display" line. After testcon_start() (its memspeed replaces the built-in one).
void console_init(void);
