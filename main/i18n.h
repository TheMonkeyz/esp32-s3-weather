#pragma once
#include "forge_i18n.h"

// Display language. Texts live in i18n_strings.h (one line per string, one column per language); the language core
// and the date rules are espforge's forge_core (forge_i18n.h: "Mercredi 1er octobre"), with English, French and
// Inuktitut (its own names, English order). The language is saved with the units (config.c, NVS "units"/"lang", an
// index in this order) and also used by the settings page.

enum { LANG_EN, LANG_FR, LANG_IU, LANG_COUNT };   // IU: Inuktitut (syllabics), draft for review

typedef enum {
#define X(id, ...) id,
#include "i18n_strings.h"
#undef X
    T_COUNT
} tid_t;

#define tr(id) i18n_text(id)              // the text in the current language (English if missing or empty)
void app_text_init(void);                 // the table and the languages to forge_i18n (before anything shows text)
const char *tr_weather(int wmo_code);
