// Settings web UI: HTTPS on 443 (needed for phone GPS), plain HTTP on 80 redirects to it.
#include "web.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_https_server.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "tlscert.h"
#include "ota.h"
#include "config.h"
#include "net.h"
#include "presence.h"
#include "display.h"
#include "ui.h"
#include "i18n.h"
#include "sound.h"
#include "lwip/sockets.h"

static const char *TAG = "web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static web_location_cb_t loc_cb;

static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_sendstr(req, s);
    free(s);
    cJSON_Delete(j);
    return r;
}

static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1024) return NULL;
    char *buf = calloc(1, req->content_len + 1);
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static esp_err_t index_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET / (page), free internal %u, largest DMA block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const char *p = (const char *)index_html_start;
    size_t left = index_html_end - index_html_start - 1;
    while (left) {                                  // small TLS records are more reliable on the ESP32
        size_t n = left > 1024 ? 1024 : left;
        if (httpd_resp_send_chunk(req, p, n) != ESP_OK) { ESP_LOGW(TAG, "page send failed"); return ESP_FAIL; }
        p += n; left -= n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t config_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /api/config");
    location_t loc;
    config_get_location(&loc);
    char ssid[33] = "";
    if (!net_in_portal()) net_get_ssid(ssid, sizeof(ssid));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", loc.name);
    cJSON_AddNumberToObject(j, "lat", loc.lat);
    cJSON_AddNumberToObject(j, "lon", loc.lon);
    cJSON_AddStringToObject(j, "ssid", ssid);
    cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
    cJSON *pl = cJSON_AddArrayToObject(j, "places");
    for (int i = 0; i < config_place_count(); i++) {
        location_t p;
        config_get_place(i, &p);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", p.name);
        cJSON_AddNumberToObject(o, "lat", p.lat);
        cJSON_AddNumberToObject(o, "lon", p.lon);
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
    return send_json(req, j);
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
    return send_json(req, j);
}

static double num_or(cJSON *j, const char *k, double def);   // below

static int hhmm_or(cJSON *j, const char *k, int def)
{
    cJSON *v = cJSON_GetObjectItem(j, k);
    int h, m;
    return cJSON_IsString(v) && sscanf(v->valuestring, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60
           ? h * 60 + m : def;
}

static esp_err_t sound_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
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
        sound_set_config(&c);
    }
    cJSON_Delete(j);
    return sound_get(req);
}

// {"select": i} shows place i on the display; {"delete": i} removes it (not the last one)
static esp_err_t places_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    cJSON *sel = j ? cJSON_GetObjectItem(j, "select") : NULL, *del = j ? cJSON_GetObjectItem(j, "delete") : NULL;
    bool ok = cJSON_IsNumber(sel) ? config_select_place(sel->valueint) :
              cJSON_IsNumber(del) ? config_delete_place(del->valueint) : false;
    cJSON_Delete(j);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad place");
    if (loc_cb) loc_cb();
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// {"temp":"c"|"f", "wind":"kmh"|"mph"|"ms", "clock":24|12, "lang":"en"|"fr"}; any subset. The screens redraw at once.
static esp_err_t units_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
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

static int by_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

// [{"ssid":"...","rssi":-52,"secure":true}, ...] strongest first, one entry per name
static esp_err_t scan_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /api/scan");
    cJSON *arr = cJSON_CreateArray();
    uint16_t n = 30;
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    wifi_scan_config_t sc = { .show_hidden = false };
    if (recs && esp_wifi_scan_start(&sc, true) == ESP_OK && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        qsort(recs, n, sizeof(recs[0]), by_rssi);
        for (int i = 0; i < n; i++) {
            const char *ssid = (const char *)recs[i].ssid;
            if (!ssid[0] || !strcmp(ssid, SETUP_AP_SSID)) continue;
            bool dup = false;
            for (int k = 0; k < i && !dup; k++) dup = !strcmp((const char *)recs[k].ssid, ssid);
            if (dup) continue;                               // already listed with a stronger signal
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ssid", ssid);
            cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
            cJSON_AddBoolToObject(o, "secure", recs[i].authmode != WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(arr, o);
        }
        ESP_LOGI(TAG, "scan: %d networks", cJSON_GetArraySize(arr));
    } else {
        ESP_LOGW(TAG, "scan failed");
    }
    free(recs);
    return send_json(req, arr);
}

static esp_err_t location_post(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /api/location (%d bytes)", req->content_len);
    cJSON *j = read_json(req);
    cJSON *name = j ? cJSON_GetObjectItem(j, "name") : NULL;
    cJSON *lat = j ? cJSON_GetObjectItem(j, "lat") : NULL;
    cJSON *lon = j ? cJSON_GetObjectItem(j, "lon") : NULL;
    cJSON *idx = j ? cJSON_GetObjectItem(j, "index") : NULL;   // which place (count = add one); default: the one shown
    location_t loc = {0};
    bool ok = cJSON_IsNumber(lat) && cJSON_IsNumber(lon);
    if (ok) {
        strlcpy(loc.name, cJSON_IsString(name) && name->valuestring[0] ? name->valuestring : "My location", sizeof(loc.name));
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

static esp_err_t presence_get(httpd_req_t *req)
{
    presence_cfg_t c;
    presence_status_t st;
    presence_get_config(&c);
    presence_get_status(&st);
    static const char *names[] = {"active", "dim", "off"};
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enabled", c.enabled);
    cJSON_AddNumberToObject(j, "margin_db", c.margin_db);
    cJSON_AddNumberToObject(j, "wake_s", c.wake_s);
    cJSON_AddNumberToObject(j, "dim_s", c.dim_s);
    cJSON_AddNumberToObject(j, "off_s", c.off_s);
    cJSON_AddNumberToObject(j, "bright_pct", c.bright_pct);
    cJSON_AddNumberToObject(j, "dim_pct", c.dim_pct);
    cJSON_AddNumberToObject(j, "baseline_db", c.baseline_db);
    cJSON_AddNumberToObject(j, "level_db", st.level_db);
    cJSON_AddNumberToObject(j, "threshold_db", st.threshold_db);
    cJSON_AddStringToObject(j, "state", names[st.state]);
    cJSON_AddNumberToObject(j, "wake_progress", st.wake_progress);
    cJSON_AddNumberToObject(j, "quiet_s", st.quiet_s);
    cJSON_AddBoolToObject(j, "calibrating", st.calibrating);
    cJSON_AddNumberToObject(j, "calib_left_s", st.calib_left_s);
    cJSON_AddBoolToObject(j, "mic_ok", st.mic_ok);
    cJSON_AddNumberToObject(j, "brightness", st.brightness);
    cJSON_AddBoolToObject(j, "imu_ok", st.imu_ok);
    cJSON_AddNumberToObject(j, "motion_g", st.motion_g);
    cJSON_AddBoolToObject(j, "motion_wake", presence_motion_wake());
    cJSON_AddNumberToObject(j, "motion_thr", st.motion_thr);
    return send_json(req, j);
}

static double num_or(cJSON *j, const char *k, double def)
{
    cJSON *v = cJSON_GetObjectItem(j, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static esp_err_t presence_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    presence_cfg_t c;
    presence_get_config(&c);
    cJSON *en = cJSON_GetObjectItem(j, "enabled");
    if (cJSON_IsBool(en)) c.enabled = cJSON_IsTrue(en);
    c.margin_db = num_or(j, "margin_db", c.margin_db);
    c.wake_s = num_or(j, "wake_s", c.wake_s);
    c.dim_s = num_or(j, "dim_s", c.dim_s);
    c.off_s = num_or(j, "off_s", c.off_s);
    c.bright_pct = (int)num_or(j, "bright_pct", c.bright_pct);
    c.dim_pct = (int)num_or(j, "dim_pct", c.dim_pct);
    cJSON *mw = cJSON_GetObjectItem(j, "motion_wake");
    presence_status_t ps;
    presence_get_status(&ps);
    if (cJSON_IsBool(mw) || cJSON_IsNumber(cJSON_GetObjectItem(j, "motion_thr")))
        presence_set_motion(cJSON_IsBool(mw) ? cJSON_IsTrue(mw) : presence_motion_wake(), num_or(j, "motion_thr", ps.motion_thr));
    cJSON_Delete(j);
    presence_set_config(&c);
    return presence_get(req);
}

static const char *ota_state_name(ota_state_t s)
{
    static const char *n[] = { "idle", "checking", "up_to_date", "available", "downloading", "done", "failed" };
    return s <= OTA_FAILED ? n[s] : "?";
}

// {"current","latest","channel","state","progress","error","notes"}; notes (see ota_get_notes) only when an
// update is offered
static esp_err_t update_get(httpd_req_t *req)
{
    ota_status_t o;
    ota_get_status(&o);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "current", o.current);
    cJSON_AddStringToObject(j, "latest", o.latest);
    cJSON_AddStringToObject(j, "channel", o.channel);
    cJSON_AddStringToObject(j, "state", ota_state_name(o.state));
    cJSON_AddNumberToObject(j, "progress", o.progress);
    cJSON_AddStringToObject(j, "error", o.error);
    // A fresh update is "pending verify" until ota.c confirms it (60 s); a restart before that rolls it back.
    // tools/harness waits for this to turn false before restarting the board.
    esp_ota_img_states_t st;
    cJSON_AddBoolToObject(j, "pending_verify", esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) ==
                                               ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY);
    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    if (o.state == OTA_AVAILABLE) {
        char *notes = heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
        if (notes) {
            ota_get_notes(notes, 3072);
            cJSON_AddStringToObject(j, "notes", notes);
            free(notes);
        }
    }
    return send_json(req, j);
}

// GET /api/snapshot?screen=status: the screen rendered off-display, as a 24-bit BMP (tools/snapshot.py)
static esp_err_t snapshot_get(httpd_req_t *req)
{
    char q[48], name[16] = "current";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "screen", name, sizeof(name));
    display_lock(-1);
    lv_draw_buf_t *db = ui_snapshot(name);
    display_unlock();
    uint8_t *buf = heap_caps_malloc(16 * 1400, MALLOC_CAP_SPIRAM);
    if (!db || !buf) {
        free(buf);
        if (db) { display_lock(-1); lv_draw_buf_destroy(db); display_unlock(); }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot failed");
    }
    int w = db->header.w, h = db->header.h, row = (w * 3 + 3) & ~3;
    uint32_t size = 54 + row * h;
    uint8_t hd[54] = { 'B', 'M' };
    #define LE32(o, v) do { uint32_t _v = (v); hd[o] = _v; hd[o + 1] = _v >> 8; hd[o + 2] = _v >> 16; hd[o + 3] = _v >> 24; } while (0)
    LE32(2, size); LE32(10, 54); LE32(14, 40); LE32(18, w); LE32(22, (uint32_t)-h);   // negative height: top-down
    hd[26] = 1; hd[28] = 24; LE32(34, row * h);
    #undef LE32
    httpd_resp_set_type(req, "image/bmp");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)hd, sizeof(hd));
    for (int y0 = 0; y0 < h && err == ESP_OK; y0 += 16) {
        int n = h - y0 < 16 ? h - y0 : 16;
        memset(buf, 0, n * row);
        for (int y = 0; y < n; y++) {
            const uint16_t *src = (const uint16_t *)(db->data + (y0 + y) * db->header.stride);
            uint8_t *d = buf + y * row;
            for (int x = 0; x < w; x++, d += 3) {
                uint16_t p = src[x];
                d[0] = (p & 0x1F) << 3; d[1] = (p >> 5 & 0x3F) << 2; d[2] = (p >> 11) << 3;   // B, G, R
            }
        }
        err = httpd_resp_send_chunk(req, (const char *)buf, n * row);
    }
    free(buf);
    display_lock(-1);
    lv_draw_buf_destroy(db);
    display_unlock();
    ESP_LOGI(TAG, "snapshot %s %dx%d %s", name, w, h, err == ESP_OK ? "sent" : "failed");
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : ESP_FAIL;
}

// {"channel":"stable"|"beta"} and/or {"action":"check"|"install"}
static esp_err_t update_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    const char *ch = cJSON_GetStringValue(cJSON_GetObjectItem(j, "channel"));
    const char *act = cJSON_GetStringValue(cJSON_GetObjectItem(j, "action"));
    if (ch) ota_set_channel(ch);
    if (act && !strcmp(act, "check")) ota_check_now();
    if (act && !strcmp(act, "install")) ota_install();
    cJSON_Delete(j);
    vTaskDelay(pdMS_TO_TICKS(200));                        // let the OTA task pick it up
    return update_get(req);
}

static esp_err_t calibrate_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    int secs = j ? (int)num_or(j, "seconds", 5) : 5;
    cJSON_Delete(j);
    if (!presence_calibrate(secs)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "busy or no mic");
    return presence_get(req);
}

static void restart_task(void *a) { vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); }

static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    cJSON *ssid = j ? cJSON_GetObjectItem(j, "ssid") : NULL;
    cJSON *pass = j ? cJSON_GetObjectItem(j, "pass") : NULL;
    bool ok = cJSON_IsString(ssid) && strlen(ssid->valuestring) < 33 &&
              net_save_creds(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(j);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad wifi");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    xTaskCreate(restart_task, "rst", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// True when the request came in on the setup access point (192.168.4.x) rather than the home network
static bool from_setup_ap(httpd_req_t *req)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    int fd = httpd_req_to_sockfd(req);
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) return false;
    if (ss.ss_family == AF_INET) {
        uint32_t a = ntohl(((struct sockaddr_in *)&ss)->sin_addr.s_addr);
        return (a & 0xFFFFFF00) == 0xC0A80400;          // 192.168.4.0/24
    }
    if (ss.ss_family == AF_INET6) {                      // IPv4-mapped (::ffff:192.168.4.x)
        const uint8_t *b = ((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr;
        return b[10] == 0xFF && b[11] == 0xFF && b[12] == 192 && b[13] == 168 && b[14] == 4;
    }
    return false;
}

static esp_err_t redirect_to(httpd_req_t *req, const char *loc)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_type(req, "text/html");
    // iOS needs a body to recognise the captive portal
    return httpd_resp_sendstr(req, "<html><body><a href=\"/\">Weather display setup</a></body></html>");
}

// Plain HTTP ":80"
//  - on the setup AP: this *is* the captive portal. Serve the page over HTTP (the phone's sign-in browser
//    rejects our self-signed certificate) and send every other URL (OS connectivity checks) to it.
//  - on the home network: move to HTTPS so the page can use the phone's GPS.
static esp_err_t http_root_get(httpd_req_t *req)
{
    if (from_setup_ap(req)) return index_get(req);
    char host[64] = "", loc[96];
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    snprintf(loc, sizeof(loc), "https://%s/", host);
    return redirect_to(req, loc);
}

// Test console only ("portal windows-quiet"), until restart: answer Windows' connectivity check as if online, so
// the test PC joining the setup network doesn't pop a browser tab (Windows then opens msftconnecttest.com/redirect,
// which lands on msn.com because the PC is online by Ethernet). Phones and other PCs still get the portal.
static volatile bool windows_quiet;
void web_test_windows_quiet(bool on) { windows_quiet = on; }

static esp_err_t http_other_get(httpd_req_t *req)
{
    if (from_setup_ap(req)) {
        if (windows_quiet && !strcmp(req->uri, "/connecttest.txt")) {
            ESP_LOGI(TAG, "captive: Windows check answered (test console)");
            httpd_resp_set_type(req, "text/plain");
            return httpd_resp_sendstr(req, "Microsoft Connect Test");
        }
        ESP_LOGI(TAG, "captive: %.60s -> portal", req->uri);
        return redirect_to(req, "http://192.168.4.1/");
    }
    return http_root_get(req);
}

static void start_https(void)
{
    const char *cert, *key;
    size_t cert_len, key_len;
    if (!tlscert_get(&cert, &cert_len, &key, &key_len)) { ESP_LOGE(TAG, "no TLS certificate, HTTPS disabled"); return; }
    tlscert_log_fingerprint();
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.servercert = (const uint8_t *)cert;
    conf.servercert_len = cert_len;
    conf.prvtkey_pem = (const uint8_t *)key;
    conf.prvtkey_len = key_len;
    conf.httpd.max_uri_handlers = 18;
    conf.httpd.stack_size = 7168;      // measured peak ~3.3 KB (TLS handshake)
    conf.httpd.max_open_sockets = 5;
    conf.httpd.lru_purge_enable = true;
    httpd_handle_t s = NULL;
    if (httpd_ssl_start(&s, &conf) != ESP_OK) { ESP_LOGE(TAG, "HTTPS server failed to start"); return; }
    httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = index_get },
        { .uri = "/api/config",   .method = HTTP_GET,  .handler = config_get },
        { .uri = "/api/scan",     .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/location", .method = HTTP_POST, .handler = location_post },
        { .uri = "/api/units",    .method = HTTP_POST, .handler = units_post },
        { .uri = "/api/places",   .method = HTTP_POST, .handler = places_post },
        { .uri = "/api/wifi",     .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/presence", .method = HTTP_GET,  .handler = presence_get },
        { .uri = "/api/presence", .method = HTTP_POST, .handler = presence_post },
        { .uri = "/api/calibrate", .method = HTTP_POST, .handler = calibrate_post },
        { .uri = "/api/update",   .method = HTTP_GET,  .handler = update_get },
        { .uri = "/api/update",   .method = HTTP_POST, .handler = update_post },
        { .uri = "/api/snapshot", .method = HTTP_GET,  .handler = snapshot_get },
        { .uri = "/api/sound",    .method = HTTP_GET,  .handler = sound_get },
        { .uri = "/api/sound",    .method = HTTP_POST, .handler = sound_post },
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s, &uris[i]);
}

void web_start(web_location_cb_t on_location_changed)
{
    static bool started;
    loc_cb = on_location_changed;
    if (started) return;
    started = true;

    start_https();

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.max_uri_handlers = 14;
    hc.max_open_sockets = 6;
    hc.lru_purge_enable = true;
    hc.stack_size = 4096;              // measured peak ~1.2 KB
    httpd_handle_t h = NULL;
    if (httpd_start(&h, &hc) == ESP_OK) {
        httpd_uri_t huris[] = {        // specific routes first; the wildcard catches everything else
            { .uri = "/",             .method = HTTP_GET,  .handler = http_root_get },
            { .uri = "/api/config",   .method = HTTP_GET,  .handler = config_get },
            { .uri = "/api/scan",     .method = HTTP_GET,  .handler = scan_get },
            { .uri = "/api/location", .method = HTTP_POST, .handler = location_post },
            { .uri = "/api/units",    .method = HTTP_POST, .handler = units_post },
            { .uri = "/api/places",   .method = HTTP_POST, .handler = places_post },
            { .uri = "/api/wifi",     .method = HTTP_POST, .handler = wifi_post },
            { .uri = "/api/presence", .method = HTTP_GET,  .handler = presence_get },
            { .uri = "/api/presence", .method = HTTP_POST, .handler = presence_post },
            { .uri = "/api/calibrate", .method = HTTP_POST, .handler = calibrate_post },
            { .uri = "/api/update",   .method = HTTP_GET,  .handler = update_get },
            { .uri = "/api/update",   .method = HTTP_POST, .handler = update_post },
            { .uri = "/*",            .method = HTTP_GET,  .handler = http_other_get },
        };
        for (int i = 0; i < sizeof(huris) / sizeof(huris[0]); i++) httpd_register_uri_handler(h, &huris[i]);
    }
    char ip[20];
    if (net_get_ip(ip, sizeof(ip))) ESP_LOGI(TAG, "Settings page: https://%s/", ip);
    else ESP_LOGI(TAG, "Web servers started (no IP yet)");
}
