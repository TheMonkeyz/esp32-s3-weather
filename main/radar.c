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
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "libs/lodepng/lodepng.h"
#include "display.h"
#include "net.h"
#include "config.h"

static const char *TAG = "radar";

#define W        DISP_W
#define H        DISP_H
#define ZOOM     7
#define REFRESH_S  (6 * 60)
#define MERC_MAX 20037508.342789244

static uint16_t *base565;   // cached basemap
static uint16_t *out565;    // basemap + radar, shown on screen
static lv_image_dsc_t dsc;
static lv_obj_t *scr, *img, *lbl_title, *lbl_status, *ring, *ring_lbl, *bar, *lbl_rtime;
static TaskHandle_t task;
static volatile bool visible;
static bool base_ok;
static time_t last_fetch;
static volatile bool relocate_pending;
static esp_http_client_handle_t geo_h;   // keep-alive connection to GeoMet
static double view_x, view_y;       // top-left of view in world pixels at ZOOM

/* ---------------- HTTP download into a growing PSRAM buffer ---------------- */

typedef struct { uint8_t *buf; size_t len, cap; } dl_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    dl_t *d = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (d->len + e->data_len + 1 > d->cap) {
        size_t nc = (d->cap ? d->cap * 2 : 65536);
        while (nc < d->len + e->data_len + 1) nc *= 2;
        uint8_t *nb = heap_caps_realloc(d->buf, nc, MALLOC_CAP_SPIRAM);
        if (!nb) return ESP_FAIL;
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
            .user_agent = "QuebecWeatherDisplay/1.0 (ESP32 hobby device; personal use)", .buffer_size = 4096,
            .keep_alive_enable = true,
        };
        *hp = esp_http_client_init(&c);
    } else {
        esp_http_client_set_url(*hp, url);
    }
    esp_http_client_set_user_data(*hp, d);
    esp_err_t err = esp_http_client_perform(*hp);
    int st = esp_http_client_get_status_code(*hp);
    if (err != ESP_OK) {                       // drop the connection, start fresh next time
        esp_http_client_cleanup(*hp);
        *hp = NULL;
    }
    if (err != ESP_OK || st != 200 || d->len == 0) {
        ESP_LOGW(TAG, "GET failed (%s, %d): %.90s", esp_err_to_name(err), st, url);
        free(d->buf); d->buf = NULL;
        return false;
    }
    return true;
}


/* ---------------- basemap cache in flash ("mapcache" partition) ---------------- */

typedef struct { uint32_t magic; int32_t zoom, x, y; } cache_hdr_t;
#define CACHE_MAGIC 0x4D415034  // "MAP4" (OSM basemap, chunked write)

static const esp_partition_t *cache_part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "mapcache");
}

static bool cache_load(void)
{
    const esp_partition_t *p = cache_part();
    cache_hdr_t h;
    if (!p || esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK) return false;
    if (h.magic != CACHE_MAGIC || h.zoom != ZOOM || h.x != (int)view_x || h.y != (int)view_y) return false;
    return esp_partition_read(p, sizeof(h), base565, W * H * 2) == ESP_OK;
}

static void cache_save(void)
{
    const esp_partition_t *p = cache_part();
    if (!p) return;
    const size_t SECT = 4096;
    size_t len = (sizeof(cache_hdr_t) + W * H * 2 + SECT - 1) & ~(SECT - 1);
    // Erase and write one sector at a time with a short pause in between, so the idle task
    // (and the task watchdog) get to run: a single 450 KB erase blocks this core for seconds.
    for (size_t off = 0; off < len; off += SECT) {
        if (esp_partition_erase_range(p, off, SECT) != ESP_OK) return;
        vTaskDelay(1);
    }
    const uint8_t *src = (const uint8_t *)base565;
    size_t total = W * H * 2;
    for (size_t off = 0; off < total; off += SECT) {
        size_t n = total - off < SECT ? total - off : SECT;
        if (esp_partition_write(p, sizeof(cache_hdr_t) + off, src + off, n) != ESP_OK) return;
        vTaskDelay(1);
    }
    cache_hdr_t h = { CACHE_MAGIC, ZOOM, (int)view_x, (int)view_y };   // header last = valid only when complete
    if (esp_partition_write(p, 0, &h, sizeof(h)) == ESP_OK) ESP_LOGI(TAG, "Basemap cached to flash");
}

static void set_status(const char *title, const char *status);

/* ---------------- image helpers ---------------- */

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// LVGL's bundled lodepng returns an lv_draw_buf_t* (not raw pixels). Pixel bytes are R,G,B,A.
static lv_draw_buf_t *decode_png(const uint8_t *png, size_t len, unsigned *w, unsigned *h)
{
    unsigned char *out = NULL;
    unsigned e = lodepng_decode32(&out, w, h, png, len);
    if (e || !out) {
        ESP_LOGW(TAG, "PNG decode error %u: %s", e, lodepng_error_text(e));
        if (out) lv_draw_buf_destroy((lv_draw_buf_t *)out);
        return NULL;
    }
    return (lv_draw_buf_t *)out;
}

// OSM's standard style is bright; desaturate half-way and dim it for a dark AMOLED-friendly map
static uint16_t dim_map(uint8_t r, uint8_t g, uint8_t b)
{
    int l = (77 * r + 150 * g + 29 * b) >> 8;
    int R = ((l + r) / 2) * 55 / 100, G = ((l + g) / 2) * 55 / 100, B = ((l + b) / 2) * 55 / 100;
    return rgb565(R, G, B);
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
            snprintf(msg, sizeof(msg), "Loading map %d/%d", ++n, total);
            set_status(NULL, msg);
            snprintf(url, sizeof(url), "https://tile.openstreetmap.org/%d/%d/%d.png", ZOOM, tx, ty);
            dl_t d;
            bool got = false;
            for (int attempt = 0; attempt < 3 && !got; attempt++) got = http_fetch(&h, url, &d);
            if (!got) continue;
            unsigned tw, th;
            lv_draw_buf_t *db = decode_png(d.buf, d.len, &tw, &th);
            free(d.buf);
            if (!db) continue;
            const uint8_t *px = db->data;
            uint32_t stride = db->header.stride;
            int ox = tx * 256 - (int)view_x, oy = ty * 256 - (int)view_y;
            for (int y = 0; y < (int)th; y++) {
                if ((y & 63) == 63) vTaskDelay(1);
                int sy = oy + y;
                if (sy < 0 || sy >= H) continue;
                for (int x = 0; x < (int)tw; x++) {
                    int sx = ox + x;
                    if (sx < 0 || sx >= W) continue;
                    const uint8_t *p = px + y * stride + x * 4;
                    base565[sy * W + sx] = dim_map(p[0], p[1], p[2]);
                }
            }
            lv_draw_buf_destroy(db);
            ok++;
        }
    }
    if (h) esp_http_client_cleanup(h);
    ESP_LOGI(TAG, "Basemap: %d/%d tiles", ok, total);
    if (ok == total) cache_save();
    return ok == total;
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
    if (config_local_time(t, &tm)) strftime(out, n, "%H:%M", &tm);
    else out[0] = 0;
}

/* ---------------- radar frames (palette-indexed, 1 byte/pixel) ---------------- */

#define NFRAMES   15
#define STEP_S    (12 * 60)

typedef struct {
    time_t t;
    bool ok;
    uint8_t *idx;           // W*H palette indices, 0 = no echo
    uint8_t r[256], g[256], b[256], a[256];
} frame_t;

static frame_t frames[NFRAMES];     // [NFRAMES-1] is the latest (live) frame
static lv_timer_t *play_timer;
static int play_i;
static volatile bool play_pending;

// Latest available radar time from GetCapabilities (<Dimension name="time" ...>start/end/PT6M<)
static time_t latest_radar_time(void)
{
    dl_t d;
    if (!http_fetch(&geo_h, "https://geo.weather.gc.ca/geomet?service=WMS&version=1.3.0&request=GetCapabilities&layer=RADAR_1KM_RRAI", &d))
        return 0;
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

static bool fetch_frame(time_t t, frame_t *f)
{
    double res = 2 * MERC_MAX / (256.0 * (1 << ZOOM));
    double minx = view_x * res - MERC_MAX, maxx = (view_x + W) * res - MERC_MAX;
    double maxy = MERC_MAX - view_y * res, miny = MERC_MAX - (view_y + H) * res;
    char iso[24];
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
    char url[420];
    snprintf(url, sizeof(url),
             "https://geo.weather.gc.ca/geomet?service=WMS&version=1.3.0&request=GetMap"
             "&layers=RADAR_1KM_RRAI&styles=&crs=EPSG:3857&bbox=%.1f,%.1f,%.1f,%.1f"
             "&width=%d&height=%d&format=image/png&transparent=true&time=%s",
             minx, miny, maxx, maxy, W, H, iso);
    dl_t d;
    bool got = false;
    for (int attempt = 0; attempt < 2 && !got; attempt++) got = http_fetch(&geo_h, url, &d);
    if (!got) return false;
    unsigned rw, rh;
    lv_draw_buf_t *db = decode_png(d.buf, d.len, &rw, &rh);
    if (!db) { ESP_LOGW(TAG, "Radar response: %.80s", (char *)d.buf); free(d.buf); return false; }
    free(d.buf);
    if ((int)rw != W || (int)rh != H) { lv_draw_buf_destroy(db); return false; }

    // Palette-index the RGBA image (radar styles use a handful of colours)
    static uint32_t key[256];
    int npal = 1, echoes = 0;
    uint32_t lastk = 0xFFFFFFFF; int lasti = 0;
    const uint8_t *px = db->data;
    uint32_t stride = db->header.stride;
    for (int y = 0; y < H; y++) {
        if ((y & 31) == 31) vTaskDelay(1);        // let other tasks / idle run
        const uint8_t *row = px + y * stride;
        uint8_t *o = f->idx + y * W;
        for (int x = 0; x < W; x++) {
            const uint8_t *p = row + x * 4;
            if (p[3] == 0) { o[x] = 0; continue; }
            echoes++;
            uint32_t k = ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
            if (k != lastk) {
                int i;
                for (i = 1; i < npal && key[i] != k; i++) {}
                if (i == npal) {
                    if (npal < 256) {
                        key[npal] = k; f->r[npal] = p[0]; f->g[npal] = p[1]; f->b[npal] = p[2];
                        f->a[npal] = p[3] * 220 / 255;   // keep a hint of the map under the rain
                        npal++;
                    } else i = npal - 1;
                }
                lastk = k; lasti = i;
            }
            o[x] = lasti;
        }
    }
    lv_draw_buf_destroy(db);
    f->t = t;
    ESP_LOGI(TAG, "Frame %s: %d px with echoes, %d colours", iso, echoes, npal - 1);
    return true;
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
}

static void refresh_img(void)
{
    lv_image_cache_drop(&dsc);
    lv_obj_invalidate(img);
}

static void set_status(const char *title, const char *status)
{
    display_lock(-1);
    if (title) lv_label_set_text(lbl_title, title);
    if (status) {
        lv_label_set_text(lbl_status, status);
        if (status[0]) lv_obj_remove_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
    }
    display_unlock();
}

static void show_live(void)      // caller holds the display lock
{
    frame_t *f = &frames[NFRAMES - 1];
    compose(f->ok ? f : NULL);
    char when[8], now[8], sub[32];
    fmt_local(time(NULL), now, sizeof(now));
    lv_label_set_text(lbl_title, now[0] ? now : "Radar");
    if (f->ok) { fmt_local(f->t, when, sizeof(when)); snprintf(sub, sizeof(sub), "Radar %s  ·  tap to play", when); }
    else snprintf(sub, sizeof(sub), "Radar");
    lv_label_set_text(lbl_rtime, sub);
    refresh_img();
}

static void radar_clock(lv_timer_t *t)
{
    if (play_timer) return;                      // the pill shows frame times while playing
    char now[8];
    fmt_local(time(NULL), now, sizeof(now));
    if (now[0] && strcmp(now, lv_label_get_text(lbl_title))) lv_label_set_text(lbl_title, now);
}

/* ---------------- playback (runs in the LVGL task) ---------------- */

static void stop_play(void)
{
    if (play_timer) { lv_timer_delete(play_timer); play_timer = NULL; }
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    show_live();
}

static void play_step(lv_timer_t *t)
{
    while (play_i < NFRAMES && !frames[play_i].ok) play_i++;
    if (play_i >= NFRAMES + 3) { stop_play(); return; }       // hold the last frame ~1 s
    if (play_i < NFRAMES) {
        frame_t *f = &frames[play_i];
        compose(f);
        char when[8];
        fmt_local(f->t, when, sizeof(when));
        lv_label_set_text(lbl_title, when);
        lv_label_set_text(lbl_rtime, "Past 3 hours");
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
    snprintf(msg, sizeof(msg), "Loading past 3 h... %d/%d", frames_ready(), NFRAMES);
    lv_label_set_text(lbl_status, msg);
    lv_obj_remove_flag(lbl_status, LV_OBJ_FLAG_HIDDEN);
    if (task) xTaskNotifyGive(task);
}

/* ---------------- view / location ---------------- */

static void apply_view(void)
{
    location_t loc;
    config_get_location(&loc);
    double lat = loc.lat * M_PI / 180.0;
    double n = 256.0 * (1 << ZOOM);
    view_x = floor((loc.lon + 180.0) / 360.0 * n) - W / 2;
    view_y = floor((1.0 - log(tan(lat) + 1.0 / cos(lat)) / M_PI) / 2.0 * n) - H / 2;
    double m_per_px = 2 * MERC_MAX / n * cos(lat);
    int ring_r = (int)(100000.0 / m_per_px);          // 100 km ring
    display_lock(-1);
    lv_obj_set_size(ring, ring_r * 2, ring_r * 2);
    lv_obj_center(ring);
    lv_obj_align(ring_lbl, LV_ALIGN_CENTER, 0, ring_r + 12);
    display_unlock();
}

// Make frames[] match the wanted times, reusing buffers of frames we already have
static void plan_frames(time_t latest)
{
    time_t want[NFRAMES];
    want[NFRAMES - 1] = latest;
    time_t a = (latest / STEP_S) * STEP_S;          // history on a fixed 12-min grid
    if (a == latest) a -= STEP_S;
    for (int k = NFRAMES - 2; k >= 0; k--) { want[k] = a; a -= STEP_S; }

    static frame_t old[NFRAMES], nw[NFRAMES];     // ~15 KB each: keep off the task stack
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
    display_lock(-1);
    if (play_timer) { lv_timer_delete(play_timer); play_timer = NULL; lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN); }
    memcpy(frames, nw, sizeof(frames));
    display_unlock();
}

// Fill frame i (not visible to playback until ok is set)
static bool load_frame(int i)
{
    static frame_t tmp;                          // ~1 KB, keep off the stack
    tmp = frames[i];
    if (!fetch_frame(tmp.t, &tmp)) return false;
    display_lock(-1);
    tmp.ok = true;
    frames[i] = tmp;
    display_unlock();
    return true;
}

static void radar_task(void *arg)
{
    apply_view();
    if (cache_load()) { base_ok = true; display_lock(-1); compose(NULL); refresh_img(); display_unlock(); }
    while (!net_is_connected()) vTaskDelay(pdMS_TO_TICKS(1000));
    vTaskDelay(pdMS_TO_TICKS(3000));                // let the weather fetch go first
    bool prefetch = true;                            // latest frame at boot so the first swipe is instant
    while (1) {
        if (!prefetch && !relocate_pending) {
            ulTaskNotifyTake(pdTRUE, visible ? pdMS_TO_TICKS(REFRESH_S * 1000) : portMAX_DELAY);
        }
        if (relocate_pending) {
            relocate_pending = false;
            apply_view();
            display_lock(-1);
            if (play_timer) { lv_timer_delete(play_timer); play_timer = NULL; lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN); }
            for (int i = 0; i < NFRAMES; i++) frames[i].ok = false;
            display_unlock();
            last_fetch = 0;
            base_ok = cache_load();
            display_lock(-1); compose(NULL); refresh_img(); display_unlock();
            prefetch = true;
        }
        bool want_work = prefetch || visible || play_pending;
        prefetch = false;
        if (!want_work) continue;
        if (!net_is_connected()) { set_status(NULL, "No Wi-Fi"); continue; }

        if (!base_ok) {
            base_ok = load_basemap();
            display_lock(-1); show_live(); display_unlock();
        }

        // Latest frame
        if (!last_fetch || time(NULL) - last_fetch >= REFRESH_S - 30) {
            if (!frames[NFRAMES - 1].ok) set_status(NULL, "Loading radar...");
            time_t latest = latest_radar_time();
            if (!latest) {
                set_status(NULL, "Radar unavailable");
                vTaskDelay(pdMS_TO_TICKS(20000));
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
        for (int i = NFRAMES - 2; i >= 0 && (visible || play_pending) && !relocate_pending; i--) {
            if (frames[i].ok) continue;
            load_frame(i);
            if (play_pending) {
                char msg[40];
                snprintf(msg, sizeof(msg), "Loading past 3 h... %d/%d", frames_ready(), NFRAMES);
                set_status(NULL, msg);
            }
        }
        if (play_pending && frames_ready() >= NFRAMES - 2) {
            display_lock(-1);
            if (visible) start_play(); else play_pending = false;
            display_unlock();
        }
    }
}

lv_obj_t *radar_create(lv_font_t *f_title, lv_font_t *f_small, lv_font_t *f_micro)
{
    base565 = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    out565 = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    assert(base565 && out565);
    for (int i = 0; i < W * H; i++) base565[i] = out565[i] = rgb565(18, 20, 24);
    for (int i = 0; i < NFRAMES; i++) {
        frames[i].idx = heap_caps_malloc(W * H, MALLOC_CAP_SPIRAM);
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
    lv_label_set_text(lbl_title, "Radar");
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 30);

    lbl_rtime = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_rtime, f_micro, 0);
    lv_obj_set_style_text_color(lbl_rtime, lv_color_white(), 0);
    lv_obj_set_style_bg_color(lbl_rtime, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lbl_rtime, LV_OPA_50, 0);
    lv_obj_set_style_radius(lbl_rtime, 8, 0);
    lv_obj_set_style_pad_hor(lbl_rtime, 8, 0);
    lv_obj_set_style_pad_ver(lbl_rtime, 2, 0);
    lv_label_set_text(lbl_rtime, "Radar");
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

    xTaskCreatePinnedToCore(radar_task, "radar", 16384, NULL, 3, &task, 0);
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

void radar_relocate(void)
{
    relocate_pending = true;
    if (task) xTaskNotifyGive(task);
}
