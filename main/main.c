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
static void offline_setup(const char *ssid)
{
    char note[96], body[160];
    snprintf(note, sizeof(note), tr(T_CANT_REACH), ssid);
    snprintf(body, sizeof(body), tr(T_CONNECTING), ssid);
    while (1) {
        if (!ui_wifi_setup_open()) ui_wifi_setup(note);   // setup network (page 1) or Easy Connect (page 2)
        while (ui_wifi_setup_open() && !net_is_connected()) vTaskDelay(pdMS_TO_TICKS(500));
        if (net_is_connected()) break;
        ui_message("Wi-Fi", body);
        if (net_wait_connected(30000)) break;
        ESP_LOGW(TAG, "Still can't reach \"%s\", offering the setup network again", ssid);
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

    char body[160];
    snprintf(body, sizeof(body), tr(T_CONNECTING), ssid);
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
    int64_t next_all = 0;                  // when every place's forecast is due again
    while (1) {
        int64_t now = esp_timer_get_time();
        int a = config_active_place();
        location_t loc;
        config_get_place(a, &loc);
        bool ok = net_is_connected();
        if (ok && now >= next_all) {                         // every place, active first
            ok = fetch_place(a);
            for (int i = 0; i < config_place_count(); i++) if (i != a) fetch_place(i);
            next_all = now + (ok ? REFRESH_MIN * 60 : 30) * 1000000LL;
        } else if (ok) {                                     // woken by a switch or an edit: only what's missing
            for (int k = 0; k < config_place_count(); k++) {   // the place shown first, then new / edited ones
                int i = k == 0 ? a : k <= a ? k - 1 : k;
                location_t p;
                if (!config_get_place(i, &p)) continue;
                display_lock(-1);
                bool c = cached(i, &p);
                display_unlock();
                if (!c && !fetch_place(i) && i == a) ok = false;
            }
        }
        if (ok && a == config_active_place()) {              // (unless the user swiped on meanwhile)
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
                    if (m) { ui_alert_map(m, ALERT_MAP_W, ALERT_MAP_H); strlcpy(map_key, key, sizeof(map_key)); }
                    else strlcpy(map_failed, key, sizeof(map_failed));   // (a 404 was retried every cycle)
                }
            }
            static air_t air;
            if (air_fetch(&air) && a == config_active_place()) ui_air(&air);
            static bool first = true;
            if (first) { first = false; diag_mark("first weather"); }
        } else if (!ok) {
            ESP_LOGW(TAG, "Update failed, retrying in 30 s");
            next_all = now + 30 * 1000000LL;
        }
        int64_t wait_ms = (next_all - esp_timer_get_time()) / 1000;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms > 1000 ? wait_ms : 1000));   // woken early by a switch / edit
    }
}
