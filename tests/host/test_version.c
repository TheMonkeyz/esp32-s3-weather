// version.c (what the updater offers) and sound_wanted() (which alerts sound when)
#include "check.h"
#include "version.h"
#include "sound.h"

static int cmp(const char *a, const char *b)
{
    ver_t x, y;
    if (!parse_ver(a, &x) || !parse_ver(b, &y)) return 99;
    int c = cmp_ver(&x, &y);
    return c < 0 ? -1 : c > 0;
}

int main(void)
{
    static const struct { const char *a, *b; int want; } pairs[] = {
        { "v1.12.0", "v1.11.1", 1 },            // numbers first
        { "v1.12.0", "v1.12.0", 0 },
        { "v1.10.0", "v1.9.9", 1 },             // numerically, not as text
        { "v1.12.0-rc.1", "v1.11.1", 1 },       // an rc of a newer version is newer
        { "v1.12.0-rc.1", "v1.12.0", -1 },      // an rc is below its release
        { "v1.12.0-rc.2", "v1.12.0-rc.1", 1 },  // rc by number
        { "v1.12.0-rc.10", "v1.12.0-rc.9", 1 },
        { "v1.12.0-fix.6", "v1.12.0-rc.1", -1 },// a test label counts below every rc of the same version...
        { "v1.12.0-fix.6", "v1.11.1", 1 },      // ...and above the previous release (it isn't offered v1.11.1)
        { "v1.12.1-x.1", "v1.12.0-rc.4", 1 },   // a test label of a higher version hides lower rcs
        { "v1.12.0-3-g1a2b3c4", "v1.12.0", 1 }, // git describe after a release: a dev build above it
        { "v1.12.0-3-g1a2b3c4", "v1.12.1-rc.1", -1 },
        { "v1.12.0-dirty", "v1.12.0-rc.1", -1 },// "-dirty" is just another suffix
        { "1.12.0", "v1.12.0", 0 },             // the v is optional
        { "v1.99.0-design.1", "v1.14.3-rc.2", 1 },  // docs/MACOS.md's test label: above every release and rc
        { "v1.99.0-design.1", "v1.98.9", 1 },       // up to v1.99, so nothing is offered over a designer's build
    };
    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
        CHECK(cmp(pairs[i].a, pairs[i].b) == pairs[i].want, "%s vs %s: %d, expected %d", pairs[i].a, pairs[i].b,
              cmp(pairs[i].a, pairs[i].b), pairs[i].want);
    ver_t v;
    CHECK(!parse_ver("1a2b3c", &v), "a plain hash is not a version");
    CHECK(!parse_ver("v1.2", &v), "two numbers");
    CHECK(!parse_ver("v1.2.3x", &v), "garbage after the numbers");

    // sound_wanted(level, sev, from, to, now)
    int q22 = 22 * 60, q7 = 7 * 60;
    CHECK(!sound_wanted(0, 3, q22, q7, 12 * 60), "off: not even red");
    CHECK(sound_wanted(1, 3, q22, q7, 23 * 60), "red only: red in quiet hours sounds");
    CHECK(!sound_wanted(1, 2, q22, q7, 12 * 60), "red only: orange doesn't");
    CHECK(sound_wanted(2, 2, q22, q7, 12 * 60), "orange+red: orange at noon");
    CHECK(!sound_wanted(2, 2, q22, q7, 23 * 60), "quiet across midnight: 23:00");
    CHECK(!sound_wanted(2, 2, q22, q7, 3 * 60), "quiet across midnight: 03:00");
    CHECK(sound_wanted(2, 2, q22, q7, 7 * 60), "quiet ends at 07:00");
    CHECK(!sound_wanted(2, 2, q22, q7, q22), "quiet starts at 22:00");
    CHECK(!sound_wanted(3, 2, 13 * 60, 14 * 60, 13 * 60 + 30), "quiet within a day");
    CHECK(sound_wanted(3, 2, 13 * 60, 14 * 60, 14 * 60), "after it");
    CHECK(sound_wanted(3, 1, q22, q22, 23 * 60), "equal times: no quiet hours");
    CHECK(sound_wanted(3, 0, q22, q7, 12 * 60), "all: a statement sounds");
    CHECK(!sound_wanted(2, 1, q22, q7, 12 * 60), "orange+red: yellow doesn't");
    CHECK(sound_wanted(2, 2, q22, q7, -1), "clock not set: no quiet hours");
    return check_done("version");
}
