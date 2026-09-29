// Fetches current conditions + today's forecast from Open-Meteo (no API key needed)
#include "weather.h"
#include <string.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "weather";

#define URL_FMT "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f" \
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day" \
    "&daily=temperature_2m_max,temperature_2m_min,weather_code&timezone=auto&forecast_days=4"

typedef struct { char *buf; int len; int cap; } rx_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    rx_t *rx = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && rx->len + e->data_len < rx->cap) {
        memcpy(rx->buf + rx->len, e->data, e->data_len);
        rx->len += e->data_len;
        rx->buf[rx->len] = 0;
    }
    return ESP_OK;
}

static double num(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

bool weather_fetch(weather_t *w)
{
    rx_t rx = { .cap = 8192 };
    rx.buf = calloc(1, rx.cap);
    if (!rx.buf) return false;

    location_t loc;
    config_get_location(&loc);
    char url[400];
    snprintf(url, sizeof(url), URL_FMT, loc.lat, loc.lon);
    esp_http_client_config_t cfg = {
        .url = url, .event_handler = http_evt, .user_data = &rx,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    bool ok = false;
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "HTTP failed: %s, status %d", esp_err_to_name(err), status);
    } else {
        cJSON *root = cJSON_Parse(rx.buf);
        cJSON *cur = cJSON_GetObjectItem(root, "current");
        cJSON *daily = cJSON_GetObjectItem(root, "daily");
        cJSON *off = cJSON_GetObjectItem(root, "utc_offset_seconds");
        if (cJSON_IsNumber(off)) config_set_utc_offset(off->valueint);
        if (cur && daily) {
            w->temp = num(cur, "temperature_2m");
            w->feels = num(cur, "apparent_temperature");
            w->humidity = (int)num(cur, "relative_humidity_2m");
            w->wind = num(cur, "wind_speed_10m");
            w->code = (int)num(cur, "weather_code");
            w->is_day = (int)num(cur, "is_day");
            cJSON *tmax = cJSON_GetObjectItem(daily, "temperature_2m_max");
            cJSON *tmin = cJSON_GetObjectItem(daily, "temperature_2m_min");
            cJSON *codes = cJSON_GetObjectItem(daily, "weather_code");
            cJSON *days = cJSON_GetObjectItem(daily, "time");
            w->ndays = 0;
            for (int i = 0; i < 4 && i < cJSON_GetArraySize(tmax); i++) {
                w->day[i].tmax = cJSON_GetArrayItem(tmax, i)->valuedouble;
                w->day[i].tmin = cJSON_GetArrayItem(tmin, i)->valuedouble;
                w->day[i].code = cJSON_GetArrayItem(codes, i)->valueint;
                strlcpy(w->day[i].date, cJSON_GetArrayItem(days, i)->valuestring, sizeof(w->day[i].date));
                w->ndays++;
            }
            ok = true;
            ESP_LOGI(TAG, "Now %.1f°C (feels %.1f), %s, RH %d%%, wind %.0f km/h, today %.0f/%.0f",
                     w->temp, w->feels, weather_text(w->code), w->humidity, w->wind,
                     w->day[0].tmax, w->day[0].tmin);
        } else {
            ESP_LOGW(TAG, "Unexpected response: %.120s", rx.buf);
        }
        cJSON_Delete(root);
    }
    free(rx.buf);
    return ok;
}

const char *weather_text(int code)
{
    switch (code) {
    case 0: return "Clear sky";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: case 82: return "Rain showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm, hail";
    default: return "—";
    }
}

wx_kind_t weather_kind(int code)
{
    if (code == 0 || code == 1) return WX_CLEAR;
    if (code == 2) return WX_PARTLY;
    if (code == 3) return WX_CLOUDY;
    if (code == 45 || code == 48) return WX_FOG;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WX_SNOW;
    if (code >= 95) return WX_STORM;
    if (code >= 51) return WX_RAIN;
    return WX_CLOUDY;
}
