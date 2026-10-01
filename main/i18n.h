#pragma once
#include <stdbool.h>
#include <time.h>

// Display language. Texts live in i18n_strings.h (one line per string, one column per language); dates use
// per-language rules here (French: "Mercredi 1er octobre", lowercase names inside a phrase, "1er" for the 1st).
// The language is saved with the units (config.c) and also used by the settings page.

typedef enum { LANG_EN, LANG_FR, LANG_COUNT } lang_t;

typedef enum {
#define X(id, ...) id,
#include "i18n_strings.h"
#undef X
    T_COUNT
} tid_t;

const char *tr(tid_t id);                 // the text in the current language (English if missing)
lang_t i18n_lang(void);
void i18n_set(lang_t l);                  // called by config.c (load / save)
const char *i18n_code(lang_t l);          // "en", "fr"
const char *i18n_name(lang_t l);          // "English", "Français" (in its own language)
lang_t i18n_from_code(const char *code);  // unknown -> LANG_EN

const char *tr_weekday(int wday, bool full);   // 0 = Sunday; "Wednesday" / "Wed", "Mercredi" / "Mer."
// Dates: "Wednesday, September 30" / "Mercredi 1er octobre"; "September 30, 2026" / "30 septembre 2026"
void tr_date_long(const struct tm *tm, char *out, int n);
void tr_date_ymd(int y, int m, int d, char *out, int n);   // m 1..12
const char *tr_weather(int wmo_code);
