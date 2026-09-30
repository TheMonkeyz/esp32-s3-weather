// Simple weather display for Waveshare ESP32-S3-Touch-AMOLED-1.75
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "display.h"
#include "net.h"
#include "ui.h"
#include "weather.h"
#include "touch.h"
#include "config.h"
#include "radar.h"
#include "web.h"
#include "presence.h"

static const char *TAG = "app";
#define BOOT_BTN        GPIO_NUM_0
#define REFRESH_MIN     10

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

static void portal(void)
{
    char body[200];
    snprintf(body, sizeof(body),
             "Scan to join the display's Wi-Fi\n(%s / %s).\nThe setup page opens by itself.",
             SETUP_AP_SSID, SETUP_AP_PASS);
    ui_message_qr("Wi-Fi setup", body, "WIFI:T:WPA;S:" SETUP_AP_SSID ";P:" SETUP_AP_PASS ";;");
    net_start_portal();
    web_start(on_location_changed);
    while (1) vTaskDelay(portMAX_DELAY);     // restarts after credentials are saved
}

void app_main(void)
{
    ESP_LOGI(TAG, "Weather display starting");
    main_task = xTaskGetCurrentTaskHandle();
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);   // until the weather service reports the local offset
    tzset();
    display_init();
    touch_init();
    net_init();                 // also initialises NVS (settings)
    presence_start();           // microphones -> screen brightness (uses touch's I2C bus + NVS)
    ui_init();
    ui_message("Weather", "Starting...");

    if (boot_button_held()) {
        net_clear_creds();
    }

    char ssid[33] = {0}, pass[65] = {0};
    if (!net_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "No saved Wi-Fi, starting setup portal");
        portal();
    }

    char body[96];
    snprintf(body, sizeof(body), "Connecting to\n%s", ssid);
    ui_message("Wi-Fi", body);
    if (!net_connect(ssid, pass, 20000)) {
        ESP_LOGW(TAG, "Wi-Fi connect failed, starting setup portal");
        portal();
    }

    web_start(on_location_changed);
    radar_preload_start();      // missing zoom-level maps download in the background
    ui_message("Weather", "Fetching forecast...");
    static weather_t w;         // ~1 KB of hourly data, keep it off the stack
    while (1) {
        int wait_s = REFRESH_MIN * 60;
        if (net_is_connected() && weather_fetch(&w)) {
            ui_weather(&w);
        } else {
            ESP_LOGW(TAG, "Update failed, retrying in 30 s");
            wait_s = 30;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_s * 1000));   // woken early on location change
    }
}
