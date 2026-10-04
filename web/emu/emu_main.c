// The display in the browser: LVGL and the firmware's own screens (ui.c, slide.c, pager.c), its forecast, air-quality
// and alerts code (weather.c, alerts.c) and its settings (config.c, i18n.c), with the hardware replaced by the page
// (emu_display.c, emu_touch.c, emu_http.c, emu_nvs.c, emu_stubs.c). This loop does what main.c does once Wi-Fi is up.
#include <stdlib.h>
#include <emscripten.h>
#include "lvgl.h"
#include "esp_timer.h"
#include "display.h"
#include "ui.h"
#include "config.h"
#include "weather.h"
#include "alerts.h"
#include "i18n.h"
#include "freertos/task.h"

#define REFRESH_US (10 * 60 * 1000000LL)          // as the display: every 10 min

static weather_t wx[MAX_PLACES];
static location_t wx_at[MAX_PLACES];
static bool have[MAX_PLACES];
static bool extras_now = true;

static bool cached(int i, const location_t *loc)
{
    return have[i] && wx_at[i].lat == loc->lat && wx_at[i].lon == loc->lon;
}

static void show_place(bool all)
{
    int a = config_active_place(), n = config_place_count();
    location_t loc;
    if (config_get_place(a, &loc) && cached(a, &loc)) config_set_utc_offset(wx[a].utc_offset);
    ui_places(n, a);
    for (int i = 0; i < n; i++) {
        if ((!all && i != a) || !config_get_place(i, &loc)) continue;
        ui_place(i, loc.name, cached(i, &loc) ? &wx[i] : NULL);
    }
}

static void place_select(int i)                    // the pager settled on place i
{
    config_select_place(i);
    show_place(false);
    static const alerts_t none;
    static const air_t no_air = { .us_aqi = -1, .pollen = { -1, -1, -1, -1 } };
    ui_alerts(&none);
    ui_air(&no_air);
    extras_now = true;
}

static void data_refresh(void) { extras_now = true; }

static uint32_t tick(void) { return (uint32_t)emscripten_get_now(); }

static void run_lvgl(int ms)                       // LVGL for a while, giving the browser its turn
{
    int64_t end = esp_timer_get_time() + ms * 1000LL;
    do {
        emu_tasks_run();                           // the radar task (radar.c), until its next wait
        uint32_t wait = lv_timer_handler();
        emscripten_sleep(wait > 15 ? 15 : wait < 1 ? 1 : wait);
    } while (esp_timer_get_time() < end);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    display_init();
    ui_init();
    ui_message(tr(T_WEATHER), tr(T_FETCHING));
    ui_on_place_select(place_select);
    ui_on_data_refresh(data_refresh);
    run_lvgl(50);
    int64_t due[MAX_PLACES] = { 0 };
    bool shown_once = false;
    for (;;) {
        int a = config_active_place(), n = config_place_count();
        int64_t now = esp_timer_get_time();
        for (int k = 0; k < n; k++) {               // the place shown first
            int i = k == 0 ? a : k <= a ? k - 1 : k;
            location_t p;
            if (!config_get_place(i, &p) || (now < due[i] && cached(i, &p))) continue;
            static weather_t w;
            bool ok = weather_fetch(&p, &w);
            due[i] = esp_timer_get_time() + (ok ? REFRESH_US : 30 * 1000000LL);
            if (ok) { wx[i] = w; wx_at[i] = p; have[i] = true; }
            if (i == a || !shown_once) show_place(!shown_once);
            else { location_t q; if (config_get_place(i, &q)) ui_place(i, q.name, &wx[i]); }
            ui_place_state(i, ok);
            if (i == a && ok && !shown_once) {
                shown_once = true;
                bool gest = config_gesture_hint_wanted();   // the gesture hint; not the location one (its QR code
                if (gest) { ui_first_run(false, true); config_gesture_hint_done(); }   // leads to the display's page)
            }
            run_lvgl(10);
        }
        location_t loc;
        if (shown_once && extras_now && config_get_place(config_active_place(), &loc)) {
            extras_now = false;
            static alerts_t al;
            if (alerts_fetch(loc.lat, loc.lon, &al)) ui_alerts(&al);
            static air_t air;
            if (air_fetch(&air)) ui_air(&air);
        }
        run_lvgl(1000);
    }
}
