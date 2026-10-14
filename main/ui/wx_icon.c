#include "wx_icon.h"

#define C_SUN        lv_color_hex(0xFDB813)
#define C_MOON       lv_color_hex(0xE2E8F0)
#define C_CLOUD      lv_color_hex(0xCBD5E1)
#define C_CLOUD_DARK lv_color_hex(0x7C8BA1)
#define C_RAIN       lv_color_hex(0x60A5FA)
#define C_SNOW       lv_color_hex(0xF8FAFC)
#define C_BOLT       lv_color_hex(0xFACC15)
#define C_FOG        lv_color_hex(0x94A3B8)

typedef enum {
    SKY_CLEAR,
    SKY_PARTLY,
    SKY_CLOUDY,
    SKY_FOG,
    SKY_DRIZZLE,
    SKY_RAIN,
    SKY_SNOW,
    SKY_STORM,
} sky_t;

static sky_t classify(int code)
{
    if (code == 0) return SKY_CLEAR;
    if (code <= 2) return SKY_PARTLY;
    if (code == 3) return SKY_CLOUDY;
    if (code == 45 || code == 48) return SKY_FOG;
    if (code >= 51 && code <= 57) return SKY_DRIZZLE;
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return SKY_RAIN;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return SKY_SNOW;
    if (code >= 95) return SKY_STORM;
    return SKY_CLOUDY;
}

static lv_obj_t *circle(lv_obj_t *parent, int32_t x, int32_t y, int32_t d, lv_color_t c)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, lv_color_t c)
{
    lv_obj_t *o = circle(parent, x, y, w, c);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    return o;
}

static void free_points(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

static void line(lv_obj_t *parent, const lv_point_precise_t *pts, int n, int32_t width, lv_color_t c)
{
    // lv_line keeps a pointer to the points, so give each line its own copy.
    lv_point_precise_t *copy = lv_malloc(sizeof(*copy) * n);
    lv_memcpy(copy, pts, sizeof(*copy) * n);
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, copy, n);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, c, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_clickable(l, false);
    lv_obj_add_event_cb(l, free_points, LV_EVENT_DELETE, copy);
}

static void sun(lv_obj_t *p, int32_t cx, int32_t cy, int32_t d)
{
    lv_obj_t *s = circle(p, cx - d / 2, cy - d / 2, d, C_SUN);
    lv_obj_set_style_shadow_color(s, C_SUN, 0);
    lv_obj_set_style_shadow_width(s, d / 3, 0);
    lv_obj_set_style_shadow_opa(s, LV_OPA_40, 0);
}

// Colour actually painted behind `o`: the first opaque ancestor, with any translucent layers
// in between (e.g. the tinted home badge) blended on top.
static lv_color_t bg_under(lv_obj_t *o)
{
    lv_obj_t *layers[12];
    int n = 0;
    lv_color_t c = lv_color_black();
    for (; o; o = lv_obj_get_parent(o)) {
        const lv_opa_t opa = lv_obj_get_style_bg_opa(o, LV_PART_MAIN);
        if (opa >= LV_OPA_MAX) {
            c = lv_obj_get_style_bg_color(o, LV_PART_MAIN);
            break;
        }
        if (opa > LV_OPA_MIN && n < 12) {
            layers[n++] = o;
        }
    }
    while (n--) {
        c = lv_color_mix(lv_obj_get_style_bg_color(layers[n], LV_PART_MAIN), c,
                         lv_obj_get_style_bg_opa(layers[n], LV_PART_MAIN));
    }
    return c;
}

static void moon(lv_obj_t *p, int32_t cx, int32_t cy, int32_t d)
{
    // Crescent: a full disc with a background-coloured disc cut out of its upper right.
    // (A constant-width arc reads as a loading spinner.)
    circle(p, cx - d / 2, cy - d / 2, d, C_MOON);
    const int32_t cut = d * 80 / 100;
    circle(p, cx - d / 2 + d * 36 / 100, cy - d / 2 - d * 16 / 100, cut, bg_under(p));
}

// Cloud fitted in a box at (x, y) of width w; height is ~0.6 w.
static void cloud(lv_obj_t *p, int32_t x, int32_t y, int32_t w, lv_color_t c)
{
    int32_t base_h = w * 30 / 100;
    int32_t base_y = y + w * 30 / 100;
    circle(p, x + w * 12 / 100, y + w * 18 / 100, w * 36 / 100, c);
    circle(p, x + w * 32 / 100, y, w * 46 / 100, c);
    circle(p, x + w * 58 / 100, y + w * 14 / 100, w * 34 / 100, c);
    rect(p, x, base_y, w, base_h, base_h / 2, c);
}

static void drops(lv_obj_t *p, int32_t x, int32_t y, int32_t w, int n, int32_t len, int32_t width)
{
    for (int i = 0; i < n; i++) {
        int32_t dx = x + w * (i + 1) / (n + 1);
        lv_point_precise_t pts[] = { { dx + len / 3, y }, { dx, y + len } };
        line(p, pts, 2, width, C_RAIN);
    }
}

static void flakes(lv_obj_t *p, int32_t x, int32_t y, int32_t w, int32_t d)
{
    for (int i = 0; i < 3; i++) {
        int32_t dx = x + w * (i + 1) / 4 - d / 2;
        circle(p, dx, y + (i % 2) * d * 3 / 2, d, C_SNOW);
    }
}

lv_obj_t *wx_icon_create(lv_obj_t *parent, int32_t size, int code, bool is_day)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, size, size);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_clickable(box, false);

    const int32_t s = size;
    const sky_t sky = classify(code);
    const int32_t cloud_w = s * 84 / 100;
    const int32_t cloud_x = (s - cloud_w) / 2;
    const int32_t stroke = LV_MAX(2, s / 22);

    switch (sky) {
    case SKY_CLEAR:
        if (is_day) {
            sun(box, s / 2, s / 2, s * 58 / 100);
        } else {
            moon(box, s / 2, s / 2, s * 62 / 100);
        }
        break;
    case SKY_PARTLY:
        if (is_day) {
            sun(box, s * 38 / 100, s * 36 / 100, s * 46 / 100);
        } else {
            moon(box, s * 38 / 100, s * 36 / 100, s * 46 / 100);
        }
        cloud(box, s * 22 / 100, s * 40 / 100, s * 74 / 100, C_CLOUD);
        break;
    case SKY_CLOUDY:
        cloud(box, cloud_x, s * 22 / 100, cloud_w, C_CLOUD);
        break;
    case SKY_FOG: {
        cloud(box, cloud_x, s * 8 / 100, cloud_w, C_CLOUD);
        for (int i = 0; i < 3; i++) {
            int32_t y = s * (66 + i * 12) / 100;
            int32_t inset = (i == 1) ? s / 10 : 0;
            lv_point_precise_t pts[] = { { s / 8 + inset, y }, { s * 7 / 8 - inset, y } };
            line(box, pts, 2, stroke, C_FOG);
        }
        break;
    }
    case SKY_DRIZZLE:
        cloud(box, cloud_x, s * 10 / 100, cloud_w, C_CLOUD);
        drops(box, cloud_x, s * 66 / 100, cloud_w, 3, s * 14 / 100, stroke);
        break;
    case SKY_RAIN:
        cloud(box, cloud_x, s * 8 / 100, cloud_w, C_CLOUD_DARK);
        drops(box, cloud_x, s * 64 / 100, cloud_w, 4, s * 24 / 100, stroke);
        break;
    case SKY_SNOW:
        cloud(box, cloud_x, s * 8 / 100, cloud_w, C_CLOUD);
        flakes(box, cloud_x, s * 66 / 100, cloud_w, LV_MAX(4, s / 9));
        break;
    case SKY_STORM: {
        cloud(box, cloud_x, s * 6 / 100, cloud_w, C_CLOUD_DARK);
        lv_point_precise_t bolt[] = {
            { s * 54 / 100, s * 56 / 100 }, { s * 42 / 100, s * 76 / 100 },
            { s * 56 / 100, s * 76 / 100 }, { s * 44 / 100, s * 96 / 100 },
        };
        line(box, bolt, 4, LV_MAX(3, s / 14), C_BOLT);
        drops(box, cloud_x, s * 62 / 100, cloud_w / 3, 1, s * 18 / 100, stroke);
        drops(box, cloud_x + cloud_w * 2 / 3, s * 62 / 100, cloud_w / 3, 1, s * 18 / 100, stroke);
        break;
    }
    }
    return box;
}
