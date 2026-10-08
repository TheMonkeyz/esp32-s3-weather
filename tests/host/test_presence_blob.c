// The settings import (forge_presence's presence_cfg_from_blob_v1): the display's old presence.c (until v1.14.x) saved
// its settings as one 32-byte NVS blob "cfg", the bytes of its presence_cfg_t. forge_presence decodes that blob field
// by field at fixed offsets; this test writes the old struct exactly as it was declared (main/presence.h before
// v1.15.0), checks its layout is the one the decoder assumes (the ESP32-S3's ABI and the host's agree for bool, float
// and int: 1, 4, 4 bytes, 4-byte aligned), and checks the values come back the same.
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "check.h"
#include "presence.h"

typedef struct {                          // main/presence.h's presence_cfg_t, v1.12.0 to v1.14.x, unchanged
    bool  enabled;
    float margin_db;
    float wake_s;
    float dim_s;
    float off_s;
    int   bright_pct;
    int   dim_pct;
    float baseline_db;
} legacy_cfg_t;

_Static_assert(sizeof(legacy_cfg_t) == 32, "the old blob is 32 bytes");
_Static_assert(offsetof(legacy_cfg_t, enabled) == 0, "enabled at 0");
_Static_assert(offsetof(legacy_cfg_t, margin_db) == 4, "margin_db at 4");
_Static_assert(offsetof(legacy_cfg_t, wake_s) == 8, "wake_s at 8");
_Static_assert(offsetof(legacy_cfg_t, dim_s) == 12, "dim_s at 12");
_Static_assert(offsetof(legacy_cfg_t, off_s) == 16, "off_s at 16");
_Static_assert(offsetof(legacy_cfg_t, bright_pct) == 20, "bright_pct at 20");
_Static_assert(offsetof(legacy_cfg_t, dim_pct) == 24, "dim_pct at 24");
_Static_assert(offsetof(legacy_cfg_t, baseline_db) == 28, "baseline_db at 28");
_Static_assert(sizeof(int) == 4 && sizeof(float) == 4 && sizeof(bool) == 1, "int, float, bool sizes");

// The old struct's bytes as nvs_set_blob(h, "cfg", &cfg, sizeof(cfg)) stored them; the padding after `enabled` holds
// whatever was there (junk here)
static void as_blob(const legacy_cfg_t *in, unsigned char out[32])
{
    legacy_cfg_t c;
    memset(&c, 0xA5, sizeof(c));
    c.enabled = in->enabled; c.margin_db = in->margin_db; c.wake_s = in->wake_s; c.dim_s = in->dim_s;
    c.off_s = in->off_s; c.bright_pct = in->bright_pct; c.dim_pct = in->dim_pct; c.baseline_db = in->baseline_db;
    memcpy(out, &c, sizeof(c));
}

static void same(const legacy_cfg_t *in, const presence_cfg_t *out, const char *what)
{
    CHECK(out->enabled == in->enabled, "%s: enabled %d != %d", what, out->enabled, in->enabled);
    CHECK(out->margin_db == in->margin_db, "%s: margin_db %g != %g", what, out->margin_db, in->margin_db);
    CHECK(out->wake_s == in->wake_s, "%s: wake_s %g != %g", what, out->wake_s, in->wake_s);
    CHECK(out->dim_s == in->dim_s, "%s: dim_s %g != %g", what, out->dim_s, in->dim_s);
    CHECK(out->off_s == in->off_s, "%s: off_s %g != %g", what, out->off_s, in->off_s);
    CHECK(out->bright_pct == in->bright_pct, "%s: bright_pct %d != %d", what, out->bright_pct, in->bright_pct);
    CHECK(out->dim_pct == in->dim_pct, "%s: dim_pct %d != %d", what, out->dim_pct, in->dim_pct);
    CHECK(out->baseline_db == in->baseline_db, "%s: baseline_db %g != %g", what, out->baseline_db, in->baseline_db);
}

int main(void)
{
    unsigned char b[32];
    presence_cfg_t out;

    // The defaults ("Normal": dim 10 min, off at 60 min) and a user's custom settings, all inside the limits
    static const legacy_cfg_t cases[] = {
        { true, 10, 3, 600, 3000, 100, 15, -60 },
        { false, 7.5f, 1.5f, 120, 780, 40, 25, -67.25f },
        { true, 60, 60, 86400, 86400, 5, 100, -100 },
        { true, 1, 0.2f, 1, 1, 100, 1, 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        as_blob(&cases[i], b);
        memset(&out, 0x5A, sizeof(out));
        CHECK(presence_cfg_from_blob_v1(b, sizeof(b), &out), "case %zu: refused", i);
        char what[16];
        snprintf(what, sizeof(what), "case %zu", i);
        same(&cases[i], &out, what);
    }

    // `enabled` is a C bool: only its first byte counts, whatever the padding after it holds
    legacy_cfg_t off = cases[0];
    off.enabled = false;
    as_blob(&off, b);
    CHECK(presence_cfg_from_blob_v1(b, sizeof(b), &out) && !out.enabled, "padding read as enabled");

    // Out-of-range values (an old firmware before v1.12.0 saved them unclamped) are held to the limits
    legacy_cfg_t wild = { true, 0.5f, 100, 200000, 0, 150, 0, -120 };
    as_blob(&wild, b);
    CHECK(presence_cfg_from_blob_v1(b, sizeof(b), &out), "wild: refused");
    CHECK(out.margin_db == 1 && out.wake_s == 60 && out.dim_s == 86400 && out.off_s == 1 && out.bright_pct == 100 &&
          out.dim_pct == 1 && out.baseline_db == -100, "wild: not clamped (%g %g %g %g %d %d %g)", out.margin_db,
          out.wake_s, out.dim_s, out.off_s, out.bright_pct, out.dim_pct, out.baseline_db);
    legacy_cfg_t nan = cases[0];
    nan.margin_db = NAN;
    as_blob(&nan, b);
    CHECK(presence_cfg_from_blob_v1(b, sizeof(b), &out) && out.margin_db == 1, "NaN margin not clamped");

    // Any other size is not this blob
    CHECK(!presence_cfg_from_blob_v1(b, 31, &out), "31 bytes accepted");
    CHECK(!presence_cfg_from_blob_v1(b, 36, &out), "36 bytes accepted");
    CHECK(!presence_cfg_from_blob_v1(NULL, 32, &out), "NULL accepted");
    return check_done("presence_blob");
}
