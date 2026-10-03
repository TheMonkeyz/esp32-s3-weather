// i18n: every text exists in every language, keeps the English format conversions, and an empty one falls back
#include <string.h>
#include "check.h"
#include "../../main/i18n.c"            // its tables too

void config_get_units(units_t *u) { memset(u, 0, sizeof(*u)); }

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
    return check_done("i18n");
}
