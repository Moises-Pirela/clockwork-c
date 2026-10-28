/* Headless tests for the data/config layer of the post-process stack:
 * the generic tunables module, the post parameter table, config text round
 * trips, order parsing, hot-reload detection, shake and camera motion. No
 * graphics backend is needed (the GPU passes live in r2d_post.c, which is
 * not linked here). */
#define _CRT_SECURE_NO_WARNINGS 1
#include "r2d_post.h"
#include "r2d_lighting.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define EXPECT(cond) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
static bool near_f(float a, float b) { return fabsf(a - b) <= 1e-3f * (1.0f + fabsf(b)); }

static void test_defaults_and_table(void) {
    r2d_post_params p;
    r2d_post_params_defaults(&p);
    EXPECT(p.post.enabled && p.post.hdr && !p.post.apply_to_ui);
    EXPECT(p.bloom.enabled && near_f(p.bloom.threshold, 0.75f) && p.bloom.passes == 3);
    EXPECT(!p.dof.enabled && p.dof.shape == R2D_DOF_RADIAL);
    EXPECT(p.tonemap.op == R2D_TONEMAP_ACES);
    EXPECT(strstr(p.stack.order, "bloom") && strstr(p.stack.order, "shake"));
    size_t count;
    const r2d_tunable *defs = r2d_post_tunable_defs(&count);
    EXPECT(count > 40);
    /* every pass has an enabled switch and every field offset is inside the struct */
    for (int pass = 0; pass < R2D_PASS_COUNT; ++pass) {
        r2d_tunable_set set = r2d_post_tunable_set(&p);
        EXPECT(r2d_tunables_find(&set, r2d_pass_name((r2d_pass)pass), "enabled") != NULL);
    }
    for (size_t i = 0; i < count; ++i) EXPECT(defs[i].offset + 4 <= sizeof(r2d_post_params));
    EXPECT(strcmp(r2d_pass_name(R2D_PASS_CHROMATIC_ABERRATION), "chromatic_aberration") == 0);
}
static void test_apply_text(void) {
    r2d_post_params p;
    r2d_post_params_defaults(&p);
    r2d_tunable_set set = r2d_post_tunable_set(&p);
    const char *text =
        "# comment\n"
        "[bloom]\n"
        "threshold = 1.25\n"
        "passes = 9          ; clamped to the range\n"
        "enabled = off\n"
        "[tonemap]\n"
        "operator = filmic\n"
        "exposure = 2\n"
        "[dof]\n"
        "shape = vertical_band\n"
        "[stack]\n"
        "order = vignette, bloom\n"
        "[nothing]\n"
        "key = 1\n"
        "[bloom]\n"
        "radius = notanumber\n"
        "broken line\n";
    char error[256];
    size_t applied = r2d_tunables_apply_text(&set, text, strlen(text), error, sizeof(error));
    EXPECT(applied == 7);
    EXPECT(near_f(p.bloom.threshold, 1.25f) && p.bloom.passes == 8 && !p.bloom.enabled);
    EXPECT(p.tonemap.op == R2D_TONEMAP_FILMIC && near_f(p.tonemap.exposure, 2.0f));
    EXPECT(p.dof.shape == R2D_DOF_VERTICAL_BAND);
    EXPECT(strcmp(p.stack.order, "vignette, bloom") == 0);
    EXPECT(strstr(error, "unknown nothing.key") && strstr(error, "bad value for bloom.radius") && strstr(error, "bad line"));
    EXPECT(near_f(p.bloom.radius, 1.6f));   /* untouched by the bad value */
    /* enum by index, bool spellings, whitespace */
    EXPECT(r2d_tunables_set(&set, r2d_tunables_find(&set, "tonemap", "operator"), " 1 ") && p.tonemap.op == R2D_TONEMAP_REINHARD);
    EXPECT(r2d_tunables_set(&set, r2d_tunables_find(&set, "vignette", "enabled"), "no") && !p.vignette.enabled);
    EXPECT(!r2d_tunables_set(&set, r2d_tunables_find(&set, "vignette", "enabled"), "maybe"));
    EXPECT(!r2d_tunables_set(&set, r2d_tunables_find(&set, "tonemap", "operator"), "unknown_op"));
    EXPECT(r2d_tunables_find(&set, "BLOOM", "Threshold") != NULL);   /* case-insensitive lookup */
}
static void test_format_roundtrip(void) {
    r2d_post_params a, b;
    r2d_post_params_defaults(&a);
    a.bloom.threshold = 0.333f;
    a.bloom.passes = 5;
    a.dof.enabled = true;
    a.dof.shape = R2D_DOF_HORIZONTAL_BAND;
    a.tonemap.op = R2D_TONEMAP_NONE;
    a.vignette.color_b = 0.25f;
    snprintf(a.stack.order, sizeof(a.stack.order), "shake, tonemap");
    r2d_tunable_set sa = r2d_post_tunable_set(&a);
    static char text[65536];
    size_t n = r2d_tunables_format(&sa, "test header", text, sizeof(text));
    EXPECT(n > 1000 && strstr(text, "# test header") && strstr(text, "[bloom]") && strstr(text, "threshold = 0.333"));
    EXPECT(strstr(text, "shape = horizontal_band") && strstr(text, "operator = none") && strstr(text, "order = shake, tonemap"));
    EXPECT(strstr(text, "[0 .. 4]"));   /* ranges documented */
    memset(&b, 0x55, sizeof(b));
    r2d_post_params_defaults(&b);
    r2d_tunable_set sb = r2d_post_tunable_set(&b);
    char error[256];
    r2d_tunables_apply_text(&sb, text, n, error, sizeof(error));
    EXPECT(error[0] == 0);
    EXPECT(near_f(b.bloom.threshold, 0.333f) && b.bloom.passes == 5 && b.dof.enabled && b.dof.shape == R2D_DOF_HORIZONTAL_BAND);
    EXPECT(b.tonemap.op == R2D_TONEMAP_NONE && near_f(b.vignette.color_b, 0.25f) && strcmp(b.stack.order, "shake, tonemap") == 0);
    /* generic get/adjust used by tweak UIs */
    const r2d_tunable *thr = r2d_tunables_find(&sb, "bloom", "threshold");
    char value[32];
    r2d_tunables_get(&sb, thr, value, sizeof(value));
    EXPECT(strcmp(value, "0.333") == 0);
    r2d_tunables_adjust(&sb, thr, 1);
    EXPECT(near_f(b.bloom.threshold, 0.383f));
    for (int i = 0; i < 200; ++i) r2d_tunables_adjust(&sb, thr, -1);
    EXPECT(near_f(b.bloom.threshold, 0));   /* clamped at the minimum */
    const r2d_tunable *op = r2d_tunables_find(&sb, "tonemap", "operator");
    r2d_tunables_adjust(&sb, op, -1);
    EXPECT(b.tonemap.op == R2D_TONEMAP_FILMIC);   /* enums wrap around */
    EXPECT(r2d_tunables_choice_count(op) == 4);
    r2d_tunables_choice_name(op, 2, value, sizeof(value));
    EXPECT(strcmp(value, "aces") == 0);
    const r2d_tunable *en = r2d_tunables_find(&sb, "shake", "enabled");
    r2d_tunables_adjust(&sb, en, 1);
    EXPECT(!b.shake.enabled);
    r2d_tunables_reset(&sb);
    EXPECT(near_f(b.bloom.threshold, 0.75f) && b.shake.enabled && b.stack.order[0] == 0);
}
static void test_file_roundtrip_and_watch(void) {
    const char *path = "r2d_post_test.cfg";
    r2d_post_params a;
    r2d_post_params_defaults(&a);
    a.chromatic_aberration.strength = 0.0123f;
    r2d_tunable_set sa = r2d_post_tunable_set(&a);
    EXPECT(r2d_tunables_save(&sa, path, "saved by the test"));
    uint64_t stamp = 0;
    EXPECT(r2d_tunables_file_changed(path, &stamp));      /* first poll always reports a change */
    EXPECT(!r2d_tunables_file_changed(path, &stamp));     /* unchanged since */
    r2d_post_params b;
    r2d_post_params_defaults(&b);
    r2d_tunable_set sb = r2d_post_tunable_set(&b);
    char error[256];
    EXPECT(r2d_tunables_load(&sb, path, error, sizeof(error)) && error[0] == 0);
    EXPECT(near_f(b.chromatic_aberration.strength, 0.0123f));
    /* append a line: size changes, so the watcher notices */
    FILE *f = fopen(path, "ab");
    EXPECT(f != NULL);
    if (f) { fputs("\n[vignette]\nintensity = 0.1\n", f); fclose(f); }
    EXPECT(r2d_tunables_file_changed(path, &stamp));
    EXPECT(r2d_tunables_load(&sb, path, error, sizeof(error)) && near_f(b.vignette.intensity, 0.1f));
    EXPECT(!r2d_tunables_load(&sb, "no/such/dir/x.cfg", error, sizeof(error)) && strstr(error, "cannot open"));
    remove(path);
    EXPECT(!r2d_tunables_file_changed(path, &stamp));    /* missing file: no change reported */
}
static void test_order(void) {
    r2d_pass passes[R2D_PASS_COUNT];
    size_t unknown = 99;
    size_t n = r2d_post_parse_order("vignette,bloom , shake\n tonemap bogus, bloom", passes, R2D_PASS_COUNT, &unknown);
    EXPECT(n == 4 && unknown == 1);
    EXPECT(passes[0] == R2D_PASS_VIGNETTE && passes[1] == R2D_PASS_BLOOM && passes[2] == R2D_PASS_SHAKE && passes[3] == R2D_PASS_TONEMAP);
    EXPECT(r2d_post_parse_order("", passes, R2D_PASS_COUNT, &unknown) == 0 && unknown == 0);
    EXPECT(r2d_post_parse_order(NULL, passes, R2D_PASS_COUNT, NULL) == 0);
    r2d_post_params p;
    r2d_post_params_defaults(&p);
    n = r2d_post_parse_order(p.stack.order, passes, R2D_PASS_COUNT, &unknown);
    EXPECT(n == R2D_PASS_COUNT && unknown == 0);   /* the default order names every pass once */
    char text[128];
    r2d_post_format_order(passes, n, text, sizeof(text));
    EXPECT(strcmp(text, p.stack.order) == 0);
}
static void test_shake_and_motion(void) {
    r2d_shake_state s = {0, 0, 7};
    r2d_vec2 off;
    float angle;
    r2d_shake_sample(&s, 20, 1, 30, 2, &off, &angle);
    EXPECT(off.x == 0 && off.y == 0 && angle == 0);     /* no trauma, no shake */
    r2d_shake_add(&s, 0.6f);
    r2d_shake_add(&s, 0.6f);
    EXPECT(near_f(s.trauma, 1.0f));                      /* clamped */
    float max_seen = 0;
    for (int i = 0; i < 200; ++i) {
        r2d_shake_update(&s, 1.0f / 120.0f, 0);           /* no decay: sample the noise */
        r2d_shake_sample(&s, 20, 1, 30, 2, &off, &angle);
        EXPECT(fabsf(off.x) <= 20.0001f && fabsf(off.y) <= 20.0001f && fabsf(angle) <= 1.0001f);
        if (fabsf(off.x) > max_seen) max_seen = fabsf(off.x);
    }
    EXPECT(max_seen > 5.0f);                              /* it does move */
    r2d_shake_state t = s;
    r2d_shake_sample(&s, 20, 1, 30, 2, &off, &angle);
    r2d_vec2 off2;
    r2d_shake_sample(&t, 20, 1, 30, 2, &off2, NULL);
    EXPECT(off.x == off2.x && off.y == off2.y);          /* deterministic */
    s.trauma = 0.5f;
    r2d_shake_update(&s, 0.5f, 0.4f);
    EXPECT(near_f(s.trauma, 0.3f));                      /* decays linearly */
    r2d_shake_update(&s, 10.0f, 1.0f);
    EXPECT(s.trauma == 0);
    /* exponent: half trauma gives a quarter of the amplitude at exponent 2 */
    s.trauma = 1.0f; r2d_shake_sample(&s, 20, 0, 30, 2, &off, NULL);
    float full = off.x;
    s.trauma = 0.5f; r2d_shake_sample(&s, 20, 0, 30, 2, &off, NULL);
    EXPECT(near_f(off.x, full * 0.25f));

    r2d_camera a = r2d_camera_make(r2d_v2(100, 100), 2.0f, false), b = a;
    r2d_vec2 d;
    float ratio;
    r2d_motion_from_cameras(&a, &b, r2d_v2(800, 600), &d, &ratio);
    EXPECT(d.x == 0 && d.y == 0 && near_f(ratio, 1));
    b.center = r2d_v2(110, 95);                           /* camera moved right/up: content moves left/down */
    r2d_motion_from_cameras(&a, &b, r2d_v2(800, 600), &d, &ratio);
    EXPECT(near_f(d.x, -20) && near_f(d.y, 10));
    b.zoom = 4.0f;
    r2d_motion_from_cameras(&a, &b, r2d_v2(800, 600), &d, &ratio);
    EXPECT(near_f(ratio, 2.0f));
}

static bool within(float a, float b, float eps) { return fabsf(a - b) <= eps; }
static void test_shadow_maps(void) {
    enum { N = 720 };
    static float map[N];
    r2d_vec2 L = r2d_v2(100, 100);
    /* circle to the right: center (200,100) r 10 -> blocked at ~90 for angle 0, free at 90 degrees */
    r2d_circle c = {r2d_v2(200, 100), 10};
    size_t tests = r2d_shadow_map_build(L, 300, &c, 1, NULL, 0, map, N);
    EXPECT(tests > 0 && tests < N);          /* only the subtended arc was tested */
    EXPECT(within(r2d_shadow_map_sample(map, N, 0), 90, 0.5f));
    EXPECT(within(r2d_shadow_map_sample(map, N, 1.5708f), 300, 0.001f));
    EXPECT(within(r2d_shadow_map_sample(map, N, 3.14159f), 300, 0.001f));
    float half = asinf(10.0f / 100.0f);
    EXPECT(r2d_shadow_map_sample(map, N, half * 0.9f) < 300);
    EXPECT(within(r2d_shadow_map_sample(map, N, half * 1.2f), 300, 0.001f));
    /* segment below, wrapping around the +-pi seam: from (-50,150) to (250,150) relative... use absolute coords */
    r2d_segment s = {r2d_v2(50, 150), r2d_v2(250, 150)};
    tests = r2d_shadow_map_build(L, 300, NULL, 0, &s, 1, map, N);
    EXPECT(within(r2d_shadow_map_sample(map, N, 1.5708f), 50, 0.5f));     /* straight down (+y) */
    EXPECT(within(r2d_shadow_map_sample(map, N, 0.7854f), 70.71f, 1.0f));  /* 45 degrees */
    EXPECT(within(r2d_shadow_map_sample(map, N, -1.5708f), 300, 0.001f));  /* up: free */
    /* segment crossing the angle seam at -x: vertical wall left of the light */
    r2d_segment w = {r2d_v2(20, 0), r2d_v2(20, 200)};
    r2d_shadow_map_build(L, 300, NULL, 0, &w, 1, map, N);
    EXPECT(within(r2d_shadow_map_sample(map, N, 3.14159f), 80, 0.5f));
    EXPECT(within(r2d_shadow_map_sample(map, N, 3.14159f + 0.5f), 80 / cosf(0.5f), 1.0f));
    EXPECT(within(r2d_shadow_map_sample(map, N, 0), 300, 0.001f));
    /* light inside a circle: fully dark; out of reach occluders ignored */
    r2d_circle in = {r2d_v2(101, 100), 5}, far = {r2d_v2(1000, 100), 5};
    r2d_shadow_map_build(L, 300, &in, 1, NULL, 0, map, N);
    EXPECT(map[0] == 0 && map[N / 2] == 0);
    tests = r2d_shadow_map_build(L, 300, &far, 1, NULL, 0, map, N);
    EXPECT(tests == 0 && map[0] == 300);
    /* nearest of several wins */
    r2d_circle two[2] = {{r2d_v2(250, 100), 10}, {r2d_v2(150, 100), 10}};
    r2d_shadow_map_build(L, 300, two, 2, NULL, 0, map, N);
    EXPECT(within(map[0], 40, 0.5f));
    /* ray helpers */
    EXPECT(within(r2d_ray_circle(r2d_v2(0, 0), r2d_v2(1, 0), (r2d_circle){r2d_v2(10, 0), 2}), 8, 1e-4f));
    EXPECT(r2d_ray_circle(r2d_v2(0, 0), r2d_v2(-1, 0), (r2d_circle){r2d_v2(10, 0), 2}) < 0);
    EXPECT(within(r2d_ray_segment(r2d_v2(0, 0), r2d_v2(0, 1), (r2d_segment){r2d_v2(-1, 5), r2d_v2(1, 5)}), 5, 1e-4f));
    EXPECT(r2d_ray_segment(r2d_v2(0, 0), r2d_v2(0, 1), (r2d_segment){r2d_v2(2, 5), r2d_v2(3, 5)}) < 0);
    EXPECT(r2d_ray_segment(r2d_v2(0, 0), r2d_v2(1, 0), (r2d_segment){r2d_v2(0, 5), r2d_v2(1, 5)}) < 0);   /* parallel */
}

static void test_quality_tiers(void) {
    /* the tunables table and r2d_tier_defaults agree */
    r2d_post_params p;
    r2d_post_params_defaults(&p);
    for (int t = 0; t < R2D_TIER_COUNT; ++t) {
        r2d_tier_settings d;
        r2d_tier_defaults((r2d_tier)t, &d);
        EXPECT(memcmp(&d, &p.tiers[t], sizeof(d)) == 0);
    }
    EXPECT(strcmp(r2d_tier_name(R2D_TIER_LOW), "low") == 0 && strcmp(r2d_tier_name(R2D_TIER_ULTRA), "ultra") == 0);
    EXPECT(p.quality.tier == 0 && p.quality.auto_adjust && near_f(p.quality.target_fps, 60));
    /* the low tier clamps the cost knobs but leaves artistic values alone */
    r2d_post_params low = p;
    low.bloom.threshold = 0.123f;
    low.bloom.passes = 5;
    low.dof.enabled = true;
    low.motion_blur.samples = 32;
    low.lighting.shadow_angles = 2000;
    r2d_tier_apply(&p.tiers[R2D_TIER_LOW], &low);
    EXPECT(near_f(low.bloom.threshold, 0.123f));
    EXPECT(low.bloom.passes == 1 && low.bloom.downscale == 4 && !low.post.hdr && near_f(low.post.render_scale, 0.7f));
    EXPECT(!low.dof.enabled && !low.motion_blur.enabled && !low.fog.enabled && low.lighting.enabled);
    EXPECT(low.lighting.shadow_angles == 180 && low.lighting.downscale == 4 && low.lighting.max_lights == 12 && low.fog.octaves == 2);
    /* ultra does not reduce the defaults; a smaller artistic value is kept */
    r2d_post_params ultra = p;
    ultra.bloom.passes = 2;
    ultra.post.render_scale = 0.5f;
    r2d_tier_apply(&p.tiers[R2D_TIER_ULTRA], &ultra);
    EXPECT(ultra.bloom.passes == 2 && near_f(ultra.post.render_scale, 0.5f) && ultra.post.hdr && ultra.dof.enabled == p.dof.enabled);
    EXPECT(ultra.lighting.shadow_angles == p.lighting.shadow_angles && ultra.motion_blur.samples == p.motion_blur.samples);
    /* the tier sections round-trip through the config text */
    r2d_post_params a = p, b;
    a.tiers[R2D_TIER_MEDIUM].shadow_angles = 333;
    a.tiers[R2D_TIER_LOW].allow_lighting = false;
    a.quality.tier = 3;   /* high */
    r2d_tunable_set sa = r2d_post_tunable_set(&a);
    static char text[65536];
    size_t n = r2d_tunables_format(&sa, NULL, text, sizeof(text));
    EXPECT(strstr(text, "[tier_medium]") && strstr(text, "tier = high") && strstr(text, "shadow_angles = 333"));
    r2d_post_params_defaults(&b);
    r2d_tunable_set sb = r2d_post_tunable_set(&b);
    char error[256];
    r2d_tunables_apply_text(&sb, text, n, error, sizeof(error));
    EXPECT(error[0] == 0 && b.tiers[R2D_TIER_MEDIUM].shadow_angles == 333 && !b.tiers[R2D_TIER_LOW].allow_lighting && b.quality.tier == 3);
    /* initial pick */
    EXPECT(r2d_quality_pick_initial(true, 1280, 800) == R2D_TIER_HIGH);
    EXPECT(r2d_quality_pick_initial(false, 1280, 800) == R2D_TIER_MEDIUM);
    EXPECT(r2d_quality_pick_initial(false, 3840, 2160) == R2D_TIER_LOW);
    EXPECT(r2d_quality_pick_initial(true, 3840, 2160) == R2D_TIER_MEDIUM);
}
static void test_quality_controller(void) {
    r2d_quality_controller c;
    r2d_quality_init(&c, R2D_TIER_HIGH, R2D_TIER_LOW, R2D_TIER_ULTRA);
    EXPECT(c.tier == R2D_TIER_HIGH && c.min_tier == R2D_TIER_LOW && c.max_tier == R2D_TIER_ULTRA);
    /* warm-up: nothing happens during the first seconds even when slow */
    int changed = 0;
    for (int i = 0; i < 60; ++i) changed += r2d_quality_update(&c, 0.020f, 60);   /* 1.2 s at 50 fps */
    EXPECT(changed == 0 && c.tier == R2D_TIER_HIGH);
    /* sustained 30 ms frames at a 60 fps target: drop one tier after about a second, then cool down */
    int drops = 0, frames_to_drop = 0;
    for (int i = 0; i < 200 && !drops; ++i) { if (r2d_quality_update(&c, 0.030f, 60) < 0) ++drops; ++frames_to_drop; }
    EXPECT(drops == 1 && c.tier == R2D_TIER_MEDIUM && frames_to_drop > 30 && frames_to_drop < 120);
    for (int i = 0; i < 30; ++i) drops += r2d_quality_update(&c, 0.030f, 60) < 0;   /* 0.9 s: still cooling down */
    EXPECT(drops == 1);
    for (int i = 0; i < 300; ++i) drops += r2d_quality_update(&c, 0.030f, 60) < 0;
    EXPECT(drops == 2 && c.tier == R2D_TIER_LOW && c.changes == 2);                 /* bottoms out at the minimum */
    for (int i = 0; i < 300; ++i) drops += r2d_quality_update(&c, 0.030f, 60) < 0;
    EXPECT(drops == 2 && c.tier == R2D_TIER_LOW);
    /* plenty of headroom: climbs back one tier per ~6 s plus cooldown */
    int raises = 0;
    for (int i = 0; i < 6000; ++i) raises += r2d_quality_update(&c, 0.005f, 60) > 0;   /* 30 s of 5 ms frames */
    EXPECT(raises >= 2 && c.tier >= R2D_TIER_HIGH);
    /* a stall is clamped and does not trigger a drop by itself */
    r2d_quality_init(&c, R2D_TIER_HIGH, R2D_TIER_LOW, R2D_TIER_ULTRA);
    for (int i = 0; i < 200; ++i) r2d_quality_update(&c, 0.010f, 60);
    EXPECT(r2d_quality_update(&c, 2.0f, 60) == 0 && c.tier == R2D_TIER_HIGH && c.ema_ms < 40);
    for (int i = 0; i < 30; ++i) r2d_quality_update(&c, 0.010f, 60);
    EXPECT(c.tier == R2D_TIER_HIGH && c.over_seconds == 0);
    /* bounds and bad inputs */
    r2d_quality_init(&c, R2D_TIER_ULTRA, R2D_TIER_MEDIUM, R2D_TIER_HIGH);
    EXPECT(c.tier == R2D_TIER_HIGH);
    r2d_quality_init(&c, R2D_TIER_LOW, R2D_TIER_HIGH, R2D_TIER_MEDIUM);   /* swapped bounds */
    EXPECT(c.min_tier == R2D_TIER_MEDIUM && c.max_tier == R2D_TIER_HIGH && c.tier == R2D_TIER_MEDIUM);
    EXPECT(r2d_quality_update(&c, 0, 60) == 0 && r2d_quality_update(&c, 0.016f, 0) == 0 && r2d_quality_update(NULL, 0.016f, 60) == 0);
}

int main(void) {
    test_quality_tiers();
    test_quality_controller();
    test_shadow_maps();
    test_defaults_and_table();
    test_apply_text();
    test_format_roundtrip();
    test_file_roundtrip_and_watch();
    test_order();
    test_shake_and_motion();
    if (failures) { printf("%d of %d post-process config checks failed\n", failures, checks); return 1; }
    printf("OK: %d post-process config checks passed\n", checks);
    return 0;
}
