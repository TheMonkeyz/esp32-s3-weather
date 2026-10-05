// i18n: every text exists in every language, keeps the English format conversions, and an empty one falls back
#include <string.h>
#include "check.h"
#include "../../main/i18n.c"            // its tables and the Inuktitut descriptor; the core is forge_core's i18n.c

// The printf conversions of a format, in order ("%s%d" for "Feels %d° at %s" would be "%d%s")
static void convs(const char *s, char *out, size_t n)
{
    size_t k = 0;
    for (; *s && k + 3 < n; s++) {
        if (*s != '%') continue;
        s++;
        if (*s == '%') continue;
        while (*s && strchr("0123456789.-+ #l", *s)) s++;          // flags, width, length
        if (!*s) break;
        out[k++] = '%';
        out[k++] = *s;
    }
    out[k] = 0;
}

int main(void)
{
    app_text_init();
    for (int id = 0; id < T_COUNT; id++) {
        char en[32], other[32];
        convs(texts[id][LANG_EN], en, sizeof(en));
        for (int l = 0; l < LANG_COUNT; l++) {
            const char *s = texts[id][l];
            // An empty text showed as nothing at all (two update messages in v1.12.0-rc.3's Inuktitut)
            CHECK(s && s[0], "text %d is empty in language %d", id, l);
            if (!s) continue;
            convs(s, other, sizeof(other));
            CHECK(!strcmp(en, other), "text %d, language %d: conversions %s, English has %s", id, l, other, en);
        }
    }
    // tr() falls back to English for a missing or empty text
    i18n_set(LANG_IU);
    CHECK(tr(T_TODAY)[0], "tr(T_TODAY) in Inuktitut");

    // The three languages as the display offers them (the order is NVS units/lang's index: never reorder)
    CHECK(i18n_count() == 3 && !strcmp(i18n_code(LANG_IU), "iu") && i18n_from_code("fr") == LANG_FR, "languages");
    // Dates: Québec French "1er"; Inuktitut in English order with its own names, its "short" weekday the full name
    char d[96];
    struct tm tm = { .tm_year = 126, .tm_mon = 9, .tm_mday = 1, .tm_wday = 4 };   // Thursday, October 1, 2026
    i18n_set(LANG_FR);
    tr_date_long(&tm, d, sizeof(d));
    CHECK(!strcmp(d, "Jeudi 1er octobre"), "%s", d);
    i18n_set(LANG_IU);
    tr_date_long(&tm, d, sizeof(d));
    CHECK(!strcmp(d, "ᓯᑕᒻᒥᖅ, ᐅᑐᐱᕆ 1"), "Inuktitut date: %s", d);
    CHECK(!strcmp(tr_weekday(4, false), "ᓯᑕᒻᒥᖅ"), "Inuktitut short weekday: %s", tr_weekday(4, false));
    i18n_set(LANG_EN);
    tr_date_ymd(2026, 10, 5, d, sizeof(d));
    CHECK(!strcmp(d, "October 5, 2026"), "%s", d);
    return check_done("i18n");
}
