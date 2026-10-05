// The display in the browser: LVGL and the firmware's own screens (ui.c, slide.c, pager.c), its forecast, air-quality
// and alerts code (weather.c, alerts.c) and its settings (config.c, i18n.c), with the hardware replaced by the page
// (emu_display.c, emu_touch.c, emu_http.c, emu_nvs.c, emu_stubs.c). This loop does what main.c does once Wi-Fi is up.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include "lvgl.h"
#include "esp_timer.h"
#include "display.h"
#include "ui.h"
#include "config.h"
#include "weather.h"
#include "alerts.h"
#include "i18n.h"
#include "services.h"
#include "freertos/task.h"
#include "sound.h"
#include "nvs.h"

#define REFRESH_US (10 * 60 * 1000000LL)          // as the display: every 10 min
#define ALERT_MAP_W 300                            // the region map on the alert screen (as main.c)
#define ALERT_MAP_H 200

// A place given in the page's address (?place=lat,lon,Name), e.g. a link to a place with an alert: it becomes the
// place shown (kept, as if chosen on the phone)
EM_JS(int, js_place_param, (char *out, int n), {
    const v = new URLSearchParams(location.search).get('place');
    if (!v) return 0;
    stringToUTF8(v, out, n);
    return 1;
});

// Two more places, so a visitor can drag between places from the start: Vancouver and Iqaluit after the first one
// (Québec City by default). Added once (NVS "emu"/"places"), to a visitor with a single place: places deleted later stay
// deleted, and a visitor's own places are kept.
static void default_places(void)
{
    static const location_t more[] = {
        { .name = "Vancouver", .lat = 49.2827, .lon = -123.1207 },
        { .name = "Iqaluit", .lat = 63.7467, .lon = -68.5170 },
    };
    nvs_handle_t h;
    uint8_t done = 0;
    if (nvs_open("emu", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_get_u8(h, "places", &done);
    if (!done && config_place_count() == 1)
        for (int i = 0; i < (int)(sizeof(more) / sizeof(more[0])); i++) config_set_place(config_place_count(), &more[i]);
    if (!done) { nvs_set_u8(h, "places", 1); nvs_commit(h); }
    nvs_close(h);
}

static void place_from_address(void)
{
    char v[96], name[48] = "";
    double lat, lon;
    if (!js_place_param(v, sizeof(v)) || sscanf(v, "%lf,%lf,%47[^\n]", &lat, &lon, name) < 2) return;
    if (lat < -85 || lat > 85 || lon < -180 || lon > 180) return;
    location_t l = { .lat = lat, .lon = lon };
    snprintf(l.name, sizeof(l.name), "%s", name[0] ? name : "Here");
    config_set_location(&l);
}

static weather_t wx[MAX_PLACES];
static location_t wx_at[MAX_PLACES];
static bool have[MAX_PLACES];
static bool extras_now = true;
static char map_key[80], map_failed[80];          // the region map shown / failed (as main.c)
static alerts_seen_t seen;                         // alerts already heard for the place shown (as main.c)

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
    ui_alert_map(NULL, 0, 0);
    map_key[0] = map_failed[0] = 0;
    memset(&seen, 0, sizeof(seen));                // the new place's current alerts don't sound
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
    app_text_init();                               // the texts and languages (forge_core's i18n), as the firmware
    services_init();                               // the outside services in the firmware's order, then its two
    svc_add("GitHub Pages", "updates", NULL);      // (forge_ota's update site: no updates in the browser)
    svc_add("Browser clock", "", NULL);            // (forge_net's NTP: the browser's clock)
    lv_init();
    lv_tick_set_cb(tick);
    display_init();
    default_places();
    place_from_address();
    ui_init();
    sound_start();                                 // sound.c: alert sounds through Web Audio (emu_audio.c)
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
            if (alerts_fetch(loc.lat, loc.lon, &al)) {
                ui_alerts(&al);
                char c = alerts_to_sound(&seen, &al);     // new or worse: a sound, as on the display
                if (c) sound_alert(c);
                char key[80];
                if (al.n) alerts_map_key(&al.a[0], key, sizeof(key));
                if (!al.n) { if (map_key[0]) { ui_alert_map(NULL, 0, 0); map_key[0] = 0; } }
                else if (strcmp(map_key, key) && strcmp(map_failed, key)) {   // map of the top alert's region
                    uint16_t *m = alerts_map(&al.a[0], loc.lat, loc.lon, ALERT_MAP_W, ALERT_MAP_H);
                    if (m) { ui_alert_map(m, ALERT_MAP_W, ALERT_MAP_H); snprintf(map_key, sizeof(map_key), "%s", key); }
                    else snprintf(map_failed, sizeof(map_failed), "%s", key);
                }
            }
            static air_t air;
            if (air_fetch(&air)) ui_air(&air);
        }
        run_lvgl(1000);
    }
}
