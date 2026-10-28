/* r2d_effects: timed visual effects on top of render2d.
 *
 * An effect is a short-lived drawing driven by an eased 0..1 progress value:
 * floating text, an expanding ring (shockwave), a fading disc, a fading beam
 * between two points, a slash arc, a full-screen flash, a camera shake, or a
 * custom callback. Effects live in a fixed pool, are updated with dt and
 * drawn per space (world effects with the camera, screen effects on top).
 *
 * Each effect returns a handle that can be used to move it (so it follows
 * an object), re-color it, or cancel it; an effect started with
 * duration <= 0 loops its progress until cancelled, which is how persistent
 * markers (auras, target rings) are made. Stale handles are ignored.
 *
 * The camera shake does not move the camera by itself: call
 * r2d_effects_camera_push before drawing the world and
 * r2d_effects_camera_pop afterwards, or read r2d_effects_shake_offset and
 * apply it yourself. Updates never touch the renderer, so the module can be
 * used and tested without a window.
 *
 * Ownership: r2d_effects_create allocates the pool, r2d_effects_destroy
 * releases it. Text is copied (truncated to R2D_EFFECT_TEXT_MAX - 1 bytes).
 */
#ifndef R2D_EFFECTS_H
#define R2D_EFFECTS_H

#include "render2d.h"

#ifdef __cplusplus
extern "C" {
#endif

#define R2D_EFFECT_TEXT_MAX 32

typedef enum {
    R2D_EASE_LINEAR,
    R2D_EASE_IN,          /* quadratic */
    R2D_EASE_OUT,
    R2D_EASE_IN_OUT,
    R2D_EASE_OUT_CUBIC,
    R2D_EASE_OUT_BACK,    /* overshoots slightly then settles */
    R2D_EASE_OUT_ELASTIC,
    R2D_EASE_OUT_BOUNCE,
    R2D_EASE_PULSE        /* 0 -> 1 -> 0 (sine) */
} r2d_ease;
float r2d_ease_apply(r2d_ease ease, float t);     /* t clamped to 0..1 */

typedef struct r2d_effects r2d_effects;
typedef struct { uint32_t id; } r2d_effect;

/* Custom effect callback: t is the eased progress (0..1), age in seconds;
 * the renderer's space is already set to the effect's space. */
typedef void (*r2d_effect_fn)(r2d_effect handle, float t, float age, r2d_vec2 position, r2d_color color, void *user);

r2d_effects *r2d_effects_create(size_t capacity);
void r2d_effects_destroy(r2d_effects *fx);
void r2d_effects_clear(r2d_effects *fx);

/* World-space effects (positions in world units unless noted). */
r2d_effect r2d_effects_text(r2d_effects *fx, r2d_vec2 position, const char *text, r2d_color color,
                            float duration, r2d_vec2 drift);            /* drifts by `drift` over its life and fades */
r2d_effect r2d_effects_ring(r2d_effects *fx, r2d_vec2 position, float radius_from, float radius_to,
                            float thickness, r2d_color color, float duration, r2d_ease ease);
r2d_effect r2d_effects_disc(r2d_effects *fx, r2d_vec2 position, float radius, r2d_color color, float duration);
r2d_effect r2d_effects_beam(r2d_effects *fx, r2d_vec2 from, r2d_vec2 to, float thickness, r2d_color color, float duration);
/* Arc sweeping `sweep` radians from `start` around `position` at `radius`
 * (a slash): the visible arc grows along the sweep and fades. */
r2d_effect r2d_effects_arc(r2d_effects *fx, r2d_vec2 position, float radius, float thickness,
                           float start, float sweep, r2d_color color, float duration);
/* Screen-space effects. */
r2d_effect r2d_effects_flash(r2d_effects *fx, r2d_color color, float duration);   /* full-screen overlay */
r2d_effect r2d_effects_shake(r2d_effects *fx, float amplitude_px, float frequency_hz, float duration);
/* Custom: the callback draws it; `space` selects world or screen. */
r2d_effect r2d_effects_custom(r2d_effects *fx, r2d_space space, r2d_vec2 position, r2d_color color,
                              float duration, r2d_ease ease, r2d_effect_fn fn, void *user);

bool r2d_effects_alive(const r2d_effects *fx, r2d_effect handle);
void r2d_effects_move(r2d_effects *fx, r2d_effect handle, r2d_vec2 position);   /* beams move their `from` */
void r2d_effects_move_end(r2d_effects *fx, r2d_effect handle, r2d_vec2 to);     /* beams: the `to` end */
void r2d_effects_recolor(r2d_effects *fx, r2d_effect handle, r2d_color color);
void r2d_effects_cancel(r2d_effects *fx, r2d_effect handle);
float r2d_effects_progress(const r2d_effects *fx, r2d_effect handle);            /* eased 0..1, 0 when dead */

void r2d_effects_update(r2d_effects *fx, float dt);
void r2d_effects_draw(const r2d_effects *fx, r2d_space space);    /* draws the effects of that space */
r2d_vec2 r2d_effects_shake_offset(const r2d_effects *fx);         /* screen pixels, sums all shakes */
void r2d_effects_camera_push(const r2d_effects *fx);              /* offset the current camera by the shake */
void r2d_effects_camera_pop(const r2d_effects *fx);               /* undo the push */

size_t r2d_effects_count(const r2d_effects *fx);
size_t r2d_effects_capacity(const r2d_effects *fx);

#ifdef __cplusplus
}
#endif
#endif
