/* Headless tests for the pure parts of render2d: vectors, rectangles,
 * colors and camera math. No window or graphics backend is needed. */
#include "render2d.h"
#include <stdio.h>
#include <stdlib.h>

static int failures, checks;
#define EXPECT(cond) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
static bool near_f(float a, float b) { return fabsf(a - b) <= 1e-3f * (1.0f + fabsf(b)); }
#define NEAR(a, b) near_f((float)(a), (float)(b))
#define NEAR_V(a, b) (NEAR((a).x, (b).x) && NEAR((a).y, (b).y))

static void test_rects(void) {
    r2d_rect r = r2d_rect_from_points(r2d_v2(10, 20), r2d_v2(-5, 2));
    EXPECT(NEAR(r.x, -5) && NEAR(r.y, 2) && NEAR(r.w, 15) && NEAR(r.h, 18));
    EXPECT(r2d_rect_contains(r, r2d_v2(0, 10)));
    EXPECT(!r2d_rect_contains(r, r2d_v2(11, 10)));
    EXPECT(r2d_rect_overlaps(r, r2d_rect_make(9, 19, 5, 5)));
    EXPECT(!r2d_rect_overlaps(r, r2d_rect_make(11, 19, 5, 5)));
    EXPECT(r2d_rect_overlaps_circle(r, r2d_v2(12, 10), 2.5f));
    EXPECT(!r2d_rect_overlaps_circle(r, r2d_v2(12, 10), 1.5f));
    EXPECT(r2d_rect_overlaps_circle(r, r2d_v2(0, 10), 0));
    r2d_rect e = r2d_rect_expand(r, 1);
    EXPECT(NEAR(e.x, -6) && NEAR(e.w, 17));
    r2d_vec2 c = r2d_rect_center(r2d_rect_centered(r2d_v2(3, 4), 10, 20));
    EXPECT(NEAR_V(c, r2d_v2(3, 4)));
}
static void test_colors(void) {
    r2d_color a = r2d_rgb(0, 100, 200), b = r2d_rgba(100, 200, 0, 0);
    r2d_color m = r2d_color_mix(a, b, 0.5f);
    EXPECT(m.r == 50 && m.g == 150 && m.b == 100 && m.a == 127);
    EXPECT(r2d_color_mix(a, b, -1).r == 0 && r2d_color_mix(a, b, 2).r == 100);
    EXPECT(r2d_with_alpha(a, 7).a == 7 && r2d_with_alpha(a, 7).b == 200);
}
static void test_camera_transforms(void) {
    r2d_vec2 viewport = r2d_v2(800, 600);
    for (int y_up = 0; y_up < 2; ++y_up) {
        r2d_camera c = r2d_camera_make(r2d_v2(100, 50), 2.0f, y_up != 0);
        /* the center maps to the viewport center */
        EXPECT(NEAR_V(r2d_camera_world_to_screen(&c, viewport, c.center), r2d_v2(400, 300)));
        /* round trips */
        r2d_vec2 p = r2d_v2(-37.5f, 412.25f);
        r2d_vec2 s = r2d_camera_world_to_screen(&c, viewport, p);
        EXPECT(NEAR_V(r2d_camera_screen_to_world(&c, viewport, s), p));
        r2d_vec2 back = r2d_camera_screen_to_world(&c, viewport, r2d_v2(13, 17));
        EXPECT(NEAR_V(r2d_camera_world_to_screen(&c, viewport, back), r2d_v2(13, 17)));
        /* y orientation */
        r2d_vec2 above = r2d_camera_world_to_screen(&c, viewport, r2d_v2(100, 60));
        EXPECT(y_up ? above.y < 300 : above.y > 300);
        EXPECT(NEAR(fabsf(above.y - 300), 20));
        /* deltas */
        r2d_vec2 d = r2d_camera_screen_delta_to_world(&c, r2d_v2(10, 10));
        EXPECT(NEAR(d.x, 5) && NEAR(fabsf(d.y), 5) && ((d.y < 0) == (y_up != 0)));
        EXPECT(NEAR_V(r2d_camera_world_delta_to_screen(&c, d), r2d_v2(10, 10)));
        /* visible rect covers exactly viewport / zoom */
        r2d_rect v = r2d_camera_visible_rect(&c, viewport);
        EXPECT(NEAR(v.w, 400) && NEAR(v.h, 300) && NEAR_V(r2d_rect_center(v), c.center));
    }
}
static void test_camera_pan_zoom(void) {
    r2d_vec2 viewport = r2d_v2(1000, 500);
    r2d_camera c = r2d_camera_make(r2d_v2(0, 0), 1.0f, false);
    /* panning by a screen delta keeps the world point under the cursor */
    r2d_vec2 cursor = r2d_v2(120, 340);
    r2d_vec2 under = r2d_camera_screen_to_world(&c, viewport, cursor);
    r2d_camera_pan_screen(&c, r2d_v2(30, -40));
    EXPECT(NEAR_V(r2d_camera_screen_to_world(&c, viewport, r2d_add(cursor, r2d_v2(30, -40))), under));
    /* zooming about an anchor keeps the world point under the anchor */
    r2d_vec2 anchor = r2d_v2(900, 50);
    under = r2d_camera_screen_to_world(&c, viewport, anchor);
    r2d_camera_zoom_at(&c, viewport, anchor, 3.0f);
    EXPECT(NEAR(c.zoom, 3.0f));
    EXPECT(NEAR_V(r2d_camera_screen_to_world(&c, viewport, anchor), under));
    r2d_camera_zoom_at(&c, viewport, anchor, 1.0f / 3.0f);
    EXPECT(NEAR(c.zoom, 1.0f));
    EXPECT(NEAR_V(r2d_camera_screen_to_world(&c, viewport, anchor), under));
    /* zoom is clamped */
    c.min_zoom = 0.5f;
    c.max_zoom = 4.0f;
    r2d_camera_zoom_at(&c, viewport, anchor, 100.0f);
    EXPECT(NEAR(c.zoom, 4.0f));
    r2d_camera_zoom_at(&c, viewport, anchor, 0.0001f);
    EXPECT(NEAR(c.zoom, 0.5f));
    EXPECT(NEAR(r2d_camera_clamp_zoom(&c, 2.0f), 2.0f));
    /* y_up zoom also keeps the anchor */
    r2d_camera u = r2d_camera_make(r2d_v2(5, 5), 2.0f, true);
    under = r2d_camera_screen_to_world(&u, viewport, anchor);
    r2d_camera_zoom_at(&u, viewport, anchor, 0.25f);
    EXPECT(NEAR_V(r2d_camera_screen_to_world(&u, viewport, anchor), under));
}
static void test_camera_fit(void) {
    r2d_vec2 viewport = r2d_v2(1280, 800);
    r2d_rect world = r2d_rect_make(0, 0, 1200, 800);
    for (int y_up = 0; y_up < 2; ++y_up) {
        r2d_camera c = r2d_camera_make(r2d_v2(0, 0), 1.0f, y_up != 0);
        r2d_camera_fit(&c, viewport, world, 0);
        EXPECT(NEAR(c.zoom, 1.0f));   /* height-limited: 800 / 800 */
        EXPECT(NEAR_V(c.center, r2d_v2(600, 400)));
        /* fitting into a sub-area: the world rect lands inside that area */
        r2d_rect area = r2d_rect_make(280, 0, 1000, 740);
        r2d_camera_fit_in(&c, viewport, area, world, 20);
        r2d_vec2 a = r2d_camera_world_to_screen(&c, viewport, r2d_rect_min(world));
        r2d_vec2 b = r2d_camera_world_to_screen(&c, viewport, r2d_rect_max(world));
        r2d_rect on_screen = r2d_rect_from_points(a, b);
        EXPECT(on_screen.x >= area.x + 19 && on_screen.x + on_screen.w <= area.x + area.w - 19);
        EXPECT(on_screen.y >= area.y + 19 && on_screen.y + on_screen.h <= area.y + area.h - 19);
        EXPECT(NEAR_V(r2d_rect_center(on_screen), r2d_rect_center(area)));
        EXPECT(NEAR(on_screen.w, 960) || NEAR(on_screen.h, 700));   /* one axis is tight */
        /* degenerate inputs leave the camera alone */
        r2d_camera before = c;
        r2d_camera_fit(&c, viewport, r2d_rect_make(0, 0, 0, 10), 0);
        r2d_camera_fit_in(&c, viewport, r2d_rect_make(0, 0, 10, 10), world, 20);
        EXPECT(NEAR(before.zoom, c.zoom) && NEAR_V(before.center, c.center));
    }
}
static void test_vectors(void) {
    EXPECT(NEAR(r2d_length(r2d_v2(3, 4)), 5));
    EXPECT(NEAR_V(r2d_normalize(r2d_v2(0, 0)), r2d_v2(0, 0)));
    EXPECT(NEAR(r2d_length(r2d_normalize(r2d_v2(-7, 2))), 1));
    EXPECT(NEAR_V(r2d_lerp(r2d_v2(0, 10), r2d_v2(10, 0), 0.25f), r2d_v2(2.5f, 7.5f)));
    EXPECT(NEAR(r2d_dot(r2d_v2(1, 2), r2d_v2(3, 4)), 11));
    EXPECT(NEAR(r2d_distance(r2d_v2(1, 1), r2d_v2(4, 5)), 5));
    EXPECT(NEAR(r2d_clampf(5, 0, 1), 1) && NEAR(r2d_clampf(-5, 0, 1), 0) && NEAR(r2d_clampf(0.5f, 0, 1), 0.5f));
}

int main(void) {
    test_vectors();
    test_rects();
    test_colors();
    test_camera_transforms();
    test_camera_pan_zoom();
    test_camera_fit();
    if (failures) { printf("%d of %d render2d checks failed\n", failures, checks); return 1; }
    printf("OK: %d render2d checks passed\n", checks);
    return 0;
}
