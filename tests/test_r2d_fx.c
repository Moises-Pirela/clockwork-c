/* Headless tests for r2d_particles and r2d_effects. The modules only call
 * the renderer from their draw functions, so this file provides recording
 * stubs for those entry points and needs no window or graphics backend. */
#include "r2d_particles.h"
#include "r2d_effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define EXPECT(cond) do { ++checks; if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
static bool near_f(float a, float b) { return fabsf(a - b) <= 1e-3f * (1.0f + fabsf(b)); }

/* --- recording renderer stubs --- */
static struct { int circles, rings, lines, rects, orects, texts, arcs; r2d_space space; r2d_rect last_rect; char last_text[64]; } stub;
static r2d_camera stub_camera;
void r2d_draw_circle(r2d_vec2 c, float r, r2d_color col) { (void)c; (void)r; (void)col; ++stub.circles; }
void r2d_draw_ring(r2d_vec2 c, float r, float t, r2d_color col) { (void)c; (void)r; (void)t; (void)col; ++stub.rings; }
void r2d_draw_arc(r2d_vec2 c, float r, float t, float a, float b, r2d_color col) { (void)c; (void)r; (void)t; (void)a; (void)b; (void)col; ++stub.arcs; }
void r2d_draw_line(r2d_vec2 a, r2d_vec2 b, float t, r2d_color col) { (void)a; (void)b; (void)t; (void)col; ++stub.lines; }
void r2d_draw_rect(r2d_rect r, r2d_color col) { (void)col; stub.last_rect = r; ++stub.rects; }
void r2d_draw_oriented_rect(r2d_vec2 c, r2d_vec2 d, float w, float h, r2d_color col) { (void)c; (void)d; (void)w; (void)h; (void)col; ++stub.orects; }
void r2d_draw_text_centered(r2d_vec2 p, r2d_color col, const char *text) { (void)p; (void)col; snprintf(stub.last_text, sizeof(stub.last_text), "%s", text); ++stub.texts; }
r2d_vec2 r2d_viewport(void) { return r2d_v2(800, 600); }
void r2d_set_space(r2d_space s) { stub.space = s; }
r2d_space r2d_get_space(void) { return stub.space; }
r2d_camera *r2d_camera_ptr(void) { return &stub_camera; }
static void reset_stub(void) { memset(&stub, 0, sizeof(stub)); }

/* --- particles --- */
static void test_particle_bursts(void) {
    r2d_particles *ps = r2d_particles_create(100, 4, 1);
    r2d_particle_desc d = r2d_particle_desc_default();
    d.lifetime = r2d_range_const(1.0f);
    d.speed = r2d_range_const(10.0f);
    d.direction = 0;
    d.spread = 0;
    d.gravity = r2d_v2(0, 0);
    r2d_particles_burst(ps, &d, r2d_v2(5, 5), 30);
    EXPECT(r2d_particles_count(ps) == 30);
    r2d_particles_burst(ps, &d, r2d_v2(0, 0), 500);          /* clipped to the capacity */
    EXPECT(r2d_particles_count(ps) == 100);
    r2d_particles_clear(ps);
    EXPECT(r2d_particles_count(ps) == 0);

    /* straight flight with no spread, no gravity: x advances by speed * t */
    r2d_particles_burst(ps, &d, r2d_v2(5, 5), 1);
    r2d_particles_update(ps, 0.5f);
    size_t i = 0;
    r2d_particle_view v;
    EXPECT(r2d_particles_next(ps, &i, &v));
    EXPECT(near_f(v.position.x, 10) && near_f(v.position.y, 5) && near_f(v.age, 0.5f));
    EXPECT(!r2d_particles_next(ps, &i, &v));
    r2d_particles_update(ps, 0.6f);                            /* past its 1 s lifetime */
    EXPECT(r2d_particles_count(ps) == 0);

    /* gravity and drag */
    d.speed = r2d_range_const(0);
    d.gravity = r2d_v2(0, 100);
    r2d_particles_burst(ps, &d, r2d_v2(0, 0), 1);
    r2d_particles_update(ps, 0.1f);
    i = 0;
    r2d_particles_next(ps, &i, &v);
    EXPECT(near_f(v.velocity.y, 10));
    r2d_particles_clear(ps);
    d.gravity = r2d_v2(0, 0);
    d.speed = r2d_range_const(100);
    d.drag = 5.0f;
    r2d_particles_burst(ps, &d, r2d_v2(0, 0), 1);
    r2d_particles_update(ps, 0.1f);
    i = 0;
    r2d_particles_next(ps, &i, &v);
    EXPECT(near_f(v.velocity.x, 50));                          /* lost 5 * 0.1 of its speed */

    /* size and color interpolate over life, alpha fades in */
    r2d_particles_clear(ps);
    d.drag = 0;
    d.size = r2d_range_const(10);
    d.size_end_scale = 0.5f;
    d.color_start = r2d_rgba(200, 0, 0, 255);
    d.color_end = r2d_rgba(0, 0, 200, 0);
    d.fade_in = 0.5f;
    r2d_particles_burst(ps, &d, r2d_v2(0, 0), 1);
    r2d_particles_update(ps, 0.25f);
    i = 0;
    r2d_particles_next(ps, &i, &v);
    EXPECT(near_f(v.size, 8.75f));
    EXPECT(v.color.r == 150 && v.color.b == 50 && v.color.a == 95);   /* 191 * 0.5 fade-in */

    /* determinism: same seed, same particles */
    r2d_particles *a = r2d_particles_create(64, 2, 77), *b = r2d_particles_create(64, 2, 77);
    r2d_particle_desc wide = r2d_particle_desc_default();
    r2d_particles_burst(a, &wide, r2d_v2(1, 2), 20);
    r2d_particles_burst(b, &wide, r2d_v2(1, 2), 20);
    r2d_particles_update(a, 0.1f);
    r2d_particles_update(b, 0.1f);
    size_t ia = 0, ib = 0;
    r2d_particle_view va, vb;
    bool same = true;
    while (r2d_particles_next(a, &ia, &va) && r2d_particles_next(b, &ib, &vb))
        same = same && memcmp(&va, &vb, sizeof(va)) == 0;
    EXPECT(same && ia == 20);
    r2d_particles_destroy(a);
    r2d_particles_destroy(b);
    r2d_particles_destroy(ps);
}
static void test_emitters(void) {
    r2d_particles *ps = r2d_particles_create(1000, 2, 3);
    r2d_particle_desc d = r2d_particle_desc_default();
    d.lifetime = r2d_range_const(10.0f);
    d.rate = 100.0f;
    d.speed = r2d_range_const(0);
    d.inherit_velocity = 1.0f;
    r2d_emitter e = r2d_emitter_start(ps, &d, r2d_v2(0, 0), 0.5f);
    EXPECT(r2d_emitter_alive(ps, e));
    EXPECT(r2d_particles_emitter_count(ps) == 1);
    r2d_emitter e2 = r2d_emitter_start(ps, &d, r2d_v2(0, 0), 0);
    r2d_emitter e3 = r2d_emitter_start(ps, &d, r2d_v2(0, 0), 0);
    EXPECT(r2d_emitter_alive(ps, e2) && !r2d_emitter_alive(ps, e3));   /* table of 2 */
    r2d_emitter_stop(ps, e2);
    EXPECT(!r2d_emitter_alive(ps, e2) && r2d_particles_emitter_count(ps) == 1);
    r2d_emitter_stop(ps, e2);                                          /* stale handle: ignored */
    r2d_emitter_move(ps, e2, r2d_v2(9, 9));

    /* 100/s for 0.2 s -> 20 particles, then the emitter ends at 0.5 s */
    r2d_particles_update(ps, 0.2f);
    EXPECT(r2d_particles_count(ps) == 20);
    r2d_particles_update(ps, 0.2f);
    r2d_particles_update(ps, 0.2f);                                    /* only 0.1 s of emission left */
    EXPECT(r2d_particles_count(ps) == 50);
    EXPECT(!r2d_emitter_alive(ps, e) && r2d_particles_emitter_count(ps) == 0);
    r2d_particles_update(ps, 1.0f);
    EXPECT(r2d_particles_count(ps) == 50);

    /* a moved emitter passes its velocity on */
    r2d_particles_clear(ps);
    r2d_emitter m = r2d_emitter_start(ps, &d, r2d_v2(0, 0), 0);
    r2d_emitter_move(ps, m, r2d_v2(10, 0));
    r2d_emitter_set_rate(ps, m, 10.0f);
    r2d_particles_update(ps, 0.1f);                                    /* 1 particle, emitter moved 100 u/s */
    EXPECT(r2d_particles_count(ps) == 1);
    size_t i = 0;
    r2d_particle_view v;
    r2d_particles_next(ps, &i, &v);
    EXPECT(near_f(v.velocity.x, 100));
    r2d_particles_update(ps, 0.1f);                                    /* not moved: velocity 0 */
    i = 1;
    r2d_particles_next(ps, &i, &v);
    EXPECT(near_f(v.velocity.x, 0) && r2d_particles_count(ps) == 2);
    r2d_emitter_stop(ps, m);
    r2d_emitter h2 = r2d_emitter_start(ps, &d, r2d_v2(0, 0), 0);
    EXPECT(h2.id != m.id && !r2d_emitter_alive(ps, m));                /* slot reuse bumps the generation */
    r2d_particles_destroy(ps);
}
static void test_particle_draw(void) {
    r2d_particles *ps = r2d_particles_create(64, 1, 5);
    r2d_particle_desc d = r2d_particle_desc_default();
    d.lifetime = r2d_range_const(1);
    d.size = r2d_range_const(3);
    reset_stub();
    d.shape = R2D_PARTICLE_CIRCLE; r2d_particles_burst(ps, &d, r2d_v2(0, 0), 2);
    d.shape = R2D_PARTICLE_SQUARE; r2d_particles_burst(ps, &d, r2d_v2(0, 0), 3);
    d.shape = R2D_PARTICLE_STREAK; r2d_particles_burst(ps, &d, r2d_v2(0, 0), 4);
    d.shape = R2D_PARTICLE_RING; r2d_particles_burst(ps, &d, r2d_v2(0, 0), 5);
    r2d_particles_draw(ps);
    EXPECT(stub.circles == 2 && stub.orects == 3 && stub.lines == 4 && stub.rings == 5);
    r2d_particles_destroy(ps);
}

/* --- effects --- */
static void test_easing(void) {
    r2d_ease eases[] = {R2D_EASE_LINEAR, R2D_EASE_IN, R2D_EASE_OUT, R2D_EASE_IN_OUT, R2D_EASE_OUT_CUBIC,
                        R2D_EASE_OUT_BACK, R2D_EASE_OUT_ELASTIC, R2D_EASE_OUT_BOUNCE};
    for (size_t i = 0; i < sizeof(eases) / sizeof(eases[0]); ++i) {
        EXPECT(near_f(r2d_ease_apply(eases[i], 0), 0));
        EXPECT(near_f(r2d_ease_apply(eases[i], 1), 1));
        EXPECT(near_f(r2d_ease_apply(eases[i], -3), 0) && near_f(r2d_ease_apply(eases[i], 7), 1));
    }
    EXPECT(near_f(r2d_ease_apply(R2D_EASE_IN, 0.5f), 0.25f));
    EXPECT(near_f(r2d_ease_apply(R2D_EASE_OUT, 0.5f), 0.75f));
    EXPECT(near_f(r2d_ease_apply(R2D_EASE_IN_OUT, 0.5f), 0.5f));
    EXPECT(r2d_ease_apply(R2D_EASE_OUT_BACK, 0.6f) > 1.0f);           /* overshoots */
    EXPECT(near_f(r2d_ease_apply(R2D_EASE_PULSE, 0.5f), 1) && near_f(r2d_ease_apply(R2D_EASE_PULSE, 1), 0));
}
static int custom_calls;
static float custom_last_t;
static void custom_draw(r2d_effect h, float t, float age, r2d_vec2 p, r2d_color c, void *user) {
    (void)h; (void)age; (void)p; (void)c;
    ++custom_calls;
    custom_last_t = t;
    EXPECT(user == &custom_calls);
}
static void test_effects_lifecycle(void) {
    r2d_effects *fx = r2d_effects_create(4);
    EXPECT(r2d_effects_capacity(fx) == 4 && r2d_effects_count(fx) == 0);
    r2d_effect t = r2d_effects_text(fx, r2d_v2(0, 0), "hit -12", r2d_rgb(255, 255, 255), 1.0f, r2d_v2(0, -20));
    r2d_effect r = r2d_effects_ring(fx, r2d_v2(0, 0), 1, 50, 2, r2d_rgb(255, 0, 0), 0.5f, R2D_EASE_OUT);
    r2d_effect loop = r2d_effects_disc(fx, r2d_v2(0, 0), 5, r2d_rgb(0, 255, 0), 0);   /* loops forever */
    r2d_effect flash = r2d_effects_flash(fx, r2d_rgba(255, 255, 255, 128), 0.25f);
    r2d_effect overflow = r2d_effects_flash(fx, r2d_rgba(255, 255, 255, 128), 0.25f);
    EXPECT(r2d_effects_count(fx) == 4 && !r2d_effects_alive(fx, overflow) && overflow.id == 0);
    EXPECT(r2d_effects_alive(fx, t) && r2d_effects_alive(fx, r) && r2d_effects_alive(fx, loop) && r2d_effects_alive(fx, flash));
    EXPECT(near_f(r2d_effects_progress(fx, r), 0));
    r2d_effects_update(fx, 0.25f);
    EXPECT(near_f(r2d_effects_progress(fx, r), 0.75f));               /* eased out: 1 - (1 - 0.5)^2 */
    EXPECT(!r2d_effects_alive(fx, flash));                            /* ended exactly at its duration */
    r2d_effects_update(fx, 0.3f);
    EXPECT(!r2d_effects_alive(fx, r) && r2d_effects_alive(fx, t) && r2d_effects_alive(fx, loop));
    r2d_effects_update(fx, 5.0f);
    EXPECT(!r2d_effects_alive(fx, t) && r2d_effects_alive(fx, loop));  /* loops survive */
    float p = r2d_effects_progress(fx, loop);
    EXPECT(p >= 0 && p <= 1);
    r2d_effects_cancel(fx, loop);
    EXPECT(!r2d_effects_alive(fx, loop) && r2d_effects_count(fx) == 0);
    r2d_effects_cancel(fx, loop);                                     /* stale: ignored */
    r2d_effect again = r2d_effects_disc(fx, r2d_v2(0, 0), 5, r2d_rgb(0, 255, 0), 1);
    EXPECT(again.id != loop.id && !r2d_effects_alive(fx, loop) && r2d_effects_alive(fx, again));
    EXPECT(near_f(r2d_effects_progress(fx, loop), 0));
    r2d_effects_clear(fx);
    EXPECT(r2d_effects_count(fx) == 0);
    r2d_effects_destroy(fx);
}
static void test_effects_draw_and_shake(void) {
    r2d_effects *fx = r2d_effects_create(16);
    reset_stub();
    r2d_effects_text(fx, r2d_v2(1, 1), "text", r2d_rgb(255, 255, 255), 1, r2d_v2(0, 0));
    r2d_effects_ring(fx, r2d_v2(0, 0), 1, 2, 1, r2d_rgb(255, 0, 0), 1, R2D_EASE_LINEAR);
    r2d_effects_disc(fx, r2d_v2(0, 0), 1, r2d_rgb(255, 0, 0), 1);
    r2d_effect beam = r2d_effects_beam(fx, r2d_v2(0, 0), r2d_v2(1, 1), 1, r2d_rgb(255, 0, 0), 1);
    r2d_effects_arc(fx, r2d_v2(0, 0), 10, 2, 0, 1.5f, r2d_rgb(255, 0, 0), 1);
    r2d_effects_flash(fx, r2d_rgba(255, 255, 255, 100), 1);
    r2d_effects_shake(fx, 10, 30, 1);
    r2d_effects_custom(fx, R2D_SPACE_SCREEN, r2d_v2(0, 0), r2d_rgb(1, 2, 3), 1, R2D_EASE_LINEAR, custom_draw, &custom_calls);
    r2d_effects_update(fx, 0.5f);
    stub.space = R2D_SPACE_WORLD;
    r2d_effects_draw(fx, R2D_SPACE_WORLD);
    EXPECT(stub.texts == 1 && stub.rings == 1 && stub.circles == 1 && stub.lines == 1 && stub.arcs == 1);
    EXPECT(stub.rects == 0 && custom_calls == 0);                     /* screen effects not drawn yet */
    EXPECT(strcmp(stub.last_text, "text") == 0);
    r2d_effects_draw(fx, R2D_SPACE_SCREEN);
    EXPECT(stub.rects == 1 && near_f(stub.last_rect.w, 800) && near_f(stub.last_rect.h, 600));
    EXPECT(custom_calls == 1 && near_f(custom_last_t, 0.5f));
    EXPECT(stub.space == R2D_SPACE_WORLD);                           /* restored */
    r2d_effects_move(fx, beam, r2d_v2(5, 5));
    r2d_effects_move_end(fx, beam, r2d_v2(6, 6));
    r2d_effects_recolor(fx, beam, r2d_rgb(0, 0, 255));
    EXPECT(r2d_effects_alive(fx, beam));

    /* shake: non-zero while running, decays to zero, camera push/pop restores */
    r2d_vec2 off = r2d_effects_shake_offset(fx);
    EXPECT(r2d_length(off) > 0 && r2d_length(off) <= 10.0f + 1e-3f);
    stub_camera = r2d_camera_make(r2d_v2(100, 100), 2.0f, false);
    r2d_effects_camera_push(fx);
    EXPECT(!near_f(stub_camera.center.x, 100) || !near_f(stub_camera.center.y, 100));
    r2d_effects_camera_pop(fx);
    EXPECT(near_f(stub_camera.center.x, 100) && near_f(stub_camera.center.y, 100));
    r2d_effects_camera_pop(fx);                                        /* unbalanced pop: no-op */
    EXPECT(near_f(stub_camera.center.x, 100));
    r2d_effects_update(fx, 0.49f);
    EXPECT(r2d_length(r2d_effects_shake_offset(fx)) < 0.5f);          /* nearly decayed */
    r2d_effects_update(fx, 1.0f);
    EXPECT(r2d_effects_count(fx) == 0 && r2d_length(r2d_effects_shake_offset(fx)) == 0);
    r2d_effects_destroy(fx);
}

int main(void) {
    test_particle_bursts();
    test_emitters();
    test_particle_draw();
    test_easing();
    test_effects_lifecycle();
    test_effects_draw_and_shake();
    if (failures) { printf("%d of %d particle/effect checks failed\n", failures, checks); return 1; }
    printf("OK: %d particle/effect checks passed\n", checks);
    return 0;
}
