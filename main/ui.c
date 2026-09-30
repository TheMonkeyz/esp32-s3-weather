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
#include "esp_log.h"
#include "esp_attr.h"

extern const uint8_t ttf_start[] asm("_binary_montserrat_ttf_start");
extern const uint8_t ttf_end[]   asm("_binary_montserrat_ttf_end");

static lv_font_t *f_time, *f_city, *f_big, *f_cond, *f_small, *f_tiny, *f_micro;
static lv_obj_t *scr_radar, *scr_extras;
static void extras_refresh(void);
static lv_obj_t *scr_msg, *msg_title, *msg_body, *msg_qr;
static lv_obj_t *overlay, *ov_qr, *ov_url, *ov_title;
static int ov_state;          // 0 hidden, 1 settings QR
static lv_obj_t *scr_hour;       // hourly detail screen
static int hr_day;
static void hour_fill(int day);
static lv_obj_t *al_pill, *al_pill_lbl, *scr_alert, *al_title, *al_sub, *al_body, *al_map;
static lv_image_dsc_t al_map_dsc;
static uint16_t *al_map_buf;
static EXT_RAM_BSS_ATTR alerts_t alerts;           // ~4 KB, in PSRAM
static lv_obj_t *scr_main, *lbl_time, *lbl_city, *icon_box, *lbl_temp, *lbl_cond, *lbl_detail, *lbl_nowcast;
static lv_obj_t *fc_day[3], *fc_temp[3], *fc_icon[3], *hero;

#define C_BG      lv_color_hex(0x000000)
#define C_TEXT    lv_color_hex(0xF2F4F7)
#define C_DIM     lv_color_hex(0x8B95A1)
#define C_ACCENT  lv_color_hex(0x5AB0FF)

static lv_font_t *mkfont(int px)
{
    return lv_tiny_ttf_create_data_ex(ttf_start, ttf_end - ttf_start, px, LV_FONT_KERNING_NONE, 96);
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

static lv_point_precise_t bolt_pts[4][4];

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

static void clock_tick(lv_timer_t *t)
{
    static char shown[8];
    struct tm tm;
    if (!config_local_time((long)time(NULL), &tm)) return;   // not synced yet
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &tm);
    if (strcmp(buf, shown)) {
        strcpy(shown, buf);
        lv_label_set_text(lbl_time, buf);
        ESP_LOGI("ui", "clock %s", buf);
        if (lv_screen_active() == scr_hour && buf[3] == '0' && buf[4] == '0') hour_fill(0);   // new hour
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
    lv_label_set_text(ov_title, "Settings");
    lv_qrcode_update(ov_qr, url, strlen(url));
    lv_label_set_text_fmt(ov_url, "%s\nScan with your phone and accept\nthe certificate warning.\n\nLong-press for Wi-Fi setup\nTap to close", url);
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

static void page_dots(lv_obj_t *scr, int active)   // 0 extras, 1 weather, 2 radar
{
    for (int i = 0; i < 3; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, i == active ? 18 : 7, 7);
        lv_obj_set_style_radius(d, 4, 0);
        lv_obj_set_style_bg_color(d, i == active ? C_TEXT : C_DIM, 0);
        lv_obj_set_style_bg_opa(d, i == active ? LV_OPA_COVER : LV_OPA_60, 0);
        lv_obj_align(d, LV_ALIGN_BOTTOM_MID, (i - 1) * 16 + (i < active ? -5 : i > active ? 5 : 0), -12);
    }
}

/* ---------- Hourly detail (tap a forecast day) ----------
 * Three day pages side by side in a horizontal scroller that snaps one page at a time,
 * so the pages follow the finger. Each page's hour list scrolls vertically and is drawn
 * by one draw callback (no per-row objects). */

#define ROW_H   46
#define LIST_W  316
typedef struct {
    lv_obj_t *page, *title, *sum, *list, *content;
    int first, count, now;           // hour indexes into wx.hour
} day_page_t;
static day_page_t pg[WX_DAYS];
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

// Draws only the rows inside the clip area
static void hr_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    day_page_t *dp = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(o, &c);
    const lv_area_t *clip = &layer->_clip_area;
    char buf[16];
    for (int r = 0; r < dp->count; r++) {
        int y = c.y1 + r * ROW_H;
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
        if (now) strcpy(buf, "Now"); else snprintf(buf, sizeof(buf), "%02d:00", i % 24);
        draw_text(layer, f_tiny, now ? C_ACCENT : C_DIM, c.x1 + 10, y, 64, LV_TEXT_ALIGN_LEFT, buf);
        P_layer = layer; P_x = c.x1 + 76; P_y = y + (ROW_H - 37) / 2;
        draw_icon(NULL, weather_kind(h->code), h->is_day, 30, 0);
        P_layer = NULL;
        snprintf(buf, sizeof(buf), "%d°", (int)(h->temp < 0 ? h->temp - 0.5f : h->temp + 0.5f));
        draw_text(layer, f_small, C_TEXT, c.x1 + 118, y, 56, LV_TEXT_ALIGN_RIGHT, buf);
        snprintf(buf, sizeof(buf), "%d%%", h->pop);
        draw_text(layer, f_tiny, h->pop >= 30 ? C_ACCENT : C_DIM, c.x1 + 180, y, 54, LV_TEXT_ALIGN_RIGHT, buf);
        snprintf(buf, sizeof(buf), "%.0f km/h", h->wind);
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
    char name[16] = "-";
    if (sscanf(d->date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
        tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_hour = 12;
        mktime(&tm);
        strftime(name, sizeof(name), "%A", &tm);        // weekday name, today included
    }
    lv_label_set_text(dp->title, name);
    lv_label_set_text_fmt(dp->sum, "%s  ·  %.0f° / %.0f°", weather_text(d->code), d->tmax, d->tmin);
    lv_obj_set_height(dp->content, dp->count * ROW_H);
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
    lv_obj_update_layout(hr_pager);
    lv_obj_scroll_to_x(hr_pager, col * DISP_W, LV_ANIM_OFF);
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

static void pager_scrolled(lv_event_t *e)
{
    int x = lv_obj_get_scroll_x(hr_pager);
    int day = (x + DISP_W / 2) / DISP_W;
    if (day < 0) day = 0;
    if (day > WX_DAYS - 1) day = WX_DAYS - 1;
    if (day != hr_day) set_dots(day);
    if (lv_event_get_code(e) == LV_EVENT_SCROLL_END)
        ESP_LOGI("ui", "hourly view: day %d, %d rows", day, pg[day].count);
}

static void hour_create(void)
{
    scr_hour = base_screen();
    hr_pager = lv_obj_create(scr_hour);
    lv_obj_remove_style_all(hr_pager);
    lv_obj_set_size(hr_pager, DISP_W, DISP_H);
    lv_obj_set_scroll_dir(hr_pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(hr_pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_add_flag(hr_pager, LV_OBJ_FLAG_SCROLL_ONE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_scrollbar_mode(hr_pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(hr_pager, pager_scrolled, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(hr_pager, pager_scrolled, LV_EVENT_SCROLL_END, NULL);

    for (int d = 0; d < WX_DAYS; d++) {
        day_page_t *dp = &pg[d];
        dp->page = lv_obj_create(hr_pager);
        lv_obj_remove_style_all(dp->page);
        lv_obj_set_size(dp->page, DISP_W, DISP_H);
        lv_obj_set_pos(dp->page, d * DISP_W, 0);
        lv_obj_remove_flag(dp->page, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(dp->page, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        dp->title = label(dp->page, f_city, C_ACCENT, 34);
        dp->sum = label(dp->page, f_tiny, C_DIM, 68);
        lv_obj_set_width(dp->sum, 340);
        lv_label_set_long_mode(dp->sum, LV_LABEL_LONG_DOT);

        // Column headers, aligned with the columns drawn in hr_draw()
        static const struct { int x, w; const char *t; } hdr[] = {
            { 118, 56, "Temp" }, { 180, 54, "Rain" }, { 236, 72, "Wind" },
        };
        for (int i = 0; i < 3; i++) {
            lv_obj_t *h = label(dp->page, f_micro, i == 1 ? C_ACCENT : C_DIM, 0);
            lv_obj_set_width(h, hdr[i].w);
            lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_text(h, hdr[i].t);
            lv_obj_align(h, LV_ALIGN_TOP_LEFT, (DISP_W - LIST_W) / 2 + hdr[i].x, 100);
        }

        dp->list = lv_obj_create(dp->page);
        lv_obj_remove_style_all(dp->list);
        lv_obj_set_size(dp->list, LIST_W, 290);
        lv_obj_align(dp->list, LV_ALIGN_TOP_MID, 0, 120);
        lv_obj_set_scroll_dir(dp->list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(dp->list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(dp->list, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

        dp->content = lv_obj_create(dp->list);
        lv_obj_remove_style_all(dp->content);
        lv_obj_set_size(dp->content, LIST_W, ROW_H);
        lv_obj_remove_flag(dp->content, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(dp->content, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(dp->content, hr_draw, LV_EVENT_DRAW_MAIN, dp);
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
    printf("ui: gesture dir=%d on %s\n", dir, cur == scr_radar ? "radar" : "main");
    if (cur == scr_main && dir == LV_DIR_RIGHT) {
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
static bool su_can_close;           // opened while online: a tap closes it
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
        lv_label_set_text(su_title, "Wi-Fi received");
        lv_label_set_text_fmt(su_body, "Got \"%s\" from your phone.\nRestarting...", ssid);
        lv_obj_add_flag(su_qr, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(su_body, "That didn't work. Scan again,\nor swipe right for other phones.");
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
        lv_label_set_text(su_title, "Wi-Fi setup");
        lv_qrcode_update(su_qr, su_ap_qr, strlen(su_ap_qr));
        lv_obj_remove_flag(su_qr, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(su_body, "Scan to join " SETUP_AP_SSID "\n(password " SETUP_AP_PASS ").\n"
                                   "Android? Swipe left to skip\nthe password");
    } else {
        lv_label_set_text(su_title, "Android: Easy Connect");
        lv_obj_add_flag(su_qr, LV_OBJ_FLAG_HIDDEN);          // until the code is generated
        lv_label_set_text(su_body, "In Wi-Fi settings, tap the QR icon\nand scan this.\n"
                                   "Your phone sends its network.\nSwipe right for other phones");
        net_setup_ap_stop_any();
        if (!net_dpp_start(su_dpp_uri, su_dpp_done))
            lv_label_set_text(su_body, "Easy Connect isn't available.\nSwipe right for other phones.");
    }
    ESP_LOGI("ui", "Wi-Fi setup page %d (%s)", page, page ? "Easy Connect" : "setup network");
}

static void su_close(void)
{
    ESP_LOGI("ui", "Wi-Fi setup closed");
    if (su_timer) { lv_timer_delete(su_timer); su_timer = NULL; }
    net_dpp_stop();
    net_setup_ap_stop();
    lv_screen_load_anim(scr_main, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

static void su_timeout(lv_timer_t *t)
{
    if (!su_can_close && !net_is_connected()) return;   // still offline: keep offering setup
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
    su_can_close = net_is_connected() && !net_in_portal();
    lv_label_set_text(su_note, su_note_text[0] ? su_note_text : su_can_close ? "Tap to cancel" : "");
    su_show_page(0);
    if (su_timer) lv_timer_delete(su_timer);
    su_timer = lv_timer_create(su_timeout, 10 * 60 * 1000, NULL);   // closes after 10 min once online
    if (lv_screen_active() != scr_setup) lv_screen_load(scr_setup);
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);                              // the long-press isn't also a tap
    display_unlock();
}

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
    if (t && config_local_time((long)t, &tm)) strftime(out, n, "Until %a %H:%M", &tm);
    else out[0] = 0;
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

void ui_alerts(const alerts_t *al)
{
    display_lock(-1);
    alerts = *al;
    if (!alerts.n) {
        lv_obj_add_flag(al_map, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(al_pill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lbl_city, LV_OBJ_FLAG_HIDDEN);
        if (lv_screen_active() == scr_alert) lv_screen_load(scr_main);
        display_unlock();
        return;
    }
    const alert_t *a = &alerts.a[0];
    lv_color_t c = alert_colour(a->colour);
    lv_obj_set_style_bg_color(al_pill, c, 0);
    lv_obj_set_style_text_color(al_pill_lbl, a->colour == 'r' ? lv_color_white() : lv_color_black(), 0);
    if (alerts.n > 1) lv_label_set_text_fmt(al_pill_lbl, "%s +%d", a->name, alerts.n - 1);
    else lv_label_set_text(al_pill_lbl, a->name);
    lv_obj_remove_flag(al_pill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_city, LV_OBJ_FLAG_HIDDEN);

    char until[32];
    fmt_until(a->ends, until, sizeof(until));
    lv_label_set_text(al_title, a->name);
    lv_obj_set_style_text_color(al_title, c, 0);
    lv_label_set_text_fmt(al_sub, "%s%s%s", until, until[0] && a->area[0] ? "\n" : "", a->area);
    lv_obj_scroll_to_y(lv_obj_get_parent(al_body), 0, LV_ANIM_OFF);
    static EXT_RAM_BSS_ATTR char body[ALERTS_MAX * 960];
    int len = snprintf(body, sizeof(body), "%s", a->text);
    for (int i = 1; i < alerts.n && len < (int)sizeof(body); i++) {
        fmt_until(alerts.a[i].ends, until, sizeof(until));
        len += snprintf(body + len, sizeof(body) - len, "\n\nAlso: %s (%s)\n\n%s", alerts.a[i].name, until, alerts.a[i].text);
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
    static const char *names[8] = { "New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
                                    "Full moon", "Waning gibbous", "Last quarter", "Waning crescent" };
    return names[(int)floor(age / syn * 8 + 0.5) % 8];
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
    if (synced) {                                        // "Wednesday, September 30"
        strftime(buf, sizeof(buf), "%A, %B", &tm);
        lv_label_set_text_fmt(ex_date, "%s %d", buf, tm.tm_mday);
    }

    // Sun
    int rise = have_wx ? hhmm(wx.day[0].sunrise) : -1, set = have_wx ? hhmm(wx.day[0].sunset) : -1;
    int cur = synced ? tm.tm_hour * 60 + tm.tm_min : -1;
    if (rise >= 0 && set > rise) {
        lv_label_set_text_fmt(ex_rise, "%s", wx.day[0].sunrise);
        lv_label_set_text_fmt(ex_set, "%s", wx.day[0].sunset);
        int len = set - rise;
        if (cur >= rise && cur <= set) {
            float p = (float)(cur - rise) / len;
            lv_arc_set_value(ex_arc, (int)(p * 1000));
            float th = (180 + 180 * p) * M_PI / 180;
            lv_obj_set_pos(ex_sun, ARC_CX + ARC_R * cosf(th) - 11, ARC_CY + ARC_R * sinf(th) - 11);
            lv_obj_remove_flag(ex_sun, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text_fmt(ex_day, "Daylight\n%d h %02d", len / 60, len % 60);
        } else {
            lv_arc_set_value(ex_arc, 0);                      // night: the whole arc dim
            lv_obj_add_flag(ex_sun, LV_OBJ_FLAG_HIDDEN);
            const char *next = cur > set && wx.ndays > 1 ? wx.day[1].sunrise : wx.day[0].sunrise;
            lv_label_set_text_fmt(ex_day, "Sunrise\n%s", next);
        }
    }

    // UV
    if (have_wx) {
        float uv = wx.uv;
        const char *lvl = uv < 3 ? "Low" : uv < 6 ? "Moderate" : uv < 8 ? "High" : uv < 11 ? "Very high" : "Extreme";
        lv_color_t c = lv_color_hex(uv < 3 ? 0x6FD08C : uv < 6 ? 0xFFC83D : uv < 8 ? 0xFF8A3D : uv < 11 ? 0xFF4D4D : 0xC77DFF);
        snprintf(buf, sizeof(buf), "%.0f  %s  (max %.0f)", uv, lvl, wx.day[0].uv_max);
        ex_row(0, "UV index", buf, c);
    }

    // Moon
    int illum;
    const char *ph = moon_phase(now, &illum);
    snprintf(buf, sizeof(buf), "%s  %d%%", ph, illum);
    ex_row(1, "Moon", buf, C_TEXT);
    lv_obj_invalidate(ex_moon);

    // Air quality (US AQI, CAMS global)
    if (have_air && ex_air.us_aqi >= 0) {
        int q = ex_air.us_aqi;
        const char *lvl = q <= 50 ? "Good" : q <= 100 ? "Moderate" : q <= 150 ? "Sensitive groups" : q <= 200 ? "Unhealthy" :
                          q <= 300 ? "Very unhealthy" : "Hazardous";
        lv_color_t c = lv_color_hex(q <= 50 ? 0x6FD08C : q <= 100 ? 0xFFC83D : q <= 150 ? 0xFF8A3D : q <= 200 ? 0xFF4D4D : 0xC77DFF);
        snprintf(buf, sizeof(buf), "%s  %d", lvl, q);
        ex_row(2, "Air quality", buf, c);
    } else ex_row(2, "Air quality", "-", C_DIM);

    // Pollen (Europe only): the strongest type
    static const char *pn[4] = { "Alder", "Birch", "Grass", "Ragweed" };
    int best = -1;
    for (int i = 0; i < 4; i++) if (ex_air.pollen[i] >= 0 && (best < 0 || ex_air.pollen[i] > ex_air.pollen[best])) best = i;
    if (have_air && best >= 0) {
        float v = ex_air.pollen[best];
        const char *lvl = v < 10 ? "Low" : v < 50 ? "Moderate" : v < 200 ? "High" : "Very high";
        snprintf(buf, sizeof(buf), "%s  %s", pn[best], lvl);
        ex_row(3, "Pollen", buf, lv_color_hex(v < 10 ? 0x6FD08C : v < 50 ? 0xFFC83D : v < 200 ? 0xFF8A3D : 0xFF4D4D));
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
        lv_obj_set_width(ex_key[i], 110);
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
    page_dots(scr_extras, 0);
    lv_obj_add_event_cb(scr_extras, gesture_cb, LV_EVENT_GESTURE, NULL);
    extras_refresh();
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
    lbl_time = label(scr_main, f_time, C_DIM, 38);
    lbl_city = label(scr_main, f_city, C_TEXT, 72);
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
    lv_label_set_text(lbl_city, loc.name);

    // Hero row: icon + big temperature, centred together
    hero = lv_obj_create(scr_main);
    lv_obj_remove_style_all(hero);
    lv_obj_set_size(hero, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, 14, 0);
    lv_obj_align(hero, LV_ALIGN_TOP_MID, 0, 112);
    icon_box = icon_box_create(hero, 80);
    lbl_temp = lv_label_create(hero);
    lv_obj_set_style_text_font(lbl_temp, f_big, 0);
    lv_obj_set_style_text_color(lbl_temp, C_TEXT, 0);
    lv_label_set_text(lbl_temp, "");

    lbl_cond = label(scr_main, f_cond, C_TEXT, 214);
    lbl_detail = label(scr_main, f_small, C_DIM, 248);
    lbl_nowcast = label(scr_main, f_tiny, C_ACCENT, 273);         // "Rain around 14:45" (hidden when none)
    lv_obj_add_flag(lbl_nowcast, LV_OBJ_FLAG_HIDDEN);

    // Divider
    lv_obj_t *div = lv_obj_create(scr_main);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 260, 2);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x2A3138), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 298);

    // 3-day forecast: day / icon / high-low
    for (int i = 0; i < 3; i++) {
        int dx = (i - 1) * 98;
        fc_day[i] = label(scr_main, f_tiny, C_ACCENT, 0);
        lv_obj_set_width(fc_day[i], 96);
        lv_obj_align(fc_day[i], LV_ALIGN_TOP_MID, dx, 306);
        fc_icon[i] = icon_box_create(scr_main, 36);
        lv_obj_align(fc_icon[i], LV_ALIGN_TOP_MID, dx, 334);
        fc_temp[i] = label(scr_main, f_tiny, C_TEXT, 0);
        lv_obj_set_width(fc_temp[i], 96);
        lv_obj_align(fc_temp[i], LV_ALIGN_TOP_MID, dx, 384);
    }
    page_dots(scr_main, 1);

    scr_radar = radar_create(f_small, f_small, f_micro);
    page_dots(scr_radar, 2);
    lv_obj_add_event_cb(scr_main, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr_main, show_settings, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(scr_main, main_tap, LV_EVENT_SHORT_CLICKED, NULL);
    hour_create();
    passthrough(scr_main);          // before the (clickable) overlay is added

    overlay = lv_obj_create(scr_main);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, DISP_W, DISP_H);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, overlay_close, LV_EVENT_CLICKED, NULL);
    ov_title = label(overlay, f_small, C_ACCENT, 48);
    lv_label_set_text(ov_title, "Settings");
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

void ui_set_city(const char *name)
{
    display_lock(-1);
    lv_label_set_text(lbl_city, name);
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

static void day_name(const char *date, int idx, char *out, size_t n)
{
    if (idx == 0) { snprintf(out, n, "Today"); return; }
    struct tm tm = {0};
    if (sscanf(date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
        tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_hour = 12;
        mktime(&tm);
        strftime(out, n, "%a", &tm);
    } else snprintf(out, n, "-");
}

void ui_weather(const weather_t *w)
{
    char buf[64];
    display_lock(-1);
    snprintf(buf, sizeof(buf), "%d°", (int)(w->temp < 0 ? w->temp - 0.5 : w->temp + 0.5));
    lv_label_set_text(lbl_temp, buf);
    lv_label_set_text(lbl_cond, weather_text(w->code));
    snprintf(buf, sizeof(buf), "Feels %.0f°  ·  %d%%  ·  %.0f km/h", w->feels, w->humidity, w->wind);
    lv_label_set_text(lbl_detail, buf);
    draw_icon(icon_box, weather_kind(w->code), w->is_day, 80, 0);
    if (w->nc_kind == NC_NONE) lv_obj_add_flag(lbl_nowcast, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_label_set_text_fmt(lbl_nowcast, w->nc_kind == NC_STARTS ? "%s around %s" : "%s until about %s",
                              w->nc_snow ? "Snow" : "Rain", w->nc_time);
        lv_obj_remove_flag(lbl_nowcast, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 3; i++) {
        if (i < w->ndays) {
            day_name(w->day[i].date, i, buf, sizeof(buf));
            lv_label_set_text(fc_day[i], buf);
            snprintf(buf, sizeof(buf), "%.0f° / %.0f°", w->day[i].tmax, w->day[i].tmin);
            lv_label_set_text(fc_temp[i], buf);
            draw_icon(fc_icon[i], weather_kind(w->day[i].code), true, 36, i + 1);
        }
    }
    wx = *w;
    have_wx = true;
    if (lv_screen_active() == scr_extras) extras_refresh();
    if (lv_screen_active() == scr_hour) for (int i = 0; i < WX_DAYS; i++) hour_fill(i);
    clock_tick(NULL);
    if (lv_screen_active() == scr_msg) lv_screen_load(scr_main);
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
        lv_obj_update_layout(hr_pager);
        lv_obj_scroll_to_x(hr_pager, DISP_W, LV_ANIM_OFF);
        set_dots(1);
        scr[n] = scr_hour; name[n++] = "hourly";
    }
    if (n < max) { scr[n] = scr_radar; name[n++] = "radar"; }
    return n;
}

lv_obj_t *ui_main_screen(void) { return scr_main; }
