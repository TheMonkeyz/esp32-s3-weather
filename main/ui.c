// Round-screen weather UI (LVGL v9 + TinyTTF Montserrat)
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include "display.h"
#include "weather.h"
#include "touch.h"
#include "radar.h"
#include "config.h"
#include "net.h"
#include "alerts.h"
#include "ota.h"
#include "svc.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "pager.h"
#include "i18n.h"
#include "sound.h"
#include "presence.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"

extern const uint8_t ttf_start[] asm("_binary_montserrat_ttf_start");
extern const uint8_t ttf_end[]   asm("_binary_montserrat_ttf_end");
extern const uint8_t syl_start[] asm("_binary_syllabics_ttf_start");   // Inuktitut syllabics (Noto subset)
extern const uint8_t syl_end[]   asm("_binary_syllabics_ttf_end");

static lv_font_t *f_time, *f_city, *f_big, *f_cond, *f_small, *f_tiny, *f_micro;
static lv_obj_t *scr_radar, *scr_extras, *scr_status, *scr_update, *up_pill, *up_pill_lbl;
static void update_show(void);
static void extras_refresh(void);
static void status_refresh(void);
static lv_obj_t *scr_msg, *msg_title, *msg_body, *msg_qr;
static lv_obj_t *scr_cfg;                     // settings screen (cfg_create)
static bool back_to_cfg;                      // the phone QR / Wi-Fi setup was opened from it: close back to it
static lv_obj_t *overlay, *ov_qr, *ov_url, *ov_title;
static int ov_state;          // 0 hidden, 1 settings QR
static lv_obj_t *scr_hour;       // hourly detail screen
static int hr_day;
static void hour_fill(int day);
static lv_obj_t *al_pill, *al_pill_lbl, *scr_alert, *al_title, *al_sub, *al_body, *al_map;
static lv_image_dsc_t al_map_dsc;
static uint16_t *al_map_buf;
static EXT_RAM_BSS_ATTR alerts_t alerts;           // ~4 KB, in PSRAM
// Weather screen: one page per place in a vertical pager (pager.c); pills, page dots and place dots stay on top.
typedef struct {
    lv_obj_t *time, *city, *icon, *temp, *cond, *nowcast, *fc_day[3], *fc_temp[3], *fc_icon[3];
    lv_obj_t *detail, *feels, *hum, *wind;      // "Feels 8°  ·  (drop) 86 %  ·  (wind) 8 km/h" (a row)
    char name[48];
    int utc_offset;
    bool has_wx;                               // pw[i] holds this place's forecast
} place_page_t;
static lv_obj_t *scr_main, *place_pager;
static place_page_t pp[MAX_PLACES];
static EXT_RAM_BSS_ATTR weather_t pw[MAX_PLACES];   // each page's forecast (redrawn on a units change)
static int n_places = 1, cur_place;            // pages in use, the place shown (alerts, hourly, extras, radar)
static lv_obj_t *pl_dot[MAX_PLACES];           // which place is shown (right edge of the weather screen)
static void (*place_select_cb)(int i);         // the pager settled on another place (main.c)

#define C_BG      lv_color_hex(0x000000)
#define C_TEXT    lv_color_hex(0xF2F4F7)
#define C_DIM     lv_color_hex(0x8B95A1)
#define C_ACCENT  lv_color_hex(0x5AB0FF)

// Labels whose text is fixed (set once): registered so a language change can re-set them (ui_units_changed)
#define TLABELS 48
static lv_obj_t *tl_obj[TLABELS];
static tid_t tl_id[TLABELS];
static int tl_n;
static void tlabel(lv_obj_t *l, tid_t id)
{
    lv_label_set_text(l, tr(id));
    if (tl_n < TLABELS) { tl_obj[tl_n] = l; tl_id[tl_n++] = id; }
}

static lv_font_t *mkfont(int px)
{
    lv_font_t *f = lv_tiny_ttf_create_data_ex(ttf_start, ttf_end - ttf_start, px, LV_FONT_KERNING_NONE, 96);
    // Montserrat has no syllabics: LVGL looks a missing glyph up in the fallback font
    // a quarter larger: Noto's syllabics are drawn about x-height, they looked small next to Montserrat's capitals
    lv_font_t *s = lv_tiny_ttf_create_data_ex(syl_start, syl_end - syl_start, px * 5 / 4, LV_FONT_KERNING_NONE, 48);
    if (f && s) f->fallback = s;
    return f;
}

static lv_obj_t *label(lv_obj_t *parent, lv_font_t *f, lv_color_t c, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, 400);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *base_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, C_BG, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

/* ---------- simple weather icons built from shapes (scaled by pct) ---------- */

static int S;  // current scale in percent
#define SC(v) ((v) * S / 100)

// Painter mode: when P_layer is set, icons are drawn straight into a layer (no LVGL objects).
// Used by the hourly list, which would otherwise need hundreds of small objects.
static lv_layer_t *P_layer;
static int P_x, P_y;
static lv_color_t icon_bg;     // colour behind the icon (for the moon's cut-out)

static lv_obj_t *blob(lv_obj_t *p, int x, int y, int w, int h, lv_color_t c, int radius)
{
    if (P_layer) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = c;
        d.bg_opa = LV_OPA_COVER;
        d.radius = radius == LV_RADIUS_CIRCLE ? radius : SC(radius);
        lv_area_t a = { P_x + SC(x), P_y + SC(y), P_x + SC(x) + SC(w) - 1, P_y + SC(y) + SC(h) - 1 };
        lv_draw_rect(P_layer, &d, &a);
        return NULL;
    }
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, SC(x), SC(y));
    lv_obj_set_size(o, SC(w), SC(h));
    lv_obj_set_style_radius(o, radius == LV_RADIUS_CIRCLE ? radius : SC(radius), 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    return o;
}

// Let presses on decorative children reach the screen (long-press, swipe)
static void passthrough(lv_obj_t *o)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *c = lv_obj_get_child(o, i);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        passthrough(c);
    }
}

static void sun(lv_obj_t *p, int x, int y, int d)
{
    lv_obj_t *s = blob(p, x, y, d, d, lv_color_hex(0xFFC83D), LV_RADIUS_CIRCLE);
    if (!s) return;
    lv_obj_set_style_shadow_color(s, lv_color_hex(0xFFB000), 0);
    lv_obj_set_style_shadow_width(s, SC(30), 0);
    lv_obj_set_style_shadow_opa(s, LV_OPA_40, 0);
}

static void moon(lv_obj_t *p, int x, int y, int d)
{
    blob(p, x, y, d, d, lv_color_hex(0xE8E6F2), LV_RADIUS_CIRCLE);
    blob(p, x + d / 3, y - d / 8, d, d, icon_bg, LV_RADIUS_CIRCLE);
}

static void cloud(lv_obj_t *p, int x, int y, lv_color_t c)
{
    blob(p, x + 8, y + 22, 48, 48, c, LV_RADIUS_CIRCLE);
    blob(p, x + 34, y, 62, 62, c, LV_RADIUS_CIRCLE);
    blob(p, x, y + 40, 112, 34, c, 17);
}

static lv_point_precise_t bolt_pts[MAX_PLACES * 4][4];   // per icon object: 4 per place page

// Icon design space is 130x122; pct scales it. slot = unique index per icon (for bolt points)
static void draw_icon(lv_obj_t *box, wx_kind_t k, bool day, int pct, int slot)
{
    S = pct;
    if (!P_layer) { lv_obj_clean(box); icon_bg = C_BG; }
    lv_color_t cl = lv_color_hex(0xCFD8E3), dark = lv_color_hex(0x7D8896);
    switch (k) {
    case WX_CLEAR:
        if (day) sun(box, 25, 12, 80); else moon(box, 25, 12, 80);
        break;
    case WX_PARTLY:
        if (day) sun(box, 55, 4, 60); else moon(box, 55, 4, 58);
        cloud(box, 6, 30, cl);
        break;
    case WX_CLOUDY:
        cloud(box, 18, 6, dark);
        cloud(box, 0, 30, cl);
        break;
    case WX_FOG:
        for (int i = 0; i < 4; i++) blob(box, 10 + (i % 2) * 12, 22 + i * 22, 100, 11, cl, 5);
        break;
    case WX_RAIN:
    case WX_STORM:
    case WX_SNOW:
        cloud(box, 9, 4, k == WX_STORM ? dark : cl);
        if (k == WX_SNOW) {
            for (int i = 0; i < 3; i++) blob(box, 30 + i * 26, 88 + (i % 2) * 10, 13, 13, lv_color_white(), LV_RADIUS_CIRCLE);
        } else if (k == WX_RAIN) {
            for (int i = 0; i < 3; i++) blob(box, 34 + i * 24, 86 + (i % 2) * 8, 8, 20, lv_color_hex(0x4DA3FF), 4);
        } else {
            static const int bx[4] = {62, 48, 64, 52}, by[4] = {76, 98, 98, 120};
            if (P_layer) {
                lv_draw_line_dsc_t d;
                lv_draw_line_dsc_init(&d);
                d.width = SC(8) < 3 ? 3 : SC(8);
                d.round_start = d.round_end = 1;
                d.color = lv_color_hex(0xFFD23D);
                for (int i = 0; i < 3; i++) {
                    d.p1.x = P_x + SC(bx[i]);     d.p1.y = P_y + SC(by[i]);
                    d.p2.x = P_x + SC(bx[i + 1]); d.p2.y = P_y + SC(by[i + 1]);
                    lv_draw_line(P_layer, &d);
                }
                break;
            }
            for (int i = 0; i < 4; i++) { bolt_pts[slot][i].x = SC(bx[i]); bolt_pts[slot][i].y = SC(by[i]); }
            lv_obj_t *l = lv_line_create(box);
            lv_line_set_points(l, bolt_pts[slot], 4);
            lv_obj_set_style_line_width(l, SC(8) < 3 ? 3 : SC(8), 0);
            lv_obj_set_style_line_rounded(l, true, 0);
            lv_obj_set_style_line_color(l, lv_color_hex(0xFFD23D), 0);
        }
        break;
    }
}

static lv_obj_t *icon_box_create(lv_obj_t *parent, int pct)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 130 * pct / 100, 124 * pct / 100);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    return b;
}

/* ---------------------------------------------------------------------- */

static char clock_shown[12];

static void clock_tick(lv_timer_t *t)
{
    struct tm tm;
    time_t now = time(NULL);
    if (!config_local_time((long)now, &tm)) return;   // not synced yet (also: the place shown)
    char buf[12];
    config_fmt_time(tm.tm_hour, tm.tm_min, buf, sizeof(buf));
    if (strcmp(buf, clock_shown)) {
        strcpy(clock_shown, buf);
        for (int i = 0; i < n_places; i++) {               // each page in its own time zone
            struct tm lt;
            time_t lt_t = now + pp[i].utc_offset;
            char b[12];
            if (i == cur_place || !pp[i].has_wx) strcpy(b, buf);
            else { gmtime_r(&lt_t, &lt); config_fmt_time(lt.tm_hour, lt.tm_min, b, sizeof(b)); }
            lv_label_set_text(pp[i].time, b);
        }
        ESP_LOGI("ui", "clock %s", buf);
        if (lv_screen_active() == scr_hour && tm.tm_min == 0) hour_fill(0);   // new hour
        if (lv_screen_active() == scr_extras) extras_refresh();
    }
}

/* Settings overlay (long-press on the weather screen) */
static uint32_t overlay_opened;

static void overlay_hide(void)
{
    ov_state = 0;
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

static void overlay_show(void)
{
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
    overlay_opened = lv_tick_get();
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);                  // this touch shouldn't also close it
}

static void overlay_close(lv_event_t *e)
{
    if (lv_tick_elaps(overlay_opened) < 800) return;   // ignore the release of the long-press itself
    ESP_LOGI("ui", "overlay closed");
    overlay_hide();
    if (back_to_cfg) { back_to_cfg = false; lv_screen_load_anim(scr_cfg, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false); }
}

static void show_wifi_setup(lv_event_t *e);

// Step 1: long-press on the weather screen -> settings page QR (home network, HTTPS)
static void show_settings(lv_event_t *e)
{
    if (!net_is_connected()) {                        // offline: the settings QR would be useless
        ESP_LOGI("ui", "long press while offline -> Wi-Fi setup");
        ui_wifi_setup(NULL);
        return;
    }
    ESP_LOGI("ui", "long press -> settings QR");
    char ip[20], url[48];
    if (!net_get_ip(ip, sizeof(ip))) strcpy(ip, "192.168.4.1");
    snprintf(url, sizeof(url), "https://%s", ip);
    lv_label_set_text(ov_title, tr(T_SETTINGS));
    lv_qrcode_update(ov_qr, url, strlen(url));
    lv_label_set_text_fmt(ov_url, tr(T_OV_HELP), url);
    ov_state = 1;
    overlay_show();
}

// Step 2: long-press on the settings QR -> Wi-Fi setup screen
static void show_wifi_setup(lv_event_t *e)
{
    if (ov_state != 1) return;
    ESP_LOGI("ui", "long press -> Wi-Fi setup");
    overlay_hide();
    ui_wifi_setup(NULL);
}

// Long-press on a status screen ("Connecting to...", "Fetching forecast..."): Wi-Fi setup screen
static void msg_long_press(lv_event_t *e)
{
    if (net_in_portal()) return;                      // first-time setup is already showing it
    ESP_LOGI("ui", "long press on status screen -> Wi-Fi setup");
    ui_wifi_setup(NULL);
}

static lv_obj_t *make_qr(lv_obj_t *parent, int size)
{
    lv_obj_t *qr = lv_qrcode_create(parent);
    lv_qrcode_set_size(qr, size);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 6, 0);
    lv_obj_remove_flag(qr, LV_OBJ_FLAG_CLICKABLE);
    return qr;
}

#define N_PAGES 4
static void page_dots(lv_obj_t *scr, int active)   // 0 status, 1 extras, 2 weather, 3 radar
{
    for (int i = 0; i < N_PAGES; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, i == active ? 18 : 7, 7);
        lv_obj_set_style_radius(d, 4, 0);
        lv_obj_set_style_bg_color(d, i == active ? C_TEXT : C_DIM, 0);
        lv_obj_set_style_bg_opa(d, i == active ? LV_OPA_COVER : LV_OPA_60, 0);
        lv_obj_align(d, LV_ALIGN_BOTTOM_MID, (2 * i - (N_PAGES - 1)) * 8 + (i < active ? -5 : i > active ? 5 : 0), -12);
    }
}

/* ---------- Hourly detail (tap a forecast day) ----------
 * Three day pages side by side in a horizontal scroller that snaps one page at a time,
 * so the pages follow the finger. Each page's hour list scrolls vertically and is drawn
 * by one draw callback (no per-row objects). The list starts with the day's temperature graph and the
 * column headers, so they scroll away with it. The graph is drawn once into a canvas per day (about 200 shapes,
 * too many to redraw on every scroll frame) and redrawn only for a new forecast or a new hour. */

#define ROW_H   46
#define LIST_W  316
#define GRAPH_H 120                  // temperature graph at the top of each day's list
#define HDR_H   24                   // column headers under it
#define TOP_H   (GRAPH_H + HDR_H)    // rows start here
typedef struct {
    lv_obj_t *page, *title, *sum, *list, *content, *graph;
    int day;
    int first, count, now;           // hour indexes into wx.hour
    int g_gen, g_now;                // what the graph canvas shows (forecast generation, now)
} day_page_t;
static day_page_t pg[WX_DAYS];
static int wx_gen;                   // bumped by every new forecast
static lv_obj_t *hr_pager, *hr_dot[WX_DAYS];
static EXT_RAM_BSS_ATTR weather_t wx;   // copy of the last forecast (PSRAM)
static bool have_wx;

static void draw_text(lv_layer_t *layer, lv_font_t *f, lv_color_t c, int x, int y, int w,
                      lv_text_align_t align, const char *txt)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = c;
    d.align = align;
    d.text = txt;
    d.text_local = 1;                                // txt lives on the stack
    int lh = lv_font_get_line_height(f);
    lv_area_t a = { x, y + (ROW_H - lh) / 2, x + w - 1, y + (ROW_H + lh) / 2 };
    lv_draw_label(layer, &d, &a);
}

static void draw_text_top(lv_layer_t *layer, lv_font_t *f, lv_color_t c, int x, int y, int w,
                          lv_text_align_t align, const char *txt)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = c;
    d.align = align;
    d.text = txt;
    d.text_local = 1;
    lv_area_t a = { x, y, x + w - 1, y + lv_font_get_line_height(f) - 1 };
    lv_draw_label(layer, &d, &a);
}


// Temperature graph, midnight to midnight (0 to 24, the last point is the next day's 00:00): filled curve, a line
// at every hour, hours labelled every 3 h, the day's high and low labelled.
// Today: the hours already past are dimmed and a dot marks now. (x0, y0) = top-left of the block.
#define G_X0  28                     // plot area inside the block
#define G_W   (LIST_W - 56)
#define G_Y0  26
#define G_H   50
static void graph_draw(lv_layer_t *layer, const day_page_t *dp, int x0, int y0)
{
    int base = dp->day * 24, n = wx.nhours - base;              // points: hours 0..24
    if (n > 25) n = 25;
    if (n < 2) return;
    float lo = wx.hour[base].temp, hi = lo, smin = lo, smax = lo;
    int ilo = 0, ihi = 0;
    for (int i = 1; i < n; i++) {
        float t = wx.hour[base + i].temp;
        if (i < 24 && t < lo) { lo = t; ilo = i; }             // the day's own low and high (00:00 to 23:00)
        if (i < 24 && t > hi) { hi = t; ihi = i; }
        if (t < smin) smin = t;                                 // the scale also fits the next day's 00:00
        if (t > smax) smax = t;
    }
    float span = smax - smin < 4 ? 4 : smax - smin, mid = (smax + smin) / 2;   // a flat day stays flat
    float bot = mid - span / 2;
    int now = dp->now >= 0 ? dp->now - base : -1;              // hour index of "now" (today only)
    #define GX(i) (x0 + G_X0 + (i) * G_W / 24.0f)
    #define GY(t) (y0 + G_Y0 + G_H - ((t) - bot) / span * G_H)
    const lv_color_t warm = lv_color_hex(0xFFC83D);

    // Fill under the curve: 2-px columns, interpolated between the hours. Opaque colours pre-mixed with the black
    // background: same look as a translucent fill, much cheaper to render while the list scrolls.
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    lv_color_t fill = lv_color_mix(warm, C_BG, 60), fill_past = lv_color_mix(warm, C_BG, 24);
    int ybase = y0 + G_Y0 + G_H + 4;
    for (int x = (int)GX(0); x < (int)GX(n - 1); x += 2) {
        float fi = (x - GX(0)) * 24.0f / G_W;
        int i = (int)fi;
        if (i >= n - 1) i = n - 2;
        float t = wx.hour[base + i].temp + (wx.hour[base + i + 1].temp - wx.hour[base + i].temp) * (fi - i);
        r.bg_color = now >= 0 && fi < now ? fill_past : fill;
        lv_area_t a = { x, (int)GY(t), x + 1, ybase };
        lv_draw_rect(layer, &r, &a);
    }

    // A thin line at every hour, brighter every 3 h (the labelled ones)
    for (int h = 0; h <= 24; h++) {
        r.bg_color = lv_color_hex(h % 3 ? 0x1E252D : 0x3A4450);
        int x = (int)GX(h);
        lv_area_t a = { x, y0 + G_Y0 - 4, x, ybase };
        lv_draw_rect(layer, &r, &a);
    }

    // Curve
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.width = 3;
    l.round_start = l.round_end = 1;
    for (int i = 0; i + 1 < n; i++) {
        l.color = now >= 0 && i < now ? C_DIM : warm;
        l.p1.x = GX(i);     l.p1.y = GY(wx.hour[base + i].temp);
        l.p2.x = GX(i + 1); l.p2.y = GY(wx.hour[base + i + 1].temp);
        lv_draw_line(layer, &l);
    }

    // Hours under the plot, every 3 h
    char buf[12];
    units_t un;
    config_get_units(&un);
    for (int h = 0; h <= 24; h += 3) {
        if (un.h12) snprintf(buf, sizeof(buf), "%d%c", (h + 11) % 12 + 1, h % 24 < 12 ? 'a' : 'p');   // 12a 3a .. 12p
        else snprintf(buf, sizeof(buf), "%d", h);
        draw_text_top(layer, f_micro, C_DIM, (int)GX(h) - 15, y0 + G_Y0 + G_H + 22, 30, LV_TEXT_ALIGN_CENTER, buf);
    }

    // High above its point, low below its point
    snprintf(buf, sizeof(buf), "%d°", config_temp(hi));
    draw_text_top(layer, f_tiny, C_TEXT, (int)GX(ihi) - 30, (int)GY(hi) - 24, 60, LV_TEXT_ALIGN_CENTER, buf);
    if (config_temp(lo) != config_temp(hi)) {
        snprintf(buf, sizeof(buf), "%d°", config_temp(lo));
        draw_text_top(layer, f_micro, C_DIM, (int)GX(ilo) - 30, (int)GY(lo) + 4, 60, LV_TEXT_ALIGN_CENTER, buf);
    }

    // Now
    if (now >= 0 && now < n) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_white();
        d.radius = LV_RADIUS_CIRCLE;
        int cx = (int)GX(now), cy = (int)GY(wx.hour[base + now].temp);
        lv_area_t a = { cx - 5, cy - 5, cx + 5, cy + 5 };
        lv_draw_rect(layer, &d, &a);
    }
    #undef GX
    #undef GY
}

// Redraws the day's graph canvas if the forecast or the current hour changed. Display lock held.
static void graph_render(day_page_t *dp)
{
    if (dp->g_gen == wx_gen && dp->g_now == dp->now) return;
    dp->g_gen = wx_gen;
    dp->g_now = dp->now;
    lv_canvas_fill_bg(dp->graph, C_BG, LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(dp->graph, &layer);
    graph_draw(&layer, dp, 0, 0);
    lv_canvas_finish_layer(dp->graph, &layer);
}

// Draws the column headers and only the rows inside the clip area (the graph is a canvas child)
static void hr_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    day_page_t *dp = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(o, &c);
    const lv_area_t *clip = &layer->_clip_area;
    char buf[16];
    if (c.y1 + TOP_H > clip->y1) {
        static const struct { int x, w; tid_t t; } hdr[] = {         // aligned with the columns below
            { 118, 56, T_HDR_TEMP }, { 180, 54, T_HDR_RAIN }, { 236, 72, T_HDR_WIND },
        };
        for (int i = 0; i < 3; i++)
            draw_text_top(layer, f_micro, i == 1 ? C_ACCENT : C_DIM, c.x1 + hdr[i].x, c.y1 + GRAPH_H + 4, hdr[i].w,
                          LV_TEXT_ALIGN_RIGHT, tr(hdr[i].t));
    }
    for (int r = 0; r < dp->count; r++) {
        int y = c.y1 + TOP_H + r * ROW_H;
        if (y + ROW_H <= clip->y1 || y > clip->y2) continue;
        int i = dp->first + r;
        const wx_hour_t *h = &wx.hour[i];
        bool now = i == dp->now;
        icon_bg = C_BG;
        if (now) {
            lv_draw_rect_dsc_t d;
            lv_draw_rect_dsc_init(&d);
            d.bg_color = icon_bg = lv_color_hex(0x16283A);
            d.radius = 14;
            lv_area_t a = { c.x1, y + 2, c.x2, y + ROW_H - 3 };
            lv_draw_rect(layer, &d, &a);
        }
        if (now) strlcpy(buf, tr(T_NOW), sizeof(buf)); else config_fmt_hour(i % 24, buf, sizeof(buf));
        draw_text(layer, f_tiny, now ? C_ACCENT : C_DIM, c.x1 + 10, y, 64, LV_TEXT_ALIGN_LEFT, buf);
        P_layer = layer; P_x = c.x1 + 76; P_y = y + (ROW_H - 37) / 2;
        draw_icon(NULL, weather_kind(h->code), h->is_day, 30, 0);
        P_layer = NULL;
        snprintf(buf, sizeof(buf), "%d°", config_temp(h->temp));
        draw_text(layer, f_small, C_TEXT, c.x1 + 118, y, 56, LV_TEXT_ALIGN_RIGHT, buf);
        snprintf(buf, sizeof(buf), "%d%%", h->pop);
        draw_text(layer, f_tiny, h->pop >= 30 ? C_ACCENT : C_DIM, c.x1 + 180, y, 54, LV_TEXT_ALIGN_RIGHT, buf);
        config_fmt_wind(h->wind, buf, sizeof(buf));
        draw_text(layer, f_micro, C_DIM, c.x1 + 236, y, 72, LV_TEXT_ALIGN_RIGHT, buf);
    }
}

static void set_dots(int day)
{
    hr_day = day;
    for (int i = 0; i < WX_DAYS; i++) {
        lv_obj_set_size(hr_dot[i], i == day ? 18 : 7, 7);
        if (i < wx.ndays) lv_obj_remove_flag(hr_dot[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(hr_dot[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(hr_dot[i], i == day ? C_TEXT : C_DIM, 0);
    }
}

static void fill_page(int day)
{
    day_page_t *dp = &pg[day];
    if (day >= wx.ndays) { lv_obj_add_flag(dp->page, LV_OBJ_FLAG_HIDDEN); dp->count = 0; return; }
    lv_obj_remove_flag(dp->page, LV_OBJ_FLAG_HIDDEN);
    struct tm now;
    int cur_h = config_local_time((long)time(NULL), &now) ? now.tm_hour : 0;
    dp->first = day * 24 + (day == 0 ? cur_h : 0);
    int end = day * 24 + 24;
    if (end > wx.nhours) end = wx.nhours;
    dp->count = end > dp->first ? end - dp->first : 0;
    dp->now = day == 0 ? dp->first : -1;

    const wx_day_t *d = &wx.day[day];
    struct tm tm = {0};
    char name[24] = "-";
    if (sscanf(d->date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
        tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_hour = 12;
        mktime(&tm);
        strlcpy(name, tr_weekday(tm.tm_wday, true), sizeof(name));   // weekday name, today included
    }
    lv_label_set_text(dp->title, name);
    lv_label_set_text_fmt(dp->sum, "%s  ·  %d° / %d°", weather_text(d->code), config_temp(d->tmax), config_temp(d->tmin));
    lv_obj_set_height(dp->content, TOP_H + dp->count * ROW_H);
    graph_render(dp);
    lv_obj_invalidate(dp->page);
}

static void hour_fill(int day)       // refresh one day's page (new data / new hour)
{
    fill_page(day);
}

static void hour_close(void)
{
    lv_screen_load_anim(scr_main, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 260, 0, false);
}

// Tap on a forecast column of the weather screen
static void main_tap(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in || ov_state || !have_wx) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (p.y > 408 && !lv_obj_has_flag(up_pill, LV_OBJ_FLAG_HIDDEN)) {   // the "Update" pill
        update_show();
        return;
    }
    if (p.y < 200 && alerts.n) {                        // top half with an alert: its details
        printf("ui: tap alert\n");
        lv_screen_load_anim(scr_alert, LV_SCR_LOAD_ANIM_MOVE_TOP, 260, 0, false);
        return;
    }
    if (p.y < 296) return;                              // only the forecast row
    int col = p.x < DISP_W / 2 - 49 ? 0 : p.x > DISP_W / 2 + 49 ? 2 : 1;
    if (col >= wx.ndays) return;
    printf("ui: tap forecast day %d\n", col);
    for (int i = 0; i < WX_DAYS; i++) {
        fill_page(i);
        lv_obj_scroll_to_y(pg[i].list, 0, LV_ANIM_OFF);
    }
    pager_go(hr_pager, col, false);
    set_dots(col);
    lv_screen_load_anim(scr_hour, LV_SCR_LOAD_ANIM_MOVE_TOP, 260, 0, false);
}

static void hour_tap(lv_event_t *e)
{
    printf("ui: hourly view closed (tap)\n");
    hour_close();
}

// A swipe that didn't scroll anything (e.g. vertical on a short list): its release is not a tap
static void hour_gesture(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);
}

static void hr_changed(int day, void *user) { set_dots(day); }

static void hr_settled(int day, void *user)
{
    ESP_LOGI("ui", "hourly view: day %d, %d rows", day, pg[day].count);
}

static void hour_create(void)
{
    scr_hour = base_screen();
    hr_pager = pager_create(scr_hour, false, WX_DAYS, hr_changed, hr_settled, NULL);

    for (int d = 0; d < WX_DAYS; d++) {
        day_page_t *dp = &pg[d];
        dp->day = d;
        dp->page = pager_page(hr_pager, d);
        dp->title = label(dp->page, f_city, C_ACCENT, 34);
        dp->sum = label(dp->page, f_tiny, C_DIM, 68);
        lv_obj_set_width(dp->sum, 340);
        lv_label_set_long_mode(dp->sum, LV_LABEL_LONG_DOT);

        dp->list = lv_obj_create(dp->page);                    // graph + headers + hour rows (hr_draw)
        lv_obj_remove_style_all(dp->list);
        lv_obj_set_size(dp->list, LIST_W, 314);
        lv_obj_align(dp->list, LV_ALIGN_TOP_MID, 0, 96);
        lv_obj_set_scroll_dir(dp->list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(dp->list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(dp->list, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

        dp->content = lv_obj_create(dp->list);
        lv_obj_remove_style_all(dp->content);
        lv_obj_set_size(dp->content, LIST_W, TOP_H + ROW_H);
        lv_obj_remove_flag(dp->content, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(dp->content, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(dp->content, hr_draw, LV_EVENT_DRAW_MAIN, dp);

        dp->graph = lv_canvas_create(dp->content);             // ~76 KB each, LVGL heap (PSRAM)
        lv_canvas_set_draw_buf(dp->graph, lv_draw_buf_create(LIST_W, GRAPH_H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO));
        lv_obj_set_pos(dp->graph, 0, 0);
        lv_obj_remove_flag(dp->graph, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(dp->graph, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        dp->g_gen = -1;
    }

    lv_obj_t *dots = lv_obj_create(scr_hour);                 // centred row of page dots
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, 7);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dots, 7, 0);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < WX_DAYS; i++) {
        hr_dot[i] = lv_obj_create(dots);
        lv_obj_remove_style_all(hr_dot[i]);
        lv_obj_set_size(hr_dot[i], 7, 7);
        lv_obj_set_style_radius(hr_dot[i], 4, 0);
        lv_obj_set_style_bg_opa(hr_dot[i], LV_OPA_COVER, 0);
        lv_obj_remove_flag(hr_dot[i], LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_add_event_cb(scr_hour, hour_tap, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(scr_hour, hour_gesture, LV_EVENT_GESTURE, NULL);
}

static void gesture_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(in);
    lv_obj_t *cur = lv_screen_active();
    if (ov_state) return;                                // settings / Wi-Fi setup overlay is open
    LV_LOG_USER("gesture dir %d", dir);
    printf("ui: gesture dir=%d on %s\n", dir, cur == scr_radar ? "radar" : cur == scr_extras ? "extras" :
           cur == scr_status ? "status" : "main");
    if (cur == scr_extras && dir == LV_DIR_RIGHT) {
        svc_probe_stale();
        status_refresh();
        lv_screen_load_anim(scr_status, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 280, 0, false);
        lv_indev_wait_release(in);
    } else if (cur == scr_status && dir == LV_DIR_LEFT) {
        extras_refresh();
        lv_screen_load_anim(scr_extras, LV_SCR_LOAD_ANIM_MOVE_LEFT, 280, 0, false);
        lv_indev_wait_release(in);
    } else if (cur == scr_main && dir == LV_DIR_RIGHT) {
        extras_refresh();
        lv_screen_load_anim(scr_extras, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 280, 0, false);
        lv_indev_wait_release(in);
    } else if (cur == scr_extras && dir == LV_DIR_LEFT) {
        lv_screen_load_anim(scr_main, LV_SCR_LOAD_ANIM_MOVE_LEFT, 280, 0, false);
        lv_indev_wait_release(in);
    } else if (cur == scr_main && dir == LV_DIR_LEFT) {
        radar_set_visible(true);
        lv_screen_load_anim(scr_radar, LV_SCR_LOAD_ANIM_MOVE_LEFT, 280, 0, false);
        lv_indev_wait_release(in);
    } else if (cur == scr_radar && (dir == LV_DIR_TOP || dir == LV_DIR_BOTTOM)) {
        radar_zoom(dir == LV_DIR_BOTTOM ? +1 : -1);  // swipe down = zoom in, up = zoom out
        lv_indev_wait_release(in);
    } else if (cur == scr_radar && dir == LV_DIR_RIGHT) {
        radar_set_visible(false);
        lv_screen_load_anim(scr_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 280, 0, false);
        lv_indev_wait_release(in);
    } else {
        lv_indev_wait_release(in);      // unused swipe: don't let its release open the hourly view
    }
}

/* ---------- Wi-Fi setup screen ----------
 * Page 1 (any phone): QR code to join the display's setup network; the setup page opens by itself.
 * Page 2 (Android 10+): Wi-Fi Easy Connect (DPP). The phone scans this QR code from its Wi-Fi settings and
 * sends the network it's connected to, password included. The radio can't do both at once, so the
 * setup network runs on page 1 and Easy Connect listens on page 2. Swipe to switch. */

static lv_obj_t *scr_setup, *su_title, *su_note, *su_qr, *su_body, *su_dot[2];
static int su_page;
static bool su_can_close;           // a tap closes it (not in first-time setup: there is no saved network)
static volatile bool su_open;
static lv_timer_t *su_timer;
static char su_note_text[96];
static const char su_ap_qr[] = "WIFI:T:WPA;S:" SETUP_AP_SSID ";P:" SETUP_AP_PASS ";;";

static void su_dots(void)
{
    for (int i = 0; i < 2; i++) {
        lv_obj_set_size(su_dot[i], i == su_page ? 18 : 7, 7);
        lv_obj_set_style_bg_color(su_dot[i], i == su_page ? C_TEXT : C_DIM, 0);
    }
}

// Easy Connect callbacks (Wi-Fi task context: take the display lock)
static void su_dpp_uri(const char *uri)
{
    display_lock(-1);
    if (su_page == 1 && lv_screen_active() == scr_setup) {
        lv_qrcode_update(su_qr, uri, strlen(uri));
        lv_obj_remove_flag(su_qr, LV_OBJ_FLAG_HIDDEN);
    }
    display_unlock();
}

static void su_dpp_done(bool ok, const char *ssid)
{
    display_lock(-1);
    if (ok) {
        lv_label_set_text(su_title, tr(T_WIFI_RECEIVED));
        lv_label_set_text_fmt(su_body, tr(T_WIFI_GOT), ssid);
        lv_obj_add_flag(su_qr, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(su_body, tr(T_WIFI_DPP_FAIL));
    }
    display_unlock();
}

static void su_show_page(int page)
{
    su_page = page;
    su_dots();
    if (page == 0) {
        net_dpp_stop();
        net_setup_ap_start();
        lv_label_set_text(su_title, tr(T_WIFI_SETUP));
        lv_qrcode_update(su_qr, su_ap_qr, strlen(su_ap_qr));
        lv_obj_remove_flag(su_qr, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(su_body, tr(T_WIFI_JOIN), SETUP_AP_SSID, SETUP_AP_PASS);
    } else {
        lv_label_set_text(su_title, tr(T_WIFI_DPP_TITLE));
        lv_obj_add_flag(su_qr, LV_OBJ_FLAG_HIDDEN);          // until the code is generated
        lv_label_set_text(su_body, tr(T_WIFI_DPP_HOW));
        net_setup_ap_stop_any();
        if (!net_dpp_start(su_dpp_uri, su_dpp_done))
            lv_label_set_text(su_body, tr(T_WIFI_DPP_NONE));
    }
    ESP_LOGI("ui", "Wi-Fi setup page %d (%s)", page, page ? "Easy Connect" : "setup network");
}

static void su_close(void)
{
    ESP_LOGI("ui", "Wi-Fi setup closed");
    su_open = false;
    if (su_timer) { lv_timer_delete(su_timer); su_timer = NULL; }
    net_dpp_stop();
    net_setup_ap_stop();
    lv_screen_load_anim(back_to_cfg ? scr_cfg : scr_main, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    back_to_cfg = false;
}

// Online: close after 10 min. Offline: close after 5 idle min so the saved network is tried again (setup
// pauses those attempts, and the router may just have been rebooting); main.c reopens setup if it still fails.
static void su_timeout(lv_timer_t *t)
{
    if (!su_can_close) return;                          // first-time setup stays
    if (!net_is_connected() && net_ap_clients() > 0) return;   // a phone is on the setup network
    su_close();
}

static void su_gesture(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(in);
    if (dir == LV_DIR_LEFT && su_page == 0) su_show_page(1);
    else if (dir == LV_DIR_RIGHT && su_page == 1) su_show_page(0);
    lv_indev_wait_release(in);
}

static void su_tap(lv_event_t *e)
{
    if (su_can_close) su_close();
}

static void setup_create(void)
{
    scr_setup = base_screen();
    su_title = label(scr_setup, f_small, C_ACCENT, 40);
    su_note = label(scr_setup, f_tiny, C_DIM, 70);
    lv_obj_set_width(su_note, 330);
    su_qr = make_qr(scr_setup, 150);
    lv_obj_align(su_qr, LV_ALIGN_TOP_MID, 0, 122);
    su_body = label(scr_setup, f_tiny, C_TEXT, 298);
    lv_obj_set_width(su_body, 360);
    for (int i = 0; i < 2; i++) {
        su_dot[i] = lv_obj_create(scr_setup);
        lv_obj_remove_style_all(su_dot[i]);
        lv_obj_set_style_radius(su_dot[i], 4, 0);
        lv_obj_set_style_bg_opa(su_dot[i], LV_OPA_COVER, 0);
        lv_obj_align(su_dot[i], LV_ALIGN_BOTTOM_MID, i == 0 ? -10 : 10, -16);
    }
    lv_obj_add_event_cb(scr_setup, su_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr_setup, su_tap, LV_EVENT_SHORT_CLICKED, NULL);
}

void ui_wifi_setup(const char *note)
{
    display_lock(-1);
    if (note) strlcpy(su_note_text, note, sizeof(su_note_text));
    else su_note_text[0] = 0;
    su_can_close = !net_in_portal();
    bool online = net_is_connected();
    lv_label_set_text(su_note, su_note_text[0] ? su_note_text : !su_can_close ? "" :
                               online ? tr(T_TAP_CANCEL) : tr(T_TAP_RETRY));
    su_open = true;
    su_show_page(0);
    if (su_timer) lv_timer_delete(su_timer);
    su_timer = lv_timer_create(su_timeout, (online ? 10 : 5) * 60 * 1000, NULL);
    if (lv_screen_active() != scr_setup) lv_screen_load(scr_setup);
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);                              // the long-press isn't also a tap
    display_unlock();
}

bool ui_wifi_setup_open(void) { return su_open; }

void ui_wifi_setup_end(void)
{
    display_lock(-1);
    if (su_timer) { lv_timer_delete(su_timer); su_timer = NULL; }
    net_dpp_stop();
    display_unlock();
}

/* ---------- Weather alerts ---------- */

static lv_color_t alert_colour(char c)
{
    return c == 'r' ? lv_color_hex(0xFF4D4D) : c == 'o' ? lv_color_hex(0xFF8A3D) :
           c == 'y' ? lv_color_hex(0xFFC83D) : lv_color_hex(0x8B95A1);
}

static void fmt_until(time_t t, char *out, size_t n)
{
    struct tm tm;
    if (t && config_local_time((long)t, &tm)) {
        char hm[12];
        config_fmt_time(tm.tm_hour, tm.tm_min, hm, sizeof(hm));
        snprintf(out, n, tr(T_UNTIL), tr_weekday(tm.tm_wday, false), hm);
    } else out[0] = 0;
}

static void alert_close(lv_event_t *e) { lv_screen_load_anim(scr_main, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 260, 0, false); }

static void alert_gesture(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);                  // a swipe is not a tap (= close)
}

static lv_obj_t *al_label(lv_obj_t *parent, lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    return l;
}

// Title fixed at the top; when, where and the text scroll together in one column, so a long area name
// wraps instead of running into the text.
static void alert_create(void)
{
    scr_alert = base_screen();
    al_title = label(scr_alert, f_city, C_TEXT, 40);
    lv_obj_set_width(al_title, 300);
    lv_obj_t *box = lv_obj_create(scr_alert);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 310, 340);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_set_style_pad_bottom(box, 90, 0);            // the last lines can scroll above the round edge
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    al_map = lv_image_create(box);                      // region map, when available
    lv_obj_set_style_radius(al_map, 16, 0);
    lv_obj_set_style_clip_corner(al_map, true, 0);
    lv_obj_set_style_margin_bottom(al_map, 8, 0);
    lv_obj_add_flag(al_map, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    al_sub = al_label(box, f_tiny, C_DIM);
    lv_obj_set_style_text_align(al_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(al_sub, 10, 0);
    al_body = al_label(box, f_tiny, C_TEXT);
    lv_obj_add_event_cb(scr_alert, alert_close, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(scr_alert, alert_gesture, LV_EVENT_GESTURE, NULL);
}

void ui_alert_map(uint16_t *buf, int w, int h)
{
    display_lock(-1);
    uint16_t *old = al_map_buf;
    al_map_buf = buf;
    if (buf) {
        memset(&al_map_dsc, 0, sizeof(al_map_dsc));
        al_map_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        al_map_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        al_map_dsc.header.w = w;
        al_map_dsc.header.h = h;
        al_map_dsc.header.stride = w * 2;
        al_map_dsc.data = (const uint8_t *)buf;
        al_map_dsc.data_size = w * h * 2;
        lv_image_cache_drop(&al_map_dsc);
        lv_image_set_src(al_map, &al_map_dsc);
        lv_obj_remove_flag(al_map, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(al_map, LV_OBJ_FLAG_HIDDEN);
    }
    display_unlock();
    free(old);
}

#define AL (i18n_lang() < ALERT_LANGS ? i18n_lang() : 0)    // alert texts exist in English and French

void ui_alerts(const alerts_t *al)
{
    display_lock(-1);
    if (al != &alerts) alerts = *al;
    if (!alerts.n) {
        lv_obj_add_flag(al_map, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(al_pill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(pp[cur_place].city, LV_OBJ_FLAG_HIDDEN);
        if (lv_screen_active() == scr_alert) lv_screen_load(scr_main);
        display_unlock();
        return;
    }
    const alert_t *a = &alerts.a[0];
    lv_color_t c = alert_colour(a->colour);
    lv_obj_set_style_bg_color(al_pill, c, 0);
    lv_obj_set_style_text_color(al_pill_lbl, a->colour == 'r' ? lv_color_white() : lv_color_black(), 0);
    if (alerts.n > 1) lv_label_set_text_fmt(al_pill_lbl, "%s +%d", a->name[AL], alerts.n - 1);
    else lv_label_set_text(al_pill_lbl, a->name[AL]);
    lv_obj_remove_flag(al_pill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(pp[cur_place].city, LV_OBJ_FLAG_HIDDEN);

    char until[32];
    fmt_until(a->ends, until, sizeof(until));
    lv_label_set_text(al_title, a->name[AL]);
    lv_obj_set_style_text_color(al_title, c, 0);
    lv_label_set_text_fmt(al_sub, "%s%s%s", until, until[0] && a->area[AL][0] ? "\n" : "", a->area[AL]);
    lv_obj_scroll_to_y(lv_obj_get_parent(al_body), 0, LV_ANIM_OFF);
    static EXT_RAM_BSS_ATTR char body[ALERTS_MAX * 960];
    int len = snprintf(body, sizeof(body), "%s", a->text[AL]);
    for (int i = 1; i < alerts.n && len < (int)sizeof(body); i++) {
        fmt_until(alerts.a[i].ends, until, sizeof(until));
        len += snprintf(body + len, sizeof(body) - len, tr(T_ALERT_ALSO), alerts.a[i].name[AL], until, alerts.a[i].text[AL]);
    }
    lv_label_set_text(al_body, body);
    display_unlock();
}

/* ---------- Extras page (swipe right from the weather screen) ----------
 * Sun arc (sunrise -> sunset, the sun at the current time), UV index, moon phase, air quality, pollen. */

static lv_obj_t *ex_moon, *ex_date, *ex_arc, *ex_sun, *ex_rise, *ex_set, *ex_day, *ex_val[4], *ex_key[4];
static air_t ex_air = { .us_aqi = -1, .pollen = { -1, -1, -1, -1 } };
static bool have_air;
#define ARC_R   120
#define ARC_CX  (DISP_W / 2)
#define ARC_CY  215

static float moon_k;           // cos(phase angle): 1 = new, -1 = full
static bool moon_waxing;
static int hhmm(const char *s) { int h, m; return s && sscanf(s, "%d:%d", &h, &m) == 2 ? h * 60 + m : -1; }

static const char *moon_phase(time_t t, int *illum)
{
    const double syn = 29.530588853, new_moon = 947182440.0;   // 2000-01-06 18:14 UTC
    double age = fmod((t - new_moon) / 86400.0, syn);
    if (age < 0) age += syn;
    moon_k = cos(2 * M_PI * age / syn);
    moon_waxing = age < syn / 2;
    *illum = (int)round((1 - cos(2 * M_PI * age / syn)) / 2 * 100);
    static const tid_t names[8] = { T_MOON_NEW, T_MOON_WAX_CR, T_MOON_FIRST_Q, T_MOON_WAX_GIB,
                                    T_MOON_FULL, T_MOON_WAN_GIB, T_MOON_LAST_Q, T_MOON_WAN_CR };
    return tr(names[(int)floor(age / syn * 8 + 0.5) % 8]);
}

// Moon drawn row by row: dark disc, then the lit part. k = cos(phase angle) puts the terminator at w*k.
static void moon_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(o, &c);
    int r = lv_area_get_width(&c) / 2, cx = c.x1 + r, cy = c.y1 + r;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(0x2A3138);
    lv_draw_rect(layer, &d, &c);
    d.radius = 0;
    d.bg_color = lv_color_hex(0xE8E6F2);
    for (int y = -r; y < r; y++) {
        float yy = y + 0.5f, w = sqrtf((float)r * r - yy * yy);
        float x0 = moon_waxing ? w * moon_k : -w, x1 = moon_waxing ? w : -w * moon_k;
        if (x1 - x0 < 0.5f) continue;
        lv_area_t a = { cx + (int)roundf(x0), cy + y, cx + (int)roundf(x1) - 1, cy + y };
        lv_draw_rect(layer, &d, &a);
    }
}

static void ex_row(int i, const char *key, const char *val, lv_color_t c)
{
    lv_label_set_text(ex_key[i], key);
    lv_label_set_text(ex_val[i], val);
    lv_obj_set_style_text_color(ex_val[i], c, 0);
}

static void extras_refresh(void)       // display lock held (LVGL task or caller)
{
    struct tm tm;
    time_t now = time(NULL);
    bool synced = config_local_time((long)now, &tm);
    char buf[64];
    if (synced) {                                        // "Wednesday, September 30" / "Mercredi 1er octobre"
        tr_date_long(&tm, buf, sizeof(buf));
        lv_label_set_text(ex_date, buf);
    }

    // Sun
    int rise = have_wx ? hhmm(wx.day[0].sunrise) : -1, set = have_wx ? hhmm(wx.day[0].sunset) : -1;
    int cur = synced ? tm.tm_hour * 60 + tm.tm_min : -1;
    if (rise >= 0 && set > rise) {
        config_fmt_hhmm(wx.day[0].sunrise, buf, sizeof(buf));
        lv_label_set_text(ex_rise, buf);
        config_fmt_hhmm(wx.day[0].sunset, buf, sizeof(buf));
        lv_label_set_text(ex_set, buf);
        int len = set - rise;
        if (cur >= rise && cur <= set) {
            float p = (float)(cur - rise) / len;
            lv_arc_set_value(ex_arc, (int)(p * 1000));
            float th = (180 + 180 * p) * M_PI / 180;
            lv_obj_set_pos(ex_sun, ARC_CX + ARC_R * cosf(th) - 11, ARC_CY + ARC_R * sinf(th) - 11);
            lv_obj_remove_flag(ex_sun, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text_fmt(ex_day, tr(T_DAYLIGHT), len / 60, len % 60);
        } else {
            lv_arc_set_value(ex_arc, 0);                      // night: the whole arc dim
            lv_obj_add_flag(ex_sun, LV_OBJ_FLAG_HIDDEN);
            const char *next = cur > set && wx.ndays > 1 ? wx.day[1].sunrise : wx.day[0].sunrise;
            config_fmt_hhmm(next, buf, sizeof(buf));
            lv_label_set_text_fmt(ex_day, tr(T_SUNRISE), buf);
        }
    }

    // UV
    if (have_wx) {
        float uv = wx.uv;
        const char *lvl = tr(uv < 3 ? T_LOW : uv < 6 ? T_MODERATE : uv < 8 ? T_HIGH : uv < 11 ? T_VERY_HIGH : T_EXTREME);
        lv_color_t c = lv_color_hex(uv < 3 ? 0x6FD08C : uv < 6 ? 0xFFC83D : uv < 8 ? 0xFF8A3D : uv < 11 ? 0xFF4D4D : 0xC77DFF);
        char mx[48];
        snprintf(mx, sizeof(mx), tr(T_UV_MAX), (int)lroundf(wx.day[0].uv_max));
        snprintf(buf, sizeof(buf), "%.0f  %s  %s", uv, lvl, mx);
        ex_row(0, tr(T_UV_INDEX), buf, c);
    }

    // Moon
    int illum;
    const char *ph = moon_phase(now, &illum);
    snprintf(buf, sizeof(buf), "%s  %d%%", ph, illum);
    lv_obj_set_style_text_font(ex_val[1], strlen(buf) > 22 ? f_micro : f_tiny, 0);   // "Gibbeuse décroissante  78%"
    ex_row(1, tr(T_MOON), buf, C_TEXT);
    lv_obj_invalidate(ex_moon);

    // Air quality (US AQI, CAMS global)
    if (have_air && ex_air.us_aqi >= 0) {
        int q = ex_air.us_aqi;
        const char *lvl = tr(q <= 50 ? T_AQI_GOOD : q <= 100 ? T_AQI_MODERATE : q <= 150 ? T_AQI_SENSITIVE :
                             q <= 200 ? T_AQI_UNHEALTHY : q <= 300 ? T_AQI_VERY_UNH : T_AQI_HAZARDOUS);
        lv_color_t c = lv_color_hex(q <= 50 ? 0x6FD08C : q <= 100 ? 0xFFC83D : q <= 150 ? 0xFF8A3D : q <= 200 ? 0xFF4D4D : 0xC77DFF);
        snprintf(buf, sizeof(buf), "%s  %d", lvl, q);
        ex_row(2, tr(T_AIR_QUALITY), buf, c);
    } else ex_row(2, tr(T_AIR_QUALITY), "-", C_DIM);

    // Pollen (Europe only): the strongest type
    static const tid_t pn[4] = { T_POLLEN_ALDER, T_POLLEN_BIRCH, T_POLLEN_GRASS, T_POLLEN_RAGWEED };
    int best = -1;
    for (int i = 0; i < 4; i++) if (ex_air.pollen[i] >= 0 && (best < 0 || ex_air.pollen[i] > ex_air.pollen[best])) best = i;
    if (have_air && best >= 0) {
        float v = ex_air.pollen[best];
        const char *lvl = tr(v < 10 ? T_LOW : v < 50 ? T_MODERATE : v < 200 ? T_HIGH : T_VERY_HIGH);
        snprintf(buf, sizeof(buf), "%s  %s", tr(pn[best]), lvl);
        ex_row(3, tr(T_POLLEN), buf, lv_color_hex(v < 10 ? 0x6FD08C : v < 50 ? 0xFFC83D : v < 200 ? 0xFF8A3D : 0xFF4D4D));
        lv_obj_remove_flag(ex_key[3], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ex_val[3], LV_OBJ_FLAG_HIDDEN);
    } else {                                   // Open-Meteo's pollen data only covers Europe
        lv_obj_add_flag(ex_key[3], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ex_val[3], LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_air(const air_t *a)
{
    display_lock(-1);
    ex_air = *a;
    have_air = true;
    if (lv_screen_active() == scr_extras) extras_refresh();
    display_unlock();
}

static void extras_create(void)
{
    scr_extras = base_screen();
    ex_date = label(scr_extras, f_tiny, C_DIM, 40);

    ex_arc = lv_arc_create(scr_extras);
    lv_obj_set_size(ex_arc, ARC_R * 2, ARC_R * 2);
    lv_obj_set_pos(ex_arc, ARC_CX - ARC_R, ARC_CY - ARC_R);
    lv_arc_set_bg_angles(ex_arc, 180, 360);
    lv_arc_set_range(ex_arc, 0, 1000);
    lv_arc_set_rotation(ex_arc, 0);
    lv_arc_set_mode(ex_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_angles(ex_arc, 180, 180);
    lv_obj_set_style_arc_width(ex_arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ex_arc, lv_color_hex(0x2A3138), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ex_arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ex_arc, lv_color_hex(0xFFC83D), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ex_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(ex_arc, 0, LV_PART_KNOB);
    lv_obj_remove_flag(ex_arc, LV_OBJ_FLAG_CLICKABLE);

    ex_sun = lv_obj_create(scr_extras);
    lv_obj_remove_style_all(ex_sun);
    lv_obj_set_size(ex_sun, 22, 22);
    lv_obj_set_style_radius(ex_sun, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ex_sun, lv_color_hex(0xFFC83D), 0);
    lv_obj_set_style_bg_opa(ex_sun, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(ex_sun, lv_color_hex(0xFFB000), 0);
    lv_obj_set_style_shadow_width(ex_sun, 16, 0);
    lv_obj_add_flag(ex_sun, LV_OBJ_FLAG_HIDDEN);

    ex_day = label(scr_extras, f_small, C_TEXT, ARC_CY - 70);
    ex_rise = label(scr_extras, f_tiny, C_DIM, ARC_CY + 8);
    lv_obj_set_width(ex_rise, 90);
    lv_obj_align(ex_rise, LV_ALIGN_TOP_MID, -ARC_R, ARC_CY + 8);
    ex_set = label(scr_extras, f_tiny, C_DIM, ARC_CY + 8);
    lv_obj_set_width(ex_set, 90);
    lv_obj_align(ex_set, LV_ALIGN_TOP_MID, ARC_R, ARC_CY + 8);

    for (int i = 0; i < 4; i++) {
        int y = 260 + i * 36;
        ex_key[i] = label(scr_extras, f_tiny, C_DIM, y);
        lv_obj_set_width(ex_key[i], 160);                     // "Qualité de l'air" on one line
        lv_obj_set_style_text_align(ex_key[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(ex_key[i], LV_ALIGN_TOP_LEFT, 72, y);
        ex_val[i] = label(scr_extras, f_tiny, C_TEXT, y);
        lv_obj_set_width(ex_val[i], 240);
        lv_obj_set_style_text_align(ex_val[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(ex_val[i], LV_ALIGN_TOP_RIGHT, -72, y);
    }
    lv_obj_align(ex_val[1], LV_ALIGN_TOP_RIGHT, -106, 260 + 36);   // room for the moon picture
    ex_moon = lv_obj_create(scr_extras);
    lv_obj_remove_style_all(ex_moon);
    lv_obj_set_size(ex_moon, 26, 26);
    lv_obj_align(ex_moon, LV_ALIGN_TOP_RIGHT, -72, 260 + 36 - 2);
    lv_obj_add_event_cb(ex_moon, moon_draw, LV_EVENT_DRAW_MAIN, NULL);
    page_dots(scr_extras, 1);
    lv_obj_add_event_cb(scr_extras, gesture_cb, LV_EVENT_GESTURE, NULL);
    extras_refresh();
}

/* ---------- Status page (swipe right from the extras page) ----------
 * Firmware version, Wi-Fi, and the health of every external service (svc.c): a dot per service
 * (green OK, amber one failure, red failing, grey not used yet), when it was last tried and why it failed. */

static lv_obj_t *st_ver, *st_net, *st_dot[SVC_COUNT], *st_age[SVC_COUNT], *st_detail[SVC_COUNT];

static void fmt_age(int64_t us, char *out, size_t n)
{
    int s = (int)(us / 1000000);
    if (s < 60) snprintf(out, n, tr(T_AGE_S), s);
    else if (s < 3600) snprintf(out, n, tr(T_AGE_MIN), s / 60);
    else if (s < 86400) snprintf(out, n, tr(T_AGE_H), s / 3600, s / 60 % 60);
    else snprintf(out, n, tr(T_AGE_D), s / 86400);
}

static void join(char *out, size_t n, const char *part)    // "a  ·  b"
{
    if (!part || !*part) return;
    size_t l = strlen(out);
    snprintf(out + l, n - l, "%s%s", l ? "  ·  " : "", part);
}

static void status_refresh(void)       // display lock held
{
    int64_t now = esp_timer_get_time();
    char a[24], b[48], d[96];
    ota_status_t o;
    ota_get_status(&o);
    const esp_partition_t *part = esp_ota_get_running_partition();
    lv_label_set_text_fmt(st_ver, "%s  ·  %s  ·  %s", o.current, tr(strcmp(o.channel, "beta") ? T_STABLE : T_BETA),
                          part ? part->label : "?");
    fmt_age(now, a, sizeof(a));
    wifi_ap_record_t ap;
    char ip[20];
    if (net_is_connected() && net_get_ip(ip, sizeof(ip)) && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        lv_label_set_text_fmt(st_net, tr(T_WIFI_UP), ap.rssi, ip, a);
    else lv_label_set_text_fmt(st_net, tr(T_WIFI_OFFLINE), a);

    for (int i = 0; i < SVC_COUNT; i++) {
        svc_info_t s;
        svc_get(i, &s);
        uint32_t c;
        d[0] = 0;
        if (i == SVC_UPDATES && o.latest[0]) { snprintf(b, sizeof(b), tr(T_SVC_OFFERS), o.latest); join(d, sizeof(d), b); }
        else join(d, sizeof(d), i == SVC_FORECAST ? tr(T_API_FORECAST) : i == SVC_AIR ? tr(T_API_AIR) :
                                i == SVC_TILES ? tr(T_API_TILES) : i == SVC_UPDATES ? tr(T_API_UPDATES) : s.api);
        if (s.probing) {
            c = 0x5A636E;
            a[0] = 0;
            join(d, sizeof(d), tr(T_SVC_CHECKING));
        } else if (!s.last_try) {
            c = 0x5A636E;
            a[0] = 0;
            join(d, sizeof(d), tr(i == SVC_NTP ? T_SVC_WAIT_SYNC : T_SVC_NOT_USED));
        } else {
            fmt_age(now - s.last_try, a, sizeof(a));
            if (s.ok) {
                c = 0x6FD08C;
                join(d, sizeof(d), "OK");
                if (s.ms) { snprintf(b, sizeof(b), "%d ms", s.ms); join(d, sizeof(d), b); }
            } else {
                c = s.fails >= 2 || !s.last_ok ? 0xFF4D4D : 0xFFC83D;
                join(d, sizeof(d), s.why);
                if (s.fails > 1) { snprintf(b, sizeof(b), tr(T_SVC_IN_A_ROW), s.fails); join(d, sizeof(d), b); }
                if (s.last_ok) {
                    char t[16];
                    fmt_age(now - s.last_ok, t, sizeof(t));
                    snprintf(b, sizeof(b), tr(T_SVC_OK_AGO), t);
                } else snprintf(b, sizeof(b), "%s", tr(T_SVC_NEVER_OK));
                join(d, sizeof(d), b);
            }
        }
        lv_obj_set_style_bg_color(st_dot[i], lv_color_hex(c), 0);
        lv_label_set_text(st_age[i], a);
        lv_label_set_text(st_detail[i], d);
    }
}

static void status_tick(lv_timer_t *t)
{
    if (lv_screen_active() == scr_status) status_refresh();
}

static lv_obj_t *st_label(lv_obj_t *parent, lv_font_t *f, lv_color_t c, int x, int y, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_size(l, w, lv_font_get_line_height(f));     // one line: LONG_DOT needs a fixed height
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

static void status_create(void)
{
    scr_status = base_screen();
    tlabel(label(scr_status, f_small, C_ACCENT, 34), T_STATUS);
    st_ver = label(scr_status, f_tiny, C_TEXT, 62);
    st_net = label(scr_status, f_micro, C_DIM, 88);
    lv_obj_set_width(st_net, 340);

    // The list scrolls inside a box that stays clear of the round edge and the page dots (y 120..400)
    lv_obj_t *box = lv_obj_create(scr_status);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 280, 280);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    for (int i = 0; i < SVC_COUNT; i++) {
        svc_info_t s;
        svc_get(i, &s);
        lv_obj_t *row = lv_obj_create(box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 280, 42);
        st_dot[i] = lv_obj_create(row);
        lv_obj_remove_style_all(st_dot[i]);
        lv_obj_set_size(st_dot[i], 10, 10);
        lv_obj_set_pos(st_dot[i], 0, 6);
        lv_obj_set_style_radius(st_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(st_dot[i], LV_OPA_COVER, 0);
        lv_label_set_text(st_label(row, f_tiny, C_TEXT, 18, 0, 196), s.name);
        st_age[i] = st_label(row, f_micro, C_DIM, 214, 2, 66);
        lv_obj_set_style_text_align(st_age[i], LV_TEXT_ALIGN_RIGHT, 0);
        st_detail[i] = st_label(row, f_micro, C_DIM, 18, 22, 262);
    }
    passthrough(box);                                       // rows and labels: presses reach the box/screen
    page_dots(scr_status, 0);
    lv_obj_add_event_cb(scr_status, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_timer_create(status_tick, 1000, NULL);
    status_refresh();
}

/* ---------- Firmware update: pill on the weather screen + update screen ---------- */

static lv_obj_t *up_title, *up_body, *up_btn, *up_bar, *up_state, *up_box, *up_notes;
static ota_status_t up_st;
static int up_notes_id = -1;

// "What's new": one accent header ("v1.4.0 · September 30, 2026") and one bulleted label per release.
static void update_notes(void)        // display lock held
{
    if (up_st.notes_id == up_notes_id) return;
    up_notes_id = up_st.notes_id;
    char *txt = heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    if (!txt) return;
    ota_get_notes(txt, 3072);
    lv_obj_clean(up_notes);
    if (!*txt) { lv_obj_add_flag(up_notes, LV_OBJ_FLAG_HIDDEN); free(txt); return; }
    lv_obj_remove_flag(up_notes, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *h = al_label(up_notes, f_small, C_TEXT);
    lv_label_set_text(h, tr(T_WHATS_NEW));
    lv_obj_set_style_pad_bottom(h, 2, 0);
    char *body = heap_caps_malloc(3200, MALLOC_CAP_SPIRAM);
    char *save = NULL;
    int bn = 0;
    for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *bar = strchr(line, '|');
        bool header = line[0] == 'v' && bar;
        if (header || !body) {
            if (body && bn) { lv_label_set_text(al_label(up_notes, f_tiny, C_TEXT), body); bn = 0; }
            if (!header) continue;
            *bar = 0;
            lv_obj_t *l = al_label(up_notes, f_micro, C_ACCENT);
            if (bar[1]) lv_label_set_text_fmt(l, "%s  ·  %s", line, bar + 1);
            else lv_label_set_text(l, line);
            lv_obj_set_style_pad_top(l, 10, 0);
            continue;
        }
        bn += snprintf(body + bn, 3200 - bn, "%s%s %s", bn ? "\n" : "", strcmp(line, "...") ? "•" : "", line);
        if (bn >= 3200) bn = 3199;
    }
    if (body && bn) lv_label_set_text(al_label(up_notes, f_tiny, C_TEXT), body);
    free(body);
    free(txt);
}

static void update_render(void)       // display lock held
{
    const ota_status_t *o = &up_st;
    bool show_pill = o->state == OTA_AVAILABLE || o->state == OTA_DOWNLOADING || o->state == OTA_DONE;
    if (show_pill) {
        if (o->state == OTA_AVAILABLE) lv_label_set_text_fmt(up_pill_lbl, tr(T_PILL_UPDATE), o->latest);
        else lv_label_set_text_fmt(up_pill_lbl, tr(T_PILL_UPDATING), o->progress);
        lv_obj_remove_flag(up_pill, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(up_pill, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text_fmt(up_body, tr(T_UP_YOU_HAVE), o->latest, o->current);
    bool busy = o->state == OTA_DOWNLOADING || o->state == OTA_DONE;
    if (o->state == OTA_AVAILABLE) lv_obj_remove_flag(up_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(up_btn, LV_OBJ_FLAG_HIDDEN);
    if (busy) {
        lv_obj_remove_flag(up_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(up_bar, o->state == OTA_DONE ? 100 : o->progress, LV_ANIM_OFF);
    } else lv_obj_add_flag(up_bar, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(up_state,
        o->state == OTA_DOWNLOADING ? tr(T_UP_DOWNLOADING) :
        o->state == OTA_DONE ? tr(T_UP_INSTALLED) :
        o->state == OTA_FAILED ? o->error :
        o->state == OTA_AVAILABLE ? tr(T_UP_KEPT) : "");
    lv_label_set_text(up_title, tr(o->state == OTA_DONE ? T_UP_DONE : busy ? T_UP_BUSY : T_UP_AVAILABLE));
    update_notes();
}

void ui_ota(const ota_status_t *o)      // OTA task
{
    display_lock(-1);
    up_st = *o;
    update_render();
    if (o->state == OTA_UP_TO_DATE && lv_screen_active() == scr_update) lv_screen_load(scr_main);
    display_unlock();
}

static void update_show(void)
{
    update_render();
    lv_obj_scroll_to_y(up_box, 0, LV_ANIM_OFF);
    lv_screen_load_anim(scr_update, LV_SCR_LOAD_ANIM_MOVE_TOP, 260, 0, false);
}

static void update_install(lv_event_t *e)
{
    ESP_LOGI("ui", "install update tapped");
    ota_install();
    lv_obj_add_flag(up_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(up_box, 0, LV_ANIM_ON);          // the progress bar is at the top
}

static void update_tap(lv_event_t *e)
{
    if (up_st.state == OTA_DOWNLOADING || up_st.state == OTA_DONE) return;    // stay while installing
    lv_screen_load_anim(scr_main, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 260, 0, false);
}

// Title fixed at the top; versions, Install, progress and the release notes scroll in one column.
static void update_create(void)
{
    up_pill = lv_obj_create(scr_main);
    lv_obj_remove_style_all(up_pill);
    lv_obj_set_size(up_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(up_pill, 12, 0);
    lv_obj_set_style_bg_color(up_pill, C_ACCENT, 0);
    lv_obj_set_style_bg_opa(up_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(up_pill, 12, 0);
    lv_obj_set_style_pad_ver(up_pill, 2, 0);
    lv_obj_align(up_pill, LV_ALIGN_BOTTOM_MID, 0, -26);
    up_pill_lbl = lv_label_create(up_pill);
    lv_obj_set_style_text_font(up_pill_lbl, f_micro, 0);
    lv_obj_set_style_text_color(up_pill_lbl, lv_color_hex(0x04121F), 0);
    lv_obj_add_flag(up_pill, LV_OBJ_FLAG_HIDDEN);

    scr_update = base_screen();
    up_title = label(scr_update, f_city, C_ACCENT, 40);
    lv_obj_set_width(up_title, 300);
    up_box = lv_obj_create(scr_update);
    lv_obj_remove_style_all(up_box);
    lv_obj_set_size(up_box, 310, 386);
    lv_obj_align(up_box, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(up_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(up_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(up_box, 10, 0);
    lv_obj_set_style_pad_bottom(up_box, 90, 0);         // the last lines can scroll above the round edge
    lv_obj_set_scroll_dir(up_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(up_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(up_box, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    up_body = al_label(up_box, f_tiny, C_DIM);
    lv_obj_set_style_text_align(up_body, LV_TEXT_ALIGN_CENTER, 0);
    up_btn = lv_button_create(up_box);
    lv_obj_set_size(up_btn, 200, 56);
    lv_obj_set_style_radius(up_btn, 28, 0);
    lv_obj_set_style_bg_color(up_btn, C_ACCENT, 0);
    lv_obj_t *bl = lv_label_create(up_btn);
    lv_obj_set_style_text_font(bl, f_small, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0x04121F), 0);
    tlabel(bl, T_INSTALL);
    lv_obj_center(bl);
    lv_obj_add_event_cb(up_btn, update_install, LV_EVENT_CLICKED, NULL);
    up_bar = lv_bar_create(up_box);
    lv_obj_set_size(up_bar, 240, 12);
    lv_obj_set_style_margin_ver(up_bar, 22, 0);
    lv_bar_set_range(up_bar, 0, 100);
    lv_obj_set_style_bg_color(up_bar, lv_color_hex(0x2A3138), LV_PART_MAIN);
    lv_obj_set_style_bg_color(up_bar, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_add_flag(up_bar, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    up_state = al_label(up_box, f_micro, C_DIM);
    lv_obj_set_style_text_align(up_state, LV_TEXT_ALIGN_CENTER, 0);
    up_notes = lv_obj_create(up_box);
    lv_obj_remove_style_all(up_notes);
    lv_obj_set_size(up_notes, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(up_notes, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(up_notes, 4, 0);
    lv_obj_set_style_pad_top(up_notes, 8, 0);
    lv_obj_add_flag(up_notes, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(up_notes, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_update, update_tap, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(scr_update, alert_gesture, LV_EVENT_GESTURE, NULL);
}

// Tiny icons for the weather screen's detail row, drawn like the weather icons (the font has no symbols).
// Droplet = humidity (rain blue), three staggered strokes = wind (a light grey, brighter than the text).
static void drop_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    int cx = (a.x1 + a.x2) / 2, w = lv_area_get_width(&a), r = w * 4 / 10;
    lv_draw_rect_dsc_t c;
    lv_draw_rect_dsc_init(&c);
    c.bg_color = lv_color_hex(0x4DA3FF);                     // the rain drops' blue
    c.radius = LV_RADIUS_CIRCLE;
    lv_area_t ball = { cx - r, a.y2 - 2 * r, cx + r, a.y2 };
    lv_draw_rect(layer, &c, &ball);
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.bg_color = lv_color_hex(0x4DA3FF);
    t.p[0].x = cx;          t.p[0].y = a.y1;
    t.p[1].x = cx - r;      t.p[1].y = a.y2 - r;
    t.p[2].x = cx + r + 1;  t.p[2].y = a.y2 - r;
    lv_draw_triangle(layer, &t);
}

static void wind_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.color = lv_color_hex(0xC9D1DA);                        // whiter than the text
    l.width = 2;
    l.round_start = l.round_end = 1;
    static const struct { int y, x0, x1; } s[3] = { { 20, 30, 100 }, { 50, 0, 85 }, { 80, 20, 70 } };   // % of the box
    for (int i = 0; i < 3; i++) {
        l.p1.x = a.x1 + w * s[i].x0 / 100; l.p2.x = a.x1 + w * s[i].x1 / 100;
        l.p1.y = l.p2.y = a.y1 + h * s[i].y / 100;
        lv_draw_line(layer, &l);
    }
}

static lv_obj_t *row_label(lv_obj_t *row)
{
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, f_small, 0);
    lv_obj_set_style_text_color(l, C_DIM, 0);
    lv_label_set_text(l, "");
    return l;
}

static void row_icon(lv_obj_t *row, int w, int h, lv_event_cb_t draw)
{
    lv_obj_t *o = lv_obj_create(row);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_add_event_cb(o, draw, LV_EVENT_DRAW_MAIN, NULL);
}

// One place's weather page: the layout the weather screen always had
static void place_page_create(int i, lv_obj_t *pg)
{
    place_page_t *p = &pp[i];
    p->time = label(pg, f_time, C_DIM, 38);
    p->city = label(pg, f_city, C_TEXT, 72);
    lv_obj_t *hero = lv_obj_create(pg);                      // icon + big temperature, centred together
    lv_obj_remove_style_all(hero);
    lv_obj_set_size(hero, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 14, 0);
    lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 112);
    p->icon = icon_box_create(hero, 80);
    p->temp = lv_label_create(hero);
    lv_obj_set_style_text_font(p->temp, f_big, 0);
    lv_obj_set_style_text_color(p->temp, C_TEXT, 0);
    lv_label_set_text(p->temp, "");
    p->cond = label(pg, f_cond, C_TEXT, 214);
    p->detail = lv_obj_create(pg);                          // feels-like · humidity · wind, centred
    lv_obj_remove_style_all(p->detail);
    lv_obj_set_size(p->detail, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p->detail, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(p->detail, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(p->detail, 6, 0);
    lv_obj_align(p->detail, LV_ALIGN_TOP_MID, 0, 248);
    p->feels = row_label(p->detail);
    lv_label_set_text(row_label(p->detail), "·");
    row_icon(p->detail, 12, 16, drop_draw);
    p->hum = row_label(p->detail);
    lv_label_set_text(row_label(p->detail), "·");
    row_icon(p->detail, 18, 14, wind_draw);
    p->wind = row_label(p->detail);
    lv_obj_add_flag(p->detail, LV_OBJ_FLAG_HIDDEN);
    p->nowcast = label(pg, f_tiny, C_ACCENT, 273);           // "Rain around 14:45" (hidden when none)
    lv_obj_add_flag(p->nowcast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *div = lv_obj_create(pg);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 260, 2);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x2A3138), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 298);
    for (int k = 0; k < 3; k++) {                              // 3-day forecast: day / icon / high-low
        int dx = (k - 1) * 98;
        p->fc_day[k] = label(pg, f_tiny, C_ACCENT, 0);
        lv_obj_set_width(p->fc_day[k], 120);                  // "Aujourd'hui" (columns are 98 px apart)
        lv_obj_align(p->fc_day[k], LV_ALIGN_TOP_MID, dx, 306);
        p->fc_icon[k] = icon_box_create(pg, 36);
        lv_obj_align(p->fc_icon[k], LV_ALIGN_TOP_MID, dx, 334);
        p->fc_temp[k] = label(pg, f_tiny, C_TEXT, 0);
        lv_obj_set_width(p->fc_temp[k], 96);
        lv_obj_align(p->fc_temp[k], LV_ALIGN_TOP_MID, dx, 384);
    }
}

static void place_dots(int active)
{
    for (int i = 0; i < MAX_PLACES; i++) {
        if (n_places < 2 || i >= n_places) { lv_obj_add_flag(pl_dot[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_set_size(pl_dot[i], 7, i == active ? 18 : 7);
        lv_obj_set_style_bg_color(pl_dot[i], i == active ? C_TEXT : C_DIM, 0);
        lv_obj_set_style_bg_opa(pl_dot[i], i == active ? LV_OPA_COVER : LV_OPA_60, 0);
        lv_obj_align(pl_dot[i], LV_ALIGN_RIGHT_MID, -14,
                     (2 * i - (n_places - 1)) * 8 + (i < active ? -5 : i > active ? 5 : 0));
        lv_obj_remove_flag(pl_dot[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void place_scrolled(int i, void *user) { place_dots(i); }     // dots follow the finger

static void place_settled(int i, void *user)
{
    if (i != cur_place && i < n_places && place_select_cb) {
        ESP_LOGI("ui", "place %d", i + 1);
        place_select_cb(i);
    }
}

/* ---------- Settings screen (long-press on the weather screen) ----------
 * Quick settings on the display itself: screen (dimming, pick-up, timing, brightness arc along the bottom edge),
 * units, and shortcuts (phone settings QR, Wi-Fi setup, updates, restart). Each change is saved at once through the
 * same functions as the settings page (presence.c, config.c), so the phone and the display always agree. Places,
 * the Wi-Fi password, custom timings and sound calibration stay on the phone (typing, map, live meter). */

enum { R_DIM, R_MOTION, R_TIMING, R_TEMP, R_WIND, R_CLOCK, R_LANG, R_CHIME, R_VOLUME, R_TEST, R_PHONE, R_WIFI, R_UPDATE,
       R_RESTART, CFG_ROWS };
static lv_obj_t *cfg_row[CFG_ROWS], *cfg_val[CFG_ROWS], *cfg_arc, *cfg_bright, *cfg_zone;
static uint32_t check_tapped;                      // tick of "Check now" (shows the result for a few seconds)
static const struct { int dim, off, wake; tid_t name; } cfg_presets[] = {          // as the settings page's PRESETS
    { 120, 900, 2, T_T_SHORT }, { 600, 3600, 3, T_T_NORMAL }, { 1800, 10800, 3, T_T_LONG },   // off = total quiet
};
static uint32_t restart_armed;                     // tick of the first "Restart" tap (a second one restarts)
static void (*data_refresh_cb)(void);              // main.c: fetch again (alerts, notes) after a language change

static int cfg_preset(const presence_cfg_t *c)     // index into cfg_presets, -1 = custom
{
    for (int i = 0; i < 3; i++)
        if ((int)c->dim_s == cfg_presets[i].dim && (int)(c->dim_s + c->off_s) == cfg_presets[i].off &&
            (int)c->wake_s == cfg_presets[i].wake) return i;
    return -1;
}

static void cfg_switch(int r, bool on)
{
    lv_obj_t *sw = cfg_val[r];
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED); else lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

static void cfg_refresh(void)                      // display lock held
{
    presence_cfg_t c;
    presence_status_t st;
    units_t u;
    ota_status_t o;
    presence_get_config(&c);
    presence_get_status(&st);
    config_get_units(&u);
    ota_get_status(&o);
    cfg_switch(R_DIM, c.enabled);
    cfg_switch(R_MOTION, presence_motion_wake());
    if (st.imu_ok) lv_obj_remove_flag(cfg_row[R_MOTION], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(cfg_row[R_MOTION], LV_OBJ_FLAG_HIDDEN);
    int p = cfg_preset(&c);
    lv_label_set_text(cfg_val[R_TIMING], tr(p < 0 ? T_T_CUSTOM : cfg_presets[p].name));
    lv_label_set_text(cfg_val[R_TEMP], u.fahrenheit ? "°F" : "°C");
    lv_label_set_text(cfg_val[R_WIND], u.wind == WIND_MPH ? "mph" : u.wind == WIND_MS ? "m/s" : "km/h");
    lv_label_set_text(cfg_val[R_CLOCK], u.h12 ? "12 h" : "24 h");
    lv_label_set_text(cfg_val[R_LANG], i18n_name(i18n_lang()));
    sound_cfg_t sc;
    sound_get_config(&sc);
    static const tid_t lvl[4] = { T_CHIME_OFF, T_CHIME_RED, T_CHIME_ORANGE, T_CHIME_ALL };
    lv_label_set_text(cfg_val[R_CHIME], tr(lvl[sc.level & 3]));
    lv_label_set_text_fmt(cfg_val[R_VOLUME], "%d%%", sc.volume);
    char b[48];
    bool just_checked = check_tapped && lv_tick_elaps(check_tapped) < 6000;
    if (o.state == OTA_AVAILABLE) snprintf(b, sizeof(b), "%s >", o.latest);             // tap: update screen
    else if (o.state == OTA_CHECKING) snprintf(b, sizeof(b), "%s", tr(T_CHECKING));
    else if (o.state == OTA_DOWNLOADING) snprintf(b, sizeof(b), "%d%%", o.progress);
    else if (just_checked && o.state == OTA_UP_TO_DATE) snprintf(b, sizeof(b), "%s", tr(T_UP_TO_DATE));
    else if (just_checked && o.state == OTA_FAILED) snprintf(b, sizeof(b), "%s", tr(T_FAILED));
    else snprintf(b, sizeof(b), "%s", tr(T_CHECK_NOW));
    lv_label_set_text(cfg_val[R_UPDATE], b);
    bool armed = restart_armed && lv_tick_elaps(restart_armed) < 4000;
    lv_label_set_text(cfg_val[R_RESTART], armed ? tr(T_TAP_AGAIN) : "");
    if (!lv_obj_has_state(cfg_zone, LV_STATE_PRESSED)) {
        lv_arc_set_value(cfg_arc, c.bright_pct);
        lv_label_set_text_fmt(cfg_bright, tr(T_BRIGHTNESS), c.bright_pct);
    }
}

static void cfg_tick(lv_timer_t *t)
{
    if (lv_screen_active() == scr_cfg) cfg_refresh();      // update state, changes made from the phone
}

static void do_restart(lv_timer_t *t) { esp_restart(); }

static void cfg_tap(lv_event_t *e)
{
    int r = (int)(intptr_t)lv_event_get_user_data(e);
    presence_cfg_t c;
    presence_status_t st;
    units_t u;
    presence_get_config(&c);
    presence_get_status(&st);
    config_get_units(&u);
    ESP_LOGI("ui", "settings row %d", r);
    switch (r) {
    case R_DIM: c.enabled = !c.enabled; presence_set_config(&c); break;
    case R_MOTION: presence_set_motion(!presence_motion_wake(), st.motion_thr); break;
    case R_TIMING: {
        int p = (cfg_preset(&c) + 1) % 3;                       // custom -> Short
        c.dim_s = cfg_presets[p].dim;
        c.off_s = cfg_presets[p].off - cfg_presets[p].dim;
        c.wake_s = cfg_presets[p].wake;
        presence_set_config(&c);
        break;
    }
    case R_TEMP: u.fahrenheit = !u.fahrenheit; config_set_units(&u); ui_units_changed(); break;
    case R_WIND: u.wind = (u.wind + 1) % 3; config_set_units(&u); ui_units_changed(); break;
    case R_CLOCK: u.h12 = !u.h12; config_set_units(&u); ui_units_changed(); break;
    case R_LANG:
        u.lang = (u.lang + 1) % LANG_COUNT;
        config_set_units(&u);
        ui_units_changed();                                     // every screen, in the new language
        if (data_refresh_cb) data_refresh_cb();                 // alerts and release notes in the new language
        break;
    case R_CHIME: case R_VOLUME: {
        sound_cfg_t sc;
        sound_get_config(&sc);
        if (r == R_CHIME) sc.level = (sc.level + 1) % 4;          // Off, Red, Orange+, All
        else sc.volume = sc.volume >= 100 ? 20 : (sc.volume / 20 + 1) * 20;   // 20, 40 ... 100
        sound_set_config(&sc);
        if (r == R_VOLUME) sound_test(2);                         // hear the new volume
        break;
    }
    case R_TEST: sound_test(2); break;
    case R_PHONE: back_to_cfg = true; lv_screen_load(scr_main); show_settings(NULL); return;
    case R_WIFI: back_to_cfg = true; ui_wifi_setup(NULL); return;
    case R_UPDATE: {
        ota_status_t o;
        ota_get_status(&o);
        if (o.state == OTA_AVAILABLE) { update_show(); return; }
        if (o.state != OTA_CHECKING && o.state != OTA_DOWNLOADING) { ota_check_now(); check_tapped = lv_tick_get(); }
        break;
    }
    case R_RESTART:
        if (restart_armed && lv_tick_elaps(restart_armed) < 4000) {
            ESP_LOGI("ui", "restart from the settings screen");
            lv_label_set_text(cfg_val[R_RESTART], tr(T_RESTARTING));
            lv_timer_create(do_restart, 400, NULL);
            return;
        }
        restart_armed = lv_tick_get();
        break;
    }
    cfg_refresh();
}

// Brightness: the band under the arc follows the finger's x (left 5 %, right 100 %); the arc only shows the value.
// (A clickable full-size lv_arc caught every touch on the screen, rows and Done included.)
#define BR_X0 60
#define BR_X1 406
static void cfg_bright_changed(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t pt;
    lv_indev_get_point(in, &pt);
    int v = 5 + (pt.x - BR_X0) * 95 / (BR_X1 - BR_X0);
    v = v < 5 ? 5 : v > 100 ? 100 : v;
    lv_arc_set_value(cfg_arc, v);
    lv_label_set_text_fmt(cfg_bright, tr(T_BRIGHTNESS), v);
    presence_preview_brightness(v);                           // the screen follows the finger
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {   // save when the finger lifts (NVS write)
        presence_cfg_t c;
        presence_get_config(&c);
        c.bright_pct = v;
        presence_set_config(&c);
        ESP_LOGI("ui", "brightness %d%%", v);
    }
}

static void cfg_close(lv_event_t *e)
{
    lv_screen_load_anim(scr_main, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 260, 0, false);
}

static void cfg_gesture(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    if (lv_indev_get_gesture_dir(in) == LV_DIR_RIGHT) cfg_close(e);   // swipe right = back
    lv_indev_wait_release(in);
}

static void open_cfg(lv_event_t *e)                // long-press on the weather screen
{
    if (!net_is_connected()) {                       // offline: Wi-Fi setup is what's needed
        ESP_LOGI("ui", "long press while offline -> Wi-Fi setup");
        ui_wifi_setup(NULL);
        return;
    }
    ESP_LOGI("ui", "long press -> settings");
    restart_armed = 0;
    check_tapped = 0;
    back_to_cfg = false;
    cfg_refresh();
    lv_obj_scroll_to_y(lv_obj_get_parent(cfg_row[0]), 0, LV_ANIM_OFF);
    lv_screen_load_anim(scr_cfg, LV_SCR_LOAD_ANIM_MOVE_TOP, 260, 0, false);
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);               // the long-press's release isn't a tap on a row
}

static lv_obj_t *cfg_add_row(lv_obj_t *box, int r, tid_t name, bool is_switch)
{
    lv_obj_t *row = cfg_row[r] = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 300, 52);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1A2027), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, cfg_tap, LV_EVENT_CLICKED, (void *)(intptr_t)r);
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, f_small, 0);
    lv_obj_set_style_text_color(l, C_TEXT, 0);
    tlabel(l, name);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 14, 0);
    if (is_switch) {
        lv_obj_t *sw = cfg_val[r] = lv_switch_create(row);
        lv_obj_set_size(sw, 54, 30);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0x2A3138), 0);
        lv_obj_set_style_bg_color(sw, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);         // the whole row is the button
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
    } else {
        lv_obj_t *v = cfg_val[r] = lv_label_create(row);
        lv_obj_set_style_text_font(v, f_small, 0);
        lv_obj_set_style_text_color(v, C_ACCENT, 0);
        lv_label_set_text(v, "");
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    return row;
}

static void cfg_section(lv_obj_t *box, tid_t name)
{
    lv_obj_t *l = lv_label_create(box);
    lv_obj_set_style_text_font(l, f_micro, 0);
    lv_obj_set_style_text_color(l, C_DIM, 0);
    lv_obj_set_style_pad_top(l, 6, 0);
    lv_obj_set_width(l, 290);
    tlabel(l, name);
    lv_obj_add_flag(l, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void cfg_create(void)
{
    scr_cfg = base_screen();
    lv_obj_t *done = lv_button_create(scr_cfg);                // top: Done
    lv_obj_set_size(done, 120, 40);
    lv_obj_set_style_radius(done, 20, 0);
    lv_obj_set_style_bg_color(done, lv_color_hex(0x2A3138), 0);
    lv_obj_set_style_shadow_width(done, 0, 0);
    lv_obj_align(done, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *dl = lv_label_create(done);
    lv_obj_set_style_text_font(dl, f_small, 0);
    tlabel(dl, T_DONE);
    lv_obj_center(dl);
    lv_obj_add_event_cb(done, cfg_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *box = lv_obj_create(scr_cfg);                    // the rows scroll; Done and brightness stay
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 300, 290);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(box, LV_OBJ_FLAG_GESTURE_BUBBLE);
    cfg_section(box, T_SEC_SCREEN);
    cfg_add_row(box, R_DIM, T_DIM_QUIET, true);
    cfg_add_row(box, R_MOTION, T_WAKE_PICKUP, true);
    cfg_add_row(box, R_TIMING, T_TIMING, false);
    cfg_section(box, T_SEC_UNITS);
    cfg_add_row(box, R_TEMP, T_TEMPERATURE, false);
    cfg_add_row(box, R_WIND, T_WIND, false);
    cfg_add_row(box, R_CLOCK, T_CLOCK, false);
    cfg_add_row(box, R_LANG, T_LANGUAGE, false);
    cfg_section(box, T_SEC_SOUND);
    cfg_add_row(box, R_CHIME, T_CHIME, false);
    cfg_add_row(box, R_VOLUME, T_VOLUME, false);
    cfg_add_row(box, R_TEST, T_TEST_SOUND, false);
    cfg_section(box, T_SEC_MORE);
    cfg_add_row(box, R_PHONE, T_PHONE, false);
    cfg_add_row(box, R_WIFI, T_WIFI_NETWORK, false);
    cfg_add_row(box, R_UPDATE, T_UPDATES, false);
    cfg_add_row(box, R_RESTART, T_RESTART, false);
    lv_label_set_text(cfg_val[R_PHONE], ">");
    lv_label_set_text(cfg_val[R_WIFI], ">");
    lv_label_set_text(cfg_val[R_TEST], ">");

    cfg_bright = label(scr_cfg, f_tiny, C_DIM, 372);           // brightness: an arc along the bottom edge
    cfg_arc = lv_arc_create(scr_cfg);
    lv_obj_set_size(cfg_arc, DISP_W - 14, DISP_W - 14);
    lv_obj_center(cfg_arc);
    lv_arc_set_bg_angles(cfg_arc, 35, 145);
    lv_arc_set_mode(cfg_arc, LV_ARC_MODE_REVERSE);             // drag towards the right = brighter
    lv_arc_set_range(cfg_arc, 5, 100);
    lv_obj_set_style_arc_width(cfg_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(cfg_arc, lv_color_hex(0x2A3138), LV_PART_MAIN);
    lv_obj_set_style_arc_width(cfg_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(cfg_arc, lv_color_hex(0xFFC83D), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(cfg_arc, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(cfg_arc, 6, LV_PART_KNOB);
    lv_obj_remove_flag(cfg_arc, LV_OBJ_FLAG_CLICKABLE);       // display only (see cfg_bright_changed)
    cfg_zone = lv_obj_create(scr_cfg);                         // touch band: the bottom of the circle
    lv_obj_remove_style_all(cfg_zone);
    lv_obj_set_size(cfg_zone, DISP_W, DISP_H - 364);
    lv_obj_align(cfg_zone, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(cfg_zone, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(cfg_zone, cfg_bright_changed, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(cfg_zone, cfg_bright_changed, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(cfg_zone, cfg_bright_changed, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_add_event_cb(scr_cfg, cfg_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(cfg_tick, 1000, NULL);
}

void ui_init(void)
{
    display_lock(-1);
    f_time = mkfont(26);  f_city = mkfont(26);  f_big = mkfont(96);
    f_cond = mkfont(28);  f_small = mkfont(20); f_tiny = mkfont(19); f_micro = mkfont(15);

    scr_msg = base_screen();
    msg_title = label(scr_msg, f_city, C_ACCENT, 140);
    msg_body = label(scr_msg, f_small, C_TEXT, 190);
    lv_obj_set_width(msg_body, 330);

    scr_main = base_screen();
    place_pager = pager_create(scr_main, true, MAX_PLACES, place_scrolled, place_settled, NULL);
    for (int i = 0; i < MAX_PLACES; i++) place_page_create(i, pager_page(place_pager, i));
    // Weather alert pill, in place of the city name while an alert is active (tap for details)
    al_pill = lv_obj_create(scr_main);
    lv_obj_remove_style_all(al_pill);
    lv_obj_set_size(al_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(al_pill, 16, 0);
    lv_obj_set_style_bg_opa(al_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(al_pill, 16, 0);
    lv_obj_set_style_pad_ver(al_pill, 4, 0);
    lv_obj_align(al_pill, LV_ALIGN_TOP_MID, 0, 70);
    al_pill_lbl = lv_label_create(al_pill);
    lv_obj_set_style_text_font(al_pill_lbl, f_small, 0);
    lv_obj_add_flag(al_pill, LV_OBJ_FLAG_HIDDEN);
    location_t loc;
    config_get_location(&loc);
    lv_label_set_text(pp[0].city, loc.name);
    for (int i = 1; i < MAX_PLACES; i++) lv_obj_add_flag(pager_page(place_pager, i), LV_OBJ_FLAG_HIDDEN);

    page_dots(scr_main, 2);
    for (int i = 0; i < MAX_PLACES; i++) {                     // place dots, vertical, right edge (ui_places)
        pl_dot[i] = lv_obj_create(scr_main);
        lv_obj_remove_style_all(pl_dot[i]);
        lv_obj_set_style_radius(pl_dot[i], 4, 0);
        lv_obj_set_style_bg_opa(pl_dot[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(pl_dot[i], LV_OBJ_FLAG_HIDDEN);
    }

    scr_radar = radar_create(f_small, f_small, f_micro);
    page_dots(scr_radar, 3);
    lv_obj_add_event_cb(scr_main, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr_main, open_cfg, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(scr_main, main_tap, LV_EVENT_SHORT_CLICKED, NULL);
    hour_create();
    update_create();                // pill on scr_main (made non-clickable by passthrough) + update screen
    passthrough(scr_main);          // before the (clickable) overlay is added
    lv_obj_add_flag(place_pager, LV_OBJ_FLAG_CLICKABLE);   // it must stay pressable to scroll between places

    overlay = lv_obj_create(scr_main);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, DISP_W, DISP_H);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, overlay_close, LV_EVENT_CLICKED, NULL);
    ov_title = label(overlay, f_small, C_ACCENT, 48);
    lv_label_set_text(ov_title, tr(T_SETTINGS));
    lv_obj_add_event_cb(overlay, show_wifi_setup, LV_EVENT_LONG_PRESSED, NULL);
    ov_qr = make_qr(overlay, 170);
    lv_obj_align(ov_qr, LV_ALIGN_TOP_MID, 0, 84);
    ov_url = label(overlay, f_tiny, C_TEXT, 276);
    lv_obj_set_width(ov_url, 330);

    lv_obj_add_event_cb(scr_msg, msg_long_press, LV_EVENT_LONG_PRESSED, NULL);
    msg_qr = make_qr(scr_msg, 140);
    lv_obj_align(msg_qr, LV_ALIGN_TOP_MID, 0, 250);
    lv_obj_add_flag(msg_qr, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(scr_radar, gesture_cb, LV_EVENT_GESTURE, NULL);
    setup_create();
    alert_create();
    extras_create();
    status_create();
    cfg_create();
    touch_register_lvgl();

    lv_timer_create(clock_tick, 1000, NULL);
    display_unlock();
}

void ui_message_qr(const char *title, const char *body, const char *qr)
{
    display_lock(-1);
    lv_label_set_text(msg_title, title);
    lv_label_set_text(msg_body, body);
    if (qr) {
        lv_qrcode_update(msg_qr, qr, strlen(qr));
        lv_obj_remove_flag(msg_qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(msg_title, LV_ALIGN_TOP_MID, 0, 70);
        lv_obj_align(msg_body, LV_ALIGN_TOP_MID, 0, 110);
    } else {
        lv_obj_add_flag(msg_qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(msg_title, LV_ALIGN_TOP_MID, 0, 140);
        lv_obj_align(msg_body, LV_ALIGN_TOP_MID, 0, 190);
    }
    lv_screen_load(scr_msg);
    display_unlock();
}

static void day_name(const char *date, int idx, char *out, size_t n)
{
    if (idx == 0) { snprintf(out, n, "%s", tr(T_TODAY)); return; }
    struct tm tm = {0};
    if (sscanf(date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
        tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_hour = 12;
        mktime(&tm);
        snprintf(out, n, "%s", tr_weekday(tm.tm_wday, false));
    } else snprintf(out, n, "-");
}

void ui_places(int n, int active)
{
    display_lock(-1);
    bool moved = active != cur_place;
    for (int i = 0; i < MAX_PLACES; i++) {
        lv_obj_t *pg = pager_page(place_pager, i);
        if (i < n) lv_obj_remove_flag(pg, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(pg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(pp[i].city, LV_OBJ_FLAG_HIDDEN);     // the alert pill goes on the place shown
    }
    n_places = n;
    cur_place = active;
    if (alerts.n) lv_obj_add_flag(pp[active].city, LV_OBJ_FLAG_HIDDEN);
    place_dots(active);
    if (pager_current(place_pager) != active) pager_go(place_pager, active, true);   // chosen on the settings page
    if (moved) {
        clock_shown[0] = 0;
        clock_tick(NULL);
        if (lv_screen_active() == scr_hour || lv_screen_active() == scr_extras) lv_screen_load(scr_main);
    }
    display_unlock();
}

void ui_on_place_select(void (*cb)(int i)) { place_select_cb = cb; }

void ui_place(int i, const char *name, const weather_t *w)
{
    if (i < 0 || i >= MAX_PLACES) return;
    display_lock(-1);
    place_page_t *p = &pp[i];
    strlcpy(p->name, name, sizeof(p->name));
    lv_label_set_text(p->city, name);
    if (!w) {                                                  // no forecast yet for this place
        p->has_wx = false;
        lv_label_set_text(p->temp, "-");
        lv_label_set_text(p->cond, tr(T_LOADING));
        lv_obj_add_flag(p->detail, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->nowcast, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(p->icon, LV_OBJ_FLAG_HIDDEN);
        for (int k = 0; k < 3; k++) {
            lv_label_set_text(p->fc_day[k], "");
            lv_label_set_text(p->fc_temp[k], "");
            lv_obj_add_flag(p->fc_icon[k], LV_OBJ_FLAG_HIDDEN);
        }
        if (i == cur_place) {
            have_wx = false;                 // the hourly view and extras wait for this place's forecast
            if (lv_screen_active() == scr_hour || lv_screen_active() == scr_extras) lv_screen_load(scr_main);
        }
        display_unlock();
        return;
    }
    if (w != &pw[i]) pw[i] = *w;
    p->has_wx = true;
    p->utc_offset = w->utc_offset;
    char buf[64];
    snprintf(buf, sizeof(buf), "%d°", config_temp(w->temp));
    lv_label_set_text(p->temp, buf);
    lv_label_set_text(p->cond, weather_text(w->code));
    char wind[16];
    config_fmt_wind(w->wind, wind, sizeof(wind));
    lv_label_set_text_fmt(p->feels, tr(T_FEELS), config_temp(w->feels));
    lv_label_set_text_fmt(p->hum, tr(T_PERCENT), w->humidity);
    lv_label_set_text(p->wind, wind);
    lv_obj_remove_flag(p->detail, LV_OBJ_FLAG_HIDDEN);
    draw_icon(p->icon, weather_kind(w->code), w->is_day, 80, i * 4);
    lv_obj_remove_flag(p->icon, LV_OBJ_FLAG_HIDDEN);
    if (w->nc_kind == NC_NONE) lv_obj_add_flag(p->nowcast, LV_OBJ_FLAG_HIDDEN);
    else {
        char hm[12];
        config_fmt_hhmm(w->nc_time, hm, sizeof(hm));
        lv_label_set_text_fmt(p->nowcast, tr(w->nc_snow ? (w->nc_kind == NC_STARTS ? T_SNOW_AROUND : T_SNOW_UNTIL)
                                                        : (w->nc_kind == NC_STARTS ? T_RAIN_AROUND : T_RAIN_UNTIL)), hm);
        lv_obj_remove_flag(p->nowcast, LV_OBJ_FLAG_HIDDEN);
    }
    for (int k = 0; k < 3; k++) {
        if (k < w->ndays) {
            day_name(w->day[k].date, k, buf, sizeof(buf));
            lv_label_set_text(p->fc_day[k], buf);
            snprintf(buf, sizeof(buf), "%d° / %d°", config_temp(w->day[k].tmax), config_temp(w->day[k].tmin));
            lv_label_set_text(p->fc_temp[k], buf);
            draw_icon(p->fc_icon[k], weather_kind(w->day[k].code), true, 36, i * 4 + k + 1);
            lv_obj_remove_flag(p->fc_icon[k], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (i == cur_place) {                    // the hourly view, extras and graphs show this place
        wx = *w;
        have_wx = true;
        wx_gen++;
        if (lv_screen_active() == scr_extras) extras_refresh();
        if (lv_screen_active() == scr_hour) for (int d = 0; d < WX_DAYS; d++) hour_fill(d);
        if (lv_screen_active() == scr_msg) lv_screen_load(scr_main);
    }
    clock_shown[0] = 0;
    clock_tick(NULL);
    display_unlock();
}

void ui_message(const char *title, const char *body)
{
    display_lock(-1);
    lv_label_set_text(msg_title, title);
    lv_label_set_text(msg_body, body);
    lv_obj_add_flag(msg_qr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(msg_title, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_align(msg_body, LV_ALIGN_TOP_MID, 0, 190);
    lv_screen_load(scr_msg);
    display_unlock();
}


// Diagnostics bench: the screens to time (hourly view filled with day 1). Call with the display lock held.
int ui_bench_screens(lv_obj_t **scr, const char **name, int max)
{
    int n = 0;
    static lv_obj_t *blank;
    if (!blank) blank = base_screen();
    if (n < max) { scr[n] = blank; name[n++] = "blank"; }
    if (n < max) { scr[n] = scr_main; name[n++] = "weather"; }
    if (have_wx && n < max && lv_screen_active() != scr_hour) {   // never disturb a view in use
        for (int i = 0; i < WX_DAYS; i++) fill_page(i);
        pager_go(hr_pager, 1, false);
        set_dots(1);
        scr[n] = scr_hour; name[n++] = "hourly";
    }
    if (n < max) { scr[n] = scr_radar; name[n++] = "radar"; }
    return n;
}

lv_obj_t *ui_main_screen(void) { return scr_main; }

void ui_on_data_refresh(void (*cb)(void)) { data_refresh_cb = cb; }

void ui_units_changed(void)
{
    display_lock(-1);
    clock_shown[0] = 0;
    for (int i = 0; i < tl_n; i++) lv_label_set_text(tl_obj[i], tr(tl_id[i]));   // fixed labels (language)
    up_notes_id = -1;                      // "What's new" header
    update_render();
    if (lv_screen_active() == scr_status) status_refresh();
    if (lv_screen_active() == scr_cfg) cfg_refresh();
    for (int i = 0; i < n_places; i++)     // every place page; the one shown also redraws hourly, graphs, extras
        if (pp[i].has_wx) ui_place(i, pp[i].name, &pw[i]);
    clock_tick(NULL);
    ui_alerts(&alerts);                    // "Until …"
    radar_units_changed();                 // clock, frame time, ring and radius
    display_unlock();
}

lv_draw_buf_t *ui_snapshot(const char *screen)
{
    lv_obj_t *s = !strcmp(screen, "weather") ? scr_main : !strcmp(screen, "extras") ? scr_extras :
                  !strcmp(screen, "status") ? scr_status : !strcmp(screen, "radar") ? scr_radar :
                  !strcmp(screen, "update") ? scr_update : !strcmp(screen, "alert") ? scr_alert :
                  !strcmp(screen, "settings") ? scr_cfg : lv_screen_active();
    if (!strncmp(screen, "hourly", 6) && have_wx) {
        s = scr_hour;
        if (lv_screen_active() != scr_hour) {
            int day = atoi(screen + 6);
            if (day < 0 || day >= wx.ndays) day = 0;
            for (int i = 0; i < WX_DAYS; i++) { fill_page(i); lv_obj_scroll_to_y(pg[i].list, 0, LV_ANIM_OFF); }
            pager_go(hr_pager, day, false);
            set_dots(day);
        }
    }
    // Text-fit checks of screens a test can't open safely: "settings1".."settings3" (the list scrolled down by
    // one screen each), "phone" (the settings-page QR), "setup0" / "setup1" (Wi-Fi setup pages, texts only:
    // no access point or Easy Connect is started).
    int cfg_down = !strncmp(screen, "settings", 8) && screen[8] ? atoi(screen + 8) : 0;
    bool phone = !strcmp(screen, "phone"), setup = !strncmp(screen, "setup", 5);
    bool ov_hidden = lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    if (cfg_down) s = scr_cfg;
    if (phone) {
        s = overlay;
        lv_label_set_text(ov_title, tr(T_SETTINGS));
        lv_label_set_text_fmt(ov_url, tr(T_OV_HELP), "https://192.168.1.10");
        lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (setup && lv_screen_active() != scr_setup) {
        s = scr_setup;
        bool p1 = screen[5] == '1';
        su_page = p1;
        su_dots();                                   // sizes the page dots
        lv_label_set_text(su_title, tr(p1 ? T_WIFI_DPP_TITLE : T_WIFI_SETUP));
        if (p1) lv_label_set_text(su_body, tr(T_WIFI_DPP_HOW));
        else lv_label_set_text_fmt(su_body, tr(T_WIFI_JOIN), SETUP_AP_SSID, SETUP_AP_PASS);
    }
    if (s == scr_status) status_refresh();
    if (s == scr_cfg) cfg_refresh();
    if (s == scr_extras) extras_refresh();
    lv_obj_t *list = lv_obj_get_parent(cfg_row[0]);
    if (cfg_down && lv_screen_active() != scr_cfg) lv_obj_scroll_to_y(list, cfg_down * 300, LV_ANIM_OFF);
    lv_obj_update_layout(s);
    lv_draw_buf_t *db = lv_snapshot_take(s, LV_COLOR_FORMAT_RGB565);
    if (cfg_down && lv_screen_active() != scr_cfg) lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    if (phone && ov_hidden) lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    return db;
}
