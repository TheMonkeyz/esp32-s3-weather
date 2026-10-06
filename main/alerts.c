// Environment Canada weather alerts from the MSC GeoMet OGC API (collection "weather-alerts").
// A tiny bounding box around the location selects the alert regions that contain it (the server intersects
// it with the real region shapes); skipGeometry keeps the response to a few KB.
#include "alerts.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_http_client.h"
#include "http_once.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "services.h"
#include "i18n.h"
#include "utf8.h"

static const char *TAG = "alerts";

// A reply in PSRAM, grown as it arrives up to `max` (a reply that doesn't fit stops growing: the parsers then fail on
// it). Most replies are a few KB: 64 KB (the list) and 160 KB (a region's shape) taken up front were mostly unused.
typedef struct { char *buf; int len; int cap; int max; } rx_t;

static bool rx_init(rx_t *rx, int max)
{
    *rx = (rx_t){ .cap = 16 * 1024, .max = max };
    rx->buf = heap_caps_malloc(rx->cap, MALLOC_CAP_SPIRAM);
    if (rx->buf) rx->buf[0] = 0;
    return rx->buf;
}

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    rx_t *rx = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (rx->len + e->data_len >= rx->cap && rx->cap < rx->max) {
        int cap = rx->cap;
        while (cap <= rx->len + e->data_len && cap < rx->max) cap *= 2;
        if (cap > rx->max) cap = rx->max;
        char *b = heap_caps_realloc(rx->buf, cap, MALLOC_CAP_SPIRAM);
        if (b) { rx->buf = b; rx->cap = cap; }
    }
    if (rx->len + e->data_len < rx->cap) {
        memcpy(rx->buf + rx->len, e->data, e->data_len);
        rx->len += e->data_len;
        rx->buf[rx->len] = 0;
    } else rx->len = rx->cap;                                   // didn't fit: marked full (and the reply refused)
    return ESP_OK;
}

// "2026-10-01T16:55:00.000Z" -> UTC epoch
static time_t parse_utc(const char *s)
{
    int y, mo, d, h, mi, se;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    y -= mo <= 2;                                               // days from civil (H. Hinnant)
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + doe - 719468;
    return (time_t)(days * 86400L + h * 3600 + mi * 60 + se);
}

static const char *str(cJSON *o, const char *k)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
    return v ? v : "";
}

static int severity(char c) { return c == 'r' ? 3 : c == 'o' ? 2 : c == 'y' ? 1 : 0; }

static int by_severity(const void *a, const void *b)
{
    return severity(((const alert_t *)b)->colour) - severity(((const alert_t *)a)->colour);
}

// The alerts in force among the features, at most ALERTS_MAX, most severe first. The cap keeps the most severe ones:
// capped in the server's order and sorted afterwards, a red warning listed fifth was dropped (no pill, no sound).
static void parse_features(cJSON *features, time_t now, alerts_t *out)
{
    out->n = 0;
    cJSON *f;
    cJSON_ArrayForEach(f, features) {
        cJSON *p = cJSON_GetObjectItem(f, "properties");
        const char *st = str(p, "status_en");
        if (!strcmp(st, "ended") || !strcmp(st, "cancelled")) continue;
        time_t ends = parse_utc(str(p, "event_end_datetime"));
        time_t expires = parse_utc(str(p, "expiration_datetime"));
        if (now > 1700000000 && ((ends && ends < now) || (expires && expires < now))) continue;
        const char *name = str(p, "alert_name_en");
        bool dup = false;                                       // same alert, neighbouring region
        for (int i = 0; i < out->n; i++) if (!strcasecmp(out->a[i].name[0], name)) dup = true;
        if (dup || !name[0]) continue;
        const char *col = str(p, "risk_colour_en");
        char colour = !strcmp(col, "red") ? 'r' : !strcmp(col, "orange") ? 'o' : !strcmp(col, "yellow") ? 'y' : 'g';
        alert_t *a;
        if (out->n < ALERTS_MAX) a = &out->a[out->n++];
        else {                                                  // full: replace the least severe, if this one is worse
            a = &out->a[0];
            for (int i = 1; i < out->n; i++) if (severity(out->a[i].colour) <= severity(a->colour)) a = &out->a[i];
            if (severity(colour) <= severity(a->colour)) continue;
        }
        strlcpy(a->id, cJSON_GetStringValue(cJSON_GetObjectItem(f, "id")) ?: "", sizeof(a->id));
        strlcpy(a->code, str(p, "alert_code"), sizeof(a->code));
        strlcpy(a->feature, str(p, "feature_id"), sizeof(a->feature));
        static const char *const suffix[ALERT_LANGS] = { "en", "fr" };
        for (int l = 0; l < ALERT_LANGS; l++) {                 // both languages, so a switch is instant
            char k[24];
            snprintf(k, sizeof(k), "alert_name_%s", suffix[l]);
            utf8_copy(a->name[l], l == 0 ? name : str(p, k), sizeof(a->name[l]));
            if (!a->name[l][0]) utf8_copy(a->name[l], name, sizeof(a->name[l]));
            a->name[l][0] = toupper((unsigned char)a->name[l][0]);   // ASCII only: "avis de gel" -> "Avis de gel"
            snprintf(k, sizeof(k), "feature_name_%s", suffix[l]);
            utf8_copy(a->area[l], str(p, k), sizeof(a->area[l]));
            snprintf(k, sizeof(k), "alert_text_%s", suffix[l]);
            utf8_copy(a->text[l], str(p, k), sizeof(a->text[l]));
            char *boiler = strstr(a->text[l], l == 0 ? "\n\nPlease continue to monitor" : "\n\nVeuillez continuer");
            if (boiler) *boiler = 0;                                // standard closing paragraph
        }
        a->colour = colour;
        a->ends = ends;
    }
    qsort(out->a, out->n, sizeof(alert_t), by_severity);
}

// The warning an alert belongs to: its code, else (an old reply without one) its name
static const char *warning_of(const alert_t *a) { return a->code[0] ? a->code : a->name[0]; }

char alerts_to_sound(alerts_seen_t *seen, const alerts_t *al)
{
    char best = 0;
    int best_sev = -1;                                          // (a grey statement sounds too, at "all alerts")
    for (int i = 0; i < al->n; i++) {
        const alert_t *a = &al->a[i];
        int k = 0;
        while (k < seen->n && strcmp(seen->s[k].code, warning_of(a))) k++;
        if (k < seen->n && severity(a->colour) <= severity(seen->s[k].colour)) continue;   // known, not worse
        if (k == seen->n) {                                     // new: drop the oldest if full
            if (seen->n == ALERTS_SEEN_MAX) { memmove(&seen->s[0], &seen->s[1], sizeof(seen->s[0]) * (ALERTS_SEEN_MAX - 1)); k = --seen->n; }
            strlcpy(seen->s[k].code, warning_of(a), sizeof(seen->s[k].code));
            seen->n++;
        }
        seen->s[k].colour = a->colour;
        if (seen->primed && severity(a->colour) > best_sev) { best_sev = severity(a->colour); best = a->colour; }
    }
    seen->primed = true;
    return best;
}

void alerts_map_key(const alert_t *a, char *out, int size)
{
    if (a->code[0] && a->feature[0]) snprintf(out, size, "%s|%s", a->code, a->feature);
    else strlcpy(out, a->id, size);
}

bool alerts_fetch(double lat, double lon, alerts_t *out)
{
    char url[300];
    const double d = 0.005;                                     // ~500 m box around the point
    snprintf(url, sizeof(url), "https://api.weather.gc.ca/collections/weather-alerts/items"
             "?f=json&lang=en&skipGeometry=true&limit=20&bbox=%.4f,%.4f,%.4f,%.4f",
             lon - d, lat - d, lon + d, lat + d);
    rx_t rx;
    if (!rx_init(&rx, 65536)) return false;
    esp_http_client_config_t cfg = {
        .url = url, .event_handler = http_evt, .user_data = &rx,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000, .buffer_size = 2048,
    };
    int64_t t0 = esp_timer_get_time();
    int status;
    esp_err_t err = http_once(&cfg, &status);
    svc_http(SVC_ALERTS, err, status, t0);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "request failed: %s, status %d", esp_err_to_name(err), status);
        free(rx.buf);
        return false;
    }
    cJSON *root = cJSON_Parse(rx.buf);
    free(rx.buf);
    cJSON *features = cJSON_GetObjectItem(root, "features");
    if (!cJSON_IsArray(features)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "unexpected response");
        svc_fail_why(SVC_ALERTS, SVC_WHY_BAD_REPLY, t0);
        return false;
    }

    parse_features(features, time(NULL), out);
    cJSON_Delete(root);
    if (out->n) ESP_LOGI(TAG, "%d alert(s): %s (%c)%s", out->n, out->a[0].name[0], out->a[0].colour, out->n > 1 ? " ..." : "");
    else ESP_LOGI(TAG, "no alerts");
    return true;
}

/* ---------------- region map ---------------- */

#include <math.h>
#include "radar.h"
#include "display.h"

#define MAX_PTS 6000
// n positions (at most cap) in rings; x, y: crop pixels, later
typedef struct { float *x, *y; int n, cap; int *ring_end; int nrings; } shape_t;

// Pull every [lon, lat] position out of the "geometry" of a GeoJSON feature (Polygon or MultiPolygon)
// without building a cJSON tree (a detailed region has thousands of points). Rings end where an array of
// positions closes.
static bool parse_shape(const char *js, shape_t *sh, float *lon, float *lat)
{
    const char *p = strstr(js, "\"coordinates\"");
    if (!p || !(p = strchr(p, '['))) return false;
    int depth = 0, nums = 0;
    double v[2];
    bool had_pos = false;                    // current array holds positions
    sh->n = sh->nrings = 0;
    for (; *p; p++) {
        if (*p == '[') { depth++; nums = 0; had_pos = false; }
        else if (*p == ']') {
            if (nums == 2) {                 // closed a position
                if (sh->n < sh->cap) { lon[sh->n] = v[0]; lat[sh->n] = v[1]; sh->n++; }
                had_pos = true;
            } else if (had_pos) {            // closed a ring
                if (sh->nrings < 64) sh->ring_end[sh->nrings++] = sh->n;
                had_pos = false;
            }
            nums = 0;
            if (--depth == 0) break;
        } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
            char *end;
            double d = strtod(p, &end);
            if (end == p) continue;          // a '-' that starts no number ("-]"): skip it, or this loop never ends
            if (nums < 2) v[nums] = d;
            nums++;
            p = end - 1;
        }
    }
    return sh->n >= 3 && sh->nrings > 0;
}

static inline uint16_t blend565(uint16_t a, uint16_t b, int alpha)   // alpha 0..255 of b over a
{
    int r = ((a >> 11) * (255 - alpha) + (b >> 11) * alpha) / 255;
    int g = (((a >> 5) & 63) * (255 - alpha) + ((b >> 5) & 63) * alpha) / 255;
    int bl = ((a & 31) * (255 - alpha) + (b & 31) * alpha) / 255;
    return (r << 11) | (g << 5) | bl;
}

static void put(uint16_t *img, int w, int h, int x, int y, uint16_t c)
{
    if (x >= 0 && y >= 0 && x < w && y < h) img[y * w + x] = c;
}

static void line(uint16_t *img, int w, int h, float x0, float y0, float x1, float y1, uint16_t c)
{
    int steps = (int)fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
    if (steps > 4000) return;                                    // way off screen
    for (int i = 0; i <= steps; i++) {
        int x = (int)(x0 + (x1 - x0) * i / steps), y = (int)(y0 + (y1 - y0) * i / steps);
        put(img, w, h, x, y, c); put(img, w, h, x + 1, y, c); put(img, w, h, x, y + 1, c);
    }
}

static int cmp_float(const void *a, const void *b) { float d = *(float *)a - *(float *)b; return (d > 0) - (d < 0); }

// The positions a reply can hold: each one closes with a ']' (rings and polygons too: an upper bound), at most MAX_PTS
static int shape_cap(const char *js)
{
    int n = 0;
    for (const char *p = js; *p && n < MAX_PTS; p++) n += *p == ']';
    return n;
}

static void no_memory(const char *what)
{
    ESP_LOGW(TAG, "region map: no memory for %s (%u KB of PSRAM free, largest %u KB)", what,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
}

// The shape first, then the picture, each with only what it needs. Up to v1.14.1 every buffer was taken at once (160 KB
// for the reply, 96 KB of points, a 434 KB copy of the whole cached map to crop 117 KB from it): ~830 KB of PSRAM on top
// of the picture cache, and PSRAM's low point fell to 11-190 KB with a frost advisory at home (October 5).
uint16_t *alerts_map(const alert_t *a, double lat, double lon, int w, int h)
{
    if (!a->id[0]) return NULL;
    char url[200];
    snprintf(url, sizeof(url), "https://api.weather.gc.ca/collections/weather-alerts/items/%s?f=json", a->id);
    rx_t rx;
    int ring_end[64];
    shape_t sh = { .ring_end = ring_end };
    float *plon = NULL, *plat = NULL;
    bool ok = rx_init(&rx, 160 * 1024);   // a county is ~4 KB, a coast ~12; MAX_PTS points fit (512 KB until v1.12.0)
    if (!ok) no_memory("the shape");
    if (ok) {
        esp_http_client_config_t cfg = {
            .url = url, .event_handler = http_evt, .user_data = &rx,
            .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 20000, .buffer_size = 2048,
        };
        int64_t t0 = esp_timer_get_time();
        int status;
        esp_err_t err = http_once(&cfg, &status);
        svc_http(SVC_ALERTS, err, status, t0);
        ok = err == ESP_OK && status == 200 && rx.len < rx.cap - 1;
        if (ok) {
            sh.cap = shape_cap(rx.buf);
            plon = heap_caps_malloc((sh.cap ? sh.cap : 1) * 2 * sizeof(float), MALLOC_CAP_SPIRAM);
            if (plon) plat = plon + sh.cap; else no_memory("the points");
            ok = plon && parse_shape(rx.buf, &sh, plon, plat);
        }
        ESP_LOGI(TAG, "region shape: %d bytes, %d points, %d rings%s", rx.len, sh.n, sh.nrings, ok ? "" : " (failed)");
    }
    free(rx.buf);                                                // (the reply is no longer needed)
    uint16_t *img = NULL;
    float *xs = NULL;
    int xs_cap = sh.n + 1;                                       // a scanline crosses each edge at most once
    if (ok) {
        img = heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM);
        xs = heap_caps_malloc(xs_cap * sizeof(float), MALLOC_CAP_SPIRAM);
        ok = img && xs;
        if (!ok) no_memory("the picture");
    }
    if (ok) {
        // Biggest zoom where the region (plus a margin) fits in the crop, else the widest map
        int z = RADAR_ZOOM_MAX;
        double lat_r = lat * M_PI / 180.0;
        for (; z >= RADAR_ZOOM_MIN; z--) {
            double n = 256.0 * (1 << z);
            double cx = (lon + 180.0) / 360.0 * n, cy = (1.0 - log(tan(lat_r) + 1.0 / cos(lat_r)) / M_PI) / 2.0 * n;
            float dx = 0, dy = 0;
            for (int i = 0; i < sh.n; i++) {
                double la = plat[i] * M_PI / 180.0;
                double x = (plon[i] + 180.0) / 360.0 * n, y = (1.0 - log(tan(la) + 1.0 / cos(la)) / M_PI) / 2.0 * n;
                dx = fmaxf(dx, fabs(x - cx)); dy = fmaxf(dy, fabs(y - cy));
            }
            if ((dx < w / 2 - 12 && dy < h / 2 - 12) || z == RADAR_ZOOM_MIN) break;
        }
        if (z > RADAR_ZOOM_MIN) z--;                             // one level out: the region plus its surroundings
        double nz = 256.0 * (1 << z), ox, oy;                    // map origin for this location (as radar.c)
        ox = floor((lon + 180.0) / 360.0 * nz) - DISP_W / 2;
        oy = floor((1.0 - log(tan(lat_r) + 1.0 / cos(lat_r)) / M_PI) / 2.0 * nz) - DISP_H / 2;
        int cx0 = (DISP_W - w) / 2, cy0 = (DISP_H - h) / 2;      // crop of the centred map
        bool have_map = radar_basemap_crop(z, ox, oy, cx0, cy0, w, h, img);
        if (!have_map) radar_osm_render(z, ox + cx0, oy + cy0, img, w, h);   // not cached (e.g. not all loaded yet)
        double n = 256.0 * (1 << z);
        for (int i = 0; i < sh.n; i++) {                         // to crop pixels, in place
            double la = plat[i] * M_PI / 180.0;
            plon[i] = (plon[i] + 180.0) / 360.0 * n - ox - cx0;
            plat[i] = (1.0 - log(tan(la) + 1.0 / cos(la)) / M_PI) / 2.0 * n - oy - cy0;
        }
        sh.x = plon; sh.y = plat;
        uint16_t col = a->colour == 'r' ? 0xFA69 : a->colour == 'o' ? 0xFC47 : a->colour == 'y' ? 0xFE47 : 0x8CB4;
        for (int y = 0; y < h; y++) {                            // even-odd scanline fill, 35 %
            int nx = 0, start = 0;
            float fy = y + 0.5f;
            for (int r = 0; r < sh.nrings; r++) {
                int end = sh.ring_end[r];
                for (int i = start; i < end; i++) {
                    int j = i + 1 < end ? i + 1 : start;
                    float y0 = sh.y[i], y1 = sh.y[j];
                    if ((y0 <= fy) != (y1 <= fy) && nx < xs_cap)
                        xs[nx++] = sh.x[i] + (fy - y0) / (y1 - y0) * (sh.x[j] - sh.x[i]);
                }
                start = end;
            }
            qsort(xs, nx, sizeof(float), cmp_float);
            for (int k = 0; k + 1 < nx; k += 2) {
                int x0 = (int)fmaxf(0, ceilf(xs[k] - 0.5f)), x1 = (int)fminf(w - 1, floorf(xs[k + 1] - 0.5f));
                for (int x = x0; x <= x1; x++) img[y * w + x] = blend565(img[y * w + x], col, 90);
            }
        }
        int start = 0;
        for (int r = 0; r < sh.nrings; r++) {                    // outline
            int end = sh.ring_end[r];
            for (int i = start; i < end; i++) {
                int j = i + 1 < end ? i + 1 : start;
                line(img, w, h, sh.x[i], sh.y[i], sh.x[j], sh.y[j], col);
            }
            start = end;
        }
        for (int y = -6; y <= 6; y++)                            // location dot
            for (int x = -6; x <= 6; x++) {
                int d = x * x + y * y;
                if (d <= 36) put(img, w, h, w / 2 + x, h / 2 + y, d <= 16 ? 0xFFFF : 0x0000);
            }
        ESP_LOGI(TAG, "region map at zoom %d%s", z, have_map ? "" : " (no cached basemap)");
    }
    free(plon); free(xs);
    if (!ok) { free(img); return NULL; }
    return img;
}
