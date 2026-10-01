// Fetches current conditions + today's forecast from Open-Meteo (no API key needed)
#include "weather.h"
#include <string.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_log.h"
#include "config.h"
#include "svc.h"
#include "esp_timer.h"

static const char *TAG = "weather";

#define URL_FMT "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f" \
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day,uv_index" \
    "&daily=temperature_2m_max,temperature_2m_min,weather_code,precipitation_probability_max,sunrise,sunset,uv_index_max" \
    "&hourly=temperature_2m,weather_code,precipitation_probability,wind_speed_10m,is_day" \
    "&minutely_15=precipitation,snowfall&forecast_minutely_15=9" \
    "&timezone=auto&forecast_days=7"

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

static double num_at(cJSON *a, int i)
{
    cJSON *v = cJSON_GetArrayItem(a, i);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

// 15-minute forecast: slot i (time T_i) holds the precipitation of (T_i - 15 min, T_i]. Slot 0 is the current
// quarter hour, 8 more cover the next 2 hours. "Starts" = dry now, wet later: the rain begins around the start
// of the first wet slot (= T of the slot before). "Stops" = wet now, dry later.
#define NC_WET_MM 0.1
static void nowcast(cJSON *m15, weather_t *w)
{
    w->nc_kind = NC_NONE;
    w->nc_time[0] = 0;
    w->nc_snow = false;
    cJSON *t = cJSON_GetObjectItem(m15, "time");
    cJSON *p = cJSON_GetObjectItem(m15, "precipitation");
    cJSON *sn = cJSON_GetObjectItem(m15, "snowfall");
    int n = cJSON_GetArraySize(p);
    if (n < 2 || cJSON_GetArraySize(t) < n) return;
    bool wet_now = num_at(p, 0) >= NC_WET_MM || num_at(p, 1) >= NC_WET_MM;
    for (int i = 2; i < n; i++) {
        bool wet = num_at(p, i) >= NC_WET_MM;
        if (wet != wet_now) {
            const char *ts = cJSON_GetStringValue(cJSON_GetArrayItem(t, i - 1));   // "YYYY-MM-DDTHH:MM"
            if (!ts || strlen(ts) < 16) return;
            w->nc_kind = wet_now ? NC_STOPS : NC_STARTS;
            strlcpy(w->nc_time, ts + 11, sizeof(w->nc_time));
            w->nc_snow = num_at(sn, wet_now ? i - 1 : i) > 0;
            return;
        }
    }
}

bool weather_fetch(const location_t *loc, weather_t *w)
{
    rx_t rx = { .cap = 49152 };    // ~10 KB with 7 days of hourly data
    rx.buf = calloc(1, rx.cap);
    if (!rx.buf) return false;

    char url[640];
    snprintf(url, sizeof(url), URL_FMT, loc->lat, loc->lon);
    esp_http_client_config_t cfg = {
        .url = url, .event_handler = http_evt, .user_data = &rx,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000, .buffer_size_tx = 1024,   // long URL
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    svc_http(SVC_FORECAST, err, status, t0);

    bool ok = false;
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "HTTP failed: %s, status %d", esp_err_to_name(err), status);
    } else {
        cJSON *root = cJSON_Parse(rx.buf);
        cJSON *cur = cJSON_GetObjectItem(root, "current");
        cJSON *daily = cJSON_GetObjectItem(root, "daily");
        cJSON *off = cJSON_GetObjectItem(root, "utc_offset_seconds");
        w->utc_offset = cJSON_IsNumber(off) ? off->valueint : 0;
        if (cur && daily) {
            w->temp = num(cur, "temperature_2m");
            w->feels = num(cur, "apparent_temperature");
            w->humidity = (int)num(cur, "relative_humidity_2m");
            w->wind = num(cur, "wind_speed_10m");
            w->code = (int)num(cur, "weather_code");
            w->is_day = (int)num(cur, "is_day");
            w->uv = num(cur, "uv_index");
            cJSON *tmax = cJSON_GetObjectItem(daily, "temperature_2m_max");
            cJSON *tmin = cJSON_GetObjectItem(daily, "temperature_2m_min");
            cJSON *codes = cJSON_GetObjectItem(daily, "weather_code");
            cJSON *days = cJSON_GetObjectItem(daily, "time");
            w->ndays = 0;
            for (int i = 0; i < WX_DAYS && i < cJSON_GetArraySize(tmax); i++) {
                w->day[i].tmax = cJSON_GetArrayItem(tmax, i)->valuedouble;
                w->day[i].tmin = cJSON_GetArrayItem(tmin, i)->valuedouble;
                w->day[i].code = cJSON_GetArrayItem(codes, i)->valueint;
                strlcpy(w->day[i].date, cJSON_GetArrayItem(days, i)->valuestring, sizeof(w->day[i].date));
                w->ndays++;
            }
            cJSON *pop = cJSON_GetObjectItem(daily, "precipitation_probability_max");
            cJSON *sr = cJSON_GetObjectItem(daily, "sunrise"), *ss = cJSON_GetObjectItem(daily, "sunset");
            cJSON *uvm = cJSON_GetObjectItem(daily, "uv_index_max");
            for (int i = 0; i < w->ndays; i++) {
                cJSON *v = cJSON_GetArrayItem(pop, i);
                w->day[i].pop = cJSON_IsNumber(v) ? v->valueint : -1;
                const char *a = cJSON_GetStringValue(cJSON_GetArrayItem(sr, i));   // "YYYY-MM-DDTHH:MM"
                const char *b = cJSON_GetStringValue(cJSON_GetArrayItem(ss, i));
                strlcpy(w->day[i].sunrise, a && strlen(a) >= 16 ? a + 11 : "", sizeof(w->day[i].sunrise));
                strlcpy(w->day[i].sunset, b && strlen(b) >= 16 ? b + 11 : "", sizeof(w->day[i].sunset));
                w->day[i].uv_max = num_at(uvm, i);
            }
            nowcast(cJSON_GetObjectItem(root, "minutely_15"), w);
            if (w->nc_kind != NC_NONE)
                ESP_LOGI(TAG, "Nowcast: %s %s %s", w->nc_snow ? "snow" : "rain",
                         w->nc_kind == NC_STARTS ? "starting around" : "stopping around", w->nc_time);
            cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
            cJSON *ht = cJSON_GetObjectItem(hourly, "temperature_2m");
            cJSON *hc = cJSON_GetObjectItem(hourly, "weather_code");
            cJSON *hp = cJSON_GetObjectItem(hourly, "precipitation_probability");
            cJSON *hw = cJSON_GetObjectItem(hourly, "wind_speed_10m");
            cJSON *hd = cJSON_GetObjectItem(hourly, "is_day");
            w->nhours = 0;
            for (int i = 0; i < WX_HOURS && i < cJSON_GetArraySize(ht); i++) {
                wx_hour_t *h = &w->hour[i];
                h->temp = num_at(ht, i);
                h->code = (unsigned char)num_at(hc, i);
                h->pop = (unsigned char)num_at(hp, i);
                h->wind = num_at(hw, i);
                h->is_day = (unsigned char)num_at(hd, i);
                w->nhours++;
            }
            ok = true;
            ESP_LOGI(TAG, "%s: now %.1f°C (feels %.1f), %s, RH %d%%, wind %.0f km/h, today %.0f/%.0f", loc->name,
                     w->temp, w->feels, weather_text(w->code), w->humidity, w->wind,
                     w->day[0].tmax, w->day[0].tmin);
            ESP_LOGI(TAG, "%d hourly points, %d bytes", w->nhours, rx.len);
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

bool air_fetch(air_t *a)
{
    location_t loc;
    config_get_location(&loc);
    char url[300];
    snprintf(url, sizeof(url), "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.4f&longitude=%.4f"
             "&current=us_aqi,pm2_5,alder_pollen,birch_pollen,grass_pollen,ragweed_pollen&timezone=auto",
             loc.lat, loc.lon);
    rx_t rx = { .cap = 4096 };
    rx.buf = calloc(1, rx.cap);
    if (!rx.buf) return false;
    esp_http_client_config_t cfg = {
        .url = url, .event_handler = http_evt, .user_data = &rx,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000, .buffer_size_tx = 1024,   // long URL
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    svc_http(SVC_AIR, err, status, t0);
    bool ok = false;
    cJSON *root = err == ESP_OK && status == 200 ? cJSON_Parse(rx.buf) : NULL;
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (cur) {
        cJSON *v = cJSON_GetObjectItem(cur, "us_aqi");
        a->us_aqi = cJSON_IsNumber(v) ? v->valueint : -1;
        a->pm25 = num(cur, "pm2_5");
        static const char *names[4] = { "alder_pollen", "birch_pollen", "grass_pollen", "ragweed_pollen" };
        for (int i = 0; i < 4; i++) {
            v = cJSON_GetObjectItem(cur, names[i]);
            a->pollen[i] = cJSON_IsNumber(v) ? v->valuedouble : -1;
        }
        ok = true;
        ESP_LOGI(TAG, "Air: US AQI %d, PM2.5 %.1f, pollen %.0f/%.0f/%.0f/%.0f", a->us_aqi, a->pm25,
                 a->pollen[0], a->pollen[1], a->pollen[2], a->pollen[3]);
    } else {
        if (err == ESP_OK && status == 200) svc_fail(SVC_AIR, "Bad response", t0);
        ESP_LOGW(TAG, "Air quality failed: %s, status %d", esp_err_to_name(err), status);
    }
    cJSON_Delete(root);
    free(rx.buf);
    return ok;
}
