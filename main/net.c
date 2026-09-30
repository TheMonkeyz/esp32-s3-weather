// Wi-Fi station with credentials in NVS, plus a SoftAP setup page to enter them
#include "net.h"
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
#include "dns_server.h"

static const char *TAG = "net";
static EventGroupHandle_t ev;
#define BIT_GOT_IP  BIT0
#define BIT_FAIL    BIT1
static int retries;
static bool connected_once;
static bool portal_mode;
static esp_netif_t *sta_netif;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!portal_mode) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(ev, BIT_GOT_IP);
        if (portal_mode) return;
        if (connected_once || retries++ < 8) {
            vTaskDelay(pdMS_TO_TICKS(connected_once ? 3000 : 1000));
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(ev, BIT_FAIL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        retries = 0;
        connected_once = true;
        xEventGroupSetBits(ev, BIT_GOT_IP);
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

bool net_connect(const char *ssid, const char *pass, int timeout_ms)
{
    if (!sta_netif) sta_netif = esp_netif_create_default_wifi_sta();
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    ESP_LOGI(TAG, "Connecting to \"%s\"...", ssid);
    esp_wifi_start();
    EventBits_t b = xEventGroupWaitBits(ev, BIT_GOT_IP | BIT_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(b & BIT_GOT_IP)) {
        ESP_LOGW(TAG, "Could not connect");
        return false;
    }
    // Clock via SNTP, Eastern time
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sc);
    return true;
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
static volatile bool ap_active;

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

void net_setup_ap_stop(void)
{
    if (!ap_active || portal_mode) return;
    if (dns) { stop_dns_server(dns); dns = NULL; }
    esp_wifi_set_mode(WIFI_MODE_STA);
    ap_active = false;
    ESP_LOGI(TAG, "Setup AP stopped");
}

bool net_setup_ap_active(void) { return ap_active; }
