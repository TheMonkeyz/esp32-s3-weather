// The settings page's app routes (espforge's forge_net web.c serves the page, the Wi-Fi setup routes, updates and
// the snapshot, and guards every route: see "Who may change things" there). Moved from web.c, October 5 (v1.14.0).
// Screen dimming's /api/presence and /api/calibrate are forge_presence's (presence_web_routes(), main.c).
#include "routes.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "web.h"
#include "ota.h"
#include "config.h"
#include "net.h"
#include "display.h"
#include "ui.h"
#include "i18n.h"
#include "sound.h"
#include "utf8.h"

static const char *TAG = "web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static void (*loc_cb)(void);

static esp_err_t config_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /api/config");
    location_t loc;
    config_get_location(&loc);
    // On the setup network (anyone with its password, during an outage too) no coordinates and no saved network
    bool ap = web_from_setup_ap(req);
    char ssid[33] = "";
    if (!net_in_portal() && !ap) net_get_ssid(ssid, sizeof(ssid));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", loc.name);
    if (!ap) cJSON_AddNumberToObject(j, "lat", loc.lat);
    if (!ap) cJSON_AddNumberToObject(j, "lon", loc.lon);
    cJSON_AddStringToObject(j, "ssid", ssid);
    if (ap) cJSON_AddTrueToObject(j, "setup");      // the page explains why its map and search don't load
    cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
    cJSON *pl = cJSON_AddArrayToObject(j, "places");
    for (int i = 0; i < config_place_count(); i++) {
        location_t p;
        config_get_place(i, &p);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", p.name);
        if (!ap) cJSON_AddNumberToObject(o, "lat", p.lat);
        if (!ap) cJSON_AddNumberToObject(o, "lon", p.lon);
        cJSON_AddItemToArray(pl, o);
    }
    cJSON_AddNumberToObject(j, "active", config_active_place());
    cJSON_AddNumberToObject(j, "max_places", MAX_PLACES);
    units_t u;
    config_get_units(&u);
    cJSON *un = cJSON_AddObjectToObject(j, "units");
    cJSON_AddStringToObject(un, "temp", u.fahrenheit ? "f" : "c");
    cJSON_AddStringToObject(un, "wind", u.wind == WIND_MPH ? "mph" : u.wind == WIND_MS ? "ms" : "kmh");
    cJSON_AddNumberToObject(un, "clock", u.h12 ? 12 : 24);
    cJSON_AddStringToObject(un, "lang", i18n_code(u.lang));
    cJSON *langs = cJSON_AddArrayToObject(j, "languages");      // [{code, name}] for the selector
    for (int i = 0; i < LANG_COUNT; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "code", i18n_code(i));
        cJSON_AddStringToObject(o, "name", i18n_name(i));
        cJSON_AddItemToArray(langs, o);
    }
    return web_send_json(req, j);
}

static double num_or(cJSON *j, const char *k, double def)
{
    cJSON *v = cJSON_GetObjectItem(j, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

// Alert chime: {"level":0..3 (off, red, orange+red, all), "volume":0..100, "quiet_from":"22:00", "quiet_to":"07:00",
// "ok":speaker present}. POST any subset, or {"test":true|1|2|3} to hear it.
static esp_err_t sound_get(httpd_req_t *req)
{
    sound_cfg_t c;
    sound_get_config(&c);
    char a[16], b[16];
    snprintf(a, sizeof(a), "%02d:%02d", c.quiet_from / 60, c.quiet_from % 60);
    snprintf(b, sizeof(b), "%02d:%02d", c.quiet_to / 60, c.quiet_to % 60);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "level", c.level);
    cJSON_AddNumberToObject(j, "volume", c.volume);
    cJSON_AddStringToObject(j, "quiet_from", a);
    cJSON_AddStringToObject(j, "quiet_to", b);
    cJSON_AddBoolToObject(j, "ok", sound_ok());
    return web_send_json(req, j);
}

static int hhmm_or(cJSON *j, const char *k, int def)
{
    cJSON *v = cJSON_GetObjectItem(j, k);
    int h, m;
    return cJSON_IsString(v) && sscanf(v->valuestring, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60
           ? h * 60 + m : def;
}

static esp_err_t sound_post(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    cJSON *tst = cJSON_GetObjectItem(j, "test");                // true (orange) or 1..3 (yellow, orange, red)
    if (cJSON_IsTrue(tst) || cJSON_IsNumber(tst)) sound_test(cJSON_IsNumber(tst) ? tst->valueint : 2);
    else {
        sound_cfg_t c;
        sound_get_config(&c);
        c.level = (int)num_or(j, "level", c.level);
        c.volume = (int)num_or(j, "volume", c.volume);
        c.quiet_from = hhmm_or(j, "quiet_from", c.quiet_from);
        c.quiet_to = hhmm_or(j, "quiet_to", c.quiet_to);
        bool saved = sound_set_config(&c);
        ui_settings_changed();
        if (!saved) { cJSON_Delete(j); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not saved"); }
    }
    cJSON_Delete(j);
    return sound_get(req);
}

// {"select": i} shows place i on the display; {"delete": i} removes it (not the last one)
static esp_err_t places_post(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    cJSON *sel = j ? cJSON_GetObjectItem(j, "select") : NULL, *del = j ? cJSON_GetObjectItem(j, "delete") : NULL;
    bool ok = cJSON_IsNumber(sel) ? config_select_place(sel->valueint) :
              cJSON_IsNumber(del) ? config_delete_place(del->valueint) : false;
    cJSON_Delete(j);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad place");
    if (loc_cb) loc_cb();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// {"temp":"c"|"f", "wind":"kmh"|"mph"|"ms", "clock":24|12, "lang":"en"|"fr"|"iu"}; any subset. The screens redraw.
static esp_err_t units_post(httpd_req_t *req)
{
    cJSON *j = web_read_json(req);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    units_t u;
    config_get_units(&u);
    cJSON *v;
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "temp"))) u.fahrenheit = !strcmp(v->valuestring, "f");
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "wind")))
        u.wind = !strcmp(v->valuestring, "mph") ? WIND_MPH : !strcmp(v->valuestring, "ms") ? WIND_MS : WIND_KMH;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "clock"))) u.h12 = v->valueint == 12;
    int old_lang = u.lang;
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "lang"))) u.lang = i18n_from_code(v->valuestring);
    cJSON_Delete(j);
    if (!config_set_units(&u)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not saved");
    ui_units_changed();
    if (u.lang != old_lang) { ota_check_now(); if (loc_cb) loc_cb(); }   // notes and alerts in the new language
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t location_post(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /api/location (%d bytes)", req->content_len);
    cJSON *j = web_read_json(req);
    cJSON *name = j ? cJSON_GetObjectItem(j, "name") : NULL;
    cJSON *lat = j ? cJSON_GetObjectItem(j, "lat") : NULL;
    cJSON *lon = j ? cJSON_GetObjectItem(j, "lon") : NULL;
    cJSON *idx = j ? cJSON_GetObjectItem(j, "index") : NULL;   // which place (count = add one); default: the one shown
    location_t loc = {0};
    bool ok = cJSON_IsNumber(lat) && cJSON_IsNumber(lon);
    if (ok) {
        utf8_copy(loc.name, cJSON_IsString(name) && name->valuestring[0] ? name->valuestring : "My location", sizeof(loc.name));
        loc.lat = lat->valuedouble;
        loc.lon = lon->valuedouble;
        ok = config_set_place(cJSON_IsNumber(idx) ? idx->valueint : config_active_place(), &loc);
    }
    cJSON_Delete(j);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad location");
    if (loc_cb) loc_cb();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const web_route_t routes[] = {
    { "/api/config",    HTTP_GET,  config_get },
    { "/api/sound",     HTTP_GET,  sound_get },
    { "/api/location",  HTTP_POST, location_post, .keyed = true },
    { "/api/units",     HTTP_POST, units_post, .keyed = true },
    { "/api/places",    HTTP_POST, places_post, .keyed = true },
    { "/api/sound",     HTTP_POST, sound_post, .keyed = true },
};

// GET /api/snapshot (forge_net): a screen rendered off-display under the display lock (ui_snapshot); the buffer is
// destroyed under it too
static bool snap_take(const char *screen, web_image_t *out)
{
    display_lock(-1);
    lv_draw_buf_t *db = ui_snapshot(screen);
    display_unlock();
    if (!db) return false;
    *out = (web_image_t){ db->data, db->header.w, db->header.h, db->header.stride, db };
    return true;
}

static void snap_free(web_image_t *img)
{
    if (!img->priv) return;
    display_lock(-1);
    lv_draw_buf_destroy(img->priv);
    display_unlock();
    img->priv = NULL;
}

void routes_init(void (*on_location_changed)(void))
{
    loc_cb = on_location_changed;
    web_set_page(index_html_start, index_html_end);
    web_add_routes(routes, sizeof(routes) / sizeof(routes[0]));
    web_set_snapshot(snap_take, snap_free);
}
