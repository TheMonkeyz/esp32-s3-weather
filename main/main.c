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

// Called by the web UI after a new location is saved
static void on_location_changed(void)
{
    location_t loc;
    config_get_location(&loc);
    ui_set_city(loc.name);
    radar_relocate();
    if (main_task) xTaskNotifyGive(main_task);   // refetch weather now
}


// The saved network can't be reached (moved house, new router, router still booting after a power
// cut...): offer the setup network with its QR code while retrying the saved one in the background.
// Whichever happens first wins: new credentials restart the board, a connection carries on normally.
static void offline_setup(const char *ssid)
{
    char note[96];
    snprintf(note, sizeof(note), "Can't reach %s\n(still trying)", ssid);
    ui_wifi_setup(note);                       // starts the setup network (page 1) or Easy Connect (page 2)
    net_wait_connected(-1);
    ESP_LOGI(TAG, "Saved network is back");
    ui_wifi_setup_end();
    ui_message("Wi-Fi", "Connected");
    for (int i = 0; i < 120 && net_ap_clients() > 0; i++) vTaskDelay(pdMS_TO_TICKS(1000));   // let a phone finish
    net_setup_ap_stop();
}

static void portal(void)
{
    net_start_portal();
    web_start(on_location_changed);
    ui_wifi_setup("First-time setup");
    while (1) vTaskDelay(portMAX_DELAY);     // restarts after credentials are saved
}

void app_main(void)
{
    ESP_LOGI(TAG, "Weather display starting");
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
    diag_mark("presence");
    ui_init();
    diag_mark("ui");
    ui_message("Weather", "Starting...");

    if (boot_button_held()) {
        net_clear_creds();
    }

    char ssid[33] = {0}, pass[65] = {0};
    if (!net_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "No saved Wi-Fi, starting setup portal");
        portal();
    }

    char body[128];
    snprintf(body, sizeof(body), "Connecting to\n%s\n\nLong-press for Wi-Fi setup", ssid);
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
    ui_message("Weather", "Fetching forecast...\n\nLong-press for Wi-Fi setup");
    static EXT_RAM_BSS_ATTR weather_t w;   // ~2 KB of hourly data: PSRAM, off the stack
    while (1) {
        int wait_s = REFRESH_MIN * 60;
        if (net_is_connected() && weather_fetch(&w)) {
            ui_weather(&w);
            static EXT_RAM_BSS_ATTR alerts_t al;
            location_t loc;
            config_get_location(&loc);
            if (alerts_fetch(loc.lat, loc.lon, &al)) {                 // failed: keep showing the last ones
                ui_alerts(&al);
                static char map_id[80];                                 // map of the top alert's region
                if (!al.n) { if (map_id[0]) { ui_alert_map(NULL, 0, 0); map_id[0] = 0; } }
                else if (strcmp(map_id, al.a[0].id)) {
                    uint16_t *m = alerts_map(&al.a[0], loc.lat, loc.lon, ALERT_MAP_W, ALERT_MAP_H);
                    if (m) { ui_alert_map(m, ALERT_MAP_W, ALERT_MAP_H); strlcpy(map_id, al.a[0].id, sizeof(map_id)); }
                }
            }
            static bool first = true;
            if (first) { first = false; diag_mark("first weather"); }
        } else {
            ESP_LOGW(TAG, "Update failed, retrying in 30 s");
            wait_s = 30;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_s * 1000));   // woken early on location change
    }
}
