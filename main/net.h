#pragma once
#include <stdbool.h>
#include <stddef.h>

#define SETUP_AP_SSID "Weather-Setup"
#define SETUP_AP_PASS "meteo1234"

void net_init(void);
bool net_load_creds(char *ssid, size_t sl, char *pass, size_t pl);
void net_clear_creds(void);
bool net_connect(const char *ssid, const char *pass, int timeout_ms);
bool net_is_connected(void);
void net_start_portal(void);
bool net_save_creds(const char *ssid, const char *pass);
bool net_get_ssid(char *out, size_t n);
bool net_get_ip(char *out, size_t n);
bool net_in_portal(void);
