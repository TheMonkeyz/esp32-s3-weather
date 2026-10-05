#pragma once

// The settings page's app routes (/api/config, location, units, places, presence, calibrate, sound), the page itself
// and the snapshot hook, handed to forge_net's web server. Before web_start(): routes added later are not served.
// on_location_changed: the place list or the language changed (the main loop refetches).
void routes_init(void (*on_location_changed)(void));
