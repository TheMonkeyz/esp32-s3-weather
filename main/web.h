#pragma once
#include <stdbool.h>

typedef void (*web_location_cb_t)(void);

// Start the settings page (HTTPS :443, HTTP :80 redirects). Callback runs after a new location is saved.
void web_start(web_location_cb_t on_location_changed);
void web_test_windows_quiet(bool on);   // test console: no captive-portal popup on the test PC (until restart)
