// alerts.c: which alerts are kept (severity cap), the region shape parser, failures
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "check.h"
#include "fake.h"
#include "../../main/alerts.c"         // its static parsers too

size_t __sanitizer_get_current_allocated_bytes(void);   // the bytes held (AddressSanitizer's allocator; its header
                                                        // isn't installed with every gcc)

// The region map's background: a pattern of the cached map's pixel positions, so the crop can be checked. Each call
// records what alerts_map holds then (its buffers are all taken by this point).
static size_t held_base, held_at_map;
static struct { int z, x0, y0, w, h; } crop;
static uint16_t pattern(int x, int y) { return (uint16_t)(y * 466 + x); }
bool radar_basemap_crop(int z, double ox, double oy, int x0, int y0, int w, int h, uint16_t *dst)
{
    held_at_map = __sanitizer_get_current_allocated_bytes() - held_base;
    crop.z = z; crop.x0 = x0; crop.y0 = y0; crop.w = w; crop.h = h;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) dst[y * w + x] = pattern(x0 + x, y0 + y);
    return true;
}
bool radar_basemap_read(int z, uint16_t *dst, double *ox, double *oy)   // (up to v1.14.1: the whole map)
{
    held_at_map = __sanitizer_get_current_allocated_bytes() - held_base;
    return false;
}
bool radar_osm_render(int z, double ox, double oy, uint16_t *dst, int w, int h) { return true; }

// A region: a circle of n points around (lat, lon), as Environment Canada sends it
static char *region(double lat, double lon, double r, int n)
{
    static char buf[200 * 1024];
    int k = snprintf(buf, sizeof(buf), "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[[");
    for (int i = 0; i <= n && k < (int)sizeof(buf) - 64; i++)
        k += snprintf(buf + k, sizeof(buf) - k, "%s[%.6f,%.6f]", i ? "," : "",
                      lon + r * 1.4 * cos(2 * M_PI * i / n), lat + r * sin(2 * M_PI * i / n));
    snprintf(buf + k, sizeof(buf) - k, "]]},\"properties\":{}}");
    return buf;
}

static alerts_t al;

static char *features(const char *const *names, const char *const *colours, int n)
{
    static char buf[8192];
    int k = snprintf(buf, sizeof(buf), "{\"features\":[");
    for (int i = 0; i < n; i++)
        k += snprintf(buf + k, sizeof(buf) - k, "%s{\"id\":\"id-%d\",\"properties\":{\"status_en\":\"active\","
                      "\"alert_code\":\"C%d\",\"feature_id\":\"fea1-%d\",\"alert_name_en\":\"%s\",\"alert_name_fr\":\"fr %s\",\"risk_colour_en\":\"%s\"}}",
                      i ? "," : "", i, i, i, names[i], names[i], colours[i]);
    snprintf(buf + k, sizeof(buf) - k, "]}");
    return buf;
}

static void on_alarm(int s) { (void)s; fprintf(stderr, "FAIL: parse_shape did not finish\n"); _exit(1); }

int main(void)
{
    // Five alerts, the red one listed fifth: it is kept, and first (the cap ran before the sort: it was dropped)
    const char *n5[] = { "statement a", "advisory b", "watch c", "advisory d", "warning e" };
    const char *c5[] = { "grey", "yellow", "orange", "yellow", "red" };
    fake_http = (fake_http_t){ .body = features(n5, c5, 5), .status = 200 };
    CHECK(alerts_fetch(46.8, -71.2, &al), "fetch");
    CHECK(al.n == ALERTS_MAX, "n %d", al.n);
    CHECK(al.a[0].colour == 'r' && !strcmp(al.a[0].id, "id-4") && !strcmp(al.a[0].name[0], "Warning e"),
          "first %c %s", al.a[0].colour, al.a[0].id);
    CHECK(al.a[1].colour == 'o', "second %c", al.a[1].colour);
    CHECK(!strcmp(al.a[0].code, "C4") && !strcmp(al.a[0].feature, "fea1-4"), "code %s feature %s", al.a[0].code, al.a[0].feature);
    bool grey = false;
    for (int i = 0; i < al.n; i++) grey |= al.a[i].colour == 'g';
    CHECK(!grey, "the statement (least severe) gave way");

    // Six: two reds after four yellows
    const char *n6[] = { "y1", "y2", "y3", "y4", "r1", "r2" };
    const char *c6[] = { "yellow", "yellow", "yellow", "yellow", "red", "red" };
    fake_http.body = features(n6, c6, 6);
    CHECK(alerts_fetch(46.8, -71.2, &al) && al.n == 4, "n %d", al.n);
    CHECK(al.a[0].colour == 'r' && al.a[1].colour == 'r' && al.a[2].colour == 'y', "%c%c%c", al.a[0].colour,
          al.a[1].colour, al.a[2].colour);

    // The same alert for a neighbouring region is listed once
    const char *nd[] = { "Frost advisory", "frost advisory" };
    const char *cd[] = { "yellow", "yellow" };
    fake_http.body = features(nd, cd, 2);
    CHECK(alerts_fetch(46.8, -71.2, &al) && al.n == 1, "dedup n %d", al.n);

    // Failures: keep the old alerts (false)
    fake_http = (fake_http_t){ .no_client = true };
    CHECK(!alerts_fetch(46.8, -71.2, &al), "no client");
    fake_http = (fake_http_t){ .body = "{\"type\":\"x\"}", .status = 200 };
    CHECK(!alerts_fetch(46.8, -71.2, &al), "no features");

    // Region shape: a '-' that starts no number used to spin forever
    signal(SIGALRM, on_alarm);
    alarm(2);
    static float pts[4 * MAX_PTS];
    int ring_end[64];
    shape_t sh = { .ring_end = ring_end, .cap = MAX_PTS };
    CHECK(!parse_shape("{\"coordinates\":[[[-71.2,46.8],[-]]]}", &sh, pts, pts + MAX_PTS), "'-]' parsed");
    CHECK(parse_shape("{\"type\":\"Polygon\",\"coordinates\":[[[-71.2,46.8],[-71.1,46.8],[-71.1,46.9],[-71.2,46.8]]]}",
                      &sh, pts, pts + MAX_PTS) && sh.n == 4 && sh.nrings == 1 && pts[MAX_PTS + 2] > 46.89f,
          "polygon n %d", sh.n);
    alarm(0);

    // Which alerts sound: once per warning (alert code), again only if it gets worse; the first fetch only records
    alerts_seen_t seen = {0};
    alerts_t x = { .n = 1 };
    x.a[0] = (alert_t){ .id = "111_fea1-1", .code = "FTA", .colour = 'y' };
    CHECK(alerts_to_sound(&seen, &x) == 0, "start-up: recorded, silent");
    x.a[0] = (alert_t){ .id = "222_fea1-1", .code = "FTA", .colour = 'y' };
    CHECK(alerts_to_sound(&seen, &x) == 0, "re-issued (new id, same warning): silent");
    x.a[0].colour = 'o';
    CHECK(alerts_to_sound(&seen, &x) == 'o', "escalated to orange: sounds");
    CHECK(alerts_to_sound(&seen, &x) == 0, "still orange: silent");
    x.a[0].colour = 'y';
    CHECK(alerts_to_sound(&seen, &x) == 0, "downgraded: silent");
    x.a[0].colour = 'o';
    CHECK(alerts_to_sound(&seen, &x) == 0, "back to orange (already heard at orange): silent");
    x.n = 2;
    x.a[1] = (alert_t){ .id = "333_fea1-1", .code = "SPS", .colour = 'g' };
    CHECK(alerts_to_sound(&seen, &x) == 'g', "a new statement sounds (the level setting decides later)");
    x.a[1] = (alert_t){ .id = "444_fea1-1", .code = "WSW", .colour = 'r' };
    CHECK(alerts_to_sound(&seen, &x) == 'r', "a new red warning");
    char k1[80], k2[80];
    alerts_map_key(&(alert_t){ .id = "111_fea1-1", .code = "FTA", .feature = "fea1-1" }, k1, sizeof(k1));
    alerts_map_key(&(alert_t){ .id = "222_fea1-1", .code = "FTA", .feature = "fea1-1" }, k2, sizeof(k2));
    CHECK(!strcmp(k1, k2), "re-issue keeps the region map (%s, %s)", k1, k2);
    fake_http = (fake_http_t){ .body = features(n5, c5, 5), .status = 200 };
    CHECK(alerts_fetch(46.8, -71.2, &al), "fetch");

    // The region map with no client for the shape download
    fake_http = (fake_http_t){ .no_client = true };
    alert_t a = { .id = "x" };
    CHECK(alerts_map(&a, 46.8, -71.2, 30, 20) == NULL, "map without a client");

    // The region map as main.c asks for it (300 x 200), from a 1500-point region (~40 KB: more than the reply buffer's
    // first 16 KB). What it holds when it draws: the picture and the points, not the reply, nor a copy of the whole
    // cached map (up to v1.14.1: ~830 KB at once, and PSRAM's low point fell to 11-190 KB with an alert)
    fake_http = (fake_http_t){ .body = region(46.8, -71.2, 0.3, 1500), .status = 200 };
    CHECK(strlen(fake_http.body) > 32 * 1024, "the region reply is %zu bytes", strlen(fake_http.body));
    held_base = __sanitizer_get_current_allocated_bytes();
    held_at_map = 0;
    uint16_t *m = alerts_map(&(alert_t){ .id = "x", .colour = 'y' }, 46.8, -71.2, 300, 200);
    CHECK(m, "map drawn");
    CHECK(held_at_map && held_at_map < 160 * 1024, "%zu KB held while drawing the map", held_at_map / 1024);
    printf("alerts: %zu KB held while drawing the map\n", held_at_map / 1024);
    if (m) {
        CHECK(crop.x0 == 83 && crop.y0 == 133 && crop.w == 300 && crop.h == 200, "crop %d,%d %dx%d", crop.x0, crop.y0,
              crop.w, crop.h);
        CHECK(m[0] == pattern(83, 133) && m[300 * 200 - 1] == pattern(83 + 299, 133 + 199),
              "the corners are the cached map's (%04x %04x)", m[0], m[300 * 200 - 1]);
        CHECK(m[100 * 300 + 150] == 0xFFFF, "the location's dot at the centre");
        int filled = 0;                                          // the region (yellow, 35 %) around the dot
        for (int x = 160; x < 290; x++) filled += m[100 * 300 + x] != pattern(83 + x, 133 + 100);
        CHECK(filled > 20, "region filled right of the dot: %d px", filled);
        free(m);
    }
    // A reply over 160 KB: refused (not cut and drawn), nothing kept
    fake_http.body = region(46.8, -71.2, 0.3, 7500);
    CHECK(strlen(fake_http.body) > 160 * 1024, "the big reply is %zu bytes", strlen(fake_http.body));
    CHECK(alerts_map(&(alert_t){ .id = "x", .colour = 'y' }, 46.8, -71.2, 300, 200) == NULL, "a reply over 160 KB");
    return check_done("alerts");
}
