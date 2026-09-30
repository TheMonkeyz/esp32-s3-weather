#pragma once
#include <stdbool.h>
#include <stddef.h>

#define SETUP_AP_SSID "Weather-Setup"
#define SETUP_AP_PASS "meteo1234"

void net_init(void);
bool net_load_creds(char *ssid, size_t sl, char *pass, size_t pl);
void net_clear_creds(void);
void net_begin(const char *ssid, const char *pass);   // start connecting; retries forever in the background
bool net_wait(int timeout_ms);             // true once connected; false after ~8 failed attempts or timeout
bool net_wait_connected(int timeout_ms);   // wait for a connection only (-1 = forever)
bool net_is_connected(void);
void net_start_portal(void);
bool net_save_creds(const char *ssid, const char *pass);
bool net_get_ssid(char *out, size_t n);
bool net_get_ip(char *out, size_t n);
bool net_in_portal(void);
void net_setup_ap_start(void);   // setup AP + captive portal while staying connected
void net_setup_ap_stop(void);
bool net_setup_ap_active(void);
int net_ap_clients(void);        // phones joined to the setup AP
void net_setup_ap_stop_any(void);  // stop the setup AP even during first-time setup (Easy Connect needs the radio)

// Wi-Fi Easy Connect (Android 10+): on_uri gets the DPP URI to show as a QR code; on_done(true, ssid) when a
// phone sent credentials (they're saved and the board restarts 2.5 s later). Callbacks run in the Wi-Fi task.
typedef void (*net_dpp_uri_cb_t)(const char *uri);
typedef void (*net_dpp_done_cb_t)(bool ok, const char *ssid);
bool net_dpp_start(net_dpp_uri_cb_t on_uri, net_dpp_done_cb_t on_done);
void net_dpp_stop(void);
bool net_dpp_active(void);
