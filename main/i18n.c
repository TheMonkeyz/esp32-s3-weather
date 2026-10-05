// Display language (see i18n.h): the text table, Inuktitut, and the weather codes' texts. The rest is forge_core's.
#include "i18n.h"

static const char *const texts[T_COUNT][LANG_COUNT] = {
#define X(id, ...) [id] = { __VA_ARGS__ },
#include "i18n_strings.h"
#undef X
};

// Inuktitut is a draft no fluent speaker has reviewed (docs/translations): its name says so where it is chosen.
// Dates in English order with its own names; no usual short weekday forms: the full names.
static const i18n_lang_t i18n_iu = {
    "iu", "ᐃᓄᒃᑎᑐᑦ (draft)",
    { "ᓈᑦᓰᖑᔭᖅ", "ᓇᒡᒐᔾᔭᐅ", "ᐊᐃᑉᐱᖅ", "ᐱᖓᑦᓯᖅ", "ᓯᑕᒻᒥᖅ", "ᑕᓪᓕᕐᒥᖅ", "ᓯᕙᑖᕐᕕᒃ" },   // Nunavut usage (Tusaalanga)
    { "ᓈᑦᓰᖑᔭᖅ", "ᓇᒡᒐᔾᔭᐅ", "ᐊᐃᑉᐱᖅ", "ᐱᖓᑦᓯᖅ", "ᓯᑕᒻᒥᖅ", "ᑕᓪᓕᕐᒥᖅ", "ᓯᕙᑖᕐᕕᒃ" },
    { "ᔮᓐᓄᐊᕆ", "ᕖᕝᕗᐊᕆ", "ᒫᑦᓯ", "ᐄᐳᕆ", "ᒪᐃ", "ᔫᓂ", "ᔪᓚᐃ", "ᐋᒡᒌᓯ", "ᓯᑎᐱᕆ", "ᐅᑐᐱᕆ", "ᓄᕕᐱᕆ", "ᑎᓯᐱᕆ" },
    I18N_DATE_EN,
};

static const i18n_lang_t *const langs[LANG_COUNT] = { &i18n_en, &i18n_fr, &i18n_iu };

void app_text_init(void) { i18n_init(&texts[0][0], T_COUNT, langs, LANG_COUNT); }

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
