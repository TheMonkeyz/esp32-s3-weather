#pragma once
#include "weather.h"
#include "lvgl.h"
#include "alerts.h"
#include "ota.h"

void ui_init(void);
void ui_message(const char *title, const char *body);
void ui_weather(const weather_t *w);
void ui_message_qr(const char *title, const char *body, const char *qr);
void ui_set_city(const char *name);
int ui_bench_screens(lv_obj_t **scr, const char **name, int max);   // diag bench
lv_obj_t *ui_main_screen(void);
void ui_wifi_setup(const char *note);   // Wi-Fi setup screen (setup network QR / Android Easy Connect)
void ui_wifi_setup_end(void);            // online again: stop Easy Connect and the auto-close timer
void ui_alerts(const alerts_t *al);     // weather alerts for the location (empty = none)
void ui_alert_map(uint16_t *buf, int w, int h);   // region map for the alert screen (takes ownership; NULL hides)
void ui_air(const air_t *a);            // air quality for the extras page
void ui_ota(const ota_status_t *st);    // update availability / progress (from the OTA task)
// Renders a screen off-display (weather, extras, status, radar, update, alert, hourly0..hourly6 = hourly view of
// that day; anything else = the one shown)
// into an RGB565 buffer. Display lock held; free it with lv_draw_buf_destroy() under the lock.
lv_draw_buf_t *ui_snapshot(const char *screen);
