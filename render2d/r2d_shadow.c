/* Pure part of the lighting system: polar shadow maps. */
#include "r2d_lighting.h"
#include <string.h>

#define R2D_S_PI 3.14159265358979f
#define R2D_S_TAU 6.28318530717959f

float r2d_ray_circle(r2d_vec2 o, r2d_vec2 d, r2d_circle c) {
    r2d_vec2 oc = r2d_sub(c.center, o);
    float proj = r2d_dot(oc, d);
    float perp2 = r2d_dot(oc, oc) - proj * proj;
    float r2 = c.radius * c.radius;
    if (perp2 > r2) return -1.0f;
    float half = sqrtf(r2 - perp2);
    float t0 = proj - half, t1 = proj + half;
    if (t1 < 0) return -1.0f;
    return t0 >= 0 ? t0 : 0.0f;   /* inside the circle: blocked immediately */
}
float r2d_ray_segment(r2d_vec2 o, r2d_vec2 d, r2d_segment s) {
    r2d_vec2 e = r2d_sub(s.b, s.a);
    float denom = d.x * e.y - d.y * e.x;
    if (fabsf(denom) < 1e-9f) return -1.0f;   /* parallel */
    r2d_vec2 ao = r2d_sub(s.a, o);
    float t = (ao.x * e.y - ao.y * e.x) / denom;
    float u = (ao.x * d.y - ao.y * d.x) / denom;
    if (t < 0 || u < 0 || u > 1) return -1.0f;
    return t;
}

static float wrap_angle(float a) {
    a = fmodf(a, R2D_S_TAU);
    return a < 0 ? a + R2D_S_TAU : a;
}
static int angle_index(float a, int angles) {
    int i = (int)floorf(wrap_angle(a) / R2D_S_TAU * (float)angles);
    return i < 0 ? 0 : (i >= angles ? angles - 1 : i);
}
/* Test the occluder against every angle of the arc [start, start + span]. */
static size_t test_arc(r2d_vec2 light, float start, float span, const r2d_circle *circle, const r2d_segment *segment,
                       float *out, int angles) {
    int first = angle_index(start, angles) - 1;
    int count = (int)ceilf(span / R2D_S_TAU * (float)angles) + 3;
    if (count > angles) count = angles;
    size_t tests = 0;
    for (int k = 0; k < count; ++k) {
        int i = ((first + k) % angles + angles) % angles;
        float a = ((float)i + 0.5f) * R2D_S_TAU / (float)angles;
        r2d_vec2 d = r2d_v2(cosf(a), sinf(a));
        float t = circle ? r2d_ray_circle(light, d, *circle) : r2d_ray_segment(light, d, *segment);
        ++tests;
        if (t >= 0 && t < out[i]) out[i] = t;
    }
    return tests;
}
size_t r2d_shadow_map_build(r2d_vec2 light, float radius, const r2d_circle *circles, size_t circle_count,
                            const r2d_segment *segments, size_t segment_count, float *out, int angles) {
    if (!out || angles <= 0) return 0;
    for (int i = 0; i < angles; ++i) out[i] = radius;
    if (radius <= 0) return 0;
    size_t tests = 0;
    for (size_t c = 0; c < circle_count; ++c) {
        const r2d_circle *circle = &circles[c];
        if (circle->radius <= 0) continue;
        r2d_vec2 to = r2d_sub(circle->center, light);
        float dist = r2d_length(to);
        if (dist - circle->radius > radius) continue;          /* entirely out of reach */
        if (dist <= circle->radius) {                          /* light inside: everything blocked */
            for (int i = 0; i < angles; ++i) out[i] = 0;
            continue;
        }
        float center_angle = atan2f(to.y, to.x), half = asinf(circle->radius / dist);
        tests += test_arc(light, center_angle - half, 2 * half, circle, NULL, out, angles);
    }
    for (size_t s = 0; s < segment_count; ++s) {
        const r2d_segment *seg = &segments[s];
        r2d_vec2 pa = r2d_sub(seg->a, light), pb = r2d_sub(seg->b, light);
        float da = r2d_length(pa), db = r2d_length(pb);
        /* quick reject: both endpoints beyond reach and the segment's closest point too */
        if (da > radius && db > radius) {
            r2d_vec2 e = r2d_sub(seg->b, seg->a);
            float len2 = r2d_dot(e, e);
            float t = len2 > 0 ? r2d_clampf(-r2d_dot(pa, e) / len2, 0, 1) : 0;
            r2d_vec2 closest = r2d_add(pa, r2d_scale(e, t));
            if (r2d_length(closest) > radius) continue;
        }
        float aa = atan2f(pa.y, pa.x), ab = atan2f(pb.y, pb.x);
        float span = wrap_angle(ab - aa);
        float start = aa;
        if (span > R2D_S_PI) { span = R2D_S_TAU - span; start = ab; }   /* the shorter arc */
        tests += test_arc(light, start, span, NULL, seg, out, angles);
    }
    return tests;
}
float r2d_shadow_map_sample(const float *map, int angles, float angle) {
    if (!map || angles <= 0) return 0;
    return map[angle_index(angle, angles)];
}
