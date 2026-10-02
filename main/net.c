// Wi-Fi station with credentials in NVS, plus a SoftAP setup page to enter them
#include "net.h"
#include "svc.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "dns_server.h"
#include "esp_dpp.h"
#include "esp_idf_version.h"
#include "esp_attr.h"

static const char *TAG = "net";
static EventGroupHandle_t ev;
#define BIT_GOT_IP  BIT0
#define BIT_FAIL    BIT1
static int retries;
static bool portal_mode;
static esp_netif_t *sta_netif;
static esp_timer_handle_t retry_timer;
static volatile bool ap_active;
static volatile bool dpp_active;

int net_ap_clients(void)
{
    wifi_sta_list_t l;
    return ap_active && esp_wifi_ap_get_sta_list(&l) == ESP_OK ? l.num : 0;
}

// Reconnect attempts slow down (1 s for the first 8, then 3 s, then every 30 s) but don't give up: the router may
// come back after a power cut. They pause while a setup mode is on (setup network, Easy Connect, first-time
// portal): an attempt makes the radio hop channels, so phones couldn't join the setup network or reach Easy
// Connect. Closing setup tries the saved network again (resume_saved).
static bool setup_on(void) { return portal_mode || dpp_active || ap_active; }

static void retry_cb(void *arg)
{
    if (setup_on() || net_is_connected()) return;
    esp_wifi_connect();
}

static void pause_saved(void)                                // a setup mode starts: no more attempts
{
    esp_timer_stop(retry_timer);
    if (!net_is_connected()) {
        esp_wifi_disconnect();                               // also cancels an attempt in progress
        ESP_LOGI(TAG, "Setup open: not trying the saved network meanwhile");
    }
}

static void resume_saved(void)                               // a setup mode ends
{
    if (setup_on() || net_is_connected()) return;
    retries = 0;
    ESP_LOGI(TAG, "Setup closed: trying the saved network again");
    esp_timer_stop(retry_timer);                             // after 1 s: switching setup pages stops one mode
    esp_timer_start_once(retry_timer, 1000 * 1000ULL);       // and starts the other just after
}

static void ntp_synced(struct timeval *tv) { svc_ok(SVC_NTP, 0); }

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!setup_on()) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(ev, BIT_GOT_IP);
        if (setup_on()) return;
        retries++;
        if (retries == 8) xEventGroupSetBits(ev, BIT_FAIL);      // net_wait() gives up; retries go on
        int ms = retries < 8 ? 1000 : retries < 20 ? 3000 : 30000;
        if (retries == 20) ESP_LOGW(TAG, "Still no Wi-Fi, retrying every 30 s");
        esp_timer_stop(retry_timer);
        esp_timer_start_once(retry_timer, ms * 1000ULL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        retries = 0;
        xEventGroupClearBits(ev, BIT_FAIL);
        xEventGroupSetBits(ev, BIT_GOT_IP);
        static bool sntp;
        if (!sntp) {                                             // clock via SNTP, once
            sntp = true;
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            sc.sync_cb = ntp_synced;
            esp_netif_sntp_init(&sc);
        }
    }
}

void net_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ev = xEventGroupCreate();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    const esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi_retry" };
    esp_timer_create(&ta, &retry_timer);
}

bool net_load_creds(char *ssid, size_t sl, char *pass, size_t pl)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, "ssid", ssid, &sl) == ESP_OK && nvs_get_str(h, "pass", pass, &pl) == ESP_OK;
    nvs_close(h);
    return ok && ssid[0];
}

void net_clear_creds(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
    ESP_LOGW(TAG, "Wi-Fi credentials cleared");
}

/* ---------------- test console hooks (testcon.c) ----------------
 * "Saved network unreachable" without touching the saved credentials: the station is given a network name that
 * doesn't exist. offline-boot keeps a flag in RTC memory (survives esp_restart, not a power cut) so the next boot
 * takes the real start-up path (Connecting... -> 30 s -> offline setup), which is where the October 1 bugs were. */
#define TEST_SSID "Weather-Test-Unreachable"
#define TEST_MAGIC 0x0FF11E55u
static RTC_NOINIT_ATTR uint32_t test_offline_boot;

static void sta_config(const char *ssid, const char *pass)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

void net_test_offline_next_boot(void) { test_offline_boot = TEST_MAGIC; }

void net_test_offline(void)
{
    ESP_LOGW(TAG, "TEST: saved network replaced by \"" TEST_SSID "\" until 'wifi online' or a restart");
    esp_timer_stop(retry_timer);
    sta_config(TEST_SSID, "unreachable");
    esp_wifi_disconnect();                        // the disconnect event schedules retries (to the fake network)
}

void net_test_online(void)
{
    char ssid[33] = "", pass[65] = "";
    if (!net_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) return;
    ESP_LOGW(TAG, "TEST: saved network \"%s\" restored", ssid);
    sta_config(ssid, pass);
    if (setup_on() || net_is_connected()) return;  // setup open: tried once it closes (resume_saved)
    retries = 0;
    esp_timer_stop(retry_timer);
    esp_wifi_disconnect();
    esp_wifi_connect();
}

void net_test_info(char *out, size_t n)
{
    wifi_config_t wc = {0};
    esp_wifi_get_config(WIFI_IF_STA, &wc);
    uint8_t ch = 0; wifi_second_chan_t sc;
    esp_wifi_get_channel(&ch, &sc);
    snprintf(out, n, "connected=%d sta_ssid=%s portal=%d ap=%d ap_clients=%d dpp=%d retries=%d channel=%u",
             net_is_connected(), (char *)wc.sta.ssid, portal_mode, ap_active, net_ap_clients(), dpp_active,
             retries, ch);
}

void net_begin(const char *ssid, const char *pass)
{
    if (!sta_netif) sta_netif = esp_netif_create_default_wifi_sta();
    if (test_offline_boot == TEST_MAGIC) {        // one boot only
        test_offline_boot = 0;
        ESP_LOGW(TAG, "TEST: this boot uses \"" TEST_SSID "\" instead of \"%s\"", ssid);
        ssid = TEST_SSID;
        pass = "unreachable";
    }
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    ESP_LOGI(TAG, "Connecting to \"%s\"...", ssid);
    esp_wifi_start();
}

bool net_wait(int timeout_ms)
{
    EventBits_t b = xEventGroupWaitBits(ev, BIT_GOT_IP | BIT_FAIL, pdFALSE, pdFALSE,
                                        timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
    if (b & BIT_GOT_IP) return true;
    ESP_LOGW(TAG, "Could not connect (still retrying in the background)");
    return false;
}

bool net_wait_connected(int timeout_ms)
{
    return xEventGroupWaitBits(ev, BIT_GOT_IP, pdFALSE, pdFALSE,
                               timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) & BIT_GOT_IP;
}

bool net_is_connected(void) { return ev && (xEventGroupGetBits(ev) & BIT_GOT_IP); }

/* ---------------- Setup portal (AP) + helpers for the web UI ---------------- */

bool net_save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (!ssid[0] || nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved credentials for \"%s\"", ssid);
    return true;
}

bool net_get_ssid(char *out, size_t n)
{
    wifi_config_t wc;
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) != ESP_OK) return false;
    strlcpy(out, (char *)wc.sta.ssid, n);
    return out[0] != 0;
}

bool net_get_ip(char *out, size_t n)
{
    esp_netif_t *nif = portal_mode ? esp_netif_get_handle_from_ifkey("WIFI_AP_DEF") : sta_netif;
    esp_netif_ip_info_t ip;
    if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.ip.addr == 0) return false;
    snprintf(out, n, IPSTR, IP2STR(&ip.ip));
    return true;
}

bool net_in_portal(void) { return portal_mode; }

static esp_netif_t *ap_netif;
static dns_server_handle_t dns;

// Bring up the setup access point with a captive portal (DNS answers everything with us,
// DHCP option 114 advertises the setup page). Keeps the station connection if there is one.
static void ap_up(void)
{
    if (!ap_netif) ap_netif = esp_netif_create_default_wifi_ap();
    wifi_config_t ap = {
        .ap = { .ssid = SETUP_AP_SSID, .ssid_len = sizeof(SETUP_AP_SSID) - 1,
                .password = SETUP_AP_PASS, .max_connection = 3,
                .authmode = WIFI_AUTH_WPA2_PSK, .channel = 6 },
    };
    esp_wifi_set_mode(WIFI_MODE_APSTA);   // AP follows the station's channel when connected
    esp_wifi_set_config(WIFI_IF_AP, &ap);

    static char uri[] = "http://192.168.4.1/";
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
    esp_netif_dhcps_start(ap_netif);

    if (!dns) {
        dns_server_config_t cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        dns = start_dns_server(&cfg);
    }
    ap_active = true;
    pause_saved();
}

void net_start_portal(void)
{
    portal_mode = true;
    esp_wifi_stop();
    if (!sta_netif) sta_netif = esp_netif_create_default_wifi_sta();
    ap_up();
    esp_wifi_start();
    ESP_LOGI(TAG, "Setup portal up: join \"%s\" (password %s)", SETUP_AP_SSID, SETUP_AP_PASS);
}

void net_setup_ap_start(void)
{
    if (ap_active) return;
    ap_up();
    ESP_LOGI(TAG, "Setup AP started alongside the current connection");
}

static void ap_down(void)
{
    if (!ap_active) return;
    // The DNS server stays up: stop_dns_server() deletes its task without closing the socket, so port 53 stayed
    // taken and the next setup network had no DNS (no captive portal). It only answers phones on the setup AP.
    esp_wifi_set_mode(WIFI_MODE_STA);
    ap_active = false;
    ESP_LOGI(TAG, "Setup AP stopped");
    resume_saved();
}

void net_setup_ap_stop(void)
{
    if (!portal_mode) ap_down();
}

void net_setup_ap_stop_any(void) { ap_down(); }

/* ---------------- Wi-Fi Easy Connect (DPP enrollee) ----------------
 * The display shows a DPP QR code; an Android phone (10+) scans it from its Wi-Fi settings and sends
 * the network it's connected to (SSID + password). Needs STA mode without connection attempts, so the
 * setup AP and our reconnects are paused while it listens. */

// Listen on ONE channel, the one the phone is most likely on: the saved network's if it is in range, else the
// strongest network's. The phone stays on its own network's channel; the display needs ~0.3 s to answer, and a
// phone that had hopped to another channel to talk to us was already back home (Auth Confirm timeout). Online,
// the display was on the router's channel anyway, which is why Easy Connect only worked then.
static char dpp_chan[4] = "6";

// One scan; returns the channel of the strongest 2.4 GHz record (of `ssid` if given), 0 if none. *seen = records.
static int scan_channel(const char *ssid, int dwell_ms, int *seen)
{
    wifi_scan_config_t sc = { .ssid = (uint8_t *)ssid, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                              .scan_time.active = { .min = dwell_ms / 2, .max = dwell_ms } };
    uint16_t n = 16;
    wifi_ap_record_t *r = calloc(n, sizeof(*r));
    int ch = 0;
    if (r && esp_wifi_scan_start(&sc, true) == ESP_OK && esp_wifi_scan_get_ap_records(&n, r) == ESP_OK) {
        for (int i = 0; i < n && !ch; i++)                  // records come sorted by signal
            if (r[i].primary >= 1 && r[i].primary <= 13) ch = r[i].primary;
    } else {
        n = 0;
    }
    free(r);
    *seen = n;
    return ch;
}

static void dpp_pick_channel(void)
{
    char saved[33] = "", pass[65];
    net_load_creds(saved, sizeof(saved), pass, sizeof(pass));
    int seen = 0, ch = 0;
    const char *why = "default";
    // The saved network first, by name: a probe request carrying its name is answered more reliably than a broadcast
    // one, and only its records come back (a broadcast scan keeps the 16 strongest). The broadcast scan at 40-80 ms per
    // channel missed a router on a busy channel (v1.10.0 harness run: it picked the strongest network, channel 11).
    if (saved[0] && (ch = scan_channel(saved, 120, &seen))) why = "saved network";      // ~1.6 s
    else if ((ch = scan_channel(NULL, 80, &seen))) why = "strongest network";          // ~1 s more
    if (ch < 1 || ch > 13) ch = 6;
    snprintf(dpp_chan, sizeof(dpp_chan), "%d", ch);
    ESP_LOGI(TAG, "Easy Connect: channel %d (%s, %d networks seen)", ch, why, seen);
}

static net_dpp_uri_cb_t dpp_uri_cb;
static net_dpp_done_cb_t dpp_done_cb;
static bool dpp_inited;

static void restart_cb(void *arg) { esp_restart(); }

static void dpp_event(esp_supp_dpp_event_t evt, void *data)
{
    switch (evt) {
    case ESP_SUPP_DPP_URI_READY:
        if (data) {
            ESP_LOGI(TAG, "Easy Connect: QR code ready, listening on channel %s", dpp_chan);
            if (dpp_uri_cb) dpp_uri_cb((const char *)data);
            // bootstrap_gen() is asynchronous: listening is only possible once the code exists
            esp_err_t e = dpp_active ? esp_supp_dpp_start_listen() : ESP_OK;
            if (e != ESP_OK) ESP_LOGE(TAG, "Easy Connect: can't listen: %s", esp_err_to_name(e));
        }
        break;
    case ESP_SUPP_DPP_CFG_RECVD: {
        wifi_config_t *wc = data;
        ESP_LOGI(TAG, "Easy Connect: received \"%s\" from the phone", (char *)wc->sta.ssid);
        bool ok = net_save_creds((char *)wc->sta.ssid, (char *)wc->sta.password);
        if (dpp_done_cb) dpp_done_cb(ok, (char *)wc->sta.ssid);
        if (ok) {                                            // same as the setup page: restart and join
            static esp_timer_handle_t t;
            const esp_timer_create_args_t ta = { .callback = restart_cb, .name = "dpp_restart" };
            if (!t && esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_once(t, 2500 * 1000);
        }
        break;
    }
    case ESP_SUPP_DPP_FAIL: {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
        int why = data ? ((wifi_event_dpp_failed_t *)data)->failure_reason : ESP_FAIL;   // an event struct since 5.5
#else
        int why = (int)(intptr_t)data;
#endif
        ESP_LOGW(TAG, "Easy Connect failed (%s, 0x%x), listening again", esp_err_to_name(why), why);
        if (dpp_done_cb) dpp_done_cb(false, "");
        if (dpp_active) esp_supp_dpp_start_listen();
        break;
    }
    default:
        break;
    }
}

bool net_dpp_start(net_dpp_uri_cb_t on_uri, net_dpp_done_cb_t on_done)
{
    if (dpp_active) return true;
    dpp_uri_cb = on_uri;
    dpp_done_cb = on_done;
    dpp_active = true;
    esp_timer_stop(retry_timer);
    esp_wifi_disconnect();                                 // listening needs the radio (also cancels an attempt)
    ESP_LOGI(TAG, "Easy Connect: not trying the saved network meanwhile");
    esp_wifi_set_mode(WIFI_MODE_STA);
    dpp_pick_channel();
    esp_err_t err = ESP_OK;
    if (!dpp_inited) {
        err = esp_supp_dpp_init(dpp_event);
        dpp_inited = err == ESP_OK;
    }
    if (err == ESP_OK) err = esp_supp_dpp_bootstrap_gen(dpp_chan, DPP_BOOTSTRAP_QR_CODE, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Easy Connect unavailable: %s", esp_err_to_name(err));
        net_dpp_stop();
        return false;
    }
    ESP_LOGI(TAG, "Easy Connect started");
    return true;
}

void net_dpp_stop(void)
{
    if (!dpp_active) return;
    esp_supp_dpp_stop_listen();
    if (dpp_inited) { esp_supp_dpp_deinit(); dpp_inited = false; }
    dpp_active = false;
    dpp_uri_cb = NULL;
    dpp_done_cb = NULL;
    ESP_LOGI(TAG, "Easy Connect stopped");
    resume_saved();
}

bool net_dpp_active(void) { return dpp_active; }

bool net_setup_ap_active(void) { return ap_active; }
