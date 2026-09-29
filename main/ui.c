// Round-screen weather UI (LVGL v9 + TinyTTF Montserrat)
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "display.h"
#include "weather.h"
#include "touch.h"
#include "radar.h"
#include "config.h"
#include "net.h"
#include "esp_log.h"

extern const uint8_t ttf_start[] asm("_binary_montserrat_ttf_start");
extern const uint8_t ttf_end[]   asm("_binary_montserrat_ttf_end");

static lv_font_t *f_time, *f_city, *f_big, *f_cond, *f_small, *f_tiny, *f_micro;
static lv_obj_t *scr_radar;
static lv_obj_t *scr_msg, *msg_title, *msg_body, *msg_qr;
static lv_obj_t *overlay, *ov_qr, *ov_url;
static lv_obj_t *scr_main, *lbl_time, *lbl_city, *icon_box, *lbl_temp, *lbl_cond, *lbl_detail;
static lv_obj_t *fc_day[3], *fc_temp[3], *fc_icon[3], *hero;

#define C_BG      lv_color_hex(0x000000)
#define C_TEXT    lv_color_hex(0xF2F4F7)
#define C_DIM     lv_color_hex(0x8B95A1)
#define C_ACCENT  lv_color_hex(0x5AB0FF)

static lv_font_t *mkfont(int px)
{
    return lv_tiny_ttf_create_data_ex(ttf_start, ttf_end - ttf_start, px, LV_FONT_KERNING_NORMAL, 96);
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

static lv_obj_t *blob(lv_obj_t *p, int x, int y, int w, int h, lv_color_t c, int radius)
{
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
    lv_obj_set_style_shadow_color(s, lv_color_hex(0xFFB000), 0);
    lv_obj_set_style_shadow_width(s, SC(30), 0);
    lv_obj_set_style_shadow_opa(s, LV_OPA_40, 0);
}

static void moon(lv_obj_t *p, int x, int y, int d)
{
    blob(p, x, y, d, d, lv_color_hex(0xE8E6F2), LV_RADIUS_CIRCLE);
    blob(p, x + d / 3, y - d / 8, d, d, C_BG, LV_RADIUS_CIRCLE);
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
    lv_obj_clean(box);
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
    }
}

/* Settings overlay (long-press on the weather screen) */
static uint32_t overlay_opened;
static void overlay_close(lv_event_t *e)
{
    if (lv_tick_elaps(overlay_opened) < 800) return;   // ignore the release of the long-press itself
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

static void show_settings(lv_event_t *e)
{
    ESP_LOGI("ui", "long press -> settings QR");
    char ip[20], url[48];
    if (!net_get_ip(ip, sizeof(ip))) strcpy(ip, "192.168.4.1");
    snprintf(url, sizeof(url), "https://%s", ip);
    lv_qrcode_update(ov_qr, url, strlen(url));
    lv_label_set_text_fmt(ov_url, "%s\n\nScan with your phone.\nAccept the certificate warning.\nTap to close", url);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
    overlay_opened = lv_tick_get();
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);                  // this touch shouldn't also close it
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

static void page_dots(lv_obj_t *scr, int active)
{
    for (int i = 0; i < 2; i++) {
        lv_obj_t *d = lv_obj_create(scr);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, i == active ? 18 : 7, 7);
        lv_obj_set_style_radius(d, 4, 0);
        lv_obj_set_style_bg_color(d, i == active ? C_TEXT : C_DIM, 0);
        lv_obj_set_style_bg_opa(d, i == active ? LV_OPA_COVER : LV_OPA_60, 0);
        lv_obj_align(d, LV_ALIGN_BOTTOM_MID, i == 0 ? -10 : 10, -12);
    }
}

static void gesture_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(in);
    lv_obj_t *cur = lv_screen_active();
    LV_LOG_USER("gesture dir %d", dir);
    printf("ui: gesture dir=%d on %s\n", dir, cur == scr_radar ? "radar" : "main");
    if (cur == scr_main && dir == LV_DIR_LEFT) {
        radar_set_visible(true);
        lv_screen_load_anim(scr_radar, LV_SCR_LOAD_ANIM_MOVE_LEFT, 280, 0, false);
        lv_indev_wait_release(in);
    } else if (cur == scr_radar && dir == LV_DIR_RIGHT) {
        radar_set_visible(false);
        lv_screen_load_anim(scr_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 280, 0, false);
        lv_indev_wait_release(in);
    }
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

    lbl_cond = label(scr_main, f_cond, C_TEXT, 220);
    lbl_detail = label(scr_main, f_small, C_DIM, 256);

    // Divider
    lv_obj_t *div = lv_obj_create(scr_main);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 260, 2);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x2A3138), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 294);

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
    page_dots(scr_main, 0);

    scr_radar = radar_create(f_small, f_small, f_micro);
    page_dots(scr_radar, 1);
    lv_obj_add_event_cb(scr_main, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr_main, show_settings, LV_EVENT_LONG_PRESSED, NULL);
    passthrough(scr_main);          // before the (clickable) overlay is added

    overlay = lv_obj_create(scr_main);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, DISP_W, DISP_H);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, overlay_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ot = label(overlay, f_small, C_ACCENT, 48);
    lv_label_set_text(ot, "Settings");
    ov_qr = make_qr(overlay, 170);
    lv_obj_align(ov_qr, LV_ALIGN_TOP_MID, 0, 84);
    ov_url = label(overlay, f_tiny, C_TEXT, 276);
    lv_obj_set_width(ov_url, 330);

    msg_qr = make_qr(scr_msg, 140);
    lv_obj_align(msg_qr, LV_ALIGN_TOP_MID, 0, 250);
    lv_obj_add_flag(msg_qr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(scr_radar, gesture_cb, LV_EVENT_GESTURE, NULL);
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
    for (int i = 0; i < 3; i++) {
        if (i < w->ndays) {
            day_name(w->day[i].date, i, buf, sizeof(buf));
            lv_label_set_text(fc_day[i], buf);
            snprintf(buf, sizeof(buf), "%.0f° / %.0f°", w->day[i].tmax, w->day[i].tmin);
            lv_label_set_text(fc_temp[i], buf);
            draw_icon(fc_icon[i], weather_kind(w->day[i].code), true, 36, i + 1);
        }
    }
    clock_tick(NULL);
    if (lv_screen_active() == scr_msg) lv_screen_load(scr_main);
    display_unlock();
}
