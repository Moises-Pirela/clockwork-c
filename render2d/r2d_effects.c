#include "r2d_effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R2D_E_PI 3.14159265358979f

typedef enum { FX_TEXT, FX_RING, FX_DISC, FX_BEAM, FX_ARC, FX_FLASH, FX_SHAKE, FX_CUSTOM } fx_kind;

typedef struct {
    fx_kind kind;
    r2d_space space;
    r2d_ease ease;
    bool active, loop;
    uint32_t generation;
    float age, duration;
    r2d_vec2 a, b;               /* position (and beam end / text drift) */
    float r0, r1, thickness, start, sweep;   /* radii, arc, shake amplitude/frequency */
    r2d_color color;
    char text[R2D_EFFECT_TEXT_MAX];
    r2d_effect_fn fn;
    void *user;
    uint32_t phase;              /* shake noise seed */
} effect;

struct r2d_effects {
    effect *items;
    size_t capacity;
    r2d_vec2 pushed;             /* camera offset applied by camera_push (world units) */
    bool pushed_active;
};

float r2d_ease_apply(r2d_ease ease, float t) {
    t = r2d_clampf(t, 0, 1);
    switch (ease) {
        case R2D_EASE_IN: return t * t;
        case R2D_EASE_OUT: return 1 - (1 - t) * (1 - t);
        case R2D_EASE_IN_OUT: return t < 0.5f ? 2 * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) * 0.5f;
        case R2D_EASE_OUT_CUBIC: { float u = 1 - t; return 1 - u * u * u; }
        case R2D_EASE_OUT_BACK: {
            const float c1 = 1.70158f, c3 = c1 + 1;
            float u = t - 1;
            return 1 + c3 * u * u * u + c1 * u * u;
        }
        case R2D_EASE_OUT_ELASTIC: {
            if (t <= 0 || t >= 1) return t;
            const float c4 = 2 * R2D_E_PI / 3;
            return powf(2, -10 * t) * sinf((t * 10 - 0.75f) * c4) + 1;
        }
        case R2D_EASE_OUT_BOUNCE: {
            const float n1 = 7.5625f, d1 = 2.75f;
            if (t < 1 / d1) return n1 * t * t;
            if (t < 2 / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
            if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
            t -= 2.625f / d1;
            return n1 * t * t + 0.984375f;
        }
        case R2D_EASE_PULSE: return sinf(t * R2D_E_PI);
        default: return t;
    }
}

r2d_effects *r2d_effects_create(size_t capacity) {
    if (!capacity) capacity = 256;
    r2d_effects *fx = calloc(1, sizeof(*fx));
    if (!fx) return NULL;
    fx->items = calloc(capacity, sizeof(*fx->items));
    if (!fx->items) { free(fx); return NULL; }
    fx->capacity = capacity;
    for (size_t i = 0; i < capacity; ++i) fx->items[i].generation = 1;
    return fx;
}
void r2d_effects_destroy(r2d_effects *fx) {
    if (!fx) return;
    free(fx->items);
    free(fx);
}
static void retire(effect *e) {
    e->active = false;
    if (++e->generation == 0) e->generation = 1;
}
void r2d_effects_clear(r2d_effects *fx) {
    if (!fx) return;
    for (size_t i = 0; i < fx->capacity; ++i) if (fx->items[i].active) retire(&fx->items[i]);
}

static effect *lookup(const r2d_effects *fx, r2d_effect handle) {
    if (!fx || !handle.id) return NULL;
    size_t index = (handle.id & 0xffff) - 1;
    uint32_t generation = handle.id >> 16;
    if (index >= fx->capacity) return NULL;
    effect *e = &fx->items[index];
    return e->active && e->generation == generation ? e : NULL;
}
static r2d_effect add(r2d_effects *fx, fx_kind kind, r2d_space space, float duration, r2d_ease ease, effect **out) {
    r2d_effect none = {0};
    *out = NULL;
    if (!fx) return none;
    for (size_t i = 0; i < fx->capacity && i < 0xffff; ++i) {
        effect *e = &fx->items[i];
        if (e->active) continue;
        uint32_t generation = e->generation;
        memset(e, 0, sizeof(*e));
        e->generation = generation;
        e->active = true;
        e->kind = kind;
        e->space = space;
        e->ease = ease;
        e->loop = duration <= 0;
        e->duration = duration > 0 ? duration : 1.0f;
        e->phase = (uint32_t)i * 2654435761u;
        *out = e;
        r2d_effect handle = {(uint32_t)(i + 1) | (generation << 16)};
        return handle;
    }
    return none;
}

r2d_effect r2d_effects_text(r2d_effects *fx, r2d_vec2 position, const char *text, r2d_color color, float duration, r2d_vec2 drift) {
    effect *e;
    r2d_effect h = add(fx, FX_TEXT, R2D_SPACE_WORLD, duration, R2D_EASE_OUT, &e);
    if (e) {
        e->a = position;
        e->b = drift;
        e->color = color;
        snprintf(e->text, sizeof(e->text), "%s", text ? text : "");
    }
    return h;
}
r2d_effect r2d_effects_ring(r2d_effects *fx, r2d_vec2 position, float r0, float r1, float thickness, r2d_color color, float duration, r2d_ease ease) {
    effect *e;
    r2d_effect h = add(fx, FX_RING, R2D_SPACE_WORLD, duration, ease, &e);
    if (e) { e->a = position; e->r0 = r0; e->r1 = r1; e->thickness = thickness; e->color = color; }
    return h;
}
r2d_effect r2d_effects_disc(r2d_effects *fx, r2d_vec2 position, float radius, r2d_color color, float duration) {
    effect *e;
    r2d_effect h = add(fx, FX_DISC, R2D_SPACE_WORLD, duration, R2D_EASE_OUT, &e);
    if (e) { e->a = position; e->r0 = radius; e->color = color; }
    return h;
}
r2d_effect r2d_effects_beam(r2d_effects *fx, r2d_vec2 from, r2d_vec2 to, float thickness, r2d_color color, float duration) {
    effect *e;
    r2d_effect h = add(fx, FX_BEAM, R2D_SPACE_WORLD, duration, R2D_EASE_OUT, &e);
    if (e) { e->a = from; e->b = to; e->thickness = thickness; e->color = color; }
    return h;
}
r2d_effect r2d_effects_arc(r2d_effects *fx, r2d_vec2 position, float radius, float thickness, float start, float sweep, r2d_color color, float duration) {
    effect *e;
    r2d_effect h = add(fx, FX_ARC, R2D_SPACE_WORLD, duration, R2D_EASE_OUT_CUBIC, &e);
    if (e) { e->a = position; e->r0 = radius; e->thickness = thickness; e->start = start; e->sweep = sweep; e->color = color; }
    return h;
}
r2d_effect r2d_effects_flash(r2d_effects *fx, r2d_color color, float duration) {
    effect *e;
    r2d_effect h = add(fx, FX_FLASH, R2D_SPACE_SCREEN, duration, R2D_EASE_OUT, &e);
    if (e) e->color = color;
    return h;
}
r2d_effect r2d_effects_shake(r2d_effects *fx, float amplitude, float frequency, float duration) {
    effect *e;
    r2d_effect h = add(fx, FX_SHAKE, R2D_SPACE_SCREEN, duration, R2D_EASE_OUT, &e);
    if (e) { e->r0 = amplitude; e->r1 = frequency > 0 ? frequency : 30.0f; }
    return h;
}
r2d_effect r2d_effects_custom(r2d_effects *fx, r2d_space space, r2d_vec2 position, r2d_color color, float duration, r2d_ease ease, r2d_effect_fn fn, void *user) {
    effect *e;
    r2d_effect h = add(fx, FX_CUSTOM, space, duration, ease, &e);
    if (e) { e->a = position; e->color = color; e->fn = fn; e->user = user; }
    return h;
}

bool r2d_effects_alive(const r2d_effects *fx, r2d_effect handle) { return lookup(fx, handle) != NULL; }
void r2d_effects_move(r2d_effects *fx, r2d_effect handle, r2d_vec2 position) {
    effect *e = lookup(fx, handle);
    if (e) e->a = position;
}
void r2d_effects_move_end(r2d_effects *fx, r2d_effect handle, r2d_vec2 to) {
    effect *e = lookup(fx, handle);
    if (e && e->kind == FX_BEAM) e->b = to;
}
void r2d_effects_recolor(r2d_effects *fx, r2d_effect handle, r2d_color color) {
    effect *e = lookup(fx, handle);
    if (e) e->color = color;
}
void r2d_effects_cancel(r2d_effects *fx, r2d_effect handle) {
    effect *e = lookup(fx, handle);
    if (e) retire(e);
}
static float progress(const effect *e) {
    float t = e->duration > 0 ? e->age / e->duration : 1.0f;
    if (e->loop) t = t - floorf(t);
    return r2d_ease_apply(e->ease, t);
}
float r2d_effects_progress(const r2d_effects *fx, r2d_effect handle) {
    const effect *e = lookup(fx, handle);
    return e ? progress(e) : 0;
}

void r2d_effects_update(r2d_effects *fx, float dt) {
    if (!fx || !(dt > 0)) return;
    for (size_t i = 0; i < fx->capacity; ++i) {
        effect *e = &fx->items[i];
        if (!e->active) continue;
        e->age += dt;
        if (!e->loop && e->age >= e->duration) retire(e);
    }
}

static uint8_t scale_alpha(uint8_t a, float f) { return (uint8_t)((float)a * r2d_clampf(f, 0, 1)); }
static r2d_vec2 shake_of(const effect *e) {
    /* two incommensurate sines per axis give a jittery, decaying offset */
    float decay = 1.0f - progress(e);
    float w = 2 * R2D_E_PI * e->r1 * e->age;
    float p = (float)(e->phase & 0xffff) * 0.001f;
    float x = sinf(w + p) * 0.7f + sinf(w * 1.37f + p * 3) * 0.3f;
    float y = cosf(w * 1.11f + p * 2) * 0.7f + sinf(w * 0.73f + p) * 0.3f;
    return r2d_scale(r2d_v2(x, y), e->r0 * decay);
}
r2d_vec2 r2d_effects_shake_offset(const r2d_effects *fx) {
    r2d_vec2 total = r2d_v2(0, 0);
    if (!fx) return total;
    for (size_t i = 0; i < fx->capacity; ++i) {
        const effect *e = &fx->items[i];
        if (e->active && e->kind == FX_SHAKE) total = r2d_add(total, shake_of(e));
    }
    return total;
}
void r2d_effects_camera_push(const r2d_effects *fx) {
    if (!fx) return;
    r2d_effects *m = (r2d_effects *)fx;
    if (m->pushed_active) r2d_effects_camera_pop(fx);
    r2d_camera *cam = r2d_camera_ptr();
    m->pushed = r2d_camera_screen_delta_to_world(cam, r2d_effects_shake_offset(fx));
    cam->center = r2d_add(cam->center, m->pushed);
    m->pushed_active = true;
}
void r2d_effects_camera_pop(const r2d_effects *fx) {
    if (!fx) return;
    r2d_effects *m = (r2d_effects *)fx;
    if (!m->pushed_active) return;
    r2d_camera *cam = r2d_camera_ptr();
    cam->center = r2d_sub(cam->center, m->pushed);
    m->pushed_active = false;
}

static void draw_one(const effect *e, r2d_effect handle) {
    float t = progress(e);
    switch (e->kind) {
        case FX_TEXT: {
            r2d_vec2 at = r2d_add(e->a, r2d_scale(e->b, t));
            r2d_color c = r2d_with_alpha(e->color, scale_alpha(e->color.a, 1.0f - t * t));
            r2d_draw_text_centered(at, c, e->text);
            break;
        }
        case FX_RING: {
            float r = e->r0 + (e->r1 - e->r0) * t;
            r2d_color c = r2d_with_alpha(e->color, scale_alpha(e->color.a, 1.0f - t));
            r2d_draw_ring(e->a, r, e->thickness, c);
            break;
        }
        case FX_DISC: {
            r2d_color c = r2d_with_alpha(e->color, scale_alpha(e->color.a, 1.0f - t));
            r2d_draw_circle(e->a, e->r0 * (0.6f + 0.4f * (1.0f - t)), c);
            break;
        }
        case FX_BEAM: {
            r2d_color c = r2d_with_alpha(e->color, scale_alpha(e->color.a, 1.0f - t));
            r2d_draw_line(e->a, e->b, e->thickness * (1.0f - 0.5f * t), c);
            break;
        }
        case FX_ARC: {
            /* the head leads along the sweep, the tail follows later, so the
             * arc grows, then shrinks away behind the head */
            float head = e->start + e->sweep * t;
            float tail = e->start + e->sweep * r2d_clampf((t - 0.4f) / 0.6f, 0, 1);
            r2d_color c = r2d_with_alpha(e->color, scale_alpha(e->color.a, 1.0f - t * t));
            if (head != tail) r2d_draw_arc(e->a, e->r0, e->thickness, tail, head, c);
            break;
        }
        case FX_FLASH: {
            r2d_vec2 v = r2d_viewport();
            r2d_color c = r2d_with_alpha(e->color, scale_alpha(e->color.a, 1.0f - t));
            r2d_draw_rect(r2d_rect_make(0, 0, v.x, v.y), c);
            break;
        }
        case FX_CUSTOM:
            if (e->fn) e->fn(handle, t, e->age, e->a, e->color, e->user);
            break;
        default:
            break;   /* shakes draw nothing */
    }
}
void r2d_effects_draw(const r2d_effects *fx, r2d_space space) {
    if (!fx) return;
    r2d_space previous = r2d_get_space();
    r2d_set_space(space);
    for (size_t i = 0; i < fx->capacity; ++i) {
        const effect *e = &fx->items[i];
        if (!e->active || e->space != space || e->kind == FX_SHAKE) continue;
        r2d_effect handle = {(uint32_t)(i + 1) | (e->generation << 16)};
        draw_one(e, handle);
    }
    r2d_set_space(previous);
}
size_t r2d_effects_count(const r2d_effects *fx) {
    size_t n = 0;
    if (fx) for (size_t i = 0; i < fx->capacity; ++i) n += fx->items[i].active;
    return n;
}
size_t r2d_effects_capacity(const r2d_effects *fx) { return fx ? fx->capacity : 0; }
