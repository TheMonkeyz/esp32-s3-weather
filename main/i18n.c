// Display language (see i18n.h)
#include "i18n.h"
#include <stdio.h>
#include <string.h>
#include "config.h"

static const char *const texts[T_COUNT][LANG_COUNT] = {
#define X(id, ...) [id] = { __VA_ARGS__ },
#include "i18n_strings.h"
#undef X
};

static const char *const LANG_CODE[LANG_COUNT] = { "en", "fr", "iu" };
// Inuktitut is a draft no fluent speaker has reviewed (docs/translations): its name says so where it is chosen
static const char *const LANG_NAME[LANG_COUNT] = { "English", "Français", "ᐃᓄᒃᑎᑐᑦ (draft)" };

static const char *const WD_FULL[LANG_COUNT][7] = {
    { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
    { "Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi" },
    { "ᓈᑦᓰᖑᔭᖅ", "ᓇᒡᒐᔾᔭᐅ", "ᐊᐃᑉᐱᖅ", "ᐱᖓᑦᓯᖅ", "ᓯᑕᒻᒥᖅ", "ᑕᓪᓕᕐᒥᖅ", "ᓯᕙᑖᕐᕕᒃ" },   // Nunavut usage (Tusaalanga)
};
static const char *const WD_SHORT[LANG_COUNT][7] = {
    { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" },
    { "Dim.", "Lun.", "Mar.", "Mer.", "Jeu.", "Ven.", "Sam." },
    { "ᓈᑦᓰᖑᔭᖅ", "ᓇᒡᒐᔾᔭᐅ", "ᐊᐃᑉᐱᖅ", "ᐱᖓᑦᓯᖅ", "ᓯᑕᒻᒥᖅ", "ᑕᓪᓕᕐᒥᖅ", "ᓯᕙᑖᕐᕕᒃ" },   // no usual short forms: full names
};
static const char *const MONTH[LANG_COUNT][12] = {
    { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
      "November", "December" },
    { "janvier", "février", "mars", "avril", "mai", "juin", "juillet", "août", "septembre", "octobre",
      "novembre", "décembre" },                      // lowercase: they're used inside a date
    { "ᔮᓐᓄᐊᕆ", "ᕖᕝᕗᐊᕆ", "ᒫᑦᓯ", "ᐄᐳᕆ", "ᒪᐃ", "ᔫᓂ", "ᔪᓚᐃ", "ᐋᒡᒌᓯ", "ᓯᑎᐱᕆ", "ᐅᑐᐱᕆ", "ᓄᕕᐱᕆ", "ᑎᓯᐱᕆ" },
};

static volatile lang_t lang = LANG_EN;
static bool loaded;

static lang_t cur(void)
{
    if (!loaded) {                                   // the saved language is read with the units
        loaded = true;
        units_t u;
        config_get_units(&u);
    }
    return lang;
}

void i18n_set(lang_t l) { lang = l < LANG_COUNT ? l : LANG_EN; loaded = true; }
lang_t i18n_lang(void) { return cur(); }
const char *i18n_code(lang_t l) { return LANG_CODE[l < LANG_COUNT ? l : 0]; }
const char *i18n_name(lang_t l) { return LANG_NAME[l < LANG_COUNT ? l : 0]; }

lang_t i18n_from_code(const char *code)
{
    for (int i = 0; i < LANG_COUNT; i++) if (code && !strcmp(code, LANG_CODE[i])) return i;
    return LANG_EN;
}

const char *tr(tid_t id)
{
    if (id >= T_COUNT) return "";
    const char *s = texts[id][cur()];
    return s && s[0] ? s : texts[id][LANG_EN];          // missing or empty (not translated yet): English
}

const char *tr_weekday(int wday, bool full)
{
    wday = ((wday % 7) + 7) % 7;
    return full ? WD_FULL[cur()][wday] : WD_SHORT[cur()][wday];
}

void tr_date_long(const struct tm *tm, char *out, int n)
{
    int m = tm->tm_mon % 12, d = tm->tm_mday;
    if (cur() == LANG_FR)                                    // 1er for the first of the month, else the number
        snprintf(out, n, "%s %d%s %s", WD_FULL[LANG_FR][tm->tm_wday % 7], d, d == 1 ? "er" : "", MONTH[LANG_FR][m]);
    else                                                     // English order, also used for Inuktitut
        snprintf(out, n, "%s, %s %d", WD_FULL[cur() == LANG_IU ? LANG_IU : LANG_EN][tm->tm_wday % 7],
                 MONTH[cur() == LANG_IU ? LANG_IU : LANG_EN][m], d);
}

void tr_date_ymd(int y, int m, int d, char *out, int n)
{
    m = (m - 1) % 12;
    if (m < 0) m = 0;
    if (cur() == LANG_FR) snprintf(out, n, "%d%s %s %d", d, d == 1 ? "er" : "", MONTH[LANG_FR][m], y);
    else snprintf(out, n, "%s %d, %d", MONTH[cur() == LANG_IU ? LANG_IU : LANG_EN][m], d, y);
}

const char *tr_weather(int code)
{
    switch (code) {
    case 0: return tr(T_WX_CLEAR);
    case 1: return tr(T_WX_MAINLY_CLEAR);
    case 2: return tr(T_WX_PARTLY);
    case 3: return tr(T_WX_OVERCAST);
    case 45: case 48: return tr(T_WX_FOG);
    case 51: case 53: case 55: return tr(T_WX_DRIZZLE);
    case 56: case 57: return tr(T_WX_FRZ_DRIZZLE);
    case 61: return tr(T_WX_LIGHT_RAIN);
    case 63: return tr(T_WX_RAIN);
    case 65: return tr(T_WX_HEAVY_RAIN);
    case 66: case 67: return tr(T_WX_FRZ_RAIN);
    case 71: return tr(T_WX_LIGHT_SNOW);
    case 73: return tr(T_WX_SNOW);
    case 75: return tr(T_WX_HEAVY_SNOW);
    case 77: return tr(T_WX_SNOW_GRAINS);
    case 80: case 81: case 82: return tr(T_WX_RAIN_SHOWERS);
    case 85: case 86: return tr(T_WX_SNOW_SHOWERS);
    case 95: return tr(T_WX_STORM);
    case 96: case 99: return tr(T_WX_STORM_HAIL);
    default: return "—";
    }
}
