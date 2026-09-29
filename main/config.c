// Persistent settings (location) + local-time helper
#include <time.h>
#include "config.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "config";
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static location_t cur;
static bool loaded;
static volatile int utc_offset;
static volatile bool offset_known;

static void load(void)
{
    if (loaded) return;
    strcpy(cur.name, "Québec");
    cur.lat = 46.8139;
    cur.lon = -71.2080;
    nvs_handle_t h;
    if (nvs_open("loc", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(cur.name);
        int64_t la, lo;
        char name[48];
        if (nvs_get_str(h, "name", name, &n) == ESP_OK &&
            nvs_get_i64(h, "lat", &la) == ESP_OK && nvs_get_i64(h, "lon", &lo) == ESP_OK) {
            strlcpy(cur.name, name, sizeof(cur.name));
            cur.lat = la / 1e6;
            cur.lon = lo / 1e6;
        }
        nvs_close(h);
    }
    loaded = true;
    ESP_LOGI(TAG, "Location: %s (%.4f, %.4f)", cur.name, cur.lat, cur.lon);
}

void config_get_location(location_t *out)
{
    load();
    taskENTER_CRITICAL(&mux);
    *out = cur;
    taskEXIT_CRITICAL(&mux);
}

bool config_set_location(const location_t *loc)
{
    if (loc->lat < -85 || loc->lat > 85 || loc->lon < -180 || loc->lon > 180) return false;
    nvs_handle_t h;
    if (nvs_open("loc", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, "name", loc->name);
    nvs_set_i64(h, "lat", (int64_t)(loc->lat * 1e6));
    nvs_set_i64(h, "lon", (int64_t)(loc->lon * 1e6));
    nvs_commit(h);
    nvs_close(h);
    taskENTER_CRITICAL(&mux);
    cur = *loc;
    taskEXIT_CRITICAL(&mux);
    offset_known = false;   // new place may be in another time zone; weather fetch will update it
    ESP_LOGI(TAG, "Saved location: %s (%.4f, %.4f)", loc->name, loc->lat, loc->lon);
    return true;
}

void config_set_utc_offset(int seconds)
{
    utc_offset = seconds;
    offset_known = true;
}

bool config_local_time(long t, struct tm *out)
{
    if (t < 1600000000L) return false;          // clock not set by SNTP yet
    time_t tt = (time_t)t;
    if (offset_known) {
        tt += utc_offset;
        gmtime_r(&tt, out);
    } else {
        localtime_r(&tt, out);                   // fallback: TZ from net.c (Eastern)
    }
    return true;
}
