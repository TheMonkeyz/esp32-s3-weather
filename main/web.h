#pragma once

typedef void (*web_location_cb_t)(void);

// Start the settings page (HTTPS :443, HTTP :80 redirects). Callback runs after a new location is saved.
void web_start(web_location_cb_t on_location_changed);
