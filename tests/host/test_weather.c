// weather.c against scripted Open-Meteo replies: whole, partial (arrays missing or short), failed
#include <string.h>
#include <math.h>
#include "check.h"
#include "fake.h"
#include "weather.h"

static const location_t here = { "Test", 46.81, -71.21 };
static weather_t w;

#define CUR "\"current\":{\"temperature_2m\":5.5,\"apparent_temperature\":3,\"relative_humidity_2m\":80," \
            "\"weather_code\":3,\"wind_speed_10m\":12,\"is_day\":1,\"uv_index\":1.5}"
#define HOURLY "\"hourly\":{\"temperature_2m\":[1,2,3],\"weather_code\":[0,1,2],\"precipitation_probability\":[0,10,20]," \
               "\"wind_speed_10m\":[5,6,7],\"is_day\":[0,0,1]}"

static bool fetch(const char *body)
{
    memset(&w, 0, sizeof(w));
    fake_http = (fake_http_t){ .body = body, .status = 200 };
    return weather_fetch(&here, &w);
}

int main(void)
{
    // A whole reply
    CHECK(fetch("{\"utc_offset_seconds\":-14400," CUR ",\"daily\":{\"time\":[\"2026-10-03\",\"2026-10-04\"],"
                "\"temperature_2m_max\":[10,12],\"temperature_2m_min\":[2,4],\"weather_code\":[3,61],"
                "\"precipitation_probability_max\":[10,80],\"sunrise\":[\"2026-10-03T06:55\",\"2026-10-04T06:56\"],"
                "\"sunset\":[\"2026-10-03T18:30\",\"2026-10-04T18:28\"],\"uv_index_max\":[3,2]}," HOURLY "}"), "whole reply");
    CHECK(w.ndays == 2 && w.day[1].tmax == 12 && w.day[1].code == 61 && !strcmp(w.day[1].date, "2026-10-04"),
          "ndays %d", w.ndays);
    CHECK(w.nhours == 3 && w.hour[2].pop == 20 && w.utc_offset == -14400, "nhours %d", w.nhours);
    CHECK(!strcmp(w.day[0].sunrise, "06:55"), "sunrise %s", w.day[0].sunrise);

    // daily.time missing: no crash (it dereferenced NULL), no forecast
    CHECK(!fetch("{" CUR ",\"daily\":{\"temperature_2m_max\":[10,12],\"temperature_2m_min\":[2,4],"
                 "\"weather_code\":[3,61]}," HOURLY "}"), "no daily.time");
    CHECK(w.ndays == 0, "ndays %d", w.ndays);

    // One daily array shorter than the others: only the days every array has
    CHECK(fetch("{" CUR ",\"daily\":{\"time\":[\"2026-10-03\",\"2026-10-04\",\"2026-10-05\"],"
                "\"temperature_2m_max\":[10,12,14],\"temperature_2m_min\":[2],\"weather_code\":[3,61,0]}}"), "short tmin");
    CHECK(w.ndays == 1, "ndays %d", w.ndays);

    // weather_code missing, values null or of the wrong type: no crash
    CHECK(!fetch("{" CUR ",\"daily\":{\"time\":[\"2026-10-03\"],\"temperature_2m_max\":[10],\"temperature_2m_min\":[2]}}"),
          "no daily.weather_code");
    CHECK(fetch("{" CUR ",\"daily\":{\"time\":[\"2026-10-03\"],\"temperature_2m_max\":[null],\"temperature_2m_min\":[\"x\"],"
                "\"weather_code\":[null]}}"), "null values");
    CHECK(fetch("{" CUR ",\"daily\":{\"time\":[\"2026-10-03\", 5],\"temperature_2m_max\":[1,2],\"temperature_2m_min\":[1,2],"
                "\"weather_code\":[1,2]}}") && w.ndays == 1, "a date that isn't a string ends the days");

    // No "daily", or not JSON at all
    CHECK(!fetch("{" CUR "}"), "no daily");
    CHECK(!fetch("<html>busy</html>"), "not JSON");

    // HTTP failure, transport error, no memory for a client (init returned NULL: perform() crashed on it)
    fake_http = (fake_http_t){ .body = "{}", .status = 500 };
    CHECK(!weather_fetch(&here, &w), "HTTP 500");
    fake_http = (fake_http_t){ .err = ESP_ERR_TIMEOUT };
    CHECK(!weather_fetch(&here, &w), "timeout");
    fake_http = (fake_http_t){ .no_client = true, .status = 200, .body = "{}" };
    CHECK(!weather_fetch(&here, &w) && fake_http.requests == 0, "no client");
    air_t air;
    CHECK(!air_fetch(&air), "air: no client");

    // Null hourly values are "no value", not 0
    CHECK(fetch("{" CUR ",\"daily\":{\"time\":[\"2026-10-03\"],\"temperature_2m_max\":[1],\"temperature_2m_min\":[0],"
                "\"weather_code\":[1]},\"hourly\":{\"temperature_2m\":[1,null],\"weather_code\":[0,1],"
                "\"precipitation_probability\":[null,40],\"wind_speed_10m\":[5,6],\"is_day\":[0,0]}}"), "nulls");
    CHECK(w.nhours == 2 && isnan(w.hour[1].temp) && w.hour[0].pop == WX_POP_NONE && w.hour[1].pop == 40,
          "temp %f pop %d", w.hour[1].temp, w.hour[0].pop);

    // After midnight: the forecast still starts yesterday until the next fetch
    memset(&w, 0, sizeof(w));
    w.ndays = 3;
    w.nhours = 72;
    strcpy(w.day[0].date, "2026-10-02"); strcpy(w.day[1].date, "2026-10-03"); strcpy(w.day[2].date, "2026-10-04");
    for (int i = 0; i < 72; i++) w.hour[i].temp = i;
    CHECK(weather_from_today(&w, "2026-10-03") == 1, "one day dropped");
    CHECK(w.ndays == 2 && !strcmp(w.day[0].date, "2026-10-03") && w.nhours == 48 && w.hour[0].temp == 24,
          "ndays %d day0 %s nhours %d hour0 %.0f", w.ndays, w.day[0].date, w.nhours, w.hour[0].temp);
    CHECK(weather_from_today(&w, "2026-10-03") == 0 && w.ndays == 2, "already today: unchanged");
    CHECK(weather_from_today(&w, "2026-10-09") == 0 && w.ndays == 2, "today not in the forecast: unchanged");
    CHECK(weather_from_today(&w, "2026-10-01") == 0, "clock behind the forecast: unchanged");
    return check_done("weather");
}

void config_get_location(location_t *out) { *out = here; }
