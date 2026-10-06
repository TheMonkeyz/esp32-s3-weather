#pragma once
#include "weather.h"
#include "lvgl.h"
#include "alerts.h"
#include "ota.h"

void ui_init(void);
void ui_message(const char *title, const char *body);
void ui_message_qr(const char *title, const char *body, const char *qr);
// Places: one weather page each, in a vertical pager (swipe up / down). ui_places() sets how many and which one
// is shown (alerts, hourly view, extras follow it); ui_place() fills page i (w NULL: "Loading...").
void ui_places(int n, int active);
void ui_place(int i, const char *name, const weather_t *w);
void ui_on_place_select(void (*cb)(int i));   // the user scrolled to place i
int ui_bench_screens(lv_obj_t **scr, const char **name, int max);   // diag bench
lv_obj_t *ui_main_screen(void);
void ui_wifi_setup(const char *note);   // Wi-Fi setup screen (setup network QR / Android Easy Connect)
void ui_wifi_setup_end(void);            // online again: stop Easy Connect and the auto-close timer
bool ui_wifi_setup_open(void);           // false once closed (a tap, or the timeout)
bool ui_wifi_setup_close(void);          // close it now, unless a phone is on the setup network
// Place i's last forecast attempt: ok (the page's "Updated N min ago" counts from now) or failed (its page says it
// can't reach the service while it has no forecast; an old one gets an age line)
void ui_place_state(int i, bool ok);
// Once: the "choose your location" settings QR (a new display) and/or the gesture hint (after it, or alone)
void ui_first_run(bool location, bool gestures);
void ui_pages(int *place, int *day, int *places, int *days);   // test console (display lock held)
const char *ui_screen_name(void);        // "weather", "radar", "settings", "setup0"... (display lock held)
bool ui_alert_sample(const char *which, int *title_h, int *lines, int *box_y);   // test console (lock held)
bool ui_setup_fail_sample(int *lines, int *bottom);  // test console (lock held): Easy Connect's failure text
void ui_alerts(const alerts_t *al);     // weather alerts for the location (empty = none)
void ui_alert_map(uint16_t *buf, int w, int h);   // region map for the alert screen (takes ownership; NULL hides)
void ui_alert_map_show(bool show);   // the map held: hidden (another place shown) or shown again (back on its place)
void ui_air(const air_t *a);            // air quality for the extras page
void ui_ota(const ota_status_t *st);    // update availability / progress (from the OTA task)
// Renders a screen off-display (weather, extras, status, radar, update, alert, hourly0..hourly6 = hourly view of
// that day; anything else = the one shown)
// into an RGB565 buffer. Display lock held; free it with lv_draw_buf_destroy() under the lock.
lv_draw_buf_t *ui_snapshot(const char *screen);
void ui_units_changed(void);            // units / clock / language changed: redraw everything that shows them
void ui_settings_changed(void);         // web.c: presence, sound or update settings changed (Settings' picture)
void ui_on_data_refresh(void (*cb)(void));   // language changed on the display: main.c fetches alerts again
