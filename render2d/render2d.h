/* render2d: a small reusable 2D renderer on top of Sokol.
 *
 * What it gives an application:
 *   - a window-sized, DPI-aware canvas that follows window resizes
 *   - a 2D camera (pan, zoom about a point, fit to a rectangle) with optional
 *     built-in mouse/keyboard controls
 *   - immediate-mode drawing of circles, rectangles, lines, polygons, grids
 *     and text, in world space or screen space
 *   - picking: shapes submitted with an id can be queried by point or box,
 *     and an optional selection controller turns clicks, shift-clicks,
 *     box-drags and item-drags into a selection set the application reads
 *   - a minimal immediate-mode UI (panels, labels, buttons, toggles, bars)
 *     that also tells the world layer when the mouse is busy with the UI
 *   - per-frame input state (mouse, wheel, keys) derived from sokol_app events
 *
 * It owns nothing of the application's data: entities are referenced only by
 * the 64-bit ids the application submits with its pickable shapes. The
 * renderer depends on sokol_app, sokol_gfx, sokol_gl and sokol_debugtext,
 * whose implementations must be compiled into exactly one translation unit
 * (examples/sokol_impl.c does this).
 *
 * Frame protocol, from the sokol_app callbacks:
 *   init:    r2d_setup(&desc)
 *   event:   r2d_handle_event(event)
 *   frame:   r2d_begin_frame();
 *            ... draw the world, submit pickables ...
 *            r2d_selection_update(&result, true);   (optional)
 *            ... draw the UI in screen space ...
 *            r2d_end_frame();
 *   cleanup: r2d_shutdown()
 *
 * Coordinates: world space is whatever the application likes; screen space
 * is in logical pixels with the origin at the top-left of the window (the
 * framebuffer may be larger on high-DPI displays; the renderer hides this).
 * With y_up the world's +y axis points up on screen, otherwise down.
 *
 * The pure parts (vectors, rectangles, colors, camera math) are inline in
 * this header and need no window, so they can be unit-tested headlessly.
 */
#ifndef RENDER2D_H
#define RENDER2D_H

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sokol_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Vectors, rectangles, colors
 * ------------------------------------------------------------------------- */
typedef struct { float x, y; } r2d_vec2;
typedef struct { float x, y, w, h; } r2d_rect;      /* x, y is the min corner */
typedef struct { uint8_t r, g, b, a; } r2d_color;

static inline r2d_vec2 r2d_v2(float x, float y) { r2d_vec2 v = {x, y}; return v; }
static inline r2d_vec2 r2d_add(r2d_vec2 a, r2d_vec2 b) { return r2d_v2(a.x + b.x, a.y + b.y); }
static inline r2d_vec2 r2d_sub(r2d_vec2 a, r2d_vec2 b) { return r2d_v2(a.x - b.x, a.y - b.y); }
static inline r2d_vec2 r2d_scale(r2d_vec2 a, float s) { return r2d_v2(a.x * s, a.y * s); }
static inline float r2d_dot(r2d_vec2 a, r2d_vec2 b) { return a.x * b.x + a.y * b.y; }
static inline float r2d_length(r2d_vec2 a) { return sqrtf(a.x * a.x + a.y * a.y); }
static inline float r2d_distance(r2d_vec2 a, r2d_vec2 b) { return r2d_length(r2d_sub(a, b)); }
static inline r2d_vec2 r2d_normalize(r2d_vec2 a) {
    float len = r2d_length(a);
    return len > 0 ? r2d_scale(a, 1.0f / len) : r2d_v2(0, 0);
}
static inline r2d_vec2 r2d_lerp(r2d_vec2 a, r2d_vec2 b, float t) {
    return r2d_v2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}
static inline float r2d_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline r2d_rect r2d_rect_make(float x, float y, float w, float h) { r2d_rect r = {x, y, w, h}; return r; }
/* Rectangle spanning two arbitrary corners (normalized so w, h >= 0). */
static inline r2d_rect r2d_rect_from_points(r2d_vec2 a, r2d_vec2 b) {
    float x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    float x1 = a.x < b.x ? b.x : a.x, y1 = a.y < b.y ? b.y : a.y;
    return r2d_rect_make(x0, y0, x1 - x0, y1 - y0);
}
static inline r2d_rect r2d_rect_centered(r2d_vec2 center, float w, float h) {
    return r2d_rect_make(center.x - w * 0.5f, center.y - h * 0.5f, w, h);
}
static inline r2d_vec2 r2d_rect_min(r2d_rect r) { return r2d_v2(r.x, r.y); }
static inline r2d_vec2 r2d_rect_max(r2d_rect r) { return r2d_v2(r.x + r.w, r.y + r.h); }
static inline r2d_vec2 r2d_rect_center(r2d_rect r) { return r2d_v2(r.x + r.w * 0.5f, r.y + r.h * 0.5f); }
static inline bool r2d_rect_contains(r2d_rect r, r2d_vec2 p) {
    return p.x >= r.x && p.x <= r.x + r.w && p.y >= r.y && p.y <= r.y + r.h;
}
static inline bool r2d_rect_overlaps(r2d_rect a, r2d_rect b) {
    return a.x <= b.x + b.w && b.x <= a.x + a.w && a.y <= b.y + b.h && b.y <= a.y + a.h;
}
static inline bool r2d_rect_overlaps_circle(r2d_rect r, r2d_vec2 c, float radius) {
    float nx = r2d_clampf(c.x, r.x, r.x + r.w), ny = r2d_clampf(c.y, r.y, r.y + r.h);
    float dx = c.x - nx, dy = c.y - ny;
    return dx * dx + dy * dy <= radius * radius;
}
static inline r2d_rect r2d_rect_expand(r2d_rect r, float amount) {
    return r2d_rect_make(r.x - amount, r.y - amount, r.w + 2 * amount, r.h + 2 * amount);
}

static inline r2d_color r2d_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { r2d_color c = {r, g, b, a}; return c; }
static inline r2d_color r2d_rgb(uint8_t r, uint8_t g, uint8_t b) { return r2d_rgba(r, g, b, 255); }
static inline r2d_color r2d_with_alpha(r2d_color c, uint8_t a) { c.a = a; return c; }
/* Linear blend of two colors, t in [0, 1]. */
static inline r2d_color r2d_color_mix(r2d_color a, r2d_color b, float t) {
    t = r2d_clampf(t, 0, 1);
    return r2d_rgba((uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
                    (uint8_t)(a.b + (b.b - a.b) * t), (uint8_t)(a.a + (b.a - a.a) * t));
}

/* ---------------------------------------------------------------------------
 * Camera
 *
 * A camera is a plain value: the world point at the viewport center, the
 * zoom in screen pixels per world unit, its limits and the y orientation.
 * The functions below are pure; `viewport` is the viewport size in logical
 * pixels. The renderer keeps one current camera (r2d_camera / r2d_set_camera)
 * and offers the same operations bound to it.
 * ------------------------------------------------------------------------- */
typedef struct {
    r2d_vec2 center;
    float zoom;
    float min_zoom, max_zoom;
    bool y_up;
} r2d_camera;

static inline r2d_camera r2d_camera_make(r2d_vec2 center, float zoom, bool y_up) {
    r2d_camera c;
    c.center = center;
    c.zoom = zoom > 0 ? zoom : 1.0f;
    c.min_zoom = 0.01f;
    c.max_zoom = 1000.0f;
    c.y_up = y_up;
    return c;
}
static inline float r2d_camera_clamp_zoom(const r2d_camera *c, float zoom) {
    return r2d_clampf(zoom, c->min_zoom, c->max_zoom);
}
static inline r2d_vec2 r2d_camera_world_to_screen(const r2d_camera *c, r2d_vec2 viewport, r2d_vec2 p) {
    float dx = (p.x - c->center.x) * c->zoom, dy = (p.y - c->center.y) * c->zoom;
    return r2d_v2(viewport.x * 0.5f + dx, viewport.y * 0.5f + (c->y_up ? -dy : dy));
}
static inline r2d_vec2 r2d_camera_screen_to_world(const r2d_camera *c, r2d_vec2 viewport, r2d_vec2 s) {
    float dx = (s.x - viewport.x * 0.5f) / c->zoom, dy = (s.y - viewport.y * 0.5f) / c->zoom;
    return r2d_v2(c->center.x + dx, c->center.y + (c->y_up ? -dy : dy));
}
/* Screen-space delta to world-space delta (and back). */
static inline r2d_vec2 r2d_camera_screen_delta_to_world(const r2d_camera *c, r2d_vec2 d) {
    return r2d_v2(d.x / c->zoom, (c->y_up ? -d.y : d.y) / c->zoom);
}
static inline r2d_vec2 r2d_camera_world_delta_to_screen(const r2d_camera *c, r2d_vec2 d) {
    return r2d_v2(d.x * c->zoom, (c->y_up ? -d.y : d.y) * c->zoom);
}
/* Move the view by a screen-space amount (the content follows the mouse). */
static inline void r2d_camera_pan_screen(r2d_camera *c, r2d_vec2 screen_delta) {
    c->center = r2d_sub(c->center, r2d_camera_screen_delta_to_world(c, screen_delta));
}
/* Multiply the zoom while keeping the world point under `screen_anchor` fixed. */
static inline void r2d_camera_zoom_at(r2d_camera *c, r2d_vec2 viewport, r2d_vec2 screen_anchor, float factor) {
    r2d_vec2 before = r2d_camera_screen_to_world(c, viewport, screen_anchor);
    c->zoom = r2d_camera_clamp_zoom(c, c->zoom * factor);
    r2d_vec2 after = r2d_camera_screen_to_world(c, viewport, screen_anchor);
    c->center = r2d_add(c->center, r2d_sub(before, after));
}
/* World rectangle currently visible in the viewport. */
static inline r2d_rect r2d_camera_visible_rect(const r2d_camera *c, r2d_vec2 viewport) {
    r2d_vec2 a = r2d_camera_screen_to_world(c, viewport, r2d_v2(0, 0));
    r2d_vec2 b = r2d_camera_screen_to_world(c, viewport, viewport);
    return r2d_rect_from_points(a, b);
}
/* Center and zoom so that `world` fills `screen_area` (a sub-rectangle of the
 * viewport) with `padding` pixels to spare on each side. */
static inline void r2d_camera_fit_in(r2d_camera *c, r2d_vec2 viewport, r2d_rect screen_area,
                                     r2d_rect world, float padding) {
    float avail_w = screen_area.w - 2 * padding, avail_h = screen_area.h - 2 * padding;
    if (avail_w <= 0 || avail_h <= 0 || world.w <= 0 || world.h <= 0) return;
    float zx = avail_w / world.w, zy = avail_h / world.h;
    c->zoom = r2d_camera_clamp_zoom(c, zx < zy ? zx : zy);
    c->center = r2d_rect_center(world);
    /* Shift so the world center lands on the area's center rather than the viewport's. */
    r2d_vec2 offset = r2d_sub(r2d_rect_center(screen_area), r2d_scale(viewport, 0.5f));
    c->center = r2d_sub(c->center, r2d_camera_screen_delta_to_world(c, offset));
}
static inline void r2d_camera_fit(r2d_camera *c, r2d_vec2 viewport, r2d_rect world, float padding) {
    r2d_camera_fit_in(c, viewport, r2d_rect_make(0, 0, viewport.x, viewport.y), world, padding);
}

/* ---------------------------------------------------------------------------
 * Setup
 * ------------------------------------------------------------------------- */
typedef enum { R2D_SPACE_WORLD, R2D_SPACE_SCREEN } r2d_space;
typedef enum { R2D_MOUSE_LEFT, R2D_MOUSE_RIGHT, R2D_MOUSE_MIDDLE, R2D_MOUSE_BUTTONS } r2d_mouse_button;
#define R2D_ID_NONE UINT64_MAX

typedef struct {
    r2d_color panel, panel_border, text, text_dim;
    r2d_color button, button_hover, button_active, button_text;
    r2d_color toggle_on;
    r2d_color selection_box, selection_box_border;
} r2d_ui_style;
r2d_ui_style r2d_ui_style_default(void);

typedef struct {
    r2d_camera camera;             /* initial camera; zero -> center (0,0), zoom 1, y down */
    r2d_color clear_color;         /* zero -> dark gray */
    float text_scale;              /* glyph size multiplier, 8 px glyphs at 1; zero -> 1.5 */
    int max_vertices, max_commands;/* sokol_gl budgets; zero -> 1M / 64K */
    int text_chars;                /* sokol_debugtext character budget; zero -> 64K */
    bool skip_gfx_setup;           /* the app already called sg_setup */
    bool no_post_process;          /* draw straight to the window; no post-process stack (see r2d_post.h) */
    const char *post_config;       /* optional tunable.cfg to load for the post-process stack at setup */

    /* Built-in camera controls (all optional). */
    bool camera_controls;          /* enable the controls below */
    bool pan_with_right_button;    /* drag with the right mouse button pans (default on) */
    bool pan_with_middle_button;   /* drag with the middle mouse button pans (default on) */
    bool pan_with_arrows;          /* arrow keys pan, +/- zoom, Home resets */
    bool pan_with_wasd;            /* WASD pans as well (leave off when the app uses those keys) */
    float key_pan_speed;           /* screen pixels per second; zero -> 600 */
    float wheel_zoom_step;         /* zoom factor per wheel notch; zero -> 1.15 */

    /* Built-in selection controller. */
    float pick_slop;               /* extra pixels around shapes when picking; zero -> 2 */
    float drag_threshold;          /* pixels before a press becomes a drag; zero -> 4 */
    bool escape_clears_selection;  /* Escape empties the selection */
    bool draw_selection_box;       /* the controller draws the marquee itself */
    r2d_ui_style style;            /* zero -> r2d_ui_style_default() */
} r2d_desc;

void r2d_setup(const r2d_desc *desc);
void r2d_shutdown(void);
bool r2d_valid(void);               /* false when graphics setup failed */
void r2d_handle_event(const sapp_event *event);
void r2d_begin_frame(void);
void r2d_end_frame(void);
void r2d_set_clear_color(r2d_color color);
void r2d_set_text_scale(float scale);   /* takes effect next frame */

/* ---------------------------------------------------------------------------
 * Viewport and camera
 * ------------------------------------------------------------------------- */
r2d_vec2 r2d_viewport(void);        /* logical size in pixels */
float r2d_dpi_scale(void);
float r2d_dt(void);                 /* seconds since the previous frame, clamped */
uint64_t r2d_frame_count(void);

r2d_camera *r2d_camera_ptr(void);   /* the current camera, editable */
void r2d_set_camera(r2d_camera camera);
void r2d_camera_reset(void);        /* back to the camera given at setup */
void r2d_camera_zoom_around(r2d_vec2 screen_anchor, float factor);
void r2d_camera_fit_rect(r2d_rect world, float padding);
void r2d_camera_fit_rect_in(r2d_rect screen_area, r2d_rect world, float padding);
r2d_rect r2d_visible_world(void);
r2d_vec2 r2d_world_to_screen(r2d_vec2 p);
r2d_vec2 r2d_screen_to_world(r2d_vec2 p);
float r2d_px(float pixels);         /* pixels -> world units at the current zoom */

/* ---------------------------------------------------------------------------
 * Input (valid between begin_frame and end_frame; "pressed"/"released" are
 * edge-triggered for the current frame)
 * ------------------------------------------------------------------------- */
r2d_vec2 r2d_mouse(void);           /* screen space */
r2d_vec2 r2d_mouse_world(void);
r2d_vec2 r2d_mouse_delta(void);     /* screen space, since the previous frame */
bool r2d_mouse_down(r2d_mouse_button button);
bool r2d_mouse_pressed(r2d_mouse_button button);
bool r2d_mouse_released(r2d_mouse_button button);
float r2d_wheel(void);              /* vertical scroll this frame, in notches */
bool r2d_key_down(sapp_keycode key);
bool r2d_key_pressed(sapp_keycode key);
bool r2d_key_released(sapp_keycode key);
uint32_t r2d_modifiers(void);       /* SAPP_MODIFIER_* bits */
bool r2d_mouse_over_ui(void);       /* the mouse is on a UI element; world input should ignore it */
bool r2d_resized_this_frame(void);

/* ---------------------------------------------------------------------------
 * Drawing
 *
 * Shapes are drawn in the current space: world (transformed by the camera)
 * or screen. Lengths such as radius and thickness are in the units of that
 * space. Text is always rendered at a fixed pixel size, anchored at a
 * position in the current space. Drawing order is submission order within a
 * space; the screen space is always composited above the world space.
 * ------------------------------------------------------------------------- */
void r2d_set_space(r2d_space space);
r2d_space r2d_get_space(void);

void r2d_draw_circle(r2d_vec2 center, float radius, r2d_color color);
void r2d_draw_ring(r2d_vec2 center, float radius, float thickness, r2d_color color);
void r2d_draw_arc(r2d_vec2 center, float radius, float thickness, float start_rad, float end_rad, r2d_color color);
void r2d_draw_rect(r2d_rect rect, r2d_color color);
void r2d_draw_rect_outline(r2d_rect rect, float thickness, r2d_color color);
void r2d_draw_line(r2d_vec2 a, r2d_vec2 b, float thickness, r2d_color color);
void r2d_draw_polyline(const r2d_vec2 *points, size_t count, float thickness, bool closed, r2d_color color);
void r2d_draw_polygon(const r2d_vec2 *points, size_t count, r2d_color color);  /* convex, fan from points[0] */
void r2d_draw_triangle(r2d_vec2 a, r2d_vec2 b, r2d_vec2 c, r2d_color color);
/* A rotated rectangle/sprite stand-in: width along `direction`, height across it. */
void r2d_draw_oriented_rect(r2d_vec2 center, r2d_vec2 direction, float width, float height, r2d_color color);
/* Grid of lines over the visible part of `bounds` (world space; a rect with
 * zero size means everything visible). The spacing is multiplied by 2, 2.5
 * and 2 in turn (1, 2, 5, 10, ...) until lines are at least `min_pixels`
 * apart on screen. */
void r2d_draw_grid(r2d_rect bounds, float spacing, float min_pixels, r2d_color color);
/* Horizontal bar filled from the left by `fraction` (0..1), e.g. a health bar. */
void r2d_draw_bar(r2d_rect rect, float fraction, r2d_color fill, r2d_color background);

void r2d_draw_text(r2d_vec2 pos, r2d_color color, const char *text);
void r2d_draw_textf(r2d_vec2 pos, r2d_color color, const char *fmt, ...);
void r2d_draw_text_centered(r2d_vec2 pos, r2d_color color, const char *text);
float r2d_text_width(const char *text);   /* pixels */
float r2d_text_height(void);              /* pixels */

/* ---------------------------------------------------------------------------
 * Picking and selection
 *
 * Pickables are submitted in the current space and live until the end of
 * the frame; queries resolve against everything submitted so far this
 * frame. The last submitted hit wins (it is drawn on top). Ids are opaque
 * 64-bit values chosen by the application; R2D_ID_NONE is reserved.
 * ------------------------------------------------------------------------- */
void r2d_pick_circle(uint64_t id, r2d_vec2 center, float radius);
void r2d_pick_rect(uint64_t id, r2d_rect rect);
uint64_t r2d_pick_at(r2d_vec2 screen);                           /* R2D_ID_NONE when nothing */
size_t r2d_pick_in_rect(r2d_rect screen, uint64_t *out, size_t capacity); /* count of all hits */
uint64_t r2d_hovered(void);                                      /* pick under the mouse */

/* Selection set, kept sorted and unique. */
size_t r2d_selection_count(void);
const uint64_t *r2d_selection(size_t *count);
bool r2d_is_selected(uint64_t id);
void r2d_select_set(uint64_t id);          /* select only this */
bool r2d_select_add(uint64_t id);          /* true when newly added */
bool r2d_select_remove(uint64_t id);       /* true when it was selected */
void r2d_select_toggle(uint64_t id);
void r2d_select_clear(void);
/* Drop every selected id for which `keep` returns false (e.g. dead entities). */
size_t r2d_selection_prune(bool (*keep)(uint64_t id, void *user), void *user);

/* The selection controller. Call once per frame after submitting pickables
 * and before drawing the UI. Behaviour:
 *   left click on an item          selects it alone
 *   shift/ctrl + click             toggles it in the selection
 *   click on empty space           clears the selection (not with shift/ctrl)
 *   drag from empty space          box-selects (shift/ctrl adds to the selection)
 *   drag from a selected item      reports an item drag with the world delta so
 *                                  the application can move what is selected
 * Input is ignored while the mouse is over the UI (unless a drag is already
 * in progress) and the whole controller is skipped when `enabled` is false
 * in which case only the result is cleared. */
typedef struct {
    bool changed;             /* the selection set changed this frame */
    uint64_t clicked;         /* item clicked (press + release without dragging), else R2D_ID_NONE */
    uint64_t hovered;         /* item under the mouse, else R2D_ID_NONE */
    bool dragging;            /* an item drag is in progress */
    bool drag_ended;          /* the item drag ended this frame */
    uint64_t drag_id;         /* item the drag started on */
    r2d_vec2 drag_delta;      /* world-space movement since the previous frame */
    r2d_vec2 drag_total;      /* world-space movement since the drag started */
    bool box_active;          /* a box selection is being dragged */
    r2d_rect box;             /* the box in screen space */
    bool clicked_empty;       /* press + release on empty space, not over the UI */
    r2d_vec2 click_world;     /* world position of `clicked`/`clicked_empty` */
} r2d_selection_result;
void r2d_selection_update(r2d_selection_result *result, bool enabled);

/* ---------------------------------------------------------------------------
 * Minimal immediate-mode UI, in screen space. Widgets register their
 * rectangles so r2d_mouse_over_ui() can shield the world from clicks. A
 * panel only draws and registers; buttons/toggles report activation on
 * release. Text is clipped by truncation to the widget width.
 * ------------------------------------------------------------------------- */
void r2d_ui_panel(r2d_rect rect);
void r2d_ui_label(r2d_vec2 pos, const char *text);
void r2d_ui_labelf(r2d_vec2 pos, const char *fmt, ...);
void r2d_ui_label_dim(r2d_vec2 pos, const char *text);
void r2d_ui_label_colored(r2d_vec2 pos, r2d_color color, const char *text);
bool r2d_ui_button(r2d_rect rect, const char *label);
bool r2d_ui_button_colored(r2d_rect rect, const char *label, r2d_color color);
bool r2d_ui_toggle(r2d_rect rect, const char *label, bool *value);
void r2d_ui_bar(r2d_rect rect, float fraction, r2d_color fill);
/* Register an area as UI without drawing it (e.g. a custom widget). */
void r2d_ui_block(r2d_rect rect);
const r2d_ui_style *r2d_ui_style_get(void);

/* Diagnostics for the last completed frame. */
typedef struct {
    size_t pickables, selection, ui_rects;
    int gl_vertices, gl_commands;
    double frame_seconds;
} r2d_stats;
r2d_stats r2d_get_stats(void);

#ifdef __cplusplus
}
#endif
#endif
