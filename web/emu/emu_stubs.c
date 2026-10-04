// What the screens ask of the rest of the firmware, answered for the browser: online, no setup network, no updates,
// no speaker or microphones. The service statuses are recorded as the firmware's are (svc_http), for the status page.
#include <stdio.h>
#include <string.h>
#include <emscripten.h>
#include "esp_timer.h"
#include "esp_err.h"
#include "net.h"
#include "ota.h"
#include "svc.h"
#include "sound.h"
#include "presence.h"
#include "web.h"
#include "alerts.h"
#include "display.h"

const char *esp_err_to_name(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return "ESP_OK";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_HTTP_CONNECT: return "ESP_ERR_HTTP_CONNECT";
    default: return "ESP_FAIL";
    }
}

int64_t esp_timer_get_time(void) { return (int64_t)(emscripten_get_now() * 1000.0); }

/* ---------- network: the browser's ---------- */
const char *net_setup_ap_pass(void) { return ""; }
bool net_is_connected(void) { return EM_ASM_INT({ return navigator.onLine ? 1 : 0; }); }
bool net_get_ip(char *out, size_t n) { snprintf(out, n, "browser"); return true; }
bool net_get_ssid(char *out, size_t n) { snprintf(out, n, "browser"); return true; }
bool net_in_portal(void) { return false; }
int net_ap_clients(void) { return 0; }
void net_setup_ap_start(void) {}
void net_setup_ap_stop(void) {}
void net_setup_ap_stop_any(void) {}
bool net_setup_ap_active(void) { return false; }
bool net_dpp_start(net_dpp_uri_cb_t on_uri, net_dpp_done_cb_t on_done) { (void)on_uri; (void)on_done; return false; }
void net_dpp_stop(void) {}
bool net_dpp_active(void) { return false; }

/* ---------- updates: none in the browser ---------- */
void ota_get_status(ota_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = OTA_UP_TO_DATE;
    snprintf(out->current, sizeof(out->current), "%s", EMU_VERSION);
    snprintf(out->channel, sizeof(out->channel), "stable");
}
void ota_check_now(void) {}
bool ota_install(void) { return false; }
void ota_get_notes(char *out, size_t size) { if (size) out[0] = 0; }
void ota_restart_when_safe(void) {}

/* ---------- service statuses ---------- */
static svc_info_t svc[SVC_COUNT] = {
    [SVC_FORECAST] = { "Open-Meteo", "Forecast API v1" },
    [SVC_AIR]      = { "Open-Meteo air", "Air quality API v1" },
    [SVC_ALERTS]   = { "EC alerts", "OGC API" },
    [SVC_RADAR]    = { "EC GeoMet radar", "WMS 1.3.0" },
    [SVC_TILES]    = { "OpenStreetMap", "tiles" },
    [SVC_UPDATES]  = { "GitHub Pages", "updates" },
    [SVC_NTP]      = { "Browser clock", "" },
};
void svc_http(svc_id_t id, esp_err_t err, int status, int64_t t0)
{
    if (err == ESP_OK && status == 200) svc_ok(id, t0);
    else {
        char why[40];
        snprintf(why, sizeof(why), status > 0 ? "HTTP %d" : "Can't connect", status);
        svc_fail(id, why, t0);
    }
}
void svc_ok(svc_id_t id, int64_t t0)
{
    svc[id].last_try = svc[id].last_ok = esp_timer_get_time();
    svc[id].ms = (int)((svc[id].last_try - t0) / 1000);
    svc[id].ok = true;
    svc[id].fails = 0;
}
void svc_fail(svc_id_t id, const char *why, int64_t t0)
{
    svc[id].last_try = esp_timer_get_time();
    svc[id].ms = (int)((svc[id].last_try - t0) / 1000);
    svc[id].ok = false;
    svc[id].fails++;
    snprintf(svc[id].why, sizeof(svc[id].why), "%s", why);
}
void svc_get(svc_id_t id, svc_info_t *out) { *out = svc[id]; }
const char *svc_user_agent(void) { return "esp32-s3-weather emulator"; }   // (not sent: see emu_http.c)
void svc_probe_stale(void) {}

/* ---------- speaker and microphones: none ---------- */
static sound_cfg_t snd = { .level = 2, .volume = 60, .quiet_from = 22 * 60, .quiet_to = 7 * 60 };
void sound_get_config(sound_cfg_t *out) { *out = snd; }
bool sound_set_config(const sound_cfg_t *in) { snd = *in; return true; }
void sound_test(int level) { (void)level; }
void sound_alert(char colour) { (void)colour; }
bool sound_ok(void) { return false; }

static presence_cfg_t pres = { .enabled = false, .margin_db = 6, .wake_s = 1, .dim_s = 600, .off_s = 1800,
                               .bright_pct = 80, .dim_pct = 15, .baseline_db = -60 };
void presence_get_config(presence_cfg_t *out) { *out = pres; }
bool presence_set_config(const presence_cfg_t *in) { pres = *in; display_brightness(pres.bright_pct * 255 / 100); return true; }
void presence_get_status(presence_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->state = PRESENCE_ACTIVE;
    st->brightness = pres.bright_pct;
}
bool presence_motion_wake(void) { return false; }
void presence_set_motion(bool on, float threshold_g) { (void)on; (void)threshold_g; }
void presence_preview_brightness(int pct) { display_brightness(pct * 255 / 100); }
void presence_wake(void) {}
bool presence_touch(void) { return false; }
bool presence_screen_off(void) { return false; }

const char *web_key(void) { return "browser"; }
