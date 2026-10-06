// Rain radar screen: OpenStreetMap basemap (dimmed) + Environment Canada GeoMet radar,
// ~200 km radius around the city, Web Mercator zoom 7 (~840 m/pixel at 47°N).
#include "radar.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"   // xTaskCreatePinnedToCoreWithCaps
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "display.h"
#include "slide.h"
#include "png_rows.h"
#include "net.h"
#include "config.h"
#include "services.h"
#include "i18n.h"
#include "esp_timer.h"

static const char *TAG = "radar";

#define W        DISP_W
#define H        DISP_H
#define ZOOM_MIN 4          // ~1560 km radius at 47°N
#define ZOOM_MAX 10         // ~24 km radius (radar data is 1 km/px; closer would just be blocky)
#define ZOOM_DEF 7          // ~195 km radius
#define SLOT_SIZE (512 * 1024)   // one cached basemap per zoom level in the mapcache partition
#define REFRESH_S  (6 * 60)
#define MERC_MAX 20037508.342789244

static uint16_t *base565;   // cached basemap
static uint16_t *out565;    // basemap + radar, shown on screen
static lv_image_dsc_t dsc;
static lv_obj_t *scr, *img, *lbl_title, *lbl_status, *ring, *ring_lbl, *bar, *lbl_rtime;
static lv_obj_t *pnl, *pnl_body, *pnl_bar, *pnl_title;     // "Preparing maps" panel (background preload)
static TaskHandle_t task;
static volatile bool visible;
static bool base_ok;
static time_t last_fetch;
static volatile bool relocate_pending;
static volatile bool loc_changed;          // relocation because the saved location changed (not a zoom)
static volatile bool preload_req, preloading;
static volatile bool preload_on;          // radar_preload_start() was called: a return to the first place preloads too
                                          // (the browser emulator never calls it: it loads the zoom shown only)
static int pre_done, pre_total;
static int last_tiles_ok;                 // tiles that arrived in the last load_basemap()
static int zoom = ZOOM_DEF;
static int radius_km = 195;
static double view_lat = 0.817;          // radians, set by apply_view()

// View radius in km at zoom z for the current latitude (W/2 pixels)
static int radius_at(int z)
{
    double m_per_px = 2 * MERC_MAX / (256.0 * (1 << z)) * cos(view_lat);
    return (int)(W / 2 * m_per_px / 1000.0 + 0.5);
}            // current zoom (used by the radar task)
static volatile int zoom_target = ZOOM_DEF;
static esp_http_client_handle_t geo_h;   // keep-alive connection to GeoMet
static double view_x, view_y;       // top-left of view in world pixels at `zoom`

/* ---------------- HTTP download into a growing PSRAM buffer ---------------- */

typedef struct { uint8_t *buf; size_t len, cap; bool oom; } dl_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    dl_t *d = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (d->len + e->data_len + 1 > d->cap) {
        size_t nc = (d->cap ? d->cap * 2 : 65536);
        while (nc < d->len + e->data_len + 1) nc *= 2;
        uint8_t *nb = heap_caps_realloc(d->buf, nc, MALLOC_CAP_SPIRAM);
        if (!nb) { d->oom = true; return ESP_FAIL; }   // (the client ignores this: http_fetch checks oom)
        d->buf = nb; d->cap = nc;
    }
    memcpy(d->buf + d->len, e->data, e->data_len);
    d->len += e->data_len;
    d->buf[d->len] = 0;
    return ESP_OK;
}

// Fetch url using *hp (created on first use, reused for keep-alive on the same host)
static bool http_fetch(esp_http_client_handle_t *hp, const char *url, dl_t *d)
{
    memset(d, 0, sizeof(*d));
    if (!*hp) {
        esp_http_client_config_t c = {
            .url = url, .event_handler = on_http,
            .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000,
            .user_agent = svc_user_agent(), .buffer_size = 4096,
            .keep_alive_enable = true,
        };
        *hp = esp_http_client_init(&c);
        if (!*hp) {                            // memory short: perform() would dereference NULL
            ESP_LOGW(TAG, "GET failed (no memory for a client): %.90s", url);
            return false;
        }
    } else {
        esp_http_client_set_url(*hp, url);
    }
    esp_http_client_set_user_data(*hp, d);
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(*hp);
    int st = esp_http_client_get_status_code(*hp);
    int id = strstr(url, "openstreetmap") ? SVC_TILES : SVC_RADAR;
    if (err == ESP_OK && st == 200 && d->len == 0) svc_fail_why(id, SVC_WHY_EMPTY, t0);
    else svc_http(id, err, st, t0);
    if (err != ESP_OK) {                       // drop the connection, start fresh next time
        esp_http_client_cleanup(*hp);
        *hp = NULL;
    }
    if (err != ESP_OK || st != 200 || d->len == 0 || d->oom) {
        ESP_LOGW(TAG, "GET failed (%s, %d%s): %.90s", esp_err_to_name(err), st, d->oom ? ", no memory for the body" : "", url);
        free(d->buf); d->buf = NULL;
        return false;
    }
    if (d->cap > d->len + 1) {                 // give back the slack: the buffer grows by doubling from 64 KB, and
        uint8_t *nb = heap_caps_realloc(d->buf, d->len + 1, MALLOC_CAP_SPIRAM);   // two frames wait in pipe_load()
        if (nb) { d->buf = nb; d->cap = d->len + 1; }
    }
    return true;
}


/* ---------------- basemap cache in flash ("mapcache" partition) ---------------- */

typedef struct { uint32_t magic; int32_t zoom, x, y; } cache_hdr_t;
#define CACHE_MAGIC 0x4D415037  // "MAP7" (per-zoom slots, background preload)

static SemaphoreHandle_t cache_mux;      // cache_save() vs readers in other tasks (radar_basemap_read)

static const esp_partition_t *cache_part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "mapcache");
}

static bool cache_load(void)
{
    const esp_partition_t *p = cache_part();
    cache_hdr_t h;
    size_t base = (size_t)(zoom - ZOOM_MIN) * SLOT_SIZE;
    if (!p || base + SLOT_SIZE > p->size) return false;
    if (esp_partition_read(p, base, &h, sizeof(h)) != ESP_OK) return false;
    if (h.magic != CACHE_MAGIC || h.zoom != zoom || h.x != (int)view_x || h.y != (int)view_y) return false;
    // In 4 KB pieces: a flash read into PSRAM goes through an internal buffer the size of the read, up to 16 KB, taken
    // from the 32 KB kept for internal-only and DMA memory. Read whole, the map left 15 KB of it, and internal RAM's
    // low point at a return to the first place (alerts and air quality loading meanwhile) was this read (October 5).
    // Costs ~20 ms per map (50 -> 70 ms), in this task.
    xSemaphoreTake(cache_mux, portMAX_DELAY);
    bool ok = true;
    for (size_t off = 0; ok && off < W * H * 2; off += 4096)
        ok = esp_partition_read(p, base + sizeof(h) + off, (uint8_t *)base565 + off,
                                W * H * 2 - off < 4096 ? W * H * 2 - off : 4096) == ESP_OK;
    xSemaphoreGive(cache_mux);
    return ok;
}

// Top-left world pixel of the map centred on the location at zoom z (same formula as apply_view)
static void view_origin(int z, double lat_deg, double lon_deg, double *x, double *y)
{
    double lat = lat_deg * M_PI / 180.0, n = 256.0 * (1 << z);
    *x = floor((lon_deg + 180.0) / 360.0 * n) - W / 2;
    *y = floor((1.0 - log(tan(lat) + 1.0 / cos(lat)) / M_PI) / 2.0 * n) - H / 2;
}

// Only the window's rows, straight from flash: the alert map read the whole 434 KB map into PSRAM to keep a 117 KB crop
// (with its other buffers, PSRAM's low point fell to 11-190 KB with an alert, October 5)
bool radar_basemap_crop(int z, double ox, double oy, int x0, int y0, int w, int h, uint16_t *dst)
{
    const esp_partition_t *p = cache_part();
    if (!p || z < ZOOM_MIN || z > ZOOM_MAX || !cache_mux || x0 < 0 || y0 < 0 || x0 + w > W || y0 + h > H) return false;
    size_t base = (size_t)(z - ZOOM_MIN) * SLOT_SIZE;
    cache_hdr_t hd;
    xSemaphoreTake(cache_mux, portMAX_DELAY);
    bool ok = esp_partition_read(p, base, &hd, sizeof(hd)) == ESP_OK && hd.magic == CACHE_MAGIC && hd.zoom == z &&
              hd.x == (int)ox && hd.y == (int)oy;
    for (int y = 0; ok && y < h; y++)
        ok = esp_partition_read(p, base + sizeof(hd) + ((size_t)(y0 + y) * W + x0) * 2, dst + y * w, w * 2) == ESP_OK;
    xSemaphoreGive(cache_mux);
    return ok;
}

static bool cache_header_ok(void)
{
    const esp_partition_t *p = cache_part();
    cache_hdr_t h;
    size_t base = (size_t)(zoom - ZOOM_MIN) * SLOT_SIZE;
    if (!p || base + SLOT_SIZE > p->size || esp_partition_read(p, base, &h, sizeof(h)) != ESP_OK) return false;
    return h.magic == CACHE_MAGIC && h.zoom == zoom && h.x == (int)view_x && h.y == (int)view_y;
}

// Only the first place (home) is cached: switching to another place would otherwise rewrite up to 3.5 MB of
// flash and fetch every zoom level from OSM each time. Other places load the zoom in use on demand.
// Flash writes stall both cores (the PSRAM and flash cache is off during each ~40 ms sector erase): not while someone
// is using the screen (slide_screen_busy), at most 10 s of waiting per map
static void wait_screen_quiet(int *waited_ms)
{
    while (slide_screen_busy() && *waited_ms < 10000) { vTaskDelay(pdMS_TO_TICKS(50)); *waited_ms += 50; }
}

static void cache_save(void)
{
    // Only the first place's maps are cached: checked by the view the tiles were downloaded for, not by the place shown
    // now. A download for another place that ended just after a switch back was saved in the first place's slot
    // (under the other place's view, so never shown, but the first place's map was lost: downloaded and written
    // again, two rounds of ~3 s of flash writes after every return to the first place).
    location_t home;
    double hx, hy;
    if (!config_get_place(0, &home)) return;
    view_origin(zoom, home.lat, home.lon, &hx, &hy);
    if (hx != view_x || hy != view_y) return;
    const esp_partition_t *p = cache_part();
    if (!p) return;
    int waited = 0;
    const size_t SECT = 4096;
    size_t base = (size_t)(zoom - ZOOM_MIN) * SLOT_SIZE;
    if (base + SLOT_SIZE > p->size) return;
    size_t len = (sizeof(cache_hdr_t) + W * H * 2 + SECT - 1) & ~(SECT - 1);
    xSemaphoreTake(cache_mux, portMAX_DELAY);
    // Erase and write one sector at a time with a short pause in between, so the idle task
    // (and the task watchdog) get to run: a single 450 KB erase blocks this core for seconds.
    for (size_t off = 0; off < len; off += SECT) {
        if (slide_screen_busy()) { xSemaphoreGive(cache_mux); wait_screen_quiet(&waited); xSemaphoreTake(cache_mux, portMAX_DELAY); }
        if (esp_partition_erase_range(p, base + off, SECT) != ESP_OK) { xSemaphoreGive(cache_mux); return; }
        vTaskDelay(1);
    }
    const uint8_t *src = (const uint8_t *)base565;
    size_t total = W * H * 2;
    for (size_t off = 0; off < total; off += SECT) {
        size_t n = total - off < SECT ? total - off : SECT;
        if (slide_screen_busy()) { xSemaphoreGive(cache_mux); wait_screen_quiet(&waited); xSemaphoreTake(cache_mux, portMAX_DELAY); }
        if (esp_partition_write(p, base + sizeof(cache_hdr_t) + off, src + off, n) != ESP_OK) { xSemaphoreGive(cache_mux); return; }
        vTaskDelay(1);
    }
    cache_hdr_t h = { CACHE_MAGIC, zoom, (int)view_x, (int)view_y };   // header last = valid only when complete
    if (esp_partition_write(p, base, &h, sizeof(h)) == ESP_OK) ESP_LOGI(TAG, "Basemap (zoom %d) cached to flash", zoom);
    xSemaphoreGive(cache_mux);
}

static void set_status(const char *title, const char *status);

/* ---------------- image helpers ---------------- */

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// OSM's standard style is bright; desaturate half-way and dim it for a dark AMOLED-friendly map
static uint16_t dim_map(uint8_t r, uint8_t g, uint8_t b);

// Images are decoded a row at a time (png_rows.c, ~50 KB): lodepng decoded whole images (~2-3 MB for a radar frame,
// in two big blocks) and failed once the drag pictures (slide.c) had fragmented PSRAM.
typedef struct { uint16_t *dst; int dw, dh, ox, oy; } tile_t;     // a 256 px map tile into a w x h window at ox, oy

static bool tile_row(unsigned y, const uint8_t *rgba, unsigned w, void *user)
{
    const tile_t *t = user;
    if ((y & 63) == 63) vTaskDelay(1);
    int sy = t->oy + (int)y;
    if (sy < 0 || sy >= t->dh) return sy < t->dh;                  // below the window: done
    for (unsigned x = 0; x < w; x++) {
        int sx = t->ox + (int)x;
        if (sx >= 0 && sx < t->dw) t->dst[sy * t->dw + sx] = dim_map(rgba[x * 4], rgba[x * 4 + 1], rgba[x * 4 + 2]);
    }
    return true;
}

static uint16_t dim_map(uint8_t r, uint8_t g, uint8_t b)
{
    int l = (77 * r + 150 * g + 29 * b) >> 8;
    int R = ((l + r) / 2) * 55 / 100, G = ((l + g) / 2) * 55 / 100, B = ((l + b) / 2) * 55 / 100;
    return rgb565(R, G, B);
}

// Update the "Preparing maps" panel on the radar screen (radar task)
static void preload_progress(int done)
{
    display_lock(-1);
    int levels = (pre_total + 8) / 9, level = done / 9 + 1;
    if (level > levels) level = levels;
    lv_label_set_text_fmt(pnl_body, tr(T_PREP_PROGRESS), level, levels, done, pre_total);
    lv_bar_set_range(pnl_bar, 0, pre_total > 0 ? pre_total : 1);
    lv_bar_set_value(pnl_bar, done, LV_ANIM_OFF);
    if (!lv_obj_has_flag(pnl, LV_OBJ_FLAG_HIDDEN)) slide_cache_dirty(scr);   // (as set_status)
    display_unlock();
}

static bool load_basemap(void)
{
    for (int i = 0; i < W * H; i++) base565[i] = rgb565(18, 20, 24);
    int tx0 = (int)floor(view_x / 256), tx1 = (int)floor((view_x + W - 1) / 256);
    int ty0 = (int)floor(view_y / 256), ty1 = (int)floor((view_y + H - 1) / 256);
    int total = (tx1 - tx0 + 1) * (ty1 - ty0 + 1), ok = 0, n = 0;
    char url[128], msg[40];
    esp_http_client_handle_t h = NULL;
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            if (relocate_pending || (!preloading && zoom_target != zoom)) {   // user moved on: stop, don't cache
                if (h) esp_http_client_cleanup(h);
                ESP_LOGI(TAG, "Basemap (zoom %d) cancelled", zoom);
                return false;
            }
            snprintf(msg, sizeof(msg), tr(T_LOADING_MAP), ++n, total);
            set_status(NULL, msg);
            snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%d/%d.png", zoom, tx, ty);
            dl_t d;
            bool got = false;
            for (int attempt = 0; attempt < 3 && !got; attempt++) got = http_fetch(&h, url, &d);
            if (!got) { if (preloading) preload_progress(++pre_done); continue; }
            tile_t t = { base565, W, H, tx * 256 - (int)view_x, ty * 256 - (int)view_y };
            bool decoded = png_rows(d.buf, d.len, tile_row, &t, NULL, NULL);
            free(d.buf);
            if (!decoded) continue;
            ok++;
            if (preloading) preload_progress(++pre_done);
        }
    }
    if (h) esp_http_client_cleanup(h);
    last_tiles_ok = ok;
    ESP_LOGI(TAG, "Basemap: %d/%d tiles", ok, total);
    if (ok == total) cache_save();
    return ok == total;
}

// Draw the dimmed OSM map for any w x h window whose top-left is (ox, oy) in world pixels at zoom z
// (used by the alert map when the radar cache doesn't cover it). Other task, own connection.
bool radar_osm_render(int z, double ox, double oy, uint16_t *dst, int w, int h)
{
    for (int i = 0; i < w * h; i++) dst[i] = rgb565(18, 20, 24);
    int tx0 = (int)floor(ox / 256), tx1 = (int)floor((ox + w - 1) / 256);
    int ty0 = (int)floor(oy / 256), ty1 = (int)floor((oy + h - 1) / 256);
    int ok = 0, total = (tx1 - tx0 + 1) * (ty1 - ty0 + 1);
    char url[128];
    esp_http_client_handle_t hc = NULL;
    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%d/%d.png", z, tx, ty);
            dl_t d;
            bool got = false;
            for (int attempt = 0; attempt < 2 && !got; attempt++) got = http_fetch(&hc, url, &d);
            if (!got) continue;
            tile_t t = { dst, w, h, tx * 256 - (int)ox, ty * 256 - (int)oy };
            bool decoded = png_rows(d.buf, d.len, tile_row, &t, NULL, NULL);
            free(d.buf);
            if (!decoded) continue;
            ok++;
        }
    }
    if (hc) esp_http_client_cleanup(hc);
    ESP_LOGI(TAG, "Alert map: %d/%d tiles at zoom %d", ok, total, z);
    return ok > 0;
}

// UTC "YYYY-MM-DDTHH:MM:SSZ" -> time_t
static time_t parse_utc(const char *s)
{
    int Y, M, D, h, m, sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    Y -= M <= 2;                                   // days from civil (H. Hinnant)
    int era = (Y >= 0 ? Y : Y - 399) / 400;
    unsigned yoe = (unsigned)(Y - era * 400);
    unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (time_t)(days * 86400L + h * 3600 + m * 60 + sec);
}

static void fmt_local(time_t t, char *out, size_t n)
{
    struct tm tm;
    if (config_local_time(t, &tm)) config_fmt_time(tm.tm_hour, tm.tm_min, out, n);
    else out[0] = 0;
}

/* ---------------- radar frames (palette-indexed, 1 byte/pixel) ---------------- */

#define NFRAMES   15
#define STEP_S    (12 * 60)

#define LTG_MAX   64        // lightning marks per frame
#define LTG_BLOCK 20        // one mark per 20x20 px block that has lightning (same look at every zoom)

typedef struct {
    time_t t;
    bool ok;
    uint8_t *idx;           // W*H palette indices, 0 = no echo
    uint8_t r[256], g[256], b[256], a[256];
    uint8_t nltg;           // lightning marks: centres of the blocks with flashes, at LTG_X(f) / LTG_Y(f)
} frame_t;
// The marks live after the pixels in the same PSRAM buffer (idx is W*H + 2*LTG_MAX), so they move with it in
// plan_frames(). Not in frame_t: it is copied ~46 times (frames, plan_frames, load_frame).
#define LTG_X(f) ((f)->idx + W * H)                 // in 2 px units (466 / 2 fits a byte)
#define LTG_Y(f) ((f)->idx + W * H + LTG_MAX)

// The 46 frame_t below (palettes: ~1 KB each) are in PSRAM: in internal RAM they were 48 KB, ~86 % of main/'s static
// internal RAM, while its low point was 8-10 KB. Read by tasks only (never from an interrupt or while flash is busy).
static EXT_RAM_BSS_ATTR frame_t frames[NFRAMES];   // [NFRAMES-1] is the latest (live) frame
static lv_timer_t *play_timer;
static int play_i;
static volatile bool play_pending;

// Latest available radar time from GetCapabilities (<Dimension name="time" ...>start/end/PT6M<)
static time_t latest_radar_time(void)
{
    dl_t d;
    bool got = false;
    // GeoMet drops idle keep-alive connections; the first attempt then fails and the retry reconnects
    for (int attempt = 0; attempt < 3 && !got; attempt++)
        got = http_fetch(&geo_h, "https://geo.weather.gc.ca/geomet?service=WMS&version=1.3.0&request=GetCapabilities&layer=RADAR_1KM_RRAI", &d);
    if (!got) return 0;
    time_t t = 0;
    char *p = strstr((char *)d.buf, "name=\"time\"");
    if (p) {
        char *def = strstr(p, "default=\"");
        char *gt = strchr(p, '>');
        char *slash = gt ? strchr(gt, '/') : NULL;
        if (slash && slash - gt < 40) t = parse_utc(slash + 1);          // end of range
        if (!t && def) t = parse_utc(def + 9);
    }
    free(d.buf);
    return t;
}

// GetMap of a GeoMet layer for the current view at time t (466x466, Web Mercator, transparent PNG)
static void geomet_url(char *url, size_t n, const char *layer, time_t t, char *iso, size_t isz)
{
    double res = 2 * MERC_MAX / (256.0 * (1 << zoom));
    double minx = view_x * res - MERC_MAX, maxx = (view_x + W) * res - MERC_MAX;
    double maxy = MERC_MAX - view_y * res, miny = MERC_MAX - (view_y + H) * res;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(iso, isz, "%Y-%m-%dT%H:%M:%SZ", &tm);
    snprintf(url, n,
             "https://geo.weather.gc.ca/geomet?service=WMS&version=1.3.0&request=GetMap"
             "&layers=%s&styles=&crs=EPSG:3857&bbox=%.1f,%.1f,%.1f,%.1f"
             "&width=%d&height=%d&format=image/png&transparent=true&time=%s",
             layer, minx, miny, maxx, maxy, W, H, iso);
}

/* Lightning (Canadian Lightning Detection Network via GeoMet, Lightning_2.5km_Density): flashes of the last
 * 10 minutes on a 2.5 km grid, published every 10 min, kept 3 h, Canada + 250 km. The image (density colours on
 * small squares) would be lost in the rain, so each 20x20 px block with any flash becomes one bolt mark at the
 * flashes' centre. Optional: a failed request leaves the frame without marks. */
enum { LTG_BW = (W + LTG_BLOCK - 1) / LTG_BLOCK, LTG_BH = (H + LTG_BLOCK - 1) / LTG_BLOCK };

static bool lightning_row(unsigned y, const uint8_t *rgba, unsigned w, void *user)
{
    uint32_t *sx = user, *sy = sx + LTG_BW * LTG_BH, *cnt = sy + LTG_BW * LTG_BH;
    if ((y & 63) == 63) vTaskDelay(1);
    if (w != W || y >= H) return false;
    for (int x = 0; x < W; x++) {
        if (!rgba[x * 4 + 3]) continue;
        int b = (y / LTG_BLOCK) * LTG_BW + x / LTG_BLOCK;
        sx[b] += x; sy[b] += y; cnt[b]++;
    }
    return true;
}

// The lightning image for radar time t: the newest window may not be published yet (an XML error instead of a PNG),
// then the one before. Returns the PNG in *d (caller frees d->buf) and its window in iso.
static bool dl_lightning(time_t t, dl_t *d, char *iso, size_t isz)
{
    time_t lt = t / 600 * 600;                  // the 10-min window the radar time falls in
    char url[420];
    for (int k = 0; k < 2; k++, lt -= 600) {
        geomet_url(url, sizeof(url), "Lightning_2.5km_Density", lt, iso, isz);
        if (!http_fetch(&geo_h, url, d)) continue;
        if (d->len > 8 && !memcmp(d->buf, "\x89PNG", 4)) return true;
        free(d->buf); d->buf = NULL;
    }
    return false;
}

// GeoMet sends the same bytes for every lightning image without a flash (466x466 transparent RGBA, ~920 bytes), and
// inflating its 868 KB took ~135 ms per frame, a quarter of a loop's load time on a quiet day. A response identical to
// the last one that held no flash holds none either. Used by one task at a time (pipe_load() drains first).
static EXT_RAM_BSS_ATTR uint8_t ltg_empty[4096];
static size_t ltg_empty_len;

// Bolt marks of frame f from a lightning PNG (frees d->buf); got = false: none available
static void dec_lightning(frame_t *f, dl_t *d, bool got, const char *iso)
{
    enum { BW = LTG_BW, BH = LTG_BH };
    f->nltg = 0;
    if (!got) { ESP_LOGW(TAG, "Lightning %s: not available", iso); return; }
    if (ltg_empty_len && d->len == ltg_empty_len && !memcmp(d->buf, ltg_empty, d->len)) {
        free(d->buf); d->buf = NULL;
        ESP_LOGI(TAG, "Lightning %s: 0 px, 0 marks", iso);
        return;
    }
    uint32_t *sx = heap_caps_calloc(3 * BW * BH, sizeof(uint32_t), MALLOC_CAP_SPIRAM);   // internal RAM is scarce
    if (!sx) { free(d->buf); d->buf = NULL; return; }
    uint32_t *sy = sx + BW * BH, *cnt = sy + BW * BH;
    unsigned rw = 0, rh = 0;
    bool ok = png_rows(d->buf, d->len, lightning_row, sx, &rw, &rh) && (int)rw == W && (int)rh == H;
    if (!ok) { ESP_LOGW(TAG, "Lightning %s: not available", iso); free(sx); free(d->buf); d->buf = NULL; return; }
    int flashes = 0;
    uint8_t *lx = LTG_X(f), *ly = LTG_Y(f);
    for (int b = 0; b < BW * BH; b++) {
        if (!cnt[b]) continue;
        flashes += cnt[b];
        if (f->nltg < LTG_MAX) {
            lx[f->nltg] = sx[b] / cnt[b] / 2;
            ly[f->nltg] = sy[b] / cnt[b] / 2;
            f->nltg++;
        }
    }
    free(sx);
    if (!flashes && d->len <= sizeof(ltg_empty)) { memcpy(ltg_empty, d->buf, d->len); ltg_empty_len = d->len; }
    free(d->buf); d->buf = NULL;
    ESP_LOGI(TAG, "Lightning %s: %d px, %d marks", iso, flashes, f->nltg);
}

// Palette-index the RGBA rows (radar styles use a handful of colours), one row at a time (png_rows)
typedef struct { frame_t *f; uint32_t key[256], lastk; int npal, lasti, echoes; } frame_dec_t;

static bool frame_row(unsigned y, const uint8_t *row, unsigned w, void *user)
{
    frame_dec_t *fd = user;
    frame_t *f = fd->f;
    if ((y & 31) == 31) vTaskDelay(1);        // let other tasks / idle run
    if (w != W || y >= H) return false;
    uint8_t *o = f->idx + y * W;
    for (int x = 0; x < W; x++) {
        const uint8_t *p = row + x * 4;
        if (p[3] == 0) { o[x] = 0; continue; }
        fd->echoes++;
        uint32_t k = ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
        if (k != fd->lastk) {
            int i;
            for (i = 1; i < fd->npal && fd->key[i] != k; i++) {}
            if (i == fd->npal) {
                if (fd->npal < 256) {
                    fd->key[fd->npal] = k; f->r[fd->npal] = p[0]; f->g[fd->npal] = p[1]; f->b[fd->npal] = p[2];
                    f->a[fd->npal] = p[3] * 220 / 255;   // keep a hint of the map under the rain
                    fd->npal++;
                } else i = fd->npal - 1;
            }
            fd->lastk = k; fd->lasti = i;
        }
        o[x] = fd->lasti;
    }
    return true;
}

static bool dl_radar(time_t t, dl_t *d)
{
    char iso[24], url[420];
    geomet_url(url, sizeof(url), "RADAR_1KM_RRAI", t, iso, sizeof(iso));
    bool got = false;
    for (int attempt = 0; attempt < 2 && !got; attempt++) got = http_fetch(&geo_h, url, d);
    return got;
}

// Frame f (time t) from its radar PNG (frees d->buf)
static bool dec_radar(time_t t, frame_t *f, dl_t *d)
{
    char iso[24];
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
    unsigned rw = 0, rh = 0;
    frame_dec_t fd = { .f = f, .npal = 1, .lastk = 0xFFFFFFFF };
    bool decoded = png_rows(d->buf, d->len, frame_row, &fd, &rw, &rh);
    if (!decoded) ESP_LOGW(TAG, "Radar response: %.80s", (char *)d->buf);
    free(d->buf); d->buf = NULL;
    if (!decoded || (int)rw != W || (int)rh != H) return false;
    f->t = t;
    ESP_LOGI(TAG, "Frame %s: %d px with echoes, %d colours", iso, fd.echoes, fd.npal - 1);
    return true;
}

static bool fetch_frame(time_t t, frame_t *f)   // one frame, in this task: download and decode in turn
{
    dl_t d;
    if (!dl_radar(t, &d)) return false;
    if (!dec_radar(t, f, &d)) return false;
    char iso[24];
    bool got = dl_lightning(t, &d, iso, sizeof(iso));
    dec_lightning(f, &d, got, iso);
    return true;
}

// Lightning bolt, 13x17: yellow, with a 1 px dark outline so it shows on rain and on the map
static const char *const BOLT[] = {
    ".......######",
    "......######.",
    ".....######..",
    ".....#####...",
    "....#####....",
    "...#####.....",
    "...##########",
    "..##########.",
    "..#########..",
    "......####...",
    ".....####....",
    ".....###.....",
    "....###......",
    "....##.......",
    "...##........",
    "...#.........",
    "..#..........",
};
#define BOLT_W 13
#define BOLT_H 17

static bool bolt_at(int x, int y)
{
    return x >= 0 && y >= 0 && x < BOLT_W && y < BOLT_H && BOLT[y][x] == '#';
}

static void draw_bolt(int cx, int cy)
{
    const uint16_t fill = rgb565(0xFF, 0xD8, 0x2A), edge = rgb565(0x20, 0x18, 0x00);
    int x0 = cx - BOLT_W / 2, y0 = cy - BOLT_H / 2;
    for (int y = -1; y <= BOLT_H; y++) {
        for (int x = -1; x <= BOLT_W; x++) {
            int px = x0 + x, py = y0 + y;
            if (px < 0 || py < 0 || px >= W || py >= H) continue;
            if (bolt_at(x, y)) { out565[py * W + px] = fill; continue; }
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; dy++)
                for (int dx = -1; dx <= 1 && !near; dx++) near = bolt_at(x + dx, y + dy);
            if (near) out565[py * W + px] = edge;
        }
    }
}

// base565 + frame -> out565 (caller holds the display lock)
static void compose(const frame_t *f)
{
    if (!f || !f->ok) { memcpy(out565, base565, W * H * 2); return; }
    for (int i = 0; i < W * H; i++) {
        if (i % (W * 64) == W * 64 - 1) vTaskDelay(1);   // yield every 64 rows
        uint8_t ix = f->idx[i];
        uint16_t b = base565[i];
        if (!ix) { out565[i] = b; continue; }
        unsigned a = f->a[ix];
        unsigned br = (b >> 8) & 0xF8, bg = (b >> 3) & 0xFC, bb = (b << 3) & 0xF8;
        out565[i] = rgb565((f->r[ix] * a + br * (255 - a)) / 255,
                           (f->g[ix] * a + bg * (255 - a)) / 255,
                           (f->b[ix] * a + bb * (255 - a)) / 255);
    }
    for (int k = 0; k < f->nltg; k++) draw_bolt(LTG_X(f)[k] * 2, LTG_Y(f)[k] * 2);
}

#define ZOOM_ANIM_MS 300
static volatile uint32_t anim_until;       // lv_tick when the zoom animation ends

static void scale_cb(void *o, int32_t v) { lv_image_set_scale((lv_obj_t *)o, v); }

// Radar task: let a running zoom animation finish before the new map replaces it
static void wait_zoom_anim(void)
{
    uint32_t now = lv_tick_get();
    if (anim_until > now && anim_until - now < 2000) vTaskDelay(pdMS_TO_TICKS(anim_until - now));
}

// A swipe made while slide.c drew a zoom (LVGL didn't see it): down = zoom in, up = zoom out, as the gestures
static void zoom_swipe(int dx, int dy)
{
    if (abs(dy) >= 40 && abs(dy) > abs(dx)) radar_zoom(dy > 0 ? 1 : -1);
}

static void start_scale_anim(int32_t from, int32_t to)     // caller holds the display lock
{
    lv_anim_delete(img, scale_cb);
    // Drawn by slide.c: scaled frames straight to the panel, ~60 fps (LVGL transforms the whole image for each
    // frame: ~10 fps). LVGL's animation when that can't run (another move going on, or the radar not shown).
    if (slide_zoom(img, out565, from, to, ZOOM_ANIM_MS, zoom_swipe)) {
        anim_until = lv_tick_get() + ZOOM_ANIM_MS + 100;
        return;
    }
    lv_image_set_scale(img, from);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, img);
    lv_anim_set_exec_cb(&a, scale_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ZOOM_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    anim_until = lv_tick_get() + ZOOM_ANIM_MS;
}

static void refresh_img(void)
{
    lv_anim_delete(img, scale_cb);
    lv_image_set_scale(img, LV_SCALE_NONE);
    lv_image_cache_drop(&dsc);
    lv_obj_invalidate(img);
}

// Labels set only when they change: a redraw that changes nothing makes the screen's picture for drags (slide.c) out
// of date. A change marks it, or a drag to the radar showed the old text for a moment.
static void set_status(const char *title, const char *status)
{
    display_lock(-1);
    bool changed = false;
    if (title && strcmp(lv_label_get_text(lbl_title), title)) { lv_label_set_text(lbl_title, title); changed = true; }
    if (status) {
        if (strcmp(lv_label_get_text(lbl_status), status)) { lv_label_set_text(lbl_status, status); changed = true; }
        bool hide = !status[0];
        if (hide != lv_obj_has_flag(lbl_status, LV_OBJ_FLAG_HIDDEN)) {
            if (hide) lv_obj_add_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
            changed = true;
        }
    }
    if (changed) slide_cache_dirty(scr);
    display_unlock();
}

static void show_live(void)      // caller holds the display lock
{
    slide_cache_dirty(scr);      // the cached picture of the radar screen (drags, slide.c) is out of date
    // Newest image that did load: if the latest download failed, keep showing the previous one
    // (its time is in the label, so its age is visible) instead of a map without rain.
    frame_t *f = &frames[NFRAMES - 1];
    for (int i = NFRAMES - 1; i >= 0; i--) if (frames[i].ok) { f = &frames[i]; break; }
    compose(f->ok ? f : NULL);
    char when[12], now[12], sub[40], dist[12];
    fmt_local(time(NULL), now, sizeof(now));
    lv_label_set_text(lbl_title, now[0] ? now : tr(T_RADAR));
    if (config_miles()) snprintf(dist, sizeof(dist), "%d mi", (int)(radius_km / 1.609344 + 0.5));
    else snprintf(dist, sizeof(dist), "%d km", radius_km);
    if (f->ok) { fmt_local(f->t, when, sizeof(when)); snprintf(sub, sizeof(sub), tr(T_RADAR_AT), when, dist); }
    else snprintf(sub, sizeof(sub), tr(T_RADAR_NOTIME), dist);
    lv_label_set_text(lbl_rtime, sub);
    refresh_img();
}

static void radar_clock(lv_timer_t *t)
{
    if (play_timer) return;                      // the pill shows frame times while playing
    char now[12];
    fmt_local(time(NULL), now, sizeof(now));
    if (now[0] && strcmp(now, lv_label_get_text(lbl_title))) {
        lv_label_set_text(lbl_title, now);
        lv_area_t a;                                 // the radar's picture (drags, slide.c): only the pill's rows
        lv_obj_get_coords(lbl_title, &a);
        slide_cache_dirty_rows(scr, a.y1 - 2, a.y2 + 2);
    }
}

/* ---------------- playback (runs in the LVGL task) ---------------- */

static void stop_play(void)
{
    if (play_timer) { lv_timer_delete(play_timer); play_timer = NULL; }
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    show_live();
}

#define PLAY_LOOP_MS  60000      // a tap loops the animation this long; another tap stops it
static uint32_t play_started;

static void play_step(lv_timer_t *t)
{
    while (play_i < NFRAMES && !frames[play_i].ok) play_i++;
    if (play_i >= NFRAMES + 3) {                               // held the last frame ~1 s
        if (lv_tick_elaps(play_started) >= PLAY_LOOP_MS) { stop_play(); return; }
        play_i = 0;                                            // loop
        while (play_i < NFRAMES && !frames[play_i].ok) play_i++;
    }
    if (play_i < NFRAMES) {
        frame_t *f = &frames[play_i];
        compose(f);
        char when[12];
        fmt_local(f->t, when, sizeof(when));
        lv_label_set_text(lbl_title, when);
        lv_label_set_text(lbl_rtime, tr(T_PAST_3H));
        lv_bar_set_value(bar, play_i + 1, LV_ANIM_OFF);
        refresh_img();
    }
    play_i++;
}

static int frames_ready(void)
{
    int n = 0;
    for (int i = 0; i < NFRAMES; i++) n += frames[i].ok;
    return n;
}

static void start_play(void)     // caller holds the display lock
{
    play_pending = false;
    lv_obj_add_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
    if (play_timer) lv_timer_delete(play_timer);
    play_i = 0;
    play_started = lv_tick_get();
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
    play_timer = lv_timer_create(play_step, 333, NULL);   // 3 frames per second
    play_step(play_timer);
}

static void on_tap(lv_event_t *e)
{
    if (play_timer) { stop_play(); return; }
    if (frames_ready() >= NFRAMES) { start_play(); return; }
    play_pending = true;
    char msg[40];
    snprintf(msg, sizeof(msg), tr(T_LOADING_PAST), frames_ready(), NFRAMES);
    lv_label_set_text(lbl_status, msg);
    lv_obj_remove_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
    if (task) xTaskNotifyGive(task);
}

/* ---------------- view / location ---------------- */

// Range ring: the "nice" distance (km, or miles when the wind is in mph) closest to half the view radius
static void ring_update(void)
{
    static const int nice[] = {2, 5, 10, 25, 50, 100, 200, 400, 800};
    double m_per_px = 2 * MERC_MAX / (256.0 * (1 << zoom)) * cos(view_lat);
    bool mi = config_miles();
    double unit_m = mi ? 1609.344 : 1000.0;
    double half = W / 4.0 * m_per_px / unit_m;
    int d = nice[0];
    for (int i = 0; i < (int)(sizeof(nice) / sizeof(nice[0])); i++)
        if (fabs(log(nice[i] / half)) < fabs(log(d / half))) d = nice[i];
    int ring_r = (int)(d * unit_m / m_per_px);
    display_lock(-1);
    lv_label_set_text_fmt(ring_lbl, "%d %s", d, mi ? "mi" : "km");
    lv_obj_set_size(ring, ring_r * 2, ring_r * 2);
    lv_obj_center(ring);
    lv_obj_align(ring_lbl, LV_ALIGN_CENTER, 0, ring_r + 12);
    display_unlock();
}

static void apply_view(void)
{
    location_t loc;
    config_get_location(&loc);
    double lat = loc.lat * M_PI / 180.0;
    double n = 256.0 * (1 << zoom);
    view_x = floor((loc.lon + 180.0) / 360.0 * n) - W / 2;
    view_y = floor((1.0 - log(tan(lat) + 1.0 / cos(lat)) / M_PI) / 2.0 * n) - H / 2;
    view_lat = lat;
    radius_km = radius_at(zoom);
    ring_update();
}

// Make frames[] match the wanted times, reusing buffers of frames we already have
static void plan_frames(time_t latest)
{
    time_t want[NFRAMES];
    want[NFRAMES - 1] = latest;
    time_t a = (latest / STEP_S) * STEP_S;          // history on a fixed 12-min grid
    if (a == latest) a -= STEP_S;
    for (int k = NFRAMES - 2; k >= 0; k--) { want[k] = a; a -= STEP_S; }

    static EXT_RAM_BSS_ATTR frame_t old[NFRAMES], nw[NFRAMES];   // ~15 KB each: off the task stack, in PSRAM
    memcpy(old, frames, sizeof(old));
    bool used[NFRAMES] = {0};
    memset(nw, 0, sizeof(nw));
    for (int i = 0; i < NFRAMES; i++) {
        for (int j = 0; j < NFRAMES; j++) {
            if (!used[j] && old[j].ok && old[j].t == want[i]) { nw[i] = old[j]; used[j] = true; break; }
        }
    }
    for (int i = 0; i < NFRAMES; i++) {             // hand spare buffers to the missing frames
        if (nw[i].ok) continue;
        for (int j = 0; j < NFRAMES; j++) {
            if (!used[j]) { nw[i].idx = old[j].idx; used[j] = true; break; }
        }
        nw[i].t = want[i];
        nw[i].ok = false;
    }
    // Playback keeps running: it reads frames[] under the display lock and skips frames that aren't ok,
    // and the buffers handed to missing frames belonged to frames that are no longer in the list.
    display_lock(-1);
    memcpy(frames, nw, sizeof(frames));
    display_unlock();
}

// Fill frame i (not visible to playback until ok is set)
static bool load_frame(int i)
{
    static EXT_RAM_BSS_ATTR frame_t tmp;         // ~1 KB, off the stack, in PSRAM
    tmp = frames[i];
    if (!fetch_frame(tmp.t, &tmp)) return false;
    display_lock(-1);
    tmp.ok = true;
    frames[i] = tmp;
    display_unlock();
    return true;
}

/* Past frames, pipelined: this task downloads frame after frame on the one GeoMet connection while a decode task
 * inflates the previous ones (466x466 RGBA PNGs: ~140 ms each, radar and lightning; a quiet-day loop took 590 ms a
 * frame, half of it decoding, October 6). Two slots: at most two frames wait, their PNGs in PSRAM. The decode task's
 * stack is in PSRAM (no internal RAM; it must never write flash: it only decodes, sets the frame under the display
 * lock and updates the status). It runs on this task's core below it, while a download waits on the network: on
 * core 1 the downloads slowed more (decoding streams 868 KB an image through PSRAM, which the TLS buffers share).
 * Measured, 14 past frames on a quiet day: 8.3 s one after the other; 7.4 s skipping empty lightning images (A);
 * 6.0 s pipelined on core 1, 4.5-5.6 s on core 0. */
#define PIPE_SLOTS 2
typedef struct { int i; time_t t; frame_t f; dl_t radar, ltg; bool radar_ok, ltg_ok; char ltg_iso[24]; } pipe_slot_t;
static EXT_RAM_BSS_ATTR pipe_slot_t slots[PIPE_SLOTS];
static QueueHandle_t pipe_full, pipe_free;      // slot numbers
static volatile bool pipe_abort;               // drop what is queued (the view changed)
static volatile int64_t pipe_dec_us;           // time spent decoding (log)

static void decode_task(void *arg)
{
    int s;
    for (;;) {
        xQueueReceive(pipe_full, &s, portMAX_DELAY);
        pipe_slot_t *p = &slots[s];
        bool ok = false;
        int64_t t0 = esp_timer_get_time();
        if (!pipe_abort && p->radar_ok && dec_radar(p->t, &p->f, &p->radar)) {
            dec_lightning(&p->f, &p->ltg, p->ltg_ok, p->ltg_iso);
            ok = true;
        }
        pipe_dec_us += esp_timer_get_time() - t0;
        free(p->radar.buf); p->radar.buf = NULL;   // (the decoders free and clear what they used)
        free(p->ltg.buf); p->ltg.buf = NULL;
        if (ok && !pipe_abort) {
            display_lock(-1);
            p->f.ok = true;
            frames[p->i] = p->f;
            display_unlock();
            if (play_pending) {
                char msg[40];
                snprintf(msg, sizeof(msg), tr(T_LOADING_PAST), frames_ready(), NFRAMES);
                set_status(NULL, msg);
            }
        }
        xQueueSend(pipe_free, &s, portMAX_DELAY);
    }
}

// The missing past frames, newest first, while the radar is wanted and the view stays the same. Returns when every
// frame queued has been decoded (or dropped): nothing else uses the connection or frames[] meanwhile.
static void pipe_load(void)
{
    if (!pipe_full) {
        pipe_full = xQueueCreate(PIPE_SLOTS, sizeof(int));
        pipe_free = xQueueCreate(PIPE_SLOTS, sizeof(int));
        for (int s = 0; s < PIPE_SLOTS; s++) xQueueSend(pipe_free, &s, 0);
        // Core 0 below this task (3): it decodes while a download waits; 6 KB of stack in PSRAM
        if (xTaskCreatePinnedToCoreWithCaps(decode_task, "radar_dec", 6144, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGW(TAG, "No decode task: past frames load one after the other");
            vQueueDelete(pipe_full); vQueueDelete(pipe_free); pipe_full = pipe_free = NULL;
        }
    }
    int64_t t0 = esp_timer_get_time(), dl_us = 0;
    int n = 0;
    pipe_dec_us = 0;
    for (int i = NFRAMES - 2; i >= 0; i--) {
        if (!(visible || play_pending) || relocate_pending || zoom_target != zoom) break;
        if (frames[i].ok) continue;
        if (!pipe_full) {                            // no decode task: one frame after the other, as before
            n += load_frame(i);
            if (play_pending) {
                char msg[40];
                snprintf(msg, sizeof(msg), tr(T_LOADING_PAST), frames_ready(), NFRAMES);
                set_status(NULL, msg);
            }
            continue;
        }
        int s;
        xQueueReceive(pipe_free, &s, portMAX_DELAY);   // a free slot: the decoder is at most two frames behind
        pipe_slot_t *p = &slots[s];
        p->i = i;
        p->t = frames[i].t;
        p->f = frames[i];
        int64_t td = esp_timer_get_time();
        p->radar_ok = dl_radar(p->t, &p->radar);
        p->ltg_ok = p->radar_ok && dl_lightning(p->t, &p->ltg, p->ltg_iso, sizeof(p->ltg_iso));
        dl_us += esp_timer_get_time() - td;
        if (!p->ltg_ok) p->ltg.buf = NULL;
        xQueueSend(pipe_full, &s, portMAX_DELAY);
        n++;
    }
    if (pipe_full) {
        if (relocate_pending || zoom_target != zoom) pipe_abort = true;   // the queued frames are for the old view
        int s[PIPE_SLOTS];
        for (int k = 0; k < PIPE_SLOTS; k++) xQueueReceive(pipe_free, &s[k], portMAX_DELAY);   // decoder done
        for (int k = 0; k < PIPE_SLOTS; k++) xQueueSend(pipe_free, &s[k], 0);
        pipe_abort = false;
    }
    if (n) ESP_LOGI(TAG, "Past frames: %d loaded in %d ms (%d of %d ready; downloads %d ms, decoding %d ms)", n,
                    (int)((esp_timer_get_time() - t0) / 1000), frames_ready(), NFRAMES, (int)(dl_us / 1000),
                    (int)(pipe_dec_us / 1000));
}

static int out_steps;      // zoom-out levels still to animate when the new map is shown

// Show the new basemap; after a zoom-out, shrink it from 2x (or 4x) into place
static void reveal_map(void)
{
    wait_zoom_anim();                           // let a zoom-in animation finish first
    display_lock(-1);
    show_live();
    if (out_steps > 0) {
        int32_t from = 256 << (out_steps > 2 ? 2 : out_steps);
        start_scale_anim(from, LV_SCALE_NONE);
        out_steps = 0;
    }
    display_unlock();
}

// Download every zoom level's basemap that isn't cached yet (boot, or after a location change)
static void preload_all(void)
{
    int cur = zoom, missing[ZOOM_MAX - ZOOM_MIN + 1], n = 0;
    for (int z = ZOOM_MIN; z <= ZOOM_MAX; z++) {
        zoom = z;
        apply_view();
        if (!cache_header_ok()) missing[n++] = z;
    }
    pre_done = 0;
    pre_total = n * 9;
    if (n) {
        ESP_LOGI(TAG, "Preloading %d map level(s) in the background", n);
        preloading = true;
        preload_progress(0);
        display_lock(-1);
        if (lv_obj_has_flag(pnl, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(pnl, LV_OBJ_FLAG_HIDDEN);    // only seen if the radar screen is opened meanwhile
            slide_cache_dirty(scr);
        }
        lv_obj_move_foreground(pnl);
        display_unlock();
        for (int i = 0; i < n && !relocate_pending; i++) {
            zoom = missing[i];
            apply_view();
            if (!load_basemap() && last_tiles_ok == 0 && !relocate_pending) {   // nothing came through: no network, give up
                ESP_LOGW(TAG, "Preload stopped (network?)");
                break;
            }
        }
        preloading = false;
        ESP_LOGI(TAG, "Preload finished");
    }
    zoom = cur;
    apply_view();
    base_ok = cache_load();
    display_lock(-1);
    if (!lv_obj_has_flag(pnl, LV_OBJ_FLAG_HIDDEN)) { lv_obj_add_flag(pnl, LV_OBJ_FLAG_HIDDEN); slide_cache_dirty(scr); }
    show_live();
    display_unlock();
}

static void radar_task(void *arg)
{
    apply_view();
    if (cache_load()) { base_ok = true; display_lock(-1); compose(NULL); refresh_img(); display_unlock(); }
    while (!net_is_connected()) vTaskDelay(pdMS_TO_TICKS(1000));
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));  // main asks for the map preload right after connecting
    bool prefetch = true;                            // latest frame at boot so the first swipe is instant
    while (1) {
        if (preload_req) {
            preload_req = false;
            if (config_active_place() == 0) preload_all();      // other places aren't cached (see cache_save)
            last_fetch = 0;
            prefetch = true;
        }
        if (zoom_target != zoom) relocate_pending = true;
        if (!prefetch && !relocate_pending) {
            ulTaskNotifyTake(pdTRUE, visible ? pdMS_TO_TICKS(REFRESH_S * 1000) : portMAX_DELAY);
        }
        if (zoom_target != zoom) relocate_pending = true;
        if (relocate_pending) {
            relocate_pending = false;
            int prev_zoom = zoom;
            zoom = zoom_target;
            out_steps = prev_zoom > zoom ? prev_zoom - zoom : 0;   // >0 = zoomed out
            apply_view();
            display_lock(-1);
            if (play_timer) { lv_timer_delete(play_timer); play_timer = NULL; lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN); }
            for (int i = 0; i < NFRAMES; i++) frames[i].ok = false;
            display_unlock();
            last_fetch = 0;
            if (loc_changed && preload_on && config_active_place() == 0) {   // home moved or shown again: every zoom level now
                loc_changed = false;
                preload_all();
            } else {
                loc_changed = false;
                base_ok = cache_load();
                if (base_ok) reveal_map();
                // else: keep the old picture on screen until the new map has downloaded
            }
            prefetch = true;
        }
        bool want_work = prefetch || visible || play_pending;
        prefetch = false;
        if (!want_work) continue;
        if (!net_is_connected()) {                       // look again soon: "No Wi-Fi" stayed up to 6 min after it
            set_status(NULL, tr(T_NO_WIFI));             // came back (the next wake was the refresh period)
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
            prefetch = true;
            continue;
        }

        if (!base_ok) {
            base_ok = load_basemap();
            if (!base_ok && (zoom_target != zoom || relocate_pending)) continue;   // superseded: handle the new request
            reveal_map();
        }

        // Latest frame
        if (!last_fetch || time(NULL) - last_fetch >= REFRESH_S - 30) {
            if (!frames[NFRAMES - 1].ok) set_status(NULL, tr(T_LOADING_RADAR));
            time_t latest = latest_radar_time();
            if (!latest) {
                set_status(NULL, tr(T_RADAR_NA));
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));    // a zoom/relocate request wakes us early
                if (visible) xTaskNotifyGive(task);
                continue;
            }
            plan_frames(latest);
            if (!frames[NFRAMES - 1].ok) load_frame(NFRAMES - 1);
            last_fetch = time(NULL);
            display_lock(-1);
            if (!play_timer) show_live();
            if (!play_pending) lv_obj_add_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
            display_unlock();
        }

        // History frames for the animation, newest first (only while the radar is on screen)
        if (visible || play_pending) pipe_load();
        if (play_pending && frames_ready() >= NFRAMES - 2) {
            display_lock(-1);
            if (visible) start_play(); else play_pending = false;
            display_unlock();
        }
    }
}

void radar_units_changed(void)
{
    ring_update();
    display_lock(-1);
    if (pnl_title) lv_label_set_text(pnl_title, tr(T_PREP_MAPS));
    display_unlock();
    display_lock(-1);
    if (!play_timer) show_live();
    display_unlock();
}

lv_obj_t *radar_create(lv_font_t *f_title, lv_font_t *f_small, lv_font_t *f_micro)
{
    cache_mux = xSemaphoreCreateMutex();
    base565 = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    out565 = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    assert(base565 && out565);
    for (int i = 0; i < W * H; i++) base565[i] = out565[i] = rgb565(18, 20, 24);
    for (int i = 0; i < NFRAMES; i++) {
        frames[i].idx = heap_caps_malloc(W * H + 2 * LTG_MAX, MALLOC_CAP_SPIRAM);   // + lightning marks
        assert(frames[i].idx);
    }

    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc.header.w = W;
    dsc.header.h = H;
    dsc.header.stride = W * 2;
    dsc.data_size = W * H * 2;
    dsc.data = (const uint8_t *)out565;

    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_tap, LV_EVENT_CLICKED, NULL);

    img = lv_image_create(scr);
    lv_image_set_src(img, &dsc);
    lv_obj_center(img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_antialias(img, false);        // nearest-neighbour while zoom-animating: much cheaper

    ring = lv_obj_create(scr);
    lv_obj_remove_style_all(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, lv_color_white(), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_30, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    ring_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(ring_lbl, f_micro, 0);
    lv_obj_set_style_text_color(ring_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_opa(ring_lbl, LV_OPA_50, 0);
    lv_label_set_text(ring_lbl, "100 km");

    lv_obj_t *dot = lv_obj_create(scr);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_center(dot);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x5AB0FF), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 2, 0);
    lv_obj_set_style_border_color(dot, lv_color_white(), 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    lbl_title = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_title, f_title, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_white(), 0);
    lv_obj_set_style_bg_color(lbl_title, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lbl_title, LV_OPA_60, 0);
    lv_obj_set_style_radius(lbl_title, 14, 0);
    lv_obj_set_style_pad_hor(lbl_title, 12, 0);
    lv_obj_set_style_pad_ver(lbl_title, 4, 0);
    lv_label_set_text(lbl_title, tr(T_RADAR));
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 30);

    lbl_rtime = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_rtime, f_micro, 0);
    lv_obj_set_style_text_color(lbl_rtime, lv_color_white(), 0);
    lv_obj_set_style_bg_color(lbl_rtime, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lbl_rtime, LV_OPA_50, 0);
    lv_obj_set_style_radius(lbl_rtime, 8, 0);
    lv_obj_set_style_pad_hor(lbl_rtime, 8, 0);
    lv_obj_set_style_pad_ver(lbl_rtime, 2, 0);
    lv_label_set_text(lbl_rtime, tr(T_RADAR));
    lv_obj_align(lbl_rtime, LV_ALIGN_TOP_MID, 0, 72);
    lv_timer_create(radar_clock, 1000, NULL);

    // Playback progress
    bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 110, 5);
    lv_bar_set_range(bar, 0, NFRAMES);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 98);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x5AB0FF), LV_PART_INDICATOR);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);

    lbl_status = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_status, f_small, 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_white(), 0);
    lv_obj_set_style_bg_color(lbl_status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lbl_status, LV_OPA_70, 0);
    lv_obj_set_style_radius(lbl_status, 12, 0);
    lv_obj_set_style_pad_hor(lbl_status, 12, 0);
    lv_obj_set_style_pad_ver(lbl_status, 4, 0);
    lv_label_set_text(lbl_status, "");
    lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, -60);
    lv_obj_add_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *attr = lv_label_create(scr);
    lv_obj_set_style_text_font(attr, f_micro, 0);
    lv_obj_set_style_text_color(attr, lv_color_hex(0xD0D6DC), 0);
    lv_obj_set_style_text_align(attr, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(attr, "© OpenStreetMap contributors\nRadar: ECCC");
    lv_obj_align(attr, LV_ALIGN_BOTTOM_MID, 0, -26);

    // "Preparing maps" panel, shown over the radar while the background preload runs
    pnl = lv_obj_create(scr);
    lv_obj_remove_style_all(pnl);
    lv_obj_set_size(pnl, W, H);
    lv_obj_set_style_bg_color(pnl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pnl, LV_OPA_COVER, 0);
    lv_obj_remove_flag(pnl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(pnl, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *pt = lv_label_create(pnl);
    lv_obj_set_style_text_font(pt, f_title, 0);
    lv_obj_set_style_text_color(pt, lv_color_hex(0x5AB0FF), 0);
    pnl_title = pt;
    lv_label_set_text(pt, tr(T_PREP_MAPS));
    lv_obj_align(pt, LV_ALIGN_TOP_MID, 0, 150);
    pnl_body = lv_label_create(pnl);
    lv_obj_set_style_text_font(pnl_body, f_small, 0);
    lv_obj_set_style_text_color(pnl_body, lv_color_white(), 0);
    lv_obj_set_style_text_align(pnl_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(pnl_body, "");
    lv_obj_align(pnl_body, LV_ALIGN_TOP_MID, 0, 200);
    pnl_bar = lv_bar_create(pnl);
    lv_obj_set_size(pnl_bar, 240, 10);
    lv_obj_align(pnl_bar, LV_ALIGN_TOP_MID, 0, 318);
    lv_obj_set_style_bg_color(pnl_bar, lv_color_hex(0x2A3138), 0);
    lv_obj_set_style_bg_opa(pnl_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(pnl_bar, lv_color_hex(0x5AB0FF), LV_PART_INDICATOR);
    lv_obj_remove_flag(pnl_bar, LV_OBJ_FLAG_CLICKABLE);

    xTaskCreatePinnedToCore(radar_task, "radar", 10240, NULL, 3, &task, 0);
    return scr;
}

void radar_set_visible(bool v)
{
    visible = v;
    if (!v) {
        display_lock(-1);
        play_pending = false;
        if (play_timer) stop_play();
        display_unlock();
    }
    if (v && task) xTaskNotifyGive(task);
}

static void hide_status_cb(lv_timer_t *t) { lv_obj_add_flag(lbl_status, LV_OBJ_FLAG_HIDDEN); }

// Called from the LVGL task (gesture): +1 = zoom in (smaller radius), -1 = zoom out
void radar_zoom(int step)
{
    int z = zoom_target + step;
    if (preloading) {
        lv_label_set_text(lbl_status, tr(T_MAPS_LOADING));
        lv_obj_remove_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(lbl_status);
        lv_timer_t *t = lv_timer_create(hide_status_cb, 1500, NULL);
        lv_timer_set_repeat_count(t, 1);
        return;
    }
    if (z < ZOOM_MIN || z > ZOOM_MAX) {
        lv_label_set_text(lbl_status, tr(step > 0 ? T_ZOOM_CLOSEST : T_ZOOM_WIDEST));
        lv_obj_remove_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
        lv_timer_t *t = lv_timer_create(hide_status_cb, 1500, NULL);
        lv_timer_set_repeat_count(t, 1);
        return;
    }
    zoom_target = z;
    if (play_timer) stop_play();
    play_pending = false;
    // Zoom in: grow the current picture 2x now, the sharper map replaces it afterwards.
    // Zoom out: the task loads the wider (cached) map first and shrinks it into place, so no borders show.
    if (step > 0) {
        int32_t to = lv_image_get_scale(img) * 2;
        start_scale_anim(lv_image_get_scale(img), to > 1024 ? 1024 : to);
    } else if (lv_image_get_scale(img) > LV_SCALE_NONE) {
        start_scale_anim(lv_image_get_scale(img), lv_image_get_scale(img) / 2);   // undo a pending zoom-in preview
    }
    if (config_miles())                                  // as the rest of the radar (it said km in miles mode)
        lv_label_set_text_fmt(lbl_status, "%s  ·  %d mi", tr(step > 0 ? T_ZOOM_IN : T_ZOOM_OUT),
                              (int)(radius_at(z) / 1.609344 + 0.5));
    else lv_label_set_text_fmt(lbl_status, "%s  ·  %d km", tr(step > 0 ? T_ZOOM_IN : T_ZOOM_OUT), radius_at(z));
    lv_obj_remove_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
    if (task) xTaskNotifyGive(task);
}

void radar_preload_start(void)
{
    preload_on = true;
    preload_req = true;
    if (task) xTaskNotifyGive(task);
}

void radar_relocate(void)
{
    loc_changed = true;
    relocate_pending = true;
    if (task) xTaskNotifyGive(task);
}
