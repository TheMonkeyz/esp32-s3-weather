// Simple weather display for Waveshare ESP32-S3-Touch-AMOLED-1.75
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "display.h"
#include "net.h"
#include "ui.h"
#include "weather.h"
#include "touch.h"
#include "config.h"
#include "radar.h"
#include "web.h"
#include "presence.h"
#include "diag.h"
#include "alerts.h"
#include "ota.h"
#include "esp_timer.h"
#include "i18n.h"
#include "testcon.h"
#include "sound.h"
#include "textfit.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

static const char *TAG = "app";
#define BOOT_BTN        GPIO_NUM_0
#define REFRESH_MIN     10
#define ALERT_MAP_W     300
#define ALERT_MAP_H     200

static bool boot_button_held(void)
{
    gpio_config_t c = { .pin_bit_mask = 1ULL << BOOT_BTN, .mode = GPIO_MODE_INPUT, .pull_up_en = 1 };
    gpio_config(&c);
    for (int i = 0; i < 20; i++) {           // must be held ~1 s
        if (gpio_get_level(BOOT_BTN)) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

static TaskHandle_t main_task;
static volatile bool extras_now;             // a switch, an edit or a language change: alerts and air quality now

// cJSON's trees (a forecast is thousands of small nodes) in PSRAM: they went to internal RAM, whose low point was
// under 10 KB, and peaked when three replies were parsed at once on a reconnect
static void *json_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

/* ---------- places ----------
 * Every place's forecast is refreshed every REFRESH_MIN and kept here; each place has its own page on the weather
 * screen (a vertical pager), so scrolling to a place shows its weather at once. Alerts, air quality and the radar are only for the place shown; they load after a switch. A cached forecast
 * belongs to the coordinates it was fetched for (wx_at), so edits and deletions on the settings page can't show
 * one place's weather under another's name. wx / wx_at / have are read and written under the display lock. */
static EXT_RAM_BSS_ATTR weather_t wx[MAX_PLACES];
static location_t wx_at[MAX_PLACES];
static bool have[MAX_PLACES];
static location_t shown;                  // the place alerts, air quality and radar are for
static char map_key[80];                  // region map shown (alerts_map_key: alert code + region)
static char map_failed[80];               // its download failed: not retried every 10 min until the alert changes

// Alerts already heard for the place shown (alerts_to_sound: once per warning, again if it gets worse; the ones there
// at start-up or after a switch are only recorded)
static alerts_seen_t seen;

static void chime_new_alerts(const alerts_t *al)
{
    char c = alerts_to_sound(&seen, al);
    if (c) sound_alert(c);
}

static bool cached(int i, const location_t *loc)     // display lock held
{
    return have[i] && wx_at[i].lat == loc->lat && wx_at[i].lon == loc->lon;
}

// Pages for the place shown (all: every place, after the list changed): its forecast, else "Loading..."
static void show_place(bool all)
{
    int a = config_active_place(), n = config_place_count();
    display_lock(-1);
    location_t loc;
    if (config_get_place(a, &loc) && cached(a, &loc)) config_set_utc_offset(wx[a].utc_offset);
    ui_places(n, a);
    for (int i = 0; i < n; i++) {
        if ((!all && i != a) || !config_get_place(i, &loc)) continue;
        ui_place(i, loc.name, cached(i, &loc) ? &wx[i] : NULL);
    }
    display_unlock();
}

// The place shown, or the list, changed: redraw, and point alerts / air quality / radar at the place shown
static void follow_active(bool all)
{
    location_t loc;
    config_get_location(&loc);
    bool moved = loc.lat != shown.lat || loc.lon != shown.lon;
    shown = loc;
    show_place(all);
    if (moved) {
        static const alerts_t none;
        static const air_t no_air = { .us_aqi = -1, .pollen = { -1, -1, -1, -1 } };
        ui_alerts(&none);
        ui_alert_map(NULL, 0, 0);
        map_key[0] = map_failed[0] = 0;
        memset(&seen, 0, sizeof(seen));          // the new place's current alerts don't chime
        ui_air(&no_air);
        radar_relocate();
    }
    extras_now = true;                           // alerts and air quality for the place shown, now
    if (main_task) xTaskNotifyGive(main_task);   // fetch what's missing now
}

// The weather screen's pager settled on place i (LVGL task)
static void place_select(int i)
{
    config_select_place(i);
    follow_active(false);
}

// Called by the web UI after a place is added, edited, deleted or chosen, or the language changed
static void on_location_changed(void)
{
    follow_active(true);
}

// The language changed on the display: wake the loop (alerts are re-shown, notes re-fetched in the new language)
static void data_refresh(void)
{
    extras_now = true;
    ota_check_now();
    if (main_task) xTaskNotifyGive(main_task);
}

static bool fetch_place(int i)
{
    static EXT_RAM_BSS_ATTR weather_t w;          // only the main task fetches
    location_t loc;
    if (!config_get_place(i, &loc) || !weather_fetch(&loc, &w)) return false;
    display_lock(-1);
    wx[i] = w;
    wx_at[i] = loc;
    have[i] = true;
    if (i == config_active_place()) config_set_utc_offset(w.utc_offset);
    ui_place(i, loc.name, &wx[i]);
    display_unlock();
    return true;
}


// The saved network can't be reached (moved house, new router, router still booting after a power
// cut...): offer the setup network with its QR code. The saved network isn't tried while setup is open (the
// attempts would knock phones off it); closing it (a tap, or 5 idle minutes) tries the saved network for 30 s,
// then setup comes back. New credentials restart the board; a connection carries on normally.
// For AUTO_SETUP_S only: after that (a long router outage) the setup network stops opening by itself and the
// display just keeps trying the saved network; a long-press still opens setup (the owner's choice, October 2026:
// a setup network left open for hours is a way in for anyone nearby).
#define AUTO_SETUP_S (15 * 60)
static void offline_setup(const char *saved)
{
    char ssid[NET_SSID_MAX + 1];
    textfit(saved, ssid, sizeof(ssid));                     // shown on screen: without characters the fonts lack
    char note[160], body[160], still[160];        // (Inuktitut's note overflowed 96 bytes for long network names)
    snprintf(note, sizeof(note), tr(T_CANT_REACH), ssid);
    snprintf(body, sizeof(body), tr(T_CONNECTING), ssid);
    snprintf(still, sizeof(still), tr(T_STILL_TRYING), ssid);
    int64_t until = esp_timer_get_time() + (net_test_short_setup() ? 60 : AUTO_SETUP_S) * 1000000LL;
    bool gave_up = false;
    while (1) {
        bool by_itself = esp_timer_get_time() < until;
        bool opened = by_itself && !ui_wifi_setup_open();
        if (opened) ui_wifi_setup(note);                  // setup network (page 1) or Easy Connect (page 2)
        while (ui_wifi_setup_open() && !net_is_connected()) {
            // The window ends while it is open: it closes, unless a phone is on it (one the user opened stays)
            if (opened && esp_timer_get_time() >= until && ui_wifi_setup_close()) break;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (net_is_connected()) break;
        if (!gave_up && esp_timer_get_time() >= until) {
            gave_up = true;
            ESP_LOGW(TAG, "Setup network: no longer opened by itself (long-press opens it); still trying \"%s\"", ssid);
        }
        ui_message("Wi-Fi", gave_up ? still : body);
        if (net_wait_connected(30000)) break;
        if (!gave_up) ESP_LOGW(TAG, "Still can't reach \"%s\", offering the setup network again", ssid);
    }
    ESP_LOGI(TAG, "Saved network is back");
    ui_wifi_setup_end();
    ui_message("Wi-Fi", tr(T_CONNECTED));
    for (int i = 0; i < 120 && net_ap_clients() > 0; i++) vTaskDelay(pdMS_TO_TICKS(1000));   // let a phone finish
    net_setup_ap_stop();
}

static void portal(void)
{
    net_start_portal();
    web_start(on_location_changed);
    ui_wifi_setup(tr(T_FIRST_SETUP));
    while (1) vTaskDelay(portMAX_DELAY);     // restarts after credentials are saved
}

void app_main(void)
{
    ESP_LOGI(TAG, "Weather display starting");
    cJSON_InitHooks(&(cJSON_Hooks){ .malloc_fn = json_alloc, .free_fn = free });
    main_task = xTaskGetCurrentTaskHandle();
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);   // until the weather service reports the local offset
    tzset();
    diag_mark("start");
    display_init();
    diag_mark("display");
    touch_init();
    net_init();                 // also initialises NVS (settings)
    diag_mark("net init");
    diag_start(60);             // "diag:" lines in the log every 60 s (heap, frames, CPU/stack per task)
    presence_start();           // microphones -> screen brightness (uses touch's I2C bus + NVS)
    sound_start();              // alert chimes (speaker shares the microphones' I2S bus)
    diag_mark("presence");
    ui_init();
    ota_start(ui_ota);          // update checks start once Wi-Fi is up; marks a new firmware valid after 60 s
    diag_mark("ui");
    testcon_start();            // USB test console (tools/harness), ready before Wi-Fi so start-up can be tested
    ui_message(tr(T_WEATHER), tr(T_STARTING));

    if (boot_button_held()) {
        net_clear_creds();
    }

    char ssid[33] = {0}, pass[65] = {0};
    if (!net_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "No saved Wi-Fi, starting setup portal");
        portal();
    }

    char body[160], fitted[sizeof(ssid)];
    textfit(ssid, fitted, sizeof(fitted));                    // on screen: without characters the fonts lack (emoji)
    snprintf(body, sizeof(body), tr(T_CONNECTING), fitted);
    ui_message("Wi-Fi", body);
    net_begin(ssid, pass);
    web_start(on_location_changed);   // up early, so a long-press can offer the setup page right away
    diag_mark("web");
    if (!net_wait(30000)) {
        ESP_LOGW(TAG, "Wi-Fi connect failed, offering the setup network");
        offline_setup(ssid);
    }
    diag_mark("wifi up");
    radar_preload_start();      // missing zoom-level maps download in the background
    ui_message(tr(T_WEATHER), tr(T_FETCHING));
    config_get_location(&shown);
    ui_on_place_select(place_select);
    ui_on_data_refresh(data_refresh);
    show_place(true);
    // Each place's forecast is due REFRESH_MIN after its last success. A failure is retried with a growing delay
    // (30 s, 1, 2, 5, 10 min; HTTP 429 "too many requests": 10 min): all places every 30 s was ~11,500 requests a
    // day in a day-long outage, over Open-Meteo's free quota. A new or edited place is fetched at once; the place
    // shown goes first. Alerts and air quality: with the place shown's forecast, every REFRESH_MIN or on a switch.
    static int64_t due[MAX_PLACES];         // 0 = now
    static int fails[MAX_PLACES];
    static location_t tried[MAX_PLACES];    // where the last attempt was for (an edit: fetch at once)
    int64_t extras_due = 0;
    bool shown_once = false;                 // a forecast has replaced the start-up message
    bool was_net = true;
    while (1) {
        int64_t now = esp_timer_get_time();
        int a = config_active_place(), n = config_place_count();
        location_t loc;
        config_get_place(a, &loc);
        bool net = net_is_connected(), ok = false;
        if (net && !was_net) {                               // Wi-Fi back: failed places now, from the first delay
            for (int i = 0; i < MAX_PLACES; i++) if (fails[i]) { fails[i] = 0; due[i] = 0; }
            extras_now = true;                               // (a place that had failed kept a 2-10 min wait:
        }                                                    // the screen came back minutes after Wi-Fi did)
        was_net = net;
        int64_t next = now + REFRESH_MIN * 60 * 1000000LL;
        for (int k = 0; k < n; k++) {                        // the place shown first
            int i = k == 0 ? a : k <= a ? k - 1 : k;
            location_t p;
            if (!config_get_place(i, &p)) continue;
            display_lock(-1);
            bool c = cached(i, &p);
            display_unlock();
            bool edited = tried[i].lat != p.lat || tried[i].lon != p.lon;
            if (net && (edited || now >= due[i])) {
                tried[i] = p;
                if (fetch_place(i)) {
                    c = true;
                    fails[i] = 0;
                    due[i] = esp_timer_get_time() + REFRESH_MIN * 60 * 1000000LL;
                    ui_place_state(i, true);
                } else {
                    static const int wait_s[] = { 30, 60, 120, 300, 600 };
                    int s = weather_last_status() == 429 ? 600 : wait_s[fails[i] < 4 ? fails[i] : 4];
                    fails[i]++;
                    due[i] = esp_timer_get_time() + s * 1000000LL;
                    ui_place_state(i, false);
                    ESP_LOGW(TAG, "Update failed for place %d (%d in a row), retrying in %d s", i + 1, fails[i], s);
                }
            }
            if (due[i] < next) next = due[i];
            if (i == a) ok = c;
        }
        if (!ok && !shown_once && net && fails[a]) ui_message(tr(T_WEATHER), tr(T_FORECAST_RETRY));   // at start-up
        if (ok && !shown_once) {                             // first forecast on screen: the one-time hints
            bool loc = config_place_is_default() && config_hint_wanted();   // a new display
            bool gest = config_gesture_hint_wanted();       // every display once (v1.12.0 added it)
            if (loc || gest) ui_first_run(loc, gest);
            if (loc) config_hint_done();
            if (gest) config_gesture_hint_done();
        }
        if (ok) shown_once = true;
        if (ok && a == config_active_place() && (extras_now || esp_timer_get_time() >= extras_due)) {
            extras_now = false;                              // (unless the user swiped on meanwhile)
            extras_due = esp_timer_get_time() + REFRESH_MIN * 60 * 1000000LL;
            if (extras_due < next) next = extras_due;
            show_place(false);
            static EXT_RAM_BSS_ATTR alerts_t al;
            if (alerts_fetch(loc.lat, loc.lon, &al) && a == config_active_place()) {   // failed: keep the last ones
                ui_alerts(&al);
                chime_new_alerts(&al);
                char key[80];
                if (al.n) alerts_map_key(&al.a[0], key, sizeof(key));
                if (!al.n) { if (map_key[0]) { ui_alert_map(NULL, 0, 0); map_key[0] = 0; } }
                else if (strcmp(map_key, key) && strcmp(map_failed, key)) {   // map of the top alert's region
                    uint16_t *m = alerts_map(&al.a[0], loc.lat, loc.lon, ALERT_MAP_W, ALERT_MAP_H);
                    ESP_LOGI(TAG, "alert map %s, main task stack %u B spare", m ? "drawn" : "failed",
                             (unsigned)uxTaskGetStackHighWaterMark(NULL));
                    if (m) { ui_alert_map(m, ALERT_MAP_W, ALERT_MAP_H); strlcpy(map_key, key, sizeof(map_key)); }
                    else strlcpy(map_failed, key, sizeof(map_failed));   // (a 404 was retried every cycle)
                }
            }
            static air_t air;
            if (air_fetch(&air) && a == config_active_place()) ui_air(&air);
            static bool first = true;
            if (first) { first = false; diag_mark("first weather"); }
        }
        if (extras_due && extras_due < next) next = extras_due;
        if (!net && next < esp_timer_get_time() + 5000000LL) next = esp_timer_get_time() + 5000000LL;   // offline: look again in 5 s
        int64_t wait_ms = (next - esp_timer_get_time()) / 1000;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms > 1000 ? wait_ms : 1000));   // woken early by a switch / edit
    }
}
