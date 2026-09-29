#pragma once
#include "weather.h"

void ui_init(void);
void ui_message(const char *title, const char *body);
void ui_weather(const weather_t *w);
void ui_message_qr(const char *title, const char *body, const char *qr);
void ui_set_city(const char *name);
