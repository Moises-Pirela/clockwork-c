/* render3d: a small 3D renderer on top of Sokol, built to coexist with
 * render2d.
 *
 * What it gives an application:
 *   - 3D math: vectors, column-major 4x4 matrices, rays
 *   - a perspective (or orthographic) camera as a plain value, with pure
 *     orbit / dolly / pan / fit operations and screen <-> world transforms
 *   - the primitives the engine knows: sphere, capsule, cube and plane,
 *     and arbitrary triangle meshes submitted per frame (level brushes),
 *     with per-vertex colours and a material: a texture, a tint, tiling, a
 *     Phong surface (specular colour and shininess, emissive) or unlit
 *   - lights: ambient, directional, point and spot, up to R3D_MAX_LIGHTS a
 *     frame, lit per pixel (Blinn would be cheaper; Phong is what was asked)
 *     with a default key light when a frame adds none
 *   - alpha blending for translucent shapes, wireframe edges, an optional
 *     world-space checker, depth-tested and overlay lines, a ground grid and
 *     axes for editors
 *   - picking: primitives and boxes submitted with an id can be hit-tested
 *     by a window-space point through exact ray casts (no readback)
 *   - an offscreen target with its own depth buffer, composited into
 *     whatever pass the host has open (render2d's scene layer, through its
 *     underlay hook) or presented on its own
 *
 * It owns nothing of the application's data and knows nothing about the
 * ECS or render2d. It depends on sokol_gfx (and sokol_app/sokol_glue for
 * the standalone present), whose implementations live in one translation
 * unit (examples/sokol_impl.c), shared with render2d.
 *
 * Frame protocol:
 *   init:    r3d_setup(&desc)            (skip_gfx_setup when render2d set sokol_gfx up)
 *   frame:   r3d_begin_frame(rect, dpi)  the viewport rectangle in logical window pixels
 *            ... r3d_draw_* ...
 *            r3d_render();               the offscreen pass (outside any other pass)
 *            r3d_composite(...);         inside the host's pass, at the rectangle
 *            or r3d_present() when render3d owns the window
 *   cleanup: r3d_shutdown()
 *
 * Conventions: right-handed, y up, the camera looks down its -z axis.
 * Rotations are Euler angles in radians applied roll (z), then pitch (x),
 * then yaw (y): M = Ry * Rx * Rz. Window space has its origin at the top
 * left with y growing downwards, like render2d's screen space. The pure
 * parts (math, camera, ray casts) are inline here and covered by
 * tests/test_render3d.c without a window.
 */
#ifndef RENDER3D_H
#define RENDER3D_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sokol_gfx.h"
#include "r3d_mesh.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Vectors, colours, matrices, rays
 * ------------------------------------------------------------------------- */
typedef struct { float x, y, z; } r3d_vec3;
typedef struct { float x, y, z, w; } r3d_vec4;
typedef struct { uint8_t r, g, b, a; } r3d_color;
typedef struct { float m[16]; } r3d_mat4;     /* column-major: element (row, col) is m[col * 4 + row] */
typedef struct { r3d_vec3 origin, direction; } r3d_ray;   /* direction need not be normalized */

#define R3D_PI 3.14159265358979f
#define R3D_ID_NONE UINT64_MAX

static inline r3d_vec3 r3d_v3(float x, float y, float z) { r3d_vec3 v = {x, y, z}; return v; }
static inline r3d_vec3 r3d_add(r3d_vec3 a, r3d_vec3 b) { return r3d_v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline r3d_vec3 r3d_sub(r3d_vec3 a, r3d_vec3 b) { return r3d_v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline r3d_vec3 r3d_scale(r3d_vec3 a, float s) { return r3d_v3(a.x * s, a.y * s, a.z * s); }
static inline r3d_vec3 r3d_mul(r3d_vec3 a, r3d_vec3 b) { return r3d_v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline r3d_vec3 r3d_neg(r3d_vec3 a) { return r3d_v3(-a.x, -a.y, -a.z); }
static inline float r3d_dot(r3d_vec3 a, r3d_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline r3d_vec3 r3d_cross(r3d_vec3 a, r3d_vec3 b) {
    return r3d_v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float r3d_length(r3d_vec3 a) { return sqrtf(r3d_dot(a, a)); }
static inline float r3d_distance(r3d_vec3 a, r3d_vec3 b) { return r3d_length(r3d_sub(a, b)); }
static inline r3d_vec3 r3d_normalize(r3d_vec3 a) {
    float len = r3d_length(a);
    return len > 0 ? r3d_scale(a, 1.0f / len) : r3d_v3(0, 0, 0);
}
static inline r3d_vec3 r3d_lerp(r3d_vec3 a, r3d_vec3 b, float t) {
    return r3d_v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}
static inline float r3d_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float r3d_maxf(float a, float b) { return a > b ? a : b; }
static inline float r3d_minf(float a, float b) { return a < b ? a : b; }
static inline r3d_vec3 r3d_ray_point(r3d_ray r, float t) { return r3d_add(r.origin, r3d_scale(r.direction, t)); }

static inline r3d_color r3d_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { r3d_color c = {r, g, b, a}; return c; }
static inline r3d_color r3d_rgb(uint8_t r, uint8_t g, uint8_t b) { return r3d_rgba(r, g, b, 255); }
static inline r3d_color r3d_with_alpha(r3d_color c, uint8_t a) { c.a = a; return c; }

static inline r3d_mat4 r3d_mat4_identity(void) {
    r3d_mat4 r = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    return r;
}
static inline float r3d_mat4_at(const r3d_mat4 *m, int row, int col) { return m->m[col * 4 + row]; }
static inline r3d_mat4 r3d_mat4_multiply(r3d_mat4 a, r3d_mat4 b) {   /* a * b: b is applied first */
    r3d_mat4 r;
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[col * 4 + k];
            r.m[col * 4 + row] = s;
        }
    return r;
}
static inline r3d_mat4 r3d_mat4_transpose(r3d_mat4 a) {
    r3d_mat4 r;
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) r.m[col * 4 + row] = a.m[row * 4 + col];
    return r;
}
static inline r3d_vec4 r3d_mat4_transform(const r3d_mat4 *m, r3d_vec4 v) {
    r3d_vec4 r;
    r.x = m->m[0] * v.x + m->m[4] * v.y + m->m[8] * v.z + m->m[12] * v.w;
    r.y = m->m[1] * v.x + m->m[5] * v.y + m->m[9] * v.z + m->m[13] * v.w;
    r.z = m->m[2] * v.x + m->m[6] * v.y + m->m[10] * v.z + m->m[14] * v.w;
    r.w = m->m[3] * v.x + m->m[7] * v.y + m->m[11] * v.z + m->m[15] * v.w;
    return r;
}
static inline r3d_vec3 r3d_mat4_transform_point(const r3d_mat4 *m, r3d_vec3 p) {
    r3d_vec4 v = {p.x, p.y, p.z, 1};
    r3d_vec4 r = r3d_mat4_transform(m, v);
    return r3d_v3(r.x, r.y, r.z);
}
static inline r3d_vec3 r3d_mat4_transform_vector(const r3d_mat4 *m, r3d_vec3 d) {
    r3d_vec4 v = {d.x, d.y, d.z, 0};
    r3d_vec4 r = r3d_mat4_transform(m, v);
    return r3d_v3(r.x, r.y, r.z);
}
static inline r3d_mat4 r3d_mat4_translation(r3d_vec3 t) {
    r3d_mat4 r = r3d_mat4_identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}
static inline r3d_mat4 r3d_mat4_scaling(r3d_vec3 s) {
    r3d_mat4 r = r3d_mat4_identity();
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}
static inline r3d_mat4 r3d_mat4_rotation_x(float a) {
    r3d_mat4 r = r3d_mat4_identity();
    float c = cosf(a), s = sinf(a);
    r.m[5] = c; r.m[9] = -s; r.m[6] = s; r.m[10] = c;
    return r;
}
static inline r3d_mat4 r3d_mat4_rotation_y(float a) {
    r3d_mat4 r = r3d_mat4_identity();
    float c = cosf(a), s = sinf(a);
    r.m[0] = c; r.m[8] = s; r.m[2] = -s; r.m[10] = c;
    return r;
}
static inline r3d_mat4 r3d_mat4_rotation_z(float a) {
    r3d_mat4 r = r3d_mat4_identity();
    float c = cosf(a), s = sinf(a);
    r.m[0] = c; r.m[4] = -s; r.m[1] = s; r.m[5] = c;
    return r;
}
/* Euler angles (radians): roll about z, then pitch about x, then yaw about y. */
static inline r3d_mat4 r3d_mat4_rotation(r3d_vec3 euler) {
    return r3d_mat4_multiply(r3d_mat4_rotation_y(euler.y), r3d_mat4_multiply(r3d_mat4_rotation_x(euler.x), r3d_mat4_rotation_z(euler.z)));
}
/* translation * rotation * scale: the usual model matrix. */
static inline r3d_mat4 r3d_mat4_trs(r3d_vec3 t, r3d_vec3 euler, r3d_vec3 s) {
    return r3d_mat4_multiply(r3d_mat4_translation(t), r3d_mat4_multiply(r3d_mat4_rotation(euler), r3d_mat4_scaling(s)));
}
/* A view matrix looking from `eye` at `target`. */
static inline r3d_mat4 r3d_mat4_look_at(r3d_vec3 eye, r3d_vec3 target, r3d_vec3 up) {
    r3d_vec3 f = r3d_normalize(r3d_sub(target, eye));
    if (r3d_length(f) == 0) f = r3d_v3(0, 0, -1);
    r3d_vec3 s = r3d_cross(f, up);
    if (r3d_length(s) < 1e-6f) s = r3d_cross(f, fabsf(f.y) < 0.9f ? r3d_v3(0, 1, 0) : r3d_v3(1, 0, 0));
    s = r3d_normalize(s);
    r3d_vec3 u = r3d_cross(s, f);
    r3d_mat4 r = r3d_mat4_identity();
    r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -r3d_dot(s, eye);
    r.m[13] = -r3d_dot(u, eye);
    r.m[14] = r3d_dot(f, eye);
    return r;
}
/* Perspective projection. Clip-space depth spans -1..1 (OpenGL) or 0..1
 * (Direct3D, Metal) as `zero_to_one` says; the renderer picks by backend. */
static inline r3d_mat4 r3d_mat4_perspective(float fov_y, float aspect, float near_plane, float far_plane, bool zero_to_one) {
    r3d_mat4 r;
    for (int i = 0; i < 16; ++i) r.m[i] = 0;
    float f = 1.0f / tanf(fov_y * 0.5f);
    r.m[0] = f / (aspect > 0 ? aspect : 1);
    r.m[5] = f;
    r.m[11] = -1;
    if (zero_to_one) {
        r.m[10] = far_plane / (near_plane - far_plane);
        r.m[14] = near_plane * far_plane / (near_plane - far_plane);
    } else {
        r.m[10] = (far_plane + near_plane) / (near_plane - far_plane);
        r.m[14] = 2 * far_plane * near_plane / (near_plane - far_plane);
    }
    return r;
}
static inline r3d_mat4 r3d_mat4_orthographic(float left, float right, float bottom, float top, float near_plane, float far_plane, bool zero_to_one) {
    r3d_mat4 r = r3d_mat4_identity();
    r.m[0] = 2 / (right - left);
    r.m[5] = 2 / (top - bottom);
    r.m[12] = -(right + left) / (right - left);
    r.m[13] = -(top + bottom) / (top - bottom);
    if (zero_to_one) {
        r.m[10] = 1 / (near_plane - far_plane);
        r.m[14] = near_plane / (near_plane - far_plane);
    } else {
        r.m[10] = 2 / (near_plane - far_plane);
        r.m[14] = (far_plane + near_plane) / (near_plane - far_plane);
    }
    return r;
}
/* General inverse (cofactor expansion); returns false for a singular matrix. */
static inline bool r3d_mat4_inverse(const r3d_mat4 *a, r3d_mat4 *out) {
    const float *m = a->m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-20f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out->m[i] = inv[i] * det;
    return true;
}

/* ---------------------------------------------------------------------------
 * Camera
 *
 * A camera is a plain value: where it is, what it looks at, its up hint,
 * its vertical field of view and clip planes. fov_y <= 0 selects an
 * orthographic projection showing `ortho_height` world units vertically.
 * The orbit operations move the position around the target; the renderer
 * keeps one current camera (r3d_camera_ptr / r3d_set_camera).
 * ------------------------------------------------------------------------- */
typedef struct {
    r3d_vec3 position, target, up;
    float fov_y;                 /* radians; zero or less -> orthographic */
    float near_plane, far_plane;
    float ortho_height;
    float min_distance;          /* the dolly never gets closer to the target than this */
} r3d_camera;

static inline r3d_camera r3d_camera_make(r3d_vec3 position, r3d_vec3 target) {
    r3d_camera c;
    c.position = position;
    c.target = target;
    c.up = r3d_v3(0, 1, 0);
    c.fov_y = 50.0f * R3D_PI / 180.0f;
    c.near_plane = 0.05f;
    c.far_plane = 2000.0f;
    c.ortho_height = 20.0f;
    c.min_distance = 0.2f;
    return c;
}
static inline r3d_vec3 r3d_camera_forward(const r3d_camera *c) { return r3d_normalize(r3d_sub(c->target, c->position)); }
static inline r3d_vec3 r3d_camera_right(const r3d_camera *c) {
    r3d_vec3 f = r3d_camera_forward(c), r = r3d_cross(f, c->up);
    if (r3d_length(r) < 1e-6f) r = r3d_cross(f, fabsf(f.y) < 0.9f ? r3d_v3(0, 1, 0) : r3d_v3(1, 0, 0));
    return r3d_normalize(r);
}
static inline r3d_vec3 r3d_camera_up(const r3d_camera *c) { return r3d_cross(r3d_camera_right(c), r3d_camera_forward(c)); }
static inline float r3d_camera_distance(const r3d_camera *c) { return r3d_distance(c->position, c->target); }
static inline r3d_mat4 r3d_camera_view(const r3d_camera *c) { return r3d_mat4_look_at(c->position, c->target, c->up); }
static inline r3d_mat4 r3d_camera_projection(const r3d_camera *c, float aspect, bool zero_to_one) {
    if (c->fov_y > 0) return r3d_mat4_perspective(c->fov_y, aspect, c->near_plane, c->far_plane, zero_to_one);
    float h = c->ortho_height * 0.5f, w = h * (aspect > 0 ? aspect : 1);
    return r3d_mat4_orthographic(-w, w, -h, h, c->near_plane, c->far_plane, zero_to_one);
}
static inline r3d_mat4 r3d_camera_view_projection(const r3d_camera *c, float aspect, bool zero_to_one) {
    return r3d_mat4_multiply(r3d_camera_projection(c, aspect, zero_to_one), r3d_camera_view(c));
}
/* Yaw around +y and pitch above the horizon of the position seen from the target. */
static inline float r3d_camera_yaw(const r3d_camera *c) {
    r3d_vec3 d = r3d_sub(c->position, c->target);
    return atan2f(d.x, d.z);
}
static inline float r3d_camera_pitch(const r3d_camera *c) {
    r3d_vec3 d = r3d_sub(c->position, c->target);
    float h = sqrtf(d.x * d.x + d.z * d.z);
    return atan2f(d.y, h);
}
static inline void r3d_camera_set_orbit(r3d_camera *c, float yaw, float pitch, float distance) {
    const float limit = 89.0f * R3D_PI / 180.0f;
    pitch = r3d_clampf(pitch, -limit, limit);
    if (distance < c->min_distance) distance = c->min_distance;
    r3d_vec3 d = r3d_v3(cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw));
    c->position = r3d_add(c->target, r3d_scale(d, distance));
}
static inline void r3d_camera_orbit(r3d_camera *c, float yaw_delta, float pitch_delta) {
    r3d_camera_set_orbit(c, r3d_camera_yaw(c) + yaw_delta, r3d_camera_pitch(c) + pitch_delta, r3d_camera_distance(c));
}
/* Multiply the distance to the target (factor < 1 approaches). */
static inline void r3d_camera_dolly(r3d_camera *c, float factor) {
    r3d_camera_set_orbit(c, r3d_camera_yaw(c), r3d_camera_pitch(c), r3d_camera_distance(c) * (factor > 0 ? factor : 1));
    if (c->fov_y <= 0) c->ortho_height = r3d_maxf(c->ortho_height * (factor > 0 ? factor : 1), 0.01f);
}
/* Move the position and the target together along the camera's right and up axes (world units). */
static inline void r3d_camera_pan(r3d_camera *c, float right, float up) {
    r3d_vec3 d = r3d_add(r3d_scale(r3d_camera_right(c), right), r3d_scale(r3d_camera_up(c), up));
    c->position = r3d_add(c->position, d);
    c->target = r3d_add(c->target, d);
}
/* World units per pixel at the target's distance, for a viewport `height_px` tall. */
static inline float r3d_camera_units_per_pixel(const r3d_camera *c, float height_px) {
    if (height_px <= 0) return 0;
    float h = c->fov_y > 0 ? 2 * r3d_camera_distance(c) * tanf(c->fov_y * 0.5f) : c->ortho_height;
    return h / height_px;
}
/* A screen-space drag (pixels, y down) that keeps the point under the mouse at the target's depth. */
static inline void r3d_camera_pan_screen(r3d_camera *c, float dx_px, float dy_px, float height_px) {
    float k = r3d_camera_units_per_pixel(c, height_px);
    r3d_camera_pan(c, -dx_px * k, dy_px * k);
}
/* Place the camera, keeping its direction, so a bounding sphere fills the view. */
static inline void r3d_camera_fit(r3d_camera *c, r3d_vec3 center, float radius, float aspect) {
    if (radius <= 0) radius = 1;
    float yaw = r3d_camera_yaw(c), pitch = r3d_camera_pitch(c);
    c->target = center;
    if (c->fov_y > 0) {
        float fov = c->fov_y;
        if (aspect > 0 && aspect < 1) fov = 2 * atanf(tanf(c->fov_y * 0.5f) * aspect);   /* the narrow side limits */
        r3d_camera_set_orbit(c, yaw, pitch, radius / sinf(fov * 0.5f) * 1.05f);
    } else {
        c->ortho_height = radius * 2.2f / (aspect > 0 && aspect < 1 ? aspect : 1);
        r3d_camera_set_orbit(c, yaw, pitch, radius * 4 + c->near_plane);
    }
    if (c->far_plane < r3d_camera_distance(c) + radius * 4) c->far_plane = r3d_camera_distance(c) + radius * 4;
}
/* World -> window point (origin top left) with a viewport `vw` x `vh`; false behind the camera. */
static inline bool r3d_project(const r3d_mat4 *view_proj, float vw, float vh, r3d_vec3 world, float *sx, float *sy, float *depth) {
    r3d_vec4 p = {world.x, world.y, world.z, 1};
    r3d_vec4 c = r3d_mat4_transform(view_proj, p);
    if (c.w <= 1e-6f) return false;
    float nx = c.x / c.w, ny = c.y / c.w;
    if (sx) *sx = (nx * 0.5f + 0.5f) * vw;
    if (sy) *sy = (1 - (ny * 0.5f + 0.5f)) * vh;
    if (depth) *depth = c.z / c.w;
    return true;
}
/* The ray through a window point (origin top left) for a viewport `vw` x `vh`. */
static inline r3d_ray r3d_camera_ray(const r3d_camera *c, float vw, float vh, float sx, float sy) {
    float aspect = vh > 0 ? vw / vh : 1;
    r3d_mat4 vp = r3d_camera_view_projection(c, aspect, false), inv;
    r3d_ray ray;
    ray.origin = c->position;
    ray.direction = r3d_camera_forward(c);
    if (!r3d_mat4_inverse(&vp, &inv) || vw <= 0 || vh <= 0) return ray;
    float nx = sx / vw * 2 - 1, ny = 1 - sy / vh * 2;
    r3d_vec4 a = {nx, ny, -1, 1}, b = {nx, ny, 1, 1};
    r3d_vec4 wa = r3d_mat4_transform(&inv, a), wb = r3d_mat4_transform(&inv, b);
    if (fabsf(wa.w) < 1e-12f || fabsf(wb.w) < 1e-12f) return ray;
    r3d_vec3 pa = r3d_v3(wa.x / wa.w, wa.y / wa.w, wa.z / wa.w), pb = r3d_v3(wb.x / wb.w, wb.y / wb.w, wb.z / wb.w);
    ray.origin = pa;
    ray.direction = r3d_normalize(r3d_sub(pb, pa));
    return ray;
}

/* ---------------------------------------------------------------------------
 * Primitives and ray casts
 * ------------------------------------------------------------------------- */
typedef enum { R3D_SPHERE, R3D_CAPSULE, R3D_CUBE, R3D_PLANE, R3D_PRIMITIVE_COUNT } r3d_primitive_kind;
/* A texture: tightly packed RGBA8 pixels uploaded once. */
typedef struct { sg_image image; sg_view view; int width, height; } r3d_texture;
/* What a surface is drawn with. Plain data, copied when a shape is
 * submitted, so the caller's copy may change afterwards; the texture it
 * names must live until r3d_render. */
typedef struct {
    r3d_texture texture;        /* zero image id: white */
    bool nearest, repeat;       /* sampling */
    float tint[4];              /* multiplies the shape's colour */
    float tiling[2], offset[2]; /* uv transform */
    float specular[3];          /* Phong highlight colour */
    float shininess;            /* Phong exponent, >= 1 */
    float emissive[3];          /* light given off */
    bool unlit;                 /* the colour alone, no lighting */
} r3d_material;
typedef struct {
    r3d_primitive_kind kind;
    r3d_vec3 position;
    r3d_vec3 rotation;   /* Euler radians: roll (z), then pitch (x), then yaw (y) */
    r3d_vec3 size;       /* sphere: x = radius; capsule: x = radius, y = distance between the cap centres (local y axis);
                            cube: full extents; plane: x by z (local +y is its normal) */
    r3d_color color;     /* alpha < 255 blends, after the opaque shapes */
    float checker;       /* > 0: a world-space checker of that many squares per unit darkens every other cell */
    bool wireframe;      /* edges only */
    uint64_t id;         /* pick id, R3D_ID_NONE when not pickable */
    const r3d_material *material;   /* NULL: the default lit white surface */
} r3d_primitive;

static inline r3d_primitive r3d_primitive_make(r3d_primitive_kind kind, r3d_vec3 position, r3d_vec3 size, r3d_color color) {
    r3d_primitive p;
    p.kind = kind;
    p.position = position;
    p.rotation = r3d_v3(0, 0, 0);
    p.size = size;
    p.color = color;
    p.checker = 0;
    p.wireframe = false;
    p.id = R3D_ID_NONE;
    p.material = NULL;
    return p;
}
/* The scale the unit mesh takes (capsules: the radius, their height is a
 * shader offset) and the local -> world matrix of a primitive. */
static inline r3d_vec3 r3d_primitive_scale(const r3d_primitive *p) {
    switch (p->kind) {
        case R3D_SPHERE: return r3d_v3(p->size.x, p->size.x, p->size.x);
        case R3D_CAPSULE: return r3d_v3(p->size.x, p->size.x, p->size.x);
        case R3D_PLANE: return r3d_v3(p->size.x, 1, p->size.z);
        default: return p->size;
    }
}
static inline r3d_mat4 r3d_primitive_matrix(const r3d_primitive *p) {
    return r3d_mat4_trs(p->position, p->rotation, r3d_primitive_scale(p));
}
/* The centres of a capsule's caps. */
static inline void r3d_capsule_segment(const r3d_primitive *p, r3d_vec3 *a, r3d_vec3 *b) {
    r3d_mat4 rot = r3d_mat4_rotation(p->rotation);
    r3d_vec3 axis = r3d_mat4_transform_vector(&rot, r3d_v3(0, r3d_maxf(p->size.y, 0) * 0.5f, 0));
    if (a) *a = r3d_sub(p->position, axis);
    if (b) *b = r3d_add(p->position, axis);
}
/* A conservative bounding sphere radius. */
static inline float r3d_primitive_radius(const r3d_primitive *p) {
    switch (p->kind) {
        case R3D_SPHERE: return fabsf(p->size.x);
        case R3D_CAPSULE: return fabsf(p->size.x) + fabsf(p->size.y) * 0.5f;
        case R3D_PLANE: return 0.5f * sqrtf(p->size.x * p->size.x + p->size.z * p->size.z);
        default: return 0.5f * r3d_length(p->size);
    }
}

/* The world-space bounding box of a primitive. */
static inline void r3d_primitive_bounds(const r3d_primitive *p, r3d_vec3 *lo, r3d_vec3 *hi) {
    r3d_vec3 l = r3d_v3(INFINITY, INFINITY, INFINITY), h = r3d_v3(-INFINITY, -INFINITY, -INFINITY);
    if (p->kind == R3D_SPHERE || p->kind == R3D_CAPSULE) {
        r3d_vec3 a = p->position, b = p->position;
        if (p->kind == R3D_CAPSULE) r3d_capsule_segment(p, &a, &b);
        float r = fabsf(p->size.x);
        l = r3d_v3(r3d_minf(a.x, b.x) - r, r3d_minf(a.y, b.y) - r, r3d_minf(a.z, b.z) - r);
        h = r3d_v3(r3d_maxf(a.x, b.x) + r, r3d_maxf(a.y, b.y) + r, r3d_maxf(a.z, b.z) + r);
    } else {
        r3d_mat4 m = r3d_primitive_matrix(p);
        float hy = p->kind == R3D_PLANE ? 0 : 0.5f;
        for (int i = 0; i < 8; ++i) {
            r3d_vec3 c = r3d_mat4_transform_point(&m, r3d_v3((i & 1) ? 0.5f : -0.5f, (i & 2) ? hy : -hy, (i & 4) ? 0.5f : -0.5f));
            l = r3d_v3(r3d_minf(l.x, c.x), r3d_minf(l.y, c.y), r3d_minf(l.z, c.z));
            h = r3d_v3(r3d_maxf(h.x, c.x), r3d_maxf(h.y, c.y), r3d_maxf(h.z, c.z));
        }
    }
    if (lo) *lo = l;
    if (hi) *hi = h;
}

/* Ray casts return the nearest hit with t >= 0 along the ray (in units of
 * the direction's length). */
static inline bool r3d_ray_sphere(r3d_ray ray, r3d_vec3 center, float radius, float *t) {
    r3d_vec3 oc = r3d_sub(ray.origin, center);
    float a = r3d_dot(ray.direction, ray.direction);
    if (a <= 0) return false;
    float b = 2 * r3d_dot(oc, ray.direction), c = r3d_dot(oc, oc) - radius * radius;
    float disc = b * b - 4 * a * c;
    if (disc < 0) return false;
    float s = sqrtf(disc), t0 = (-b - s) / (2 * a), t1 = (-b + s) / (2 * a);
    float hit = t0 >= 0 ? t0 : t1;
    if (hit < 0) return false;
    if (t) *t = hit;
    return true;
}
/* Axis-aligned box in the ray's space. */
static inline bool r3d_ray_aabb(r3d_ray ray, r3d_vec3 lo, r3d_vec3 hi, float *t) {
    float tmin = -INFINITY, tmax = INFINITY;
    const float *o = &ray.origin.x, *d = &ray.direction.x, *l = &lo.x, *h = &hi.x;
    for (int i = 0; i < 3; ++i) {
        if (fabsf(d[i]) < 1e-12f) {
            if (o[i] < l[i] || o[i] > h[i]) return false;
        } else {
            float a = (l[i] - o[i]) / d[i], b = (h[i] - o[i]) / d[i];
            if (a > b) { float s = a; a = b; b = s; }
            if (a > tmin) tmin = a;
            if (b < tmax) tmax = b;
            if (tmin > tmax) return false;
        }
    }
    if (tmax < 0) return false;
    if (t) *t = tmin >= 0 ? tmin : tmax;
    return true;
}
/* A capsule between cap centres a and b. */
static inline bool r3d_ray_capsule(r3d_ray ray, r3d_vec3 a, r3d_vec3 b, float radius, float *t) {
    bool hit = false;
    float best = INFINITY, tc;
    if (r3d_ray_sphere(ray, a, radius, &tc) && tc < best) { best = tc; hit = true; }
    if (r3d_ray_sphere(ray, b, radius, &tc) && tc < best) { best = tc; hit = true; }
    r3d_vec3 axis = r3d_sub(b, a);
    float len = r3d_length(axis);
    if (len > 1e-6f) {
        r3d_vec3 n = r3d_scale(axis, 1.0f / len);
        /* the infinite cylinder: components perpendicular to the axis */
        r3d_vec3 oc = r3d_sub(ray.origin, a);
        r3d_vec3 dp = r3d_sub(ray.direction, r3d_scale(n, r3d_dot(ray.direction, n)));
        r3d_vec3 op = r3d_sub(oc, r3d_scale(n, r3d_dot(oc, n)));
        float qa = r3d_dot(dp, dp), qb = 2 * r3d_dot(dp, op), qc = r3d_dot(op, op) - radius * radius;
        if (qa > 1e-12f) {
            float disc = qb * qb - 4 * qa * qc;
            if (disc >= 0) {
                float s = sqrtf(disc);
                float roots[2] = {(-qb - s) / (2 * qa), (-qb + s) / (2 * qa)};
                for (int i = 0; i < 2; ++i) {
                    if (roots[i] < 0 || roots[i] >= best) continue;
                    float along = r3d_dot(r3d_sub(r3d_ray_point(ray, roots[i]), a), n);
                    if (along >= 0 && along <= len) { best = roots[i]; hit = true; }
                }
            }
        }
    }
    if (hit && t) *t = best;
    return hit;
}
/* A finite rectangle on the local y = 0 plane (x and z within +-0.5), given the world -> local matrix. */
static inline bool r3d_ray_unit_plane(r3d_ray ray, const r3d_mat4 *world_to_local, float *t) {
    r3d_vec3 o = r3d_mat4_transform_point(world_to_local, ray.origin), d = r3d_mat4_transform_vector(world_to_local, ray.direction);
    if (fabsf(d.y) < 1e-9f) return false;
    float hit = -o.y / d.y;
    if (hit < 0) return false;
    float x = o.x + d.x * hit, z = o.z + d.z * hit;
    if (x < -0.5f || x > 0.5f || z < -0.5f || z > 0.5f) return false;
    if (t) *t = hit;
    return true;
}
/* The nearest hit on a primitive, in units of the ray direction's length. */
static inline bool r3d_ray_primitive(r3d_ray ray, const r3d_primitive *p, float *t) {
    switch (p->kind) {
        case R3D_SPHERE: return r3d_ray_sphere(ray, p->position, fabsf(p->size.x), t);
        case R3D_CAPSULE: {
            r3d_vec3 a, b;
            r3d_capsule_segment(p, &a, &b);
            return r3d_ray_capsule(ray, a, b, fabsf(p->size.x), t);
        }
        case R3D_CUBE:
        case R3D_PLANE: {
            r3d_mat4 model = r3d_primitive_matrix(p), inv;
            if (!r3d_mat4_inverse(&model, &inv)) return false;
            if (p->kind == R3D_PLANE) return r3d_ray_unit_plane(ray, &inv, t);
            r3d_ray local;
            local.origin = r3d_mat4_transform_point(&inv, ray.origin);
            local.direction = r3d_mat4_transform_vector(&inv, ray.direction);
            return r3d_ray_aabb(local, r3d_v3(-0.5f, -0.5f, -0.5f), r3d_v3(0.5f, 0.5f, 0.5f), t);
        }
        default: return false;
    }
}
/* A convex solid as the intersection of half-spaces n . p <= d: the nearest
 * entry (or the exit when the origin is inside) with the plane hit. */
typedef struct { r3d_vec3 n; float d; } r3d_plane;
static inline bool r3d_ray_convex(r3d_ray ray, const r3d_plane *planes, int count, float *t, int *plane_index) {
    float t_enter = -INFINITY, t_exit = INFINITY;
    int enter = -1, leave = -1;
    for (int i = 0; i < count; ++i) {
        float denom = r3d_dot(planes[i].n, ray.direction), dist = planes[i].d - r3d_dot(planes[i].n, ray.origin);
        if (fabsf(denom) < 1e-12f) { if (dist < 0) return false; continue; }
        float ti = dist / denom;
        if (denom < 0) { if (ti > t_enter) { t_enter = ti; enter = i; } }
        else if (ti < t_exit) { t_exit = ti; leave = i; }
        if (t_enter > t_exit) return false;
    }
    if (t_exit < 0 || count == 0) return false;
    bool inside = t_enter < 0;
    if (t) *t = inside ? t_exit : t_enter;
    if (plane_index) *plane_index = inside ? leave : enter;
    return true;
}
/* Where a ray meets a plane through `point` with `normal`; false when parallel or behind the origin. */
static inline bool r3d_ray_plane(r3d_ray ray, r3d_vec3 point, r3d_vec3 normal, float *t) {
    float denom = r3d_dot(ray.direction, normal);
    if (fabsf(denom) < 1e-9f) return false;
    float hit = r3d_dot(r3d_sub(point, ray.origin), normal) / denom;
    if (hit < 0) return false;
    if (t) *t = hit;
    return true;
}
/* The parameter along the line `origin + dir * s` of the point closest to
 * the ray (what a drag along a gizmo axis reads); false when parallel. */
static inline bool r3d_ray_line_closest(r3d_ray ray, r3d_vec3 origin, r3d_vec3 dir, float *s_out) {
    r3d_vec3 w = r3d_sub(ray.origin, origin);
    float a = r3d_dot(ray.direction, ray.direction), b = r3d_dot(ray.direction, dir), c = r3d_dot(dir, dir);
    float d = r3d_dot(ray.direction, w), e = r3d_dot(dir, w);
    float denom = a * c - b * b;
    if (fabsf(denom) < 1e-9f * a * c || c <= 0) return false;
    if (s_out) *s_out = (a * e - b * d) / denom;
    return true;
}
/* Where a ray meets the horizontal plane y = height (for placing things on the ground). */
static inline bool r3d_ray_ground(r3d_ray ray, float height, r3d_vec3 *out) {
    if (fabsf(ray.direction.y) < 1e-9f) return false;
    float t = (height - ray.origin.y) / ray.direction.y;
    if (t < 0) return false;
    if (out) *out = r3d_ray_point(ray, t);
    return true;
}

/* ---------------------------------------------------------------------------
 * Setup and frames
 * ------------------------------------------------------------------------- */
typedef struct {
    r3d_color background;       /* zero -> an opaque dark gray; alpha < 255 lets the host's layer below show through */
    int sample_count;           /* MSAA samples of the 3D target; zero -> 4 (falls back to 1 when unsupported) */
    int max_primitives;         /* shapes and meshes per frame; zero -> 16384 */
    int max_lines;              /* per frame; zero -> 65536 */
    int max_mesh_vertices;      /* dynamic mesh vertices per frame; zero -> 262144 (indices: 3x) */
    int max_pick_boxes;         /* pick boxes and convex solids per frame; zero -> 16384 */
    int shadow_map_size;        /* the directional shadow map, square; zero -> 2048; negative: no shadows */
    int local_shadow_map_size;  /* the spot and point shadow maps (an array of R3D_SHADOW_LAYERS layers); zero -> 1024 */
    int sphere_rings, sphere_sectors;   /* mesh detail; zero -> 24 x 48 */
    bool skip_gfx_setup;        /* sokol_gfx is already set up (render2d did it) */
} r3d_desc;

bool r3d_setup(const r3d_desc *desc);
void r3d_shutdown(void);
bool r3d_valid(void);

/* The viewport: a rectangle in logical window pixels (origin top left) and
 * the DPI scale that turns it into framebuffer pixels. Everything submitted
 * until r3d_render is drawn into a target of that size. */
void r3d_begin_frame(float x, float y, float w, float h, float dpi_scale);
void r3d_set_background(r3d_color color);
/* What the primitives show: their colour (unlit), their normals or their
 * uvs (debug views for checking meshes and transforms). */
typedef enum { R3D_VIEW_COLOR, R3D_VIEW_NORMALS, R3D_VIEW_UV, R3D_VIEW_MODES } r3d_view_mode;
void r3d_set_view_mode(r3d_view_mode mode);
r3d_view_mode r3d_view_mode_get(void);
r3d_camera *r3d_camera_ptr(void);
void r3d_set_camera(const r3d_camera *camera);
float r3d_aspect(void);
/* Window point <-> world, with the current camera and viewport. */
r3d_ray r3d_screen_ray(float sx, float sy);
bool r3d_world_to_screen(r3d_vec3 world, float *sx, float *sy);
/* Gizmo sizing: world units per pixel at the target distance or at a point. */
float r3d_px(float pixels);
float r3d_px_at(r3d_vec3 world, float pixels);

/* Textures and materials. */
bool r3d_texture_create(r3d_texture *texture, int width, int height, const void *rgba8);
void r3d_texture_destroy(r3d_texture *texture);
r3d_material r3d_material_default(void);   /* white, lit, a soft highlight */

/* Lights, per frame (r3d_begin_frame forgets them). With default lighting
 * on (the default) a frame that adds no light gets an ambient term and a
 * key light from over the camera's shoulder, so unlit scenes still read. */
typedef enum { R3D_LIGHT_DIRECTIONAL, R3D_LIGHT_POINT, R3D_LIGHT_SPOT } r3d_light_kind;
typedef struct {
    r3d_light_kind kind;
    r3d_vec3 position;       /* point and spot */
    r3d_vec3 direction;      /* directional and spot: where the light travels (normalized by the renderer) */
    r3d_color color;
    float intensity;
    float range;             /* point and spot: the light is gone at this distance */
    float cone;              /* spot: the full angle, radians */
    bool shadows;            /* casts shadows: the first shadowing directional light (a map over the frame's geometry),
                                the first R3D_MAX_SPOT_SHADOWS spots (a perspective map each) and the first
                                R3D_MAX_POINT_SHADOWS point lights (six cube faces each) */
} r3d_light;
#define R3D_MAX_LIGHTS 8
#define R3D_MAX_SPOT_SHADOWS 2
#define R3D_MAX_POINT_SHADOWS 1
#define R3D_SHADOW_LAYERS (R3D_MAX_SPOT_SHADOWS + R3D_MAX_POINT_SHADOWS * 6)
void r3d_set_shadows(bool enabled);   /* default on (when the maps could be created) */
bool r3d_shadows_enabled(void);

/* The sky: drawn behind everything (at the far plane). A gradient from the
 * ground colour through the horizon to the zenith with a sun disc along the
 * first directional light, or a cube map (six square RGBA8 faces in the
 * order +x, -x, +y, -y, +z, -z). Per frame; begin_frame clears it. */
typedef struct {
    r3d_color zenith, horizon, ground;
    float sun_size;          /* angular size, radians (0: no sun) */
    float sun_intensity;
    r3d_texture cube;        /* zero image id: the gradient */
} r3d_sky;
bool r3d_texture_create_cube(r3d_texture *texture, int size, const void *const faces[6]);
void r3d_set_sky(const r3d_sky *sky);
void r3d_set_ambient(r3d_color color, float intensity);   /* replaces the frame's ambient term */
void r3d_add_ambient(r3d_color color, float intensity);   /* adds to it */
bool r3d_add_light(const r3d_light *light);               /* false when the frame's lights are full */
void r3d_set_default_lighting(bool enabled);
int r3d_light_count(void);

/* Drawing. Primitives with alpha < 255 are blended after the opaque ones,
 * back to front. Lines are one pixel wide: depth-tested, or overlay (always
 * visible, for gizmos). */
void r3d_draw_primitive(const r3d_primitive *primitive);
void r3d_draw_sphere(r3d_vec3 center, float radius, r3d_color color);
void r3d_draw_capsule(r3d_vec3 center, float radius, float height, r3d_vec3 rotation, r3d_color color);
void r3d_draw_cube(r3d_vec3 center, r3d_vec3 size, r3d_vec3 rotation, r3d_color color);
void r3d_draw_plane(r3d_vec3 center, float width, float depth, r3d_vec3 rotation, r3d_color color);
/* A triangle mesh in world space (r3d_mesh.h vertices: position, normal,
 * uv, colour; `cap` must be 0), copied for this frame. `tint` multiplies
 * the vertex colours; alpha < 255 blends. */
void r3d_draw_mesh(const r3d_vertex *vertices, int vertex_count, const uint32_t *indices, int index_count,
                   const r3d_material *material, r3d_color tint, bool wireframe);
/* A decal: a texture projected onto the opaque shapes and meshes of the
 * frame that its box overlaps (bullet holes, posters, stains - Quake's way:
 * the surfaces' triangles are clipped to the box and drawn again with the
 * decal's material, blended and pulled a little towards the camera). The
 * box is `size` (x, y, z) around `position`, rotated like a primitive, and
 * projects along its local -y: an unrotated decal lies flat on the ground
 * with its image's top towards -z, one pitched by 90 degrees sits on a wall
 * facing +z. Surfaces whose normal is more than `max_angle` (radians; zero
 * -> 80 degrees) from the projection are skipped, as are translucent
 * shapes, wireframes and other decals. `id` registers the box for picking
 * (R3D_ID_NONE: not pickable). The tint's alpha fades the whole decal. */
typedef struct {
    r3d_vec3 position, rotation, size;
    r3d_color color;
    const r3d_material *material;     /* its texture (the alpha cuts it out); NULL: the tint alone */
    float max_angle;
    uint64_t id;
} r3d_decal;
#define R3D_MAX_DECALS 256
void r3d_draw_decal(const r3d_decal *decal);
/* The world-space corners (8) and the local axes of a decal's box, for gizmos. */
void r3d_decal_corners(const r3d_decal *decal, r3d_vec3 corners[8]);
void r3d_draw_line(r3d_vec3 a, r3d_vec3 b, r3d_color color);
void r3d_draw_line_overlay(r3d_vec3 a, r3d_vec3 b, r3d_color color);
/* The edges of a primitive as overlay lines (selection outlines). */
void r3d_draw_outline(const r3d_primitive *primitive, r3d_color color);
void r3d_draw_box_outline(r3d_vec3 lo, r3d_vec3 hi, r3d_color color);   /* an axis-aligned box, overlay */
/* A grid on y = 0 centred on the origin: `half_lines` lines each side of
 * the axes, `spacing` apart, every tenth one in `major`. */
void r3d_draw_grid(float spacing, int half_lines, r3d_color minor, r3d_color major);
void r3d_draw_axes(r3d_vec3 origin, float length);   /* x red, y green, z blue, overlay */

/* Picking: primitives submitted with an id and boxes registered with
 * r3d_pick_aabb are hit-tested by exact ray casts; the nearest wins. */
void r3d_pick_aabb(uint64_t id, r3d_vec3 lo, r3d_vec3 hi);
#define R3D_MAX_PICK_PLANES 12
void r3d_pick_convex(uint64_t id, const r3d_plane *planes, int count);   /* a convex solid (brushes) */
typedef struct {
    uint64_t id;             /* R3D_ID_NONE when nothing was hit */
    float t;
    r3d_vec3 point, normal;  /* the surface normal at the hit (boxes, solids, spheres, cubes and planes exactly; capsules approximately) */
} r3d_pick_hit;
bool r3d_pick(r3d_ray ray, r3d_pick_hit *hit);
/* The same, considering only the ids `accept` approves (an editor's surface tools skip its decal boxes). */
bool r3d_pick_filtered(r3d_ray ray, r3d_pick_hit *hit, bool (*accept)(uint64_t id, void *user), void *user);
uint64_t r3d_pick_at(float sx, float sy);         /* window point -> id */
uint64_t r3d_pick_ray(r3d_ray ray, float *t);

/* Renders everything submitted since r3d_begin_frame into the 3D target
 * (its own pass: call it outside any other pass). */
void r3d_render(void);
/* Draws the 3D target at the viewport rectangle into the pass currently
 * open, whose attachments have these formats and sample count and which
 * covers a window `pass_w` x `pass_h` logical pixels (the rectangle given
 * to r3d_begin_frame is placed within it). Once per frame. */
void r3d_composite(sg_pixel_format color_format, sg_pixel_format depth_format, int sample_count, float pass_w, float pass_h);
/* Standalone: a window pass with the composite, then commit. */
void r3d_present(void);

typedef struct {
    int primitives, meshes, mesh_vertices, lines, lights, draw_calls;
    int target_width, target_height, sample_count;
    bool shadow_pass;        /* a directional shadow map was rendered this frame */
    int local_shadow_passes; /* spot and point shadow faces rendered this frame */
    int decals, decal_triangles;   /* decals drawn and the triangles their projections produced */
} r3d_stats;
r3d_stats r3d_get_stats(void);

#ifdef __cplusplus
}
#endif
#endif
