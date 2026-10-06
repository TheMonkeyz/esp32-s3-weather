// What the screens ask of the rest of the firmware, answered for the browser: online, no setup network, no updates
// (Restart reloads the page). The microphones and the motion sensor are presence.c itself with emu_audio.c and
// emu_imu.c; the speaker is sound.c with emu_audio.c. The service statuses are recorded as the firmware's are
// (svc_http), for the status page.
#include <stdio.h>
#include <string.h>
#include <emscripten.h>
#include "esp_timer.h"
#include "esp_err.h"
#include "net.h"
#include "ota.h"
#include "services.h"
#include "web.h"
#include "alerts.h"
#include "display.h"
#include "esp_system.h"

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

/* ---------- updates: none in the browser (the settings page's channel picker is remembered) ---------- */
static char channel[8] = "stable";
void ota_get_status(ota_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = OTA_UP_TO_DATE;
    snprintf(out->current, sizeof(out->current), "%s", EMU_VERSION);
    snprintf(out->channel, sizeof(out->channel), "%s", channel);
}
void ota_check_now(void) {}
bool ota_install(void) { return false; }
void ota_set_channel(const char *ch) { if (ch && (!strcmp(ch, "stable") || !strcmp(ch, "beta"))) snprintf(channel, sizeof(channel), "%s", ch); }
bool ota_pending_verify(void) { return false; }
void ota_get_notes(char *out, size_t size) { if (size) out[0] = 0; }
void ota_restart_when_safe(void) { esp_restart(); }   // Settings > Restart: the page reloads (settings are already saved)
void ota_set_err_text(const char *(*fn)(ota_err_t err)) { (void)fn; }

/* ---------- service statuses (forge_net's svc.h, recorded as the firmware does for the status page) ---------- */
static svc_info_t svc[SVC_MAX];
static int nsvc;
static const char *(*why_text)(svc_why_t, int);
int svc_add(const char *name, const char *api, svc_probe_url_t probe)
{
    (void)probe;
    if (nsvc >= SVC_MAX) return -1;
    svc[nsvc] = (svc_info_t){ .name = name, .api = api };
    return nsvc++;
}
int svc_count(void) { return nsvc; }
// The firmware's NTP row is the browser's clock here (emu_main adds it under this name)
int svc_find(const char *name)
{
    if (name && !strcmp(name, SVC_NAME_NTP)) name = "Browser clock";
    for (int i = 0; i < nsvc; i++) if (name && !strcmp(svc[i].name, name)) return i;
    return -1;
}
void svc_set_why_text(const char *(*fn)(svc_why_t, int)) { why_text = fn; }
void svc_http(int id, esp_err_t err, int status, int64_t t0)
{
    if (err == ESP_OK && status == 200) svc_ok(id, t0);
    else if (status > 0) { char why[40]; snprintf(why, sizeof(why), "HTTP %d", status); svc_fail(id, why, t0); }
    else svc_fail_why(id, SVC_WHY_CONNECT, t0);
}
void svc_fail_why(int id, svc_why_t code, int64_t t0)
{
    const char *t = why_text ? why_text(code, 0) : NULL;
    svc_fail(id, t ? t : "error", t0);
}
void svc_ok(int id, int64_t t0)
{
    if (id < 0 || id >= nsvc) return;
    svc[id].last_try = svc[id].last_ok = esp_timer_get_time();
    svc[id].ms = (int)((svc[id].last_try - t0) / 1000);
    svc[id].ok = true;
    svc[id].fails = 0;
}
void svc_fail(int id, const char *why, int64_t t0)
{
    if (id < 0 || id >= nsvc) return;
    svc[id].last_try = esp_timer_get_time();
    svc[id].ms = (int)((svc[id].last_try - t0) / 1000);
    svc[id].ok = false;
    svc[id].fails++;
    snprintf(svc[id].why, sizeof(svc[id].why), "%s", why);
}
void svc_get(int id, svc_info_t *out)
{
    static const svc_info_t none = { "", "" };
    *out = id >= 0 && id < nsvc ? svc[id] : none;
}
const char *svc_user_agent(void) { return "esp32-s3-weather emulator"; }   // (not sent: see emu_http.c)
void svc_probe_stale(void) {}

const char *web_key(void) { return "browser"; }

// forge_core's NVS check (config.c's saves): emu_nvs.c never fails
bool nvs_check(esp_err_t err, const char *what) { (void)what; return err == ESP_OK; }
